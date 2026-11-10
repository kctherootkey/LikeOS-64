/*
 * sys/ttydefaults.h - the conventional default terminal settings
 *
 * The 4.4BSD header: the control characters and mode flags a program uses
 * when it puts a terminal back into a sane state (getty, reset, stty sane).
 * Only constants; the c_cc slots they go into are the V* indices and the
 * flags the bits of <termios.h>.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _SYS_TTYDEFAULTS_H
#define _SYS_TTYDEFAULTS_H

#include <termios.h>

/* Default mode flags. */
#define TTYDEF_IFLAG (BRKINT | ISTRIP | ICRNL | IMAXBEL | IXON | IXANY)
#define TTYDEF_OFLAG (OPOST | ONLCR)
#define TTYDEF_LFLAG (ECHO | ICANON | ISIG | IEXTEN | ECHOE | ECHOKE | ECHOCTL)
#define TTYDEF_CFLAG (CREAD | CS7 | PARENB | HUPCL)
#define TTYDEF_SPEED (B9600)

/* The control character for a letter: CTRL('c') is ^C. */
#define CTRL(x) ((x) & 037)

/* Default control characters. */
#define CEOF     CTRL('d')
#define CEOL     _POSIX_VDISABLE
#define CERASE   0177
#define CINTR    CTRL('c')
#define CSTATUS  _POSIX_VDISABLE
#define CKILL    CTRL('u')
#define CMIN     1
#define CQUIT    034 /* ^\ */
#define CSUSP    CTRL('z')
#define CTIME    0
#define CDSUSP   CTRL('y')
#define CSTART   CTRL('q')
#define CSTOP    CTRL('s')
#define CLNEXT   CTRL('v')
#define CDISCARD CTRL('o')
#define CWERASE  CTRL('w')
#define CREPRINT CTRL('r')
#define CEOT     CEOF
/* Compatibility spellings. */
#define CBRK     CEOL
#define CRPRNT   CREPRINT
#define CFLUSH   CDISCARD

#endif /* _SYS_TTYDEFAULTS_H */
