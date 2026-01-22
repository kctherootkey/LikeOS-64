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
