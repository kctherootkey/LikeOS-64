#!/usr/bin/env python3
# Crafted ISO 9660 images for the iso9660 driver's host tests
# (host/test-isofs.sh), covering what the usual mastering tools do not
# produce on demand:
#
#   - logical block sizes 512, 1024 and 2048
#   - multi-extent files with small extents, block-aligned and not
#   - interleaved files (file unit size / interleave gap), extended
#     attribute records, file data inside the system area (block 0 too)
#   - hidden and associated files
#   - Rock Ridge: SP, ER, PX (short and long form), PN, SL split over
#     several entries and with split components, NM split over several
#     entries and continued through CE chains, TF short and long form,
#     CL / PL / RE deep-directory relocation, SF, ZF (zisofs with
#     compressible data, zero blocks, block sizes 2^15..2^17)
#   - Joliet (UCS-2 with surrogate pairs) and ISO 9660:1999 trees
#
#   test-isofs.py build VARIANT IMAGE STAGEDIR
#       write IMAGE and the tree it should show into STAGEDIR (which must
#       not exist): regular files, directories and symbolic links, from
#       which `mktree.py manifest' makes the expected manifest.  Things a
#       manifest cannot hold (device nodes, unreadable SF data, hidden
#       entries) are checked by the runner scripts in test-isofs.sh.
#   test-isofs.py --list
#
# Variants: rr2048 rr1024 rr512 joliet plain iso1999 zisofs multi
#           reloc ce
#
# Copyright (C) 2026 The LikeOS Project
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# encoding helpers
# ---------------------------------------------------------------------------


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def date7(t=(126, 11, 20, 12, 34, 56, 4)):
    return bytes(t)


def date17(s="2026112012345600", off=4):
    return s.encode() + bytes([off & 0xFF])


def pattern(seed, n):
    out = bytearray()
    x = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out += struct.pack("<I", x)
    return bytes(out[:n])


def text(seed, n):
    words = [b"alpha ", b"beta ", b"gamma ", b"delta ", b"iso9660 ",
             b"zisofs ", b"likeos ", b"\n"]
    out = bytearray()
    i = seed
    while len(out) < n:
        out += words[(i * 7 + len(out)) % len(words)]
        i += 1
    return bytes(out[:n])


# ---------------------------------------------------------------------------
# SUSP / RRIP entries
# ---------------------------------------------------------------------------


def su(sig, data, ver=1):
    assert len(data) + 4 <= 255
    return sig.encode() + bytes([len(data) + 4, ver]) + data


def su_sp(skip=0):
    return su("SP", b"\xbe\xef" + bytes([skip]))


def su_er():
    ident = b"RRIP_1991A"
    desc = b"THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS"
    src = b"PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE."
    return su("ER", bytes([len(ident), len(desc), len(src), 1]) + ident + desc + src)


def su_px(mode, nlink=1, uid=1000, gid=1000, serial=None):
    d = both32(mode) + both32(nlink) + both32(uid) + both32(gid)
    if serial is not None:
        d += both32(serial)
    return su("PX", d)


def su_pn(high, low):
    return su("PN", both32(high) + both32(low))


def su_tf(long_form=False):
    flags = 0x02 | 0x04 | 0x08
    if long_form:
        flags |= 0x80
        stamps = date17("2025010203040550", 0) + date17("2025010203040600", 0) + \
            date17("2025010203040700", 0)
    else:
        stamps = date7((125, 1, 2, 3, 4, 5, 0)) + date7((125, 1, 2, 3, 4, 6, 0)) + \
            date7((125, 1, 2, 3, 4, 7, 0))
    return su("TF", bytes([flags]) + stamps)


def su_nm_pieces(name, piece=40):
    b = name.encode()
    out = []
    for i in range(0, len(b), piece):
        chunk = b[i:i + piece]
        cont = 1 if i + piece < len(b) else 0
        out.append(su("NM", bytes([cont]) + chunk))
    if not out:
        out.append(su("NM", b"\x00"))
    return out


def su_sl_pieces(target, split_component=False):
    """SL entries for `target', one component per entry, a long
    component split in two (CONTINUE) when asked."""
    comps = []
    if target.startswith("/"):
        comps.append(bytes([0x08, 0]))
    for part in [p for p in target.split("/") if p != ""]:
        if part == ".":
            comps.append(bytes([0x02, 0]))
        elif part == "..":
            comps.append(bytes([0x04, 0]))
        else:
            b = part.encode()
            if split_component and len(b) > 4:
                comps.append(bytes([0x01, 3]) + b[:3])
                comps.append(bytes([0x00, len(b) - 3]) + b[3:])
            else:
                comps.append(bytes([0x00, len(b)]) + b)
    out = []
    for i, c in enumerate(comps):
        more = 1 if i + 1 < len(comps) else 0
        out.append(su("SL", bytes([more]) + c))
    return out


def su_ce(lba, off, ln):
    return su("CE", both32(lba) + both32(off) + both32(ln))


def su_zf(blog, usize):
    return su("ZF", b"pz" + bytes([4, blog]) + both32(usize))


def su_sf(size):
    return su("SF", both32(size >> 32) + both32(size & 0xFFFFFFFF) + b"\x00")


# ---------------------------------------------------------------------------
# the tree
# ---------------------------------------------------------------------------


class Node:
    def __init__(self, name, kind, **kw):
        self.name = name
        self.kind = kind  # 'd' 'f' 'l' 'c' (device)
        self.children = []
        self.data = b""
        self.target = ""
        self.kw = kw
        self.parent = None
        self.ident = None
        self.extents = []  # [(lba, len)]
        self.lba = 0
        self.size = 0
        self.ino_hint = 0

    def add(self, n):
        n.parent = self
        self.children.append(n)
        return n


def D(name, *kids, **kw):
    n = Node(name, "d", **kw)
    for k in kids:
        n.add(k)
    return n


def F(name, data, **kw):
    n = Node(name, "f", **kw)
    n.data = data
    return n


def L(name, target, **kw):
    n = Node(name, "l", **kw)
    n.target = target
    return n


def C(name, high, low, block=False, **kw):
    n = Node(name, "c", **kw)
    n.kw["pn"] = (high, low)
    n.kw["block"] = block
    return n


# ---------------------------------------------------------------------------
# the image
# ---------------------------------------------------------------------------


class Iso:
    def __init__(self, lbs=2048, rock=True, joliet=False, iso1999=False,
                 long_px=True, tf_long=False):
        self.lbs = lbs
        self.rock = rock
        self.joliet = joliet
        self.iso1999 = iso1999
        self.long_px = long_px
        self.tf_long = tf_long
        self.img = bytearray()
        # first free block after the system area and the descriptors
        self.next = (32768 + 8 * 2048) // lbs
        self.serial = 100

    def alloc(self, nbytes, align=1):
        n = max(1, (nbytes + self.lbs - 1) // self.lbs)
        if self.next % align:
            self.next += align - self.next % align
        lba = self.next
        self.next += n
        return lba

    def put(self, off, data):
        end = off + len(data)
        if len(self.img) < end:
            self.img.extend(b"\0" * (end - len(self.img)))
        self.img[off:end] = data

    def putb(self, lba, data):
        self.put(lba * self.lbs, data)

    # ---- identifiers -------------------------------------------------

    def assign_idents(self, d, joliet):
        used = set()
        for i, c in enumerate(sorted(d.children, key=lambda n: n.name)):
            if joliet:
                ident = c.name.encode("utf-16-be")
                if c.kind != "d":
                    ident += ";1".encode("utf-16-be")
            else:
                base = c.kw.get("ident")
                if base is None:
                    base = ("D%04d" % i) if c.kind == "d" else ("F%04d.DAT;1" % i)
                ident = base.encode()
            assert ident not in used
            used.add(ident)
            if joliet:
                c.jident = ident
            else:
                c.ident = ident
            if c.kind == "d":
                self.assign_idents(c, joliet)

    # ---- System Use ---------------------------------------------------

    def su_for(self, n, is_dot=False, is_root_dot=False, is_dotdot=False):
        if not self.rock:
            return [], []
        inline, extra = [], []
        if is_root_dot:
            inline.append(su_sp())
            extra.append(su_er())
        k = n.kind
        mode = {"d": 0o40755, "f": 0o100644, "l": 0o120777,
                "c": 0o20640}[k]
        if k == "c" and n.kw.get("block"):
            mode = 0o60640
        mode = n.kw.get("mode", mode)
        self.serial += 1
        px = su_px(mode, 2 if k == "d" else 1, n.kw.get("uid", 1000),
                   n.kw.get("gid", 1000),
                   self.serial if self.long_px else None)
        inline.append(px)
        inline.append(su_tf(self.tf_long))
        if is_dot or is_dotdot:
            if is_dotdot and n.kw.get("pl") is not None:
                inline.append(su("PL", both32(n.kw["pl"])))
            return inline, extra
        if k == "c":
            inline.append(su_pn(*n.kw["pn"]))
        nm = su_nm_pieces(n.name, n.kw.get("nm_piece", 200))
        if n.kw.get("nm_unterminated"):
            last = bytearray(nm[-1])
            last[4] |= 1  # "more follows" -- and nothing does
            nm[-1] = bytes(last)
        if n.kw.get("use_chain"):
            inline.append(su_ce(*self.chain))
        if n.kw.get("nm_in_ce"):
            extra.extend(nm)
        else:
            inline.extend(nm)
        if k == "l":
            sl = su_sl_pieces(n.target, n.kw.get("sl_split", False))
            if n.kw.get("sl_in_ce"):
                extra.extend(sl)
            else:
                inline.extend(sl)
        if n.kw.get("zf"):
            inline.append(su_zf(n.kw["zf"], len(n.data)))
        if n.kw.get("sf") is not None:
            inline.append(su_sf(n.kw["sf"]))
        if n.kw.get("re"):
            inline.append(su("RE", b""))
        if n.kw.get("cl") is not None:
            inline.append(su("CL", both32(n.kw["cl"])))
        return inline, extra

    def place_su(self, inline, extra, room, hops=1, ce_mode=None):
        """Return the System Use bytes for a record: what fits inline,
        the rest through a chain of `hops' continuation areas.  ce_mode
        "beyond": the CE points past the end of the device; "selfloop":
        the one continuation area continues into itself."""
        data = b"".join(inline)
        if not extra and len(data) <= room:
            return data
        if ce_mode == "selfloop":
            body = b"".join(extra)
            lba = self.alloc(len(body) + 28)
            body += su_ce(lba, 0, len(body) + 28)
            self.putb(lba, body)
            return data + su_ce(lba, 0, len(body))
        # everything that does not fit goes to CE areas
        moved = list(extra)
        while len(data) + 28 > room:
            moved.insert(0, inline.pop())
            data = b"".join(inline)
        chunks = [[] for _ in range(hops)]
        for i, e in enumerate(moved):
            chunks[min(i * hops // max(1, len(moved)), hops - 1)].append(e)
        # allocate from the last area backwards so each can point on
        areas = [b"".join(c) for c in chunks if c]
        nxt = None
        for i in range(len(areas) - 1, -1, -1):
            body = areas[i]
            if nxt is not None:
                body += su_ce(*nxt)
            assert len(body) <= self.lbs
            lba = self.alloc(len(body))
            off = 0
            self.putb(lba, body)
            nxt = (lba, off, len(body))
        if ce_mode == "beyond":
            nxt = (0x7FFFFFF0, 0, nxt[2])
        return data + su_ce(*nxt)

    # ---- records ------------------------------------------------------

    def record(self, ident, lba, size, flags, su_bytes, xar=0, fus=0, igs=0):
        fi = len(ident)
        pad = 1 if fi % 2 == 0 else 0
        ln = 33 + fi + pad + len(su_bytes)
        if ln % 2:
            ln += 1
            su_bytes += b"\0"
        assert ln <= 255, (ident, ln)
        r = bytes([ln, xar]) + both32(lba) + both32(size) + date7() + \
            bytes([flags, fus, igs]) + both16(1) + bytes([fi]) + ident + \
            b"\0" * pad + su_bytes
        return r

    def dir_records(self, d, joliet, parent_lba, parent_size, final):
        """The records of directory `d' (with placeholder extents while
        not `final')."""
        recs = []
        rootdot = d.parent is None and not joliet
        inl, ext = self.su_for(d, is_dot=True, is_root_dot=rootdot) \
            if not joliet else ([], [])
        recs.append(("dot", inl, ext, b"\x00", d.lba, d.size, 0x02,
                     {"fus": d.kw.get("dfus", 0), "igs": d.kw.get("digs", 0)}))
        inl, ext = self.su_for(d.parent or d, is_dotdot=True) \
            if not joliet else ([], [])
        if not joliet and d.kw.get("pl") is not None:
            inl.append(su("PL", both32(d.kw["pl"])))
        recs.append(("dotdot", inl, ext, b"\x01", parent_lba, parent_size,
                     0x02, {}))
        kids = sorted(d.children, key=lambda n: (n.jident if joliet else n.ident))
        for c in kids:
            if joliet and c.kw.get("no_joliet"):
                continue
            ident = c.jident if joliet else c.ident
            inl, ext = self.su_for(c) if not joliet else ([], [])
            flags = c.kw.get("flags", 0)
            if c.kind == "d" and not c.kw.get("cl_placeholder"):
                flags |= 0x02
            if c.kind == "d":
                recs.append(("dir", inl, ext, ident, c.lba, c.size, flags,
                             {"fus": c.kw.get("dfus", 0),
                              "igs": c.kw.get("digs", 0),
                              "ce_mode": c.kw.get("ce_mode")}))
                continue
            exts = c.extents or [(0, 0)]
            for i, (lba, ln) in enumerate(exts):
                f = flags | (0x80 if i + 1 < len(exts) else 0)
                sub_inl = inl if i == 0 else ([] if not self.rock else [])
                recs.append(("file", sub_inl, ext if i == 0 else [], ident,
                             lba, ln, f,
                             {"xar": c.kw.get("xar", 0),
                              "fus": c.kw.get("fus", 0),
                              "igs": c.kw.get("igs", 0),
                              "ce_mode": c.kw.get("ce_mode") if i == 0 else None,
                              "path": c}))
        out = bytearray()
        for kind, inl, ext, ident, lba, size, flags, kw in recs:
            fi = len(ident)
            room = 255 - (33 + fi + (1 if fi % 2 == 0 else 0)) - 1
            if final:
                su_bytes = self.place_su(list(inl), list(ext), room,
                                         hops=kw.get("hops", 2 if ext else 1),
                                         ce_mode=kw.get("ce_mode"))
            else:
                # sizes only: CE entry is 28 bytes
                data = b"".join(inl)
                su_bytes = data if (not ext and len(data) <= room) else b"\0" * min(room, len(data) + 28)
            r = self.record(ident, lba, size, flags, su_bytes, kw.get("xar", 0),
                            kw.get("fus", 0), kw.get("igs", 0))
            # records never straddle a block
            if len(out) // self.lbs != (len(out) + len(r) - 1) // self.lbs:
                out += b"\0" * (self.lbs - len(out) % self.lbs)
            if final and kind == "file" and kw.get("path") is not None and \
                    kw["path"].kw.get("_doff") is None:
                kw["path"].kw["_doff"] = len(out)
            out += r
        return bytes(out)

    # ---- data ---------------------------------------------------------

    def place_file(self, n):
        data = n.data
        kw = n.kw
        if kw.get("lba_fixed") is not None:
            lba = kw["lba_fixed"]
            self.putb(lba, data)
            n.extents = [(lba, len(data))]
            return
        if kw.get("zf"):
            data = zisofs_encode(n.data, kw["zf"], kw.get("zero_block"))
        if kw.get("multi"):
            sizes = kw["multi"]
            assert sum(sizes) == len(data)
            pos = 0
            n.extents = []
            for s in sizes:
                if s == 0:
                    n.extents.append((0, 0))
                    continue
                lba = self.alloc(max(1, s))
                self.putb(lba, data[pos:pos + s])
                n.extents.append((lba, s))
                pos += s
            # shuffle the order on the medium: extents need not ascend
            return
        if kw.get("fus"):
            fus, igs = kw["fus"], kw["igs"]
            nblk = (len(data) + self.lbs - 1) // self.lbs
            units = (nblk + fus - 1) // fus
            lba = self.alloc(units * (fus + igs) * self.lbs)
            for b in range(nblk):
                pl = lba + (b // fus) * (fus + igs) + b % fus
                self.putb(pl, data[b * self.lbs:(b + 1) * self.lbs])
            # fill the gaps with garbage that must never be read
            for u in range(units):
                for g in range(igs):
                    self.putb(lba + u * (fus + igs) + fus + g, b"\xee" * self.lbs)
            n.extents = [(lba, len(data))]
            return
        xar = kw.get("xar", 0)
        lba = self.alloc(len(data) + xar * self.lbs)
        if xar:
            self.putb(lba, b"\xaa" * (xar * self.lbs))
        self.putb(lba + xar, data)
        n.extents = [(lba, len(data))]
        if len(data) == 0 and not kw.get("keep_lba"):
            n.extents = [(0, 0)]

    def walk(self, d):
        yield d
        for c in sorted(d.children, key=lambda n: n.name):
            if c.kind == "d" and not c.kw.get("cl_placeholder"):
                yield from self.walk(c)

    def make_chain(self, n):
        """A chain of `n' continuation areas, the last holding a PX
        entry; every record with kw use_chain points at its head."""
        nxt = None
        for i in range(n):
            body = su("PD", b"\0" * 8) if nxt is None else su_ce(*nxt)
            lba = self.alloc(len(body))
            self.putb(lba, body)
            nxt = (lba, 0, len(body))
        self.chain = nxt

    def dir_block(self, d, b):
        fus, igs = d.kw.get("dfus", 0), d.kw.get("digs", 0)
        if not fus:
            return d.lba + b
        return d.lba + (b // fus) * (fus + igs) + b % fus

    def build(self, root):
        self.root = root
        if getattr(self, "chain_len", 0):
            self.make_chain(self.chain_len)
        self.assign_idents(root, False)
        if self.joliet:
            self.assign_idents(root, True)
        # file data first (fixed sizes), then directories
        for d in self.walk(root):
            for c in d.children:
                if c.kind == "f":
                    self.place_file(c)
        # relocation targets: resolved by name after placement
        dirs = list(self.walk(root))
        # pass 1: sizes (CL / PL entries already counted)
        for d in dirs:
            d.lba, d.size = 0, 0
            for c in d.children:
                if c.kw.get("cl_placeholder"):
                    c.kw["cl"] = 0
                    for t in dirs:
                        if t.kw.get("reloc_id") == c.kw["cl_of"]:
                            t.kw["pl"] = 0
        for d in dirs:
            recs = self.dir_records(d, False, 0, 0, False)
            d.size = len(recs) + (self.lbs - len(recs) % self.lbs) % self.lbs
            if d.kw.get("in_gap_of"):
                tgt = next(x for x in dirs if x.name == d.kw["in_gap_of"])
                assert d.size <= tgt.kw["digs"] * self.lbs
                d.lba = tgt.lba + tgt.kw["dfus"]
            elif d.kw.get("dfus"):
                fus, igs = d.kw["dfus"], d.kw["digs"]
                nblk = d.size // self.lbs
                units = (nblk + fus - 1) // fus
                d.lba = self.alloc(units * (fus + igs) * self.lbs)
            else:
                d.lba = self.alloc(d.size)
        self.fix_relocation(dirs)
        # pass 2: real records
        for d in dirs:
            p = d.parent or d
            recs = self.dir_records(d, False, p.lba, p.size, True)
            assert len(recs) <= d.size, (d.name, len(recs), d.size)
            for b in range(0, len(recs), self.lbs):
                self.putb(self.dir_block(d, b // self.lbs),
                          recs[b:b + self.lbs])
            # st_ino of every file: its first record's real byte offset
            for c in d.children:
                if c.kw.get("_doff") is not None:
                    doff = c.kw["_doff"]
                    real = self.dir_block(d, doff // self.lbs) * self.lbs + \
                        doff % self.lbs
                    c.ino = (1 << 44) | real
        if self.joliet:
            jl = {}
            for d in dirs:
                recs = self.dir_records_j(d, None)
                sz = len(recs) + (self.lbs - len(recs) % self.lbs) % self.lbs
                jl[id(d)] = (self.alloc(sz), sz)
            for d in dirs:
                p = d.parent or d
                recs = self.dir_records_j(d, jl, p)
                self.putb(jl[id(d)][0], recs)
            self.jroot = jl[id(root)]
        # whole 4096-byte sectors, so every device sector size holds it
        self.next += (-self.next * self.lbs % 4096) // self.lbs
        self.descriptors()
        self.put(self.next * self.lbs - 1, b"\0")

    def dir_records_j(self, d, jl, parent=None):
        out = bytearray()

        def pos(x):
            return jl[id(x)] if jl else (0, 0)
        recs = [(b"\x00", pos(d)[0], pos(d)[1], 2), (b"\x01", pos(parent or d)[0],
                                                     pos(parent or d)[1], 2)]
        for c in sorted(d.children, key=lambda n: n.jident):
            if c.kind == "d":
                if c.kw.get("cl_placeholder"):
                    continue
                recs.append((c.jident, pos(c)[0], pos(c)[1], 2))
            elif c.kind == "f":
                for i, (lba, ln) in enumerate(c.extents or [(0, 0)]):
                    recs.append((c.jident, lba, ln,
                                 0x80 if i + 1 < len(c.extents) else 0))
        for ident, lba, size, flags in recs:
            r = self.record(ident, lba, size, flags, b"")
            if len(out) // self.lbs != (len(out) + len(r) - 1) // self.lbs:
                out += b"\0" * (self.lbs - len(out) % self.lbs)
            out += r
        return bytes(out)

    def fix_relocation(self, dirs):
        by_name = {d.kw.get("reloc_id"): d for d in dirs if d.kw.get("reloc_id")}
        for d in dirs:
            for c in d.children:
                if c.kw.get("cl_placeholder"):
                    tgt = by_name[c.kw["cl_of"]]
                    c.kw["cl"] = tgt.lba
                    c.lba, c.size = 0, 0
                    tgt.kw["pl"] = d.lba

    def descriptors(self):
        vds = []
        space = self.next
        root = self.root

        def vd(t, ident=b"CD001", ver=1, root_rec=None, esc=b"", lbs=None,
               fsver=1):
            b = bytearray(2048)
            b[0] = t
            b[1:6] = ident
            b[6] = ver
            b[8:40] = b"LIKEOS".ljust(32)
            b[40:72] = b"CRAFTED".ljust(32)
            b[80:88] = both32(space)
            b[88:88 + len(esc)] = esc
            b[120:124] = both16(1)
            b[124:128] = both16(1)
            b[128:132] = both16(self.lbs)
            b[132:140] = both32(0)
            b[156:190] = root_rec
            b[813:830] = date17()
            b[830:847] = date17()
            b[881] = fsver
            return bytes(b)
        rr = self.record(b"\x00", root.lba, root.size, 2, b"")
        vds.append(vd(1, root_rec=rr))
        if self.joliet:
            jr = self.record(b"\x00", self.jroot[0], self.jroot[1], 2, b"")
            vds.append(vd(2, root_rec=jr, esc=b"%/E"))
        if self.iso1999:
            vds.append(vd(2, ver=2, root_rec=rr, fsver=2))
        t = bytearray(2048)
        t[0] = 255
        t[1:6] = b"CD001"
        t[6] = 1
        vds.append(bytes(t))
        for i, v in enumerate(vds):
            self.put(32768 + i * 2048, v)


def zisofs_encode(data, blog, zero_block=None):
    bs = 1 << blog
    nblk = (len(data) + bs - 1) // bs
    hdr = bytes([0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07]) + \
        struct.pack("<I", len(data)) + bytes([4, blog, 0, 0])
    table_len = (nblk + 1) * 4
    ptr = len(hdr) + table_len
    ptrs = [ptr]
    body = bytearray()
    for i in range(nblk):
        chunk = data[i * bs:(i + 1) * bs]
        if zero_block is not None and i == zero_block:
            assert chunk == b"\0" * len(chunk)
            comp = b""
        else:
            comp = zlib.compress(chunk, 9 if i % 2 else 1)
        body += comp
        ptr += len(comp)
        ptrs.append(ptr)
    return hdr + b"".join(struct.pack("<I", p) for p in ptrs) + bytes(body)


# ---------------------------------------------------------------------------
# variants
# ---------------------------------------------------------------------------


def basic_tree():
    long_name = "L" + "o" * 200 + "ng_name_" + "x" * 40  # 249 chars
    return D("",
             F("small.txt", b"hello iso9660\n"),
             F("empty", b""),
             F("block_exact.bin", pattern(1, 2048)),
             F("pages.bin", pattern(2, 3 * 4096 + 17)),
             F(long_name, pattern(3, 777), nm_piece=30),
             F("ce_name_" + "q" * 120, pattern(4, 100), nm_in_ce=True,
               nm_piece=25),
             F("xar.bin", pattern(5, 5000), xar=1),
             F("interleaved.bin", pattern(6, 9 * 2048 + 100), fus=2, igs=1),
             F("interleaved1.bin", pattern(7, 5 * 2048 + 5), fus=1, igs=3),
             L("abs_link", "/dir/sub/target.txt"),
             L("rel_link", "dir/sub/target.txt", sl_split=True),
             L("dot_link", "./dir/../dir/sub/target.txt"),
             L("ce_link", "dir/sub/" + "t" * 0 + "target.txt", sl_in_ce=True),
             L("dangling", "no/such/file"),
             L("dir_link", "dir/sub"),
             D("dir",
               D("sub", F("target.txt", b"the target\n")),
               F("in_dir.bin", pattern(8, 4096))),
             D("empty_dir"),
             )


def variant(name):
    if name in ("rr2048", "rr1024", "rr512"):
        lbs = int(name[2:])
        iso = Iso(lbs=lbs, tf_long=(lbs == 1024), long_px=(lbs != 512))
        return iso, basic_tree()
    if name == "joliet":
        iso = Iso(rock=False, joliet=True)
        t = D("", F("Joliet Mixed Case.TXT", pattern(9, 1000)),
              F("emoji_\U0001F600.txt", pattern(10, 50)),
              F("umlaut_äöü.txt", pattern(11, 4097)),
              F("multi_j.bin", pattern(12, 6144 + 3000), multi=[4096, 2048, 3000]),
              D("Sub Dir", F("Inner File.bin", pattern(13, 5000))))
        return iso, t
    if name in ("plain", "iso1999"):
        iso = Iso(rock=False, iso1999=(name == "iso1999"))
        t = D("", F("readme", b"plain names\n", ident="README.TXT;1"),
              F("noext", b"no extension\n", ident="NOEXT.;1"),
              F("ver", b"version 2\n", ident="VER.DAT;2"),
              D("subdir", F("inner", b"inner\n", ident="INNER.BIN;1"),
                ident="SUBDIR"))
        return iso, t
    if name == "zisofs":
        iso = Iso()
        z0 = text(1, 100000)
        z1 = text(2, 3 * 65536 + 1234)
        z2 = text(3, 300000)
        z3 = text(4, 65536) + b"\0" * 65536 + text(5, 1000)
        t = D("", F("z15.txt", z0, zf=15), F("z16.txt", z1, zf=16),
              F("z17.txt", z2, zf=17), F("zzero.txt", z3, zf=16, zero_block=1),
              F("ztiny.txt", b"tiny\n", zf=15),
              F("zempty.txt", b"", zf=15, keep_lba=True),
              F("plain.txt", text(6, 5000)))
        return iso, t
    if name == "multi":
        iso = Iso()
        t = D("", F("aligned.bin", pattern(20, 6 * 2048 + 4096 + 777),
                    multi=[6 * 2048, 4096, 777]),
              F("unaligned.bin", pattern(21, 3000 + 5000 + 100 + 9000),
                multi=[3000, 5000, 100, 9000]),
              F("onebyte_ext.bin", pattern(22, 2048 + 1 + 2048),
                multi=[2048, 1, 2048]),
              F("many_ext.bin", pattern(23, 40 * 2048 + 10),
                multi=[2048] * 40 + [10]),
              F("lba0.bin", pattern(24, 1500), lba_fixed=0),
              F("lba3.bin", pattern(25, 2048 * 2 + 3), lba_fixed=3))
        return iso, t
    if name == "reloc":
        iso = Iso()
        # /a/b/c relocated to /rr_moved/c (placeholder with CL in /a/b)
        c = D("c", F("deep.txt", b"relocated content\n"),
              D("d", F("deeper.txt", b"deeper\n")), reloc_id="c", re=True)
        t = D("", D("a", D("b", Node("c", "d", cl_placeholder=True,
                                     cl_of="c", mode=0o40755),
                           F("b.txt", b"in b\n"))),
              D("rr_moved", c))
        return iso, t
    if name == "ce":
        iso = Iso()
        kids = []
        for i in range(30):
            kids.append(F("ce_file_%02d_" % i + "n" * (40 + i * 5),
                          pattern(30 + i, i * 100 + 1), nm_in_ce=True,
                          nm_piece=17))
        t = D("", *kids)
        return iso, t
    if name == "jcase":
        iso = Iso(rock=False, joliet=True)
        t = D("", F("Case.txt", b"capital first\n"), F("case.txt", b"lower\n"),
              F("CASE.TXT", b"all capitals\n"), F("other.txt", b"other\n"))
        return iso, t
    if name == "damaged":
        iso = Iso()
        t = D("", F("good.txt", b"good\n"),
              F("lostname.txt", b"lost\n", nm_in_ce=True, ce_mode="beyond"),
              L("loop_link", "good.txt", sl_in_ce=True, ce_mode="selfloop"),
              F("unterminated.txt", b"cut\n", nm_unterminated=True))
        return iso, t
    if name == "ceflood":
        iso = Iso()
        iso.chain_len = 30
        t = D("", *[F("f%05d" % i, b"", use_chain=True) for i in range(10000)])
        return iso, t
    if name == "bigext":
        iso = Iso()
        t = D("", *[F("m%03d.bin" % i, b"\x5a", multi=[0] * 3999 + [1])
                    for i in range(60)])
        return iso, t
    if name == "ileave":
        iso = Iso()
        a = D("a_ileave", *[F("a_%02d_" % i + "n" * 60, pattern(40 + i, 100 + i))
                            for i in range(60)], dfus=1, digs=1)
        b = D("b_gap", *[F("b_%02d_" % i + "m" * 60, pattern(140 + i, 50))
                         for i in range(8)], in_gap_of="a_ileave")
        t = D("", a, b)
        return iso, t
    if name == "special":
        # checked by script (host/test-isofs.sh), not by manifest
        iso = Iso()
        t = D("", C("chr", 0, 0x0103), C("blk", 8, 1, block=True),
              F("fifo", b"", mode=0o10644),
              F("sock", b"", mode=0o140755),
              F("hidden.txt", b"hidden but listed\n", flags=0x01),
              F("assoc.txt", b"associated\n", flags=0x04),
              F("sparse.bin", b"", sf=1 << 20),
              F("normal.txt", b"normal\n"))
        return iso, t
    raise SystemExit("unknown variant %s" % name)


VARIANTS = ["rr2048", "rr1024", "rr512", "joliet", "plain", "iso1999",
            "zisofs", "multi", "reloc", "ce", "special", "jcase", "damaged",
            "ceflood", "bigext", "ileave"]


def stage(n, path, iso):
    """What the mounted image should show."""
    if n.kind == "d":
        if n.kw.get("re"):
            return  # hidden at its relocated place
        os.mkdir(path)
        for c in n.children:
            if c.kw.get("cl_placeholder"):
                tgt = next(d for d in iso.walk(iso.root)
                           if d.kw.get("reloc_id") == c.kw["cl_of"])
                stage_dir_as(tgt, os.path.join(path, c.name), iso)
                continue
            stage(c, os.path.join(path, c.name), iso)
    elif n.kind == "f":
        with open(path, "wb") as f:
            f.write(n.data)
    elif n.kind == "l":
        os.symlink(n.target, path)


def stage_dir_as(d, path, iso):
    os.mkdir(path)
    for c in d.children:
        stage(c, os.path.join(path, c.name), iso)


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--list":
        print(" ".join(VARIANTS))
        return 0
    if len(sys.argv) != 5 or sys.argv[1] != "build":
        print("usage: test-isofs.py build VARIANT IMAGE STAGEDIR | --list",
              file=sys.stderr)
        return 2
    v, img, stdir = sys.argv[2], sys.argv[3], sys.argv[4]
    iso, root = variant(v)
    iso.build(root)
    with open(img, "wb") as f:
        f.write(iso.img)
    if v in ("special", "damaged", "ceflood", "bigext"):
        os.mkdir(stdir)  # checked by script, not by manifest
        return 0
    if v == "ileave":
        with open(img + ".ino", "w") as f:
            for d in iso.walk(root):
                for c in d.children:
                    if c.kind == "f":
                        f.write("/%s/%s %d\n" % (d.name, c.name, c.ino))
    if v in ("plain", "iso1999"):
        # names as the plain mapping shows them
        os.mkdir(stdir)
        for c in root.children:
            nm = c.kw["ident"].split(";")[0].rstrip(".").lower()
            if c.kind == "d":
                os.mkdir(os.path.join(stdir, nm))
                for g in c.children:
                    gn = g.kw["ident"].split(";")[0].rstrip(".").lower()
                    with open(os.path.join(stdir, nm, gn), "wb") as f:
                        f.write(g.data)
            else:
                with open(os.path.join(stdir, nm), "wb") as f:
                    f.write(c.data)
        return 0
    stage(root, stdir, iso)
    return 0


if __name__ == "__main__":
    sys.exit(main())
