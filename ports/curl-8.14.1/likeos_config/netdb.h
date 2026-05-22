/* likeos_config/netdb.h — wrapper around LikeOS <netdb.h> for curl.
 *
 * LikeOS netdb.h defines DNS_TYPE_A, DNS_TYPE_CNAME, DNS_TYPE_PTR, and
 * DNS_CLASS_IN as numeric macros.  curl's own lib/doh.h defines an enum
 * named DNStype with those same identifiers.  When the preprocessor expands
 * the macros inside the enum declaration, the enum members become numbers
 * (e.g.  `1 = 1`) and the compiler rejects them with "expected identifier
 * before numeric constant".  Undefine the conflicting names right after
 * pulling in the system header so only curl's own enum definition is used.
 */
#include_next <netdb.h>

#undef DNS_TYPE_A
#undef DNS_TYPE_PTR
#undef DNS_TYPE_CNAME
#undef DNS_CLASS_IN
