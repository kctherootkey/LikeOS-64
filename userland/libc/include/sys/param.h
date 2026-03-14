/*
 * sys/param.h - system parameters for LikeOS
 */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

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
#define NOFILE      256

#endif /* _SYS_PARAM_H */
