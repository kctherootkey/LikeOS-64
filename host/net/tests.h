/* Test registry of the network harness (host side).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_NET_TESTS_H
#define HOST_NET_TESTS_H

#include "sim.h"

struct test {
	const char *name;
	const char *group; /* unit, interop, script, fuzz, bench */
	int (*fn)(void); /* 0 pass */
	/* non-NULL: a known bug (expected to fail); the reason */
	const char *xfail;
	unsigned hz; /* 0: --hz (200) */
	int ninst; /* 0: both */
	unsigned mtu; /* 0: 1500 */
	simtime_t limit; /* simulated time limit, 0: 3600 s */
	int allow_warn; /* kernel WARNINGs do not fail the test */
	const char *arg; /* for the test function (g_cur_test->arg) */
};
extern const struct test *g_cur_test;

struct run_opts {
	unsigned long long seed;
	unsigned hz;
	const char *pcap_dir;
	unsigned long long iters;
	double secs;
	const char *out;
	unsigned timeout;
	const char *script;
};
extern struct run_opts g_opts;

/* every file's table, NULL-name terminated */
extern const struct test tests_unit[], tests_interop[], tests_script[],
	tests_fuzz[], tests_bench[], tests_core[], tests_ip[], tests_tcp[],
	tests_tcp2[];
int tests_collect(const struct test **out, int max);
/* script mode: one test per .pkt file in host/net/scripts */
int script_tests_collect(const struct test **out, int max);

#endif
