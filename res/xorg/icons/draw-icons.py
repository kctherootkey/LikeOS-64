#!/usr/bin/env python3
# LikeOS -- draw the full-colour artwork of the LikeOS icon theme as SVG.
#
# Copyright (C) 2026 The LikeOS Project
#
# Every folder, document, device, application, dialog symbol, the LikeOS
# mark and the start orb of res/xorg/icons/LikeOS is drawn by this script, from a handful of
# shared shapes, so the family stays consistent: one folder, one page, one
# app tile, one palette.  The output is res/xorg/icons/src/<context>/*.svg,
# committed; gen-icons.py rasterizes those files into the PNG theme.
#
# Each icon is written in four variants, because a drawing that is good at
# 256 pixels is mush at 16:
#
#   <name>.svg      the master, a 64-unit drawing with full detail, used for
#                   48 pixels and up.  Main edges sit on multiples of four
#                   units, so they land on pixel boundaries at 48 and 96 too.
#   <name>-32.svg   32 pixels, edges snapped to the pixel grid.
#   <name>-24.svg   24 pixels (also drawn at 22), snapped, less detail.
#   <name>-16.svg   16 pixels, snapped, emblems as pixel art, no labels.
#
# All four come from the same functions: a drawing asks the canvas to snap
# an edge (Cv.q) or a stroke centre (Cv.qc) and to tell it how much detail
# the size can carry (Cv.px), and the canvas does the rest.
#
# Usage: draw-icons.py [--out res/xorg/icons/src]
# Needs nothing but Python 3.

import argparse
import math
import os

# ---------------------------------------------------------------------------
# Palette.
# ---------------------------------------------------------------------------
ACCENT = '#2a5bd7'
ACCENT_HI = '#3b74f0'
INK = '#141a24'

FOLDER_BACK = ('#2a5bd7', '#2f66e3')
FOLDER_FRONT = ('#6a9cf7', '#4a82f0')

PAGE_EDGE = '#b9c1ce'
PAGE_FILL = ('#ffffff', '#f2f5fa')
FOLD_FILL = ('#f4f6fa', '#d9dfe9')
LINE_GREY = '#c4cbd7'

# Document type colours.
T_PDF = '#d63a3a'
T_DOC = '#2a5bd7'
T_XLS = '#1f9d55'
T_PPT = '#d9622b'
T_ZIP = '#d98a00'
T_CODE = '#7a4fd6'
T_AUDIO = '#e8743b'
T_VIDEO = '#c2368f'
T_IMAGE = '#1a9e9e'
T_TEXT = '#6b7486'
T_SLATE = '#4d5a70'

# Device bodies.
SLATE_LIGHT = ('#f3f5f9', '#c9d0dc')
SLATE_DARK = ('#535e75', '#2c3344')
METAL = ('#e3e8ef', '#aab3c2')
SCREEN = ('#2a5bd7', '#7fb2ff')
DEVICE_EDGE = '#8d97a8'


def shade(hex_colour, f):
	"""Lighten (f > 0) towards white or darken (f < 0) towards black."""
	r, g, b = (int(hex_colour[i:i + 2], 16) for i in (1, 3, 5))
	if f >= 0:
		r, g, b = (round(c + (255 - c) * f) for c in (r, g, b))
	else:
		r, g, b = (round(c * (1 + f)) for c in (r, g, b))
	return '#%02x%02x%02x' % (r, g, b)


def n(v):
	s = ('%.3f' % v).rstrip('0').rstrip('.')
	return '0' if s in ('-0', '') else s


# ---------------------------------------------------------------------------
# The canvas.  All drawing is in 64-unit design space inside one scale
# transform; the canvas only decides where edges land.
# ---------------------------------------------------------------------------
class Cv:
	def __init__(self, px):
		self.px = px or 64
		self.master = not px
		self.k = self.px / 64.0
		self.defs = []
		self.body = []
		self.ids = 0

	# -- grid ------------------------------------------------------------
	def q(self, v):
		"""An edge, on a pixel boundary."""
		if self.master:
			return v
		return round(v * self.k) / self.k

	def qc(self, v, wpx=1):
		"""A stroke centre for a stroke wpx pixels wide."""
		if self.master:
			return v
		if round(wpx) % 2:
			return (math.floor(v * self.k) + 0.5) / self.k
		return round(v * self.k) / self.k

	def u(self, pixels):
		"""A length of so many pixels, in design units."""
		return pixels / self.k

	def pick(self, p16, p24, p32, master):
		return {16: p16, 24: p24, 32: p32}.get(self.px, master)

	# -- paint -----------------------------------------------------------
	def _id(self, kind):
		self.ids += 1
		return '%s%d' % (kind, self.ids)

	def lin(self, x1, y1, x2, y2, *stops):
		"""A linear gradient; stops are colours, or (offset, colour[,
		opacity]) tuples."""
		gid = self._id('g')
		s = []
		for i, st in enumerate(stops):
			if isinstance(st, str):
				st = (i / max(1, len(stops) - 1), st)
			op = '' if len(st) < 3 else ' stop-opacity="%s"' % n(st[2])
			s.append('<stop offset="%s" stop-color="%s"%s/>' %
				 (n(st[0]), st[1], op))
		self.defs.append('<linearGradient id="%s" gradientUnits='
				 '"userSpaceOnUse" x1="%s" y1="%s" x2="%s" '
				 'y2="%s">%s</linearGradient>' %
				 (gid, n(x1), n(y1), n(x2), n(y2), ''.join(s)))
		return 'url(#%s)' % gid

	def vgrad(self, y1, y2, *stops):
		return self.lin(0, y1, 0, y2, *stops)

	def rad(self, cx, cy, r, *stops, fx=None, fy=None):
		gid = self._id('r')
		s = []
		for i, st in enumerate(stops):
			if isinstance(st, str):
				st = (i / max(1, len(stops) - 1), st)
			op = '' if len(st) < 3 else ' stop-opacity="%s"' % n(st[2])
			s.append('<stop offset="%s" stop-color="%s"%s/>' %
				 (n(st[0]), st[1], op))
		f = ''
		if fx is not None:
			f = ' fx="%s" fy="%s"' % (n(fx), n(fy))
		self.defs.append('<radialGradient id="%s" gradientUnits='
				 '"userSpaceOnUse" cx="%s" cy="%s" r="%s"%s>%s'
				 '</radialGradient>' %
				 (gid, n(cx), n(cy), n(r), f, ''.join(s)))
		return 'url(#%s)' % gid

	def blur(self, sd):
		fid = self._id('f')
		self.defs.append('<filter id="%s" x="-25%%" y="-25%%" '
				 'width="150%%" height="150%%"><feGaussianBlur '
				 'stdDeviation="%s"/></filter>' % (fid, n(sd)))
		return 'url(#%s)' % fid

	def clip(self, d):
		cid = self._id('c')
		self.defs.append('<clipPath id="%s"><path d="%s"/></clipPath>' %
				 (cid, d))
		return 'url(#%s)' % cid

	# -- output ----------------------------------------------------------
	def add(self, s):
		self.body.append(s)

	def path(self, d, fill='none', stroke=None, sw=None, op=None,
		 cap='round', join='round', extra=''):
		a = ['d="%s"' % d, 'fill="%s"' % fill]
		if stroke:
			a.append('stroke="%s" stroke-width="%s" stroke-linecap='
				 '"%s" stroke-linejoin="%s"' %
				 (stroke, n(sw), cap, join))
		if op is not None:
			a.append('opacity="%s"' % n(op))
		if extra:
			a.append(extra)
		self.add('<path %s/>' % ' '.join(a))

	def svg(self, comment):
		k = self.k
		head = ('<?xml version="1.0" encoding="UTF-8"?>\n'
			'<!-- %s\n'
			'     Drawn by res/xorg/icons/draw-icons.py: edit that, '
			'not this file.\n'
			'     Copyright (C) 2026 The LikeOS Project -->\n' % comment)
		return (head +
			'<svg xmlns="http://www.w3.org/2000/svg" width="%d" '
			'height="%d" viewBox="0 0 %d %d">\n<defs>%s</defs>\n'
			'<g transform="scale(%s)">\n%s\n</g>\n</svg>\n' %
			(self.px, self.px, self.px, self.px, ''.join(self.defs),
			 n(k), '\n'.join(self.body)))


# ---------------------------------------------------------------------------
# Path helpers (design units).
# ---------------------------------------------------------------------------
def rr(x0, y0, x1, y1, r):
	"""Rounded rectangle."""
	r = max(0.0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
	if r <= 0:
		return 'M%s %sH%sV%sH%sZ' % (n(x0), n(y0), n(x1), n(y1), n(x0))
	return ('M%s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sH%s'
		'A%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %sZ' %
		(n(x0 + r), n(y0), n(x1 - r), n(r), n(r), n(x1), n(y0 + r),
		 n(y1 - r), n(r), n(r), n(x1 - r), n(y1), n(x0 + r), n(r), n(r),
		 n(x0), n(y1 - r), n(y0 + r), n(r), n(r), n(x0 + r), n(y0)))


def circ(cx, cy, r):
	return ('M%s %sA%s %s 0 1 0 %s %sA%s %s 0 1 0 %s %sZ' %
		(n(cx - r), n(cy), n(r), n(r), n(cx + r), n(cy), n(r), n(r),
		 n(cx - r), n(cy)))


def ell(cx, cy, rx, ry):
	return ('M%s %sA%s %s 0 1 0 %s %sA%s %s 0 1 0 %s %sZ' %
		(n(cx - rx), n(cy), n(rx), n(ry), n(cx + rx), n(cy), n(rx),
		 n(ry), n(cx - rx), n(cy)))


def poly(pts, close=True):
	d = 'M' + 'L'.join('%s %s' % (n(x), n(y)) for x, y in pts)
	return d + ('Z' if close else '')


def pixels(cv, rows, x0, y0, colour, op=1.0):
	"""Pixel art at 16 pixels: '#' full, '+' half, anything else empty.
	x0, y0 are pixel coordinates."""
	full, half = [], []
	for j, row in enumerate(rows):
		for i, ch in enumerate(row):
			if ch in '#+':
				(full if ch == '#' else half).append(
					'M%s %sh%sv%sh-%sZ' %
					(n(cv.u(x0 + i)), n(cv.u(y0 + j)), n(cv.u(1)),
					 n(cv.u(1)), n(cv.u(1))))
	if full:
		cv.path(''.join(full), fill=colour, op=op)
	if half:
		cv.path(''.join(half), fill=colour, op=op * 0.5)


def drop_shadow(cv, d, dy=1.2, sd=1.1, op=0.22):
	"""A soft shadow under a silhouette, at the master size only."""
	if not cv.master:
		return
	cv.path(d, fill='#0b1730', op=op,
		extra='transform="translate(0 %s)" filter="%s"' %
		(n(dy), cv.blur(sd)))


def hairline(cv):
	"""One pixel, in design units (the master assumes 64 pixels)."""
	return cv.u(1) if not cv.master else 1.0


# ---------------------------------------------------------------------------
# Lettering for the document bands.  Pixel letters (3x5, a few wider) at
# 24 and 32 pixels; a monoline vector alphabet at the master size.
# ---------------------------------------------------------------------------
PIXEL_FONT = {
	'A': ['.#.', '#.#', '###', '#.#', '#.#'],
	'B': ['##.', '#.#', '##.', '#.#', '##.'],
	'C': ['.##', '#..', '#..', '#..', '.##'],
	'D': ['##.', '#.#', '#.#', '#.#', '##.'],
	'E': ['###', '#..', '##.', '#..', '###'],
	'F': ['###', '#..', '##.', '#..', '#..'],
	'H': ['#.#', '#.#', '###', '#.#', '#.#'],
	'I': ['###', '.#.', '.#.', '.#.', '###'],
	'J': ['..#', '..#', '..#', '#.#', '.#.'],
	'K': ['#.#', '#.#', '##.', '#.#', '#.#'],
	'L': ['#..', '#..', '#..', '#..', '###'],
	'M': ['#...#', '##.##', '#.#.#', '#...#', '#...#'],
	'N': ['#..#', '##.#', '#.##', '#..#', '#..#'],
	'O': ['.#.', '#.#', '#.#', '#.#', '.#.'],
	'P': ['##.', '#.#', '##.', '#..', '#..'],
	'R': ['##.', '#.#', '##.', '#.#', '#.#'],
	'S': ['.##', '#..', '.#.', '..#', '##.'],
	'T': ['###', '.#.', '.#.', '.#.', '.#.'],
	'X': ['#.#', '#.#', '.#.', '#.#', '#.#'],
	'Y': ['#.#', '#.#', '.#.', '.#.', '.#.'],
	'Z': ['###', '..#', '.#.', '#..', '###'],
}

# (width, path) on a box 6 units tall.
VECTOR_FONT = {
	'A': (4, 'M0 6L2 0l2 6M0.7 4h2.6'),
	'B': (3.8, 'M0 3h2.2a1.6 1.5 0 0 1 0 3H0V0h2a1.45 1.5 0 0 1 0 3'),
	'C': (3.8, 'M3.8 0.9C3.3 0.3 2.7 0 2 0 0.7 0 0 1.4 0 3s0.7 3 2 3'
		   'c0.7 0 1.3-0.3 1.8-0.9'),
	'D': (3.8, 'M0 0v6h1.4C3.1 6 3.8 4.6 3.8 3S3.1 0 1.4 0Z'),
	'E': (3.4, 'M3.4 0H0v6h3.4M0 3h2.8'),
	'F': (3.4, 'M3.4 0H0v6M0 3h2.8'),
	'H': (3.8, 'M0 0v6M3.8 0v6M0 3h3.8'),
	'I': (2.4, 'M1.2 0v6M0 0h2.4M0 6h2.4'),
	'J': (3.2, 'M3.2 0v4.1C3.2 5.3 2.6 6 1.6 6 0.8 6 0.3 5.6 0 4.9'),
	'K': (3.8, 'M0 0v6M3.8 0L0 4M1.3 2.6L3.8 6'),
	'L': (3.4, 'M0 0v6h3.4'),
	'M': (5, 'M0 6V0l2.5 4.2L5 0v6'),
	'N': (4, 'M0 6V0l4 6V0'),
	'O': (4, 'M2 0C0.7 0 0 1.4 0 3s0.7 3 2 3 2-1.4 2-3-0.7-3-2-3Z'),
	'P': (3.8, 'M0 6V0h2.2a1.6 1.6 0 0 1 0 3.2H0'),
	'R': (3.8, 'M0 6V0h2.2a1.6 1.6 0 0 1 0 3.2H0M2.2 3.2L3.8 6'),
	'S': (3.8, 'M3.6 0.8C3.2 0.3 2.6 0 1.9 0 0.9 0 0.2 0.6 0.2 1.5c0 2.1 '
		   '3.6 1 3.6 3 0 0.9-0.8 1.5-1.9 1.5-0.8 0-1.5-0.3-1.9-0.9'),
	'T': (4, 'M0 0h4M2 0v6'),
	'X': (4, 'M0 0l4 6M4 0L0 6'),
	'Y': (4, 'M0 0l2 3 2-3M2 3v3'),
	'Z': (3.8, 'M0 0h3.8L0 6h3.8'),
}


def label(cv, text, x0, y0, x1, y1, colour='#ffffff'):
	"""Letters centred in the band (x0, y0)-(x1, y1).  Returns False when
	the size cannot carry them."""
	if cv.px == 16:
		return False
	if not cv.master:
		gap = 1
		w = sum(len(PIXEL_FONT[c][0]) for c in text) + gap * (len(text) - 1)
		bw = round((x1 - x0) * cv.k)
		bh = round((y1 - y0) * cv.k)
		if w > bw - 2 or bh < 7:
			return False
		px0 = round(x0 * cv.k) + (bw - w) // 2
		py0 = round(y0 * cv.k) + (bh - 5) // 2
		for c in text:
			pixels(cv, PIXEL_FONT[c], px0, py0, colour)
			px0 += len(PIXEL_FONT[c][0]) + gap
		return True
	gap = 1.5
	h = 6.0
	w = sum(VECTOR_FONT[c][0] for c in text) + gap * (len(text) - 1)
	scale = min(1.0, (x1 - x0 - 6) / w)
	x = (x0 + x1) / 2 - w * scale / 2
	y = (y0 + y1) / 2 - h * scale / 2
	for c in text:
		cw, d = VECTOR_FONT[c]
		cv.add('<path d="%s" fill="none" stroke="%s" stroke-width="%s" '
		       'stroke-linecap="round" stroke-linejoin="round" '
		       'transform="translate(%s %s) scale(%s)"/>' %
		       (d, colour, n(1.6 / scale), n(x), n(y), n(scale)))
		x += (cw + gap) * scale
	return True


# ---------------------------------------------------------------------------
# Line glyphs (emblems).  Each takes the canvas, a centre, a half-size h and
# a stroke width in design units and draws white lines.  At 16 pixels the
# same emblems exist as pixel art (EMBLEM_PIXELS) instead.
# ---------------------------------------------------------------------------
def _g(cx, cy, h):
	def T(x, y):
		return '%s %s' % (n(cx + x * h), n(cy + y * h))
	return T


def g_home(cv, cx, cy, h, sw, col):
	T = _g(cx, cy, h)
	cv.path('M%sL%sL%s' % (T(-0.95, 0.0), T(0, -0.85), T(0.95, 0.0)),
		stroke=col, sw=sw)
	cv.path('M%sL%sL%sL%s' % (T(-0.62, -0.28), T(-0.62, 0.85),
				  T(0.62, 0.85), T(0.62, -0.28)),
		stroke=col, sw=sw)
	cv.path('M%sL%sL%sL%s' % (T(-0.2, 0.85), T(-0.2, 0.35), T(0.2, 0.35),
				  T(0.2, 0.85)), stroke=col, sw=sw)


def g_desktop(cv, cx, cy, h, sw, col):
	cv.path(rr(cx - 0.95 * h, cy - 0.8 * h, cx + 0.95 * h, cy + 0.4 * h,
		   0.18 * h), stroke=col, sw=sw)
	T = _g(cx, cy, h)
	cv.path('M%sL%sM%sL%s' % (T(0, 0.4), T(0, 0.85), T(-0.45, 0.88),
				  T(0.45, 0.88)), stroke=col, sw=sw)


def g_document(cv, cx, cy, h, sw, col):
	T = _g(cx, cy, h)
	cv.path('M%sL%sL%sL%sL%sZ' % (T(-0.65, -0.9), T(0.2, -0.9),
				      T(0.65, -0.45), T(0.65, 0.9),
				      T(-0.65, 0.9)), stroke=col, sw=sw)
	cv.path('M%sL%sM%sL%s' % (T(-0.3, 0.0), T(0.3, 0.0), T(-0.3, 0.45),
				  T(0.3, 0.45)), stroke=col, sw=sw)


def g_download(cv, cx, cy, h, sw, col):
	T = _g(cx, cy, h)
	cv.path('M%sL%sM%sL%sL%s' % (T(0, -0.9), T(0, 0.4), T(-0.5, -0.1),
				     T(0, 0.42), T(0.5, -0.1)), stroke=col, sw=sw)
	cv.path('M%sL%s' % (T(-0.85, 0.88), T(0.85, 0.88)), stroke=col, sw=sw)


def g_music(cv, cx, cy, h, sw, col):
	T = _g(cx, cy, h)
	cv.path('M%sL%sL%sL%s' % (T(-0.28, 0.55), T(-0.28, -0.7),
				  T(0.72, -0.9), T(0.72, 0.38)),
		stroke=col, sw=sw)
	cv.path(ell(cx - 0.52 * h, cy + 0.58 * h, 0.3 * h, 0.25 * h) +
		ell(cx + 0.48 * h, cy + 0.42 * h, 0.3 * h, 0.25 * h), fill=col)


def g_picture(cv, cx, cy, h, sw, col):
	cv.path(rr(cx - 0.95 * h, cy - 0.75 * h, cx + 0.95 * h, cy + 0.75 * h,
		   0.18 * h), stroke=col, sw=sw)
	T = _g(cx, cy, h)
	cv.path('M%sL%sL%sL%sL%s' % (T(-0.95, 0.55), T(-0.3, -0.05),
				     T(0.15, 0.35), T(0.42, 0.12),
				     T(0.95, 0.55)), stroke=col, sw=sw)
	cv.path(circ(cx + 0.42 * h, cy - 0.3 * h, 0.16 * h), fill=col)


def g_video(cv, cx, cy, h, sw, col):
	cv.path(rr(cx - 0.95 * h, cy - 0.68 * h, cx + 0.95 * h, cy + 0.68 * h,
		   0.2 * h), stroke=col, sw=sw)
	T = _g(cx, cy, h)
	cv.path('M%sL%sL%sZ' % (T(-0.22, -0.35), T(0.4, 0), T(-0.22, 0.35)),
		fill=col, stroke=col, sw=sw * 0.6)


def g_templates(cv, cx, cy, h, sw, col):
	cv.path(rr(cx - 0.85 * h, cy - 0.85 * h, cx + 0.85 * h, cy + 0.85 * h,
		   0.15 * h), stroke=col, sw=sw)
	T = _g(cx, cy, h)
	cv.path('M%sL%sM%sL%s' % (T(-0.85, -0.3), T(0.85, -0.3), T(-0.2, -0.3),
				  T(-0.2, 0.85)), stroke=col, sw=sw)


def g_public(cv, cx, cy, h, sw, col):
	T = _g(cx, cy, h)
	cv.path(circ(cx - 0.38 * h, cy - 0.42 * h, 0.27 * h), stroke=col, sw=sw)
	cv.path(circ(cx + 0.45 * h, cy - 0.3 * h, 0.22 * h), stroke=col,
		sw=sw * 0.9)
	cv.path('M%sC%s %s %sM%sC%s %s %s' %
		(T(-0.95, 0.85), T(-0.95, 0.05), T(0.2, 0.05), T(0.2, 0.85),
		 T(0.45, 0.2), T(0.75, 0.2), T(0.98, 0.4), T(0.98, 0.8)),
		stroke=col, sw=sw)


def g_globe(cv, cx, cy, h, sw, col):
	cv.path(circ(cx, cy, 0.9 * h), stroke=col, sw=sw)
	cv.path(ell(cx, cy, 0.38 * h, 0.9 * h), stroke=col, sw=sw * 0.85)
	T = _g(cx, cy, h)
	cv.path('M%sL%s' % (T(-0.9, 0), T(0.9, 0)), stroke=col, sw=sw * 0.85)


def g_star(cv, cx, cy, h, sw, col):
	pts = []
	for i in range(10):
		a = -math.pi / 2 + i * math.pi / 5
		r = h * (0.95 if i % 2 == 0 else 0.42)
		pts.append((cx + r * math.cos(a), cy + r * math.sin(a) + 0.06 * h))
	cv.path(poly(pts), stroke=col, sw=sw)


EMBLEMS = {
	'home': g_home, 'desktop': g_desktop, 'documents': g_document,
	'download': g_download, 'music': g_music, 'pictures': g_picture,
	'videos': g_video, 'templates': g_templates, 'public': g_public,
	'remote': g_globe,
}

# The same emblems at 16 pixels: 7 x 6 (columns 4-10, rows 7-12).
EMBLEM_PIXELS = {
	'home': ['...#...',
		 '..###..',
		 '.#####.',
		 '#######',
		 '.##.##.',
		 '.##.##.'],
	'desktop': ['#######',
		    '#.....#',
		    '#.....#',
		    '#######',
		    '...#...',
		    '..###..'],
	'documents': ['.####..',
		      '.#..##.',
		      '.#...#.',
		      '.#.#.#.',
		      '.#...#.',
		      '.#####.'],
	'download': ['...#...',
		     '...#...',
		     '.#.#.#.',
		     '..###..',
		     '...#...',
		     '#######'],
	'music': ['..#####',
		  '..#...#',
		  '..#...#',
		  '..#..##',
		  '###.###',
		  '##.....'],
	'pictures': ['#######',
		     '#....##',
		     '#.....#',
		     '#.#...#',
		     '#####.#',
		     '#######'],
	'videos': ['#######',
		   '##.####',
		   '##..###',
		   '##..###',
		   '##.####',
		   '#######'],
	'templates': ['#######',
		      '#.....#',
		      '#######',
		      '#.#...#',
		      '#.#...#',
		      '#######'],
	'public': ['.#...#.',
		   '###.###',
		   '.#...#.',
		   '.......',
		   '###.###',
		   '###.###'],
	'remote': ['..###..',
		   '.#.#.#.',
		   '#######',
		   '#..#..#',
		   '.#.#.#.',
		   '..###..'],
}


# The emblems at 24 pixels, as pixel art too: a one-pixel line is all the
# room there is, and a vector line of that width lands between pixels.
EMBLEM_PIXELS_24 = {
	'home': ['....#....',
		 '...###...',
		 '..#####..',
		 '.#######.',
		 '#########',
		 '.###.###.',
		 '.###.###.',
		 '.###.###.'],
	'desktop': ['#########',
		    '#.......#',
		    '#.......#',
		    '#.......#',
		    '#########',
		    '....#....',
		    '..#####..'],
	'documents': ['#####..',
		      '#...##.',
		      '#....##',
		      '#.###.#',
		      '#.....#',
		      '#.###.#',
		      '#.....#',
		      '#######'],
	'download': ['....#....',
		     '....#....',
		     '....#....',
		     '.#..#..#.',
		     '..#.#.#..',
		     '...###...',
		     '....#....',
		     '#########'],
	'music': ['..######',
		  '..#....#',
		  '..#....#',
		  '..#....#',
		  '.##...##',
		  '###..###',
		  '###..###',
		  '.#....#.'],
	'pictures': ['#########',
		     '#.....#.#',
		     '#.......#',
		     '#..#....#',
		     '#.###.#.#',
		     '#########'],
	'videos': ['#########',
		   '#..#....#',
		   '#..##...#',
		   '#..###..#',
		   '#..##...#',
		   '#..#....#',
		   '#########'],
	'templates': ['#########',
		      '#.......#',
		      '#########',
		      '#..#....#',
		      '#..#....#',
		      '#..#....#',
		      '#########'],
	'public': ['.##...##.',
		   '.##...##.',
		   '.........',
		   '####.####',
		   '####.####'],
	'remote': ['...###...',
		   '.##.#.##.',
		   '.#..#..#.',
		   '#########',
		   '.#..#..#.',
		   '.##.#.##.',
		   '...###...'],
}


# ---------------------------------------------------------------------------
# Folders.
# ---------------------------------------------------------------------------
def folder_back(cv):
	x0, x1 = cv.q(4), cv.q(60)
	yt, yb = cv.q(8), cv.q(54)
	ybody = cv.q(13) if cv.px != 16 else cv.q(12)
	tr = cv.q(24)
	r = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 3.5)
	slope = cv.pick(cv.u(1), cv.u(2), cv.u(2), 5)
	d = ('M%s %sA%s %s 0 0 1 %s %sH%s'
	     'C%s %s %s %s %s %sH%sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 1 %s %s'
	     'H%sA%s %s 0 0 1 %s %sZ' %
	     (n(x0), n(yt + r), n(r), n(r), n(x0 + r), n(yt), n(tr),
	      n(tr + slope * 0.5), n(yt), n(tr + slope * 0.5), n(ybody),
	      n(tr + slope), n(ybody), n(x1 - r), n(r), n(r), n(x1),
	      n(ybody + r), n(yb - r), n(r), n(r), n(x1 - r), n(yb),
	      n(x0 + r), n(r), n(r), n(x0), n(yb - r)))
	return d


def rpoly(pts, r):
	"""A closed polygon with every corner rounded by r."""
	d = []
	m = len(pts)
	for i in range(m):
		p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % m]
		l0 = math.hypot(p0[0] - p1[0], p0[1] - p1[1])
		l2 = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
		t0 = min(0.5, r / l0) if l0 else 0
		t2 = min(0.5, r / l2) if l2 else 0
		a = (p1[0] + (p0[0] - p1[0]) * t0, p1[1] + (p0[1] - p1[1]) * t0)
		b = (p1[0] + (p2[0] - p1[0]) * t2, p1[1] + (p2[1] - p1[1]) * t2)
		d.append('%s%s %sQ%s %s %s %s' % ('M' if i == 0 else 'L',
						  n(a[0]), n(a[1]), n(p1[0]),
						  n(p1[1]), n(b[0]), n(b[1])))
	return ''.join(d) + 'Z'


def folder_shape(cv, open_=False):
	"""Back panel, a sheet of paper, front panel; returns the front
	panel's box (x0, y0, x1, y1) for an emblem."""
	back = folder_back(cv)
	x0, x1 = cv.q(4), cv.q(60)
	yb = cv.q(54)
	r = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 3.5)
	drop_shadow(cv, back)
	cv.path(back, fill=cv.vgrad(8, 54, *FOLDER_BACK))
	if cv.px != 16:
		# A sheet of paper between the panels: a sliver when closed,
		# more of it when open.
		sy = cv.q(16) if not open_ else cv.q(12)
		sx0, sx1 = cv.q(8), cv.q(56)
		if open_ and not cv.master:
			sx1 = cv.q(54)
		cv.path(rr(sx0, sy, sx1, cv.q(44), cv.pick(0, cv.u(1), cv.u(1),
							   1.5)),
			fill=cv.vgrad(sy, 30, '#ffffff', '#dfe6f2'))
		if open_ and cv.px != 24:
			text_lines(cv, (sx0 + cv.pick(0, 0, cv.u(3), 6),
					sy + cv.pick(0, 0, cv.u(2), 4),
					sx1 - cv.pick(0, 0, cv.u(5), 12),
					cv.q(24)), count=cv.pick(0, 0, 1, 2))
	if not open_:
		top = cv.q(20)
		front = rr(x0, top, x1, yb, r)
		hx0, hx1 = x0 + r, x1 - r
		box = (x0, top, x1, yb)
	else:
		top = cv.q(24) if cv.px != 16 else cv.q(24)
		sk = cv.pick(cv.u(1), cv.u(2), cv.u(2), 5)
		front = rpoly([(x0 + sk, top), (x1 + sk * 0.6, top), (x1, yb),
			       (x0, yb)], r)
		hx0, hx1 = x0 + sk + r, x1 + sk * 0.6 - r
		box = (x0 + sk * 0.5, top, x1 + sk * 0.3, yb)
	if cv.master:
		# The front panel's shadow on what is behind it.
		cv.path(front, fill='#0d2a78', op=0.3 if open_ else 0.28,
			extra='transform="translate(0 %s)" filter="%s"' %
			('-1.6' if open_ else '-1.2', cv.blur(1.0)))
	cv.path(front, fill=cv.vgrad(top, yb, *FOLDER_FRONT))
	# The light top edge of the front panel.
	hw = hairline(cv)
	cv.path('M%s %sH%s' % (n(hx0), n(top + hw / 2), n(hx1)),
		stroke='#ffffff', sw=hw, op=0.6 if cv.master else 0.45,
		cap='butt')
	if cv.master:
		# A faint darker bottom edge to seat it.
		cv.path('M%s %sH%s' % (n(x0 + r), n(yb - 0.5), n(x1 - r)),
			stroke='#1d47b0', sw=1, op=0.35, cap='butt')
	return box


def place_pixels(cv, rows, box, colour, op=1.0):
	"""Pixel art centred in box (design units), whole pixels only."""
	w = max(len(r) for r in rows)
	h = len(rows)
	bx0, by0 = cv.q(box[0]) * cv.k, cv.q(box[1]) * cv.k
	bx1, by1 = cv.q(box[2]) * cv.k, cv.q(box[3]) * cv.k
	x = math.floor((bx0 + bx1 - w) / 2 + 0.5)
	y = math.floor((by0 + by1 - h) / 2 + 0.5)
	pixels(cv, rows, x, y, colour, op)


def folder_icon(emblem=None, open_=False):
	def draw(cv):
		fx0, fy0, fx1, fy1 = folder_shape(cv, open_)
		if not emblem:
			return
		if cv.px in (16, 24):
			art = (EMBLEM_PIXELS if cv.px == 16 else
			       EMBLEM_PIXELS_24)[emblem]
			# Below the highlight row, above the bottom edge.
			place_pixels(cv, art, (fx0, fy0 + cv.u(1), fx1, fy1),
				     '#ffffff', 0.95)
			return
		cx = (fx0 + fx1) / 2
		cy = (fy0 + fy1) / 2 + cv.pick(0, 0, 0, 0.5)
		h, sw = cv.pick(None, None, (cv.u(5.5), cv.u(2)), (10, 2.8))
		if cv.px == 32:
			cx, cy = cv.q(cx), cv.q(cy)
		EMBLEMS[emblem](cv, cx, cy, h, sw, '#ffffff')
	return draw


# ---------------------------------------------------------------------------
# Document pages.
# ---------------------------------------------------------------------------
def page_geometry(cv):
	"""(x0, y0, x1, y1, fold) of the page."""
	if cv.px == 16:
		return (cv.u(3), cv.u(1), cv.u(13), cv.u(15), cv.u(4))
	if cv.px == 24:
		return (cv.u(4), cv.u(1), cv.u(20), cv.u(23), cv.u(6))
	if cv.px == 32:
		return (cv.u(6), cv.u(2), cv.u(26), cv.u(30), cv.u(7))
	return (12, 4, 52, 60, 14)


def band_geometry(cv):
	if cv.px == 16:
		return (cv.u(4), cv.u(10), cv.u(12), cv.u(13))
	if cv.px == 24:
		return (cv.u(5), cv.u(14), cv.u(19), cv.u(21))
	if cv.px == 32:
		return (cv.u(7), cv.u(18), cv.u(25), cv.u(27))
	return (15, 40, 49, 53)


def page(cv, colour=None, text=None, band_glyph=None):
	"""The page, its fold and, if colour is set, the type band with the
	label (or a glyph when the band is too small or has no label).
	Returns the content box above the band."""
	x0, y0, x1, y1, f = page_geometry(cv)
	r = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 3)
	hw = hairline(cv)
	outline = ('M%s %sH%sL%s %sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %s'
		   'V%sA%s %s 0 0 1 %s %sZ' %
		   (n(x0 + r), n(y0), n(x1 - f), n(x1), n(y0 + f), n(y1 - r),
		    n(r), n(r), n(x1 - r), n(y1), n(x0 + r), n(r), n(r), n(x0),
		    n(y1 - r), n(y0 + r), n(r), n(r), n(x0 + r), n(y0)))
	drop_shadow(cv, outline, dy=1.0, sd=1.0, op=0.18)
	# Outline as a filled silhouette with the page inset by one pixel: a
	# stroke would straddle the pixel grid.
	cv.path(outline, fill=PAGE_EDGE)
	inner = ('M%s %sH%sL%s %sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %s'
		 'V%sA%s %s 0 0 1 %s %sZ' %
		 (n(x0 + r), n(y0 + hw), n(x1 - f - hw * 0.4), n(x1 - hw),
		  n(y0 + f + hw * 0.4), n(y1 - r), n(r - hw), n(r - hw),
		  n(x1 - r), n(y1 - hw), n(x0 + r), n(r - hw), n(r - hw),
		  n(x0 + hw), n(y1 - r), n(y0 + r), n(r - hw), n(r - hw),
		  n(x0 + r), n(y0 + hw)))
	cv.path(inner, fill=cv.vgrad(y0, y1, *PAGE_FILL))
	# The fold.
	fr = cv.pick(0, cv.u(1), cv.u(1), 2)
	fold = ('M%s %sV%sQ%s %s %s %sH%sZ' %
		(n(x1 - f), n(y0), n(y0 + f - fr), n(x1 - f), n(y0 + f),
		 n(x1 - f + fr), n(y0 + f), n(x1)))
	if cv.master:
		cv.path(fold, fill='#0b1730', op=0.12,
			extra='transform="translate(-0.6 0.9)" filter="%s"' %
			cv.blur(0.8))
	cv.path(fold, fill=PAGE_EDGE)
	fold_in = ('M%s %sV%sQ%s %s %s %sH%sZ' %
		   (n(x1 - f + hw), n(y0 + hw * 2.4), n(y0 + f - fr - hw),
		    n(x1 - f + hw), n(y0 + f - hw), n(x1 - f + fr + hw),
		    n(y0 + f - hw), n(x1 - hw * 2.4)))
	cv.path(fold_in, fill=cv.lin(x1 - f, y0 + f, x1 - f * 0.4, y0 + f * 0.4,
				     *FOLD_FILL))
	bx0, by0, bx1, by1 = band_geometry(cv)
	if colour:
		br = cv.pick(0, cv.u(1), cv.u(1.5), 2.5)
		band = rr(bx0, by0, bx1, by1, br)
		cv.path(band, fill=cv.vgrad(by0, by1, shade(colour, 0.12),
					    shade(colour, -0.08)))
		if cv.master:
			cv.path('M%s %sH%s' % (n(bx0 + br), n(by0 + 0.5),
					       n(bx1 - br)),
				stroke='#ffffff', sw=1, op=0.3, cap='butt')
		drawn = False
		if text:
			drawn = label(cv, text, bx0, by0, bx1, by1)
		if not drawn and band_glyph and cv.master:
			band_glyph(cv, (bx0 + bx1) / 2, (by0 + by1) / 2,
				   (by1 - by0) * 0.3)
	top = y0 + cv.pick(cv.u(3), cv.u(4), cv.u(5), 11)
	bottom = (by0 if colour else y1) - cv.pick(cv.u(1), cv.u(1.5),
						   cv.u(2), 4)
	return (x0 + cv.pick(cv.u(2), cv.u(3), cv.u(3), 6), top,
		x1 - cv.pick(cv.u(2), cv.u(3), cv.u(3), 6), bottom)


def text_lines(cv, box, colour=LINE_GREY, count=None, widths=None,
	       heading=None):
	"""Grey text lines in box.  At small sizes, crisp one-pixel lines."""
	x0, y0, x1, y1 = box
	w = cv.pick(1, 1, 1, None)
	if cv.master:
		lw, step = 2.2, 5.2
	else:
		lw, step = cv.u(1), cv.u(cv.pick(2, 2, 3, 0))
	widths = widths or [1.0, 0.82, 0.94, 0.66, 0.88, 0.74]
	y = y0 + lw / 2
	i = 0
	while y + lw / 2 <= y1 + 0.01 and (count is None or i < count):
		col = heading if (heading and i == 0) else colour
		xe = x0 + (x1 - x0) * widths[i % len(widths)]
		if cv.master:
			cv.path('M%s %sH%s' % (n(x0 + lw / 2), n(y),
					       n(xe - lw / 2)),
				stroke=col, sw=lw)
		else:
			yy = cv.qc(y, w)
			cv.path('M%s %sH%s' % (n(cv.q(x0)), n(yy),
					       n(cv.q(xe))),
				stroke=col, sw=cv.u(1), cap='butt')
		y += step
		i += 1


def doc_icon(colour=None, text=None, content=None, band_glyph=None):
	def draw(cv):
		box = page(cv, colour, text, band_glyph)
		if content:
			content(cv, box)
	return draw


# Band glyphs (white, inside the band) -- used where there is no label.
def bg_note(cv, cx, cy, h):
	g_music(cv, cx, cy, h, cv.u(1) if not cv.master else 1.4, '#ffffff')


def bg_play(cv, cx, cy, h):
	cv.path(poly([(cx - 0.7 * h, cy - 0.9 * h), (cx + 0.9 * h, cy),
		       (cx - 0.7 * h, cy + 0.9 * h)]), fill='#ffffff',
		stroke='#ffffff', sw=0.6 if cv.master else cv.u(0.5))


def bg_picture(cv, cx, cy, h):
	cv.path(poly([(cx - 1.4 * h, cy + 0.85 * h), (cx - 0.4 * h, cy - 0.5 * h),
		       (cx + 0.2 * h, cy + 0.3 * h), (cx + 0.6 * h, cy - 0.1 * h),
		       (cx + 1.4 * h, cy + 0.85 * h)]), fill='#ffffff')
	cv.path(circ(cx + 0.9 * h, cy - 0.55 * h, 0.32 * h), fill='#ffffff')


def bg_info(cv, cx, cy, h):
	cv.path(circ(cx, cy - 0.75 * h, 0.2 * h), fill='#ffffff')
	cv.path('M%s %sV%s' % (n(cx), n(cy - 0.15 * h), n(cy + 0.85 * h)),
		stroke='#ffffff', sw=0.38 * h)


def bg_arrow(cv, cx, cy, h):
	cv.path('M%s %sL%s %sM%s %sH%sV%s' %
		(n(cx - 0.7 * h), n(cy + 0.7 * h), n(cx + 0.7 * h),
		 n(cy - 0.7 * h), n(cx - 0.1 * h), n(cy - 0.7 * h),
		 n(cx + 0.7 * h), n(cy - 0.1 * h)),
		stroke='#ffffff', sw=0.3 * h)


def bg_disc(cv, cx, cy, h):
	cv.path(circ(cx, cy, 0.95 * h) + circ(cx, cy, 0.3 * h), fill='#ffffff',
		extra='fill-rule="evenodd"')


# Page contents.
def c_lines(heading=None):
	def f(cv, box):
		text_lines(cv, box, heading=heading)
	return f


def c_glyph(colour, glyph):
	"""A pictogram of the type in its colour, centred in the content box."""
	def f(cv, box):
		x0, y0, x1, y1 = box
		cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
		if cv.px == 16:
			return
		h = min(x1 - x0, y1 - y0) / 2 * 0.9
		sw = cv.pick(None, cv.u(1), cv.u(1.5), 2.4)
		glyph(cv, cx, cy, h, sw, colour)
	return f


def c_picture(cv, box, on_page=True):
	"""A small photo: sky, sun, two hills."""
	x0, y0, x1, y1 = box
	if cv.px == 16 and on_page:
		x0, y0, x1, y1 = cv.u(5), cv.u(4), cv.u(11), cv.u(9)
	r = cv.pick(0, cv.u(1), cv.u(1), 2)
	clip = cv.clip(rr(x0, y0, x1, y1, r))
	cv.add('<g clip-path="%s">' % clip)
	cv.path(rr(x0, y0, x1, y1, 0), fill=cv.vgrad(y0, y1, '#8fd3e6',
						     '#d7f1f3'))
	w, h = x1 - x0, y1 - y0
	cv.path(poly([(x0, y1), (x0, y0 + h * 0.62), (x0 + w * 0.32, y0 + h * 0.3),
		       (x0 + w * 0.62, y0 + h * 0.72), (x0 + w * 0.62, y1)]),
		fill=cv.vgrad(y0 + h * 0.3, y1, '#22b3a8', T_IMAGE))
	cv.path(poly([(x0 + w * 0.35, y1), (x0 + w * 0.7, y0 + h * 0.48),
		       (x1, y0 + h * 0.75), (x1, y1)]),
		fill=cv.vgrad(y0 + h * 0.48, y1, '#159088', '#0e7470'))
	cv.add('</g>')
	if cv.px != 16:
		cv.path(circ(x0 + w * 0.76, y0 + h * 0.27, max(h * 0.12,
							       cv.u(1))),
			fill='#ffd166')


def c_video(cv, box):
	x0, y0, x1, y1 = box
	if cv.px == 16:
		x0, y0, x1, y1 = cv.u(5), cv.u(4), cv.u(11), cv.u(9)
	r = cv.pick(cv.u(1), cv.u(1), cv.u(1.5), 2.5)
	cv.path(rr(x0, y0, x1, y1, r), fill=cv.vgrad(y0, y1, '#d4519f',
						     '#9c2470'))
	cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
	h = (y1 - y0) * 0.28
	cv.path(poly([(cx - 0.75 * h, cy - h), (cx + h, cy),
		       (cx - 0.75 * h, cy + h)]), fill='#ffffff')
	if cv.master:
		# Film perforations.
		for x in range(int(x0) + 3, int(x1) - 2, 5):
			cv.path(rr(x, y0 + 1.6, x + 2.4, y0 + 3.4, 0.6),
				fill='#ffffff', op=0.55)
			cv.path(rr(x, y1 - 3.4, x + 2.4, y1 - 1.6, 0.6),
				fill='#ffffff', op=0.55)


def c_audio(cv, box):
	x0, y0, x1, y1 = box
	cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
	if cv.px == 16:
		pixels(cv, ['..###', '..#.#', '..#.#', '###.#', '##.##'], 5, 4,
		       T_AUDIO)
		return
	h = min(x1 - x0, y1 - y0) / 2 * 0.95
	g_music(cv, cx, cy, h, cv.pick(None, cv.u(1), cv.u(1.5), 2.6), T_AUDIO)


def c_sheet(cv, box):
	"""A small table: header row in green, grid lines."""
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['#######', '#..#..#', '#######', '#..#..#'], 5, 4,
		       T_XLS, 0.9)
		return
	hw = hairline(cv)
	sw = cv.pick(None, cv.u(1), cv.u(1), 1.4)
	rows = cv.pick(None, 3, 4, 4)
	cv.path(rr(x0, y0, x1, y0 + (y1 - y0) / rows, cv.pick(0, 0, 0, 1)),
		fill=T_XLS, op=0.9)
	d = []
	for i in range(1, rows + 1):
		y = cv.qc(y0 + (y1 - y0) * i / rows, 1) if not cv.master else \
			y0 + (y1 - y0) * i / rows
		d.append('M%s %sH%s' % (n(x0), n(y - (hw / 2 if i == rows and
							 cv.master else 0)),
					n(x1)))
	for t in (0.38, 0.69):
		x = cv.qc(x0 + (x1 - x0) * t, 1) if not cv.master else \
			x0 + (x1 - x0) * t
		d.append('M%s %sV%s' % (n(x), n(y0), n(y1)))
	cv.path(''.join(d), stroke=shade(T_XLS, 0.35), sw=sw, cap='butt')


def c_slide(cv, box):
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['#######', '#.....#', '#.##..#', '#######'], 5, 4,
		       T_PPT, 0.9)
		return
	r = cv.pick(0, cv.u(1), cv.u(1), 1.5)
	cv.path(rr(x0, y0, x1, y1, r), fill=shade(T_PPT, 0.82))
	w, h = x1 - x0, y1 - y0
	# A bar chart on the slide.
	bw = w * 0.14
	for i, t in enumerate((0.45, 0.75, 0.3)):
		bx = x0 + w * (0.18 + i * 0.24)
		cv.path(rr(cv.q(bx), cv.q(y1 - h * 0.12 - h * t * 0.8),
			   cv.q(bx + bw) if not cv.master else bx + bw,
			   cv.q(y1 - h * 0.12), 0), fill=T_PPT)


def c_zip(cv, box):
	"""A zip running down the page."""
	x0, y0, x1, y1 = box
	cx = (x0 + x1) / 2
	if cv.px == 16:
		pixels(cv, ['.#', '#.', '.#', '#.', '##'], 7, 3, T_ZIP)
		return
	step = cv.pick(None, cv.u(2), cv.u(2), 4)
	tw = cv.pick(None, cv.u(1), cv.u(2), 4)
	y = y0
	i = 0
	while y + step <= y1 - step * 1.5:
		x = cv.q(cx) - (tw if i % 2 == 0 else 0)
		cv.path(rr(x, cv.q(y), x + tw, cv.q(y + step / 2 + 0.01), 0),
			fill=T_ZIP)
		y += step / 2
		i += 1
	# The pull.
	pw = cv.pick(None, cv.u(3), cv.u(4), 7)
	cv.path(rr(cv.q(cx - pw / 2), cv.q(y), cv.q(cx + pw / 2),
		   cv.q(y + pw * 1.2), cv.pick(0, cv.u(1), cv.u(1), 1.6)),
		fill=shade(T_ZIP, -0.15))


def c_code(colour=T_CODE):
	"""Indented code lines."""
	def f(cv, box):
		x0, y0, x1, y1 = box
		if cv.px == 16:
			pixels(cv, ['####..', '......', '.#####', '......',
				    '.###..'], 5, 4, colour, 0.9)
			return
		w = x1 - x0
		lines = [(0, 0.55), (0.18, 0.75), (0.18, 0.5), (0.36, 0.62),
			 (0, 0.3), (0.18, 0.7)]
		lw = 2.2 if cv.master else cv.u(1)
		step = 5.2 if cv.master else cv.u(cv.pick(2, 2, 3, 0))
		y = y0 + lw / 2
		for i, (ind, ln) in enumerate(lines):
			if y + lw / 2 > y1 + 0.01:
				break
			col = colour if i % 3 == 0 else LINE_GREY
			xs, xe = x0 + w * ind, x0 + w * min(1, ind + ln)
			if cv.master:
				cv.path('M%s %sH%s' % (n(xs + lw / 2), n(y),
						       n(xe - lw / 2)),
					stroke=col, sw=lw)
			else:
				cv.path('M%s %sH%s' % (n(cv.q(xs)), n(cv.qc(y)),
						       n(cv.q(xe))),
					stroke=col, sw=cv.u(1), cap='butt')
			y += step
	return f


def c_brackets(open_, close, colour=T_CODE):
	"""A pair of bracket glyphs: '<' '>' with a slash, or braces."""
	def f(cv, box):
		x0, y0, x1, y1 = box
		cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
		if cv.px == 16:
			pix = (['.#...#.', '#.....#', '.#...#.']
			       if open_ == '<' else
			       ['.#...#.', '#.....#', '.#...#.'])
			pixels(cv, ['.#..#.#.', '#..#...#', '.#.#..#.']
			       if open_ == '<' else pix, 4, 5, colour)
			return
		h = min(x1 - x0, y1 - y0) / 2 * 0.75
		sw = cv.pick(None, cv.u(1.2), cv.u(1.6), 2.6)
		if open_ == '<':
			cv.path('M%s %sL%s %sL%s %sM%s %sL%s %sL%s %sM%s %sL%s %s' %
				(n(cx - 0.45 * h), n(cy - 0.7 * h),
				 n(cx - 1.15 * h), n(cy), n(cx - 0.45 * h),
				 n(cy + 0.7 * h), n(cx + 0.45 * h),
				 n(cy - 0.7 * h), n(cx + 1.15 * h), n(cy),
				 n(cx + 0.45 * h), n(cy + 0.7 * h),
				 n(cx + 0.2 * h), n(cy - 0.9 * h),
				 n(cx - 0.2 * h), n(cy + 0.9 * h)),
				stroke=colour, sw=sw)
		else:
			for s in (-1, 1):
				# s = -1 is the opening brace: its ends point
				# right, its middle point left.
				x = cx + s * 0.6 * h
				o = -s
				cv.path('M%s %sQ%s %s %s %sV%sQ%s %s %s %s'
					'Q%s %s %s %sV%sQ%s %s %s %s' %
					(n(x + o * 0.35 * h), n(cy - h),
					 n(x), n(cy - h), n(x), n(cy - 0.6 * h),
					 n(cy - 0.3 * h), n(x), n(cy),
					 n(x - o * 0.4 * h), n(cy),
					 n(x), n(cy), n(x), n(cy + 0.3 * h),
					 n(cy + 0.6 * h), n(x), n(cy + h),
					 n(x + o * 0.35 * h), n(cy + h)),
					stroke=colour, sw=sw)
	return f


def c_diff(cv, box):
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['####..', '......', '.#####', '......',
			    '.###..'], 5, 4, T_XLS, 0.9)
		return
	lw = 2.2 if cv.master else cv.u(1)
	step = 5.2 if cv.master else cv.u(cv.pick(2, 2, 3, 0))
	y = y0 + lw / 2
	cols = [LINE_GREY, T_PDF, T_XLS, T_XLS, LINE_GREY, T_PDF]
	widths = [0.8, 0.95, 0.7, 0.9, 0.6, 0.85]
	i = 0
	while y + lw / 2 <= y1 + 0.01 and i < len(cols):
		xe = x0 + (x1 - x0) * widths[i]
		if cv.master:
			cv.path('M%s %sH%s' % (n(x0 + lw / 2), n(y),
					       n(xe - lw / 2)),
				stroke=cols[i], sw=lw)
		else:
			cv.path('M%s %sH%s' % (n(cv.q(x0)), n(cv.qc(y)),
					       n(cv.q(xe))),
				stroke=cols[i], sw=cv.u(1), cap='butt')
		y += step
		i += 1


def c_font(cv, box):
	"""Two letter forms: a large 'A' and a smaller 'a'-like bowl."""
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['..#....', '.#.#...', '.###.##', '#...#.#',
			    '#...###'], 4, 4, INK, 0.85)
		return
	h = (y1 - y0)
	sw = cv.pick(None, cv.u(1.3), cv.u(2), 3.2)
	ax = x0 + (x1 - x0) * 0.3
	cv.path('M%s %sL%s %sL%s %sM%s %sH%s' %
		(n(ax - h * 0.42), n(y1), n(ax), n(y0), n(ax + h * 0.42), n(y1),
		 n(ax - h * 0.25), n(y0 + h * 0.66), n(ax + h * 0.25)),
		stroke=INK, sw=sw, join='round')
	bx = x0 + (x1 - x0) * 0.8
	r = h * 0.24
	cv.path(circ(bx - r * 0.2, y1 - r, r), stroke=INK, sw=sw * 0.85)
	cv.path('M%s %sV%s' % (n(bx + r * 0.85), n(y1 - 2 * r - sw * 0.2),
			       n(y1)), stroke=INK, sw=sw * 0.85)


def c_calendar(cv, box):
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['#######', '#+#+#+#', '#######', '#+#+#+#'], 5, 4,
		       ACCENT, 0.9)
		return
	r = cv.pick(0, cv.u(1), cv.u(1), 1.5)
	hh = (y1 - y0) * 0.3
	cv.path(rr(x0, y0, x1, y1, r), fill='#ffffff', stroke=LINE_GREY,
		sw=hairline(cv))
	cv.path(rr(x0, y0, x1, cv.q(y0 + hh), r), fill=ACCENT)
	cols, rows = 4, 2 if not cv.master else 3
	for j in range(rows):
		for i in range(cols):
			cx = x0 + (x1 - x0) * (i + 0.5) / cols
			cy = y0 + hh + (y1 - y0 - hh) * (j + 0.5) / rows
			s = cv.pick(None, cv.u(1), cv.u(2), 3)
			col = T_PDF if (i == 2 and j == 1) else '#b4bccb'
			cv.path(rr(cv.q(cx - s / 2), cv.q(cy - s / 2),
				   cv.q(cx - s / 2) + s, cv.q(cy - s / 2) + s,
				   0 if not cv.master else 0.6), fill=col)


def c_gear(colour):
	def f(cv, box):
		x0, y0, x1, y1 = box
		cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
		if cv.px == 16:
			pixels(cv, ['.#.#.', '#####', '##.##', '#####', '.#.#.'],
			       6, 3, colour)
			return
		h = min(x1 - x0, y1 - y0) / 2 * 0.95
		gear(cv, cx, cy, h, colour, hole=True)
	return f


def gear(cv, cx, cy, h, colour, hole=True, teeth=8):
	"""A gear: round body, rounded teeth, a hole."""
	ro, ri = h, h * 0.74
	pts = []
	steps = teeth * 4
	for i in range(steps):
		a = 2 * math.pi * (i + 0.5) / steps
		r = ro if (i % 4) in (1, 2) else ri
		pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
	d = poly(pts)
	if hole:
		d += circ(cx, cy, h * 0.34)
	cv.path(d, fill=colour, stroke=colour, sw=h * 0.08,
		extra='fill-rule="evenodd"')


def c_cd(cv, box):
	x0, y0, x1, y1 = box
	cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
	if cv.px == 16:
		pixels(cv, ['.###.', '##.##', '#.+.#', '##.##', '.###.'], 6, 3,
		       '#8d97a8')
		return
	r = min(x1 - x0, y1 - y0) / 2
	disc(cv, cx, cy, r)


def c_desktop_entry(cv, box):
	"""A launcher: a small app tile with an arrow."""
	x0, y0, x1, y1 = box
	cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
	if cv.px == 16:
		pixels(cv, ['#####', '#####', '##.##', '#####', '#####'], 6, 3,
		       ACCENT)
		return
	h = min(x1 - x0, y1 - y0) / 2 * 0.9
	cv.path(rr(cx - h, cy - h, cx + h, cy + h, h * 0.35),
		fill=cv.lin(cx - h, cy - h, cx + h, cy + h, '#5b8ff9', ACCENT))
	bg_arrow(cv, cx, cy, h * 0.6)


def c_terminal(cv, box):
	x0, y0, x1, y1 = box
	if cv.px == 16:
		pixels(cv, ['#.....', '.#....', '#..###'], 5, 5, T_XLS)
		return
	h = (y1 - y0)
	sw = cv.pick(None, cv.u(1.2), cv.u(1.6), 2.6)
	cv.path('M%s %sL%s %sL%s %s' %
		(n(x0 + h * 0.05), n(y0 + h * 0.15), n(x0 + h * 0.4),
		 n(y0 + h * 0.45), n(x0 + h * 0.05), n(y0 + h * 0.75)),
		stroke=T_XLS, sw=sw)
	cv.path('M%s %sH%s' % (n(x0 + h * 0.6), n(y0 + h * 0.78),
			       n(x0 + h * 1.15)), stroke=LINE_GREY, sw=sw)


# ---------------------------------------------------------------------------
# Devices and other objects.
# ---------------------------------------------------------------------------
def disc(cv, cx, cy, r):
	d = circ(cx, cy, r) + circ(cx, cy, r * 0.16)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.lin(cx - r, cy - r, cx + r, cy + r,
			       (0, '#e9eef6'), (0.3, '#dbe6fb'), (0.45, '#f6ecf7'),
			       (0.6, '#e2f5ee'), (0.78, '#eef1f6'),
			       (1, '#c7cfdc')),
		extra='fill-rule="evenodd"')
	cv.path(circ(cx, cy, r - hairline(cv) / 2), stroke='#9aa4b5',
		sw=hairline(cv))
	cv.path(circ(cx, cy, r * 0.38) + circ(cx, cy, r * 0.16),
		fill='#ffffff', op=0.7, extra='fill-rule="evenodd"')
	cv.path(circ(cx, cy, r * 0.38), stroke='#aab3c2', sw=hairline(cv) * 0.8)
	cv.path(circ(cx, cy, r * 0.16 + hairline(cv) / 2), stroke='#8d97a8',
		sw=hairline(cv))
	if cv.px != 16:
		# Two sheen wedges.
		for a0 in (-2.2, 0.94):
			a1 = a0 + 0.5
			ro, ri = r * 0.92, r * 0.45
			cv.path('M%s %sA%s %s 0 0 1 %s %sL%s %sA%s %s 0 0 0 %s %sZ' %
				(n(cx + ro * math.cos(a0)), n(cy + ro * math.sin(a0)),
				 n(ro), n(ro), n(cx + ro * math.cos(a1)),
				 n(cy + ro * math.sin(a1)),
				 n(cx + ri * math.cos(a1)), n(cy + ri * math.sin(a1)),
				 n(ri), n(ri), n(cx + ri * math.cos(a0)),
				 n(cy + ri * math.sin(a0))),
				fill='#ffffff', op=0.55)


# ---------------------------------------------------------------------------
# The LikeOS mark: the letters L and O of the name as one square figure.  A
# bold "L" with rounded ends takes two sides of the square; an open ring,
# its stroke about as heavy as the stem and its counter half its width, sits
# in the free corner, flush with the top of the stem and the end of the
# foot, with the same clear gap to both.
# ---------------------------------------------------------------------------
def circ_ccw(cx, cy, r):
	"""A circle drawn the other way round from circ(), to cut a hole in a
	shape under the nonzero fill rule."""
	return ('M%s %sA%s %s 0 1 1 %s %sA%s %s 0 1 1 %s %sZ' %
		(n(cx - r), n(cy), n(r), n(r), n(cx + r), n(cy), n(r), n(r),
		 n(cx - r), n(cy)))


def mark_paths(x0, y0, s, w=None, d=None, t=None):
	"""The L and the ring as two path strings, for a mark filling the
	square x0, y0, s; w is the stem, d the ring's outer diameter, t its
	stroke."""
	w = s * 9 / 48 if w is None else w
	d = s * 33 / 48 if d is None else d
	t = s / 6 if t is None else t
	x1, y1 = x0 + s, y0 + s
	ro = w * 0.9			# the outer corner
	ri = w * 0.25			# the inner corner
	ell_ = ('M%s %sA%s %s 0 0 1 %s %sV%sA%s %s 0 0 0 %s %sH%s'
		'A%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %sZ' %
		(n(x0), n(y0 + w / 2), n(w / 2), n(w / 2), n(x0 + w),
		 n(y0 + w / 2), n(y1 - w - ri), n(ri), n(ri), n(x0 + w + ri),
		 n(y1 - w), n(x1 - w / 2), n(w / 2), n(w / 2), n(x1 - w / 2),
		 n(y1), n(x0 + ro), n(ro), n(ro), n(x0), n(y1 - ro)))
	cx, cy = x1 - d / 2, y0 + d / 2
	return ell_, circ(cx, cy, d / 2) + circ_ccw(cx, cy, d / 2 - t)


# The mark at the snapped sizes, in pixels: (x0, y0, s, stem, ring
# diameter, ring stroke).  Every edge of the L lands on the pixel grid, and
# the ring is centred on a pixel corner, its counter an even number of
# pixels wide.
MARK_PX = {16: (2, 2, 12, 2, 8, 2), 24: (3, 3, 18, 3, 12, 3),
	   32: (4, 4, 24, 4, 16, 4)}


def mark_box(cv):
	"""The mark filling an icon: (x0, y0, s, stem, diameter, stroke) in
	units."""
	if cv.master:
		return 8, 8, 48, 9, 33, 8
	return tuple(cv.u(v) for v in MARK_PX[cv.px])


def mark(cv, x0, y0, s, mode='colour', colour='#ffffff', op=1.0, w=None,
	 d=None, t=None):
	"""mode: 'colour' (blue L, sky-blue ring), 'flat' (one colour), 'tint'
	(one colour, the ring a little lighter than the L)."""
	ell_, ring = mark_paths(x0, y0, s, w, d, t)
	if mode == 'colour':
		if cv.master:
			drop_shadow(cv, ell_ + ring, dy=1.0, sd=1.0, op=0.18)
		cv.path(ell_, fill=cv.lin(x0, y0 + s, x0 + s * 0.6, y0,
					  (0, '#2450c8'), (1, '#4a80f2')))
		cv.path(ring, fill=cv.lin(x0 + s * 0.45, y0 + s * 0.6, x0 + s,
					  y0, (0, '#1f8ff0'), (1, '#6cd4ff')))
		if cv.master:
			# A thin light edge along the top of each shape.
			cv.path(ell_, stroke='#ffffff', sw=0.8, op=0.18)
		return
	if mode == 'tint':
		cv.path(ell_, fill=colour, op=op * 0.8)
		cv.path(ring, fill=colour, op=op)
		return
	cv.path(ell_ + ring, fill=colour, op=op)


def monitor(cv, x0, y0, x1, y1, stand=True, with_mark=True):
	r = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 4)
	sb = cv.pick(cv.u(1), cv.u(1), cv.u(2), 3)
	if stand:
		cx = (x0 + x1) / 2
		nw = cv.pick(cv.u(2), cv.u(3), cv.u(4), 10)
		ny1 = cv.q(y1 + (60 - y1) * 0.55) if cv.master else \
			y1 + cv.pick(cv.u(1), cv.u(2), cv.u(2), 0)
		fy1 = cv.q(58) if cv.master else ny1 + cv.u(1) * \
			cv.pick(1, 1, 2, 1)
		fw = cv.pick(cv.u(6), cv.u(10), cv.u(12), 28)
		cv.path(rr(cx - nw / 2, y1 - 1, cx + nw / 2, ny1 + 0.01, 0),
			fill=cv.vgrad(y1, ny1, '#9ea8b8', '#c3cad6'))
		foot = rr(cv.q(cx - fw / 2), ny1, cv.q(cx + fw / 2), fy1,
			  cv.pick(0, cv.u(1), cv.u(1), 2))
		drop_shadow(cv, foot, dy=0.8, sd=0.8)
		cv.path(foot, fill=cv.vgrad(ny1, fy1, *METAL))
	bezel = rr(x0, y0, x1, y1, r)
	drop_shadow(cv, bezel)
	cv.path(bezel, fill=cv.vgrad(y0, y1, *SLATE_DARK))
	rim(cv, bezel)
	sx0, sy0, sx1, sy1 = x0 + sb, y0 + sb, x1 - sb, y1 - sb
	cv.path(rr(sx0, sy0, sx1, sy1, max(0, r - sb)),
		fill=cv.lin(sx0, sy1, sx1, sy0, *SCREEN))
	if cv.px >= 32 or cv.master:
		# A soft light wash across the screen.
		cv.path('M%s %sC%s %s %s %s %s %sV%sH%sZ' %
			(n(sx0), n(sy0 + (sy1 - sy0) * 0.7),
			 n(sx0 + (sx1 - sx0) * 0.35), n(sy0 + (sy1 - sy0) * 0.45),
			 n(sx0 + (sx1 - sx0) * 0.7), n(sy0 + (sy1 - sy0) * 0.75),
			 n(sx1), n(sy0 + (sy1 - sy0) * 0.5), n(sy1), n(sx0)),
			fill='#ffffff', op=0.14)
	if with_mark and cv.master:
		ms = (sy1 - sy0) * 0.42
		mark(cv, (sx0 + sx1 - ms) / 2, (sy0 + sy1 - ms) / 2, ms,
		     mode='tint', colour='#ffffff', op=0.8)
	if cv.master:
		cv.path(rr(x0 + 0.5, y0 + 0.5, x1 - 0.5, y1 - 0.5, r - 0.5),
			stroke='#ffffff', sw=1, op=0.12)


def d_computer(cv):
	monitor(cv, cv.q(4), cv.q(8), cv.q(60), cv.q(46) if cv.master else
		cv.pick(cv.u(11), cv.u(17), cv.u(23), 0))


def d_laptop(cv):
	x0, x1 = cv.q(10), cv.q(54)
	y0, y1 = cv.q(10), cv.q(44)
	if cv.px == 16:
		x0, x1, y0, y1 = cv.u(3), cv.u(13), cv.u(3), cv.u(11)
	monitor(cv, x0, y0, x1, y1, stand=False)
	by0 = y1
	by1 = by0 + cv.pick(cv.u(2), cv.u(2), cv.u(3), 6)
	ext = cv.pick(cv.u(2), cv.u(3), cv.u(3), 6)
	base = ('M%s %sH%sL%s %sQ%s %s %s %sH%sQ%s %s %s %sZ' %
		(n(x0), n(by0), n(x1), n(x1 + ext), n(by1 - cv.u(1) if not
						      cv.master else by1 - 2),
		 n(x1 + ext), n(by1), n(x1 + ext - (2 if cv.master else
						    cv.u(1))), n(by1),
		 n(x0 - ext + (2 if cv.master else cv.u(1))), n(x0 - ext),
		 n(by1), n(x0 - ext), n(by1 - (2 if cv.master else cv.u(1)))))
	drop_shadow(cv, base)
	cv.path(base, fill=cv.vgrad(by0, by1, '#e9edf3', '#aab3c2'))
	if cv.master:
		cv.path(rr(28, by0, 36, by0 + 2, 1), fill='#8d97a8')


def slab(cv, x0, y0, x1, y1, front_top):
	"""A drive enclosure: light top, dark front plate.  Returns the front
	plate box."""
	r = cv.pick(cv.u(1), cv.u(2), cv.u(2), 5)
	body = rr(x0, y0, x1, y1, r)
	drop_shadow(cv, body)
	cv.path(body, fill=cv.vgrad(y0, front_top, *SLATE_LIGHT))
	front = ('M%s %sH%sV%sA%s %s 0 0 1 %s %sH%sA%s %s 0 0 1 %s %sZ' %
		 (n(x0), n(front_top), n(x1), n(y1 - r), n(r), n(r), n(x1 - r),
		  n(y1), n(x0 + r), n(r), n(r), n(x0), n(y1 - r)))
	cv.path(front, fill=cv.vgrad(front_top, y1, *SLATE_DARK))
	rim(cv, front)
	hw = hairline(cv)
	cv.path('M%s %sH%s' % (n(x0 + (r if cv.master else 0)),
			       n(front_top + hw / 2), n(x1 - (r if cv.master
								 else 0))),
		stroke='#ffffff', sw=hw, op=0.2, cap='butt')
	if cv.master:
		cv.path('M%s %sH%s' % (n(x0 + r), n(y0 + 0.5), n(x1 - r)),
			stroke='#ffffff', sw=1, op=0.9, cap='butt')
	return (x0, front_top, x1, y1)


def rim(cv, d):
	"""A faint light edge around a dark body, so it holds its shape on
	the dark shell surfaces."""
	if cv.master:
		cv.path(d, stroke='#ffffff', sw=1, op=0.16)


def led(cv, cx, cy, colour):
	if cv.master:
		cv.path(circ(cx, cy, 4.5), fill=cv.rad(cx, cy, 4.5,
						       (0, colour, 0.55),
						       (1, colour, 0)))
		cv.path(circ(cx, cy, 2), fill=colour)
		cv.path(circ(cx - 0.5, cy - 0.5, 0.7), fill='#ffffff', op=0.8)
	else:
		s = cv.pick(1, 1, 2, 0)
		cv.path(rr(cv.q(cx) - cv.u(s) / 2 if s == 2 else
			   math.floor(cx * cv.k) / cv.k,
			   math.floor(cy * cv.k) / cv.k,
			   (cv.q(cx) + cv.u(s) / 2 if s == 2 else
			    math.floor(cx * cv.k) / cv.k + cv.u(1)),
			   math.floor(cy * cv.k) / cv.k + cv.u(s), 0),
			fill=colour)


def d_harddisk(cv):
	if cv.px == 16:
		box = slab(cv, cv.u(1), cv.u(4), cv.u(15), cv.u(12), cv.u(7))
	else:
		box = slab(cv, cv.q(4), cv.q(16), cv.q(60), cv.q(48), cv.q(28))
	x0, fy, x1, y1 = box
	led(cv, x1 - cv.pick(cv.u(3), cv.u(4), cv.u(5), 9), (fy + y1) / 2,
	    '#4f8cff')
	if cv.px != 16:
		sw = hairline(cv)
		d = []
		for i in range(cv.pick(0, 1, 2, 3)):
			y = cv.qc(fy + (y1 - fy) * (0.38 + 0.24 * i))
			d.append('M%s %sH%s' % (n(cv.q(x0 + (x1 - x0) * 0.1)), n(y),
						n(cv.q(x0 + (x1 - x0) * 0.42))))
		cv.path(''.join(d), stroke='#8d97a8', sw=sw * (1.6 if cv.master
							       else 1),
			cap='round' if cv.master else 'butt')


def d_optical_drive(cv):
	if cv.px == 16:
		box = slab(cv, cv.u(1), cv.u(4), cv.u(15), cv.u(12), cv.u(7))
	else:
		box = slab(cv, cv.q(4), cv.q(16), cv.q(60), cv.q(48), cv.q(28))
	x0, fy, x1, y1 = box
	# The tray slot.
	hw = hairline(cv)
	sy = cv.qc(fy + (y1 - fy) * 0.3)
	cv.path('M%s %sH%s' % (n(cv.q(x0 + (x1 - x0) * 0.08)), n(sy),
			       n(cv.q(x1 - (x1 - x0) * 0.08))),
		stroke='#11151e', sw=hw * (2 if cv.master else 1), cap='butt')
	if cv.px == 16:
		return
	# Eject symbol.
	ex, ey = x1 - (x1 - x0) * 0.16, fy + (y1 - fy) * 0.66
	s = cv.pick(None, cv.u(2.5), cv.u(3), 5)
	cv.path(poly([(ex - s, ey + s * 0.1), (ex + s, ey + s * 0.1),
		      (ex, ey - s)]), fill='#c9d0dc')
	cv.path(rr(ex - s, ey + s * 0.45, ex + s, ey + s * 0.85, 0),
		fill='#c9d0dc')
	# A disc logo-less ring at the left.
	cv.path(circ(x0 + (x1 - x0) * 0.2, ey, s * 1.1), stroke='#8d97a8',
		sw=hw * (1.4 if cv.master else 1))
	cv.path(circ(x0 + (x1 - x0) * 0.2, ey, s * 0.3), fill='#8d97a8')


def d_media_optical(cv):
	disc(cv, 32, 32, cv.pick(cv.u(7), cv.u(11), cv.u(14.5), 28))


def d_usb(cv):
	# Connector.
	if cv.px == 16:
		cx0, cx1, cy0, cy1 = cv.u(6), cv.u(10), cv.u(1), cv.u(5)
		bx0, bx1, by0, by1 = cv.u(4), cv.u(12), cv.u(5), cv.u(15)
	else:
		cx0, cx1, cy0, cy1 = cv.q(23), cv.q(41), cv.q(4), cv.q(20)
		bx0, bx1, by0, by1 = cv.q(16), cv.q(48), cv.q(18), cv.q(60)
	cv.path(rr(cx0, cy0, cx1, cy1, cv.pick(0, cv.u(1), cv.u(1), 1.5)),
		fill=cv.lin(cx0, 0, cx1, 0, '#c9d0dc', '#f3f5f9', '#aab3c2'))
	if cv.px != 16:
		hs = cv.pick(None, cv.u(2), cv.u(2), 4)
		for t in (0.3, 0.7):
			hx = cv.q(cx0 + (cx1 - cx0) * t - hs / 2)
			cv.path(rr(hx, cv.q(cy0 + (cy1 - cy0) * 0.3), hx + hs,
				   cv.q(cy0 + (cy1 - cy0) * 0.3) + hs, 0),
				fill='#6b7486')
	r = cv.pick(cv.u(1), cv.u(2), cv.u(2), 7)
	body = rr(bx0, by0, bx1, by1, r)
	drop_shadow(cv, body)
	cv.path(body, fill=cv.lin(bx0, by0, bx1, by1, '#5b8ff9', '#2049b8'))
	if cv.master:
		cv.path(rr(bx0 + 0.5, by0 + 0.5, bx1 - 0.5, by1 - 0.5, r - 0.5),
			stroke='#ffffff', sw=1, op=0.25)
	if cv.px != 16:
		# A label recess and an LED.
		lx0, lx1 = bx0 + (bx1 - bx0) * 0.22, bx1 - (bx1 - bx0) * 0.22
		ly0 = by0 + (by1 - by0) * 0.3
		ly1 = by0 + (by1 - by0) * 0.72
		cv.path(rr(cv.q(lx0), cv.q(ly0), cv.q(lx1), cv.q(ly1),
			   cv.pick(0, cv.u(1), cv.u(1), 3)),
			fill='#ffffff', op=0.22)
		if cv.master:
			ms = 11
			mark(cv, 32 - ms / 2, (ly0 + ly1 - ms) / 2, ms,
			     mode='tint', colour='#ffffff', op=0.9)
		led(cv, (bx0 + bx1) / 2, by1 - (by1 - by0) * 0.13, '#9fe6b8')


def d_printer(cv):
	if cv.px == 16:
		pin = (cv.u(4), cv.u(1), cv.u(12), cv.u(6))
		body = (cv.u(1), cv.u(5), cv.u(15), cv.u(12))
		pout = (cv.u(4), cv.u(10), cv.u(12), cv.u(15))
	else:
		pin = (cv.q(16), cv.q(4), cv.q(48), cv.q(26))
		body = (cv.q(4), cv.q(22), cv.q(60), cv.q(48))
		pout = (cv.q(16), cv.q(40), cv.q(48), cv.q(60))
	hw = hairline(cv)

	def sheet(b):
		cv.path(rr(*b, cv.pick(0, 0, 0, 1)), fill=PAGE_EDGE)
		cv.path(rr(b[0] + hw, b[1] + hw, b[2] - hw, b[3] - hw, 0),
			fill='#ffffff')
	sheet(pin)
	r = cv.pick(cv.u(1), cv.u(2), cv.u(2), 6)
	d = rr(*body, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(body[1], body[3], *SLATE_LIGHT))
	cv.path(rr(body[0] + hw / 2, body[1] + hw / 2, body[2] - hw / 2,
		   body[3] - hw / 2, r), stroke=DEVICE_EDGE, sw=hw)
	# Output slot.
	sy0 = body[1] + (body[3] - body[1]) * 0.62
	cv.path(rr(cv.q(body[0] + (body[2] - body[0]) * 0.12), cv.q(sy0),
		   cv.q(body[2] - (body[2] - body[0]) * 0.12),
		   cv.q(sy0) + cv.pick(cv.u(1), cv.u(2), cv.u(2), 4), 0),
		fill='#3a4357')
	sheet(pout)
	if cv.px != 16:
		text_lines(cv, (pout[0] + cv.pick(0, cv.u(2), cv.u(3), 5),
				pout[1] + cv.pick(0, cv.u(3), cv.u(4), 8),
				pout[2] - cv.pick(0, cv.u(2), cv.u(3), 5),
				pout[3] - cv.pick(0, cv.u(1), cv.u(1), 2)),
			   count=3)
		led(cv, body[2] - cv.pick(0, cv.u(3), cv.u(4), 8),
		    body[1] + cv.pick(0, cv.u(3), cv.u(4), 7), '#4f8cff')


def d_keyboard(cv):
	if cv.px == 16:
		b = (cv.u(1), cv.u(4), cv.u(15), cv.u(12))
	else:
		b = (cv.q(2), cv.q(18), cv.q(62), cv.q(48))
	r = cv.pick(cv.u(1), cv.u(2), cv.u(2), 5)
	d = rr(*b, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(b[1], b[3], *SLATE_DARK))
	rim(cv, d)
	if cv.px == 16:
		pixels(cv, ['#.#.#.#.#.#', '.#.#.#.#.#.', '..#######..'], 3, 6,
		       '#c9d0dc')
		return
	cols, rows = 10, 3
	m = cv.pick(None, cv.u(2), cv.u(2), 4)
	gx0, gy0 = b[0] + m, b[1] + m
	gx1, gy1 = b[2] - m, b[3] - m
	kw = (gx1 - gx0) / cols
	kh = (gy1 - gy0) / (rows + 1)
	kg = cv.pick(None, cv.u(1), cv.u(1), 1.2)
	for j in range(rows + 1):
		y0 = gy0 + j * kh
		if j < rows:
			off = (kw / 2) * (j % 2)
			for i in range(cols if j % 2 == 0 else cols - 1):
				x0 = gx0 + off + i * kw
				cv.path(rr(cv.q(x0), cv.q(y0), cv.q(x0 + kw - kg),
					   cv.q(y0 + kh - kg),
					   0 if not cv.master else 1),
					fill='#e9edf3' if cv.master else '#c9d0dc')
		else:
			cv.path(rr(cv.q(gx0 + kw * 2.5), cv.q(y0),
				   cv.q(gx1 - kw * 2.5), cv.q(y0 + kh - kg),
				   0 if not cv.master else 1),
				fill='#e9edf3' if cv.master else '#c9d0dc')


def d_mouse(cv):
	if cv.px == 16:
		b = (cv.u(4), cv.u(1), cv.u(12), cv.u(15))
	else:
		b = (cv.q(16), cv.q(4), cv.q(48), cv.q(60))
	r = (b[2] - b[0]) / 2
	d = rr(*b, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.lin(b[0], b[1], b[2], b[3], '#ffffff', '#c9d0dc'))
	hw = hairline(cv)
	cv.path(rr(b[0] + hw / 2, b[1] + hw / 2, b[2] - hw / 2, b[3] - hw / 2,
		   r - hw / 2), stroke=DEVICE_EDGE, sw=hw)
	cx = cv.qc((b[0] + b[2]) / 2)
	sy = b[1] + (b[3] - b[1]) * 0.4
	cv.path('M%s %sV%sM%s %sH%s' % (n(cx), n(b[1]), n(cv.qc(sy)),
					n(b[0]), n(cv.qc(sy)), n(b[2])),
		stroke=DEVICE_EDGE, sw=hw, cap='butt')
	ww = cv.pick(cv.u(1), cv.u(2), cv.u(2), 5)
	cv.path(rr(cv.q((b[0] + b[2]) / 2 - ww / 2), cv.q(b[1] + (b[3] - b[1]) *
							  0.1),
		   cv.q((b[0] + b[2]) / 2 - ww / 2) + ww,
		   cv.q(b[1] + (b[3] - b[1]) * 0.28), ww / 2 if cv.master
		   else 0), fill=ACCENT_HI)


def d_speakers(cv):
	if cv.px == 16:
		b = (cv.u(3), cv.u(1), cv.u(13), cv.u(15))
	else:
		b = (cv.q(12), cv.q(4), cv.q(52), cv.q(60))
	r = cv.pick(cv.u(1), cv.u(2), cv.u(2), 6)
	d = rr(*b, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(b[1], b[3], *SLATE_DARK))
	rim(cv, d)
	cx = (b[0] + b[2]) / 2
	w = b[2] - b[0]

	def cone(cy, rad):
		cv.path(circ(cx, cy, rad), fill=cv.rad(cx, cy - rad * 0.3, rad,
						       '#c9d0dc', '#6b7486'))
		if cv.px != 16:
			cv.path(circ(cx, cy, rad * 0.45),
				fill=cv.rad(cx, cy - rad * 0.1, rad * 0.45,
					    '#3a4357', '#1b2130'))
			cv.path(circ(cx, cy, rad * 0.15), fill='#8d97a8')
	cone(b[1] + (b[3] - b[1]) * 0.24, w * 0.17)
	cone(b[1] + (b[3] - b[1]) * 0.66, w * 0.33)


def d_headphones(cv):
	hw = hairline(cv)
	if cv.px == 16:
		band_sw = cv.u(2)
	else:
		band_sw = cv.pick(None, cv.u(2), cv.u(3), 5)
	cv.path('M%s %sV%sA%s %s 0 0 1 %s %sV%s' %
		(n(cv.q(12) + band_sw / 2), n(cv.q(40)), n(32), n(20 - band_sw / 2
								  + 0.01),
		 n(20 - band_sw / 2), n(cv.q(52) - band_sw / 2), n(32),
		 n(cv.q(40))),
		stroke=cv.vgrad(12, 40, '#7d889c', '#4f5b72'), sw=band_sw,
		cap='butt')
	for x0, x1 in ((cv.q(6), cv.q(22)), (cv.q(42), cv.q(58))):
		d = rr(x0, cv.q(32), x1, cv.q(58), cv.pick(cv.u(1), cv.u(2),
							   cv.u(3), 7))
		drop_shadow(cv, d)
		cv.path(d, fill=cv.lin(x0, 32, x1, 58, '#5b8ff9', '#2049b8'))
		if cv.master:
			cv.path(rr(x0 + 0.5, 32.5, x1 - 0.5, 57.5, 6.5),
				stroke='#ffffff', sw=1, op=0.25)


def d_camera(cv):
	if cv.px == 16:
		body = (cv.u(1), cv.u(4), cv.u(15), cv.u(14))
		bump = (cv.u(4), cv.u(2), cv.u(8), cv.u(5))
	else:
		body = (cv.q(4), cv.q(16), cv.q(60), cv.q(54))
		bump = (cv.q(14), cv.q(9), cv.q(30), cv.q(18))
	r = cv.pick(cv.u(1), cv.u(2), cv.u(3), 7)
	cv.path(rr(*bump, cv.pick(0, cv.u(1), cv.u(1), 3)),
		fill='#3a4357')
	d = rr(*body, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(body[1], body[3], *SLATE_DARK))
	rim(cv, d)
	cx = 32
	cy = (body[1] + body[3]) / 2 + cv.pick(0, cv.u(0.5), 0, 1)
	R = (body[3] - body[1]) * 0.36
	cv.path(circ(cx, cy, R), fill=cv.vgrad(cy - R, cy + R, '#e3e8ef',
					       '#8d97a8'))
	cv.path(circ(cx, cy, R * 0.78), fill='#161c2a')
	cv.path(circ(cx, cy, R * 0.58), fill=cv.lin(cx - R, cy + R, cx + R,
						    cy - R, *SCREEN))
	if cv.px != 16:
		cv.path(circ(cx - R * 0.22, cy - R * 0.22, R * 0.16),
			fill='#ffffff', op=0.85)
		fx = body[2] - (body[2] - body[0]) * 0.14
		fy = body[1] + (body[3] - body[1]) * 0.18
		fs = cv.pick(None, cv.u(2), cv.u(2), 4)
		cv.path(rr(cv.q(fx - fs), cv.q(fy), cv.q(fx), cv.q(fy) + fs * 0.75,
			   0 if not cv.master else 1), fill='#ffd166')


def d_phone(cv):
	if cv.px == 16:
		b = (cv.u(4), cv.u(1), cv.u(12), cv.u(15))
	else:
		b = (cv.q(16), cv.q(4), cv.q(48), cv.q(60))
	r = cv.pick(cv.u(1), cv.u(2), cv.u(3), 7)
	d = rr(*b, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(b[1], b[3], *SLATE_DARK))
	rim(cv, d)
	m = cv.pick(cv.u(1), cv.u(1), cv.u(2), 3)
	s = (b[0] + m, b[1] + m * 2, b[2] - m, b[3] - m * 2)
	cv.path(rr(*s, max(0, r - m)), fill=cv.lin(s[0], s[3], s[2], s[1],
						   *SCREEN))
	if cv.master:
		mark(cv, 26, 26, 12, mode='tint', colour='#ffffff', op=0.8)
		cv.path('M28 6.8H36', stroke='#8d97a8', sw=1.4)


def d_server(cv):
	units = 3
	if cv.px == 16:
		ys = [(cv.u(1), cv.u(5)), (cv.u(6), cv.u(10)), (cv.u(11),
								 cv.u(15))]
		x0, x1 = cv.u(2), cv.u(14)
	else:
		ys = [(cv.q(4), cv.q(20)), (cv.q(24), cv.q(40)),
		      (cv.q(44), cv.q(60))]
		x0, x1 = cv.q(8), cv.q(56)
	r = cv.pick(cv.u(1), cv.u(1), cv.u(2), 4)
	for i in range(units):
		y0, y1 = ys[i]
		d = rr(x0, y0, x1, y1, r)
		drop_shadow(cv, d, dy=0.8, sd=0.8)
		cv.path(d, fill=cv.vgrad(y0, y1, *SLATE_DARK))
		rim(cv, d)
		if cv.master:
			cv.path('M%s %sH%s' % (n(x0 + r), n(y0 + 0.5), n(x1 - r)),
				stroke='#ffffff', sw=1, op=0.2, cap='butt')
		cy = (y0 + y1) / 2
		led(cv, x1 - cv.pick(cv.u(2), cv.u(3), cv.u(4), 8), cy - (
			cv.u(0.5) if cv.px == 24 else 0),
		    '#46d17f' if i != 1 else '#4f8cff')
		if cv.px != 16:
			d = []
			for t in range(cv.pick(0, 2, 3, 4)):
				x = cv.qc(x0 + (x1 - x0) * (0.12 + t * 0.07))
				d.append('M%s %sV%s' % (n(x), n(cv.q(y0 + (y1 - y0)
								   * 0.3)),
							 n(cv.q(y1 - (y1 - y0) *
								0.3))))
			cv.path(''.join(d), stroke='#8d97a8',
				sw=hairline(cv) * (1.4 if cv.master else 1),
				cap='round' if cv.master else 'butt')


def globe(cv, cx, cy, r):
	"""A blue sphere with a meridian, the equator and the axis in white."""
	if cv.px == 16:
		# Whole pixels: centre on a pixel so the lines can be one wide.
		cx = cy = (math.floor(cx * cv.k) + 0.5) / cv.k
		r = round(r * cv.k) / cv.k
	d = circ(cx, cy, r)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.rad(cx - r * 0.35, cy - r * 0.45, r * 1.5,
			       '#9cc8ff', '#3b74f0', '#1a3fa8'))
	op = 0.9
	if not cv.master:
		# One-pixel lines, clipped to the sphere.
		clip = cv.clip(d)
		cv.add('<g clip-path="%s" opacity="%s">' % (clip, n(op)))
		px = lambda v: math.floor(v * cv.k) / cv.k
		one = cv.u(1)
		ex = round(r * 0.5 * cv.k) / cv.k
		x = px(cx)
		y = px(cy)
		segs = [rr(cx - r, y, cx + r, y + one, 0),
			rr(x, cy - r, x + one, cy + r, 0)]
		cv.path(''.join(segs), fill='#ffffff')
		cv.path(ell(x + one / 2, cy, ex, r - one / 2), stroke='#ffffff',
			sw=one)
		cv.add('</g>')
		return
	sw = max(2.0, r * 0.075)
	cv.path(ell(cx, cy, r * 0.46, r - sw / 2), stroke='#ffffff', sw=sw,
		op=op)
	cv.path('M%s %sV%sM%s %sH%s' % (n(cx), n(cy - r + sw / 2),
				       n(cy + r - sw / 2), n(cx - r + sw / 2),
				       n(cy), n(cx + r - sw / 2)),
		stroke='#ffffff', sw=sw, op=op, cap='butt')
	for t in (-0.52, 0.52):
		y = cy + t * r
		hw = math.sqrt(r * r - (t * r) ** 2) - sw * 0.8
		cv.path('M%s %sH%s' % (n(cx - hw), n(y), n(cx + hw)),
			stroke='#ffffff', sw=sw * 0.8, op=op * 0.75)
	# Rim light and a highlight on the upper left.
	cv.path(circ(cx, cy, r - 0.6), stroke='#ffffff', sw=1.2, op=0.25)
	cv.path(ell(cx - r * 0.38, cy - r * 0.46, r * 0.3, r * 0.17),
		fill='#ffffff', op=0.28,
		extra='transform="rotate(-35 %s %s)"' % (n(cx - r * 0.38),
							n(cy - r * 0.46)))


def d_network(cv):
	globe(cv, 32, 32, cv.pick(cv.u(7), cv.u(11), cv.u(14), 28))


def p_home(cv):
	"""The Home page: a house."""
	if cv.px == 16:
		roof = poly([(cv.u(0.5), cv.u(8)), (cv.u(8), cv.u(1)),
			     (cv.u(15.5), cv.u(8))])
		body = (cv.u(3), cv.u(7), cv.u(13), cv.u(15))
		door = (cv.u(7), cv.u(10), cv.u(9), cv.u(15))
	else:
		roof = poly([(cv.q(2), cv.q(32)), (32, cv.q(4)),
			     (cv.q(62), cv.q(32))])
		body = (cv.q(10), cv.q(26), cv.q(54), cv.q(60))
		door = (cv.q(26), cv.q(40), cv.q(38), cv.q(60))
	hw = hairline(cv)
	r = cv.pick(0, cv.u(1), cv.u(1), 2.5)
	d = rr(*body, r)
	drop_shadow(cv, d)
	cv.path(d, fill=PAGE_EDGE)
	cv.path(rr(body[0] + hw, body[1], body[2] - hw, body[3] - hw,
		   max(0, r - hw)),
		fill=cv.vgrad(body[1], body[3], '#ffffff', '#e8edf5'))
	if cv.master:
		cv.path(rr(40, 8, 48, 24, 1.5), fill='#1f4fc9')
	cv.path(roof, fill=cv.vgrad(4, 32, '#5b8ff9', ACCENT),
		stroke=cv.vgrad(4, 32, '#5b8ff9', ACCENT),
		sw=cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 5))
	cv.path(rr(*door, cv.pick(0, 0, cv.u(1), 2)), fill=cv.vgrad(
		door[1], door[3], '#4a82f0', ACCENT))
	if cv.master:
		cv.path('M6 31.5L32 7.5L58 31.5', stroke='#ffffff', sw=1.2,
			op=0.35)
		cv.path(circ(35, 51, 1), fill='#ffffff', op=0.9)


def p_gallery(cv):
	"""The Gallery: two photos, one turned behind the other."""
	if cv.px == 16:
		back = (cv.u(3), cv.u(1), cv.u(15), cv.u(10))
		front = (cv.u(1), cv.u(5), cv.u(13), cv.u(15))
	else:
		back = (cv.q(16), cv.q(4), cv.q(60), cv.q(40))
		front = (cv.q(4), cv.q(20), cv.q(48), cv.q(60))
	hw = hairline(cv)
	m = cv.pick(cv.u(1), cv.u(2), cv.u(2), 3)

	def photo(b, tilt, pic):
		g = ('<g transform="rotate(%s %s %s)">' %
		     (n(tilt), n((b[0] + b[2]) / 2), n((b[1] + b[3]) / 2))
		     if tilt else '<g>')
		cv.add(g)
		d = rr(*b, cv.pick(0, cv.u(1), cv.u(1), 2.5))
		drop_shadow(cv, d)
		cv.path(d, fill='#ffffff', stroke=PAGE_EDGE, sw=hw)
		c_picture(cv, (b[0] + m, b[1] + m, b[2] - m, b[3] - m),
			  on_page=False) if pic else cv.path(rr(b[0] + m, b[1] + m, b[2] - m,
					       b[3] - m, 0),
					    fill=cv.vgrad(b[1], b[3], '#c9dcff',
							  '#7fa6f0'))
		cv.add('</g>')
	photo(back, 8 if cv.master else 0, False)
	photo(front, 0, True)


def p_star(cv):
	pts = []
	cx, cy = 32, 33.5
	R = cv.pick(cv.u(7.5), cv.u(11.5), cv.u(15), 28)
	for i in range(10):
		a = -math.pi / 2 + i * math.pi / 5
		r = R if i % 2 == 0 else R * 0.47
		pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
	d = poly(pts)
	drop_shadow(cv, d)
	sw = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 4)
	cv.path(d, fill=cv.vgrad(cy - R, cy + R, '#ffd86b', '#f2a516'),
		stroke=cv.vgrad(cy - R, cy + R, '#f7c040', '#e08d00'), sw=sw)
	if cv.master:
		cv.path(poly(pts[:3] + [(cx, cy)]), fill='#ffffff', op=0.18)


def p_recent(cv):
	cx, cy = 32, 32
	R = cv.pick(cv.u(7), cv.u(11), cv.u(14.5), 28)
	d = circ(cx, cy, R)
	drop_shadow(cv, d)
	ring = cv.pick(cv.u(1.5), cv.u(2), cv.u(2.5), 5)
	cv.path(d, fill=cv.lin(cx, cy - R, cx, cy + R, '#5b8ff9', ACCENT))
	cv.path(circ(cx, cy, R - ring), fill=cv.vgrad(cy - R, cy + R,
						      '#ffffff', '#e8edf5'))
	sw = cv.pick(cv.u(1), cv.u(1.5), cv.u(2), 3.4)
	if cv.master or cv.px >= 24:
		for i in range(12):
			a = i * math.pi / 6
			if not cv.master and i % 3:
				continue
			r0, r1 = R - ring - (5 if cv.master else cv.u(2)), \
				R - ring - (2 if cv.master else cv.u(1))
			cv.path('M%s %sL%s %s' % (n(cx + r0 * math.cos(a)),
						  n(cy + r0 * math.sin(a)),
						  n(cx + r1 * math.cos(a)),
						  n(cy + r1 * math.sin(a))),
				stroke='#9aa4b5', sw=1.6 if cv.master and
				i % 3 else sw * 0.7)
	cv.path('M%s %sV%sL%s %s' % (n(cx), n(cy - (R - ring) * 0.62), n(cy),
				     n(cx + (R - ring) * 0.42),
				     n(cy + (R - ring) * 0.3)),
		stroke=INK, sw=sw)
	cv.path(circ(cx, cy, sw * 0.7), fill=ACCENT_HI)


def p_trash(full):
	def draw(cv):
		if cv.px == 16:
			body = poly([(cv.u(3.5), cv.u(5)), (cv.u(12.5), cv.u(5)),
				     (cv.u(12), cv.u(15)), (cv.u(4), cv.u(15))])
			lid = (cv.u(2), cv.u(3), cv.u(14), cv.u(5))
			handle = (cv.u(6), cv.u(1), cv.u(10), cv.u(3))
		else:
			body = poly([(cv.q(13), cv.q(20)), (cv.q(51), cv.q(20)),
				     (cv.q(47), cv.q(60)), (cv.q(17), cv.q(60))])
			lid = (cv.q(9), cv.q(13), cv.q(55), cv.q(20))
			handle = (cv.q(24), cv.q(6), cv.q(40), cv.q(14))
		sw = cv.pick(0, cv.u(1), cv.u(1.5), 2)
		drop_shadow(cv, body)
		cv.path(body, fill=cv.lin(13, 20, 51, 60, '#8e9ab0', '#5a667d'),
			stroke=cv.lin(13, 20, 51, 60, '#8e9ab0', '#5a667d'),
			sw=sw or None) if sw else \
			cv.path(body, fill='#76839a')
		# Ribs.
		if cv.px != 16:
			d = []
			for t in (0.3, 0.5, 0.7):
				x = cv.qc(13 + 38 * t, 1 if not cv.master else 1)
				d.append('M%s %sV%s' % (n(x), n(cv.q(27)),
							 n(cv.q(53))))
			cv.path(''.join(d), stroke='#c3cad6',
				sw=hairline(cv) * (2.4 if cv.master else 1),
				cap='round' if cv.master else 'butt', op=0.75)
		else:
			pixels(cv, ['#.#.#', '#.#.#', '#.#.#', '#.#.#', '#.#.#'],
			       5, 8, '#c3cad6', 0.7)
		if full:
			# Paper above the rim.
			if cv.px == 16:
				pixels(cv, ['.###.', '#####'], 5, 1, '#ffffff')
			else:
				cv.path(poly([(cv.q(18), cv.q(20)),
					       (cv.q(20), cv.q(5)),
					       (cv.q(34), cv.q(3)),
					       (cv.q(36), cv.q(20))]),
					fill='#ffffff', stroke=PAGE_EDGE,
					sw=hairline(cv))
				cv.path(poly([(cv.q(30), cv.q(20)),
					       (cv.q(37), cv.q(7)),
					       (cv.q(50), cv.q(12)),
					       (cv.q(46), cv.q(20))]),
					fill='#eef2f8', stroke=PAGE_EDGE,
					sw=hairline(cv))
				if cv.master:
					text_lines(cv, (23, 9, 32, 17), count=2)
		lr = cv.pick(0, cv.u(1), cv.u(1), 3)
		lid_g = ''
		if full:
			lid_g = 'transform="rotate(%s %s %s)"' % (
				-9 if cv.master else (-8 if cv.px >= 24 else 0),
				n(lid[2]), n(lid[3]))
		cv.add('<g %s>' % lid_g)
		cv.path(rr(*handle, cv.pick(0, cv.u(1), cv.u(1), 3)),
			stroke='#5a667d', sw=cv.pick(cv.u(1), cv.u(1.5), cv.u(2),
						     3.2),
			extra='') if cv.px != 16 else \
			cv.path(rr(*handle, 0), fill='#5a667d')
		d = rr(*lid, lr)
		cv.path(d, fill=cv.vgrad(lid[1], lid[3], '#7d8aa1', '#4f5b72'))
		if cv.master:
			cv.path('M%s %sH%s' % (n(lid[0] + lr), n(lid[1] + 0.5),
					       n(lid[2] - lr)),
				stroke='#ffffff', sw=1, op=0.35, cap='butt')
		cv.add('</g>')
	return draw


# ---------------------------------------------------------------------------
# Dialog symbols.
# ---------------------------------------------------------------------------
def s_info(cv):
	R = cv.pick(cv.u(7.5), cv.u(11.5), cv.u(15), 29)
	d = circ(32, 32, R)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(32 - R, 32 + R, '#4f86f3', '#2049b8'))
	if cv.px == 16:
		pixels(cv, ['##', '..', '##', '##', '##', '##'], 7, 4, '#ffffff')
		return
	w = R * 0.22
	cv.path(circ(32, 32 - R * 0.47, w * 0.62), fill='#ffffff')
	cv.path('M32 %sV%s' % (n(32 - R * 0.12), n(32 + R * 0.5)),
		stroke='#ffffff', sw=w)


def s_warning(cv):
	if cv.px == 16:
		tri = poly([(cv.u(8), cv.u(1)), (cv.u(15.5), cv.u(14.5)),
			    (cv.u(0.5), cv.u(14.5))])
	else:
		tri = poly([(32, cv.q(6) + 2), (cv.q(61) - 2.5, cv.q(56)),
			    (cv.q(3) + 2.5, cv.q(56))])
	sw = cv.pick(cv.u(1), cv.u(2), cv.u(2), 7)
	drop_shadow(cv, tri)
	cv.path(tri, fill=cv.vgrad(6, 58, '#f5b434', '#d98a00'),
		stroke=cv.vgrad(6, 58, '#f5b434', '#d98a00'), sw=sw)
	if cv.px == 16:
		pixels(cv, ['##', '##', '##', '..', '##'], 7, 6, '#ffffff')
		return
	w = cv.pick(None, cv.u(2), cv.u(2.5), 5.2)
	cv.path('M32 %sV%s' % (n(cv.q(24)), n(cv.q(40))), stroke='#ffffff',
		sw=w)
	cv.path(circ(32, cv.q(48), w * 0.62), fill='#ffffff')


def s_error(cv):
	# A rounded octagon.
	R = cv.pick(cv.u(7.6), cv.u(11.5), cv.u(15.2), 29)
	pts = [(32 + R * math.cos(math.pi / 8 + i * math.pi / 4),
		32 + R * math.sin(math.pi / 8 + i * math.pi / 4)) for i in range(8)]
	d = poly(pts)
	sw = cv.pick(cv.u(0.5), cv.u(1), cv.u(1.5), 4)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(32 - R, 32 + R, '#ea5a5a', '#c42e2e'),
		stroke=cv.vgrad(32 - R, 32 + R, '#ea5a5a', '#c42e2e'), sw=sw)
	if cv.px == 16:
		pixels(cv, ['#....#', '.#..#.', '..##..', '..##..', '.#..#.',
			    '#....#'], 5, 5, '#ffffff')
		return
	a = R * 0.33
	w = cv.pick(None, cv.u(2), cv.u(2.5), 5)
	cv.path('M%s %sL%s %sM%s %sL%s %s' % (n(32 - a), n(32 - a), n(32 + a),
					      n(32 + a), n(32 + a), n(32 - a),
					      n(32 - a), n(32 + a)),
		stroke='#ffffff', sw=w)


def s_question(cv):
	R = cv.pick(cv.u(7.5), cv.u(11.5), cv.u(15), 29)
	d = circ(32, 32, R)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(32 - R, 32 + R, '#8c66ea', '#5b3fc4'))
	if cv.px == 16:
		pixels(cv, ['.###.', '#...#', '...#.', '..#..', '.....',
			    '..#..'], 6, 4, '#ffffff')
		return
	w = cv.pick(None, cv.u(2), cv.u(2.5), 5)
	s = R / 29
	cv.path('M%s %sC%s %s %s %s %s %sC%s %s %s %s %s %sV%s' %
		(n(32 - 8 * s), n(32 - 9 * s), n(32 - 8 * s), n(32 - 15 * s),
		 n(32 + 8 * s), n(32 - 16 * s), n(32 + 8 * s), n(32 - 8 * s),
		 n(32 + 8 * s), n(32 - 3 * s), n(32), n(32 - 3 * s), n(32),
		 n(32 + 3 * s), n(32 + 6 * s)),
		stroke='#ffffff', sw=w)
	cv.path(circ(32, 32 + 15 * s, w * 0.62), fill='#ffffff')


# ---------------------------------------------------------------------------
# Applications: a rounded tile with a white glyph, or an object.
# ---------------------------------------------------------------------------
def tile(cv, c1, c2, light=False):
	"""The app tile.  Returns its box."""
	x0 = cv.pick(cv.u(1), cv.u(2), cv.u(2), 4)
	x1 = cv.pick(cv.u(15), cv.u(22), cv.u(30), 60)
	r = cv.pick(cv.u(3), cv.u(4.5), cv.u(6), 13)
	d = rr(x0, x0, x1, x1, r)
	drop_shadow(cv, d, dy=1.4, sd=1.3, op=0.28)
	cv.path(d, fill=cv.lin(x0, x0, x1 * 0.6, x1, c1, c2))
	hw = hairline(cv)
	if cv.master:
		cv.path(rr(x0 + 0.5, x0 + 0.5, x1 - 0.5, x1 - 0.5, r - 0.5),
			stroke=cv.vgrad(x0, x1, (0, '#ffffff', 0.45),
					(0.5, '#ffffff', 0.08),
					(1, '#000000', 0.12)), sw=1)
		# A soft sheen across the top half.
		cv.path('M%s %sH%sA%s %s 0 0 1 %s %sV%sC%s %s %s %s %s %sV%s'
			'A%s %s 0 0 1 %s %sZ' %
			(n(x0 + r), n(x0), n(x1 - r), n(r), n(r), n(x1),
			 n(x0 + r), n(26), n(44), n(22), n(20), n(32), n(x0),
			 n(30), n(x0 + r), n(r), n(r), n(x0 + r), n(x0)),
			fill='#ffffff', op=0.08)
	elif light:
		cv.path(rr(x0 + hw / 2, x0 + hw / 2, x1 - hw / 2, x1 - hw / 2,
			   r - hw / 2), stroke='#b9c1ce', sw=hw)
	if light and cv.master:
		cv.path(rr(x0 + 0.5, x0 + 0.5, x1 - 0.5, x1 - 0.5, r - 0.5),
			stroke='#9aa4b5', sw=1, op=0.6)
	return (x0, x0, x1, x1)


def tile_pixels(cv, rows, colour='#ffffff', op=1.0):
	"""Pixel art centred on the 16-pixel tile."""
	place_pixels(cv, rows, (cv.u(1), cv.u(1), cv.u(15), cv.u(15)), colour,
		     op)


def a_files(cv):
	"""LikeOS Explorer: a white tile holding a small blue folder."""
	b = tile(cv, '#ffffff', '#dfe6f1', light=True)
	if cv.px == 16:
		tile_pixels(cv, ['###.......', '##########', '++++++++++',
				 '##########', '##########', '##########'],
			    ACCENT)
		return
	# The folder, scaled into the tile.
	s = 0.62
	ox = 32 - 32 * s
	oy = 33 - 31 * s
	cv.add('<g transform="translate(%s %s) scale(%s)">' % (n(ox), n(oy),
							       n(s)))
	sub = Cv(None)
	sub.ids = cv.ids + 100
	folder_shape(sub)
	cv.ids = sub.ids
	cv.defs.extend(sub.defs)
	cv.add('\n'.join(sub.body))
	cv.add('</g>')


def a_terminal(cv):
	b = tile(cv, '#3a4459', '#141a26')
	x0, y0, x1, y1 = b
	if cv.px == 16:
		tile_pixels(cv, ['#......', '.#.....', '..#....', '.#.....',
				 '#..####'])
		return
	sw = cv.pick(None, cv.u(1.5), cv.u(2), 4.4)
	cv.path('M%s %sL%s %sL%s %s' % (n(cv.q(16)), n(cv.q(22)), n(cv.q(28)),
					n(32), n(cv.q(16)), n(cv.q(42))),
		stroke='#ffffff', sw=sw)
	cv.path('M%s %sH%s' % (n(cv.q(32)), n(cv.qc(42, 2) if not cv.master
					     else 42), n(cv.q(48))),
		stroke='#5b8ff9', sw=sw, cap='round' if cv.master else 'butt')


def a_calculator(cv):
	tile(cv, '#ff9a57', '#d9532a')
	if cv.px == 16:
		tile_pixels(cv, ['########', '########', '........', '##.##.##',
				 '##.##.##', '........', '##.##.##', '##.##.##'])
		return
	# Display and keys.
	m = 16
	cv.path(rr(cv.q(m), cv.q(14), cv.q(64 - m), cv.q(24),
		   cv.pick(0, cv.u(1), cv.u(1), 2.5)), fill='#ffffff', op=0.95)
	ks = cv.pick(None, cv.u(3), cv.u(4), 8)
	gap = (64 - 2 * m - 3 * ks) / 2
	for j in range(2):
		for i in range(3):
			x = m + i * (ks + gap)
			y = 30 + j * (ks + gap)
			if i == 2 and j == 0:
				# The tall "=" key spans both rows.
				cv.path(rr(cv.q(x), cv.q(y), cv.q(x) + ks,
					   cv.q(y + ks * 2 + gap),
					   cv.pick(0, cv.u(1), cv.u(1), 2.5)),
					fill='#ffffff')
				continue
			if i == 2:
				continue
			cv.path(rr(cv.q(x), cv.q(y), cv.q(x) + ks, cv.q(y) + ks,
				   cv.pick(0, cv.u(1), cv.u(1), 2.5)),
				fill='#ffffff', op=0.75)


def a_text_editor(cv):
	tile(cv, '#3fc283', '#16834a')
	if cv.px == 16:
		tile_pixels(cv, ['######', '#....#', '#.##.#', '#....#',
				 '#.##.#', '#....#', '#.#..#', '######'])
		return
	# A page with lines and, where there is room, a pencil across it.
	pen = cv.px in (32, 64)
	px0, px1 = (cv.q(14), cv.q(42)) if pen else (cv.q(18), cv.q(46))
	py0, py1 = cv.q(12), cv.q(52)
	cv.path(rr(px0, py0, px1, py1, cv.pick(0, cv.u(1), cv.u(1), 3)),
		fill='#ffffff')
	text_lines(cv, (px0 + cv.pick(0, cv.u(2), cv.u(3), 6),
			py0 + cv.pick(0, cv.u(3), cv.u(4), 8),
			px1 - cv.pick(0, cv.u(2), cv.u(4), 9),
			py1 - cv.pick(0, cv.u(2), cv.u(3), 6)),
		   colour='#93cfae')
	if not pen:
		return
	cv.add('<g transform="rotate(38 46 34)">')
	if cv.master:
		cv.path(rr(41, 14, 51, 46, 0), fill='#0b3a20', op=0.25,
			extra='transform="translate(1.2 1.2)" filter="%s"' %
			cv.blur(1.0))
		cv.path(rr(41, 6, 51, 13, 3) , fill='#f28b9b')
		cv.path(rr(41, 12, 51, 17, 0), fill=cv.lin(41, 0, 51, 0,
							   '#e3e8ef', '#9aa4b5'))
		cv.path(rr(41, 17, 51, 46, 0), fill=cv.lin(41, 0, 51, 0,
							   '#ffd86b', '#f0a020'))
		cv.path('M44.3 17V46', stroke='#ffffff', sw=1, op=0.45)
		cv.path(poly([(41, 46), (51, 46), (46, 57)]), fill='#f6dcb3')
		cv.path(poly([(44.2, 53), (47.8, 53), (46, 57)]),
			fill='#3a4357')
	else:
		cv.path(rr(42, 10, 50, 44, 0), fill='#ffffff')
		cv.path(rr(42, 15, 50, 17, 0), fill='#16834a')
		cv.path(poly([(42, 44), (50, 44), (46, 53)]), fill='#ffffff')
	cv.add('</g>')


def a_mail(cv):
	tile(cv, '#5cb0ff', '#2265d8')
	if cv.px == 16:
		tile_pixels(cv, ['##########', '#+......+#', '#.#....#.#',
				 '#..#..#..#', '#...##...#', '##########'])
		return
	x0, y0, x1, y1 = cv.q(13), cv.q(19), cv.q(51), cv.q(45)
	cv.path(rr(x0, y0, x1, y1, cv.pick(0, cv.u(1), cv.u(1.5), 3)),
		fill='#ffffff')
	sw = cv.pick(None, cv.u(1), cv.u(1.5), 2.6)
	cv.path('M%s %sL32 %sL%s %s' % (n(x0 + sw), n(y0 + sw * 1.2),
					n(y0 + (y1 - y0) * 0.58), n(x1 - sw),
					n(y0 + sw * 1.2)),
		stroke='#2265d8', sw=sw)


def a_media_player(cv):
	tile(cv, '#7a5cf0', '#2f3fb8')
	if cv.px == 16:
		tile_pixels(cv, ['..####..', '.######.', '###.####', '###..###',
				 '###..###', '###.####', '.######.', '..####..'])
		return
	R = cv.pick(None, cv.u(7), cv.u(9.5), 19)
	sw = cv.pick(None, cv.u(1.5), cv.u(2), 3.6)
	cv.path(circ(32, 32, R), fill='#ffffff', op=0.18)
	cv.path(circ(32, 32, R - sw / 2), stroke='#ffffff', sw=sw)
	h = R * 0.5
	cv.path(poly([(32 - h * 0.6, 32 - h), (32 + h * 1.0, 32),
		       (32 - h * 0.6, 32 + h)]), fill='#ffffff',
		stroke='#ffffff', sw=sw * 0.8)


def a_image_viewer(cv):
	tile(cv, '#33c6d6', '#127f97')
	if cv.px == 16:
		tile_pixels(cv, ['########', '#....#.#', '#......#', '#.#....#',
				 '####.#.#', '########'])
		return
	x0, y0, x1, y1 = cv.q(13), cv.q(16), cv.q(51), cv.q(48)
	sw = cv.pick(None, cv.u(1.5), cv.u(2), 3.6)
	cv.path(rr(x0 + sw / 2, y0 + sw / 2, x1 - sw / 2, y1 - sw / 2,
		   cv.pick(0, cv.u(1), cv.u(1.5), 3)), stroke='#ffffff', sw=sw)
	cv.path(poly([(x0 + sw, y1 - sw), (x0 + sw, 40), (24, 29), (34, 39),
		       (39, 34), (x1 - sw, 43), (x1 - sw, y1 - sw)]),
		fill='#ffffff')
	cv.path(circ(41, 25, cv.pick(None, cv.u(1.5), cv.u(2), 3.6)),
		fill='#ffffff')


def a_settings(cv):
	tile(cv, '#7a879e', '#3b465c')
	if cv.px == 16:
		tile_pixels(cv, ['...##...', '.######.', '.##..##.', '###..###',
				 '###..###', '.##..##.', '.######.', '...##...'])
		return
	gear(cv, 32, 32, cv.pick(None, cv.u(7), cv.u(9.5), 19), '#ffffff')


def a_display(cv):
	tile(cv, '#4f8cff', '#2346c0')
	if cv.px == 16:
		tile_pixels(cv, ['########', '#......#', '#..##..#', '#......#',
				 '########', '...##...', '.######.'])
		return
	sw = cv.pick(None, cv.u(1.5), cv.u(2), 3.6)
	x0, y0, x1, y1 = cv.q(12), cv.q(14), cv.q(52), cv.q(42)
	cv.path(rr(x0 + sw / 2, y0 + sw / 2, x1 - sw / 2, y1 - sw / 2,
		   cv.pick(0, cv.u(1), cv.u(1.5), 3)), stroke='#ffffff', sw=sw)
	cv.path('M32 %sV%sM%s %sH%s' % (n(y1), n(cv.q(49)), n(cv.q(22)),
					n(cv.q(49) + sw / 2), n(cv.q(42))),
		stroke='#ffffff', sw=sw)
	# A brightness sun on the screen.
	cx, cy = 32, (y0 + y1) / 2
	r = cv.pick(None, cv.u(2), cv.u(2.5), 4.5)
	cv.path(circ(cx, cy, r), fill='#ffffff')
	if cv.px != 24:
		d = []
		for i in range(8):
			a = i * math.pi / 4
			d.append('M%s %sL%s %s' % (n(cx + r * 1.6 * math.cos(a)),
						   n(cy + r * 1.6 * math.sin(a)),
						   n(cx + r * 2.2 * math.cos(a)),
						   n(cy + r * 2.2 * math.sin(a))))
		cv.path(''.join(d), stroke='#ffffff', sw=sw * 0.7)


def a_keyboard(cv):
	"""Keyboard settings: a white keyboard on a teal tile, its keys cut
	out of it in the tile's own colour."""
	tile(cv, '#2fb5a3', '#147a86')
	if cv.px == 16:
		tile_pixels(cv, ['############', '#.#.#.#.#.##', '############',
				 '##.#.#.#.#.#', '############', '###......###',
				 '############'])
		return
	hole = '#167f8a'
	if cv.master:
		body = (12, 19, 52, 45)
		r, key, pitch = 4, 4.4, 7
		cols = [16 + i * pitch for i in range(5)]
		rows = [23.5, 30.5]
		space = (23, 37.5, 41.4, 41.9)
	else:
		# In pixels of the size drawn: 2-pixel keys a pixel apart.
		body, cols, rows, space = cv.pick(
			None,
			((4, 7, 20, 17), [5, 8, 11, 14, 17], [8, 11],
			 (8, 14, 16, 16)),
			((7, 9, 25, 21), [9, 12, 15, 18, 21], [11, 14],
			 (12, 17, 20, 19)), None)
		body = tuple(cv.u(v) for v in body)
		cols = [cv.u(v) for v in cols]
		rows = [cv.u(v) for v in rows]
		space = tuple(cv.u(v) for v in space)
		r, key = cv.u(1), cv.u(2)
	cv.path(rr(*body, r), fill='#ffffff')
	keys = []
	for y in rows:
		for x in cols:
			keys.append(rr(x, y, x + key, y + key,
				       1 if cv.master else 0))
	keys.append(rr(space[0], space[1], space[2], space[3],
		       1 if cv.master else 0))
	cv.path(''.join(keys), fill=hole)


def a_wallpaper(cv):
	x0, y0, x1, y1 = tile(cv, '#7fb8ff', '#3b74f0')
	r = cv.pick(cv.u(3), cv.u(4.5), cv.u(6), 13)
	clip = cv.clip(rr(x0, y0, x1, y1, r))
	cv.add('<g clip-path="%s">' % clip)
	cv.path(circ(44, 22, cv.pick(cv.u(1.5), cv.u(2.5), cv.u(3), 6)),
		fill='#fff3c4')
	cv.path('M0 46C14 34 26 34 40 44S58 48 64 42V64H0Z',
		fill=cv.vgrad(34, 64, '#3fc283', '#16834a'))
	cv.path('M0 54C16 46 34 50 46 56S60 58 64 54V64H0Z',
		fill=cv.vgrad(46, 64, '#21a067', '#0f6b3d'))
	cv.add('</g>')
	if cv.master:
		cv.path(rr(x0 + 0.5, y0 + 0.5, x1 - 0.5, y1 - 0.5, r - 0.5),
			stroke='#ffffff', sw=1, op=0.3)


def a_browser(cv):
	x0, y0, x1, y1 = tile(cv, '#ffffff', '#dfe6f1', light=True)
	globe(cv, 32, 32, cv.pick(cv.u(5.5), cv.u(8), cv.u(10.5), 20))


def a_chat(cv):
	tile(cv, '#f06aa8', '#b0306e')
	if cv.px == 16:
		tile_pixels(cv, ['.########.', '##########', '#.##.##.##',
				 '##########', '.########.', '.##.......',
				 '.#........'])
		return
	# Two speech bubbles, the back one translucent.
	cv.path(rr(cv.q(26), cv.q(26), cv.q(52), cv.q(46),
		   cv.pick(0, cv.u(2), cv.u(3), 7)) +
		poly([(44, 44), (48, 52), (36, 45)]), fill='#ffffff', op=0.6)
	cv.path(rr(cv.q(12), cv.q(14), cv.q(42), cv.q(36),
		   cv.pick(0, cv.u(2), cv.u(3), 7)) +
		poly([(18, 34), (16, 44), (26, 35)]), fill='#ffffff')
	if cv.px != 24:
		for i in range(3):
			cv.path(circ(20 + i * 7, 25, cv.pick(None, None, cv.u(1),
							     2)),
				fill='#b0306e')


def a_avatar(cv):
	R = cv.pick(cv.u(7.5), cv.u(11.5), cv.u(15), 29)
	d = circ(32, 32, R)
	drop_shadow(cv, d)
	cv.path(d, fill=cv.vgrad(32 - R, 32 + R, '#a9bde6', '#6380c0'))
	clip = cv.clip(d)
	cv.add('<g clip-path="%s">' % clip)
	s = R / 29
	cv.path(circ(32, 32 - 7 * s, 10 * s), fill='#ffffff')
	cv.path(ell(32, 32 + 27 * s, 20 * s, 15 * s), fill='#ffffff')
	cv.add('</g>')


def a_logo(cv):
	x0, y0, s, w, d, t = mark_box(cv)
	mark(cv, x0, y0, s, mode='colour', w=w, d=d, t=t)


def a_logo_symbolic(cv):
	x0, y0, s, w, d, t = mark_box(cv)
	mark(cv, x0, y0, s, mode='flat', colour='#000000', w=w, d=d, t=t)


# ---------------------------------------------------------------------------
# The start orb: the panel's launcher button, a round glass body with the
# mark in white.  A 44-pixel orb centred in a 48-pixel square (the margin
# holds the hover halo), so the 64-unit canvas holds an orb of radius
# 29.33.  Three states the panel cross-fades between: at rest, lit (the
# pointer over it, or the launcher open) and pressed.
# ---------------------------------------------------------------------------
ORB_R = 88 / 3

# Glass body, from the light pooled at the bottom to the deep edge.
ORB_GLASS = {
	'normal': ((0, '#7fe0ff'), (0.3, '#3a88f0'), (0.64, '#2350c8'),
		   (1, '#0c226b')),
	'hover': ((0, '#b4f3ff'), (0.32, '#5aa8ff'), (0.66, '#2e64de'),
		  (1, '#14328a')),
	'pressed': ((0, '#4aa6e4'), (0.3, '#2763d2'), (0.62, '#183a9e'),
		    (1, '#081a52')),
}


def start_orb(state):
	def draw(cv):
		c, R = 32, ORB_R
		lit, down = state == 'hover', state == 'pressed'
		g = R - 1.0			# the glass inside the rim
		if lit:
			# The halo: a soft light just outside the rim.
			cv.path(circ(c, c, 32), fill=cv.rad(
				c, c, 32, (0.84, '#7cc4ff', 0.6),
				(0.93, '#5fa8ff', 0.22), (1, '#5fa8ff', 0)))
		else:
			cv.path(circ(c, c + 0.8, R), fill='#03081a', op=0.55,
				extra='filter="%s"' % cv.blur(0.9))
		cv.path(circ(c, c, R), fill='#081638')
		cv.path(circ(c, c, g), fill=cv.rad(
			c, c + R * 0.62, R * 1.3, *ORB_GLASS[state],
			fx=c, fy=c + R * 0.92))
		# Depth: the glass darkens towards its edge.
		cv.path(circ(c, c, g), fill=cv.rad(
			c, c, g, (0.68, '#050f33', 0), (1, '#050f33', 0.45)))
		# Light gathered at the bottom of the glass.
		cv.path(ell(c, c + R * 0.64, R * 0.58, R * 0.3), fill=cv.rad(
			c, c + R * 0.7, R * 0.6, (0, '#c8f6ff',
						  0.9 if lit else 0.5),
			(1, '#8fe6ff', 0)))
		if down:
			# Pressed in: a shadow along the top inside the rim.
			cv.path(circ(c, c, g), fill=cv.vgrad(
				c - g, c + g * 0.1, (0, '#020823', 0.55),
				(1, '#020823', 0)))
		s = 80 / 3			# 20 pixels
		x0 = c - s / 2
		y0 = c - s / 2 + (0.6 if down else 0)
		# Stem 4, ring 14 with a 3.5 stroke, in pixels at 48.
		ell_, ring = mark_paths(x0, y0, s, 16 / 3, 56 / 3, 14 / 3)
		if lit:
			cv.path(ell_ + ring, fill='#e8f6ff', op=0.55,
				extra='filter="%s"' % cv.blur(1.8))
		cv.path(ell_ + ring, fill='#061440', op=0.5,
			extra='transform="translate(0 0.7)" filter="%s"' %
			cv.blur(0.6))
		cv.path(ell_ + ring, fill=cv.vgrad(y0, y0 + s,
						  (0, '#ffffff'),
						  (1, '#e4eeff')),
			op=0.88 if down else 0.97)
		# The gloss: a crescent of light along the top of the glass,
		# fading downwards, and a small soft glint high on the left.
		gloss = 0.4 if down else 0.78 if lit else 0.7
		top = c - g + 0.6
		cv.add('<g clip-path="%s">' % cv.clip(circ(c, c, g)))
		cv.path(circ(c, c, g - 0.6) + circ(c, c + R * 0.46, R * 1.04),
			fill=cv.vgrad(top, c - R * 0.5,
				      (0, '#ffffff', gloss),
				      (0.6, '#ffffff', gloss * 0.35),
				      (1, '#ffffff', 0.05)),
			extra='fill-rule="evenodd"')
		cv.add('</g>')
		cv.path(ell(0, 0, R * 0.2, R * 0.1), fill='#ffffff',
			op=gloss * 0.6,
			extra='transform="translate(%s %s) rotate(-32)" '
			'filter="%s"' % (n(c - R * 0.5), n(c - R * 0.6),
					 cv.blur(0.9)))
		# Rim light, brightest at the bottom where the glow exits.
		cv.path(circ(c, c, R - 1.5), stroke=cv.vgrad(
			c - R, c + R, (0, '#ffffff', 0.3 if not down else 0.15),
			(0.5, '#ffffff', 0.03),
			(1, '#c4f4ff', 0.75 if lit else 0.5 if not down else 0.25)),
			sw=0.9)
		cv.path(circ(c, c, R - 0.5), stroke='#2a4d9e' if lit else
			'#061230', sw=1.0, op=0.9)
	return draw


ORBS = {
	'likeos-start-orb': start_orb('normal'),
	'likeos-start-orb-hover': start_orb('hover'),
	'likeos-start-orb-pressed': start_orb('pressed'),
}


# ---------------------------------------------------------------------------
# The catalogue: context -> name -> drawing.  gen-icons.py names these files
# in its tables; a name added here and not there is drawn but not installed.
# ---------------------------------------------------------------------------
ICONS = {
	'places': {
		'folder': folder_icon(),
		'folder-open': folder_icon(open_=True),
		'folder-documents': folder_icon('documents'),
		'folder-download': folder_icon('download'),
		'folder-music': folder_icon('music'),
		'folder-pictures': folder_icon('pictures'),
		'folder-videos': folder_icon('videos'),
		'folder-templates': folder_icon('templates'),
		'folder-publicshare': folder_icon('public'),
		'folder-remote': folder_icon('remote'),
		'user-desktop': folder_icon('desktop'),
		'user-home': folder_icon('home'),
		'user-trash': p_trash(False),
		'user-trash-full': p_trash(True),
		'network-workgroup': d_network,
		'network-server': d_server,
		'go-home': p_home,
		'gallery': p_gallery,
		'user-bookmarks': p_star,
		'document-open-recent': p_recent,
	},
	'devices': {
		'computer': d_computer,
		'computer-laptop': d_laptop,
		'drive-harddisk': d_harddisk,
		'drive-removable-media': d_usb,
		'drive-optical': d_optical_drive,
		'media-optical': d_media_optical,
		'printer': d_printer,
		'input-keyboard': d_keyboard,
		'input-mouse': d_mouse,
		'audio-speakers': d_speakers,
		'audio-headphones': d_headphones,
		'camera-photo': d_camera,
		'phone': d_phone,
	},
	'mimetypes': {
		'text-x-generic': doc_icon(T_TEXT, 'TXT', c_lines()),
		'image-x-generic': doc_icon(T_IMAGE, None, c_picture,
					    band_glyph=bg_picture),
		'video-x-generic': doc_icon(T_VIDEO, None, c_video,
					    band_glyph=bg_play),
		'audio-x-generic': doc_icon(T_AUDIO, None, c_audio,
					    band_glyph=bg_note),
		'application-pdf': doc_icon(T_PDF, 'PDF', c_lines(T_PDF)),
		'package-x-generic': doc_icon(T_ZIP, 'ZIP', c_zip),
		'application-x-executable': doc_icon(T_SLATE, 'APP',
						     c_gear(T_SLATE)),
		'application-x-sharedlib': doc_icon(T_SLATE, 'LIB',
						    c_gear('#8d97a8')),
		'application-x-object': doc_icon(T_SLATE, 'OBJ',
						 c_code(T_SLATE)),
		'text-html': doc_icon(T_DOC, 'HTML', c_brackets('<', '>', T_DOC)),
		'application-x-shellscript': doc_icon(T_XLS, 'SH', c_terminal),
		'x-office-document': doc_icon(T_DOC, 'DOC', c_lines(T_DOC)),
		'x-office-spreadsheet': doc_icon(T_XLS, 'XLS', c_sheet),
		'x-office-presentation': doc_icon(T_PPT, 'PPT', c_slide),
		'application-x-desktop': doc_icon(None, None, c_desktop_entry),
		'unknown': doc_icon(),
		'application-octet-stream': doc_icon(T_TEXT, 'BIN',
						     c_code(T_TEXT)),
		'text-x-csrc': doc_icon(T_CODE, 'C', c_code()),
		'text-x-python': doc_icon(T_CODE, 'PY', c_code()),
		'text-x-makefile': doc_icon(T_CODE, 'MAKE', c_gear(T_CODE)),
		'text-markdown': doc_icon(T_TEXT, 'MD', c_lines(INK)),
		'text-x-readme': doc_icon(T_DOC, None, c_lines(),
					  band_glyph=bg_info),
		'application-json': doc_icon(T_CODE, 'JSON',
					     c_brackets('{', '}')),
		'text-xml': doc_icon(T_CODE, 'XML', c_brackets('<', '>')),
		'text-x-patch': doc_icon(T_TEXT, 'DIFF', c_diff),
		'font-x-generic': doc_icon(T_TEXT, 'FONT', c_font),
		'application-x-cd-image': doc_icon(T_SLATE, 'ISO', c_cd),
		'x-office-calendar': doc_icon(T_DOC, 'ICS', c_calendar),
	},
	'apps': {
		'system-file-manager': a_files,
		'utilities-terminal': a_terminal,
		'accessories-calculator': a_calculator,
		'accessories-text-editor': a_text_editor,
		'internet-mail': a_mail,
		'multimedia-player': a_media_player,
		'image-viewer': a_image_viewer,
		'preferences-system': a_settings,
		'preferences-desktop-display': a_display,
		'preferences-desktop-keyboard': a_keyboard,
		'preferences-desktop-wallpaper': a_wallpaper,
		'avatar-default': a_avatar,
		'web-browser': a_browser,
		'internet-chat': a_chat,
		'likeos-logo': a_logo,
		'likeos-logo-symbolic': a_logo_symbolic,
	},
	'status': {
		'dialog-information': s_info,
		'dialog-warning': s_warning,
		'dialog-error': s_error,
		'dialog-question': s_question,
	},
}

VARIANTS = [(None, ''), (32, '-32'), (24, '-24'), (16, '-16')]


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('--out', default=os.path.join(
		os.path.dirname(os.path.abspath(__file__)), 'src'))
	ap.add_argument('--only', help='draw just this name')
	a = ap.parse_args()
	count = 0
	for ctx, table in ICONS.items():
		d = os.path.join(a.out, ctx)
		os.makedirs(d, exist_ok=True)
		for name, fn in table.items():
			if a.only and name != a.only:
				continue
			for px, suffix in VARIANTS:
				cv = Cv(px)
				fn(cv)
				what = ('%s, %d pixels' % (name, px) if px else
					'%s, master drawing' % name)
				with open(os.path.join(d, name + suffix + '.svg'),
					  'w') as f:
					f.write(cv.svg('LikeOS icon: ' + what + '.'))
				count += 1
	# The start orb is drawn once, for 48 pixels and up (the panel loads
	# it at 48 and, on a doubled display, 96).
	d = os.path.join(a.out, 'orb')
	os.makedirs(d, exist_ok=True)
	for name, fn in ORBS.items():
		if a.only and name != a.only:
			continue
		cv = Cv(None)
		fn(cv)
		with open(os.path.join(d, name + '.svg'), 'w') as f:
			f.write(cv.svg('LikeOS start orb: %s.' % name))
		count += 1
	print('draw-icons: %d SVG files written to %s' % (count, a.out))


if __name__ == '__main__':
	main()
