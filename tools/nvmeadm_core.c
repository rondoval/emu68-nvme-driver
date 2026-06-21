// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

UWORD read_le16(const UBYTE *bytes)
{
    return (UWORD)((UWORD)bytes[0] | (UWORD)((UWORD)bytes[1] << 8));
}

ULONG read_le32(const UBYTE *bytes)
{
    return (ULONG)bytes[0] |
           ((ULONG)bytes[1] << 8) |
           ((ULONG)bytes[2] << 16) |
           ((ULONG)bytes[3] << 24);
}

unsigned long long read_le64(const UBYTE *bytes)
{
    return (unsigned long long)bytes[0] |
           ((unsigned long long)bytes[1] << 8) |
           ((unsigned long long)bytes[2] << 16) |
           ((unsigned long long)bytes[3] << 24) |
           ((unsigned long long)bytes[4] << 32) |
           ((unsigned long long)bytes[5] << 40) |
           ((unsigned long long)bytes[6] << 48) |
           ((unsigned long long)bytes[7] << 56);
}

BOOL bytes_all_zero(const UBYTE *bytes, ULONG len)
{
    while (len-- != 0)
    {
        if (*bytes++ != 0)
            return FALSE;
    }
    return TRUE;
}

static void format_le_decimal(const UBYTE *value, ULONG value_len,
                              char *out, ULONG out_len)
{
    UBYTE tmp[24];
    char digits[40];
    ULONG digit_count = 0;
    ULONG out_pos = 0;

    if (!out || out_len == 0)
        return;

    if (value_len > (ULONG)sizeof(tmp))
    {
        out[0] = '\0';
        return;
    }

    if (bytes_all_zero(value, value_len))
    {
        if (out_len > 1)
        {
            out[0] = '0';
            out[1] = '\0';
        }
        else
        {
            out[0] = '\0';
        }
        return;
    }

    memset(tmp, 0, sizeof(tmp));
    CopyMem(value, tmp, value_len);
    while (!bytes_all_zero(tmp, value_len) &&
           digit_count < (ULONG)(sizeof(digits) - 1))
    {
        ULONG carry = 0;

        for (LONG i = (LONG)value_len - 1; i >= 0; i--)
        {
            ULONG cur = (carry << 8) | (ULONG)tmp[i];
            tmp[i] = (UBYTE)(cur / 10UL);
            carry = cur % 10UL;
        }
        digits[digit_count++] = (char)('0' + (char)carry);
    }

    while (digit_count > 0 && out_pos + 1 < out_len)
        out[out_pos++] = digits[--digit_count];
    out[out_pos] = '\0';
}

void format_le128_decimal(const UBYTE *value, char *out, ULONG out_len)
{
    format_le_decimal(value, 16, out, out_len);
}

void format_u64_decimal(unsigned long long value, char *out, ULONG out_len)
{
    UBYTE le_value[16] = {0};

    le_value[0] = (UBYTE)(value & 0xffULL);
    le_value[1] = (UBYTE)((value >> 8) & 0xffULL);
    le_value[2] = (UBYTE)((value >> 16) & 0xffULL);
    le_value[3] = (UBYTE)((value >> 24) & 0xffULL);
    le_value[4] = (UBYTE)((value >> 32) & 0xffULL);
    le_value[5] = (UBYTE)((value >> 40) & 0xffULL);
    le_value[6] = (UBYTE)((value >> 48) & 0xffULL);
    le_value[7] = (UBYTE)((value >> 56) & 0xffULL);

    format_le128_decimal(le_value, out, out_len);
}

static void mul_u128_small(const UBYTE *value, ULONG multiplier,
                           UBYTE *out, ULONG out_len)
{
    ULONG carry = 0;

    memset(out, 0, out_len);
    for (ULONG i = 0; i < 16 && i < out_len; i++)
    {
        ULONG product = ((ULONG)value[i] * multiplier) + carry;
        out[i] = (UBYTE)(product & 0xffUL);
        carry = product >> 8;
    }

    for (ULONG i = 16; i < out_len && carry != 0; i++)
    {
        out[i] = (UBYTE)(carry & 0xffUL);
        carry >>= 8;
    }
}

static BOOL le_bytes_ge_u16(const UBYTE *value, ULONG value_len, UWORD threshold)
{
    for (ULONG i = value_len; i-- > 2; )
    {
        if (value[i] != 0)
            return TRUE;
    }

    return read_le16(value) >= threshold;
}

static UWORD le_bytes_divide_u16(UBYTE *value, ULONG value_len, UWORD divisor)
{
    ULONG remainder = 0;

    for (LONG i = (LONG)value_len - 1; i >= 0; i--)
    {
        ULONG cur = (remainder << 8) | (ULONG)value[i];
        value[i] = (UBYTE)(cur / divisor);
        remainder = cur % divisor;
    }

    return (UWORD)remainder;
}

/* Scale a little-endian byte count to B/KiB/.../TiB and print it with one
 * decimal of precision.  The value buffer is consumed by the division. */
static void print_le_bytes_scaled(CONST_STRPTR label, UBYTE *bytes, ULONG len)
{
    static const CONST_STRPTR units[] = {
        (CONST_STRPTR)"B",
        (CONST_STRPTR)"KiB",
        (CONST_STRPTR)"MiB",
        (CONST_STRPTR)"GiB",
        (CONST_STRPTR)"TiB",
    };
    char integer_text[40];
    UWORD remainder = 0;
    ULONG unit_index = 0;

    while (unit_index < 4 && le_bytes_ge_u16(bytes, len, 1024))
    {
        remainder = le_bytes_divide_u16(bytes, len, 1024);
        unit_index++;
    }

    format_le_decimal(bytes, len, integer_text, sizeof(integer_text));
    if (unit_index == 0 || remainder == 0)
    {
        Printf((CONST_STRPTR)"%-24s %s %s\n",
               (ULONG)label, (ULONG)integer_text, (ULONG)units[unit_index]);
    }
    else
    {
        ULONG tenths = ((ULONG)remainder * 10UL) / 1024UL;

        Printf((CONST_STRPTR)"%-24s %s.%lu %s\n",
               (ULONG)label, (ULONG)integer_text, tenths, (ULONG)units[unit_index]);
    }
}

void print_smart_data_amount(CONST_STRPTR label, const UBYTE *value)
{
    UBYTE scaled[24];

    mul_u128_small(value, 512000UL, scaled, sizeof(scaled));
    print_le_bytes_scaled(label, scaled, sizeof(scaled));
}

void print_binary_size(CONST_STRPTR label, unsigned long long bytes)
{
    UBYTE le_bytes[8];

    for (ULONG i = 0; i < 8; i++)
        le_bytes[i] = (UBYTE)((bytes >> (i * 8)) & 0xffULL);
    print_le_bytes_scaled(label, le_bytes, sizeof(le_bytes));
}

CONST_STRPTR nvme_status_type_name(UWORD sct)
{
    switch ((ULONG)sct)
    {
        case NVME_SCT_GENERIC:
            return (CONST_STRPTR)"Generic";
        case 0x0100:
            return (CONST_STRPTR)"Command Specific";
        case NVME_SCT_MEDIA_ERROR:
            return (CONST_STRPTR)"Media/Data Integrity";
        case NVME_SCT_PATH:
            return (CONST_STRPTR)"Path";
        case 0x0700:
            return (CONST_STRPTR)"Vendor Specific";
        default:
            return (CONST_STRPTR)"Reserved";
    }
}

static ULONG nvme_bytes_to_numd(ULONG bytes)
{
    return (bytes >> 2) - 1UL;
}

static BOOL nvmeadm_fetch_unit_info(struct nvmeadm_session *session)
{
    struct IOStdReq *req = (struct IOStdReq *)session->io;

    req->io_Command = NSCMD_NVME_UNIT_INFO;
    req->io_Data = &session->info;
    req->io_Length = sizeof(session->info);
    DoIO(session->io);

    if (req->io_Error != 0)
    {
        Printf((CONST_STRPTR)"nvme.device lacks the unit-info query; update the driver\n");
        return FALSE;
    }

    return TRUE;
}

/* On failure nothing is left behind: partial port/IORequest state is
 * released here, so callers never call nvmeadm_close() for a failed open.
 * The quiet variant suppresses the OpenDevice failure message so unit scans
 * can probe past the last unit. */
static BOOL nvmeadm_open_unit_internal(struct nvmeadm_session *session,
                                       ULONG unit, BOOL quiet)
{
    memset(session, 0, sizeof(*session));

    session->port = CreateMsgPort();
    if (!session->port)
        return FALSE;

    session->io = (struct IORequest *)CreateIORequest(session->port,
                                                      sizeof(struct IOStdReq));
    if (!session->io)
    {
        nvmeadm_close(session);
        return FALSE;
    }

    BYTE open_err = OpenDevice((CONST_STRPTR)"nvme.device", unit, session->io, 0);
    if (open_err != 0)
    {
        if (!quiet)
            Printf((CONST_STRPTR)"OpenDevice(nvme.device,%lu) failed: %ld\n",
                   unit, (LONG)(UBYTE)open_err);
        nvmeadm_close(session);
        return FALSE;
    }

    session->device_open = TRUE;
    if (!nvmeadm_fetch_unit_info(session))
    {
        nvmeadm_close(session);
        return FALSE;
    }

    return TRUE;
}

BOOL nvmeadm_open_unit_quiet(struct nvmeadm_session *session, ULONG unit)
{
    return nvmeadm_open_unit_internal(session, unit, TRUE);
}

BOOL nvmeadm_open_unit(struct nvmeadm_session *session, ULONG unit)
{
    return nvmeadm_open_unit_internal(session, unit, FALSE);
}

void nvmeadm_close(struct nvmeadm_session *session)
{
    if (session->device_open)
        CloseDevice(session->io);
    if (session->io)
        DeleteIORequest(session->io);
    if (session->port)
        DeleteMsgPort(session->port);
}

BYTE nvmeadm_admin_passthru(struct nvmeadm_session *session,
                            struct NVMePassthruCmd *cmd,
                            ULONG *actual_out)
{
    struct IOStdReq *req = (struct IOStdReq *)session->io;

    req->io_Command = NSCMD_NVME_ADMIN_PASS;
    req->io_Data = cmd;
    req->io_Length = sizeof(*cmd);
    DoIO(session->io);

    if (actual_out)
        *actual_out = req->io_Actual;
    return (BYTE)req->io_Error;
}

void print_admin_failure(CONST_STRPTR operation, BYTE status,
                         ULONG result, ULONG actual)
{
    Printf((CONST_STRPTR)"%s failed: status=0x%02lx result=0x%08lx actual=%lu\n",
           (ULONG)operation, (ULONG)(UBYTE)status, result, actual);
}

BOOL nvmeadm_submit(struct nvmeadm_session *session,
                    struct NVMePassthruCmd *cmd,
                    CONST_STRPTR operation, ULONG *result_out)
{
    ULONG actual = 0;
    BYTE status = nvmeadm_admin_passthru(session, cmd, &actual);

    if (status != 0)
    {
        print_admin_failure(operation, status, cmd->pt_Result, actual);
        return FALSE;
    }

    if (result_out)
        *result_out = cmd->pt_Result;
    return TRUE;
}

/* For best-effort probes of optional features: no failure message, the
 * caller turns a miss into its own "not supported" output. */
BOOL nvmeadm_submit_quiet(struct nvmeadm_session *session,
                          struct NVMePassthruCmd *cmd, ULONG *result_out)
{
    if (nvmeadm_admin_passthru(session, cmd, NULL) != 0)
        return FALSE;

    if (result_out)
        *result_out = cmd->pt_Result;
    return TRUE;
}

APTR nvmeadm_alloc_clear(ULONG size)
{
    APTR ptr = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);

    if (!ptr)
        Printf((CONST_STRPTR)"alloc failed\n");
    return ptr;
}

static void nvmeadm_build_get_log(struct NVMePassthruCmd *cmd, ULONG nsid,
                                  UBYTE log_page, UBYTE lsp, UBYTE csi,
                                  APTR buffer, ULONG size)
{
    ULONG numd = nvme_bytes_to_numd(size);

    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_get_log_page;
    cmd->pt_Nsid = nsid;
    cmd->pt_Addr = buffer;
    cmd->pt_DataLen = size;
    cmd->pt_Cdw10 = (ULONG)log_page |
                    ((ULONG)lsp << 8) |
                    ((numd & 0xFFFFUL) << 16);
    cmd->pt_Cdw11 = (numd >> 16) & 0xFFFFUL;
    cmd->pt_Cdw12 = 0;
    cmd->pt_Cdw13 = 0;
    cmd->pt_Cdw14 = (ULONG)csi << 24;
    cmd->pt_Cdw15 = 0;
}

static void nvmeadm_build_identify(struct NVMePassthruCmd *cmd, ULONG nsid,
                                   ULONG cns, APTR buffer, ULONG size)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_identify;
    cmd->pt_Nsid = nsid;
    cmd->pt_Addr = buffer;
    cmd->pt_DataLen = size;
    cmd->pt_Cdw10 = cns;
}

static void nvmeadm_build_identify_csi(struct NVMePassthruCmd *cmd, ULONG nsid,
                                       UBYTE cns, UBYTE csi,
                                       APTR buffer, ULONG size)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_identify;
    cmd->pt_Nsid = nsid;
    cmd->pt_Addr = buffer;
    cmd->pt_DataLen = size;
    cmd->pt_Cdw10 = (ULONG)cns;
    cmd->pt_Cdw11 = (ULONG)csi << 24;
}

BOOL nvmeadm_fetch_get_log(struct nvmeadm_session *session,
                           CONST_STRPTR operation, ULONG nsid,
                           UBYTE log_page, UBYTE lsp, UBYTE csi,
                           APTR buffer, ULONG size)
{
    struct NVMePassthruCmd cmd;
    ULONG actual = 0;

    nvmeadm_build_get_log(&cmd, nsid, log_page, lsp, csi, buffer, size);
    BYTE status = nvmeadm_admin_passthru(session, &cmd, &actual);
    if (status != 0)
    {
        print_admin_failure(operation, status, cmd.pt_Result, actual);
        return FALSE;
    }

    return TRUE;
}

BOOL nvmeadm_fetch_identify(struct nvmeadm_session *session,
                            CONST_STRPTR operation, ULONG nsid,
                            ULONG cns, APTR buffer, ULONG size)
{
    struct NVMePassthruCmd cmd;
    ULONG actual = 0;

    nvmeadm_build_identify(&cmd, nsid, cns, buffer, size);
    BYTE status = nvmeadm_admin_passthru(session, &cmd, &actual);
    if (status != 0)
    {
        print_admin_failure(operation, status, cmd.pt_Result, actual);
        return FALSE;
    }

    return TRUE;
}

BOOL nvmeadm_fetch_identify_quiet(struct nvmeadm_session *session,
                                  ULONG nsid, ULONG cns,
                                  APTR buffer, ULONG size)
{
    struct NVMePassthruCmd cmd;

    nvmeadm_build_identify(&cmd, nsid, cns, buffer, size);
    return nvmeadm_submit_quiet(session, &cmd, NULL);
}

BOOL nvmeadm_fetch_identify_csi(struct nvmeadm_session *session,
                                ULONG nsid, UBYTE cns, UBYTE csi,
                                APTR buffer, ULONG size,
                                BYTE *status_out, ULONG *result_out)
{
    struct NVMePassthruCmd cmd;
    ULONG actual = 0;

    nvmeadm_build_identify_csi(&cmd, nsid, cns, csi, buffer, size);
    BYTE status = nvmeadm_admin_passthru(session, &cmd, &actual);

    if (status_out)
        *status_out = status;
    if (result_out)
        *result_out = cmd.pt_Result;

    return status == 0;
}

void nvmeadm_build_get_features(struct NVMePassthruCmd *cmd, ULONG nsid,
                                UBYTE fid, UBYTE sel,
                                APTR buffer, ULONG size)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_get_features;
    cmd->pt_Nsid = nsid;
    cmd->pt_Addr = buffer;
    cmd->pt_DataLen = size;
    cmd->pt_Cdw10 = (ULONG)fid | ((ULONG)sel << 8);
}

void nvmeadm_build_self_test(struct NVMePassthruCmd *cmd, ULONG nsid,
                             UBYTE stc)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_dev_self_test;
    cmd->pt_Nsid = nsid;
    cmd->pt_Cdw10 = (ULONG)stc;
}

void nvmeadm_build_download_fw(struct NVMePassthruCmd *cmd, APTR buffer,
                               ULONG size, ULONG offset_bytes)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_download_fw;
    cmd->pt_Addr = buffer;
    cmd->pt_DataLen = size;
    cmd->pt_Cdw10 = nvme_bytes_to_numd(size);
    cmd->pt_Cdw11 = offset_bytes >> 2;
}

void nvmeadm_build_format_nvm(struct NVMePassthruCmd *cmd, ULONG nsid,
                              UBYTE lbaf, UBYTE ses)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_format_nvm;
    cmd->pt_Nsid = nsid;
    cmd->pt_Cdw10 = (ULONG)lbaf | ((ULONG)ses << 9);
}

void nvmeadm_build_sanitize_nvm(struct NVMePassthruCmd *cmd, UBYTE sanact,
                                BOOL ause, UBYTE owpass,
                                BOOL oipbp, BOOL nodas)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_sanitize_nvm;
    cmd->pt_Cdw10 = (ULONG)(sanact & 0x7U) |
                    ((ULONG)(ause ? 1U : 0U) << 3) |
                    ((ULONG)(owpass & 0x0fU) << 4) |
                    ((ULONG)(oipbp ? 1U : 0U) << 8) |
                    ((ULONG)(nodas ? 1U : 0U) << 9);
}

void nvmeadm_build_activate_fw(struct NVMePassthruCmd *cmd, UBYTE slot,
                               UBYTE action)
{
    memset(cmd, 0, sizeof(*cmd));
    cmd->pt_Opcode = nvme_admin_activate_fw;
    cmd->pt_Cdw10 = (ULONG)slot | (ULONG)action;
}