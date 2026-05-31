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

    /* SQ ring (page-aligned, dma_alloc'd from ctrl->memoryPool). */
    struct nvme_command    *sq;
    u16                     sq_tail;
    u32                     sq_db_off;  /* BAR0 offset of SQ-tail doorbell   */

    /* CQ ring (page-aligned, dma_alloc'd from ctrl->memoryPool). */
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

    /* CID free-list — pool_zalloc'd u16 array of size `depth`.
     * nvme_alloc_tag pops from free_stack[--free_top];
     * nvme_inflight_release pushes via free_stack[free_top++] = cid.
     * Initialised at setup with every CID 0..depth-1 in reverse so the
     * first pop returns CID 0. */
    u16                    *free_stack;
    u16                     free_top;
};

/*
 * Queue lifecycle.
 */
s32  nvme_setup_admin_queue(struct NVMeController *ctrl);
s32  nvme_setup_io_queue(struct NVMeController *ctrl);
void nvme_teardown_queue(struct nvme_queue *q);

/*
 * Completion drain and watchdog tick — both called from the unit task's
 * Wait()-loop.
 */
void nvme_process_completions(struct NVMeController *ctrl);
void nvme_tick_watchdog(struct NVMeController *ctrl);

/*
 * Inflight slot accounting + CID free-list — wraps every write to
 * q->inflight[cid] and q->free_stack so the per-queue inflight_count
 * stays accurate without scanning and CID allocation is O(1).
 *
 * claim   - write @req into inflight[@cid]; bump inflight_count if the
 *           slot was previously NULL (re-writes by submit-retry are
 *           defensively idempotent).
 * release - clear inflight[@cid]; decrement inflight_count and push
 *           @cid back to the free stack so nvme_alloc_tag can hand it
 *           out again.  Idempotent if the slot was already NULL.
 *
 * alloc_tag - pop a CID off the free stack (O(1)); returns 0xFFFF when
 *             the queue is fully busy.
 */
static inline void nvme_inflight_claim(struct nvme_queue *q, u16 cid,
                                       struct nvme_request *req)
{
    if (!q->inflight[cid])
        q->inflight_count++;
    q->inflight[cid] = req;
}

static inline void nvme_inflight_release(struct nvme_queue *q, u16 cid)
{
    if (!q->inflight[cid])
        return;
    q->inflight[cid] = NULL;
    q->inflight_count--;
    q->free_stack[q->free_top++] = cid;
}

static inline u16 nvme_alloc_tag(struct nvme_queue *q)
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
