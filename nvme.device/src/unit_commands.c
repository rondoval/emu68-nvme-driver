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
#include <devices/scsidisk.h>
#include "devices/nvme.h" /* NSCMD_NVME_WRITE_ZEROES */

#include "device.h"
#include "nvme/nvme_ctrl.h"  /* struct NVMeController */
#include "nvme/nvme_admin.h" /* nvme_io_abort */
#include "nvme/nvme_io.h"

/*
 * reply_io - set error code and reply an IOStdReq.
 */
static inline void reply_io(struct IOStdReq *io, BYTE error)
{
    KprintfH("[nvme] reply_io: cmd=0x%04lx unit=%ld error=%ld io_Actual=%lu\n",
             (ULONG)io->io_Command, ((struct NVMeUnit *)io->io_Unit)->unitNumber,
             error, io->io_Actual);
    io->io_Error = error;
    ReplyMsg((struct Message *)io);
}

/*
 * decode_lba - assemble a 64-bit block LBA from a saved io_Actual + io_Offset.
 *
 * io_actual: value of io->io_Actual saved before any zeroing (high 32-bit byte offset)
 * io_offset: io->io_Offset (low 32-bit byte offset)
 * blockShift: log2(blockSize); NVMe minimum is 9 (512 B) so the shift-by-32 edge
 *             case cannot occur.
 *
 * Avoids 64-bit libgcc emulation routines by keeping operands as two ULONGs.
 * Identical arithmetic to lide.device iotask.c:488-496.
 */
static inline u64 decode_lba(ULONG io_actual, ULONG io_offset, UWORD blockShift)
{
    ULONG hi = io_actual >> blockShift;
    ULONG lo = (io_actual << (32u - blockShift)) | (io_offset >> blockShift);
    KprintfH("[nvme] decode_lba: io_actual=0x%08lx io_offset=0x%08lx blockShift=%lu => lba=0x%08lx%08lx\n",
             io_actual, io_offset, (ULONG)blockShift, hi, lo);
    return ((u64)hi << 32) | lo;
}

/*
 * ProcessCommand - dispatch an IOStdReq received by the unit task.
 *
 * Called from the unit task's message-drain loop.  Synchronous commands
 * (device control) are replied immediately.  Data-transfer commands are
 * dispatched via nvme_io_submit_*(), which builds the SQE and rings the
 * doorbell — async completion drives reply_io from the CQE path.
 */
void ProcessCommand(struct IOStdReq *io)
{
    struct NVMeUnit *unit = (struct NVMeUnit *)io->io_Unit;

    /* Race-close: a BeginIO that passed its NVME_UNIT_DEAD check, was
     * PutMsg'd, and then the unit got marked dead before we picked the
     * message up.  Re-check here so the gone-namespace I/O fails cleanly
     * instead of dispatching to a controller that no longer has the NSID. */
    if (unit->flags & NVME_UNIT_DEAD)
    {
        io->io_Error = TDERR_DiskChanged;
        ReplyMsg((struct Message *)io);
        return;
    }

    /*
     * Save the high 32 bits of the byte offset BEFORE zeroing io_Actual.
     * 64-bit commands (TD_READ64, NSCMD_TD_READ64, NSCMD_ETD_READ64, …)
     * pass the high byte offset in io_Actual on entry; we need it for
     * decode_lba().
     */
    ULONG high_offset = 0u;
    switch (io->io_Command)
    {
    case TD_READ64:
    case TD_WRITE64:
    case TD_FORMAT64:
    case NSCMD_TD_READ64:
    case NSCMD_TD_WRITE64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_READ64:
    case NSCMD_ETD_WRITE64:
    case NSCMD_ETD_FORMAT64:
    case NSCMD_NVME_WRITE_ZEROES:
        high_offset = (ULONG)io->io_Actual;
        break;
    }

    io->io_Actual = 0;
    enum
    {
        READ,
        WRITE
    } direction = WRITE;

    switch (io->io_Command)
    {
    case CMD_START:
        // TODO resume from sleep?
        reply_io(io, 0);
        break;

    case CMD_STOP:
        // TODO enter low power mode?
        reply_io(io, 0);
        break;

    case CMD_READ:
    case ETD_READ:
    case TD_READ64:
    case NSCMD_TD_READ64:
    case NSCMD_ETD_READ64:
        direction = READ;
        /* fall through to shared read/write handling */

    case CMD_WRITE:
    case ETD_WRITE:
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
    case NSCMD_ETD_WRITE64:
    case TD_FORMAT:
    case ETD_FORMAT:
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
    case NSCMD_ETD_FORMAT64:
    {
        u64 lba = decode_lba(high_offset, (ULONG)io->io_Offset, (UWORD)unit->blockShift);
        ULONG blockCount = (ULONG)io->io_Length >> unit->blockShift;

        BYTE error = nvme_io_submit_rw(unit, io, lba, blockCount,
                                       direction == READ ? nvme_cmd_read : nvme_cmd_write,
                                       io->io_Data);
        if (error != NVME_IO_ASYNC)
            reply_io(io, error);
        break;
    }

    case NSCMD_NVME_WRITE_ZEROES:
    {
        u64 lba = decode_lba(high_offset, (ULONG)io->io_Offset, (UWORD)unit->blockShift);
        ULONG blockCount = (ULONG)io->io_Length >> unit->blockShift;

        BYTE error = nvme_io_submit_write_zeroes(unit, io, lba, blockCount);
        if (error != NVME_IO_ASYNC)
            reply_io(io, error);
        break;
    }

    case NSCMD_NVME_TRIM:
    {
        struct NVMeController *ctrl = unit->ctrl;
        const struct NVMeTrimRange *tr = (const struct NVMeTrimRange *)io->io_Data;
        ULONG nr = tr ? (ULONG)io->io_Length / (ULONG)sizeof(struct NVMeTrimRange) : 0;

        if (!tr || nr == 0)
        {
            reply_io(io, IOERR_BADLENGTH);
            break;
        }
        if (nr > NVME_DSM_MAX_RANGES)
        {
            KprintfH("[nvme] %s: TRIM %lu ranges exceeds DSM cap %lu\n",
                     __func__, nr, (ULONG)NVME_DSM_MAX_RANGES);
            reply_io(io, IOERR_BADLENGTH);
            break;
        }

        struct nvme_dsm_range *ranges =
            dma_zalloc(ctrl->memoryPool, NVME_CTRL_PAGE_SIZE,
                       sizeof(*ranges) * NVME_DSM_MAX_RANGES);
        if (!ranges)
        {
            reply_io(io, IOERR_SELFTEST);
            break;
        }

        BYTE rerr = 0;
        for (ULONG i = 0; i < nr; i++)
        {
            u64 slba = ((u64)tr[i].ntr_SectorHi << 32) | tr[i].ntr_SectorLo;
            ULONG blocks = tr[i].ntr_Count;

            if (blocks == 0 ||
                (unit->logicalSectors > 0 && slba + blocks > unit->logicalSectors))
            {
                KprintfH("[nvme] %s: TRIM range[%lu] invalid (lba=0x%08lx%08lx blocks=%lu)\n",
                         __func__, i, (ULONG)(slba >> 32), (ULONG)slba, blocks);
                rerr = IOERR_BADADDRESS;
                break;
            }
            ranges[i].cattr = le32(0);
            ranges[i].nlb = le32(blocks);
            ranges[i].slba = le64(slba);
        }
        if (rerr)
        {
            dma_free(ctrl->memoryPool, ranges);
            reply_io(io, rerr);
            break;
        }

        BYTE error = nvme_io_submit_dsm(unit, io, ranges, (u16)nr);
        if (error != NVME_IO_ASYNC)
            reply_io(io, error);
        break;
    }

    case CMD_UPDATE:
    case ETD_UPDATE:
    {
        /* AmigaOS "flush dirty buffers" -> NVMe Flush (opcode 0x00).
         * Commits the controller's Volatile Write Cache so HDToolBox's
         * RDB writes survive reboot. */
        BYTE error = nvme_io_submit_flush(unit, io);
        if (error != NVME_IO_ASYNC)
            reply_io(io, error);
        break;
    }

    case HD_SCSICMD:
    {
        BYTE scsi_err = handle_scsi_cmd(io);
        if (scsi_err != NVME_IO_ASYNC)
            reply_io(io, scsi_err);
        break;
    }

    case CMD_INTERNAL_ABORT_REQUEST:
    {
        /* The abort message carries io->io_Data = target IOStdReq.
         * nvme_io_abort searches the I/O queue's inflight table and
         * submits the Abort admin command; the target's own CQE
         * (NVME_SC_ABORT_REQ) arrives asynchronously and fires the
         * normal completion path. */
        struct IOStdReq *target = (struct IOStdReq *)io->io_Data;
        KprintfH("[nvme] %s: internal abort target=%lx\n", __func__, (ULONG)target);
        if (unit && unit->ctrl)
        {
            (void)nvme_io_abort(unit->ctrl, target);
            if (unit->ctrl->memoryPool)
                pool_free(unit->ctrl->memoryPool, io);
        }
        /* Internal request: no ReplyMsg. */
        break;
    }

    default:
        Kprintf("[nvme] %s: unknown command 0x%04lx (unit %ld)\n", __func__,
                (ULONG)io->io_Command, unit->unitNumber);
        reply_io(io, IOERR_NOCMD);
        break;
    }
}
