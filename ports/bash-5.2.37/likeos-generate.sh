#!/bin/sh
# likeos-generate.sh - produce the generated sources for the LikeOS bash port.
#
# Upstream bash creates these with `configure` plus a set of build tools that
# run on the BUILD machine.  There is no configure run here (LikeOS is not an
# autoconf target), so this script does the equivalent by hand:
#
#   config.h          from config.h.in, answering every feature test for the
#                     LikeOS libc (see the DEF1 list and the value defines)
#   config-likeos.h   symbols configure would AC_DEFINE directly, i.e. that
#                     have no #undef template line in config.h.in
#   pathnames.h       from pathnames.h.in
#   version.h         via the shipped support/mkversion.sh
#   pipesize.h        the kernel's pipe capacity (kernel/ke/syscall.c:
#                     pipe_create(4096))
#   syntax.c          via the shipped mksyntax tool
#   signames.h        via the shipped support/mksignames tool
#   builtins/*.c      one per builtins/*.def, plus builtins.c + builtext.h,
#                     via the shipped builtins/mkbuiltins tool
#
# The three tools are compiled with the HOST compiler into hosttools/ - they
# run during the build, they are not part of the shell.
#
# Re-run with:  make -f Makefile.likeos generate
set -e
cd "$(dirname "$0")"

HOSTCC="${CC_FOR_BUILD:-gcc}"

# ---------------------------------------------------------------------------
# config.h
# ---------------------------------------------------------------------------
cp config.h.in config.h

# Plain feature switches: "#undef X" -> "#define X 1".
# Everything the LikeOS libc provides, plus every optional bash feature (this
# is a full-featured shell: job control, readline/history, arrays, extended
# globbing, [[ =~ ]], process substitution, coprocesses, completion...).
DEF1="
JOB_CONTROL ALIAS PUSHD_AND_POPD BRACE_EXPANSION READLINE BANG_HISTORY HISTORY
RESTRICTED_SHELL PROCESS_SUBSTITUTION PROMPT_STRING_DECODE SELECT_COMMAND
HELP_BUILTIN ARRAY_VARS DPAREN_ARITHMETIC EXTENDED_GLOB COND_COMMAND
COND_REGEXP COPROCESS_SUPPORT ARITH_FOR_COMMAND NETWORK_REDIRECTIONS
PROGRAMMABLE_COMPLETION DEBUGGER CASEMOD_ATTRS CASEMOD_EXPANSIONS
GLOBASCII_DEFAULT FUNCTION_IMPORT TRANSLATABLE_STRINGS
COMMAND_TIMING
V9_ECHO GETPGRP_VOID HAVE_POSIX_SIGNALS HAVE_POSIX_SIGSETJMP
STDC_HEADERS PROTOTYPES __PROTOTYPES HAVE_ALLOCA
HAVE_LONG_LONG_INT HAVE_UNSIGNED_LONG_LONG_INT HAVE_LONG_LONG HAVE_UNSIGNED_LONG_LONG
HAVE_LONG_DOUBLE HAVE_C_LONG_DOUBLE HAVE_VA_COPY HAVE_C99_VARIADIC_MACROS
HAVE_HASH_BANG_EXEC HAVE_STRTOLD
HAVE_DEV_FD HAVE_DEV_STDIN
HAVE_GETPW_DECLS
GWINSZ_IN_SYS_IOCTL STRUCT_WINSIZE_IN_TERMIOS
HAVE_STRUCT_DIRENT_D_INO HAVE_STRUCT_STAT_ST_BLOCKS HAVE_STRUCT_TIMEVAL
HAVE_TIMEVAL HAVE_STRUCT_TIMEZONE
HAVE_LANGINFO_CODESET
HAVE_LIBDL
HAVE_UNISTD_H HAVE_STDLIB_H HAVE_STDDEF_H HAVE_STDINT_H HAVE_STRING_H
HAVE_STRINGS_H HAVE_LIMITS_H HAVE_LOCALE_H HAVE_LANGINFO_H HAVE_DIRENT_H
HAVE_TERMIOS_H HAVE_SYS_FILE_H HAVE_SYS_IOCTL_H HAVE_SYS_PARAM_H
HAVE_SYS_RESOURCE_H HAVE_SYS_SELECT_H HAVE_SYS_SOCKET_H HAVE_SYS_STAT_H
HAVE_SYS_TIME_H HAVE_SYS_TIMES_H HAVE_SYS_WAIT_H HAVE_NETINET_IN_H
HAVE_ARPA_INET_H HAVE_NETDB_H HAVE_GRP_H HAVE_PWD_H HAVE_INTTYPES_H
HAVE_SYSLOG_H HAVE_WCHAR_H HAVE_WCTYPE_H HAVE_DLFCN_H HAVE_FCNTL_H
HAVE_REGEX_H HAVE_SYSEXITS_H
HAVE_BCOPY HAVE_BZERO HAVE_DPRINTF HAVE_DUP2 HAVE_FACCESSAT HAVE_FCNTL
HAVE_GETADDRINFO HAVE_GETCWD HAVE_GETDTABLESIZE HAVE_GETGROUPS
HAVE_GETHOSTBYNAME HAVE_GETHOSTNAME HAVE_GETPAGESIZE HAVE_GETPEERNAME
HAVE_GETPWENT HAVE_GETPWNAM HAVE_GETPWUID HAVE_GETRLIMIT HAVE_GETRUSAGE
HAVE_GETSERVBYNAME HAVE_GETTIMEOFDAY HAVE_INET_ATON HAVE_ISASCII
HAVE_ISBLANK HAVE_ISGRAPH HAVE_ISPRINT HAVE_ISSPACE HAVE_ISXDIGIT
HAVE_KILL HAVE_KILLPG HAVE_LSTAT HAVE_MEMMOVE HAVE_MEMSET HAVE_MKSTEMP
HAVE_PATHCONF HAVE_PUTENV HAVE_RAISE HAVE_READLINK HAVE_RENAME
HAVE_SBRK HAVE_SELECT HAVE_SETENV HAVE_SETLINEBUF HAVE_SETLOCALE
HAVE_SETREGID HAVE_SETVBUF HAVE_SNPRINTF HAVE_STRCASECMP HAVE_STRCASESTR
HAVE_STRCHR HAVE_STRERROR HAVE_STRFTIME HAVE_STRNLEN HAVE_STRPBRK
HAVE_STRSIGNAL HAVE_STRSTR HAVE_STRTOD HAVE_STRTOIMAX HAVE_STRTOL
HAVE_STRTOLL HAVE_STRTOUL HAVE_STRTOULL HAVE_STRTOUMAX HAVE_SYSCONF
HAVE_SYSLOG HAVE_TCGETATTR HAVE_TCGETPGRP HAVE_TIMES HAVE_TTYNAME
HAVE_TZSET HAVE_UNAME HAVE_UNSETENV HAVE_VASPRINTF HAVE_VSNPRINTF
HAVE_ASPRINTF HAVE_VPRINTF HAVE_WAITPID HAVE_ALARM HAVE_FDOPEN
HAVE_GETLINE HAVE_GETDELIM HAVE_MKTIME HAVE_NL_LANGINFO HAVE_STPCPY
HAVE_STRDUP HAVE_STRNDUP HAVE_DLOPEN HAVE_DLCLOSE HAVE_DLSYM
HAVE_POSIX_SIGNALBLOCK
HAVE_GETRANDOM HAVE_NANOSLEEP HAVE_WCSWIDTH HAVE_MKFIFO HAVE_MKNOD
HAVE_MBSTATE_T HAVE_MBRTOWC HAVE_MBRLEN HAVE_MBSRTOWCS HAVE_WCRTOMB
HAVE_WCSRTOMBS HAVE_ISWCTYPE HAVE_ISWLOWER HAVE_ISWUPPER HAVE_TOWLOWER
HAVE_TOWUPPER HAVE_WCHAR_T HAVE_WCTYPE_T HAVE_WINT_T HAVE_WCWIDTH
HAVE_WCTYPE HAVE_ISWDIGIT HAVE_ISWXDIGIT
HAVE_ERRNO_H HAVE_SYS_TYPES_H HAVE_STDBOOL_H HAVE_SYS_MMAN_H
HAVE_MMAP HAVE_MUNMAP HAVE_REGCOMP HAVE_REGEXEC HAVE_FNMATCH
HAVE_LOCALECONV HAVE_MKDTEMP HAVE_STRCSPN HAVE_STRINGIZE
HAVE_STD_PUTENV HAVE_STD_UNSETENV HAVE_TZNAME
FIONREAD_IN_SYS_IOCTL
HAVE_STRUCT_STAT_ST_ATIM_TV_NSEC TYPEOF_STRUCT_STAT_ST_ATIM_IS_STRUCT_TIMESPEC
NAMED_PIPES_MISSING
"
for d in $DEF1; do
	sed -i "s|^#undef ${d}\$|#define ${d} 1|" config.h
done

# Value defines
sed -i 's|^#undef RETSIGTYPE$|#define RETSIGTYPE void|' config.h
sed -i 's|^#undef RLIMTYPE$|#define RLIMTYPE rlim_t|' config.h
sed -i 's|^#undef DEV_FD_PREFIX$|#define DEV_FD_PREFIX "/dev/fd/"|' config.h
sed -i 's|^#undef DEFAULT_MAIL_DIRECTORY$|#define DEFAULT_MAIL_DIRECTORY "/var/mail"|' config.h
sed -i 's|^#undef SIZEOF_CHAR$|#define SIZEOF_CHAR 1|' config.h
sed -i 's|^#undef SIZEOF_SHORT$|#define SIZEOF_SHORT 2|' config.h
sed -i 's|^#undef SIZEOF_INT$|#define SIZEOF_INT 4|' config.h
sed -i 's|^#undef SIZEOF_LONG$|#define SIZEOF_LONG 8|' config.h
sed -i 's|^#undef SIZEOF_LONG_LONG$|#define SIZEOF_LONG_LONG 8|' config.h
sed -i 's|^#undef SIZEOF_CHAR_P$|#define SIZEOF_CHAR_P 8|' config.h
sed -i 's|^#undef SIZEOF_DOUBLE$|#define SIZEOF_DOUBLE 8|' config.h
sed -i 's|^#undef SIZEOF_SIZE_T$|#define SIZEOF_SIZE_T 8|' config.h
sed -i 's|^#undef SIZEOF_WCHAR_T$|#define SIZEOF_WCHAR_T 4|' config.h
for d in STRTOLD PRINTF SBRK SETREGID STRCPY STRSIGNAL STRTOIMAX STRTOL \
	 STRTOLL STRTOUL STRTOULL STRTOUMAX; do
	sed -i "s|^#undef HAVE_DECL_${d}\$|#define HAVE_DECL_${d} 1|" config.h
done
sed -i 's|^#undef HAVE_DECL_CONFSTR$|#define HAVE_DECL_CONFSTR 0|' config.h
sed -i 's|^#undef HAVE_DECL_TZNAME$|#define HAVE_DECL_TZNAME 1|' config.h
sed -i 's|^#undef SIZEOF_INTMAX_T$|#define SIZEOF_INTMAX_T 8|' config.h
# x86-64: the stack grows toward lower addresses
sed -i 's|^#undef STACK_DIRECTION$|#define STACK_DIRECTION -1|' config.h
sed -i 's|^#undef HAVE_DECL_AUDIT_USER_TTY$|#define HAVE_DECL_AUDIT_USER_TTY 0|' config.h

# Fixed-width helper types the libc headers do not name.  u_int/u_long are
# NOT set here: <sys/types.h> already provides them, and defining them would
# turn every use into "unsigned unsigned int".
sed -i 's|^#undef bits16_t$|#define bits16_t short|' config.h
sed -i 's|^#undef u_bits16_t$|#define u_bits16_t unsigned short|' config.h
sed -i 's|^#undef bits32_t$|#define bits32_t int|' config.h
sed -i 's|^#undef u_bits32_t$|#define u_bits32_t unsigned int|' config.h
sed -i 's|^#undef bits64_t$|#define bits64_t long long|' config.h
sed -i 's|^#undef GETGROUPS_T$|#define GETGROUPS_T gid_t|' config.h
sed -i 's|^#undef HAVE_STRUCT_TIMESPEC$|#define HAVE_STRUCT_TIMESPEC 1|' config.h
sed -i 's|^#undef TIME_H_DEFINES_STRUCT_TIMESPEC$|#define TIME_H_DEFINES_STRUCT_TIMESPEC 1|' config.h
sed -i 's|^#undef EXTGLOB_DEFAULT$|#define EXTGLOB_DEFAULT 0|' config.h
# WEXITSTATUS(s) is ((s) >> 8) & 0xff in <sys/wait.h>, so the shift is 8.
sed -i 's|^#undef WEXITSTATUS_OFFSET$|#define WEXITSTATUS_OFFSET 8|' config.h
sed -i 's|^#undef HAVE_STDARG_H$|#define HAVE_STDARG_H 1|' config.h

cat > config-likeos.h <<'EOF'
/* config-likeos.h -- LikeOS-specific settings for the bash port that have no
   template line in config.h.in (configure normally AC_DEFINEs them
   directly).  Generated by likeos-generate.sh; included from config.h.

   PREFER_STDARG is deliberately absent: config-bot.h defines it whenever
   HAVE_STDARG_H is set, and defining it here too only triggers a
   redefinition warning. */

#define CONF_HOSTTYPE "x86_64"
#define CONF_OSTYPE "likeos"
#define CONF_MACHTYPE "x86_64-unknown-likeos"
#define CONF_VENDOR "unknown"

/* i18n identifiers (NLS itself is off; include/gettext.h stubs these out) */
#define PACKAGE "bash"
#define LOCALEDIR "/usr/share/locale"
EOF
sed -i 's|^#include "config-bot.h"$|#include "config-likeos.h"\n\n#include "config-bot.h"|' config.h

# ---------------------------------------------------------------------------
# pathnames.h, version.h, pipesize.h
# ---------------------------------------------------------------------------
sed 's|@DEBUGGER_START_FILE@|/usr/share/bashdb/bashdb-main.inc|' \
	pathnames.h.in > pathnames.h

# No -b: that flag increments the build counter and rewrites .build, a TRACKED
# upstream file, so every regeneration left the tree dirty and the reported
# build number drifting.  Without it mkversion.sh just reads the current value,
# which keeps the version string stable and the port byte-identical upstream.
sh support/mkversion.sh -S . -s release -d 5.2 -o newversion.h >/dev/null
mv newversion.h version.h

cat > pipesize.h <<'EOF'
/* pipesize.h - LikeOS pipe capacity (kernel/ke/syscall.c: pipe_create(4096)).
   Generated by likeos-generate.sh. */
#define PIPESIZE 4096
EOF
cp pipesize.h builtins/pipesize.h

# ---------------------------------------------------------------------------
# Build-machine tools, then the sources they generate
# ---------------------------------------------------------------------------
mkdir -p hosttools
$HOSTCC -O2 -I. -I./include -o hosttools/mksyntax mksyntax.c
$HOSTCC -O2 -I. -o hosttools/mksignames support/mksignames.c support/signames.c
$HOSTCC -O2 -DHAVE_CONFIG_H -DHAVE_STRING_H -DHAVE_STDLIB_H -I. -I./include \
	-o hosttools/mkbuiltins builtins/mkbuiltins.c

./hosttools/mksyntax -o syntax.c
./hosttools/mksignames signames.h

# One .c per .def (reserved.def is documentation only and produces none),
# then the builtins table + extern declarations.
( cd builtins && for d in *.def; do ../hosttools/mkbuiltins -D . "$d"; done
  ../hosttools/mkbuiltins -externfile builtext.h -structfile builtins.c \
	-noproduction -D . *.def )

echo "likeos-generate.sh: generated config.h and $(ls builtins/*.c | wc -l) builtins sources"
