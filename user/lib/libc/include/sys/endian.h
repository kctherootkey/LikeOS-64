/*
 * sys/endian.h - the BSD spelling of <endian.h>.
 *
 * Two names for the same header exist for historical reasons: BSD put it in
 * sys/, the GNU world at the top level.  Portable code therefore tends to
 * #include one or the other behind an #ifdef chain, and a system providing
 * only one spelling falls into whichever arm the author did not test.
 *
 * Providing both costs nothing and removes a whole class of port breakage, so
 * this header is the BSD name for the same definitions.
 *
 * It also adds the letohNN and betohNN aliases.  Those are OpenBSD's spelling
 * of leNNtoh and beNNtoh, and code reaching for this header often uses them --
 * libpciaccess is one example, where the non-BSD arm of its #ifdef chain still
 * calls letoh16().  Without them the header would resolve but the call would
 * not.
 */
#ifndef _SYS_ENDIAN_H
#define _SYS_ENDIAN_H

#include <endian.h>

/* OpenBSD aliases.  Same direction as le16toh()/be16toh(), different name. */
#define letoh16(x) le16toh(x)
#define letoh32(x) le32toh(x)
#define letoh64(x) le64toh(x)
#define betoh16(x) be16toh(x)
#define betoh32(x) be32toh(x)
#define betoh64(x) be64toh(x)

/* BSD also spells the swaps bswap16/32/64, without the underscore that
 * <byteswap.h> uses. */
#ifndef bswap16
#define bswap16(x) bswap_16(x)
#endif
#ifndef bswap32
#define bswap32(x) bswap_32(x)
#endif
#ifndef bswap64
#define bswap64(x) bswap_64(x)
#endif

#endif /* _SYS_ENDIAN_H */
