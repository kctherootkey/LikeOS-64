/*
 * Tests for the colour arithmetic of the Intel displays before DDI
 * (kernel/dev/gpu/i915/display/intel_legacy_color.c): CSC coefficient
 * formats and palette entry formats.  See test-intel-legacy-color.sh.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

/* The kernel's fixed-width types come first and stand in for stdint.h. */
#include <kernel/uapi/types.h>
#include <kernel/uapi/drm/drm_mode.h>
#include <stdio.h>
#include <string.h>

#define mm_memset memset

/* the entry conversion of drm_color_mgmt.h */
static uint32_t drm_color_lut_extract(uint32_t user_input, int bit_precision)
{
	return (uint32_t)((user_input * ((1ull << bit_precision) - 1) + 0x7fff) / 0xffff);
}

#include "color_cut.h"

static int fails, checks;
#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			fails++;                                               \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

/* S31.32 sign-magnitude from a numerator / 2^shift */
static uint64_t s3132(int neg, uint64_t num, int shift)
{
	uint64_t v = (num << 32) >> shift;

	return v | (neg ? CTM_COEFF_SIGN : 0);
}

static void test_ilk_csc(void)
{
	struct drm_color_ctm m;
	struct lcol_state s;

	/* 1.0: exponent 7 (x2), mantissa 0x800 */
	CHECK(((7u << 12) | ilk_csc_coeff_fp(CTM_COEFF_1_0, 8)) == ILK_CSC_COEFF_1_0,
	      "1.0 -> %04x", (7u << 12) | ilk_csc_coeff_fp(CTM_COEFF_1_0, 8));

	memset(&m, 0, sizeof(m));
	m.matrix[0] = s3132(0, 1, 0);	/* 1.0 */
	m.matrix[1] = s3132(0, 1, 1);	/* 0.5 */
	m.matrix[2] = s3132(1, 1, 0);	/* -1.0 */
	m.matrix[3] = s3132(0, 1, 3);	/* 0.125 */
	m.matrix[4] = s3132(0, 3, 1);	/* 1.5 */
	m.matrix[5] = s3132(0, 3, 0);	/* 3.0 */
	m.matrix[6] = s3132(0, 100, 0);	/* clamped below 4.0 */
	m.matrix[7] = s3132(0, 1, 4);	/* 0.0625 */
	m.matrix[8] = s3132(1, 1, 2);	/* -0.25 */
	memset(&s, 0xa5, sizeof(s));
	ilk_csc_convert_ctm(&m, &s);
	CHECK(s.coeff[0] == 0x7800, "1.0 %04x", s.coeff[0]);
	CHECK(s.coeff[1] == 0x0800, "0.5 %04x", s.coeff[1]);
	CHECK(s.coeff[2] == 0xf800, "-1.0 %04x", s.coeff[2]);
	/* 0.125 is the first value of the x1/4 window: exponent 2 */
	CHECK(s.coeff[3] == ((2u << 12) | 0x800), "0.125 %04x", s.coeff[3]);
	CHECK(s.coeff[4] == ((7u << 12) | 0xc00), "1.5 %04x", s.coeff[4]);
	CHECK(s.coeff[5] == ((6u << 12) | 0xc00), "3.0 %04x", s.coeff[5]);
	CHECK(s.coeff[6] == ((6u << 12) | 0xff8), "4.0- %04x", s.coeff[6]);
	CHECK(s.coeff[7] == ((3u << 12) | 0x800), "0.0625 %04x", s.coeff[7]);
	CHECK(s.coeff[8] == (0x8000u | (1u << 12) | 0x800), "-0.25 %04x", s.coeff[8]);
	CHECK(!s.preoff[0] && !s.preoff[1] && !s.preoff[2] && !s.postoff[0] && !s.postoff[1] &&
		      !s.postoff[2],
	      "offsets not cleared");
}

static void test_twos_complement(void)
{
	/* Cherryview CGM, s4.12 */
	CHECK(ctm_to_twos_complement(s3132(0, 1, 0), 4, 12) == CHV_CGM_CSC_COEFF_1_0, "chv 1.0");
	CHECK(ctm_to_twos_complement(s3132(1, 1, 0), 4, 12) == 0xf000, "chv -1.0 %04x",
	      ctm_to_twos_complement(s3132(1, 1, 0), 4, 12));
	CHECK(ctm_to_twos_complement(s3132(0, 100, 0), 4, 12) == 0x7fff, "chv clamp hi");
	CHECK(ctm_to_twos_complement(s3132(1, 100, 0), 4, 12) == 0x8000, "chv clamp lo");
	CHECK(ctm_to_twos_complement(s3132(0, 1, 1), 4, 12) == 0x800, "chv 0.5");
	/* Valleyview WGC, s2.10 */
	CHECK(ctm_to_twos_complement(s3132(0, 1, 0), 2, 10) == 0x400, "vlv 1.0");
	CHECK(ctm_to_twos_complement(s3132(1, 1, 0), 2, 10) == 0xc00, "vlv -1.0 %04x",
	      ctm_to_twos_complement(s3132(1, 1, 0), 2, 10));
	CHECK(ctm_to_twos_complement(s3132(0, 3, 0), 2, 10) == 0x7ff, "vlv clamp");
	/* rounding to nearest: 1/2048 is half an LSB of s2.10 */
	CHECK(ctm_to_twos_complement(s3132(0, 1, 11), 2, 10) == 1, "vlv round");
	CHECK(ctm_to_twos_complement(s3132(0, 1, 12), 2, 10) == 0, "vlv round down");
}

static void test_palettes(void)
{
	struct drm_color_lut lut[129];

	for (int i = 0; i < 129; i++) {
		uint16_t v = (uint16_t)(0xffff * i / 128);
		lut[i].red = lut[i].green = lut[i].blue = v;
		lut[i].reserved = 0;
	}
	/* gen2/3 10-bit: low byte, then exponent / mantissa / high bits */
	for (int i = 0; i < 128; i++) {
		uint32_t a = drm_color_lut_extract(lut[i].red, 10);
		uint32_t b = drm_color_lut_extract(lut[i + 1].red, 10);
		uint32_t ldw = i9xx_lut_10_ldw(&lut[i]);
		uint32_t udw = i9xx_lut_10_udw(&lut[i]);
		uint32_t e = (udw >> 22) & 3, mant = (udw >> 18) & 0xf, hi = (udw >> 16) & 3;
		CHECK(((ldw >> 16) & 0xff) == (a & 0xff), "ldw %d", i);
		CHECK(ldw == (((a & 0xff) << 16) | ((a & 0xff) << 8) | (a & 0xff)), "ldw ch %d", i);
		CHECK(hi == a >> 8, "udw hi %d", i);
		/* a slope below 16 is exact: exponent 3, mantissa b - a
		 * (the slope is b - a = m * 2^(3 - e)) */
		CHECK((b - a) > 0xf || (e == 3 && mant == b - a), "slope %d: %u vs m %u e %u", i,
		      b - a, mant, e);
		CHECK((b - a) <= 0xf || (mant << (3 - e)) <= b - a, "slope %d rounds up", i);
	}
	/* a steep last step clamps at 0x7f */
	{
		struct drm_color_lut st[2] = { { 0, 0, 0, 0 }, { 0xffff, 0xffff, 0xffff, 0 } };
		uint32_t udw = i9xx_lut_10_udw(st);
		CHECK(((udw >> 22) & 3) == 0 && ((udw >> 18) & 0xf) == 0xf, "steep %06x", udw);
	}
	/* gen4 10.6: low and high byte of the 16-bit values */
	{
		struct drm_color_lut c = { 0x1234, 0xabcd, 0xff00, 0 };
		CHECK(i965_lut_10p6_ldw(&c) == 0x34cd00, "10p6 ldw %06x", i965_lut_10p6_ldw(&c));
		CHECK(i965_lut_10p6_udw(&c) == 0x12abff, "10p6 udw %06x", i965_lut_10p6_udw(&c));
	}
	/* Ironlake 10-bit precision palette */
	{
		struct drm_color_lut w = { 0xffff, 0x0000, 0x8000, 0 };
		uint32_t v = ilk_lut_10(&w);
		CHECK(v == ((1023u << 20) | (0u << 10) | 512u), "ilk 10 %08x", v);
	}
}

int main(void)
{
	test_ilk_csc();
	test_twos_complement();
	test_palettes();
	printf("%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
