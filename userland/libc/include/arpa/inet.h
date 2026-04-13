#ifndef _ARPA_INET_H
#define _ARPA_INET_H

#include <stdint.h>
#include <netinet/in.h>

// Convert IPv4 dotted-decimal string to network byte order
static inline in_addr_t inet_addr(const char *cp) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    while (*cp && part < 4) {
        if (*cp >= '0' && *cp <= '9') {
            parts[part] = parts[part] * 10 + (*cp - '0');
        } else if (*cp == '.') {
            part++;
        } else {
            return (in_addr_t)-1;
        }
        cp++;
    }
    if (part != 3) return (in_addr_t)-1;
    for (int i = 0; i < 4; i++)
        if (parts[i] > 255) return (in_addr_t)-1;

    return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
}

// Convert network byte order to dotted-decimal string (static buffer)
static inline char* inet_ntoa(struct in_addr in) {
    static char buf[16];
    uint32_t addr = ntohl(in.s_addr);
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        uint8_t octet = (addr >> (i * 8)) & 0xFF;
        if (octet >= 100) buf[pos++] = '0' + octet / 100;
        if (octet >= 10)  buf[pos++] = '0' + (octet / 10) % 10;
        buf[pos++] = '0' + octet % 10;
        if (i > 0) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    return buf;
}

#endif /* _ARPA_INET_H */
