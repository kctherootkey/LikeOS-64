#!/usr/bin/env python3
# Deterministic test trees and manifests for the filesystem harness.
#
#   mktree.py tree PROFILE SEED DIR [--no-symlinks] [--no-hardlinks]
#             [--ascii] [--dos] [--maxname N] [--big MB]
#       populate DIR (must not exist) with a tree described by PROFILE:
#         basic  files of many sizes (0, 1, block boundaries, up to 300 KB),
#                nested directories, long and non-ASCII names, symbolic
#                links (relative, absolute, dangling), a hard link
#         many   one directory with 3000 files (index allocation, multi-
#                cluster directories) plus a few small ones
#         deep   a 40-level directory chain with a file at each level
#         big    one file of --big MB (default 64) plus small ones
#         tiny   a handful of small files (fuzzing seeds)
#   mktree.py manifest DIR [--no-symlinks] [--ci]
#       print DIR in the runner's manifest format (test-fs "manifest"):
#         d /path | f /path SIZE SHA256 | l /path -> TARGET
#       names escaped as the runner escapes them (\xHH for bytes <= ' ',
#       '\', >= 0x7f), sorted the way the runner walks (parent first,
#       entries by byte order, subdirectories after their parent's entries)
#   mktree.py compare EXPECTED ACTUAL [--ci] [--no-symlinks] [--no-empty-files]
#       compare two manifests; prints differences, status 1 when any
#
# Content of every file is a pattern derived from the seed and the path,
# so trees are reproducible byte for byte.
#
# Copyright (C) 2026 The LikeOS Project
import hashlib
import os
import sys


def pattern(seed, path, size):
    h = hashlib.sha256(("%d:%s" % (seed, path)).encode()).digest()
    out = bytearray()
    block = h
    while len(out) < size:
        block = hashlib.sha256(block).digest()
        out += block
    return bytes(out[:size])


class Rng:
    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 0x632BE59BD9B4E019) & ((1 << 64) - 1) or 1

    def next(self):
        s = self.s
        s ^= s >> 12
        s ^= (s << 25) & ((1 << 64) - 1)
        s ^= s >> 27
        self.s = s
        return (s * 2685821657736338717) & ((1 << 64) - 1)

    def below(self, n):
        return self.next() % n if n else 0


def wfile(root, rel, seed, size):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(pattern(seed, rel, size))


def tree(profile, seed, root, opts):
    if os.path.exists(root):
        sys.exit("mktree: %s exists" % root)
    os.makedirs(root)
    r = Rng(seed)
    dos = opts.get("dos")
    ascii_only = opts.get("ascii") or dos
    maxname = int(opts.get("maxname", 200))

    def nm(s):
        if dos:
            s = s.replace(" ", "_")
        return s[:maxname]

    if profile in ("basic", "tiny"):
        sizes = [0, 1, 2, 511, 512, 513, 1023, 1024, 1025, 2047, 2048, 2049,
                 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537,
                 131072, 300000]
        if profile == "tiny":
            sizes = [0, 1, 700, 4096, 5000]
        for i, s in enumerate(sizes):
            wfile(root, "size_%06d_%02d.bin" % (s, i), seed, s)
        if profile == "tiny":
            wfile(root, "dir/inner.txt", seed, 33)
            if not opts.get("no_symlinks"):
                os.symlink("dir/inner.txt", os.path.join(root, "link"))
            return
        wfile(root, nm("Mixed Case Name.TXT"), seed, 77)
        wfile(root, nm("a" * 180 + ".long"), seed, 1234)
        if not ascii_only:
            wfile(root, "umlaut_äöüß.txt", seed, 99)
            wfile(root, "cjk_日本語.txt", seed, 4097)
            wfile(root, "euro_€_sign", seed, 3)
        for d in range(4):
            for e in range(3):
                base = "dir%d/sub%d" % (d, e)
                for k in range(3 + r.below(5)):
                    wfile(root, "%s/f%02d.dat" % (base, k), seed, r.below(70000))
            os.makedirs(os.path.join(root, "dir%d/empty" % d), exist_ok=True)
        wfile(root, nm("dir0/with space/and.dots.in.name"), seed, 10)
        if not opts.get("no_symlinks"):
            os.symlink("size_000001_01.bin", os.path.join(root, "rel_link"))
            os.symlink("/dir1/sub1", os.path.join(root, "abs_link"))
            os.symlink("does/not/exist", os.path.join(root, "dangling"))
            os.symlink("../dir1/sub0", os.path.join(root, "dir0/up_link"))
            os.symlink("x" * 150, os.path.join(root, "long_target"))
        if not opts.get("no_hardlinks"):
            os.link(os.path.join(root, "size_004097_14.bin"),
                    os.path.join(root, "dir2/hardlink.bin"))
    elif profile == "many":
        for i in range(int(opts.get("count", 3000))):
            wfile(root, "many/file_%05d_%s" % (i, "x" * (i % 40)), seed, i % 300)
        wfile(root, "small.txt", seed, 10)
    elif profile == "deep":
        path = ""
        for lvl in range(40):
            path = os.path.join(path, "d%02d" % lvl)
            wfile(root, os.path.join(path, "f.txt"), seed, 100 + lvl)
    elif profile == "big":
        mb = int(opts.get("big", 64))
        p = os.path.join(root, "big.bin")
        with open(p, "wb") as f:
            chunk = pattern(seed, "big", 1 << 20)
            for i in range(mb):
                f.write(chunk[i:] + chunk[:i])
        wfile(root, "small.txt", seed, 10)
    else:
        sys.exit("mktree: unknown profile %s" % profile)


def esc(name):
    out = []
    for b in name.encode("utf-8", "surrogateescape"):
        if b <= 0x20 or b == 0x5C or b >= 0x7F:
            out.append("\\x%02x" % b)
        else:
            out.append(chr(b))
    return "".join(out)


def manifest(root, opts):
    lines = []

    def one(rel):
        p = os.path.join(root, rel.lstrip("/"))
        st = os.lstat(p)
        import stat as S
        if S.S_ISLNK(st.st_mode):
            if opts.get("no_symlinks"):
                return None
            return "l %s -> %s" % (esc(rel), esc(os.readlink(p)))
        if S.S_ISDIR(st.st_mode):
            return "d %s" % esc(rel)
        if S.S_ISREG(st.st_mode):
            h = hashlib.sha256()
            with open(p, "rb") as f:
                for blk in iter(lambda: f.read(1 << 20), b""):
                    h.update(blk)
            return "f %s %d %s" % (esc(rel), st.st_size, h.hexdigest())
        return "? %s" % esc(rel)

    lines.append("d /")
    stack = ["/"]
    while stack:
        d = stack.pop()
        names = sorted(os.listdir(os.path.join(root, d.lstrip("/"))),
                       key=lambda n: n.encode("utf-8", "surrogateescape"))
        subdirs = []
        for n in names:
            rel = (d.rstrip("/") + "/" + n)
            l = one(rel)
            if l is None:
                continue
            lines.append(l)
            if l.startswith("d "):
                subdirs.append(rel)
        stack.extend(reversed(subdirs))
    return lines


def load(path):
    with open(path) as f:
        return [l.rstrip("\n") for l in f if l[:2] in ("d ", "f ", "l ", "c ", "b ", "p ", "s ")]


def key(line, ci):
    parts = line.split(" ")
    k = parts[1]
    return k.lower() if ci else k


def compare(exp, act, opts):
    ci = opts.get("ci")
    e = {key(l, ci): l for l in load(exp)}
    a = {key(l, ci): l for l in load(act)}
    bad = 0
    for k in sorted(set(e) | set(a)):
        le, la = e.get(k), a.get(k)
        if le and opts.get("no_symlinks") and le.startswith("l "):
            continue
        if le is None:
            print("extra:   %s" % la)
            bad += 1
        elif la is None:
            print("missing: %s" % le)
            bad += 1
        else:
            fe, fa = le.split(" "), la.split(" ")
            if fe[0] == "l" and fa[0] == "l" and "?" in (fe[-1], fa[-1]):
                continue  # a reader that cannot tell the target
            # compare type, size, hash / target; names compared by key
            if fe[0] != fa[0] or fe[2:] != fa[2:len(fe)]:
                print("differs: %s\n     got: %s" % (le, la))
                bad += 1
    print("compare: %d difference(s), %d expected, %d found" % (bad, len(e), len(a)))
    return 1 if bad else 0


def parse_opts(args):
    o = {}
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a.startswith("--"):
            k = a[2:].replace("-", "_")
            if k in ("maxname", "big", "count"):
                i += 1
                o[k] = args[i]
            else:
                o[k] = True
        else:
            pos.append(a)
        i += 1
    return pos, o


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ if __doc__ else "usage: mktree.py tree|manifest|compare ...")
    cmd = sys.argv[1]
    pos, o = parse_opts(sys.argv[2:])
    if cmd == "tree":
        tree(pos[0], int(pos[1]), pos[2], o)
    elif cmd == "manifest":
        for l in manifest(pos[0], o):
            print(l)
    elif cmd == "compare":
        sys.exit(compare(pos[0], pos[1], o))
    else:
        sys.exit("mktree: unknown command %s" % cmd)


if __name__ == "__main__":
    main()
