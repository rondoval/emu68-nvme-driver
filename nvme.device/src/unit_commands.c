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
#include <debug.h>
#include <memory.h>

#include <device.h>
#include <nvme/nvme_io.h>
#include <nvme/nvme.h>       /* struct nvme_command, nvme_submit_sync_cmd */
#include <nvme/nvme_linux.h> /* nvme_admin_abort_cmd, struct nvme_command body */

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

        if (blockCount == 0)
        {
            KprintfH("[nvme] %s: blockCount is zero (io_Length=%lu blockShift=%lu)\n",
                     __func__, (ULONG)io->io_Length, (ULONG)unit->blockShift);
            reply_io(io, IOERR_BADLENGTH);
            break;
        }
        if (unit->logicalSectors > 0 && lba + blockCount > unit->logicalSectors)
        {
            KprintfH("[nvme] %s: LBA out of range (lba=0x%08lx%08lx blockCount=%lu logicalSectors=%lu)\n",
                     __func__, (ULONG)(lba >> 32), (ULONG)lba, blockCount, (ULONG)unit->logicalSectors);
            reply_io(io, IOERR_BADADDRESS);
            break;
        }

        BYTE error = nvme_io_submit_rw(unit, io, lba, blockCount,
                                       direction == READ ? nvme_cmd_read : nvme_cmd_write,
                                       io->io_Data);
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

        if (ctrl && ctrl->memoryPool)
            pool_free(ctrl->memoryPool, io);
        /* Do NOT ReplyMsg — this is an internal request, not a caller request */
        break;
    }

    default:
        Kprintf("[nvme] %s: unknown command 0x%04lx (unit %ld)\n", __func__,
                (ULONG)io->io_Command, unit->unitNumber);
        reply_io(io, IOERR_NOCMD);
        break;
    }
}
