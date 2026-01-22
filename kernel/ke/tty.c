// LikeOS-64 TTY/PTY subsystem
#include "../../include/kernel/tty.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/signal.h"

#define TTY_MAX_PTYS 16

typedef struct pty {
    int id;
    tty_t slave;
    char master_buf[1024];
    uint16_t m_head;
    uint16_t m_tail;
    uint16_t m_count;
    task_t* master_read_waiters;
    int master_open;
    int slave_open;
} pty_t;

static tty_t g_console_tty;
static pty_t g_ptys[TTY_MAX_PTYS];

static void tty_wake_readers(task_t** waiters) {
    task_t* t = *waiters;
    while (t) {
        task_t* next = t->wait_next;
        t->wait_next = NULL;
        t->wait_channel = NULL;
        if (t->state == TASK_BLOCKED) {
            t->state = TASK_READY;
        }
        t = next;
    }
    *waiters = NULL;
}

static void tty_enqueue_read(tty_t* tty, char c) {
    if (tty->read_count >= sizeof(tty->read_buf)) {
        return;
    }
    tty->read_buf[tty->read_tail] = c;
    tty->read_tail = (tty->read_tail + 1) % sizeof(tty->read_buf);
    tty->read_count++;
}

static int tty_dequeue_read(tty_t* tty, char* out) {
    if (tty->read_count == 0) {
        return 0;
    }
    *out = tty->read_buf[tty->read_head];
    tty->read_head = (tty->read_head + 1) % sizeof(tty->read_buf);
    tty->read_count--;
    return 1;
}

static void tty_output_console(tty_t* tty, char c) {
    (void)tty;
    console_putchar(c);
}

static pty_t* tty_get_pty(int id) {
    if (id < 0 || id >= TTY_MAX_PTYS) {
        return NULL;
    }
    if (g_ptys[id].id != id) {
        return NULL;
    }
    return &g_ptys[id];
}

static void pty_master_enqueue(pty_t* pty, char c) {
    if (!pty) {
        return;
    }
    if (pty->m_count >= sizeof(pty->master_buf)) {
        return;
    }
    pty->master_buf[pty->m_tail] = c;
    pty->m_tail = (pty->m_tail + 1) % sizeof(pty->master_buf);
    pty->m_count++;
    tty_wake_readers(&pty->master_read_waiters);
}

static void tty_output_pty_slave(tty_t* tty, char c) {
    if (!tty || !tty->priv) {
        return;
    }
    pty_t* pty = (pty_t*)tty->priv;
    pty_master_enqueue(pty, c);
}

static void tty_set_default_termios(tty_t* tty) {
    tty->term.c_iflag = ICRNL;
    tty->term.c_oflag = 0;
    tty->term.c_cflag = 0;
    tty->term.c_lflag = ISIG | ICANON | ECHO;
    tty->term.c_cc[VINTR] = 3;   // Ctrl+C
    tty->term.c_cc[VQUIT] = 28;  // Ctrl+\
    // Use volatile to prevent compiler from optimizing away the write
    volatile cc_t* verase_ptr = &tty->term.c_cc[VERASE];
    *verase_ptr = 8;  // Backspace (ASCII 8)
    tty->term.c_cc[VKILL] = 21;  // Ctrl+U
    tty->term.c_cc[VEOF] = 4;    // Ctrl+D
    tty->term.c_cc[VSTART] = 17; // Ctrl+Q
    tty->term.c_cc[VSTOP] = 19;  // Ctrl+S
    tty->term.c_cc[VSUSP] = 26;  // Ctrl+Z
}

void tty_init(void) {
    mm_memset(&g_console_tty, 0, sizeof(g_console_tty));
    g_console_tty.id = 0;
    g_console_tty.is_pty = 0;
    g_console_tty.is_master = 0;
    g_console_tty.fg_pgid = 0;
    g_console_tty.output = tty_output_console;
    g_console_tty.winsz.ws_row = 25;
    g_console_tty.winsz.ws_col = 80;
    tty_set_default_termios(&g_console_tty);

    for (int i = 0; i < TTY_MAX_PTYS; ++i) {
        mm_memset(&g_ptys[i], 0, sizeof(pty_t));
        g_ptys[i].id = -1;
    }
}

tty_t* tty_get_console(void) {
    return &g_console_tty;
}

void tty_reset_termios(tty_t* tty) {
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

static void tty_signal_pgrp(tty_t* tty, int sig) {
    if (!tty || tty->fg_pgid == 0) {
        return;
    }
    sched_signal_pgrp(tty->fg_pgid, sig);
    // Wake any blocked readers so they can see they've been signaled/killed
    tty_wake_readers(&tty->read_waiters);
}

void tty_input_char(tty_t* tty, char c, int ctrl) {
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
        tty->output(tty, c);
    }
    tty_wake_readers(&tty->read_waiters);
}

long tty_read(tty_t* tty, void* buf, long count, int nonblock) {
    if (!tty || !buf || count <= 0) {
        return 0;
    }

    task_t* cur = sched_current();
    char* out = (char*)buf;
    long read = 0;

    while (read < count) {
        // Check if we've been killed or have a pending signal
        if (cur && (cur->state == TASK_ZOMBIE || cur->pending_signal > 0)) {
            // Mark task as exited due to signal
            if (cur->pending_signal > 0) {
                sched_mark_task_exited(cur, cur->exit_code);
                sched_yield();
            }
            return read > 0 ? read : -EINTR;
        }
        if (tty->eof_pending && tty->read_count == 0) {
            tty->eof_pending = 0;
            return read;
        }
        char c = 0;
        if (!tty_dequeue_read(tty, &c)) {
            if (read > 0 || nonblock) {
                break;
            }
            if (cur) {
                cur->state = TASK_BLOCKED;
                cur->wait_next = tty->read_waiters;
                cur->wait_channel = tty;
                tty->read_waiters = cur;
                sched_yield();
                // Check if we were killed or have a pending signal
                if (cur->state == TASK_ZOMBIE || cur->has_exited || cur->pending_signal > 0) {
                    // Mark task as exited due to signal
                    if (cur->pending_signal > 0) {
                        sched_mark_task_exited(cur, cur->exit_code);
                        sched_yield();
                    }
                    return read > 0 ? read : -EINTR;
                }
                continue;
            }
            break;
        }
        out[read++] = c;
        if ((tty->term.c_lflag & ICANON) && c == '\n') {
            break;
        }
    }

    return read;
}

long tty_write(tty_t* tty, const void* buf, long count) {
    if (!tty || !buf || count <= 0) {
        return 0;
    }
    const char* in = (const char*)buf;
    for (long i = 0; i < count; ++i) {
        char c = in[i];
        if (tty->term.c_iflag & INLCR) {
            if (c == '\n') {
                c = '\r';
            }
        }
        if (tty->term.c_iflag & IGNCR) {
            if (c == '\r') {
                continue;
            }
        }
        tty->output(tty, c);
    }
    return count;
}

int tty_ioctl(tty_t* tty, unsigned long req, void* argp, task_t* cur) {
    if (!tty) {
        return -ENOTTY;
    }
    switch (req) {
        case TCGETS:
            if (!argp) return -EFAULT;
            mm_memcpy(argp, &tty->term, sizeof(termios_k_t));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (!argp) return -EFAULT;
            mm_memcpy(&tty->term, argp, sizeof(termios_k_t));
            return 0;
        case TIOCGPGRP:
            if (!argp) return -EFAULT;
            *(int*)argp = tty->fg_pgid;
            return 0;
        case TIOCSPGRP:
            if (!argp) return -EFAULT;
            tty->fg_pgid = *(int*)argp;
            return 0;
        case TIOCSCTTY:
            if (!cur) return -EINVAL;
            cur->ctty = tty;
            return 0;
        case TIOCGWINSZ:
            if (!argp) return -EFAULT;
            mm_memcpy(argp, &tty->winsz, sizeof(struct winsize));
            return 0;
        case TIOCSWINSZ:
            if (!argp) return -EFAULT;
            mm_memcpy(&tty->winsz, argp, sizeof(struct winsize));
            return 0;
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

int tty_pty_allocate(int* out_id) {
    if (!out_id) {
        return -EINVAL;
    }
    for (int i = 0; i < TTY_MAX_PTYS; ++i) {
        if (g_ptys[i].id == -1) {
            pty_t* pty = &g_ptys[i];
            mm_memset(pty, 0, sizeof(pty_t));
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

tty_t* tty_get_pty_slave(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return NULL;
    }
    return &pty->slave;
}

int tty_pty_slave_open(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->slave_open = 1;
    return 0;
}

int tty_pty_is_allocated(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return 0;
    }
    return pty->master_open || pty->slave_open;
}

long tty_pty_master_read(int id, void* buf, long count, int nonblock) {
    pty_t* pty = tty_get_pty(id);
    if (!pty || !buf || count <= 0) {
        return -EINVAL;
    }
    task_t* cur = sched_current();
    char* out = (char*)buf;
    long read = 0;
    while (read < count) {
        if (pty->m_count == 0) {
            if (read > 0 || nonblock) {
                break;
            }
            if (cur) {
                cur->state = TASK_BLOCKED;
                cur->wait_next = pty->master_read_waiters;
                cur->wait_channel = pty;
                pty->master_read_waiters = cur;
                sched_yield();
                continue;
            }
            break;
        }
        out[read++] = pty->master_buf[pty->m_head];
        pty->m_head = (pty->m_head + 1) % sizeof(pty->master_buf);
        pty->m_count--;
    }
    return read;
}

long tty_pty_master_write(int id, const void* buf, long count) {
    pty_t* pty = tty_get_pty(id);
    if (!pty || !buf || count <= 0) {
        return -EINVAL;
    }
    const char* in = (const char*)buf;
    for (long i = 0; i < count; ++i) {
        tty_input_char(&pty->slave, in[i], 0);
    }
    return count;
}

int tty_pty_master_close(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->master_open = 0;
    if (!pty->slave_open) {
        pty->id = -1;
    }
    return 0;
}

int tty_pty_slave_close(int id) {
    pty_t* pty = tty_get_pty(id);
    if (!pty) {
        return -EINVAL;
    }
    pty->slave_open = 0;
    if (!pty->master_open) {
        pty->id = -1;
    }
    return 0;
}
