/* curl_config.h — Hand-crafted configuration header for LikeOS-64.
 *
 * This file plays the role of the autoconf-generated lib/curl_config.h.
 * It is included (indirectly) by every curl source file via curl_setup.h
 * when HAVE_CONFIG_H is defined, which we pass on the compiler command line.
 *
 * Architecture: x86-64 (LP64 — sizeof(long) == sizeof(void*) == 8)
 * Platform:     LikeOS-64 (custom POSIX-compatible OS)
 * TLS backend:  OpenSSL 3.5.6
 * HTTP/2:       nghttp2 1.65.0
 * Compression:  zlib 1.3.1
 */

#ifndef CURL_CONFIG_LIKEOS_H
#define CURL_CONFIG_LIKEOS_H

/* ----------------------------------------------------------------
 * Identity
 * ---------------------------------------------------------------- */
#define OS "LikeOS"
#define CURL_OS "LikeOS"
#define PACKAGE "curl"
#define PACKAGE_NAME "curl"
#define PACKAGE_VERSION "8.14.1"
#define PACKAGE_STRING "curl 8.14.1"
#define PACKAGE_TARNAME "curl"
#define PACKAGE_URL ""
#define PACKAGE_BUGREPORT "https://curl.se/mail/list.cgi"
#define VERSION "8.14.1"

/* ----------------------------------------------------------------
 * TLS backend: OpenSSL 3.x
 * ---------------------------------------------------------------- */
#define USE_SSL 1
#define USE_OPENSSL 1
#define HAVE_OPENSSL_SSL_H 1
#define HAVE_OPENSSL_CRYPTO_H 1
#define HAVE_OPENSSL_ERR_H 1
#define HAVE_OPENSSL_RAND_H 1
#define HAVE_OPENSSL_X509_H 1
#define HAVE_OPENSSL_PEM_H 1
#define HAVE_OPENSSL_PKCS12_H 1
#define HAVE_OPENSSL_SRP 1
#define HAVE_OPENSSL_VERSION 1
#define HAVE_EVP_PKEY_GET_PARAMS 1
#define HAVE_OPAQUE_EVP_PKEY 1
#define HAVE_OPAQUE_RSA_DSA_DH 1
#define HAVE_KEYLOG_CALLBACK 1
#define HAVE_RANDOM_INIT_BY_DEFAULT 1
/* Not available: ENGINE, SHA-512/256, QUIC */
/* TLS-SRP: disabled — OpenSSL 3.5.6 built with no-deprecated, SRP API removed */
#undef USE_TLS_SRP
#undef USE_OPENSSL_SRP
/* OpenSSL DES/MD5 legacy APIs are not available (no-deprecated build).
 * curl will use its own internal implementations instead. */
#undef USE_OPENSSL_DES
#undef USE_OPENSSL_MD5
#undef USE_OPENSSL_ENGINE
#undef USE_OPENSSL_SHA512_256
#undef USE_OPENSSL_QUIC
/* No early data support in our build */
#undef HAVE_OPENSSL_EARLYDATA

/* ----------------------------------------------------------------
 * HTTP/2 via nghttp2
 * ---------------------------------------------------------------- */
#define USE_NGHTTP2 1
#define USE_HTTP2 1
#define HAVE_NGHTTP2_NGHTTP2_H 1

/* ----------------------------------------------------------------
 * HTTP/3 — not supported (no QUIC library)
 * ---------------------------------------------------------------- */
#undef USE_HTTP3
#undef USE_NGHTTP3
#undef USE_NGTCP2
#undef USE_QUICHE
#undef USE_MSH3
#undef USE_OPENSSL_QUIC

/* ----------------------------------------------------------------
 * Compression via zlib
 * ---------------------------------------------------------------- */
#define HAVE_LIBZ 1
#define HAVE_ZLIB_H 1

/* ----------------------------------------------------------------
 * Network features
 * ---------------------------------------------------------------- */
#define USE_IPV6 1
#define ENABLE_IPV6 1
#define HAVE_GETADDRINFO 1
#define HAVE_GETADDRINFO_THREADSAFE 1
#define HAVE_FREEADDRINFO 1
#define HAVE_GETNAMEINFO 1
#define HAVE_GETPEERNAME 1
#define HAVE_GETSOCKNAME 1
#define HAVE_GETHOSTNAME 1
#define HAVE_SOCKET 1
#define HAVE_INET_NTOP 1
#define HAVE_INET_PTON 1
#define HAVE_NET_IF_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
/* netinet/in6.h not present as a separate file; IPv6 is in netinet/in.h */
#undef HAVE_NETINET_IN6_H
#define HAVE_NETINET_TCP_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_MSG_NOSIGNAL 1
#define HAVE_IF_NAMETOINDEX 1
/* LikeOS has Unix domain sockets */
#define USE_UNIX_SOCKETS 1

/* ----------------------------------------------------------------
 * I/O multiplexing
 * ---------------------------------------------------------------- */
#define HAVE_POLL 1
#define HAVE_POLL_FINE 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_FCNTL 1
#define HAVE_FCNTL_H 1
#define HAVE_FCNTL_O_NONBLOCK 1
#define HAVE_PIPE 1
#define HAVE_PIPE2 1
#define HAVE_ACCEPT4 1

/* ----------------------------------------------------------------
 * POSIX threading
 * ---------------------------------------------------------------- */
#define USE_THREADS_POSIX 1
#define HAVE_PTHREAD_H 1

/* ----------------------------------------------------------------
 * Authentication: Digest, Basic, Bearer
 * NTLM disabled: requires legacy DES from OpenSSL deprecated API,
 * which is not available in our no-deprecated OpenSSL 3.5.6 build.
 * ---------------------------------------------------------------- */
#define CURL_DISABLE_NTLM 1
#undef USE_NTLM
#undef USE_OPENSSL_DES

/* ----------------------------------------------------------------
 * Not supported (disabled)
 * ---------------------------------------------------------------- */
/* No LDAP */
#undef USE_LDAP
#undef USE_OPENLDAP
#undef HAVE_LDAP_SSL
#undef HAVE_LDAP_URL_PARSE
/* No Kerberos/GSS */
#undef USE_KERBEROS5
#undef HAVE_GSSAPI
/* No SASL-based GSASL */
#undef USE_GSASL
/* No SPNEGO (no libkrb5 or similar) */
#undef USE_SPNEGO
/* No SSH */
#undef USE_LIBSSH2
#undef USE_LIBSSH
/* No IDN / PSL */
#undef USE_LIBIDN2
#undef USE_WIN32_IDN
#undef USE_IDN
#undef USE_LIBPSL
/* No brotli or zstd */
#undef HAVE_BROTLI
/* No c-ares async DNS */
/* No RTMP */
#undef USE_LIBRTMP
/* No WolfSSL, mbedTLS, GnuTLS, BearSSL, RustTLS, Schannel, SecureTransport */
#undef USE_WOLFSSL
#undef USE_MBEDTLS
#undef USE_GNUTLS
/* No HTTPSRR */
#undef USE_HTTPSRR

/* ----------------------------------------------------------------
 * Standard C headers
 * ---------------------------------------------------------------- */
#define HAVE_ASSERT_H 1
#define HAVE_DIRENT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_NET_IF_H 1
#define HAVE_PWD_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1

/* ----------------------------------------------------------------
 * Standard library functions
 * ---------------------------------------------------------------- */
#define HAVE_ALARM 1
/* basename: not in LikeOS libc — curl has its own Curl_basename */
#define HAVE_BASENAME 1
#define HAVE_FNMATCH 1
#define HAVE_FORK 1
#define HAVE_GETCWD 1
#define HAVE_GETEUID 1
/* getpwuid_r: not in LikeOS libc */
#undef HAVE_GETPWUID_R
#define HAVE_GETRLIMIT 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_MEMRCHR 0
#define HAVE_MKFIFO 1
#define HAVE_MKOSTEMP 0
#define HAVE_MKSTEMP 1
#define HAVE_OPENDIR 1
#define HAVE_SETJMP_H 1
#define HAVE_SIGACTION 1
#define HAVE_SIGNAL 1
#define HAVE_SNPRINTF 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
/* LikeOS libc has only strerror(), not strerror_r() */
#define HAVE_STRERROR 1
#define HAVE_STRERROR_R 1
#undef  HAVE_GLIBC_STRERROR_R
#define HAVE_POSIX_STRERROR_R 1
#define HAVE_STRDUP 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
/* utime/utimes: not present in LikeOS libc */
#define HAVE_UTIME 1
#define HAVE_UTIMES 1
/* wordexp not available in LikeOS libc — curl uses fallback path */
#undef HAVE_WORDEXP_H
#define HAVE_GETIFADDRS 1
#define HAVE_IFADDRS_H 1

/* strsignal is available in LikeOS libc */
#define HAVE_STRSIGNAL 1

/* fseeko / ftello */
#define HAVE_DECL_FSEEKO 1
#define HAVE_FSEEKO 1

/* getpwuid_r 5-argument version */
#define HAVE_GETHOSTBYNAME_R 1
#define HAVE_GETHOSTBYNAME_R_6 1

/* ----------------------------------------------------------------
 * struct stat
 * ---------------------------------------------------------------- */
#define HAVE_STRUCT_STAT_ST_BLKSIZE 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_IN6_ADDR 1

/* ----------------------------------------------------------------
 * Type sizes (x86-64 LP64)
 * ---------------------------------------------------------------- */
#define SIZEOF_INT      4
#define SIZEOF_SHORT    2
#define SIZEOF_LONG     8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_SIZE_T   8
#define SIZEOF_SSIZE_T  8
#define SIZEOF_OFF_T    8
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_VOIDP    8

/* ----------------------------------------------------------------
 * curl_off_t definition (must match include/curl/system.h selection)
 * ---------------------------------------------------------------- */
#define CURL_TYPEOF_CURL_OFF_T     long long
#define CURL_FORMAT_CURL_OFF_T     "lld"
#define CURL_FORMAT_CURL_OFF_TU    "llu"
#define CURL_SUFFIX_CURL_OFF_T     LL
#define CURL_SUFFIX_CURL_OFF_TU    ULL

/* ----------------------------------------------------------------
 * Misc capability flags
 * ---------------------------------------------------------------- */
#define HAVE_LONGLONG 1
#define HAVE_LONG_LONG_TYPE 1
/* variadic macros — C99 */
#define HAVE_VARIADIC_MACROS_C99 1
#define HAVE_VARIADIC_MACROS_GCC 1

/* recv()/send() presence + argument types */
#define HAVE_RECV 1
#define RECV_TYPE_ARG1 int
#define RECV_TYPE_ARG2 void *
#define RECV_TYPE_ARG3 size_t
#define RECV_TYPE_ARG4 int
#define RECV_TYPE_RETV ssize_t
#define HAVE_SEND 1
#define SEND_TYPE_ARG1 int
#define SEND_TYPE_ARG2 void *
#define SEND_TYPE_ARG3 size_t
#define SEND_TYPE_ARG4 int
#define SEND_TYPE_RETV ssize_t
#define SEND_QUAL_ARG2 const

/* socklen_t */
#define HAVE_SOCKLEN_T 1

/* CA bundle/path — LikeOS image has the Mozilla CA bundle at this path */
#define CURL_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#define CURL_CA_PATH   "/etc/ssl/certs"

/* ----------------------------------------------------------------
 * Linux-specific extras: LikeOS uses netinet/tcp.h, not linux/tcp.h
 * ---------------------------------------------------------------- */
#undef HAVE_LINUX_TCP_H

/* TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT are in netinet/tcp.h */
#define HAVE_TCP_KEEPIDLE 1
#define HAVE_TCP_KEEPINTVL 1
#define HAVE_TCP_KEEPCNT 1

/* ----------------------------------------------------------------
 * Disable Windows-only paths
 * ---------------------------------------------------------------- */
#undef USE_WINSOCK
#undef USE_WINDOWS_SSPI
#undef USE_WIN32_CRYPTO
#undef USE_WIN32_LDAP
#undef USE_WIN32_LARGE_FILES
#undef HAVE_IO_H
#undef HAVE_IPHLPAPI_H

/* ----------------------------------------------------------------
 * Build-time markers
 * ---------------------------------------------------------------- */
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1

#endif /* CURL_CONFIG_LIKEOS_H */
