// inet_pton / inet_ntop / inet_aton — RFC 3493 §6.6, §6.7.
// Only AF_INET supported.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <stdint.h>

int inet_aton(const char *cp, struct in_addr *inp) {
    uint32_t parts[4] = {0,0,0,0};
    int part = 0, digits = 0;
    if (!cp) return 0;
    while (*cp) {
        if (*cp >= '0' && *cp <= '9') {
            parts[part] = parts[part] * 10 + (uint32_t)(*cp - '0');
            if (parts[part] > 255) return 0;
            digits++;
        } else if (*cp == '.') {
            if (digits == 0 || part == 3) return 0;
            part++;
            digits = 0;
        } else {
            return 0;
        }
        cp++;
    }
    if (part != 3 || digits == 0) return 0;
    if (inp) inp->s_addr = htonl((parts[0] << 24) | (parts[1] << 16) |
                                  (parts[2] << 8)  |  parts[3]);
    return 1;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET) return -1;
    struct in_addr a;
    if (!inet_aton(src, &a)) return 0;
    *(uint32_t*)dst = a.s_addr;
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) return 0;
    if (size < INET_ADDRSTRLEN) return 0;
    uint32_t a = ntohl(*(const uint32_t*)src);
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        uint8_t o = (a >> (i*8)) & 0xFF;
        if (o >= 100) dst[p++] = '0' + o/100;
        if (o >= 10)  dst[p++] = '0' + (o/10)%10;
        dst[p++] = '0' + o%10;
        if (i) dst[p++] = '.';
    }
    dst[p] = 0;
    return dst;
}
