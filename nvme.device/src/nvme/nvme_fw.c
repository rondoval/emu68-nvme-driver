// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_fw.c — firmware activation work.
 *
 * Triggered by a Firmware Activation Starting AEN that nvme_handle_aen_notice()
 * routes to nvme_queue_fw_act_work().  The admin task wakes
 * nvme_fw_act_work() which polls CSTS.PP, transitions back to LIVE,
 * unquiesces I/O, and reads the FW Slot Information log to update the
 * cached firmware revision and clear the AEN.
 */
#include <timing.h> /* get_time, delay_ms, time_deadline_passed */

#include <nvme/nvme_core.h>
#include <nvme/nvme_completion.h>
#include <nvme/nvme_admin.h>
#include <nvme/nvme_ctrl.h> /* nvme_change_ctrl_state */
#include <nvme/nvme_fw.h>
#include <nvme/nvme_aen.h> /* nvme_submit_aer */
#include <nvme/nvme_probe.h>
#include <nvme/nvme_queue.h> /* nvme_quiesce_io_queues, nvme_unquiesce_io_queues */

/*
 * nvme_ctrl_pp_status - check whether the controller is in Processing Paused state
 *
 * Reads CSTS and returns TRUE only if CC.EN is set and CSTS.PP is set,
 * indicating the controller has paused I/O processing (e.g. during sanitize).
 * Returns FALSE on register read failure or if CSTS reads as all-Fs.
 *
 * @ctrl: controller to check
 * Returns: TRUE if the controller is in Processing Paused state
 */
static BOOL nvme_ctrl_pp_status(struct NVMeController *ctrl)
{
	u32 csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);

	if (csts == ~0U)
		return FALSE;

	return ((ctrl->ctrl_config & NVME_CC_ENABLE) && (csts & NVME_CSTS_PP));
}

/*
 * fw_slot_info_done - async completion callback for nvme_get_fw_slot_info.
 *
 * req->priv is the log buffer (passed through nvme_submit_async_cmd).
 * Parses it, refreshes ctrl->id_strings.firmware (the revision shown via SCSI
 * INQUIRY) for a settled slot, then frees both the log buffer and the request.
 *
 * Runs from drain_cq → nvme_end_req — see the calling notes on
 * nvme_submit_async_cmd in nvme_admin.h.
 */
static void fw_slot_info_done(struct nvme_request *req)
{
	struct NVMeController *nc = req->ac;
	struct nvme_fw_slot_info_log *log = req->priv;

	if (req->status)
	{
		Kprintf("[nvme] fw_slot_info_done: Get FW SLOT INFO error status=0x%lx (%s)\n",
				(ULONG)req->status, nvme_get_error_status_str(req->status));
		goto out;
	}
	if (!log)
		goto out;

	/* Device→host transfer: invalidate stale CPU lines before reading. */
	nvme_cache_inval(log, sizeof(*log));

	u8 cur_fw_slot = log->afi & 0x7;
	u8 next_fw_slot = (log->afi & 0x70) >> 4;
	if (!cur_fw_slot || (next_fw_slot && (cur_fw_slot != next_fw_slot)))
	{
		Kprintf("[nvme] fw_slot_info_done: Firmware activated after next Controller Level Reset\n");
		goto out;
	}

	/* frs[] entries are 8-byte space-padded firmware revision strings,
	 * matching id_strings.firmware; refresh it to the active slot. */
	CopyMem((CONST_APTR)&log->frs[cur_fw_slot - 1],
			(APTR)nc->id_strings.firmware,
			sizeof(nc->id_strings.firmware));

out:
	if (log)
		pool_free(nc->memoryPool, log);
	slab_free(&nc->req_slab, req);
}

/*
 * nvme_get_fw_slot_info - read the Firmware Slot Information log and update cached FW rev
 *
 * Fetches the FW Slot Info log page and refreshes ctrl->id_strings.firmware to
 * reflect the currently active slot if the activation has completed.  If the
 * active firmware index suggests an activation is pending (next_slot != 0 and
 * differs from cur_slot), logs an info message instead.  Also used to clear
 * the firmware activation AEN.
 *
 * Async (not sync) because this is reached from nvme_fw_act_work, which
 * runs in the admin task — a sync admin submit there would deadlock.
 *
 * @ctrl: controller to query
 */
static void nvme_get_fw_slot_info(struct NVMeController *ctrl)
{
	struct nvme_fw_slot_info_log *log = pool_zalloc(ctrl->memoryPool, sizeof(*log));
	if (!log)
	{
		Kprintf("[nvme] %s: log alloc failed\n", __func__);
		return;
	}

	int ret = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0,
						   NVME_CSI_NVM, log, sizeof(*log), 0, fw_slot_info_done, log);
	if (ret)
	{
		Kprintf("[nvme] %s: async submit failed (%ld)\n", __func__, (LONG)ret);
		pool_free(ctrl->memoryPool, log);
	}
}

/*
 * nvme_fw_act_work - complete a firmware activation
 *
 * Runs in the admin task (AdminWorker) after a Firmware Activation Starting AEN
 * sets fw_act_signal; the task's Wait loop calls this directly with the ctrl in
 * scope.  Quiesces I/O, then polls CSTS.PP until the controller finishes
 * activating the new firmware or the MTFA timeout expires (which triggers a
 * controller reset).  Once activation completes, transitions the controller
 * back to LIVE, unquiesces I/O queues, reads the FW slot log to refresh the
 * cached firmware revision (and acknowledge the AEN), and re-arms the AER.
 *
 * @ctrl: controller whose firmware is activating
 */
void nvme_fw_act_work(struct NVMeController *ctrl)
{
	/* MTFA is reported in 100 ms units (NVMe spec §5.15.2); the
	 * fallback NVME_ADMIN_TIMEOUT is already in ms. */
	const u32 fw_act_deadline_us = ctrl->mtfa
									   ? get_time() + (u32)ctrl->mtfa * 100U * 1000U
									   : get_time() + (u32)NVME_ADMIN_TIMEOUT * 1000U;

	nvme_quiesce_io_queues(ctrl);
	while (nvme_ctrl_pp_status(ctrl))
	{
		if (time_deadline_passed(get_time(), fw_act_deadline_us))
		{
			Kprintf("[nvme] Fw activation timeout, reset controller\n");
			nvme_reset_ctrl(ctrl);
			return;
		}
		delay_ms(100);
	}

	if (!nvme_change_ctrl_state(ctrl, NVME_CTRL_CONNECTING) ||
		!nvme_change_ctrl_state(ctrl, NVME_CTRL_LIVE))
		return;

	nvme_unquiesce_io_queues(ctrl);
	/* read FW slot information to clear the AER */
	nvme_get_fw_slot_info(ctrl);

	nvme_submit_aer(ctrl);
}
