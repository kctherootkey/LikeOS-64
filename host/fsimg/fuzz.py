#!/usr/bin/env python3
# Deterministic metadata fuzzer for the filesystem harness.
#
#   fuzz.py --bin TEST_FS_BINARY --fstype FSTYPE --image BASE
#           [--seed S] [--iters N] [--timeout SECS] [--sector N]
#           [--rw] [--cmds 'CMD; CMD'] [--regress DIR] [--max-muts K]
#           [--only-iter I]
#
# For every iteration the image's metadata regions (fsparse.py: boot
# sectors, MFT records, INDX blocks, $LogFile restart pages, runlists,
# exFAT boot region / FAT / bitmap / up-case / directory entry sets,
# ISO descriptors / path table / directory records / SUSP continuation
# areas / zisofs headers, ...) get 1..K seeded mutations: bit flips,
# boundary bytes, 16/32/64-bit fields set to edge values, zeroed or
# duplicated stretches.  The mutations go to the runner as a patch file
# over a copy-on-write device (--cow --patch): the base image is never
# modified and nothing is copied.  The runner mounts, walks the whole tree,
# reads every file (and with --rw also writes, then walks again) and
# unmounts.
#
# I/O errors, refused mounts and read failures are the expected answers
# to a corrupt image.  A finding is: a crash (ASan, UBSan, BUG, abort,
# deadlock or livelock report), a write trap, a time-out (hang), a leak or
# a lock left held after the unmount, an allocation larger than the
# harness allows (uncapped size from disk data).  Findings are printed and
# saved into --regress (default host/fsimg/regress): the base image
# (xz-compressed, once per image), the patch and the command line, which
# regress/replay.sh runs again.  Same seed, same mutations.
#
# Copyright (C) 2026 The LikeOS Project
import argparse
import hashlib
import lzma
import os
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fsparse  # noqa: E402

M64 = (1 << 64) - 1


class Rng:
    def __init__(self, *seeds):
        h = hashlib.sha256(repr(seeds).encode()).digest()
        self.s = int.from_bytes(h[:8], "little") or 1

    def next(self):
        s = self.s
        s ^= s >> 12
        s ^= (s << 25) & M64
        s ^= s >> 27
        self.s = s
        return (s * 2685821657736338717) & M64

    def below(self, n):
        return self.next() % n if n > 0 else 0

    def pick(self, seq):
        return seq[self.below(len(seq))]


EDGE8 = [0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF]
EDGE16 = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x0200, 0x1000, 0xFFFE]
EDGE32 = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x1000, 0x10000, 0xFFFFFFF0, 24, 0x400]
EDGE64 = [0, 1, (1 << 63) - 1, 1 << 63, M64, 1 << 32, 0x100000000 - 1, 1 << 40]


def mutate(rng, regions, img_size, base_read, kmax):
    """List of (offset, bytes)."""
    muts = []
    for _ in range(1 + rng.below(kmax)):
        o, l, lab = rng.pick(regions)
        if l <= 0 or o >= img_size:
            continue
        l = min(l, img_size - o)
        kind = rng.below(100)
        at = o + rng.below(l)
        if kind < 25:  # bit flip
            b = base_read(at, 1)
            if not b:
                continue
            muts.append((at, bytes([b[0] ^ (1 << rng.below(8))])))
        elif kind < 40:
            muts.append((at, bytes([rng.pick(EDGE8)])))
        elif kind < 55:
            at -= at % 2
            muts.append((at, struct.pack("<H", rng.pick(EDGE16 + [rng.below(l + 64)]) & 0xFFFF)))
        elif kind < 75:
            at -= at % 4
            v = rng.pick(EDGE32 + [rng.below(l + 4096), img_size // 512 + rng.below(16)])
            muts.append((at, struct.pack("<I", v & 0xFFFFFFFF)))
        elif kind < 85:
            at -= at % 8
            muts.append((at, struct.pack("<Q", rng.pick(EDGE64) & M64)))
        elif kind < 93:  # zero a stretch
            n = 1 + rng.below(min(64, o + l - at))
            muts.append((at, b"\0" * n))
        else:  # copy a stretch from elsewhere in the region
            n = 1 + rng.below(min(32, l))
            src = o + rng.below(max(1, l - n))
            data = base_read(src, n)
            muts.append((at, data))
    return muts


OUT_KEEP = 4 << 20


def bounded_output(f):
    """The first and the last OUT_KEEP bytes of a run's output file."""
    size = f.seek(0, 2)
    f.seek(0)
    if size <= 2 * OUT_KEEP:
        data = f.read()
    else:
        head = f.read(OUT_KEEP)
        f.seek(size - OUT_KEEP)
        data = head + b"\n[... %d bytes of output skipped ...]\n" % (size - 2 * OUT_KEEP) + f.read(OUT_KEEP)
    return data.decode("utf-8", "replace")


def write_patch(path, muts):
    with open(path, "w") as f:
        for o, b in muts:
            f.write("%d %s\n" % (o, b.hex()))


FINDING_PATTERNS = [
    "unmount left", "still held after", "failed mount left", "WRITE TRAP",
    "larger than the harness limit", "DEADLOCK", "livelock", "FATAL",
    "BUG:", "runtime error", "AddressSanitizer", "beyond device end",
    "dirent64", "readdir returned",
    "readdir never ends", "malformed",
]


def classify(rc, out):
    """None when the run is an acceptable reaction, else a reason."""
    if rc == 124 or rc == 137:
        return "timeout (hang?)"
    if rc not in (0, 1):
        for p in ("AddressSanitizer", "runtime error", "WRITE TRAP", "DEADLOCK",
                  "livelock", "BUG:", "FATAL"):
            if p in out:
                return "crash: " + p
        return "crash: exit status %d" % rc
    for p in FINDING_PATTERNS:
        if p in out:
            return "finding: " + p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--fstype", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--sector", type=int, default=0)
    ap.add_argument("--rw", action="store_true")
    ap.add_argument("--cmds", default=None)
    ap.add_argument("--regress", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "regress"))
    ap.add_argument("--max-muts", type=int, default=4)
    # runner page-cache limit (4 KiB pages): small enough that a run fits a
    # memory-capped box even with AddressSanitizer
    ap.add_argument("--pages", type=int, default=32768)
    ap.add_argument("--only-iter", type=int, default=-1)
    ap.add_argument("--keep-going", action="store_true")
    a = ap.parse_args()

    img = fsparse.Img(a.image)
    regions = fsparse.regions(a.fstype, a.image)
    if not regions:
        sys.exit("fuzz: no metadata regions found")
    sector = a.sector or (2048 if a.fstype in ("iso", "isofs") else 512)
    if a.cmds:
        cmds = a.cmds
    elif a.rw:
        cmds = ("mount; readall; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; "
                "?append /fz_dir/f pat:8:5000; ?rename /fz_dir/f /fz_dir/g; "
                "?unlink /fz_dir/g; ?rmdir /fz_dir; readall; umount; ?mount ro; "
                "readall; umount")
    else:
        cmds = "mount ro; readall; umount"
    # every command may fail on a corrupt image: '?' them all
    cmds = "; ".join(("?" + c.strip()) if c.strip() and not c.strip().startswith(("?", "=")) else c.strip()
                     for c in cmds.split(";"))
    h = hashlib.sha256()
    with open(a.image, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    base_sha = h.hexdigest()[:12]
    tmp = tempfile.mkdtemp(prefix="fuzz.")
    findings = 0
    past_end = 0
    print("fuzz: %s %s seed %d iters %d, %d regions, base %s" %
          (a.fstype, a.image, a.seed, a.iters, len(regions), base_sha))
    sys.stdout.flush()
    iters = [a.only_iter] if a.only_iter >= 0 else range(a.iters)
    for i in iters:
        rng = Rng(a.seed, i, base_sha)
        muts = mutate(rng, regions, img.size, img.read, a.max_muts)
        patch = os.path.join(tmp, "p")
        write_patch(patch, muts)
        argv = [a.bin, a.image, "--cow", "--patch", patch, "--allow-warn",
                "--sector", str(sector), "--kalloc-max", str(64 << 20),
                "--pages", str(a.pages), "-c", cmds]
        try:
            # the run's output goes to a file, never into this process
            # whole: a runner that loops printing can write gigabytes
            with tempfile.TemporaryFile(dir=tmp) as of:
                r = subprocess.run(["timeout", "-k", "5", str(a.timeout)] + argv,
                                   stdout=of, stderr=subprocess.STDOUT)
                rc, out = r.returncode, bounded_output(of)
        except Exception as e:  # noqa: BLE001
            rc, out = 255, str(e)
        why = classify(rc, out)
        if "read beyond the end of the device" in out:
            past_end += 1
        if why is None:
            if (i + 1) % 25 == 0:
                print("fuzz: %d/%d ok" % (i + 1, a.iters))
                sys.stdout.flush()
            continue
        findings += 1
        name = "%s-%s-s%d-i%d" % (a.fstype, base_sha, a.seed, i)
        print("FINDING %s: %s" % (name, why))
        os.makedirs(a.regress, exist_ok=True)
        basexz = os.path.join(a.regress, "base-%s-%s.img.xz" % (a.fstype, base_sha))
        if not os.path.exists(basexz) and img.size <= (64 << 20):
            with open(a.image, "rb") as f, lzma.open(basexz, "wb", preset=6) as z:
                for blk in iter(lambda: f.read(1 << 20), b""):
                    z.write(blk)
        write_patch(os.path.join(a.regress, name + ".patch"), muts)
        with open(os.path.join(a.regress, name + ".cmd"), "w") as f:
            f.write("fstype=%s\nbase=%s\nsector=%d\nwhy=%s\ncmds=%s\n" %
                    (a.fstype, os.path.basename(basexz), sector, why, cmds))
        with open(os.path.join(a.regress, name + ".log"), "w") as f:
            f.write(out[-20000:])
        tail = []
        for l in out.split("\n"):
            if any(p in l for p in FINDING_PATTERNS + ["#0", "#1", "#2", "#3"]) and l not in tail:
                tail.append(l)
        for l in tail[:12]:
            print("    " + l)
        sys.stdout.flush()
        if not a.keep_going and findings >= 20:
            print("fuzz: stopping after 20 findings")
            break
    os.unlink(os.path.join(tmp, "p")) if os.path.exists(os.path.join(tmp, "p")) else None
    os.rmdir(tmp)
    print("fuzz: done, %d finding(s); %d run(s) read past the end of the device "
          "(refused by the device: not a finding)" % (findings, past_end))
    sys.exit(1 if findings else 0)


if __name__ == "__main__":
    main()
