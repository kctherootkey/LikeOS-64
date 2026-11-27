/* Kernel services for the keyboard part of the input core harness
 * (host/test-input.sh, group "keyboard").
 *
 * The real kernel/dev/input/keyboard.c and keymap.c are linked into the
 * harness; what they call outside the input layer is simulated here: the
 * console terminal records what it is typed, the debug dumps count their
 * calls, the console's output gate (console_owner.h) is fbdev.c's with its
 * takes and repaints counted, and whether the caller is privileged is a
 * flag the test sets (capable).  No PS/2 keyboard is ever "seen", so the
 * LED exchange never touches an I/O port; the LEDs are read back through
 * keyboard_led_state(), which the USB keyboards follow.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/io/tty.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/video/console_owner.h>
#include <kernel/uapi/bug.h>
#include "kstubs.h"

static tty_t g_console;
static char g_typed[256];
static int g_ntyped;
static int g_dumps;
static int g_redraws;
static int g_capable = 1;

static void typed(char c)
{
	if (g_ntyped < (int)sizeof(g_typed) - 1)
		g_typed[g_ntyped++] = c;
}

tty_t *tty_get_console(void)
{
	g_console.term.c_cc[VINTR] = 3;
	return &g_console;
}

void tty_input_char(tty_t *tty, char c, int ctrl)
{
	(void)tty;
	(void)ctrl;
	typed(c);
}

void tty_input_char_raw(tty_t *tty, char c)
{
	(void)tty;
	typed(c);
}

void sched_dump_tasks(struct tty *tty)
{
	(void)tty;
	__atomic_fetch_add(&g_dumps, 1, __ATOMIC_RELAXED);
}

void tcp_dump_table(struct tty *tty)
{
	(void)tty;
	__atomic_fetch_add(&g_dumps, 1, __ATOMIC_RELAXED);
}

void unix_dump_sockets(struct tty *tty)
{
	(void)tty;
}

void tty_dump_ptys(struct tty *tty)
{
	(void)tty;
}

void mm_get_memory_stats(memory_stats_t *stats)
{
	kmemset(stats, 0, sizeof(*stats));
}

void console_get_dimensions(uint32_t *rows, uint32_t *cols)
{
	*rows = 25;
	*cols = 80;
}

void console_scroll_up_line(void)
{
}

void console_scroll_down_line(void)
{
}

uint64_t timer_ticks(void)
{
	return timer_get_precise_ns() / 1000000ULL;
}

uint64_t timer_ms_to_ticks(uint64_t ms)
{
	return ms;
}

void entropy_add_interrupt_timing(uint64_t extra)
{
	(void)extra;
}

int capable(void)
{
	return __atomic_load_n(&g_capable, __ATOMIC_ACQUIRE);
}

int copy_from_user(void *kernel_dst, const void *user_src, size_t len)
{
	kmemcpy(kernel_dst, user_src, len);
	return 0;
}

int copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	kmemcpy(user_dst, kernel_src, len);
	return 0;
}

/* The console's output gate, as fbdev.c has it: the same state machine
 * (console_owner.h); the actions are counted -- a take that still finds the
 * display owned (the hardware cursor goes), a release that still finds it
 * free (the repaint). */
static unsigned g_owners;
static int g_takes;

int console_display_owned(void)
{
	return console_owner_is_owned(&g_owners);
}

int console_display_kd_note(int graphics)
{
	return (int)console_owner_set_kd(&g_owners, graphics);
}

int console_display_master_note(int set)
{
	int underflow;
	enum console_owner_change c;

	if (set)
		return (int)console_owner_master_set(&g_owners);
	c = console_owner_master_drop(&g_owners, &underflow);
	WARN_ON(underflow);
	return (int)c;
}

void console_display_settle(int change)
{
	if (change == CONSOLE_OWNER_TAKEN) {
		if (console_display_owned())
			__atomic_fetch_add(&g_takes, 1, __ATOMIC_RELAXED);
	} else if (change == CONSOLE_OWNER_RELEASED) {
		if (!console_display_owned())
			__atomic_fetch_add(&g_redraws, 1, __ATOMIC_RELAXED);
	}
}

/* ---- for the test scenarios ---- */

void test_set_capable(int on)
{
	__atomic_store_n(&g_capable, on, __ATOMIC_RELEASE);
}

int test_console_redraws(void)
{
	return __atomic_load_n(&g_redraws, __ATOMIC_RELAXED);
}

int test_console_take(char *out, int cap)
{
	int n = g_ntyped < cap - 1 ? g_ntyped : cap - 1;

	kmemcpy(out, g_typed, (size_t)n);
	out[n] = 0;
	g_ntyped = 0;
	return n;
}

int test_console_dumps(void)
{
	return __atomic_load_n(&g_dumps, __ATOMIC_RELAXED);
}

void test_display_master(int set)
{
	console_display_settle(console_display_master_note(set));
}

int test_display_takes(void)
{
	return __atomic_load_n(&g_takes, __ATOMIC_RELAXED);
}
