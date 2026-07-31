/*
 * paths.h - canonical pathnames as defined by 4.4BSD <paths.h>.
 */
#ifndef _PATHS_H
#define _PATHS_H

#define _PATH_BSHELL    "/bin/sh"
#define _PATH_CSHELL    "/bin/sh"
#define _PATH_DEFPATH   "/usr/local/bin:/usr/bin:/bin"
#define _PATH_DEV       "/dev/"
#define _PATH_DEVNULL   "/dev/null"
#define _PATH_TTY       "/dev/tty"
#define _PATH_CONSOLE   "/dev/console"
#define _PATH_STDPATH   "/usr/bin:/bin"
#define _PATH_TMP       "/tmp/"
#define _PATH_VARTMP    "/var/tmp/"
#define _PATH_VARRUN    "/var/run/"
#define _PATH_VI        "/usr/bin/vi"
#define _PATH_MAILDIR   "/var/mail"
/* Where rwhod deposits one whod.<host> file per host it has heard from.  4.4BSD
 * kept this in <protocols/rwhod.h>; it belongs here, and that header includes
 * this one so code written against either spelling compiles. */
#define _PATH_RWHODIR   "/var/spool/rwho"

#endif /* _PATHS_H */
