// LikeOS HAL - 16550 UART Serial Console (COM1)
// Polled 16550 driver: the console log mirror (QEMU -serial, VMware serial port)
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/hal/serial.h>
#include <kernel/ke/interrupt.h> // for inb/outb
#include <kernel/ke/sched.h>
#include <kernel/ke/smp.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/waitq.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define COM1_PORT 0x3F8

// UART registers (offsets from base)
#define UART_DATA 0 // THR (write) / RBR (read)
#define UART_IER 1
#define UART_IIR_FCR 2 // IIR (read) / FCR (write)
#define UART_LCR 3
#define UART_MCR 4
#define UART_LSR 5
#define UART_MSR 6
#define UART_SCR 7

// LSR bits
#define LSR_DATA_READY 0x01
#define LSR_THR_EMPTY 0x20

// Spinlock for serial port access
static spinlock_t serial_lock = SPINLOCK_INIT("serial");

static int g_serial_available = 0;
static int g_serial_initialized = 0;

static inline void uart_out(unsigned short off, unsigned char v)
{
	outb(COM1_PORT + off, v);
}

static inline unsigned char uart_in(unsigned short off)
{
	return inb(COM1_PORT + off);
}

static int uart_detect(void)
{
	/* Try SCR (scratch) register read-back test */
	unsigned char orig = uart_in(UART_SCR);
	uart_out(UART_SCR, 0x55);
	unsigned char t1 = uart_in(UART_SCR);
	uart_out(UART_SCR, 0xAA);
	unsigned char t2 = uart_in(UART_SCR);
	uart_out(UART_SCR, orig); /* Restore */
	return (t1 == 0x55 && t2 == 0xAA);
}

void serial_init(void)
{
	if (g_serial_initialized) {
		return;
	}

	// Disable interrupts
	uart_out(UART_IER, 0x00);

	// Enable DLAB to set divisor (baud rate). Divisor 1 -> 115200 baud
	uart_out(UART_LCR, 0x80);
	uart_out(UART_DATA, 0x01); // DLL
	uart_out(UART_IER, 0x00); // DLM

	// 8 bits, no parity, one stop bit
	uart_out(UART_LCR, 0x03);

	// Enable FIFO, clear them, 14-byte threshold
	uart_out(UART_IIR_FCR, 0xC7);

	// Modem control: RTS/DTR set, OUT2 set (enables IRQ line; harmless here)
	uart_out(UART_MCR, 0x0B);

	g_serial_available = uart_detect();
	g_serial_initialized = 1;
}

int serial_is_available(void)
{
	return g_serial_available;
}

/* ==========================================================================
 * Output: a ring the console writes into, drained as the UART takes bytes.
 *
 * The console mirrors everything it prints to this port, and it does so
 * with its own lock held and interrupts off.  It must therefore never WAIT
 * for the UART: a virtual UART is serviced by the hypervisor, and while the
 * host is busy (a guest writing to its virtual disk at full speed is
 * enough) every status-register read is an exit that can take a long time
 * -- the old per-character "wait until the transmitter is empty" loop then
 * ran for seconds per character, with the serial lock held and interrupts
 * off, and every other CPU that printed spun behind it: the whole guest
 * stopped, the mouse included.
 *
 * So writers only queue.  Once the scheduler runs, one kernel thread,
 * kserial, does all the port I/O -- with interrupts enabled and no lock
 * held, so however long an exit takes, it delays only the serial log:
 * while the line status says the transmitter is empty it hands the
 * 16-byte FIFO a burst, otherwise it sleeps a tick.  Until the thread
 * exists (boot), the writers and the boot CPU's timer tick (serial_poll)
 * drain whatever the UART takes at that moment, never waiting for it.  A ring that fills up (the
 * UART is that slow, or the console that busy) drops new bytes and says
 * so with a marker once there is room again.  After a panic, when the
 * other CPUs are halted and nothing will drain later, output goes out
 * synchronously again (time-bounded per byte).
 * ========================================================================== */

#define SERIAL_RING_SIZE (64u * 1024u) /* power of two */
#define SERIAL_FIFO_BURST 16 /* 16550 transmit FIFO */
#define SERIAL_POLL_BURSTS 4 /* per timer tick: 64 bytes, > 115200 baud */

static char g_ring[SERIAL_RING_SIZE];
/* Set once kserial runs: from then on only it touches the UART. */
static volatile int g_serial_async;
/* Set when the system goes down (oops, panic, reboot, power-off): output
 * is written out at once again, the ring first -- nothing will drain it
 * later. */
static volatile int g_serial_sync;
static uint32_t g_ring_head; /* next byte to send */
static uint32_t g_ring_tail; /* next free slot */
static uint32_t g_ring_dropped; /* bytes lost to a full ring */

static inline uint32_t ring_used(void)
{
	return g_ring_tail - g_ring_head;
}

static inline void ring_put_locked(char c)
{
	if (ring_used() >= SERIAL_RING_SIZE) {
		g_ring_dropped++;
		return;
	}
	g_ring[g_ring_tail & (SERIAL_RING_SIZE - 1)] = c;
	g_ring_tail++;
}

/* Note lost output in the stream itself, once there is room for it. */
static void ring_note_drops_locked(void)
{
	char msg[48];
	int n = 0;
	uint32_t d = g_ring_dropped, div = 1000000000u;
	const char *pre = "\r\n[serial: ", *post = " bytes dropped]\r\n";

	if (!d || ring_used() + sizeof(msg) > SERIAL_RING_SIZE)
		return;
	for (const char *q = pre; *q; q++)
		msg[n++] = *q;
	while (div > 1 && d / div == 0)
		div /= 10;
	for (; div; div /= 10)
		msg[n++] = (char)('0' + (d / div) % 10);
	for (const char *q = post; *q && n < (int)sizeof(msg); q++)
		msg[n++] = *q;
	g_ring_dropped = 0;
	for (int k = 0; k < n; k++)
		ring_put_locked(msg[k]);
}

/* Send what the UART can take right now: per burst one line-status read,
 * then up to a FIFO's worth.  Never waits.  Returns bytes sent. */
static uint32_t ring_drain_locked(int bursts)
{
	uint32_t sent = 0;

	while (bursts-- > 0 && ring_used()) {
		if ((uart_in(UART_LSR) & LSR_THR_EMPTY) == 0)
			break;
		for (int k = 0; k < SERIAL_FIFO_BURST && ring_used(); k++) {
			uart_out(UART_DATA,
				 (unsigned char)g_ring[g_ring_head &
						       (SERIAL_RING_SIZE - 1)]);
			g_ring_head++;
			sent++;
		}
	}
	if (g_ring_dropped)
		ring_note_drops_locked();
	return sent;
}

/* Panic path: a bounded wait for the transmitter (the loop counts reads;
 * after a panic nothing else competes for the time). */
static inline void uart_wait_thr_empty(void)
{
	volatile int timeout = 100000;
	while ((uart_in(UART_LSR) & LSR_THR_EMPTY) == 0) {
		if (--timeout <= 0)
			return;
	}
}

static void serial_sync_flush_locked(void)
{
	while (ring_used()) {
		uart_wait_thr_empty();
		uart_out(UART_DATA,
			 (unsigned char)g_ring[g_ring_head &
					       (SERIAL_RING_SIZE - 1)]);
		g_ring_head++;
	}
}

static inline void serial_put_locked(char c, int sync)
{
	if (sync) {
		serial_sync_flush_locked();
		if (c == '\n') {
			uart_wait_thr_empty();
			uart_out(UART_DATA, '\r');
		}
		uart_wait_thr_empty();
		uart_out(UART_DATA, (unsigned char)c);
		return;
	}
	if (c == '\n')
		ring_put_locked('\r');
	ring_put_locked(c);
}

void serial_write_char(char c)
{
	uint64_t flags;
	int sync = g_serial_sync || smp_others_halted();

	spin_lock_irqsave(&serial_lock, &flags);
	if (!g_serial_initialized)
		serial_init();
	if (!g_serial_available) {
		spin_unlock_irqrestore(&serial_lock, flags);
		return;
	}
	serial_put_locked(c, sync);
	if (!sync && !g_serial_async)
		(void)ring_drain_locked(1);
	spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_write(const char *s, uint32_t len)
{
	BUG_ON(s == NULL && len > 0);
	uint64_t flags;
	int sync = g_serial_sync || smp_others_halted();

	spin_lock_irqsave(&serial_lock, &flags);
	if (!g_serial_initialized)
		serial_init();
	if (!g_serial_available || !s) {
		spin_unlock_irqrestore(&serial_lock, flags);
		return;
	}
	for (uint32_t i = 0; i < len; ++i)
		serial_put_locked(s[i], sync);
	if (!sync && !g_serial_async)
		(void)ring_drain_locked(SERIAL_POLL_BURSTS);
	spin_unlock_irqrestore(&serial_lock, flags);
}

void serial_poll(void)
{
	uint64_t flags;

	if (!g_serial_available || g_serial_async || !ring_used())
		return;
	/* From the timer tick: never wait for the lock either -- a writer
	 * holding it drains on its own way out. */
	flags = local_irq_save();
	if (!spin_trylock(&serial_lock)) {
		local_irq_restore(flags);
		return;
	}
	(void)ring_drain_locked(SERIAL_POLL_BURSTS);
	spin_unlock_irqrestore(&serial_lock, flags);
}

/* ==========================================================================
 * kserial: the only user of the UART once the scheduler runs
 * ========================================================================== */

#define KSERIAL_STACK_SIZE (16u * 1024u)

static uint8_t g_kserial_stack[KSERIAL_STACK_SIZE] __attribute__((aligned(16)));
static struct wait_queue_head g_kserial_wq;

/* Sleep `ns' nanoseconds (an hrtimer wakes the thread), or about one timer
 * tick when `ns' is 0.  Nobody else wakes this thread: it polls. */
static void kserial_nap(uint64_t ns)
{
	task_t *cur = sched_current();
	struct wait_queue_entry we;
	uint64_t fl, until = ns ? hrtimer_now_ns() + ns : 0;

	wq_entry_init(&we, cur);
	spin_lock_irqsave(&serial_lock, &fl);
	wq_add(&g_kserial_wq, &we);
	cur->wait_channel = &g_kserial_wq;
	cur->wakeup_tick = timer_ticks() + 1;
	task_set_blocked(cur);
	spin_unlock_irqrestore(&serial_lock, fl);
	if (until)
		hrtimer_wait_arm(cur, until);
	sched_schedule();
	if (until)
		hrtimer_wait_disarm(cur);
	cur->wakeup_tick = 0;
	cur->wait_channel = NULL;
	wq_remove(&g_kserial_wq, &we);
}

static void kserial_main(void *arg)
{
	(void)arg;
	for (;;) {
		char burst[SERIAL_FIFO_BURST];
		int n = 0;
		uint64_t fl;

		/* The port, unlocked and with interrupts on: a slow exit
		 * here costs this thread time, nobody else's. */
		if (!ring_used()) {
			kserial_nap(0);
			continue;
		}
		if ((uart_in(UART_LSR) & LSR_THR_EMPTY) == 0) {
			/* The UART is sending at line rate: a FIFO's worth
			 * at 115200 baud takes about 1.4 ms -- look again
			 * then, not a whole tick later. */
			kserial_nap(1500000ULL);
			continue;
		}
		spin_lock_irqsave(&serial_lock, &fl);
		while (n < SERIAL_FIFO_BURST && ring_used()) {
			burst[n++] = g_ring[g_ring_head & (SERIAL_RING_SIZE - 1)];
			g_ring_head++;
		}
		if (g_ring_dropped)
			ring_note_drops_locked();
		spin_unlock_irqrestore(&serial_lock, fl);
		for (int k = 0; k < n; k++)
			uart_out(UART_DATA, (unsigned char)burst[k]);
	}
}

void serial_start_thread(void)
{
	task_t *t;
	const char *nm = "kserial";
	unsigned i = 0;

	if (!g_serial_available || g_serial_async || !sched_current())
		return;
	wq_head_init(&g_kserial_wq, "kserial");
	/* Writers stop draining before the thread can run: from here on
	 * only kserial touches the port (no interleaved bursts). */
	g_serial_async = 1;
	t = sched_add_task(kserial_main, NULL, g_kserial_stack,
			   sizeof(g_kserial_stack));
	if (!t) {
		g_serial_async = 0;
		kprintf("serial: cannot start kserial; the console drains the port itself\n");
		return;
	}
	for (; nm[i] && i < sizeof(t->comm) - 1; i++)
		t->comm[i] = nm[i];
	t->comm[i] = '\0';
}

void serial_set_sync(void)
{
	g_serial_sync = 1;
}
