// SPDX-License-Identifier: GPL-2.0-only
#ifndef NVME_CTRL_H
#define NVME_CTRL_H

#include <nvme/nvme_core.h>	 /* foundation types, nvme_defs.h (enum nvme_ctrl_type, NVME_CC_*) */
#include <nvme/nvme_queue.h> /* struct nvme_queue embedded in NVMeController */

/* Device/unit framework types live in device.h; referenced here by pointer. */
struct NVMeDevice;
struct pci_dev;

/* The below value is the specific amount of delay needed before checking
 * readiness in case of the PCI_DEVICE(0x1c58, 0x0003), which needs the
 * NVME_QUIRK_DELAY_BEFORE_CHK_RDY quirk enabled. The value (in ms) was
 * found empirically.
 */
#define NVME_QUIRK_DELAY_AMOUNT 2300

/*
 * Per-controller workaround bits for controllers that need behavior the NVMe
 * specification does not define.  The set is gathered when a controller is
 * identified and consulted wherever the affected decision is made.
 *
 * Every bit below records two things: the device problem, and what this driver
 * does about it (the "Driver:" note).  "Driver: nothing to do" means the bit is
 * accepted but needs no action here -- either the driver already always behaves
 * the way the bit asks, or the feature the bit concerns (system suspend,
 * temperature-threshold programming, wide DMA addresses, and the like) does not
 * exist on this platform.
 */
enum nvme_quirks
{
	/*
	 * Prefers I/O aligned to a stripe size specified in a vendor
	 * specific Identify field.
	 *
	 * Driver: read and write transfers are broken into pieces so that no
	 * single command crosses one of these stripe boundaries.
	 */
	NVME_QUIRK_STRIPE_SIZE = (1 << 0),

	/*
	 * The controller doesn't handle Identify value others than 0 or 1
	 * correctly.
	 *
	 * Driver: the range of Identify selector values issued is clamped to
	 * what such a controller accepts.
	 */
	NVME_QUIRK_IDENTIFY_CNS = (1 << 1),

	/*
	 * The controller deterministically returns 0's on reads to
	 * logical blocks that deallocate was called on.
	 *
	 * Driver: a request to zero a range is carried out with a single
	 * deallocate command, which then reads back as zeroes on such a
	 * controller -- faster than writing zeroes.
	 */
	NVME_QUIRK_DEALLOCATE_ZEROES = (1 << 2),

	/*
	 * The controller needs a delay before starts checking the device
	 * readiness, which is done by reading the NVME_CSTS_RDY bit.
	 *
	 * Driver: a fixed delay is inserted before the ready bit is first
	 * polled during start-up.
	 */
	NVME_QUIRK_DELAY_BEFORE_CHK_RDY = (1 << 3),

	/*
	 * Problems seen with concurrent commands.
	 *
	 * Driver: only one I/O command is kept outstanding at a time;
	 * further requests are held and submitted as earlier ones complete.
	 */
	NVME_QUIRK_QDEPTH_ONE = (1 << 6),

	/*
	 * Set MEDIUM priority on SQ creation.
	 *
	 * Driver: the I/O submission queue is created with medium priority,
	 * which stops such a controller from internally treating every queue
	 * as urgent.
	 */
	NVME_QUIRK_MEDIUM_PRIO_SQ = (1 << 7),

	/*
	 * Ignore device provided subnqn.
	 *
	 * Driver: nothing to do -- the device-supplied subsystem name is
	 * never read here, so there is nothing to ignore.
	 */
	NVME_QUIRK_IGNORE_DEV_SUBNQN = (1 << 8),

	/*
	 * Broken Write Zeroes.
	 *
	 * Driver: the Write Zeroes command is treated as unavailable, so a
	 * zeroing request is reported unsupported rather than sent to such a
	 * controller.
	 */
	NVME_QUIRK_DISABLE_WRITE_ZEROES = (1 << 9),

	/*
	 * Force simple suspend/resume path.
	 *
	 * Driver: nothing to do -- there is no system suspend or resume on
	 * this platform; the controller is fully started when first used and
	 * fully stopped when released, which is what this path amounts to.
	 */
	NVME_QUIRK_SIMPLE_SUSPEND = (1 << 10),

	/*
	 * Use only one interrupt vector for all queues.
	 *
	 * Driver: nothing to do -- a single interrupt is always used for both
	 * the admin and I/O queues, and more than one vector is never
	 * requested.
	 */
	NVME_QUIRK_SINGLE_VECTOR = (1 << 11),

	/*
	 * Use non-standard 128 bytes SQEs.
	 *
	 * Driver: entries in the I/O submission queue are spaced 128 bytes
	 * apart instead of 64 (the command itself is still 64 bytes); the
	 * admin queue is left at 64.
	 */
	NVME_QUIRK_128_BYTES_SQES = (1 << 12),

	/*
	 * Prevent tag overlap between queues.
	 *
	 * Driver: nothing to do -- the admin and I/O queues already use
	 * separate command-identifier spaces, and the controllers that need
	 * this are not reachable on this hardware.
	 */
	NVME_QUIRK_SHARED_TAGS = (1 << 13),

	/*
	 * Don't change the value of the temperature threshold feature.
	 *
	 * Driver: nothing to do -- temperature thresholds are never written.
	 */
	NVME_QUIRK_NO_TEMP_THRESH_CHANGE = (1 << 14),

	/*
	 * The controller doesn't handle the Identify Namespace
	 * Identification Descriptor list subcommand despite claiming
	 * NVMe 1.3 compliance.
	 *
	 * Driver: the namespace identification descriptor list is not
	 * requested from such a controller.
	 */
	NVME_QUIRK_NO_NS_DESC_LIST = (1 << 15),

	/*
	 * The controller does not properly handle DMA addresses over
	 * 48 bits.
	 *
	 * Driver: nothing to do -- every address handed to the controller
	 * already fits well within 48 bits.
	 */
	NVME_QUIRK_DMA_ADDRESS_BITS_48 = (1 << 16),

	/*
	 * The controller requires the command_id value be limited, so skip
	 * encoding the generation sequence number.
	 *
	 * Driver: the command identifier is sent as a plain slot number,
	 * omitting the small generation counter this driver would otherwise
	 * pack alongside it to detect stale completions.
	 */
	NVME_QUIRK_SKIP_CID_GEN = (1 << 17),

	/*
	 * Reports garbage in the namespace identifiers (eui64, nguid, uuid).
	 *
	 * Driver: the reported eui64, nguid and uuid identifiers are
	 * discarded.  This bit is also raised on the fly when two namespaces
	 * report the same non-zero identifier.
	 */
	NVME_QUIRK_BOGUS_NID = (1 << 18),

	/*
	 * No temperature thresholds for channels other than 0 (Composite).
	 *
	 * Driver: nothing to do -- temperature thresholds are never written
	 * for any sensor.
	 */
	NVME_QUIRK_NO_SECONDARY_TEMP_THRESH = (1 << 19),

	/*
	 * Disables simple suspend/resume path.
	 *
	 * Driver: nothing to do -- this platform has no suspend or resume
	 * path to turn off.
	 */
	NVME_QUIRK_FORCE_NO_SIMPLE_SUSPEND = (1 << 20),

	/*
	 * MSI (but not MSI-X) interrupts are broken and never fire.
	 *
	 * Driver: message-signalled interrupts are not enabled for such a
	 * controller; legacy pin interrupts are used instead.
	 */
	NVME_QUIRK_BROKEN_MSI = (1 << 21),

	/*
	 * Align dma pool segment size to 512 bytes.
	 *
	 * Driver: the buffers that hold short descriptor lists are aligned to
	 * at least 512 bytes for such a controller.
	 */
	NVME_QUIRK_DMAPOOL_ALIGN_512 = (1 << 22),
};

/* Controller life-cycle states (driver state machine; see
 * nvme_change_ctrl_state in nvme_ctrl.c). */
enum nvme_ctrl_state
{
	NVME_CTRL_NEW,
	NVME_CTRL_LIVE,
	NVME_CTRL_RESETTING,
	NVME_CTRL_CONNECTING,
	NVME_CTRL_DELETING,
	NVME_CTRL_DELETING_NOIO,
	NVME_CTRL_DEAD,
};

enum nvme_ctrl_flags
{
	NVME_CTRL_ADMIN_Q_STOPPED = 0,
	NVME_CTRL_STARTED_ONCE = 1,
	NVME_CTRL_STOPPED = 2,
	NVME_CTRL_SKIP_ID_CNS_CS = 3,
	NVME_CTRL_DIRTY_CAPABILITY = 4,
	NVME_CTRL_FROZEN = 5,
};

/* Identify Controller fields cached on NVMeController for SCSI INQUIRY
 * (and any future diagnostic UI).  Filled in by nvme_init_identify in
 * nvme_ctrl.c.  Sizes match NVMe spec field widths; not NUL-terminated. */
struct NVMeIdentifyStrings
{
	char model[40];	  /* mn[] from Identify Controller */
	char firmware[8]; /* fr[] */
	char serial[20];  /* sn[] */
};

/*
 * Per-controller state — one instance per NVMe PCIe device found by the probe.
 * Merges what Linux split between struct nvme_ctrl and the PCI transport's
 * nvme_dev: hardware resources (tasks, IRQ, memory pool, BAR0) plus the
 * Identify-derived capability/state fields, all reached through one `ctrl`
 * pointer.  Set up on the first UnitOpen for the controller and torn down on
 * its last UnitClose; the struct itself persists until device expunge.
 */
struct NVMeController
{
	/* Controller identity and owner linkage. */
	struct MinNode node; /* Link node in NVMeDevice.controllers; must stay first. */
	struct NVMeDevice *device; /* Owning device base shared by all controllers. */
	struct Library *utilityBase; /* Cached Utility library pointer for helper calls. */

	/* PCIe attachment and interrupt plumbing. */
	struct pci_dev *pci_dev; /* OpenPCI descriptor for this NVMe function. */
	volatile void *bar0; /* BAR0 MMIO base, or NULL while the controller is closed. */
	struct Interrupt irq_isr; /* Exec interrupt server registered for this controller. */
	BOOL msi_enabled; /* TRUE when MSI is active instead of legacy INTx. */
	u32 db_stride; /* Doorbell spacing in bytes, derived from CAP.DSTRD. */

	/* Controller tasks, ports, and signal bits. */
	struct Task *unit_task; /* UnitTask driving the data-path wait loop. */
	BYTE irq_signal; /* Signal bit raised by the ISR to drain completions. */
	BYTE reset_signal; /* Signal bit requesting synchronous controller reset work. */
	struct MsgPort *msgPort; /* Shared I/O request port for all block namespaces. */
	struct MinList io_pending; /* I/O held for back-pressure when io_q is full; FIFO,
	                            * drained by the unit task as completions free slots. */

	struct Task *admin_task; /* AdminWorker handling blocking admin operations. */
	BYTE scan_signal; /* Signal bit requesting namespace rescan work. */
	BYTE fw_act_signal; /* Signal bit requesting firmware-activation follow-up. */
	struct MsgPort *adminPort; /* Passthrough and admin-only request port. */

	/* Shared allocation state. */
	APTR memoryPool; /* Shared pool backing pool_* and dma_* allocations. */
	struct slab_cache req_slab; /* Slab cache for struct nvme_request objects. */
	struct slab_cache ctx_slab; /* Slab cache for struct nvme_io_context objects. */
	struct slab_cache prp_large_slab; /* Slab cache for 4 KiB PRP-list pages (chained / >32-entry lists). */
	struct slab_cache prp_small_slab; /* Slab cache for 256 B PRP-list pages (≤32-entry lists; the common case). */

	/* Runtime queue state. */
	struct nvme_queue admin_q; /* Admin submission and completion queue pair. */
	struct nvme_queue io_q; /* Single I/O submission and completion queue pair. */
	struct MinList retry_list; /* Deferred CRDT retries waiting for resubmission. */
	u32 queue_count; /* Queue count granted or configured on the controller. */
	u32 abort_limit; /* Remaining abort budget before escalating to reset. */

	/* Namespace tracking and controller-wide locks. */
	struct SignalSemaphore scan_lock; /* Serialises namespace scan and rescan work. */
	struct MinList namespaces; /* Live nvme_ns objects discovered on this controller. */
	struct SignalSemaphore namespaces_lock; /* Protects namespace list mutations. */
	ULONG nsCount; /* Number of namespace units exposed by probe. */
	ULONG nsids[NVME_MAX_NS]; /* Active NSID list cached from namespace scan. */
	ULONG firstUnitNumber; /* Global Amiga unit number assigned to nsids[0]. */
	ULONG openUnits; /* Number of namespace units currently opened by clients. */

	/* High-level controller state and cached identity strings. */
	BOOL identified; /* TRUE once Identify Controller data has been cached. */
	enum nvme_ctrl_state state; /* Current driver lifecycle state for the controller. */
	enum nvme_ctrl_type cntrltype; /* Controller type reported by Identify. */
	struct NVMeIdentifyStrings id_strings; /* Cached serial, model, and firmware strings. */
	struct nvme_effects_log *effects; /* Cached Command Effects Log, if allocated. */
	u32 aen_result; /* Last AEN payload cached for follow-up handling. */
	unsigned long events; /* Pending async-event bits raised by AEN processing. */

	/* Host Memory Buffer state. */
	APTR hmb_descs; /* DMA-visible descriptor table for Host Memory Buffer chunks. */
	ULONG hmb_nr_descs; /* Number of valid entries in hmb_descs[]. */
	ULONG hmb_size; /* Total Host Memory Buffer size currently enabled. */
	u16 hmmaxd; /* Maximum HMB descriptor count from Identify Controller. */
	u32 hmpre; /* Preferred HMB size in controller-page units. */
	u32 hmmin; /* Minimum HMB size in controller-page units. */
	u32 hmminds; /* Minimum per-descriptor HMB chunk size in page units. */

	/* Cached capability, feature, and limit fields. */
	u64 cap; /* Raw CAP register cached during bring-up. */
	u32 vs; /* Raw VS register cached after controller enable. */
	u32 ctrl_config; /* Shadow copy of the programmed CC register value. */
	u16 cntlid; /* Controller identifier from Identify Controller. */
	u16 sqsize; /* Maximum I/O SQ size exposed to queue setup. */
	u16 io_max_inflight; /* Max simultaneous in-flight I/O commands (>=1): io_q.depth-1, or 1 under NVME_QUIRK_QDEPTH_ONE. Cached by nvme_setup_io_queue; gates dispatch/back-pressure and caps chunk siblings. */
	u32 max_transfer_bytes; /* Maximum payload size accepted for one I/O command. */
	u32 max_hw_sectors; /* Hardware max transfer in 512-byte sectors. */
	u32 max_zeroes_sectors; /* Hardware max write-zeroes transfer in sectors. */
	u32 max_namespaces; /* Maximum namespaces reported by the controller. */
	u16 crdt[3]; /* Cached command retry delay time values. */
	u16 mtfa; /* Maximum firmware activation time from Identify. */
	u16 oncs; /* Optional NVM command support bits from Identify. */
	u8 dmrl; /* Dataset Management range limit from NVM Identify. */
	u32 dmrsl; /* Dataset Management range size limit from NVM Identify. */
	u16 oacs; /* Optional admin command support bits from Identify. */
	u8 vwc; /* Volatile write cache capabilities from Identify. */
	u32 sgls; /* SGL capability bits from Identify. */
	u16 wctemp; /* Warning composite temperature threshold. */
	u16 cctemp; /* Critical composite temperature threshold. */
	u32 oaes; /* Supported asynchronous event mask from Identify. */
	u32 ctratt; /* Controller attributes bitmask from Identify. */
	unsigned int shutdown_timeout; /* Shutdown timeout in seconds for CC.SHN. */

	/* Software policy and quirk flags. */
	unsigned long quirks; /* Quirk bits affecting transport and command behaviour. */
	unsigned long flags; /* Internal controller runtime flags. */
};

/* ------------------------------------------------------------------ */
/* Inline register / state accessors                                   */
/* ------------------------------------------------------------------ */

/*
 * Controller register accessors.
 *
 * NVMe spec §3.1 places the controller's registers in BAR0 (little-endian
 * on the wire; emu68-common's mmio_read32/mmio_write32 byte-swap to
 * native u32).  64-bit registers are accessed as two adjacent 32-bit
 * reads, low dword first — some controllers stall on a single 64-bit
 * transaction.
 *
 * These replace the old struct nvme_ctrl_ops vtable indirection.  There
 * is only one transport (PiStorm BAR0 MMIO) and the operations never
 * fail, so no out-params or return codes.
 */
static inline u32 nvme_reg_read32(struct NVMeController *ctrl, u32 off)
{
	return mmio_read32((volatile UBYTE *)ctrl->bar0 + off);
}

static inline void nvme_reg_write32(struct NVMeController *ctrl, u32 off, u32 val)
{
	mmio_write32(val, (volatile UBYTE *)ctrl->bar0 + off);
}

static inline u64 nvme_reg_read64(struct NVMeController *ctrl, u32 off)
{
	u32 lo = nvme_reg_read32(ctrl, off);
	u32 hi = nvme_reg_read32(ctrl, off + 4);
	return ((u64)hi << 32) | lo;
}

/*
 * nvme_ctrl_state - read the controller's current state
 *
 * @ctrl: NVMe controller
 * Returns: current enum nvme_ctrl_state value
 */
static inline enum nvme_ctrl_state nvme_ctrl_state(struct NVMeController *ctrl)
{
	return ctrl->state;
}

/*
 * nvme_state_terminal - test whether the controller is in a terminal state
 *
 * Returns TRUE for states that are permanent failure or deletion sinks from
 * which the controller state machine cannot return to LIVE.  Used by error
 * paths to avoid scheduling recovery work on a controller that is already
 * being torn down.
 *
 * @ctrl: NVMe controller
 * Returns: TRUE if the state is DELETING, DELETING_NOIO, or DEAD
 */
static inline BOOL nvme_state_terminal(struct NVMeController *ctrl)
{
	switch (nvme_ctrl_state(ctrl))
	{
	case NVME_CTRL_NEW:
	case NVME_CTRL_LIVE:
	case NVME_CTRL_RESETTING:
	case NVME_CTRL_CONNECTING:
		return FALSE;
	case NVME_CTRL_DELETING:
	case NVME_CTRL_DELETING_NOIO:
	case NVME_CTRL_DEAD:
		return TRUE;
	default:
		Kprintf("[nvme] %s: Unhandled ctrl state:%d\n", __func__, nvme_ctrl_state(ctrl));
		return TRUE;
	}
}

/*
 * nvme_multi_css - test whether the controller is configured for multiple command sets
 *
 * Returns TRUE when the CC.CSS field is set to the "I/O Command Set supported"
 * value (NVME_CC_CSS_CSI), meaning the controller may expose namespaces using
 * command sets other than NVM (e.g., Zoned Namespace or Key-Value).  Used to
 * gate CSI-specific namespace identification paths.
 *
 * @ctrl: NVMe controller
 * Returns: TRUE if multiple command sets are active
 */
static inline BOOL nvme_multi_css(struct NVMeController *ctrl)
{
	return (ctrl->ctrl_config & NVME_CC_CSS_MASK) == NVME_CC_CSS_CSI;
}

/*
 * nvme_admin_ctrl - check whether a controller is administration-only
 * (no I/O namespaces).
 */
static inline BOOL nvme_admin_ctrl(struct NVMeController *ctrl)
{
	return ctrl->cntrltype == NVME_CTRL_ADMIN;
}

/*
 * nvme_is_io_ctrl - check whether a controller can host I/O namespaces.
 */
static inline BOOL nvme_is_io_ctrl(struct NVMeController *ctrl)
{
	return !nvme_admin_ctrl(ctrl);
}

/* ------------------------------------------------------------------ */
/* Out-of-line entry points (defined in nvme_ctrl.c)                   */
/* ------------------------------------------------------------------ */

/*
 * nvme_change_ctrl_state - attempt a controller state machine transition
 *
 * Checks whether new_state is reachable from the current state and, if so,
 * commits ctrl->state.  The allowed transitions mirror the NVMe spec lifecycle:
 * NEW→CONNECTING→LIVE, LIVE/RESETTING/CONNECTING→DELETING, etc.
 *
 * @ctrl:      controller whose state to change
 * @new_state: desired new state
 * Returns: TRUE if the transition succeeded, FALSE if it was not allowed
 */
BOOL nvme_change_ctrl_state(struct NVMeController *ctrl,
							enum nvme_ctrl_state new_state);

/*
 * nvme_init_identify - populate controller capabilities from Identify
 * Controller; init the effects log on first call.
 */
int nvme_init_identify(struct NVMeController *ctrl);

/*
 * nvme_init_non_mdts_limits - read NVM Command Set controller limits
 * (write-zeroes max, DSM ranges) from Identify Controller (CNS=0x06).
 */
int nvme_init_non_mdts_limits(struct NVMeController *ctrl);

#endif /* NVME_CTRL_H */
