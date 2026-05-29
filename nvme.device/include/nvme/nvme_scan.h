// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_SCAN_H
#define NVME_SCAN_H

struct NVMeController;

/*
 * nvme_scan_namespaces - workqueue handler that (re)scans the controller's
 * namespaces.  Triggered by nvme_queue_scan() from AEN handling,
 * post-reset, and subsystem attach.
 */
void nvme_queue_scan(struct NVMeController *ctrl);
void nvme_scan_namespaces(struct NVMeController *ctrl);

/*
 * nvme_remove_namespaces - tear down and remove all namespaces.
 * Called from controller teardown.
 */
void nvme_remove_namespaces(struct NVMeController *ctrl);

#endif /* NVME_SCAN_H */
