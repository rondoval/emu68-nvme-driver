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
