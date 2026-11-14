#!/usr/bin/env python3
# likeos-install host test: the terminal on a pseudo-terminal.
#
#   tty-test.py reset PROG
#       --reset-tty puts a terminal left raw (no line mode, no echo, no
#       signals, VMIN 0, no CR/NL mapping, no output processing) back into
#       line mode with echo; a standard input that is no terminal is
#       refused with exit status 4.
#   tty-test.py sigterm PROG INPUT ARGS...
#       PROG ARGS... on a pseudo-terminal, the lines of INPUT typed until
#       the first password prompt (echo off); SIGTERM then ends it, and the
#       terminal has its echo back.
#
# One line per check: "PASS ..." or "FAIL ...".  Exit status 0 when every
# check passed.
#
# Copyright (C) 2026 The LikeOS Project

import os
import pty
import select
import signal
import subprocess
import sys
import termios
import time

failures = 0


def report(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures += 1


def cc_value(c):
    # python's termios gives VMIN and VTIME as numbers only outside line
    # mode, as one-byte strings in it
    return c if isinstance(c, int) else c[0]


def make_raw(fd):
    a = termios.tcgetattr(fd)
    a[0] &= ~termios.ICRNL
    a[1] &= ~termios.OPOST
    a[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE |
              termios.ECHOK | termios.ISIG)
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)


def test_reset(prog):
    master, slave = pty.openpty()
    try:
        make_raw(slave)
        rc = subprocess.call([prog, "--reset-tty"], stdin=slave,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=30)
        a = termios.tcgetattr(slave)
        report(rc == 0, "--reset-tty: exit 0 on a terminal")
        report(bool(a[3] & termios.ICANON) and bool(a[3] & termios.ECHO) and
               bool(a[3] & termios.ISIG) and bool(a[3] & termios.ECHOE) and
               bool(a[3] & termios.ECHOK),
               "--reset-tty: line mode, echo and signals back")
        report(cc_value(a[6][termios.VMIN]) == 1 and
               cc_value(a[6][termios.VTIME]) == 0,
               "--reset-tty: VMIN 1, VTIME 0")
        report(bool(a[0] & termios.ICRNL), "--reset-tty: CR read as NL")
        report(bool(a[1] & termios.OPOST) and bool(a[1] & termios.ONLCR),
               "--reset-tty: NL written as CR NL")
    finally:
        os.close(master)
        os.close(slave)
    with open(os.devnull, "rb") as null:
        rc = subprocess.call([prog, "--reset-tty"], stdin=null,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=30)
    report(rc == 4, "--reset-tty: not a terminal, exit 4 (got %d)" % rc)


def test_sigterm(prog, input_path, args):
    with open(input_path, "rb") as f:
        lines = f.read().split(b"\n")
    pid, master = pty.fork()
    if pid == 0:
        env = dict(os.environ)
        env.pop("LIKEOS_INSTALL_PIPE", None)
        try:
            os.execve(prog, [prog] + args, env)
        finally:
            os._exit(127)
    out = b""
    sent = 0
    quiet = False
    deadline = time.time() + 120
    try:
        while time.time() < deadline:
            # The first password prompt switches the echo off (nothing
            # before it does).
            if not (termios.tcgetattr(master)[3] & termios.ECHO):
                quiet = True
                break
            r, _, _ = select.select([master], [], [], 0.1)
            if r:
                try:
                    data = os.read(master, 65536)
                except OSError:
                    break
                if not data:
                    break
                out += data
                continue
            # The program waits for input: the next line.
            if sent < len(lines):
                os.write(master, lines[sent] + b"\n")
                sent += 1
        report(quiet, "SIGTERM: the password prompt reached, echo off")
        os.kill(pid, signal.SIGTERM)
        status = None
        end = time.time() + 30
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.1)
            if r:
                try:
                    os.read(master, 65536)
                except OSError:
                    pass
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
                break
        if status is None:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        report(status is not None and os.WIFSIGNALED(status) and
               os.WTERMSIG(status) == signal.SIGTERM,
               "SIGTERM: the program ended by the signal")
        a = termios.tcgetattr(master)
        report(bool(a[3] & termios.ECHO) and bool(a[3] & termios.ICANON),
               "SIGTERM: the terminal has its echo and line mode back")
    finally:
        os.close(master)
    if failures:
        sys.stdout.write(out[-2000:].decode("utf-8", "replace") + "\n")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "reset":
        test_reset(sys.argv[2])
    elif len(sys.argv) >= 4 and sys.argv[1] == "sigterm":
        test_sigterm(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        sys.stderr.write("usage: tty-test.py reset PROG | "
                         "sigterm PROG INPUT ARGS...\n")
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
