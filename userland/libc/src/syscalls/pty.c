#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <fcntl.h>

int posix_openpt(int flags) {
    return open("/dev/ptmx", flags);
}

int grantpt(int fd) {
    (void)fd;
    return 0;
}

int unlockpt(int fd) {
    (void)fd;
    return 0;
}

char* ptsname(int fd) {
    static char buf[32];
    int pty = -1;
    if (ioctl(fd, TIOCGPTN, &pty) != 0) {
        return NULL;
    }
    snprintf(buf, sizeof(buf), "/dev/pts/%d", pty);
    return buf;
}
