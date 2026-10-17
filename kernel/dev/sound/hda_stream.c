// LikeOS Intel High Definition Audio - stream DMA engines.
//
// An HD Audio controller has up to 30 stream descriptors: input engines
// first in register order, then output engines, then bidirectional ones.
// Each engine walks a buffer descriptor list (BDL) of address/length pairs
// cyclically and moves samples between memory and the link, tagged with a
// 4-bit stream number the codec's converter listens for.  This file owns
// the engines: handing them out, programming the BDL, the format and the
// tag, starting and stopping them (optionally in lock step through SSYNC),
// the DMA position (from the position buffer the controller writes into
// memory, or from the LPIB register), and the per-stream interrupt status.
//
// Position reporting is where controllers differ most.  The position
// buffer is cheap to read but on several parts it is updated a little
// after the buffer-completion interrupt, so at the interrupt it can still
// show the end of the previous period; on others it is never written at
// all.  LPIB is exact on most parts but on some reflects what went out on
// the link rather than what the DMA fetched.  The policy here: the table
// quirk decides (POSFIX_LPIB, POSFIX_POSBUF, POSFIX_COMBO); without one the
// position buffer is used, and the driver falls back to LPIB on its own
// when the buffer stays zero while LPIB moves.  Small backward steps are
// treated as jitter and the position never moves back within a lap, and a
// playback position lagging just short of a period boundary at the
// boundary's interrupt is snapped to the boundary.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_regs.h"
#include <kernel/mm/memory.h>
#include <kernel/hal/lapic.h>

/* Engine allocation, INTCTL and SSYNC read-modify-writes. */
static spinlock_t g_hda_stream_lock = SPINLOCK_INIT("hda_stream");

#define HDA_BDL_BYTES 4096 /* HDA_MAX_BDL entries of 16 bytes */
#define HDA_SRST_POLL 1000 /* x 1 us */
#define HDA_RUN_POLL 2000 /* x 1 us */
#define HDA_FIFORDY_POLL 2000 /* x 1 us */

/* ---- small helpers ---------------------------------------------------------- */

static inline uint32_t sd_reg(struct hda_stream *st, uint32_t off)
{
	return st->sd_off + off;
}

static int st_is_bss(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;
	return st->index >= c->num_iss + c->num_oss;
}

/* SDnCTL as a 24-bit value. */
static uint32_t sd_ctl_read(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;

	if (c->flags & HDA_FX_SDCTL_DWORD)
		return hda_read32(c, sd_reg(st, HDA_SD_CTL)) & 0x00ffffffu;
	return (uint32_t)hda_read8(c, sd_reg(st, HDA_SD_CTL)) |
	       ((uint32_t)hda_read8(c, sd_reg(st, HDA_SD_CTL) + 1) << 8) |
	       ((uint32_t)hda_read8(c, sd_reg(st, HDA_SD_CTL2)) << 16);
}

/* Write SDnCTL.  The dword form leaves the status byte zero, which is a
 * no-op for its write-one-to-clear bits.  The byte form writes the tag
 * byte before the control byte so RUN never sees a stale tag. */
static void sd_ctl_write(struct hda_stream *st, uint32_t v)
{
	struct hda_controller *c = st->ctrl;

	v &= 0x00ffffffu;
	if (c->flags & HDA_FX_SDCTL_DWORD) {
		hda_write32(c, sd_reg(st, HDA_SD_CTL), v);
		return;
	}
	hda_write8(c, sd_reg(st, HDA_SD_CTL2), (uint8_t)(v >> 16));
	hda_write8(c, sd_reg(st, HDA_SD_CTL), (uint8_t)v);
}

static uint8_t sd_sts_read(struct hda_stream *st)
{
	return hda_read8(st->ctrl, sd_reg(st, HDA_SD_STS));
}

static void sd_sts_clear(struct hda_stream *st, uint8_t bits)
{
	struct hda_controller *c = st->ctrl;

	bits &= HDA_SD_STS_INT_MASK;
	if (c->flags & HDA_FX_SDCTL_DWORD) {
		/* the status byte is bits 31:24 of the control dword; the
		 * control bits are written back unchanged */
		uint32_t v = hda_read32(c, sd_reg(st, HDA_SD_CTL)) & 0x00ffffffu;
		hda_write32(c, sd_reg(st, HDA_SD_CTL), v | ((uint32_t)bits << 24));
		return;
	}
	hda_write8(c, sd_reg(st, HDA_SD_STS), bits);
}

static uint32_t ssync_reg(struct hda_controller *c)
{
	return (c->flags & HDA_FX_OLD_SSYNC) ? HDA_REG_OLD_SSYNC : HDA_REG_SSYNC;
}

static volatile uint32_t *posbuf_slot(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;

	if (!c->posbuf || st->index < 0 || st->index >= HDA_MAX_STREAMS)
		return NULL;
	return (volatile uint32_t *)&c->posbuf[st->index * HDA_POSBUF_STRIDE_DW];
}

/* ---- layout ----------------------------------------------------------------- */

void hda_streams_init(struct hda_controller *ctrl)
{
	int n = ctrl->num_iss + ctrl->num_oss + ctrl->num_bss;

	if (n > HDA_MAX_STREAMS) {
		/* clip the bidirectional engines first, then outputs */
		int over = n - HDA_MAX_STREAMS;
		int cut = over < ctrl->num_bss ? over : ctrl->num_bss;
		ctrl->num_bss -= cut;
		over -= cut;
		if (over > 0)
			ctrl->num_oss -= over;
		n = HDA_MAX_STREAMS;
	}
	ctrl->nstreams = n;

	for (int i = 0; i < HDA_MAX_STREAMS; i++) {
		struct hda_stream *st = &ctrl->streams[i];
		void *bdl = st->bdl;
		uint64_t bdl_phys = st->bdl_phys;

		kmemset(st, 0, sizeof(*st));
		st->bdl = bdl; /* keep an allocation across re-inits */
		st->bdl_phys = bdl_phys;
		st->ctrl = ctrl;
		st->index = i;
		st->sd_off = HDA_SD_OFF(i);
		if (i < ctrl->num_iss)
			st->direction = AUDIO_DIR_CAPTURE;
		else
			st->direction = AUDIO_DIR_PLAYBACK;
		/* Fixed, globally unique tags while they fit in four bits;
		 * otherwise handed out per direction at assignment. */
		st->tag = (n <= 15 && i < n) ? i + 1 : 0;
	}
}

void hda_streams_fini(struct hda_controller *ctrl)
{
	for (int i = 0; i < HDA_MAX_STREAMS; i++) {
		struct hda_stream *st = &ctrl->streams[i];
		if (st->bdl) {
			snd_dma_free(st->bdl, HDA_BDL_BYTES);
			st->bdl = NULL;
			st->bdl_phys = 0;
		}
	}
}

/* ---- assignment ------------------------------------------------------------- */

static int tag_in_use(struct hda_controller *c, int direction, int tag)
{
	for (int i = 0; i < c->nstreams; i++) {
		struct hda_stream *o = &c->streams[i];
		if (o->in_use && o->direction == direction && o->tag == tag)
			return 1;
	}
	return 0;
}

struct hda_stream *hda_stream_assign(struct hda_controller *ctrl,
				     int direction)
{
	struct hda_stream *found = NULL;
	uint64_t fl;
	int first, last;

	if (!ctrl || (direction != AUDIO_DIR_PLAYBACK &&
		      direction != AUDIO_DIR_CAPTURE))
		return NULL;
	if (hda_ctrl_dead(ctrl))
		return NULL;

	if (direction == AUDIO_DIR_PLAYBACK) {
		first = ctrl->num_iss;
		last = ctrl->num_iss + ctrl->num_oss;
	} else {
		first = 0;
		last = ctrl->num_iss;
	}

	spin_lock_irqsave(&g_hda_stream_lock, &fl);
	for (int i = first; i < last && i < ctrl->nstreams; i++) {
		if (!ctrl->streams[i].in_use) {
			found = &ctrl->streams[i];
			break;
		}
	}
	/* bidirectional engines serve either direction */
	if (!found) {
		for (int i = ctrl->num_iss + ctrl->num_oss; i < ctrl->nstreams; i++) {
			if (!ctrl->streams[i].in_use) {
				found = &ctrl->streams[i];
				break;
			}
		}
	}
	if (found) {
		found->in_use = 1;
		found->direction = direction;
		found->running = 0;
		found->substream = NULL;
		found->codec = NULL;
		found->hdmi = 0;
		found->last_pos = 0;
		found->irq_count = 0;
		if (ctrl->nstreams > 15) {
			int t;
			found->tag = 0;
			for (t = 1; t <= 15; t++)
				if (!tag_in_use(ctrl, direction, t))
					break;
			if (t > 15) {
				found->in_use = 0;
				found = NULL;
			} else {
				found->tag = t;
			}
		}
	}
	spin_unlock_irqrestore(&g_hda_stream_lock, fl);
	return found;
}

void hda_stream_release(struct hda_stream *st)
{
	uint64_t fl;

	if (!st || !st->ctrl)
		return;
	if (st->running)
		hda_stream_stop(st);
	if (!hda_ctrl_dead(st->ctrl))
		hda_stream_reset(st);
	spin_lock_irqsave(&g_hda_stream_lock, &fl);
	st->substream = NULL;
	st->codec = NULL;
	st->hdmi = 0;
	st->running = 0;
	st->format = 0;
	st->buffer_bytes = 0;
	st->period_bytes = 0;
	st->buf_phys = 0;
	st->nbdl = 0;
	if (st->ctrl->nstreams > 15)
		st->tag = 0;
	st->in_use = 0;
	spin_unlock_irqrestore(&g_hda_stream_lock, fl);
}

/* ---- reset ------------------------------------------------------------------ */

void hda_stream_reset(struct hda_stream *st)
{
	struct hda_controller *c;
	uint32_t ctl;
	int i;

	if (!st || !(c = st->ctrl) || hda_ctrl_dead(c))
		return;

	/* stop the DMA and mask the stream's interrupts first */
	ctl = sd_ctl_read(st);
	if (ctl & (HDA_SD_CTL_RUN | HDA_SD_CTL_INT_MASK)) {
		sd_ctl_write(st, ctl & ~(HDA_SD_CTL_RUN | HDA_SD_CTL_INT_MASK));
		for (i = 0; i < HDA_RUN_POLL; i++) {
			if (!(sd_ctl_read(st) & HDA_SD_CTL_RUN))
				break;
			lapic_delay_us(1);
		}
	}
	sd_sts_clear(st, HDA_SD_STS_INT_MASK);
	st->running = 0;

	if (c->flags & HDA_F_NO_STREAM_RESET)
		goto out;

	/* enter reset, wait for the engine to acknowledge, leave it */
	ctl = sd_ctl_read(st);
	sd_ctl_write(st, ctl | HDA_SD_CTL_SRST);
	for (i = 0; i < HDA_SRST_POLL; i++) {
		if (sd_ctl_read(st) & HDA_SD_CTL_SRST)
			break;
		lapic_delay_us(1);
	}
	/* a few more cycles so the engine settles in reset */
	lapic_delay_us(3);
	ctl = sd_ctl_read(st);
	sd_ctl_write(st, ctl & ~HDA_SD_CTL_SRST);
	for (i = 0; i < HDA_SRST_POLL; i++) {
		if (!(sd_ctl_read(st) & HDA_SD_CTL_SRST))
			break;
		lapic_delay_us(1);
	}
	if (i == HDA_SRST_POLL)
		snd_info("hda", "stream %d stuck in reset\n", st->index);

out:;
	volatile uint32_t *pb = posbuf_slot(st);
	if (pb)
		*pb = 0;
	st->last_pos = 0;
}

/* ---- setup ------------------------------------------------------------------ */

int hda_stream_setup(struct hda_stream *st, uint64_t buf_phys,
		     uint32_t buffer_bytes, uint32_t period_bytes,
		     uint16_t format)
{
	struct hda_controller *c;
	uint32_t periods, n = 0;
	uint32_t ctl;

	if (!st || !(c = st->ctrl) || !st->in_use)
		return -EINVAL;
	if (hda_ctrl_dead(c))
		return -ENODEV;
	if (!buffer_bytes || !period_bytes || period_bytes > buffer_bytes ||
	    (buffer_bytes % period_bytes) != 0)
		return -EINVAL;
	periods = buffer_bytes / period_bytes;
	if (periods < 2 || periods > HDA_MAX_BDL)
		return -EINVAL;
	/* The spec wants every buffer segment 128-byte aligned; the Intel
	 * controllers cope with any even length, others do not. */
	if ((period_bytes & 1) || (buf_phys & 127))
		return -EINVAL;
	if ((period_bytes & 127) && !(c->flags & (HDA_F_INTEL_PCH | HDA_F_SKL_PLUS)))
		return -EINVAL;
	if (buf_phys + buffer_bytes < buf_phys)
		return -EINVAL;
	if (!c->addr64 && (buf_phys + buffer_bytes) > 0x100000000ull)
		return -EINVAL;
	if (!hda_format_rate(format))
		return -EINVAL;
	if (st->tag < 1 || st->tag > 15)
		return -EINVAL;

	if (st->running)
		hda_stream_stop(st);
	hda_stream_reset(st);

	if (!st->bdl) {
		uint64_t phys = 0;
		void *v = snd_dma_alloc(HDA_BDL_BYTES, !c->addr64, 0, &phys);
		if (!v)
			return -ENOMEM;
		if (phys & 127) {
			/* the BDL base must be 128-byte aligned */
			snd_dma_free(v, HDA_BDL_BYTES);
			return -ENOMEM;
		}
		st->bdl = (struct hda_bdle *)v;
		st->bdl_phys = phys;
	}

	/* One entry per period, interrupt on the last byte of each period.
	 * Controllers that cannot cross a 4K page inside one entry get the
	 * period split at page boundaries, the interrupt on the last piece. */
	for (uint32_t p = 0; p < periods; p++) {
		uint64_t addr = buf_phys + (uint64_t)p * period_bytes;
		uint32_t left = period_bytes;

		while (left) {
			uint32_t chunk = left;
			if (c->flags & HDA_F_ALIGN_BDLE_4K) {
				uint32_t room = 4096u - (uint32_t)(addr & 4095u);
				if (chunk > room)
					chunk = room;
			}
			if (n >= HDA_MAX_BDL)
				return -EINVAL;
			volatile struct hda_bdle *e = &st->bdl[n];
			e->addr = addr;
			e->len = chunk;
			e->ioc = (chunk == left) ? HDA_BDLE_IOC : 0;
			n++;
			addr += chunk;
			left -= chunk;
		}
	}
	if (n < 2)
		return -EINVAL;
	for (uint32_t i = n; i < HDA_MAX_BDL; i++) {
		volatile struct hda_bdle *e = &st->bdl[i];
		e->addr = 0;
		e->len = 0;
		e->ioc = 0;
	}
	__asm__ volatile("mfence" ::: "memory");

	/* tag and direction */
	ctl = sd_ctl_read(st);
	ctl &= ~(HDA_SD_CTL_STRM_MASK_DW | HDA_SD_CTL_DIR_DW | (3u << 16) |
		 (1u << 18) | HDA_SD_CTL_RUN | HDA_SD_CTL_INT_MASK | HDA_SD_CTL_SRST);
	ctl |= (uint32_t)st->tag << HDA_SD_CTL_STRM_SHIFT_DW;
	if (st_is_bss(st) && st->direction == AUDIO_DIR_PLAYBACK)
		ctl |= HDA_SD_CTL_DIR_DW;
	sd_ctl_write(st, ctl);

	hda_write32(c, sd_reg(st, HDA_SD_CBL), buffer_bytes);
	hda_write16(c, sd_reg(st, HDA_SD_LVI), (uint16_t)(n - 1));
	hda_write16(c, sd_reg(st, HDA_SD_FMT), format);
	hda_write32(c, sd_reg(st, HDA_SD_BDPL), (uint32_t)st->bdl_phys);
	hda_write32(c, sd_reg(st, HDA_SD_BDPU), (uint32_t)(st->bdl_phys >> 32));

	st->buf_phys = buf_phys;
	st->buffer_bytes = buffer_bytes;
	st->period_bytes = period_bytes;
	st->format = format;
	st->nbdl = n;
	st->last_pos = 0;
	st->irq_count = 0;
	volatile uint32_t *pb = posbuf_slot(st);
	if (pb)
		*pb = 0;
	return 0;
}

/* ---- run control ------------------------------------------------------------ */

static void intctl_update(struct hda_controller *c, uint32_t set, uint32_t clr)
{
	uint32_t v = hda_read32(c, HDA_REG_INTCTL);
	if (v == 0xffffffffu)
		return;
	v = (v & ~clr) | set;
	hda_write32(c, HDA_REG_INTCTL, v);
}

/* Arm one engine: clear old status, enable its interrupts, set RUN. */
static void stream_run_locked(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;
	uint32_t ctl;

	sd_sts_clear(st, HDA_SD_STS_INT_MASK);
	intctl_update(c, 1u << st->index, 0);
	ctl = sd_ctl_read(st);
	ctl &= ~HDA_SD_CTL_SRST;
	ctl |= HDA_SD_CTL_RUN | HDA_SD_CTL_INT_MASK;
	sd_ctl_write(st, ctl);
	st->running = 1;
}

static void stream_halt_locked(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;
	uint32_t ctl = sd_ctl_read(st);

	sd_ctl_write(st, ctl & ~(HDA_SD_CTL_RUN | HDA_SD_CTL_INT_MASK));
	intctl_update(c, 0, 1u << st->index);
	st->running = 0;
}

void hda_streams_start_sync(struct hda_controller *ctrl, uint32_t mask)
{
	uint64_t fl;
	uint32_t sreg, ss;
	int i, n;

	if (!ctrl || hda_ctrl_dead(ctrl))
		return;
	mask &= (ctrl->nstreams >= 32) ? 0xffffffffu : ((1u << ctrl->nstreams) - 1);
	if (!mask)
		return;
	sreg = ssync_reg(ctrl);

	spin_lock_irqsave(&g_hda_stream_lock, &fl);
	/* hold the engines at the link while their FIFOs fill */
	ss = hda_read32(ctrl, sreg);
	if (ss != 0xffffffffu)
		hda_write32(ctrl, sreg, ss | mask);
	for (i = 0; i < ctrl->nstreams; i++)
		if (mask & (1u << i))
			stream_run_locked(&ctrl->streams[i]);
	/* wait until every playback FIFO has been primed */
	for (n = 0; n < HDA_FIFORDY_POLL; n++) {
		int waiting = 0;
		for (i = 0; i < ctrl->nstreams; i++) {
			struct hda_stream *st = &ctrl->streams[i];
			if (!(mask & (1u << i)) || st->direction != AUDIO_DIR_PLAYBACK)
				continue;
			uint8_t s = sd_sts_read(st);
			if (s != 0xff && !(s & HDA_SD_STS_FIFORDY))
				waiting++;
		}
		if (!waiting)
			break;
		lapic_delay_us(1);
	}
	/* release them together */
	ss = hda_read32(ctrl, sreg);
	if (ss != 0xffffffffu)
		hda_write32(ctrl, sreg, ss & ~mask);
	spin_unlock_irqrestore(&g_hda_stream_lock, fl);

	for (i = 0; i < ctrl->nstreams; i++)
		if (mask & (1u << i))
			hda_ctrl_stream_event(&ctrl->streams[i], 1);
}

void hda_streams_stop_sync(struct hda_controller *ctrl, uint32_t mask)
{
	uint64_t fl;
	int i, n;

	if (!ctrl)
		return;
	mask &= (ctrl->nstreams >= 32) ? 0xffffffffu : ((1u << ctrl->nstreams) - 1);
	if (!mask)
		return;
	if (hda_ctrl_dead(ctrl)) {
		for (i = 0; i < ctrl->nstreams; i++)
			if (mask & (1u << i)) {
				ctrl->streams[i].running = 0;
				hda_ctrl_stream_event(&ctrl->streams[i], 0);
			}
		return;
	}

	spin_lock_irqsave(&g_hda_stream_lock, &fl);
	for (i = 0; i < ctrl->nstreams; i++)
		if (mask & (1u << i))
			stream_halt_locked(&ctrl->streams[i]);
	for (n = 0; n < HDA_RUN_POLL; n++) {
		int waiting = 0;
		for (i = 0; i < ctrl->nstreams; i++) {
			if (!(mask & (1u << i)))
				continue;
			uint32_t ctl = sd_ctl_read(&ctrl->streams[i]);
			if (ctl != 0x00ffffffu && (ctl & HDA_SD_CTL_RUN))
				waiting++;
		}
		if (!waiting)
			break;
		lapic_delay_us(1);
	}
	for (i = 0; i < ctrl->nstreams; i++)
		if (mask & (1u << i))
			sd_sts_clear(&ctrl->streams[i], HDA_SD_STS_INT_MASK);
	spin_unlock_irqrestore(&g_hda_stream_lock, fl);

	for (i = 0; i < ctrl->nstreams; i++)
		if (mask & (1u << i))
			hda_ctrl_stream_event(&ctrl->streams[i], 0);
}

void hda_stream_start(struct hda_stream *st)
{
	if (!st || !st->ctrl || !st->in_use || !st->nbdl)
		return;
	hda_streams_start_sync(st->ctrl, 1u << st->index);
}

void hda_stream_stop(struct hda_stream *st)
{
	if (!st || !st->ctrl)
		return;
	hda_streams_stop_sync(st->ctrl, 1u << st->index);
}

void hda_streams_stop_all(struct hda_controller *ctrl)
{
	uint64_t fl;

	if (!ctrl || ctrl->nstreams <= 0)
		return;
	if (hda_ctrl_dead(ctrl)) {
		for (int i = 0; i < ctrl->nstreams; i++)
			ctrl->streams[i].running = 0;
		return;
	}
	hda_streams_stop_sync(ctrl, (ctrl->nstreams >= 32) ? 0xffffffffu :
				    ((1u << ctrl->nstreams) - 1));
	spin_lock_irqsave(&g_hda_stream_lock, &fl);
	intctl_update(ctrl, 0, HDA_INT_SIE_MASK);
	spin_unlock_irqrestore(&g_hda_stream_lock, fl);
}

/* ---- position --------------------------------------------------------------- */

static int use_lpib(struct hda_stream *st)
{
	struct hda_controller *c = st->ctrl;

	if (!c->posbuf || (c->flags & HDA_F_POSFIX_LPIB))
		return 1;
	if (c->flags & HDA_F_POSFIX_POSBUF)
		return 0;
	if ((c->flags & HDA_F_POSFIX_COMBO) && st->direction == AUDIO_DIR_PLAYBACK)
		return 1;
	return 0;
}

static uint32_t raw_position(struct hda_stream *st, int *valid)
{
	struct hda_controller *c = st->ctrl;
	uint32_t pos;

	*valid = 1;
	if (use_lpib(st)) {
		pos = hda_read32(c, sd_reg(st, HDA_SD_LPIB));
		if (pos == 0xffffffffu) {
			*valid = 0;
			return 0;
		}
	} else {
		volatile uint32_t *pb = posbuf_slot(st);
		pos = pb ? *pb : 0;
	}
	/* LPIB counts up to CBL; some parts report CBL itself at the wrap,
	 * a stale position buffer can hold anything */
	if (pos >= st->buffer_bytes)
		pos = (pos == st->buffer_bytes) ? 0 : pos % st->buffer_bytes;
	return pos;
}

uint32_t hda_stream_position(struct hda_stream *st)
{
	uint32_t pos, back, jitter;
	int valid;

	if (!st || !st->ctrl || !st->buffer_bytes)
		return 0;
	if (hda_ctrl_dead(st->ctrl))
		return st->last_pos < st->buffer_bytes ? st->last_pos : 0;

	pos = raw_position(st, &valid);
	if (!valid)
		return st->last_pos < st->buffer_bytes ? st->last_pos : 0;

	/* Never step backwards within a lap: a position slightly behind the
	 * last one reported is read jitter (or a lag already corrected for
	 * at the interrupt).  A large backward distance is a forward wrap. */
	jitter = st->period_bytes / 4;
	if (jitter > 4096)
		jitter = 4096;
	back = (st->last_pos + st->buffer_bytes - pos) % st->buffer_bytes;
	if (back != 0 && back < jitter)
		pos = st->last_pos;

	st->last_pos = pos;
	return pos;
}

uint32_t hda_stream_fifo_bytes(struct hda_stream *st)
{
	uint16_t v;

	if (!st || !st->ctrl || hda_ctrl_dead(st->ctrl))
		return 0;
	v = hda_read16(st->ctrl, sd_reg(st, HDA_SD_FIFOS));
	if (v == 0xffff)
		return 0;
	return (uint32_t)v + 1;
}

uint32_t hda_stream_link_delay(struct hda_stream *st)
{
	struct hda_controller *c;
	volatile uint32_t *pb;
	uint32_t lpib, pbpos, d;

	if (!st || !(c = st->ctrl) || !st->buffer_bytes || hda_ctrl_dead(c))
		return 0;
	/* only where both counters are trustworthy: the position buffer is
	 * the DMA side, LPIB the link side */
	if (!(c->flags & HDA_F_INTEL_PCH) || use_lpib(st))
		return 0;
	pb = posbuf_slot(st);
	if (!pb)
		return 0;
	lpib = hda_read32(c, sd_reg(st, HDA_SD_LPIB));
	if (lpib == 0xffffffffu || lpib >= st->buffer_bytes)
		return 0;
	pbpos = *pb;
	if (pbpos >= st->buffer_bytes)
		return 0;
	if (st->direction == AUDIO_DIR_PLAYBACK)
		d = (pbpos + st->buffer_bytes - lpib) % st->buffer_bytes;
	else
		d = (lpib + st->buffer_bytes - pbpos) % st->buffer_bytes;
	if (d >= st->period_bytes)
		return 0;
	return d;
}

/* ---- interrupt -------------------------------------------------------------- */

static uint32_t g_hda_desc_err_reports;

void hda_stream_irq(struct hda_stream *st)
{
	struct hda_controller *c;
	uint8_t sts;

	if (!st || !(c = st->ctrl))
		return;
	sts = sd_sts_read(st);
	if (sts == 0xff)
		return; /* device gone: the controller code notices */
	sts &= HDA_SD_STS_INT_MASK;
	if (!sts)
		return;
	sd_sts_clear(st, sts);

	if (sts & HDA_SD_STS_DESE) {
		/* a descriptor error stops the engine; the PCM core sees the
		 * position freeze and reports an xrun */
		if (g_hda_desc_err_reports < 8) {
			g_hda_desc_err_reports++;
			snd_info("hda", "stream %d: descriptor error (bdl %llx)\n",
				 st->index, (unsigned long long)st->bdl_phys);
		}
		st->running = 0;
	}

	if (!(sts & HDA_SD_STS_BCIS) || !st->running)
		return;
	st->irq_count++;

	/* Automatic position fix: the position buffer has never been
	 * written while LPIB moves -> this controller does not maintain it. */
	if (st->irq_count <= 4 && c->posbuf &&
	    !(c->flags & (HDA_F_POSFIX_LPIB | HDA_F_POSFIX_POSBUF | HDA_F_POSFIX_COMBO))) {
		volatile uint32_t *pb = posbuf_slot(st);
		uint32_t lpib = hda_read32(c, sd_reg(st, HDA_SD_LPIB));
		if (pb && *pb == 0 && lpib != 0 && lpib != 0xffffffffu) {
			c->flags |= HDA_F_POSFIX_LPIB;
			snd_info("hda", "position buffer not updated; using LPIB\n");
		}
	}

	/* A playback position just short of a period boundary at the
	 * boundary's interrupt lags the hardware: report the boundary. */
	if (st->direction == AUDIO_DIR_PLAYBACK && st->period_bytes &&
	    st->buffer_bytes) {
		int valid;
		uint32_t pos = raw_position(st, &valid);
		if (valid) {
			uint32_t off = pos % st->period_bytes;
			uint32_t lag = st->period_bytes / 4;
			if (lag > 4096)
				lag = 4096;
			if (off && st->period_bytes - off <= lag) {
				uint32_t snapped = (pos - off + st->period_bytes) %
						   st->buffer_bytes;
				st->last_pos = snapped;
			}
		}
	}

	if (st->substream)
		snd_pcm_period_elapsed(st->substream);
}

/* ---- stream format ---------------------------------------------------------- */

/* The rate is base x mult / div with base 48 kHz or 44.1 kHz, multiplier
 * 1-4 (or 8 on the controllers and codecs that do 384 kHz) and divisor
 * 1-8.  The search prefers the smallest multiplier and divisor and the
 * 48 kHz family. */
static const uint8_t hda_mults[] = { 1, 2, 3, 4, 8 };

uint16_t hda_calc_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
	uint16_t fmt = 0;
	uint32_t bitsf;
	int found = 0;

	if (rate < 6000 || rate > 384000)
		return 0;
	if (channels < 1 || channels > 16)
		return 0;
	switch (bits) {
	case 8:
		bitsf = AC_FMT_BITS_8;
		break;
	case 16:
		bitsf = AC_FMT_BITS_16;
		break;
	case 20:
		bitsf = AC_FMT_BITS_20;
		break;
	case 24:
		bitsf = AC_FMT_BITS_24;
		break;
	case 32:
		bitsf = AC_FMT_BITS_32;
		break;
	default:
		return 0;
	}

	for (unsigned m = 0; m < sizeof(hda_mults) && !found; m++) {
		uint32_t mult = hda_mults[m];
		for (uint32_t div = 1; div <= 8 && !found; div++) {
			for (int b = 0; b < 2 && !found; b++) {
				uint32_t base = b ? 44100 : 48000;
				uint32_t num = base * mult;
				if (num % div || num / div != rate)
					continue;
				fmt = (uint16_t)((b ? AC_FMT_BASE_44K : AC_FMT_BASE_48K) |
						 ((mult - 1) << AC_FMT_MULT_SHIFT) |
						 ((div - 1) << AC_FMT_DIV_SHIFT));
				found = 1;
			}
		}
	}
	if (!found)
		return 0;
	fmt |= (uint16_t)(bitsf | (channels - 1));
	return fmt;
}

uint32_t hda_format_rate(uint16_t fmt)
{
	uint32_t base = (fmt & AC_FMT_BASE_44K) ? 44100 : 48000;
	uint32_t mult = ((fmt & AC_FMT_MULT_MASK) >> AC_FMT_MULT_SHIFT) + 1;
	uint32_t div = ((fmt & AC_FMT_DIV_MASK) >> AC_FMT_DIV_SHIFT) + 1;

	if (fmt & AC_FMT_TYPE_NON_PCM)
		return 0;
	if (mult > 4 && mult != 8)
		return 0;
	return base * mult / div;
}

uint32_t hda_format_channels(uint16_t fmt)
{
	return (uint32_t)(fmt & AC_FMT_CHAN_MASK) + 1;
}

uint32_t hda_format_bits(uint16_t fmt)
{
	switch (fmt & AC_FMT_BITS_MASK) {
	case AC_FMT_BITS_8:
		return 8;
	case AC_FMT_BITS_16:
		return 16;
	case AC_FMT_BITS_20:
		return 20;
	case AC_FMT_BITS_24:
		return 24;
	case AC_FMT_BITS_32:
		return 32;
	default:
		return 0;
	}
}
