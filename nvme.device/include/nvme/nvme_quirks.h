// SPDX-License-Identifier: GPL-2.0
#ifndef NVME_QUIRKS_H
#define NVME_QUIRKS_H

struct pci_dev;      /* OpenPCI descriptor; only the pointer is needed here */
struct nvme_id_ctrl; /* Identify Controller data (nvme_defs.h) */

/*
 * nvme_lookup_quirks - return the NVME_QUIRK_* bitmask for a PCI device
 *
 * Matches @pdev->vendor / @pdev->device against the built-in PCI ID
 * table (ported from the Linux nvme_id_table).  Returns 0 if the device is not
 * listed.  The result is meant to seed ctrl->quirks via nvme_init_ctrl().
 *
 * @pdev: OpenPCI descriptor of the NVMe function being probed
 * Returns: OR of NVME_QUIRK_* bits, or 0 when no table entry matches
 */
unsigned long nvme_lookup_quirks(const struct pci_dev *pdev);

/*
 * nvme_match_id_quirks - return the NVME_QUIRK_* bitmask for a controller's identity
 *
 * Matches the Identify Controller vid + model + firmware strings
 * against the built-in core_quirks table, for workarounds that vary by
 * firmware/model under a shared PCI ID.  Call after a successful Identify
 * Controller and OR the result into ctrl->quirks.
 *
 * @id: parsed Identify Controller data
 * Returns: OR of NVME_QUIRK_* bits, or 0 when no entry matches
 */
unsigned long nvme_match_id_quirks(const struct nvme_id_ctrl *id);

#endif /* NVME_QUIRKS_H */
