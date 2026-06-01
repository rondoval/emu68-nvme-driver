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

/* Private 3rd-party commands must stay out of NSD's reserved ranges.
 * Keep these in the legal 0x8000-0xBFFF command space.
 *
 * NSCMD_NVME_TRIM uses an explicit logical-block range list
 * (struct NVMeTrimRange below). Callers that start from byte ranges should
 * query TD_GETGEOMETRY first and use dg_SectorSize to convert bytes to LBAs
 * and block counts. The current native TRIM interface accepts at most 256
 * ranges per request.
 *
 * NSCMD_NVME_WRITE_ZEROES is different: it follows the 64-bit trackdisk-style
 * offset/length contract, using io_Offset plus the high 32 bits in io_Actual
 * on entry, and io_Length as the byte count. It does not consume io_Data. */
#define NSCMD_NVME_ADMIN_PASS  0x8020   /* submit on the admin queue */
#define NSCMD_NVME_IO_PASS     0x8021   /* submit on the I/O queue (not yet supported) */
#define NSCMD_NVME_TRIM        0x8022   /* deallocate ranges: io_Data = NVMeTrimRange[], io_Length = nr*sizeof */
#define NSCMD_NVME_WRITE_ZEROES 0x8023  /* zero a single range: 64-bit byte offset in io_Offset/io_Actual, byte count in io_Length, no io_Data */

/*
 * struct NVMeTrimRange - one range for NSCMD_NVME_TRIM (NVMe DSM Deallocate).
 *
 * The LBA is split hi/lo to stay 32-bit-clean. io_Data points to an array of
 * these. io_Length must be nr * sizeof(struct NVMeTrimRange), with
 * 1 <= nr <= 256.
 *
 * ntr_SectorHi/ntr_SectorLo name the starting logical block address.
 * ntr_Count is the number of logical blocks to deallocate.
 *
 * These are block units, not bytes. Callers that start from byte ranges
 * should issue TD_GETGEOMETRY first and use dg_SectorSize to convert byte
 * offsets/lengths into LBAs and block counts.
 */
struct NVMeTrimRange
{
    ULONG ntr_SectorHi; /* high 32 bits of the starting LBA */
    ULONG ntr_SectorLo; /* low 32 bits of the starting LBA  */
    ULONG ntr_Count;    /* number of logical blocks to deallocate */
};

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
