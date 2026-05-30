// SPDX-License-Identifier: GPL-2.0
/*
 * nvme_quirks.c — per-device workaround (quirk) tables and lookups.
 *
 * Two complementary mechanisms, both ported from the Linux nvme driver, feed
 * ctrl->quirks so the (already-wired) consumers in nvme_identify.c /
 * nvme_probe.c / nvme_ctrl.c act on affected devices:
 *
 *   - nvme_lookup_quirks()   — keys on PCI vendor:device, against a table ported
 *                              from Linux nvme_id_table[].  Called from
 *                              nvme_probe_controller() to seed ctrl->quirks.
 *   - nvme_match_id_quirks() — keys on the Identify Controller model/firmware
 *                              strings, for workarounds that vary by firmware or
 *                              model under a PCI ID shared with other devices.
 *                              Called from nvme_init_identify().
 */
#include <nvme/nvme_core.h>        /* must come first: exec types, ARRAY_SIZE, Kprintf */

#include <libraries/openpci.h>     /* struct pci_dev (vendor/device) */
#include <libraries/pci_ids.h>     /* PCI_VENDOR_ID_* */

#include <nvme/nvme_ctrl.h>        /* enum nvme_quirks */
#include <nvme/nvme_quirks.h>

/* Amazon's PCI vendor ID is not defined in the reachable headers. */
#ifndef PCI_VENDOR_ID_AMAZON
#define PCI_VENDOR_ID_AMAZON 0x1d0f
#endif

/*
 * File-local match macros so the table rows copy near-verbatim from the Linux
 * nvme_id_table.  struct pci_dev has no subsystem fields, so we key on
 * vendor+device only (subvendor/subdevice/class wildcards are dropped).  Kept
 * out of the header so they cannot collide with the pcie-library macros of the
 * same name elsewhere.
 */
#define PCI_VDEVICE(vend, dev) .vendor = PCI_VENDOR_ID_##vend, .device = (dev)
#define PCI_DEVICE(vend, dev)  .vendor = (vend), .device = (dev)

struct nvme_quirk_id {
	u16 vendor;
	u16 device;
	unsigned long driver_data; /* OR of NVME_QUIRK_* */
};

/*
 * Ported from Linux nvme_id_table[].  The generic
 * PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, ...) catch-all and the {0,}
 * sentinel are intentionally omitted: discovery already class-filters, and a
 * non-match returns 0 quirks.  A few vendor:device pairs appear twice — that is
 * harmless, the duplicates carry the same bit and first-match wins (as in
 * Linux pci_match_id).
 */
static const struct nvme_quirk_id nvme_quirk_ids[] = {
	{ PCI_VDEVICE(INTEL, 0x0953),	/* Intel 750/P3500/P3600/P3700 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a53),	/* Intel P3520 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_DEALLOCATE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0x0a54),	/* Intel P4500/P4600 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE |
				NVME_QUIRK_IGNORE_DEV_SUBNQN |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_VDEVICE(INTEL, 0x0a55),	/* Dell Express Flash P4600 */
		.driver_data = NVME_QUIRK_STRIPE_SIZE, },
	{ PCI_VDEVICE(INTEL, 0xf1a5),	/* Intel 600P/P3100 */
		.driver_data = NVME_QUIRK_MEDIUM_PRIO_SQ |
				NVME_QUIRK_NO_TEMP_THRESH_CHANGE |
				NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_VDEVICE(INTEL, 0xf1a6),	/* Intel 760p/Pro 7600p */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_VDEVICE(INTEL, 0x5845),	/* Qemu emulated controller */
		.driver_data = NVME_QUIRK_IDENTIFY_CNS |
				NVME_QUIRK_DISABLE_WRITE_ZEROES |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_VDEVICE(REDHAT, 0x0010),	/* Qemu emulated controller */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1217, 0x8760), /* O2 Micro 64GB Steam Deck */
		.driver_data = NVME_QUIRK_DMAPOOL_ALIGN_512, },
	{ PCI_DEVICE(0x126f, 0x1001),	/* Silicon Motion generic */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_DEVICE(0x126f, 0x2262),	/* Silicon Motion generic */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x126f, 0x2263),	/* Silicon Motion unidentified */
		.driver_data = NVME_QUIRK_NO_NS_DESC_LIST |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1bb1, 0x0100),   /* Seagate Nytro Flash Storage */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
				NVME_QUIRK_NO_NS_DESC_LIST, },
	{ PCI_DEVICE(0x1c58, 0x0003),	/* HGST adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1c58, 0x0023),	/* WDC SN200 adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x1c5f, 0x0540),	/* Memblaze Pblaze4 adapter */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x144d, 0xa821),   /* Samsung PM1725 */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY, },
	{ PCI_DEVICE(0x144d, 0xa822),   /* Samsung PM1725a */
		.driver_data = NVME_QUIRK_DELAY_BEFORE_CHK_RDY |
				NVME_QUIRK_DISABLE_WRITE_ZEROES|
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_DEVICE(0x15b7, 0x5008),   /* Sandisk SN530 */
		.driver_data = NVME_QUIRK_BROKEN_MSI },
	{ PCI_DEVICE(0x15b7, 0x5009),   /* Sandisk SN550 */
		.driver_data = NVME_QUIRK_BROKEN_MSI },
	{ PCI_DEVICE(0x1987, 0x5012),	/* Phison E12 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1987, 0x5016),	/* Phison E16 */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1987, 0x5019),  /* phison E19 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1987, 0x5021),   /* Phison E21 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1b4b, 0x1092),	/* Lexar 256 GB SSD */
		.driver_data = NVME_QUIRK_NO_NS_DESC_LIST |
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_DEVICE(0x1cc1, 0x33f8),   /* ADATA IM2P33F8ABR1 1 TB */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x10ec, 0x5762),   /* ADATA SX6000LNP */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x10ec, 0x5763),  /* ADATA SX6000PNP */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1cc1, 0x8201),   /* ADATA SX8200PNP 512GB */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_DEVICE(0x1344, 0x5407), /* Micron Technology Inc NVMe SSD */
		.driver_data = NVME_QUIRK_IGNORE_DEV_SUBNQN },
	{ PCI_DEVICE(0x1344, 0x6001),   /* Micron Nitro NVMe */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1c5c, 0x1504),   /* SK Hynix PC400 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1c5c, 0x174a),   /* SK Hynix P31 SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1c5c, 0x1D59),   /* SK Hynix BC901 */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x15b7, 0x2001),   /*  Sandisk Skyhawk */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1d97, 0x2263),   /* SPCC */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x144d, 0xa80b),   /* Samsung PM9B1 256G and 512G */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES |
				NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x144d, 0xa809),   /* Samsung MZALQ256HBJD 256G */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x144d, 0xa802),   /* Samsung SM953 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1cc4, 0x6303),   /* UMIS RPJTJ512MGE1QDY 512G */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1cc4, 0x6302),   /* UMIS RPJTJ256MGE1QDY 256G */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x2646, 0x5013),   /* Kingston KC3000, Kingston FURY Renegade */
		.driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH, },
	{ PCI_DEVICE(0x2646, 0x5018),   /* KINGSTON OM8SFP4xxxxP OS21012 NVMe SSD */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x2646, 0x5016),   /* KINGSTON OM3PGP4xxxxP OS21011 NVMe SSD */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x2646, 0x501A),   /* KINGSTON OM8PGP4xxxxP OS21005 NVMe SSD */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x2646, 0x501B),   /* KINGSTON OM8PGP4xxxxQ OS21005 NVMe SSD */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x2646, 0x501E),   /* KINGSTON OM3PGP4xxxxQ OS21011 NVMe SSD */
		.driver_data = NVME_QUIRK_DISABLE_WRITE_ZEROES, },
	{ PCI_DEVICE(0x1f40, 0x1202),   /* Netac Technologies Co. NV3000 NVMe SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1f40, 0x5236),   /* Netac Technologies Co. NV7000 NVMe SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1e4B, 0x1001),   /* MAXIO MAP1001 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1e4B, 0x1002),   /* MAXIO MAP1002 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1e4B, 0x1202),   /* MAXIO MAP1202 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1e4B, 0x1602),   /* MAXIO MAP1602 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1cc1, 0x5350),   /* ADATA XPG GAMMIX S50 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1dbe, 0x5216),   /* Acer/INNOGRIT FA100/5216 NVMe SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1dbe, 0x5236),   /* ADATA XPG GAMMIX S70 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1fa0, 0x2283),   /* Wodposit WPBSNM8-256GTP */
		.driver_data = NVME_QUIRK_NO_SECONDARY_TEMP_THRESH, },
	{ PCI_DEVICE(0xc0a9, 0x540a),   /* Crucial P2 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1d97, 0x2263), /* Lexar NM610 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1d97, 0x1d97), /* Lexar NM620 */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1d97, 0x2269), /* Lexar NM760 */
		.driver_data = NVME_QUIRK_BOGUS_NID |
				NVME_QUIRK_IGNORE_DEV_SUBNQN, },
	{ PCI_DEVICE(0x10ec, 0x5763), /* TEAMGROUP T-FORCE CARDEA ZERO Z330 SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x1e4b, 0x1602), /* HS-SSD-FUTURE 2048G  */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(0x10ec, 0x5765), /* TEAMGROUP MP33 2TB SSD */
		.driver_data = NVME_QUIRK_BOGUS_NID, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0061),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x0065),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0x8061),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd00),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd01),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMAZON, 0xcd02),
		.driver_data = NVME_QUIRK_DMA_ADDRESS_BITS_48, },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2001),
		/*
		 * Fix for the Apple controller found in the MacBook8,1 and
		 * some MacBook7,1 to avoid controller resets and data loss.
		 */
		.driver_data = NVME_QUIRK_SINGLE_VECTOR |
				NVME_QUIRK_QDEPTH_ONE },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2003) },
	{ PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x2005),
		.driver_data = NVME_QUIRK_SINGLE_VECTOR |
				NVME_QUIRK_128_BYTES_SQES |
				NVME_QUIRK_SHARED_TAGS |
				NVME_QUIRK_SKIP_CID_GEN |
				NVME_QUIRK_IDENTIFY_CNS },
};

unsigned long nvme_lookup_quirks(const struct pci_dev *pdev)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(nvme_quirk_ids); i++) {
		if (nvme_quirk_ids[i].vendor == pdev->vendor &&
		    nvme_quirk_ids[i].device == pdev->device) {
			if (nvme_quirk_ids[i].driver_data)
				Kprintf("[nvme] %s: %04lx:%04lx -> quirks 0x%lx\n",
					__func__, (ULONG)pdev->vendor,
					(ULONG)pdev->device,
					(ULONG)nvme_quirk_ids[i].driver_data);
			return nvme_quirk_ids[i].driver_data;
		}
	}
	return 0;
}

/*
 * Identify-string quirks — matched on the Identify Controller vid + model +
 * firmware strings, for workarounds that vary by firmware/model under a PCI ID
 * shared with other devices (which the PCI ID table above cannot distinguish).
 */
struct nvme_core_quirk_entry {
	/*
	 * NVMe model and firmware strings are space-padded; for simplicity the
	 * strings in this table are NUL-terminated instead.  A NULL pattern is a
	 * wildcard.
	 */
	u16 vid;
	const char *mn;
	const char *fr;
	unsigned long quirks;
};

static const struct nvme_core_quirk_entry core_quirks[] = {
	{
		/*
		 * Samsung Portable SSD X5: fails initialisation without a delay
		 * before the readiness check.  Matched by model string because it
		 * shares its PCI ID with the internal Samsung 970 Evo Plus, which
		 * must NOT get this quirk — so it cannot go in the PCI ID table.
		 * Upstream also sets NVME_QUIRK_NO_DEEPEST_PS and
		 * NVME_QUIRK_IGNORE_DEV_SUBNQN; both are inapplicable here (this
		 * port programs no APST/power states and never reads the SUBNQN)
		 * and are omitted.
		 */
		.vid = 0x144d,
		.mn = "Samsung Portable SSD X5",
		.quirks = NVME_QUIRK_DELAY_BEFORE_CHK_RDY,
	},
};

/*
 * string_matches - compare a space-padded Identify string against a quirk pattern
 *
 * NVMe Identify Controller MN/FR are space-padded fixed-width; quirk patterns are
 * NUL-terminated.  A NULL pattern is a wildcard.  Otherwise @idstr must start
 * with @match and be space-padded to @len thereafter.  (Reimplemented without
 * strlen, which this port does not provide.)
 *
 * @idstr: space-padded field from the Identify Controller response
 * @match: NUL-terminated quirk-table pattern (may be NULL)
 * @len:   width of the Identify field
 * Returns: TRUE if the field matches the pattern
 */
static BOOL string_matches(const char *idstr, const char *match, u32 len)
{
	u32 i;

	if (!match)
		return TRUE;

	for (i = 0; match[i]; i++)
		if (i >= len || idstr[i] != match[i])
			return FALSE;

	for (; i < len; i++)
		if (idstr[i] != ' ')
			return FALSE;

	return TRUE;
}

/*
 * quirk_matches - test whether an Identify Controller response matches an entry
 *
 * @id: Identify Controller data from the device
 * @q:  quirk-table entry to compare against
 * Returns: TRUE if vid, model, and firmware all match
 */
static BOOL quirk_matches(const struct nvme_id_ctrl *id,
		const struct nvme_core_quirk_entry *q)
{
	return q->vid == le16(id->vid) &&
		string_matches(id->mn, q->mn, sizeof(id->mn)) &&
		string_matches(id->fr, q->fr, sizeof(id->fr));
}

unsigned long nvme_match_id_quirks(const struct nvme_id_ctrl *id)
{
	unsigned long quirks = 0;

	for (unsigned int i = 0; i < ARRAY_SIZE(core_quirks); i++)
		if (quirk_matches(id, &core_quirks[i]))
			quirks |= core_quirks[i].quirks;

	if (quirks)
		Kprintf("[nvme] %s: %04lx mn='%.40s' -> quirks 0x%lx\n",
			__func__, (ULONG)le16(id->vid), id->mn, (ULONG)quirks);

	return quirks;
}
