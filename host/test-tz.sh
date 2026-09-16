#!/bin/sh
# Host test for the libc's time zone support (user/lib/libc/src/time/time.c).
#
# The file is compiled natively with every public name it defines renamed out
# of the way, so it can be linked beside the host's own libc and called
# directly.  The zone logic itself is pure: a TZ string in, an offset out.
#
# Copyright (C) 2026 The LikeOS Project

set -e
TMP=${TMPDIR:-/tmp}/likeos-tz-test.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

RENAME="-Dtzset=lk_tzset -Dlocaltime=lk_localtime -Dlocaltime_r=lk_localtime_r \
	-Dgmtime=lk_gmtime -Dgmtime_r=lk_gmtime_r -Dmktime=lk_mktime \
	-Dtimegm=lk_timegm -Dstrftime=lk_strftime -Dstrptime=lk_strptime \
	-Dctime=lk_ctime -Dctime_r=lk_ctime_r -Dasctime=lk_asctime \
	-Dasctime_r=lk_asctime_r -Ddifftime=lk_difftime -Dtime=lk_time \
	-Dclock=lk_clock -Dnanosleep=lk_nanosleep -Dclock_gettime=lk_clock_gettime \
	-Dclock_settime=lk_clock_settime -Dclock_getres=lk_clock_getres \
	-Dtzname=lk_tzname -Dtimezone=lk_timezone -Ddaylight=lk_daylight"

cc -std=gnu11 -w -O1 $RENAME -I user/lib/libc/include \
	-c user/lib/libc/src/time/time.c -o "$TMP/time.o"
cc -std=gnu11 -Wall -Wextra -O1 -o "$TMP/test" host/test-tz.c "$TMP/time.o"
"$TMP/test"
