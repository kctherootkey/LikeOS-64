/*
 * sys/param.h - system parameters for LikeOS
 */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

#ifndef MIN
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#endif

#define MAXPATHLEN  4096
#define MAXHOSTNAMELEN 256
#define PAGE_SIZE   4096
/* BSD spelling of OPEN_MAX; kept in step with it rather than repeated. */
#define NOFILE      OPEN_MAX

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PARAM_H */
