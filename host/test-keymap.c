/*
 * Host test of the console keymap engine (kernel/dev/input/keymap.c) and of
 * the kbd port's loadkeys/dumpkeys against it.  Driven by host/test-keymap.sh.
 *
 * The engine is compiled unchanged for the host.  This file stands in for
 * the kernel around it:
 *
 *  - ioctl() and isatty() are interposed.  A descriptor of /dev/null is "the
 *    console": its KD* requests go to the engine, answered the way
 *    keyboard_kd_ioctl() (kernel/dev/input/keyboard.c) answers them, so the
 *    real loadkeys and dumpkeys code of the port runs against the real
 *    engine.  KBDTEST_NOCONSOLE=1 makes the console unreachable (KDGKBTYPE
 *    fails), KBDTEST_EPERM=1 makes every request that changes the keyboard
 *    answer EPERM, as for a process that is not allowed to.
 *
 *  - The engine's tables live in a file between the programs of one test:
 *    KBDTEST_STATE is read at start, KBDTEST_STATE_OUT written at exit.
 *
 *  - With KBDTEST_FEED_MAIN this file is also the feeder: its arguments are
 *    set-1 scancode bytes in hex, run through the decoder and the engine;
 *    what the console terminal would receive is printed, bytes >= 0x80 as
 *    they are (UTF-8), ESC as \e, other control bytes as \xHH, the console
 *    hotkeys as [TASKS]/[NET], and the LEDs at the end as "leds=N".
 *
 * Copyright (C) 2026 The LikeOS Project
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

#include "kernel/dev/input/keymap.h"

static struct keymap_state km;
static int km_ready;

/* ---- state file ------------------------------------------------------ */

static void state_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint16_t tab[NR_KEYS];
	unsigned char flag;
	uint8_t modes[5];
	unsigned n;

	if (!f)
		return;
	km.kbdmode = K_UNICODE;
	for (unsigned t = 0; t < MAX_NR_KEYMAPS; t++) {
		uint16_t *spare = NULL, *freed = NULL;

		if (fread(&flag, 1, 1, f) != 1)
			goto out;
		if (!flag) {
			keymap_set_entry(&km, t, 0, K_NOSUCHMAP, &spare, &freed);
			free(freed);
			continue;
		}
		if (fread(tab, sizeof(tab), 1, f) != 1)
			goto out;
		for (unsigned i = 1; i < NR_KEYS; i++) {
			spare = malloc(sizeof(tab));
			keymap_set_entry(&km, t, i, tab[i], &spare, &freed);
			free(spare);
			free(freed);
		}
	}
	for (unsigned i = 0; i < MAX_NR_FUNC; i++) {
		uint16_t len;
		char *s = NULL;

		if (fread(&len, 2, 1, f) != 1)
			goto out;
		if (len) {
			s = calloc(1, len + 1u);
			if (fread(s, len, 1, f) != 1)
				goto out;
		}
		free(keymap_set_func(&km, i, s));
	}
	if (fread(&n, sizeof(n), 1, f) == 1 && n <= MAX_DIACR) {
		struct kbdiacruc a[MAX_DIACR];

		if (fread(a, sizeof(a[0]), n, f) == n)
			keymap_set_accents(&km, a, n);
	}
	if (fread(modes, sizeof(modes), 1, f) == 1) {
		km.kbdmode = modes[0];
		km.metamode = modes[1];
		km.ledflags = modes[2];
		km.default_ledflags = modes[3];
		km.lockstate = modes[4];
	}
	if (fread(km.kc_plain, sizeof(km.kc_plain), 1, f) != 1 ||
	    fread(km.kc_e0, sizeof(km.kc_e0), 1, f) != 1)
		goto out;
out:
	fclose(f);
}

static void state_save(void)
{
	const char *path = getenv("KBDTEST_STATE_OUT");
	FILE *f;
	uint8_t modes[5];

	if (!path || !km_ready)
		return;
	f = fopen(path, "wb");
	if (!f)
		return;
	for (unsigned t = 0; t < MAX_NR_KEYMAPS; t++) {
		unsigned char flag = km.maps[t] != NULL;

		fwrite(&flag, 1, 1, f);
		if (flag) {
			uint16_t tab[NR_KEYS];

			memcpy(tab, km.maps[t], sizeof(tab));
			tab[0] = 0;
			fwrite(tab, sizeof(tab), 1, f);
		}
	}
	for (unsigned i = 0; i < MAX_NR_FUNC; i++) {
		char buf[512];
		uint16_t len;

		keymap_get_func(&km, i, buf, sizeof(buf));
		len = (uint16_t)strlen(buf);
		fwrite(&len, 2, 1, f);
		fwrite(buf, len, 1, f);
	}
	{
		struct kbdiacruc a[MAX_DIACR];
		unsigned n = keymap_get_accents(&km, a, MAX_DIACR);

		fwrite(&n, sizeof(n), 1, f);
		fwrite(a, sizeof(a[0]), n, f);
	}
	modes[0] = km.kbdmode;
	modes[1] = km.metamode;
	modes[2] = km.ledflags;
	modes[3] = km.default_ledflags;
	modes[4] = km.lockstate;
	fwrite(modes, sizeof(modes), 1, f);
	fwrite(km.kc_plain, sizeof(km.kc_plain), 1, f);
	fwrite(km.kc_e0, sizeof(km.kc_e0), 1, f);
	fclose(f);
}

static void km_setup(void)
{
	const char *st;

	if (km_ready)
		return;
	keymap_init(&km);
	km_ready = 1;
	st = getenv("KBDTEST_STATE");
	if (st)
		state_load(st);
	atexit(state_save);
}

/* ---- the console: interposed isatty() and ioctl() -------------------- */

static int is_fake_console(int fd)
{
	struct stat sb;

	return fstat(fd, &sb) == 0 && S_ISCHR(sb.st_mode) &&
	       major(sb.st_rdev) == 1 && minor(sb.st_rdev) == 3;
}

int isatty(int fd)
{
	if (is_fake_console(fd))
		return 1;
	return syscall(SYS_ioctl, fd, 0x5401 /* TCGETS */, (char[64]){ 0 }) ==
	       0;
}

/* What keyboard_kd_ioctl() does, with the user copies as plain copies. */
static int kd_ioctl(unsigned long req, void *argp)
{
	uintptr_t arg = (uintptr_t)argp;
	int perm = !getenv("KBDTEST_EPERM");

	switch (req) {
	case KDGKBTYPE:
		if (getenv("KBDTEST_NOCONSOLE"))
			return -ENOTTY;
		*(char *)argp = KB_101;
		return 0;
	case KDGETLED:
		*(char *)argp = (char)keymap_leds(&km);
		return 0;
	case KDSETLED:
		if (!perm)
			return -EPERM;
		keymap_set_led_override(&km, (unsigned)arg);
		return 0;
	case KDGKBLED:
		*(char *)argp = (char)keymap_get_ledflags(&km);
		return 0;
	case KDSKBLED:
		if (!perm)
			return -EPERM;
		return keymap_set_ledflags(&km, (unsigned)arg);
	case KDGKBMODE:
		*(int *)argp = km.kbdmode;
		return 0;
	case KDSKBMODE:
		if (!perm)
			return -EPERM;
		return keymap_set_kbdmode(&km, (unsigned)arg);
	case KDGKBMETA:
		*(int *)argp = km.metamode;
		return 0;
	case KDSKBMETA:
		if (!perm)
			return -EPERM;
		return keymap_set_metamode(&km, (unsigned)arg);
	case KDGETMODE:
		*(int *)argp = km.kdmode;
		return 0;
	case KDSETMODE:
		if (!perm)
			return -EPERM;
		if (arg != KD_TEXT && arg != KD_GRAPHICS)
			return -EINVAL;
		km.kdmode = (uint8_t)arg;
		return 0;
	case KDGKBENT: {
		struct kbentry *ke = argp;
		uint16_t v;
		int r = keymap_get_entry(&km, ke->kb_table, ke->kb_index, &v);

		if (!r)
			ke->kb_value = v;
		return r;
	}
	case KDSKBENT: {
		struct kbentry *ke = argp;
		uint16_t *spare = NULL, *freed = NULL;
		int r;

		if (!perm)
			return -EPERM;
		r = keymap_set_entry(&km, ke->kb_table, ke->kb_index,
				     ke->kb_value, &spare, &freed);
		free(freed);
		if (r == -ENOMEM) {
			spare = malloc(NR_KEYS * sizeof(uint16_t));
			r = keymap_set_entry(&km, ke->kb_table, ke->kb_index,
					     ke->kb_value, &spare, &freed);
			free(spare);
			free(freed);
		}
		return r;
	}
	case KDGKBSENT: {
		struct kbsentry *k = argp;

		keymap_get_func(&km, k->kb_func, (char *)k->kb_string,
				sizeof(k->kb_string));
		return 0;
	}
	case KDSKBSENT: {
		struct kbsentry *k = argp;
		char *s = NULL;

		if (!perm)
			return -EPERM;
		k->kb_string[sizeof(k->kb_string) - 1] = 0;
		if (k->kb_string[0])
			s = strdup((char *)k->kb_string);
		free(keymap_set_func(&km, k->kb_func, s));
		return 0;
	}
	case KDGKBDIACRUC: {
		struct kbdiacrsuc *d = argp;

		d->kb_cnt = keymap_get_accents(&km, d->kbdiacruc, MAX_DIACR);
		return 0;
	}
	case KDGKBDIACR: {
		struct kbdiacrs *d = argp;
		struct kbdiacruc uc[MAX_DIACR];
		unsigned n = keymap_get_accents(&km, uc, MAX_DIACR), cnt = 0;

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
		return 0;
	}
	case KDSKBDIACRUC: {
		struct kbdiacrsuc *d = argp;

		if (!perm)
			return -EPERM;
		if (d->kb_cnt > MAX_DIACR)
			return -EINVAL;
		return keymap_set_accents(&km, d->kbdiacruc, d->kb_cnt);
	}
	case KDSKBDIACR: {
		struct kbdiacrs *d = argp;
		struct kbdiacruc uc[MAX_DIACR];

		if (!perm)
			return -EPERM;
		if (d->kb_cnt > MAX_DIACR)
			return -EINVAL;
		for (unsigned i = 0; i < d->kb_cnt; i++) {
			uc[i].diacr = d->kbdiacr[i].diacr;
			uc[i].base = d->kbdiacr[i].base;
			uc[i].result = d->kbdiacr[i].result;
		}
		return keymap_set_accents(&km, uc, d->kb_cnt);
	}
	case KDGETKEYCODE: {
		struct kbkeycode *k = argp;

		return keymap_get_keycode(&km, k->scancode, &k->keycode);
	}
	case KDSETKEYCODE: {
		struct kbkeycode *k = argp;

		if (!perm)
			return -EPERM;
		return keymap_set_keycode(&km, k->scancode, k->keycode);
	}
	default:
		return -ENOTTY;
	}
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *argp;
	int r;

	va_start(ap, req);
	argp = va_arg(ap, void *);
	va_end(ap);
	if (!is_fake_console(fd) || (req & ~0xFFUL) != 0x4B00UL)
		return (int)syscall(SYS_ioctl, fd, req, argp);
	km_setup();
	r = kd_ioctl(req, argp);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}

__attribute__((constructor)) static void kbdtest_init(void)
{
	km_setup();
}

/* ---- the feeder --------------------------------------------------------- */

#ifdef KBDTEST_FEED_MAIN
static void print_out(const struct keymap_out *o)
{
	for (unsigned i = 0; i < o->len; i++) {
		unsigned char c = o->buf[i];

		if (c == 27)
			fputs("\\e", stdout);
		else if (c == '\\')
			fputs("\\\\", stdout);
		else if (c < 0x20 || c == 0x7f)
			printf("\\x%02x", c);
		else
			putchar(c);
	}
	if (o->actions & KEYMAP_ACT_SHOW_STATE)
		fputs("[SHOW_STATE]", stdout);
	if (o->actions & KEYMAP_ACT_SHOW_MEM)
		fputs("[SHOW_MEM]", stdout);
	if (o->actions & KEYMAP_ACT_SHOW_REGS)
		fputs("[SHOW_REGS]", stdout);
	if (o->actions & KEYMAP_ACT_BREAK)
		fputs("[BREAK]", stdout);
	if (o->actions & KEYMAP_ACT_SCROLL_BACK)
		fputs("[SCROLL_BACK]", stdout);
	if (o->actions & KEYMAP_ACT_SCROLL_FWD)
		fputs("[SCROLL_FWD]", stdout);
}

int main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		unsigned kc[2];
		int down[2];
		uint8_t sc = (uint8_t)strtoul(argv[a], NULL, 16);
		int n = keymap_decode(&km, sc, kc, down);

		for (int i = 0; i < n; i++) {
			struct keymap_out o = { 0 };

			// The console hotkeys, as keyboard_hotkey() decides.
			if (down[i] &&
			    (km.kbdmode == K_UNICODE || km.kbdmode == K_XLATE) &&
			    keymap_ctrl_held(&km) && !keymap_alt_held(&km)) {
				int32_t c = keymap_peek_char(&km, kc[i]);

				if (c == 0x04 || c == 0x0e) {
					keymap_note_key(&km, kc[i], 1);
					fputs(c == 0x04 ? "[TASKS]" : "[NET]",
					      stdout);
					continue;
				}
			}
			keymap_key(&km, kc[i], down[i], &o);
			print_out(&o);
		}
	}
	printf(" leds=%u\n", keymap_leds(&km));
	return 0;
}
#endif
