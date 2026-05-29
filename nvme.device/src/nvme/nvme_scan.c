// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_scan.c — namespace discovery, validation, and removal.
 *
 * Runs from nvme_scan_namespaces(), invoked at probe time from the foreign
 * init task and on AEN-driven rescans from AdminWorker (admin_task.c picks
 * up ctrl->scan_signal and calls nvme_scan_namespaces).  scan_lock
 * serialises both paths so we never have two scans walking ctrl->namespaces
 * concurrently.
 */

#include <types.h>
#include <errors.h>

#include <nvme/nvme_admin.h>
#include <nvme/nvme_ctrl.h>		/* nvme_change_ctrl_state, nvme_init_non_mdts_limits */
#include <nvme/nvme_identify.h> /* struct nvme_ns_info, nvme_identify_* */
#include <nvme/nvme_task.h>
#include <nvme/nvme_probe.h>	/* nvme_alloc_nvmeunit */
#include <nvme/nvme_queue.h>	/* nvme_unquiesce_io_queues */
#include <nvme/nvme_scan.h>

/*
 * nvme_find_ns - return the controller's nvme_ns for @nsid, or NULL.
 *
 * Linear walk of ctrl->namespaces.  Both callers hold ctrl->scan_lock,
 * so no synchronization is required; there is no refcount because the
 * scan path is the sole consumer of the returned pointer.
 */
static struct nvme_ns *nvme_find_ns(struct NVMeController *ctrl, u32 nsid)
{
	for (struct MinNode *node = ctrl->namespaces.mlh_Head;
		 node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct nvme_ns *ns = (struct nvme_ns *)node;
		if (ns->ns_id == nsid)
			return ns;
	}
	return NULL;
}

/*
 * nvme_ns_ids_equal - compare two namespace ID sets for equality
 *
 * Checks UUID, NGUID, EUI64, and CSI fields.  Used during namespace
 * revalidation to determine whether two nvme_ns_ids structures refer to the
 * same namespace.
 *
 * @a: first ID set
 * @b: second ID set
 * Returns: TRUE if all four identifiers match, FALSE otherwise
 */
static BOOL nvme_ns_ids_equal(struct nvme_ns_ids *a, struct nvme_ns_ids *b)
{
	return memcmp(&a->uuid, &b->uuid, NVME_NIDT_UUID_LEN) == 0 &&
		memcmp(&a->nguid, &b->nguid, sizeof(a->nguid)) == 0 &&
		memcmp(&a->eui64, &b->eui64, sizeof(a->eui64)) == 0 &&
		a->csi == b->csi;
}

/*
 * nvme_update_ns_info - commit gathered namespace info to the nvme_ns
 *
 * All Identify data was already fetched by nvme_identify_ns_info(), so this
 * issues no command: it copies the block geometry and caches the metadata/PI
 * characteristics onto @ns.  This driver presents plain logical blocks only, so
 * block capacity is exposed only for a usable LBA size (512 B..4 KiB) with no
 * metadata and no protection information; otherwise the namespace stays present
 * (READY) but reports zero capacity and won't accept block I/O.  Called from
 * nvme_alloc_ns() and nvme_validate_ns() during probe and rescan.
 *
 * @ns:   namespace to update
 * @info: namespace info gathered by nvme_identify_ns_info()
 */
static void nvme_update_ns_info(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	if (info->ids.csi != NVME_CSI_NVM) {
		Kprintf("[nvme] %s: block device for nsid %lu not supported (csi %lu)\n",
			__func__, info->nsid, info->ids.csi);
		set_bit(NVME_NS_READY, &ns->flags);
		return;
	}

	/* cache namespace geometry + metadata/PI characteristics */
	ns->lba_shift = info->lba_shift;
	ns->ms = info->ms;
	ns->pi_type = info->pi_type;
	ns->pic = info->pic;
	ns->elbaf = info->elbaf;
	ns->lbstm = info->lbstm;
	if (info->deac)
		ns->features |= NVME_NS_DEAC;

	if (info->ms == 0 && info->pi_type == 0 &&
	    ns->lba_shift >= 9 && ns->lba_shift <= 12)
		ns->disk_capacity_sectors = nvme_lba_to_sect(ns, info->nsze);
	else
		Kprintf("[nvme] %s: nsid %lu not a plain block ns "
			"(ms=%lu pi=%lu ds=%lu) - no block I/O\n",
			__func__, info->nsid, (ULONG)info->ms,
			(ULONG)info->pi_type, (ULONG)ns->lba_shift);

	set_bit(NVME_NS_READY, &ns->flags);
}

/*
 * nvme_alloc_ns - allocate and initialise a new NVMe namespace
 *
 * Pool-allocates an nvme_ns, applies geometry via nvme_update_ns_info, then
 * asks nvme_alloc_nvmeunit (src/nvme/nvme_probe.c) to create and link the
 * corresponding NVMeUnit so Amiga clients can OpenDevice it.  Finally
 * links the ns onto ctrl->namespaces.  Called from nvme_scan_ns() when a
 * previously-unseen NSID shows up.
 *
 * @ctrl: controller the namespace belongs to
 * @info: namespace info from the scan (nsid, ids, ...)
 */
static void nvme_alloc_ns(struct NVMeController *ctrl, struct nvme_ns_info *info)
{
	struct nvme_ns *ns = pool_zalloc(ctrl->memoryPool, sizeof(*ns));
	if (!ns)
		return;

	ns->ctrl = ctrl;
	ns->ns_id = info->nsid;
	ns->ids = info->ids;

	nvme_update_ns_info(ns, info);

	/* Create Unit for the namespace */
	ns->unit = nvme_alloc_nvmeunit(ctrl, ns->ns_id,
								   1u << ns->lba_shift,
								   ns->lba_shift,
								   ns->disk_capacity_sectors);

	AddTail((struct List *)&ctrl->namespaces, (struct Node *)&ns->mn_Node);
}

/*
 * nvme_ns_remove - tear down and unlink a namespace
 *
 * Sets NVME_NS_REMOVING to make re-entry a no-op, clears NVME_NS_READY,
 * unlinks the ns from ctrl->namespaces, then pool-frees it.  Called when
 * a scan finds the namespace gone, when validate detects an unrecoverable
 * error, or from nvme_remove_namespaces() during controller teardown.
 *
 * @ns: namespace to remove
 */
static void nvme_ns_remove(struct nvme_ns *ns)
{
	if (test_and_set_bit(NVME_NS_REMOVING, &ns->flags))
		return;

	clear_bit(NVME_NS_READY, &ns->flags);

	/* Tear down the matching NVMeUnit so OpenDevice can't reach it and any
	 * in-flight BeginIO returns TDERR_DiskChanged.  We don't pool_free the
	 * unit: existing OpenDevice holders still have the pointer, and there's
	 * no per-unit in-flight refcount to know when it's safe.  The pool is
	 * destroyed on controller expunge, which reclaims everything. */
	if (ns->unit)
	{
		struct NVMeUnit *unit = ns->unit;
		ns->unit = NULL;
		Forbid();
		unit->flags |= NVME_UNIT_DEAD; /* publish dead BEFORE unlink */
		Remove((struct Node *)unit);   /* off base->units */
		Permit();
	}

	Remove((struct Node *)&ns->mn_Node);
	pool_free(ns->ctrl->memoryPool, ns);
}

/*
 * nvme_ns_remove_by_nsid - find a namespace by NSID and remove it.
 *
 * Called from nvme_scan_ns() when the identify response indicates the
 * namespace is no longer present.
 */
static void nvme_ns_remove_by_nsid(struct NVMeController *ctrl, u32 nsid)
{
	struct nvme_ns *ns = nvme_find_ns(ctrl, nsid);

	if (ns)
		nvme_ns_remove(ns);
}

/*
 * nvme_validate_ns - re-check an existing namespace against a fresh scan
 *
 * Compares the cached ns->ids against the freshly-retrieved info; if they
 * changed (NSID re-used for a different namespace) the namespace is removed.
 * Otherwise refreshes geometry via nvme_update_ns_info().
 *
 * @ns:   namespace to validate
 * @info: fresh namespace info from the current scan
 */
static void nvme_validate_ns(struct nvme_ns *ns, struct nvme_ns_info *info)
{
	if (!nvme_ns_ids_equal(&ns->ids, &info->ids))
	{
		Kprintf("[nvme] %s: identifiers changed for nsid %ld\n", __func__, ns->ns_id);
		nvme_ns_remove(ns);
		return;
	}

	nvme_update_ns_info(ns, info);
}

/*
 * nvme_scan_ns - scan a single namespace ID and add or validate it
 *
 * Gathers all Identify data for nsid via nvme_identify_ns_info().  Removes the
 * namespace if it is gone, skips it if not ready, and either validates the
 * existing namespace or allocates a new one.  Called from nvme_scan_ns_list()
 * per active-NSID and from nvme_scan_ns_sequential() per 1..NN.
 *
 * @ctrl: controller to scan
 * @nsid: namespace ID to probe
 */
static void nvme_scan_ns(struct NVMeController *ctrl, unsigned nsid)
{
	struct nvme_ns_info info = {.nsid = nsid};

	int ret = nvme_identify_ns_info(ctrl, &info);

	if (info.is_removed)
		nvme_ns_remove_by_nsid(ctrl, nsid);

	/*
	 * Ignore the namespace if the gather failed or it is not ready.  We will
	 * get an AEN once it becomes ready and restart the scan.
	 */
	if (ret || !info.is_ready)
		return;

	struct nvme_ns *ns = nvme_find_ns(ctrl, nsid);
	if (ns)
		nvme_validate_ns(ns, &info);
	else
		nvme_alloc_ns(ctrl, &info);
}

/*
 * nvme_remove_invalid_namespaces - remove namespaces with IDs beyond the current maximum
 *
 * After a scan completes, any namespace whose ns_id is greater than the highest
 * NSID seen in the scan result is stale and must be removed.  This handles the
 * case where namespaces were deleted while the scan was running.  Called at the
 * end of nvme_scan_ns_list() and nvme_scan_ns_sequential().
 *
 * @ctrl: controller whose namespace list to clean up
 * @nsid: highest valid namespace ID seen during the scan
 */
static void nvme_remove_invalid_namespaces(struct NVMeController *ctrl,
										   unsigned nsid)
{
	struct MinNode *node = ctrl->namespaces.mlh_Head;
	struct MinNode *next;

	while ((next = node->mln_Succ) != NULL)
	{
		struct nvme_ns *ns = (struct nvme_ns *)node;
		node = next;
		if (ns->ns_id > nsid)
			nvme_ns_remove(ns);
	}
}

/*
 * nvme_scan_ns_list - scan all active namespaces using Identify Active NS List
 *
 * Issues Identify (CNS=0x02) in a loop to page through the active namespace
 * list in batches of 1024 entries.  For each NSID returned, calls
 * nvme_scan_ns() synchronously; any NSIDs between the previous and current
 * entry are treated as gone and removed.  After the last page,
 * nvme_remove_invalid_namespaces() prunes anything above the highest seen
 * NSID.  Called from nvme_scan_namespaces().
 *
 * @ctrl: controller to scan
 * Returns: 0 on success, negative errno or positive NVMe status on failure
 */
static int nvme_scan_ns_list(struct NVMeController *ctrl)
{
	const int nr_entries = NVME_IDENTIFY_DATA_SIZE / sizeof(__le32);
	int ret = 0;

	__le32 *ns_list = pool_zalloc(ctrl->memoryPool, NVME_IDENTIFY_DATA_SIZE);
	if (!ns_list)
		return -ENOMEM;

	u32 prev = 0;
	for (;;)
	{
		struct nvme_command cmd = {
			.identify.opcode = nvme_admin_identify,
			.identify.cns = NVME_ID_CNS_NS_ACTIVE_LIST,
			.identify.nsid = le32(prev),
		};

		ret = nvme_submit_sync_cmd(ctrl, &cmd, NULL, ns_list, NVME_IDENTIFY_DATA_SIZE);
		if (ret)
		{
			Kprintf("[nvme] %s: Identify NS List failed (status=0x%lx)\n", __func__, ret);
			goto free;
		}

		for (int i = 0; i < nr_entries; i++)
		{
			u32 nsid = le32(ns_list[i]);
			if (!nsid) /* end of the list? */
				goto out;

			nvme_scan_ns(ctrl, nsid);

			while (++prev < nsid)
				nvme_ns_remove_by_nsid(ctrl, prev);
		}
	}
out:
	nvme_remove_invalid_namespaces(ctrl, prev);
free:
	pool_free(ctrl->memoryPool, ns_list);
	return ret;
}

/*
 * nvme_scan_ns_sequential - scan namespaces 1..NN sequentially
 *
 * Used as a fallback when Identify Active NS List is not supported.  Reads NN
 * from the Identify Controller response and probes each NSID from 1 to NN in
 * order.  Removes any NSIDs above the new NN.  Called from nvme_scan_namespaces()
 * when the controller is too old to support CNS=0x02.
 *
 * @ctrl: controller to scan
 */
static void nvme_scan_ns_sequential(struct NVMeController *ctrl)
{
	struct nvme_id_ctrl *id;

	if (nvme_identify_ctrl(ctrl, &id))
		return;
	u32 nn = le32(id->nn);
	pool_free(ctrl->memoryPool, id);

	for (u32 i = 1; i <= nn; i++)
		nvme_scan_ns(ctrl, i);

	nvme_remove_invalid_namespaces(ctrl, nn);
}

/*
 * nvme_clear_changed_ns_log - drain the Changed Namespace List log to clear the AEN
 *
 * NVMe requires reading the Changed Namespace List log page to acknowledge the
 * AEN.  We read it here but intentionally discard the content — user space
 * may have already read it, and we do a full rescan anyway.  Called from
 * nvme_scan_namespaces() when NVME_AER_NOTICE_NS_CHANGED is set.
 *
 * @ctrl: controller whose Changed Namespace List log to clear
 */
static void nvme_clear_changed_ns_log(struct NVMeController *ctrl)
{
	size_t log_size = NVME_MAX_CHANGED_NAMESPACES * sizeof(__le32);
	int error;

	__le32 *log = pool_zalloc(ctrl->memoryPool, log_size);
	if (!log)
		return;

	/*
	 * We need to read the log to clear the AEN, but we don't want to rely
	 * on it for the changed namespace information as userspace could have
	 * raced with us in reading the log page, which could cause us to miss
	 * updates.
	 */
	error = nvme_get_log(ctrl, NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0,
						 NVME_CSI_NVM, log, log_size, 0, NULL, NULL);
	if (error)
		Kprintf("[nvme] %s: reading changed ns log failed: %ld\n", __func__, error);

	pool_free(ctrl->memoryPool, log);
}

/*
 * nvme_scan_namespaces - (re)scan the controller's active namespaces
 *
 * Refreshes non-MDTS limits, clears the changed-NS AEN log if set, then
 * walks the namespace list (Identify Active-NS-List if supported, otherwise
 * sequential 1..NN).  Requeues itself via nvme_queue_scan() if another
 * namespace-changed AEN arrived while the scan was in progress.
 *
 * Triggered by nvme_queue_scan() from AEN handling and post-reset.  MUST
 * run on AdminWorker (or the foreign init task at probe time) and NEVER on
 * ctrl->unit_task — the helpers below issue nvme_submit_sync_cmd, which
 * would self-deadlock on the unit task.
 */
void nvme_scan_namespaces(struct NVMeController *ctrl)
{
	if (nvme_ctrl_state(ctrl) != NVME_CTRL_LIVE)
		return;

	ObtainSemaphore(&ctrl->scan_lock);

	int ret = nvme_init_non_mdts_limits(ctrl);
	if (ret < 0)
	{
		Kprintf("[nvme] %s: reading non-mdts-limits failed: %ld\n", __func__, ret);
		goto out;
	}

	if (test_and_clear_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events))
	{
		Kprintf("[nvme] %s: rescanning namespaces.\n", __func__);
		nvme_clear_changed_ns_log(ctrl);
	}

	if (!nvme_id_cns_ok(ctrl, NVME_ID_CNS_NS_ACTIVE_LIST))
	{
		nvme_scan_ns_sequential(ctrl);
	}
	else
	{
		/* Fall back to sequential scan if DNR is set to handle broken
		 * devices that claim Identify NS List support but don't. */
		ret = nvme_scan_ns_list(ctrl);
		if (ret > 0 && ret & NVME_STATUS_DNR)
			nvme_scan_ns_sequential(ctrl);
	}

	/* Requeue if we missed an AEN during the scan */
	if (test_bit(NVME_AER_NOTICE_NS_CHANGED, &ctrl->events))
		nvme_queue_scan(ctrl);

out:
	ReleaseSemaphore(&ctrl->scan_lock);
}

/*
 * nvme_remove_namespaces - tear down and remove all namespaces on a controller
 *
 * Unquiesces I/O queues (to let any in-flight scan-spawned I/O complete),
 * transitions to DELETING_NOIO, then walks the namespace list and calls
 * nvme_ns_remove() on each entry.  The caller must ensure the scan path is
 * not running concurrently (e.g. by having already called nvme_stop_ctrl).
 *
 * @ctrl: controller whose namespaces to remove
 */
void nvme_remove_namespaces(struct NVMeController *ctrl)
{
	struct MinNode *node, *next;

	nvme_unquiesce_io_queues(ctrl);

	nvme_change_ctrl_state(ctrl, NVME_CTRL_DELETING_NOIO);

	node = ctrl->namespaces.mlh_Head;
	while ((next = node->mln_Succ) != NULL)
	{
		struct nvme_ns *ns = (struct nvme_ns *)node;
		node = next; /* advance BEFORE remove */
		nvme_ns_remove(ns);
	}
}
