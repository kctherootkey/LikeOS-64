#!/bin/sh
# Unpack the GTK3 stack's tarballs into predictable directory names.
#
# The X.Org port's unpack.sh does the work; this only says which manifest to
# read.  See ../build.sh for why the two ports share one set of scripts.
#
# Usage:  ./unpack.sh [package ...]      (no arguments = everything)

here=$(cd "$(dirname "$0")" && pwd)
exec env LIKEOS_PORT_DIR="$here" "$here/../unpack.sh" "$@"
