// LikeOS — datagram socket queues (UDP and raw sockets)
//
// A datagram socket keeps what arrived for it as skbs on s->rx_queue and
// the errors reported to it (IP_RECVERR) on s->err_queue.  Both are
// charged to the socket's receive buffer by truesize: a datagram is taken
// while what the socket holds does not exceed SO_RCVBUF (so one datagram of
// any size always fits an empty socket) and dropped otherwise.
//
// s->udp_rx_ready is the readiness flag the receive paths park on: set
// when a datagram or an error is queued, cleared (under s->lock) when the
// receive queue is empty again and no error is pending.
//
// Lock order: the protocol's table lock -> s->lock.  s->lock is held only
// for queue manipulation; copies to user memory happen with no lock held
// (a peeked datagram is held by a reference meanwhile).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/net/ip.h>
#include <kernel/net/stats.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/mm/memory.h>
#include <kernel/uapi/bug.h>

extern void poll_notify_wq(struct wait_queue_head *);

/* Every receive path's readiness: something to read or report. */
static void dgram_update_ready_locked(net_socket_t *s)
{
	s->udp_rx_ready = !skb_queue_empty(&s->rx_queue) ||
			  !skb_queue_empty(&s->err_queue) || s->error != 0;
}

int dgram_enqueue(net_socket_t *s, sk_buff_t *skb)
{
	uint64_t f;

	spin_lock_irqsave(&s->lock, &f);
	if (!s->active || s->closed) {
		spin_unlock_irqrestore(&s->lock, f);
		return -1;
	}
	if (s->rx_mem > (uint32_t)s->rcvbuf_size) {
		s->rx_drops++;
		spin_unlock_irqrestore(&s->lock, f);
		return -1;
	}
	s->rx_mem += skb->truesize;
	/* When it arrived (SO_TIMESTAMP, SIOCGSTAMP), if the device path
	 * did not already say. */
	if (!skb->tstamp_ns)
		skb->tstamp_ns = hrtimer_now_ns();
	__skb_queue_tail(&s->rx_queue, skb);
	s->udp_rx_ready = 1;
	spin_unlock_irqrestore(&s->lock, f);
	return 0;
}

void dgram_wake(net_socket_t *s)
{
	sched_wake_channel((void *)&s->udp_rx_ready);
	poll_notify_wq(&s->poll_wq);
}

sk_buff_t *dgram_dequeue(net_socket_t *s, int peek)
{
	sk_buff_t *skb;
	uint64_t f;

	spin_lock_irqsave(&s->lock, &f);
	if (peek) {
		skb = skb_get(skb_peek(&s->rx_queue));
	} else {
		skb = __skb_dequeue(&s->rx_queue);
		if (skb) {
			BUG_ON(s->rx_mem < skb->truesize);
			s->rx_mem -= skb->truesize;
		}
	}
	dgram_update_ready_locked(s);
	spin_unlock_irqrestore(&s->lock, f);
	return skb;
}

uint32_t dgram_first_len(net_socket_t *s)
{
	uint32_t n = 0;
	uint64_t f;

	spin_lock_irqsave(&s->lock, &f);
	if (skb_peek(&s->rx_queue))
		n = skb_peek(&s->rx_queue)->len;
	spin_unlock_irqrestore(&s->lock, f);
	return n;
}

int dgram_take_error(net_socket_t *s)
{
	uint64_t f;
	int e;

	spin_lock_irqsave(&s->lock, &f);
	e = s->error;
	s->error = 0;
	dgram_update_ready_locked(s);
	spin_unlock_irqrestore(&s->lock, f);
	return e;
}

sk_buff_t *dgram_dequeue_err(net_socket_t *s)
{
	sk_buff_t *skb, *next;
	uint64_t f;

	spin_lock_irqsave(&s->lock, &f);
	skb = __skb_dequeue(&s->err_queue);
	if (skb) {
		BUG_ON(s->rx_mem < skb->truesize);
		s->rx_mem -= skb->truesize;
		/* The pending error becomes the next queued one's, or none:
		 * the one just read has been reported. */
		next = skb_peek(&s->err_queue);
		s->error = next ? (int)SKB_EXT_ERR(next)->ee.ee_errno : 0;
	}
	dgram_update_ready_locked(s);
	spin_unlock_irqrestore(&s->lock, f);
	return skb;
}

void dgram_purge(net_socket_t *s, int errq_only)
{
	sk_buff_t *a = NULL, *b, *n;
	uint32_t cnt;
	uint64_t f;

	spin_lock_irqsave(&s->lock, &f);
	if (!errq_only)
		a = skb_queue_take_all(&s->rx_queue, &cnt);
	b = skb_queue_take_all(&s->err_queue, &cnt);
	for (sk_buff_t *x = a; x; x = x->next)
		s->rx_mem -= x->truesize;
	for (sk_buff_t *x = b; x; x = x->next)
		s->rx_mem -= x->truesize;
	if (!errq_only)
		WARN_ON_ONCE(s->rx_mem != 0);
	dgram_update_ready_locked(s);
	spin_unlock_irqrestore(&s->lock, f);
	for (; a; a = n) {
		n = a->next;
		a->next = a->prev = NULL;
		skb_put(a);
	}
	for (; b; b = n) {
		n = b->next;
		b->next = b->prev = NULL;
		skb_put(b);
	}
}

void ip_queue_error(net_socket_t *s, int err, uint8_t origin, uint8_t type,
		    uint8_t code, uint32_t info, uint32_t offender,
		    uint32_t daddr, uint16_t dport, const uint8_t *payload,
		    uint32_t plen, int nowake)
{
	sk_buff_t *skb = NULL;
	struct sock_exterr_cb *ec;
	uint64_t f;
	int queued = 0;

	if (s->ip_recverr) {
		if (plen > 576)
			plen = 576;
		skb = skb_alloc(plen);
		if (skb) {
			if (plen)
				mm_memcpy(skb_append(skb, plen), payload, plen);
			ec = SKB_EXT_ERR(skb);
			mm_memset(ec, 0, sizeof(*ec));
			ec->ee.ee_errno = (uint32_t)err;
			ec->ee.ee_origin = origin;
			ec->ee.ee_type = type;
			ec->ee.ee_code = code;
			ec->ee.ee_info = info;
			ec->offender = offender;
			ec->addr = daddr;
			ec->port = dport;
		}
	}
	spin_lock_irqsave(&s->lock, &f);
	if (!s->active || s->closed) {
		spin_unlock_irqrestore(&s->lock, f);
		skb_put(skb);
		return;
	}
	if (skb && s->ip_recverr &&
	    s->rx_mem + skb->truesize < (uint32_t)s->rcvbuf_size) {
		s->rx_mem += skb->truesize;
		__skb_queue_tail(&s->err_queue, skb);
		queued = 1;
	}
	s->error = err;
	s->udp_rx_ready = 1;
	spin_unlock_irqrestore(&s->lock, f);
	if (!queued)
		skb_put(skb);
	if (!nowake)
		dgram_wake(s);
}
