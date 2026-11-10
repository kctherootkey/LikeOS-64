// LikeOS - USB Mass Storage (Bulk-Only Transport) Driver
// Clean implementation following USB Mass Storage Class specification
//
// BOT Protocol:
// 1. Send Command Block Wrapper (CBW) via bulk OUT
// 2. Transfer data (if any) via bulk IN or OUT
// 3. Receive Command Status Wrapper (CSW) via bulk IN
//
// The device is registered with the disk layer (see "Disk layer
// integration" below) and its requests run on a worker thread; the BOT and
// SCSI command code is unchanged by that.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h> /* first: it includes ke/syscall.h */
#include <kernel/dev/usb/usb_msd.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/ke/cred.h>
#include <kernel/uapi/bug.h>

// Debug output control
#define MSD_DEBUG 0
#if MSD_DEBUG
#define msd_dbg(fmt, ...) kprintf("[MSD] " fmt, ##__VA_ARGS__)
#else
#define msd_dbg(fmt, ...) ((void)0)
#endif

// Spinlock for MSD device list access
static spinlock_t msd_lock = SPINLOCK_INIT("usb_msd");

// Global MSD device list
usb_msd_device_t *g_msd_devices[8] = { 0 };
int g_msd_count = 0;

// Memory helpers — forward to the rep-movs-backed kernel routines so big
// DMA-buffer copies (up to 128 KB per BOT transfer) don't go through a
// byte-at-a-time loop.  At ~30 MB/s a 64 KB byte-loop adds ~2 ms per
// transfer — i.e. it was its own bottleneck on the curl-download path.
static inline void msd_memset(void *dst, int val, size_t n)
{
	mm_memset(dst, val, n);
}

static inline void msd_memcpy(void *dst, const void *src, size_t n)
{
	mm_memcpy(dst, src, n);
}

/* Bounded string copy that always terminates (cap >= 1). */
static void msd_strlcpy(char *dst, size_t cap, const char *src)
{
	size_t i = 0;
	if (!cap)
		return;
	for (; src && src[i] && i + 1 < cap; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

/* SCSI identity strings are blank padded: drop the trailing blanks. */
static void msd_trim(char *s)
{
	int n = 0;
	while (s[n])
		n++;
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == 0))
		s[--n] = 0;
}

static usb_msd_device_t *msd_from_disk(struct gendisk *disk)
{
	if (!disk || disk->magic != GENDISK_MAGIC)
		return NULL;
	return (usb_msd_device_t *)disk->private_data;
}

static usb_msd_device_t *msd_from_bdev(block_device_t *bdev)
{
	if (!bdev)
		return NULL;
	if (blk_bdev_is_extended(bdev))
		return msd_from_disk(bdev->disk);
	return (usb_msd_device_t *)bdev->driver_data;
}


//=============================================================================
// BOT Protocol Core
//=============================================================================

int usb_msd_bot_transfer(usb_msd_device_t *msd, usb_msd_cbw_t *cbw,
			 void *data_buf, uint32_t data_len, usb_msd_csw_t *csw)
{
	BUG_ON(cbw == NULL);
	BUG_ON(csw == NULL);
	BUILD_BUG_ON(CBW_SIZE != 31);
	BUILD_BUG_ON(CSW_SIZE != 13);
	if (!msd || !msd->usb_dev || !msd->ctrl)
		return ST_INVALID;

	xhci_controller_t *ctrl = msd->ctrl;
	usb_device_t *dev = msd->usb_dev;
	uint32_t transferred;
	int st;

	// Use the device's persistent page-aligned CBW/CSW staging buffers if
	// available; otherwise fall back to a one-shot allocation (only happens
	// if the one-time init allocation failed).  raw_cbw/raw_csw are non-NULL
	// only for the fallback path, so the kfree_dma() calls below are no-ops
	// (kfree_dma(NULL) returns immediately) when the persistent buffers are
	// used.  Page alignment guarantees the tiny 31/13-byte transfers never
	// straddle a 64 KB DMA boundary.
	uint8_t *raw_cbw = NULL, *raw_csw = NULL;
	uint8_t *dma_cbw, *dma_csw;
	if (msd->cbw_buf && msd->csw_buf) {
		dma_cbw = msd->cbw_buf;
		dma_csw = msd->csw_buf;
	} else {
		raw_cbw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
		raw_csw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
		if (!raw_cbw || !raw_csw) {
			if (raw_cbw)
				kfree_dma(raw_cbw);
			if (raw_csw)
				kfree_dma(raw_csw);
			return ST_NOMEM;
		}
		dma_cbw = (uint8_t *)(((uint64_t)raw_cbw + 4095) & ~4095ULL);
		dma_csw = (uint8_t *)(((uint64_t)raw_csw + 4095) & ~4095ULL);
	}

	// Copy CBW to DMA buffer.  The memcpy writes all 31 bytes, so no prior
	// zero-fill of the staging page is needed.
	msd_memcpy(dma_cbw, cbw, CBW_SIZE);

	// Phase 1: Send CBW
	msd_dbg("Sending CBW: tag=%08x, len=%d, flags=%02x, cmd=%02x\n",
		cbw->tag, cbw->data_transfer_len, cbw->flags, cbw->cb[0]);

	st = xhci_bulk_transfer_out(ctrl, dev, dma_cbw, CBW_SIZE, &transferred);
	if (st != ST_OK) {
		msd_dbg("CBW send failed: st=%d\n", st);
		kfree_dma(raw_cbw);
		kfree_dma(raw_csw);
		return st;
	}

	// Phase 2: Data transfer (if any)
	// The DMA buffer must be PHYSICALLY CONTIGUOUS: the kernel heap only
	// guarantees virtual contiguity, but for transfers > 1 page the xHCI
	// controller writes sequentially in physical memory, so non-contiguous
	// pages would cause silent memory corruption.  Use the device's
	// persistent contiguous bounce buffer when the transfer fits; only fall
	// back to a one-shot contiguous allocation for oversized transfers (the
	// block layer never issues those, so the hot path never allocates).
	int data_st = ST_OK;
	uint64_t data_phys = 0;
	size_t data_pages = 0;
	int data_persistent = 0;
	if (data_len > 0 && data_buf) {
		uint8_t *dma_data;
		if (msd->data_buf && data_len <= msd->data_buf_size) {
			dma_data = msd->data_buf;
			data_persistent = 1;
		} else {
			data_pages = ((uint32_t)data_len + 4095) / 4096;
			data_phys = mm_allocate_contiguous_pages(data_pages);
			if (!data_phys) {
				msd_dbg("Failed to allocate contiguous DMA buffer for data\n");
				kfree_dma(raw_cbw);
				kfree_dma(raw_csw);
				return ST_NOMEM;
			}
			dma_data = (uint8_t *)phys_to_virt(data_phys);
		}

		if (cbw->flags & CBW_FLAG_DATA_IN) {
			// Data IN
			msd_dbg("Data IN: %d bytes\n", data_len);
			data_st = xhci_bulk_transfer_in(ctrl, dev, dma_data,
							data_len, &transferred);
			if (data_st == ST_OK) {
				// Compiler barrier to prevent compiler from reordering the memcpy
				// before the DMA transfer completes.  On x86, DMA is cache-coherent
				// so mfence is not needed — the transfer_in already returned.
				__asm__ volatile("" ::: "memory");
				// A short read leaves [transferred, data_len) holding stale
				// bounce-buffer contents; zero just that tail so the caller
				// never sees leftovers (matches the old pre-zeroed buffer).
				// The common full-length read skips this entirely.
				if (transferred < data_len) {
					msd_memset(dma_data + transferred, 0,
						   data_len - transferred);
				}
				// Copy received data back to caller
				msd_memcpy(data_buf, dma_data, data_len);
			}
		} else {
			// Data OUT - copy data to DMA buffer first
			msd_memcpy(dma_data, data_buf, data_len);
			msd_dbg("Data OUT: %d bytes\n", data_len);
			data_st = xhci_bulk_transfer_out(
				ctrl, dev, dma_data, data_len, &transferred);
		}

		if (!data_persistent) {
			mm_free_contiguous_pages(data_phys, data_pages);
			data_phys = 0;
		}

		if (data_st != ST_OK) {
			msd_dbg("Data transfer failed: st=%d\n", data_st);
			// Try to recover by reading CSW anyway
		}
	}

	// Phase 3: Receive CSW
	msd_memset(dma_csw, 0, CSW_SIZE);
	st = xhci_bulk_transfer_in(ctrl, dev, dma_csw, CSW_SIZE, &transferred);
	if (st != ST_OK) {
		msd_dbg("CSW receive failed: st=%d\n", st);
		kfree_dma(raw_cbw);
		kfree_dma(raw_csw);
		return st;
	}

	// Copy CSW back to caller's buffer
	msd_memcpy(csw, dma_csw, CSW_SIZE);
	kfree_dma(raw_cbw);
	kfree_dma(raw_csw);

	// Validate CSW
	if (csw->signature != CSW_SIGNATURE) {
		WARN_RATELIMIT(
			1,
			"usb_msd: CSW signature 0x%08x != expected 0x%08x: hardware/firmware bug or USB corruption",
			csw->signature, CSW_SIGNATURE);
		msd_dbg("Invalid CSW signature: %08x\n", csw->signature);
		return ST_IO;
	}

	if (csw->tag != cbw->tag) {
		WARN_RATELIMIT(
			1,
			"usb_msd: CSW tag 0x%08x != CBW tag 0x%08x: out-of-order response or firmware bug",
			csw->tag, cbw->tag);
		msd_dbg("CSW tag mismatch: expected %08x, got %08x\n", cbw->tag,
			csw->tag);
		return ST_IO;
	}

	msd_dbg("CSW: status=%d, residue=%d\n", csw->status, csw->data_residue);

	if (csw->status == CSW_STATUS_PASSED) {
		return ST_OK;
	} else if (csw->status == CSW_STATUS_FAILED) {
		return ST_IO;
	} else {
		return ST_ERR; // Phase error
	}
}

//=============================================================================
// SCSI Commands
//=============================================================================

int usb_msd_test_unit_ready(usb_msd_device_t *msd)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 0;
	cbw.flags = 0;
	cbw.lun = 0;
	cbw.cb_length = 6;
	cbw.cb[0] = SCSI_TEST_UNIT_READY;

	return usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
}

int usb_msd_inquiry(usb_msd_device_t *msd, scsi_inquiry_data_t *data)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 36; // Standard inquiry response
	cbw.flags = CBW_FLAG_DATA_IN;
	cbw.lun = 0;
	cbw.cb_length = 6;
	cbw.cb[0] = SCSI_INQUIRY;
	cbw.cb[4] = 36; // Allocation length

	msd_memset(data, 0, sizeof(*data));

	int st = usb_msd_bot_transfer(msd, &cbw, data, 36, &csw);

	if (st == ST_OK) {
		msd_dbg("Inquiry: DevType=%02x, Vendor=%.8s, Product=%.16s\n",
			data->device_type, data->vendor, data->product);
	}

	return st;
}

int usb_msd_request_sense(usb_msd_device_t *msd, scsi_sense_data_t *data)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 18;
	cbw.flags = CBW_FLAG_DATA_IN;
	cbw.lun = 0;
	cbw.cb_length = 6;
	cbw.cb[0] = SCSI_REQUEST_SENSE;
	cbw.cb[4] = 18; // Allocation length

	msd_memset(data, 0, sizeof(*data));

	return usb_msd_bot_transfer(msd, &cbw, data, 18, &csw);
}

int usb_msd_read_capacity(usb_msd_device_t *msd, uint32_t *block_count,
			  uint32_t *block_size)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;
	scsi_read_capacity_data_t data;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 8;
	cbw.flags = CBW_FLAG_DATA_IN;
	cbw.lun = 0;
	cbw.cb_length = 10;
	cbw.cb[0] = SCSI_READ_CAPACITY_10;

	msd_memset(&data, 0, sizeof(data));

	int st = usb_msd_bot_transfer(msd, &cbw, &data, 8, &csw);

	if (st == ST_OK) {
		// Convert from big-endian
		*block_count = bswap32(data.last_lba) + 1;
		*block_size = bswap32(data.block_size);
		msd_dbg("Capacity: %u blocks, %u bytes/block\n", *block_count,
			*block_size);
	}

	return st;
}

/* Max blocks per SCSI READ(10)/WRITE(10).  Raised from 128 (64KiB) so the
 * filesystem can issue MiB-sized transfers and amortise the per-command
 * (CBW/data/CSW) latency.  2048 blocks = 1 MiB; the xHCI bulk path chains the
 * needed TRBs (TRB_MAX_BUFF_SIZE=64KiB each, ring = 256 TRBs) and SCSI(10)
 * encodes the count in 16 bits, so this is well within both limits. */
#define USB_MSD_MAX_BLOCKS 2048

/* Sectors per BOT transaction issued by the block layer.  Each chunk is one
 * SCSI READ(10)/WRITE(10), so larger chunks amortise the CBW/CSW round-trips.
 * The persistent DMA bounce buffer (usb_msd_init) is sized to exactly this many
 * sectors, so the two MUST stay in sync — keep this a single named constant. */
#define USB_MSD_BLOCK_CHUNK 256

int usb_msd_read(usb_msd_device_t *msd, uint32_t lba, uint32_t count, void *buf)
{
	BUG_ON(buf == NULL);
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	if (count == 0 || count > USB_MSD_MAX_BLOCKS) {
		msd_dbg("Invalid read count: %u\n", count);
		return ST_INVALID;
	}

	uint32_t transfer_len = count * msd->block_size;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = transfer_len;
	cbw.flags = CBW_FLAG_DATA_IN;
	cbw.lun = 0;
	cbw.cb_length = 10;
	cbw.cb[0] = SCSI_READ_10;
	// LBA (big-endian)
	cbw.cb[2] = (lba >> 24) & 0xFF;
	cbw.cb[3] = (lba >> 16) & 0xFF;
	cbw.cb[4] = (lba >> 8) & 0xFF;
	cbw.cb[5] = lba & 0xFF;
	// Transfer length in blocks (big-endian)
	cbw.cb[7] = (count >> 8) & 0xFF;
	cbw.cb[8] = count & 0xFF;

	msd_dbg("Read: LBA=%u, Count=%u, Len=%u\n", lba, count, transfer_len);

	return usb_msd_bot_transfer(msd, &cbw, buf, transfer_len, &csw);
}

int usb_msd_write(usb_msd_device_t *msd, uint32_t lba, uint32_t count,
		  const void *buf)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	if (count == 0 || count > USB_MSD_MAX_BLOCKS) {
		return ST_INVALID;
	}

	uint32_t transfer_len = count * msd->block_size;

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = transfer_len;
	cbw.flags = CBW_FLAG_DATA_OUT;
	cbw.lun = 0;
	cbw.cb_length = 10;
	cbw.cb[0] = SCSI_WRITE_10;
	// LBA (big-endian)
	cbw.cb[2] = (lba >> 24) & 0xFF;
	cbw.cb[3] = (lba >> 16) & 0xFF;
	cbw.cb[4] = (lba >> 8) & 0xFF;
	cbw.cb[5] = lba & 0xFF;
	// Transfer length in blocks (big-endian)
	cbw.cb[7] = (count >> 8) & 0xFF;
	cbw.cb[8] = count & 0xFF;

	return usb_msd_bot_transfer(msd, &cbw, (void *)buf, transfer_len, &csw);
}

int usb_msd_sync(usb_msd_device_t *msd)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	int st;

	if (!msd || !msd->ready) {
		return ST_INVALID;
	}
	/* Refused once as an invalid command: the device has nothing a
	 * flush could reach, so a flush is complete as it is. */
	if (msd->no_sync_cache)
		return ST_OK;

	msd_memset(&cbw, 0, sizeof(cbw));
	msd_memset(&csw, 0, sizeof(csw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 0; // No data transfer
	cbw.flags = CBW_FLAG_DATA_IN; // No data but use IN for status
	cbw.lun = 0;
	cbw.cb_length = 10;
	cbw.cb[0] = SCSI_SYNCHRONIZE_CACHE_10;
	// All other bytes are 0: sync entire device

	msd_dbg("Sync: sending SYNCHRONIZE_CACHE_10\n");

	st = usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
	if (st == ST_IO && csw.signature == CSW_SIGNATURE &&
	    csw.tag == cbw.tag && csw.status == CSW_STATUS_FAILED) {
		/* CHECK CONDITION.  Fetch the sense, as an initiator must
		 * before the next command clears it, and decide the way the
		 * established disk drivers do: ILLEGAL REQUEST with INVALID
		 * COMMAND OPERATION CODE (20h) or INVALID FIELD IN CDB (24h)
		 * means the device has no SYNCHRONIZE CACHE -- many USB sticks
		 * claim a write cache and still refuse it -- and MEDIUM NOT
		 * PRESENT (3Ah) leaves nothing to flush.  Both count as a
		 * completed flush; anything else stays an I/O error. */
		scsi_sense_data_t sense;

		msd_memset(&sense, 0, sizeof(sense));
		if (usb_msd_request_sense(msd, &sense) == ST_OK) {
			uint8_t key = sense.sense_key & 0x0F;

			if (key == 0x05 &&
			    (sense.asc == 0x20 || sense.asc == 0x24)) {
				msd->no_sync_cache = 1;
				kprintf("[usb-storage] %s: the device refuses "
					"SYNCHRONIZE CACHE (sense %x/%02x/%02x); "
					"flushes complete without it\n",
					msd->disk ? msd->disk->disk_name :
						    msd->name,
					key, sense.asc, sense.ascq);
				return ST_OK;
			}
			if (sense.asc == 0x3A)
				return ST_OK;
			if (!msd->sync_fail_reported) {
				msd->sync_fail_reported = 1;
				kprintf("[usb-storage] %s: SYNCHRONIZE CACHE "
					"failed, sense %x/%02x/%02x\n",
					msd->disk ? msd->disk->disk_name :
						    msd->name,
					key, sense.asc, sense.ascq);
			}
		}
	}
	return st;
}

//=============================================================================
// Block Device Interface
//=============================================================================

// Sleeping mutex for MSD I/O serialization.
// Unlike a spinlock, this blocks the calling task (via the scheduler) when
// the device is busy, keeping IRQs enabled so TLB shootdowns, timer ticks,
// and other IPIs can still be serviced.
static void msd_io_lock(usb_msd_device_t *msd)
{
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&msd->io_wait_lock, &flags);
		if (!msd->io_locked) {
			msd->io_locked = 1;
			msd->io_owner = my_id;
			task_sleep_lock_taken();
			spin_unlock_irqrestore(&msd->io_wait_lock, flags);
			return;
		}
		// Device busy — sleep until the holder releases it.
		if (cur) {
			cur->wait_channel = (void *)&msd->io_locked;
			task_set_blocked(cur);
		}
		spin_unlock_irqrestore(&msd->io_wait_lock, flags);
		sched_schedule(); // yields with IRQs enabled
	}
}

static void msd_io_unlock(usb_msd_device_t *msd)
{
	uint64_t flags;
	spin_lock_irqsave(&msd->io_wait_lock, &flags);
	msd->io_locked = 0;
	msd->io_owner = (uint64_t)-1;
	spin_unlock_irqrestore(&msd->io_wait_lock, flags);
	task_sleep_lock_dropped();
	sched_wake_channel((void *)&msd->io_locked);
}

/* Force-release this device's I/O mutex if it is currently owned by the
 * given task id.  Companion to fat32_io_release_if_owner; called when a
 * task that was killed mid-FS-write may have died while sleeping inside
 * usb_msd_block_read/write holding both locks.
 *
 * Also clears the xHCI pending_xfer entries for this MSD's bulk endpoints:
 * the dying task may have died inside xhci_bulk_transfer_in/out, leaving
 * a pointer to its (about-to-be-freed) kernel stack in pending_xfer.
 * Without this, the next bulk-completion IRQ writes through the dangling
 * pointer and the kernel oopses with a page fault in
 * xhci_handle_transfer_event. */
int usb_msd_io_release_if_owner(usb_msd_device_t *msd, uint64_t task_id)
{
	if (!msd)
		return 0;
	uint64_t flags;
	int released = 0;
	spin_lock_irqsave(&msd->io_wait_lock, &flags);
	if (msd->io_locked && msd->io_owner == task_id) {
		msd->io_locked = 0;
		msd->io_owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&msd->io_wait_lock, flags);
	if (released) {
		/* Defang any in-flight transfer events for this MSD before the
         * dying task's kernel stack is freed.  Walk both bulk endpoints
         * (the only ones we use).  Clearing pending_xfer alone is not
         * enough — the xHCI still has the dead task's TRBs queued on the
         * endpoint ring and will not advance to subsequent TRBs until
         * they complete.  Reset each endpoint to drop the orphan TD and
         * reposition the dequeue pointer so the NEXT bulk_transfer call
         * (typically by `echo`/curl run after the killed curl) can
         * actually make progress instead of timing out. */
		if (msd->ctrl && msd->usb_dev) {
			uint8_t slot = msd->usb_dev->slot_id;
			/* Clear pending_xfer FIRST so any in-flight transfer-event
             * IRQ harmlessly skips the write (no dangling-pointer fault).
             * Don't call xhci_reset_endpoint here: with an endpoint in
             * RUNNING state and the controller still draining queued
             * TRBs, the Reset EP command can time out — and the next
             * bulk_transfer's own error path already calls
             * xhci_reset_endpoint() when it sees the orphan TRBs cause
             * a transaction error.  We just need to defang the IRQ. */
			if (msd->usb_dev->bulk_in_ep) {
				xhci_clear_pending_xfer(
					msd->ctrl, slot,
					msd->usb_dev->bulk_in_ep * 2 + 1);
			}
			if (msd->usb_dev->bulk_out_ep) {
				xhci_clear_pending_xfer(
					msd->ctrl, slot,
					msd->usb_dev->bulk_out_ep * 2);
			}
		}
		sched_wake_channel((void *)&msd->io_locked);
	}
	return released;
}

int usb_msd_block_read(block_device_t *dev, unsigned long lba,
		       unsigned long count, void *buf)
{
	BUG_ON(dev == NULL);
	BUG_ON(buf == NULL && count > 0);
	usb_msd_device_t *msd = msd_from_bdev(dev);

	if (!msd || !msd->ready) {
		return ST_NO_DEVICE;
	}

	// Read in USB_MSD_BLOCK_CHUNK-sector chunks — large enough to amortize
	// the CBW + CSW round-trip overhead per SCSI READ_10/WRITE_10
	// transaction.  Each chunk reuses the device's persistent bounce buffer.
	//
	// The device mutex is taken PER CHUNK, not across the whole request:
	// a multi-megabyte transfer otherwise monopolises the device end to
	// end, and every other task's first disk access (page-in, metadata
	// read) stalls behind it.  Per-chunk locking lets competing streams
	// interleave at ~128 KB granularity; the extra lock/unlock per chunk
	// is noise next to the transfer itself.
	uint8_t *ptr = (uint8_t *)buf;
	unsigned long remaining = count;
	unsigned long current_lba = lba;
	int result = ST_OK;

	while (remaining > 0) {
		unsigned long chunk = (remaining > USB_MSD_BLOCK_CHUNK) ?
					      USB_MSD_BLOCK_CHUNK :
					      remaining;

		msd_io_lock(msd);
		int st = usb_msd_read(msd, (uint32_t)current_lba,
				      (uint32_t)chunk, ptr);
		msd_io_unlock(msd);
		if (st != ST_OK) {
			msd_dbg("Block read failed at LBA %lu: st=%d\n",
				current_lba, st);
			result = st;
			break;
		}

		ptr += chunk * msd->block_size;
		current_lba += chunk;
		remaining -= chunk;
	}

	return result;
}

int usb_msd_block_write(block_device_t *dev, unsigned long lba,
			unsigned long count, const void *buf)
{
	BUG_ON(dev == NULL);
	BUG_ON(buf == NULL && count > 0);
	usb_msd_device_t *msd = msd_from_bdev(dev);

	if (!msd || !msd->ready) {
		return ST_NO_DEVICE;
	}

	// Per-chunk device locking — same rationale as usb_msd_block_read.
	const uint8_t *ptr = (const uint8_t *)buf;
	unsigned long remaining = count;
	unsigned long current_lba = lba;
	int result = ST_OK;

	while (remaining > 0) {
		unsigned long chunk = (remaining > USB_MSD_BLOCK_CHUNK) ?
					      USB_MSD_BLOCK_CHUNK :
					      remaining;

		msd_io_lock(msd);
		int st = usb_msd_write(msd, (uint32_t)current_lba,
				       (uint32_t)chunk, ptr);
		msd_io_unlock(msd);
		if (st != ST_OK) {
			result = st;
			break;
		}

		ptr += chunk * msd->block_size;
		current_lba += chunk;
		remaining -= chunk;
	}

	return result;
}

int usb_msd_block_sync(block_device_t *dev)
{
	usb_msd_device_t *msd = msd_from_bdev(dev);

	if (!msd || !msd->ready) {
		return ST_NO_DEVICE;
	}

	// Serialize I/O to this device (sleeping mutex — IRQs stay enabled)
	msd_io_lock(msd);
	int result = usb_msd_sync(msd);
	msd_io_unlock(msd);
	return result;
}

int usb_msd_block_release_locks(block_device_t *dev, uint64_t task_id)
{
	usb_msd_device_t *msd = msd_from_bdev(dev);
	if (!msd)
		return 0;
	return usb_msd_io_release_if_owner(msd, task_id);
}

//=============================================================================
// Disk layer integration
//=============================================================================
//
// The disk layer hands requests to usb_msd_blk_submit() from the task that
// dispatches -- the submitter or kblockd -- and that function must not
// sleep.  A BOT transfer
// sleeps (it waits for xHCI transfer events and takes the device's sleeping
// I/O mutex), so submit only parks the request in the device's ring and
// wakes the worker thread "kusbmsd<N>", which executes the requests in
// order and completes each with blk_complete_request_direct() (the end
// of the request runs right there, in the worker).  The worker issues
// exactly the commands the synchronous path always issued: READ(10)/
// WRITE(10) in USB_MSD_BLOCK_CHUNK-sector pieces, each under the per-device
// I/O mutex, and SYNCHRONIZE CACHE(10) for a flush.
//
// Lock order: rq_lock is a leaf spinlock (irqsave) protecting only the
// ring; it is never held across a transfer, a wake-up or a completion.  The
// sleeping I/O mutex (msd_io_lock) is taken by the worker per chunk, as the
// old callers did.

static uint8_t g_msd_worker_stacks[8][16384] __attribute__((aligned(16)));

/* ST_* of a BOT/SCSI call -> the disk layer's -errno */
static int msd_st_to_errno(int st)
{
	switch (st) {
	case ST_OK:
		return 0;
	case ST_NO_DEVICE:
		return -ENODEV;
	case ST_INVALID:
		return -EINVAL;
	case ST_NOMEM:
		return -ENOMEM;
	case ST_TIMEOUT:
		return -ETIMEDOUT;
	case ST_UNSUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

/* Take the oldest queued request (NULL when the ring is empty); rq_lock
 * held. */
static struct blk_request *msd_rq_pop_locked(usb_msd_device_t *msd)
{
	struct blk_request *req = NULL;
	if (msd->rq_count) {
		req = msd->rq_ring[msd->rq_head];
		msd->rq_ring[msd->rq_head] = NULL;
		msd->rq_head = (msd->rq_head + 1) % USB_MSD_QUEUE_DEPTH;
		msd->rq_count--;
	}
	return req;
}

static struct blk_request *msd_rq_pop(usb_msd_device_t *msd)
{
	struct blk_request *req;
	uint64_t flags;
	spin_lock_irqsave(&msd->rq_lock, &flags);
	req = msd_rq_pop_locked(msd);
	spin_unlock_irqrestore(&msd->rq_lock, flags);
	return req;
}

/* ---- Scatter/gather helpers ------------------------------------------- */

/* A cursor over a request's sg list */
struct msd_sg_cursor {
	struct blk_request *req;
	uint32_t idx; /* current element */
	uint32_t off; /* bytes of it already consumed */
};

static uint8_t *msd_sg_base(const struct blk_sg_entry *e)
{
	if (e->virt)
		return (uint8_t *)e->virt;
	return e->phys ? (uint8_t *)phys_to_virt(e->phys) : NULL;
}

/* Skip exhausted elements; 0 when an element with data remains. */
static int msd_sg_settle(struct msd_sg_cursor *c)
{
	while (c->idx < c->req->nsg && c->off >= c->req->sg[c->idx].len) {
		c->idx++;
		c->off = 0;
	}
	return c->idx < c->req->nsg ? 0 : -EINVAL;
}

/* Copy `len' bytes between the sg list (at the cursor) and `buf'. */
static int msd_sg_copy(struct msd_sg_cursor *c, uint8_t *buf, uint32_t len,
		       int to_sg)
{
	while (len) {
		if (msd_sg_settle(c))
			return -EINVAL;
		const struct blk_sg_entry *e = &c->req->sg[c->idx];
		uint8_t *base = msd_sg_base(e);
		uint32_t n = e->len - c->off;
		if (!base)
			return -EINVAL;
		if (n > len)
			n = len;
		if (to_sg)
			msd_memcpy(base + c->off, buf, n);
		else
			msd_memcpy(buf, base + c->off, n);
		c->off += n;
		buf += n;
		len -= n;
	}
	return 0;
}

/* READ / WRITE of a request: the sectors [req->sector, +count) of the
 * disk, data in the request's sg list.  Each piece is one READ(10)/WRITE(10)
 * of at most USB_MSD_BLOCK_CHUNK sectors, issued under the I/O mutex.  An
 * element holding at least one whole sector is transferred in place (the
 * BOT layer copies through its DMA bounce buffer anyway); a sector split
 * across elements goes through the staging buffer. */
static int msd_exec_rw(usb_msd_device_t *msd, struct blk_request *req,
		       uint32_t *resid)
{
	const int write = (req->op == BLK_OP_WRITE);
	const uint32_t ss = msd->block_size;
	uint64_t lba = req->sector;
	uint32_t left = req->count;
	struct msd_sg_cursor cur = { .req = req, .idx = 0, .off = 0 };

	*resid = left * ss;
	if (!left)
		return 0;
	if (lba >= msd->block_count || left > msd->block_count - lba) {
		WARN_RATELIMIT(1,
			       "usb_msd: request beyond the end: lba %llu + %u > %u\n",
			       (unsigned long long)lba, left, msd->block_count);
		return -EINVAL;
	}
	VM_BUG_ON(!req->sg && req->nsg);

	while (left) {
		uint8_t *ptr;
		uint32_t n;
		int staged = 0;

		if (msd_sg_settle(&cur))
			return -EINVAL; /* sg list shorter than count */
		const struct blk_sg_entry *e = &req->sg[cur.idx];
		uint32_t avail = e->len - cur.off;
		if (avail >= ss && msd_sg_base(e)) {
			n = avail / ss;
			ptr = msd_sg_base(e) + cur.off;
		} else {
			/* a sector straddles two elements */
			if (!msd->stage_buf) {
				msd->stage_size = USB_MSD_BLOCK_CHUNK * ss;
				msd->stage_buf = (uint8_t *)kalloc(msd->stage_size);
				if (!msd->stage_buf) {
					msd->stage_size = 0;
					return -ENOMEM;
				}
			}
			n = msd->stage_size / ss;
			ptr = msd->stage_buf;
			staged = 1;
		}
		if (n > left)
			n = left;
		if (n > USB_MSD_BLOCK_CHUNK)
			n = USB_MSD_BLOCK_CHUNK;
		if (staged && write &&
		    msd_sg_copy(&cur, ptr, n * ss, 0) != 0)
			return -EINVAL;

		msd_io_lock(msd);
		int st = write ? usb_msd_write(msd, (uint32_t)lba, n, ptr) :
				 usb_msd_read(msd, (uint32_t)lba, n, ptr);
		msd_io_unlock(msd);
		if (st != ST_OK) {
			WARN_RATELIMIT(1,
				       "usb_msd: %s of %u sectors at %llu failed (st %d)\n",
				       write ? "write" : "read", n,
				       (unsigned long long)lba, st);
			return msd_st_to_errno(st);
		}
		if (staged) {
			if (!write && msd_sg_copy(&cur, ptr, n * ss, 1) != 0)
				return -EINVAL;
		} else {
			cur.off += n * ss;
		}
		lba += n;
		left -= n;
		*resid = left * ss;
	}
	return 0;
}

/* ---- SG_IO pass-through ------------------------------------------------- */

/* A SCSI command handed through the queue as BLK_OP_DRV_IN/OUT
 * (req->drv_cmd).  Filled by the ioctl, executed by the worker. */
struct msd_passthru {
	uint8_t cdb[16];
	uint8_t cdb_len;
	uint8_t data_in; /* data stage device -> host */
	uint8_t status; /* SAM status */
	uint8_t sense_len;
	uint8_t sense[32];
	uint16_t host_status; /* SG_ERR_DID_* */
	uint16_t driver_status; /* SG_ERR_DRIVER_* */
	void *buf; /* kernel buffer, len bytes */
	uint32_t len;
	uint32_t resid;
};

static void msd_exec_passthru(usb_msd_device_t *msd, struct msd_passthru *pt)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_memset(&cbw, 0, sizeof(cbw));
	msd_memset(&csw, 0, sizeof(csw));
	pt->status = SAM_STAT_GOOD;
	pt->host_status = SG_ERR_DID_OK;
	pt->driver_status = SG_ERR_DRIVER_OK;
	pt->sense_len = 0;
	pt->resid = pt->len;

	msd_io_lock(msd);
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = pt->buf ? pt->len : 0;
	cbw.flags = pt->data_in ? CBW_FLAG_DATA_IN : CBW_FLAG_DATA_OUT;
	cbw.lun = 0;
	cbw.cb_length = pt->cdb_len;
	msd_memcpy(cbw.cb, pt->cdb, pt->cdb_len);
	int st = usb_msd_bot_transfer(msd, &cbw, pt->buf,
				      pt->buf ? pt->len : 0, &csw);
	int csw_valid = (csw.signature == CSW_SIGNATURE && csw.tag == cbw.tag);
	if (csw_valid)
		pt->resid = csw.data_residue <= pt->len ? csw.data_residue :
							  pt->len;
	if (st == ST_OK) {
		if (!csw_valid)
			pt->resid = 0;
	} else if (csw_valid && csw.status == CSW_STATUS_FAILED) {
		/* CHECK CONDITION: fetch the sense data, as an initiator must
		 * before the next command clears it. */
		scsi_sense_data_t sense;
		pt->status = SAM_STAT_CHECK_CONDITION;
		if (usb_msd_request_sense(msd, &sense) == ST_OK) {
			uint32_t n = sizeof(sense) < sizeof(pt->sense) ?
					     sizeof(sense) :
					     sizeof(pt->sense);
			msd_memcpy(pt->sense, &sense, n);
			pt->sense_len = (uint8_t)n;
			pt->driver_status = SG_ERR_DRIVER_SENSE;
		}
	} else if (csw_valid && csw.status == CSW_STATUS_PHASE_ERROR) {
		pt->host_status = SG_ERR_DID_ERROR;
		pt->driver_status = SG_ERR_DRIVER_ERROR;
	} else {
		pt->host_status = (st == ST_TIMEOUT) ? SG_ERR_DID_TIME_OUT :
						       SG_ERR_DID_ERROR;
		pt->driver_status = SG_ERR_DRIVER_ERROR;
	}
	msd_io_unlock(msd);
	msd->nr_passthrough++;
}

/* ---- Request execution -------------------------------------------------- */

static void msd_execute(usb_msd_device_t *msd, struct blk_request *req)
{
	uint32_t resid = 0;
	int status;

	BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	msd->rq_active = req;
	if (!msd->ready) {
		status = -ENODEV;
	} else {
		switch (req->op) {
		case BLK_OP_READ:
		case BLK_OP_WRITE:
			status = msd_exec_rw(msd, req, &resid);
			break;
		case BLK_OP_FLUSH:
			msd_io_lock(msd);
			status = msd_st_to_errno(usb_msd_sync(msd));
			msd_io_unlock(msd);
			break;
		case BLK_OP_DRV_IN:
		case BLK_OP_DRV_OUT:
			if (!req->drv_cmd) {
				status = -EINVAL;
				break;
			}
			msd_exec_passthru(msd, (struct msd_passthru *)req->drv_cmd);
			status = 0; /* the SCSI result is in the command */
			break;
		default:
			/* discard / write zeroes: not offered in the limits */
			status = -EOPNOTSUPP;
			break;
		}
	}
	msd->rq_active = NULL;
	msd->nr_requests++;
	if (status)
		msd->nr_errors++;
	/* The worker is a task: the request ends right here; the next
	 * dispatch comes from the submitter or kblockd, never from here. */
	blk_complete_request_direct(req, status, resid);
}

/* The worker: runs queued requests; sleeps on rq_wait when there are none.
 * The idle check and the switch to BLOCKED happen under rq_lock, and submit
 * queues under the same lock before it wakes the channel, so a request
 * queued at any moment either is seen here or finds the worker blocked on
 * the channel -- no wake-up is lost.  A nap deadline backs that up anyway. */
#define MSD_WORKER_NAP_MS 1000

static void msd_worker(void *arg)
{
	usb_msd_device_t *msd = (usb_msd_device_t *)arg;
	task_t *cur = sched_current();

	BUG_ON(msd == NULL || cur == NULL);
	msd->worker_running = 1;
	for (;;) {
		struct blk_request *req = msd_rq_pop(msd);
		if (req) {
			msd_execute(msd, req);
			continue;
		}
		uint64_t flags;
		spin_lock_irqsave(&msd->rq_lock, &flags);
		if (msd->rq_count) {
			spin_unlock_irqrestore(&msd->rq_lock, flags);
			continue;
		}
		cur->wait_channel = (void *)&msd->rq_wait;
		cur->wakeup_tick =
			timer_ticks() + timer_ms_to_ticks(MSD_WORKER_NAP_MS) + 1;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&msd->rq_lock, flags);
		sched_schedule();
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		if (cur->state != TASK_RUNNING)
			cur->state = TASK_RUNNING;
	}
}

/* Run queued requests in the calling context -- only when there is no
 * worker thread (it could not be created) and the caller may block.  One
 * runner at a time per device: a completion dispatches the next request
 * straight back into submit, which must then only queue it (the running
 * loop picks it up) instead of recursing once per request. */
static int msd_run_inline(usb_msd_device_t *msd)
{
	struct blk_request *req;
	uint64_t flags;
	int done = 0;

	if (msd->worker_running || !irqs_enabled() || !sched_current())
		return 0;
	spin_lock_irqsave(&msd->rq_lock, &flags);
	if (msd->inline_running) {
		spin_unlock_irqrestore(&msd->rq_lock, flags);
		return 0;
	}
	msd->inline_running = 1;
	for (;;) {
		req = msd_rq_pop_locked(msd);
		if (!req) {
			/* cleared under the lock that queues: nothing queued
			 * after this check is left without a runner */
			msd->inline_running = 0;
			spin_unlock_irqrestore(&msd->rq_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&msd->rq_lock, flags);
		msd_execute(msd, req);
		done++;
		spin_lock_irqsave(&msd->rq_lock, &flags);
	}
	return done;
}

static int usb_msd_blk_submit(struct gendisk *disk, struct blk_request *req)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	uint64_t flags;

	if (unlikely(!msd))
		return -ENODEV;
	if (unlikely(!msd->ready))
		return -ENODEV;
	switch (req->op) {
	case BLK_OP_READ:
	case BLK_OP_WRITE:
	case BLK_OP_FLUSH:
	case BLK_OP_DRV_IN:
	case BLK_OP_DRV_OUT:
		break;
	default:
		return -EOPNOTSUPP;
	}
	spin_lock_irqsave(&msd->rq_lock, &flags);
	if (msd->rq_count >= USB_MSD_QUEUE_DEPTH) {
		spin_unlock_irqrestore(&msd->rq_lock, flags);
		return -EBUSY; /* the queue retries after a completion */
	}
	msd->rq_ring[(msd->rq_head + msd->rq_count) % USB_MSD_QUEUE_DEPTH] =
		req;
	msd->rq_count++;
	req->driver_data = msd;
	spin_unlock_irqrestore(&msd->rq_lock, flags);

	if (likely(msd->worker_running))
		sched_wake_channel_once((void *)&msd->rq_wait, 1);
	else
		msd_run_inline(msd);
	return 0;
}

/* Polled completion (blk_wait without the scheduler): this driver needs its
 * worker, or a context that may block; with interrupts off nothing can be
 * done -- BOT transfers wait for xHCI interrupts. */
static int usb_msd_blk_poll(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	if (!msd)
		return 0;
	return msd_run_inline(msd);
}

/* The read-only SCSI commands an unprivileged opener may send (the same
 * short list the disk layer documents for SG_IO). */
static int msd_sgio_cmd_harmless(const uint8_t *cdb)
{
	switch (cdb[0]) {
	case TEST_UNIT_READY:
	case REQUEST_SENSE:
	case INQUIRY:
	case READ_CAPACITY:
	case MODE_SENSE:
	case MODE_SENSE_10:
	case REPORT_LUNS:
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		return 1;
	case SERVICE_ACTION_IN_16:
		return (cdb[1] & 0x1f) == SAI_READ_CAPACITY_16;
	default:
		return 0;
	}
}

/* The commands an unprivileged opener may send in addition when the node
 * was opened for writing: writing the medium and its cache, which write(2)
 * on the same open already allows, and on an optical drive its burning
 * commands (SET CD SPEED, SET STREAMING, BLANK, CLOSE TRACK, RESERVE TRACK,
 * SEND CUE SHEET, SEND OPC INFORMATION, FORMAT UNIT). */
static int msd_sgio_cmd_write_safe(const uint8_t *cdb, int optical)
{
	switch (cdb[0]) {
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
	case WRITE_VERIFY_16:
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
	case ALLOW_MEDIUM_REMOVAL:
	case START_STOP:
		return 1;
	default:
		break;
	}
	if (!optical)
		return 0;
	switch (cdb[0]) {
	case SET_CD_SPEED:
	case 0xB6: /* SET STREAMING */
	case 0xA1: /* BLANK */
	case 0x5B: /* CLOSE TRACK/SESSION */
	case 0x53: /* RESERVE TRACK */
	case 0x5D: /* SEND CUE SHEET */
	case 0x54: /* SEND OPC INFORMATION */
	case FORMAT_UNIT:
		return 1;
	default:
		return 0;
	}
}

static long msd_sgio(usb_msd_device_t *msd, block_device_t *bdev, void *argp,
		     int open_flags)
{
	sg_io_hdr_t h;
	struct msd_passthru pt;
	struct blk_request req;
	sg_iovec_t *iov = NULL;
	uint8_t *kbuf = NULL;
	long rc = 0;

	might_sleep();
	if (!msd->ready)
		return -ENODEV;
	if (copy_from_user(&h, argp, sizeof(h)))
		return -EFAULT;
	if (h.interface_id != 'S')
		return -ENOSYS;
	if (h.cmd_len < 6 || h.cmd_len > 16 || !h.cmdp)
		return -EINVAL;
	msd_memset(&pt, 0, sizeof(pt));
	if (copy_from_user(pt.cdb, h.cmdp, h.cmd_len))
		return -EFAULT;
	pt.cdb_len = h.cmd_len;
	/* Root sends anything; otherwise the harmless list on a node opened
	 * for reading, plus the medium-writing list on one opened for
	 * writing. */
	if (!capable()) {
		int acc = open_flags & 3;
		int ok = 0;

		if (acc != O_WRONLY && msd_sgio_cmd_harmless(pt.cdb))
			ok = 1;
		if (acc != O_RDONLY &&
		    msd_sgio_cmd_write_safe(pt.cdb, (msd->device_type == TYPE_ROM ||
						     msd->device_type == TYPE_WORM)))
			ok = 1;
		if (!ok)
			return -EPERM;
	}

	if (h.dxfer_len) {
		switch (h.dxfer_direction) {
		case SG_DXFER_TO_DEV:
			pt.data_in = 0;
			break;
		case SG_DXFER_FROM_DEV:
		case SG_DXFER_TO_FROM_DEV:
			pt.data_in = 1;
			break;
		default:
			return -EINVAL;
		}
		if (h.dxfer_len > USB_MSD_SGIO_MAX_BYTES || !h.dxferp)
			return -EINVAL;
		pt.len = h.dxfer_len;
		kbuf = (uint8_t *)kalloc(pt.len);
		if (!kbuf)
			return -ENOMEM;
		msd_memset(kbuf, 0, pt.len);
		pt.buf = kbuf;
		if (h.iovec_count) {
			if (h.iovec_count > 64) {
				rc = -EINVAL;
				goto out;
			}
			iov = (sg_iovec_t *)kalloc(sizeof(*iov) * h.iovec_count);
			if (!iov) {
				rc = -ENOMEM;
				goto out;
			}
			if (copy_from_user(iov, h.dxferp,
					   sizeof(*iov) * h.iovec_count)) {
				rc = -EFAULT;
				goto out;
			}
		}
		/* data to the device (TO_DEV, and TO_FROM_DEV's initial copy) */
		if (h.dxfer_direction != SG_DXFER_FROM_DEV) {
			uint32_t done = 0;
			if (!iov) {
				if (copy_from_user(kbuf, h.dxferp, pt.len)) {
					rc = -EFAULT;
					goto out;
				}
			} else {
				for (unsigned i = 0;
				     i < h.iovec_count && done < pt.len; i++) {
					uint32_t n = (uint32_t)iov[i].iov_len;
					if (n > pt.len - done)
						n = pt.len - done;
					if (n && copy_from_user(kbuf + done,
								iov[i].iov_base, n)) {
						rc = -EFAULT;
						goto out;
					}
					done += n;
				}
			}
		}
	}

	uint64_t t0 = timer_ticks();
	blk_request_init(&req, bdev, pt.data_in ? BLK_OP_DRV_IN : BLK_OP_DRV_OUT,
			 0, 0);
	req.drv_cmd = &pt;
	req.flags |= BLK_REQ_NO_ACCOUNT | BLK_REQ_SYNC;
	if (!(h.flags & SG_FLAG_Q_AT_TAIL))
		req.flags |= BLK_REQ_HEAD;
	req.timeout_ms = h.timeout ? h.timeout : SG_DEFAULT_TIMEOUT_MS;
	rc = blk_execute(&req);
	blk_request_release(&req);
	if (rc)
		goto out;

	/* data from the device */
	if (pt.buf && pt.data_in) {
		uint32_t got = pt.len - pt.resid;
		if (!iov) {
			if (got && copy_to_user(h.dxferp, kbuf, got)) {
				rc = -EFAULT;
				goto out;
			}
		} else {
			uint32_t done = 0;
			for (unsigned i = 0; i < h.iovec_count && done < got;
			     i++) {
				uint32_t n = (uint32_t)iov[i].iov_len;
				if (n > got - done)
					n = got - done;
				if (n && copy_to_user(iov[i].iov_base, kbuf + done,
						      n)) {
					rc = -EFAULT;
					goto out;
				}
				done += n;
			}
		}
	}
	h.status = pt.status;
	h.masked_status = (unsigned char)((pt.status >> 1) & 0x1f);
	h.msg_status = 0;
	h.host_status = pt.host_status;
	h.driver_status = pt.driver_status;
	h.resid = (int)pt.resid;
	h.sb_len_wr = 0;
	if (pt.sense_len && h.mx_sb_len && h.sbp) {
		uint8_t n = pt.sense_len < h.mx_sb_len ? pt.sense_len :
							 h.mx_sb_len;
		if (copy_to_user(h.sbp, pt.sense, n)) {
			rc = -EFAULT;
			goto out;
		}
		h.sb_len_wr = n;
	}
	h.duration = (unsigned int)((timer_ticks() - t0) * 1000 /
				    (timer_ms_to_ticks(1000) ?
					     timer_ms_to_ticks(1000) :
					     1));
	h.info = (h.status || h.host_status || h.driver_status) ?
			 SG_INFO_CHECK :
			 SG_INFO_OK;
	if (copy_to_user(argp, &h, sizeof(h)))
		rc = -EFAULT;
out:
	kfree(iov);
	kfree(kbuf);
	return rc;
}

static long usb_msd_blk_ioctl_mode(block_device_t *bdev, unsigned long cmd,
				   void *argp, struct task *cur,
				   int open_flags);

/* blk_ops.ioctl: without the node's open mode -- a read-only open. */
static long usb_msd_blk_ioctl(block_device_t *bdev, unsigned long cmd,
			      void *argp, struct task *cur)
{
	return usb_msd_blk_ioctl_mode(bdev, cmd, argp, cur, O_RDONLY);
}

/* blk_ops.ioctl_mode: `open_flags' of the node the request came through
 * decide which SG_IO commands an unprivileged caller may send. */
static long usb_msd_blk_ioctl_mode(block_device_t *bdev, unsigned long cmd,
				   void *argp, struct task *cur,
				   int open_flags)
{
	usb_msd_device_t *msd = msd_from_bdev(bdev);
	(void)cur;
	if (!msd)
		return -ENODEV;
	/* SG_IO through a partition node addresses the whole disk with
	 * absolute LBAs: root only there, everybody else gets the generic
	 * block requests. */
	if (bdev->part && !capable())
		return -ENOTTY;
	switch (cmd) {
	case SG_IO: {
		/* Pass-through addresses the whole device; through a
		 * partition node it reaches the disk the partition is on. */
		if (!msd->disk)
			return -ENODEV;
		return msd_sgio(msd, &msd->disk->bdev, argp, open_flags);
	}
	case SG_EMULATED_HOST: {
		/* A real SCSI target behind the transport, not a translation */
		int v = 0;
		return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY; /* the generic BLK* / HDIO / SG_* handling */
	}
}

/* Flush the device's cache (shutdown / suspend). */
static int msd_flush_direct(usb_msd_device_t *msd)
{
	might_sleep();
	if (!msd->ready)
		return -ENODEV;
	msd_io_lock(msd);
	int st = usb_msd_sync(msd);
	msd_io_unlock(msd);
	return msd_st_to_errno(st);
}

static int usb_msd_blk_shutdown(struct gendisk *disk,
				enum blk_shutdown_mode mode)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	(void)mode;
	if (!msd)
		return -ENODEV;
	int rc = msd_flush_direct(msd);
	if (rc)
		kprintf("[usb-storage] %s: cache flush at shutdown failed (%d)\n",
			disk->disk_name, rc);
	return rc;
}

static int usb_msd_blk_suspend(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	return msd ? msd_flush_direct(msd) : -ENODEV;
}

/* Resume: the device must answer TEST UNIT READY again (a stick that was
 * powered down reports a unit attention first; REQUEST SENSE clears it). */
static int usb_msd_blk_resume(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	int st = ST_ERR;
	might_sleep();
	if (!msd)
		return -ENODEV;
	msd_io_lock(msd);
	for (int i = 0; i < 3 && st != ST_OK; i++) {
		st = usb_msd_test_unit_ready(msd);
		if (st != ST_OK) {
			scsi_sense_data_t sense;
			usb_msd_request_sense(msd, &sense);
		}
	}
	msd_io_unlock(msd);
	return msd_st_to_errno(st);
}

/* Revalidate: read the capacity again (a card reader's medium changed). */
static int usb_msd_blk_revalidate(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	uint32_t count = 0, size = 0;
	might_sleep();
	if (!msd)
		return -ENODEV;
	msd_io_lock(msd);
	int st = usb_msd_read_capacity(msd, &count, &size);
	msd_io_unlock(msd);
	if (st != ST_OK)
		return msd_st_to_errno(st);
	if (size != msd->block_size) {
		kprintf("[usb-storage] %s: block size changed %u -> %u; not "
			"supported without re-plugging\n",
			disk->disk_name, msd->block_size, size);
		return -EIO;
	}
	if (count != msd->block_count) {
		msd->block_count = count;
		return disk_set_capacity(disk, count);
	}
	return 0;
}

static void usb_msd_blk_free_disk(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	if (msd && msd->disk == disk)
		msd->disk = NULL;
}

static const struct blk_ops usb_msd_blk_ops = {
	.name = "usb-storage",
	.submit = usb_msd_blk_submit,
	.poll = usb_msd_blk_poll,
	.ioctl = usb_msd_blk_ioctl,
	.ioctl_mode = usb_msd_blk_ioctl_mode,
	.shutdown = usb_msd_blk_shutdown,
	.suspend = usb_msd_blk_suspend,
	.resume = usb_msd_blk_resume,
	.revalidate = usb_msd_blk_revalidate,
	.free_disk = usb_msd_blk_free_disk,
};

/* Start the worker thread of `msd'. */
static void msd_start_worker(usb_msd_device_t *msd)
{
	if (msd->worker || msd->index < 0 || msd->index >= 8)
		return;
	task_t *t = sched_add_task(msd_worker, msd,
				   g_msd_worker_stacks[msd->index],
				   sizeof(g_msd_worker_stacks[msd->index]));
	if (!t) {
		kprintf("[usb-storage] could not start the worker thread; I/O "
			"runs in the caller's context\n");
		return;
	}
	ksnprintf(t->comm, sizeof(t->comm), "kusbmsd%d", msd->index);
	msd->worker = t;
}

/* Register the device as a disk of the disk layer: sdX (srN for an optical
 * drive), removable, its partitions scanned by disk_add().  0 or -errno. */
static int msd_register_disk(usb_msd_device_t *msd)
{
	const int optical = ((msd->device_type & 0x1f) == TYPE_ROM ||
			     (msd->device_type & 0x1f) == TYPE_WORM);
	struct gendisk *disk;
	int rc;

	disk = disk_alloc(optical ? BLK_DISK_SR : BLK_DISK_SD, &usb_msd_blk_ops,
			  msd);
	if (!disk)
		return -ENOMEM;

	struct blk_limits *lim = &disk->limits;
	msd_memset(lim, 0, sizeof(*lim));
	lim->logical_block_size = msd->block_size;
	lim->physical_block_size = msd->block_size;
	lim->io_min = msd->block_size;
	lim->io_opt = 0;
	lim->alignment_offset = 0;
	lim->max_hw_sectors = USB_MSD_MAX_BLOCKS;
	lim->max_sectors = USB_MSD_MAX_BLOCKS;
	lim->max_segments = 128;
	lim->max_segment_size = USB_MSD_MAX_BLOCKS * msd->block_size;
	lim->seg_boundary_mask = ~0ULL;
	lim->dma_alignment = 0; /* everything goes through a bounce buffer */
	lim->discard_granularity = 0;
	lim->max_discard_sectors = 0;
	lim->max_write_zeroes_sectors = 0;
	lim->queue_depth = USB_MSD_QUEUE_DEPTH;
	lim->rotational = 1; /* unknown: a USB disk may spin */
	lim->write_cache = 1; /* SYNCHRONIZE CACHE is sent, as always */
	lim->fua = 0;
	lim->discard_zeroes_data = 0;
	lim->secure_erase = 0;

	disk->capacity = msd->block_count;
	disk->flags |= GENHD_FL_REMOVABLE | GENHD_FL_MEDIA_PRESENT;
	if (optical)
		disk->flags |= GENHD_FL_NO_PART_SCAN | GENHD_FL_RO;
	msd_strlcpy(disk->vendor, sizeof(disk->vendor), msd->vendor);
	msd_strlcpy(disk->model, sizeof(disk->model), msd->product);
	msd_strlcpy(disk->firmware, sizeof(disk->firmware), msd->revision);
	disk->serial[0] = 0;
	disk->wwn[0] = 0;
	msd_strlcpy(disk->bus, sizeof(disk->bus), "usb");
	disk->pci = NULL;
	disk->host_no = USB_MSD_SCSI_HOST_BASE + msd->index;
	disk->channel = 0;
	disk->target = 0;
	disk->lun = 0;
	disk->scsi_type = optical ? TYPE_ROM : TYPE_DISK;
	disk->bdev.ready = 1;

	msd->disk = disk;
	rc = disk_add(disk);
	if (rc) {
		msd->disk = NULL;
		disk_put(disk);
		return rc;
	}
	/* The device answers I/O; disk_add filled the block device. */
	disk->bdev.removable = 1;
	if (!disk->bdev.release_locks_for_task)
		disk->bdev.release_locks_for_task = usb_msd_block_release_locks;
	__asm__ volatile("" ::: "memory");
	disk->bdev.ready = 1;
	kprintf("[usb-storage] %s: %.8s %.16s rev %.4s, %u x %u-byte blocks%s\n",
		disk->disk_name, msd->vendor, msd->product, msd->revision,
		msd->block_count, msd->block_size,
		optical ? " (optical)" : "");
	return 0;
}

//=============================================================================
// MSD Initialization
//=============================================================================

int usb_msd_init(usb_msd_device_t *msd, usb_device_t *dev,
		 xhci_controller_t *ctrl)
{
	if (!msd || !dev || !ctrl)
		return ST_INVALID;
	might_sleep();

	/* Initialising a device that is already registered would wipe the
	 * state its worker thread and its disk are using. */
	{
		uint64_t flags;
		int known = 0;
		spin_lock_irqsave(&msd_lock, &flags);
		for (int i = 0; i < g_msd_count && i < 8; i++)
			if (g_msd_devices[i] == msd)
				known = 1;
		spin_unlock_irqrestore(&msd_lock, flags);
		if (known && (msd->disk || msd->legacy_registered ||
			      msd->worker)) {
			WARN(1, "usb_msd: device already initialised\n");
			return ST_OK;
		}
	}

	msd_memset(msd, 0, sizeof(*msd));
	msd->usb_dev = dev;
	msd->ctrl = ctrl;
	msd->next_tag = 0x12340000;
	msd->index = -1;

	// Initialize per-device sleeping I/O mutex for SMP safety
	msd->io_locked = 0;
	msd->io_owner = (uint64_t)-1;
	spinlock_init(&msd->io_wait_lock, "msd_io_wait");
	spinlock_init(&msd->rq_lock, "msd_rq");

	// Allocate the persistent, page-aligned CBW/CSW staging buffers once.
	// Reused for every command so the data path never touches the heap.
	// (The data bounce buffer is allocated later, once block_size is known.)
	// On failure, the pointers stay NULL and usb_msd_bot_transfer falls back
	// to per-command allocation, preserving the old behaviour.
	msd->cbw_buf_raw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
	msd->csw_buf_raw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
	if (msd->cbw_buf_raw && msd->csw_buf_raw) {
		msd->cbw_buf = (uint8_t *)(((uint64_t)msd->cbw_buf_raw + 4095) &
					   ~4095ULL);
		msd->csw_buf = (uint8_t *)(((uint64_t)msd->csw_buf_raw + 4095) &
					   ~4095ULL);
	} else {
		if (msd->cbw_buf_raw)
			kfree_dma(msd->cbw_buf_raw);
		if (msd->csw_buf_raw)
			kfree_dma(msd->csw_buf_raw);
		msd->cbw_buf_raw = msd->csw_buf_raw = NULL;
		msd->cbw_buf = msd->csw_buf = NULL;
	}

	msd_dbg("Initializing MSD device...\n");

	// Send Inquiry command
	scsi_inquiry_data_t inquiry;
	int st = usb_msd_inquiry(msd, &inquiry);
	if (st != ST_OK) {
		msd_dbg("Inquiry failed: st=%d\n", st);
		// Continue anyway, some devices don't respond to inquiry immediately
	} else {
		// Copy vendor and product strings
		msd_memcpy(msd->vendor, inquiry.vendor, 8);
		msd->vendor[8] = '\0';
		msd_memcpy(msd->product, inquiry.product, 16);
		msd->product[16] = '\0';
		msd_memcpy(msd->revision, inquiry.revision, 4);
		msd->revision[4] = '\0';
		msd_trim(msd->vendor);
		msd_trim(msd->product);
		msd_trim(msd->revision);
		msd->removable = (inquiry.rmb & 0x80) ? 1 : 0;
		msd->device_type = inquiry.device_type & 0x1f;
	}

	// Wait for device to be ready
	int ready_attempts = 0;
	for (int i = 0; i < 10; i++) {
		st = usb_msd_test_unit_ready(msd);
		if (st == ST_OK) {
			msd_dbg("Device ready after %d attempts\n", i + 1);
			ready_attempts = i + 1;
			break;
		}

		// Request sense to clear any pending condition
		scsi_sense_data_t sense;
		usb_msd_request_sense(msd, &sense);

		// Small delay
		for (volatile int j = 0; j < 100000; j++)
			;
	}

	if (ready_attempts == 0) {
		msd_dbg("Device not ready, continuing anyway...\n");
	}

	// Get capacity
	st = usb_msd_read_capacity(msd, &msd->block_count, &msd->block_size);
	if (st != ST_OK) {
		msd_dbg("Read capacity failed: st=%d\n", st);
		// Use defaults
		msd->block_size = 512;
		msd->block_count = 0;
	}

	// Validate block size
	if (msd->block_size == 0 || msd->block_size > 4096) {
		msd_dbg("Invalid block size %u, defaulting to 512\n",
			msd->block_size);
		msd->block_size = 512;
	}

	// Allocate the persistent contiguous data bounce buffer, sized to the
	// largest chunk the block layer issues (USB_MSD_BLOCK_CHUNK sectors).
	// Done once here so the read/write fast path never calls the
	// contiguous-page allocator (an O(total_pages) scan under a global lock).
	// On failure, data_buf stays NULL and bot_transfer falls back to
	// per-command allocation.
	{
		uint32_t max_xfer = (uint32_t)USB_MSD_BLOCK_CHUNK *
				    msd->block_size;
		size_t pages = ((size_t)max_xfer + 4095) / 4096;
		uint64_t phys = mm_allocate_contiguous_pages(pages);
		if (phys) {
			msd->data_buf_phys = phys;
			msd->data_buf = (uint8_t *)phys_to_virt(phys);
			msd->data_buf_size = (uint32_t)(pages * 4096);
			msd->data_buf_pages = (uint32_t)pages;
		}
	}

	msd->ready = 1;

	// Add to the global list (protected by spinlock); the slot number
	// names the worker thread and the legacy fallback device.
	{
		uint64_t flags;
		spin_lock_irqsave(&msd_lock, &flags);
		for (int i = 0; i < g_msd_count && i < 8; i++)
			if (g_msd_devices[i] == msd)
				msd->index = i; /* re-initialised device */
		if (msd->index < 0 && g_msd_count < 8) {
			msd->index = g_msd_count;
			g_msd_devices[g_msd_count++] = msd;
		}
		spin_unlock_irqrestore(&msd_lock, flags);
	}

	// The old block device, complete: its callbacks are the original
	// synchronous path and serve the legacy registration below.
	ksnprintf(msd->name, sizeof(msd->name), "usbmsd%d",
		  msd->index < 0 ? 9 : msd->index);
	msd->blk.name = msd->name;
	msd->blk.sector_size = msd->block_size;
	msd->blk.total_sectors = msd->block_count;
	msd->blk.read = usb_msd_block_read;
	msd->blk.write = usb_msd_block_write;
	msd->blk.sync = usb_msd_block_sync;
	msd->blk.release_locks_for_task = usb_msd_block_release_locks;
	msd->blk.driver_data = msd;

	// Register with the disk layer: the worker first (the partition scan
	// in disk_add() already reads through it), then the disk.
	msd_start_worker(msd);
	int rc = msd_register_disk(msd);
	if (rc) {
		// The disk layer refused (no memory, names exhausted): keep the
		// stick usable as a plain block device the old way.  The root
		// search (kernel/ke/storage.c) reads its partition table itself.
		kprintf("[usb-storage] disk registration failed (%d); registering "
			"%s as a legacy block device\n",
			rc, msd->name);
		if (block_register(&msd->blk) == ST_OK)
			msd->legacy_registered = 1;
		else
			kprintf("[usb-storage] %s: block registry full\n",
				msd->name);
	}

	return ST_OK;
}
