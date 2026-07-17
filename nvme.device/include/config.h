/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef NVME_DEVICE_CONFIG_H
#define NVME_DEVICE_CONFIG_H

#ifndef DEVICE_NAME
#define DEVICE_NAME "nvme.device"
#endif

#ifndef DEVICE_IDSTRING
#define DEVICE_IDSTRING "NVMe PCIe Storage Driver"
#endif

#ifndef DEVICE_VERSION
#define DEVICE_VERSION 1
#endif

#ifndef DEVICE_REVISION
#define DEVICE_REVISION 0
#endif

#ifndef DEVICE_USE_MSI
#define DEVICE_USE_MSI TRUE
#endif

/* Prefer MSI-X when the device and controller support it (falls back to MSI
 * then INTx).  Set FALSE to forbid MSI-X for this driver. */
#ifndef DEVICE_USE_MSIX
#define DEVICE_USE_MSIX TRUE
#endif

/* NVMe interrupt coalescing (Set Features 0x08), disabled by default.
 *
 * TIME is the aggregation window in 100 µs units; THR is the completion
 * threshold (the controller waits for THR+1 completions, or the window to
 * elapse, before raising an interrupt).  Both 0 = feature not issued.
 *
 * Off by default to match Linux: coalescing trades completion latency for
 * fewer interrupts, the single shared vector would also delay admin
 * completions, and the driver already coalesces naturally by masking the
 * IRQ for the whole CQ drain.  Set non-zero to experiment on a given device.
 */
#ifndef DEVICE_IRQ_COALESCE_TIME
#define DEVICE_IRQ_COALESCE_TIME 0
#endif

#ifndef DEVICE_IRQ_COALESCE_THR
#define DEVICE_IRQ_COALESCE_THR 0
#endif

#define STACK_SIZE              65535
#define UNIT_TASK_PRIORITY      10
#define UNIT_TASK_POLL_DELAY_MS 100
/* Command timeout: 5 seconds */
#define CMD_TIMEOUT_MS          5000

/* Admin queue size (entries, must be power of 2, NVMe spec max 4096) */
#define NVME_ADMIN_QUEUE_SIZE   64
/* I/O queue size (entries, must be power of 2) */
#define NVME_IO_QUEUE_SIZE      256

/* Maximum active namespaces per controller.
 * NVMe spec §5.15.4: Identify Active NSID List returns up to 1024 NSIDs. */
#define NVME_MAX_NS             1024

/* Automount recipes for MBR/GPT/superfloppy filesystems (mounter submodule).
 * RDB partitions carry their own filesystem/handler info and ignore these. */
#ifndef NVME_FAT_DOSTYPE
#define NVME_FAT_DOSTYPE        0x46415401u /* 'FAT\1' (fat95) */
#endif

#ifndef NVME_FAT_HANDLER
#define NVME_FAT_HANDLER        "L:fat95"
#endif

#ifndef NVME_NTFS_DOSTYPE
#define NVME_NTFS_DOSTYPE       0x4E544653u /* 'NTFS' */
#endif

#ifndef NVME_NTFS_HANDLER
#define NVME_NTFS_HANDLER       "L:NTFileSystem3G"
#endif

/* The mounter ensures a trailing digit and bumps past collisions,
 * so partitions come up as NVME0:, NVME1:, ... */
#ifndef NVME_LEGACY_DOSNAME
#define NVME_LEGACY_DOSNAME     "NVME"
#endif

/* Fixed internal disk: a more generous filesystem cache than the
 * mounter's removable-media default. */
#ifndef NVME_LEGACY_BUFFERS
#define NVME_LEGACY_BUFFERS     500
#endif

#ifndef NVME_LEGACY_MAXTRANSFER
#define NVME_LEGACY_MAXTRANSFER 0x00FFFFFFu
#endif

#endif /* NVME_DEVICE_CONFIG_H */
