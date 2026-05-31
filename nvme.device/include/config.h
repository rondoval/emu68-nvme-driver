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

#endif /* NVME_DEVICE_CONFIG_H */
