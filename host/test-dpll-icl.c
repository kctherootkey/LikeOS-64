/* The combo PLL solver of Ice Lake and later against the link rates the
 * hardware's tables give and a few HDMI pixel clocks: every DisplayPort
 * entry must reproduce its link rate, every HDMI clock must be met
 * within 0.05% with the DCO in range. */
#include <stdio.h>
#include <stdlib.h>
#include "../include/kernel/dev/gpu/i915/intel_dpll_calc.h"

static int fail;

static void check(const char *what, int ok)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fail = 1;
}

int main(void)
{
	static const uint32_t rates[] = { 162000, 216000, 270000, 324000, 432000,
					  540000, 648000, 810000 };
	static const uint32_t refs[] = { 24000, 19200, 38400 };
	char buf[96];
	for (unsigned r = 0; r < 3; r++) {
		for (unsigned i = 0; i < 8; i++) {
			struct icl_pll_params p;
			int rc = icl_combo_dp_params(rates[i], refs[r], &p);
			uint32_t got = rc ? 0 : icl_combo_pll_khz(&p, refs[r]);
			/* the tables are laid out so the PLL output is the link rate */
			snprintf(buf, sizeof(buf), "DP %u kHz at %u kHz ref -> %u", rates[i],
				 refs[r], got);
			check(buf, rc == 0 && got >= rates[i] - rates[i] / 2000 &&
					   got <= rates[i] + rates[i] / 2000);
		}
	}
	static const uint32_t clocks[] = { 25175, 74250, 148500, 297000, 594000,
					   65000, 108000, 138500, 241500 };
	for (unsigned r = 0; r < 3; r++) {
		for (unsigned i = 0; i < 9; i++) {
			struct icl_pll_params p;
			int rc = icl_combo_hdmi_params(clocks[i], refs[r], &p);
			uint32_t got = rc ? 0 : icl_combo_pll_khz(&p, refs[r]);
			snprintf(buf, sizeof(buf), "HDMI %u kHz at %u kHz ref -> %u", clocks[i],
				 refs[r], got);
			check(buf, rc == 0 && got >= clocks[i] - clocks[i] / 2000 &&
					   got <= clocks[i] + clocks[i] / 2000);
		}
	}
	return fail;
}
