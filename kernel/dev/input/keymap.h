// LikeOS console keymap engine
//
// Turns key events (key code + press/release) into what the console terminal
// receives: characters as UTF-8 (or 8-bit), function key strings, cursor
// sequences, and the console actions some keys stand for.  The tables it
// works from -- one keymap per modifier combination, the function key
// strings, the accent table -- are the ones the KD* ioctls of
// <kernel/uapi/kd.h> read and write, so a keymap loaded with loadkeys is
// exactly what this engine runs.
//
// The engine is self-contained on purpose: it allocates nothing, takes no
// lock and calls nothing outside this file, so the same source is compiled
// into the kernel and into the host test (host/test-keymap.sh).  The caller
// (keyboard.c) serialises it, supplies any memory a table change needs, and
// delivers struct keymap_out to the terminal after dropping its lock.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_INPUT_KEYMAP_H_
#define _KERNEL_DEV_INPUT_KEYMAP_H_

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif
#include <kernel/uapi/keyboard.h>
#include <kernel/uapi/kd.h>

// Bytes one scancode can produce: a function key string (at most 511) plus
// a pending accent or a character typed by number, with room to spare.  It
// lives on the interrupt's stack, so no larger than that.
#define KEYMAP_OUT_MAX 640

// Console actions a key can ask for.  They are reported rather than done so
// that they run outside the caller's lock (a task dump takes a while).
#define KEYMAP_ACT_SHOW_REGS 0x01u
#define KEYMAP_ACT_SHOW_MEM 0x02u
#define KEYMAP_ACT_SHOW_STATE 0x04u
#define KEYMAP_ACT_BREAK 0x08u // interrupt the foreground job
#define KEYMAP_ACT_SCROLL_BACK 0x10u // scrollback: half a screen up
#define KEYMAP_ACT_SCROLL_FWD 0x20u // scrollback: half a screen down

struct keymap_out {
	unsigned len;
	unsigned actions; // KEYMAP_ACT_*
	int raw; // keyboard-mode bytes (K_MEDIUMRAW): no line discipline
	unsigned char buf[KEYMAP_OUT_MAX];
};

struct keymap_state {
	// The tables.  maps[t][k] is the symbol of key code k in the keymap of
	// modifier combination t, in the ioctl encoding (K(type, value), or a
	// Unicode code point XOR 0xF000).  A NULL keymap does not exist.
	uint16_t *maps[MAX_NR_KEYMAPS];
	unsigned nmaps;
	const char *func[MAX_NR_FUNC]; // NULL = no string
	uint8_t func_owned[MAX_NR_FUNC / 8]; // set: func[i] came from the caller
	struct kbdiacruc accent[MAX_DIACR];
	unsigned naccent;

	// Modes.
	uint8_t kbdmode; // K_RAW .. K_OFF
	uint8_t metamode; // K_METABIT / K_ESCPREFIX
	uint8_t kdmode; // KD_TEXT / KD_GRAPHICS (recorded only)
	uint8_t ledflags; // K_SCROLLLOCK | K_NUMLOCK | K_CAPSLOCK
	uint8_t default_ledflags;
	int16_t led_override; // -1: LEDs show the lock flags; else LED_* bits

	// Modifier state.
	uint16_t shift_state; // KG_* bits of the modifier keys held
	uint8_t lockstate; // KT_LOCK toggles
	uint8_t slockstate; // KT_SLOCK: applies to the next key only
	uint8_t shift_down[NR_SHIFT];

	// Composition state.
	uint32_t diacr; // pending accent or Compose character, 0 = none
	uint8_t diacr_dead; // the pending accent came from a dead key
	uint8_t dead_key_next; // Compose: the next character becomes diacr
	uint8_t npadch_active; // a character is being typed by its number
	uint32_t npadch_value;
	uint16_t brl_pressed, brl_committing;

	uint8_t key_down[NR_KEYS / 8];

	// Set-1 scancodes -> key codes (KDGETKEYCODE / KDSETKEYCODE), and the
	// decoder's prefix state.
	uint8_t kc_plain[128];
	uint8_t kc_e0[128];
	uint8_t prefix; // 0, 0xE0 or 0xE1
	uint8_t e1_count;
	uint8_t e1_first;
};

// The compiled-in keymap, function strings and accent table, and the
// default modes.  Called once on a zeroed state.  Any table a previous
// configuration allocated must have been released with keymap_release().
void keymap_init(struct keymap_state *st);

// Hand back everything the caller supplied (keymaps, strings) through
// `free_fn`, leaving the state as keymap_init() found it.  Host test only.
void keymap_release(struct keymap_state *st, void (*free_fn)(void *));

// One byte of the set-1 scancode stream: the key events it completes (0..2)
// as key codes in kc[] with press (1) / release (0) in down[].
int keymap_decode(struct keymap_state *st, uint8_t sc, unsigned kc[2],
		  int down[2]);

// The scancode -> key code table.  scancode: the make code, plus 128 after
// the 0xE0 prefix.
int keymap_get_keycode(const struct keymap_state *st, unsigned scancode,
		       unsigned *keycode);
int keymap_set_keycode(struct keymap_state *st, unsigned scancode,
		       unsigned keycode);

// One key event.  `out` must be zeroed (len = 0, actions = 0) by the caller;
// the event appends to it.
void keymap_key(struct keymap_state *st, unsigned keycode, int down,
		struct keymap_out *out);

// Record a key event without acting on it (a key the caller consumed).
void keymap_note_key(struct keymap_state *st, unsigned keycode, int down);

// The character key code `keycode' would type now (no state change), or -1
// when it types none.  For the console's own hotkeys.
int32_t keymap_peek_char(const struct keymap_state *st, unsigned keycode);

// Forget held keys and every transient state (pending accent, sticky
// modifiers, a half-received scancode sequence); the locks and the tables
// stay.
void keymap_reset_keys(struct keymap_state *st);

// Which modifiers are physically held (for the console's hotkeys).
int keymap_ctrl_held(const struct keymap_state *st);
int keymap_alt_held(const struct keymap_state *st);

// The LEDs to light (LED_SCR | LED_NUM | LED_CAP).
unsigned keymap_leds(const struct keymap_state *st);

// ---- Table and mode access for the ioctls ---------------------------------
// All return 0 or a negative errno.

int keymap_get_entry(const struct keymap_state *st, unsigned table,
		     unsigned index, uint16_t *value);
// Allocating a keymap takes `*spare' (NR_KEYS uint16_t from the caller; set
// to NULL when taken).  A keymap that is deallocated is returned through
// `*freed' for the caller to release (NULL otherwise).  -ENOMEM: a keymap
// is needed and no spare was given; supply one and call again.
int keymap_set_entry(struct keymap_state *st, unsigned table, unsigned index,
		     unsigned value, uint16_t **spare, uint16_t **freed);
int keymap_table_exists(const struct keymap_state *st, unsigned table);

// Copy function string `idx' (NUL-terminated, at most `cap' - 1 bytes).
void keymap_get_func(const struct keymap_state *st, unsigned idx, char *buf,
		     unsigned cap);
// Install `s' (caller-owned, or NULL for none) as function string `idx'.
// Returns the previous caller-owned string for the caller to release, or
// NULL.
char *keymap_set_func(struct keymap_state *st, unsigned idx, char *s);

unsigned keymap_get_accents(const struct keymap_state *st,
			    struct kbdiacruc *out, unsigned cap);
int keymap_set_accents(struct keymap_state *st, const struct kbdiacruc *in,
		       unsigned count);

int keymap_set_kbdmode(struct keymap_state *st, unsigned mode);
int keymap_set_metamode(struct keymap_state *st, unsigned mode);
// KDSKBLED: flags in bits 0-2, default flags in bits 4-6.
int keymap_set_ledflags(struct keymap_state *st, unsigned arg);
unsigned keymap_get_ledflags(const struct keymap_state *st);
// KDSETLED: light exactly these LEDs; a value above 7 returns the LEDs to
// showing the lock flags.
void keymap_set_led_override(struct keymap_state *st, unsigned leds);

// Latin-1 conversions of the 8-bit accent table interface (KDGKBDIACR):
// characters outside Latin-1 do not convert (-1).
int32_t keymap_uni_to_8bit(uint32_t cp);

#endif // _KERNEL_DEV_INPUT_KEYMAP_H_
