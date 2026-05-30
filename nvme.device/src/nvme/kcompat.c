// SPDX-License-Identifier: GPL-2.0
/*
 * Linux-compat helpers that need concrete freestanding definitions.
 *
 * kcompat.h provides most shims inline or as macros. This file holds
 * the small set of helpers that need a real out-of-line definition.
 */
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#include <proto/timer.h>
#endif

#include <devices/timer.h>

#include <device.h>
#include <nvme/nvme_core.h>

/*
 * nvme_unix_time_ms - Unix-epoch wall-clock time in milliseconds.
 *
 * Single caller: nvme_configure_timestamp() during controller probe,
 * which feeds the value to NVMe Set Features(Timestamp) so the device's
 * internal event/error log entries line up with host time.  One-shot,
 * so per-call timer.device open/close is fine.
 *
 * AmigaOS epoch is 1978-01-01 UTC; Unix is 1970-01-01 UTC.
 * Offset = 8 * 365 days + 2 leap days (1972, 1976) = 2922 days
 *        = 252,460,800 seconds.
 */
#define AMIGA_TO_UNIX_EPOCH_SEC  252460800LL

s64 nvme_unix_time_ms(void)
{
	struct MsgPort *port = CreateMsgPort();
	if (!port)
		return 0;
	struct timerequest *tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr));
	if (!tr) {
		DeleteMsgPort(port);
		return 0;
	}

	if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK, (struct IORequest *)tr, 0) == 0) {
		struct Device *TimerBase = tr->tr_node.io_Device;

		struct timeval tv;
		GetSysTime(&tv);
		CloseDevice((struct IORequest *)tr);
		s64 ms = ((s64)tv.tv_secs + AMIGA_TO_UNIX_EPOCH_SEC) * 1000LL
		   + (s64)tv.tv_micro / 1000LL;
		DeleteIORequest((struct IORequest *)tr);
		DeleteMsgPort(port);
		return ms;
	}

	DeleteIORequest((struct IORequest *)tr);
	DeleteMsgPort(port);
	return 0;
}

/* libc memcmp fallback: kcompat.h aliases normal call sites to the GCC
 * builtin, but freestanding variable-size compares may still need a real
 * symbol when libc is absent. */
int memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *pa = a, *pb = b;
	while (n--) {
		if (*pa != *pb)
			return *pa - *pb;
		pa++; pb++;
	}
	return 0;
}
