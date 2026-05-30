// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvme_passthru.c — NVMe admin passthrough on the AdminWorker task.
 *
 * Userland sends a struct NVMePassthruCmd via NSCMD_NVME_ADMIN_PASS;
 * device_beginio.c PutMsgs the IOStdReq to ctrl->adminPort; AdminWorker
 * pulls it off and invokes nvme_passthru_process serially per request.
 *
 * Sequence:
 *   1. validate the user struct + opcode
 *   2. bounce the data buffer if PiStorm DMA can't reach it
 *   3. assemble the nvme_command SQE from the user's CDW fields
 *   4. nvme_passthru_start  -> freeze the I/O queue if CSE bits set
 *   5. nvme_submit_sync_cmd -> ring admin doorbell, Wait on completion
 *   6. nvme_passthru_end    -> unfreeze, fire CCC/NIC/NCC side-effects
 *   7. copy bounce buffer back (for reads), copy CQE DW0 into pt_Result
 *   8. ReplyMsg
 *
 * I/O passthrough (NSCMD_NVME_IO_PASS) is not implemented yet — would
 * need a sync-submit helper that targets the I/O queue.  Returns
 * IOERR_NOCMD for now.
 *
 * Concurrency model — important for anyone tempted to "add the missing
 * locks" matching Linux's nvme_passthru_start/end:
 *
 *   Linux protects CSE-bearing passthrough against concurrent scan_work
 *   with ctrl->scan_lock and ctrl->subsys->lock.  On Amiga the scan path
 *   (nvme_scan_namespaces) and the passthrough path (nvme_passthru_process)
 *   both run on AdminWorker — i.e. on the same task — so they are
 *   serialised by being mutually exclusive in the wait-loop dispatcher.
 *   No mutexes needed.  nvme_wait_freeze polls io_q.inflight_count which
 *   the unit task decrements via drain_cq; the counter is a single word
 *   so cross-task reads are safe.  NVME_CTRL_FROZEN is an atomic flag bit
 *   set/cleared by AdminWorker and read by the unit task's msgPort gate.
 *
 *   nvme_submit_sync_cmd is safe here because the documented calling-task
 *   invariant (see nvme_admin.c) forbids only ctrl->unit_task — and we
 *   run on AdminWorker.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/errors.h>
#include <devices/nvme.h>

#include <nvme/nvme_ctrl.h>     /* struct NVMeController, nvme_ctrl_state, nvme_state_terminal */
#include <nvme/nvme_identify.h> /* struct nvme_ns */
#include <nvme/nvme_admin.h>    /* nvme_submit_sync_cmd */
#include <nvme/nvme_io.h>       /* nvme_needs_bounce, DMA_ALIGN_MIN */
#include <nvme/nvme_passthru.h>
#include <nvme/nvme_queue.h> /* nvme_start_freeze, nvme_wait_freeze, nvme_unfreeze */
#include <nvme/nvme_scan.h>

/*
 * nvme_command_effects - return the Effects Log entry for a given opcode.
 *
 * Reads the Command Supported and Effects entry for the opcode from either
 * the I/O command set effects table (when ns != NULL) or the admin command
 * set table.  Masks out CSE bits for I/O opcodes to prevent queue freezes
 * (which would deadlock when done on an I/O command) and ignores CSE bits
 * for admin opcodes that have relaxation flags set.
 */
static u32 nvme_command_effects(struct NVMeController *ctrl, struct nvme_ns *ns, u8 opcode)
{
    u32 effects = 0;

    if (ns)
    {
        effects = le32(ns->effects->iocs[opcode]);
        if (effects & ~(u32)(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC))
            KprintfH("[nvme] IO command:%02lx has unusual effects:%08lx\n", opcode, effects);

        /* CSE bits would request an I/O-queue freeze, which would
         * self-deadlock if requested by an I/O-side command. */
        effects &= ~(u32)NVME_CMD_EFFECTS_CSE_MASK;
    }
    else
    {
        effects = le32(ctrl->effects->acs[opcode]);

        /* Honour the controller's CSER relaxation flags. */
        if (effects & NVME_CMD_EFFECTS_CSER_MASK)
            effects &= ~(u32)NVME_CMD_EFFECTS_CSE_MASK;
    }

    return effects;
}

/*
 * nvme_passthru_start - take the freeze (and only the freeze) when needed.
 *
 * If the opcode's Effects Log entry carries CSE bits, freezes the I/O
 * queue and waits for it to drain so the passthrough runs alone.
 * Returns the effects mask; pass it unchanged to nvme_passthru_end()
 * so it can mirror the unfreeze.
 */
static u32 nvme_passthru_start(struct NVMeController *ctrl, struct nvme_ns *ns, u8 opcode)
{
    u32 effects = nvme_command_effects(ctrl, ns, opcode);

    if (effects & NVME_CMD_EFFECTS_CSE_MASK)
    {
        nvme_start_freeze(ctrl);
        nvme_wait_freeze(ctrl);
    }
    return effects;
}

/*
 * nvme_passthru_end - release the freeze and dispatch effect-driven side work.
 *
 * Mirror of nvme_passthru_start's freeze.  Additionally:
 *   - CCC (controller capabilities changed): log once until reset.
 *   - NIC/NCC (namespace inventory changed): signal a rescan.  AdminWorker
 *     is the same task running this passthrough, so the scan can't start
 *     until we return — natural serialisation; no flush_work-equivalent
 *     needed.
 */
static void nvme_passthru_end(struct NVMeController *ctrl, u32 effects)
{
    if (effects & NVME_CMD_EFFECTS_CSE_MASK)
        nvme_unfreeze(ctrl);

    if (effects & NVME_CMD_EFFECTS_CCC)
    {
        if (!test_and_set_bit(NVME_CTRL_DIRTY_CAPABILITY, &ctrl->flags))
            Kprintf("[nvme] controller capabilities changed, reset may be required to take effect.\n");
    }

    if (effects & (NVME_CMD_EFFECTS_NIC | NVME_CMD_EFFECTS_NCC))
        nvme_queue_scan(ctrl);
}

static inline void reply_passthru(struct IOStdReq *io, BYTE error)
{
    io->io_Error = error;
    ReplyMsg((struct Message *)io);
}

void nvme_passthru_process(struct NVMeController *ctrl, struct IOStdReq *io)
{
    struct NVMePassthruCmd *uc = (struct NVMePassthruCmd *)io->io_Data;

    KprintfH("[nvme] passthru: cmd=0x%04lx opcode=0x%02lx (%s) nsid=%lu data_len=%lu\n",
             (ULONG)io->io_Command, (ULONG)uc->pt_Opcode,
             nvme_get_admin_opcode_str(uc->pt_Opcode),
             (ULONG)uc->pt_Nsid, (ULONG)uc->pt_DataLen);

    /* Readiness gate.  Passthrough is a USERCMD on the admin queue: only
     * safe when the controller is fully LIVE.  During CONNECTING/RESETTING
     * the admin queue is carrying init traffic that user commands must
     * not race; during DELETING/DEAD the controller is gone. */
    enum nvme_ctrl_state state = nvme_ctrl_state(ctrl);
    if (state != NVME_CTRL_LIVE)
    {
        BOOL terminal = nvme_state_terminal(ctrl);
        KprintfH("[nvme] passthru: ctrl not LIVE (state=%ld) — rejecting %s\n",
                 (LONG)state, terminal ? "terminally" : "transiently");
        reply_passthru(io, IOERR_UNITBUSY);
        return;
    }

    BOOL is_io = (io->io_Command == NSCMD_NVME_IO_PASS);

    /* I/O passthrough not yet supported — needs a sync-submit helper
     * targeting the I/O queue. */
    if (is_io)
    {
        Kprintf("[nvme] passthru: NSCMD_NVME_IO_PASS not yet supported\n");
        reply_passthru(io, IOERR_NOCMD);
        return;
    }

    /* Metadata buffers not yet supported. */
    if (uc->pt_Metadata || uc->pt_MetadataLen)
    {
        Kprintf("[nvme] passthru: metadata buffers not supported\n");
        reply_passthru(io, IOERR_BADLENGTH);
        return;
    }

    /* Hard 1 MiB cap on passthrough data buffers.  Passthrough is for
     * admin commands (Identify, Get Log Page, etc.) whose payloads are
     * always small; reject obviously-wrong sizes rather than try to
     * split them.  Not an MDTS check — admin commands don't carry data
     * the same way I/O commands do. */
    if (uc->pt_DataLen > 1024UL * 1024UL)
    {
        Kprintf("[nvme] passthru: data_len %lu exceeds 1 MiB cap\n",
                (ULONG)uc->pt_DataLen);
        reply_passthru(io, IOERR_BADLENGTH);
        return;
    }

    /* Set up data buffer / bounce. NVMe data
     * transfer direction is encoded in opcode bit 0 (1 = host -> device);
     * we don't decode it here — copy in always, copy out always.  Cheap
     * for the small admin transfers passthrough sees. */
    void *bounce = NULL;
    void *dma_buf = uc->pt_Addr;
    if (dma_buf && uc->pt_DataLen && nvme_needs_bounce(dma_buf))
    {
        bounce = dma_alloc(ctrl->memoryPool, DMA_ALIGN_MIN, uc->pt_DataLen);
        if (!bounce)
        {
            Kprintf("[nvme] passthru: bounce alloc (%lu B) failed\n",
                    (ULONG)uc->pt_DataLen);
            reply_passthru(io, IOERR_BADADDRESS);
            return;
        }
        CopyMem(uc->pt_Addr, bounce, uc->pt_DataLen);
        dma_buf = bounce;
        KprintfH("[nvme] passthru: bounce=%lx (user=%lx, %lu B)\n",
                 (ULONG)bounce, (ULONG)uc->pt_Addr, (ULONG)uc->pt_DataLen);
    }

    /* Build the SQE.  The common view of struct nvme_command covers
     * every admin opcode we care about (Identify, Get/Set Features,
     * Get Log Page, Format NVM, Sanitize, FW Commit, …). */
    struct nvme_command cmd;
    mem_zero(&cmd, sizeof(cmd));
    cmd.common.opcode = uc->pt_Opcode;
    cmd.common.flags = uc->pt_Flags;
    cmd.common.nsid = le32(uc->pt_Nsid);
    cmd.common.cdw2[0] = le32(uc->pt_Cdw2);
    cmd.common.cdw2[1] = le32(uc->pt_Cdw3);
    cmd.common.cdw10 = le32(uc->pt_Cdw10);
    cmd.common.cdw11 = le32(uc->pt_Cdw11);
    cmd.common.cdw12 = le32(uc->pt_Cdw12);
    cmd.common.cdw13 = le32(uc->pt_Cdw13);
    cmd.common.cdw14 = le32(uc->pt_Cdw14);
    cmd.common.cdw15 = le32(uc->pt_Cdw15);
    /* command_id is assigned by nvme_submit_sync_cmd from the chosen tag;
     * PRPs are built by nvme_stage_admin_prps from (dma_buf, pt_DataLen). */

    /* Freeze the I/O queue if the opcode's CSE bits demand exclusivity.
     * ns is NULL because this is an admin-only path. */
    u32 effects = nvme_passthru_start(ctrl, NULL, uc->pt_Opcode);

    /* Submit synchronously.  AdminWorker is a separate task from
     * ctrl->unit_task, so Wait-on-completion can't self-deadlock. */
    union nvme_result result;
    result.u32 = 0;
    int status = nvme_submit_sync_cmd(ctrl, &cmd, &result, dma_buf, uc->pt_DataLen);

    nvme_passthru_end(ctrl, effects);

    /* Read direction: copy bounce back into the user buffer.  We don't
     * know which direction without consulting the opcode tables, so
     * always copy back if a bounce was used. */
    if (bounce)
    {
        CopyMem(bounce, uc->pt_Addr, uc->pt_DataLen);
        dma_free(ctrl->memoryPool, bounce);
    }

    /* Report results.  pt_Result always carries the CQE DW0.  io_Error
     * is the NVMe status code (low 8 bits) on success, or
     * IOERR_BADADDRESS if submit itself failed with a negative errno. */
    uc->pt_Result = le32(result.u32);
    if (status < 0)
    {
        Kprintf("[nvme] passthru: submit_sync_cmd errno %ld\n", (LONG)status);
        reply_passthru(io, IOERR_BADADDRESS);
        return;
    }

    io->io_Actual = uc->pt_DataLen;
    KprintfH("[nvme] passthru: done status=0x%lx (%s) result=0x%08lx\n",
             (ULONG)status, nvme_get_error_status_str((u16)status), (ULONG)uc->pt_Result);
    reply_passthru(io, (BYTE)(status & 0xff));
}
