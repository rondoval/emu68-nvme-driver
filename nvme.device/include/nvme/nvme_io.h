// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_IO_H
#define NVME_IO_H

struct nvme_io_context;

/*
 * NVME_IO_ASYNC - submit returned successfully; the IOStdReq is now
 * owned by the completion path (reply_io will reply it).  Any other
 * BYTE return from a submit helper is an IOERR_* and the caller still
 * owns the IOStdReq.
 */
#define NVME_IO_ASYNC  ((BYTE)1)

/*
 * nvme_io_submit_rw / _flush / _dsm - public I/O submission helpers.
 *
 * Each handles request allocation, tag allocation, SQE setup, inflight
 * registration, submit, and synchronous-failure cleanup.  Returns
 * NVME_IO_ASYNC on success or an IOERR_* code (the request has been
 * freed; caller maps to reply_io / scsi_make_sense).
 *
 * nvme_io_submit_rw transparently splits transfers larger than MDTS
 * into sibling commands ridden by a parent nvme_io_context; up to
 * NVME_MAX_INFLIGHT_PER_IO siblings are kept in flight.
 *
 * Callers must have validated namespace bounds (LBA, block count)
 * before calling.
 *
 * nvme_io_submit_dsm: @ranges must be a dma_zalloc'd page-aligned
 * buffer of size sizeof(*ranges) * NVME_DSM_MAX_RANGES (4 KiB) with
 * the first @nr slots filled by the caller; slots past @nr must
 * remain zero (the dma_zalloc guarantee — the device may DMA the
 * whole 4 KiB regardless of @nr).  The function takes ownership of
 * @ranges unconditionally: on NVME_IO_ASYNC the completion path
 * frees it; on any error return the function frees it before
 * returning.  Caller must not touch @ranges after the call.
 */
BYTE nvme_io_submit_rw(struct NVMeUnit *unit, struct IOStdReq *io,
                       u64 lba, ULONG blocks, u8 opcode, void *buffer);
BYTE nvme_io_submit_flush(struct NVMeUnit *unit, struct IOStdReq *io);
BYTE nvme_io_submit_dsm(struct NVMeUnit *unit, struct IOStdReq *io,
                        struct nvme_dsm_range *ranges, u16 nr);

/*
 * nvme_io_context_pump - dispatch as many chunk siblings as the
 * NVME_MAX_INFLIGHT_PER_IO cap allows.  Called from nvme_io_submit_rw
 * (initial) and nvme_complete_rq (refill on each sibling CQE).
 * NVME_IO_ASYNC if at least one sibling is now in flight; IOERR_* if
 * dispatch failed with no sibling inflight (caller frees @ctx).
 */
BYTE nvme_io_context_pump(struct nvme_io_context *ctx);

/*
 * nvme_io_context_finish - last sibling has completed: stamp
 * io_Error / io_Actual from ctx->first_error / ctx->total_bytes,
 * ReplyMsg the originating IOStdReq, and pool_free @ctx.
 */
void nvme_io_context_finish(struct nvme_io_context *ctx);

/*
 * nvme_resubmit_io - retry-path entry point.  Allocates a fresh tag
 * for @req (drain_cq already returned the previous one), patches
 * req->cmd.common.command_id, and submits.  On tag-pool exhaustion
 * routes @req through nvme_complete_rq with NVME_SC_HOST_PATH_ERROR
 * and returns NVME_IO_ASYNC.
 */
BYTE nvme_resubmit_io(struct nvme_request *req);

/*
 * Per-request lifecycle helpers shared with the admin path in
 * nvme_admin.c.  Tag allocation lives in nvme_queue.h.
 */
void nvme_req_destroy(struct nvme_request *req);
BYTE nvme_req_submit(struct nvme_request *req);

/*
 * nvme_cleanup_cmd - release per-command DMA resources after completion:
 * chained PRP-list pages, the Fast-RAM bounce buffer (with bounce->user
 * copy-back for reads), and post-DMA cache invalidate for non-bounced
 * reads.  Run exactly once on the final completion of a given DMA setup
 * (NOT on the retry path — nvme_retry_req re-rings the same SQE).
 */
void nvme_cleanup_cmd(struct nvme_request *req);

/*
 * nvme_needs_bounce - true if @buffer can't be DMA'd directly.
 *
 * Bounces only buffers PCIe cannot reach (Amiga Chip RAM, first 2 MiB
 * under PiStorm) or that fail the NVMe spec §4.1.2 PRP1 Dword-alignment
 * rule.
 */
static inline BOOL nvme_needs_bounce(const void *buffer)
{
    uintptr_t addr = (uintptr_t)buffer;

    if (addr <= 0x1FFFFFu)
        return TRUE;          /* Chip RAM — PCIe DMA cannot reach */
    if (addr & 0x3u)
        return TRUE;          /* NVMe spec §4.1.2: PRP1 Dword-aligned */
    return FALSE;
}

#endif /* NVME_IO_H */
