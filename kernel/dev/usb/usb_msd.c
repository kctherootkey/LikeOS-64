// LikeOS-64 - USB Mass Storage (Bulk-Only Transport) Driver
// Clean implementation following USB Mass Storage Class specification
//
// BOT Protocol:
// 1. Send Command Block Wrapper (CBW) via bulk OUT
// 2. Transfer data (if any) via bulk IN or OUT
// 3. Receive Command Status Wrapper (CSW) via bulk IN

#include <kernel/dev/usb/usb_msd.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
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

static int msd_strlen(const char *s)
{
	int len = 0;
	while (*s++)
		len++;
	return len;
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

	if (!msd || !msd->ready) {
		return ST_INVALID;
	}

	msd_memset(&cbw, 0, sizeof(cbw));
	cbw.signature = CBW_SIGNATURE;
	cbw.tag = ++msd->next_tag;
	cbw.data_transfer_len = 0; // No data transfer
	cbw.flags = CBW_FLAG_DATA_IN; // No data but use IN for status
	cbw.lun = 0;
	cbw.cb_length = 10;
	cbw.cb[0] = SCSI_SYNCHRONIZE_CACHE_10;
	// All other bytes are 0: sync entire device

	msd_dbg("Sync: sending SYNCHRONIZE_CACHE_10\n");

	return usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
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
			spin_unlock_irqrestore(&msd->io_wait_lock, flags);
			return;
		}
		// Device busy — sleep until the holder releases it.
		if (cur) {
			cur->state = TASK_BLOCKED;
			cur->wait_channel = &msd->io_locked;
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
	sched_wake_channel(&msd->io_locked);
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
		sched_wake_channel(&msd->io_locked);
	}
	return released;
}

int usb_msd_block_read(block_device_t *dev, unsigned long lba,
		       unsigned long count, void *buf)
{
	BUG_ON(dev == NULL);
	BUG_ON(buf == NULL && count > 0);
	usb_msd_device_t *msd = (usb_msd_device_t *)dev->driver_data;

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
	usb_msd_device_t *msd = (usb_msd_device_t *)dev->driver_data;

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
	usb_msd_device_t *msd = (usb_msd_device_t *)dev->driver_data;

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
	usb_msd_device_t *msd = (usb_msd_device_t *)dev->driver_data;
	if (!msd)
		return 0;
	return usb_msd_io_release_if_owner(msd, task_id);
}

//=============================================================================
// MSD Initialization
//=============================================================================

int usb_msd_init(usb_msd_device_t *msd, usb_device_t *dev,
		 xhci_controller_t *ctrl)
{
	if (!msd || !dev || !ctrl)
		return ST_INVALID;

	msd_memset(msd, 0, sizeof(*msd));
	msd->usb_dev = dev;
	msd->ctrl = ctrl;
	msd->next_tag = 0x12340000;

	// Initialize per-device sleeping I/O mutex for SMP safety
	msd->io_locked = 0;
	msd->io_owner = (uint64_t)-1;
	spinlock_init(&msd->io_wait_lock, "msd_io_wait");

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
		msd->removable = (inquiry.rmb & 0x80) ? 1 : 0;
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

	// Setup block device
	msd->blk.name = "usb0";
	msd->blk.sector_size = msd->block_size;
	msd->blk.total_sectors = msd->block_count;
	msd->blk.read = usb_msd_block_read;
	msd->blk.write = usb_msd_block_write;
	msd->blk.sync = usb_msd_block_sync;
	msd->blk.release_locks_for_task = usb_msd_block_release_locks;
	msd->blk.driver_data = msd;

	// Register block device
	if (block_register(&msd->blk) == ST_OK) {
		msd_dbg("Block device registered: %s\n", msd->blk.name);
	}

	// Add to global list (protected by spinlock)
	uint64_t flags;
	spin_lock_irqsave(&msd_lock, &flags);
	if (g_msd_count < 8) {
		g_msd_devices[g_msd_count++] = msd;
	}
	spin_unlock_irqrestore(&msd_lock, flags);

	return ST_OK;
}
