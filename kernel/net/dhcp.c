// LikeOS-64 DHCP Client
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"
#include "../../include/kernel/timer.h"
#include "../../include/kernel/random.h"

// DHCP Message types
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6
#define DHCP_RELEASE    7

// DHCP Options
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_REQ_IP         50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_T1             58
#define DHCP_OPT_T2             59
#define DHCP_OPT_DOMAIN_NAME    15
#define DHCP_OPT_CLASSLESS_ROUTES 121   // RFC 3442
#define DHCP_OPT_DOMAIN_SEARCH    119   // RFC 3397
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_END            255

// DHCP ports
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

// DHCP constants
#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2
#define DHCP_MAGIC_COOKIE   0x63825363

// DHCP states (RFC 2131 §4.4)
#define DHCP_STATE_IDLE         0
#define DHCP_STATE_DISCOVERING  1
#define DHCP_STATE_REQUESTING   2
#define DHCP_STATE_BOUND        3
#define DHCP_STATE_RENEWING     4
#define DHCP_STATE_REBINDING    5

static int dhcp_state = DHCP_STATE_IDLE;
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_offered_ip = 0;
static uint64_t dhcp_lease_start_ticks = 0;     // 100Hz
static uint32_t dhcp_lease_seconds = 0;
static uint32_t dhcp_t1_seconds = 0;            // RFC 2131 §4.4.5: lease/2
static uint32_t dhcp_t2_seconds = 0;            // 0.875 * lease
static net_device_t* dhcp_bound_dev = NULL;

// Offset of the variable-length options[] field inside dhcp_packet_t.
// sizeof(dhcp_packet_t) includes a fixed 312-byte options array, but real
// DHCP packets may be shorter.  All size checks must use this constant.
#define DHCP_FIXED_HDR  240

// Build DHCP packet with given message type
static int dhcp_build_packet(net_device_t* dev, uint8_t msg_type,
                              uint32_t requested_ip, uint32_t server_ip,
                              uint8_t* buf, int buflen) {
    if (buflen < DHCP_FIXED_HDR + 4) return -1;  // need at least header + minimal options

    dhcp_packet_t* dhcp = (dhcp_packet_t*)buf;
    for (int i = 0; i < buflen; i++) buf[i] = 0;

    dhcp->op = DHCP_BOOTREQUEST;
    dhcp->htype = 1;  // Ethernet
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = net_htonl(dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = net_htons(0x8000);  // Broadcast flag
    dhcp->ciaddr = 0;
    dhcp->yiaddr = 0;
    dhcp->siaddr = 0;
    dhcp->giaddr = 0;

    // Client hardware address
    for (int i = 0; i < 6; i++)
        dhcp->chaddr[i] = dev->mac_addr[i];

    // Magic cookie
    dhcp->magic_cookie = net_htonl(DHCP_MAGIC_COOKIE);

    // Options
    uint8_t* opt = dhcp->options;
    int pos = 0;

    // Message type
    opt[pos++] = DHCP_OPT_MSG_TYPE;
    opt[pos++] = 1;
    opt[pos++] = msg_type;

    if (msg_type == DHCP_REQUEST) {
        // Requested IP
        if (requested_ip != 0) {
            opt[pos++] = DHCP_OPT_REQ_IP;
            opt[pos++] = 4;
            opt[pos++] = (requested_ip >> 24) & 0xFF;
            opt[pos++] = (requested_ip >> 16) & 0xFF;
            opt[pos++] = (requested_ip >> 8) & 0xFF;
            opt[pos++] = requested_ip & 0xFF;
        }
        // Server identifier
        if (server_ip != 0) {
            opt[pos++] = DHCP_OPT_SERVER_ID;
            opt[pos++] = 4;
            opt[pos++] = (server_ip >> 24) & 0xFF;
            opt[pos++] = (server_ip >> 16) & 0xFF;
            opt[pos++] = (server_ip >> 8) & 0xFF;
            opt[pos++] = server_ip & 0xFF;
        }
    }

    // Parameter request list
    opt[pos++] = DHCP_OPT_PARAM_LIST;
    opt[pos++] = 3;
    opt[pos++] = DHCP_OPT_SUBNET_MASK;
    opt[pos++] = DHCP_OPT_ROUTER;
    opt[pos++] = DHCP_OPT_DNS;

    // End
    opt[pos++] = DHCP_OPT_END;

    return DHCP_FIXED_HDR + pos;
}

// Send a DHCP packet via UDP broadcast
static int dhcp_send(net_device_t* dev, uint8_t* pkt, int pktlen) {
    return udp_send(dev, 0xFFFFFFFF, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                    pkt, (uint16_t)pktlen);
}

// Parse DHCP options, extracting relevant fields
static void dhcp_parse_options(const uint8_t* options, int optlen,
                               uint8_t* msg_type, uint32_t* subnet,
                               uint32_t* router, uint32_t* dns,
                               uint32_t* server_id, uint32_t* lease,
                               uint32_t* t1, uint32_t* t2,
                               const uint8_t** classless, int* classless_len) {
    int i = 0;
    while (i < optlen) {
        uint8_t opt = options[i++];
        if (opt == DHCP_OPT_END) break;
        if (opt == 0) continue;  // Padding

        if (i >= optlen) break;
        uint8_t len = options[i++];
        if (i + len > optlen) break;

        switch (opt) {
        case DHCP_OPT_MSG_TYPE:
            if (len >= 1) *msg_type = options[i];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (len >= 4) *subnet = ((uint32_t)options[i] << 24) |
                                    ((uint32_t)options[i+1] << 16) |
                                    ((uint32_t)options[i+2] << 8) |
                                    options[i+3];
            break;
        case DHCP_OPT_ROUTER:
            if (len >= 4) *router = ((uint32_t)options[i] << 24) |
                                    ((uint32_t)options[i+1] << 16) |
                                    ((uint32_t)options[i+2] << 8) |
                                    options[i+3];
            break;
        case DHCP_OPT_DNS:
            if (len >= 4) *dns = ((uint32_t)options[i] << 24) |
                                 ((uint32_t)options[i+1] << 16) |
                                 ((uint32_t)options[i+2] << 8) |
                                 options[i+3];
            break;
        case DHCP_OPT_SERVER_ID:
            if (len >= 4) *server_id = ((uint32_t)options[i] << 24) |
                                       ((uint32_t)options[i+1] << 16) |
                                       ((uint32_t)options[i+2] << 8) |
                                       options[i+3];
            break;
        case DHCP_OPT_LEASE_TIME:
            if (len >= 4) *lease = ((uint32_t)options[i] << 24) |
                                   ((uint32_t)options[i+1] << 16) |
                                   ((uint32_t)options[i+2] << 8) |
                                   options[i+3];
            break;
        case DHCP_OPT_T1:
            if (len >= 4 && t1) *t1 = ((uint32_t)options[i] << 24) |
                                      ((uint32_t)options[i+1] << 16) |
                                      ((uint32_t)options[i+2] << 8) |
                                      options[i+3];
            break;
        case DHCP_OPT_T2:
            if (len >= 4 && t2) *t2 = ((uint32_t)options[i] << 24) |
                                      ((uint32_t)options[i+1] << 16) |
                                      ((uint32_t)options[i+2] << 8) |
                                      options[i+3];
            break;
        case DHCP_OPT_CLASSLESS_ROUTES:
            if (classless && classless_len) {
                *classless = &options[i];
                *classless_len = len;
            }
            break;
        }
        i += len;
    }
}

// Process received DHCP packet (called from udp_rx for port 68)
void dhcp_rx(net_device_t* dev, const uint8_t* data, uint16_t len) {

    // Minimum: fixed DHCP header up to (and including) magic_cookie + at least
    // 1 byte of options.
    if (len < DHCP_FIXED_HDR + 1) return;

    const dhcp_packet_t* dhcp = (const dhcp_packet_t*)data;

    // Verify it's a reply to our transaction
    if (dhcp->op != DHCP_BOOTREPLY) return;
    if (net_ntohl(dhcp->xid) != dhcp_xid) return;
    if (net_ntohl(dhcp->magic_cookie) != DHCP_MAGIC_COOKIE) return;

    // Parse options (variable-length tail after the fixed header)
    int optlen = (int)len - DHCP_FIXED_HDR;
    if (optlen <= 0) return;

    uint8_t msg_type = 0;
    uint32_t subnet = 0, router = 0, dns = 0, server_id = 0, lease = 0;
    uint32_t t1 = 0, t2 = 0;
    const uint8_t* classless = NULL;
    int classless_len = 0;
    dhcp_parse_options(dhcp->options, optlen, &msg_type, &subnet, &router,
                       &dns, &server_id, &lease, &t1, &t2,
                       &classless, &classless_len);

    uint32_t offered = net_ntohl(dhcp->yiaddr);

    switch (dhcp_state) {
    case DHCP_STATE_DISCOVERING:
        if (msg_type == DHCP_OFFER && offered != 0) {
            // Got an offer, send DHCP REQUEST
            dhcp_offered_ip = offered;
            dhcp_server_ip = server_id;
            dhcp_state = DHCP_STATE_REQUESTING;

            uint8_t pkt[576];
            int pktlen = dhcp_build_packet(dev, DHCP_REQUEST,
                                            dhcp_offered_ip, dhcp_server_ip,
                                            pkt, sizeof(pkt));
            if (pktlen > 0) {
                kprintf("[DHCP] Requesting IP %d.%d.%d.%d\n",
                        (dhcp_offered_ip >> 24) & 0xFF,
                        (dhcp_offered_ip >> 16) & 0xFF,
                        (dhcp_offered_ip >> 8) & 0xFF,
                        dhcp_offered_ip & 0xFF);
                dhcp_send(dev, pkt, pktlen);
            }
        }
        break;

    case DHCP_STATE_REQUESTING:
    case DHCP_STATE_RENEWING:
    case DHCP_STATE_REBINDING:
        if (msg_type == DHCP_ACK && offered != 0) {
            // Configure the network device
            dev->ip_addr = offered;
            dev->netmask = subnet;
            dev->gateway = router;
            dev->dns_server = dns;
            dhcp_state = DHCP_STATE_BOUND;
            dhcp_bound_dev = dev;
            dhcp_lease_seconds = lease ? lease : 3600;
            dhcp_t1_seconds = t1 ? t1 : (dhcp_lease_seconds / 2);
            // RFC 2131 §4.4.5: T2 = 0.875 * lease
            dhcp_t2_seconds = t2 ? t2 :
                              ((dhcp_lease_seconds * 7) / 8);
            dhcp_lease_start_ticks = timer_ticks();

            kprintf("[DHCP] Bound to %d.%d.%d.%d",
                    (offered >> 24) & 0xFF, (offered >> 16) & 0xFF,
                    (offered >> 8) & 0xFF, offered & 0xFF);
            kprintf(" mask %d.%d.%d.%d",
                    (subnet >> 24) & 0xFF, (subnet >> 16) & 0xFF,
                    (subnet >> 8) & 0xFF, subnet & 0xFF);
            kprintf(" gw %d.%d.%d.%d lease=%us\n",
                    (router >> 24) & 0xFF, (router >> 16) & 0xFF,
                    (router >> 8) & 0xFF, router & 0xFF,
                    (unsigned)dhcp_lease_seconds);

            // Add routes: connected subnet + default gateway
            if (subnet != 0) {
                uint32_t net_addr = offered & subnet;
                route_add(net_addr, subnet, 0, dev, 0, RTF_UP);
            }
            // RFC 3442: classless routes (option 121) override option 3
            // when present.  Each entry: width + dest + gateway.
            if (classless && classless_len > 0) {
                int i = 0;
                while (i < classless_len) {
                    uint8_t w = classless[i++];
                    if (w > 32) break;
                    uint8_t obytes = (w + 7) / 8;
                    if (i + obytes + 4 > classless_len) break;
                    uint32_t dst = 0;
                    for (uint8_t k = 0; k < obytes; k++)
                        dst |= ((uint32_t)classless[i + k]) << (24 - 8*k);
                    i += obytes;
                    uint32_t gw = ((uint32_t)classless[i] << 24) |
                                  ((uint32_t)classless[i+1] << 16) |
                                  ((uint32_t)classless[i+2] << 8) |
                                   classless[i+3];
                    i += 4;
                    uint32_t mask = w ? (0xFFFFFFFFU << (32 - w)) : 0;
                    route_add(dst, mask, gw, dev, 0,
                              RTF_UP | (gw ? RTF_GATEWAY : 0));
                }
            } else if (router != 0) {
                route_add(0, 0, router, dev, 0, RTF_UP | RTF_GATEWAY);
            }
        } else if (msg_type == DHCP_NAK) {
            kprintf("[DHCP] NAK received, restarting\n");
            dhcp_state = DHCP_STATE_IDLE;
            dhcp_discover(dev);
        }
        break;

    default:
        break;
    }
}

// Start DHCP discovery
int dhcp_discover(net_device_t* dev) {
    dhcp_xid = random_u32();
    dhcp_state = DHCP_STATE_DISCOVERING;
    dhcp_offered_ip = 0;
    dhcp_server_ip = 0;

    uint8_t pkt[576];
    int pktlen = dhcp_build_packet(dev, DHCP_DISCOVER, 0, 0, pkt, sizeof(pkt));
    if (pktlen > 0) {
        kprintf("[DHCP] Sending DISCOVER\n");
        dhcp_send(dev, pkt, pktlen);
    }
    return pktlen > 0 ? 0 : -1;
}

void dhcp_init(void) {
    dhcp_state = DHCP_STATE_IDLE;
    dhcp_xid = 0;
    dhcp_offered_ip = 0;
    dhcp_server_ip = 0;
}

int dhcp_configured(void) {
    return dhcp_state == DHCP_STATE_BOUND;
}

int dhcp_get_status(void) {
    return dhcp_state;
}

int dhcp_release(net_device_t* dev) {
    if (dhcp_state != DHCP_STATE_BOUND) return -1;
    
    // Build and send DHCP RELEASE
    uint8_t pkt[576];
    int pktlen = dhcp_build_packet(dev, DHCP_RELEASE, dev->ip_addr,
                                    dhcp_server_ip, pkt, sizeof(pkt));
    if (pktlen > 0) {
        // DHCP RELEASE is unicast to server
        udp_send(dev, dhcp_server_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 pkt, (uint16_t)pktlen);
    }
    
    // Clear network configuration
    dev->ip_addr = 0;
    dev->netmask = 0;
    dev->gateway = 0;
    dev->dns_server = 0;
    dhcp_state = DHCP_STATE_IDLE;
    dhcp_offered_ip = 0;
    
    kprintf("[DHCP] Released IP lease\n");
    return 0;
}

int dhcp_renew(net_device_t* dev) {
    if (dhcp_state != DHCP_STATE_BOUND) {
        // Not bound - do full discovery
        return dhcp_discover(dev);
    }
    
    // Send DHCP REQUEST to renew
    dhcp_state = DHCP_STATE_REQUESTING;
    uint8_t pkt[576];
    int pktlen = dhcp_build_packet(dev, DHCP_REQUEST,
                                    dhcp_offered_ip, dhcp_server_ip,
                                    pkt, sizeof(pkt));
    if (pktlen > 0) {
        kprintf("[DHCP] Renewing lease for %d.%d.%d.%d\n",
                (dhcp_offered_ip >> 24) & 0xFF,
                (dhcp_offered_ip >> 16) & 0xFF,
                (dhcp_offered_ip >> 8) & 0xFF,
                dhcp_offered_ip & 0xFF);
        dhcp_send(dev, pkt, pktlen);
    }
    return pktlen > 0 ? 0 : -1;
}

// Invalidate the cached lease.  Called by link drivers on a link-DOWN
// edge: the network we are attached to may have changed (cable moved
// to a different switch, VM hypervisor switched bridged<->NAT, etc.),
// so the previously bound IP/gateway/server are no longer valid.
// Clearing dev->ip_addr makes user-space dhclient correctly wait for a
// fresh ACK instead of treating the stale address as a successful
// renewal, and forcing dhcp_state back to IDLE makes the next
// dhcp_renew() perform a full DISCOVER instead of a (doomed) unicast
// renewal aimed at the previous network's DHCP server.
void dhcp_invalidate(net_device_t* dev) {
    if (!dev) return;

    // Tear down routes that depended on the old binding before we
    // forget the values.  Connected-subnet route was added with
    // (offered & subnet, subnet, 0); default route was added with
    // (0, 0, router).
    if (dev->netmask != 0) {
        uint32_t net_addr = dev->ip_addr & dev->netmask;
        (void)route_del(net_addr, dev->netmask, 0);
    }
    if (dev->gateway != 0) {
        (void)route_del(0, 0, dev->gateway);
    }

    dev->ip_addr = 0;
    dev->netmask = 0;
    dev->gateway = 0;
    dev->dns_server = 0;

    dhcp_state = DHCP_STATE_IDLE;
    dhcp_offered_ip = 0;
    dhcp_server_ip = 0;
}

// RFC 2131 §4.4.5: lease lifecycle.  Called periodically (e.g. once per
// second) by net_timer_tick.  Drives unicast renewal at T1, broadcast at T2,
// full re-discovery at lease expiry.
void dhcp_tick(void) {
    if (dhcp_state != DHCP_STATE_BOUND &&
        dhcp_state != DHCP_STATE_RENEWING &&
        dhcp_state != DHCP_STATE_REBINDING) return;
    if (!dhcp_bound_dev || dhcp_lease_seconds == 0) return;

    uint64_t now = timer_ticks();
    // Timer frequency is TSC-calibrated at boot and is NOT necessarily
    // 100 Hz (e.g. on VMware it lands elsewhere).  Hard-coding /100 here
    // caused T1 (lease/2) to trip far earlier than wall-clock half-lease,
    // making the client RENEW long before the lease was actually half-
    // expired.  Use the real timer rate.
    uint32_t hz = timer_get_frequency();
    if (hz == 0) hz = 100;
    uint64_t elapsed = (now - dhcp_lease_start_ticks) / hz;

    if (elapsed >= dhcp_lease_seconds) {
        // Lease expired — fall back to DISCOVER
        kprintf("[DHCP] Lease expired, restarting\n");
        dhcp_bound_dev->ip_addr = 0;
        dhcp_state = DHCP_STATE_IDLE;
        dhcp_discover(dhcp_bound_dev);
        return;
    }
    if (elapsed >= dhcp_t2_seconds && dhcp_state != DHCP_STATE_REBINDING) {
        kprintf("[DHCP] T2 reached, broadcasting REQUEST\n");
        dhcp_state = DHCP_STATE_REBINDING;
        uint8_t pkt[576];
        int n = dhcp_build_packet(dhcp_bound_dev, DHCP_REQUEST,
                                  dhcp_offered_ip, 0, pkt, sizeof(pkt));
        if (n > 0) dhcp_send(dhcp_bound_dev, pkt, n);
        return;
    }
    if (elapsed >= dhcp_t1_seconds && dhcp_state == DHCP_STATE_BOUND) {
        kprintf("[DHCP] T1 reached, unicast RENEW\n");
        dhcp_state = DHCP_STATE_RENEWING;
        uint8_t pkt[576];
        int n = dhcp_build_packet(dhcp_bound_dev, DHCP_REQUEST,
                                  dhcp_offered_ip, dhcp_server_ip,
                                  pkt, sizeof(pkt));
        // Unicast to current server per RFC 2131 §4.3.6
        if (n > 0)
            udp_send(dhcp_bound_dev, dhcp_server_ip,
                     DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                     pkt, (uint16_t)n);
    }
}
