#!/usr/bin/env python3
# Populate a freshly formatted exFAT image (mkfs.exfat) from a host
# directory, without mounting it: there is no unprivileged way to mount
# exFAT and exfatprogs has no copy-in tool, so this writes the on-disk
# structures itself -- allocation bitmap, FAT chains, directory entry sets
# with their SetChecksum and NameHash (names up-cased through the volume's
# own up-case table) -- and fsck.exfat -n checks the result.
#
#   exfat-populate.py IMAGE HOSTDIR [--fragment] [--dirty] [--seed N]
#
#   --fragment  every file and directory gets a FAT chain over scattered
#               clusters instead of a contiguous NoFatChain run
#   --dirty     set VolumeDirty in the main boot sector
#
# Symbolic links and other non-regular files are skipped (exFAT has none).
#
# Copyright (C) 2026 The LikeOS Project
import os
import struct
import sys

TS = (2026 - 1980) << 25 | 1 << 21 | 1 << 16 | 12 << 11 | 0 << 5 | 0  # 2026-01-01 12:00:00


class ExFat:
    def __init__(self, path):
        self.f = open(path, "r+b")
        b = self.read(0, 512)
        if b[3:11] != b"EXFAT   ":
            sys.exit("exfat-populate: not an exFAT image")
        (self.fat_off, self.fat_len, self.heap_off, self.nclusters,
         self.root) = struct.unpack_from("<IIIII", b, 0x50)
        self.sshift = b[0x6C]
        self.cshift = b[0x6D]
        self.bps = 1 << self.sshift
        self.cs = self.bps << self.cshift
        self.boot = bytearray(b)
        # root directory: bitmap and up-case entries
        self.bitmap_cl = self.bitmap_len = self.up_cl = self.up_len = None
        for e in self.dir_entries_chain(self.root):
            t = e[0]
            if t == 0x81:
                self.bitmap_cl, self.bitmap_len = struct.unpack_from("<IQ", e, 20)
            elif t == 0x82:
                self.up_cl, self.up_len = struct.unpack_from("<IQ", e, 20)
        if self.bitmap_cl is None or self.up_cl is None:
            sys.exit("exfat-populate: no bitmap or up-case entry")
        self.bitmap = bytearray(self.read(self.cl_off(self.bitmap_cl), self.bitmap_len))
        self.upcase = self.load_upcase()
        self.next_free = 2

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)

    def cl_off(self, c):
        return (self.heap_off << self.sshift) + (c - 2) * self.cs

    def fat_get(self, c):
        return struct.unpack("<I", self.read((self.fat_off << self.sshift) + 4 * c, 4))[0]

    def fat_set(self, c, v):
        self.write((self.fat_off << self.sshift) + 4 * c, struct.pack("<I", v))

    def chain(self, c):
        seen = set()
        while 2 <= c < 0xFFFFFFF7 and c not in seen:
            seen.add(c)
            yield c
            c = self.fat_get(c)

    def dir_entries_chain(self, first):
        for c in self.chain(first):
            data = self.read(self.cl_off(c), self.cs)
            for i in range(0, self.cs, 32):
                yield data[i:i + 32]

    def load_upcase(self):
        raw = self.read(self.cl_off(self.up_cl), self.up_len)
        table = {}
        i = 0
        ch = 0
        while i + 1 < len(raw):
            v = struct.unpack_from("<H", raw, i)[0]
            i += 2
            if v == 0xFFFF and i + 1 < len(raw):
                skip = struct.unpack_from("<H", raw, i)[0]
                i += 2
                ch += skip
                continue
            table[ch] = v
            ch += 1
        return table

    def up(self, u):
        return self.upcase.get(u, u)

    def bit_used(self, c):
        i = c - 2
        return self.bitmap[i >> 3] >> (i & 7) & 1

    def bit_set(self, c):
        i = c - 2
        self.bitmap[i >> 3] |= 1 << (i & 7)

    def alloc(self, n, fragment):
        """n clusters: contiguous (returns list) or scattered with gaps."""
        out = []
        c = self.next_free
        if not fragment:
            while True:
                if c + n - 2 > self.nclusters:
                    sys.exit("exfat-populate: volume full")
                if all(not self.bit_used(c + k) for k in range(n)):
                    out = list(range(c, c + n))
                    break
                c += 1
        else:
            while len(out) < n:
                if c - 2 >= self.nclusters:
                    sys.exit("exfat-populate: volume full")
                if not self.bit_used(c):
                    out.append(c)
                    c += 1  # leave a gap after each cluster
                c += 1
        for k in out:
            self.bit_set(k)
        self.next_free = out[-1] + 1 if out else self.next_free
        return out

    def link(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            self.fat_set(a, b)
        if clusters:
            self.fat_set(clusters[-1], 0xFFFFFFFF)

    def write_run(self, clusters, data):
        # Only the bytes there are: the clusters of a fresh volume read as
        # zeros already, and a sparse image file stays sparse.
        for k, c in enumerate(clusters):
            chunk = data[k * self.cs:(k + 1) * self.cs].rstrip(b"\0")
            if chunk:
                self.write(self.cl_off(c), chunk)

    def finish(self, dirty):
        self.write(self.cl_off(self.bitmap_cl), bytes(self.bitmap))
        used = sum(bin(b).count("1") for b in self.bitmap)
        pct = used * 100 // self.nclusters if self.nclusters else 0
        for base in (0, 12):  # main and backup boot sector: not checksummed
            off = base << self.sshift
            self.write(off + 0x70, bytes([pct]))
        if dirty:
            flags = struct.unpack_from("<H", self.boot, 0x6A)[0] | 0x0002
            self.write(0x6A, struct.pack("<H", flags))
        self.f.close()


def name_hash(fs, units):
    h = 0
    for u in units:
        u = fs.up(u)
        for b in (u & 0xFF, u >> 8):
            h = ((0x8000 if h & 1 else 0) + (h >> 1) + b) & 0xFFFF
    return h


def set_checksum(entries):
    buf = b"".join(entries)
    c = 0
    for i, b in enumerate(buf):
        if i in (2, 3):
            continue
        c = ((0x8000 if c & 1 else 0) + (c >> 1) + b) & 0xFFFF
    return c


def utf16(name):
    b = name.encode("utf-16-le", "surrogatepass")
    return list(struct.unpack("<%dH" % (len(b) // 2), b))


def entry_set(fs, name, attr, first, size, alloc_size, nofat):
    units = utf16(name)
    if not units or len(units) > 255:
        raise ValueError("bad name length: %r" % name)
    nname = (len(units) + 14) // 15
    e1 = bytearray(32)
    e1[0] = 0x85
    e1[1] = 1 + nname
    struct.pack_into("<H", e1, 4, attr)
    struct.pack_into("<III", e1, 8, TS, TS, TS)
    e1[20] = e1[21] = 0
    e1[22] = e1[23] = e1[24] = 0x80  # UTC offset valid, +00:00
    e2 = bytearray(32)
    e2[0] = 0xC0
    e2[1] = (0x01 if first else 0) | (0x02 if nofat and first else 0)
    e2[3] = len(units)
    struct.pack_into("<H", e2, 4, name_hash(fs, units))
    struct.pack_into("<Q", e2, 8, size if first else 0)  # ValidDataLength
    struct.pack_into("<I", e2, 20, first)
    struct.pack_into("<Q", e2, 24, size if first else 0)  # DataLength (the size)
    ents = [e1, e2]
    for k in range(nname):
        e = bytearray(32)
        e[0] = 0xC1
        part = units[k * 15:(k + 1) * 15]
        for j, u in enumerate(part):
            struct.pack_into("<H", e, 2 + 2 * j, u)
        ents.append(e)
    struct.pack_into("<H", e1, 2, set_checksum(ents))
    return b"".join(ents)


def entries_needed(name):
    return 2 + (len(utf16(name)) + 14) // 15


def populate(fs, host, fragment):
    def place(path, data_len):
        n = (data_len + fs.cs - 1) // fs.cs
        return fs.alloc(n, fragment) if n else []

    def build_dir(hostdir):
        """Write the children of hostdir; return the entry bytes."""
        out = b""
        names = sorted(os.listdir(hostdir), key=lambda n: n.encode("utf-8", "surrogateescape"))
        for n in names:
            p = os.path.join(hostdir, n)
            if os.path.islink(p):
                continue
            if os.path.isdir(p):
                body = build_dir(p)
                if not body:
                    body = b""
                size = max(len(body), 1)
                size = (size + fs.cs - 1) // fs.cs * fs.cs
                cl = place(p, size)
                fs.write_run(cl, body.ljust(size, b"\0"))
                if fragment:
                    fs.link(cl)
                out += entry_set(fs, n, 0x10, cl[0], size, size, not fragment)
            elif os.path.isfile(p):
                with open(p, "rb") as f:
                    data = f.read()
                cl = place(p, len(data))
                if cl:
                    fs.write_run(cl, data)
                    if fragment:
                        fs.link(cl)
                alloc = len(cl) * fs.cs
                out += entry_set(fs, n, 0x20, cl[0] if cl else 0, len(data), alloc,
                                 not fragment)
        return out

    body = build_dir(host)
    # append to the root directory after its existing entries
    root_cl = list(fs.chain(fs.root))
    raw = b"".join(fs.read(fs.cl_off(c), fs.cs) for c in root_cl)
    used = 0
    for i in range(0, len(raw), 32):
        if raw[i] == 0:
            break
        used = i + 32
    need = used + len(body)
    while len(root_cl) * fs.cs < need + 32:
        nc = fs.alloc(1, fragment)[0]
        if any(fs.read(fs.cl_off(nc), fs.cs)):
            fs.write(fs.cl_off(nc), b"\0" * fs.cs)
        root_cl.append(nc)
    fs.link(root_cl)
    newraw = (raw[:used] + body).ljust(len(root_cl) * fs.cs, b"\0")
    fs.write_run(root_cl, newraw)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit("usage: exfat-populate.py IMAGE HOSTDIR [--fragment] [--dirty]")
    fs = ExFat(args[0])
    populate(fs, args[1], "--fragment" in args)
    fs.finish("--dirty" in args)


if __name__ == "__main__":
    main()
