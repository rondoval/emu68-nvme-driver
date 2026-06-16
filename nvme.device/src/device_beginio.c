// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>

#include "devices/nvme.h"
#include "nvme/nvme_ctrl.h"
#include "device.h"

static const UWORD SupportedCommands[] = {
    CMD_READ,
    CMD_WRITE,
    CMD_UPDATE,
    CMD_CLEAR,  /* quick */
    CMD_STOP,   /* standby? */
    CMD_START,  /* resume? */
    TD_MOTOR,   /* quick */
    TD_FORMAT,
    TD_CHANGENUM,    /* quick */
    TD_CHANGESTATE,  /* quick */
    TD_PROTSTATUS,   /* quick */
    TD_GETDRIVETYPE, /* quick */
    TD_ADDCHANGEINT, /* quick */
    TD_REMCHANGEINT, /* quick */
    TD_GETGEOMETRY,  /* quick */
    TD_READ64,
    TD_WRITE64,
    TD_FORMAT64,
    ETD_MOTOR, /* quick */
    ETD_FORMAT,
    ETD_READ,
    ETD_WRITE,
    ETD_UPDATE,
    ETD_CLEAR,  /* quick */
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    NSCMD_ETD_READ64,
    NSCMD_ETD_WRITE64,
    NSCMD_ETD_FORMAT64,
    NSCMD_DEVICEQUERY, /* quick */
    NSCMD_NVME_ADMIN_PASS,
    NSCMD_NVME_IO_PASS,
    NSCMD_NVME_TRIM,
    NSCMD_NVME_WRITE_ZEROES,
    NSCMD_NVME_UNIT_INFO, /* quick */
    0};

/*
 * Handle NSCMD_DEVICEQUERY inline (no unit task round-trip needed).
 */
static void do_nscmd_devicequery(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *dq = (struct NSDeviceQueryResult *)io->io_Data;
    KprintfH("[nvme] NSCMD_DEVICEQUERY: io_Length=%lu\n", io->io_Length);

    if (io->io_Length < (ULONG)sizeof(struct NSDeviceQueryResult))
    {
        KprintfH("[nvme] NSCMD_DEVICEQUERY: buffer too small (need %lu)\n",
                 (ULONG)sizeof(struct NSDeviceQueryResult));
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    dq->nsdqr_SizeAvailable = sizeof(struct NSDeviceQueryResult);
    dq->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
    dq->nsdqr_DeviceSubType = 0;
    dq->nsdqr_SupportedCommands = (UWORD *)SupportedCommands;
    io->io_Actual = dq->nsdqr_SizeAvailable;
    io->io_Error = 0;
}

/* Copy a fixed-width Identify text field, NUL-terminating and stripping the
 * trailing space padding so userland never re-implements the trim. */
static void copy_trimmed(char *dst, const char *src, ULONG src_len)
{
    ULONG len = src_len;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\0'))
        len--;
    CopyMem((APTR)src, dst, len);
    dst[len] = '\0';
}

/*
 * Handle NSCMD_NVME_UNIT_INFO inline (cached unit/ctrl state plus two MMIO
 * register reads; no admin command, no task round-trip).
 */
static void do_nscmd_unit_info(struct IOStdReq *io, struct NVMeUnit *unit)
{
    struct NVMeUnitInfo *info = (struct NVMeUnitInfo *)io->io_Data;
    struct NVMeController *ctrl = unit->ctrl;

    if (!info || io->io_Length < (ULONG)sizeof(struct NVMeUnitInfo))
    {
        KprintfH("[nvme] NSCMD_NVME_UNIT_INFO: buffer too small (need %lu)\n",
                 (ULONG)sizeof(struct NVMeUnitInfo));
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    info->nui_StructSize = sizeof(struct NVMeUnitInfo);
    info->nui_UnitNumber = (ULONG)unit->unitNumber;
    info->nui_Nsid = unit->nsid;
    info->nui_CtrlFirstUnit = ctrl->firstUnitNumber;
    info->nui_CtrlUnitCount = ctrl->nsCount;
    info->nui_CapLo = nvme_reg_read32(ctrl, NVME_REG_CAP);
    info->nui_CapHi = nvme_reg_read32(ctrl, NVME_REG_CAP + 4);
    info->nui_Version = nvme_reg_read32(ctrl, NVME_REG_VS);
    copy_trimmed(info->nui_Serial, ctrl->id_strings.serial,
                 sizeof(ctrl->id_strings.serial));
    copy_trimmed(info->nui_Model, ctrl->id_strings.model,
                 sizeof(ctrl->id_strings.model));
    copy_trimmed(info->nui_Firmware, ctrl->id_strings.firmware,
                 sizeof(ctrl->id_strings.firmware));

    io->io_Actual = info->nui_StructSize;
    io->io_Error = 0;
}

/*
 * Handle TD_GETGEOMETRY inline.
 * Returns cached geometry from the unit; zeros until Phase 2 populates
 * the fields via Identify Namespace.
 */
static void do_td_getgeometry(struct IOStdReq *io, struct NVMeUnit *unit)
{
    struct DriveGeometry *dg = (struct DriveGeometry *)io->io_Data;
    KprintfH("[nvme] TD_GETGEOMETRY: unit %ld io_Length=%lu\n", unit->unitNumber, io->io_Length);

    if (io->io_Length < (ULONG)sizeof(struct DriveGeometry))
    {
        io->io_Error = IOERR_BADLENGTH;
        KprintfH("[nvme] TD_GETGEOMETRY: buffer too small (need %lu)\n",
                 (ULONG)sizeof(struct DriveGeometry));
        return;
    }

    /* Capacity in 512-byte sectors, capped at ULONG_MAX for large drives */
    ULONG totalSectors;
    if (unit->blockSize == 0)
    {
        totalSectors = 0;
    }
    else
    {
        ULONG sectorsPerBlock = unit->blockSize >> 9; /* blockSize / 512 */
        uint64_t total64 = unit->logicalSectors * sectorsPerBlock;
        totalSectors = (total64 > 0xFFFFFFFFUL) ? 0xFFFFFFFFUL : (ULONG)total64;
    }

    dg->dg_SectorSize = (unit->blockSize != 0) ? unit->blockSize : 512;
    dg->dg_TotalSectors = totalSectors;
    dg->dg_Cylinders = 0; /* Phase 2: derive from Identify Controller */
    dg->dg_CylSectors = 0;
    dg->dg_Heads = 0;
    dg->dg_TrackSectors = 0;
    dg->dg_BufMemType = MEMF_PUBLIC | MEMF_FAST;
    dg->dg_DeviceType = DG_DIRECT_ACCESS;
    dg->dg_Flags = 0; /* Fixed media, not removable */
    dg->dg_Reserved = 0;

    io->io_Actual = sizeof(struct DriveGeometry);
    io->io_Error = 0;
    KprintfH("[nvme] TD_GETGEOMETRY: blockSize=%lu logicalSectors=%lu totalSectors=%lu\n",
             unit->blockSize, (ULONG)unit->logicalSectors, totalSectors);
}

/*
 * nvme_cmd_is_etd - TRUE for extended (ETD_*) commands carrying a struct IOExtTD
 *
 * These commands embed an iotd_Count (the caller's view of the media-change
 * counter) after the IOStdReq, so beginIO may validate it.  A bare
 * (cmd & TDF_EXTCOM) test is unusable: the NSCMD_*64 values also set bit 15.
 */
static inline BOOL nvme_cmd_is_etd(UWORD cmd)
{
    switch (cmd)
    {
    case ETD_READ:
    case ETD_WRITE:
    case ETD_MOTOR:
    case ETD_SEEK:
    case ETD_FORMAT:
    case ETD_UPDATE:
    case ETD_CLEAR:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
        return TRUE;
    default:
        return FALSE;
    }
}

/*
 * beginIO - dispatch an I/O request for nvme.device.
 *
 * Trivial commands that only read cached state or set flags are handled
 * immediately (synchronous / quick reply).  All data-transfer commands
 * are queued to the unit task for asynchronous processing.
 */
void beginIO(struct IOStdReq *io asm("a1"), struct NVMeDevice *base asm("a6") __attribute__((unused)))
{
    struct NVMeUnit *unit = (struct NVMeUnit *)io->io_Unit;
    BOOL queue = FALSE;

    io->io_Error = 0;
    /* io_Actual is NOT cleared here: 64-bit commands (TD_READ64, NSCMD_TD_READ64, …)
     * pass the high 32 bits of the byte offset in io_Actual before calling BeginIO.
     * ProcessCommand saves it before doing anything else. */
    KprintfH("[nvme] beginIO: cmd=0x%04lx unit=%ld io_Length=%lu io_Actual=%lu\n",
             (ULONG)io->io_Command, unit->unitNumber, io->io_Length, io->io_Actual);

    /* Namespace went away under us — refuse new I/O.  Set TDERR_DiskChanged
     * (standard trackdisk "media gone" reply) so filesystems mark the volume
     * not-ready instead of retrying forever. */
    if (unit->flags & NVME_UNIT_DEAD)
    {
        io->io_Error = TDERR_DiskChanged;
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg((struct Message *)io);
        return;
    }

    /* Extended (ETD_*) commands carry a struct IOExtTD whose iotd_Count is the
     * caller's view of the media-change counter.  If it is older than the
     * unit's current count, the caller's media assumptions are stale, so fail
     * with TDERR_DiskChanged per the trackdisk ETD contract. */
    if (nvme_cmd_is_etd(io->io_Command) &&
        ((struct IOExtTD *)io)->iotd_Count < unit->changeCount)
    {
        io->io_Error = TDERR_DiskChanged;
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg((struct Message *)io);
        return;
    }

    switch (io->io_Command)
    {
    case TD_MOTOR: /* NVMe drives have no spindle motor; always report success */
    case ETD_MOTOR:
    case CMD_CLEAR: /* No explicit cache update */
    case ETD_CLEAR:
    case TD_CHANGESTATE: /* Fixed media: always present (0 = disk inserted) */
    case TD_PROTSTATUS:   /* NVMe drives are not write-protected by default */
    case TD_ADDCHANGEINT: /* Fixed media: no change interrupts. */
    case TD_REMCHANGEINT: /* Nothing to remove for fixed media */
        io->io_Actual = 0;
        break;

    case TD_CHANGENUM: /* Current media-change counter (constant for fixed media) */
        io->io_Actual = unit->changeCount;
        break;

    case TD_EJECT:        /* Fixed media: no eject */
    case TD_GETNUMTRACKS: /* No CHS geometry */
        io->io_Error = IOERR_NOCMD;
        break;

    case TD_GETDRIVETYPE:
        io->io_Actual = DG_DIRECT_ACCESS;
        break;

    case TD_GETGEOMETRY:
        do_td_getgeometry(io, unit);
        break;

    case NSCMD_DEVICEQUERY:
        do_nscmd_devicequery(io);
        break;

    case NSCMD_NVME_UNIT_INFO:
        do_nscmd_unit_info(io, unit);
        break;

    case NSCMD_NVME_ADMIN_PASS:
    case NSCMD_NVME_IO_PASS:
        if (!io->io_Data || io->io_Length != sizeof(struct NVMePassthruCmd))
        {
            io->io_Error = IOERR_BADLENGTH;
            break;
        }
        if (!unit->ctrl->admin_task)
        {
            io->io_Error = IOERR_OPENFAIL;
            break;
        }
        io->io_Flags &= (UBYTE)~IOF_QUICK;
        PutMsg(unit->ctrl->adminPort, (struct Message *)io);
        return; /* AdminWorker ReplyMsg's after completion */

    default:
        /* All other commands (reads, writes, format, SCSI, …)
         * must run in the unit task context */
        queue = TRUE;
        break;
    }

    if (queue)
    {
        KprintfH("[nvme] beginIO: queuing command 0x%04lx to unit %ld\n",
                 (ULONG)io->io_Command, unit->unitNumber);
        io->io_Flags &= (UBYTE)~IOF_QUICK;
        PutMsg(unit->ctrl->msgPort, (struct Message *)io);
    }
    else
    {
        KprintfH("[nvme] beginIO: completed command 0x%04lx for unit %ld — io_Error=%ld io_Actual=%lu\n",
                 (ULONG)io->io_Command, unit->unitNumber, io->io_Error, io->io_Actual);
        /* Synchronous completion — reply if the caller is not WaitIO()ing */
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg((struct Message *)io);
    }
}
