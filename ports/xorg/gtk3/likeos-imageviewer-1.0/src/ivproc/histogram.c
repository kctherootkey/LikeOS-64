// LikeOS -- LikeOS Image Viewer: histograms.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "ivpriv.h"

void iv_histogram(const IvImage *img, uint32_t out[4][256])
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof(uint32_t) * 4 * 256);
	if (img == NULL || img->px == NULL)
		return;
	for (int y = 0; y < img->h; y++) {
		const uint8_t *p = ivp_px(img, 0, y);
		for (int x = 0; x < img->w; x++, p += 4) {
			if (p[3] == 0)
				continue;
			out[0][p[0]]++;
			out[1][p[1]]++;
			out[2][p[2]]++;
			/* Rec. 709 luma in 8.8 fixed point; the weights sum to
			 * 256, so white maps exactly to 255. */
			out[3][(54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8]++;
		}
	}
}
