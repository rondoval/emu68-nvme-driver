# CLAUDE.md

Guidance for Claude Code when working in the `nvme.device` driver source.

## What this is

`nvme.device` is the AmigaOS block-storage driver for PCIe NVMe SSDs on PiStorm/Emu68 (Pi 4B/CM4). Its protocol core was ported from the Linux 7.0 NVMe host driver onto the AmigaOS model: Exec tasks/message ports instead of blk-mq/workqueues, `bcmpcie.library` for PCIe, `gic400.library` for MSI, `emu68-common` DMA pools instead of the Linux DMA API.

Exec units map 1:1 to NVMe **namespaces** — a two-namespace drive is two units sharing one controller.

Port history, the `pci.c` harvest inventory, and the open backlog: [`README-internal.md`](README-internal.md) (local only, gitignored — not shipped). User-facing docs: [`README.md`](../../../README.md), [`README-nvmeadm.md`](../../../README-nvmeadm.md), [`RELEASE-NOTES.md`](../../../RELEASE-NOTES.md).

## Build

From the stack root: `cmake --build build --target emu68-nvme-driver` (CMake target `nvme.device`, installs to `DEVS:`; flags and deps in [`CMakeLists.txt`](../../CMakeLists.txt)). Debug defines come via `emu68_debug_definitions()` (sink `EMU68_DEBUG_BACKEND`, tier `EMU68_TIER`) — see emu68-common; the same `CMakeLists.txt` maps the tier onto the `mounter` submodule's own `MOUNTER_LOG`/`MOUNTER_TRACE` switches. ROM-able (`emu68_rom_check`) except the `serial` debug backend. **Always build after C edits and confirm 0 errors / 0 warnings before reporting done.**

## Source layout

**Device layer (`src/`)** — the AmigaOS wrapper: `device*.c` (Open/Close/Expunge/`BeginIO`/`AbortIO`), [`unit.c`](../unit.c) (open/close = refcount bump; controller bringup is at probe time), [`unit_task.c`](../unit_task.c) (per-controller task: drains completions, watchdog, reset/scan/AEN/firmware signals), `admin_task.c`/[`irq.c`](../irq.c) (MSI handler + admin helper task), `unit_commands*.c` (trackdisk/NSCMD/SCSI-emulation dispatch), `mounter/mounter.c` (RDB automount, the A4091 `mounter` fork).

**NVMe core (`src/nvme/`)** — the ported protocol, one file per subsystem: `nvme_probe.c` (discovery/bringup, one `NVMeUnit` per namespace), `nvme_ctrl.c` (lifecycle/registers/state machine), `nvme_admin.c` (queue setup, enable/disable, reset), `nvme_queue.c` (SQ/CQ rings, doorbells, watchdog), `nvme_io.c` (I/O submission, PRPs, sync-waiter idiom), `nvme_completion.c` (CQE disposition), `nvme_identify.c`/`nvme_scan.c` (Identify + namespace scan), `nvme_passthru.c` (admin passthrough), `nvme_fw.c`/`nvme_hmb.c`/`nvme_aen.c`/`nvme_quirks.c`/`nvme_constants.c` (firmware, Host Memory Buffer, async events, quirks, status strings), `kcompat.c` + [`kcompat.h`](../../include/nvme/kcompat.h) (the Linux-API shim — shim new unported Linux calls here).

Headers: `include/devices/nvme.h` (public passthrough ABI) and `include/nvme/nvme_defs.h` (spec defs), both installed; private internals in [`device.h`](../../include/device.h), `nvme/kcompat.h`, `nvme/nvme_io.h`, `nvme/nvme_completion.h`.

## `pci.c` is NOT part of the driver

[`pci.c`](pci.c) is the original Linux PCIe transport, kept verbatim as a read-only reference quarry — not in the build, never compiled into `nvme.device`. Its logic was reimplemented across `nvme_admin.c`, `nvme_queue.c`, `nvme_io.c`, `nvme_probe.c`, and [`irq.c`](../irq.c). Editing `pci.c` does nothing to the driver — consult it only when harvesting more reference logic. Per-function harvest verdict + deletion plan: `README-internal.md`.

## Harvesting more from the Linux source

The upstream source (Linux 7.0.1, `drivers/nvme/host/`) is kept as a local reference checkout outside this repo — reference only, not buildable here (objtool needs `libelf-dev`). Port additional logic into the matching `src/nvme/` file and shim any new Linux calls in `kcompat.h`.
