// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static CONST_STRPTR format_secure_erase_name(ULONG ses)
{
    switch (ses)
    {
     case 0: return (CONST_STRPTR)"none";
     case 1: return (CONST_STRPTR)"user data erase";
     case 2: return (CONST_STRPTR)"cryptographic erase";
     default: return (CONST_STRPTR)"unknown";
    }
}

static void print_format_summary(ULONG unit, ULONG nsid, ULONG current_lbaf,
                     ULONG requested_lbaf, ULONG sector_size,
                     UWORD metadata_size, ULONG ses,
                     BOOL currently_supported,
                     BOOL requested_supported)
{
    Printf((CONST_STRPTR)"Format NVM\n");
    print_kv_u32((CONST_STRPTR)"  Unit:", unit);
    print_kv_u32((CONST_STRPTR)"  NSID:", nsid);
    print_kv_u32((CONST_STRPTR)"  Current LBAF:", current_lbaf);
    print_kv_u32((CONST_STRPTR)"  Requested LBAF:", requested_lbaf);
    Printf((CONST_STRPTR)"  Requested Sector:     %lu bytes\n", sector_size);
    Printf((CONST_STRPTR)"  Requested Metadata:   %lu bytes\n", (ULONG)metadata_size);
    Printf((CONST_STRPTR)"  Secure Erase:         %s (%lu)\n",
        (ULONG)format_secure_erase_name(ses), ses);
    print_kv_str((CONST_STRPTR)"  Current Driver View:", (CONST_STRPTR)(currently_supported ? (CONST_STRPTR)"usable" : (CONST_STRPTR)"not-exposed"));
    print_kv_str((CONST_STRPTR)"  Requested Driver View:", (CONST_STRPTR)(requested_supported ? (CONST_STRPTR)"usable" : (CONST_STRPTR)"not-exposed"));
    Printf((CONST_STRPTR)"  Opcode:               0x%02lx (Format NVM)\n",
        (ULONG)nvme_admin_format_nvm);
}

static void print_sanitize_summary(ULONG unit, CONST_STRPTR action_name,
                    UBYTE sanact, BOOL ause,
                    BOOL has_owpass, UBYTE owpass,
                    BOOL oipbp, BOOL nodas)
{
    Printf((CONST_STRPTR)"Sanitize NVM\n");
    print_kv_u32((CONST_STRPTR)"  Unit:", unit);
    print_kv_str((CONST_STRPTR)"  Action:", (CONST_STRPTR)action_name);
    print_kv_hex8((CONST_STRPTR)"  SANACT:", (ULONG)sanact);
    print_kv_str((CONST_STRPTR)"  AUSE:", (CONST_STRPTR)(ause ? (CONST_STRPTR)"set" : (CONST_STRPTR)"clear"));
    if (has_owpass)
    {
        print_kv_u32((CONST_STRPTR)"  OWPASS:", (ULONG)owpass);
    }
    else
    {
        Printf((CONST_STRPTR)"  OWPASS:               default (controller-defined; 0 means 16 passes if used)\n");
    }
    print_kv_str((CONST_STRPTR)"  OIPBP:", (CONST_STRPTR)(oipbp ? (CONST_STRPTR)"set" : (CONST_STRPTR)"clear"));
    print_kv_str((CONST_STRPTR)"  NODAS:", (CONST_STRPTR)(nodas ? (CONST_STRPTR)"set" : (CONST_STRPTR)"clear"));
    Printf((CONST_STRPTR)"  Opcode:               0x%02lx (Sanitize NVM)\n",
           (ULONG)nvme_admin_sanitize_nvm);
}

static CONST_STRPTR sanitize_nodmmas_name(ULONG nodmmas)
{
    switch (nodmmas)
    {
        case 0: return (CONST_STRPTR)"not reported";
        case 1: return (CONST_STRPTR)"media not modified after sanitize";
        case 2: return (CONST_STRPTR)"media modified after sanitize";
        default: return (CONST_STRPTR)"reserved";
    }
}

static void print_sanitize_capabilities(ULONG sanicap)
{
    print_kv_hex32((CONST_STRPTR)"Sanitize Capabilities:", sanicap);
    print_yes_no((CONST_STRPTR)"  Crypto Erase:", (sanicap & NVME_SANICAP_CES) != 0);
    print_yes_no((CONST_STRPTR)"  Block Erase:", (sanicap & NVME_SANICAP_BES) != 0);
    print_yes_no((CONST_STRPTR)"  Overwrite:", (sanicap & NVME_SANICAP_OWS) != 0);
    print_yes_no((CONST_STRPTR)"  No-Dealloc Inhibited:", (sanicap & NVME_SANICAP_NDI) != 0);
    ULONG nodmmas = (sanicap & NVME_SANICAP_NODMMAS_MASK) >> NVME_SANICAP_NODMMAS_SHIFT;
    Printf((CONST_STRPTR)"  NODMMAS:              %lu (%s)\n",
           nodmmas, (ULONG)sanitize_nodmmas_name(nodmmas));
}

static CONST_STRPTR sanitize_status_name(ULONG status)
{
    switch (status)
    {
        case NVME_SSTAT_NEVER_SANITIZED: return (CONST_STRPTR)"never sanitized";
        case NVME_SSTAT_COMPLETE_SUCCESS: return (CONST_STRPTR)"last sanitize completed successfully";
        case NVME_SSTAT_IN_PROGRESS: return (CONST_STRPTR)"sanitize in progress";
        case NVME_SSTAT_COMPLETED_FAILED: return (CONST_STRPTR)"last sanitize failed";
        case NVME_SSTAT_COMPLETE_SUCCESS_NO_DEALLOC: return (CONST_STRPTR)"completed successfully, no deallocation";
        default: return (CONST_STRPTR)"reserved";
    }
}

static void print_sanitize_time(CONST_STRPTR label, ULONG seconds)
{
    if (seconds == 0xFFFFFFFFUL)
        print_kv_str(label, (CONST_STRPTR)"not reported");
    else
        Printf((CONST_STRPTR)"%-24s %lu s\n", (ULONG)label, seconds);
}

static void print_sanitize_log(const struct nvme_sanitize_log_page *log)
{
    UWORD sstat = read_le16((const UBYTE *)&log->sstat);
    ULONG status = (ULONG)sstat & NVME_SSTAT_STATUS_MASK;

    Printf((CONST_STRPTR)"Status:                 0x%lx (%s)\n",
           status, (ULONG)sanitize_status_name(status));

    if (status == NVME_SSTAT_IN_PROGRESS)
    {
        UWORD sprog = read_le16((const UBYTE *)&log->sprog);
        Printf((CONST_STRPTR)"Progress:               %lu %%\n",
               ((ULONG)sprog * 100UL) >> 16);
    }

    print_kv_u32((CONST_STRPTR)"Overwrite Passes:",
                 ((ULONG)sstat & NVME_SSTAT_OWPASS_MASK) >> NVME_SSTAT_OWPASS_SHIFT);
    print_yes_no((CONST_STRPTR)"Global Data Erased:", (sstat & NVME_SSTAT_GDE) != 0);
    print_kv_hex32((CONST_STRPTR)"Last Sanitize CDW10:", read_le32((const UBYTE *)&log->scdw10));
    print_sanitize_time((CONST_STRPTR)"Est. Overwrite:", read_le32((const UBYTE *)&log->eto));
    print_sanitize_time((CONST_STRPTR)"Est. Block Erase:", read_le32((const UBYTE *)&log->etbe));
    print_sanitize_time((CONST_STRPTR)"Est. Crypto Erase:", read_le32((const UBYTE *)&log->etce));
}

int run_format_nvm(void)
{
    ULONG lbaf;
    if (!nvmeadm_require_lbaf(&lbaf))
        return RETURN_ERROR;

    ULONG ses = 0;
    if (nvmeadm_cli()->has_ses)
    {
        ses = nvmeadm_cli()->ses;
        if (ses > 2UL)
        {
            Printf((CONST_STRPTR)"format only supports SES values 0, 1, or 2\n");
            return RETURN_ERROR;
        }
    }

    struct nvme_id_ns *ns = (struct nvme_id_ns *)nvmeadm_alloc_clear(sizeof(*ns));
    if (!ns)
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free;

    /* The namespace is always the opened unit's own. */
    ULONG nsid = session.info.nui_Nsid;
    ULONG unit = session.info.nui_UnitNumber;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Namespace",
                                nsid, NVME_ID_CNS_NS, ns, sizeof(*ns)))
        goto out_close;

    if (lbaf > (ULONG)ns->nlbaf)
    {
        Printf((CONST_STRPTR)"Requested LBAF %lu is out of range for this namespace (max=%lu)\n",
               lbaf, (ULONG)ns->nlbaf);
        goto out_close;
    }

    if (lbaf > 15UL)
    {
        Printf((CONST_STRPTR)"Requested LBAF %lu needs extended Format NVM encoding; this first implementation supports 0..15 only\n",
               lbaf);
        goto out_close;
    }

    ULONG current_lbaf = (ULONG)nvme_lbaf_index(ns->flbas);
    ULONG current_sector_size = 0;
    UWORD current_metadata_size = 0;
    BOOL current_supported = FALSE;
    if (current_lbaf <= (ULONG)ns->nlbaf)
    {
        current_metadata_size = read_le16((const UBYTE *)&ns->lbaf[current_lbaf].ms);
        current_sector_size = lbaf_data_size_bytes(&ns->lbaf[current_lbaf]);
        current_supported = namespace_plain_block_supported(ns,
                                                            current_sector_size,
                                                            current_metadata_size);
    }

    UWORD requested_metadata_size = read_le16((const UBYTE *)&ns->lbaf[lbaf].ms);
    ULONG requested_sector_size = lbaf_data_size_bytes(&ns->lbaf[lbaf]);
    BOOL requested_supported = candidate_plain_block_supported(requested_sector_size,
                                                          requested_metadata_size);

    print_format_summary(unit, nsid, current_lbaf, lbaf,
                         requested_sector_size, requested_metadata_size,
                         ses, current_supported, requested_supported);

    if (current_lbaf != lbaf)
    {
        Printf((CONST_STRPTR)"  Warning:              requested LBAF differs from the current namespace format\n");
    }

    if (!requested_supported)
    {
        Printf((CONST_STRPTR)"  Warning:              requested format will not be exposed by the current Amiga driver model\n");
        Printf((CONST_STRPTR)"Refusing to submit Format NVM for a non-exposed namespace shape\n");
        goto out_close;
    }

    if (!nvmeadm_require_confirm())
        goto out_close;

    struct NVMePassthruCmd cmd;
    nvmeadm_build_format_nvm(&cmd, nsid, (UBYTE)lbaf, (UBYTE)ses);
    ULONG result = 0;
    if (!nvmeadm_submit(&session, &cmd, (CONST_STRPTR)"Format NVM", &result))
        goto out_close;

    Printf((CONST_STRPTR)"Format NVM submitted successfully\n");
    print_kv_hex32((CONST_STRPTR)"Completion result:", result);
    Printf((CONST_STRPTR)"Follow-up:               namespace readiness may change until the controller finishes formatting\n");
    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(ns, sizeof(*ns));
    return rc;
}

int run_sanitize_control(UBYTE sanact, CONST_STRPTR action_name)
{
    ULONG unit = nvmeadm_cli()->unit;

    BOOL ause = nvmeadm_cli()->ause;
    BOOL has_owpass = nvmeadm_cli()->has_owpass;
    ULONG cli_owpass = nvmeadm_cli()->owpass;
    BOOL oipbp = nvmeadm_cli()->oipbp;
    BOOL nodas = nvmeadm_cli()->nodas;

    if ((ause || nodas) && sanact == NVMEADM_SANITIZE_EXIT_FAILURE)
    {
        Printf((CONST_STRPTR)"AUSE and NODAS are not valid with sanitize exit-failure\n");
        return RETURN_ERROR;
    }

    if (sanact != NVMEADM_SANITIZE_OVERWRITE)
    {
        if (has_owpass || oipbp)
        {
            Printf((CONST_STRPTR)"OWPASS and OIPBP are only valid with sanitize overwrite\n");
            return RETURN_ERROR;
        }
    }
    else if (has_owpass && cli_owpass > 15UL)
    {
        Printf((CONST_STRPTR)"OWPASS must be in the range 0-15\n");
        return RETURN_ERROR;
    }

    print_sanitize_summary(unit, action_name, sanact,
                           ause, has_owpass, (UBYTE)cli_owpass,
                           oipbp, nodas);
    if (!nvmeadm_require_confirm())
        return RETURN_FAIL;

    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct NVMePassthruCmd cmd;
    nvmeadm_build_sanitize_nvm(&cmd, sanact, ause, (UBYTE)cli_owpass, oipbp, nodas);
    ULONG result = 0;
    if (!nvmeadm_submit(&session, &cmd, (CONST_STRPTR)"Sanitize NVM", &result))
        goto out_close;

    Printf((CONST_STRPTR)"Sanitize NVM submitted successfully\n");
    print_kv_hex32((CONST_STRPTR)"Completion result:", result);
    Printf((CONST_STRPTR)"Follow-up:               run 'nvmeadm sanitize-status' to track progress and completion\n");
    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
    return rc;
}

int run_sanitize_status(void)
{
    struct nvme_id_ctrl *ctrl = (struct nvme_id_ctrl *)nvmeadm_alloc_clear(sizeof(*ctrl));
    if (!ctrl)
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        goto out_free;

    if (!nvmeadm_fetch_identify(&session, (CONST_STRPTR)"Identify Controller",
                                0, NVME_ID_CNS_CTRL, ctrl, sizeof(*ctrl)))
        goto out_close;

    ULONG sanicap = read_le32((const UBYTE *)&ctrl->sanicap);
    Printf((CONST_STRPTR)"Sanitize Status\n");
    if (sanicap == 0)
    {
        Printf((CONST_STRPTR)"Sanitize:               not supported by this controller\n");
        rc = RETURN_OK;
        goto out_close;
    }

    print_sanitize_capabilities(sanicap);

    struct nvme_sanitize_log_page *log =
        (struct nvme_sanitize_log_page *)nvmeadm_alloc_clear(sizeof(*log));
    if (!log)
        goto out_close;

    if (nvmeadm_fetch_get_log(&session, (CONST_STRPTR)"Sanitize Status log",
                              NVME_NSID_ALL, NVME_LOG_SANITIZE, 0, 0,
                              log, sizeof(*log)))
    {
        print_sanitize_log(log);
    }
    FreeMem(log, sizeof(*log));

    /* Feature 17h is the *configured* no-deallocate response mode, distinct
     * from the log's completion status above.  It is an NVMe 1.4 addition
     * that controllers may lack even when they support Sanitize, so probe
     * quietly and report a miss as absence. */
    struct NVMePassthruCmd cmd;
    nvmeadm_build_get_features(&cmd, 0, NVME_FEAT_SANITIZE, 0, NULL, 0);
    ULONG result = 0;
    if (nvmeadm_submit_quiet(&session, &cmd, &result))
        print_feature_value((CONST_STRPTR)"Configured NODRM:", NVME_FEAT_SANITIZE, result);
    else
        Printf((CONST_STRPTR)"%-24s not reported by this controller\n",
               (ULONG)"Configured NODRM:");

    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
out_free:
    FreeMem(ctrl, sizeof(*ctrl));
    return rc;
}
