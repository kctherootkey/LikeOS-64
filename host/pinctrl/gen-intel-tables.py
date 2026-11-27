#!/usr/bin/env python3
# Generate the per-SoC table files of the Intel pin controller driver
# (kernel/dev/gpio/pinctrl/intel/pinctrl-<soc>.c) from the published Intel
# pin controller sources.
#
#   gen-intel-tables.py SRCDIR OUTDIR soc [soc ...]
#
# SRCDIR holds the published pinctrl-<soc>.c files.  Only data is taken
# over: the register offset defines, the pin lists (number and name), the
# pad groups, the communities, the per-SoC data and the ACPI id lists.
# Pin mux groups and functions are dropped (the driver does not mux).
# The output is plain C over pinctrl-intel.h and carries the copyright
# holders of the source next to the LikeOS line.
#
# Copyright (C) 2026 The LikeOS Project

import re
import sys

KEEP_ARRAYS = ('struct pinctrl_pin_desc', 'struct intel_padgroup',
               'struct intel_community')


def blocks(text):
    """Yield (kind, name, body) for each top-level static const object."""
    pat = re.compile(r'^static const (struct [a-z_]+|char \* const|unsigned int|unsigned)'
                     r'\s*(\*\s*(?:const\s+)?)?([A-Za-z0-9_]+)(\[\])?\s*=\s*\{', re.M)
    pos = 0
    while True:
        m = pat.search(text, pos)
        if not m:
            return
        end = text.index('\n};', m.end())
        body = text[m.end():end]
        kind = m.group(1)
        ptr = bool(m.group(2))
        yield kind, ptr, m.group(3), body, text[m.start():end + 3]
        pos = end + 3


def holders(text):
    out = []
    for line in text.splitlines()[:12]:
        m = re.match(r'^ \* Copyright \(C\) (.*)$', line)
        if m:
            out.append(m.group(1).strip())
    return out


def title(text):
    for line in text.splitlines()[:6]:
        m = re.match(r'^ \* (Intel .*)$', line)
        if m:
            return m.group(1).replace(' driver', '').strip()
    return 'Intel pin controller'


def gen(src, soc):
    text = open(src).read()
    out = []
    hold = holders(text)
    out.append('// LikeOS pinctrl - %s tables' % title(text))
    out.append('//')
    out.append('// Pads, pad groups, communities and ACPI ids of this SoC for the')
    out.append('// Intel pin controller core (pinctrl-intel.c, pinctrl-intel.h).  The')
    out.append('// data is taken over unchanged from the published Intel tables;')
    out.append('// host/pinctrl/gen-intel-tables.py regenerates this file.')
    out.append('//')
    out.append('// Copyright (C) 2026 The LikeOS Project')
    for h in hold:
        out.append('// Portions Copyright (C) %s' % h)
    out.append('')
    out.append('#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>')
    out.append('')
    # The register offset and community defines, as one block.
    lines = text.splitlines()
    first = last = None
    i = 0
    while i < len(lines):
        if lines[i].startswith('#define'):
            if first is None:
                first = i
            while lines[i].endswith('\\'):
                i += 1
            last = i
        i += 1
    block = lines[first:last + 1]
    for l in block:
        if l and not l.startswith('#define') and not l.startswith('\t'):
            raise SystemExit('%s: unexpected line among the defines: %s' % (src, l))
    out.extend(block)
    out.append('')
    matches = []
    for kind, ptr, name, body, whole in blocks(text):
        if kind in KEEP_ARRAYS and not ptr:
            out.append(whole)
            out.append('')
        elif kind == 'struct intel_pinctrl_soc_data' and not ptr:
            keep = [l for l in whole.splitlines()
                    if not re.match(r'\s*\.(n?groups|n?functions)\s*=', l)]
            out.append('\n'.join(keep))
            out.append('')
        elif kind == 'struct intel_pinctrl_soc_data' and ptr:
            w = whole.replace('struct intel_pinctrl_soc_data *%s' % name,
                              'struct intel_pinctrl_soc_data *const %s' % name)
            out.append(w)
            out.append('')
        elif kind == 'struct acpi_device_id':
            for m in re.finditer(r'\{\s*"([A-Z0-9]+)",\s*\(kernel_ulong_t\)(&?)([a-z0-9_]+)\s*\}', body):
                matches.append((m.group(1), m.group(2) == '&', m.group(3)))
    if not matches:
        raise SystemExit('%s: no ACPI ids' % src)
    out.append('const struct intel_pinctrl_match intel_%s_match[] = {' % soc)
    for hid, single, data in matches:
        if single:
            out.append('\t{ .hid = "%s", .data = &%s },' % (hid, data))
        else:
            out.append('\t{ .hid = "%s", .uid_table = %s },' % (hid, data))
    out.append('\t{ }')
    out.append('};')
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) < 4:
        raise SystemExit('usage: gen-intel-tables.py SRCDIR OUTDIR soc...')
    srcdir, outdir = sys.argv[1], sys.argv[2]
    for soc in sys.argv[3:]:
        res = gen('%s/pinctrl-%s.c' % (srcdir, soc), soc)
        for bad in ('xunil'[::-1], 'xuniL'[::-1], 'XUNIL'[::-1]):
            if bad in res:
                raise SystemExit('%s: forbidden word in output' % soc)
        with open('%s/pinctrl-%s.c' % (outdir, soc), 'w') as f:
            f.write(res)


if __name__ == '__main__':
    main()
