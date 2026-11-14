// LikeOS - USB Mass Storage (Bulk-Only Transport) Driver
//
// A USB stick (or USB disk, card reader, USB optical drive) is a SCSI device
// reached through the Bulk-Only Transport: every command is a Command Block
// Wrapper on the bulk OUT endpoint, an optional data stage, and a Command
// Status Wrapper on the bulk IN endpoint.  The driver presents the device to
// the disk layer (kernel/dev/block/blkdev.h) as a disk like any other --
// sdX with partitions sdXN (or srN for an optical drive), removable -- and
// executes the requests of its queue on a kernel thread of its own
// ("kusbmsd<N>"), because a BOT transfer sleeps and the disk layer's submit
// path must not.  SG_IO pass-through of SCSI CDBs goes through the same
// thread, in order with ordinary I/O.
//
// Devices come and go at any time.  A stick present at boot is attached by
// the boot enumeration itself (the root filesystem may be on it); one
// plugged in later is enumerated by the xHCI hot-plug poll and attached by
// the attach thread ("kusbstor"), which may sleep for as long as the device
// takes to become ready.  Every logical unit (GET MAX LUN) is a disk of its
// own.  When a device is unplugged, its transport fails every command at
// once, the disks are marked gone (their I/O fails with EIO from then on)
// and deleted -- /dev nodes, /dev/disk links and /sys/block entries go --
// and their memory is released once the last opener or mount lets go.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef LIKEOS_USB_MSD_H
#define LIKEOS_USB_MSD_H

#include <kernel/uapi/types.h>
#include <kernel/uapi/status.h>
#include <kernel/dev/usb/xhci.h>
#include <kernel/dev/block/block.h>
#include <kernel/ke/sched.h>

struct gendisk;
struct blk_request;

// Requests the driver accepts at once per disk (the disk queue's depth)
#define USB_MSD_QUEUE_DEPTH 8
// SCSI host numbers of USB disks start here, after the ATA ports
#define USB_MSD_SCSI_HOST_BASE 32
// Largest SG_IO data transfer accepted (bytes)
#define USB_MSD_SGIO_MAX_BYTES (1024u * 1024u)

// USB Mass Storage class codes
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_SUBCLASS_SCSI 0x06
#define USB_PROTOCOL_BOT 0x50

// SCSI opcodes
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_SYNCHRONIZE_CACHE_10 0x35
#define SCSI_READ_16 0x88
#define SCSI_WRITE_16 0x8A
#define SCSI_SYNCHRONIZE_CACHE_16 0x91
#define SCSI_SERVICE_ACTION_IN_16 0x9E
#define SCSI_SAI_READ_CAPACITY_16 0x10

// Bulk-Only class requests
#define USB_MSD_REQ_RESET 0xFF // Bulk-Only Mass Storage Reset
#define USB_MSD_REQ_GET_MAX_LUN 0xFE

// Command Block Wrapper (CBW) - 31 bytes
#define CBW_SIGNATURE 0x43425355 // "USBC"
#define CBW_SIZE 31
#define CBW_FLAG_DATA_IN 0x80
#define CBW_FLAG_DATA_OUT 0x00

typedef struct __attribute__((packed)) {
	uint32_t signature; // 0x43425355 = "USBC"
	uint32_t tag; // Transaction tag
	uint32_t data_transfer_len; // Number of bytes to transfer
	uint8_t flags; // Direction: 0x80=in, 0x00=out
	uint8_t lun; // Logical Unit Number
	uint8_t cb_length; // Command block length (1-16)
	uint8_t cb[16]; // Command block (SCSI CDB)
} usb_msd_cbw_t;

// Command Status Wrapper (CSW) - 13 bytes
#define CSW_SIGNATURE 0x53425355 // "USBS"
#define CSW_SIZE 13
#define CSW_STATUS_PASSED 0
#define CSW_STATUS_FAILED 1
#define CSW_STATUS_PHASE_ERROR 2

typedef struct __attribute__((packed)) {
	uint32_t signature; // 0x53425355 = "USBS"
	uint32_t tag; // Same as CBW tag
	uint32_t data_residue; // Difference between expected and actual data
	uint8_t status; // 0=passed, 1=failed, 2=phase error
} usb_msd_csw_t;

// SCSI Inquiry response
typedef struct __attribute__((packed)) {
	uint8_t device_type; // Peripheral device type
	uint8_t rmb; // Removable media bit
	uint8_t version; // SCSI version
	uint8_t response_format; // Response data format
	uint8_t additional_len; // Additional length
	uint8_t reserved[3];
	char vendor[8]; // Vendor identification
	char product[16]; // Product identification
	char revision[4]; // Product revision
} scsi_inquiry_data_t;

// SCSI Read Capacity (10) response
typedef struct __attribute__((packed)) {
	uint32_t last_lba; // Last logical block address (big-endian)
	uint32_t block_size; // Block size in bytes (big-endian)
} scsi_read_capacity_data_t;

// SCSI Request Sense response
typedef struct __attribute__((packed)) {
	uint8_t response_code;
	uint8_t segment;
	uint8_t sense_key;
	uint8_t information[4];
	uint8_t additional_len;
	uint8_t reserved[4];
	uint8_t asc; // Additional sense code
	uint8_t ascq; // Additional sense code qualifier
	uint8_t reserved2[4];
} scsi_sense_data_t;

// Hosts (USB mass-storage interfaces) and logical units (disks) at once
#define USB_MSD_MAX_HOSTS 8
#define USB_MSD_MAX_LUNS 8 // per host (GET MAX LUN answers up to 15)
#define USB_MSD_MAX_DEVICES 16 // logical units of all hosts
// Requests of all LUNs of a host waiting for its worker (room for every
// unit's full queue depth: a full ring would leave a disk with nothing in
// flight and its queue retrying without pause)
#define USB_MSD_RING_SIZE (USB_MSD_MAX_LUNS * USB_MSD_QUEUE_DEPTH)

// Host (transport) states
#define USB_MSD_HOST_FREE 0 // the slot is unused
#define USB_MSD_HOST_ATTACHING 1 // being probed (attach thread / boot)
#define USB_MSD_HOST_LIVE 2 // its disks are registered
#define USB_MSD_HOST_GONE 3 // detached; waits for its disks to be released

// Logical unit states
#define USB_MSD_LUN_FREE 0
#define USB_MSD_LUN_BOUND 1 // belongs to a host (probed, maybe registered)
#define USB_MSD_LUN_RETIRED 2 // a legacy registration: never reused

struct usb_msd_device;

// One USB mass-storage interface: the Bulk-Only Transport to it, shared by
// its logical units.  The slots are static: a slot's worker thread, locks
// and DMA buffers live for as long as the system, and a slot is reused only
// once the device it served is detached and the last of its disks released.
typedef struct usb_msd_host {
	// --- the slot (set up once, kept across devices) ---
	int index; // slot in g_msd_hosts[], the worker's number
	uint8_t slot_ready; // locks initialised, worker started
	task_t *worker; // the "kusbmsd<N>" thread, NULL if it could not start
	volatile int worker_running;

	// Per-host sleeping mutex serialising the transport (one BOT command
	// at a time).  A spinlock is inappropriate here: USB transfers take
	// milliseconds on real hardware, and holding a spinlock with IRQs
	// disabled that long blocks TLB shootdown ACKs and starves other CPUs.
	volatile int io_locked; // 0 = free, 1 = held
	volatile uint64_t
		io_owner; // task id of holder (-1 = none); for orphan-lock recovery
	spinlock_t io_wait_lock; // protects the sleep/wake race

	// Persistent DMA staging buffers, allocated once and reused for every
	// command (and every later device of the slot): the contiguous
	// allocator is an O(total_pages) bitmap scan under a global lock with
	// IRQs disabled, far too slow for the I/O path.  The transport is
	// serialised by the I/O mutex, so one set per host is enough.  NULL
	// pointers mean "fall back to per-command allocation".
	uint8_t *cbw_buf; // page-aligned CBW staging buffer (one page)
	uint8_t *csw_buf; // page-aligned CSW staging buffer (one page)
	uint8_t *ctl_buf; // page-aligned control-transfer buffer (one page)
	uint8_t *cbw_buf_raw; // raw allocations backing them
	uint8_t *csw_buf_raw;
	uint8_t *ctl_buf_raw;
	uint8_t *data_buf; // virtual base of contiguous bounce buffer
	uint64_t data_buf_phys; // physical base of bounce buffer (0 = none)
	uint32_t data_buf_size; // usable size of bounce buffer in bytes
	uint32_t data_buf_pages; // page count backing the bounce buffer

	// Request execution: requests the disk layer handed to submit wait in
	// this ring until the worker runs them (BOT is one command at a time;
	// the ring lets the next request be queued while one runs).
	spinlock_t rq_lock; // protects rq_ring/rq_head/rq_count; leaf, irqsave
	struct blk_request *rq_ring[USB_MSD_RING_SIZE];
	uint32_t rq_head; // index of the oldest queued request
	uint32_t rq_count; // queued, not yet taken by the worker
	volatile int rq_wait; // wait channel of the idle worker
	uint8_t inline_running; // a caller runs the ring itself (no worker)
	struct blk_request *rq_active; // the request the worker executes

	// --- the device it serves (reset at every attach) ---
	volatile int state; // USB_MSD_HOST_* (g_msd_lock)
	// The transport is dead (unplugged, or the device stopped answering):
	// every command fails at once without touching the controller.  Set
	// before the fence on the I/O mutex in usb_msd_disconnect().
	volatile int gone;
	volatile int failed; // gone because the device stopped answering
	volatile int detach_pending; // the attach thread is to detach it
	int refs; // the attachment + each bound logical unit (g_msd_lock)
	usb_device_t *usb_dev; // NULL once gone and fenced
	xhci_controller_t *ctrl;
	uint32_t usb_gen; // usb_dev->gen at attach
	uint8_t iface; // bInterfaceNumber of the BOT interface
	uint8_t bulk_in_addr; // endpoint addresses (for CLEAR_FEATURE)
	uint8_t bulk_out_addr;
	uint8_t max_lun; // GET MAX LUN
	uint8_t port; // root port (for messages)
	uint16_t vendor_id, product_id;
	char manufacturer[33]; // USB string descriptors (ASCII)
	char product[33];
	char serial[33];
	uint32_t next_tag; // Transaction tracking
	struct usb_msd_device *luns[USB_MSD_MAX_LUNS];
	uint64_t next_media_poll; // timer tick of the next medium check
	uint8_t has_media_poll; // a logical unit wants its medium polled

	// Counters (diagnostics)
	uint64_t nr_requests; // requests completed
	uint64_t nr_errors; // requests completed with an error
	uint64_t nr_passthrough; // SG_IO commands executed
	uint64_t nr_resets; // reset recoveries
} usb_msd_host_t;

// One logical unit: a disk (sdX, or srN for an optical drive).
typedef struct usb_msd_device {
	int index; // slot in g_msd_devices[]
	volatile int state; // USB_MSD_LUN_* (g_msd_lock)
	usb_msd_host_t *host; // the transport (kept until the slot is freed)
	uint8_t lun;

	// SCSI device info
	uint64_t block_count;
	uint32_t block_size;
	uint8_t ready; // registered and answering I/O
	uint8_t removable; // INQUIRY RMB
	uint8_t medium; // a medium is present
	uint8_t poll_media; // check the medium while idle (card readers)
	uint8_t media_event; // the medium changed: the attach thread tells
			     // the disk layer
	uint8_t media_miss; // consecutive polls that disagreed with `medium'
	volatile uint8_t pinned; // the worker checks its medium (g_msd_lock):
				 // the slot is not handed out meanwhile
	uint8_t use_16; // the capacity needs READ/WRITE(16)
	char vendor[9];
	char product[17];
	char revision[5];
	uint8_t device_type; // peripheral device type (0 disk, 5 optical)

	// The disk-layer disk (sdX / srN); NULL until registered, or when the
	// disk layer refused it and the legacy registration below is used.
	struct gendisk *disk;
	char name[16]; // legacy fallback name ("usbmsd<N>")
	uint8_t legacy_registered; // `blk' was registered the old way
	// The device refuses SYNCHRONIZE CACHE as an invalid command (many USB
	// sticks do, while reporting a write cache): flushes complete without
	// sending it from then on, as for a device without a cache.
	uint8_t no_sync_cache;
	uint8_t sync_fail_reported; // the first other refusal was logged

	// Staging buffer for scatter/gather elements that split a sector
	// (USB_MSD_BLOCK_CHUNK sectors); NULL until first needed
	uint8_t *stage_buf;
	uint32_t stage_size;

	// Block device of the legacy fallback registration (and of the old
	// read/write/sync callbacks, which still work on it)
	block_device_t blk;
} usb_msd_device_t;

// Attach a mass-storage device found by the boot enumeration, right here
// (the root filesystem may be on it).  ST_OK when the device is attached
// (now or before), ST_BUSY when it is being attached elsewhere, an error
// when it could not be.  May sleep.
int usb_msd_attach_boot(xhci_controller_t *ctrl, usb_device_t *dev);
// The boot enumeration is over: devices found from now on are attached by
// the attach thread ("kusbstor"); everything configured but not attached
// yet is handed to it.
void usb_msd_boot_done(void);
// Called by the enumeration for a configured device with a mass-storage
// interface: queues it for the attach thread (after the boot phase).
// Never sleeps.
void usb_msd_probe(xhci_controller_t *ctrl, usb_device_t *dev);
// The device was unplugged (called by the hot-plug poll before its slot is
// disabled): its transport fails every command from now on, and when this
// returns no transfer of the storage driver runs on it.  The disks are
// removed by the attach thread.  May sleep.
void usb_msd_disconnect(xhci_controller_t *ctrl, usb_device_t *dev);

// Low-level BOT protocol (the host's I/O mutex held)
int usb_msd_bot_transfer(usb_msd_device_t *msd, usb_msd_cbw_t *cbw,
			 void *data_buf, uint32_t data_len, usb_msd_csw_t *csw);

// SCSI commands (the host's I/O mutex held)
int usb_msd_test_unit_ready(usb_msd_device_t *msd);
int usb_msd_inquiry(usb_msd_device_t *msd, scsi_inquiry_data_t *data);
int usb_msd_read_capacity(usb_msd_device_t *msd, uint64_t *block_count,
			  uint32_t *block_size);
int usb_msd_request_sense(usb_msd_device_t *msd, scsi_sense_data_t *data);
int usb_msd_read(usb_msd_device_t *msd, uint64_t lba, uint32_t count,
		 void *buf);
int usb_msd_write(usb_msd_device_t *msd, uint64_t lba, uint32_t count,
		  const void *buf);
int usb_msd_sync(usb_msd_device_t *msd);

// Block device interface callbacks
int usb_msd_block_read(block_device_t *dev, unsigned long lba,
		       unsigned long count, void *buf);
int usb_msd_block_write(block_device_t *dev, unsigned long lba,
			unsigned long count, const void *buf);
int usb_msd_block_sync(block_device_t *dev);
int usb_msd_block_release_locks(block_device_t *dev, uint64_t task_id);

/* Force-release a host's I/O mutex if it is currently owned by the given task id. */
int usb_msd_io_release_if_owner(usb_msd_host_t *host, uint64_t task_id);

// The host and logical-unit slots (diagnostics read them)
extern usb_msd_host_t g_msd_hosts[USB_MSD_MAX_HOSTS];
extern usb_msd_device_t g_msd_devices[USB_MSD_MAX_DEVICES];

// Byte swapping for big-endian SCSI data
static inline uint32_t bswap32(uint32_t val)
{
	return ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
	       ((val << 8) & 0xFF0000) | ((val << 24) & 0xFF000000);
}

static inline uint16_t bswap16(uint16_t val)
{
	return (val >> 8) | (val << 8);
}

#endif // LIKEOS_USB_MSD_H
