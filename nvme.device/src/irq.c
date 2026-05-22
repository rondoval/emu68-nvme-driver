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

#include <iomem.h>
#include <debug.h>
#include <libraries/openpci.h>

#include <device.h>
#include <config.h>
#include <nvme/nvme_regs.h>

/*
 * nvme_int_isr - NVMe interrupt service routine.
 *
 * Called at interrupt level.  Confirms the controller is alive (CSTS
 * not all-ones), masks the interrupt source at the PCIe and NVMe levels,
 * then signals the controller task to drain the completion queue.
 *
 * Returns 1 if the interrupt was ours, 0 otherwise.
 */
static ULONG nvme_int_isr(struct ExecBase *execBase asm("a6"),
                          struct NVMeController *ctrl asm("a1"),
                          ULONG vector asm("d0"))
{
    (void)execBase;
    (void)vector;

    struct Library *pcielibBase = ctrl->device->pcieBase;

    ULONG csts = mmio_read32((volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_CSTS));
    if (csts == 0xFFFFFFFFUL)
        return 0;

    if (ctrl->msi_enabled)
    {
        MaskMSI(ctrl->pci_dev);
    }
    else if (!CheckSetINTxMask(ctrl->pci_dev, TRUE))
    {
        KprintfH("[nvme] %s: failed to mask INTx\n", __func__);
    }

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));

    Signal(ctrl->task, 1UL << ctrl->irq_signal);
    return 1;
}

static void nvme_setup_isr(struct NVMeController *ctrl)
{
    ctrl->irq_isr.is_Node.ln_Type = NT_INTERRUPT;
    ctrl->irq_isr.is_Node.ln_Name = "nvme_isr";
    ctrl->irq_isr.is_Data = ctrl;
    ctrl->irq_isr.is_Code = (APTR)nvme_int_isr;
}

static s32 nvme_pci_int_enable(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;

    if (DEVICE_USE_MSI && EnableMSI(ctrl->pci_dev) == 0)
    {
        Kprintf("[nvme] %s: MSI enabled\n", __func__);
        ctrl->msi_enabled = TRUE;
    }
    else
    {
        Kprintf("[nvme] %s: MSI unavailable, using INTx\n", __func__);
    }

    if (!pci_add_intserver(&ctrl->irq_isr, ctrl->pci_dev))
    {
        Kprintf("[nvme] %s: pci_add_intserver failed\n", __func__);
        return -1;
    }

    return ERR_NO_ERROR;
}

/*
 * nvme_int_enable - set up and enable the interrupt handler for a controller.
 *
 * Tries MSI first with INTx fallback.  After the interrupt server is
 * registered, unmasks the controller-level interrupt via NVME_REG_INTMC.
 */
s32 nvme_int_enable(struct NVMeController *ctrl)
{
    nvme_setup_isr(ctrl);

    s32 result = nvme_pci_int_enable(ctrl);
    if (result < 0)
        return result;

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
    return 0;
}

/*
 * nvme_int_shutdown - disable and remove the interrupt handler.
 */
void nvme_int_shutdown(struct NVMeController *ctrl)
{
    if (!ctrl || !ctrl->bar0)
        return;

    struct Library *pcielibBase = ctrl->device->pcieBase;

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));
    pci_rem_intserver(&ctrl->irq_isr, ctrl->pci_dev);

    if (ctrl->msi_enabled)
        DisableMSI(ctrl->pci_dev);
    ctrl->msi_enabled = FALSE;
}

/*
 * nvme_int_rearm - re-enable the interrupt source after completion processing.
 *
 * Called from the controller task after nvme_process_completions() returns.
 */
void nvme_int_rearm(struct NVMeController *ctrl)
{
    struct Library *pcielibBase = ctrl->device->pcieBase;

    if (ctrl->msi_enabled)
    {
        UnmaskMSI(ctrl->pci_dev);
    }
    else if (!CheckSetINTxMask(ctrl->pci_dev, FALSE))
    {
        /* INTx unmask failed — re-signal ourselves so the task retries */
        Signal(ctrl->task, 1UL << ctrl->irq_signal);
        return;
    }

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
}
