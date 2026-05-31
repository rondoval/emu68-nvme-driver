// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_PASSTHRU_H
#define NVME_PASSTHRU_H

struct NVMeController;
struct IOStdReq;

/*
 * nvme_passthru_process - run one NVMe admin passthrough IOStdReq to completion.
 *
 * Called from AdminWorker's adminPort drain loop.  Validates the request,
 * bounces the data buffer if needed, submits the command synchronously to
 * the admin queue, and always ReplyMsg's @io on return.
 */
void nvme_passthru_process(struct NVMeController *ctrl, struct IOStdReq *io);

#endif /* NVME_PASSTHRU_H */
