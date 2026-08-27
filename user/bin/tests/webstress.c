/*
 * webstress: drive the network stack the way WebKit's network process does,
 * and say WHERE the time goes.
 *
 * The browser stalls a few seconds at 10% on some navigations, and every
 * single-connection test (curl, souptest, snifftest) is fast.  What the
 * browser does that they do not: a dozen THREADS resolving names, opening
 * TCP connections and running TLS handshakes AT THE SAME TIME, while the UI
 * process forks children and fields SIGCHLD.  So this reproduces exactly
 * that, phase-timed:
 *
 *   - N worker threads, each looping over a host list:
 *       DNS   getaddrinfo()                      (kernel resolver, slots)
 *       TCP   g_socket_client without TLS       (kernel connect path)
 *       TLS   g_socket_client with TLS          (gnutls handshake on top)
 *       HTTP  soup_session_send_and_read()      (libsoup, h1/h2, the works)
 *     Each phase is timed alone; anything slower than SLOW_MS prints
 *     immediately with its phase name, host and thread id.
 *
 *   - Optionally (-f) a forker thread spawns short-lived children the whole
 *     time, so SIGCHLD keeps landing mid-computation in every thread: the
 *     browser's signal load.  An FPU sentinel thread computes known-answer
 *     floating point in a tight loop and screams if a result ever comes out
 *     wrong -- that is the signal-frame FPU preservation being tested from
 *     userspace.
 *
 * Reading the summary: the phase whose max/avg carries the stall names the
 * subsystem.  DNS slow -> kernel resolver; TCP slow -> connect path or ARP;
 * TLS slow -> handshake data path (the browser's "Broken pipe"/"Connection
 * reset" handshake errors would show here too, as FAILED lines); HTTP slow
 * with the rest fast -> libsoup/session layer above them all.
 */
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define SLOW_MS 500
#define PHASES 4
static const char *phase_name[PHASES] = { "DNS ", "TCP ", "TLS ", "HTTP" };

static const char *default_hosts[] = {
	"www.google.com", "www.heise.de",   "news.google.com",
	"edition.cnn.com", "www.wikipedia.org", "duckduckgo.com",
	"github.com",     "www.gentoo.org", "ubuntu.com",
	"www.freebsd.org",
};
#define NHOSTS ((int)(sizeof(default_hosts) / sizeof(default_hosts[0])))

typedef struct {
	long count[PHASES];
	long fail[PHASES];
	double total_ms[PHASES];
	double max_ms[PHASES];
} stats_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static stats_t g_stats;
static volatile int g_running = 1;
static volatile long g_fpu_errors = 0;
static volatile long g_sigchld_seen = 0;

static double now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

static void record(int phase, double ms, int ok, const char *host, int tid)
{
	pthread_mutex_lock(&g_lock);
	if (ok) {
		g_stats.count[phase]++;
		g_stats.total_ms[phase] += ms;
		if (ms > g_stats.max_ms[phase])
			g_stats.max_ms[phase] = ms;
	} else {
		g_stats.fail[phase]++;
	}
	pthread_mutex_unlock(&g_lock);
	if (ms > SLOW_MS)
		printf("  SLOW %s %7.0fms  t%02d %s%s\n", phase_name[phase], ms,
		       tid, host, ok ? "" : "  (FAILED)");
}

/* --- phase 0: bare name resolution ------------------------------------- */
static void run_dns(const char *host, int tid)
{
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	double t0 = now_ms();
	int rc = getaddrinfo(host, "443", &hints, &res);
	record(0, now_ms() - t0, rc == 0, host, tid);
	if (rc != 0 && g_running)
		printf("  FAIL DNS  t%02d %s: %s\n", tid, host,
		       gai_strerror(rc));
	if (res)
		freeaddrinfo(res);
}

/* --- phases 1 and 2: TCP alone, then TCP+TLS --------------------------- */
static void run_connect(const char *host, int tid, int tls)
{
	GSocketClient *c = g_socket_client_new();
	GError *e = NULL;
	if (tls)
		g_socket_client_set_tls(c, TRUE);
	g_socket_client_set_timeout(c, 20);
	double t0 = now_ms();
	GSocketConnection *conn = g_socket_client_connect_to_host(
		c, host, tls ? 443 : 80, NULL, &e);
	double ms = now_ms() - t0;
	record(tls ? 2 : 1, ms, conn != NULL, host, tid);
	if (!conn && g_running)
		printf("  FAIL %s  t%02d %s: %s\n", tls ? "TLS " : "TCP ", tid,
		       host, e ? e->message : "?");
	g_clear_error(&e);
	if (conn) {
		g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
		g_object_unref(conn);
	}
	g_object_unref(c);
}

/* --- phase 3: a whole HTTP request through libsoup --------------------- */
static void run_http(SoupSession *s, const char *host, int tid)
{
	char url[256];
	snprintf(url, sizeof(url), "https://%s/", host);
	SoupMessage *msg = soup_message_new("GET", url);
	if (!msg)
		return;
	GError *e = NULL;
	double t0 = now_ms();
	GBytes *body = soup_session_send_and_read(s, msg, NULL, &e);
	double ms = now_ms() - t0;
	record(3, ms, body != NULL, host, tid);
	if (!body && g_running)
		printf("  FAIL HTTP  t%02d %s: %s\n", tid, host,
		       e ? e->message : "?");
	g_clear_error(&e);
	if (body)
		g_bytes_unref(body);
	g_object_unref(msg);
}

typedef struct {
	int tid;
	int iterations;
} worker_arg_t;

static void *worker(void *p)
{
	worker_arg_t *a = p;
	/* One session per thread, exactly like the network process's
	 * per-context sessions.  20s timeout so a hang reports as a slow
	 * FAILED line rather than silence. */
	SoupSession *s = soup_session_new();
	soup_session_set_timeout(s, 20);
	for (int i = 0; i < a->iterations && g_running; i++) {
		const char *host =
			default_hosts[(a->tid + i) % NHOSTS];
		run_dns(host, a->tid);
		run_connect(host, a->tid, 0);
		run_connect(host, a->tid, 1);
		run_http(s, host, a->tid);
	}
	g_object_unref(s);
	return NULL;
}

/* --- the browser's signal load ----------------------------------------- */
static void on_sigchld(int sig)
{
	(void)sig;
	g_sigchld_seen++;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static void *forker(void *p)
{
	(void)p;
	while (g_running) {
		pid_t pid = fork();
		if (pid == 0)
			_exit(0);
		usleep(20000); /* ~50 SIGCHLDs a second across the process */
	}
	return NULL;
}

/* Known-answer floating point, forever.  If a signal handler tramples the
 * XMM registers of interrupted code, this is the thread that notices. */
static void *fpu_sentinel(void *p)
{
	(void)p;
	while (g_running) {
		volatile double a = 1.5, b = 2.25, acc = 0.0;
		for (int i = 0; i < 1000; i++)
			acc += a * b; /* 3.375 each */
		if (acc < 3374.999 || acc > 3375.001) {
			g_fpu_errors++;
			printf("  FPU CORRUPTION: acc=%.6f (expected 3375)\n",
			       acc);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int threads = 8, iterations = 5, with_forker = 0;
	int opt;
	while ((opt = getopt(argc, argv, "t:i:f")) != -1) {
		if (opt == 't')
			threads = atoi(optarg);
		else if (opt == 'i')
			iterations = atoi(optarg);
		else if (opt == 'f')
			with_forker = 1;
		else {
			fprintf(stderr,
				"usage: %s [-t threads] [-i iterations] [-f]\n"
				"  -f  add a fork/SIGCHLD storm + FPU sentinel\n",
				argv[0]);
			return 2;
		}
	}
	if (threads < 1)
		threads = 1;
	if (threads > 32)
		threads = 32;

	printf("webstress: %d threads x %d iterations over %d hosts%s\n",
	       threads, iterations, NHOSTS,
	       with_forker ? ", with SIGCHLD storm" : "");
	printf("(phases slower than %dms print as they happen)\n\n", SLOW_MS);

	signal(SIGCHLD, on_sigchld);
	signal(SIGPIPE, SIG_IGN);

	pthread_t aux[2];
	int naux = 0;
	if (with_forker) {
		pthread_create(&aux[naux++], NULL, forker, NULL);
		pthread_create(&aux[naux++], NULL, fpu_sentinel, NULL);
	}

	double t0 = now_ms();
	pthread_t th[32];
	worker_arg_t args[32];
	for (int i = 0; i < threads; i++) {
		args[i].tid = i;
		args[i].iterations = iterations;
		pthread_create(&th[i], NULL, worker, &args[i]);
	}
	for (int i = 0; i < threads; i++)
		pthread_join(th[i], NULL);
	double wall = now_ms() - t0;

	g_running = 0;
	for (int i = 0; i < naux; i++)
		pthread_join(aux[i], NULL);

	printf("\n==== summary (%.1fs wall) ====\n", wall / 1000.0);
	printf("phase    ops  fail    avg ms    max ms\n");
	for (int i = 0; i < PHASES; i++) {
		double avg = g_stats.count[i] ?
				     g_stats.total_ms[i] / g_stats.count[i] :
				     0.0;
		printf("%s  %5ld %5ld  %8.1f  %8.1f\n", phase_name[i],
		       g_stats.count[i], g_stats.fail[i], avg,
		       g_stats.max_ms[i]);
	}
	if (with_forker)
		printf("SIGCHLD delivered: %ld   FPU errors: %ld\n",
		       g_sigchld_seen, g_fpu_errors);

	printf("\nReading it:\n"
	       "  DNS max high, rest sane   -> kernel resolver under parallel load\n"
	       "  TCP max high              -> connect path (SYN, ARP, accept of SYN-ACK)\n"
	       "  TLS max high, TCP sane    -> handshake data path; FAILED lines here\n"
	       "                               are the browser's 'Broken pipe'/'reset'\n"
	       "  HTTP max high, rest sane  -> libsoup/session layer\n"
	       "  FPU errors nonzero        -> signal frames still lose SSE state\n");
	return (g_fpu_errors || g_stats.fail[0] + g_stats.fail[1] +
					g_stats.fail[2] + g_stats.fail[3] >
				threads * iterations) ?
		       1 :
		       0;
}
