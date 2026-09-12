// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_DEVICE_H
#define NVME_DEVICE_H

#if defined(__INTELLISENSE__)
#define asm(x)
#define __attribute__(x)
#endif

#include <types.h>   /* u64 / s32 used in the struct + prototype declarations below */
#include <reset_guard.h>

struct NVMeController;

#define LIB_MIN_VERSION 39 /* we use memory pools */

/* Below FileSystem.resource (80) and bcmpcie.library (-41) which we open, above the
 * boot menu (-50) that lists the volumes we mount.
 */
#define DEVICE_PRIORITY (-43)

#define COMMAND_PROCESSED 1
#define COMMAND_SCHEDULED 0

/* Error codes used by Unit lifecycle functions */
#define ERR_NO_ERROR 0
#define ERR_ALLOC_ERROR -1
#define ERR_CONTROLLER_ERROR -2
#define ERR_LIBRARY_ERROR -3

/* Internal command: posted by AbortIO to the unit task */
#define CMD_INTERNAL_ABORT_REQUEST (CMD_NONSTD + 0x100)

/* New Style Device Enhanced-Trackdisk 64-bit commands (not in the NDK) */
#define NSCMD_ETD_READ64 0xE000
#define NSCMD_ETD_WRITE64 0xE001
#define NSCMD_ETD_SEEK64 0xE002
#define NSCMD_ETD_FORMAT64 0xE003

/* Mark internal pool-allocated requests so ProcessCommand can free them */
#define REQ_INTERNAL (1UL << 0)

enum nvme_unit_features {
    NVME_NS_DEAC = 1 << 2, /* DEAC bit in Write Zeroes supported */
};

/*
 * Device base structure — one instance per nvme.device resident.
 */
struct NVMeDevice
{
    struct Device device;
    ULONG segList;
    struct Library *utilityBase;
    struct Library *pcieBase;
    struct reset_guard resetGuard; /* pre-reset DMA quiesce + SHN hooks */

    BOOL probed;                /* TRUE after nvme_probe_all() has run */
    struct MinList controllers; /* list of NVMeController */
    struct MinList units;       /* flat list of NVMeUnit ordered by unitNumber */
    ULONG nextUnitNumber;       /* monotonically increasing; assigned by
                                 * nvme_alloc_nvmeunit on each NSID found */
};

/*
 * Per-namespace (per-unit) state.
 *
 * Pre-allocated by nvme_probe_all(); not freed until device expunge.
 */
struct NVMeUnit
{
    struct Unit unit;            /* MUST be first: mp_Node.ln_Succ/Pred = MinList links */
    struct NVMeController *ctrl; /* owning controller */
    struct NVMeDevice *device;

    LONG unitNumber; /* global, 0-based */
    ULONG nsid;      /* NVMe Namespace ID (1-based) */
    LONG flags;      /* NVME_UNIT_* bits below */

    /* Disk geometry — populated from Identify Namespace */
    ULONG blockSize;
    ULONG blockShift; /* log2(blockSize) */
    u64 logicalSectors;
    ULONG features;   /* namespace capability bits */
    u32 wz_max_bytes; /* max bytes one Write Zeroes covers: min(max_zeroes_sectors, 64K blocks), block-aligned. Cached at nvme_alloc_nvmeunit. */

    /* Media-change counter reported by TD_CHANGENUM and checked against the
     * caller's iotd_Count on ETD_* commands.  Fixed media: initialised to 1 at
     * alloc so a stale iotd_Count of 0 is rejected (TDERR_DiskChanged). */
    ULONG changeCount;
};

/* NVMeUnit flag bits.  Read by openLib / UnitOpen / beginIO / ProcessCommand;
 * written by nvme_ns_remove when the underlying namespace disappears. */
#define NVME_UNIT_DEAD (1L << 0) /* namespace gone; refuse new I/O */

/* ------------------------------------------------------------------ */
/* device.c                                                            */
/* ------------------------------------------------------------------ */
void beginIO(struct IOStdReq *io asm("a1"), struct NVMeDevice *base asm("a6"));
LONG abortIO(struct IOStdReq *io asm("a1"), struct NVMeDevice *base asm("a6"));

/* ------------------------------------------------------------------ */
/* unit.c                                                              */
/* ------------------------------------------------------------------ */
s32 UnitOpen(struct NVMeUnit *unit, LONG unitNumber, LONG flags);
s32 UnitClose(struct NVMeUnit *unit);

/* ------------------------------------------------------------------ */
/* unit_task.c — UnitTask entry (spawn/join via emu68-common driver_task) */
/* ------------------------------------------------------------------ */
void UnitTask(struct NVMeController *ctrl, struct Task *parent);

/* ------------------------------------------------------------------ */
/* admin_task.c                                                        */
/* ------------------------------------------------------------------ */
void AdminWorker(struct NVMeController *ctrl, struct Task *parent);

/* ------------------------------------------------------------------ */
/* unit_commands.c                                                     */
/* ------------------------------------------------------------------ */
void ProcessCommand(struct IOStdReq *io);

/* ------------------------------------------------------------------ */
/* unit_commands_scsi.c                                                */
/* ------------------------------------------------------------------ */
/* HD_SCSICMD entry point.  ProcessCommand dispatches here; returns
 * NVME_IO_ASYNC if the command was forwarded to the NVMe I/O path
 * (completion path replies the IOStdReq), or an IOERR_* code that
 * ProcessCommand maps to reply_io. */
BYTE handle_scsi_cmd(struct IOStdReq *io);

/* ------------------------------------------------------------------ */
/* irq.c                                                               */
/* ------------------------------------------------------------------ */
s32 nvme_int_enable(struct NVMeController *ctrl);
void nvme_int_shutdown(struct NVMeController *ctrl);
void nvme_int_rearm(struct NVMeController *ctrl);

#endif /* NVME_DEVICE_H */
