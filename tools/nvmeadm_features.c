// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

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
        { NVME_FEAT_HOST_MEM_BUF, (CONST_STRPTR)"Host Memory Buffer:" },
        { NVME_FEAT_TIMESTAMP,   (CONST_STRPTR)"Timestamp:" },
        { NVME_FEAT_KATO,        (CONST_STRPTR)"Keep Alive Timeout:" },
        { NVME_FEAT_SANITIZE,    (CONST_STRPTR)"Sanitize Config:" },
        { NVME_FEAT_NOPSC,       (CONST_STRPTR)"Non-Operational PS:" },
    };
    struct nvmeadm_session session;
    if (!nvmeadm_open_cli_session(&session))
        return RETURN_FAIL;

    Printf((CONST_STRPTR)"Get Features (controller scope)\n");
    for (ULONG i = 0; i < (ULONG)(sizeof(features) / sizeof(features[0])); i++)
    {
        struct NVMePassthruCmd cmd;
        nvmeadm_build_get_features(&cmd, 0, features[i].fid, 0, NULL, 0);
        ULONG actual = 0;
        BYTE status = nvmeadm_admin_passthru(&session, &cmd, &actual);
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

    nvmeadm_close(&session);
    return RETURN_OK;
}
