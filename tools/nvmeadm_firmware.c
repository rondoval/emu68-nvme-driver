// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

static void print_fw_slot_label(char *label, ULONG label_len, ULONG slot,
                                UBYTE active_slot, UBYTE next_slot)
{
    if ((UBYTE)slot == active_slot && next_slot != 0 && next_slot != active_slot)
    {
        _SNPrintf((STRPTR)label, label_len, (CONST_STRPTR)"Slot %lu [active, reset pending]:", slot);
        return;
    }

    if ((UBYTE)slot == active_slot)
    {
        _SNPrintf((STRPTR)label, label_len, (CONST_STRPTR)"Slot %lu [active]:", slot);
        return;
    }

    if ((UBYTE)slot == next_slot)
    {
        _SNPrintf((STRPTR)label, label_len, (CONST_STRPTR)"Slot %lu [next reset]:", slot);
        return;
    }

    _SNPrintf((STRPTR)label, label_len, (CONST_STRPTR)"Slot %lu:", slot);
}

static void print_fw_activate_summary(CONST_STRPTR action_name,
                                      ULONG unit, ULONG slot)
{
    Printf((CONST_STRPTR)"Activate Firmware\n");
    print_kv_u32((CONST_STRPTR)"  Unit:", unit);
    print_kv_u32((CONST_STRPTR)"  Slot:", slot);
    print_kv_str((CONST_STRPTR)"  Action:", (CONST_STRPTR)action_name);
    Printf((CONST_STRPTR)"  Opcode:               0x%02lx (Firmware Commit)\n",
           (ULONG)nvme_admin_activate_fw);
    Printf((CONST_STRPTR)"  Warning:              controller reset or delayed activation may be required\n");
}

static void print_fw_download_summary(CONST_STRPTR file_path, ULONG unit,
                                      ULONG file_size, ULONG chunk_size)
{
    ULONG chunk_count = (file_size + chunk_size - 1UL) / chunk_size;

    Printf((CONST_STRPTR)"Download Firmware\n");
    print_kv_u32((CONST_STRPTR)"  Unit:", unit);
    print_kv_str((CONST_STRPTR)"  File:", (CONST_STRPTR)file_path);
    print_kv_u32((CONST_STRPTR)"  Bytes:", file_size);
    print_kv_u32((CONST_STRPTR)"  Chunks:", chunk_count);
    print_kv_u32((CONST_STRPTR)"  Chunk Size:", chunk_size);
    Printf((CONST_STRPTR)"  Opcode:               0x%02lx (Download Firmware)\n",
           (ULONG)nvme_admin_download_fw);
    Printf((CONST_STRPTR)"  Follow-up:            choose a slot later with fw-activate\n");
}

int run_fw_log(void)
{
    struct nvme_fw_slot_info_log *log = (struct nvme_fw_slot_info_log *)nvmeadm_read_log(
        (CONST_STRPTR)"Firmware Slot log", NVME_NSID_ALL,
        NVME_LOG_FW_SLOT, 0, NVME_CSI_NVM, sizeof(*log));
    if (!log)
        return RETURN_FAIL;

    UBYTE active_slot = (UBYTE)(log->afi & 0x7U);
    UBYTE next_slot = (UBYTE)((log->afi & 0x70U) >> 4);

    Printf((CONST_STRPTR)"Firmware Slot Information\n");
    print_kv_u32((CONST_STRPTR)"Active Slot:", (ULONG)active_slot);
    if (next_slot != 0)
        print_kv_u32((CONST_STRPTR)"Next Active Slot:", (ULONG)next_slot);
    else
        print_kv_str((CONST_STRPTR)"Next Active Slot:", (CONST_STRPTR)"none");
    if (next_slot != 0 && next_slot != active_slot)
        print_kv_str((CONST_STRPTR)"Activation State:",
                     (CONST_STRPTR)"activation deferred until controller reset");
    else
        print_kv_str((CONST_STRPTR)"Activation State:", (CONST_STRPTR)"settled");

    for (ULONG slot = 0; slot < 7; slot++)
    {
        const UBYTE *rev = (const UBYTE *)&log->frs[slot];

        if (bytes_all_zero(rev, 8))
            continue;

        char label[24];
        print_fw_slot_label(label, sizeof(label), slot + 1UL, active_slot, next_slot);
        print_fw_revision((CONST_STRPTR)label, rev);
    }

    FreeMem(log, sizeof(*log));
    return RETURN_OK;
}

int run_fw_download(void)
{
    enum { FW_DOWNLOAD_CHUNK = 4096UL };

    CONST_STRPTR file_path;
    if (!nvmeadm_require_file(&file_path))
        return RETURN_ERROR;

    ULONG unit = nvmeadm_cli()->unit;

    BPTR file_handle = Open(file_path, MODE_OLDFILE);
    if (!file_handle)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"Open firmware file");
        return RETURN_FAIL;
    }

    int rc = RETURN_FAIL;
    UBYTE *buffer = NULL;
    struct nvmeadm_session session;

    if (Seek(file_handle, 0, OFFSET_END) == -1)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"Seek firmware file end");
        goto out_close_file;
    }

    LONG file_size = Seek(file_handle, 0, OFFSET_BEGINNING);
    if (file_size < 0)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"Seek firmware file start");
        goto out_close_file;
    }

    if (file_size == 0)
    {
        Printf((CONST_STRPTR)"Firmware file is empty\n");
        goto out_close_file;
    }

    if (((ULONG)file_size & 0x3UL) != 0)
    {
        Printf((CONST_STRPTR)"Firmware file size must be a multiple of 4 bytes\n");
        goto out_close_file;
    }

    buffer = (UBYTE *)nvmeadm_alloc_clear(FW_DOWNLOAD_CHUNK);
    if (!buffer)
        goto out_close_file;

    print_fw_download_summary(file_path, unit, (ULONG)file_size, FW_DOWNLOAD_CHUNK);
    if (!nvmeadm_require_confirm())
        goto out_free_buffer;

    if (!nvmeadm_open_unit(&session, unit))
        goto out_free_buffer;

    for (ULONG offset = 0; offset < (ULONG)file_size; )
    {
        /* Aborting mid-download is safe: the image only takes effect once
         * fw-activate commits it to a slot. */
        if (nvmeadm_check_break())
            goto out_close_session;

        ULONG chunk_size = (ULONG)file_size - offset;
        if (chunk_size > FW_DOWNLOAD_CHUNK)
            chunk_size = FW_DOWNLOAD_CHUNK;

        LONG read_len = Read(file_handle, buffer, (LONG)chunk_size);
        if (read_len != (LONG)chunk_size)
        {
            if (read_len == -1)
            {
                Printf((CONST_STRPTR)"Read firmware file failed at offset %lu\n",
                       offset);
                PrintFault(IoErr(), (CONST_STRPTR)"Read firmware file");
            }
            else
            {
                Printf((CONST_STRPTR)"Read firmware file failed at offset %lu (read=%lu wanted=%lu)\n",
                       offset, (ULONG)read_len, chunk_size);
            }
            goto out_close_session;
        }

        struct NVMePassthruCmd cmd;
        nvmeadm_build_download_fw(&cmd, buffer, chunk_size, offset);
        ULONG actual = 0;
        BYTE status = nvmeadm_admin_passthru(&session, &cmd, &actual);
        if (status != 0)
        {
            Printf((CONST_STRPTR)"Download Firmware failed at byte offset %lu\n", offset);
            print_admin_failure((CONST_STRPTR)"Download Firmware",
                                status, cmd.pt_Result, actual);
            goto out_close_session;
        }

        offset += chunk_size;
    }

    Printf((CONST_STRPTR)"Firmware download submitted successfully\n");
    print_kv_u32((CONST_STRPTR)"Transferred Bytes:", (ULONG)file_size);
    Printf((CONST_STRPTR)"Follow-up:              run fw-activate to commit the downloaded image to a slot\n");
    rc = RETURN_OK;

out_close_session:
    nvmeadm_close(&session);
out_free_buffer:
    FreeMem(buffer, FW_DOWNLOAD_CHUNK);
out_close_file:
    Close(file_handle);
    return rc;
}

int run_fw_activate_control(UBYTE action, CONST_STRPTR action_name)
{
    if (!nvmeadm_cli()->has_unit)
    {
        Printf((CONST_STRPTR)"fw-activate requires UNIT <n>\n");
        SetIoErr(ERROR_REQUIRED_ARG_MISSING);
        return RETURN_ERROR;
    }

    if (!nvmeadm_cli()->has_slot)
    {
        Printf((CONST_STRPTR)"fw-activate requires SLOT <n>\n");
        SetIoErr(ERROR_REQUIRED_ARG_MISSING);
        return RETURN_ERROR;
    }

    ULONG unit = nvmeadm_cli()->unit;
    ULONG slot = nvmeadm_cli()->slot;
    if (slot == 0 || slot > 7)
    {
        Printf((CONST_STRPTR)"fw-activate requires SLOT in the range 1..7\n");
        return RETURN_ERROR;
    }

    print_fw_activate_summary(action_name, unit, slot);
    if (!nvmeadm_require_confirm())
        return RETURN_FAIL;

    struct nvmeadm_session session;
    if (!nvmeadm_open_unit(&session, unit))
        return RETURN_FAIL;

    int rc = RETURN_FAIL;
    struct NVMePassthruCmd cmd;
    nvmeadm_build_activate_fw(&cmd, (UBYTE)slot, action);
    ULONG result = 0;
    if (!nvmeadm_submit(&session, &cmd, (CONST_STRPTR)"Firmware activate", &result))
        goto out_close;

    Printf((CONST_STRPTR)"Firmware activate submitted successfully\n");
    print_kv_hex32((CONST_STRPTR)"Completion result:", result);
    Printf((CONST_STRPTR)"Follow-up:               run 'nvmeadm fw-log' and reset if activation is deferred\n");
    rc = RETURN_OK;

out_close:
    nvmeadm_close(&session);
    return rc;
}
