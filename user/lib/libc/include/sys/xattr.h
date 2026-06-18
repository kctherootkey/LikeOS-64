/* sys/xattr.h - extended attribute calls (LikeOS-64 libc) */
#ifndef _SYS_XATTR_H
#define _SYS_XATTR_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flags for *setxattr() */
#define XATTR_CREATE   1   /* set value, fail if attr already exists */
#define XATTR_REPLACE  2   /* set value, fail if attr does not exist */

int     setxattr (const char *path, const char *name, const void *value, size_t size, int flags);
int     lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int     fsetxattr(int fd,           const char *name, const void *value, size_t size, int flags);

ssize_t getxattr (const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t fgetxattr(int fd,           const char *name, void *value, size_t size);

ssize_t listxattr (const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
ssize_t flistxattr(int fd,           char *list, size_t size);

int     removexattr (const char *path, const char *name);
int     lremovexattr(const char *path, const char *name);
int     fremovexattr(int fd,           const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_XATTR_H */
