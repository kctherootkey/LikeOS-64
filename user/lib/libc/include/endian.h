/*
 * endian.h - byte order of the machine, and conversions to and from it.
 *
 * x86-64 is little-endian, so the "to big endian" directions are the ones that
 * actually swap.  Everything is a macro over the compiler's builtins: these
 * appear in inner loops (X11 request marshalling, image conversion) and must
 * not cost a function call.
 *
 * Both spellings are provided because both are in wide use: the underscored
 * __BYTE_ORDER/__LITTLE_ENDIAN names, and the plain BYTE_ORDER/LITTLE_ENDIAN
 * ones that came from BSD.  X11's Xarch.h expects the underscored set and
 * derives the plain names from them if they are absent.
 */
#ifndef _ENDIAN_H
#define _ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <byteswap.h>

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

/* Trust the compiler's own view of the target rather than assuming: it is
 * authoritative, and this header would otherwise be silently wrong if the
 * libc is ever built for something other than x86-64. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#  if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define __BYTE_ORDER __BIG_ENDIAN
#  else
#    define __BYTE_ORDER __LITTLE_ENDIAN
#  endif
#else
#  define __BYTE_ORDER __LITTLE_ENDIAN
#endif

#define __FLOAT_WORD_ORDER __BYTE_ORDER

/* BSD spellings. */
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#define BYTE_ORDER    __BYTE_ORDER

#if __BYTE_ORDER == __LITTLE_ENDIAN

#define htobe16(x) bswap_16(x)
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) bswap_16(x)
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) bswap_32(x)
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) bswap_32(x)
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) bswap_64(x)
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) bswap_64(x)
#define le64toh(x) ((uint64_t)(x))

#else /* big endian */

#define htobe16(x) ((uint16_t)(x))
#define htole16(x) bswap_16(x)
#define be16toh(x) ((uint16_t)(x))
#define le16toh(x) bswap_16(x)

#define htobe32(x) ((uint32_t)(x))
#define htole32(x) bswap_32(x)
#define be32toh(x) ((uint32_t)(x))
#define le32toh(x) bswap_32(x)

#define htobe64(x) ((uint64_t)(x))
#define htole64(x) bswap_64(x)
#define be64toh(x) ((uint64_t)(x))
#define le64toh(x) bswap_64(x)

#endif

#ifdef __cplusplus
}
#endif

#endif /* _ENDIAN_H */
