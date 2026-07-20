// SPDX-License-Identifier: GPL-2.0-only
/*
 * admin_task.c — long-lived AdminWorker task.
 *
 * The unit task (ctrl->unit_task) drains the data plane: IRQ -> CQ drain,
 * IOStdReq dispatch, watchdog, controller reset.  It must NEVER call
 * nvme_submit_sync_cmd or nvme_wait_freeze — those Wait() on completions
 * that only the unit task itself can deliver, so a self-call deadlocks.
 *
 * AdminWorker is the second permanent thread per controller and hosts
 * everything that needs blocking sync admin I/O:
 *   - NVMe passthrough (NSCMD_NVME_*_PASS): freeze (if CSE), submit_sync,
 *     unfreeze, ReplyMsg.
 *   - AEN-driven namespace rescan (nvme_scan_namespaces).
 *   - Firmware activation polling (nvme_fw_act_work).
 *
 * Lifecycle: AdminTaskStart at probe (after UnitTaskStart so signals can
 * forward), AdminTaskStop at unprobe (before UnitTaskStop, see below).
 *
 * Stop order matters: stopping the unit task first leaves AdminWorker
 * blocked waiting on a completion that will never arrive.  AdminTaskStop
 * MUST run first.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <dos/dos.h> /* SIGBREAKF_CTRL_F / SIGBREAKF_CTRL_C */

#include "device.h"             /* AdminWorker proto, ProcessCommand, NVMeController */
#include "nvme/nvme_ctrl.h"     /* struct NVMeController, NVME_CTRL_* flags */
#include "nvme/nvme_fw.h"       /* nvme_fw_act_work */
#include "nvme/nvme_passthru.h" /* nvme_passthru_process */
#include "nvme/nvme_scan.h"     /* nvme_scan_namespaces */

/* AdminWorker owns scan_signal and fw_act_signal; callers only need to
 * poke the task so its Wait() loop drains the queued work. */
void nvme_queue_scan(struct NVMeController *ctrl)
{
    if (ctrl && ctrl->admin_task)
        Signal(ctrl->admin_task, 1UL << ctrl->scan_signal);
}

void nvme_queue_fw_act_work(struct NVMeController *ctrl)
{
    if (ctrl && ctrl->admin_task)
        Signal(ctrl->admin_task, 1UL << ctrl->fw_act_signal);
}

/*
 * AdminWorker - body of the per-controller admin task.
 *
 * Creates ctrl->adminPort via CreateMsgPort plus the scan + fw_act signal bits, then
 * runs a Wait() loop dispatching by signal mask.  Exits on CTRL_C from
 * task_join, clearing ctrl->admin_task so the joiner can poll.
 *
 * Spawned via task_spawn(ctrl, AdminWorker, ...) at probe time.
 */
void AdminWorker(struct NVMeController *ctrl, struct Task *parent)
{
    KprintfT("[nvme] AdminWorker: ctrl=%lx parent=%lx\n",
             (ULONG)ctrl, (ULONG)parent);

    ctrl->adminPort = CreateMsgPort();
    if (!ctrl->adminPort)
    {
        Kprintf("[nvme] %s: failed to create admin port\n", __func__);
        goto fail;
    }

    ctrl->scan_signal = AllocSignal(-1);
    if (ctrl->scan_signal == -1)
    {
        Kprintf("[nvme] %s: failed to allocate scan signal\n", __func__);
        goto free_admin_port;
    }

    ctrl->fw_act_signal = AllocSignal(-1);
    if (ctrl->fw_act_signal == -1)
    {
        Kprintf("[nvme] %s: failed to allocate fw_act signal\n", __func__);
        goto free_scan_signal;
    }

    ctrl->admin_task = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F);

    KprintfT("[nvme] %s: admin worker running (port=%lx)\n",
            __func__, (ULONG)ctrl->adminPort);

    ULONG waitMask = (1UL << ctrl->adminPort->mp_SigBit) | (1UL << ctrl->scan_signal) | (1UL << ctrl->fw_act_signal) | SIGBREAKF_CTRL_C;
    ULONG sigset;

    do
    {
        sigset = Wait(waitMask);

        if (sigset & (1UL << ctrl->scan_signal))
        {
            SetSignal(0, 1UL << ctrl->scan_signal);
            nvme_scan_namespaces(ctrl);
        }

        if (sigset & (1UL << ctrl->fw_act_signal))
        {
            SetSignal(0, 1UL << ctrl->fw_act_signal);
            nvme_fw_act_work(ctrl);
        }

        if (sigset & (1UL << ctrl->adminPort->mp_SigBit))
        {
            /* Skip passthrough dispatch while the admin queue is quiesced
             * (e.g. during controller reset).  Messages stay in the port;
             * nvme_unquiesce_admin_queue re-signals us so we drain them
             * on resume. */
            if (!test_bit(NVME_CTRL_ADMIN_Q_STOPPED, &ctrl->flags))
            {
                struct IOStdReq *io;
                while ((io = (struct IOStdReq *)GetMsg(ctrl->adminPort)))
                    nvme_passthru_process(ctrl, io);
            }
        }
    } while ((sigset & SIGBREAKF_CTRL_C) == 0);

    Kprintf("[nvme] %s: admin worker stopping\n", __func__);

    FreeSignal(ctrl->fw_act_signal);
free_scan_signal:
    FreeSignal(ctrl->scan_signal);
free_admin_port:
    DeleteMsgPort(ctrl->adminPort);
    ctrl->adminPort = NULL;
fail:
    ctrl->admin_task = NULL;
    Signal(parent, SIGBREAKF_CTRL_C);
    return;
}
