#!/bin/sh
# Build the GTK3 stack into the shared port sysroot, in dependency order.
#
# The X.Org port's build.sh does the work; this only says which manifest to
# read.  See ../build.sh for why the two ports share one set of scripts.
#
# Usage:  ./build.sh [-f] [package ...]  (no arguments = everything not yet built)

here=$(cd "$(dirname "$0")" && pwd)
exec env LIKEOS_PORT_DIR="$here" "$here/../build.sh" "$@"
