#!/usr/bin/env python3
# Compares two directory trees object by object: type, mode, owner, size,
# modification time, link target, device number, extended attributes,
# hard-link grouping and file contents (apparent size and bytes, which
# covers sparse files).
#
#   compare_trees.py [--seconds] [--no-xattrs] [--no-root-meta] SRC DST
#
# Exits 0 when the trees match, 1 with a list of differences otherwise.
#
# Copyright (C) 2026 The LikeOS Project

import os
import stat
import sys


def walk(root):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        rel = os.path.relpath(dirpath, root)
        out[rel] = os.lstat(dirpath)
        for n in dirnames + filenames:
            p = os.path.join(dirpath, n)
            out[os.path.normpath(os.path.join(rel, n))] = os.lstat(p)
    return out


def xattrs(path):
    try:
        names = os.listxattr(path, follow_symlinks=False)
    except OSError:
        return {}
    res = {}
    for n in names:
        try:
            res[n] = os.getxattr(path, n, follow_symlinks=False)
        except OSError as e:
            res[n] = "error %d" % e.errno
    return res


def same_content(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x = fa.read(1 << 20)
            y = fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def main():
    args = sys.argv[1:]
    seconds = "--seconds" in args
    no_x = "--no-xattrs" in args
    no_root = "--no-root-meta" in args
    args = [a for a in args if not a.startswith("--")]
    src, dst = args
    a = walk(src)
    b = walk(dst)
    diffs = []
    for k in sorted(set(a) | set(b)):
        if k not in b:
            diffs.append("missing in extracted tree: %s" % k)
            continue
        if k not in a:
            diffs.append("extra in extracted tree: %s" % k)
            continue
        sa, sb = a[k], b[k]
        pa, pb = os.path.join(src, k), os.path.join(dst, k)
        if stat.S_IFMT(sa.st_mode) != stat.S_IFMT(sb.st_mode):
            diffs.append("%s: type %o != %o" % (k, stat.S_IFMT(sa.st_mode),
                                                stat.S_IFMT(sb.st_mode)))
            continue
        root_skip = no_root and k == "."
        is_link = stat.S_ISLNK(sa.st_mode)
        if not is_link and not root_skip and \
                stat.S_IMODE(sa.st_mode) != stat.S_IMODE(sb.st_mode):
            diffs.append("%s: mode %o != %o" % (k, stat.S_IMODE(sa.st_mode),
                                                stat.S_IMODE(sb.st_mode)))
        if not root_skip and (sa.st_uid, sa.st_gid) != (sb.st_uid, sb.st_gid):
            diffs.append("%s: owner %d:%d != %d:%d" % (
                k, sa.st_uid, sa.st_gid, sb.st_uid, sb.st_gid))
        if not root_skip:
            ma, mb = sa.st_mtime_ns, sb.st_mtime_ns
            if seconds:
                ma, mb = ma // 10**9, mb // 10**9
            if ma != mb:
                diffs.append("%s: mtime %d != %d" % (k, ma, mb))
        if is_link:
            if os.readlink(pa) != os.readlink(pb):
                diffs.append("%s: link %s != %s" % (k, os.readlink(pa),
                                                    os.readlink(pb)))
        elif stat.S_ISREG(sa.st_mode):
            if sa.st_size != sb.st_size:
                diffs.append("%s: size %d != %d" % (k, sa.st_size, sb.st_size))
            elif not same_content(pa, pb):
                diffs.append("%s: contents differ" % k)
        elif stat.S_ISCHR(sa.st_mode) or stat.S_ISBLK(sa.st_mode):
            if sa.st_rdev != sb.st_rdev:
                diffs.append("%s: device %x != %x" % (k, sa.st_rdev, sb.st_rdev))
        if not no_x and not is_link:
            xa, xb = xattrs(pa), xattrs(pb)
            if xa != xb:
                diffs.append("%s: xattrs %r != %r" % (k, xa, xb))
    # hard-link groups
    ga, gb = {}, {}
    for k, s in a.items():
        if stat.S_ISREG(s.st_mode) and s.st_nlink > 1:
            ga.setdefault((s.st_dev, s.st_ino), set()).add(k)
    for grp in ga.values():
        inos = {(b[k].st_dev, b[k].st_ino) for k in grp if k in b}
        if len(inos) != 1:
            diffs.append("hard links not preserved: %s" % sorted(grp))
    for d in diffs:
        print("DIFF " + d)
    print("compare_trees: %d objects, %d differences" % (len(a), len(diffs)))
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
