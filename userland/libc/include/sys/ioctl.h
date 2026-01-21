#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

int ioctl(int fd, unsigned long request, void* argp);

#endif
