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

/* The virtual drives' device (mounter/vdrive.h).  Its id string carries no
 * "$VER: ": the one in this binary is nvme.device's. */
#define VD_NAME "nvmevd.device"

#ifndef VD_IDSTRING
#define VD_IDSTRING VD_NAME
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
/* I/O queue depth asked for (entries). A controller whose CAP.MQES allows
 * fewer gets MQES + 1 instead (nvme_set_io_queue_depth); the ring code takes
 * any depth. */
#define NVME_IO_QUEUE_SIZE      256
/* Smallest queue the NVMe spec allows: CAP.MQES is at least 1h, two entries. */
#define NVME_MIN_IO_QUEUE_SIZE  2

/* Ctrl-Amiga-Amiga (deferred reset_guard path, nvme_reset_warn_tick): after the
 * warning the filesystems still flush through us; a controller shuts down once its
 * I/O has been idle this long, or this long after the warning at the latest. The
 * keyboard resets the machine 10 s after the warning whatever happens. */
#define NVME_RESET_IDLE_MS      500
#define NVME_RESET_DEADLINE_MS  7000
/* CC.SHN wait on the reset paths, whatever RTD3E says: deferred (after the Host
 * Memory Buffer is released, which makes shutdown fast) and inline (ColdReboot,
 * within reset_guard's per-driver budget). */
#define NVME_RESET_SHUTDOWN_S   2
#define NVME_INLINE_SHUTDOWN_S  5

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

/* Same dostype/handler pair massstorage.class defaults to, so a drive moved
 * between a USB enclosure and an NVMe slot mounts the same way. */
#ifndef NVME_EXFAT_DOSTYPE
#define NVME_EXFAT_DOSTYPE      0x46415458u /* 'FATX' */
#endif

#ifndef NVME_EXFAT_HANDLER
#define NVME_EXFAT_HANDLER      "L:exFATFileSystem"
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

/* Stack for the two handlers built on filesysbox.library.  64 KB is what is
 * recommended for NTFileSystem3G (ntfs-3g).
 * exFATFileSystem gets the same. */
#ifndef NVME_NTFS_STACK
#define NVME_NTFS_STACK         65536
#endif

#ifndef NVME_EXFAT_STACK
#define NVME_EXFAT_STACK        65536
#endif

#ifndef NVME_LEGACY_MAXTRANSFER
#define NVME_LEGACY_MAXTRANSFER 0x00FFFFFFu
#endif

#endif /* NVME_DEVICE_CONFIG_H */
