// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_identify.c — Identify Controller / Identify Namespace path.
 *
 * Issues the various Identify admin commands and parses the responses
 * into the driver's nvme_ns_info / nvme_id_ctrl structures.
 */
#include <nvme/nvme_core.h> /* must come first: kcompat.h establishes proto/exec.h
                          * decls before emu68-common's memory.h pool inlines */

#include <errors.h>

#include <device.h>
#include <nvme/nvme_admin.h> /* nvme_submit_sync_cmd */
#include <nvme/nvme_identify.h>

/*
 * nvme_submit_identify - allocate a buffer and submit a prepared Identify command
 *
 * Pool-allocates a zeroed @len-byte response buffer, submits the caller-built
 * Identify command synchronously, and hands the buffer back via @out on
 * success.  On failure the buffer is freed and the error returned, so callers
 * never have to clean up on the error path.  Shared by every Identify issuer
 * in this file.
 *
 * @ctrl: controller to query
 * @c:    fully-built Identify command (opcode/cns/nsid/csi already set)
 * @len:  size of the response buffer to allocate
 * @out:  receives the pool-allocated buffer on success (caller pool_free()s it)
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_submit_identify(struct NVMeController *ctrl,
								struct nvme_command *c, u32 len, void **out)
{
	void *buf = pool_zalloc(ctrl->memoryPool, len);
	if (!buf)
		return -ENOMEM;

	int ret = nvme_submit_sync_cmd(ctrl, c, NULL, buf, len);
	if (ret)
	{
		pool_free(ctrl->memoryPool, buf);
		return ret;
	}

	*out = buf;
	return 0;
}

/*
 * nvme_identify_ctrl - issue an Identify Controller admin command
 *
 * Submits an Identify with CNS=0x01 and returns the result.  The caller is
 * responsible for pool_free()ing *id on success.  Called during controller
 * initialization to discover capabilities, firmware revision, vendor IDs, and
 * supported features.
 *
 * @dev: controller to identify
 * @id:  out-parameter filled with a pool-allocated Identify Controller buffer
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
int nvme_identify_ctrl(struct NVMeController *dev, struct nvme_id_ctrl **id)
{
	struct nvme_command c = {};

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify;
	c.identify.cns = NVME_ID_CNS_CTRL;

	return nvme_submit_identify(dev, &c, sizeof(struct nvme_id_ctrl),
								(void **)id);
}

/*
 * nvme_identify_ctrl_nvm - issue an Identify Controller for the NVM Command Set
 *
 * Submits an Identify with CNS=0x06 and CSI=0x00 to retrieve NVM-Command-Set
 * controller limits (write-zeroes max, Dataset Management ranges).  The caller
 * is responsible for pool_free()ing *id on success.
 *
 * @ctrl: controller to identify
 * @id:   out-parameter filled with a pool-allocated NVM Identify Controller buffer
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
int nvme_identify_ctrl_nvm(struct NVMeController *ctrl, struct nvme_id_ctrl_nvm **id)
{
	struct nvme_command c = {
		.identify.opcode = nvme_admin_identify,
		.identify.cns = NVME_ID_CNS_CS_CTRL,
		.identify.csi = NVME_CSI_NVM,
	};

	return nvme_submit_identify(ctrl, &c, sizeof(**id), (void **)id);
}

/*
 * nvme_process_ns_desc - parse a single Namespace Identification Descriptor
 *
 * Validates the descriptor length, then copies the EUI64, NGUID, UUID, or CSI
 * value into the appropriate ids field.  Bogus-NID-quirked controllers have
 * their identifiers skipped.  Returns the length of the descriptor payload so
 * the caller can advance to the next descriptor in the list.
 *
 * @ctrl:     controller (for dev_warn and quirk checks)
 * @ids:      ns_ids structure to fill in
 * @cur:      pointer to the current descriptor entry in the buffer
 * @csi_seen: set to TRUE when a CSI descriptor is processed
 * Returns: payload length in bytes on success, -1 if the descriptor is invalid
 */
static int nvme_process_ns_desc(struct NVMeController *ctrl, struct nvme_ns_ids *ids,
								struct nvme_ns_id_desc *cur, BOOL *csi_seen)
{
	const char *warn_str = "ctrl returned bogus length:";
	void *data = cur;

	switch (cur->nidt)
	{
	case NVME_NIDT_EUI64:
		if (cur->nidl != NVME_NIDT_EUI64_LEN)
		{
			Kprintf("[nvme] %s: %s %ld for NVME_NIDT_EUI64\n",
					__func__, warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_EUI64_LEN;
		CopyMem(data + sizeof(*cur), ids->eui64, NVME_NIDT_EUI64_LEN);
		return NVME_NIDT_EUI64_LEN;
	case NVME_NIDT_NGUID:
		if (cur->nidl != NVME_NIDT_NGUID_LEN)
		{
			Kprintf("[nvme] %s: %s %ld for NVME_NIDT_NGUID\n",
					__func__, warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_NGUID_LEN;
		CopyMem(data + sizeof(*cur), ids->nguid, NVME_NIDT_NGUID_LEN);
		return NVME_NIDT_NGUID_LEN;
	case NVME_NIDT_UUID:
		if (cur->nidl != NVME_NIDT_UUID_LEN)
		{
			Kprintf("[nvme] %s: %s %ld for NVME_NIDT_UUID\n",
					__func__, warn_str, cur->nidl);
			return -1;
		}
		if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
			return NVME_NIDT_UUID_LEN;
		CopyMem(data + sizeof(*cur), &ids->uuid, NVME_NIDT_UUID_LEN);
		return NVME_NIDT_UUID_LEN;
	case NVME_NIDT_CSI:
		if (cur->nidl != NVME_NIDT_CSI_LEN)
		{
			Kprintf("[nvme] %s: %s %ld for NVME_NIDT_CSI\n",
					__func__, warn_str, cur->nidl);
			return -1;
		}
		CopyMem(data + sizeof(*cur), &ids->csi, NVME_NIDT_CSI_LEN);
		*csi_seen = TRUE;
		return NVME_NIDT_CSI_LEN;
	default:
		/* Skip unknown types */
		return cur->nidl;
	}
}

/*
 * nvme_identify_ns_descs - fetch and parse the Namespace ID Descriptor list
 *
 * Issues an Identify with CNS=0x03 to retrieve EUI64/NGUID/UUID/CSI for the
 * namespace.  Skipped for controllers older than NVMe 1.3 that don't support
 * multiple command sets and for controllers with the NO_NS_DESC_LIST quirk.
 * Logs a warning if a multi-CSS controller doesn't report a CSI descriptor.
 *
 * @ctrl: controller to query
 * @info: ns_info struct whose ids and csi fields are populated on success
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_identify_ns_descs(struct NVMeController *ctrl,
								  struct nvme_ns_info *info)
{
	struct nvme_command c = {};
	BOOL csi_seen = FALSE;

	if (ctrl->vs < NVME_VS(1, 3, 0) && !nvme_multi_css(ctrl))
		return 0;
	if (ctrl->quirks & NVME_QUIRK_NO_NS_DESC_LIST)
		return 0;

	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = le32(info->nsid);
	c.identify.cns = NVME_ID_CNS_NS_DESC_LIST;

	void *data;
	int status = nvme_submit_identify(ctrl, &c, NVME_IDENTIFY_DATA_SIZE, &data);
	if (status)
	{
		Kprintf("[nvme] %s: Identify Descriptors failed (nsid=%lu, status=0x%lx (%s))\n",
				__func__, info->nsid, status, nvme_get_error_status_str((u16)status));
		return status;
	}

	for (int pos = 0, len = 0; pos < NVME_IDENTIFY_DATA_SIZE; pos += len)
	{
		struct nvme_ns_id_desc *cur = data + pos;

		if (cur->nidl == 0)
			break;

		len = nvme_process_ns_desc(ctrl, &info->ids, cur, &csi_seen);
		if (len < 0)
			break;

		len += (int)sizeof(*cur);
	}

	if (nvme_multi_css(ctrl) && !csi_seen)
	{
		Kprintf("[nvme] %s: Command set not reported for nsid:%ld\n",
				__func__, info->nsid);
		status = -EINVAL;
	}

	pool_free(ctrl->memoryPool, data);
	return status;
}

/*
 * nvme_identify_ns - issue an Identify Namespace admin command
 *
 * Issues Identify with CNS=0x00 for the given namespace ID and returns the
 * result.  On success *id points to a pool-allocated buffer the caller must
 * pool_free().  Used during namespace initialization to read LBA format,
 * capacity, and feature fields.
 *
 * @ctrl:  controller to query
 * @nsid:  namespace ID to identify
 * @id:    out-parameter receiving a pool-allocated Identify Namespace buffer
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_identify_ns(struct NVMeController *ctrl, unsigned nsid,
							struct nvme_id_ns **id)
{
	struct nvme_command c = {};

	/* gcc-4.4.4 (at least) has issues with initializers and anon unions */
	c.identify.opcode = nvme_admin_identify;
	c.identify.nsid = le32(nsid);
	c.identify.cns = NVME_ID_CNS_NS;

	int error = nvme_submit_identify(ctrl, &c, sizeof(**id), (void **)id);
	if (error)
		Kprintf("[nvme] %s: Identify namespace failed (nsid=%lu, status=0x%lx (%s))\n",
				__func__, nsid, error, nvme_get_error_status_str((u16)error));
	return error;
}

/*
 * nvme_identify_ns_nvm - fetch NVM-Command-Set namespace metadata/PI info
 *
 * Issues an Identify with CNS=0x05 (I/O Command Set specific) and CSI=0x00
 * (NVM) and caches the active LBA format's extended-format word plus the
 * namespace protection-info capabilities and storage-tag mask into @info.
 * Only meaningful on controllers advertising NVME_CTRL_ATTR_ELBAS.
 *
 * @ctrl: controller to query
 * @nsid: namespace ID to identify
 * @lbaf: active LBA format index (selects the elbaf entry)
 * @info: ns_info whose elbaf/pic/lbstm fields are populated on success
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_identify_ns_nvm(struct NVMeController *ctrl, unsigned nsid,
								unsigned lbaf, struct nvme_ns_info *info)
{
	struct nvme_command c = {
		.identify.opcode = nvme_admin_identify,
		.identify.nsid = le32(nsid),
		.identify.cns = NVME_ID_CNS_CS_NS,
		.identify.csi = NVME_CSI_NVM,
	};
	struct nvme_id_ns_nvm *nvm;

	int ret = nvme_submit_identify(ctrl, &c, sizeof(*nvm), (void **)&nvm);
	if (ret)
		return ret;

	info->elbaf = le32(nvm->elbaf[lbaf]);
	info->pic = nvm->pic;
	info->lbstm = le64(nvm->lbstm);

	pool_free(ctrl->memoryPool, nvm);
	return 0;
}

/*
 * nvme_ns_info_from_identify - populate ns_info from an Identify Namespace response
 *
 * Issues a single Identify Namespace and extracts everything the scan path
 * needs from it: block geometry (lba_shift, size), metadata/PI characteristics
 * of the active LBA format, capacity-zero (absent) detection, ANA group,
 * shared/readonly flags, and EUI64/NGUID identifiers.  On ELBAS controllers it
 * additionally fetches the NVM-Command-Set identify to cache the extended LBA
 * format details.  This is the only place the full Identify Namespace is read.
 *
 * @ctrl: controller to query
 * @info: ns_info struct to fill; info->nsid must already be set
 * Returns: 0 on success, -ENODEV if namespace is not present, or other error
 */
static int nvme_ns_info_from_identify(struct NVMeController *ctrl,
									  struct nvme_ns_info *info)
{
	struct nvme_id_ns *id;

	int ret = nvme_identify_ns(ctrl, info->nsid, &id);
	if (ret)
		return ret;

	if (id->ncap == 0)
	{
		/* namespace not allocated or attached */
		info->is_removed = TRUE;
		ret = -ENODEV;
		goto error;
	}

	info->anagrpid = id->anagrpid;
	info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
	info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
	info->is_ready = TRUE;
	info->endgid = le16(id->endgid);

	/* block geometry + metadata/PI of the active LBA format */
	unsigned lbaf = nvme_lbaf_index(id->flbas);
	info->lba_shift = id->lbaf[lbaf].ds;
	info->nsze = le64(id->nsze);
	info->ms = le16(id->lbaf[lbaf].ms);
	info->pi_type = id->dps & NVME_NS_DPS_PI_MASK;
	info->deac = (id->dlfeat & 0x7) == 0x1 && (id->dlfeat & (1 << 3));

	if (ctrl->quirks & NVME_QUIRK_BOGUS_NID)
	{
		Kprintf("[nvme] %s: controller has bogus NID quirk, skipping identifiers\n",
				__func__);
	}
	else
	{
		struct nvme_ns_ids *ids = &info->ids;

		if (ctrl->vs >= NVME_VS(1, 1, 0) &&
			!memchr_inv(ids->eui64, 0, sizeof(ids->eui64)))
			CopyMem(id->eui64, ids->eui64, sizeof(ids->eui64));
		if (ctrl->vs >= NVME_VS(1, 2, 0) &&
			!memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
			CopyMem(id->nguid, ids->nguid, sizeof(ids->nguid));
	}

	if (ctrl->ctratt & NVME_CTRL_ATTR_ELBAS)
	{
		ret = nvme_identify_ns_nvm(ctrl, info->nsid, lbaf, info);
		if (ret > 0)
			ret = 0; /* unsupported: leave elbaf cache zeroed */
	}

error:
	pool_free(ctrl->memoryPool, id);
	return ret;
}

/*
 * nvme_ns_info_from_id_cs_indep - populate ns_info from Identify NS CS-Independent
 *
 * Issues an Identify Namespace (CNS=0x08, Command Set Independent) command and
 * extracts ANA group, shared/readonly/ready flags, rotational hint, VWC
 * absence flag, and endpoint group ID.  Preferred over nvme_ns_info_from_identify()
 * on controllers that support it because the CS-independent descriptor contains
 * more accurate readiness information.
 *
 * @ctrl: controller to query
 * @info: ns_info struct to fill; info->nsid must already be set
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_ns_info_from_id_cs_indep(struct NVMeController *ctrl,
										 struct nvme_ns_info *info)
{
	struct nvme_command c = {
		.identify.opcode = nvme_admin_identify,
		.identify.nsid = le32(info->nsid),
		.identify.cns = NVME_ID_CNS_NS_CS_INDEP,
	};
	struct nvme_id_ns_cs_indep *id;

	int ret = nvme_submit_identify(ctrl, &c, sizeof(*id), (void **)&id);
	if (ret)
		return ret;

	info->anagrpid = id->anagrpid;
	info->is_shared = id->nmic & NVME_NS_NMIC_SHARED;
	info->is_readonly = id->nsattr & NVME_NS_ATTR_RO;
	info->is_ready = id->nstat & NVME_NSTAT_NRDY;
	info->is_rotational = id->nsfeat & NVME_NS_ROTATIONAL;
	info->no_vwc = id->nsfeat & NVME_NS_VWC_NOT_PRESENT;
	info->endgid = le16(id->endgid);

	pool_free(ctrl->memoryPool, id);
	return 0;
}

/*
 * nvme_identify_ns_info - gather everything the scan path needs for one nsid
 *
 * Fetches the Namespace ID Descriptor list, then a single Identify Namespace
 * (geometry + metadata/PI + baseline readiness + absent-namespace detection),
 * then — on controllers that support it — the Command-Set Independent Identify
 * to refine the readiness fields.  Centralises the policy that used to live in
 * nvme_scan_ns().
 *
 * @ctrl: controller to query
 * @info: ns_info to fill; info->nsid must already be set
 * Returns: 0 on success; non-zero means "skip this namespace"
 */
int nvme_identify_ns_info(struct NVMeController *ctrl, struct nvme_ns_info *info)
{
	int ret = nvme_identify_ns_descs(ctrl, info);
	if (ret)
		return ret;

	if (info->ids.csi != NVME_CSI_NVM && !nvme_multi_css(ctrl))
	{
		Kprintf("[nvme] %s: command set not reported for nsid: %ld\n",
				__func__, info->nsid);
		return -EINVAL;
	}

	/*
	 * A single Identify Namespace supplies block geometry, metadata/PI,
	 * baseline readiness/identifiers, and detects an absent namespace.
	 */
	ret = nvme_ns_info_from_identify(ctrl, info);
	if (ret)
		return ret;

	/*
	 * Where available, the Command Set Independent Identify Namespace carries
	 * more accurate readiness information, so use it to override the baseline.
	 * A positive NVMe status means "unsupported" — keep the baseline; a
	 * negative errno is fatal.
	 */
	if ((ctrl->cap & NVME_CAP_CRMS_CRIMS) ||
		(info->ids.csi != NVME_CSI_NVM) ||
		ctrl->vs >= NVME_VS(2, 0, 0))
	{
		ret = nvme_ns_info_from_id_cs_indep(ctrl, info);
		if (ret < 0)
			return ret;
	}

	return 0;
}
