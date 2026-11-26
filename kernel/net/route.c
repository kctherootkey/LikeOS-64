// LikeOS - IPv4 routing: the route interfaces on top of the FIB
//
// route_add/route_del (DHCP, SIOCADDRT/SIOCDELRT), route_lookup (device
// and next hop for an address), ip_route_output (a held dst_entry for a
// destination, with the local and broadcast cases), the path-MTU cache
// interface (pmtu_set/pmtu_get) and the routing table view for netstat.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/net.h>
#include <kernel/net/fib.h>
#include <kernel/net/dst.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/net_timer.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

#define ROUTE_EXC_GC_MS 60000

/* The local and broadcast entries of each device: [slot][0] RTN_LOCAL
 * (through loopback), [slot][1] RTN_BROADCAST.  Slot NET_MAX_DEVICES is
 * the loopback device itself. */
static struct dst_entry *special_dst[NET_MAX_DEVICES + 1][2];
static spinlock_t special_lock = SPINLOCK_INIT("route_special");
static net_timer_t exc_gc_timer;

static void route_exc_gc(net_timer_t *t)
{
	fib_exceptions_gc();
	net_timer_mod(t, timer_ticks() + net_ms_to_ticks(ROUTE_EXC_GC_MS));
}

static void route_netdev_event(struct netdev_notifier *nb, unsigned long ev,
			       net_device_t *dev)
{
	(void)nb;
	(void)dev;
	switch (ev) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEADDR:
	case NETDEV_CHANGEMTU:
		// every route handed out is looked up again
		fib_genid_bump();
		break;
	default:
		break;
	}
}

static struct netdev_notifier route_notifier = {
	.fn = route_netdev_event,
};

void route_init(void)
{
	dst_init();
	fib_init();
	register_netdevice_notifier(&route_notifier);
	net_timer_setup(&exc_gc_timer, route_exc_gc);
	net_timer_mod(&exc_gc_timer,
		      timer_ticks() + net_ms_to_ticks(ROUTE_EXC_GC_MS));
}

int route_add(uint32_t dst_net, uint32_t netmask, uint32_t gateway,
	      net_device_t *dev, uint32_t metric, uint16_t flags)
{
	struct fib_config cfg = { 0 };

	if (flags & RTF_HOST)
		netmask = 0xFFFFFFFFu;
	/* A gateway route needs a gateway, and bits outside the mask make a
	 * prefix that can never match: refused (both come from user space
	 * through SIOCADDRT). */
	if ((flags & RTF_GATEWAY) && gateway == 0)
		return -EINVAL;
	if (!dev && !(flags & RTF_REJECT)) {
		// the device the gateway is reached through
		uint32_t nh;

		if (gateway)
			dev = route_lookup(gateway, &nh);
		if (!dev)
			return -ENETUNREACH;
	}
	cfg.dst = dst_net;
	cfg.mask = netmask;
	cfg.gw = gateway;
	cfg.dev = dev;
	cfg.metric = metric;
	cfg.flags = flags;
	cfg.type = (flags & RTF_REJECT) ? RTN_UNREACHABLE : RTN_UNICAST;
	return fib_table_insert(&cfg);
}

int route_del(uint32_t dst_net, uint32_t netmask, uint32_t gateway)
{
	return fib_table_delete(dst_net, netmask, gateway);
}

// Longest-prefix-match route lookup.
// Returns: device to use, and sets *next_hop_out to the next-hop IP
net_device_t *route_lookup(uint32_t dst_ip, uint32_t *next_hop_out)
{
	struct fib_result res;

	if (fib_lookup(dst_ip, &res) == 0 && res.dev &&
	    (res.type == RTN_UNICAST || res.type == RTN_LOCAL)) {
		if (next_hop_out)
			*next_hop_out = res.gw ? res.gw : dst_ip;
		return res.dev;
	}
	if (next_hop_out)
		*next_hop_out = dst_ip;
	return NULL;
}

/* The held local (kind 0) or broadcast (kind 1) entry of `dev'. */
static struct dst_entry *special_get(net_device_t *dev, int slot, int kind)
{
	struct dst_entry *d, *old = NULL;
	net_device_t *lo = net_get_loopback();
	uint64_t f;

	spin_lock_irqsave(&special_lock, &f);
	d = special_dst[slot][kind];
	if (!d || !dst_valid(d) || d->prefsrc != dev->ip_addr) {
		old = d;
		special_dst[slot][kind] = NULL;
		d = dst_alloc(kind == 0 && lo ? lo : dev,
			      kind == 0 ? RTN_LOCAL : RTN_BROADCAST);
		if (d) {
			d->prefsrc = dev->ip_addr;
			special_dst[slot][kind] = d;
		}
	}
	dst_hold(d);
	spin_unlock_irqrestore(&special_lock, f);
	dst_release(old);
	return d;
}

int ip_route_output(uint32_t daddr, uint32_t saddr, net_device_t *oif,
		    struct dst_entry **out)
{
	net_device_t *lo = net_get_loopback();
	int n = net_device_count();
	int rc;

	(void)saddr;
	*out = NULL;
	if (daddr == 0xFFFFFFFFu) {
		net_device_t *dev = oif ? oif : net_get_default_device();
		int slot = NET_MAX_DEVICES;

		if (!dev)
			return -ENETUNREACH;
		for (int i = 0; i < n; i++)
			if (net_get_device(i) == dev)
				slot = i;
		if (slot == NET_MAX_DEVICES && dev != lo)
			return -ENETUNREACH;
		*out = special_get(dev, slot, 1);
		return *out ? 0 : -ENOBUFS;
	}
	for (int i = 0; i < n; i++) {
		net_device_t *dev = net_get_device(i);

		if (!dev || !dev->ip_addr)
			continue;
		if (daddr == dev->ip_addr) {
			*out = special_get(dev, i, 0);
			return *out ? 0 : -ENOBUFS;
		}
		if (dev->netmask && dev->netmask != 0xFFFFFFFFu &&
		    daddr == (dev->ip_addr | ~dev->netmask) &&
		    (!oif || oif == dev)) {
			*out = special_get(dev, i, 1);
			return *out ? 0 : -ENOBUFS;
		}
	}
	/* Anything else through the table -- only its routes out of the
	 * requested device, when the sender named one (SO_BINDTODEVICE, the
	 * DHCP client's renewal); 127.0.0.0/8 is a route to the loopback
	 * device there. */
	rc = fib_route_output(daddr, oif, out);
	return rc;
}

/* An upper layer saw `daddr' make progress (a TCP ACK that advanced the
 * window): the next hop towards it is reachable. */
void ip_confirm_daddr(uint32_t daddr)
{
	struct fib_result res;

	if (fib_lookup(daddr, &res) != 0 || !res.dev ||
	    res.dev == net_get_loopback())
		return;
	ip_neigh_confirm(res.dev, res.gw ? res.gw : daddr);
}

// RFC 1191 path MTU: a "fragmentation needed" report for `dst_ip'.
void pmtu_set(uint32_t dst_ip, uint16_t mtu)
{
	if (mtu < 68)
		mtu = 68; // RFC 791 minimum
	(void)fib_exception_set_pmtu(dst_ip, mtu);
}

uint16_t pmtu_get(uint32_t dst_ip)
{
	return (uint16_t)fib_exception_get_pmtu(dst_ip);
}

// Get route table for userspace
int net_get_route_table(net_route_info_t *entries, int max_entries)
{
	BUG_ON(entries == NULL);
	BUG_ON(max_entries < 0);
	return fib_dump(entries, max_entries);
}
