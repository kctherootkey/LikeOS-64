/*
 * utime.h — file access and modification time for LikeOS
 */
#ifndef _UTIME_H
#define _UTIME_H

#include <time.h>  /* time_t */

#ifdef __cplusplus
extern "C" {
#endif

struct utimbuf {
    time_t actime;   /* access time */
    time_t modtime;  /* modification time */
};

#ifndef UTIME_NOW
#define UTIME_NOW  ((long)1073741823)   /* (1<<30) - 1 */
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((long)1073741822)   /* (1<<30) - 2 */
#endif

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif /* _UTIME_H */
