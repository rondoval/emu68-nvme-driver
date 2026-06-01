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

#include "nvme/nvme_ctrl.h"
#include "device.h"

/*
 * nvme_int_isr - NVMe interrupt service routine.
 *
 * Called at interrupt level.  Masks the interrupt source so completions that
 * land during the drain don't re-interrupt (this is the natural coalescing
 * that lets one IRQ reap a whole burst), then signals the controller task to
 * drain the completion queue.
 *
 * MSI vs INTx split: MSI is edge-triggered and not shared, so there's
 * no surprise-removal CSTS probe to perform and no need for the per-vector
 * PCIe-config MaskMSI.  Masking at the NVMe level (INTMS) already suppresses 
 * further interrupts for MSI and pin-based modes alike.  INTx keeps the full path:
 * it is level-triggered and may share the line, so the all-ones CSTS probe
 * (surprise-removal) and the PCIe-pin mask still matter.
 *
 * Returns 1 if the interrupt was ours, 0 otherwise.
 */
static ULONG nvme_int_isr(struct ExecBase *execBase asm("a6"),
                          struct NVMeController *ctrl asm("a1"),
                          ULONG vector asm("d0"))
{
    (void)execBase;
    (void)vector;

    if (likely(ctrl->msi_enabled))
    {
        mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));
        Signal(ctrl->unit_task, 1UL << ctrl->irq_signal);
        return 1;
    }

    struct Library *pcielibBase = ctrl->device->pcieBase;

    ULONG csts = mmio_read32((volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_CSTS));
    if (csts == 0xFFFFFFFFUL)
        return 0;

    if (!CheckSetINTxMask(ctrl->pci_dev, TRUE))
    {
        KprintfH("[nvme] %s: failed to mask INTx\n", __func__);
    }

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));

    Signal(ctrl->unit_task, 1UL << ctrl->irq_signal);
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

    if (ctrl->quirks & NVME_QUIRK_BROKEN_MSI)
    {
        /* Device advertises MSI but never fires it — skip MSI and use INTx. */
        Kprintf("[nvme] %s: NVME_QUIRK_BROKEN_MSI set, forcing INTx\n", __func__);
    }
    else if (DEVICE_USE_MSI && EnableMSI(ctrl->pci_dev) == 0)
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
 * Mirrors the ISR's MSI/INTx split: MSI clears only the NVMe-level mask
 * (INTMC); INTx also unmasks the PCIe pin.  If completions arrived while
 * masked, clearing INTMC re-raises the interrupt so the stragglers are
 * drained on the next pass.
 */
void nvme_int_rearm(struct NVMeController *ctrl)
{
    if (ctrl->msi_enabled)
    {
        mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
        return;
    }

    struct Library *pcielibBase = ctrl->device->pcieBase;

    if (!CheckSetINTxMask(ctrl->pci_dev, FALSE))
    {
        /* INTx unmask failed — re-signal ourselves so the task retries */
        Signal(ctrl->unit_task, 1UL << ctrl->irq_signal);
        return;
    }

    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
}
