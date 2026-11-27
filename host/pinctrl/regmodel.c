/* Register model of the GPIO controllers (host/test-pinctrl.sh); see
 * regmodel.h.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/pinctrl/regmodel.h"

#define RM_MAX_WIN 32
#define RM_MAX_PADS 64
#define RM_LOG 65536

struct rm_win {
	uint8_t *base;
	size_t size;
	uint8_t *attr; /* per dword */
};

struct rm_pad {
	uint8_t *padcfg0;
	uint8_t *is;
	uint32_t bit;
	int level;
};

static struct rm_win g_win[RM_MAX_WIN];
static int g_nwin;
static struct rm_pad g_pads[RM_MAX_PADS];
static int g_npads;
static struct {
	uint8_t *addr;
	uint32_t val;
} g_log[RM_LOG];
static unsigned long g_nlog, g_nreads;
static uint8_t *g_amd_base;
static int g_amd_level[256];
static spinlock_t g_rm_lock = SPINLOCK_INIT("regmodel");

void rm_reset(void)
{
	for (int i = 0; i < g_nwin; i++) {
		kfree(g_win[i].base);
		kfree(g_win[i].attr);
	}
	g_nwin = 0;
	g_npads = 0;
	g_nlog = 0;
	g_nreads = 0;
	g_amd_base = NULL;
	kmemset(g_amd_level, 0, sizeof(g_amd_level));
}

uint8_t *rm_window(size_t size)
{
	struct rm_win *w;

	T_ASSERT(g_nwin < RM_MAX_WIN);
	w = &g_win[g_nwin++];
	w->base = kcalloc(1, size);
	w->attr = kcalloc(1, size / 4 + 1);
	w->size = size;
	T_ASSERT(w->base && w->attr);
	return w->base;
}

static struct rm_win *win_of(const void *addr, size_t *off)
{
	for (int i = 0; i < g_nwin; i++) {
		const uint8_t *b = g_win[i].base;

		if ((const uint8_t *)addr >= b &&
		    (const uint8_t *)addr + 4 <= b + g_win[i].size) {
			*off = (size_t)((const uint8_t *)addr - b);
			return &g_win[i];
		}
	}
	return NULL;
}

void rm_set_attr(void *addr, int attr)
{
	size_t off;
	struct rm_win *w = win_of(addr, &off);

	T_ASSERT(w && (off & 3) == 0);
	w->attr[off / 4] = (uint8_t)attr;
}

void rm_set_attr_range(void *addr, size_t bytes, int attr)
{
	for (size_t i = 0; i < bytes; i += 4)
		rm_set_attr((uint8_t *)addr + i, attr);
}

uint32_t rm_peek(const void *addr)
{
	return *(const uint32_t *)addr;
}

void rm_poke(void *addr, uint32_t v)
{
	*(uint32_t *)addr = v;
}

void rm_log_clear(void)
{
	g_nlog = 0;
	g_nreads = 0;
}

unsigned long rm_writes_in(const void *addr, size_t bytes)
{
	unsigned long n = 0;

	for (unsigned long i = 0; i < g_nlog && i < RM_LOG; i++)
		if (g_log[i].addr >= (const uint8_t *)addr &&
		    g_log[i].addr < (const uint8_t *)addr + bytes)
			n++;
	return n;
}

unsigned long rm_reads(void)
{
	return g_nreads;
}

/* ---- Intel pad inputs ---------------------------------------------------- */

#define PADCFG0_RXEVCFG(v) (((v) >> 25) & 3u)
#define PADCFG0_RXINV (1u << 23)
#define PADCFG0_RXSTATE (1u << 1)

static int pad_effective(const struct rm_pad *p)
{
	uint32_t cfg = rm_peek(p->padcfg0);

	return p->level ^ !!(cfg & PADCFG0_RXINV);
}

/* Level-mode pads latch again while active (rm lock held). */
static void relatch_levels(void)
{
	for (int i = 0; i < g_npads; i++) {
		struct rm_pad *p = &g_pads[i];

		if (PADCFG0_RXEVCFG(rm_peek(p->padcfg0)) == 0 &&
		    pad_effective(p))
			rm_poke(p->is, rm_peek(p->is) | p->bit);
	}
}

int rm_intel_pad(void *padcfg0, void *is_reg, uint32_t bit)
{
	T_ASSERT(g_npads < RM_MAX_PADS);
	g_pads[g_npads].padcfg0 = padcfg0;
	g_pads[g_npads].is = is_reg;
	g_pads[g_npads].bit = bit;
	g_pads[g_npads].level = (rm_peek(padcfg0) & PADCFG0_RXSTATE) ? 1 : 0;
	return g_npads++;
}

void rm_intel_drive(int pad, int level)
{
	struct rm_pad *p = &g_pads[pad];
	uint64_t fl;
	int before, after;
	uint32_t cfg;

	spin_lock_irqsave(&g_rm_lock, &fl);
	before = pad_effective(p);
	p->level = level ? 1 : 0;
	cfg = rm_peek(p->padcfg0);
	cfg = level ? (cfg | PADCFG0_RXSTATE) : (cfg & ~PADCFG0_RXSTATE);
	rm_poke(p->padcfg0, cfg);
	after = pad_effective(p);
	switch (PADCFG0_RXEVCFG(cfg)) {
	case 0: /* level */
		if (after)
			rm_poke(p->is, rm_peek(p->is) | p->bit);
		break;
	case 1: /* edge (RXINV picks falling) */
		if (!before && after)
			rm_poke(p->is, rm_peek(p->is) | p->bit);
		break;
	case 3: /* both edges */
		if (before != after)
			rm_poke(p->is, rm_peek(p->is) | p->bit);
		break;
	default: /* disabled */
		break;
	}
	spin_unlock_irqrestore(&g_rm_lock, fl);
}

/* ---- AMD pins ----------------------------------------------------------------- */

#define AMD_LEVEL_TRIG (1u << 8)
#define AMD_ACTIVE_LEVEL(v) (((v) >> 9) & 3u)
#define AMD_INT_ENABLE (1u << 11)
#define AMD_PIN_STS (1u << 16)
#define AMD_INT_STS (1u << 28)
#define AMD_WAKE_STS (1u << 29)

void rm_amd_set_base(uint8_t *base)
{
	g_amd_base = base;
}

static int amd_active(unsigned int pin)
{
	uint32_t v = rm_peek(g_amd_base + pin * 4);
	int lvl = g_amd_level[pin];

	return AMD_ACTIVE_LEVEL(v) == 1 ? !lvl : lvl;
}

static void amd_relatch(void)
{
	for (unsigned int pin = 0; pin < 184; pin++) {
		uint32_t v = rm_peek(g_amd_base + pin * 4);

		if ((v & AMD_LEVEL_TRIG) && (v & AMD_INT_ENABLE) &&
		    amd_active(pin))
			rm_poke(g_amd_base + pin * 4, v | AMD_INT_STS);
	}
}

void rm_amd_drive(unsigned int pin, int level)
{
	uint32_t v = rm_peek(g_amd_base + pin * 4);
	int before = amd_active(pin), after;

	g_amd_level[pin] = level ? 1 : 0;
	v = level ? (v | AMD_PIN_STS) : (v & ~AMD_PIN_STS);
	rm_poke(g_amd_base + pin * 4, v);
	after = amd_active(pin);
	if (!(v & AMD_INT_ENABLE))
		return;
	if (v & AMD_LEVEL_TRIG) {
		if (after)
			rm_poke(g_amd_base + pin * 4, v | AMD_INT_STS);
	} else if (AMD_ACTIVE_LEVEL(v) == 2 ? before != after :
					     (!before && after)) {
		rm_poke(g_amd_base + pin * 4, v | AMD_INT_STS);
	}
}

/* ---- accessors ------------------------------------------------------------- */

uint32_t model_readl(const volatile void *addr)
{
	size_t off;
	struct rm_win *w = win_of((const void *)addr, &off);

	T_ASSERT(w != NULL); /* an access outside every window */
	T_ASSERT((off & 3) == 0);
	g_nreads++;
	if (g_amd_base && w->base == g_amd_base &&
	    (off == 0x2f8 || off == 0x2fc)) {
		/* the summary: one bit per four pins with a status */
		uint64_t sum = 0;

		for (unsigned int pin = 0; pin < 184; pin++)
			if (pin * 4 != 0xfc &&
			    (rm_peek(g_amd_base + pin * 4) &
			     (AMD_INT_STS | AMD_WAKE_STS)))
				sum |= 1ull << (pin / 4);
		return off == 0x2f8 ? (uint32_t)sum : (uint32_t)(sum >> 32);
	}
	return rm_peek((const void *)addr);
}

void model_writel(uint32_t v, volatile void *addr)
{
	size_t off;
	struct rm_win *w = win_of((const void *)addr, &off);
	uint32_t old;
	uint64_t fl;

	T_ASSERT(w != NULL);
	T_ASSERT((off & 3) == 0);
	spin_lock_irqsave(&g_rm_lock, &fl);
	if (g_nlog < RM_LOG) {
		g_log[g_nlog].addr = (uint8_t *)addr;
		g_log[g_nlog].val = v;
	}
	g_nlog++;
	old = rm_peek((const void *)addr);
	switch (w->attr[off / 4]) {
	case RM_RO:
		break;
	case RM_W1C:
		rm_poke((void *)addr, old & ~v);
		break;
	case RM_AMDPIN: {
		uint32_t sts = (AMD_INT_STS | AMD_WAKE_STS);

		rm_poke((void *)addr,
			((v & ~sts) & ~AMD_PIN_STS) | (old & AMD_PIN_STS) |
				((old & sts) & ~(v & sts)));
		break;
	}
	default:
		rm_poke((void *)addr, v);
		break;
	}
	relatch_levels();
	if (g_amd_base && w->base == g_amd_base)
		amd_relatch();
	spin_unlock_irqrestore(&g_rm_lock, fl);
}
