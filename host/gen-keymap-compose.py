#!/usr/bin/env python3
#
# gen-keymap-compose.py - the console's dead key tables, from the X compose
# table
#
# Usage: gen-keymap-compose.py COMPOSE KEYSYMDEF > kernel/dev/input/keymap_compose.h
#
#   COMPOSE    the X compose table of the UTF-8 locale
#              (build/xorg-sysroot/usr/share/X11/locale/en_US.UTF-8/Compose)
#   KEYSYMDEF  X11/keysymdef.h of the same tree, for keysym -> Unicode
#
# The console keymap engine (kernel/dev/input/keymap.c) composes a dead key
# with the next character through the accent table, which keymaps load with
# their compose lines.  That table holds 256 entries, while the dead keys of
# all the layouts together have well over a thousand sequences, so the engine
# also carries every two-key dead key sequence of the X compose table as a
# fixed fallback: a dead key followed by a letter then produces what an X
# program would produce, whatever the loaded table happens to hold.  This
# script writes that fallback, the bare-accent character of every dead key
# (dead key + space), the compiled-in default accent table, and the Unicode
# case pairs Caps Lock uses for letters outside Latin-1.
#
# The output is committed; this is how it is reproduced, not part of the
# build.
#
# Copyright (C) 2026 The LikeOS Project

import re
import sys

# The console's dead keys in symbol order (KT_DEAD value), with the X names
# that land on each and the character the accent table knows it by.
DEAD = [
    (["dead_grave"], "`"),
    (["dead_acute"], "'"),
    (["dead_circumflex"], "^"),
    (["dead_tilde", "dead_perispomeni"], "~"),
    (["dead_diaeresis"], '"'),
    (["dead_cedilla"], ","),
    (["dead_macron"], "_"),
    (["dead_breve"], "U"),
    (["dead_abovedot"], "."),
    (["dead_abovering"], "*"),
    (["dead_doubleacute"], "="),
    (["dead_caron"], "c"),
    (["dead_ogonek"], "k"),
    (["dead_iota"], "i"),
    (["dead_voiced_sound"], "#"),
    (["dead_semivoiced_sound"], "o"),
    (["dead_belowdot"], "!"),
    (["dead_hook"], "?"),
    (["dead_horn"], "+"),
    (["dead_stroke"], "-"),
    (["dead_abovecomma", "dead_psili"], ")"),
    (["dead_abovereversedcomma", "dead_dasia"], "("),
    (["dead_doublegrave"], ":"),
    (["dead_invertedbreve"], "n"),
    (["dead_belowcomma"], ";"),
    (["dead_currency"], "$"),
    (["dead_greek"], "@"),
]

# Spacing accents a keymap may use as a KT_DEAD2 dead key or in its compose
# lines, and the dead key each one stands for.
SPACING_ALIASES = {
    0x00B4: 1,   # acute accent
    0x00A8: 4,   # diaeresis
    0x00B8: 5,   # cedilla
    0x00AF: 6,   # macron
    0x02D8: 7,   # breve
    0x02D9: 8,   # dot above
    0x02DA: 9,   # ring above
    0x00B0: 9,   # degree sign (what dead ring + space gives)
    0x02DD: 10,  # double acute
    0x02C7: 11,  # caron
    0x02DB: 12,  # ogonek
    0x037A: 13,  # ypogegrammeni
    0x309B: 14,  # voiced sound mark
    0x309C: 15,  # semi-voiced sound mark
}


def keysyms(path):
    out = {}
    pat = re.compile(r"^#define XK_(\w+)\s+0x([0-9a-fA-F]+)\s*(/\*.*\*/)?")
    for line in open(path, encoding="utf-8", errors="replace"):
        m = pat.match(line)
        if not m:
            continue
        name, val, comment = m.group(1), int(m.group(2), 16), m.group(3) or ""
        u = re.search(r"U\+([0-9A-Fa-f]{4,6})", comment)
        if u:
            cp = int(u.group(1), 16)
        elif 0x20 <= val <= 0x7E or 0xA0 <= val <= 0xFF:
            cp = val
        elif val & 0xFF000000 == 0x01000000:
            cp = val & 0x00FFFFFF
        else:
            continue
        out.setdefault(name, cp)
    return out


def keysym_cp(name, ks):
    if name in ks:
        return ks[name]
    m = re.fullmatch(r"U([0-9A-Fa-f]{4,6})", name)
    if m:
        return int(m.group(1), 16)
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen-keymap-compose.py COMPOSE KEYSYMDEF")
    ks = keysyms(sys.argv[2])
    xname = {}
    for idx, (names, _) in enumerate(DEAD):
        for prio, n in enumerate(names):
            xname[n] = (idx, prio)

    seq = {}          # (dead index, base) -> (priority, result)
    spacing = {}      # dead index -> (priority, result)
    multi = {}        # (first, second) -> result, Latin-1 only
    pat = re.compile(r'^<(\w+)>\s+<(\w+)>\s*:\s*"((?:[^"\\]|\\.)+)"')
    pat3 = re.compile(r'^<Multi_key>\s+<(\w+)>\s+<(\w+)>\s*:\s*"((?:[^"\\]|\\.)+)"')
    for line in open(sys.argv[1], encoding="utf-8"):
        m3 = pat3.match(line)
        if m3:
            a = keysym_cp(m3.group(1), ks)
            b = keysym_cp(m3.group(2), ks)
            r = m3.group(3).encode("latin-1", "backslashreplace").decode("unicode_escape") \
                if "\\" in m3.group(3) else m3.group(3)
            if a is not None and b is not None and len(r) == 1 and \
                    a < 0x80 and b < 0x80 and 0xA0 <= ord(r) <= 0xFF:
                multi.setdefault((a, b), ord(r))
            continue
        m = pat.match(line)
        if not m or m.group(1) not in xname:
            continue
        idx, prio = xname[m.group(1)]
        res = m.group(3)
        if "\\" in res:
            res = res.encode("latin-1", "backslashreplace").decode("unicode_escape")
        if len(res) != 1:
            continue
        r = ord(res)
        if r > 0xFFFF:
            continue
        if m.group(2) == "space":
            if idx not in spacing or prio < spacing[idx][0]:
                spacing[idx] = (prio, r)
            continue
        base = keysym_cp(m.group(2), ks)
        if base is None or base > 0xFFFF or base < 0x20:
            continue
        key = (idx, base)
        if key not in seq or prio < seq[key][0]:
            seq[key] = (prio, r)

    # The default accent table: the six classic accents on the Latin-1
    # letters, their bare forms, and the two-letter Compose sequences that
    # make a Latin-1 character -- at most 256 entries.
    default = []
    seen = set()
    for idx in range(6):
        ch = ord(DEAD[idx][1])
        if (ch, 0x20) not in seen:
            default.append((ch, 0x20, spacing.get(idx, (0, ch))[1]))
            seen.add((ch, 0x20))
        for (i, base), (_, r) in sorted(seq.items()):
            if i == idx and base < 0x80 and r <= 0xFF and (ch, base) not in seen:
                default.append((ch, base, r))
                seen.add((ch, base))
    # A Compose pair that starts with a dead key's character would also
    # answer that dead key (dead caron + o must not type the copyright
    # sign of Compose c o), so those are left out.
    dead_chars = set(ord(ch) for _, ch in DEAD)
    for (a, b), r in sorted(multi.items()):
        if len(default) >= 255:
            break
        if a in dead_chars:
            continue
        if (a, b) not in seen:
            default.append((a, b, r))
            seen.add((a, b))

    w = sys.stdout.write
    w("// LikeOS console keymap engine - dead key tables\n")
    w("//\n")
    w("// GENERATED by host/gen-keymap-compose.py from the X compose table of the\n")
    w("// UTF-8 locale and X11/keysymdef.h; do not edit, regenerate.\n")
    w("//\n")
    w("// keymap_dead_char[]     the accent table character of each dead key\n")
    w("// keymap_dead_spacing[]  what a dead key followed by space types\n")
    w("// keymap_dead_seq[]      every two-key dead key sequence, sorted by\n")
    w("//                        (dead key, base): the fallback after the\n")
    w("//                        loaded accent table\n")
    w("// keymap_spacing_alias[] spacing accents that stand for a dead key\n")
    w("// keymap_default_accents[] the compiled-in accent table\n")
    w("// keymap_case_pairs[]    lower/upper case pairs, sorted by lower\n")
    w("//\n")
    w("// Copyright (C) 2026 The LikeOS Project\n\n")
    w("#ifndef _KERNEL_DEV_INPUT_KEYMAP_COMPOSE_H_\n")
    w("#define _KERNEL_DEV_INPUT_KEYMAP_COMPOSE_H_\n\n")
    w("static const uint8_t keymap_dead_char[NR_DEAD] = {\n")
    for idx, (names, ch) in enumerate(DEAD):
        w("\t'%s', // %s\n" % ("\\'" if ch == "'" else ch, names[0]))
    w("};\n\n")
    w("static const uint16_t keymap_dead_spacing[NR_DEAD] = {\n")
    for idx, (names, ch) in enumerate(DEAD):
        w("\t0x%04x, // %s\n" % (spacing.get(idx, (0, ord(ch)))[1], names[0]))
    w("};\n\n")
    w("struct keymap_dead_seq {\n\tuint8_t dead;\n\tuint16_t base;\n\tuint16_t result;\n};\n\n")
    w("static const struct keymap_dead_seq keymap_dead_seq[] = {\n")
    for (idx, base), (_, r) in sorted(seq.items()):
        w("\t{ %d, 0x%04x, 0x%04x },\n" % (idx, base, r))
    w("};\n\n")
    w("struct keymap_spacing_alias {\n\tuint16_t cp;\n\tuint8_t dead;\n};\n\n")
    w("static const struct keymap_spacing_alias keymap_spacing_alias[] = {\n")
    aliases = dict(SPACING_ALIASES)
    for idx in range(len(DEAD)):
        sp = spacing.get(idx)
        if sp and sp[1] >= 0x80 and sp[1] not in aliases:
            aliases[sp[1]] = idx
    for cp in sorted(aliases):
        w("\t{ 0x%04x, %d },\n" % (cp, aliases[cp]))
    w("};\n\n")
    w("static const struct kbdiacruc keymap_default_accents[] = {\n")
    for d, b, r in default:
        w("\t{ 0x%02x, 0x%02x, 0x%02x },\n" % (d, b, r))
    w("};\n\n")
    # Caps Lock and Unicode letters: a keymap marks its letters for Caps
    # Lock, but the mark only survives in the 16-bit symbol for Latin-1
    # (KT_LETTER).  For every other letter the engine recognises a case
    # pair -- the symbol and its other Shift level -- from this table of
    # simple lower -> upper mappings.
    pairs = []
    for cp in range(0x80, 0x10000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        up = chr(cp).upper()
        if len(up) == 1 and ord(up) != cp and ord(up) <= 0xFFFF and \
                up.lower() == chr(cp):
            pairs.append((cp, ord(up)))
    w("struct keymap_case_pair {\n\tuint16_t lower;\n\tuint16_t upper;\n};\n\n")
    w("static const struct keymap_case_pair keymap_case_pairs[] = {\n")
    for lo, up in pairs:
        w("\t{ 0x%04x, 0x%04x },\n" % (lo, up))
    w("};\n\n")
    w("#endif // _KERNEL_DEV_INPUT_KEYMAP_COMPOSE_H_\n")
    sys.stderr.write("%d sequences, %d spacing, %d default accents\n" %
                     (len(seq), len(spacing), len(default)))


main()
