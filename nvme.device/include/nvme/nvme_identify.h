// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_IDENTIFY_H
#define NVME_IDENTIFY_H

#include <nvme/nvme_core.h>     /* integer types, BOOL, nvme_defs.h (NVME_VS, __le32) */
#include <nvme/nvme_ctrl.h>     /* struct NVMeController, enum nvme_quirks (nvme_id_cns_ok) */

struct NVMeUnit;

/*
 * Container structure for uniqueue namespace identifiers.
 */
struct nvme_ns_ids {
	u8	eui64[8];
	u8	nguid[16];
	u8	uuid[16];
	u8	csi;
};

enum nvme_ns_features {
	NVME_NS_DEAC = 1 << 2,		/* DEAC bit in Write Zeroes supported */
};

struct nvme_ns {
	struct MinNode mn_Node;

	struct NVMeController *ctrl;
	u32 ns_id;
	u8 lba_shift;
	u64 disk_capacity_sectors;
	struct NVMeUnit *unit;

	/* metadata / end-to-end-protection characteristics of the active LBA
	 * format, cached from Identify Namespace (+ Identify NS NVM under ELBAS)
	 * for future metadata/PI support */
	u16 ms;			/* metadata bytes per LBA			*/
	u8 pi_type;		/* protection-information type (0 = none)	*/
	u8 pic;			/* protection-information capabilities (ELBAS)	*/
	u32 elbaf;		/* active extended LBA format word (ELBAS)	*/
	u64 lbstm;		/* LBA storage-tag mask (ELBAS)			*/

	struct nvme_ns_ids ids;
	enum nvme_ns_features features;

	struct nvme_effects_log *effects;   /* Stage 8 stub */

	unsigned long flags;
#define NVME_NS_REMOVING		0
#define NVME_NS_READY			4
};

/*
 * struct nvme_ns_info - namespace-info descriptor passed between the
 * identify builders here and the scan path in nvme_scan.c.
 */
struct nvme_ns_info {
	struct nvme_ns_ids ids;
	u32 nsid;		/* namespace identifier being scanned */
	__le32 anagrpid;	/* ANA group id reported by Identify Namespace */
	u16 endgid;		/* endurance group id for this namespace */
	u64 runs;		/* optimal I/O boundary / preferred run length in LBAs */
	BOOL is_shared;		/* namespace is attached to multiple controllers */
	BOOL is_readonly;	/* namespace currently rejects writes */
	BOOL is_ready;		/* namespace is usable for normal I/O */
	BOOL is_removed;	/* namespace is absent or has been detached */
	BOOL is_rotational;	/* backing media is rotational rather than solid-state */
	BOOL no_vwc;		/* writes should assume no volatile write cache */

	/* block geometry + metadata/PI of the active LBA format, gathered from
	 * the single Identify Namespace fetch (and Identify NS NVM under ELBAS) */
	u8 lba_shift;		/* id->lbaf[lbaf].ds			*/
	u64 nsze;		/* raw namespace size in LBAs (host order) */
	BOOL deac;		/* DEAC: deallocated blocks read back zero */
	u16 ms;			/* id->lbaf[lbaf].ms — metadata bytes/LBA	*/
	u8 pi_type;		/* id->dps & NVME_NS_DPS_PI_MASK		*/
	u8 pic;			/* nvm->pic        (ELBAS only)		*/
	u32 elbaf;		/* nvm->elbaf[lbaf] (ELBAS only, active fmt) */
	u64 lbstm;		/* nvm->lbstm      (ELBAS only)		*/
};

/*
 * nvme_id_cns_ok - check whether a CNS value is safe to use with this controller
 *
 * Older NVMe specs used fewer bits for the CNS field; using a value outside
 * the supported range may be misinterpreted by the device.  Also works around
 * a QEMU quirk where CNS > 3 is incorrectly truncated on 1.1-claiming
 * controllers.  Called before issuing Identify commands that use extended CNS
 * values.
 *
 * @ctrl: controller to check
 * @cns:  Identify CNS value to validate
 * Returns: TRUE if the controller can handle this CNS value safely
 */
static inline BOOL nvme_id_cns_ok(struct NVMeController *ctrl, u8 cns)
{
	/*
	 * The CNS field occupies a full byte starting with NVMe 1.2
	 */
	if (ctrl->vs >= NVME_VS(1, 2, 0))
		return TRUE;

	/*
	 * NVMe 1.1 expanded the CNS value to two bits, which means values
	 * larger than that could get truncated and treated as an incorrect
	 * value.
	 *
	 * Qemu implemented 1.0 behavior for controllers claiming 1.1
	 * compliance, so they need to be quirked here.
	 */
	if (ctrl->vs >= NVME_VS(1, 1, 0) &&
	    !(ctrl->quirks & NVME_QUIRK_IDENTIFY_CNS))
		return cns <= 3;

	/*
	 * NVMe 1.0 used a single bit for the CNS value.
	 */
	return cns <= 1;
}

/*
 * nvme_identify_ctrl - issue an Identify Controller admin command.
 * On success *id points to a pool-allocated buffer the caller must
 * pool_free().
 */
int nvme_identify_ctrl(struct NVMeController *dev, struct nvme_id_ctrl **id);

/*
 * nvme_identify_ctrl_nvm - issue an Identify Controller for the NVM Command
 * Set (CNS=0x06).  On success *id points to a pool-allocated buffer the caller
 * must pool_free().
 */
int nvme_identify_ctrl_nvm(struct NVMeController *ctrl, struct nvme_id_ctrl_nvm **id);

/*
 * nvme_identify_ns_info - gather everything the scan path needs for one nsid.
 *
 * Issues the Namespace ID Descriptor list, a single Identify Namespace (block
 * geometry + metadata/PI + baseline readiness + absent-namespace detection),
 * and — on controllers that support it — the Command-Set Independent Identify
 * to refine the readiness fields.  Populates @info.
 *
 * @ctrl: controller to query
 * @info: ns_info to fill; info->nsid must already be set
 * Returns: 0 on success, negative errno or positive NVMe status otherwise
 *          (non-zero means "skip this namespace").
 */
int nvme_identify_ns_info(struct NVMeController *ctrl,
		struct nvme_ns_info *info);

#endif /* NVME_IDENTIFY_H */
