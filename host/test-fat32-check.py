#!/usr/bin/env python3
# Independent consistency checker of FAT12 / FAT16 / FAT32 images, for
# host/test-fat32.sh.  fsck.fat -n is the first oracle; this one checks what
# it reports only in passing or not at all, and classifies what a power cut
# may leave behind:
#
#   - every FAT copy equal (the active one only when mirroring is off);
#   - FAT[0] carries the media byte; the FAT16/FAT32 "clean" bit of FAT[1]
#     and the dirty bit of the boot sector's state byte (clean unless
#     --allow-dirty; --expect-dirty demands them dirty);
#   - every cluster chain: valid cluster numbers, no loop, ends in an
#     end-of-chain mark, no cluster owned twice (cross-link), no free or bad
#     cluster inside a chain; a file's chain exactly covers its size (a
#     longer one is a "chain tail", allowed with --allow-chain-tail or
#     --allow-leaks), a directory has at least one cluster;
#   - allocated clusters nobody owns are "lost clusters" (allowed with
#     --allow-leaks);
#   - directories: "." and ".." of every subdirectory name the right
#     clusters, long-name entry sets well formed (sequence, checksum, the
#     last flag, no orphans), no two entries with the same short name, no
#     two names equal when compared case-insensitively, short names made of
#     valid characters only, no file whose size exceeds the volume;
#   - FAT32: the FSInfo free count equals the real number of free clusters
#     (unless it is 0xFFFFFFFF "unknown", or with --allow-leaks /
#     --allow-dirty) and its next-free hint is a valid cluster or unknown.
#
#   test-fat32-check.py IMAGE [--allow-leaks] [--allow-dirty]
#                             [--allow-chain-tail] [--expect-dirty]
#                             [--allow-fat-mismatch]  (FAT copies may
#                             differ -- a stop between their writes --
#                             but each one must be consistent)
#   test-fat32-check.py IMAGE --list DIR     the entries of DIR, one per line:
#                             SHORT<TAB>long name<TAB>attr<TAB>case bits
#                             <TAB>size<TAB>first cluster<TAB>mdate mtime
#   test-fat32-check.py IMAGE --info         type, geometry, flags, FSInfo
#
# The image is read through seek/read (the FAT copy and one cluster at a
# time); nothing proportional to the image size is held besides one owner
# number per cluster.  Exit status 0 consistent, 1 findings (printed), 2
# usage or an image that is no FAT volume.
#
# Copyright (C) 2026 The LikeOS Project
import array
import struct
import sys

SHORT_OK = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !#$%&'()-@^_`{}~") | set(range(0x80, 0x100))


class Vol:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        b = self.rd(0, 512)
        if len(b) < 512 or b[510:512] != b"\x55\xaa":
            raise ValueError("no boot sector signature")
        self.bps = struct.unpack_from("<H", b, 11)[0]
        self.spc = b[13]
        self.res = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.rootents = struct.unpack_from("<H", b, 17)[0]
        tot16 = struct.unpack_from("<H", b, 19)[0]
        self.media = b[21]
        fsz16 = struct.unpack_from("<H", b, 22)[0]
        tot32 = struct.unpack_from("<I", b, 32)[0]
        if self.bps not in (512, 1024, 2048, 4096) or self.spc == 0 or self.spc & (self.spc - 1):
            raise ValueError("bad geometry")
        self.total = tot16 or tot32
        if fsz16:
            self.fatsz = fsz16
            self.extflags = 0
            self.root_clus = 0
            self.fsinfo = 0
            self.state_off = 37 if b[38] in (0x28, 0x29) else 0
        else:
            self.fatsz = struct.unpack_from("<I", b, 36)[0]
            self.extflags = struct.unpack_from("<H", b, 40)[0]
            self.root_clus = struct.unpack_from("<I", b, 44)[0]
            self.fsinfo = struct.unpack_from("<H", b, 48)[0]
            self.state_off = 65 if b[66] in (0x28, 0x29) else 0
        self.boot = b
        self.cs = self.bps * self.spc
        self.root_secs = (self.rootents * 32 + self.bps - 1) // self.bps
        self.fat_start = self.res * self.bps
        self.root_start = (self.res + self.nfats * self.fatsz) * self.bps
        self.data_start = self.root_start + self.root_secs * self.bps
        data_secs = self.total - (self.res + self.nfats * self.fatsz + self.root_secs)
        self.nclusters = data_secs // self.spc
        if fsz16 == 0:
            self.bits = 32
        elif self.nclusters < 4085:
            self.bits = 12
        else:
            self.bits = 16
        self.end = self.nclusters + 2
        if self.bits == 32:
            self.eoc_min, self.bad, self.mask = 0x0FFFFFF8, 0x0FFFFFF7, 0x0FFFFFFF
        elif self.bits == 16:
            self.eoc_min, self.bad, self.mask = 0xFFF8, 0xFFF7, 0xFFFF
        else:
            self.eoc_min, self.bad, self.mask = 0xFF8, 0xFF7, 0xFFF
        active = 0
        if self.bits == 32 and (self.extflags & 0x80):
            active = self.extflags & 0x0F
        self.active = active
        self.fat_bytes = self.fatsz * self.bps
        self.fat = self.rd(self.fat_start + active * self.fat_bytes, self.fat_bytes)

    def rd(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def ent(self, c):
        if self.bits == 32:
            return struct.unpack_from("<I", self.fat, c * 4)[0] & 0x0FFFFFFF
        if self.bits == 16:
            return struct.unpack_from("<H", self.fat, c * 2)[0]
        o = c + c // 2
        v = self.fat[o] | (self.fat[o + 1] << 8)
        return (v >> 4) if c & 1 else (v & 0xFFF)

    def cluster(self, c):
        return self.rd(self.data_start + (c - 2) * self.cs, self.cs)


def lfn_checksum(name11):
    s = 0
    for ch in name11:
        s = (((s & 1) << 7) | (s >> 1)) + ch
        s &= 0xFF
    return s


def fold(name):
    return name.upper()


class Checker:
    def __init__(self, v, opts):
        self.v = v
        self.o = opts
        self.errs = []
        self.lost = 0
        self.tails = 0
        self.owner = array.array("i", [0]) * v.end   # 0 free/unowned, else object id
        self.nobj = 0
        self.files = 0
        self.dirs = 0

    def err(self, msg):
        if len(self.errs) < 200:
            self.errs.append(msg)

    def chain(self, first, what):
        """The clusters of a chain (owned by a new object id), or None."""
        v = self.v
        self.nobj += 1
        oid = self.nobj
        out = []
        c = first
        while True:
            if c < 2 or c >= v.end:
                self.err("%s: chain reaches invalid cluster %d" % (what, c))
                return out
            if self.owner[c]:
                if self.owner[c] == oid:
                    self.err("%s: chain loops at cluster %d" % (what, c))
                else:
                    self.err("%s: cluster %d cross-linked with another object" % (what, c))
                return out
            self.owner[c] = oid
            out.append(c)
            n = v.ent(c)
            if n == 0:
                self.err("%s: chain runs into free cluster (entry of %d is 0)" % (what, c))
                return out
            if n == v.bad:
                self.err("%s: chain contains bad cluster mark at %d" % (what, c))
                return out
            if n >= v.eoc_min:
                return out
            if len(out) > v.nclusters:
                self.err("%s: chain longer than the volume" % what)
                return out
            c = n

    def entries(self, clusters, fixed_root):
        v = self.v
        if fixed_root:
            data = v.rd(v.root_start, v.rootents * 32)
            for i in range(0, len(data), 32):
                yield data[i:i + 32]
            return
        for c in clusters:
            d = v.cluster(c)
            for i in range(0, len(d), 32):
                yield d[i:i + 32]

    def walk(self):
        v = self.v
        # stack of (path, clusters or None for the fixed root, own cluster, parent cluster)
        if v.bits == 32:
            if v.root_clus < 2 or v.root_clus >= v.end:
                self.err("root cluster %d invalid" % v.root_clus)
                return
            rc = self.chain(v.root_clus, "/")
            stack = [("/", rc, v.root_clus, 0, False)]
        else:
            stack = [("/", None, 0, 0, True)]
        self.dirs = 1
        depth_guard = 0
        while stack:
            depth_guard += 1
            if depth_guard > 1000000:
                self.err("directory walk does not end")
                return
            path, clus, own, parent, fixed = stack.pop()
            self.dir(path, clus, own, parent, fixed, stack)

    def dir(self, path, clus, own, parent, fixed, stack):
        v = self.v
        lfn = []          # pending long-name entries (as read, in order)
        names = {}
        shorts = {}
        slot = 0
        ended = False
        for e in self.entries(clus, fixed):
            idx = slot
            slot += 1
            if ended:
                continue
            if e[0] == 0:
                if lfn:
                    self.err("%s: orphan long-name entries before the end marker" % path)
                ended = True
                lfn = []
                continue
            if e[0] == 0xE5:
                if lfn:
                    self.err("%s: orphan long-name entries before free slot %d" % (path, idx))
                lfn = []
                continue
            attr = e[11]
            if attr & 0x3F == 0x0F:
                lfn.append(e)
                continue
            name11 = bytes(e[0:11])
            if name11[0] == 0x05:
                name11 = b"\xe5" + name11[1:]
            if path != "/" and idx < 2:
                # "." and ".."
                want = b".          " if idx == 0 else b"..         "
                if bytes(e[0:11]) != want:
                    self.err("%s: slot %d is not the %s entry" % (path, idx, "dot" if idx == 0 else "dotdot"))
                else:
                    cl = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16 if v.bits == 32 else 0)
                    exp = own if idx == 0 else parent
                    if v.bits == 32 and idx == 1 and exp == v.root_clus:
                        exp = 0
                    if cl != exp:
                        self.err("%s: %s names cluster %d, expected %d" % (path, "." if idx == 0 else "..", cl, exp))
                if lfn:
                    self.err("%s: long-name entries in front of a dot entry" % path)
                lfn = []
                continue
            if attr & 0x08:
                if lfn:
                    self.err("%s: long-name entries in front of the volume label" % path)
                lfn = []
                continue
            # a short entry of a file / directory
            long_name = None
            if lfn:
                long_name = self.lfn_name(path, lfn, name11)
            lfn = []
            for ch in name11:
                if ch not in SHORT_OK:
                    self.err("%s: short name %r holds an invalid character" % (path, name11))
                    break
            if name11[0] == 0x20:
                self.err("%s: short name %r starts with a space" % (path, name11))
            if name11 in shorts:
                self.err("%s: short name %r used twice" % (path, name11))
            shorts[name11] = 1
            base = name11[0:8].rstrip(b" ")
            ext = name11[8:11].rstrip(b" ")
            shown = base.decode("latin-1") + ("." + ext.decode("latin-1") if ext else "")
            for nm in set([fold(shown)] + ([fold(long_name)] if long_name else [])):
                if nm in names:
                    self.err("%s: name %r answers for two entries" % (path, nm))
                names[nm] = 1
            label = path.rstrip("/") + "/" + (long_name if long_name is not None else shown)
            cl = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16 if v.bits == 32 else 0)
            size = struct.unpack_from("<I", e, 28)[0]
            if attr & 0x10:
                self.dirs += 1
                if cl == 0:
                    self.err("%s: directory without a cluster" % label)
                    continue
                ch = self.chain(cl, label)
                if not ch:
                    continue
                stack.append((label, ch, cl, own if not fixed else 0, False))
            else:
                self.files += 1
                if size and cl == 0:
                    self.err("%s: size %d but no cluster" % (label, size))
                    continue
                if cl == 0:
                    continue
                ch = self.chain(cl, label)
                need = (size + v.cs - 1) // v.cs
                if len(ch) < need:
                    self.err("%s: size %d needs %d clusters, chain has %d" % (label, size, need, len(ch)))
                elif len(ch) > max(need, 1):
                    self.tails += len(ch) - max(need, 1)
                    if not (self.o.get("allow_chain_tail") or self.o.get("allow_leaks")):
                        self.err("%s: size %d needs %d clusters, chain has %d (chain tail)" % (label, size, need, len(ch)))
        if lfn:
            self.err("%s: orphan long-name entries at the end" % path)

    def lfn_name(self, path, ents, name11):
        # ents in disk order: the last part (flag 0x40) first
        n = len(ents)
        first = ents[0]
        if not first[0] & 0x40 or (first[0] & 0x1F) != n or n > 20:
            self.err("%s: long-name entry sequence broken in front of %r" % (path, name11))
            return None
        ck = lfn_checksum(name11)
        units = []
        for k, e in enumerate(ents):
            if (e[0] & 0x1F) != n - k:
                self.err("%s: long-name entry %d out of sequence in front of %r" % (path, k, name11))
                return None
            if e[13] != ck:
                self.err("%s: long-name checksum mismatch in front of %r" % (path, name11))
                return None
            if e[12] != 0 or struct.unpack_from("<H", e, 26)[0] != 0:
                self.err("%s: long-name entry with type/cluster set in front of %r" % (path, name11))
        for e in reversed(ents):
            raw = e[1:11] + e[14:26] + e[28:32]
            units += list(struct.unpack("<13H", raw))
        out = []
        for u in units:
            if u == 0:
                break
            out.append(u)
        if not out or len(out) > 255:
            self.err("%s: long name empty or longer than 255 units (%r)" % (path, name11))
            return None
        try:
            return struct.pack("<%dH" % len(out), *out).decode("utf-16-le")
        except UnicodeDecodeError:
            return struct.pack("<%dH" % len(out), *out).decode("utf-16-le", "replace")

    def finish(self):
        v = self.v
        free = 0
        for c in range(2, v.end):
            e = v.ent(c)
            if e == 0:
                free += 1
                continue
            if e == v.bad:
                continue
            if not self.owner[c]:
                self.lost += 1
        if self.lost and not self.o.get("allow_leaks"):
            self.err("%d lost clusters (allocated, owned by nothing)" % self.lost)
        self.free = free
        # FAT copies
        if not (v.bits == 32 and (v.extflags & 0x80)) and not self.o.get("fat_mismatch"):
            for k in range(v.nfats):
                if k == v.active:
                    continue
                other = v.rd(v.fat_start + k * v.fat_bytes, v.fat_bytes)
                if other != v.fat:
                    self.err("FAT copy %d differs from FAT copy %d" % (k, v.active))
        # media byte, flags
        if v.ent(0) & 0xFF != v.media:
            self.err("FAT[0] does not carry the media byte 0x%02x" % v.media)
        dirty = False
        if v.bits == 16:
            dirty = not (v.ent(1) & 0x8000)
        elif v.bits == 32:
            dirty = not (v.ent(1) & 0x08000000)
        bdirty = bool(v.state_off and v.boot[v.state_off] & 1)
        if self.o.get("expect_dirty"):
            if not (dirty or bdirty):
                self.err("volume not marked dirty (expected dirty)")
        elif (dirty or bdirty) and not self.o.get("allow_dirty"):
            self.err("volume marked dirty (FAT[1] clean bit %s, boot state %s)" %
                     ("clear" if dirty else "set", "dirty" if bdirty else "clean"))
        if v.bits == 32 and v.fsinfo:
            fi = v.rd(v.fsinfo * v.bps, 512)
            if struct.unpack_from("<I", fi, 0)[0] != 0x41615252 or struct.unpack_from("<I", fi, 484)[0] != 0x61417272:
                self.err("FSInfo signatures broken")
            else:
                fc = struct.unpack_from("<I", fi, 488)[0]
                nx = struct.unpack_from("<I", fi, 492)[0]
                if fc != 0xFFFFFFFF and fc != free and not (self.o.get("allow_leaks") or self.o.get("allow_dirty")):
                    self.err("FSInfo free count %d, real %d" % (fc, free))
                if nx != 0xFFFFFFFF and not (2 <= nx < v.end):
                    self.err("FSInfo next-free hint %d out of range" % nx)


def list_dir(v, path):
    c = Checker(v, {"allow_leaks": 1, "allow_chain_tail": 1, "allow_dirty": 1})
    # find the directory
    comps = [p for p in path.split("/") if p]
    if v.bits == 32:
        clus, fixed = c.chain(v.root_clus, "/"), False
    else:
        clus, fixed = None, True
    rows = []

    def scan(clus, fixed):
        out = []
        lfn = []
        for e in c.entries(clus, fixed):
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] & 0x3F == 0x0F:
                lfn.append(e)
                continue
            if e[11] & 0x08 or e[0:2] in (b". ", b".."):
                lfn = []
                continue
            n11 = bytes(e[0:11])
            ln = c.lfn_name("", lfn, n11) if lfn else None
            lfn = []
            base = n11[0:8].rstrip(b" ").decode("latin-1")
            ext = n11[8:11].rstrip(b" ").decode("latin-1")
            cl = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16 if v.bits == 32 else 0)
            out.append((base + ("." + ext if ext else ""), ln, e[11], e[12],
                        struct.unpack_from("<I", e, 28)[0], cl,
                        struct.unpack_from("<H", e, 24)[0], struct.unpack_from("<H", e, 22)[0],
                        e[13], struct.unpack_from("<H", e, 16)[0], struct.unpack_from("<H", e, 14)[0],
                        struct.unpack_from("<H", e, 18)[0]))
        return out

    for comp in comps:
        found = None
        for r in scan(clus, fixed):
            if fold(comp) in (fold(r[0]), fold(r[1] or "")) and r[2] & 0x10:
                found = r
                break
        if not found:
            print("list: %s not found" % comp, file=sys.stderr)
            return 1
        c.owner = array.array("i", [0]) * v.end
        clus, fixed = c.chain(found[5], comp), False
    for r in scan(clus, fixed):
        print("%s\t%s\t0x%02x\t0x%02x\t%d\t%d\t%04x %04x\tc=%d %04x %04x a=%04x" %
              (r[0], r[1] if r[1] is not None else "", r[2], r[3], r[4], r[5], r[6], r[7],
               r[8], r[9], r[10], r[11]))
    return 0


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit("usage: test-fat32-check.py IMAGE [options] | --list DIR | --info")
    try:
        v = Vol(a[0])
    except (ValueError, struct.error) as e:
        print("check: not a FAT volume: %s" % e)
        sys.exit(2)
    if "--list" in a:
        sys.exit(list_dir(v, a[a.index("--list") + 1]))
    if "--info" in a:
        dirty = None
        if v.bits == 16:
            dirty = not (v.ent(1) & 0x8000)
        elif v.bits == 32:
            dirty = not (v.ent(1) & 0x08000000)
        fc = nx = None
        if v.bits == 32 and v.fsinfo:
            fi = v.rd(v.fsinfo * v.bps, 512)
            fc, nx = struct.unpack_from("<I", fi, 488)[0], struct.unpack_from("<I", fi, 492)[0]
        print("fat%d bps=%d spc=%d clusters=%d fat1_dirty=%s boot_dirty=%s fsinfo_free=%s fsinfo_next=%s" %
              (v.bits, v.bps, v.spc, v.nclusters, dirty,
               bool(v.state_off and v.boot[v.state_off] & 1), fc, nx))
        sys.exit(0)
    opts = {
        "allow_leaks": "--allow-leaks" in a,
        "allow_dirty": "--allow-dirty" in a,
        "allow_chain_tail": "--allow-chain-tail" in a,
        "expect_dirty": "--expect-dirty" in a,
    }
    copies = [v.active]
    if "--allow-fat-mismatch" in a and not (v.bits == 32 and (v.extflags & 0x80)):
        # A stop while the FAT copies are written one after the other
        # leaves them different: then EACH copy must describe a consistent
        # volume (whichever one a checker or another system picks).
        opts["fat_mismatch"] = True
        base = v.fat
        copies = [k for k in range(v.nfats)
                  if k == v.active or v.rd(v.fat_start + k * v.fat_bytes, v.fat_bytes) != base]
    bad = 0
    for k in copies:
        v.fat = v.rd(v.fat_start + k * v.fat_bytes, v.fat_bytes)
        c = Checker(v, opts)
        c.walk()
        c.finish()
        tag = "" if len(copies) == 1 else " (FAT copy %d)" % k
        print("check: fat%d %d files %d dirs, %d free, %d lost clusters, %d chain-tail clusters%s" %
              (v.bits, c.files, c.dirs, c.free, c.lost, c.tails, tag))
        for e in c.errs:
            print("check: ERROR " + e + tag)
        bad |= bool(c.errs)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
