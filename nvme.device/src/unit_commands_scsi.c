// SPDX-License-Identifier: GPL-2.0-only
/*
 * unit_commands_scsi.c — SCSI passthrough (HD_SCSICMD) emulation.
 *
 * ProcessCommand() in unit_commands.c dispatches HD_SCSICMD here.  We
 * translate the inbound SCSICmd into the matching NVMe operation
 * (INQUIRY → identify-string cache, READ/WRITE/SYNC_CACHE/UNMAP → NVMe
 * I/O via nvme_io_submit_*) or emulate it locally (MODE SENSE, READ
 * CAPACITY, REQUEST SENSE).
 *
 * Only handle_scsi_cmd() is exposed; the per-opcode helpers are static.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <devices/trackdisk.h>
#include <debug.h>
#include <memory.h>

#include <device.h>
#include <nvme/nvme.h> /* NVME_CTRL_PAGE_SIZE */
#include <nvme/nvme_io.h>

/* ------------------------------------------------------------------ */
/* SCSI CDB opcodes (SPC-4 / SBC-3).                                   */
/* ------------------------------------------------------------------ */
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_REQUEST_SENSE 0x03
#define SCSI_CMD_READ_6 0x08
#define SCSI_CMD_WRITE_6 0x0A
#define SCSI_CMD_INQUIRY 0x12
#define SCSI_CMD_MODE_SENSE_6 0x1A
#define SCSI_CMD_START_STOP_UNIT 0x1B
#define SCSI_CMD_READ_CAPACITY_10 0x25
#define SCSI_CMD_READ_10 0x28
#define SCSI_CMD_WRITE_10 0x2A
#define SCSI_CMD_SYNC_CACHE_10 0x35
#define SCSI_CMD_MODE_SENSE_10 0x5A
#define SCSI_CMD_UNMAP 0x42
#define SCSI_CMD_READ_16 0x88
#define SCSI_CMD_WRITE_16 0x8A
#define SCSI_CMD_READ_CAPACITY_16 0x9E
#define SCSI_CHECK_CONDITION 0x02

/* ------------------------------------------------------------------ */
/* On-wire SCSI structures.                                            */
/* ------------------------------------------------------------------ */

/* SPC-4 / SBC-3 UNMAP CDB (10 bytes).  Length fields are big-endian
 * on the wire; we read them through byte-by-byte helpers. */
struct __attribute__((packed)) SCSI_CDB_UNMAP
{
    UBYTE operation; /* 0x42 */
    UBYTE anchor;    /* bit 0 = ANCHOR (reserved/unused for us) */
    UBYTE reserved2[4];
    UBYTE group;             /* bits 4:0 = GROUP NUMBER */
    UWORD param_list_length; /* big-endian, total bytes of parameter data */
    UBYTE control;
};

/* UNMAP parameter list header (SBC-3 §5.32.1).  Always 8 bytes,
 * followed by zero or more 16-byte block descriptors. */
struct __attribute__((packed)) SCSI_UNMAP_PARAM_HDR
{
    UWORD data_length;       /* big-endian; n - 2 */
    UWORD block_desc_length; /* big-endian; n - 8 (multiple of 16) */
    UBYTE reserved[4];
};

/* UNMAP block descriptor (16 bytes, big-endian fields). */
struct __attribute__((packed)) SCSI_UNMAP_DESC
{
    UBYTE lba[8];        /* big-endian u64 LBA */
    UBYTE num_blocks[4]; /* big-endian u32 block count */
    UBYTE reserved[4];
};

struct __attribute__((packed)) SCSI_Inquiry
{
    UBYTE peripheral_type;
    UBYTE removable_media;
    UBYTE version;
    UBYTE response_format;
    UBYTE additional_length;
    UBYTE flags[3];
    UBYTE vendor[8];
    UBYTE product[16];
    UBYTE revision[4];
    UBYTE serial[8];
};

struct __attribute__((packed)) SCSI_CDB_6
{
    UBYTE operation;
    UBYTE lba_high;
    UBYTE lba_mid;
    UBYTE lba_low;
    UBYTE length;
    UBYTE control;
};

struct __attribute__((packed)) SCSI_CDB_10
{
    UBYTE operation;
    UBYTE flags;
    ULONG lba;
    UBYTE group;
    UWORD length;
    UBYTE control;
};

struct __attribute__((packed)) SCSI_CDB_16
{
    UBYTE operation;
    UBYTE flags;
    uint64_t lba;
    ULONG length;
    UBYTE group;
    UBYTE control;
};

struct __attribute__((packed)) SCSI_READ_CAPACITY_10
{
    UBYTE operation;
    UBYTE reserved1;
    ULONG lba;
    UWORD reserved2;
    UBYTE flags;
    UBYTE control;
};

struct __attribute__((packed)) SCSI_READ_CAPACITY_16
{
    UBYTE operation;
    UBYTE serviceAction;
    uint64_t lba;
    ULONG allocation;
    UBYTE flags;
    UBYTE control;
};

struct __attribute__((packed)) SCSI_CAPACITY_10
{
    ULONG lba;
    ULONG block_size;
};

struct __attribute__((packed)) SCSI_CAPACITY_16
{
    uint64_t lba;
    ULONG block_size;
    char reserved[3];
};

struct __attribute__((packed)) SCSI_FIXED_SENSE
{
    UBYTE response;
    UBYTE pad;
    UBYTE senseKey;
    ULONG info;
    UBYTE additional;
    ULONG specific;
    UBYTE asc;
    UBYTE asq;
    UBYTE fru;
    UBYTE sks[3];
};

/*
 * scsi_make_sense - populate SCSI fixed-format sense data.
 */
static void scsi_make_sense(struct SCSICmd *cmd, ULONG info, ULONG specific, BYTE error)
{
    struct SCSI_FIXED_SENSE *sense = (struct SCSI_FIXED_SENSE *)cmd->scsi_SenseData;

    if (!(cmd->scsi_Flags & SCSIF_AUTOSENSE) || error == 0 || sense == NULL ||
        cmd->scsi_SenseLength < (UWORD)sizeof(struct SCSI_FIXED_SENSE))
    {
        cmd->scsi_SenseActual = 0;
        return;
    }

    cmd->scsi_SenseActual = sizeof(struct SCSI_FIXED_SENSE);

    sense->response = 0x70; /* fixed format, current status */
    sense->pad = 0;
    sense->info = info;
    sense->additional = (UBYTE)(sizeof(struct SCSI_FIXED_SENSE) - 7);
    sense->specific = specific;
    sense->fru = (UBYTE)error;

    switch (error)
    {
    case IOERR_UNITBUSY:
        sense->senseKey = 0x03;
        sense->asc = 0x04; /* unit not ready, cause not reportable */
        sense->asq = 0x00;
        break;
    case IOERR_BADADDRESS:
        sense->senseKey = 0x03;
        sense->asc = 0x21; /* LBA out of range */
        sense->asq = 0x00;
        break;
    case IOERR_NOCMD:
        sense->senseKey = 0x05; /* illegal request / invalid command op code */
        sense->asc = 0x20;
        sense->asq = 0x00;
        break;
    default:
        sense->senseKey = 0x00;
        sense->asc = 0x00;
        sense->asq = 0x00;
        break;
    }
}

/*
 * copy_padded - SCSI field copy with space-pad and source truncation.
 *
 * Both NVMe identify strings and SCSI INQUIRY fields are space-padded
 * fixed-width.  Copy up to @dstlen bytes from @src (which is itself at
 * least @dstlen bytes long), space-padding any remaining bytes.  No
 * NUL terminator.
 */
static void copy_padded(UBYTE *dst, const char *src, UWORD dstlen)
{
    UWORD i;
    for (i = 0; i < dstlen && src[i] != '\0'; i++)
        dst[i] = (UBYTE)src[i];
    for (; i < dstlen; i++)
        dst[i] = ' ';
}

/*
 * unit_supports_dsm - does the namespace's controller advertise
 * Dataset Management (Deallocate)?  Used by VPD page 0xB2 and by the
 * UNMAP handler to gate capability advertising and command acceptance.
 */
static BOOL unit_supports_dsm(struct NVMeUnit *unit)
{
    return unit && unit->ctrl && (unit->ctrl->oncs & NVME_CTRL_ONCS_DSM) != 0;
}

/* Helper: clamp the response to scsi_Length and stamp scsi_Actual. */
static void scsi_set_actual(struct SCSICmd *cmd, ULONG produced)
{
    cmd->scsi_Actual = (cmd->scsi_Length >= produced) ? produced : cmd->scsi_Length;
}

static inline void scsi_store_be16(UBYTE *dst, UWORD value)
{
    dst[0] = (UBYTE)(value >> 8);
    dst[1] = (UBYTE)value;
}

static inline void scsi_store_be32(UBYTE *dst, ULONG value)
{
    dst[0] = (UBYTE)(value >> 24);
    dst[1] = (UBYTE)(value >> 16);
    dst[2] = (UBYTE)(value >> 8);
    dst[3] = (UBYTE)value;
}

/*
 * scsi_inquiry_standard - the original "no EVPD" INQUIRY response,
 * pulled into its own helper so the EVPD dispatcher can fall through to
 * the SCSI-2 standard data when the client doesn't ask for a VPD page.
 */
static BYTE scsi_inquiry_standard(struct NVMeUnit *unit,
                                  struct SCSICmd *cmd)
{
    struct SCSI_Inquiry *data = (struct SCSI_Inquiry *)cmd->scsi_Data;
    struct NVMeController *ctrl = unit ? unit->ctrl : NULL;
    BOOL have_id = ctrl && ctrl->id_strings.model[0] != 0;

    if (data == NULL)
        return IOERR_BADADDRESS;

    data->peripheral_type = 0; /* direct-access block device */
    data->removable_media = 0;
    data->version = 2; /* SCSI-2 */
    data->response_format = 2;
    data->additional_length = (UBYTE)(sizeof(struct SCSI_Inquiry) - 4);
    data->flags[0] = data->flags[1] = data->flags[2] = 0;

    /* Vendor is always "NVMe    " (8 bytes) — the NVMe spec doesn't
     * carry a separate vendor field, just a 40-byte model that
     * usually starts with the maker. */
    CopyMem((CONST_APTR) "NVMe    ", (APTR)data->vendor, 8);

    if (have_id)
    {
        /* Product: first 16 chars of the 40-byte model string. */
        copy_padded((UBYTE *)data->product, ctrl->id_strings.model, 16);
        /* Revision: first 4 chars of the 8-byte firmware string. */
        copy_padded((UBYTE *)data->revision, ctrl->id_strings.firmware, 4);
        /* Serial: first 8 chars of the 20-byte serial string. */
        copy_padded((UBYTE *)data->serial, ctrl->id_strings.serial, 8);
    }
    else
    {
        CopyMem((CONST_APTR) "Storage Device  ", (APTR)data->product, 16);
        CopyMem((CONST_APTR) "0001", (APTR)data->revision, 4);
        CopyMem((CONST_APTR) "        ", (APTR)data->serial, 8);
    }

    scsi_set_actual(cmd, sizeof(struct SCSI_Inquiry));
    return 0;
}

/*
 * VPD page 0x00 - Supported VPD Pages.
 *
 * Lists the VPD page codes we implement.  Required by SPC-4 whenever
 * any EVPD page is supported.  We include 0x00 itself (mandatory) and
 * the two pages that matter for UNMAP discovery: 0xB0 (Block Limits)
 * and 0xB2 (Logical Block Provisioning).
 */
static BYTE scsi_inquiry_vpd_00(struct NVMeUnit *unit,
                                struct SCSICmd *cmd)
{
    UBYTE *buf = (UBYTE *)cmd->scsi_Data;
    UWORD page_len = 3;
    (void)unit;

    if (!buf)
        return IOERR_BADADDRESS;
    if (cmd->scsi_Length < (ULONG)(4 + page_len))
        page_len = (UWORD)(cmd->scsi_Length > 4 ? cmd->scsi_Length - 4 : 0);

    buf[0] = 0;    /* peripheral type = direct access block */
    buf[1] = 0x00; /* page code */
    buf[2] = 0;    /* page length MSB */
    buf[3] = (UBYTE)page_len;

    /* Supported page list — order matters: ascending. */
    if (page_len > 0)
        buf[4] = 0x00;
    if (page_len > 1)
        buf[5] = 0xB0;
    if (page_len > 2)
        buf[6] = 0xB2;

    scsi_set_actual(cmd, (ULONG)(4 + page_len));
    return 0;
}

/*
 * VPD page 0xB0 - Block Limits (SBC-3 §6.6.4).
 *
 * Tells the SCSI client our transfer and UNMAP limits.  Without this,
 * conservative clients refuse to issue UNMAP at all.  Field offsets
 * and bit positions per SBC-3 — all multi-byte fields are big-endian,
 * which on m68k matches native byte order so direct field writes work.
 *
 * 64 bytes total response length (page length field = 60).
 */
static BYTE scsi_inquiry_vpd_b0(struct NVMeUnit *unit,
                                struct SCSICmd *cmd)
{
    UBYTE *buf = (UBYTE *)cmd->scsi_Data;
    struct NVMeController *ctrl = unit ? unit->ctrl : NULL;
    ULONG max_xfer_blocks;
    ULONG copy_len;

    if (!buf || !ctrl)
        return IOERR_BADADDRESS;

    /* Build the response in a small stack buffer so we don't have to
     * worry about scsi_Length truncation mid-write. */
    UBYTE page[64];
    mem_zero(page, sizeof(page));

    page[0] = 0;                   /* peripheral type */
    page[1] = 0xB0;                /* page code */
    scsi_store_be16(&page[2], 60); /* page length (n - 3 where n = 63) */

    /* Maximum Transfer Length in blocks — derive from our internal
     * max_transfer_bytes cap (chunked I/O dispatcher splits anyway,
     * but advertising the real per-command ceiling is what the spec
     * wants).  0 means "no limit". */
    if (ctrl->max_transfer_bytes && unit->blockShift)
        max_xfer_blocks = (ULONG)(ctrl->max_transfer_bytes >> unit->blockShift);
    else
        max_xfer_blocks = 0;
    scsi_store_be32(&page[8], max_xfer_blocks); /* MAXIMUM TRANSFER LENGTH */

    /* UNMAP capability fields, gated on DSM support. */
    if (unit_supports_dsm(unit))
    {
        /* MAXIMUM UNMAP LBA COUNT: 0xFFFFFFFF = "no limit per range" */
        scsi_store_be32(&page[20], 0xFFFFFFFFUL);
        /* MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT: matches NVMe DSM cap */
        scsi_store_be32(&page[24], NVME_DSM_MAX_RANGES);
    }
    /* Everything else (optimal transfer length, write-same, etc.) we
     * leave at 0 — meaning "no preference / not supported". */

    copy_len = sizeof(page);
    if (cmd->scsi_Length < copy_len)
        copy_len = cmd->scsi_Length;
    CopyMem(page, cmd->scsi_Data, copy_len);
    scsi_set_actual(cmd, sizeof(page));
    return 0;
}

/*
 * VPD page 0xB2 - Logical Block Provisioning (SBC-3 §6.6.6).
 *
 * Advertises which deallocation primitives the device supports.
 * LBPU=1 here is what allows the SCSI client to actually emit UNMAP.
 * Provisioning Type = 2 (Thinly Provisioned) is a reasonable default
 * for any NVMe namespace — NVMe doesn't expose provisioning depth
 * directly; we just want the client to issue UNMAPs.
 */
static BYTE scsi_inquiry_vpd_b2(struct NVMeUnit *unit,
                                struct SCSICmd *cmd)
{
    UBYTE *buf = (UBYTE *)cmd->scsi_Data;
    UBYTE page[8];
    ULONG copy_len;

    if (!buf)
        return IOERR_BADADDRESS;

    mem_zero(page, sizeof(page));
    page[0] = 0;                  /* peripheral type */
    page[1] = 0xB2;               /* page code */
    scsi_store_be16(&page[2], 4); /* page length (n - 3 with n=7) */

    page[4] = 0; /* threshold exponent */

    /* byte 5 bits: LBPU(7) LBPWS(6) LBPWS10(5) LBPRZ(2) ANC_SUP(1) DP(0)
     * Only set LBPU when DSM is supported. */
    // TODO implement LBPWS LBPRZ
    if (unit_supports_dsm(unit))
        page[5] = 0x80; /* LBPU = 1 */

    page[6] = 0x02; /* Provisioning Type: thinly provisioned */
    page[7] = 0;

    copy_len = sizeof(page);
    if (cmd->scsi_Length < copy_len)
        copy_len = cmd->scsi_Length;
    CopyMem(page, cmd->scsi_Data, copy_len);
    scsi_set_actual(cmd, sizeof(page));
    return 0;
}

/*
 * scsi_inquiry - SCSI INQUIRY dispatcher.
 *
 * Looks at the EVPD bit (CDB[1] bit 0) and page code (CDB[2]) to
 * decide whether to return the standard inquiry data or a VPD page.
 * Unknown VPD pages produce INVALID_FIELD_IN_CDB sense per SPC-4.
 */
static BYTE scsi_inquiry(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    UBYTE *command = (UBYTE *)cmd->scsi_Command;
    UBYTE evpd, page_code;

    if (cmd->scsi_CmdLength < 6 || command == NULL)
        return IOERR_BADLENGTH;

    evpd = (UBYTE)(command[1] & 0x01);
    page_code = command[2];

    /* Without EVPD, page_code must be zero per SPC-4 — otherwise it's
     * an illegal request.  We ignore that subtlety and just serve the
     * standard data. */
    if (!evpd)
        return scsi_inquiry_standard(unit, cmd);

    switch (page_code)
    {
    case 0x00:
        return scsi_inquiry_vpd_00(unit, cmd);
    case 0xB0:
        return scsi_inquiry_vpd_b0(unit, cmd);
    case 0xB2:
        return scsi_inquiry_vpd_b2(unit, cmd);
    default:
        Kprintf("[nvme] %s: unsupported VPD page 0x%02lx\n",
                __func__, (ULONG)page_code);
        scsi_make_sense(cmd, 0, 0, IOERR_NOCMD);
        return IOERR_NOCMD;
    }
}

/*
 * scsi_read_capacity_10 - emulate READ CAPACITY (10).
 *
 * NVMe has no CHS geometry, so the PMI (Partial Medium Indicator) flag
 * is ignored.
 */
static BYTE scsi_read_capacity_10(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    struct SCSI_CAPACITY_10 *data = (struct SCSI_CAPACITY_10 *)cmd->scsi_Data;

    if (data == NULL)
    {
        scsi_make_sense(cmd, 0, 0, IOERR_BADADDRESS);
        return IOERR_BADADDRESS;
    }

    data->block_size = unit->blockSize ? unit->blockSize : 512;

    if (unit->logicalSectors < (uint64_t)0xFFFFFFFFUL)
        data->lba = (ULONG)(unit->logicalSectors - 1);
    else
        data->lba = 0xFFFFFFFFUL;

    cmd->scsi_Actual = sizeof(struct SCSI_CAPACITY_10);
    return 0;
}

/*
 * scsi_read_capacity_16 - emulate READ CAPACITY (16).
 */
static BYTE scsi_read_capacity_16(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    struct SCSI_CAPACITY_16 *data = (struct SCSI_CAPACITY_16 *)cmd->scsi_Data;

    if (data == NULL)
    {
        scsi_make_sense(cmd, 0, 0, IOERR_BADADDRESS);
        return IOERR_BADADDRESS;
    }

    data->block_size = unit->blockSize ? unit->blockSize : 512;
    data->lba = unit->logicalSectors ? unit->logicalSectors - 1 : 0;

    cmd->scsi_Actual = sizeof(struct SCSI_CAPACITY_16);
    return 0;
}

/*
 * scsi_write_mode_pages - write mode page data into buf starting at idx.
 *
 * Shared by MODE SENSE 6 and MODE SENSE 10.  Returns updated idx.
 */
static UBYTE scsi_write_mode_pages(UBYTE *data, UBYTE page, UBYTE idx, ULONG blockSize)
{
    if (page == 0x3F || page == 0x03)
    {
        data[idx++] = 0x03; /* page code: Format Device Parameters */
        data[idx++] = 0x16; /* page length */
        for (int i = 0; i < 8; i++)
            data[idx++] = 0;
        data[idx++] = 0; /* sectors per track (unknown for NVMe) */
        data[idx++] = 0;
        data[idx++] = (UBYTE)(blockSize >> 8);
        data[idx++] = (UBYTE)(blockSize);
        for (int i = 0; i < 10; i++)
            data[idx++] = 0;
    }

    if (page == 0x3F || page == 0x04)
    {
        data[idx++] = 0x04; /* page code: Rigid Drive Geometry Parameters */
        data[idx++] = 0x16; /* page length */
        for (int i = 0; i < 24; i++)
            data[idx++] = 0;
    }

    return idx;
}

/*
 * scsi_mode_sense - emulate MODE SENSE (6).
 *
 * NVMe has no CHS geometry; pages 0x03 and 0x04 return zeroed geometry fields.
 */
static BYTE scsi_mode_sense(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    UBYTE *data = (UBYTE *)cmd->scsi_Data;
    UBYTE *command = (UBYTE *)cmd->scsi_Command;

    if (data == NULL)
        return IOERR_BADADDRESS;

    UBYTE page = command[2] & 0x3F;
    UBYTE subpage = command[3];

    if (subpage != 0 || (page != 0x3F && page != 0x03 && page != 0x04))
    {
        scsi_make_sense(cmd, 0, 0, IOERR_NOCMD);
        return IOERR_NOCMD;
    }

    ULONG min_len = (page == 0x3F) ? 52UL : 28UL;
    if ((ULONG)cmd->scsi_Length < min_len)
        return IOERR_BADLENGTH;

    ULONG blockSize = unit->blockSize ? unit->blockSize : 512;

    data[0] = 3; /* mode data length placeholder */
    data[1] = 0; /* medium type: disk */
    data[2] = 0; /* DPOFUA */
    data[3] = 0; /* block descriptor length */

    UBYTE idx = scsi_write_mode_pages(data, page, 4, blockSize);

    data[0] = (UBYTE)(idx - 1);
    cmd->scsi_Actual = idx;
    return 0;
}

/*
 * scsi_mode_sense_10 - emulate MODE SENSE (10).
 *
 * Same page data as MODE SENSE 6 but with an 8-byte response header and a
 * 10-byte CDB (allocation length at bytes 7–8).
 */
static BYTE scsi_mode_sense_10(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    UBYTE *data = (UBYTE *)cmd->scsi_Data;
    UBYTE *command = (UBYTE *)cmd->scsi_Command;

    if (data == NULL)
        return IOERR_BADADDRESS;

    UBYTE page = command[2] & 0x3F;
    UBYTE subpage = command[3];

    if (subpage != 0 || (page != 0x3F && page != 0x03 && page != 0x04))
    {
        scsi_make_sense(cmd, 0, 0, IOERR_NOCMD);
        return IOERR_NOCMD;
    }

    /* 8-byte header + same page data as MODE SENSE 6 */
    ULONG min_len = (page == 0x3F) ? 56UL : 32UL;
    if ((ULONG)cmd->scsi_Length < min_len)
        return IOERR_BADLENGTH;

    ULONG blockSize = unit->blockSize ? unit->blockSize : 512;

    data[0] = 0; /* mode data length high byte (filled in below) */
    data[1] = 0; /* mode data length low byte */
    data[2] = 0; /* medium type: disk */
    data[3] = 0; /* device-specific parameter */
    data[4] = 0; /* LONGLBA */
    data[5] = 0; /* reserved */
    data[6] = 0; /* block descriptor length high byte */
    data[7] = 0; /* block descriptor length low byte */

    UBYTE idx = scsi_write_mode_pages(data, page, 8, blockSize);

    /* mode data length = total bytes - 2 (excludes the length field itself) */
    UWORD len = (UWORD)(idx - 2);
    data[0] = (UBYTE)(len >> 8);
    data[1] = (UBYTE)(len);
    cmd->scsi_Actual = idx;
    return 0;
}

/*
 * scsi_request_sense - emulate REQUEST SENSE.
 *
 * The driver has no persistent sense state, so we always return "no sense"
 * (senseKey 0x00, ASC 0x00, ASQ 0x00) in fixed format.
 */
static BYTE scsi_request_sense(struct NVMeUnit *unit, struct SCSICmd *cmd)
{
    (void)unit;
    struct SCSI_FIXED_SENSE *sense = (struct SCSI_FIXED_SENSE *)cmd->scsi_Data;
    UBYTE *command = (UBYTE *)cmd->scsi_Command;

    if (sense == NULL)
        return IOERR_BADADDRESS;

    UBYTE alloc = command[4];

    sense->response = 0x70; /* fixed format, current */
    sense->pad = 0;
    sense->senseKey = 0x00; /* no sense */
    sense->info = 0;
    sense->additional = (UBYTE)(sizeof(struct SCSI_FIXED_SENSE) - 7);
    sense->specific = 0;
    sense->asc = 0x00;
    sense->asq = 0x00;
    sense->fru = 0;
    sense->sks[0] = sense->sks[1] = sense->sks[2] = 0;

    cmd->scsi_Actual = (alloc < (UBYTE)sizeof(struct SCSI_FIXED_SENSE))
                           ? alloc
                           : (UBYTE)sizeof(struct SCSI_FIXED_SENSE);
    return 0;
}

/*
 * scsi_unmap - SCSI UNMAP (0x42) -> NVMe Dataset Management (Deallocate).
 *
 * Parameter-list layout (SBC-3 §5.32):
 *   bytes 0..1:  data length (n-2)            big-endian u16
 *   bytes 2..3:  block descriptor data length big-endian u16
 *   bytes 4..7:  reserved
 *   bytes 8+:    16-byte UNMAP block descriptors, each:
 *                  bytes 0..7:   LBA            big-endian u64
 *                  bytes 8..11:  num blocks     big-endian u32
 *                  bytes 12..15: reserved
 *
 * We translate each SCSI descriptor into a little-endian
 * nvme_dsm_range, then hand the array to nvme_io_submit_dsm() which
 * builds the SQE.
 *
 * Returns NVME_IO_ASYNC on successful submission (completion path
 * replies the IOStdReq); otherwise an IOERR_* code and we've already
 * filled in scsi_Status / sense.
 */
static BYTE scsi_unmap(struct NVMeUnit *unit, struct IOStdReq *io)
{
    struct SCSICmd *cmd = (struct SCSICmd *)io->io_Data;
    struct NVMeController *ctrl = unit ? unit->ctrl : NULL;

    if (!ctrl)
    {
        scsi_make_sense(cmd, 0, 0, IOERR_OPENFAIL);
        return IOERR_OPENFAIL;
    }

    /* Capability gate.  If the controller doesn't advertise DSM, we
     * have nothing to translate to. */
    if (!unit_supports_dsm(unit))
    {
        KprintfH("[nvme] %s: controller does not support DSM\n", __func__);
        scsi_make_sense(cmd, 0, 0, IOERR_NOCMD);
        return IOERR_NOCMD;
    }

    /* Zero-length UNMAP is a legal no-op (no descriptors to process). */
    UBYTE *param = (UBYTE *)cmd->scsi_Data;
    ULONG param_len = cmd->scsi_Length;
    if (param_len == 0 || param == NULL)
    {
        cmd->scsi_Actual = 0;
        return 0;
    }

    if (param_len < sizeof(struct SCSI_UNMAP_PARAM_HDR))
    {
        Kprintf("[nvme] %s: param list too short (%lu)\n",
                __func__, param_len);
        scsi_make_sense(cmd, 0, 0, IOERR_BADLENGTH);
        return IOERR_BADLENGTH;
    }

    /* Parameter list header is big-endian on the wire, which on m68k
     * matches host order — read directly. */
    UWORD block_desc_len = *(UWORD *)&param[2];
    UWORD nr_ranges = (UWORD)(block_desc_len / 16U);

    KprintfH("[nvme] %s: param_len=%lu block_desc_len=%lu nr_ranges=%lu\n",
             __func__, param_len, (ULONG)block_desc_len, (ULONG)nr_ranges);

    if (nr_ranges == 0)
    {
        /* Header only, nothing to discard. */
        cmd->scsi_Actual = 0;
        return 0;
    }

    if (nr_ranges > NVME_DSM_MAX_RANGES)
    {
        Kprintf("[nvme] %s: %lu ranges exceeds DSM cap %lu\n", __func__, (ULONG)nr_ranges, (ULONG)NVME_DSM_MAX_RANGES);
        /* Sense: INVALID FIELD IN PARAMETER LIST (key=0x05, asc=0x26).
         * The fake_sense helper only sets sense via IOERR_NOCMD path
         * (asc=0x20); the asc=0x26 distinction matters less to our
         * Amiga clients than just refusing the command. */
        scsi_make_sense(cmd, 0, 0, IOERR_NOCMD);
        return IOERR_NOCMD;
    }

    if ((ULONG)(8 + (ULONG)block_desc_len) > param_len)
    {
        Kprintf("[nvme] %s: header lies about descriptor length (need %lu, have %lu)\n",
                __func__, (ULONG)(8 + (ULONG)block_desc_len), param_len);
        scsi_make_sense(cmd, 0, 0, IOERR_BADLENGTH);
        return IOERR_BADLENGTH;
    }

    /* Allocate the DMA buffer up front and fill it in place; the slots
     * past nr_ranges remain zero (dma_zalloc) which the device-quirk
     * note in nvme_setup_dsm requires.  nvme_io_submit_dsm takes
     * ownership unconditionally — we must not touch @ranges after the
     * call, including on the err != ASYNC path. */
    struct nvme_dsm_range *ranges =
        dma_zalloc(ctrl->memoryPool, NVME_CTRL_PAGE_SIZE,
                   sizeof(*ranges) * NVME_DSM_MAX_RANGES);
    if (!ranges)
    {
        scsi_make_sense(cmd, 0, 0, IOERR_SELFTEST);
        return IOERR_SELFTEST;
    }

    UBYTE *desc = param + 8;
    for (UWORD i = 0; i < nr_ranges; i++, desc += 16)
    {
        uint64_t slba = *(uint64_t *)&desc[0];
        ULONG blocks = *(ULONG *)&desc[8];

        if (unit->logicalSectors > 0 &&
            (slba + blocks) > unit->logicalSectors)
        {
            Kprintf("[nvme] %s: range[%lu] LBA 0x%08lx%08lx + %lu blocks exceeds disk\n",
                    __func__, (ULONG)i,
                    (ULONG)(slba >> 32), (ULONG)slba, blocks);
            dma_free(ctrl->memoryPool, ranges);
            scsi_make_sense(cmd, 0, 0, IOERR_BADADDRESS);
            return IOERR_BADADDRESS;
        }

        ranges[i].cattr = le32(0);
        ranges[i].nlb = le32(blocks);
        ranges[i].slba = le64(slba);
    }

    BYTE err = nvme_io_submit_dsm(unit, io, ranges, nr_ranges);
    if (err != NVME_IO_ASYNC)
    {
        scsi_make_sense(cmd, 0, 0, err);
        return err;
    }

    /* Async: reply happens from nvme_process_completions(). */
    cmd->scsi_CmdActual = cmd->scsi_CmdLength;
    cmd->scsi_Status = 0;
    return NVME_IO_ASYNC;
}

/*
 * scsi_sync_cache - SCSI SYNCHRONIZE CACHE (10) -> NVMe Flush.
 *
 * SBC-3 SYNCHRONIZE CACHE has LBA/length parameters but NVMe Flush is
 * always whole-namespace.  We ignore the range and flush the entire
 * volatile write cache for our namespace, which is a conservative
 * superset of what the client requested.
 *
 * Returns NVME_IO_ASYNC on successful submission, otherwise an
 * IOERR_* code with sense already filled in.
 */
static BYTE scsi_sync_cache(struct NVMeUnit *unit, struct IOStdReq *io)
{
    struct SCSICmd *cmd = (struct SCSICmd *)io->io_Data;
    struct NVMeController *ctrl = unit ? unit->ctrl : NULL;

    if (!ctrl)
    {
        scsi_make_sense(cmd, 0, 0, IOERR_OPENFAIL);
        return IOERR_OPENFAIL;
    }

    BYTE err = nvme_io_submit_flush(unit, io);
    if (err != NVME_IO_ASYNC)
    {
        scsi_make_sense(cmd, 0, 0, err);
        return err;
    }

    cmd->scsi_CmdActual = cmd->scsi_CmdLength;
    cmd->scsi_Status = 0;
    return NVME_IO_ASYNC;
}

BYTE handle_scsi_cmd(struct IOStdReq *io)
{
    struct SCSICmd *cmd = (struct SCSICmd *)io->io_Data;
    struct NVMeUnit *unit = (struct NVMeUnit *)io->io_Unit;

    if (!cmd)
        return IOERR_BADADDRESS;

    UBYTE *command = (UBYTE *)cmd->scsi_Command;
    BYTE error = 0;
    cmd->scsi_SenseActual = 0;

    uint64_t lba;
    ULONG count;

    KprintfH("[nvme] %s: SCSI cmd 0x%02lx (unit %ld)\n", __func__, (ULONG)command[0], unit->unitNumber);

    switch (command[0])
    {
    case SCSI_CMD_TEST_UNIT_READY:
        cmd->scsi_Actual = 0;
        break;

    case SCSI_CMD_REQUEST_SENSE:
        error = scsi_request_sense(unit, cmd);
        break;

    case SCSI_CMD_INQUIRY:
        error = scsi_inquiry(unit, cmd);
        break;

    case SCSI_CMD_MODE_SENSE_6:
        error = scsi_mode_sense(unit, cmd);
        break;

    case SCSI_CMD_MODE_SENSE_10:
        error = scsi_mode_sense_10(unit, cmd);
        break;

    case SCSI_CMD_READ_CAPACITY_10:
        error = scsi_read_capacity_10(unit, cmd);
        break;

    case SCSI_CMD_READ_CAPACITY_16:
        error = scsi_read_capacity_16(unit, cmd);
        break;

    case SCSI_CMD_UNMAP:
        error = scsi_unmap(unit, io);
        if (error == NVME_IO_ASYNC)
            return NVME_IO_ASYNC;
        break;

    case SCSI_CMD_SYNC_CACHE_10:
        error = scsi_sync_cache(unit, io);
        if (error == NVME_IO_ASYNC)
            return NVME_IO_ASYNC;
        break;

    case SCSI_CMD_READ_6:
    case SCSI_CMD_WRITE_6:
    {
        struct SCSI_CDB_6 *cdb6 = (struct SCSI_CDB_6 *)command;
        lba = (uint64_t)(((ULONG)(cdb6->lba_high & 0x1F) << 16) |
                         ((ULONG)cdb6->lba_mid << 8) |
                         (ULONG)cdb6->lba_low);
        count = cdb6->length ? cdb6->length : 256;
        goto do_scsi_transfer;
    }

    case SCSI_CMD_READ_10:
    case SCSI_CMD_WRITE_10:
    {
        struct SCSI_CDB_10 *cdb10 = (struct SCSI_CDB_10 *)command;
        lba = cdb10->lba;
        count = cdb10->length;
        goto do_scsi_transfer;
    }

    case SCSI_CMD_READ_16:
    case SCSI_CMD_WRITE_16:
    {
        struct SCSI_CDB_16 *cdb16 = (struct SCSI_CDB_16 *)command;
        lba = cdb16->lba;
        count = cdb16->length;
        /* fall through to do_scsi_transfer */

    do_scsi_transfer:
        if (cmd->scsi_Data == NULL || (lba + count) > unit->logicalSectors)
        {
            error = IOERR_BADADDRESS;
            scsi_make_sense(cmd, (ULONG)lba, count, error);
            break;
        }

        error = nvme_io_submit_rw(unit, io, lba, count,
                                  (cmd->scsi_Flags & SCSIF_READ) ? nvme_cmd_read : nvme_cmd_write,
                                  cmd->scsi_Data);
        if (error != NVME_IO_ASYNC)
        {
            scsi_make_sense(cmd, (ULONG)lba, count, error);
            break;
        }
        /* Async path: nvme_process_completions() will call reply_io().
         * Set SCSI fields now; io_Actual is set by the completion path. */
        cmd->scsi_CmdActual = cmd->scsi_CmdLength;
        cmd->scsi_Status = 0;
        return NVME_IO_ASYNC;
    }

    default:
        error = IOERR_NOCMD;
        scsi_make_sense(cmd, 0, 0, error);
        break;
    }

    cmd->scsi_CmdActual = cmd->scsi_CmdLength;
    cmd->scsi_Status = error ? SCSI_CHECK_CONDITION : 0;
    return error;
}
