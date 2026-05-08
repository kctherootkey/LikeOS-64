/*
 * pty_util.c - openpty(3) / forkpty(3) / login_tty(3) implementations.
 *
 * Built atop the existing posix_openpt(3) + grantpt + unlockpt + ptsname
 * helpers.  login_tty arranges for the slave fd to become the controlling
 * tty of the calling process via setsid + ioctl(TIOCSCTTY) and rebinds
 * stdin/stdout/stderr to it.
 */
#include "../../include/util.h"
#include "../../include/unistd.h"
#include "../../include/fcntl.h"
#include "../../include/string.h"
#include "../../include/errno.h"
#include "../../include/termios.h"
#include "../../include/sys/ioctl.h"
#include "../../include/stdio.h"

int openpty(int* amaster, int* aslave, char* name,
            const struct termios* termp, const struct winsize* winp)
{
    int master, slave;
    char* slave_name;

    if (!amaster || !aslave) { errno = EINVAL; return -1; }

    master = posix_openpt(O_RDWR);
    if (master < 0) return -1;
    if (grantpt(master) < 0)  goto fail;
    if (unlockpt(master) < 0) goto fail;

    slave_name = ptsname(master);
    if (!slave_name) goto fail;

    slave = open(slave_name, O_RDWR);
    if (slave < 0) goto fail;

    if (termp) (void)tcsetattr(slave, 0 /* TCSANOW */, termp);
    if (winp)  (void)ioctl(slave, TIOCSWINSZ, (void*)winp);

    *amaster = master;
    *aslave  = slave;
    if (name) {
        size_t i = 0;
        while (slave_name[i] && i < 31) { name[i] = slave_name[i]; i++; }
        name[i] = '\0';
    }
    return 0;

fail:
    {
        int saved = errno;
        close(master);
        errno = saved;
        return -1;
    }
}

int login_tty(int fd)
{
    (void)setsid();
    if (ioctl(fd, TIOCSCTTY, 0) == -1) {
        /* Not fatal on PTYs already without a controlling tty. */
    }
    if (dup2(fd, 0) < 0) return -1;
    if (dup2(fd, 1) < 0) return -1;
    if (dup2(fd, 2) < 0) return -1;
    if (fd > 2) close(fd);
    return 0;
}

pid_t forkpty(int* amaster, char* name,
              const struct termios* termp, const struct winsize* winp)
{
    int master, slave;
    pid_t pid;

    if (openpty(&master, &slave, name, termp, winp) == -1)
        return -1;

    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(master); close(slave);
        errno = saved;
        return -1;
    }
    if (pid == 0) {
        /* Child - replace stdio with the slave end and exit on failure. */
        close(master);
        if (login_tty(slave) == -1)
            _exit(1);
        return 0;
    }
    /* Parent - keep the master, drop the slave. */
    if (amaster) *amaster = master; else close(master);
    close(slave);
    return pid;
}
