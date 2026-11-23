#!/usr/bin/env python3
# Randomized write workloads for the exFAT driver on the filesystem
# harness, with an exact model (host/test-audit-exfat.sh drives it).
#
# The runner (host/test-fs.sh's binary) is driven interactively: every
# command is sent as "?cmd" with -v, so its status comes back ("= STATUS")
# and the model decides whether that status is right.  The model knows
# every name (case preserved, compared through exFAT's case folding for
# the characters used), every file's bytes (sparse 64 KiB chunks, so files
# beyond 4 GB cost nothing), open handles of unlinked files, and the free
# space only roughly -- a NOSPC is accepted when the volume is nearly full
# and the model then asks the driver what happened (stat) and checks that
# the outcome is all-or-nothing (or a correct short write).
#
# After every "verify" point the volume is remounted and the whole tree is
# compared with the model ("manifest /": names, types, sizes, SHA-256 of
# every file).  At the end the image is unmounted; the caller runs
# fsck.exfat -n and host/test-exfat-check.py.
#
#   test-audit-exfat.py run BIN IMAGE [options]
#       --seed N --ops N --verify-every N --profile NAME --sector N
#       --record FILE   write the commands sent (for power-cut / fault
#                       sweeps: test-audit-exfat.py replay)
#       --snapshots FILE  with --record: model manifests at every sync
#   profiles: mixed (default), bigdir, deep, big, full, names, holes
#
#   test-audit-exfat.py sweep BIN IMAGE JOURNAL SNAPSHOTS WRITES_AT
#       [--mode prefix|reorder] [--step N] [--samples N] [--sector N]
#       power cuts over a recorded workload's write journal (class Sweep)
#
#   test-audit-exfat.py to4k IMAGE
#       rewrite a 512-byte-sector image as a 4096-byte-sector volume
#
# Copyright (C) 2026 The LikeOS Project

import bisect
import hashlib
import json
import os
import random
import re
import subprocess
import sys

CHUNK = 64 << 10
ZERO_CHUNK = bytes(CHUNK)
FOLD = {ord(a): ord(b) for a, b in zip("abcdefghijklmnopqrstuvwxyzäöüéñ",
                                       "ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÜÉÑ")}


def fold(name):
    return name.translate(FOLD)


def esc(path):
    """A path for the runner's tokenizer: \\xHH for everything special."""
    out = []
    for b in path.encode("utf-8"):
        c = chr(b)
        if c.isascii() and (c.isalnum() or c in "/._-"):
            out.append(c)
        else:
            out.append("\\x%02x" % b)
    return "".join(out)


def mesc(path):
    """A path as the runner's manifest prints it."""
    out = []
    for b in path.encode("utf-8"):
        if b <= 0x20 or b == 0x5C or b >= 0x7F:
            out.append("\\x%02x" % b)
        else:
            out.append(chr(b))
    return "".join(out)


class Data:
    """File content: sparse 64 KiB chunks (absent = zeros), each only as
    long as its highest written byte (thousands of small files cost what
    they hold, not a chunk each)."""

    def __init__(self):
        self.size = 0
        self.ch = {}
        self._sha = None

    def _chunk(self, ci, need):
        c = self.ch.get(ci)
        if c is None:
            c = self.ch[ci] = bytearray()
        if len(c) < need:
            c.extend(bytes(need - len(c)))
        return c

    def write(self, off, buf):
        self._sha = None
        end = off + len(buf)
        p = 0
        while p < len(buf):
            at = off + p
            ci, co = divmod(at, CHUNK)
            n = min(CHUNK - co, len(buf) - p)
            c = self._chunk(ci, co + n)
            c[co:co + n] = buf[p:p + n]
            p += n
        if end > self.size:
            self.size = end

    def fill(self, off, byte, length):
        self._sha = None
        end = off + length
        at = off
        while at < end:
            ci, co = divmod(at, CHUNK)
            n = min(CHUNK - co, end - at)
            c = self.ch.get(ci)
            if byte == 0:
                if c is not None and len(c) > co:
                    k = min(n, len(c) - co)
                    c[co:co + k] = bytes(k)
            else:
                c = self._chunk(ci, co + n)
                c[co:co + n] = bytes([byte]) * n
            at += n
        if end > self.size:
            self.size = end

    def truncate(self, n):
        self._sha = None
        if n < self.size:
            for ci in [k for k in self.ch if k * CHUNK >= n]:
                del self.ch[ci]
            ci, co = divmod(n, CHUNK)
            c = self.ch.get(ci)
            if co and c is not None and len(c) > co:
                del c[co:]
        self.size = n

    def _piece(self, ci, co, k):
        c = self.ch.get(ci)
        if c is None:
            return bytes(k)
        got = bytes(c[co:co + k])
        return got + bytes(k - len(got))

    def read(self, off, n):
        out = bytearray()
        end = min(off + n, self.size)
        at = off
        while at < end:
            ci, co = divmod(at, CHUNK)
            k = min(CHUNK - co, end - at)
            out += self._piece(ci, co, k)
            at += k
        return bytes(out)

    def sha(self):
        if self._sha is None:
            h = hashlib.sha256()
            full = self.size // CHUNK
            for ci in range(full):
                c = self.ch.get(ci)
                h.update(self._piece(ci, 0, CHUNK) if c is not None else ZERO_CHUNK)
            rest = self.size - full * CHUNK
            if rest:
                h.update(self._piece(full, 0, rest))
            self._sha = h.hexdigest()
        return self._sha

    def copy(self):
        d = Data()
        d.size = self.size
        d.ch = {k: bytearray(v) for k, v in self.ch.items()}
        d._sha = self._sha
        return d


class Node:
    def __init__(self, name, kind, parent):
        self.name = name
        self.kind = kind  # 'd' or 'f'
        self.parent = parent
        self.kids = {}  # fold(name) -> Node
        self.data = Data() if kind == "f" else None

    def path(self):
        parts = []
        n = self
        while n.parent is not None:
            parts.append(n.name)
            n = n.parent
        return "/" + "/".join(reversed(parts))

    def depth(self):
        d = 0
        n = self
        while n.parent is not None:
            d += 1
            n = n.parent
        return d

    def under(self, other):
        n = self
        while n is not None:
            if n is other:
                return True
            n = n.parent
        return False


class Finding(Exception):
    pass


class Runner:
    def __init__(self, binp, image, args, stderr_path, timeout=3600):
        pre = []
        # the caller's memory-capped scope bounds everything; a scope of
        # its own (outside the box) only when asked
        if os.environ.get("AUDIT_SCOPE") == "1" and \
                subprocess.run(["systemd-run", "--user", "--scope", "-q", "-p",
                                "MemoryMax=1M", "true"], capture_output=True).returncode == 0:
            pre = ["systemd-run", "--user", "--scope", "-q", "-p",
                   "MemoryMax=" + os.environ.get("AUDIT_MEM", "2G"),
                   "-p", "MemorySwapMax=0", "--"]
        # --pages: the page cache gives pages back as the kernel's does
        # under memory pressure (unbounded, it holds every page written)
        cmd = ["timeout", "-k", "10", str(timeout)] + pre + \
              ["nice", "-n", "10", binp, "-v", "--pages", "32768"] + args + \
              ["-s", "-", image]
        self.err = open(stderr_path, "w")
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:handle_abort=1:handle_sigill=1:"
                       "allocator_may_return_null=1:exitcode=4:detect_stack_use_after_return=1")
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=1:exitcode=5")
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err, env=env, bufsize=0)
        self.n = 0
        self.fails = []
        self.record = None
        self.sent = 0
        self.trace = None

    def cmd(self, c, record=True):
        """Send one command; returns (status, output lines)."""
        self.n += 1
        mark = "@@%d@@" % self.n
        line = "?" + c
        if self.record is not None and record:
            self.record.append(line)
        try:
            self.p.stdin.write((line + "\necho " + mark + "\n").encode())
            self.p.stdin.flush()
        except BrokenPipeError:
            raise Finding("runner died before: %s" % c)
        st = None
        out = []
        while True:
            l = self.p.stdout.readline()
            if not l:
                rc = self.p.wait()
                raise Finding("runner exited (status %d) during: %s" % (rc, c))
            l = l.decode("utf-8", "replace").rstrip("\n")
            if l == mark:
                break
            if l.startswith("> "):
                continue
            if l.startswith("= "):
                st = l[2:]
                continue
            if l.startswith("FAIL"):
                self.fails.append("%s  [during: %s]" % (l, c[:200]))
                continue
            out.append(l)
        self.sent += 1
        if self.trace:
            self.trace.write("%s -> %s%s\n" % (c[:300], st, "".join("\n    " + l for l in out[:5])))
        return st, out

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        out = self.p.stdout.read().decode("utf-8", "replace")
        rc = self.p.wait()
        self.err.close()
        for l in out.splitlines():
            if l.startswith("FAIL"):
                self.fails.append(l)
        return rc


class Workload:
    def __init__(self, a):
        self.a = a
        self.rng = random.Random(a.seed)
        self.root = Node("", "d", None)
        self.handles = {}  # h -> (node or None, Data, path at open)
        self.tmpdir = a.tmpdir
        self.datafile = os.path.join(self.tmpdir, "data.bin")
        self.used = 0
        self.findings = []
        self.r = None
        self.epoch = 0
        self.touch = []  # (epoch, folded path) of every change
        self.snaps = []
        self.nospc_seen = 0
        self.stats = {}

    # ---- helpers ----------------------------------------------------------
    def note(self, what):
        self.stats[what] = self.stats.get(what, 0) + 1

    def bad(self, msg):
        if self.r is not None and self.r.trace:
            self.r.trace.write("FINDING: %s\n" % msg)
        self.findings.append(msg)
        print("FINDING: " + msg, flush=True)
        if len(self.findings) > 20:
            raise Finding("too many findings")

    def touched(self, path):
        self.touch.append((self.epoch, fold(path)))

    def nodes(self, kind=None):
        out = []
        st = [self.root]
        while st:
            n = st.pop()
            if kind is None or n.kind == kind:
                out.append(n)
            if n.kind == "d":
                st.extend(n.kids.values())
        return out

    def pick(self, kind):
        ns = self.nodes(kind)
        if kind == "d" and self.a.profile == "deep" and self.rng.random() < 0.7:
            ns.sort(key=lambda n: -n.depth())
            ns = ns[:3]
        return self.rng.choice(ns) if ns else None

    def name(self, d):
        r = self.rng
        prof = self.a.profile
        for _ in range(50):
            k = r.random()
            if prof == "names" or k < 0.15:
                alpha = "abcxyzABCXYZ019 _-.+=,()[]{}!~'&%$@^äöüéñÄÖÜÉÑ日本😀"
                n = r.choice([1, 2, 5, 12, 30, 60, 120, 200, 255]) if prof == "names" else r.randint(1, 40)
            else:
                alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGH0123456789_-"
                n = r.randint(1, 14)
            s = "".join(r.choice(alpha) for _ in range(n))
            # exFAT counts UTF-16 units
            # exFAT counts UTF-16 units, the VFS bytes (NAME_MAX)
            while len(s.encode("utf-16-le")) // 2 > 255 or len(s.encode()) > 255:
                s = s[:-1]
            s = s.strip(" ")
            s = s.rstrip(".")
            if not s or s in (".", ".."):
                continue
            if fold(s) in d.kids:
                continue
            return s
        return None

    def newdata(self, n):
        r = self.rng
        if n > (8 << 20) or r.random() < 0.1:
            b = r.randrange(256)
            return ("fill:%d:%d" % (b, n), ("fill", b, n))
        buf = r.randbytes(n)
        path = self.datafile
        if self.a.record:
            # the recorded script is replayed later: every write keeps
            # its own data file
            self.ndata = getattr(self, "ndata", 0) + 1
            path = os.path.join(self.tmpdir, "data-%d.bin" % self.ndata)
        with open(path, "wb") as f:
            f.write(buf)
        return ("file:" + path, buf)

    def apply(self, data, off, src):
        if isinstance(src, tuple):
            data.fill(off, src[1], src[2])
        else:
            data.write(off, src)

    def size_pick(self):
        r = self.rng
        cs = self.a.cluster
        k = r.random()
        if k < 0.2:
            return r.randint(0, 64)
        if k < 0.45:
            return r.choice([cs - 1, cs, cs + 1, 4095, 4096, 4097, 2 * cs])
        if k < 0.8:
            return r.randint(0, 3 * max(cs, 4096))
        if k < 0.97:
            return r.randint(0, 300000)
        return r.randint(0, self.a.maxfile)

    def capacity(self):
        return self.a.capacity

    def full_ok(self):
        """Is a NOSPC plausible now?"""
        return self.a.profile == "full" or self.used > self.capacity() * 0.6

    def expect(self, st, want, what):
        if st != want:
            if st == "NOSPC" and self.full_ok():
                self.nospc_seen += 1
                return False
            self.bad("%s: got %s, expected %s" % (what, st, want))
            return False
        return True

    def stat(self, path):
        st, out = self.r.cmd("stat " + esc(path), record=False)
        if st != "OK":
            return st, None, None
        for l in out:
            m = re.search(r" type=(\w) .* size=(\d+) ", l)
            if m:
                return st, m.group(1), int(m.group(2))
        return st, None, None

    def file_sha_check(self, node):
        st, out = self.r.cmd("sha " + esc(node.path()), record=False)
        want = node.data.sha()
        for l in out:
            if l.startswith("sha "):
                p = l.split(" ")
                if p[1] != want or int(p[2]) != node.data.size:
                    self.bad("content of %s after NOSPC: %s %s, model %s %d" %
                             (node.path(), p[1][:16], p[2], want[:16], node.data.size))
                return
        self.bad("sha %s: %s" % (node.path(), st))

    # ---- the operations -----------------------------------------------------
    def op_create(self, size=None):
        d = self.pick("d")
        if d is None or (self.a.profile != "deep" and d.depth() > 20):
            return
        nm = self.name(d)
        if nm is None:
            return
        if size is None:
            size = self.size_pick()
            if self.used + size > self.capacity() * (0.97 if self.a.profile == "full" else 0.5):
                size = min(size, 4096)
        n = Node(nm, "f", d)
        path = n.path()
        if len(path.encode()) > 3800:
            return
        spec, src = self.newdata(size)
        # a different case of an existing name must hit that file
        st, _ = self.r.cmd("write %s %s" % (esc(path), spec))
        self.note("create")
        if st == "OK":
            d.kids[fold(nm)] = n
            self.apply(n.data, 0, src)
            self.used += size
            self.touched(path)
            return
        if not self.expect(st, "OK", "write " + path):
            if st == "NOSPC":
                self.created_short(d, nm, n, src)

    def created_short(self, d, nm, n, src):
        """A write of a new file met a full volume: the file may exist
        (created, its data short -- what fit, or nothing)."""
        path = n.path()
        s2, t, sz = self.stat(path)
        if s2 == "OK":
            d.kids[fold(nm)] = n
            full = src if not isinstance(src, tuple) else None
            if full is None:
                n.data.fill(0, src[1], sz)
            else:
                n.data.write(0, full[:sz])
            n.data.truncate(sz)
            self.used += sz
            self.touched(path)
            self.file_sha_check(n)

    def op_overwrite_existing_case(self):
        """write to an existing file through another case of its name."""
        f = self.pick("f")
        if f is None:
            return
        alt = f.name.swapcase()
        if fold(alt) != fold(f.name):
            return
        path = f.parent.path().rstrip("/") + "/" + alt
        size = min(self.size_pick(), 20000)
        spec, src = self.newdata(size)
        st, _ = self.r.cmd("write %s %s" % (esc(path), spec))
        self.note("write-other-case")
        if self.expect(st, "OK", "write (other case) " + path):
            self.used += size - f.data.size
            f.data.truncate(0)
            self.apply(f.data, 0, src)
            self.touched(f.path())
        elif st == "NOSPC":
            self.reconcile_size(f, 0, src, truncated=True)

    def reconcile_size(self, f, off, src, truncated=False):
        s2, t, sz = self.stat(f.path())
        if s2 != "OK":
            self.bad("stat %s after NOSPC: %s" % (f.path(), s2))
            return
        if truncated:
            f.data.truncate(0)
        old = f.data.size
        # A short write stores what fits: [off, end) -- up to the new size
        # when the file grew, else up to the old size (the clusters it had)
        # when the write began inside it, else nothing.
        ln = src[2] if isinstance(src, tuple) else len(src)
        if sz > old:
            end = sz
        elif off < old:
            end = old
        else:
            end = off
        end = min(end, off + ln)
        if sz < old:
            self.bad("%s shrank from %d to %d after a failed write" % (f.path(), old, sz))
        elif end > off:
            n = end - off
            if isinstance(src, tuple):
                f.data.fill(off, src[1], n)
            else:
                f.data.write(off, src[:n])
            if f.data.size < sz:
                f.data.truncate(sz)
            self.used += sz - old
        self.touched(f.path())
        self.file_sha_check(f)

    def op_pwrite(self):
        f = self.pick("f")
        if f is None:
            return
        r = self.rng
        size = f.data.size
        off = r.randint(0, size)
        if r.random() < 0.25:
            off = size + r.randint(0, 4 * max(self.a.cluster, 4096))
        if self.a.profile == "holes" and r.random() < 0.5:
            off = size + r.randint(0, 50 << 20)
        ln = 1 + r.randint(0, 2 * 4096 if r.random() < 0.6 else 128 * 1024)
        grow = max(0, off + ln - size)
        if off + ln > self.a.maxfile and self.a.profile not in ("big", "holes"):
            return
        if self.used + grow > self.capacity() * (0.97 if self.a.profile == "full" else 0.5):
            return
        spec, src = self.newdata(ln)
        st, _ = self.r.cmd("pwrite %s %d %s" % (esc(f.path()), off, spec))
        self.note("pwrite")
        if self.expect(st, "OK", "pwrite %s %d+%d" % (f.path(), off, ln)):
            self.apply(f.data, off, src)
            self.used += grow
            self.touched(f.path())
        elif st == "NOSPC":
            self.reconcile_size(f, off, src)

    def op_append(self):
        f = self.pick("f")
        if f is None:
            return
        ln = 1 + self.rng.randint(0, 3 * max(self.a.cluster, 4096))
        if f.data.size + ln > self.a.maxfile and self.a.profile not in ("big", "holes"):
            return
        if self.used + ln > self.capacity() * (0.97 if self.a.profile == "full" else 0.5):
            return
        spec, src = self.newdata(ln)
        off = f.data.size
        st, _ = self.r.cmd("append %s %s" % (esc(f.path()), spec))
        self.note("append")
        if self.expect(st, "OK", "append " + f.path()):
            self.apply(f.data, off, src)
            self.used += ln
            self.touched(f.path())
        elif st == "NOSPC":
            self.reconcile_size(f, off, src)

    def op_truncate(self):
        f = self.pick("f")
        if f is None:
            return
        r = self.rng
        size = f.data.size
        if r.random() < 0.5:
            ns = r.randint(0, size)
        else:
            ns = size + r.randint(0, 3 * max(self.a.cluster, 4096))
            if self.a.profile in ("big", "holes") and r.random() < 0.3:
                ns = size + r.randint(0, 1 << 30)
        if ns > size and self.used + ns - size > self.capacity() * (0.97 if self.a.profile == "full" else 0.5):
            return
        st, _ = self.r.cmd("truncate %s %d" % (esc(f.path()), ns))
        self.note("truncate")
        if self.expect(st, "OK", "truncate %s %d" % (f.path(), ns)):
            self.used += ns - size
            f.data.truncate(ns)
            self.touched(f.path())
        elif st == "NOSPC":
            s2, t, sz = self.stat(f.path())
            if sz != size:
                self.bad("truncate NOSPC changed the size of %s: %d -> %s" % (f.path(), size, sz))

    def op_mkdir(self):
        d = self.pick("d")
        lim = 60 if self.a.profile == "deep" else 12
        if d is None or d.depth() > lim:
            return
        nm = self.name(d)
        if nm is None:
            return
        n = Node(nm, "d", d)
        if len(n.path().encode()) > 3800:
            return
        st, _ = self.r.cmd("mkdir " + esc(n.path()))
        self.note("mkdir")
        if self.expect(st, "OK", "mkdir " + n.path()):
            d.kids[fold(nm)] = n
            self.used += self.a.cluster
            self.touched(n.path())
        elif st == "NOSPC":
            s2, _, _ = self.stat(n.path())
            if s2 != "NOT_FOUND":
                self.bad("mkdir %s NOSPC but stat says %s" % (n.path(), s2))

    def op_unlink(self):
        f = self.pick("f")
        if f is None:
            return
        st, _ = self.r.cmd("unlink " + esc(f.path()))
        self.note("unlink")
        if self.expect(st, "OK", "unlink " + f.path()):
            self.touched(f.path())
            del f.parent.kids[fold(f.name)]
            if not self.orphan(f):
                self.used -= f.data.size
            f.parent = None

    def op_rmdir(self):
        d = self.pick("d")
        if d is None or d is self.root:
            return
        st, _ = self.r.cmd("rmdir " + esc(d.path()))
        self.note("rmdir")
        if d.kids:
            self.expect(st, "NOTEMPTY", "rmdir (non-empty) " + d.path())
            return
        if self.expect(st, "OK", "rmdir " + d.path()):
            self.touched(d.path())
            del d.parent.kids[fold(d.name)]
            self.used -= self.a.cluster

    def op_bad_ops(self):
        """Operations that must fail -- and change nothing."""
        r = self.rng
        k = r.randrange(6)
        f = self.pick("f")
        d = self.pick("d")
        self.note("failing-op")
        if k == 0 and d is not None and d is not self.root:
            self.expect(self.r.cmd("unlink " + esc(d.path()))[0], "ISDIR", "unlink dir")
        elif k == 1 and f is not None:
            self.expect(self.r.cmd("rmdir " + esc(f.path()))[0], "NOTDIR", "rmdir file")
        elif k == 2 and f is not None:
            self.expect(self.r.cmd("mkdir " + esc(f.parent.path().rstrip("/") + "/" + f.name.swapcase()))[0],
                        "EXISTS", "mkdir over a file (other case)")
        elif k == 3 and f is not None:
            self.expect(self.r.cmd("mkdir " + esc(f.path() + "/sub"))[0], "NOTDIR", "mkdir under a file")
        elif k == 4 and d is not None:
            self.expect(self.r.cmd("create " + esc(d.path().rstrip("/") + "/bad:name"))[0], "INVALID",
                        "create with ':'")
        elif k == 5 and d is not None:
            nm = self.name(d)
            if nm:
                self.expect(self.r.cmd("unlink " + esc(d.path().rstrip("/") + "/" + nm))[0], "NOT_FOUND",
                            "unlink missing")

    def op_rename(self):
        r = self.rng
        n = self.pick(None)
        if n is None or n is self.root:
            return
        k = r.random()
        old = n.path()
        if k < 0.15:
            # case-only rename in place
            nm = n.name.swapcase() if r.random() < 0.5 else n.name.upper()
            if nm == n.name:
                nm = n.name.lower()
            if nm == n.name or fold(nm) != fold(n.name):
                return
            newp = n.parent.path().rstrip("/") + "/" + nm
            if len(esc(old)) + len(esc(newp)) > 15000:
                return  # the runner's line limit, not the driver's
            st, _ = self.r.cmd("rename %s %s" % (esc(old), esc(newp)))
            self.note("rename-case")
            if self.expect(st, "OK", "rename (case) %s %s" % (old, newp)):
                self.touched(old)
                n.name = nm
            return
        d = self.pick("d") if k < 0.6 else n.parent
        if d is None:
            return
        if d.under(n):
            # a directory into itself or below itself
            newp = d.path().rstrip("/") + "/x"
            st, _ = self.r.cmd("rename %s %s" % (esc(old), esc(newp)))
            self.note("rename-into-self")
            self.expect(st, "INVALID", "rename dir into itself")
            return
        victim = None
        if r.random() < 0.3:
            cands = [c for c in d.kids.values() if c is not n]
            if cands:
                victim = r.choice(cands)
        if victim is not None:
            nm = victim.name if r.random() < 0.6 else victim.name.swapcase()
            if fold(nm) != fold(victim.name):
                nm = victim.name
        else:
            nm = self.name(d)
            if nm is None:
                return
        newp = d.path().rstrip("/") + "/" + nm
        if len(newp.encode()) + self.sublen(n) > 3800:
            return
        if len(esc(old)) + len(esc(newp)) > 15000:
            return  # the runner's line limit, not the driver's
        st, _ = self.r.cmd("rename %s %s" % (esc(old), esc(newp)))
        self.note("rename" + ("-over" if victim else "") + ("-xdir" if d is not n.parent else ""))
        want = "OK"
        if victim is not None:
            if n.kind == "f" and victim.kind == "d":
                want = "ISDIR"
            elif n.kind == "d" and victim.kind == "f":
                want = "NOTDIR"
            elif n.kind == "d" and victim.kids:
                want = "NOTEMPTY"
        if not self.expect(st, want, "rename %s %s" % (old, newp)) or want != "OK":
            if st == "NOSPC":
                s1, _, _ = self.stat(old)
                if s1 != "OK":
                    self.bad("rename NOSPC lost the source %s (%s)" % (old, s1))
            return
        self.touched(old)
        self.touched(newp)
        if victim is not None:
            del d.kids[fold(victim.name)]
            if victim.kind == "f":
                if not self.orphan(victim):
                    self.used -= victim.data.size
            else:
                self.used -= self.a.cluster
            victim.parent = None
        del n.parent.kids[fold(n.name)]
        # a rename onto an existing name takes the NEW spelling
        n.name = nm
        n.parent = d
        d.kids[fold(nm)] = n

    def orphan(self, node):
        """node loses its last name: open handles keep its data."""
        held = False
        for h, (nd, data, p) in list(self.handles.items()):
            if nd is node:
                self.handles[h] = (None, data, p)
                held = True
        return held

    def sublen(self, n):
        m = 0
        st = [(n, 0)]
        while st:
            x, l = st.pop()
            m = max(m, l)
            if x.kind == "d":
                for c in x.kids.values():
                    st.append((c, l + 1 + len(c.name.encode())))
        return m

    def op_handle(self):
        """open, unlink while open, write and read through the handle."""
        r = self.rng
        free = [h for h in range(1, 15) if h not in self.handles]
        if self.handles and (not free or r.random() < 0.4):
            h = r.choice(list(self.handles))
            node, data, p = self.handles[h]
            k = r.random()
            if k < 0.4:
                off = r.randint(0, data.size + 9000)
                ln = r.randint(1, 70000)
                if self.used + max(0, off + ln - data.size) > self.capacity() * 0.5:
                    return
                spec, src = self.newdata(ln)
                st, _ = self.r.cmd("fpwrite %d %d %s" % (h, off, spec))
                self.note("fpwrite")
                if self.expect(st, "OK", "fpwrite %s" % p):
                    self.used += max(0, off + ln - data.size)
                    self.apply(data, off, src)
                    if node is not None:
                        self.touched(node.path())
                else:
                    self.r.cmd("fclose %d" % h)
                    del self.handles[h]
                    if st == "NOSPC" and node is not None:
                        self.reconcile_size(node, off, src)
                    elif node is None:
                        self.recount()
                return
            if k < 0.7 and data is not None:
                off = r.randint(0, data.size)
                ln = r.randint(1, 100000)
                st, out = self.r.cmd("fpread %d %d %d" % (h, off, ln))
                self.note("fpread")
                want = data.read(off, ln)
                got = None
                for l in out:
                    if l.startswith("fread "):
                        got = l.split(" ")
                if st != "OK" or got is None or int(got[1]) != len(want) or \
                        got[2] != hashlib.sha256(want).hexdigest():
                    self.bad("fpread %s %d+%d: %s %s, model %d" %
                             (p, off, ln, st, got, len(want)))
                return
            st, _ = self.r.cmd("fclose %d" % h)
            self.note("fclose")
            self.expect(st, "OK", "fclose " + p)
            if node is None:
                self.used -= data.size  # unlinked: its clusters go now
            del self.handles[h]
            return
        f = self.pick("f")
        if f is None or not free:
            return
        h = r.choice(free)
        st, _ = self.r.cmd("fopen %d %s +" % (h, esc(f.path())))
        self.note("fopen")
        if not self.expect(st, "OK", "fopen " + f.path()):
            return
        self.handles[h] = (f, f.data, f.path())
        if r.random() < 0.5:
            st, _ = self.r.cmd("unlink " + esc(f.path()))
            self.note("unlink-open")
            if self.expect(st, "OK", "unlink (open) " + f.path()):
                self.touched(f.path())
                del f.parent.kids[fold(f.name)]
                self.orphan(f)
                f.parent = None

    def close_handles(self):
        for h in list(self.handles):
            node, data, p = self.handles[h]
            st, _ = self.r.cmd("fclose %d" % h)
            self.expect(st, "OK", "fclose " + p)
            if node is None and data is not None:
                self.used -= data.size
            del self.handles[h]

    def op_fill(self):
        """full profile: a file larger than the free space -- NOSPC, with
        what fit stored (or nothing); then, mostly, gone again."""
        free = max(0, self.capacity() - self.used)
        st, out = self.r.cmd("statfs", record=False)
        for l in out:
            m = re.search(r"bsize=(\d+) .*?blocks=(\d+) bfree=(\d+)", l)
            if m:
                free = int(m.group(3)) * int(m.group(1))
        self.note("fill")
        self.op_create(size=free + self.rng.randint(1, 4 * max(self.a.cluster, 4096)))
        if self.rng.random() < 0.7:
            self.op_unlink_biggest()

    def op_unlink_biggest(self):
        fs = self.nodes("f")
        if not fs:
            return
        f = max(fs, key=lambda n: n.data.size)
        st, _ = self.r.cmd("unlink " + esc(f.path()))
        self.note("unlink")
        if self.expect(st, "OK", "unlink " + f.path()):
            self.touched(f.path())
            del f.parent.kids[fold(f.name)]
            if not self.orphan(f):
                self.used -= f.data.size
            f.parent = None
            self.recount()

    def op_misc(self):
        r = self.rng
        k = r.random()
        if self.a.profile == "full" and k < 0.15:
            return self.op_fill()
        if k < 0.35:
            f = self.pick("f")
            if f is not None:
                self.note("fsync")
                self.expect(self.r.cmd("fsync " + esc(f.path()))[0], "OK", "fsync " + f.path())
        elif k < 0.55:
            self.note("wb")
            self.r.cmd("tick 5000")
            self.r.cmd("wb")
        elif k < 0.65:
            self.note("shrink")
            self.r.cmd("shrink 256 dirty")
        elif k < 0.75:
            self.note("statfs")
            self.expect(self.r.cmd("statfs")[0], "OK", "statfs")
        else:
            self.do_sync()

    def do_sync(self):
        self.note("sync")
        self.expect(self.r.cmd("sync")[0], "OK", "sync")
        self.epoch += 1
        if self.a.snapshots:
            st, out = self.r.cmd("counts", record=True)
            self.snaps.append({"epoch": self.epoch, "manifest": self.manifest_model(),
                               "touch_upto": len(self.touch)})

    # ---- verification -------------------------------------------------------
    def manifest_model(self):
        out = {}
        st = [self.root]
        while st:
            n = st.pop()
            p = n.path()
            if n is not self.root:
                key = mesc(p)
                if n.kind == "d":
                    out[key] = "d"
                else:
                    out[key] = "f %d %s" % (n.data.size, n.data.sha())
            if n.kind == "d":
                st.extend(n.kids.values())
        return out

    def verify(self, remount=True):
        self.close_handles()
        if remount:
            st, _ = self.r.cmd("cycle")
            self.note("cycle")
            if st != "OK":
                self.bad("cycle: " + str(st))
                return
        st, out = self.r.cmd("manifest /", record=False)
        if st != "OK":
            self.bad("manifest: " + str(st))
        got = {}
        for l in out:
            m = re.match(r"([dfl]) (\S+)(?: (\d+) ([0-9a-f]{64}))?", l)
            if not m:
                continue
            if m.group(2) == "/":
                continue
            got[m.group(2)] = "d" if m.group(1) == "d" else \
                "%s %s %s" % (m.group(1), m.group(3), m.group(4))
        want = self.manifest_model()
        diffs = 0
        for k in sorted(set(want) | set(got)):
            if want.get(k) != got.get(k):
                diffs += 1
                if diffs <= 10:
                    self.bad("tree differs at %s: model %s, volume %s" %
                             (k, want.get(k), got.get(k)))
        self.note("verify")
        self.used_check()

    def used_check(self):
        pass

    def recount(self):
        """The space in use, in whole clusters (what a full volume
        is about): files, directories (entry sets), open unlinked files."""
        cs = self.a.cluster
        al = lambda n: -(-n // cs) * cs
        u = 0
        seen = set()
        st = [self.root]
        while st:
            n = st.pop()
            if n.kind == "f":
                u += al(n.data.size)
                seen.add(id(n.data))
            else:
                u += al(max(1, len(n.kids) * 3 * 32 + 32 * 16))
                st.extend(n.kids.values())
        for h, (nd, data, p) in self.handles.items():
            if data is not None and id(data) not in seen:
                u += al(data.size)
                seen.add(id(data))
        self.used = u

    # ---- profiles ------------------------------------------------------------
    def step(self):
        r = self.rng
        prof = self.a.profile
        k = r.random()
        if prof == "bigdir":
            if k < 0.75:
                return self.op_create_in(self.bigdir)
        if k < 0.17:
            self.op_create()
        elif k < 0.22:
            self.op_mkdir()
        elif k < 0.37:
            self.op_pwrite()
        elif k < 0.44:
            self.op_append()
        elif k < 0.52:
            self.op_truncate()
        elif k < 0.61:
            self.op_unlink()
        elif k < 0.64:
            self.op_rmdir()
        elif k < 0.77:
            self.op_rename()
        elif k < 0.83:
            self.op_handle()
        elif k < 0.86:
            self.op_bad_ops()
        elif k < 0.88:
            self.op_overwrite_existing_case()
        else:
            self.op_misc()

    def op_create_in(self, d):
        nm = self.name(d)
        if nm is None:
            return
        n = Node(nm, "f", d)
        size = self.rng.randint(0, 3000)
        spec, src = self.newdata(size)
        st, _ = self.r.cmd("write %s %s" % (esc(n.path()), spec))
        self.note("create-bigdir")
        if self.expect(st, "OK", "write " + n.path()):
            d.kids[fold(nm)] = n
            self.apply(n.data, 0, src)
            self.used += size
            self.touched(n.path())
        elif st == "NOSPC":
            self.created_short(d, nm, n, src)

    def load(self):
        """Take what is on the volume already into the model."""
        st, out = self.r.cmd("manifest /", record=False)
        ents = []
        for l in out:
            m = re.match(r"([dfl]) (\S+)(?: (\d+) ([0-9a-f]{64}))?", l)
            if m and m.group(2) != "/":
                ents.append((m.group(1), unescape(m.group(2)), m.group(3), m.group(4)))
        ents.sort(key=lambda e: e[1].count("/"))
        for kind, path, size, sha in ents:
            parts = path.strip("/").split("/")
            d = self.root
            for p in parts[:-1]:
                d = d.kids[fold(p)]
            n = Node(parts[-1], "d" if kind == "d" else "f", d)
            d.kids[fold(parts[-1])] = n
            if kind == "f":
                st, hx = self.r.cmd("hexdump " + esc(path), record=False)
                buf = bytes.fromhex("".join(x.strip() for x in hx if re.fullmatch(r"[0-9a-f]+", x.strip())))
                n.data.write(0, buf)
                n.data.truncate(int(size))
                if n.data.sha() != sha:
                    raise Finding("could not load %s into the model" % path)
        self.recount()

    def setup(self):
        if self.a.profile == "bigdir":
            self.bigdir = Node("big", "d", self.root)
            self.expect(self.r.cmd("mkdir /big")[0], "OK", "mkdir /big")
            self.root.kids[fold("big")] = self.bigdir
        if self.a.profile == "big":
            # > 4 GB, sparse-extended, then written around the 4 GB marks
            n = Node("huge", "f", self.root)
            self.root.kids[fold("huge")] = n
            spec, src = self.newdata(100000)
            self.expect(self.r.cmd("write /huge %s" % spec)[0], "OK", "write /huge")
            self.apply(n.data, 0, src)
            for size in (4294967296 + 123, 4500000000):
                self.expect(self.r.cmd("truncate /huge %d" % size)[0], "OK", "truncate /huge")
                n.data.truncate(size)
            self.used += 4500000000
            for off in (4294967296 - 5000, 4294967296 * 1 + 7):
                spec, src = self.newdata(20000)
                self.expect(self.r.cmd("pwrite /huge %d %s" % (off, spec))[0], "OK", "pwrite /huge")
                self.apply(n.data, off, src)
            self.verify()

    def run(self):
        a = self.a
        args = ["--sector", str(a.sector)]
        if a.extra:
            args += a.extra.split()
        self.r = Runner(a.bin, a.image, args, os.path.join(a.tmpdir, "stderr.log"),
                        timeout=a.timeout)
        if a.record:
            self.r.record = []
        if a.trace:
            self.r.trace = open(a.trace, "w")
        try:
            st, _ = self.r.cmd("mount rw " + (a.mopts or ""))
            if st != "OK":
                raise Finding("mount: %s" % st)
            st, out = self.r.cmd("statfs", record=False)
            for l in out:
                m = re.search(r"bsize=(\d+) .*?blocks=(\d+) bfree=(\d+)", l)
                if m:
                    a.capacity = int(m.group(3)) * int(m.group(1))
            if not getattr(a, "capacity", 0):
                a.capacity = 16 << 20
            if a.profile != "full":
                # what the model holds in memory (and the image on disk)
                # stays bounded on big volumes
                a.capacity = min(a.capacity, 512 << 20)
            self.load()
            self.setup()
            for i in range(a.ops):
                if i % 50 == 0:
                    self.recount()
                self.step()
                if a.verify_every and (i + 1) % a.verify_every == 0:
                    self.verify()
                if self.r.fails:
                    for f in self.r.fails:
                        self.bad("runner: " + f)
                    self.r.fails = []
            self.verify()
            self.close_handles()
            st, _ = self.r.cmd("umount")
            if st != "OK":
                self.bad("umount: %s" % st)
            # read-only remount: the tree once more, nothing written
            st, _ = self.r.cmd("mount ro", record=False)
            if st == "OK":
                self.r.cmd("mark", record=False)
                self.verify(remount=False)
                self.r.cmd("nowrites", record=False)
                self.r.cmd("umount", record=False)
            else:
                self.bad("read-only mount: %s" % st)
        except Finding as e:
            self.bad(str(e))
        rc = self.r.close()
        for f in self.r.fails:
            self.bad("runner: " + f)
        if rc not in (0, 1):
            self.bad("runner exit status %d" % rc)
        if a.record:
            with open(a.record, "w") as f:
                f.write("\n".join(self.r.record) + "\n")
        if a.snapshots:
            with open(a.snapshots, "w") as f:
                json.dump({"snaps": self.snaps, "touch": self.touch}, f)
        print("audit: seed %d profile %s ops %d sent %d nospc %d findings %d stats %s" %
              (a.seed, a.profile, a.ops, self.r.sent, self.nospc_seen, len(self.findings),
               json.dumps(self.stats, sort_keys=True)), flush=True)
        return 1 if self.findings else 0


class Journal:
    """The write journal of a run (host/test-audit-exfat.sh's preload
    shim), mapped, not read: entries [('W', off, pos, len) | ('F',)] in
    order, the data of a write at `pos' of the mapping."""

    def __init__(self, path):
        import mmap
        import struct
        self.f = open(path, "rb")
        size = os.fstat(self.f.fileno()).st_size
        self.m = mmap.mmap(self.f.fileno(), size, access=mmap.ACCESS_READ) if size else b""
        self.e = []
        i = 0
        while i + 13 <= size:
            t = self.m[i:i + 1]
            off, ln = struct.unpack_from("<QI", self.m, i + 1)
            i += 13
            if t == b"W":
                self.e.append(("W", off, i, ln))
            else:
                self.e.append(("F",))
            i += ln

    def data(self, pos, ln):
        return self.m[pos:pos + ln]


def unescape(key):
    raw = re.sub(r"\\x([0-9a-f]{2})", lambda m: chr(int(m.group(1), 16)), key)
    return raw.encode("latin-1").decode("utf-8")


class Sweep:
    """Power cuts over a recorded workload's write journal.

    prefix:  the first N writes reach the device, nothing after (every N)
    reorder: everything before the last flush preceding write N, and of
             the writes between that flush and N a random subset at
             sector granularity (64 KB pieces of requests larger than
             that), each sector holding any one of the versions written
             to it or its old content (a volatile write cache that loses
             power: the device keeps no order between flushes, and a
             request may be torn at sector boundaries)

    The journal is mapped, not read, and the durable part of the crash
    images is applied once, in order, as the cut point advances: memory
    stays small whatever the journal's size.

    Every crash image: fsck.exfat -n clean, test-exfat-check.py clean but
    for lost clusters / chain tails / VolumeDirty (the documented
    window), a read-only mount that reads everything, every file the
    workload did not touch after the last completed sync unchanged, and a
    read-write mount (force: the volume is dirty) that can still write
    (volumes over 1 GB: on every 10th crash image)."""

    def __init__(self, a):
        self.a = a
        self.jn = Journal(a.journal)
        self.j = self.jn.e
        self.writes = [k for k, e in enumerate(self.j) if e[0] == "W"]
        self.flushes = [k for k, e in enumerate(self.j) if e[0] == "F"]
        with open(a.snapshots) as f:
            s = json.load(f)
        self.snaps = s["snaps"]
        self.touch = s["touch"]
        self.syncw = [int(x) for x in open(a.writes_at).read().split()]
        self.img = open(a.image, "r+b")
        self.fails = 0
        self.kept = 0
        self.stats = {"cuts": 0, "lost": 0, "tail": 0, "max_lost": 0, "dirty": 0}
        self.ss = a.sector
        # journal entries [0, self.done) are on the image for good: the
        # crash images of increasing cut points share their durable part,
        # so it is applied once, never undone (every check reads the image
        # or mounts it copy-on-write)
        self.done = 0

    def put(self, off, pos, ln):
        self.img.seek(off)
        self.img.write(self.jn.data(pos, ln))

    def advance(self, upto):
        """Apply the journal's writes [self.done, upto) for good."""
        for e in self.j[self.done:upto]:
            if e[0] == "W":
                self.put(e[1], e[2], e[3])
        self.done = max(self.done, upto)
        self.img.flush()

    def apply(self, pieces):
        """Write [(off, pos, len)] of the journal, returning the undo list:
        the old bytes of exactly those ranges, kept in a file (in-flight
        data can be large)."""
        undo = []
        u = open(os.path.join(self.a.tmpdir, "undo.bin"), "w+b")
        for off, pos, ln in pieces:
            self.img.seek(off)
            old = self.img.read(ln)
            old += bytes(ln - len(old))
            undo.append((off, u.tell(), ln))
            u.write(old)
            self.put(off, pos, ln)
        self.img.flush()
        u.flush()
        return (u, undo)

    def restore(self, undo):
        if not undo:
            return
        u, lst = undo
        for off, upos, ln in reversed(lst):
            u.seek(upos)
            self.img.seek(off)
            self.img.write(u.read(ln))
        self.img.flush()
        u.close()

    # a piece of an in-flight write: sectors of small requests, 64 KB of
    # large ones (cluster zeroing, data) -- each piece reaches the device
    # or not, independently, later pieces over earlier ones
    PIECE_BIG = 64 << 10

    def crash(self, n, mode, rng):
        """Make the image the crash image after write #n (0-based count of
        writes that may have reached the device); returns (undo, durable
        write count)."""
        end = self.writes[n] if n < len(self.writes) else len(self.j)
        if mode == "prefix":
            self.advance(end)
            return [], n
        # everything before the last flush preceding write #n is durable
        i = bisect.bisect_left(self.flushes, end)
        last_f = self.flushes[i - 1] if i else 0
        self.advance(last_f)
        durable = bisect.bisect_left(self.writes, last_f)
        pieces = []
        for e in self.j[last_f:end]:
            if e[0] != "W":
                continue
            unit = self.ss if e[3] <= self.PIECE_BIG else self.PIECE_BIG
            for k in range(0, e[3], unit):
                pieces.append((e[1] + k, e[2] + k, min(unit, e[3] - k)))
        style = rng.random()
        out = []
        for p in pieces:
            if style < 0.2:
                keep = True  # everything stored
            elif style < 0.4:
                keep = rng.random() < 0.5
            else:
                keep = rng.random() < 0.7
            if keep:
                out.append(p)
        return self.apply(out), durable

    def check(self, label, durable):
        a = self.a
        bad = []
        f = subprocess.run(["fsck.exfat", "-n", a.image], capture_output=True, text=True)
        if f.returncode != 0 or "clean" not in f.stdout:
            errs = [l for l in (f.stdout + f.stderr).splitlines() if "ERROR" in l]
            # the one documented window: a FAT chain that is longer than
            # its DataLength (the link into a new cluster is stored before
            # the entry set that grows the length); fsck trims the tail,
            # no byte of any file is lost
            if errs and all("more clusters are allocated. truncate to" in l for l in errs):
                self.stats["window"] = self.stats.get("window", 0) + 1
            else:
                bad.append("fsck.exfat: " + (f.stdout + f.stderr).strip().replace("\n", " | ")[:400])
        c = subprocess.run([sys.executable, "-I", a.checker, a.image, "--allow-leaks", "--allow-dirty"],
                           capture_output=True, text=True)
        if c.returncode != 0:
            bad.append("check: " + c.stdout.strip().replace("\n", " | ")[:600])
        m = re.search(r"(\d+) lost clusters, (\d+) chain-tail", c.stdout)
        if m:
            lost, tail = int(m.group(1)), int(m.group(2))
            self.stats["lost"] += lost
            self.stats["tail"] += tail
            self.stats["max_lost"] = max(self.stats["max_lost"], lost + tail)
        if "VolumeDirty" in c.stdout:
            self.stats["dirty"] += 1
        bad += self.mount_check(durable)
        self.stats["cuts"] += 1
        if bad:
            self.fails += 1
            for b in bad:
                print("FINDING: cut %s: %s" % (label, b), flush=True)
            if a.keep and self.kept < 3:
                self.kept += 1
                subprocess.run(["cp", "--sparse=always", a.image,
                                os.path.join(a.tmpdir, "cut-%s.img" % label.replace(" ", "_"))])

    def mount_check(self, durable):
        a = self.a
        bad = []
        # the read-write part (a forced mount that writes) costs most on
        # big volumes with big clusters: there on every 10th image
        self.nmc = getattr(self, "nmc", 0) + 1
        rw = os.path.getsize(a.image) <= (1 << 30) or self.nmc % 10 == 1
        # the last snapshot all of whose writes were durable
        k = -1
        for i, w in enumerate(self.syncw):
            if i < len(self.snaps) and w <= durable:
                k = i
        r = Runner(a.bin, a.image, ["--cow", "--sector", str(a.sector)],
                   os.path.join(a.tmpdir, "cut-stderr.log"), timeout=600)
        try:
            st, _ = r.cmd("mount ro")
            if st != "OK":
                bad.append("mount ro: %s" % st)
                raise Finding("no mount")
            st, out = r.cmd("manifest /")
            if st != "OK":
                bad.append("manifest: %s" % st)
            got = {}
            for l in out:
                m = re.match(r"([dfl]) (\S+)(?: (\d+) ([0-9a-f]{64}))?", l)
                if m and m.group(2) != "/":
                    got[m.group(2)] = "d" if m.group(1) == "d" else \
                        "%s %s %s" % (m.group(1), m.group(3), m.group(4))
            if k >= 0:
                snap = self.snaps[k]
                later = set(p for (e, p) in self.touch[snap["touch_upto"]:])
                for key, val in snap["manifest"].items():
                    fk = fold(unescape(key))
                    if any(fk == p or fk.startswith(p + "/") or p.startswith(fk + "/")
                           for p in later):
                        continue
                    if got.get(key) != val:
                        bad.append("data loss: %s is %s, was %s at sync %d" %
                                   (key, got.get(key), val, k + 1))
            r.cmd("umount")
            if not rw:
                raise Finding("no rw check")
            st, _ = r.cmd("mount rw force")
            if st != "OK":
                bad.append("mount rw force: %s" % st)
            else:
                for c in ("mkdir /after_cut_dir", "write /after_cut_dir/f pat:9:70000",
                          "write /after_cut_file pat:8:5000", "umount", "mount ro",
                          "check /after_cut_dir/f pat:9:70000", "umount"):
                    st, _ = r.cmd(c)
                    if st == "NOSPC" and c.startswith(("mkdir", "write")):
                        # a full volume (the clusters a cut leaves lost
                        # count too): what fit must be all right
                        r.cmd("umount")
                        st, _ = r.cmd("mount ro")
                        if st == "OK":
                            st, _ = r.cmd("manifest /")
                        if st != "OK":
                            bad.append("after the cut (full): %s" % st)
                        r.cmd("umount")
                        break
                    if st != "OK":
                        bad.append("after the cut: %s: %s" % (c, st))
                        break
        except Finding as e:
            if str(e) not in ("no mount", "no rw check"):
                bad.append(str(e))
        rc = r.close()
        for f in r.fails:
            bad.append("runner: " + f)
        if rc not in (0, 1):
            bad.append("runner exit %d" % rc)
        return bad

    def run(self):
        a = self.a
        rng = random.Random(a.seed)
        total = len(self.writes)
        cuts = list(range(0, total + 1, a.step))
        print("sweep: %d writes, %d flushes, mode %s, %d cut points%s" %
              (total, len(self.j) - total, a.mode, len(cuts),
               (" x %d samples" % a.samples) if a.mode == "reorder" else ""), flush=True)
        for n in cuts:
            for s in range(a.samples if a.mode == "reorder" else 1):
                undo, durable = self.crash(n, a.mode, rng)
                try:
                    self.check("%s-%d-%d" % (a.mode, n, s), durable)
                finally:
                    self.restore(undo)
                if self.fails > 20:
                    break
        print("sweep: %s" % json.dumps(self.stats, sort_keys=True))
        print("sweep: %d failing crash image(s)" % self.fails, flush=True)
        return 1 if self.fails else 0


def boot_checksum(region, ss):
    s = 0
    for i, b in enumerate(region[:ss * 11]):
        if i in (106, 107, 112):
            continue
        s = (((s & 1) << 31) | (s >> 1)) + b & 0xFFFFFFFF
    return s


def to4k(image):
    """Rewrite a 512-byte-sector exFAT image (made with a FAT offset and a
    cluster heap on 4 KB boundaries, clusters >= 4 KB) as a volume with
    4096-byte sectors: the same bytes everywhere, the boot regions rebuilt
    with 4 KB sectors (main at 0, backup at 48 KB)."""
    import struct
    with open(image, "r+b") as f:
        b = bytearray(f.read(512))
        if b[108] != 9:
            sys.exit("to4k: not a 512-byte-sector volume")
        fat_off, fat_len, heap, ccount = struct.unpack_from("<IIII", b, 80)
        vol_len = struct.unpack_from("<Q", b, 72)[0]
        spc = b[109]
        if fat_off % 8 or heap % 8 or vol_len % 8 or spc < 3 or fat_off < 24 * 8:
            sys.exit("to4k: layout not on 4 KB boundaries (fat %d heap %d)" % (fat_off, heap))
        ss = 4096
        struct.pack_into("<Q", b, 72, vol_len // 8)
        struct.pack_into("<IIII", b, 80, fat_off // 8, (fat_len + 7) // 8, heap // 8, ccount)
        if fat_off // 8 + (fat_len + 7) // 8 * b[110] > heap // 8:
            sys.exit("to4k: FAT would overlap the heap")
        b[108] = 12
        b[109] = spc - 3
        region = bytearray(ss * 12)
        region[0:512] = b
        for k in range(1, 9):
            struct.pack_into("<I", region, k * ss + ss - 4, 0xAA550000)
        cs = boot_checksum(region, ss)
        for i in range(0, ss, 4):
            struct.pack_into("<I", region, 11 * ss + i, cs)
        f.seek(0)
        f.write(region)
        f.write(region)


def main():
    import argparse
    if len(sys.argv) > 2 and sys.argv[1] == "to4k":
        to4k(sys.argv[2])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "sweep":
        ap = argparse.ArgumentParser()
        ap.add_argument("mode0")
        ap.add_argument("bin")
        ap.add_argument("image")
        ap.add_argument("journal")
        ap.add_argument("snapshots")
        ap.add_argument("writes_at")
        ap.add_argument("--mode", default="prefix")
        ap.add_argument("--step", type=int, default=1)
        ap.add_argument("--samples", type=int, default=3)
        ap.add_argument("--seed", type=int, default=1)
        ap.add_argument("--sector", type=int, default=512)
        ap.add_argument("--tmpdir", default="/tmp")
        ap.add_argument("--keep", action="store_true")
        ap.add_argument("--checker", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          "test-exfat-check.py"))
        a = ap.parse_args()
        sys.exit(Sweep(a).run())
    ap = argparse.ArgumentParser()
    ap.add_argument("mode")
    ap.add_argument("bin")
    ap.add_argument("image")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--verify-every", type=int, default=500)
    ap.add_argument("--profile", default="mixed")
    ap.add_argument("--sector", type=int, default=512)
    ap.add_argument("--cluster", type=int, default=4096)
    ap.add_argument("--maxfile", type=int, default=2 << 20)
    ap.add_argument("--mopts", default="")
    ap.add_argument("--extra", default="")
    ap.add_argument("--record")
    ap.add_argument("--snapshots")
    ap.add_argument("--trace")
    ap.add_argument("--tmpdir", default="/tmp")
    ap.add_argument("--timeout", type=int, default=7200)
    a = ap.parse_args()
    a.capacity = 0
    sys.exit(Workload(a).run())


if __name__ == "__main__":
    main()
