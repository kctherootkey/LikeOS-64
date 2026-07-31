/*
 * protocols/rwhod.h - on-the-wire format of the rwho protocol, as defined by
 * 4.3BSD and provided under this name by every system since.
 *
 * rwhod broadcasts one of these packets per interval describing its host's
 * uptime, load average and logged-in users, and writes what it receives from
 * other hosts into one file per host under _PATH_RWHODIR.  Readers -- xload's
 * -remote option is one -- open that file and take the load figures from it.
 *
 * The layout is a wire format, so the field types are fixed-width where the
 * original was and the structure must not be rearranged: a file written by one
 * host is read byte-for-byte by another.
 *
 * Providing the header does not provide the daemon.  Nothing on this system
 * broadcasts or collects these packets, so a reader will find no files and say
 * so -- exactly as it would on any system where rwhod is not running.  The
 * header is here because programs conditionally compile against it and expect
 * it to exist.
 */
#ifndef _PROTOCOLS_RWHOD_H
#define _PROTOCOLS_RWHOD_H 1

#include <sys/types.h>
#include <stdint.h>

/* One logged-in user, in the form the protocol carries rather than the local
 * utmp layout -- hence the separate structure with its own fixed widths. */
struct outmp {
	char out_line[8]; /* tty name */
	char out_name[8]; /* user id */
	int32_t out_time; /* time logged on */
};

struct whod {
	char wd_vers; /* protocol version, WHODVERSION */
	char wd_type; /* packet type, WHODTYPE_* */
	char wd_pad[2];
	int wd_sendtime; /* stamped by the sender */
	int wd_recvtime; /* stamped by the receiver */
	char wd_hostname[32]; /* sending host's name */
	int wd_loadav[3]; /* 1, 5 and 15 minute load, scaled by 100 */
	int wd_boottime; /* when the sending host booted */
	struct whoent {
		struct outmp we_utmp; /* the tty and who is on it */
		int we_idle; /* seconds that tty has been idle */
	} wd_we[1024 / sizeof(struct whoent)];
};

#define WHODVERSION 1
#define WHODTYPE_STATUS 1 /* host status */

/* _PATH_RWHODIR lives in <paths.h>, where the rest of the canonical pathnames
 * are.  Included here because 4.3BSD defined it in this header and code still
 * expects to get it by including only this one. */
#include <paths.h>

#endif /* _PROTOCOLS_RWHOD_H */
