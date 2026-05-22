// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/timer.h>
#endif

#include <dos/dos.h>
#include <minlist.h>
#include <debug.h>

#include <device.h>
#include <config.h>
#include <nvme/nvme.h>          /* nvme_scan_work, nvme_fw_act_work_amiga */

/*
 * UnitTask - per-controller task body.
 *
 * Initialises ctrl->msgPort (the shared I/O port for all namespaces on
 * this controller) and ctrl->irq_signal, then enters the main wait loop.
 *
 * Signals parent with SIGBREAKF_CTRL_F on successful startup or
 * SIGBREAKF_CTRL_C on failure so UnitTaskStart() can detect both cases.
 */
static void UnitTask(struct NVMeController *ctrl, struct Task *parent)
{
    /* Initialise the shared controller message port */
    _NewMinList((struct MinList *)&ctrl->msgPort.mp_MsgList);
    ctrl->msgPort.mp_SigTask = FindTask(NULL);
    BYTE msg_sigbit = AllocSignal(-1);
    if (msg_sigbit == -1)
    {
        Kprintf("[nvme] %s: failed to allocate message signal\n", __func__);
        goto fail;
    }
    ctrl->msgPort.mp_SigBit = (UBYTE)msg_sigbit;
    ctrl->msgPort.mp_Flags = PA_SIGNAL;
    ctrl->msgPort.mp_Node.ln_Type = NT_MSGPORT;

    ctrl->irq_signal = AllocSignal(-1);
    if (ctrl->irq_signal == -1)
    {
        Kprintf("[nvme] %s: failed to allocate IRQ signal\n", __func__);
        goto free_msg_signal;
    }

    // Create a watchdog timer to check for command timeouts
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));
    if (!timerPort || !timerReq)
    {
        Kprintf("[nvme] %s: failed to create timer resources\n", __func__);
        goto free_irq_signal;
    }

    LONG ret = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                          (struct IORequest *)timerReq, LIB_MIN_VERSION);
    if (ret)
    {
        Kprintf("[nvme] %s: failed to open timer.device (%ld)\n", __func__, ret);
        goto free_timer;
    }

    const u32 delay = UNIT_TASK_POLL_DELAY_MS * 1000UL; /* µs */

    timerReq->tr_node.io_Command = TR_ADDREQUEST;
    timerReq->tr_time.tv_secs = 0;
    timerReq->tr_time.tv_micro = delay;
    SendIO(&timerReq->tr_node);

    ctrl->task = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F); /* signal success */

    Kprintf("[nvme] %s: controller task running (bar0=%lx)\n", __func__, (ULONG)ctrl->bar0);

    ULONG waitMask = (1UL << ctrl->msgPort.mp_SigBit) |
                     (1UL << timerPort->mp_SigBit) |
                     (1UL << ctrl->irq_signal) |
                     SIGBREAKF_CTRL_C;
    ULONG sigset;

    do
    {
        sigset = Wait(waitMask);

        if (sigset & (1UL << ctrl->irq_signal))
        {
            nvme_process_completions(ctrl);
            nvme_int_rearm(ctrl);
        }

        if (sigset & (1UL << ctrl->msgPort.mp_SigBit))
        {
            struct IOStdReq *io;
            while ((io = (struct IOStdReq *)GetMsg(&ctrl->msgPort)))
                ProcessCommand(io);
        }

        if (sigset & (1UL << timerPort->mp_SigBit))
        {
            if (CheckIO(&timerReq->tr_node))
                WaitIO(&timerReq->tr_node);

            nvme_tick_watchdog(ctrl);

            timerReq->tr_node.io_Command = TR_ADDREQUEST;
            timerReq->tr_time.tv_secs = 0;
            timerReq->tr_time.tv_micro = delay;
            SendIO(&timerReq->tr_node);
        }

        if (sigset & SIGBREAKF_CTRL_C)
        {
            Kprintf("[nvme] %s: controller task stopping\n", __func__);
            AbortIO(&timerReq->tr_node);
            WaitIO(&timerReq->tr_node);
        }

    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    CloseDevice(&timerReq->tr_node);
free_timer:
    DeleteIORequest((struct IORequest *)timerReq);
    DeleteMsgPort(timerPort);
free_irq_signal:
    FreeSignal(ctrl->irq_signal);
free_msg_signal:
    FreeSignal((BYTE)ctrl->msgPort.mp_SigBit);
    ctrl->task = NULL;
    Signal(parent, SIGBREAKF_CTRL_C);
    return;

fail:
    ctrl->task = NULL;
    Signal(parent, SIGBREAKF_CTRL_C);
}

/*
 * UnitTaskStart - allocate stack/task and start the controller task.
 *
 * Waits for SIGBREAKF_CTRL_F (success) or SIGBREAKF_CTRL_C (failure)
 * from the new task before returning.
 */
s32 UnitTaskStart(struct NVMeController *ctrl)
{
    Kprintf("[nvme] %s: starting controller task\n", __func__);

    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry), MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);

    if (!ml || !task || !stack)
    {
        Kprintf("[nvme] %s: failed to allocate task resources\n", __func__);
        if (ml)
            FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        if (task)
            FreeMem(task, sizeof(struct Task));
        if (stack)
            FreeMem(stack, STACK_SIZE);
        return ERR_ALLOC_ERROR;
    }

    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Un.meu_Addr = task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);
    ml->ml_ME[1].me_Un.meu_Addr = stack;
    ml->ml_ME[1].me_Length = STACK_SIZE;

    task->tc_SPLower = stack;
    task->tc_SPUpper = &stack[STACK_SIZE / sizeof(ULONG)];

    /* Push UnitTask arguments (ctrl, parent) onto the initial stack */
    ULONG *sp = (ULONG *)task->tc_SPUpper;
    *--sp = (ULONG)FindTask(NULL); /* parent */
    *--sp = (ULONG)ctrl;
    task->tc_SPReg = sp;

    task->tc_Node.ln_Name = "NVMe storage driver";
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = UNIT_TASK_PRIORITY;

    _NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    SetSignal(0UL, SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);

    APTR result = AddTask(task, UnitTask, NULL);
    if (!result)
    {
        Kprintf("[nvme] %s: AddTask failed\n", __func__);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
        FreeMem(stack, STACK_SIZE);
        return ERR_CONTROLLER_ERROR;
    }

    ULONG sig = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
    if (sig & SIGBREAKF_CTRL_C)
    {
        Kprintf("[nvme] %s: controller task failed to initialise\n", __func__);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
        FreeMem(stack, STACK_SIZE);
        return ERR_CONTROLLER_ERROR;
    }

    Kprintf("[nvme] %s: controller task started\n", __func__);
    return ERR_NO_ERROR;
}

/*
 * UnitTaskStop - signal the controller task to exit and wait until it does.
 */
void UnitTaskStop(struct NVMeController *ctrl)
{
    if (!ctrl->task)
        return;

    Kprintf("[nvme] %s: stopping controller task\n", __func__);

    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));

    if (timerPort && timerReq)
    {
        BYTE result = OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK, (struct IORequest *)timerReq, LIB_MIN_VERSION);
        if (result != NULL)
        {
            Kprintf("[nvme] %s: Failed to open timer device: %ld\n", __func__, result);
            // We'll continue anyway
        }
    }

    Signal(ctrl->task, SIGBREAKF_CTRL_C);

    /* Poll until the task clears ctrl->task */
    while (ctrl->task != NULL)
    {
        if (timerReq && timerPort)
        {
            timerReq->tr_node.io_Command = TR_ADDREQUEST;
            timerReq->tr_time.tv_secs = 0;
            timerReq->tr_time.tv_micro = 250000;
            DoIO(&timerReq->tr_node);
        }
    }

    SetSignal(0UL, SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);

    if (timerReq)
    {
        CloseDevice(&timerReq->tr_node);
        DeleteIORequest(&timerReq->tr_node);
    }
    if (timerPort)
        DeleteMsgPort(timerPort);

    Kprintf("[nvme] %s: controller task stopped\n", __func__);
}
