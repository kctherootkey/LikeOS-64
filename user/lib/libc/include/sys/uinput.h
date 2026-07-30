// LikeOS-64 uinput UAPI - the interface for creating an input device from
// userspace, as opposed to reading one through <sys/input.h>.
//
// A program opens /dev/uinput, describes a device with the UI_SET_* and
// UI_DEV_SETUP ioctls, calls UI_DEV_CREATE, and thereafter anything it write()s
// in struct input_event form appears on a new /dev/input/eventN as though it
// came from hardware.  That is how on-screen keyboards, remote-input bridges
// and input test harnesses work.
//
// THE KERNEL HERE DOES NOT IMPLEMENT IT.  There is no /dev/uinput node, so
// open() fails with ENOENT and every library built against this header reports
// that cleanly rather than failing to build.  The header exists because the ABI
// it describes is fixed and widely depended on: libevdev's uinput half compiles
// against it and keeps its full API, and libevdev_uinput_create_from_device()
// simply returns -ENOENT at runtime.
//
// If /dev/uinput is ever implemented, nothing here changes and nothing built
// against it needs rebuilding — the definitions are already the real ones.
#ifndef _SYS_UINPUT_H
#define _SYS_UINPUT_H

#include <stdint.h>
#include <sys/input.h>

#define UINPUT_VERSION       5
#define UINPUT_MAX_NAME_SIZE 80

// Device description for UI_DEV_SETUP.  Supersedes writing a struct
// uinput_user_dev to the file descriptor, which is the older way of saying the
// same thing and is kept below for programs that still do it.
struct uinput_setup {
	struct input_id id;
	char name[UINPUT_MAX_NAME_SIZE];
	uint32_t ff_effects_max;
};

// One absolute axis, for UI_ABS_SETUP.  Set per axis after UI_DEV_SETUP and
// before UI_DEV_CREATE.
struct uinput_abs_setup {
	uint16_t code;
	struct input_absinfo absinfo;
};

// The original setup structure, written to the descriptor rather than passed
// through an ioctl.  Present because programs written before UI_DEV_SETUP
// existed still use it, and it is part of the ABI.
struct uinput_user_dev {
	char name[UINPUT_MAX_NAME_SIZE];
	struct input_id id;
	uint32_t ff_effects_max;
	int32_t absmax[ABS_CNT];
	int32_t absmin[ABS_CNT];
	int32_t absfuzz[ABS_CNT];
	int32_t absflat[ABS_CNT];
};

// ioctl encoding, sharing <sys/input.h>'s local macro names so the two headers
// cannot disagree about the layout.
#define UINPUT_IOCTL_BASE 'U'

#define UI_DEV_CREATE  _INPUT_IOC(_INPUT_IOC_NONE, UINPUT_IOCTL_BASE, 1, 0)
#define UI_DEV_DESTROY _INPUT_IOC(_INPUT_IOC_NONE, UINPUT_IOCTL_BASE, 2, 0)
#define UI_DEV_SETUP   _INPUT_IOW(UINPUT_IOCTL_BASE, 3, struct uinput_setup)
#define UI_ABS_SETUP   _INPUT_IOW(UINPUT_IOCTL_BASE, 4, struct uinput_abs_setup)

// Declaring which codes of each kind the device will emit.  Called once per
// code before UI_DEV_CREATE; the argument is the code itself, so the whole
// capability set is built up one ioctl at a time.
#define UI_SET_EVBIT   _INPUT_IOW(UINPUT_IOCTL_BASE, 100, int)
#define UI_SET_KEYBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 101, int)
#define UI_SET_RELBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 102, int)
#define UI_SET_ABSBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 103, int)
#define UI_SET_MSCBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 104, int)
#define UI_SET_LEDBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 105, int)
#define UI_SET_SNDBIT  _INPUT_IOW(UINPUT_IOCTL_BASE, 106, int)
#define UI_SET_FFBIT   _INPUT_IOW(UINPUT_IOCTL_BASE, 107, int)
#define UI_SET_PHYS    _INPUT_IOW(UINPUT_IOCTL_BASE, 108, char *)
#define UI_SET_SWBIT   _INPUT_IOW(UINPUT_IOCTL_BASE, 109, int)
#define UI_SET_PROPBIT _INPUT_IOW(UINPUT_IOCTL_BASE, 110, int)

// Reading back what was created.  The length is part of the request, as with
// EVIOCGNAME, because the caller owns the buffer.
#define UI_GET_SYSNAME(len) \
	_INPUT_IOC(_INPUT_IOC_READ, UINPUT_IOCTL_BASE, 44, (len))
#define UI_GET_VERSION _INPUT_IOR(UINPUT_IOCTL_BASE, 45, unsigned int)

#endif // _SYS_UINPUT_H
