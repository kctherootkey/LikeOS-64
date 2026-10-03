#!/bin/sh
# Host test for the connector probe (kernel/dev/gpu/drm/drm_probe_helper.c)
# on the EDID and mode libraries: the list an EDID gives a connector, the
# checks, the preferred mode at modes[0], the probe's fallbacks and the
# hotplug counting.  Compiled against the kernel's own headers, with the
# fence test's prelude standing in for the scheduler.  Run from the
# repository root; "-v" prints each EDID's list size and first mode.
#
# Copyright (C) 2026 The LikeOS Project

set -e
ulimit -v 2000000
TMP=${TMPDIR:-/tmp}/likeos-drm-probe-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
D=kernel/dev/gpu/drm
timeout 120 nice -n 19 cc -std=gnu11 -Wall -Wextra -Werror -O1 -g \
	-include host/test-drm-fence-prelude.h -Iinclude -o "$TMP/test" \
	host/test-drm-probe.c host/test-edid-corpus.c \
	$D/drm_probe_helper.c $D/drm_edid_core.c $D/drm_edid_parse.c \
	$D/drm_displayid.c $D/drm_eld.c $D/drm_edid_tables.c $D/drm_modes.c
timeout 120 nice -n 19 "$TMP/test" "$@"
