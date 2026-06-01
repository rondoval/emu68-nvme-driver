// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVMe admin command path.
 *
 * Holds:
 *   - admin-queue request alloc + PRP staging
 *   - the public sync/async admin submit entry points
 *   - the Abort admin command builder (used both for AbortIO and the
 *     watchdog timeout escalation)
 *   - Set Features helpers (Number of Queues / Timestamp / Host Behavior)
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <errors.h>

#include <device.h>
#include <nvme/nvme_ctrl.h> /* struct NVMeController */
#include <nvme/nvme_completion.h>
#include <nvme/nvme_admin.h>
#include <nvme/nvme_io.h>    /* NVME_IO_ASYNC, nvme_submit_io, nvme_req_destroy */
#include <nvme/nvme_queue.h> /* nvme_alloc_cid, nvme_inflight_claim */

/* ---------------------------------------------------------------- *
 *  Section 1: static helpers (request alloc, PRP staging)          *
 * ---------------------------------------------------------------- */

/*
 * nvme_bytes_to_numd - convert a byte length to an NVMe 0-based NUMD value
 *
 * Many NVMe admin commands encode a data transfer length as Number of Dwords
 * Minus One (NUMD).  This divides @len by 4 (bytes to dwords) then subtracts
 * 1 to produce the 0-based value the spec requires.
 *
 * @len: transfer length in bytes (must be a non-zero multiple of 4)
 * Returns: 0-based dword count suitable for the NVMe NUMD field
 */
static inline u32 nvme_bytes_to_numd(size_t len)
{
    return (len >> 2) - 1;
}

/*
 * nvme_req_alloc_admin - admin-queue counterpart of nvme_req_alloc_io.
 *
 * Picks a CID from the admin queue's per-queue inflight table and
 * leaves io/unit NULL — caller is responsible for setting waiter +
 * wait_signal before submit.  Returns -ENOMEM / -EBUSY via @err_out on
 * failure to match the kernel-style error convention used by
 * __nvme_submit_sync_cmd.
 */
static struct nvme_request *nvme_req_alloc_admin(struct NVMeController *ctrl,
                                                 int *err_out)
{
    struct nvme_queue *q = &ctrl->admin_q;

    struct nvme_request *req = slab_zalloc(&ctrl->req_slab);
    if (!req)
    {
        *err_out = -ENOMEM;
        return NULL;
    }

    u16 cid = nvme_alloc_cid(q);
    if (cid == 0xFFFF)
    {
        slab_free(&ctrl->req_slab, req);
        *err_out = -EBUSY;
        return NULL;
    }

    req->ac = ctrl;
    req->q = q;
    req->cid = cid;
    nvme_inflight_claim(q, cid, req);
    return req;
}

/*
 * nvme_init_request - stage a caller-supplied nvme_command into @req.
 *
 * Clears any SGL flags the caller left on @cmd (this driver always
 * uses PRPs), scrubs status/retries/flags, and copies the embedded
 * command.  Called from both submit primitives below so every admin
 * code path goes through the same SGL-flag mask.
 */
static void nvme_init_request(struct nvme_request *req, struct nvme_command *cmd)
{
    cmd->common.flags = (u8)(cmd->common.flags & (u8)~NVME_CMD_SGL_ALL);
    req->status = 0;
    req->retries = 0;
    req->flags = 0;
    req->cmd = *cmd; /* embedded copy */
}

/*
 * nvme_stage_admin_prps - PRP1/PRP2 setup for an admin-cmd buffer.
 *
 * Admin commands carry <= one controller page (4 KiB) of data, but the
 * caller's buffer can be ANY 4-byte-aligned address — pool_zalloc does
 * not page-align — so a 4 KiB transfer may straddle two host pages.
 * In that case PRP1 covers the partial first page and PRP2 must point
 * at the next page's base address.
 *
 *   PRP1 = buffer  (any alignment, NVMe spec §4.1.2)
 *   first_page_bytes = NVME_CTRL_PAGE_SIZE - (buffer & PAGE_MASK)
 *   if buflen > first_page_bytes: PRP2 = next 4 KiB-aligned page
 *
 * Caller is responsible for the post-DMA cache
 * invalidate (sync waiter does it after Wait(); async callback does
 * it before reading req->result).
 */
static void nvme_stage_admin_prps(struct nvme_request *req,
                                  void *buffer, u32 buflen)
{
    /* No data payload: leave dptr untouched.  Some admin commands
     * (Create I/O CQ / Create I/O SQ) carry the queue base address in
     * prp1 instead of pointing at a data buffer — the caller fills
     * those bytes before submission and we must not stomp them. */
    if (!buffer || !buflen)
        return;

    u64 buf_addr = (u64)(ULONG)buffer;
    u32 first_page_left = (u32)(NVME_CTRL_PAGE_SIZE - (buf_addr & (NVME_CTRL_PAGE_SIZE - 1)));

    req->cmd.common.dptr.prp1 = le64(buf_addr);

    if (buflen <= first_page_left)
    {
        req->cmd.common.dptr.prp2 = 0;
    }
    else
    {
        u64 prp2_addr = (buf_addr + first_page_left) & ~(u64)(NVME_CTRL_PAGE_SIZE - 1);
        req->cmd.common.dptr.prp2 = le64(prp2_addr);
    }

    nvme_cache_flush(buffer, buflen);
}

/* ---------------------------------------------------------------- *
 *  Section 2: submit primitives (the public sync/async API)        *
 * ---------------------------------------------------------------- */

/*
 * nvme_submit_sync_cmd - submit an admin command and block until completion
 *
 * Implements the Amiga waiter idiom replacing the Linux blk-mq
 * nvme_submit_sync_cmd().  The caller's task is parked on a freshly
 * allocated signal bit; when nvme_process_completions() picks up the
 * CQE for our tag it calls nvme_end_req() which Signal()s us back.
 *
 * @result is optional — pass NULL if the caller doesn't need the CQE's
 * Result DW0.  Only a handful of admin opcodes (Set Features, Get
 * Features, Abort) put meaningful data in Result DW0; most return only
 * a status.
 *
 * WARNING — calling-task invariant:
 *   drain_cq runs inside ctrl->unit_task, so this MUST NOT be called
 *   from unit_task or anything reached via its Wait() loop
 *   (ProcessCommand, watchdog, IRQ-signal handlers) — that would park
 *   the task on a signal only itself can deliver.  Unit-task contexts
 *   must use nvme_submit_async_cmd instead.
 *
 *   Safe contexts:
 *     - ctrl->admin_task (AdminWorker): NVMe passthrough, AEN-driven
 *       rescan, fw_act polling.
 *     - One-shot probe / bring-up before either task exists.
 */
int nvme_submit_sync_cmd(struct NVMeController *ctrl, struct nvme_command *cmd,
                         union nvme_result *result, void *buffer, u32 buflen)
{
    KprintfH("[nvme] submit_sync_cmd: ctrl=%lx opcode=0x%02lx (%s) buf=%lx buflen=%lu result=%lx\n",
             (ULONG)ctrl, (ULONG)cmd->common.opcode,
             nvme_get_admin_opcode_str(cmd->common.opcode),
             (ULONG)buffer, (ULONG)buflen, (ULONG)result);

    if (!ctrl)
        return -ENODEV;

    int err;
    struct nvme_request *req = nvme_req_alloc_admin(ctrl, &err);
    if (!req)
        return err;

    BYTE signal_bit = (BYTE)AllocSignal(-1);
    if (signal_bit == (BYTE)-1)
    {
        nvme_req_destroy(req);
        return -ENOMEM;
    }

    req->waiter = FindTask(NULL);
    req->wait_signal = signal_bit;
    /* nvme_init_request copies @cmd into req->cmd and clears any SGL
     * flags the caller may have left set — admin commands always use
     * PRPs on this driver. */
    nvme_init_request(req, cmd);
    req->cmd.common.command_id = req->cid;

    nvme_stage_admin_prps(req, buffer, buflen);

    if (nvme_submit_io(req) != NVME_IO_ASYNC)
    {
        FreeSignal(signal_bit);
        /* nvme_submit_io already destroyed @req. */
        return -EIO;
    }

    Wait(1UL << signal_bit);

    /* Invalidate any stale CPU cache lines covering @buffer so the
     * caller's next read sees what the device DMA-wrote (e.g. the
     * Identify Controller / Identify Namespace responses). */
    if (buffer && buflen)
        nvme_cache_inval(buffer, buflen);

    int status = (int)req->status;
    if (result)
        *result = req->result;

    FreeSignal(signal_bit);
    slab_free(&ctrl->req_slab, req);
    return status;
}

/*
 * nvme_submit_async_cmd - submit an admin command without blocking.
 *
 * Variant of nvme_submit_sync_cmd that stores @done on the request
 * instead of Wait()ing.  @done runs inside the unit task's
 * drain_cq → nvme_complete_rq → nvme_end_req chain with the CQE
 * already written to req->status / req->result.
 *
 * Ownership: on success the callback owns the request — it must
 * slab_free(&req->ac->req_slab, req) and FreeMem (or equivalent)
 * any data buffer it staged.  On synchronous submit failure
 * (return < 0) the request is already destroyed and @done is NOT
 * called.
 *
 * Safe from any task, including ctrl->unit_task — required for
 * unit-task work (nvme_io_abort, watchdog Abort escalation) and for
 * long-lived AERs that sit in admin inflight[] indefinitely.  See the
 * warning on nvme_submit_sync_cmd for why the sync variant is
 * forbidden in unit-task contexts.
 *
 * The callback is responsible for the post-DMA cache invalidate on
 * any device→host buffer it staged, BEFORE reading req->result.
 */
int nvme_submit_async_cmd(struct NVMeController *ctrl, struct nvme_command *cmd,
                          void *buffer, u32 buflen,
                          void (*done)(struct nvme_request *req), void *priv,
                          unsigned int req_flags)
{
    KprintfH("[nvme] submit_async_cmd: ctrl=%lx opcode=0x%02lx (%s) buf=%lx buflen=%lu done=%lx priv=%lx flags=0x%lx\n",
             (ULONG)ctrl, (ULONG)cmd->common.opcode,
             nvme_get_admin_opcode_str(cmd->common.opcode),
             (ULONG)buffer, (ULONG)buflen, (ULONG)done, (ULONG)priv,
             (ULONG)req_flags);

    if (!ctrl)
        return -ENODEV;
    if (!done)
    {
        Kprintf("[nvme] submit_async_cmd: NULL done callback (req would leak)\n");
        return -EINVAL;
    }

    int err;
    struct nvme_request *req = nvme_req_alloc_admin(ctrl, &err);
    if (!req)
        return err;

    req->done = done;
    req->priv = priv;
    /* nvme_init_request resets status/retries/flags and copies @cmd
     * with SGL flags masked off; req_flags is OR'd in afterwards so
     * the caller's USERCMD / AER markers survive. */
    nvme_init_request(req, cmd);
    req->cmd.common.command_id = req->cid;
    req->flags |= (enum nvme_req_flags)req_flags;

    nvme_stage_admin_prps(req, buffer, buflen);

    if (nvme_submit_io(req) != NVME_IO_ASYNC)
    {
        /* nvme_submit_io already destroyed @req. */
        return -EIO;
    }
    return 0;
}

/* ---------------------------------------------------------------- *
 *  Section 3: abort family (built on nvme_submit_async_cmd)        *
 * ---------------------------------------------------------------- */

/*
 * abort_done - completion callback for the Abort admin command.
 *
 * Logs the Abort acceptance status (the target's own NVME_SC_ABORT_REQ
 * CQE arrives separately via the normal completion path) and releases
 * the admin request.
 */
static void abort_done(struct nvme_request *req)
{
    KprintfH("[nvme] abort_done: sqid=%lu cid=%lu status=0x%lx (%s)\n",
             (ULONG)le16(req->cmd.abort.sqid),
             (ULONG)le16(req->cmd.abort.cid),
             (ULONG)req->status, nvme_get_error_status_str(req->status));
    slab_free(&req->ac->req_slab, req);
}

/*
 * nvme_submit_abort_sqe - build and submit an Abort SQE for (sqid, cid).
 *
 * The lone place in the driver that encodes the Abort SQE.  Both
 * exported abort entry points (nvme_abort_request, nvme_io_abort) end
 * up here.  Returns 0 on submit, negative errno on failure (the
 * request was not enqueued).
 */
static int nvme_submit_abort_sqe(struct NVMeController *ctrl, u16 sqid, u16 cid)
{
    struct nvme_command cmd;

    mem_zero(&cmd, sizeof(cmd));
    cmd.abort.opcode = nvme_admin_abort_cmd;
    /* cdw10: CID in high 16, SQID in low 16 — see NVMe spec §5.1. */
    cmd.abort.cid = le16(cid);
    cmd.abort.sqid = le16(sqid);

    int ret = nvme_submit_async_cmd(ctrl, &cmd, NULL, 0,
                                    abort_done, NULL, 0);
    if (ret)
    {
        Kprintf("[nvme] submit_abort_sqe: sqid=%lu cid=%lu failed: %ld\n",
                (ULONG)sqid, (ULONG)cid, (LONG)ret);
        return ret;
    }
    KprintfH("[nvme] submit_abort_sqe: sqid=%lu cid=%lu submitted\n",
             (ULONG)sqid, (ULONG)cid);
    return 0;
}

/*
 * nvme_abort_request - submit an Abort admin command for one in-flight req.
 *
 * Used by both the user-initiated AbortIO path (via nvme_io_abort's
 * inner loop) and the watchdog timeout escalation in nvme_queue.c —
 * both already hold the nvme_request whose (qid, tag) pair we want to
 * abort.  Returns 0 on submit, negative errno on failure.  The
 * target's NVME_SC_ABORT_REQ CQE arrives via the normal completion
 * path; this function never blocks.
 */
int nvme_abort_request(struct NVMeController *ctrl, struct nvme_request *req)
{
    return nvme_submit_abort_sqe(ctrl, req->q->qid, req->cid);
}

/*
 * nvme_io_abort - submit Abort admin cmd(s) for an in-flight I/O request.
 *
 * Walks the I/O queue's inflight[] for any request whose originating
 * IOStdReq matches @target.  Single-shot commands carry the IOStdReq
 * on req->io directly; chunked I/Os carry it on req->ctx->io with each
 * sibling's req->io being NULL.  We submit an Abort SQE for EVERY
 * matching sibling — the controller doesn't cascade aborts across
 * sibling tags, so each one needs its own.  The targets' NVME_SC_ABORT_REQ
 * CQEs arrive asynchronously via the normal completion path;
 * nvme_complete_rq + nvme_io_context_finish reply the originating io
 * exactly once (after the last sibling).
 *
 * Async because this is called from CMD_INTERNAL_ABORT_REQUEST
 * inside the unit task's ProcessCommand — a sync submit would deadlock
 * waiting for ourselves to drain the admin CQE.  See the calling-task
 * invariant on nvme_submit_sync_cmd in nvme_admin.h.
 *
 * Returns 0 if at least one Abort was submitted, -1 if @target wasn't
 * found in-flight (already completed) or every Abort submission failed.
 */
int nvme_io_abort(struct NVMeController *ctrl, struct IOStdReq *target)
{
    if (!ctrl || !target)
        return -1;

    struct nvme_queue *q = &ctrl->io_q;
    if (!q->inflight)
        return -1;

    int aborted = 0;
    for (u16 cid = 0; cid < q->depth; cid++)
    {
        struct nvme_request *r = q->inflight[cid];

        if (!r)
            continue;
        BOOL match = (r->io == target) || (r->ctx && r->ctx->io == target);
        if (!match)
            continue;

        if (nvme_abort_request(ctrl, r) == 0)
            aborted++;
    }

    return aborted ? 0 : -1;
}

/* ---------------------------------------------------------------- *
 *  Section 4: Set Features + controller-init configurators         *
 * ---------------------------------------------------------------- */

/*
 * nvme_set_features - issue a Set Features admin command
 *
 * Sets a controller or namespace feature identified by @fid.  On a
 * successful CQE the controller's CDW0 result is written back to
 * @result (host byte order) if non-NULL.
 *
 * @dev:     controller to configure
 * @fid:     feature identifier (NVME_FEAT_*)
 * @dword11: feature-specific CDW11 value
 * @buffer:  optional data buffer (may be NULL)
 * @buflen:  size of data buffer
 * @result:  if non-NULL, receives the CDW0 result from the CQE (u32 *)
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
int nvme_set_features(struct NVMeController *dev, unsigned int fid,
                      unsigned int dword11, void *buffer, size_t buflen,
                      void *result)
{
    union nvme_result res = {0};
    struct nvme_command c = {};

    c.features.opcode = nvme_admin_set_features;
    c.features.fid = le32(fid);
    c.features.dword11 = le32(dword11);

    int ret = nvme_submit_sync_cmd(dev, &c, &res, buffer, buflen);
    if (ret >= 0 && result)
        *(u32 *)result = le32(res.u32);
    return ret;
}

/*
 * nvme_configure_timestamp - synchronize the controller's internal timestamp
 *
 * If the controller supports the Timestamp feature (ONCS bit 6), sets it to
 * the current real-wall-clock time in milliseconds.  Called once during
 * controller initialization so that the device's internal event log timestamps
 * are aligned with host time.
 *
 * @ctrl: controller to configure
 * Returns: 0 on success or if the feature is not supported, non-zero on error
 */
int nvme_configure_timestamp(struct NVMeController *ctrl)
{
    if (!(ctrl->oncs & NVME_CTRL_ONCS_TIMESTAMP))
        return 0;

    __le64 ts __attribute__((aligned(4))) = le64(nvme_unix_time_ms());
    int ret = nvme_set_features(ctrl, NVME_FEAT_TIMESTAMP, 0, &ts, sizeof(ts), NULL);
    if (ret)
        Kprintf("[nvme] %s: could not set timestamp (%ld)\n", __func__, ret);
    return ret;
}

/*
 * nvme_configure_irq_coalesce - enable NVMe interrupt coalescing (Set
 * Features 0x08) when configured via the DEVICE_IRQ_COALESCE_* knobs.
 *
 * CDW11: bits 7:0 = aggregation threshold (THR, 0-based — the controller
 * fires after THR+1 completions), bits 15:8 = aggregation time (TIME, in
 * 100 µs units).  Both knobs 0 (the default) leaves the feature at the
 * controller default and sends no command — see config.h for the rationale
 * (matches Linux, which never enables coalescing by default).
 *
 * @ctrl: controller to configure
 * Returns: 0 on success or when disabled, negative errno / NVMe status on error
 */
int nvme_configure_irq_coalesce(struct NVMeController *ctrl)
{
    const unsigned int time = DEVICE_IRQ_COALESCE_TIME;
    const unsigned int thr = DEVICE_IRQ_COALESCE_THR;

    if (time == 0 && thr == 0)
        return 0; /* feature disabled — leave controller default */

    const unsigned int dword11 = ((time & 0xFFu) << 8) | (thr & 0xFFu);
    int ret = nvme_set_features(ctrl, NVME_FEAT_IRQ_COALESCE, dword11, NULL, 0, NULL);
    if (ret)
        Kprintf("[nvme] %s: could not set IRQ coalescing (time=%lu thr=%lu): %ld\n",
                __func__, (ULONG)time, (ULONG)thr, (LONG)ret);
    else
        Kprintf("[nvme] %s: IRQ coalescing on (time=%lu*100us thr=%lu)\n",
                __func__, (ULONG)time, (ULONG)thr);
    return ret;
}

/*
 * nvme_configure_host_options - set the Host Behavior Support feature
 *
 * Enables ACRE (Advanced Command Retry Enable) if the controller reports
 * non-zero CRD values, and LBAFEE (LBA Format Extension Enable) if the
 * controller supports extended LBA formats.  If neither is needed the
 * feature is not sent.  Called once during controller initialization.
 *
 * @ctrl: controller to configure
 * Returns: 0 on success, negative errno or NVMe status on failure
 */
int nvme_configure_host_options(struct NVMeController *ctrl)
{
    struct nvme_feat_host_behavior *host;
    host = pool_zalloc(ctrl->memoryPool, sizeof(*host));
    if (!host)
        return 0;

    /* Don't bother enabling the feature if retry delay is not reported */
    if (ctrl->crdt[0])
        host->acre = NVME_ENABLE_ACRE;
    if (ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)
        host->lbafee = NVME_ENABLE_LBAFEE;

    if (!host->acre && !host->lbafee)
        return 0;

    int ret = nvme_set_features(ctrl, NVME_FEAT_HOST_BEHAVIOR, 0,
                                host, sizeof(*host), NULL);
    pool_free(ctrl->memoryPool, host);
    return ret;
}

/*
 * nvme_get_log - issue a Get Log Page command, synchronously or asynchronously
 *
 * Builds the Get Log Page command (LSI is always 0 in this driver) and submits
 * it.  The submission mode is chosen by @done:
 *
 *   @done == NULL: submitted synchronously; the result is in @log on return and
 *                  the caller owns @log.  Must NOT be called from the admin/unit
 *                  task — it blocks on completion (see nvme_submit_sync_cmd).
 *   @done != NULL: submitted asynchronously; @done fires from the completion
 *                  drain with the CQE latched, owns the request, and must free
 *                  @log.  Safe from any task.  @priv is recovered as req->priv.
 *
 * Used by AEN handling (changed-ns log), the Command Effects Log fetch during
 * controller identification (both sync), and the firmware-slot read in
 * nvme_fw.c (async).
 *
 * @ctrl:     controller to query
 * @nsid:     namespace ID (NVME_NSID_ALL for controller-level logs)
 * @log_page: log page identifier (NVME_LOG_*)
 * @lsp:      log specific parameter
 * @csi:      command set identifier for I/O Command Set specific logs
 * @log:      output buffer
 * @size:     size of the output buffer in bytes
 * @offset:   byte offset within the log page
 * @done:     async completion callback, or NULL for a synchronous submit
 * @priv:     opaque value recovered as req->priv in @done (async only)
 * Returns: 0 on success (sync) / on submit (async); negative errno or positive
 *          NVMe status otherwise.  On async submit failure @done does not run
 *          and the caller still owns @log.
 */
int nvme_get_log(struct NVMeController *ctrl, u32 nsid, u8 log_page, u8 lsp, u8 csi,
                 void *log, size_t size, u64 offset,
                 void (*done)(struct nvme_request *), void *priv)
{
    struct nvme_command c = {};
    u32 dwlen = nvme_bytes_to_numd(size);

    c.get_log_page.opcode = nvme_admin_get_log_page;
    c.get_log_page.nsid = le32(nsid);
    c.get_log_page.lid = log_page;
    c.get_log_page.lsp = lsp;
    c.get_log_page.numdl = le16(dwlen & ((1 << 16) - 1));
    c.get_log_page.numdu = le16(dwlen >> 16);
    c.get_log_page.lpol = le32(u64_lo32(offset));
    c.get_log_page.lpou = le32(u64_hi32(offset));
    c.get_log_page.csi = csi;

    if (done)
        return nvme_submit_async_cmd(ctrl, &c, log, size, done, priv, 0);
    return nvme_submit_sync_cmd(ctrl, &c, NULL, log, size);
}
