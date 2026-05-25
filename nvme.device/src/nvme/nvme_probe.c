// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvme_probe.c — controller-level bringup.
 *
 * controller is brought up exactly once at device-init time and stays
 * enabled until expunge.  Namespaces are enumerated via Identify NS
 * List (or sequential fallback) during probe; each found NSID gets a
 * NVMeUnit allocated by the nvme_alloc_nvmeunit callback that
 * core.c::nvme_alloc_ns calls.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/bcmpcie_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#define BCMPCIE_BASE_NAME pcielibBase
#include <proto/bcmpcie.h>
#endif

#include <libraries/pci_constants.h>
#include <libraries/openpci.h>
#include <libraries/pcitags.h>
#include <utility/tagitem.h>

#include <types.h>
#include <minlist.h>
#include <debug.h>

#include <device.h>
#include <config.h>

#include <nvme/nvme.h> /* struct NVMeController, nvme_init_ctrl, … */
#include <nvme/nvme_hmb.h>

/* NVMe PCI class code: Mass Storage / NVM Express (base 0x01, sub 0x08, prog-if 0x02) */
#define NVME_PCI_CLASS 0x010802UL

/* ------------------------------------------------------------------ */
/* HW init / shutdown (moved from src/unit.c)                          */
/* ------------------------------------------------------------------ */

static BOOL nvme_pci_is_supported(struct Library *pcielibBase, struct pci_dev *pd)
{
    KprintfH("[nvme] pci_is_supported: pd=%lx vendor=%04lx device=%04lx\n",
             (ULONG)pd, (ULONG)pd->vendor, (ULONG)pd->device);
    if (pd->vendor == 0xFFFFU && pd->device == 0xFFFFU)
    {
        Kprintf("[nvme] %s: device not responding\n", __func__);
        return FALSE;
    }

    (void)pcielibBase;
    return TRUE;
}

static s32 hw_init(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;
    struct pci_dev *pd = ctrl->pci_dev;

    KprintfH("[nvme] hw_init: ctrl=%lx pd=%lx\n", (ULONG)ctrl, (ULONG)pd);

    if (!nvme_pci_is_supported(pcielibBase, pd))
        return ERR_CONTROLLER_ERROR;

    if (!SetBoardAttrs(pd, PRM_BoardOwner, (ULONG)FindTask(NULL), TAG_DONE))
    {
        Kprintf("[nvme] %s: board already owned\n", __func__);
        return ERR_CONTROLLER_ERROR;
    }
    if (!pd->base_address[0])
    {
        Kprintf("[nvme] %s: BAR0 not mapped\n", __func__);
        SetBoardAttrs(pd, PRM_BoardOwner, 0UL, TAG_DONE);
        return ERR_CONTROLLER_ERROR;
    }

    pci_set_master(pd);
    ctrl->bar0 = (volatile void *)(ULONG)pd->base_address[0];
    KprintfH("[nvme] %s: BAR0=%lx\n", __func__, (ULONG)ctrl->bar0);
    return ERR_NO_ERROR;
}

static void hw_shutdown(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;
    KprintfH("[nvme] hw_shutdown: ctrl=%lx pd=%lx\n",
             (ULONG)ctrl, (ULONG)ctrl->pci_dev);
    if (pcielibBase && ctrl->pci_dev)
        SetBoardAttrs(ctrl->pci_dev, PRM_BoardOwner, 0UL, TAG_DONE);
    ctrl->bar0 = NULL;
}

/* ------------------------------------------------------------------ */
/* Per-controller bringup                                              */
/* ------------------------------------------------------------------ */

/*
 * nvme_probe_controller - bring a single NVMe PCIe controller fully
 * online and enumerate its namespaces.
 *
 * On any failure between the steps, the partial state is unwound and
 * a negative-ish error returned.  The controller stays brought-up on
 * success until nvme_unprobe_all() at device expunge.
 */
static s32 nvme_probe_controller(struct NVMeController *ctrl)
{
    s32 ret;

    KprintfH("[nvme] probe_controller: ctrl=%lx pci_dev=%lx\n", (ULONG)ctrl, (ULONG)ctrl->pci_dev);

    ret = hw_init(ctrl);
    if (ret != ERR_NO_ERROR)
        return ret;

    ctrl->memoryPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 256 * 1024, 8192);
    if (!ctrl->memoryPool)
    {
        Kprintf("[nvme] %s: pool alloc failed\n", __func__);
        ret = ERR_ALLOC_ERROR;
        goto fail_hw;
    }

    ret = UnitTaskStart(ctrl);
    if (ret != ERR_NO_ERROR)
    {
        Kprintf("[nvme] %s: UnitTaskStart failed: %ld\n", __func__, ret);
        goto fail_pool;
    }

    ret = nvme_int_enable(ctrl);
    if (ret != ERR_NO_ERROR)
    {
        Kprintf("[nvme] %s: nvme_int_enable failed: %ld\n", __func__, ret);
        goto fail_task;
    }

    // Initialise the state machine to NEW, namespaces MinList, scan_lock semaphore.
    nvme_init_ctrl(ctrl, NULL, 0);

    KprintfH("[nvme] %s: calling nvme_disable_ctrl (CC.EN=0, wait RDY=0)\n", __func__);
    if (nvme_disable_ctrl(ctrl, FALSE) != 0)
    {
        Kprintf("[nvme] %s: nvme_disable_ctrl failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_int;
    }
    KprintfH("[nvme] %s: nvme_disable_ctrl OK\n", __func__);

    if (nvme_setup_admin_queue(ctrl) != 0)
    {
        Kprintf("[nvme] %s: setup_admin_queue failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_int;
    }
    KprintfH("[nvme] %s: nvme_setup_admin_queue OK\n", __func__);

    KprintfH("[nvme] %s: calling nvme_enable_ctrl (CC.EN=1, wait RDY=1)\n", __func__);
    if (nvme_enable_ctrl(ctrl) != 0)
    {
        Kprintf("[nvme] %s: nvme_enable_ctrl failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_admin;
    }
    KprintfH("[nvme] %s: nvme_enable_ctrl OK, controller LIVE-ready\n", __func__);

    if (nvme_setup_io_queue(ctrl) != 0)
    {
        Kprintf("[nvme] %s: setup_io_queue failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_enable;
    }
    KprintfH("[nvme] %s: nvme_setup_io_queue OK\n", __func__);

    nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING);

    /* Identify Controller + per-controller setup (MDTS, timestamp,
     * host options).  Sets ctrl->max_hw_sectors so the Amiga side's
     * max_transfer_bytes is populated before any data I/O arrives —
     * without this, ProcessCommand can't size-split large requests
     * and the controller returns "Invalid Field" (status 0x02) for
     * anything beyond MDTS. */
    if (nvme_init_ctrl_finish(ctrl, FALSE) != 0)
    {
        Kprintf("[nvme] %s: nvme_init_ctrl_finish failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_enable;
    }
    KprintfH("[nvme] %s: nvme_init_ctrl_finish OK\n", __func__);

    /* If the controller advertises HMB (DRAM-less or otherwise wants
     * host RAM for FTL caching), allocate and enable it now — before
     * we expose namespaces and start serving I/O.  Non-fatal on
     * failure: the controller still works without HMB, just slower. */
    nvme_setup_host_mem(ctrl);

    nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE);

    KprintfH("[nvme] %s: starting namespace scan\n", __func__);
    /* Identify Controller + per-NSID Identify; each found NSID calls
     * nvme_alloc_nvmeunit which adds a NVMeUnit to base->units. */
    nvme_scan_work(ctrl);
    KprintfH("[nvme] %s: namespace scan complete\n", __func__);

    Kprintf("[nvme] %s: controller %lx fully brought up\n",
            __func__, (ULONG)ctrl->pci_dev);
    return ERR_NO_ERROR;

fail_enable:
    nvme_disable_ctrl(ctrl, FALSE);
fail_admin:
    nvme_teardown_queue(&ctrl->admin_q);
fail_int:
    nvme_int_shutdown(ctrl);
fail_task:
    UnitTaskStop(ctrl);
fail_pool:
    DeletePool(ctrl->memoryPool);
    ctrl->memoryPool = NULL;
fail_hw:
    hw_shutdown(ctrl);
    return ret;
}

/*
 * nvme_alloc_nvmeunit - callback fired by core.c::nvme_alloc_ns for
 * each newly-discovered NSID during scan.
 *
 * Allocates a fresh NVMeUnit from the controller's memoryPool, fills
 * in geometry, and AddTails to base->units with the next free unit
 * number.  The Amiga unit number is monotonically increasing across
 * all controllers.
 */
struct NVMeUnit *nvme_alloc_nvmeunit(struct NVMeController *ctrl,
                                     u32 nsid,
                                     ULONG blockSize, u8 blockShift,
                                     u64 logicalSectors)
{
    struct NVMeDevice *base = ctrl->device;

    struct NVMeUnit *unit = pool_zalloc(ctrl->memoryPool, sizeof(*unit));
    if (!unit)
    {
        Kprintf("[nvme] %s: NVMeUnit alloc failed (nsid=%lu)\n", __func__, (ULONG)nsid);
        return NULL;
    }

    unit->ctrl = ctrl;
    unit->device = base;
    unit->nsid = nsid;
    unit->unitNumber = (LONG)(base->nextUnitNumber++);
    unit->blockSize = blockSize;
    unit->blockShift = blockShift;
    unit->logicalSectors = logicalSectors;

    AddTailMinList(&base->units, (struct MinNode *)unit);

    Kprintf("[nvme] %s: unit %ld NSID %lu blockSize=%lu blocks=%lu\n", __func__,
            unit->unitNumber, (ULONG)nsid, blockSize, (ULONG)logicalSectors);
    return unit;
}

/*
 * nvme_cache_id_strings - stash Identify Controller text fields.
 *
 * Called once by core.c::nvme_init_identify after the
 * Identify Controller (CNS=0x01) response is parsed.  The buffer
 * lives in pool memory and is freed by core.c when the function
 * returns, so we copy out before that happens.
 *
 * Strings are space-padded fixed-width per NVMe spec §5.15.2;
 * nvme_scsi.c handles trimming when populating the INQUIRY response.
 */
void nvme_cache_id_strings(struct NVMeController *ctrl,
                           const char *mn, const char *fr,
                           const char *sn)
{
    if (!ctrl)
        return;
    CopyMem(mn, ctrl->id_strings.model, sizeof(ctrl->id_strings.model));
    CopyMem(fr, ctrl->id_strings.firmware, sizeof(ctrl->id_strings.firmware));
    CopyMem(sn, ctrl->id_strings.serial, sizeof(ctrl->id_strings.serial));
    Kprintf("[nvme] %s: model='%.40s' fw='%.8s' sn='%.20s'\n",
            __func__,
            ctrl->id_strings.model,
            ctrl->id_strings.firmware,
            ctrl->id_strings.serial);
}

/*
 * nvme_set_limits - record per-controller transfer ceiling.
 *
 * Three inputs combine:
 *   1. Identify Controller MDTS field (passed in as @max_hw_sectors,
 *      in 512B units; UINT_MAX when id->mdts == 0).
 *   2. NVME_AMIGA_DEFAULT_MAX_BYTES — fallback for MDTS-unreported
 *      devices, matching Linux's spirit (Linux uses NVME_MAX_BYTES =
 *      8 MiB in pci.c).
 */
#define NVME_AMIGA_DEFAULT_MAX_BYTES (8UL * 1024UL * 1024UL)

void nvme_set_limits(struct NVMeController *ctrl, u32 max_hw_sectors)
{
    u32 bytes;

    if (!ctrl)
        return;

    if (max_hw_sectors == 0 || max_hw_sectors == U32_MAX)
        bytes = NVME_AMIGA_DEFAULT_MAX_BYTES;
    else
        bytes = max_hw_sectors << 9;

    ctrl->max_transfer_bytes = bytes;

    Kprintf("[nvme] %s: max_hw_sectors=%lu max_transfer_bytes=%lu\n",
            __func__, (ULONG)max_hw_sectors,
            (ULONG)ctrl->max_transfer_bytes);
}

/* ------------------------------------------------------------------ */
/* Device-wide probe / unprobe                                         */
/* ------------------------------------------------------------------ */

/*
 * nvme_probe_all - enumerate PCIe bus, bring up every NVMe controller.
 *
 * Caller (device.c::initDevice) runs us once at device-base init.  We
 * find every NVMe-class PCIe device, allocate a NVMeController, and
 * call nvme_probe_controller to bring it fully online (admin queue,
 * I/O queue, namespace scan).  Failed controllers are freed and
 * skipped; successful ones link into base->controllers and stay alive
 * until nvme_unprobe_all() at expunge.
 */
s32 nvme_probe_all(struct NVMeDevice *base)
{
    struct Library *pcielibBase = base->pcieBase;
    struct pci_dev *pd = NULL;
    int nctrls = 0;

    KprintfH("[nvme] probe_all: base=%lx\n", (ULONG)base);

    base->nextUnitNumber = 0;

    while ((pd = pci_find_class(NVME_PCI_CLASS, pd)) != NULL)
    {
        struct NVMeController *ctrl = AllocMem(sizeof(*ctrl), MEMF_PUBLIC | MEMF_CLEAR);
        if (!ctrl)
        {
            Kprintf("[nvme] %s: NVMeController alloc failed\n", __func__);
            return nctrls > 0 ? 0 : -1;
        }
        ctrl->pci_dev = pd;
        ctrl->bar0 = NULL;
        ctrl->device = base;
        ctrl->utilityBase = base->utilityBase;

        if (nvme_probe_controller(ctrl) != ERR_NO_ERROR)
        {
            Kprintf("[nvme] %s: probe failed, skipping ctrl %lx\n",
                    __func__, (ULONG)pd);
            FreeMem(ctrl, sizeof(*ctrl));
            continue;
        }

        AddTailMinList(&base->controllers, &ctrl->node);
        nctrls++;
    }

    Kprintf("[nvme] %s: %ld controller(s), %lu unit(s)\n", __func__,
            (LONG)nctrls, base->nextUnitNumber);

    return base->nextUnitNumber > 0 ? 0 : -1;
}

/*
 * nvme_unprobe_all - tear down every probed NVMe controller.
 *
 * Called from device expunge.  For each controller: clean-shutdown
 * the device (CC.SHN=normal), tear down queues, stop the task,
 * release PCIe ownership, delete the memory pool.  Every NVMeUnit
 * allocated by nvme_alloc_nvmeunit lives in ctrl->memoryPool and
 * is freed implicitly when DeletePool runs — so we don't touch
 * base->units explicitly.
 */
void nvme_unprobe_all(struct NVMeDevice *base)
{
    KprintfH("[nvme] unprobe_all: base=%lx\n", (ULONG)base);

    struct MinNode *node, *next;
    node = base->controllers.mlh_Head;
    while ((next = node->mln_Succ) != NULL)
    {
        struct NVMeController *ctrl = (struct NVMeController *)node;
        node = next;

        Kprintf("[nvme] %s: tearing down ctrl %lx\n", __func__,
                (ULONG)ctrl->pci_dev);

        if (ctrl->bar0)
        {
            /* Release Host Memory Buffer first.  This issues Set
             * Features with NVME_HOST_MEM_ENABLE=0 so the controller
             * stops DMA-ing into the buffer before we free its
             * pages.  Must happen while the admin queue is still
             * functional, i.e. before nvme_disable_ctrl. */
            nvme_free_host_mem(ctrl);

            nvme_disable_ctrl(ctrl, TRUE); /* CC.SHN=normal */
            nvme_teardown_queue(&ctrl->io_q);
            nvme_teardown_queue(&ctrl->admin_q);
        }
        nvme_int_shutdown(ctrl);
        UnitTaskStop(ctrl);
        hw_shutdown(ctrl);
        if (ctrl->memoryPool)
        {
            DeletePool(ctrl->memoryPool);
            ctrl->memoryPool = NULL;
        }
    }

    /* Free NVMeController structs themselves.  Walk forward; each
     * Remove() invalidates node->mln_Succ before we read it, so
     * snapshot first. */
    while ((node = base->controllers.mlh_Head) && node->mln_Succ)
    {
        struct NVMeController *ctrl = (struct NVMeController *)node;
        Remove((struct Node *)node);
        FreeMem(ctrl, sizeof(*ctrl));
    }

    /* base->units entries were pool_zalloc'd from each ctrl's pool,
     * which is now deleted.  Just clear the list head. */
    _NewMinList(&base->units);
}

/*
 * nvme_reset_controller - synchronous controller reset (runs on the
 * controller task).
 *
 * Equivalent of Linux's `nvme_reset_work` + `nvme_dev_disable`,
 * tailored to the Amiga lifecycle: there's only one ctrl task and one
 * I/O queue pair, so no per-CPU teardown is needed.
 *
 * Sequence:
 *   1. State NEW/LIVE → RESETTING.
 *   2. Cancel every inflight request (signal their waiters with
 *      NVME_SC_HOST_ABORTED_CMD) so blocked admin callers unblock.
 *   3. CC.EN=0, wait CSTS.RDY=0.
 *   4. Free the I/O + admin rings.
 *   5. Re-allocate admin ring, re-program AQA/ASQ/ACQ, CC.EN=1.
 *   6. Re-create the I/O queue pair via admin commands.
 *   7. State CONNECTING → LIVE; re-run namespace scan.
 *
 * On hard failure (re-enable doesn't bring CSTS.RDY up) we transition
 * to DEAD and leave the controller in that state — the next IO
 * request will fail.  An expunge cycle is required to recover.
 */
void nvme_reset_controller(struct NVMeController *ctrl)
{
    KprintfH("[nvme] reset_controller: ctrl=%lx\n", (ULONG)ctrl);

    if (!ctrl || !ctrl->bar0)
        return;

    Kprintf("[nvme] reset: starting\n");

    if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
    {
        Kprintf("[nvme] reset: state transition refused (state=%ld)\n",
                (long)nvme_ctrl_state(ctrl));
        return;
    }

    nvme_cancel_tagset(ctrl);

    if (nvme_disable_ctrl(ctrl, FALSE) != 0)
        Kprintf("[nvme] reset: disable_ctrl reports error (ignoring)\n");

    nvme_teardown_queue(&ctrl->io_q);
    nvme_teardown_queue(&ctrl->admin_q);

    if (nvme_setup_admin_queue(ctrl) != 0)
    {
        Kprintf("[nvme] reset: setup_admin_queue failed\n");
        goto dead;
    }
    if (nvme_enable_ctrl(ctrl) != 0)
    {
        Kprintf("[nvme] reset: enable_ctrl failed\n");
        goto dead;
    }
    if (nvme_setup_io_queue(ctrl) != 0)
    {
        Kprintf("[nvme] reset: setup_io_queue failed\n");
        goto dead;
    }

    nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING);
    nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE);

    nvme_scan_work(ctrl);

    Kprintf("[nvme] reset: complete, controller LIVE\n");
    return;

dead:
    nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING);
    nvme_change_ctrl_state(ctrl, NVME_CTRL_DEAD);
    Kprintf("[nvme] reset: controller transitioned to DEAD\n");
}
