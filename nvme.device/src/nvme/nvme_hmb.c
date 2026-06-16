// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host Memory Buffer (HMB) support for DRAM-less NVMe controllers.
 *
 * Controllers without onboard DRAM (e.g. budget consumer SSDs) keep
 * their Flash Translation Layer (FTL) tables in NAND.  Without HMB
 * every I/O may incur additional NAND reads to look up the
 * physical-block mapping.
 *
 * NVMe Identify Controller reports the controller's HMB needs:
 *   hmpre — preferred HMB size, in 4-KiB pages
 *   hmmin — minimum HMB size, in 4-KiB pages
 *   hmminds — minimum descriptor entry size (HMB chunk), in 4-KiB pages
 *   hmmaxd — maximum number of descriptor entries
 *
 * The host allocates page-aligned chunks summing to at least hmmin and
 * preferably hmpre × 4 KiB, builds a Host Memory Descriptor List
 * (array of {addr, size} pairs), and issues Set Features 0x0D
 * (NVME_FEAT_HOST_MEM_BUF) pointing at it.  The controller starts
 * DMAing FTL pages in/out of the buffer.
 *
 * On the Amiga side every chunk is allocated from the controller's
 * memory pool via dma_alloc() (page-aligned).  The chunk pointers
 * live in descs[i].addr — the teardown path reads them back from
 * there to dma_free() each chunk.  No separate back-pointer array.
 */

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <nvme/nvme_ctrl.h> /* struct NVMeController */
#include <device.h>
#include <nvme/nvme_hmb.h>
#include <nvme/nvme_admin.h>

/*
 * Tunables.  Match the Linux defaults where reasonable — PiStorm
 * Amigas typically run with 1-2 GiB of fast RAM mapped by Emu68, so
 * a 128 MiB ceiling per controller is comfortable.
 */
#define NVME_MAX_HMB_BYTES (128UL * 1024UL * 1024UL)  /* per controller */
#define NVME_HMB_START_CHUNK (16UL * 1024UL * 1024UL) /* starting chunk size */
#define NVME_HMB_MIN_CHUNK (8UL * 1024UL)             /* never below 8 KiB */

static void hmb_chunks_free(struct NVMeController *ctrl);

/*
 * nvme_set_host_mem - submit Set Features (Host Memory Buffer) to
 * the controller.
 *
 * The Set Features helper in core.c only exposes DW11; HMB needs
 * DW11..DW15, so we hand-build the SQE here.  Called once with
 * NVME_HOST_MEM_ENABLE to turn HMB on after allocation, and once
 * with bits=0 to disable HMB before teardown — disabling first is
 * mandatory because the controller may still be DMA-ing to the
 * buffer.
 */
static int nvme_set_host_mem(struct NVMeController *ctrl, u32 bits)
{
    struct nvme_command cmd;
    u32 host_mem_size_pages = (u32)(ctrl->hmb_size / NVME_CTRL_PAGE_SIZE);
    u64 dma_addr = (u64)(ULONG)ctrl->hmb_descs;

    KprintfH("[nvme] %s: bits=0x%lx host_mem_size_pages=%lu desc_dma=%lx nr_descs=%lu\n",
             __func__, (ULONG)bits, (ULONG)host_mem_size_pages,
             (ULONG)dma_addr, (ULONG)ctrl->hmb_nr_descs);

    mem_zero(&cmd, sizeof(cmd));
    cmd.features.opcode = nvme_admin_set_features;
    cmd.features.fid = le32(NVME_FEAT_HOST_MEM_BUF);
    cmd.features.dword11 = le32(bits);
    cmd.features.dword12 = le32(host_mem_size_pages);
    cmd.features.dword13 = le32((u32)(dma_addr & 0xFFFFFFFFul));
    cmd.features.dword14 = le32((u32)(dma_addr >> 32));
    cmd.features.dword15 = le32((u32)ctrl->hmb_nr_descs);

    int ret = nvme_submit_sync_cmd(ctrl, &cmd, NULL, NULL, 0);
    if (ret)
    {
        Kprintf("[nvme] %s: Set Features HMB failed (bits=0x%lx status=%ld)\n",
                __func__, (ULONG)bits, (LONG)ret);
    }
    else
    {
        Kprintf("[nvme] %s: HMB %s (size=%lu MiB descs=%lu)\n",
                __func__,
                (bits & NVME_HOST_MEM_ENABLE) ? "enabled" : "disabled",
                (ULONG)(ctrl->hmb_size >> 20), (ULONG)ctrl->hmb_nr_descs);
    }
    return ret;
}

/*
 * nvme_alloc_host_mem - greedy variable-size HMB allocator.
 *
 * Two phases:
 *
 *   1. Single-block shot — try dma_alloc(@preferred) as one descriptor
 *      entry.  Lucky path: controller wants a small buffer and the
 *      pool has the room in one piece.  Skipped when @preferred is
 *      below HMMINDS (would violate the per-entry minimum).
 *
 *   2. Greedy fill — start from NVME_HMB_START_CHUNK (clamped to
 *      remaining preferred), accept whatever the pool gives us, halve
 *      on alloc failure.  Each successful chunk is recorded as its own
 *      descriptor entry — NVMe permits variable per-entry sizes, only
 *      HMMINDS pegs the minimum.  Keep going until we've reached
 *      preferred, run out of descriptor slots (HMMAXD), or the
 *      fallback chunk size drops below the floor.
 *
 * After both phases:
 *   - total_allocated >= min  → success
 *   - total_allocated <  min  → release everything and fail
 *
 * @min      lower bound from Identify (HMMIN * page size); HMB can't be
 *           enabled below this.
 * @preferred upper target from Identify (HMPRE * page size), already
 *           capped by NVME_MAX_HMB_BYTES upstream.
 *
 * Returns 0 on success (ctrl->hmb_* populated), -1 on failure (all
 * state cleared).
 */
static int nvme_alloc_host_mem(struct NVMeController *ctrl,
                               u64 min, u64 preferred)
{
    u32 chunks_allocated = 0;
    u64 total_allocated = 0;

    const u64 hmminds_bytes = (u64)ctrl->hmminds * NVME_CTRL_PAGE_SIZE;
    const u64 floor = hmminds_bytes > NVME_HMB_MIN_CHUNK ? hmminds_bytes : NVME_HMB_MIN_CHUNK;

    /* Per-descriptor cap.  hmmaxd==0 means "controller doesn't say" —
     * fall back to a bound derived from the smallest legal chunk, so
     * the descriptor table size stays sane. */
    const u32 hmmaxd_cap = ctrl->hmmaxd ? ctrl->hmmaxd : (u32)((preferred + floor - 1) / floor);

    /* Allocate the descriptor table up front, worst-case sized to
     * hmmaxd_cap.  Unused trailing entries stay zero; Set Features
     * (DW15) gets the actual nr_descs == chunks_allocated.  The chunk
     * pointers are recovered from descs[i].addr on teardown — no
     * separate back-pointer array. */
    u32 descs_size = hmmaxd_cap * (u32)sizeof(struct nvme_host_mem_buf_desc);
    struct nvme_host_mem_buf_desc *descs = dma_zalloc(ctrl->dmaPool, NVME_CTRL_PAGE_SIZE, descs_size);
    if (!descs)
    {
        Kprintf("[nvme] %s: descriptor table alloc failed (%lu bytes)\n", __func__, (ULONG)descs_size);
        return -1;
    }

    /* Stash the table on the controller so hmb_chunks_free can release
     * both the chunks (via descs[i].addr) and the table itself on the
     * failure path. */
    ctrl->hmb_descs = descs;
    ctrl->hmb_nr_descs = 0;
    ctrl->hmb_size = 0;

    /* Phase 1: try @preferred in one block. */
    if (preferred >= floor)
    {
        APTR buf = dma_alloc(ctrl->dmaPool, NVME_CTRL_PAGE_SIZE, (ULONG)preferred);
        if (buf)
        {
            descs[0].addr = le64((u64)(ULONG)buf);
            descs[0].size = le32((u32)(preferred / NVME_CTRL_PAGE_SIZE));
            descs[0].rsvd = 0;
            total_allocated = preferred;
            chunks_allocated = 1;
            KprintfH("[nvme] %s: phase 1 success: one %lu KiB block @ %lx\n",
                     __func__, (ULONG)(preferred >> 10), (ULONG)buf);
        }
    }

    /* Phase 2: greedy fill.  Skips itself if phase 1 already covered
     * preferred (the loop condition `total_allocated < preferred` is false). */
    u64 attempt = NVME_HMB_START_CHUNK;
    if (attempt > preferred)
        attempt = preferred;

    while (total_allocated < preferred && chunks_allocated < hmmaxd_cap && attempt >= floor)
    {
        u64 remaining = preferred - total_allocated;
        u64 chunk_size = attempt < remaining ? attempt : remaining;

        if (chunk_size < floor)
            break; /* tail too small to be a valid descriptor */

        APTR buf = dma_alloc(ctrl->dmaPool, NVME_CTRL_PAGE_SIZE, (ULONG)chunk_size);
        if (buf)
        {
            descs[chunks_allocated].addr = le64((u64)(ULONG)buf);
            descs[chunks_allocated].size = le32((u32)(chunk_size / NVME_CTRL_PAGE_SIZE));
            descs[chunks_allocated].rsvd = 0;
            total_allocated += chunk_size;
            chunks_allocated++;
            KprintfH("[nvme] %s: phase 2 chunk[%lu] @ %lx size=%lu KiB total_allocated=%lu KiB\n",
                     __func__, (ULONG)(chunks_allocated - 1), (ULONG)buf,
                     (ULONG)(chunk_size >> 10), (ULONG)(total_allocated >> 10));
        }
        else
        {
            KprintfH("[nvme] %s: phase 2 attempt %lu KiB failed; halving\n",
                     __func__, (ULONG)(attempt >> 10));
            attempt /= 2;
        }
    }

    ctrl->hmb_nr_descs = chunks_allocated;
    ctrl->hmb_size = (ULONG)total_allocated;

    if (total_allocated < min)
    {
        Kprintf("[nvme] %s: gathered %lu KiB (%lu chunks) below min=%lu KiB — releasing\n",
                __func__, (ULONG)(total_allocated >> 10), (ULONG)chunks_allocated, (ULONG)(min >> 10));
        hmb_chunks_free(ctrl);
        return -1;
    }

    /* Device DMA-reads the descriptor table; clean dirty cache lines
     * before announcing it via Set Features. */
    nvme_cache_flush(descs, descs_size, TRUE);

    KprintfH("[nvme] %s: allocated %lu KiB in %lu chunks (target %lu KiB, min %lu KiB)\n",
             __func__, (ULONG)(total_allocated >> 10), (ULONG)chunks_allocated,
             (ULONG)(preferred >> 10), (ULONG)(min >> 10));
    return 0;
}

/*
 * hmb_chunks_free - release the chunk buffers and the descriptor
 * table, zeroing the controller's HMB state.  Used as the cleanup
 * path from nvme_alloc_host_mem on under-min failure and from
 * nvme_free_host_mem after the controller has been told to release.
 *
 * Each chunk's address lives in descs[i].addr (little-endian on wire).
 * dma_free needs only the pointer — the pool tracks the size — so we
 * don't carry a separate back-pointer array.
 */
static void hmb_chunks_free(struct NVMeController *ctrl)
{
    struct nvme_host_mem_buf_desc *descs = ctrl->hmb_descs;
    if (descs)
    {
        for (ULONG i = 0; i < ctrl->hmb_nr_descs; i++)
        {
            APTR buf = (APTR)(ULONG)le64(descs[i].addr);
            if (buf)
                dma_free(ctrl->dmaPool, buf);
        }
        dma_free(ctrl->dmaPool, descs);
        ctrl->hmb_descs = NULL;
    }
    ctrl->hmb_nr_descs = 0;
    ctrl->hmb_size = 0;
}

/*
 * nvme_setup_host_mem - if the controller advertises HMB support
 * (id->hmpre > 0), allocate a buffer and enable HMB via Set Features.
 *
 * Caller is the probe path, after nvme_init_ctrl_finish() has read
 * id->hmpre/hmmin/hmminds/hmmaxd into ctrl.  Failure is
 * non-fatal — the controller must function (albeit slower) without
 * HMB, so we just log and return.
 */
void nvme_setup_host_mem(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;

    if (ctrl->hmpre == 0)
    {
        Kprintf("[nvme] %s: controller does not request HMB (hmpre=0)\n",
                __func__);
        return;
    }

    u64 preferred = (u64)ctrl->hmpre * NVME_CTRL_PAGE_SIZE;
    u64 minimum = (u64)ctrl->hmmin * NVME_CTRL_PAGE_SIZE;

    Kprintf("[nvme] %s: preferred=%lu MiB min=%lu MiB cap=%lu MiB\n",
            __func__,
            (ULONG)(preferred >> 20), (ULONG)(minimum >> 20),
            (ULONG)(NVME_MAX_HMB_BYTES >> 20));

    if (preferred > NVME_MAX_HMB_BYTES)
        preferred = NVME_MAX_HMB_BYTES;

    if (minimum > NVME_MAX_HMB_BYTES)
    {
        Kprintf("[nvme] %s: hmmin (%lu MiB) above our cap (%lu MiB); skipping HMB\n",
                __func__, (ULONG)(minimum >> 20), (ULONG)(NVME_MAX_HMB_BYTES >> 20));
        return;
    }

    if (nvme_alloc_host_mem(ctrl, minimum, preferred) != 0)
    {
        Kprintf("[nvme] %s: unable to allocate HMB; continuing without it\n", __func__);
        return;
    }

    if (nvme_set_host_mem(ctrl, NVME_HOST_MEM_ENABLE) != 0)
    {
        Kprintf("[nvme] %s: Set Features failed; freeing buffer\n", __func__);
        hmb_chunks_free(ctrl);
        return;
    }
}

/*
 * nvme_free_host_mem - tell the controller to release HMB, then free
 * the chunks and descriptor table.  Order matters: until Set Features
 * with bits=0 completes, the controller may still DMA into the chunks.
 *
 * Called from the controller teardown path (nvme_unprobe_controller)
 * before the memory pool is destroyed.
 */
void nvme_free_host_mem(struct NVMeController *ctrl)
{
    if (!ctrl || !ctrl->hmb_descs)
        return;

    KprintfH("[nvme] %s: releasing %lu MiB HMB\n", __func__, (ULONG)(ctrl->hmb_size >> 20));

    (void)nvme_set_host_mem(ctrl, 0); /* clear NVME_HOST_MEM_ENABLE */
    hmb_chunks_free(ctrl);
}
