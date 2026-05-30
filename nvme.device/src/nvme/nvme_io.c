// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvme_io.c — I/O dispatch primitives.
 *
 * SQE construction (read/write/flush/DSM), PRP-list building,
 * bounce-buffer staging, the SQ-tail doorbell write (nvme_submit_io),
 * per-request alloc / destroy / DMA cleanup, the NVME_IO_ASYNC
 * convention used at the submit boundary, the readiness gate
 * (__nvme_check_ready, nvme_fail_nonready_command), the
 * nvme_io_submit_* public dispatch helpers, and the chunked-I/O
 * scheduler (nvme_io_context_pump / _finish for BeginIOs that exceed
 * the controller MDTS).
 *
 * The per-request state machine and completion delivery live in
 * nvme_completion.c; tag allocation and the inflight-table primitives
 * live in nvme_queue.{c,h}.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/errors.h>
#include <timing.h>

#include <device.h>
#include <nvme/nvme_ctrl.h> /* struct NVMeController, nvme_ctrl_state, nvme_state_terminal */
#include <nvme/nvme_completion.h>
#include <nvme/nvme_io.h>
#include <nvme/nvme_queue.h> /* nvme_inflight_claim, nvme_inflight_release */

#define NVME_PRP_ENTRIES_PER_PAGE (NVME_CTRL_PAGE_SIZE / sizeof(u64)) /* 512 */

/* ---------------------------------------------------------------- *
 *  Section 1: PRP & SQE helpers                                    *
 * ---------------------------------------------------------------- */

/*
 * build_prps - lay PRP1 / PRP2 / chained PRP list for a contiguous buffer.
 *
 * NVMe spec §4.1.2:
 *   - PRP1 holds the address of the first page region (page-aligned
 *     or not; partial-page offset allowed).
 *   - If the transfer ends inside that first page, PRP2 is ignored.
 *   - If the transfer needs exactly two pages, PRP2 holds the second
 *     page's base.
 *   - For three or more pages, PRP2 points to a PRP list page — an
 *     array of 512 u64 PRP entries.  The last entry of a non-final
 *     list page chains to the next list page; the last entry of the
 *     final list page holds the last data PRP.
 *
 * Supports chains of up to 8 list pages, i.e. up to roughly
 * (1 + 1 + 8*511) pages * 4 KiB ≈ 16 MiB per transfer.
 * The list pages are AllocMem'd and stashed on req->prp_pages[];
 * nvme_cleanup_cmd FreeMems them on completion.
 *
 * On emu68 PiStorm the bus address == CPU address, so we use the
 * buffer pointer directly with no translation.
 */
static int build_prps(struct nvme_request *req, void *buffer, u32 bytes)
{
    const u64 buf_addr = (u64)(ULONG)buffer;
    const u32 first = (u32)(NVME_CTRL_PAGE_SIZE - (buf_addr & (NVME_CTRL_PAGE_SIZE - 1)));

    req->cmd.rw.dptr.prp1 = le64(buf_addr);
    req->prp_page_count = 0;

    if (bytes <= first)
    {
        req->cmd.rw.dptr.prp2 = 0;
        return 0;
    }

    const u32 remain = bytes - first;
    const u32 nr_data_pages = (remain + NVME_CTRL_PAGE_SIZE - 1) / NVME_CTRL_PAGE_SIZE;

    if (nr_data_pages == 1)
    {
        req->cmd.rw.dptr.prp2 = le64(buf_addr + first);
        return 0;
    }

    u64 *list = NULL;
    u32 list_pos = 0;

    /* Need at least one PRP list page.  Each PRP-list page MUST be
     * page-aligned per NVMe spec §4.1.2 (entries describe full pages,
     * page-offset of every list entry must be 0).  AllocMem returns
     * 4-byte aligned memory, so use dma_alloc with align=4 KiB to
     * guarantee page alignment.  The pool comes from the owning
     * controller (req->ac->memoryPool was set by the I/O dispatch
     * caller in ProcessCommand). */
    for (u32 page_idx = 0; page_idx < nr_data_pages; page_idx++)
    {
        const u64 entry_addr = buf_addr + first + (u64)page_idx * NVME_CTRL_PAGE_SIZE;

        if (list == NULL)
        {
            if (req->prp_page_count >= (u8)(sizeof(req->prp_pages) / sizeof(req->prp_pages[0])))
            {
                Kprintf("[nvme] %s: transfer needs more than %lu PRP list pages\n",
                        __func__, (ULONG)(sizeof(req->prp_pages) / sizeof(req->prp_pages[0])));
                goto free_lists;
            }
            list = slab_alloc(&req->ac->prp_page_slab);
            if (!list)
            {
                Kprintf("[nvme] %s: slab_alloc PRP-list failed\n", __func__);
                goto free_lists;
            }
            mem_zero(list, NVME_CTRL_PAGE_SIZE);
            req->prp_pages[req->prp_page_count++] = list;
            list_pos = 0;

            if (req->prp_page_count == 1)
            {
                req->cmd.rw.dptr.prp2 = le64((u64)(ULONG)list);
            }
            else
            {
                /* Patch the chain entry of the previous (now full)
                 * list page, then flush it in one 4 KiB op */
                u64 *prev = (u64 *)req->prp_pages[req->prp_page_count - 2];
                prev[NVME_PRP_ENTRIES_PER_PAGE - 1] = le64((u64)(ULONG)list);
                nvme_cache_flush(prev, NVME_CTRL_PAGE_SIZE);
            }
        }

        list[list_pos++] = le64(entry_addr);

        if (list_pos == NVME_PRP_ENTRIES_PER_PAGE - 1 && (page_idx + 1) < nr_data_pages)
            list = NULL;
        else if (list_pos == NVME_PRP_ENTRIES_PER_PAGE)
            break;
    }
    /* Final (possibly only) list page: nothing chains off it, so
     * flush here. */
    if (list)
        nvme_cache_flush(list, NVME_CTRL_PAGE_SIZE);

    req->flags |= NVME_REQ_PRP_LIST | NVME_REQ_PRP_SLAB;
    return 0;

free_lists:
    while (req->prp_page_count > 0)
    {
        req->prp_page_count--;
        slab_free(&req->ac->prp_page_slab, req->prp_pages[req->prp_page_count]);
        req->prp_pages[req->prp_page_count] = NULL;
    }
    return -1;
}

/*
 * nvme_setup_dsm - build a Dataset Management (Deallocate) SQE.
 *
 * @ranges is owned by @req from this point: stashed on req->prp_pages[0]
 * under NVME_REQ_PRP_LIST so nvme_cleanup_cmd / nvme_req_destroy free
 * it via dma_free.  The caller must have allocated it as a 4 KiB
 * page-aligned zero-padded DMA-able buffer (see nvme_io_submit_dsm).
 *
 * Spec note: "Some devices do not consider the DSM 'Number of Ranges'
 * field when determining how much data to DMA. Always allocate memory
 * for maximum number of segments to prevent device reading beyond
 * end of buffer."  The full-4-KiB-zero-padded contract above satisfies
 * this quirk.
 */
static int nvme_setup_dsm(struct nvme_request *req, struct nvme_dsm_range *ranges,
                          u16 nr)
{
    KprintfH("[nvme] setup_dsm: req=%lx nsid=%lu nr=%lu\n",
             (ULONG)req,
             (ULONG)(req->unit ? req->unit->nsid : 0),
             (ULONG)nr);

    if (!req || !ranges || nr == 0 || nr > NVME_DSM_MAX_RANGES)
    {
        Kprintf("[nvme] %s: bad params (nr=%lu)\n", __func__, (ULONG)nr);
        return -1;
    }

    /* Device DMA-reads the range list; flush dirty CPU lines. */
    nvme_cache_flush(ranges, sizeof(*ranges) * NVME_DSM_MAX_RANGES);

    mem_zero(&req->cmd, sizeof(req->cmd));
    req->cmd.dsm.opcode = nvme_cmd_dsm;
    req->cmd.dsm.command_id = req->tag;
    req->cmd.dsm.nsid = le32(req->unit ? req->unit->nsid : 0);
    req->cmd.dsm.nr = le32((u32)(nr - 1));
    req->cmd.dsm.attributes = le32(NVME_DSMGMT_AD);
    req->cmd.dsm.dptr.prp1 = le64((u64)(ULONG)ranges);
    req->cmd.dsm.dptr.prp2 = 0;

    /* Stash so cleanup_cmd frees it.  prp_pages[] is named for PRP-list
     * pages but is really "auxiliary pages dma_free'd on completion" —
     * the DSM range list fits that contract. */
    req->prp_pages[0] = ranges;
    req->prp_page_count = 1;
    req->flags |= NVME_REQ_PRP_LIST;

    return 0;
}

/*
 * nvme_setup_flush - build an NVMe Flush SQE for the request's
 * namespace.  No PRPs, no data.  Commits the device's Volatile Write
 * Cache to non-volatile media; without this, writes survive only until
 * the next power loss (which is why HDToolBox's RDB writes didn't stick).
 */
static void nvme_setup_flush(struct nvme_request *req)
{
    KprintfH("[nvme] setup_flush: req=%lx nsid=%lu\n",
             (ULONG)req,
             (ULONG)(req->unit ? req->unit->nsid : 0));

    mem_zero(&req->cmd, sizeof(req->cmd));
    req->cmd.common.opcode = nvme_cmd_flush;
    req->cmd.common.command_id = req->tag;
    req->cmd.common.nsid = le32(req->unit ? req->unit->nsid : 0);
}

/*
 * nvme_setup_rw - fill req->cmd for a read or write command, allocating
 * a bounce buffer when the user pointer isn't directly DMA-able, and
 * building PRP1/2 (plus a chained PRP list if needed) into req->cmd.
 *
 * Also handles the TD_FORMAT / "wipe" idiom where the user passes
 * io_Data == NULL meaning "fill this LBA range with zeros" — a literal
 * read-from-NULL would DMA Chip-RAM contents to the device.  When
 * detected, allocates a zero-filled bounce instead.
 *
 * Returns 0 on success, IOERR_SELFTEST if a bounce-buffer or PRP-list
 * allocation failed.  On non-zero return the caller still owns @req
 * and must call nvme_req_destroy to release the tag and any partial
 * DMA state.
 */
static BYTE nvme_setup_rw(struct nvme_request *req, u64 lba, ULONG count,
                          u8 opcode, void *buffer)
{
    const u32 bytes = count << (req->unit ? req->unit->blockShift : 9);

    /* AmigaOS TD_FORMAT (and some implementations of NSCMD_TD_FORMAT64,
     * CMD_WRITE issued with a "wipe" intent) signals "fill this LBA
     * range with zeros" by passing io_Data == NULL.  Detect by
     * checking the ORIGINAL user buffer — held on the parent context
     * for chunked siblings, or on req->io for single-shot I/Os. */
    APTR data_base = req->ctx ? req->ctx->user_data : (req->io ? req->io->io_Data : (APTR)1);
    BOOL is_zero_fill = (opcode == nvme_cmd_write) && data_base == NULL;

    KprintfH("[nvme] setup_rw: opcode=0x%02lx (%s) nsid=%lu lba=0x%08lx%08lx count=%lu bytes=%lu buf=%lx%s\n",
             (ULONG)opcode, nvme_get_opcode_str(opcode),
             (ULONG)(req->unit ? req->unit->nsid : 0),
             (ULONG)(lba >> 32), (ULONG)lba, count, bytes, (ULONG)buffer,
             is_zero_fill ? " (zero-fill)" : "");

    /* Remember the caller-supplied buffer + length so cleanup_cmd
     * can copy the bounce back (for reads) and apply the post-DMA
     * cache invalidate to whichever buffer the DMA actually used.
     * In zero-fill mode there is no caller buffer to copy back to —
     * leave user_buf NULL so cleanup_cmd skips copy / inval. */
    req->user_buf = is_zero_fill ? NULL : buffer;
    req->user_len = bytes;
    req->bounce_buf = NULL;

    void *dma_buf = buffer;

    if (is_zero_fill)
    {
        APTR b = dma_zalloc(req->ac->memoryPool, DMA_ALIGN_MIN, bytes);
        if (!b)
        {
            Kprintf("[nvme] %s: zero-fill bounce alloc (%lu bytes) failed\n",
                    __func__, (ULONG)bytes);
            return IOERR_SELFTEST;
        }
        req->bounce_buf = b;
        KprintfH("[nvme] %s: zero-fill bounce=%lx (%lu bytes)\n",
                 __func__, (ULONG)b, (ULONG)bytes);
        dma_buf = b;
    }
    else if (buffer && bytes && nvme_needs_bounce(buffer))
    {
        /* Allocate a Fast-RAM bounce.  Write commands need user data
         * copied in before submit; read commands fill the bounce and
         * cleanup_cmd copies it back. */
        APTR b = dma_alloc(req->ac->memoryPool, DMA_ALIGN_MIN, bytes);
        if (!b)
        {
            Kprintf("[nvme] %s: bounce alloc (%lu bytes) failed\n",
                    __func__, (ULONG)bytes);
            return IOERR_SELFTEST;
        }
        req->bounce_buf = b;
        if (opcode == nvme_cmd_write)
            CopyMem(buffer, b, bytes);
        dma_buf = b;
    }

    mem_zero(&req->cmd, sizeof(req->cmd));
    req->cmd.rw.opcode = opcode;
    req->cmd.rw.command_id = req->tag;
    req->cmd.rw.nsid = le32(req->unit ? req->unit->nsid : 0);
    req->cmd.rw.slba = le64(lba);
    req->cmd.rw.length = le16((u16)(count - 1));

    if (build_prps(req, dma_buf, bytes) != 0)
    {
        Kprintf("[nvme] %s: PRP setup failed\n", __func__);
        return IOERR_SELFTEST;
    }

    /* Make the DMA buffer coherent with the device.  CachePreDMA
     * writes back dirty lines for writes and pre-invalidates for
     * reads so the device's writes won't be shadowed by stale cache.
     * The matching post-DMA invalidate runs from nvme_cleanup_cmd. */
    if (dma_buf && bytes)
        nvme_cache_flush(dma_buf, bytes);

    return 0;
}

/* ---------------------------------------------------------------- *
 *  Section 2: per-request lifecycle (alloc / init / destroy /      *
 *             cleanup)                                             *
 * ---------------------------------------------------------------- */

/*
 * nvme_req_free_dma_buffers - free the per-command DMA resources
 * owned by @req: any chained PRP-list pages (or DSM range buffer)
 * stashed in prp_pages[], and the Fast-RAM bounce buffer.  No cache
 * work — callers that need an invalidate or bounce->user copy-back do
 * it before invoking this helper.
 *
 * Shared between nvme_cleanup_cmd (completion path) and
 * nvme_req_destroy (submit-failed path); the two only differ in the
 * cache work and the inflight-release / pool_free of the request
 * struct around this body.
 */
static void nvme_req_free_dma_buffers(struct nvme_request *req)
{
    APTR pool = req->ac ? req->ac->memoryPool : NULL;

    if (req->flags & NVME_REQ_PRP_LIST)
    {
        /* Slab-backed PRP-list pages (build_prps) vs caller-provided
         * dma_zalloc buffer (nvme_setup_dsm) — flag bit disambiguates. */
        BOOL use_slab = (req->flags & NVME_REQ_PRP_SLAB) != 0;
        while (req->prp_page_count > 0)
        {
            req->prp_page_count--;
            APTR p = req->prp_pages[req->prp_page_count];
            if (p)
            {
                if (use_slab)
                    slab_free(&req->ac->prp_page_slab, p);
                else if (pool)
                    dma_free(pool, p);
            }
            req->prp_pages[req->prp_page_count] = NULL;
        }
        req->flags &= (enum nvme_req_flags) ~(NVME_REQ_PRP_LIST | NVME_REQ_PRP_SLAB);
    }

    if (req->bounce_buf)
    {
        if (pool)
            dma_free(pool, req->bounce_buf);
        req->bounce_buf = NULL;
    }
}

/*
 * nvme_cleanup_cmd - release per-command DMA resources after completion.
 *
 * Frees any chained PRP-list pages allocated by build_prps(), and the
 * Fast-RAM bounce buffer (with the bounce->user copy-back for reads).
 * For non-bounced reads, invalidates the user buffer's cache lines.
 *
 * Contract: run exactly once, on the final completion of a given DMA
 * setup — i.e. inside nvme_complete_rq's COMPLETE branch.  Must NOT
 * run on the retry path: nvme_retry_req re-rings the doorbell with
 * the same SQE, so PRPs and any bounce buffer have to survive.
 */
void nvme_cleanup_cmd(struct nvme_request *req)
{
    /* Read path post-DMA cache work, before the bounce is freed.
     * The device wrote into whichever buffer DMA actually used
     * (the bounce if there is one, else user_buf directly). */
    if (req->cmd.rw.opcode == nvme_cmd_read &&
        req->user_buf && req->user_len)
    {
        APTR dma_buf = req->bounce_buf ? req->bounce_buf : req->user_buf;
        nvme_cache_inval(dma_buf, req->user_len);
        if (req->bounce_buf)
            CopyMem(req->bounce_buf, req->user_buf, req->user_len);
    }

    nvme_req_free_dma_buffers(req);
}

/*
 * nvme_req_destroy - undo a request that was allocated but never reached
 * a successful nvme_submit_io.  Releases the queue inflight slot, any
 * DSM/PRP scratch pages held in prp_pages[], the bounce buffer, then
 * frees the request itself.
 *
 * Successful submissions go through nvme_cleanup_cmd on the completion
 * path instead — they own these resources for the duration of the I/O.
 */
void nvme_req_destroy(struct nvme_request *req)
{
    struct NVMeController *ctrl = req->ac;

    if (req->q && req->q->inflight && req->tag < req->q->depth)
        nvme_inflight_release(req->q, req->tag);

    nvme_req_free_dma_buffers(req);

    if (ctrl)
        slab_free(&ctrl->req_slab, req);
}

/*
 * nvme_req_alloc_io - allocate a zero-initialised request and claim an
 * I/O-queue CID for it.
 *
 * Single entry point for the I/O submission helpers: handles pool
 * exhaustion (returns IOERR_SELFTEST) and full inflight table (returns
 * IOERR_UNITBUSY) via @err_out, so callers don't repeat the two-step
 * mapping.  On success the caller fills in command-specific state with
 * the setup helpers and hands off to nvme_submit_io().
 */
static struct nvme_request *nvme_req_alloc_io(struct NVMeUnit *unit,
                                              struct IOStdReq *io,
                                              BYTE *err_out)
{
    struct NVMeController *ctrl = unit->ctrl;
    struct nvme_queue *q = &ctrl->io_q;

    struct nvme_request *req = slab_zalloc(&ctrl->req_slab);
    if (!req)
    {
        *err_out = IOERR_SELFTEST;
        return NULL;
    }

    u16 tag = nvme_alloc_tag(q);
    if (tag == 0xFFFF)
    {
        slab_free(&ctrl->req_slab, req);
        *err_out = IOERR_UNITBUSY;
        return NULL;
    }

    req->io = io;
    req->unit = unit;
    req->ac = ctrl;
    req->q = q;
    req->tag = tag;
    nvme_inflight_claim(q, tag, req);
    return req;
}

/* ---------------------------------------------------------------- *
 *  Section 3: submit primitives (SQ-tail doorbell + retry)         *
 * ---------------------------------------------------------------- */

/*
 * nvme_submit_io - write req->cmd into req->q's SQ and ring the SQ-tail
 * doorbell.
 *
 * Single path for admin and I/O — req->q (set by nvme_req_alloc_admin /
 * nvme_req_alloc_io) carries the SQ pointer, depth, tail, CQ head (for
 * the SQ-full check), doorbell offset, and inflight table.
 *
 * Returns NVME_IO_ASYNC on success — the IRQ handler picks up the CQE
 * and nvme_end_req() Signal()s the waiter or ReplyMsg()s the IOStdReq.
 * On submission failure returns an IOERR_* code and has already called
 * nvme_req_destroy(@req) — caller must not touch @req afterwards.
 */
BYTE nvme_submit_io(struct nvme_request *req)
{
    if (!req || !req->q || !req->ac)
    {
        Kprintf("[nvme] submit_io: request missing queue/controller back-pointer\n");
        return IOERR_BADADDRESS;
    }
    struct nvme_queue *q = req->q;

    if (!q->sq)
    {
        Kprintf("[nvme] submit_io: SQ not initialised (qid=%lu)\n", (ULONG)q->qid);
        nvme_req_destroy(req);
        return IOERR_BADADDRESS;
    }

    u16 tail = q->sq_tail;
    u16 next = (u16)((tail + 1) % q->depth);
    if (next == q->cq_head)
    {
        Kprintf("[nvme] submit_io: SQ full (qid=%lu head=%lu tail=%lu)\n",
                (ULONG)q->qid, (ULONG)q->cq_head, (ULONG)tail);
        nvme_req_destroy(req);
        return IOERR_UNITBUSY;
    }

    /* Copy the command into the SQE.  NVMe commands are little-endian
     * on the wire; the fields we wrote into req->cmd are already in
     * little-endian via cpu_to_leXX, so plain memcpy is correct. */
    CopyMem(&req->cmd, &q->sq[tail], sizeof(req->cmd));

    /* Cache flush: device DMA-reads this SQE from DRAM.  Without
     * CachePreDMA the device may see stale cache lines and read garbage. */
    nvme_cache_flush(&q->sq[tail], sizeof(struct nvme_command));

    q->sq_tail = next;

    req->submit_us = get_time();

    KprintfH("[nvme] submit_io: qid=%lu tag=%lu opcode=0x%02lx (%s) slot=%lu next=%lu db_off=0x%lx\n",
             (ULONG)q->qid,
             (ULONG)req->tag,
             (ULONG)req->cmd.common.opcode,
             nvme_opcode_str(q->qid, req->cmd.common.opcode),
             (ULONG)tail,
             (ULONG)next,
             (ULONG)q->sq_db_off);

    mmio_write32((u32)next, (volatile UBYTE *)req->ac->bar0 + q->sq_db_off);

    return NVME_IO_ASYNC;
}

/*
 * nvme_resubmit_io - allocate a fresh tag for @req and resubmit.
 *
 * Used by the retry paths (nvme_retry_req's immediate-resubmit branch
 * and the watchdog's CRDT-retry-list deadline branch) after drain_cq
 * already released the original tag back to the per-queue free pool.
 *
 * On tag-pool exhaustion fails @req via nvme_complete_rq with
 * NVME_SC_HOST_PATH_ERROR and returns NVME_IO_ASYNC so the caller
 * doesn't try to touch @req again.
 */
BYTE nvme_resubmit_io(struct nvme_request *req)
{
    u16 tag = nvme_alloc_tag(req->q);
    if (unlikely(tag == 0xFFFF))
    {
        Kprintf("[nvme] resubmit: tag-pool exhausted on qid=%lu\n",
                (ULONG)req->q->qid);
        req->status = NVME_SC_HOST_PATH_ERROR;
        nvme_complete_rq(req);
        return NVME_IO_ASYNC;
    }
    req->tag = tag;
    req->cmd.common.command_id = tag;
    nvme_inflight_claim(req->q, tag, req);
    return nvme_submit_io(req);
}

/* ---------------------------------------------------------------- *
 *  Section 4: readiness gate (slow path + non-ready rejection)     *
 * ---------------------------------------------------------------- */

/*
 * nvme_check_ready - I/O-submit readiness gate.
 *
 * The I/O queue can only carry traffic when the controller is LIVE.
 * The admin queue is gated separately (passthru has its own LIVE check
 * in nvme_passthru_process; internal admin must work pre-LIVE to bring
 * the controller up at all).
 */
static inline BOOL nvme_check_ready(struct NVMeController *ctrl)
{
    return likely(nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE) ? TRUE : FALSE;
}

/*
 * nvme_fail_nonready_command - reject a submission while the ctrl is not LIVE.
 *
 * For transient states (CONNECTING / RESETTING / NEW) destroys the freshly
 * allocated request and returns IOERR_UNITBUSY so the caller passes that
 * back to the originating IOStdReq (AmigaOS-side may retry).
 *
 * For terminal states (DELETING / DELETING_NOIO / DEAD) — and for any
 * request flagged noretry — marks the request with NVME_SC_HOST_PATH_ERROR
 * and routes it through nvme_complete_rq, which replies the io and
 * pool_frees the request via the normal completion path; returns
 * NVME_IO_ASYNC so the caller does not touch io again.
 */
static BYTE nvme_fail_nonready_command(struct nvme_request *req)
{
    BOOL terminal = nvme_state_terminal(req->ac);
    BOOL noretry = nvme_req_noretry(req);

    /* Scrub the per-request bookkeeping; both branches assume a clean
     * slate.  Inspect noretry first because it lives in flags. */
    req->status = 0;
    req->retries = 0;
    req->flags = 0;

    if (!terminal && !noretry)
    {
        nvme_req_destroy(req);
        return IOERR_UNITBUSY;
    }

    req->status = NVME_SC_HOST_PATH_ERROR;
    nvme_complete_rq(req);
    return NVME_IO_ASYNC;
}

/* ---------------------------------------------------------------- *
 *  Section 5: chunked-I/O scheduler (parent context + sibling      *
 *             dispatch/refill)                                     *
 * ---------------------------------------------------------------- */

/*
 * NVME_MAX_INFLIGHT_PER_IO - cap on sibling chunk-commands kept in
 * flight for a single Amiga BeginIO that exceeds the controller MDTS.
 *
 * 16 chosen as the sweet spot: the device gets enough pipelining to
 * hide PCIe round-trip latency, while bounce-buffer memory pressure
 * stays bounded (16 × MDTS = ~2 MB at the common 128 KB MDTS) and the
 * 256-slot I/O queue still has 240 tags free for concurrent BeginIOs
 * and admin traffic.
 */
#define NVME_MAX_INFLIGHT_PER_IO 16u

/*
 * nvme_io_context_pump - dispatch up to NVME_MAX_INFLIGHT_PER_IO sibling
 * commands for a chunked BeginIO.  Called once from nvme_io_submit_rw
 * to start the I/O, and again from nvme_complete_rq on each sibling's
 * CQE to refill the slot just freed.
 *
 * On synchronous failure with no sibling inflight, latches ctx->first_error
 * and returns an IOERR_* so submit_rw can free @ctx and the caller can
 * reply_io with the error.  Otherwise returns NVME_IO_ASYNC and the
 * completion path takes ownership.
 */
BYTE nvme_io_context_pump(struct nvme_io_context *ctx)
{
    struct NVMeUnit *unit = ctx->unit;
    struct NVMeController *ctrl = unit->ctrl;
    const u8 shift = (u8)unit->blockShift;
    const ULONG mdts = ctrl->max_transfer_bytes;

    while (ctx->dispatched < ctx->total_bytes && ctx->inflight < NVME_MAX_INFLIGHT_PER_IO)
    {
        const u32 remaining = ctx->total_bytes - ctx->dispatched;
        u32 next_bytes = remaining;
        if (mdts && next_bytes > mdts)
            next_bytes = mdts;
        const u32 next_blocks = next_bytes >> shift;
        const u64 next_lba = ctx->start_lba + ((u64)ctx->dispatched >> shift);
        APTR next_buf = (APTR)((ULONG)ctx->user_data + ctx->dispatched);

        BYTE err;
        struct nvme_request *req = nvme_req_alloc_io(unit, NULL, &err);
        if (!req)
        {
            /* tag / pool pressure: if we already have siblings in
             * flight, let them drain and re-pump from CQE.  If we
             * have nothing in flight, latch the error and bail. */
            if (ctx->inflight == 0)
            {
                ctx->first_error = err;
                return err;
            }
            break;
        }
        req->ctx = ctx;

        BYTE serr = nvme_setup_rw(req, next_lba, next_blocks, ctx->opcode, next_buf);
        if (serr)
        {
            nvme_req_destroy(req);
            ctx->first_error = serr;
            if (ctx->inflight == 0)
                return serr;
            /* Some siblings still flying; latch so refill stops and
             * let them drain. */
            break;
        }

        ctx->dispatched += next_bytes;
        ctx->inflight++;

        KprintfH("[nvme] ctx_pump: dispatched chunk ctx=%lx tag=%lu offset=%lu bytes=%lu inflight=%lu\n",
                 (ULONG)ctx, (ULONG)req->tag,
                 (ULONG)(ctx->dispatched - next_bytes),
                 (ULONG)next_bytes, (ULONG)ctx->inflight);

        if (nvme_submit_io(req) != NVME_IO_ASYNC)
        {
            /* Rollback accounting; the request has not entered the SQ.
             * nvme_submit_io already destroyed @req. */
            ctx->dispatched -= next_bytes;
            ctx->inflight--;
            if (ctx->inflight == 0)
            {
                ctx->first_error = IOERR_BADADDRESS;
                return IOERR_BADADDRESS;
            }
            /* In-flight siblings will fire CQEs and re-pump us.
             * Latch so they don't try to dispatch more either. */
            ctx->first_error = IOERR_BADADDRESS;
            break;
        }
    }
    return NVME_IO_ASYNC;
}

/*
 * nvme_io_context_finish - last sibling completed: stamp io_Error and
 * io_Actual, ReplyMsg the originating IOStdReq, and slab_free @ctx.
 */
void nvme_io_context_finish(struct nvme_io_context *ctx)
{
    struct NVMeController *ctrl = ctx->unit->ctrl;
    struct IOStdReq *io = ctx->io;
    BYTE err = ctx->first_error;

    io->io_Error = err;
    io->io_Actual = err ? 0 : ctx->total_bytes;

    KprintfH("[nvme] ctx_finish: io=%lx err=%ld actual=%lu (ctx=%lx)\n",
             (ULONG)io, (LONG)err, (ULONG)io->io_Actual, (ULONG)ctx);

    ReplyMsg((struct Message *)io);
    slab_free(&ctrl->ctx_slab, ctx);
}

/* ---------------------------------------------------------------- *
 *  Section 6: public I/O dispatchers                               *
 * ---------------------------------------------------------------- */

/*
 * nvme_io_submit_rw - read/write submission with transparent MDTS split.
 *
 * Small I/Os (size <= MDTS) take the single-shot fast path: one
 * nvme_request, no parent context, freed by the completion path.
 *
 * Larger I/Os allocate a parent nvme_io_context and hand off to
 * nvme_io_context_pump, which keeps up to NVME_MAX_INFLIGHT_PER_IO
 * sibling chunks in flight; each sibling carries its own tag, PRPs,
 * and bounce buffer.  On every sibling CQE, nvme_complete_rq
 * decrements ctx->inflight, refills the slot via _pump, and replies
 * the originating IOStdReq via _finish once the last sibling lands.
 *
 * Returns NVME_IO_ASYNC on success or IOERR_* on synchronous failure.
 */
BYTE nvme_io_submit_rw(struct NVMeUnit *unit, struct IOStdReq *io,
                       u64 lba, ULONG blocks, u8 opcode, void *buffer)
{
    struct NVMeController *ctrl = unit->ctrl;
    const ULONG bytes = blocks << unit->blockShift;
    const ULONG mdts = ctrl->max_transfer_bytes;

    if (bytes == 0)
        return IOERR_BADLENGTH;

    /* Single-shot fast path. */
    if (mdts == 0 || bytes <= mdts)
    {
        BYTE err;
        struct nvme_request *req = nvme_req_alloc_io(unit, io, &err);
        if (!req)
            return err;

        if (unlikely(!nvme_check_ready(ctrl)))
            return nvme_fail_nonready_command(req);

        KprintfH("[nvme] io_submit_rw: single-shot io_Length=%lu lba=0x%08lx%08lx\n",
                 bytes, (ULONG)(lba >> 32), (ULONG)lba);

        BYTE serr = nvme_setup_rw(req, lba, blocks, opcode, buffer);
        if (serr)
        {
            nvme_req_destroy(req);
            return serr;
        }
        return nvme_submit_io(req);
    }

    /* Multi-chunk path.  Readiness check up front — the pump itself
     * has no nvme_request yet to route through nvme_fail_nonready_command. */
    enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);
    if (unlikely(state != NVME_CTRL_LIVE))
        return nvme_state_terminal(ctrl) ? IOERR_BADADDRESS : IOERR_UNITBUSY;

    struct nvme_io_context *ctx = slab_zalloc(&ctrl->ctx_slab);
    if (!ctx)
        return IOERR_SELFTEST;

    ctx->io = io;
    ctx->unit = unit;
    ctx->start_lba = lba;
    ctx->total_bytes = (u32)bytes;
    ctx->opcode = opcode;
    ctx->user_data = buffer;

    KprintfH("[nvme] io_submit_rw: ctx=%lx io_Length=%lu mdts=%lu — multi-chunk\n",
             (ULONG)ctx, bytes, mdts);

    BYTE err = nvme_io_context_pump(ctx);
    if (err != NVME_IO_ASYNC)
    {
        slab_free(&ctrl->ctx_slab, ctx);
        return err;
    }
    return NVME_IO_ASYNC;
}

/*
 * nvme_io_submit_flush - one-call NVMe Flush submission.
 */
BYTE nvme_io_submit_flush(struct NVMeUnit *unit, struct IOStdReq *io)
{
    BYTE err;
    struct nvme_request *req = nvme_req_alloc_io(unit, io, &err);
    if (!req)
        return err;

    if (unlikely(!nvme_check_ready(unit->ctrl)))
        return nvme_fail_nonready_command(req);

    nvme_setup_flush(req);
    return nvme_submit_io(req);
}

/*
 * nvme_io_submit_dsm - one-call DSM (Deallocate / discard) submission.
 *
 * @ranges must be a dma_zalloc'd page-aligned buffer of size
 * sizeof(*ranges) * NVME_DSM_MAX_RANGES (4 KiB) with the first @nr
 * slots filled by the caller and slots [nr..MAX-1] left zero (the
 * dma_zalloc guarantee).
 *
 * Takes ownership of @ranges unconditionally: on NVME_IO_ASYNC the
 * completion path frees it; on any error return the function frees
 * it before returning.  Caller must not touch @ranges after this call.
 */
BYTE nvme_io_submit_dsm(struct NVMeUnit *unit, struct IOStdReq *io,
                        struct nvme_dsm_range *ranges, u16 nr)
{
    BYTE err;
    struct nvme_request *req = nvme_req_alloc_io(unit, io, &err);
    if (!req)
    {
        dma_free(unit->ctrl->memoryPool, ranges);
        return err;
    }

    if (unlikely(!nvme_check_ready(unit->ctrl)))
    {
        dma_free(unit->ctrl->memoryPool, ranges);
        return nvme_fail_nonready_command(req);
    }

    if (nvme_setup_dsm(req, ranges, nr) != 0)
    {
        dma_free(unit->ctrl->memoryPool, ranges);
        nvme_req_destroy(req);
        return IOERR_SELFTEST;
    }

    /* nvme_setup_dsm attached @ranges to req->prp_pages[0]; from here
     * the request owns it and nvme_req_destroy / nvme_cleanup_cmd free
     * it via nvme_req_free_dma_buffers. */
    return nvme_submit_io(req);
}
