// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvme_queue.c - SQ + CQ ring lifecycle and CQ-drain mechanics.
 *
 * One generic struct nvme_queue + helpers handles both the admin (qid 0)
 * and the I/O (qid 1) queues.  Setup/teardown allocates page-aligned SQ
 * and CQ ring memory plus the per-queue in-flight table; drain walks
 * the CQ by phase bit; the watchdog tick handler scans every queue's
 * in-flight table for timed-out commands.
 *
 * Entry points called from the unit task Wait-loop:
 *   nvme_process_completions() - drain fresh CQEs on both queues, dispatch
 *                                  each via nvme_complete_rq().
 *   nvme_tick_watchdog()       - sweep inflight[] on both queues for
 *                                  timed-out commands, plus advance the
 *                                  controller-wide CRDT retry_list.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <timing.h>

#include <nvme/nvme_ctrl.h> /* struct NVMeController, NVME_CTRL_* flags, nvme_ctrl_state */
#include <nvme/nvme_completion.h>
#include <device.h>
#include <nvme/nvme_io.h>
#include <nvme/nvme_admin.h> /* nvme_abort_request, nvme_submit_sync_cmd */
#include <nvme/nvme_queue.h>
#include <nvme/nvme_probe.h> /* nvme_reset_finish (async reset terminal) */

#define NVME_IO_QID 1 /* sole I/O queue ID (we create one pair) */

/*
 * nvme_setup_queue - allocate ring memory + inflight table for one queue.
 *
 * SQ and CQ rings are page-aligned (NVMe spec §4.1.4 requires the
 * controller-visible base addresses to be 4 KiB aligned) and come from
 * the controller's pool via dma_zalloc.  The inflight table is plain
 * pool memory (no alignment requirement).
 *
 * Computes doorbell offsets from @qid and the controller's db_stride
 * (which must already be set — for admin queue setup, the caller reads
 * CAP.DSTRD first).  Initialises cq_phase=1 so the first device-written
 * CQE (which the controller writes with phase=1) is recognised as fresh.
 *
 * Caller publishes the queue (by writing ASQ/ACQ for admin, or by
 * submitting Create I/O CQ/SQ admin commands) AFTER this returns.
 * Drain on this queue is safe before publication because cq_phase=1 vs
 * cq[*].phase=0 makes drain_cq exit on the first iteration.
 *
 * Returns 0 on success, -1 on OOM.  Partial-success cleanup is done
 * locally; on return q is either fully initialised or zeroed.
 */
static s32 nvme_setup_queue(struct NVMeController *ctrl, struct nvme_queue *q, u16 qid, u16 depth)
{
    /* SQE stride: 64 B normally; 128 B on the I/O queue when
     * NVME_QUIRK_128_BYTES_SQES is set (some Apple controllers require a
     * non-standard stride and ignore CC.IOSQES).  Admin (qid 0) is always 64. */
    const BOOL sqe_128b = (qid != 0 && (ctrl->quirks & NVME_QUIRK_128_BYTES_SQES)) != 0;
    const ULONG sq_bytes = (ULONG)depth *
                           (sqe_128b ? (ULONG)(2u * sizeof(struct nvme_command))
                                     : (ULONG)sizeof(struct nvme_command));
    const ULONG cq_bytes = (ULONG)depth * sizeof(struct nvme_completion);
    const ULONG inflight_bytes = (ULONG)depth * sizeof(struct nvme_request *);

    KprintfH("[nvme] setup_queue: ctrl=%lx q=%lx qid=%lu depth=%lu\n",
             (ULONG)ctrl, (ULONG)q, (ULONG)qid, (ULONG)depth);

    mem_zero(q, sizeof(*q));
    q->ctrl = ctrl;
    q->qid = qid;
    q->depth = depth;
    q->sqe_128b = sqe_128b;
    q->skip_cid_gen = (ctrl->quirks & NVME_QUIRK_SKIP_CID_GEN) != 0;
    q->cq_phase = 1;
    q->sq_db_off = NVME_REG_DBS + (u32)(2u * qid) * ctrl->db_stride;
    q->cq_db_off = NVME_REG_DBS + (u32)(2u * qid + 1u) * ctrl->db_stride;

    q->sq = dma_zalloc(ctrl->dmaPool, NVME_CTRL_PAGE_SIZE, sq_bytes);
    if (!q->sq)
    {
        Kprintf("[nvme] %s: SQ alloc (%lu B) failed\n", __func__, sq_bytes);
        goto fail_sq;
    }
    nvme_cache_flush(q->sq, sq_bytes, TRUE); /* device reads SQ entries */

    q->cq = dma_zalloc(ctrl->dmaPool, NVME_CTRL_PAGE_SIZE, cq_bytes);
    if (!q->cq)
    {
        Kprintf("[nvme] %s: CQ alloc (%lu B) failed\n", __func__, cq_bytes);
        goto fail_cq;
    }
    /* CQ is zero-init from dma_zalloc; clean+invalidate (device writes it) so the
     * zeroed phase=0 reaches RAM and the dirty lines can't evict over CQEs. */
    nvme_cache_flush(q->cq, cq_bytes, FALSE);

    q->inflight = pool_zalloc(ctrl->metaPool, inflight_bytes);
    if (!q->inflight)
    {
        Kprintf("[nvme] %s: inflight alloc (%lu B) failed\n", __func__, inflight_bytes);
        goto fail_inflight;
    }

    q->free_stack = pool_zalloc(ctrl->metaPool, (ULONG)depth * sizeof(u16));
    if (!q->free_stack)
    {
        Kprintf("[nvme] %s: free-stack alloc (%lu B) failed\n", __func__,
                (ULONG)depth * sizeof(u16));
        goto fail_free_stack;
    }
    /* Push every CID onto the free stack in reverse so the first pop
     * returns CID 0 (low-numbered tags first — friendlier for tracing).
     * Entries are encoded CIDs with generation 0, which equal the bare tag;
     * nvme_inflight_release re-pushes them with the bumped generation. */
    for (u16 i = 0; i < depth; i++)
        q->free_stack[i] = (u16)(depth - 1 - i);
    q->free_top = depth;

    return 0;

fail_free_stack:
    pool_free(ctrl->metaPool, q->inflight);
fail_inflight:
    dma_free(ctrl->dmaPool, q->cq);
fail_cq:
    dma_free(ctrl->dmaPool, q->sq);
fail_sq:
    mem_zero(q, sizeof(*q));
    return -1;
}

/*
 * nvme_teardown_queue - release ring memory + inflight table.
 *
 * Safe to call on a zero / partially-initialised queue (does nothing
 * for whichever piece was not allocated).  Zeroes the struct so a
 * subsequent setup starts from a known state — important for the reset
 * path that tears down and rebuilds.
 */
void nvme_teardown_queue(struct nvme_queue *q)
{
    KprintfH("[nvme] teardown_queue: q=%lx qid=%lu sq=%lx cq=%lx inflight=%lx\n",
             (ULONG)q, (ULONG)q->qid,
             (ULONG)q->sq, (ULONG)q->cq, (ULONG)q->inflight);

    if (q->ctrl)
    {
        if (q->sq)
            dma_free(q->ctrl->dmaPool, q->sq);
        if (q->cq)
            dma_free(q->ctrl->dmaPool, q->cq);
        if (q->inflight)
            pool_free(q->ctrl->metaPool, q->inflight);
        if (q->free_stack)
            pool_free(q->ctrl->metaPool, q->free_stack);
    }

    mem_zero(q, sizeof(*q));
}

/*
 * drain_cq - walk a single queue's CQ from cq_head, looking for fresh
 * entries (phase bit matches q->cq_phase).  Each fresh CQE has its
 * status/result copied into the inflight request, then nvme_complete_rq
 * fires (which signals the waiter or replies the IOStdReq).  Head
 * advances with phase-flip on ring wrap; the new head is written to
 * q->cq_db_off if anything was drained.
 */
static void drain_cq(struct nvme_queue *q)
{
    u16 head = q->cq_head;
    u16 phase = q->cq_phase;
    int drained = 0;

    if (!q->cq)
        return;

    while (1)
    {
        struct nvme_completion *cqe = &q->cq[head];

        /* Invalidate the CQE cache line so we see what the controller
         * DMA-wrote rather than a stale CPU-cached value. */
        nvme_cache_inval(cqe, sizeof(*cqe));

        const u16 status_le = le16(cqe->status);
        const u16 cqe_phase = status_le & 1;
        const u16 status = status_le >> 1;
        const u16 raw_cid = cqe->command_id;

        if (cqe_phase != phase)
            break; /* nothing fresh */

        /* The slot index is the low 12 bits; the generation packed above it
         * guards against stale/duplicate CQEs for a reused slot.  The expected
         * generation lives in the in-flight request's own cid, so a fresh CQE
         * matches iff raw_cid == req->cid (under SKIP_CID_GEN both are the bare
         * tag, so this still holds). */
        const u16 tag = raw_cid & 0x0fff;

        struct nvme_request *req = (tag < q->depth) ? q->inflight[tag] : NULL;
        if (req && raw_cid == req->cid)
        {
            req->status = status;
            req->result = cqe->result;
            nvme_inflight_release(q, req->cid);
            KprintfH("[nvme] CQE: qid=%lu head=%lu cid=0x%lx status=0x%04lx (%s) phase=%lu → completing %s\n",
                     (ULONG)q->qid, (ULONG)head, (ULONG)req->cid,
                     (ULONG)status, nvme_get_error_status_str(status), (ULONG)cqe_phase,
                     req->unit ? "I/O" : "admin");
            nvme_complete_rq(req);
        }
        else
        {
            Kprintf("[nvme] drain_cq: %s CQE qid=%lu head=%lu cid=0x%lx status=0x%lx (%s) phase=%lu\n",
                    req ? "stale (cid mismatch)" : "unexpected",
                    (ULONG)q->qid, (ULONG)head, (ULONG)raw_cid,
                    (ULONG)status, nvme_get_error_status_str(status), (ULONG)cqe_phase);
        }

        head++;
        if (head == q->depth)
        {
            head = 0;
            phase ^= 1; /* phase flips on every ring wrap */
        }
        drained++;
    }

    if (drained)
    {
        q->cq_head = head;
        q->cq_phase = (u8)phase;
        mmio_write32((u32)head, (volatile UBYTE *)q->ctrl->bar0 + q->cq_db_off);
    }
}

/*
 * nvme_process_completions - drain both queues' CQs.
 *
 * Called from the controller task when the MSI signal fires.
 */
void nvme_process_completions(struct NVMeController *ctrl)
{
    drain_cq(&ctrl->admin_q);

    /* Draining the I/O CQ can resubmit work on the same queue: chunked-I/O
     * refills (nvme_io_context_pump) and CRDT/transient retries
     * (nvme_resubmit_io) both fire from nvme_complete_rq.  Batch their SQ-tail
     * doorbells into a single commit for the whole drain pass. */
    nvme_sq_batch_begin(&ctrl->io_q);
    drain_cq(&ctrl->io_q);
    nvme_sq_batch_end(&ctrl->io_q);
}

/*
 * watchdog_scan_queue - timeout sweep for one queue's inflight table.
 *
 * Called by nvme_tick_watchdog for both the admin and I/O queues.
 *
 * Three states per in-flight request:
 *
 *   1. Healthy or under its per-class timeout — leave alone.
 *
 *   2. First timeout (elapsed >= NVME_IO_TIMEOUT / NVME_ADMIN_TIMEOUT,
 *      NVME_REQ_ABORT_SENT not yet set) — submit an Abort admin
 *      command targeting (sqid=q->qid, cid=req->cid), mark the
 *      request NVME_REQ_ABORT_SENT, stash abort_us = now.  The
 *      request stays in inflight[]: the original or NVME_SC_ABORT_REQ
 *      CQE will arrive via the normal drain_cq path.
 *
 *   3. Abort grace expired (NVME_REQ_ABORT_SENT set and elapsed since
 *      abort_us >= NVME_ABORT_TIMEOUT) — controller is wedged.
 *      Signal reset_signal so the unit task runs
 *      nvme_reset_controller, which tears down both queues' inflight
 *      tables.
 *
 *   Special case: if the timed-out request is itself an Abort admin
 *   command, sending another Abort would loop forever.  Escalate
 *   straight to reset.
 */

/*
 * nvme_watchdog_escalate - last-resort recovery for a wedged command.
 *
 * Only a LIVE controller is reset: reset (RESETTING) tears down and
 * rebuilds the queues, which must not run concurrently with the
 * controller bring-up that owns its own failure/cleanup (NEW/CONNECTING)
 * or with an in-progress reset (RESETTING) — both would race the teardown.
 * Off the LIVE path we instead force-complete @q's in-flight requests in
 * place (host-aborted), so a blocked bring-up / reset-chain caller fails
 * gracefully without a second, racing reset.
 */
static void nvme_watchdog_escalate(struct NVMeController *ctrl, struct nvme_queue *q)
{
    if (nvme_ctrl_state(ctrl) == NVME_CTRL_LIVE)
        Signal(ctrl->unit_task, 1UL << ctrl->reset_signal);
    else
        nvme_flush_queue_inflight(q);
}

static void watchdog_scan_queue(struct nvme_queue *q, u32 now)
{
    struct NVMeController *ctrl = q->ctrl;

    for (u16 i = 0; i < q->depth; i++)
    {
        struct nvme_request *req = q->inflight[i];
        if (!req || req->submit_us == 0)
            continue;

        /* AERs sit in the admin inflight[] indefinitely waiting for
         * the controller to post an event.  Never abort them. */
        if (req->flags & NVME_REQ_AER)
            continue;

        /* an Abort is already in flight for this request.
         * Measure the grace period separately from submit_us. */
        if (req->flags & NVME_REQ_ABORT_SENT)
        {
            if ((now - req->abort_us) / 1000U < NVME_ABORT_TIMEOUT)
                continue;

            KprintfH("[nvme] abort grace expired: qid=%lu cid=0x%lx "
                     "opcode=0x%02lx — escalating\n",
                     (ULONG)q->qid, (ULONG)req->cid,
                     (ULONG)req->cmd.common.opcode);
            nvme_watchdog_escalate(ctrl, q);
            return; /* one escalation is enough per tick */
        }

        /* first timeout test. */
        u32 limit_ms = req->unit ? NVME_IO_TIMEOUT : NVME_ADMIN_TIMEOUT;
        u32 elapsed_ms = (now - req->submit_us) / 1000U;
        if (elapsed_ms < limit_ms)
            continue;

        /* Special case: the request that timed out IS an Abort cmd
         * itself.  Sending another Abort would infinite-loop, so go
         * straight to controller reset. */
        if (req->cmd.common.opcode == nvme_admin_abort_cmd)
        {
            Kprintf("[nvme] abort cmd itself timed out: cid=0x%lx — escalating\n",
                    (ULONG)req->cid);
            nvme_watchdog_escalate(ctrl, q);
            return;
        }

        Kprintf("[nvme] timeout: qid=%lu cid=0x%lx opcode=0x%02lx (%s) "
                "elapsed=%lu ms — issuing Abort\n",
                (ULONG)q->qid, (ULONG)req->cid,
                (ULONG)req->cmd.common.opcode,
                nvme_opcode_str(q->qid, req->cmd.common.opcode),
                (ULONG)elapsed_ms);

        if (nvme_abort_request(ctrl, req) != 0)
        {
            Kprintf("[nvme] failed to submit Abort for qid=%lu cid=0x%lx\n",
                    (ULONG)q->qid, (ULONG)req->cid);
            continue;
        }

        req->flags |= NVME_REQ_ABORT_SENT;
        req->abort_us = now;
    }
}

/*
 * nvme_tick_watchdog - per-tick housekeeping fired by the watchdog timer.
 *
 * Two responsibilities:
 *
 *   1. Inflight timeout — walk both queues' inflight tables via
 *      watchdog_scan_queue; issue Aborts for stalled requests and
 *      escalate to controller reset if Abort itself stalls.
 *
 *   2. CRDT retry — walk ctrl->retry_list.  For each entry whose
 *      deadline_us has passed, Remove and re-submit via
 *      nvme_submit_io.  These are requests that nvme_retry_req parked
 *      because the controller returned a non-zero CRDT.
 */
void nvme_tick_watchdog(struct NVMeController *ctrl)
{
    /* Reap any CQEs the controller posted without (or before) an MSI: the
     * completion was DMA-written to host memory regardless of whether the
     * interrupt was delivered, so a missing/late MSI must not strand it
     * until the timeout sweep below aborts a command that already finished.
     * Same unit-task context as the IRQ-driven drain, so this is safe. */
    nvme_process_completions(ctrl);

    u32 now = get_time();

    watchdog_scan_queue(&ctrl->admin_q, now);
    watchdog_scan_queue(&ctrl->io_q, now);

    /* CRDT retries */
    struct MinNode *node = ctrl->retry_list.mlh_Head;
    struct MinNode *next;

    while ((next = node->mln_Succ) != NULL)
    {
        struct nvme_request *req = (struct nvme_request *)node;
        node = next; /* advance BEFORE Remove/submit */

        if ((s32)(now - req->deadline_us) < 0)
            continue;

        Remove((struct Node *)&req->node);
        (void)nvme_resubmit_io(req);
    }
}

/* ------------------------------------------------------------------ */
/* Queue-type-specific bring-up.                                       */
/* ------------------------------------------------------------------ */

/*
 * nvme_setup_admin_queue - allocate admin SQ+CQ rings and program AQA/ASQ/ACQ.
 *
 * Called from nvme_probe_controller after the memory pool is created and
 * the controller task is running, but before nvme_enable_ctrl().  Reads
 * CAP.DSTRD to derive ctrl->db_stride (which the queue helper needs to
 * compute doorbell offsets), then allocates the rings via the generic
 * nvme_setup_queue and finally programs the admin-specific registers.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
s32 nvme_setup_admin_queue(struct NVMeController *ctrl)
{
    volatile UBYTE *bar = (volatile UBYTE *)ctrl->bar0;

    KprintfH("[nvme] setup_admin_queue: ctrl=%lx\n", (ULONG)ctrl);

    /* Doorbell stride = 4 << CAP.DSTRD (DSTRD in CAP[35:32], so bits
     * [3:0] of the high dword).  Must be read before nvme_setup_queue
     * because each queue's sq_db_off / cq_db_off depend on it. */
#ifdef DEBUG_HIGH     
    u32 cap_lo = mmio_read32(bar + NVME_REG_CAP);
#endif
    u32 cap_hi = mmio_read32(bar + NVME_REG_CAP + 4);
    u32 dstrd = cap_hi & 0xF;
    ctrl->db_stride = 4UL << dstrd;
    KprintfH("[nvme] %s: CAP lo=%08lx hi=%08lx → dstrd=%lu, db_stride=%lu\n",
             __func__, cap_lo, cap_hi, dstrd, ctrl->db_stride);

    if (nvme_setup_queue(ctrl, &ctrl->admin_q, 0, NVME_ADMIN_QUEUE_SIZE) != 0)
        return -1;

    /* AQA: ACQS in [27:16], ASQS in [11:0].  Both are 0-based, so a
     * 64-entry queue is encoded as 63 = 0x3F. */
    u32 aqa = ((u32)(NVME_ADMIN_QUEUE_SIZE - 1) << 16) |
              ((u32)(NVME_ADMIN_QUEUE_SIZE - 1));
    mmio_write32(aqa, bar + NVME_REG_AQA);

    /* On emu68 PiStorm the device sees the same physical address the
     * CPU sees, so the SQ/CQ pointers go straight into ASQ/ACQ.  Low
     * 12 bits MUST be zero — dma_alloc with page alignment guarantees
     * that. */
    u64 asq_addr = (u64)(ULONG)ctrl->admin_q.sq;
    u64 acq_addr = (u64)(ULONG)ctrl->admin_q.cq;
    mmio_write32((u32)asq_addr, bar + NVME_REG_ASQ);
    mmio_write32((u32)(asq_addr >> 32), bar + NVME_REG_ASQ + 4);
    mmio_write32((u32)acq_addr, bar + NVME_REG_ACQ);
    mmio_write32((u32)(acq_addr >> 32), bar + NVME_REG_ACQ + 4);

    return 0;
}

/*
 * I/O-queue bring-up SQE builders — shared by the synchronous probe path
 * (nvme_setup_io_queue) and the asynchronous reset path
 * (nvme_reset_rebuild_io_async) so the two encodings cannot drift.
 */

/* Set Features (Number of Queues): dword11 bits[15:0]=NSQR, [31:16]=NCQR,
 * both 0-based — request exactly 1 SQ + 1 CQ. */
static void nvme_build_set_num_queues(struct nvme_command *cmd)
{
    mem_zero(cmd, sizeof(*cmd));
    cmd->features.opcode = nvme_admin_set_features;
    cmd->features.fid = le32(NVME_FEAT_NUM_QUEUES);
    cmd->features.dword11 = le32(0);
}

/* Create I/O CQ pointing at the just-allocated ctrl->io_q.cq, IRQ vector 0. */
static void nvme_build_create_cq(struct NVMeController *ctrl, struct nvme_command *cmd)
{
    mem_zero(cmd, sizeof(*cmd));
    cmd->create_cq.opcode = nvme_admin_create_cq;
    cmd->create_cq.prp1 = le64((u64)(ULONG)ctrl->io_q.cq);
    cmd->create_cq.cqid = le16(NVME_IO_QID);
    cmd->create_cq.qsize = le16(NVME_IO_QUEUE_SIZE - 1);
    cmd->create_cq.cq_flags = le16(NVME_QUEUE_PHYS_CONTIG | NVME_CQ_IRQ_ENABLED);
    cmd->create_cq.irq_vector = le16(0);
}

/* Create I/O SQ pointing at ctrl->io_q.sq and the I/O CQ just created.
 * NVME_QUIRK_MEDIUM_PRIO_SQ: some drives (Intel 600p / P3100) auto-enable
 * weighted-round-robin internally unless the SQ priority is MEDIUM; since
 * URGENT encodes as zero, leaving it default makes every queue URGENT.
 * Setting QPRIO=MEDIUM works around the bug regardless of host CC.AMS. */
static void nvme_build_create_sq(struct NVMeController *ctrl, struct nvme_command *cmd)
{
    const u16 sq_prio = (ctrl->quirks & NVME_QUIRK_MEDIUM_PRIO_SQ)
                            ? NVME_SQ_PRIO_MEDIUM
                            : NVME_SQ_PRIO_URGENT;
    mem_zero(cmd, sizeof(*cmd));
    cmd->create_sq.opcode = nvme_admin_create_sq;
    cmd->create_sq.prp1 = le64((u64)(ULONG)ctrl->io_q.sq);
    cmd->create_sq.sqid = le16(NVME_IO_QID);
    cmd->create_sq.qsize = le16(NVME_IO_QUEUE_SIZE - 1);
    cmd->create_sq.sq_flags = le16(NVME_QUEUE_PHYS_CONTIG | sq_prio);
    cmd->create_sq.cqid = le16(NVME_IO_QID);
}

/* Cache the in-flight ceiling once io_q.depth is known; read directly as
 * ctrl->io_max_inflight on every dispatch / back-pressure check. */
static void nvme_set_io_max_inflight(struct NVMeController *ctrl)
{
    ctrl->io_max_inflight = (ctrl->quirks & NVME_QUIRK_QDEPTH_ONE)
                                ? 1
                                : (ctrl->io_q.depth > 1 ? (u16)(ctrl->io_q.depth - 1) : 1);
}

/*
 * nvme_setup_io_queue - negotiate, allocate, and create one I/O queue pair.
 *
 * Three-step admin sequence:
 *   1. Set Features (Number of Queues) — request 1 SQ + 1 CQ.  The
 *      controller returns the granted count in the CQE result.
 *   2. Allocate the I/O CQ + SQ rings via nvme_setup_queue.
 *   3. Submit Create I/O CQ + Create I/O SQ admin commands pointing at
 *      the now-allocated rings.
 *
 * Step 2 publishes ctrl->io_q before the Create CQ/SQ commands run.
 * That's safe: drain_cq sees cq_phase=1 vs zero-initialised slots
 * (phase=0) and exits immediately — so a stray admin CQE arrival
 * cannot mistake an empty io_cq for completed work.
 *
 * Preconditions:
 *   - nvme_setup_admin_queue() and nvme_enable_ctrl() have already run
 *     (admin queue is live, controller is CSTS.RDY).
 */
s32 nvme_setup_io_queue(struct NVMeController *ctrl)
{
    struct nvme_command cmd;
    union nvme_result result;
    int ret;

    KprintfH("[nvme] setup_io_queue: ctrl=%lx\n", (ULONG)ctrl);

    /* Step 1: ask for 1 I/O SQ and 1 I/O CQ. */
    nvme_build_set_num_queues(&cmd);
    ret = nvme_submit_sync_cmd(ctrl, &cmd, &result, NULL, 0);
    if (ret)
    {
        Kprintf("[nvme] %s: Set Features (Num Queues) failed: %ld\n", __func__, ret);
        return -1;
    }
    /* Result dword0: NSQA in [15:0], NCQA in [31:16].  Both are
     * 0-based, so 0 means "1 queue granted". */
#ifdef DEBUG_HIGH
    u32 num_queues = le32(result.u32);
    KprintfH("[nvme] %s: granted NSQA=%lu NCQA=%lu (both 0-based; we need 1+1)\n",
             __func__, num_queues & 0xFFFF, num_queues >> 16);
#endif

    /* Step 2: allocate rings + inflight table for the I/O queue. */
    if (nvme_setup_queue(ctrl, &ctrl->io_q, NVME_IO_QID, NVME_IO_QUEUE_SIZE) != 0)
        return -1;

    /* Step 3a: Create I/O CQ. */
    nvme_build_create_cq(ctrl, &cmd);
    ret = nvme_submit_sync_cmd(ctrl, &cmd, NULL, NULL, 0);
    if (ret)
    {
        Kprintf("[nvme] %s: Create I/O CQ failed: %ld\n", __func__, ret);
        goto fail;
    }

    /* Step 3b: Create I/O SQ pointing at the CQ just created. */
    nvme_build_create_sq(ctrl, &cmd);
    ret = nvme_submit_sync_cmd(ctrl, &cmd, NULL, NULL, 0);
    if (ret)
    {
        Kprintf("[nvme] %s: Create I/O SQ failed: %ld\n", __func__, ret);
        goto fail;
    }

    nvme_set_io_max_inflight(ctrl);

    KprintfH("[nvme] %s: I/O queue pair (QID=%lu) ready, SQ@%lx CQ@%lx\n",
             __func__, (ULONG)NVME_IO_QID,
             (ULONG)ctrl->io_q.sq, (ULONG)ctrl->io_q.cq);
    return 0;

fail:
    nvme_teardown_queue(&ctrl->io_q);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Asynchronous I/O-queue bring-up (controller-reset path).            */
/*                                                                     */
/* nvme_reset_controller() runs on the unit task — the same task that  */
/* drains the CQ — so it cannot use nvme_submit_sync_cmd (Wait()ing on */
/* a completion only it can deliver would deadlock).  Instead the      */
/* bring-up is a short async chain: each step submits via              */
/* nvme_submit_async_cmd and its completion callback (fired from the   */
/* unit task's own drain_cq) launches the next.  The unit task never   */
/* blocks; the terminal step calls nvme_reset_finish() to drive the    */
/* LIVE/scan transition (or DEAD on failure).  ctrl is threaded        */
/* through as the async 'priv'.                                        */
/* ------------------------------------------------------------------ */

/* done() for Create I/O SQ — the last step: cache the depth, go LIVE. */
static void nvme_io_setup_async_sq(struct nvme_request *req)
{
    struct NVMeController *ctrl = (struct NVMeController *)req->priv;
    int status = (int)req->status;
    slab_free(&req->ac->req_slab, req);

    if (status)
    {
        Kprintf("[nvme] reset: Create I/O SQ failed: %ld\n", (LONG)status);
        nvme_teardown_queue(&ctrl->io_q);
        nvme_reset_finish(ctrl, FALSE);
        return;
    }

    nvme_set_io_max_inflight(ctrl);
    KprintfH("[nvme] reset: I/O queue pair (QID=%lu) ready (async)\n",
             (ULONG)NVME_IO_QID);
    nvme_reset_finish(ctrl, TRUE);
}

/* done() for Create I/O CQ — submit Create I/O SQ. */
static void nvme_io_setup_async_cq(struct nvme_request *req)
{
    struct NVMeController *ctrl = (struct NVMeController *)req->priv;
    int status = (int)req->status;
    slab_free(&req->ac->req_slab, req);

    if (status)
    {
        Kprintf("[nvme] reset: Create I/O CQ failed: %ld\n", (LONG)status);
        nvme_teardown_queue(&ctrl->io_q);
        nvme_reset_finish(ctrl, FALSE);
        return;
    }

    struct nvme_command cmd;
    nvme_build_create_sq(ctrl, &cmd);
    if (nvme_submit_async_cmd(ctrl, &cmd, NULL, 0,
                              nvme_io_setup_async_sq, ctrl, 0) != 0)
    {
        Kprintf("[nvme] reset: Create I/O SQ submit failed\n");
        nvme_teardown_queue(&ctrl->io_q);
        nvme_reset_finish(ctrl, FALSE);
    }
}

/* done() for Set Features (Num Queues) — allocate rings, submit Create CQ. */
static void nvme_io_setup_async_features(struct nvme_request *req)
{
    struct NVMeController *ctrl = (struct NVMeController *)req->priv;
    int status = (int)req->status;
    slab_free(&req->ac->req_slab, req);

    if (status)
    {
        Kprintf("[nvme] reset: Set Features (Num Queues) failed: %ld\n", (LONG)status);
        nvme_reset_finish(ctrl, FALSE);
        return;
    }

    if (nvme_setup_queue(ctrl, &ctrl->io_q, NVME_IO_QID, NVME_IO_QUEUE_SIZE) != 0)
    {
        Kprintf("[nvme] reset: I/O queue alloc failed\n");
        nvme_reset_finish(ctrl, FALSE);
        return;
    }

    struct nvme_command cmd;
    nvme_build_create_cq(ctrl, &cmd);
    if (nvme_submit_async_cmd(ctrl, &cmd, NULL, 0,
                              nvme_io_setup_async_cq, ctrl, 0) != 0)
    {
        Kprintf("[nvme] reset: Create I/O CQ submit failed\n");
        nvme_teardown_queue(&ctrl->io_q);
        nvme_reset_finish(ctrl, FALSE);
    }
}

/*
 * nvme_reset_rebuild_io_async - kick off the async I/O-queue bring-up.
 *
 * Called by nvme_reset_controller after the (synchronous) admin-queue
 * rebuild.  Submits the first command and returns immediately; the unit
 * task's drain loop carries the chain forward.  On a submit failure (or
 * any step's failure) nvme_reset_finish(ctrl, FALSE) takes the controller
 * to DEAD.
 */
void nvme_reset_rebuild_io_async(struct NVMeController *ctrl)
{
    struct nvme_command cmd;

    KprintfH("[nvme] reset: starting async I/O-queue bring-up\n");
    nvme_build_set_num_queues(&cmd);
    if (nvme_submit_async_cmd(ctrl, &cmd, NULL, 0,
                              nvme_io_setup_async_features, ctrl, 0) != 0)
    {
        Kprintf("[nvme] reset: Set Features (Num Queues) submit failed\n");
        nvme_reset_finish(ctrl, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* Cancel walkers + freeze/quiesce gates                              */
/* ------------------------------------------------------------------ */

/*
 * nvme_cancel_request - force-complete a single in-flight request.
 *
 * Marks @req NVME_SC_HOST_ABORTED_CMD + NVME_REQ_CANCELLED and routes it
 * through nvme_complete_rq so any blocked waiter / IOStdReq unblocks
 * without waiting for a real CQE.  Used both directly and as the
 * per-slot action inside nvme_flush_queue_inflight().
 */
void nvme_cancel_request(struct nvme_request *req)
{
    KprintfH("[nvme] cancel: cid 0x%lx\n", (ULONG)req->cid);
    req->status = NVME_SC_HOST_ABORTED_CMD;
    req->flags |= NVME_REQ_CANCELLED;
    nvme_complete_rq(req);
}

/*
 * nvme_flush_queue_inflight - cancel every in-flight request on @q.
 *
 * Walks the full inflight[] table releasing each slot (which keeps
 * inflight_count / aer_inflight accurate) and force-completing the
 * request via nvme_cancel_request.  Called during controller reset
 * and unprobe to drain a queue before its rings are torn down.
 */
void nvme_flush_queue_inflight(struct nvme_queue *q)
{
    if (!q || !q->inflight)
        return;
    for (u16 i = 0; i < q->depth; i++)
    {
        struct nvme_request *req = q->inflight[i];
        if (req)
        {
            nvme_inflight_release(q, req->cid);
            nvme_cancel_request(req);
        }
    }
}

/*
 * nvme_start_freeze - hold off new I/O dispatch for an exclusive command.
 *
 * Linux freezes per-namespace block-mq queues so new requests sit at the
 * block layer.  Amiga has no block layer; the equivalent gate is the
 * unit task's msgPort drain (see unit_task.c) which skips when
 * NVME_CTRL_FROZEN is set.  In-flight commands on the SQ continue.
 *
 * Pair with nvme_wait_freeze() to block until in-flight drains, then
 * with nvme_unfreeze() to release.
 */
void nvme_start_freeze(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_set_bit(NVME_CTRL_FROZEN, &ctrl->flags))
        Kprintf("[nvme] %s: I/O queues freezing\n", __func__);
}

/*
 * nvme_wait_freeze - block until the I/O queue has drained.
 *
 * Admin queue is intentionally not waited on: passthrough's own admin
 * command issues there while frozen, and the persistent AER would
 * otherwise prevent ever reaching zero.
 *
 * MUST NOT be called from ctrl->unit_task — would deadlock waiting for
 * completions that only the unit task processes.  Passthrough runs on
 * AdminWorker (scan-worker model), so polling via delay_ms() is safe.
 */
void nvme_wait_freeze(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    while (ctrl->io_q.inflight_count > 0)
        delay_ms(10);
}

/*
 * nvme_unfreeze - release the freeze gate and kick the unit task to
 * drain any IOStdReqs that piled up on msgPort during the freeze.
 */
void nvme_unfreeze(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_clear_bit(NVME_CTRL_FROZEN, &ctrl->flags))
        return;
    Kprintf("[nvme] %s: I/O queues unfrozen\n", __func__);
    if (ctrl->unit_task)
        Signal(ctrl->unit_task, 1UL << ctrl->msgPort->mp_SigBit);
}

/*
 * nvme_quiesce_io_queues - stop dispatching new I/O to the controller.
 *
 * Amiga has no blk-mq tagset; the moral equivalent is to stop draining
 * ctrl->msgPort in the unit task's wait loop.  Incoming BeginIO traffic
 * sits in the message port until nvme_unquiesce_io_queues releases it.
 * In-flight commands (already on the SQ) are not disturbed — they
 * continue to complete via the normal CQE path.
 *
 * Idempotent: a second call while already quiesced is a no-op.
 */
void nvme_quiesce_io_queues(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_set_bit(NVME_CTRL_STOPPED, &ctrl->flags))
        Kprintf("[nvme] %s: I/O queues quiesced\n", __func__);
}

/*
 * nvme_unquiesce_io_queues - resume I/O dispatch after a quiesce.
 *
 * Clears NVME_CTRL_STOPPED and self-signals the unit task on the
 * msgPort signal bit so the wait loop drains any messages that
 * accumulated during the quiesce (the Exec msgPort signal would
 * normally only fire on a fresh PutMsg).
 *
 * No-op if not currently quiesced.
 */
void nvme_unquiesce_io_queues(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_clear_bit(NVME_CTRL_STOPPED, &ctrl->flags))
        return;
    Kprintf("[nvme] %s: I/O queues unquiesced\n", __func__);

    if (ctrl->unit_task)
        Signal(ctrl->unit_task, 1UL << ctrl->msgPort->mp_SigBit);
}

/*
 * nvme_quiesce_admin_queue - stop draining ctrl->adminPort.
 *
 * Mirror of nvme_quiesce_io_queues for the admin path: sets
 * NVME_CTRL_ADMIN_Q_STOPPED so AdminWorker leaves new passthrough
 * IOCTLs queued in adminPort instead of dispatching them.  Internal
 * async admin submits (AER re-arm, watchdog-issued Aborts) keep
 * working — those are called from controller-owned contexts that
 * are already serialised against reset by running on unit_task.
 *
 * Idempotent.
 */
void nvme_quiesce_admin_queue(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_set_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
        Kprintf("[nvme] %s: admin queue quiesced\n", __func__);
}

/*
 * nvme_unquiesce_admin_queue - resume admin port dispatch.
 *
 * Clears NVME_CTRL_ADMIN_Q_STOPPED and self-signals AdminWorker on
 * the adminPort signal bit so the wait loop drains any IOCTLs that
 * accumulated during the quiesce.  No-op if not currently quiesced.
 */
void nvme_unquiesce_admin_queue(struct NVMeController *ctrl)
{
    if (!ctrl)
        return;
    if (!test_and_clear_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
        return;
    Kprintf("[nvme] %s: admin queue unquiesced\n", __func__);
    if (ctrl->admin_task && ctrl->adminPort)
        Signal(ctrl->admin_task, 1UL << ctrl->adminPort->mp_SigBit);
}
