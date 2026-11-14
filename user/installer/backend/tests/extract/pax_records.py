#!/usr/bin/env python3
# Writes an archive whose pax records exercise what GNU tar under fakeroot
# does not produce (ACL text records, LIBARCHIVE.xattr, fractional time
# stamps, a global header), and checks what the extractor made of it.
# The global header also carries path, linkpath and size records, which
# must not rename, relink or resize the entries after it; its gid and
# mtime records must still apply.
#
#   pax_records.py make ARCHIVE
#   pax_records.py check DIR
#
# Copyright (C) 2026 The LikeOS Project

import io
import os
import struct
import sys
import tarfile

U = 0xffffffff


def acl(ents):
    return struct.pack("<I", 2) + b"".join(struct.pack("<HHI", *e)
                                           for e in ents)


def add(tf, name, kind=tarfile.REGTYPE, data=b"", pax=None, mode=0o644,
        link=""):
    ti = tarfile.TarInfo(name)
    ti.type = kind
    ti.mode = mode
    ti.linkname = link
    ti.mtime = 1700000000
    ti.size = len(data) if kind == tarfile.REGTYPE else 0
    if pax:
        ti.pax_headers = pax
    tf.addfile(ti, io.BytesIO(data) if kind == tarfile.REGTYPE else None)


def make(path):
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT,
                      pax_headers={"gid": "777", "comment": "global",
                                   "mtime": "1650000000",
                                   "path": "global-path",
                                   "linkpath": "global-link",
                                   "size": "3"}) as tf:
        add(tf, "aclf", data=b"acl\n", pax={
            "SCHILY.acl.access":
                "user::rw-,user:bob:r--:1000,group::r--,other::---"})
        add(tf, "acld", tarfile.DIRTYPE, mode=0o755, pax={
            "SCHILY.acl.access": "u::rwx,u:1000:r-x,g::r-x,m::r-x,o::r-x",
            "SCHILY.acl.default": "user::rwx\ngroup::r-x\nother::---\n"})
        add(tf, "lax", data=b"x\n", pax={
            "LIBARCHIVE.xattr.user.b%20c": "aGVsbG8=",
            "SCHILY.xattr.user.plain": "value"})
        add(tf, "times", data=b"t\n", pax={
            "mtime": "1700000000.5", "atime": "1600000000.25"})
        add(tf, "baseacl", data=b"b\n", pax={
            "SCHILY.acl.access": "user::rw-,group::r--,other::r--"})
        add(tf, "sl", tarfile.SYMTYPE, mode=0o777, link="aclf")


def check(d):
    errs = []

    def x(p, n):
        try:
            return os.getxattr(os.path.join(d, p), n, follow_symlinks=False)
        except OSError:
            return None

    want = acl([(1, 6, U), (2, 4, 1000), (4, 4, U), (16, 4, U), (32, 0, U)])
    if x("aclf", "system.posix_acl_access") != want:
        errs.append("aclf access ACL %r" % x("aclf", "system.posix_acl_access"))
    want = acl([(1, 7, U), (2, 5, 1000), (4, 5, U), (16, 5, U), (32, 5, U)])
    if x("acld", "system.posix_acl_access") != want:
        errs.append("acld access ACL %r" % x("acld", "system.posix_acl_access"))
    want = acl([(1, 7, U), (4, 5, U), (32, 0, U)])
    if x("acld", "system.posix_acl_default") != want:
        errs.append("acld default ACL %r" % x("acld", "system.posix_acl_default"))
    if x("baseacl", "system.posix_acl_access") is not None:
        errs.append("baseacl got an access ACL equal to its mode")
    if x("lax", "user.b c") != b"hello":
        errs.append("LIBARCHIVE.xattr %r" % x("lax", "user.b c"))
    if x("lax", "user.plain") != b"value":
        errs.append("SCHILY.xattr %r" % x("lax", "user.plain"))
    st = os.lstat(os.path.join(d, "times"))
    if st.st_mtime_ns != 1700000000500000000:
        errs.append("mtime %d" % st.st_mtime_ns)
    if st.st_atime_ns != 1600000000250000000:
        errs.append("atime %d" % st.st_atime_ns)
    if st.st_gid != 777:
        errs.append("global gid not applied: %d" % st.st_gid)
    # the global path, linkpath and size records are ignored
    for n in ("aclf", "acld", "lax", "times", "baseacl", "sl"):
        if not os.path.lexists(os.path.join(d, n)):
            errs.append("%s missing (global path applied?)" % n)
    if os.path.lexists(os.path.join(d, "global-path")):
        errs.append("an entry was named after the global path record")
    try:
        if os.readlink(os.path.join(d, "sl")) != "aclf":
            errs.append("sl -> %s" % os.readlink(os.path.join(d, "sl")))
    except OSError as e:
        errs.append("sl: %s" % e)
    try:
        with open(os.path.join(d, "aclf"), "rb") as f:
            if f.read() != b"acl\n":
                errs.append("aclf content changed (global size applied?)")
        st = os.lstat(os.path.join(d, "aclf"))
        if st.st_mtime_ns != 1650000000000000000:
            errs.append("global mtime not applied: %d" % st.st_mtime_ns)
    except OSError as e:
        errs.append("aclf: %s" % e)
    for e in errs:
        print("DIFF " + e)
    print("pax_records: %d problems" % len(errs))
    return 1 if errs else 0


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2])
        sys.exit(0)
    sys.exit(check(sys.argv[2]))
