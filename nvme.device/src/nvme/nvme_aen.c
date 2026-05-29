// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_aen.c — Async Event Notification dispatcher.
 *
 * nvme_complete_async_event() is called from the admin completion path
 * when an outstanding AER command completes.  We decode the event type
 * and either kick off a namespace scan / firmware activation / reset,
 * or stash the result for user-space delivery.  Re-arming the AER (so
 * the controller has another outstanding slot to report the next event)
 * happens by calling nvme_submit_aer() directly from the aer_done callback.
 */
#include <nvme/nvme_admin.h> /* nvme_set_features, nvme_submit_async_cmd */
#include <nvme/nvme_aen.h>
#include <nvme/nvme_ctrl.h>	   /* nvme_change_ctrl_state */
#include <nvme/nvme_fw.h>
#include <nvme/nvme_probe.h>
#include <nvme/nvme_scan.h>

/*
 * nvme_aer_type - extract the AER type from an AEN completion result
 *
 * Returns bits [2:0] of the AEN result, which encode the event type
 * (NVME_AER_NOTICE, NVME_AER_ERROR, NVME_AER_SMART, etc.).
 *
 * @result: 32-bit completion result from the AEN command
 * Returns: 3-bit AER type field
 */
static inline u32 nvme_aer_type(u32 result)
{
	return result & 0x7;
}

/*
 * nvme_aer_subtype - extract the AER information field from an AEN result
 *
 * Returns bits [15:8] of the AEN result, which encode the event-type-specific
 * subtype (e.g. NVME_AER_NOTICE_NS_CHANGED for Notice events).
 *
 * @result: 32-bit completion result from the AEN command
 * Returns: 8-bit AER information / subtype field
 */
static inline u32 nvme_aer_subtype(u32 result)
{
	return (result & 0xff00) >> 8;
}

/*
 * nvme_handle_aen_notice - process an AER Notice event
 *
 * Dispatches based on the AER notice subtype.  Namespace Changed notices set
 * the NS_CHANGED event bit and queue a scan.  Firmware Activation Starting
 * notices transition the controller to RESETTING and queue fw_act_work.
 * Discovery Changed notices stash the result in ctrl->aen_result.
 *
 * @ctrl:   controller that received the notice
 * @result: 32-bit AEN completion result
 */
static BOOL nvme_handle_aen_notice(struct NVMeController *ctrl, u32 result)
{
	u32 aer_notice_type = nvme_aer_subtype(result);
	BOOL requeue = TRUE;

	switch (aer_notice_type)
	{
	case NVME_AER_NOTICE_NS_CHANGED:
		set_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events);
		nvme_queue_scan(ctrl);
		break;
	case NVME_AER_NOTICE_FW_ACT_STARTING:
		/*
		 * We are (ab)using the RESETTING state to prevent subsequent
		 * recovery actions from interfering with the controller's
		 * firmware activation.
		 */
		if (nvme_change_ctrl_state(ctrl, NVME_CTRL_RESETTING))
		{
			requeue = FALSE;
			nvme_queue_fw_act_work(ctrl);
		}
		break;
	case NVME_AER_NOTICE_DISC_CHANGED:
		ctrl->aen_result = result;
		break;
	default:
		Kprintf("[nvme] %s: async event result %08lx\n",
			__func__, (ULONG)result);
	}
	return requeue;
}

/*
 * nvme_handle_aer_persistent_error - handle a Persistent Internal Error AEN
 *
 * Logs a warning and initiates a controller reset.  Unlike other error AENs
 * where we requeue a new AEN command immediately, a persistent error means
 * the controller must be reset before it can accept further admin commands;
 * resubmission happens after the reset completes.
 *
 * @ctrl: controller that reported the persistent internal error
 */
static void nvme_handle_aer_persistent_error(struct NVMeController *ctrl)
{
	Kprintf("[nvme] %s: Persistent Internal Error AER received\n", __func__);
	nvme_reset_ctrl(ctrl);
}

/*
 * nvme_complete_async_event - process the completion of an AEN command
 *
 * Called from aer_done() when an AEN command completes.  Decodes the event
 * type and routes to the appropriate handler.  For Notice events, dispatches
 * to nvme_handle_aen_notice.  For SMART, CSS, VS, and non-persistent error
 * events, stashes the result in ctrl->aen_result.  For persistent internal
 * errors, triggers a controller reset; the reset path handles resubmission.
 *
 * @ctrl:   controller that issued the AEN
 * @status: completion status (events with status != NVME_SC_SUCCESS are dropped)
 * @res:    completion result containing the AEN payload
 */
static void nvme_complete_async_event(struct NVMeController *ctrl, __le16 status,
									  volatile union nvme_result *res)
{
	u32 result = le32(res->u32);
	u32 aer_type = nvme_aer_type(result);
	u32 aer_subtype = nvme_aer_subtype(result);
	if (le16(status) >> 1 != NVME_SC_SUCCESS)
		return;

	switch (aer_type)
	{
	case NVME_AER_NOTICE:
		(void)nvme_handle_aen_notice(ctrl, result);
		break;
	case NVME_AER_ERROR:
		/*
		 * For a persistent internal error the controller must be reset
		 * before it can accept further admin commands; the reset path
		 * will re-arm the AER slot.
		 */
		if (aer_subtype == NVME_AER_ERROR_PERSIST_INT_ERR)
		{
			nvme_handle_aer_persistent_error(ctrl);
			return;
		}
		/* fallthrough */
	case NVME_AER_SMART:
	case NVME_AER_CSS:
	case NVME_AER_VS:
		ctrl->aen_result = result;
		break;
	default:
		break;
	}
}

/*
 * nvme_enable_aen - configure which event classes the controller may report.
 *
 * Issues Set Features (Async Event Configuration, fid 0x0B) with the
 * intersection of NVME_AEN_SUPPORTED and ctrl->oaes (populated by
 * Identify Controller in nvme_init_identify).  Idempotent — safe to
 * re-call after reset.
 *
 * Set Features here only *configures* what the controller may report;
 * the controller still requires at least one outstanding AER admin
 * command before it will actually post a notification.  See
 * nvme_submit_aer below for that half of the protocol.
 */
void nvme_enable_aen(struct NVMeController *ctrl)
{
	if (!ctrl)
		return;

	u32 supported_aens = (u32)NVME_AEN_SUPPORTED & ctrl->oaes;
	if (!supported_aens)
	{
		Kprintf("[nvme] %s: controller advertises no supported AENs (oaes=%08lx)\n",
				__func__, (ULONG)ctrl->oaes);
		return;
	}

	int ret = nvme_set_features(ctrl, NVME_FEAT_ASYNC_EVENT,
								supported_aens, NULL, 0, NULL);
	if (ret)
	{
		Kprintf("[nvme] %s: Set Features (Async Event Cfg) failed: %ld\n",
				__func__, (LONG)ret);
		return;
	}
	Kprintf("[nvme] %s: AEN configured (mask=%08lx)\n",
			__func__, (ULONG)supported_aens);
}

/*
 * aer_done - completion callback for an AER admin command.
 *
 * Called from the admin queue completion path when an outstanding AER command
 * completes.  Dispatches the event via nvme_complete_async_event, frees the
 * request, then re-arms by calling nvme_submit_aer() inline.
 * nvme_submit_async_cmd is safe to call from the admin task context — it
 * writes the SQ ring and rings the doorbell without blocking or re-entering
 * the CQ.
 *
 * req->status holds the phase-stripped status word; we shift left by 1 to
 * re-encode the phase bit before passing it to nvme_complete_async_event.
 */
static void aer_done(struct nvme_request *req)
{
	struct NVMeController *ctrl = req->ac;
	__le16 status = (__le16)((u16)req->status << 1);

	nvme_complete_async_event(ctrl, status, &req->result);

	slab_free(&ctrl->req_slab, req);

	/* Re-arm: submit the next AER inline.  Persistent-error events
	 * trigger a reset instead — the reset path re-arms via nvme_start_ctrl. */
	nvme_submit_aer(ctrl);
}

/*
 * nvme_submit_aer - post one Async Event Request admin command.
 *
 * The AER sits in admin inflight[] indefinitely; the NVME_REQ_AER flag
 * tells watchdog_scan_queue to skip it.  Completion routes through
 * aer_done above.
 */
void nvme_submit_aer(struct NVMeController *ctrl)
{
	if (!ctrl)
		return;

	struct nvme_command cmd;
	mem_zero(&cmd, sizeof(cmd));
	cmd.common.opcode = nvme_admin_async_event;

	int ret = nvme_submit_async_cmd(ctrl, &cmd, NULL, 0,
									aer_done, NULL, NVME_REQ_AER);
	if (ret)
		Kprintf("[nvme] %s: AER submit failed: %ld\n",
				__func__, (LONG)ret);
}
