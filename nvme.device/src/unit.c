// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/execbase.h>
#include <exec/types.h>

#include <types.h>
#include <debug.h>

#include <device.h>

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
    struct NVMeController *ctrl = unit->ctrl;

    KprintfH("[nvme] UnitOpen: unit=%lx unitNumber=%ld nsid=%lu flags=0x%lx ctrl=%lx\n",
             (ULONG)unit, unitNumber, unit->nsid, flags,
             (ULONG)ctrl);

    if (!ctrl) {
        Kprintf("[nvme] %s: controller %lx never brought up\n",
                __func__, (ULONG)ctrl->pci_dev);
        return ERR_CONTROLLER_ERROR;
    }

    unit->flags = flags;
    unit->unit.unit_OpenCnt++;
    ctrl->openUnits++;

    return ERR_NO_ERROR;
}

/*
 * UnitClose - close a namespace unit.
 *
 * the controller stays brought up until device expunge.
 * UnitClose just decrements the counters.  Teardown lives in
 * nvme_unprobe_all().
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

    return (s32)unit->unit.unit_OpenCnt;
}
