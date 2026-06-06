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
#define IXON    0x0008
#define IXOFF   0x0010
#define IXANY   0x0020
#define ISTRIP  0x0040
#define INPCK   0x0080
#define IGNBRK  0x0100
#define BRKINT  0x0200
#define IMAXBEL 0x0400
#define IUTF8   0x0800
#define PARMRK  0x1000
#define IUCLC   0x2000

// Output flags
#define OPOST   0x0001
#define ONLCR   0x0002
#define OCRNL   0x0004
#define ONOCR   0x0008
#define ONLRET  0x0010
#define OFILL   0x0040
#define OFDEL   0x0080
#define OLCUC   0x0100
#define NLDLY   0x0100
#define CRDLY   0x0600
#define TABDLY  0x1800
#define BSDLY   0x2000
#define VTDLY   0x4000
#define FFDLY   0x8000

// Local flags
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0004
#define TOSTOP  0x0008
#define IEXTEN  0x0010
#define ECHOE   0x0020
#define ECHOK   0x0040
#define ECHONL  0x0080
#define NOFLSH  0x0100
#define ECHOCTL 0x0200
#define ECHOKE  0x0400

// Control flags
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

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
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t s);
int cfsetospeed(struct termios *t, speed_t s);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
void cfmakeraw(struct termios* termios_p);

// tcsetattr actions
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* "Disabled" sentinel for c_cc[] entries (POSIX). */
#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE '\377'
#endif

// tcflush queue selectors
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

// tcflow actions
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

int tcflush(int fd, int queue_selector);
int tcdrain(int fd);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);
pid_t tcgetsid(int fd);

#endif
