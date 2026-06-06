#include <termios.h>
#include <sys/ioctl.h>

int tcgetattr(int fd, struct termios* termios_p) {
    if (!termios_p) {
        return -1;
    }
    return ioctl(fd, TCGETS, termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    unsigned long req = TCSETS;
    if (!termios_p) {
        return -1;
    }
    if (optional_actions == TCSADRAIN) {
        req = TCSETSW;
    } else if (optional_actions == TCSAFLUSH) {
        req = TCSETSF;
    }
    return ioctl(fd, req, (void*)termios_p);
}

void cfmakeraw(struct termios* termios_p) {
    if (!termios_p) {
        return;
    }
    termios_p->c_iflag = 0;
    termios_p->c_oflag = 0;
    termios_p->c_cflag = 0;
    termios_p->c_lflag = 0;
}

int tcflush(int fd, int q) { (void)fd; (void)q; return 0; }
int tcdrain(int fd) { (void)fd; return 0; }
int tcflow(int fd, int a) { (void)fd; (void)a; return 0; }
int tcsendbreak(int fd, int d) { (void)fd; (void)d; return 0; }
pid_t tcgetsid(int fd) { (void)fd; return getpgrp(); }


/* Baud-rate accessors. We don't expose a configurable baud setting
 * (the kernel TTY layer is line-discipline only), so report a
 * conservative B38400 (15) and accept any input silently. */
speed_t cfgetispeed(const struct termios *t) { (void)t; return 15; }
speed_t cfgetospeed(const struct termios *t) { (void)t; return 15; }
int cfsetispeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { (void)t; (void)s; return 0; }
