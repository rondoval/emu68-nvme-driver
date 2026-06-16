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
#include <devices/trackdisk.h>
#include <dos/dos.h>

#include <format.h>
#include <memory.h>
#include <bits.h>
#include <nvme/nvme_defs.h>

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

#define NVMEADM_ARG_TEMPLATE  "COMMAND/A,SUBCOMMAND,ACTION,UNIT/N/K,SLOT/N/K,LBAF/N/K,SES/N/K,FILE/K,AUSE/S,OWPASS/N/K,OIPBP/S,NODAS/S,CONFIRM/S"

struct nvmeadm_session
{
    struct MsgPort *port;
    struct IORequest *io;
    BOOL device_open;
    struct NVMeUnitInfo info;   /* filled by NSCMD_NVME_UNIT_INFO at open */
};

enum
{
    NVMEADM_SELF_TEST_SHORT = 0x01,
    NVMEADM_SELF_TEST_EXTENDED = 0x02,
    NVMEADM_SELF_TEST_ABORT = 0x0f,
    NVMEADM_SANITIZE_EXIT_FAILURE = 0x01,
    NVMEADM_SANITIZE_BLOCK_ERASE = 0x02,
    NVMEADM_SANITIZE_OVERWRITE = 0x03,
    NVMEADM_SANITIZE_CRYPTO_ERASE = 0x04,
};

BOOL bytes_all_zero(const UBYTE *bytes, ULONG len);
UWORD read_le16(const UBYTE *bytes);
ULONG read_le32(const UBYTE *bytes);
unsigned long long read_le64(const UBYTE *bytes);
void format_le128_decimal(const UBYTE *value, char *out, ULONG out_len);
void format_u64_decimal(unsigned long long value, char *out, ULONG out_len);
void print_smart_data_amount(CONST_STRPTR label, const UBYTE *value);
void print_binary_size(CONST_STRPTR label, unsigned long long bytes);
CONST_STRPTR nvme_status_type_name(UWORD sct);

/* Parsed command-line arguments; values are 0/NULL when absent and the
 * has_* flags say whether the keyword was given at all. */
struct nvmeadm_cli_args
{
    ULONG unit;
    ULONG slot;
    ULONG lbaf;
    ULONG ses;
    ULONG owpass;
    CONST_STRPTR file;          /* NULL when not given */
    BOOL has_unit;
    BOOL has_slot;
    BOOL has_lbaf;
    BOOL has_ses;
    BOOL has_owpass;
    BOOL ause;
    BOOL oipbp;
    BOOL nodas;
    BOOL confirm;
};

const struct nvmeadm_cli_args *nvmeadm_cli(void);

BOOL nvmeadm_open_unit(struct nvmeadm_session *session, ULONG unit);
BOOL nvmeadm_open_unit_quiet(struct nvmeadm_session *session, ULONG unit);
void nvmeadm_close(struct nvmeadm_session *session);
BYTE nvmeadm_admin_passthru(struct nvmeadm_session *session,
                            struct NVMePassthruCmd *cmd,
                            ULONG *actual_out);
void print_admin_failure(CONST_STRPTR operation, BYTE status,
                         ULONG result, ULONG actual);
BOOL nvmeadm_submit(struct nvmeadm_session *session,
                    struct NVMePassthruCmd *cmd,
                    CONST_STRPTR operation, ULONG *result_out);
BOOL nvmeadm_submit_quiet(struct nvmeadm_session *session,
                          struct NVMePassthruCmd *cmd, ULONG *result_out);
APTR nvmeadm_alloc_clear(ULONG size);
BOOL nvmeadm_fetch_get_log(struct nvmeadm_session *session,
                           CONST_STRPTR operation, ULONG nsid,
                           UBYTE log_page, UBYTE lsp, UBYTE csi,
                           APTR buffer, ULONG size);
BOOL nvmeadm_fetch_identify(struct nvmeadm_session *session,
                            CONST_STRPTR operation, ULONG nsid,
                            ULONG cns, APTR buffer, ULONG size);
BOOL nvmeadm_fetch_identify_quiet(struct nvmeadm_session *session,
                                  ULONG nsid, ULONG cns,
                                  APTR buffer, ULONG size);
BOOL nvmeadm_fetch_identify_csi(struct nvmeadm_session *session,
                                ULONG nsid, UBYTE cns, UBYTE csi,
                                APTR buffer, ULONG size,
                                BYTE *status_out, ULONG *result_out);
void nvmeadm_build_get_features(struct NVMePassthruCmd *cmd, ULONG nsid,
                                UBYTE fid, UBYTE sel,
                                APTR buffer, ULONG size);
void nvmeadm_build_self_test(struct NVMePassthruCmd *cmd, ULONG nsid,
                             UBYTE stc);
void nvmeadm_build_download_fw(struct NVMePassthruCmd *cmd, APTR buffer,
                              ULONG size, ULONG offset_bytes);
void nvmeadm_build_format_nvm(struct NVMePassthruCmd *cmd, ULONG nsid,
                              UBYTE lbaf, UBYTE ses);
void nvmeadm_build_sanitize_nvm(struct NVMePassthruCmd *cmd, UBYTE sanact,
                                BOOL ause, UBYTE owpass,
                                BOOL oipbp, BOOL nodas);
void nvmeadm_build_activate_fw(struct NVMePassthruCmd *cmd, UBYTE slot,
                               UBYTE action);

/* nvmeadm_util.c: cross-domain print and CLI-gate helpers */
BOOL nvmeadm_open_cli_session(struct nvmeadm_session *session);
BOOL nvmeadm_require_file(CONST_STRPTR *file_out);
BOOL nvmeadm_require_lbaf(ULONG *lbaf_out);
BOOL nvmeadm_require_confirm(void);
BOOL nvmeadm_check_break(void);
APTR nvmeadm_read_log(CONST_STRPTR operation, ULONG nsid,
                      UBYTE log_page, UBYTE lsp, UBYTE csi, ULONG size);
APTR nvmeadm_read_identify(CONST_STRPTR operation, ULONG nsid,
                           ULONG cns, ULONG size);
void print_yes_no(CONST_STRPTR label, BOOL enabled);
void print_kv_str(CONST_STRPTR label, CONST_STRPTR value);
void print_kv_u32(CONST_STRPTR label, ULONG value);
void print_kv_hex8(CONST_STRPTR label, ULONG value);
void print_kv_hex16(CONST_STRPTR label, ULONG value);
void print_kv_hex32(CONST_STRPTR label, ULONG value);
void print_fw_revision(CONST_STRPTR label, const UBYTE *rev);
void print_feature_value(CONST_STRPTR label, UBYTE fid, ULONG value);
ULONG lbaf_data_size_bytes(const struct nvme_lbaf *lbaf);
BOOL namespace_plain_block_supported(const struct nvme_id_ns *id,
                                     ULONG sector_size, UWORD metadata_size);
BOOL candidate_plain_block_supported(ULONG sector_size, UWORD metadata_size);

int run_smart(void);
int run_units(void);
int run_identify(void);
int run_identify_caps(void);
int run_identify_ns(void);
int run_list_ns(void);
int run_effects_log(void);
int run_sanitize_status(void);
int run_fw_log(void);
int run_error_log(void);
int run_self_test(void);
int run_self_test_control(UBYTE stc, CONST_STRPTR label);
int run_fw_download(void);
int run_format_nvm(void);
int run_sanitize_control(UBYTE sanact, CONST_STRPTR action_name);
int run_fw_activate_control(UBYTE action, CONST_STRPTR action_name);
int run_get_feature(void);
int run_changed_ns(void);

#endif