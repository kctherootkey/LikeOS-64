#!/bin/sh
# config-status.sh - make a file from its .in template the way configure's
# config.status does: every @NAME@ is replaced by the value recorded for
# NAME.  Used by Makefile.likeos for gnu/, lib/ and src/Makefile.
#
# The values come from config.subst (NAME<tab>VALUE per line, recorded by
# likeos-survey.sh) and from NAME=VALUE arguments, which take precedence:
# the directory names, which differ per Makefile, and the compiler and its
# flags, which are Makefile.likeos's.  A @NAME@ with no value is left as it
# is, as config.status leaves it.
#
# Usage:  config-status.sh SUBST IN OUT [NAME=VALUE ...]
#
# Copyright (C) 2026 The LikeOS Project

set -eu

[ $# -ge 3 ] || { echo "usage: $0 SUBST IN OUT [NAME=VALUE ...]" >&2; exit 2; }
subst=$1 in=$2 out=$3
shift 3

# The overrides go to awk through a file: values may hold anything a shell
# word can, awk -v would interpret backslashes in them.
ovr="$out.ovr"
: >"$ovr"
for a in "$@"; do
	printf '%s\t%s\n' "${a%%=*}" "${a#*=}" >>"$ovr"
done

awk -F '\t' -v ovr="$ovr" -v subst="$subst" '
	BEGIN {
		while ((getline line < subst) > 0) {
			i = index(line, "\t")
			if (i) val[substr(line, 1, i - 1)] = substr(line, i + 1)
		}
		while ((getline line < ovr) > 0) {
			i = index(line, "\t")
			if (i) val[substr(line, 1, i - 1)] = substr(line, i + 1)
		}
	}
	{
		s = $0; r = ""
		while (match(s, /@[A-Za-z_][A-Za-z_0-9]*@/)) {
			name = substr(s, RSTART + 1, RLENGTH - 2)
			r = r substr(s, 1, RSTART - 1)
			if (name in val) r = r val[name]
			else r = r substr(s, RSTART, RLENGTH)
			s = substr(s, RSTART + RLENGTH)
		}
		print r s
	}' "$in" >"$out.tmp"
rm -f "$ovr"
mv "$out.tmp" "$out"
