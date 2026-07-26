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

#include <minlist.h>
#include <timing.h> /* get_time, delay_us, delay_ms, time_deadline_passed */

#include <nvme/nvme_completion.h>
#include <device.h>          /* NVMeController/NVMeDevice/NVMeUnit, ERR_*, UnitTask, nvme_int_* */
#include <config.h>          /* STACK_SIZE, UNIT_TASK_PRIORITY */
#include <driver_task.h>     /* drv_task_spawn / drv_task_join */
#include <nvme/nvme_admin.h> /* nvme_configure_timestamp, nvme_configure_host_options */
#include <nvme/nvme_aen.h>   /* nvme_enable_aen, nvme_submit_aer */
#include <nvme/nvme_ctrl.h>  /* nvme_admin_ctrl, nvme_change_ctrl_state, nvme_init_identify */
#include <nvme/nvme_hmb.h>
#include <nvme/nvme_probe.h>
#include <nvme/nvme_queue.h>  /* nvme_setup_admin_queue, nvme_setup_io_queue, nvme_unquiesce_io_queues */
#include <nvme/nvme_quirks.h> /* nvme_lookup_quirks */
#include <nvme/nvme_scan.h>   /* nvme_scan_namespaces */

/* NVMe PCI class code: Mass Storage / NVM Express (base 0x01, sub 0x08, prog-if 0x02) */
#define NVME_PCI_CLASS 0x010802UL

/* ------------------------------------------------------------------ */
/* HW init / shutdown (moved from src/unit.c)                          */
/* ------------------------------------------------------------------ */

static BOOL nvme_pci_is_supported(struct Library *pcielibBase, struct pci_dev *pd)
{
    KprintfT("[nvme] pci_is_supported: pd=%lx vendor=%04lx device=%04lx\n",
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

    KprintfT("[nvme] hw_init: ctrl=%lx pd=%lx\n", (ULONG)ctrl, (ULONG)pd);

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
    KprintfT("[nvme] %s: BAR0=%lx\n", __func__, (ULONG)ctrl->bar0);
    return ERR_NO_ERROR;
}

static void hw_shutdown(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;
    KprintfT("[nvme] hw_shutdown: ctrl=%lx pd=%lx\n",
             (ULONG)ctrl, (ULONG)ctrl->pci_dev);
    if (pcielibBase && ctrl->pci_dev)
        SetBoardAttrs(ctrl->pci_dev, PRM_BoardOwner, 0UL, TAG_DONE);
    ctrl->bar0 = NULL;
}

/* ------------------------------------------------------------------ */
/* Controller lifecycle primitives (moved from core.c)                 */
/* ------------------------------------------------------------------ */

/*
 * nvme_wait_ready - poll CSTS until a masked field reaches an expected value
 *
 * Spins reading NVME_REG_CSTS at 1 ms intervals until (csts & mask) == val,
 * the @timeout expires, or the register reads all-Fs (device removed).
 * Used by nvme_enable_ctrl() to wait for CSTS.RDY=1 and nvme_disable_ctrl()
 * to wait for shutdown completion.
 *
 * @ctrl:    controller to poll
 * @mask:    bit mask to apply to CSTS before comparison
 * @val:     expected value after masking
 * @timeout: timeout in seconds (matches NVMe CAP.TO encoding)
 * @op:      human-readable operation name for the error message
 * Returns: 0 on success, -ENODEV on timeout / device removal
 */
static int nvme_wait_ready(struct NVMeController *ctrl, u32 mask, u32 val,
                           u32 timeout, const char *op)
{
#ifndef DEBUG
    (void)op; /* only referenced by debug logging */
#endif
    u32 start_us = get_time();
    u32 deadline_us = start_us + timeout * 1000000U;
    int polls = 0;
#ifndef TRACE
    (void)polls; /* only referenced by trace-tier logging below */
#endif

    KprintfT("[nvme] wait_ready(%s): mask=%08lx val=%08lx timeout=%lu s\n",
             op, (ULONG)mask, (ULONG)val, (ULONG)timeout);

    for (;;)
    {
        u32 csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
        polls++;
        if (csts == ~0U)
        {
            Kprintf("[nvme] wait_ready(%s): CSTS=~0 — device gone\n", op);
            return -ENODEV;
        }
        if ((csts & mask) == val)
        {
            KprintfT("[nvme] wait_ready(%s): OK CSTS=%08lx after %ld polls / %lu ms\n",
                     op, (ULONG)csts, (LONG)polls,
                     (ULONG)((get_time() - start_us) / 1000U));
            return 0;
        }

        delay_ms(1);
        if (time_deadline_passed(get_time(), deadline_us))
        {
            Kprintf("[nvme] %s: Device not ready; aborting %s, CSTS=%08lx after %lu ms\n",
                    __func__, op, (ULONG)csts,
                    (ULONG)((get_time() - start_us) / 1000U));
            return -ENODEV;
        }
    }
}

static int nvme_init_ctrl(struct NVMeController *ctrl, unsigned long quirks)
{
    ctrl->state = NVME_CTRL_NEW;
    _NewMinList(&ctrl->namespaces);
    InitSemaphore(&ctrl->scan_lock);
    ctrl->quirks = quirks;
    return 0;
}

/*
 * Initialize the cached copies of the Identify data and various controller
 * register in our nvme_ctrl structure.  This should be called as soon as
 * the admin queue is fully up and running.
 */
static int nvme_init_ctrl_finish(struct NVMeController *ctrl, BOOL was_suspended)
{
    int ret;
    (void)was_suspended;

    ctrl->vs = nvme_reg_read32(ctrl, NVME_REG_VS);

    ctrl->sqsize = (u16)((NVME_CAP_MQES(ctrl->cap) < (u32)ctrl->sqsize) ? NVME_CAP_MQES(ctrl->cap) : (u32)ctrl->sqsize);

    ret = nvme_init_identify(ctrl);
    if (ret)
        return ret;

    if (nvme_admin_ctrl(ctrl))
    {
        /*
         * An admin controller has one admin queue, but no I/O queues.
         * Override queue_count so it only creates an admin queue.
         */
        Kprintf("[nvme] %s: Subsystem is an administrative controller\n",
                __func__);
        ctrl->queue_count = 1;
    }

    ret = nvme_configure_timestamp(ctrl);
    if (ret < 0)
        return ret;

    ret = nvme_configure_host_options(ctrl);
    if (ret < 0)
        return ret;

    /* Optional interrupt coalescing — no-op unless DEVICE_IRQ_COALESCE_*
     * are set non-zero.  Best-effort: a controller that rejects it must
     * not fail probe. */
    nvme_configure_irq_coalesce(ctrl);

    clear_bit(NVME_CTRL_DIRTY_CAPABILITY, &ctrl->flags);
    ctrl->identified = TRUE;

    return 0;
}

/*
 * nvme_enable_ctrl - configure and enable an NVMe controller
 *
 * Reads CAP, validates the host page size is within the device's supported
 * range, selects the command set (CSI or NVM-only), sets CC fields (page
 * size, arbitration, entry sizes), writes CC.EN=1, then waits for CSTS.RDY.
 * Reads CRTO if the controller supports it to use the correct ready timeout.
 * Called at the start of each controller initialization or reset recovery.
 *
 * @ctrl: controller to enable
 * Returns: 0 on success, -ENODEV if page size incompatible or timeout,
 *          negative errno on register read/write failure
 */
static int nvme_enable_ctrl(struct NVMeController *ctrl)
{
    ctrl->cap = nvme_reg_read64(ctrl, NVME_REG_CAP);
    KprintfT("[nvme] enable_ctrl: CAP=%08lx%08lx\n", (u32)(ctrl->cap >> 32), (u32)ctrl->cap);
    unsigned dev_page_min = NVME_CAP_MPSMIN(ctrl->cap) + 12;

    if (NVME_CTRL_PAGE_SHIFT < dev_page_min)
    {
        Kprintf("[nvme] %s: Minimum device page size %lu too large for host (%lu)\n",
                __func__, 1 << dev_page_min, 1 << NVME_CTRL_PAGE_SHIFT);
        return -ENODEV;
    }

    if (NVME_CAP_CSS(ctrl->cap) & NVME_CAP_CSS_CSI)
        ctrl->ctrl_config = NVME_CC_CSS_CSI;
    else
        ctrl->ctrl_config = NVME_CC_CSS_NVM;

    /*
     * Setting CRIME results in CSTS.RDY before the media is ready. This
     * makes it possible for media related commands to return the error
     * NVME_SC_ADMIN_COMMAND_MEDIA_NOT_READY. Until the driver is
     * restructured to handle retries, disable CC.CRIME.
     */
    ctrl->ctrl_config &= ~(u32)NVME_CC_CRIME;

    ctrl->ctrl_config |= (NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT;
    ctrl->ctrl_config |= NVME_CC_AMS_RR | NVME_CC_SHN_NONE;
    ctrl->ctrl_config |= NVME_CC_IOSQES | NVME_CC_IOCQES;
    nvme_reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);

    /* CAP value may change after initial CC write */
    ctrl->cap = nvme_reg_read64(ctrl, NVME_REG_CAP);

    u32 timeout = NVME_CAP_TIMEOUT(ctrl->cap);
    if (ctrl->cap & NVME_CAP_CRMS_CRWMS)
    {
        u32 crto = nvme_reg_read32(ctrl, NVME_REG_CRTO);

        /*
         * CRTO should always be greater or equal to CAP.TO, but some
         * devices are known to get this wrong. Use the larger of the
         * two values.
         */
        u32 ready_timeout = NVME_CRTO_CRWMT(crto);

        if (ready_timeout < timeout)
            Kprintf("[nvme] %s: bad crto:%lx cap:%lx\n",
                    __func__, crto, ctrl->cap);
        else
            timeout = ready_timeout;
    }

    ctrl->ctrl_config |= NVME_CC_ENABLE;
    KprintfT("[nvme] enable_ctrl: writing final CC=%08lx (with CC.EN)\n", ctrl->ctrl_config);
    nvme_reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);
    KprintfT("[nvme] enable_ctrl: now waiting for CSTS.RDY=1, timeout=%lu s\n", (ULONG)((timeout + 1) / 2));
    return nvme_wait_ready(ctrl, NVME_CSTS_RDY, NVME_CSTS_RDY,
                           (timeout + 1) / 2, "initialisation");
}

/*
 * nvme_disable_ctrl - disable a controller or initiate a shutdown
 *
 * Clears CC.EN (or sets CC.SHN for a clean shutdown) and waits for the
 * controller to acknowledge via CSTS.RDY or CSTS.SHST.  For reset (not
 * shutdown), applies the DELAY_BEFORE_CHK_RDY quirk if needed.  Called
 * during controller reset, shutdown, and removal.
 *
 * @ctrl:     controller to disable
 * @shutdown: TRUE for a clean NVM Subsystem shutdown, FALSE for a bare reset
 * Returns: 0 on success, -ENODEV if the controller does not respond
 */
static int nvme_disable_ctrl(struct NVMeController *ctrl, BOOL shutdown)
{
#ifdef TRACE
    u32 csts_before = nvme_reg_read32(ctrl, NVME_REG_CSTS);

    KprintfT("[nvme] disable_ctrl(shutdown=%ld): CSTS_before=%08lx CC_before=%08lx\n",
             (LONG)shutdown, csts_before, ctrl->ctrl_config);
#endif

    ctrl->ctrl_config &= ~(u32)NVME_CC_SHN_MASK;
    if (shutdown)
        ctrl->ctrl_config |= NVME_CC_SHN_NORMAL;
    else
        ctrl->ctrl_config &= ~(u32)NVME_CC_ENABLE;

    KprintfT("[nvme] disable_ctrl: writing CC=%08lx\n", ctrl->ctrl_config);
    nvme_reg_write32(ctrl, NVME_REG_CC, ctrl->ctrl_config);

    if (shutdown)
    {
        return nvme_wait_ready(ctrl, NVME_CSTS_SHST_MASK,
                               NVME_CSTS_SHST_CMPLT,
                               ctrl->shutdown_timeout, "shutdown");
    }
    if (ctrl->quirks & NVME_QUIRK_DELAY_BEFORE_CHK_RDY)
        delay_ms(NVME_QUIRK_DELAY_AMOUNT);
    return nvme_wait_ready(ctrl, NVME_CSTS_RDY, 0,
                           (u32)((NVME_CAP_TIMEOUT(ctrl->cap) + 1U) / 2U), "reset");
}

/*
 * nvme_start_ctrl - arm the controller for serving I/O after the LIVE
 * transition.  Configures + posts the first AER and unquiesces the
 * I/O dispatch path.
 *
 * Called from nvme_probe_controller (initial bringup) and
 * nvme_reset_controller (reset recovery) — both immediately after
 * nvme_change_ctrl_state(NVME_CTRL_LIVE).  Each caller follows with
 * the namespace scan flavor appropriate to its task context:
 *   - probe runs on a foreign task → nvme_scan_namespaces (sync, units
 *     visible before probe returns so openLib can find them).
 *   - reset runs on the unit task  → nvme_queue_scan (async, spawns
 *     a fresh ScanWorker; sync admin from the unit task deadlocks).
 *
 * NVME_CTRL_STARTED_ONCE is intentionally not set — nothing in the
 * compiled Amiga sources reads it.
 */
static void nvme_start_ctrl(struct NVMeController *ctrl)
{
    nvme_enable_aen(ctrl);
    nvme_submit_aer(ctrl);
    nvme_unquiesce_io_queues(ctrl);
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

    KprintfT("[nvme] probe_controller: ctrl=%lx pci_dev=%lx\n", (ULONG)ctrl, (ULONG)ctrl->pci_dev);

    ret = hw_init(ctrl);
    if (ret != ERR_NO_ERROR)
        return ret;

    /* DMA buffers must live in Emu68 (Pi-DRAM) RAM the PCIe engine can reach, so the
     * DMA pool is region-restricted; with no device tree there is no reachable region
     * and we refuse to attach.  CPU-only metadata uses a separate ordinary Exec pool. */
    dma_mem_init(&ctrl->dma_ctx);
    ctrl->dmaPool = dma_pool_create(&ctrl->dma_ctx);
    ctrl->metaPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 256 * 1024, 8192);
    if (!ctrl->dmaPool || !ctrl->metaPool)
    {
        Kprintf("[nvme] %s: pool alloc failed (dma=%lx meta=%lx)\n", __func__,
                (ULONG)ctrl->dmaPool, (ULONG)ctrl->metaPool);
        dma_pool_delete(ctrl->dmaPool);
        ctrl->dmaPool = NULL;
        if (ctrl->metaPool)
        {
            DeletePool(ctrl->metaPool);
            ctrl->metaPool = NULL;
        }
        ret = ERR_ALLOC_ERROR;
        goto fail_hw;
    }

    /* Initialise the state machine (NEW), namespaces MinList, scan_lock, and seed
     * ctrl->quirks from the PCI ID table — BEFORE the slab caches (so
     * NVME_QUIRK_DMAPOOL_ALIGN_512 can steer the small-pool alignment), the task
     * spawns, and nvme_int_enable (so NVME_QUIRK_BROKEN_MSI can steer interrupt
     * setup). */
    nvme_init_ctrl(ctrl, nvme_lookup_quirks(ctrl->pci_dev));

    /* Hot-path slab caches.  Capacities sized to amortise per-grow
     * dma_alloc cost against worst-case in-flight working set:
     *   req_slab       — 64 per grow (peak ≈ IOQD 256 + admin 16).
     *   ctx_slab       — 16 per grow (chunked-I/O parent contexts).
     *   prp_large_slab — 16 per grow (4 KiB; chained / >32-entry lists, rare).
     *   prp_small_slab — 64 per grow (256 B; the common ≤128 KiB transfer).
     * The small pool is 256 B / 32 entries (Linux's prp_small_pool); its
     * alignment is bumped to 512 B under NVME_QUIRK_DMAPOOL_ALIGN_512. */
    slab_cache_init(&ctrl->req_slab, ctrl->metaPool, NULL,
                    sizeof(struct nvme_request), 0, 64);
    slab_cache_init(&ctrl->ctx_slab, ctrl->metaPool, NULL,
                    sizeof(struct nvme_io_context), 0, 16);
    slab_cache_init(&ctrl->prp_large_slab, ctrl->metaPool, ctrl->dmaPool,
                    NVME_CTRL_PAGE_SIZE, NVME_CTRL_PAGE_SIZE, 16);
    slab_cache_init(&ctrl->prp_small_slab, ctrl->metaPool, ctrl->dmaPool,
                    NVME_SMALL_POOL_SIZE,
                    (ctrl->quirks & NVME_QUIRK_DMAPOOL_ALIGN_512) ? 512u
                                                                  : NVME_SMALL_POOL_SIZE,
                    64);

    ret = drv_task_spawn(ctrl, UnitTask, "NVMe storage driver",
                         STACK_SIZE, UNIT_TASK_PRIORITY);
    if (ret != 0)
    {
        Kprintf("[nvme] %s: UnitTask spawn failed: %ld\n", __func__, ret);
        goto fail_pool;
    }

    ret = drv_task_spawn(ctrl, AdminWorker, "NVMe admin worker",
                         STACK_SIZE, UNIT_TASK_PRIORITY);
    if (ret != 0)
    {
        Kprintf("[nvme] %s: AdminWorker spawn failed: %ld\n", __func__, ret);
        goto fail_unit_task;
    }

    ret = nvme_int_enable(ctrl);
    if (ret != ERR_NO_ERROR)
    {
        Kprintf("[nvme] %s: nvme_int_enable failed: %ld\n", __func__, ret);
        goto fail_admin_task;
    }

    KprintfT("[nvme] %s: calling nvme_disable_ctrl (CC.EN=0, wait RDY=0)\n", __func__);
    if (nvme_disable_ctrl(ctrl, FALSE) != 0)
    {
        Kprintf("[nvme] %s: nvme_disable_ctrl failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_int;
    }
    KprintfT("[nvme] %s: nvme_disable_ctrl OK\n", __func__);

    if (nvme_setup_admin_queue(ctrl) != 0)
    {
        Kprintf("[nvme] %s: setup_admin_queue failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_int;
    }
    KprintfT("[nvme] %s: nvme_setup_admin_queue OK\n", __func__);

    KprintfT("[nvme] %s: calling nvme_enable_ctrl (CC.EN=1, wait RDY=1)\n", __func__);
    if (nvme_enable_ctrl(ctrl) != 0)
    {
        Kprintf("[nvme] %s: nvme_enable_ctrl failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_admin;
    }
    KprintfT("[nvme] %s: nvme_enable_ctrl OK, controller LIVE-ready\n", __func__);

    if (nvme_setup_io_queue(ctrl) != 0)
    {
        Kprintf("[nvme] %s: setup_io_queue failed\n", __func__);
        ret = ERR_CONTROLLER_ERROR;
        goto fail_enable;
    }
    KprintfT("[nvme] %s: nvme_setup_io_queue OK\n", __func__);

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
    KprintfT("[nvme] %s: nvme_init_ctrl_finish OK\n", __func__);

    /* If the controller advertises HMB (DRAM-less or otherwise wants
     * host RAM for FTL caching), allocate and enable it now — before
     * we expose namespaces and start serving I/O.  Non-fatal on
     * failure: the controller still works without HMB, just slower. */
    nvme_setup_host_mem(ctrl);

    nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE);

    /* Arm AEN and unquiesce I/O dispatch before the scan so we won't
     * miss namespace-change events the scan might trigger. */
    nvme_start_ctrl(ctrl);

    KprintfT("[nvme] %s: starting namespace scan\n", __func__);
    /* Identify Controller + per-NSID Identify; each found NSID calls
     * nvme_alloc_nvmeunit which adds a NVMeUnit to base->units.
     * Sync call: probe runs on a foreign task, and units must be
     * visible in base->units before nvme_probe_all returns so
     * subsequent openLib calls can find them. */
    nvme_scan_namespaces(ctrl);
    KprintfT("[nvme] %s: namespace scan complete\n", __func__);

    Kprintf("[nvme] %s: controller %lx fully brought up\n",
            __func__, (ULONG)ctrl->pci_dev);
    return ERR_NO_ERROR;

fail_enable:
    nvme_disable_ctrl(ctrl, FALSE);
fail_admin:
    nvme_teardown_queue(&ctrl->admin_q);
fail_int:
    nvme_int_shutdown(ctrl);
fail_admin_task:
    drv_task_join(&ctrl->admin_task);
fail_unit_task:
    drv_task_join(&ctrl->unit_task);
fail_pool:
    slab_cache_destroy(&ctrl->prp_small_slab);
    slab_cache_destroy(&ctrl->prp_large_slab);
    slab_cache_destroy(&ctrl->ctx_slab);
    slab_cache_destroy(&ctrl->req_slab);
    dma_pool_delete(ctrl->dmaPool);
    ctrl->dmaPool = NULL;
    DeletePool(ctrl->metaPool);
    ctrl->metaPool = NULL;
fail_hw:
    hw_shutdown(ctrl);
    return ret;
}

/*
 * nvme_alloc_nvmeunit - callback fired by core.c::nvme_alloc_ns for
 * each newly-discovered NSID during scan.
 *
 * Allocates a fresh NVMeUnit from the controller's metaPool, fills
 * in geometry, and AddTails to base->units with the next free unit
 * number.  The Amiga unit number is monotonically increasing across
 * all controllers.
 */
struct NVMeUnit *nvme_alloc_nvmeunit(struct NVMeController *ctrl,
                                     u32 nsid,
                                     ULONG blockSize, u8 blockShift,
                                     u64 logicalSectors,
                                     ULONG features)
{
    struct NVMeDevice *base = ctrl->device;

    struct NVMeUnit *unit = pool_zalloc(ctrl->metaPool, sizeof(*unit));
    if (!unit)
    {
        Kprintf("[nvme] %s: NVMeUnit alloc failed (nsid=%lu)\n", __func__, (ULONG)nsid);
        return NULL;
    }

    unit->ctrl = ctrl;
    unit->device = base;
    unit->nsid = nsid;
    unit->blockSize = blockSize;
    unit->blockShift = blockShift;
    unit->logicalSectors = logicalSectors;
    unit->features = features;

    /* Cache the per-unit Write Zeroes byte cap (read on the write-zeroes
     * dispatch paths): min(controller max_zeroes_sectors, the command's
     * 16-bit NLB = 64K logical blocks), block-aligned.  Pure function of
     * max_zeroes_sectors (set at identify, before scan) and blockShift, so
     * it shares blockShift's validity lifetime. */
    u64 by_dev = (u64)ctrl->max_zeroes_sectors << SECTOR_SHIFT;
    u64 by_nlb = (u64)0x10000u << blockShift;
    u64 lim = (by_dev < by_nlb) ? by_dev : by_nlb;
    if (lim > 0xFFFFFFFFu)
        lim = 0xFFFFFFFFu;
    unit->wz_max_bytes = (u32)lim & ~((1u << blockShift) - 1u);

    unit->changeCount = 1; /* present media; iotd_Count of 0 is "stale" */

    /* base->units is walked lockless by openLib (device.c) and now mutated
     * from the rescan path too — Forbid around the unit-number assignment
     * and link so a concurrent walker never observes a half-linked node. */
    Forbid();
    unit->unitNumber = (LONG)(base->nextUnitNumber++);
    AddTailMinList(&base->units, (struct MinNode *)unit);
    Permit();

    Kprintf("[nvme] %s: unit %ld NSID %lu blockSize=%lu blocks=%lu\n", __func__,
            unit->unitNumber, (ULONG)nsid, blockSize, (ULONG)logicalSectors);
    return unit;
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

    KprintfT("[nvme] probe_all: base=%lx\n", (ULONG)base);

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
 * nvme_ctrl_reset_quiesce - minimal pre-reset shutdown: the CC.SHN=NORMAL
 * handshake only.  Interrupt-safe (MMIO + busy-poll; reset_guard prepare
 * contract).
 *
 * Shutdown processing makes the controller finish or abort outstanding
 * commands, flush its caches, and cease all host-memory access (HMB
 * included) once CSTS.SHST reads complete.
 */
static void nvme_ctrl_reset_quiesce(struct NVMeController *ctrl)
{
    if (ctrl->bar0)
        nvme_disable_ctrl(ctrl, TRUE); /* CC.SHN=normal */
}

/*
 * nvme_reset_quiesce_all - quiesce every probed controller before a
 * machine reset so each SSD records a safe shutdown and ceases all
 * host-memory access (HMB included).
 */
void nvme_reset_quiesce_all(struct NVMeDevice *base)
{
    for (struct MinNode *node = base->controllers.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
    {
        struct NVMeController *ctrl = (struct NVMeController *)node;
        nvme_ctrl_reset_quiesce(ctrl);
    }
}

/*
 * nvme_ctrl_shutdown - quiesce a controller and run the NVMe shutdown
 * handshake (CC.SHN=NORMAL, wait CSTS.SHST complete).
 */
static void nvme_ctrl_shutdown(struct NVMeController *ctrl)
{
    /* Stop draining both ports before tearing the device down, then
     * force-complete every still-inflight request on both queues
     * so blocked waiters / IOStdReqs unblock before we free rings. */
    nvme_quiesce_io_queues(ctrl);
    nvme_quiesce_admin_queue(ctrl);
    nvme_flush_queue_inflight(&ctrl->io_q);
    nvme_flush_queue_inflight(&ctrl->admin_q);

    if (ctrl->bar0)
    {
        /* Release Host Memory Buffer first.  This issues Set
         * Features with NVME_HOST_MEM_ENABLE=0 so the controller
         * stops DMA-ing into the buffer before we free its
         * pages.  Must happen while the admin queue is still
         * functional, i.e. before nvme_disable_ctrl. */
        nvme_free_host_mem(ctrl);

        nvme_disable_ctrl(ctrl, TRUE); /* CC.SHN=normal */
    }
}

/*
 * nvme_unprobe_all - tear down every probed NVMe controller.
 *
 * Called from device expunge.  For each controller: clean-shutdown
 * the device (CC.SHN=normal), tear down queues, stop the task,
 * release PCIe ownership, delete the memory pools.  Every NVMeUnit
 * allocated by nvme_alloc_nvmeunit lives in ctrl->metaPool and
 * is freed implicitly when DeletePool runs — so we don't touch
 * base->units explicitly.
 */
void nvme_unprobe_all(struct NVMeDevice *base)
{
    KprintfT("[nvme] unprobe_all: base=%lx\n", (ULONG)base);

    struct MinNode *node, *next;
    node = base->controllers.mlh_Head;
    while ((next = node->mln_Succ) != NULL)
    {
        struct NVMeController *ctrl = (struct NVMeController *)node;
        node = next;

        KprintfT("[nvme] %s: tearing down ctrl %lx\n", __func__,
                 (ULONG)ctrl->pci_dev);

        nvme_ctrl_shutdown(ctrl);

        if (ctrl->bar0)
        {
            nvme_teardown_queue(&ctrl->io_q);
            nvme_teardown_queue(&ctrl->admin_q);
        }
        nvme_int_shutdown(ctrl);
        /* Stop AdminWorker BEFORE the unit task: AdminWorker may be
         * parked in nvme_submit_sync_cmd waiting on a CQE that only
         * the unit task delivers. */
        drv_task_join(&ctrl->admin_task);
        drv_task_join(&ctrl->unit_task);
        hw_shutdown(ctrl);

        if (ctrl->effects)
        {
            dma_free(ctrl->dmaPool, ctrl->effects);
            ctrl->effects = NULL;
        }
        slab_cache_destroy(&ctrl->prp_small_slab);
        slab_cache_destroy(&ctrl->prp_large_slab);
        slab_cache_destroy(&ctrl->ctx_slab);
        slab_cache_destroy(&ctrl->req_slab);
        if (ctrl->dmaPool)
        {
            dma_pool_delete(ctrl->dmaPool);
            ctrl->dmaPool = NULL;
        }
        if (ctrl->metaPool)
        {
            DeletePool(ctrl->metaPool);
            ctrl->metaPool = NULL;
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
    KprintfT("[nvme] reset_controller: ctrl=%lx\n", (ULONG)ctrl);

    if (!ctrl || !ctrl->bar0)
        return;

    KprintfT("[nvme] reset: starting\n");

    if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
    {
        Kprintf("[nvme] reset: state transition refused (state=%ld)\n",
                (long)nvme_ctrl_state(ctrl));
        return;
    }

    /* Hold off new dispatch from msgPort and adminPort while we tear the
     * device down.  Inflight requests on both queues are about to be
     * cancelled; the unquiesce happens after the rings are rebuilt. */
    nvme_quiesce_io_queues(ctrl);
    nvme_quiesce_admin_queue(ctrl);

    nvme_flush_queue_inflight(&ctrl->io_q);
    nvme_flush_queue_inflight(&ctrl->admin_q);

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
    /* Admin queue is back up; AdminWorker can dispatch again before we
     * use the admin path for Create I/O CQ/SQ. */
    nvme_unquiesce_admin_queue(ctrl);

    /* Rebuild the I/O queue pair ASYNCHRONOUSLY.  Reset runs on the unit
     * task — the same task that drains the CQ — so the sync
     * nvme_setup_io_queue would deadlock (Wait()ing on a completion only
     * this task can deliver).  Kick the async chain and return to the
     * drain loop; its terminal step calls nvme_reset_finish() to drive
     * the LIVE transition + rescan (or DEAD on failure).  The controller
     * stays in RESETTING until then. */
    nvme_reset_rebuild_io_async(ctrl);
    return;

dead:
    nvme_reset_finish(ctrl, FALSE);
}

/*
 * nvme_reset_finish - terminal step of the async reset bring-up; see the
 * header doc.  Mirrors the LIVE-go sequence from nvme_probe_controller.
 */
void nvme_reset_finish(struct NVMeController *ctrl, BOOL ok)
{
    if (!ok)
    {
        nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING);
        nvme_change_ctrl_state(ctrl, NVME_CTRL_DEAD);
        Kprintf("[nvme] reset: controller transitioned to DEAD\n");
        return;
    }

    nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING);
    nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE);

    /* Re-arm AEN and release the I/O msgPort.  Reset runs on the unit
     * task, so we MUST defer the rescan via nvme_queue_scan (signals
     * the unit task to spawn a ScanWorker); a sync nvme_scan_namespaces
     * here would deadlock on its own admin completions. */
    nvme_start_ctrl(ctrl);
    nvme_queue_scan(ctrl);

    KprintfT("[nvme] reset: complete, controller LIVE\n");
}
