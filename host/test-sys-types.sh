#!/bin/sh
# Host check of what <sys/types.h> alone must declare: the POSIX thread types
# (a program that includes only that header and X11's may declare a
# pthread_mutex_t), sigset_t, and the usual id/size types -- compiled with
# the port's own cross compiler against the libc's live headers.  Run from
# the repository root.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-sys-types-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
cat >"$TMP/types.c" <<'SRC'
#include <sys/types.h>
static pthread_t t;
static pthread_attr_t ta;
static pthread_mutex_t m;
static pthread_mutexattr_t ma;
static pthread_cond_t c;
static pthread_condattr_t ca;
static pthread_rwlock_t rw;
static pthread_rwlockattr_t rwa;
static pthread_spinlock_t sp;
static pthread_barrier_t b;
static pthread_barrierattr_t ba;
static pthread_key_t k;
static pthread_once_t o;
static sigset_t ss;
static pid_t p; static uid_t u; static gid_t g; static off_t off; static ssize_t s;
static size_t z; static time_t tm; static dev_t d; static ino_t i; static mode_t md;
int main(void) { return (int)(sizeof(t) + sizeof(ta) + sizeof(m) + sizeof(ma) + sizeof(c) + sizeof(ca) + sizeof(rw) + sizeof(rwa) + sizeof(sp) + sizeof(b) + sizeof(ba) + sizeof(k) + sizeof(o) + sizeof(ss) + sizeof(p) + sizeof(u) + sizeof(g) + sizeof(off) + sizeof(s) + sizeof(z) + sizeof(tm) + sizeof(d) + sizeof(i) + sizeof(md)) & 0; }
SRC
cat >"$TMP/both.c" <<'SRC'
#include <pthread.h>
#include <sys/types.h>
#include <sched.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static struct sched_param sp;
int main(void) { return (int)(sizeof(m) + sizeof(sp)) & 0; }
SRC
ports/xorg/toolchain/likeos-cc -Wall -Wextra -fsyntax-only "$TMP/types.c"
ports/xorg/toolchain/likeos-cc -Wall -Wextra -fsyntax-only "$TMP/both.c"
echo "test-sys-types: the thread types are visible from <sys/types.h> alone"
