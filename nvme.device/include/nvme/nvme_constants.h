// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_CONSTANTS_H
#define NVME_CONSTANTS_H

/*
 * Stringification of NVMe completion statuses and opcodes — used by the
 * error-logging and debug-trace paths throughout the driver.  Implementations
 * live in nvme_constants.c (large switch tables).
 */
const char *nvme_get_error_status_str(u16 status);
const char *nvme_get_opcode_str(u8 opcode);
const char *nvme_get_admin_opcode_str(u8 opcode);

/*
 * nvme_opcode_str - opcode name for a command, by queue type.
 * qid 0 is the admin queue (admin opcode space); any other qid is an I/O queue.
 */
static inline const char *nvme_opcode_str(int qid, u8 opcode)
{
	return qid ? nvme_get_opcode_str(opcode) :
		nvme_get_admin_opcode_str(opcode);
}

#endif /* NVME_CONSTANTS_H */
