/* Kernel-side probes for the TCP tests of the network harness
 * (host/net/t_tcp.c calls them as n0_kt_tcp_*, n1_kt_tcp_*): the state
 * of one socket's connection, the TCP counters by name, the socket,
 * TIME_WAIT and orphan counts, and a check of the red-black tree.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/net/tcp.h>
#include <kernel/net/rbtree.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include "kstubs.h"
#include "kt_tcp.h"

extern char kt_msg[256];

int kt_tcp_sockinfo(int sock, struct kt_tcp_sockinfo *o)
{
	net_socket_t *s = sock_get(sock);
	tcp_conn_t *c = s ? s->tcp : NULL;
	uint32_t n = 0;

	mm_memset(o, 0, sizeof(*o));
	if (!c)
		return -1;
	o->state = c->state;
	o->rcvbuf = c->rcvbuf;
	o->sndbuf = c->sndbuf;
	o->rmem_alloc = c->rmem_alloc;
	o->wmem_queued = c->wmem_queued;
	o->window_clamp = c->window_clamp;
	o->rcv_ssthresh = c->rcv_ssthresh;
	o->rcv_wnd = c->rcv_wnd;
	o->rcv_wscale = c->rcv_wscale;
	o->snd_wscale = c->snd_wscale;
	o->ws_enabled = c->ws_enabled;
	o->sack_ok = c->sack_ok;
	o->ts_enabled = c->ts_enabled;
	o->mss_cache = c->mss_cache;
	o->advmss = c->advmss;
	o->mss_clamp = c->mss_clamp;
	o->rcv_mss = c->rcv_mss;
	o->ack_quick = c->ack_quick;
	o->ack_pingpong = c->ack_pingpong;
	o->ack_ato_ms = (unsigned)tcp_ticks_to_ms(c->ack_ato);
	o->num_sacks = c->num_sacks;
	for (struct rb_node *r = rb_first(&c->ooo_queue); r; r = rb_next(r))
		n++;
	o->ooo_skbs = n;
	o->rcvq_skbs = skb_queue_len(&c->rcv_queue);
	o->rcvq_bytes = c->rcvq_bytes;
	o->cwnd = c->cwnd;
	o->ssthresh = c->ssthresh;
	o->packets_out = c->packets_out;
	o->snd_wnd = c->snd_wnd;
	o->userlocks = c->userlocks;
	o->scaling_ratio = c->scaling_ratio;
	o->rcv_space = c->rcvq_space.space;
	o->rcv_rtt_us = c->rcv_rtt_est.rtt_us >> 3;
	return 0;
}

unsigned long long kt_tcp_xmib(const char *name)
{
	return tcp_xmib_read(name);
}

unsigned kt_tcp_full_socks(void)
{
	tcp_reap_pending();
	return tcp_full_sock_count();
}

unsigned kt_tcp_tw(void)
{
	return tcp_tw_count();
}

unsigned kt_tcp_orphans(void)
{
	return tcp_orphan_count();
}

unsigned long long kt_tcp_mem(void)
{
	return tcp_mem_allocated();
}

/* ---- the red-black tree: random inserts and erases, every property
 * checked after each operation, the in-order walk sorted ---- */
struct kt_rbn {
	struct rb_node node;
	unsigned key;
	int in;
};

static unsigned kt_lcg(unsigned *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

static void kt_rb_insert(struct rb_root *root, struct kt_rbn *n)
{
	struct rb_node **p = &root->rb_node, *parent = NULL;

	while (*p) {
		struct kt_rbn *e = rb_entry(*p, struct kt_rbn, node);

		parent = *p;
		p = n->key < e->key ? &(*p)->rb_left : &(*p)->rb_right;
	}
	rb_link_node(&n->node, parent, p);
	rb_insert_color(&n->node, root);
}

static int kt_rb_sorted(struct rb_root *root, unsigned expect)
{
	unsigned last = 0, count = 0;

	for (struct rb_node *r = rb_first(root); r; r = rb_next(r)) {
		struct kt_rbn *e = rb_entry(r, struct kt_rbn, node);

		if (count && e->key < last)
			return 0;
		last = e->key;
		count++;
	}
	if (count != expect)
		return 0;
	count = 0;
	for (struct rb_node *r = rb_last(root); r; r = rb_prev(r))
		count++;
	return count == expect;
}

int kt_rbtree(unsigned seed, int nodes, int ops)
{
	struct kt_rbn *v = kalloc(sizeof(*v) * (size_t)nodes);
	struct rb_root root = RB_ROOT_INIT;
	unsigned in = 0;
	int rc = 0;

	if (!v) {
		ksnprintf(kt_msg, sizeof(kt_msg), "no memory");
		return __LINE__;
	}
	mm_memset(v, 0, sizeof(*v) * (size_t)nodes);
	for (int i = 0; i < ops; i++) {
		struct kt_rbn *n = &v[kt_lcg(&seed) % (unsigned)nodes];
		unsigned what = kt_lcg(&seed) % 8;

		if (!n->in) {
			n->key = kt_lcg(&seed) % 1000;
			kt_rb_insert(&root, n);
			n->in = 1;
			in++;
		} else if (what < 3) {
			// replace with a twin of the same key
			struct kt_rbn *m = NULL;

			for (int j = 0; j < nodes; j++)
				if (!v[j].in) {
					m = &v[j];
					break;
				}
			if (m) {
				m->key = n->key;
				rb_replace_node(&n->node, &m->node, &root);
				m->in = 1;
				n->in = 0;
			}
		} else {
			rb_erase(&n->node, &root);
			n->in = 0;
			in--;
		}
		if (rb_validate(&root) < 0) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "red-black property broken after op %d", i);
			rc = __LINE__;
			break;
		}
		if ((i % 64) == 0 && !kt_rb_sorted(&root, in)) {
			ksnprintf(kt_msg, sizeof(kt_msg),
				  "order or count wrong after op %d", i);
			rc = __LINE__;
			break;
		}
	}
	if (!rc && !kt_rb_sorted(&root, in)) {
		ksnprintf(kt_msg, sizeof(kt_msg), "final order wrong");
		rc = __LINE__;
	}
	kfree(v);
	return rc;
}
