#!/bin/sh
# Return ports/xorg/gtk3/ to its checked-in state.
#
# The X.Org port's clean.sh does the work; this only says which manifest to
# read.  See ../build.sh for why the two ports share one set of scripts.
#
# Usage:  ./clean.sh [-a]               (-a also removes the downloaded tarballs)

here=$(cd "$(dirname "$0")" && pwd)
exec env LIKEOS_PORT_DIR="$here" "$here/../clean.sh" "$@"
