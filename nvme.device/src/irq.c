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
#include <libraries/pci_irq.h>

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
 * MSI vs INTx split: MSI is edge-triggered and not shared, so there's no
 * surprise-removal CSTS probe to perform.  INTx is level-triggered and may
 * share the gic line, so it keeps the all-ones CSTS probe, which doubles as the
 * "is this interrupt ours?" check for the shared case (we return 0 and let the
 * next server run when it isn't).
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

    ULONG csts = mmio_read32((volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_CSTS));
    if (csts == 0xFFFFFFFFUL)
        return 0;

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

    ULONG itype = 0;
    LONG rc = pci_irq_attach(pcielibBase, ctrl->pci_dev, &ctrl->irq_isr, flags, &itype);
    if (rc != 0)
    {
        Kprintf("[nvme] %s: interrupt attach failed: %s (%ld)\n", __func__,
                pcie_strerror(rc), rc);
        return -1;
    }

    /* Message-signalled (MSI or MSI-X) vs INTx steers the ISR's masking path. */
    ctrl->msi_enabled = (itype != PCI_IRQ_INTX);
    Kprintf("[nvme] %s: using %s\n", __func__,
            itype == PCI_IRQ_MSIX ? "MSI-X" : itype == PCI_IRQ_MSI ? "MSI"
                                                                   : "INTx");

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
    pci_irq_detach(pcielibBase, ctrl->pci_dev, &ctrl->irq_isr);
    ctrl->msi_enabled = FALSE;
}

/*
 * nvme_int_rearm - re-enable the interrupt source after completion processing.
 *
 * Called from the controller task after nvme_process_completions() returns.
 * Clearing the NVMe-level mask (INTMC) rearms the source for MSI and INTx
 * alike; if completions arrived while masked, it re-raises the interrupt so the
 * stragglers are drained on the next pass.
 */
void nvme_int_rearm(struct NVMeController *ctrl)
{
    mmio_write32(1UL, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
}
