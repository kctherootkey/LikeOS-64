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

// Requests the driver accepts at once (the disk queue's dispatch depth)
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

// USB Mass Storage device structure
typedef struct usb_msd_device {
	usb_device_t *usb_dev; // Back-reference to USB device
	xhci_controller_t *ctrl; // Controller reference

	// SCSI device info
	uint32_t block_count;
	uint32_t block_size;
	uint8_t ready;
	uint8_t removable;
	char vendor[9];
	char product[17];

	// Transaction tracking
	uint32_t next_tag;

	// Per-device sleeping mutex for serializing I/O (SMP safety).
	// A spinlock is inappropriate here: USB transfers take milliseconds on
	// real hardware, and holding a spinlock with IRQs disabled that long
	// blocks TLB shootdown ACKs and starves other CPUs.
	volatile int io_locked; // 0 = free, 1 = held
	volatile uint64_t
		io_owner; // task id of holder (-1 = none); for orphan-lock recovery
	spinlock_t io_wait_lock; // protects the sleep/wake race

	// Persistent DMA staging buffers, allocated once at init and reused for
	// every command.  The previous code allocated (and freed) a CBW buffer,
	// a CSW buffer and a contiguous data bounce buffer on *every* transfer.
	// The contiguous allocator is an O(total_pages) bitmap scan under a
	// global lock with IRQs disabled — running it per transfer dominated the
	// I/O path.  I/O to a device is serialized by io_lock, so one shared set
	// of buffers per device is safe.  NULL pointers mean "fall back to
	// per-command allocation" (used if these one-time allocations fail).
	uint8_t *cbw_buf; // page-aligned CBW staging buffer (one page)
	uint8_t *csw_buf; // page-aligned CSW staging buffer (one page)
	uint8_t *cbw_buf_raw; // raw allocation backing cbw_buf
	uint8_t *csw_buf_raw; // raw allocation backing csw_buf
	uint8_t *data_buf; // virtual base of contiguous bounce buffer
	uint64_t data_buf_phys; // physical base of bounce buffer (0 = none)
	uint32_t data_buf_size; // usable size of bounce buffer in bytes
	uint32_t data_buf_pages; // page count backing the bounce buffer

	// SCSI identity beyond vendor/product (INQUIRY)
	char revision[5];
	uint8_t device_type; // peripheral device type (0 disk, 5 optical)

	// The disk-layer disk (sdX / srN); NULL until registered, or when the
	// disk layer refused it and the legacy registration below is used.
	struct gendisk *disk;
	int index; // slot in g_msd_devices[], -1 none
	char name[16]; // legacy fallback name ("usbmsd<N>")
	uint8_t legacy_registered; // `blk' was registered the old way
	// The device refuses SYNCHRONIZE CACHE as an invalid command (many USB
	// sticks do, while reporting a write cache): flushes complete without
	// sending it from then on, as for a device without a cache.
	uint8_t no_sync_cache;
	uint8_t sync_fail_reported; // the first other refusal was logged

	// Request execution: requests the disk layer handed to submit wait in
	// this ring until the worker thread runs them (BOT is one command at a
	// time; the ring lets the next request be queued while one runs).
	spinlock_t rq_lock; // protects rq_ring/rq_head/rq_count; leaf, irqsave
	struct blk_request *rq_ring[USB_MSD_QUEUE_DEPTH];
	uint32_t rq_head; // index of the oldest queued request
	uint32_t rq_count; // queued, not yet taken by the worker
	volatile int rq_wait; // wait channel of the idle worker
	task_t *worker; // the "kusbmsd<N>" thread, NULL if it could not start
	volatile int worker_running;
	uint8_t inline_running; // a caller runs the ring itself (no worker)
	struct blk_request *rq_active; // the request the worker executes

	// Staging buffer for scatter/gather elements that split a sector
	// (USB_MSD_BLOCK_CHUNK sectors); NULL until first needed
	uint8_t *stage_buf;
	uint32_t stage_size;

	// Counters (diagnostics)
	uint64_t nr_requests; // requests completed
	uint64_t nr_errors; // requests completed with an error
	uint64_t nr_passthrough; // SG_IO commands executed

	// Block device of the legacy fallback registration (and of the old
	// read/write/sync callbacks, which still work on it)
	block_device_t blk;
} usb_msd_device_t;


// MSD state for boot integration
typedef struct {
	usb_msd_device_t *msd;
	int initialized;
} msd_state_t;

// Core MSD functions
int usb_msd_init(usb_msd_device_t *msd, usb_device_t *dev,
		 xhci_controller_t *ctrl);
int usb_msd_test_unit_ready(usb_msd_device_t *msd);
int usb_msd_inquiry(usb_msd_device_t *msd, scsi_inquiry_data_t *data);
int usb_msd_read_capacity(usb_msd_device_t *msd, uint32_t *block_count,
			  uint32_t *block_size);
int usb_msd_request_sense(usb_msd_device_t *msd, scsi_sense_data_t *data);
int usb_msd_read(usb_msd_device_t *msd, uint32_t lba, uint32_t count,
		 void *buf);
int usb_msd_write(usb_msd_device_t *msd, uint32_t lba, uint32_t count,
		  const void *buf);
int usb_msd_sync(usb_msd_device_t *msd);

// Low-level BOT protocol functions
int usb_msd_bot_transfer(usb_msd_device_t *msd, usb_msd_cbw_t *cbw,
			 void *data_buf, uint32_t data_len, usb_msd_csw_t *csw);

// Block device interface callbacks
int usb_msd_block_read(block_device_t *dev, unsigned long lba,
		       unsigned long count, void *buf);
int usb_msd_block_write(block_device_t *dev, unsigned long lba,
			unsigned long count, const void *buf);
int usb_msd_block_sync(block_device_t *dev);
int usb_msd_block_release_locks(block_device_t *dev, uint64_t task_id);

/* Force-release this device's I/O mutex if it is currently owned by the given task id. */
int usb_msd_io_release_if_owner(usb_msd_device_t *msd, uint64_t task_id);

// Global MSD state
extern usb_msd_device_t *g_msd_devices[8];
extern int g_msd_count;

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
