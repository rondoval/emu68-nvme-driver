// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_AEN_H
#define NVME_AEN_H

#include <nvme/nvme_core.h>     /* nvme_defs.h: NVME_AEN_CFG_* */

struct NVMeController;

/*
 * Default set of Asynchronous Event Configuration bits we ask the
 * controller to report.  AND with ctrl->oaes before passing to
 * Set Features so we only enable events the device actually supports.
 * The handlers for each class live in nvme_complete_async_event below.
 */
#define NVME_AEN_SUPPORTED \
	(NVME_AEN_CFG_NS_ATTR | NVME_AEN_CFG_FW_ACT | \
	 NVME_AEN_CFG_ANA_CHANGE | NVME_AEN_CFG_DISC_CHANGE)

/*
 * nvme_enable_aen - configure which Asynchronous Event classes the
 * controller should report.  Issues Set Features (Async Event
 * Configuration) with the intersection of NVME_AEN_SUPPORTED and
 * ctrl->oaes.  Called once per controller at LIVE-transition time
 * from nvme_start_ctrl.  The first AER admin command is posted
 * separately via nvme_submit_aer.
 */
void nvme_enable_aen(struct NVMeController *ctrl);

/*
 * nvme_submit_aer - post one Async Event Request admin command.
 *
 * The AER sits in the admin queue's inflight[] table indefinitely
 * until the controller has an event to report (could be hours).  The
 * NVME_REQ_AER flag is set on the request so the watchdog skips it.
 *
 * When the AER CQE arrives, the internal aer_done callback dispatches
 * via nvme_complete_async_event, frees the request, then calls this
 * function directly to re-arm.
 *
 * Called from nvme_start_ctrl (initial arm), aer_done (re-arm after
 * each AEN), and nvme_fw_act_work (re-arm after firmware activation).
 */
void nvme_submit_aer(struct NVMeController *ctrl);

#endif /* NVME_AEN_H */
