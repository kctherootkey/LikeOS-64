/* Host test of the GPIO core and the Intel pin controller drivers
 * (kernel/dev/gpio, kernel/dev/gpio/pinctrl/intel) over a register model
 * (regmodel.c), a model of the interrupt core (host/kshim/irq_model.c),
 * the real work queues and the real ACPI device layer over a modelled
 * namespace (host/kshim/acpica_mock.c).
 *
 *   * every SoC table (every ACPI id and _UID): groups tile their
 *     communities, per-group register arrays do not run into each other,
 *     GPIO numbers do not overlap, GPIO <-> pin round trips; and each
 *     table cross-checked against expectations taken from the published
 *     Intel sources by an independent parser (expect_tables.h):
 *     communities, offsets, groups, GPIO bases, first and last pin;
 *   * datasheet anchors: register offsets of Tiger Lake-LP/-H pads and
 *     groups as the PCH datasheets list them;
 *   * community features from REVID (2 or 4 dwords per pad, 3-bit
 *     PAD_OWN) and the capability list;
 *   * a Tiger Lake-LP controller probed from the namespace, touchpad
 *     GpioInt resolved to an interrupt, the interrupt flows, ownership,
 *     ACPI mode, locked pads, pulls, debounce, direction, wake (with a
 *     DMI quirk ignoring one device's wake flag), unexpected interrupts,
 *     _AEI events, the GPIO operation region, system sleep;
 *   * a firmware-described controller (INTC105F, _DSD) with every kind
 *     of data node link;
 *   * Lynx Point;
 *   * DMI decoding and matching, the ignore lists.
 *
 * Bay Trail, Cherry View and AMD are in test_atom_amd.c.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/kshim/acpica_mock.h"
#include "host/pinctrl/expect_tables.h"
#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>
#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/dev/gpio/gpiolib-acpi.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/workqueue.h>

int irqm_fire_gsi(int gsi);
int irqm_run_threads(void);
unsigned long irqm_count(int irq);
void test_byt(void);
void test_chv(void);
void test_amd(void);

/* Register bits (the driver keeps its own copies private). */
#define INTEL_REVID 0x000
#define INTEL_CAPLIST 0x004
#define INTEL_PADBAR 0x00c
#define INTEL_PADCFG0_RXEVCFG_MASK (3u << 25)
#define INTEL_PADCFG0_RXEVCFG_LEVEL 0u
#define INTEL_PADCFG0_RXEVCFG_EDGE 1u
#define INTEL_PADCFG0_RXEVCFG_EDGE_BOTH 3u
#define INTEL_PADCFG0_PREGFRXSEL (1u << 24)
#define INTEL_PADCFG0_RXINV (1u << 23)
#define INTEL_PADCFG0_GPIROUTIOXAPIC (1u << 20)
#define INTEL_PADCFG0_GPIROUTSCI (1u << 19)
#define INTEL_PADCFG0_GPIROUTSMI (1u << 18)
#define INTEL_PADCFG0_GPIROUTNMI (1u << 17)
#define INTEL_PADCFG0_PMODE_MASK (0xfu << 10)
#define INTEL_PADCFG0_GPIORXDIS (1u << 9)
#define INTEL_PADCFG0_GPIOTXDIS (1u << 8)
#define INTEL_PADCFG0_GPIORXSTATE (1u << 1)
#define INTEL_PADCFG0_GPIOTXSTATE (1u << 0)
#define INTEL_PADCFG1_TERM_UP (1u << 13)
#define INTEL_PADCFG1_TERM_MASK (7u << 10)
#define INTEL_PADCFG1_TERM_20K 4u
#define INTEL_PADCFG1_TERM_5K 2u
#define INTEL_PADCFG1_TERM_1K 1u
#define INTEL_PADCFG1_TERM_833 3u

/* ==========================================================================
 * Stubs of the firmware-table access the harness does not model
 * ========================================================================== */

uint64_t g_boot_smbios_entry;
uint64_t g_boot_smbios3_entry;
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
	(void)where;
	(void)length;
	return NULL;
}
void AcpiOsUnmapMemory(void *where, ACPI_SIZE length)
{
	(void)where;
	(void)length;
}
static const char *g_cmdline = "";
const char *storage_boot_cmdline(void)
{
	return g_cmdline;
}

/* ==========================================================================
 * Table consistency and cross-check, every SoC
 * ========================================================================== */

static unsigned int gpp_padown_regs(const struct intel_community *c,
				    const struct intel_padgroup *g,
				    const struct intel_community *tbl)
{
	(void)c;
	if (tbl->gpps)
		return (g->size * 4 + 31) / 32;
	return tbl->gpp_num_padown_regs;
}

/* The next register block after `off' in the community header. */
static unsigned int next_block(const struct intel_community *c,
			       unsigned int off)
{
	unsigned int cand[4] = { c->padcfglock_offset, c->hostown_offset,
				 c->is_offset, c->ie_offset };
	unsigned int best = 0x700;

	for (int i = 0; i < 4; i++)
		if (cand[i] > off && cand[i] < best)
			best = cand[i];
	return best;
}

static int g_tables_checked;

static struct intel_pinctrl *setup_soc_data(const struct intel_pinctrl_soc_data *soc,
					    uint32_t rev, uint32_t padbar)
{
	struct intel_pinctrl *p = kcalloc(1, sizeof(*p));
	uint8_t *win[8];
	uint64_t len[8];

	T_ASSERT(soc && p);
	rm_reset();
	for (int b = 0; b < 8; b++) {
		win[b] = rm_window(0x10000);
		len[b] = 0x10000;
		rm_poke(win[b] + INTEL_REVID, rev << 16);
		rm_poke(win[b] + INTEL_PADBAR, padbar);
	}
	T_EQ(intel_pinctrl_setup(p, soc, win, len, 8), 0);
	return p;
}

static void check_soc(const struct intel_pinctrl_soc_data *soc)
{
	struct intel_pinctrl *p = setup_soc_data(soc, 0x94, 0x700);
	unsigned int total = 0;

	for (size_t i = 0; i < p->ncommunities; i++) {
		const struct intel_community *c = &p->communities[i];
		const struct intel_community *tbl = &soc->communities[i];
		unsigned int expect_base = c->pin_base, maxreg = 0;
		unsigned int padown_end;

		total += (unsigned int)c->npins;
		T_ASSERT(c->ngpps > 0);
		T_ASSERT(c->features & PINCTRL_FEATURE_DEBOUNCE);
		/* groups tile the community in order */
		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];

			T_EQ(g->base, expect_base);
			T_ASSERT(g->size >= 1 && g->size <= 32);
			expect_base += g->size;
			if (g->reg_num > maxreg)
				maxreg = g->reg_num;
			T_EQ(g->reg_num, j);
			if (j)
				T_EQ(g->padown_num,
				     c->gpps[j - 1].padown_num +
					     gpp_padown_regs(c, &c->gpps[j - 1],
							     tbl));
		}
		T_EQ(expect_base, c->pin_base + c->npins);
		/* register arrays stay clear of each other */
		padown_end = c->padown_offset +
			     (c->gpps[c->ngpps - 1].padown_num +
			      gpp_padown_regs(c, &c->gpps[c->ngpps - 1], tbl)) *
				     4;
		T_ASSERT(padown_end <= next_block(c, c->padown_offset));
		T_ASSERT(c->padcfglock_offset + (maxreg + 1) * 8 <=
			 next_block(c, c->padcfglock_offset));
		T_ASSERT(c->hostown_offset + (maxreg + 1) * 4 <=
			 next_block(c, c->hostown_offset));
		T_ASSERT(c->is_offset + (maxreg + 1) * 4 <=
			 next_block(c, c->is_offset));
		T_ASSERT(c->ie_offset + (maxreg + 1) * 4 <= 0x700);
		/* every pin: pad registers inside the window, GPIO round
		 * trip, a name */
		for (unsigned int pin = c->pin_base;
		     pin < c->pin_base + c->npins; pin++) {
			uint8_t *r = intel_get_padcfg(p, pin, INTEL_PADCFG0);
			int gpio = intel_pin_to_gpio(p, pin);

			T_ASSERT(r >= c->regs + 0x700 &&
				 r + 16 <= c->regs + 0x10000);
			if (gpio >= 0) {
				T_EQ(intel_gpio_to_pin(p, (unsigned int)gpio,
						       NULL, NULL),
				     pin);
				T_ASSERT((unsigned int)gpio < p->chip.ngpio);
			}
		}
	}
	/* pins: dense, numbered as their index */
	T_EQ(total, soc->npins);
	for (size_t i = 0; i < soc->npins; i++) {
		T_EQ(soc->pins[i].number, i);
		T_ASSERT(soc->pins[i].name && soc->pins[i].name[0]);
	}
	/* GPIO numbers: groups never overlap */
	for (unsigned int gpio = 0; gpio < p->chip.ngpio; gpio++) {
		int pin = intel_gpio_to_pin(p, gpio, NULL, NULL);
		int hits = 0;

		for (size_t i = 0; i < p->ncommunities; i++)
			for (size_t j = 0; j < p->communities[i].ngpps; j++) {
				const struct intel_padgroup *g =
					&p->communities[i].gpps[j];

				if (g->gpio_base >= 0 &&
				    gpio >= (unsigned int)g->gpio_base &&
				    gpio < (unsigned int)g->gpio_base + g->size)
					hits++;
			}
		T_ASSERT(hits <= 1);
		if (pin >= 0)
			T_EQ(intel_pin_to_gpio(p, (unsigned int)pin), gpio);
	}
	intel_pinctrl_teardown(p);
	kfree(p);
	g_tables_checked++;
}

/* The soc data the driver picks for a device of `hid' (and _UID). */
static const struct intel_pinctrl_soc_data *lookup(const char *hid,
						   const char *uid)
{
	struct acpi_device *a = kcalloc(1, sizeof(*a));
	const struct intel_pinctrl_soc_data *s = NULL;

	ksnprintf(a->hid, sizeof(a->hid), "%s", hid);
	a->has_hid = 1;
	if (uid) {
		ksnprintf(a->uid, sizeof(a->uid), "%s", uid);
		a->has_uid = 1;
	}
	for (unsigned int t = 0; intel_pinctrl_tables[t] && !s; t++)
		s = intel_pinctrl_lookup(intel_pinctrl_tables[t], a);
	kfree(a);
	return s;
}

/* One table against the expectation from the published source. */
static void cross_check(const struct exp_soc *e)
{
	const struct intel_pinctrl_soc_data *s = lookup(e->hid, e->uid);

	T_ASSERT(s);
	T_EQ(s->npins, e->npins);
	T_EQ(s->pins[0].number, e->first_pin);
	T_EQ(kstrcmp(s->pins[0].name, e->first_name), 0);
	T_EQ(s->pins[s->npins - 1].number, e->last_pin);
	T_EQ(kstrcmp(s->pins[s->npins - 1].name, e->last_name), 0);
	if (e->uid)
		T_EQ(kstrcmp(s->uid, e->uid), 0);
	T_EQ(s->ncommunities, e->ncomms);
	for (unsigned int i = 0; i < e->ncomms; i++) {
		const struct intel_community *c = &s->communities[i];
		const struct exp_comm *x = &e->comms[i];

		T_EQ(c->barno, x->barno);
		T_EQ(c->padown_offset, x->off[0]);
		T_EQ(c->padcfglock_offset, x->off[1]);
		T_EQ(c->hostown_offset, x->off[2]);
		T_EQ(c->is_offset, x->off[3]);
		T_EQ(c->ie_offset, x->off[4]);
		T_EQ(c->pin_base, x->pin_base);
		T_EQ(c->npins, x->npins);
		T_EQ(c->gpp_size, x->gpp_size);
		T_EQ(c->gpp_num_padown_regs, x->gpp_npadown);
		T_EQ(c->features, 0);
		T_EQ(c->ngpps, x->ngpps);
		for (unsigned int j = 0; j < x->ngpps; j++) {
			T_EQ(c->gpps[j].reg_num, x->gpps[j].reg);
			T_EQ(c->gpps[j].base, x->gpps[j].base);
			T_EQ(c->gpps[j].size, x->gpps[j].size);
			T_EQ(c->gpps[j].gpio_base, x->gpps[j].gpio_base);
		}
	}
}

static int table_has_hid(const char *hid)
{
	for (unsigned int i = 0; i < sizeof(g_expect) / sizeof(g_expect[0]);
	     i++)
		if (!kstrcmp(g_expect[i].hid, hid))
			return 1;
	return 0;
}

static void t_tables(void)
{
	static const char *const ids[] = {
		"INT344B", "INT3451", "INT345D", "INT3450", "INT34BB",
		"INTC3001", "INTC3000", "INT3536", "INTC1071", "INT3455",
		"INT34C3", "INT34C4", "INT34C5", "INT34C6", "INTC1055",
		"INT34C8", "INTC1020", "INTC1056", "INTC1057", "INTC1085",
		"INTC105E", "INTC1083", "INTC1082", "INTC1084", "INT3452",
		"INT34D1", "INT3453",
	};
	unsigned int nids = 0;

	/* every table entry, every _UID */
	for (unsigned int t = 0; intel_pinctrl_tables[t]; t++)
		for (const struct intel_pinctrl_match *m =
			     intel_pinctrl_tables[t];
		     m->hid; m++) {
			T_ASSERT(table_has_hid(m->hid));
			if (m->data) {
				check_soc(m->data);
			} else {
				for (const struct intel_pinctrl_soc_data *const
					     *s = m->uid_table;
				     *s; s++)
					check_soc(*s);
			}
			nids++;
		}
	/* Lynx Point's table in the common format */
	check_soc(intel_lynxpoint_match[0].data);
	T_ASSERT(intel_lynxpoint_match[0].data == intel_lynxpoint_match[1].data);
	T_EQ(kstrcmp(intel_lynxpoint_match[0].hid, "INT33C7"), 0);
	T_EQ(kstrcmp(intel_lynxpoint_match[1].hid, "INT3437"), 0);
	/* every expectation */
	for (unsigned int i = 0; i < sizeof(g_expect) / sizeof(g_expect[0]);
	     i++)
		cross_check(&g_expect[i]);
	/* the ids the community driver answers to: exactly these */
	T_EQ(nids, sizeof(ids) / sizeof(ids[0]));
	for (unsigned int i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
		T_ASSERT(lookup(ids[i], "1") != NULL);
	/* a per-_UID controller without a known _UID gets nothing */
	T_ASSERT(lookup("INT3452", "9") == NULL);
	T_ASSERT(lookup("INT3452", NULL) == NULL);
	T_ASSERT(lookup("INTC1020", "5") != NULL);
	T_ASSERT(lookup("INTC1020", "6") == NULL);
	/* shared tables */
	T_ASSERT(lookup("INT3451", NULL) == lookup("INT345D", NULL));
	T_ASSERT(lookup("INT34C5", NULL) == lookup("INTC1055", NULL));
	T_ASSERT(lookup("INTC1056", NULL) == lookup("INTC1085", NULL));
	T_ASSERT(lookup("INTC105E", NULL) == lookup("INTC1083", NULL));
	kprintf("  %d SoC tables (per id and _UID) consistent, %u cross-checked\n",
		g_tables_checked,
		(unsigned int)(sizeof(g_expect) / sizeof(g_expect[0])));
}

/* ==========================================================================
 * Datasheet anchors
 * ========================================================================== */

/* Offset (from the community window) of a pin's PAD_OWN register. */
static unsigned int padown_off(struct intel_pinctrl *p, unsigned int pin)
{
	for (size_t i = 0; i < p->ncommunities; i++) {
		const struct intel_community *c = &p->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			const struct intel_padgroup *g = &c->gpps[j];

			if (pin >= g->base && pin < g->base + g->size)
				return c->padown_offset + g->padown_num * 4 +
				       ((pin - g->base) / 8) * 4;
		}
	}
	return 0;
}

static const struct intel_padgroup *group_of(struct intel_pinctrl *p,
					     unsigned int pin,
					     const struct intel_community **cc)
{
	for (size_t i = 0; i < p->ncommunities; i++) {
		const struct intel_community *c = &p->communities[i];

		for (size_t j = 0; j < c->ngpps; j++)
			if (pin >= c->gpps[j].base &&
			    pin < c->gpps[j].base + c->gpps[j].size) {
				*cc = c;
				return &c->gpps[j];
			}
	}
	return NULL;
}

static unsigned int padcfg_off(struct intel_pinctrl *p, unsigned int pin)
{
	const struct intel_community *c;

	(void)group_of(p, pin, &c);
	return (unsigned int)(intel_get_padcfg(p, pin, INTEL_PADCFG0) -
			      c->regs);
}

static struct intel_pinctrl *setup_soc(const char *hid, uint32_t padbar)
{
	return setup_soc_data(lookup(hid, NULL), 0x94, padbar);
}

static void t_datasheet(void)
{
	struct intel_pinctrl *p = setup_soc("INT34C5", 0x700);
	const struct intel_community *c;
	const struct intel_padgroup *g;

	/* 600-series on-package PCH (Tiger Lake-LP layout), community 0 */
	T_EQ(padown_off(p, 0), 0x20); /* GPP_B_0 */
	T_EQ(padown_off(p, 26), 0x30); /* GPP_T_0 */
	T_EQ(padown_off(p, 42), 0x38); /* GPP_A_0 */
	T_EQ(padown_off(p, 50), 0x3c); /* GPP_A_8 */
	T_EQ(padcfg_off(p, 26 + 2), 0x8c0); /* GPPC_T_2 */
	T_EQ(padcfg_off(p, 42), 0x9a0); /* GPPC_A_0 */
	T_EQ(padcfg_off(p, 42 + 23), 0xb10); /* GPPC_A_23 */
	g = group_of(p, 42, &c);
	T_EQ(c->padcfglock_offset + g->reg_num * 8, 0x90);
	T_EQ(c->hostown_offset + g->reg_num * 4, 0xb8);
	T_EQ(c->is_offset + g->reg_num * 4, 0x108);
	T_EQ(c->ie_offset + g->reg_num * 4, 0x128);
	/* community 1: GPP_S, GPP_H, GPP_D */
	T_EQ(padown_off(p, 75), 0x24);
	T_EQ(padown_off(p, 99), 0x30);
	T_EQ(padcfg_off(p, 75), 0x780);
	T_EQ(padcfg_off(p, 99), 0x900);
	T_EQ(padcfg_off(p, 99 + 19), 0xa30);
	/* community 4: GPP_C, GPP_F, HVCMOS, GPP_E */
	T_EQ(padown_off(p, 195), 0x2c);
	T_EQ(padown_off(p, 226), 0x40);
	T_EQ(padcfg_off(p, 195), 0x880);
	T_EQ(padcfg_off(p, 226), 0xa70);
	T_EQ(padcfg_off(p, 226 + 23), 0xbe0);
	g = group_of(p, 226, &c);
	T_EQ(c->padcfglock_offset + g->reg_num * 8, 0x98);
	T_EQ(c->hostown_offset + g->reg_num * 4, 0xbc);
	T_EQ(c->is_offset + g->reg_num * 4, 0x10c);
	/* GPP_E7's GPIO number: E starts at 320 */
	T_EQ(intel_pin_to_gpio(p, 226 + 7), 327);
	/* the pin names */
	T_EQ(kstrcmp(p->soc->pins[0].name, "CORE_VID_0"), 0);
	intel_pinctrl_teardown(p);
	kfree(p);

	/* 500-series PCH-H (Tiger Lake-H) */
	p = setup_soc("INT34C6", 0x600);
	T_EQ(padown_off(p, 25), 0x30); /* GPP_R_0 */
	T_EQ(padown_off(p, 45), 0x3c); /* GPP_B_0 */
	T_EQ(padcfg_off(p, 25), 0x790);
	T_EQ(padcfg_off(p, 45), 0x8d0);
	T_EQ(padcfg_off(p, 45 + 23), 0xa40);
	g = group_of(p, 45, &c);
	T_EQ(c->padcfglock_offset + g->reg_num * 8, 0xa0);
	T_EQ(c->hostown_offset + g->reg_num * 4, 0xc8);
	T_EQ(padown_off(p, 105), 0x30); /* GPP_C_0 */
	T_EQ(padown_off(p, 129), 0x3c); /* GPP_S_0 */
	T_EQ(padown_off(p, 137), 0x40); /* GPP_G_0 */
	T_EQ(padcfg_off(p, 105), 0x7a0);
	T_EQ(padcfg_off(p, 129), 0x920);
	T_EQ(padcfg_off(p, 137), 0x9a0);
	g = group_of(p, 137, &c);
	T_EQ(c->padcfglock_offset + g->reg_num * 8, 0xa8);
	T_EQ(c->hostown_offset + g->reg_num * 4, 0xcc);
	T_EQ(c->is_offset + g->reg_num * 4, 0x10c);
	T_EQ(padown_off(p, 194), 0x28); /* GPP_F_0 */
	T_EQ(padcfg_off(p, 194), 0x6d0);
	T_EQ(padown_off(p, 242), 0x2c); /* GPP_J_0 */
	T_EQ(padown_off(p, 252), 0x34); /* GPP_K_0 */
	T_EQ(padcfg_off(p, 242), 0x780);
	T_EQ(padcfg_off(p, 252), 0x820);
	intel_pinctrl_teardown(p);
	kfree(p);
}

/* ==========================================================================
 * Community features: revision and capability list
 * ========================================================================== */

static void t_features(void)
{
	const struct intel_pinctrl_soc_data *spt = lookup("INT344B", NULL);
	struct intel_pinctrl *p;
	struct intel_pinctrl q;
	uint8_t *win[8];
	uint64_t len[8];

	/* revision 0x80: two dwords per pad, no debounce, no 1k pull-down */
	p = setup_soc_data(spt, 0x80, 0x400);
	T_EQ(intel_get_padcfg(p, 1, INTEL_PADCFG0) -
		     intel_get_padcfg(p, 0, INTEL_PADCFG0),
	     8);
	T_ASSERT(intel_get_padcfg(p, 1, INTEL_PADCFG2) == NULL);
	T_EQ(p->communities[0].features & (PINCTRL_FEATURE_DEBOUNCE |
					   PINCTRL_FEATURE_1K_PD),
	     0);
	T_EQ(p->chip.set_config(&p->chip, 1, PIN_CONFIG_INPUT_DEBOUNCE, 1000),
	     -ENOTSUPP);
	T_EQ(p->chip.set_config(&p->chip, 1, PIN_CONFIG_BIAS_PULL_DOWN, 1000),
	     -EINVAL);
	T_EQ(p->chip.set_config(&p->chip, 1, PIN_CONFIG_BIAS_PULL_UP, 1000), 0);
	intel_pinctrl_teardown(p);
	kfree(p);
	/* revision 0x092 is the first with PADCFG2 */
	p = setup_soc_data(spt, 0x92, 0x400);
	T_ASSERT(intel_get_padcfg(p, 1, INTEL_PADCFG2) != NULL);
	T_EQ(p->communities[0].features & PINCTRL_FEATURE_3BIT_PAD_OWN, 0);
	intel_pinctrl_teardown(p);
	kfree(p);
	/* revision 0x110: one 3-bit PAD_OWN register per pad */
	p = setup_soc_data(spt, 0x110, 0x400);
	T_ASSERT(p->communities[0].features & PINCTRL_FEATURE_3BIT_PAD_OWN);
	T_ASSERT(intel_pad_owned_by_host(p, 5));
	rm_poke(p->communities[0].regs + 0x20 + 5 * 4, 8);
	T_ASSERT(intel_pad_owned_by_host(p, 5)); /* bit 3 is not an owner bit */
	rm_poke(p->communities[0].regs + 0x20 + 5 * 4, 4);
	T_ASSERT(!intel_pad_owned_by_host(p, 5));
	rm_poke(p->communities[0].regs + 0x20 + 5 * 4, 1);
	T_ASSERT(!intel_pad_owned_by_host(p, 5));
	T_ASSERT(intel_pad_owned_by_host(p, 4));
	intel_pinctrl_teardown(p);
	kfree(p);

	/* the capability list: PWM at 0x20, then BLINK at 0x30 */
	rm_reset();
	for (int b = 0; b < 8; b++) {
		win[b] = rm_window(0x10000);
		len[b] = 0x10000;
		rm_poke(win[b] + INTEL_REVID, 0x00940000u);
		rm_poke(win[b] + INTEL_PADBAR, 0x400);
		rm_poke(win[b] + INTEL_CAPLIST, (2u << 16) | 0x20);
		rm_poke(win[b] + 0x20, (3u << 16) | 0x30);
		rm_poke(win[b] + 0x30, 4u << 16);
	}
	kmemset(&q, 0, sizeof(q));
	T_EQ(intel_pinctrl_setup(&q, spt, win, len, 8), 0);
	T_EQ(q.communities[0].features &
		     (PINCTRL_FEATURE_PWM | PINCTRL_FEATURE_BLINK |
		      PINCTRL_FEATURE_EXP | PINCTRL_FEATURE_GPIO_HW_INFO),
	     PINCTRL_FEATURE_PWM | PINCTRL_FEATURE_BLINK | PINCTRL_FEATURE_EXP);
	T_EQ(q.communities[0].cap_offset[2], 0x04);
	T_EQ(q.communities[0].cap_offset[3], 0x20);
	T_EQ(q.communities[0].cap_offset[4], 0x30);
	intel_pinctrl_teardown(&q);
	/* a list that loops ends */
	rm_poke(win[0] + 0x30, (4u << 16) | 0x20);
	kmemset(&q, 0, sizeof(q));
	T_EQ(intel_pinctrl_setup(&q, spt, win, len, 8), 0);
	intel_pinctrl_teardown(&q);
	/* hardware that is not there, or a pad array that does not fit */
	rm_poke(win[1] + INTEL_REVID, 0xffffffffu);
	kmemset(&q, 0, sizeof(q));
	T_EQ(intel_pinctrl_setup(&q, spt, win, len, 8), -ENODEV);
	T_ASSERT(q.communities == NULL);
	rm_poke(win[1] + INTEL_REVID, 0x00940000u);
	rm_poke(win[2] + INTEL_PADBAR, 0xff00);
	kmemset(&q, 0, sizeof(q));
	T_EQ(intel_pinctrl_setup(&q, spt, win, len, 8), -ENODEV);
	rm_poke(win[2] + INTEL_PADBAR, 0x400);
	/* fewer memory resources than the table needs */
	kmemset(&q, 0, sizeof(q));
	T_EQ(intel_pinctrl_setup(&q, spt, win, len, 2), -ENODEV);
}

/* ==========================================================================
 * A Tiger Lake-LP controller from the namespace
 * ========================================================================== */

#define GSI_GPIO 14

struct tgl {
	uint8_t *win[4];
	uint8_t *pwin; /* the firmware-described controller */
	struct mock_node *gpi0, *tpd0, *dev2, *tpda, *tpdb, *gpi1;
	struct mock_node *evt, *l0a;
	struct intel_pinctrl *pctrl;
	struct gpio_chip *gc;
};

static struct tgl T;
static union {
	ACPI_RESOURCE r;
	UINT8 pad[sizeof(ACPI_RESOURCE) + 16];
} g_gpi_crs[8], g_gpi1_crs[4];
static ACPI_RESOURCE g_tp_crs[4], g_dev2_crs[4], g_tpa_crs[4], g_tpb_crs[4];
static ACPI_RESOURCE g_aei[4];
static UINT16 g_tp_pin[1] = { 327 }; /* GPP_E7 */
static UINT16 g_d2_pin[1] = { 256 + 5 }; /* GPP_C5 */
static UINT16 g_b9_pin[1] = { 9 }; /* GPP_B9 */
static UINT16 g_evt_pin[1] = { 256 + 12 }; /* GPP_C12 */
static UINT16 g_l0a_pin[1] = { 10 }; /* GPP_B10 */
static int g_evt_arg = -1;

/* The register address of `what' (is/ie/hostown/lock) for pin. */
static uint8_t *grp_reg(unsigned int pin, unsigned int which, uint32_t *bit)
{
	const struct intel_community *c;
	const struct intel_padgroup *g = group_of(T.pctrl, pin, &c);
	unsigned int off;

	T_ASSERT(g);
	*bit = 1u << (pin - g->base);
	switch (which) {
	case 0:
		off = c->is_offset + g->reg_num * 4;
		break;
	case 1:
		off = c->ie_offset + g->reg_num * 4;
		break;
	case 2:
		off = c->hostown_offset + g->reg_num * 4;
		break;
	default:
		off = c->padcfglock_offset + g->reg_num * 8;
		break;
	}
	return c->regs + off;
}

static uint32_t cfg0(unsigned int pin)
{
	return rm_peek(intel_get_padcfg(T.pctrl, pin, INTEL_PADCFG0));
}

static uint32_t cfg1(unsigned int pin)
{
	return rm_peek(intel_get_padcfg(T.pctrl, pin, INTEL_PADCFG1));
}

/* Pins used below (TGL-LP numbering). */
#define PIN_B9 9 /* quirk-ignored wake, GPIO 9 */
#define PIN_B10 10 /* _AEI level event, GPIO 10 */
#define PIN_E7 (226 + 7) /* touchpad, GPIO 327 */
#define PIN_C5 (171 + 5) /* edge device, GPIO 261 */
#define PIN_C3 (171 + 3) /* plain GPIO, 259 */
#define PIN_C9 (171 + 9) /* unexpected interrupt, 265 */
#define PIN_C12 (171 + 12) /* _AEI ActiveBoth event, 268 */
#define PIN_C13 (171 + 13) /* firmware direct IRQ, 269 */
#define PIN_C14 (171 + 14) /* unused, 270 */
#define PIN_H4 (75 + 4) /* owned by the ME, GPIO 132 */
#define PIN_H5 (75 + 5) /* ACPI mode, GPIO 133 */
#define PIN_H6 (75 + 6) /* locked, GPIO 134 */
#define PIN_C10 (171 + 10) /* OpRegion output, 266 */
#define PIN_C11 (171 + 11) /* OpRegion input, 267 */

static ACPI_STATUS evt_eval(struct mock_node *self, const ACPI_OBJECT_LIST *args,
			    ACPI_OBJECT *out)
{
	(void)self;
	(void)out;
	if (args && args->Count == 1 &&
	    args->Pointer[0].Type == ACPI_TYPE_INTEGER)
		g_evt_arg = (int)args->Pointer[0].Integer.Value;
	return AE_OK;
}

/* ---- the firmware-described controller's _DSD ---- */

static UINT8 g_props_uuid[16], g_data_uuid[16];

static ACPI_OBJECT g_kv[6][2], g_props[6], g_names[2];
static ACPI_OBJECT g_g0_kv[2][2], g_g0_props[2], g_g0[2];
static ACPI_OBJECT g_links_kv[3][2], g_links[3];
static ACPI_OBJECT g_dsd[4];
static ACPI_OBJECT g_g1_kv[2][2], g_g1_props[2], g_g1[2];
static ACPI_OBJECT g_g2_kv[2][2], g_g2_props[2], g_g2[2];

static void kv_int(ACPI_OBJECT *kv, ACPI_OBJECT *pkg, const char *k, UINT64 v)
{
	mock_obj_str(&kv[0], k);
	mock_obj_int(&kv[1], v);
	mock_obj_pkg(pkg, kv, 2);
}

static void kv_str(ACPI_OBJECT *kv, ACPI_OBJECT *pkg, const char *k,
		   const char *v)
{
	mock_obj_str(&kv[0], k);
	mock_obj_str(&kv[1], v);
	mock_obj_pkg(pkg, kv, 2);
}

/* A group node: { props-UUID, { name, count } } */
static void group_node(ACPI_OBJECT (*kv)[2], ACPI_OBJECT *props,
		       ACPI_OBJECT *node, const char *name, UINT64 count)
{
	kv_str(kv[0], &props[0], "intc-gpio-group-name", name);
	kv_int(kv[1], &props[1], "intc-gpio-pad-count", count);
	mock_obj_buf(&node[0], g_props_uuid, 16);
	mock_obj_pkg(&node[1], props, 2);
}

static ACPI_STATUS grp1_eval(struct mock_node *self,
			     const ACPI_OBJECT_LIST *args, ACPI_OBJECT *out)
{
	(void)self;
	(void)args;
	if (out)
		mock_obj_pkg(out, g_g1, 2);
	return AE_OK;
}

static ACPI_STATUS grp2_eval(struct mock_node *self,
			     const ACPI_OBJECT_LIST *args, ACPI_OBJECT *out)
{
	(void)self;
	(void)args;
	if (out)
		mock_obj_pkg(out, g_g2, 2);
	return AE_OK;
}

static ACPI_STATUS dsd_eval(struct mock_node *self, const ACPI_OBJECT_LIST *args,
			    ACPI_OBJECT *out)
{
	(void)args;
	T_EQ(acpi_uuid_parse("daffd814-6eba-4d8c-8a91-bc9bbf4aa301",
			     g_props_uuid),
	     0);
	T_EQ(acpi_uuid_parse("dbb8e3e6-5886-4ba6-8795-1319f52a966b",
			     g_data_uuid),
	     0);
	kv_int(g_kv[0], &g_props[0], "intc-gpio-pad-ownership-offset", 0x20);
	kv_int(g_kv[1], &g_props[1],
	       "intc-gpio-pad-configuration-lock-offset", 0x80);
	kv_int(g_kv[2], &g_props[2],
	       "intc-gpio-host-software-pad-ownership-offset", 0xb0);
	kv_int(g_kv[3], &g_props[3], "intc-gpio-gpi-interrupt-status-offset",
	       0x100);
	kv_int(g_kv[4], &g_props[4], "intc-gpio-gpi-interrupt-enable-offset",
	       0x120);
	/* a string list, as resource names are given */
	mock_obj_str(&g_names[0], "main");
	mock_obj_str(&g_names[1], "rgpio");
	mock_obj_str(&g_kv[5][0], "pinctrl-resource-names");
	mock_obj_pkg(&g_kv[5][1], g_names, 2);
	mock_obj_pkg(&g_props[5], g_kv[5], 2);
	/* group 0 written in place, group 1 by path, group 2 by reference */
	group_node(g_g0_kv, g_g0_props, g_g0, "GPP-A", 24);
	group_node(g_g1_kv, g_g1_props, g_g1, "GPP_B", 9);
	group_node(g_g2_kv, g_g2_props, g_g2, "vGPIO", 32);
	mock_obj_str(&g_links_kv[0][0], "group@0");
	mock_obj_pkg(&g_links_kv[0][1], g_g0, 2);
	mock_obj_pkg(&g_links[0], g_links_kv[0], 2);
	mock_obj_str(&g_links_kv[1][0], "group@1");
	mock_obj_str(&g_links_kv[1][1], "GRP1");
	mock_obj_pkg(&g_links[1], g_links_kv[1], 2);
	mock_obj_str(&g_links_kv[2][0], "group@2");
	mock_obj_ref(&g_links_kv[2][1], self->priv);
	mock_obj_pkg(&g_links[2], g_links_kv[2], 2);
	mock_obj_buf(&g_dsd[0], g_props_uuid, 16);
	mock_obj_pkg(&g_dsd[1], g_props, 6);
	mock_obj_buf(&g_dsd[2], g_data_uuid, 16);
	mock_obj_pkg(&g_dsd[3], g_links, 3);
	if (out)
		mock_obj_pkg(out, g_dsd, 4);
	return AE_OK;
}

static void build_tgl(void)
{
	static const UINT32 gsi[] = { GSI_GPIO };
	const struct intel_pinctrl_soc_data *soc = lookup("INT34C5", NULL);
	struct mock_node *grp2;
	int i = 0;

	rm_reset();
	mock_ns_reset();
	kmemset(&T, 0, sizeof(T));
	for (int b = 0; b < 4; b++) {
		uint8_t *w = rm_window(0x10000);
		const struct intel_community *tc = &soc->communities[b];

		T.win[b] = w;
		rm_poke(w + INTEL_REVID, 0x00940000u);
		rm_poke(w + INTEL_PADBAR, 0x700);
		/* hardware access types of the header */
		rm_set_attr_range(w, 0x10, RM_RO);
		rm_set_attr_range(w + tc->padown_offset,
				  tc->padcfglock_offset - tc->padown_offset,
				  RM_RO);
		rm_set_attr_range(w + tc->padcfglock_offset,
				  tc->hostown_offset - tc->padcfglock_offset,
				  RM_RO);
		rm_set_attr_range(w + tc->is_offset, 0x20, RM_W1C);
		/* the firmware gave every pad's interrupt to the OS ... */
		for (unsigned int r = 0; r < 8; r++)
			rm_poke(w + tc->hostown_offset + r * 4, 0xffffffffu);
		/* ... and left interrupts enabled and pending */
		for (unsigned int r = 0; r < 8; r++) {
			rm_poke(w + tc->ie_offset + r * 4, 0x00f0u);
			rm_poke(w + tc->is_offset + r * 4, 0x0f0fu);
		}
		mock_res_addr64(&g_gpi_crs[i++].r, (UINT64)(unsigned long)w,
				(UINT64)(unsigned long)w + 0xffff, 0x10000, 0,
				0);
	}
	mock_res_extirq(&g_gpi_crs[i++].r, 1, ACPI_ACTIVE_LOW, 1, 0, gsi, 1);
	mock_res_end(&g_gpi_crs[i++].r);
	T.gpi0 = mock_device(NULL, "GPI0", "INT34C5", NULL);
	(void)mock_resources_stride(T.gpi0, "_CRS", g_gpi_crs, i,
				    (int)sizeof(g_gpi_crs[0]));
	/* GPIO-signalled events: ActiveBoth edge on C12 (pin > 255 in
	 * the GPIO numbering: _EVT), level high on B10 (_L0A) */
	mock_res_gpio(&g_aei[0], 1, 0, ACPI_ACTIVE_BOTH, 0, 0,
		      ACPI_PIN_CONFIG_DEFAULT, 0, 0, "\\_SB.GPI0", g_evt_pin,
		      1);
	mock_res_gpio(&g_aei[1], 1, 1, ACPI_ACTIVE_HIGH, 0, 0,
		      ACPI_PIN_CONFIG_DEFAULT, 0, 0, "\\_SB.GPI0", g_l0a_pin,
		      1);
	mock_res_end(&g_aei[2]);
	(void)mock_resources(T.gpi0, "_AEI", g_aei, 3);
	T.evt = mock_method(T.gpi0, "_EVT", evt_eval, NULL);
	T.l0a = mock_method(T.gpi0, "_L0A", evt_eval, NULL);

	T.tpd0 = mock_device(NULL, "TPD0", "ELAN073B", NULL);
	mock_add_cid(T.tpd0, "PNP0C50");
	mock_res_gpio(&g_tp_crs[0], 1, 1, ACPI_ACTIVE_LOW, 0, 1,
		      ACPI_PIN_CONFIG_PULLUP, 0, 0, "\\_SB.GPI0", g_tp_pin, 1);
	mock_res_end(&g_tp_crs[1]);
	(void)mock_resources(T.tpd0, "_CRS", g_tp_crs, 2);
	T.dev2 = mock_device(NULL, "DEV2", "TEST0001", NULL);
	mock_res_gpio(&g_dev2_crs[0], 1, 0, ACPI_ACTIVE_HIGH, 0, 0,
		      ACPI_PIN_CONFIG_DEFAULT, 0, 0, "\\_SB.GPI0", g_d2_pin, 1);
	mock_res_end(&g_dev2_crs[1]);
	(void)mock_resources(T.dev2, "_CRS", g_dev2_crs, 2);
	/* two devices of the quirked _HID: instance 00 is ignored for
	 * wake (the DMI quirk), instance 01 is not */
	T.tpda = mock_device(NULL, "TPDA", "ELAN0415", NULL);
	mock_res_gpio(&g_tpa_crs[0], 1, 1, ACPI_ACTIVE_LOW, 0, 1,
		      ACPI_PIN_CONFIG_DEFAULT, 0, 0, "\\_SB.GPI0", g_b9_pin, 1);
	mock_res_end(&g_tpa_crs[1]);
	(void)mock_resources(T.tpda, "_CRS", g_tpa_crs, 2);
	T.tpdb = mock_device(NULL, "TPDB", "ELAN0415", NULL);
	mock_res_gpio(&g_tpb_crs[0], 1, 1, ACPI_ACTIVE_LOW, 1, 1,
		      ACPI_PIN_CONFIG_DEFAULT, 0, 0, "\\_SB.GPI0", g_b9_pin, 1);
	mock_res_end(&g_tpb_crs[1]);
	(void)mock_resources(T.tpdb, "_CRS", g_tpb_crs, 2);

	/* a firmware-described controller (no interrupt) */
	T.pwin = rm_window(0x10000);
	rm_poke(T.pwin + INTEL_REVID, 0x01100000u);
	rm_poke(T.pwin + INTEL_PADBAR, 0x700);
	mock_res_addr64(&g_gpi1_crs[0].r, (UINT64)(unsigned long)T.pwin,
			(UINT64)(unsigned long)T.pwin + 0xffff, 0x10000, 0, 0);
	mock_res_end(&g_gpi1_crs[1].r);
	T.gpi1 = mock_device(NULL, "GPI1", "INTC105F", NULL);
	(void)mock_resources_stride(T.gpi1, "_CRS", g_gpi1_crs, 2,
				    (int)sizeof(g_gpi1_crs[0]));
	(void)mock_method(T.gpi1, "GRP1", grp1_eval, NULL);
	grp2 = mock_method(NULL, "GRP2", grp2_eval, NULL);
	(void)mock_method(T.gpi1, "_DSD", dsd_eval, grp2);
}

/* After probe (pctrl known): pad states and attributes. */
static void tgl_pads(void)
{
	uint32_t bit;
	uint8_t *r;
	unsigned int off;

	/* GPP_E7 in a native function, as the firmware might leave it */
	r = intel_get_padcfg(T.pctrl, PIN_E7, INTEL_PADCFG0);
	rm_poke(r, (1u << 10) | INTEL_PADCFG0_GPIROUTIOXAPIC |
			   INTEL_PADCFG0_GPIORXSTATE);
	/* GPP_H4 owned by the ME */
	off = padown_off(T.pctrl, PIN_H4);
	r = T.pctrl->communities[1].regs + off;
	rm_poke(r, rm_peek(r) | (1u << (((PIN_H4 - 75) % 8) * 4)));
	/* GPP_H5 in ACPI mode */
	r = grp_reg(PIN_H5, 2, &bit);
	rm_poke(r, rm_peek(r) & ~bit);
	/* GPP_H6 locked (configuration read-only) */
	r = grp_reg(PIN_H6, 3, &bit);
	rm_poke(r, rm_peek(r) | bit);
	rm_poke(intel_get_padcfg(T.pctrl, PIN_H6, INTEL_PADCFG0),
		INTEL_PADCFG0_GPIOTXDIS);
	rm_set_attr(intel_get_padcfg(T.pctrl, PIN_H6, INTEL_PADCFG0), RM_RO);
	rm_set_attr(intel_get_padcfg(T.pctrl, PIN_H6, INTEL_PADCFG1), RM_RO);
	/* plain GPIO pads in GPIO mode, input */
	rm_poke(intel_get_padcfg(T.pctrl, PIN_C3, INTEL_PADCFG0),
		INTEL_PADCFG0_GPIOTXDIS);
	rm_poke(intel_get_padcfg(T.pctrl, PIN_C5, INTEL_PADCFG0),
		INTEL_PADCFG0_GPIOTXDIS);
	rm_poke(intel_get_padcfg(T.pctrl, PIN_C10, INTEL_PADCFG0),
		INTEL_PADCFG0_GPIOTXDIS);
	rm_poke(intel_get_padcfg(T.pctrl, PIN_C11, INTEL_PADCFG0),
		INTEL_PADCFG0_GPIOTXDIS);
}

/* Register a pad with the model (its PADCFG0 and GPI_IS bit). */
static int pad_of(unsigned int pin)
{
	uint32_t bit;
	uint8_t *is = grp_reg(pin, 0, &bit);

	return rm_intel_pad(intel_get_padcfg(T.pctrl, pin, INTEL_PADCFG0), is,
			    bit);
}

static struct tp_ctx {
	volatile int hard, thread;
	int deassert; /* the thread services the device */
	int pad;
} g_tp;

static int tp_thread(void *arg)
{
	struct tp_ctx *c = arg;

	T_ASSERT(!irqs_disabled());
	c->thread++;
	if (c->deassert)
		rm_intel_drive(c->pad, 1); /* line back high: not active */
	return IRQ_HANDLED;
}

static volatile int g_edge_hits;
static int edge_handler(void *arg)
{
	(void)arg;
	T_ASSERT(irqs_disabled());
	g_edge_hits++;
	return IRQ_HANDLED;
}

/* Fire the parent while the controller signals (bounded). */
static int fire_while_pending(void)
{
	int n = 0;

	while (n < 8) {
		int pend = 0;

		for (size_t i = 0; i < T.pctrl->ncommunities; i++) {
			const struct intel_community *c =
				&T.pctrl->communities[i];

			for (size_t j = 0; j < c->ngpps; j++)
				pend |= (rm_peek(c->regs + c->is_offset +
						 c->gpps[j].reg_num * 4) &
					 rm_peek(c->regs + c->ie_offset +
						 c->gpps[j].reg_num * 4)) != 0;
		}
		if (!pend)
			break;
		T_ASSERT(irqm_fire_gsi(GSI_GPIO) & IRQ_HANDLED);
		n++;
	}
	return n;
}

static void t_tgl_probe_and_irq(void)
{
	struct acpi_device *tp, *d2, *ta, *tb;
	uint32_t bit, v;
	int irq, wake = 0, irq2, pad, warn0;

	build_tgl();
	AcpiGbl_FADT.Flags |= ACPI_FADT_LOW_POWER_S0;
	gpiolib_acpi_events_enable(1);
	intel_pinctrl_init();
	gpiolib_acpi_events_enable(0);
	T.gc = gpiochip_find_by_acpi_handle(T.gpi0);
	T_ASSERT(T.gc);
	T.pctrl = container_of(T.gc, struct intel_pinctrl, chip);
	T_EQ(T.pctrl->ncommunities, 4);
	T_EQ(T.gc->ngpio, 352 + 8);
	T_ASSERT(T.pctrl->irq > 0);
	T_EQ(T.pctrl->gsi, GSI_GPIO);
	T_EQ(kstrcmp(T.pctrl->label, "INT34C5:0"), 0);
	/* probe masked and cleared everything the firmware left, then the
	 * two _AEI events were set up (their lines only) */
	for (size_t i = 0; i < 4; i++) {
		const struct intel_community *c = &T.pctrl->communities[i];

		for (size_t j = 0; j < c->ngpps; j++) {
			uint32_t ie = rm_peek(c->regs + c->ie_offset +
					      c->gpps[j].reg_num * 4);

			if (i == 0 && j == 0)
				T_EQ(ie, 1u << PIN_B10);
			else if (i == 2 && j == 0)
				T_EQ(ie, 1u << (PIN_C12 - 171));
			else
				T_EQ(ie, 0);
			T_EQ(rm_peek(c->regs + c->is_offset +
				     c->gpps[j].reg_num * 4),
			     0);
		}
	}
	/* the ActiveBoth event's line was low at start: run once */
	T_EQ(g_evt_arg, 256 + 12);
	T_EQ(T.evt->calls, 1);
	T_EQ(T.l0a->calls, 0);
	T_ASSERT(gpiochip_line_is_requested(T.gc, 268));
	T_EQ((cfg0(PIN_C12) & INTEL_PADCFG0_RXEVCFG_MASK) >> 25,
	     INTEL_PADCFG0_RXEVCFG_EDGE_BOTH);
	T_EQ((cfg0(PIN_B10) & INTEL_PADCFG0_RXEVCFG_MASK) >> 25,
	     INTEL_PADCFG0_RXEVCFG_LEVEL);
	/* nothing pending: the shared line is not ours */
	T_EQ(irqm_fire_gsi(GSI_GPIO), IRQ_NONE);
	/* the level event fires: _L0A runs in the thread */
	pad = pad_of(PIN_B10);
	rm_intel_drive(pad, 1);
	T_EQ(fire_while_pending(), 1);
	rm_intel_drive(pad, 0);
	T_EQ(irqm_run_threads(), 1);
	T_EQ(T.l0a->calls, 1);
	tgl_pads();

	/* ---- the touchpad's GpioInt ---- */
	tp = acpi_dev_from_handle(T.tpd0);
	irq = acpi_dev_gpio_irq_wake_get(tp, 0, &wake);
	T_ASSERT(irq > 0);
	T_EQ(wake, 1);
	T_EQ(irq_get_trigger_type(irq), IRQ_TYPE_LEVEL_LOW);
	T_EQ(acpi_dev_gpio_irq_get(tp, 1), -ENOENT);
	v = cfg0(PIN_E7);
	T_EQ((v & INTEL_PADCFG0_PMODE_MASK), 0); /* now GPIO */
	T_EQ((v & INTEL_PADCFG0_RXEVCFG_MASK) >> 25, INTEL_PADCFG0_RXEVCFG_LEVEL);
	T_ASSERT(v & INTEL_PADCFG0_RXINV);
	T_ASSERT(v & INTEL_PADCFG0_GPIOTXDIS);
	T_ASSERT(!(v & INTEL_PADCFG0_GPIORXDIS));
	T_ASSERT(!(v & (INTEL_PADCFG0_GPIROUTIOXAPIC | INTEL_PADCFG0_GPIROUTSCI |
			INTEL_PADCFG0_GPIROUTSMI | INTEL_PADCFG0_GPIROUTNMI)));
	/* pull-up from the descriptor: 5 kOhm up */
	T_EQ(cfg1(PIN_E7) & (INTEL_PADCFG1_TERM_MASK | INTEL_PADCFG1_TERM_UP),
	     INTEL_PADCFG1_TERM_UP | (INTEL_PADCFG1_TERM_5K << 10));
	/* debounce 0 in the descriptor: debouncer and glitch filter off */
	T_EQ(cfg0(PIN_E7) & INTEL_PADCFG0_PREGFRXSEL, 0);
	/* without low-power S0 the wake flag is not passed on */
	AcpiGbl_FADT.Flags &= ~ACPI_FADT_LOW_POWER_S0;
	wake = 5;
	T_EQ(acpi_dev_gpio_irq_wake_get(tp, 0, &wake), irq);
	T_EQ(wake, 0);
	AcpiGbl_FADT.Flags |= ACPI_FADT_LOW_POWER_S0;
	/* a thread-only handler must be oneshot on a GPIO line */
	T_EQ(request_threaded_irq(irq, NULL, tp_thread, 0, "tp", &g_tp),
	     -EINVAL);
	kmemset(&g_tp, 0, sizeof(g_tp));
	pad = pad_of(PIN_E7);
	g_tp.pad = pad;
	rm_intel_drive(pad, 1); /* idle: high */
	T_EQ(request_threaded_irq(irq, NULL, tp_thread, IRQF_ONESHOT, "tp",
				  &g_tp),
	     0);
	T_ASSERT(rm_peek(grp_reg(PIN_E7, 1, &bit)) & bit); /* IE on */
	T_ASSERT(gpiochip_line_is_irq(T.gc, 327));

	/* level asserted: masked until the thread ran */
	g_tp.deassert = 1;
	rm_intel_drive(pad, 0);
	T_EQ(fire_while_pending(), 1);
	T_EQ(g_tp.thread, 0);
	T_ASSERT(!(rm_peek(grp_reg(PIN_E7, 1, &bit)) & bit)); /* IE off */
	T_ASSERT(rm_peek(grp_reg(PIN_E7, 0, &bit)) & bit); /* still latched */
	T_EQ(fire_while_pending(), 0); /* masked: no storm */
	T_EQ(irqm_run_threads(), 1);
	T_EQ(g_tp.thread, 1);
	T_ASSERT(rm_peek(grp_reg(PIN_E7, 1, &bit)) & bit); /* IE on again */
	T_EQ(rm_peek(grp_reg(PIN_E7, 0, &bit)) & bit, 0);
	T_EQ(fire_while_pending(), 0);

	/* a thread that leaves the level asserted runs again */
	g_tp.deassert = 0;
	rm_intel_drive(pad, 0);
	T_EQ(fire_while_pending(), 1);
	T_EQ(irqm_run_threads(), 1);
	T_EQ(fire_while_pending(), 1); /* re-latched after the unmask */
	g_tp.deassert = 1;
	T_EQ(irqm_run_threads(), 1);
	T_EQ(g_tp.thread, 3);
	T_EQ(fire_while_pending(), 0);

	/* disabled: no delivery; enabled: the level comes through */
	disable_irq(irq);
	rm_intel_drive(pad, 0);
	T_EQ(fire_while_pending(), 0);
	enable_irq(irq);
	T_EQ(fire_while_pending(), 1);
	T_EQ(irqm_run_threads(), 1);
	T_EQ(g_tp.thread, 4);

	/* wake goes to the controller's own interrupt */
	T_EQ(enable_irq_wake(irq), 0);
	T_ASSERT(irq_is_wake_enabled(T.pctrl->irq));
	T_EQ(disable_irq_wake(irq), 0);
	T_ASSERT(!irq_is_wake_enabled(T.pctrl->irq));

	/* ---- the wake quirk: ELAN0415:00 pin 9 ---- */
	{
		char name[24];

		ta = acpi_dev_from_handle(T.tpda);
		tb = acpi_dev_from_handle(T.tpdb);
		T_EQ(acpi_gpio_dev_name(ta, name, sizeof(name)), 0);
		T_EQ(kstrcmp(name, "ELAN0415:00"), 0);
		T_EQ(acpi_gpio_dev_name(tb, name, sizeof(name)), 0);
		T_EQ(kstrcmp(name, "ELAN0415:01"), 0);
		wake = 5;
		T_ASSERT(acpi_dev_gpio_irq_wake_get(ta, 0, &wake) > 0);
		T_EQ(wake, 0);
		wake = 5;
		T_ASSERT(acpi_dev_gpio_irq_wake_get(tb, 0, &wake) > 0);
		T_EQ(wake, 1);
		acpi_dev_put(ta);
		acpi_dev_put(tb);
	}

	/* ---- an edge device ---- */
	d2 = acpi_dev_from_handle(T.dev2);
	irq2 = acpi_dev_gpio_irq_get(d2, 0);
	T_ASSERT(irq2 > 0 && irq2 != irq);
	v = cfg0(PIN_C5);
	T_EQ((v & INTEL_PADCFG0_RXEVCFG_MASK) >> 25, INTEL_PADCFG0_RXEVCFG_EDGE);
	T_ASSERT(!(v & INTEL_PADCFG0_RXINV));
	T_EQ(irq_set_irq_type(irq2, IRQ_TYPE_EDGE_FALLING), 0);
	T_ASSERT(cfg0(PIN_C5) & INTEL_PADCFG0_RXINV);
	T_EQ(irq_set_irq_type(irq2, IRQ_TYPE_EDGE_BOTH), 0);
	T_EQ((cfg0(PIN_C5) & INTEL_PADCFG0_RXEVCFG_MASK) >> 25,
	     INTEL_PADCFG0_RXEVCFG_EDGE_BOTH);
	T_EQ(irq_set_irq_type(irq2, IRQ_TYPE_EDGE_RISING), 0);
	T_EQ(request_irq(irq2, edge_handler, 0, "edge", (void *)&g_edge_hits), 0);
	pad = pad_of(PIN_C5);
	g_edge_hits = 0;
	rm_intel_drive(pad, 1);
	T_EQ(fire_while_pending(), 1);
	T_EQ(g_edge_hits, 1);
	rm_intel_drive(pad, 0); /* falling: not an event for rising */
	T_EQ(fire_while_pending(), 0);
	/* an edge the demultiplexer saw just as the interrupt got disabled
	 * is replayed when it is enabled again */
	disable_irq_nosync(irq2);
	{
		uint64_t fl = local_irq_save();

		gpiochip_handle_irq(T.gc, 261);
		local_irq_restore(fl);
	}
	T_EQ(g_edge_hits, 1);
	enable_irq(irq2);
	flush_workqueue(system_highpri_wq);
	T_EQ(g_edge_hits, 2);
	(void)free_irq(irq2, (void *)&g_edge_hits);
	T_ASSERT(!gpiochip_line_is_irq(T.gc, 261));

	/* ---- an interrupt nobody asked for ---- */
	{
		uint8_t *ie = grp_reg(PIN_C9, 1, &bit), *is;

		rm_poke(ie, rm_peek(ie) | bit);
		is = grp_reg(PIN_C9, 0, &bit);
		rm_poke(is, rm_peek(is) | bit);
		warn0 = g_test_warnings;
		T_EQ(fire_while_pending(), 1);
		T_EQ(g_test_warnings - warn0, 1);
		g_test_warnings = warn0;
		T_EQ(rm_peek(ie) & bit, 0);
	}
	/* ---- one enabled by firmware on a pad without GPIO number ---- */
	{
		/* HVCMOS (pins 220-225) is group 2 of community 2 */
		const struct intel_community *c = &T.pctrl->communities[2];
		uint8_t *ie = c->regs + c->ie_offset + 2 * 4;
		uint8_t *is = c->regs + c->is_offset + 2 * 4;

		rm_poke(ie, 1u << 3);
		rm_poke(is, 1u << 3);
		warn0 = g_test_warnings;
		T_EQ(fire_while_pending(), 1);
		T_EQ(g_test_warnings - warn0, 1);
		g_test_warnings = warn0;
		T_EQ(rm_peek(ie), 0);
	}
	acpi_dev_put(tp);
	acpi_dev_put(d2);
}

static void t_tgl_pads(void)
{
	struct gpio_desc *d;
	char buf[128];
	int pad;

	/* owned by the ME: no GPIO, no interrupt */
	d = gpiochip_get_desc(T.gc, 132);
	T_EQ(gpiod_request(d, "x"), -EBUSY);
	T_EQ(gpiochip_irq_map(T.gc, 132), -EBUSY);
	T.gc->describe(T.gc, 132, buf, sizeof(buf));
	T_ASSERT(kstrncmp(buf, "GPP_H_4", 7) == 0 || buf[0]);
	/* ACPI mode: a GPIO, but no interrupt */
	d = gpiochip_get_desc(T.gc, 133);
	T_EQ(gpiod_request(d, "acpi-mode"), 0);
	T_EQ(gpiochip_irq_map(T.gc, 133), -EPERM);
	gpiod_free(d);
	/* locked: request works without touching the pad; direction
	 * writes go to the hardware, which ignores them; no pulls or
	 * debounce */
	d = gpiochip_get_desc(T.gc, 134);
	rm_log_clear();
	T_EQ(gpiod_request(d, "locked"), 0);
	T_EQ(rm_writes_in(intel_get_padcfg(T.pctrl, PIN_H6, 0), 16), 0);
	T_EQ(gpiod_direction_input(d), 0);
	T_EQ(gpiod_direction_output(d, 1), 0);
	T_EQ(cfg0(PIN_H6), INTEL_PADCFG0_GPIOTXDIS); /* still an input */
	T_EQ(gpiod_set_debounce(d, 1000), -ENOTSUPP);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1), -ENOTSUPP);
	T.gc->describe(T.gc, 134, buf, sizeof(buf));
	T_ASSERT(buf[0]);
	gpiod_free(d);
	/* holes in the numbering (HVCMOS, JTAG, SPI have none) */
	T_ASSERT(!T.gc->valid(T.gc, 352 + 8));
	T_ASSERT(T.gc->valid(T.gc, 352 + 7));
	/* GPP_S has 8 pads at 96..103: 104..127 are no GPIOs */
	T_EQ(gpiod_request(gpiochip_get_desc(T.gc, 110), "hole"), -EINVAL);
	T_EQ(gpiochip_irq_map(T.gc, 110), -EINVAL);

	/* direction and values on GPP_C3 */
	d = gpiochip_get_desc(T.gc, 259);
	T_EQ(gpiod_request(d, "c3"), 0);
	T_EQ(gpiod_request(d, "again"), -EBUSY);
	T_EQ(gpiod_get_direction(d), GPIO_LINE_DIRECTION_IN);
	T_EQ(gpiod_direction_output(d, 1), 0);
	T_EQ(cfg0(PIN_C3) & (INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_GPIORXDIS |
			     INTEL_PADCFG0_GPIOTXSTATE),
	     INTEL_PADCFG0_GPIORXDIS | INTEL_PADCFG0_GPIOTXSTATE);
	T_EQ(gpiod_get_value(d), 1);
	gpiod_set_value(d, 0);
	T_EQ(gpiod_get_value(d), 0);
	T_EQ(gpiod_direction_input(d), 0);
	T_EQ(cfg0(PIN_C3) & (INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_GPIORXDIS),
	     INTEL_PADCFG0_GPIOTXDIS);
	pad = pad_of(PIN_C3);
	rm_intel_drive(pad, 1);
	T_EQ(gpiod_get_value(d), 1);
	gpiod_toggle_active_low(d);
	T_EQ(gpiod_get_value(d), 0);
	T_EQ(gpiod_get_raw_value(d), 1);
	gpiod_toggle_active_low(d);
	/* pulls */
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 20000), 0);
	T_EQ(cfg1(PIN_C3) & (INTEL_PADCFG1_TERM_MASK | INTEL_PADCFG1_TERM_UP),
	     INTEL_PADCFG1_TERM_20K << 10);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 1000), 0);
	T_EQ(cfg1(PIN_C3) & INTEL_PADCFG1_TERM_MASK, INTEL_PADCFG1_TERM_1K << 10);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 833), 0);
	T_EQ(cfg1(PIN_C3) & (INTEL_PADCFG1_TERM_MASK | INTEL_PADCFG1_TERM_UP),
	     INTEL_PADCFG1_TERM_UP | (INTEL_PADCFG1_TERM_833 << 10));
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1234), -EINVAL);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_DISABLE, 0), 0);
	T_EQ(cfg1(PIN_C3) & (INTEL_PADCFG1_TERM_MASK | INTEL_PADCFG1_TERM_UP), 0);
	/* debounce: 2^n * 31.25 us, n = 3..15, with the glitch filter */
	T_EQ(gpiod_set_debounce(d, 1000), 0);
	T_EQ(rm_peek(intel_get_padcfg(T.pctrl, PIN_C3, INTEL_PADCFG2)),
	     (5u << 1) | 1u);
	T_ASSERT(cfg0(PIN_C3) & INTEL_PADCFG0_PREGFRXSEL);
	T_EQ(gpiod_set_debounce(d, 1100), 0); /* rounded up */
	T_EQ(rm_peek(intel_get_padcfg(T.pctrl, PIN_C3, INTEL_PADCFG2)),
	     (6u << 1) | 1u);
	T_EQ(gpiod_set_debounce(d, 50), -EINVAL);
	T_EQ(gpiod_set_debounce(d, 0), 0);
	T_EQ(rm_peek(intel_get_padcfg(T.pctrl, PIN_C3, INTEL_PADCFG2)), 0);
	T_EQ(cfg0(PIN_C3) & INTEL_PADCFG0_PREGFRXSEL, 0);
	/* high impedance: both buffers off */
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_HIGH_IMPEDANCE, 0), 0);
	T_EQ(cfg0(PIN_C3) & (INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_GPIORXDIS),
	     INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_GPIORXDIS);
	T_EQ(gpiod_direction_input(d), 0);
	gpiod_free(d);
}

/* System sleep: what is restored, and what is not. */
static void t_tgl_sleep(void)
{
	struct gpio_desc *c3 = gpiochip_get_desc(T.gc, 259);
	uint8_t *e7 = intel_get_padcfg(T.pctrl, PIN_E7, INTEL_PADCFG0);
	uint8_t *c13 = intel_get_padcfg(T.pctrl, PIN_C13, INTEL_PADCFG0);
	uint8_t *c14 = intel_get_padcfg(T.pctrl, PIN_C14, INTEL_PADCFG0);
	uint8_t *ie_e7, *ho_c3, *ho_c14;
	uint32_t e7v, c13v, bit_e7, bit_c3, bit_c14, iev;

	/* the touchpad (E7) is still an interrupt; C3 is requested */
	T_EQ(gpiod_request(c3, "c3-sleep"), 0);
	/* C13: a direct interrupt the firmware set up */
	rm_poke(c13, INTEL_PADCFG0_GPIROUTIOXAPIC | INTEL_PADCFG0_GPIOTXDIS |
			     INTEL_PADCFG0_RXINV);
	c13v = rm_peek(c13);
	rm_poke(c14, INTEL_PADCFG0_GPIOTXDIS);
	e7v = rm_peek(e7) & ~INTEL_PADCFG0_GPIORXSTATE;
	ie_e7 = grp_reg(PIN_E7, 1, &bit_e7);
	iev = rm_peek(ie_e7);
	ho_c3 = grp_reg(PIN_C3, 2, &bit_c3);
	ho_c14 = grp_reg(PIN_C14, 2, &bit_c14);
	intel_pinctrl_suspend(T.pctrl);
	/* the firmware on resume: */
	rm_poke(e7, 0);
	rm_poke(c13, c13v & ~INTEL_PADCFG0_RXINV);
	rm_poke(c14, INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_RXINV);
	rm_poke(ie_e7, 0);
	rm_poke(ho_c3, rm_peek(ho_c3) & ~bit_c3);
	rm_poke(ho_c14, rm_peek(ho_c14) & ~bit_c14);
	intel_pinctrl_resume(T.pctrl);
	T_EQ(rm_peek(e7) & ~INTEL_PADCFG0_GPIORXSTATE, e7v);
	T_EQ(rm_peek(c13), c13v);
	T_EQ(rm_peek(c14), INTEL_PADCFG0_GPIOTXDIS | INTEL_PADCFG0_RXINV);
	T_EQ(rm_peek(ie_e7), iev);
	T_ASSERT(rm_peek(ho_c3) & bit_c3); /* a requested line */
	T_EQ(rm_peek(ho_c14) & bit_c14, 0); /* the firmware's */
	rm_poke(ho_c14, rm_peek(ho_c14) | bit_c14);

	/* masked between suspend and resume: the restored enable must not
	 * bring it back behind gpiolib's back (its cached mask), and the
	 * unmask after still works */
	{
		int irq = gpiochip_irq_lookup(T.gc, 327);

		T_ASSERT(irq > 0);
		T_ASSERT(rm_peek(ie_e7) & bit_e7);
		intel_pinctrl_suspend(T.pctrl);
		disable_irq(irq);
		T_EQ(rm_peek(ie_e7) & bit_e7, 0);
		intel_pinctrl_resume(T.pctrl);
		T_EQ(rm_peek(ie_e7) & bit_e7, 0);
		enable_irq(irq);
		T_ASSERT(rm_peek(ie_e7) & bit_e7);
		/* the other way round: unmasked after the save */
		disable_irq(irq);
		intel_pinctrl_suspend(T.pctrl);
		enable_irq(irq);
		T_ASSERT(rm_peek(ie_e7) & bit_e7);
		intel_pinctrl_resume(T.pctrl);
		T_ASSERT(rm_peek(ie_e7) & bit_e7);
	}
	gpiod_free(c3);
	(void)free_irq(gpiochip_irq_lookup(T.gc, 327), &g_tp);
}

/* GeneralPurposeIo operation region: AML drives and reads pins. */
static void t_tgl_opregion(void)
{
	/* GpioIo(Exclusive, PullDefault, 0, 0, IoRestrictionOutputOnly,
	 *        "\_SB.GPI0") {266, 267} as raw AML */
	static UINT8 aml[64];
	const char *src = "\\_SB.GPI0";
	unsigned int n = 0, pins_off = 23, name_off = 27;
	uint64_t val = 2;
	int pad, warn0;

	kmemset(aml, 0, sizeof(aml));
	aml[n++] = 0x8c;
	n += 2; /* length, unused by the decoder */
	aml[3] = 1; /* revision */
	aml[4] = 1; /* GpioIo */
	aml[5] = 1; /* consumer */
	aml[7] = 2; /* output only */
	aml[9] = 0; /* pull default */
	aml[14] = (UINT8)pins_off;
	aml[17] = (UINT8)name_off;
	aml[19] = (UINT8)(name_off + 10);
	aml[pins_off] = 266 & 0xff;
	aml[pins_off + 1] = 266 >> 8;
	aml[pins_off + 2] = 267 & 0xff;
	aml[pins_off + 3] = 267 >> 8;
	kmemcpy(aml + name_off, src, kstrlen(src) + 1);
	T_EQ(acpi_gpio_opregion_access(1, 0, 2, &val, aml, 40), 0);
	T_EQ(cfg0(PIN_C10) & INTEL_PADCFG0_GPIOTXSTATE, 0);
	T_ASSERT(cfg0(PIN_C11) & INTEL_PADCFG0_GPIOTXSTATE);
	T_ASSERT(gpiochip_line_is_requested(T.gc, 266));
	/* reads: an output reads back its TX state */
	val = 0xff;
	T_EQ(acpi_gpio_opregion_access(0, 1, 1, &val, aml, 40), 0);
	T_EQ(val, 0); /* 267 is still an input (output only, no pull) */
	pad = pad_of(PIN_C11);
	rm_intel_drive(pad, 1);
	T_EQ(acpi_gpio_opregion_access(0, 1, 1, &val, aml, 40), 0);
	T_EQ(val, 1);
	/* an index past the connection's pins */
	T_EQ(acpi_gpio_opregion_access(0, 2, 1, &val, aml, 40), -EINVAL);
	/* an input-only pin refuses writes */
	aml[7] = 1;
	warn0 = g_test_warnings;
	T_EQ(acpi_gpio_opregion_access(1, 0, 1, &val, aml, 40), -EINVAL);
	g_test_warnings = warn0; /* expected */
	aml[7] = 2;
	/* an unknown controller: the caller falls back to its stub */
	kmemcpy(aml + name_off, "\\_SB.GPI9", 10);
	T_EQ(acpi_gpio_opregion_access(1, 0, 1, &val, aml, 40), -ENODEV);
}

/* ==========================================================================
 * The firmware-described controller (INTC105F)
 * ========================================================================== */

static void t_platform(void)
{
	struct intel_pinctrl_soc_data *soc;
	struct gpio_chip *gc;
	struct intel_pinctrl *p;
	const struct intel_community *c;

	/* the soc data alone */
	T_EQ(intel_platform_prepare_soc_data(T.gpi1, &soc), 0);
	T_EQ(soc->ncommunities, 1);
	T_EQ(soc->npins, 24 + 9 + 32);
	c = &soc->communities[0];
	T_EQ(c->padown_offset, 0x20);
	T_EQ(c->padcfglock_offset, 0x80);
	T_EQ(c->hostown_offset, 0xb0);
	T_EQ(c->is_offset, 0x100);
	T_EQ(c->ie_offset, 0x120);
	T_EQ(c->ngpps, 3);
	T_EQ(c->gpps[1].base, 24);
	T_EQ(c->gpps[1].size, 9);
	T_EQ(c->gpps[2].reg_num, 2);
	T_EQ(c->gpps[2].gpio_base, INTEL_GPIO_BASE_MATCH);
	T_EQ(kstrcmp(soc->pins[0].name, "GPP_A_0"), 0);
	T_EQ(kstrcmp(soc->pins[32].name, "GPP_B_8"), 0);
	T_EQ(kstrcmp(soc->pins[64].name, "vGPIO_31"), 0);
	intel_platform_free_soc_data(soc);
	/* string-list properties */
	T_EQ(acpi_dev_property_match_string(T.gpi1, "pinctrl-resource-names",
					    "rgpio"),
	     1);
	T_EQ(acpi_dev_property_match_string(T.gpi1, "pinctrl-resource-names",
					    "main"),
	     0);
	T_EQ(acpi_dev_property_match_string(T.gpi1, "pinctrl-resource-names",
					    "iomux"),
	     -ENODATA);
	T_EQ(acpi_dev_property_match_string(T.gpi1,
					    "intc-gpio-pad-count", "x"),
	     -ENOENT);
	T_EQ(acpi_dev_property_match_string(
		     T.gpi1, "intc-gpio-pad-ownership-offset", "x"),
	     -EPROTO);
	/* no _DSD: nothing */
	T_EQ(intel_platform_prepare_soc_data(T.tpd0, &soc), -ENOENT);
	T_ASSERT(soc == NULL);

	/* probed */
	intel_platform_pinctrl_init();
	gc = gpiochip_find_by_acpi_handle(T.gpi1);
	T_ASSERT(gc);
	p = container_of(gc, struct intel_pinctrl, chip);
	T_EQ(gc->ngpio, 65);
	T_ASSERT(gc->irq == NULL);
	T_ASSERT(p->communities[0].features & PINCTRL_FEATURE_3BIT_PAD_OWN);
	T_EQ(intel_gpio_to_pin(p, 40, NULL, NULL), 40);
	/* pin 40 owned by someone else: one register per pad */
	rm_poke(T.pwin + 0x20 + 40 * 4, 2);
	T_EQ(gpiod_request(gpiochip_get_desc(gc, 40), "x"), -EBUSY);
	T_EQ(gpiod_request(gpiochip_get_desc(gc, 41), "y"), 0);
	gpiod_free(gpiochip_get_desc(gc, 41));
}

/* ==========================================================================
 * Lynx Point
 * ========================================================================== */

#define LP_OWNED 0x00
#define LP_IOXAPIC 0x10
#define LP_INT_STAT 0x80
#define LP_INT_ENABLE 0x90
#define LP_CONF1(n) (0x100 + (n) * 8)
#define LP_CONF2(n) (0x104 + (n) * 8)

static volatile int g_lp_hits;
static int lp_handler(void *arg)
{
	(void)arg;
	g_lp_hits++;
	return IRQ_HANDLED;
}

static void t_lynxpoint(void)
{
	uint8_t *w;
	struct gpio_chip *gc;
	struct gpio_desc *d;
	int irq;
	uint64_t fl;

	rm_reset();
	w = rm_window(0x400);
	rm_set_attr_range(w + LP_INT_STAT, 12, RM_W1C);
	/* firmware: GPIO 0-63 for the OS, 64-94 for ACPI; pin 5 native
	 * with an output level set; interrupts left on */
	rm_poke(w + LP_OWNED, 0xffffffffu);
	rm_poke(w + LP_OWNED + 4, 0xffffffffu);
	rm_poke(w + LP_CONF1(5), (1u << 31) | (1u << 3) | 0u);
	rm_poke(w + LP_CONF2(5), 1u << 2);
	rm_poke(w + LP_INT_ENABLE, 0xffu);
	rm_poke(w + LP_INT_STAT, 0x0fu);
	T_ASSERT(lp_gpio_create((unsigned long)w, 0x100, "short") == NULL);
	gc = lp_gpio_create((unsigned long)w, 0x400, "INT33C7:0");
	T_ASSERT(gc);
	T_EQ(gc->ngpio, 95);
	T_EQ(rm_peek(w + LP_INT_ENABLE), 0);
	T_EQ(rm_peek(w + LP_INT_STAT), 0);
	T_EQ(gpiochip_add(gc), 0);
	T_ASSERT(gc->valid(gc, 94));
	T_ASSERT(!gc->valid(gc, 95));

	/* request: GPIO mode (the mode bits only), input sensing on */
	d = gpiochip_get_desc(gc, 5);
	T_EQ(gpiod_request(d, "lp5"), 0);
	T_EQ(rm_peek(w + LP_CONF1(5)), (1u << 31) | (1u << 3) | 1u);
	T_EQ(rm_peek(w + LP_CONF2(5)) & (1u << 2), 0);
	T_EQ(gpiod_direction_input(d), 0);
	T_EQ(gpiod_get_direction(d), GPIO_LINE_DIRECTION_IN);
	rm_poke(w + LP_CONF1(5), rm_peek(w + LP_CONF1(5)) | (1u << 30));
	T_EQ(gpiod_get_raw_value(d), 1);
	T_EQ(gpiod_direction_output(d, 0), 0);
	T_EQ(rm_peek(w + LP_CONF1(5)) & ((1u << 31) | (1u << 2)), 0);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_UP, 1), 0);
	T_EQ(rm_peek(w + LP_CONF2(5)) & 3u, 2u);
	T_EQ(gpiod_set_config(d, PIN_CONFIG_BIAS_PULL_DOWN, 1), 0);
	T_EQ(rm_peek(w + LP_CONF2(5)) & 3u, 1u);
	T_EQ(gpiod_set_debounce(d, 1000), -ENOTSUPP);
	gpiod_free(d);
	T_ASSERT(rm_peek(w + LP_CONF2(5)) & (1u << 2)); /* sensing off */

	/* an output on an IOxAPIC-routed GPIO warns */
	rm_poke(w + LP_IOXAPIC, 1u << 1); /* GPIO 9 */
	d = gpiochip_get_desc(gc, 9);
	T_EQ(gpiod_request(d, "lp9"), 0);
	{
		int warn0 = g_test_warnings;

		T_EQ(gpiod_direction_output(d, 1), 0);
		T_EQ(g_test_warnings - warn0, 1);
		g_test_warnings = warn0;
	}
	gpiod_free(d);

	/* interrupts: ACPI's pins refuse */
	T_EQ(gpiochip_irq_map(gc, 70), -EBUSY);
	irq = gpiochip_irq_map(gc, 33);
	T_ASSERT(irq > 0);
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW), 0);
	T_EQ(rm_peek(w + LP_CONF1(33)) & ((1u << 4) | (1u << 3)), 1u << 4);
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_LEVEL_HIGH), 0);
	T_EQ(rm_peek(w + LP_CONF1(33)) & ((1u << 4) | (1u << 3)),
	     (1u << 4) | (1u << 3));
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_EDGE_FALLING), 0);
	T_EQ(rm_peek(w + LP_CONF1(33)) & ((1u << 4) | (1u << 3)), 1u << 3);
	T_EQ(irq_set_irq_type(irq, IRQ_TYPE_EDGE_RISING), 0);
	T_EQ(rm_peek(w + LP_CONF1(33)) & ((1u << 4) | (1u << 3)), 0);
	T_EQ(request_irq(irq, lp_handler, 0, "lp", (void *)&g_lp_hits), 0);
	T_EQ(rm_peek(w + LP_INT_ENABLE + 4), 1u << 1);
	g_lp_hits = 0;
	fl = local_irq_save();
	T_EQ(lp_gpio_handle_parent(gc), IRQ_NONE);
	rm_poke(w + LP_INT_STAT + 4, 1u << 1);
	T_EQ(lp_gpio_handle_parent(gc), IRQ_HANDLED);
	local_irq_restore(fl);
	T_EQ(g_lp_hits, 1);
	T_EQ(rm_peek(w + LP_INT_STAT + 4), 0);
	/* masked: the enable bit off */
	disable_irq(irq);
	T_EQ(rm_peek(w + LP_INT_ENABLE + 4), 0);
	enable_irq(irq);
	T_EQ(rm_peek(w + LP_INT_ENABLE + 4), 1u << 1);
	/* resume turns input sensing back on for requested lines */
	d = gpiochip_get_desc(gc, 12);
	T_EQ(gpiod_request(d, "lp12"), 0);
	rm_poke(w + LP_CONF2(12), 1u << 2);
	lp_gpio_resume(gc);
	T_EQ(rm_peek(w + LP_CONF2(12)) & (1u << 2), 0);
	gpiod_free(d);
	(void)free_irq(irq, (void *)&g_lp_hits);
	gpiochip_remove(gc);
	lp_gpio_destroy(gc);
}

/* ==========================================================================
 * DMI and the ignore lists
 * ========================================================================== */

/* A small SMBIOS table: BIOS, system, baseboard, end. */
static uint8_t g_smbios[256];
static unsigned int g_smbios_len;

static unsigned int put_struct(unsigned int at, uint8_t type, uint8_t flen,
			       const uint8_t *fixed, const char *const *strs,
			       int nstr)
{
	g_smbios[at] = type;
	g_smbios[at + 1] = flen;
	kmemcpy(g_smbios + at + 2, fixed, flen - 2);
	at += flen;
	for (int i = 0; i < nstr; i++) {
		size_t l = kstrlen(strs[i]);

		kmemcpy(g_smbios + at, strs[i], l + 1);
		at += (unsigned int)l + 1;
	}
	if (!nstr)
		g_smbios[at++] = 0;
	g_smbios[at++] = 0;
	return at;
}

static void load_dmi(void)
{
	static const char *const bios[] = { "Insyde", "1.7.8 ", "05/10/2022" };
	static const char *const sys[] = { "TUXEDO", "InfinityBook Pro 14",
					   "Not Applicable" };
	static const char *const board[] = { "Notebook", "NL5xNU" };
	uint8_t f0[0x16], f1[0x1b], f2[0x0d];
	unsigned int at = 0;

	kmemset(f0, 0, sizeof(f0));
	kmemset(f1, 0, sizeof(f1));
	kmemset(f2, 0, sizeof(f2));
	/* type 0: vendor 1, version 2, date 3 (offsets 4, 5, 8) */
	f0[4 - 2] = 1;
	f0[5 - 2] = 2;
	f0[8 - 2] = 3;
	at = put_struct(at, 0, 0x18, f0, bios, 3);
	/* type 1: vendor 1, product 2, version 3; family at 0x1a none */
	f1[4 - 2] = 1;
	f1[5 - 2] = 2;
	f1[6 - 2] = 3;
	at = put_struct(at, 1, 0x1b, f1, sys, 3);
	f2[4 - 2] = 1;
	f2[5 - 2] = 2;
	at = put_struct(at, 2, 0x0f, f2, board, 2);
	at = put_struct(at, 127, 4, f2, NULL, 0);
	g_smbios_len = at;
	dmi_load_table(g_smbios, g_smbios_len, 0);
}

static const struct dmi_system_id g_dmi_list[] = {
	{ .ident = "exact miss",
	  .matches = { DMI_EXACT_MATCH(DMI_SYS_VENDOR, "TUX") } },
	{ .ident = "two fields",
	  .matches = { DMI_MATCH(DMI_SYS_VENDOR, "TUX"),
		       DMI_MATCH(DMI_BOARD_NAME, "NL5x") } },
	{ 0 }
};

static void t_dmi(void)
{
	T_EQ(kstrcmp(dmi_get_system_info(DMI_BIOS_VENDOR), "Insyde"), 0);
	/* strings as the firmware wrote them, trailing blank included */
	T_EQ(kstrcmp(dmi_get_system_info(DMI_BIOS_VERSION), "1.7.8 "), 0);
	T_EQ(kstrcmp(dmi_get_system_info(DMI_BIOS_DATE), "05/10/2022"), 0);
	T_EQ(kstrcmp(dmi_get_system_info(DMI_PRODUCT_NAME),
		     "InfinityBook Pro 14"),
	     0);
	T_EQ(kstrcmp(dmi_get_system_info(DMI_BOARD_NAME), "NL5xNU"), 0);
	T_EQ(dmi_get_system_info(DMI_PRODUCT_FAMILY)[0], 0);
	T_ASSERT(dmi_first_match(g_dmi_list) == &g_dmi_list[1]);
	/* the ignore lists the quirk table chose for this board */
	T_ASSERT(acpi_gpio_ignore_list_active(ACPI_GPIO_IGNORE_WAKE));
	T_ASSERT(!acpi_gpio_ignore_list_active(ACPI_GPIO_IGNORE_INTERRUPT));
	T_ASSERT(acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_WAKE, "ELAN0415:00",
					  9));
	T_ASSERT(!acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_WAKE,
					   "ELAN0415:00", 19));
	T_ASSERT(!acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_WAKE,
					   "ELAN0415:0", 9));
	T_ASSERT(!acpi_gpio_in_ignore_list(ACPI_GPIO_IGNORE_WAKE,
					   "ELAN0415:01", 9));
	T_EQ(acpi_gpio_need_run_edge_events_on_boot(), 1);
}

int main(void);
int main(void)
{
	kshim_init_main();
	workqueue_init();
	load_dmi();
	test_case("dmi", t_dmi);
	test_case("tables", t_tables);
	test_case("datasheet", t_datasheet);
	test_case("features", t_features);
	test_case("tgl_probe_and_irq", t_tgl_probe_and_irq);
	test_case("tgl_pads", t_tgl_pads);
	test_case("tgl_sleep", t_tgl_sleep);
	test_case("tgl_opregion", t_tgl_opregion);
	test_case("platform", t_platform);
	test_case("lynxpoint", t_lynxpoint);
	test_case("baytrail", test_byt);
	test_case("cherryview", test_chv);
	test_case("amd", test_amd);
	test_summary_and_exit();
	return 0;
}
