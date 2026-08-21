# Release notes — nvme.device 1.5

Changes since v1.4.

> Warning: still a young storage driver — keep current backups and use it at
> your own risk.

---

## Breaking changes

**MBR/GPT partition numbering can shift.** exFAT partitions now mount (below),
and each takes the next free `NVME<n>:`, so partitions that follow an exFAT one
may come up under a different number than in 1.4. RDB partitions keep the names
their RDB carries and are unaffected.

---

## New features

### Automount: exFAT partitions

MBR, GPT and superfloppy disks now mount their **exFAT** filesystems as well as
FAT and NTFS, through `L:exFATFileSystem` (dostype `FATX`).

`exFATFileSystem` is optional, like the other two handlers: a disk with no
matching handler in `L:` and no registered dostype is skipped, not mounted dead.

### Boot priority: usable from a Kickstart ROM image

The romtag priority drops from **90 to −43**. Below it is `bootmenu` (−50), which lists
the volumes this driver mounts; above it is `romboot` (−40), which is what binds
Emu68's own m68k modules — `devicetree.resource`, `gic400.library`,
`mailbox.resource`, `68040.library`. At 90 the driver initialised before any of
those existed, so `bcmpcie.library` found no device tree and no host bridge, and
NVMe never came up.

---

# Release notes — nvme.device 1.4

Changes since v1.3.

> Warning: still a young storage driver — keep current backups and use it at
> your own risk.

---

## Breaking changes

None.

---

## Reliability

### Firmware gate for rangeops builds

Builds using the inline Emu68 range cache opcodes (`EMU68_FORCE_LVO_CACHE_OPS`
off — the `-rangeops` stack archives) now check the `/emu68` device-tree
node's `dcache-range-ops` capability at init and refuse to load on firmware
that would Line-F trap on those opcodes, instead of crashing. Standard (LVO)
builds are unaffected.

---

## Bug fixes

### Mounter: fixed a use of uninitialized data on LSEG read failure

`lseg_read_long()` unconditionally combined `lseg_read_longs()`'s output
buffer into its result and stashed it in `md->lsegwordbuf`, even when the
read had failed and left that buffer untouched — a real (if narrow) read of
uninitialized stack memory on a truncated/failed LSEG chain. Both writes are now gated on the read actually succeeding.

---

## Build & tooling

### GCC 16.1 build portability

The driver now builds cleanly under GCC 16.1. None of this changes behavior:

- `-ffreestanding` moved from link options to compile options, where it
  actually affects code generation — as a link-only flag it was silently
  inert.
- `ProcessCommand`'s intentional `CMD_READ`-into-`CMD_WRITE` fall-through now
  carries `__attribute__((fallthrough))`.
- `nvme_wait_ready`'s poll counter is explicitly cast to void under
  `!TRACE`, where it's incremented but (with trace logging compiled out)
  never read.

### Interrupt setup uses the shared PCIe IRQ helper

`nvme_pci_int_enable` / `nvme_int_shutdown` now call `bcmpcie.library`'s new
`pci_irq_attach()` / `pci_irq_detach()` inline helpers instead of open-coding
the `AllocIntVectors` → `GetIntVectorType` → `AddIntVectorServer` sequence
(and its `RemIntVectorServer` / `FreeIntVectors` teardown) directly. Same
typed multi-vector API introduced in `bcmpcie.library` 2.0, same MSI-X → MSI →
INTx preference — just no longer duplicated per driver. No functional change.

### Unit-task watchdog timer uses the shared `drv_timer` helper

The controller watchdog tick in `UnitTask` now uses `emu68-common`'s
`drv_timer` instead of a hand-rolled `CreateMsgPort` / `CreateIORequest` /
`OpenDevice` / `SendIO` / `CheckIO` / `WaitIO` dance around a MICROHZ timer
request. Same periodic-tick semantics. No functional change.

### Dependencies

Building now requires **`emu68-pcie-library` 2.3** or later
(`libraries/pci_irq.h`) and **`emu68-common` 1.9.0** or later (`drv_timer.h`).
The runtime requirement of `bcmpcie.library` 2.0 is unchanged; nothing above
touches the interrupt ABI.

---

# Release notes — nvme.device 1.3

Changes since v1.2.

> Warning: still a young storage driver — keep current backups and use it at
> your own risk.

---

## Breaking changes

None.

---

## Build & tooling

### Debug output follows emu68-common's tier ladder

Verbose logging now gates on `TRACE` instead of the old `DEBUG_HIGH`, and logs
through `KprintfT` instead of `KprintfH`, matching emu68-common's cumulative
`PROFILE`/`DEBUG`/`TRACE` tier system. The build calls `emu68_debug_definitions()`
(renamed from `emu68_debug_backend_definitions()`) in both `nvme.device` and the
`tools` binaries. No behavior change — the `EMU68_DEBUG_BACKEND` selection
(`pistorm` | `serial` | `off`) still works the same way.

The mounter submodule's diagnostics now split across the same ladder instead of
following the sink alone: its `printf()` (mount errors/status) is wired to the
`debug` tier, its `dbg()` (per-step tracing) to `trace`, via
`emu68_tier_at_least()`. Previously both followed `EMU68_DEBUG_BACKEND != off`
as one flag. The mounter submodule itself also picked up a cleanup — dead
`DEBUG_MOUNTER`/`TRACE_LSEG` knobs removed, `MOUNTER_TRACE` now documented.

### Worker task setup uses the shared helpers

The driver's worker-task setup now uses emu68-common's shared `drv_task_spawn` /
`drv_task_join` helpers instead of an open-coded copy, keeping task management consistent
across the driver stack.

### Dependencies

Building now requires **`emu68-common` 1.8.0** or later (`emu68_debug_definitions()`
and `emu68_tier_at_least()` don't exist in 1.7.0). The runtime requirement of
`bcmpcie.library` 2.0 is unchanged.

---

# Release notes — nvme.device 1.2

Changes since v1.1.

> Warning: still a young storage driver — keep current backups and use it at
> your own risk.

---

## Breaking changes

Automounted **MBR/GPT partitions get new DOS device names**: the mounter rework
below names them from the driver — `NVME0:`, `NVME1:`, … — instead of the old
hardcoded `MS0:`, `MS1:`, ….  RDB partitions are unaffected and keep the names
their RDB carries.  Startup-Sequence lines, assigns and `DEVS:DOSDrivers` entries
that referred to `MS<n>:` need updating.

Those partitions also load their handler from `L:` (`L:fat95`,
`L:NTFileSystem3G`) when the dostype is not already registered in
`FileSystem.resource`, so those files should be present for MBR/GPT disks — a
partition whose handler resolves to neither is skipped cleanly rather than
mounted broken.

---

## New features

### Automount: shared `mounter` fork with FAT and NTFS recipes

The driver drops its vendored copy of the A4091 mounter for the
[`rondoval/mounter`](https://github.com/rondoval/mounter) fork (branch
`poseidon-fixes`), carried as a submodule and shared with the Poseidon USB
mass-storage class.  Rather than the mounter hardcoding filesystem policy, the
driver now hands it a *recipe* per filesystem family — dostype, handler file,
DOS name, buffers, MaxTransfer — as `NVME_*` constants in `include/config.h`:

- **FAT** (`fat95`) and **NTFS** (`NTFileSystem3G`) partitions on MBR, GPT and
  superfloppy (filesystem at block 0) disks mount, with the handler loaded from
  `L:` when the dostype isn't in `FileSystem.resource`.  Previously only FAT
  mounted, and only if `fat95` was already registered.
- **exFAT and unrecognized boot sectors are skipped** instead of being mounted
  as FAT: boot sectors are now sniffed rather than assumed.
- **GPT is gated and validated** — protective-MBR entry plus header position,
  size and CRC32 — and the extended-MBR chain is walked with correct
  container-relative links.
- Partition names are bumped past collisions, checked against both the pre-DOS
  MountList and the live DOS lists.
- **Every probed namespace is offered to the mounter**, not just units 0–7: the
  driver passes an explicit `{count, unit…}` list instead of leaving the mounter
  to scan the classic SCSI target range.  Probing itself is now shared between
  the init-time automount and the first `OpenDevice`, and is retried on open if
  the support libraries weren't available at init.

RDB partitions keep their existing behavior throughout, including autoboot.

---

## Reliability

### SCSI emulation no longer overruns the caller's buffer

`HD_SCSICMD` responses were written at full length regardless of how much the
caller asked for.  INQUIRY was the visible case — it returns 44 bytes where the
SCSI standard defines 36, so a tool passing a 36-byte buffer had 8 bytes written
past the end of it: xSysInfo's drives view gurus `8000000B` on its SCSI check.
Responses are now cut to the requested length.  MODE SENSE, REQUEST SENSE, READ
CAPACITY and the VPD pages had the same flaw, as did READ/WRITE, where a block
count larger than the buffer could overrun it over DMA and is now refused.

### Exact partition extents on MBR/GPT disks

The old mounter fitted legacy partitions to a synthetic CHS geometry, rounding
partitions whose start is not cylinder-aligned — the classic LBA-63 MBR layout
among them — so `de_LowCyl` could land one block off and writes through such a
mount would corrupt data.  Legacy partitions now map block-for-block (1 block =
1 "cylinder"), so extents are exact.

### Partition tables past 4 GB, and hardened parsing

Reads beyond 4 GB use `TD_READ64`, so the boot sector of a partition located
past the first 4 GB of a large SSD — and the GPT backup header at the end of the
disk — are read at their real offset instead of a wrapped 32-bit one.  Malformed
on-disk structures are now clamped or rejected rather than trusted: RDB
environment vectors, drive-name length bytes, hunk counts / sizes and relocation
offsets (which could overflow the heap or stack), PART/FSHD chain cycles, and
implausible partition tables.

---

## Performance

### Batched and inlined DMA cache maintenance

Cache maintenance moves onto `emu68-common` 1.7.0's `cache_ops.h`:

- The private `DMAF_NoSync` flag lets a batch of range ops pay **one** `dsb sy`
  instead of one per op.  Per command, the PRP-list flushes and the data flush
  now carry it; the SQE flush in `nvme_submit_io` — always the last clean before
  any doorbell write, immediate or batched — closes the batch.
- `nvme_cache_flush` / `nvme_cache_inval` emit the private LINE-F range opcode
  **inline** instead of calling exec's `CachePreDMA` / `CachePostDMA`, skipping
  ~3 JIT dispatcher round-trips per call — the dominant cost for small ranges.
  Building with `-DEMU68_FORCE_LVO_CACHE_OPS=ON` routes them back through the
  exec LVO, for an Emu68 that doesn't have the range opcode; release CI builds
  set it, so shipped binaries keep the LVO path.
- Completion-queue draining invalidates **once per 64-byte cache line** rather
  than once per CQE: four 16-byte CQEs share a line, and one invalidate makes
  all four visible.  A CQE the controller writes after that invalidate shows a
  stale phase bit, the drain loop stops, and the next pass re-invalidates before
  reading it — no completion can be missed.

---

## Build & tooling

### NDK 3.2 only

1.1's NDK 3.9 portability shims are reverted — the driver (and CI, now on the
`amiga-build-container` image) targets **NDK 3.2 only**.  The local `TD_READ64` …
`TD_FORMAT64` fallback definitions are gone, since NDK 3.2's
`<devices/trackdisk.h>` defines them, as are the defensive `<exec/execbase.h>` /
`<minlist.h>` includes that NDK 3.2 reaches through `<proto/exec.h>`.  The
type-correctness fixes from that work are kept.  No functional change.

### Mounter diagnostics follow the debug backend

The mounter fork's output is routed into the stack's debug backend
(`MOUNTER_LOG`, gated on `EMU68_DEBUG_BACKEND != off`), so automount decisions
appear in the same log as the rest of the driver.  Builds with the backend off
compile it out.

### Dependencies

Building now requires **`emu68-common` 1.7.0** or later (`cache_ops.h`,
`barrier.h`, and the standard `strncmp` the mounter fork links against), and a
`git submodule update --init` for the mounter.  The runtime requirement of
`bcmpcie.library` 2.0 introduced in 1.1 is unchanged.


# Release notes — nvme.device 1.1

Changes since v1.0.

> Warning: still a young storage driver — keep current backups and use it at
> your own risk.

---

## Breaking changes

None to the `nvme.device` / `nvmeadm` interfaces — unit numbering, the 1:1
namespace mapping and the private command set are unchanged.

The runtime dependency moves forward, though: 1.1 uses the typed, multi-vector
interrupt API and therefore **requires `bcmpcie.library` 2.0 or later** (it calls
`AllocIntVectors` and friends at LVOs -342…).  The driver now opens the library
requesting version 2, so it fails to start cleanly — rather than crashing — if
only an older 1.x library is installed.

---

## New features

### MSI-X interrupts

The controller interrupt is now allocated through `bcmpcie.library` 2.0's typed,
multi-vector API (`AllocIntVectors` → `AddIntVectorServer`), choosing the best
available type in the order **MSI-X → MSI → INTx**.  MSI-X is used whenever the
controller supports it, which rescues drives whose single-message MSI is broken
(e.g. the Micron 2300): the `NVME_QUIRK_BROKEN_MSI` quirk now drops only plain
MSI and lets such a device fall back to INTx.  A new `DEVICE_USE_MSIX`
build option (default on) can forbid MSI-X, mirroring `DEVICE_USE_MSI`.

Interrupt masking was simplified to match.  The ISR masks and rearms only at the
NVMe level (`INTMS` / `INTMC`), which suppresses and re-raises the source for
MSI, MSI-X and INTx alike, so the driver no longer performs any PCIe-config-level
masking (`MaskMSI` / `CheckSetINTxMask` are gone from the path).  Interrupt-setup
failures are now logged with `pcie_strerror()` for a readable reason.

---

## Reliability

### Asynchronous I/O-queue rebuild during controller reset

A controller reset runs on the unit task — the same task that drains the
completion queue — so the previous *synchronous* I/O-queue rebuild could
deadlock, `Wait()`ing for a completion only that task could deliver.  1.1
rebuilds the I/O queue pair through an asynchronous bring-up chain that returns
to the drain loop immediately; its terminal step (`nvme_reset_finish`) drives the
`CONNECTING → LIVE` transition and queues a namespace rescan, or fails the
controller to `DEAD`.  The controller stays in `RESETTING` until the chain
completes.  This lets controller reset and recovery actually finish instead of
hanging the driver.

---

## Build & tooling

### Release builds drop diagnostics; debug backend selectable

The error-logging helpers are now gated behind `DEBUG`, so release builds
compile them out, and the stack-wide debug backend is selectable at build time
(`-DEMU68_DEBUG_BACKEND=pistorm|serial|off`, via `emu68-common`).

### NDK 3.9 / -O3 build portability

The driver builds cleanly under NDK 3.9 at `-O3` with `-Wconversion`: the TD64
trackdisk command codes (`TD_READ64` … `TD_FORMAT64`) are defined locally when
the toolchain header lacks them, `<exec/execbase.h>` is included for
`DMA_ReadFromRAM`, the hand-rolled `memcmp` fallback was dropped in favour of the
compiler builtin, and assorted conversion warnings across the I/O, queue, scan
and `nvmeadm` paths were resolved.  No functional change.

### Versioning

`$VER:` strings (the driver, `nvmeadm`, and now `nvmeinfo`) are stamped
`MAJOR.MINOR`, and a CI versioning / release-check workflow was added.


# Release notes — nvme.device 1.0

First tagged release of `nvme.device`, an AmigaOS 3.x block-storage driver for
PCIe NVMe SSDs on PiStorm/Emu68 (Raspberry Pi 4B / CM4). The driver is ported
from the Linux 7.0 NVMe host driver and adapted to the AmigaOS task,
message-port, and Exec memory model.

This document summarizes what the 1.0 release provides.

> Warning: this is a new driver. Data loss, corruption, and other storage
> failures are still possible. Use it at your own risk and keep current backups.

---

## Highlights

### NVMe block device on the Emu68 PCIe path

`nvme.device` discovers and initializes PCIe-attached NVMe controllers through
`bcmpcie.library`, with MSI interrupt delivery through `gic400.library`. It
operates an admin queue plus I/O queue against the AmigaOS task model and
exposes supported namespaces as Amiga block units.

Units map **1:1 to NVMe namespaces**, not to controllers — a drive with two
namespaces appears as two units that share one physical controller. Only plain
`NVM` block namespaces are exposed: no metadata payload, no protection
information, and a logical block size of 512 B, 1 KiB, 2 KiB, or 4 KiB.

### Standard block I/O

`CMD_READ` / `CMD_WRITE`, `TD_READ64` / `TD_WRITE64`, the newstyle 64-bit
read/write commands, `TD_FORMAT` / `TD_FORMAT64` (through the normal write
path), and flush / cache synchronization are all supported. Data transfer is
PRP-based, including multi-page transfers.

Because the PiStorm PCIe engine cannot DMA to Chip RAM or to unaligned buffers,
the driver transparently **bounce-buffers through Fast RAM** when the caller's
buffer is not directly DMA-safe.

### Native Write Zeroes and TRIM

Two private commands are added for native callers (defined in
`devices/nvme.h`):

- **`NSCMD_NVME_WRITE_ZEROES`** — controller-backed zero-fill. Uses the 64-bit
  trackdisk-style offset/length contract (byte offset in `io_Offset` plus the
  high 32 bits in `io_Actual`, byte count in `io_Length`); no `io_Data`. Issued
  as the NVMe Write Zeroes opcode rather than a zeroed-buffer write.
- **`NSCMD_NVME_TRIM`** — explicit logical-block deallocate. `io_Data` points to
  an array of `struct NVMeTrimRange` entries (LBA + block count, in the unit's
  sector size); up to 256 ranges per request. Translates to NVMe Dataset
  Management / Deallocate.

Discard remains available to generic clients through the existing `HD_SCSICMD`
SCSI UNMAP translation as well.

### `NSCMD_NVME_UNIT_INFO` unit/topology query

A new `NSCMD_NVME_UNIT_INFO` command fills a `struct NVMeUnitInfo` for the
opened unit: its namespace ID, the unit range that shares its controller, the
controller `CAP` and `VS` registers, and the trimmed Identify serial / model /
firmware strings. The query is size-versioned (`nui_StructSize`), so the struct
can grow by appending fields in later driver versions. This is the data the
`nvmeadm units` listing and the `nvmeinfo` helper rely on.

### `nvmeadm` administration and diagnostics utility

The bundled `nvmeadm` CLI (installed in `C:`) is substantially expanded into a
full release-facing admin and diagnostics tool. Subcommands:

- **Information** — `units`, `identify`, `identify caps`, `identify ns`,
  `list-ns`, `smart`, `get-feature`
- **Logs** — `error-log`, `fw-log`, `effects-log`, `changed-ns`
- **Self-test** — `self-test status` / `start short` / `start extended` /
  `abort`
- **Firmware** — `fw-download`, `fw-activate replace` / `replace-activate` /
  `activate`
- **Destructive maintenance** — `format`, `sanitize` (block-erase / crypto /
  overwrite / exit-failure), `sanitize-status`

`smart` reports the SMART / health log (warnings, temperature and sensors,
spare, percentage used, data read/written, power-on hours, unsafe shutdowns,
media errors). `format` and `sanitize` destroy data and refuse to run without
the `CONFIRM` switch; `format` additionally refuses LBA formats that
`nvme.device` would not expose. The firmware-update commands
(`fw-download` / `fw-activate`) have **not yet been tested against real
hardware** — treat them as experimental. See
[README-nvmeadm.md](README-nvmeadm.md) for the full guide.

`nvmeinfo`, a lower-level passthrough-oriented inspection helper, is also built
and installed.

### Admin passthrough and device-specific quirks

Userland tooling can submit admin commands through `NSCMD_NVME_ADMIN_PASS`.
Device-specific quirk handling carried over from the Linux driver is applied
per controller (matched on PCI vendor:device) — for example, some controllers
report MDTS = 0 yet cap transfers at 128 KB, and the driver clamps the
effective transfer size accordingly for those devices. Host Memory Buffer setup
is performed for DRAM-less controllers that expose HMB capability and fit the
Amiga memory budget.

---

## Reliability and data safety

### DMA-pool memory management

All DMA buffers are allocated from dedicated DMA pools (`dmaPool` / `metaPool`)
backed by a region the PiStorm PCIe engine can reach, rather than from the
general system pool. Every queue, command, identify, firmware, and log buffer
therefore lives in PCIe-reachable memory, which helps both compatibility and
performance on the Emu68 architecture. Small-pool alignment can be steered per
device by quirk (`NVME_QUIRK_DMAPOOL_ALIGN_512`).

### Reset guard and flush on unit close

A reset guard ensures controller shutdown is performed safely and only once. On
the **last close of a unit**, the driver issues a synchronous NVMe Flush from the
caller's context (never the controller task), pushing data out of the drive's
volatile write cache before the unit is released — reducing the risk of data loss
on close and on shutdown.

### I/O back-pressure handling

The I/O path handles controller back-pressure: when a submission queue is full,
block requests are deferred and retried rather than dropped or spun on. (Admin
passthrough commands are intentionally *not* queued under back-pressure — they
return `IOERR_UNITBUSY` and the diagnostic caller simply retries.)

### Cache management for DMA operations

Cache maintenance around DMA buffers carries an explicit per-buffer
flush/invalidate direction, so it matches each transfer's data direction. This
keeps the bounce-buffer and descriptor paths coherent.

---

## Compatibility

`nvme.device` and `nvmeadm` are released together, and the driver/tool ABI is
still evolving. An older driver that lacks the unit-info query is reported by
`nvmeadm` so the matching build can be installed.

---

## Known limitations

- I/O passthrough (`NSCMD_NVME_IO_PASS`), passthrough metadata buffers, and
  protection-information data paths are not implemented.
- Controller Memory Buffer queue placement and Shadow Doorbell Buffer are not
  yet implemented (listed as planned).
- Namespaces formatted for metadata, end-to-end protection, Zoned Namespace,
  Key-Value, or other non-plain-block formats may enumerate at the controller
  level but are not exposed as Amiga storage units.
- `nvmeadm` namespace management (`create-ns` / `delete-ns` / `attach-ns` /
  `detach-ns`) is planned but not available.

See [README.md](README.md) and [README-nvmeadm.md](README-nvmeadm.md) for the
full scope, requirements, and usage details.
