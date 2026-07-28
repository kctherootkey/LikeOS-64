#!/bin/sh
# likeos-configure.sh — configure OpenSSH for the LikeOS-64 target.
#
# Runs the stock autoconf configure through the LikeOS cross-compiler wrapper
# (likeos-cc), then applies the handful of config.h corrections the cross probe
# cannot make on its own:
#   * functions the libc supplies as static-inline (their link probes fail)
#   * OpenSSL ECC support (the probe needs the deprecated EC_KEY type)
#   * statvfs (now implemented in libc)
#   * disable utmpx (no utmpx database on this platform)
#
# Idempotent: re-running reconfigures from scratch.
set -e
cd "$(dirname "$0")"

CC="$PWD/likeos-cc"
ZLIB="$(cd ../lib/zlib-1.3.1 && pwd)"

cat > likeos.cache <<'EOF'
ac_cv_func_getpgrp_void=yes
ac_cv_func_setpgrp_void=yes
ac_cv_have_broken_snprintf=no
ac_cv_have_working_snprintf=yes
ac_cv_have_broken_getaddrinfo=no
ac_cv_have_space_d_name_in_struct_dirent=yes
ac_cv_have_accrights_in_msghdr=no
ac_cv_have_control_in_msghdr=yes
ac_cv_func_mblen=yes
ac_cv_func_mbtowc=yes
ac_cv_func_wcwidth=yes
ac_cv_func_nl_langinfo=yes
EOF

CC="$CC" ./configure \
	--host=x86_64-unknown-likeos --build="$(gcc -dumpmachine)" \
	--cache-file=likeos.cache \
	--prefix=/usr --sysconfdir=/etc/ssh --libexecdir=/usr/libexec \
	--without-pam --without-libedit --without-selinux --without-kerberos5 \
	--without-audit --without-ssl-engine --disable-strip \
	--disable-utmp --disable-wtmp --disable-lastlog \
	--disable-pututline --disable-pututxline \
	--with-zlib="$ZLIB"

# --- config.h corrections the cross probe cannot make -----------------------
enable() {
	sed -i "s|/\\* #undef $1 \\*/|#define $1 1|" config.h
}
# libc provides these as inline / would-be-linkable, but the probe missed them
enable HAVE_BZERO
enable HAVE_BCOPY
enable HAVE_INET_NTOA
# OpenSSL elliptic curve support (probe needs the now-deprecated EC_KEY type)
enable OPENSSL_HAS_ECC
enable OPENSSL_HAS_NISTP256
enable OPENSSL_HAS_NISTP384
enable OPENSSL_HAS_NISTP521
# statvfs is implemented in the LikeOS libc
enable HAVE_STATVFS
enable HAVE_FSTATVFS
enable HAVE_STRUCT_STATVFS_F_NAMEMAX
# no utmpx database on this platform
enable DISABLE_UTMPX

echo "likeos-configure.sh: OpenSSH configured for LikeOS-64"
