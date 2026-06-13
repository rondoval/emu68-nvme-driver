// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_completion.c — per-request state machine and completion delivery.
 *
 * Holds the error-status mapping, retry policy / disposition, retry
 * execution (including the CRDT park-on-retry_list), end_req delivery
 * (reply io / signal waiter / fire async done), and the complete_rq
 * conductor.
 */
#include <nvme/nvme_core.h>
#include <nvme/nvme_ctrl.h> /* struct NVMeController */
#include <device.h>			/* struct NVMeUnit (req->unit) */

#include <exec/errors.h>
#include <devices/trackdisk.h>

#include <timing.h> /* get_time */

#include <nvme/nvme_io.h> /* nvme_cleanup_cmd, nvme_req_destroy, NVME_IO_ASYNC,
                                   * nvme_io_context_pump, nvme_io_context_finish */
#include <nvme/nvme_completion.h>

/* max number of retries a command may have */
#define NVME_MAX_RETRIES 5U

/*
 * nvme_error_status - map an NVMe completion status code to a amigaos error code
 *
 * Translates the NVMe Status Code Type + Status Code into the AmigaOS error
 * value that gets stored in io_Error.  Called from nvme_end_req() for every
 * completed I/O command.
 *
 * @status: 16-bit NVMe status field (DNR/More bits are included but only
 *          the SCT+SC bits are examined)
 * Returns: a s32 value (IOERR_NO_ERROR, IOERR_BADADDRESS, IOERR_NOCMD, …)
 */
static BYTE nvme_error_status(u16 status)
{
	switch (status & NVME_SCT_SC_MASK)
	{
	case NVME_SC_SUCCESS:
		return IOERR_NO_ERROR;
	case NVME_SC_INVALID_NS:
		return TDERR_BadUnitNum;
	case NVME_SC_CAP_EXCEEDED:
	case NVME_SC_LBA_RANGE:
		return IOERR_BADADDRESS;
	default:
		return TDERR_NotSpecified;
	}
}

/*
 * nvme_log_err_normal - print a rate-limited error message for a failed command
 *
 * Formats a human-readable log line that includes the disk name (or controller
 * name for admin commands), opcode string, LBA/length for I/O commands, and the
 * decoded status code.  Uses pr_err_ratelimited to avoid flooding the log on
 * repeated errors.  Called from nvme_end_req() for non-passthrough commands.
 *
 * @req: the failed request
 */
static void nvme_log_err_normal(struct nvme_request *req)
{
	if (req->unit)
	{
		u8 shift = (u8)req->unit->blockShift;
		u64 fail_lba;
		u32 fail_blks = req->user_len >> shift;

		/* For chunked siblings the parent context knows where this
		 * chunk started in the original I/O.  For single-shot
		 * commands the SQE's slba is the source of truth. */
		if (req->ctx)
			fail_lba = req->ctx->start_lba +
					   (u64)(req->ctx->completed >> shift);
		else
			fail_lba = le64(req->cmd.rw.slba); /* self-inverse: LE->native */

		Kprintf("%.40s: %s(0x%lx) @ LBA %08lx%08lx, %lu blocks, %s (sct 0x%lx / sc 0x%lx) %s%s\n",
				req->ac->id_strings.model,
				nvme_get_opcode_str(req->cmd.common.opcode),
				req->cmd.common.opcode,
				u64_hi32(fail_lba), u64_lo32(fail_lba),
				fail_blks,
				nvme_get_error_status_str(req->status),
				NVME_SCT(req->status),		/* Status Code Type */
				req->status & NVME_SC_MASK, /* Status Code */
				req->status & NVME_STATUS_MORE ? "MORE " : "",
				req->status & NVME_STATUS_DNR ? "DNR " : "");
		return;
	}

	Kprintf("%s: %s(0x%lx), %s (sct 0x%lx / sc 0x%lx) %s%s\n",
			"(admin)",
			nvme_get_admin_opcode_str(req->cmd.common.opcode),
			req->cmd.common.opcode,
			nvme_get_error_status_str(req->status),
			NVME_SCT(req->status),		/* Status Code Type */
			req->status & NVME_SC_MASK, /* Status Code */
			req->status & NVME_STATUS_MORE ? "MORE " : "",
			req->status & NVME_STATUS_DNR ? "DNR " : "");
}

/*
 * nvme_log_err_passthru - print extended error info for a failed passthrough cmd
 *
 * Like nvme_log_err_normal() but additionally dumps the raw CDW10–CDW15 values,
 * which are meaningful for passthrough commands issued via ioctl or io_uring.
 * Rate-limited.  Called from nvme_end_req() when the request is a
 * passthrough command.
 *
 * @req: the failed passthrough request
 */
static void nvme_log_err_passthru(struct nvme_request *req)
{
	Kprintf(req->unit
				? "%.40s: %s(0x%lx), %s (sct 0x%lx / sc 0x%lx) %s%scdw10=0x%lx cdw11=0x%lx cdw12=0x%lx cdw13=0x%lx cdw14=0x%lx cdw15=0x%lx\n"
				: "%s: %s(0x%lx), %s (sct 0x%lx / sc 0x%lx) %s%scdw10=0x%lx cdw11=0x%lx cdw12=0x%lx cdw13=0x%lx cdw14=0x%lx cdw15=0x%lx\n",
			req->unit ? (const void *)req->ac->id_strings.model : "(admin)",
			req->unit ? nvme_get_opcode_str(req->cmd.common.opcode) : nvme_get_admin_opcode_str(req->cmd.common.opcode),
			req->cmd.common.opcode,
			nvme_get_error_status_str(req->status),
			NVME_SCT(req->status),		/* Status Code Type */
			req->status & NVME_SC_MASK, /* Status Code */
			req->status & NVME_STATUS_MORE ? "MORE " : "",
			req->status & NVME_STATUS_DNR ? "DNR " : "",
			le32(req->cmd.common.cdw10),
			le32(req->cmd.common.cdw11),
			le32(req->cmd.common.cdw12),
			le32(req->cmd.common.cdw13),
			le32(req->cmd.common.cdw14),
			le32(req->cmd.common.cdw15));
}

enum nvme_disposition
{
	COMPLETE,
	RETRY,
};

/*
 * nvme_decide_disposition - decide whether to complete or retry a request
 *
 * Returns COMPLETE if the request succeeded, must not be retried (DNR set,
 * max retries reached, or noretry/cancelled flag), or RETRY otherwise.
 * Called from nvme_complete_rq() after every command completion.
 *
 * @req: completed request
 * Returns: COMPLETE or RETRY
 */
static inline enum nvme_disposition nvme_decide_disposition(struct nvme_request *req)
{
	if (likely(req->status == 0))
		return COMPLETE;

	if (nvme_req_noretry(req) ||
		(req->status & NVME_STATUS_DNR) ||
		req->retries >= NVME_MAX_RETRIES)
		return COMPLETE;

	/* Non-transient generic errors: the controller rejected the
	 * command itself (Invalid Field, Invalid NSID, LBA out of
	 * Range, etc.).  These are status code type 0x0 (generic) with
	 * non-zero status code in the lower 0xFF range.  Retrying the
	 * same command guarantees the same error.  Linux's
	 * nvme_decide_disposition consults a per-status policy table;
	 * we approximate by treating all generic errors as DNR. */
	if ((req->status & NVME_SCT_MASK) == NVME_SCT_GENERIC &&
		(req->status & NVME_SC_MASK) != 0)
		return COMPLETE;

	return RETRY;
}

/*
 * nvme_retry_req - requeue a request for retry, honouring the CRD delay
 *
 * Increments the retry counter and re-rings the SQ doorbell with the same
 * SQE.  If the completion included a Command Retry Delay (CRD) hint, the
 * request is parked on ctrl->retry_list with a future deadline and
 * nvme_tick_watchdog (see nvme_queue.c) resubmits it once the deadline
 * passes — the park-and-resubmit is split between this function and the
 * watchdog tick.  Called from nvme_complete_rq() when
 * nvme_decide_disposition() returns RETRY; PRPs and any bounce buffer
 * are intentionally left intact so the resubmit hits valid DMA targets.
 *
 * @req: the request to requeue
 */
static void nvme_retry_req(struct nvme_request *req)
{
	u16 crd = (req->status & NVME_STATUS_CRD) >> 11;
	struct NVMeController *ac = req->ac;

	req->status = 0;
	req->retries++;

	/* CRDT (Command Retry Delay) is encoded in 100 ms units (Linux
	 * convention via NVME_STATUS_CRD: bits 12:11 select crdt[0..2]).
	 * Park the request on the controller's retry_list and let the
	 * watchdog tick re-submit it when the deadline passes. */
	if (crd && ac)
	{
		u32 delay_ms = ac->crdt[crd - 1] * 100U;
		req->deadline_us = (u32)get_time() + delay_ms * 1000U;
		AddTailMinList(&ac->retry_list, &req->node);
		return;
	}

	nvme_resubmit_io(req);
}

/*
 * nvme_log_error - log an error if the request completed with a non-zero
 * status.  Called from nvme_end_req on every completion before the io is
 * replied; selects the right log function depending on whether the request
 * is a passthrough command.
 *
 * @req: completed request
 */
static inline void nvme_log_error(struct nvme_request *req)
{
	if (unlikely(req->status))
	{
		if (nvme_req_is_passthrough(req))
			nvme_log_err_passthru(req);
		else
			nvme_log_err_normal(req);
	}
}

/*
 * nvme_end_req - log any error and deliver completion to the originating
 * caller.
 *
 * For sync admin commands this signals the waiter; for async admin it
 * invokes the done callback (which then owns @req); for I/O commands it
 * sets io_Error / io_Actual and ReplyMsg()s the originating IOStdReq.
 * Called by nvme_complete_rq's COMPLETE branch after
 * nvme_decide_disposition; pool_free is the caller's responsibility.
 *
 * @req: completed request
 */
static void nvme_end_req(struct nvme_request *req)
{
	BYTE error = nvme_error_status(req->status);

	KprintfH("[nvme] end_req: req=%lx cid=0x%lx status=0x%lx (%s) error=%ld done=%lx waiter=%lx io=%lx\n",
			 (ULONG)req, (ULONG)req->cid, (ULONG)req->status,
			 nvme_get_error_status_str(req->status),
			 (LONG)error, (ULONG)req->done,
			 (ULONG)req->waiter, (ULONG)req->io);

	nvme_log_error(req);
	if (req->done)
	{
		req->done(req);
		/* callback owns req from here — do NOT touch */
		return;
	}
	if (req->waiter)
	{
		Signal(req->waiter, 1UL << req->wait_signal);
	}
	else if (req->io)
	{
		if (!error)
			req->io->io_Actual = req->io->io_Length;
		req->io->io_Error = error;
		ReplyMsg((struct Message *)req->io);
	}
}

/*
 * nvme_complete_rq - main completion handler for NVMe I/O requests
 *
 * Two regimes:
 *
 *  - Single-shot (req->ctx == NULL): admin / flush / DSM / small I/O.
 *    Cleans up DMA, runs end_req (which replies the io / signals the
 *    waiter / fires the async done callback), then frees the request
 *    per the ownership table.
 *
 *  - Chunked sibling (req->ctx != NULL): one slice of a larger
 *    BeginIO.  Cleans up this sibling's DMA, decrements ctx->inflight,
 *    latches ctx->first_error on failure, frees the sibling, refills
 *    the dispatch slot via nvme_io_context_pump if more chunks remain,
 *    and lets the LAST completing sibling fire nvme_io_context_finish
 *    to reply the originating IOStdReq.
 *
 * The retry path leaves PRPs/bounce intact so nvme_submit_io can
 * re-ring the doorbell with the same SQE.
 *
 * @req: completed request
 */
void nvme_complete_rq(struct nvme_request *req)
{
	KprintfH("[nvme] complete_rq: req=%lx cid=0x%lx opcode=0x%02lx (%s) status=0x%lx (%s) unit=%lx ctx=%lx\n",
			 (ULONG)req, (ULONG)req->cid,
			 (ULONG)req->cmd.common.opcode,
			 req->unit ? nvme_get_opcode_str(req->cmd.common.opcode)
					   : nvme_get_admin_opcode_str(req->cmd.common.opcode),
			 (ULONG)req->status, nvme_get_error_status_str(req->status),
			 (ULONG)req->unit, (ULONG)req->ctx);

	switch (nvme_decide_disposition(req))
	{
	case COMPLETE:
		nvme_cleanup_cmd(req);

		if (req->ctx)
		{
			struct nvme_io_context *ctx = req->ctx;

			/* Latch first sibling error; log via the standard
			 * per-request path so the log entry has all the per-
			 * chunk detail. */
			if (req->status && ctx->first_error == 0)
				ctx->first_error = nvme_error_status(req->status);
			nvme_log_error(req);

			ctx->completed += req->user_len;
			ctx->inflight--;
			slab_free(&req->ac->req_slab, req);

			/* Refill the slot if there's more to dispatch and no
			 * sibling has failed yet — once first_error is latched
			 * we let the in-flight siblings drain without adding
			 * more work. */
			if (ctx->dispatched < ctx->total_bytes &&
				ctx->first_error == 0)
				nvme_io_context_pump(ctx);

			/* A pump under tag/SQ pressure with nothing left in
			 * flight parks the ctx (ctx->stalled) for the unit task
			 * to re-pump — it must not be finished here. */
			if (ctx->inflight == 0 && !ctx->stalled)
				nvme_io_context_finish(ctx);
			return;
		}

		/* Single-shot completion. */
		nvme_end_req(req);
		/* Ownership:
		 *  - Async admin (req->done != NULL): the callback already
		 *    ran inside nvme_end_req and owns req — we MUST NOT
		 *    touch it here (it may have been pool_free'd already).
		 *  - Sync admin (req->waiter != NULL): nvme_submit_sync_cmd
		 *    Wait()s, then reads req->status/result and pool_free's.
		 *    We MUST NOT free here.
		 *  - I/O (req->io != NULL): the originating IOStdReq has just
		 *    been ReplyMsg'd by nvme_end_req; the request is ours to
		 *    free.  slab_free via req->ac.
		 *  - Otherwise: skip. */
		if (req->done || req->waiter)
		{
			/* callback or waiter owns req — already handled */
		}
		else if (req->io && req->ac)
		{
			slab_free(&req->ac->req_slab, req);
		}

		return;
	case RETRY:
		/* DO NOT cleanup here — nvme_retry_req (immediate path or via
		 * retry_list watchdog) rings the doorbell again with the same
		 * SQE, so PRPs and any bounce buffer must remain live. */
		nvme_retry_req(req);
		return;
	}
}
