# nvmeadm — NVMe Admin Utility for nvme.device

`nvmeadm` is the command-line administration and diagnostics utility for
NVMe SSDs driven by `nvme.device`. It reports drive identity, health, and
logs, runs device self-tests, and performs maintenance operations such as
firmware updates, namespace formatting, and sanitize.

It installs as `C:nvmeadm` together with `nvme.device`. The tool and the
driver are released together — if `nvmeadm` reports that the driver lacks
the unit-info query, install the matching `nvme.device` build.

> Warning: the `fw-download`, `fw-activate`, `format`, and `sanitize`
> commands modify the drive. `format` and `sanitize` DESTROY DATA. Read
> their sections before use and keep backups.

---

## Units, namespaces, and controllers

Every `nvme.device` unit is one NVMe **namespace**. A drive (controller)
with two namespaces shows up as two units — so units 0 and 1 may be the
same physical drive. Commands that talk to the whole drive (`smart`,
`error-log`, `sanitize`, firmware commands, …) address the controller that
owns the selected unit; commands that talk to a namespace (`identify ns`,
`format`) operate on the selected unit's own namespace.

`UNIT <n>` selects the unit everywhere and defaults to 0. To see what is
installed:

```
> nvmeadm units
Controller at unit 0: Samsung SSD 980 500GB (S/N S5XXNX0R123456, FW 2B4QFXO7, NVMe 1.4)
  Unit 0:  NSID 1  sector=512
```

## General usage

```
nvmeadm <command> [subcommand] [action] [options]
```

- Commands and keywords are case-insensitive; keywords accept both
  `UNIT 1` and `UNIT=1` forms.
- `nvmeadm ?` shows the AmigaDOS argument template; `nvmeadm` with an
  unknown or incomplete command prints usage, and commands with options
  (`sanitize`, `format`, `fw-activate`) print a detailed help page.
- Return codes follow AmigaDOS conventions: 0 success, 10 bad arguments
  (`WHY` explains), 20 operation failed.
- Long-running operations (firmware download, listings) can be aborted
  with Ctrl-C.

---

## Information commands

### `nvmeadm units`

Lists all units, grouped by controller, with model, serial, firmware
revision, NVMe version, namespace ID, and sector size. Start here.

### `nvmeadm identify [UNIT <n>]`

Short Identify Controller summary: vendor ID, serial number, model, and
firmware revision.

### `nvmeadm identify caps [UNIT <n>]`

Decoded controller capability report: controller type, multipath flags,
transfer and queue limits (including the exact maximum transfer size),
admin and NVM command capabilities, log/event support, controller
attributes, Host Memory Buffer fields, and command-set support.

### `nvmeadm identify ns [UNIT <n>]`

Identify Namespace data for the unit's namespace: size, capacity,
utilization, current LBA format, metadata and protection-information
settings, readiness, and the full LBA format table. Formats are marked
`exposed` / `not-exposed` according to whether `nvme.device` would present
them as an Amiga block device — check this before using `format`.

### `nvmeadm list-ns [UNIT <n>]`

Lists the active namespaces on the unit's controller with sector size,
metadata, protection info, and a usability verdict per namespace.

### `nvmeadm smart [UNIT <n>]`

SMART / health log: critical warnings, temperature (including sensors and
thermal-management statistics), spare capacity, percentage used, data
read/written, power-on hours, unsafe shutdowns, and media errors.

### `nvmeadm get-feature [UNIT <n>]`

Current values of common controller features (power management,
temperature threshold, volatile write cache, queue counts, and others),
with lightweight decoding.

## Log commands

### `nvmeadm error-log [UNIT <n>]`

Error Information log. Only meaningful entries are decoded in full; stale
or empty slots are summarized in one line. Entries not associated with a
specific command are marked as such.

### `nvmeadm fw-log [UNIT <n>]`

Firmware Slot Information: active slot, pending next-reset slot, and the
firmware revision stored in each populated slot.

### `nvmeadm effects-log [UNIT <n>]`

Command Effects log for selected admin and I/O commands — shows whether a
command changes logical block content or namespace capabilities.

### `nvmeadm changed-ns [UNIT <n>]`

Changed Namespace List log.

## Self-test

```
nvmeadm self-test status [UNIT <n>]
nvmeadm self-test start short [UNIT <n>]
nvmeadm self-test start extended [UNIT <n>]
nvmeadm self-test abort [UNIT <n>]
```

`status` shows the running test (with completion percentage) and the
recorded results of previous tests. Short tests typically finish within a
couple of minutes; extended tests can take much longer and run in the
background on the drive.

## Firmware update

Firmware update is a two-step process; nothing takes effect until the
image is committed, and most drives additionally require a reset (usually
a reboot) afterwards.

### `nvmeadm fw-download FILE <path> [UNIT <n>] CONFIRM`

Transfers a firmware image file to the drive in chunks. The file size must
be a multiple of 4 bytes. Prints a transfer summary before asking for
`CONFIRM`. Aborting mid-download (Ctrl-C) is safe — the image is only used
once committed.

### `nvmeadm fw-activate <replace|replace-activate|activate> UNIT <n> SLOT <n> CONFIRM`

Commits firmware to a slot:

- `replace` — store the downloaded image in SLOT without activating it
- `replace-activate` — store the image in SLOT and activate it on the next
  reset
- `activate` — activate the image already stored in SLOT on the next reset

Check `fw-log` afterwards; if activation is deferred, it shows the pending
slot until the controller is reset.

## Destructive maintenance

Both commands print a full summary of what will be submitted and refuse to
run without the `CONFIRM` switch.

### `nvmeadm format LBAF <n> [SES <0|1|2>] [UNIT <n>] CONFIRM`

Formats the unit's namespace to the LBA format index `LBAF` (as listed by
`identify ns`). **All data in the namespace is destroyed.**

SES selects the secure-erase mode:

- `0` — no secure erase (default)
- `1` — user data erase
- `2` — cryptographic erase (destroys the media encryption key)

`nvmeadm` refuses to format into an LBA format that `nvme.device` would
not expose as a usable Amiga block device (e.g. formats with metadata or
protection information).

### `nvmeadm sanitize <action> [UNIT <n>] [options] CONFIRM`

Starts a sanitize operation on the unit's controller. **Sanitize affects
the entire drive — every namespace — and destroys all data.**

Actions:

- `block-erase` — erase all blocks (typically fast; recommended)
- `crypto` — destroy the media encryption key (instant; needs drive
  support)
- `overwrite` — overwrite the media with a pattern (slow)
- `exit-failure` — recover a drive stuck in the sanitize-failed state

Options:

- `AUSE` — Allow Unrestricted Sanitize Exit: the drive stays usable if the
  sanitize fails (default: restricted — the drive blocks I/O until a
  successful sanitize or `exit-failure`)
- `OWPASS <0-15>` — overwrite pass count (0 or omitted = 16 passes;
  overwrite only)
- `OIPBP` — invert the overwrite pattern between passes (overwrite only)
- `NODAS` — no deallocate after sanitize

Run `sanitize-status` first to see which actions the drive supports, and
afterwards to track progress — sanitize keeps running on the drive after
the command is accepted.

### `nvmeadm sanitize-status [UNIT <n>]`

Shows the drive's sanitize capabilities (block erase / crypto erase /
overwrite support) and the current status: never sanitized, in progress
(with a progress percentage), completed, or failed, plus estimated times
per action where the drive reports them.

---

## Planned

Under consideration, not yet available:

- **Namespace management** — `create-ns`, `delete-ns`, `attach-ns`, and
  `detach-ns` to repartition a drive at the namespace level. Deferred behind a
  design decision (how to target a namespace by ID, given every other command
  works on the opened unit's own namespace) and validation on real
  multi-namespace hardware. `delete-ns` will be destructive and gated by
  `CONFIRM` like `format` and `sanitize`.

`set-feature` and any other write-capable feature commands remain deliberately
excluded for now. I/O passthrough, metadata transfers, large telemetry dumps,
Security Send/Receive, and reservations are out of scope for this tool, not
planned work.

---

## Troubleshooting

- `OpenDevice(nvme.device,N) failed` — the unit does not exist; run
  `nvmeadm units` (or check that `nvme.device` loaded at boot).
- `nvme.device lacks the unit-info query; update the driver` — the
  installed `nvme.device` is older than this `nvmeadm`; install the
  matching driver build.
- Admin command failures print the NVMe status and result codes
  (`status=0x.. result=0x..`); these identify the controller's reason for
  rejecting a command.
