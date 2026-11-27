// LikeOS I/O Subsystem - Keyboard Driver
// PS/2 keyboard input handling and device management
//
// Every keyboard is one stream of set-1 scancodes here: the PS/2 keyboard
// delivers it from its interrupt, USB HID keyboards translate their reports
// into it and hand it to keyboard_buffer_add().  The stream goes two ways:
//
//   raw      -> evdev (/dev/input/event0): the display server does its own
//               layout, so it sees the scancodes exactly as they come.
//   decoded  -> key codes -> the keymap engine (keymap.c) -> the console
//               terminal: characters in the loaded layout as UTF-8,
//               function key strings, cursor sequences.
//
// While another program has the keyboard -- it switched the console's
// keyboard off (KDSKBMODE K_OFF), or it holds the exclusive grab on event0
// -- the console side is bypassed entirely (see "Who has the keyboard"
// below).
//
// The keymap, the keyboard mode, the display mode and the lock LEDs are the
// console's KD* ioctls (<kernel/uapi/kd.h>), answered by keyboard_kd_ioctl()
// for the console terminal.  The program that switched the console off gets
// it back automatically when its last descriptor closes ("The console's
// modes and who set them").
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/input/keyboard.h>
#include <kernel/dev/input/keymap.h>
#include <kernel/ke/interrupt.h>
#include <kernel/io/tty.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/cred.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/uaccess.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/rand/random.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/uapi/bug.h>

// The console's keymap engine state, and the lock that serialises it: the
// PS/2 interrupt, USB HID completions and the ioctls all reach it.
static struct keymap_state g_km;
static spinlock_t kb_lock = SPINLOCK_INIT("keyboard");
static uint8_t kb_input_ready = 0;

// ---------------------------------------------------------------------------
// Lock LEDs
// ---------------------------------------------------------------------------
//
// PS/2: command 0xED, then the LED byte, each acknowledged with 0xFA through
// the keyboard interrupt.  The exchange is driven from there, one step per
// acknowledgement, so nothing ever waits for the keyboard.  Only a keyboard
// that has sent something is ever written to.  USB keyboards pick the state
// up from keyboard_led_state() (usbhid_poll).

static volatile uint8_t kb_ps2_seen;
static uint8_t led_cmd_state; // 0 idle, 1 0xED sent, 2 LED byte sent
static uint8_t led_cmd_tries;
static uint8_t led_hw = 0xFF; // what the PS/2 keyboard shows (0xFF unknown)
static uint8_t led_sending;
static volatile uint8_t led_want;
static uint64_t led_cmd_tick;

// Who has the keyboard (kb_lock): 1 while the console side is bypassed.
static uint8_t kb_console_off;
// The lock LEDs the client of event0 last asked for (EV_LED), in the
// console's LED_* bits; shown while the console is bypassed (kb_lock).
static uint8_t led_client;

static void ps2_write_data(uint8_t b)
{
	for (int i = 0; i < 100000; i++)
		if (!(inb(KEYBOARD_STATUS_PORT) & KEYBOARD_STATUS_INPUT_FULL))
			break;
	outb(KEYBOARD_DATA_PORT, b);
}

// Start an LED update if one is due.  Caller holds kb_lock.
static void ps2_led_kick(void)
{
	if (!kb_ps2_seen || !kb_input_ready)
		return;
	if (led_cmd_state) {
		// An exchange that the keyboard never acknowledged is given
		// up after a while rather than blocking every later update.
		if (timer_ticks() - led_cmd_tick < timer_ms_to_ticks(250))
			return;
		led_cmd_state = 0;
	}
	if (led_hw == led_want)
		return;
	led_cmd_state = 1;
	led_cmd_tries = 0;
	led_cmd_tick = timer_ticks();
	ps2_write_data(0xED);
}

// The keyboard's answer during an LED exchange.  Returns 1 when the byte
// belonged to the exchange.  Caller holds kb_lock.
static int ps2_led_answer(uint8_t sc)
{
	if (!led_cmd_state || (sc != 0xFA && sc != 0xFE))
		return 0;
	if (sc == 0xFE) {
		// Resend: repeat the current step a few times, then give up.
		if (++led_cmd_tries > 3) {
			led_cmd_state = 0;
			return 1;
		}
		ps2_write_data(led_cmd_state == 1 ? 0xED : led_sending);
		return 1;
	}
	if (led_cmd_state == 1) {
		led_sending = led_want & 7;
		led_cmd_state = 2;
		led_cmd_tries = 0;
		led_cmd_tick = timer_ticks();
		ps2_write_data(led_sending);
		return 1;
	}
	led_cmd_state = 0;
	led_hw = led_sending;
	ps2_led_kick();
	return 1;
}

// Re-read the LEDs to show: the keymap's, or while the console is bypassed
// the ones the client of event0 set.  Caller holds kb_lock.
static void keyboard_leds_update_locked(void)
{
	led_want = kb_console_off ? led_client : (uint8_t)keymap_leds(&g_km);
	ps2_led_kick();
}

uint8_t keyboard_led_state(void)
{
	return led_want;
}

// ---------------------------------------------------------------------------
// Delivery to the console terminal
// ---------------------------------------------------------------------------

static void keyboard_deliver(const struct keymap_out *o, int raw)
{
	tty_t *tty;

	if (!o->len && !o->actions)
		return;
	tty = tty_get_console();
	if (!tty)
		return;
	for (unsigned i = 0; i < o->len; i++) {
		char c = (char)o->buf[i];

		// Raw keyboard modes and NUL (Ctrl+Space) bypass the line
		// discipline; everything else is typed.
		if (raw || o->raw || c == 0)
			tty_input_char_raw(tty, c);
		else
			tty_input_char(tty, c, 0);
	}
	if (o->actions & KEYMAP_ACT_SHOW_STATE)
		sched_dump_tasks(NULL);
	if (o->actions & KEYMAP_ACT_SHOW_MEM) {
		memory_stats_t ms;

		mm_get_memory_stats(&ms);
		kprintf("Memory: %llu KB total, %llu KB free, %llu KB used, heap %llu KB, page cache %llu pages\n",
			ms.total_memory / 1024, ms.free_memory / 1024,
			ms.used_memory / 1024, ms.heap_allocated / 1024,
			ms.pagecache_pages);
	}
	if (o->actions & KEYMAP_ACT_BREAK)
		tty_input_char(tty, (char)tty->term.c_cc[VINTR], 0);
	if (o->actions & (KEYMAP_ACT_SCROLL_BACK | KEYMAP_ACT_SCROLL_FWD)) {
		uint32_t rows = 0, cols = 0;

		console_get_dimensions(&rows, &cols);
		for (uint32_t i = 0; i < (rows ? rows / 2 : 12); i++) {
			if (o->actions & KEYMAP_ACT_SCROLL_BACK)
				console_scroll_up_line();
			else
				console_scroll_down_line();
		}
	}
}

// Debug hotkeys of the console: Ctrl+D dumps the task table, Ctrl+N the
// TCP, AF_UNIX and PTY tables.  Both go through kprintf -- tty_printf(NULL)
// -- and NOT to tty_get_active(): this runs in interrupt context, and a pty
// write is a blocking enqueue a handler may not make.  kprintf reaches the
// console and the serial line without sleeping, and lands in the kernel log,
// so the tables can also be read back with dmesg.  For a dump without a key
// press, kdump(1) calls SYS_DEBUG_DUMP and emits the same tables.
//
// The keys are the characters, not key positions: the hotkey is whatever
// key types Ctrl+D (^D) or Ctrl+N (^N) in the loaded layout, with Ctrl held
// and Alt not.  The key's ^D / ^N is not typed.
#define HOTKEY_NONE 0
#define HOTKEY_TASKS 1
#define HOTKEY_NET 2

static int keyboard_hotkey(unsigned kc)
{
	int32_t c;

	if (!keymap_ctrl_held(&g_km) || keymap_alt_held(&g_km))
		return HOTKEY_NONE;
	c = keymap_peek_char(&g_km, kc);
	if (c == 0x04)
		return HOTKEY_TASKS;
	if (c == 0x0E)
		return HOTKEY_NET;
	return HOTKEY_NONE;
}

static void keyboard_run_hotkey(int hk)
{
	if (hk == HOTKEY_TASKS) {
		sched_dump_tasks(NULL);
	} else if (hk == HOTKEY_NET) {
		extern void tcp_dump_table(struct tty * tty);
		extern void tty_dump_ptys(struct tty * tty);
		extern void unix_dump_sockets(struct tty * tty);
		tcp_dump_table(NULL);
		unix_dump_sockets(NULL);
		tty_dump_ptys(NULL);
	}
}

// ---------------------------------------------------------------------------
// Who has the keyboard
// ---------------------------------------------------------------------------
//
// The console terminal gets the keyboard only while nobody else has it, and
// two things take it away:
//
//   - the keyboard mode is K_OFF.  A display server sets it when it opens
//     the console (and KD_GRAPHICS with it): it reads event0 without a grab
//     -- its input stack pairs the keyboard with the touchpad to pause the
//     touchpad while typing -- so without this every keystroke would also be
//     typed into the shell that started it, unseen, and that line would run
//     the moment the session ended;
//   - a client holds the exclusive grab on event0 (EVIOCGRAB).
//
// Meanwhile the scancodes still reach evdev, and nothing of the console's
// runs: no characters, no raw bytes, no keymap actions (Break, Show_Memory,
// Show_State, scrollback) and no Ctrl+D / Ctrl+N hotkeys -- exactly as under
// the grab.  There are no virtual terminals and no other console keys to
// keep.  K_RAW and K_MEDIUMRAW are not "off": the console terminal gets the
// scancodes or key codes as before.
//
// Which program draws on the screen does not matter here: a program that
// maps /dev/fb0 (fbtest, the test suite) keeps the keyboard.
//
// Keys pressed or released meanwhile never reached the keymap engine, so
// its record of held keys and modifiers (and a half-read 0xE0/0xE1
// sequence) is dropped when the console gets the keyboard back: a Shift
// released inside the session must not stay held on the console.  Its lock
// state -- Caps, Num, Scroll Lock -- is its own and is kept.  The LEDs
// follow whoever has the keyboard.
//
// Checked for every byte, under kb_lock, so a key never reaches the console
// while it is off; a mode change also re-evaluates at once, so the LEDs
// switch without waiting for a key.

// Re-evaluate who has the keyboard; nonzero while it is not the console.
// Caller holds kb_lock.
static int keyboard_owner_update_locked(int grabbed)
{
	uint8_t off = (grabbed || g_km.kbdmode == K_OFF) ? 1 : 0;

	if (off != kb_console_off) {
		kb_console_off = off;
		if (!off)
			keymap_reset_keys(&g_km);
		keyboard_leds_update_locked();
	}
	return off;
}

void keyboard_client_leds(int num, int caps, int scroll)
{
	uint64_t flags;

	spin_lock_irqsave(&kb_lock, &flags);
	led_client = (uint8_t)((num ? LED_NUM : 0) | (caps ? LED_CAP : 0) |
			       (scroll ? LED_SCR : 0));
	if (kb_console_off)
		keyboard_leds_update_locked();
	spin_unlock_irqrestore(&kb_lock, flags);
}

// ---------------------------------------------------------------------------
// The console's modes and who set them
// ---------------------------------------------------------------------------
//
// The keyboard mode (KDSKBMODE) and the display mode (KDSETMODE) are global
// state of the one console, and a display server that dies without putting
// them back would leave the machine without a keyboard: K_OFF stays, and
// KD_GRAPHICS keeps the console's pointer dead.  So the kernel remembers
// which open file of the console last switched it away from its normal
// state -- KD_GRAPHICS, or K_OFF -- and when that file is finally closed
// (the program exited, crashed or was killed: the descriptor table goes
// either way) the console gets KD_TEXT and the keyboard mode from before
// back, with no key left held, its own LEDs and a full redraw.
//
// Only the LAST file to change a mode owns the restore: another file that
// sets a mode afterwards takes it over (inheriting the mode to go back to),
// and the earlier file's close then changes nothing.  A file that puts both
// modes back to normal itself leaves nothing to restore.
//
// Files are told apart by a token handed out at open (never reused), not by
// address: a closed file's memory can be the next open's.  Token 0 is a
// request that came without a file (a console descriptor of a task's stdio
// markers); it owns nothing.
//
// The owner may also always change the modes back, even when it would no
// longer pass kd_permitted(): the display server opens the console as root
// and drops its privileges before it exits.

static uint64_t kd_owner; // token of the owning file, 0 none (kb_lock)
static uint8_t kd_saved_kbdmode = K_UNICODE; // to go back to (kb_lock)
static uint64_t kd_next_token = 1;

uint64_t keyboard_console_file_token(void)
{
	return __atomic_fetch_add(&kd_next_token, 1, __ATOMIC_RELAXED);
}

int keyboard_kd_graphics(void)
{
	return __atomic_load_n(&g_km.kdmode, __ATOMIC_ACQUIRE) == KD_GRAPHICS;
}

// Both modes as the console normally has them.  Caller holds kb_lock.
static int kd_state_normal(void)
{
	return g_km.kdmode == KD_TEXT && g_km.kbdmode != K_OFF;
}

// Record who changed a mode, after the change.  was_normal and prev_kbdmode
// describe the state before it.  Caller holds kb_lock.
static void kd_note_change_locked(uint64_t token, int was_normal,
				  uint8_t prev_kbdmode)
{
	if (kd_state_normal()) {
		kd_owner = 0;
		return;
	}
	// Leaving the normal state: what to come back to is the mode in
	// effect now -- never K_OFF, which is not normal.  Otherwise the
	// earlier owner's saved mode is kept.
	if (was_normal)
		kd_saved_kbdmode = prev_kbdmode;
	WARN_ON(kd_saved_kbdmode == K_OFF);
	kd_owner = token;
}

void keyboard_console_file_closed(uint64_t token)
{
	int grabbed;
	uint64_t flags;
	int restored = 0;
	int change = 0;

	if (!token)
		return;
	grabbed = evdev_kbd_grabbed();
	spin_lock_irqsave(&kb_lock, &flags);
	if (kd_owner == token) {
		kd_owner = 0;
		(void)keymap_set_kbdmode(&g_km, kd_saved_kbdmode);
		__atomic_store_n(&g_km.kdmode, (uint8_t)KD_TEXT,
				 __ATOMIC_SEQ_CST);
		change = console_display_kd_note(0);
		// Whatever was held when the owner went is not held now.
		keymap_reset_keys(&g_km);
		(void)keyboard_owner_update_locked(grabbed);
		keyboard_leds_update_locked();
		restored = 1;
	}
	spin_unlock_irqrestore(&kb_lock, flags);
	if (!restored)
		return;
	kprintf("console: keyboard and display modes restored (their owner closed the console)\n");
	// The screen: whatever that program left on it goes -- if this ended
	// the display's ownership.  While a DRM master still holds it, its
	// drop repaints instead.
	console_display_settle(change);
}

// One byte of the stream, console side.
static void keyboard_process(uint8_t sc, int grabbed)
{
	struct keymap_out o; // filled under kb_lock, delivered after it
	unsigned kc[2];
	int down[2];
	int n, raw, hk = HOTKEY_NONE;
	uint64_t flags;

	spin_lock_irqsave(&kb_lock, &flags);
	if (keyboard_owner_update_locked(grabbed)) {
		spin_unlock_irqrestore(&kb_lock, flags);
		return;
	}
	o.len = 0;
	o.actions = 0;
	o.raw = 0;
	raw = g_km.kbdmode == K_RAW;
	if (raw)
		o.buf[o.len++] = sc; // K_RAW: the scancodes themselves
	n = keymap_decode(&g_km, sc, kc, down);
	for (int i = 0; i < n; i++) {
		if (down[i] && (g_km.kbdmode == K_UNICODE ||
				g_km.kbdmode == K_XLATE) &&
		    (hk = keyboard_hotkey(kc[i])) != HOTKEY_NONE) {
			keymap_note_key(&g_km, kc[i], 1);
			continue;
		}
		keymap_key(&g_km, kc[i], down[i], &o);
	}
	keyboard_leds_update_locked();
	spin_unlock_irqrestore(&kb_lock, flags);

	// Outside the lock: the terminal takes its own locks and echoes to
	// the console.
	keyboard_deliver(&o, raw);
	if (hk != HOTKEY_NONE)
		keyboard_run_hotkey(hk);
}

// ---------------------------------------------------------------------------
// Initialisation and the producers
// ---------------------------------------------------------------------------

void keyboard_init(void)
{
	keymap_init(&g_km);
	kb_input_ready = 0;
	led_want = (uint8_t)keymap_leds(&g_km);

	kprintf("Keyboard initialized\n");
}

void keyboard_activate(void)
{
	uint64_t flags;

	spin_lock_irqsave(&kb_lock, &flags);
	keymap_reset_keys(&g_km);
	spin_unlock_irqrestore(&kb_lock, flags);

	for (int i = 0; i < 32; ++i) {
		uint8_t status = inb(KEYBOARD_STATUS_PORT);
		if (!(status & KEYBOARD_STATUS_OUTPUT_FULL)) {
			break;
		}
		if (status & KEYBOARD_STATUS_AUXDATA) {
			break;
		}
		(void)inb(KEYBOARD_DATA_PORT);
	}

	kb_input_ready = 1;
}

void keyboard_reset_state(void)
{
	uint64_t flags;

	spin_lock_irqsave(&kb_lock, &flags);
	keymap_reset_keys(&g_km);
	spin_unlock_irqrestore(&kb_lock, flags);
}

// Read scan code from keyboard
uint8_t keyboard_read_scan_code(void)
{
	return inb(KEYBOARD_DATA_PORT);
}

// Producers other than the PS/2 keyboard (USB HID): one byte of the set-1
// stream.  evdev sees it first, exactly as it comes; the console side
// follows unless another program has the keyboard.
void keyboard_buffer_add(uint8_t scan_code)
{
	int grabbed = evdev_feed_keyboard(scan_code);

	keyboard_process(scan_code, grabbed);
}

// Keyboard interrupt handler
void keyboard_irq_handler(void)
{
	uint8_t scan_code = keyboard_read_scan_code();
	int grabbed;

	// Feed keyboard timing entropy
	entropy_add_interrupt_timing((uint64_t)scan_code);

	if (!kb_input_ready) {
		return;
	}
	kb_ps2_seen = 1;

	// The keyboard's answers to an LED update are not keys: they must
	// reach neither evdev nor the console.
	if (led_cmd_state && (scan_code == 0xFA || scan_code == 0xFE)) {
		uint64_t flags;
		int mine;

		spin_lock_irqsave(&kb_lock, &flags);
		mine = ps2_led_answer(scan_code);
		spin_unlock_irqrestore(&kb_lock, flags);
		if (mine)
			return;
	}

	// evdev tap: the raw set-1 stream (0xE0 prefixes included) becomes
	// input events on /dev/input/event0 in parallel with the cooked tty
	// path.  While a client holds a grab or another program drives the
	// display, the tty path (and thus SIGINT generation) is suppressed.
	grabbed = evdev_feed_keyboard(scan_code);
	keyboard_process(scan_code, grabbed);
}

// ---------------------------------------------------------------------------
// KD* ioctls of the console terminal
// ---------------------------------------------------------------------------

// Changing the keyboard is for root and for the console's own session.
static int kd_permitted(task_t *cur)
{
	if (!cur || capable())
		return 1;
	return cur->ctty == tty_get_console();
}

static int put_user_u8(void *argp, uint8_t v)
{
	return copy_to_user(argp, &v, 1) ? -EFAULT : 0;
}

static int put_user_int(void *argp, int v)
{
	return copy_to_user(argp, &v, sizeof(v)) ? -EFAULT : 0;
}

static int kd_set_entry(const struct kbentry *ke)
{
	uint16_t *spare = NULL, *freed = NULL;
	uint64_t flags;
	int r;

	for (int attempt = 0; attempt < 2; attempt++) {
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_set_entry(&g_km, ke->kb_table, ke->kb_index,
				     ke->kb_value, &spare, &freed);
		if (r == 0)
			keyboard_leds_update_locked();
		spin_unlock_irqrestore(&kb_lock, flags);
		if (freed)
			kfree(freed);
		if (r != -ENOMEM || spare)
			break;
		// The keymap has to be created: allocate outside the lock
		// and try again.
		spare = (uint16_t *)kalloc(NR_KEYS * sizeof(uint16_t));
		if (!spare)
			return -ENOMEM;
	}
	if (spare)
		kfree(spare);
	return r;
}

static int kd_get_diacrs(void *argp, int unicode)
{
	struct kbdiacruc *uc;
	unsigned n;
	uint64_t flags;
	int r = 0;

	uc = (struct kbdiacruc *)kalloc(MAX_DIACR * sizeof(*uc));
	if (!uc)
		return -ENOMEM;
	spin_lock_irqsave(&kb_lock, &flags);
	n = keymap_get_accents(&g_km, uc, MAX_DIACR);
	spin_unlock_irqrestore(&kb_lock, flags);

	if (unicode) {
		unsigned cnt = n;

		if (copy_to_user(argp, &cnt, sizeof(cnt)) ||
		    copy_to_user((char *)argp + sizeof(unsigned int), uc,
				 n * sizeof(*uc)))
			r = -EFAULT;
	} else {
		// The 8-bit table holds what converts to Latin-1.
		struct kbdiacrs *d = (struct kbdiacrs *)kalloc(sizeof(*d));
		unsigned cnt = 0;

		if (!d) {
			kfree(uc);
			return -ENOMEM;
		}
		for (unsigned i = 0; i < n; i++) {
			int32_t a = keymap_uni_to_8bit(uc[i].diacr);
			int32_t b = keymap_uni_to_8bit(uc[i].base);
			int32_t c = keymap_uni_to_8bit(uc[i].result);

			if (a < 0 || b < 0 || c < 0)
				continue;
			d->kbdiacr[cnt].diacr = (unsigned char)a;
			d->kbdiacr[cnt].base = (unsigned char)b;
			d->kbdiacr[cnt].result = (unsigned char)c;
			cnt++;
		}
		d->kb_cnt = cnt;
		if (copy_to_user(argp, d,
				 sizeof(unsigned int) +
					 cnt * sizeof(struct kbdiacr)))
			r = -EFAULT;
		kfree(d);
	}
	kfree(uc);
	return r;
}

static int kd_set_diacrs(void *argp, int unicode)
{
	struct kbdiacruc *uc;
	unsigned int cnt;
	uint64_t flags;
	int r;

	if (copy_from_user(&cnt, argp, sizeof(cnt)))
		return -EFAULT;
	if (cnt > MAX_DIACR)
		return -EINVAL;
	uc = (struct kbdiacruc *)kalloc(MAX_DIACR * sizeof(*uc));
	if (!uc)
		return -ENOMEM;
	if (unicode) {
		if (cnt && copy_from_user(uc, (char *)argp + sizeof(cnt),
					  cnt * sizeof(*uc))) {
			kfree(uc);
			return -EFAULT;
		}
	} else {
		struct kbdiacr *d =
			(struct kbdiacr *)kalloc(MAX_DIACR * sizeof(*d));

		if (!d) {
			kfree(uc);
			return -ENOMEM;
		}
		if (cnt && copy_from_user(d, (char *)argp + sizeof(cnt),
					  cnt * sizeof(*d))) {
			kfree(d);
			kfree(uc);
			return -EFAULT;
		}
		for (unsigned i = 0; i < cnt; i++) {
			uc[i].diacr = d[i].diacr;
			uc[i].base = d[i].base;
			uc[i].result = d[i].result;
		}
		kfree(d);
	}
	spin_lock_irqsave(&kb_lock, &flags);
	r = keymap_set_accents(&g_km, uc, cnt);
	spin_unlock_irqrestore(&kb_lock, flags);
	kfree(uc);
	return r;
}

static int kd_get_func(void *argp)
{
	unsigned char idx;
	char *buf;
	unsigned len = 0;
	uint64_t flags;
	int r = 0;

	if (copy_from_user(&idx, argp, 1))
		return -EFAULT;
	buf = (char *)kalloc(sizeof(((struct kbsentry *)0)->kb_string));
	if (!buf)
		return -ENOMEM;
	spin_lock_irqsave(&kb_lock, &flags);
	keymap_get_func(&g_km, idx, buf,
			sizeof(((struct kbsentry *)0)->kb_string));
	spin_unlock_irqrestore(&kb_lock, flags);
	while (buf[len])
		len++;
	if (copy_to_user((char *)argp + 1, buf, len + 1))
		r = -EFAULT;
	kfree(buf);
	return r;
}

static int kd_set_func(void *argp)
{
	struct kbsentry *kbs;
	char *s = NULL, *old;
	unsigned len = 0;
	uint64_t flags;

	kbs = (struct kbsentry *)kalloc(sizeof(*kbs));
	if (!kbs)
		return -ENOMEM;
	if (copy_from_user(kbs, argp, sizeof(*kbs))) {
		kfree(kbs);
		return -EFAULT;
	}
	kbs->kb_string[sizeof(kbs->kb_string) - 1] = 0;
	while (kbs->kb_string[len])
		len++;
	if (len) {
		s = (char *)kalloc(len + 1);
		if (!s) {
			kfree(kbs);
			return -ENOMEM;
		}
		for (unsigned i = 0; i <= len; i++)
			s[i] = (char)kbs->kb_string[i];
	}
	spin_lock_irqsave(&kb_lock, &flags);
	old = keymap_set_func(&g_km, kbs->kb_func, s);
	spin_unlock_irqrestore(&kb_lock, flags);
	if (old)
		kfree(old);
	kfree(kbs);
	return 0;
}

// KDSKBMODE (kbdmode >= 0) and KDSETMODE (kdmode >= 0), with the record of
// who changed them ("The console's modes and who set them").
static int kd_set_modes(task_t *cur, uint64_t token, int kbdmode, int kdmode)
{
	int grabbed = evdev_kbd_grabbed();
	int was_normal, change = 0, r = 0;
	uint8_t prev_kbdmode;
	uint64_t flags;

	spin_lock_irqsave(&kb_lock, &flags);
	// Permission is checked under the lock: whether the caller owns the
	// modes can change under it.
	if (!kd_permitted(cur) && !(token && token == kd_owner)) {
		spin_unlock_irqrestore(&kb_lock, flags);
		return -EPERM;
	}
	was_normal = kd_state_normal();
	prev_kbdmode = g_km.kbdmode;
	if (kbdmode >= 0)
		r = keymap_set_kbdmode(&g_km, (unsigned)kbdmode);
	if (!r && kdmode >= 0) {
		__atomic_store_n(&g_km.kdmode, (uint8_t)kdmode,
				 __ATOMIC_SEQ_CST);
		// The console's output gate follows the display mode in the
		// order the modes are set: noted here, under kb_lock.
		change = console_display_kd_note(kdmode == KD_GRAPHICS);
	}
	if (!r) {
		kd_note_change_locked(token, was_normal, prev_kbdmode);
		(void)keyboard_owner_update_locked(grabbed);
	}
	spin_unlock_irqrestore(&kb_lock, flags);

	// KD_GRAPHICS: the console stops painting (and its hardware cursor
	// goes) -- if this took the display.  Back to text: the console's
	// pointer works again and what the graphics program left is
	// repainted -- if this ended the display's ownership; while a DRM
	// master still holds it, its drop repaints instead.
	console_display_settle(change);
	return r;
}

int keyboard_kd_ioctl(unsigned long req, void *argp, task_t *cur,
		      uint64_t token)
{
	uintptr_t arg = (uintptr_t)argp;
	uint64_t flags;
	int r = 0;

	switch (req) {
	case KDGKBTYPE:
		return put_user_u8(argp, KB_101);

	case KDGETLED:
		return put_user_u8(argp, (uint8_t)keymap_leds(&g_km));
	case KDSETLED:
		if (!kd_permitted(cur))
			return -EPERM;
		spin_lock_irqsave(&kb_lock, &flags);
		keymap_set_led_override(&g_km, (unsigned)arg);
		keyboard_leds_update_locked();
		spin_unlock_irqrestore(&kb_lock, flags);
		return 0;

	case KDGKBLED:
		return put_user_u8(argp, (uint8_t)keymap_get_ledflags(&g_km));
	case KDSKBLED:
		if (!kd_permitted(cur))
			return -EPERM;
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_set_ledflags(&g_km, (unsigned)arg);
		keyboard_leds_update_locked();
		spin_unlock_irqrestore(&kb_lock, flags);
		return r;

	case KDGKBMODE:
		return put_user_int(argp, g_km.kbdmode);
	case KDSKBMODE:
		if (arg > K_OFF)
			return -EINVAL;
		return kd_set_modes(cur, token, (int)arg, -1);

	case KDGKBMETA:
		return put_user_int(argp, g_km.metamode);
	case KDSKBMETA:
		if (!kd_permitted(cur))
			return -EPERM;
		if (arg > 0xFF)
			return -EINVAL;
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_set_metamode(&g_km, (unsigned)arg);
		spin_unlock_irqrestore(&kb_lock, flags);
		return r;

	case KDGETMODE:
		return put_user_int(argp, g_km.kdmode);
	case KDSETMODE:
		if (arg != KD_TEXT && arg != KD_GRAPHICS)
			return -EINVAL;
		return kd_set_modes(cur, token, -1, (int)arg);

	case KDGKBENT: {
		struct kbentry ke;
		uint16_t v = 0;

		if (copy_from_user(&ke, argp, sizeof(ke)))
			return -EFAULT;
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_get_entry(&g_km, ke.kb_table, ke.kb_index, &v);
		spin_unlock_irqrestore(&kb_lock, flags);
		if (r)
			return r;
		ke.kb_value = v;
		return copy_to_user(argp, &ke, sizeof(ke)) ? -EFAULT : 0;
	}
	case KDSKBENT: {
		struct kbentry ke;

		if (!kd_permitted(cur))
			return -EPERM;
		if (copy_from_user(&ke, argp, sizeof(ke)))
			return -EFAULT;
		return kd_set_entry(&ke);
	}

	case KDGKBSENT:
		return kd_get_func(argp);
	case KDSKBSENT:
		if (!kd_permitted(cur))
			return -EPERM;
		return kd_set_func(argp);

	case KDGKBDIACR:
		return kd_get_diacrs(argp, 0);
	case KDGKBDIACRUC:
		return kd_get_diacrs(argp, 1);
	case KDSKBDIACR:
		if (!kd_permitted(cur))
			return -EPERM;
		return kd_set_diacrs(argp, 0);
	case KDSKBDIACRUC:
		if (!kd_permitted(cur))
			return -EPERM;
		return kd_set_diacrs(argp, 1);

	case KDGETKEYCODE: {
		struct kbkeycode k;

		if (copy_from_user(&k, argp, sizeof(k)))
			return -EFAULT;
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_get_keycode(&g_km, k.scancode, &k.keycode);
		spin_unlock_irqrestore(&kb_lock, flags);
		if (r)
			return r;
		return copy_to_user(argp, &k, sizeof(k)) ? -EFAULT : 0;
	}
	case KDSETKEYCODE: {
		struct kbkeycode k;

		if (!kd_permitted(cur))
			return -EPERM;
		if (copy_from_user(&k, argp, sizeof(k)))
			return -EFAULT;
		spin_lock_irqsave(&kb_lock, &flags);
		r = keymap_set_keycode(&g_km, k.scancode, k.keycode);
		spin_unlock_irqrestore(&kb_lock, flags);
		return r;
	}

	default:
		return -ENOTTY;
	}
}
