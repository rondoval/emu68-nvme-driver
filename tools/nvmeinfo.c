// SPDX-License-Identifier: GPL-2.0-only
#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/bcmpcie_protos.h>
#else
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bcmpcie.h>
#endif

#include <exec/types.h>
#include <exec/tasks.h>
#include <libraries/openpci.h>
#include <libraries/pcitags.h>
#include <iomem.h>

struct Library     *BCMPCIEBase;

static const char verstag[] __attribute__((used)) = VERSTAG;

#define NVME_CLASS_CODE 0x010802UL

/* NVMe BAR0 register offsets */
#define NVME_REG_CAP_LO  0x00UL
#define NVME_REG_CAP_HI  0x04UL
#define NVME_REG_VS      0x08UL
#define NVME_REG_INTMS   0x0CUL
#define NVME_REG_INTMC   0x10UL
#define NVME_REG_CC      0x14UL
#define NVME_REG_CSTS    0x1CUL
#define NVME_REG_NSSR    0x20UL
#define NVME_REG_AQA     0x24UL
#define NVME_REG_ASQ_LO  0x28UL
#define NVME_REG_ASQ_HI  0x2CUL
#define NVME_REG_ACQ_LO  0x30UL
#define NVME_REG_ACQ_HI  0x34UL

/* ------------------------------------------------------------------ */
/* CAP [63:0] — Controller Capabilities                               */
/* ------------------------------------------------------------------ */
static void decode_cap(ULONG lo, ULONG hi)
{
    ULONG mqes   = (lo & 0xFFFFUL) + 1UL;
    ULONG cqr    = (lo >> 16) & 1UL;
    ULONG ams    = (lo >> 17) & 3UL;
    ULONG to     = (lo >> 24) & 0xFFUL;
    ULONG dstrd  =  hi        & 0xFUL;
    ULONG nssrs  = (hi >>  4) & 1UL;
    ULONG css    = (hi >>  5) & 0xFFUL;
    ULONG bps    = (hi >> 13) & 1UL;
    ULONG cps    = (hi >> 14) & 3UL;
    ULONG mpsmin = (hi >> 16) & 0xFUL;
    ULONG mpsmax = (hi >> 20) & 0xFUL;
    ULONG pmrs   = (hi >> 24) & 1UL;
    ULONG cmbs   = (hi >> 25) & 1UL;
    ULONG nsss   = (hi >> 26) & 1UL;

    static const CONST_STRPTR cps_str[] = {
        (CONST_STRPTR)"not reported",
        (CONST_STRPTR)"controller",
        (CONST_STRPTR)"domain",
        (CONST_STRPTR)"NVM subsystem"
    };

    Printf((CONST_STRPTR)"  CAP   = 0x%08lx_%08lx\n", hi, lo);
    Printf((CONST_STRPTR)"    Max Queue Entries:    %lu\n", mqes);
    Printf((CONST_STRPTR)"    Contiguous Queues:    %s\n",
           (ULONG)(cqr ? "required" : "not required"));
    Printf((CONST_STRPTR)"    Arbitration (AMS):    Round Robin");
    if (ams & 1UL) Printf((CONST_STRPTR)", WRR+urgent");
    if (ams & 2UL) Printf((CONST_STRPTR)", vendor-specific");
    Printf((CONST_STRPTR)"\n");
    Printf((CONST_STRPTR)"    Timeout (TO):         %lu ms max\n", to * 500UL);
    Printf((CONST_STRPTR)"    Doorbell Stride:      %lu bytes\n", 4UL << dstrd);
    Printf((CONST_STRPTR)"    Subsys Reset (NSSRS): %s\n",
           (ULONG)(nssrs ? "yes" : "no"));
    Printf((CONST_STRPTR)"    Command Sets (CSS):\n");
    if (css == 0UL)     Printf((CONST_STRPTR)"      (none reported)\n");
    if (css & 0x01UL)   Printf((CONST_STRPTR)"      NVM Command Set\n");
    if (css & 0x40UL)   Printf((CONST_STRPTR)"      I/O Command Sets (KV/ZNS)\n");
    if (css & 0x80UL)   Printf((CONST_STRPTR)"      Admin-only (no I/O)\n");
    Printf((CONST_STRPTR)"    Boot Partitions:      %s\n",
           (ULONG)(bps ? "yes" : "no"));
    Printf((CONST_STRPTR)"    Power Scope (CPS):    %s\n",
           (ULONG)cps_str[cps]);
    Printf((CONST_STRPTR)"    Min Page Size:        %lu bytes (2^%lu)\n",
           4096UL << mpsmin, 12UL + mpsmin);
    Printf((CONST_STRPTR)"    Max Page Size:        %lu bytes (2^%lu)\n",
           4096UL << mpsmax, 12UL + mpsmax);
    Printf((CONST_STRPTR)"    Persistent Mem (PMRS):%s\n",
           (ULONG)(pmrs ? " yes" : " no"));
    Printf((CONST_STRPTR)"    Ctrl Mem Buf  (CMBS): %s\n",
           (ULONG)(cmbs ? "yes" : "no"));
    Printf((CONST_STRPTR)"    Subsys Shutdown:      %s\n",
           (ULONG)(nsss ? "yes" : "no"));
}

/* ------------------------------------------------------------------ */
/* VS — Version                                                        */
/* ------------------------------------------------------------------ */
static void decode_vs(ULONG vs)
{
    ULONG mjr = (vs >> 16) & 0xFFFFUL;
    ULONG mnr = (vs >>  8) & 0xFFUL;
    ULONG ter =  vs        & 0xFFUL;

    if (ter)
        Printf((CONST_STRPTR)"  VS    = 0x%08lx  ->  NVMe %lu.%lu.%lu\n",
               vs, mjr, mnr, ter);
    else
        Printf((CONST_STRPTR)"  VS    = 0x%08lx  ->  NVMe %lu.%lu\n",
               vs, mjr, mnr);
}

/* ------------------------------------------------------------------ */
/* CC — Controller Configuration                                       */
/* ------------------------------------------------------------------ */
static void decode_cc(ULONG cc)
{
    static const CONST_STRPTR css_str[] = {
        (CONST_STRPTR)"NVM Command Set",
        (CONST_STRPTR)"?", (CONST_STRPTR)"?", (CONST_STRPTR)"?",
        (CONST_STRPTR)"?", (CONST_STRPTR)"?",
        (CONST_STRPTR)"I/O Command Sets",
        (CONST_STRPTR)"Admin-only"
    };
    static const CONST_STRPTR ams_str[] = {
        (CONST_STRPTR)"Round Robin",
        (CONST_STRPTR)"WRR+urgent",
        (CONST_STRPTR)"?", (CONST_STRPTR)"?",
        (CONST_STRPTR)"?", (CONST_STRPTR)"?", (CONST_STRPTR)"?",
        (CONST_STRPTR)"vendor"
    };
    static const CONST_STRPTR shn_str[] = {
        (CONST_STRPTR)"none",
        (CONST_STRPTR)"normal shutdown",
        (CONST_STRPTR)"abrupt shutdown",
        (CONST_STRPTR)"?"
    };

    ULONG en     =  cc        & 1UL;
    ULONG css    = (cc >>  4) & 7UL;
    ULONG mps    = (cc >>  7) & 0xFUL;
    ULONG ams    = (cc >> 11) & 7UL;
    ULONG shn    = (cc >> 14) & 3UL;
    ULONG iosqes = (cc >> 16) & 0xFUL;
    ULONG iocqes = (cc >> 20) & 0xFUL;

    Printf((CONST_STRPTR)"  CC    = 0x%08lx\n", cc);
    Printf((CONST_STRPTR)"    Enable (EN):          %s\n",
           (ULONG)(en ? "yes" : "no"));
    Printf((CONST_STRPTR)"    I/O Command Set:      %s\n", (ULONG)css_str[css]);
    Printf((CONST_STRPTR)"    Memory Page Size:     %lu bytes\n", 4096UL << mps);
    Printf((CONST_STRPTR)"    Arbitration (AMS):    %s\n", (ULONG)ams_str[ams]);
    Printf((CONST_STRPTR)"    Shutdown (SHN):       %s\n", (ULONG)shn_str[shn]);
    Printf((CONST_STRPTR)"    SQ Entry Size:        %lu bytes\n", 1UL << iosqes);
    Printf((CONST_STRPTR)"    CQ Entry Size:        %lu bytes\n", 1UL << iocqes);
}

/* ------------------------------------------------------------------ */
/* CSTS — Controller Status                                            */
/* ------------------------------------------------------------------ */
static void decode_csts(ULONG csts)
{
    static const CONST_STRPTR shst_str[] = {
        (CONST_STRPTR)"normal operation",
        (CONST_STRPTR)"shutdown in progress",
        (CONST_STRPTR)"shutdown complete",
        (CONST_STRPTR)"?"
    };

    ULONG rdy   =  csts       & 1UL;
    ULONG cfs   = (csts >> 1) & 1UL;
    ULONG shst  = (csts >> 2) & 3UL;
    ULONG nssro = (csts >> 4) & 1UL;
    ULONG pp    = (csts >> 5) & 1UL;

    Printf((CONST_STRPTR)"  CSTS  = 0x%08lx\n", csts);
    Printf((CONST_STRPTR)"    Ready (RDY):          %s\n",
           (ULONG)(rdy ? "yes" : "no"));
    Printf((CONST_STRPTR)"    Fatal Status (CFS):   %s\n",
           (ULONG)(cfs ? "*** CONTROLLER FATAL ERROR ***" : "no"));
    Printf((CONST_STRPTR)"    Shutdown (SHST):      %s\n", (ULONG)shst_str[shst]);
    Printf((CONST_STRPTR)"    Subsys Reset Occurred:%s\n",
           (ULONG)(nssro ? " yes" : " no"));
    Printf((CONST_STRPTR)"    Processing Paused:    %s\n",
           (ULONG)(pp ? "yes" : "no"));
}

/* ------------------------------------------------------------------ */
/* AQA — Admin Queue Attributes                                        */
/* ------------------------------------------------------------------ */
static void decode_aqa(ULONG aqa)
{
    ULONG asqs = (aqa         & 0xFFFUL) + 1UL;
    ULONG acqs = ((aqa >> 16) & 0xFFFUL) + 1UL;

    Printf((CONST_STRPTR)"  AQA   = 0x%08lx\n", aqa);
    Printf((CONST_STRPTR)"    Admin SQ size:        %lu entries\n", asqs);
    Printf((CONST_STRPTR)"    Admin CQ size:        %lu entries\n", acqs);
}

/* ------------------------------------------------------------------ */
/* Feature summary                                                     */
/* ------------------------------------------------------------------ */
static void feature_summary(ULONG cap_lo, ULONG cap_hi, ULONG cc, ULONG csts)
{
    ULONG css_cap = (cap_hi >>  5) & 0xFFUL;
    ULONG ams_cap = (cap_lo >> 17) & 3UL;
    ULONG en      =  cc            & 1UL;
    ULONG rdy     =  csts          & 1UL;
    ULONG cfs     = (csts >>  1)   & 1UL;

    Printf((CONST_STRPTR)"\n--- Feature Summary ---\n");
    if (cfs)
        Printf((CONST_STRPTR)"  State:                %s [*** FATAL ERROR ***]\n",
               (ULONG)(en ? "enabled" : "disabled"));
    else
        Printf((CONST_STRPTR)"  State:                %s\n",
               (ULONG)(en ? (rdy ? "enabled, ready" : "enabled, NOT ready")
                           : "disabled"));

    Printf((CONST_STRPTR)"  Command sets:        ");
    if (css_cap & 0x01UL) Printf((CONST_STRPTR)" NVM");
    if (css_cap & 0x40UL) Printf((CONST_STRPTR)" KV/ZNS");
    if (css_cap & 0x80UL) Printf((CONST_STRPTR)" admin-only");
    if (!(css_cap & 0xC1UL)) Printf((CONST_STRPTR)" (none reported)");
    Printf((CONST_STRPTR)"\n");

    Printf((CONST_STRPTR)"  Arbitration:          Round Robin");
    if (ams_cap & 1UL) Printf((CONST_STRPTR)", WRR+urgent");
    if (ams_cap & 2UL) Printf((CONST_STRPTR)", vendor");
    Printf((CONST_STRPTR)"\n");

    Printf((CONST_STRPTR)"  NVM Subsystem Reset:  %s\n",
           (ULONG)((cap_hi >>  4) & 1UL ? "supported" : "not supported"));
    Printf((CONST_STRPTR)"  Boot Partitions:      %s\n",
           (ULONG)((cap_hi >> 13) & 1UL ? "supported" : "not supported"));
    Printf((CONST_STRPTR)"  Persistent Mem Rgn:   %s\n",
           (ULONG)((cap_hi >> 24) & 1UL ? "supported" : "not supported"));
    Printf((CONST_STRPTR)"  Controller Mem Buf:   %s\n",
           (ULONG)((cap_hi >> 25) & 1UL ? "supported" : "not supported"));
    Printf((CONST_STRPTR)"  Subsystem Shutdown:   %s\n",
           (ULONG)((cap_hi >> 26) & 1UL ? "supported" : "not supported"));
}

/* ------------------------------------------------------------------ */
/* Per-device register dump                                            */
/* ------------------------------------------------------------------ */
static void dump_nvme_regs(volatile UBYTE *bar0)
{
    ULONG cap_lo = mmio_read32(bar0 + NVME_REG_CAP_LO);
    ULONG cap_hi = mmio_read32(bar0 + NVME_REG_CAP_HI);
    ULONG vs     = mmio_read32(bar0 + NVME_REG_VS);
    ULONG intms  = mmio_read32(bar0 + NVME_REG_INTMS);
    ULONG intmc  = mmio_read32(bar0 + NVME_REG_INTMC);
    ULONG cc     = mmio_read32(bar0 + NVME_REG_CC);
    ULONG csts   = mmio_read32(bar0 + NVME_REG_CSTS);
    ULONG aqa    = mmio_read32(bar0 + NVME_REG_AQA);
    ULONG asq_lo = mmio_read32(bar0 + NVME_REG_ASQ_LO);
    ULONG asq_hi = mmio_read32(bar0 + NVME_REG_ASQ_HI);
    ULONG acq_lo = mmio_read32(bar0 + NVME_REG_ACQ_LO);
    ULONG acq_hi = mmio_read32(bar0 + NVME_REG_ACQ_HI);

    decode_cap(cap_lo, cap_hi);
    decode_vs(vs);
    decode_cc(cc);
    decode_csts(csts);
    decode_aqa(aqa);
    Printf((CONST_STRPTR)"  INTMS = 0x%08lx  INTMC = 0x%08lx\n", intms, intmc);
    Printf((CONST_STRPTR)"  ASQ   = 0x%08lx_%08lx\n", asq_hi, asq_lo);
    Printf((CONST_STRPTR)"  ACQ   = 0x%08lx_%08lx\n", acq_hi, acq_lo);

    feature_summary(cap_lo, cap_hi, cc, csts);
}

int main(void)
{
    BCMPCIEBase = OpenLibrary((CONST_STRPTR)"bcmpcie.library", 1);
    if (!BCMPCIEBase) {
        Printf((CONST_STRPTR)"Failed to open bcmpcie.library\n");
        return 10;
    }

    struct pci_dev *pd = NULL;
    int found = 0;

    while ((pd = pci_find_class(NVME_CLASS_CODE, pd))) {
        found++;
        Printf((CONST_STRPTR)"NVMe device #%ld\n", (ULONG)found);
        Printf((CONST_STRPTR)"  Vendor:Device = %04lx:%04lx\n",
               (ULONG)pd->vendor, (ULONG)pd->device);
        Printf((CONST_STRPTR)"  BAR0          = 0x%08lx\n\n",
               pd->base_address[0]);

        if (pd->base_address[0])
            dump_nvme_regs((volatile UBYTE *)pd->base_address[0]);
        else
            Printf((CONST_STRPTR)"  BAR0 not mapped\n");

        Printf((CONST_STRPTR)"\n");
    }

    if (!found)
        Printf((CONST_STRPTR)"No NVMe devices found (class 0x%06lx)\n",
               NVME_CLASS_CODE);

    CloseLibrary(BCMPCIEBase);
    return 0;
}
