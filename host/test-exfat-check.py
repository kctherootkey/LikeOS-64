#!/usr/bin/env python3
# Independent consistency checker for exFAT images (host/test-exfat.sh).
#
# fsck.exfat -n (exfatprogs 1.2.x) does not compare the allocation bitmap
# with what the files use, so it cannot tell a consistent volume from one
# that hands a file's clusters out a second time.  This checker walks the
# whole tree and verifies, from the on-disk structures alone:
#
#   - the main boot region (sanity, checksum sector); the backup region's
#     checksum
#   - every entry set: secondary count, stream / name entry layout, the
#     SetChecksum, the NameHash (through the volume's upcase table), the
#     name characters, ValidDataLength <= DataLength, attributes
#   - every stream's clusters: in the heap, chain long enough for its
#     DataLength (FAT chains end with the end mark there; a longer chain
#     is reported as a leak, not an error), NoFatChain runs inside the heap
#   - no cluster owned twice (cross-links), every owned cluster marked in
#     the bitmap ("used but free" is an error)
#   - directories: DataLength a multiple of the cluster size, VDL = size,
#     no two names equal under the upcase table
#   - leaks: clusters marked in the bitmap that nothing owns
#
#   test-exfat-check.py IMAGE [--allow-leaks] [--allow-dirty] [-v]
#
# Exit 0: consistent (leaks / dirty allowed when asked); 1: errors; 2: only
# leaks or the dirty flag, not allowed.
#
# Copyright (C) 2026 The LikeOS Project

import struct
import sys


class Checker:
    def __init__(self, path, verbose=False):
        self.f = open(path, "rb")
        self.verbose = verbose
        self.errors = []
        self.leaks = 0
        self.tail_leaks = 0
        self.owner = {}
        self.files = 0
        self.dirs = 0

    def err(self, msg):
        if len(self.errors) < 200:
            self.errors.append(msg)

    def read(self, off, n):
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) != n:
            raise IOError("short read at %d" % off)
        return d

    # ---- boot region -----------------------------------------------------
    def boot(self):
        b = self.read(0, 512)
        if b[3:11] != b"EXFAT   ":
            raise SystemExit("not exFAT")
        self.bps_shift = b[108]
        self.spc_shift = b[109]
        self.bps = 1 << self.bps_shift
        self.spc = 1 << self.spc_shift
        self.cs = self.bps * self.spc
        (self.fat_off, self.fat_len, self.heap_off, self.cc,
         self.root) = struct.unpack_from("<IIIII", b, 80)
        self.flags = struct.unpack_from("<H", b, 106)[0]
        self.nfats = b[110]
        region = self.read(0, 12 * self.bps)
        if self.checksum(region) != struct.unpack_from("<I", region, 11 * self.bps)[0]:
            self.err("main boot region checksum mismatch")
        for i in range(0, self.bps, 4):
            if region[11 * self.bps + i:11 * self.bps + i + 4] != region[11 * self.bps:11 * self.bps + 4]:
                self.err("main boot checksum sector not uniform")
                break
        backup = self.read(12 * self.bps, 12 * self.bps)
        if self.checksum(backup) != struct.unpack_from("<I", backup, 11 * self.bps)[0]:
            self.err("backup boot region checksum mismatch")
        active = 1 if (self.nfats == 2 and (self.flags & 1)) else 0
        fat = self.read((self.fat_off + active * self.fat_len) * self.bps,
                        (self.cc + 2) * 4)
        self.fat = struct.unpack("<%dI" % (self.cc + 2), fat)

    def checksum(self, region):
        s = 0
        for i, x in enumerate(region[:11 * self.bps]):
            if i in (106, 107, 112):
                continue
            s = (((s >> 1) | ((s & 1) << 31)) + x) & 0xFFFFFFFF
        return s

    def cl_off(self, c):
        return (self.heap_off + (c - 2) * self.spc) * self.bps

    def ok_cluster(self, c):
        return 2 <= c <= self.cc + 1

    # ---- chains ----------------------------------------------------------------
    def chain(self, first, nofat, want, what, until_eoc=False):
        """Clusters of a stream; `want' clusters (or to the end mark)."""
        out = []
        if want == 0 and not until_eoc:
            return out
        if not self.ok_cluster(first):
            self.err("%s: first cluster %d out of range" % (what, first))
            return out
        if nofat:
            if first + want - 1 > self.cc + 1:
                self.err("%s: contiguous run leaves the heap" % what)
                return out
            return list(range(first, first + want))
        c = first
        seen = set()
        limit = self.cc if until_eoc else want
        while True:
            if c in seen:
                self.err("%s: FAT chain loops at %d" % (what, c))
                return out
            seen.add(c)
            out.append(c)
            if not until_eoc and len(out) == want:
                nxt = self.fat[c]
                if nxt != 0xFFFFFFFF:
                    # A chain longer than the data: clusters nobody can
                    # reach through the file (a leak, like a lost one).
                    extra = 0
                    while self.ok_cluster(nxt) and nxt not in seen and extra < self.cc:
                        seen.add(nxt)
                        self.claim(nxt, what + " (chain tail)")
                        self.tail_leaks += 1
                        extra += 1
                        nxt = self.fat[nxt]
                    if nxt != 0xFFFFFFFF and extra == 0:
                        self.err("%s: chain does not end at its length (FAT %#x)" % (what, nxt))
                return out
            nxt = self.fat[c]
            if nxt == 0xFFFFFFFF:
                if not until_eoc:
                    self.err("%s: chain ends after %d of %d clusters" % (what, len(out), want))
                return out
            if not self.ok_cluster(nxt):
                self.err("%s: FAT value %#x at cluster %d" % (what, nxt, c))
                return out
            if len(out) >= limit:
                self.err("%s: chain too long" % what)
                return out
            c = nxt

    def claim(self, c, what):
        if c in self.owner:
            self.err("cluster %d owned by %s and %s (cross-link)" % (c, self.owner[c], what))
        else:
            self.owner[c] = what

    def data(self, clusters, length):
        out = bytearray()
        for c in clusters:
            out += self.read(self.cl_off(c), self.cs)
            if len(out) >= length:
                break
        return bytes(out[:length])

    # ---- the tree ---------------------------------------------------------------
    def run(self):
        self.boot()
        root_cl = self.chain(self.root, False, 0, "root", until_eoc=True)
        for c in root_cl:
            self.claim(c, "root")
        rootdata = self.data(root_cl, len(root_cl) * self.cs)
        self.upcase = list(range(65536))
        self.bitmap = None
        for i in range(0, len(rootdata), 32):
            t = rootdata[i]
            if t == 0:
                break
            if t == 0x81:
                first, length = struct.unpack_from("<IQ", rootdata, i + 20)
                cl = self.chain(first, False, (length + self.cs - 1) // self.cs, "bitmap")
                for c in cl:
                    self.claim(c, "bitmap")
                self.bitmap = self.data(cl, length)
            elif t == 0x82:
                csum, = struct.unpack_from("<I", rootdata, i + 4)
                first, length = struct.unpack_from("<IQ", rootdata, i + 20)
                cl = self.chain(first, False, (length + self.cs - 1) // self.cs, "upcase")
                for c in cl:
                    self.claim(c, "upcase")
                raw = self.data(cl, length)
                s = 0
                for x in raw:
                    s = (((s >> 1) | ((s & 1) << 31)) + x) & 0xFFFFFFFF
                if s != csum:
                    self.err("upcase table checksum mismatch")
                idx = 0
                k = 0
                while k + 1 < len(raw) and idx < 65536:
                    v = struct.unpack_from("<H", raw, k)[0]
                    if v == 0xFFFF and k + 3 < len(raw):
                        idx += struct.unpack_from("<H", raw, k + 2)[0]
                        k += 4
                        continue
                    self.upcase[idx] = v
                    idx += 1
                    k += 2
        if self.bitmap is None:
            self.err("no allocation bitmap")
            return
        stack = [("/", rootdata, True)]
        while stack:
            path, d, is_root = stack.pop()
            self.walk_dir(path, d, is_root, stack)
        for c in range(2, self.cc + 2):
            bit = (self.bitmap[(c - 2) >> 3] >> ((c - 2) & 7)) & 1
            if c in self.owner and not bit:
                self.err("cluster %d (%s) used but free in the bitmap" % (c, self.owner[c]))
            elif bit and c not in self.owner:
                self.leaks += 1

    def walk_dir(self, path, d, is_root, stack):
        names = {}
        n = len(d) // 32
        i = 0
        while i < n:
            t = d[i * 32]
            if t == 0:
                break
            if not (t & 0x80):
                i += 1
                continue
            if t != 0x85:
                if t & 0x40:
                    if t != 0xC0 and t != 0xC1:
                        pass
                    self.err("%s: stray secondary %#x at slot %d" % (path, t, i))
                    i += 1
                    continue
                if t in (0x81, 0x82, 0x83):
                    if not is_root:
                        self.err("%s: volume entry %#x outside the root" % (path, t))
                    i += 1
                    continue
                i += 1 + d[i * 32 + 1]
                continue
            sc = d[i * 32 + 1]
            if sc < 2 or sc > 18 or i + sc >= n:
                self.err("%s: bad secondary count %d at slot %d" % (path, sc, i))
                i += 1
                continue
            s = d[i * 32:(i + sc + 1) * 32]
            self.check_set(path, s, sc, i, names, stack)
            i += 1 + sc

    def check_set(self, path, s, sc, slot, names, stack):
        what = "%s[slot %d]" % (path, slot)
        sm = 0
        for j, x in enumerate(s):
            if j in (2, 3):
                continue
            sm = (((sm >> 1) | ((sm & 1) << 15)) + x) & 0xFFFF
        if sm != struct.unpack_from("<H", s, 2)[0]:
            self.err("%s: set checksum mismatch" % what)
            return
        attr = struct.unpack_from("<H", s, 4)[0]
        se = s[32:64]
        if se[0] != 0xC0:
            self.err("%s: no stream extension" % what)
            return
        flags, nlen = se[1], se[3]
        nhash, = struct.unpack_from("<H", se, 4)
        vdl, = struct.unpack_from("<Q", se, 8)
        first, size = struct.unpack_from("<IQ", se, 20)
        nent = (nlen + 14) // 15
        if nlen == 0 or 1 + nent > sc:
            self.err("%s: name length %d with %d secondaries" % (what, nlen, sc))
            return
        units = []
        for k in range(nent):
            e = s[64 + 32 * k:96 + 32 * k]
            if e[0] != 0xC1:
                self.err("%s: name entry %d has type %#x" % (what, k, e[0]))
                return
            units += struct.unpack_from("<15H", e, 2)
        units = units[:nlen]
        up = [self.upcase[u] for u in units]
        h = 0
        for u in up:
            h = (((h >> 1) | ((h & 1) << 15)) + (u & 0xFF)) & 0xFFFF
            h = (((h >> 1) | ((h & 1) << 15)) + (u >> 8)) & 0xFFFF
        if h != nhash:
            self.err("%s: NameHash mismatch" % what)
        for u in units:
            if u < 0x20 or u in (0x22, 0x2A, 0x2F, 0x3A, 0x3C, 0x3E, 0x3F, 0x5C, 0x7C):
                self.err("%s: forbidden character %#x in name" % (what, u))
                break
        key = tuple(up)
        name = bytes(struct.pack("<%dH" % len(units), *units)).decode("utf-16-le", "replace")
        full = path + name
        if key in names:
            self.err("%s: name %r twice in the directory" % (what, name))
        names[key] = 1
        for k in range(2 + nent, sc + 1):
            x = s[32 * k:32 * k + 32]
            if (x[0] & 0xC0) != 0xC0:
                self.err("%s: entry %d of the set not a secondary in use" % (what, k))
            if x[0] in (0xC0, 0xC1):
                self.err("%s: misplaced stream/name entry" % what)
            if x[1] & 1:
                f2, l2 = struct.unpack_from("<IQ", x, 20)
                for c in self.chain(f2, x[1] & 2, (l2 + self.cs - 1) // self.cs, full + " (vendor)"):
                    self.claim(c, full + " (vendor)")
        if vdl > size:
            self.err("%s: ValidDataLength %d > DataLength %d" % (full, vdl, size))
        if not (flags & 1) and (first or size):
            self.err("%s: allocation without AllocationPossible" % full)
        want = (size + self.cs - 1) // self.cs
        if size and not first:
            self.err("%s: %d bytes without clusters" % (full, size))
            return
        cl = self.chain(first, flags & 2, want, full) if want else []
        for c in cl:
            self.claim(c, full)
        if attr & 0x10:
            self.dirs += 1
            if size % self.cs or size == 0:
                self.err("%s: directory size %d" % (full, size))
            if vdl != size:
                self.err("%s: directory VDL %d != size %d" % (full, vdl, size))
            if len(cl) == want and want:
                stack.append((full + "/", self.data(cl, size), False))
        else:
            self.files += 1
        if self.verbose:
            print("%s %s size=%d vdl=%d first=%d flags=%#x" %
                  ("d" if attr & 0x10 else "f", full, size, vdl, first, flags))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) != 1:
        sys.exit("usage: test-exfat-check.py IMAGE [--allow-leaks] [--allow-dirty] [-v]")
    c = Checker(args[0], "-v" in sys.argv)
    try:
        c.run()
    except IOError as e:
        c.err(str(e))
    dirty = bool(c.flags & 2)
    print("exfat-check: %s: %d dirs, %d files, %d errors, %d lost clusters, %d chain-tail clusters%s" %
          (args[0], c.dirs, c.files, len(c.errors), c.leaks, c.tail_leaks,
           ", VolumeDirty set" if dirty else ""))
    for e in c.errors:
        print("  ERROR " + e)
    if c.errors:
        sys.exit(1)
    if (c.leaks or c.tail_leaks) and "--allow-leaks" not in sys.argv:
        sys.exit(2)
    if dirty and "--allow-dirty" not in sys.argv:
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
