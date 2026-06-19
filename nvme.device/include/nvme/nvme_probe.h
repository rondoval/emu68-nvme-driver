/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NVME_PROBE_H
#define NVME_PROBE_H

#include <nvme/nvme_core.h>     /* s32, ULONG, u8, u64 */

struct NVMeDevice;
struct NVMeController;
struct NVMeUnit;

/* ------------------------------------------------------------------ */
/* Device-level probe / unprobe                                        */
/* ------------------------------------------------------------------ */

/*
 * nvme_probe_all - enumerate all NVMe PCIe controllers and build the unit
 * map.  Must be called exactly once before any UnitOpen.
 *
 * Returns 0 if at least one controller was found, -1 otherwise.
 */
s32 nvme_probe_all(struct NVMeDevice *base);

/*
 * nvme_unprobe_all - tear down every controller / unit allocated by
 * nvme_probe_all().  Called from the device expunge path.
 */
void nvme_unprobe_all(struct NVMeDevice *base);

/*
 * nvme_reset_quiesce_all - minimal pre-reset shutdown of every controller
 * (CC.SHN handshake only).  Interrupt-safe; called from the reset_guard
 * prepare callback.
 */
void nvme_reset_quiesce_all(struct NVMeDevice *base);

/* ------------------------------------------------------------------ */
/* Per-controller lifecycle                                            */
/* ------------------------------------------------------------------ */

/*
 * nvme_reset_controller - run the bring-up sequence on an already-
 * allocated controller.  Called from the unit task in response to a
 * reset_signal raised by nvme_reset_ctrl().
 */
int nvme_reset_ctrl(struct NVMeController *ctrl);
void nvme_reset_controller(struct NVMeController *ctrl);

/*
 * nvme_reset_finish - terminal step of the asynchronous reset bring-up.
 *
 * Called from the I/O-queue async chain (nvme_queue.c) once Create I/O SQ
 * completes (@ok = TRUE) or any bring-up step fails (@ok = FALSE).  On
 * success: CONNECTING -> LIVE, arm AEN + unquiesce I/O, queue a rescan.
 * On failure: DELETING -> DEAD.  Runs on the unit task.
 */
void nvme_reset_finish(struct NVMeController *ctrl, BOOL ok);

/* ------------------------------------------------------------------ */
/* Amiga-side helpers populated by probe                                */
/* ------------------------------------------------------------------ */

/*
 * nvme_alloc_nvmeunit - callback fired by nvme_scan.c when a new NSID
 * is discovered.  Allocates and links an NVMeUnit reflecting the
 * namespace geometry.
 */
struct NVMeUnit *nvme_alloc_nvmeunit(struct NVMeController *ctrl, u32 nsid,
		ULONG blockSize, u8 blockShift, u64 logicalSectors, ULONG features);

#endif /* NVME_PROBE_H */
