/*
 * config.h - hand-written tmux 3.6a feature flags for LikeOS-64.
 *
 * These macros are normally produced by autoconf's `configure` script.
 * Since we cross-compile statically against a fixed environment (the
 * LikeOS-64 libc + libevent.so), the appropriate values are known at
 * port time and recorded here.
 *
 * Conventions:
 *   HAVE_xxx        - libc/syscall feature is present.
 *   When undefined, tmux falls back to the corresponding compat/xxx.c
 *   implementation supplied with the upstream tarball.
 */
#ifndef TMUX_CONFIG_H_LIKEOS
#define TMUX_CONFIG_H_LIKEOS

/* Identification */
#define PACKAGE          "tmux"
#define PACKAGE_NAME     "tmux"
#define PACKAGE_VERSION  "3.6a"
#define PACKAGE_STRING   "tmux 3.6a"
#define PACKAGE_TARNAME  "tmux"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_URL      ""
#define VERSION          "3.6a"

/* Feature-test macros */
#define _GNU_SOURCE      1
#define STDC_HEADERS     1

/* Standard headers we have */
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H  1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_STRINGS_H   1
#define HAVE_INTTYPES_H  1
#define HAVE_STDINT_H    1
#define HAVE_UNISTD_H    1
#define HAVE_DIRENT_H    1
#define HAVE_FCNTL_H     1
#define HAVE_PATHS_H     1
#define HAVE_PTY_H       1
#define HAVE_SYS_DIR_H   1
#define HAVE_UTIL_H      1
#define HAVE_QUEUE_H     1
#define HAVE_SYS_QUEUE_H 1

/* libevent flavour */
#define HAVE_EVENT2_EVENT_H 1

/* terminal-info */
#define HAVE_NCURSES_H   1
#define HAVE_TIPARM      1

/* libc functions present */
#define HAVE_DIRFD       1
#define HAVE_SYSCONF     1
#define HAVE_GETLINE     1
#define HAVE_STRCASESTR  1
#define HAVE_STRSEP      1
#define HAVE_STRNDUP     1
#define HAVE_STRLCAT     1
#define HAVE_STRLCPY     1
#define HAVE_FORKPTY     1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_FNMATCH     1
#define HAVE_GLOB        1
#define HAVE_PROC_PID    1
#define HAVE_SETENV      1

/* Routines we *don't* have - the compat/ fall-back is used instead. */
/* (left undefined: HAVE_ASPRINTF, HAVE_CFMAKERAW, HAVE_CLOSEFROM,
 *  HAVE_DAEMON, HAVE_EXPLICIT_BZERO, HAVE_FGETLN, HAVE_FREEZERO,
 *  HAVE_GETDTABLECOUNT, HAVE_GETDTABLESIZE, HAVE_GETPEEREID,
 *  HAVE_GETPROGNAME, HAVE_GETRANDOM, HAVE_HTONLL, HAVE_NTOHLL,
 *  HAVE_IMSG, HAVE_MEMMEM, HAVE_REALLOCARRAY, HAVE_RECALLOCARRAY,
 *  HAVE_SETENV, HAVE_SETPROCTITLE, HAVE_STRTONUM, HAVE_VIS,
 *  HAVE_UNVIS, HAVE_FLOCK, HAVE_PRCTL, HAVE_PR_SET_NAME,
 *  HAVE_PROGRAM_INVOCATION_SHORT_NAME, HAVE___PROGNAME,
 *  HAVE_SO_PEERCRED, HAVE_MALLOC_TRIM, HAVE_B64_NTOP,
 *  HAVE_TIPARM, HAVE_TIPARM_S, HAVE_NCURSES_H, HAVE_LIBM)
 */

/* Path defaults */
#define TMUX_CONF        "/etc/tmux.conf:~/.tmux.conf"
#define TMUX_LOCK_CMD    "lock"
#define TMUX_TERM        "screen"

#endif /* TMUX_CONFIG_H_LIKEOS */
