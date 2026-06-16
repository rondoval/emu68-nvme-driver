# emu68-nvme-driver

`emu68-nvme-driver` provides `nvme.device`, an AmigaOS block-storage driver for NVMe SSDs on
Emu68 systems using the CM4 PCIe path.

The driver is ported from the Linux 7.0 NVMe host driver and then heavily adapted to AmigaOS:
Linux blk-mq, sysfs, and kernel service layers are replaced with Amiga tasks, message ports,
Exec memory management, and an Amiga-facing block-device API.

> Warning: this is still a new driver. Data loss, corruption, and other storage failures are still
> possible. Use it at your own risk and keep current backups of anything you care about.

---

## Scope

`nvme.device` is intended as a local PCIe NVMe block driver for Emu68 systems, not as a full port
of the entire Linux NVMe subsystem.

The best-supported devices are consumer or datacenter PCIe SSDs whose active namespaces are plain
NVM block namespaces with:

- command set identifier `NVM`
- no metadata payload
- no protection information
- 512-byte, 1 KiB, 2 KiB, or 4 KiB logical block sizes

DRAM-less SSDs that advertise Host Memory Buffer can also work; the driver will attempt to
configure HMB when the controller exposes it and the requested allocation fits within the Amiga
system's available memory budget.

The following areas are intentionally outside the scope of this driver:

- NVMe over Fabrics transports other than PCIe, including TCP, RDMA, Fibre Channel, and loop
- NVMe target mode
- non-NVM command sets such as Zoned Namespace and Key-Value storage
- Linux block-layer integration such as blk-mq request queues, tag sets, and block queue policy
- sysfs, hwmon, uevents, and other Linux kernel object / filesystem reporting surfaces
- Linux ioctl-based control paths
- host multipath and ANA plumbing as implemented by the Linux NVMe subsystem
- multi-controller namespace management as implemented by the Linux NVMe subsystem

---

## Requirements

- AmigaOS 3.x running under Emu68
- PiStorm accelerator with CM4
- Emu68 exposing the Raspberry Pi PCIe path needed for NVMe access
- `gic400.library` in `LIBS:` for interrupt delivery
- `bcmpcie.library` in `LIBS:` for BCM2711 PCIe bring-up, BAR assignment, and MSI support
- an NVMe SSD reachable through the BCM2711 PCIe controller

For building from source you also need:

- Bebbo's m68k AmigaOS cross toolchain in `/opt/m68k-amigaos`
- a common install prefix containing the companion packages used by this driver stack

---

## Installation

If you install from the packaged driver stack output, the relevant files are:

| File | Destination |
|---|---|
| `nvme.device` | `DEVS:` |
| `nvmeadm` | `C:` |

Runtime companion files that must already be present:

| File | Destination |
|---|---|
| `gic400.library` | `LIBS:` |
| `bcmpcie.library` | `LIBS:` |

`nvme.device` is a local storage driver. It does not require a separate filesystem-specific
configuration file, but it does depend on the PCIe and interrupt libraries above being installed
first.

For RDB-based automount and autoboot handling, the driver uses the
[`mounter`](https://github.com/A4091/mounter) code from the A4091 project.

---

## Features

### Core driver functionality

- PCIe-attached NVMe controller discovery and initialization through `bcmpcie.library`
- controller reset, reinitialization, and basic failure recovery
- MSI-backed interrupt handling through `gic400.library`
- admin queue plus I/O queue operation adapted to the AmigaOS task model
- namespace discovery for supported NVM namespaces
- namespace filtering so only plain block namespaces are exposed as Amiga units
- support for namespaces using 512-byte, 1 KiB, 2 KiB, and 4 KiB logical block sizes
- device-specific quirk handling carried over from Linux where it is relevant to this port
- Host Memory Buffer setup for DRAM-less controllers that expose HMB capability

### Block I/O functionality

- `CMD_READ`, `CMD_WRITE`, `TD_READ64`, `TD_WRITE64`, and newstyle 64-bit read/write commands
- `TD_FORMAT` and `TD_FORMAT64` compatibility through the normal write path
- native `NSCMD_NVME_WRITE_ZEROES` support for controller-backed zero-fill requests
- native `NSCMD_NVME_TRIM` support for explicit logical-block deallocate requests
- flush / cache synchronization
- discard / trim support through both `NSCMD_NVME_TRIM` and `HD_SCSICMD` SCSI UNMAP translation to NVMe Dataset Management
- PRP-based data transfer handling, including multi-page transfers
- internal bounce-buffer staging when the caller's buffer is not directly DMA-safe


Discard is not exposed as a standard trackdisk command. Native callers may use the
private `NSCMD_NVME_TRIM` command from `devices/nvme.h`, where `io_Data` points to an array of
`struct NVMeTrimRange` entries expressed in logical blocks of the unit's sector size. Callers that
start from byte ranges should first query `TD_GETGEOMETRY.dg_SectorSize` and convert bytes to LBAs
and block counts. The current native trim ABI accepts up to 256 ranges per request. Generic
storage clients can also use the SCSI emulation path: issue SCSI UNMAP via `HD_SCSICMD`, and the
driver translates that into NVMe Dataset Management / Deallocate.

### Controller and media diagnostics

- admin-command passthrough for userland tooling through `NSCMD_NVME_ADMIN_PASS`
- drive identity, health (SMART), logs, self-tests, firmware updates, and
  maintenance are surfaced through the bundled `nvmeadm` tool — see
  [README-nvmeadm.md](README-nvmeadm.md)

### Internal controller behavior

- asynchronous event handling for namespace-change, firmware-activation, SMART/error, and persistent-error notifications

Async Event Notification is used internally by the driver. Namespace-change notices trigger a
rescan, firmware-activation notices trigger the reset path, and persistent internal error events
trigger controller recovery. This is support for NVMe AEN handling inside the driver, not a broad
AmigaOS event-notification API.

---

## Limitations

The following areas are not currently implemented in `nvme.device`:

- I/O passthrough through `NSCMD_NVME_IO_PASS`
- metadata-buffer handling in the public passthrough ABI
- protection information data paths
- Controller Memory Buffer queue placement and related CMB optimizations
- the broader Linux timeout and error-recovery machinery beyond the Amiga-specific restart model

Namespaces are intentionally not exposed as Amiga block units when any of the following is true:

- the namespace command set is not `NVM`
- the active LBA format carries metadata
- the namespace uses protection information
- the active logical block size is outside 512-byte through 4 KiB support

That means drives whose namespaces are formatted for metadata, end-to-end protection, Zoned
Namespace operation, Key-Value storage, or other non-plain-block formats may still enumerate at the
controller level, but those namespaces will not be exposed as usable Amiga storage units.

---

## Planned features

These are under consideration rather than committed; none is required for the driver to function,
and there is no fixed timeline:

- **Controller Memory Buffer (CMB) queue placement** — placing the I/O submission queue in
  controller-side memory.
- **Shadow Doorbell Buffer** — when the controller advertises Doorbell Buffer Config support
  (`OACS.DBBUF`), keep shadow doorbell and event-index buffers in host memory so most MMIO
  doorbell writes (costly on the PiStorm PCIe path) can be elided. Only effective on controllers
  that support it; `nvmeadm identify` reports whether a given drive does.
- **`nvmeadm` namespace management** — `create-ns` / `delete-ns` / `attach-ns` / `detach-ns`, so
  drives can be repartitioned at the namespace level from AmigaOS.

The broader machinery intentionally left out of scope (NVMe over Fabrics, target mode, non-NVM
command sets) is not on this list and is not planned.

---

## Included Tools

Two CLI tools are built and installed with the component:

- `nvmeadm` is the release-facing administration and diagnostics utility — drive
  identification, health reporting, logs, self-tests, firmware updates, and
  maintenance (format, sanitize). See [README-nvmeadm.md](README-nvmeadm.md) for
  the full user guide.
- `nvmeinfo` is a lower-level helper and development tool for passthrough-oriented
  inspection.

The `nvmeadm` subcommand surface is intentionally narrower than Linux `nvme-cli`:
the goal is the most useful local diagnostics first, without inventing new driver
ABI just for userland reporting.

---

## Building

Build the component standalone against a populated install prefix:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/emu68-driver-stack/install
cmake --build build
cmake --install build
```

From the superbuild root, the usual validation target is:

```sh
cmake --build build --target emu68-nvme-driver
```

The install step places `nvme.device` in `DEVS:` and the utilities in `C:` inside the install
tree produced by the driver stack.

---

## Notes On Design

This driver is a Linux port, but it is not a Linux compatibility layer pretending to be a full
kernel NVMe subsystem. The transport, queueing, interrupt, and memory-management pieces have been
reshaped around AmigaOS constraints:

- controller work is driven by Amiga tasks and message ports rather than Linux workqueues and blk-mq
- DMA-capable buffers use the common Emu68 allocation helpers
- PCIe access goes through `bcmpcie.library`
- interrupt delivery goes through `gic400.library`
- user-facing diagnostics are exposed through Amiga device commands and small CLI tools, not sysfs

That keeps the code close enough to Linux to reuse mature NVMe logic where it helps, while still
behaving like an AmigaOS driver package.
