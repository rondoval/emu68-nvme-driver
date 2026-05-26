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

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/io.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <dos/dos.h>

#include <libraries/openpci.h>
#include <minlist.h>
#include <debug.h>

#include <device.h>
#include <config.h>
#include <nvme/nvme_probe.h>
#include <mounter.h>

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

/*
 * Forward declarations needed before _doInit and the Resident struct.
 */
static struct Library *_doInit(BPTR segList asm("a0"), struct ExecBase *SysBase asm("a6"));
APTR initFunction(struct NVMeDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"));
static const APTR funcTable[];

static struct Resident const nvmeDeviceResident __attribute__((used,no_reorder)) = {
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
    (void)SysBase;
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

    struct MountStruct ms = {
        .deviceName = (const UBYTE *)DEVICE_NAME,
        .unitNum = NULL,
        .creatorName = (const UBYTE *)DEVICE_NAME,
        .configDev = NULL,
        .SysBase = SysBase,
        .luns = FALSE,
        .slowSpinup = FALSE,
        .cdBoot = FALSE,
        .ignoreLast = TRUE,
    };
    MountDrive(&ms);

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

    base->pcieBase = OpenLibrary((CONST_STRPTR) "bcmpcie.library", 1);
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

APTR initFunction(struct NVMeDevice *base asm("d0"), ULONG segList asm("a0"), struct ExecBase *_SysBase asm("a6"))
{
    (void)_SysBase;
    Kprintf("[nvme] %s: initializing device\n", __func__);
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
    Kprintf("[nvme] %s: opening unit %ld flags=0x%lx\n", __func__, unitNumber, flags);

    /* Probe once: enumerate all NVMe controllers and build the unit list */
    if (!base->probed)
    {
        Kprintf("[nvme] %s: first open, probing for controllers\n", __func__);
        if (nvme_open_libraries(base) != ERR_NO_ERROR)
        {
            Kprintf("[nvme] %s: failed to open support libraries\n", __func__);
            io->io_Error = IOERR_OPENFAIL;
            return;
        }

        /* Best-effort: if no controllers are found the unit list stays empty
         * and all subsequent opens will return IOERR_OPENFAIL. */
        nvme_probe_all(base);
        base->probed = TRUE;
    }

    /* Look up the pre-allocated unit by its global unit number */
    struct NVMeUnit *unit = NULL;
    for (struct MinNode *n = base->units.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
    {
        struct NVMeUnit *u = (struct NVMeUnit *)n;
        if (u->unitNumber == unitNumber)
        {
            unit = u;
            break;
        }
    }

    if (unit == NULL)
    {
        Kprintf("[nvme] %s: no unit %ld found\n", __func__, unitNumber);
        io->io_Error = IOERR_OPENFAIL;
        return;
    }
    KprintfH("[nvme] %s: found unit %ld, opening\n", __func__, unitNumber);

    s32 result = UnitOpen(unit, unitNumber, (LONG)flags);

    if (result == ERR_NO_ERROR)
    {
        Kprintf("[nvme] %s: unit %ld opened (openCnt=%lu)\n", __func__, unitNumber, (ULONG)unit->unit.unit_OpenCnt);
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
    Kprintf("[nvme] %s: expunge\n", __func__);
    if (base->device.dd_Library.lib_OpenCnt > 0)
    {
        base->device.dd_Library.lib_Flags |= LIBF_DELEXP;
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
    Kprintf("[nvme] %s: closing unit %ld\n", __func__, unit->unitNumber);

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