/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2011-2014, Intel Corporation.
 */

#ifndef NVME_CORE_H
#define NVME_CORE_H

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#endif

#include <exec/types.h>

/* Foundation shared by all subsystem headers/sources (e.g. struct slab_cache
 * in nvme_ctrl.h's NVMeController; mmio_read32/write32 in its register
 * accessors; BIT(); Kprintf). */
#include <slab.h>
#include <iomem.h>
#include <bits.h>
#include <debug.h>

#include "config.h"
#include "kcompat.h"
#include "nvme_defs.h"
#include "nvme_constants.h"

typedef u32 size_t;
#define IOERR_NO_ERROR 0
#define SECTOR_SHIFT 9

/*
 * Default to a 4K page size, with the intention to update this
 * path in the future to accommodate architectures with differing
 * kernel and IO page sizes.
 */
#define NVME_CTRL_PAGE_SHIFT	12
#define NVME_CTRL_PAGE_SIZE	(1 << NVME_CTRL_PAGE_SHIFT)

#endif /* NVME_CORE_H */
