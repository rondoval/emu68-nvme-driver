// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/bcmpcie_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#define BCMPCIE_BASE_NAME pcielibBase
#include <proto/bcmpcie.h>
#endif

#include <exec/execbase.h>
#include <exec/types.h>
#include <errors.h>
#include <iomem.h>
#include <types.h>
#include <debug.h>

#include <libraries/pci_constants.h>
#include <libraries/openpci.h>
#include <libraries/pcitags.h>
#include <utility/tagitem.h>

#include "../include/device.h"
#include "../include/config.h"

/*
 * Verify that the PCIe device is responding to config space reads.
 */
static BOOL nvme_pci_is_supported(struct Library *pcielibBase, struct pci_dev *pd)
{
    if (pd->vendor == 0xFFFFU && pd->device == 0xFFFFU)
    {
        Kprintf("[nvme] %s: device not responding to config space reads\n", __func__);
        return FALSE;
    }

    UBYTE revision = pci_read_config_byte(PCI_REVISION_ID, pd);
    UBYTE prog_if = pci_read_config_byte(PCI_CLASS_PROG, pd);
    UBYTE subclass = pci_read_config_byte((UBYTE)PCI_CLASS_DEVICE, pd);
    UBYTE baseclass = pci_read_config_byte((UBYTE)(PCI_CLASS_DEVICE + 1), pd);

    Kprintf("[nvme] %s: %04lx:%04lx class=%02lx:%02lx:%02lx rev=%02lx\n", __func__,
            (ULONG)pd->vendor, (ULONG)pd->device,
            (ULONG)baseclass, (ULONG)subclass, (ULONG)prog_if, (ULONG)revision);

    return TRUE;
}

/*
 * Bring the controller hardware online: take PCIe board ownership,
 * validate BAR0, and enable bus mastering.
 *
 * Called on the first UnitOpen for a given controller.
 */
static s32 nvme_ctrl_hw_init(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;
    struct pci_dev *pd = ctrl->pci_dev;

    if (!nvme_pci_is_supported(pcielibBase, pd))
    {
        Kprintf("[nvme] %s: unsupported controller\n", __func__);
        return ERR_CONTROLLER_ERROR;
    }

    if (!SetBoardAttrs(pd, PRM_BoardOwner, (ULONG)FindTask(NULL), TAG_DONE))
    {
        Kprintf("[nvme] %s: device already owned by another task\n", __func__);
        return ERR_CONTROLLER_ERROR;
    }

    if (!pd->base_address[0])
    {
        Kprintf("[nvme] %s: BAR0 not mapped\n", __func__);
        SetBoardAttrs(pd, PRM_BoardOwner, 0UL, TAG_DONE);
        return ERR_CONTROLLER_ERROR;
    }

    pci_set_master(pd);
    ctrl->bar0 = (volatile void *)(ULONG)pd->base_address[0];

    Kprintf("[nvme] %s: BAR0=%lx\n", __func__, (ULONG)ctrl->bar0);
    return ERR_NO_ERROR;
}

/*
 * Release PCIe hardware resources for a controller.
 *
 * Called on the last UnitClose.  The NVMeController struct itself is
 * NOT freed here — it persists until device expunge.
 */
static void nvme_ctrl_hw_shutdown(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;
    if (pcielibBase && ctrl->pci_dev)
        SetBoardAttrs(ctrl->pci_dev, PRM_BoardOwner, 0UL, TAG_DONE);
    ctrl->bar0 = NULL;
}

/*
 * UnitOpen - open or re-open a namespace unit.
 *
 * On the first open of any namespace on a controller: take PCIe board
 * ownership, map BAR0, create the shared memory pool, start the shared
 * unit task, and enable interrupts.
 *
 * Subsequent opens of the same or a sibling namespace on the same
 * controller just increment the reference counts.
 */
s32 UnitOpen(struct NVMeUnit *unit, LONG unitNumber, LONG flags)
{
    Kprintf("[nvme] %s: unit %ld nsid %lu flags=0x%lx\n", __func__, unitNumber, unit->nsid, flags);

    struct NVMeController *ctrl = unit->ctrl;

    if (unit->unit.unit_OpenCnt > 0)
    {
        unit->unit.unit_OpenCnt++;
        ctrl->openUnits++;
        return ERR_NO_ERROR;
    }

    unit->flags = flags;
    s32 error = ERR_NO_ERROR;

    // TODO controller semaphore?
    if (ctrl->openUnits == 0)
    {
        error = nvme_ctrl_hw_init(ctrl);
        if (error != ERR_NO_ERROR)
            return error;

        ctrl->memoryPool = CreatePool(MEMF_FAST | MEMF_PUBLIC, 256 * 1024, 8192);
        if (!ctrl->memoryPool)
        {
            Kprintf("[nvme] %s: failed to create memory pool\n", __func__);
            error = ERR_ALLOC_ERROR;
            goto ctrl_shutdown;
        }

        error = UnitTaskStart(ctrl);
        if (error != ERR_NO_ERROR)
        {
            Kprintf("[nvme] %s: failed to start unit task (%ld)\n", __func__, error);
            goto pool_delete;
        }

        error = nvme_int_enable(ctrl);
        if (error != ERR_NO_ERROR)
        {
            Kprintf("[nvme] %s: failed to enable interrupts (%ld)\n", __func__, (LONG)error);
            goto task_stop;
        }

        /* Phase 2: enable controller (CC.EN=1), wait for CSTS.RDY,
         * send Identify Controller + Namespace, populate geometry fields,
         * create I/O queues for this namespace. */
    }

    ctrl->openUnits++;
    unit->unit.unit_OpenCnt = 1;

    Kprintf("[nvme] %s: unit %ld opened (ctrl openUnits=%lu)\n", __func__, unitNumber, ctrl->openUnits);
    return ERR_NO_ERROR;

task_stop:
    UnitTaskStop(ctrl);
pool_delete:
    DeletePool(ctrl->memoryPool);
    ctrl->memoryPool = NULL;
ctrl_shutdown:
    nvme_ctrl_hw_shutdown(ctrl);
    return error;
}

/*
 * UnitClose - close a namespace unit.
 *
 * On the last close of any namespace on a given controller: shut down
 * the interrupt handler, unit task, and memory pool, then release PCIe
 * board ownership.
 *
 * The NVMeUnit and NVMeController structs are NOT freed here — they
 * persist until device expunge and may be reopened.
 *
 * Returns the new unit open count (0 means the namespace is now idle).
 */
s32 UnitClose(struct NVMeUnit *unit)
{
    struct NVMeController *ctrl = unit->ctrl;

    Kprintf("[nvme] %s: unit %ld (openCnt=%lu ctrl openUnits=%lu)\n", __func__,
            unit->unitNumber, (ULONG)unit->unit.unit_OpenCnt, ctrl->openUnits);

    unit->unit.unit_OpenCnt--;
    ctrl->openUnits--;

    if (ctrl->openUnits == 0)
    {
        Kprintf("[nvme] %s: last namespace on controller closing, tearing down\n", __func__);

        nvme_int_shutdown(ctrl);
        UnitTaskStop(ctrl);
        nvme_ctrl_hw_shutdown(ctrl);

        DeletePool(ctrl->memoryPool);
        ctrl->memoryPool = NULL;
    }

    return (s32)unit->unit.unit_OpenCnt;
}
