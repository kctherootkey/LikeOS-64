// LikeOS-64 ICMP (Internet Control Message Protocol)
#include "../../include/kernel/net.h"
#include "../../include/kernel/console.h"

// Process received ICMP packet
void icmp_rx(net_device_t* dev, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t* icmp = (const icmp_header_t*)data;

    // Verify checksum over entire ICMP message
    if (ipv4_checksum(data, len) != 0) return;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        // Send echo reply
        uint8_t reply[NET_MTU_DEFAULT];
        if (len > NET_MTU_DEFAULT) return;

        // Copy entire ICMP message
        for (uint16_t i = 0; i < len; i++)
            reply[i] = data[i];

        // Modify header for reply
        icmp_header_t* r = (icmp_header_t*)reply;
        r->type = ICMP_ECHO_REPLY;
        r->code = 0;
        r->checksum = 0;
        r->checksum = ipv4_checksum(reply, len);

        ipv4_send(dev, src_ip, IP_PROTO_ICMP, reply, len);
    }
}
