// LikeOS - USB Mass Storage (Bulk-Only Transport) Driver
// Clean implementation following USB Mass Storage Class specification
//
// BOT Protocol:
// 1. Send Command Block Wrapper (CBW) via bulk OUT
// 2. Transfer data (if any) via bulk IN or OUT
// 3. Receive Command Status Wrapper (CSW) via bulk IN
// A failed stage is recovered as the class specification says: a halted
// pipe is cleared (CLEAR_FEATURE ENDPOINT_HALT) and the status read again;
// an invalid status or a phase error takes the reset recovery (Bulk-Only
// Mass Storage Reset).  A device that does not even take that any more is
// given up: it is detached like an unplugged one.
//
// Structure:
//   host  -- one USB mass-storage interface, the transport to it: its I/O
//            mutex (one BOT command at a time), DMA buffers, request ring
//            and worker thread "kusbmsd<N>";
//   LUN   -- one logical unit of a host: a disk of the disk layer.
// Both are static slots.  A host slot is claimed when a device is attached
// and becomes free again once the device is detached AND the last of its
// disks has been released by the disk layer (free_disk) -- a disk that is
// still mounted or open keeps its LUN and host slot, so no structure the
// disk layer can still reach is ever reused under it.
//
// Contexts:
//   - attach and detach run on the attach thread "kusbstor" (they sleep:
//     the device may take seconds to become ready, and disk_add/disk_del
//     do I/O and wait for the queue); a stick present at boot is attached by
//     the boot enumeration itself, on the device-poll thread;
//   - usb_msd_disconnect() runs on the device-poll thread when the port
//     reports the device gone: it marks the transport dead and waits on the
//     I/O mutex until no transfer runs (they abort at once on a gone
//     device), so the controller can disable the slot right after;
//   - requests run on the host's worker.
//
// Lock order: g_msd_lock (spinlock, leaf, irqsave) protects the slot states,
// hold counts and the attach queue; it is never held across a
// transfer, a sleep or a call into the disk layer.  host->rq_lock is a leaf
// as well.  The host's I/O mutex is taken outside both.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/block/blkdev.h> /* first: it includes ke/syscall.h */
#include <kernel/dev/usb/usb_msd.h>
#include <kernel/dev/usb/usb.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/hal/lapic.h>
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

#define MSD_LOG "[usb-storage] "

/* How long a logical unit may take to become ready after the attach (TEST
 * UNIT READY answering NOT READY / becoming ready), and the pause between
 * two tries. */
#define MSD_READY_TIMEOUT_MS 10000
#define MSD_READY_RETRY_MS 100
/* The same wait in the boot enumeration, which runs on the device-poll
 * thread (console input waits meanwhile): a unit still not ready after this
 * is handed to the attach thread, which waits for it as long as above. */
#define MSD_BOOT_READY_MS 1000
/* Attach attempts per connection of a device */
#define MSD_ATTACH_ATTEMPTS 3
/* Idle medium checks of card-reader slots */
#define MSD_MEDIA_POLL_MS 2000

// Slot states, hold counts, the attach queue (leaf spinlock)
static spinlock_t g_msd_lock = SPINLOCK_INIT("usb_msd");

usb_msd_host_t g_msd_hosts[USB_MSD_MAX_HOSTS];
usb_msd_device_t g_msd_devices[USB_MSD_MAX_DEVICES];

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

/* Sleep `ms' milliseconds where that is possible, else spin. */
static void msd_msleep(uint32_t ms)
{
	if (!ms)
		return;
	if (blk_can_sleep()) {
		uint64_t until = hrtimer_now_ns() + (uint64_t)ms * 1000000ULL;

		while (hrtimer_sleep_until(until, NULL) != 0 &&
		       hrtimer_now_ns() < until)
			;
		return;
	}
	lapic_delay_ms(ms);
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

/* The transport is dead: nothing may be sent any more. */
static inline int msd_host_dead(const usb_msd_host_t *h)
{
	return !h || h->gone;
}

//=============================================================================
// The host's I/O mutex
//=============================================================================

// Sleeping mutex for transport serialization.
// Unlike a spinlock, this blocks the calling task (via the scheduler) when
// the device is busy, keeping IRQs enabled so TLB shootdowns, timer ticks,
// and other IPIs can still be serviced.
static void msd_io_lock(usb_msd_host_t *h)
{
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;
	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&h->io_wait_lock, &flags);
		if (!h->io_locked) {
			h->io_locked = 1;
			h->io_owner = my_id;
			task_sleep_lock_taken();
			spin_unlock_irqrestore(&h->io_wait_lock, flags);
			return;
		}
		// Device busy — sleep until the holder releases it.
		if (cur) {
			cur->wait_channel = (void *)&h->io_locked;
			task_set_blocked(cur);
		}
		spin_unlock_irqrestore(&h->io_wait_lock, flags);
		sched_schedule(); // yields with IRQs enabled
	}
}

static void msd_io_unlock(usb_msd_host_t *h)
{
	uint64_t flags;
	spin_lock_irqsave(&h->io_wait_lock, &flags);
	h->io_locked = 0;
	h->io_owner = (uint64_t)-1;
	spin_unlock_irqrestore(&h->io_wait_lock, flags);
	task_sleep_lock_dropped();
	sched_wake_channel((void *)&h->io_locked);
}

/* Force-release this host's I/O mutex if it is currently owned by the
 * given task id.  Companion to fat32_io_release_if_owner; called when a
 * task that was killed mid-FS-write may have died while sleeping inside
 * usb_msd_block_read/write holding both locks.
 *
 * Also clears the xHCI pending_xfer entries for this host's bulk endpoints:
 * the dying task may have died inside xhci_bulk_transfer_in/out, leaving
 * a pointer to its (about-to-be-freed) kernel stack in pending_xfer.
 * Without this, the next bulk-completion IRQ writes through the dangling
 * pointer and the kernel oopses with a page fault in
 * xhci_handle_transfer_event. */
int usb_msd_io_release_if_owner(usb_msd_host_t *h, uint64_t task_id)
{
	if (!h)
		return 0;
	uint64_t flags;
	int released = 0;
	spin_lock_irqsave(&h->io_wait_lock, &flags);
	if (h->io_locked && h->io_owner == task_id) {
		h->io_locked = 0;
		h->io_owner = (uint64_t)-1;
		released = 1;
	}
	spin_unlock_irqrestore(&h->io_wait_lock, flags);
	if (released) {
		/* Defang any in-flight transfer events for this host before
		 * the dying task's kernel stack is freed.  Clear pending_xfer
		 * so an in-flight transfer-event IRQ harmlessly skips the
		 * write (no dangling-pointer fault).  Don't reset the
		 * endpoints here: with an endpoint in RUNNING state and the
		 * controller still draining queued TRBs, the Reset EP command
		 * can time out — and the next bulk transfer's own error path
		 * already resets the endpoint when it sees the orphan TRBs
		 * cause a transaction error.  A gone device has no transfers
		 * left to defang (its slot is disabled). */
		usb_device_t *dev = h->usb_dev;
		if (!h->gone && h->ctrl && dev) {
			uint8_t slot = dev->slot_id;
			if (slot && dev->bulk_in_ep) {
				xhci_clear_pending_xfer(h->ctrl, slot,
							dev->bulk_in_ep * 2 + 1);
			}
			if (slot && dev->bulk_out_ep) {
				xhci_clear_pending_xfer(h->ctrl, slot,
							dev->bulk_out_ep * 2);
			}
		}
		sched_wake_channel((void *)&h->io_locked);
	}
	return released;
}

//=============================================================================
// Transport recovery
//=============================================================================

static void msd_kick_attach_thread(void);

/* A control transfer to the device (I/O mutex held).  `data' must be the
 * host's DMA control buffer (or NULL without a data stage). */
static int msd_control(usb_msd_host_t *h, uint8_t rt, uint8_t req,
		       uint16_t val, uint16_t idx, uint16_t len, void *data)
{
	if (msd_host_dead(h) || !h->usb_dev || !h->ctrl)
		return ST_NO_DEVICE;
	return xhci_control_transfer(h->ctrl, h->usb_dev, rt, req, val, idx,
				     len, data);
}

/* CLEAR_FEATURE(ENDPOINT_HALT) of a bulk endpoint.  The host side of the
 * endpoint was reset by the transfer that failed on it (the xHCI transfer
 * functions reset the endpoint on every failure), so the data toggles of
 * both ends start over together. */
static int msd_clear_halt(usb_msd_host_t *h, uint8_t ep_addr)
{
	return msd_control(h, USB_RT_H2D | USB_RT_STD | USB_RT_EP,
			   USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, ep_addr,
			   0, NULL);
}

/* The device stopped answering: give it up.  Its transport fails every
 * command from now on and its disks are detached as for an unplugged
 * device; it works again once re-plugged.  I/O mutex held (or the attach
 * still owns the host). */
static void msd_host_fail(usb_msd_host_t *h, const char *why)
{
	uint64_t flags;
	int kick = 0;

	if (h->gone)
		return;
	/* Not attached again while it stays plugged in (the I/O mutex is
	 * held or the attach owns the host: the device is still ours). */
	if (h->usb_dev)
		h->usb_dev->msd_attempts = MSD_ATTACH_ATTEMPTS;
	h->failed = 1;
	h->gone = 1;
	__asm__ volatile("" ::: "memory");
	kprintf(MSD_LOG "port %u: %s; device given up (re-plug it to use it "
		"again)\n",
		h->port, why);
	spin_lock_irqsave(&g_msd_lock, &flags);
	if (h->state == USB_MSD_HOST_LIVE) {
		h->state = USB_MSD_HOST_GONE;
		h->detach_pending = 1;
		kick = 1;
	}
	/* (A host still attaching is finished by its attacher.) */
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (kick)
		msd_kick_attach_thread();
}

/* Reset recovery (Bulk-Only Transport 5.3.4): a Bulk-Only Mass Storage
 * Reset, then the halt of each bulk pipe the failed command left halted is
 * cleared.  A device that refuses the reset is given up.  I/O mutex held. */
static int msd_reset_recovery(usb_msd_host_t *h, int in_halted,
			      int out_halted)
{
	int st;

	if (msd_host_dead(h))
		return ST_NO_DEVICE;
	h->nr_resets++;
	st = msd_control(h, USB_RT_H2D | USB_RT_CLASS | USB_RT_IFACE,
			 USB_MSD_REQ_RESET, 0, h->iface, 0, NULL);
	if (st != ST_OK) {
		if (st != ST_NO_DEVICE)
			msd_host_fail(h, "the Bulk-Only reset failed");
		return ST_NO_DEVICE;
	}
	if (in_halted)
		(void)msd_clear_halt(h, h->bulk_in_addr);
	if (out_halted)
		(void)msd_clear_halt(h, h->bulk_out_addr);
	return ST_OK;
}

//=============================================================================
// BOT Protocol Core
//=============================================================================

/* A transfer status after a failure: ST_NO_DEVICE once the transport is
 * dead, `st' otherwise. */
static inline int msd_fail_st(usb_msd_host_t *h, int st)
{
	return (st == ST_NO_DEVICE || h->gone) ? ST_NO_DEVICE : st;
}

int usb_msd_bot_transfer(usb_msd_device_t *msd, usb_msd_cbw_t *cbw,
			 void *data_buf, uint32_t data_len, usb_msd_csw_t *csw)
{
	BUG_ON(cbw == NULL);
	BUG_ON(csw == NULL);
	BUILD_BUG_ON(CBW_SIZE != 31);
	BUILD_BUG_ON(CSW_SIZE != 13);
	if (!msd || !msd->host)
		return ST_INVALID;

	usb_msd_host_t *h = msd->host;
	if (msd_host_dead(h) || !h->usb_dev || !h->ctrl)
		return ST_NO_DEVICE;

	xhci_controller_t *ctrl = h->ctrl;
	usb_device_t *dev = h->usb_dev;
	uint32_t transferred;
	int st;
	int in_err = 0, out_err = 0;

	cbw->lun = msd->lun;

	// Use the host's persistent page-aligned CBW/CSW staging buffers if
	// available; otherwise fall back to a one-shot allocation (only happens
	// if the one-time allocation failed).  raw_cbw/raw_csw are non-NULL
	// only for the fallback path, so the kfree_dma() calls below are no-ops
	// (kfree_dma(NULL) returns immediately) when the persistent buffers are
	// used.  Page alignment guarantees the tiny 31/13-byte transfers never
	// straddle a 64 KB DMA boundary.
	uint8_t *raw_cbw = NULL, *raw_csw = NULL;
	uint8_t *dma_cbw, *dma_csw;
	if (h->cbw_buf && h->csw_buf) {
		dma_cbw = h->cbw_buf;
		dma_csw = h->csw_buf;
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
		st = msd_fail_st(h, st);
		/* The device did not take the command block: reset
		 * recovery (BOT 6.6.1). */
		if (st != ST_NO_DEVICE) {
			(void)msd_reset_recovery(h, 0, 1);
			st = msd_fail_st(h, ST_IO);
		}
		return st;
	}

	// Phase 2: Data transfer (if any)
	// The DMA buffer must be PHYSICALLY CONTIGUOUS: the kernel heap only
	// guarantees virtual contiguity, but for transfers > 1 page the xHCI
	// controller writes sequentially in physical memory, so non-contiguous
	// pages would cause silent memory corruption.  Use the host's
	// persistent contiguous bounce buffer when the transfer fits; only fall
	// back to a one-shot contiguous allocation for oversized transfers (the
	// block layer never issues those, so the hot path never allocates).
	// A one-shot buffer is kept until the status is in: after a stalled
	// data stage the status says how much of it the device did fill.
	int data_st = ST_OK;
	uint32_t data_done = 0; // bytes the data stage moved
	uint64_t data_phys = 0;
	size_t data_pages = 0;
	uint8_t *dma_data = NULL;
	const int data_in = (cbw->flags & CBW_FLAG_DATA_IN) != 0;
	if (data_len > 0 && data_buf) {
		if (h->data_buf && data_len <= h->data_buf_size) {
			dma_data = h->data_buf;
		} else {
			data_pages = ((uint32_t)data_len + 4095) / 4096;
			data_phys = mm_allocate_contiguous_pages(data_pages);
			if (!data_phys) {
				msd_dbg("Failed to allocate contiguous DMA buffer for data\n");
				kfree_dma(raw_cbw);
				kfree_dma(raw_csw);
				/* The command block is out without its data
				 * stage: put the device back in step. */
				(void)msd_reset_recovery(h, 0, 0);
				return ST_NOMEM;
			}
			dma_data = (uint8_t *)phys_to_virt(data_phys);
		}

		if (data_in) {
			// Data IN
			msd_dbg("Data IN: %d bytes\n", data_len);
			data_st = xhci_bulk_transfer_in(ctrl, dev, dma_data,
							data_len, &transferred);
			if (data_st == ST_OK) {
				data_done = transferred;
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
			} else {
				in_err = 1;
			}
		} else {
			// Data OUT - copy data to DMA buffer first
			msd_memcpy(dma_data, data_buf, data_len);
			msd_dbg("Data OUT: %d bytes\n", data_len);
			data_st = xhci_bulk_transfer_out(
				ctrl, dev, dma_data, data_len, &transferred);
			if (data_st != ST_OK)
				out_err = 1;
			else
				data_done = transferred;
		}

		if (data_st != ST_OK) {
			msd_dbg("Data transfer failed: st=%d\n", data_st);
			if (msd_fail_st(h, data_st) == ST_NO_DEVICE) {
				st = ST_NO_DEVICE;
				goto out;
			}
			/* The device ended the data stage with a STALL (or
			 * the transfer failed): clear the pipe's halt, then
			 * read the status as usual (BOT 6.7.2/6.7.3). */
			(void)msd_clear_halt(h, in_err ? h->bulk_in_addr :
							 h->bulk_out_addr);
		}
	}

	// Phase 3: Receive CSW (a second try after clearing a halted IN pipe)
	for (int attempt = 0;; attempt++) {
		msd_memset(dma_csw, 0, CSW_SIZE);
		st = xhci_bulk_transfer_in(ctrl, dev, dma_csw, CSW_SIZE,
					   &transferred);
		if (st == ST_OK)
			break;
		msd_dbg("CSW receive failed: st=%d\n", st);
		st = msd_fail_st(h, st);
		if (st == ST_NO_DEVICE)
			goto out;
		if (attempt == 0) {
			(void)msd_clear_halt(h, h->bulk_in_addr);
			continue;
		}
		(void)msd_reset_recovery(h, 1, out_err);
		st = msd_fail_st(h, st);
		goto out;
	}

	// Copy CSW back to caller's buffer
	msd_memcpy(csw, dma_csw, CSW_SIZE);

	// Validate CSW
	if (csw->signature != CSW_SIGNATURE) {
		WARN_RATELIMIT(
			1,
			"usb_msd: CSW signature 0x%08x != expected 0x%08x: hardware/firmware bug or USB corruption",
			csw->signature, CSW_SIGNATURE);
		msd_dbg("Invalid CSW signature: %08x\n", csw->signature);
		(void)msd_reset_recovery(h, 0, 0);
		st = msd_fail_st(h, ST_IO);
		goto out;
	}

	if (csw->tag != cbw->tag) {
		WARN_RATELIMIT(
			1,
			"usb_msd: CSW tag 0x%08x != CBW tag 0x%08x: out-of-order response or firmware bug",
			csw->tag, cbw->tag);
		msd_dbg("CSW tag mismatch: expected %08x, got %08x\n", cbw->tag,
			csw->tag);
		(void)msd_reset_recovery(h, 0, 0);
		st = msd_fail_st(h, ST_IO);
		goto out;
	}

	msd_dbg("CSW: status=%d, residue=%d\n", csw->status, csw->data_residue);

	if (csw->status == CSW_STATUS_PASSED) {
		uint8_t rw_op = cbw->cb[0];
		const int rw = rw_op == SCSI_READ_10 || rw_op == SCSI_WRITE_10 ||
			       rw_op == SCSI_READ_16 || rw_op == SCSI_WRITE_16;
		st = ST_OK;
		/* A READ/WRITE that moved less than the whole range did not
		 * do its job, whatever the status says. */
		if (rw && data_st == ST_OK && data_buf && data_done < data_len)
			st = ST_IO;
		if (data_st != ST_OK) {
			/* The data stage was cut short (a device with less
			 * data than asked for stalls the IN pipe) and the
			 * command still passed.  What the device did send is
			 * in the bounce buffer: the residue says how much is
			 * missing.  For the medium's own READ/WRITE a short
			 * transfer is an I/O error. */
			uint32_t resid = csw->data_residue < data_len ?
						 csw->data_residue :
						 data_len;
			uint8_t op = cbw->cb[0];
			if (data_in) {
				msd_memcpy(data_buf, dma_data, data_len - resid);
				msd_memset((uint8_t *)data_buf + (data_len - resid),
					   0, resid);
			}
			if (!resid || op == SCSI_READ_10 || op == SCSI_WRITE_10 ||
			    op == SCSI_READ_16 || op == SCSI_WRITE_16)
				st = ST_IO;
		}
	} else if (csw->status == CSW_STATUS_FAILED) {
		st = ST_IO;
	} else {
		/* Phase error: reset recovery (BOT 6.7.2) */
		(void)msd_reset_recovery(h, 0, 0);
		st = msd_fail_st(h, ST_ERR);
	}

out:
	if (data_phys)
		mm_free_contiguous_pages(data_phys, data_pages);
	kfree_dma(raw_cbw);
	kfree_dma(raw_csw);
	return st;
}

//=============================================================================
// SCSI Commands (the host's I/O mutex held)
//=============================================================================

static void msd_cbw_init(usb_msd_device_t *msd, usb_msd_cbw_t *cbw,
			 uint32_t len, uint8_t flags, uint8_t cb_len)
{
	msd_memset(cbw, 0, sizeof(*cbw));
	cbw->signature = CBW_SIGNATURE;
	cbw->tag = ++msd->host->next_tag;
	cbw->data_transfer_len = len;
	cbw->flags = flags;
	cbw->lun = msd->lun;
	cbw->cb_length = cb_len;
}

int usb_msd_test_unit_ready(usb_msd_device_t *msd)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_cbw_init(msd, &cbw, 0, 0, 6);
	cbw.cb[0] = SCSI_TEST_UNIT_READY;

	return usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
}

int usb_msd_inquiry(usb_msd_device_t *msd, scsi_inquiry_data_t *data)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_cbw_init(msd, &cbw, 36, CBW_FLAG_DATA_IN, 6);
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

	msd_cbw_init(msd, &cbw, 18, CBW_FLAG_DATA_IN, 6);
	cbw.cb[0] = SCSI_REQUEST_SENSE;
	cbw.cb[4] = 18; // Allocation length

	msd_memset(data, 0, sizeof(*data));

	return usb_msd_bot_transfer(msd, &cbw, data, 18, &csw);
}

static uint32_t msd_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* READ CAPACITY(10), and READ CAPACITY(16) for a unit with 2^32 blocks or
 * more (the 10-byte answer is all ones then). */
int usb_msd_read_capacity(usb_msd_device_t *msd, uint64_t *block_count,
			  uint32_t *block_size)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;
	uint8_t data[32];

	msd_cbw_init(msd, &cbw, 8, CBW_FLAG_DATA_IN, 10);
	cbw.cb[0] = SCSI_READ_CAPACITY_10;

	msd_memset(data, 0, sizeof(data));

	int st = usb_msd_bot_transfer(msd, &cbw, data, 8, &csw);
	if (st != ST_OK)
		return st;

	uint32_t last = msd_be32(data);
	uint32_t bs = msd_be32(data + 4);
	if (last != 0xFFFFFFFFu) {
		*block_count = (uint64_t)last + 1;
		*block_size = bs;
		msd_dbg("Capacity: %llu blocks, %u bytes/block\n",
			(unsigned long long)*block_count, *block_size);
		return ST_OK;
	}

	msd_cbw_init(msd, &cbw, 32, CBW_FLAG_DATA_IN, 16);
	cbw.cb[0] = SCSI_SERVICE_ACTION_IN_16;
	cbw.cb[1] = SCSI_SAI_READ_CAPACITY_16;
	cbw.cb[13] = 32; // allocation length (bytes 10..13, big-endian)
	msd_memset(data, 0, sizeof(data));
	st = usb_msd_bot_transfer(msd, &cbw, data, 32, &csw);
	if (st != ST_OK)
		return st;
	uint64_t last64 = ((uint64_t)msd_be32(data) << 32) | msd_be32(data + 4);
	*block_count = last64 + 1;
	*block_size = msd_be32(data + 8);
	return ST_OK;
}

/* Max blocks per SCSI READ/WRITE.  Raised from 128 (64KiB) so the
 * filesystem can issue MiB-sized transfers and amortise the per-command
 * (CBW/data/CSW) latency.  2048 blocks = 1 MiB; the xHCI bulk path chains the
 * needed TRBs (TRB_MAX_BUFF_SIZE=64KiB each, ring = 256 TRBs) and SCSI(10)
 * encodes the count in 16 bits, so this is well within both limits. */
#define USB_MSD_MAX_BLOCKS 2048

/* Sectors per BOT transaction issued by the block layer.  Each chunk is one
 * SCSI READ/WRITE, so larger chunks amortise the CBW/CSW round-trips.
 * The persistent DMA bounce buffer (msd_host_buffers) is sized to exactly
 * this many sectors, so the two MUST stay in sync — keep this a single named
 * constant. */
#define USB_MSD_BLOCK_CHUNK 256

/* The command block of a READ or WRITE: the 10-byte form while the range
 * fits in 32 bits, the 16-byte one beyond. */
static void msd_rw_cdb(usb_msd_cbw_t *cbw, int write, uint64_t lba,
		       uint32_t count)
{
	if (lba + count <= 0x100000000ULL) {
		cbw->cb_length = 10;
		cbw->cb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
		// LBA (big-endian)
		cbw->cb[2] = (lba >> 24) & 0xFF;
		cbw->cb[3] = (lba >> 16) & 0xFF;
		cbw->cb[4] = (lba >> 8) & 0xFF;
		cbw->cb[5] = lba & 0xFF;
		// Transfer length in blocks (big-endian)
		cbw->cb[7] = (count >> 8) & 0xFF;
		cbw->cb[8] = count & 0xFF;
		return;
	}
	cbw->cb_length = 16;
	cbw->cb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
	for (int i = 0; i < 8; i++)
		cbw->cb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
	cbw->cb[10] = (count >> 24) & 0xFF;
	cbw->cb[11] = (count >> 16) & 0xFF;
	cbw->cb[12] = (count >> 8) & 0xFF;
	cbw->cb[13] = count & 0xFF;
}

int usb_msd_read(usb_msd_device_t *msd, uint64_t lba, uint32_t count, void *buf)
{
	BUG_ON(buf == NULL);
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	if (count == 0 || count > USB_MSD_MAX_BLOCKS) {
		msd_dbg("Invalid read count: %u\n", count);
		return ST_INVALID;
	}

	uint32_t transfer_len = count * msd->block_size;

	msd_cbw_init(msd, &cbw, transfer_len, CBW_FLAG_DATA_IN, 10);
	msd_rw_cdb(&cbw, 0, lba, count);

	msd_dbg("Read: LBA=%llu, Count=%u, Len=%u\n", (unsigned long long)lba,
		count, transfer_len);

	return usb_msd_bot_transfer(msd, &cbw, buf, transfer_len, &csw);
}

int usb_msd_write(usb_msd_device_t *msd, uint64_t lba, uint32_t count,
		  const void *buf)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	if (count == 0 || count > USB_MSD_MAX_BLOCKS) {
		return ST_INVALID;
	}

	uint32_t transfer_len = count * msd->block_size;

	msd_cbw_init(msd, &cbw, transfer_len, CBW_FLAG_DATA_OUT, 10);
	msd_rw_cdb(&cbw, 1, lba, count);

	return usb_msd_bot_transfer(msd, &cbw, (void *)buf, transfer_len, &csw);
}

int usb_msd_sync(usb_msd_device_t *msd)
{
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	int st;

	if (!msd || !msd->host)
		return ST_INVALID;
	if (msd_host_dead(msd->host))
		return ST_NO_DEVICE;
	/* Refused once as an invalid command: the device has nothing a
	 * flush could reach, so a flush is complete as it is.  Without a
	 * medium there is nothing to flush either. */
	if (msd->no_sync_cache || !msd->medium)
		return ST_OK;

	msd_cbw_init(msd, &cbw, 0, CBW_FLAG_DATA_IN, 10);
	msd_memset(&csw, 0, sizeof(csw));
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
				kprintf(MSD_LOG "%s: the device refuses "
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
				kprintf(MSD_LOG "%s: SYNCHRONIZE CACHE "
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
// Block Device Interface (the legacy synchronous callbacks)
//=============================================================================

int usb_msd_block_read(block_device_t *dev, unsigned long lba,
		       unsigned long count, void *buf)
{
	BUG_ON(dev == NULL);
	BUG_ON(buf == NULL && count > 0);
	usb_msd_device_t *msd = msd_from_bdev(dev);

	if (!msd || !msd->host || msd_host_dead(msd->host)) {
		return ST_NO_DEVICE;
	}

	// Read in USB_MSD_BLOCK_CHUNK-sector chunks — large enough to amortize
	// the CBW + CSW round-trip overhead per SCSI READ/WRITE transaction.
	// Each chunk reuses the host's persistent bounce buffer.
	//
	// The host mutex is taken PER CHUNK, not across the whole request:
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

		msd_io_lock(msd->host);
		int st = usb_msd_read(msd, (uint64_t)current_lba,
				      (uint32_t)chunk, ptr);
		msd_io_unlock(msd->host);
		if (st != ST_OK) {
			msd_dbg("Block read failed at LBA %lu: st=%d\n",
				current_lba, st);
			result = st == ST_NO_DEVICE ? ST_IO : st;
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

	if (!msd || !msd->host || msd_host_dead(msd->host)) {
		return ST_NO_DEVICE;
	}

	// Per-chunk host locking — same rationale as usb_msd_block_read.
	const uint8_t *ptr = (const uint8_t *)buf;
	unsigned long remaining = count;
	unsigned long current_lba = lba;
	int result = ST_OK;

	while (remaining > 0) {
		unsigned long chunk = (remaining > USB_MSD_BLOCK_CHUNK) ?
					      USB_MSD_BLOCK_CHUNK :
					      remaining;

		msd_io_lock(msd->host);
		int st = usb_msd_write(msd, (uint64_t)current_lba,
				       (uint32_t)chunk, ptr);
		msd_io_unlock(msd->host);
		if (st != ST_OK) {
			result = st == ST_NO_DEVICE ? ST_IO : st;
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

	if (!msd || !msd->host || msd_host_dead(msd->host)) {
		return ST_NO_DEVICE;
	}

	// Serialize I/O to this device (sleeping mutex — IRQs stay enabled)
	msd_io_lock(msd->host);
	int result = usb_msd_sync(msd);
	msd_io_unlock(msd->host);
	return result;
}

int usb_msd_block_release_locks(block_device_t *dev, uint64_t task_id)
{
	usb_msd_device_t *msd = msd_from_bdev(dev);
	if (!msd || !msd->host)
		return 0;
	return usb_msd_io_release_if_owner(msd->host, task_id);
}

//=============================================================================
// Disk layer integration
//=============================================================================
//
// The disk layer hands requests to usb_msd_blk_submit() from the task that
// dispatches -- the submitter or kblockd -- and that function must not
// sleep.  A BOT transfer
// sleeps (it waits for xHCI transfer events and takes the host's sleeping
// I/O mutex), so submit only parks the request in the host's ring and
// wakes the worker thread "kusbmsd<N>", which executes the requests of all
// the host's logical units in order and completes each with
// blk_complete_request_direct() (the end of the request runs right there,
// in the worker).  The worker issues READ/WRITE in USB_MSD_BLOCK_CHUNK-sector
// pieces, each under the host's I/O mutex, and SYNCHRONIZE CACHE(10) for a
// flush.  Once the device is gone, every request fails with -EIO without
// touching the controller.
//
// Lock order: rq_lock is a leaf spinlock (irqsave) protecting only the
// ring; it is never held across a transfer, a wake-up or a completion.  The
// sleeping I/O mutex (msd_io_lock) is taken by the worker per chunk.

static uint8_t g_msd_worker_stacks[USB_MSD_MAX_HOSTS][16384]
	__attribute__((aligned(16)));

/* ST_* of a BOT/SCSI call -> the disk layer's -errno.  A device that is gone
 * gives an I/O error, as a disk does that stopped answering. */
static int msd_st_to_errno(usb_msd_host_t *h, int st)
{
	if (st != ST_OK && (st == ST_NO_DEVICE || (h && h->gone)))
		return -EIO;
	switch (st) {
	case ST_OK:
		return 0;
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
static struct blk_request *msd_rq_pop_locked(usb_msd_host_t *h)
{
	struct blk_request *req = NULL;
	if (h->rq_count) {
		req = h->rq_ring[h->rq_head];
		h->rq_ring[h->rq_head] = NULL;
		h->rq_head = (h->rq_head + 1) % USB_MSD_RING_SIZE;
		h->rq_count--;
	}
	return req;
}

static struct blk_request *msd_rq_pop(usb_msd_host_t *h)
{
	struct blk_request *req;
	uint64_t flags;
	spin_lock_irqsave(&h->rq_lock, &flags);
	req = msd_rq_pop_locked(h);
	spin_unlock_irqrestore(&h->rq_lock, flags);
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
 * disk, data in the request's sg list.  Each piece is one READ/WRITE
 * of at most USB_MSD_BLOCK_CHUNK sectors, issued under the I/O mutex.  An
 * element holding at least one whole sector is transferred in place (the
 * BOT layer copies through its DMA bounce buffer anyway); a sector split
 * across elements goes through the staging buffer. */
static int msd_exec_rw(usb_msd_device_t *msd, struct blk_request *req,
		       uint32_t *resid)
{
	usb_msd_host_t *h = msd->host;
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
			       "usb_msd: request beyond the end: lba %llu + %u > %llu\n",
			       (unsigned long long)lba, left,
			       (unsigned long long)msd->block_count);
		return -EINVAL;
	}
	VM_BUG_ON(!req->sg && req->nsg);

	while (left) {
		uint8_t *ptr;
		uint32_t n;
		int staged = 0;

		if (msd_host_dead(h))
			return -EIO;
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

		msd_io_lock(h);
		int st = write ? usb_msd_write(msd, lba, n, ptr) :
				 usb_msd_read(msd, lba, n, ptr);
		msd_io_unlock(h);
		if (st != ST_OK) {
			/* An unplugged device fails all its I/O: no
			 * warning for each request of it. */
			if (!h->gone)
				WARN_RATELIMIT(1,
					       "usb_msd: %s of %u sectors at %llu failed (st %d)\n",
					       write ? "write" : "read", n,
					       (unsigned long long)lba, st);
			return msd_st_to_errno(h, st);
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
	usb_msd_host_t *h = msd->host;
	usb_msd_cbw_t cbw;
	usb_msd_csw_t csw;

	msd_memset(&csw, 0, sizeof(csw));
	pt->status = SAM_STAT_GOOD;
	pt->host_status = SG_ERR_DID_OK;
	pt->driver_status = SG_ERR_DRIVER_OK;
	pt->sense_len = 0;
	pt->resid = pt->len;

	msd_io_lock(h);
	msd_cbw_init(msd, &cbw, pt->buf ? pt->len : 0,
		     pt->data_in ? CBW_FLAG_DATA_IN : CBW_FLAG_DATA_OUT,
		     pt->cdb_len);
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
	} else if (st == ST_NO_DEVICE) {
		pt->host_status = SG_ERR_DID_NO_CONNECT;
		pt->driver_status = SG_ERR_DRIVER_ERROR;
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
	msd_io_unlock(h);
	h->nr_passthrough++;
}

/* ---- Request execution -------------------------------------------------- */

static void msd_execute(usb_msd_host_t *h, struct blk_request *req)
{
	usb_msd_device_t *msd = (usb_msd_device_t *)req->driver_data;
	uint32_t resid = 0;
	int status;

	BUG_ON(req->magic != BLK_REQUEST_MAGIC);
	h->rq_active = req;
	if (!msd || msd->host != h) {
		status = -ENODEV;
	} else if (msd_host_dead(h)) {
		/* The device is gone: an I/O error, at once. */
		status = -EIO;
		if (req->op == BLK_OP_READ || req->op == BLK_OP_WRITE)
			resid = req->count * msd->block_size;
	} else {
		switch (req->op) {
		case BLK_OP_READ:
		case BLK_OP_WRITE:
			status = msd_exec_rw(msd, req, &resid);
			break;
		case BLK_OP_FLUSH:
			msd_io_lock(h);
			status = msd_st_to_errno(h, usb_msd_sync(msd));
			msd_io_unlock(h);
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
	h->rq_active = NULL;
	h->nr_requests++;
	if (status)
		h->nr_errors++;
	/* The worker is a task: the request ends right here; the next
	 * dispatch comes from the submitter or kblockd, never from here.
	 * Nothing of the logical unit is touched after this: completing the
	 * last request may let its disk be released. */
	blk_complete_request_direct(req, status, resid);
}

/* ---- Medium checks of card-reader slots ---------------------------------- */

/* Is a medium in the unit?  1 yes, 0 no, -1 cannot tell (a unit attention
 * was reported and cleared, or the transport failed).  `changed' is set when
 * the unit reports that the medium may have changed.  I/O mutex held. */
static int msd_medium_state(usb_msd_device_t *msd, int *changed)
{
	scsi_sense_data_t sense;
	int st = usb_msd_test_unit_ready(msd);

	if (st == ST_OK)
		return 1;
	if (st == ST_NO_DEVICE || msd_host_dead(msd->host))
		return -1;
	if (usb_msd_request_sense(msd, &sense) != ST_OK)
		return -1;
	uint8_t key = sense.sense_key & 0x0F;
	if (key == 0x06) { /* UNIT ATTENTION */
		if (sense.asc == 0x28) /* not ready to ready: medium changed */
			*changed = 1;
		return -1;
	}
	if (key == 0x02 && sense.asc == 0x3A) /* MEDIUM NOT PRESENT */
		return 0;
	if (key == 0x02) /* NOT READY otherwise: becoming ready */
		return -1;
	return 1; /* some other condition: the medium is there */
}

/* The idle worker checks the units that want it: a change seen on two
 * checks in a row (or a reported medium change) goes to the attach thread,
 * which tells the disk layer (disk_media_changed). */
static void msd_poll_media(usb_msd_host_t *h)
{
	int kick = 0;

	for (int l = 0; l < USB_MSD_MAX_LUNS; l++) {
		usb_msd_device_t *msd;
		int changed = 0, present;
		uint64_t flags;

		/* Pinned while it is looked at: a unit released meanwhile
		 * (its device unplugged, its disk freed) is not given to
		 * another device until the check is over. */
		spin_lock_irqsave(&g_msd_lock, &flags);
		msd = h->luns[l];
		if (!msd || msd->state != USB_MSD_LUN_BOUND ||
		    msd->host != h || !msd->poll_media || !msd->disk ||
		    msd->media_event || h->state != USB_MSD_HOST_LIVE) {
			spin_unlock_irqrestore(&g_msd_lock, flags);
			continue;
		}
		uint32_t gen = h->usb_gen;
		msd->pinned = 1;
		spin_unlock_irqrestore(&g_msd_lock, flags);

		/* Still this host's unit, of the same device, once the
		 * transport is ours (the slot may have been detached and
		 * given to another device meanwhile). */
		msd_io_lock(h);
		if (msd->state == USB_MSD_LUN_BOUND && msd->host == h &&
		    h->usb_gen == gen && h->state == USB_MSD_HOST_LIVE &&
		    !msd_host_dead(h))
			present = msd_medium_state(msd, &changed);
		else
			present = -2;
		msd_io_unlock(h);

		spin_lock_irqsave(&g_msd_lock, &flags);
		if (present != -2 && (msd->state != USB_MSD_LUN_BOUND ||
				      msd->host != h || h->usb_gen != gen ||
				      h->state != USB_MSD_HOST_LIVE))
			present = -2;
		if (present == -2 || (present < 0 && !changed)) {
			msd->pinned = 0;
			spin_unlock_irqrestore(&g_msd_lock, flags);
			if (msd_host_dead(h))
				return;
			continue;
		}
		if (changed && present < 0)
			present = msd->medium; /* re-read it, whatever it is */
		if (!changed && present == msd->medium) {
			msd->media_miss = 0;
		} else if (!changed && ++msd->media_miss < 2) {
			/* seen once: wait for the next check */
		} else {
			msd->media_miss = 0;
			msd->medium = (uint8_t)present;
			msd->media_event = 1;
			kick = 1;
		}
		msd->pinned = 0;
		spin_unlock_irqrestore(&g_msd_lock, flags);
	}
	if (kick)
		msd_kick_attach_thread();
}

/* The worker: runs queued requests; sleeps on rq_wait when there are none.
 * The idle check and the switch to BLOCKED happen under rq_lock, and submit
 * queues under the same lock before it wakes the channel, so a request
 * queued at any moment either is seen here or finds the worker blocked on
 * the channel -- no wake-up is lost.  A nap deadline backs that up anyway
 * (and paces the medium checks).  The worker belongs to the host slot: it
 * serves whichever device the slot is attached to, and naps while it is
 * free. */
#define MSD_WORKER_NAP_MS 1000

static void msd_worker(void *arg)
{
	usb_msd_host_t *h = (usb_msd_host_t *)arg;
	task_t *cur = sched_current();

	BUG_ON(h == NULL || cur == NULL);
	h->worker_running = 1;
	for (;;) {
		struct blk_request *req = msd_rq_pop(h);
		if (req) {
			msd_execute(h, req);
			continue;
		}
		if (h->has_media_poll && h->state == USB_MSD_HOST_LIVE &&
		    !h->gone && timer_ticks() >= h->next_media_poll) {
			h->next_media_poll =
				timer_ticks() +
				timer_ms_to_ticks(MSD_MEDIA_POLL_MS);
			msd_poll_media(h);
			continue;
		}
		uint64_t flags;
		spin_lock_irqsave(&h->rq_lock, &flags);
		if (h->rq_count) {
			spin_unlock_irqrestore(&h->rq_lock, flags);
			continue;
		}
		cur->wait_channel = (void *)&h->rq_wait;
		cur->wakeup_tick =
			timer_ticks() + timer_ms_to_ticks(MSD_WORKER_NAP_MS) + 1;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&h->rq_lock, flags);
		sched_schedule();
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		if (cur->state != TASK_RUNNING)
			cur->state = TASK_RUNNING;
	}
}

/* Run queued requests in the calling context -- only when there is no
 * worker thread (it could not be created) and the caller may block.  One
 * runner at a time per host: a completion dispatches the next request
 * straight back into submit, which must then only queue it (the running
 * loop picks it up) instead of recursing once per request. */
static int msd_run_inline(usb_msd_host_t *h)
{
	struct blk_request *req;
	uint64_t flags;
	int done = 0;

	/* With a worker thread (running or about to: it empties the ring
	 * before it first sleeps) requests never run in the caller. */
	if (h->worker || !irqs_enabled() || !sched_current())
		return 0;
	spin_lock_irqsave(&h->rq_lock, &flags);
	if (h->inline_running) {
		spin_unlock_irqrestore(&h->rq_lock, flags);
		return 0;
	}
	h->inline_running = 1;
	for (;;) {
		req = msd_rq_pop_locked(h);
		if (!req) {
			/* cleared under the lock that queues: nothing queued
			 * after this check is left without a runner */
			h->inline_running = 0;
			spin_unlock_irqrestore(&h->rq_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&h->rq_lock, flags);
		msd_execute(h, req);
		done++;
		spin_lock_irqsave(&h->rq_lock, &flags);
	}
	return done;
}

static int usb_msd_blk_submit(struct gendisk *disk, struct blk_request *req)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	usb_msd_host_t *h;
	uint64_t flags;

	if (unlikely(!msd || !msd->host))
		return -ENODEV;
	h = msd->host;
	if (unlikely(msd_host_dead(h)))
		return -EIO; /* the device is gone */
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
	spin_lock_irqsave(&h->rq_lock, &flags);
	if (h->rq_count >= USB_MSD_RING_SIZE) {
		spin_unlock_irqrestore(&h->rq_lock, flags);
		return -EBUSY; /* the queue retries after a completion */
	}
	h->rq_ring[(h->rq_head + h->rq_count) % USB_MSD_RING_SIZE] = req;
	h->rq_count++;
	req->driver_data = msd;
	spin_unlock_irqrestore(&h->rq_lock, flags);

	if (likely(h->worker))
		sched_wake_channel_once((void *)&h->rq_wait, 1);
	else
		msd_run_inline(h);
	return 0;
}

/* Polled completion (blk_wait without the scheduler): this driver needs its
 * worker, or a context that may block; with interrupts off nothing can be
 * done -- BOT transfers wait for xHCI interrupts. */
static int usb_msd_blk_poll(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	if (!msd || !msd->host)
		return 0;
	return msd_run_inline(msd->host);
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
	if (!msd->host || msd_host_dead(msd->host))
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
	if (!msd->host || msd_host_dead(msd->host))
		return -ENODEV;
	msd_io_lock(msd->host);
	int st = usb_msd_sync(msd);
	msd_io_unlock(msd->host);
	return msd_st_to_errno(msd->host, st);
}

static int usb_msd_blk_shutdown(struct gendisk *disk,
				enum blk_shutdown_mode mode)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	(void)mode;
	if (!msd)
		return -ENODEV;
	/* A device that is gone has no cache left to flush. */
	if (!msd->host || msd_host_dead(msd->host))
		return 0;
	int rc = msd_flush_direct(msd);
	if (rc)
		kprintf(MSD_LOG "%s: cache flush at shutdown failed (%d)\n",
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
	if (!msd || !msd->host)
		return -ENODEV;
	msd_io_lock(msd->host);
	for (int i = 0; i < 3 && st != ST_OK; i++) {
		st = usb_msd_test_unit_ready(msd);
		if (st == ST_NO_DEVICE)
			break;
		if (st != ST_OK) {
			scsi_sense_data_t sense;
			usb_msd_request_sense(msd, &sense);
		}
	}
	msd_io_unlock(msd->host);
	return msd_st_to_errno(msd->host, st);
}

static void msd_host_buffers(usb_msd_host_t *h, uint32_t block_size);

/* Revalidate: read the capacity again (a card reader's medium changed).  A
 * medium with another block size than the unit had (a unit registered
 * without a medium keeps 512 bytes, or 2048 for an optical drive) changes
 * the disk's logical block size: the old medium's partitions are gone by
 * then (msd_attach_thread_step reports a swapped medium as removed first),
 * so the disk layer takes the new size. */
static int usb_msd_blk_revalidate(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	uint64_t count = 0;
	uint32_t size = 0;
	int st = ST_ERR;
	might_sleep();
	if (!msd || !msd->host)
		return -ENODEV;
	msd_io_lock(msd->host);
	for (int i = 0; i < 3 && st != ST_OK; i++) {
		st = usb_msd_read_capacity(msd, &count, &size);
		if (st == ST_NO_DEVICE)
			break;
		if (st != ST_OK) {
			scsi_sense_data_t sense;
			usb_msd_request_sense(msd, &sense);
		}
	}
	msd_io_unlock(msd->host);
	if (st != ST_OK)
		return msd_st_to_errno(msd->host, st);
	if (count && (size == 0 || size > 4096))
		size = 512; /* as at the attach */
	if (count && size != msd->block_size) {
		struct blk_limits lim;
		int rc;

		if (!disk->queue)
			return -EIO;
		queue_limits_start_update(disk->queue, &lim);
		lim.logical_block_size = size;
		lim.physical_block_size = size;
		lim.io_min = size;
		lim.max_segment_size = USB_MSD_MAX_BLOCKS * size;
		rc = queue_limits_commit_update(disk->queue, &lim);
		if (rc) {
			kprintf(MSD_LOG "%s: block size change %u -> %u "
				"refused (%d)\n",
				disk->disk_name, msd->block_size, size, rc);
			return -EIO;
		}
		/* The transport's side: chunks of the new size fit the
		 * bounce buffer, and commands use it -- under the I/O mutex,
		 * between two commands. */
		msd_io_lock(msd->host);
		msd_host_buffers(msd->host, size);
		msd->block_size = size;
		msd_io_unlock(msd->host);
		kprintf(MSD_LOG "%s: medium has %u-byte blocks\n",
			disk->disk_name, size);
	}
	if (count != msd->block_count) {
		msd->block_count = count;
		return disk_set_capacity(disk, count);
	}
	return 0;
}

static void msd_lun_release(usb_msd_device_t *msd);

/* The disk layer's last hold on the disk is gone (after disk_del):
 * the logical unit -- and once all of them are released, the host slot --
 * may be reused. */
static void usb_msd_blk_free_disk(struct gendisk *disk)
{
	usb_msd_device_t *msd = msd_from_disk(disk);
	if (msd && msd->disk == disk)
		msd_lun_release(msd);
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

/* "30.8 GB" / "512 MB": the decimal size a stick is sold as. */
static void msd_format_size(uint64_t bytes, char *out, size_t cap)
{
	static const char *units[] = { "kB", "MB", "GB", "TB", "PB" };
	uint64_t div = 1000;
	int u = 0;

	if (bytes < 1000) {
		ksnprintf(out, cap, "%llu B", (unsigned long long)bytes);
		return;
	}
	while (u < 4 && bytes / div >= 1000) {
		div *= 1000;
		u++;
	}
	uint64_t whole = bytes / div;
	uint64_t tenth = (bytes % div) * 10 / div;
	ksnprintf(out, cap, "%llu.%llu %s", (unsigned long long)whole,
		  (unsigned long long)tenth, units[u]);
}

/* Register a logical unit as a disk of the disk layer: sdX (srN for an
 * optical drive), removable, its partitions scanned by disk_add().  0 or
 * -errno. */
static int msd_register_disk(usb_msd_device_t *msd)
{
	usb_msd_host_t *h = msd->host;
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

	disk->capacity = msd->medium ? msd->block_count : 0;
	disk->flags |= GENHD_FL_REMOVABLE;
	if (msd->medium)
		disk->flags |= GENHD_FL_MEDIA_PRESENT;
	if (optical)
		disk->flags |= GENHD_FL_NO_PART_SCAN | GENHD_FL_RO;
	/* The SCSI identity; a device that left it blank is named by its
	 * USB strings. */
	msd_strlcpy(disk->vendor, sizeof(disk->vendor),
		    msd->vendor[0] ? msd->vendor : h->manufacturer);
	msd_strlcpy(disk->model, sizeof(disk->model),
		    msd->product[0] ? msd->product : h->product);
	msd_strlcpy(disk->firmware, sizeof(disk->firmware), msd->revision);
	msd_strlcpy(disk->serial, sizeof(disk->serial), h->serial);
	disk->wwn[0] = 0;
	msd_strlcpy(disk->bus, sizeof(disk->bus), "usb");
	disk->pci = NULL;
	disk->host_no = USB_MSD_SCSI_HOST_BASE + h->index;
	disk->channel = 0;
	disk->target = 0;
	disk->lun = msd->lun;
	disk->scsi_type = optical ? TYPE_ROM : TYPE_DISK;
	disk->bdev.ready = msd->medium ? 1 : 0;

	msd->disk = disk;
	msd->ready = 1;
	rc = disk_add(disk);
	if (rc) {
		msd->ready = 0;
		msd->disk = NULL;
		/* never added: the put frees it (free_disk sees msd->disk
		 * cleared and leaves the unit alone) */
		disk_put(disk);
		return rc;
	}
	/* The device answers I/O; disk_add filled the block device. */
	disk->bdev.removable = 1;
	if (!disk->bdev.release_locks_for_task)
		disk->bdev.release_locks_for_task = usb_msd_block_release_locks;
	__asm__ volatile("" ::: "memory");
	disk->bdev.ready = msd->medium ? 1 : 0;

	char size[24], lun[16];
	msd_format_size(msd->block_count * (uint64_t)msd->block_size, size,
			sizeof(size));
	lun[0] = 0;
	if (h->max_lun)
		ksnprintf(lun, sizeof(lun), " (LUN %u)", msd->lun);
	if (msd->medium)
		kprintf(MSD_LOG "%s: %s %s %s%s%s\n", disk->disk_name,
			disk->vendor, disk->model, size, lun,
			optical ? ", optical" : "");
	else
		kprintf(MSD_LOG "%s: %s %s, no medium%s\n", disk->disk_name,
			disk->vendor, disk->model, lun);
	return 0;
}

//=============================================================================
// Slots
//=============================================================================

/* Initialise a host slot the first time it is used: its locks, its
 * worker thread, its DMA staging pages.  The attach thread / boot poll
 * that claimed the slot is the only one touching it then. */
static void msd_host_setup_slot(usb_msd_host_t *h)
{
	if (h->slot_ready)
		return;
	h->io_locked = 0;
	h->io_owner = (uint64_t)-1;
	spinlock_init(&h->io_wait_lock, "msd_io_wait");
	spinlock_init(&h->rq_lock, "msd_rq");
	h->rq_head = 0;
	h->rq_count = 0;

	// Allocate the persistent, page-aligned CBW/CSW/control staging
	// buffers once.  Reused for every command so the data path never
	// touches the heap.  On failure, the pointers stay NULL and
	// usb_msd_bot_transfer falls back to per-command allocation.
	h->cbw_buf_raw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
	h->csw_buf_raw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
	h->ctl_buf_raw = (uint8_t *)kcalloc_dma(1, 4096 + 4096);
	if (h->cbw_buf_raw && h->csw_buf_raw) {
		h->cbw_buf = (uint8_t *)(((uint64_t)h->cbw_buf_raw + 4095) &
					 ~4095ULL);
		h->csw_buf = (uint8_t *)(((uint64_t)h->csw_buf_raw + 4095) &
					 ~4095ULL);
	} else {
		if (h->cbw_buf_raw)
			kfree_dma(h->cbw_buf_raw);
		if (h->csw_buf_raw)
			kfree_dma(h->csw_buf_raw);
		h->cbw_buf_raw = h->csw_buf_raw = NULL;
		h->cbw_buf = h->csw_buf = NULL;
	}
	if (h->ctl_buf_raw)
		h->ctl_buf = (uint8_t *)(((uint64_t)h->ctl_buf_raw + 4095) &
					 ~4095ULL);

	task_t *t = sched_add_task(msd_worker, h, g_msd_worker_stacks[h->index],
				   sizeof(g_msd_worker_stacks[h->index]));
	if (!t) {
		kprintf(MSD_LOG "could not start the worker thread; I/O "
			"runs in the caller's context\n");
	} else {
		ksnprintf(t->comm, sizeof(t->comm), "kusbmsd%d", h->index);
		h->worker = t;
	}
	h->slot_ready = 1;
}

/* The persistent contiguous data bounce buffer, sized to the largest chunk
 * the block layer issues (USB_MSD_BLOCK_CHUNK sectors of the largest block
 * size of the host's units).  Allocated here, at attach, so the read/write
 * fast path never calls the contiguous-page allocator.  A smaller buffer of
 * an earlier device of the slot is replaced (that device's slot is long
 * disabled).  On failure, data_buf stays NULL and bot_transfer falls back
 * to per-command allocation. */
static void msd_host_buffers(usb_msd_host_t *h, uint32_t block_size)
{
	uint32_t max_xfer = (uint32_t)USB_MSD_BLOCK_CHUNK * block_size;
	size_t pages = ((size_t)max_xfer + 4095) / 4096;

	if (h->data_buf && h->data_buf_size >= max_xfer)
		return;
	/* The new buffer first: when it cannot be had, the old one stays
	 * (transfers it cannot hold take the per-command allocation). */
	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys)
		return;
	if (h->data_buf)
		mm_free_contiguous_pages(h->data_buf_phys, h->data_buf_pages);
	h->data_buf_phys = phys;
	h->data_buf = (uint8_t *)phys_to_virt(phys);
	h->data_buf_size = (uint32_t)(pages * 4096);
	h->data_buf_pages = (uint32_t)pages;
}

/* Drop a hold on a host; the last one frees the slot. */
static void msd_host_put(usb_msd_host_t *h)
{
	uint64_t flags;

	spin_lock_irqsave(&g_msd_lock, &flags);
	WARN_ON(h->refs <= 0);
	if (--h->refs <= 0) {
		h->refs = 0;
		WARN_ON(h->state != USB_MSD_HOST_GONE);
		WARN_ON(h->rq_count != 0);
		h->state = USB_MSD_HOST_FREE;
		h->usb_dev = NULL;
		h->ctrl = NULL;
		h->detach_pending = 0;
		h->has_media_poll = 0;
	}
	spin_unlock_irqrestore(&g_msd_lock, flags);
}

/* A free logical-unit slot for unit `lun' of `h', bound to it (and holding
 * a hold on the host); NULL when all are in use. */
static usb_msd_device_t *msd_lun_alloc(usb_msd_host_t *h, uint8_t lun)
{
	usb_msd_device_t *msd = NULL;
	uint64_t flags;

	spin_lock_irqsave(&g_msd_lock, &flags);
	for (int i = 0; i < USB_MSD_MAX_DEVICES; i++) {
		if (g_msd_devices[i].state == USB_MSD_LUN_FREE &&
		    !g_msd_devices[i].pinned) {
			msd = &g_msd_devices[i];
			break;
		}
	}
	if (msd) {
		uint8_t *stage = msd->stage_buf;
		uint32_t stage_size = msd->stage_size;

		msd_memset(msd, 0, sizeof(*msd));
		msd->index = (int)(msd - g_msd_devices);
		msd->stage_buf = stage; /* kept from an earlier unit */
		msd->stage_size = stage_size;
		msd->state = USB_MSD_LUN_BOUND;
		msd->host = h;
		msd->lun = lun;
		msd->block_size = 512;
		h->luns[lun] = msd;
		h->refs++;
	}
	spin_unlock_irqrestore(&g_msd_lock, flags);
	return msd;
}

/* Unbind a logical unit from its host (its disk is released, or it never
 * got one) and drop its hold on the host.  A unit registered the legacy way
 * is retired, not reused: that registry takes no holds. */
static void msd_lun_release(usb_msd_device_t *msd)
{
	usb_msd_host_t *h;
	uint64_t flags;

	spin_lock_irqsave(&g_msd_lock, &flags);
	if (msd->state != USB_MSD_LUN_BOUND) {
		spin_unlock_irqrestore(&g_msd_lock, flags);
		return;
	}
	h = msd->host;
	if (h && msd->lun < USB_MSD_MAX_LUNS && h->luns[msd->lun] == msd)
		h->luns[msd->lun] = NULL;
	msd->disk = NULL;
	msd->ready = 0;
	msd->media_event = 0;
	/* (The staging buffer stays with the slot for its next unit; the
	 * host pointer as well, for a worker still finishing up.) */
	if (msd->legacy_registered) {
		/* The legacy registry takes no holds: a user of `blk' may
		 * still call in, and its calls must keep finding this dead
		 * host, never a later device in the slot.  Unit and host slot
		 * stay retired. */
		msd->state = USB_MSD_LUN_RETIRED;
		spin_unlock_irqrestore(&g_msd_lock, flags);
		return;
	}
	msd->state = USB_MSD_LUN_FREE;
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (h)
		msd_host_put(h);
}

//=============================================================================
// Attach
//=============================================================================

/* A string descriptor as printable ASCII ("" when there is none).  I/O
 * mutex held. */
static void msd_read_string(usb_msd_host_t *h, uint8_t idx, uint16_t langid,
			    char *out, size_t cap)
{
	out[0] = 0;
	if (!idx || !h->ctl_buf || cap < 2)
		return;
	msd_memset(h->ctl_buf, 0, 256);
	if (msd_control(h, USB_RT_D2H | USB_RT_STD | USB_RT_DEV,
			USB_REQ_GET_DESCRIPTOR, (USB_DESC_STRING << 8) | idx,
			langid, 255, h->ctl_buf) != ST_OK)
		return;
	uint8_t len = h->ctl_buf[0];
	if (len < 2 || h->ctl_buf[1] != USB_DESC_STRING)
		return;
	size_t o = 0;
	for (uint32_t i = 2; i + 1 < len && o + 1 < cap; i += 2) {
		uint16_t c = (uint16_t)(h->ctl_buf[i] | (h->ctl_buf[i + 1] << 8));
		if (c == 0)
			break;
		out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
	}
	out[o] = 0;
	/* blanks at both ends are padding */
	msd_trim(out);
	size_t lead = 0;
	while (out[lead] == ' ')
		lead++;
	if (lead) {
		size_t k = 0;
		while (out[lead + k]) {
			out[k] = out[lead + k];
			k++;
		}
		out[k] = 0;
	}
}

/* The device's USB identity strings and its number of logical units.  I/O
 * mutex held. */
static void msd_host_identify(usb_msd_host_t *h)
{
	uint16_t langid = 0x0409;
	usb_device_t *dev = h->usb_dev;

	if (msd_host_dead(h) || !dev)
		return;
	if (h->ctl_buf) {
		msd_memset(h->ctl_buf, 0, 256);
		if (msd_control(h, USB_RT_D2H | USB_RT_STD | USB_RT_DEV,
				USB_REQ_GET_DESCRIPTOR, (USB_DESC_STRING << 8),
				0, 255, h->ctl_buf) == ST_OK &&
		    h->ctl_buf[0] >= 4 && h->ctl_buf[1] == USB_DESC_STRING)
			langid = (uint16_t)(h->ctl_buf[2] |
					    (h->ctl_buf[3] << 8));
		msd_read_string(h, dev->str_manufacturer, langid,
				h->manufacturer, sizeof(h->manufacturer));
		msd_read_string(h, dev->str_product, langid, h->product,
				sizeof(h->product));
		msd_read_string(h, dev->str_serial, langid, h->serial,
				sizeof(h->serial));
	}

	/* GET MAX LUN; a device with a single unit may stall it. */
	h->max_lun = 0;
	if (h->ctl_buf && !msd_host_dead(h)) {
		h->ctl_buf[0] = 0;
		if (msd_control(h, USB_RT_D2H | USB_RT_CLASS | USB_RT_IFACE,
				USB_MSD_REQ_GET_MAX_LUN, 0, h->iface, 1,
				h->ctl_buf) == ST_OK) {
			h->max_lun = h->ctl_buf[0] & 0x0F;
			if (h->max_lun >= USB_MSD_MAX_LUNS)
				h->max_lun = USB_MSD_MAX_LUNS - 1;
		}
	}
}

/* Probe one logical unit: INQUIRY, wait until it is ready (at most
 * `ready_ms', see `defer'), READ CAPACITY.
 *   ST_OK          the unit exists (with or without a medium);
 *   ST_NOT_FOUND   no unit at this number;
 *   ST_NO_DEVICE   the device went away;
 *   ST_AGAIN       `defer' is set and the unit was still becoming ready
 *                  when `ready_ms' ran out (the caller lets the attach
 *                  thread wait for it);
 *   other          the unit does not answer. */
static int msd_lun_probe(usb_msd_device_t *msd, uint32_t ready_ms, int defer)
{
	usb_msd_host_t *h = msd->host;
	scsi_inquiry_data_t inquiry;
	scsi_sense_data_t sense;
	int st = ST_ERR;
	int inquiry_st;

	// Inquiry (a unit that just powered up may answer only the second)
	for (int i = 0; i < 3 && st != ST_OK; i++) {
		msd_io_lock(h);
		st = usb_msd_inquiry(msd, &inquiry);
		if (st != ST_OK && st != ST_NO_DEVICE)
			(void)usb_msd_request_sense(msd, &sense);
		msd_io_unlock(h);
		if (st == ST_NO_DEVICE || msd_host_dead(h))
			return ST_NO_DEVICE;
		if (st != ST_OK)
			msd_msleep(MSD_READY_RETRY_MS);
	}
	inquiry_st = st;
	if (st != ST_OK) {
		msd_dbg("Inquiry failed: st=%d\n", st);
		/* Logical unit 0 goes on without the identity, as the driver
		 * always did: a device that answers READ CAPACITY is used.
		 * Further units must answer. */
		if (msd->lun)
			return st;
		msd_memset(&inquiry, 0, sizeof(inquiry));
	}
	/* Peripheral qualifier 3: no unit here; 1: not connected now (for
	 * the units past the first, which is always taken) */
	uint8_t qual = (inquiry.device_type >> 5) & 0x7;
	if (msd->lun && (qual == 3 || qual == 1 ||
			 (inquiry.device_type & 0x1f) == 0x1f))
		return ST_NOT_FOUND;

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
	/* The block size a unit registered without a medium keeps (the disk
	 * layer cannot change it later): an optical drive's is 2048. */
	if (msd->device_type == TYPE_ROM || msd->device_type == TYPE_WORM)
		msd->block_size = 2048;

	// Wait for the unit to be ready: a stick needs a moment after it
	// was plugged in.  NOT READY / UNIT ATTENTION are waited out (up to
	// MSD_READY_TIMEOUT_MS); MEDIUM NOT PRESENT ends the wait at once.
	uint64_t deadline = hrtimer_now_ns() + (uint64_t)ready_ms * 1000000ULL;
	int ready = 0, no_medium = 0, attentions = 0;
	for (;;) {
		msd_io_lock(h);
		st = usb_msd_test_unit_ready(msd);
		int key = -1, asc = -1;
		if (st != ST_OK && st != ST_NO_DEVICE &&
		    usb_msd_request_sense(msd, &sense) == ST_OK) {
			key = sense.sense_key & 0x0F;
			asc = sense.asc;
		}
		msd_io_unlock(h);
		if (st == ST_NO_DEVICE || msd_host_dead(h))
			return ST_NO_DEVICE;
		if (st == ST_OK) {
			ready = 1;
			break;
		}
		if (key == 0x02 && asc == 0x3A) {
			no_medium = 1;
			break;
		}
		if (hrtimer_now_ns() >= deadline) {
			/* Still becoming ready (or not answering the
			 * question at all): the attach thread waits on. */
			if (defer)
				return ST_AGAIN;
			break;
		}
		/* a unit attention (power on, medium changed) is reported
		 * once and cleared by the sense: ask again right away */
		if (key == 0x06 && ++attentions < 8)
			continue;
		msd_msleep(MSD_READY_RETRY_MS);
	}
	if (!ready && !no_medium)
		msd_dbg("LUN %u not ready, continuing anyway...\n", msd->lun);

	// Get capacity
	msd->medium = 0;
	if (!no_medium) {
		uint64_t count = 0;
		uint32_t size = 0;
		st = ST_ERR;
		for (int i = 0; i < 3 && st != ST_OK; i++) {
			msd_io_lock(h);
			st = usb_msd_read_capacity(msd, &count, &size);
			if (st != ST_OK && st != ST_NO_DEVICE)
				(void)usb_msd_request_sense(msd, &sense);
			msd_io_unlock(h);
			if (st == ST_NO_DEVICE || msd_host_dead(h))
				return ST_NO_DEVICE;
		}
		if (st == ST_OK && count) {
			// Validate block size
			if (size == 0 || size > 4096) {
				msd_dbg("Invalid block size %u, defaulting to 512\n",
					size);
				size = 512;
			}
			msd->block_count = count;
			msd->block_size = size;
			msd->medium = 1;
		} else {
			msd_dbg("Read capacity failed: st=%d\n", st);
		}
	}
	/* Neither an identity nor a capacity: not a SCSI unit that works. */
	if (inquiry_st != ST_OK && !msd->medium && !no_medium)
		return inquiry_st;
	return ST_OK;
}

/* Claim a free host slot for the device: ST_OK with *out set; ST_EXISTS when
 * the device is attached (or being attached) already; ST_NO_DEVICE when the
 * device is gone or no longer the one meant; ST_BUSY when every slot is
 * taken. */
static int msd_host_claim(xhci_controller_t *ctrl, usb_device_t *dev,
			  uint32_t gen, usb_msd_host_t **out)
{
	usb_msd_host_t *h = NULL;
	uint64_t flags;
	int st = ST_OK;

	*out = NULL;
	spin_lock_irqsave(&g_msd_lock, &flags);
	if (dev->gone || dev->gen != gen || !dev->configured ||
	    !dev->msd_iface_valid) {
		st = ST_NO_DEVICE;
		goto out;
	}
	for (int i = 0; i < USB_MSD_MAX_HOSTS; i++) {
		usb_msd_host_t *o = &g_msd_hosts[i];
		if (o->state != USB_MSD_HOST_FREE && !o->gone &&
		    o->usb_dev == dev && o->usb_gen == gen) {
			st = ST_EXISTS;
			goto out;
		}
	}
	for (int i = 0; i < USB_MSD_MAX_HOSTS; i++) {
		if (g_msd_hosts[i].state == USB_MSD_HOST_FREE) {
			h = &g_msd_hosts[i];
			break;
		}
	}
	if (!h) {
		st = ST_BUSY;
		goto out;
	}
	h->index = (int)(h - g_msd_hosts);
	h->state = USB_MSD_HOST_ATTACHING;
	h->gone = 0;
	h->failed = 0;
	h->detach_pending = 0;
	h->refs = 1; /* the attachment's */
	h->usb_dev = dev;
	h->ctrl = ctrl;
	h->usb_gen = gen;
	h->iface = dev->msd_iface;
	h->bulk_in_addr = (uint8_t)(USB_EP_DIR_IN | dev->bulk_in_ep);
	h->bulk_out_addr = dev->bulk_out_ep;
	h->max_lun = 0;
	h->port = dev->port;
	h->vendor_id = dev->vendor_id;
	h->product_id = dev->product_id;
	h->manufacturer[0] = h->product[0] = h->serial[0] = 0;
	h->next_tag = 0x12340000;
	for (int l = 0; l < USB_MSD_MAX_LUNS; l++)
		h->luns[l] = NULL;
	h->next_media_poll = 0;
	h->has_media_poll = 0;
	h->nr_requests = h->nr_errors = h->nr_passthrough = h->nr_resets = 0;
	*out = h;
out:
	spin_unlock_irqrestore(&g_msd_lock, flags);
	return st;
}

static void msd_host_detach(usb_msd_host_t *h);

/* Attach a configured mass-storage device: probe its units and register
 * each as a disk.  ST_OK attached, ST_EXISTS attached already, ST_AGAIN
 * (`boot' only) a unit was not ready within MSD_BOOT_READY_MS and nothing
 * was attached, otherwise the error (the device stays unattached).  May
 * sleep. */
static int msd_attach(xhci_controller_t *ctrl, usb_device_t *dev, uint32_t gen,
		      int boot)
{
	usb_msd_host_t *h;
	usb_msd_device_t *units[USB_MSD_MAX_LUNS];
	int nunits = 0, st, need_detach = 0;
	uint32_t max_bs = 512;
	uint64_t flags;

	might_sleep();
	st = msd_host_claim(ctrl, dev, gen, &h);
	if (st == ST_BUSY)
		kprintf(MSD_LOG "port %u: no free slot (%d USB storage devices "
			"attached already)\n",
			dev->port, USB_MSD_MAX_HOSTS);
	if (st != ST_OK)
		return st;
	msd_host_setup_slot(h);
	msd_host_buffers(h, 512);

	msd_io_lock(h);
	msd_host_identify(h);
	msd_io_unlock(h);

	for (int l = 0; l <= h->max_lun && !msd_host_dead(h); l++) {
		usb_msd_device_t *msd = msd_lun_alloc(h, (uint8_t)l);
		if (!msd) {
			kprintf(MSD_LOG "port %u: no free unit slot for LUN %d\n",
				h->port, l);
			break;
		}
		st = msd_lun_probe(msd,
				   boot ? MSD_BOOT_READY_MS : MSD_READY_TIMEOUT_MS,
				   boot);
		if (st != ST_OK) {
			msd_lun_release(msd);
			if (st == ST_NO_DEVICE || st == ST_AGAIN)
				break;
			if (st != ST_NOT_FOUND && l == 0)
				break; /* LUN 0 must answer */
			continue;
		}
		ksnprintf(msd->name, sizeof(msd->name), "usbmsd%d", msd->index);
		msd->poll_media = msd->removable && (h->max_lun || !msd->medium);
		if (msd->poll_media)
			h->has_media_poll = 1;
		if (msd->medium && msd->block_size > max_bs)
			max_bs = msd->block_size;
		units[nunits++] = msd;
	}
	if (!nunits || msd_host_dead(h) || st == ST_AGAIN) {
		int deferred = (st == ST_AGAIN);

		for (int i = 0; i < nunits; i++)
			msd_lun_release(units[i]);
		spin_lock_irqsave(&g_msd_lock, &flags);
		h->state = USB_MSD_HOST_GONE;
		spin_unlock_irqrestore(&g_msd_lock, flags);
		st = msd_host_dead(h) ? ST_NO_DEVICE :
		     deferred	      ? ST_AGAIN :
					ST_IO;
		if (st == ST_IO)
			kprintf(MSD_LOG "port %u: device %04x:%04x does not answer "
				"as a SCSI disk\n",
				h->port, h->vendor_id, h->product_id);
		msd_host_put(h);
		return st;
	}
	msd_host_buffers(h, max_bs);

	// Register with the disk layer (the worker serves the partition scan
	// in disk_add()).
	for (int i = 0; i < nunits; i++) {
		usb_msd_device_t *msd = units[i];
		int rc;

		if (msd_host_dead(h))
			break;
		// The old block device, complete: its callbacks are the
		// original synchronous path and serve the legacy registration.
		msd->blk.name = msd->name;
		msd->blk.sector_size = msd->block_size;
		msd->blk.total_sectors = (unsigned long)msd->block_count;
		msd->blk.read = usb_msd_block_read;
		msd->blk.write = usb_msd_block_write;
		msd->blk.sync = usb_msd_block_sync;
		msd->blk.release_locks_for_task = usb_msd_block_release_locks;
		msd->blk.driver_data = msd;
		rc = msd_register_disk(msd);
		if (rc) {
			// The disk layer refused (no memory, names exhausted):
			// keep the unit usable as a plain block device the old
			// way.  The root search (kernel/ke/storage.c) reads its
			// partition table itself.
			kprintf(MSD_LOG "disk registration failed (%d); "
				"registering %s as a legacy block device\n",
				rc, msd->name);
			if (msd->medium && block_register(&msd->blk) == ST_OK) {
				msd->legacy_registered = 1;
				msd->ready = 1;
			} else {
				kprintf(MSD_LOG "%s: not registered\n",
					msd->name);
			}
		}
	}

	spin_lock_irqsave(&g_msd_lock, &flags);
	if (h->gone) {
		h->state = USB_MSD_HOST_GONE;
		need_detach = 1;
	} else {
		h->state = USB_MSD_HOST_LIVE;
	}
	h->next_media_poll = timer_ticks() + timer_ms_to_ticks(MSD_MEDIA_POLL_MS);
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (need_detach) {
		/* Unplugged while it was being attached. */
		msd_host_detach(h);
		return ST_NO_DEVICE;
	}
	if (h->worker)
		sched_wake_channel_once((void *)&h->rq_wait, 1);
	return ST_OK;
}

//=============================================================================
// Detach
//=============================================================================

/* Take a detached host's disks down: they are marked gone (I/O fails with
 * EIO from now on, without reaching the driver), then deleted -- their
 * partitions, /dev nodes, /dev/disk links and /sys/block entries go, and
 * what waits in their queues fails.  Each logical unit is released when
 * the disk layer frees its disk (now, or when the last opener or mount lets
 * go), and the host slot with the last of them.  The transport is dead
 * already (h->gone).  May sleep; never on the host's own worker. */
static void msd_host_detach(usb_msd_host_t *h)
{
	usb_msd_device_t *units[USB_MSD_MAX_LUNS];
	struct gendisk *disks[USB_MSD_MAX_LUNS];
	uint64_t flags;
	int n = 0;

	might_sleep();
	BUG_ON(!h->gone);
	spin_lock_irqsave(&g_msd_lock, &flags);
	for (int l = 0; l < USB_MSD_MAX_LUNS; l++) {
		usb_msd_device_t *msd = h->luns[l];
		if (!msd || msd->state != USB_MSD_LUN_BOUND)
			continue;
		units[n] = msd;
		disks[n] = msd->disk;
		if (disks[n])
			disk_get(disks[n]); /* across the deletion below */
		n++;
	}
	spin_unlock_irqrestore(&g_msd_lock, flags);

	/* What the worker still holds fails at once now. */
	if (h->worker)
		sched_wake_channel_once((void *)&h->rq_wait, 1);

	for (int i = 0; i < n; i++)
		if (disks[i])
			blk_mark_disk_dead(disks[i]);
	for (int i = 0; i < n; i++) {
		usb_msd_device_t *msd = units[i];

		if (disks[i]) {
			kprintf(MSD_LOG "%s: device removed%s\n",
				disks[i]->disk_name,
				h->failed ? " (not answering)" : "");
			msd->ready = 0;
			disk_del(disks[i]);
			disk_put(disks[i]); /* may release the unit (free_disk) */
		} else if (msd->legacy_registered) {
			msd->ready = 0;
			block_unregister(&msd->blk);
			msd_lun_release(msd);
		} else {
			msd_lun_release(msd);
		}
	}
	/* The attachment's hold: the slot is free once the units are. */
	msd_host_put(h);
}

//=============================================================================
// The attach thread ("kusbstor")
//=============================================================================

#define MSD_ATTACH_QUEUE 16

struct msd_attach_req {
	xhci_controller_t *ctrl;
	usb_device_t *dev;
	uint32_t gen;
};

static struct msd_attach_req g_attach_q[MSD_ATTACH_QUEUE]; /* g_msd_lock */
static int g_attach_n; /* g_msd_lock */
/* The request the attach thread took off the queue and works on (until it
 * is attached, given up, or queued again); g_msd_lock */
static usb_device_t *g_attach_cur_dev;
static uint32_t g_attach_cur_gen;
static volatile int g_msd_hotplug; /* the boot phase is over */
static volatile int g_kusbstor_wait; /* its wait channel */
static volatile int g_kusbstor_kicked; /* work was queued since it looked */
static task_t *g_kusbstor;
static uint8_t g_kusbstor_stack[32768] __attribute__((aligned(16)));

/* Wake the attach thread.  The flag is set under the lock the thread
 * checks it under before it blocks: either it sees the flag, or it is
 * blocked already when the wake-up comes.  Never with g_msd_lock held. */
static void msd_kick_attach_thread(void)
{
	uint64_t flags;

	spin_lock_irqsave(&g_msd_lock, &flags);
	g_kusbstor_kicked = 1;
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (g_kusbstor)
		sched_wake_channel((void *)&g_kusbstor_wait);
}

/* Queue an attach request (g_msd_lock held).  0, or -1 when the queue is
 * full or the device is queued already. */
static int msd_attach_queue_locked(xhci_controller_t *ctrl, usb_device_t *dev,
				   uint32_t gen)
{
	for (int i = 0; i < g_attach_n; i++)
		if (g_attach_q[i].dev == dev && g_attach_q[i].gen == gen)
			return -1;
	if (g_attach_n >= MSD_ATTACH_QUEUE)
		return -1;
	g_attach_q[g_attach_n].ctrl = ctrl;
	g_attach_q[g_attach_n].dev = dev;
	g_attach_q[g_attach_n].gen = gen;
	g_attach_n++;
	return 0;
}

/* One piece of work, taken under the lock: a detach first (a removal must
 * not wait for a slow attach of another device more than necessary), then
 * a medium change, then an attach.  1 when something was done. */
static int msd_attach_thread_step(void)
{
	usb_msd_host_t *det = NULL;
	usb_msd_device_t *media = NULL;
	struct gendisk *media_disk = NULL;
	struct msd_attach_req req = { NULL, NULL, 0 };
	int have_req = 0;
	uint64_t flags;

	spin_lock_irqsave(&g_msd_lock, &flags);
	for (int i = 0; i < USB_MSD_MAX_HOSTS && !det; i++) {
		if (g_msd_hosts[i].detach_pending) {
			g_msd_hosts[i].detach_pending = 0;
			det = &g_msd_hosts[i];
		}
	}
	if (!det) {
		for (int i = 0; i < USB_MSD_MAX_DEVICES && !media; i++) {
			usb_msd_device_t *m = &g_msd_devices[i];
			if (m->state == USB_MSD_LUN_BOUND && m->media_event) {
				m->media_event = 0;
				if (m->disk && m->host && !m->host->gone &&
				    m->host->state == USB_MSD_HOST_LIVE) {
					media = m;
					media_disk = m->disk;
					disk_get(media_disk);
				}
			}
		}
	}
	if (!det && !media && g_attach_n) {
		req = g_attach_q[0];
		for (int i = 1; i < g_attach_n; i++)
			g_attach_q[i - 1] = g_attach_q[i];
		g_attach_n--;
		have_req = 1;
		g_attach_cur_dev = req.dev;
		g_attach_cur_gen = req.gen;
	}
	spin_unlock_irqrestore(&g_msd_lock, flags);

	if (det) {
		msd_host_detach(det);
		return 1;
	}
	if (media) {
		kprintf(MSD_LOG "%s: medium %s\n", media_disk->disk_name,
			media->medium ? "present" : "removed");
		/* A medium swapped for another (the unit reported the change
		 * while one was in): the old one is gone first, with its
		 * partitions, before the new one is read -- it may not even
		 * have the same block size. */
		if (media->medium &&
		    (disk_flags(media_disk) & GENHD_FL_MEDIA_PRESENT))
			disk_media_changed(media_disk, 0);
		disk_media_changed(media_disk, media->medium);
		disk_put(media_disk);
		return 1;
	}
	if (!have_req)
		return 0;

	/* The device may be gone by now, or its slot taken by another one:
	 * the claim checks under the lock.  A stale entry is dropped here,
	 * before it spends an attempt of the device now in the slot. */
	if (req.dev->gen == req.gen &&
	    req.dev->msd_attempts < MSD_ATTACH_ATTEMPTS) {
		req.dev->msd_attempts++;
		int st = msd_attach(req.ctrl, req.dev, req.gen, 0);
		if (st != ST_OK && st != ST_EXISTS && st != ST_NO_DEVICE) {
			if (req.dev->msd_attempts < MSD_ATTACH_ATTEMPTS) {
				/* Some devices answer only after a pause. */
				msd_msleep(1000);
				spin_lock_irqsave(&g_msd_lock, &flags);
				(void)msd_attach_queue_locked(req.ctrl, req.dev,
							      req.gen);
				spin_unlock_irqrestore(&g_msd_lock, flags);
			} else {
				kprintf(MSD_LOG "port %u: device %04x:%04x "
					"could not be attached (%d)\n",
					req.dev->port, req.dev->vendor_id,
					req.dev->product_id, st);
			}
		}
	}
	spin_lock_irqsave(&g_msd_lock, &flags);
	g_attach_cur_dev = NULL;
	spin_unlock_irqrestore(&g_msd_lock, flags);
	return 1;
}

static void msd_attach_thread(void *arg)
{
	task_t *cur = sched_current();

	(void)arg;
	BUG_ON(cur == NULL);
	for (;;) {
		__atomic_store_n(&g_kusbstor_kicked, 0, __ATOMIC_SEQ_CST);
		if (msd_attach_thread_step())
			continue;
		/* Nothing to do: sleep until kicked.  A kick between the
		 * look above and the block below is seen by the flag; the
		 * nap backs that up. */
		uint64_t flags;
		spin_lock_irqsave(&g_msd_lock, &flags);
		if (__atomic_load_n(&g_kusbstor_kicked, __ATOMIC_SEQ_CST)) {
			spin_unlock_irqrestore(&g_msd_lock, flags);
			continue;
		}
		cur->wait_channel = (void *)&g_kusbstor_wait;
		cur->wakeup_tick = timer_ticks() + timer_ms_to_ticks(1000) + 1;
		task_set_blocked(cur);
		spin_unlock_irqrestore(&g_msd_lock, flags);
		sched_schedule();
		cur->wakeup_tick = 0;
		cur->wait_channel = NULL;
		if (cur->state != TASK_RUNNING)
			cur->state = TASK_RUNNING;
	}
}

/* Start the attach thread (once; from the device-poll thread). */
static void msd_start_attach_thread(void)
{
	if (g_kusbstor)
		return;
	task_t *t = sched_add_task(msd_attach_thread, NULL, g_kusbstor_stack,
				   sizeof(g_kusbstor_stack));
	if (!t) {
		kprintf(MSD_LOG "could not start the attach thread; USB storage "
			"plugged in from now on is not attached\n");
		return;
	}
	ksnprintf(t->comm, sizeof(t->comm), "kusbstor");
	g_kusbstor = t;
}

//=============================================================================
// Entry points of the USB core
//=============================================================================

int usb_msd_attach_boot(xhci_controller_t *ctrl, usb_device_t *dev)
{
	int st;

	if (!ctrl || !dev)
		return ST_INVALID;
	if (g_msd_hotplug)
		return ST_BUSY; /* the attach thread has it */
	if (!dev->configured || dev->gone || !dev->msd_iface_valid)
		return ST_NOT_FOUND;
	{
		/* attached already (a claim would say so, but without
		 * spending an attempt), or handed to the attach thread */
		uint64_t flags;
		int known = 0;
		spin_lock_irqsave(&g_msd_lock, &flags);
		for (int i = 0; i < USB_MSD_MAX_HOSTS; i++) {
			usb_msd_host_t *o = &g_msd_hosts[i];
			if (o->state != USB_MSD_HOST_FREE && !o->gone &&
			    o->usb_dev == dev && o->usb_gen == dev->gen)
				known = o->state == USB_MSD_HOST_LIVE ? 1 : 2;
		}
		for (int i = 0; i < g_attach_n && !known; i++)
			if (g_attach_q[i].dev == dev &&
			    g_attach_q[i].gen == dev->gen)
				known = 2;
		if (!known && g_attach_cur_dev == dev &&
		    g_attach_cur_gen == dev->gen)
			known = 2;
		spin_unlock_irqrestore(&g_msd_lock, flags);
		if (known)
			return known == 1 ? ST_EXISTS : ST_BUSY;
	}
	if (dev->msd_attempts >= MSD_ATTACH_ATTEMPTS)
		return ST_NOT_FOUND;
	dev->msd_attempts++;
	st = msd_attach(ctrl, dev, dev->gen, 1);
	if (st == ST_AGAIN) {
		/* A unit is still becoming ready: the attach thread waits for
		 * it, off this thread (it also reads the console).  The root
		 * search picks its disk up once it is registered. */
		uint64_t flags;
		int rc;

		msd_start_attach_thread();
		rc = -1;
		if (g_kusbstor) {
			spin_lock_irqsave(&g_msd_lock, &flags);
			rc = msd_attach_queue_locked(ctrl, dev, dev->gen);
			spin_unlock_irqrestore(&g_msd_lock, flags);
		}
		if (rc == 0) {
			kprintf(MSD_LOG "port %u: device %04x:%04x not ready "
				"yet; attaching it in the background\n",
				dev->port, dev->vendor_id, dev->product_id);
			msd_kick_attach_thread();
			return ST_BUSY;
		}
		/* No attach thread to hand it to: wait for it here. */
		st = msd_attach(ctrl, dev, dev->gen, 0);
	}
	return st;
}

/* Hand every configured, unattached storage device of a controller to the
 * attach thread (g_msd_lock held). */
static int msd_queue_unattached_locked(xhci_controller_t *ctrl)
{
	int queued = 0;

	if (!ctrl || !ctrl->initialized)
		return 0;
	for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
		usb_device_t *dev = &ctrl->devices[i];
		int known = 0;

		if (!dev->configured || dev->gone || !dev->msd_iface_valid ||
		    dev->msd_attempts >= MSD_ATTACH_ATTEMPTS)
			continue;
		for (int j = 0; j < USB_MSD_MAX_HOSTS; j++) {
			usb_msd_host_t *o = &g_msd_hosts[j];
			if (o->state != USB_MSD_HOST_FREE && !o->gone &&
			    o->usb_dev == dev && o->usb_gen == dev->gen)
				known = 1;
		}
		if (g_attach_cur_dev == dev && g_attach_cur_gen == dev->gen)
			known = 1;
		if (!known && msd_attach_queue_locked(ctrl, dev, dev->gen) == 0)
			queued++;
	}
	return queued;
}

void usb_msd_boot_done(void)
{
	uint64_t flags;
	int queued;

	msd_start_attach_thread();
	spin_lock_irqsave(&g_msd_lock, &flags);
	g_msd_hotplug = 1;
	queued = msd_queue_unattached_locked(&g_xhci);
	queued += msd_queue_unattached_locked(&g_xhci_hid);
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (queued)
		msd_kick_attach_thread();
}

void usb_msd_probe(xhci_controller_t *ctrl, usb_device_t *dev)
{
	uint64_t flags;
	int rc;

	if (!ctrl || !dev || !dev->msd_iface_valid || !g_msd_hotplug)
		return;
	msd_start_attach_thread();
	spin_lock_irqsave(&g_msd_lock, &flags);
	rc = msd_attach_queue_locked(ctrl, dev, dev->gen);
	spin_unlock_irqrestore(&g_msd_lock, flags);
	if (rc == 0) {
		kprintf(MSD_LOG "port %u: USB mass storage device %04x:%04x "
			"found\n",
			dev->port, dev->vendor_id, dev->product_id);
		msd_kick_attach_thread();
	}
}

void usb_msd_disconnect(xhci_controller_t *ctrl, usb_device_t *dev)
{
	usb_msd_host_t *hit[USB_MSD_MAX_HOSTS];
	uint64_t flags;
	int n = 0, kick = 0;

	if (!ctrl || !dev)
		return;
	might_sleep();
	spin_lock_irqsave(&g_msd_lock, &flags);
	/* A pending attach of it is void. */
	for (int i = 0; i < g_attach_n;) {
		if (g_attach_q[i].dev == dev) {
			for (int j = i + 1; j < g_attach_n; j++)
				g_attach_q[j - 1] = g_attach_q[j];
			g_attach_n--;
		} else {
			i++;
		}
	}
	for (int i = 0; i < USB_MSD_MAX_HOSTS; i++) {
		usb_msd_host_t *h = &g_msd_hosts[i];
		if (h->state == USB_MSD_HOST_FREE || h->usb_dev != dev ||
		    h->ctrl != ctrl)
			continue;
		h->gone = 1;
		if (h->state == USB_MSD_HOST_LIVE) {
			h->state = USB_MSD_HOST_GONE;
			h->detach_pending = 1;
			kick = 1;
		}
		/* (one still attaching is detached by its attacher, which
		 * finds it gone) */
		hit[n++] = h;
	}
	spin_unlock_irqrestore(&g_msd_lock, flags);
	__asm__ volatile("" ::: "memory");

	/* The fence: every command checks `gone' under the I/O mutex, and a
	 * transfer running now aborts at once (the USB device is marked gone
	 * as well).  Once the mutex has been held here, nothing of this
	 * driver touches the device, its rings or its slot again -- the
	 * controller may disable the slot. */
	for (int i = 0; i < n; i++) {
		msd_io_lock(hit[i]);
		hit[i]->usb_dev = NULL;
		msd_io_unlock(hit[i]);
		kprintf(MSD_LOG "port %u: USB mass storage device %04x:%04x "
			"disconnected\n",
			hit[i]->port, hit[i]->vendor_id, hit[i]->product_id);
	}
	if (kick)
		msd_kick_attach_thread();
}
