// LikeOS console keyboard UAPI - the KD* ioctls of the console terminal
//
// Requests and structures of the established console keyboard interface:
// the keyboard type, the keyboard mode (translated, Unicode, raw ...), the
// keymap entries, the function key strings, the accent table, the Meta mode
// and the lock LEDs.  kbd's loadkeys, dumpkeys, kbd_mode, setleds, showkey
// and setmetamode use exactly these.  Key symbols and table limits are in
// <kernel/uapi/keyboard.h>.
//
// Only the console terminal answers them; any other terminal (a pty) answers
// ENOTTY, which is how a program finds out whether it holds the console.
// Requests that change state are allowed to root and to a process whose
// controlling terminal is the console; the read requests to anyone who has
// the console open.
//
// Mirrored into the userspace libc include tree as <sys/kd.h>; keep both
// copies in sync.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_KD_H_
#define _KERNEL_UAPI_KD_H_

#ifdef __cplusplus
extern "C" {
#endif

// ---- Lock LEDs --------------------------------------------------------------
#define KDGETLED 0x4B31 // char *: LEDs currently lit (LED_*)
#define KDSETLED 0x4B32 // value: light these LEDs; > 7 = show the locks again
#define LED_SCR 0x01 // Scroll Lock
#define LED_NUM 0x02 // Num Lock
#define LED_CAP 0x04 // Caps Lock

// ---- Keyboard type ----------------------------------------------------------
#define KDGKBTYPE 0x4B33 // char *: KB_*
#define KB_84 0x01
#define KB_101 0x02 // what the console reports
#define KB_OTHER 0x03

// ---- Console display mode ---------------------------------------------------
#define KDSETMODE 0x4B3A // value: KD_TEXT / KD_GRAPHICS
#define KDGETMODE 0x4B3B // int *
#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01
#define KD_TEXT0 0x02 // obsolete
#define KD_TEXT1 0x03 // obsolete

// ---- Keyboard mode ----------------------------------------------------------
#define KDGKBMODE 0x4B44 // int *: K_*
#define KDSKBMODE 0x4B45 // value: K_*
#define K_RAW 0x00 // scancodes, as the keyboard sends them
#define K_XLATE 0x01 // characters, 8-bit
#define K_MEDIUMRAW 0x02 // key codes (bit 7 = release)
#define K_UNICODE 0x03 // characters, UTF-8 (the console's default)
#define K_OFF 0x04 // nothing

// ---- Meta (Alt) handling ----------------------------------------------------
#define KDGKBMETA 0x4B62 // int *
#define KDSKBMETA 0x4B63 // value
#define K_METABIT 0x03 // Meta sets bit 7 of the character
#define K_ESCPREFIX 0x04 // Meta sends ESC before the character (default)

// ---- Lock flags (what the LEDs show unless KDSETLED overrides them) --------
#define KDGKBLED 0x4B64 // char *: flags in bits 0-2, defaults in bits 4-6
#define KDSKBLED 0x4B65 // value: the same layout
#define K_SCROLLLOCK 0x01
#define K_NUMLOCK 0x02
#define K_CAPSLOCK 0x04

// ---- Keymap entries ---------------------------------------------------------
struct kbentry {
	unsigned char kb_table; // keymap: the modifier combination
	unsigned char kb_index; // key code
	unsigned short kb_value; // key symbol
};
#define KDGKBENT 0x4B46
#define KDSKBENT 0x4B47

// ---- Function key strings ---------------------------------------------------
struct kbsentry {
	unsigned char kb_func; // KVAL of the KT_FN symbol
	unsigned char kb_string[512]; // NUL-terminated
};
#define KDGKBSENT 0x4B48
#define KDSKBSENT 0x4B49

// ---- Accent table (dead keys and Compose) -----------------------------------
struct kbdiacr {
	unsigned char diacr, base, result;
};
struct kbdiacrs {
	unsigned int kb_cnt; // entries used
	struct kbdiacr kbdiacr[256];
};
#define KDGKBDIACR 0x4B4A
#define KDSKBDIACR 0x4B4B

struct kbdiacruc {
	unsigned int diacr, base, result; // Unicode code points
};
struct kbdiacrsuc {
	unsigned int kb_cnt;
	struct kbdiacruc kbdiacruc[256];
};
#define KDGKBDIACRUC 0x4BFA
#define KDSKBDIACRUC 0x4BFB

// ---- Scancode to key code table ---------------------------------------------
// scancode: the set-1 make code, plus 128 when it follows the 0xE0 prefix.
struct kbkeycode {
	unsigned int scancode, keycode;
};
#define KDGETKEYCODE 0x4B4C
#define KDSETKEYCODE 0x4B4D

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_KD_H_
