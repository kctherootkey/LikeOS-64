// LikeOS SoundWire bus core.
//
// Protocol logic on top of the Intel/Cadence master (sdw_intel.c):
//
//   - peripheral register access through the command FIFO, with the SCP
//     address paging that registers above 0x8000 need, split into batches
//     the FIFO takes, plus the 32-bit (Cirrus) and multi-byte-quantity
//     (SDCA) conventions on top;
//   - enumeration: unnumbered devices answer as device 0; we read their
//     48-bit id (DEVID_0..5), match it against the firmware description and
//     give each a device number (SCP_DEVNUMBER), then set its interrupt
//     masks and bus clock registers;
//   - peripheral status changes and alerts (bus clash, parity, data port
//     and implementation-defined/SDCA interrupts handed to the codec);
//   - streams: per link and direction one stream feeding one master data
//     port (PDI) and the codecs' data ports.  The frame shape follows the
//     bus clock and frame rate; every rate group gets the columns its
//     payload needs, packed from the right; transport and port parameters
//     go into the inactive register bank and a broadcast write of the
//     next bank's frame control, tagged for the next stream
//     synchronisation point, switches every device at once;
//   - the card-level codec controls the sound core calls (volume, mute,
//     outputs, jack-aware routing).
//
// Locking:
//   bus_lock      spinlock, IRQs off, one FIFO transaction (<= ~1 ms;
//                 bounded by the transfer timeout when hardware is gone)
//   stream_busy   flag owning a stream sequence; process context waits
//                 for it (sleeping), a caller that cannot sleep (trigger
//                 with IRQs off) hands its request to the work thread
//   service_lock  sleeping lock for enumeration and alert handling
//   ctl_lock      sleeping lock for the card-level control state
// Codec drivers have their own locks and are never called with ctl_lock
// held; jack reports only queue the routing update.
//
// Copyright (C) 2026 The LikeOS Project

#include "sdw.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

#define SDW_ALERT_RETRY 10
#define SDW_ENUM_MAX_LOOPS (2 * SDW_MAX_DEVNUM)
#define SDW_BANK_SWITCH_US 3000u
#define SDW_POLL_MS 1000u

/* ==========================================================================
 * Context helpers
 * ========================================================================== */

int sdw_can_sleep(void)
{
	return irqs_enabled() && sched_current() != NULL;
}

void sdw_msleep(uint32_t ms)
{
	uint64_t end;
	int guard = 0;

	if (!ms)
		return;
	if (!sdw_can_sleep()) {
		snd_mdelay(ms);
		return;
	}
	end = hrtimer_now_ns() + (uint64_t)ms * 1000000ull;
	while (hrtimer_now_ns() < end && guard++ < 10000) {
		if (hrtimer_sleep_until(end, NULL) == 0)
			break;
		/* a signal cut the sleep short: keep the delay, gently */
		snd_udelay(100);
	}
}

static void bus_lock(struct sdw_link *l, uint64_t *fl)
{
	spin_lock_irqsave(&l->bus_lock, fl);
}

static void bus_unlock(struct sdw_link *l, uint64_t fl)
{
	spin_unlock_irqrestore(&l->bus_lock, fl);
}

/* Take the stream sequence flag.  1 = taken. */
static int stream_try_lock(struct sdw_link *l)
{
	return __atomic_exchange_n(&l->stream_busy, 1, __ATOMIC_ACQUIRE) == 0;
}

static int stream_lock_wait(struct sdw_link *l, uint32_t timeout_ms)
{
	uint64_t end = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	while (!stream_try_lock(l)) {
		if (hrtimer_now_ns() >= end)
			return -EBUSY;
		if (sdw_can_sleep())
			sdw_msleep(1);
		else
			snd_udelay(50);
	}
	return 0;
}

static void stream_unlock(struct sdw_link *l)
{
	__atomic_store_n(&l->stream_busy, 0, __ATOMIC_RELEASE);
}

static int do_release_idx(struct sdw_link *l, int idx);

static int cmd_to_errno(int rc)
{
	switch (rc) {
	case SDW_CMD_OK:
		return 0;
	case SDW_CMD_IGNORED:
		return -ENODATA;
	case SDW_CMD_FAIL:
		return -EIO;
	default:
		return -ETIMEDOUT;
	}
}

/* ==========================================================================
 * Register access
 * ========================================================================== */

/* Raw transfer to a device number (also 0 and 15), with paging. */
static int xfer_dev(struct sdw_link *l, int dev, int paging, uint32_t addr,
		    size_t n, uint8_t *buf, int read)
{
	uint64_t fl;
	size_t done = 0;

	if (!l || !buf || !n)
		return -EINVAL;
	if (!l->up || l->dead)
		return -ENODEV;
	if (addr >= SDW_R_MAX || n > 4096 || addr + n > SDW_R_MAX)
		return -EINVAL;

	while (done < n) {
		uint32_t a = addr + (uint32_t)done;
		size_t chunk = n - done;
		uint16_t baddr;
		int page = 0, rc;
		uint8_t p1 = 0, p2 = 0;

		if (chunk > 8)
			chunk = 8;
		if (a < SDW_R_NO_PAGE_LIMIT ||
		    (a < SDW_R_OPTIONAL_PAGE && !paging)) {
			/* stay below the boundary in one batch */
			uint32_t lim = a < SDW_R_NO_PAGE_LIMIT ?
					       SDW_R_NO_PAGE_LIMIT :
					       SDW_R_OPTIONAL_PAGE;
			if (a + chunk > lim)
				chunk = lim - a;
			baddr = (uint16_t)a;
		} else {
			uint32_t in_page;
			if (!paging || dev == SDW_ENUM_DEVNUM ||
			    dev == SDW_BCAST_DEVNUM)
				return -EINVAL;
			page = 1;
			p1 = (uint8_t)SDW_R_PAGE1(a);
			p2 = (uint8_t)SDW_R_PAGE2(a);
			in_page = a & 0x7fffu;
			if (in_page + chunk > 0x8000u)
				chunk = 0x8000u - in_page;
			baddr = (uint16_t)(in_page | 0x8000u);
		}
		bus_lock(l, &fl);
		rc = sdw_cdns_xfer(l, dev, baddr, read, buf + done, (int)chunk,
				   0, page, p1, p2);
		bus_unlock(l, fl);
		if (rc != SDW_CMD_OK)
			return cmd_to_errno(rc);
		done += chunk;
	}
	return 0;
}

static int slave_usable(struct sdw_slave *s)
{
	if (!s || !s->link || !s->dev_num)
		return 0;
	if (!s->link->up || s->link->dead)
		return 0;
	return 1;
}

int sdw_nread(struct sdw_slave *s, uint32_t addr, size_t n, uint8_t *buf)
{
	int rc;

	if (!slave_usable(s))
		return -ENODEV;
	rc = xfer_dev(s->link, s->dev_num, s->props.paging, addr, n, buf, 1);
	if (rc && rc != -ENODATA)
		s->io_errors++;
	return rc;
}

int sdw_nwrite(struct sdw_slave *s, uint32_t addr, size_t n,
	       const uint8_t *buf)
{
	uint8_t tmp[64];
	size_t done = 0;
	int rc = 0;

	if (!slave_usable(s))
		return -ENODEV;
	/* the transfer path needs a writable buffer; copy in pieces */
	while (done < n) {
		size_t chunk = n - done;
		if (chunk > sizeof(tmp))
			chunk = sizeof(tmp);
		kmemcpy(tmp, buf + done, chunk);
		rc = xfer_dev(s->link, s->dev_num, s->props.paging,
			      addr + (uint32_t)done, chunk, tmp, 0);
		if (rc)
			break;
		done += chunk;
	}
	if (rc && rc != -ENODATA)
		s->io_errors++;
	return rc;
}

int sdw_read(struct sdw_slave *s, uint32_t addr)
{
	uint8_t v = 0;
	int rc = sdw_nread(s, addr, 1, &v);

	return rc ? rc : (int)v;
}

int sdw_write(struct sdw_slave *s, uint32_t addr, uint8_t val)
{
	return sdw_nwrite(s, addr, 1, &val);
}

int sdw_update(struct sdw_slave *s, uint32_t addr, uint8_t mask, uint8_t val)
{
	int v = sdw_read(s, addr);

	if (v < 0)
		return v;
	return sdw_write(s, addr, (uint8_t)(((uint8_t)v & ~mask) | (val & mask)));
}

int sdw_read32(struct sdw_slave *s, uint32_t addr, uint32_t *val)
{
	uint8_t b[4];
	int rc;

	if (!val)
		return -EINVAL;
	rc = sdw_nread(s, addr, 4, b);
	if (rc)
		return rc;
	*val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
	return 0;
}

int sdw_write32(struct sdw_slave *s, uint32_t addr, uint32_t val)
{
	uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8),
			 (uint8_t)(val >> 16), (uint8_t)(val >> 24) };

	return sdw_nwrite(s, addr, 4, b);
}

int sdw_update32(struct sdw_slave *s, uint32_t addr, uint32_t mask,
		 uint32_t val)
{
	uint32_t v;
	int rc = sdw_read32(s, addr, &v);

	if (rc)
		return rc;
	return sdw_write32(s, addr, (v & ~mask) | (val & mask));
}

int sdw_mbq_write(struct sdw_slave *s, uint32_t addr, int bytes, uint32_t val)
{
	int rc;

	if (bytes < 1 || bytes > 4)
		return -EINVAL;
	/* the high bytes go to the MBQ shadow, the low byte commits */
	for (int i = bytes - 1; i > 0; i--) {
		rc = sdw_write(s, SDW_SDCA_MBQ(addr), (uint8_t)(val >> (8 * i)));
		if (rc)
			return rc;
	}
	return sdw_write(s, addr, (uint8_t)val);
}

int sdw_mbq_read(struct sdw_slave *s, uint32_t addr, int bytes, uint32_t *val)
{
	int v;
	uint32_t out;

	if (bytes < 1 || bytes > 4 || !val)
		return -EINVAL;
	v = sdw_read(s, addr);
	if (v < 0)
		return v;
	out = (uint32_t)v;
	for (int i = 1; i < bytes; i++) {
		v = sdw_read(s, SDW_SDCA_MBQ(addr));
		if (v < 0)
			return v;
		out |= (uint32_t)v << (8 * i);
	}
	*val = out;
	return 0;
}

/* ==========================================================================
 * Peripheral table
 * ========================================================================== */

const struct sdw_codec_ops *const g_codec_drivers[] = {
	&sdw_cs42l43_ops,
	&sdw_max98373_ops,
	&sdw_sdca_ops,
};

static const struct sdw_codec_ops *pick_codec(const struct sof_sdw_peripheral *p)
{
	for (unsigned i = 0;
	     i < sizeof(g_codec_drivers) / sizeof(g_codec_drivers[0]); i++) {
		if (g_codec_drivers[i]->match && g_codec_drivers[i]->match(p))
			return g_codec_drivers[i];
	}
	return &sdw_null_ops;
}

static void slave_attach_ops(struct sdw_slave *s)
{
	s->ops = pick_codec(s->p);
	if (s->ops->probe && s->ops->probe(s)) {
		sdw_info("%s: %s probe failed, using the generic handling\n",
			 s->name, s->ops->name);
		s->ops = &sdw_null_ops;
		s->codec = NULL;
		if (s->ops->probe)
			s->ops->probe(s);
	}
	s->p->codec_priv = s->codec;
}

static void slave_name(struct sdw_slave *s)
{
	ksnprintf(s->name, sizeof(s->name), "%04x:%04x-l%d-u%u", s->p->mfg_id,
		  s->p->part_id, s->p->link, s->p->unique_id);
}

static void bus_params_init(struct sdw_link *l)
{
	struct sdw_master_props *m = &l->mprops;
	struct sdw_bus_params *bp = &l->params;
	uint32_t total;

	bp->max_dr_freq = m->max_clk_freq * 2;
	bp->curr_dr_freq = bp->max_dr_freq;
	bp->bandwidth = 0;
	bp->curr_bank = 0;
	bp->next_bank = 1;
	bp->row = m->default_row;
	bp->col = m->default_col;
	if (!m->default_frame_rate || !bp->curr_dr_freq)
		return;
	total = bp->curr_dr_freq / m->default_frame_rate;
	/* rows x columns must be the bits of one frame */
	if (sdw_find_row_index(bp->row) >= 0 && bp->row &&
	    total % bp->row == 0 &&
	    sdw_find_col_index(total / bp->row) >= 0) {
		bp->col = total / bp->row;
		return;
	}
	for (int c = 0; c < 8; c++) {
		uint32_t col = sdw_cols[c];
		if (total % col)
			continue;
		if (sdw_find_row_index(total / col) >= 0 && col >= 2) {
			bp->col = col;
			bp->row = total / col;
			return;
		}
	}
	sdw_info("link %d: no frame shape for %u bits per frame, using %ux%u\n",
		 l->id, total, bp->row, bp->col);
}

int sdw_bus_link_setup(struct sdw_link *l)
{
	struct sof_dev *sdev = l->sdev;
	struct sof_machine *m = &sdev->machine;

	l->nslaves = 0;
	l->devnum_used = 0;
	for (int i = 0; i < m->nperiph && l->nslaves < SOF_MAX_SDW_PERIPHERALS;
	     i++) {
		struct sof_sdw_peripheral *p = &m->periph[i];
		struct sdw_slave *s;

		if (p->link != l->id)
			continue;
		s = &l->slaves[l->nslaves++];
		kmemset(s, 0, sizeof(*s));
		s->link = l;
		s->p = p;
		s->periph_idx = i;
		s->status = SDW_SLAVE_UNATTACHED;
		p->dev_num = 0;
		p->attached = 0;
		slave_name(s);
		sof_acpi_slave_props(sdev, i, &s->props);
		slave_attach_ops(s);
	}
	bus_params_init(l);
	return 0;
}

void sdw_bus_link_teardown(struct sdw_link *l)
{
	if (stream_lock_wait(l, 2000) == 0) {
		for (int i = 0; i < SDW_LINK_STREAMS; i++)
			if (l->streams[i].state != SDW_STREAM_IDLE)
				do_release_idx(l, i);
		stream_unlock(l);
	}
	for (int i = 0; i < l->nslaves; i++) {
		struct sdw_slave *s = &l->slaves[i];
		if (s->ops && s->ops->remove)
			s->ops->remove(s);
		s->codec = NULL;
		if (s->p)
			s->p->codec_priv = NULL;
	}
}

/* ==========================================================================
 * Enumeration
 * ========================================================================== */

static int count_same_type(struct sdw_link *l, uint16_t mfg, uint16_t part,
			   uint8_t cls)
{
	int n = 0;

	for (int i = 0; i < l->nslaves; i++) {
		struct sof_sdw_peripheral *p = l->slaves[i].p;
		if (p->mfg_id == mfg && p->part_id == part && p->class_id == cls)
			n++;
	}
	return n;
}

static struct sdw_slave *match_slave(struct sdw_link *l, uint64_t id)
{
	uint8_t uid = (uint8_t)((id >> 40) & 0xf);
	uint16_t mfg = (uint16_t)((id >> 24) & 0xffff);
	uint16_t part = (uint16_t)((id >> 8) & 0xffff);
	uint8_t cls = (uint8_t)(id & 0xff);
	int same = count_same_type(l, mfg, part, cls);
	struct sdw_slave *loose = NULL;

	for (int i = 0; i < l->nslaves; i++) {
		struct sdw_slave *s = &l->slaves[i];
		struct sof_sdw_peripheral *p = s->p;

		if (p->mfg_id != mfg || p->part_id != part ||
		    p->class_id != cls)
			continue;
		if (p->unique_id == uid)
			return s;
		/* the unique id only matters between devices of one kind */
		if (same <= 1 && !loose)
			loose = s;
	}
	return loose;
}

/* A device the firmware did not describe: keep track of it anyway. */
static struct sdw_slave *add_unlisted_slave(struct sdw_link *l, uint64_t id)
{
	struct sof_machine *m = &l->sdev->machine;
	struct sof_sdw_peripheral *p;
	struct sdw_slave *s;

	if (l->nslaves >= SOF_MAX_SDW_PERIPHERALS ||
	    m->nperiph >= SOF_MAX_SDW_PERIPHERALS)
		return NULL;
	p = &m->periph[m->nperiph];
	kmemset(p, 0, sizeof(*p));
	p->link = l->id;
	p->version = (uint8_t)((id >> 44) & 0xf);
	p->unique_id = (uint8_t)((id >> 40) & 0xf);
	p->mfg_id = (uint16_t)((id >> 24) & 0xffff);
	p->part_id = (uint16_t)((id >> 8) & 0xffff);
	p->class_id = (uint8_t)(id & 0xff);
	s = &l->slaves[l->nslaves++];
	kmemset(s, 0, sizeof(*s));
	s->link = l;
	s->p = p;
	s->periph_idx = m->nperiph;
	s->status = SDW_SLAVE_UNATTACHED;
	m->nperiph++;
	slave_name(s);
	sof_acpi_slave_props(l->sdev, -1, &s->props);
	/* without firmware data, assume paging (every codec has it) */
	s->props.paging = 1;
	slave_attach_ops(s);
	sdw_info("%s: not described by the firmware, added\n", s->name);
	return s;
}

static int pick_devnum(struct sdw_link *l)
{
	for (int n = 1; n <= SDW_MAX_DEVNUM; n++)
		if (!(l->devnum_used & (1u << n)))
			return n;
	return -ENOSPC;
}

/* Number every device currently answering as device 0.  Returns how many
 * got a number. */
static int program_device_numbers(struct sdw_link *l)
{
	int assigned = 0;

	for (int loop = 0; loop < SDW_ENUM_MAX_LOOPS; loop++) {
		uint8_t b[6];
		uint64_t id;
		struct sdw_slave *s;
		uint8_t dn;
		int rc;

		rc = xfer_dev(l, SDW_ENUM_DEVNUM, 0, SDW_SCP_DEVID_0, 6, b, 1);
		if (rc == -ENODATA)
			break; /* nobody left at device 0 */
		if (rc) {
			sdw_info("link %d: DEVID read failed (%d)\n", l->id,
				 rc);
			break;
		}
		id = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) |
		     ((uint64_t)b[2] << 24) | ((uint64_t)b[3] << 16) |
		     ((uint64_t)b[4] << 8) | (uint64_t)b[5];
		if (!id || id == 0xffffffffffffull) {
			sdw_info("link %d: implausible device id\n", l->id);
			break;
		}
		s = match_slave(l, id);
		if (!s)
			s = add_unlisted_slave(l, id);
		if (!s) {
			l->st_unknown_dev++;
			break;
		}
		if (s->dev_num && s->status != SDW_SLAVE_UNATTACHED) {
			/* it reset behind our back: it starts over */
			s->status = SDW_SLAVE_UNATTACHED;
			if (s->ops && s->ops->detached)
				s->ops->detached(s);
			if (s->initialized && !s->in_init)
				s->needs_init = 1;
		}
		if (!s->dev_num) {
			int n = pick_devnum(l);
			if (n < 0) {
				sdw_info("link %d: out of device numbers\n",
					 l->id);
				break;
			}
			s->dev_num = n;
			l->devnum_used |= 1u << n;
		}
		s->id48 = id;
		s->p->version = (uint8_t)((id >> 44) & 0xf);
		dn = (uint8_t)s->dev_num;
		rc = xfer_dev(l, SDW_ENUM_DEVNUM, 0, SDW_SCP_DEVNUMBER, 1, &dn,
			      0);
		if (rc) {
			sdw_info("%s: device number %d not taken (%d)\n",
				 s->name, s->dev_num, rc);
			break;
		}
		s->p->dev_num = s->dev_num;
		l->st_enum++;
		assigned++;
		/* the device changes number at the next frame */
		snd_udelay(100);
	}
	return assigned;
}

/* Bus clock base and scale (SDCA devices and those that ask for it). */
static void slave_set_bus_clock(struct sdw_slave *s)
{
	struct sdw_link *l = s->link;
	uint32_t mclk = l->mprops.mclk_freq;
	uint32_t curr = l->params.curr_dr_freq / 2;
	uint32_t scale;
	uint8_t base;
	int idx;

	if (!(s->p->class_id || s->props.clock_reg) || !mclk || !curr)
		return;
	if (!(19200000u % mclk)) {
		mclk = 19200000u;
		base = SDW_CLK_BASE_19200000;
	} else if (!(22579200u % mclk)) {
		mclk = 22579200u;
		base = SDW_CLK_BASE_22579200;
	} else if (!(24576000u % mclk)) {
		mclk = 24576000u;
		base = SDW_CLK_BASE_24576000;
	} else if (!(32000000u % mclk)) {
		mclk = 32000000u;
		base = SDW_CLK_BASE_32000000;
	} else if (!(96000000u % mclk)) {
		mclk = 24000000u;
		base = SDW_CLK_BASE_24000000;
	} else {
		sdw_info("%s: unsupported clock base %u\n", s->name, mclk);
		return;
	}
	if (mclk % curr) {
		sdw_info("%s: bus clock %u not a divisor of %u\n", s->name, curr,
			 mclk);
		return;
	}
	scale = mclk / curr;
	idx = __builtin_ctz(scale);
	if ((1u << idx) != scale || idx > 6) {
		sdw_info("%s: no clock scale for %u\n", s->name, scale);
		return;
	}
	idx++;
	sdw_write(s, SDW_SCP_BUS_CLOCK_BASE, base);
	sdw_write(s, SDW_SCP_BUSCLOCK_SCALE_B0, (uint8_t)idx);
	sdw_write(s, SDW_SCP_BUSCLOCK_SCALE_B1, (uint8_t)idx);
}

/* Bus-level set-up right after a device attached at its number. */
static void slave_initialize(struct sdw_slave *s)
{
	int v;
	uint8_t mask;

	slave_set_bus_clock(s);

	/* clashes and parity errors from before enumeration are noise */
	v = sdw_read(s, SDW_SCP_INT1);
	if (v > 0) {
		uint8_t clr = (uint8_t)v & (SDW_SCP_INT1_BUS_CLASH |
					    SDW_SCP_INT1_PARITY);
		if (clr)
			sdw_write(s, SDW_SCP_INT1, clr);
	}
	mask = (uint8_t)(s->props.scp_int1_mask &
			 (SDW_SCP_INT1_PARITY | SDW_SCP_INT1_BUS_CLASH |
			  SDW_SCP_INT1_IMPL_DEF));
	if (mask)
		sdw_update(s, SDW_SCP_INTMASK1, mask, mask);
}

static void slave_detached(struct sdw_slave *s)
{
	if (s->status == SDW_SLAVE_UNATTACHED)
		return;
	s->status = SDW_SLAVE_UNATTACHED;
	s->p->attached = 0;
	if (s->ops && s->ops->detached)
		s->ops->detached(s);
	if (s->initialized && !s->in_init)
		s->needs_init = 1;
}

static struct sdw_slave *slave_by_devnum(struct sdw_link *l, int dn)
{
	for (int i = 0; i < l->nslaves; i++)
		if (l->slaves[i].dev_num == dn)
			return &l->slaves[i];
	return NULL;
}

/* ---- alerts --------------------------------------------------------------- */

static void handle_dp0(struct sdw_slave *s)
{
	int v = sdw_read(s, SDW_DP0_INT);
	uint8_t clr;

	if (v <= 0)
		return;
	clr = (uint8_t)v & (SDW_DP0_INT_TEST_FAIL | SDW_DP0_INT_PORT_READY |
			    SDW_DP0_INT_BRA_FAILURE | SDW_DP0_INT_IMPDEF);
	if (v & SDW_DP0_INT_TEST_FAIL)
		sdw_info("%s: port 0 test failure\n", s->name);
	if (clr)
		sdw_write(s, SDW_DP0_INT, clr);
}

static void handle_dpn(struct sdw_slave *s, int port)
{
	int v = sdw_read(s, SDW_DPN_INT(port));
	uint8_t clr;

	if (v <= 0)
		return;
	clr = (uint8_t)v & (SDW_DPN_INT_TEST_FAIL | SDW_DPN_INT_PORT_READY |
			    SDW_DPN_INT_IMPDEF);
	if (v & SDW_DPN_INT_TEST_FAIL)
		sdw_info("%s: port %d test failure\n", s->name, port);
	if (clr)
		sdw_write(s, SDW_DPN_INT(port), clr);
}

static void handle_alerts(struct sdw_slave *s)
{
	for (int round = 0; round < SDW_ALERT_RETRY; round++) {
		uint8_t st23[2] = { 0, 0 };
		uint8_t sdca[4] = { 0, 0, 0, 0 };
		int int1, cascade = 0;
		uint8_t clr = 0;
		uint32_t ports;

		int1 = sdw_read(s, SDW_SCP_INT1);
		if (int1 < 0)
			return;
		if (sdw_nread(s, SDW_SCP_INTSTAT2, 2, st23))
			st23[0] = st23[1] = 0;
		if (s->p->class_id) {
			int dp0 = sdw_read(s, SDW_DP0_INT);
			if (dp0 > 0 && (dp0 & SDW_DP0_SDCA_CASCADE)) {
				cascade = 1;
				if (sdw_nread(s, SDW_SCP_SDCA_INT1, 4, sdca))
					kmemset(sdca, 0, sizeof(sdca));
			}
		}
		if (!(int1 & 0xff) && !st23[0] && !st23[1] && !cascade)
			return;
		s->alert_count++;

		if (int1 & SDW_SCP_INT1_PARITY) {
			if (s->parity_reported || !s->props.invalid_initial_parity)
				if (s->alert_count < 16)
					sdw_info("%s: parity error\n", s->name);
			s->parity_reported = 1;
			clr |= SDW_SCP_INT1_PARITY;
		}
		if (int1 & SDW_SCP_INT1_BUS_CLASH) {
			if (s->alert_count < 16)
				sdw_info("%s: bus clash\n", s->name);
			clr |= SDW_SCP_INT1_BUS_CLASH;
		}
		if (int1 & SDW_SCP_INT1_IMPL_DEF)
			clr |= SDW_SCP_INT1_IMPL_DEF;

		/* data port interrupts */
		ports = ((uint32_t)(int1 & SDW_SCP_INT1_PORT0_3) >> 3) |
			((uint32_t)(st23[0] & SDW_SCP_INTSTAT2_PORT4_10) << 4) |
			((uint32_t)(st23[1] & SDW_SCP_INTSTAT3_PORT11_14) << 11);
		for (int p = 0; p < SDW_MAX_PORTS; p++) {
			if (!(ports & (1u << p)))
				continue;
			if (p == 0)
				handle_dp0(s);
			else
				handle_dpn(s, p);
		}

		/* the codec's own sources, then the SDCA bits */
		if (((int1 & SDW_SCP_INT1_IMPL_DEF) || cascade) && s->ops &&
		    s->ops->alert)
			s->ops->alert(s, (uint8_t)int1, sdca);
		if (cascade) {
			for (int i = 0; i < 4; i++)
				if (sdca[i])
					sdw_write(s, SDW_SCP_SDCA_INT1 + i,
						  sdca[i]);
		}
		if (clr)
			sdw_write(s, SDW_SCP_INT1, clr);
	}
	if (s->alert_count < 32)
		sdw_info("%s: alert did not clear\n", s->name);
}

/* ---- the service ------------------------------------------------------------ */

void sdw_bus_service(struct sdw_link *l)
{
	struct sdw_ctx *c;

	if (!l || !l->up || l->dead)
		return;
	c = l->ctx;
	if (c->stopping)
		return;
	if (snd_mutex_lock(&l->service_lock, 0))
		return;
	for (int iter = 0; iter < 6; iter++) {
		uint32_t stat = sdw_cdns_slave_stat(l);
		int again = 0;

		if (stat == 0xffffffffu || l->dead)
			break;
		l->last_slave_stat = stat;

		for (int i = 0; i < l->nslaves; i++) {
			struct sdw_slave *s = &l->slaves[i];
			uint32_t st;

			if (!s->dev_num)
				continue;
			st = (stat >> (2 * s->dev_num)) & 3;
			switch (st) {
			case SDW_SLAVE_UNATTACHED:
				slave_detached(s);
				break;
			case SDW_SLAVE_ATTACHED:
			case SDW_SLAVE_ALERT:
				if (s->status == SDW_SLAVE_UNATTACHED) {
					s->status = SDW_SLAVE_ATTACHED;
					s->present = 1;
					s->attach_count++;
					s->p->attached = 1;
					slave_initialize(s);
					if (!s->initialized && !s->in_init)
						s->needs_init = 1;
				}
				if (st == SDW_SLAVE_ALERT) {
					s->status = SDW_SLAVE_ALERT;
					handle_alerts(s);
					s->status = SDW_SLAVE_ATTACHED;
				}
				break;
			default:
				break;
			}
		}
		/* numbers in use that we never handed out */
		for (int dn = 1; dn <= SDW_MAX_DEVNUM; dn++) {
			if (((stat >> (2 * dn)) & 3) && !slave_by_devnum(l, dn) &&
			    !(l->devnum_used & (1u << dn))) {
				/* a device numbered before we started: make it
				 * forget (it comes back as device 0) */
				uint8_t z = 0;
				xfer_dev(l, dn, 0, SDW_SCP_DEVNUMBER, 1, &z, 0);
				l->st_unknown_dev++;
				again = 1;
			}
		}
		if (stat & 3) {
			if (program_device_numbers(l) > 0)
				again = 1;
		}
		if (!again)
			break;
		snd_udelay(500);
	}
	snd_mutex_unlock(&l->service_lock);
}

int sdw_wait_attached(struct sdw_slave *s, uint32_t timeout_ms)
{
	uint64_t end = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	if (!s || !s->link)
		return -EINVAL;
	for (;;) {
		sdw_bus_service(s->link);
		if (s->status != SDW_SLAVE_UNATTACHED && s->dev_num)
			return 0;
		if (s->link->dead || s->link->ctx->stopping)
			return -ENODEV;
		if (hrtimer_now_ns() >= end)
			return -ETIMEDOUT;
		sdw_msleep(5);
	}
}

int sdw_wait_detached(struct sdw_slave *s, uint32_t timeout_ms)
{
	uint64_t end = hrtimer_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	if (!s || !s->link)
		return -EINVAL;
	for (;;) {
		uint32_t stat = sdw_cdns_slave_stat(s->link);
		if (s->dev_num && !((stat >> (2 * s->dev_num)) & 3)) {
			slave_detached(s);
			return 0;
		}
		if (s->link->dead)
			return -ENODEV;
		if (hrtimer_now_ns() >= end)
			return -ETIMEDOUT;
		sdw_msleep(2);
	}
}

/* ==========================================================================
 * Work: interrupts, polling, codec initialisation
 * ========================================================================== */

static void link_irq_work(void *arg)
{
	struct sdw_link *l = arg;

	if (!l->ctx->stopping) {
		do {
			uint32_t s0, s1;
			l->irq_pending = 0;
			sdw_cdns_ack_slave_intstat(l, &s0, &s1);
			sdw_bus_service(l);
		} while (l->irq_pending && !l->ctx->stopping);
		sdw_cdns_slave_irq_enable(l, 1);
		sdw_bus_init_pending(l->ctx);
	}
	__atomic_store_n(&l->work_queued, 0, __ATOMIC_RELEASE);
	if (l->irq_pending && !l->ctx->stopping &&
	    !__atomic_exchange_n(&l->work_queued, 1, __ATOMIC_ACQ_REL)) {
		if (snd_schedule_work(link_irq_work, l, 0))
			__atomic_store_n(&l->work_queued, 0, __ATOMIC_RELEASE);
	}
}

void sdw_bus_irq_notify(struct sdw_link *l)
{
	if (!l || l->ctx->stopping)
		return;
	l->irq_pending = 1;
	if (__atomic_exchange_n(&l->work_queued, 1, __ATOMIC_ACQ_REL))
		return;
	if (snd_schedule_work(link_irq_work, l, 0))
		__atomic_store_n(&l->work_queued, 0, __ATOMIC_RELEASE);
}

void sdw_bus_link_started(struct sdw_link *l)
{
	l->params.curr_bank = 0;
	l->params.next_bank = 1;
	for (int i = 0; i < l->nslaves; i++) {
		l->slaves[i].status = SDW_SLAVE_UNATTACHED;
		l->slaves[i].dev_num = 0;
		l->slaves[i].p->dev_num = 0;
	}
	l->devnum_used = 0;
}

static void apply_routing(struct sdw_ctx *c);
static void apply_saved_controls(struct sdw_slave *s);

void sdw_bus_init_pending(struct sdw_ctx *c)
{
	if (!c || c->stopping)
		return;
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->up || l->dead)
			continue;
		for (int j = 0; j < l->nslaves; j++) {
			struct sdw_slave *s = &l->slaves[j];
			int rc;

			if (!s->needs_init || s->in_init ||
			    s->status == SDW_SLAVE_UNATTACHED)
				continue;
			/* one initialisation at a time, whoever calls */
			if (__atomic_exchange_n(&s->in_init, 1, __ATOMIC_ACQ_REL))
				continue;
			s->needs_init = 0;
			if (!s->ops || !s->ops->init) {
				s->initialized = 1;
				__atomic_store_n(&s->in_init, 0, __ATOMIC_RELEASE);
				continue;
			}
			rc = s->ops->init(s);
			s->in_init = 0;
			if (rc) {
				s->initialized = 0;
				s->init_errors++;
				sdw_info("%s: %s initialisation failed (%d)%s\n",
					 s->name, s->ops->name, rc,
					 s->init_errors < 3 ? ", will retry" :
							      "");
				if (s->init_errors < 3)
					s->needs_init = 1;
				continue;
			}
			s->initialized = 1;
			sdw_info("%s: %s ready (device %d)\n", s->name,
				 s->ops->name, s->dev_num);
			apply_saved_controls(s);
		}
	}
	apply_routing(c);
}

static void sdw_poll_work(void *arg)
{
	struct sdw_ctx *c = arg;

	if (c->stopping) {
		c->poll_armed = 0;
		return;
	}
	c->poll_count++;
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->up || l->dead)
			continue;
		if (!l->work_queued) {
			sdw_bus_service(l);
			sdw_cdns_slave_irq_enable(l, 1);
		}
	}
	sdw_bus_init_pending(c);
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->up || l->dead)
			continue;
		for (int j = 0; j < l->nslaves; j++) {
			struct sdw_slave *s = &l->slaves[j];
			if (s->initialized && !s->in_init &&
			    s->status != SDW_SLAVE_UNATTACHED && s->ops &&
			    s->ops->poll)
				s->ops->poll(s);
		}
	}
	if (c->stopping || snd_schedule_work(sdw_poll_work, c, SDW_POLL_MS))
		c->poll_armed = 0;
}

void sdw_bus_start_polling(struct sdw_ctx *c)
{
	if (!c || c->poll_armed || c->stopping)
		return;
	c->poll_armed = 1;
	if (snd_schedule_work(sdw_poll_work, c, SDW_POLL_MS))
		c->poll_armed = 0;
}

/* ==========================================================================
 * Streams: bandwidth and transport parameters
 * ========================================================================== */

static int stream_active(const struct sdw_stream_rt *st)
{
	return st->state == SDW_STREAM_CONFIGURED ||
	       st->state == SDW_STREAM_PREPARED ||
	       st->state == SDW_STREAM_ENABLED ||
	       st->state == SDW_STREAM_DISABLED;
}

/* Lay out every active stream in the frame.  Each rate group gets
 * ceil(columns * payload / bits-per-sample-period) columns, groups packed
 * from the rightmost column; column 0 carries the control word. */
static int compute_params(struct sdw_link *l)
{
	struct sdw_bus_params *bp = &l->params;
	uint32_t rates[SDW_LINK_STREAMS];
	int nrates = 0;
	uint32_t hstop;
	uint32_t bandwidth = 0;

	for (int d = 0; d < SDW_LINK_STREAMS; d++) {
		struct sdw_stream_rt *st = &l->streams[d];
		int found = 0;

		if (!stream_active(st))
			continue;
		bandwidth += st->rate * st->ch * st->bps;
		for (int i = 0; i < nrates; i++)
			if (rates[i] == st->rate)
				found = 1;
		if (!found)
			rates[nrates++] = st->rate;
	}
	bp->bandwidth = bandwidth;
	if (!nrates)
		return 0;
	if (!bp->col || !bp->row || !bp->curr_dr_freq)
		return -EINVAL;
	/* column 0 is the control word: the data must fit in the rest */
	if ((uint64_t)bp->curr_dr_freq * (bp->col - 1) <
	    (uint64_t)bandwidth * bp->col) {
		sdw_info("link %d: %u bit/s do not fit a %ux%u frame at %u Hz\n",
			 l->id, bandwidth, bp->row, bp->col,
			 bp->curr_dr_freq / 2);
		return -ENOSPC;
	}

	hstop = bp->col - 1;
	for (int g = 0; g < nrates; g++) {
		uint32_t rate = rates[g];
		uint32_t full_bw = bp->curr_dr_freq / rate;
		uint32_t payload = 0, hwidth, port_bo = 1;

		if (!full_bw)
			return -EINVAL;
		for (int d = 0; d < SDW_LINK_STREAMS; d++) {
			struct sdw_stream_rt *st = &l->streams[d];
			if (stream_active(st) && st->rate == rate)
				payload += st->bps * st->ch;
		}
		hwidth = (bp->col * payload + full_bw - 1) / full_bw;
		if (!hwidth)
			hwidth = 1;
		if (hwidth > hstop) {
			sdw_info("link %d: rate group %u needs %u columns, %u left\n",
				 l->id, rate, hwidth, hstop);
			return -ENOSPC;
		}
		for (int d = 0; d < SDW_LINK_STREAMS; d++) {
			struct sdw_stream_rt *st = &l->streams[d];
			struct sdw_port_rt *mp = &st->mport;
			uint32_t sbo;
			uint32_t total_ch = 0;

			if (!stream_active(st) || st->rate != rate)
				continue;
			mp->num = st->pdi;
			mp->ch_mask = (1u << st->ch) - 1;
			mp->bps = st->bps;
			mp->sample_interval = full_bw;
			mp->offset1 = port_bo & 0xff;
			mp->offset2 = (port_bo >> 8) & 0xff;
			mp->hstop = hstop;
			mp->hstart = hstop - hwidth + 1;
			mp->lane = 0;
			mp->blk_pkg_mode = 0; /* per port */
			mp->blk_grp_ctrl = 0; /* group count 1 */

			/* peripherals: on playback each reads the channels
			 * it takes from the stream's block; on capture they
			 * write after each other */
			sbo = port_bo;
			for (int i = 0; i < st->nslave; i++) {
				struct sdw_port_rt *sp = &st->sl[i].port;
				uint32_t ch = sdw_hweight32(sp->ch_mask);

				sp->bps = st->bps;
				sp->sample_interval = full_bw;
				sp->offset1 = sbo & 0xff;
				sp->offset2 = (sbo >> 8) & 0xff;
				sp->hstart = mp->hstart;
				sp->hstop = mp->hstop;
				sp->lane = 0;
				sp->blk_pkg_mode = 0;
				sp->blk_grp_ctrl = 0;
				sbo += st->bps * ch;
				total_ch += ch;
				if (st->dir == AUDIO_DIR_PLAYBACK &&
				    total_ch >= st->ch) {
					sbo = port_bo;
					total_ch = 0;
				}
			}
			port_bo += st->bps * st->ch;
		}
		hstop -= hwidth;
	}
	return 0;
}

static int program_slave_port(struct sdw_slave *s, const struct sdw_port_rt *p,
			      int bank)
{
	const struct sdw_dpn_props *dp;
	uint32_t n = (uint32_t)p->num;
	uint32_t boff = bank ? SDW_BANK1_OFFSET : 0;
	uint32_t si = p->sample_interval ? p->sample_interval - 1 : 0;
	int rc;

	if (p->num < 1 || p->num >= SDW_MAX_PORTS)
		return -EINVAL;
	dp = &s->props.dp[p->num];
	/* isochronous flow, normal data */
	rc = sdw_update(s, SDW_DPN_PORTCTRL(n), 0x0f, 0);
	if (rc)
		return rc;
	if (!dp->read_only_wordlength) {
		rc = sdw_write(s, SDW_DPN_BLOCKCTRL1(n),
			       (uint8_t)((p->bps - 1) & 0x3f));
		if (rc)
			return rc;
	}
	rc = sdw_write(s, SDW_DPN_SAMPLECTRL1(n) + boff, (uint8_t)(si & 0xff));
	if (rc)
		return rc;
	rc = sdw_write(s, SDW_DPN_OFFSETCTRL1(n) + boff, (uint8_t)p->offset1);
	if (rc)
		return rc;
	if (s->props.lane_control || dp->lane_control) {
		rc = sdw_write(s, SDW_DPN_LANECTRL(n) + boff, (uint8_t)p->lane);
		if (rc)
			return rc;
	}
	if (dp->type == SDW_DPN_SIMPLE)
		return 0;
	rc = sdw_write(s, SDW_DPN_OFFSETCTRL2(n) + boff, (uint8_t)p->offset2);
	if (rc)
		return rc;
	rc = sdw_write(s, SDW_DPN_BLOCKCTRL3(n) + boff,
		       (uint8_t)p->blk_pkg_mode);
	if (rc)
		return rc;
	if (dp->type != SDW_DPN_FULL)
		return 0;
	rc = sdw_write(s, SDW_DPN_SAMPLECTRL2(n) + boff,
		       (uint8_t)((si >> 8) & 0xff));
	if (rc)
		return rc;
	return sdw_write(s, SDW_DPN_HCTRL(n) + boff,
			 (uint8_t)(((p->hstart & 0xf) << 4) | (p->hstop & 0xf)));
}

static int slave_port_enable(struct sdw_slave *s, const struct sdw_port_rt *p,
			     int bank, int on)
{
	uint32_t boff = bank ? SDW_BANK1_OFFSET : 0;

	if (p->num < 1 || p->num >= SDW_MAX_PORTS)
		return -EINVAL;
	return sdw_write(s, SDW_DPN_CHANNELEN((uint32_t)p->num) + boff,
			 on ? (uint8_t)p->ch_mask : 0);
}

/* Write every active stream's parameters into the next bank. */
static int program_params(struct sdw_link *l)
{
	int bank = l->params.next_bank;
	int err = 0;

	/* peripheral bus clock scale for the next bank */
	for (int i = 0; i < l->nslaves; i++) {
		struct sdw_slave *s = &l->slaves[i];
		(void)s;
	}
	for (int d = 0; d < SDW_LINK_STREAMS; d++) {
		struct sdw_stream_rt *st = &l->streams[d];
		int on;

		if (!stream_active(st))
			continue;
		on = st->state == SDW_STREAM_ENABLED;
		for (int i = 0; i < st->nslave; i++) {
			struct sdw_slave *s = st->sl[i].s;
			int rc;

			if (s->status == SDW_SLAVE_UNATTACHED)
				continue;
			rc = program_slave_port(s, &st->sl[i].port, bank);
			if (!rc)
				rc = slave_port_enable(s, &st->sl[i].port, bank,
						       on);
			if (rc) {
				sdw_info("%s: port %d programming failed (%d)\n",
					 s->name, st->sl[i].port.num, rc);
				err = rc;
			}
		}
		sdw_cdns_dpn_params(l, bank, &st->mport);
		sdw_cdns_dpn_enable(l, bank, st->mport.num,
				    on ? st->mport.ch_mask : 0);
	}
	sdw_cdns_bus_conf(l, bank, l->params.curr_dr_freq);
	return err;
}

static int bank_switch(struct sdw_link *l)
{
	struct sdw_bus_params *bp = &l->params;
	int ri = sdw_find_row_index(bp->row);
	int ci = sdw_find_col_index(bp->col);
	uint32_t addr = bp->next_bank ? SDW_SCP_FRAMECTRL_B1 :
					SDW_SCP_FRAMECTRL_B0;
	uint8_t v;
	uint64_t fl;
	int rc;

	if (ri < 0 || ci < 0)
		return -EINVAL;
	v = (uint8_t)(ci | (ri << 3));
	sdw_intel_sync_arm(l);
	bus_lock(l, &fl);
	rc = sdw_cdns_xfer(l, SDW_BCAST_DEVNUM, (uint16_t)addr, 0, &v, 1, 1, 0,
			   0, 0);
	bus_unlock(l, fl);
	sdw_intel_sync_go(l);
	if (rc != SDW_CMD_OK && rc != SDW_CMD_IGNORED) {
		sdw_info("link %d: bank switch failed (%d)\n", l->id, rc);
		return cmd_to_errno(rc);
	}
	/* the switch happens at the next stream synchronisation point */
	for (uint32_t us = 0; us < SDW_BANK_SWITCH_US; us += 20) {
		if (sdw_cdns_active_bank(l) == bp->next_bank)
			break;
		snd_udelay(20);
	}
	bp->curr_bank = bp->next_bank;
	bp->next_bank = !bp->next_bank;
	l->st_bank_switch++;
	return 0;
}

static int slave_port_prepare(struct sdw_slave *s, const struct sdw_port_rt *p,
			      int prep)
{
	const struct sdw_dpn_props *dp;
	uint32_t n = (uint32_t)p->num;
	uint8_t imask;
	int intr, rc = 0;

	if (p->num < 1 || p->num >= SDW_MAX_PORTS)
		return -EINVAL;
	dp = &s->props.dp[p->num];
	imask = (uint8_t)(SDW_DPN_INT_TEST_FAIL | SDW_DPN_INT_PORT_READY |
			  (dp->imp_def_interrupts & SDW_DPN_INT_IMPDEF));
	intr = dp->imp_def_interrupts || !dp->simple_ch_prep_sm;
	if (prep && intr)
		sdw_update(s, SDW_DPN_INTMASK(n), imask, imask);

	if (!dp->simple_ch_prep_sm) {
		uint32_t tmo = dp->ch_prep_timeout_ms ? dp->ch_prep_timeout_ms :
							50;
		uint64_t end = hrtimer_now_ns() + (uint64_t)tmo * 1000000ull;

		rc = sdw_write(s, SDW_DPN_PREPARECTRL(n),
			       prep ? (uint8_t)p->ch_mask : 0);
		if (rc)
			return rc;
		for (;;) {
			int v = sdw_read(s, SDW_DPN_PREPARESTATUS(n));
			if (v < 0) {
				rc = v;
				break;
			}
			if (!(v & (int)p->ch_mask))
				break;
			if (hrtimer_now_ns() >= end) {
				sdw_info("%s: port %u channel %s timed out (0x%02x)\n",
					 s->name, n,
					 prep ? "prepare" : "deprepare", v);
				rc = -ETIMEDOUT;
				break;
			}
			if (sdw_can_sleep())
				sdw_msleep(1);
			else
				snd_udelay(100);
		}
	}
	if (!prep && intr)
		sdw_update(s, SDW_DPN_INTMASK(n), imask, 0);
	return rc;
}

/* ==========================================================================
 * Streams: the contract functions
 * ========================================================================== */

static struct sdw_link *link_of(struct sof_dev *sdev, int link)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);

	if (!c || link < 0 || link >= SDW_MAX_LINKS)
		return NULL;
	if (!c->link[link].up || c->link[link].dead)
		return NULL;
	return &c->link[link];
}

static int valid_rate(struct sdw_link *l, uint32_t rate)
{
	if (rate < 8000 || rate > 384000)
		return 0;
	/* the bus clock must carry whole samples */
	if (l->mprops.max_clk_freq % rate)
		return 0;
	if ((l->params.curr_dr_freq / 2) % rate)
		return 0;
	return 1;
}

static void stream_reset(struct sdw_stream_rt *st)
{
	uint32_t gen = st->generation;

	kmemset(st, 0, sizeof(*st));
	st->state = SDW_STREAM_IDLE;
	st->pdi = -1;
	st->alh_id = -1;
	st->deferred_on = -1;
	st->generation = gen + 1;
}

/* Stream slots.  A link carries up to SDW_LINK_STREAMS streams; the CS42L43
 * for instance takes the jack and the speaker playback on one link in the
 * same direction.  The contract names a stream by (link, direction) only;
 * the audio function the DSP side sets in sdev->sdw_prep_function picks
 * between several of one direction.  Without it, a single stream of the
 * direction is meant, and with several, all of them. */
static int find_slot(struct sdw_link *l, int dir, int fn)
{
	for (int i = 0; i < SDW_LINK_STREAMS; i++) {
		struct sdw_stream_rt *st = &l->streams[i];
		if (st->state != SDW_STREAM_IDLE && st->dir == dir &&
		    st->fn == fn)
			return i;
	}
	return -1;
}

/* Slots meant by (dir, fn) for enable/release: a bit mask. */
static uint32_t select_slots(struct sdw_link *l, int dir, int fn)
{
	uint32_t all = 0, match = 0;

	for (int i = 0; i < SDW_LINK_STREAMS; i++) {
		struct sdw_stream_rt *st = &l->streams[i];
		if (st->state == SDW_STREAM_IDLE || st->dir != dir)
			continue;
		all |= 1u << i;
		if (fn && st->fn == fn)
			match |= 1u << i;
	}
	if (fn)
		return match;
	return all;
}

int sdw_stream_prepare(struct sof_dev *sdev, int link, int direction,
		       uint32_t rate, uint32_t channels, uint32_t bits,
		       uint32_t peripheral_mask)
{
	struct sdw_link *l = link_of(sdev, link);
	struct sdw_stream_rt *st;
	uint32_t bps;
	int pdi, alh, rc, slot, fn;

	if (!l)
		return -ENODEV;
	if (direction != AUDIO_DIR_PLAYBACK && direction != AUDIO_DIR_CAPTURE)
		return -EINVAL;
	if (!channels || channels > 8 || bits < 8 || bits > 32)
		return -EINVAL;
	if (!valid_rate(l, rate)) {
		sdw_info("link %d: %u Hz does not divide the bus clock\n", l->id,
			 rate);
		return -EINVAL;
	}
	if (!sdw_can_sleep() && sched_current())
		return -EAGAIN;
	fn = sdev->sdw_prep_function;
	if (fn != SOF_SDW_FN_JACK && fn != SOF_SDW_FN_AMP &&
	    fn != SOF_SDW_FN_MIC)
		fn = 0;
	if (stream_lock_wait(l, 2000))
		return -EBUSY;
	/* a second prepare of the same stream starts it over */
	slot = find_slot(l, direction, fn);
	if (slot >= 0)
		do_release_idx(l, slot);
	for (int i = 0; slot < 0 && i < SDW_LINK_STREAMS; i++)
		if (l->streams[i].state == SDW_STREAM_IDLE)
			slot = i;
	if (slot < 0) {
		stream_unlock(l);
		return -EBUSY;
	}
	st = &l->streams[slot];
	stream_reset(st);
	st->dir = direction;
	st->fn = fn;
	st->rate = rate;
	st->ch = channels;
	st->bits = bits;
	bps = bits;

	/* the peripherals taking part */
	for (int i = 0; i < l->nslaves && st->nslave < SDW_STREAM_MAX_SLAVES;
	     i++) {
		struct sdw_slave *s = &l->slaves[i];
		struct sdw_port_rt *sp;
		const struct sdw_dpn_props *dp;
		int port, busy = 0;

		if (peripheral_mask &&
		    !(peripheral_mask & (1u << s->periph_idx)))
			continue;
		if (s->status == SDW_SLAVE_UNATTACHED || !s->ops ||
		    !s->ops->port_for)
			continue;
		port = s->ops->port_for(s, direction, fn);
		if (port < 1 || port >= SDW_MAX_PORTS)
			continue;
		/* one stream per peripheral data port */
		for (int k = 0; k < SDW_LINK_STREAMS; k++) {
			struct sdw_stream_rt *o = &l->streams[k];
			if (k == slot || o->state == SDW_STREAM_IDLE)
				continue;
			for (int j = 0; j < o->nslave; j++)
				if (o->sl[j].s == s && o->sl[j].port.num == port)
					busy = 1;
		}
		if (busy) {
			sdw_info("%s: port %d already streams\n", s->name, port);
			continue;
		}
		dp = &s->props.dp[port];
		sp = &st->sl[st->nslave].port;
		st->sl[st->nslave].s = s;
		kmemset(sp, 0, sizeof(*sp));
		sp->num = port;
		{
			uint32_t ch = channels;
			if (dp->max_ch && ch > dp->max_ch)
				ch = dp->max_ch;
			sp->ch_mask = (1u << ch) - 1;
		}
		if (dp->max_word && bps > dp->max_word)
			bps = dp->max_word;
		st->nslave++;
	}
	if (!st->nslave)
		sdw_info("link %d: %s stream without a peripheral\n", l->id,
			 direction ? "capture" : "playback");
	if (bps < 8)
		bps = 8;
	st->bps = bps;

	rc = sdw_intel_pdi_alloc(l, direction, channels, &pdi, &alh);
	if (rc) {
		sdw_info("link %d: no free PDI for %u channels\n", l->id,
			 channels);
		stream_reset(st);
		stream_unlock(l);
		return rc;
	}
	st->pdi = pdi;
	st->alh_id = alh;
	sdw_intel_pdi_config(l, pdi, direction, channels, alh, st->stream_tag);

	/* tell the DSP which gateway carries this stream */
	rc = sof_dai_sdw_config(sdev, link, alh, rate, channels, bits);
	if (rc < 0) {
		sdw_info("link %d: DSP refused gateway %d (%d)\n", l->id, alh,
			 rc);
		sdw_intel_pdi_free(l, pdi);
		stream_reset(st);
		stream_unlock(l);
		return rc;
	}

	/* the codecs: clocks, paths, sample rate */
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->ops->prepare) {
			rc = s->ops->prepare(s, direction, st->sl[i].port.num,
					     rate, channels, bits);
			if (rc)
				sdw_info("%s: codec prepare failed (%d)\n",
					 s->name, rc);
		}
	}

	st->state = SDW_STREAM_CONFIGURED;
	rc = compute_params(l);
	if (rc)
		goto fail;
	if (program_params(l))
		sdw_info("link %d: parameters partly not taken\n", l->id);
	rc = bank_switch(l);
	if (rc)
		goto fail;
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->status == SDW_SLAVE_UNATTACHED)
			continue;
		if (slave_port_prepare(s, &st->sl[i].port, 1))
			sdw_info("%s: channel prepare failed\n", s->name);
	}
	st->state = SDW_STREAM_PREPARED;
	stream_unlock(l);
	return 0;

fail:
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->ops->release)
			s->ops->release(s, direction, st->sl[i].port.num);
	}
	sdw_intel_pdi_free(l, st->pdi);
	stream_reset(st);
	compute_params(l);
	stream_unlock(l);
	return rc;
}

/* Enable/disable slot `idx' with the stream flag held. */
static int do_enable(struct sdw_link *l, int idx, int on)
{
	struct sdw_stream_rt *st = &l->streams[idx];
	int rc;

	if (on) {
		if (st->state == SDW_STREAM_ENABLED)
			return 0;
		if (st->state != SDW_STREAM_PREPARED &&
		    st->state != SDW_STREAM_DISABLED)
			return -EINVAL;
		st->state = SDW_STREAM_ENABLED;
		program_params(l);
		rc = bank_switch(l);
		if (rc) {
			st->state = SDW_STREAM_DISABLED;
			return rc;
		}
		for (int i = 0; i < st->nslave; i++) {
			struct sdw_slave *s = st->sl[i].s;
			if (s->ops->trigger)
				s->ops->trigger(s, st->dir, st->sl[i].port.num,
						1);
		}
		return 0;
	}
	if (st->state != SDW_STREAM_ENABLED)
		return 0;
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->ops->trigger)
			s->ops->trigger(s, st->dir, st->sl[i].port.num, 0);
	}
	st->state = SDW_STREAM_DISABLED;
	program_params(l);
	rc = bank_switch(l);
	/* the bank that was active keeps the old enables: clear them */
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->status != SDW_SLAVE_UNATTACHED)
			slave_port_enable(s, &st->sl[i].port,
					  l->params.next_bank, 0);
	}
	sdw_cdns_dpn_enable(l, l->params.next_bank, st->mport.num, 0);
	return rc;
}

static void stream_deferred_work(void *arg)
{
	struct sdw_link *l = arg;

	if (!l->ctx->stopping && !stream_lock_wait(l, 2000)) {
		for (int d = 0; d < SDW_LINK_STREAMS; d++) {
			int want = l->streams[d].deferred_on;
			l->streams[d].deferred_on = -1;
			if (want >= 0)
				do_enable(l, d, want);
		}
		stream_unlock(l);
	}
	__atomic_store_n(&l->stream_work_queued, 0, __ATOMIC_RELEASE);
}

int sdw_stream_enable(struct sof_dev *sdev, int link, int direction, int on)
{
	struct sdw_link *l = link_of(sdev, link);
	uint32_t slots;
	int rc = 0;

	if (!l)
		return -ENODEV;
	if (direction != AUDIO_DIR_PLAYBACK && direction != AUDIO_DIR_CAPTURE)
		return -EINVAL;
	on = on ? 1 : 0;
	if (sdw_can_sleep()) {
		if (stream_lock_wait(l, 2000))
			return -EBUSY;
	} else if (!stream_try_lock(l)) {
		/* called with interrupts off while a sequence runs: the
		 * work thread finishes the job right after it */
		slots = select_slots(l, direction, sdev->sdw_prep_function);
		if (!slots)
			return -ENOENT;
		for (int i = 0; i < SDW_LINK_STREAMS; i++)
			if (slots & (1u << i))
				l->streams[i].deferred_on = on;
		if (!__atomic_exchange_n(&l->stream_work_queued, 1,
					 __ATOMIC_ACQ_REL)) {
			if (snd_schedule_work(stream_deferred_work, l, 0)) {
				__atomic_store_n(&l->stream_work_queued, 0,
						 __ATOMIC_RELEASE);
				return -EBUSY;
			}
		}
		return 0;
	}
	slots = select_slots(l, direction, sdev->sdw_prep_function);
	if (!slots) {
		stream_unlock(l);
		return -ENOENT;
	}
	for (int i = 0; i < SDW_LINK_STREAMS; i++) {
		int r;
		if (!(slots & (1u << i)))
			continue;
		l->streams[i].deferred_on = -1;
		r = do_enable(l, i, on);
		if (r)
			rc = r;
	}
	stream_unlock(l);
	return rc;
}

static int do_release_idx(struct sdw_link *l, int idx)
{
	struct sdw_stream_rt *st = &l->streams[idx];
	int rc = 0, dir = st->dir;

	if (st->state == SDW_STREAM_IDLE)
		return 0;
	if (!l->up || l->dead) {
		/* no hardware: only the bookkeeping */
		for (int i = 0; i < st->nslave; i++) {
			struct sdw_slave *s = st->sl[i].s;
			if (s && s->ops && s->ops->release)
				s->ops->release(s, dir, st->sl[i].port.num);
		}
		if (st->pdi >= 0)
			sdw_intel_pdi_free(l, st->pdi);
		stream_reset(st);
		return 0;
	}
	if (st->state == SDW_STREAM_ENABLED)
		do_enable(l, idx, 0);
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->status != SDW_SLAVE_UNATTACHED &&
		    st->state != SDW_STREAM_CONFIGURED)
			slave_port_prepare(s, &st->sl[i].port, 0);
	}
	for (int i = 0; i < st->nslave; i++) {
		struct sdw_slave *s = st->sl[i].s;
		if (s->ops->release)
			s->ops->release(s, dir, st->sl[i].port.num);
	}
	sdw_intel_pdi_free(l, st->pdi);
	stream_reset(st);
	/* the remaining streams keep their places; refresh both banks */
	if (!compute_params(l)) {
		program_params(l);
		rc = bank_switch(l);
		if (!rc)
			program_params(l);
	}
	return rc;
}

int sdw_stream_release(struct sof_dev *sdev, int link, int direction)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	struct sdw_link *l;
	uint32_t slots;
	int rc = 0;

	if (!c || link < 0 || link >= SDW_MAX_LINKS)
		return -ENODEV;
	if (direction != AUDIO_DIR_PLAYBACK && direction != AUDIO_DIR_CAPTURE)
		return -EINVAL;
	l = &c->link[link];
	if (stream_lock_wait(l, 2000))
		return -EBUSY;
	slots = select_slots(l, direction, sdev->sdw_prep_function);
	for (int i = 0; i < SDW_LINK_STREAMS; i++) {
		int r;
		if (!(slots & (1u << i)))
			continue;
		r = do_release_idx(l, i);
		if (r)
			rc = r;
	}
	stream_unlock(l);
	return rc;
}

/* ==========================================================================
 * Card-level controls
 * ========================================================================== */

static int for_each_slave(struct sdw_ctx *c,
			  int (*fn)(struct sdw_slave *s, void *arg), void *arg)
{
	int n = 0;

	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->up || l->dead)
			continue;
		for (int j = 0; j < l->nslaves; j++) {
			struct sdw_slave *s = &l->slaves[j];
			if (!s->initialized || s->in_init ||
			    s->status == SDW_SLAVE_UNATTACHED || !s->ops)
				continue;
			if (fn(s, arg) == 0)
				n++;
		}
	}
	return n;
}

static uint32_t caps_all(struct sdw_ctx *c)
{
	uint32_t caps = 0;

	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];
		if (!l->enabled)
			continue;
		for (int j = 0; j < l->nslaves; j++)
			caps |= l->slaves[j].caps;
	}
	return caps;
}

static int ctl_supported(struct sdw_ctx *c, uint32_t ctl)
{
	uint32_t caps = caps_all(c);

	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		return (caps & (SDW_CAP_SPEAKER | SDW_CAP_HEADPHONE)) != 0;
	case AUDIO_CTL_SPEAKER:
		return (caps & SDW_CAP_SPEAKER) != 0;
	case AUDIO_CTL_HEADPHONE:
		return (caps & SDW_CAP_HEADPHONE) != 0;
	case AUDIO_CTL_CAPTURE:
	case AUDIO_CTL_MIC_BOOST:
		return (caps & (SDW_CAP_HEADSET_MIC | SDW_CAP_DMIC)) != 0;
	default:
		return 0;
	}
}

struct vol_arg {
	uint32_t ctl, l, r, mute;
	struct audio_volume *out;
	int got;
};

static int slave_set_vol(struct sdw_slave *s, void *arg)
{
	struct vol_arg *a = arg;
	return s->ops->set_volume ? s->ops->set_volume(s, a->ctl, a->l, a->r) :
				    -ENOENT;
}

static int slave_get_vol(struct sdw_slave *s, void *arg)
{
	struct vol_arg *a = arg;
	int rc;

	if (a->got || !s->ops->get_volume)
		return -ENOENT;
	rc = s->ops->get_volume(s, a->ctl, a->out);
	if (!rc)
		a->got = 1;
	return rc;
}

static int slave_set_mute(struct sdw_slave *s, void *arg)
{
	struct vol_arg *a = arg;
	return s->ops->set_mute ? s->ops->set_mute(s, a->ctl, a->mute) :
				  -ENOENT;
}

/* Re-apply the stored controls to a codec that (re)initialised. */
static void apply_saved_controls(struct sdw_slave *s)
{
	struct sdw_ctx *c = s->link->ctx;
	static const uint32_t ctls[] = { AUDIO_CTL_MASTER, AUDIO_CTL_SPEAKER,
					 AUDIO_CTL_HEADPHONE, AUDIO_CTL_CAPTURE };

	for (unsigned i = 0; i < sizeof(ctls) / sizeof(ctls[0]); i++) {
		uint32_t ctl = ctls[i];
		if (s->ops->set_volume)
			s->ops->set_volume(s, ctl, c->vol[ctl][0],
					   c->vol[ctl][1]);
		if (s->ops->set_mute)
			s->ops->set_mute(s, ctl, c->mute[ctl]);
	}
	if (s->ops->set_routing)
		s->ops->set_routing(s, c->spk_active, c->hp_active);
}

int sdw_codec_get_volume(struct sof_dev *sdev, uint32_t ctl,
			 struct audio_volume *v)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	struct vol_arg a;

	if (!c)
		return -ENODEV;
	if (!v || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	if (!ctl_supported(c, ctl))
		return -ENOENT;
	kmemset(&a, 0, sizeof(a));
	a.ctl = ctl;
	a.out = v;
	kmemset(v, 0, sizeof(*v));
	v->control = ctl;
	if (snd_mutex_lock(&c->ctl_lock, 1))
		return -ERESTARTSYS;
	if (ctl != AUDIO_CTL_MASTER && ctl != AUDIO_CTL_PCM)
		for_each_slave(c, slave_get_vol, &a);
	if (!a.got) {
		/* software state */
		v->control = ctl;
		v->left = c->vol[ctl][0];
		v->right = c->vol[ctl][1];
		v->steps = 0;
	}
	snd_mutex_unlock(&c->ctl_lock);
	return 0;
}

int sdw_codec_set_volume(struct sof_dev *sdev, uint32_t ctl, uint32_t l,
			 uint32_t r)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	struct vol_arg a;
	int n;

	if (!c)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	if (!ctl_supported(c, ctl))
		return -ENOENT;
	if (l > 100)
		l = 100;
	if (r > 100)
		r = 100;
	if (snd_mutex_lock(&c->ctl_lock, 1))
		return -ERESTARTSYS;
	c->vol[ctl][0] = l;
	c->vol[ctl][1] = r;
	snd_mutex_unlock(&c->ctl_lock);
	kmemset(&a, 0, sizeof(a));
	a.ctl = ctl;
	a.l = l;
	a.r = r;
	n = for_each_slave(c, slave_set_vol, &a);
	if (!n && ctl != AUDIO_CTL_MASTER && ctl != AUDIO_CTL_PCM)
		return -EIO;
	return 0;
}

int sdw_codec_get_mute(struct sof_dev *sdev, uint32_t ctl, uint32_t *mute)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);

	if (!c)
		return -ENODEV;
	if (!mute || ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	if (!ctl_supported(c, ctl))
		return -ENOENT;
	*mute = c->mute[ctl];
	return 0;
}

int sdw_codec_set_mute(struct sof_dev *sdev, uint32_t ctl, uint32_t mute)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	struct vol_arg a;

	if (!c)
		return -ENODEV;
	if (ctl > AUDIO_CTL_MAX)
		return -EINVAL;
	if (!ctl_supported(c, ctl))
		return -ENOENT;
	if (snd_mutex_lock(&c->ctl_lock, 1))
		return -ERESTARTSYS;
	c->mute[ctl] = mute ? 1 : 0;
	snd_mutex_unlock(&c->ctl_lock);
	kmemset(&a, 0, sizeof(a));
	a.ctl = ctl;
	a.mute = mute ? 1 : 0;
	for_each_slave(c, slave_set_mute, &a);
	return 0;
}

static int hp_present(struct sdw_ctx *c)
{
	return (c->jack & (SDW_JACK_HEADPHONE | SDW_JACK_LINEOUT)) != 0;
}

struct route_arg {
	int spk, hp;
};

static int slave_route(struct sdw_slave *s, void *arg)
{
	struct route_arg *a = arg;
	return s->ops->set_routing ? s->ops->set_routing(s, a->spk, a->hp) :
				     -ENOENT;
}

static void apply_routing(struct sdw_ctx *c)
{
	uint32_t caps = caps_all(c);
	struct route_arg a;
	int changed;

	if (!c || c->stopping)
		return;
	if (snd_mutex_lock(&c->ctl_lock, 0))
		return;
	a.spk = 0;
	a.hp = 0;
	switch (c->out_mode) {
	case AUDIO_OUTSEL_ID:
		if (c->out_selected == SDW_OUT_ID_HEADPHONE)
			a.hp = 1;
		else
			a.spk = 1;
		break;
	case AUDIO_OUTSEL_ALL:
		a.spk = 1;
		a.hp = (caps & SDW_CAP_JACK_DETECT) ? hp_present(c) : 1;
		break;
	default: /* AUDIO_OUTSEL_AUTO */
		if ((caps & SDW_CAP_JACK_DETECT) && hp_present(c))
			a.hp = 1;
		else if (!(caps & SDW_CAP_SPEAKER) &&
			 (caps & SDW_CAP_HEADPHONE))
			a.hp = 1; /* no speaker at all */
		else
			a.spk = 1;
		break;
	}
	if (!(caps & SDW_CAP_SPEAKER))
		a.spk = 0;
	if (!(caps & SDW_CAP_HEADPHONE))
		a.hp = 0;
	changed = a.spk != c->spk_active || a.hp != c->hp_active;
	c->spk_active = a.spk;
	c->hp_active = a.hp;
	snd_mutex_unlock(&c->ctl_lock);
	for_each_slave(c, slave_route, &a);
	if (changed && c->sdev->card)
		snd_card_event(c->sdev->card, AUDIO_EV_OUTPUT,
			       a.hp ? SDW_OUT_ID_HEADPHONE : SDW_OUT_ID_SPEAKER,
			       (uint32_t)(a.spk | (a.hp << 1)));
}

static void routing_work(void *arg)
{
	struct sdw_ctx *c = arg;

	__atomic_store_n(&c->routing_work_queued, 0, __ATOMIC_RELEASE);
	if (!c->stopping)
		apply_routing(c);
}

void sdw_report_jack(struct sdw_slave *s, uint32_t jack, uint32_t mask)
{
	struct sdw_ctx *c;
	uint32_t before, after;

	if (!s || !s->link)
		return;
	c = s->link->ctx;
	before = c->jack;
	after = (before & ~mask) | (jack & mask);
	c->jack = after;
	if (((before ^ after) & (SDW_JACK_HEADPHONE | SDW_JACK_LINEOUT)) &&
	    c->sdev->card)
		snd_jack_report(c->sdev->card, SDW_OUT_ID_HEADPHONE,
				(after & (SDW_JACK_HEADPHONE | SDW_JACK_LINEOUT)) ?
					1 : 0);
	if (before != after) {
		sdw_info("%s: jack %s%s%s\n", s->name,
			 (after & SDW_JACK_HEADPHONE) ? "headphone " : "",
			 (after & SDW_JACK_MIC) ? "microphone " : "",
			 (after & (SDW_JACK_HEADPHONE | SDW_JACK_MIC |
				   SDW_JACK_LINEOUT)) ?
				 "plugged" :
				 "unplugged");
		if (!__atomic_exchange_n(&c->routing_work_queued, 1,
					 __ATOMIC_ACQ_REL)) {
			if (snd_schedule_work(routing_work, c, 0))
				__atomic_store_n(&c->routing_work_queued, 0,
						 __ATOMIC_RELEASE);
		}
	}
}

int sdw_codec_get_outputs(struct sof_dev *sdev, struct audio_outputs *o)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	uint32_t caps;

	if (!o)
		return -EINVAL;
	if (!c)
		return -ENODEV;
	if (o->count > AUDIO_MAX_OUTPUTS)
		o->count = 0;
	caps = caps_all(c);
	if (snd_mutex_lock(&c->ctl_lock, 1))
		return -ERESTARTSYS;
	if ((caps & SDW_CAP_SPEAKER) && o->count < AUDIO_MAX_OUTPUTS) {
		struct audio_output_desc *d = &o->out[o->count++];
		kmemset(d, 0, sizeof(*d));
		d->id = SDW_OUT_ID_SPEAKER;
		d->type = AUDIO_OUT_SPEAKER;
		d->flags = AUDIO_OUTF_FIXED;
		d->present = 1;
		d->active = (uint32_t)c->spk_active;
		ksnprintf(d->name, sizeof(d->name), "Speaker");
	}
	if ((caps & SDW_CAP_HEADPHONE) && o->count < AUDIO_MAX_OUTPUTS) {
		struct audio_output_desc *d = &o->out[o->count++];
		kmemset(d, 0, sizeof(*d));
		d->id = SDW_OUT_ID_HEADPHONE;
		d->type = (c->jack & SDW_JACK_LINEOUT) ? AUDIO_OUT_LINE :
			  (c->jack & SDW_JACK_MIC)	? AUDIO_OUT_HEADSET :
							  AUDIO_OUT_HEADPHONE;
		d->flags = (caps & SDW_CAP_JACK_DETECT) ? AUDIO_OUTF_JACK_DETECT :
							  0;
		d->present = (caps & SDW_CAP_JACK_DETECT) ? (uint32_t)hp_present(c) :
							    1;
		d->active = (uint32_t)c->hp_active;
		ksnprintf(d->name, sizeof(d->name),
			  (c->jack & SDW_JACK_LINEOUT) ? "Line Out" :
							 "Headphone");
	}
	o->mode = c->out_mode;
	o->selected = c->out_selected;
	snd_mutex_unlock(&c->ctl_lock);
	return 0;
}

int sdw_codec_set_output(struct sof_dev *sdev, const struct audio_setoutput *s)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	uint32_t caps;

	if (!c)
		return -ENODEV;
	if (!s)
		return -EINVAL;
	caps = caps_all(c);
	switch (s->mode) {
	case AUDIO_OUTSEL_AUTO:
	case AUDIO_OUTSEL_ALL:
		break;
	case AUDIO_OUTSEL_ID:
		if (s->id == SDW_OUT_ID_SPEAKER && (caps & SDW_CAP_SPEAKER))
			break;
		if (s->id == SDW_OUT_ID_HEADPHONE &&
		    (caps & SDW_CAP_HEADPHONE))
			break;
		return -ENOENT;
	default:
		return -EINVAL;
	}
	if (snd_mutex_lock(&c->ctl_lock, 1))
		return -ERESTARTSYS;
	c->out_mode = s->mode;
	c->out_selected = s->mode == AUDIO_OUTSEL_ID ? s->id : 0;
	snd_mutex_unlock(&c->ctl_lock);
	apply_routing(c);
	return 0;
}

/* ==========================================================================
 * Dump
 * ========================================================================== */

static const char *status_name(int st)
{
	switch (st) {
	case SDW_SLAVE_ATTACHED:
		return "attached";
	case SDW_SLAVE_ALERT:
		return "alert";
	case SDW_SLAVE_RESERVED:
		return "reserved";
	default:
		return "unattached";
	}
}

static const char *stream_state_name(int st)
{
	switch (st) {
	case SDW_STREAM_CONFIGURED:
		return "configured";
	case SDW_STREAM_PREPARED:
		return "prepared";
	case SDW_STREAM_ENABLED:
		return "enabled";
	case SDW_STREAM_DISABLED:
		return "disabled";
	default:
		return "idle";
	}
}

long sdw_dump(struct sof_dev *sdev, char *buf, size_t len)
{
	struct sdw_ctx *c = sdw_ctx_of(sdev);
	size_t pos = 0;
	long n;

	if (!buf || !len)
		return 0;
	buf[0] = 0;
	if (!c) {
		pos = sdw_dump_printf(buf, len, pos, "SoundWire: not running\n");
		return (long)pos;
	}
	n = sdw_intel_dump(c, buf + pos, len - pos);
	if (n > 0)
		pos += (size_t)n;
	pos = sdw_dump_printf(buf, len, pos,
			      "outputs: mode %u speaker %d headphone %d jack 0x%x polls %u\n",
			      c->out_mode, c->spk_active, c->hp_active, c->jack,
			      c->poll_count);
	for (int i = 0; i < SDW_MAX_LINKS; i++) {
		struct sdw_link *l = &c->link[i];

		if (!l->enabled)
			continue;
		pos = sdw_dump_printf(buf, len, pos,
				      "link %d: slave status 0x%08x, bandwidth %u bit/s, %u enumerations, %u unknown\n",
				      i, l->last_slave_stat, l->params.bandwidth,
				      l->st_enum, l->st_unknown_dev);
		for (int j = 0; j < l->nslaves; j++) {
			struct sdw_slave *s = &l->slaves[j];
			pos = sdw_dump_printf(buf, len, pos,
					      "  %s [%s] dev %d %s%s%s caps 0x%x attach %u alerts %u io-errors %u id %04x%08x\n",
					      s->name,
					      s->ops ? s->ops->name : "-",
					      s->dev_num,
					      status_name(s->status),
					      s->initialized ? " ready" : "",
					      s->in_init ? " (initialising)" : "",
					      s->caps, s->attach_count,
					      s->alert_count, s->io_errors,
					      (uint32_t)(s->id48 >> 32),
					      (uint32_t)s->id48);
			if (s->ops && s->ops->dump && pos + 1 < len) {
				n = s->ops->dump(s, buf + pos, len - pos);
				if (n > 0)
					pos += (size_t)n;
				if (pos >= len)
					pos = len - 1;
			}
		}
		for (int d = 0; d < SDW_LINK_STREAMS; d++) {
			struct sdw_stream_rt *st = &l->streams[d];
			if (st->state == SDW_STREAM_IDLE)
				continue;
			pos = sdw_dump_printf(buf, len, pos,
					      "  %s stream: %s %u Hz %u ch %u bit (bus %u), PDI %d ALH %d, cols %u-%u offset %u\n",
					      d ? "capture" : "playback",
					      stream_state_name(st->state),
					      st->rate, st->ch, st->bits,
					      st->bps, st->pdi, st->alh_id,
					      st->mport.hstart, st->mport.hstop,
					      st->mport.offset1 |
						      (st->mport.offset2 << 8));
			for (int k = 0; k < st->nslave; k++)
				pos = sdw_dump_printf(buf, len, pos,
						      "    -> %s port %d mask 0x%x offset %u\n",
						      st->sl[k].s->name,
						      st->sl[k].port.num,
						      st->sl[k].port.ch_mask,
						      st->sl[k].port.offset1 |
							      (st->sl[k].port.offset2
							       << 8));
		}
	}
	return (long)pos;
}

/* ==========================================================================
 * Generic handling for peripherals without a driver
 * ========================================================================== */

static int null_match(const struct sof_sdw_peripheral *p)
{
	(void)p;
	return 1;
}

static int null_probe(struct sdw_slave *s)
{
	s->caps = 0;
	if (s->props.sink_ports)
		s->caps |= SDW_CAP_PLAYBACK;
	if (s->props.source_ports)
		s->caps |= SDW_CAP_CAPTURE;
	return 0;
}

static int null_port_for(struct sdw_slave *s, int dir, int fn)
{
	(void)fn;
	uint32_t ports = dir == AUDIO_DIR_PLAYBACK ? s->props.sink_ports :
						     s->props.source_ports;

	for (int p = 1; p < SDW_MAX_PORTS; p++)
		if (ports & (1u << p))
			return p;
	return -ENOENT;
}

const struct sdw_codec_ops sdw_null_ops = {
	.name = "generic",
	.match = null_match,
	.probe = null_probe,
	.port_for = null_port_for,
};
