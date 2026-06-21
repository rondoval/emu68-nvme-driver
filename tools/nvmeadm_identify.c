// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static BOOL nvmeadm_id_cns_ok(ULONG version, UBYTE cns)
{
    /* CNS 05h-08h (command-set specific / CS-independent) are NVMe 2.0. */
    if (cns >= NVME_ID_CNS_CS_NS && cns <= NVME_ID_CNS_NS_CS_INDEP)
        return version >= NVME_VS(2, 0, 0);

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

static void print_mdts(UBYTE mdts, UBYTE mpsmin)
{
    unsigned long long bytes;

    if (mdts == 0)
    {
        Printf((CONST_STRPTR)"MDTS:                   not reported\n");
        return;
    }

    Printf((CONST_STRPTR)"MDTS:                   raw=%lu  => 2^%lu * min page size\n",
           (ULONG)mdts, (ULONG)mdts);
    if (mdts_bytes_for_mpsmin(mdts, mpsmin, &bytes))
        print_binary_size((CONST_STRPTR)"MDTS Bytes:", bytes);
}

static void print_hmb_capabilities(const struct nvme_id_ctrl *ctrl)
{
    ULONG preferred_size = read_le32((const UBYTE *)&ctrl->hmpre);
    ULONG minimum_size = read_le32((const UBYTE *)&ctrl->hmmin);
    ULONG minimum_desc_size = read_le32((const UBYTE *)&ctrl->hmminds);

    if (preferred_size == 0 && minimum_size == 0 && minimum_desc_size == 0)
    {
        print_kv_str((CONST_STRPTR)"Host Memory Buffer:",
                     (CONST_STRPTR)"not reported or not supported");
        return;
    }

    print_kv_str((CONST_STRPTR)"Host Memory Buffer:", (CONST_STRPTR)"reported");
    print_kv_hex32((CONST_STRPTR)"  Preferred Size:", preferred_size);
    print_kv_hex32((CONST_STRPTR)"  Minimum Size:", minimum_size);
    print_kv_hex32((CONST_STRPTR)"  Min Desc Entry Size:", minimum_desc_size);
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

static CONST_STRPTR lbaf_relative_performance_name(UBYTE rp)
{
    switch (rp)
    {
        case NVME_LBAF_RP_BEST: return (CONST_STRPTR)"best";
        case NVME_LBAF_RP_BETTER: return (CONST_STRPTR)"better";
        case NVME_LBAF_RP_GOOD: return (CONST_STRPTR)"good";
        case NVME_LBAF_RP_DEGRADED: return (CONST_STRPTR)"degraded";
        default: return (CONST_STRPTR)"unknown";
    }
}

static void print_u64_le_decimal_line(CONST_STRPTR label, const UBYTE *value)
{
    char text[40];

    format_u64_decimal(read_le64(value), text, sizeof(text));
    Printf((CONST_STRPTR)"%-24s %s\n", (ULONG)label, (ULONG)text);
}

static void print_nsid_list_entry(ULONG nsid, const struct nvme_id_ns *id)
{
    UBYTE current_lbaf = nvme_lbaf_index(id->flbas);
    UWORD metadata_size = 0;
    ULONG sector_size = 0;
    BOOL supported = FALSE;

    if (current_lbaf <= id->nlbaf)
    {
        metadata_size = read_le16((const UBYTE *)&id->lbaf[current_lbaf].ms);
        sector_size = lbaf_data_size_bytes(&id->lbaf[current_lbaf]);
        supported = namespace_plain_block_supported(id, sector_size, metadata_size);
    }

    if (current_lbaf > id->nlbaf)
    {
        Printf((CONST_STRPTR)"NSID %lu: current LBAF out of range (nlbaf=%lu)\n",
               nsid, (ULONG)id->nlbaf);
        return;
    }

    Printf((CONST_STRPTR)"NSID %lu: sector=%lu metadata=%lu PI=%lu %s\n",
           nsid,
           sector_size,
           (ULONG)metadata_size,
           (ULONG)(id->dps & NVME_NS_DPS_PI_MASK),
           (ULONG)(supported ? (CONST_STRPTR)"usable" : (CONST_STRPTR)"not-exposed"));
}

int run_units(void)
{
    BOOL any = FALSE;

    for (ULONG unit = 0;; unit++)
    {
        if (nvmeadm_check_break())
            break;

        struct nvmeadm_session session;
        if (!nvmeadm_open_unit_quiet(&session, unit))
            break;

        any = TRUE;
        const struct NVMeUnitInfo *info = &session.info;
        if (unit == info->nui_CtrlFirstUnit)
        {
            Printf((CONST_STRPTR)"Controller at unit %lu: %s (S/N %s, FW %s, NVMe %lu.%lu)\n",
                   unit,
                   (ULONG)info->nui_Model,
                   (ULONG)info->nui_Serial,
                   (ULONG)info->nui_Firmware,
                   info->nui_Version >> 16,
                   (info->nui_Version >> 8) & 0xFFUL);
        }

        struct DriveGeometry dg;
        memset(&dg, 0, sizeof(dg));
        struct IOStdReq *req = (struct IOStdReq *)session.io;
        req->io_Command = TD_GETGEOMETRY;
        req->io_Data = &dg;
        req->io_Length = sizeof(dg);
        DoIO(session.io);

        Printf((CONST_STRPTR)"  Unit %lu:  NSID %lu  sector=%lu\n",
               unit, info->nui_Nsid, dg.dg_SectorSize);
        nvmeadm_close(&session);
    }

    if (!any)
    {
        Printf((CONST_STRPTR)"No nvme.device units found.\n");
        return RETURN_FAIL;
    }

    return RETURN_OK;
}

int run_identify(void)
{
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_read_identify(
        (CONST_STRPTR)"Identify Controller", 0, NVME_ID_CNS_CTRL, sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    Printf((CONST_STRPTR)"Identify Controller\n");
    Printf((CONST_STRPTR)"VID:                    %04lx\n",
           (ULONG)read_le16((const UBYTE *)&ctrl->vid));
    Printf((CONST_STRPTR)"SSVID:                  %04lx\n",
           (ULONG)read_le16((const UBYTE *)&ctrl->ssvid));
    Printf((CONST_STRPTR)"Serial Number:          %.20s\n", (ULONG)ctrl->sn);
    Printf((CONST_STRPTR)"Model Number:           %.40s\n", (ULONG)ctrl->mn);
    Printf((CONST_STRPTR)"Firmware Revision:      %.8s\n", (ULONG)ctrl->fr);

    FreeMem(ctrl, sizeof(*ctrl));
    return RETURN_OK;
}

int run_identify_ns(void)
{
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    struct nvme_id_ns *ns = (struct nvme_id_ns *)nvmeadm_alloc_clear(sizeof(*ns));
    if (!ns)
    {
        FreeMem(ctrl, sizeof(*ctrl));
        return RETURN_FAIL;
    }

    struct nvme_id_ns_cs_indep *ns_indep =
        (struct nvme_id_ns_cs_indep *)nvmeadm_alloc_clear(sizeof(*ns_indep));
    if (!ns_indep)
    {
        FreeMem(ns, sizeof(*ns));
        FreeMem(ctrl, sizeof(*ctrl));
        return RETURN_FAIL;
    }

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free_all;

    ULONG nsid = session.info.nui_Nsid;
    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, NVME_ID_CNS_CTRL, ctrl, sizeof(*ctrl)))
        goto out_close;

    ULONG version = read_le32((const UBYTE *)&ctrl->ver);
    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Namespace",
                                nsid, NVME_ID_CNS_NS, ns, sizeof(*ns)))
        goto out_close;

    /* Best-effort: CNS 08h is optional even on controllers that report
     * NVMe 2.0, so a miss is normal — the printout falls back below. */
    BOOL have_indep = FALSE;
    if (nvmeadm_id_cns_ok(version, NVME_ID_CNS_NS_CS_INDEP) &&
        nvmeadm_fetch_identify_quiet(&session, nsid, NVME_ID_CNS_NS_CS_INDEP,
                                     ns_indep, sizeof(*ns_indep)))
    {
        have_indep = TRUE;
    }

    UBYTE current_lbaf = nvme_lbaf_index(ns->flbas);
    ULONG lbaf_count = (ULONG)ns->nlbaf + 1UL;

    Printf((CONST_STRPTR)"Identify Namespace\n");
    print_kv_u32((CONST_STRPTR)"NSID:", nsid);
    print_u64_le_decimal_line((CONST_STRPTR)"Namespace Size:", (const UBYTE *)&ns->nsze);
    print_u64_le_decimal_line((CONST_STRPTR)"Namespace Capacity:", (const UBYTE *)&ns->ncap);
    print_u64_le_decimal_line((CONST_STRPTR)"Namespace Utilization:", (const UBYTE *)&ns->nuse);
    print_kv_hex8((CONST_STRPTR)"Namespace Features:", (ULONG)ns->nsfeat);
    Printf((CONST_STRPTR)"Current LBAF:           %lu%s\n",
           (ULONG)current_lbaf,
           (ULONG)((ns->flbas & NVME_NS_FLBAS_META_EXT) != 0 ? (CONST_STRPTR)" (metadata in extended LBA)" : (CONST_STRPTR)""));
    print_kv_hex8((CONST_STRPTR)"Metadata Capabilities:", (ULONG)ns->mc);
    print_kv_hex8((CONST_STRPTR)"Protection Capabilities:", (ULONG)ns->dpc);
    print_kv_hex8((CONST_STRPTR)"Protection Setting:", (ULONG)ns->dps);
    print_yes_no((CONST_STRPTR)"Shared Namespace:", (ns->nmic & NVME_NS_NMIC_SHARED) != 0);
    if (have_indep)
    {
        print_kv_str((CONST_STRPTR)"Namespace Ready:", (CONST_STRPTR)((ns_indep->nstat & NVME_NSTAT_NRDY) != 0 ? (CONST_STRPTR)"no" : (CONST_STRPTR)"yes"));
    }
    else
    {
        Printf((CONST_STRPTR)"Namespace Ready:        not reported by this controller path\n");
    }

    Printf((CONST_STRPTR)"\nLBA Formats\n");
    for (ULONG i = 0; i < lbaf_count && i < 64UL; i++)
    {
        const struct nvme_lbaf *lbaf = &ns->lbaf[i];
        UWORD metadata_size = read_le16((const UBYTE *)&lbaf->ms);
        ULONG sector_size = lbaf_data_size_bytes(lbaf);
        BOOL supported = namespace_plain_block_supported(ns, sector_size, metadata_size);

        Printf((CONST_STRPTR)"  LBAF %lu%s:          data=%lu metadata=%lu rp=%s %s\n",
               i,
               (ULONG)(i == current_lbaf ? (CONST_STRPTR)" [current]" : (CONST_STRPTR)""),
               sector_size,
               (ULONG)metadata_size,
               (ULONG)lbaf_relative_performance_name((UBYTE)(lbaf->rp & 0x3U)),
               (ULONG)(supported ? (CONST_STRPTR)"exposed" : (CONST_STRPTR)"not-exposed"));
    }

    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
out_free_all:
    FreeMem(ns_indep, sizeof(*ns_indep));
    FreeMem(ns, sizeof(*ns));
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}

int run_list_ns(void)
{
    UBYTE *list = (UBYTE *)nvmeadm_alloc_clear(4096);
    if (!list)
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free;

    struct nvme_id_ns *ns = (struct nvme_id_ns *)nvmeadm_alloc_clear(sizeof(*ns));
    if (!ns)
        goto out_close;

    Printf((CONST_STRPTR)"Active Namespaces\n");
    BOOL printed_any = FALSE;

    /* CNS 02h: the NSID field is a *starting* NSID — the controller returns
     * active NSIDs greater than it, in ascending order.  Page through until
     * a short list; starting values >= FFFFFFFEh are invalid per spec. */
    ULONG start_nsid = 0;
    for (;;)
    {
        if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Active Namespace List",
                                    start_nsid, NVME_ID_CNS_NS_ACTIVE_LIST,
                                    list, 4096))
            goto out_free_ns;

        ULONG count = 0;
        while (count < 1024UL && read_le32(list + (count * 4UL)) != 0)
            count++;

        for (ULONG i = 0; i < count; i++)
        {
            if (nvmeadm_check_break())
                goto out_free_ns;

            ULONG nsid = read_le32(list + (i * 4UL));

            memset(ns, 0, sizeof(*ns));
            if (nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Namespace",
                                       nsid, NVME_ID_CNS_NS, ns, sizeof(*ns)))
            {
                print_nsid_list_entry(nsid, ns);
            }
            else
            {
                Printf((CONST_STRPTR)"NSID %lu: Identify Namespace failed, skipped\n", nsid);
            }
            printed_any = TRUE;
        }

        if (count < 1024UL)
            break;

        start_nsid = read_le32(list + (1023UL * 4UL));
        if (start_nsid >= 0xFFFFFFFEUL)
            break;
    }

    if (!printed_any)
        Printf((CONST_STRPTR)"No active namespaces reported.\n");

    rc = RETURN_OK;

out_free_ns:
    FreeMem(ns, sizeof(*ns));
out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(list, 4096);
    return rc;
}

int run_identify_caps(void)
{
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    struct nvme_id_ctrl_nvm *nvm_ctrl =
        (struct nvme_id_ctrl_nvm *)nvmeadm_alloc_clear(sizeof(*nvm_ctrl));
    if (!nvm_ctrl)
    {
        FreeMem(ctrl, sizeof(*ctrl));
        return RETURN_FAIL;
    }

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free_all;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, 0x01UL, ctrl, sizeof(*ctrl)))
        goto out_close;

    ULONG version = read_le32((const UBYTE *)&ctrl->ver);
    ULONG ctratt = read_le32((const UBYTE *)&ctrl->ctratt);
    ULONG oaes = read_le32((const UBYTE *)&ctrl->oaes);
    UWORD cntlid = read_le16((const UBYTE *)&ctrl->cntlid);
    UWORD maxcmd = read_le16((const UBYTE *)&ctrl->maxcmd);
    UWORD oacs = read_le16((const UBYTE *)&ctrl->oacs);
    UWORD oncs = read_le16((const UBYTE *)&ctrl->oncs);
    UWORD hctma = read_le16((const UBYTE *)&ctrl->hctma);
    ULONG ioccsz = read_le32((const UBYTE *)&ctrl->ioccsz);
    BOOL can_probe_cs = nvmeadm_id_cns_ok(version, NVME_ID_CNS_CS_CTRL);

    BYTE nvm_status = 0;
    BYTE zns_status = 0;
    ULONG nvm_result = 0;
    ULONG zns_result = 0;
    BOOL nvm_supported = FALSE;
    BOOL zns_supported = FALSE;
    if (can_probe_cs)
    {
        nvm_supported = nvmeadm_fetch_identify_csi(&session, 0,
                                                   NVME_ID_CNS_CS_CTRL,
                                                   NVME_CSI_NVM,
                                                   nvm_ctrl, sizeof(*nvm_ctrl),
                                                   &nvm_status, &nvm_result);

        memset(nvm_ctrl, 0, sizeof(*nvm_ctrl));
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
    print_kv_u32((CONST_STRPTR)"Controller ID:", (ULONG)cntlid);
    Printf((CONST_STRPTR)"Async Req Limit:        %lu outstanding requests\n",
           (ULONG)ctrl->aerl + 1UL);
    print_kv_hex32((CONST_STRPTR)"OAES:", oaes);

    Printf((CONST_STRPTR)"\nController Multipath / Sharing\n");
    print_yes_no((CONST_STRPTR)"  Multi-port:",
                 (ctrl->cmic & NVME_CTRL_CMIC_MULTI_PORT) != 0);
    print_yes_no((CONST_STRPTR)"  Multi-controller:",
                 (ctrl->cmic & NVME_CTRL_CMIC_MULTI_CTRL) != 0);
    print_yes_no((CONST_STRPTR)"  ANA reporting:",
                 (ctrl->cmic & NVME_CTRL_CMIC_ANA) != 0);

    Printf((CONST_STRPTR)"\nTransfer / Queue Limits\n");
    /* CAP.MPSMIN lives in CAP bits 51:48. */
    print_mdts(ctrl->mdts, (UBYTE)((session.info.nui_CapHi >> 16) & 0xFU));
    print_queue_entry_size((CONST_STRPTR)"SQ Entry Size:", ctrl->sqes);
    print_queue_entry_size((CONST_STRPTR)"CQ Entry Size:", ctrl->cqes);
    print_kv_u32((CONST_STRPTR)"Max Outstanding Cmds:", (ULONG)maxcmd);
    Printf((CONST_STRPTR)"IO Command Capsule:     %lu bytes\n",
           ioccsz * 16UL);

    Printf((CONST_STRPTR)"\nAdmin Capabilities\n");
    print_kv_hex16((CONST_STRPTR)"OACS:", (ULONG)oacs);
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
    print_kv_hex16((CONST_STRPTR)"ONCS:", (ULONG)oncs);
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
    print_kv_hex8((CONST_STRPTR)"LPA:", (ULONG)ctrl->lpa);
    print_yes_no((CONST_STRPTR)"  Cmd Effects Log:",
                 (ctrl->lpa & NVME_CTRL_LPA_CMD_EFFECTS_LOG) != 0);
    print_kv_hex32((CONST_STRPTR)"AER Support Mask:", oaes);

    Printf((CONST_STRPTR)"\nController Attributes\n");
    print_kv_hex32((CONST_STRPTR)"CTRATT:", ctratt);
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
    print_kv_hex16((CONST_STRPTR)"HCTMA:", (ULONG)hctma);
    print_kv_hex8((CONST_STRPTR)"DSTO:", (ULONG)ctrl->dsto);

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

    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
out_free_all:
    FreeMem(nvm_ctrl, sizeof(*nvm_ctrl));
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}
