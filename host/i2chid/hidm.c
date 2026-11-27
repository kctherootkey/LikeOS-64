/* An I2C-HID device on the DesignWare bus model, for the I2C-HID harness
 * (host/test-i2chid.sh).  Bus world: compiled with the kshim prelude.
 *
 * The model speaks the HID-over-I2C protocol as a touchpad does:
 *
 *   registers   HID descriptor (30 bytes), report descriptor, input,
 *               output, command, data -- 16-bit, little-endian, written
 *               first in a transaction; a read without a register (the
 *               interrupt path's plain read) returns the input register;
 *   commands    RESET (clears the queue and the configuration, then the
 *               "reset done" zero-length input report with the line
 *               asserted), SET_POWER ON/SLEEP, GET_REPORT (answer from the
 *               data register in the same transaction, after a repeated
 *               START), SET_REPORT (length-prefixed data to the data
 *               register) -- with the 4-bit report ID field, or 0xF and
 *               the ID in a third byte for IDs of 15 and up; every
 *               malformed command is counted as a protocol error;
 *   input       a queue of reports; the line (level, active low -- here
 *               "asserted") is asserted while a report or a reset
 *               acknowledgement is waiting and the device is ON; a read of
 *               the input register returns [length lo, hi, report...]
 *               padded with zeros to the length the host reads, and the
 *               report leaves the queue at the STOP of a read of at least
 *               the two length bytes;
 *   faults      see kworld.h (HIDM_*).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include "host/i2c/model.h"
#include "host/i2c/mocks.h"
#include "host/i2chid/kworld.h"
#include "host/i2chid/kworld_int.h"

#define HIDM_Q 128
#define HIDM_RMAX 600 /* one framed report */
#define HIDM_W 1200 /* bytes written in one transaction */
#define HIDM_RBUF 4200 /* one read answer */

#define OP_RESET 0x01
#define OP_GET_REPORT 0x02
#define OP_SET_REPORT 0x03
#define OP_SET_POWER 0x08

enum { RD_NONE, RD_INPUT, RD_REG, RD_GET };
enum { SRC_NONE, SRC_ACK, SRC_QUEUE, SRC_BOGUS, SRC_GARBAGE, SRC_IDLE };

struct hidm_report {
	uint8_t b[HIDM_RMAX];
	int len;
};

struct hidm {
	spinlock_t lock;
	struct dwm_slave *slv;
	/* registers */
	uint16_t desc_reg, rdesc_reg, input_reg, out_reg, cmd_reg, data_reg;
	uint8_t hdesc[30];
	uint8_t *rdesc;
	int rdesc_len;
	uint16_t max_input;
	/* the interrupt line */
	int irq, gsi;
	int line;
	/* state */
	int power; /* 0 ON, 1 SLEEP */
	int woke; /* the deep-sleep NACK was given */
	int reset_ack; /* an acknowledgement waits */
	uint64_t reset_ack_due_ns;
	struct hidm_report q[HIDM_Q];
	int qh, qn;
	/* the transaction */
	uint8_t w[HIDM_W];
	int wn;
	int in_read;
	int rd_kind, rd_src;
	uint8_t rbuf[HIDM_RBUF];
	int rlen, rpos;
	/* features: answers to GET_REPORT, data of SET_REPORT */
	uint8_t get_ans[256][300];
	int get_len[256];
	uint8_t set_data[256][300];
	int set_len[256];
	uint8_t out_data[300];
	int out_len, out_via_reg;
	/* counters */
	int c[32];
	/* faults */
	int f[16];
	unsigned int rng;
};

static struct hidm g_hidm;

static unsigned int hidm_rand(struct hidm *h)
{
	h->rng = h->rng * 1103515245u + 12345u;
	return h->rng >> 8;
}

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* The level the line should have.  Lock held. */
static void hidm_update_line(struct hidm *h)
{
	uint64_t now = h_now_ns();
	int level;

	if (h->reset_ack && h->reset_ack_due_ns && now < h->reset_ack_due_ns)
		level = h->qn > 0 && h->power == 0;
	else
		level = h->reset_ack ||
			((h->power == 0 || h->f[HIDM_ASLEEP_REPORTS]) &&
			 (h->qn > 0 || h->f[HIDM_BOGUS] > 0 ||
			  h->f[HIDM_GARBAGE] > 0));
	/* A device that stopped answering (HIDM_GONE) keeps its line where
	 * it was: a hung controller holding its interrupt asserted. */
	h->line = level;
	if (h->irq > 0)
		mock_irq_set_ext(h->irq, level);
	else if (h->gsi >= 0)
		mock_gsi_set_level(h->gsi, level);
}

/* Fill rbuf with what a plain read of the input register returns. */
static void hidm_serve_input(struct hidm *h)
{
	int n = h->max_input > 2 ? h->max_input : 2;

	if (n > HIDM_RBUF)
		n = HIDM_RBUF;
	kmemset(h->rbuf, 0, (size_t)n);
	h->rlen = n;
	h->rd_kind = RD_INPUT;
	if (h->reset_ack &&
	    (!h->reset_ack_due_ns || h_now_ns() >= h->reset_ack_due_ns)) {
		h->rd_src = SRC_ACK; /* length 0: reset done */
		return;
	}
	if (h->f[HIDM_BOGUS] > 0) {
		h->rd_src = SRC_BOGUS;
		h->rbuf[0] = 0xff;
		h->rbuf[1] = 0xff;
		return;
	}
	if (h->f[HIDM_GARBAGE] > 0) {
		h->rd_src = SRC_GARBAGE;
		for (int i = 0; i < n; i++)
			h->rbuf[i] = (uint8_t)hidm_rand(h);
		return;
	}
	if (h->qn > 0) {
		struct hidm_report *r = &h->q[h->qh];
		int len = r->len + 2;

		h->rd_src = SRC_QUEUE;
		put16(h->rbuf, (unsigned int)len);
		if (h->f[HIDM_BAD_LEN] > 0)
			put16(h->rbuf, (unsigned int)(h->max_input + 7));
		else if (h->f[HIDM_SHORT_LEN] > 0)
			put16(h->rbuf, 1);
		for (int i = 0; i < r->len && i + 2 < n; i++)
			h->rbuf[i + 2] = r->b[i];
		return;
	}
	/* Nothing waiting: an empty report. */
	h->rd_src = SRC_IDLE;
	put16(h->rbuf, 2);
}

/* A read that follows a register write in the same transaction. */
static void hidm_serve_register(struct hidm *h)
{
	uint16_t reg = (uint16_t)(h->w[0] | h->w[1] << 8);

	h->rlen = 0;
	h->rd_kind = RD_REG;
	h->rd_src = SRC_NONE;
	if (h->wn == 2) {
		if (reg == h->desc_reg) {
			kmemcpy(h->rbuf, h->hdesc, 30);
			h->rlen = 30;
			h->c[HIDM_DESC_READS]++;
		} else if (reg == h->rdesc_reg && h->rdesc) {
			int n = h->rdesc_len < HIDM_RBUF ? h->rdesc_len :
							   HIDM_RBUF;

			kmemcpy(h->rbuf, h->rdesc, (size_t)n);
			h->rlen = n;
			h->c[HIDM_RDESC_READS]++;
		} else if (reg == h->input_reg) {
			hidm_serve_input(h);
		}
		return;
	}
	if (reg == h->cmd_reg && h->wn >= 4) {
		int type = h->w[2] >> 4, id = h->w[2] & 0xf, off = 4;
		int op = h->w[3];
		int len;

		if (id == 0xf) {
			if (h->wn < 5) {
				h->c[HIDM_PROTO_ERRORS]++;
				return;
			}
			id = h->w[4];
			off = 5;
			h->c[HIDM_LONG_ID_CMDS]++;
		}
		if (op != OP_GET_REPORT || h->wn != off + 2 ||
		    (uint16_t)(h->w[off] | h->w[off + 1] << 8) != h->data_reg ||
		    (type != 1 && type != 3)) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		h->c[HIDM_GET_REPORTS]++;
		h->rd_kind = RD_GET;
		len = h->get_len[id];
		put16(h->rbuf, (unsigned int)(len + 2));
		if (len)
			kmemcpy(h->rbuf + 2, h->get_ans[id], (size_t)len);
		h->rlen = len + 2;
		return;
	}
	h->c[HIDM_PROTO_ERRORS]++;
}

static void hidm_reset(struct hidm *h)
{
	h->c[HIDM_RESETS]++;
	h->power = 0;
	h->qh = h->qn = 0;
	/* The configuration is gone (input mode back to mouse mode). */
	for (int i = 0; i < 256; i++)
		h->set_len[i] = 0;
	if (!h->f[HIDM_NO_RESET_ACK]) {
		h->reset_ack = 1;
		h->reset_ack_due_ns =
			h->f[HIDM_LATE_RESET_ACK_MS] ?
				h_now_ns() + (uint64_t)h->f[HIDM_LATE_RESET_ACK_MS] *
						     1000000ull :
				0;
	}
}

/* A write-only transaction ended: a command or an output report. */
static void hidm_command(struct hidm *h)
{
	uint16_t reg;
	int type, id, op, off = 4;

	if (h->wn < 2)
		return;
	reg = (uint16_t)(h->w[0] | h->w[1] << 8);
	if (reg == h->out_reg && h->out_reg) {
		int len = h->wn >= 4 ? (h->w[2] | h->w[3] << 8) : -1;

		if (len != h->wn - 2 || len < 2) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		h->out_len = len - 2 < 300 ? len - 2 : 300;
		kmemcpy(h->out_data, h->w + 4, (size_t)h->out_len);
		h->out_via_reg = 1;
		h->c[HIDM_OUTPUT_REPORTS]++;
		return;
	}
	if (reg != h->cmd_reg || h->wn < 4) {
		h->c[HIDM_PROTO_ERRORS]++;
		return;
	}
	type = h->w[2] >> 4;
	id = h->w[2] & 0xf;
	op = h->w[3];
	if (id == 0xf && op != OP_SET_POWER && op != OP_RESET) {
		if (h->wn < 5) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		id = h->w[4];
		off = 5;
		h->c[HIDM_LONG_ID_CMDS]++;
	}
	switch (op) {
	case OP_RESET:
		if (h->wn != 4) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		hidm_reset(h);
		break;
	case OP_SET_POWER:
		if (h->wn != 4 || type != 0 || id > 1) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		h->power = id;
		if (id) {
			h->c[HIDM_SET_POWER_SLEEP]++;
			h->woke = 0;
		} else {
			h->c[HIDM_SET_POWER_ON]++;
		}
		break;
	case OP_SET_REPORT: {
		int dlen;

		if (h->wn < off + 4 ||
		    (uint16_t)(h->w[off] | h->w[off + 1] << 8) != h->data_reg ||
		    (type != 2 && type != 3)) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		dlen = (h->w[off + 2] | h->w[off + 3] << 8) - 2;
		if (dlen != h->wn - (off + 4) || dlen < 0) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		/* Numbered reports carry their ID after the length. */
		if (id && (dlen < 1 || h->w[off + 4] != id)) {
			h->c[HIDM_PROTO_ERRORS]++;
			return;
		}
		if (type == 2) {
			h->out_len = dlen < 300 ? dlen : 300;
			kmemcpy(h->out_data, h->w + off + 4,
				(size_t)h->out_len);
			h->out_via_reg = 0;
			h->c[HIDM_OUTPUT_REPORTS]++;
		} else {
			h->set_len[id] = dlen < 300 ? dlen : 300;
			kmemcpy(h->set_data[id], h->w + off + 4,
				(size_t)h->set_len[id]);
		}
		h->c[HIDM_SET_REPORTS]++;
		h->c[HIDM_LAST_SET_ID] = id;
		break;
	}
	default:
		h->c[HIDM_PROTO_ERRORS]++;
		break;
	}
}

/* ---- bus callbacks (the bus model's lock held) ---- */

static int hidm_cb_start(struct dwm_slave *s, int read, int restart)
{
	struct hidm *h = s->cb_priv;
	uint64_t fl;
	int ack = 1;

	spin_lock_irqsave(&h->lock, &fl);
	if (h->f[HIDM_GONE]) {
		ack = 0;
	} else if (h->power == 1 && h->f[HIDM_NACK_WAKE] && !h->woke &&
		   !restart) {
		/* Deep sleep: this address byte wakes it, unacknowledged. */
		h->woke = 1;
		ack = 0;
	} else if (read && !restart && h->f[HIDM_NACK_READS] > 0) {
		h->f[HIDM_NACK_READS]--;
		ack = 0;
	}
	if (!ack) {
		h->c[HIDM_NACKS]++;
		spin_unlock_irqrestore(&h->lock, fl);
		return 0;
	}
	if (!read) {
		if (!restart)
			h->wn = 0;
		h->in_read = 0;
	} else {
		h->in_read = 1;
		h->rpos = 0;
		if (h->wn == 0 && !restart) {
			hidm_serve_input(h);
			h->c[HIDM_INPUT_READS]++;
		} else {
			hidm_serve_register(h);
		}
	}
	spin_unlock_irqrestore(&h->lock, fl);
	return 1;
}

static int hidm_cb_write(struct dwm_slave *s, uint8_t b)
{
	struct hidm *h = s->cb_priv;
	uint64_t fl;

	spin_lock_irqsave(&h->lock, &fl);
	if (h->wn < HIDM_W)
		h->w[h->wn++] = b;
	else
		h->c[HIDM_PROTO_ERRORS]++;
	spin_unlock_irqrestore(&h->lock, fl);
	return 1;
}

static uint8_t hidm_cb_read(struct dwm_slave *s)
{
	struct hidm *h = s->cb_priv;
	uint64_t fl;
	uint8_t b = 0;

	spin_lock_irqsave(&h->lock, &fl);
	if (h->rpos < h->rlen)
		b = h->rbuf[h->rpos];
	h->rpos++;
	spin_unlock_irqrestore(&h->lock, fl);
	if (h->f[HIDM_STRETCH_US])
		h_usleep((unsigned long)h->f[HIDM_STRETCH_US]);
	return b;
}

static void hidm_cb_stop(struct dwm_slave *s)
{
	struct hidm *h = s->cb_priv;
	uint64_t fl;

	spin_lock_irqsave(&h->lock, &fl);
	if (!h->in_read) {
		hidm_command(h);
	} else if (h->rd_kind == RD_INPUT && h->rpos >= 2) {
		switch (h->rd_src) {
		case SRC_ACK:
			h->reset_ack = 0;
			h->reset_ack_due_ns = 0;
			h->c[HIDM_RESET_ACKS_SERVED]++;
			break;
		case SRC_QUEUE:
			if (h->f[HIDM_BAD_LEN] > 0)
				h->f[HIDM_BAD_LEN]--;
			else if (h->f[HIDM_SHORT_LEN] > 0)
				h->f[HIDM_SHORT_LEN]--;
			h->qh = (h->qh + 1) % HIDM_Q;
			h->qn--;
			break;
		case SRC_BOGUS:
			h->f[HIDM_BOGUS]--;
			break;
		case SRC_GARBAGE:
			h->f[HIDM_GARBAGE]--;
			break;
		}
	}
	h->wn = 0;
	h->in_read = 0;
	h->rd_kind = RD_NONE;
	hidm_update_line(h);
	spin_unlock_irqrestore(&h->lock, fl);
}

/* ---- the fixtures' side (kworld.c) ---- */

void hidm_attach(struct dw_model *m, uint16_t addr, const struct kw_cfg *cfg,
		 uint16_t desc_reg, int irq, int gsi)
{
	struct hidm *h = &g_hidm;
	unsigned int maxin = 0;

	kfree(h->rdesc);
	kmemset(h, 0, sizeof(*h));
	spinlock_init(&h->lock, "hidm");
	h->rng = 1;
	h->desc_reg = desc_reg;
	if (cfg->machine == KW_SIPODEV) {
		/* The layout the DMI override describes for that device. */
		h->rdesc_reg = 0x21;
		h->input_reg = 0x24;
		h->out_reg = 0x25;
		h->cmd_reg = 0x22;
		h->data_reg = 0x23;
	} else {
		h->rdesc_reg = (uint16_t)(desc_reg + 1);
		h->input_reg = (uint16_t)(desc_reg + 2);
		h->out_reg = (uint16_t)(desc_reg + 3);
		h->cmd_reg = (uint16_t)(desc_reg + 4);
		h->data_reg = (uint16_t)(desc_reg + 5);
	}
	h->irq = irq;
	h->gsi = gsi;
	h->power = 1; /* asleep until told otherwise */
	h->woke = 1;
	if (cfg->rdesc && cfg->rdesc_len > 0) {
		h->rdesc = kcalloc(1, (size_t)cfg->rdesc_len);
		T_ASSERT(h->rdesc);
		kmemcpy(h->rdesc, cfg->rdesc, (size_t)cfg->rdesc_len);
		h->rdesc_len = cfg->rdesc_len;
	}
	maxin = cfg->max_input ? cfg->max_input : 64;
	h->max_input = (uint16_t)maxin;
	put16(&h->hdesc[0], cfg->desc_len ? cfg->desc_len : 30);
	put16(&h->hdesc[2], cfg->bcd ? cfg->bcd : 0x0100);
	put16(&h->hdesc[4], (unsigned int)h->rdesc_len);
	put16(&h->hdesc[6], h->rdesc_reg);
	put16(&h->hdesc[8], h->input_reg);
	put16(&h->hdesc[10], maxin);
	put16(&h->hdesc[12], h->out_reg);
	put16(&h->hdesc[14], 64);
	put16(&h->hdesc[16], h->cmd_reg);
	put16(&h->hdesc[18], h->data_reg);
	put16(&h->hdesc[20], cfg->vendor);
	put16(&h->hdesc[22], cfg->product);
	put16(&h->hdesc[24], 0x0001);
	h->slv = dwm_slave_new(SLV_CB, addr, 0);
	h->slv->cb_start = hidm_cb_start;
	h->slv->cb_write = hidm_cb_write;
	h->slv->cb_read = hidm_cb_read;
	h->slv->cb_stop = hidm_cb_stop;
	h->slv->cb_priv = h;
	h->slv->present = !cfg->absent;
	dwm_add_slave(m, h->slv);
}

void hidm_set_irq(int irq)
{
	uint64_t fl;

	spin_lock_irqsave(&g_hidm.lock, &fl);
	g_hidm.irq = irq;
	hidm_update_line(&g_hidm);
	spin_unlock_irqrestore(&g_hidm.lock, fl);
}

void hidm_detach(void)
{
	struct hidm *h = &g_hidm;

	if (h->irq > 0)
		mock_irq_set_ext(h->irq, 0);
	if (h->gsi >= 0)
		mock_gsi_set_level(h->gsi, 0);
	kfree(h->rdesc);
	h->rdesc = NULL;
	/* The slave belongs to the bus model, freed with it. */
	h->slv = NULL;
}

/* A periodic nudge: late acknowledgements become due without bus
 * traffic. */
void hidm_tick(void)
{
	uint64_t fl;

	if (!g_hidm.slv)
		return;
	spin_lock_irqsave(&g_hidm.lock, &fl);
	hidm_update_line(&g_hidm);
	spin_unlock_irqrestore(&g_hidm.lock, fl);
}

/* ---- the scenarios' side (through kworld.h) ---- */

void hidm_set_feature(int id, const uint8_t *data, int len)
{
	uint64_t fl;

	kw_adopt();
	T_ASSERT(id >= 0 && id < 256 && len >= 0 && len <= 300);
	spin_lock_irqsave(&g_hidm.lock, &fl);
	kmemcpy(g_hidm.get_ans[id], data, (size_t)len);
	g_hidm.get_len[id] = len;
	spin_unlock_irqrestore(&g_hidm.lock, fl);
}

int hidm_feature(int id, uint8_t *out, int max)
{
	uint64_t fl;
	int n;

	kw_adopt();
	if (id < 0 || id > 255)
		return 0;
	spin_lock_irqsave(&g_hidm.lock, &fl);
	n = g_hidm.set_len[id] < max ? g_hidm.set_len[id] : max;
	kmemcpy(out, g_hidm.set_data[id], (size_t)n);
	spin_unlock_irqrestore(&g_hidm.lock, fl);
	return n;
}

int hidm_output(uint8_t *out, int max, int *via_output_register)
{
	uint64_t fl;
	int n;

	kw_adopt();
	spin_lock_irqsave(&g_hidm.lock, &fl);
	n = g_hidm.out_len < max ? g_hidm.out_len : max;
	kmemcpy(out, g_hidm.out_data, (size_t)n);
	if (via_output_register)
		*via_output_register = g_hidm.out_via_reg;
	spin_unlock_irqrestore(&g_hidm.lock, fl);
	return n;
}

void hidm_push_report(const uint8_t *r, int len)
{
	uint64_t fl;

	kw_adopt();
	T_ASSERT(len > 0 && len <= HIDM_RMAX);
	spin_lock_irqsave(&g_hidm.lock, &fl);
	if (g_hidm.qn < HIDM_Q) {
		struct hidm_report *q =
			&g_hidm.q[(g_hidm.qh + g_hidm.qn) % HIDM_Q];

		kmemcpy(q->b, r, (size_t)len);
		q->len = len;
		g_hidm.qn++;
	}
	hidm_update_line(&g_hidm);
	spin_unlock_irqrestore(&g_hidm.lock, fl);
}

void hidm_set(int fault, int value)
{
	uint64_t fl;

	kw_adopt();
	T_ASSERT(fault > 0 && fault < 16);
	spin_lock_irqsave(&g_hidm.lock, &fl);
	g_hidm.f[fault] = value;
	if (fault == HIDM_SPONTANEOUS_RESET && value > 0) {
		/* The device reset on its own: acknowledgements nobody asked
		 * for. */
		g_hidm.f[fault] = 0;
		g_hidm.reset_ack = 1;
		g_hidm.reset_ack_due_ns = 0;
		g_hidm.qh = g_hidm.qn = 0;
		for (int i = 0; i < 256; i++)
			g_hidm.set_len[i] = 0;
	}
	hidm_update_line(&g_hidm);
	spin_unlock_irqrestore(&g_hidm.lock, fl);
}

int hidm_get(int what)
{
	uint64_t fl;
	int v;

	kw_adopt();
	spin_lock_irqsave(&g_hidm.lock, &fl);
	switch (what) {
	case HIDM_POWER:
		v = g_hidm.power;
		break;
	case HIDM_QUEUED:
		v = g_hidm.qn;
		break;
	case HIDM_LINE:
		v = g_hidm.line;
		break;
	default:
		v = what > 0 && what < 32 ? g_hidm.c[what] : -1;
		break;
	}
	spin_unlock_irqrestore(&g_hidm.lock, fl);
	return v;
}

int hidm_wait_drained(int ms)
{
	uint64_t end = h_now_ns() + (uint64_t)ms * 1000000ull;

	kw_adopt();
	while (h_now_ns() < end) {
		if (!__atomic_load_n(&g_hidm.qn, __ATOMIC_ACQUIRE) &&
		    !__atomic_load_n(&g_hidm.reset_ack, __ATOMIC_ACQUIRE))
			return 1;
		h_usleep(200);
	}
	return 0;
}

void hidm_seed(unsigned int seed)
{
	kw_adopt();
	g_hidm.rng = seed ? seed : 1;
}
