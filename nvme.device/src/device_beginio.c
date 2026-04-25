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
#include <debug.h>

#include "../include/device.h"

static const UWORD SupportedCommands[] = {
    CMD_READ,
    CMD_WRITE,
    CMD_UPDATE, /* quick */
    CMD_CLEAR,  /* quick */
    CMD_STOP, /* standby? */
    CMD_START, /* resume? */
    TD_MOTOR, /* quick */
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
    ETD_UPDATE, /* quick */
    ETD_CLEAR,  /* quick */
    NSCMD_TD_READ64,
    NSCMD_TD_WRITE64,
    NSCMD_TD_FORMAT64,
    NSCMD_ETD_READ64,
    NSCMD_ETD_WRITE64,
    NSCMD_ETD_FORMAT64,
    NSCMD_DEVICEQUERY, /* quick */
    0};

/*
 * Handle NSCMD_DEVICEQUERY inline (no unit task round-trip needed).
 */
static void do_nscmd_devicequery(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *dq = (struct NSDeviceQueryResult *)io->io_Data;

    if (io->io_Length < (ULONG)sizeof(struct NSDeviceQueryResult))
    {
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

/*
 * Handle TD_GETGEOMETRY inline.
 * Returns cached geometry from the unit; zeros until Phase 2 populates
 * the fields via Identify Namespace.
 */
static void do_td_getgeometry(struct IOStdReq *io, struct NVMeUnit *unit)
{
    struct DriveGeometry *dg = (struct DriveGeometry *)io->io_Data;

    if (io->io_Length < (ULONG)sizeof(struct DriveGeometry))
    {
        io->io_Error = IOERR_BADLENGTH;
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

    switch (io->io_Command)
    {
    case TD_MOTOR: /* NVMe drives have no spindle motor; always report success */
    case ETD_MOTOR:
    case CMD_UPDATE: /* No explicit cache flush */
    case ETD_UPDATE:
    case CMD_CLEAR: /* No explicit cache update */
    case ETD_CLEAR:
    case TD_CHANGENUM: /* Fixed media: no media changes */
    case TD_CHANGESTATE:
    case TD_PROTSTATUS: /* NVMe drives are not write-protected by default */
    case TD_ADDCHANGEINT: /* Fixed media: no change interrupts. */
    case TD_REMCHANGEINT: /* Nothing to remove for fixed media */
        io->io_Actual = 0;
        break;

    case TD_EJECT: /* Fixed media: no eject */
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

    default:
        /* All other commands (reads, writes, format, SCSI, …)
         * must run in the unit task context */
        queue = TRUE;
        break;
    }

    if (queue)
    {
        io->io_Flags &= (UBYTE)~IOF_QUICK;
        PutMsg(&unit->ctrl->msgPort, (struct Message *)io);
    }
    else
    {
        /* Synchronous completion — reply if the caller is not WaitIO()ing */
        if (!(io->io_Flags & IOF_QUICK))
            ReplyMsg((struct Message *)io);
    }
}
