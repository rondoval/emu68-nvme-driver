// SPDX-License-Identifier: GPL-2.0
/*
 * kcompat.h — Linux-kernel-API shims used across the nvme_*.c sources.
 *
 * Much of the driver is ported from Linux's NVMe host code, which expects a
 * handful of kernel primitives (bitops, byte-swap, size limits, overflow
 * checks, ktime).  This header provides just enough surface to compile that
 * code against the Amiga build environment.  The shims are deliberately
 * minimal: uniprocessor m68k semantics, plain memory accesses, no SMP barriers.
 */
#ifndef _NVME_KCOMPAT_H
#define _NVME_KCOMPAT_H

#include <exec/execbase.h> /* DMA_ReadFromRAM for CachePreDMA(); older NDKs don't pull it in transitively */

#define USEC_PER_SEC 1000000UL

/* Pre-DMA cache maintenance.  @to_device selects the memory->device direction
 * (DMA_ReadFromRAM): the device will READ this buffer (NVMe write, SQE, PRP
 * list), so a clean is enough and the lines stay valid.  When clear the device
 * will WRITE the buffer (NVMe read), so it is clean+invalidated. */
static inline void nvme_cache_flush(void *addr, ULONG len, BOOL to_device)
{
	ULONG cache_len = len;
	CachePreDMA((APTR)addr, &cache_len, to_device ? DMA_ReadFromRAM : 0);
}

/* Post-DMA: invalidate stale CPU lines after the device wrote @addr (NVMe read,
 * CQE).  Not needed after a device read (the library would no-op it anyway). */
static inline void nvme_cache_inval(void *addr, ULONG len)
{
	ULONG cache_len = len;
	CachePostDMA((APTR)addr, &cache_len, 0);
}

/* ---- bitops (uniprocessor) --------------------------------------- */
static inline int test_bit(unsigned bit, const volatile unsigned long *addr)
{
	return (*addr >> bit) & 1UL;
}
static inline void set_bit(unsigned bit, volatile unsigned long *addr)
{
	*addr |= (1UL << bit);
}
static inline void clear_bit(unsigned bit, volatile unsigned long *addr)
{
	*addr &= ~(1UL << bit);
}
static inline int test_and_set_bit(unsigned bit, volatile unsigned long *addr)
{
	unsigned long mask = 1UL << bit;
	int prev = (*addr & mask) ? 1 : 0;
	*addr |= mask;
	return prev;
}
static inline int test_and_clear_bit(unsigned bit, volatile unsigned long *addr)
{
	unsigned long mask = 1UL << bit;
	int prev = (*addr & mask) ? 1 : 0;
	*addr &= ~mask;
	return prev;
}

/* ---- string + format -------------------------------------------- */
/* Linux memchr_inv(): return the first byte that differs from c, or NULL
 * if the whole range matches.  Used by namespace-ID fallback logic to avoid
 * overwriting descriptor-derived identifiers with all-zero defaults. */
static inline void *memchr_inv(const void *start, int c, unsigned long bytes)
{
	const unsigned char *ptr = start;
	unsigned char match = (unsigned char)c;

	while (bytes-- != 0UL)
	{
		if (*ptr != match)
			return (void *)ptr;
		ptr++;
	}

	return (void *)0;
}

/* check_shl_overflow(value, shift, &result): TRUE if value << shift would
 * overflow a 32-bit u32.  A zero shift is allowed and simply copies value.
 * Used only by nvme_mps_to_sectors. */
static inline int check_shl_overflow(u32 value, u32 shift, u32 *result)
{
	if (shift >= 32U)
		return 1;
	if (shift != 0U && (value >> (32U - shift)) != 0U)
		return 1;
	*result = value << shift;
	return 0;
}

int memcmp(const void *a, const void *b, unsigned long n);

/* Wall-clock helper for NVMe's Timestamp feature payload.
 * Returns Unix-epoch milliseconds. */
s64 nvme_unix_time_ms(void);

/* ---- misc -------------------------------------------------------- */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif /* _NVME_KCOMPAT_H */
