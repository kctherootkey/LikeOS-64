// LikeOS IGMPv2 (RFC 2236): a Membership Report on join (repeated once
// after a random delay of up to the Unsolicited Report Interval), Leave
// Group on the last drop, and the answer to a query: a report for each
// joined group the query covers, after a random delay of up to the query's
// Max Response Time (a report that would go later is moved earlier, never
// later), so the hosts of a LAN do not all answer at once.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>
#include <kernel/ke/syscall.h> /* errno constants returned to the socket layer */
#include <kernel/net/net_timer.h>
#include <kernel/dev/rand/random.h>
#include <kernel/ke/timer.h>

#define IGMP_UNSOLICITED_REPORT_MS 10000
#define IGMP_ALL_HOSTS 0xE0000001u

// IGMPv2 packet
typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t max_resp; // tenths of a second (queries) / 0 (reports)
	uint16_t checksum;
	uint32_t group_addr; // network byte order
} igmp_packet_t;

#define IGMP_TYPE_QUERY 0x11
#define IGMP_TYPE_V1_REPORT 0x12
#define IGMP_TYPE_V2_REPORT 0x16
#define IGMP_TYPE_LEAVE 0x17

// Lock order: igmp_lock -> the device's multicast list lock (dev->mc.lock)
// and the net timer wheel lock.  Nothing else is taken under igmp_lock;
// reports and the driver's filter update happen after it is dropped.
//
// Per-device joined-group table.
//
// Membership is REFCOUNTED: this table is per (device, group), but any number
// of sockets — in any number of processes — may join the same group on the same
// device.  The device must stay joined until the LAST of them leaves.  Without
// a count, the first leave tore the membership down and every other holder's
// leave then failed (and, worse, stopped receiving the group's traffic while
// still believing it was a member).
#define IGMP_MAX_GROUPS 16
typedef struct {
	int active;
	unsigned refs; // joiners holding this membership; 0 == free
	net_device_t *dev;
	uint32_t group; // host byte order
	net_timer_t report; // a delayed report is due
} igmp_membership_t;

static igmp_membership_t igmp_table[IGMP_MAX_GROUPS];
static spinlock_t igmp_lock = SPINLOCK_INIT("igmp");

static void igmp_report_timer(net_timer_t *t);

void igmp_init(void)
{
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		igmp_table[i].active = 0;
		igmp_table[i].refs = 0;
		net_timer_setup(&igmp_table[i].report, igmp_report_timer);
	}
}

int igmp_is_member(net_device_t *dev, uint32_t group)
{
	uint64_t flags;
	int found = 0;

	spin_lock_irqsave(&igmp_lock, &flags);
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		if (igmp_table[i].active && igmp_table[i].dev == dev &&
		    igmp_table[i].group == group) {
			found = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&igmp_lock, flags);
	return found;
}

/* Arm entry e's report for a random moment within `max_ms' -- unless one
 * is already due sooner (igmp_lock held). */
static void igmp_schedule_report(igmp_membership_t *e, uint64_t max_ms,
				 uint32_t rnd)
{
	uint64_t delay = max_ms ? (uint64_t)rnd % max_ms : 0;
	uint64_t when = timer_ticks() + net_ms_to_ticks(delay);

	if (net_timer_pending(&e->report) &&
	    (int64_t)(e->report.expires - when) <= 0)
		return;
	net_timer_mod(&e->report, when);
}

static int igmp_send(net_device_t *dev, uint32_t dst_group, uint8_t type)
{
	BUG_ON(dev == NULL);
	if (!dev)
		return -1;
	igmp_packet_t pkt;
	pkt.type = type;
	pkt.max_resp = 0;
	pkt.checksum = 0;
	pkt.group_addr = net_htonl(dst_group);
	pkt.checksum = ipv4_checksum(&pkt, sizeof(pkt));

	// IGMP messages must use TTL=1 (RFC 2236 sec 9).  Destination IP is the
	// group itself for reports, 224.0.0.2 (all-routers) for leaves.
	uint32_t dst_ip = (type == IGMP_TYPE_LEAVE) ? 0xE0000002U : dst_group;
	return ipv4_send_full(dev, dst_ip, /*proto*/ 2, (const uint8_t *)&pkt,
			      sizeof(pkt), 1, 0);
}

// RFC 1112: the Ethernet group address of an IPv4 group (host order).
static void igmp_group_mac(uint32_t group, uint8_t mac[ETH_ALEN])
{
	mac[0] = 0x01;
	mac[1] = 0x00;
	mac[2] = 0x5E;
	mac[3] = (uint8_t)((group >> 16) & 0x7F);
	mac[4] = (uint8_t)(group >> 8);
	mac[5] = (uint8_t)group;
}

int igmp_join(net_device_t *dev, uint32_t group)
{
	BUG_ON(dev == NULL);
	WARN_ON((group >> 28) !=
		0xE); /* igmp_join: address not in multicast range 224.x.x.x - 239.x.x.x */
	if (!dev || (group >> 28) != 0xE)
		return -EINVAL;

	uint32_t rnd = random_u32(); // not under igmp_lock
	int lo = dev == net_get_loopback(), sync = 0;
	uint64_t flags;
	spin_lock_irqsave(&igmp_lock, &flags);

	int slot = -1;
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		if (igmp_table[i].active && igmp_table[i].dev == dev &&
		    igmp_table[i].group == group) {
			/* Already joined by someone else: take a reference and
			 * skip the report — the device is already a member. */
			igmp_table[i].refs++;
			spin_unlock_irqrestore(&igmp_lock, flags);
			return 0;
		}
		if (!igmp_table[i].active && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		spin_unlock_irqrestore(&igmp_lock, flags);
		return -ENOBUFS;
	}
	igmp_table[slot].active = 1;
	igmp_table[slot].refs = 1;
	igmp_table[slot].dev = dev;
	igmp_table[slot].group = group;
	// the unsolicited report goes out twice (RFC 2236 3)
	if (!lo && group != IGMP_ALL_HOSTS)
		igmp_schedule_report(&igmp_table[slot],
				     IGMP_UNSOLICITED_REPORT_MS, rnd);
	if (!lo) {
		uint8_t mac[ETH_ALEN];

		/* The device receives the group's frames (its multicast
		 * list), changed with the membership under igmp_lock: a
		 * racing last leave of the group cannot reorder the two. */
		igmp_group_mac(group, mac);
		sync = __dev_mc_add(dev, mac) > 0;
	}
	spin_unlock_irqrestore(&igmp_lock, flags);

	if (sync)
		dev_set_rx_mode(dev);
	// Send unsolicited Membership Report (RFC 2236 sec 3).
	if (!lo)
		igmp_send(dev, group, IGMP_TYPE_V2_REPORT);
	return 0;
}

int igmp_leave(net_device_t *dev, uint32_t group)
{
	int lo = dev == net_get_loopback(), sync = 0;
	uint64_t flags;
	spin_lock_irqsave(&igmp_lock, &flags);
	int found = 0, last = 0;
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		if (igmp_table[i].active && igmp_table[i].dev == dev &&
		    igmp_table[i].group == group) {
			found = 1;
			/* Only the final joiner tears the membership down; the
			 * others just drop their reference. */
			if (igmp_table[i].refs > 0)
				igmp_table[i].refs--;
			if (igmp_table[i].refs == 0) {
				igmp_table[i].active = 0;
				net_timer_del(&igmp_table[i].report);
				last = 1;
			}
			break;
		}
	}
	if (last && !lo) {
		uint8_t mac[ETH_ALEN];

		// off the device's list together with the membership
		igmp_group_mac(group, mac);
		sync = __dev_mc_del(dev, mac) > 0;
	}
	spin_unlock_irqrestore(&igmp_lock, flags);
	if (sync)
		dev_set_rx_mode(dev);
	if (last && !lo)
		igmp_send(dev, group, IGMP_TYPE_LEAVE);
	/* Not a member is a caller error, not a permission problem: return a
	 * real errno.  The old bare -1 was read by the socket layer as a
	 * negative errno and surfaced to userspace as EPERM. */
	return found ? 0 : -EADDRNOTAVAIL;
}

void igmp_rx(net_device_t *dev, uint32_t src_ip, const uint8_t *data,
	     uint16_t len)
{
	BUG_ON(dev == NULL);
	BUG_ON(data == NULL);
	(void)src_ip;
	if (len < sizeof(igmp_packet_t))
		return;
	if (ipv4_checksum(data, len) != 0)
		return;

	const igmp_packet_t *pkt = (const igmp_packet_t *)data;
	if (pkt->type != IGMP_TYPE_QUERY)
		return;

	uint32_t qgroup = net_ntohl(pkt->group_addr);
	/* Max Response Time in tenths of a second; 0 is an IGMPv1 query,
	 * which allows 10 s. */
	uint64_t max_ms = pkt->max_resp ? (uint64_t)pkt->max_resp * 100 : 10000;
	uint32_t rnd = random_u32();

	uint64_t flags;
	spin_lock_irqsave(&igmp_lock, &flags);
	for (int i = 0; i < IGMP_MAX_GROUPS; i++) {
		if (!igmp_table[i].active || igmp_table[i].dev != dev)
			continue;
		if (igmp_table[i].group == IGMP_ALL_HOSTS)
			continue; // never reported
		if (qgroup == 0 || qgroup == igmp_table[i].group)
			igmp_schedule_report(&igmp_table[i], max_ms,
					     rnd * (uint32_t)(i + 1));
	}
	spin_unlock_irqrestore(&igmp_lock, flags);
}

/* A delayed report is due (softirq). */
static void igmp_report_timer(net_timer_t *t)
{
	igmp_membership_t *e = (igmp_membership_t *)((uint8_t *)t -
		__builtin_offsetof(igmp_membership_t, report));
	net_device_t *dev = NULL;
	uint32_t group = 0;
	uint64_t flags;

	spin_lock_irqsave(&igmp_lock, &flags);
	if (e->active) {
		dev = e->dev;
		group = e->group;
	}
	spin_unlock_irqrestore(&igmp_lock, flags);
	if (dev)
		igmp_send(dev, group, IGMP_TYPE_V2_REPORT);
}
