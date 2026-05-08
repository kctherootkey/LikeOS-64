// LikeOS-64 TTY/PTY subsystem
#ifndef _KERNEL_TTY_H_
#define _KERNEL_TTY_H_

#include "types.h"
#include "sched.h"

// Termios-like types
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 16

// Input flags
#define ICRNL   0x0001
#define INLCR   0x0002
#define IGNCR   0x0004

// Output flags (must match userland libc <termios.h>)
#define OPOST   0x0001
#define ONLCR   0x0002

// Local flags
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0004
#define TOSTOP  0x0008

// Control character indices
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VSTART  5
#define VSTOP   6
#define VSUSP   7
#define VMIN    8
#define VTIME   9

// ioctl request codes (Linux-compatible)
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN   0x80045430
#define TIOCSGUARD 0x5420

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

typedef struct termios_k {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
} termios_k_t;

typedef struct tty {
    int id;
    int is_pty;
    int is_master;
    int fg_pgid;
    termios_k_t term;
    struct winsize winsz;

    char canon_buf[256];
    uint16_t canon_len;
    char read_buf[1024];
    uint16_t read_head;
    uint16_t read_tail;
    uint16_t read_count;
    uint8_t eof_pending;

    // Mouse tracking state (DEC private modes)
    uint8_t mouse_tracking;    // mode 1000: normal tracking
    uint8_t mouse_btn_event;   // mode 1002: button-event tracking
    uint8_t mouse_sgr_mode;    // mode 1006: SGR extended coordinates
    uint8_t mouse_last_buttons; // last button state for release detection

    task_t* read_waiters;

    void (*output)(struct tty* tty, char c);
    void* priv; // pty linkage
} tty_t;

void tty_init(void);
tty_t* tty_get_console(void);
void tty_reset_termios(tty_t* tty);

// Input from keyboard/pty master
void tty_input_char(tty_t* tty, char c, int ctrl);
void tty_input_char_raw(tty_t* tty, char c);

// Mouse event reporting (called from mouse IRQ handler)
void tty_mouse_report(int pixel_x, int pixel_y, uint8_t buttons, uint8_t prev_buttons);
void tty_mouse_report_scroll(int pixel_x, int pixel_y, int scroll_delta);

// Read/write for tty endpoints
long tty_read(tty_t* tty, void* buf, long count, int nonblock);
long tty_write(tty_t* tty, const void* buf, long count);
int tty_ioctl(tty_t* tty, unsigned long req, void* argp, task_t* cur);

// PTY helpers
int tty_pty_allocate(int* out_id);
tty_t* tty_get_pty_slave(int id);
int tty_pty_slave_open(int id);
int tty_pty_is_allocated(int id);
long tty_pty_master_read(int id, void* buf, long count, int nonblock);
long tty_pty_master_write(int id, const void* buf, long count);
int tty_pty_master_close(int id);
int tty_pty_master_poll(int id, int events);
int tty_pty_slave_close(int id);

#endif
