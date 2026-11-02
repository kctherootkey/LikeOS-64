#!/usr/bin/env python3
# LikeOS -- draw the extra desktop wallpapers.
#
# Three abstract pictures a user can pick from ~/Pictures/Wallpapers (the
# default wallpaper, res/LikeOS.png, is not drawn here):
#
#   polar     curtains of polar light over a deep night field: fine vertical
#             rays rising from a bright, gently waving lower edge, aqua at
#             the edge, blue and violet higher up, under a sparse sky.
#   horizon   the curved rim of a planet seen from orbit, a thin lit band of
#             atmosphere across the lower third and a soft dawn glow where
#             the light comes over it.
#   drift     out-of-focus discs of light at several depths drifting along
#             a soft diagonal beam, blue, indigo and a little teal.
#
# Everything is drawn here with cairo and Pillow -- no image goes in -- so
# the pictures are ours to ship and can be redrawn at any size.  The random
# parts (stars, rays, discs, film grain) come from fixed seeds, so a run
# reproduces the committed files byte for byte.  The grain, a fraction of
# one level, keeps the long gradients from banding on 8-bit displays.
#
#     python3 host/gen-wallpaper.py [--only NAME] [--size WxH] [--out DIR]
#
# The default is res/LikeOS-<name>.png at 1920x1200, the 16:10 size the
# build asks the firmware for (see res/xorg/gtk3/pcmanfm.conf).
#
# Copyright (C) 2026 The LikeOS Project

import argparse
import math
import os
import random

import cairo
from PIL import Image, ImageChops, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))


def rgb(h, a=1.0):
    return ((h >> 16) / 255.0, ((h >> 8) & 0xff) / 255.0, (h & 0xff) / 255.0, a)


def to_image(surf):
    """A cairo RGB24/ARGB32 surface as a Pillow RGB image."""
    surf.flush()
    im = Image.frombuffer('RGBA', (surf.get_width(), surf.get_height()),
                          bytes(surf.get_data()), 'raw', 'BGRA',
                          surf.get_stride(), 1)
    return im.convert('RGB')


def scaled(im, f):
    """Every channel multiplied by f (0..1)."""
    lut = [min(255, round(v * f)) for v in range(256)]
    return im.point(lut * 3)


def blur(im, r):
    return im.filter(ImageFilter.GaussianBlur(r))


def grain(im, seed, amp=1.6):
    """Monochrome film grain of +-amp levels, tiled from one fixed square."""
    rnd = random.Random(seed)
    t = 509                           # a prime, so the tiling does not line up
    data = bytes(max(0, min(255, round(128 + rnd.gauss(0, amp * 0.6))))
                 for _ in range(t * t))
    tile = Image.frombytes('L', (t, t), data)
    w, h = im.size
    g = Image.new('L', (w, h))
    for y in range(0, h, t):
        for x in range(0, w, t):
            g.paste(tile, (x, y))
    g = Image.merge('RGB', (g, g, g))
    return ImageChops.add(im, g, 1.0, -128)


def vignette(cr, W, H, strength):
    g = cairo.RadialGradient(W * 0.5, H * 0.5, min(W, H) * 0.35,
                             W * 0.5, H * 0.5, math.hypot(W, H) * 0.62)
    g.add_color_stop_rgba(0, 0, 0, 0, 0)
    g.add_color_stop_rgba(1, 0.0, 0.0, 0.02, strength)
    cr.set_source(g)
    cr.paint()


def stars(cr, W, H, S, rnd, count, top, bottom, bright=1.0):
    for _ in range(count):
        x = rnd.uniform(0, W)
        y = top + (bottom - top) * rnd.random() ** 1.6
        r = (0.45 + rnd.random() ** 3 * 1.1) * S
        a = (0.18 + 0.7 * rnd.random() ** 2) * bright
        tint = rnd.choice((0xffffff, 0xdfe9ff, 0xcfe0ff, 0xfff3e6))
        if r > 1.2 * S:
            g = cairo.RadialGradient(x, y, 0, x, y, r * 4)
            g.add_color_stop_rgba(0, *rgb(tint, a * 0.35))
            g.add_color_stop_rgba(1, *rgb(tint, 0))
            cr.set_source(g)
            cr.arc(x, y, r * 4, 0, 2 * math.pi)
            cr.fill()
        cr.set_source_rgba(*rgb(tint, a))
        cr.arc(x, y, r, 0, 2 * math.pi)
        cr.fill()


# ---------------------------------------------------------------------------
# polar
# ---------------------------------------------------------------------------
def draw_polar(W, H):
    S = W / 1920.0
    rnd = random.Random(0x1a0b5)
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    g = cairo.LinearGradient(0, 0, 0, H)
    g.add_color_stop_rgba(0.0, *rgb(0x03071a))
    g.add_color_stop_rgba(0.5, *rgb(0x07123a))
    g.add_color_stop_rgba(1.0, *rgb(0x0a1a4a))
    cr.set_source(g)
    cr.paint()
    # A low glow along the bottom, the light the curtains throw down.
    g = cairo.RadialGradient(W * 0.45, H * 0.62, 0, W * 0.45, H * 0.62, H * 0.8)
    g.add_color_stop_rgba(0, *rgb(0x1d6fb4, 0.32))
    g.add_color_stop_rgba(0.5, *rgb(0x164f9a, 0.12))
    g.add_color_stop_rgba(1, *rgb(0x0b1f55, 0))
    cr.set_source(g)
    cr.paint()
    stars(cr, W, H, S, rnd, int(260 * S * S + 40), 0, H * 0.75)
    base = to_image(surf)

    # The curtains, ray by ray, added up on black.
    lay = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    lc = cairo.Context(lay)
    lc.set_source_rgb(0, 0, 0)
    lc.paint()
    lc.set_operator(cairo.OPERATOR_ADD)
    lc.set_line_cap(cairo.LINE_CAP_BUTT)
    curtains = [
        # (edge height at the start and at the end, wave amplitude, ray
        #  length at the start and at the end, x from, x to, phase, gain):
        # each curtain recedes, so its edge rises and its rays shorten.
        (0.80, 0.44, 0.050, 0.50, 0.30, -0.06, 0.78, 0.4, 0.95),
        (0.66, 0.36, 0.040, 0.36, 0.22, 0.34, 1.06, 2.1, 0.7),
        (0.40, 0.27, 0.025, 0.20, 0.13, 0.58, 1.08, 4.0, 0.45),
    ]
    step = 1.25 * S
    for (ea, eb, amp, la, lb, xa, xb, ph, gain) in curtains:
        f = [rnd.uniform(0.6, 1.4) for _ in range(6)]
        p = [rnd.uniform(0, 2 * math.pi) for _ in range(6)]
        x = xa * W
        while x < xb * W:
            u = (x - xa * W) / ((xb - xa) * W)       # 0..1 along
            env = math.sin(math.pi * min(1.0, max(0.0, u))) ** 0.7
            xs = x / S
            edge = H * (ea + (eb - ea) * u) + H * amp * (
                math.sin(xs / 260 * f[0] + ph) * 0.65 +
                math.sin(xs / 109 * f[1] + p[1]) * 0.25 +
                math.sin(xs / 47 * f[2] + p[2]) * 0.10)
            fold = (0.5 + 0.5 * math.sin(xs / 11.0 * f[3] + p[3]) *
                    math.sin(xs / 37.0 * f[4] + p[4]))
            fine = 0.5 + 0.5 * math.sin(xs / 3.1 * f[5] + p[5])
            inten = gain * env * (0.35 + 0.45 * fold + 0.2 * fine)
            length = H * (la + (lb - la) * u) * (
                0.75 + 0.25 * math.sin(xs / 170 + p[0]))
            tilt = 0.06 * length
            below = 0.16 * length          # light scattered under the edge
            lg = cairo.LinearGradient(x, edge + below, x + tilt, edge - length)
            lg.add_color_stop_rgba(0.0, *rgb(0x2f8fd0, 0))
            lg.add_color_stop_rgba(0.1, *rgb(0x5fd8ea, inten * 0.22))
            lg.add_color_stop_rgba(0.14, *rgb(0x8ff7ea, inten * 0.85))
            lg.add_color_stop_rgba(0.22, *rgb(0x52d6e8, inten * 0.6))
            lg.add_color_stop_rgba(0.45, *rgb(0x3b7ff0, inten * 0.32))
            lg.add_color_stop_rgba(0.74, *rgb(0x6650e6, inten * 0.15))
            lg.add_color_stop_rgba(1.0, *rgb(0x5a3cd0, 0))
            lc.set_source(lg)
            lc.set_line_width(step * 1.1)
            lc.move_to(x, edge + below)
            lc.line_to(x + tilt, edge - length)
            lc.stroke()
            x += step
    rays = to_image(lay)
    glow = scaled(blur(rays, 40 * S), 0.9)
    soft = scaled(blur(rays, 6 * S), 0.85)
    fine_ = scaled(blur(rays, 1.2 * S), 0.35)
    im = ImageChops.screen(base, glow)
    im = ImageChops.screen(im, soft)
    im = ImageChops.add(im, fine_)
    return finish(im, W, H, 0.45, 0x1a0b6)


# ---------------------------------------------------------------------------
# horizon
# ---------------------------------------------------------------------------
def draw_horizon(W, H):
    S = W / 1920.0
    rnd = random.Random(0x4021)
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    g = cairo.RadialGradient(W * 0.3, H * 0.75, 0, W * 0.3, H * 0.75, W * 0.95)
    g.add_color_stop_rgba(0.0, *rgb(0x16236a))
    g.add_color_stop_rgba(0.45, *rgb(0x0a1440))
    g.add_color_stop_rgba(1.0, *rgb(0x03061a))
    cr.set_source(g)
    cr.paint()
    stars(cr, W, H, S, rnd, int(340 * S * S + 40), 0, H * 0.8)
    base = to_image(surf)

    # The planet: a huge disc whose rim crosses the picture low and
    # rising to the right.
    R = W * 1.55
    pcx, pcy = W * 0.34, H * 0.71 + R
    rise = -0.11                      # tilt of the rim, radians
    pcx, pcy = (pcx + R * math.sin(rise) * -1.0, pcy)

    def planet_path(c, r):
        c.arc(pcx, pcy, r, 0, 2 * math.pi)

    # The lit atmosphere, a thin band along the rim, and the dawn glow at
    # one point of it.
    atm = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    ac = cairo.Context(atm)
    ac.set_source_rgb(0, 0, 0)
    ac.paint()
    ac.set_operator(cairo.OPERATOR_ADD)
    sun_a = -math.pi / 2 - 0.12        # where the light comes over
    sx = pcx + R * math.cos(sun_a)
    for width, col, a in ((26 * S, 0x2e6bd8, 0.55), (10 * S, 0x4fb4f2, 0.7),
                          (3.2 * S, 0xbdf0ff, 0.9)):
        # Brightest near the dawn point, fading along the rim.
        lg = cairo.LinearGradient(0, 0, W, 0)
        for i in range(25):
            u = i / 24.0
            k = math.exp(-((u * W - sx) / (0.36 * W)) ** 2) * 0.85 + 0.15
            lg.add_color_stop_rgba(u, *rgb(col, a * k))
        ac.set_source(lg)
        ac.set_line_width(width)
        ac.arc(pcx, pcy, R + width * 0.15, -math.pi / 2 - 0.9,
               -math.pi / 2 + 0.9)
        ac.stroke()
    sy = pcy + R * math.sin(sun_a)
    g = cairo.RadialGradient(sx, sy, 0, sx, sy, 420 * S)
    g.add_color_stop_rgba(0, *rgb(0xe6f7ff, 0.85))
    g.add_color_stop_rgba(0.08, *rgb(0x9fd9ff, 0.45))
    g.add_color_stop_rgba(0.4, *rgb(0x3d7ae0, 0.14))
    g.add_color_stop_rgba(1, *rgb(0x16236a, 0))
    ac.set_source(g)
    ac.paint()
    # A faint horizontal flare through the dawn point.
    ac.save()
    ac.translate(sx, sy)
    ac.rotate(sun_a + math.pi / 2)
    ac.scale(1, 0.035)
    g = cairo.RadialGradient(0, 0, 0, 0, 0, 520 * S)
    g.add_color_stop_rgba(0, *rgb(0xcfeeff, 0.2))
    g.add_color_stop_rgba(1, *rgb(0x6aa8ff, 0))
    ac.set_source(g)
    ac.arc(0, 0, 520 * S, 0, 2 * math.pi)
    ac.fill()
    ac.restore()
    light = to_image(atm)
    im = ImageChops.screen(base, scaled(blur(light, 30 * S), 0.8))
    im = ImageChops.screen(im, scaled(blur(light, 3 * S), 0.9))
    im = ImageChops.add(im, scaled(blur(light, 0.8 * S), 0.35))

    # The night side of the planet over everything below the rim, faintly
    # lit along its edge.
    body = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    bc = cairo.Context(body)
    g = cairo.RadialGradient(sx, sy + 60 * S, 0, sx, sy + 60 * S, W * 0.9)
    g.add_color_stop_rgba(0, *rgb(0x1a3a7a))
    g.add_color_stop_rgba(0.12, *rgb(0x0e2152))
    g.add_color_stop_rgba(0.45, *rgb(0x070f2c))
    g.add_color_stop_rgba(1, *rgb(0x03071a))
    bc.set_source(g)
    planet_path(bc, R)
    bc.fill()
    # Light bleeding just inside the rim.
    g = cairo.RadialGradient(pcx, pcy, R - 90 * S, pcx, pcy, R)
    g.add_color_stop_rgba(0, *rgb(0x2a5bd7, 0))
    g.add_color_stop_rgba(0.85, *rgb(0x2a5bd7, 0.10))
    g.add_color_stop_rgba(1, *rgb(0x6fb8ff, 0.32))
    bc.set_source(g)
    planet_path(bc, R)
    bc.fill()
    body.flush()
    bim = Image.frombuffer('RGBA', (W, H), bytes(body.get_data()), 'raw',
                           'BGRa', body.get_stride(), 1)
    # Soften the rim edge by a pixel, as optics would.
    mask = blur(bim.getchannel('A'), 0.9 * S)
    im = Image.composite(bim.convert('RGB'), im, mask)
    # The atmosphere glows over the dark edge too.
    im = ImageChops.screen(im, scaled(blur(light, 2.0 * S), 0.6))
    return finish(im, W, H, 0.35, 0x4022)


# ---------------------------------------------------------------------------
# drift
# ---------------------------------------------------------------------------
def draw_drift(W, H):
    S = W / 1920.0
    rnd = random.Random(0xd71f7)
    ang = 0.36                        # the beam rises to the right
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    g = cairo.LinearGradient(0, 0, W * 0.4, H)
    g.add_color_stop_rgba(0.0, *rgb(0x070b26))
    g.add_color_stop_rgba(0.55, *rgb(0x0c1a52))
    g.add_color_stop_rgba(1.0, *rgb(0x101a5e))
    cr.set_source(g)
    cr.paint()
    base = to_image(surf)

    def along(t, off):
        """A point t (0..1) along the beam, off pixels across it."""
        x = W * (-0.1 + 1.2 * t)
        y = H * 0.58 - (x - W * 0.5) * math.tan(ang)
        return x - off * math.sin(ang), y + off * math.cos(ang)

    # The beam: a soft band of light, violet at the left, blue through the
    # middle, teal where it leaves at the upper right.
    lay = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    lc = cairo.Context(lay)
    lc.set_source_rgb(0, 0, 0)
    lc.paint()
    lc.set_operator(cairo.OPERATOR_ADD)
    x0, y0 = along(0, 0)
    x1, y1 = along(1, 0)
    lg = cairo.LinearGradient(x0, y0, x1, y1)
    lg.add_color_stop_rgba(0.0, *rgb(0x5a3cd0, 0.5))
    lg.add_color_stop_rgba(0.45, *rgb(0x3b74f0, 0.75))
    lg.add_color_stop_rgba(0.8, *rgb(0x3aa6e8, 0.7))
    lg.add_color_stop_rgba(1.0, *rgb(0x46c6e6, 0.5))
    for width, k in ((H * 0.42, 0.35), (H * 0.2, 0.45), (H * 0.06, 0.5)):
        lc.set_source(lg)
        lc.push_group()
        lc.set_source(lg)
        lc.set_line_width(width)
        lc.move_to(x0, y0)
        lc.line_to(x1, y1)
        lc.stroke()
        lc.pop_group_to_source()
        lc.paint_with_alpha(k)
    beam = to_image(lay)
    im = ImageChops.screen(base, scaled(blur(beam, 120 * S), 0.85))

    # Discs on three depth layers: large and very soft far away, smaller
    # and crisper close to the band's middle.
    palette = [0x5b8ff9, 0x3f74f2, 0x7aa6ff, 0x6f5cf0, 0x46c6e6, 0x9cc2ff]
    for count, rmin, rmax, spread, alpha, sharp in (
            (12, 110, 220, 0.22, 0.13, 22),
            (26, 40, 110, 0.15, 0.17, 6),
            (40, 10, 38, 0.09, 0.28, 1.4)):
        lay = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
        lc = cairo.Context(lay)
        lc.set_source_rgb(0, 0, 0)
        lc.paint()
        lc.set_operator(cairo.OPERATOR_ADD)
        for _ in range(count):
            t = rnd.random()
            x, y = along(t, rnd.gauss(0, H * spread))
            r = rnd.uniform(rmin, rmax) * S
            # Colour follows the beam: violet left, teal right.
            col = palette[min(len(palette) - 1,
                              max(0, int(rnd.gauss(t * 5.0, 1.2))))]
            a = alpha * rnd.uniform(0.5, 1.0)
            # A lens disc: an even fill with a slightly brighter rim.
            g = cairo.RadialGradient(x, y, 0, x, y, r)
            g.add_color_stop_rgba(0, *rgb(col, a * 0.7))
            g.add_color_stop_rgba(0.86, *rgb(col, a * 0.9))
            g.add_color_stop_rgba(0.97, *rgb(col, a * 1.3))
            g.add_color_stop_rgba(1, *rgb(col, 0))
            lc.set_source(g)
            lc.arc(x, y, r, 0, 2 * math.pi)
            lc.fill()
        im = ImageChops.screen(im, blur(to_image(lay), sharp * S))
    # Pin points of light, densest in the band.
    pts = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    pc = cairo.Context(pts)
    pc.set_source_rgb(0, 0, 0)
    pc.paint()
    for _ in range(int(140 * S * S + 10)):
        x, y = along(rnd.random(), rnd.gauss(0, H * 0.1))
        r = rnd.uniform(0.6, 1.5) * S
        pc.set_source_rgba(*rgb(0xdfeaff, rnd.uniform(0.25, 0.8)))
        pc.arc(x, y, r, 0, 2 * math.pi)
        pc.fill()
    im = ImageChops.screen(im, blur(to_image(pts), 0.6 * S))
    return finish(im, W, H, 0.5, 0xd71f8)


def finish(im, W, H, vig, seed):
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
    vignette(cairo.Context(surf), W, H, vig)
    surf.flush()
    v = Image.frombuffer('RGBA', (W, H), bytes(surf.get_data()), 'raw',
                         'BGRa', surf.get_stride(), 1)
    im = Image.composite(Image.new('RGB', (W, H), (0, 0, 1)), im,
                         v.getchannel('A'))
    return grain(im, seed)


WALLPAPERS = {'polar': draw_polar, 'horizon': draw_horizon,
              'drift': draw_drift}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', choices=sorted(WALLPAPERS))
    ap.add_argument('--size', default='1920x1200')
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'res'))
    a = ap.parse_args()
    W, H = (int(v) for v in a.size.split('x'))
    for name, fn in WALLPAPERS.items():
        if a.only and name != a.only:
            continue
        p = os.path.join(a.out, 'LikeOS-%s.png' % name)
        fn(W, H).save(p, 'PNG', optimize=True)
        print('wrote', os.path.normpath(p), W, 'x', H)


if __name__ == '__main__':
    main()
