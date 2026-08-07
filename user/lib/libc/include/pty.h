/*
 * pty.h - GNU-libc-style PTY helper header.  Just an alias for util.h
 * since both export the same forkpty/openpty/login_tty interface.
 */
#ifndef _PTY_H
#define _PTY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <util.h>

#ifdef __cplusplus
}
#endif

#endif /* _PTY_H */
