// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/io.h>

#include <memory.h>

#include "device.h"
#include "nvme/nvme_ctrl.h"

/*
 * UnitOpen - open or re-open a namespace unit.
 *
 * the heavy lifting (PCIe ownership, BAR0 map, memory pool,
 * controller task, admin/IO queues, namespace scan) all happened at
 * probe time in nvme_probe_controller.  UnitOpen now just bumps the
 * open counters.  If probe failed for this controller, ctrl is
 * NULL and we refuse the open.
 */
s32 UnitOpen(struct NVMeUnit *unit, LONG unitNumber, LONG flags)
{
#ifndef DEBUG
    (void)unitNumber; /* only referenced by debug logging */
#endif
    struct NVMeController *ctrl = unit->ctrl;

    KprintfH("[nvme] UnitOpen: unit=%lx unitNumber=%ld nsid=%lu flags=0x%lx ctrl=%lx\n",
             (ULONG)unit, unitNumber, unit->nsid, flags,
             (ULONG)ctrl);

    if (!ctrl)
    {
        Kprintf("[nvme] %s: controller %lx never brought up\n",
                __func__, (ULONG)ctrl->pci_dev);
        return ERR_CONTROLLER_ERROR;
    }

    /* Belt-and-suspenders: nvme_ns_remove unlinks dead units from
     * base->units so openLib's walk won't find them, but a holder that
     * obtained the pointer some other way still funnels through here. */
    if (unit->flags & NVME_UNIT_DEAD)
    {
        Kprintf("[nvme] %s: unit %ld is dead (namespace removed)\n",
                __func__, unitNumber);
        return ERR_CONTROLLER_ERROR;
    }

    unit->flags = flags;
    unit->unit.unit_OpenCnt++;
    ctrl->openUnits++;

    return ERR_NO_ERROR;
}

/*
 * nvme_unit_close_flush - synchronous NVMe Flush through the unit task.
 *
 * Issued on the last close of a unit so data in the drive's volatile
 * write cache reaches NAND even if power is pulled later.  Runs in the
 * CloseDevice caller's context (never ctrl->task), so waiting is safe.
 * Skipped when the controller has no volatile write cache.
 */
static void nvme_unit_close_flush(struct NVMeUnit *unit)
{
    struct NVMeController *ctrl = unit->ctrl;

    if (!(ctrl->vwc & NVME_CTRL_VWC_PRESENT))
        return;
    if (unit->flags & NVME_UNIT_DEAD)
        return;
    if (!ctrl->unit_task)
        return;

    struct MsgPort *port = CreateMsgPort();
    if (!port)
        return;

    struct IOStdReq io;
    memset(&io, 0, sizeof(io));
    io.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io.io_Message.mn_ReplyPort = port;
    io.io_Message.mn_Length = sizeof(io);
    io.io_Unit = (struct Unit *)unit;
    io.io_Command = CMD_UPDATE;

    KprintfH("[nvme] %s: flushing unit %ld on last close\n",
             __func__, unit->unitNumber);
    PutMsg(ctrl->msgPort, &io.io_Message);
    WaitPort(port);
    GetMsg(port);
    DeleteMsgPort(port);
}

/*
 * UnitClose - close a namespace unit.
 *
 * the controller stays brought up until device expunge.
 * UnitClose decrements the counters; on the last close it flushes the
 * drive's volatile write cache.  Teardown lives in nvme_unprobe_all().
 *
 * Returns the new unit open count (0 means the namespace is now idle).
 */
s32 UnitClose(struct NVMeUnit *unit)
{
    struct NVMeController *ctrl = unit->ctrl;

    KprintfH("[nvme] UnitClose: unit=%lx unitNumber=%ld ctrl=%lx\n",
             (ULONG)unit, unit->unitNumber, (ULONG)ctrl);

    if (unit->unit.unit_OpenCnt > 0)
        unit->unit.unit_OpenCnt--;
    if (ctrl->openUnits > 0)
        ctrl->openUnits--;

    if (unit->unit.unit_OpenCnt == 0)
        nvme_unit_close_flush(unit);

    return (s32)unit->unit.unit_OpenCnt;
}
