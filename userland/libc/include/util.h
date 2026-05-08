/*
 * util.h - PTY helper API (forkpty, openpty, login_tty).
 *
 * Modelled on the BSD libutil interface so portable terminal applications
 * (notably tmux) can build unmodified.  The implementations live in
 * src/syscalls/pty_util.c and use posix_openpt + ioctl(TIOCSCTTY).
 */
#ifndef _UTIL_H
#define _UTIL_H

#include <sys/types.h>
#include <termios.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

int   openpty(int* amaster, int* aslave, char* name,
              const struct termios* termp, const struct winsize* winp);
pid_t forkpty(int* amaster, char* name,
              const struct termios* termp, const struct winsize* winp);
int   login_tty(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _UTIL_H */
