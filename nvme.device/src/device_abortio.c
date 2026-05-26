// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/io.h>
#include <memory.h>
#include <debug.h>

#include "../include/device.h"

/*
 * Post an internal abort request to the controller task on behalf of the
 * caller.  The abort request is pool-allocated (from the shared controller
 * pool) so the task can free it after processing.
 */
static inline LONG post_abort_request(struct NVMeUnit *unit, struct IOStdReq *io)
{
    struct NVMeController *ctrl = unit->ctrl;
    if (!ctrl || !ctrl->memoryPool)
        return -1;

    struct IOStdReq *abort_req = pool_zalloc(ctrl->memoryPool, sizeof(*abort_req));
    if (!abort_req)
        return -1;

    abort_req->io_Message.mn_Length = sizeof(*abort_req);
    abort_req->io_Unit              = io->io_Unit;
    abort_req->io_Command           = CMD_INTERNAL_ABORT_REQUEST;
    abort_req->io_Flags             = IOF_QUICK | REQ_INTERNAL;
    abort_req->io_Data              = io; /* request to abort */

    PutMsg(ctrl->msgPort, (struct Message *)abort_req);
    return 0;
}

/*
 * abortIO - request cancellation of a queued I/O request.
 *
 * AbortIO() is a "best-effort wish": the request may already have
 * completed by the time this runs.  We post an internal abort message
 * to the controller task; the task will set IOERR_ABORTED and ReplyMsg
 * if the request is still pending.
 */
LONG abortIO(struct IOStdReq *io asm("a1"), struct NVMeDevice *base asm("a6") __attribute__((unused)))
{
    KprintfH("[nvme] %s: abort request %lx\n", __func__, (ULONG)io);
    if (!io)
        return -1;

    if (io->io_Unit != NULL)
        return post_abort_request((struct NVMeUnit *)io->io_Unit, io);

    return -1;
}
