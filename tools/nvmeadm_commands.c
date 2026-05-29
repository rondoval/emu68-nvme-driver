// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static void print_fw_revision(CONST_STRPTR label, const UBYTE *rev)
{
    char text[9];

    CopyMem(rev, text, 8);
    text[8] = '\0';
    Printf((CONST_STRPTR)"%-24s %.8s\n", (ULONG)label, (ULONG)text);
}

static void print_critical_warning(UBYTE warning)
{
    Printf((CONST_STRPTR)"Critical Warning:       0x%02lx\n", (ULONG)warning);
    if (warning == 0)
    {
        Printf((CONST_STRPTR)"  none\n");
        return;
    }

    if ((warning & (1U << 0)) != 0)
        Printf((CONST_STRPTR)"  available spare below threshold\n");
    if ((warning & (1U << 1)) != 0)
        Printf((CONST_STRPTR)"  temperature threshold exceeded\n");
    if ((warning & (1U << 2)) != 0)
        Printf((CONST_STRPTR)"  device reliability degraded\n");
    if ((warning & (1U << 3)) != 0)
        Printf((CONST_STRPTR)"  media is read-only\n");
    if ((warning & (1U << 4)) != 0)
        Printf((CONST_STRPTR)"  volatile memory backup failed\n");
    if ((warning & (1U << 5)) != 0)
        Printf((CONST_STRPTR)"  persistent memory region degraded\n");
}

static void print_endurance_group_warning(UBYTE warning)
{
    Printf((CONST_STRPTR)"Endurance Warnings:    0x%02lx\n", (ULONG)warning);
    if (warning == 0)
    {
        Printf((CONST_STRPTR)"  none\n");
        return;
    }

    if ((warning & (1U << 0)) != 0)
        Printf((CONST_STRPTR)"  endurance-group spare below threshold\n");
    if ((warning & (1U << 2)) != 0)
        Printf((CONST_STRPTR)"  endurance-group reliability degraded\n");
    if ((warning & (1U << 3)) != 0)
        Printf((CONST_STRPTR)"  endurance-group media is read-only\n");
}

static void print_temperature_sensor_readings(const __le16 *sensors, ULONG count)
{
    ULONG i;
    BOOL printed_any = FALSE;

    for (i = 0; i < count; i++)
    {
        UWORD temp_k = read_le16((const UBYTE *)&sensors[i]);
        LONG temp_c;

        if (temp_k == 0)
            continue;

        if (!printed_any)
        {
            Printf((CONST_STRPTR)"Temperature Sensors:\n");
            printed_any = TRUE;
        }

        temp_c = (LONG)temp_k - 273L;
        if (temp_c < 0)
        {
            Printf((CONST_STRPTR)"  Sensor %lu:            %lu K (-%lu C)\n",
                   i + 1UL, (ULONG)temp_k, (ULONG)(0L - temp_c));
        }
        else
        {
            Printf((CONST_STRPTR)"  Sensor %lu:            %lu K (%lu C)\n",
                   i + 1UL, (ULONG)temp_k, (ULONG)temp_c);
        }
    }
}

static void print_smart_u32_value(CONST_STRPTR label, ULONG value,
                                  CONST_STRPTR units,
                                  CONST_STRPTR zero_text)
{
    if (value == 0 && zero_text)
    {
        Printf((CONST_STRPTR)"%-24s %s\n",
               (ULONG)label, (ULONG)zero_text);
        return;
    }

    if (units)
    {
        Printf((CONST_STRPTR)"%-24s %lu %s\n",
               (ULONG)label, value, (ULONG)units);
    }
    else
    {
        Printf((CONST_STRPTR)"%-24s %lu\n",
               (ULONG)label, value);
    }
}

static void print_yes_no(CONST_STRPTR label, BOOL enabled)
{
    Printf((CONST_STRPTR)"%-24s %s\n",
           (ULONG)label,
           (ULONG)(enabled ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no"));
}

static BOOL mdts_bytes_for_mpsmin(UBYTE mdts, UBYTE mpsmin,
                                  unsigned long long *bytes_out)
{
    ULONG shift;

    if (!bytes_out)
        return FALSE;

    shift = (ULONG)mdts + 12UL + (ULONG)mpsmin;
    if (shift >= 64UL)
        return FALSE;

    *bytes_out = 1ULL << shift;
    return TRUE;
}

static void print_binary_size(CONST_STRPTR label, unsigned long long bytes)
{
    static const CONST_STRPTR units[] = {
        (CONST_STRPTR)"B",
        (CONST_STRPTR)"KiB",
        (CONST_STRPTR)"MiB",
        (CONST_STRPTR)"GiB",
        (CONST_STRPTR)"TiB",
    };
    unsigned long long scaled = bytes;
    unsigned long long unit_bytes = 1ULL;
    ULONG unit_index = 0;
    char whole_text[40];

    while (scaled >= 1024ULL && unit_index < 4)
    {
        scaled /= 1024ULL;
        unit_bytes <<= 10;
        unit_index++;
    }

    if (unit_index == 0 || (bytes % unit_bytes) == 0)
    {
        format_u64_decimal(bytes / unit_bytes, whole_text, sizeof(whole_text));
        Printf((CONST_STRPTR)"%-24s %s %s\n",
               (ULONG)label, (ULONG)whole_text, (ULONG)units[unit_index]);
    }
    else
    {
        ULONG tenths = (ULONG)(((bytes % unit_bytes) * 10ULL) / unit_bytes);

        format_u64_decimal(bytes / unit_bytes, whole_text, sizeof(whole_text));
        Printf((CONST_STRPTR)"%-24s %s.%lu %s\n",
               (ULONG)label, (ULONG)whole_text, tenths, (ULONG)units[unit_index]);
    }
}

static BOOL nvmeadm_id_cns_ok(ULONG version, UBYTE cns)
{
    if (version >= NVME_VS(1, 2, 0))
        return TRUE;

    if (version >= NVME_VS(1, 1, 0))
        return cns <= 3U;

    return cns <= 1U;
}

static CONST_STRPTR controller_type_name(UBYTE type)
{
    switch (type)
    {
        case 0: return (CONST_STRPTR)"not reported";
        case 1: return (CONST_STRPTR)"I/O controller";
        case 2: return (CONST_STRPTR)"discovery controller";
        case 3: return (CONST_STRPTR)"administrative controller";
        default: return (CONST_STRPTR)"unknown";
    }
}

static CONST_STRPTR discovery_controller_type_name(UBYTE type)
{
    switch (type)
    {
        case 0: return (CONST_STRPTR)"not reported";
        case 1: return (CONST_STRPTR)"direct discovery controller";
        case 2: return (CONST_STRPTR)"central discovery controller";
        default: return (CONST_STRPTR)"unknown";
    }
}

static void print_queue_entry_size(CONST_STRPTR label, UBYTE encoded)
{
    UBYTE required_log2 = (UBYTE)(encoded & 0x0fU);
    UBYTE maximum_log2 = (UBYTE)((encoded >> 4) & 0x0fU);

    if (required_log2 == 0 && maximum_log2 == 0)
    {
        Printf((CONST_STRPTR)"%-24s not reported\n", (ULONG)label);
        return;
    }

    Printf((CONST_STRPTR)"%-24s req=%lu B max=%lu B\n",
           (ULONG)label,
           (ULONG)(1UL << required_log2),
           (ULONG)(1UL << maximum_log2));
}

static void print_mdts(UBYTE mdts)
{
    unsigned long long assumed_bytes;

    if (mdts == 0)
    {
        Printf((CONST_STRPTR)"MDTS:                   not reported\n");
        return;
    }

    Printf((CONST_STRPTR)"MDTS:                   raw=%lu  => 2^%lu * min page size\n",
           (ULONG)mdts, (ULONG)mdts);
    if (mdts_bytes_for_mpsmin(mdts, 0, &assumed_bytes))
        print_binary_size((CONST_STRPTR)"MDTS @ 4 KiB pages:", assumed_bytes);
    Printf((CONST_STRPTR)"MDTS Bytes:             exact byte size still needs CAP.MPSMIN\n");
}

static void print_hmb_capabilities(const struct nvme_id_ctrl *ctrl)
{
    ULONG preferred_size = read_le32((const UBYTE *)&ctrl->hmpre);
    ULONG minimum_size = read_le32((const UBYTE *)&ctrl->hmmin);
    ULONG minimum_desc_size = read_le32((const UBYTE *)&ctrl->hmminds);

    if (preferred_size == 0 && minimum_size == 0 && minimum_desc_size == 0)
    {
        Printf((CONST_STRPTR)"Host Memory Buffer:     not reported or not supported\n");
        return;
    }

    Printf((CONST_STRPTR)"Host Memory Buffer:     reported\n");
    Printf((CONST_STRPTR)"  Preferred Size:       0x%08lx\n", preferred_size);
    Printf((CONST_STRPTR)"  Minimum Size:         0x%08lx\n", minimum_size);
    Printf((CONST_STRPTR)"  Min Desc Entry Size:  0x%08lx\n", minimum_desc_size);
}

static void print_command_set_probe(CONST_STRPTR label, BOOL supported,
                                    BOOL probed, BYTE status, ULONG result)
{
    if (!probed)
    {
        Printf((CONST_STRPTR)"%-24s not probed on this controller version\n",
               (ULONG)label);
        return;
    }

    if (supported)
    {
        Printf((CONST_STRPTR)"%-24s yes\n", (ULONG)label);
        return;
    }

    Printf((CONST_STRPTR)"%-24s no (status=0x%02lx result=0x%08lx)\n",
           (ULONG)label, (ULONG)(UBYTE)status, result);
}

static void print_percentage_used(UBYTE value)
{
    if (value == 255U)
    {
        Printf((CONST_STRPTR)"Percentage Used:        >=255 %%\n");
        return;
    }

    Printf((CONST_STRPTR)"Percentage Used:        %lu %%\n",
           (ULONG)value);
}

static void print_smart_counter(CONST_STRPTR label, const UBYTE *value,
                                CONST_STRPTR units)
{
    char decimal[40];

    format_le128_decimal(value, decimal, sizeof(decimal));
    if (units)
    {
        Printf((CONST_STRPTR)"%-24s %s %s\n",
               (ULONG)label, (ULONG)decimal, (ULONG)units);
    }
    else
    {
        Printf((CONST_STRPTR)"%-24s %s\n",
               (ULONG)label, (ULONG)decimal);
    }
}

static CONST_STRPTR self_test_op_name(UBYTE op)
{
    switch (op)
    {
        case 0x00: return (CONST_STRPTR)"no self-test in progress";
        case 0x01: return (CONST_STRPTR)"short self-test in progress";
        case 0x02: return (CONST_STRPTR)"extended self-test in progress";
        case 0x0e: return (CONST_STRPTR)"vendor specific self-test in progress";
        case 0x0f: return (CONST_STRPTR)"reserved";
        default:   return (CONST_STRPTR)"unknown";
    }
}

static CONST_STRPTR self_test_code_name(UBYTE code)
{
    switch (code)
    {
        case 0x00: return (CONST_STRPTR)"reserved";
        case 0x01: return (CONST_STRPTR)"short self-test";
        case 0x02: return (CONST_STRPTR)"extended self-test";
        case 0x03: return (CONST_STRPTR)"host-initiated refresh";
        case 0x0e: return (CONST_STRPTR)"vendor specific";
        case 0x0f: return (CONST_STRPTR)"abort self-test";
        default:   return (CONST_STRPTR)"unknown";
    }
}

static CONST_STRPTR self_test_result_name(UBYTE code)
{
    switch (code)
    {
        case 0x00: return (CONST_STRPTR)"completed without error";
        case 0x01: return (CONST_STRPTR)"aborted by device self-test command";
        case 0x02: return (CONST_STRPTR)"aborted by controller reset";
        case 0x03: return (CONST_STRPTR)"aborted due to namespace removal";
        case 0x04: return (CONST_STRPTR)"aborted due to format nvm command";
        case 0x05: return (CONST_STRPTR)"fatal error or unknown test error";
        case 0x06: return (CONST_STRPTR)"completed with unknown failed segment";
        case 0x07: return (CONST_STRPTR)"completed with known failed segment";
        case 0x08: return (CONST_STRPTR)"aborted for unknown reason";
        case 0x09: return (CONST_STRPTR)"aborted due to sanitize operation";
        case 0x0f: return (CONST_STRPTR)"entry not used";
        default:   return (CONST_STRPTR)"unknown";
    }
}

static void print_feature_value(CONST_STRPTR label, UBYTE fid, ULONG value)
{
    switch (fid)
    {
        case NVME_FEAT_POWER_MGMT:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  ps=%lu\n",
                   (ULONG)label, value, value & 0x1fUL);
            break;
        case NVME_FEAT_TEMP_THRESH:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  threshold=%lu K\n",
                   (ULONG)label, value, value & 0xffffUL);
            break;
        case NVME_FEAT_ERR_RECOVERY:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  tler=%lu\n",
                   (ULONG)label, value, value & 0xffffUL);
            break;
        case NVME_FEAT_VOLATILE_WC:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  %s\n",
                   (ULONG)label, value,
                   (ULONG)(((value & 0x1UL) != 0) ? "enabled" : "disabled"));
            break;
        case NVME_FEAT_NUM_QUEUES:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  sq=%lu cq=%lu\n",
                   (ULONG)label, value,
                   (value & 0xffffUL) + 1UL,
                   ((value >> 16) & 0xffffUL) + 1UL);
            break;
        case NVME_FEAT_IRQ_COALESCE:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  thr=%lu time=%lu\n",
                   (ULONG)label, value,
                   value & 0xffUL,
                   (value >> 8) & 0xffUL);
            break;
        case NVME_FEAT_ASYNC_EVENT:
            Printf((CONST_STRPTR)"%-24s 0x%08lx\n",
                   (ULONG)label, value);
            break;
        case NVME_FEAT_NOPSC:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  %s\n",
                   (ULONG)label, value,
                   (ULONG)(((value & 0x1UL) != 0) ? "enabled" : "disabled"));
            break;
        case NVME_FEAT_KATO:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  %lu x 100ms\n",
                   (ULONG)label, value, value & 0xffffUL);
            break;
        default:
            Printf((CONST_STRPTR)"%-24s 0x%08lx\n",
                   (ULONG)label, value);
            break;
    }
}

int run_smart(void)
{
    struct nvmeadm_session session;
    struct nvme_smart_log *log;
    UWORD temp_k;
    LONG temp_c;
    int rc = 20;

    log = (struct nvme_smart_log *)nvmeadm_alloc_clear(sizeof(*log));
    if (!log)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"SMART log",
                               NVME_NSID_ALL, NVME_LOG_SMART, 0, 0,
                               log, sizeof(*log)))
        goto out_close;

    Printf((CONST_STRPTR)"SMART / Health Information\n");
    print_critical_warning(log->critical_warning);
    temp_k = read_le16(log->temperature);
    if (temp_k == 0)
    {
        Printf((CONST_STRPTR)"Temperature:            not reported\n");
    }
    else if ((temp_c = (LONG)temp_k - 273L) < 0)
    {
        Printf((CONST_STRPTR)"Temperature:            %lu K (-%lu C)\n",
               (ULONG)temp_k, (ULONG)(0L - temp_c));
    }
    else
    {
        Printf((CONST_STRPTR)"Temperature:            %lu K (%lu C)\n",
               (ULONG)temp_k, (ULONG)temp_c);
    }
    Printf((CONST_STRPTR)"Available Spare:        %lu %%\n",
           (ULONG)log->avail_spare);
    Printf((CONST_STRPTR)"Spare Threshold:        %lu %%\n",
           (ULONG)log->spare_thresh);
    print_percentage_used(log->percent_used);
    print_endurance_group_warning(log->endu_grp_crit_warn_sumry);
    print_smart_u32_value((CONST_STRPTR)"Warning Temp Time:",
                          read_le32((const UBYTE *)&log->warning_temp_time),
                          (CONST_STRPTR)"minutes",
                          (CONST_STRPTR)"0 (none or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Critical Comp Time:",
                          read_le32((const UBYTE *)&log->critical_comp_time),
                          (CONST_STRPTR)"minutes",
                          (CONST_STRPTR)"0 (none or unsupported)");
    print_smart_data_amount((CONST_STRPTR)"Data Units Read:",
                            log->data_units_read);
    print_smart_data_amount((CONST_STRPTR)"Data Units Written:",
                            log->data_units_written);
    print_smart_counter((CONST_STRPTR)"Host Read Commands:",
                        log->host_reads, NULL);
    print_smart_counter((CONST_STRPTR)"Host Write Commands:",
                        log->host_writes, NULL);
    print_smart_counter((CONST_STRPTR)"Controller Busy Time:",
                        log->ctrl_busy_time,
                        (CONST_STRPTR)"minutes");
    print_smart_counter((CONST_STRPTR)"Power Cycles:",
                        log->power_cycles, NULL);
    print_smart_counter((CONST_STRPTR)"Power On Hours:",
                        log->power_on_hours,
                        (CONST_STRPTR)"hours");
    print_smart_counter((CONST_STRPTR)"Unsafe Shutdowns:",
                        log->unsafe_shutdowns, NULL);
    print_smart_counter((CONST_STRPTR)"Media/Data Errors:",
                        log->media_errors, NULL);
    print_smart_counter((CONST_STRPTR)"Error Log Entries:",
                        log->num_err_log_entries, NULL);
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp1 Count:",
                          read_le32((const UBYTE *)&log->thm_temp1_trans_count),
                          NULL,
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp2 Count:",
                          read_le32((const UBYTE *)&log->thm_temp2_trans_count),
                          NULL,
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp1 Time:",
                          read_le32((const UBYTE *)&log->thm_temp1_total_time),
                          (CONST_STRPTR)"seconds",
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_smart_u32_value((CONST_STRPTR)"Thermal Temp2 Time:",
                          read_le32((const UBYTE *)&log->thm_temp2_total_time),
                          (CONST_STRPTR)"seconds",
                          (CONST_STRPTR)"0 (never or unsupported)");
    print_temperature_sensor_readings(log->temp_sensor, 8UL);

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, sizeof(*log));
    return rc;
}

int run_identify(void)
{
    struct nvmeadm_session session;
    UBYTE *buf;
    UWORD vid;
    UWORD ssvid;
    int rc = 20;

    buf = (UBYTE *)nvmeadm_alloc_clear(4096);
    if (!buf)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, 0x01UL, buf, 4096))
        goto out_close;

    vid = read_le16(buf + 0);
    ssvid = read_le16(buf + 2);

    Printf((CONST_STRPTR)"Identify Controller\n");
    Printf((CONST_STRPTR)"VID:                    %04lx\n", (ULONG)vid);
    Printf((CONST_STRPTR)"SSVID:                  %04lx\n", (ULONG)ssvid);
    Printf((CONST_STRPTR)"Serial Number:          %.20s\n", (ULONG)(buf + 4));
    Printf((CONST_STRPTR)"Model Number:           %.40s\n", (ULONG)(buf + 24));
    Printf((CONST_STRPTR)"Firmware Revision:      %.8s\n", (ULONG)(buf + 64));

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(buf, 4096);
    return rc;
}

int run_identify_caps(void)
{
    struct nvmeadm_session session;
    struct nvme_id_ctrl *ctrl;
    struct nvme_id_ctrl_nvm *nvm_ctrl;
    ULONG version;
    ULONG ctratt;
    ULONG oaes;
    UWORD cntlid;
    UWORD maxcmd;
    UWORD oacs;
    UWORD oncs;
    UWORD hctma;
    ULONG ioccsz;
    BYTE nvm_status = 0;
    BYTE zns_status = 0;
    ULONG nvm_result = 0;
    ULONG zns_result = 0;
    BOOL can_probe_cs;
    BOOL nvm_supported = FALSE;
    BOOL zns_supported = FALSE;
    int rc = 20;

    ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return 20;

    nvm_ctrl = (struct nvme_id_ctrl_nvm *)nvmeadm_alloc_clear(sizeof(*nvm_ctrl));
    if (!nvm_ctrl)
    {
        FreeMem(ctrl, sizeof(*ctrl));
        return 20;
    }

    if (!nvmeadm_open(&session))
        goto out_free_all;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, 0x01UL, ctrl, sizeof(*ctrl)))
        goto out_close;

    version = read_le32((const UBYTE *)&ctrl->ver);
    ctratt = read_le32((const UBYTE *)&ctrl->ctratt);
    oaes = read_le32((const UBYTE *)&ctrl->oaes);
    cntlid = read_le16((const UBYTE *)&ctrl->cntlid);
    maxcmd = read_le16((const UBYTE *)&ctrl->maxcmd);
    oacs = read_le16((const UBYTE *)&ctrl->oacs);
    oncs = read_le16((const UBYTE *)&ctrl->oncs);
    hctma = read_le16((const UBYTE *)&ctrl->hctma);
    ioccsz = read_le32((const UBYTE *)&ctrl->ioccsz);
    can_probe_cs = nvmeadm_id_cns_ok(version, NVME_ID_CNS_CS_CTRL);

    if (can_probe_cs)
    {
        nvm_supported = nvmeadm_fetch_identify_csi(&session, 0,
                                                   NVME_ID_CNS_CS_CTRL,
                                                   NVME_CSI_NVM,
                                                   nvm_ctrl, sizeof(*nvm_ctrl),
                                                   &nvm_status, &nvm_result);

        mem_zero(nvm_ctrl, sizeof(*nvm_ctrl));
        zns_supported = nvmeadm_fetch_identify_csi(&session, 0,
                                                   NVME_ID_CNS_CS_CTRL,
                                                   NVME_CSI_ZNS,
                                                   nvm_ctrl, sizeof(*nvm_ctrl),
                                                   &zns_status, &zns_result);
    }

    Printf((CONST_STRPTR)"Identify Controller Capabilities\n");
    Printf((CONST_STRPTR)"Controller Type:        0x%02lx (%s)\n",
           (ULONG)ctrl->cntrltype,
           (ULONG)controller_type_name(ctrl->cntrltype));
    Printf((CONST_STRPTR)"Discovery Type:         0x%02lx (%s)\n",
           (ULONG)ctrl->dctype,
           (ULONG)discovery_controller_type_name(ctrl->dctype));
    Printf((CONST_STRPTR)"Controller ID:          %lu\n",
           (ULONG)cntlid);
    Printf((CONST_STRPTR)"Async Req Limit:        %lu outstanding requests\n",
           (ULONG)ctrl->aerl + 1UL);
    Printf((CONST_STRPTR)"OAES:                   0x%08lx\n",
           oaes);

    Printf((CONST_STRPTR)"\nController Multipath / Sharing\n");
    print_yes_no((CONST_STRPTR)"  Multi-port:",
                 (ctrl->cmic & NVME_CTRL_CMIC_MULTI_PORT) != 0);
    print_yes_no((CONST_STRPTR)"  Multi-controller:",
                 (ctrl->cmic & NVME_CTRL_CMIC_MULTI_CTRL) != 0);
    print_yes_no((CONST_STRPTR)"  ANA reporting:",
                 (ctrl->cmic & NVME_CTRL_CMIC_ANA) != 0);

    Printf((CONST_STRPTR)"\nTransfer / Queue Limits\n");
    print_mdts(ctrl->mdts);
    print_queue_entry_size((CONST_STRPTR)"SQ Entry Size:", ctrl->sqes);
    print_queue_entry_size((CONST_STRPTR)"CQ Entry Size:", ctrl->cqes);
    Printf((CONST_STRPTR)"Max Outstanding Cmds:   %lu\n",
           (ULONG)maxcmd);
    Printf((CONST_STRPTR)"IO Command Capsule:     %lu bytes\n",
           ioccsz * 16UL);

    Printf((CONST_STRPTR)"\nAdmin Capabilities\n");
    Printf((CONST_STRPTR)"OACS:                   0x%04lx\n",
           (ULONG)oacs);
    print_yes_no((CONST_STRPTR)"  Security Cmds:",
                 (oacs & NVME_CTRL_OACS_SEC_SUPP) != 0);
    print_yes_no((CONST_STRPTR)"  Namespace Mgmt:",
                 (oacs & NVME_CTRL_OACS_NS_MNGT_SUPP) != 0);
    print_yes_no((CONST_STRPTR)"  Namespace Creation:",
                 (oacs & NVME_CTRL_OACS_NS_MNGT_SUPP) != 0);
    print_yes_no((CONST_STRPTR)"  Directives:",
                 (oacs & NVME_CTRL_OACS_DIRECTIVES) != 0);
    print_yes_no((CONST_STRPTR)"  Doorbell Buffer:",
                 (oacs & NVME_CTRL_OACS_DBBUF_SUPP) != 0);

    Printf((CONST_STRPTR)"\nNVM Command Capabilities\n");
    Printf((CONST_STRPTR)"ONCS:                   0x%04lx\n",
           (ULONG)oncs);
    print_yes_no((CONST_STRPTR)"  Compare:",
                 (oncs & NVME_CTRL_ONCS_COMPARE) != 0);
    print_yes_no((CONST_STRPTR)"  Write Uncorrectable:",
                 (oncs & NVME_CTRL_ONCS_WRITE_UNCORRECTABLE) != 0);
    print_yes_no((CONST_STRPTR)"  Dataset Mgmt:",
                 (oncs & NVME_CTRL_ONCS_DSM) != 0);
    print_yes_no((CONST_STRPTR)"  Write Zeroes:",
                 (oncs & NVME_CTRL_ONCS_WRITE_ZEROES) != 0);
    print_yes_no((CONST_STRPTR)"  Reservations:",
                 (oncs & NVME_CTRL_ONCS_RESERVATIONS) != 0);
    print_yes_no((CONST_STRPTR)"  Timestamp:",
                 (oncs & NVME_CTRL_ONCS_TIMESTAMP) != 0);

    Printf((CONST_STRPTR)"\nLog / Event Support\n");
    Printf((CONST_STRPTR)"LPA:                    0x%02lx\n", (ULONG)ctrl->lpa);
    print_yes_no((CONST_STRPTR)"  Cmd Effects Log:",
                 (ctrl->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) != 0);
    Printf((CONST_STRPTR)"AER Support Mask:       0x%08lx\n",
           oaes);

    Printf((CONST_STRPTR)"\nController Attributes\n");
    Printf((CONST_STRPTR)"CTRATT:                 0x%08lx\n",
           ctratt);
    print_yes_no((CONST_STRPTR)"  128-bit Host ID:",
                 (ctratt & NVME_CTRL_CTRATT_128_ID) != 0);
    print_yes_no((CONST_STRPTR)"  Non-op PSP:",
                 (ctratt & NVME_CTRL_CTRATT_NON_OP_PSP) != 0);
    print_yes_no((CONST_STRPTR)"  NVM Sets:",
                 (ctratt & NVME_CTRL_CTRATT_NVM_SETS) != 0);
    print_yes_no((CONST_STRPTR)"  Read Recovery Lvls:",
                 (ctratt & NVME_CTRL_CTRATT_READ_RECV_LVLS) != 0);
    print_yes_no((CONST_STRPTR)"  Endurance Groups:",
                 (ctratt & NVME_CTRL_CTRATT_ENDURANCE_GROUPS) != 0);
    print_yes_no((CONST_STRPTR)"  Predictable Latency:",
                 (ctratt & NVME_CTRL_CTRATT_PREDICTABLE_LAT) != 0);
    print_yes_no((CONST_STRPTR)"  Namespace Granularity:",
                 (ctratt & NVME_CTRL_CTRATT_NAMESPACE_GRANULARITY) != 0);
    print_yes_no((CONST_STRPTR)"  UUID List:",
                 (ctratt & NVME_CTRL_CTRATT_UUID_LIST) != 0);

    Printf((CONST_STRPTR)"\nMemory / Thermal / Self-test\n");
    print_hmb_capabilities(ctrl);
    Printf((CONST_STRPTR)"HCTMA:                  0x%04lx\n",
           (ULONG)hctma);
    Printf((CONST_STRPTR)"DSTO:                   0x%02lx\n", (ULONG)ctrl->dsto);

    Printf((CONST_STRPTR)"\nSupported Command Sets\n");
    print_yes_no((CONST_STRPTR)"  NVM Sets Attr:",
                 (ctratt & NVME_CTRL_CTRATT_NVM_SETS) != 0);
    print_command_set_probe((CONST_STRPTR)"  NVM CS Identify:",
                            nvm_supported, can_probe_cs, nvm_status, nvm_result);
    print_command_set_probe((CONST_STRPTR)"  ZNS CS Identify:",
                            zns_supported, can_probe_cs, zns_status, zns_result);
    if (!can_probe_cs)
    {
        Printf((CONST_STRPTR)"  CSI note:             controller version predates safe extended CNS probing\n");
    }
    else if (!nvm_supported && !zns_supported)
    {
        Printf((CONST_STRPTR)"  CSI combinations:     no command-set-specific Identify data advertised\n");
    }
    else if (nvm_supported && !zns_supported)
    {
        Printf((CONST_STRPTR)"  CSI combinations:     NVM-specific Identify data available\n");
    }
    else
    {
        Printf((CONST_STRPTR)"  CSI combinations:     multiple CSI-specific Identify paths responded\n");
    }

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free_all:
    FreeMem(nvm_ctrl, sizeof(*nvm_ctrl));
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}

int run_fw_log(void)
{
    struct nvmeadm_session session;
    struct nvme_fw_slot_info_log *log;
    UBYTE active_slot;
    UBYTE next_slot;
    ULONG slot;
    int rc = 20;

    log = (struct nvme_fw_slot_info_log *)nvmeadm_alloc_clear(sizeof(*log));
    if (!log)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Firmware Slot log",
                               NVME_NSID_ALL, NVME_LOG_FW_SLOT, 0, NVME_CSI_NVM,
                               log, sizeof(*log)))
        goto out_close;

    active_slot = (UBYTE)(log->afi & 0x7U);
    next_slot = (UBYTE)((log->afi & 0x70U) >> 4);

    Printf((CONST_STRPTR)"Firmware Slot Information\n");
    Printf((CONST_STRPTR)"Active Slot:            %lu\n", (ULONG)active_slot);
    if (next_slot != 0)
        Printf((CONST_STRPTR)"Next Active Slot:       %lu\n", (ULONG)next_slot);
    else
        Printf((CONST_STRPTR)"Next Active Slot:       none\n");

    for (slot = 0; slot < 7; slot++)
    {
        const UBYTE *rev = (const UBYTE *)&log->frs[slot];
        char label[24];

        if (bytes_all_zero(rev, 8))
            continue;

        _SNPrintf((STRPTR)label, sizeof(label), (CONST_STRPTR)"Slot %lu:", slot + 1UL);
        print_fw_revision((CONST_STRPTR)label, rev);
    }

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, sizeof(*log));
    return rc;
}

int run_error_log(void)
{
    struct nvmeadm_session session;
    struct nvme_error_slot *log;
    const ULONG entry_count = 16;
    const ULONG log_size = entry_count * (ULONG)sizeof(*log);
    ULONG i;
    BOOL printed_any = FALSE;
    int rc = 20;

    log = (struct nvme_error_slot *)nvmeadm_alloc_clear(log_size);
    if (!log)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Error log",
                               NVME_NSID_ALL, NVME_LOG_ERROR, 0, 0,
                               log, log_size))
        goto out_close;

    Printf((CONST_STRPTR)"Error Information Log\n");

    for (i = 0; i < entry_count; i++)
    {
        const struct nvme_error_slot *slot = &log[i];
        unsigned long long error_count = read_le64((const UBYTE *)&slot->error_count);
        unsigned long long lba = read_le64((const UBYTE *)&slot->lba);
        unsigned long long cs = read_le64((const UBYTE *)&slot->cs);
        char error_count_text[24];
        UWORD sqid;
        UWORD cmdid;
        UWORD status_field;
        UWORD pel;
        UWORD phase_tag;
        UWORD sc;
        UWORD sct;
        UWORD pel_byte;
        UWORD pel_bit;
        ULONG nsid;

        if (error_count == 0)
            continue;

        sqid = read_le16((const UBYTE *)&slot->sqid);
        cmdid = read_le16((const UBYTE *)&slot->cmdid);
        status_field = read_le16((const UBYTE *)&slot->status_field);
        pel = read_le16((const UBYTE *)&slot->param_error_location);
        nsid = read_le32((const UBYTE *)&slot->nsid);
        phase_tag = (UWORD)(status_field & 0x1U);
        sc = (UWORD)((ULONG)status_field & NVME_SC_MASK);
        sct = (UWORD)((ULONG)status_field & NVME_SCT_MASK);
        pel_byte = (UWORD)(pel & 0x00ffU);
        pel_bit = (UWORD)((pel >> 8) & 0x7U);
        format_u64_decimal(error_count, error_count_text, sizeof(error_count_text));

        printed_any = TRUE;
        Printf((CONST_STRPTR)"Entry %lu\n", i + 1UL);
        Printf((CONST_STRPTR)"  Error Count:          %s\n", (ULONG)error_count_text);
        Printf((CONST_STRPTR)"  SQID:                 %lu\n", (ULONG)sqid);
        Printf((CONST_STRPTR)"  Command ID:           0x%04lx\n", (ULONG)cmdid);
        Printf((CONST_STRPTR)"  Status Field:         0x%04lx\n", (ULONG)status_field);
        Printf((CONST_STRPTR)"    Phase Tag:          %lu\n", (ULONG)phase_tag);
        Printf((CONST_STRPTR)"    SC:                 0x%02lx\n", (ULONG)sc);
        Printf((CONST_STRPTR)"    SCT:                0x%02lx (%s)\n",
               (ULONG)(sct >> 8), (ULONG)nvme_status_type_name(sct));
        Printf((CONST_STRPTR)"    More:               %s\n",
               (ULONG)((status_field & NVME_STATUS_MORE) != 0 ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no"));
        Printf((CONST_STRPTR)"    Do Not Retry:       %s\n",
               (ULONG)((status_field & NVME_STATUS_DNR) != 0 ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no"));
        Printf((CONST_STRPTR)"  Param Error Location: 0x%04lx\n", (ULONG)pel);
        Printf((CONST_STRPTR)"    Byte:               %lu\n", (ULONG)pel_byte);
        Printf((CONST_STRPTR)"    Bit:                %lu\n", (ULONG)pel_bit);
        Printf((CONST_STRPTR)"  NSID:                 %lu\n", nsid);
        Printf((CONST_STRPTR)"  LBA:                  0x%08lx%08lx\n",
               (ULONG)(lba >> 32), (ULONG)lba);
        Printf((CONST_STRPTR)"  Vendor Specific:      0x%02lx\n", (ULONG)slot->vs);
        Printf((CONST_STRPTR)"  Command Specific:     0x%08lx%08lx\n",
               (ULONG)(cs >> 32), (ULONG)cs);
    }

    if (!printed_any)
        Printf((CONST_STRPTR)"No populated error log entries.\n");

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, log_size);
    return rc;
}

int run_self_test(void)
{
    struct nvmeadm_session session;
    struct nvmeadm_self_test_log *log;
    ULONG i;
    BOOL printed_any = FALSE;
    UBYTE current_operation;
    UBYTE current_completion;
    int rc = 20;

    log = (struct nvmeadm_self_test_log *)nvmeadm_alloc_clear(sizeof(*log));
    if (!log)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Self-test log",
                               NVME_NSID_ALL, NVME_LOG_DEVICE_SELF_TEST, 0, 0,
                               log, sizeof(*log)))
        goto out_close;

    current_operation = (UBYTE)(log->current_operation & 0x0fU);
    current_completion = (UBYTE)(log->current_completion & 0x7fU);

    Printf((CONST_STRPTR)"Device Self-test Log\n");
    Printf((CONST_STRPTR)"Current Operation:      0x%02lx (%s)\n",
           (ULONG)current_operation,
           (ULONG)self_test_op_name(current_operation));
    if (current_operation == 0x00)
        Printf((CONST_STRPTR)"Completion:             n/a\n");
    else
        Printf((CONST_STRPTR)"Completion:             %lu %%\n",
               (ULONG)current_completion);

    for (i = 0; i < 20; i++)
    {
        const struct nvmeadm_st_result *entry = &log->result[i];
        UBYTE result = (UBYTE)(entry->dsts & 0x0fU);
        UBYTE code = (UBYTE)((entry->dsts >> 4) & 0x0fU);
        unsigned long long poh;
        unsigned long long flba;
        ULONG nsid;

        if (result == 0x0fU || code == 0x00U ||
            bytes_all_zero((const UBYTE *)entry, sizeof(*entry)))
            continue;

        poh = read_le64(entry->poh);
        flba = read_le64(entry->flba);
        nsid = read_le32(entry->nsid);

        printed_any = TRUE;
        Printf((CONST_STRPTR)"Result %lu\n", i + 1UL);
        Printf((CONST_STRPTR)"  Self-test Code:       0x%02lx (%s)\n",
               (ULONG)code, (ULONG)self_test_code_name(code));
        Printf((CONST_STRPTR)"  Result:               0x%02lx (%s)\n",
               (ULONG)result, (ULONG)self_test_result_name(result));
        Printf((CONST_STRPTR)"  Segment:              %lu\n", (ULONG)entry->seg);
        Printf((CONST_STRPTR)"  Valid Diagnostic Info:0x%02lx\n", (ULONG)entry->vdi);
        Printf((CONST_STRPTR)"  Power On Hours:       %lu\n", (ULONG)poh);
        Printf((CONST_STRPTR)"  NSID:                 %lu\n", nsid);
        Printf((CONST_STRPTR)"  Failing LBA:          0x%08lx%08lx\n",
               (ULONG)(flba >> 32), (ULONG)flba);
        Printf((CONST_STRPTR)"  Status Code Type:     0x%02lx\n", (ULONG)entry->sct);
        Printf((CONST_STRPTR)"  Status Code:          0x%02lx\n", (ULONG)entry->sc);
        Printf((CONST_STRPTR)"  Vendor Specific:      0x%02lx%02lx\n",
               (ULONG)entry->vs[0], (ULONG)entry->vs[1]);
    }

    if (!printed_any)
        Printf((CONST_STRPTR)"No completed self-test results recorded.\n");

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, sizeof(*log));
    return rc;
}

int run_self_test_control(UBYTE stc, CONST_STRPTR label)
{
    struct nvmeadm_session session;
    struct NVMePassthruCmd cmd;
    ULONG actual = 0;
    BYTE status;
    int rc = 20;

    if (!nvmeadm_open(&session))
        return 20;

    nvmeadm_build_self_test(&cmd, NVME_NSID_ALL, stc);
    status = nvmeadm_admin_passthru(&session, &cmd, &actual);
    if (status != 0)
    {
        print_admin_failure(label, status, cmd.pt_Result, actual);
        goto out_close;
    }

    Printf((CONST_STRPTR)"%s submitted successfully\n", (ULONG)label);
    Printf((CONST_STRPTR)"Completion result:       0x%08lx\n", cmd.pt_Result);
    rc = 0;

out_close:
    nvmeadm_close(&session);
    return rc;
}

int run_get_feature(void)
{
    static const struct
    {
        UBYTE fid;
        CONST_STRPTR label;
    } features[] = {
        { NVME_FEAT_POWER_MGMT,  (CONST_STRPTR)"Power Management:" },
        { NVME_FEAT_TEMP_THRESH, (CONST_STRPTR)"Temperature Threshold:" },
        { NVME_FEAT_ERR_RECOVERY, (CONST_STRPTR)"Error Recovery:" },
        { NVME_FEAT_VOLATILE_WC, (CONST_STRPTR)"Volatile Write Cache:" },
        { NVME_FEAT_NUM_QUEUES,  (CONST_STRPTR)"Number of Queues:" },
        { NVME_FEAT_IRQ_COALESCE, (CONST_STRPTR)"IRQ Coalescing:" },
        { NVME_FEAT_ASYNC_EVENT, (CONST_STRPTR)"Async Event Config:" },
        { NVME_FEAT_KATO,        (CONST_STRPTR)"Keep Alive Timeout:" },
        { NVME_FEAT_NOPSC,       (CONST_STRPTR)"Non-Operational PS:" },
    };
    struct nvmeadm_session session;
    struct NVMePassthruCmd cmd;
    ULONG actual = 0;
    BYTE status;
    ULONG i;
    int rc = 20;

    if (!nvmeadm_open(&session))
        return 20;

    Printf((CONST_STRPTR)"Get Features (controller scope)\n");
    for (i = 0; i < (ULONG)(sizeof(features) / sizeof(features[0])); i++)
    {
        nvmeadm_build_get_features(&cmd, 0, features[i].fid, 0, NULL, 0);
        status = nvmeadm_admin_passthru(&session, &cmd, &actual);
        if (status != 0)
        {
            Printf((CONST_STRPTR)"%-24s failed: status=0x%02lx result=0x%08lx\n",
                   (ULONG)features[i].label,
                   (ULONG)(UBYTE)status,
                   cmd.pt_Result);
            continue;
        }

        print_feature_value(features[i].label, features[i].fid, cmd.pt_Result);
    }

    rc = 0;
    nvmeadm_close(&session);
    return rc;
}

int run_changed_ns(void)
{
    const ULONG entry_count = NVME_MAX_CHANGED_NAMESPACES;
    const ULONG log_size = entry_count * (ULONG)sizeof(ULONG);
    struct nvmeadm_session session;
    UBYTE *log;
    ULONG i;
    BOOL printed_any = FALSE;
    int rc = 20;

    log = (UBYTE *)nvmeadm_alloc_clear(log_size);
    if (!log)
        return 20;

    if (!nvmeadm_open(&session))
        goto out_free;

    if (!nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Changed Namespace log",
                               NVME_NSID_ALL, NVME_LOG_CHANGED_NS, 0, NVME_CSI_NVM,
                               log, log_size))
        goto out_close;

    Printf((CONST_STRPTR)"Changed Namespace List\n");

    for (i = 0; i < entry_count; i++)
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

    rc = 0;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(log, log_size);
    return rc;
}