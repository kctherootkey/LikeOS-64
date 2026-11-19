#!/usr/bin/env python3
# Writes the hostile and damaged archives of the negative tests.
#
#   make_bad_archives.py DIR OUTSIDE
#
# DIR/dotdot.tar     a member named "../escape.txt"
# DIR/abs.tar        a member named "/abs/file.txt" (unpacked as abs/...)
# DIR/symesc.tar     symlink "evil" -> OUTSIDE, then the file "evil/x"
# DIR/symesc2.tar    symlink "a/b" -> "../..", then "a/b/c/x"
# DIR/hlesc.tar      a hard link to "../../etc/passwd"
# DIR/pre.tar        the file "pre/z" (the test plants pre -> OUTSIDE)
# DIR/over.tar       the file "victim" (the test plants victim -> a file
#                    outside the target)
# DIR/paxsize.tar    "big.txt" whose header says 0 bytes and whose pax
#                    record says 13 (the pax size counts), then "after.txt"
# DIR/bigsuid.tar    "bigsuid", mode 04755, owner 99999999999 (out of
#                    range: no owner is set, so no set-id bit either)
#
# Copyright (C) 2026 The LikeOS Project

import io
import sys
import tarfile


def add_file(tf, name, data=b"payload\n"):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mode = 0o644
    ti.mtime = 1700000000
    tf.addfile(ti, io.BytesIO(data))


def add_link(tf, name, target, kind=tarfile.SYMTYPE):
    ti = tarfile.TarInfo(name)
    ti.type = kind
    ti.linkname = target
    ti.mode = 0o777
    ti.mtime = 1700000000
    tf.addfile(ti)


def add_dir(tf, name):
    ti = tarfile.TarInfo(name)
    ti.type = tarfile.DIRTYPE
    ti.mode = 0o755
    ti.mtime = 1700000000
    tf.addfile(ti)


def ustar_header(name, size, typeflag=b"0", mode=0o644):
    h = bytearray(512)
    h[0:len(name)] = name.encode()
    h[100:108] = b"%07o\0" % mode
    h[108:116] = b"%07o\0" % 0
    h[116:124] = b"%07o\0" % 0
    h[124:136] = b"%011o\0" % size
    h[136:148] = b"%011o\0" % 1700000000
    h[148:156] = b" " * 8
    h[156:157] = typeflag
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    h[148:156] = b"%06o\0 " % sum(h)
    return bytes(h)


def padded(data):
    return data + b"\0" * ((512 - len(data) % 512) % 512)


def pax_record(key, value):
    body = " %s=%s\n" % (key, value)
    n = len(body) + len(str(len(body)))
    if len(str(n)) != len(str(len(body))):
        n += 1
    return (str(n) + body).encode()


def main():
    d, outside = sys.argv[1], sys.argv[2]
    fmt = tarfile.PAX_FORMAT
    with tarfile.open(d + "/dotdot.tar", "w", format=fmt) as tf:
        add_file(tf, "fine.txt")
        add_file(tf, "../escape.txt")
    with tarfile.open(d + "/abs.tar", "w", format=fmt) as tf:
        add_file(tf, "/abs/file.txt", b"absolute\n")
    with tarfile.open(d + "/symesc.tar", "w", format=fmt) as tf:
        add_link(tf, "evil", outside)
        add_file(tf, "evil/x", b"must not land outside\n")
    with tarfile.open(d + "/symesc2.tar", "w", format=fmt) as tf:
        add_dir(tf, "a")
        add_link(tf, "a/b", "../..")
        add_file(tf, "a/b/c/x", b"must not land outside\n")
    with tarfile.open(d + "/hlesc.tar", "w", format=fmt) as tf:
        add_link(tf, "hl", "../../etc/passwd", tarfile.LNKTYPE)
    with tarfile.open(d + "/pre.tar", "w", format=fmt) as tf:
        add_file(tf, "pre/z", b"must not land outside\n")
    with tarfile.open(d + "/over.tar", "w", format=fmt) as tf:
        add_file(tf, "victim", b"replaced inside the target\n")
    data = b"thirteen byte"
    rec = pax_record("size", len(data))
    with open(d + "/paxsize.tar", "wb") as f:
        f.write(ustar_header("PaxHeaders/big.txt", len(rec), b"x"))
        f.write(padded(rec))
        f.write(ustar_header("big.txt", 0))
        f.write(padded(data))
        f.write(ustar_header("after.txt", 6))
        f.write(padded(b"after\n"))
        f.write(b"\0" * 1024)
    with tarfile.open(d + "/bigsuid.tar", "w", format=fmt) as tf:
        ti = tarfile.TarInfo("bigsuid")
        ti.size = 4
        ti.mode = 0o4755
        ti.uid = 99999999999
        ti.gid = 99999999999
        ti.mtime = 1700000000
        tf.addfile(ti, io.BytesIO(b"bin\n"))


if __name__ == "__main__":
    main()
