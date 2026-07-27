// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

#include <strutil.h>

static const char verstag[] __attribute__((used)) = VERSTAG;

enum
{
    ARG_COMMAND,
    ARG_SUBCOMMAND,
    ARG_ACTION,
    ARG_UNIT,
    ARG_SLOT,
    ARG_LBAF,
    ARG_SES,
    ARG_FILE,
    ARG_AUSE,
    ARG_OWPASS,
    ARG_OIPBP,
    ARG_NODAS,
    ARG_CONFIRM,
    ARG_COUNT
};

static struct nvmeadm_cli_args cli_args;

const struct nvmeadm_cli_args *nvmeadm_cli(void)
{
    return &cli_args;
}

typedef int (*nvmeadm_handler_t)(void);
typedef int (*nvmeadm_arg_handler_t)(UBYTE arg, CONST_STRPTR label);

/*
 * Dispatch table: one row per command/subcommand/action shape.  Rows either
 * carry a plain handler or an arg_handler plus the (arg, label) pair it
 * receives — that replaces the per-action wrapper functions.
 */
struct nvmeadm_command_entry
{
    CONST_STRPTR command;
    CONST_STRPTR subcommand;
    CONST_STRPTR action;
    nvmeadm_handler_t handler;
    nvmeadm_arg_handler_t arg_handler;
    UBYTE arg;
    CONST_STRPTR arg_label;
};

/*
 * Documentation table: exactly one row per command.  This is the only place
 * usage/summary/help text lives, so the command list and the per-command
 * usage can no longer drift apart.
 */
struct nvmeadm_command_doc
{
    CONST_STRPTR command;
    CONST_STRPTR usage;
    CONST_STRPTR summary;
    CONST_STRPTR help;
};

static BOOL strempty(CONST_STRPTR text)
{
    return text == NULL || *text == '\0';
}

static BOOL token_matches(CONST_STRPTR expected, CONST_STRPTR actual)
{
    if (expected == NULL)
        return strempty(actual);
    if (actual == NULL)
        return FALSE;

    return _Stricmp(expected, actual) == 0;
}

static const struct nvmeadm_command_entry nvmeadm_commands[] = {
    { (CONST_STRPTR)"units",      NULL, NULL, run_units, NULL, 0, NULL },
    { (CONST_STRPTR)"smart",      NULL, NULL, run_smart, NULL, 0, NULL },
    { (CONST_STRPTR)"identify",   NULL, NULL, run_identify, NULL, 0, NULL },
    { (CONST_STRPTR)"identify",   (CONST_STRPTR)"caps", NULL, run_identify_caps, NULL, 0, NULL },
    { (CONST_STRPTR)"identify",   (CONST_STRPTR)"ns",   NULL, run_identify_ns, NULL, 0, NULL },
    { (CONST_STRPTR)"list-ns",    NULL, NULL, run_list_ns, NULL, 0, NULL },
    { (CONST_STRPTR)"changed-ns", NULL, NULL, run_changed_ns, NULL, 0, NULL },
    { (CONST_STRPTR)"error-log",  NULL, NULL, run_error_log, NULL, 0, NULL },
    { (CONST_STRPTR)"effects-log",NULL, NULL, run_effects_log, NULL, 0, NULL },
    { (CONST_STRPTR)"fw-log",     NULL, NULL, run_fw_log, NULL, 0, NULL },
    { (CONST_STRPTR)"fw-download",NULL, NULL, run_fw_download, NULL, 0, NULL },
    { (CONST_STRPTR)"fw-activate",(CONST_STRPTR)"replace", NULL,
      NULL, run_fw_activate_control, NVME_FWACT_REPL, (CONST_STRPTR)"replace" },
    { (CONST_STRPTR)"fw-activate",(CONST_STRPTR)"replace-activate", NULL,
      NULL, run_fw_activate_control, NVME_FWACT_REPL_ACTV, (CONST_STRPTR)"replace-activate" },
    { (CONST_STRPTR)"fw-activate",(CONST_STRPTR)"activate", NULL,
      NULL, run_fw_activate_control, NVME_FWACT_ACTV, (CONST_STRPTR)"activate" },
    { (CONST_STRPTR)"format",     NULL, NULL, run_format_nvm, NULL, 0, NULL },
    { (CONST_STRPTR)"sanitize",   (CONST_STRPTR)"exit-failure", NULL,
      NULL, run_sanitize_control, NVMEADM_SANITIZE_EXIT_FAILURE, (CONST_STRPTR)"exit-failure" },
    { (CONST_STRPTR)"sanitize",   (CONST_STRPTR)"block-erase", NULL,
      NULL, run_sanitize_control, NVMEADM_SANITIZE_BLOCK_ERASE, (CONST_STRPTR)"block-erase" },
    { (CONST_STRPTR)"sanitize",   (CONST_STRPTR)"overwrite", NULL,
      NULL, run_sanitize_control, NVMEADM_SANITIZE_OVERWRITE, (CONST_STRPTR)"overwrite" },
    { (CONST_STRPTR)"sanitize",   (CONST_STRPTR)"crypto", NULL,
      NULL, run_sanitize_control, NVMEADM_SANITIZE_CRYPTO_ERASE, (CONST_STRPTR)"crypto" },
    { (CONST_STRPTR)"sanitize-status", NULL, NULL, run_sanitize_status, NULL, 0, NULL },
    { (CONST_STRPTR)"get-feature",NULL, NULL, run_get_feature, NULL, 0, NULL },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"status", NULL, run_self_test, NULL, 0, NULL },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"start", (CONST_STRPTR)"short",
      NULL, run_self_test_control, NVMEADM_SELF_TEST_SHORT, (CONST_STRPTR)"Self-test start short" },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"start", (CONST_STRPTR)"extended",
      NULL, run_self_test_control, NVMEADM_SELF_TEST_EXTENDED, (CONST_STRPTR)"Self-test start extended" },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"abort", NULL,
      NULL, run_self_test_control, NVMEADM_SELF_TEST_ABORT, (CONST_STRPTR)"Self-test abort" },
};

static const struct nvmeadm_command_doc nvmeadm_docs[] = {
    { (CONST_STRPTR)"units",
      (CONST_STRPTR)"units",
      (CONST_STRPTR)"List units grouped by controller", NULL },
    { (CONST_STRPTR)"smart",
      (CONST_STRPTR)"smart [UNIT <n>]",
      (CONST_STRPTR)"Read SMART / health log", NULL },
    { (CONST_STRPTR)"identify",
      (CONST_STRPTR)"identify [caps|ns] [UNIT <n>]",
      (CONST_STRPTR)"Identify Controller; 'caps' decodes capabilities, 'ns' the unit's namespace", NULL },
    { (CONST_STRPTR)"list-ns",
      (CONST_STRPTR)"list-ns [UNIT <n>]",
      (CONST_STRPTR)"List active namespaces on the unit's controller", NULL },
    { (CONST_STRPTR)"changed-ns",
      (CONST_STRPTR)"changed-ns [UNIT <n>]",
      (CONST_STRPTR)"Read Changed Namespace List log", NULL },
    { (CONST_STRPTR)"error-log",
      (CONST_STRPTR)"error-log [UNIT <n>]",
      (CONST_STRPTR)"Read Error Information log", NULL },
    { (CONST_STRPTR)"effects-log",
      (CONST_STRPTR)"effects-log [UNIT <n>]",
      (CONST_STRPTR)"Read Command Effects log", NULL },
    { (CONST_STRPTR)"fw-log",
      (CONST_STRPTR)"fw-log [UNIT <n>]",
      (CONST_STRPTR)"Read Firmware Slot Information log", NULL },
    { (CONST_STRPTR)"fw-download",
      (CONST_STRPTR)"fw-download FILE <path> [UNIT <n>] CONFIRM",
      (CONST_STRPTR)"Download a firmware image", NULL },
    { (CONST_STRPTR)"fw-activate",
      (CONST_STRPTR)"fw-activate <replace|replace-activate|activate> UNIT <n> SLOT <n> CONFIRM",
      (CONST_STRPTR)"Commit downloaded firmware to a slot",
      (CONST_STRPTR)
      "Actions:\n"
      "  replace           Store the downloaded image in SLOT without activating\n"
      "  replace-activate  Store the image in SLOT and activate it on next reset\n"
      "  activate          Activate the image already stored in SLOT on next reset\n" },
    { (CONST_STRPTR)"format",
      (CONST_STRPTR)"format LBAF <n> [SES <0|1|2>] [UNIT <n>] CONFIRM",
      (CONST_STRPTR)"Format the unit's namespace to an LBA format",
      (CONST_STRPTR)
      "LBAF selects an LBA format index as listed by 'nvmeadm identify ns'.\n"
      "\n"
      "SES (Secure Erase Settings):\n"
      "  0  no secure erase (default)\n"
      "  1  user data erase - erase all user data\n"
      "  2  cryptographic erase - destroy the media encryption key\n"
      "\n"
      "CONFIRM is required. Formatting DESTROYS ALL DATA in the namespace.\n" },
    { (CONST_STRPTR)"sanitize",
      (CONST_STRPTR)"sanitize <action> [UNIT <n>] [AUSE] [OWPASS <0-15>] [OIPBP] [NODAS] CONFIRM",
      (CONST_STRPTR)"Sanitize the unit's controller (all namespaces)",
      (CONST_STRPTR)
      "Actions:\n"
      "  block-erase   Erase all blocks (typically fast; recommended)\n"
      "  crypto        Destroy the media encryption key (instant, needs crypto support)\n"
      "  overwrite     Overwrite media with a pattern (slow; see OWPASS/OIPBP)\n"
      "  exit-failure  Recover a drive stuck in sanitize-failed state\n"
      "\n"
      "Options:\n"
      "  AUSE          Allow Unrestricted Sanitize Exit - drive stays usable if\n"
      "                sanitize fails (default: restricted, drive blocks I/O)\n"
      "  OWPASS <n>    Overwrite passes, 1-15 (0 or omitted = 16 passes; overwrite only)\n"
      "  OIPBP         Invert the overwrite pattern between passes (overwrite only)\n"
      "  NODAS         No Deallocate After Sanitize (leave blocks allocated)\n"
      "  CONFIRM       Required. Sanitize DESTROYS ALL DATA on the drive.\n"
      "\n"
      "Run 'nvmeadm sanitize-status' first to see which actions the drive\n"
      "supports, and afterwards to track progress and completion.\n" },
    { (CONST_STRPTR)"sanitize-status",
      (CONST_STRPTR)"sanitize-status [UNIT <n>]",
      (CONST_STRPTR)"Read sanitize capabilities, status, and progress", NULL },
    { (CONST_STRPTR)"get-feature",
      (CONST_STRPTR)"get-feature [UNIT <n>]",
      (CONST_STRPTR)"Read selected controller feature values", NULL },
    { (CONST_STRPTR)"self-test",
      (CONST_STRPTR)"self-test status|start <short|extended>|abort [UNIT <n>]",
      (CONST_STRPTR)"Device self-test status and control", NULL },
};

static void print_usage(void)
{
    Printf((CONST_STRPTR)"nvmeadm - NVMe admin utility for nvme.device\n");
    Printf((CONST_STRPTR)"Usage: nvmeadm <command> [subcommand] [action] [options]\n");
    Printf((CONST_STRPTR)"Commands:\n");
    for (ULONG i = 0; i < (ULONG)(sizeof(nvmeadm_docs) / sizeof(nvmeadm_docs[0])); i++)
    {
        Printf((CONST_STRPTR)"  %-16s %s\n",
               (ULONG)nvmeadm_docs[i].command, (ULONG)nvmeadm_docs[i].summary);
    }
    Printf((CONST_STRPTR)"\n");
    Printf((CONST_STRPTR)"A unit is one namespace; UNIT <n> (or UNIT=<n>) also selects the\n");
    Printf((CONST_STRPTR)"controller that owns it - see 'nvmeadm units'. Commands and keywords\n");
    Printf((CONST_STRPTR)"are case-insensitive.\n");
}

static void print_command_help(const struct nvmeadm_command_doc *doc)
{
    Printf((CONST_STRPTR)"Usage: nvmeadm %s\n", (ULONG)doc->usage);
    if (doc->help)
        Printf((CONST_STRPTR)"\n%s", (ULONG)doc->help);
}

static int dispatch_command(CONST_STRPTR command, CONST_STRPTR subcommand,
                            CONST_STRPTR action)
{
    for (ULONG i = 0; i < (ULONG)(sizeof(nvmeadm_commands) / sizeof(nvmeadm_commands[0])); i++)
    {
        const struct nvmeadm_command_entry *entry = &nvmeadm_commands[i];

        if (!token_matches(entry->command, command))
            continue;
        if (!token_matches(entry->subcommand, subcommand))
            continue;
        if (!token_matches(entry->action, action))
            continue;

        return entry->arg_handler != NULL
                   ? entry->arg_handler(entry->arg, entry->arg_label)
                   : entry->handler();
    }

    /* Known command with a bad/missing subcommand or action: show its help. */
    for (ULONG i = 0; i < (ULONG)(sizeof(nvmeadm_docs) / sizeof(nvmeadm_docs[0])); i++)
    {
        if (token_matches(nvmeadm_docs[i].command, command))
        {
            print_command_help(&nvmeadm_docs[i]);
            return RETURN_ERROR;
        }
    }

    Printf((CONST_STRPTR)"Unknown command: %s", (ULONG)command);
    if (!strempty(subcommand))
        Printf((CONST_STRPTR)" %s", (ULONG)subcommand);
    if (!strempty(action))
        Printf((CONST_STRPTR)" %s", (ULONG)action);
    Printf((CONST_STRPTR)"\n\n");
    print_usage();
    return RETURN_ERROR;
}

int main(void)
{
    LONG argvals[ARG_COUNT] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)NVMEADM_ARG_TEMPLATE, argvals, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"nvmeadm");
        return RETURN_ERROR;
    }

    CONST_STRPTR command = (CONST_STRPTR)argvals[ARG_COMMAND];
    CONST_STRPTR subcommand = (CONST_STRPTR)argvals[ARG_SUBCOMMAND];
    CONST_STRPTR action = (CONST_STRPTR)argvals[ARG_ACTION];
    LONG *unit_arg = (LONG *)argvals[ARG_UNIT];
    LONG *slot_arg = (LONG *)argvals[ARG_SLOT];
    LONG *lbaf_arg = (LONG *)argvals[ARG_LBAF];
    LONG *ses_arg = (LONG *)argvals[ARG_SES];
    CONST_STRPTR file_arg = (CONST_STRPTR)argvals[ARG_FILE];
    LONG *owpass_arg = (LONG *)argvals[ARG_OWPASS];
    cli_args.has_unit = unit_arg != NULL;
    cli_args.has_slot = slot_arg != NULL;
    cli_args.has_lbaf = lbaf_arg != NULL;
    cli_args.has_ses = ses_arg != NULL;
    cli_args.has_owpass = owpass_arg != NULL;
    cli_args.unit = cli_args.has_unit ? (ULONG)*unit_arg : 0;
    cli_args.slot = cli_args.has_slot ? (ULONG)*slot_arg : 0;
    cli_args.lbaf = cli_args.has_lbaf ? (ULONG)*lbaf_arg : 0;
    cli_args.ses = cli_args.has_ses ? (ULONG)*ses_arg : 0;
    cli_args.owpass = cli_args.has_owpass ? (ULONG)*owpass_arg : 0;
    cli_args.file = (file_arg != NULL && *file_arg != '\0') ? file_arg : NULL;
    cli_args.ause = argvals[ARG_AUSE] != 0;
    cli_args.oipbp = argvals[ARG_OIPBP] != 0;
    cli_args.nodas = argvals[ARG_NODAS] != 0;
    cli_args.confirm = argvals[ARG_CONFIRM] != 0;

    int rc = dispatch_command(command, subcommand, action);

    /* The command words and FILE string live in ReadArgs' buffers, so the
     * args must stay allocated until dispatch returns. */
    FreeArgs(rda);
    return rc;
}
