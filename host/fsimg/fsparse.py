#!/usr/bin/env python3
# On-disk metadata of filesystem images, for the fuzzer and the image
# tweaks of the harness (host/fsimg).  Pure reading of the formats: where
# the metadata of an image lies, as (offset, length, label) regions.
#
#   fsparse.py regions FSTYPE IMAGE        list the regions
#   fsparse.py ntfs-tweak IMAGE OP...       change an NTFS image:
#        dirty | clean          VOLUME_IS_DIRTY in $Volume (MFT + mirror)
#        vflags-HEX             set these $Volume flag bits (MFT + mirror),
#                               e.g. vflags-0x8000 (modified by chkdsk)
#        fragfree-RUN           mark RUN clusters in use, RUN free, RUN in
#                               use ... over the free space in $Bitmap
#                               (IMAGE.fragstate remembers which), so that
#                               a tool run next allocates in fragments
#        unfragfree             give those clusters back (bits cleared)
#        dospair-SHORT          a file with two names in one directory,
#                               SHORT and a long one (made as hard links),
#                               becomes a Win32 + DOS pair: SHORT gets the
#                               DOS namespace, the other one Win32, in the
#                               record and in the directory's (resident)
#                               index root
#        logfile-unclean        clear "volume is clean" in both $LogFile
#                               restart areas (needs replay / refuses rw)
#        mftmirr-mismatch       make $MFTMirr differ from $MFT (record 1)
#        log-SCENARIO           an unclean $LogFile with real records:
#                               redo-resident, undo-resident, redo-create,
#                               bad-op (see logfile_scenario)
#        hiber                  the first data bytes of /hiberfil.sys
#                               become "hibr" (file must exist, non-resident
#                               or resident)
#   fsparse.py wof-blob SRC OUT FMT         system-compressed stream data
#   fsparse.py wof-reparse IMAGE NAME FMT   WOF reparse point on /NAME
#   fsparse.py exfat-tweak IMAGE OP...
#        dirty | clean          VolumeDirty in the main boot sector
#        bad-boot-checksum      corrupt the main boot region checksum
#
# FSTYPE: ntfs exfat iso fat ext4
#
# Copyright (C) 2026 The LikeOS Project
import hashlib
import heapq
import random
import struct
import sys


class Img:
    def __init__(self, path, mode="rb"):
        self.f = open(path, mode)
        self.f.seek(0, 2)
        self.size = self.f.tell()

    def read(self, off, n):
        if off < 0 or off >= self.size:
            return b""
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)


# ---------------------------------------------------------------- NTFS --

class Ntfs:
    def __init__(self, img):
        self.img = img
        b = img.read(0, 512)
        if b[3:11] != b"NTFS    ":
            raise ValueError("not NTFS")
        self.bps = struct.unpack_from("<H", b, 0x0B)[0]
        spc = b[0x0D]
        self.spc = 1 << (256 - spc) if spc > 0x80 else spc
        self.cs = self.bps * self.spc
        self.nsectors = struct.unpack_from("<Q", b, 0x28)[0]
        self.mft_lcn, self.mirr_lcn = struct.unpack_from("<QQ", b, 0x30)
        cpr = struct.unpack_from("<b", b, 0x40)[0]
        self.rec = self.cs * cpr if cpr > 0 else 1 << -cpr
        cpi = struct.unpack_from("<b", b, 0x44)[0]
        self.idx = self.cs * cpi if cpi > 0 else 1 << -cpi
        rec0 = self.fixup(img.read(self.mft_lcn * self.cs, self.rec))
        self.mft_runs = []
        for a in self.attrs(rec0):
            if a["type"] == 0x80 and not a["name"]:
                self.mft_runs = a["runs"]
        if not self.mft_runs:
            self.mft_runs = [(self.mft_lcn, 16 * self.rec // self.cs or 1)]

    def fixup(self, rec):
        rec = bytearray(rec)
        if len(rec) < 48:
            return rec
        uo, uc = struct.unpack_from("<HH", rec, 4)
        if uo + 2 * uc > len(rec) or uc < 1:
            return rec
        for i in range(1, uc):
            end = i * 512 - 2
            if end + 2 <= len(rec):
                rec[end:end + 2] = rec[uo + 2 * i:uo + 2 * i + 2]
        return rec

    def unfixup(self, rec):
        rec = bytearray(rec)
        uo, uc = struct.unpack_from("<HH", rec, 4)
        usn = rec[uo:uo + 2]
        for i in range(1, uc):
            end = i * 512 - 2
            rec[uo + 2 * i:uo + 2 * i + 2] = rec[end:end + 2]
            rec[end:end + 2] = usn
        return rec

    @staticmethod
    def runs(data):
        out = []
        i = 0
        lcn = 0
        while i < len(data) and data[i]:
            h = data[i]
            ll, lo = h & 0xF, h >> 4
            i += 1
            if ll == 0 or i + ll + lo > len(data):
                break
            length = int.from_bytes(data[i:i + ll], "little")
            i += ll
            if lo:
                d = int.from_bytes(data[i:i + lo], "little", signed=True)
                lcn += d
                out.append((lcn, length))
            else:
                out.append((None, length))
            i += lo
        return out

    def attrs(self, rec):
        out = []
        if rec[:4] != b"FILE":
            return out
        off = struct.unpack_from("<H", rec, 0x14)[0]
        while off + 16 <= len(rec):
            t, l = struct.unpack_from("<II", rec, off)
            if t == 0xFFFFFFFF or l < 16 or off + l > len(rec):
                break
            nonres = rec[off + 8]
            nl, no = rec[off + 9], struct.unpack_from("<H", rec, off + 10)[0]
            name = bytes(rec[off + no:off + no + 2 * nl]).decode("utf-16-le", "replace")
            a = {"type": t, "off": off, "len": l, "name": name, "nonres": nonres}
            if nonres:
                mp = struct.unpack_from("<H", rec, off + 32)[0]
                a["runs"] = self.runs(bytes(rec[off + mp:off + l]))
                a["size"] = struct.unpack_from("<Q", rec, off + 48)[0]
            else:
                vl, vo = struct.unpack_from("<IH", rec, off + 16)
                a["voff"] = off + vo
                a["vlen"] = vl
            out.append(a)
            off += l
        return out

    def rec_off(self, n):
        pos = n * self.rec
        for lcn, length in self.mft_runs:
            b = length * self.cs
            if pos < b:
                return None if lcn is None else lcn * self.cs + pos
            pos -= b
        return None

    def nrecs(self):
        return sum(l for _, l in self.mft_runs) * self.cs // self.rec

    def record(self, n):
        o = self.rec_off(n)
        return None if o is None else self.fixup(self.img.read(o, self.rec))

    def write_record(self, n, rec, mirror=True):
        o = self.rec_off(n)
        self.img.write(o, bytes(self.unfixup(rec)))
        if mirror and n < 4:
            self.img.write(self.mirr_lcn * self.cs + n * self.rec, bytes(self.unfixup(rec)))

    def find(self, path):
        """MFT number of an entry in the root (one level, by name)."""
        rec = self.record(5)
        name = path.strip("/")
        for n in range(16, min(self.nrecs(), 4096)):
            r = self.record(n)
            if r is None or r[:4] != b"FILE":
                continue
            for a in self.attrs(r):
                if a["type"] == 0x30 and not a["nonres"]:
                    v = r[a["voff"]:a["voff"] + a["vlen"]]
                    parent = int.from_bytes(v[0:6], "little")
                    nl = v[64]
                    fname = bytes(v[66:66 + 2 * nl]).decode("utf-16-le", "replace")
                    if parent == 5 and fname.lower() == name.lower():
                        return n
        return None

    def regions(self):
        out = [(0, 512, "boot"), ((self.nsectors) * self.bps, 512, "backup-boot")]
        out.append((self.mirr_lcn * self.cs, 4 * self.rec, "mftmirr"))
        for n in range(min(self.nrecs(), 2048)):
            o = self.rec_off(n)
            if o is None:
                continue
            r = self.img.read(o, 4)
            if r == b"FILE":
                out.append((o, self.rec, "mft-%d" % n))
        # index blocks, $LogFile restart + first log pages, $Secure, $UpCase
        for n in range(min(self.nrecs(), 2048)):
            r = self.record(n)
            if r is None or r[:4] != b"FILE":
                continue
            for a in self.attrs(r):
                if a["nonres"] and a["type"] == 0xA0:
                    for lcn, ln in a["runs"]:
                        if lcn is None:
                            continue
                        for k in range(0, ln * self.cs, self.idx):
                            o = lcn * self.cs + k
                            if self.img.read(o, 4) == b"INDX":
                                out.append((o, self.idx, "indx-%d" % n))
                if a["nonres"] and n == 2 and a["type"] == 0x80 and a["runs"]:
                    lcn = a["runs"][0][0]
                    if lcn is not None:
                        out.append((lcn * self.cs, 8192, "logfile-restart"))
                        out.append((lcn * self.cs + 8192, 8192, "logfile-records"))
                if a["nonres"] and n == 9 and a["type"] == 0x80 and a["runs"]:
                    lcn = a["runs"][0][0]
                    if lcn is not None:
                        out.append((lcn * self.cs, 4096, "secure-sds"))
                if a["nonres"] and n == 10 and a["type"] == 0x80 and a["runs"]:
                    lcn = a["runs"][0][0]
                    if lcn is not None:
                        out.append((lcn * self.cs, 1024, "upcase"))
                if a["nonres"] and a["type"] == 0x80 and n >= 16:
                    # compressed / sparse data streams: the data itself
                    flags = struct.unpack_from("<H", r, a["off"] + 12)[0]
                    if flags & 0x0001 and a["runs"] and a["runs"][0][0] is not None:
                        out.append((a["runs"][0][0] * self.cs, 4096, "lznt1-%d" % n))
        return out

    def logfile(self):
        r = self.record(2)
        for a in self.attrs(r):
            if a["type"] == 0x80 and a["nonres"]:
                return a["runs"][0][0] * self.cs
        return None


# ------------------------------------------------- unclean $LogFile ------
#
# Real log records for simple operations, so that a replay can be tested:
# restart pages (version 1.1, one client "NTFS" in use, not clean), a
# checkpoint with an open-attribute table, a dirty-page table and a
# transaction table, and update records.  Scenarios:
#   redo-resident   a committed update of a small file's resident data
#                   (the MFT record still holds the old bytes)
#   undo-resident   an update that reached the record but whose
#                   transaction never committed (must be rolled back)
#   redo-create     a committed creation of /<dir>/logged.txt in a
#                   directory with a resident index: record initialised,
#                   $MFT:$BITMAP bit set, index entry added
#   bad-op          a committed record with an operation a replay must
#                   refuse (hot fix): the volume stays as it is
# Each prints what a correct replay leaves ("expect PATH|TEXT").

LOG_PS = 4096


class LogGen:
    def __init__(self, fs, img):
        self.fs, self.img = fs, img
        r = fs.record(2)
        self.lruns, self.lsize = None, 0
        for a in fs.attrs(r):
            if a["type"] == 0x80 and a["nonres"] and not a["name"]:
                self.lruns, self.lsize = a["runs"], a["size"]
        self.lsize -= self.lsize % LOG_PS
        bits = self.lsize.bit_length()
        self.seq_bits = 67 - bits
        self.fbits = 64 - self.seq_bits
        self.seq = 5
        self.first = 4 * LOG_PS
        self.page = bytearray(LOG_PS)
        self.pos = self.first + 0x40
        self.recs = []
        self.oat = []  # (mref, type, bytes_per_index)
        self.dpt = []  # (oat key, vcn, lcn, oldest lsn)

    def log_off(self, off):
        for lcn, length in self.lruns:
            b = length * self.fs.cs
            if off < b:
                return lcn * self.fs.cs + off
            off -= b
        raise ValueError("log offset")

    def lsn(self, pos):
        return (self.seq << self.fbits) | (pos >> 3)

    def oat_key(self, mref, typ, bpi=0):
        for i, e in enumerate(self.oat):
            if e[0] == mref and e[1] == typ:
                return 0x18 + 0x28 * i
        self.oat.append((mref, typ, bpi))
        return 0x18 + 0x28 * (len(self.oat) - 1)

    def add(self, rtype, tx, cd, undo_next=0):
        assert self.pos + 0x30 + len(cd) <= LOG_PS + self.first
        lsn = self.lsn(self.pos)
        prev = self.recs[-1][0] if self.recs else 0
        hdr = struct.pack("<qqqIHHIIH6x", lsn, prev, undo_next, len(cd),
                          0, 0, rtype, tx, 0)
        o = self.pos - self.first
        self.page[o:o + 0x30] = hdr
        self.page[o + 0x30:o + 0x30 + len(cd)] = cd
        self.recs.append((lsn, self.pos))
        self.pos = (self.pos + 0x30 + len(cd) + 7) & ~7
        return lsn

    @staticmethod
    def op(redo_op, undo_op, redo, undo, target=0, vcn=0, lcns=(),
           rec_off=0, attr_off=0, cidx=0):
        lcnb = b"".join(struct.pack("<q", l) for l in lcns)
        ro = 0x20 + len(lcnb)
        rl = len(redo)
        uo = ro + ((rl + 7) & ~7)
        ul = len(undo)
        h = struct.pack("<HHHHHHHHHHHHq", redo_op, undo_op, ro, rl,
                        uo if ul else 0, ul, target, len(lcns), rec_off,
                        attr_off, cidx, 0, vcn)
        cd = bytearray(h + lcnb + redo + bytes((-rl) % 8) + undo +
                       bytes((-ul) % 8))
        return bytes(cd)

    def mft_target(self, n):
        pos = n * self.fs.rec
        vcn = pos // self.fs.cs
        cidx = (pos % self.fs.cs) // 512
        lcns = []
        need = max(1, (self.fs.rec + self.fs.cs - 1) // self.fs.cs)
        for k in range(need):
            v = vcn + k
            for lcn, length in self.fs.mft_runs:
                if v < length:
                    lcns.append(lcn + v)
                    break
                v -= length
        return vcn, lcns, cidx

    def table(self, esz, entries):
        t = struct.pack("<HHH6xIII", esz, len(entries), len(entries), 0,
                        0, 0)
        return t + b"".join(entries)

    def finish(self, txs, oldest=None):
        """Dumps + checkpoint after the updates are known: the dumps go
        first in the page, so the records are rebuilt in order."""
        updates = self.recs_spec
        self.page = bytearray(LOG_PS)
        self.pos = self.first + 0x40
        self.recs = []
        oat = [struct.pack("<IIIBBBBQQQ", 0xFFFFFFFF, bpi, typ, 0, 0, 0,
                           0, mref, 0, 0) for mref, typ, bpi in self.oat]
        oat_t = self.table(0x28, oat)
        a_oat = self.add(1, 0, self.op(0x1D, 0, oat_t, b""))
        # dirty pages: every target, oldest LSN = the first update's
        first_upd = self.lsn(self.pos) + 0x10000  # placeholder, fixed below
        dpt_n = len(self.dpt)
        a_dpt = self.add(1, 0, self.op(0x1F, 0, bytes(0x18 + 0x28 * dpt_n), b""))
        a_tt = self.add(1, 0, self.op(0x20, 0, bytes(0x18 + 0x28 * len(txs)), b""))
        ck = self.add(2, 0, struct.pack("<IIqqqqqIIII", 0, 0, 0, a_oat, 0,
                                        a_dpt, a_tt, len(oat_t), 0,
                                        0x18 + 0x28 * dpt_n,
                                        0x18 + 0x28 * len(txs)))
        # check_point_start: the checkpoint record itself
        o = self.recs[-1][1] - self.first + 0x30
        struct.pack_into("<q", self.page, o + 8, ck)
        lsns = []
        for spec in updates:
            rtype, tx, cd, chain = spec
            un = 0
            if chain:
                prior = [l for l, t2 in zip(lsns, updates) if t2[1] == tx and t2[3]]
                un = prior[-1] if prior else 0
            lsns.append(self.add(rtype, tx, cd, un))
        first_upd = lsns[0] if lsns else ck
        # now the DPT and TT contents
        dents = []
        for key, vcn, lcn, _ in self.dpt:
            dents.append(struct.pack("<IIIIqqq", 0xFFFFFFFF, key, 1, 1, vcn,
                                     first_upd, lcn))
        dpt_t = self.table(0x28, dents)
        tents = []
        for key, state in txs:
            tents.append(struct.pack("<IB3xqqqII", 0xFFFFFFFF, state, 0, 0,
                                     0, 0, 0))
        tt_t = self.table(0x28, tents)
        for lsn, pos, tab in ((a_dpt, None, dpt_t), (a_tt, None, tt_t)):
            for l2, p2 in self.recs:
                if l2 == lsn:
                    o = p2 - self.first + 0x30 + 0x20
                    self.page[o:o + len(tab)] = tab
        last = self.recs[-1][0]
        self.write(ck, last, oldest if oldest else ck)
        return lsns

    def write(self, ck, last, oldest):
        fs, img = self.fs, self.img
        pg = bytearray(self.page)
        pg[0:4] = b"RCRD"
        struct.pack_into("<HH", pg, 4, 0x28, LOG_PS // 512 + 1)
        struct.pack_into("<qIHHH", pg, 8, last, 1, 1, 1,
                         self.recs[-1][1] - self.first + 0x30 +
                         struct.unpack_from("<I", pg, self.recs[-1][1] - self.first + 0x18)[0])
        struct.pack_into("<q", pg, 0x20, last)
        struct.pack_into("<H", pg, 0x28, 7)
        img.write(self.log_off(self.first), bytes(fs.unfixup(pg)))
        if getattr(self, "tails", True):
            # the ping-pong copies of the tail page (their last LSN
            # tells which page they copy)
            tp = bytearray(pg)
            for i in (2, 3):
                img.write(self.log_off(i * LOG_PS), bytes(fs.unfixup(tp)))
        for page in (0, LOG_PS):
            p = bytearray(LOG_PS)
            p[0:4] = b"RSTR"
            struct.pack_into("<HH", p, 4, 0x1E, LOG_PS // 512 + 1)
            struct.pack_into("<qIIHhh", p, 8, 0, LOG_PS, LOG_PS, 0x30, 1, 1)
            ra = 0x30
            struct.pack_into("<qHHHHIHHqIHHI", p, ra, last, 1, 0xFFFF, 0, 0,
                             self.seq_bits, 0x30 + 0xA0, 0x30, self.lsize,
                             0x40, 0x30, 0x40, 1)
            c = ra + 0x30
            struct.pack_into("<qqHHH", p, c, oldest, ck, 0xFFFF, 0xFFFF, 0)
            struct.pack_into("<I", p, c + 0x1C, 8)
            p[c + 0x20:c + 0x28] = "NTFS".encode("utf-16-le")
            struct.pack_into("<H", p, 0x1E, 3)
            img.write(self.log_off(page), bytes(fs.unfixup(p)))


def find_resident_file(fs, minlen=8):
    for n in range(24, min(fs.nrecs(), 4096)):
        r = fs.record(n)
        if r is None or r[:4] != b"FILE" or not (struct.unpack_from("<H", r, 0x16)[0] & 1):
            continue
        if struct.unpack_from("<H", r, 0x16)[0] & 2:
            continue
        name, data = None, None
        for a in fs.attrs(r):
            if a["type"] == 0x30 and not a["nonres"] and name is None:
                v = r[a["voff"]:a["voff"] + a["vlen"]]
                if int.from_bytes(v[0:6], "little") != 5:
                    break
                name = bytes(v[66:66 + 2 * v[64]]).decode("utf-16-le")
            if a["type"] == 0x80 and not a["name"] and not a["nonres"] and a["vlen"] >= minlen:
                data = a
        if name and data:
            return n, r, name, data
    return None


def find_resident_dir(fs):
    for n in range(24, min(fs.nrecs(), 4096)):
        r = fs.record(n)
        if r is None or r[:4] != b"FILE" or (struct.unpack_from("<H", r, 0x16)[0] & 3) != 3:
            continue
        name, root, alloc = None, None, False
        for a in fs.attrs(r):
            if a["type"] == 0x30 and not a["nonres"] and name is None:
                v = r[a["voff"]:a["voff"] + a["vlen"]]
                if int.from_bytes(v[0:6], "little") != 5:
                    break
                name = bytes(v[66:66 + 2 * v[64]]).decode("utf-16-le")
            if a["type"] == 0x90:
                root = a
            if a["type"] == 0xA0:
                alloc = True
        if name and root and not alloc and r and struct.unpack_from("<I", r, 0x18)[0] + 256 < fs.rec:
            return n, r, name, root
    return None


def logfile_scenario(path, scen):
    img = Img(path, "r+b")
    fs = Ntfs(img)
    g = LogGen(fs, img)
    g.recs_spec = []
    mft = g.oat_key(0, 0x80)
    tx = 0x18
    if scen in ("redo-resident", "undo-resident", "bad-op"):
        n, r, name, d = find_resident_file(fs)
        old = bytes(r[d["voff"]:d["voff"] + 8])
        new = b"REPLAYED" if scen != "undo-resident" else b"UNDONE!!"
        vcn, lcns, cidx = g.mft_target(n)
        g.dpt.append((mft, vcn, lcns[0], 0))
        opc = 0x07 if scen != "bad-op" else 0x17
        cd = g.op(opc, 0x07, new, old, mft, vcn, lcns, d["off"],
                  d["voff"] - d["off"], cidx)
        g.recs_spec.append((1, tx, cd, True))
        txs = [(tx, 1)]
        if scen != "undo-resident":
            g.recs_spec.append((1, tx, g.op(0x1A, 0, b"", b""), False))
        lsns = g.finish(txs)
        if scen == "undo-resident":
            # the update reached the record (page LSN = the record's)
            r[d["voff"]:d["voff"] + 8] = new
            struct.pack_into("<q", r, 8, lsns[0])
            fs.write_record(n, r)
            print("expect /%s|%s" % (name, old.decode("latin-1")))
        elif scen == "bad-op":
            print("expect /%s|%s" % (name, old.decode("latin-1")))
        else:
            print("expect /%s|%s" % (name, new.decode()))
    elif scen == "redo-create":
        dn, dr, dname, root = find_resident_dir(fs)
        # a free record
        bmrec = fs.record(0)
        bm = None
        for a in fs.attrs(bmrec):
            if a["type"] == 0xB0:
                bm = a
        if bm["nonres"]:
            blcn = bm["runs"][0][0]
            bits = img.read(blcn * fs.cs, fs.cs)
        else:
            bits = bmrec[bm["voff"]:bm["voff"] + bm["vlen"]]
        newn = next(i for i in range(24, len(bits) * 8)
                    if not (bits[i // 8] >> (i % 8)) & 1 and i < fs.nrecs())
        oldrec = fs.record(newn)
        seqno = struct.unpack_from("<H", oldrec, 0x10)[0] if oldrec[:4] == b"FILE" else 1
        seqno = seqno or 1
        # the new record image
        rec = bytearray(fs.rec)
        rec[0:4] = b"FILE"
        struct.pack_into("<HHqHHHHII", rec, 4, 0x30, fs.rec // 512 + 1, 0,
                         seqno, 1, 0x38, 1, 0, fs.rec)
        struct.pack_into("<QHHI", rec, 0x20, 0, 3, 0, newn)
        struct.pack_into("<H", rec, 0x30, 1)  # USN
        t = 132_000_000_000_000_000
        si = struct.pack("<qqqqI12x", t, t, t, t, 0x20)
        nm = "logged.txt".encode("utf-16-le")
        content = b"hello log"
        fn = struct.pack("<Qqqqqqqi4xBB", dn | (struct.unpack_from("<H", dr, 0x10)[0] << 48),
                         t, t, t, t, 16, len(content), 0x20, 10, 1) + nm
        off = 0x38
        def resattr(typ, val, inst, idx=0):
            l = (0x18 + len(val) + 7) & ~7
            b = bytearray(l)
            struct.pack_into("<IIBBHHHIHBB", b, 0, typ, l, 0, 0, 0, 0, inst,
                             len(val), 0x18, idx, 0)
            b[0x18:0x18 + len(val)] = val
            return b
        for typ, val, inst, idx in ((0x10, si, 0, 0), (0x30, fn, 1, 1), (0x80, content, 2, 0)):
            b = resattr(typ, val, inst, idx)
            rec[off:off + len(b)] = b
            off += len(b)
        struct.pack_into("<I", rec, off, 0xFFFFFFFF)
        used = off + 8
        struct.pack_into("<I", rec, 0x18, used)
        vcn, lcns, cidx = g.mft_target(newn)
        g.dpt.append((mft, vcn, lcns[0], 0))
        g.recs_spec.append((1, tx, g.op(0x02, 0x03, bytes(rec[:used]), b"", mft, vcn, lcns, 0, 0, cidx), True))
        # the $MFT:$BITMAP bit
        byte = newn // 8
        if bm["nonres"]:
            key = g.oat_key(0, 0xB0)
            g.dpt.append((key, 0, blcn, 0))
            rng = struct.pack("<II", newn, 1)
            g.recs_spec.append((1, tx, g.op(0x15, 0x16, rng, rng, key, 0, [blcn]), True))
        else:
            vcn0, lcns0, cidx0 = g.mft_target(0)
            nb = bytes([bits[byte] | (1 << (newn % 8))])
            g.dpt.append((mft, vcn0, lcns0[0], 0))
            g.recs_spec.append((1, tx, g.op(0x07, 0x07, nb, bytes([bits[byte]]), mft, vcn0, lcns0,
                                            bm["off"], bm["voff"] - bm["off"] + byte, cidx0), True))
        # the index entry, before the end entry of the root
        dvcn, dlcns, dcidx = g.mft_target(dn)
        g.dpt.append((mft, dvcn, dlcns[0], 0))
        rv = dr[root["voff"]:root["voff"] + root["vlen"]]
        eo = struct.unpack_from("<I", rv, 16)[0]
        il = struct.unpack_from("<I", rv, 20)[0]
        # entries: find insertion point by name order (upcase-ish compare)
        pos = 16 + eo
        while pos < 16 + il:
            el, kl, fl = struct.unpack_from("<HHH", rv, pos + 8)
            if fl & 2:
                break
            kn = bytes(rv[pos + 16 + 66:pos + 16 + 66 + 2 * rv[pos + 16 + 64]]).decode("utf-16-le")
            if kn.upper() > "LOGGED.TXT":
                break
            pos += el
        ent = bytearray(16 + ((len(fn) + 7) & ~7))
        struct.pack_into("<QHHH", ent, 0, newn | (seqno << 48), len(ent), len(fn), 0)
        ent[16:16 + len(fn)] = fn
        g.recs_spec.append((1, tx, g.op(0x0C, 0x0D, bytes(ent), b"", mft, dvcn, dlcns,
                                        root["off"], root["voff"] - root["off"] + pos, dcidx), True))
        g.recs_spec.append((1, tx, g.op(0x1A, 0, b"", b""), False))
        g.finish([(tx, 1)])
        print("expect /%s/logged.txt|%s" % (dname, content.decode()))
    else:
        sys.exit("ntfs-tweak: unknown log scenario %s" % scen)


def ntfs_tweak(path, ops):
    img = Img(path, "r+b")
    fs = Ntfs(img)
    for op in ops:
        if op in ("dirty", "clean") or op.startswith("vflags-"):
            r = fs.record(3)
            done = False
            for a in fs.attrs(r):
                if a["type"] == 0x70 and not a["nonres"]:
                    fo = a["voff"] + 10
                    fl = struct.unpack_from("<H", r, fo)[0]
                    if op.startswith("vflags-"):
                        fl |= int(op[7:], 0)
                    else:
                        fl = fl | 1 if op == "dirty" else fl & ~1
                    struct.pack_into("<H", r, fo, fl)
                    done = True
            if not done:
                sys.exit("ntfs-tweak: no $VOLUME_INFORMATION")
            fs.write_record(3, r)
        elif op == "logfile-unclean":
            lo = fs.logfile()
            found = 0
            for page in (0, 4096):
                p = fs.fixup(img.read(lo + page, 4096))
                if p[:4] != b"RSTR":
                    continue
                found += 1
                ra = struct.unpack_from("<H", p, 0x18)[0]
                fl = struct.unpack_from("<H", p, ra + 0x0E)[0]
                struct.pack_into("<H", p, ra + 0x0E, fl & ~0x0002)
                if struct.unpack_from("<H", p, ra + 0x0C)[0] == 0xFFFF:
                    struct.pack_into("<H", p, ra + 0x0C, 0)
                img.write(lo + page, bytes(fs.unfixup(p)))
            if not found:
                # An emptied (all 0xFF) $LogFile: write the two restart
                # pages of a log with one client in use that was not
                # closed cleanly (current LSN past the client's restart
                # LSN), so the volume needs a replay.
                r = fs.record(2)
                size = 0
                for a in fs.attrs(r):
                    if a["type"] == 0x80 and a["nonres"]:
                        size = a["size"]
                for page in (0, 4096):
                    p = bytearray(4096)
                    p[0:4] = b"RSTR"
                    struct.pack_into("<HH", p, 4, 0x1E, 4096 // 512 + 1)
                    struct.pack_into("<qIIHhh", p, 8, 0, 4096, 4096, 0x30, 1, 1)
                    ra = 0x30
                    # current LSN, clients, free list, in-use list, flags,
                    # sequence bits, area length, client array offset,
                    # file size, last data length, record header length,
                    # page data offset, open count
                    struct.pack_into("<qHHHHIHHqIHHI", p, ra, 0x1000,
                                     1, 0xFFFF, 0, 0, 67 - size.bit_length(), 48 + 160, 48,
                                     size, 0, 0x30, 0x40, 1)
                    c = ra + 48
                    # oldest LSN, restart LSN, prev, next, seq, name
                    struct.pack_into("<qqHHH", p, c, 0x800, 0x800, 0xFFFF,
                                     0xFFFF, 0)
                    # name length (bytes) at 0x1C, the name at 0x20
                    struct.pack_into("<I", p, c + 0x1C, 8)
                    p[c + 0x20:c + 0x28] = "NTFS".encode("utf-16-le")
                    struct.pack_into("<H", p, 0x1E, 1)  # the USN
                    img.write(lo + page, bytes(fs.unfixup(p)))
        elif op.startswith("log-"):
            img.f.flush()
            logfile_scenario(path, op[4:])
        elif op.startswith("fragfree-") or op == "unfragfree":
            r6 = fs.record(6)
            bruns, bsize = None, 0
            for a in fs.attrs(r6):
                if a["type"] == 0x80 and not a["name"] and a["nonres"]:
                    bruns, bsize = a["runs"], a["size"]
            if not bruns:
                sys.exit("ntfs-tweak: $Bitmap not found")
            bm = bytearray()
            for lcn, ln in bruns:
                bm += img.read(lcn * fs.cs, ln * fs.cs)
            bm = bm[:bsize]
            ncl = fs.nsectors * fs.bps // fs.cs
            state = path + ".fragstate"
            if op == "unfragfree":
                marked = [int(x) for x in open(state).read().split()]
                for c in marked:
                    bm[c // 8] &= ~(1 << (c % 8)) & 0xFF
            else:
                run = int(op[9:])
                marked, k = [], 0
                for c in range(ncl):
                    if not (bm[c // 8] >> (c % 8)) & 1:
                        if (k // run) % 2:
                            bm[c // 8] |= 1 << (c % 8)
                            marked.append(c)
                        k += 1
                open(state, "w").write(" ".join(str(c) for c in marked))
            pos = 0
            for lcn, ln in bruns:
                n = min(ln * fs.cs, len(bm) - pos)
                if n <= 0:
                    break
                img.write(lcn * fs.cs, bytes(bm[pos:pos + n]))
                pos += n
        elif op.startswith("dospair-"):
            short = op[8:]
            done = False
            for n in range(16, fs.nrecs()):
                r = fs.record(n)
                if r is None or r[:4] != b"FILE":
                    continue
                fns = []
                for a in fs.attrs(r):
                    if a["type"] == 0x30 and not a["nonres"]:
                        v = r[a["voff"]:a["voff"] + a["vlen"]]
                        nl = v[64]
                        fns.append((a["voff"], int.from_bytes(v[0:6], "little"),
                                    bytes(v[66:66 + 2 * nl]).decode("utf-16-le", "replace")))
                hit = [f for f in fns if f[2] == short]
                if not hit:
                    continue
                par = hit[0][1]
                other = [f for f in fns if f[1] == par and f[2] != short]
                if len(other) != 1:
                    sys.exit("ntfs-tweak: %s: need exactly one other name" % short)
                types = {short: 2, other[0][2]: 1}
                r[hit[0][0] + 65] = 2
                r[other[0][0] + 65] = 1
                fs.write_record(n, r)
                d = fs.record(par)
                patched = 0
                for a in fs.attrs(d):
                    if a["type"] != 0x90 or a["nonres"]:
                        continue
                    ib = a["voff"] + 16
                    eo = ib + struct.unpack_from("<I", d, ib)[0]
                    end = ib + struct.unpack_from("<I", d, ib + 4)[0]
                    while eo + 16 <= end:
                        ln, kl, fl = struct.unpack_from("<HHI", d, eo + 8)
                        if fl & 2 or ln < 16:
                            break
                        if kl >= 66:
                            k = eo + 16
                            nl = d[k + 64]
                            nm = bytes(d[k + 66:k + 66 + 2 * nl]).decode("utf-16-le", "replace")
                            if nm in types and int.from_bytes(d[eo:eo + 6], "little") == n:
                                d[k + 65] = types[nm]
                                patched += 1
                        eo += ln
                if patched != 2:
                    sys.exit("ntfs-tweak: %s: directory index not resident" % short)
                fs.write_record(par, d)
                done = True
                break
            if not done:
                sys.exit("ntfs-tweak: no name %s" % short)
        elif op == "mftmirr-mismatch":
            o = fs.mirr_lcn * fs.cs + 1 * fs.rec
            r = bytearray(img.read(o, fs.rec))
            r[0x10] ^= 0x01  # sequence number byte
            img.write(o, bytes(r))
        elif op == "hiber":
            n = fs.find("/hiberfil.sys")
            if n is None:
                sys.exit("ntfs-tweak: no /hiberfil.sys")
            r = fs.record(n)
            for a in fs.attrs(r):
                if a["type"] == 0x80 and not a["name"]:
                    if a["nonres"]:
                        img.write(a["runs"][0][0] * fs.cs, b"hibr")
                    else:
                        r[a["voff"]:a["voff"] + 4] = b"hibr"
                        fs.write_record(n, r)
        else:
            sys.exit("ntfs-tweak: unknown op %s" % op)


# ------------------------------------------- system-compressed files ------
#
# Test data for the WOF read path: XPRESS Huffman and LZX compressors
# (own code, a straightforward greedy parse) and the two tweaks
#   wof-blob SRC OUT FMT     the WofCompressedData stream for the file
#                            contents SRC (chunk table + chunks), FMT 0..3
#                            (XPRESS4K, LZX, XPRESS8K, XPRESS16K)
#   wof-reparse NAME FMT     give root-level file NAME the WOF reparse
#                            point (file provider, version 1, format FMT)

# --------------------------------------------------------------------------
# Huffman helpers
# --------------------------------------------------------------------------

def huff_lengths(freqs, limit, rng=None, skew=False):
    """Complete, length-limited Huffman code lengths for freqs.
    skew=True replaces the real frequencies of used symbols by a steep
    (Fibonacci-like) distribution to force long codewords."""
    n = len(freqs)
    used = [i for i in range(n) if freqs[i] > 0]
    if not used:
        return [0] * n
    f = list(freqs)
    if len(used) == 1:
        extra = 0 if used[0] != 0 else 1
        f[extra] = 1
        used = sorted(used + [extra])
    if skew:
        order = list(used)
        if rng:
            rng.shuffle(order)
        a, b = 1, 1
        for s in order:
            f[s] = a
            a, b = b, min(a + b, 1 << 40)
    while True:
        heap = [(f[s], s, s) for s in used]   # (weight, tiebreak, node)
        heapq.heapify(heap)
        parent = {}
        nxt = n
        while len(heap) > 1:
            w1, _, a1 = heapq.heappop(heap)
            w2, _, a2 = heapq.heappop(heap)
            parent[a1] = nxt
            parent[a2] = nxt
            heapq.heappush(heap, (w1 + w2, nxt, nxt))
            nxt += 1
        lens = [0] * n
        for s in used:
            d = 0
            x = s
            while x in parent:
                x = parent[x]
                d += 1
            lens[s] = d
        if max(lens) <= limit:
            assert sum(2.0 ** -l for l in lens if l) == 1.0
            return lens
        for s in used:
            f[s] = (f[s] >> 1) | 1


def canon_codes(lens):
    codes = [0] * len(lens)
    code = 0
    for l in range(1, max(lens) + 1 if any(lens) else 1):
        for s in range(len(lens)):
            if lens[s] == l:
                codes[s] = code
                code += 1
        code <<= 1
    return codes

# --------------------------------------------------------------------------
# Match finder (hash chains on 3-byte prefixes)
# --------------------------------------------------------------------------

class MatchFinder:
    def __init__(self, data, max_chain=48):
        self.data = data
        self.head = {}
        self.prev = [-1] * len(data)
        self.max_chain = max_chain
        self.inserted = 0

    def insert_upto(self, pos):
        d = self.data
        while self.inserted < pos:
            i = self.inserted
            if i + 3 <= len(d):
                k = d[i:i + 3]
                self.prev[i] = self.head.get(k, -1)
                self.head[k] = i
            self.inserted += 1

    def matchlen(self, i, j, maxlen):
        d = self.data
        n = 0
        while n < maxlen and d[i + n] == d[j + n]:
            n += 1
        return n

    def find(self, pos, maxlen, maxoff, minpos=0):
        """Longest match at pos (len >= 3) among earlier positions >= minpos."""
        self.insert_upto(pos)
        d = self.data
        if pos + 3 > len(d) or maxlen < 3:
            return 0, 0
        cand = self.head.get(d[pos:pos + 3], -1)
        best = (0, 0)
        chain = 0
        while cand >= 0 and chain < self.max_chain:
            off = pos - cand
            if off > maxoff or cand < minpos:
                break
            l = self.matchlen(cand, pos, maxlen)
            if l > best[0]:
                best = (l, off)
                if l == maxlen:
                    break
            cand = self.prev[cand]
            chain += 1
        return best

# --------------------------------------------------------------------------
# XPRESS Huffman compressor
# --------------------------------------------------------------------------

class XpressWriter:
    """Lays out bits and plain bytes exactly in the order the decoder loads
    them: before a plain byte is written, max(2, 1 + ceil(C / 16)) words
    must already be reserved, C = number of bits written so far."""

    def __init__(self):
        self.out = bytearray()
        self.slots = []
        self.C = 0
        self.acc = 0
        self.n = 0

    def reserve(self, k):
        while len(self.slots) < k:
            self.slots.append(len(self.out))
            self.out += b"\0\0"

    def bits(self, v, nb):
        for i in range(nb - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1)
            self.C += 1
            self.n += 1
            if self.n == 16:
                idx = self.C // 16 - 1
                self.reserve(idx + 1)
                p = self.slots[idx]
                self.out[p:p + 2] = struct.pack("<H", self.acc)
                self.acc = 0
                self.n = 0

    def byte(self, b):
        self.reserve(max(2, 1 + (self.C + 15) // 16))
        self.out.append(b)

    def finish(self, trim_tail=False):
        if self.n:
            self.bits(0, 16 - self.n)
        need = max(2, self.C // 16 + (0 if trim_tail else 1))
        self.reserve(need)
        return bytes(self.out)


def xpress_parse(data, rng, opts):
    """Greedy LZ77 parse: list of ('L', byte) / ('M', length, offset)."""
    mf = MatchFinder(data)
    items = []
    i = 0
    n = len(data)
    while i < n:
        l, off = mf.find(i, n - i, 65535)
        if l >= 3:
            if opts.get("vary") and l > 4 and rng.random() < 0.3:
                l = rng.randint(3, l)
            items.append(("M", l, off))
            i += l
        else:
            items.append(("L", data[i]))
            i += 1
    return items


def xpress_compress(data, rng, opts=None):
    opts = opts or {}
    items = xpress_parse(data, rng, opts)
    syms = []
    for it in items:
        if it[0] == "L":
            syms.append(it[1])
        else:
            l, off = it[1], it[2]
            olog = off.bit_length() - 1
            syms.append(256 + (olog << 4) + min(l - 3, 15))
    freqs = [0] * 512
    for s in syms:
        freqs[s] += 1
    if opts.get("eof", True):
        freqs[256] += 1
    lens = huff_lengths(freqs, 15, rng, skew=opts.get("skew", False))
    codes = canon_codes(lens)
    table = bytearray(256)
    for k in range(256):
        table[k] = lens[2 * k] | (lens[2 * k + 1] << 4)
    w = XpressWriter()
    for it, s in zip(items, syms):
        w.bits(codes[s], lens[s])
        if it[0] == "M":
            l, off = it[1], it[2]
            L = l - 3
            if L >= 15:
                form = opts.get("lenform", "auto")
                if form == "auto":
                    form = "b" if L - 15 < 255 else "w"
                if form == "b" and L - 15 >= 255:
                    form = "w"
                if form == "b":
                    w.byte(L - 15)
                elif form == "w":
                    w.byte(255)
                    w.byte(L & 255)
                    w.byte(L >> 8)
                else:   # 32-bit form
                    w.byte(255)
                    w.byte(0)
                    w.byte(0)
                    for k in range(4):
                        w.byte((L >> (8 * k)) & 255)
            olog = off.bit_length() - 1
            w.bits(off - (1 << olog), olog)
    if opts.get("eof", True):
        w.bits(codes[256], lens[256])
    return bytes(table) + w.finish(opts.get("trim_tail", False))

# --------------------------------------------------------------------------
# LZX compressor
# --------------------------------------------------------------------------

E8_SIZE = 12000000
NUM_SLOTS = 30
MAIN_SYMS = 256 + 8 * NUM_SLOTS
LEN_SYMS = 249


def e8_forward(data):
    d = bytearray(data)
    n = len(d)
    if n <= 10:
        return bytes(d)
    i = 0
    while i < n - 10:
        if d[i] != 0xE8:
            i += 1
            continue
        rel = struct.unpack_from("<i", d, i + 1)[0]
        if -i <= rel < E8_SIZE:
            a = rel + i if rel < E8_SIZE - i else rel - E8_SIZE
            struct.pack_into("<i", d, i + 1, a)
        i += 5
    return bytes(d)


def e8_backward(data):   # python mirror of the decoder (sanity only)
    d = bytearray(data)
    n = len(d)
    if n <= 10:
        return bytes(d)
    i = 0
    while i < n - 10:
        if d[i] != 0xE8:
            i += 1
            continue
        a = struct.unpack_from("<i", d, i + 1)[0]
        if -i <= a < E8_SIZE:
            struct.pack_into("<i", d, i + 1, a - i if a >= 0 else a + E8_SIZE)
        i += 5
    return bytes(d)


def slot_base(s):
    return (2 + (s & 1)) << ((s - 2) >> 1)


def slot_bits(s):
    return (s - 2) >> 1


def offset_slot(formatted):
    for s in range(NUM_SLOTS - 1, 2, -1):
        if slot_base(s) <= formatted:
            return s
    raise ValueError(formatted)


class LzxWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def bits(self, v, nb):
        assert 0 <= v < (1 << nb) or nb == 0
        self.acc = (self.acc << nb) | v
        self.n += nb
        while self.n >= 16:
            self.n -= 16
            self.out += struct.pack("<H", (self.acc >> self.n) & 0xFFFF)
        self.acc &= (1 << self.n) - 1

    def aligned(self):
        return self.n == 0

    def align_uncompressed(self):
        self.bits(0, 16 - self.n if self.n else 16)

    def raw(self, b):
        assert self.n == 0
        self.out += b

    def finish(self):
        if self.n:
            self.bits(0, 16 - self.n)
        return bytes(self.out)


def lzx_parse_block(data, start, end, R, mf, rng, opts):
    """Parse data[start:end] with recent-offset queue R (modified).
    Returns list of ('L', b) / ('M', len, slot, footer)."""
    items = []
    i = start
    while i < end:
        maxlen = min(257, end - i)
        best = None
        if maxlen >= 2:
            for k in range(3):
                off = R[k]
                if 1 <= off <= i:
                    l = mf.matchlen(i - off, i, maxlen)
                    if l >= 2 and (best is None or l > best[0]):
                        best = (l, off, k)
            l, off = mf.find(i, maxlen, min(i, 32765))
            if l >= 3 and (best is None or l > best[0] + 1):
                best = (l, off, None)
        if best is None:
            items.append(("L", data[i]))
            i += 1
            continue
        l, off, k = best
        if opts.get("vary") and l > 2 and rng.random() < 0.25:
            l = rng.randint(2 if k is not None else 3, l)
        if k is not None and opts.get("explicit_rep") and rng.random() < 0.2 \
                and off <= 32765:
            k = None   # send a recent offset explicitly (legal, pushes dup)
        if k == 0:
            items.append(("M", l, 0, 0))
        elif k == 1:
            R[0], R[1] = R[1], R[0]
            items.append(("M", l, 1, 0))
        elif k == 2:
            R[0], R[2] = R[2], R[0]
            items.append(("M", l, 2, 0))
        else:
            fo = off + 2
            s = offset_slot(fo)
            items.append(("M", l, s, fo - slot_base(s)))
            R[2], R[1], R[0] = R[1], R[0], off
        i += l
    return items


def pretree_tokens(new, prev, rng, opts):
    toks = []   # (sym, extra_value, extra_bits, second_sym or None)
    n = len(new)
    i = 0
    while i < n:
        v = new[i]
        j = i
        while j < n and new[j] == v:
            j += 1
        run = j - i
        use19 = opts.get("mix19") and rng.random() < 0.4
        if v == 0 and run >= 20 and not use19:
            r = min(run, 51)
            toks.append((18, r - 20, 5, None))
            i += r
        elif v == 0 and run >= 4 and not use19:
            r = min(run, 19)
            toks.append((17, r - 4, 4, None))
            i += r
        elif run >= 4:
            r = min(run, 5)
            toks.append((19, r - 4, 1, (prev[i] - v) % 17))
            i += r
        else:
            toks.append(((prev[i] - v) % 17, 0, 0, None))
            i += 1
    return toks


def write_lens(w, new, prev, rng, opts):
    toks = pretree_tokens(new, prev, rng, opts)
    freqs = [0] * 20
    for t in toks:
        freqs[t[0]] += 1
        if t[3] is not None:
            freqs[t[3]] += 1
    plens = huff_lengths(freqs, 15, rng, skew=opts.get("skew_pre", False))
    pcodes = canon_codes(plens)
    for l in plens:
        w.bits(l, 4)
    for sym, ev, eb, s2 in toks:
        w.bits(pcodes[sym], plens[sym])
        w.bits(ev, eb)
        if s2 is not None:
            w.bits(pcodes[s2], plens[s2])


def lzx_compress(data, rng, plan, opts=None):
    """plan: list of (block_type, size) with sizes summing to len(data);
    types 'V', 'A', 'U'.  Returns the compressed chunk."""
    opts = opts or {}
    pre = e8_forward(data) if opts.get("e8", True) else data
    assert sum(s for _, s in plan) == len(pre)
    mf = MatchFinder(pre)
    R = [1, 1, 1]
    prev_main = [0] * MAIN_SYMS
    prev_len = [0] * LEN_SYMS
    w = LzxWriter()
    pos = 0
    stats = {"u_aligned_pad": 0}
    for btype, size in plan:
        end = pos + size
        tcode = {"V": 1, "A": 2, "U": 3}[btype]
        w.bits(tcode, 3)
        if (size == 32768 or opts.get("force_default_size")) and \
                not opts.get("never_default_size"):
            w.bits(1, 1)
        else:
            w.bits(0, 1)
            w.bits(size, 16)
        if btype == "U":
            if w.aligned():
                stats["u_aligned_pad"] += 1
            w.align_uncompressed()
            rr = list(R)
            if opts.get("u_new_r") and pos >= 3:
                rr = [rng.randint(1, pos) for _ in range(3)]
            w.raw(struct.pack("<III", *rr))
            R = rr
            w.raw(pre[pos:end])
            if size & 1:
                w.raw(b"\0")
            mf.insert_upto(end)
            pos = end
            continue
        items = lzx_parse_block(pre, pos, end, R, mf, rng, opts)
        mainf = [0] * MAIN_SYMS
        lenf = [0] * LEN_SYMS
        alf = [0] * 8
        enc = []
        for it in items:
            if it[0] == "L":
                mainf[it[1]] += 1
                enc.append((it[1], None, None))
                continue
            _, l, s, footer = it
            lh = min(l - 2, 7)
            ms = 256 + s * 8 + lh
            mainf[ms] += 1
            ls = l - 9 if lh == 7 else None
            if ls is not None:
                lenf[ls] += 1
            if btype == "A" and s >= 3 and slot_bits(s) >= 3:
                alf[footer & 7] += 1
            enc.append((ms, ls, (s, footer)))
        skew = opts.get("skew", False)
        main_lens = huff_lengths(mainf, 16, rng, skew=skew)
        len_lens = huff_lengths(lenf, 16, rng, skew=skew)
        if btype == "A":
            if opts.get("empty_aligned_ok") and sum(alf) == 0:
                al_lens = [0] * 8
            else:
                if sum(alf) == 0:
                    alf = [1] * 8
                al_lens = huff_lengths(alf, 7, rng)
            al_codes = canon_codes(al_lens)
            for l in al_lens:
                w.bits(l, 3)
        write_lens(w, main_lens[:256], prev_main[:256], rng, opts)
        write_lens(w, main_lens[256:], prev_main[256:], rng, opts)
        write_lens(w, len_lens, prev_len, rng, opts)
        prev_main = main_lens
        prev_len = len_lens
        mcodes = canon_codes(main_lens)
        lcodes = canon_codes(len_lens)
        for ms, ls, of in enc:
            w.bits(mcodes[ms], main_lens[ms])
            if ls is not None:
                w.bits(lcodes[ls], len_lens[ls])
            if of is not None:
                s, footer = of
                if s >= 3:
                    nb = slot_bits(s)
                    if btype == "A" and nb >= 3:
                        w.bits(footer >> 3, nb - 3)
                        w.bits(al_codes[footer & 7], al_lens[footer & 7])
                    else:
                        w.bits(footer, nb)
        mf.insert_upto(end)
        pos = end
    return w.finish(), stats

# --------------------------------------------------------------------------
# Test data
# --------------------------------------------------------------------------

WORDS = (b"the of and to in is for on that with as it by from at this be or "
         b"are an file block chunk window offset length match literal tree "
         b"code symbol stream kernel driver volume cluster record").split()




WOF_CHUNK = {0: 4096, 1: 32768, 2: 8192, 3: 16384}


def wof_blob(src, out, fmt):
    data = open(src, "rb").read()
    cs = WOF_CHUNK[fmt]
    rng = random.Random(7)
    chunks = []
    for i in range(0, len(data), cs):
        ch = data[i:i + cs]
        if fmt == 1:
            c = lzx_compress(ch, rng, [("V", len(ch))])[0]
        else:
            c = xpress_compress(ch, rng)
        chunks.append(c if len(c) < len(ch) else ch)
    esz = 8 if len(data) >= 1 << 32 else 4
    table, off = b"", 0
    for c in chunks[:-1]:
        off += len(c)
        table += off.to_bytes(esz, "little")
    open(out, "wb").write(table + b"".join(chunks))


def wof_reparse(path, name, fmt):
    img = Img(path, "r+b")
    fs = Ntfs(img)
    n = fs.find("/" + name)
    if n is None:
        sys.exit("ntfs-tweak: no /%s" % name)
    r = fs.record(n)
    used = struct.unpack_from("<I", r, 0x18)[0]
    val = struct.pack("<IHHIIII", 0x80000017, 16, 0, 1, 2, 1, fmt)
    alen = (0x18 + len(val) + 7) & ~7
    if used + alen > fs.rec:
        sys.exit("ntfs-tweak: no room for the reparse point")
    pos = None
    for a in fs.attrs(r):
        if a["type"] > 0xC0 and pos is None:
            pos = a["off"]
        if a["type"] == 0x10:
            struct.pack_into("<I", r, a["voff"] + 32,
                             struct.unpack_from("<I", r, a["voff"] + 32)[0] | 0x400)
    if pos is None:
        pos = used - 8
    inst = struct.unpack_from("<H", r, 0x28)[0]
    b = bytearray(alen)
    struct.pack_into("<IIBBHHHIHBB", b, 0, 0xC0, alen, 0, 0, 0, 0, inst,
                     len(val), 0x18, 0, 0)
    b[0x18:0x18 + len(val)] = val
    r[pos + alen:used + alen] = r[pos:used]
    r[pos:pos + alen] = b
    struct.pack_into("<I", r, 0x18, used + alen)
    struct.pack_into("<H", r, 0x28, inst + 1)
    fs.write_record(n, r)


# --------------------------------------------------------------- exFAT --

def exfat_regions(img):
    b = img.read(0, 512)
    if b[3:11] != b"EXFAT   ":
        raise ValueError("not exFAT")
    fat_off, fat_len, heap, ncl, root = struct.unpack_from("<IIIII", b, 0x50)
    ss = 1 << b[0x6C]
    cs = ss << b[0x6D]
    out = [(0, 12 * ss, "boot-region"), (12 * ss, 12 * ss, "backup-boot-region"),
           (fat_off * ss, min(fat_len * ss, 65536), "fat")]

    def cl(c):
        return heap * ss + (c - 2) * cs

    def fat(c):
        return struct.unpack("<I", img.read(fat_off * ss + 4 * c, 4) or b"\xff\xff\xff\xff")[0]

    # walk directories (explicit stack), noting entry clusters
    stack = [(root, None, False)]
    seen = set()
    while stack and len(seen) < 4000:
        first, length, nofat = stack.pop()
        c = first
        k = 0
        while 2 <= c < 0xFFFFFFF7 and c not in seen and len(seen) < 4000:
            seen.add(c)
            data = img.read(cl(c), cs)
            out.append((cl(c), cs, "dir"))
            for i in range(0, len(data) - 31, 32):
                t = data[i]
                if t == 0x81:
                    bc, bl = struct.unpack_from("<IQ", data, i + 20)
                    out.append((cl(bc), min(bl, 65536), "bitmap"))
                elif t == 0x82:
                    uc, ul = struct.unpack_from("<IQ", data, i + 20)
                    out.append((cl(uc), min(ul, 8192), "upcase"))
                elif t == 0x85 and i + 64 <= len(data) and data[i + 32] == 0xC0:
                    attr = struct.unpack_from("<H", data, i + 4)[0]
                    fl = data[i + 33]
                    fc, dl = struct.unpack_from("<IQ", data, i + 32 + 20)
                    if attr & 0x10 and fc >= 2:
                        stack.append((fc, dl, bool(fl & 2)))
            k += 1
            if nofat:
                if length is None or k * cs >= length:
                    break
                c += 1
            else:
                c = fat(c)
    return out


def exfat_tweak(path, ops):
    img = Img(path, "r+b")
    b = bytearray(img.read(0, 512))
    ss = 1 << b[0x6C]
    for op in ops:
        if op in ("dirty", "clean"):
            fl = struct.unpack_from("<H", b, 0x6A)[0]
            fl = fl | 2 if op == "dirty" else fl & ~2
            struct.pack_into("<H", b, 0x6A, fl)
            img.write(0, bytes(b))
        elif op == "bad-boot-checksum":
            o = 11 * ss
            c = bytearray(img.read(o, 4))
            c[0] ^= 0xFF
            img.write(o, bytes(c))
        else:
            sys.exit("exfat-tweak: unknown op %s" % op)


# ----------------------------------------------------------------- ISO --

def iso_regions(img):
    out = []
    pvd = None
    for s in range(16, 64):
        d = img.read(s * 2048, 2048)
        if d[1:6] != b"CD001":
            break
        out.append((s * 2048, 2048, "vd-%d" % d[0]))
        if d[0] == 1 and pvd is None:
            pvd = d
        if d[0] == 2 and d[88:91] in (b"%/@", b"%/C", b"%/E"):
            jroot = d[156:190]
            out.append((struct.unpack_from("<I", jroot, 2)[0] * 2048, 2048, "joliet-root"))
        if d[0] == 255:
            break
    if pvd is None:
        raise ValueError("not ISO 9660")
    bs = struct.unpack_from("<H", pvd, 128)[0] or 2048
    ptl = struct.unpack_from("<I", pvd, 132)[0]
    ptlloc = struct.unpack_from("<I", pvd, 140)[0]
    out.append((ptlloc * bs, min(ptl, 8192), "path-table-l"))
    root = pvd[156:190]
    stack = [(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])]
    seen = set()
    while stack and len(seen) < 3000:
        lba, size = stack.pop()
        if lba in seen:
            continue
        seen.add(lba)
        data = img.read(lba * bs, min(size, 1 << 20))
        out.append((lba * bs, min(size, 65536), "dir"))
        i = 0
        while i < len(data):
            l = data[i]
            if l == 0:
                i = (i // bs + 1) * bs
                continue
            if i + 34 > len(data):
                break
            rec = data[i:i + l]
            el = struct.unpack_from("<I", rec, 2)[0]
            sz = struct.unpack_from("<I", rec, 10)[0]
            fl = rec[25]
            nl = rec[32]
            nm = rec[33:33 + nl]
            su = 33 + nl + (1 - nl % 2)
            if su < l and b"CE" in rec[su:]:
                k = rec.find(b"CE", su)
                if k + 28 <= l:
                    ce = struct.unpack_from("<I", rec, k + 4)[0]
                    out.append((ce * bs, 2048, "susp-ce"))
            if fl & 2 and nm not in (b"\x00", b"\x01"):
                stack.append((el, sz))
            elif not fl & 2 and sz >= 16 and len(seen) < 3000:
                h = img.read(el * bs, 16)
                if h[:8] == bytes.fromhex("37E45396C9DBD607"):
                    out.append((el * bs, 64, "zisofs-header"))
            i += l
    return out


# ------------------------------------------------------------ FAT/ext4 --

def fat_regions(img):
    """Boot sector, FSInfo and backup (FAT32), the first 64 KB of the FAT,
    the root directory (the fixed region of FAT12/16, the first cluster of
    a FAT32 one) and the first cluster of up to 64 directories found from
    the root down (breadth first, bounded)."""
    b = img.read(0, 512)
    bps = struct.unpack_from("<H", b, 11)[0] or 512
    spc = b[13] or 1
    res = struct.unpack_from("<H", b, 14)[0]
    nf = b[16]
    rootents = struct.unpack_from("<H", b, 17)[0]
    fsz16 = struct.unpack_from("<H", b, 22)[0]
    fat32 = fsz16 == 0
    fsz = struct.unpack_from("<I", b, 36)[0] if fat32 else fsz16
    root_secs = (rootents * 32 + bps - 1) // bps
    root_start = (res + nf * fsz) * bps
    data = root_start + root_secs * bps
    cs = bps * spc
    out = [(0, 512, "boot"), (res * bps, min(fsz * bps, 65536), "fat")]
    if fat32:
        out += [(512, 512, "fsinfo"), (6 * bps, 512, "backup-boot")]
        root = struct.unpack_from("<I", b, 44)[0]
        first = [(data + (root - 2) * cs, cs, "root")] if root >= 2 else []
    else:
        first = [(root_start, root_secs * bps, "root")] if root_secs else []
    out += first
    # subdirectories: the first cluster of each, found through the entries
    todo = [r[0:2] for r in first]
    seen = 0
    while todo and seen < 64:
        off, ln = todo.pop(0)
        d = img.read(off, min(ln, 1 << 16))
        for i in range(0, len(d) - 31, 32):
            e = d[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x3F == 0x0F or not e[11] & 0x10 or e[0] == 0x2E:
                continue
            cl = struct.unpack_from("<H", e, 26)[0]
            if fat32:
                cl |= struct.unpack_from("<H", e, 20)[0] << 16
            o = data + (cl - 2) * cs
            if cl < 2 or o + cs > img.size:
                continue
            out.append((o, cs, "dir"))
            todo.append((o, cs))
            seen += 1
            if seen >= 64:
                break
    return [r for r in out if r[1] > 0 and r[0] < img.size]


def ext4_regions(img):
    sb = img.read(1024, 1024)
    if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
        raise ValueError("not ext2/3/4")
    bs = 1024 << struct.unpack_from("<I", sb, 24)[0]
    ipg = struct.unpack_from("<I", sb, 40)[0]
    isz = struct.unpack_from("<H", sb, 88)[0] or 128
    gdt = (2 if bs == 1024 else 1) * bs
    out = [(1024, 1024, "superblock"), (gdt, bs, "gdt")]
    g = img.read(gdt, 64)
    it = struct.unpack_from("<I", g, 8)[0]
    out.append((it * bs, min(ipg * isz, 64 * isz), "inode-table"))
    out.append((struct.unpack_from("<I", g, 0)[0] * bs, bs, "block-bitmap"))
    out.append((struct.unpack_from("<I", g, 4)[0] * bs, bs, "inode-bitmap"))
    return out


def regions(fstype, path):
    img = Img(path)
    if fstype == "ntfs":
        return Ntfs(img).regions()
    if fstype == "exfat":
        return exfat_regions(img)
    if fstype in ("iso", "isofs", "iso9660"):
        return iso_regions(img)
    if fstype in ("fat", "fat32", "vfat"):
        return fat_regions(img)
    if fstype in ("ext4", "ext2", "ext3"):
        return ext4_regions(img)
    raise ValueError("unknown fstype %s" % fstype)


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "regions":
        for o, l, lab in regions(a[1], a[2]):
            print("%d %d %s" % (o, l, lab))
    elif len(a) >= 3 and a[0] == "ntfs-tweak":
        ntfs_tweak(a[1], a[2:])
    elif len(a) == 4 and a[0] == "wof-blob":
        wof_blob(a[1], a[2], int(a[3]))
    elif len(a) == 4 and a[0] == "wof-reparse":
        wof_reparse(a[1], a[2], int(a[3]))
    elif len(a) >= 3 and a[0] == "exfat-tweak":
        exfat_tweak(a[1], a[2:])
    else:
        sys.exit("usage: fsparse.py regions FSTYPE IMAGE | ntfs-tweak IMAGE OP... | exfat-tweak IMAGE OP...")


if __name__ == "__main__":
    main()
