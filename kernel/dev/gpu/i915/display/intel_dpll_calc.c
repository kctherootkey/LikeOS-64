// LikeOS-64 -- pure PLL arithmetic for the display PLLs of Ice Lake and
// later (combo PHY DCO/divider values), kept free of kernel headers so
// the build host can run it (host/test-dpll-icl.sh).
#include <kernel/dev/gpu/i915/intel_dpll_calc.h>

/* The combo PLL's DCO and dividers for the DisplayPort link rates, per
 * reference clock, as the hardware's tuning tables give them (the 38.4
 * MHz reference uses the 19.2 MHz table: the PLL halves it). */
struct dp_entry {
	uint32_t link_khz;
	uint16_t dco_integer, dco_fraction;
	uint8_t pdiv, kdiv, qdiv_mode, qdiv_ratio;
};

static const struct dp_entry dp_24mhz[] = {
	{ 540000, 0x151, 0x4000, 0x2, 1, 0, 0 },
	{ 270000, 0x151, 0x4000, 0x2, 2, 0, 0 },
	{ 162000, 0x151, 0x4000, 0x4, 2, 0, 0 },
	{ 324000, 0x151, 0x4000, 0x4, 1, 0, 0 },
	{ 216000, 0x168, 0x0000, 0x1, 2, 1, 2 },
	{ 432000, 0x168, 0x0000, 0x1, 2, 0, 0 },
	{ 648000, 0x195, 0x0000, 0x2, 1, 0, 0 },
	{ 810000, 0x151, 0x4000, 0x1, 1, 0, 0 },
};

static const struct dp_entry dp_19_2mhz[] = {
	{ 540000, 0x1A5, 0x7000, 0x2, 1, 0, 0 },
	{ 270000, 0x1A5, 0x7000, 0x2, 2, 0, 0 },
	{ 162000, 0x1A5, 0x7000, 0x4, 2, 0, 0 },
	{ 324000, 0x1A5, 0x7000, 0x4, 1, 0, 0 },
	{ 216000, 0x1C2, 0x0000, 0x1, 2, 1, 2 },
	{ 432000, 0x1C2, 0x0000, 0x1, 2, 0, 0 },
	{ 648000, 0x1FA, 0x2000, 0x2, 1, 0, 0 },
	{ 810000, 0x1A5, 0x7000, 0x1, 1, 0, 0 },
};

int icl_combo_dp_params(uint32_t link_khz, uint32_t ref_khz,
			struct icl_pll_params *out)
{
	const struct dp_entry *t = ref_khz == 24000 ? dp_24mhz : dp_19_2mhz;
	for (unsigned i = 0; i < sizeof(dp_24mhz) / sizeof(dp_24mhz[0]); i++) {
		if (t[i].link_khz != link_khz)
			continue;
		out->dco_integer = t[i].dco_integer;
		out->dco_fraction = t[i].dco_fraction;
		out->pdiv = t[i].pdiv;
		out->kdiv = t[i].kdiv;
		out->qdiv_mode = t[i].qdiv_mode;
		out->qdiv_ratio = t[i].qdiv_ratio;
		return 0;
	}
	return -1;
}

/* HDMI: the DCO must land between 7998 and 10000 MHz; the AFE clock
 * (5x the pixel clock) is the DCO over p*q*k, with the divider split
 * the way the hardware's own tables split it. */
#define ICL_DCO_MIN_KHZ 7998000u
#define ICL_DCO_MAX_KHZ 10000000u
#define ICL_DCO_MID_KHZ 8999000u

static int split_divider(uint32_t div, uint32_t *p, uint32_t *q, uint32_t *k)
{
	if (div % 2 == 0) {
		if (div == 2) {
			*p = 2; *q = 1; *k = 1;
		} else if (div % 4 == 0) {
			*p = 2; *q = div / 4; *k = 2;
		} else if (div % 6 == 0) {
			*p = 3; *q = div / 6; *k = 2;
		} else if (div % 5 == 0) {
			*p = 5; *q = div / 10; *k = 2;
		} else if (div % 14 == 0) {
			*p = 7; *q = div / 14; *k = 2;
		} else {
			return -1;
		}
	} else if (div == 3 || div == 5 || div == 7) {
		*p = div; *q = 1; *k = 1;
	} else if (div == 9 || div == 15 || div == 21) {
		*p = div / 3; *q = 1; *k = 3;
	} else {
		return -1;
	}
	return 0;
}

int icl_combo_hdmi_params(uint32_t clock_khz, uint32_t ref_khz,
			  struct icl_pll_params *out)
{
	static const uint8_t dividers[] = { 2, 4, 6, 8, 10, 12, 14, 16, 18, 20,
					    24, 28, 30, 32, 36, 40, 42, 44, 48,
					    50, 52, 54, 56, 60, 64, 66, 68, 70,
					    72, 76, 78, 80, 84, 88, 90, 92, 96,
					    98, 100, 102, 3, 5, 7, 9, 15, 21 };
	uint64_t afe = (uint64_t)clock_khz * 5;
	uint32_t best_div = 0;
	uint64_t best_dco = 0, best_dev = ~0ULL;

	for (unsigned i = 0; i < sizeof(dividers); i++) {
		uint64_t dco = afe * dividers[i];
		if (dco < ICL_DCO_MIN_KHZ || dco > ICL_DCO_MAX_KHZ)
			continue;
		uint64_t dev = dco > ICL_DCO_MID_KHZ ? dco - ICL_DCO_MID_KHZ :
						       ICL_DCO_MID_KHZ - dco;
		if (dev < best_dev) {
			best_dev = dev;
			best_div = dividers[i];
			best_dco = dco;
		}
	}
	if (!best_div)
		return -1;
	uint32_t p, q, k;
	if (split_divider(best_div, &p, &q, &k) != 0)
		return -1;
	/* the DCO counts the reference; a 38.4 MHz reference is halved */
	uint32_t ref = ref_khz == 38400 ? 19200 : ref_khz;
	uint64_t dco_fp = (best_dco << 15) / ref;
	out->dco_integer = (uint16_t)(dco_fp >> 15);
	out->dco_fraction = (uint16_t)(dco_fp & 0x7fff);
	out->pdiv = p == 2 ? 0x1 : p == 3 ? 0x2 : p == 5 ? 0x4 : 0x8;
	out->kdiv = k == 1 ? 1 : k == 2 ? 2 : 4;
	out->qdiv_mode = q != 1;
	out->qdiv_ratio = q != 1 ? (uint8_t)q : 0;
	return 0;
}

/* What a set of parameters runs at, in kHz (the inverse, for checks). */
uint32_t icl_combo_pll_khz(const struct icl_pll_params *p, uint32_t ref_khz)
{
	uint32_t ref = ref_khz == 38400 ? 19200 : ref_khz;
	uint64_t dco = ((uint64_t)p->dco_integer << 15 | p->dco_fraction) * ref >> 15;
	uint32_t pdiv = p->pdiv == 0x1 ? 2 : p->pdiv == 0x2 ? 3 : p->pdiv == 0x4 ? 5 : 7;
	uint32_t kdiv = p->kdiv == 1 ? 1 : p->kdiv == 2 ? 2 : 3;
	uint32_t qdiv = p->qdiv_mode ? p->qdiv_ratio : 1;
	/* the port clock is the DCO over the dividers, over 5 (AFE) */
	return (uint32_t)(dco / (pdiv * kdiv * qdiv) / 5);
}
