// LikeOS -- LikeOS Image Viewer: the Styles presets.
//
// Each style is data: a master tone curve and optional per-channel curves
// (monotone splines), an optional 3x3 colour matrix, monochrome mixing
// weights, a saturation / vibrance change, split toning (a colour pushed
// into the shadows and another into the highlights) and an optional
// vignette and grain.  Applying a style evaluates those in that order and
// blends the result with the original by the intensity.
//
// The presets and their names are the LikeOS set.
//
// Copyright (C) 2026 The LikeOS Project

#include <stdlib.h>
#include <string.h>

#include "ivpriv.h"

#define MAXPTS 8

typedef struct {
	int n;
	float x[MAXPTS], y[MAXPTS];
} Curve;

typedef struct {
	const char *name;
	Curve master;
	Curve red, green, blue;		/* n = 0: identity */
	int has_matrix;
	float matrix[9];		/* row major, out = M * rgb */
	int mono;
	float mono_w[3];		/* channel mix for monochrome */
	float sat;			/* chroma factor, 1 = unchanged */
	float vib;			/* extra for muted colours, 0 = none */
	float sh_col[3], hi_col[3];	/* split toning colours */
	float sh_amt, hi_amt;		/* their strengths */
	float vignette;			/* -1 .. 1 as in Adjust */
	float grain;			/* 0 .. 1 */
} StyleDef;

#define C2(a, b, c, d) { 2, { a, c }, { b, d } }
#define C3(a, b, c, d, e, f) { 3, { a, c, e }, { b, d, f } }
#define C4(a, b, c, d, e, f, g, h) { 4, { a, c, e, g }, { b, d, f, h } }
#define C5(a, b, c, d, e, f, g, h, i, j) { 5, { a, c, e, g, i }, { b, d, f, h, j } }
#define NOCURVE { 0, { 0 }, { 0 } }
#define IDENT { 1, 0, 0, 0, 1, 0, 0, 0, 1 }

static const StyleDef styles[IV_STYLE_COUNT] = {
	[IV_STYLE_NONE] = {
		.name = "Original",
		.master = C2(0, 0, 1, 1), .red = NOCURVE, .green = NOCURVE,
		.blue = NOCURVE, .matrix = IDENT, .sat = 1.0f,
	},
	[IV_STYLE_NATURAL] = {
		.name = "Natural",
		.master = C5(0, 0, 0.25f, 0.235f, 0.5f, 0.51f, 0.75f, 0.775f, 1, 1),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 1.06f, .vib = 0.18f,
	},
	[IV_STYLE_CRISP] = {
		.name = "Crisp",
		.master = C5(0, 0, 0.22f, 0.17f, 0.5f, 0.5f, 0.78f, 0.84f, 1, 1),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 1.16f, .vib = 0.1f, .vignette = -0.12f,
	},
	[IV_STYLE_AMBER] = {
		.name = "Amber",
		.master = C3(0, 0.02f, 0.5f, 0.52f, 1, 0.985f),
		.red = C3(0, 0, 0.5f, 0.56f, 1, 1),
		.green = C3(0, 0, 0.5f, 0.515f, 1, 0.99f),
		.blue = C3(0, 0, 0.5f, 0.42f, 1, 0.9f),
		.matrix = IDENT, .sat = 1.06f,
		.hi_col = { 1.0f, 0.72f, 0.38f }, .hi_amt = 0.20f,
	},
	[IV_STYLE_GLOW] = {
		.name = "Glow",
		.master = C4(0, 0.035f, 0.3f, 0.335f, 0.62f, 0.7f, 1, 1),
		.red = C3(0, 0, 0.5f, 0.52f, 1, 1), .green = NOCURVE,
		.blue = C3(0, 0.02f, 0.5f, 0.49f, 1, 0.97f),
		.matrix = IDENT, .sat = 1.03f, .vib = 0.12f,
		.hi_col = { 1.0f, 0.82f, 0.6f }, .hi_amt = 0.16f,
	},
	[IV_STYLE_VELVET] = {
		.name = "Velvet",
		.master = C5(0, 0, 0.3f, 0.235f, 0.6f, 0.56f, 0.85f, 0.85f, 1, 0.97f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.has_matrix = 1,
		.matrix = { 1.06f, -0.04f, -0.02f, -0.02f, 1.02f, 0.0f, 0.0f, -0.04f, 1.04f },
		.sat = 1.14f, .sh_col = { 0.45f, 0.2f, 0.4f }, .sh_amt = 0.16f,
		.vignette = -0.28f,
	},
	[IV_STYLE_FROST] = {
		.name = "Frost",
		.master = C3(0, 0.04f, 0.5f, 0.56f, 1, 1),
		.red = C3(0, 0, 0.5f, 0.46f, 1, 0.97f),
		.green = C3(0, 0, 0.5f, 0.505f, 1, 1),
		.blue = C3(0, 0.03f, 0.5f, 0.57f, 1, 1),
		.matrix = IDENT, .sat = 0.74f,
		.hi_col = { 0.78f, 0.9f, 1.0f }, .hi_amt = 0.14f,
	},
	[IV_STYLE_AZURE] = {
		.name = "Azure",
		.master = C4(0, 0, 0.3f, 0.28f, 0.7f, 0.73f, 1, 1),
		.red = NOCURVE, .green = NOCURVE,
		.blue = C3(0, 0, 0.5f, 0.55f, 1, 1),
		.has_matrix = 1,
		.matrix = { 1.0f, 0.0f, 0.0f, -0.03f, 1.03f, 0.0f, -0.06f, -0.02f, 1.08f },
		.sat = 1.1f, .vib = 0.25f,
	},
	[IV_STYLE_NOIR] = {
		.name = "Noir",
		.master = C5(0, 0, 0.2f, 0.09f, 0.5f, 0.5f, 0.8f, 0.91f, 1, 1),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .mono = 1, .mono_w = { 0.38f, 0.52f, 0.10f },
		.sat = 1.0f, .vignette = -0.3f, .grain = 0.22f,
	},
	[IV_STYLE_SILVER] = {
		.name = "Silver",
		.master = C4(0, 0.06f, 0.35f, 0.38f, 0.75f, 0.78f, 1, 0.96f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .mono = 1, .mono_w = { 0.30f, 0.59f, 0.11f },
		.sat = 1.0f, .sh_col = { 0.55f, 0.62f, 0.72f }, .sh_amt = 0.10f,
	},
	[IV_STYLE_SEPIA_PRINT] = {
		.name = "Sepia Print",
		.master = C3(0, 0.05f, 0.5f, 0.5f, 1, 0.95f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .mono = 1, .mono_w = { 0.33f, 0.56f, 0.11f },
		.sat = 1.0f, .sh_col = { 0.42f, 0.27f, 0.14f }, .sh_amt = 0.90f,
		.hi_col = { 1.0f, 0.88f, 0.7f }, .hi_amt = 0.50f,
		.vignette = -0.15f, .grain = 0.08f,
	},
	[IV_STYLE_FILM] = {
		.name = "Film",
		.master = C5(0, 0.06f, 0.25f, 0.235f, 0.5f, 0.5f, 0.75f, 0.785f, 1, 0.95f),
		.red = C3(0, 0.02f, 0.5f, 0.5f, 1, 0.98f),
		.green = C3(0, 0.03f, 0.5f, 0.5f, 1, 0.97f),
		.blue = C3(0, 0.05f, 0.5f, 0.48f, 1, 0.92f),
		.matrix = IDENT, .sat = 0.9f,
		.sh_col = { 0.2f, 0.42f, 0.38f }, .sh_amt = 0.10f,
		.grain = 0.2f,
	},
	[IV_STYLE_HAZE] = {
		.name = "Haze",
		.master = C3(0, 0.14f, 0.5f, 0.55f, 1, 0.93f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 0.8f,
		.hi_col = { 1.0f, 0.93f, 0.78f }, .hi_amt = 0.24f,
	},
	[IV_STYLE_NEON_NIGHT] = {
		.name = "Neon Night",
		.master = C4(0, 0, 0.25f, 0.15f, 0.5f, 0.47f, 1, 1),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 1.24f,
		.sh_col = { 0.0f, 0.55f, 0.75f }, .sh_amt = 0.36f,
		.hi_col = { 1.0f, 0.3f, 0.8f }, .hi_amt = 0.28f,
		.vignette = -0.22f,
	},
	[IV_STYLE_HARBOR] = {
		.name = "Harbor",
		.master = C4(0, 0.01f, 0.28f, 0.25f, 0.72f, 0.75f, 1, 0.99f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 1.04f,
		.sh_col = { 0.05f, 0.45f, 0.5f }, .sh_amt = 0.32f,
		.hi_col = { 1.0f, 0.74f, 0.48f }, .hi_amt = 0.24f,
	},
	[IV_STYLE_BLOOM] = {
		.name = "Bloom",
		.master = C3(0, 0.05f, 0.5f, 0.58f, 1, 1),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 0.95f, .vib = 0.35f,
		.hi_col = { 1.0f, 0.78f, 0.86f }, .hi_amt = 0.16f,
	},
	[IV_STYLE_MEADOW] = {
		.name = "Meadow",
		.master = C4(0, 0, 0.3f, 0.3f, 0.7f, 0.72f, 1, 1),
		.red = NOCURVE,
		.green = C3(0, 0, 0.5f, 0.53f, 1, 1), .blue = NOCURVE,
		.has_matrix = 1,
		.matrix = { 1.02f, 0.0f, -0.02f, -0.04f, 1.08f, -0.04f, -0.02f, 0.0f, 1.02f },
		.sat = 1.1f, .vib = 0.15f,
		.hi_col = { 1.0f, 0.9f, 0.6f }, .hi_amt = 0.12f,
	},
	[IV_STYLE_EMBER] = {
		.name = "Ember",
		.master = C4(0, 0, 0.3f, 0.27f, 0.7f, 0.73f, 1, 1),
		.red = C3(0, 0, 0.5f, 0.57f, 1, 1),
		.green = C3(0, 0, 0.5f, 0.49f, 1, 0.98f),
		.blue = C3(0, 0, 0.5f, 0.43f, 1, 0.92f),
		.has_matrix = 1,
		.matrix = { 1.08f, -0.05f, -0.03f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.96f },
		.sat = 1.2f, .vignette = -0.2f,
	},
	[IV_STYLE_DUSK] = {
		.name = "Dusk",
		.master = C4(0, 0.02f, 0.3f, 0.28f, 0.7f, 0.72f, 1, 0.98f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 0.98f,
		.sh_col = { 0.45f, 0.28f, 0.65f }, .sh_amt = 0.32f,
		.hi_col = { 1.0f, 0.7f, 0.68f }, .hi_amt = 0.24f,
		.vignette = -0.12f,
	},
	[IV_STYLE_MATTE] = {
		.name = "Matte",
		.master = C3(0, 0.10f, 0.5f, 0.5f, 1, 0.95f),
		.red = NOCURVE, .green = NOCURVE, .blue = NOCURVE,
		.matrix = IDENT, .sat = 0.85f,
	},
	[IV_STYLE_LAGOON] = {
		.name = "Lagoon",
		.master = C4(0, 0, 0.3f, 0.29f, 0.7f, 0.73f, 1, 1),
		.red = NOCURVE,
		.green = C3(0, 0, 0.5f, 0.52f, 1, 1),
		.blue = C3(0, 0, 0.5f, 0.53f, 1, 1),
		.matrix = IDENT, .sat = 1.1f,
		.sh_col = { 0.0f, 0.55f, 0.55f }, .sh_amt = 0.28f,
		.hi_col = { 1.0f, 0.86f, 0.62f }, .hi_amt = 0.20f,
	},
};

const char *iv_style_name(IvStyle s)
{
	if ((int)s < 0 || s >= IV_STYLE_COUNT)
		return "";
	return styles[s].name;
}

/* Sample a curve at an arbitrary value through a 1024-entry table. */
static float curve_at(const float *lut, float v)
{
	v = ivp_clampf(v, 0.0f, 1.0f) * 1023.0f;
	int i = (int)v;
	if (i >= 1023)
		return lut[1023];
	float f = v - (float)i;
	return lut[i] + (lut[i + 1] - lut[i]) * f;
}

/* The style's per-channel byte tables: channel curve after master curve. */
static int build_luts(const StyleDef *sd, float lut[3][256])
{
	float *m = malloc(sizeof(float) * 1024 * 4);
	if (m == NULL)
		return -1;
	float *ch[3] = { m + 1024, m + 2048, m + 3072 };
	const Curve *cc[3] = { &sd->red, &sd->green, &sd->blue };
	ivp_curve_lut(sd->master.x, sd->master.y, sd->master.n, m, 1024);
	for (int c = 0; c < 3; c++) {
		if (cc[c]->n >= 2)
			ivp_curve_lut(cc[c]->x, cc[c]->y, cc[c]->n, ch[c], 1024);
		else
			for (int i = 0; i < 1024; i++)
				ch[c][i] = (float)i / 1023.0f;
	}
	for (int c = 0; c < 3; c++)
		for (int i = 0; i < 256; i++)
			lut[c][i] = curve_at(ch[c], curve_at(m, (float)i / 255.0f));
	free(m);
	return 0;
}

void iv_style_apply(IvImage *img, IvStyle s, double intensity)
{
	if (img == NULL || img->px == NULL || (int)s <= 0 || s >= IV_STYLE_COUNT)
		return;
	if (!(intensity > 0.0))
		return;
	if (intensity > 1.0)
		intensity = 1.0;
	const StyleDef *sd = &styles[s];
	float (*lut)[256] = malloc(sizeof(float) * 3 * 256);
	if (lut == NULL || build_luts(sd, lut) < 0) {
		free(lut);
		return;
	}
	float *nx2 = NULL, *ny2 = NULL;
	if (sd->vignette != 0.0f)
		ivp_vignette_tables(img->w, img->h, &nx2, &ny2);

	/* Split-toning offsets: the colour's chroma only, so toning moves hue
	 * and not brightness. */
	float sh_off[3], hi_off[3];
	float shy = ivp_luma(sd->sh_col[0], sd->sh_col[1], sd->sh_col[2]);
	float hiy = ivp_luma(sd->hi_col[0], sd->hi_col[1], sd->hi_col[2]);
	for (int c = 0; c < 3; c++) {
		sh_off[c] = (sd->sh_col[c] - shy) * sd->sh_amt;
		hi_off[c] = (sd->hi_col[c] - hiy) * sd->hi_amt;
	}
	const float k = (float)intensity;
	/* Grain cell relative to the image: a preview and the full image get
	 * grain of the same apparent size. */
	int lng = img->w > img->h ? img->w : img->h;
	double cell = 1.6 * (double)lng / 3000.0;

	for (int y = 0; y < img->h; y++) {
		uint8_t *p = ivp_px(img, 0, y);
		for (int x = 0; x < img->w; x++, p += 4) {
			float r0 = (float)p[0] / 255.0f, g0 = (float)p[1] / 255.0f,
			      b0 = (float)p[2] / 255.0f;
			float r = lut[0][p[0]], g = lut[1][p[1]], b = lut[2][p[2]];
			if (sd->has_matrix) {
				const float *M = sd->matrix;
				float nr = M[0] * r + M[1] * g + M[2] * b;
				float ng = M[3] * r + M[4] * g + M[5] * b;
				float nb = M[6] * r + M[7] * g + M[8] * b;
				r = nr;
				g = ng;
				b = nb;
			}
			if (sd->mono) {
				float m = sd->mono_w[0] * r + sd->mono_w[1] * g +
					  sd->mono_w[2] * b;
				r = g = b = m;
			}
			if (sd->sat != 1.0f || sd->vib != 0.0f) {
				float Y = ivp_luma(r, g, b);
				float f = sd->sat;
				if (sd->vib != 0.0f) {
					float sat;
					ivp_hue_sat(ivp_clampf(r, 0, 1), ivp_clampf(g, 0, 1),
						    ivp_clampf(b, 0, 1), NULL, &sat);
					f *= 1.0f + sd->vib * (1.0f - sat) * (1.0f - sat);
				}
				r = Y + (r - Y) * f;
				g = Y + (g - Y) * f;
				b = Y + (b - Y) * f;
			}
			if (sd->sh_amt != 0.0f || sd->hi_amt != 0.0f) {
				float Y = ivp_clampf(ivp_luma(r, g, b), 0.0f, 1.0f);
				float ws = (1.0f - Y) * (1.0f - Y);
				float wh = Y * Y;
				r += sh_off[0] * ws + hi_off[0] * wh;
				g += sh_off[1] * ws + hi_off[1] * wh;
				b += sh_off[2] * ws + hi_off[2] * wh;
			}
			if (nx2 != NULL) {
				float fall = ivp_vignette_falloff(nx2[x], ny2[y]);
				float v = sd->vignette;
				if (v < 0.0f) {
					float m = 1.0f + 0.78f * v * fall;
					r *= m;
					g *= m;
					b *= m;
				} else {
					float m = 1.0f - 0.75f * v * fall;
					r = 1.0f - (1.0f - r) * m;
					g = 1.0f - (1.0f - g) * m;
					b = 1.0f - (1.0f - b) * m;
				}
			}
			if (sd->grain > 0.0f) {
				float Y = ivp_clampf(ivp_luma(r, g, b), 0.0f, 1.0f);
				float n = ivp_grain_sample(x, y, cell, 0x6EA1u + (uint32_t)s) *
					  sd->grain * 0.1f * (0.35f + 2.6f * Y * (1.0f - Y));
				r += n;
				g += n;
				b += n;
			}
			ivp_gamut_fit(&r, &g, &b);
			p[0] = ivp_to_u8(r0 + (r - r0) * k);
			p[1] = ivp_to_u8(g0 + (g - g0) * k);
			p[2] = ivp_to_u8(b0 + (b - b0) * k);
		}
	}
	free(nx2);
	free(ny2);
	free(lut);
}
