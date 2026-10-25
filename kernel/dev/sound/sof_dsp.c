// LikeOS Intel audio DSP host driver - the hardware layer: the HDA side of
// the controller (reset, extended capabilities, multi-link, DMA engines),
// the DSP's power and core control for cAVS 2.5 and ACE 1.x-3.x, interrupt
// enables, firmware status decoding and crash dumps.
//
// Everything here runs either from process context (probe, boot, PCM
// setup) or with interrupts off (trigger, interrupt handler); the waits
// therefore come in two flavours: they sleep when the scheduler allows it
// and busy-wait otherwise, with the same time budget.
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"

#include <kernel/hal/lapic.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>

/* Set once the scheduler runs (sof_late_init); before that every wait
 * busy-waits, as snd_init() requires. */
int g_sof_can_block;

static struct wait_queue_head g_sof_nap_wq;

/* ===================================================================== */
/* time and waiting                                                       */
/* ===================================================================== */

uint64_t sof_now_us(void)
{
	return timer_get_precise_us();
}

int sof_can_sleep(void)
{
	return g_sof_can_block && sched_current() != NULL && irqs_enabled();
}

/* Block the current task for about `ticks' timer ticks.  The queue is
 * never woken: the deadline does it. */
static void sof_nap_ticks(uint64_t ticks)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	uint64_t fl;

	if (!cur)
		return;
	wq_head_init_once(&g_sof_nap_wq, "sof-nap");
	if (ticks == 0)
		ticks = 1;
	fl = local_irq_save();
	wq_entry_init(&we, cur);
	wq_add(&g_sof_nap_wq, &we);
	cur->wait_channel = &g_sof_nap_wq;
	cur->wakeup_tick = timer_ticks() + ticks;
	task_set_blocked(cur);
	local_irq_restore(fl);
	sched_schedule();
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(&g_sof_nap_wq, &we);
}

void sof_wait_us(uint32_t us)
{
	if (us >= 2000 && sof_can_sleep()) {
		sof_nap_ticks(timer_us_to_ticks(us));
		return;
	}
	while (us >= 1000) {
		lapic_delay_us(1000);
		us -= 1000;
	}
	if (us)
		lapic_delay_us(us);
}

/* A time budget that works with and without a running clock: busy waits
 * count what they waited, sleeps measure it. */
struct sof_budget {
	uint64_t start;
	uint64_t spent;
	uint32_t timeout;
	uint32_t polls;
};

static void sof_budget_init(struct sof_budget *b, uint32_t timeout_us)
{
	b->start = sof_now_us();
	b->spent = 0;
	b->timeout = timeout_us;
	b->polls = 0;
}

/* Wait a little; returns 1 while time remains. */
static int sof_budget_step(struct sof_budget *b)
{
	uint64_t measured;

	b->polls++;
	if (b->polls > 64 && sof_can_sleep()) {
		sof_nap_ticks(1);
	} else {
		uint32_t d = b->polls < 16 ? 5 : 20;
		lapic_delay_us(d);
		b->spent += d;
	}
	measured = sof_now_us() - b->start;
	if (measured > b->spent)
		b->spent = measured;
	return b->spent < b->timeout;
}

int sof_poll_hda32(struct sof_host *h, uint32_t off, uint32_t mask,
		   uint32_t want, uint32_t timeout_us, uint32_t *last)
{
	struct sof_budget b;
	uint32_t v;

	sof_budget_init(&b, timeout_us);
	for (;;) {
		v = sof_hda_r32(h, off);
		if ((v & mask) == want)
			break;
		if (!sof_budget_step(&b)) {
			v = sof_hda_r32(h, off);
			if ((v & mask) == want)
				break;
			if (last)
				*last = v;
			return -ETIMEDOUT;
		}
	}
	if (last)
		*last = v;
	return 0;
}

int sof_poll_dsp32(struct sof_host *h, uint32_t off, uint32_t mask,
		   uint32_t want, uint32_t timeout_us, uint32_t *last)
{
	struct sof_budget b;
	uint32_t v;

	sof_budget_init(&b, timeout_us);
	for (;;) {
		v = sof_dsp_r32(h, off);
		if ((v & mask) == want)
			break;
		if (!sof_budget_step(&b)) {
			v = sof_dsp_r32(h, off);
			if ((v & mask) == want)
				break;
			if (last)
				*last = v;
			return -ETIMEDOUT;
		}
	}
	if (last)
		*last = v;
	return 0;
}

void sof_mutex_init(struct sof_mutex *m, const char *name)
{
	m->locked = 0;
	spinlock_init(&m->lock, name);
}

void sof_mutex_lock(struct sof_mutex *m)
{
	for (;;) {
		uint64_t fl;

		spin_lock_irqsave(&m->lock, &fl);
		if (!m->locked) {
			m->locked = 1;
			spin_unlock_irqrestore(&m->lock, fl);
			return;
		}
		spin_unlock_irqrestore(&m->lock, fl);
		if (sof_can_sleep())
			sched_yield_in_kernel();
		else
			lapic_delay_us(10);
	}
}

void sof_mutex_unlock(struct sof_mutex *m)
{
	uint64_t fl;

	spin_lock_irqsave(&m->lock, &fl);
	m->locked = 0;
	spin_unlock_irqrestore(&m->lock, fl);
}

/* ===================================================================== */
/* text helpers                                                           */
/* ===================================================================== */

size_t sof_append(char *buf, size_t len, size_t pos, const char *fmt, ...)
{
	__builtin_va_list ap;

	if (!buf || pos + 1 >= len)
		return pos;
	__builtin_va_start(ap, fmt);
	kvsnprintf(buf + pos, len - pos, fmt, ap);
	__builtin_va_end(ap);
	pos += kstrlen(buf + pos);
	if (pos >= len)
		pos = len - 1;
	return pos;
}

void sof_strlcpy(char *dst, const char *src, size_t n)
{
	size_t i = 0;

	if (!n)
		return;
	if (src) {
		for (; i + 1 < n && src[i]; i++)
			dst[i] = src[i];
	}
	dst[i] = 0;
}

/* ===================================================================== */
/* HDA controller                                                         */
/* ===================================================================== */

int sof_hda_enum_caps(struct sof_host *h)
{
	uint32_t off, cap;
	int n = 0;

	h->ppcap = h->spbcap = h->mlcap = h->gtscap = h->drsmcap = 0;
	off = sof_hda_r32(h, SOF_HDA_LLCH) & SOF_CAP_NEXT_MASK;
	while (off && n++ < SOF_CAP_MAX_WALK) {
		uint32_t id;

		/* a capability lies inside the mapped window and above the
		 * standard register block */
		if (off < 0x100 || off + 0x40 > h->bar0_map_size || (off & 3)) {
			sof_log(h, "bogus capability pointer %#x\n", off);
			break;
		}
		cap = sof_hda_r32(h, off);
		if (cap == 0xffffffffu)
			break;
		id = (cap & SOF_CAP_ID_MASK) >> SOF_CAP_ID_SHIFT;
		switch (id) {
		case SOF_CAP_ID_PP:
			h->ppcap = off;
			break;
		case SOF_CAP_ID_SPB:
			h->spbcap = off;
			break;
		case SOF_CAP_ID_DRSM:
			h->drsmcap = off;
			break;
		case SOF_CAP_ID_GTS:
			h->gtscap = off;
			break;
		case SOF_CAP_ID_ML:
			h->mlcap = off;
			break;
		default:
			break;
		}
		if ((cap & SOF_CAP_NEXT_MASK) == off)
			break; /* a loop */
		off = cap & SOF_CAP_NEXT_MASK;
	}
	return h->ppcap ? 0 : -ENODEV;
}

int sof_hda_link_reset(struct sof_host *h, int enter)
{
	uint32_t want = enter ? 0 : SOF_HDA_GCTL_CRST;
	uint32_t last = 0;
	int rc;

	sof_hda_upd32(h, SOF_HDA_GCTL, SOF_HDA_GCTL_CRST, want);
	rc = sof_poll_hda32(h, SOF_HDA_GCTL, SOF_HDA_GCTL_CRST, want, 100000,
			    &last);
	if (rc)
		sof_log(h, "controller failed to %s reset (GCTL %#x)\n",
			enter ? "enter" : "leave", last);
	return rc;
}

static void sof_misc_clock_gating(struct sof_host *h, int enable)
{
	uint32_t v = pci_cfg_read32_dev(h->s.pci, SOF_PCI_CGCTL);

	if (enable)
		v |= SOF_PCI_CGCTL_MISCBDCGE;
	else
		v &= ~SOF_PCI_CGCTL_MISCBDCGE;
	pci_cfg_write32_dev(h->s.pci, SOF_PCI_CGCTL, v);
}

/* Bring the HDA controller to a defined state: reset it, enumerate the
 * links, learn which codecs answer, set up the position buffer and the
 * interrupt enables.  The stream status bits are cleared on the way. */
int sof_hda_init_chip(struct sof_host *h)
{
	int rc, i;
	uint16_t statests;

	sof_misc_clock_gating(h, 0);

	if (sof_hda_r32(h, SOF_HDA_GCTL) & SOF_HDA_GCTL_CRST)
		sof_hda_w16(h, SOF_HDA_STATESTS, SOF_HDA_STATESTS_MASK);

	rc = sof_hda_link_reset(h, 1);
	if (rc)
		goto out;
	lapic_delay_us(1000);
	rc = sof_hda_link_reset(h, 0);
	if (rc)
		goto out;
	/* codecs request their address within 521 us of the reset exit */
	lapic_delay_us(1500);

	sof_hda_upd32(h, SOF_HDA_GCTL, SOF_HDA_GCTL_UNSOL, SOF_HDA_GCTL_UNSOL);

	sof_ml_enum(h);
	sof_ml_power_hda_links(h, 1);
	lapic_delay_us(1000);

	statests = sof_hda_r16(h, SOF_HDA_STATESTS) & SOF_HDA_STATESTS_MASK;
	if (statests != 0x7fff)
		h->s.machine.hda_codec_mask = statests;
	sof_hda_w16(h, SOF_HDA_STATESTS, SOF_HDA_STATESTS_MASK);

	for (i = 0; i < h->nstreams; i++)
		sof_hda_w8(h, SOF_HDA_SD_BASE + SOF_HDA_SD_STRIDE * (uint32_t)i +
				      SOF_SD_STS,
			   SOF_SD_STS_MASK);
	sof_hda_w8(h, SOF_HDA_RIRBSTS, SOF_HDA_RIRBSTS_MASK);
	sof_hda_w32(h, SOF_HDA_INTSTS, SOF_HDA_INTSTS_CIS | 0x3fffffffu);

	if (h->posbuf_phys) {
		sof_hda_w32(h, SOF_HDA_DPUBASE, (uint32_t)(h->posbuf_phys >> 32));
		sof_hda_w32(h, SOF_HDA_DPLBASE,
			    (uint32_t)h->posbuf_phys | SOF_HDA_DPLBASE_ENABLE);
	}

	/* nothing streams through the HDA links yet */
	for (i = 0; i < h->nlinks; i++) {
		if (!h->link[i].alt)
			sof_hda_w16(h, h->link[i].off + SOF_ML_LOSIDV, 0);
	}

	sof_hda_upd32(h, SOF_HDA_INTCTL, SOF_HDA_INTCTL_CIE | SOF_HDA_INTCTL_GIE,
		      SOF_HDA_INTCTL_CIE | SOF_HDA_INTCTL_GIE);
	h->chip_up = 1;
out:
	sof_misc_clock_gating(h, 1);
	return rc;
}

void sof_hda_stop_chip(struct sof_host *h)
{
	int i;

	if (!h->chip_up)
		return;
	for (i = 0; i < h->nstreams; i++) {
		uint32_t sd = SOF_HDA_SD_BASE + SOF_HDA_SD_STRIDE * (uint32_t)i;

		sof_hda_upd32(h, sd + SOF_SD_CTL,
			      SOF_SD_CTL_RUN | SOF_SD_CTL_INT_MASK, 0);
	}
	sof_hda_upd32(h, SOF_HDA_INTCTL, 0xffffffffu, 0);
	for (i = 0; i < h->nstreams; i++)
		sof_hda_w8(h, SOF_HDA_SD_BASE + SOF_HDA_SD_STRIDE * (uint32_t)i +
				      SOF_SD_STS,
			   SOF_SD_STS_MASK);
	sof_hda_w16(h, SOF_HDA_STATESTS, SOF_HDA_STATESTS_MASK);
	sof_hda_w8(h, SOF_HDA_RIRBSTS, SOF_HDA_RIRBSTS_MASK);
	sof_hda_w32(h, SOF_HDA_INTSTS, SOF_HDA_INTSTS_CIS | 0x3fffffffu);
	sof_hda_w32(h, SOF_HDA_DPLBASE, 0);
	sof_hda_w32(h, SOF_HDA_DPUBASE, 0);
	h->chip_up = 0;
}

/* ===================================================================== */
/* multi-link capability                                                  */
/* ===================================================================== */

void sof_ml_enum(struct sof_host *h)
{
	uint32_t count;
	int i;

	h->nlinks = 0;
	if (!h->mlcap)
		return;
	count = (sof_hda_r32(h, h->mlcap + SOF_ML_MLCD) & 0xf) + 1;
	if (count > SOF_ML_MAX_LINKS)
		count = SOF_ML_MAX_LINKS;
	for (i = 0; i < (int)count; i++) {
		uint32_t off = h->mlcap + SOF_ML_LINK_BASE +
			       SOF_ML_LINK_STRIDE * (uint32_t)i;

		if (off + SOF_ML_LINK_STRIDE > h->bar0_map_size)
			break;
		h->link[i].off = off;
		h->link[i].lcap = sof_hda_r32(h, off + SOF_ML_LCAP);
		h->link[i].alt = (h->link[i].lcap & SOF_ML_LCAP_ALT) ? 1 : 0;
		h->link[i].elid = -1;
		h->link[i].slcount = 1;
		h->link[i].leptr = 0;
		h->link[i].base = 0;
		h->link[i].ofls = 0;
		h->link[i].intc = 0;
		if (h->link[i].alt) {
			uint32_t ptr;

			h->link[i].slcount =
				(int)((h->link[i].lcap & SOF_ML_LCAP_SLCOUNT_MASK) >>
				      SOF_ML_LCAP_SLCOUNT_SHIFT) + 1;
			h->link[i].ofls =
				(h->link[i].lcap & SOF_ML_LCAP_OFLS) ? 1 : 0;
			h->link[i].intc =
				(h->link[i].lcap & SOF_ML_LCAP_INTC) ? 1 : 0;
			h->link[i].leptr = sof_hda_r32(h, off + SOF_ML_LEPTR);
			h->link[i].elid =
				(int)(h->link[i].leptr >> SOF_ML_LEPTR_ID_SHIFT);
			ptr = h->link[i].leptr & SOF_ML_LEPTR_PTR_MASK;
			if (ptr && ptr < h->bar0_map_size)
				h->link[i].base = ptr;
		}
		h->nlinks = i + 1;
	}
}

static int sof_ml_sublink_power(struct sof_host *h, int link, int sub, int on)
{
	uint32_t lctl = h->link[link].off + SOF_ML_LCTL;
	uint32_t spa = 1u << (SOF_ML_LCTL_SPA_SHIFT + sub);
	uint32_t cpa = 1u << (SOF_ML_LCTL_CPA_SHIFT + sub);

	sof_hda_upd32(h, lctl, spa, on ? spa : 0);
	lapic_delay_us(10);
	return sof_poll_hda32(h, lctl, cpa, on ? cpa : 0, 2000, NULL);
}

/* Power the plain HDA links (HDMI codecs live there). */
int sof_ml_power_hda_links(struct sof_host *h, int on)
{
	int i, rc = 0;

	for (i = 0; i < h->nlinks; i++) {
		if (h->link[i].alt)
			continue;
		if (sof_ml_sublink_power(h, i, 0, on)) {
			sof_log(h, "HDA link %d did not power %s\n", i,
				on ? "up" : "down");
			rc = -EIO;
		}
	}
	return rc;
}

/* ACE 2.x: SSP, DMIC and USB audio offload links are given to the DSP. */
void sof_ml_set_offload(struct sof_host *h, int on)
{
	int i;

	for (i = 0; i < h->nlinks; i++) {
		int id = h->link[i].elid;

		if (!h->link[i].alt || !h->link[i].ofls)
			continue;
		if (id != SOF_ML_ID_SSP && id != SOF_ML_ID_DMIC &&
		    id != SOF_ML_ID_UAOL)
			continue;
		sof_hda_upd32(h, h->link[i].off + SOF_ML_LCTL, SOF_ML_LCTL_OFLEN,
			      on ? SOF_ML_LCTL_OFLEN : 0);
	}
}

int sof_ml_find_alt(struct sof_host *h, int elid)
{
	int i;

	for (i = 0; i < h->nlinks; i++)
		if (h->link[i].alt && h->link[i].elid == elid)
			return i;
	return -1;
}

int sof_ml_sdw_irq_pending(struct sof_host *h)
{
	int l = sof_ml_find_alt(h, SOF_ML_ID_SDW);
	uint32_t v;

	if (l < 0)
		return 0;
	v = sof_hda_r32(h, h->link[l].off + SOF_ML_LCTL);
	if (v == 0xffffffffu)
		return 0;
	return (v & SOF_ML_LCTL_INTSTS) ? 1 : 0;
}

/* ===================================================================== */
/* processing pipe, gating                                                */
/* ===================================================================== */

void sof_pp_enable(struct sof_host *h, int on)
{
	uint64_t fl;

	if (!h->ppcap)
		return;
	spin_lock_irqsave(&h->reg_lock, &fl);
	sof_hda_upd32(h, h->ppcap + SOF_PP_PPCTL, SOF_PP_PPCTL_GPROCEN,
		      on ? SOF_PP_PPCTL_GPROCEN : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

void sof_pp_int_enable(struct sof_host *h, int on)
{
	uint64_t fl;

	if (!h->ppcap)
		return;
	spin_lock_irqsave(&h->reg_lock, &fl);
	sof_hda_upd32(h, h->ppcap + SOF_PP_PPCTL, SOF_PP_PPCTL_PIE,
		      on ? SOF_PP_PPCTL_PIE : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

/* cAVS: the DSP must not be clock or power gated while it boots. */
void sof_dsp_gating(struct sof_host *h, int enable)
{
	uint32_t v;
	uint64_t fl;

	if (sof_is_ace(h))
		return;
	v = pci_cfg_read32_dev(h->s.pci, SOF_PCI_CGCTL);
	if (enable)
		v |= SOF_PCI_CGCTL_ADSPDCGE;
	else
		v &= ~SOF_PCI_CGCTL_ADSPDCGE;
	pci_cfg_write32_dev(h->s.pci, SOF_PCI_CGCTL, v);

	spin_lock_irqsave(&h->reg_lock, &fl);
	if (!enable || !h->capture_l1_blocks)
		sof_hda_upd32(h, SOF_HDA_VS_EM2, SOF_HDA_VS_EM2_L1SEN,
			      enable ? SOF_HDA_VS_EM2_L1SEN : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);

	v = pci_cfg_read32_dev(h->s.pci, SOF_PCI_PGCTL);
	if (enable)
		v &= ~SOF_PCI_PGCTL_ADSPPGD;
	else
		v |= SOF_PCI_PGCTL_ADSPPGD;
	pci_cfg_write32_dev(h->s.pci, SOF_PCI_PGCTL, v);
	h->gating_off = !enable;
}

/* ===================================================================== */
/* cAVS core control                                                      */
/* ===================================================================== */

int sof_cavs_core_is_enabled(struct sof_host *h, uint32_t mask)
{
	uint32_t v = sof_dsp_r32(h, SOF_CAVS_ADSPCS);

	if (v == 0xffffffffu)
		return 0;
	return (v & SOF_CAVS_ADSPCS_CPA(mask)) == SOF_CAVS_ADSPCS_CPA(mask) &&
	       (v & SOF_CAVS_ADSPCS_SPA(mask)) == SOF_CAVS_ADSPCS_SPA(mask) &&
	       !(v & SOF_CAVS_ADSPCS_CRST(mask)) &&
	       !(v & SOF_CAVS_ADSPCS_CSTALL(mask));
}

int sof_cavs_core_power_up(struct sof_host *h, uint32_t mask)
{
	uint32_t last = 0;

	mask &= h->chip->host_managed_mask;
	if (!mask)
		return 0;
	sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_SPA(mask),
		      SOF_CAVS_ADSPCS_SPA(mask));
	if (sof_poll_dsp32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CPA(mask),
			   SOF_CAVS_ADSPCS_CPA(mask), SOF_RESET_TIMEOUT_US,
			   &last)) {
		sof_log(h, "cores %#x did not power up (ADSPCS %#x)\n", mask,
			last);
		return -EIO;
	}
	return 0;
}

static int sof_cavs_core_reset(struct sof_host *h, uint32_t mask, int enter)
{
	uint32_t want = enter ? SOF_CAVS_ADSPCS_CRST(mask) : 0;
	uint32_t last = 0;

	sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CRST(mask), want);
	if (sof_poll_dsp32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CRST(mask), want,
			   SOF_RESET_TIMEOUT_US, &last)) {
		sof_log(h, "cores %#x did not %s reset (ADSPCS %#x)\n", mask,
			enter ? "enter" : "leave", last);
		return -EIO;
	}
	return 0;
}

int sof_cavs_core_run(struct sof_host *h, uint32_t mask)
{
	int rc = sof_cavs_core_reset(h, mask, 0);

	if (rc)
		return rc;
	sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CSTALL(mask), 0);
	if (!sof_cavs_core_is_enabled(h, mask)) {
		sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CSTALL(mask),
			      SOF_CAVS_ADSPCS_CSTALL(mask));
		sof_cavs_core_reset(h, mask, 1);
		sof_log(h, "core start failed, mask %#x ADSPCS %#x\n", mask,
			sof_dsp_r32(h, SOF_CAVS_ADSPCS));
		return -EIO;
	}
	return 0;
}

int sof_cavs_core_reset_power_down(struct sof_host *h, uint32_t mask)
{
	uint32_t last = 0;
	int rc;

	mask &= h->chip->host_managed_mask;
	if (!mask)
		return 0;
	/* stall, reset, then remove power */
	sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CSTALL(mask),
		      SOF_CAVS_ADSPCS_CSTALL(mask));
	rc = sof_cavs_core_reset(h, mask, 1);
	if (rc)
		return rc;
	sof_dsp_upd32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_SPA(mask), 0);
	if (sof_poll_dsp32(h, SOF_CAVS_ADSPCS, SOF_CAVS_ADSPCS_CPA(mask), 0,
			   SOF_PD_TIMEOUT_US, &last)) {
		sof_log(h, "cores %#x did not power down (ADSPCS %#x)\n", mask,
			last);
		return -EIO;
	}
	return 0;
}

/* The DSP is about to run its ROM: make every SSP a clock consumer so no
 * port drives a clock line before the topology configures it. */
void sof_cavs_ssp_set_consumer(struct sof_host *h)
{
	int i;

	for (i = 0; i < h->chip->ssp_count; i++) {
		uint32_t off = h->chip->ssp_base +
			       SOF_CAVS_SSP_STRIDE * (uint32_t)i + SOF_SSP_SSC1;

		if (off + 4 > h->bar4_map_size)
			break;
		sof_dsp_upd32(h, off, SOF_SSP_SSC1_CBP_CFP, SOF_SSP_SSC1_CBP_CFP);
	}
}

/* ===================================================================== */
/* ACE power control                                                      */
/* ===================================================================== */

int sof_ace_core0_power(struct sof_host *h, int on)
{
	uint32_t v = sof_dsp_r32(h, SOF_ACE_DSP2CXCTL);
	uint32_t last = 0;

	if (v == 0xffffffffu)
		return -EIO;
	if (on) {
		if (v & SOF_ACE_DSP2CXCTL_CPA) {
			h->core0_running = 1;
			return 0;
		}
		/* the host owns the core's IP and SHIM registers */
		sof_dsp_upd32(h, SOF_ACE_DSP2CXCTL, SOF_ACE_DSP2CXCTL_OSEL_MASK,
			      SOF_ACE_DSP2CXCTL_OSEL_HOST
				      << SOF_ACE_DSP2CXCTL_OSEL_SHIFT);
		sof_dsp_upd32(h, SOF_ACE_DSP2CXCTL, SOF_ACE_DSP2CXCTL_SPA,
			      SOF_ACE_DSP2CXCTL_SPA);
		/* CPA flickers right after SPA changes */
		sof_wait_us(1000);
		if (sof_poll_dsp32(h, SOF_ACE_DSP2CXCTL, SOF_ACE_DSP2CXCTL_CPA,
				   SOF_ACE_DSP2CXCTL_CPA, SOF_RESET_TIMEOUT_US,
				   &last)) {
			sof_log(h, "DSP core 0 did not power up (DSP2CXCTL %#x)\n",
				last);
			return -EIO;
		}
		h->core0_running = 1;
		return 0;
	}
	if (!(v & SOF_ACE_DSP2CXCTL_CPA)) {
		h->core0_running = 0;
		return 0;
	}
	sof_dsp_upd32(h, SOF_ACE_DSP2CXCTL, SOF_ACE_DSP2CXCTL_SPA, 0);
	sof_wait_us(1000);
	if (sof_poll_dsp32(h, SOF_ACE_DSP2CXCTL, SOF_ACE_DSP2CXCTL_CPA, 0,
			   SOF_PD_TIMEOUT_US, &last)) {
		sof_log(h, "DSP core 0 did not power down (DSP2CXCTL %#x)\n",
			last);
		return -EIO;
	}
	h->core0_running = 0;
	return 0;
}

static int sof_ace_subsystem_power(struct sof_host *h, int on)
{
	uint32_t last = 0;

	sof_dsp_upd32(h, SOF_ACE_HFDSSCS, SOF_ACE_HFDSSCS_SPA,
		      on ? SOF_ACE_HFDSSCS_SPA : 0);
	sof_wait_us(1000);
	if (sof_poll_dsp32(h, SOF_ACE_HFDSSCS, SOF_ACE_HFDSSCS_CPA,
			   on ? SOF_ACE_HFDSSCS_CPA : 0, SOF_RESET_TIMEOUT_US,
			   &last)) {
		sof_log(h, "DSP subsystem did not power %s (HfDSSCS %#x)\n",
			on ? "up" : "down", last);
		return -EIO;
	}
	return 0;
}

/* Everything that must be powered before the ROM can be started. */
int sof_dsp_power_up(struct sof_host *h)
{
	int rc;

	if (!sof_is_ace(h)) {
		if (sof_cavs_core_is_enabled(h, h->chip->host_managed_mask)) {
			/* left running by firmware or a previous boot */
			if (sof_cavs_core_reset_power_down(
				    h, h->chip->host_managed_mask))
				sof_log(h, "could not stop the running DSP; trying anyway\n");
		}
		sof_dsp_gating(h, 0);
		h->dsp_powered = 1;
		return 0;
	}

	if (sof_dsp_r32(h, SOF_ACE_HFDSSCS) & SOF_ACE_HFDSSCS_CPA) {
		if (sof_dsp_power_down(h))
			sof_log(h, "could not power down the enabled DSP; trying anyway\n");
	}

	rc = sof_ace_subsystem_power(h, 1);
	if (rc)
		return rc;

	{
		uint32_t ctl = (h->chip->flags & SOF_CHIP_ACE3) ?
				       SOF_ACE3_HFPWRCTL2 :
				       SOF_ACE_HFPWRCTL;
		uint32_t sts = (h->chip->flags & SOF_CHIP_ACE3) ?
				       SOF_ACE3_HFPWRSTS2 :
				       SOF_ACE_HFPWRSTS;
		uint32_t last = 0;

		/* the gated DSP domain holds the core's SHIM registers */
		sof_dsp_upd32(h, ctl, SOF_ACE_HFPWRCTL_WPDSPHPXPG,
			      SOF_ACE_HFPWRCTL_WPDSPHPXPG);
		sof_wait_us(1000);
		if (sof_poll_dsp32(h, sts, SOF_ACE_HFPWRSTS_DSPHPXPGS,
				   SOF_ACE_HFPWRSTS_DSPHPXPGS,
				   SOF_RESET_TIMEOUT_US, &last))
			sof_log(h, "gated DSP domain did not power up (%#x)\n",
				last);
	}

	/* SoundWire is in the I/O power domain: keep it ungated */
	if (h->s.machine.sdw_link_mask)
		sof_dsp_upd32(h, SOF_ACE_HFPWRCTL, SOF_ACE_HFPWRCTL_WPIOXPG(1),
			      SOF_ACE_HFPWRCTL_WPIOXPG(1));
	h->dsp_powered = 1;
	return 0;
}

int sof_dsp_power_down(struct sof_host *h)
{
	int rc;

	if (!sof_is_ace(h)) {
		rc = sof_cavs_core_reset_power_down(h, h->chip->host_managed_mask);
		sof_dsp_gating(h, 1);
		h->dsp_powered = 0;
		h->core0_running = 0;
		return rc;
	}
	rc = sof_ace_core0_power(h, 0);
	if (rc)
		return rc;
	rc = sof_ace_subsystem_power(h, 0);
	h->dsp_powered = 0;
	return rc;
}

/* ===================================================================== */
/* DSP interrupt enables                                                  */
/* ===================================================================== */

static uint32_t sof_ace_intptr(struct sof_host *h)
{
	uint32_t p = sof_dsp_r32(h, SOF_ACE_HFINTIPPTR);

	if (p == 0xffffffffu)
		return 0;
	p &= SOF_ACE_HFINTIPPTR_MASK;
	if (p == 0 || p + SOF_ACE_IRQSTS + 4 > h->bar4_map_size)
		return 0;
	return p;
}

/* IPC busy/done interrupts at the doorbell controller. */
void sof_dsp_ipc_int_enable(struct sof_host *h, int on)
{
	uint32_t ctl = sof_is_ace(h) ? SOF_ACE_IPC_CTL : SOF_CAVS_IPC_CTL;
	uint32_t bits = SOF_IPC_CTL_BUSY | SOF_IPC_CTL_DONE;

	sof_dsp_upd32(h, ctl, bits, on ? bits : 0);
	if (!sof_is_ace(h))
		sof_dsp_upd32(h, SOF_CAVS_ADSPIC, SOF_CAVS_ADSPIX_IPC,
			      on ? SOF_CAVS_ADSPIX_IPC : 0);
}

/* ACE: the interrupt routing to the host (HfINTIPPTR and HfHIPCIE). */
int sof_dsp_irq_enable(struct sof_host *h, int on)
{
	uint32_t p, mask, last = 0;

	if (!sof_is_ace(h))
		return 0;
	p = sof_ace_intptr(h);
	if (!p) {
		sof_log(h, "no interrupt pointer (HfINTIPPTR %#x)\n",
			sof_dsp_r32(h, SOF_ACE_HFINTIPPTR));
		return -EIO;
	}
	mask = SOF_ACE_INTEN_IPC | SOF_ACE_INTEN_SDW;
	sof_dsp_upd32(h, p, mask, on ? mask : 0);
	if (sof_poll_dsp32(h, p, mask, on ? mask : 0, SOF_RESET_TIMEOUT_US,
			   &last))
		sof_log(h, "interrupt enable at %#x stuck at %#x\n", p, last);
	sof_dsp_upd32(h, SOF_ACE_HFHIPCIE, SOF_ACE_HFHIPCIE_IE,
		      on ? SOF_ACE_HFHIPCIE_IE : 0);
	if (sof_poll_dsp32(h, SOF_ACE_HFHIPCIE, SOF_ACE_HFHIPCIE_IE,
			   on ? SOF_ACE_HFHIPCIE_IE : 0, SOF_RESET_TIMEOUT_US,
			   &last)) {
		sof_log(h, "HfHIPCIE stuck at %#x\n", last);
		return -EIO;
	}
	return 0;
}

void sof_dsp_sdw_int_enable(struct sof_host *h, int on)
{
	if (!sof_is_ace(h)) {
		sof_dsp_upd32(h, SOF_CAVS_ADSPIC2, SOF_CAVS_ADSPIX2_SNDW,
			      on ? SOF_CAVS_ADSPIX2_SNDW : 0);
		return;
	}
	if (!sof_is_ace2(h)) {
		sof_dsp_upd32(h, SOF_ACE_HFSNDWIE, SOF_ACE_HFSNDWIE_MASK,
			      on ? SOF_ACE_HFSNDWIE_MASK : 0);
		return;
	}
	/* ACE 2.x+: the SoundWire alternate link has its own enable */
	{
		int l = sof_ml_find_alt(h, SOF_ML_ID_SDW);

		if (l >= 0 && h->link[l].intc)
			sof_hda_upd32(h, h->link[l].off + SOF_ML_LCTL,
				      SOF_ML_LCTL_INTEN,
				      on ? SOF_ML_LCTL_INTEN : 0);
	}
}

int sof_dsp_ipc_irq_pending(struct sof_host *h)
{
	uint32_t v;

	if (!sof_is_ace(h)) {
		v = sof_dsp_r32(h, SOF_CAVS_ADSPIS);
		return v != 0xffffffffu && (v & SOF_CAVS_ADSPIX_IPC);
	}
	{
		uint32_t p = sof_ace_intptr(h);

		if (!p)
			return 0;
		v = sof_dsp_r32(h, p + SOF_ACE_IRQSTS);
		return v != 0xffffffffu && (v & SOF_ACE_IRQSTS_IPC);
	}
}

int sof_dsp_sdw_irq_pending(struct sof_host *h)
{
	uint32_t v;

	if (!sof_is_ace(h)) {
		v = sof_dsp_r32(h, SOF_CAVS_ADSPIS2);
		return v != 0xffffffffu && (v & SOF_CAVS_ADSPIX2_SNDW);
	}
	if (sof_is_ace2(h))
		return sof_ml_sdw_irq_pending(h);
	{
		uint32_t p = sof_ace_intptr(h);

		if (!p)
			return 0;
		v = sof_dsp_r32(h, p + SOF_ACE_IRQSTS);
		return v != 0xffffffffu && (v & SOF_ACE_IRQSTS_SDW);
	}
}

/* ===================================================================== */
/* firmware status decoding                                               */
/* ===================================================================== */

uint32_t sof_dsp_rom_status(struct sof_host *h)
{
	return sof_dsp_r32(h, h->chip->rom_status);
}

struct sof_code_name {
	uint32_t code;
	const char *name;
};

static const struct sof_code_name sof_cavs_rom_states[] = {
	{ 0x0, "INIT" },
	{ 0x1, "INIT_DONE" },
	{ 0x2, "CSE_MANIFEST_LOADED" },
	{ 0x3, "FW_MANIFEST_LOADED" },
	{ 0x4, "FW_FW_LOADED" },
	{ 0x5, "FW_ENTERED" },
	{ 0x6, "VERIFY_FEATURE_MASK" },
	{ 0x7, "GET_LOAD_OFFSET" },
	{ 0x8, "FETCH_ROM_EXT" },
	{ 0x9, "FETCH_ROM_EXT_DONE" },
	{ 0x10, "CSE_IMR_REQUEST" },
	{ 0x11, "CSE_IMR_GRANTED" },
	{ 0x12, "CSE_VALIDATE_IMAGE_REQUEST" },
	{ 0x13, "CSE_IMAGE_VALIDATED" },
	{ 0x20, "CSE_IPC_IFACE_INIT" },
	{ 0x21, "CSE_IPC_RESET_PHASE_1" },
	{ 0x22, "CSE_IPC_OPERATIONAL_ENTRY" },
	{ 0x23, "CSE_IPC_OPERATIONAL" },
	{ 0x24, "CSE_IPC_DOWN" },
};

static const struct sof_code_name sof_ace_rom_states[] = {
	{ 0x0, "INIT" },
	{ 0x1, "INIT_DONE" },
	{ 0x2, "CSE_MANIFEST_LOADED" },
	{ 0x3, "FW_MANIFEST_LOADED" },
	{ 0x4, "FW_FW_LOADED" },
	{ 0x5, "FW_ENTERED" },
	{ 0x6, "VERIFY_FEATURE_MASK" },
	{ 0x7, "GET_LOAD_OFFSET" },
	{ 0x8, "RESET_VECTOR_DONE" },
	{ 0x9, "PURGE_BOOT" },
	{ 0xa, "RESTORE_BOOT" },
	{ 0xb, "FW_ENTRY_POINT" },
	{ 0xc, "VALIDATE_PUB_KEY" },
	{ 0xd, "POWER_DOWN_HPSRAM" },
	{ 0xe, "POWER_DOWN_ULPSRAM" },
	{ 0xf, "POWER_UP_ULPSRAM_STACK" },
	{ 0x10, "POWER_UP_HPSRAM_DMA" },
	{ 0x11, "BEFORE_EP_POINTER_READ" },
	{ 0x12, "VALIDATE_MANIFEST" },
	{ 0x13, "VALIDATE_FW_MODULE" },
	{ 0x14, "PROTECT_IMR_REGION" },
	{ 0x15, "PUSH_MODEL_ROUTINE" },
	{ 0x16, "PULL_MODEL_ROUTINE" },
	{ 0x17, "VALIDATE_PKG_DIR" },
	{ 0x18, "VALIDATE_CPD" },
	{ 0x19, "VALIDATE_CSS_MAN_HEADER" },
	{ 0x1a, "VALIDATE_BLOB_SVN" },
	{ 0x1b, "VERIFY_IFWI_PARTITION" },
	{ 0x1c, "REMOVE_ACCESS_CONTROL" },
	{ 0x1d, "AUTH_BYPASS" },
	{ 0x1e, "AUTH_ENABLED" },
	{ 0x1f, "INIT_DMA" },
	{ 0x20, "PURGE_FW_ENTRY" },
	{ 0x21, "PURGE_FW_END" },
	{ 0x22, "CLEAN_UP_BSS_DONE" },
	{ 0x23, "IMR_RESTORE_ENTRY" },
	{ 0x24, "IMR_RESTORE_END" },
	{ 0x25, "FW_MANIFEST_IN_DMA_BUFF" },
	{ 0x26, "LOAD_CSE_MAN_TO_IMR" },
	{ 0x27, "LOAD_FW_MAN_TO_IMR" },
	{ 0x28, "LOAD_FW_CODE_TO_IMR" },
	{ 0x29, "FW_LOADING_DONE" },
	{ 0x2a, "FW_CODE_LOADED" },
	{ 0x2b, "VERIFY_IMAGE_TYPE" },
	{ 0x2c, "AUTH_API_INIT" },
	{ 0x2d, "AUTH_API_PROC" },
	{ 0x2e, "AUTH_API_FIRST_BUSY" },
	{ 0x2f, "AUTH_API_FIRST_RESULT" },
	{ 0x30, "AUTH_API_CLEANUP" },
};

static const struct sof_code_name sof_bringup_states[] = {
	{ 0x0, "INIT" },
	{ 0x1, "INIT_DONE" },
	{ 0x2, "HPSRAM_LOAD" },
	{ 0x3, "UNPACK_START" },
	{ 0x4, "IMR_RESTORE" },
	{ 0x5, "FW_ENTERED" },
};

static const struct sof_code_name sof_rom_errors[] = {
	{ 40, "CSE error" },
	{ 41, "CSE wrong response" },
	{ 42, "IMR too small" },
	{ 43, "base firmware not found" },
	{ 44, "signature verification failed" },
	{ 45, "IPC fatal error" },
	{ 46, "L2 cache error" },
	{ 47, "load offset too small" },
	{ 50, "API pointer invalid" },
	{ 51, "base firmware incompatible" },
	{ 0xbee00000u, "unhandled interrupt" },
	{ 0xecc00000u, "ECC memory hole" },
	{ 0xcafe0000u, "kernel exception" },
	{ 0xbeef0000u, "user exception" },
	{ 0xdecaf000u, "unexpected reset" },
	{ 0x4c4c4e55u, "null firmware entry point" },
};

static const char *sof_code_lookup(const struct sof_code_name *t, size_t n,
				   uint32_t code)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (t[i].code == code)
			return t[i].name;
	return NULL;
}

#define SOF_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

const char *sof_fsr_state_name(struct sof_host *h, uint32_t fsr)
{
	uint32_t module = (fsr & SOF_FSR_MODULE_MASK) >> SOF_FSR_MODULE_SHIFT;
	uint32_t state = fsr & SOF_FSR_STATE_MASK;
	const char *n = NULL;

	switch (module) {
	case 0: /* ROM */
	case 5: /* ROM extension */
		if (sof_is_ace(h))
			n = sof_code_lookup(sof_ace_rom_states,
					    SOF_ARRAY_SIZE(sof_ace_rom_states),
					    state);
		else
			n = sof_code_lookup(sof_cavs_rom_states,
					    SOF_ARRAY_SIZE(sof_cavs_rom_states),
					    state);
		break;
	case 4: /* bring-up */
		n = sof_code_lookup(sof_bringup_states,
				    SOF_ARRAY_SIZE(sof_bringup_states), state);
		break;
	case 2: /* base firmware */
		n = state == SOF_FSR_STATE_FW_ENTERED ? "FW_ENTERED" :
		    state == SOF_FSR_STATE_INIT_DONE  ? "INIT_DONE" :
		    state == SOF_FSR_STATE_INIT	      ? "INIT" :
							"RUNNING";
		break;
	default:
		break;
	}
	return n ? n : "unknown";
}

const char *sof_rom_error_name(uint32_t code)
{
	const char *n = sof_code_lookup(sof_rom_errors,
					SOF_ARRAY_SIZE(sof_rom_errors), code);

	if (!n && (code & 0xffff0000u) == 0xbee00000u)
		n = "unhandled interrupt";
	return n ? n : "unknown";
}

static const char *sof_fsr_module_name(uint32_t module)
{
	switch (module) {
	case 0:
		return "ROM";
	case 1:
		return "ROM-bypass";
	case 2:
		return "base-FW";
	case 3:
		return "LP-boot";
	case 4:
		return "bring-up";
	case 5:
		return "ROM-ext";
	default:
		return "?";
	}
}

static const char *sof_fsr_wait_name(uint32_t w)
{
	switch (w) {
	case 0:
		return "-";
	case 1:
		return "IPC busy";
	case 2:
		return "IPC done";
	case 3:
		return "cache invalidation";
	case 4:
		return "LP SRAM off";
	case 5:
		return "DMA buffer full";
	case 6:
		return "CSE CSR";
	default:
		return "?";
	}
}

static size_t sof_dsp_state_text(struct sof_host *h, char *buf, size_t len,
				 size_t pos)
{
	uint32_t fsr = sof_dsp_rom_status(h);
	uint32_t err = sof_dsp_r32(h, h->chip->rom_status + 4);
	int i;

	pos = sof_append(buf, len, pos,
			 "firmware status %#010x: module %s, state %#x (%s), wait %s%s\n",
			 fsr,
			 sof_fsr_module_name((fsr & SOF_FSR_MODULE_MASK) >>
					     SOF_FSR_MODULE_SHIFT),
			 fsr & SOF_FSR_STATE_MASK, sof_fsr_state_name(h, fsr),
			 sof_fsr_wait_name((fsr & SOF_FSR_WAIT_MASK) >>
					   SOF_FSR_WAIT_SHIFT),
			 (fsr & SOF_FSR_HALTED) ? ", HALTED" : "");
	if (err)
		pos = sof_append(buf, len, pos, "  %s code %#x (%s)\n",
				 (fsr & SOF_FSR_STATE_MASK) ==
						 SOF_FSR_STATE_FW_ENTERED ?
					 "status" :
					 "error",
				 err, sof_rom_error_name(err));
	pos = sof_append(buf, len, pos, "  extended status:");
	for (i = 0; i < 8; i++)
		pos = sof_append(buf, len, pos, " %#x",
				 sof_dsp_r32(h, h->chip->rom_status +
							(uint32_t)i * 4));
	pos = sof_append(buf, len, pos, "\n");

	if (sof_is_ace(h)) {
		pos = sof_append(buf, len, pos,
				 "  HfDSSCS %#x DSP2CxCTL %#x HfPWRCTL %#x HfPWRSTS %#x HfINTIPPTR %#x HfHIPCIE %#x\n",
				 sof_dsp_r32(h, SOF_ACE_HFDSSCS),
				 sof_dsp_r32(h, SOF_ACE_DSP2CXCTL),
				 sof_dsp_r32(h, SOF_ACE_HFPWRCTL),
				 sof_dsp_r32(h, SOF_ACE_HFPWRSTS),
				 sof_dsp_r32(h, SOF_ACE_HFINTIPPTR),
				 sof_dsp_r32(h, SOF_ACE_HFHIPCIE));
		if (h->chip->flags & SOF_CHIP_ACE3)
			pos = sof_append(buf, len, pos,
					 "  HfPWRCTL2 %#x HfPWRSTS2 %#x\n",
					 sof_dsp_r32(h, SOF_ACE3_HFPWRCTL2),
					 sof_dsp_r32(h, SOF_ACE3_HFPWRSTS2));
		if (h->fwregs_off)
			pos = sof_append(buf, len, pos,
					 "  window 0: %#x %#x %#x %#x\n",
					 sof_dsp_r32(h, h->fwregs_off),
					 sof_dsp_r32(h, h->fwregs_off + 4),
					 sof_dsp_r32(h, h->fwregs_off + 8),
					 sof_dsp_r32(h, h->fwregs_off + 12));
	} else {
		pos = sof_append(buf, len, pos,
				 "  ADSPCS %#x ADSPIC %#x ADSPIS %#x ADSPIC2 %#x ADSPIS2 %#x\n",
				 sof_dsp_r32(h, SOF_CAVS_ADSPCS),
				 sof_dsp_r32(h, SOF_CAVS_ADSPIC),
				 sof_dsp_r32(h, SOF_CAVS_ADSPIS),
				 sof_dsp_r32(h, SOF_CAVS_ADSPIC2),
				 sof_dsp_r32(h, SOF_CAVS_ADSPIS2));
	}
	pos = sof_append(buf, len, pos,
			 "  HDA: GCTL %#x INTCTL %#x INTSTS %#x STATESTS %#x",
			 sof_hda_r32(h, SOF_HDA_GCTL),
			 sof_hda_r32(h, SOF_HDA_INTCTL),
			 sof_hda_r32(h, SOF_HDA_INTSTS),
			 sof_hda_r16(h, SOF_HDA_STATESTS));
	if (h->ppcap)
		pos = sof_append(buf, len, pos, " PPCTL %#x PPSTS %#x",
				 sof_hda_r32(h, h->ppcap + SOF_PP_PPCTL),
				 sof_hda_r32(h, h->ppcap + SOF_PP_PPSTS));
	pos = sof_append(buf, len, pos, "\n");
	return pos;
}

void sof_dsp_dump(struct sof_host *h, const char *why)
{
	char *buf = kalloc(1024);

	if (!buf) {
		sof_log(h, "%s: FSR %#x\n", why, sof_dsp_rom_status(h));
		return;
	}
	buf[0] = 0;
	sof_dsp_state_text(h, buf, 1024, 0);
	sof_log(h, "%s\n", why);
	kprintf("%s", buf);
	kfree(buf);
	sof_ipc4_dump_regs(h);
}

static const char *sof_debug_slot_name(uint32_t type)
{
	switch (type & SOF_IPC4_DEBUG_SLOT_TYPE_MASK) {
	case 0:
		return "unused";
	case SOF_IPC4_DEBUG_SLOT_CRITICAL_LOG:
		return "critical-log";
	case SOF_IPC4_DEBUG_SLOT_DEBUG_LOG:
		return "debug-log";
	case SOF_IPC4_DEBUG_SLOT_GDB_STUB:
		return "gdb-stub";
	case SOF_IPC4_DEBUG_SLOT_TELEMETRY:
		return "telemetry";
	default:
		if (type == SOF_IPC4_DEBUG_SLOT_BROKEN)
			return "broken";
		return "other";
	}
}

/* The firmware crashed (exception notification, IPC timeout with a halted
 * core): print what it left in the SRAM windows.  The telemetry slot holds
 * the core dump header and register file; the critical log slot holds the
 * last log lines in text form when the firmware was built that way. */
void sof_dsp_panic_dump(struct sof_host *h)
{
	int i;

	if (h->panic_dumped)
		return;
	h->panic_dumped = 1;
	sof_dsp_dump(h, "DSP panic");
	if (!h->debug_off ||
	    h->debug_off + SOF_IPC4_DEBUG_SLOT_SIZE * (SOF_IPC4_DEBUG_SLOTS + 1) >
		    h->bar4_map_size)
		return;
	for (i = 0; i < SOF_IPC4_DEBUG_SLOTS; i++) {
		uint32_t d = h->debug_off + SOF_IPC4_DEBUG_DESC_SIZE * (uint32_t)i;
		uint32_t res_id = sof_dsp_r32(h, d);
		uint32_t type = sof_dsp_r32(h, d + 4);
		uint32_t vma = sof_dsp_r32(h, d + 8);
		uint32_t slot = h->debug_off +
				SOF_IPC4_DEBUG_SLOT_SIZE * (uint32_t)(i + 1);
		int k;

		if (!type || type == 0xffffffffu)
			continue;
		kprintf("[snd] sof: debug slot %d: %s (type %#x res %#x vma %#x)\n",
			i, sof_debug_slot_name(type), type, res_id, vma);
		switch (type & SOF_IPC4_DEBUG_SLOT_TYPE_MASK) {
		case SOF_IPC4_DEBUG_SLOT_TELEMETRY:
			for (k = 0; k < 64; k += 8)
				kprintf("[snd] sof:   %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					(unsigned)(k * 4),
					sof_dsp_r32(h, slot + (uint32_t)k * 4),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 4),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 8),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 12),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 16),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 20),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 24),
					sof_dsp_r32(h, slot + (uint32_t)k * 4 + 28));
			break;
		case SOF_IPC4_DEBUG_SLOT_CRITICAL_LOG:
		case SOF_IPC4_DEBUG_SLOT_DEBUG_LOG: {
			/* {host read ptr, dsp write ptr, ring} */
			uint32_t wp = sof_dsp_r32(h, slot + 4);
			char line[129];
			uint32_t n = 0, j;
			uint32_t ring = SOF_IPC4_DEBUG_SLOT_SIZE - 8;
			uint32_t start;

			if (wp >= ring)
				wp = 0;
			/* the last 512 bytes before the write pointer */
			start = wp >= 512 ? wp - 512 : ring - (512 - wp);
			for (j = 0; j < 512; j++) {
				uint32_t o = (start + j) % ring;
				uint32_t word = sof_dsp_r32(h, slot + 8 + (o & ~3u));
				char c = (char)(word >> ((o & 3u) * 8));

				if (c == '\n' || n == 128) {
					line[n] = 0;
					if (n)
						kprintf("[snd] sof:   | %s\n", line);
					n = 0;
					continue;
				}
				if (c >= 32 && c < 127)
					line[n++] = c;
			}
			if (n) {
				line[n] = 0;
				kprintf("[snd] sof:   | %s\n", line);
			}
			break;
		}
		default:
			break;
		}
	}
}

long sof_dsp_diag(struct sof_host *h, char *buf, size_t len)
{
	size_t pos = 0;
	int i;

	pos = sof_append(buf, len, pos,
			 "SOF %s (%s, %s), PCI %04x:%04x at %s\n",
			 h->id->name, h->chip->generation, h->id->tag,
			 h->s.pci->vendor_id, h->s.pci->device_id, h->bus);
	pos = sof_append(buf, len, pos,
			 "HDA: GCAP %#x, %d in / %d out / %d bidir streams, %s DMA, caps PP@%#x SPB@%#x ML@%#x GTS@%#x DRSM@%#x\n",
			 h->gcap, h->num_iss, h->num_oss, h->num_bss,
			 h->addr64 ? "64-bit" : "32-bit", h->ppcap, h->spbcap,
			 h->mlcap, h->gtscap, h->drsmcap);
	pos = sof_append(buf, len, pos, "codec mask %#x, interrupt %s%d\n",
			 h->s.machine.hda_codec_mask, h->msi ? "MSI vector " : "GSI ",
			 h->msi ? h->s.irq_vector : h->gsi);
	for (i = 0; i < h->nlinks; i++) {
		uint32_t lctl = sof_hda_r32(h, h->link[i].off + SOF_ML_LCTL);

		if (h->link[i].alt)
			pos = sof_append(buf, len, pos,
					 "  link %d: alt id %#x, %d sublinks%s%s, LCTL %#x\n",
					 i, (unsigned)h->link[i].elid,
					 h->link[i].slcount,
					 h->link[i].ofls ? ", offload" : "",
					 h->link[i].intc ? ", irq" : "", lctl);
		else
			pos = sof_append(buf, len, pos,
					 "  link %d: HDA, LCTL %#x LOSIDV %#x\n",
					 i, lctl,
					 sof_hda_r16(h, h->link[i].off +
								SOF_ML_LOSIDV));
	}
	pos = sof_append(buf, len, pos,
			 "interrupts %u (ipc %u, streams %u, soundwire %u, foreign %u)\n",
			 h->irq_count, h->irq_ipc, h->irq_stream, h->irq_sdw,
			 h->irq_spurious);
	pos = sof_dsp_state_text(h, buf, len, pos);
	return (long)pos;
}

/* ===================================================================== */
/* DMA engines                                                            */
/* ===================================================================== */

int sof_streams_init(struct sof_host *h)
{
	int i;
	int dma32 = !h->addr64;

	h->gcap = sof_hda_r16(h, SOF_HDA_GCAP);
	h->num_iss = (h->gcap >> 8) & 0xf;
	h->num_oss = (h->gcap >> 12) & 0xf;
	h->num_bss = (h->gcap >> 3) & 0x1f;
	h->nstreams = h->num_iss + h->num_oss;
	if (h->nstreams > SOF_MAX_STREAMS)
		h->nstreams = SOF_MAX_STREAMS;
	if (h->nstreams == 0) {
		sof_log(h, "controller reports no DMA engines (GCAP %#x)\n",
			h->gcap);
		return -ENODEV;
	}
	if (SOF_HDA_SD_BASE + SOF_HDA_SD_STRIDE * (uint32_t)h->nstreams >
	    h->bar0_map_size)
		return -ENODEV;

	for (i = 0; i < h->nstreams; i++) {
		struct sof_hstream *st = &h->st[i];

		st->host = h;
		st->index = i;
		st->sd_off = SOF_HDA_SD_BASE + SOF_HDA_SD_STRIDE * (uint32_t)i;
		if (i < h->num_iss) {
			st->direction = AUDIO_DIR_CAPTURE;
			st->tag = i + 1;
		} else {
			st->direction = AUDIO_DIR_PLAYBACK;
			st->tag = i - h->num_iss + 1;
		}
		if (!st->bdl) {
			st->bdl = snd_dma_alloc(SOF_BDL_BYTES, dma32, 0,
						&st->bdl_phys);
			if (!st->bdl) {
				sof_log(h, "no memory for stream %d's BDL\n", i);
				return -ENOMEM;
			}
		}
	}
	if (!h->posbuf) {
		h->posbuf = snd_dma_alloc(4096, dma32, 0, &h->posbuf_phys);
		if (!h->posbuf)
			return -ENOMEM;
	}
	sof_hda_w32(h, SOF_HDA_DPUBASE, (uint32_t)(h->posbuf_phys >> 32));
	sof_hda_w32(h, SOF_HDA_DPLBASE,
		    (uint32_t)h->posbuf_phys | SOF_HDA_DPLBASE_ENABLE);
	return 0;
}

void sof_streams_free(struct sof_host *h)
{
	int i;

	for (i = 0; i < SOF_MAX_STREAMS; i++) {
		struct sof_hstream *st = &h->st[i];

		if (st->bdl) {
			snd_dma_free(st->bdl, SOF_BDL_BYTES);
			st->bdl = NULL;
			st->bdl_phys = 0;
		}
	}
	if (h->posbuf) {
		sof_hda_w32(h, SOF_HDA_DPLBASE, 0);
		sof_hda_w32(h, SOF_HDA_DPUBASE, 0);
		snd_dma_free(h->posbuf, 4096);
		h->posbuf = NULL;
		h->posbuf_phys = 0;
	}
}

struct sof_hstream *sof_stream_get(struct sof_host *h, int direction)
{
	struct sof_hstream *found = NULL;
	uint64_t fl;
	int i;

	spin_lock_irqsave(&h->reg_lock, &fl);
	/* prefer an engine whose link side is idle as well */
	for (i = 0; i < h->nstreams && !found; i++) {
		struct sof_hstream *st = &h->st[i];

		if (st->direction == direction && !st->host_in_use &&
		    !st->link_in_use)
			found = st;
	}
	for (i = 0; i < h->nstreams && !found; i++) {
		struct sof_hstream *st = &h->st[i];

		if (st->direction == direction && !st->host_in_use)
			found = st;
	}
	if (found) {
		found->host_in_use = 1;
		found->running = 0;
		found->substream = NULL;
		found->owner = NULL;
		found->ioc_count = 0;
		/* cAVS: capture DMA misbehaves across DMI L1 transitions */
		if (direction == AUDIO_DIR_CAPTURE && !sof_is_ace(h)) {
			found->l1_blocked = 1;
			if (h->capture_l1_blocks++ == 0)
				sof_hda_upd32(h, SOF_HDA_VS_EM2,
					      SOF_HDA_VS_EM2_L1SEN, 0);
		}
	}
	spin_unlock_irqrestore(&h->reg_lock, fl);
	return found;
}

void sof_stream_put(struct sof_hstream *st)
{
	struct sof_host *h;
	uint64_t fl;

	if (!st)
		return;
	h = st->host;
	spin_lock_irqsave(&h->reg_lock, &fl);
	st->host_in_use = 0;
	st->substream = NULL;
	st->owner = NULL;
	if (st->l1_blocked) {
		st->l1_blocked = 0;
		if (h->capture_l1_blocks > 0 && --h->capture_l1_blocks == 0 &&
		    !h->gating_off)
			sof_hda_upd32(h, SOF_HDA_VS_EM2, SOF_HDA_VS_EM2_L1SEN,
				      SOF_HDA_VS_EM2_L1SEN);
	}
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

struct sof_hstream *sof_link_stream_get(struct sof_host *h, int direction)
{
	struct sof_hstream *found = NULL;
	uint64_t fl;
	int i;

	if (!h->ppcap)
		return NULL;
	spin_lock_irqsave(&h->reg_lock, &fl);
	for (i = 0; i < h->nstreams && !found; i++) {
		struct sof_hstream *st = &h->st[i];

		if (st->direction == direction && !st->link_in_use &&
		    !st->host_in_use)
			found = st;
	}
	for (i = 0; i < h->nstreams && !found; i++) {
		struct sof_hstream *st = &h->st[i];

		if (st->direction == direction && !st->link_in_use)
			found = st;
	}
	if (found) {
		found->link_in_use = 1;
		found->link_running = 0;
	}
	spin_unlock_irqrestore(&h->reg_lock, fl);
	return found;
}

void sof_link_stream_put(struct sof_hstream *st)
{
	uint64_t fl;

	if (!st)
		return;
	if (st->link_running)
		sof_link_dma_run(st, 0);
	spin_lock_irqsave(&st->host->reg_lock, &fl);
	st->link_in_use = 0;
	spin_unlock_irqrestore(&st->host->reg_lock, fl);
}

static void sof_ppctl_procen(struct sof_host *h, int index, int on)
{
	uint64_t fl;

	if (!h->ppcap)
		return;
	spin_lock_irqsave(&h->reg_lock, &fl);
	sof_hda_upd32(h, h->ppcap + SOF_PP_PPCTL, SOF_PP_PPCTL_PROCEN(index),
		      on ? SOF_PP_PPCTL_PROCEN(index) : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

static void sof_intctl_stream(struct sof_host *h, int index, int on)
{
	uint64_t fl;

	spin_lock_irqsave(&h->reg_lock, &fl);
	sof_hda_upd32(h, SOF_HDA_INTCTL, 1u << index, on ? (1u << index) : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

int sof_stream_reset(struct sof_hstream *st)
{
	struct sof_host *h = st->host;
	uint32_t last = 0;

	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_SRST,
		      SOF_SD_CTL_SRST);
	if (sof_poll_hda32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_SRST,
			   SOF_SD_CTL_SRST, 1000, &last)) {
		sof_log(h, "stream %d did not enter reset (CTL %#x)\n",
			st->index, last);
		return -ETIMEDOUT;
	}
	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_SRST, 0);
	lapic_delay_us(3);
	if (sof_poll_hda32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_SRST, 0, 1000,
			   &last)) {
		sof_log(h, "stream %d did not leave reset (CTL %#x)\n",
			st->index, last);
		return -ETIMEDOUT;
	}
	return 0;
}

static int sof_stream_halt(struct sof_hstream *st)
{
	struct sof_host *h = st->host;
	uint32_t last = 0;
	int rc;

	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL,
		      SOF_SD_CTL_RUN | SOF_SD_CTL_INT_MASK, 0);
	rc = sof_poll_hda32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_RUN, 0, 1000,
			    &last);
	if (rc)
		sof_log(h, "stream %d did not stop (CTL %#x)\n", st->index,
			last);
	sof_hda_w8(h, st->sd_off + SOF_SD_STS, SOF_SD_STS_MASK);
	return rc;
}

/* Fill one BDL entry. */
static void sof_bdl_entry(struct sof_hstream *st, uint32_t i, uint64_t addr,
			  uint32_t len, int ioc)
{
	volatile uint32_t *e = (volatile uint32_t *)(st->bdl + i * 16u);

	e[0] = (uint32_t)addr;
	e[1] = (uint32_t)(addr >> 32);
	e[2] = len;
	e[3] = ioc ? 1u : 0u;
}

int sof_stream_program(struct sof_hstream *st,
		       const struct sof_dma_chunk *chunk, int nchunks,
		       uint32_t total_bytes, uint32_t period_bytes,
		       uint16_t format, int decouple)
{
	struct sof_host *h = st->host;
	uint32_t n = 0, done = 0, period_left;
	int c, rc;

	if (!st->bdl || nchunks <= 0 || total_bytes == 0)
		return -EINVAL;

	if (decouple)
		sof_ppctl_procen(h, st->index, 1);

	sof_stream_halt(st);
	rc = sof_stream_reset(st);
	if (rc)
		return rc;
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPL, 0);
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPU, 0);
	sof_stream_halt(st);

	/* Walk the chunks, cutting entries at period boundaries so every
	 * period completion raises an interrupt. */
	period_left = period_bytes ? period_bytes : total_bytes;
	for (c = 0; c < nchunks && done < total_bytes; c++) {
		uint32_t off = 0;
		uint32_t clen = chunk[c].bytes;

		if (clen > total_bytes - done)
			clen = total_bytes - done;
		while (off < clen) {
			uint32_t len = clen - off;
			int ioc = 0;

			if (len > period_left)
				len = period_left;
			if (n >= SOF_BDL_MAX_ENTRIES) {
				sof_log(h, "stream %d: too many buffer pieces\n",
					st->index);
				return -ENOSPC;
			}
			period_left -= len;
			if (period_left == 0) {
				ioc = 1;
				period_left = period_bytes ? period_bytes :
							     total_bytes;
			}
			if (done + len == total_bytes)
				ioc = 1;
			sof_bdl_entry(st, n++, chunk[c].phys + off, len, ioc);
			off += len;
			done += len;
		}
	}
	if (done != total_bytes)
		return -EINVAL;

	/* The engine needs LVI >= 1: split a lone entry in two halves (the
	 * second half starting on a 128-byte boundary). */
	if (n == 1) {
		uint32_t half = (total_bytes / 2) & ~127u;

		if (half == 0 || half >= total_bytes)
			return -EINVAL;
		sof_bdl_entry(st, 0, chunk[0].phys, half, 0);
		sof_bdl_entry(st, 1, chunk[0].phys + half, total_bytes - half, 1);
		n = 2;
	}
	st->nbdl = n;

	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_TAG_MASK,
		      (uint32_t)st->tag << SOF_SD_CTL_TAG_SHIFT);
	sof_hda_w32(h, st->sd_off + SOF_SD_CBL, total_bytes);
	sof_hda_w16(h, st->sd_off + SOF_SD_FMT, format);
	sof_hda_w16(h, st->sd_off + SOF_SD_LVI, (uint16_t)(n - 1));
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPL, (uint32_t)st->bdl_phys);
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPU,
		    (uint32_t)(st->bdl_phys >> 32));
	if (h->posbuf)
		h->posbuf[st->index * 2] = 0;
	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_INT_MASK,
		      SOF_SD_CTL_INT_MASK);

	st->buffer_bytes = total_bytes;
	st->period_bytes = period_bytes;
	st->format = format;
	st->running = 0;
	return 0;
}

void sof_stream_spib(struct sof_hstream *st, int enable, uint32_t value)
{
	struct sof_host *h = st->host;
	uint32_t reg;
	uint64_t fl;

	if (!h->spbcap)
		return;
	reg = h->spbcap + SOF_SPB_BASE + SOF_SPB_STRIDE * (uint32_t)st->index +
	      SOF_SPB_SPIB;
	if (!enable)
		sof_hda_w32(h, reg, 0);
	spin_lock_irqsave(&h->reg_lock, &fl);
	sof_hda_upd32(h, h->spbcap + SOF_SPB_SPBFCCTL, 1u << st->index,
		      enable ? (1u << st->index) : 0);
	spin_unlock_irqrestore(&h->reg_lock, fl);
	if (enable)
		sof_hda_w32(h, reg, value);
	st->spib_on = enable;
}

int sof_stream_start(struct sof_hstream *st)
{
	struct sof_host *h = st->host;
	uint32_t last = 0;

	if (st->running)
		return 0;
	sof_intctl_stream(h, st->index, 1);
	sof_hda_upd32(h, st->sd_off + SOF_SD_CTL,
		      SOF_SD_CTL_RUN | SOF_SD_CTL_INT_MASK,
		      SOF_SD_CTL_RUN | SOF_SD_CTL_INT_MASK);
	if (sof_poll_hda32(h, st->sd_off + SOF_SD_CTL, SOF_SD_CTL_RUN,
			   SOF_SD_CTL_RUN, 1000, &last)) {
		sof_log(h, "stream %d did not start (CTL %#x)\n", st->index,
			last);
		return -EIO;
	}
	st->running = 1;
	return 0;
}

int sof_stream_stop(struct sof_hstream *st)
{
	struct sof_host *h = st->host;
	int rc = sof_stream_halt(st);

	sof_intctl_stream(h, st->index, 0);
	st->running = 0;
	return rc;
}

void sof_stream_release_hw(struct sof_hstream *st)
{
	struct sof_host *h = st->host;

	sof_stream_stop(st);
	if (st->spib_on)
		sof_stream_spib(st, 0, 0);
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPL, 0);
	sof_hda_w32(h, st->sd_off + SOF_SD_BDLPU, 0);
	if (!st->link_in_use)
		sof_ppctl_procen(h, st->index, 0);
	st->buffer_bytes = 0;
	st->period_bytes = 0;
	st->nbdl = 0;
}

/* The DMA position: the DPIB register reflects what really moved. */
uint32_t sof_stream_position(struct sof_hstream *st)
{
	struct sof_host *h = st->host;
	uint32_t pos;

	if (!st->buffer_bytes)
		return 0;
	pos = sof_hda_r32(h, SOF_HDA_VS_DPIB_BASE +
				     SOF_HDA_VS_DPIB_STRIDE * (uint32_t)st->index);
	if (pos == 0xffffffffu)
		return 0;
	/* DPIB reads the buffer length at the instant it wraps: that is the
	 * start of the ring again.  Never LPIB instead: with the engine
	 * decoupled LPIB is the link side's counter, which nothing keeps for
	 * a DSP-fed stream: a stale value read at a wrap makes the position
	 * jump, which the core takes for a lap of the ring. */
	if (pos >= st->buffer_bytes)
		pos = 0;
	return pos;
}

/* ---- link DMA (link side of a decoupled engine) ------------------------ */

static uint32_t sof_pplc_off(struct sof_hstream *st)
{
	struct sof_host *h = st->host;

	return h->ppcap + SOF_PP_PPLC_BASE +
	       SOF_PP_PPHC_STRIDE * (uint32_t)h->nstreams +
	       SOF_PP_PPLC_STRIDE * (uint32_t)st->index;
}

int sof_link_dma_setup(struct sof_hstream *st, uint16_t format)
{
	struct sof_host *h = st->host;
	uint32_t off;
	uint32_t last = 0;

	if (!h->ppcap)
		return -ENODEV;
	off = sof_pplc_off(st);
	if (off + 0x10 > h->bar0_map_size)
		return -ENODEV;
	sof_ppctl_procen(h, st->index, 1);
	sof_hda_upd32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_RUN, 0);
	sof_hda_upd32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_STRST,
		      SOF_PPLC_CTL_STRST);
	if (sof_poll_hda32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_STRST,
			   SOF_PPLC_CTL_STRST, 1000, &last))
		sof_log(h, "link DMA %d did not enter reset (%#x)\n", st->index,
			last);
	sof_hda_upd32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_STRST, 0);
	if (sof_poll_hda32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_STRST, 0, 1000,
			   &last)) {
		sof_log(h, "link DMA %d did not leave reset (%#x)\n",
			st->index, last);
		return -EIO;
	}
	sof_hda_upd32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_STRM_MASK,
		      (uint32_t)st->tag << SOF_PPLC_CTL_STRM_SHIFT);
	sof_hda_w32(h, off + SOF_PPLC_FMT, format);
	return 0;
}

int sof_link_dma_run(struct sof_hstream *st, int run)
{
	struct sof_host *h = st->host;
	uint32_t off;

	if (!h->ppcap)
		return -ENODEV;
	off = sof_pplc_off(st);
	sof_hda_upd32(h, off + SOF_PPLC_CTL, SOF_PPLC_CTL_RUN,
		      run ? SOF_PPLC_CTL_RUN : 0);
	st->link_running = run;
	return 0;
}

/* Output stream id valid bits of an HDA link (codecs on that link listen
 * to the stream tag). */
void sof_link_losidv(struct sof_host *h, int link, int tag, int on)
{
	uint64_t fl;
	uint16_t v;

	if (link < 0 || link >= h->nlinks || h->link[link].alt || tag <= 0 ||
	    tag > 15)
		return;
	spin_lock_irqsave(&h->reg_lock, &fl);
	v = sof_hda_r16(h, h->link[link].off + SOF_ML_LOSIDV);
	if (on)
		v = (uint16_t)(v | (1u << tag));
	else
		v = (uint16_t)(v & ~(1u << tag));
	sof_hda_w16(h, h->link[link].off + SOF_ML_LOSIDV, v);
	spin_unlock_irqrestore(&h->reg_lock, fl);
}

/* The standard HDA stream format word. */
uint16_t sof_hda_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
	static const struct {
		uint32_t rate;
		uint16_t bits;
	} rates[] = {
		{ 8000, 0x0500 },   { 11025, 0x4300 }, { 16000, 0x0200 },
		{ 22050, 0x4100 },  { 32000, 0x0a00 }, { 44100, 0x4000 },
		{ 48000, 0x0000 },  { 88200, 0x4800 }, { 96000, 0x0800 },
		{ 176400, 0x5800 }, { 192000, 0x1800 }, { 384000, 0x1800 },
	};
	uint16_t v = 0xffff;
	size_t i;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i].rate == rate) {
			v = rates[i].bits;
			break;
		}
	}
	if (v == 0xffff || channels == 0 || channels > 16)
		return 0;
	switch (bits) {
	case 8:
		break;
	case 16:
		v |= 0x10;
		break;
	case 20:
		v |= 0x20;
		break;
	case 24:
		v |= 0x30;
		break;
	case 32:
		v |= 0x40;
		break;
	default:
		return 0;
	}
	v |= (uint16_t)(channels - 1);
	return v;
}

/* From the interrupt handler: acknowledge the stream status of every
 * engine INTSTS names, and tell the PCM core about completed periods. */
void sof_streams_irq(struct sof_host *h, uint32_t intsts)
{
	struct snd_pcm_substream *elapsed[SOF_MAX_STREAMS];
	int nelapsed = 0;
	uint64_t fl;
	int i;

	spin_lock_irqsave(&h->reg_lock, &fl);
	for (i = 0; i < h->nstreams; i++) {
		struct sof_hstream *st = &h->st[i];
		uint8_t sts;

		if (!(intsts & (1u << i)))
			continue;
		sts = sof_hda_r8(h, st->sd_off + SOF_SD_STS);
		if (sts == 0xff)
			continue; /* gone */
		sof_hda_w8(h, st->sd_off + SOF_SD_STS, sts & SOF_SD_STS_MASK);
		st->irq_count++;
		if (sts & SOF_SD_STS_FIFOE)
			st->fifo_errors++;
		if (sts & SOF_SD_STS_DESE)
			st->desc_errors++;
		if (!(sts & SOF_SD_STS_BCIS))
			continue;
		st->ioc_count++;
		if (st->running && st->substream && st->host_in_use)
			elapsed[nelapsed++] = st->substream;
	}
	spin_unlock_irqrestore(&h->reg_lock, fl);

	for (i = 0; i < nelapsed; i++)
		snd_pcm_period_elapsed(elapsed[i]);
}
