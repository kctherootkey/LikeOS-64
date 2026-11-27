#!/usr/bin/env python3
# Expectations for the Intel pin controller tables, taken straight from
# the published Intel sources (not through the generated kernel tables),
# for host/pinctrl/test_pinctrl.c to cross-check against.
#
#   gen-expect.py SRCDIR OUT.h soc [soc ...]
#
# For every ACPI id (and _UID) the header lists the communities with
# their register offsets as the community macros expand them, the pad
# groups as written, and the first and last pin of the pin list.  The
# parser works on the source text only: defines are evaluated, the
# per-SoC community macros expanded by their parameters.
#
# Copyright (C) 2026 The LikeOS Project

import re
import sys

SPECIAL = {'INTEL_GPIO_BASE_NOMAP': -1, 'INTEL_GPIO_BASE_ZERO': -2,
           'INTEL_GPIO_BASE_MATCH': 0}


def strip_comments(t):
    return re.sub(r'/\*.*?\*/', '', t, flags=re.S)


def blocks(text, kind):
    pat = re.compile(r'static const struct ' + kind +
                     r'\s+(\*\s*(?:const\s+)?)?([a-z0-9_]+)(\[\])?\s*=\s*\{')
    for m in pat.finditer(text):
        end = text.index('\n};', m.end())
        yield m.group(2), bool(m.group(1)), text[m.end():end]


def parse(src):
    raw = open(src).read()
    text = strip_comments(raw)
    defs = {}
    macros = {}
    for m in re.finditer(r'#define\s+([A-Z0-9_]+)\s+(0[xX][0-9a-fA-F]+|\d+)\s*$',
                         text, re.M):
        defs[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r'#define\s+([A-Z0-9_]+_COMMUNITY)\(([^)]*)\)\s*\\\s*\n\s*'
                         r'(INTEL_COMMUNITY_GPPS|INTEL_COMMUNITY_SIZE)\(([^)]*)\)',
                         text):
        params = [p.strip() for p in m.group(2).split(',')]
        body = [a.strip() for a in m.group(4).split(',')]
        macros[m.group(1)] = (params, m.group(3), body)
    gpps = {}
    for name, ptr, body in blocks(text, 'intel_padgroup'):
        lst = []
        for g in re.finditer(r'INTEL_GPP\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*([A-Z_0-9]+)\s*\)',
                             body):
            gb = g.group(4)
            gb = SPECIAL[gb] if gb in SPECIAL else int(gb, 0)
            lst.append((int(g.group(1)), int(g.group(2)),
                        int(g.group(3)) - int(g.group(2)) + 1, gb))
        gpps[name] = lst
    comms = {}
    for name, ptr, body in blocks(text, 'intel_community'):
        lst = []
        for c in re.finditer(r'([A-Z0-9_]+_COMMUNITY)\(([^)]*)\)', body):
            params, kind, mbody = macros[c.group(1)]
            args = [a.strip() for a in c.group(2).split(',')]
            env = dict(zip(params, args))
            ex = [env.get(a, a) for a in mbody]
            b, s, e = int(ex[0]), int(ex[1]), int(ex[2])
            if kind == 'INTEL_COMMUNITY_GPPS':
                g, soc = ex[3], ex[4]
                gs = gn = 0
                gl = gpps[g]
            else:
                gs, gn, soc = int(ex[3]), int(ex[4]), ex[5]
                gl = []
            off = [defs[soc + suf] for suf in ('_PAD_OWN', '_PADCFGLOCK',
                                               '_HOSTSW_OWN', '_GPI_IS',
                                               '_GPI_IE')]
            lst.append(dict(barno=b, pin_base=s, npins=e - s + 1, off=off,
                            gs=gs, gn=gn, gpps=gl))
        comms[name] = lst
    pins = {}
    for name, ptr, body in blocks(text, 'pinctrl_pin_desc'):
        pl = re.findall(r'PINCTRL_PIN\(\s*(\d+)\s*,\s*"([^"]*)"\s*\)', body)
        pins[name] = [(int(n), s) for n, s in pl]
    socs = {}
    for name, ptr, body in blocks(text, 'intel_pinctrl_soc_data'):
        if ptr:
            continue
        uid = re.search(r'\.uid\s*=\s*"([^"]*)"', body)
        p = re.search(r'\.pins\s*=\s*([a-z0-9_]+)', body).group(1)
        c = re.search(r'\.communities\s*=\s*([a-z0-9_]+)', body).group(1)
        socs[name] = dict(uid=uid.group(1) if uid else None, pins=pins[p],
                          comms=comms[c])
    tables = {}
    for name, ptr, body in blocks(text, 'intel_pinctrl_soc_data'):
        if ptr:
            tables[name] = re.findall(r'&([a-z0-9_]+)', body)
    ids = []
    for name, ptr, body in blocks(text, 'acpi_device_id'):
        for m in re.finditer(r'\{\s*"([A-Z0-9]+)",\s*\(kernel_ulong_t\)(&?)([a-z0-9_]+)\s*\}',
                             body):
            if m.group(2):
                ids.append((m.group(1), [m.group(3)]))
            else:
                ids.append((m.group(1), tables[m.group(3)]))
    return ids, socs


def emit(out, socname, hid, s, n):
    cname = 'x_%d' % n
    lines = []
    for ci, c in enumerate(s['comms']):
        gl = ', '.join('{%d, %d, %d, %d}' % g for g in c['gpps'])
        lines.append('static const struct exp_gpp %s_g%d[] = { %s };' %
                     (cname, ci, gl if gl else '{0, 0, 0, 0}'))
    lines.append('static const struct exp_comm %s_c[] = {' % cname)
    for ci, c in enumerate(s['comms']):
        lines.append('\t{ %d, { %s }, %d, %d, %d, %d, %s_g%d, %d },' % (
            c['barno'], ', '.join('0x%x' % o for o in c['off']),
            c['pin_base'], c['npins'], c['gs'], c['gn'], cname, ci,
            len(c['gpps'])))
    lines.append('};')
    p = s['pins']
    out.append('\n'.join(lines))
    return ('\t{ "%s", %s, "%s", %d, %d, "%s", %d, "%s", %s_c, %d },' % (
        hid, '"%s"' % s['uid'] if s['uid'] else 'NULL', socname, len(p),
        p[0][0], p[0][1], p[-1][0], p[-1][1], cname, len(s['comms'])))


def main():
    if len(sys.argv) < 4:
        raise SystemExit('usage: gen-expect.py SRCDIR OUT.h soc...')
    out = ['/* Generated by host/pinctrl/gen-expect.py: the Intel pin',
           ' * controller tables as the published sources state them. */',
           '#ifndef HOST_PINCTRL_EXPECT_TABLES_H',
           '#define HOST_PINCTRL_EXPECT_TABLES_H', '',
           'struct exp_gpp { unsigned int reg, base, size; int gpio_base; };',
           'struct exp_comm { unsigned int barno; unsigned int off[5];',
           '\tunsigned int pin_base, npins, gpp_size, gpp_npadown;',
           '\tconst struct exp_gpp *gpps; unsigned int ngpps; };',
           'struct exp_soc { const char *hid, *uid, *soc; unsigned int npins;',
           '\tunsigned int first_pin; const char *first_name;',
           '\tunsigned int last_pin; const char *last_name;',
           '\tconst struct exp_comm *comms; unsigned int ncomms; };', '']
    rows = []
    n = 0
    for soc in sys.argv[3:]:
        ids, socs = parse('%s/pinctrl-%s.c' % (sys.argv[1], soc))
        for hid, names in ids:
            for sn in names:
                rows.append(emit(out, sn, hid, socs[sn], n))
                n += 1
    out.append('')
    out.append('static const struct exp_soc g_expect[] = {')
    out.extend(rows)
    out.append('};')
    out.append('')
    out.append('#endif')
    text = '\n'.join(out) + '\n'
    for bad in ('xunil'[::-1], 'xuniL'[::-1], 'XUNIL'[::-1]):
        if bad in text:
            raise SystemExit('forbidden word in output')
    open(sys.argv[2], 'w').write(text)


if __name__ == '__main__':
    main()
