// SPDX-License-Identifier: GPL-2.0-only
#ifndef DEVICES_NVME_H
#define DEVICES_NVME_H

/*
 * Public command interface for nvme.device — NVMe passthrough.
 *
 * Mirrors Linux's NVME_IOCTL_ADMIN_CMD / NVME_IOCTL_IO_CMD: lets userland
 * build a raw NVMe SQE plus an optional data buffer and submit it through
 * the driver.  The driver bounces the buffer if PiStorm DMA can't reach
 * it, runs nvme_passthru_start/end so CSE (exclusive-access) opcodes
 * freeze the I/O queue, and returns the CQE's status and DW0 result.
 */

#include <exec/types.h>
#include <devices/newstyle.h>

/* NSCMD numbers in the gap between NSCMD_TD_* (0xC003) and the device's
 * private NSCMD_ETD_* range (0xE000). */
#define NSCMD_NVME_ADMIN_PASS  0xD000   /* submit on the admin queue */
#define NSCMD_NVME_IO_PASS     0xD001   /* submit on the I/O queue (not yet supported) */

/*
 * struct NVMePassthruCmd - userland-supplied passthrough command.
 *
 * The IOStdReq's io_Data points to this struct; io_Length must equal
 * sizeof(struct NVMePassthruCmd).  On reply, io_Actual holds bytes
 * transferred and io_Error holds the NVMe status code (0 = success).
 *
 * Layout mirrors Linux's `struct nvme_passthru_cmd` (32-bit variant).
 * For the 64-bit result variant or metadata buffers, see TODO notes.
 */
struct NVMePassthruCmd
{
    UBYTE  pt_Opcode;       /* NVMe opcode (e.g. 0x06 = Identify) */
    UBYTE  pt_Flags;        /* nvme_command.common.flags */
    UWORD  pt_Rsvd1;
    ULONG  pt_Nsid;         /* namespace ID (0 for controller-scoped admin) */
    ULONG  pt_Cdw2;
    ULONG  pt_Cdw3;
    APTR   pt_Metadata;     /* NULL = none (metadata not yet supported) */
    APTR   pt_Addr;         /* data buffer; NULL if data_len = 0 */
    ULONG  pt_MetadataLen;
    ULONG  pt_DataLen;      /* data buffer size in bytes */
    ULONG  pt_Cdw10;
    ULONG  pt_Cdw11;
    ULONG  pt_Cdw12;
    ULONG  pt_Cdw13;
    ULONG  pt_Cdw14;
    ULONG  pt_Cdw15;
    ULONG  pt_TimeoutMs;    /* 0 = use driver default */
    ULONG  pt_Result;       /* OUT: CQE DW0 (command-specific) */
};

#endif /* DEVICES_NVME_H */
