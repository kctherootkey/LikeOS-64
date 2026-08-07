#!/bin/sh
# Fetch the GTK3 stack's source tarballs.
#
# The X.Org port's fetch.sh does the work; this only says which manifest to
# read.  See ../build.sh for why the two ports share one set of scripts: they
# cross-compile the same way, into the same sysroot, with the same toolchain --
# only the package list differs.
#
# Usage:  ./fetch.sh [package ...]      (no arguments = everything)

here=$(cd "$(dirname "$0")" && pwd)
exec env LIKEOS_PORT_DIR="$here" "$here/../fetch.sh" "$@"
