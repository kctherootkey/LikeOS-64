/*
 * sys/file.h - flock(2) constants (subset).
 *
 * Our libc does not implement BSD-style advisory file locking yet, but
 * the symbol must exist so portable applications compile.  Calls return
 * 0 (no-op) - the kernel does not enforce flock semantics.
 */
#ifndef _SYS_FILE_H
#define _SYS_FILE_H

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#ifdef __cplusplus
extern "C" {
#endif

int flock(int fd, int operation);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FILE_H */
