// LikeOS-64 input event UAPI - /dev/input/eventN interface
//
// Event structure, event types/codes and ioctl encoding follow the de-facto
// standard evdev interface so existing input drivers (xf86-input-evdev) can
// be ported against it unchanged.  Mirrored into the userspace libc include
// tree; keep both copies in sync.

#ifndef _KERNEL_UAPI_INPUT_H_
#define _KERNEL_UAPI_INPUT_H_

#ifdef __LIKEOS__
#include <kernel/uapi/types.h>
#else
#include <stdint.h>
#include <sys/time.h>
#endif

// The event structure returned by read().  24 bytes on 64-bit.
struct input_event {
#ifdef __LIKEOS__
	struct {
		int64_t tv_sec;
		int64_t tv_usec;
	} time;
#else
	struct timeval time;
#endif
	uint16_t type; // EV_*
	uint16_t code; // KEY_* / REL_* / SYN_*
	int32_t value; // 1=press 0=release 2=autorepeat, or relative delta
};

#define EV_VERSION 0x010001

struct input_id {
	uint16_t bustype; // BUS_*
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};

// Bus types
#define BUS_PCI 0x01
#define BUS_USB 0x03
#define BUS_HOST 0x19
#define BUS_I8042 0x11

// Event types
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_MAX 0x1f
#define EV_CNT (EV_MAX + 1)

// Synchronization events
#define SYN_REPORT 0
#define SYN_CONFIG 1
#define SYN_DROPPED 3
#define SYN_MAX 0xf
#define SYN_CNT (SYN_MAX + 1)

// Keys and buttons (standard evdev key code numbering)
#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_I 23
#define KEY_O 24
#define KEY_P 25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_F 33
#define KEY_G 34
#define KEY_H 35
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE 41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z 44
#define KEY_X 45
#define KEY_C 46
#define KEY_V 47
#define KEY_B 48
#define KEY_N 49
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT 56
#define KEY_SPACE 57
#define KEY_CAPSLOCK 58
#define KEY_F1 59
#define KEY_F2 60
#define KEY_F3 61
#define KEY_F4 62
#define KEY_F5 63
#define KEY_F6 64
#define KEY_F7 65
#define KEY_F8 66
#define KEY_F9 67
#define KEY_F10 68
#define KEY_NUMLOCK 69
#define KEY_SCROLLLOCK 70
#define KEY_KP7 71
#define KEY_KP8 72
#define KEY_KP9 73
#define KEY_KPMINUS 74
#define KEY_KP4 75
#define KEY_KP5 76
#define KEY_KP6 77
#define KEY_KPPLUS 78
#define KEY_KP1 79
#define KEY_KP2 80
#define KEY_KP3 81
#define KEY_KP0 82
#define KEY_KPDOT 83
#define KEY_102ND 86
#define KEY_F11 87
#define KEY_F12 88
#define KEY_KPENTER 96
#define KEY_RIGHTCTRL 97
#define KEY_KPSLASH 98
#define KEY_SYSRQ 99
#define KEY_RIGHTALT 100
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_INSERT 110
#define KEY_DELETE 111
#define KEY_KPEQUAL 117
#define KEY_PAUSE 119
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126
#define KEY_COMPOSE 127

#define BTN_MISC 0x100
#define BTN_MOUSE 0x110
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

#define KEY_MAX 0x2ff
#define KEY_CNT (KEY_MAX + 1)

// Relative axes
#define REL_X 0x00
#define REL_Y 0x01
#define REL_Z 0x02
#define REL_HWHEEL 0x06
#define REL_DIAL 0x07
#define REL_WHEEL 0x08
#define REL_MISC 0x09
#define REL_MAX 0x0f
#define REL_CNT (REL_MAX + 1)

// ioctl encoding (standard dir/size/type/nr layout).  Local names are used
// to avoid clashing with any host toolchain definitions.
#define _INPUT_IOC_NRBITS 8
#define _INPUT_IOC_TYPEBITS 8
#define _INPUT_IOC_SIZEBITS 14
#define _INPUT_IOC_DIRBITS 2

#define _INPUT_IOC_NRSHIFT 0
#define _INPUT_IOC_TYPESHIFT 8
#define _INPUT_IOC_SIZESHIFT 16
#define _INPUT_IOC_DIRSHIFT 30

#define _INPUT_IOC_NONE 0U
#define _INPUT_IOC_WRITE 1U
#define _INPUT_IOC_READ 2U

#define _INPUT_IOC(dir, type, nr, size)                       \
	(((dir) << _INPUT_IOC_DIRSHIFT) |                     \
	 ((unsigned long)(type) << _INPUT_IOC_TYPESHIFT) |    \
	 ((unsigned long)(nr) << _INPUT_IOC_NRSHIFT) |        \
	 ((unsigned long)(size) << _INPUT_IOC_SIZESHIFT))
#define _INPUT_IOR(type, nr, argtype) \
	_INPUT_IOC(_INPUT_IOC_READ, (type), (nr), sizeof(argtype))
#define _INPUT_IOW(type, nr, argtype) \
	_INPUT_IOC(_INPUT_IOC_WRITE, (type), (nr), sizeof(argtype))

// evdev ioctls
#define EVIOCGVERSION _INPUT_IOR('E', 0x01, int) // get driver version
#define EVIOCGID _INPUT_IOR('E', 0x02, struct input_id) // get device ID
#define EVIOCGNAME(len) \
	_INPUT_IOC(_INPUT_IOC_READ, 'E', 0x06, (len)) // get device name
#define EVIOCGKEY(len) \
	_INPUT_IOC(_INPUT_IOC_READ, 'E', 0x18, (len)) // get key state bitmap
#define EVIOCGBIT(ev, len) \
	_INPUT_IOC(_INPUT_IOC_READ, 'E', 0x20 + (ev), (len)) // get event bits
#define EVIOCGRAB _INPUT_IOW('E', 0x90, int) // grab/release device

#endif // _KERNEL_UAPI_INPUT_H_
