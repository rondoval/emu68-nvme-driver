// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

void print_kv_str(CONST_STRPTR label, CONST_STRPTR value)
{
    Printf((CONST_STRPTR)"%-24s %s\n", (ULONG)label, (ULONG)value);
}

void print_kv_u32(CONST_STRPTR label, ULONG value)
{
    Printf((CONST_STRPTR)"%-24s %lu\n", (ULONG)label, value);
}

void print_kv_hex8(CONST_STRPTR label, ULONG value)
{
    Printf((CONST_STRPTR)"%-24s 0x%02lx\n", (ULONG)label, value);
}

void print_kv_hex16(CONST_STRPTR label, ULONG value)
{
    Printf((CONST_STRPTR)"%-24s 0x%04lx\n", (ULONG)label, value);
}

void print_kv_hex32(CONST_STRPTR label, ULONG value)
{
    Printf((CONST_STRPTR)"%-24s 0x%08lx\n", (ULONG)label, value);
}

void print_yes_no(CONST_STRPTR label, BOOL enabled)
{
    Printf((CONST_STRPTR)"%-24s %s\n",
           (ULONG)label,
           (ULONG)(enabled ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no"));
}

void print_fw_revision(CONST_STRPTR label, const UBYTE *rev)
{
    char text[9];

    CopyMem(rev, text, 8);
    text[8] = '\0';
    Printf((CONST_STRPTR)"%-24s %.8s\n", (ULONG)label, (ULONG)text);
}

void print_feature_value(CONST_STRPTR label, UBYTE fid, ULONG value)
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
        case NVME_FEAT_HOST_MEM_BUF:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  %s\n",
                   (ULONG)label, value,
                   (ULONG)(((value & 0x1UL) != 0) ? "enabled" : "disabled"));
            break;
        case NVME_FEAT_TIMESTAMP:
            Printf((CONST_STRPTR)"%-24s 0x%08lx\n",
                   (ULONG)label, value);
            break;
        case NVME_FEAT_SANITIZE:
            Printf((CONST_STRPTR)"%-24s 0x%08lx  no-deallocate=%s\n",
                   (ULONG)label, value,
                   (ULONG)(((value & 0x1UL) != 0) ? "set" : "clear"));
            break;
        default:
            Printf((CONST_STRPTR)"%-24s 0x%08lx\n",
                   (ULONG)label, value);
            break;
    }
}

ULONG lbaf_data_size_bytes(const struct nvme_lbaf *lbaf)
{
    if ((ULONG)lbaf->ds >= 32UL)
        return 0;

    return 1UL << lbaf->ds;
}

BOOL namespace_plain_block_supported(const struct nvme_id_ns *id,
                                     ULONG sector_size, UWORD metadata_size)
{
    UBYTE pi_type = (UBYTE)(id->dps & NVME_NS_DPS_PI_MASK);

    if (metadata_size != 0)
        return FALSE;
    if (pi_type != 0)
        return FALSE;
    return sector_size >= 512UL && sector_size <= 4096UL;
}

BOOL candidate_plain_block_supported(ULONG sector_size, UWORD metadata_size)
{
    if (metadata_size != 0)
        return FALSE;
    return sector_size >= 512UL && sector_size <= 4096UL;
}

BOOL nvmeadm_open_cli_session(struct nvmeadm_session *session)
{
    return nvmeadm_open_unit(session, nvmeadm_cli()->unit);
}

BOOL nvmeadm_require_file(CONST_STRPTR *file_out)
{
    if (!nvmeadm_cli()->file)
    {
        Printf((CONST_STRPTR)"This command requires FILE <path>\n");
        SetIoErr(ERROR_REQUIRED_ARG_MISSING);
        return FALSE;
    }

    if (file_out)
        *file_out = nvmeadm_cli()->file;
    return TRUE;
}

BOOL nvmeadm_require_lbaf(ULONG *lbaf_out)
{
    if (!nvmeadm_cli()->has_lbaf)
    {
        Printf((CONST_STRPTR)"This command requires LBAF <n>\n");
        SetIoErr(ERROR_REQUIRED_ARG_MISSING);
        return FALSE;
    }

    if (lbaf_out)
        *lbaf_out = nvmeadm_cli()->lbaf;
    return TRUE;
}

/* Polled in long loops so Ctrl-C aborts with the conventional "***Break". */
BOOL nvmeadm_check_break(void)
{
    if (CheckSignal(SIGBREAKF_CTRL_C) == 0)
        return FALSE;

    PrintFault(ERROR_BREAK, NULL);
    return TRUE;
}

BOOL nvmeadm_require_confirm(void)
{
    if (!nvmeadm_cli()->confirm)
    {
        Printf((CONST_STRPTR)"Refusing to submit without CONFIRM\n");
        return FALSE;
    }

    return TRUE;
}

APTR nvmeadm_read_log(CONST_STRPTR operation, ULONG nsid,
                      UBYTE log_page, UBYTE lsp, UBYTE csi, ULONG size)
{
    APTR buffer = nvmeadm_alloc_clear(size);
    if (!buffer)
        return NULL;

    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
    {
        FreeMem(buffer, size);
        return NULL;
    }

    BOOL ok = nvmeadm_fetch_get_log(&session, operation, nsid,
                                    log_page, lsp, csi, buffer, size);
    nvmeadm_close(&session);
    if (!ok)
    {
        FreeMem(buffer, size);
        return NULL;
    }

    return buffer;
}

APTR nvmeadm_read_identify(CONST_STRPTR operation, ULONG nsid,
                           ULONG cns, ULONG size)
{
    APTR buffer = nvmeadm_alloc_clear(size);
    if (!buffer)
        return NULL;

    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
    {
        FreeMem(buffer, size);
        return NULL;
    }

    BOOL ok = nvmeadm_fetch_identify(&session, operation, nsid,
                                     cns, buffer, size);
    nvmeadm_close(&session);
    if (!ok)
    {
        FreeMem(buffer, size);
        return NULL;
    }

    return buffer;
}
