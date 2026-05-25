// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_HMB_H
#define NVME_HMB_H

/*
 * Host Memory Buffer setup and teardown.  See src/nvme/nvme_hmb.c
 * for the implementation notes.
 *
 * nvme_setup_host_mem - called from the probe path after MDTS /
 *   hmpre / hmmin have been read by nvme_init_ctrl_finish().
 *   Returns 0 unconditionally; HMB failure is non-fatal.
 *
 * nvme_free_host_mem - called from the controller teardown path
 *   before the memory pool is destroyed.  Disables HMB on the
 *   controller (Set Features bits=0) and releases all allocated
 *   chunks plus the descriptor table.
 */
void nvme_setup_host_mem(struct NVMeController *ctrl);
void nvme_free_host_mem(struct NVMeController *ctrl);

#endif /* NVME_HMB_H */
