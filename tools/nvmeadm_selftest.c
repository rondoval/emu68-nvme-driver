// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

struct nvmeadm_st_result
{
    UBYTE dsts;
    UBYTE seg;
    UBYTE vdi;
    UBYTE rsvd;
    UBYTE poh[8];
    UBYTE nsid[4];
    UBYTE flba[8];
    UBYTE sct;
    UBYTE sc;
    UBYTE vs[2];
};

struct nvmeadm_self_test_log
{
    UBYTE current_operation;
    UBYTE current_completion;
    UBYTE rsvd[2];
    struct nvmeadm_st_result result[20];
};

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

int run_self_test(void)
{
    struct nvmeadm_self_test_log *log = (struct nvmeadm_self_test_log *)nvmeadm_read_log(
        (CONST_STRPTR)"Self-test log", NVME_NSID_ALL,
        NVME_LOG_DEVICE_SELF_TEST, 0, 0, sizeof(*log));
    if (!log)
        return RETURN_FAIL;

    UBYTE current_operation = (UBYTE)(log->current_operation & 0x0fU);
    UBYTE current_completion = (UBYTE)(log->current_completion & 0x7fU);

    Printf((CONST_STRPTR)"Device Self-test Log\n");
    Printf((CONST_STRPTR)"Current Operation:      0x%02lx (%s)\n",
           (ULONG)current_operation,
           (ULONG)self_test_op_name(current_operation));
    if (current_operation == 0x00)
        Printf((CONST_STRPTR)"Completion:             n/a\n");
    else
        Printf((CONST_STRPTR)"Completion:             %lu %%\n",
               (ULONG)current_completion);

    BOOL printed_any = FALSE;
    for (ULONG i = 0; i < 20; i++)
    {
        const struct nvmeadm_st_result *entry = &log->result[i];
        UBYTE result = (UBYTE)(entry->dsts & 0x0fU);
        UBYTE code = (UBYTE)((entry->dsts >> 4) & 0x0fU);

        if (result == 0x0fU || code == 0x00U ||
            bytes_all_zero((const UBYTE *)entry, sizeof(*entry)))
            continue;

        unsigned long long poh = read_le64(entry->poh);
        unsigned long long flba = read_le64(entry->flba);
        ULONG nsid = read_le32(entry->nsid);

        printed_any = TRUE;
        Printf((CONST_STRPTR)"Result %lu\n", i + 1UL);
        Printf((CONST_STRPTR)"  Self-test Code:       0x%02lx (%s)\n",
               (ULONG)code, (ULONG)self_test_code_name(code));
        Printf((CONST_STRPTR)"  Result:               0x%02lx (%s)\n",
               (ULONG)result, (ULONG)self_test_result_name(result));
        print_kv_u32((CONST_STRPTR)"  Segment:", (ULONG)entry->seg);
        print_kv_hex8((CONST_STRPTR)"  Valid Diagnostic Info:", (ULONG)entry->vdi);
        print_kv_u32((CONST_STRPTR)"  Power On Hours:", (ULONG)poh);
        print_kv_u32((CONST_STRPTR)"  NSID:", nsid);
        Printf((CONST_STRPTR)"  Failing LBA:          0x%08lx%08lx\n",
               (ULONG)(flba >> 32), (ULONG)flba);
        print_kv_hex8((CONST_STRPTR)"  Status Code Type:", (ULONG)entry->sct);
        print_kv_hex8((CONST_STRPTR)"  Status Code:", (ULONG)entry->sc);
        Printf((CONST_STRPTR)"  Vendor Specific:      0x%02lx%02lx\n",
               (ULONG)entry->vs[0], (ULONG)entry->vs[1]);
    }

    if (!printed_any)
        Printf((CONST_STRPTR)"No completed self-test results recorded.\n");

    FreeMem(log, sizeof(*log));
    return RETURN_OK;
}

int run_self_test_control(UBYTE stc, CONST_STRPTR label)
{
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct NVMePassthruCmd cmd;
    nvmeadm_build_self_test(&cmd, NVME_NSID_ALL, stc);
    ULONG result = 0;
    if (!nvmeadm_submit(&session, &cmd, label, &result))
        goto out_close;

    Printf((CONST_STRPTR)"%s submitted successfully\n", (ULONG)label);
    print_kv_hex32((CONST_STRPTR)"Completion result:", result);
    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
    return rc;
}
