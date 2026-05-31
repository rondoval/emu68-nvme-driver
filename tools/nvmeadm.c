// SPDX-License-Identifier: GPL-2.0-only
#include "nvmeadm.h"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;

enum
{
    ARG_COMMAND,
    ARG_SUBCOMMAND,
    ARG_ACTION,
    ARG_COUNT
};

typedef int (*nvmeadm_handler_t)(void);

struct nvmeadm_command_entry
{
    CONST_STRPTR command;
    CONST_STRPTR subcommand;
    CONST_STRPTR action;
    nvmeadm_handler_t handler;
};

struct nvmeadm_usage_entry
{
    CONST_STRPTR command;
    CONST_STRPTR usage;
};

static BOOL streq(CONST_STRPTR lhs, CONST_STRPTR rhs)
{
    if (!lhs || !rhs)
        return FALSE;

    while (*lhs != '\0' && *rhs != '\0')
    {
        if (*lhs != *rhs)
            return FALSE;
        lhs++;
        rhs++;
    }

    return *lhs == *rhs;
}

static BOOL strempty(CONST_STRPTR text)
{
    return text == NULL || *text == '\0';
}

static void print_command_usage(CONST_STRPTR usage)
{
    Printf((CONST_STRPTR)"Usage: nvmeadm %s\n", (ULONG)usage);
}

static void print_usage(void)
{
    Printf((CONST_STRPTR)"Usage: nvmeadm <command> [subcommand] [action]\n");
    Printf((CONST_STRPTR)"Commands:\n");
    Printf((CONST_STRPTR)"  smart                    Read NVMe SMART / health log\n");
    Printf((CONST_STRPTR)"  identify                 Read NVMe Identify Controller data\n");
    Printf((CONST_STRPTR)"  identify caps            Read decoded NVMe controller capabilities\n");
    Printf((CONST_STRPTR)"  error-log                Read NVMe Error Information log\n");
    Printf((CONST_STRPTR)"  fw-log                   Read NVMe Firmware Slot Information log\n");
    Printf((CONST_STRPTR)"  get-feature              Read selected NVMe controller feature values\n");
    Printf((CONST_STRPTR)"  changed-ns               Read NVMe Changed Namespace List log\n");
    Printf((CONST_STRPTR)"  self-test status         Read NVMe Device Self-test status log\n");
    Printf((CONST_STRPTR)"  self-test start short    Start a short device self-test\n");
    Printf((CONST_STRPTR)"  self-test start extended Start an extended device self-test\n");
    Printf((CONST_STRPTR)"  self-test abort          Abort an in-progress self-test\n");
    Printf((CONST_STRPTR)"\n");
}

static int run_self_test_start_short_wrapper(void)
{
    return run_self_test_control(NVMEADM_SELF_TEST_SHORT,
                                 (CONST_STRPTR)"Self-test start short");
}

static int run_self_test_start_extended_wrapper(void)
{
    return run_self_test_control(NVMEADM_SELF_TEST_EXTENDED,
                                 (CONST_STRPTR)"Self-test start extended");
}

static int run_self_test_abort_wrapper(void)
{
    return run_self_test_control(NVMEADM_SELF_TEST_ABORT,
                                 (CONST_STRPTR)"Self-test abort");
}

static const struct nvmeadm_command_entry nvmeadm_commands[] = {
    { (CONST_STRPTR)"smart",      NULL,                     NULL,                     run_smart },
    { (CONST_STRPTR)"identify",   NULL,                     NULL,                     run_identify },
    { (CONST_STRPTR)"identify",   (CONST_STRPTR)"caps",   NULL,                     run_identify_caps },
    { (CONST_STRPTR)"changed-ns", NULL,                     NULL,                     run_changed_ns },
    { (CONST_STRPTR)"error-log",  NULL,                     NULL,                     run_error_log },
    { (CONST_STRPTR)"get-feature",NULL,                     NULL,                     run_get_feature },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"status", NULL,                     run_self_test },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"start",  (CONST_STRPTR)"short",  run_self_test_start_short_wrapper },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"start",  (CONST_STRPTR)"extended", run_self_test_start_extended_wrapper },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"abort",  NULL,                     run_self_test_abort_wrapper },
    { (CONST_STRPTR)"fw-log",     NULL,                     NULL,                     run_fw_log },
};

static const struct nvmeadm_usage_entry nvmeadm_usage_entries[] = {
    { (CONST_STRPTR)"identify",   (CONST_STRPTR)"identify|identify caps" },
    { (CONST_STRPTR)"changed-ns", (CONST_STRPTR)"changed-ns" },
    { (CONST_STRPTR)"error-log",  (CONST_STRPTR)"error-log" },
    { (CONST_STRPTR)"get-feature",(CONST_STRPTR)"get-feature" },
    { (CONST_STRPTR)"self-test",  (CONST_STRPTR)"self-test status|start <short|extended>|abort" },
    { (CONST_STRPTR)"fw-log",     (CONST_STRPTR)"fw-log" },
};

static BOOL token_matches(CONST_STRPTR expected, CONST_STRPTR actual)
{
    if (expected == NULL)
        return strempty(actual);

    return streq(expected, actual);
}

static int dispatch_command(CONST_STRPTR command, CONST_STRPTR subcommand,
                            CONST_STRPTR action)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(nvmeadm_commands) / sizeof(nvmeadm_commands[0])); i++)
    {
        const struct nvmeadm_command_entry *entry = &nvmeadm_commands[i];

        if (!streq(command, entry->command))
            continue;
        if (!token_matches(entry->subcommand, subcommand))
            continue;
        if (!token_matches(entry->action, action))
            continue;

        return entry->handler();
    }

    for (i = 0; i < (ULONG)(sizeof(nvmeadm_usage_entries) / sizeof(nvmeadm_usage_entries[0])); i++)
    {
        const struct nvmeadm_usage_entry *entry = &nvmeadm_usage_entries[i];

        if (streq(command, entry->command))
        {
            print_command_usage(entry->usage);
            return 20;
        }
    }

    Printf((CONST_STRPTR)"Unknown command: %s", (ULONG)command);
    if (!strempty(subcommand))
        Printf((CONST_STRPTR)" %s", (ULONG)subcommand);
    if (!strempty(action))
        Printf((CONST_STRPTR)" %s", (ULONG)action);
    Printf((CONST_STRPTR)"\n\n");
    print_usage();
    return 20;
}

int main(void)
{
    LONG argvals[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    CONST_STRPTR command;
    CONST_STRPTR subcommand;
    CONST_STRPTR action;
    int rc = 20;

    SysBase = *(struct ExecBase **)4UL;

    DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 0);
    if (!DOSBase)
        return 50;

    rda = ReadArgs((CONST_STRPTR)NVMEADM_ARG_TEMPLATE, argvals, NULL);
    if (!rda)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"nvmeadm");
        CloseLibrary((struct Library *)DOSBase);
        return 20;
    }

    command = (CONST_STRPTR)argvals[ARG_COMMAND];
    subcommand = (CONST_STRPTR)argvals[ARG_SUBCOMMAND];
    action = (CONST_STRPTR)argvals[ARG_ACTION];
    FreeArgs(rda);

    rc = dispatch_command(command, subcommand, action);

    CloseLibrary((struct Library *)DOSBase);
    return rc;
}