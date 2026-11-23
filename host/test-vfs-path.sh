#!/bin/bash
# Host unit tests of the VFS whole-path code (host/test-vfs-path.c: the
# path-buffer allocator, canonicalisation and the walk's path handling at
# 4095/4096/4097 bytes, the task's path strings) and of execve's argument
# limits (host/test-exec-args.c), built with the filesystem harness's kernel services
# (host/test-fs.sh builds those first), AddressSanitizer and UBSan.
#
#   host/test-vfs-path.sh
# Environment: TESTFS_DIR as host/test-fs.sh.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2
TESTFS_DIR=${TESTFS_DIR:-${TMPDIR:-/tmp}/likeos-test-fs}
export TESTFS_DIR
B=$(dirname "$(host/test-fs.sh fat32 --build | tail -1)") || exit 2
O="$TESTFS_DIR/build-vfs-path"
mkdir -p "$O" || exit 2
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1
	-Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
	-Wno-unused-parameter -Wno-discarded-qualifiers
	-Werror=implicit-function-declaration $SAN -include host/test-fs-prelude.h"
gcc $KFLAGS -c kernel/fs/namei.c -o "$O/namei.o" || exit 2
# (the harness stubs have their own vfs_meta_changed)
gcc $KFLAGS -Dvfs_meta_changed=tvp_vfs_meta_changed \
	-c host/test-vfs-path.c -o "$O/test-vfs-path.o" || exit 2
# The harness's kernel services, without its runner (main) and the driver;
# what vfs.c/namei.c reach of the rest of the kernel and the test does not
# call stays unresolved.
objs=$(ls "$B"/*.o | grep -v '/test-fs-main\.o$\|/fat32\.o$')
gcc $SAN -o "$O/test-vfs-path" "$O/test-vfs-path.o" "$O/namei.o" $objs \
	-no-pie -Wl,--unresolved-symbols=ignore-all || exit 2
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:exitcode=4}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1:exitcode=5}
timeout 120 "$O/test-vfs-path"
rc=$?
# execve's argument limits (host/test-exec-args.c)
gcc $KFLAGS -c host/test-exec-args.c -o "$O/test-exec-args.o" || exit 2
gcc $SAN -o "$O/test-exec-args" "$O/test-exec-args.o" $objs \
	-no-pie -Wl,--unresolved-symbols=ignore-all || exit 2
timeout 300 "$O/test-exec-args" || rc=1
exit $rc
