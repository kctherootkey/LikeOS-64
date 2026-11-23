#!/usr/bin/env python3
# Strict NTFS consistency checker for the NTFS driver's host tests.
#
# ntfsfix -n only looks at $MFTMirr, the boot sectors and the volume flags.
# This checker walks the whole volume the way a full check would and
# reports, for an image file:
#
#   ERROR  anything a later read could trip over: MFT records that fail
#          their update sequence check or header rules, attributes that
#          break the format (bounds, residency of resident-only types,
#          sizes, extents that do not continue each other, mapping pairs
#          outside the volume), attribute lists that do not match the
#          records (also an attribute of an extension record the list
#          does not name), clusters used twice or used but free in $Bitmap,
#          records in use but free in $MFT:$BITMAP, index blocks that fail
#          their checks, entries out of collation order, entries pointing
#          to records that are not in use or have no such $FILE_NAME,
#          names in a record without their directory entry, an index block
#          referenced but free in the index bitmap, $MFTMirr differing
#          from $MFT;
#   LEAK   what a power cut is allowed to leave behind: clusters / MFT
#          records / index blocks marked in use that nothing references,
#          orphan records (in use, no name), names in a record that their
#          directory does not list, link counts that are too high, a
#          $MFTMirr record one change behind $MFT (valid older copy);
#   INFO   statistics.
#
#   test-ntfs-check.py IMAGE [--strict] [--quiet]
#
# Exit status: 0 clean (leaks allowed unless --strict), 1 errors (or
# leaks with --strict), 2 usage / unreadable image.
#
# Copyright (C) 2026 The LikeOS Project

import struct
import sys

ERRORS = []
LEAKS = []
INFO = []


def err(msg):
    ERRORS.append(msg)


def leak(msg):
    LEAKS.append(msg)


AT_SI, AT_AL, AT_FN, AT_OBJID, AT_SD = 0x10, 0x20, 0x30, 0x40, 0x50
AT_VOLNAME, AT_VOLINFO, AT_DATA, AT_IR, AT_IA = 0x60, 0x70, 0x80, 0x90, 0xA0
AT_BITMAP, AT_REPARSE, AT_EAINFO, AT_EA, AT_END = 0xB0, 0xC0, 0xD0, 0xE0, 0xFFFFFFFF
RESIDENT_ONLY = {AT_SI, AT_FN, AT_OBJID, AT_VOLNAME, AT_VOLINFO, AT_IR, AT_EAINFO}


def le16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def le32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def le64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def sle64(b, o):
    return struct.unpack_from('<q', b, o)[0]


class Vol:
    def __init__(self, path):
        self.f = open(path, 'rb')
        boot = self.f.read(512)
        if boot[3:11] != b'NTFS    ':
            raise SystemExit('not NTFS')
        self.bps = le16(boot, 0x0B)
        spc = boot[0x0D]
        self.cs = self.bps * spc if spc < 0xF4 else self.bps << (256 - spc)
        self.nclusters = le64(boot, 0x28) // (self.cs // self.bps)
        self.mft_lcn = le64(boot, 0x30)
        self.mirr_lcn = le64(boot, 0x38)
        c = struct.unpack_from('<b', boot, 0x40)[0]
        self.rs = c * self.cs if c > 0 else 1 << -c
        c = struct.unpack_from('<b', boot, 0x44)[0]
        self.ibs = c * self.cs if c > 0 else 1 << -c

    def read(self, off, n):
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) < n:
            d += b'\0' * (n - len(d))
        return d

    def read_rl(self, rl, off, n):
        out = bytearray()
        while n > 0:
            vcn = off // self.cs
            inn = off % self.cs
            lcn = None
            for (v, l, ln) in rl:
                if v <= vcn < v + ln:
                    lcn = None if l is None else l + (vcn - v)
                    left = v + ln - vcn
                    break
            else:
                raise ValueError('unmapped vcn %d' % vcn)
            take = min(n, left * self.cs - inn)
            if lcn is None:
                out += b'\0' * take
            else:
                out += self.read(lcn * self.cs + inn, take)
            off += take
            n -= take
        return bytes(out)


def fixup(buf, size, what):
    b = bytearray(buf)
    usa = le16(b, 4)
    cnt = le16(b, 6)
    strides = size // 512
    if cnt != strides + 1 or usa & 1 or usa + 2 * cnt > 510:
        return None, '%s: bad update sequence array (ofs %d count %d)' % (what, usa, cnt)
    usn = le16(b, usa)
    for i in range(strides):
        if le16(b, (i + 1) * 512 - 2) != usn:
            return None, '%s: torn (sector %d)' % (what, i)
    for i in range(strides):
        b[(i + 1) * 512 - 2:(i + 1) * 512] = b[usa + 2 + 2 * i:usa + 4 + 2 * i]
    return bytes(b), None


def decode_mp(b, off, end, lowest, nclusters, what):
    runs = []
    vcn = lowest
    lcn = 0
    p = off
    while True:
        if p >= end:
            err('%s: mapping pairs without terminator' % what)
            return None
        h = b[p]
        if h == 0:
            break
        lb = h & 15
        cb = h >> 4
        if lb == 0 or lb > 8 or cb > 8 or p + 1 + lb + cb > end:
            err('%s: bad mapping pair header' % what)
            return None
        ln = int.from_bytes(b[p + 1:p + 1 + lb], 'little', signed=True)
        if ln <= 0:
            err('%s: run length %d' % (what, ln))
            return None
        if cb:
            d = int.from_bytes(b[p + 1 + lb:p + 1 + lb + cb], 'little', signed=True)
            lcn += d
            if lcn < 0 or lcn + ln > nclusters:
                err('%s: run outside the volume (lcn %d len %d)' % (what, lcn, ln))
                return None
            runs.append((vcn, lcn, ln))
        else:
            runs.append((vcn, None, ln))
        vcn += ln
        p += 1 + lb + cb
    return runs, vcn


def parse_attrs(rec, no, rs, nclusters):
    """Validated attributes of a record: list of dicts."""
    out = []
    ao = le16(rec, 0x14)
    used = le32(rec, 0x18)
    nxt = le16(rec, 0x28)
    p = ao
    insts = set()
    last_key = None
    while True:
        if p + 4 > used:
            err('MFT %d: attributes run past bytes_in_use' % no)
            return out
        t = le32(rec, p)
        if t == AT_END:
            if p + 8 != used:
                err('MFT %d: end marker at 0x%x but bytes_in_use 0x%x' % (no, p, used))
            break
        ln = le32(rec, p + 4)
        if ln < 16 or ln & 7 or p + ln > used:
            err('MFT %d: attribute at 0x%x bad length %d' % (no, p, ln))
            return out
        nr = rec[p + 8]
        nl = rec[p + 9]
        no_ = le16(rec, p + 10)
        flags = le16(rec, p + 12)
        inst = le16(rec, p + 14)
        if inst in insts:
            err('MFT %d: duplicate attribute instance %d' % (no, inst))
        insts.add(inst)
        if inst >= nxt:
            err('MFT %d: instance %d >= next_attr_instance %d' % (no, inst, nxt))
        if nl and (no_ + 2 * nl > ln):
            err('MFT %d: attribute name outside' % no)
        name = rec[p + no_:p + no_ + 2 * nl] if nl else b''
        a = {'type': t, 'name': name, 'flags': flags, 'inst': inst, 'rec': no,
             'nonres': nr, 'raw': rec[p:p + ln]}
        if nr == 0:
            vl = le32(rec, p + 16)
            vo = le16(rec, p + 20)
            if vo < 24 or vo + vl > ln:
                err('MFT %d: resident value outside (type 0x%x)' % (no, t))
                return out
            a['value'] = rec[p + vo:p + vo + vl]
            a['lowest'] = 0
        elif nr == 1:
            if t in RESIDENT_ONLY:
                err('MFT %d: non-resident attribute of resident-only type 0x%x' % (no, t))
            low = sle64(rec, p + 16)
            high = sle64(rec, p + 24)
            mpo = le16(rec, p + 32)
            hdr = 72 if (flags & 0x80FF) and low == 0 else 64
            if ln < hdr or mpo < 64 or mpo > ln or (nl and (no_ < hdr or no_ + 2 * nl > mpo)):
                err('MFT %d: non-resident header bad (type 0x%x)' % (no, t))
                return out
            a['lowest'] = low
            a['highest'] = high
            if low == 0:
                a['alloc'] = sle64(rec, p + 40)
                a['data'] = sle64(rec, p + 48)
                a['init'] = sle64(rec, p + 56)
                a['comp'] = sle64(rec, p + 64) if hdr == 72 else None
                if not (0 <= a['init'] <= a['data'] <= a['alloc']) or a['alloc'] % 512:
                    err('MFT %d: type 0x%x sizes alloc %d data %d init %d' %
                        (no, t, a['alloc'], a['data'], a['init']))
            res = decode_mp(rec, p + mpo, p + ln, low, nclusters, 'MFT %d type 0x%x' % (no, t))
            if res is None:
                return out
            a['runs'], endv = res
            if endv != high + 1:
                err('MFT %d: type 0x%x extent ends at %d, highest_vcn %d' % (no, t, endv, high))
        else:
            err('MFT %d: bad residency %d' % (no, nr))
            return out
        key = (t, name.decode('utf-16le', 'replace').upper(), name, a['lowest'])
        if last_key and key[:1] < last_key[:1]:
            err('MFT %d: attributes out of order' % no)
        last_key = key
        out.append(a)
        p += ln
    return out


def upcase_table(vol, rl, size):
    data = vol.read_rl(rl, 0, size)
    return struct.unpack('<%dH' % (size // 2), data)


def main():
    if len(sys.argv) < 2:
        print(__doc__ if __doc__ else 'usage: test-ntfs-check.py IMAGE [--strict]')
        return 2
    strict = '--strict' in sys.argv
    quiet = '--quiet' in sys.argv
    vol = Vol(sys.argv[1])
    cs, rs = vol.cs, vol.rs
    # $MFT from record 0
    raw0 = vol.read(vol.mft_lcn * cs, rs)
    rec0, e = fixup(raw0, rs, 'MFT 0')
    if e:
        err(e)
        return finish(strict, quiet)
    attrs0 = parse_attrs(rec0, 0, rs, vol.nclusters)
    mft_rl = []
    mft_size = 0
    for a in attrs0:
        if a['type'] == AT_DATA and not a['name'] and a['nonres']:
            mft_rl += a['runs']
            if a['lowest'] == 0:
                mft_size = a['data']
                mft_init = a['init']
    has_al0 = any(a['type'] == AT_AL for a in attrs0)
    if has_al0:
        INFO.append('$MFT has an attribute list (extents in other records are read through it)')
        # a fragmented $MFT: the $DATA / $BITMAP extents in extension
        # records (named by the list; they live in records the extents
        # of record 0 map)
        a = [x for x in attrs0 if x['type'] == AT_AL][0]
        alb = vol.read_rl(a['runs'], 0, a['data']) if a['nonres'] else a['value']
        p = 0
        while p + 26 <= len(alb):
            t = le32(alb, p)
            ln = le16(alb, p + 4)
            if ln < 26 or ln & 7:
                break
            nl = alb[p + 6]
            rno = le64(alb, p + 16) & 0xFFFFFFFFFFFF
            if rno != 0 and nl == 0 and t in (AT_DATA, AT_BITMAP):
                try:
                    xr, e = fixup(vol.read_rl(mft_rl, rno * rs, rs), rs, 'MFT %d' % rno)
                except ValueError:
                    xr, e = None, 'MFT %d: $MFT extension record not mapped' % rno
                if e:
                    err(e)
                else:
                    for x in parse_attrs(xr, rno, rs, vol.nclusters):
                        if x['type'] == t and not x['name']:
                            attrs0.append(x)
                            if t == AT_DATA and x['nonres']:
                                mft_rl += x['runs']
                                if x['lowest'] == 0:
                                    mft_size = x['data']
                                    mft_init = x['init']
            p += ln
        mft_rl.sort()
    nrec = mft_size // rs
    mftdata = None
    records = {}
    raw_records = {}
    for no in range(nrec):
        off = no * rs
        if off >= mft_init:
            break
        try:
            raw = vol.read_rl(mft_rl, off, rs)
        except ValueError:
            if not has_al0:
                err('MFT %d: not mapped by $MFT' % no)
            break
        raw_records[no] = raw
        if raw[:4] != b'FILE':
            if raw.strip(b'\0'):
                if raw[:4] == b'BAAD':
                    err('MFT %d: BAAD record' % no)
            continue
        rec, e = fixup(raw, rs, 'MFT %d' % no)
        if e:
            err(e)
            continue
        flags = le16(rec, 0x16)
        if le32(rec, 0x1C) != rs or le32(rec, 0x18) > rs:
            err('MFT %d: header sizes' % no)
            continue
        records[no] = rec
    # $MFT:$BITMAP
    mftbm = None
    for a in attrs0:
        if a['type'] == AT_BITMAP and not a['name']:
            if a['nonres']:
                if a['lowest'] == 0:
                    mftbm = vol.read_rl(a['runs'], 0, a['data'])
            else:
                mftbm = a['value']
    if mftbm is None:
        err('no $MFT:$BITMAP')
        return finish(strict, quiet)
    in_use = {}
    for no, rec in records.items():
        flags = le16(rec, 0x16)
        if flags & 1:
            in_use[no] = rec
    for no in range(nrec):
        bit = (mftbm[no // 8] >> (no % 8)) & 1 if no // 8 < len(mftbm) else 0
        if no in in_use and not bit:
            err('MFT %d: in use but free in $MFT:$BITMAP' % no)
        if bit and no not in in_use and no >= 24:
            leak('MFT %d: marked in $MFT:$BITMAP, not in use' % no)
    # $MFTMirr
    mirr = None
    # Attribute sets per base record
    files = {}
    ext_owner = {}
    for no, rec in in_use.items():
        base = le64(rec, 0x20) & 0xFFFFFFFFFFFF
        # an extension record of $MFT (base record 0) has a non-zero
        # reference all the same (its sequence number)
        if base or le64(rec, 0x20):
            ext_owner[no] = (base, le16(rec, 0x10))
            continue
        files[no] = {'rec': rec, 'attrs': parse_attrs(rec, no, rs, vol.nclusters),
                     'seq': le16(rec, 0x10), 'links': le16(rec, 0x12),
                     'flags': le16(rec, 0x16)}
    # attribute lists
    used_ext = set()
    for no, f in files.items():
        al = [a for a in f['attrs'] if a['type'] == AT_AL]
        if not al:
            continue
        a = al[0]
        if a['nonres']:
            alb = vol.read_rl(a['runs'], 0, a['data'])
        else:
            alb = a['value']
        p = 0
        allattrs = [x for x in f['attrs'] if x['type'] != AT_AL]
        base_listed = 0
        ext_listed = {}
        while p < len(alb):
            if p + 26 > len(alb):
                err('MFT %d: attribute list entry header outside' % no)
                break
            t = le32(alb, p)
            ln = le16(alb, p + 4)
            nl = alb[p + 6]
            noff = alb[p + 7]
            low = sle64(alb, p + 8)
            ref = le64(alb, p + 16)
            inst = le16(alb, p + 24)
            if ln < 26 or ln & 7 or p + ln > len(alb):
                err('MFT %d: attribute list entry length' % no)
                break
            name = alb[p + noff:p + noff + 2 * nl]
            rno = ref & 0xFFFFFFFFFFFF
            rseq = ref >> 48
            if rno == no:
                base_listed += 1
                src = f['attrs']
            else:
                if rno not in ext_owner or ext_owner[rno][0] != no:
                    err('MFT %d: attribute list names record %d which is not its extension' % (no, rno))
                    p += ln
                    continue
                if rseq and ext_owner[rno][1] != rseq:
                    err('MFT %d: attribute list sequence for record %d' % (no, rno))
                used_ext.add(rno)
                src = parse_attrs(in_use[rno], rno, rs, vol.nclusters)
            hit = [x for x in src if x['type'] == t and x['inst'] == inst and x['name'] == name and x['lowest'] == low]
            if not hit:
                err('MFT %d: attribute list entry (0x%x inst %d in %d) has no attribute' % (no, t, inst, rno))
            elif rno != no:
                allattrs.append(hit[0])
                ext_listed.setdefault(rno, set()).add((t, inst))
            p += ln
        # every attribute of an extension record must be listed (one that
        # is not is invisible to the file but still maps its clusters)
        for rno, lst in ext_listed.items():
            for x in parse_attrs(in_use[rno], rno, rs, vol.nclusters):
                if (x['type'], x['inst']) not in lst:
                    err('MFT %d: attribute 0x%x (inst %d) in extension record %d is not in the attribute list' %
                        (no, x['type'], x['inst'], rno))
        if base_listed != len([x for x in f['attrs'] if x['type'] != AT_AL]):
            err('MFT %d: base record attributes not all listed (%d of %d)' %
                (no, base_listed, len([x for x in f['attrs'] if x['type'] != AT_AL])))
        f['attrs'] = allattrs + al
    for rno, (base, seq) in ext_owner.items():
        if rno not in used_ext:
            if base in files:
                leak('MFT %d: extension record of %d not in its attribute list' % (rno, base))
            else:
                leak('MFT %d: extension record of a missing base %d' % (rno, base))
    # Streams: extents continuity and sizes; cluster usage
    owner = {}
    usage_errors = 0

    def use(lcn, ln, who):
        nonlocal usage_errors
        for c in range(lcn, lcn + ln):
            if c in owner:
                if usage_errors < 20:
                    err('cluster %d used by %s and %s' % (c, owner[c], who))
                usage_errors += 1
            owner[c] = who

    for no, f in files.items():
        groups = {}
        for a in f['attrs']:
            if a['nonres']:
                groups.setdefault((a['type'], a['name']), []).append(a)
        for (t, name), exts in groups.items():
            exts.sort(key=lambda x: x['lowest'])
            first = exts[0]
            if first['lowest'] != 0:
                err('MFT %d: type 0x%x has no first extent' % (no, t))
                continue
            v = 0
            for x in exts:
                if x['lowest'] != v:
                    err('MFT %d: type 0x%x extents do not continue (%d vs %d)' % (no, t, x['lowest'], v))
                v = x['highest'] + 1
            first['all_runs'] = [r for x in exts for r in x['runs']]
            if v * cs != first['alloc']:
                err('MFT %d: type 0x%x runlist %d clusters, allocated %d' % (no, t, v, first['alloc']))
            if no == 8 and t == AT_DATA:
                continue
            tag = 'MFT %d/0x%x' % (no, t)
            nalloc = 0
            for x in exts:
                for (vv, l, ln) in x['runs']:
                    if l is not None:
                        use(l, ln, tag)
                        nalloc += ln
            if first['flags'] & 0x8000 and first['comp'] is not None and first['comp'] != nalloc * cs:
                err('MFT %d: sparse compressed_size %d != allocated %d' % (no, first['comp'], nalloc * cs))
    # $BadClus:$Bad
    if 8 in files:
        for a in files[8]['attrs']:
            if a['type'] == AT_DATA and a['name'] and a['nonres']:
                for (v, l, ln) in a['runs']:
                    if l is not None:
                        use(l, ln, '$BadClus')
    # $Bitmap
    bm = None
    for a in files.get(6, {'attrs': []})['attrs']:
        if a['type'] == AT_DATA and not a['name'] and a['nonres'] and a['lowest'] == 0:
            bm = vol.read_rl(a['all_runs'], 0, a['data'])
    if bm is None:
        err('$Bitmap unreadable')
        return finish(strict, quiet)
    used_free = 0
    leaked = 0
    for c in range(vol.nclusters):
        bit = (bm[c // 8] >> (c % 8)) & 1
        if c in owner and not bit:
            if used_free < 20:
                err('cluster %d used by %s but free in $Bitmap' % (c, owner[c]))
            used_free += 1
        elif bit and c not in owner and c != 0:
            leaked += 1
    if used_free:
        INFO.append('%d clusters used but free' % used_free)
    if leaked:
        leak('%d clusters marked in use that nothing references' % leaked)
    # upcase
    up = None
    for a in files.get(10, {'attrs': []})['attrs']:
        if a['type'] == AT_DATA and not a['name'] and a['nonres'] and a['lowest'] == 0:
            up = upcase_table(vol, a['all_runs'], a['data'])

    def upc(s):
        return [up[c] if c < len(up) else c for c in s]

    def coll(a, b):
        ua, ub = upc(a), upc(b)
        if ua != ub:
            return -1 if ua < ub else 1
        if a != b:
            return -1 if a < b else 1
        return 0

    # Directories
    names_in_index = set()
    for no, f in files.items():
        irs = [a for a in f['attrs'] if a['type'] == AT_IR and a['name'] == '$I30'.encode('utf-16le')]
        if not irs:
            continue
        if not (f['flags'] & 2):
            err('MFT %d: $I30 root without the directory flag' % no)
        ir = irs[0]['value']
        ib = le32(ir, 8)
        ia = [a for a in f['attrs'] if a['type'] == AT_IA and a['name'] == '$I30'.encode('utf-16le') and a['lowest'] == 0]
        ibm = [a for a in f['attrs'] if a['type'] == AT_BITMAP and a['name'] == '$I30'.encode('utf-16le')]
        large = ir[16 + 12] & 1
        if large and not ia:
            err('MFT %d: large index without $INDEX_ALLOCATION' % no)
            continue
        vbits = (cs.bit_length() - 1) if ib >= cs else 9
        bmap = b''
        if ibm:
            b0 = ibm[0]
            bmap = b0['value'] if not b0['nonres'] else vol.read_rl(b0['runs'], 0, b0['data'])
        if ia and not ibm:
            err('MFT %d: $INDEX_ALLOCATION without $BITMAP' % no)
        reached = set()
        prev = [None]

        def visit_entries(h_off, buf, is_node, depth, where):
            pos = le32(buf, h_off) + h_off
            end = le32(buf, h_off + 4) + h_off
            children = []
            ents = []
            while pos < end:
                ln = le16(buf, pos + 8)
                kl = le16(buf, pos + 10)
                fl = le16(buf, pos + 12)
                if ln < 16 or pos + ln > end:
                    err('dir %d %s: entry length' % (no, where))
                    return children, ents
                if bool(fl & 1) != bool(is_node):
                    err('dir %d %s: NODE flag mismatch' % (no, where))
                child = le64(buf, pos + ln - 8) if fl & 1 else None
                if fl & 2:
                    ents.append((None, child))
                    return children, ents
                ref = le64(buf, pos)
                fn = buf[pos + 16:pos + 16 + kl]
                nlen = fn[64]
                name = struct.unpack('<%dH' % nlen, fn[66:66 + 2 * nlen])
                ents.append(((ref, fn, name), child))
                pos += ln
            err('dir %d %s: no END entry' % (no, where))
            return children, ents

        # iterative in-order walk to check order and collect
        stack = [('root', None, 0)]
        order = []
        steps = 0
        node_cache = {}

        def load(vcn):
            if vcn in node_cache:
                return node_cache[vcn]
            off = vcn << vbits
            try:
                raw = vol.read_rl(ia[0]['all_runs'], off, ib)
            except Exception:
                err('dir %d: index block vcn %d unmapped' % (no, vcn))
                return None
            if raw[:4] != b'INDX':
                err('dir %d: index block vcn %d not INDX' % (no, vcn))
                return None
            blk, e = fixup(raw, ib, 'dir %d INDX %d' % (no, vcn))
            if e:
                err(e)
                return None
            if sle64(blk, 16) != vcn:
                err('dir %d: INDX %d claims vcn %d' % (no, vcn, sle64(blk, 16)))
            b = (vcn << vbits) // ib
            if b // 8 >= len(bmap) or not (bmap[b // 8] >> (b % 8)) & 1:
                err('dir %d: index block %d referenced but free in $BITMAP' % (no, vcn))
            if vcn in reached:
                err('dir %d: index block %d referenced twice' % (no, vcn))
            reached.add(vcn)
            node_cache[vcn] = blk
            return blk

        def walk(buf, h_off, is_node, depth):
            if depth > 32:
                err('dir %d: too deep' % no)
                return
            _, ents = visit_entries(h_off, buf, is_node, depth, 'node')
            for (e, child) in ents:
                if child is not None:
                    blk = load(child)
                    if blk is not None:
                        walk(blk, 0x18, blk[0x18 + 12] & 1, depth + 1)
                if e is not None:
                    order.append(e)

        walk(ir, 16, large, 0)
        for i in range(1, len(order)):
            if coll(order[i - 1][2], order[i][2]) >= 0:
                err('dir %d: entries out of order (%s, %s)' % (
                    no, bytes(struct.pack('<%dH' % len(order[i - 1][2]), *order[i - 1][2])).decode('utf-16le', 'replace'),
                    bytes(struct.pack('<%dH' % len(order[i][2]), *order[i][2])).decode('utf-16le', 'replace')))
        for (ref, fn, name) in order:
            rno = ref & 0xFFFFFFFFFFFF
            rseq = ref >> 48
            if le64(fn, 0) & 0xFFFFFFFFFFFF != no:
                err('dir %d: entry parent field %d' % (no, le64(fn, 0) & 0xFFFFFFFFFFFF))
            if rno not in files:
                err('dir %d: entry %s -> record %d not in use' % (no, bytes(fn[66:]).decode('utf-16le', 'replace'), rno))
                continue
            if rseq and files[rno]['seq'] != rseq:
                err('dir %d: entry -> record %d stale sequence %d (record %d)' % (no, rno, rseq, files[rno]['seq']))
            ok = False
            for a in files[rno]['attrs']:
                if a['type'] == AT_FN:
                    v = a['value']
                    if (le64(v, 0) & 0xFFFFFFFFFFFF) == no and v[64] == fn[64] and v[66:66 + 2 * v[64]] == fn[66:66 + 2 * fn[64]]:
                        ok = True
            if not ok:
                err('dir %d: entry %s -> record %d has no such $FILE_NAME' % (no, bytes(fn[66:66 + 2 * fn[64]]).decode('utf-16le', 'replace'), rno))
            names_in_index.add((no, rno, fn[66:66 + 2 * fn[64]]))
        # blocks marked but not reached
        if ia:
            nblk = ia[0]['data'] // ib
            for b in range(nblk):
                if b // 8 < len(bmap) and (bmap[b // 8] >> (b % 8)) & 1:
                    vcn = (b * ib) >> vbits
                    if vcn not in reached:
                        leak('dir %d: index block %d in use, unreferenced' % (no, vcn))
    # Names in records must be in their directories; link counts
    for no, f in files.items():
        fns = [a for a in f['attrs'] if a['type'] == AT_FN]
        if not fns and no >= 24:
            leak('MFT %d: in use without a name (orphan)' % no)
        for a in fns:
            v = a['value']
            parent = le64(v, 0) & 0xFFFFFFFFFFFF
            name = v[66:66 + 2 * v[64]]
            if no == 5 and parent == 5:
                continue
            if (parent, no, name) not in names_in_index and parent in files:
                # A name the directory does not list: the record was
                # written before its entry (allocate before reference) or
                # the entry removed before the name (dereference first):
                # unreachable through this name, nothing points to freed
                # space -- what a power cut may leave (chkdsk drops it).
                leak('MFT %d: name %s not in directory %d' % (no, name.decode('utf-16le', 'replace'), parent))
        if no >= 24 or no == 5:
            if f['links'] != len(fns) and len(fns):
                (err if f['links'] < len(fns) else leak)('MFT %d: link count %d, %d names' % (no, f['links'], len(fns)))
    # $MFTMirr
    mf = files.get(1)
    if mf:
        for a in mf['attrs']:
            if a['type'] == AT_DATA and not a['name'] and a['nonres'] and a['lowest'] == 0:
                n = min(4, a['data'] // rs)
                for i in range(n):
                    m = vol.read_rl(a['all_runs'], i * rs, rs)
                    if i in raw_records and m != raw_records[i]:
                        # Two writes, $MFT first: a power cut between them
                        # leaves the mirror one change behind (a valid
                        # older copy; chkdsk / ntfsfix rewrite it).
                        mrec, me = fixup(m, rs, 'mirror %d' % i)
                        if me or mrec[:4] != b'FILE':
                            err('$MFTMirr record %d differs from $MFT and is invalid' % i)
                        else:
                            leak('$MFTMirr record %d is an older copy of $MFT record %d' % (i, i))
    # $Volume flags
    vf = None
    for a in files.get(3, {'attrs': []})['attrs']:
        if a['type'] == AT_VOLINFO:
            vf = le16(a['value'], 10)
    INFO.append('records in use %d, files %d, clusters used %d of %d, volume flags 0x%x' %
                (len(in_use), len(files), len(owner), vol.nclusters, vf or 0))
    if vf is not None and vf & 1:
        INFO.append('volume is marked dirty')
    return finish(strict, quiet)


def finish(strict, quiet):
    for e in ERRORS[:200]:
        print('ERROR', e)
    for l in LEAKS[:50]:
        print('LEAK', l)
    if not quiet:
        for i in INFO:
            print('INFO', i)
    print('check: %d errors, %d leaks' % (len(ERRORS), len(LEAKS)))
    if ERRORS or (strict and LEAKS):
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
