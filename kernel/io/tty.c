// LikeOS-64 TTY/PTY subsystem
#include <kernel/ke/waitq.h>
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
#include <kernel/fs/vfs.h> /* vfs_file refcount in the pty diagnostic dump */

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
	/* Channel id (address only, no list) for a master parked in
	 * tty_pty_master_write waiting for room in the slave's input queue. */
	task_t *slave_write_waiters;
	int master_open;
	int slave_open;
	void *slave_vf; // diagnostic: slave's vfs_file (refcount visibility)
	/* Who is polling the MASTER end.  The slave end has its own, in the
	 * tty_t above -- the two directions become ready independently, and a
	 * shared queue would wake a terminal emulator every time the shell it
	 * runs read a keystroke. */
	struct wait_queue_head poll_wq;
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
static void tty_wake_readers(task_t **waiters, struct wait_queue_head *pq)
{
	sched_wake_channel((void *)waiters);
	/* Also wake anything sleeping in poll/select/epoll_wait on THIS
	 * endpoint, so it re-scans immediately instead of waiting for the next
	 * timer tick.  Without it, programs that multiplex stdin and a socket
	 * through poll() see up to one tick of lag per keystroke.
	 *
	 * `pq' names the endpoint because the two ends of a pty become ready
	 * independently: bytes the shell writes make the MASTER readable, and
	 * a keystroke the emulator forwards makes the SLAVE readable.  Waking
	 * both -- or worse, every poller on the machine, which is what a
	 * single shared channel did -- means a terminal emulator is woken
	 * every time its own shell reads a character. */
	extern void poll_notify_wq(struct wait_queue_head *);
	poll_notify_wq(pq);
}

static void tty_enqueue_read(tty_t *tty, char c)
{
	uint64_t flags;
	spin_lock_irqsave(&tty_lock, &flags);

	if (tty->read_count >= sizeof(tty->read_buf)) {
		/* Say WHICH queue overflowed and what was thrown away.
		 *
		 * "input dropped" on its own cannot be acted on: the console
		 * and every pty share this path, and a full console queue
		 * that nobody is reading is ordinary, while a full pty queue
		 * means a program's input is being corrupted.  Those want
		 * opposite reactions and the message could not tell them
		 * apart.  The byte is printed because a dropped escape
		 * sequence is what the damage looks like from userspace --
		 * losing 0x1b or '[' out of the middle of one turns a paste
		 * into an unbound-key error. */
		WARN_RATELIMIT(
			1,
			"tty input dropped on %s%d: queue full (%u bytes), byte 0x%02x",
			tty->is_pty ? "pts/" : "console",
			tty->is_pty && tty->priv ? ((pty_t *)tty->priv)->id : 0,
			tty->read_count, (unsigned char)c);
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

/* Is there room in the input queue for one more character?
 *
 * Not always one byte's worth: in canonical mode the line sits in canon_buf
 * until a newline commits the whole of it at once, so the character that ends
 * a line needs room for the line.  canon_buf is a quarter of read_buf, so the
 * requirement is always satisfiable once the queue drains.
 *
 * Callers that park on the answer must re-check it under tty_lock, hence the
 * locked form.
 */
static int tty_input_has_room_locked(const tty_t *tty)
{
	uint32_t space = (uint32_t)sizeof(tty->read_buf) - tty->read_count;
	uint32_t need = (tty->term.c_lflag & ICANON) ?
				(uint32_t)tty->canon_len + 1 :
				1;

	lockdep_assert_held(&tty_lock);
	return space >= need;
}

static int tty_input_has_room(const tty_t *tty)
{
	uint64_t flags;
	int room;

	spin_lock_irqsave(&tty_lock, &flags);
	room = tty_input_has_room_locked(tty);
	spin_unlock_irqrestore(&tty_lock, flags);
	return room;
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
		tty_wake_readers(&pty->master_read_waiters, &pty->poll_wq);
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
	wq_head_init_once(&g_console_tty.poll_wq, "console-poll");
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

static void tty_signal_pgrp(tty_t *tty, int sig);

// While a client holds an exclusive keyboard grab (/dev/input/event0), no
// cooked input may reach the console tty — the grabber owns the console.
// PTYs are unaffected.  (Declared here: tty sits below the input drivers.)
extern int evdev_kbd_grabbed(void);

// Runtime resolution change: re-read the console dimensions, resize the VT
// emulator and notify the foreground process group (SIGWINCH).
void tty_console_resize(void)
{
	uint32_t rows = 0, cols = 0;

	console_get_dimensions(&rows, &cols);
	if (WARN_ON_ONCE(rows == 0 || cols == 0))
		return;
	if (g_console_tty.winsz.ws_row == rows &&
	    g_console_tty.winsz.ws_col == cols)
		return;
	g_console_tty.winsz.ws_row = (unsigned short)rows;
	g_console_tty.winsz.ws_col = (unsigned short)cols;
	vt_resize(&g_console_vt, (int)cols, (int)rows);
	if (g_console_tty.fg_pgid > 0)
		tty_signal_pgrp(&g_console_tty, SIGWINCH);
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
	tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
}

void tty_input_char_raw(tty_t *tty, char c)
{
	if (!tty)
		return;
	// See tty_input_char: grabbed keyboards bypass the console tty.
	if (!tty->is_pty && evdev_kbd_grabbed())
		return;
	tty_enqueue_read(tty, c);
	tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
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
		tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
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
	tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
}

/* Feed one character through the line discipline.  Returns non-zero when the
 * character made input available to a reader -- i.e. when a wake is owed.
 *
 * Split out so a bulk producer can deliver a whole write and wake ONCE at the
 * end of it.  Waking per character does not merely cost wakeups, it chops the
 * write up on the way out: a reader parked on an empty queue is woken by the
 * first byte, runs, takes that byte, finds nothing behind it and returns --
 * while the rest of the same write is still being pushed in behind it.
 *
 * Measured, not surmised.  A terminal on the far end of an ssh session sends
 * a bracketed paste as one 16-byte write; dumping the raw reads on the slave
 * side showed that write arriving as sixteen one-byte reads, and the
 * terminal's own cursor-position report fragmenting the same way, with the
 * occasional whole-burst read on the runs where the reader had not yet parked.
 *
 * That is fatal to anything parsing escape sequences.  A sequence is only
 * recognisable while its bytes are together: an editor that reads a lone ESC
 * with nothing behind it has to conclude the user pressed Escape, because
 * that is what a lone ESC means.  The bytes that follow are then taken as
 * individual keys -- the paste's framing markers turn into unbound-key errors
 * and the text they were supposed to frame is mangled.  Nothing the reader
 * can do fixes this; one write has to arrive as one read.
 */
static int tty_input_char_core(tty_t *tty, char c, int ctrl)
{
	BUG_ON(tty == NULL);
	if (!tty || c == 0) {
		return 0;
	}
	// Exclusive keyboard grab: hardware input bypasses the console tty
	// entirely (the grabbing display server reads /dev/input/event0).
	if (!tty->is_pty && evdev_kbd_grabbed()) {
		return 0;
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
			return 0;
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
			return 0;
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
			return 0;
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
			return 0;
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
			return 0;
		}
		if (c == tty->term.c_cc[VEOF]) {
			if (tty->canon_len == 0) {
				tty->eof_pending = 1;
				return 1;
			}
			for (uint16_t i = 0; i < tty->canon_len; ++i) {
				tty_enqueue_read(tty, tty->canon_buf[i]);
			}
			tty->canon_len = 0;
			return 1;
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
			return 1;
		}
		return 0;
	}

	tty_enqueue_read(tty, c);
	if (tty->term.c_lflag & ECHO) {
		if (c == '\n' && (tty->term.c_oflag & OPOST) &&
		    (tty->term.c_oflag & ONLCR)) {
			tty->output(tty, '\r');
		}
		tty->output(tty, c);
	}
	return 1;
}

/* One character, delivered and announced.  For single-character producers
 * (the keyboard interrupt); bulk producers use the core and wake once. */
void tty_input_char(tty_t *tty, char c, int ctrl)
{
	if (tty_input_char_core(tty, c, ctrl))
		tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
}

/* Background process group access check (POSIX job control).  When a
 * process whose group is not the terminal's foreground group reads from
 * the terminal (or writes with TOSTOP set), its whole group gets SIGTTIN/
 * SIGTTOU - default action: stop - and the call fails with EINTR so it is
 * retried after SIGCONT.  A process that blocks or ignores the signal
 * gets EIO for reads and proceeds for writes, per POSIX.  Returns 0 when
 * access is allowed. */
static long tty_bg_pgrp_check(tty_t *tty, int sig)
{
	task_t *cur = sched_current();
	/* POSIX scopes this to the CONTROLLING terminal: a process holding an
	 * fd to some other tty (a pty master, another session's console) must
	 * never be stopped for using it. */
	if (!cur || cur->ctty != tty || tty->fg_pgid <= 0 || cur->pgid <= 0 ||
	    cur->pgid == tty->fg_pgid)
		return 0;
	struct k_sigaction *act = &cur->signals.action[sig];
	if (act->sa_handler == SIG_IGN ||
	    sigismember_k(&cur->signals.blocked, sig))
		return (sig == SIGTTIN) ? -EIO : 0;
	sched_signal_pgrp(cur->pgid, sig);
	return -EINTR;
}

static long tty_read_inner(tty_t *tty, void *buf, long count, int nonblock);

/* Read from the terminal, then release whoever is waiting for the room.
 *
 * A master parked in tty_pty_master_write is waiting on exactly this: the
 * queue it filled being drained.  Woken once per read rather than per byte in
 * tty_dequeue_read -- waking walks the task list, and a byte at a time would
 * make every keystroke pay for it. */
long tty_read(tty_t *tty, void *buf, long count, int nonblock)
{
	long ret = tty_read_inner(tty, buf, count, nonblock);

	if (ret > 0 && tty && tty->is_pty && !tty->is_master && tty->priv)
		tty_wake_readers(&((pty_t *)tty->priv)->slave_write_waiters,
				 &((pty_t *)tty->priv)->poll_wq);
	return ret;
}

static long tty_read_inner(tty_t *tty, void *buf, long count, int nonblock)
{
	if (!tty || !buf || count <= 0) {
		return 0;
	}

	long bg = tty_bg_pgrp_check(tty, SIGTTIN);
	if (bg < 0)
		return bg;

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
		/* PTY slave hangup: once the master is closed the line is
		 * dead — return EOF after draining, so even readers that
		 * ignore SIGHUP cannot block forever. */
		if (tty->is_pty && !tty->is_master && tty->priv &&
		    !((pty_t *)tty->priv)->master_open &&
		    tty->read_count == 0) {
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
				/* Disarm the VTIME deadline.  It belongs to this
				 * one park: the loop below re-arms it if it
				 * takes this branch again, and the branch after
				 * it -- the read with no VTIME -- sets no
				 * deadline at all and would inherit this one.
				 * Left behind, a deadline already in the past
				 * is claimed by sched_wake_expired_sleepers()
				 * on the next tick of every later wait this
				 * task makes, wherever it makes it. */
				cur->wakeup_tick = 0;
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

	/* Background writes stop the writer only when TOSTOP is set. */
	if (tty->term.c_lflag & TOSTOP) {
		long bg = tty_bg_pgrp_check(tty, SIGTTOU);
		if (bg < 0)
			return bg;
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
			tty_wake_readers(&tty->read_waiters, &tty->poll_wq);
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
		/* Only a process whose controlling terminal is this tty (or the
		 * privileged caller) may set the foreground process group. */
		if (cur && !capable() && cur->ctty != tty)
			return -EPERM;
		if (pgid < 0)
			return -EINVAL;

		/* And the group must be one of THIS SESSION's.
		 *
		 * POSIX requires the refusal and it is not a formality.  A
		 * debugger attaching to a process on another terminal does
		 * exactly this: gdb hands its own terminal's foreground group
		 * to the group of the process it attached to, which lives in
		 * the other terminal's session.  Allowed, it puts gdb itself in
		 * the background of the terminal it is talking to -- Ctrl-C
		 * from then on goes to the unrelated program on the OTHER
		 * terminal, and gdb stops with SIGTTIN the moment it reads its
		 * own prompt.  Both terminals are wedged by one ioctl.
		 *
		 * Refused only when the group is positively known to belong
		 * somewhere else.  A group with no live member left, or a
		 * terminal with no session recorded, is allowed through as
		 * before: this exists to catch a caller naming another
		 * session, not to make job control depend on bookkeeping being
		 * complete.  Not exempted for root either -- the rule is about
		 * which terminal a group can be the foreground of, and being
		 * root does not put a process on a different terminal. */
		if (pgid > 0 && tty->sid > 0 &&
		    sched_pgrp_in_session(pgid, tty->sid) < 0)
			return -EPERM;

		tty->fg_pgid = pgid;
		return 0;
	}
	case TIOCSCTTY:
		if (!cur)
			return -EINVAL;
		/* Only a session leader may acquire a controlling terminal; the
		 * privileged caller is exempt. */
		if (!capable() && cur->sid != (int)cur->id)
			return -EPERM;
		cur->ctty = tty;
		tty->sid = cur->sid;
		/* Claiming the terminal also makes the claimant's process group
		 * its FOREGROUND group.  Without this the tty kept whatever
		 * group happened to open it first — in a pty that is the
		 * process which called openpty(), not the session that goes on
		 * to run in it.  A shell starting in a fresh pane then saw
		 * tcgetpgrp() != getpgrp(), decided it was a background job,
		 * and tried to stop itself with SIGTTIN until it gave up and
		 * turned job control off ("no job control in background"). */
		if (cur->pgid > 0)
			tty->fg_pgid = cur->pgid;
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
			/* Both poll queues survive the slot being reused --
			 * see wq_head_init_once(). */
			struct wait_queue_head saved_mwq = pty->poll_wq;
			struct wait_queue_head saved_swq = pty->slave.poll_wq;

			mm_memset(pty, 0, sizeof(pty_t));
			pty->poll_wq = saved_mwq;
			pty->slave.poll_wq = saved_swq;
			spinlock_init(&pty->lock, "pty");
			/* Both directions get their own queue: the master's
			 * here, the slave's on the tty below. */
			wq_head_init_once(&pty->poll_wq, "pty-master-poll");
			wq_head_init_once(&pty->slave.poll_wq,
					  "pty-slave-poll");
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

/* slave_open is an OPEN COUNT, not a flag.  Every open() of /dev/pts/N takes a
 * reference and every close() drops one; the line is only torn down when the
 * last one goes.  As a flat flag, two independent opens of the same slave meant
 * the first close hung up the line for both — the master saw EOF and POLLHUP
 * while another descriptor was still perfectly usable.  Boolean tests elsewhere
 * ("is the slave open?") read a count just as well. */
int tty_pty_slave_open(int id)
{
	pty_t *pty = tty_get_pty(id);
	uint64_t f;

	if (!pty) {
		return -EINVAL;
	}
	spin_lock_irqsave(&pty->lock, &f);
	pty->slave_open++;
	spin_unlock_irqrestore(&pty->lock, f);
	return 0;
}

/* Diagnostic: remember the vfs_file backing the slave so the pty dump can
 * show its live refcount (who is keeping the line open). */
void tty_pty_slave_set_vf(int id, void *vf)
{
	pty_t *pty = tty_get_pty(id);
	if (pty)
		pty->slave_vf = vf;
}

/* Diagnostic dump for the Ctrl+N hotkey: per-pty open/buffer/refcount state.
 * The tmux window-close chain depends on slave refcount → 0 → slave_open=0 →
 * master POLLHUP; this dump shows exactly which link is stuck. */
void tty_dump_ptys(tty_t *out)
{
	tty_printf(out, "=== PTY table ===\n");
	for (int i = 0; i < TTY_MAX_PTYS; i++) {
		pty_t *p = &g_ptys[i];
		if (p->id == -1)
			continue;
		int refs = -1;
		if (p->slave_vf)
			refs = ((vfs_file_t *)p->slave_vf)->refcount;
		tty_printf(
			out,
			"pty%d master_open=%d slave_open=%d m_count=%u fg_pgid=%d slave_refs=%d\n",
			p->id, p->master_open, p->slave_open,
			(unsigned)p->m_count, p->slave.fg_pgid, refs);
	}
	tty_printf(out, "=================\n");
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

		/* Slave end is closed and the master ring is empty: EOF.
		 *
		 * This is the ONLY condition that may return zero.  Zero from
		 * read() means end of file, and for a pty master that means
		 * the line is down -- every reader treats it as "the child is
		 * gone" and tears the terminal down. */
		if (!pty->slave_open) {
			spin_unlock_irqrestore(&pty->lock, flags);
			break;
		}
		if (nonblock) {
			spin_unlock_irqrestore(&pty->lock, flags);
			/* POSIX: O_NONBLOCK with no data yet -> EAGAIN, not
			 * EOF -- the same rule tty_read() above follows for
			 * the slave side, and tty_pty_master_write() for
			 * writes.  This path returned 0 instead, and 0 is a
			 * lie: the slave is open and the child may simply not
			 * have written anything yet.
			 *
			 * VTE opens the master O_NONBLOCK and reads it from a
			 * timer, so its very first read happened before the
			 * freshly exec'd shell had printed a prompt.  It read
			 * the zero as end-of-file, released the pty, and the
			 * close hung the line up -- SIGHUP to the shell it had
			 * just started.  The terminal window never appeared,
			 * and nothing anywhere reported an error, because as
			 * far as every layer was concerned the child had
			 * simply finished. */
			return -EAGAIN;
		}
		if (!cur) {
			spin_unlock_irqrestore(&pty->lock, flags);
			/* No task to park: this cannot wait either, and
			 * reporting EOF would be the same lie. */
			return -EAGAIN;
		}
		if (signal_pending(cur)) {
			spin_unlock_irqrestore(&pty->lock, flags);
			return -EINTR;
		}
		cur->wait_channel = (void *)&pty->master_read_waiters;
		cur->state = TASK_BLOCKED;
		spin_unlock_irqrestore(&pty->lock, flags);
		sched_schedule();
	}
	return read;
}

/* Feed bytes from the master side into the slave's input queue.
 *
 * Waits for room rather than discarding.  tty_enqueue_read drops on a full
 * queue, so anything arriving faster than the slave reads it was thrown away
 * while write() reported the whole count as delivered -- and the loss landed
 * wherever the queue happened to fill, which for a paste means in the middle
 * of its framing markers.  An editor that sees the tail of one without its
 * head reports the remains as an unbound key and mangles the text.
 *
 * That is not a rare condition, it is the normal one for a paste of any size.
 * The queue holds 1024 bytes; a terminal sends a paste as fast as the link
 * carries it, and a program that inserts and redraws as it reads is slower
 * than that by a wide margin.  Dumping the raw reads showed over 512 bytes
 * standing in the queue at once for a twenty-line paste, against a reader
 * doing nothing but printing -- an editor does far more per byte.
 *
 * Blocking is the conventional behaviour for a master whose slave is not
 * keeping up, and it is what makes the write honest: the count returned is
 * the count delivered.  Partial progress returns short rather than sleeping,
 * so a caller with other work to do gets control back.
 */
long tty_pty_master_write(int id, const void *buf, long count, int nonblock)
{
	pty_t *pty = tty_get_pty(id);
	task_t *cur = sched_current();

	if (!pty || !buf || count <= 0) {
		return -EINVAL;
	}
	/* Track the active pane: the PTY slave receiving keystrokes is the
     * one currently in focus.  Relaxed ordering is sufficient — we only
     * need an approximately-current pointer for the dump hotkeys. */
	__atomic_store_n(&g_active_tty, &pty->slave, __ATOMIC_RELAXED);
	const char *in = (const char *)buf;
	int wake = 0;
	long i = 0;

	/* Deliver the whole write, THEN wake once.
	 *
	 * One write is one burst of input and has to arrive as one read.  See
	 * tty_input_char_core for what waking per character did to it, and for
	 * the measurements. */
	while (i < count) {
		char c;

		if (!tty_input_has_room(&pty->slave)) {
			uint64_t f;

			/* Hand back what landed and let the caller retry the
			 * rest, rather than sleeping on it. */
			if (i > 0)
				break;
			if (nonblock)
				return -EAGAIN;
			/* Nobody left to drain it: the line is down. */
			if (!pty->slave_open)
				return -EIO;
			if (cur && signal_pending(cur))
				return -EINTR;
			if (!cur)
				break; /* no task context: cannot wait */

			/* Park against the reader.  Un-parking is a CAS, not a
			 * store: a reader that claimed us in the window has
			 * already moved us to READY and is enqueueing us, and
			 * writing RUNNING over that claim makes its enqueue
			 * refuse the task.  On a lost CAS, fall through and
			 * schedule as a READY current -- the queue entry brings
			 * us straight back. */
			cur->wait_channel = (void *)&pty->slave_write_waiters;
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			cur->state = TASK_BLOCKED;
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			spin_lock_irqsave(&tty_lock, &f);
			if (tty_input_has_room_locked(&pty->slave)) {
				task_state_t expected = TASK_BLOCKED;

				if (__atomic_compare_exchange_n(
					    &cur->state, &expected, TASK_RUNNING,
					    false, __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE)) {
					cur->wait_channel = NULL;
					spin_unlock_irqrestore(&tty_lock, f);
					continue;
				}
			}
			spin_unlock_irqrestore(&tty_lock, f);
			sched_schedule();
			cur->wait_channel = NULL;
			if (signal_pending(cur))
				return i > 0 ? i : -EINTR;
			continue;
		}

		// SMAP-aware read from user buffer
		smap_disable();
		c = in[i];
		smap_enable();
		wake |= tty_input_char_core(&pty->slave, c, 0);
		i++;
	}
	if (wake)
		tty_wake_readers(&pty->slave.read_waiters, &pty->slave.poll_wq);
	return i;
}

int tty_pty_master_close(int id)
{
	pty_t *pty = tty_get_pty(id);
	if (!pty) {
		return -EINVAL;
	}
	pty->master_open = 0;
	if (pty->slave_open) {
		/* POSIX hangup: closing the master takes the line down.  The
		 * slave side's foreground process group gets SIGHUP (default
		 * action: terminate) and blocked readers are woken so they
		 * observe the hangup instead of sleeping forever.  Without
		 * this, shells in tmux panes survived `tmux kill-server` as
		 * orphans pinned to a dead pts. */
		if (pty->slave.fg_pgid > 0)
			sched_signal_pgrp(pty->slave.fg_pgid, SIGHUP);
		tty_wake_readers(&pty->slave.read_waiters, &pty->slave.poll_wq);
	}
	if (!pty->slave_open) {
		pty->id = -1;
	}
	return 0;
}

int tty_pty_slave_close(int id)
{
	pty_t *pty = tty_get_pty(id);
	uint64_t f;
	int last;

	if (!pty) {
		return -EINVAL;
	}
	spin_lock_irqsave(&pty->lock, &f);
	if (pty->slave_open > 0)
		pty->slave_open--;
	last = (pty->slave_open == 0);
	spin_unlock_irqrestore(&pty->lock, f);

	/* Other descriptors still hold the slave open: the line stays up and
	 * the master must not see a hangup. */
	if (!last)
		return 0;

	pty->slave_vf = NULL;
	/* Wake any task blocked in tty_pty_master_read so it can observe
     * EOF (read returns 0) and the master fd's poll set transitions to
     * POLLHUP.  Without this, the last shell `exit` leaves tmux's I/O
     * loop blocked indefinitely. */
	tty_wake_readers(&pty->master_read_waiters, &pty->poll_wq);
	/* And any task waiting for room in the input queue: with the slave
	 * gone nothing will ever drain it, so the wait must end in EIO
	 * rather than never. */
	tty_wake_readers(&pty->slave_write_waiters, &pty->poll_wq);
	if (!pty->master_open) {
		pty->id = -1;
	}
	return 0;
}

struct wait_queue_head *tty_pty_master_pollq(int id)
{
	pty_t *pty = tty_get_pty(id);

	return pty ? &pty->poll_wq : NULL;
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
	/* Writable only while the slave's input queue has room.  Reporting
	 * it unconditionally sends a caller that multiplexes with select()
	 * straight into a write that can only return EAGAIN, and round the
	 * loop again -- a spin, not a wait. */
	if ((events & (POLLOUT | POLLWRNORM)) &&
	    (!slave_open || tty_input_has_room(&pty->slave)))
		rev |= POLLOUT | POLLWRNORM;
	if (!slave_open && count == 0)
		rev |= POLLHUP;
	return rev;
}
