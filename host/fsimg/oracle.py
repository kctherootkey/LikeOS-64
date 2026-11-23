#!/usr/bin/env python3
# Independent views of an image, through the host's own filesystem tools,
# to check what a LikeOS driver reads and writes.
#
#   oracle.py fsck FSTYPE IMAGE
#       structural check, never modifying the image:
#         ntfs  ntfsfix -n (plus: refuses a volume marked dirty unless
#               --allow-dirty)        exfat  fsck.exfat -n
#         iso   isoinfo -d and a full listing   fat  fsck.fat -n
#         ext4  e2fsck -fn
#       status 0 clean, 1 problems found, 77 tool missing
#   oracle.py manifest FSTYPE IMAGE
#       the image's tree in the runner's manifest format (see mktree.py):
#         ntfs  ntfsls + ntfscat (symbolic links / reparse points as
#               "l PATH -> ?": the target is not compared)
#         iso   isoinfo -R (Rock Ridge names, symbolic link targets)
#         iso-joliet  the Joliet tree, read directly (UTF-16 names as UTF-8)
#         iso-plain   the ISO 9660 tree, read directly; names without ";1"
#                     and trailing dot, lower case
#         fat   mtools (mdir / mtype)
#         ext4  debugfs rdump into a temporary directory
#
# Copyright (C) 2026 The LikeOS Project
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mktree  # noqa: E402


def have(tool):
    return shutil.which(tool) is not None


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kw)


def fsck(fstype, img, opts):
    if fstype == "ntfs":
        if not have("ntfsfix"):
            return 77
        r = run(["ntfsfix", "-n", img])
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        sys.stdout.write(out)
        bad = r.returncode != 0 or "error" in out.lower() and "successfully" not in out
        if not opts.get("allow_dirty") and have("ntfsinfo"):
            r2 = run(["ntfsinfo", "-m", img])
            o2 = (r2.stdout + r2.stderr).decode("utf-8", "replace")
            if "scheduled for check" in o2 or "is dirty" in o2.lower():
                print("oracle: the volume is marked dirty")
                bad = True
        return 1 if bad else 0
    if fstype == "exfat":
        if not have("fsck.exfat"):
            return 77
        r = run(["fsck.exfat", "-n", img])
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        sys.stdout.write(out)
        return 0 if r.returncode == 0 and "clean" in out else 1
    if fstype == "iso":
        if not have("isoinfo"):
            return 77
        r = run(["isoinfo", "-d", "-i", img])
        sys.stdout.write(r.stdout.decode("utf-8", "replace"))
        r2 = run(["isoinfo", "-R", "-l", "-i", img])
        return 0 if r.returncode == 0 and r2.returncode == 0 else 1
    if fstype in ("fat", "fat32", "vfat"):
        if not have("fsck.fat"):
            return 77
        r = run(["fsck.fat", "-n", img])
        sys.stdout.write((r.stdout + r.stderr).decode("utf-8", "replace"))
        return 0 if r.returncode == 0 else 1
    if fstype in ("ext4", "ext2", "ext3"):
        if not have("e2fsck"):
            return 77
        r = run(["e2fsck", "-fn", img], env=dict(os.environ, LANG="C", LC_ALL="C"))
        sys.stdout.write((r.stdout + r.stderr).decode("utf-8", "replace"))
        return 0 if r.returncode == 0 else 1
    sys.exit("oracle: unknown fstype %s" % fstype)


def esc(s):
    return mktree.esc(s)


def sha_of(data):
    return hashlib.sha256(data).hexdigest()


def ntfs_manifest(img):
    lines = ["d /"]
    stack = ["/"]
    while stack:
        d = stack.pop()
        r = run(["ntfsls", "-F", "-p", d, img])
        names = [l for l in r.stdout.decode("utf-8", "surrogateescape").split("\n")
                 if l and l not in ("./", "../")]
        names.sort(key=lambda n: n.rstrip("/").encode("utf-8", "surrogateescape"))
        subdirs = []
        for n in names:
            isdir = n.endswith("/")
            n = n.rstrip("/")
            p = (d.rstrip("/") + "/" + n)
            info = run(["ntfsinfo", "-F", p, img]).stdout.decode("utf-8", "replace")
            if "REPARSE_POINT" in info:
                lines.append("l %s -> ?" % esc(p))
                continue
            if isdir:
                lines.append("d %s" % esc(p))
                subdirs.append(p)
                continue
            data = run(["ntfscat", img, p]).stdout
            lines.append("f %s %d %s" % (esc(p), len(data), sha_of(data)))
        stack.extend(reversed(subdirs))
    return lines


def iso_name(name, mode):
    """Plain ISO 9660 names as a reader conventionally shows them: no
    ";1" version, no trailing dot, lower case."""
    if mode != "plain":
        return name
    name = re.sub(r";\d+$", "", name)
    if name.endswith("."):
        name = name[:-1]
    return name.lower()


def iso_direct_manifest(img, mode):
    """Joliet or plain ISO 9660 tree read straight from the image (isoinfo
    cannot print Joliet names as UTF-8)."""
    import struct
    f = open(img, "rb")

    def rd(off, n):
        f.seek(off)
        return f.read(n)

    root = None
    for sct in range(16, 64):
        d = rd(sct * 2048, 2048)
        if d[1:6] != b"CD001":
            break
        if mode == "plain" and d[0] == 1:
            root = d[156:190]
            break
        if mode == "joliet" and d[0] == 2 and d[88:91] in (b"%/@", b"%/C", b"%/E"):
            root = d[156:190]
            break
        if d[0] == 255:
            break
    if root is None:
        sys.exit("oracle: no %s volume descriptor" % mode)
    lines = ["d /"]
    stack = [("/", struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])]
    while stack:
        path, lba, size = stack.pop()
        data = rd(lba * 2048, size)
        i = 0
        ents = []
        while i < len(data):
            l = data[i]
            if l == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + l]
            i += l
            nl = rec[32]
            raw = rec[33:33 + nl]
            if raw in (b"\x00", b"\x01"):
                continue
            if mode == "joliet":
                name = raw.decode("utf-16-be", "surrogatepass")
                name = re.sub(r";\d+$", "", name)
            else:
                name = iso_name(raw.decode("latin-1"), "plain")
            el, sz = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            fl = rec[25]
            if ents and ents[-1][0] == name and ents[-1][3] & 0x80:
                ents[-1][1].append((el, sz))
                ents[-1][3] = fl
                continue
            ents.append([name, [(el, sz)], fl & 2, fl])
        subs = []
        for name, exts, isdir, _ in sorted(ents, key=lambda e: e[0].encode("utf-8", "surrogatepass")):
            p = path.rstrip("/") + "/" + name
            if isdir:
                lines.append("d %s" % esc(p))
                subs.append((p, exts[0][0], exts[0][1]))
            else:
                h = hashlib.sha256()
                total = 0
                for el, sz in exts:
                    left, off = sz, el * 2048
                    while left:
                        chunk = rd(off, min(left, 1 << 20))
                        if not chunk:
                            break
                        h.update(chunk)
                        off += len(chunk)
                        left -= len(chunk)
                        total += len(chunk)
                lines.append("f %s %d %s" % (esc(p), total, h.hexdigest()))
        stack.extend(reversed(subs))
    return lines


def iso_manifest(img, mode="rr"):
    flag = {"rr": ["-R"], "joliet": ["-J", "-j", "utf8"], "plain": []}[mode]
    r = run(["isoinfo"] + flag + ["-l", "-i", img])
    cur = None
    ents = []
    for line in r.stdout.decode("utf-8", "surrogateescape").split("\n"):
        m = re.match(r"^Directory listing of (.*)$", line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r"^([dl-])\S+\s+\d+\s+\d+\s+\d+\s+(\d+)\s+\S+\s+\d+\s+\d+\s+\[\s*\d+\s+\d+\]\s+(.*?)\s*$", line)
        if not m or cur is None:
            continue
        t, size, name = m.group(1), int(m.group(2)), m.group(3)
        if name in (".", "..", "\\x00", "\\x01") or not name:
            continue
        tgt = None
        if t == "l" and " -> " in name:
            name, tgt = name.split(" -> ", 1)
        ents.append((cur.rstrip("/") + "/" + name, t, size, tgt, name))
    lines = ["d /"]
    for p, t, size, tgt, name in ents:
        shown = "/".join(iso_name(c, mode) for c in p.split("/"))
        if t == "d":
            lines.append("d %s" % esc(shown))
        elif t == "l":
            lines.append("l %s -> %s" % (esc(shown), esc(tgt or "")))
        else:
            data = run(["isoinfo"] + flag + ["-i", img, "-x", p]).stdout
            lines.append("f %s %d %s" % (esc(shown), len(data), sha_of(data)))
    return lines


def dir_manifest_after(cmd, img, sub=""):
    tmp = tempfile.mkdtemp(prefix="oracle.")
    try:
        cmd(tmp)
        return mktree.manifest(os.path.join(tmp, sub) if sub else tmp, {})
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def manifest(fstype, img):
    if fstype == "ntfs":
        return ntfs_manifest(img)
    if fstype == "iso":
        return iso_manifest(img)
    if fstype == "iso-joliet":
        return iso_direct_manifest(img, "joliet")
    if fstype == "iso-plain":
        return iso_direct_manifest(img, "plain")
    if fstype in ("fat", "fat32", "vfat"):
        def cp(d):
            run(["mcopy", "-s", "-n", "-p", "-i", img, "::/", d + "/"])
        return dir_manifest_after(cp, img)
    if fstype in ("ext4", "ext2", "ext3"):
        def cp(d):
            run(["debugfs", "-R", "rdump / %s" % d, img])
        return dir_manifest_after(cp, img)
    sys.exit("oracle: no manifest reader for %s" % fstype)


def main():
    a = sys.argv[1:]
    opts = {"allow_dirty": "--allow-dirty" in a}
    a = [x for x in a if not x.startswith("--")]
    if len(a) == 3 and a[0] == "fsck":
        sys.exit(fsck(a[1], a[2], opts))
    if len(a) == 3 and a[0] == "manifest":
        for l in manifest(a[1], a[2]):
            print(l)
        return
    sys.exit("usage: oracle.py fsck|manifest FSTYPE IMAGE [--allow-dirty]")


if __name__ == "__main__":
    main()
