// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_ADMIN_H
#define NVME_ADMIN_H

#include <nvme/nvme_core.h>     /* size_t, nvme_defs.h (struct nvme_command, union nvme_result) */

struct NVMeController;
struct nvme_request;

/*
 * nvme_io_abort - submit Abort admin cmd(s) for an in-flight I/O.
 *
 * Walks ctrl->io_q.inflight[] for every nvme_request whose originating
 * IOStdReq matches @target — single-shot commands hold it on req->io,
 * chunked I/Os hold it on req->ctx->io.  One Abort SQE per matching
 * sibling; the controller doesn't cascade aborts across tags.
 *
 * Async (the only safe choice here — see nvme_submit_sync_cmd).
 * Returns 0 if at least one Abort was submitted, -1 otherwise (target
 * not in-flight, NULL args, or every submission failed).
 */
int nvme_io_abort(struct NVMeController *ctrl, struct IOStdReq *target);

/*
 * nvme_abort_request - submit one Abort SQE for an in-flight request.
 *
 * Shared low-level entry point: nvme_io_abort calls it per matching
 * sibling, and the queue watchdog calls it on timeout escalation.  The
 * target's NVME_SC_ABORT_REQ CQE arrives via the normal completion
 * path; this never blocks.  Returns 0 on submit, negative errno on
 * failure.
 */
int nvme_abort_request(struct NVMeController *ctrl, struct nvme_request *req);

/*
 * nvme_submit_sync_cmd - submit an admin command and block until completion.
 *
 * Parks the calling task on a fresh signal bit until drain_cq writes
 * the CQE into @req and Signal()s us.  @result is optional — pass NULL
 * if you don't need CQE Result DW0.
 *
 * WARNING — calling-task invariant:
 *   drain_cq runs inside ctrl->unit_task.  Calling this from
 *   unit_task (ProcessCommand, watchdog, IRQ-signal handlers) parks
 *   the task on a signal only itself can deliver — deadlock.
 *
 *   Safe contexts:
 *     - ctrl->admin_task (AdminWorker): NVMe passthrough, AEN-driven
 *       rescan, fw_act polling.
 *     - One-shot probe / bring-up before either task exists.
 *
 *   Anything reached from unit_task MUST use nvme_submit_async_cmd.
 */
int nvme_submit_sync_cmd(struct NVMeController *ctrl, struct nvme_command *cmd,
			 union nvme_result *result, void *buffer, u32 buflen);

/*
 * nvme_submit_async_cmd - submit an admin command without blocking.
 *
 * Stores @done on the request; it runs from drain_cq → nvme_end_req
 * with the CQE already latched in req->status / req->result.  The
 * callback OWNS the request — it must pool_free(req->ac->memoryPool,
 * req) and release any data buffer.
 *
 * @priv is opaque, recovered as req->priv (typically the data buffer
 * pointer so the callback can free it).  May be NULL.
 *
 * @req_flags is OR'd into req->flags before submit (e.g. NVME_REQ_AER
 * for long-lived AERs the watchdog must skip).  Pass 0 otherwise.
 *
 * Safe from any task.  Required for unit_task contexts and for
 * long-lived AERs.  Returns 0 on submit (@done will fire exactly
 * once); negative errno on submit failure (@done is NOT called; the
 * request is destroyed internally).
 */
int nvme_submit_async_cmd(struct NVMeController *ctrl, struct nvme_command *cmd,
			  void *buffer, u32 buflen,
			  void (*done)(struct nvme_request *req), void *priv,
			  unsigned int req_flags);

/*
 * nvme_set_features - issue a Set Features admin command.
 * @result (u32 *, may be NULL) receives CDW0 from the CQE in host
 * byte order on success.
 */
int nvme_set_features(struct NVMeController *dev, unsigned int fid,
		unsigned int dword11, void *buffer, size_t buflen,
		void *result);

/*
 * Probe-time feature configurators called from nvme_init_ctrl_finish().
 */
int nvme_configure_timestamp(struct NVMeController *ctrl);
int nvme_configure_host_options(struct NVMeController *ctrl);
int nvme_configure_irq_coalesce(struct NVMeController *ctrl);

/*
 * nvme_get_log - issue a Get Log Page command, synchronously or asynchronously.
 *
 * @done == NULL submits synchronously (result in @log on return, caller owns
 * @log; must not be called from the admin/unit task).  @done != NULL submits
 * asynchronously: @done fires from the completion drain, owns the request, and
 * must free @log; @priv is recovered as req->priv.  Safe from any task.
 */
int nvme_get_log(struct NVMeController *ctrl, u32 nsid, u8 log_page,
		u8 lsp, u8 csi, void *log, size_t size, u64 offset,
		void (*done)(struct nvme_request *), void *priv);

#endif /* NVME_ADMIN_H */
