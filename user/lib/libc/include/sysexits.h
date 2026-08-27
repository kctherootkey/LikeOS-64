/*
 * sysexits.h - conventional exit codes for programs, from BSD.
 *
 * Nothing enforces these; they are a convention shared by mailers, cron
 * implementations and anything else whose caller wants to distinguish "bad
 * usage" from "temporary failure" by exit status.  The values are fixed by
 * decades of use (EX_USAGE has been 64 since 4.0BSD) and every system that
 * ships this header uses the same numbers, so a program compiled here
 * exits with the codes its remote peer expects.
 */
#ifndef _SYSEXITS_H
#define _SYSEXITS_H

#define EX_OK           0   /* successful termination */

#define EX__BASE        64  /* base value for error messages */

#define EX_USAGE        64  /* command line usage error */
#define EX_DATAERR      65  /* data format error */
#define EX_NOINPUT      66  /* cannot open input */
#define EX_NOUSER       67  /* addressee unknown */
#define EX_NOHOST       68  /* host name unknown */
#define EX_UNAVAILABLE  69  /* service unavailable */
#define EX_SOFTWARE     70  /* internal software error */
#define EX_OSERR        71  /* system error (e.g., can't fork) */
#define EX_OSFILE       72  /* critical OS file missing */
#define EX_CANTCREAT    73  /* can't create (user) output file */
#define EX_IOERR        74  /* input/output error */
#define EX_TEMPFAIL     75  /* temp failure; user is invited to retry */
#define EX_PROTOCOL     76  /* remote error in protocol */
#define EX_NOPERM       77  /* permission denied */
#define EX_CONFIG       78  /* configuration error */

#define EX__MAX         78  /* maximum listed value */

#endif /* _SYSEXITS_H */
