#ifndef _NETDB_H
#define _NETDB_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* DNS resolve syscall wrapper - returns IP in host byte order */
int dns_resolve(const char *hostname, uint32_t *ip_out);

/* Simplified gethostbyname-style structure */
struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};

#define h_addr h_addr_list[0]

/* Error codes */
#define HOST_NOT_FOUND  1
#define TRY_AGAIN       2
#define NO_RECOVERY     3
#define NO_DATA         4

#endif /* _NETDB_H */
