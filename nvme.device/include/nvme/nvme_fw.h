// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_FW_H
#define NVME_FW_H

/*
 * nvme_fw_act_work - poll for firmware-activation completion and
 * refresh cached FW revision.  Triggered by a Firmware Activation
 * Starting AEN routed through nvme_queue_fw_act_work().
 */
void nvme_fw_act_work(struct NVMeController *ctrl);

#endif /* NVME_FW_H */
