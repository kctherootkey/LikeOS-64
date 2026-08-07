#ifndef _NETINET_IN_SYSTM_H
#define _NETINET_IN_SYSTM_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Network-order integer typedefs, as defined by 4.4BSD.  Historically these
 * distinguished host- vs network-order integers in the IP headers; today they
 * are plain fixed-width unsigned types kept for source compatibility with
 * software (OpenSSH, traceroute, ...) that includes this header.
 */

#include <stdint.h>

typedef uint16_t n_short; /* short as received from the net */
typedef uint32_t n_long;  /* long as received from the net  */
typedef uint32_t n_time;  /* ms since 00:00 GMT, byte-rev    */

#ifdef __cplusplus
}
#endif

#endif /* _NETINET_IN_SYSTM_H */
