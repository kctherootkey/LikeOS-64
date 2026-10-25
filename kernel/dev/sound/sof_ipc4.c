// LikeOS Intel audio DSP host driver - the IPC4 message channel.
//
// The host and the DSP talk through two doorbell pairs and two shared SRAM
// windows:
//
//   host -> DSP   IDR (primary word | BUSY), IDD (extension), payload in
//                 the "outbox" window; the DSP sets DONE in IDA when it has
//                 taken the message.
//   DSP -> host   TDR (primary | BUSY), TDD (extension), payload in the
//                 "inbox" window; the host acknowledges by clearing BUSY
//                 (and on cAVS by setting DONE in TDA).
//
// A reply to a host request arrives as a DSP->host message with the reply
// direction bit set and the status in the low 24 bits; everything else
// coming that way is a notification (firmware ready, exception, resource
// events, log buffer updates).
//
// Requests are serialised: one transaction owns the doorbell from writing
// IDR until its reply is consumed.  The reply is collected by whoever
// services the doorbells first - the interrupt handler or the waiting
// sender itself, which polls so that a request also completes with
// interrupts disabled (PCM trigger) or when an interrupt is lost.
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"

#include <kernel/hal/lapic.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>

/* ===================================================================== */
/* register layout                                                        */
/* ===================================================================== */

struct sof_ipc_regs {
	uint32_t tdr, tda, tdd, idr, ida, idd, ctl;
};

static void sof_ipc_regs_of(struct sof_host *h, struct sof_ipc_regs *r)
{
	if (h->chip->ipc_layout == SOF_IPC_LAYOUT_ACE) {
		r->tdr = SOF_ACE_IPC_TDR;
		r->tda = SOF_ACE_IPC_TDA;
		r->tdd = SOF_ACE_IPC_TDD;
		r->idr = SOF_ACE_IPC_IDR;
		r->ida = SOF_ACE_IPC_IDA;
		r->idd = SOF_ACE_IPC_IDD;
		r->ctl = SOF_ACE_IPC_CTL;
	} else {
		r->tdr = SOF_CAVS_IPC_TDR;
		r->tda = SOF_CAVS_IPC_TDA;
		r->tdd = SOF_CAVS_IPC_TDD;
		r->idr = SOF_CAVS_IPC_IDR;
		r->ida = SOF_CAVS_IPC_IDA;
		r->idd = SOF_CAVS_IPC_IDD;
		r->ctl = SOF_CAVS_IPC_CTL;
	}
}

void sof_ipc4_init_host(struct sof_host *h)
{
	if (sof_is_ace(h)) {
		h->fwregs_off = SOF_ACE_SRAM_WINDOW(SOF_IPC4_WINDOW_INBOX);
		h->inbox_off = SOF_ACE_SRAM_WINDOW(SOF_IPC4_WINDOW_INBOX) +
			       SOF_IPC4_UPLINK_OFFSET;
		h->outbox_off = SOF_ACE_SRAM_WINDOW(SOF_IPC4_WINDOW_OUTBOX);
		h->debug_off = SOF_ACE_SRAM_WINDOW(SOF_IPC4_WINDOW_DEBUG);
	} else {
		h->fwregs_off = SOF_CAVS_SRAM_WINDOW(SOF_IPC4_WINDOW_INBOX);
		h->inbox_off = SOF_CAVS_SRAM_WINDOW(SOF_IPC4_WINDOW_INBOX) +
			       SOF_IPC4_UPLINK_OFFSET;
		h->outbox_off = SOF_CAVS_SRAM_WINDOW(SOF_IPC4_WINDOW_OUTBOX);
		h->debug_off = SOF_CAVS_SRAM_WINDOW(SOF_IPC4_WINDOW_DEBUG);
	}
	/* a window must lie inside what was mapped */
	if (h->inbox_off + SOF_IPC4_MAILBOX_SIZE > h->bar4_map_size)
		h->inbox_off = 0;
	if (h->outbox_off + SOF_IPC4_MAILBOX_SIZE > h->bar4_map_size)
		h->outbox_off = 0;
	if (h->debug_off + SOF_IPC4_DEBUG_SLOT_SIZE > h->bar4_map_size)
		h->debug_off = 0;
	if (h->fwregs_off + 64 > h->bar4_map_size)
		h->fwregs_off = 0;
	h->s.ipc_mailbox = h->outbox_off ? (uint8_t *)(h->s.dsp + h->outbox_off) :
					   NULL;
	h->dl_mbox_bytes = SOF_IPC4_MAILBOX_SIZE;
	h->ul_mbox_bytes = SOF_IPC4_MAILBOX_SIZE;
	spinlock_init(&h->s.ipc_lock, "sof-ipc");
	wq_head_init(&h->ipc_wq, "sof-ipc");
	if (!h->rx_buf)
		h->rx_buf = kcalloc(1, SOF_IPC4_MAILBOX_SIZE);
}

/* Forget any transaction state (before a (re)boot). */
void sof_ipc4_reset(struct sof_host *h)
{
	uint64_t fl;

	spin_lock_irqsave(&h->s.ipc_lock, &fl);
	h->ipc_live = 0;
	h->fw_ready = 0;
	h->fw_ready_primary = 0;
	h->tx_busy = 0;
	h->tx_waiting_reply = 0;
	h->tx_acked = 0;
	h->s.ipc_reply_ready = 0;
	h->crashed = 0;
	h->panic_dumped = 0;
	spin_unlock_irqrestore(&h->s.ipc_lock, fl);
}

/* ===================================================================== */
/* status codes                                                           */
/* ===================================================================== */

struct sof_status_text {
	uint32_t code;
	const char *text;
};

static const struct sof_status_text sof_ipc4_status[] = {
	{ 0, "success" },
	{ 1, "invalid parameter" },
	{ 2, "unknown message type" },
	{ 3, "reply buffer too small" },
	{ 4, "busy" },
	{ 6, "unknown failure" },
	{ 7, "unsupported operation" },
	{ 9, "resource not found" },
	{ 10, "resource id already in use" },
	{ 12, "resource in an invalid state" },
	{ 13, "power transition failed" },
	{ 14, "invalid library manifest" },
	{ 15, "service unavailable on this platform" },
	{ 42, "library address outside the storage memory" },
	{ 44, "image verification failed" },
	{ 100, "module management error" },
	{ 101, "module load failed" },
	{ 102, "module integrity check failed" },
	{ 103, "module code still in use" },
	{ 104, "module instance initialisation failed" },
	{ 110, "module id out of range" },
	{ 111, "invalid module instance" },
	{ 112, "invalid queue (pin) id" },
	{ 113, "invalid destination queue (pin) id" },
	{ 116, "invalid target code id" },
	{ 117, "injection buffer too small" },
	{ 118, "extraction buffer too small" },
	{ 120, "invalid configuration item id" },
	{ 121, "invalid configuration item length" },
	{ 122, "invalid configuration item" },
	{ 140, "DMA gateway initialisation failed" },
	{ 141, "invalid gateway id" },
	{ 142, "gateway state change failed" },
	{ 143, "gateway not allocated" },
	{ 150, "I2S clock reconfiguration while running (SCLK)" },
	{ 151, "I2S clock reconfiguration while running (MCLK)" },
	{ 152, "SCLK not running" },
	{ 153, "MCLK not running" },
};

const char *sof_ipc4_status_text(uint32_t status)
{
	size_t i;

	status &= SOF_IPC4_STATUS_MASK;
	for (i = 0; i < sizeof(sof_ipc4_status) / sizeof(sof_ipc4_status[0]);
	     i++)
		if (sof_ipc4_status[i].code == status)
			return sof_ipc4_status[i].text;
	return "unknown status";
}

int sof_ipc4_status_errno(uint32_t status)
{
	status &= SOF_IPC4_STATUS_MASK;
	switch (status) {
	case 0:
		return 0;
	case 2:
	case 7:
	case 15:
		return -EOPNOTSUPP;
	case 3:
		return -EMSGSIZE;
	case 4:
	case 103:
	case 150:
	case 151:
		return -EBUSY;
	case 9:
	case 143:
		return -ENOENT;
	case 10:
		return -EEXIST;
	case 13:
		return -EIO;
	case 14:
	case 42:
	case 44:
	case 101:
	case 102:
		return -ENOEXEC;
	case 12:
	case 142:
	case 152:
	case 153:
		return -EPROTO;
	default:
		return -EINVAL;
	}
}

static const char *const sof_notif_names[SOF_IPC4_NOTIFY_TYPE_LAST] = {
	[SOF_IPC4_NOTIFY_PHRASE_DETECTED] = "phrase-detected",
	[SOF_IPC4_NOTIFY_RESOURCE_EVENT] = "resource-event",
	[SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS] = "log-buffer",
	[SOF_IPC4_NOTIFY_TIMESTAMP_CAPTURED] = "timestamp",
	[SOF_IPC4_NOTIFY_FW_READY] = "fw-ready",
	[SOF_IPC4_NOTIFY_FW_AUD_CLASS_RESULT] = "audio-classifier",
	[SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT] = "exception",
	[SOF_IPC4_NOTIFY_MODULE_NOTIFICATION] = "module",
	[SOF_IPC4_NOTIFY_PROBE_DATA_AVAILABLE] = "probe-data",
	[SOF_IPC4_NOTIFY_ASYNC_MSG_SRVC_MESSAGE] = "async-message",
};

const char *sof_ipc4_notif_name(uint32_t type)
{
	if (type < SOF_IPC4_NOTIFY_TYPE_LAST && sof_notif_names[type])
		return sof_notif_names[type];
	return "unknown";
}

static const char *sof_resource_event_name(uint32_t ev)
{
	switch (ev) {
	case 1:
		return "mixer underrun";
	case 3:
		return "processing error";
	case 6:
		return "gateway underrun";
	case 7:
		return "gateway overrun";
	default:
		return "event";
	}
}

/* ===================================================================== */
/* mailbox copies (MMIO, 32-bit accesses only)                            */
/* ===================================================================== */

static void sof_mbox_write(struct sof_host *h, uint32_t off, const void *src,
			   size_t len)
{
	const uint8_t *s = src;
	size_t i;

	for (i = 0; i + 4 <= len; i += 4) {
		uint32_t w = (uint32_t)s[i] | ((uint32_t)s[i + 1] << 8) |
			     ((uint32_t)s[i + 2] << 16) |
			     ((uint32_t)s[i + 3] << 24);
		sof_dsp_w32(h, off + (uint32_t)i, w);
	}
	if (i < len) {
		uint32_t w = 0;
		size_t k;

		for (k = 0; i + k < len; k++)
			w |= (uint32_t)s[i + k] << (8 * k);
		sof_dsp_w32(h, off + (uint32_t)i, w);
	}
}

static void sof_mbox_read(struct sof_host *h, uint32_t off, void *dst,
			  size_t len)
{
	uint8_t *d = dst;
	size_t i;

	for (i = 0; i < len; i += 4) {
		uint32_t w = sof_dsp_r32(h, off + (uint32_t)i);
		size_t k;

		for (k = 0; k < 4 && i + k < len; k++)
			d[i + k] = (uint8_t)(w >> (8 * k));
	}
}

/* ===================================================================== */
/* doorbell service                                                       */
/* ===================================================================== */

/* We consumed a DSP->host message: clear BUSY so the DSP may send again. */
static void sof_ipc_host_done(struct sof_host *h, const struct sof_ipc_regs *r)
{
	sof_dsp_w32(h, r->tdr, sof_dsp_r32(h, r->tdr) | SOF_IPC_BUSY);
	if (h->chip->ipc_layout == SOF_IPC_LAYOUT_ACE)
		sof_dsp_w32(h, r->tda, sof_dsp_r32(h, r->tda) & ~SOF_IPC_BUSY);
	else
		sof_dsp_w32(h, r->tda, sof_dsp_r32(h, r->tda) | SOF_IPC_DONE);
}

/* The DSP took our message: acknowledge DONE and unmask its interrupt. */
static void sof_ipc_dsp_done(struct sof_host *h, const struct sof_ipc_regs *r)
{
	sof_dsp_w32(h, r->ida, sof_dsp_r32(h, r->ida) | SOF_IPC_DONE);
	sof_dsp_upd32(h, r->ctl, SOF_IPC_CTL_DONE, SOF_IPC_CTL_DONE);
}

static void sof_panic_work(void *arg)
{
	struct sof_host *h = arg;

	sof_dsp_panic_dump(h);
	if (h->s.card)
		snd_card_event(h->s.card, AUDIO_EV_XRUN, 0, AUDIO_DIR_PLAYBACK);
}

/* Firmware log lines arriving on their own (warnings while streaming): a
 * bounded number per boot, the rest is consumed silently. */
#define SOF_FW_LOG_NOTIFY_LINES 64

static void sof_fw_log_work(void *arg)
{
	struct sof_host *h = arg;
	int left = SOF_FW_LOG_NOTIFY_LINES - (int)h->fw_log_lines;

	__atomic_store_n(&h->fw_log_work_queued, 0, __ATOMIC_RELEASE);
	if (left < 0)
		left = 0;
	if (left > 16)
		left = 16;
	h->fw_log_lines += (uint32_t)left;
	sof_dsp_fw_log_flush(h, left);
}

/* A notification with a payload: read the bits worth logging.  Called
 * with the IPC lock held; reads only a few words. */
static void sof_ipc_notification(struct sof_host *h, uint32_t primary,
				 uint32_t ext, int *crash)
{
	uint32_t type = SOF_IPC4_NOTIF_TYPE_GET(primary);
	uint32_t w[10];

	if (type < SOF_IPC4_NOTIFY_TYPE_LAST)
		h->notif_count[type]++;
	else
		h->notif_unknown++;

	switch (type) {
	case SOF_IPC4_NOTIFY_FW_READY:
		h->fw_ready_primary = primary;
		h->fw_ready = 1;
		break;
	case SOF_IPC4_NOTIFY_EXCEPTION_CAUGHT:
		h->exception_count++;
		if (!h->crashed) {
			h->crashed = 1;
			*crash = 1;
		}
		break;
	case SOF_IPC4_NOTIFY_RESOURCE_EVENT:
		if (!h->inbox_off)
			break;
		sof_mbox_read(h, h->inbox_off, w, sizeof(w));
		/* resource type, id, event type, reserved, event data */
		if (w[2] == 1 || w[2] == 6)
			h->underruns++;
		else if (w[2] == 7)
			h->overruns++;
		if (h->notif_count[type] <= 8)
			kprintf("[snd] sof: %s: %s on %s %#x (data %#x %#x %#x)\n",
				h->id->tag, sof_resource_event_name(w[2]),
				w[0] == 0 ? "module" :
				w[0] == 1 ? "pipeline" :
				w[0] == 2 ? "gateway" :
					    "resource",
				w[1], w[4], w[5], w[6]);
		break;
	case SOF_IPC4_NOTIFY_MODULE_NOTIFICATION:
		if (!h->inbox_off)
			break;
		sof_mbox_read(h, h->inbox_off, w, 12);
		if (h->notif_count[type] <= 8)
			kprintf("[snd] sof: %s: module %#x instance %u event %#x (%u bytes)\n",
				h->id->tag, w[0] >> 16, w[0] & 0xffff, w[1],
				w[2]);
		break;
	case SOF_IPC4_NOTIFY_LOG_BUFFER_STATUS:
		/* the firmware logged something (core in bits 15:12): print
		 * it from the work thread, not under the IPC lock */
		if (h->fw_log_on &&
		    !__atomic_exchange_n(&h->fw_log_work_queued, 1,
					 __ATOMIC_ACQ_REL) &&
		    snd_schedule_work(sof_fw_log_work, h, 0) < 0)
			__atomic_store_n(&h->fw_log_work_queued, 0,
					 __ATOMIC_RELEASE);
		break;
	default:
		if (h->notif_unknown + (type < SOF_IPC4_NOTIFY_TYPE_LAST ?
						h->notif_count[type] :
						0) <=
		    4)
			kprintf("[snd] sof: %s: notification %s (%#x|%#x)\n",
				h->id->tag, sof_ipc4_notif_name(type), primary,
				ext);
		break;
	}
}

int sof_ipc4_service(struct sof_host *h)
{
	struct sof_ipc_regs r;
	uint32_t ida, tdr;
	int handled = 0, crash = 0;
	uint64_t fl;

	/* before the ROM handshake the doorbells belong to the loader */
	if (!h->s.dsp || !h->chip || !h->ipc_live)
		return 0;
	sof_ipc_regs_of(h, &r);

	spin_lock_irqsave(&h->s.ipc_lock, &fl);
	ida = sof_dsp_r32(h, r.ida);
	tdr = sof_dsp_r32(h, r.tdr);
	if (ida == 0xffffffffu || tdr == 0xffffffffu) {
		spin_unlock_irqrestore(&h->s.ipc_lock, fl);
		return 0;
	}

	if (ida & SOF_IPC_DONE) {
		/* mask DONE until we acknowledged it */
		sof_dsp_upd32(h, r.ctl, SOF_IPC_CTL_DONE, 0);
		sof_ipc_dsp_done(h, &r);
		h->tx_acked = 1;
		handled = 1;
	}

	if (tdr & SOF_IPC_BUSY) {
		uint32_t primary = tdr & SOF_IPC_MSG_MASK;
		uint32_t ext = sof_dsp_r32(h, r.tdd);

		if (h->chip->ipc_layout == SOF_IPC_LAYOUT_CAVS)
			ext &= SOF_IPC_MSG_MASK;

		if (primary & SOF_IPC4_DIR_REPLY) {
			if (h->tx_waiting_reply) {
				uint32_t status = primary & SOF_IPC4_STATUS_MASK;

				h->s.ipc_reply_primary = primary;
				h->s.ipc_reply_ext = ext;
				h->rx_len = 0;
				/* the payload must be read before the host
				 * acknowledges: the DSP reuses the window */
				if (!status && h->rx_want && h->rx_buf &&
				    h->inbox_off) {
					uint32_t n = h->rx_want;

					if (n > SOF_IPC4_MAILBOX_SIZE)
						n = SOF_IPC4_MAILBOX_SIZE;
					sof_mbox_read(h, h->inbox_off, h->rx_buf,
						      n);
					h->rx_len = n;
				}
				h->tx_waiting_reply = 0;
				h->s.ipc_reply_ready = 1;
			}
			/* else: a reply nobody waits for (timed out) */
		} else {
			sof_ipc_notification(h, primary, ext, &crash);
		}
		sof_ipc_host_done(h, &r);
		handled = 1;
	}
	spin_unlock_irqrestore(&h->s.ipc_lock, fl);

	if (handled)
		wq_wake_all(&h->ipc_wq);
	if (crash) {
		h->boot_state = SOF_BOOT_CRASHED;
		kprintf("[snd] sof: %s: DSP firmware reported an exception\n",
			h->id->tag);
		if (snd_schedule_work(sof_panic_work, h, 0) < 0)
			h->panic_dumped = 0; /* dumped on the next diag */
	}
	return handled;
}

void sof_ipc4_dump_regs(struct sof_host *h)
{
	struct sof_ipc_regs r;

	if (!h->s.dsp || !h->chip)
		return;
	sof_ipc_regs_of(h, &r);
	kprintf("[snd] sof: %s: IPC host->dsp %#x|%#x|%#x  dsp->host %#x|%#x|%#x  ctl %#x\n",
		h->id->tag, sof_dsp_r32(h, r.idr), sof_dsp_r32(h, r.idd),
		sof_dsp_r32(h, r.ida), sof_dsp_r32(h, r.tdr),
		sof_dsp_r32(h, r.tdd), sof_dsp_r32(h, r.tda),
		sof_dsp_r32(h, r.ctl));
}

/* ===================================================================== */
/* transactions                                                           */
/* ===================================================================== */

/* Wait for the condition `*flag' to become non-zero; services the doorbell
 * between checks.  Returns 0 or -ETIMEDOUT. */
static int sof_ipc_wait_flag(struct sof_host *h, volatile int *flag,
			     uint32_t timeout_us)
{
	uint64_t start = sof_now_us();
	uint64_t spent = 0;
	uint32_t spins = 0;
	int can_sleep = sof_can_sleep();

	for (;;) {
		uint64_t now;

		sof_ipc4_service(h);
		if (*flag)
			return 0;
		if (h->crashed)
			return -EIO;
		if (can_sleep && spins >= 50) {
			task_t *cur = sched_current();
			struct wait_queue_entry we;
			uint64_t fl = local_irq_save();

			wq_entry_init(&we, cur);
			wq_add(&h->ipc_wq, &we);
			if (*flag) {
				local_irq_restore(fl);
				wq_remove(&h->ipc_wq, &we);
				return 0;
			}
			cur->wait_channel = &h->ipc_wq;
			cur->wakeup_tick = timer_ticks() + 1;
			task_set_blocked(cur);
			local_irq_restore(fl);
			sched_schedule();
			cur->wakeup_tick = 0;
			cur->wait_channel = NULL;
			wq_remove(&h->ipc_wq, &we);
		} else {
			lapic_delay_us(10);
			spent += 10;
			spins++;
		}
		now = sof_now_us() - start;
		if (now > spent)
			spent = now;
		if (spent >= timeout_us) {
			sof_ipc4_service(h);
			return *flag ? 0 : -ETIMEDOUT;
		}
	}
}

static int sof_ipc_claim(struct sof_host *h, uint32_t timeout_us)
{
	uint64_t start = sof_now_us();
	uint64_t spent = 0;

	for (;;) {
		uint64_t fl, now;

		spin_lock_irqsave(&h->s.ipc_lock, &fl);
		if (!h->tx_busy) {
			h->tx_busy = 1;
			spin_unlock_irqrestore(&h->s.ipc_lock, fl);
			return 0;
		}
		spin_unlock_irqrestore(&h->s.ipc_lock, fl);
		/* help the owner along: its reply may be sitting there */
		sof_ipc4_service(h);
		if (sof_can_sleep()) {
			sched_yield_in_kernel();
		} else {
			lapic_delay_us(10);
			spent += 10;
		}
		now = sof_now_us() - start;
		if (now > spent)
			spent = now;
		if (spent >= timeout_us)
			return -EBUSY;
	}
}

static void sof_ipc_release(struct sof_host *h)
{
	uint64_t fl;

	spin_lock_irqsave(&h->s.ipc_lock, &fl);
	h->tx_busy = 0;
	h->tx_waiting_reply = 0;
	spin_unlock_irqrestore(&h->s.ipc_lock, fl);
	wq_wake_all(&h->ipc_wq);
}

/* One request/reply exchange.  `boot' allows it before FW_READY. */
static int sof_ipc_tx(struct sof_host *h, uint32_t primary, uint32_t ext,
		      const void *data, size_t len, void *reply,
		      size_t reply_max, size_t *reply_len)
{
	struct sof_ipc_regs r;
	uint32_t timeout = sof_can_sleep() ? SOF_IPC_TIMEOUT_US :
					     SOF_IPC_ATOMIC_TIMEOUT_US;
	uint32_t last = 0;
	uint32_t status;
	uint64_t fl;
	int rc;

	if (reply_len)
		*reply_len = 0;
	if (len > h->dl_mbox_bytes || reply_max > h->ul_mbox_bytes)
		return -EMSGSIZE;
	if ((len && !h->outbox_off) || (reply_max && !h->inbox_off))
		return -ENODEV;
	if (!h->rx_buf)
		return -ENOMEM;
	if (h->crashed)
		return -EIO;

	if (h->ipc_timeout_us > timeout && sof_can_sleep())
		timeout = h->ipc_timeout_us;
	sof_ipc_regs_of(h, &r);
	rc = sof_ipc_claim(h, timeout);
	if (rc) {
		sof_log(h, "IPC channel busy, dropping %#x|%#x\n", primary, ext);
		return rc;
	}

	/* the previous message must have been taken */
	rc = sof_poll_dsp32(h, r.idr, SOF_IPC_BUSY, 0, timeout, &last);
	if (rc) {
		sof_log(h, "doorbell still busy (%#x), dropping %#x|%#x\n", last,
			primary, ext);
		h->ipc_timeouts++;
		sof_ipc_release(h);
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&h->s.ipc_lock, &fl);
	if (len)
		sof_mbox_write(h, h->outbox_off, data, len);
	h->rx_want = (uint32_t)reply_max;
	h->rx_len = 0;
	h->s.ipc_reply_ready = 0;
	h->tx_waiting_reply = 1;
	h->tx_acked = 0;
	h->tx_primary = primary;
	h->tx_extension = ext;
	sof_dsp_w32(h, r.idd, ext);
	sof_dsp_w32(h, r.idr, (primary & SOF_IPC_MSG_MASK) | SOF_IPC_BUSY);
	spin_unlock_irqrestore(&h->s.ipc_lock, fl);
	h->ipc_tx_count++;

	rc = sof_ipc_wait_flag(h, &h->s.ipc_reply_ready, timeout);
	if (rc) {
		h->ipc_timeouts++;
		if (h->ipc_timeouts <= 8) {
			sof_log(h, "IPC %#x|%#x %s\n", primary, ext,
				rc == -EIO ? "aborted: firmware crashed" :
					     "timed out");
			sof_ipc4_dump_regs(h);
			sof_dsp_fw_log_flush(h, 32);
		}
		if (sof_dsp_rom_status(h) & SOF_FSR_HALTED) {
			h->crashed = 1;
			h->boot_state = SOF_BOOT_CRASHED;
			snd_schedule_work(sof_panic_work, h, 0);
		}
		sof_ipc_release(h);
		return rc;
	}

	status = h->s.ipc_reply_primary & SOF_IPC4_STATUS_MASK;
	rc = sof_ipc4_status_errno(status);
	if (rc) {
		h->ipc_err_count++;
		h->last_error_status = status;
		h->last_error_primary = primary;
		if (h->ipc_err_count <= 16)
			sof_log(h, "IPC %#x|%#x failed: status %u (%s)\n",
				primary, ext, status,
				sof_ipc4_status_text(status));
		/* the firmware says why in its log */
		sof_dsp_fw_log_flush(h, 32);
	} else if (reply && reply_max) {
		size_t n = h->rx_len < reply_max ? h->rx_len : reply_max;

		kmemcpy(reply, h->rx_buf, n);
		if (reply_len)
			*reply_len = n;
	}
	sof_ipc_release(h);
	return rc;
}

static int sof_is_lcfg(uint32_t primary, uint32_t *type)
{
	uint32_t t = SOF_IPC4_TYPE_GET(primary);

	*type = t;
	if (!(primary & SOF_IPC4_TARGET_MODULE))
		return 0;
	return t == SOF_IPC4_MOD_LARGE_CONFIG_SET ||
	       t == SOF_IPC4_MOD_LARGE_CONFIG_GET;
}

/* LARGE_CONFIG_SET/GET of any size, cut into mailbox-sized blocks.  The
 * first block carries the total size, later ones the data offset. */
static int sof_ipc4_lcfg_xfer(struct sof_host *h, uint32_t primary,
			      uint32_t ext_keep, int set, const void *tx,
			      size_t txlen, void *buf, size_t buflen,
			      size_t *out_len)
{
	size_t limit = set ? h->dl_mbox_bytes : h->ul_mbox_bytes;
	size_t remaining = buflen, offset = 0;
	uint32_t base = (primary & ~SOF_IPC4_TYPE_MASK) | SOF_IPC4_TARGET_MODULE;
	int first = 1;
	int rc = 0;

	if (out_len)
		*out_len = 0;
	if (!limit)
		limit = SOF_IPC4_MAILBOX_SIZE;
	if (buflen > SOF_IPC4_LCFG_SIZE_MASK)
		return -EMSGSIZE;
	if (!set && txlen > h->dl_mbox_bytes)
		return -EMSGSIZE;
	base |= SOF_IPC4_TYPE(set ? SOF_IPC4_MOD_LARGE_CONFIG_SET :
				    SOF_IPC4_MOD_LARGE_CONFIG_GET);
	ext_keep &= ~(SOF_IPC4_LCFG_SIZE_MASK | SOF_IPC4_LCFG_FIRST |
		      SOF_IPC4_LCFG_LAST);

	do {
		size_t chunk = remaining > limit ? limit : remaining;
		uint32_t ext = ext_keep;
		size_t got = 0;

		if (first)
			ext |= SOF_IPC4_LCFG_FIRST |
			       ((uint32_t)buflen & SOF_IPC4_LCFG_SIZE_MASK);
		else
			ext |= (uint32_t)offset & SOF_IPC4_LCFG_SIZE_MASK;
		if (set && chunk == remaining)
			ext |= SOF_IPC4_LCFG_LAST;

		if (set) {
			rc = sof_ipc_tx(h, base, ext, (const uint8_t *)buf + offset,
					chunk, NULL, 0, NULL);
		} else {
			rc = sof_ipc_tx(h, base, ext, tx, txlen,
					(uint8_t *)buf + offset, chunk, &got);
		}
		if (rc) {
			if (offset)
				sof_log(h, "large config %s stopped at %u of %u bytes: %d\n",
					set ? "set" : "get", (unsigned)offset,
					(unsigned)buflen, rc);
			return rc;
		}
		if (!set && first &&
		    (h->s.ipc_reply_ext & SOF_IPC4_LCFG_FIRST)) {
			/* the firmware says how much there really is */
			size_t total = h->s.ipc_reply_ext & SOF_IPC4_LCFG_SIZE_MASK;

			if (total > buflen) {
				sof_log(h, "large config reply of %u bytes does not fit %u\n",
					(unsigned)total, (unsigned)buflen);
				return -EMSGSIZE;
			}
			if (total < chunk) {
				chunk = total;
				remaining = total;
			} else if (total < buflen) {
				remaining = total;
			}
		}
		offset += chunk;
		remaining -= chunk;
		first = 0;
	} while (remaining);

	if (out_len)
		*out_len = offset;
	return 0;
}

int sof_ipc4_large_config(struct sof_host *h, uint32_t module_id,
			  uint32_t instance, uint32_t param, void *data,
			  size_t len, int set, size_t *out_len)
{
	uint32_t primary = SOF_IPC4_TARGET_MODULE | SOF_IPC4_MOD_ID(module_id) |
			   SOF_IPC4_MOD_INSTANCE(instance);

	if (!data || !len)
		return -EINVAL;
	return sof_ipc4_lcfg_xfer(h, primary, SOF_IPC4_LCFG_PARAM(param), set,
				  NULL, 0, data, len, out_len);
}

/* The contract entry point used by the topology and SoundWire sides. */
int sof_ipc4_send(struct sof_dev *sdev, uint32_t primary, uint32_t extension,
		  const void *data, size_t len, void *reply, size_t reply_max)
{
	struct sof_host *h = sof_host_of(sdev);
	uint32_t type;

	if (!sdev || !h->s.dsp || !h->chip)
		return -ENODEV;
	if ((len && !data) || (reply_max && !reply))
		return -EINVAL;
	if (h->crashed)
		return -EIO;
	if (!h->fw_ready)
		return -ENODEV;
	if (primary & SOF_IPC4_DIR_REPLY)
		return -EINVAL; /* the host never sends replies */

	if (sof_is_lcfg(primary, &type)) {
		if (type == SOF_IPC4_MOD_LARGE_CONFIG_SET && len > h->dl_mbox_bytes)
			return sof_ipc4_lcfg_xfer(h, primary, extension, 1, NULL,
						  0, (void *)data, len, NULL);
		if (type == SOF_IPC4_MOD_LARGE_CONFIG_GET &&
		    reply_max > h->ul_mbox_bytes)
			return sof_ipc4_lcfg_xfer(h, primary, extension, 0, data,
						  len, reply, reply_max, NULL);
	}
	return sof_ipc_tx(h, primary, extension, data, len, reply, reply_max,
			  NULL);
}

/* ===================================================================== */
/* convenience messages                                                   */
/* ===================================================================== */

int sof_ipc4_glb(struct sof_host *h, uint32_t type, uint32_t primary_bits,
		 uint32_t extension, const void *data, size_t len)
{
	uint32_t primary = SOF_IPC4_TYPE(type) |
			   (primary_bits & ~(SOF_IPC4_TYPE_MASK |
					     SOF_IPC4_TARGET_MODULE |
					     SOF_IPC4_DIR_REPLY));

	return sof_ipc_tx(h, primary, extension, data, len, NULL, 0, NULL);
}

int sof_ipc4_set_pipeline_state(struct sof_host *h, uint32_t pipe,
				uint32_t state)
{
	return sof_ipc4_glb(h, SOF_IPC4_GLB_SET_PIPELINE_STATE,
			    SOF_IPC4_PPL_INSTANCE(pipe) | SOF_IPC4_PPL_STATE(state),
			    0, NULL, 0);
}

int sof_ipc4_set_dx(struct sof_host *h, uint32_t core_mask, uint32_t dx_mask)
{
	uint32_t payload[2] = { core_mask, dx_mask };
	uint32_t primary = SOF_IPC4_TARGET_MODULE |
			   SOF_IPC4_TYPE(SOF_IPC4_MOD_SET_DX);

	return sof_ipc_tx(h, primary, 0, payload, sizeof(payload), NULL, 0,
			  NULL);
}

/* Parse a tuple list {type, size, value[size]} into the host fields. */
static void sof_parse_fw_config(struct sof_host *h, const uint8_t *p,
				size_t len)
{
	size_t off = 0;

	while (off + 8 <= len) {
		uint32_t type = *(const uint32_t *)(p + off);
		uint32_t size = *(const uint32_t *)(p + off + 4);
		const uint32_t *v = (const uint32_t *)(p + off + 8);

		if (size > len - off - 8)
			break;
		if (size >= 4) {
			switch (type) {
			case SOF_IPC4_FW_CFG_FW_VERSION:
				if (size >= 8) {
					const uint16_t *ver = (const uint16_t *)v;

					h->s.fw_version[0] = ver[0];
					h->s.fw_version[1] = ver[1];
					h->s.fw_version[2] = ver[2];
					h->s.fw_version[3] = ver[3];
				}
				break;
			case SOF_IPC4_FW_CFG_SLOW_CLOCK_FREQ_HZ:
				h->cfg_slow_clk = v[0];
				break;
			case SOF_IPC4_FW_CFG_FAST_CLOCK_FREQ_HZ:
				h->cfg_fast_clk = v[0];
				break;
			case SOF_IPC4_FW_CFG_ALH_SUPPORT_LEVEL:
				h->cfg_alh_level = v[0];
				break;
			case SOF_IPC4_FW_CFG_DL_MAILBOX_BYTES:
				if (v[0] >= 256 && v[0] <= SOF_IPC4_MAILBOX_SIZE)
					h->dl_mbox_bytes = v[0];
				break;
			case SOF_IPC4_FW_CFG_UL_MAILBOX_BYTES:
				if (v[0] >= 256 && v[0] <= SOF_IPC4_MAILBOX_SIZE)
					h->ul_mbox_bytes = v[0];
				break;
			case SOF_IPC4_FW_CFG_TRACE_LOG_BYTES:
				h->cfg_trace_bytes = v[0];
				break;
			case SOF_IPC4_FW_CFG_MAX_PPL_COUNT:
				h->cfg_max_ppl = v[0];
				break;
			case SOF_IPC4_FW_CFG_MODULES_COUNT:
				h->cfg_modules = v[0];
				break;
			case SOF_IPC4_FW_CFG_MAX_MOD_INST_COUNT:
				h->cfg_max_mod_inst = v[0];
				break;
			case SOF_IPC4_FW_CFG_MAX_LIBS_COUNT:
				h->cfg_max_libs = v[0] ? v[0] : 1;
				break;
			case SOF_IPC4_FW_CFG_XTAL_FREQ_HZ:
				h->cfg_xtal = v[0];
				break;
			case SOF_IPC4_FW_CFG_POWER_GATING_POLICY:
				h->cfg_power_gating = v[0];
				break;
			case SOF_IPC4_FW_CFG_CONTEXT_SAVE:
				h->cfg_context_save = v[0];
				break;
			default:
				break;
			}
		}
		off += 8 + ((size + 3) & ~3u);
	}
}

static void sof_parse_hw_config(struct sof_host *h, const uint8_t *p,
				size_t len)
{
	size_t off = 0;

	while (off + 8 <= len) {
		uint32_t type = *(const uint32_t *)(p + off);
		uint32_t size = *(const uint32_t *)(p + off + 4);
		const uint32_t *v = (const uint32_t *)(p + off + 8);

		if (size > len - off - 8)
			break;
		if (size >= 4) {
			switch (type) {
			case SOF_IPC4_HW_CFG_DSP_CORES:
				h->hw_cores = v[0];
				break;
			case SOF_IPC4_HW_CFG_MEM_PAGE_BYTES:
				h->hw_page_bytes = v[0];
				break;
			case SOF_IPC4_HW_CFG_TOTAL_PHYS_MEM_PAGES:
				h->hw_total_pages = v[0];
				break;
			case SOF_IPC4_HW_CFG_GATEWAY_COUNT:
				h->hw_gateways = v[0];
				break;
			case SOF_IPC4_HW_CFG_MIC_PRIVACY_CAPS:
				/* version, length, capabilities[] */
				if (size >= 12)
					h->hw_mic_privacy = v[2];
				break;
			default:
				break;
			}
		}
		off += 8 + ((size + 3) & ~3u);
	}
}

/* ENABLE_LOGS: how often the firmware reports new log lines (ms), and a
 * priority mask per core -- bit 0 critical, 1 error, 2 warning, 3 info. */
struct sof_ipc4_log_state {
	uint32_t aging_timer_period;
	uint32_t fifo_full_timer_period;
	uint32_t enable;
	uint32_t logs_mask[8];
};

int sof_ipc4_enable_fw_logs(struct sof_host *h)
{
	struct sof_ipc4_log_state st;
	int i, rc;

	if (!h->debug_off)
		return -ENODEV;
	kmemset(&st, 0, sizeof(st));
	st.aging_timer_period = 100;
	st.fifo_full_timer_period = 100;
	st.enable = 1;
	for (i = 0; i < 8; i++)
		st.logs_mask[i] = 0x7; /* critical, errors, warnings */
	rc = sof_ipc4_large_config(h, SOF_IPC4_BASEFW_MODULE,
				   SOF_IPC4_BASEFW_INSTANCE,
				   SOF_IPC4_FW_PARAM_ENABLE_LOGS, &st, sizeof(st),
				   1, NULL);
	if (rc) {
		sof_log(h, "firmware log not enabled (%d); IPC errors will not say why\n",
			rc);
		return rc;
	}
	h->fw_log_on = 1;
	return 0;
}

int sof_ipc4_query_fw_config(struct sof_host *h)
{
	uint8_t *buf = kcalloc(1, SOF_IPC4_MAILBOX_SIZE);
	size_t got = 0;
	int rc;

	if (!buf)
		return -ENOMEM;
	rc = sof_ipc4_large_config(h, SOF_IPC4_BASEFW_MODULE,
				   SOF_IPC4_BASEFW_INSTANCE,
				   SOF_IPC4_FW_PARAM_FW_CONFIG, buf,
				   SOF_IPC4_MAILBOX_SIZE, 0, &got);
	if (rc) {
		sof_log(h, "firmware configuration query failed: %d\n", rc);
		goto out;
	}
	sof_parse_fw_config(h, buf, got);

	kmemset(buf, 0, SOF_IPC4_MAILBOX_SIZE);
	got = 0;
	if (sof_ipc4_large_config(h, SOF_IPC4_BASEFW_MODULE,
				  SOF_IPC4_BASEFW_INSTANCE,
				  SOF_IPC4_FW_PARAM_HW_CONFIG, buf,
				  SOF_IPC4_MAILBOX_SIZE, 0, &got) == 0)
		sof_parse_hw_config(h, buf, got);
	else
		sof_log(h, "hardware configuration query failed (ignored)\n");
out:
	kfree(buf);
	return rc;
}

long sof_ipc4_diag(struct sof_host *h, char *buf, size_t len)
{
	size_t pos = 0;
	uint32_t i;

	pos = sof_append(buf, len, pos,
			 "IPC4: %u requests, %u errors (last status %u '%s' for %#x), %u timeouts%s\n",
			 h->ipc_tx_count, h->ipc_err_count, h->last_error_status,
			 sof_ipc4_status_text(h->last_error_status),
			 h->last_error_primary, h->ipc_timeouts,
			 h->crashed ? ", FIRMWARE CRASHED" : "");
	pos = sof_append(buf, len, pos,
			 "  mailboxes: in %#x out %#x debug %#x (dl %u ul %u bytes)\n",
			 h->inbox_off, h->outbox_off, h->debug_off,
			 h->dl_mbox_bytes, h->ul_mbox_bytes);
	pos = sof_append(buf, len, pos, "  notifications:");
	for (i = 0; i < SOF_IPC4_NOTIFY_TYPE_LAST; i++)
		if (h->notif_count[i])
			pos = sof_append(buf, len, pos, " %s=%u",
					 sof_ipc4_notif_name(i),
					 h->notif_count[i]);
	if (h->notif_unknown)
		pos = sof_append(buf, len, pos, " unknown=%u", h->notif_unknown);
	pos = sof_append(buf, len, pos, "\n  underruns %u overruns %u exceptions %u\n",
			 h->underruns, h->overruns, h->exception_count);
	if (h->cfg_max_ppl || h->cfg_fast_clk)
		pos = sof_append(buf, len, pos,
				 "  firmware limits: %u pipelines, %u module instances, %u libraries, clocks %u/%u Hz, xtal %u Hz, ALH level %u\n",
				 h->cfg_max_ppl, h->cfg_max_mod_inst,
				 h->cfg_max_libs, h->cfg_slow_clk,
				 h->cfg_fast_clk, h->cfg_xtal, h->cfg_alh_level);
	if (h->hw_cores)
		pos = sof_append(buf, len, pos,
				 "  hardware: %u cores, %u pages of %u bytes, %u gateways, mic privacy %#x\n",
				 h->hw_cores, h->hw_total_pages, h->hw_page_bytes,
				 h->hw_gateways, h->hw_mic_privacy);
	return (long)pos;
}
