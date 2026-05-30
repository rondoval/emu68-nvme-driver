// SPDX-License-Identifier: GPL-2.0
#ifndef NVME_QUIRKS_H
#define NVME_QUIRKS_H

struct pci_dev; /* OpenPCI descriptor; only the pointer is needed here */

/*
 * nvme_lookup_quirks - return the NVME_QUIRK_* bitmask for a PCI device
 *
 * Matches @pdev->vendor / @pdev->device against the built-in quirk table
 * (ported from the Linux nvme_id_table).  Returns 0 if the device is not
 * listed.  The result is meant to seed ctrl->quirks via nvme_init_ctrl().
 *
 * @pdev: OpenPCI descriptor of the NVMe function being probed
 * Returns: OR of NVME_QUIRK_* bits, or 0 when no table entry matches
 */
unsigned long nvme_lookup_quirks(const struct pci_dev *pdev);

#endif /* NVME_QUIRKS_H */
