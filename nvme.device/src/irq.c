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

#include <libraries/pci_constants.h> /* PCI_IRQ_* flags */

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

    /* Allowed interrupt types, from build config.  The broken-MSI quirk drops
     * plain MSI, so a quirked device falls back to MSI-X (preferred anyway) or
     * INTx and never uses its dead single-message MSI. */
    ULONG flags = PCI_IRQ_INTX;
    if (DEVICE_USE_MSI && !(ctrl->quirks & NVME_QUIRK_BROKEN_MSI))
        flags |= PCI_IRQ_MSI;
    if (DEVICE_USE_MSIX)
        flags |= PCI_IRQ_MSIX;

    LONG nvec = AllocIntVectors(ctrl->pci_dev, 1, 1, flags);
    if (nvec < 1)
    {
        Kprintf("[nvme] %s: AllocIntVectors failed (%ld)\n", __func__, (LONG)nvec);
        return -1;
    }

    /* Message-signalled (MSI or MSI-X) vs INTx steers the ISR's masking path. */
    ULONG itype = GetIntVectorType(ctrl->pci_dev);
    ctrl->msi_enabled = (itype != PCI_IRQ_INTX);
    Kprintf("[nvme] %s: using %s\n", __func__,
            itype == PCI_IRQ_MSIX ? "MSI-X" : itype == PCI_IRQ_MSI ? "MSI" : "INTx");

    if (AddIntVectorServer(ctrl->pci_dev, 0, &ctrl->irq_isr) != 0)
    {
        Kprintf("[nvme] %s: AddIntVectorServer failed\n", __func__);
        FreeIntVectors(ctrl->pci_dev);
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
    RemIntVectorServer(ctrl->pci_dev, 0, &ctrl->irq_isr);
    FreeIntVectors(ctrl->pci_dev);
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
