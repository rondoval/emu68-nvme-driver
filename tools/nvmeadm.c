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

    if (streq(command, (CONST_STRPTR)"smart") && strempty(subcommand) && strempty(action))
    {
        rc = run_smart();
    }
    else if (streq(command, (CONST_STRPTR)"identify") && strempty(subcommand) && strempty(action))
    {
        rc = run_identify();
    }
    else if (streq(command, (CONST_STRPTR)"identify") &&
             streq(subcommand, (CONST_STRPTR)"caps") && strempty(action))
    {
        rc = run_identify_caps();
    }
    else if (streq(command, (CONST_STRPTR)"identify"))
    {
        print_command_usage((CONST_STRPTR)"identify|identify caps");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"changed-ns") && strempty(subcommand) && strempty(action))
    {
        rc = run_changed_ns();
    }
    else if (streq(command, (CONST_STRPTR)"changed-ns"))
    {
        print_command_usage((CONST_STRPTR)"changed-ns");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"error-log") && strempty(subcommand) && strempty(action))
    {
        rc = run_error_log();
    }
    else if (streq(command, (CONST_STRPTR)"error-log"))
    {
        print_command_usage((CONST_STRPTR)"error-log");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"get-feature") && strempty(subcommand) && strempty(action))
    {
        rc = run_get_feature();
    }
    else if (streq(command, (CONST_STRPTR)"get-feature"))
    {
        print_command_usage((CONST_STRPTR)"get-feature");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"self-test") && strempty(subcommand) && strempty(action))
    {
        print_command_usage((CONST_STRPTR)"self-test status|start <short|extended>|abort");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"status") &&
             strempty(action))
    {
        rc = run_self_test();
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"status"))
    {
        print_command_usage((CONST_STRPTR)"self-test status");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"start") &&
             strempty(action))
    {
        print_command_usage((CONST_STRPTR)"self-test start <short|extended>");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"start") &&
             streq(action, (CONST_STRPTR)"short"))
    {
        rc = run_self_test_control(NVMEADM_SELF_TEST_SHORT,
                                   (CONST_STRPTR)"Self-test start short");
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"start") &&
             streq(action, (CONST_STRPTR)"extended"))
    {
        rc = run_self_test_control(NVMEADM_SELF_TEST_EXTENDED,
                                   (CONST_STRPTR)"Self-test start extended");
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"abort") &&
             strempty(action))
    {
        rc = run_self_test_control(NVMEADM_SELF_TEST_ABORT,
                                   (CONST_STRPTR)"Self-test abort");
    }
    else if (streq(command, (CONST_STRPTR)"self-test") &&
             streq(subcommand, (CONST_STRPTR)"abort"))
    {
        print_command_usage((CONST_STRPTR)"self-test abort");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"self-test"))
    {
        print_command_usage((CONST_STRPTR)"self-test status|start <short|extended>|abort");
        rc = 20;
    }
    else if (streq(command, (CONST_STRPTR)"fw-log") && strempty(subcommand) && strempty(action))
    {
        rc = run_fw_log();
    }
    else if (streq(command, (CONST_STRPTR)"fw-log"))
    {
        print_command_usage((CONST_STRPTR)"fw-log");
        rc = 20;
    }
    else
    {
        Printf((CONST_STRPTR)"Unknown command: %s", (ULONG)command);
        if (!strempty(subcommand))
            Printf((CONST_STRPTR)" %s", (ULONG)subcommand);
        if (!strempty(action))
            Printf((CONST_STRPTR)" %s", (ULONG)action);
        Printf((CONST_STRPTR)"\n\n");
        print_usage();
        rc = 20;
    }

    CloseLibrary((struct Library *)DOSBase);
    return rc;
}