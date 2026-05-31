// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVMEADM_H
#define NVMEADM_H

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#endif

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/nvme.h>
#include <dos/dos.h>

#include <format.h>
#include <memory.h>
#include <bits.h>
#include <nvme/nvme_defs.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define NVMEADM_ARG_TEMPLATE  "COMMAND/A,SUBCOMMAND,ACTION"

struct nvmeadm_session
{
    struct MsgPort *port;
    struct IORequest *io;
    BOOL device_open;
};

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

enum
{
    NVMEADM_SELF_TEST_SHORT = 0x01,
    NVMEADM_SELF_TEST_EXTENDED = 0x02,
    NVMEADM_SELF_TEST_ABORT = 0x0f,
};

BOOL bytes_all_zero(const UBYTE *bytes, ULONG len);
UWORD read_le16(const UBYTE *bytes);
ULONG read_le32(const UBYTE *bytes);
unsigned long long read_le64(const UBYTE *bytes);
void format_le128_decimal(const UBYTE *value, char *out, ULONG out_len);
void format_u64_decimal(unsigned long long value, char *out, ULONG out_len);
void print_smart_data_amount(CONST_STRPTR label, const UBYTE *value);
CONST_STRPTR nvme_status_type_name(UWORD sct);

BOOL nvmeadm_open(struct nvmeadm_session *session);
void nvmeadm_close(struct nvmeadm_session *session);
BYTE nvmeadm_admin_passthru(struct nvmeadm_session *session,
                            struct NVMePassthruCmd *cmd,
                            ULONG *actual_out);
void print_admin_failure(CONST_STRPTR operation, BYTE status,
                         ULONG result, ULONG actual);
APTR nvmeadm_alloc_clear(ULONG size);
BOOL nvmeadm_fetch_get_log(struct nvmeadm_session *session,
                           CONST_STRPTR operation, ULONG nsid,
                           UBYTE log_page, UBYTE lsp, UBYTE csi,
                           APTR buffer, ULONG size);
BOOL nvmeadm_fetch_identify(struct nvmeadm_session *session,
                            CONST_STRPTR operation, ULONG nsid,
                            ULONG cns, APTR buffer, ULONG size);
BOOL nvmeadm_fetch_identify_csi(struct nvmeadm_session *session,
                                ULONG nsid, UBYTE cns, UBYTE csi,
                                APTR buffer, ULONG size,
                                BYTE *status_out, ULONG *result_out);
void nvmeadm_build_get_features(struct NVMePassthruCmd *cmd, ULONG nsid,
                                UBYTE fid, UBYTE sel,
                                APTR buffer, ULONG size);
void nvmeadm_build_self_test(struct NVMePassthruCmd *cmd, ULONG nsid,
                             UBYTE stc);

int run_smart(void);
int run_identify(void);
int run_identify_caps(void);
int run_fw_log(void);
int run_error_log(void);
int run_self_test(void);
int run_self_test_control(UBYTE stc, CONST_STRPTR label);
int run_get_feature(void);
int run_changed_ns(void);

#endif