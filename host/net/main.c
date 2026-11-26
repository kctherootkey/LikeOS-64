/* The network harness's runner: runs each test in a forked child with a
 * fresh pair of stack instances and reports PASS / FAIL / XFAIL / XPASS.
 *
 *   test-net GROUP [FILTER] [--seed N] [--pcap DIR] [-v] [--list]
 *            [--iters N] [--secs T] [--out FILE]
 *
 * GROUP is unit, interop, script, fuzz, bench or all.  See host/net/README.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "sim.h"
#include "tests.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

struct run_opts g_opts;
const struct test *g_cur_test;

int tests_collect(const struct test **out, int max)
{
	const struct test *tabs[] = { tests_unit,   tests_core,	 tests_ip, tests_tcp,
				      tests_tcp2,
				      tests_interop, tests_script, tests_fuzz,
				      tests_bench };
	int n = 0;

	for (size_t i = 0; i < sizeof(tabs) / sizeof(tabs[0]); i++)
		for (const struct test *t = tabs[i]; t->name && n < max; t++)
			out[n++] = t;
	n += script_tests_collect(out + n, max - n);
	return n;
}

static int run_child(const struct test *t)
{
	struct sim_opts so = { 0 };
	unsigned long long w0[NINST];

	so.seed = g_opts.seed ? g_opts.seed : 1;
	so.hz = t->hz ? t->hz : g_opts.hz;
	so.ninst = t->ninst;
	so.mtu = t->mtu;
	so.limit = t->limit;
	if (g_opts.timeout)
		alarm(g_opts.timeout);
	sim_init(&so);
	if (g_opts.pcap_dir) {
		char p[1024];
		snprintf(p, sizeof(p), "%s/%s.pcap", g_opts.pcap_dir, t->name);
		pcap_open(p, getenv("TESTNET_PCAP_RX") != NULL);
	}
	int ni = so.ninst ? so.ninst : NINST;
	for (int i = 0; i < ni; i++)
		w0[i] = NS[i]->counter(NSC_WARNINGS);
	g_cur_test = t;
	int r = t->fn();
	if (r == 0 && !t->allow_warn) {
		for (int i = 0; i < ni; i++) {
			unsigned long long w = NS[i]->counter(NSC_WARNINGS) - w0[i];
			if (w) {
				printf("    instance %d printed %llu WARNING/BUG "
				       "line(s)\n",
				       i, w);
				r = 1;
			}
		}
	}
	pcap_close();
	if (r) {
		printf("    --- last kernel log lines ---\n");
		fflush(stdout);
		klog_dump_tail(stdout, 30);
	}
	fflush(stdout);
	fflush(stderr);
	return r ? 1 : 0;
}

static int matches(const struct test *t, const char *group, const char *filter)
{
	if (strcmp(group, "all") && strcmp(group, t->group))
		return 0;
	if (!strcmp(group, "all") && (!strcmp(t->group, "bench") ||
				      !strcmp(t->group, "fuzz")))
		return 0;
	return !filter || strstr(t->name, filter);
}

int main(int argc, char **argv)
{
	const char *group = NULL, *filter = NULL;
	int list = 0;

	g_opts.hz = 200;
	g_opts.timeout = 900;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--seed") && i + 1 < argc)
			g_opts.seed = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(a, "--hz") && i + 1 < argc)
			g_opts.hz = (unsigned)atoi(argv[++i]);
		else if (!strcmp(a, "--pcap") && i + 1 < argc)
			g_opts.pcap_dir = argv[++i];
		else if (!strcmp(a, "--iters") && i + 1 < argc)
			g_opts.iters = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(a, "--secs") && i + 1 < argc)
			g_opts.secs = atof(argv[++i]);
		else if (!strcmp(a, "--out") && i + 1 < argc)
			g_opts.out = argv[++i];
		else if (!strcmp(a, "--timeout") && i + 1 < argc)
			g_opts.timeout = (unsigned)atoi(argv[++i]);
		else if (!strcmp(a, "--script") && i + 1 < argc)
			g_opts.script = argv[++i];
		else if (!strcmp(a, "-v"))
			g_verbose = 1;
		else if (!strcmp(a, "--list"))
			list = 1;
		else if (!group)
			group = a;
		else if (!filter)
			filter = a;
		else {
			fprintf(stderr, "test-net: unexpected argument %s\n", a);
			return 2;
		}
	}
	if (!group)
		group = "all";
	if (g_opts.secs > 0 && g_opts.timeout < g_opts.secs + 600)
		g_opts.timeout = (unsigned)g_opts.secs + 600;
	if (getenv("TESTNET_VERBOSE"))
		g_verbose = 1;

	const struct test *all[1024];
	int n = tests_collect(all, 1024);
	int pass = 0, fail = 0, xfail = 0, xpass = 0, ran = 0;
	char failed[8192] = "";

	for (int i = 0; i < n; i++) {
		const struct test *t = all[i];
		if (!matches(t, group, filter))
			continue;
		if (list) {
			printf("%-8s %-40s%s%s\n", t->group, t->name,
			       t->xfail ? " xfail: " : "",
			       t->xfail ? t->xfail : "");
			continue;
		}
		ran++;
		fflush(stdout);
		fflush(stderr);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 2;
		}
		if (pid == 0)
			_exit(run_child(t));
		int st = 0;
		waitpid(pid, &st, 0);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double secs = (double)(t1.tv_sec - t0.tv_sec) +
			      (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
		int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
		char how[64] = "";
		if (WIFSIGNALED(st))
			snprintf(how, sizeof(how), ", killed by signal %d%s",
				 WTERMSIG(st),
				 WTERMSIG(st) == SIGALRM ? " (timeout)" : "");
		else if (!ok)
			snprintf(how, sizeof(how), ", status %d",
				 WEXITSTATUS(st));
		if (t->xfail) {
			if (ok) {
				xpass++;
				printf("XPASS %s (%.2f s) -- known bug fixed? %s\n",
				       t->name, secs, t->xfail);
			} else {
				xfail++;
				printf("XFAIL %s (%.2f s%s): %s\n", t->name,
				       secs, how, t->xfail);
			}
		} else if (ok) {
			pass++;
			printf("PASS %s (%.2f s)\n", t->name, secs);
		} else {
			fail++;
			printf("FAIL %s (%.2f s%s)\n", t->name, secs, how);
			if (strlen(failed) + strlen(t->name) + 2 < sizeof(failed)) {
				strcat(failed, " ");
				strcat(failed, t->name);
			}
		}
		fflush(stdout);
	}
	if (list)
		return 0;
	if (!ran) {
		fprintf(stderr, "test-net: no test matches %s %s\n", group,
			filter ? filter : "");
		return 2;
	}
	printf("test-net: %d passed, %d failed", pass, fail);
	if (xfail || xpass)
		printf(" (%d expected failures, %d unexpected passes)", xfail,
		       xpass);
	printf("\n");
	if (fail)
		printf("failed:%s\n", failed);
	return fail ? 1 : 0;
}
