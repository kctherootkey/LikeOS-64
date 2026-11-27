#!/usr/bin/env python3
# Generate the LikeOS login-screen background ("LikeOS Tide").
#
# A deep night-blue field crossed by two sheets of light: each sheet is a
# ribbon of fine parallel threads that twists along its length, so the
# threads bunch into bright folds where the sheet turns edge-on and open out
# where it faces the viewer.  Everything is computed analytically in linear
# light (no source images, no photographs, no third-party artwork), tone
# mapped, then finished with a fine film grain that also dithers the
# gradients so they do not band on 8-bit panels.
#
# Outputs (in --out, default ../theme/likeos/img next to this script):
#   background-3840.jpg       3840x2160, the lock view on large screens
#   background-1920.jpg       1920x1080, the lock view up to 2560 px wide
#   background-blur.jpg       1920x1080, blurred and dimmed, the sign-in view
#
# Requirements: Python 3, numpy, Pillow (tested with numpy 2.x, Pillow 10).
# The output is fully reproducible: the same --seed gives the same files.
#
#   python3 gen-background.py [--out DIR] [--seed N] [--preview]
#
# --preview renders preview.jpg at 1920x1080 only (fast, for design
# iterations) into --out, or the current directory.
#
# Copyright (C) 2026 The LikeOS Project

import argparse
import math
import os

import numpy as np
from PIL import Image

# --- palette (sRGB hex; the LikeOS shell and accent blues) --------------------

PAL = {
    "night_top": "#02040b",
    "night_bottom": "#070f2b",
    "deep_glow": "#12348c",
    "violet_glow": "#2e2378",
    "blue": "#2f66ff",       # near the LikeOS accent highlight #3b74f0
    "cyan": "#43c8ff",
    "ice": "#bfeaff",
    "indigo": "#5b4cf0",
    "violet": "#8a5cff",
    "accent": "#2a5bd7",     # the LikeOS accent fill
}


def hex_lin(h):
    """sRGB hex -> linear-light float triple."""
    h = h.lstrip("#")
    c = np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)],
                 dtype=np.float32)
    return np.where(c <= 0.04045, c / 12.92,
                    ((c + 0.055) / 1.055) ** 2.4).astype(np.float32)


def lin_srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92,
                    1.055 * np.power(x, 1 / 2.4) - 0.055)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# --- blur helpers (separable Gaussian on a small image) -----------------------

def gauss_kernel(sigma):
    r = max(1, int(math.ceil(sigma * 3)))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    return (k / k.sum()).astype(np.float32), r


def blur2d(img, sigma):
    """Gaussian blur of an (h, w) or (h, w, c) float array, edge-clamped."""
    k, r = gauss_kernel(sigma)
    out = img
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        n = out.shape[axis]
        for i, kv in enumerate(k):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + n)
            acc += kv * p[tuple(sl)]
        out = acc
    return out


def resize_f(arr, w, h):
    """Resize a float (h, w, 3) array with Pillow's bicubic filter."""
    chans = [np.asarray(Image.fromarray(arr[..., c], mode="F")
                        .resize((w, h), Image.BICUBIC)) for c in range(3)]
    return np.stack(chans, axis=-1)


def box_down(arr, f):
    h, w = arr.shape[0] // f, arr.shape[1] // f
    a = arr[:h * f, :w * f]
    return a.reshape(h, f, w, f, -1).mean(axis=(1, 3))


# --- the scene ------------------------------------------------------------------

class Ribbon:
    """A twisting sheet of threads, seen slightly from the side.

    Lengths are in screen heights (y in 0..1, x in 0..aspect).  Along its
    length u the sheet has a middle line centre(u), a half-width width(u)
    and a turn angle phase(u).  Thread s (-1..1 across the sheet) is at
        x = u + skew * s * width(u) * sin(phase(u))
        y = centre(u) + s * width(u) * cos(phase(u))
    so where the sheet turns edge-on its threads cross in a soft S-shaped
    fold, not at one point.
    """

    def __init__(self, centre, width, phase, threads, col_a, col_b, col_edge,
                 gain, skew=0.35, light=None, back=0.45, seed=0):
        self.centre, self.width, self.phase = centre, width, phase
        self.threads = threads
        self.col_a, self.col_b, self.col_edge = col_a, col_b, col_edge
        self.gain = gain
        self.skew = skew
        self.light = light      # (x, length): where light runs along it
        self.back = back        # brightness of the sheet's reverse face
        self.seed = seed


def render_ribbon(rb, W, H, aspect, out):
    """Add ribbon `rb' (linear light) into `out' (H, W, 3)."""
    rng = np.random.default_rng(rb.seed)
    cols = np.arange(W, dtype=np.float64)
    xc = (cols + 0.5) / H                         # column centres, in heights
    u = np.arange(-0.4, aspect + 0.4, 0.5 / H)    # half-pixel steps
    cen = rb.centre(u)
    wid = rb.width(u)
    ph = rb.phase(u)
    cphi, sphi = np.cos(ph), np.sin(ph)
    sigma = 0.55 * H / 2160.0 + 0.25              # thread half-thickness, px
    R = 5

    # Light along the sheet; the reverse face in its own shadow.
    lit = np.ones_like(u)
    if rb.light is not None:
        lx, ll = rb.light
        lit = 0.22 + 0.78 * np.exp(-0.5 * ((u - lx) / ll) ** 2)
    face = rb.back + (1 - rb.back) * smoothstep(-0.35, 0.35, cphi)

    n = rb.threads
    for j in range(n):
        s = -1.0 + 2.0 * j / (n - 1)
        xs = u + rb.skew * s * wid * sphi
        ys = cen + s * wid * cphi
        if np.any(np.diff(xs) <= 0):
            # Keep x increasing so the thread is a function of x.
            xs = np.maximum.accumulate(xs + 1e-9 * np.arange(len(xs)))
        yc = np.interp(xc, xs, ys) * H            # thread centre, px
        uc = np.interp(xc, xs, u)
        k_lit = np.interp(uc, u, lit * face)
        slope = np.gradient(yc)
        cosf = 1.0 / np.sqrt(1.0 + slope * slope)
        # Rims catch more light than the open middle; every thread
        # varies a little, as real threads do.
        rim = 0.20 + 0.80 * abs(s) ** 3
        jitter = 0.80 + 0.40 * rng.random()
        t = np.clip(uc / aspect, 0, 1)[:, None]
        col = rb.col_a * (1 - t) + rb.col_b * t
        e = abs(s) ** 4 * 0.55
        col = col * (1 - e) + rb.col_edge * e
        amp = (rb.gain * rim * jitter * k_lit)[:, None] * col
        base = np.floor(yc - 0.5).astype(np.int64)
        for o in range(-R, R + 1):
            r = base + o
            ok = (r >= 0) & (r < H)
            if not ok.any():
                continue
            d = (r + 0.5 - yc) * cosf
            w = np.exp(-0.5 * (d / sigma) ** 2)
            ok &= w > 1e-4
            out[r[ok], cols[ok].astype(np.int64)] += \
                (w[ok, None] * amp[ok]).astype(np.float32)


def scene(W, H, seed):
    aspect = W / H
    Hpx = H
    ys = (np.arange(H, dtype=np.float32) + 0.5) / H
    xs = (np.arange(W, dtype=np.float32) + 0.5) / H
    X, Y = np.meshgrid(xs, ys)

    # Base: the night gradient, two broad glows, a soft vignette.
    top, bot = hex_lin(PAL["night_top"]), hex_lin(PAL["night_bottom"])
    v = smoothstep(0.0, 1.0, Y)[..., None]
    img = top * (1 - v) + bot * v

    def glow(cx, cy, rx, ry, colour, k):
        d2 = ((X - cx) / rx) ** 2 + ((Y - cy) / ry) ** 2
        return np.exp(-d2)[..., None] * hex_lin(colour) * k

    img += glow(aspect * 0.80, 0.78, 0.90, 0.45, PAL["deep_glow"], 0.60)
    img += glow(aspect * 0.05, 1.00, 0.80, 0.45, PAL["violet_glow"], 0.65)
    img += glow(aspect * 0.50, 0.30, 0.80, 0.28, PAL["accent"], 0.030)

    A = aspect
    light = np.zeros_like(img)

    # High sheet: a faint, slender veil above the others.
    render_ribbon(Ribbon(
        centre=lambda x: 0.71 - 0.17 * (x / A) + 0.030 * np.sin(2.4 * x + 1.1),
        width=lambda x: 0.042 + 0.0 * x,
        phase=lambda x: 1.9 * x + 0.2 + np.pi / 2,
        threads=30, skew=0.8,
        col_a=hex_lin(PAL["indigo"]), col_b=hex_lin(PAL["cyan"]),
        col_edge=hex_lin(PAL["ice"]),
        gain=0.14, light=(A * 0.62, 0.45), back=0.5, seed=seed + 11),
        W, H, A, light)

    # Back sheet: wide, indigo to blue, quiet.
    render_ribbon(Ribbon(
        centre=lambda x: 0.90 - 0.20 * (x / A) + 0.045 * np.sin(1.6 * x + 2.3),
        width=lambda x: 0.170 * (0.75 + 0.25 * np.sin(0.9 * x + 0.4)),
        phase=lambda x: 1.05 * x + 2.6 + 0.30 * np.sin(1.4 * x),
        threads=64, skew=0.9,
        col_a=hex_lin(PAL["violet"]), col_b=hex_lin(PAL["blue"]),
        col_edge=hex_lin(PAL["cyan"]),
        gain=0.24, light=(A * 0.22, 0.50), back=0.45, seed=seed + 12),
        W, H, A, light)

    # Main sheet: rises from the lower left towards the right edge and
    # turns over once, right of centre; light runs along it to that fold.
    render_ribbon(Ribbon(
        centre=lambda x: 1.02 - 0.62 * (x / A) + 0.22 * (x / A) ** 2
        + 0.030 * np.sin(2.0 * x + 0.5),
        width=lambda x: 0.120 * (0.8 + 0.2 * np.sin(1.3 * x + 1.0)),
        phase=lambda x: 1.45 * x - 0.40 + 0.30 * np.sin(1.1 * x + 0.3),
        threads=84, skew=1.1,
        col_a=hex_lin(PAL["blue"]), col_b=hex_lin(PAL["cyan"]),
        col_edge=hex_lin(PAL["ice"]),
        gain=0.48, light=(A * 0.64, 0.40), back=0.40, seed=seed + 13),
        W, H, A, light)

    # Bloom: the light of the sheets, widely blurred, under them.
    satin = blur2d(box_down(light, 4), sigma=H / 4 * 0.004)
    light = light + resize_f(satin, W, H) * 0.6
    f = 8
    small = box_down(light, f)
    bloom = blur2d(small, sigma=H / f * 0.022)
    bloom2 = blur2d(small, sigma=H / f * 0.075)
    img += light + resize_f(bloom, W, H) * 0.9 + resize_f(bloom2, W, H) * 0.8

    # A few faint stars in the open sky, as in the LikeOS desktop
    # wallpapers: fewer towards the sheets, none in the lower third.
    rng = np.random.default_rng(seed + 7)
    star = hex_lin(PAL["ice"])
    for _ in range(260):
        sx, sy = rng.random() * aspect, rng.random() ** 1.35 * 0.66
        if rng.random() < sy * 1.2:          # thin out towards the sheets
            continue
        b = 0.05 + 0.55 * rng.random() ** 5
        rad = (0.55 + 0.6 * rng.random()) * H / 2160.0 + 0.25
        cx, cy = sx * H, sy * H
        x0, y0 = int(cx) - 4, int(cy) - 4
        if x0 < 0 or y0 < 0 or x0 + 9 > W or y0 + 9 > H:
            continue
        gx = np.arange(x0, x0 + 9) + 0.5 - cx
        gy = np.arange(y0, y0 + 9) + 0.5 - cy
        g = np.exp(-0.5 * ((gx[None, :] ** 2 + gy[:, None] ** 2) / rad ** 2))
        img[y0:y0 + 9, x0:x0 + 9] += g[..., None] * star * b

    # Vignette.
    vx = (X / aspect - 0.5) * 2
    vy = (Y - 0.5) * 2
    vig = 1.0 - 0.38 * smoothstep(0.35, 1.45, np.sqrt(vx * vx * 0.8 + vy * vy))
    img *= vig[..., None]
    return img


def tonemap(img):
    # A soft shoulder so the folds never clip to flat white.
    return 1.0 - np.exp(-img * 1.0)


def finish(lin, seed, grain=1.0):
    """Linear -> 8-bit sRGB with film grain used as the dither."""
    rng = np.random.default_rng(seed)
    s = lin_srgb(tonemap(lin)) * 255.0
    h, w = s.shape[:2]
    mono = rng.normal(0.0, 1.1 * grain, (h, w, 1)).astype(np.float32)
    chroma = rng.normal(0.0, 0.30 * grain, (h, w, 3)).astype(np.float32)
    # Grain is strongest in the mid-darks, where film shows it most and
    # where 8-bit gradients band.
    s = s + mono + chroma
    return np.clip(np.rint(s), 0, 255).astype(np.uint8)


def save(arr, path, q):
    Image.fromarray(arr, "RGB").save(path, "JPEG", quality=q, subsampling=0,
                                     optimize=True, progressive=True)
    print(f"{path}: {os.path.getsize(path) // 1024} KiB")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=None)
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()
    if a.out is None:
        a.out = os.getcwd() if a.preview else \
            os.path.join(here, "..", "theme", "likeos", "img")
    os.makedirs(a.out, exist_ok=True)

    if a.preview:
        lin = scene(1920, 1080, a.seed)
        save(finish(lin, a.seed), os.path.join(a.out, "preview.jpg"), 92)
        return

    lin = scene(3840, 2160, a.seed)
    save(finish(lin, a.seed), os.path.join(a.out, "background-3840.jpg"), 92)

    # 1920 derivative: downsample in linear light (no moire), regrain.
    small = box_down(lin, 2)
    save(finish(small, a.seed + 1),
         os.path.join(a.out, "background-1920.jpg"), 93)

    # Sign-in backdrop: heavily blurred and dimmed, so the card and its
    # text sit on a calm field.  Pre-blurred because the greeter also runs
    # on software rendering, where a live CSS blur is expensive.
    del lin
    quarter = box_down(small, 4)                  # 480x270
    soft = blur2d(quarter, sigma=270 * 0.024)
    soft = resize_f(soft, 1920, 1080)
    lum = soft.mean(axis=-1, keepdims=True)
    soft = np.maximum(lum + (soft - lum) * 1.12, 0) * 0.74   # richer, dimmer
    save(finish(soft, a.seed + 2, grain=0.9),
         os.path.join(a.out, "background-blur.jpg"), 93)


if __name__ == "__main__":
    main()
