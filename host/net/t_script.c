/* Script mode of the network harness: a small packetdrill-like language
 * that injects crafted segments into instance 0 and expects exact
 * outgoing segments at given simulated times.  One test per file in
 * .pkt file in host/net/scripts (or --script FILE).  The format is described in
 * host/net/README.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "tests.h"
#include "pkt.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>

#define MAXQ 256
struct outq {
	simtime_t t;
	unsigned len;
	uint8_t f[1600];
};

static struct outq *q;
static int qn;

static int script_tap(int inst, const uint8_t *f, unsigned len, void *ctx)
{
	(void)ctx;
	if (inst != 0)
		return 0;
	if (pkt_proto(f, len) != 6)
		return 1; /* ARP & co: not part of the dialogue */
	if (qn < MAXQ && len <= sizeof(q[0].f)) {
		q[qn].t = now_ns();
		q[qn].len = len;
		memcpy(q[qn].f, f, len);
		qn++;
	}
	return 1;
}

static void q_pop(void)
{
	memmove(q, q + 1, (size_t)(qn - 1) * sizeof(*q));
	qn--;
}

/* ---- script state ---- */
struct sock_name {
	char name[16];
	int s;
};

struct sstate {
	const char *file;
	int line;
	simtime_t t0, last;
	double tol_ms;
	uint32_t risn; /* the script's (remote) ISN */
	uint32_t lisn; /* the stack's ISN, learned */
	int have_lisn;
	uint16_t lport, rport;
	uint32_t last_tsval; /* last TSval the stack sent */
	simtime_t last_tsval_t; /* and when */
	int have_tsval;
	struct sock_name socks[32];
	int nsocks;
	uint8_t lmac[6], rmac[6];
};

static int fail_line(struct sstate *st, const char *fmt, const char *a)
{
	printf("    %s:%d: ", st->file, st->line);
	printf(fmt, a);
	printf("\n");
	return 1;
}

static int sock_lookup(struct sstate *st, const char *n)
{
	for (int i = 0; i < st->nsocks; i++)
		if (!strcmp(st->socks[i].name, n))
			return st->socks[i].s;
	return -1;
}

static void sock_set(struct sstate *st, const char *n, int s)
{
	for (int i = 0; i < st->nsocks; i++)
		if (!strcmp(st->socks[i].name, n)) {
			st->socks[i].s = s;
			return;
		}
	if (st->nsocks < 32) {
		snprintf(st->socks[st->nsocks].name, 16, "%s", n);
		st->socks[st->nsocks++].s = s;
	}
}

static const struct {
	const char *n;
	int v;
} errnames[] = { { "EAGAIN", EAGAIN },
		 { "EINPROGRESS", EINPROGRESS },
		 { "ECONNREFUSED", ECONNREFUSED },
		 { "ECONNRESET", ECONNRESET },
		 { "EPIPE", EPIPE },
		 { "ENOTCONN", ENOTCONN },
		 { "ETIMEDOUT", ETIMEDOUT },
		 { "EISCONN", EISCONN },
		 { "EALREADY", EALREADY },
		 { "EINVAL", EINVAL },
		 { "EBADF", EBADF },
		 { "EADDRINUSE", EADDRINUSE },
		 { NULL, 0 } };

static int parse_ret(const char *s, long *out)
{
	int neg = 0;

	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (isdigit((unsigned char)*s)) {
		*out = strtol(s, NULL, 0) * (neg ? -1 : 1);
		return 0;
	}
	for (int i = 0; errnames[i].n; i++)
		if (!strcmp(errnames[i].n, s)) {
			*out = neg ? -errnames[i].v : errnames[i].v;
			return 0;
		}
	return -1;
}

static const char *state_names[] = { "CLOSED",	    "LISTEN",
				     "SYN_SENT",    "SYN_RECEIVED",
				     "ESTABLISHED", "FIN_WAIT_1",
				     "FIN_WAIT_2",  "CLOSE_WAIT",
				     "CLOSING",	    "LAST_ACK",
				     "TIME_WAIT" };

/* ---- packet lines ---- */
struct pspec {
	uint8_t flags;
	int seq_any;
	uint32_t seq, end;
	int has_ack, ack_any;
	uint32_t ack;
	int has_win, win_any;
	uint32_t win;
	int has_opts;
	int mss, mss_any, ws, ws_any, sackok, ts, tsval_any, tsecr_any,
		tsecr_echo, tsval_cont;
	uint32_t tsval, tsecr;
	int nsack;
	uint32_t sack[4][2];
	int bad_csum;
	uint32_t urg;
};

static int parse_num(const char **p, uint32_t *v, int *any)
{
	while (**p == ' ')
		(*p)++;
	if (**p == '*') {
		(*p)++;
		if (any)
			*any = 1;
		return 0;
	}
	if (!isdigit((unsigned char)**p))
		return -1;
	char *e;
	*v = (uint32_t)strtoul(*p, &e, 0);
	*p = e;
	return 0;
}

static int word(const char **p, char *w, int cap)
{
	int n = 0;

	while (**p == ' ' || **p == ',')
		(*p)++;
	while (**p && **p != ' ' && **p != ',' && **p != '>' && n < cap - 1)
		w[n++] = *(*p)++;
	w[n] = 0;
	return n;
}

static int parse_opts(const char *p, struct pspec *ps)
{
	char w[32];

	ps->has_opts = 1;
	ps->mss = -1;
	ps->ws = -1;
	while (*p && *p != '>') {
		if (!word(&p, w, sizeof(w)))
			break;
		if (!strcmp(w, "nop") || !strcmp(w, "eol"))
			continue;
		if (!strcmp(w, "mss")) {
			uint32_t v = 0;
			if (parse_num(&p, &v, &ps->mss_any))
				return -1;
			ps->mss = (int)v;
		} else if (!strcmp(w, "wscale")) {
			uint32_t v = 0;
			if (parse_num(&p, &v, &ps->ws_any))
				return -1;
			ps->ws = (int)v;
		} else if (!strcmp(w, "sackOK")) {
			ps->sackok = 1;
		} else if (!strcmp(w, "TS")) {
			ps->ts = 1;
			if (!word(&p, w, sizeof(w)) || strcmp(w, "val"))
				return -1;
			while (*p == ' ')
				p++;
			if (*p == '~') {
				ps->tsval_cont = 1;
				p++;
			} else if (parse_num(&p, &ps->tsval, &ps->tsval_any))
				return -1;
			if (!word(&p, w, sizeof(w)) || strcmp(w, "ecr"))
				return -1;
			while (*p == ' ')
				p++;
			if (!strncmp(p, "echo", 4)) {
				ps->tsecr_echo = 1;
				p += 4;
			} else if (parse_num(&p, &ps->tsecr, &ps->tsecr_any))
				return -1;
		} else if (!strcmp(w, "sack")) {
			while (ps->nsack < 4) {
				const char *save = p;
				uint32_t a, b;
				if (parse_num(&p, &a, NULL) || *p != ':') {
					p = save;
					break;
				}
				p++;
				if (parse_num(&p, &b, NULL))
					return -1;
				ps->sack[ps->nsack][0] = a;
				ps->sack[ps->nsack][1] = b;
				ps->nsack++;
			}
		} else {
			return -1;
		}
	}
	return 0;
}

/* FLAGS seq:end(len) [ack N] [win N] [urg N] [csum bad] [<opts>] */
static int parse_packet(const char *p, struct pspec *ps)
{
	char w[32];

	memset(ps, 0, sizeof(*ps));
	ps->mss = ps->ws = -1;
	if (!word(&p, w, sizeof(w)))
		return -1;
	for (const char *c = w; *c; c++) {
		switch (*c) {
		case 'S': ps->flags |= F_SYN; break;
		case 'F': ps->flags |= F_FIN; break;
		case 'R': ps->flags |= F_RST; break;
		case 'P': ps->flags |= F_PSH; break;
		case 'U': ps->flags |= F_URG; break;
		case '.': ps->flags |= F_ACK; break;
		default: return -1;
		}
	}
	while (*p == ' ')
		p++;
	if (*p == '*') {
		ps->seq_any = 1;
		p++;
	} else {
		if (parse_num(&p, &ps->seq, NULL) || *p != ':')
			return -1;
		p++;
		if (parse_num(&p, &ps->end, NULL))
			return -1;
		if (*p == '(') {
			uint32_t l;
			p++;
			if (parse_num(&p, &l, NULL) || *p != ')' ||
			    l != ps->end - ps->seq)
				return -1;
			p++;
		}
	}
	for (;;) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (*p == '<')
			return parse_opts(p + 1, ps);
		if (!word(&p, w, sizeof(w)))
			break;
		if (!strcmp(w, "ack")) {
			ps->has_ack = 1;
			if (parse_num(&p, &ps->ack, &ps->ack_any))
				return -1;
		} else if (!strcmp(w, "win")) {
			ps->has_win = 1;
			if (parse_num(&p, &ps->win, &ps->win_any))
				return -1;
		} else if (!strcmp(w, "urg")) {
			if (parse_num(&p, &ps->urg, NULL))
				return -1;
		} else if (!strcmp(w, "csum")) {
			word(&p, w, sizeof(w));
			ps->bad_csum = 1;
		} else {
			return -1;
		}
	}
	return 0;
}

static void run_to(simtime_t t)
{
	if (t > now_ns())
		sim_run_until_time(t);
}

static int no_unexpected(struct sstate *st)
{
	if (!qn)
		return 0;
	struct tcpseg s;
	char fl[8], buf[256];
	pkt_parse_tcp(q[0].f, q[0].len, &s);
	pkt_flags_str(s.flags, fl);
	snprintf(buf, sizeof(buf),
		 "%s seq %u ack %u len %u win %u at %.4f",
		 fl, s.seq - (st->have_lisn ? st->lisn : 0), s.ack - st->risn,
		 s.len, s.win, (double)(q[0].t - st->t0) / 1e9);
	return fail_line(st, "unexpected outbound packet %s", buf);
}

static int do_inbound(struct sstate *st, const char *p)
{
	struct pspec ps;
	struct tcpseg s;
	static uint8_t data[1600];
	uint8_t f[1700];

	if (parse_packet(p, &ps) || ps.seq_any)
		return fail_line(st, "cannot parse inbound packet: %s", p);
	if (no_unexpected(st))
		return 1;
	tcpseg_init(&s);
	s.sip = inst_ip(1);
	s.dip = inst_ip(0);
	s.sport = st->rport;
	s.dport = st->lport;
	s.seq = st->risn + ps.seq;
	s.flags = ps.flags;
	s.ack = ps.has_ack ? (st->have_lisn ? st->lisn : 0) + ps.ack : 0;
	if (ps.has_ack && !(ps.flags & F_ACK))
		s.ack = (st->have_lisn ? st->lisn : 0) + ps.ack;
	s.win = (uint16_t)(ps.has_win ? ps.win : 65535);
	s.urg = (uint16_t)ps.urg;
	if (ps.has_opts) {
		s.mss = ps.mss;
		s.ws = ps.ws;
		s.sackok = ps.sackok;
		s.ts = ps.ts;
		s.tsval = ps.tsval;
		s.tsecr = ps.tsecr_echo ? st->last_tsval : ps.tsecr;
		s.nsack = ps.nsack;
		for (int i = 0; i < ps.nsack; i++) {
			s.sack[i][0] = st->lisn + ps.sack[i][0];
			s.sack[i][1] = st->lisn + ps.sack[i][1];
		}
	}
	s.len = ps.end - ps.seq;
	if (s.len > 1400)
		return fail_line(st, "inbound payload too long: %s", p);
	pat_fill(7, ps.seq, data, s.len);
	s.data = data;
	unsigned n = pkt_tcp(f, st->lmac, st->rmac, &s);
	if (ps.bad_csum)
		f[14 + 20 + 16] ^= 0x5a; /* TCP checksum field */
	NS[0]->rx(f, n);
	return 0;
}

static int do_outbound(struct sstate *st, simtime_t t, const char *p)
{
	struct pspec ps;
	struct tcpseg s;
	char fl[8], efl[8], buf[256];
	simtime_t tol = (simtime_t)(st->tol_ms * MS);

	if (parse_packet(p, &ps))
		return fail_line(st, "cannot parse outbound packet: %s", p);
	while (!qn && now_ns() <= t + tol)
		if (!sim_step())
			break;
	if (!qn) {
		return fail_line(st, "expected packet never sent: %s", p);
	}
	if (q[0].t + tol < t) {
		snprintf(buf, sizeof(buf), "%.4f s, expected %.4f: %s",
			 (double)(q[0].t - st->t0) / 1e9,
			 (double)(t - st->t0) / 1e9, p);
		return fail_line(st, "packet sent too early at %s", buf);
	}
	if (q[0].t > t + tol) {
		snprintf(buf, sizeof(buf), "%.4f s, expected %.4f: %s",
			 (double)(q[0].t - st->t0) / 1e9,
			 (double)(t - st->t0) / 1e9, p);
		return fail_line(st, "packet sent too late at %s", buf);
	}
	if (pkt_parse_tcp(q[0].f, q[0].len, &s))
		return fail_line(st, "unparsable outbound frame %s", "");
	if (!st->have_lisn && (s.flags & F_SYN)) {
		st->lisn = s.seq - ps.seq;
		st->have_lisn = 1;
		if (!st->lport)
			st->lport = s.sport;
	}
	int ts_jump = 0;
	if (s.ts && st->have_tsval) {
		/* a TSval clock runs at 1 ms .. 1 s per unit (RFC 7323) */
		int32_t d = (int32_t)(s.tsval - st->last_tsval);
		double ms = (double)(q[0].t - st->last_tsval_t) / 1e6;
		if (d < 0 || (double)d > ms + 50)
			ts_jump = 1;
	}
	uint32_t prev_tsval = st->last_tsval;
	if (s.ts) {
		st->last_tsval = s.tsval;
		st->last_tsval_t = q[0].t;
		st->have_tsval = 1;
	}
	pkt_flags_str(s.flags, fl);
	pkt_flags_str(ps.flags, efl);
	uint32_t rseq = s.seq - st->lisn, rack = s.ack - st->risn;
	snprintf(buf, sizeof(buf),
		 "got %s %u:%u(%u) ack %u win %u, expected: %s", fl, rseq,
		 rseq + s.len, s.len, rack, s.win, p);
	int bad = 0;
	if (s.flags != ps.flags)
		bad = 1;
	if (!ps.seq_any && (rseq != ps.seq || s.len != ps.end - ps.seq))
		bad = 1;
	if (ps.has_ack && !ps.ack_any && rack != ps.ack)
		bad = 1;
	if (ps.has_win && !ps.win_any && s.win != ps.win)
		bad = 1;
	if (s.sport != st->lport || s.dport != st->rport)
		bad = 1;
	if (!s.csum_ok) {
		q_pop();
		return fail_line(st, "bad TCP checksum on %s", buf);
	}
	if (bad) {
		q_pop();
		return fail_line(st, "packet mismatch: %s", buf);
	}
	if (ps.has_opts) {
		char ob[200];
		int obad = 0;
		if ((ps.mss >= 0) != (s.mss >= 0) ||
		    (ps.mss >= 0 && !ps.mss_any && ps.mss != s.mss))
			obad = 1;
		if ((ps.ws >= 0) != (s.ws >= 0) ||
		    (ps.ws >= 0 && !ps.ws_any && ps.ws != s.ws))
			obad = 1;
		if (ps.sackok != s.sackok || ps.ts != s.ts)
			obad = 1;
		if (ps.ts && s.ts && !ps.tsecr_any && s.tsecr != ps.tsecr)
			obad = 1;
		if (ps.ts && s.ts && ps.tsval_cont && ts_jump)
			obad = 1;
		if (ps.nsack != s.nsack)
			obad = 1;
		for (int i = 0; i < ps.nsack && i < s.nsack; i++)
			if (s.sack[i][0] - st->risn != ps.sack[i][0] ||
			    s.sack[i][1] - st->risn != ps.sack[i][1])
				obad = 1;
		if (obad) {
			int n = snprintf(ob, sizeof(ob), "got <");
			if (s.mss >= 0)
				n += snprintf(ob + n, sizeof(ob) - n, "mss %d,", s.mss);
			if (s.sackok)
				n += snprintf(ob + n, sizeof(ob) - n, "sackOK,");
			if (s.ts)
				n += snprintf(ob + n, sizeof(ob) - n,
					      "TS val %u (previous %u) ecr %u,",
					      s.tsval, prev_tsval, s.tsecr);
			if (s.ws >= 0)
				n += snprintf(ob + n, sizeof(ob) - n, "wscale %d,", s.ws);
			for (int i = 0; i < s.nsack; i++)
				n += snprintf(ob + n, sizeof(ob) - n, "sack %u:%u,",
					      s.sack[i][0] - st->risn,
					      s.sack[i][1] - st->risn);
			snprintf(ob + n, sizeof(ob) - n, "> for %s", p);
			q_pop();
			return fail_line(st, "options mismatch: %s", ob);
		}
	}
	q_pop();
	return 0;
}

static int do_call(struct sstate *st, char *p)
{
	char *argv[8];
	int argc = 0;
	long want = 0;
	int has_want = 0;
	char *eq = strstr(p, " = ");

	if (eq) {
		*eq = 0;
		if (parse_ret(eq + 3, &want))
			return fail_line(st, "bad return value %s", eq + 3);
		has_want = 1;
	}
	for (char *tok = strtok(p, " \t"); tok && argc < 8;
	     tok = strtok(NULL, " \t"))
		argv[argc++] = tok;
	if (!argc)
		return 0;
	if (no_unexpected(st))
		return 1;
	const struct ns_ops *o = NS[0];
	const char *op = argv[0];
	long r = 0;
	int s = argc > 1 ? sock_lookup(st, argv[1]) : -1;

	if (!strcmp(op, "socket") && argc >= 3) {
		r = o->socket(!strcmp(argv[2], "dgram") ? K_SOCK_DGRAM :
							  K_SOCK_STREAM);
		if (r >= 0) {
			o->set_nonblock((int)r, 1);
			sock_set(st, argv[1], (int)r);
			r = 0;
		}
	} else if (s < 0 && strcmp(op, "sleep")) {
		return fail_line(st, "unknown socket in: %s", op);
	} else if (!strcmp(op, "bind") && argc >= 3) {
		st->lport = (uint16_t)atoi(argv[2]);
		r = o->bind(s, 0, st->lport);
	} else if (!strcmp(op, "listen")) {
		r = o->listen(s, argc >= 3 ? atoi(argv[2]) : 16);
	} else if (!strcmp(op, "accept") && argc >= 3) {
		r = o->accept(s, NULL, NULL);
		if (r >= 0) {
			o->set_nonblock((int)r, 1);
			sock_set(st, argv[2], (int)r);
			r = 0;
		}
	} else if (!strcmp(op, "connect") && argc >= 3) {
		st->rport = (uint16_t)atoi(argv[2]);
		r = o->connect(s, inst_ip(1), st->rport);
	} else if (!strcmp(op, "send") && argc >= 3) {
		unsigned long n = strtoul(argv[2], NULL, 0);
		uint8_t *b = malloc(n + 1);
		pat_fill(3, 0, b, n);
		r = o->send(s, b, n, argc >= 4 && !strcmp(argv[3], "oob") ?
					   K_MSG_OOB :
					   0);
		free(b);
	} else if (!strcmp(op, "recv") && argc >= 3) {
		unsigned long n = strtoul(argv[2], NULL, 0);
		uint8_t *b = malloc(n + 1);
		int fl = 0;

		for (int k = 3; k < argc; k++) {
			if (!strcmp(argv[k], "oob"))
				fl |= K_MSG_OOB;
			else if (!strcmp(argv[k], "peek"))
				fl |= K_MSG_PEEK;
		}
		r = o->recv(s, b, n, fl);
		free(b);
	} else if (!strcmp(op, "atmark") && argc >= 2) {
		int v = -1;

		r = o->ioctl(s, 0x8905 /* SIOCATMARK */, &v);
		if (r == 0)
			r = v;
	} else if (!strcmp(op, "poll") && argc >= 2) {
		r = o->poll(s, K_POLLIN | K_POLLPRI | K_POLLOUT);
	} else if (!strcmp(op, "close")) {
		r = o->close(s);
	} else if (!strcmp(op, "shutdown") && argc >= 3) {
		r = o->shutdown(s, !strcmp(argv[2], "rd") ? K_SHUT_RD :
				   !strcmp(argv[2], "wr") ? K_SHUT_WR :
							    K_SHUT_RDWR);
	} else if (!strcmp(op, "setsockopt") && argc >= 4) {
		int v = atoi(argv[3]), lvl = K_SOL_SOCKET, nm = -1;
		if (!strcmp(argv[2], "nodelay"))
			lvl = K_IPPROTO_TCP, nm = K_TCP_NODELAY;
		else if (!strcmp(argv[2], "cork"))
			lvl = K_IPPROTO_TCP, nm = K_TCP_CORK;
		else if (!strcmp(argv[2], "keepalive"))
			nm = K_SO_KEEPALIVE;
		else if (!strcmp(argv[2], "linger0")) {
			int lg[2] = { 1, 0 };
			r = o->setsockopt(s, K_SOL_SOCKET, K_SO_LINGER, lg,
					  sizeof(lg));
			goto done;
		}
		if (nm < 0)
			return fail_line(st, "unknown option %s", argv[2]);
		r = o->setsockopt(s, lvl, nm, &v, sizeof(v));
	} else if (!strcmp(op, "state") && argc >= 3) {
		int stt = o->tcp_state(s);
		const char *have = stt >= 0 && stt <= 10 ? state_names[stt] :
							   "none";
		if (strcmp(have, argv[2])) {
			char b[64];
			snprintf(b, sizeof(b), "%s, expected %s", have, argv[2]);
			return fail_line(st, "state %s", b);
		}
	} else if (!strcmp(op, "so_error") && argc >= 2) {
		r = so_error(0, s);
	} else {
		return fail_line(st, "unknown call %s", op);
	}
done:
	if (has_want && r != want) {
		char b[96];
		snprintf(b, sizeof(b), "%s returned %ld, expected %ld", op, r,
			 want);
		return fail_line(st, "%s", b);
	}
	if (!has_want && r < 0) {
		char b[96];
		snprintf(b, sizeof(b), "%s failed: %ld", op, r);
		return fail_line(st, "%s", b);
	}
	return 0;
}

static int run_script(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	struct sstate st;

	if (!f) {
		printf("    cannot open %s\n", path);
		return 1;
	}
	memset(&st, 0, sizeof(st));
	st.file = path;
	st.tol_ms = 10;
	st.risn = 1000000;
	st.rport = 40000;
	inst_mac(0, st.lmac);
	inst_mac(1, st.rmac);
	st.t0 = now_ns();
	st.last = st.t0;
	q = calloc(MAXQ, sizeof(*q));
	qn = 0;
	wire_set_tap(script_tap, NULL);
	NS[0]->arp_add(inst_ip(1), st.rmac);

	int rc = 0;
	while (!rc && fgets(line, sizeof(line), f)) {
		st.line++;
		char *p = line, *e = line + strlen(line);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
			*--e = 0;
		char *hash = strchr(p, '#');
		if (hash)
			*hash = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			continue;
		if (!strncmp(p, "xfail", 5) || !strncmp(p, "hz ", 3))
			continue; /* header, read by the collector */
		if (!strncmp(p, "tolerance ", 10)) {
			st.tol_ms = atof(p + 10);
			continue;
		}
		if (!strncmp(p, "remote_isn ", 11)) {
			st.risn = (uint32_t)strtoul(p + 11, NULL, 0);
			continue;
		}
		if (!strncmp(p, "remote_port ", 12)) {
			st.rport = (uint16_t)atoi(p + 12);
			continue;
		}
		/* time */
		char *end;
		double tv;
		simtime_t t;
		if (*p == '+') {
			tv = strtod(p + 1, &end);
			t = st.last + (simtime_t)(tv * 1e9 + 0.5);
		} else {
			tv = strtod(p, &end);
			if (end == p) {
				rc = fail_line(&st, "no time in: %s", p);
				break;
			}
			t = st.t0 + (simtime_t)(tv * 1e9 + 0.5);
		}
		st.last = t;
		p = end;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '<') {
			run_to(t);
			rc = do_inbound(&st, p + 1);
		} else if (*p == '>') {
			rc = do_outbound(&st, t, p + 1);
		} else {
			run_to(t);
			rc = do_call(&st, p);
		}
	}
	fclose(f);
	if (!rc) {
		/* nothing more may go out within the tolerance */
		run_to(st.last + (simtime_t)(st.tol_ms * MS));
		rc = no_unexpected(&st);
	}
	wire_set_tap(NULL, NULL);
	free(q);
	return rc;
}

static int t_script(void)
{
	return run_script(g_cur_test->arg);
}

const struct test tests_script[] = {
	{ 0 },
};

int script_tests_collect(const struct test **out, int max)
{
	static const char *dir = "host/net/scripts";
	struct dirent **ents = NULL;
	int n = 0, ne;

	if (g_opts.script) {
		struct test *t = calloc(1, sizeof(*t));
		t->name = g_opts.script;
		t->group = "script";
		t->fn = t_script;
		t->ninst = 1;
		t->arg = g_opts.script;
		if (max > 0)
			out[n++] = t;
		return n;
	}
	ne = scandir(dir, &ents, NULL, alphasort);
	for (int i = 0; i < ne; i++) {
		const char *nm = ents[i]->d_name;
		size_t l = strlen(nm);
		if (l > 4 && !strcmp(nm + l - 4, ".pkt") && n < max) {
			struct test *t = calloc(1, sizeof(*t));
			char *path = malloc(strlen(dir) + l + 2);
			char *name = malloc(l + 8);
			sprintf(path, "%s/%s", dir, nm);
			sprintf(name, "script:%.*s", (int)(l - 4), nm);
			t->name = name;
			t->group = "script";
			t->fn = t_script;
			t->ninst = 1;
			t->arg = path;
			FILE *f = fopen(path, "r");
			char line[512];
			while (f && fgets(line, sizeof(line), f)) {
				if (!strncmp(line, "xfail ", 6)) {
					line[strcspn(line, "\n")] = 0;
					t->xfail = strdup(line + 6);
				} else if (!strncmp(line, "hz ", 3)) {
					t->hz = (unsigned)atoi(line + 3);
				}
			}
			if (f)
				fclose(f);
			out[n++] = t;
		}
		free(ents[i]);
	}
	free(ents);
	return n;
}
