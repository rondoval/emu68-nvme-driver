// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_CONSTANTS_H
#define NVME_CONSTANTS_H

/*
 * Stringification of NVMe completion statuses and opcodes — used by the
 * error-logging path in nvme_completion.c.  Implementations live in
 * nvme_constants.c (large switch tables).
 */
const char *nvme_get_error_status_str(u16 status);
const char *nvme_get_opcode_str(u8 opcode);
const char *nvme_get_admin_opcode_str(u8 opcode);

#endif /* NVME_CONSTANTS_H */
