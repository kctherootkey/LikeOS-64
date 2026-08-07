#!/bin/sh
# Shared configure driver for the autotools X packages.
#
# Every one of them is configured the same way, so the per-package
# Makefile.likeos only has to say what is different.  Sourcing one script also
# means a fix to the cross-build arrangement lands everywhere at once instead
# of being copied into forty files.
#
# Usage (from a package directory):
#   ../toolchain/likeos-autogen.sh [extra ./configure arguments...]

set -e

self=$0
case "$self" in
/*) ;;
*) self=$(pwd)/$self ;;
esac
TOOLCHAIN=$(cd "$(dirname "$self")" && pwd)
ROOT=$(cd "$TOOLCHAIN/../../.." && pwd)
SYSROOT="${LIKEOS_SYSROOT:-$ROOT/build/xorg-sysroot}"

CC="$TOOLCHAIN/likeos-cc"
# CXX too, even though most packages here are C.
#
# An autotools package that builds any C++ at all takes CXX from the
# environment and falls back to plain `g++' -- the BUILD HOST's compiler,
# with the host's libraries and the host's C library.  It does not fail
# where the mistake is: the C parts build correctly with the cross compiler
# and only the C++ link goes wrong, somewhere much later, complaining about
# a library that is in the sysroot rather than on this machine.  libtiff is
# where that first showed up (its optional iostream wrapper) and Enchant,
# which is C++ throughout, would have hit it next.
CXX="$TOOLCHAIN/likeos-c++"
PKG_CONFIG="$TOOLCHAIN/likeos-pkg-config"
export CC CXX PKG_CONFIG

# util-macros installs its m4 into the sysroot; without this every package's
# configure dies on an undefined XORG_MACROS_VERSION.
ACLOCAL_PATH="$SYSROOT/usr/share/aclocal"
export ACLOCAL_PATH
ACLOCAL="aclocal -I $SYSROOT/usr/share/aclocal"
export ACLOCAL

# Answers to probes that cannot run a target binary.  Seeded rather than
# guessed by configure, which would otherwise assume the host's answers.
# Absolute, because it has to survive a change of directory.  freetype's
# top-level configure is a wrapper that re-runs the real one from builds/unix,
# and a relative --cache-file then names a different (empty) file -- so the
# seeded answers below silently did not apply, libtool fell back to dropping
# inter-library dependencies, and freetype built a static archive.
cache=$(pwd)/likeos.cache
cat >"$cache" <<'EOF'
ac_cv_func_malloc_0_nonnull=yes
ac_cv_func_realloc_0_nonnull=yes
ac_cv_func_memcmp_working=yes
ac_cv_func_strtod=yes
ac_cv_have_decl_environ=yes
ac_cv_file__dev_random=yes
ac_cv_file__dev_urandom=yes
# Motif's configure.ac opens with two AC_CHECK_FILE probes for an X.h under
# /usr/X and /usr/X11R6, purely to pick a default --prefix.  AC_CHECK_FILE
# ABORTS configure outright when cross compiling ("cannot check for file
# existence when cross compiling"), so the answer has to be supplied here --
# and "no" is right: the prefix is passed explicitly below either way.
ac_cv_file__usr_X_include_X11_X_h=no
ac_cv_file__usr_X11R6_include_X11_X_h=no
# AC_FUNC_SETPGRP also refuses to run cross.  This libc's setpgrp() takes no
# argument (the System V form POSIX standardised), so "void" is yes.
ac_cv_func_setpgrp_void=yes
xorg_cv_malloc0_returns_null=no

# How libtool decides whether a -l dependency is a library it understands.
# Left to itself it answers "unknown" for a system it does not recognise, and
# then DROPS every dependency it cannot verify.  Dropping one makes it decide
# the library would contain undefined symbols, and — because these libraries
# are linked -no-undefined — it quietly builds a static archive instead, with
# no error and despite --enable-shared.  That is how libX11 came out as
# libX11.a while libXau, which has no external dependencies to drop, came out
# shared.
#
# pass_all is what every ELF platform uses: the linker resolves the deps, so
# libtool does not need to second-guess them.
lt_cv_deplibs_check_method=pass_all

# How startx generates an X authority cookie (xinit only).
#
# Its configure does AC_PATH_PROGS(MCOOKIE, [mcookie], ...) and then bakes the
# ABSOLUTE PATH it found into the startx script.  Searching for a program is
# the right thing when building for the machine you are on and exactly the
# wrong thing when cross-compiling: it finds the BUILD HOST's /usr/bin/mcookie
# and writes that into a script that will run somewhere else.  The failure is
# at runtime, on the target, and reads
#
#     Couldn't create cookie
#
# with no hint that a build-host path is the reason.
#
# So it is answered rather than probed, with a command that exists on the
# TARGET: openssl ships at /bin/openssl there, and `openssl rand -hex 16` is
# the same 128-bit hex cookie mcookie produces.  (configure has this exact
# fallback itself, for hosts without mcookie -- it just never gets there when
# the build host has one.)
ac_cv_path_MCOOKIE='/bin/openssl rand -hex 16'
EOF

if [ ! -f configure ] && [ -f autogen.sh ]; then
	NOCONFIGURE=1 ./autogen.sh
elif [ ! -f configure ] && [ -f configure.ac ]; then
	# A release tarball ships a generated `configure` and no autogen.sh, so
	# a patch to configure.ac has no effect until it is regenerated.  The
	# port deletes `configure` to ask for exactly that; without this arm the
	# build fails with "./configure: not found", which does not hint at the
	# cause.
	autoreconf -fi
fi

# Teach config.sub about the target triple.
#
# It validates the OS field against a fixed list and rejects anything it has
# not heard of.  Four different list layouts appear across these packages
# (names one-per-line, names in a pattern list, names with and without a
# leading dash), so matching a neighbouring OS name is fragile — the first
# three attempts at this each worked for some packages and silently missed
# others.
#
# Instead, target the one thing every generation has in common: the validation
# ends in a `*)` catch-all that prints "system ... not recognized".  Inserting
# an arm immediately before that arm is correct regardless of how the list
# above it is written.
# Packages do not agree on where config.sub lives: at the top, one level down
# (libtool's aux dir), or two (freetype keeps it in builds/unix/).
for sub in config.sub */config.sub */*/config.sub; do
	[ -f "$sub" ] || continue
	chmod u+w "$sub" 2>/dev/null || true

	# Already correct?  Test it rather than trusting that the name appears
	# somewhere: an arm inserted into the wrong case statement is present
	# but inert, and would otherwise never be corrected.
	if sh "$sub" x86_64-unknown-likeos >/dev/null 2>&1; then
		continue
	fi
	# Present but not working: drop it and place it again.
	if grep -q 'likeos' "$sub"; then
		# Remove the arm AND the ";;" that belongs to it.  Dropping only
		# the pattern line leaves an orphaned ";;", and two in a row is
		# a syntax error that breaks config.sub outright.
		awk '/^[[:space:]]*likeos\*/ { skip = 1; next }
		     skip && /^[[:space:]]*;;[[:space:]]*$/ { skip = 0; next }
		     { skip = 0; print }' "$sub" >"$sub.new" &&
			mv "$sub.new" "$sub" && chmod +x "$sub"
	fi

	# Anchor on the OS message specifically.  A config.sub has up to three
	# "not recognized" messages — machine, OS and object format — and an arm
	# inserted before the wrong one is accepted silently and does nothing.
	# Match on the words either side rather than on how the variable is
	# quoted: the message is spelled with backticks, with plain quotes, and
	# with escaped quotes across the generations here, but always names the
	# OS.  The machine and object-format messages never do.
	ln=$(grep -nE "(system|OS).*not recognized" "$sub" | head -1 | cut -d: -f1)
	[ -n "$ln" ] || continue
	# The last `*)` before that message opens the catch-all arm.
	star=$(awk -v end="$ln" 'NR < end && /^[[:space:]]*\*\)/ { n = NR } END { print n }' "$sub")
	[ -n "$star" ] && [ "$star" -gt 0 ] || continue

	# Both spellings: older config.sub keeps the OS with its leading dash
	# ("-likeos") at the point this case runs, newer ones have stripped it.
	awk -v at="$star" 'NR == at { print "\tlikeos* | -likeos*)"; print "\t\t;;" } { print }' \
		"$sub" >"$sub.new" && mv "$sub.new" "$sub"
	# Rewriting through a temporary file drops the executable bit, and
	# configure runs config.sub as a command.
	chmod +x "$sub"

	grep -q 'likeos' "$sub" ||
		echo "warning: could not teach $sub about likeos" >&2
done

# Teach libtool that this platform can build shared libraries.
#
# libtool decides that from a `case $host_os in` listing every system it knows.
# An unknown OS silently falls through to "cannot build shared libraries", and
# configure then reports:
#
#     checking if libtool supports shared libraries... no
#     checking whether to build shared libraries... no
#
# and builds static archives instead — with no error, despite --enable-shared.
# The result links but is wrong: the X server dlopen()s its drivers, and every
# client would carry its own copy of Xlib.
#
# LikeOS is ELF with GNU ld, which is what those Linux branches assume, so
# adding it alongside them is accurate rather than a bodge.
#
# This must be SCOPED, and getting that wrong is not theoretical: a blanket
# rewrite of every `linux*)` arm also hits the package's own arms, and those
# select entirely different things.  In xorg-server it silently selected the
# Linux os-support backend — VT_ACTIVATE, KDSETMODE, <linux/kd.h>, none of
# which exist here — instead of the "stub" backend configure would otherwise
# have picked.  The compile errors that produces point at the backend, not at
# this script, so the cause is a long way from the symptom.
#
# The two multi-system arms below (`linux* | k*bsd*-gnu`, `gnu* | linux*`) are
# libtool's own spelling and appear nowhere else, so they are rewritten as-is.
# A bare `linux*)` is ambiguous, so the arm's body decides: only arms that set
# a libtool variable are rewritten.
# Packages do not agree on where the real configure script lives.  freetype's
# top-level `configure` is a 137-line wrapper that delegates to
# builds/unix/configure, and rewriting only the wrapper taught it nothing:
# freetype built a static archive for weeks without complaint, which is exactly
# the failure this section exists to prevent.  So every candidate is tried, and
# a candidate is anything that switches on $host_os.
for cfg in configure */configure */*/configure; do
	[ -f "$cfg" ] || continue
	grep -q 'host_os' "$cfg" || continue
	grep -q 'likeos\*' "$cfg" && continue

	sed -i \
		-e 's/^\([[:space:]]*\)linux\* | k\*bsd\*-gnu/\1likeos* | linux* | k*bsd*-gnu/g' \
		-e 's/^\([[:space:]]*\)gnu\* | linux\*/\1likeos* | gnu* | linux*/g' \
		"$cfg"

	awk '
	/^[[:space:]]*linux\*\)[[:space:]]*$/ { start = NR; body = ""; inarm = 1; next }
	inarm {
		if ($0 ~ /^[[:space:]]*;;[[:space:]]*$/) {
			if (body ~ /(lt_cv_|ld_shlibs|version_type|library_names_spec|soname_spec|shlibpath_var|dynamic_linker|hardcode_|archive_cmds|deplibs_check_method|finish_cmds|need_lib_prefix|need_version|export_dynamic_flag_spec|whole_archive_flag_spec|link_all_deplibs|sys_lib_search_path_spec|postinstall_cmds|striplib)/)
				print start
			inarm = 0
			next
		}
		body = body "\n" $0
	}' "$cfg" >.likeos-libtool-arms

	while read -r ln; do
		[ -n "$ln" ] || continue
		sed -i "${ln}s/^\\([[:space:]]*\\)linux\\*)/\\1likeos* | linux*)/" "$cfg"
	done <.likeos-libtool-arms
	rm -f .likeos-libtool-arms
done

# Make sure the package can NAME this system before asking it to build for it.
#
# config.sub validates the host triple against a list of operating systems it
# knows, and x86_64-unknown-likeos is on nobody's list.  Most packages here ship
# a copy old enough not to care -- startup-notification's is from 2011 and
# echoes back whatever looks syntactically plausible -- but a recent one rejects
# it outright and configure stops before it starts:
#
#     configure: error: /bin/bash ./config.sub x86_64-unknown-likeos failed
#
# So the package's copy is REPLACED, but only when its own answer is no.  A
# package that already accepts the triple keeps its file: GMP's config.sub, for
# one, is a wrapper adding CPU spellings of its own around the standard script,
# and overwriting a working one would throw those away for nothing.
#
# configfsf.sub is checked as well, because that wrapper is what GMP delegates
# to and replacing only the outer file would leave the rejection in place.
for sub in config.sub configfsf.sub; do
	[ -f "$sub" ] || continue
	./"$sub" x86_64-unknown-likeos >/dev/null 2>&1 && continue
	cp -f "$TOOLCHAIN/config.sub" "$sub"
	chmod 755 "$sub"
done
for guess in config.guess configfsf.guess; do
	[ -f "$guess" ] || continue
	./"$guess" >/dev/null 2>&1 && continue
	cp -f "$TOOLCHAIN/config.guess" "$guess"
	chmod 755 "$guess"
done

# Because the scoping above can only err toward NOT rewriting an arm, and
# because the failure it would reintroduce is the silent one this whole section
# exists to prevent, the result is checked rather than assumed.
./configure \
	--host=x86_64-unknown-likeos \
	--build="$(gcc -dumpmachine)" \
	--prefix=/usr \
	--sysconfdir=/etc \
	--localstatedir=/var \
	--libdir=/usr/lib \
	--disable-static \
	--enable-shared \
	--disable-silent-rules \
	--cache-file="$cache" \
	"$@" 2>&1 | tee .likeos-configure.out
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

if grep -q 'whether to build shared libraries\.\.\. no' .likeos-configure.out; then
	echo "likeos-autogen: libtool refused to build shared libraries." >&2
	echo "  No host_os arm in configure matched, so it fell through to the" >&2
	echo "  'cannot build shared libraries' default -- see the scoped rewrite" >&2
	echo "  above.  Building static archives here would link but be wrong." >&2
	exit 1
fi
exit 0
