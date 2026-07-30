/*
 * byteswap.h - unconditional byte-order reversal.
 *
 * These swap regardless of the machine's own byte order; endian.h builds the
 * host/network conversions on top of them.  GCC's builtins compile to a single
 * bswap/rotate instruction, so there is no reason to hand-roll the shifts.
 */
#ifndef _BYTESWAP_H
#define _BYTESWAP_H

#include <stdint.h>

#define bswap_16(x) __builtin_bswap16((uint16_t)(x))
#define bswap_32(x) __builtin_bswap32((uint32_t)(x))
#define bswap_64(x) __builtin_bswap64((uint64_t)(x))

#endif /* _BYTESWAP_H */
