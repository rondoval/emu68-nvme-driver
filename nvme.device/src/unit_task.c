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
#include <exec/errors.h>
#include <exec/io.h>
#include <minlist.h>

#include "device.h"
#include "nvme/nvme_ctrl.h"  /* struct NVMeController, nvme_ctrl_state */
#include "nvme/nvme_probe.h" /* nvme_reset_controller */
#include "nvme/nvme_queue.h" /* nvme_process_completions, nvme_tick_watchdog */

/* Request an asynchronous controller reset from whichever path noticed
 * the fault; the unit task consumes reset_signal in its Wait() loop. */
int nvme_reset_ctrl(struct NVMeController *ctrl)
{
    struct NVMeController *ac = ctrl ? ctrl : NULL;

    if (!ac || !ac->unit_task)
    {
        Kprintf("[nvme] reset_ctrl: no task to signal\n");
        return -1;
    }

    Signal(ac->unit_task, 1UL << ac->reset_signal);
    return 0;
}

/*
 * nvme_drain_msgport - dispatch queued I/O under I/O-queue back-pressure.
 *
 * The I/O queue holds at most ctrl->io_max_inflight commands (one under
 * NVME_QUIRK_QDEPTH_ONE); beyond that, nvme_alloc_cid would fail and the
 * client would see IOERR_UNITBUSY.  Instead we serialize: I/O-queue commands
 * that don't fit are stashed on ctrl->io_pending (FIFO) and resubmitted as
 * completions free slots.  Called at the end of every unit-task loop pass —
 * after completions are reaped — so a freed slot is immediately refilled.
 *
 * While quiesced/frozen nothing is dispatched; messages and stashed I/O wait
 * in place until nvme_unquiesce_io_queues / nvme_unfreeze re-signal us.
 */
static void nvme_drain_msgport(struct NVMeController *ctrl)
{
    if (test_bit(NVME_CTRL_STOPPED, &ctrl->flags) ||
        test_bit(NVME_CTRL_FROZEN, &ctrl->flags))
        return;

    struct List *pending = (struct List *)&ctrl->io_pending;

    /* Batch the I/O-queue SQ-tail doorbell: every ProcessCommand below that
     * reaches nvme_submit_io defers its doorbell, and the single _end commits
     * once for the whole drain pass (one doorbell instead of one per command). */
    nvme_sq_batch_begin(&ctrl->io_q);

    /* 1. Resubmit stashed I/O first (FIFO) while the queue has room. */
    while (ctrl->io_q.inflight_count < ctrl->io_max_inflight)
    {
        struct IOStdReq *io = (struct IOStdReq *)RemHead(pending);
        if (!io)
            break;
        ProcessCommand(io);
    }

    /* 2. Drain newly arrived messages.  An I/O-queue command is stashed
     * (FIFO) when the queue is full or earlier I/O is still waiting, so
     * ordering is preserved; control commands always pass straight through. */
    struct IOStdReq *io;
    while ((io = (struct IOStdReq *)GetMsg(ctrl->msgPort)))
    {
        if (io->io_Command != CMD_INTERNAL_ABORT_REQUEST &&
            (ctrl->io_q.inflight_count >= ctrl->io_max_inflight ||
             !IsListEmpty(pending)))
            AddTail(pending, (struct Node *)io);
        else
            ProcessCommand(io);
    }

    nvme_sq_batch_end(&ctrl->io_q);
}

/*
 * UnitTask - per-controller task body.
 *
 * Initialises ctrl->msgPort (the shared I/O port for all namespaces on
 * this controller) and ctrl->irq_signal, then enters the main wait loop.
 *
 * Signals parent with SIGBREAKF_CTRL_F on successful startup or
 * SIGBREAKF_CTRL_C on failure so task_spawn() can detect both cases.
 */
void UnitTask(struct NVMeController *ctrl, struct Task *parent)
{
    /* Retry queue used by nvme_retry_req for CRDT-deferred resubmits */
    _NewMinList(&ctrl->retry_list);
    /* Back-pressure FIFO for I/O the full I/O queue can't accept yet */
    _NewMinList(&ctrl->io_pending);
    ctrl->msgPort = CreateMsgPort();
    if (!ctrl->msgPort)
    {
        Kprintf("[nvme] %s: failed to create message port\n", __func__);
        goto fail;
    }

    ctrl->irq_signal = AllocSignal(-1);
    if (ctrl->irq_signal == -1)
    {
        Kprintf("[nvme] %s: failed to allocate IRQ signal\n", __func__);
        goto free_msg_port;
    }

    ctrl->reset_signal = AllocSignal(-1);
    if (ctrl->reset_signal == -1)
    {
        Kprintf("[nvme] %s: failed to allocate reset signal\n", __func__);
        goto free_irq_signal;
    }

    // Create a watchdog timer to check for command timeouts
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));
    if (!timerPort || !timerReq)
    {
        Kprintf("[nvme] %s: failed to create timer resources\n", __func__);
        goto free_timer_handles;
    }

    LONG ret = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
                          (struct IORequest *)timerReq, LIB_MIN_VERSION);
    if (ret)
    {
        Kprintf("[nvme] %s: failed to open timer.device (%ld)\n", __func__, ret);
        goto free_timer_handles;
    }

    const u32 delay = UNIT_TASK_POLL_DELAY_MS * 1000UL; /* µs */

    timerReq->tr_node.io_Command = TR_ADDREQUEST;
    timerReq->tr_time.tv_secs = 0;
    timerReq->tr_time.tv_micro = delay;
    SendIO(&timerReq->tr_node);

    ctrl->unit_task = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F); /* signal success */

    KprintfH("[nvme] %s: controller task running (bar0=%lx)\n", __func__, (ULONG)ctrl->bar0);

    ULONG waitMask = (1UL << ctrl->msgPort->mp_SigBit) |
                     (1UL << timerPort->mp_SigBit) |
                     (1UL << ctrl->irq_signal) |
                     (1UL << ctrl->reset_signal) |
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

        if (sigset & (1UL << ctrl->reset_signal))
        {
            /* Coalesce any further reset requests that arrived while
             * we were processing other signals: clear the bit so a
             * single reset cycle handles the burst. */
            SetSignal(0, 1UL << ctrl->reset_signal);
            nvme_reset_controller(ctrl);
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

        /* Dispatch queued I/O last — after completions are reaped — so a
         * just-freed I/O-queue slot is refilled the same pass.  Handles
         * new msgPort traffic and resubmits stashed (back-pressured) I/O.
         * Skipped on the stopping pass: we're about to tear down. */
        if (!(sigset & SIGBREAKF_CTRL_C))
            nvme_drain_msgport(ctrl);

        if (sigset & SIGBREAKF_CTRL_C)
        {
            Kprintf("[nvme] %s: controller task stopping\n", __func__);
            AbortIO(&timerReq->tr_node);
            WaitIO(&timerReq->tr_node);
        }

    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    /* Fail any I/O still held for back-pressure: the task is exiting and the
     * controller is being torn down, so reply rather than leak the requests.
     * (Control commands never enter io_pending, so every node here is a
     * client IOStdReq safe to ReplyMsg.) */
    {
        struct IOStdReq *pio;
        while ((pio = (struct IOStdReq *)RemHead((struct List *)&ctrl->io_pending)))
        {
            pio->io_Error = IOERR_ABORTED;
            ReplyMsg((struct Message *)pio);
        }
    }

    CloseDevice(&timerReq->tr_node);
free_timer_handles:
    if (timerReq)
        DeleteIORequest((struct IORequest *)timerReq);
    if (timerPort)
        DeleteMsgPort(timerPort);
    FreeSignal(ctrl->reset_signal);
free_irq_signal:
    FreeSignal(ctrl->irq_signal);
free_msg_port:
    DeleteMsgPort(ctrl->msgPort);
    ctrl->msgPort = NULL;
    ctrl->unit_task = NULL;
    Signal(parent, SIGBREAKF_CTRL_C);
    return;

fail:
    ctrl->unit_task = NULL;
    Signal(parent, SIGBREAKF_CTRL_C);
}

/*
 * task_spawn - allocate stack/Task/MemList and AddTask one of the
 * controller worker tasks (UnitTask, AdminWorker).
 *
 * Pushes (ctrl, parent) onto the new task's stack, AddTasks it, then
 * blocks on SIGBREAKF_CTRL_F (success) or SIGBREAKF_CTRL_C (failure)
 * from the spawned task.  The entry function is expected to clear its
 * own slot on exit so task_join can detect completion.
 *
 * Generic in shape — both UnitTask and AdminWorker share the exact
 * same launch sequence, only their entry function and task name
 * differ.
 */
s32 task_spawn(struct NVMeController *ctrl,
               task_entry entry,
               const char *name)
{
    KprintfH("[nvme] %s: starting %s\n", __func__, name);

    struct MemList *ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry),
                                  MEMF_PUBLIC | MEMF_CLEAR);
    struct Task *task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    ULONG *stack = AllocMem(STACK_SIZE, MEMF_PUBLIC | MEMF_CLEAR);

    if (!ml || !task || !stack)
    {
        Kprintf("[nvme] %s: alloc failed for %s\n", __func__, name);
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

    /* Push entry args (ctrl, parent) onto the initial stack. */
    ULONG *sp = (ULONG *)task->tc_SPUpper;
    *--sp = (ULONG)FindTask(NULL);
    *--sp = (ULONG)ctrl;
    task->tc_SPReg = sp;

    task->tc_Node.ln_Name = (char *)name;
    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = UNIT_TASK_PRIORITY;

    _NewMinList((struct MinList *)&task->tc_MemEntry);
    AddHead(&task->tc_MemEntry, &ml->ml_Node);

    SetSignal(0UL, SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);

    APTR result = AddTask(task, entry, NULL);
    if (!result)
    {
        Kprintf("[nvme] %s: AddTask(%s) failed\n", __func__, name);
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(task, sizeof(struct Task));
        FreeMem(stack, STACK_SIZE);
        return ERR_CONTROLLER_ERROR;
    }

    ULONG sig = Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
    if (sig & SIGBREAKF_CTRL_C)
    {
        Kprintf("[nvme] %s: %s failed to initialise\n", __func__, name);
        return ERR_CONTROLLER_ERROR;
    }

    return ERR_NO_ERROR;
}

/*
 * task_join - signal CTRL_C and wait for the task to clear @slot.
 *
 * @slot points to a `struct Task *` field on the controller (e.g.
 * &ctrl->unit_task or &ctrl->admin_task).  The entry function clears it on
 * exit; we poll at 250 ms via timer.device, then free our timer.
 */
void task_join(struct Task **slot)
{
    if (!slot || !*slot)
        return;

    KprintfH("[nvme] %s: stopping task=%lx\n", __func__, (ULONG)*slot);

    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *timerReq = CreateIORequest(timerPort, sizeof(struct timerequest));
    BOOL haveTimer = FALSE;

    if (timerPort && timerReq)
    {
        BYTE result = OpenDevice((CONST_STRPTR) "timer.device", UNIT_VBLANK, (struct IORequest *)timerReq, LIB_MIN_VERSION);
        if (result)
            Kprintf("[nvme] %s: Failed to open timer device: %ld\n", __func__, result);
        else
            haveTimer = TRUE;
    }

    Signal(*slot, SIGBREAKF_CTRL_C);

    while (*slot != NULL)
    {
        if (haveTimer)
        {
            timerReq->tr_node.io_Command = TR_ADDREQUEST;
            timerReq->tr_time.tv_secs = 0;
            timerReq->tr_time.tv_micro = 250000;
            DoIO(&timerReq->tr_node);
        }
    }

    SetSignal(0UL, SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);

    if (haveTimer)
        CloseDevice(&timerReq->tr_node);
    if (timerReq)
        DeleteIORequest(&timerReq->tr_node);
    if (timerPort)
        DeleteMsgPort(timerPort);
}
