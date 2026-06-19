// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H

#include <nvme/nvme_core.h>     /* foundation types; struct nvme_command/completion */

struct NVMeController;
struct nvme_request;

#define NVME_IO_TIMEOUT	(30000) // 30s timeout for I/O commands
#define NVME_ADMIN_TIMEOUT	(60000) // 60s timeout for admin commands
#define NVME_ABORT_TIMEOUT	(15000) // 15s grace for Abort to settle before reset

/*
 * NVMe SQ + CQ ring pair.  One instance per queue ID on the controller —
 * admin (qid 0) and I/O (qid 1).  Holds the ring memory, the tail/head
 * pointers, the per-queue phase bit, the doorbell offsets pre-computed
 * from db_stride at setup time, and the per-queue in-flight table.
 *
 * Per-queue CIDs (a CQE on this queue carries a tag in [0, depth)) live
 * in inflight[], indexed by local CID.  CIDs are NOT unique across
 * queues — admin tag 5 and I/O tag 5 are entirely separate slots.
 */
struct nvme_queue
{
    struct NVMeController  *ctrl;       /* back-pointer to owning controller */
    u16                     qid;        /* 0 = admin, 1 = I/O                */
    u16                     depth;      /* number of ring slots              */
    BOOL                    sqe_128b;   /* TRUE when this queue uses 128-byte
                                         * SQ slots under
                                         * NVME_QUIRK_128_BYTES_SQES (Apple).
                                         * The command is always 64 B; only the
                                         * slot-to-slot stride changes.       */
    BOOL                    skip_cid_gen; /* cached NVME_QUIRK_SKIP_CID_GEN:
                                         * command_id == tag, no generation
                                         * bits.  Set at setup; read by
                                         * nvme_inflight_release.            */

    /* SQ ring (page-aligned, dma_alloc'd from ctrl->dmaPool). */
    struct nvme_command    *sq;
    u16                     sq_tail;
    u16                     last_sq_tail;   /* sq_tail at the last doorbell write;
                                             * the SQ-tail doorbell only needs the
                                             * newest tail, so a run of SQEs costs
                                             * one doorbell (Linux last_sq_tail).  */
    u16                     sq_batch_depth; /* >0 → nvme_submit_io defers the
                                             * doorbell; the outermost
                                             * nvme_sq_batch_end commits it once.  */
    u32                     sq_db_off;  /* BAR0 offset of SQ-tail doorbell   */

    /* CQ ring (page-aligned, dma_alloc'd from ctrl->dmaPool). */
    struct nvme_completion *cq;
    u16                     cq_head;
    u8                      cq_phase;   /* expected phase bit (1 → 0 → 1 …)  */
    u32                     cq_db_off;  /* BAR0 offset of CQ-head doorbell   */

    /* pool_zalloc'd array of `depth` pointers.  inflight[cid] is the
     * in-flight request whose CQE will land at this CQ position, or
     * NULL if the slot is free. */
    struct nvme_request   **inflight;

    /* Non-NULL slots in inflight[]. Maintained by nvme_inflight_claim /
     * nvme_inflight_release on every slot write so the freeze/wait
     * helpers don't need to scan. */
    u16                     inflight_count;

    /* CID free-list — pool_zalloc'd u16 array of size `depth`, holding
     * ENCODED CIDs (gen<<12 | tag), not bare tags.  nvme_alloc_cid pops
     * from free_stack[--free_top]; nvme_inflight_release bumps the slot's
     * generation and pushes the next encoded CID via free_stack[free_top++].
     * The generation thus rides in the free entry while the slot is idle and
     * in inflight[tag]->cid while in flight — no separate gen array needed.
     * Initialised at setup with every CID 0..depth-1 (gen 0) in reverse so
     * the first pop returns CID 0. */
    u16                    *free_stack;
    u16                     free_top;
};

/*
 * Queue lifecycle.
 */
s32  nvme_setup_admin_queue(struct NVMeController *ctrl);
s32  nvme_setup_io_queue(struct NVMeController *ctrl);
/* Asynchronous I/O-queue bring-up for the controller-reset path: runs on
 * the unit task without blocking it (see nvme_queue.c).  Terminal step
 * calls nvme_reset_finish(). */
void nvme_reset_rebuild_io_async(struct NVMeController *ctrl);
void nvme_teardown_queue(struct nvme_queue *q);

/*
 * Completion drain and watchdog tick — both called from the unit task's
 * Wait()-loop.
 */
void nvme_process_completions(struct NVMeController *ctrl);
void nvme_tick_watchdog(struct NVMeController *ctrl);

/*
 * SQ-tail doorbell batching (mirrors Linux nvme_write_sq_db / commit_rqs).
 *
 * nvme_submit_io rings the doorbell immediately when sq_batch_depth == 0.
 * A caller that submits several commands in one unit-task pass brackets the
 * run with nvme_sq_batch_begin/_end so the doorbell is written once for the
 * whole batch instead of once per command.  The depth counter nests safely:
 * an inner batch (e.g. the chunk pump running inside the msgport drain) only
 * commits when the outermost _end unwinds to depth 0.
 */
void nvme_sq_batch_begin(struct nvme_queue *q);
void nvme_sq_batch_end(struct nvme_queue *q);

/*
 * Inflight slot accounting + CID free-list — wraps every write to
 * q->inflight[] and q->free_stack so the per-queue inflight_count stays
 * accurate without scanning and CID allocation is O(1).  All three take the
 * ENCODED CID (gen<<12 | tag) and derive the slot index as cid & 0x0fff.
 *
 * claim   - write @req into inflight[tag]; bump inflight_count if the slot
 *           was previously NULL (re-writes by submit-retry are defensively
 *           idempotent).
 * release - clear inflight[tag]; decrement inflight_count and push the slot's
 *           NEXT generation (encoded CID) back to the free stack so
 *           nvme_alloc_cid hands out a fresh gen next time.  Idempotent if
 *           the slot was already NULL.
 *
 * alloc_cid - pop an encoded CID off the free stack (O(1)); returns 0xFFFF
 *             when the queue is fully busy (a real CID never reaches 0xFFFF:
 *             max is 0xF<<12 | (depth-1) and depth <= 256).
 */
static inline void nvme_inflight_claim(struct nvme_queue *q, u16 cid,
                                       struct nvme_request *req)
{
    u16 tag = cid & 0x0fff;
    if (!q->inflight[tag])
        q->inflight_count++;
    q->inflight[tag] = req;
}

static inline void nvme_inflight_release(struct nvme_queue *q, u16 cid)
{
    u16 tag = cid & 0x0fff;
    if (!q->inflight[tag])
        return;
    q->inflight[tag] = NULL;
    q->inflight_count--;
    /* Bump the generation for the next reuse and stash it in the free entry.
     * Skip under SKIP_CID_GEN: those controllers need command_id == tag.
     * (q->skip_cid_gen is cached at setup — struct NVMeController is only
     * forward-declared here, so we can't read ctrl->quirks directly.) */
    u16 next = q->skip_cid_gen
                   ? tag
                   : (u16)(((((cid >> 12) + 1) & 0x0f) << 12) | tag);
    q->free_stack[q->free_top++] = next;
}

static inline u16 nvme_alloc_cid(struct nvme_queue *q)
{
    if (q->free_top == 0)
        return 0xFFFF;
    return q->free_stack[--q->free_top];
}

/*
 * Cancel walkers — drain in-flight requests on reset / delete.
 *   nvme_cancel_request       — force-complete a single request.
 *   nvme_flush_queue_inflight — force-complete every inflight on @q.
 */
void nvme_cancel_request(struct nvme_request *req);
void nvme_flush_queue_inflight(struct nvme_queue *q);

/*
 * Freeze / quiesce gates.  Amiga-side analogues of the Linux blk-mq APIs;
 * gate the unit task's msgPort drain (io) and the admin task's adminPort
 * drain (admin) via NVME_CTRL_{FROZEN,STOPPED,ADMIN_Q_STOPPED} flags.
 */
void nvme_quiesce_io_queues(struct NVMeController *ctrl);
void nvme_unquiesce_io_queues(struct NVMeController *ctrl);
void nvme_quiesce_admin_queue(struct NVMeController *ctrl);
void nvme_unquiesce_admin_queue(struct NVMeController *ctrl);
void nvme_unfreeze(struct NVMeController *ctrl);
void nvme_wait_freeze(struct NVMeController *ctrl);
void nvme_start_freeze(struct NVMeController *ctrl);

#endif /* NVME_QUEUE_H */
