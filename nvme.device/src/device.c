// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/bcmpcie_protos.h>
#else
#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <proto/exec.h>
#define BCMPCIE_BASE_NAME pcielibBase
#include <proto/bcmpcie.h>
#endif

#include <exec/resident.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <dos/dos.h>

#include <libraries/openpci.h>
#include <emu68_features.h>
#include <minlist.h>

#include "device.h"
#include "nvme/nvme_probe.h"
#include "mounter.h"

/*
 * Placed first so that accidentally running the device as a program
 * returns -1 instead of jumping into code.
 */
int doNotExecute(void);
int __attribute__((used, no_reorder)) doNotExecute(void)
{
    return -1;
}

extern const UBYTE endOfCode;

static const char deviceName[] = DEVICE_NAME;
static const char deviceIdString[] = DEVICE_IDSTRING;

#ifdef MOUNTER_LOG
/* MOUNTER_LOG sink: route the mounter submodule's diagnostics (%l-normalized
 * RawDoFmt format strings) to the debug backend.  Prototype declared here
 * because the mounter only declares it internally. */
void mounter_log(const char *fmt, ...);
void mounter_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
    RawDoFmt((CONST_STRPTR)fmt, args, (APTR)putch, NULL);
#pragma GCC diagnostic pop
    va_end(args);
}
#endif

/*
 * Forward declarations needed before _doInit and the Resident struct.
 */
static struct Library *_doInit(BPTR segList asm("a0"), struct ExecBase *SysBase asm("a6"));
APTR initFunction(struct NVMeDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"));
static const APTR funcTable[];
static s32 devEnsureProbed(struct NVMeDevice *base);
static void devMountUnits(struct NVMeDevice *base, struct ExecBase *SysBase);

static struct Resident const nvmeDeviceResident __attribute__((used, no_reorder)) = {
    RTC_MATCHWORD,
    (struct Resident *)&nvmeDeviceResident,
    (APTR)&endOfCode,
    RTF_COLDSTART, /* no RTF_AUTOINIT — _doInit handles MakeLibrary */
    DEVICE_VERSION,
    NT_DEVICE,
    DEVICE_PRIORITY,
    (APTR)&deviceName,
    (APTR)&deviceIdString,
    (APTR)_doInit};

/*
 * _doInit - manual Resident init, called directly by exec.
 *
 * Exec passes: a0 = segment list BPTR, a6 = SysBase.
 * Returns the device base in d0 (via C return value).
 *
 * RTF_AUTOINIT would call MakeLibrary+AddDevice internally, but the device
 * would not yet be in the DeviceList when initFunction runs.  The mounter
 * needs to call OpenDevice("nvme.device", …) on itself, so AddDevice MUST
 * happen before MountDrive.  Manual init gives us that ordering.
 */
static struct Library *_doInit(BPTR segList asm("a0"), struct ExecBase *SysBase asm("a6"))
{
    if (!emu68_has_dcache_range_ops())
    {
        Kprintf("[nvme] %s: rangeops build, but Emu68 lacks dcache-range-ops rev 1 - refusing to load. Install the standard driver package or update Emu68.\n", __func__);
        return NULL;
    }

    /* MakeLibrary macro uses old-style '()' function pointer — suppress the warning */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
    struct NVMeDevice *base = (struct NVMeDevice *)MakeLibrary(
        (APTR *)funcTable,
        NULL,
        (APTR)initFunction,
        sizeof(struct NVMeDevice),
        (ULONG)segList);
#pragma GCC diagnostic pop

    if (!base)
        return NULL;

    AddDevice((struct Device *)base);

    if (devEnsureProbed(base) == ERR_NO_ERROR)
        devMountUnits(base, SysBase);

    return (struct Library *)base;
}

static void nvme_close_libraries(struct NVMeDevice *base)
{
    if (base->pcieBase != NULL)
    {
        CloseLibrary(base->pcieBase);
        base->pcieBase = NULL;
    }

    if (base->utilityBase != NULL)
    {
        CloseLibrary(base->utilityBase);
        base->utilityBase = NULL;
    }
}

static s32 nvme_open_libraries(struct NVMeDevice *base)
{
    if (base->utilityBase != NULL && base->pcieBase != NULL)
        return ERR_NO_ERROR; /* already open */

    nvme_close_libraries(base);

    base->utilityBase = OpenLibrary((CONST_STRPTR) "utility.library", LIB_MIN_VERSION);
    if (base->utilityBase == NULL)
    {
        Kprintf("[nvme] %s: failed to open utility.library\n", __func__);
        return ERR_LIBRARY_ERROR;
    }

    /* v2: the typed multi-vector interrupt API (AllocIntVectors, LVO -342…) is
     * only present in bcmpcie.library 2.0.  Requesting v2 makes the open fail
     * cleanly against an older 1.x library rather than crashing on a call that
     * lands past its function table. */
    base->pcieBase = OpenLibrary((CONST_STRPTR) "bcmpcie.library", 2);
    if (base->pcieBase == NULL)
    {
        Kprintf("[nvme] %s: Failed to open %s\n", __func__, "bcmpcie.library");
        nvme_close_libraries(base);
        return ERR_LIBRARY_ERROR;
    }

    struct Library *pcielibBase = base->pcieBase;
    UWORD flags = pci_bus();
    if (!(flags & BCM2711PCIeBus))
    {
        Kprintf("[nvme] %s: %s bus flags=0x%04lx, BCM2711PCIeBus not set\n", __func__, "bcmpcie.library", (ULONG)flags);
        nvme_close_libraries(base);
        return ERR_LIBRARY_ERROR;
    }

    return ERR_NO_ERROR;
}

/* Probe once: open support libraries and enumerate all PCIe NVMe controllers,
 * populating base->units.  Shared by the init-time automount and the first
 * OpenDevice; on library failure probed stays FALSE so a later open retries. */
static s32 devEnsureProbed(struct NVMeDevice *base)
{
    if (base->probed)
        return ERR_NO_ERROR;

    s32 err = nvme_open_libraries(base);
    if (err != ERR_NO_ERROR)
        return err;

    /* Best-effort: if no controllers are found the unit list stays empty
     * and all opens will return IOERR_OPENFAIL. */
    nvme_probe_all(base);
    base->probed = TRUE;
    return ERR_NO_ERROR;
}

/* MBR/GPT/superfloppy automount recipes; values in config.h.  RDB partitions
 * carry their own filesystem/handler info and don't use these. */
static const struct MountFS fatRecipe = {
    .dosType = NVME_FAT_DOSTYPE,
    .handler = (const UBYTE *)NVME_FAT_HANDLER,
    .dosName = (const UBYTE *)NVME_LEGACY_DOSNAME,
    .buffers = NVME_LEGACY_BUFFERS,
    .maxTransfer = NVME_LEGACY_MAXTRANSFER,
};

static const struct MountFS ntfsRecipe = {
    .dosType = NVME_NTFS_DOSTYPE,
    .handler = (const UBYTE *)NVME_NTFS_HANDLER,
    .dosName = (const UBYTE *)NVME_LEGACY_DOSNAME,
    .buffers = NVME_LEGACY_BUFFERS,
    .maxTransfer = NVME_LEGACY_MAXTRANSFER,
};

/* exFATFileSystem sizes its own cache and ignores de_NumBuffers, de_MaxTransfer
 * and de_Mask; the values are kept for uniformity with the other two.  Ignoring
 * de_Mask means its buffers can land unaligned and take the driver's bounce
 * path — correct, just not zero-copy.  A drive with no exFATFileSystem in L: is
 * skipped by the mounter's own availability check, exactly as before. */
static const struct MountFS exfatRecipe = {
    .dosType = NVME_EXFAT_DOSTYPE,
    .handler = (const UBYTE *)NVME_EXFAT_HANDLER,
    .dosName = (const UBYTE *)NVME_LEGACY_DOSNAME,
    .buffers = NVME_LEGACY_BUFFERS,
    .maxTransfer = NVME_LEGACY_MAXTRANSFER,
};

/* Mount every probed namespace: RDB partitions plus MBR/GPT/superfloppy
 * filesystems via the recipes above.  Uses the mounter's explicit
 * {count, unit...} list: unit numbers are contiguous 0..nextUnitNumber-1
 * right after nvme_probe_all() and may exceed the classic 0-7 scan range. */
static void devMountUnits(struct NVMeDevice *base, struct ExecBase *SysBase)
{
    ULONG count = base->nextUnitNumber;
    if (count == 0)
        return;

    /* The mounter takes the unit list as pure input and reports per-unit results
     * into a separate array, so the two are allocated apart. */
    ULONG *units = AllocMem(count * sizeof(ULONG), MEMF_PUBLIC);
    LONG *results = AllocMem(count * sizeof(LONG), MEMF_PUBLIC);

    if (units != NULL && results != NULL) {
        for (ULONG i = 0; i < count; i++)
            units[i] = i;

        struct MountStruct ms = {
            .deviceName = (const UBYTE *)DEVICE_NAME,
            .units = units,
            .unitCount = count,
            .unitResults = results,
            .creatorName = (const UBYTE *)DEVICE_NAME,
            .configDev = NULL, /* no autoconfig board; MountDrive() supplies a fake one
                                  pre-DOS, without which its BootNodes are not bootable */
            .SysBase = SysBase,
            /* namespaces report DG_DIRECT_ACCESS, and are independent disks, so one
             * namespace's RDBFF_LAST says nothing about the next */
            .flags = MSF_NO_CD | MSF_IGNORE_LAST,
            .fs = {
                [MOUNTFS_FAT]   = &fatRecipe,
                [MOUNTFS_NTFS]  = &ntfsRecipe,
                [MOUNTFS_EXFAT] = &exfatRecipe,
            },
            .dmaAlign = DMA_ALIGN_MIN, /* recipe FS buffers never take the bounce path */
        };

        LONG mounted = MountDrive(&ms, NULL);
        (void)mounted; /* only read by Kprintf when the debug backend is on */
        Kprintf("[nvme] %s: mounted %ld partition(s) on %lu unit(s)\n", __func__, mounted, count);
        for (ULONG i = 0; i < count; i++)
            KprintfT("[nvme] %s: unit %lu: %ld\n", __func__, i, results[i]);
    }

    if (results != NULL)
        FreeMem(results, count * sizeof(LONG));
    if (units != NULL)
        FreeMem(units, count * sizeof(ULONG));
}

/* reset_guard prepare callback (interrupt-safe). */
static void nvme_device_reset_prepare(APTR user)
{
    nvme_reset_quiesce_all(user);
}

APTR initFunction(struct NVMeDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"))
{
    (void)_SysBase;
    base->segList = segList;
    base->device.dd_Library.lib_IdString = (APTR)deviceIdString;
    base->device.dd_Library.lib_Version = DEVICE_VERSION;
    base->device.dd_Library.lib_Revision = DEVICE_REVISION;
    base->device.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->device.dd_Library.lib_Node.ln_Name = (APTR)deviceName;
    base->device.dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;

    _NewMinList(&base->controllers);
    _NewMinList(&base->units);
    base->probed = FALSE;
    base->utilityBase = NULL;
    base->pcieBase = NULL;

    if (!reset_guard_install(&base->resetGuard, nvme_device_reset_prepare, base,
                             (CONST_STRPTR) "nvme.device"))
        Kprintf("[nvme] %s: reset guard install failed\n", __func__);

    return base;
}

/*
 * openLib - open nvme.device for a given unit number.
 *
 * On the first call ever: opens support libraries, runs nvme_probe_all()
 * to enumerate all PCIe NVMe controllers and pre-populate the unit list.
 *
 * The unit is looked up by unitNumber in the pre-populated base->units
 * list.  If not found the unit number is invalid (no such controller /
 * namespace) and IOERR_OPENFAIL is returned.
 */
static void openLib(struct IOStdReq *io asm("a1"), LONG unitNumber asm("d0"),
                    ULONG flags asm("d1"), struct NVMeDevice *base asm("a6"))
{
    KprintfT("[nvme] %s: opening unit %ld flags=0x%lx\n", __func__, unitNumber, flags);

    /* Probe once: enumerate all NVMe controllers and build the unit list */
    if (devEnsureProbed(base) != ERR_NO_ERROR)
    {
        Kprintf("[nvme] %s: failed to open support libraries\n", __func__);
        io->io_Error = IOERR_OPENFAIL;
        return;
    }

    /* Look up the pre-allocated unit by its global unit number.
     * Forbid around the walk: AdminWorker may concurrently add (rescan
     * discovered a new namespace) or remove (namespace went away) nodes
     * via nvme_alloc_nvmeunit / nvme_ns_remove — both mutate base->units
     * under their own Forbid.  No need to hold past the walk: extracting
     * a pointer is safe because the unit struct outlives the list
     * membership (deferred-free at controller expunge), and any
     * subsequent state change is gated by NVME_UNIT_DEAD which UnitOpen
     * re-checks. */
    struct NVMeUnit *unit = NULL;
    Forbid();
    for (struct MinNode *n = base->units.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
    {
        struct NVMeUnit *u = (struct NVMeUnit *)n;
        if (u->unitNumber == unitNumber)
        {
            unit = u;
            break;
        }
    }
    Permit();

    if (unit == NULL)
    {
        io->io_Error = IOERR_OPENFAIL;
        return;
    }
    KprintfT("[nvme] %s: found unit %ld, opening\n", __func__, unitNumber);

    s32 result = UnitOpen(unit, unitNumber, (LONG)flags);

    if (result == ERR_NO_ERROR)
    {
        KprintfT("[nvme] %s: unit %ld opened (openCnt=%lu)\n", __func__, unitNumber, (ULONG)unit->unit.unit_OpenCnt);
        io->io_Unit = (struct Unit *)unit;
        base->device.dd_Library.lib_OpenCnt++;
        base->device.dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
        io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    }
    else
    {
        Kprintf("[nvme] %s: UnitOpen failed (%ld)\n", __func__, result);
        io->io_Error = IOERR_OPENFAIL;
    }
}

static ULONG expungeLib(struct NVMeDevice *base asm("a6"))
{
    if (base->device.dd_Library.lib_OpenCnt > 0)
    {
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    /* The ColdReboot vector may have been re-patched on top of our stub —
     * then the code must stay resident. */
    if (!reset_guard_remove(&base->resetGuard))
    {
        KprintfT("[nvme] %s: reset guard not removable, staying resident\n", __func__);
        return 0;
    }

    nvme_unprobe_all(base);

    nvme_close_libraries(base);

    ULONG segList = base->segList;

    Forbid();
    Remove((struct Node *)base);
    Permit();

    ULONG size = (ULONG)(base->device.dd_Library.lib_NegSize +
                         base->device.dd_Library.lib_PosSize);
    APTR pointer = (APTR)((ULONG)base - base->device.dd_Library.lib_NegSize);
    FreeMem(pointer, size);

    return segList;
}

static ULONG closeLib(struct IOStdReq *io asm("a1"), struct NVMeDevice *base asm("a6"))
{
    struct NVMeUnit *unit = (struct NVMeUnit *)io->io_Unit;
    KprintfT("[nvme] %s: closing unit %ld\n", __func__, unit->unitNumber);

    /* UnitClose handles hardware teardown on last close; the NVMeUnit
     * struct itself is NOT freed — it lives until expungeLib. */
    UnitClose(unit);

    base->device.dd_Library.lib_OpenCnt--;

    if (base->device.dd_Library.lib_OpenCnt == 0)
    {
        if (base->device.dd_Library.lib_Flags & LIBF_DELEXP)
            return expungeLib(base);
    }

    return 0;
}

static APTR extFunc(struct NVMeDevice *base asm("a6"))
{
    return base;
}

static const APTR funcTable[] = {
    (APTR)openLib,
    (APTR)closeLib,
    (APTR)expungeLib,
    (APTR)extFunc,
    (APTR)beginIO,
    (APTR)abortIO,
    (APTR)-1};