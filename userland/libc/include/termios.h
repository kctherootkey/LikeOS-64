#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <sys/types.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 16

// Input flags
#define ICRNL   0x0001
#define INLCR   0x0002
#define IGNCR   0x0004

// Local flags
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0004

// Control character indices
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VSTART  5
#define VSTOP   6
#define VSUSP   7

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
void cfmakeraw(struct termios* termios_p);

// tcsetattr actions
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#endif
