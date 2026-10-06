// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/bcmpcie_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from its context's sysBase */
#include <proto/exec.h>
#define BCMPCIE_BASE_NAME pcielibBase
#include <proto/bcmpcie.h>
#endif

#include <libraries/pci_constants.h> /* PCI_IRQ_* flags */
#include <libraries/pci_irq.h>

#include <intserver.h>

#include "nvme/nvme_ctrl.h"
#include "device.h"

/*
 * What INTMS/INTMC are written with: every vector, not vector 0 alone.
 *
 * Pin-based mode defines only vector 0 (NVMe 1.4c 7.5.1: IS[0] drives the
 * line, and Create I/O CQ takes IV = 0), so bit 0 ought to be enough.  The
 * Realtek 10ec:5765 raises its I/O queue's interrupt on the bit of the queue
 * ID instead, and drives the pin from that too: masking bit 0 quiets admin
 * completions, but the first I/O completion leaves the line asserted, and the
 * interrupt then repeats without end, keeping the unit task from ever running
 * to acknowledge it.  A controller that follows the specification has nothing
 * behind the other bits, so writing them all costs it nothing.
 */
#define NVME_INTM_ALL_VECTORS 0xFFFFFFFFUL

/*
 * nvme_int_isr - NVMe interrupt service routine.
 *
 * Called at interrupt level.  Masks the interrupt source so completions that
 * land during the drain don't re-interrupt (this is the natural coalescing
 * that lets one IRQ reap a whole burst), then signals the controller task to
 * drain the completion queue.  nvme_int_rearm unmasks after the drain.
 *
 * MSI/MSI-X vs INTx split:
 *   - MSI/MSI-X mask the vector at the root complex (MaskIntVector: a local
 *     register write; a message arriving while masked fires on unmask).  The
 *     NVMe INTMS/INTMC registers are not touched: the spec forbids host
 *     access to them in MSI-X mode.  A vector is ours alone, so there is
 *     nothing to check before claiming the interrupt.
 *   - INTx is level-triggered: it masks at the controller (INTMS), which
 *     deasserts the pin.  (MaskIntVector on INTx is a config-space access and
 *     not allowed from an interrupt server.)  An INTx line can be shared with
 *     another function, so this path first asks whether the interrupt is ours
 *     at all and reports not-handled if it is not, leaving the rest of the
 *     chain to run.  NVMe has no interrupt-status register; the pin is
 *     asserted only while a completion queue holds entries we have not
 *     consumed, so a fresh CQE on either queue is the test.  It also covers
 *     the controller that has gone away: no CQEs, nothing to mask or drain.
 */
static EMU68_INTSERVER(nvme_int_isr)
ULONG nvme_int_isr(struct ExecBase *SysBase asm("a6"),
                   struct NVMeController *ctrl asm("a1"),
                   ULONG vector asm("d0"))
{
    (void)vector;

#ifdef PROFILE
    ctrl->irq_stamp = get_time() | 1; /* 0 means "no stamp" */
#endif

    if (likely(ctrl->msi_enabled))
    {
        struct Library *pcielibBase = ctrl->device->pcieBase;
        MaskIntVector(ctrl->pci_dev, 0);
        Signal(ctrl->unit_task, 1UL << ctrl->irq_signal);
        return 1;
    }

    if (!nvme_cq_pending(&ctrl->io_q) && !nvme_cq_pending(&ctrl->admin_q))
        return 0; /* not ours: let a shared line's chain walk go on */

    mmio_write32(NVME_INTM_ALL_VECTORS, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));

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

    /* Start with the controller-level mask clear.  Not under MSI-X: the spec
     * forbids host access to INTMS/INTMC there (the MSI-X entry, opened by the
     * attach, is the gate). */
    if (itype != PCI_IRQ_MSIX)
        mmio_write32(NVME_INTM_ALL_VECTORS, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
    Kprintf("[nvme] %s: using %s\n", __func__,
            itype == PCI_IRQ_MSIX ? "MSI-X" : itype == PCI_IRQ_MSI ? "MSI"
                                                                   : "INTx");

    return ERR_NO_ERROR;
}

/*
 * nvme_int_enable - set up and enable the interrupt handler for a controller.
 *
 * Tries MSI-X, then MSI, then INTx.  nvme_pci_int_enable registers the
 * interrupt server and opens the controller-level mask where the mode allows.
 */
s32 nvme_int_enable(struct NVMeController *ctrl)
{
    nvme_setup_isr(ctrl);

    s32 result = nvme_pci_int_enable(ctrl);
    if (result < 0)
        return result;

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

    /* INTx: quiet the pin at the controller.  MSI/MSI-X: the detach closes the
     * vector at the device (and INTMS is off limits under MSI-X). */
    if (!ctrl->msi_enabled)
        mmio_write32(NVME_INTM_ALL_VECTORS, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMS));
    pci_irq_detach(pcielibBase, ctrl->pci_dev, &ctrl->irq_isr);
    ctrl->msi_enabled = FALSE;
}

/*
 * nvme_int_rearm - re-enable the interrupt source after completion processing.
 *
 * Called from the controller task after nvme_process_completions() returns.
 * Undoes the ISR's mask (the root-complex vector mask for MSI/MSI-X, INTMC for
 * INTx); if completions arrived while masked, the interrupt fires again so the
 * stragglers are drained on the next pass.
 */
void nvme_int_rearm(struct NVMeController *ctrl)
{
    if (likely(ctrl->msi_enabled))
    {
        struct Library *pcielibBase = ctrl->device->pcieBase;
        UnmaskIntVector(ctrl->pci_dev, 0);
    }
    else
        mmio_write32(NVME_INTM_ALL_VECTORS, (volatile u32 *)((ULONG)ctrl->bar0 + NVME_REG_INTMC));
}
