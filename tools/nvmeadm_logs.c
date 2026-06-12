// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static void print_effect_flags(ULONG effects)
{
    ULONG cse = (effects & NVME_CMD_EFFECTS_CSE_MASK) >> 16;

    print_kv_hex32((CONST_STRPTR)"    Raw:", effects);
    print_yes_no((CONST_STRPTR)"    Supported:", (effects & NVME_CMD_EFFECTS_CSUPP) != 0);
    print_yes_no((CONST_STRPTR)"    LBCC:", (effects & NVME_CMD_EFFECTS_LBCC) != 0);
    print_yes_no((CONST_STRPTR)"    NCC:", (effects & NVME_CMD_EFFECTS_NCC) != 0);
    print_yes_no((CONST_STRPTR)"    NIC:", (effects & NVME_CMD_EFFECTS_NIC) != 0);
    print_yes_no((CONST_STRPTR)"    CCC:", (effects & NVME_CMD_EFFECTS_CCC) != 0);
    print_kv_u32((CONST_STRPTR)"    CSE Field:", cse);
}

static void print_effect_entry(CONST_STRPTR label, ULONG effects)
{
    Printf((CONST_STRPTR)"%s\n", (ULONG)label);
    print_effect_flags(effects);
}

static void print_error_entry(ULONG index, const struct nvme_error_slot *slot)
{
    unsigned long long error_count = read_le64((const UBYTE *)&slot->error_count);
    UWORD cmdid = read_le16((const UBYTE *)&slot->cmdid);
    UWORD status_field = read_le16((const UBYTE *)&slot->status_field);
    UWORD sc = (UWORD)((ULONG)status_field & NVME_SC_MASK);
    UWORD sct = (UWORD)((ULONG)status_field & NVME_SCT_MASK);
    ULONG nsid = read_le32((const UBYTE *)&slot->nsid);

    char error_count_text[24];
    format_u64_decimal(error_count, error_count_text, sizeof(error_count_text));

    Printf((CONST_STRPTR)"Entry %lu\n", index + 1UL);
    print_kv_str((CONST_STRPTR)"  Error Count:", (CONST_STRPTR)error_count_text);

    /* CMDID FFFFh: the error is not tied to a particular command, so the
     * queue/command/parameter locators carry no information. */
    if (cmdid == 0xFFFFU)
    {
        print_kv_str((CONST_STRPTR)"  Command:", (CONST_STRPTR)"not associated with a specific command");
    }
    else
    {
        print_kv_u32((CONST_STRPTR)"  SQID:", (ULONG)read_le16((const UBYTE *)&slot->sqid));
        print_kv_hex16((CONST_STRPTR)"  Command ID:", (ULONG)cmdid);

        UWORD pel = read_le16((const UBYTE *)&slot->param_error_location);
        if (pel == 0xFFFFU)
        {
            print_kv_str((CONST_STRPTR)"  Param Error Location:", (CONST_STRPTR)"not reported");
        }
        else
        {
            print_kv_hex16((CONST_STRPTR)"  Param Error Location:", (ULONG)pel);
            print_kv_u32((CONST_STRPTR)"    Byte:", (ULONG)(pel & 0x00ffU));
            print_kv_u32((CONST_STRPTR)"    Bit:", (ULONG)((pel >> 8) & 0x7U));
        }
    }

    print_kv_hex16((CONST_STRPTR)"  Status Field:", (ULONG)status_field);
    print_kv_u32((CONST_STRPTR)"    Phase Tag:", (ULONG)(status_field & 0x1U));
    print_kv_hex8((CONST_STRPTR)"    SC:", (ULONG)sc);
    Printf((CONST_STRPTR)"    SCT:                0x%02lx (%s)\n",
           (ULONG)(sct >> 8), (ULONG)nvme_status_type_name(sct));
    print_yes_no((CONST_STRPTR)"    More:", (status_field & NVME_STATUS_MORE) != 0);
    print_yes_no((CONST_STRPTR)"    Do Not Retry:", (status_field & NVME_STATUS_DNR) != 0);

    if (nsid == 0)
    {
        print_kv_str((CONST_STRPTR)"  NSID:", (CONST_STRPTR)"n/a");
    }
    else if (nsid == NVME_NSID_ALL)
    {
        print_kv_str((CONST_STRPTR)"  NSID:", (CONST_STRPTR)"all namespaces");
    }
    else
    {
        print_kv_u32((CONST_STRPTR)"  NSID:", nsid);
        unsigned long long lba = read_le64((const UBYTE *)&slot->lba);
        Printf((CONST_STRPTR)"  LBA:                  0x%08lx%08lx\n",
               (ULONG)(lba >> 32), (ULONG)lba);
    }

    if (slot->vs != 0)
        print_kv_hex8((CONST_STRPTR)"  Vendor Specific:", (ULONG)slot->vs);
    unsigned long long cs = read_le64((const UBYTE *)&slot->cs);
    if (cs != 0)
        Printf((CONST_STRPTR)"  Command Specific:     0x%08lx%08lx\n",
               (ULONG)(cs >> 32), (ULONG)cs);
}

int run_error_log(void)
{
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free_ctrl;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, NVME_ID_CNS_CTRL, ctrl, sizeof(*ctrl)))
        goto out_close;

    /* The controller maintains ELPE + 1 entries; asking for more returns
     * undefined trailing data on some firmware. */
    ULONG entry_count = (ULONG)ctrl->elpe + 1UL;
    if (entry_count > 16UL)
        entry_count = 16UL;

    const ULONG log_size = entry_count * (ULONG)sizeof(struct nvme_error_slot);
    struct nvme_error_slot *log = (struct nvme_error_slot *)nvmeadm_alloc_clear(log_size);
    if (!log)
        goto out_close;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Error log",
                               NVME_NSID_ALL, NVME_LOG_ERROR, 0, 0,
                               log, log_size))
        goto out_free_log;

    Printf((CONST_STRPTR)"Error Information Log\n");

    BOOL printed_any = FALSE;
    ULONG stale_count = 0;
    for (ULONG i = 0; i < entry_count; i++)
    {
        const struct nvme_error_slot *slot = &log[i];

        /* Error Count 0 marks an invalid (never used) entry. */
        if (read_le64((const UBYTE *)&slot->error_count) == 0)
            continue;

        /* A rolling error count with every other field zeroed is a stale
         * rotated slot some controllers keep around; it carries no
         * information worth a full dump. */
        if (bytes_all_zero((const UBYTE *)slot + 8, (ULONG)sizeof(*slot) - 8UL))
        {
            stale_count++;
            continue;
        }

        printed_any = TRUE;
        print_error_entry(i, slot);
    }

    if (stale_count != 0)
        Printf((CONST_STRPTR)"%lu stale/empty entries skipped.\n", stale_count);
    if (!printed_any && stale_count == 0)
        Printf((CONST_STRPTR)"No populated error log entries.\n");

    rc = RETURN_OK;

out_free_log:
    FreeMem(log, log_size);
out_close:
    nvmeadm_close(&session);
out_free_ctrl:
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}

int run_effects_log(void)
{
    struct nvme_effects_log *log = (struct nvme_effects_log *)nvmeadm_read_log(
        (CONST_STRPTR)"Command Effects log", 0, NVME_LOG_CMD_EFFECTS,
        0, NVME_CSI_NVM, sizeof(*log));
    if (!log)
        return RETURN_FAIL;

    Printf((CONST_STRPTR)"Command Effects Log\n");
    Printf((CONST_STRPTR)"Admin Commands\n");
    print_effect_entry((CONST_STRPTR)"  Get Log Page", read_le32((const UBYTE *)&log->acs[nvme_admin_get_log_page]));
    print_effect_entry((CONST_STRPTR)"  Identify", read_le32((const UBYTE *)&log->acs[nvme_admin_identify]));
    print_effect_entry((CONST_STRPTR)"  Activate Firmware", read_le32((const UBYTE *)&log->acs[nvme_admin_activate_fw]));
    print_effect_entry((CONST_STRPTR)"  Format NVM", read_le32((const UBYTE *)&log->acs[nvme_admin_format_nvm]));
    print_effect_entry((CONST_STRPTR)"  Sanitize NVM", read_le32((const UBYTE *)&log->acs[nvme_admin_sanitize_nvm]));

    Printf((CONST_STRPTR)"I/O Commands\n");
    print_effect_entry((CONST_STRPTR)"  Write", read_le32((const UBYTE *)&log->iocs[nvme_cmd_write]));
    print_effect_entry((CONST_STRPTR)"  Write Uncorrectable", read_le32((const UBYTE *)&log->iocs[nvme_cmd_write_uncor]));
    print_effect_entry((CONST_STRPTR)"  Write Zeroes", read_le32((const UBYTE *)&log->iocs[nvme_cmd_write_zeroes]));

    FreeMem(log, sizeof(*log));
    return RETURN_OK;
}

int run_changed_ns(void)
{
    const ULONG entry_count = NVME_MAX_CHANGED_NAMESPACES;
    const ULONG log_size = entry_count * (ULONG)sizeof(ULONG);
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    UBYTE *log = (UBYTE *)nvmeadm_alloc_clear(log_size);
    if (!log)
    {
        FreeMem(ctrl, sizeof(*ctrl));
        return RETURN_FAIL;
    }

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, NVME_ID_CNS_CTRL, ctrl, sizeof(*ctrl)))
        goto out_close;

    Printf((CONST_STRPTR)"Changed Namespace List\n");

    /* The log page exists only on controllers that support Namespace
     * Attribute Notices; others reject it with Invalid Log Page. */
    if ((read_le32((const UBYTE *)&ctrl->oaes) & NVME_AEN_CFG_NS_ATTR) == 0)
    {
        Printf((CONST_STRPTR)"Not supported by this controller (no Namespace Attribute Notices)\n");
        rc = RETURN_OK;
        goto out_close;
    }

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Changed Namespace log",
                               NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0,
                               NVME_CSI_NVM, log, log_size))
        goto out_close;

    BOOL printed_any = FALSE;
    for (ULONG i = 0; i < entry_count; i++)
    {
        ULONG nsid = read_le32(log + (i * 4UL));

        if (nsid == 0)
            continue;

        printed_any = TRUE;
        if (nsid == NVME_NSID_ALL)
        {
            Printf((CONST_STRPTR)"Entry %lu:               all namespaces changed\n", i + 1UL);
        }
        else
        {
            Printf((CONST_STRPTR)"Entry %lu:               nsid=%lu\n", i + 1UL, nsid);
        }
    }

    if (!printed_any)
        Printf((CONST_STRPTR)"No changed namespaces reported.\n");

    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, log_size);
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}
