/* <sys/prctl.h> -- process control knobs.
 *
 * The subset that has meaning here: the thread name (PR_SET_NAME /
 * PR_GET_NAME).  The other well-known options are accepted and answered
 * with their conventional defaults so that portable start-up code
 * (hardening, dumpable state, child-subreaper) runs unchanged. */
#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37

int prctl(int option, ...);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PRCTL_H */
