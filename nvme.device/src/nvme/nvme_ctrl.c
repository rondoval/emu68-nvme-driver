// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_ctrl.c — controller state machine, capability identification, and
 * Command Effects Log initialisation.
 *
 * The PCI-side bringup/teardown sequencing lives in nvme_probe.c, which calls
 * into this module.  What lives here is everything about establishing and
 * representing the controller itself, organised into four sections:
 *
 *   1. Controller state machine — nvme_change_ctrl_state
 *   2. Transfer limits          — nvme_mps_to_sectors, nvme_set_limits
 *   3. Command Effects Log       — nvme_init_effects and its helpers
 *   4. Identify Controller       — nvme_init_identify / nvme_init_non_mdts_limits,
 *                                  populating capability fields from the
 *                                  Identify Controller response
 *
 * The actual Identify admin commands are issued by nvme_identify.c; this file
 * consumes their parsed results.  The two static-inline accessors
 * nvme_admin_ctrl / nvme_is_io_ctrl live in <nvme/nvme_ctrl.h> so they remain
 * inlinable from all the sibling .c files.
 */
#include <nvme/nvme_core.h>        /* must come first: kcompat.h establishes proto/exec.h
                          * decls before emu68-common's memory.h pool inlines */

#include <errors.h>

#include <device.h>
#include <nvme/nvme_admin.h>      /* nvme_get_log (effects log) */
#include <nvme/nvme_ctrl.h>
#include <nvme/nvme_identify.h>   /* nvme_identify_ctrl, nvme_identify_ctrl_nvm, nvme_id_cns_ok */
#include <nvme/nvme_quirks.h>     /* nvme_match_id_quirks (Identify-string quirks) */

/* ------------------------------------------------------------------ */
/* Controller state machine                                            */
/* ------------------------------------------------------------------ */

/*
 * nvme_change_ctrl_state - attempt a controller state machine transition
 *
 * Checks whether new_state is reachable from the current state; if so, commits
 * ctrl->state.  The allowed transitions mirror the NVMe spec lifecycle:
 * NEW→CONNECTING→LIVE, LIVE/RESETTING/CONNECTING→DELETING, etc.  (Unlike the
 * Linux original there is no state lock or wait-queue wake — the pci.c reset
 * path polls ctrl->state, and the field is a single aligned word.)
 *
 * @ctrl:      controller whose state to change
 * @new_state: desired new state
 * Returns: TRUE if the transition succeeded, FALSE if it was not allowed
 */
BOOL nvme_change_ctrl_state(struct NVMeController *ctrl,
		enum nvme_ctrl_state new_state)
{
	enum nvme_ctrl_state old_state = nvme_ctrl_state(ctrl);
	BOOL changed = FALSE;

	switch (new_state) {
	case NVME_CTRL_LIVE:
		switch (old_state) {
		case NVME_CTRL_CONNECTING:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	case NVME_CTRL_RESETTING:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_LIVE:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	case NVME_CTRL_CONNECTING:
		switch (old_state) {
		case NVME_CTRL_NEW:
		case NVME_CTRL_RESETTING:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	case NVME_CTRL_DELETING:
		switch (old_state) {
		case NVME_CTRL_LIVE:
		case NVME_CTRL_RESETTING:
		case NVME_CTRL_CONNECTING:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	case NVME_CTRL_DELETING_NOIO:
		switch (old_state) {
		case NVME_CTRL_DELETING:
		case NVME_CTRL_DEAD:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	case NVME_CTRL_DEAD:
		switch (old_state) {
		case NVME_CTRL_DELETING:
			changed = TRUE;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (!changed)
		return FALSE;

	ctrl->state = new_state;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Transfer limits                                                     */
/* ------------------------------------------------------------------ */

/*
 * nvme_mps_to_sectors - convert controller-page-size units to 512B sectors
 *
 * The NVMe spec expresses some size fields (MDTS, WZSL, etc.) as a power-of-2
 * multiplier of the minimum page size reported in CAP.MPSMIN.  This helper
 * converts that exponent to a number of 512-byte sectors, saturating to
 * UINT_MAX on overflow.
 *
 * @ctrl:  controller whose CAP.MPSMIN value to use
 * @units: field value (exponent)
 * Returns: size in 512B sectors, or UINT_MAX on overflow
 */
static inline u32 nvme_mps_to_sectors(struct NVMeController *ctrl, u32 units)
{
	u32 page_shift = NVME_CAP_MPSMIN(ctrl->cap) + 12, val;

	if (check_shl_overflow(1U, units + page_shift - 9, &val))
		return UINT_MAX;
	return val;
}

/*
 * nvme_set_limits - record per-controller transfer ceiling.
 *
 * Three inputs combine:
 *   1. Identify Controller MDTS field (passed in as @max_hw_sectors,
 *      in 512B units; UINT_MAX when id->mdts == 0).
 *   2. NVME_AMIGA_DEFAULT_MAX_BYTES — fallback for MDTS-unreported
 *      devices, matching Linux's spirit (Linux uses NVME_MAX_BYTES =
 *      8 MiB in pci.c).
 */
#define NVME_AMIGA_DEFAULT_MAX_BYTES (8UL * 1024UL * 1024UL)

static void nvme_set_limits(struct NVMeController *ctrl, u32 max_hw_sectors)
{
	u32 bytes;
	if (max_hw_sectors == 0 || max_hw_sectors == UINT_MAX)
		bytes = NVME_AMIGA_DEFAULT_MAX_BYTES;
	else
		bytes = max_hw_sectors << 9;

	ctrl->max_transfer_bytes = bytes;

	Kprintf("[nvme] %s: max_hw_sectors=%lu max_transfer_bytes=%lu\n",
		__func__, (ULONG)max_hw_sectors,
		(ULONG)ctrl->max_transfer_bytes);
}

/* ------------------------------------------------------------------ */
/* Command Effects Log                                                 */
/* ------------------------------------------------------------------ */

/*
 * nvme_init_effects_log - allocate an empty Effects Log placeholder for a CSI
 *
 * Used when the controller does not support the Commands Supported and Effects
 * log: returns a zeroed buffer that nvme_init_known_nvm_effects() then patches
 * with a fixed set of well-known effects.
 *
 * @ctrl: controller whose memory pool to allocate from
 * @csi:  Command Set Identifier (unused in this port; only NVME_CSI_NVM ever flows here)
 * @log:  out-parameter set to the allocated placeholder
 * Returns: 0 on success, negative errno on allocation failure
 */
static int nvme_init_effects_log(struct NVMeController *ctrl,
		u8 csi, struct nvme_effects_log **log)
{
	(void)csi;

	struct nvme_effects_log *effects = pool_zalloc(ctrl->memoryPool,
			sizeof(*effects));
	if (!effects)
		return -ENOMEM;

	*log = effects;
	return 0;
}

/*
 * nvme_init_known_nvm_effects - patch well-known effects into the NVM Effects Log
 *
 * Some controllers do not report the correct effects for Format NVM, Sanitize,
 * or Security Receive (or do not support the Effects Log at all).  This
 * function applies a fixed set of known-correct effects to the driver's cached
 * log so that nvme_passthru_start() applies the right locking regardless of
 * what the device reported.  Also forces LBCC on Write/Write-Zeroes/Write-
 * Uncorrectable since those commands always modify LBA content.
 *
 * @ctrl: controller whose effects log to patch
 */
static void nvme_init_known_nvm_effects(struct NVMeController *ctrl)
{
	struct nvme_effects_log	*log = ctrl->effects;

	log->acs[nvme_admin_format_nvm] |= le32(NVME_CMD_EFFECTS_LBCC |
						NVME_CMD_EFFECTS_NCC |
						NVME_CMD_EFFECTS_CSE_MASK);
	log->acs[nvme_admin_sanitize_nvm] |= le32(NVME_CMD_EFFECTS_LBCC |
						NVME_CMD_EFFECTS_CSE_MASK);

	/*
	 * The spec says the result of a security receive command depends on
	 * the previous security send command. As such, many vendors log this
	 * command as one to submitted only when no other commands to the same
	 * namespace are outstanding. The intention is to tell the host to
	 * prevent mixing security send and receive.
	 *
	 * This driver can only enforce such exclusive access against IO
	 * queues, though. We are not readily able to enforce such a rule for
	 * two commands to the admin queue, which is the only queue that
	 * matters for this command.
	 *
	 * Rather than blindly freezing the IO queues for this effect that
	 * doesn't even apply to IO, mask it off.
	 */
	log->acs[nvme_admin_security_recv] &= le32(~NVME_CMD_EFFECTS_CSE_MASK);

	log->iocs[nvme_cmd_write] |= le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_zeroes] |= le32(NVME_CMD_EFFECTS_LBCC);
	log->iocs[nvme_cmd_write_uncor] |= le32(NVME_CMD_EFFECTS_LBCC);
}

/*
 * nvme_get_effects_log - retrieve the Commands Supported and Effects log for a CSI
 *
 * Allocates a buffer from the controller's memory pool and issues Get Log Page
 * for NVME_LOG_CMD_EFFECTS.  Called once from nvme_init_effects() during
 * controller initialization; the caller stores the result on ctrl->effects and
 * gates re-entry on that pointer being non-NULL.
 *
 * @ctrl: controller to query
 * @csi:  Command Set Identifier (NVME_CSI_NVM in this port) for the log to fetch
 * @log:  out-parameter set to the freshly fetched effects log pointer
 * Returns: 0 on success, negative errno on allocation or command failure
 */
static int nvme_get_effects_log(struct NVMeController *ctrl, u8 csi,
				struct nvme_effects_log **log)
{
	struct nvme_effects_log *cel = pool_zalloc(ctrl->memoryPool,
			sizeof(*cel));
	if (!cel)
		return -ENOMEM;

	int ret = nvme_get_log(ctrl, 0x00, NVME_LOG_CMD_EFFECTS, 0, csi,
			cel, sizeof(*cel), 0, NULL, NULL);
	if (ret) {
		pool_free(ctrl->memoryPool, cel);
		return ret;
	}

	*log = cel;
	return 0;
}

/*
 * nvme_init_effects - initialise the NVM Command Effects Log for the controller
 *
 * Fetches the Effects Log if the controller supports it (LPA bit 1), or
 * allocates an empty placeholder if not.  In both cases, overlays known-correct
 * effects via nvme_init_known_nvm_effects().  Called once per controller from
 * nvme_init_identify() on first identification.
 *
 * @ctrl: controller to initialise effects for
 * @id:   Identify Controller data (used to check LPA bit)
 * Returns: 0 on success, negative errno on allocation or command failure
 */
static int nvme_init_effects(struct NVMeController *ctrl, struct nvme_id_ctrl *id)
{
	if (ctrl->effects)
		return 0;

	if (id->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) {
		int ret = nvme_get_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
	}

	if (!ctrl->effects) {
		int ret = nvme_init_effects_log(ctrl, NVME_CSI_NVM, &ctrl->effects);
		if (ret < 0)
			return ret;
	}

	nvme_init_known_nvm_effects(ctrl);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Identify Controller                                                 */
/* ------------------------------------------------------------------ */

/*
 * nvme_cache_id_strings - stash Identify Controller text fields.
 *
 * Called once by nvme_init_identify after the Identify Controller
 * (CNS=0x01) response is parsed.  The buffer lives in pool memory and
 * is freed when nvme_init_identify returns, so we copy out before that
 * happens.
 *
 * Strings are space-padded fixed-width per NVMe spec §5.15.2;
 * nvme_scsi.c handles trimming when populating the INQUIRY response.
 */
static void nvme_cache_id_strings(struct NVMeController *ctrl,
		const char *mn, const char *fr, const char *sn)
{
	CopyMem(mn, ctrl->id_strings.model, sizeof(ctrl->id_strings.model));
	CopyMem(fr, ctrl->id_strings.firmware, sizeof(ctrl->id_strings.firmware));
	CopyMem(sn, ctrl->id_strings.serial, sizeof(ctrl->id_strings.serial));
	Kprintf("[nvme] %s: model='%.40s' fw='%.8s' sn='%.20s'\n",
		__func__,
		ctrl->id_strings.model,
		ctrl->id_strings.firmware,
		ctrl->id_strings.serial);
}

/* default controller shutdown timeout (seconds) */
static unsigned char shutdown_timeout = 5;

/*
 * nvme_init_identify - populate controller capabilities from Identify Controller
 *
 * Issues nvme_identify_ctrl(), extracts all capability fields (MDTS, SGLS,
 * OACS, ONCS, OAES, RTD3E, etc.) into ctrl, updates the admin queue limits,
 * and on the first identification call initialises the effects log.  Called at
 * the start of every reset recovery as well as first-time init.
 *
 * @ctrl: controller to initialise
 * Returns: 0 on success, -EIO or negative errno on failure
 */
int nvme_init_identify(struct NVMeController *ctrl)
{
	struct nvme_id_ctrl *id;

	int ret = nvme_identify_ctrl(ctrl, &id);
	if (ret) {
		Kprintf("[nvme] %s: Identify Controller failed (%ld)\n", __func__, ret);
		return -EIO;
	}

	ctrl->cntlid = le16(id->cntlid);

	/* OR in quirks matched on the Identify Controller model/
	 * firmware strings (workarounds a shared PCI ID can't express). */
	ctrl->quirks |= nvme_match_id_quirks(id);

	if (!ctrl->identified) {
		ret = nvme_init_effects(ctrl, id);
		if (ret)
			goto out_free;
	}

	/* Cache model/firmware/serial on the Amiga controller for SCSI INQUIRY.
	 * Strings are space-padded fixed-width.  (The Linux original also stashes
	 * id->fr in ctrl->subsys->firmware_rev for the FW-Slot AEN handler, but
	 * this port has no nvme_subsystem — id_strings.firmware is the cache.) */
	nvme_cache_id_strings(ctrl, id->mn, id->fr, id->sn);

	ctrl->crdt[0] = le16(id->crdt1);
	ctrl->crdt[1] = le16(id->crdt2);
	ctrl->crdt[2] = le16(id->crdt3);

	ctrl->oacs = le16(id->oacs);
	ctrl->oncs = le16(id->oncs);
	ctrl->mtfa = le16(id->mtfa);
	ctrl->oaes = le32(id->oaes);
	ctrl->wctemp = le16(id->wctemp);
	ctrl->cctemp = le16(id->cctemp);

	ctrl->abort_limit = (u32)id->acl + 1;
	ctrl->vwc = id->vwc;

	u32 max_hw_sectors = id->mdts ? nvme_mps_to_sectors(ctrl, id->mdts)
				      : UINT_MAX;

	if (ctrl->max_hw_sectors == 0)
			ctrl->max_hw_sectors = max_hw_sectors;
	else
		ctrl->max_hw_sectors = (ctrl->max_hw_sectors < max_hw_sectors) ?
			ctrl->max_hw_sectors : max_hw_sectors;

	/* Propagate the MDTS ceiling to the Amiga side so ProcessCommand
	 * can split oversized BeginIO requests into device-acceptable
	 * chunks.  Without this the device returns status 0x02 (Invalid
	 * Field) for any read/write whose NLB exceeds MDTS. */
	nvme_set_limits(ctrl, ctrl->max_hw_sectors);

	ctrl->sgls = le32(id->sgls);
	ctrl->max_namespaces = le32(id->mnan);
	ctrl->ctratt = le32(id->ctratt);

	ctrl->cntrltype = id->cntrltype;

	if (id->rtd3e) {
		/* us -> s */
		u32 transition_time = le32(id->rtd3e) / USEC_PER_SEC;
		if (transition_time < shutdown_timeout)
			transition_time = shutdown_timeout;
		if (transition_time > 60U)
			transition_time = 60U;

		ctrl->shutdown_timeout = transition_time;

		if (ctrl->shutdown_timeout != shutdown_timeout)
			Kprintf("[nvme] %s: D3 entry latency set to %lu seconds\n",
					__func__, ctrl->shutdown_timeout);
	} else
		ctrl->shutdown_timeout = shutdown_timeout;

	ctrl->hmpre = le32(id->hmpre);
	ctrl->hmmin = le32(id->hmmin);
	ctrl->hmminds = le32(id->hmminds);
	ctrl->hmmaxd = le16(id->hmmaxd);

out_free:
	pool_free(ctrl->memoryPool, id);
	return ret;
}

/*
 * nvme_init_non_mdts_limits - read NVM CS-specific size limits from the controller
 *
 * Initialises max_zeroes_sectors, dmrl (Dataset Management Range Limit), and
 * dmrsl (Dataset Management Range Size Limit) from the Identify Controller
 * NVM Command Set (CNS=0x06) response.  Falls back to using max_hw_sectors for
 * write zeroes if the CS-specific identify is unsupported.  Sets the
 * NVME_CTRL_SKIP_ID_CNS_CS flag if the command fails so it is not retried.
 *
 * @ctrl: controller to query
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
int nvme_init_non_mdts_limits(struct NVMeController *ctrl)
{
	/*
	 * Even though NVMe spec explicitly states that MDTS is not applicable
	 * to the write-zeroes, we are cautious and limit the size to the
	 * controllers max_hw_sectors value, which is based on the MDTS field
	 * and possibly other limiting factors.
	 */
	if ((ctrl->oncs & NVME_CTRL_ONCS_WRITE_ZEROES) &&
	    !(ctrl->quirks & NVME_QUIRK_DISABLE_WRITE_ZEROES))
		ctrl->max_zeroes_sectors = ctrl->max_hw_sectors;
	else
		ctrl->max_zeroes_sectors = 0;

	if (!nvme_is_io_ctrl(ctrl) ||
	    !nvme_id_cns_ok(ctrl, NVME_ID_CNS_CS_CTRL) ||
	    test_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags))
		return 0;

	struct nvme_id_ctrl_nvm *id;

	int ret = nvme_identify_ctrl_nvm(ctrl, &id);
	if (ret) {
		if (ret > 0)	/* command rejected: don't retry next time */
			set_bit(NVME_CTRL_SKIP_ID_CNS_CS, &ctrl->flags);
		return ret;
	}

	ctrl->dmrl = id->dmrl;
	ctrl->dmrsl = le32(id->dmrsl);
	if (id->wzsl)
		ctrl->max_zeroes_sectors = nvme_mps_to_sectors(ctrl, id->wzsl);

	pool_free(ctrl->memoryPool, id);
	return 0;
}
