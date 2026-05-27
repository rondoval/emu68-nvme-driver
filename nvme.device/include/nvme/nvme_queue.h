// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_QUEUE_H
#define NVME_QUEUE_H

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
