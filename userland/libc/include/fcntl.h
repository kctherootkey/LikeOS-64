#ifndef _FCNTL_H
#define _FCNTL_H

// File open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Special dirfd for *at() syscalls
#define AT_FDCWD    -100

// fcntl commands
#define F_GETFL     3
#define F_SETFL     4

// File access
int open(const char* pathname, int flags, ...);
int openat(int dirfd, const char* pathname, int flags, ...);

#endif
