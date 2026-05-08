/*
 * arpa/nameser.h - empty libresolv compat stub.
 *
 * Some BSD compatibility shims (notably tmux's compat/base64.c) blindly
 * include this header to gain access to the b64_ntop/b64_pton helpers.
 * Those routines are actually declared in <resolv.h>; this header just
 * needs to exist to short-circuit the system one.
 */
#ifndef _ARPA_NAMESER_H
#define _ARPA_NAMESER_H
/* intentionally empty */
#endif
