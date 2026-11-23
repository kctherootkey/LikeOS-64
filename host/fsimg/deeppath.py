#!/usr/bin/env python3
# Deep-path cases for the filesystem harness (host/test-deep-paths.sh):
# runner scripts (host/test-fs-main.c commands) and host trees that drive a
# filesystem driver with whole paths up to the kernel's longest one
# (VFS_MAX_PATH = 4096 bytes with the NUL: 4095 bytes of path), names of
# NAME_MAX (255) bytes, long symbolic link targets and renames across deep
# directories.
#
#   deeppath.py rw-script [--symlinks] [--xdir-rename] [--name N]
#       a read-write runner script: builds the tree, checks it, cycles the
#       mount, checks again, renames, removes everything again
#   deeppath.py expect [--symlinks]
#       the readlink lines the rw script must print (one per line)
#   deeppath.py mktree DIR [--symlinks] [--total N]
#       the same tree on the host (for an ISO image made from DIR)
#   deeppath.py ro-script [--symlinks]
#       a read-only runner script checking that tree on a mounted image
#
# Copyright (C) 2026 The LikeOS Project

import os
import sys

PATH_MAX = 4096          # VFS_MAX_PATH, the NUL included
NAME_MAX = 255
SEG = 200                # length of most directory names
DATA = "deep-path-file"  # contents of the deepest file


NAME_LEN = NAME_MAX      # the long name's length (--name N)
TOTAL = PATH_MAX - 1     # the deepest file's path length (--total N)


def levels():
    """Directory names d00.., the deepest file's name, and the NAME_LEN-byte
    name used half way down.  The deepest file's absolute path is exactly
    PATH_MAX - 1 bytes."""
    names = []
    total = 0
    # leave room for "/" + a file name of at least 100 bytes
    while total + 1 + SEG + 1 + 100 <= TOTAL:
        i = len(names)
        names.append(("d%02d_" % i) + "x" * (SEG - 4))
        total += 1 + SEG
    fname_len = TOTAL - total - 1
    if fname_len > 200:
        # one shorter directory more, so the file name is 150 bytes
        extra = fname_len - 150 - 1
        i = len(names)
        names.append(("d%02d_" % i) + "x" * (extra - 4))
        total += 1 + extra
        fname_len = TOTAL - total - 1
    assert 1 <= fname_len <= NAME_MAX, fname_len
    fname = "f" + "y" * (fname_len - 1)
    long_name = "n" * NAME_LEN
    return names, fname, long_name


def dirpath(names, k=None):
    k = len(names) if k is None else k
    return "/" + "/".join(names[:k])


def quote(s):
    # the runner tokenizes on blanks; our names have none
    return s


def links(names, fname):
    """(link path, target) pairs; the targets lead to the deepest dir."""
    deep = dirpath(names)
    out = []
    # absolute: ~3900 bytes
    out.append(("/la", deep))
    # relative, long: from the deepest directory "../<last>/" repeated
    last = names[-1]
    rel = ""
    while len(rel) + 4 + len(last) + 1 + len(fname) < min(3000, TOTAL - 100):
        rel += "../" + last + "/"
    out.append((deep + "/lr", rel + fname))
    # relative, short: back to the root
    # (no trailing slash: Rock Ridge SL records cannot keep one)
    out.append((deep + "/up", "/".join([".."] * len(names))))
    return out


def rw_script(symlinks, xdir):
    names, fname, long_name = levels()
    deep = dirpath(names)
    half = dirpath(names, len(names) // 2)
    L = []
    a = L.append
    a("mount")
    for k in range(1, len(names) + 1):
        a("mkdir " + dirpath(names, k))
    f = deep + "/" + fname
    assert len(f) == TOTAL, len(f)
    a("write %s text:%s" % (f, DATA))
    a("check %s text:%s" % (f, DATA))
    a("stat " + f)
    a("write %s/%s text:n255" % (half, long_name))
    a("check %s/%s text:n255" % (half, long_name))
    a("write /top text:top")
    if symlinks:
        for lp, t in links(names, fname):
            a("symlink %s %s" % (t, lp))
            a("readlink " + lp)
        a("check /la/%s text:%s" % (fname, DATA))
        a("check %s/lr text:%s" % (deep, DATA))
        a("check %s/up/top text:top" % deep)
        a("symlink /loop /loop")
        a("=LOOP stat /loop")
    a("cycle")
    a("check %s text:%s" % (f, DATA))
    a("check %s/%s text:n255" % (half, long_name))
    if symlinks:
        for lp, t in links(names, fname):
            a("readlink " + lp)
        a("check /la/%s text:%s" % (fname, DATA))
    # renames within one directory, deep down and at the top
    a("rename %s %s/g" % (f, deep))
    a("check %s/g text:%s" % (deep, DATA))
    a("rename %s/g %s" % (deep, f))
    a("rename /%s /e00" % names[0])
    a("check /e00/%s text:%s" % ("/".join(names[1:] + [fname]), DATA))
    a("rename /e00 /%s" % names[0])
    if xdir:
        # across directories: deepest file to the top and back, a subtree
        # out of the tree and back
        a("rename %s /moved" % f)
        a("check /moved text:" + DATA)
        a("rename /moved " + f)
        a("rename %s /sub" % dirpath(names, 2))
        a("check /sub/%s text:%s" % ("/".join(names[2:] + [fname]), DATA))
        a("rename /sub " + dirpath(names, 2))
    a("check %s text:%s" % (f, DATA))
    a("cycle")
    a("check %s text:%s" % (f, DATA))
    # everything away again
    if symlinks:
        for lp, t in links(names, fname):
            a("unlink " + lp)
        a("unlink /loop")
    a("unlink " + f)
    a("unlink %s/%s" % (half, long_name))
    a("unlink /top")
    for k in range(len(names), 0, -1):
        a("rmdir " + dirpath(names, k))
    a("=NOT_FOUND stat /" + names[0])
    a("umount")
    return "\n".join(L) + "\n"


def expect(symlinks):
    names, fname, _ = levels()
    if not symlinks:
        return ""
    return "".join("readlink %s -> %s\n" % (lp, t)
                   for lp, t in links(names, fname))


def mktree(root, symlinks):
    """The tree under `root', made a directory at a time from inside it
    (with `root' in front the host's own paths would be too long)."""
    names, fname, long_name = levels()
    old = os.getcwd()
    root = os.path.abspath(root)
    try:
        os.chdir(root)
        with open("top", "w") as fh:
            fh.write("top")
        for k, n in enumerate(names):
            os.mkdir(n)
            os.chdir(n)
            if k + 1 == len(names) // 2:
                with open(long_name, "w") as fh:
                    fh.write("n255")
        with open(fname, "w") as fh:
            fh.write(DATA)
        if symlinks:
            for lp, t in links(names, fname):
                d, b = lp.rsplit("/", 1)
                os.chdir(root)
                for n in [x for x in d.split("/") if x]:
                    os.chdir(n)
                os.symlink(t, b)
    finally:
        os.chdir(old)


def ro_script(symlinks):
    names, fname, long_name = levels()
    deep = dirpath(names)
    f = deep + "/" + fname
    L = ["mount ro",
         "stat " + f,
         "check %s text:%s" % (f, DATA),
         "check %s/%s text:n255" % (dirpath(names, len(names) // 2),
                                    long_name),
         "ls " + deep]
    if symlinks:
        for lp, t in links(names, fname):
            L.append("readlink " + lp)
        L.append("check /la/%s text:%s" % (fname, DATA))
        L.append("check %s/lr text:%s" % (deep, DATA))
        L.append("check %s/up/top text:top" % deep)
    L.append("=ROFS write /new text:x")
    L.append("umount")
    return "\n".join(L) + "\n"


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__ if __doc__ else "usage\n")
        return 2
    global NAME_LEN, TOTAL
    cmd = argv[1]
    if "--total" in argv:
        TOTAL = int(argv[argv.index("--total") + 1])
    sl = "--symlinks" in argv
    if "--name" in argv:
        NAME_LEN = int(argv[argv.index("--name") + 1])
    if cmd == "rw-script":
        sys.stdout.write(rw_script(sl, "--xdir-rename" in argv))
    elif cmd == "expect":
        sys.stdout.write(expect(sl))
    elif cmd == "mktree":
        mktree(argv[2], sl)
    elif cmd == "ro-script":
        sys.stdout.write(ro_script(sl))
    else:
        sys.stderr.write("deeppath.py: unknown command %s\n" % cmd)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
