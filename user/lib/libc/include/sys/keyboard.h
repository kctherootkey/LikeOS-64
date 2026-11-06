// LikeOS console keyboard UAPI - key codes, key symbols and keymap limits
//
// The console turns key codes into characters through a keymap: one table of
// NR_KEYS entries per modifier combination, each entry a 16-bit key symbol.
// This header gives the symbol encoding, the symbol types and their named
// values, and the table limits.  It is the established console keymap ABI, so
// kbd's loadkeys and dumpkeys work against it unchanged.  The ioctls that read
// and write the tables are in <kernel/uapi/kd.h>.
//
// A key symbol is K(type, value) for the classic types below (type <
// NR_TYPES).  Any other 16-bit value is a Unicode character: the code point
// XOR 0xF000 (so U+00FC is 0xF0FC and U+20AC is 0xD0AC), which keeps every
// Unicode symbol out of the classic range.
//
// Mirrored into the userspace libc include tree as <sys/keyboard.h>; keep
// both copies in sync.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_KEYBOARD_H_
#define _KERNEL_UAPI_KEYBOARD_H_

#ifdef __cplusplus
extern "C" {
#endif

// ---- Modifier bits: a keymap index is a set of these -----------------------
#define KG_SHIFT 0
#define KG_CTRL 2
#define KG_ALT 3
#define KG_ALTGR 1
#define KG_SHIFTL 4
#define KG_KANASHIFT 4
#define KG_SHIFTR 5
#define KG_CTRLL 6
#define KG_CTRLR 7
#define KG_CAPSSHIFT 8

#define NR_SHIFT 9

#define NR_KEYS 256 // key codes 0..255 per keymap
#define MAX_NR_KEYMAPS 256 // one keymap per modifier combination 0..255
// One table may only be allocated by a privileged caller once this many
// exist; nothing smaller than MAX_NR_KEYMAPS is enforced here.
#define MAX_NR_OF_USER_KEYMAPS 256
#define MAX_NR_FUNC 256 // function key strings

// ---- Symbol encoding --------------------------------------------------------
#define K(t, v) (((t) << 8) | (v))
#define KTYP(x) ((x) >> 8)
#define KVAL(x) ((x) & 0xff)

// ---- Symbol types -----------------------------------------------------------
#define KT_LATIN 0 // a character (Latin-1 range)
#define KT_FN 1 // a function key: sends its string
#define KT_SPEC 2 // a special action (Enter, Caps_Lock, Compose, ...)
#define KT_PAD 3 // a keypad key
#define KT_DEAD 4 // a dead key (diacritic)
#define KT_CONS 5 // switch to console n
#define KT_CUR 6 // a cursor key
#define KT_SHIFT 7 // a modifier key
#define KT_META 8 // a character with the Meta (Alt) bit
#define KT_ASCII 9 // a digit of a character typed by its number
#define KT_LOCK 10 // a modifier lock (toggles)
#define KT_LETTER 11 // a letter: affected by Caps Lock
#define KT_SLOCK 12 // a sticky modifier (applies to the next key)
#define KT_DEAD2 13 // a dead key whose value is the diacritic itself
#define KT_BRL 14 // a Braille dot key

#define NR_TYPES 15

// ---- KT_FN ------------------------------------------------------------------
#define K_F1 K(KT_FN, 0)
#define K_F2 K(KT_FN, 1)
#define K_F3 K(KT_FN, 2)
#define K_F4 K(KT_FN, 3)
#define K_F5 K(KT_FN, 4)
#define K_F6 K(KT_FN, 5)
#define K_F7 K(KT_FN, 6)
#define K_F8 K(KT_FN, 7)
#define K_F9 K(KT_FN, 8)
#define K_F10 K(KT_FN, 9)
#define K_F11 K(KT_FN, 10)
#define K_F12 K(KT_FN, 11)
#define K_F13 K(KT_FN, 12)
#define K_F14 K(KT_FN, 13)
#define K_F15 K(KT_FN, 14)
#define K_F16 K(KT_FN, 15)
#define K_F17 K(KT_FN, 16)
#define K_F18 K(KT_FN, 17)
#define K_F19 K(KT_FN, 18)
#define K_F20 K(KT_FN, 19)
#define K_FIND K(KT_FN, 20)
#define K_INSERT K(KT_FN, 21)
#define K_REMOVE K(KT_FN, 22)
#define K_SELECT K(KT_FN, 23)
#define K_PGUP K(KT_FN, 24) // Prior
#define K_PGDN K(KT_FN, 25) // Next
#define K_MACRO K(KT_FN, 26)
#define K_HELP K(KT_FN, 27)
#define K_DO K(KT_FN, 28)
#define K_PAUSE K(KT_FN, 29)
// F21..F245 follow at values 30..254
#define K_F21 K(KT_FN, 30)
#define K_F245 K(KT_FN, 254)
#define K_UNDO K(KT_FN, 255)

// ---- KT_SPEC ----------------------------------------------------------------
#define K_HOLE K(KT_SPEC, 0)
#define K_ENTER K(KT_SPEC, 1)
#define K_SH_REGS K(KT_SPEC, 2)
#define K_SH_MEM K(KT_SPEC, 3)
#define K_SH_STAT K(KT_SPEC, 4)
#define K_BREAK K(KT_SPEC, 5)
#define K_CONS K(KT_SPEC, 6)
#define K_CAPS K(KT_SPEC, 7)
#define K_NUM K(KT_SPEC, 8)
#define K_HOLD K(KT_SPEC, 9)
#define K_SCROLLFORW K(KT_SPEC, 10)
#define K_SCROLLBACK K(KT_SPEC, 11)
#define K_BOOT K(KT_SPEC, 12)
#define K_CAPSON K(KT_SPEC, 13)
#define K_COMPOSE K(KT_SPEC, 14)
#define K_SAK K(KT_SPEC, 15)
#define K_DECRCONSOLE K(KT_SPEC, 16)
#define K_INCRCONSOLE K(KT_SPEC, 17)
#define K_SPAWNCONSOLE K(KT_SPEC, 18)
#define K_BARENUMLOCK K(KT_SPEC, 19)

#define K_ALLOCATED K(KT_SPEC, 126) // entry 0 of an allocated keymap
#define K_NOSUCHMAP K(KT_SPEC, 127) // KDGKBENT: that keymap does not exist

// ---- KT_PAD -----------------------------------------------------------------
#define K_P0 K(KT_PAD, 0)
#define K_P1 K(KT_PAD, 1)
#define K_P2 K(KT_PAD, 2)
#define K_P3 K(KT_PAD, 3)
#define K_P4 K(KT_PAD, 4)
#define K_P5 K(KT_PAD, 5)
#define K_P6 K(KT_PAD, 6)
#define K_P7 K(KT_PAD, 7)
#define K_P8 K(KT_PAD, 8)
#define K_P9 K(KT_PAD, 9)
#define K_PPLUS K(KT_PAD, 10)
#define K_PMINUS K(KT_PAD, 11)
#define K_PSTAR K(KT_PAD, 12)
#define K_PSLASH K(KT_PAD, 13)
#define K_PENTER K(KT_PAD, 14)
#define K_PCOMMA K(KT_PAD, 15)
#define K_PDOT K(KT_PAD, 16)
#define K_PPLUSMINUS K(KT_PAD, 17)
#define K_PPARENL K(KT_PAD, 18)
#define K_PPARENR K(KT_PAD, 19)

#define NR_PAD 20

// ---- KT_DEAD ----------------------------------------------------------------
#define K_DGRAVE K(KT_DEAD, 0)
#define K_DACUTE K(KT_DEAD, 1)
#define K_DCIRCM K(KT_DEAD, 2)
#define K_DTILDE K(KT_DEAD, 3)
#define K_DDIERE K(KT_DEAD, 4)
#define K_DCEDIL K(KT_DEAD, 5)
#define K_DMACRON K(KT_DEAD, 6)
#define K_DBREVE K(KT_DEAD, 7)
#define K_DABDOT K(KT_DEAD, 8)
#define K_DABRING K(KT_DEAD, 9)
#define K_DDBACUTE K(KT_DEAD, 10)
#define K_DCARON K(KT_DEAD, 11)
#define K_DOGONEK K(KT_DEAD, 12)
#define K_DIOTA K(KT_DEAD, 13)
#define K_DVOICED K(KT_DEAD, 14)
#define K_DSEMVOICED K(KT_DEAD, 15)
#define K_DBEDOT K(KT_DEAD, 16)
#define K_DHOOK K(KT_DEAD, 17)
#define K_DHORN K(KT_DEAD, 18)
#define K_DSTROKE K(KT_DEAD, 19)
#define K_DABCOMMA K(KT_DEAD, 20)
#define K_DABREVCOMMA K(KT_DEAD, 21)
#define K_DDBGRAVE K(KT_DEAD, 22)
#define K_DINVBREVE K(KT_DEAD, 23)
#define K_DBECOMMA K(KT_DEAD, 24)
#define K_DCURRENCY K(KT_DEAD, 25)
#define K_DGREEK K(KT_DEAD, 26)

#define NR_DEAD 27

// ---- KT_CUR -----------------------------------------------------------------
#define K_DOWN K(KT_CUR, 0)
#define K_LEFT K(KT_CUR, 1)
#define K_RIGHT K(KT_CUR, 2)
#define K_UP K(KT_CUR, 3)

// ---- KT_SHIFT ---------------------------------------------------------------
#define K_SHIFT K(KT_SHIFT, KG_SHIFT)
#define K_CTRL K(KT_SHIFT, KG_CTRL)
#define K_ALT K(KT_SHIFT, KG_ALT)
#define K_ALTGR K(KT_SHIFT, KG_ALTGR)
#define K_SHIFTL K(KT_SHIFT, KG_SHIFTL)
#define K_SHIFTR K(KT_SHIFT, KG_SHIFTR)
#define K_CTRLL K(KT_SHIFT, KG_CTRLL)
#define K_CTRLR K(KT_SHIFT, KG_CTRLR)
#define K_CAPSSHIFT K(KT_SHIFT, KG_CAPSSHIFT)

// ---- KT_ASCII ---------------------------------------------------------------
#define K_ASC0 K(KT_ASCII, 0)
#define K_ASC1 K(KT_ASCII, 1)
#define K_ASC2 K(KT_ASCII, 2)
#define K_ASC3 K(KT_ASCII, 3)
#define K_ASC4 K(KT_ASCII, 4)
#define K_ASC5 K(KT_ASCII, 5)
#define K_ASC6 K(KT_ASCII, 6)
#define K_ASC7 K(KT_ASCII, 7)
#define K_ASC8 K(KT_ASCII, 8)
#define K_ASC9 K(KT_ASCII, 9)
#define K_HEX0 K(KT_ASCII, 10)
#define K_HEX1 K(KT_ASCII, 11)
#define K_HEX2 K(KT_ASCII, 12)
#define K_HEX3 K(KT_ASCII, 13)
#define K_HEX4 K(KT_ASCII, 14)
#define K_HEX5 K(KT_ASCII, 15)
#define K_HEX6 K(KT_ASCII, 16)
#define K_HEX7 K(KT_ASCII, 17)
#define K_HEX8 K(KT_ASCII, 18)
#define K_HEX9 K(KT_ASCII, 19)
#define K_HEXa K(KT_ASCII, 20)
#define K_HEXb K(KT_ASCII, 21)
#define K_HEXc K(KT_ASCII, 22)
#define K_HEXd K(KT_ASCII, 23)
#define K_HEXe K(KT_ASCII, 24)
#define K_HEXf K(KT_ASCII, 25)

#define NR_ASCII 26

// ---- KT_LOCK / KT_SLOCK -----------------------------------------------------
#define K_SHIFTLOCK K(KT_LOCK, KG_SHIFT)
#define K_CTRLLOCK K(KT_LOCK, KG_CTRL)
#define K_ALTLOCK K(KT_LOCK, KG_ALT)
#define K_ALTGRLOCK K(KT_LOCK, KG_ALTGR)
#define K_SHIFTLLOCK K(KT_LOCK, KG_SHIFTL)
#define K_SHIFTRLOCK K(KT_LOCK, KG_SHIFTR)
#define K_CTRLLLOCK K(KT_LOCK, KG_CTRLL)
#define K_CTRLRLOCK K(KT_LOCK, KG_CTRLR)
#define K_CAPSSHIFTLOCK K(KT_LOCK, KG_CAPSSHIFT)

#define K_SHIFT_SLOCK K(KT_SLOCK, KG_SHIFT)
#define K_CTRL_SLOCK K(KT_SLOCK, KG_CTRL)
#define K_ALT_SLOCK K(KT_SLOCK, KG_ALT)
#define K_ALTGR_SLOCK K(KT_SLOCK, KG_ALTGR)
#define K_SHIFTL_SLOCK K(KT_SLOCK, KG_SHIFTL)
#define K_SHIFTR_SLOCK K(KT_SLOCK, KG_SHIFTR)
#define K_CTRLL_SLOCK K(KT_SLOCK, KG_CTRLL)
#define K_CTRLR_SLOCK K(KT_SLOCK, KG_CTRLR)
#define K_CAPSSHIFT_SLOCK K(KT_SLOCK, KG_CAPSSHIFT)

// Lock state is eight bits wide: KG_SHIFT .. KG_CTRLR.
#define NR_LOCK 8

// ---- KT_BRL -----------------------------------------------------------------
#define K_BRL_BLANK K(KT_BRL, 0)
#define K_BRL_DOT1 K(KT_BRL, 1)
#define K_BRL_DOT2 K(KT_BRL, 2)
#define K_BRL_DOT3 K(KT_BRL, 3)
#define K_BRL_DOT4 K(KT_BRL, 4)
#define K_BRL_DOT5 K(KT_BRL, 5)
#define K_BRL_DOT6 K(KT_BRL, 6)
#define K_BRL_DOT7 K(KT_BRL, 7)
#define K_BRL_DOT8 K(KT_BRL, 8)
#define K_BRL_DOT9 K(KT_BRL, 9)
#define K_BRL_DOT10 K(KT_BRL, 10)

#define NR_BRL 11

// Accent (dead key / compose) table size.
#define MAX_DIACR 256

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_KEYBOARD_H_
