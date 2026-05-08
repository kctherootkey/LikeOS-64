/*
 * resolv.h - libresolv compat shim.
 *
 * The full libresolv API (DNS resolver, MX records, etc.) is not part of
 * this libc.  Portable applications include this header just to pick up
 * a declaration for b64_ntop / b64_pton; we forward-declare those here
 * so they can be supplied by either libc or a port-local compat file.
 */
#ifndef _RESOLV_H
#define _RESOLV_H

#include <sys/types.h>

/*
 * Declarations are intentionally omitted - portable ports (tmux, openssh)
 * include this header only to make sure b64_ntop / b64_pton are visible,
 * but they ship their own declaration in their compat layer.
 */

#endif /* _RESOLV_H */
