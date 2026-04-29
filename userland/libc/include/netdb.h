#ifndef _NETDB_H
#define _NETDB_H

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* NET_GETINFO sub-commands */
#define NET_GET_ARP_TABLE     1
#define NET_GET_ROUTE_TABLE   2
#define NET_GET_TCP_CONNS     3
#define NET_GET_UDP_SOCKS     4
#define NET_GET_IFACE_INFO    5
#define NET_DNS_QUERY         6

/* DHCP_CONTROL sub-commands — must match kernel include/kernel/syscall.h */
#define DHCP_CMD_DISCOVER     1
#define DHCP_CMD_RELEASE      2
#define DHCP_CMD_RENEW        3
#define DHCP_CMD_STATUS       4
#define DHCP_CMD_REBIND       5

/* RAW_SEND sub-commands */
#define RAW_SEND_ICMP_ECHO    1
#define RAW_SEND_ARP_REQUEST  2

/* RAW_RECV sub-commands */
#define RAW_RECV_ICMP_REPLY   1
#define RAW_RECV_ARP_REPLY    2

/* ICMP types returned through RAW_RECV_ICMP_REPLY */
#define ICMP_ECHO_REPLY       0
#define ICMP_DEST_UNREACH     3
#define ICMP_TIME_EXCEEDED    11

/* Info structures for NET_GETINFO — must match kernel include/kernel/net.h */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  pad;
} net_arp_info_t;

typedef struct {
    uint32_t dst_net;
    uint32_t netmask;
    uint32_t gateway;
    uint16_t flags;
    uint16_t metric;
    char     dev_name[16];
} net_route_info_t;

typedef struct {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      state;
    uint32_t rx_queue;
    uint32_t tx_queue;
} net_tcp_info_t;

typedef struct {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint32_t rx_queue;
} net_udp_info_t;

typedef struct {
    char     name[16];
    uint8_t  mac[6];
    uint16_t mtu;
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    uint16_t flags;
    uint16_t pad;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;
} net_iface_info_t;

typedef struct __attribute__((packed)) {
    uint32_t src_ip;
    uint8_t type;
    uint8_t code;
    uint16_t seq;
    uint64_t rtt_us;
    uint8_t ttl;
    uint8_t reserved[7];
} icmp_reply_t;

/* ARP reply result - 6 bytes MAC */
typedef struct {
    uint8_t mac[6];
} arp_reply_t;

/* DNS resolve syscall wrapper - returns IP in host byte order */
int dns_resolve(const char *hostname, uint32_t *ip_out);

/* Reverse DNS lookup (PTR) - ip_nbo is IP in network byte order */
int dns_resolve_reverse(uint32_t ip_nbo, char *out, int maxlen);

/* Raw DNS query - returns raw DNS response for userland parsing */
#define DNS_TYPE_A      1
#define DNS_TYPE_PTR    12
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1

typedef struct {
    char     name[256];       /* Query name (input) */
    uint16_t qtype;           /* Query type: DNS_TYPE_A, DNS_TYPE_PTR (input) */
    uint16_t pad;
    int      response_len;    /* Response length (output, -1 on error) */
    uint8_t  response[512];   /* Raw DNS response (output) */
} dns_query_buf_t;

int dns_query(dns_query_buf_t *buf);

/* Network info syscall wrappers */
int net_getinfo(int subcmd, void *buf, int max_entries);
int dhcp_control(int subcmd);
int raw_send(int subcmd, uint32_t dst, uint32_t id_seq, uint32_t ttl);
int raw_recv(int subcmd, void *result, uint32_t param, uint64_t timeout);

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
