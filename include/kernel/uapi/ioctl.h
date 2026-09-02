// LikeOS-64 -- ioctl request encoding (the conventional layout).
//
//   bits  0..7   number
//   bits  8..15  type (a driver's magic character)
//   bits 16..29  argument size
//   bits 30..31  direction: 0 none, 1 write (user -> kernel), 2 read
//
// Identical to the libc's <sys/ioctl.h>, so a request built on either side
// decodes on the other.
#ifndef KERNEL_UAPI_IOCTL_H
#define KERNEL_UAPI_IOCTL_H

#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2

#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U

#define _IOC(dir, type, nr, size)                                    \
	(((unsigned long)(dir) << _IOC_DIRSHIFT) |                   \
	 ((unsigned long)(type) << _IOC_TYPESHIFT) |                 \
	 ((unsigned long)(nr) << _IOC_NRSHIFT) |                     \
	 ((unsigned long)(size) << _IOC_SIZESHIFT))

#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, argtype) _IOC(_IOC_READ, (type), (nr), sizeof(argtype))
#define _IOW(type, nr, argtype) _IOC(_IOC_WRITE, (type), (nr), sizeof(argtype))
#define _IOWR(type, nr, argtype) \
	_IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(argtype))

#define _IOC_DIR(nr) (((nr) >> _IOC_DIRSHIFT) & ((1 << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & ((1 << _IOC_TYPEBITS) - 1))
#define _IOC_NR(nr) (((nr) >> _IOC_NRSHIFT) & ((1 << _IOC_NRBITS) - 1))
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & ((1 << _IOC_SIZEBITS) - 1))

#endif
