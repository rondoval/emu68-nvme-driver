// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_COMPLETION_H
#define NVME_COMPLETION_H

#include <nvme/nvme_core.h>     /* nvme_defs.h: struct nvme_command, union nvme_result */

struct NVMeController;
struct NVMeUnit;
struct nvme_queue;
struct IOStdReq;

/*
 * Parent struct for a single Amiga BeginIO whose payload is larger than
 * the controller MDTS and is therefore split across multiple sibling
 * nvme_request commands.  Allocated from ctrl->ctx_slab by
 * nvme_io_submit_rw alongside the first child(ren); freed by
 * nvme_complete_rq once @inflight drops to zero and the originating
 * IOStdReq has been ReplyMsg'd.
 *
 * Single-shot I/Os (size <= MDTS) and admin commands do NOT allocate a
 * context — req->ctx stays NULL, and the existing single-request
 * completion path runs unchanged.
 */
struct nvme_io_context
{
    struct MinNode stall_node; /* MUST be first: ctrl->ctx_stalled link while
                                * parked under tag/SQ pressure with no sibling
                                * in flight (see nvme_io_context_pump) */
    struct IOStdReq *io;   /* originating Amiga request    */
    struct NVMeUnit *unit; /* namespace unit               */
    u64 start_lba;         /* LBA of byte 0 of the I/O     */
    u32 total_bytes;       /* == io->io_Length             */
    u32 dispatched;        /* bytes whose child has been submitted */
    u32 completed;         /* bytes whose CQE has arrived  */
    u16 inflight;          /* siblings still without CQE   */
    BYTE first_error;      /* AmigaOS error from first failed sibling */
    u8 opcode;             /* nvme_cmd_read or nvme_cmd_write */
    u8 data_precached;     /* whole user buffer was cache-prepared once in
                            * nvme_io_submit_rw (direct, non-bounced transfer),
                            * so siblings skip the per-chunk data flush and the
                            * post-DMA invalidate is done once in _finish. */
    BOOL stalled;            /* parked on ctrl->ctx_stalled; the unit task
                            * re-pumps as completions free slots and the
                            * completion path must not _finish a parked ctx */
    APTR user_data;        /* == io->io_Data, base for chunk slicing */
};

enum nvme_req_flags
{
    NVME_REQ_CANCELLED = (1 << 0),
    NVME_REQ_USERCMD = (1 << 1),    /* admin passthrough — selects the
                                     * extended logger in nvme_log_error
                                     * via nvme_req_is_passthrough */
    NVME_REQ_PRP_LIST = (1 << 2),   /* result.u64 holds a PRP-list
                                     * page address to FreeMem on
                                     * completion (see build_prps) */
    NVME_REQ_ABORT_SENT = (1 << 3), /* watchdog has issued an Abort
                                     * for this request; abort_us
                                     * is the start of the grace
                                     * timer before reset escalation */
    NVME_REQ_AER = (1 << 4),        /* outstanding Asynchronous Event
                                     * Request; sits in admin
                                     * inflight[] indefinitely.  The
                                     * watchdog must skip it. */
    NVME_REQ_PRP_SLAB = (1 << 5),   /* prp_pages[] are slab-backed
                                     * (free with slab_free).  Set
                                     * alongside NVME_REQ_PRP_LIST by
                                     * build_prps.  When clear, the
                                     * prp_pages[] were dma_alloc'd
                                     * by the caller (e.g. DSM range
                                     * buffer in nvme_setup_dsm) and
                                     * must use dma_free. */
    NVME_REQ_PRP_SMALL = (1 << 6),  /* slab pages came from prp_small_slab
                                     * (256 B) rather than prp_large_slab
                                     * (4 KiB).  A request's list is always
                                     * all-small (a single ≤32-entry page) or
                                     * all-large, never mixed, so this one bit
                                     * selects the slab for every prp_pages[]
                                     * entry in nvme_req_free_dma_buffers. */
};

/*
 * In-flight NVMe command descriptor.  One per outstanding SQ entry.
 * Allocated from ctrl->req_slab.
 *
 * cid is the encoded command_id (gen<<12 | tag); its low 12 bits are the
 * per-queue slot index (0..req->q->depth-1) and req->q->inflight[tag] points
 * back at this request until the CQE is drained.  CIDs overlap between the
 * admin and I/O queues — the queue pointer is the disambiguator.
 *
 * Admin commands: set io=NULL, waiter=FindTask(NULL),
 * wait_signal=AllocSignal(-1), then Wait(1UL<<wait_signal) after submit.
 */
struct nvme_request
{
    struct MinNode node;       /* linkage in a pending MinList         */
    struct IOStdReq *io;       /* originating Amiga request; NULL=admin */
    struct NVMeUnit *unit;     /* namespace unit; NULL for admin cmds  */
    struct NVMeController *ac; /* owning controller (always set, even
                                * for admin where unit==NULL)         */
    struct nvme_queue *q;      /* queue this request lives on; set by
                                * nvme_req_alloc_io / _alloc_admin    */

    /* Completion fields — written from CQE by nvme_process_completions() */
    union nvme_result result; /* CQE DW0                              */
    u16 status;               /* CQE status field (bits 14:1)         */
    u16 cid;                  /* encoded command_id = gen<<12 | tag;
                               * slot index = cid & 0x0fff (nvme_req_tag) */
    u8 retries;
    enum nvme_req_flags flags;

    /* timing
     *   submit_us:   set by nvme_submit_io just before doorbell write;
     *                watchdog tick compares (get_time() - submit_us)
     *                against NVME_*_TIMEOUT.
     *   deadline_us: set by nvme_retry_req when CRDT > 0; the request
     *                is parked on NVMeController.retry_list until the
     *                watchdog tick sees deadline reached.
     *   abort_us:    set by watchdog_scan_queue when it issues an
     *                Abort admin cmd for this in-flight request.  The
     *                next tick compares (get_time() - abort_us)
     *                against NVME_ABORT_TIMEOUT and signals a
     *                controller reset if the abort itself stalls.
     *                Only meaningful while NVME_REQ_ABORT_SENT is
     *                set. */
    u32 submit_us;
    u32 deadline_us;
    u32 abort_us;

    /* PRP list pages.
     *   prp_pages[0..n-1] hold AllocMem'd page bases used to back PRP
     *   entries for multi-page transfers.  nvme_cleanup_cmd FreeMems
     *   each on completion.  prp_page_count == 0 for transfers that
     *   fit in PRP1+PRP2 alone. */
    APTR prp_pages[8]; /* up to 8 chained list pages */
    u8 prp_page_count;

    /* Synchronous admin completion — unused for data I/O (set both to 0) */
    struct Task *waiter; /* Signal() this task on completion     */
    BYTE wait_signal;    /* signal bit number (from AllocSignal) */

    /* Asynchronous admin completion.  Mutually exclusive with .waiter
     * and .io: if set, nvme_end_req invokes done(req) and the callback
     * OWNS the pool_free of req (plus any buffer it allocated for the
     * transfer).  Used by unit-task contexts (scan_work, fw_act_work,
     * nvme_io_abort) that cannot block — see nvme_submit_async_cmd
     * in nvme_io.h for the calling-task rule.
     *
     * priv is opaque context the submitter wants to recover in done()
     * (typically a pointer to the data buffer that needs to be freed). */
    void (*done)(struct nvme_request *req);
    void *priv;

    /* Chunked-I/O linkage.  When @ctx is non-NULL this request is one
     * of several sibling chunks of a larger BeginIO; the parent context
     * owns the IOStdReq, total length, error latch, and the dispatch
     * cursor.  When @ctx is NULL this is a single-shot command (admin /
     * flush / DSM / small I/O) and req->io is the originating Amiga
     * request directly.  See nvme_io_context above. */
    struct nvme_io_context *ctx;

    /* Bounce-buffer tracking for the data payload of this chunk.
     * PCIe DMA on PiStorm cannot reach Amiga Chip RAM (and unaligned
     * Fast-RAM buffers), so nvme_setup_rw transparently bounces such
     *  buffers through a Fast-RAM scratch.
     *
     *   user_buf   — caller's buffer for THIS chunk (the slice being
     *                transferred right now).  NULL on admin / flush /
     *                DSM commands.
     *   user_len   — bytes for THIS chunk (full I/O size on single-shot
     *                submissions; per-sibling slice on chunked I/O).
     *   bounce_buf — non-NULL when we allocated a Fast-RAM bounce.
     *                Write path memcpy'd user->bounce in setup_rw;
     *                read path memcpys bounce->user in cleanup_cmd. */
    APTR user_buf;
    u32 user_len;
    APTR bounce_buf;

    /* Embedded SQE — avoids a second pool allocation */
    struct nvme_command cmd;
};

/* Flag inspectors */
static inline BOOL nvme_req_noretry(const struct nvme_request *r)
{
    return (r->flags & NVME_REQ_CANCELLED) != 0;
}

static inline BOOL nvme_req_is_passthrough(const struct nvme_request *r)
{
    return (r->flags & NVME_REQ_USERCMD) != 0;
}

/* ------------------------------------------------------------------ */
/* Public request-lifecycle API                                        */
/* ------------------------------------------------------------------ */

/*
 * nvme_complete_rq - main completion entry point.  Called from the queue
 * drain (drain_cq) and from the cancel walker.  Drives retry / chunk
 * continuation / DMA cleanup / end-of-request reply.
 */
void nvme_complete_rq(struct nvme_request *req);

#endif /* NVME_COMPLETION_H */
