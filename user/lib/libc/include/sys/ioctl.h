#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

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
#define TIOCLINUX  0x541C
#define FIONREAD   0x541B
#define FIONBIO    0x5421
#define FIOCLEX    0x5451
#define FIONCLEX   0x5450
#define FIOASYNC   0x5452
#define TIOCNOTTY  0x5422
#define TIOCSTI    0x5412
#define TIOCEXCL   0x540C
#define TIOCNXCL   0x540D
#define TIOCGETD   0x5424
#define TIOCSETD   0x5423
#define TIOCMGET   0x5415
#define TIOCMSET   0x5418
#define TIOCMBIC   0x5417
#define TIOCMBIS   0x5416

// Network interface ioctls (Linux-compatible)
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFDSTADDR  0x8917
#define SIOCSIFDSTADDR  0x8918
#define SIOCGIFBRDADDR  0x8919
#define SIOCSIFBRDADDR  0x891A
#define SIOCGIFNETMASK  0x891B
#define SIOCSIFNETMASK  0x891C
#define SIOCGIFMETRIC   0x891D
#define SIOCSIFMETRIC   0x891E
#define SIOCGIFMTU      0x8921
#define SIOCSIFMTU      0x8922
#define SIOCSIFHWADDR   0x8924
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933
#define SIOCGIFNAME     0x8910
#define SIOCGIFCOUNT    0x8938
#define SIOCGIFTXQLEN   0x8942
#define SIOCSIFTXQLEN   0x8943

// ARP ioctls
#define SIOCGARP        0x8954
#define SIOCSARP        0x8955
#define SIOCDARP        0x8953

// Routing ioctls
#define SIOCADDRT       0x890B
#define SIOCDELRT       0x890C
#define SIOCRTMSG       0x890D

int ioctl(int fd, unsigned long request, void* argp);

#endif
