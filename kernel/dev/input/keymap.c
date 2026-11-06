// LikeOS console keymap engine
//
// See keymap.h for what this is and how it is called.  In short: a key code
// and the modifier state select one symbol out of the keymap tables, and the
// symbol's type decides what the console terminal receives.
//
//   shift_final = (modifiers held | lock toggles) ^ sticky modifiers
//   symbol      = maps[shift_final][keycode]
//
// A missing keymap for a combination means the key does nothing in it (the
// held-modifier record is then rebuilt from the keys actually down, which is
// what recovers from a modifier release that was never seen).
//
// The compiled-in tables reproduce the console's keyboard as it was before
// keymaps could be loaded: the US layout, Caps Lock as a lock of the Shift
// level, Ctrl+letter as the control character, Alt as an ESC prefix, the
// xterm-style sequences for the cursor block and F1-F12.  A keymap loaded
// with loadkeys replaces any of it.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/input/keymap.h>
#ifdef __LIKEOS_KERNEL__
#include <kernel/ke/syscall.h> // errno values
#else
#include <errno.h>
#endif

#include <kernel/dev/input/keymap_compose.h>

#define BRL_UC_ROW 0x2800u
#define UNICODE_MASK 0xf000u

#define SHIFT_KEYS_MASK \
	((1u << KG_SHIFT) | (1u << KG_SHIFTL) | (1u << KG_SHIFTR))
#define CTRL_KEYS_MASK ((1u << KG_CTRL) | (1u << KG_CTRLL) | (1u << KG_CTRLR))
#define ALT_KEYS_MASK (1u << KG_ALT)

// ---------------------------------------------------------------------------
// Small helpers (no library underneath: this file is built for the host too)
// ---------------------------------------------------------------------------

static void km_zero(void *p, size_t n)
{
	unsigned char *b = p;

	while (n--)
		*b++ = 0;
}

static void km_copy(void *d, const void *s, size_t n)
{
	unsigned char *db = d;
	const unsigned char *sb = s;

	while (n--)
		*db++ = *sb++;
}

static int is_unicode_sym(unsigned sym)
{
	return KTYP(sym) >= NR_TYPES;
}

static int key_is_down(const struct keymap_state *st, unsigned kc)
{
	return (st->key_down[kc >> 3] >> (kc & 7)) & 1;
}

static void key_mark(struct keymap_state *st, unsigned kc, int down)
{
	if (down)
		st->key_down[kc >> 3] |= (uint8_t)(1u << (kc & 7));
	else
		st->key_down[kc >> 3] &= (uint8_t)~(1u << (kc & 7));
}

static void put_byte(struct keymap_out *o, unsigned char c)
{
	if (o->len < KEYMAP_OUT_MAX)
		o->buf[o->len++] = c;
}

static void put_str(struct keymap_out *o, const char *s)
{
	while (*s)
		put_byte(o, (unsigned char)*s++);
}

static void put_utf8(struct keymap_out *o, uint32_t cp)
{
	if (cp >= 0xd800 && cp <= 0xdfff)
		return;
	if (cp < 0x80) {
		put_byte(o, (unsigned char)cp);
	} else if (cp < 0x800) {
		put_byte(o, (unsigned char)(0xc0 | (cp >> 6)));
		put_byte(o, (unsigned char)(0x80 | (cp & 0x3f)));
	} else if (cp < 0x10000) {
		put_byte(o, (unsigned char)(0xe0 | (cp >> 12)));
		put_byte(o, (unsigned char)(0x80 | ((cp >> 6) & 0x3f)));
		put_byte(o, (unsigned char)(0x80 | (cp & 0x3f)));
	} else if (cp < 0x110000) {
		put_byte(o, (unsigned char)(0xf0 | (cp >> 18)));
		put_byte(o, (unsigned char)(0x80 | ((cp >> 12) & 0x3f)));
		put_byte(o, (unsigned char)(0x80 | ((cp >> 6) & 0x3f)));
		put_byte(o, (unsigned char)(0x80 | (cp & 0x3f)));
	}
}

int32_t keymap_uni_to_8bit(uint32_t cp)
{
	return cp <= 0xff ? (int32_t)cp : -1;
}

// One character to the terminal, in the keyboard mode's encoding: UTF-8, or
// Latin-1 bytes (a character outside Latin-1 has no 8-bit form and is
// dropped).
static void put_char(const struct keymap_state *st, struct keymap_out *o,
		     uint32_t cp)
{
	if (st->kbdmode == K_UNICODE) {
		put_utf8(o, cp);
	} else {
		int32_t c = keymap_uni_to_8bit(cp);

		if (c >= 0)
			put_byte(o, (unsigned char)c);
	}
}

// ---------------------------------------------------------------------------
// Modifier bookkeeping
// ---------------------------------------------------------------------------

// Rebuild the held-modifier record from the keys that are down, as keymap 0
// describes them.
static void compute_shiftstate(struct keymap_state *st)
{
	const uint16_t *map0 = st->maps[0];

	km_zero(st->shift_down, sizeof(st->shift_down));
	st->shift_state = 0;
	if (!map0)
		return;
	for (unsigned kc = 0; kc < NR_KEYS; kc++) {
		unsigned sym, val;

		if (!key_is_down(st, kc))
			continue;
		sym = map0[kc];
		if (is_unicode_sym(sym) ||
		    (KTYP(sym) != KT_SHIFT && KTYP(sym) != KT_SLOCK))
			continue;
		val = KVAL(sym);
		if (val == KG_CAPSSHIFT)
			val = KG_SHIFT;
		if (val >= NR_SHIFT)
			continue;
		st->shift_down[val]++;
		st->shift_state |= (uint16_t)(1u << val);
	}
}

static unsigned shift_final(const struct keymap_state *st)
{
	return ((st->shift_state | st->lockstate) ^ st->slockstate) &
	       (MAX_NR_KEYMAPS - 1);
}

int keymap_ctrl_held(const struct keymap_state *st)
{
	return (st->shift_state & CTRL_KEYS_MASK) != 0;
}

int keymap_alt_held(const struct keymap_state *st)
{
	return (st->shift_state & ALT_KEYS_MASK) != 0;
}

// xterm's modifier parameter for the cursor block: 1 + Shift + 4 * Ctrl, as
// the keys are physically held (a lock does not count).
static unsigned cursor_modifier(const struct keymap_state *st)
{
	unsigned mod = 1;

	if (st->shift_state & SHIFT_KEYS_MASK)
		mod += 1;
	if (st->shift_state & CTRL_KEYS_MASK)
		mod += 4;
	return mod;
}

// ---------------------------------------------------------------------------
// Accents
// ---------------------------------------------------------------------------

// The dead key a diacritic character stands for, or -1.
static int dead_index_of(uint32_t d)
{
	for (unsigned i = 0; i < NR_DEAD; i++)
		if (keymap_dead_char[i] == d)
			return (int)i;
	for (unsigned i = 0;
	     i < sizeof(keymap_spacing_alias) / sizeof(keymap_spacing_alias[0]);
	     i++)
		if (keymap_spacing_alias[i].cp == d)
			return keymap_spacing_alias[i].dead;
	return -1;
}

// The built-in sequences of the X compose table, after the loaded accent
// table found nothing: 0 when there is none.
static uint32_t dead_fallback(uint32_t d, uint32_t ch)
{
	int dead = dead_index_of(d);
	unsigned lo = 0,
		 hi = sizeof(keymap_dead_seq) / sizeof(keymap_dead_seq[0]);

	if (dead < 0 || ch > 0xffff)
		return 0;
	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;
		const struct keymap_dead_seq *e = &keymap_dead_seq[mid];

		if (e->dead == dead && e->base == ch)
			return e->result;
		if (e->dead < dead || (e->dead == dead && e->base < ch))
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

// What a pending accent types on its own.  A dead key's diacritic is known
// to the accent table by an ASCII stand-in (caron is 'c', breve 'U'); on its
// own it types the real spacing accent.
static uint32_t lone_accent(const struct keymap_state *st, uint32_t d)
{
	if (st->diacr_dead) {
		for (unsigned i = 0; i < NR_DEAD; i++)
			if (keymap_dead_char[i] == d)
				return keymap_dead_spacing[i];
	}
	return d;
}

// Combine the pending accent with `ch'.  Returns what `ch' becomes; a
// pending accent that does not combine is typed first, on its own.
static uint32_t handle_diacr(struct keymap_state *st, struct keymap_out *o,
			     uint32_t d, int dead, uint32_t ch)
{
	uint32_t r;

	st->diacr = 0;
	st->diacr_dead = (uint8_t)dead;
	if ((d & ~0xffu) == BRL_UC_ROW) {
		if ((ch & ~0xffu) == BRL_UC_ROW)
			return d | ch;
	} else {
		for (unsigned i = 0; i < st->naccent; i++)
			if (st->accent[i].diacr == d &&
			    st->accent[i].base == ch)
				return st->accent[i].result;
		if (dead && (r = dead_fallback(d, ch)) != 0)
			return r;
	}
	if (ch == ' ' || ch == BRL_UC_ROW || ch == d)
		return lone_accent(st, d);
	put_char(st, o, lone_accent(st, d));
	return ch;
}

// A character: through the pending accent, into the pending Compose, or to
// the terminal.
static void k_unicode(struct keymap_state *st, struct keymap_out *o,
		      uint32_t cp)
{
	if (st->diacr)
		cp = handle_diacr(st, o, st->diacr, st->diacr_dead, cp);
	if (st->dead_key_next) {
		st->dead_key_next = 0;
		st->diacr = cp;
		st->diacr_dead = 0;
		return;
	}
	put_char(st, o, cp);
}

// A dead key: start an accent, or combine with the pending one.
static void k_deadunicode(struct keymap_state *st, struct keymap_out *o,
			  uint32_t ch)
{
	if (st->dead_key_next) {
		// Compose followed by a dead key: the accent itself is the
		// first character of the sequence.
		st->dead_key_next = 0;
		st->diacr = ch;
		st->diacr_dead = 0;
		return;
	}
	if (st->diacr) {
		uint32_t d = st->diacr;
		int dead = st->diacr_dead;
		uint32_t r = 0;

		// Two accents that combine (the table may say so) stay
		// pending as the combination; the same accent twice types it;
		// two others type the first and keep the second.
		for (unsigned i = 0; i < st->naccent; i++)
			if (st->accent[i].diacr == d &&
			    st->accent[i].base == ch) {
				r = st->accent[i].result;
				break;
			}
		st->diacr = 0;
		if (r) {
			st->diacr = r;
			st->diacr_dead = 0;
			return;
		}
		st->diacr_dead = (uint8_t)dead;
		put_char(st, o, lone_accent(st, d));
		if (d == ch)
			return;
	}
	st->diacr = ch;
	st->diacr_dead = 1;
}

// ---------------------------------------------------------------------------
// Caps Lock and Unicode letters
// ---------------------------------------------------------------------------
//
// A keymap marks the keys Caps Lock applies to, but in the 16-bit symbol the
// mark only exists for Latin-1 (KT_LETTER); a Cyrillic, Greek or Armenian
// letter is a bare Unicode symbol.  For those, Caps Lock applies when the
// key's symbol and the symbol of its other Shift level are the lower and
// upper case of one letter -- which is what makes a key a letter key.

static uint32_t case_upper(uint32_t cp)
{
	unsigned lo = 0,
		 hi = sizeof(keymap_case_pairs) / sizeof(keymap_case_pairs[0]);

	while (lo < hi) {
		unsigned mid = (lo + hi) / 2;

		if (keymap_case_pairs[mid].lower == cp)
			return keymap_case_pairs[mid].upper;
		if (keymap_case_pairs[mid].lower < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return cp;
}

static int case_pair(uint32_t a, uint32_t b)
{
	return a != b && (case_upper(a) == b || case_upper(b) == a);
}

// The character a Unicode symbol types with Caps Lock on: the other Shift
// level's, when the two are a case pair.
static uint32_t caps_unicode(const struct keymap_state *st, unsigned sf,
			     unsigned kc, uint32_t cp)
{
	const uint16_t *m2;
	unsigned s2;

	if (!(st->ledflags & K_CAPSLOCK))
		return cp;
	m2 = st->maps[sf ^ (1u << KG_SHIFT)];
	if (!m2)
		return cp;
	s2 = m2[kc];
	if (is_unicode_sym(s2) && case_pair(cp, s2 ^ UNICODE_MASK))
		return s2 ^ UNICODE_MASK;
	return cp;
}

// ---------------------------------------------------------------------------
// Symbol handlers
// ---------------------------------------------------------------------------

// Function key string `idx'.  The six keys of the cursor block (Find ..
// Next) take xterm's modifier parameter while Shift or Ctrl is held when
// `modifiers' is set: "\033[H" becomes "\033[1;2H", "\033[5~" "\033[5;5~".
static void k_fn(struct keymap_state *st, struct keymap_out *o, unsigned idx,
		 int modifiers)
{
	const char *s;
	unsigned mod;

	if (idx >= MAX_NR_FUNC || !(s = st->func[idx]))
		return;
	mod = cursor_modifier(st);
	if (modifiers && mod > 1 && idx >= KVAL(K_FIND) &&
	    idx <= KVAL(K_PGDN) && s[0] == 27 && s[1] == '[') {
		const char *p = s + 2;
		const char *digits = p;
		unsigned nd = 0;

		while (p[nd] >= '0' && p[nd] <= '9')
			nd++;
		if (p[nd] && !p[nd + 1]) {
			put_byte(o, 27);
			put_byte(o, '[');
			if (nd) {
				for (unsigned i = 0; i < nd; i++)
					put_byte(o, (unsigned char)digits[i]);
			} else {
				put_byte(o, '1');
			}
			put_byte(o, ';');
			put_byte(o, (unsigned char)('0' + mod));
			put_byte(o, (unsigned char)p[nd]);
			return;
		}
	}
	put_str(o, s);
}

static void k_cur(struct keymap_state *st, struct keymap_out *o, unsigned val,
		  int modifiers)
{
	static const char cur_chars[] = "BDCA";
	unsigned mod = modifiers ? cursor_modifier(st) : 1;

	if (val > 3)
		return;
	put_byte(o, 27);
	put_byte(o, '[');
	if (mod > 1) {
		put_byte(o, '1');
		put_byte(o, ';');
		put_byte(o, (unsigned char)('0' + mod));
	}
	put_byte(o, (unsigned char)cur_chars[val]);
}

static void k_pad(struct keymap_state *st, struct keymap_out *o, unsigned val)
{
	static const uint16_t pad_chars[NR_PAD] = {
		'0', '1', '2', '3', '4', '5', '6',    '7', '8', '9',
		'+', '-', '*', '/', '\r', ',', '.', 0x00b1, '(', ')'
	};

	if (val >= NR_PAD)
		return;
	// Num Lock off: the keypad is the cursor block, without modifiers --
	// what the keypad of this console always typed.  The middle key types
	// its digit either way.
	if (!(st->ledflags & K_NUMLOCK)) {
		switch (val) {
		case KVAL(K_PCOMMA):
		case KVAL(K_PDOT):
			k_fn(st, o, KVAL(K_REMOVE), 0);
			return;
		case KVAL(K_P0):
			k_fn(st, o, KVAL(K_INSERT), 0);
			return;
		case KVAL(K_P1):
			k_fn(st, o, KVAL(K_SELECT), 0);
			return;
		case KVAL(K_P2):
			k_cur(st, o, KVAL(K_DOWN), 0);
			return;
		case KVAL(K_P3):
			k_fn(st, o, KVAL(K_PGDN), 0);
			return;
		case KVAL(K_P4):
			k_cur(st, o, KVAL(K_LEFT), 0);
			return;
		case KVAL(K_P6):
			k_cur(st, o, KVAL(K_RIGHT), 0);
			return;
		case KVAL(K_P7):
			k_fn(st, o, KVAL(K_FIND), 0);
			return;
		case KVAL(K_P8):
			k_cur(st, o, KVAL(K_UP), 0);
			return;
		case KVAL(K_P9):
			k_fn(st, o, KVAL(K_PGUP), 0);
			return;
		default:
			break;
		}
	}
	put_char(st, o, pad_chars[val]);
}

static void k_shift(struct keymap_state *st, struct keymap_out *o,
		    unsigned val, int up, int rep)
{
	uint16_t old = st->shift_state;

	if (rep)
		return;
	// A CapsShift key is a Shift that also releases Caps Lock.
	if (val == KG_CAPSSHIFT) {
		val = KG_SHIFT;
		if (!up)
			st->ledflags &= (uint8_t)~K_CAPSLOCK;
	}
	if (val >= NR_SHIFT)
		return;
	if (up) {
		if (st->shift_down[val])
			st->shift_down[val]--;
	} else if (st->shift_down[val] < 255) {
		st->shift_down[val]++;
	}
	if (st->shift_down[val])
		st->shift_state |= (uint16_t)(1u << val);
	else
		st->shift_state &= (uint16_t)~(1u << val);

	// Releasing the modifier ends a character typed by its number.
	if (up && st->shift_state != old && st->npadch_active) {
		if (st->kbdmode == K_UNICODE)
			put_utf8(o, st->npadch_value);
		else
			put_byte(o, (unsigned char)(st->npadch_value & 0xff));
		st->npadch_active = 0;
	}
}

static void k_spec(struct keymap_state *st, struct keymap_out *o, unsigned val,
		   int rep)
{
	switch (val) {
	case KVAL(K_HOLE):
		compute_shiftstate(st);
		break;
	case KVAL(K_ENTER):
		if (st->diacr) {
			put_char(st, o, lone_accent(st, st->diacr));
			st->diacr = 0;
		}
		put_byte(o, '\r');
		break;
	case KVAL(K_SH_REGS):
		o->actions |= KEYMAP_ACT_SHOW_REGS;
		break;
	case KVAL(K_SH_MEM):
		o->actions |= KEYMAP_ACT_SHOW_MEM;
		break;
	case KVAL(K_SH_STAT):
		o->actions |= KEYMAP_ACT_SHOW_STATE;
		break;
	case KVAL(K_BREAK):
		o->actions |= KEYMAP_ACT_BREAK;
		break;
	case KVAL(K_CAPS):
		if (!rep)
			st->ledflags ^= K_CAPSLOCK;
		break;
	case KVAL(K_NUM):
	case KVAL(K_BARENUMLOCK):
		if (!rep)
			st->ledflags ^= K_NUMLOCK;
		break;
	case KVAL(K_HOLD):
		if (!rep)
			st->ledflags ^= K_SCROLLLOCK;
		break;
	case KVAL(K_SCROLLFORW):
		o->actions |= KEYMAP_ACT_SCROLL_FWD;
		break;
	case KVAL(K_SCROLLBACK):
		o->actions |= KEYMAP_ACT_SCROLL_BACK;
		break;
	case KVAL(K_CAPSON):
		if (!rep)
			st->ledflags |= K_CAPSLOCK;
		break;
	case KVAL(K_COMPOSE):
		st->dead_key_next = 1;
		break;
	default:
		// Console switching, Boot, SAK, KeyboardSignal: there is one
		// console and no handler for them.
		break;
	}
}

static void k_brl(struct keymap_state *st, struct keymap_out *o, unsigned val,
		  int up)
{
	if (st->kbdmode != K_UNICODE)
		return;
	if (!val) {
		if (!up)
			k_unicode(st, o, BRL_UC_ROW);
		return;
	}
	if (val > 8)
		return;
	// A chord: the dots pressed together are typed as one pattern when
	// the last of them is released.
	if (!up) {
		st->brl_pressed |= (uint16_t)(1u << (val - 1));
		st->brl_committing |= (uint16_t)(1u << (val - 1));
		return;
	}
	st->brl_pressed &= (uint16_t)~(1u << (val - 1));
	if (!st->brl_pressed && st->brl_committing) {
		k_unicode(st, o, BRL_UC_ROW | st->brl_committing);
		st->brl_committing = 0;
	}
}

// ---------------------------------------------------------------------------
// Set-1 scancodes -> key codes
// ---------------------------------------------------------------------------
//
// A plain make code 0x01..0x58 is its own key code; the 0xE0-prefixed codes
// take the 96..127 block (and a few above); 0xE1 1D 45 is Pause.  These are
// the key codes evdev reports for the same keys, which is also what keymaps
// made from the X layouts are indexed by.

static void default_keycodes(struct keymap_state *st)
{
	uint8_t *p = st->kc_plain, *e = st->kc_e0;

	for (unsigned i = 0; i < 128; i++)
		p[i] = e[i] = 0;
	for (unsigned i = 0x01; i <= 0x58; i++)
		p[i] = (uint8_t)i;
	p[0x59] = 117; // keypad =
	p[0x70] = 93; // Katakana/Hiragana
	p[0x71] = 123; // Hanja    (sent as 0xF1, press only)
	p[0x72] = 122; // Hangul   (sent as 0xF2, press only)
	p[0x73] = 89; // Ro
	p[0x79] = 92; // Henkan
	p[0x7B] = 94; // Muhenkan
	p[0x7D] = 124; // Yen
	p[0x7E] = 121; // keypad comma

	e[0x10] = 165; // previous track
	e[0x19] = 163; // next track
	e[0x1C] = 96; // keypad Enter
	e[0x1D] = 97; // right Ctrl
	e[0x20] = 113; // mute
	e[0x22] = 164; // play/pause
	e[0x24] = 166; // stop
	e[0x2E] = 114; // volume down
	e[0x30] = 115; // volume up
	e[0x35] = 98; // keypad /
	e[0x37] = 99; // Print Screen / SysRq
	e[0x38] = 100; // right Alt (AltGr)
	e[0x46] = 119; // Ctrl+Break: Pause
	e[0x47] = 102; // Home
	e[0x48] = 103; // Up
	e[0x49] = 104; // Page Up
	e[0x4B] = 105; // Left
	e[0x4D] = 106; // Right
	e[0x4F] = 107; // End
	e[0x50] = 108; // Down
	e[0x51] = 109; // Page Down
	e[0x52] = 110; // Insert
	e[0x53] = 111; // Delete
	e[0x5B] = 125; // left Meta
	e[0x5C] = 126; // right Meta
	e[0x5D] = 127; // Menu (Compose)
	e[0x5E] = 116; // power
	e[0x5F] = 142; // sleep
	e[0x63] = 143; // wake
	// 0xE0 0x2A / 0xE0 0x36 are the keyboard's fake Shifts around the
	// cursor block: no key code, ignored.
}

int keymap_decode(struct keymap_state *st, uint8_t sc, unsigned kc[2],
		  int down[2])
{
	if (sc == 0xE0) {
		st->prefix = 0xE0;
		return 0;
	}
	if (sc == 0xE1) {
		st->prefix = 0xE1;
		st->e1_count = 0;
		return 0;
	}
	if (st->prefix == 0xE1) {
		if (st->e1_count == 0) {
			st->e1_first = sc;
			st->e1_count = 1;
			return 0;
		}
		st->prefix = 0;
		if ((st->e1_first & 0x7F) != 0x1D || (sc & 0x7F) != 0x45)
			return 0;
		kc[0] = 119; // Pause
		down[0] = !(sc & 0x80);
		return 1;
	}
	if (st->prefix == 0xE0) {
		st->prefix = 0;
		kc[0] = st->kc_e0[sc & 0x7F];
		down[0] = !(sc & 0x80);
		return kc[0] ? 1 : 0;
	}
	// Hanja and Hangul have no release code: their one byte is a press,
	// and the release follows at once.
	if (sc == 0xF1 || sc == 0xF2) {
		kc[0] = kc[1] = st->kc_plain[sc & 0x7F];
		down[0] = 1;
		down[1] = 0;
		return kc[0] ? 2 : 0;
	}
	kc[0] = st->kc_plain[sc & 0x7F];
	down[0] = !(sc & 0x80);
	return kc[0] ? 1 : 0;
}

int keymap_get_keycode(const struct keymap_state *st, unsigned scancode,
		       unsigned *keycode)
{
	if (scancode >= 256)
		return -EINVAL;
	*keycode = scancode < 128 ? st->kc_plain[scancode] :
				    st->kc_e0[scancode - 128];
	return 0;
}

int keymap_set_keycode(struct keymap_state *st, unsigned scancode,
		       unsigned keycode)
{
	// 0xE0 and 0xE1 are prefixes, not keys (their low seven bits are
	// 0x60 and 0x61), and 0 is no key.
	if (scancode >= 256 || keycode >= NR_KEYS || (scancode & 0x7F) == 0 ||
	    scancode == 0x60 || scancode == 0x61)
		return -EINVAL;
	if (scancode < 128)
		st->kc_plain[scancode] = (uint8_t)keycode;
	else
		st->kc_e0[scancode - 128] = (uint8_t)keycode;
	return 0;
}

// ---------------------------------------------------------------------------
// The key event
// ---------------------------------------------------------------------------

void keymap_note_key(struct keymap_state *st, unsigned keycode, int down)
{
	if (keycode < NR_KEYS)
		key_mark(st, keycode, down);
}

void keymap_key(struct keymap_state *st, unsigned kc, int down,
		struct keymap_out *o)
{
	const uint16_t *map;
	unsigned sf, sym, type, val;
	int rep, raw_mode;

	if (kc >= NR_KEYS)
		return;
	rep = down && key_is_down(st, kc);
	key_mark(st, kc, down);

	if (st->kbdmode == K_MEDIUMRAW) {
		// Key codes: bit 7 marks a release; a code above 127 is sent
		// as 0 followed by its two seven-bit halves.
		if (kc < 128) {
			put_byte(o, (unsigned char)(kc | (down ? 0 : 0x80)));
		} else {
			put_byte(o, down ? 0 : 0x80);
			put_byte(o, (unsigned char)((kc >> 7) | 0x80));
			put_byte(o, (unsigned char)((kc & 0x7f) | 0x80));
		}
		o->raw = 1;
	}
	raw_mode = st->kbdmode == K_RAW || st->kbdmode == K_MEDIUMRAW ||
		   st->kbdmode == K_OFF;

	sf = shift_final(st);
	map = st->maps[sf];
	if (!map) {
		compute_shiftstate(st);
		st->slockstate = 0;
		return;
	}
	sym = map[kc];

	if (is_unicode_sym(sym)) {
		if (down && !raw_mode) {
			k_unicode(st, o,
				  caps_unicode(st, sf, kc, sym ^ UNICODE_MASK));
			st->slockstate = 0;
		}
		return;
	}
	type = KTYP(sym);
	val = KVAL(sym);

	if (type == KT_LETTER) {
		type = KT_LATIN;
		// Caps Lock: a letter takes the symbol of the other Shift level.
		if (st->ledflags & K_CAPSLOCK) {
			const uint16_t *m2 =
				st->maps[sf ^ (1u << KG_SHIFT)];

			if (m2) {
				unsigned s2 = m2[kc];

				if (is_unicode_sym(s2)) {
					if (down && !raw_mode) {
						k_unicode(st, o,
							  s2 ^ UNICODE_MASK);
						st->slockstate = 0;
					}
					return;
				}
				if (KTYP(s2) == KT_LATIN ||
				    KTYP(s2) == KT_LETTER)
					val = KVAL(s2);
			}
		}
	}

	if (raw_mode && type != KT_SPEC && type != KT_SHIFT)
		return;

	switch (type) {
	case KT_LATIN:
		if (down)
			k_unicode(st, o, val);
		break;
	case KT_FN:
		if (down)
			k_fn(st, o, val, 1);
		break;
	case KT_SPEC:
		if (down && !raw_mode)
			k_spec(st, o, val, rep);
		break;
	case KT_PAD:
		if (down)
			k_pad(st, o, val);
		break;
	case KT_DEAD:
		if (down && val < NR_DEAD)
			k_deadunicode(st, o, keymap_dead_char[val]);
		break;
	case KT_DEAD2:
		if (down)
			k_deadunicode(st, o, val);
		break;
	case KT_CONS:
		break;
	case KT_CUR:
		if (down)
			k_cur(st, o, val, 1);
		break;
	case KT_SHIFT:
		k_shift(st, o, val, !down, rep);
		break;
	case KT_META:
		if (down) {
			if (st->metamode == K_ESCPREFIX) {
				put_byte(o, 27);
				put_char(st, o, val);
			} else {
				put_byte(o, (unsigned char)(val | 0x80));
			}
		}
		break;
	case KT_ASCII:
		if (down) {
			unsigned base = 10;

			if (val >= 10) {
				val -= 10;
				base = 16;
			}
			if (!st->npadch_active) {
				st->npadch_value = 0;
				st->npadch_active = 1;
			}
			if (st->npadch_value < 0x110000)
				st->npadch_value =
					st->npadch_value * base + val;
		}
		break;
	case KT_LOCK:
		if (down && !rep && val < NR_LOCK)
			st->lockstate ^= (uint8_t)(1u << val);
		break;
	case KT_SLOCK:
		k_shift(st, o, val, !down, rep);
		if (down && !rep && val < NR_LOCK) {
			st->slockstate ^= (uint8_t)(1u << val);
			// A sticky modifier with no keymap of its own: keep
			// only this one rather than none.
			if (!st->maps[(st->lockstate ^ st->slockstate) &
				      (MAX_NR_KEYMAPS - 1)])
				st->slockstate = (uint8_t)(1u << val);
		}
		break;
	case KT_BRL:
		k_brl(st, o, val, !down);
		break;
	default:
		break;
	}
	if (type != KT_SLOCK)
		st->slockstate = 0;
}

int32_t keymap_peek_char(const struct keymap_state *st, unsigned kc)
{
	const uint16_t *map;
	unsigned sf, sym;

	if (kc >= NR_KEYS)
		return -1;
	sf = shift_final(st);
	map = st->maps[sf];
	if (!map)
		return -1;
	sym = map[kc];
	if (is_unicode_sym(sym))
		return (int32_t)caps_unicode(st, sf, kc, sym ^ UNICODE_MASK);
	if (KTYP(sym) == KT_LETTER && (st->ledflags & K_CAPSLOCK)) {
		const uint16_t *m2 = st->maps[sf ^ (1u << KG_SHIFT)];

		if (m2) {
			unsigned s2 = m2[kc];

			if (is_unicode_sym(s2))
				return (int32_t)(s2 ^ UNICODE_MASK);
			if (KTYP(s2) == KT_LATIN || KTYP(s2) == KT_LETTER)
				return (int32_t)KVAL(s2);
		}
	}
	if (KTYP(sym) == KT_LATIN || KTYP(sym) == KT_LETTER)
		return (int32_t)KVAL(sym);
	return -1;
}

void keymap_reset_keys(struct keymap_state *st)
{
	st->prefix = 0;
	st->e1_count = 0;
	km_zero(st->key_down, sizeof(st->key_down));
	km_zero(st->shift_down, sizeof(st->shift_down));
	st->shift_state = 0;
	st->slockstate = 0;
	st->diacr = 0;
	st->diacr_dead = 0;
	st->dead_key_next = 0;
	st->npadch_active = 0;
	st->brl_pressed = 0;
	st->brl_committing = 0;
}

unsigned keymap_leds(const struct keymap_state *st)
{
	unsigned leds;

	if (st->led_override >= 0)
		return (unsigned)st->led_override & 7;
	// The flag bits are the LED bits (K_SCROLLLOCK == LED_SCR, ...).
	leds = st->ledflags & 7;
	// A lock of the Shift level is a Caps Lock as far as the user can
	// tell: the compiled-in keymap uses one, and generated keymaps use the
	// left-Ctrl lock to switch their keymaps to capitals.
	if (st->lockstate & ((1u << KG_SHIFT) | (1u << KG_CTRLL)))
		leds |= LED_CAP;
	return leds;
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

// The keymaps of the compiled-in configuration live here rather than in
// memory the caller supplied; they are never handed back.
#define DEFAULT_TABLES 8
static uint16_t default_maps[DEFAULT_TABLES][NR_KEYS];
static const uint8_t default_table_index[DEFAULT_TABLES] = {
	0, // plain
	1 << KG_SHIFT,
	1 << KG_CTRL,
	(1 << KG_CTRL) | (1 << KG_SHIFT),
	1 << KG_ALT,
	(1 << KG_ALT) | (1 << KG_SHIFT),
	(1 << KG_ALT) | (1 << KG_CTRL),
	(1 << KG_ALT) | (1 << KG_CTRL) | (1 << KG_SHIFT),
};

static int map_is_default(const uint16_t *m)
{
	return m >= &default_maps[0][0] &&
	       m < &default_maps[0][0] + DEFAULT_TABLES * NR_KEYS;
}

int keymap_table_exists(const struct keymap_state *st, unsigned table)
{
	return table < MAX_NR_KEYMAPS && st->maps[table] != NULL;
}

int keymap_get_entry(const struct keymap_state *st, unsigned table,
		     unsigned index, uint16_t *value)
{
	const uint16_t *m;
	unsigned v;

	if (table >= MAX_NR_KEYMAPS || index >= NR_KEYS)
		return -EINVAL;
	m = st->maps[table];
	if (!m) {
		*value = index ? K_HOLE : K_NOSUCHMAP;
		return 0;
	}
	v = index ? m[index] : K_ALLOCATED;
	// A Unicode symbol has no 8-bit form.
	if (st->kbdmode != K_UNICODE && is_unicode_sym(v))
		v = K_HOLE;
	*value = (uint16_t)v;
	return 0;
}

// The largest value each symbol type accepts.
static unsigned max_value(unsigned type)
{
	switch (type) {
	case KT_LATIN:
		return 255;
	case KT_FN:
		return MAX_NR_FUNC - 1;
	case KT_SPEC:
		return KVAL(K_BARENUMLOCK);
	case KT_PAD:
		return NR_PAD - 1;
	case KT_DEAD:
		return NR_DEAD - 1;
	case KT_CONS:
		return 255;
	case KT_CUR:
		return 3;
	case KT_SHIFT:
		return NR_SHIFT - 1;
	case KT_META:
		return 255;
	case KT_ASCII:
		return NR_ASCII - 1;
	case KT_LOCK:
		return NR_LOCK - 1;
	case KT_LETTER:
		return 255;
	case KT_SLOCK:
		return NR_LOCK - 1;
	case KT_DEAD2:
		return 255;
	case KT_BRL:
		return NR_BRL - 1;
	default:
		return 0;
	}
}

int keymap_set_entry(struct keymap_state *st, unsigned table, unsigned index,
		     unsigned value, uint16_t **spare, uint16_t **freed)
{
	uint16_t *m;
	unsigned old;

	*freed = NULL;
	if (table >= MAX_NR_KEYMAPS || index >= NR_KEYS || value > 0xffff)
		return -EINVAL;

	// Index 0 with K_NOSUCHMAP: deallocate the keymap.
	if (!index && value == K_NOSUCHMAP) {
		m = st->maps[table];
		if (!m)
			return 0;
		st->maps[table] = NULL;
		st->nmaps--;
		if (!map_is_default(m))
			*freed = m;
		return 0;
	}
	if (!is_unicode_sym(value)) {
		if (KVAL(value) > max_value(KTYP(value)))
			return -EINVAL;
	} else if (st->kbdmode != K_UNICODE) {
		return -EINVAL;
	}
	// Entry 0 only validates the arguments.
	if (!index)
		return 0;

	m = st->maps[table];
	if (!m) {
		if (!*spare)
			return -ENOMEM;
		m = *spare;
		*spare = NULL;
		for (unsigned i = 0; i < NR_KEYS; i++)
			m[i] = K_HOLE;
		st->maps[table] = m;
		st->nmaps++;
	}
	old = m[index];
	if (old == value)
		return 0;
	m[index] = (uint16_t)value;
	if (!table && ((!is_unicode_sym(old) && KTYP(old) == KT_SHIFT) ||
		       (!is_unicode_sym(value) && KTYP(value) == KT_SHIFT)))
		compute_shiftstate(st);
	return 0;
}

static int func_is_owned(const struct keymap_state *st, unsigned i)
{
	return (st->func_owned[i >> 3] >> (i & 7)) & 1;
}

void keymap_get_func(const struct keymap_state *st, unsigned idx, char *buf,
		     unsigned cap)
{
	const char *s = idx < MAX_NR_FUNC ? st->func[idx] : NULL;
	unsigned n = 0;

	if (!cap)
		return;
	if (s)
		while (s[n] && n + 1 < cap) {
			buf[n] = s[n];
			n++;
		}
	buf[n] = 0;
}

char *keymap_set_func(struct keymap_state *st, unsigned idx, char *s)
{
	char *old = NULL;

	if (idx >= MAX_NR_FUNC)
		return s;
	if (func_is_owned(st, idx))
		old = (char *)st->func[idx];
	st->func[idx] = s;
	if (s)
		st->func_owned[idx >> 3] |= (uint8_t)(1u << (idx & 7));
	else
		st->func_owned[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
	return old;
}

unsigned keymap_get_accents(const struct keymap_state *st,
			    struct kbdiacruc *out, unsigned cap)
{
	unsigned n = st->naccent < cap ? st->naccent : cap;

	km_copy(out, st->accent, n * sizeof(*out));
	return n;
}

int keymap_set_accents(struct keymap_state *st, const struct kbdiacruc *in,
		       unsigned count)
{
	if (count > MAX_DIACR)
		return -EINVAL;
	km_copy(st->accent, in, count * sizeof(*in));
	st->naccent = count;
	st->diacr = 0;
	return 0;
}

int keymap_set_kbdmode(struct keymap_state *st, unsigned mode)
{
	if (mode > K_OFF)
		return -EINVAL;
	st->kbdmode = (uint8_t)mode;
	st->diacr = 0;
	st->dead_key_next = 0;
	st->npadch_active = 0;
	compute_shiftstate(st);
	return 0;
}

int keymap_set_metamode(struct keymap_state *st, unsigned mode)
{
	if (mode != K_METABIT && mode != K_ESCPREFIX)
		return -EINVAL;
	st->metamode = (uint8_t)mode;
	return 0;
}

int keymap_set_ledflags(struct keymap_state *st, unsigned arg)
{
	if (arg & ~0x77u)
		return -EINVAL;
	st->ledflags = (uint8_t)(arg & 7);
	st->default_ledflags = (uint8_t)((arg >> 4) & 7);
	return 0;
}

unsigned keymap_get_ledflags(const struct keymap_state *st)
{
	return (unsigned)st->ledflags | ((unsigned)st->default_ledflags << 4);
}

void keymap_set_led_override(struct keymap_state *st, unsigned leds)
{
	st->led_override = leds > 7 ? -1 : (int16_t)leds;
}

// ---------------------------------------------------------------------------
// The compiled-in configuration
// ---------------------------------------------------------------------------

// The US layout by key code (key codes 1..88 are the set-1 make codes).
static const char us_plain[89] = {
	0,    27,  '1', '2',  '3',  '4', '5',  '6', // 0x00-0x07
	'7',  '8', '9', '0',  '-',  '=', '\b', '\t', // 0x08-0x0F
	'q',  'w', 'e', 'r',  't',  'y', 'u',  'i', // 0x10-0x17
	'o',  'p', '[', ']',  '\r', 0,   'a',  's', // 0x18-0x1F
	'd',  'f', 'g', 'h',  'j',  'k', 'l',  ';', // 0x20-0x27
	'\'', '`', 0,   '\\', 'z',  'x', 'c',  'v', // 0x28-0x2F
	'b',  'n', 'm', ',',  '.',  '/', 0,    '*', // 0x30-0x37
	0,    ' ', 0,   0,    0,    0,   0,    0, // 0x38-0x3F
	0,    0,   0,   0,    0,    0,   0,    0, // 0x40-0x47
	0,    0,   '-', 0,    0,    0,   '+',  0, // 0x48-0x4F
	0,    0,   0,   0,    0,    0,   0,    0, // 0x50-0x57
	0 // 0x58
};

static const char us_shift[89] = {
	0,   27,  '!', '@', '#',  '$', '%',  '^', // 0x00-0x07
	'&', '*', '(', ')', '_',  '+', '\b', '\t', // 0x08-0x0F
	'Q', 'W', 'E', 'R', 'T',  'Y', 'U',  'I', // 0x10-0x17
	'O', 'P', '{', '}', '\r', 0,   'A',  'S', // 0x18-0x1F
	'D', 'F', 'G', 'H', 'J',  'K', 'L',  ':', // 0x20-0x27
	'"', '~', 0,   '|', 'Z',  'X', 'C',  'V', // 0x28-0x2F
	'B', 'N', 'M', '<', '>',  '?', 0,    '*', // 0x30-0x37
	0,   ' ', 0,   0,   0,    0,   0,    0, // 0x38-0x3F
	0,   0,   0,   0,   0,    0,   0,    0, // 0x40-0x47
	0,   0,   '-', 0,   0,    0,   '+',  0, // 0x48-0x4F
	0,   0,   0,   0,   0,    0,   0,    0, // 0x50-0x57
	0 // 0x58
};

// The symbol of a key that is the same in every keymap, or K_HOLE when the
// key types a character (or nothing).
static unsigned default_fixed_symbol(unsigned kc)
{
	switch (kc) {
	case 29: // left Ctrl
	case 97: // right Ctrl
		return K_CTRL;
	case 42: // left Shift
	case 54: // right Shift
		return K_SHIFT;
	case 56: // left Alt
	case 100: // right Alt
		return K_ALT;
	case 58: // Caps Lock: a lock of the Shift level
		return K_SHIFTLOCK;
	case 69:
		return K_NUM;
	case 70:
		return K_HOLD;
	case 71:
		return K_P7;
	case 72:
		return K_P8;
	case 73:
		return K_P9;
	case 75:
		return K_P4;
	case 76:
		return K_P5;
	case 77:
		return K_P6;
	case 79:
		return K_P1;
	case 80:
		return K_P2;
	case 81:
		return K_P3;
	case 82:
		return K_P0;
	case 83:
		return K_PDOT;
	case 87:
		return K_F11;
	case 88:
		return K_F12;
	case 96:
		return K_PENTER;
	case 98:
		return K_PSLASH;
	case 102:
		return K_FIND;
	case 103:
		return K_UP;
	case 104:
		return K_PGUP;
	case 105:
		return K_LEFT;
	case 106:
		return K_RIGHT;
	case 107:
		return K_SELECT;
	case 108:
		return K_DOWN;
	case 109:
		return K_PGDN;
	case 110:
		return K_INSERT;
	case 111:
		return K_REMOVE;
	default:
		if (kc >= 59 && kc <= 68) // F1-F10
			return K(KT_FN, kc - 59);
		return K_HOLE;
	}
}

// The function key strings of the compiled-in configuration.  F1-F12 and the
// cursor block are the console's own; F13-F48 are F1-F12 with Shift, Ctrl and
// Ctrl+Shift in xterm's encoding, which is where generated keymaps put those
// combinations.
static const char *const default_fn_f1_12[12] = {
	"\033OP",   "\033OQ",	"\033OR",   "\033OS",	"\033[15~", "\033[17~",
	"\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
};
static const char *const default_fn_mod[3][12] = {
	{ "\033[1;2P", "\033[1;2Q", "\033[1;2R", "\033[1;2S", "\033[15;2~",
	  "\033[17;2~", "\033[18;2~", "\033[19;2~", "\033[20;2~", "\033[21;2~",
	  "\033[23;2~", "\033[24;2~" },
	{ "\033[1;5P", "\033[1;5Q", "\033[1;5R", "\033[1;5S", "\033[15;5~",
	  "\033[17;5~", "\033[18;5~", "\033[19;5~", "\033[20;5~", "\033[21;5~",
	  "\033[23;5~", "\033[24;5~" },
	{ "\033[1;6P", "\033[1;6Q", "\033[1;6R", "\033[1;6S", "\033[15;6~",
	  "\033[17;6~", "\033[18;6~", "\033[19;6~", "\033[20;6~", "\033[21;6~",
	  "\033[23;6~", "\033[24;6~" },
};

// The KT_FN value of function key Fn (1..245).
static unsigned fn_index(unsigned n)
{
	return n <= 20 ? n - 1 : n - 21 + KVAL(K_F21);
}

void keymap_init(struct keymap_state *st)
{
	km_zero(st, sizeof(*st));
	st->kbdmode = K_UNICODE;
	st->metamode = K_ESCPREFIX;
	st->kdmode = KD_TEXT;
	st->led_override = -1;
	default_keycodes(st);

	for (unsigned t = 0; t < DEFAULT_TABLES; t++) {
		unsigned idx = default_table_index[t];
		int shift = (idx >> KG_SHIFT) & 1;
		int ctrl = (idx >> KG_CTRL) & 1;
		int alt = (idx >> KG_ALT) & 1;
		uint16_t *m = default_maps[t];

		for (unsigned kc = 0; kc < NR_KEYS; kc++) {
			unsigned sym = default_fixed_symbol(kc);
			unsigned c;

			m[kc] = (uint16_t)sym;
			if (sym != K_HOLE || kc > 88)
				continue;
			c = (unsigned char)(shift ? us_shift[kc] : us_plain[kc]);
			if (!c)
				continue;
			if (ctrl && c == ' ') {
				// Ctrl+Space is NUL, Alt or not.
				m[kc] = K(KT_LATIN, 0);
				continue;
			}
			if (ctrl && ((c >= 'a' && c <= 'z') ||
				     (c >= 'A' && c <= 'Z')))
				c &= 0x1f;
			m[kc] = (uint16_t)(alt ? K(KT_META, c) :
						 K(KT_LATIN, c));
		}
		st->maps[idx] = m;
		st->nmaps++;
	}

	for (unsigned i = 0; i < 12; i++) {
		st->func[fn_index(i + 1)] = default_fn_f1_12[i];
		for (unsigned j = 0; j < 3; j++)
			st->func[fn_index(13 + 12 * j + i)] =
				default_fn_mod[j][i];
	}
	st->func[KVAL(K_FIND)] = "\033[H";
	st->func[KVAL(K_INSERT)] = "\033[2~";
	st->func[KVAL(K_REMOVE)] = "\033[3~";
	st->func[KVAL(K_SELECT)] = "\033[F";
	st->func[KVAL(K_PGUP)] = "\033[5~";
	st->func[KVAL(K_PGDN)] = "\033[6~";
	st->func[255] = "\033[Z"; // Backtab

	st->naccent = sizeof(keymap_default_accents) /
		      sizeof(keymap_default_accents[0]);
	km_copy(st->accent, keymap_default_accents, sizeof(keymap_default_accents));
}

void keymap_release(struct keymap_state *st, void (*free_fn)(void *))
{
	for (unsigned t = 0; t < MAX_NR_KEYMAPS; t++)
		if (st->maps[t] && !map_is_default(st->maps[t]))
			free_fn(st->maps[t]);
	for (unsigned i = 0; i < MAX_NR_FUNC; i++)
		if (func_is_owned(st, i))
			free_fn((void *)st->func[i]);
	km_zero(st, sizeof(*st));
}
