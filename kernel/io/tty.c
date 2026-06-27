// LikeOS-64 TTY/PTY subsystem
#include <kernel/io/tty.h>
#include <kernel/io/vt.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>
#include <kernel/dev/usb/usb_serial.h>
#include <kernel/ke/signal.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/net/net.h>
#include <kernel/uapi/bug.h>

#define TTY_MAX_PTYS 16

// Spinlock for TTY buffer access
static spinlock_t tty_lock = SPINLOCK_INIT("tty");

// Security: Validate user pointer is in user space
static bool tty_validate_user_ptr(uint64_t ptr, size_t len)
{
	if (ptr < 0x10000)
		return false; // Reject low addresses (NULL deref protection)
	if (ptr >= 0x7FFFFFFFFFFF)
		return false; // Beyond user space
	if (ptr + len < ptr)
		return false; // Overflow check
	return true;
}

// Security: SMAP-aware copy from kernel to user
static int tty_copy_to_user(void *user_dst, const void *kernel_src, size_t len)
{
	if (!tty_validate_user_ptr((uint64_t)user_dst, len)) {
		return -EFAULT;
	}
	smap_disable();
	mm_memcpy(user_dst, kernel_src, len);
	smap_enable();
	return 0;
}

// Security: SMAP-aware copy from user to kernel
static int tty_copy_from_user(void *kernel_dst, const void *user_src,
			      size_t len)
{
	if (!tty_validate_user_ptr((uint64_t)user_src, len)) {
		return -EFAULT;
	}
	smap_disable();
	mm_memcpy(kernel_dst, user_src, len);
	smap_enable();
	return 0;
}

/* PTY master ring buffer.
 *
 * The slave-side process writes here through tty_write -> tty->output.
 * The master-side reader (e.g. tmux) drains it through tty_pty_master_read.
 *
 * Sizing: must accommodate one full screen redraw of a full-screen app
 * (top, less, nano) plus enough headroom for bursts like `dmesg` or
 * `ps aux` (which write tens of KiB in a tight loop).  64 KiB is
 * comfortable for any reasonable terminal size; the master reader
 * drains at IRQ-rate so it almost never fills.
 *
 * On overflow we drop trailing bytes (non-blocking).  We do NOT block
 * the slave writer on a wait queue — a slave that gets killed while
 * blocked there would be reaped while still linked into the queue,
 * and the next wake would walk freed memory.  With 64 KiB and no SMP
 * race on the indices, drops are vanishingly rare for normal apps.
 *
 * Locking: 'lock' protects master_buf/m_head/m_tail/m_count and
 * slave_open.  Acquired with IRQs disabled because tty_pty_master_read
 * can be called from syscall context on one CPU while a slave write
 * runs on another.
 */
#define PTY_MASTER_BUF_SIZE 262144u
typedef struct pty {
	int id;
	tty_t slave;
	char master_buf[PTY_MASTER_BUF_SIZE];
	uint32_t m_head;
	uint32_t m_tail;
	uint32_t m_count;
	spinlock_t lock;
	task_t *master_read_waiters;
	int master_open;
	int slave_open;
} pty_t;

static tty_t g_console_tty;
static pty_t g_ptys[TTY_MAX_PTYS];
/* Points to the PTY slave that received keyboard input most recently.
 * Updated in tty_pty_master_write whenever tmux (or any app) forwards
 * a keystroke to a pane.  Used by Ctrl+D/Ctrl+N dump hotkeys to route
 * output into the currently active tmux pane instead of the raw console. */
static tty_t *g_active_tty = NULL;

/* Wake all tasks parked on a wait queue identified by 'waiters'.
 *
 * The pointer value (e.g. &tty->read_waiters or &pty->master_read_waiters)
 * is used purely as a stable channel identifier — we no longer maintain a
 * linked list rooted at *waiters.  Instead, sched_wake_channel walks the
 * global task list (under g_task_list_lock) and wakes every task whose
 * wait_channel matches this address.
 *
 * This avoids a use-after-free that the old linked-list design suffered
 * from: when a task blocked here was killed (e.g. SIGKILL of nano from a
 * second tmux pane), sched_remove_task freed the task struct without
 * unlinking it from any custom wait queues, and the next wake walked
 * t->wait_next through freed memory and page-faulted in tty_wake_readers.
 *
 * 'waiters' itself is no longer touched (kept as a parameter only so
 * existing call sites need no change).
 */
static void tty_wake_readers(task_t **waiters)
{
	sched_wake_channel((void *)waiters);
	/* Also wake any task sleeping in poll/select/epoll_wait so that it
     * re-scans its fd set immediately instead of waiting for the next
     * timer tick.  Without this, programs that multiplex stdin and a
     * socket via poll() (e.g. nc, openssl) see up to one tick of lag
     * per keystroke. */
	extern void poll_notify_io_ready(void);
	poll_notify_io_ready();
}

static void tty_enqueue_read(tty_t *tty, char c)
{
	uint64_t flags;
	spin_lock_irqsave(&tty_lock, &flags);

	if (tty->read_count >= sizeof(tty->read_buf)) {
		WARN_RATELIMIT(
			1,
			"tty_enqueue_read: input dropped (read_buf full, count=%u)",
			tty->read_count);
		spin_unlock_irqrestore(&tty_lock, flags);
		return;
	}
	tty->read_buf[tty->read_tail] = c;
	tty->read_tail = (tty->read_tail + 1) % sizeof(tty->read_buf);
	tty->read_count++;

	spin_unlock_irqrestore(&tty_lock, flags);
}

/* Lock-free variant: caller MUST already hold tty_lock.  Used from the
 * VT layer's DSR/DA reply injection, which runs under tty_write's lock
 * and would otherwise self-deadlock the same CPU.  Non-static: also
 * referenced by vt.c (extern declaration there).                       */
void tty_enqueue_read_locked(tty_t *tty, char c)
{
	lockdep_assert_held(&tty_lock);
	if (tty->read_count >= sizeof(tty->read_buf))
		return;
	tty->read_buf[tty->read_tail] = c;
	tty->read_tail = (tty->read_tail + 1) % sizeof(tty->read_buf);
	tty->read_count++;
}

static int tty_dequeue_read(tty_t *tty, char *out)
{
	BUG_ON(tty == NULL);
	uint64_t flags;
	spin_lock_irqsave(&tty_lock, &flags);

	if (tty->read_count == 0) {
		spin_unlock_irqrestore(&tty_lock, flags);
		return 0;
	}
	*out = tty->read_buf[tty->read_head];
	tty->read_head = (tty->read_head + 1) % sizeof(tty->read_buf);
	tty->read_count--;
	WARN_ON((int)tty->read_count < 0); /* read_count underflow */

	spin_unlock_irqrestore(&tty_lock, flags);
	return 1;
}
static pty_t *tty_get_pty(int id)
{
	BUG_ON(id < 0 || id >= TTY_MAX_PTYS);
	if (id < 0 || id >= TTY_MAX_PTYS) {
		return NULL;
	}
	if (g_ptys[id].id != id) {
		return NULL;
	}
	return &g_ptys[id];
}

/* Bulk non-blocking enqueue into the master ring.  Returns bytes
 * actually copied (may be less than len if the ring fills).  Wakes any
 * blocked master reader if at least one byte was accepted. */
static long pty_master_enqueue_bulk(pty_t *pty, const char *buf, long len)
{
	if (!pty || !buf || len <= 0)
		return 0;
	uint64_t flags;
	spin_lock_irqsave(&pty->lock, &flags);
	uint32_t free = PTY_MASTER_BUF_SIZE - pty->m_count;
	uint32_t to_copy = (uint32_t)len;
	int dropped = 0;
	if (to_copy > free) {
		to_copy = free;
		dropped = 1;
	}
	for (uint32_t i = 0; i < to_copy; i++) {
		pty->master_buf[pty->m_tail] = buf[i];
		pty->m_tail = (pty->m_tail + 1) % PTY_MASTER_BUF_SIZE;
	}
	pty->m_count += to_copy;
	WARN_ON(pty->m_count >
		PTY_MASTER_BUF_SIZE); /* PTY master ring buffer overflow: m_count > buffer capacity */
	spin_unlock_irqrestore(&pty->lock, flags);
	if (to_copy > 0) {
		tty_wake_readers(&pty->master_read_waiters);
	}
	return (long)to_copy;
}

/* Single-byte non-blocking enqueue.  Used by the ECHO path inside
 * tty_input_char (master-side context: keystroke arriving from the
 * master's tty_pty_master_write).  Drops on full — ECHO is one byte at
 * a time so loss is benign. */
static void pty_master_enqueue(pty_t *pty, char c)
{
	if (!pty) {
		return;
	}
	pty_master_enqueue_bulk(pty, &c, 1);
}

static void tty_output_pty_slave(tty_t *tty, char c)
{
	if (!tty || !tty->priv) {
		return;
	}
	pty_t *pty = (pty_t *)tty->priv;
	pty_master_enqueue(pty, c);
}

static void tty_set_default_termios(tty_t *tty)
{
	tty->term.c_iflag = ICRNL;
	/* Output post-processing on by default: translate bare LF to CR+LF.
     * Without this, programs like ls/ps that emit only '\n' between
     * lines render the next line starting at the previous column,
     * producing the "run-on" appearance.  Apps that need raw output
     * (tmux master, full-screen apps) clear OPOST via tcsetattr. */
	tty->term.c_oflag = OPOST | ONLCR;
	tty->term.c_cflag = 0;
	tty->term.c_lflag = ISIG | ICANON | ECHO;
	tty->term.c_cc[VINTR] = 3; // Ctrl+C
	tty->term.c_cc[VQUIT] = 28; // Ctrl+\
    // Use volatile to prevent compiler from optimizing away the write
	volatile cc_t *verase_ptr = &tty->term.c_cc[VERASE];
	*verase_ptr = 8; // Backspace (ASCII 8)
	tty->term.c_cc[VKILL] = 21; // Ctrl+U
	tty->term.c_cc[VEOF] = 4; // Ctrl+D
	tty->term.c_cc[VSTART] = 17; // Ctrl+Q
	tty->term.c_cc[VSTOP] = 19; // Ctrl+S
	tty->term.c_cc[VSUSP] = 26; // Ctrl+Z
	tty->term.c_cc[VMIN] = 1; // Block until at least 1 byte available
	tty->term.c_cc[VTIME] = 0; // No timeout
}

void tty_init(void)
{
	mm_memset(&g_console_tty, 0, sizeof(g_console_tty));
	g_console_tty.id = 1; // 1-based so 0 means "no tty"
	g_console_tty.is_pty = 0;
	g_console_tty.is_master = 0;
	g_console_tty.fg_pgid = 0;
	g_console_tty.output = vt_output_char;

	/* Query actual console dimensions from the framebuffer driver */
	uint32_t rows = 25, cols = 80;
	console_get_dimensions(&rows, &cols);
	g_console_tty.winsz.ws_row = (unsigned short)rows;
	g_console_tty.winsz.ws_col = (unsigned short)cols;

	/* Initialise the VT terminal emulator for the console TTY. */
	vt_init(&g_console_vt, (int)cols, (int)rows, &g_console_tty);
	tty_set_default_termios(&g_console_tty);

	for (int i = 0; i < TTY_MAX_PTYS; ++i) {
		mm_memset(&g_ptys[i], 0, sizeof(pty_t));
		g_ptys[i].id = -1;
		spinlock_init(&g_ptys[i].lock, "pty");
	}
}

tty_t *tty_get_console(void)
{
	return &g_console_tty;
}

/* Returns the most recently active PTY slave (the pane with keyboard focus),
 * or the console TTY if no PTY has ever received input. */
tty_t *tty_get_active(void)
{
	tty_t *active = __atomic_load_n(&g_active_tty, __ATOMIC_RELAXED);
	return active ? active : &g_console_tty;
}

void tty_reset_termios(tty_t *tty)
{
	if (!tty) {
		return;
	}
	tty_set_default_termios(tty);
	tty->canon_len = 0;
	tty->read_head = 0;
	tty->read_tail = 0;
	tty->read_count = 0;
	tty->eof_pending = 0;
}

static void tty_signal_pgrp(tty_t *tty, int sig)
{
	if (!tty || tty->fg_pgid == 0) {
		return;
	}
	sched_signal_pgrp(tty->fg_pgid, sig);
	// Wake any blocked readers so they can see they've been signaled/killed
	tty_wake_readers(&tty->read_waiters);
}

void tty_input_char_raw(tty_t *tty, char c)
{
	if (!tty)
		return;
	tty_enqueue_read(tty, c);
	tty_wake_readers(&tty->read_waiters);
}

/* Helper: inject a string into the TTY read buffer (raw, no line discipline) */
static void tty_inject_string(tty_t *tty, const char *s)
{
	while (*s) {
		tty_enqueue_read(tty, *s++);
	}
}

/* Simple integer-to-decimal helper for escape sequence generation.
 * Non-static: also used by vt.c (extern declaration there).            */
int itoa_simple(int val, char *buf)
{
	if (val < 0)
		val = 0;
	char tmp[12];
	int len = 0;
	if (val == 0) {
		tmp[len++] = '0';
	} else {
		while (val > 0) {
			tmp[len++] = '0' + (val % 10);
			val /= 10;
		}
	}
	/* reverse */
	for (int i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	buf[len] = '\0';
	return len;
}

/*
 * tty_mouse_report - Generate SGR mouse escape sequences from mouse events.
 * Called from the mouse IRQ handler when mouse tracking modes are enabled.
 *
 * pixel_x, pixel_y: mouse position in pixels
 * buttons: current button state bitmask (bit0=left, bit1=right, bit2=middle)
 * prev_buttons: previous button state bitmask (for detecting press/release)
 *
 * SGR format: \033[<Cb;Cx;CyM  (press) or \033[<Cb;Cx;Cym  (release)
 *   Cb: 0=left, 1=middle, 2=right, +32=motion, +64=scroll
 *   Cx, Cy: 1-based cell coordinates
 */
void tty_mouse_report(int pixel_x, int pixel_y, uint8_t buttons,
		      uint8_t prev_buttons)
{
	tty_t *tty = &g_console_tty;

	/* Only report if mouse tracking is enabled */
	if (!tty->mouse_tracking && !tty->mouse_btn_event)
		return;

	/* Convert pixel coordinates to cell coordinates (1-based) */
	uint32_t rows, cols;
	console_get_dimensions(&rows, &cols);
	if (rows == 0 || cols == 0)
		return;

	/* Get character dimensions: default 8x16 but use actual from console */
	uint32_t cw = 8, ch = 16; /* default char size */
	/* We use screen_width/cols and screen_height/rows for cell size */
	/* Actually, use console char dimensions directly */
	extern uint32_t char_width;
	extern uint32_t char_height;
	cw = char_width;
	ch = char_height;
	if (cw == 0)
		cw = 8;
	if (ch == 0)
		ch = 16;

	int cx = (pixel_x / (int)cw) + 1; /* 1-based column */
	int cy = (pixel_y / (int)ch) + 1; /* 1-based row */
	if (cx < 1)
		cx = 1;
	if (cy < 1)
		cy = 1;
	if (cx > (int)cols)
		cx = (int)cols;
	if (cy > (int)rows)
		cy = (int)rows;

	/* Detect button changes */
	uint8_t pressed = buttons & ~prev_buttons; /* newly pressed */
	uint8_t released = prev_buttons & ~buttons; /* newly released */
	int motion = (buttons == prev_buttons) && (buttons != 0);

/* Helper macro for building and injecting an SGR mouse sequence */
#define INJECT_SGR_MOUSE(cb_val, is_release)           \
	do {                                           \
		char seq[32];                          \
		int pos = 0;                           \
		seq[pos++] = '\033';                   \
		seq[pos++] = '[';                      \
		seq[pos++] = '<';                      \
		pos += itoa_simple(cb_val, seq + pos); \
		seq[pos++] = ';';                      \
		pos += itoa_simple(cx, seq + pos);     \
		seq[pos++] = ';';                      \
		pos += itoa_simple(cy, seq + pos);     \
		seq[pos++] = (is_release) ? 'm' : 'M'; \
		seq[pos] = '\0';                       \
		tty_inject_string(tty, seq);           \
	} while (0)

	/* Report button presses */
	if (pressed & 0x01) { /* left press */
		INJECT_SGR_MOUSE(0, 0);
	}
	if (pressed & 0x04) { /* middle press */
		INJECT_SGR_MOUSE(1, 0);
	}
	if (pressed & 0x02) { /* right press */
		INJECT_SGR_MOUSE(2, 0);
	}

	/* Report button releases */
	if (released & 0x01) { /* left release */
		INJECT_SGR_MOUSE(0, 1);
	}
	if (released & 0x04) { /* middle release */
		INJECT_SGR_MOUSE(1, 1);
	}
	if (released & 0x02) { /* right release */
		INJECT_SGR_MOUSE(2, 1);
	}

	/* Report motion with button held (mode 1002: button-event tracking) */
	if (motion && tty->mouse_btn_event) {
		int cb = 32; /* motion flag */
		if (buttons & 0x01)
			cb |= 0; /* left + motion */
		else if (buttons & 0x04)
			cb |= 1; /* middle + motion */
		else if (buttons & 0x02)
			cb |= 2; /* right + motion */
		INJECT_SGR_MOUSE(cb, 0);
	}

#undef INJECT_SGR_MOUSE

	/* Wake readers waiting for input */
	if (pressed || released || (motion && tty->mouse_btn_event))
		tty_wake_readers(&tty->read_waiters);
}

/*
 * tty_mouse_report_scroll - Generate SGR mouse escape for scroll wheel events.
 * scroll_delta: negative = scroll up, positive = scroll down
 */
void tty_mouse_report_scroll(int pixel_x, int pixel_y, int scroll_delta)
{
	tty_t *tty = &g_console_tty;

	if (!tty->mouse_tracking && !tty->mouse_btn_event)
		return;

	uint32_t rows, cols;
	console_get_dimensions(&rows, &cols);
	if (rows == 0 || cols == 0)
		return;

	extern uint32_t char_width;
	extern uint32_t char_height;
	uint32_t cw = char_width ? char_width : 8;
	uint32_t ch = char_height ? char_height : 16;

	int cx = (pixel_x / (int)cw) + 1;
	int cy = (pixel_y / (int)ch) + 1;
	if (cx < 1)
		cx = 1;
	if (cy < 1)
		cy = 1;
	if (cx > (int)cols)
		cx = (int)cols;
	if (cy > (int)rows)
		cy = (int)rows;

	/* SGR scroll: Cb=64 for scroll-up, Cb=65 for scroll-down */
	int cb = (scroll_delta < 0) ? 64 : 65;

	char seq[32];
	int pos = 0;
	seq[pos++] = '\033';
	seq[pos++] = '[';
	seq[pos++] = '<';
	pos += itoa_simple(cb, seq + pos);
	seq[pos++] = ';';
	pos += itoa_simple(cx, seq + pos);
	seq[pos++] = ';';
	pos += itoa_simple(cy, seq + pos);
	seq[pos++] = 'M';
	seq[pos] = '\0';
	tty_inject_string(tty, seq);
	tty_wake_readers(&tty->read_waiters);
}

void tty_input_char(tty_t *tty, char c, int ctrl)
{
	BUG_ON(tty == NULL);
	if (!tty || c == 0) {
		return;
	}

	if (ctrl && c >= 'A' && c <= 'Z') {
		c = (char)((c - 'A' + 1) & 0x1F);
	} else if (ctrl && c >= 'a' && c <= 'z') {
		c = (char)((c - 'a' + 1) & 0x1F);
	}

	if (tty->term.c_iflag & ICRNL) {
		if (c == '\r') {
			c = '\n';
		}
	}

	if (tty->term.c_lflag & ISIG) {
		if (c == tty->term.c_cc[VINTR]) {
			// Echo ^C if ECHO is set
			if (tty->term.c_lflag & ECHO) {
				tty->output(tty, '^');
				tty->output(tty, 'C');
				tty->output(tty, '\n');
			}
			// Clear any pending canonical input
			tty->canon_len = 0;
			tty_signal_pgrp(tty, SIGINT);
			return;
		}
		if (c == tty->term.c_cc[VQUIT]) {
			// Echo ^\\ if ECHO is set
			if (tty->term.c_lflag & ECHO) {
				tty->output(tty, '^');
				tty->output(tty, '\\');
				tty->output(tty, '\n');
			}
			tty->canon_len = 0;
			tty_signal_pgrp(tty, SIGQUIT);
			return;
		}
		if (c == tty->term.c_cc[VSUSP]) {
			// Echo ^Z if ECHO is set
			if (tty->term.c_lflag & ECHO) {
				tty->output(tty, '^');
				tty->output(tty, 'Z');
				tty->output(tty, '\n');
			}
			tty->canon_len = 0;
			tty_signal_pgrp(tty, SIGTSTP);
			return;
		}
	}

	if (tty->term.c_lflag & ICANON) {
		// Handle backspace: check VERASE (typically 8) or DEL (127)
		if (c == tty->term.c_cc[VERASE] || c == 127) {
			if (tty->canon_len > 0) {
				tty->canon_len--;
				if (tty->term.c_lflag & ECHO) {
					tty->output(tty, '\b');
					tty->output(tty, ' ');
					tty->output(tty, '\b');
				}
			}
			return;
		}
		if (c == tty->term.c_cc[VKILL]) {
			while (tty->canon_len > 0) {
				tty->canon_len--;
				if (tty->term.c_lflag & ECHO) {
					tty->output(tty, '\b');
					tty->output(tty, ' ');
					tty->output(tty, '\b');
				}
			}
			return;
		}
		if (c == tty->term.c_cc[VEOF]) {
			if (tty->canon_len == 0) {
				tty->eof_pending = 1;
				tty_wake_readers(&tty->read_waiters);
				return;
			}
			for (uint16_t i = 0; i < tty->canon_len; ++i) {
				tty_enqueue_read(tty, tty->canon_buf[i]);
			}
			tty->canon_len = 0;
			tty_wake_readers(&tty->read_waiters);
			return;
		}
		if (tty->canon_len < sizeof(tty->canon_buf)) {
			tty->canon_buf[tty->canon_len++] = c;
		}
		if (tty->term.c_lflag & ECHO) {
			if (c == '\n' && (tty->term.c_oflag & OPOST) &&
			    (tty->term.c_oflag & ONLCR)) {
				tty->output(tty, '\r');
			}
			tty->output(tty, c);
		}
		if (c == '\n') {
			for (uint16_t i = 0; i < tty->canon_len; ++i) {
				tty_enqueue_read(tty, tty->canon_buf[i]);
			}
			tty->canon_len = 0;
			tty_wake_readers(&tty->read_waiters);
		}
		return;
	}

	tty_enqueue_read(tty, c);
	if (tty->term.c_lflag & ECHO) {
		if (c == '\n' && (tty->term.c_oflag & OPOST) &&
		    (tty->term.c_oflag & ONLCR)) {
			tty->output(tty, '\r');
		}
		tty->output(tty, c);
	}
	tty_wake_readers(&tty->read_waiters);
}

long tty_read(tty_t *tty, void *buf, long count, int nonblock)
{
	if (!tty || !buf || count <= 0) {
		return 0;
	}

	task_t *cur = sched_current();
	char *out = (char *)buf;
	long read = 0;

	/* In non-canonical mode, honour VMIN / VTIME (POSIX semantics).
     * VMIN=0, VTIME=0 → never block (return 0 if nothing available).
     * VMIN=0, VTIME>0 → wait up to VTIME*100ms for a byte, then return 0.
     * VMIN>0, VTIME=0 → block until VMIN bytes available (default). */
	int vmin_zero = 0;
	uint64_t vtime_deadline = 0; /* 0 = no deadline */
	if (!(tty->term.c_lflag & ICANON) && tty->term.c_cc[VMIN] == 0) {
		vmin_zero = 1;
		unsigned char vtime = tty->term.c_cc[VTIME];
		if (vtime > 0) {
			/* VTIME is in tenths of a second; compute deadline in ticks */
			uint32_t freq = timer_get_frequency();
			uint64_t timeout_ticks =
				((uint64_t)vtime * freq + 9) / 10;
			vtime_deadline = timer_ticks() + timeout_ticks;
		} else {
			/* VMIN=0, VTIME=0: pure non-blocking */
			nonblock = 1;
		}
	}

	while (read < count) {
		// Check if we've been killed or have a pending signal
		if (cur && (cur->state == TASK_ZOMBIE || signal_pending(cur))) {
			// Handle pending signal
			if (signal_pending(cur)) {
				return read > 0 ? read : -EINTR;
			}
			return read > 0 ? read : -EINTR;
		}
		if (tty->eof_pending && tty->read_count == 0) {
			tty->eof_pending = 0;
			return read;
		}
		char c = 0;
		if (!tty_dequeue_read(tty, &c)) {
			if (read > 0)
				break;
			if (nonblock) {
				/* POSIX: O_NONBLOCK with no data → EAGAIN, not EOF. */
				return -EAGAIN;
			}
			/* VMIN=0, VTIME>0: timed wait — block with a deadline */
			if (vtime_deadline && cur) {
				if (timer_ticks() >= vtime_deadline) {
					break; /* timeout expired, return 0 */
				}
				/* Park atomically wrt the producer: set BLOCKED+channel,
                 * then re-check read_count under tty_lock.  If a producer
                 * already enqueued a byte (and its wake fired before we
                 * marked ourselves BLOCKED), undo the park and loop. */
				uint64_t _f;
				cur->wait_channel = (void *)&tty->read_waiters;
				cur->wakeup_tick = vtime_deadline;
				cur->state = TASK_BLOCKED;
				spin_lock_irqsave(&tty_lock, &_f);
				if (tty->read_count > 0) {
					cur->state = TASK_READY;
					cur->wait_channel = NULL;
					cur->wakeup_tick = 0;
					spin_unlock_irqrestore(&tty_lock, _f);
					continue;
				}
				spin_unlock_irqrestore(&tty_lock, _f);
				sched_schedule();
				if (cur->state == TASK_ZOMBIE ||
				    cur->has_exited || signal_pending(cur)) {
					if (signal_pending(cur)) {
						return read > 0 ? read : -EINTR;
					}
					return read > 0 ? read : -EINTR;
				}
				/* Woke up — could be data arrival or timeout; loop to check */
				continue;
			}
			if (cur) {
				/* Park atomically wrt the producer: set BLOCKED+channel,
                 * then re-check read_count under tty_lock to close the
                 * lost-wakeup race window between tty_dequeue_read (which
                 * dropped tty_lock before returning 0) and our own park.
                 * Without this, a producer that enqueues+wakes here can
                 * find no blocked task and we sleep forever — which is
                 * exactly what kept ncurses-based readers (nano, top)
                 * stuck while non-ncurses readers (less) drained the
                 * buffer in single large reads and rarely re-blocked. */
				uint64_t _f;
				cur->wait_channel = (void *)&tty->read_waiters;
				cur->state = TASK_BLOCKED;
				spin_lock_irqsave(&tty_lock, &_f);
				if (tty->read_count > 0) {
					cur->state = TASK_READY;
					cur->wait_channel = NULL;
					spin_unlock_irqrestore(&tty_lock, _f);
					continue;
				}
				spin_unlock_irqrestore(&tty_lock, _f);
				sched_schedule();
				// Check if we were killed or have a pending signal
				if (cur->state == TASK_ZOMBIE ||
				    cur->has_exited || signal_pending(cur)) {
					// Handle pending signal
					if (signal_pending(cur)) {
						return read > 0 ? read : -EINTR;
					}
					return read > 0 ? read : -EINTR;
				}
				continue;
			}
			break;
		}
		// SMAP-aware write to user buffer
		smap_disable();
		out[read++] = c;
		smap_enable();
		if ((tty->term.c_lflag & ICANON) && c == '\n') {
			break;
		}
	}

	/* O_NONBLOCK with no data → EAGAIN; VMIN=0 with no data → 0 */
	if (read == 0 && nonblock && !vmin_zero) {
		return -EAGAIN;
	}
	return read;
}

long tty_write(tty_t *tty, const void *buf, long count)
{
	if (!tty || !buf || count <= 0) {
		return 0;
	}

	/* PTY slave writes go to a per-pty ring buffer that has its own lock
     * and a wait queue for blocking when full.  Holding the global
     * tty_lock here would (a) be unnecessary because the ring is per-pty
     * and (b) prevent the writer from sleeping when the ring is full
     * (tty_lock is taken with IRQs off).  Use the blocking enqueue path
     * instead so bursts like `ps aux` / `dmesg` / a top-screen redraw
     * never silently lose bytes \u2014 dropped bytes corrupt ANSI sequences
     * and confuse the master-side terminal emulator (e.g. tmux). */
	if (tty->is_pty && !tty->is_master && tty->priv) {
		pty_t *pty = (pty_t *)tty->priv;
		const char *in = (const char *)buf;
		long total = 0;
#define PTY_OUT_CHUNK 512
		char tmpbuf[PTY_OUT_CHUNK];
		char outbuf[PTY_OUT_CHUNK *
			    2]; /* worst case: every byte expands \n -> \r\n */
		int do_opost = (tty->term.c_oflag & OPOST) &&
			       (tty->term.c_oflag & ONLCR);
		while (total < count) {
			long chunk = count - total;
			if (chunk > PTY_OUT_CHUNK)
				chunk = PTY_OUT_CHUNK;
			smap_disable();
			for (long i = 0; i < chunk; i++)
				tmpbuf[i] = in[total + i];
			smap_enable();
			long out_len = 0;
			if (do_opost) {
				for (long i = 0; i < chunk; i++) {
					char c = tmpbuf[i];
					if (c == '\n')
						outbuf[out_len++] = '\r';
					outbuf[out_len++] = c;
				}
			} else {
				for (long i = 0; i < chunk; i++)
					outbuf[out_len++] = tmpbuf[i];
			}
			(void)pty_master_enqueue_bulk(pty, outbuf, out_len);
			/* Always advance by the user-visible chunk size: ring overflow
             * drops bytes silently rather than blocking the writer.  See
             * comment on PTY_MASTER_BUF_SIZE for why blocking is unsafe. */
			total += chunk;
		}
#undef PTY_OUT_CHUNK
		return total;
	}

// Copy user buffer into a small kernel-side staging buffer so we do
// one SMAP window per chunk instead of per character, and hold the
// tty_lock for the entire write so two CPUs can't interleave chars.
#define TTY_WRITE_CHUNK 256
	char tmp[TTY_WRITE_CHUNK];
	char mirror_tmp[TTY_WRITE_CHUNK];
	long written = 0;
	int mirror_console = (tty == tty_get_console());

	// Enter batch mode: suppresses cursor updates on other CPUs
	// and enables rate-limited VRAM flushing (~50fps)
	console_batch_begin();

	while (written < count) {
		long chunk = count - written;
		if (chunk > TTY_WRITE_CHUNK)
			chunk = TTY_WRITE_CHUNK;

		// Bulk copy from user space
		smap_disable();
		for (long i = 0; i < chunk; i++)
			tmp[i] = ((const char *)buf)[written + i];
		smap_enable();

		uint64_t flags;
		long mirror_len = 0;
		spin_lock_irqsave(&tty_lock, &flags);
		for (long i = 0; i < chunk; i++) {
			char c = tmp[i];
			/* Output post-processing (OPOST/ONLCR): translate bare LF
             * to CR+LF so cursor returns to column 0.  Skip when the
             * caller has cleared OPOST (raw mode — tmux, full-screen
             * apps), which set their own \r\n explicitly. */
			if ((tty->term.c_oflag & OPOST) &&
			    (tty->term.c_oflag & ONLCR) && c == '\n') {
				if (mirror_console &&
				    mirror_len < (long)sizeof(mirror_tmp))
					mirror_tmp[mirror_len++] = '\r';
				tty->output(tty, '\r');
			}
			if (mirror_console &&
			    mirror_len < (long)sizeof(mirror_tmp)) {
				mirror_tmp[mirror_len++] = c;
			}
			tty->output(tty, c);
		}
		spin_unlock_irqrestore(&tty_lock, flags);

		if (mirror_console && mirror_len > 0) {
			usbserial_log_write(mirror_tmp, (uint32_t)mirror_len);
		}

		/* If the parser injected a DSR/DA reply into our read buffer
         * during this chunk, wake any blocked readers now that we’ve
         * dropped tty_lock.  Doing the wake under tty_lock is unsafe:
         * sched_enqueue_ready may take scheduler locks and we hold
         * tty_lock with IRQs off. */
		if (mirror_console && vt_consume_reply_pending()) {
			tty_wake_readers(&tty->read_waiters);
		}

		// Rate-limited VRAM flush (~50fps) — skips if too recent
		console_flush();

		written += chunk;
	}

	// End batch mode: unconditional final flush to ensure last frame is visible
	console_batch_end();

	return count;
#undef TTY_WRITE_CHUNK
}

void __attribute__((format(printf, 2, 3))) tty_printf(tty_t *tty,
						      const char *fmt, ...)
{
	char buf[320];
	va_list args;
	__builtin_va_start(args, fmt);
	int len = kvsnprintf(buf, sizeof(buf), fmt, args);
	__builtin_va_end(args);
	if (tty)
		tty_write(tty, buf, len);
	else
		kprintf("%s", buf);
}

int tty_ioctl(tty_t *tty, unsigned long req, void *argp, task_t *cur)
{
	if (!tty) {
		return -ENOTTY;
	}
	switch (req) {
	case TCGETS:
		if (!argp)
			return -EFAULT;
		// Security: Use SMAP-aware copy to user space
		return tty_copy_to_user(argp, &tty->term, sizeof(termios_k_t));
	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		if (!argp)
			return -EFAULT;
		// Security: Use SMAP-aware copy from user space
		{
			/* Only log when termios actually CHANGES, not the
                 * spinning re-arm done by ncurses' input timing loop. */
			int _r = tty_copy_from_user(&tty->term, argp,
						    sizeof(termios_k_t));
			return _r;
		}
	case TIOCGPGRP: {
		if (!argp)
			return -EFAULT;
		int pgid = tty->fg_pgid;
		// Security: Use SMAP-aware copy to user space
		return tty_copy_to_user(argp, &pgid, sizeof(int));
	}
	case TIOCSPGRP: {
		if (!argp)
			return -EFAULT;
		int pgid;
		// Security: Use SMAP-aware copy from user space
		int ret = tty_copy_from_user(&pgid, argp, sizeof(int));
		if (ret != 0)
			return ret;
		tty->fg_pgid = pgid;
		return 0;
	}
	case TIOCSCTTY:
		if (!cur)
			return -EINVAL;
		cur->ctty = tty;
		return 0;
	case TIOCGWINSZ:
		if (!argp)
			return -EFAULT;
		// Security: Use SMAP-aware copy to user space
		return tty_copy_to_user(argp, &tty->winsz,
					sizeof(struct winsize));
	case TIOCSWINSZ: {
		if (!argp)
			return -EFAULT;
		struct winsize old_winsz = tty->winsz;
		int ret = tty_copy_from_user(&tty->winsz, argp,
					     sizeof(struct winsize));
		if (ret != 0)
			return ret;
		if (tty->winsz.ws_row != old_winsz.ws_row ||
		    tty->winsz.ws_col != old_winsz.ws_col) {
			tty_signal_pgrp(tty, SIGWINCH);
		}
		return 0;
	}
	case TIOCSGUARD:
		if (tty == tty_get_console()) {
			console_set_prompt_guard();
			return 0;
		}
		return 0;
	default:
		return -ENOTTY;
	}
}

int tty_pty_allocate(int *out_id)
{
	if (!out_id) {
		return -EINVAL;
	}
	for (int i = 0; i < TTY_MAX_PTYS; ++i) {
		if (g_ptys[i].id == -1) {
			pty_t *pty = &g_ptys[i];
			mm_memset(pty, 0, sizeof(pty_t));
			spinlock_init(&pty->lock, "pty");
			pty->id = i;
			pty->master_open = 1;
			pty->slave_open = 0;
			pty->slave.id = i;
			pty->slave.is_pty = 1;
			pty->slave.is_master = 0;
			pty->slave.output = tty_output_pty_slave;
			pty->slave.priv = pty;
			tty_set_default_termios(&pty->slave);
			pty->slave.winsz.ws_row = 25;
			pty->slave.winsz.ws_col = 80;
			if (out_id) {
				*out_id = i;
			}
			return 0;
		}
	}
	return -ENOSYS;
}

tty_t *tty_get_pty_slave(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return NULL;
	}
	return &pty->slave;
}

int tty_pty_slave_open(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return -EINVAL;
	}
	pty->slave_open = 1;
	return 0;
}

int tty_pty_is_allocated(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return 0;
	}
	return pty->master_open || pty->slave_open;
}

long tty_pty_master_read(int id, void *buf, long count, int nonblock)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty || !buf || count <= 0) {
		return -EINVAL;
	}
	task_t *cur = sched_current();
	char *out = (char *)buf;
	long read = 0;
	while (read < count) {
		uint64_t flags;
		spin_lock_irqsave(&pty->lock, &flags);

		/* Drain whatever is queued under the lock. */
		while (pty->m_count > 0 && read < count) {
			char c = pty->master_buf[pty->m_head];
			pty->m_head = (pty->m_head + 1) % PTY_MASTER_BUF_SIZE;
			pty->m_count--;
			spin_unlock_irqrestore(&pty->lock, flags);
			/* SMAP-aware write to user buffer (must be done with the lock
             * dropped — user-space access can fault). */
			smap_disable();
			out[read++] = c;
			smap_enable();
			spin_lock_irqsave(&pty->lock, &flags);
		}

		if (read > 0) {
			spin_unlock_irqrestore(&pty->lock, flags);
			break;
		}

		/* Slave end is closed and the master ring is empty: EOF. */
		if (!pty->slave_open) {
			spin_unlock_irqrestore(&pty->lock, flags);
			break;
		}
		if (nonblock) {
			spin_unlock_irqrestore(&pty->lock, flags);
			break;
		}
		if (!cur) {
			spin_unlock_irqrestore(&pty->lock, flags);
			break;
		}
		if (signal_pending(cur)) {
			spin_unlock_irqrestore(&pty->lock, flags);
			return -EINTR;
		}
		cur->state = TASK_BLOCKED;
		cur->wait_channel = (void *)&pty->master_read_waiters;
		spin_unlock_irqrestore(&pty->lock, flags);
		sched_schedule();
	}
	return read;
}

long tty_pty_master_write(int id, const void *buf, long count)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty || !buf || count <= 0) {
		return -EINVAL;
	}
	/* Track the active pane: the PTY slave receiving keystrokes is the
     * one currently in focus.  Relaxed ordering is sufficient — we only
     * need an approximately-current pointer for the dump hotkeys. */
	__atomic_store_n(&g_active_tty, &pty->slave, __ATOMIC_RELAXED);
	const char *in = (const char *)buf;
	for (long i = 0; i < count; ++i) {
		// SMAP-aware read from user buffer
		smap_disable();
		char c = in[i];
		smap_enable();
		tty_input_char(&pty->slave, c, 0);
	}
	return count;
}

int tty_pty_master_close(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return -EINVAL;
	}
	pty->master_open = 0;
	if (!pty->slave_open) {
		pty->id = -1;
	}
	return 0;
}

int tty_pty_slave_close(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return -EINVAL;
	}
	pty->slave_open = 0;
	/* Wake any task blocked in tty_pty_master_read so it can observe
     * EOF (read returns 0) and the master fd's poll set transitions to
     * POLLHUP.  Without this, the last shell `exit` leaves tmux's I/O
     * loop blocked indefinitely. */
	tty_wake_readers(&pty->master_read_waiters);
	if (!pty->master_open) {
		pty->id = -1;
	}
	return 0;
}

/* Poll a pty master endpoint. Returns POLLIN when there are bytes
 * queued from the slave, POLLOUT always (writes are always accepted),
 * POLLHUP when the slave end is closed and no data remains. */
int tty_pty_master_poll(int id, int events)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty)
		return 0;
	int rev = 0;
	uint64_t flags;
	spin_lock_irqsave(&pty->lock, &flags);
	uint32_t count = pty->m_count;
	int slave_open = pty->slave_open;
	spin_unlock_irqrestore(&pty->lock, flags);
	if ((events & (POLLIN | POLLRDNORM)) && count > 0)
		rev |= POLLIN | POLLRDNORM;
	if (events & (POLLOUT | POLLWRNORM))
		rev |= POLLOUT | POLLWRNORM;
	if (!slave_open && count == 0)
		rev |= POLLHUP;
	return rev;
}
