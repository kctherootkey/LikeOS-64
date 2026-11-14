#!/usr/bin/env python3
# LikeOS -- rasterise the boot menu's font into a C header.
#
# The UEFI boot loader draws its menu with anti-aliased proportional text but
# carries no font engine.  This script renders the glyphs it needs ahead of
# time, at a range of pixel sizes, into 8-bit coverage bitmaps with their pen
# metrics and a pair-kerning table, and writes them as C data:
#
#     python3 host/gen-bootfont.py [--out boot/bootfont.h]
#
# The header is committed, so building the loader needs neither Python nor a
# font engine; rerun this after changing the glyph set, the sizes or the font.
#
# Faces: Selawik (the desktop's interface font, SIL Open Font License 1.1)
# regular and semibold.  Code points Selawik has no glyph for -- arrows and
# geometric shapes -- come from DejaVu Sans (regular / bold) at the same
# pixel size.  Which font has a glyph is decided from the font's own cmap
# table, read here directly.
#
# Kerning: Selawik keeps its kerning in OpenType GPOS, which only the complex
# text layout engine (raqm) applies; with it available the pair adjustments
# are measured with that engine (ligatures off), otherwise with the basic
# engine, which reads only a legacy 'kern' table.  Pairs under a quarter of a
# pixel are dropped.
#
# Output is deterministic: the same inputs give the same bytes.
#
# Copyright (C) 2026 The LikeOS Project

import argparse
import os
import struct
import sys

from PIL import Image, ImageFont, features

SELAWIK_DIR = "ports/xorg/gtk3/Selawik-1.01"
FONT_REGULAR = os.path.join(SELAWIK_DIR, "selawk.ttf")
FONT_SEMIBOLD = os.path.join(SELAWIK_DIR, "selawksb.ttf")
FALLBACK_REGULAR = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FALLBACK_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

SIZES_REGULAR = [14, 16, 20, 24, 30, 36, 44]
SIZES_SEMIBOLD = [20, 26, 32, 40, 52, 64]

EXTRA_CPS = [0x00A9, 0x00B7, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191,
             0x2192, 0x2193, 0x21B5, 0x25B2, 0x25BC]
CODEPOINTS = list(range(0x20, 0x7F)) + EXTRA_CPS

KERN_MIN64 = 16          # 1/4 pixel


# ---- TrueType tables -------------------------------------------------------

def sfnt_tables(data):
    (num,) = struct.unpack(">H", data[4:6])
    tables = {}
    for i in range(num):
        tag, _csum, off, length = struct.unpack(">4sIII",
                                                data[12 + 16 * i:28 + 16 * i])
        tables[tag.decode("latin-1")] = data[off:off + length]
    return tables


def cmap_codepoints(path):
    """The set of code points the font maps to a glyph other than 0."""
    with open(path, "rb") as f:
        tables = sfnt_tables(f.read())
    cmap = tables["cmap"]
    _ver, nsub = struct.unpack(">HH", cmap[:4])
    best = None
    for i in range(nsub):
        pid, eid, off = struct.unpack(">HHI", cmap[4 + 8 * i:12 + 8 * i])
        (fmt,) = struct.unpack(">H", cmap[off:off + 2])
        # Unicode subtables only; prefer the full-repertoire format 12.
        if not ((pid == 3 and eid in (1, 10)) or pid == 0):
            continue
        if fmt == 12 and (best is None or best[0] != 12):
            best = (12, off)
        elif fmt == 4 and best is None:
            best = (4, off)
    if best is None:
        raise SystemExit("%s: no Unicode cmap subtable" % path)
    fmt, off = best
    cps = set()
    if fmt == 4:
        (segx2,) = struct.unpack(">H", cmap[off + 6:off + 8])
        seg = segx2 // 2
        ends = struct.unpack(">%dH" % seg, cmap[off + 14:off + 14 + segx2])
        base = off + 16 + segx2
        starts = struct.unpack(">%dH" % seg, cmap[base:base + segx2])
        deltas = struct.unpack(">%dh" % seg,
                               cmap[base + segx2:base + 2 * segx2])
        ro_pos = base + 2 * segx2
        ranges = struct.unpack(">%dH" % seg, cmap[ro_pos:ro_pos + segx2])
        for s in range(seg):
            for c in range(starts[s], ends[s] + 1):
                if c == 0xFFFF:
                    continue
                if ranges[s] == 0:
                    gid = (c + deltas[s]) & 0xFFFF
                else:
                    p = ro_pos + 2 * s + ranges[s] + 2 * (c - starts[s])
                    (gid,) = struct.unpack(">H", cmap[p:p + 2])
                    if gid:
                        gid = (gid + deltas[s]) & 0xFFFF
                if gid:
                    cps.add(c)
    else:
        (ngroups,) = struct.unpack(">I", cmap[off + 12:off + 16])
        for g in range(ngroups):
            sc, ec, sg = struct.unpack(">III",
                                       cmap[off + 16 + 12 * g:off + 28 + 12 * g])
            for c in range(sc, ec + 1):
                if sg + (c - sc):
                    cps.add(c)
    return cps


def line_gap_px(path, px):
    with open(path, "rb") as f:
        tables = sfnt_tables(f.read())
    (upem,) = struct.unpack(">H", tables["head"][18:20])
    (gap,) = struct.unpack(">h", tables["hhea"][8:10])
    return max(0, int(round(gap * px / upem)))


# ---- rasterising -------------------------------------------------------------

def render_glyph(font, ch):
    """(w, h, left, top, coverage bytes) with a tight bounding box."""
    mask, (ox, oy) = font.getmask2(ch, mode="L", anchor="ls")
    w, h = mask.size
    if w == 0 or h == 0:
        return 0, 0, 0, 0, b""
    img = Image.frombytes("L", (w, h), bytes(mask))
    bbox = img.getbbox()
    if bbox is None:
        return 0, 0, 0, 0, b""
    x0, y0, x1, y1 = bbox
    img = img.crop(bbox)
    return x1 - x0, y1 - y0, ox + x0, -(oy + y0), img.tobytes()


def kern_pairs(path, px):
    """{(a, b): adj64} for ASCII pairs whose spacing the font adjusts."""
    if features.check("raqm"):
        font = ImageFont.truetype(path, px, layout_engine=ImageFont.Layout.RAQM)
        feats = ["kern", "-liga", "-clig", "-calt", "-dlig"]

        def length(s):
            return font.getlength(s, features=feats)
    else:
        font = ImageFont.truetype(path, px,
                                  layout_engine=ImageFont.Layout.BASIC)

        def length(s):
            return font.getlength(s)
    chars = [chr(c) for c in range(0x21, 0x7F)]
    single = {c: length(c) for c in chars}
    pairs = {}
    for a in chars:
        for b in chars:
            k = int(round((length(a + b) - single[a] - single[b]) * 64))
            if abs(k) >= KERN_MIN64:
                pairs[(ord(a), ord(b))] = k
    return pairs


def build_face(path, fallback, px, weight, have_cps, fallback_used):
    font = ImageFont.truetype(path, px, layout_engine=ImageFont.Layout.BASIC)
    fb = ImageFont.truetype(fallback, px, layout_engine=ImageFont.Layout.BASIC)
    ascent, descent = font.getmetrics()
    glyphs = []
    cov = bytearray()
    for cp in CODEPOINTS:
        ch = chr(cp)
        f = font
        if cp not in have_cps:
            f = fb
            fallback_used.add(cp)
        w, h, left, top, data = render_glyph(f, ch)
        adv64 = int(round(f.getlength(ch) * 64))
        glyphs.append((cp, w, h, left, top, adv64, len(cov)))
        cov += data
    kern = kern_pairs(path, px)
    return {
        "px": px,
        "weight": weight,
        "ascent": ascent,
        "descent": descent,
        "line": ascent + descent + line_gap_px(path, px),
        "glyphs": glyphs,
        "cov": bytes(cov),
        "kern": sorted(kern.items()),
    }


# ---- output -----------------------------------------------------------------

def face_id(face):
    return "%s%d" % ("r" if face["weight"] == 0 else "s", face["px"])


def emit(faces, fallback_cps, out):
    L = []
    fb_list = " ".join("U+%04X" % c for c in sorted(fallback_cps))
    L.append("/* Generated by host/gen-bootfont.py -- do not edit.")
    L.append(" *")
    L.append(" * The boot menu's text faces (boot/gfx.c): 8-bit anti-aliased coverage")
    L.append(" * bitmaps with pen metrics and pair kerning.  Glyphs rasterised from")
    L.append(" * Selawik (SIL Open Font License 1.1, see")
    L.append(" * res/xorg/gtk3/fonts/Selawik-LICENSE.txt)%s"
             % (" and DejaVu Sans for" if fb_list else "."))
    if fb_list:
        words = fb_list.split(" ")
        line = " *"
        for w in words:
            if len(line) + len(w) + 1 > 76:
                L.append(line)
                line = " *"
            line += " " + w
        L.append(line + ".")
    L.append(" *")
    L.append(" * Copyright (C) 2026 The LikeOS Project")
    L.append(" */")
    L.append("")
    L.append("#ifndef LIKEOS_BOOT_BOOTFONT_H")
    L.append("#define LIKEOS_BOOT_BOOTFONT_H")
    L.append("")
    L.append("struct bootfont_glyph {")
    L.append("\tunsigned int cp;        /* Unicode code point */")
    L.append("\tunsigned short w, h;    /* coverage bitmap size in pixels */")
    L.append("\tshort left;             /* bitmap left edge relative to the pen */")
    L.append("\tshort top;              /* bitmap top row, pixels above the baseline */")
    L.append("\tunsigned short adv64;   /* advance width, 1/64 pixel */")
    L.append("\tunsigned int off;       /* first coverage byte in the face's cov[] */")
    L.append("};")
    L.append("")
    L.append("/* Pair a,b (ASCII): add adj64/64 px between them. */")
    L.append("struct bootfont_kern {")
    L.append("\tunsigned char a, b;")
    L.append("\tshort adj64;")
    L.append("};")
    L.append("")
    L.append("struct bootfont_face {")
    L.append("\tunsigned short px;      /* pixel size (em) */")
    L.append("\tunsigned char weight;   /* 0 regular, 1 semibold */")
    L.append("\tshort ascent;           /* pixels above the baseline */")
    L.append("\tshort descent;          /* pixels below the baseline (positive) */")
    L.append("\tunsigned short line;    /* line height */")
    L.append("\tunsigned short nglyphs;")
    L.append("\tunsigned short nkern;")
    L.append("\tconst struct bootfont_glyph *glyphs;   /* ascending cp */")
    L.append("\tconst unsigned char *cov;              /* 8-bit coverage, row-major */")
    L.append("\tconst struct bootfont_kern *kern;      /* ascending (a,b) */")
    L.append("};")
    for face in faces:
        fid = face_id(face)
        L.append("")
        L.append("/* %s %d px */" % ("Regular" if face["weight"] == 0
                                    else "Semibold", face["px"]))
        cov = face["cov"]
        L.append("static const unsigned char bootfont_cov_%s[%d] = {"
                 % (fid, max(1, len(cov))))
        if not cov:
            cov = b"\x00"
        for i in range(0, len(cov), 16):
            L.append("\t" + ",".join("%d" % b for b in cov[i:i + 16]) + ",")
        L.append("};")
        L.append("static const struct bootfont_glyph bootfont_glyphs_%s[%d] = {"
                 % (fid, len(face["glyphs"])))
        for cp, w, h, left, top, adv64, off in face["glyphs"]:
            L.append("\t{ 0x%04x, %d, %d, %d, %d, %d, %d }," %
                     (cp, w, h, left, top, adv64, off))
        L.append("};")
        if face["kern"]:
            L.append("static const struct bootfont_kern bootfont_kern_%s[%d] = {"
                     % (fid, len(face["kern"])))
            items = ["{ %d, %d, %d }" % (a, b, k) for (a, b), k in face["kern"]]
            for i in range(0, len(items), 6):
                L.append("\t" + ", ".join(items[i:i + 6]) + ",")
            L.append("};")
    L.append("")
    L.append("#define BOOTFONT_NFACES %d" % len(faces))
    L.append("")
    L.append("/* Regular by ascending size, then semibold by ascending size. */")
    L.append("static const struct bootfont_face bootfont_faces[BOOTFONT_NFACES] = {")
    for face in faces:
        fid = face_id(face)
        kern = "bootfont_kern_%s" % fid if face["kern"] else "0"
        L.append("\t{ %d, %d, %d, %d, %d, %d, %d, bootfont_glyphs_%s, "
                 "bootfont_cov_%s, %s }," %
                 (face["px"], face["weight"], face["ascent"], face["descent"],
                  face["line"], len(face["glyphs"]), len(face["kern"]),
                  fid, fid, kern))
    L.append("};")
    L.append("")
    L.append("#endif /* LIKEOS_BOOT_BOOTFONT_H */")
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="boot/bootfont.h")
    args = ap.parse_args()
    faces = []
    fallback_cps = set()
    have_regular = cmap_codepoints(FONT_REGULAR)
    have_semibold = cmap_codepoints(FONT_SEMIBOLD)
    for px in SIZES_REGULAR:
        faces.append(build_face(FONT_REGULAR, FALLBACK_REGULAR, px, 0,
                                have_regular, fallback_cps))
    for px in SIZES_SEMIBOLD:
        faces.append(build_face(FONT_SEMIBOLD, FALLBACK_BOLD, px, 1,
                                have_semibold, fallback_cps))
    emit(faces, fallback_cps, args.out)
    total = sum(len(f["cov"]) for f in faces)
    print("%s: %d faces, %d glyphs each, %d coverage bytes, kerning pairs %s"
          % (args.out, len(faces), len(CODEPOINTS), total,
             [len(f["kern"]) for f in faces]))
    if fallback_cps:
        print("from DejaVu Sans: " +
              " ".join("U+%04X" % c for c in sorted(fallback_cps)))


if __name__ == "__main__":
    sys.exit(main())
