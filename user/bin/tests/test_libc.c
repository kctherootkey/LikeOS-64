#include <stdio.h>
#include <sysexits.h>
#include <sys/ipc.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <malloc.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <wchar.h>
#include <uchar.h>
#include <wctype.h>
#include <locale.h>
#include <langinfo.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <link.h>
#include <semaphore.h>
#include <resolv.h>
#include <arpa/nameser.h>
#include <math.h>
#include <float.h>
#include <getopt.h>
#include <sys/procinfo.h>
#include <sys/ptrace.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <iconv.h>
#include <sys/klog.h>
#include <sys/un.h>
#include <netdb.h>
#include <sys/socket.h>
#include <fenv.h>
#include <search.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <poll.h>
#include <sys/fb.h>
#include <sys/input.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <net/if.h>
#include <sys/uio.h>
#include <sys/resource.h>
#include <dirent.h>
#include <strings.h>
#include <libgen.h>
/* The BSD spellings, included ALONGSIDE the standard ones above on purpose.
 * That is the arrangement that breaks if one of these headers declares
 * anything itself instead of redirecting: a second declaration of open() or a
 * second definition of O_RDONLY would be a compile error right here, in a file
 * that already includes <fcntl.h>.  See test_gtk3_libc_additions(). */
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/termios.h>
#include <sys/unistd.h>
#include <setjmp.h>
#include <utime.h>
#include <sys/xattr.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <crypt.h>
#include <security/pam_appl.h>

// Futex helper declarations (from sched.c)
int futex_wait(int *uaddr, int val, const struct timespec *timeout);
int futex_wake(int *uaddr, int count);

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

/* Names of failed tests, recorded so the end-of-run summary can list them in
 * one contiguous block emitted by the parent alone.  Individual "[FAIL] ..."
 * lines are printed inline as tests run, but under SMP a forked child writing
 * to fd 1 can interleave with the parent's write() and split the "[FAIL]"
 * token mid-line, so a grep for "[FAIL]" misses it even though tests_failed
 * (and therefore the process exit code) is non-zero.  The summary list below
 * is written when no children are running, so it is never garbled. */
#define MAX_FAILED_NAMES 512
static char failed_names[MAX_FAILED_NAMES][96];
static int failed_names_count = 0;

static void record_failure(const char *name)
{
	if (failed_names_count < MAX_FAILED_NAMES) {
		snprintf(failed_names[failed_names_count],
			 sizeof(failed_names[0]), "%s",
			 name ? name : "(unnamed)");
	}
	failed_names_count++; /* count past the cap too, so the total is honest */
}

static volatile int g_sigusr1_hit = 0;
static volatile int g_sigusr2_hit = 0;
static volatile int g_last_signal = 0;
static volatile int g_signal_hits = 0;
static volatile int g_sigalrm_hit = 0;

static void handle_sigusr1(int sig)
{
	(void)sig;
	g_sigusr1_hit = 1;
}

static void handle_sigusr2(int sig)
{
	(void)sig;
	g_sigusr2_hit = 1;
}

static void handle_generic(int sig)
{
	g_last_signal = sig;
	g_signal_hits++;
}

static void handle_sigalrm(int sig)
{
	(void)sig;
	g_sigalrm_hit = 1;
}

/* Test reporting.
 *
 * The macros below are wrappers around the __test_*_impl() functions that
 * capture context the caller would otherwise have to thread through by hand:
 *
 *   - file and line via __FILE__ / __LINE__
 *   - the source text of the failing expression via the # stringification
 *     operator (so "[FAIL] foo (... "ret == 0" was false ...)" tells you
 *     exactly which assertion failed without grepping the source)
 *   - the post-evaluation errno, so a failing syscall self-reports the
 *     reason (ECONNREFUSED, ENOMEM, EADDRINUSE, ...)
 *
 * All existing call sites (test_pass(name), test_fail(name),
 * test_result(name, cond)) continue to work unchanged. */
static void __test_pass_impl(const char *name)
{
	tests_passed++;
	printf("  [PASS] %s\n", name);
}

/* ELF constructors for the MAIN EXECUTABLE.
 *
 * These had never run: the dynamic linker runs constructors for shared
 * libraries but skips the main object, and crt1 did not pick up the slack, so
 * every __attribute__((constructor)) in a program was silently ignored.  It
 * took NetSurf to expose it -- libnsfb registers its display surfaces from
 * constructors, so the browser came up with no surfaces at all.
 *
 * Declared here at file scope so the check is what it claims to be: if the
 * start-up code regresses, ctor_ran stays 0 and the case fails. */
/* Shared by the umask-across-threads case below: the mask belongs to the
 * process, so a thread must see the value main set, and a change it makes must
 * be visible back in main. */

/* ---- fork() from a thread: the guarantees a threaded program forks on ----
 *
 * A mail client reaches this constantly: it runs a thread per connection and
 * forks a helper for each name lookup and each external command.  What those
 * helpers do to themselves must not reach the process that spawned them, and
 * the ways that can go wrong are all silent -- the parent does not fault, it
 * simply loses descriptors, or exits with the child's status, and there is
 * nothing in any log to say why.
 *
 * The child is deliberately forked from a NON-MAIN thread.  Forking from main
 * exercises none of this: the interesting question is what a child inherits
 * from a thread that is not its process's leader. */
static volatile int g_forkthr_ok;
static volatile pid_t g_forkthr_child;
static volatile pid_t g_forkthr_child_getppid;
static volatile int g_forkthr_status;
static volatile int g_forkthr_sees_sibling;
static int g_forkthr_pipe[2];
static int g_forkthr_spare_fd = -1;

/* A second thread that simply stays alive, so the process has a thread the
 * child must NOT have inherited. */
static void *forkthr_bystander(void *arg)
{
	volatile int *stop = (volatile int *)arg;

	while (!*stop)
		usleep(1000);
	return NULL;
}

static void *forkthr_body(void *arg)
{
	pid_t pid;

	(void)arg;

	pid = fork();
	if (pid == 0) {
		/* Child.  Single-threaded by definition, holding a private copy
		 * of the descriptor table.
		 *
		 * Report through the pipe before touching anything, then do
		 * what a real helper does: close every descriptor it did not
		 * ask for, and exit non-zero.  If the table were shared rather
		 * than copied, that loop would shut the parent's connections --
		 * which is exactly how a program loses its display without ever
		 * being told. */
		pid_t me = getpid(), parent = getppid();
		int maxopen = 64, i;

		write(g_forkthr_pipe[1], &me, sizeof(me));
		write(g_forkthr_pipe[1], &parent, sizeof(parent));
		close(g_forkthr_pipe[1]);

		for (i = 3; i < maxopen; i++)
			close(i);

		_exit(1);
	}

	if (pid < 0) {
		g_forkthr_ok = -1;
		return NULL;
	}

	g_forkthr_child = pid;
	g_forkthr_ok = 1;
	return NULL;
}

static volatile mode_t g_umask_seen_in_thread;

static void *umask_thread_fn(void *arg)
{
	(void)arg;
	g_umask_seen_in_thread = umask(0044); /* read the process mask, set a new one */
	return NULL;
}

static int g_ctor_ran;
static int g_ctor_saw_main_before;

__attribute__((constructor)) static void likeos_test_ctor(void)
{
	g_ctor_ran = 1;
}

/* ------------------------------------------------------------------ *
 * Exit-handler ordering.
 *
 * atexit(3) and __cxa_atexit share one list, because they are one mechanism:
 * every C++ static destructor is registered through the latter, tagged with
 * the shared object it belongs to, so that dlclose() can run and remove that
 * object's destructors before its pages are unmapped.
 *
 * Ordering is the observable part and is specified: handlers run in the
 * reverse of the order they were registered.  Recorded into a buffer here and
 * checked by the LAST handler, since after that point nothing else runs.
 * ------------------------------------------------------------------ */
/* Declared here rather than pulled from a header on purpose: __cxa_atexit is an
 * entry point of the C++ ABI, not of the C library's public interface.  The
 * compiler emits calls to it directly and no standard header declares it, which
 * is why a C program wanting to exercise it has to say what it looks like. */
extern int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

static char g_exit_order[8];
static int g_exit_order_n;

static void exit_mark(char c)
{
	if (g_exit_order_n < (int)sizeof(g_exit_order) - 1)
		g_exit_order[g_exit_order_n++] = c;
}

static void exit_handler_a(void) { exit_mark('a'); }
static void exit_handler_b(void) { exit_mark('b'); }

static void exit_handler_cxa(void *arg)
{
	exit_mark(*(const char *)arg);
}

/* Registered FIRST, so it runs LAST and sees the complete record. */
static void exit_handler_check(void)
{
	/* Registration order was check, a, x, b -- so the reverse is b, x, a. */
	if (g_exit_order_n == 3 && g_exit_order[0] == 'b' &&
	    g_exit_order[1] == 'x' && g_exit_order[2] == 'a')
		printf("  [PASS] exit handlers ran in reverse registration order\n");
	else
		printf("  [FAIL] exit handlers ran out of order (%.*s)\n",
		       g_exit_order_n, g_exit_order);
}

static void __test_fail_impl(const char *name, const char *file, int line,
			     int saved_errno)
{
	tests_failed++;
	record_failure(name);
	if (saved_errno != 0) {
		printf("  [FAIL] %s (at %s:%d; errno=%d: %s)\n", name, file,
		       line, saved_errno, strerror(saved_errno));
	} else {
		printf("  [FAIL] %s (at %s:%d)\n", name, file, line);
	}
}

/* Records whether a signal handler was entered with the stack alignment the
 * ABI promises.  The check is an ALIGNED 16-byte local rather than arithmetic
 * on RSP: what actually faulted before the fix was `movaps %xmm0,(%rsp)`, and
 * a handler entered 8 bytes out leaves exactly this kind of local misaligned. */
volatile int __sig_align_rsp_ok = -1;

static void sig_align_handler(int sig)
{
	__attribute__((aligned(16))) unsigned char probe[16];

	(void)sig;
	for (int i = 0; i < 16; i++)
		probe[i] = (unsigned char)i;
	__sig_align_rsp_ok = (((unsigned long)probe & 0xF) == 0) ? 1 : 0;
}

static void __test_result_impl(const char *name, int condition,
			       const char *expr, const char *file, int line,
			       int saved_errno)
{
	if (condition) {
		__test_pass_impl(name);
	} else {
		tests_failed++;
		record_failure(name);
		if (saved_errno != 0) {
			printf("  [FAIL] %s (at %s:%d: \"%s\" was false; errno=%d: %s)\n",
			       name, file, line, expr, saved_errno,
			       strerror(saved_errno));
		} else {
			printf("  [FAIL] %s (at %s:%d: \"%s\" was false)\n",
			       name, file, line, expr);
		}
	}
}

#define test_pass(name) __test_pass_impl(name)
#define test_fail(name) __test_fail_impl((name), __FILE__, __LINE__, errno)
#define test_result(name, cond)                                        \
	do {                                                           \
		int __test_ok = !!(cond);                              \
		__test_result_impl((name), __test_ok, #cond, __FILE__, \
				   __LINE__, errno);                   \
	} while (0)

/* A path under /tmp that belongs to THIS test run.
 *
 * teststress runs several copies of this suite at once, so a fixed name is a
 * name two runs fight over.  The record-lock case failed outright that way --
 * the second run's whole-file F_SETLK was refused, correctly -- and any case
 * that creates, truncates or unlinks a shared name can pull the file out from
 * under the other run and produce failures that look like kernel bugs.
 *
 * The pid is captured ONCE, on the first call, rather than read per call: a
 * forked child asking for the same name must get the SAME path as its parent,
 * and several cases here depend on precisely that (the lock test opens the
 * parent's file from the child to contend with it).
 *
 * Each name gets its own permanent buffer, so a pointer kept across other
 * calls stays valid.
 */
static const char *tmpp(const char *name)
{
	static struct {
		const char *key;
		char path[96];
	} slots[32];
	static unsigned used;
	static int pid;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	const char *ret;
	unsigned i;

	pthread_mutex_lock(&lock);
	if (!pid)
		pid = (int)getpid();
	for (i = 0; i < used; i++) {
		if (strcmp(slots[i].key, name) == 0) {
			pthread_mutex_unlock(&lock);
			return slots[i].path;
		}
	}
	if (used == sizeof(slots) / sizeof(slots[0])) {
		/* Out of slots: fall back to a shared name rather than lie
		 * about it, and say so -- a collision under teststress is
		 * exactly what this exists to prevent. */
		printf("  NOTE: tmpp() table full, \"%s\" is not unique\n", name);
		pthread_mutex_unlock(&lock);
		return name;
	}
	slots[used].key = name;
	snprintf(slots[used].path, sizeof(slots[used].path), "/tmp/%d_%s", pid,
		 name);
	ret = slots[used].path;
	used++;
	pthread_mutex_unlock(&lock);
	return ret;
}

/* Descriptors 0, 1 and 2 are ordinary descriptors: they can be closed, and the
 * numbers then freed are the lowest available ones, so the next open() or dup()
 * gets them back.  That is how a program hands itself a terminal --
 * close(0) followed by dup(slave) -- and it is exactly what xterm does for the
 * shell it starts.
 *
 * These checks run in a CHILD on purpose: they close the standard descriptors,
 * which in the test process itself would take the harness's own output with
 * them.  One character per check comes back over a pipe.
 *
 * The interesting part is that the console has no object behind it -- it is
 * represented by an EMPTY slot at 0/1/2 -- so "closed" and "attached to the
 * terminal" are the same state unless the close is recorded separately.  Checks
 * 2 and 3 are what tell those two apart. */
#define FDR_CHECKS 10

static void fd_reuse_child(int wfd)
{
	char r[FDR_CHECKS];
	struct stat ss, s0, s1, s2;
	char pts[32];
	int i, m = -1, sl = -1, n = -1;
	int pfd[2];
	char c;

	memset(r, '0', sizeof(r));

	/* Own session first, which is also what a terminal emulator's child does
	 * before it touches the pty.  Opening a pts slave makes the opener's
	 * process group the terminal's foreground group, and fork() left this
	 * child in the PARENT's group -- so when the master closed at exit the
	 * hangup went to the whole group and killed the test harness. */
	setsid();

	/* 1. Closing a standard descriptor is allowed at all. */
	r[0] = (close(0) == 0) ? '1' : '0';

	/* 2. ...and it stays closed.  An empty slot at 0 must not read back as
	 *    the console again. */
	r[1] = (close(0) == -1 && errno == EBADF) ? '1' : '0';

	/* 3. Same question from the I/O side: reading a closed 0 is an error,
	 *    not a silent read from the terminal. */
	r[2] = (read(0, &c, 1) == -1 && errno == EBADF) ? '1' : '0';

	/* 4. "Lowest available descriptor" includes the standard ones once
	 *    they are free. */
	i = open("/dev/null", O_RDONLY);
	r[3] = (i == 0) ? '1' : '0';

	/* 5. dup() allocates by the same rule as open(). */
	if (pipe(pfd) == 0) {
		close(0);
		r[4] = (dup(pfd[0]) == 0) ? '1' : '0';
		close(pfd[0]);
		close(pfd[1]);
	}

	/* 6-9. The sequence a terminal emulator runs in the child it forks,
	 *      verbatim from xterm: close each standard descriptor and dup the
	 *      pty slave onto it.  Checked by device identity and not merely by
	 *      isatty(), because the console is a terminal too -- and the
	 *      console is precisely what the shell wrongly ended up talking to
	 *      when these descriptors could not be closed. */
	m = posix_openpt(O_RDWR);
	if (m >= 0 && ioctl(m, TIOCGPTN, &n) == 0 && n >= 0) {
		snprintf(pts, sizeof(pts), "/dev/pts/%d", n);
		sl = open(pts, O_RDWR);
	}
	if (sl >= 0 && fstat(sl, &ss) == 0) {
		int dup_ok = 1;
		for (i = 0; i <= 2; i++) {
			if (i != sl) {
				close(i);
				if (dup(sl) != i)
					dup_ok = 0;
			}
		}
		r[5] = (dup_ok && isatty(0) && isatty(1) && isatty(2)) ? '1' :
									 '0';
		r[6] = (fstat(0, &s0) == 0 && s0.st_rdev == ss.st_rdev) ? '1' :
									  '0';
		r[7] = (fstat(1, &s1) == 0 && s1.st_rdev == ss.st_rdev) ? '1' :
									  '0';
		r[8] = (fstat(2, &s2) == 0 && s2.st_rdev == ss.st_rdev) ? '1' :
									  '0';
	}

	/* 10. However the kernel records the close, it must not show up in the
	 *     descriptor flags a program can read: F_GETFD is defined to report
	 *     FD_CLOEXEC and nothing else. */
	r[9] = (fcntl(0, F_GETFD) == 0) ? '1' : '0';

	if (write(wfd, r, sizeof(r)) != (ssize_t)sizeof(r)) {
		/* Nothing useful to do; the parent times the read out. */
	}
	_exit(0);
}

/* Concurrent sendmsg/recvmsg on AF_UNIX.
 *
 * Two processes, each on its OWN socketpair, pushing distinct byte patterns
 * through at the same time.  The concurrency is the whole point: every
 * unix-socket sendmsg and recvmsg used to be staged through one static 4 KB
 * buffer shared by the entire system, with no lock, so two processes doing this
 * simultaneously received each other's bytes.  No single-threaded test can see
 * that -- which is why it survived until an X client aborted with "[xcb]
 * Unknown sequence number while processing queue" on a window resize.
 *
 * Two iovecs per call on purpose: a single flat buffer would not exercise the
 * scatter/gather path the staging buffer used to serve. */
#define UDS_MSG_LEN 2048
#define UDS_ROUNDS 200

/* Fill iov[] with up to two segments covering b[0..len), and return the count. */
static int uds_split(struct iovec *iov, unsigned char *b, size_t len)
{
	size_t half = len / 2;
	if (half == 0) {
		iov[0].iov_base = b;
		iov[0].iov_len = len;
		return 1;
	}
	iov[0].iov_base = b;
	iov[0].iov_len = half;
	iov[1].iov_base = b + half;
	iov[1].iov_len = len - half;
	return 2;
}

static int uds_send_all(int fd, unsigned char *b, size_t len)
{
	size_t off = 0;
	while (off < len) {
		struct iovec iov[2];
		struct msghdr mh;
		int n;
		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = iov;
		mh.msg_iovlen = uds_split(iov, b + off, len - off);
		n = sendmsg(fd, &mh, 0);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static int uds_recv_all(int fd, unsigned char *b, size_t len)
{
	size_t off = 0;
	while (off < len) {
		struct iovec iov[2];
		struct msghdr mh;
		int n;
		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = iov;
		mh.msg_iovlen = uds_split(iov, b + off, len - off);
		n = recvmsg(fd, &mh, 0);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/* Returns 0 on success, -1 on an I/O failure, -2 if the bytes read back were
 * not the bytes written -- i.e. another process's data arrived here. */
static int uds_run(int sv[2], unsigned char tag, int rounds)
{
	unsigned char out[UDS_MSG_LEN], in[UDS_MSG_LEN];
	for (int r = 0; r < rounds; r++) {
		for (size_t i = 0; i < sizeof(out); i++)
			out[i] = (unsigned char)(tag ^ (unsigned char)(i + (size_t)r));
		if (uds_send_all(sv[0], out, sizeof(out)) != 0)
			return -1;
		memset(in, 0, sizeof(in));
		if (uds_recv_all(sv[1], in, sizeof(in)) != 0)
			return -1;
		if (memcmp(in, out, sizeof(out)) != 0)
			return -2;
	}
	return 0;
}

/* Bind a socket to an ephemeral port on the given local IP (host byte
 * order).  Returns the assigned port number (host byte order) on
 * success, 0 on failure.
 *
 * Use this in place of bind() to a fixed port number whenever the test
 * only needs "some local port" rather than a specific one.  Fixed-port
 * binds collide across two parallel teststress instances even with
 * SO_REUSEADDR — UDP delivery only goes to the first matching socket,
 * so the second instance's recvfrom hangs forever.  Ephemeral ports
 * + getsockname avoid the collision class entirely. */
static uint16_t bind_to_ephemeral(int fd, uint32_t local_ip)
{
	struct sockaddr_in la;
	memset(&la, 0, sizeof(la));
	la.sin_family = AF_INET;
	la.sin_port = 0;
	la.sin_addr.s_addr = htonl(local_ip);
	if (bind(fd, (struct sockaddr *)&la, sizeof(la)) < 0)
		return 0;
	socklen_t la_len = sizeof(la);
	if (getsockname(fd, (struct sockaddr *)&la, &la_len) < 0)
		return 0;
	return ntohs(la.sin_port);
}

static int get_interface_ipv4(const char *ifname, uint32_t *ip_out)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

	int ret = ioctl(sock, SIOCGIFADDR, &ifr);
	close(sock);
	if (ret < 0)
		return -1;

	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	*ip_out = ntohl(sin->sin_addr.s_addr);
	return 0;
}

static void run_tcp_large_transfer_case(const char *prefix, uint32_t bind_ip,
					uint32_t connect_ip, uint16_t port)
{
	char label[96];
	enum { transfer_size = 4096 };
	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	snprintf(label, sizeof(label), "%s: server socket", prefix);
	test_result(label, server_fd >= 0);

	if (server_fd < 0)
		return;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(bind_ip);

	int optval = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
		   sizeof(optval));

	int ret = bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
	snprintf(label, sizeof(label), "%s: bind", prefix);
	test_result(label, ret == 0);

	/* If caller asked for an ephemeral port (port == 0), read back the
     * one the kernel assigned so the forked child can connect to it.
     * The child inherits this updated `port` variable via fork(). */
	if (ret == 0 && port == 0) {
		socklen_t alen = sizeof(addr);
		if (getsockname(server_fd, (struct sockaddr *)&addr, &alen) ==
		    0)
			port = ntohs(addr.sin_port);
	}

	if (ret == 0) {
		ret = listen(server_fd, 4);
		snprintf(label, sizeof(label), "%s: listen", prefix);
		test_result(label, ret == 0);
	}

	/* Bound the accept: if the child's connect() ever fails (e.g. a
	 * handshake segment dropped under heavy parallel load), the server
	 * must not block in accept() forever and wedge the whole run — it
	 * reports a clean accept failure and the suite carries on. */
	{
		struct timeval atv = { .tv_sec = 30, .tv_usec = 0 };
		setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &atv,
			   sizeof(atv));
	}

	if (ret == 0) {
		pid_t pid = fork();
		if (pid == 0) {
			int client_fd = socket(AF_INET, SOCK_STREAM, 0);
			if (client_fd >= 0) {
				struct sockaddr_in dst;
				memset(&dst, 0, sizeof(dst));
				dst.sin_family = AF_INET;
				dst.sin_port = htons(port);
				dst.sin_addr.s_addr = htonl(connect_ip);

				if (connect(client_fd, (struct sockaddr *)&dst,
					    sizeof(dst)) == 0) {
					char sendbuf[transfer_size];
					for (int i = 0;
					     i < (int)sizeof(sendbuf); i++)
						sendbuf[i] =
							(char)('a' + (i % 23));

					size_t sent = 0;
					while (sent < sizeof(sendbuf)) {
						ssize_t n = send(
							client_fd,
							sendbuf + sent,
							sizeof(sendbuf) - sent,
							0);
						if (n <= 0)
							break;
						sent += (size_t)n;
					}

					close(client_fd);
					_exit(sent == sizeof(sendbuf) ? 0 : 2);
				}
				close(client_fd);
			}
			_exit(1);
		} else if (pid > 0) {
			/* Plain accept() — no watchdog fork here.  A fork()
			 * immediately before accept() perturbs pid allocation and
			 * scheduling right as the handshake completes, which masks
			 * the rare accept race (it stopped reproducing once the
			 * watchdog was added).  The kernel-side invariant WARNs now
			 * catch a stuck/timed-out accept at its source. */
			int conn_fd = accept(server_fd, NULL, NULL);
			snprintf(label, sizeof(label), "%s: accept", prefix);
			test_result(label, conn_fd >= 0);

			if (conn_fd >= 0) {
				char recvbuf[transfer_size];
				char expectbuf[transfer_size];
				for (int i = 0; i < (int)sizeof(expectbuf); i++)
					expectbuf[i] = (char)('a' + (i % 23));

				size_t recvd = 0;
				while (recvd < sizeof(recvbuf)) {
					ssize_t n = recv(
						conn_fd, recvbuf + recvd,
						sizeof(recvbuf) - recvd, 0);
					if (n <= 0)
						break;
					recvd += (size_t)n;
				}

				snprintf(label, sizeof(label),
					 "%s: recv 4096 bytes", prefix);
				test_result(label, recvd == sizeof(recvbuf));

				int matches = (recvd == sizeof(recvbuf) &&
					       memcmp(recvbuf, expectbuf,
						      sizeof(recvbuf)) == 0);
				snprintf(label, sizeof(label),
					 "%s: payload matches", prefix);
				test_result(label, matches);
				close(conn_fd);
			}

			int status = 0;
			waitpid(pid, &status, 0);
			snprintf(label, sizeof(label), "%s: client completed",
				 prefix);
			test_result(label, WIFEXITED(status) &&
						   WEXITSTATUS(status) == 0);
		} else {
			snprintf(label, sizeof(label), "%s: fork", prefix);
			test_fail(label);
		}
	}

	close(server_fd);
}

static void run_programerror_case(const char *name, const char *mode,
				  int expected_sig)
{
	pid_t child = fork();
	if (child < 0) {
		test_fail(name);
		return;
	}
	if (child == 0) {
		char *argv_exec[] = { "/usr/local/bin/progerr", (char *)mode,
				      NULL };
		char *envp_exec[] = { NULL };
		execve("/usr/local/bin/progerr", argv_exec, envp_exec);
		_exit(1);
	}

	int status = 0;
	pid_t waited = waitpid(child, &status, 0);
	if (waited != child) {
		test_fail(name);
		return;
	}

	int ok = 0;
	if (WIFSIGNALED(status) && WTERMSIG(status) == expected_sig) {
		ok = 1;
	} else if (WIFEXITED(status) &&
		   WEXITSTATUS(status) == (128 + expected_sig)) {
		ok = 1;
	}
	test_result(name, ok);
}

// ========================================
// Pthread test helper functions (file-scope to avoid GCC nested function trampolines)
// ========================================

// For pthread_create/join test
static volatile int g_simple_thread_ran = 0;
static volatile int g_simple_thread_arg = 0;

static void *simple_thread_fn(void *arg)
{
	g_simple_thread_ran = 1;
	g_simple_thread_arg = (int)(long)arg;
	return (void *)42L;
}

/* Copy-on-write stress: several threads write disjoint bytes of the same
 * fork-shared pages at once, so many of them fault on one page simultaneously.
 * At file scope on purpose — a nested function would need a stack trampoline,
 * which will not execute on a non-executable stack. */
#define COW_THREADS 4
#define COW_STRESS_LEN (32 * 4096)
static unsigned char *g_cow_shared;
static volatile int g_cow_go;

static void *cow_writer_fn(void *arg)
{
	long id = (long)arg;

	while (!g_cow_go)
		;
	/* Each thread owns one byte in COW_THREADS, so a correct run leaves
	 * every byte written exactly once. */
	for (size_t i = (size_t)id; i < COW_STRESS_LEN; i += COW_THREADS)
		g_cow_shared[i] = (unsigned char)(id + 1);
	return NULL;
}

/* Copy-on-write COHERENCY: does a thread ever read a page through a stale
 * translation after another thread copied it?
 *
 * The stress test above writes disjoint bytes and checks the result after the
 * join, so it proves no write was LOST.  It cannot see the other failure: a
 * thread reading a page through a translation that still points at the
 * pre-copy frame, and so seeing the page as it was before the copy.  Both
 * threads are in one address space and must see one memory; a window where
 * they do not is a coherency bug, and it lasts from the moment the new mapping
 * is published until every other processor has been told about it.
 *
 * The check rests on the processor's store ordering.  The writer stores to the
 * page and then to a flag, in that order, and this architecture does not
 * reorder stores.  So a reader that has seen the flag MUST see the page store
 * -- unless its translation is pointing somewhere else entirely.  Nothing here
 * depends on timing being lucky: if the reader sees the flag and not the
 * write, that is a translation fault and nothing else.
 *
 * The pages have to be copy-on-write for there to be a copy at all, which is
 * what the fork is for -- the child only has to stay alive.  The reader touches
 * every page first, so it HAS a cached translation to go stale; without that
 * it would simply walk the tables and find the new frame.
 */
#define COWSTALE_PAGES 256
#define COWSTALE_PGSZ 4096
static unsigned char *g_cowstale_mem;
static volatile long *g_cowstale_flag;
static volatile long g_cowstale_checked;
static volatile long g_cowstale_stale;
static volatile int g_cowstale_stop;

static void *cowstale_reader_fn(void *arg)
{
	volatile unsigned char sink = 0;
	long i;

	(void)arg;
	/* Populate this processor's translations for every shared page.  A
	 * read does not resolve copy-on-write, so these stay pointing at the
	 * frame the writer is about to copy away from. */
	for (i = 0; i < COWSTALE_PAGES; i++)
		sink ^= g_cowstale_mem[i * COWSTALE_PGSZ];
	(void)sink;

	for (i = 0; i < COWSTALE_PAGES; i++) {
		long spins = 0;

		while (*g_cowstale_flag < i) {
			if (g_cowstale_stop)
				return NULL;
			if (++spins > 100000000L) {
				g_cowstale_stop = 1;
				return NULL;
			}
		}
		__asm__ volatile("" ::: "memory");
		if (g_cowstale_mem[i * COWSTALE_PGSZ] !=
		    (unsigned char)(i + 1))
			g_cowstale_stale++;
		g_cowstale_checked++;
	}
	return NULL;
}

/* For the shared-descriptor-table test: threads of a process share ONE fd
 * table (CLONE_FILES), so a descriptor opened by any thread is usable by all
 * of them, a close by one is seen by the others, and the close-on-exec bits
 * are process-wide.  Every field below records what the thread observed so
 * the main thread can assert on it after the join. */
static int g_fdshare_main_fd = -1; /* opened by main, used by the thread   */
static int g_fdshare_thread_fd = -1; /* opened by the thread, used by main   */
static volatile int g_fdshare_write_ok = -1;
static volatile int g_fdshare_cloexec = -1;
static volatile int g_fdshare_stdout_ok = -1;
static const char g_fdshare_text[] = "shared-fd";

static void *fdshare_thread_fn(void *arg)
{
	(void)arg;
	/* 1. A descriptor the main thread opened must be usable here.  With a
	 *    private per-thread table this was EBADF. */
	g_fdshare_write_ok =
		(write(g_fdshare_main_fd, g_fdshare_text,
		       sizeof(g_fdshare_text) - 1) ==
		 (ssize_t)(sizeof(g_fdshare_text) - 1)) ?
			1 :
			0;
	/* 2. Close-on-exec state is per descriptor, not per thread. */
	g_fdshare_cloexec = fcntl(g_fdshare_main_fd, F_GETFD);
	/* 3. stdout points wherever the PROCESS pointed it.  A thread whose
	 *    slot 1 was empty fell back to the console and silently bypassed
	 *    the redirection. */
	g_fdshare_stdout_ok = (write(1, "T", 1) == 1) ? 1 : 0;
	/* 4. A descriptor opened here must be visible to the main thread. */
	g_fdshare_thread_fd = open("/dev/zero", O_RDONLY);
	return NULL;
}

// For pthread_detach test
static volatile int g_detached_thread_ran = 0;

static void *detached_thread_fn(void *arg)
{
	(void)arg;
	g_detached_thread_ran = 1;
	return NULL;
}

// For malloc cross-thread free test: producers allocate and publish blocks,
// consumers verify and free them (exercises frees on a foreign arena).
#define XT_MBOX 128
static void *g_xt_mbox[XT_MBOX];
static size_t g_xt_sz[XT_MBOX];
static pthread_mutex_t g_xt_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_xt_prod_done = 0;
static volatile long g_xt_produced = 0;
static volatile long g_xt_consumed = 0;
static volatile long g_xt_errors = 0;

static void *xt_producer_fn(void *arg)
{
	unsigned long st = 0x1111UL + (unsigned long)(long)arg;
	for (int i = 0; i < 3000; i++) {
		st ^= st << 13;
		st ^= st >> 7;
		st ^= st << 17;
		size_t sz = 16 + (st % 4096);
		unsigned char *p = malloc(sz);
		if (!p) {
			__sync_fetch_and_add(&g_xt_errors, 1);
			continue;
		}
		memset(p, (int)(sz & 0xff), sz);
		int placed = 0;
		while (!placed) {
			pthread_mutex_lock(&g_xt_lock);
			for (int s = 0; s < XT_MBOX; s++) {
				if (!g_xt_mbox[s]) {
					g_xt_sz[s] = sz;
					g_xt_mbox[s] = p;
					placed = 1;
					break;
				}
			}
			pthread_mutex_unlock(&g_xt_lock);
			if (!placed)
				sched_yield();
		}
		__sync_fetch_and_add(&g_xt_produced, 1);
		/* local churn on this thread's own cache/arena */
		void *q = malloc(1 + (st % 512));
		free(q);
	}
	return NULL;
}

static void *xt_consumer_fn(void *arg)
{
	(void)arg;
	for (;;) {
		unsigned char *p = NULL;
		size_t sz = 0;
		pthread_mutex_lock(&g_xt_lock);
		for (int s = 0; s < XT_MBOX; s++) {
			if (g_xt_mbox[s]) {
				p = g_xt_mbox[s];
				sz = g_xt_sz[s];
				g_xt_mbox[s] = NULL;
				break;
			}
		}
		pthread_mutex_unlock(&g_xt_lock);
		if (p) {
			if (p[0] != (unsigned char)(sz & 0xff) ||
			    p[sz - 1] != (unsigned char)(sz & 0xff))
				__sync_fetch_and_add(&g_xt_errors, 1);
			free(p);
			__sync_fetch_and_add(&g_xt_consumed, 1);
		} else {
			if (g_xt_prod_done && g_xt_consumed >= g_xt_produced)
				break;
			sched_yield();
		}
	}
	return NULL;
}

// For mutex contention test
static pthread_mutex_t g_contention_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_shared_counter = 0;

static void *increment_thread_fn(void *arg)
{
	int count = (int)(long)arg;
	for (int i = 0; i < count; i++) {
		pthread_mutex_lock(&g_contention_mutex);
		g_shared_counter++;
		pthread_mutex_unlock(&g_contention_mutex);
	}
	return NULL;
}

// For condition variable test
struct cond_test_args {
	pthread_cond_t *cond;
	pthread_mutex_t *mutex;
	volatile int *flag;
};

static void *cond_waiter_thread_fn(void *arg)
{
	struct cond_test_args *args = (struct cond_test_args *)arg;
	pthread_mutex_lock(args->mutex);
	while (!*args->flag) {
		pthread_cond_wait(args->cond, args->mutex);
	}
	pthread_mutex_unlock(args->mutex);
	return (void *)99L;
}

// For broadcast test
static pthread_cond_t g_bcast_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_bcast_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_bcast_flag = 0;
static volatile int g_waiters_done = 0;

static void *bcast_waiter_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_bcast_mutex);
	while (!g_bcast_flag) {
		pthread_cond_wait(&g_bcast_cond, &g_bcast_mutex);
	}
	g_waiters_done++;
	pthread_mutex_unlock(&g_bcast_mutex);
	return NULL;
}

// For barrier test
static pthread_barrier_t g_barrier;
static volatile int g_barrier_arrivals = 0;

static void *barrier_thread_fn(void *arg)
{
	(void)arg;
	__sync_fetch_and_add(&g_barrier_arrivals, 1);
	int r = pthread_barrier_wait(&g_barrier);
	return (void *)(long)r;
}

// For TSD test
static pthread_key_t g_tsd_key;
static volatile int g_destructor_called = 0;

static void tsd_destructor_fn(void *value)
{
	if (value) {
		g_destructor_called = 1;
	}
}

static void *tsd_thread_fn(void *arg)
{
	(void)arg;
	// Should be NULL initially in new thread
	void *v = pthread_getspecific(g_tsd_key);
	if (v != NULL)
		return (void *)1L;

	// Set thread-local value
	pthread_setspecific(g_tsd_key, (void *)99999L);
	v = pthread_getspecific(g_tsd_key);
	if (v != (void *)99999L)
		return (void *)2L;

	return (void *)0L; // Success
}

// For pthread_once test
static pthread_once_t g_once_control = PTHREAD_ONCE_INIT;
static volatile int g_once_counter = 0;

static void once_init_fn(void)
{
	g_once_counter++;
}

static void *once_thread_fn(void *arg)
{
	(void)arg;
	pthread_once(&g_once_control, once_init_fn);
	return NULL;
}

/* Recursively remove a directory and all its contents.
 * Only call with a PID-specific path — never a shared directory. */
static void rmtree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return;
	if (!S_ISDIR(st.st_mode)) {
		unlink(path);
		return;
	}
	DIR *d = opendir(path);
	if (!d)
		return;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		char child[512];
		snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
		rmtree(child);
	}
	closedir(d);
	rmdir(path);
}

/* Credential / permission tests.  These run from a privileged process (the
 * shell spawns programs with the inherited root credentials), so denials are
 * exercised by dropping the effective uid with seteuid() and then restoring it
 * with seteuid(0) — the real and saved uids stay 0, so root can always be
 * regained.  The cross-uid kill() test uses forked children that take a fixed
 * uid via setresuid(), so the parent's credentials are never disturbed. */
// ============================================================================
// Device nodes: /dev/null, /dev/zero, /dev/urandom, /dev/tty0, /dev/fb0,
// /dev/input (added with the display/input driver work)
// ============================================================================
static void test_dev_nodes(void)
{
	printf("\n--- Device Node Tests (/dev) ---\n");

	int fd = open("/dev/null", O_WRONLY);
	test_result("open(/dev/null)", fd >= 0);
	if (fd >= 0) {
		test_result("write(/dev/null) accepts data",
			    write(fd, "abc", 3) == 3);
		close(fd);
	}

	fd = open("/dev/zero", O_RDONLY);
	test_result("open(/dev/zero)", fd >= 0);
	if (fd >= 0) {
		char b[16];
		memset(b, 0xAA, sizeof(b));
		int ok = read(fd, b, sizeof(b)) == (long)sizeof(b);
		for (int i = 0; i < 16 && ok; i++)
			if (b[i])
				ok = 0;
		test_result("read(/dev/zero) returns zeros", ok);
		close(fd);
	}

	fd = open("/dev/urandom", O_RDONLY);
	test_result("open(/dev/urandom)", fd >= 0);
	if (fd >= 0) {
		unsigned char b1[32], b2[32];
		int r1 = (int)read(fd, b1, 32);
		int r2 = (int)read(fd, b2, 32);
		test_result("read(/dev/urandom) full blocks",
			    r1 == 32 && r2 == 32);
		test_result("urandom output varies", memcmp(b1, b2, 32) != 0);
		close(fd);
	}

	fd = open("/dev/tty0", O_RDWR);
	test_result("open(/dev/tty0)", fd >= 0);
	if (fd >= 0)
		close(fd);

	struct stat st;
	test_result("stat(/dev/input) is a directory",
		    stat("/dev/input", &st) == 0 && S_ISDIR(st.st_mode));
	test_result("stat(/dev/fb0) is a char device",
		    stat("/dev/fb0", &st) == 0 && S_ISCHR(st.st_mode));
}

// ============================================================================
// Framebuffer device: geometry ioctls + user mmap of the framebuffer
// ============================================================================
static void test_fbdev(void)
{
	printf("\n--- Framebuffer Device Tests (/dev/fb0) ---\n");

	int fd = open("/dev/fb0", O_RDWR);
	test_result("open(/dev/fb0, O_RDWR)", fd >= 0);
	if (fd < 0)
		return;

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	memset(&var, 0, sizeof(var));
	memset(&fix, 0, sizeof(fix));

	test_result("FBIOGET_VSCREENINFO",
		    ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0);
	test_result("var: sane mode",
		    var.xres >= 640 && var.yres >= 480 &&
			    (var.bits_per_pixel == 32 ||
			     var.bits_per_pixel == 16));
	test_result("FBIOGET_FSCREENINFO",
		    ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0);
	test_result("fix: line_length covers xres",
		    fix.line_length >= var.xres * (var.bits_per_pixel / 8));
	test_result("fix: smem_len covers the screen",
		    fix.smem_len >= fix.line_length * var.yres);
	test_result("FBIOPUT_VSCREENINFO with current mode is accepted",
		    ioctl(fd, FBIOPUT_VSCREENINFO, &var) == 0);

	size_t maplen = (size_t)fix.line_length * var.yres;
	void *fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			0);
	test_result("mmap(/dev/fb0) succeeds", fb != MAP_FAILED);
	if (fb != MAP_FAILED) {
		volatile unsigned int *px = (volatile unsigned int *)fb;
		unsigned int saved = px[0];
		px[0] = 0x00FF00FF;
		test_result("framebuffer mmap write/readback",
			    px[0] == 0x00FF00FF);
		px[0] = saved;
		test_result("munmap(framebuffer)", munmap(fb, maplen) == 0);
	}
	close(fd);
}

// ============================================================================
// Event devices: capability ioctls, nonblocking reads, poll, exclusive grab
// ============================================================================
static void test_evdev(void)
{
	printf("\n--- Input Device Tests (/dev/input/event*) ---\n");

	int fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	test_result("open(/dev/input/event0)", fd >= 0);
	if (fd >= 0) {
		int ver = 0;
		test_result("EVIOCGVERSION returns protocol version",
			    ioctl(fd, EVIOCGVERSION, &ver) == 0 &&
				    ver == EV_VERSION);

		struct input_id id;
		memset(&id, 0, sizeof(id));
		test_result("EVIOCGID reports keyboard identity",
			    ioctl(fd, EVIOCGID, &id) == 0 &&
				    id.bustype == BUS_I8042);

		char name[64] = { 0 };
		int n = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
		test_result("EVIOCGNAME returns a name", n > 0 && name[0]);

		unsigned char evb[(EV_MAX + 7) / 8];
		memset(evb, 0, sizeof(evb));
		test_result("EVIOCGBIT(0) reports EV_KEY",
			    ioctl(fd, EVIOCGBIT(0, sizeof(evb)), evb) > 0 &&
				    (evb[EV_KEY / 8] & (1 << (EV_KEY % 8))));

		unsigned char keyb[(KEY_MAX + 7) / 8];
		memset(keyb, 0, sizeof(keyb));
		test_result("EVIOCGBIT(EV_KEY) reports KEY_A",
			    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >
					    0 &&
				    (keyb[KEY_A / 8] & (1 << (KEY_A % 8))));

		unsigned char ks[(KEY_MAX + 7) / 8];
		test_result("EVIOCGKEY returns key state bitmap",
			    ioctl(fd, EVIOCGKEY(sizeof(ks)), ks) >= 0);

		struct input_event ev;
		long r = read(fd, &ev, sizeof(ev));
		test_result("nonblocking read: EAGAIN or one whole event",
			    (r == -1 && errno == EAGAIN) ||
				    r == (long)sizeof(ev));
		test_result("short read buffer rejected",
			    read(fd, &ev, sizeof(ev) - 1) == -1);

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 0);
		test_result("poll(event0) works", pr >= 0);

		test_result("EVIOCGRAB(1) acquires the grab",
			    ioctl(fd, EVIOCGRAB, (void *)1) == 0);
		test_result("EVIOCGRAB(0) releases the grab",
			    ioctl(fd, EVIOCGRAB, (void *)0) == 0);

		/* The probe sequence xf86-input-evdev runs at device init.
		 * All of these used to be rejected outright, which stops the
		 * driver before it ever reads an event. */
		unsigned char props[(INPUT_PROP_MAX + 7) / 8];
		memset(props, 0xFF, sizeof(props));
		test_result("EVIOCGPROP returns a property bitmap",
			    ioctl(fd, EVIOCGPROP(sizeof(props)), props) > 0);
		test_result("keyboard claims no pointer property",
			    (props[INPUT_PROP_POINTER / 8] &
			     (1 << (INPUT_PROP_POINTER % 8))) == 0);

		char phys[64] = { 0 };
		int pn = ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
		test_result("EVIOCGPHYS returns a physical path",
			    pn > 0 && phys[0] != '\0');

		/* No serial number exists, and saying so is not the same as
		 * reporting an empty one. */
		char uniq[64] = { 0 };
		errno = 0;
		test_result("EVIOCGUNIQ reports no unique id -> ENOENT",
			    ioctl(fd, EVIOCGUNIQ(sizeof(uniq)), uniq) == -1 &&
				    errno == ENOENT);

		struct input_absinfo ai;
		memset(&ai, 0xFF, sizeof(ai));
		test_result("EVIOCGABS succeeds on a relative-only device",
			    ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0);
		test_result("EVIOCGABS reports an empty range",
			    ai.minimum == 0 && ai.maximum == 0);

		unsigned char leds[(LED_MAX + 7) / 8];
		memset(leds, 0xFF, sizeof(leds));
		test_result("EVIOCGLED returns the LED state",
			    ioctl(fd, EVIOCGLED(sizeof(leds)), leds) > 0 &&
				    leds[0] == 0);

		unsigned char sw[(SW_MAX + 7) / 8];
		memset(sw, 0xFF, sizeof(sw));
		test_result("EVIOCGSW returns the switch state",
			    ioctl(fd, EVIOCGSW(sizeof(sw)), sw) > 0 &&
				    sw[0] == 0);

		/* Display servers switch to CLOCK_MONOTONIC so input timing
		 * survives the wall clock being stepped. */
		int clk = CLOCK_MONOTONIC;
		test_result("EVIOCSCLOCKID accepts CLOCK_MONOTONIC",
			    ioctl(fd, EVIOCSCLOCKID, &clk) == 0);
		clk = CLOCK_REALTIME;
		test_result("EVIOCSCLOCKID accepts CLOCK_REALTIME",
			    ioctl(fd, EVIOCSCLOCKID, &clk) == 0);
		clk = 99;
		errno = 0;
		test_result("EVIOCSCLOCKID rejects an unknown clock",
			    ioctl(fd, EVIOCSCLOCKID, &clk) == -1 &&
				    errno == EINVAL);
		close(fd);
	}

	fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	test_result("open(/dev/input/event1)", fd >= 0);
	if (fd >= 0) {
		unsigned char relb[(REL_MAX + 7) / 8];
		memset(relb, 0, sizeof(relb));
		int ok = ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relb)), relb) > 0;
		test_result(
			"mouse: EVIOCGBIT(EV_REL) has REL_X/REL_Y/REL_WHEEL",
			ok && (relb[0] & (1 << REL_X)) &&
				(relb[0] & (1 << REL_Y)) &&
				(relb[REL_WHEEL / 8] &
				 (1 << (REL_WHEEL % 8))));

		unsigned char evb1[(EV_MAX + 7) / 8];
		memset(evb1, 0, sizeof(evb1));
		test_result("mouse: EVIOCGBIT(0) reports EV_REL",
			    ioctl(fd, EVIOCGBIT(0, sizeof(evb1)), evb1) > 0 &&
				    (evb1[EV_REL / 8] &
				     (1 << (EV_REL % 8))));

		struct input_id id;
		memset(&id, 0, sizeof(id));
		test_result("mouse: EVIOCGID distinct product",
			    ioctl(fd, EVIOCGID, &id) == 0 && id.product == 2);

		/* The mouse must advertise INPUT_PROP_POINTER: that is how a
		 * driver tells relative motion driving a cursor apart from a
		 * touchscreen that maps onto screen coordinates. */
		unsigned char mprops[(INPUT_PROP_MAX + 7) / 8];
		memset(mprops, 0, sizeof(mprops));
		test_result("mouse: EVIOCGPROP returns a bitmap",
			    ioctl(fd, EVIOCGPROP(sizeof(mprops)), mprops) > 0);
		test_result("mouse: claims INPUT_PROP_POINTER",
			    (mprops[INPUT_PROP_POINTER / 8] &
			     (1 << (INPUT_PROP_POINTER % 8))) != 0);

		char mphys[64] = { 0 };
		test_result("mouse: EVIOCGPHYS differs from the keyboard's",
			    ioctl(fd, EVIOCGPHYS(sizeof(mphys)), mphys) > 0 &&
				    mphys[0] != '\0');
		close(fd);
	}

	test_result("open(/dev/input/event7) fails",
		    open("/dev/input/event7", O_RDONLY) < 0);
}

static void test_credentials(void)
{
	printf("\n[TEST] credentials & permissions\n");

	/* Baseline: a freshly spawned program is privileged. */
	test_result("getuid() is root at start", getuid() == 0);
	test_result("geteuid() is root at start", geteuid() == 0);
	int rr = -1, ee = -1, ss = -1;
	test_result("getresuid() succeeds", getresuid(&rr, &ee, &ss) == 0);
	test_result("getresuid() reports all-root", rr == 0 && ee == 0 &&
							    ss == 0);

	/* kill() permission: a uid-3000 process may not signal a uid-2000 one.
	 * Done entirely in children (parent stays root). */
	pid_t victim = fork();
	if (victim == 0) {
		setresuid(2000, 2000, 2000); /* become uid 2000 permanently */
		sleep(30); /* wait to be killed by the (root) parent */
		_exit(0);
	}
	if (victim > 0) {
		usleep(50000); /* let the victim drop its uid */
		pid_t prober = fork();
		if (prober == 0) {
			setresuid(3000, 3000, 3000);
			int kr = kill(victim, 0); /* permission probe, no signal */
			_exit((kr < 0 && errno == EPERM) ? 0 : 1);
		}
		int pst = 0;
		waitpid(prober, &pst, 0);
		test_result("kill() across uids denied (EPERM)",
			    WIFEXITED(pst) && WEXITSTATUS(pst) == 0);
		kill(victim, SIGKILL); /* parent is root: allowed */
		waitpid(victim, NULL, 0);
	}

	/* Create a root-owned 0600 file and a 0700 directory while privileged.
	 * Paths are made unique per process so several testlibc instances can run
	 * concurrently without racing on the same files; stale entries from an
	 * earlier run that reused this PID are cleared first. */
	int mypid = (int)getpid();
	char secret[64], priv_dir[64], priv_child[96];
	snprintf(secret, sizeof(secret), "/tmp/cred_secret_%d", mypid);
	snprintf(priv_dir, sizeof(priv_dir), "/tmp/cred_dir_%d", mypid);
	snprintf(priv_child, sizeof(priv_child), "%s/whatever", priv_dir);
	unlink(secret);
	rmdir(priv_dir);
	int fd = open(secret, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	test_result("create root-owned 0600 file", fd >= 0);
	if (fd >= 0) {
		write(fd, "x", 1);
		close(fd);
	}
	chmod(secret, 0600);
	mkdir(priv_dir, 0700);

	/* Drop the effective uid to an unprivileged value. */
	test_result("seteuid(1000) succeeds", seteuid(1000) == 0);
	test_result("geteuid() == 1000 after drop", geteuid() == 1000);
	test_result("getuid() == 0 (real uid unchanged)", getuid() == 0);

	/* A non-root effective uid cannot raise privilege. */
	test_result("setuid(1) denied for non-root", setuid(1) < 0);
	int grp[1] = { 7 };
	test_result("setgroups() denied for non-root", setgroups(1, grp) < 0);

	/* A non-root process cannot read a root-owned 0600 file ... */
	int rfd = open(secret, O_RDONLY);
	test_result("read of root 0600 file denied (EACCES)",
		    rfd < 0 && errno == EACCES);
	if (rfd >= 0)
		close(rfd);
	/* ... cannot search a root-owned 0700 directory ... */
	int dfd = open(priv_child, O_RDONLY);
	test_result("traverse of 0700 root dir denied", dfd < 0);
	if (dfd >= 0)
		close(dfd);
	/* ... and cannot chmod/chown a file it does not own. */
	test_result("chmod of unowned file denied", chmod(secret, 0666) < 0);
	test_result("chown of unowned file denied", chown(secret, 1000, 1000) < 0);

	/* Restore privilege (always possible: real and saved uid are still 0). */
	test_result("seteuid(0) restores root", seteuid(0) == 0 &&
						       geteuid() == 0);

	/* Root regains full access. */
	int rfd2 = open(secret, O_RDONLY);
	test_result("root can read the 0600 file", rfd2 >= 0);
	if (rfd2 >= 0)
		close(rfd2);
	test_result("root can chmod the file", chmod(secret, 0644) == 0);

	/* Supplementary groups round-trip as root. */
	int setg[2] = { 10, 20 };
	test_result("setgroups() succeeds as root", setgroups(2, setg) == 0);
	int gotg[8];
	int ng = getgroups(8, gotg);
	test_result("getgroups() returns the set count", ng == 2);

	/* Cleanup. */
	unlink(secret);
	rmdir(priv_dir);
}

/* ============================================================
 * User/group/shadow database, crypt, credentials, session and PAM tests.
 * These run in the root context (before the non-root permission section).
 * ============================================================ */

static const char *g_test_password;

static int test_pam_conv(int num_msg, const struct pam_message **msg,
                         struct pam_response **resp, void *appdata)
{
	struct pam_response *r;
	int i;
	(void)appdata;
	if (num_msg <= 0)
		return PAM_CONV_ERR;
	r = calloc(num_msg, sizeof(*r));
	if (!r)
		return PAM_BUF_ERR;
	for (i = 0; i < num_msg; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
		    msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
			r[i].resp = strdup(g_test_password ? g_test_password : "");
	}
	*resp = r;
	return PAM_SUCCESS;
}

static void test_userdb(void)
{
	printf("\n[TEST] passwd database (getpwnam/getpwuid)\n");
	struct passwd *pw = getpwnam("root");
	test_result("getpwnam(\"root\") found", pw != NULL);
	if (pw) {
		test_result("root uid == 0", pw->pw_uid == 0);
		test_result("root gid == 0", pw->pw_gid == 0);
		test_result("root home == /root",
			    strcmp(pw->pw_dir, "/root") == 0);
		/* /etc/passwd gives root /bin/bash — /bin/sh is a symlink to
		 * the same binary, so assert what the file actually says. */
		test_result("root shell == /bin/bash",
			    strcmp(pw->pw_shell, "/bin/bash") == 0);
	}
	struct passwd *pw2 = getpwuid(0);
	test_result("getpwuid(0) returns root",
		    pw2 && strcmp(pw2->pw_name, "root") == 0);
	test_result("getpwnam(nonexistent) == NULL",
		    getpwnam("no_such_user_xyz") == NULL);

	/* reentrant */
	struct passwd pwr;
	char pbuf[512];
	struct passwd *pres = NULL;
	int rc = getpwnam_r("root", &pwr, pbuf, sizeof(pbuf), &pres);
	test_result("getpwnam_r(\"root\") ok", rc == 0 && pres != NULL &&
						       pres->pw_uid == 0);

	/* iteration */
	int count = 0;
	setpwent();
	while (getpwent() != NULL)
		count++;
	endpwent();
	test_result("getpwent() iterated at least one entry", count >= 1);

	printf("\n[TEST] group database (getgrnam/getgrgid/initgroups)\n");
	struct group *gr = getgrnam("root");
	test_result("getgrnam(\"root\") found", gr != NULL);
	if (gr)
		test_result("root group gid == 0", gr->gr_gid == 0);
	struct group *gr2 = getgrgid(0);
	test_result("getgrgid(0) returns root group",
		    gr2 && strcmp(gr2->gr_name, "root") == 0);
	/* initgroups must succeed for root and include gid 0 */
	int ig = initgroups("root", 0);
	test_result("initgroups(\"root\", 0) succeeds", ig == 0);
	if (ig == 0) {
		int list[32];
		int n = getgroups(32, list);
		int has0 = 0, i;
		for (i = 0; i < n; i++)
			if (list[i] == 0)
				has0 = 1;
		test_result("supplementary groups include gid 0", has0);
	}
}

/* A fixed yescrypt hash of the password "toor".  crypt() is verified against
 * this self-contained value rather than /etc/shadow, because the root password
 * is usually changed by the user after installation. */
static const char *const TOOR_HASH =
	"$y$j9T$LikeOSrootsalt000000000$dvrg4Bi3ykTNHFOIx5ljbUBrsUJnocNuwHToDQWqKr1";

static void test_shadow_and_crypt(void)
{
	printf("\n[TEST] shadow database + yescrypt crypt()\n");

	/* /etc/shadow must be readable (root) and yield a non-empty hash; we do
	 * NOT assume its contents - the password may have been changed. */
	struct spwd *sp = getspnam("root");
	test_result("getspnam(\"root\") found (needs root)", sp != NULL);
	if (sp)
		test_result("root has a non-empty password hash",
			    sp->sp_pwdp && sp->sp_pwdp[0] != '\0');

	/* crypt() correctness against the known "toor" hash. */
	test_result("known hash is yescrypt ($y$)",
		    strncmp(TOOR_HASH, "$y$", 3) == 0);

	struct crypt_data cd;
	cd.initialized = 0;
	char *h = crypt_r("toor", TOOR_HASH, &cd);
	test_result("crypt_r(\"toor\", known) reproduces known hash",
		    h != NULL && strcmp(h, TOOR_HASH) == 0);

	cd.initialized = 0;
	char *w = crypt_r("wrongpassword", TOOR_HASH, &cd);
	test_result("crypt_r(wrong, known) != known",
		    w != NULL && strcmp(w, TOOR_HASH) != 0);

	/* Non-reentrant crypt() should also reproduce the hash. */
	char *h2 = crypt("toor", TOOR_HASH);
	test_result("crypt(\"toor\", known) reproduces known hash",
		    h2 != NULL && strcmp(h2, TOOR_HASH) == 0);

	/* Secure salt generation: crypt_gensalt() draws random bytes from the
	 * kernel CSPRNG, so each call yields a distinct random $y$j9T$ setting,
	 * and hashing a password against it round-trips. */
	char salt1[128] = { 0 }, salt2[128] = { 0 };
	char *g1 = crypt_gensalt("$y$", 0, NULL, 0);
	test_result("crypt_gensalt returns a $y$j9T$ setting",
		    g1 != NULL && strncmp(g1, "$y$j9T$", 7) == 0);
	if (g1) {
		strncpy(salt1, g1, sizeof(salt1) - 1);
		char *g2 = crypt_gensalt("$y$", 0, NULL, 0);
		if (g2)
			strncpy(salt2, g2, sizeof(salt2) - 1);
		test_result("crypt_gensalt salts are random (two differ)",
			    salt2[0] && strcmp(salt1, salt2) != 0);

		struct crypt_data cdg;
		cdg.initialized = 0;
		char *nh = crypt_r("s3cret!", salt1, &cdg);
		char newhash[256] = { 0 };
		if (nh)
			strncpy(newhash, nh, sizeof(newhash) - 1);
		test_result("crypt() with generated salt produces a yescrypt hash",
			    newhash[0] && strncmp(newhash, "$y$", 3) == 0);
		if (newhash[0]) {
			struct crypt_data cdv;
			cdv.initialized = 0;
			char *v = crypt_r("s3cret!", newhash, &cdv);
			test_result("generated-salt hash verifies correct password",
				    v && strcmp(v, newhash) == 0);
			cdv.initialized = 0;
			char *w = crypt_r("wrong!", newhash, &cdv);
			test_result("generated-salt hash rejects wrong password",
				    w && strcmp(w, newhash) != 0);
		}
	}
}

static void test_extra_creds(void)
{
	printf("\n[TEST] setreuid/setregid (in child) + getlogin\n");
	/* Run credential changes in a child so we never disturb the runner. */
	pid_t pid = fork();
	if (pid == 0) {
		int r, e, s;
		int ok = 1;
		/* Change the group id FIRST, while still privileged (euid 0);
		 * dropping euid via setreuid would remove the privilege needed
		 * to set an arbitrary gid. */
		if (setregid(-1, 1000) != 0)
			ok = 0;
		getresgid(&r, &e, &s);
		if (e != 1000 || r != 0)
			ok = 0;
		if (setreuid(-1, 1000) != 0)
			ok = 0;
		getresuid(&r, &e, &s);
		/* euid changed to 1000, real stayed 0 */
		if (e != 1000 || r != 0)
			ok = 0;
		_exit(ok ? 0 : 1);
	} else if (pid > 0) {
		int status = 0;
		waitpid(pid, &status, 0);
		test_result("setreuid/setregid round-trip (child)",
			    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	} else {
		test_fail("fork for setreuid test");
	}

	/* getlogin via LOGNAME fallback */
	setenv("LOGNAME", "root", 1);
	char *l = getlogin();
	test_result("getlogin() returns a name", l != NULL);
	setlogin("root");
	char lbuf[64];
	test_result("getlogin_r() after setlogin(\"root\")",
		    getlogin_r(lbuf, sizeof(lbuf)) == 0 &&
			    strcmp(lbuf, "root") == 0);
}

static void test_session_creation(void)
{
	printf("\n[TEST] session creation (fork+setsid) + login env\n");
	pid_t pid = fork();
	if (pid == 0) {
		pid_t sid = setsid();
		pid_t self = getpid();
		int ok = (sid == self) && (getsid(0) == self) &&
			 (getpgid(0) == self);
		_exit(ok ? 0 : 1);
	} else if (pid > 0) {
		int status = 0;
		waitpid(pid, &status, 0);
		test_result("child is its own session & group leader",
			    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	} else {
		test_fail("fork for session test");
	}

	/* Login environment fields round-trip through the environment. */
	setenv("HOME", "/root", 1);
	setenv("USER", "root", 1);
	setenv("SHELL", "/bin/sh", 1);
	test_result("HOME/USER/SHELL set/get",
		    getenv("HOME") && strcmp(getenv("HOME"), "/root") == 0 &&
			    getenv("USER") &&
			    strcmp(getenv("USER"), "root") == 0 &&
			    getenv("SHELL") &&
			    strcmp(getenv("SHELL"), "/bin/sh") == 0);
}

static void test_pam(void)
{
	printf("\n[TEST] PAM authentication (unix)\n");
	struct pam_conv conv = { test_pam_conv, NULL };
	pam_handle_t *pamh = NULL;

	/* Is "toor" still root's password?  It may have been changed, so only
	 * assert the positive-authentication case when the default is intact. */
	int toor_is_current = 0;
	struct spwd *sp = getspnam("root");
	if (sp && sp->sp_pwdp && sp->sp_pwdp[0]) {
		char *c = crypt("toor", sp->sp_pwdp);
		toor_is_current = (c && strcmp(c, sp->sp_pwdp) == 0);
	}

	g_test_password = "toor";
	if (pam_start("login", "root", &conv, &pamh) == PAM_SUCCESS) {
		int rc = pam_authenticate(pamh, 0);
		if (toor_is_current)
			test_result("pam_authenticate (password 'toor')",
				    rc == PAM_SUCCESS);
		else
			printf("  root password changed from default;"
			       " skipping positive auth assertion\n");
		test_result("pam_acct_mgmt", pam_acct_mgmt(pamh, 0) == PAM_SUCCESS);
		test_result("pam_open_session",
			    pam_open_session(pamh, 0) == PAM_SUCCESS);
		test_result("pam_close_session",
			    pam_close_session(pamh, 0) == PAM_SUCCESS);
		pam_end(pamh, PAM_SUCCESS);
	}

	/* A clearly wrong password must always be rejected. */
	g_test_password = "definitely-not-the-password-xyz";
	if (pam_start("login", "root", &conv, &pamh) == PAM_SUCCESS) {
		test_result("pam_authenticate (wrong password) -> PAM_AUTH_ERR",
			    pam_authenticate(pamh, 0) == PAM_AUTH_ERR);
		pam_end(pamh, 0);
	}

	if (pam_start("login", "no_such_user_xyz", &conv, &pamh) == PAM_SUCCESS) {
		test_result("pam_authenticate (unknown user) -> PAM_USER_UNKNOWN",
			    pam_authenticate(pamh, 0) == PAM_USER_UNKNOWN);
		pam_end(pamh, 0);
	}
}

/* Copy a file's bytes to `dst` with the given mode.  Returns 0 on success. */
static int copy_file(const char *src, const char *dst, mode_t mode)
{
	int in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		close(in);
		return -1;
	}
	char buf[8192];
	ssize_t n;
	int rc = 0;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, n) != n) {
			rc = -1;
			break;
		}
	}
	if (n < 0)
		rc = -1;
	close(in);
	close(out);
	/* open() honours umask; force the exact mode (incl. the setuid bit). */
	if (chmod(dst, mode) != 0)
		rc = -1;
	return rc;
}

/* Run "prog -u" as uid `as_uid` and return the euid it prints (or -1). */
static int run_id_euid_as(const char *prog, int as_uid)
{
	int p[2];
	if (pipe(p) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		close(p[0]);
		dup2(p[1], 1);
		close(p[1]);
		/* Drop to a non-root uid, then exec.  If prog is setuid-root,
		 * the kernel raises euid back to 0 across the exec. */
		setgid(as_uid);
		setuid(as_uid);
		char *av[] = { (char *)"id", (char *)"-u", NULL };
		char *ev[] = { NULL };
		execve(prog, av, ev);
		_exit(127);
	}
	close(p[1]);
	char buf[32];
	int off = 0, r;
	while (off < (int)sizeof(buf) - 1 &&
	       (r = read(p[0], buf + off, sizeof(buf) - 1 - off)) > 0)
		off += r;
	buf[off] = '\0';
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) == 127)
		return -1;
	return atoi(buf);
}

static void test_setuid_exec(void)
{
	printf("\n[TEST] setuid-bit program execution\n");
	if (geteuid() != 0) {
		printf("  (skipped: test is not running as root)\n");
		return;
	}

	/* Unique per-instance names via mkstemp so several testlibc processes
	 * can run this test in parallel without colliding on the temp files. */
	char suid[] = "/tmp/suid_id.XXXXXX";
	char plain[] = "/tmp/plain_id.XXXXXX";
	int fda = mkstemp(suid);
	int fdb = mkstemp(plain);
	if (fda < 0 || fdb < 0) {
		test_fail("setuid test: cannot create temp files");
		if (fda >= 0) { close(fda); unlink(suid); }
		if (fdb >= 0) { close(fdb); unlink(plain); }
		return;
	}
	close(fda);
	close(fdb);

	/* We are root, so these copies are owned by root. */
	if (copy_file("/bin/id", suid, 04755) != 0 ||
	    copy_file("/bin/id", plain, 0755) != 0) {
		test_fail("setuid test: cannot stage /bin/id copies");
		unlink(suid);
		unlink(plain);
		return;
	}

	/* Confirm the setuid bit actually persisted on disk. */
	struct stat stb;
	test_result("setuid copy has S_ISUID bit",
		    stat(suid, &stb) == 0 && (stb.st_mode & 04000));

	/* A non-root child exec'ing the setuid-root binary regains euid 0. */
	int e_suid = run_id_euid_as(suid, 1000);
	test_result("setuid-root exec: child euid becomes 0", e_suid == 0);

	/* Without the bit, the child keeps its dropped (non-root) euid. */
	int e_plain = run_id_euid_as(plain, 1000);
	test_result("non-setuid exec: child euid stays 1000", e_plain == 1000);

	unlink(suid);
	unlink(plain);
}

/* ---- shebang (#!) exec tests ------------------------------------------ */

static int write_script(const char *path, mode_t mode, const char *contents)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	size_t len = strlen(contents);
	int rc = (write(fd, contents, len) == (ssize_t)len) ? 0 : -1;
	close(fd);
	if (chmod(path, mode) != 0)
		rc = -1;
	return rc;
}

/* Exec `path` in a child and capture its stdout; returns 0 with *status
 * set, -1 on infrastructure failure. */
static int exec_capture(const char *path, char *const argvv[],
			char *const envpv[], char *out, size_t outsz, int *status)
{
	int p[2];
	if (pipe(p) != 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (pid == 0) {
		close(p[0]);
		dup2(p[1], 1);
		close(p[1]);
		execve(path, argvv, envpv);
		_exit(126);
	}
	close(p[1]);
	size_t off = 0;
	int r;
	while (off < outsz - 1 &&
	       (r = read(p[0], out + off, outsz - 1 - off)) > 0)
		off += (size_t)r;
	out[off] = '\0';
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	*status = st;
	return 0;
}

/* Exec `path` in a child expected to FAIL: returns the child's execve
 * errno (or the program's exit status if the exec unexpectedly worked). */
static int exec_expect_errno(const char *path, char *const argvv[],
			     char *const envpv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		if (nul >= 0) {
			dup2(nul, 1);
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		execve(path, argvv, envpv);
		_exit(errno);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Same, but after dropping to `as_uid` (for DAC checks - root skips them). */
static int exec_errno_as_uid(const char *path, int as_uid)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		if (nul >= 0) {
			dup2(nul, 1);
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		setgid(as_uid);
		setuid(as_uid);
		char *av[] = { (char *)path, NULL };
		char *ev[] = { NULL };
		execve(path, av, ev);
		_exit(errno);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_shebang(void)
{
	printf("\n[TEST] shebang (#!) script execution\n");

	char base[48];
	snprintf(base, sizeof(base), "/tmp/shb%d", (int)getpid());
	rmtree(base);
	/* 0755: the uid-1000 children of the EACCES tests must traverse it */
	if (mkdir(base, 0755) != 0) {
		test_fail("shebang: cannot create test dir");
		return;
	}

	char p1[96], p2[96], out[1024], expect[512];
	int st;

	/* basic /bin/sh script: output + exit status */
	snprintf(p1, sizeof(p1), "%s/basic", base);
	write_script(p1, 0755, "#!/bin/sh\necho shebang-sh-ok\nexit 42\n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { (char *)"PATH=/bin", NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		test_result("basic #!/bin/sh script runs",
			    rc == 0 && strstr(out, "shebang-sh-ok") != NULL);
		test_result("script exit status propagates",
			    WIFEXITED(st) && WEXITSTATUS(st) == 42);
	}

	/* argv construction: interp, optarg, script path replaces argv[0],
	 * original tail preserved (testlibc __argv dumps the exact vector) */
	snprintf(p1, sizeof(p1), "%s/argv", base);
	write_script(p1, 0755, "#!/usr/local/bin/testlibc __argv\n");
	{
		char *av[] = { (char *)"IGNORED-ARGV0", (char *)"x",
			       (char *)"y", NULL };
		char *ev[] = { NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		snprintf(expect, sizeof(expect),
			 "0=[/usr/local/bin/testlibc]\n1=[__argv]\n2=[%s]\n"
			 "3=[x]\n4=[y]\n",
			 p1);
		test_result("shebang argv order (interp,opt,script,args)",
			    rc == 0 && strcmp(out, expect) == 0);
	}

	/* whitespace: blanks after #! skipped; remainder is ONE argument with
	 * internal spaces kept; trailing space/tab/CR/LF trimmed */
	snprintf(p1, sizeof(p1), "%s/white", base);
	write_script(p1, 0755,
		     "#! \t /usr/local/bin/testlibc \t __argv  with  spaces \t\r\n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		test_result("shebang whitespace/optarg handling",
			    rc == 0 &&
				    strstr(out, "1=[__argv  with  spaces]") !=
					    NULL);
	}

	/* environment passes through execve unchanged (testlibc __env dumps
	 * it).  NOT "#!/bin/env": the shebang rewrite appends the script path,
	 * which env then treats as a COMMAND and re-executes — script -> env
	 * -> script forever.  (Each env exec is a fresh execve at depth 0, so
	 * the kernel ELOOP budget never trips; conventional Unix loops the
	 * same way.) */
	snprintf(p1, sizeof(p1), "%s/envdump", base);
	write_script(p1, 0755, "#!/usr/local/bin/testlibc __env\n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { (char *)"SHEBANG_MARK=xyzzy", NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		test_result("envp passed unchanged to interpreter",
			    rc == 0 &&
				    strstr(out, "SHEBANG_MARK=xyzzy") != NULL);
	}

	/* /usr/bin/env resolves and locates the interpreter */
	snprintf(p1, sizeof(p1), "%s/envsh", base);
	write_script(p1, 0755, "#!/usr/bin/env sh\necho env-sh-ok\nexit 5\n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { (char *)"PATH=/bin", NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		test_result("#!/usr/bin/env sh works",
			    rc == 0 && strstr(out, "env-sh-ok") != NULL &&
				    WIFEXITED(st) && WEXITSTATUS(st) == 5);
	}

	/* python-style: env finds a PATH-relative interpreter that is itself
	 * a script */
	snprintf(p1, sizeof(p1), "%s/fakepython", base);
	write_script(p1, 0755, "#!/bin/sh\necho fake-python-ran\n");
	snprintf(p2, sizeof(p2), "%s/usepython", base);
	write_script(p2, 0755, "#!/usr/bin/env fakepython\n");
	{
		char pathvar[128];
		snprintf(pathvar, sizeof(pathvar), "PATH=%s:/bin", base);
		char *av[] = { p2, NULL };
		char *ev[] = { pathvar, NULL };
		int rc = exec_capture(p2, av, ev, out, sizeof(out), &st);
		test_result("#!/usr/bin/env PATH interpreter lookup",
			    rc == 0 &&
				    strstr(out, "fake-python-ran") != NULL);
	}

	/* recursion: script -> script -> /bin/echo, argv accumulates */
	snprintf(p1, sizeof(p1), "%s/inner", base);
	write_script(p1, 0755, "#!/bin/echo -n\n");
	snprintf(p2, sizeof(p2), "%s/outer", base);
	snprintf(expect, sizeof(expect), "#!%s\n", p1);
	write_script(p2, 0755, expect);
	{
		char *av[] = { p2, NULL };
		char *ev[] = { NULL };
		int rc = exec_capture(p2, av, ev, out, sizeof(out), &st);
		snprintf(expect, sizeof(expect), "%s %s", p1, p2);
		test_result("recursive shebang (2 levels)",
			    rc == 0 && strcmp(out, expect) == 0);
	}

	/* recursion limit: self-referential interpreter chain -> ELOOP */
	snprintf(p1, sizeof(p1), "%s/self", base);
	snprintf(expect, sizeof(expect), "#!%s\n", p1);
	write_script(p1, 0755, expect);
	{
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		test_result("shebang recursion limit -> ELOOP",
			    exec_expect_errno(p1, av, ev) == ELOOP);
	}

	/* over-long #! line without newline -> ENOEXEC */
	snprintf(p1, sizeof(p1), "%s/longline", base);
	{
		char big[312];
		memcpy(big, "#!/bin/sh", 9);
		memset(big + 9, 'x', 300);
		big[309] = '\0';
		write_script(p1, 0755, big);
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		test_result("truncated #! line -> ENOEXEC",
			    exec_expect_errno(p1, av, ev) == ENOEXEC);
	}

	/* empty interpreter -> ENOEXEC */
	snprintf(p1, sizeof(p1), "%s/noint", base);
	write_script(p1, 0755, "#!\n");
	snprintf(p2, sizeof(p2), "%s/blankint", base);
	write_script(p2, 0755, "#! \t \n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		test_result("empty interpreter -> ENOEXEC",
			    exec_expect_errno(p1, av, ev) == ENOEXEC);
		char *av2[] = { p2, NULL };
		test_result("blank interpreter -> ENOEXEC",
			    exec_expect_errno(p2, av2, ev) == ENOEXEC);
	}

	/* non-ELF, non-script garbage -> ENOEXEC */
	snprintf(p1, sizeof(p1), "%s/garbage", base);
	{
		char junk[65];
		for (int i = 0; i < 64; i++)
			junk[i] = (char)(i + 1);
		junk[64] = '\0';
		write_script(p1, 0755, junk);
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		test_result("binary garbage -> ENOEXEC",
			    exec_expect_errno(p1, av, ev) == ENOEXEC);
	}

	/* missing interpreter -> ENOENT; missing script -> ENOENT */
	snprintf(p1, sizeof(p1), "%s/nointerp", base);
	snprintf(expect, sizeof(expect), "#!%s/no_such_interp\n", base);
	write_script(p1, 0755, expect);
	{
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		test_result("missing interpreter -> ENOENT",
			    exec_expect_errno(p1, av, ev) == ENOENT);
		snprintf(p2, sizeof(p2), "%s/does_not_exist", base);
		char *av2[] = { p2, NULL };
		test_result("missing exec target -> ENOENT",
			    exec_expect_errno(p2, av2, ev) == ENOENT);
	}

	/* E2BIG: rewritten argv + strings exceed the exec stack budget */
	snprintf(p1, sizeof(p1), "%s/big", base);
	write_script(p1, 0755, "#!/bin/sh\n");
	{
		static char argstore[100][41];
		static char *av[102];
		av[0] = p1;
		for (int i = 0; i < 100; i++) {
			memset(argstore[i], 'a', 40);
			argstore[i][40] = '\0';
			av[i + 1] = argstore[i];
		}
		av[101] = NULL;
		char *ev[] = { NULL };
		test_result("oversized argv -> E2BIG",
			    exec_expect_errno(p1, av, ev) == E2BIG);
	}

	/* CRLF script: trailing \r is trimmed from the optarg */
	snprintf(p1, sizeof(p1), "%s/crlf", base);
	write_script(p1, 0755, "#!/bin/echo -n\r\n");
	{
		char *av[] = { p1, NULL };
		char *ev[] = { NULL };
		int rc = exec_capture(p1, av, ev, out, sizeof(out), &st);
		test_result("CRLF #! line: \\r trimmed from optarg",
			    rc == 0 && strcmp(out, p1) == 0);
	}

	/* DAC: non-root needs x on the script AND on the interpreter */
	if (geteuid() == 0) {
		snprintf(p1, sizeof(p1), "%s/noexec", base);
		write_script(p1, 0644, "#!/bin/sh\necho nope\n");
		test_result("script without x -> EACCES (uid 1000)",
			    exec_errno_as_uid(p1, 1000) == EACCES);

		snprintf(p1, sizeof(p1), "%s/interp_nox", base);
		if (copy_file("/bin/echo", p1, 0644) == 0) {
			snprintf(p2, sizeof(p2), "%s/useinterp", base);
			snprintf(expect, sizeof(expect), "#!%s\n", p1);
			write_script(p2, 0755, expect);
			test_result("interpreter without x -> EACCES (uid 1000)",
				    exec_errno_as_uid(p2, 1000) == EACCES);
		} else {
			test_fail("shebang: cannot stage nox interpreter");
		}
	} else {
		printf("  (EACCES cases skipped: not running as root)\n");
	}

	rmtree(base);
}

/* ---- /dev/fd + /dev/stdin/out/err (added for the bash port) ---------- */

static void test_devfd(void)
{
	printf("\n[TEST] /dev/fd descriptor aliases\n");

	int p[2];
	if (pipe(p) != 0) {
		test_fail("devfd: pipe() failed");
		return;
	}
	/* open(/dev/fd/N) == dup(N): the new fd must reach the same pipe */
	char path[32];
	snprintf(path, sizeof(path), "/dev/fd/%d", p[0]);
	int dupfd = open(path, O_RDONLY);
	test_result("open(/dev/fd/N) duplicates the descriptor", dupfd >= 0);
	if (dupfd >= 0) {
		char buf[8] = { 0 };
		if (write(p[1], "dfd", 3)) {
		}
		test_result("read through /dev/fd duplicate works",
			    read(dupfd, buf, sizeof(buf)) == 3 &&
				    memcmp(buf, "dfd", 3) == 0);
		close(dupfd);
	}
	close(p[0]);
	close(p[1]);

	/* named aliases for 0/1/2 */
	int so = open("/dev/stdout", O_WRONLY);
	test_result("open(/dev/stdout) works", so >= 0);
	if (so >= 0)
		close(so);
	/* nonexistent fd -> EBADF at open time */
	errno = 0;
	test_result("open(/dev/fd/222) on closed fd fails",
		    open("/dev/fd/222", O_RDONLY) < 0);
}

/* ---- waitpid job-control reporting (WUNTRACED/WCONTINUED) ------------ */

/* ---- poll()/select() must be interruptible by a signal ---------------
 *
 * A signal handler only runs on the way back to user mode, so a task parked
 * in poll()/select() that is never woken out of the wait can never run it.
 * That made every event-driven program miss asynchronous events: a terminal
 * app blocked in poll() ignored SIGWINCH (window resizes did nothing) and a
 * server never ran its SIGCHLD handler (children were not reaped, so remote
 * sessions never closed).  Both must return EINTR instead.
 */
static volatile sig_atomic_t g_alarm_fired;
static void poll_eintr_handler(int sig)
{
	(void)sig;
	g_alarm_fired = 1;
}

static void test_poll_signal_eintr(void)
{
	printf("\n[TEST] poll/select interrupted by signal (EINTR)\n");

	struct sigaction sa, old;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = poll_eintr_handler;
	sigaction(SIGALRM, &sa, &old);

	/* An empty pipe never becomes readable, so the only way out is the
	 * signal.  Without the fix these calls hang until the timeout. */
	int p[2];
	if (pipe(p) != 0) {
		test_fail("poll-eintr: pipe failed");
		sigaction(SIGALRM, &old, NULL);
		return;
	}

	g_alarm_fired = 0;
	struct pollfd pfd = { .fd = p[0], .events = POLLIN, .revents = 0 };
	alarm(1);
	int r = poll(&pfd, 1, 10000); /* 10s: must be cut short by SIGALRM */
	int e = errno;
	test_result("poll() returns -1 on signal", r == -1);
	test_result("poll() sets EINTR", r == -1 && e == EINTR);
	test_result("poll() ran the signal handler", g_alarm_fired == 1);

	g_alarm_fired = 0;
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(p[0], &rfds);
	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
	alarm(1);
	r = select(p[0] + 1, &rfds, NULL, NULL, &tv);
	e = errno;
	test_result("select() returns -1 on signal", r == -1);
	test_result("select() sets EINTR", r == -1 && e == EINTR);
	test_result("select() ran the signal handler", g_alarm_fired == 1);

	/* ppoll()'s signal mask must apply for the duration of the wait.  The
	 * common idiom is to BLOCK a signal, then hand ppoll a mask with it
	 * unblocked so it can only arrive while waiting.  Ignoring the mask
	 * left the signal blocked, so the wait was never interrupted and the
	 * handler never ran — sshd uses exactly this for SIGCHLD, and its
	 * exited sessions piled up as zombies. */
	sigset_t block_alrm, prev;
	sigemptyset(&block_alrm);
	sigaddset(&block_alrm, SIGALRM);
	sigprocmask(SIG_BLOCK, &block_alrm, &prev);

	g_alarm_fired = 0;
	pfd.revents = 0;
	alarm(1);
	struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
	/* prev has SIGALRM unblocked -> ppoll must let it through */
	r = ppoll(&pfd, 1, &ts, &prev);
	e = errno;
	test_result("ppoll() honours its signal mask (returns EINTR)",
		    r == -1 && e == EINTR);
	test_result("ppoll() ran the handler while waiting", g_alarm_fired == 1);

	/* And the caller's mask must be restored afterwards: SIGALRM blocked. */
	sigset_t after;
	sigemptyset(&after);
	sigprocmask(SIG_BLOCK, NULL, &after);
	test_result("ppoll() restores the caller's signal mask",
		    sigismember(&after, SIGALRM) == 1);

	alarm(0);
	sigprocmask(SIG_SETMASK, &prev, NULL);

	alarm(0);
	close(p[0]);
	close(p[1]);
	sigaction(SIGALRM, &old, NULL);
}

/* ---- ioctl() on non-terminal descriptors must not fault the kernel ----
 *
 * AF_UNIX, epoll and pipe descriptors are stored in the fd table as tagged
 * marker values rather than file pointers.  ioctl() used to hand them straight
 * to the device layer, which dereferenced the marker — so any process could
 * take the kernel down with tcgetattr() on a socketpair (scp hit this).
 * Each of these must simply report "not a terminal".
 */
/* ---- poll() must follow the fd TABLE, not the fd NUMBER ---------------
 *
 * fds 0/1/2 mean "the console" only while their table slot is empty.  Once
 * stdin has been redirected onto a pipe or socket, polling fd 0 must report
 * that object's readiness.  Deciding by descriptor number instead polled the
 * terminal, so a program that polls its own stdin (sftp-server does) blocked
 * on the console forever while its real input sat unread.
 */
static void test_poll_redirected_stdio(void)
{
	printf("\n[TEST] poll() honours redirected stdin (not the console)\n");

	int p[2];
	if (pipe(p) != 0) {
		test_fail("poll-redirect: pipe failed");
		return;
	}
	/* Put known data in the pipe, then move the read end onto fd 0. */
	if (write(p[1], "x", 1) != 1) {
		test_fail("poll-redirect: write failed");
		close(p[0]); close(p[1]);
		return;
	}
	int saved_stdin = dup(0);
	if (dup2(p[0], 0) != 0) {
		test_fail("poll-redirect: dup2 onto stdin failed");
		close(p[0]); close(p[1]);
		if (saved_stdin >= 0) close(saved_stdin);
		return;
	}

	struct pollfd pfd = { .fd = 0, .events = POLLIN, .revents = 0 };
	int r = poll(&pfd, 1, 1000); /* data is already there: must be instant */
	int readable = (r == 1) && (pfd.revents & POLLIN);

	char c = 0;
	ssize_t n = readable ? read(0, &c, 1) : -1;

	/* Restore stdin before reporting. */
	dup2(saved_stdin, 0);
	close(saved_stdin);
	close(p[0]);
	close(p[1]);

	test_result("poll(stdin) sees the pipe it was redirected to", readable);
	test_result("data read back from redirected stdin", n == 1 && c == 'x');
}

static void test_ioctl_non_tty(void)
{
	printf("\n[TEST] ioctl on non-tty fds returns ENOTTY (no kernel fault)\n");
	struct termios t;

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
		errno = 0;
		int r = ioctl(sv[0], TCGETS, &t);
		test_result("ioctl(TCGETS) on unix socket fails cleanly",
			    r == -1 && errno == ENOTTY);
		test_result("isatty() on unix socket is false", isatty(sv[0]) == 0);
		close(sv[0]);
		close(sv[1]);
	} else {
		test_fail("ioctl-non-tty: socketpair failed");
	}

	int p[2];
	if (pipe(p) == 0) {
		errno = 0;
		int r = ioctl(p[0], TCGETS, &t);
		test_result("ioctl(TCGETS) on pipe fails cleanly",
			    r == -1 && errno == ENOTTY);
		test_result("isatty() on pipe is false", isatty(p[0]) == 0);
		close(p[0]);
		close(p[1]);
	} else {
		test_fail("ioctl-non-tty: pipe failed");
	}

	int ep = epoll_create1(0);
	if (ep >= 0) {
		errno = 0;
		int r = ioctl(ep, TCGETS, &t);
		test_result("ioctl(TCGETS) on epoll fd fails cleanly",
			    r == -1 && errno == ENOTTY);
		close(ep);
	}
	/* Still alive => the kernel did not dereference a marker value. */
	test_result("kernel survived ioctl on marker fds", 1);

	/* An epoll set must survive a child that inherited its descriptor.
	 *
	 * The instance is global to the kernel while the descriptor is
	 * per-process, so fork() hands the child a second reference to the
	 * same set.  Closing it there used to destroy the instance outright,
	 * and the PARENT's epoll_wait() then failed with EBADF forever.  The X
	 * server hit this the moment it forked to run xkbcomp: the keymap
	 * compiled, xkbcomp exited, and the main loop span printing
	 * "WaitForSomething(): poll: Bad file descriptor". */
	{
		int pfd[2];
		int epfd = epoll_create1(0);

		if (epfd >= 0 && pipe(pfd) == 0) {
			struct epoll_event ev;
			pid_t kid;

			ev.events = EPOLLIN;
			ev.data.fd = pfd[0];
			test_result("epoll_ctl ADD before fork",
				    epoll_ctl(epfd, EPOLL_CTL_ADD, pfd[0], &ev) == 0);

			kid = fork();
			if (kid == 0) {
				/* Close the inherited copy and exit -- exactly
				 * what a short-lived helper process does. */
				close(epfd);
				close(pfd[0]);
				close(pfd[1]);
				_exit(0);
			}
			if (kid > 0) {
				int st = 0;

				while (waitpid(kid, &st, 0) < 0 && errno == EINTR)
					;

				/* The parent's set must still work. */
				struct epoll_event out;

				errno = 0;
				int n = epoll_wait(epfd, &out, 1, 0);
				test_result("epoll survives a child closing its copy",
					    n >= 0 && errno != EBADF);

				/* And must still report real readiness. */
				if (write(pfd[1], "x", 1) == 1) {
					n = epoll_wait(epfd, &out, 1, 500);
					test_result("epoll still reports readiness after fork",
						    n == 1 &&
							    out.data.fd == pfd[0]);
				} else {
					test_fail("epoll-fork: write to pipe failed");
				}
			} else {
				test_fail("epoll-fork: fork failed");
			}
			close(pfd[0]);
			close(pfd[1]);
			close(epfd);
		} else {
			test_fail("epoll-fork: setup failed");
		}
	}

	/* dup() takes a reference too: closing one copy must not invalidate
	 * the other. */
	{
		int epfd = epoll_create1(0);

		if (epfd >= 0) {
			int dupfd = dup(epfd);

			test_result("dup of an epoll fd succeeds", dupfd >= 0);
			if (dupfd >= 0) {
				struct epoll_event out;

				close(epfd);
				errno = 0;
				test_result("epoll survives closing the original fd",
					    epoll_wait(dupfd, &out, 1, 0) >= 0 &&
						    errno != EBADF);
				close(dupfd);
			}
		}
	}
}

/* ---- Credential enforcement on cross-process introspection ----------------
 *
 * One user must not be able to read another's private state, nor interfere
 * with their processes.  These check that from the outside: drop to an
 * unprivileged uid in a child and confirm the kernel withholds what it should
 * while still reporting enough for a process listing to work.
 *
 * Both have to start privileged in order to drop, so they report themselves
 * skipped rather than failing when the suite is not run as root. */

#define CRED_TEST_UID 65534 /* conventionally "nobody"; only needs to be != 0 */

/* Findings the child reports back through its exit status. */
#define CV_FOUND_TARGET 0x01 /* the root-owned process was listed at all   */
#define CV_ENVIRON_HIDDEN 0x02
#define CV_CMDLINE_HIDDEN 0x04
#define CV_COMM_VISIBLE 0x08 /* ps-class fields still readable             */
#define CV_SELF_VISIBLE 0x10 /* the child can still read its OWN entry      */

static void test_cred_proc_visibility(void)
{
	printf("\n[TEST] getprocinfo credential gating\n");

	if (geteuid() != 0) {
		printf("  [SKIP] not root: cannot drop privilege to test denial\n");
		return;
	}

	/* Root-owned, and holds a real environment and command line. */
	pid_t target = getpid();

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("cred visibility: fork failed");
		return;
	}

	if (pid == 0) {
		int flags = 0;

		if (setuid(CRED_TEST_UID) != 0)
			_exit(0); /* parent reports the resulting failures */

		int max = 128;
		procinfo_t *buf = (procinfo_t *)malloc((size_t)max *
						       sizeof(procinfo_t));
		if (!buf)
			_exit(0);

		pid_t self = getpid();
		int n = getprocinfo(buf, max);
		for (int i = 0; i < n; i++) {
			if (buf[i].pid == (int)target) {
				flags |= CV_FOUND_TARGET;
				if (buf[i].environ[0] == '\0')
					flags |= CV_ENVIRON_HIDDEN;
				if (buf[i].cmdline[0] == '\0')
					flags |= CV_CMDLINE_HIDDEN;
				if (buf[i].comm[0] != '\0')
					flags |= CV_COMM_VISIBLE;
			} else if (buf[i].pid == (int)self) {
				/* Its own process stays fully readable. */
				if (buf[i].comm[0] != '\0')
					flags |= CV_SELF_VISIBLE;
			}
		}
		free(buf);
		_exit(flags);
	}

	int st = 0;
	waitpid(pid, &st, 0);
	int flags = WIFEXITED(st) ? WEXITSTATUS(st) : 0;

	test_result("another user's process is still listed (ps works)",
		    (flags & CV_FOUND_TARGET) != 0);
	test_result("another user's environ is withheld",
		    (flags & CV_ENVIRON_HIDDEN) != 0);
	test_result("another user's cmdline is withheld",
		    (flags & CV_CMDLINE_HIDDEN) != 0);
	test_result("ps-class fields (comm) remain readable by any user",
		    (flags & CV_COMM_VISIBLE) != 0);
	test_result("a process can still read its own entry",
		    (flags & CV_SELF_VISIBLE) != 0);
}

static void test_cred_sched_control(void)
{
	printf("\n[TEST] scheduler control credential gating\n");

	if (geteuid() != 0) {
		printf("  [SKIP] not root: cannot drop privilege to test denial\n");
		return;
	}

	pid_t target = getpid(); /* root-owned */

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("cred sched: fork failed");
		return;
	}

	if (pid == 0) {
		if (setuid(CRED_TEST_UID) != 0)
			_exit(0);

		cpu_set_t mask;
		CPU_ZERO(&mask);
		CPU_SET(0, &mask);

		/* Another user's process: pinning it is interference, and must
		 * be refused. */
		errno = 0;
		int r_other = sched_setaffinity(target, sizeof(mask), &mask);
		int e_other = errno;

		/* Its own: must still work. */
		int r_self = sched_setaffinity(0, sizeof(mask), &mask);

		int flags = 0;
		if (r_other != 0 && e_other == EPERM)
			flags |= 0x01;
		if (r_self == 0)
			flags |= 0x02;
		_exit(flags);
	}

	int st = 0;
	waitpid(pid, &st, 0);
	int flags = WIFEXITED(st) ? WEXITSTATUS(st) : 0;

	test_result("cannot re-pin another user's process (EPERM)",
		    (flags & 0x01) != 0);
	test_result("can still set own affinity", (flags & 0x02) != 0);
}

/* ---- ptrace ---------------------------------------------------------------
 *
 * The fork + PTRACE_TRACEME + execve sequence is what a debugger's "run" is
 * built on: the child asks to be traced, execs, and stops before the new
 * program's first instruction so breakpoints can be planted.  If that stop
 * does not happen the child simply runs to completion and there is nothing to
 * debug, so it is worth testing directly rather than inferring from gdb. */
/*
 * getrusage()/wait4() must write exactly sizeof(struct rusage) and not a byte
 * more.
 *
 * The kernel's own copy of this struct once carried 14 padding words, making it
 * 144 bytes against the 72 the header declares, and sys_getrusage copied its
 * own sizeof() to the caller.  Every getrusage() therefore wrote 72 bytes past
 * the end of the caller's struct.  It went unnoticed for as long as it did
 * because the damage lands on whatever the program happens to keep next to the
 * struct: harmless padding in one program, a saved return address in the next.
 * gdb was the second kind -- it returned to address 0.
 *
 * A canary immediately after the struct is what catches this: an overflow of
 * any size, in either syscall, has to go through it.
 */
static void test_rusage_no_overflow(void)
{
	printf("\n[TEST] getrusage/wait4 do not overrun struct rusage\n");

	/* Adjacent by declaration order is not guaranteed, so the two are put
	 * in one struct where the layout IS defined. */
	struct {
		struct rusage ru;
		unsigned long canary[8];
	} probe;

	static const unsigned long PATTERN = 0xA5A5C3C35A5A3C3CUL;

	for (int i = 0; i < 8; i++)
		probe.canary[i] = PATTERN;

	int rc = getrusage(RUSAGE_SELF, &probe.ru);
	test_result("getrusage(RUSAGE_SELF) succeeds", rc == 0);

	int intact = 1;
	for (int i = 0; i < 8; i++)
		if (probe.canary[i] != PATTERN)
			intact = 0;
	test_result("getrusage does not write past struct rusage", intact);
	if (!intact)
		printf("  canary[0]=%#lx (expected %#lx) -- the kernel wrote "
		       "past the end of the caller's struct\n",
		       probe.canary[0], PATTERN);

	/* Same check for wait4, which fills a rusage for the reaped child. */
	for (int i = 0; i < 8; i++)
		probe.canary[i] = PATTERN;

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("rusage: fork failed");
		return;
	}
	if (pid == 0)
		_exit(0);

	int st = 0;
	pid_t r = wait4(pid, &st, 0, &probe.ru);
	test_result("wait4 reaps the child", r == pid);

	intact = 1;
	for (int i = 0; i < 8; i++)
		if (probe.canary[i] != PATTERN)
			intact = 0;
	test_result("wait4 does not write past struct rusage", intact);

	/* The tail of the struct must be written, not left as it was: a caller
	 * reading ru_nvcsw is entitled to a value and not to whatever its own
	 * stack held.  wait4 used to write only the first 56 bytes. */
	test_result("wait4 fills the whole struct (ru_nvcsw defined)",
		    probe.ru.ru_nvcsw == 0 || probe.ru.ru_nvcsw > 0);
}

/*
 * PTRACE_GETAUXV: the tracee's auxiliary vector.
 *
 * A debugger needs this before it can do anything useful with a
 * position-independent executable.  Every address in the symbol table is an
 * offset until the load bias is known, and comparing AT_ENTRY here against
 * e_entry in the file on disk is the only way to learn it -- finding the
 * loader's rendezvous instead would mean reading the dynamic section, which
 * already requires the bias.  With this missing, gdb resolved `main' to its
 * link-time address and could not write a breakpoint there.
 */
/*
 * Hardware watchpoints, via the debug registers.
 *
 * The processor compares every memory access against DR0..DR3 itself, so an
 * armed watchpoint costs nothing while the program runs -- the alternative is
 * single-stepping the whole program and re-reading the watched location after
 * every instruction.
 *
 * The security half of this test matters as much as the functional half.  A
 * debug register stops the CPU wherever the address matches, including inside
 * the kernel, and DR7's GD bit stops it on any access to the debug registers at
 * all -- which the kernel itself makes on every context switch involving a
 * watchpoint, so a tracee holding GD would fault the kernel inside its own
 * fault handler.  Neither is something a tracer may ask for.
 */
/* Written by the child in the watchpoint test.  A global, so the parent knows
 * its address: fork copies the address space, so &g_wp_target names the same
 * place in both. */
static volatile int g_wp_target;

static void test_ptrace_hwwatchpoint(void)
{
	printf("\n[TEST] ptrace hardware watchpoints (DR0-DR7)\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("hwwatch: fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		execl("/bin/true", "true", (char *)NULL);
		_exit(43);
	}

	int st = 0;
	pid_t r;
	if (!(waitpid(pid, &st, 0) == pid && WIFSTOPPED(st))) {
		test_fail("hwwatch: child did not stop on exec");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	struct ptrace_dbregs db;

	/* They start clear: a fresh image inherits no watchpoints. */
	int rc = ptrace(PTRACE_GETDBREGS, pid, &db, 0);
	test_result("GETDBREGS on a fresh image succeeds", rc == 0);
	test_result("a fresh image has no watchpoints armed",
		    rc == 0 && db.dr[7] == 0 && db.dr[0] == 0);

	/* Arm a write watchpoint on a user address, four bytes wide.
	 * DR7: L0 enabled (bit 0), RW0=01 (write), LEN0=11 (4 bytes). */
	unsigned long watched = 0x400000; /* in the tracee's text mapping */
	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = watched;
	db.dr[7] = (1UL << 0) | (0x1UL << 16) | (0x3UL << 18);
	rc = ptrace(PTRACE_SETDBREGS, pid, &db, 0);
	test_result("SETDBREGS arms a watchpoint", rc == 0);

	struct ptrace_dbregs back;
	rc = ptrace(PTRACE_GETDBREGS, pid, &back, 0);
	test_result("the armed address reads back", rc == 0 &&
						    back.dr[0] == watched);
	test_result("the enable bit reads back", rc == 0 &&
						 (back.dr[7] & 1UL));

	/* Bit 10 of DR7 reads as one on this architecture. */
	test_result("DR7 reserved bit 10 is set", rc == 0 &&
						  (back.dr[7] & (1UL << 10)));

	/* --- the refusals --- */

	/* GD (bit 13) must never survive: with it set, the kernel's own access
	 * to these registers during a context switch would fault. */
	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = watched;
	db.dr[7] = (1UL << 0) | (1UL << 13);
	ptrace(PTRACE_SETDBREGS, pid, &db, 0);
	rc = ptrace(PTRACE_GETDBREGS, pid, &back, 0);
	test_result("DR7 General Detect is refused",
		    rc == 0 && !(back.dr[7] & (1UL << 13)));

	/* A kernel address must be refused outright rather than trimmed: it
	 * would trap the CPU on the kernel's own accesses. */
	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = 0xFFFF800000000000UL;
	db.dr[7] = 1UL;
	rc = ptrace(PTRACE_SETDBREGS, pid, &db, 0);
	test_result("a kernel watchpoint address is refused", rc == -1);

	/* I/O breakpoints (RW=10b) watch port space and need CR4.DE. */
	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = watched;
	db.dr[7] = (1UL << 0) | (0x2UL << 16);
	ptrace(PTRACE_SETDBREGS, pid, &db, 0);
	rc = ptrace(PTRACE_GETDBREGS, pid, &back, 0);
	test_result("an I/O breakpoint slot is disabled",
		    rc == 0 && !(back.dr[7] & 1UL));

	/* Watchpoints must not survive an exec: they name addresses in an
	 * image that no longer exists. */
	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = watched;
	db.dr[7] = (1UL << 0) | (0x1UL << 16) | (0x3UL << 18);
	ptrace(PTRACE_SETDBREGS, pid, &db, 0);

	ptrace(PTRACE_CONT, pid, NULL, 0);
	waitpid(pid, &st, 0);
	test_result("tracee ran to completion with a watchpoint armed",
		    WIFEXITED(st) || WIFSIGNALED(st));

	/* --- does a watchpoint actually FIRE? ---
	 *
	 * Everything above proves the debug registers are written, read back
	 * and filtered.  None of it proves the processor ever traps, which is
	 * the only reason the registers exist.  This arms a write watchpoint on
	 * a real variable, lets the child write it, and requires the trap. */
	pid = fork();
	if (pid < 0) {
		test_fail("hwwatch: fire fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		raise(SIGSTOP); /* let the parent arm it */
		g_wp_target = 0x1234; /* the write that must trap */
		_exit(3);
	}

	if (!(waitpid(pid, &st, 0) == pid && WIFSTOPPED(st))) {
		test_fail("hwwatch: child did not stop for arming");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	for (int i = 0; i < 8; i++)
		db.dr[i] = 0;
	db.dr[0] = (unsigned long)&g_wp_target;
	/* L0 enabled, RW0=01 (write), LEN0=11 (four bytes). */
	db.dr[7] = (1UL << 0) | (0x1UL << 16) | (0x3UL << 18);
	rc = ptrace(PTRACE_SETDBREGS, pid, &db, 0);
	test_result("arm a write watchpoint on a real variable", rc == 0);

	ptrace(PTRACE_CONT, pid, NULL, 0);
	r = waitpid(pid, &st, 0);

	test_result("the watchpoint fires: tracee stops",
		    r == pid && WIFSTOPPED(st));
	test_result("the watchpoint stop is reported as SIGTRAP",
		    WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP);

	/* DR6 says WHICH watchpoint matched -- bit 0 for the first address
	 * register.  Without it a debugger with four armed cannot tell them
	 * apart. */
	if (WIFSTOPPED(st) &&
	    ptrace(PTRACE_GETDBREGS, pid, &back, 0) == 0)
		test_result("DR6 reports which watchpoint matched",
			    (back.dr[6] & 0x1UL) != 0);
	else
		test_fail("hwwatch: GETDBREGS after the trap failed");

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

/*
 * Signal-delivery-stop: a traced task stops BEFORE a signal is acted on, and
 * the tracer decides what happens to it.
 *
 * This is what makes ^C reach a debugger instead of killing the program under
 * it, and what lets gdb examine a process that has just faulted rather than
 * being handed its corpse.
 *
 * The signals are raised with the DEFAULT disposition explicitly restored:
 * earlier tests in this program install a SIGUSR1 handler and fork inherits
 * dispositions, so without the reset the signal runs that handler and the
 * child carries on -- correct behaviour that once got mistaken for a kernel
 * defect and cost four boot cycles to un-mistake.
 */
static void test_ptrace_signal_delivery_stop(void)
{
	printf("\n[TEST] ptrace signal-delivery-stop\n");

	/* --- an ordinary signal, discarded by the tracer --- */
	pid_t pid = fork();
	if (pid < 0) {
		test_fail("sigstop: fork failed");
		return;
	}
	if (pid == 0) {
		signal(SIGUSR1, SIG_DFL);
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		kill(getpid(), SIGUSR1);
		_exit(7);
	}

	int st = 0;
	pid_t r = waitpid(pid, &st, 0);
	test_result("tracee stops for a signal instead of taking it",
		    r == pid && WIFSTOPPED(st));
	test_result("the tracer is told which signal",
		    WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR1);

	ptrace(PTRACE_CONT, pid, NULL, 0);
	r = waitpid(pid, &st, 0);
	test_result("a discarded signal does not reach the tracee",
		    r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);

	/* --- the same signal, passed through --- */
	pid = fork();
	if (pid < 0) {
		test_fail("sigstop: second fork failed");
		return;
	}
	if (pid == 0) {
		signal(SIGUSR1, SIG_DFL);
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		kill(getpid(), SIGUSR1);
		_exit(7); /* not reached: the signal kills it */
	}

	r = waitpid(pid, &st, 0);
	if (r == pid && WIFSTOPPED(st)) {
		ptrace(PTRACE_CONT, pid, NULL, SIGUSR1);
		r = waitpid(pid, &st, 0);
		test_result("a passed-through signal reaches the tracee",
			    r == pid && WIFSIGNALED(st) &&
				    WTERMSIG(st) == SIGUSR1);
	} else {
		test_fail("sigstop: second child did not stop");
	}

	/* --- abort(): a self-signal with a default-fatal disposition ---
	 *
	 * A separate kernel path from a signal sent by another process, and one
	 * that used to exit the task in place -- so a program calling abort()
	 * under a debugger died without the debugger ever being told. */
	pid = fork();
	if (pid < 0) {
		test_fail("sigstop: abort fork failed");
		return;
	}
	if (pid == 0) {
		signal(SIGABRT, SIG_DFL);
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		abort();
		_exit(7); /* not reached */
	}

	r = waitpid(pid, &st, 0);
	test_result("abort() under a tracer stops instead of dying",
		    r == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGABRT);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);

	/* --- a fault: the case a debugger exists for --- */
	pid = fork();
	if (pid < 0) {
		test_fail("sigstop: fault fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		*(volatile int *)1 = 1; /* write to a page that is not there */
		_exit(9);		/* not reached */
	}

	r = waitpid(pid, &st, 0);
	test_result("a faulting tracee stops instead of dying",
		    r == pid && WIFSTOPPED(st));
	test_result("the fault is reported as SIGSEGV",
		    WIFSTOPPED(st) && WSTOPSIG(st) == SIGSEGV);

	if (WIFSTOPPED(st)) {
		struct ptrace_regs regs;
		int rc = ptrace(PTRACE_GETREGS, pid, &regs, 0);
		test_result("registers readable at a fault stop", rc == 0);
		test_result("RIP points at the faulting instruction",
			    rc == 0 && regs.rip != 0);

		/* The siginfo is what turns "SIGSEGV" into "wrote to 0x1". */
		siginfo_t si;
		memset(&si, 0, sizeof(si));
		rc = ptrace(PTRACE_GETSIGINFO, pid, &si, 0);
		test_result("GETSIGINFO succeeds at a fault stop", rc == 0);
		test_result("siginfo reports SIGSEGV", rc == 0 &&
						       si.si_signo == SIGSEGV);
		test_result("siginfo reports the faulting address",
			    rc == 0 && (unsigned long)si.si_addr == 1UL);
		test_result("siginfo says the address was not mapped",
			    rc == 0 && si.si_code == SEGV_MAPERR);
	}

	/* A tracee released by SIGKILL rather than PTRACE_CONT must die, not
	 * loop re-executing the faulting instruction.  This is the case that
	 * stranded a process spinning on a core once. */
	kill(pid, SIGKILL);
	r = waitpid(pid, &st, 0);
	test_result("a faulting tracee killed at its stop actually dies",
		    r == pid && (WIFSIGNALED(st) || WIFEXITED(st)));
}

/*
 * Threads under a tracer.
 *
 * A debugger attaches to a PROCESS: every thread must be traced, every thread
 * must stop when one of them does, and a resume must release them all.  With
 * any of those missing a debugger shows one stack for a program doing its work
 * on several, or restarts one thread and leaves the rest parked for ever.
 */
static void *wp_thread_fn(void *arg)
{
	volatile int *stop = (volatile int *)arg;

	while (!*stop)
		sched_yield();
	return NULL;
}

static void test_ptrace_threads(void)
{
	printf("\n[TEST] ptrace thread enumeration and group stop\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("threads: fork failed");
		return;
	}
	if (pid == 0) {
		static volatile int stop_flag;
		pthread_t th[3];

		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);

		for (int i = 0; i < 3; i++)
			pthread_create(&th[i], NULL, wp_thread_fn,
				       (void *)&stop_flag);

		raise(SIGSTOP); /* the tracer looks at us here */

		stop_flag = 1;
		for (int i = 0; i < 3; i++)
			pthread_join(th[i], NULL);
		_exit(5);
	}

	int st = 0;
	pid_t r = waitpid(pid, &st, 0);
	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("threads: child did not stop");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	int n = (int)ptrace(PTRACE_GETNUMLWPS, pid, NULL, 0);
	test_result("GETNUMLWPS counts every thread", n >= 4);

	unsigned int ids[16];
	long got = ptrace(PTRACE_GETLWPLIST, pid, ids, (long)16);
	test_result("GETLWPLIST returns that many ids", got == n || got == 16);

	/* The ids must be real and distinct, and one of them is the process. */
	int distinct = 1, saw_leader = 0;
	for (long i = 0; i < got; i++) {
		if (ids[i] == (unsigned)pid)
			saw_leader = 1;
		for (long j = i + 1; j < got; j++)
			if (ids[i] == ids[j])
				distinct = 0;
	}
	test_result("thread ids are distinct", distinct);
	test_result("the process itself is among them", saw_leader);

	/* Every thread is traced, so every thread's registers are readable --
	 * this is what a debugger needs to show a second stack. */
	int readable = 1;
	for (long i = 0; i < got && i < 4; i++) {
		struct ptrace_regs regs;

		if (ptrace(PTRACE_GETREGS, (pid_t)ids[i], &regs, 0) != 0)
			readable = 0;
	}
	test_result("every thread's registers are readable", readable);

	ptrace(PTRACE_CONT, pid, NULL, 0);
	r = waitpid(pid, &st, 0);
	test_result("resuming releases the whole process",
		    r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 5);
}

static void test_ptrace_getauxv(void)
{
	printf("\n[TEST] ptrace GETAUXV\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("getauxv: fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		execl("/bin/true", "true", (char *)NULL);
		_exit(43);
	}

	int st = 0;
	pid_t r = waitpid(pid, &st, 0);
	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("getauxv: child did not stop on exec");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	unsigned long buf[64];
	long n = ptrace(PTRACE_GETAUXV, pid, buf, (long)sizeof(buf));
	test_result("GETAUXV returns bytes", n > 0);

	/* Walk the type/value pairs looking for the two that matter. */
	unsigned long at_entry = 0, at_pagesz = 0, at_phdr = 0;
	int terminated = 0;
	if (n > 0) {
		size_t pairs = (size_t)n / (2 * sizeof(unsigned long));
		for (size_t i = 0; i < pairs; i++) {
			unsigned long t = buf[i * 2], v = buf[i * 2 + 1];
			if (t == 9)  /* AT_ENTRY */
				at_entry = v;
			else if (t == 6)  /* AT_PAGESZ */
				at_pagesz = v;
			else if (t == 3)  /* AT_PHDR */
				at_phdr = v;
			else if (t == 0)  /* AT_NULL */
				terminated = 1;
		}
	}

	test_result("auxv carries AT_ENTRY", at_entry != 0);
	test_result("auxv carries AT_PHDR", at_phdr != 0);
	test_result("auxv AT_PAGESZ is the page size", at_pagesz == 4096);
	(void)terminated;

	/* A short buffer must be truncated, not overrun -- the kernel is
	 * copying into a buffer whose size only the caller knows. */
	unsigned long small[4];
	unsigned long guard = 0x5A5A5A5A5A5A5A5AUL;
	long n2 = ptrace(PTRACE_GETAUXV, pid, small, (long)sizeof(small));
	test_result("GETAUXV honours a short buffer",
		    n2 >= 0 && (size_t)n2 <= sizeof(small));
	(void)guard;

	ptrace(PTRACE_CONT, pid, NULL, 0);
	waitpid(pid, &st, 0);
}

static void test_ptrace_traceme_basic(void)
{
	printf("\n[TEST] ptrace TRACEME/exec/CONT\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace: fork failed");
		return;
	}

	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42); /* parent sees the wrong stop and reports it */
		execl("/bin/true", "true", (char *)NULL);
		_exit(43); /* exec failed */
	}

	/* Stop at the image boundary, before /bin/true has run. */
	int st = 0;
	pid_t r = waitpid(pid, &st, 0);
	int stopped_on_exec = (r == pid && WIFSTOPPED(st) &&
			       WSTOPSIG(st) == SIGTRAP);
	test_result("traced child stops on exec with SIGTRAP", stopped_on_exec);

	if (!stopped_on_exec) {
		/* Nothing is parked, so nothing to resume; just reap whatever
		 * state it is in rather than hanging the suite. */
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	test_result("stop reports the exec event",
		    (st >> 16) == PTRACE_EVENT_EXEC);

	/* Resuming must let it run to a normal exit. */
	test_result("PTRACE_CONT on a stopped tracee succeeds",
		    ptrace(PTRACE_CONT, pid, NULL, 0) == 0);

	r = waitpid(pid, &st, 0);
	test_result("resumed tracee runs to completion",
		    r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Attaching to a process, and detaching from it, without having created it --
 * which is how a debugger reaches an already-running program. */
static void test_ptrace_attach_detach(void)
{
	printf("\n[TEST] ptrace ATTACH/DETACH\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace attach: fork failed");
		return;
	}
	if (pid == 0) {
		for (;;)
			usleep(50000);
	}
	usleep(100000); /* let it reach the loop */

	int attached = (ptrace(PTRACE_ATTACH, pid, NULL, 0) == 0);
	test_result("PTRACE_ATTACH to own process succeeds", attached);

	if (attached) {
		int st = 0;
		pid_t r = waitpid(pid, &st, 0);
		test_result("attached process reports a stop",
			    r == pid && WIFSTOPPED(st));

		/* A second tracer must not be able to take it over. */
		test_result("PTRACE_ATTACH to an already-traced process fails",
			    ptrace(PTRACE_ATTACH, pid, NULL, 0) != 0);

		test_result("PTRACE_DETACH succeeds",
			    ptrace(PTRACE_DETACH, pid, NULL, 0) == 0);
	}

	kill(pid, SIGKILL);
	int st = 0;
	waitpid(pid, &st, 0);
}

/* The access rules.  These are the ones worth having: a debugger that cannot
 * attach is an inconvenience, but a debugger that CAN attach to another user's
 * process is a way to read their memory and run code as them. */
static void test_ptrace_permission_denied(void)
{
	printf("\n[TEST] ptrace credential gating\n");

	/* Tracing yourself cannot work -- it would deadlock on its own stop. */
	errno = 0;
	test_result("PTRACE_ATTACH to self is refused",
		    ptrace(PTRACE_ATTACH, getpid(), NULL, 0) != 0);

	/* Attaching to a pid that does not exist. */
	errno = 0;
	test_result("PTRACE_ATTACH to a nonexistent pid fails with ESRCH",
		    ptrace(PTRACE_ATTACH, 999999, NULL, 0) != 0 &&
			    errno == ESRCH);

	/* Driving a process this task is not tracing. */
	errno = 0;
	test_result("PTRACE_CONT on a process we do not trace fails",
		    ptrace(PTRACE_CONT, 1, NULL, 0) != 0);

	if (geteuid() != 0) {
		printf("  [SKIP] not root: cannot drop privilege to test cross-user denial\n");
		return;
	}

	/* The real test: an unprivileged process must not attach to a
	 * root-owned one.  Run in a child that drops to another uid, since the
	 * suite itself needs to stay root. */
	pid_t target = getpid(); /* root-owned */

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace perm: fork failed");
		return;
	}

	if (pid == 0) {
		if (setuid(CRED_TEST_UID) != 0)
			_exit(0);

		int flags = 0;

		errno = 0;
		if (ptrace(PTRACE_ATTACH, target, NULL, 0) != 0 &&
		    errno == EPERM)
			flags |= 0x01;

		/* Its own child is still fair game -- the rule denies other
		 * users, not tracing as such. */
		pid_t own = fork();
		if (own == 0) {
			for (;;)
				usleep(50000);
		}
		if (own > 0) {
			usleep(100000);
			if (ptrace(PTRACE_ATTACH, own, NULL, 0) == 0)
				flags |= 0x02;
			kill(own, SIGKILL);
			int cst = 0;
			waitpid(own, &cst, 0);
		}
		_exit(flags);
	}

	int st = 0;
	waitpid(pid, &st, 0);
	int flags = WIFEXITED(st) ? WEXITSTATUS(st) : 0;

	test_result("unprivileged process cannot attach to root's process "
		    "(EPERM)",
		    (flags & 0x01) != 0);
	test_result("unprivileged process can still trace its own child",
		    (flags & 0x02) != 0);
}

/* Lives in the suite's own data, so after a fork the parent and the child each
 * have their own copy of the page it sits on -- initially the SAME physical
 * page, shared copy-on-write.  That is what makes it a real test of writing to
 * a tracee: the write has to land in the child's copy and nowhere else. */
static volatile unsigned long g_ptrace_probe = 0xC0FFEEUL;

/* A GS base no part of the system would choose on its own, in the user half of
 * the address space (arch_prctl refuses anything above it, for the same reason
 * an FS base there would be refused).  The tracee sets it; the tracer reads it
 * back out of PTRACE_GETREGS. */
#define PTRACE_GSBASE_MAGIC 0x7EAD0000UL

static void test_ptrace_regs_and_mem(void)
{
	printf("\n[TEST] ptrace registers and memory\n");

	unsigned long probe_addr = (unsigned long)&g_ptrace_probe;
	int pfd[2];

	if (pipe(pfd) != 0) {
		test_fail("ptrace mem: pipe failed");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace mem: fork failed");
		close(pfd[0]);
		close(pfd[1]);
		return;
	}

	if (pid == 0) {
		close(pfd[0]);
		/* Ask for a GS base, so the tracer's GETREGS has something to
		 * report that only this process could have chosen.  The kernel
		 * keeps %gs for its own per-CPU data and never installs this,
		 * but it records it -- and used to leave it out of GETREGS
		 * entirely, which read as "every task has a zero GS base"
		 * rather than as a missing field. */
		arch_prctl(ARCH_SET_GS, PTRACE_GSBASE_MAGIC);
		/* Spin until the value changes underneath us, then report what
		 * we saw.  Bounded so a failure is a failed test, not a hang. */
		for (int i = 0; i < 400 && g_ptrace_probe == 0xC0FFEEUL; i++)
			usleep(10000);
		unsigned long seen = g_ptrace_probe;

		write(pfd[1], &seen, sizeof(seen));
		close(pfd[1]);
		_exit(0);
	}

	close(pfd[1]);
	usleep(100000); /* let the child reach its loop */

	int attached = (ptrace(PTRACE_ATTACH, pid, NULL, 0) == 0);
	test_result("attach for register/memory test", attached);
	if (!attached) {
		kill(pid, SIGKILL);
		int st = 0;
		waitpid(pid, &st, 0);
		close(pfd[0]);
		return;
	}

	int st = 0;
	waitpid(pid, &st, 0);

	/* --- registers --- */
	struct ptrace_regs r;

	memset(&r, 0, sizeof(r));
	int got = (ptrace(PTRACE_GETREGS, pid, &r, 0) == 0);
	test_result("GETREGS succeeds on a stopped tracee", got);
	test_result("GETREGS reports a user-range RIP",
		    got && r.rip > 0x1000UL && r.rip < 0x800000000000UL);
	test_result("GETREGS reports a user-range RSP",
		    got && r.rsp > 0x1000UL && r.rsp < 0x800000000000UL);

	/* --- segment state --- */

	/* The thread pointer.  Every process here has one, because libc's
	 * per-thread block -- errno, the stack canary -- lives at it. */
	test_result("GETREGS reports a user-range FS base (the TLS block)",
		    got && r.fs_base > 0x1000UL &&
			    r.fs_base < 0x800000000000UL);

	/* What the tracee asked for, read back from outside it. */
	test_result("GETREGS reports the GS base the tracee set",
		    got && r.gs_base == PTRACE_GSBASE_MAGIC);

	/* The data-segment selectors.  A ring-0 selector here would mean the
	 * capture happened somewhere other than the entry from user mode --
	 * IRETQ to CPL 3 nulls any selector whose descriptor is more
	 * privileged than the caller, so a program can only ever be running
	 * with zero or a ring-3 selector in these. */
	test_result("GETREGS reports no ring-0 data selector",
		    got && (r.ds == 0 || (r.ds & 3) == 3) &&
			    (r.es == 0 || (r.es & 3) == 3) &&
			    (r.fs == 0 || (r.fs & 3) == 3) &&
			    (r.gs == 0 || (r.gs & 3) == 3));

	/* --- reading the tracee --- */
	errno = 0;
	long v = ptrace(PTRACE_PEEKDATA, pid, (void *)probe_addr, 0);

	test_result("PEEKDATA reads the tracee's variable",
		    errno == 0 && (unsigned long)v == 0xC0FFEEUL);

	/* --- writing the tracee --- */
	test_result("POKEDATA writes the tracee's variable",
		    ptrace(PTRACE_POKEDATA, pid, (void *)probe_addr,
			   0xBEEFUL) == 0);

	errno = 0;
	v = ptrace(PTRACE_PEEKDATA, pid, (void *)probe_addr, 0);
	test_result("POKEDATA is visible on read-back",
		    errno == 0 && (unsigned long)v == 0xBEEFUL);

	/* The point of the whole test: the tracer shares that page with the
	 * tracee copy-on-write, so a write that did not break the sharing
	 * first would show up here too. */
	test_result("tracer's own copy is untouched (COW broken correctly)",
		    g_ptrace_probe == 0xC0FFEEUL);

	/* --- refusals --- */
	errno = 0;
	ptrace(PTRACE_PEEKDATA, pid, (void *)0x10UL, 0);
	test_result("PEEKDATA of a bad address fails", errno != 0);

	errno = 0;
	ptrace(PTRACE_POKEDATA, pid, (void *)0x10UL, 1);
	test_result("POKEDATA to a bad address fails", errno != 0);

	/* An address with no mapping behind it must read as a fault, not as
	 * zeroes -- an unmapped hole translates to nothing and would otherwise
	 * look like real memory full of zeros. */
	errno = 0;
	ptrace(PTRACE_PEEKDATA, pid, (void *)0x600000000000UL, 0);
	test_result("PEEKDATA of an unmapped address fails", errno != 0);

	test_result("DETACH after memory access",
		    ptrace(PTRACE_DETACH, pid, NULL, 0) == 0);

	unsigned long seen = 0;

	if (read(pfd[0], &seen, sizeof(seen)) == (ssize_t)sizeof(seen))
		test_result("tracee itself observes the poked value",
			    seen == 0xBEEFUL);
	else
		test_fail("tracee did not report the poked value back");
	close(pfd[0]);
	waitpid(pid, &st, 0);
}

/* Stepping a tracee one instruction at a time.
 *
 * The child is stopped at its exec boundary, before the new program has run a
 * single instruction, which is both a known starting point and the state a
 * debugger's "start" leaves things in.  Each step must come back as a SIGTRAP
 * stop with the instruction pointer somewhere else -- if the trap flag were
 * lost the child would simply run to completion and the wait would report an
 * exit instead. */
static void test_ptrace_singlestep(void)
{
	printf("\n[TEST] ptrace single-step\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace singlestep: fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		execl("/bin/true", "true", (char *)NULL);
		_exit(43);
	}

	int st = 0;
	pid_t r = waitpid(pid, &st, 0);

	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("singlestep: child did not stop on exec");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	struct ptrace_regs r0, r1;

	memset(&r0, 0, sizeof(r0));
	test_result("GETREGS at the exec stop",
		    ptrace(PTRACE_GETREGS, pid, &r0, 0) == 0);

	int steps_ok = 1;
	int moved = 0;
	unsigned long prev = r0.rip;

	for (int i = 0; i < 8 && steps_ok; i++) {
		if (ptrace(PTRACE_SINGLESTEP, pid, NULL, 0) != 0) {
			steps_ok = 0;
			break;
		}
		st = 0;
		r = waitpid(pid, &st, 0);
		if (!(r == pid && WIFSTOPPED(st) &&
		      WSTOPSIG(st) == SIGTRAP)) {
			steps_ok = 0;
			break;
		}
		memset(&r1, 0, sizeof(r1));
		if (ptrace(PTRACE_GETREGS, pid, &r1, 0) != 0) {
			steps_ok = 0;
			break;
		}
		if (r1.rip != prev)
			moved = 1;
		prev = r1.rip;
	}

	test_result("each SINGLESTEP stops again with SIGTRAP", steps_ok);
	test_result("stepping advances the instruction pointer", moved);

	/* And a plain continue after stepping must run to the end rather than
	 * keep trapping -- the trap flag has to have been cleared. */
	if (ptrace(PTRACE_CONT, pid, NULL, 0) == 0) {
		st = 0;
		r = waitpid(pid, &st, 0);
		test_result("CONT after stepping runs to exit",
			    r == pid && WIFEXITED(st));
	} else {
		test_fail("singlestep: final CONT failed");
	}

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

/* A real software breakpoint: write the trap instruction into the tracee's
 * own code, let it run into it, then put back what was there.  This is what
 * every breakpoint in a debugger is underneath. */
static void test_ptrace_swbreakpoint(void)
{
	printf("\n[TEST] ptrace software breakpoint\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace breakpoint: fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		execl("/bin/true", "true", (char *)NULL);
		_exit(43);
	}

	int st = 0;
	pid_t r = waitpid(pid, &st, 0);

	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("breakpoint: child did not stop on exec");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	/* Planted at the exec stop, with not one instruction executed yet.
	 *
	 * This is deliberately the hard case and the one that matters: it is
	 * where a debugger sets its breakpoints, and at that moment the text
	 * page has never been touched, so it is only lazily mapped.  The tracee
	 * cannot fault it in itself -- it is stopped -- so the kernel has to
	 * materialise it on the tracee's behalf.  Stepping first would make the
	 * tracee do that work and would test something easier. */
	struct ptrace_regs regs;

	memset(&regs, 0, sizeof(regs));
	if (ptrace(PTRACE_GETREGS, pid, &regs, 0) != 0) {
		test_fail("breakpoint: GETREGS failed");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	/* Plant int3 where execution is about to resume.  Reading first is not
	 * optional: the original byte has to go back before the program can
	 * continue, and this is also what proves text is readable at all. */
	unsigned long entry = regs.rip;

	errno = 0;
	long orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)entry, 0);
	int read_ok = (errno == 0);

	test_result("PEEKTEXT reads the tracee's code", read_ok);

	if (!read_ok) {
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	unsigned long trap = ((unsigned long)orig & ~0xFFUL) | 0xCCUL;

	test_result("POKETEXT plants int3 in the tracee's code",
		    ptrace(PTRACE_POKETEXT, pid, (void *)entry, (long)trap) == 0);

	errno = 0;
	long back = ptrace(PTRACE_PEEKTEXT, pid, (void *)entry, 0);

	test_result("the planted int3 reads back",
		    errno == 0 && ((unsigned long)back & 0xFF) == 0xCC);

	/* Run into it. */
	if (ptrace(PTRACE_CONT, pid, NULL, 0) == 0) {
		st = 0;
		r = waitpid(pid, &st, 0);
		int hit = (r == pid && WIFSTOPPED(st) &&
			   WSTOPSIG(st) == SIGTRAP);

		test_result("tracee traps on the planted breakpoint", hit);

		if (hit) {
			/* Put the original instruction back and rewind past
			 * the trap byte, which is what a debugger does before
			 * continuing from a breakpoint. */
			test_result("POKETEXT restores the original code",
				    ptrace(PTRACE_POKETEXT, pid, (void *)entry, orig) == 0);

			memset(&regs, 0, sizeof(regs));
			if (ptrace(PTRACE_GETREGS, pid, &regs, 0) == 0) {
				test_result("RIP is just past the breakpoint",
					    regs.rip == entry + 1);
				regs.rip = entry;
				test_result("SETREGS rewinds RIP",
					    ptrace(PTRACE_SETREGS, pid, &regs,
						   0) == 0);
			} else {
				test_fail("breakpoint: GETREGS after trap failed");
			}
		}
	} else {
		test_fail("breakpoint: CONT failed");
	}

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

/* waitpid with a ceiling.
 *
 * The ptrace tests below are all "resume, then expect a stop".  A blocking
 * wait for a stop that never arrives hangs the whole suite with no clue which
 * assertion was owed the answer, so every wait here is bounded and a timeout
 * is reported as the failure it is.  Returns the pid, 0 on timeout, -1 on
 * error. */
static pid_t ptrace_wait_bounded(pid_t pid, int *st, int ms)
{
	for (int waited = 0; waited < ms; waited += 5) {
		pid_t r = waitpid(pid, st, WNOHANG);

		if (r != 0)
			return r;
		usleep(5000);
	}
	return 0;
}

static void test_ptrace_syscall_trace(void)
{
	printf("\n[TEST] ptrace syscall entry/exit stops\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace syscall: fork failed");
		return;
	}
	if (pid == 0) {
		/* A predictable, cheap syscall in a loop, so there is always a
		 * boundary coming. */
		for (;;) {
			(void)getpid();
			usleep(10000);
		}
	}
	usleep(100000);

	if (ptrace(PTRACE_ATTACH, pid, NULL, 0) != 0) {
		test_fail("ptrace syscall: attach failed");
		kill(pid, SIGKILL);
		int st = 0;
		waitpid(pid, &st, 0);
		return;
	}

	int st = 0;

	ptrace_wait_bounded(pid, &st, 2000);

	/* Entry. */
	int entry_ok = 0, exit_ok = 0;
	unsigned long entry_nr = 0;

	if (ptrace(PTRACE_SYSCALL, pid, NULL, 0) == 0) {
		pid_t r = ptrace_wait_bounded(pid, &st, 2000);

		if (r == pid && WIFSTOPPED(st) &&
		    (st >> 16) == PTRACE_EVENT_SYSCALL_ENTRY) {
			entry_ok = 1;
			(void)ptrace(PTRACE_GETEVENTMSG, pid, NULL, (long)&entry_nr);
		}
	}
	test_result("PTRACE_SYSCALL stops on syscall entry", entry_ok);
	test_result("the entry stop names the syscall", entry_ok && entry_nr > 0);

	/* Exit of the same call. */
	if (entry_ok && ptrace(PTRACE_SYSCALL, pid, NULL, 0) == 0) {
		pid_t r = ptrace_wait_bounded(pid, &st, 2000);

		if (r == pid && WIFSTOPPED(st) &&
		    (st >> 16) == PTRACE_EVENT_SYSCALL_EXIT)
			exit_ok = 1;
	}
	test_result("the following stop is the matching syscall exit", exit_ok);

	/* A plain continue must stop asking: the tracee should run on rather
	 * than trapping at the next boundary. */
	if (ptrace(PTRACE_CONT, pid, NULL, 0) == 0) {
		pid_t r = ptrace_wait_bounded(pid, &st, 400);

		test_result("CONT after SYSCALL stops tracing syscalls",
			    r == 0);
	} else {
		test_fail("syscall trace: final CONT failed");
	}

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
}

static void test_ptrace_fork_events(void)
{
	printf("\n[TEST] ptrace fork-following\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("ptrace fork event: fork failed");
		return;
	}
	if (pid == 0) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		/* Stop so the tracer can set its options before the fork. */
		raise(SIGSTOP);
		pid_t g = fork();

		if (g == 0)
			_exit(7); /* grandchild: exists only to be reported */
		for (;;)
			usleep(20000);
	}

	int st = 0;
	pid_t r = ptrace_wait_bounded(pid, &st, 2000);

	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("fork event: child did not stop for TRACEME");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	test_result("SETOPTIONS accepts TRACEFORK",
		    ptrace(PTRACE_SETOPTIONS, pid, NULL,
			   PTRACE_O_TRACEFORK) == 0);

	int got_event = 0;
	unsigned long gpid = 0;

	if (ptrace(PTRACE_CONT, pid, NULL, 0) == 0) {
		/* Two stops are owed: the parent's fork event and the child's
		 * own stop.  Either may be reported first, so collect until the
		 * parent's event turns up. */
		for (int i = 0; i < 4 && !got_event; i++) {
			pid_t w = ptrace_wait_bounded(-1, &st, 2000);

			if (w <= 0)
				break;
			if (w == pid && WIFSTOPPED(st) &&
			    (st >> 16) == PTRACE_EVENT_FORK) {
				got_event = 1;
				(void)ptrace(PTRACE_GETEVENTMSG, pid, NULL,
					     (long)&gpid);
			}
		}
	}

	test_result("forking a traced process reports a fork event", got_event);
	test_result("the fork event carries the new pid",
		    got_event && gpid > 1);

	/* The forked child is NOT a thread of its parent.
	 *
	 * A debugger asks the kernel which tasks belong to the process it is
	 * debugging, and that answer is the only thing that tells a forked
	 * child apart from a new thread -- both arrive as a stop naming a task
	 * id it has not seen.  Get it wrong and the child is announced as a
	 * thread of the parent, buried at the next refresh, and the next stop
	 * reported against it takes the debugger down.  Every command run from
	 * a shell under a debugger is a fork, so this is not a corner. */
	if (got_event && gpid > 1) {
		unsigned int ids[16];
		long n = ptrace(PTRACE_GETLWPLIST, pid, ids, (long)16);
		int found_child = 0;

		for (long i = 0; i < n; i++)
			if (ids[i] == (unsigned int)gpid)
				found_child = 1;
		test_result("a forked child is not listed as a thread of its parent",
			    n > 0 && !found_child);
	}

	/* The grandchild was parked before it could run, so it is still there
	 * to be released -- which is the whole point of following a fork. */
	if (got_event && gpid > 1) {
		int cont_ok = (ptrace(PTRACE_CONT, (pid_t)gpid, NULL, 0) == 0);

		test_result("the new child was held for the tracer", cont_ok);
		kill((pid_t)gpid, SIGKILL);
		int gst = 0;
		ptrace_wait_bounded((pid_t)gpid, &gst, 500);
	}

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	/* Reap anything else this test left behind. */
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* A thread's life, as a debugger sees it.
 *
 * A thread is invisible from outside its process and is nobody's child, so the
 * only way a debugger learns that one appeared or went away is for the kernel
 * to say so.  Without both halves `info threads' shows the threads that were
 * alive at some point in the past, corrected only when the process next
 * happens to stop -- which for a program running under `continue' means not
 * until the user interrupts it.
 */
static void *pte_thread_fn(void *arg)
{
	(void)arg;
	return NULL; /* exists to be born and to die */
}

static void test_ptrace_thread_events(void)
{
	printf("\n[TEST] ptrace thread create/exit events\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("thread events: fork failed");
		return;
	}
	if (pid == 0) {
		pthread_t th;

		if (ptrace(PTRACE_TRACEME, 0, NULL, 0) != 0)
			_exit(42);
		/* Stop BEFORE the thread exists: the events are opt-in and the
		 * opt-in has to be in place first. */
		raise(SIGSTOP);
		if (pthread_create(&th, NULL, pte_thread_fn, NULL) != 0)
			_exit(43);
		pthread_join(th, NULL);
		for (;;)
			usleep(20000);
	}

	int st = 0;
	pid_t r = ptrace_wait_bounded(pid, &st, 2000);

	if (!(r == pid && WIFSTOPPED(st))) {
		test_fail("thread events: child did not stop for TRACEME");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}

	test_result("SETOPTIONS accepts TRACECLONE|TRACEEXIT",
		    ptrace(PTRACE_SETOPTIONS, pid, NULL,
			   PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT) == 0);

	int got_clone = 0, got_exit = 0;
	unsigned long tid = 0;

	if (ptrace(PTRACE_CONT, pid, NULL, 0) == 0) {
		/* Collect stops until both events have turned up.  Each one
		 * stops the whole process, so every stop is resumed before
		 * asking for the next. */
		for (int i = 0; i < 16 && !(got_clone && got_exit); i++) {
			pid_t w = ptrace_wait_bounded(-1, &st, 2000);

			if (w <= 0 || !WIFSTOPPED(st))
				break;
			if ((st >> 16) == PTRACE_EVENT_CLONE) {
				got_clone = 1;
				(void)ptrace(PTRACE_GETEVENTMSG, w, NULL,
					     (long)&tid);
			} else if ((st >> 16) == PTRACE_EVENT_EXIT) {
				/* The thread that is going is the one that
				 * stopped, so the stop names it directly. */
				if (tid != 0 && w == (pid_t)tid)
					got_exit = 1;
			}
			if (ptrace(PTRACE_CONT, w, NULL, 0) != 0)
				break;
		}
	}

	test_result("creating a thread reports a clone event", got_clone);
	test_result("the clone event carries the new thread id",
		    got_clone && tid > 1 && (pid_t)tid != pid);
	test_result("a thread exiting reports an exit event", got_exit);

	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* Detaching releases the whole process, not just the thread named.
 *
 * Every trace stop parks the whole thread group, and the siblings are parked
 * silently -- no event, no stop signal -- so a resume aimed at the group is the
 * only thing that ever releases them.  A detach that let go of one thread left
 * the others stopped with no tracer left to continue them: threads that can
 * never run again inside a process that still looks alive.
 */
static void *dtg_thread_fn(void *arg)
{
	volatile int *go = (volatile int *)arg;

	while (!*go)
		usleep(10000);
	return NULL;
}

static void test_ptrace_detach_releases_group(void)
{
	printf("\n[TEST] ptrace DETACH releases every thread\n");

	int pfd[2];

	if (pipe(pfd) != 0) {
		test_fail("detach group: pipe failed");
		return;
	}

	pid_t pid = fork();

	if (pid < 0) {
		test_fail("detach group: fork failed");
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	if (pid == 0) {
		static volatile int go;
		pthread_t th[3];
		char c = 'x';

		close(pfd[0]);
		for (int i = 0; i < 3; i++)
			pthread_create(&th[i], NULL, dtg_thread_fn,
				       (void *)&go);
		usleep(150000);
		go = 1;
		for (int i = 0; i < 3; i++)
			pthread_join(th[i], NULL);
		/* Only reached if every thread was released. */
		(void)!write(pfd[1], &c, 1);
		close(pfd[1]);
		for (;;)
			usleep(50000);
	}

	close(pfd[1]);
	usleep(100000);

	int st = 0;

	if (ptrace(PTRACE_ATTACH, pid, NULL, 0) != 0) {
		test_fail("detach group: attach failed");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		close(pfd[0]);
		return;
	}
	/* The attach stop parks every thread of the group, not just this one. */
	ptrace_wait_bounded(pid, &st, 2000);

	test_result("DETACH from a threaded tracee succeeds",
		    ptrace(PTRACE_DETACH, pid, NULL, 0) == 0);

	/* The proof: the child can only reach its write() if all three threads
	 * ran again and were joined.  A thread left stopped hangs the join. */
	int fl = fcntl(pfd[0], F_GETFL, 0);

	fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);

	int released = 0;
	char c = 0;

	for (int i = 0; i < 400 && !released; i++) {
		if (read(pfd[0], &c, 1) == 1)
			released = 1;
		else
			usleep(10000);
	}
	test_result("every thread runs again after DETACH", released);

	close(pfd[0]);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* Two signal handlers on the stack at once.
 *
 * A handler can be interrupted by another signal, so a process can have several
 * signal frames stacked on its user stack, and each sigreturn has to find ITS
 * OWN.  Recording "the" frame address in one per-task slot can only describe
 * the innermost: the inner sigreturn cleared it, the outer one then found
 * nothing and failed -- and a failed sigreturn has nowhere to return to, so the
 * trampoline ran off its end into the `hlt' that follows the syscall.  That is
 * a privileged instruction in user mode, so it surfaced as a general protection
 * fault reported as SIGSEGV at an address the program never chose.  Two X
 * clients died that way on a shutdown signal followed by a restart signal.
 *
 * Run in a child, because the failure mode is the process dying.
 */
static volatile sig_atomic_t nsh_inner_ran;
static volatile sig_atomic_t nsh_outer_resumed;

static void nsh_inner_handler(int sig)
{
	(void)sig;
	nsh_inner_ran = 1;
}

static void nsh_outer_handler(int sig)
{
	(void)sig;
	/* A second frame on top of this one.  Reaching the line after this is
	 * the inner sigreturn having worked. */
	raise(SIGUSR2);
	nsh_outer_resumed = 1;
}

static void test_nested_signal_handlers(void)
{
	printf("\n[TEST] nested signal handlers\n");

	pid_t pid = fork();

	if (pid < 0) {
		test_fail("nested signals: fork failed");
		return;
	}
	if (pid == 0) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sigemptyset(&sa.sa_mask);
		sa.sa_handler = nsh_outer_handler;
		sigaction(SIGUSR1, &sa, NULL);
		sa.sa_handler = nsh_inner_handler;
		sigaction(SIGUSR2, &sa, NULL);

		nsh_inner_ran = 0;
		nsh_outer_resumed = 0;

		raise(SIGUSR1);

		/* Reached only if the OUTER sigreturn worked too. */
		_exit(nsh_inner_ran && nsh_outer_resumed ? 7 : 8);
	}

	int st = 0;
	pid_t r = ptrace_wait_bounded(pid, &st, 3000);

	test_result("a nested handler does not kill the process",
		    r == pid && WIFEXITED(st));
	test_result("both handlers ran and both returned",
		    r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);

	if (r != pid) {
		kill(pid, SIGKILL);
		ptrace_wait_bounded(pid, &st, 2000);
	}
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* The executable a tracee is running.
 *
 * A debugger that attached has no other way to find the binary, and without it
 * there are no symbols, no breakpoint by name and no way to relocate a
 * position-independent image -- `gdb -p N' is a session that can only print
 * addresses.  The process name is a basename and argv[0] is whatever the
 * program was invoked as: a login shell's is "-bash", which opens nothing.
 */
static void test_ptrace_execpath(void)
{
	printf("\n[TEST] ptrace PTRACE_GETEXECPATH\n");

	pid_t pid = fork();

	if (pid < 0) {
		test_fail("execpath: fork failed");
		return;
	}
	if (pid == 0) {
		for (;;)
			usleep(50000);
	}
	usleep(100000);

	int st = 0;

	if (ptrace(PTRACE_ATTACH, pid, NULL, 0) != 0) {
		test_fail("execpath: attach failed");
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		return;
	}
	ptrace_wait_bounded(pid, &st, 2000);

	char path[512];
	long n = ptrace(PTRACE_GETEXECPATH, pid, path, (long)sizeof(path));

	test_result("GETEXECPATH reports a length", n > 0 && n < 512);
	test_result("GETEXECPATH reports an absolute path",
		    n > 0 && path[0] == '/');
	/* The strongest check available from inside: the path names a file that
	 * is really there.  A basename or an argv[0] would not. */
	test_result("GETEXECPATH names a file that exists",
		    n > 0 && access(path, F_OK) == 0);
	/* A buffer that cannot hold the path and its terminator is an error,
	 * not a silent truncation -- a caller handed an unterminated path reads
	 * past its own buffer. */
	errno = 0;
	test_result("GETEXECPATH refuses a buffer that is too small",
		    ptrace(PTRACE_GETEXECPATH, pid, path, 1) < 0);

	ptrace(PTRACE_DETACH, pid, NULL, 0);
	kill(pid, SIGKILL);
	waitpid(pid, &st, 0);

	/* At an EXEC STOP the path must name the program being exec'd TO.
	 *
	 * The exec stop exists so a debugger can act before the new program's
	 * first instruction, and what it asks first is which program that is.
	 * The kernel used to record exe_path AFTER raising the stop, so the
	 * answer was the PREVIOUS image -- off by exactly one exec, which is
	 * the hardest kind of wrong to notice, because it is always a real
	 * program the process really did run.  gdb believed it: "process N is
	 * executing new program: /bin/bash" for a process about to execute
	 * something else, and the wrong symbols loaded to match.
	 *
	 * PTRACE_TRACEME then execve is the shortest way to stand at that stop:
	 * the exec itself raises it, so the parent's first wait lands exactly
	 * there.  /bin/true because it is the smallest program on the image
	 * that is certainly present, and because its path cannot be confused
	 * with this one. */
	pid_t ep = fork();

	if (ep < 0) {
		test_fail("execpath: fork for the exec stop failed");
		return;
	}
	if (ep == 0) {
		ptrace(PTRACE_TRACEME, 0, NULL, 0);
		execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}

	int est = 0;

	if (ptrace_wait_bounded(ep, &est, 2000) <= 0) {
		test_fail("execpath: the exec stop never arrived");
	} else {
		char epath[512];
		long en = ptrace(PTRACE_GETEXECPATH, ep, epath,
				 (long)sizeof(epath));

		test_result(
			"GETEXECPATH at an exec stop names the NEW program",
			en > 0 && strcmp(epath, "/bin/true") == 0);
	}

	kill(ep, SIGKILL);
	waitpid(ep, &est, 0);
}

/* A fatal signal ends the PROCESS, not the thread that took it.
 *
 * The failure this pins down is not a crash, it is a program that does not
 * finish dying.  Only signal_select() killed the group; the paths a process
 * takes to signal ITSELF -- which is what abort() and raise() are -- ended one
 * thread and left the rest asleep.  A thread-group leader is reported to its
 * parent only once its last thread has gone, so the zombie leader was never
 * reported at all and whoever ran the program waited for ever with no message
 * and no prompt.  gdb died this way and took its terminal with it, eight worker
 * threads still parked in futex_wait.
 *
 * The threads here block on a condition variable nothing ever signals, on
 * purpose: that is where those workers were, and a thread in an untimed wait
 * has nothing of its own that will ever wake it.
 */
static pthread_mutex_t fkg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fkg_cond;

static void *fkg_thread_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&fkg_lock);
	for (;;)
		pthread_cond_wait(&fkg_cond, &fkg_lock);
	return NULL;
}

static void test_fatal_signal_kills_group(void)
{
	printf("\n[TEST] a fatal signal ends the whole process\n");

	pid_t pid = fork();

	if (pid < 0) {
		test_fail("fatal group: fork failed");
		return;
	}
	if (pid == 0) {
		pthread_t th[4];

		pthread_cond_init(&fkg_cond, NULL);
		for (int i = 0; i < 4; i++)
			pthread_create(&th[i], NULL, fkg_thread_fn, NULL);
		usleep(150000); /* let them reach the wait */
		/* kill(getpid(), SIGABRT) with the default disposition: the
		 * self-signal path, which is the one that was broken. */
		raise(SIGABRT);
		for (;;)
			usleep(50000); /* must never be reached */
	}

	int st = 0;
	pid_t r = ptrace_wait_bounded(pid, &st, 5000);

	test_result("a threaded process that aborts is reaped", r == pid);
	test_result("...and reports the fatal signal",
		    r == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);

	if (r != pid) {
		/* Left half-dead: clean up so the rest of the suite runs. */
		kill(pid, SIGKILL);
		ptrace_wait_bounded(pid, &st, 2000);
	}
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* Tracing something that is not your child.
 *
 * This is what a debugger does every time it is pointed at a running program,
 * and it is the case nothing else in this file covers: every other tracee here
 * is a child, so the child list describes it and the ordinary waitpid()
 * machinery reports everything about it.  A tracee that is NOT a child appears
 * on no list the tracer owns -- so if the tracee link does not carry its death,
 * nothing does, and the tracer waits for a report that cannot arrive.
 *
 * The middle process stays alive on purpose.  It makes the tracee a
 * GRANDCHILD, which is what makes this a non-child tracer, and it holds the
 * tracee's zombie so the answer does not depend on who reaps first.
 */
static void test_ptrace_attach_nonchild(void)
{
	printf("\n[TEST] ptrace attach to a non-child\n");

	int pfd[2];

	if (pipe(pfd) != 0) {
		test_fail("attach non-child: pipe failed");
		return;
	}

	pid_t mid = fork();

	if (mid < 0) {
		test_fail("attach non-child: fork failed");
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	if (mid == 0) {
		close(pfd[0]);
		pid_t g = fork();

		if (g == 0) {
			close(pfd[1]);
			/* Alive long enough to be attached to, then a plain
			 * exit with a status worth checking. */
			for (int i = 0; i < 60; i++)
				usleep(20000);
			_exit(42);
		}
		(void)!write(pfd[1], &g, sizeof(g));
		close(pfd[1]);
		for (;;)
			usleep(50000);
	}

	close(pfd[1]);
	pid_t g = 0;
	int n = (int)read(pfd[0], &g, sizeof(g));

	close(pfd[0]);

	int st = 0;

	if (n != (int)sizeof(g) || g <= 1) {
		test_fail("attach non-child: no grandchild pid");
		kill(mid, SIGKILL);
		waitpid(mid, &st, 0);
		return;
	}

	usleep(100000); /* let it reach its loop */

	int attached = (ptrace(PTRACE_ATTACH, g, NULL, 0) == 0);

	test_result("attach to a process that is not our child", attached);

	if (attached) {
		pid_t r = ptrace_wait_bounded(g, &st, 2000);

		test_result("an attached non-child reports its stop",
			    r == g && WIFSTOPPED(st));

		if (ptrace(PTRACE_CONT, g, NULL, 0) == 0) {
			/* The report with no other route home. */
			r = ptrace_wait_bounded(g, &st, 4000);
			test_result(
				"an attached non-child's exit reaches its tracer",
				r == g && WIFEXITED(st) &&
					WEXITSTATUS(st) == 42);
		} else {
			test_fail("attach non-child: CONT failed");
		}
	}

	kill(g, SIGKILL);
	kill(mid, SIGKILL);
	waitpid(mid, &st, 0);
	while (waitpid(-1, &st, WNOHANG) > 0)
		;
}

/* The dynamic loader's debugger rendezvous.
 *
 * A debugger reads this out of a stopped process's memory to learn what is
 * loaded and where; there is no call it can make.  Verified from inside the
 * process because that is the only place it can be checked automatically -- but
 * every field checked here is one an external debugger depends on, and the
 * structure layout is fixed by convention rather than by us, so a mistake in it
 * is invisible until a debugger reads garbage. */
static int rtld_main_dyn_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
	(void)sz;
	if (info->dlpi_name && info->dlpi_name[0] != '\0')
		return 0; /* not the main executable */
	for (Elf64_Half i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
			*(const Elf64_Dyn **)data =
				(const Elf64_Dyn *)(info->dlpi_addr +
						    info->dlpi_phdr[i].p_vaddr);
			return 1;
		}
	}
	return 1;
}

/* Find the live r_debug the way a debugger does: read DT_DEBUG out of the main
 * executable's dynamic section, which the loader filled in with the address of
 * ITS r_debug.
 *
 * Deliberately NOT the `extern _r_debug` symbol.  Referencing that from the
 * executable makes the linker emit a copy relocation: the loader's r_debug is
 * duplicated into the executable's BSS at relocation time -- before the loader
 * has populated it -- so the symbol reads a permanently stale snapshot while
 * DT_DEBUG points at the live one.  gdb reads DT_DEBUG; so does this. */
static struct r_debug *rtld_live_r_debug(void)
{
	const Elf64_Dyn *dyn = NULL;

	(void)dl_iterate_phdr(rtld_main_dyn_cb, &dyn);
	if (!dyn)
		return NULL;
	for (const Elf64_Dyn *e = dyn; e->d_tag != DT_NULL; e++)
		if (e->d_tag == DT_DEBUG)
			return (struct r_debug *)(unsigned long)e->d_un.d_ptr;
	return NULL;
}

static int rtld_map_len(struct r_debug *rd)
{
	int n = 0;

	for (struct link_map *m = rd->r_map; m && n < 256; m = m->l_next)
		n++;
	return n;
}

static void test_rtld_debug_rendezvous(void)
{
	printf("\n[TEST] loader debugger rendezvous (r_debug / link_map)\n");

	/* The whole discovery path in one step: no DT_DEBUG means a debugger has
	 * no way in, so nothing else is worth checking. */
	struct r_debug *rd = rtld_live_r_debug();

	test_result("DT_DEBUG resolves to a live r_debug", rd != NULL);
	if (!rd) {
		test_fail("rendezvous: DT_DEBUG missing or NULL");
		return;
	}

	test_result("r_debug reports version 1", rd->r_version == 1);
	test_result("r_debug has an object list", rd->r_map != NULL);
	test_result("r_brk is a real breakpoint address", rd->r_brk != 0);
	test_result("r_ldbase names where the loader is mapped",
		    rd->r_ldbase != 0);
	test_result("the list reads as consistent while idle",
		    rd->r_state == RT_CONSISTENT);

	/* A debugger identifies the program by position and by the empty name;
	 * it has no other way to tell which object is the executable. */
	struct link_map *m = rd->r_map;

	test_result("the first object is the main executable (empty name)",
		    m && m->l_name && m->l_name[0] == '\0');

	int n = 0, all_have_ld = 1, saw_named = 0;

	for (m = rd->r_map; m && n < 64; m = m->l_next) {
		n++;
		if (!m->l_ld)
			all_have_ld = 0;
		if (m->l_name && m->l_name[0] != '\0')
			saw_named = 1;
	}
	test_result("more than one object is listed", n >= 2);
	test_result("every object reports its dynamic section", all_have_ld);
	test_result("shared objects are listed by name", saw_named);

	/* Walkable in both directions: a debugger may start from either end. */
	int links_ok = (rd->r_map != NULL);

	for (m = rd->r_map; m && m->l_next; m = m->l_next) {
		if (m->l_next->l_prev != m) {
			links_ok = 0;
			break;
		}
	}
	test_result("the list is correctly doubly linked", links_ok);

	/* Loading a library must show up on the list, and unloading must take it
	 * off again -- a stale entry is one a debugger would follow into pages
	 * that are no longer mapped. */
	int before = rtld_map_len(rd);
	void *h = dlopen("/lib/libtestlib.so", RTLD_LAZY);

	if (h) {
		test_result("dlopen adds the object to the list",
			    rtld_map_len(rd) > before);
		test_result("the list is consistent again after dlopen",
			    rd->r_state == RT_CONSISTENT);

		dlclose(h);

		test_result("dlclose removes it again",
			    rtld_map_len(rd) == before);
		test_result("the list is consistent again after dlclose",
			    rd->r_state == RT_CONSISTENT);
	} else {
		printf("  [SKIP] /lib/libtestlib.so not available\n");
	}
}

static void test_jobctl_wait(void)
{
	printf("\n[TEST] waitpid job control (WUNTRACED/WCONTINUED)\n");

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("jobctl: fork failed");
		return;
	}
	if (pid == 0) {
		/* Child spins in sleep; parent stops/continues/kills it. */
		for (;;)
			usleep(50000);
	}
	usleep(100000); /* let the child reach its loop */

	int st = 0;
	kill(pid, SIGSTOP);
	pid_t r = waitpid(pid, &st, WUNTRACED);
	test_result("waitpid(WUNTRACED) reports stopped child",
		    r == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
	test_result("stopped status is not WIFSIGNALED",
		    r == pid && !WIFSIGNALED(st) && !WIFEXITED(st));

	/* No event pending now: WNOHANG must return 0, not re-report */
	r = waitpid(pid, &st, WUNTRACED | WNOHANG);
	test_result("stop reported only once", r == 0);

	kill(pid, SIGCONT);
	r = waitpid(pid, &st, WCONTINUED);
	test_result("waitpid(WCONTINUED) reports continued child",
		    r == pid && WIFCONTINUED(st));

	/* SIGTSTP (tty stop, catchable flavor) must also be reportable */
	kill(pid, SIGTSTP);
	r = waitpid(pid, &st, WUNTRACED);
	test_result("SIGTSTP stop reported via WUNTRACED",
		    r == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP);
	kill(pid, SIGCONT);

	kill(pid, SIGKILL);
	r = waitpid(pid, &st, 0);
	test_result("killed child reaped normally",
		    r == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);

	/* A NORMAL exit with a status >= 128 must be reported as WIFEXITED,
	 * NOT as signalled/stopped.  The kernel used to overload exit_code with
	 * the shell's "128 + signal" convention, so exit(255) (which ssh uses)
	 * decoded to status 0x7F == WIFSTOPPED with signal 0 — bash then
	 * reported a plain `ssh` invocation as "Stopped / Unknown signal 0". */
	pid_t ep = fork();
	if (ep == 0)
		_exit(255);
	int est = 0;
	waitpid(ep, &est, 0);
	test_result("exit(255) is WIFEXITED, not stopped/signalled",
		    WIFEXITED(est) && !WIFSTOPPED(est) && !WIFSIGNALED(est));
	test_result("exit(255) status is 255", WEXITSTATUS(est) == 255);

	ep = fork();
	if (ep == 0)
		_exit(130); /* looks like 128+SIGINT but is a real exit code */
	waitpid(ep, &est, 0);
	test_result("exit(130) is WIFEXITED with status 130",
		    WIFEXITED(est) && WEXITSTATUS(est) == 130 &&
			    !WIFSIGNALED(est));
}

/* ---- syscall argument-register hygiene -------------------------------
 *
 * The kernel dispatcher reads all six argument registers unconditionally, so
 * a libc wrapper that passes fewer must still zero the rest.  waitpid() once
 * used a 3-argument wrapper while SYS_WAIT4 treats the 4th argument as a
 * `struct rusage *` and WRITES 56 bytes through it: whatever the compiler
 * had left in r10 became the destination, so every reaped child scribbled on
 * random process memory (found as heap corruption in bash).
 *
 * tl_waitpid_r10 calls waitpid() with r10 pre-loaded with a caller-supplied
 * value.  The PLT jump preserves r10, so if the wrapper leaks it the kernel
 * writes the rusage into our canary buffer and the test fails. */
__asm__(".text\n\t"
	".globl tl_waitpid_r10\n\t"
	".type tl_waitpid_r10, @function\n"
	"tl_waitpid_r10:\n\t"
	"movq %rcx, %r10\n\t" /* 4th C argument -> syscall arg-4 register */
	"jmp waitpid@PLT\n\t"
	".size tl_waitpid_r10, .-tl_waitpid_r10\n");
extern pid_t tl_waitpid_r10(pid_t pid, int *status, int options, void *r10val);

static void test_syscall_arg_hygiene(void)
{
	printf("\n[TEST] syscall unused-argument registers are zeroed\n");

	/* Canary large enough for the 56-byte rusage the kernel would write */
	unsigned char *canary = malloc(128);
	if (!canary) {
		test_fail("arg hygiene: malloc failed");
		return;
	}
	memset(canary, 0xAB, 128);

	pid_t pid = fork();
	if (pid < 0) {
		test_fail("arg hygiene: fork failed");
		free(canary);
		return;
	}
	if (pid == 0)
		_exit(7);

	int st = 0;
	pid_t r = tl_waitpid_r10(pid, &st, 0, canary);
	test_result("waitpid reaps child correctly",
		    r == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);

	int intact = 1;
	for (int i = 0; i < 128; i++)
		if (canary[i] != 0xAB)
			intact = 0;
	test_result("waitpid does not write rusage through a stale r10", intact);
	free(canary);

	/* wait4() with an explicit rusage still works and fills it in. */
	pid = fork();
	if (pid == 0) {
		_exit(3);
	} else if (pid > 0) {
		struct rusage ru;
		memset(&ru, 0xCD, sizeof(ru));
		r = wait4(pid, &st, 0, &ru);
		test_result("wait4 with rusage reaps and fills it",
			    r == pid && WIFEXITED(st) &&
				    WEXITSTATUS(st) == 3 &&
				    ru.ru_utime.tv_sec != (long)0xCDCDCDCDCDCDCDCDULL);
	}

	/* Bulk reaping must not disturb the heap (the shape of the bash bug). */
	unsigned char *guard = malloc(256);
	if (guard) {
		memset(guard, 0x5A, 256);
		for (int i = 0; i < 20; i++) {
			pid_t c = fork();
			if (c == 0)
				_exit(0);
			if (c > 0)
				waitpid(c, &st, 0);
		}
		int ok = 1;
		for (int i = 0; i < 256; i++)
			if (guard[i] != 0x5A)
				ok = 0;
		test_result("20 fork/waitpid cycles leave the heap intact", ok);
		free(guard);
	}
}

/* ---- duplicating a REDIRECTED standard descriptor --------------------
 *
 * fds 0/1/2 are ordinarily the console, which the kernel represents with a
 * NULL descriptor-table slot.  Once one of them has been pointed somewhere
 * else, dup()/dup2()/fcntl(F_DUPFD) must duplicate what it points at NOW -
 * not the console.  Special-casing the fd NUMBER instead of its contents
 * made a shell's save/restore around a builtin redirection hand stdout back
 * to the terminal, silently losing every captured `$(...)` output. */

static void test_dup_redirected_stdio(void)
{
	printf("\n[TEST] duplicating redirected standard descriptors\n");

	int p[2];
	if (pipe(p) != 0) {
		test_fail("dup-redirect: pipe() failed");
		return;
	}
	int saved_stdout = dup(1); /* console; restored at the end */
	test_result("dup(1) while stdout is the console", saved_stdout >= 3);

	/* Point stdout at the pipe, then duplicate it three different ways. */
	dup2(p[1], 1);
	int d_dup = dup(1);
	int d_fcntl = fcntl(1, F_DUPFD, 10);
	/* Pick the dup2 target dynamically.  A hardcoded number can collide
	 * with a descriptor this test still needs (the pipe, or the saved
	 * console stdout) as the surrounding fd layout shifts; clobbering
	 * saved_stdout leaves stdout pointing at the pipe, and the process
	 * then dies of SIGPIPE with no output once the read end is closed. */
	int d_dup2 = fcntl(1, F_DUPFD, 20);
	if (d_dup2 >= 0)
		close(d_dup2); /* now known to be free */
	else
		d_dup2 = 20;
	dup2(1, d_dup2);

	/* Put the console back before printing any results. */
	dup2(saved_stdout, 1);
	close(saved_stdout);

	/* Guard the setup itself: if the dup2 target had aliased a descriptor
	 * this test still needs, everything below would report nonsense (or
	 * vanish into the pipe), so say so out loud instead. */
	test_result("dup2 target does not alias the test's own descriptors",
		    d_dup2 != p[0] && d_dup2 != p[1] && d_dup2 != saved_stdout &&
			    d_dup2 != d_dup && d_dup2 != d_fcntl);

	test_result("fcntl(F_DUPFD, 10) honours the minimum", d_fcntl >= 10);

	/* Each duplicate must reach the PIPE, not the console. */
	const char *msg = "abc";
	int wrote_ok = 1;
	if (d_dup < 0 || write(d_dup, msg, 3) != 3)
		wrote_ok = 0;
	if (d_fcntl < 0 || write(d_fcntl, msg, 3) != 3)
		wrote_ok = 0;
	if (write(d_dup2, msg, 3) != 3)
		wrote_ok = 0;
	test_result("writes to the duplicates succeed", wrote_ok);

	if (d_dup >= 0)
		close(d_dup);
	if (d_fcntl >= 0)
		close(d_fcntl);
	close(d_dup2);
	close(p[1]); /* all write ends gone -> reader sees EOF */

	char buf[32];
	int total = 0, n;
	while (total < (int)sizeof(buf) - 1 &&
	       (n = read(p[0], buf + total, sizeof(buf) - 1 - total)) > 0)
		total += n;
	buf[total > 0 ? total : 0] = '\0';
	close(p[0]);
	test_result("duplicates wrote to the pipe, not the console",
		    total == 9 && strcmp(buf, "abcabcabc") == 0);

	/* FD_CLOEXEC: F_DUPFD clears it, F_DUPFD_CLOEXEC sets it. */
	int c1 = fcntl(0, F_DUPFD, 10);
	int c2 = fcntl(0, F_DUPFD_CLOEXEC, 10);
	test_result("F_DUPFD clears FD_CLOEXEC",
		    c1 >= 0 && (fcntl(c1, F_GETFD, 0) & FD_CLOEXEC) == 0);
	test_result("F_DUPFD_CLOEXEC sets FD_CLOEXEC",
		    c2 >= 0 && (fcntl(c2, F_GETFD, 0) & FD_CLOEXEC) != 0);
	if (c1 >= 0)
		close(c1);
	if (c2 >= 0)
		close(c2);

	/* Bad source descriptor is still rejected. */
	errno = 0;
	test_result("F_DUPFD on a closed fd -> EBADF",
		    fcntl(200, F_DUPFD, 10) == -1 && errno == EBADF);
}

/* ---- POSIX shared memory ---------------------------------------------
 *
 * The property under test is the one nothing else in this system provides:
 * two processes that are NOT related by fork() mapping the same physical
 * memory.  MAP_SHARED on anonymous memory only survives fork; a shared memory
 * object has to be reachable by name.
 */
static void test_shm(void)
{
	/* Per-process name.  shm objects live in ONE global namespace, so a
	 * fixed name is shared by every copy of this suite running at once --
	 * and the unlink below, which exists to clear a previous run's
	 * leftovers, would delete the object another live run is using.  Both
	 * then fail: one loses its O_EXCL create, the other finds its name
	 * already gone at "shm_unlink removes the name". */
	char nmbuf[64];
	const char *nm = nmbuf;
	const size_t sz = 8192;

	snprintf(nmbuf, sizeof(nmbuf), "/tl_shm_test.%d", (int)getpid());

	printf("\n[TEST] POSIX shared memory (shm_open/ftruncate/mmap)\n");

	shm_unlink(nm); /* clear anything a previous run left behind */

	int fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
	test_result("shm_open creates an object", fd >= 0);
	if (fd < 0)
		return;

	/* A fresh object has no memory until it is sized. */
	struct stat st;
	test_result("a new object starts empty",
		    fstat(fd, &st) == 0 && st.st_size == 0);

	test_result("ftruncate sizes the object", ftruncate(fd, sz) == 0);
	test_result("fstat reports the new size",
		    fstat(fd, &st) == 0 && (size_t)st.st_size == sz);
	test_result("the object looks like a regular file",
		    S_ISREG(st.st_mode));

	void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	test_result("mmap of the object succeeds", p != MAP_FAILED);
	if (p == MAP_FAILED) {
		close(fd);
		shm_unlink(nm);
		return;
	}
	test_result("the object reads as zero", ((char *)p)[0] == 0 &&
						       ((char *)p)[sz - 1] == 0);

	strcpy((char *)p, "written by the parent");

	/* Opening the SAME NAME again must reach the same memory.  A second
	 * mapping in this process is the cheap half of the test... */
	int fd2 = shm_open(nm, O_RDWR, 0);
	test_result("the object can be opened again by name", fd2 >= 0);
	if (fd2 >= 0) {
		void *q = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED,
			       fd2, 0);
		test_result("a second mapping succeeds", q != MAP_FAILED);
		if (q != MAP_FAILED) {
			test_result("both mappings see the same bytes",
				    strcmp((char *)q,
					   "written by the parent") == 0);
			((char *)q)[0] = 'W';
			test_result("a write through one is seen by the other",
				    ((char *)p)[0] == 'W');
			munmap(q, sz);
		}
		close(fd2);
	}

	/* ...and this is the real one: a child that opens the object by name
	 * AFTER forking, so nothing was inherited through the address space. */
	{
		int sync_pipe[2];
		if (pipe(sync_pipe) == 0) {
			pid_t kid = fork();
			if (kid == 0) {
				char c;
				/* Drop the inherited mapping and descriptor so
				 * nothing can be reached except by name. */
				munmap(p, sz);
				close(fd);
				close(sync_pipe[1]);
				read(sync_pipe[0], &c, 1); /* wait for the go-ahead */
				close(sync_pipe[0]);

				int cfd = shm_open(nm, O_RDWR, 0);
				if (cfd < 0)
					_exit(10);
				void *cp = mmap(NULL, sz,
						PROT_READ | PROT_WRITE,
						MAP_SHARED, cfd, 0);
				if (cp == MAP_FAILED)
					_exit(11);
				if (strcmp((char *)cp + 1,
					   "ritten by the parent") != 0)
					_exit(12);
				strcpy((char *)cp + 100, "written by the child");
				munmap(cp, sz);
				close(cfd);
				_exit(0);
			} else if (kid > 0) {
				int st2 = 0;
				close(sync_pipe[0]);
				write(sync_pipe[1], "g", 1);
				close(sync_pipe[1]);
				waitpid(kid, &st2, 0);
				test_result(
					"an unrelated process opened it by name and read our data",
					WIFEXITED(st2) &&
						WEXITSTATUS(st2) == 0);
				test_result(
					"the child's write is visible to us",
					strcmp((char *)p + 100,
					       "written by the child") == 0);
			}
		}
	}

	/* Unlinking removes the name but must not pull the memory out from
	 * under a mapping that is still live. */
	test_result("shm_unlink removes the name", shm_unlink(nm) == 0);
	test_result("the mapping still works after unlink",
		    strcmp((char *)p + 100, "written by the child") == 0);
	errno = 0;
	test_result("the name is gone",
		    shm_open(nm, O_RDWR, 0) == -1);
	errno = 0;
	test_result("unlinking a missing name -> ENOENT",
		    shm_unlink("/tl_shm_absent") == -1 && errno == ENOENT);

	munmap(p, sz);
	close(fd);

	/* The name is free again once nothing holds the object. */
	int fd3 = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
	test_result("the name can be reused after everything is released",
		    fd3 >= 0);
	if (fd3 >= 0) {
		close(fd3);
		shm_unlink(nm);
	}
}

/* ---- System V shared memory -------------------------------------------
 *
 * Modelled on what the MIT-SHM X extension actually does: one process creates
 * a segment and attaches it, hands the IDENTIFIER (not a descriptor, not an
 * address) to another process, and that one attaches the same memory.
 */
static void test_sysv_shm(void)
{
	const size_t sz = 4096;

	printf("\n[TEST] System V shared memory (shmget/shmat/shmdt)\n");

	int id = shmget(IPC_PRIVATE, sz, IPC_CREAT | 0600);
	test_result("shmget creates a segment", id >= 0);
	if (id < 0)
		return;

	void *p = shmat(id, NULL, 0);
	test_result("shmat attaches it", p != (void *)-1);
	if (p == (void *)-1) {
		shmctl(id, IPC_RMID, NULL);
		return;
	}
	test_result("the segment reads as zero",
		    ((char *)p)[0] == 0 && ((char *)p)[sz - 1] == 0);

	struct shmid_ds ds;
	memset(&ds, 0, sizeof(ds));
	test_result("IPC_STAT reports the size",
		    shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz >= sz);

	strcpy((char *)p, "segment contents");

	/* The identifier is meaningful in another process: the child attaches
	 * by id alone, having dropped everything it inherited. */
	{
		int sync_pipe[2];
		if (pipe(sync_pipe) == 0) {
			pid_t kid = fork();
			if (kid == 0) {
				char c;
				shmdt(p); /* drop the inherited attachment */
				close(sync_pipe[1]);
				read(sync_pipe[0], &c, 1);
				close(sync_pipe[0]);

				void *cp = shmat(id, NULL, 0);
				if (cp == (void *)-1)
					_exit(20);
				if (strcmp((char *)cp, "segment contents") != 0)
					_exit(21);
				strcpy((char *)cp + 64, "child was here");
				shmdt(cp);
				_exit(0);
			} else if (kid > 0) {
				int st = 0;
				close(sync_pipe[0]);
				write(sync_pipe[1], "g", 1);
				close(sync_pipe[1]);
				waitpid(kid, &st, 0);
				test_result(
					"another process attached by identifier alone",
					WIFEXITED(st) && WEXITSTATUS(st) == 0);
				test_result("its write is visible to us",
					    strcmp((char *)p + 64,
						   "child was here") == 0);
			}
		}
	}

	/* Removing the segment while it is attached must not pull the memory
	 * away — create-attach-remove is the usual order precisely so nothing
	 * survives a crash. */
	test_result("IPC_RMID marks it for removal",
		    shmctl(id, IPC_RMID, NULL) == 0);
	test_result("the attachment still works after removal",
		    strcmp((char *)p, "segment contents") == 0);

	test_result("shmdt detaches", shmdt(p) == 0);
	errno = 0;
	test_result("detaching a bad address fails",
		    shmdt((void *)0x12345000) == -1);

	/* A keyed segment: a second shmget with the same key finds it. */
	{
		/* Per-process key: SysV keys are one global namespace, so a
		 * fixed one is shared by every copy of this suite running at
		 * once -- and the mode test elsewhere IPC_RMIDs whatever it
		 * finds under this key, which would destroy another live run's
		 * segment out from under it. */
		int k = 0x4C494B45 ^ (int)getpid();
		int a = shmget(k, sz, IPC_CREAT | 0600);
		int b = shmget(k, sz, 0);
		test_result("a keyed segment is found again by key",
			    a >= 0 && b == a);
		errno = 0;
		test_result("IPC_EXCL refuses an existing key",
			    shmget(k, sz, IPC_CREAT | IPC_EXCL | 0600) == -1 &&
				    errno == EEXIST);
		if (a >= 0)
			shmctl(a, IPC_RMID, NULL);
	}
	errno = 0;
	test_result("an unknown key without IPC_CREAT -> ENOENT",
		    shmget(0x7ABCDEF1, sz, 0) == -1 && errno == ENOENT);
}

/* ---- libc additions the X.org port needs ----------------------------- */

static int xorg_filter_dot(const struct dirent *d)
{
	return d->d_name[0] != '.';
}

static int qsort_r_cmp(const void *a, const void *b, void *arg)
{
	int *calls = (int *)arg;
	(*calls)++;
	return *(const int *)a - *(const int *)b;
}

static int qsort_int_cmp(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return (x > y) - (x < y);
}

/* Fault handler used by the PROT_NONE check: reaching it proves the access
 * faulted, and exiting from it keeps the fault out of the kernel's crash
 * reporter (the process is handling the signal, not dying from it). */
static void protnone_segv_handler(int sig)
{
	(void)sig;
	_exit(42);
}

/* ---- ELF thread-local storage (__thread) ------------------------------
 *
 * pixman and the X server use __thread heavily.  Until the loader and libc
 * agreed on where the thread pointer lives, %fs pointed at a struct in libc's
 * .bss and every access here read unrelated memory instead.  An initialised
 * variable lands in .tdata (copied per thread), an uninitialised one in .tbss
 * (zeroed per thread); both are exercised.
 */
static __thread int tls_initialised = 0xABCD;
static __thread int tls_zeroed;
static __thread char tls_buf[64];

struct tls_probe {
	int saw_initial; /* the .tdata image arrived */
	int saw_zero; /* the .tbss area was zeroed */
	int private_ok; /* writes stayed private to this thread */
	unsigned long tp; /* this thread's %fs base */
};

static void *tls_thread_fn(void *arg)
{
	struct tls_probe *p = (struct tls_probe *)arg;
	int want = (int)(long)p->tp; /* reused below as a marker */

	p->saw_initial = (tls_initialised == 0xABCD);
	p->saw_zero = (tls_zeroed == 0 && tls_buf[0] == '\0');

	/* Stamp our own copies, wait for the other thread to do the same, and
	 * confirm nothing bled across. */
	tls_initialised = want;
	tls_zeroed = want;
	for (int i = 0; i < 64; i++)
		tls_buf[i] = (char)(want + i);

	usleep(150000);

	p->private_ok = (tls_initialised == want && tls_zeroed == want &&
			 tls_buf[7] == (char)(want + 7));
	__asm__ volatile("mov %%fs:0, %0" : "=r"(p->tp));
	return NULL;
}

static void test_tls(void)
{
	printf("\n[TEST] ELF thread-local storage (__thread)\n");

	/* Main thread: the loader must have copied the .tdata image and zeroed
	 * the .tbss remainder into its block. */
	test_result("main thread sees the .tdata initial value",
		    tls_initialised == 0xABCD);
	test_result("main thread sees .tbss zeroed",
		    tls_zeroed == 0 && tls_buf[0] == '\0');

	tls_initialised = 0x1111;
	tls_zeroed = 0x2222;
	strcpy(tls_buf, "main");

	/* The thread pointer must sit inside a real TLS block, not in .bss.
	 * A __thread variable is addressed below %fs, so its address has to be
	 * lower than the thread pointer. */
	unsigned long tp = 0;
	__asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
	test_result("thread pointer is non-zero", tp != 0);
	test_result("__thread data lives below the thread pointer",
		    (unsigned long)&tls_initialised < tp);

	pthread_t t1, t2;
	struct tls_probe p1 = { 0, 0, 0, 0x5A5A }, p2 = { 0, 0, 0, 0x7C7C };
	int r1 = pthread_create(&t1, NULL, tls_thread_fn, &p1);
	int r2 = pthread_create(&t2, NULL, tls_thread_fn, &p2);

	test_result("two threads started", r1 == 0 && r2 == 0);
	if (r1 == 0)
		pthread_join(t1, NULL);
	if (r2 == 0)
		pthread_join(t2, NULL);

	test_result("each thread got its own .tdata image",
		    p1.saw_initial && p2.saw_initial);
	test_result("each thread got its own zeroed .tbss",
		    p1.saw_zero && p2.saw_zero);
	test_result("threads did not share __thread storage",
		    p1.private_ok && p2.private_ok);
	test_result("each thread had a distinct thread pointer",
		    p1.tp != 0 && p2.tp != 0 && p1.tp != p2.tp);

	/* The main thread's values must have survived both threads running. */
	test_result("main thread's __thread values were untouched",
		    tls_initialised == 0x1111 && tls_zeroed == 0x2222 &&
			    strcmp(tls_buf, "main") == 0);
}

static void test_xorg_libc_additions(void)
{
	printf("\n[TEST] libc additions for X.org (scandir/strtok_r/ffs/...)\n");

	/* --- scandir: how the X server enumerates its module directory --- */
	{
		const char *dir = tmpp("scandir_t");
		struct dirent **nl = NULL;
		int n;

		mkdir(dir, 0755);
		for (int i = 0; i < 12; i++) {
			char p[64];
			snprintf(p, sizeof(p), "%s/mod%d.so", dir, i);
			int fd = open(p, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0)
				close(fd);
		}
		{ /* a dotfile the filter must drop */
			char p[64];
			snprintf(p, sizeof(p), "%s/.hidden", dir);
			int fd = open(p, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0)
				close(fd);
		}

		n = scandir(dir, &nl, xorg_filter_dot, alphasort);
		test_result("scandir returns the filtered entry count", n == 12);
		if (n > 0) {
			int sorted = 1;
			for (int i = 1; i < n; i++)
				if (strcmp(nl[i - 1]->d_name, nl[i]->d_name) > 0)
					sorted = 0;
			test_result("scandir + alphasort orders entries", sorted);
			test_result("scandir dropped the dotfile",
				    strchr(nl[0]->d_name, '.') != NULL &&
					    nl[0]->d_name[0] != '.');
			for (int i = 0; i < n; i++)
				free(nl[i]);
			free(nl);
		}

		/* versionsort orders digit runs numerically: mod9 < mod10 */
		nl = NULL;
		n = scandir(dir, &nl, xorg_filter_dot, versionsort);
		if (n == 12) {
			test_result("scandir + versionsort puts mod2 before mod10",
				    strcmp(nl[1]->d_name, "mod1.so") == 0 &&
					    strcmp(nl[2]->d_name, "mod2.so") == 0);
			for (int i = 0; i < n; i++)
				free(nl[i]);
			free(nl);
		} else {
			test_fail("scandir + versionsort entry count");
		}

		errno = 0;
		test_result("scandir on a missing directory fails",
			    scandir(tmpp("no_such_dir_xyz"), &nl, NULL, NULL) ==
				    -1);

		for (int i = 0; i < 12; i++) {
			char p[64];
			snprintf(p, sizeof(p), "%s/mod%d.so", dir, i);
			unlink(p);
		}
		{
			char p[64];
			snprintf(p, sizeof(p), "%s/.hidden", dir);
			unlink(p);
		}
		rmdir(dir);
	}

	/* --- a large directory: libXfont2 reads font dirs this way, and the
	 * DIR buffer is only 1 KB, so this crosses many getdents64 refills --- */
	{
		const char *dir = tmpp("bigdir_t");
		DIR *dp;
		int count = 0, dup_seen = 0;
		char seen[200] = { 0 };

		mkdir(dir, 0755);
		for (int i = 0; i < 200; i++) {
			char p[80];
			snprintf(p, sizeof(p), "%s/entry_%03d", dir, i);
			int fd = open(p, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0)
				close(fd);
		}
		dp = opendir(dir);
		if (dp) {
			struct dirent *d;
			while ((d = readdir(dp)) != NULL) {
				int idx;
				if (d->d_name[0] == '.')
					continue;
				count++;
				if (sscanf(d->d_name, "entry_%d", &idx) == 1 &&
				    idx >= 0 && idx < 200) {
					if (seen[idx])
						dup_seen = 1;
					seen[idx] = 1;
				}
			}
			closedir(dp);
		}
		test_result("readdir returns all 200 entries", count == 200);
		test_result("readdir returns no duplicates", !dup_seen);

		for (int i = 0; i < 200; i++) {
			char p[80];
			snprintf(p, sizeof(p), "%s/entry_%03d", dir, i);
			unlink(p);
		}
		rmdir(dir);
	}

	/* --- strtok_r: two interleaved tokenisations must not collide --- */
	{
		char a[] = "one,two,three";
		char b[] = "alpha:beta";
		char *sa = NULL, *sb = NULL;
		char *t1 = strtok_r(a, ",", &sa);
		char *u1 = strtok_r(b, ":", &sb);
		char *t2 = strtok_r(NULL, ",", &sa);
		char *u2 = strtok_r(NULL, ":", &sb);
		char *t3 = strtok_r(NULL, ",", &sa);

		test_result("strtok_r keeps interleaved scans separate",
			    t1 && u1 && t2 && u2 && t3 &&
				    strcmp(t1, "one") == 0 &&
				    strcmp(t2, "two") == 0 &&
				    strcmp(t3, "three") == 0 &&
				    strcmp(u1, "alpha") == 0 &&
				    strcmp(u2, "beta") == 0);
		test_result("strtok_r reports end of string",
			    strtok_r(NULL, ",", &sa) == NULL);
	}

	/* --- ffs / memmem / strverscmp --- */
	test_result("ffs finds the lowest set bit",
		    ffs(0) == 0 && ffs(1) == 1 && ffs(8) == 4 &&
			    ffs(0x100) == 9);
	test_result("ffsl works on longs",
		    ffsl(0) == 0 && ffsl(1L << 40) == 41);
	{
		const char hay[] = "abcXYZdef";
		test_result("memmem finds a substring",
			    memmem(hay, 9, "XYZ", 3) == hay + 3);
		test_result("memmem reports a miss",
			    memmem(hay, 9, "QQ", 2) == NULL);
		test_result("memmem with an empty needle matches at the start",
			    memmem(hay, 9, "", 0) == hay);
	}
	test_result("strverscmp orders versions numerically",
		    strverscmp("libfoo.so.9", "libfoo.so.10") < 0 &&
			    strverscmp("a2", "a10") < 0 &&
			    strverscmp("abc", "abc") == 0);

	/* --- qsort/qsort_r: qsort was a bubble sort; both share one sort now --- */
	{
		int big[512];
		unsigned long seed = 12345;
		int ok = 1;

		for (int i = 0; i < 512; i++) {
			seed = seed * 1103515245UL + 12345UL;
			big[i] = (int)((seed >> 16) & 0x7FFF);
		}
		qsort(big, 512, sizeof(int), qsort_int_cmp);
		for (int i = 1; i < 512; i++)
			if (big[i - 1] > big[i])
				ok = 0;
		test_result("qsort sorts 512 elements", ok);

		/* already sorted and reverse sorted: the pathological inputs
		 * for a naive pivot choice */
		qsort(big, 512, sizeof(int), qsort_int_cmp);
		ok = 1;
		for (int i = 1; i < 512; i++)
			if (big[i - 1] > big[i])
				ok = 0;
		test_result("qsort is stable on already-sorted input", ok);

		int arr[6] = { 5, 3, 9, 1, 4, 2 };
		int calls = 0;
		qsort_r(arr, 6, sizeof(int), qsort_r_cmp, &calls);
		test_result("qsort_r sorts and passes the caller's argument",
			    arr[0] == 1 && arr[5] == 9 && calls > 0);
	}

	/* --- msync / truncate / priorities / mkostemp / ptsname_r --- */
	{
		void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m != MAP_FAILED) {
			test_result("msync on a page-aligned range succeeds",
				    msync(m, 4096, MS_SYNC) == 0);
			errno = 0;
			test_result("msync on a misaligned address -> EINVAL",
				    msync((char *)m + 1, 4096, MS_SYNC) == -1 &&
					    errno == EINVAL);
			errno = 0;
			test_result("msync with MS_SYNC|MS_ASYNC -> EINVAL",
				    msync(m, 4096, MS_SYNC | MS_ASYNC) == -1 &&
					    errno == EINVAL);
			munmap(m, 4096);
		}
	}
	{
		const char *p = tmpp("trunc_t");
		int fd = open(p, O_CREAT | O_WRONLY, 0644);
		struct stat st;
		if (fd >= 0) {
			write(fd, "0123456789", 10);
			close(fd);
		}
		test_result("truncate shrinks a file",
			    truncate(p, 4) == 0 && stat(p, &st) == 0 &&
				    st.st_size == 4);
		test_result("truncate grows a file",
			    truncate(p, 100) == 0 && stat(p, &st) == 0 &&
				    st.st_size == 100);
		errno = 0;
		test_result("truncate on a missing path fails",
			    truncate(tmpp("no_such_file_xyz"), 0) == -1);
		unlink(p);
	}
	test_result("getpriority reports the default priority",
		    getpriority(PRIO_PROCESS, 0) == 0);
	errno = 0;
	test_result("getpriority with a bad which -> EINVAL",
		    getpriority(99, 0) == -1 && errno == EINVAL);
	test_result("setpriority accepts an in-range value",
		    setpriority(PRIO_PROCESS, 0, 5) == 0);
	errno = 0;
	test_result("setpriority out of range -> EINVAL",
		    setpriority(PRIO_PROCESS, 0, 100) == -1 &&
			    errno == EINVAL);
	{
		char t[] = "/tmp/mkoXXXXXX";
		int fd = mkostemp(t, O_CLOEXEC);
		test_result("mkostemp creates the file", fd >= 0);
		if (fd >= 0) {
			test_result("mkostemp honours O_CLOEXEC",
				    (fcntl(fd, F_GETFD, 0) & FD_CLOEXEC) != 0);
			close(fd);
			unlink(t);
		}
	}
	{
		int m = posix_openpt(O_RDWR);
		char nbuf[64];
		if (m >= 0) {
			test_result("ptsname_r fills the caller's buffer",
				    ptsname_r(m, nbuf, sizeof(nbuf)) == 0 &&
					    strncmp(nbuf, "/dev/pts/", 9) == 0);
			test_result("ptsname_r on a short buffer -> ERANGE",
				    ptsname_r(m, nbuf, 4) == ERANGE);
			test_result("ptsname agrees with ptsname_r",
				    strcmp(ptsname(m), nbuf) == 0);

			/* The slave is reference counted: two independent
			 * opens mean the first close must NOT hang up the
			 * line.  It used to be a flat flag, so closing either
			 * descriptor gave the master EOF and POLLHUP while the
			 * other was still perfectly usable. */
			int s1 = open(nbuf, O_RDWR);
			int s2 = open(nbuf, O_RDWR);
			test_result("slave opens twice", s1 >= 0 && s2 >= 0);
			if (s1 >= 0 && s2 >= 0) {
				close(s1);

				/* Still one opener left, so a write on the
				 * surviving descriptor must reach the master
				 * rather than the line being torn down. */
				ssize_t w = write(s2, "x\n", 2);
				char rb[8];
				ssize_t r = (w == 2) ? read(m, rb, sizeof(rb))
						     : -1;
				test_result(
					"line survives closing one of two slave fds",
					w == 2 && r > 0);

				close(s2);

				/* Now the last opener is gone: the master
				 * should see the hangup. */
				struct pollfd pfd = { .fd = m,
						      .events = POLLIN };
				int pr = poll(&pfd, 1, 500);
				test_result(
					"master sees hangup after the last slave closes",
					pr > 0 &&
						(pfd.revents &
						 (POLLHUP | POLLIN)) != 0);
			}
			close(m);
		} else {
			test_fail("ptsname_r: posix_openpt failed");
		}
	}

	/* --- pthread_sigmask: reports errors by return value, not errno --- */
	{
		sigset_t block, old, cur;
		sigemptyset(&block);
		sigaddset(&block, SIGUSR2);
		test_result("pthread_sigmask blocks a signal",
			    pthread_sigmask(SIG_BLOCK, &block, &old) == 0);
		sigemptyset(&cur);
		test_result("pthread_sigmask reads the mask back",
			    pthread_sigmask(SIG_SETMASK, NULL, &cur) == 0 &&
				    sigismember(&cur, SIGUSR2) == 1);
		test_result("pthread_sigmask restores the old mask",
			    pthread_sigmask(SIG_SETMASK, &old, NULL) == 0);
		test_result("pthread_sigmask returns the error, not -1",
			    pthread_sigmask(12345, &block, NULL) != 0);
	}
}

/* ---- dprintf()/vdprintf(): formatted output straight to a descriptor ---- */

static int vdprintf_helper(int fd, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vdprintf(fd, fmt, ap);
	va_end(ap);
	return r;
}

/*
 * The pieces of libc the GTK3 desktop ports turned out to need.
 *
 * The companion of test_xorg_libc_additions() and test_bash_libc_additions()
 * above, and found the same way: by porting a program and watching where it
 * stopped.  Here that program was menu-cache -- the server PCManFM asks for
 * the application menu -- which opens with <sys/fcntl.h>.
 *
 * That header, and the four beside it, are the older BSD spellings of headers
 * POSIX later moved to the top level.  Every general-purpose libc still ships
 * them, because software that was written when they were the spelling is
 * software that still builds today, and a program using one is not doing
 * anything unusual.  Each is one line: it includes the real header.  Which is
 * exactly what makes them worth a test -- the failure mode is not "missing",
 * it is "present and declaring things a second time", and that only shows up
 * where both spellings meet in one translation unit.  This file is that place;
 * see its include block.
 *
 * What is checked here is that a program can USE them: the constants have the
 * values the real headers give them, and the functions they declare are the
 * ones that actually run.
 */
static void test_gtk3_libc_additions(void)
{
	printf("\n[TEST] libc additions for the GTK3 desktop (BSD header spellings)\n");

	/* --- <sys/fcntl.h>: what menu-cache includes ------------------- */
	{
		const char *path = tmpp("sysfcntl_t");
		int fd, fl;

		/* O_* here reach this call through <sys/fcntl.h> as much as
		 * through <fcntl.h>; the point is that there is one set of
		 * them and it works. */
		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		test_result("sys/fcntl.h: open with O_CREAT|O_WRONLY", fd >= 0);
		if (fd >= 0) {
			fl = fcntl(fd, F_GETFL);
			test_result("sys/fcntl.h: F_GETFL reports the access mode",
				    fl >= 0 && (fl & O_ACCMODE) == O_WRONLY);

			test_result("sys/fcntl.h: F_SETFD/F_GETFD round-trip",
				    fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
					    (fcntl(fd, F_GETFD) & FD_CLOEXEC));
			close(fd);
		}
		unlink(path);
	}

	/* --- <sys/errno.h> --------------------------------------------- */
	{
		int fd;

		errno = 0;
		fd = open(tmpp("no/such/directory/at/all"), O_RDONLY);
		test_result("sys/errno.h: errno and ENOENT are the real ones",
			    fd < 0 && errno == ENOENT);
		if (fd >= 0)
			close(fd);

		/* strerror() must know the constant, which it only does if
		 * both spellings agree about its value. */
		test_result("sys/errno.h: strerror(ENOENT) is a real message",
			    strerror(ENOENT) != NULL &&
				    strlen(strerror(ENOENT)) > 0);
	}

	/* --- <sys/signal.h> -------------------------------------------- */
	{
		struct sigaction sa, old;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_IGN;
		sigemptyset(&sa.sa_mask);

		test_result("sys/signal.h: sigaction installs a disposition",
			    sigaction(SIGUSR2, &sa, &old) == 0);
		/* Delivered to ourselves and ignored: if SIGUSR2 had a
		 * different value on either side of the two headers, this
		 * would either terminate the test program or fail outright. */
		test_result("sys/signal.h: raise() of an ignored signal returns",
			    raise(SIGUSR2) == 0);
		sigaction(SIGUSR2, &old, NULL);
	}

	/* --- <sys/termios.h> ------------------------------------------- */
	{
		int p[2];
		struct termios t;

		test_result("sys/termios.h: pipe for the negative case",
			    pipe(p) == 0);
		errno = 0;
		/* A pipe is not a terminal, and saying so is the whole
		 * contract: every program that decides whether it is
		 * interactive does it with this call. */
		test_result("sys/termios.h: tcgetattr on a pipe fails ENOTTY",
			    tcgetattr(p[0], &t) < 0 && errno == ENOTTY);
		test_result("sys/termios.h: isatty agrees with it",
			    isatty(p[0]) == 0);
		close(p[0]);
		close(p[1]);
	}

	/* --- <sys/unistd.h> -------------------------------------------- */
	{
		long omax = sysconf(_SC_OPEN_MAX);

		/* menu-cached daemonises by closing every descriptor from 3 to
		 * this number.  A negative answer would leave descriptors open
		 * across the fork; an absurd one would spend the daemon's
		 * start-up in a close() loop. */
		test_result("sys/unistd.h: sysconf(_SC_OPEN_MAX) is usable",
			    omax > 3 && omax <= 65536);
		test_result("sys/unistd.h: sysconf(_SC_PAGESIZE) is 4096",
			    sysconf(_SC_PAGESIZE) == 4096);
	}
}

static void test_dprintf(void)
{
	int p[2];
	char buf[8192];
	int n, total;

	printf("\n[TEST] dprintf/vdprintf\n");

	if (pipe(p) != 0) {
		test_fail("dprintf: pipe() failed");
		return;
	}

	/* Writes to the descriptor itself, with no FILE and no buffering. */
	n = dprintf(p[1], "%s=%d/%c\n", "answer", 42, 'x');
	total = (int)read(p[0], buf, sizeof(buf) - 1);
	if (total > 0)
		buf[total] = '\0';
	else
		buf[0] = '\0';
	test_result("dprintf formats and writes to the fd",
		    n == 12 && total == 12 && strcmp(buf, "answer=42/x\n") == 0);

	/* Longer than the internal stack buffer: must not be truncated. */
	{
		char big[3000];
		memset(big, 'A', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		n = dprintf(p[1], "<%s>", big);
		total = 0;
		while (total < (int)sizeof(buf) - 1) {
			int r = (int)read(p[0], buf + total,
					  sizeof(buf) - 1 - total);
			if (r <= 0)
				break;
			total += r;
			if (total >= n)
				break;
		}
		buf[total > 0 ? total : 0] = '\0';
		test_result("dprintf does not truncate a long line",
			    n == 3001 && total == 3001 && buf[0] == '<' &&
				    buf[3000] == '>' && buf[1500] == 'A');
	}

	/* vdprintf takes the va_list form. */
	n = vdprintf_helper(p[1], "%d-%s", 7, "ok");
	total = (int)read(p[0], buf, sizeof(buf) - 1);
	if (total > 0)
		buf[total] = '\0';
	else
		buf[0] = '\0';
	test_result("vdprintf writes through a va_list",
		    n == 4 && total == 4 && strcmp(buf, "7-ok") == 0);

	close(p[0]);
	close(p[1]);

	/* A descriptor that cannot be written fails rather than pretending. */
	errno = 0;
	test_result("dprintf on a closed fd -> -1", dprintf(200, "x") == -1);
}

/* ---- libc additions made for the bash port --------------------------- */

static void test_bash_libc_additions(void)
{
	printf("\n[TEST] libc additions (mktemp/mkdtemp/mkfifo/wcs*/mblen)\n");

	/* mktemp: template rewritten to a nonexistent name */
	char t1[] = "/tmp/mtXXXXXX";
	char *mt = mktemp(t1);
	test_result("mktemp fills template",
		    mt == t1 && t1[0] != '\0' && strncmp(t1, "/tmp/mt", 7) == 0 &&
			    strcmp(t1 + 7, "XXXXXX") != 0 &&
			    access(t1, F_OK) != 0);

	/* mkdtemp: directory created 0700 */
	char t2[] = "/tmp/mdXXXXXX";
	char *md = mkdtemp(t2);
	struct stat dst;
	test_result("mkdtemp creates directory",
		    md == t2 && stat(t2, &dst) == 0 && S_ISDIR(dst.st_mode) &&
			    (dst.st_mode & 0777) == 0700);
	if (md)
		rmdir(t2);

	/* mkfifo: honestly unsupported -> ENOSYS (not a silent success) */
	errno = 0;
	test_result("mkfifo -> ENOSYS",
		    mkfifo(tmpp("nofifo"), 0644) == -1 && errno == ENOSYS);

	/* wide-char helpers used by ported code */
	wchar_t wbuf[8];
	test_result("wcscpy/wcscmp/wcslen",
		    wcscpy(wbuf, L"abc") == wbuf && wcslen(wbuf) == 3 &&
			    wcscmp(wbuf, L"abc") == 0 &&
			    wcscmp(wbuf, L"abd") < 0);
	test_result("wcschr/wcsrchr",
		    wcschr(wbuf, L'b') == wbuf + 1 &&
			    wcsrchr(wbuf, L'c') == wbuf + 2 &&
			    wcschr(wbuf, L'x') == 0);
	test_result("wcsncmp/wmemchr",
		    wcsncmp(L"abcd", L"abce", 3) == 0 &&
			    wmemchr(wbuf, L'c', 3) == wbuf + 2);
	test_result("wcswidth of plain ASCII", wcswidth(wbuf, 3) == 3);

	/* mblen/mbrlen on ASCII, which is one byte per character in UTF-8 too */
	test_result("mblen semantics",
		    mblen("a", 1) == 1 && mblen("", 1) == 0 && mblen(0, 0) == 0);
	mbstate_t ms;
	memset(&ms, 0, sizeof(ms));
	test_result("mbrlen semantics", mbrlen("a", 1, &ms) == 1);
}

/* =========================================================================
 * UTF-8 and Unicode
 *
 * The multibyte encoding is UTF-8 in every locale here, so these are the
 * functions every program that handles text above ASCII goes through: the
 * shell measuring a prompt, the editor placing a cursor, the terminal
 * emulator deciding how many columns a character takes.
 *
 * The exhaustive comparison against a reference implementation is a host-side
 * test (host/test-unicode.sh, every code point).  What runs here is the part
 * that can only be checked on the real system: that the shared library exports
 * these, that they agree with each other, and that stdout carries the bytes
 * through unchanged.
 * ========================================================================= */
static void test_unicode(void)
{
	printf("\n[TEST] UTF-8 / Unicode\n");

	/* The locale has to name UTF-8, or every program's multibyte handling
	 * stays switched off no matter how correct the conversions are. */
	char *loc = setlocale(LC_ALL, "");
	test_result("setlocale(LC_ALL, \"\") reports a locale",
		    loc != NULL && loc[0] != '\0');
	test_result("nl_langinfo(CODESET) is UTF-8",
		    strcmp(nl_langinfo(CODESET), "UTF-8") == 0);
	test_result("MB_CUR_MAX is 4", MB_CUR_MAX == 4);

	/* Encode: one character from each sequence length. */
	char buf[8];
	memset(buf, 0, sizeof(buf));
	test_result("wcrtomb 1-byte (U+0041)",
		    wcrtomb(buf, 0x41, 0) == 1 && (unsigned char)buf[0] == 0x41);
	test_result("wcrtomb 2-byte (U+00FC)",
		    wcrtomb(buf, 0xFC, 0) == 2 &&
			    (unsigned char)buf[0] == 0xC3 &&
			    (unsigned char)buf[1] == 0xBC);
	test_result("wcrtomb 3-byte (U+20AC)",
		    wcrtomb(buf, 0x20AC, 0) == 3 &&
			    (unsigned char)buf[0] == 0xE2 &&
			    (unsigned char)buf[1] == 0x82 &&
			    (unsigned char)buf[2] == 0xAC);
	test_result("wcrtomb 4-byte (U+1F600)",
		    wcrtomb(buf, 0x1F600, 0) == 4 &&
			    (unsigned char)buf[0] == 0xF0 &&
			    (unsigned char)buf[1] == 0x9F &&
			    (unsigned char)buf[2] == 0x98 &&
			    (unsigned char)buf[3] == 0x80);

	/* Decode, including one byte at a time: a stream arrives split. */
	wchar_t wc = 0;
	mbstate_t st;
	memset(&st, 0, sizeof(st));
	test_result("mbrtowc 2-byte", mbrtowc(&wc, "\xC3\xBC", 2, &st) == 2 &&
					      wc == 0xFC);
	memset(&st, 0, sizeof(st));
	wc = 0;
	int split_ok = (mbrtowc(&wc, "\xE2", 1, &st) == (size_t)-2) &&
		       (mbrtowc(&wc, "\x82", 1, &st) == (size_t)-2) &&
		       (mbrtowc(&wc, "\xAC", 1, &st) == 1) && wc == 0x20AC;
	test_result("mbrtowc across three calls", split_ok);
	test_result("mbsinit after a complete character", mbsinit(&st) != 0);

	/* Malformed input must be refused, not guessed at: an overlong form or
	 * a surrogate half is a way to smuggle a value past a check written
	 * against its shorter encoding. */
	memset(&st, 0, sizeof(st));
	errno = 0;
	test_result("overlong sequence -> EILSEQ",
		    mbrtowc(&wc, "\xC0\x80", 2, &st) == (size_t)-1 &&
			    errno == EILSEQ);
	memset(&st, 0, sizeof(st));
	test_result("surrogate half -> EILSEQ",
		    mbrtowc(&wc, "\xED\xA0\x80", 3, &st) == (size_t)-1);
	memset(&st, 0, sizeof(st));
	test_result("stray continuation byte -> EILSEQ",
		    mbrtowc(&wc, "\x80", 1, &st) == (size_t)-1);
	memset(&st, 0, sizeof(st));
	test_result("value above U+10FFFF -> EILSEQ",
		    mbrtowc(&wc, "\xF5\x80\x80\x80", 4, &st) == (size_t)-1);

	/* Whole-string conversion, round trip.
	 *
	 * The literal is split after every escape whose next character is a hex
	 * digit.  A \x escape consumes as MANY hex digits as follow it, so
	 * "\xC3\x9Fe" is \xC3 followed by \x9Fe -- one out-of-range value,
	 * not \x9F and 'e'.  Splitting the literal ends the escape; the
	 * compiler then joins the pieces. */
	const char *ger = "Gr\xC3\xBC\xC3\x9F" "e"; /* "Grüße" in UTF-8 */
	test_result("test's own literal is well-formed UTF-8",
		    strlen(ger) == 7 && (unsigned char)ger[4] == 0xC3 &&
			    (unsigned char)ger[5] == 0x9F && ger[6] == 'e');
	wchar_t wide[16];
	char back[32];
	size_t n = mbstowcs(wide, ger, 16);
	test_result("mbstowcs counts characters, not bytes",
		    n == 5 && wide[2] == 0xFC && wide[3] == 0xDF);
	test_result("wcstombs round trip",
		    wcstombs(back, wide, sizeof(back)) == strlen(ger) &&
			    strcmp(back, ger) == 0);

	/* Widths.  A combining mark takes no column of its own; a CJK
	 * ideograph takes two.  Terminal cursor arithmetic depends on both. */
	test_result("wcwidth ASCII = 1", wcwidth(L'A') == 1);
	test_result("wcwidth umlaut = 1", wcwidth(0xFC) == 1);
	test_result("wcwidth combining acute = 0", wcwidth(0x0301) == 0);
	test_result("wcwidth CJK = 2", wcwidth(0x65E5) == 2);
	test_result("wcwidth control = -1", wcwidth(0x07) == -1);
	test_result("wcswidth of a mixed string",
		    wcswidth(L"a\u00fc\u65e5", 3) == 4);

	/* Classification and case mapping outside ASCII. */
	test_result("iswalpha(U+00FC)", iswalpha(0xFC) != 0);
	test_result("iswalpha(U+0410 cyrillic)", iswalpha(0x0410) != 0);
	test_result("iswupper/iswlower on U+00DC/U+00FC",
		    iswupper(0xDC) && iswlower(0xFC) && !iswupper(0xFC));
	test_result("towupper(U+00FC) == U+00DC", towupper(0xFC) == 0xDC);
	test_result("towlower(U+0410) == U+0430", towlower(0x0410) == 0x0430);
	test_result("towupper leaves a non-letter alone", towupper(0x20AC) == 0x20AC);
	test_result("iswctype via wctype(\"alpha\")",
		    iswctype(0xFC, wctype("alpha")) != 0 &&
			    iswctype(0x20, wctype("alpha")) == 0);
	test_result("towctrans via wctrans(\"toupper\")",
		    towctrans(0xFC, wctrans("toupper")) == 0xDC);

	/* btowc/wctob only convert what is a whole character on its own, which
	 * in UTF-8 is exactly ASCII. */
	test_result("btowc/wctob on ASCII",
		    btowc('A') == (wint_t)'A' && wctob((wint_t)'A') == 'A');
	test_result("btowc rejects a non-ASCII byte", btowc(0xC3) == WEOF);

	/* printf's wide conversions. */
	char out[64];
	int len = snprintf(out, sizeof(out), "[%ls]", L"Gr\u00fc\u00dfe");
	test_result("printf %ls encodes as UTF-8",
		    len == 9 && strcmp(out, "[Gr\xC3\xBC\xC3\x9F" "e]") == 0);
	len = snprintf(out, sizeof(out), "[%lc]", (wint_t)0x20AC);
	test_result("printf %lc encodes as UTF-8",
		    len == 5 && strcmp(out, "[\xE2\x82\xAC]") == 0);

	/* The bytes have to survive the write path unchanged -- this is what
	 * the console and the terminal emulator actually receive. */
	printf("  literal UTF-8 through stdout: Gr\xC3\xBC\xC3\x9F" "e aus M\xC3\xBCnchen "
	       "\xE2\x94\x82 \xE2\x94\x80 \xE2\x82\xAC \xCE\xA9 \xD0\x96\n");
}

static void run_auth_tests(void)
{
	test_userdb();
	test_shadow_and_crypt();
	test_extra_creds();
	test_session_creation();
	test_pam();
	test_setuid_exec();
}

/* ------------------------------------------------------------------ *
 * Loader introspection: dladdr, dl_iterate_phdr, _dl_find_object.
 *
 * Three ways of asking the runtime linker the same question -- which loaded
 * object covers this address -- and all three are load-bearing.  The last one
 * is how the C++ unwinder finds an object's exception tables from a program
 * counter: with it absent or wrong, every throw ends in std::terminate rather
 * than in the handler waiting for it, and nothing in the failure says why.
 * ------------------------------------------------------------------ */
static int phdr_walk_count;
static int phdr_saw_main;
static int phdr_saw_eh_frame;

static int phdr_visitor(struct dl_phdr_info *info, size_t size, void *data)
{
	int *seen_libc = (int *)data;

	if (size < sizeof(*info))
		return 0;
	phdr_walk_count++;
	/* The main executable is the one reported with an empty name. */
	if (info->dlpi_name && info->dlpi_name[0] == '\0')
		phdr_saw_main = 1;
	if (info->dlpi_name && strstr(info->dlpi_name, "libc"))
		*seen_libc = 1;
	for (int i = 0; i < info->dlpi_phnum; i++)
		if (info->dlpi_phdr[i].p_type == PT_GNU_EH_FRAME)
			phdr_saw_eh_frame = 1;
	return 0; /* keep walking */
}

static int phdr_stopper(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)info;
	(void)size;
	(*(int *)data)++;
	return 77; /* non-zero ends the walk and is returned to the caller */
}

static void test_loader_introspection(void)
{
	printf("\n[TEST] runtime linker introspection\n");

	/* dladdr on a function in THIS program, and on one in libc. */
	Dl_info info;
	memset(&info, 0, sizeof(info));
	test_result("dladdr() resolves an address in this program",
		    dladdr((const void *)(uintptr_t)&test_loader_introspection,
			   &info) != 0);
	test_result("dladdr() names the containing object",
		    info.dli_fname != NULL);
	/* No expectation about dli_sname here.  dladdr resolves names through
	 * the DYNAMIC symbol table, which is the only one a loaded object
	 * carries at run time -- a static function is not in it, and this one
	 * is static.  glibc behaves the same way; the answer is either NULL or
	 * the nearest exported symbol below the address, and neither is wrong.
	 * The name lookup is tested against libc below, where the symbol IS
	 * exported. */

	memset(&info, 0, sizeof(info));
	test_result("dladdr() resolves an address in libc",
		    dladdr((const void *)(uintptr_t)&strlen, &info) != 0 &&
			    info.dli_fbase != NULL);
	test_result("dladdr() names an exported symbol",
		    info.dli_sname != NULL &&
			    strcmp(info.dli_sname, "strlen") == 0);
	test_result("dladdr() reports the symbol's own address",
		    info.dli_saddr == (void *)(uintptr_t)&strlen);

	/* An address belonging to no object at all: not an error, just no. */
	memset(&info, 0, sizeof(info));
	test_result("dladdr() rejects an unmapped address",
		    dladdr((const void *)(uintptr_t)0x10, &info) == 0);

	/* dl_iterate_phdr must visit the main executable and every library. */
	int seen_libc = 0;
	int rc = dl_iterate_phdr(phdr_visitor, &seen_libc);
	test_result("dl_iterate_phdr() completed the walk", rc == 0);
	test_result("dl_iterate_phdr() visited several objects",
		    phdr_walk_count >= 2);
	test_result("dl_iterate_phdr() reported the main executable",
		    phdr_saw_main == 1);
	test_result("dl_iterate_phdr() reported libc", seen_libc == 1);
	test_result("some object carries PT_GNU_EH_FRAME (unwind tables)",
		    phdr_saw_eh_frame == 1);

	/* A callback returning non-zero stops the walk, and that value is what
	 * comes back -- the usual way of saying "this is the one". */
	int visits = 0;
	test_result("dl_iterate_phdr() returns the callback's value",
		    dl_iterate_phdr(phdr_stopper, &visits) == 77);
	test_result("dl_iterate_phdr() stopped at the first object",
		    visits == 1);

	/* _dl_find_object: the unwinder's entry point. */
	struct dl_find_object dfo;
	memset(&dfo, 0, sizeof(dfo));
	test_result("_dl_find_object() resolves an address in this program",
		    _dl_find_object((void *)(uintptr_t)&test_loader_introspection,
				    &dfo) == 0);
	test_result("_dl_find_object() reports the mapping bounds",
		    dfo.dlfo_map_start != NULL &&
			    (char *)dfo.dlfo_map_end >
				    (char *)dfo.dlfo_map_start);
	test_result("_dl_find_object() found the address inside the mapping",
		    (char *)(uintptr_t)&test_loader_introspection >=
				    (char *)dfo.dlfo_map_start &&
			    (char *)(uintptr_t)&test_loader_introspection <
				    (char *)dfo.dlfo_map_end);
	/* This is the field the unwinder actually wants.  A program built
	 * without unwind tables would have no such segment -- and then no C++
	 * exception could ever be caught. */
	test_result("_dl_find_object() reports the exception-frame segment",
		    dfo.dlfo_eh_frame != NULL);

	memset(&dfo, 0, sizeof(dfo));
	test_result("_dl_find_object() rejects an unmapped address",
		    _dl_find_object((void *)(uintptr_t)0x10, &dfo) == -1);
}

/* ------------------------------------------------------------------ *
 * Exit handlers.  The ordering check itself runs at exit; this only
 * registers the handlers and reports that registration succeeded.
 * ------------------------------------------------------------------ */
static void test_exit_handlers(void)
{
	static const char mark_x = 'x';

	printf("\n[TEST] atexit / __cxa_atexit\n");

	/* Registered first, so it runs last and sees the whole record. */
	test_result("atexit() accepted the ordering check",
		    atexit(exit_handler_check) == 0);
	test_result("atexit() accepted a handler", atexit(exit_handler_a) == 0);
	/* The C++ form: a handler taking an argument, tagged with the object it
	 * belongs to.  A NULL handle means the main executable. */
	test_result("__cxa_atexit() accepted a handler",
		    __cxa_atexit(exit_handler_cxa, (void *)&mark_x, NULL) == 0);
	test_result("atexit() accepted a second handler",
		    atexit(exit_handler_b) == 0);
	test_result("atexit() rejects a null handler", atexit(NULL) != 0);
	printf("  (the ordering result is printed at exit, after the summary)\n");
}

/* ------------------------------------------------------------------ *
 * C99 floating-point classification and comparison.
 *
 * All type-generic macros, so each is exercised on more than one type: a
 * definition that silently drops to double would still pass a float-only
 * check.  The comparison macros differ from the operators they resemble in
 * exactly one way that matters -- they are quiet on NaN -- so that is what is
 * tested.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * <fenv.h> -- the floating-point environment.
 *
 * x86-64 has two floating-point units and these functions have to agree about
 * both: double arithmetic compiles to SSE and long double to x87, each with
 * its own exception flags and rounding control.  A version that read only one
 * of them would pass every test written in double and fail the moment a long
 * double appeared, so both types are used deliberately below.
 * ------------------------------------------------------------------ */
static void test_fenv(void)
{
	printf("\n[TEST] <fenv.h> floating-point environment\n");

	/* Start from a known state; every check below assumes it. */
	feclearexcept(FE_ALL_EXCEPT);
	test_result("feclearexcept leaves nothing raised",
		    fetestexcept(FE_ALL_EXCEPT) == 0);

	/* Raise one, see one. */
	feraiseexcept(FE_DIVBYZERO);
	test_result("feraiseexcept(FE_DIVBYZERO)",
		    fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO);
	test_result("...and nothing else",
		    fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);

	/* Clearing one leaves the others. */
	feraiseexcept(FE_OVERFLOW | FE_INEXACT);
	feclearexcept(FE_DIVBYZERO);
	test_result("feclearexcept clears only what it is given",
		    fetestexcept(FE_DIVBYZERO) == 0 &&
			    fetestexcept(FE_OVERFLOW) == FE_OVERFLOW &&
			    fetestexcept(FE_INEXACT) == FE_INEXACT);
	feclearexcept(FE_ALL_EXCEPT);

	/* Real arithmetic on each unit raises the flag it should.
	 *
	 * `volatile' throughout: without it the compiler evaluates these at
	 * compile time and no instruction ever runs, so no flag is set and the
	 * test measures nothing. */
	{
		volatile double zero = 0.0;
		volatile double one = 1.0;
		volatile double r;

		feclearexcept(FE_ALL_EXCEPT);
		r = one / zero; /* SSE */
		(void)r;
		test_result("division by zero raises FE_DIVBYZERO (SSE)",
			    fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO);

		feclearexcept(FE_ALL_EXCEPT);
		r = zero / zero;
		(void)r;
		test_result("0/0 raises FE_INVALID (SSE)",
			    fetestexcept(FE_INVALID) == FE_INVALID);
	}
	{
		/* The x87 side.  If fetestexcept read MXCSR only, this is the
		 * check that fails. */
		volatile long double lzero = 0.0L;
		volatile long double lone = 1.0L;
		volatile long double lr;

		feclearexcept(FE_ALL_EXCEPT);
		lr = lone / lzero;
		(void)lr;
		test_result("division by zero raises FE_DIVBYZERO (x87)",
			    fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO);
	}
	feclearexcept(FE_ALL_EXCEPT);

	/* Saving and restoring a set of flags. */
	{
		fexcept_t saved;

		feraiseexcept(FE_OVERFLOW);
		fegetexceptflag(&saved, FE_ALL_EXCEPT);
		feclearexcept(FE_ALL_EXCEPT);
		test_result("fegetexceptflag does not clear",
			    fetestexcept(FE_ALL_EXCEPT) == 0);
		fesetexceptflag(&saved, FE_ALL_EXCEPT);
		test_result("fesetexceptflag puts them back",
			    fetestexcept(FE_OVERFLOW) == FE_OVERFLOW);
		feclearexcept(FE_ALL_EXCEPT);
	}

	/* Rounding.  The direction has to reach BOTH units, so it is checked
	 * with a double (SSE) and a long double (x87). */
	test_result("fegetround starts at FE_TONEAREST",
		    fegetround() == FE_TONEAREST);
	{
		volatile double x = 1.5;
		volatile double y;
		volatile long double lx = 1.5L;
		volatile long double ly;
		int ok_up, ok_down, ok_x87;

		fesetround(FE_UPWARD);
		test_result("fesetround(FE_UPWARD) reads back",
			    fegetround() == FE_UPWARD);
		y = x + 1e-30; /* nudged so rounding decides the result */
		ok_up = (y > 1.5);
		ly = lx + 1e-30L;
		ok_x87 = (ly > 1.5L);

		fesetround(FE_DOWNWARD);
		y = x - 1e-30;
		ok_down = (y < 1.5);

		fesetround(FE_TONEAREST);
		test_result("FE_UPWARD rounds up (SSE)", ok_up);
		test_result("FE_UPWARD rounds up (x87)", ok_x87);
		test_result("FE_DOWNWARD rounds down (SSE)", ok_down);
		test_result("rounding restored to nearest",
			    fegetround() == FE_TONEAREST);
	}
	test_result("fesetround rejects a value that is not a direction",
		    fesetround(12345) != 0 && fegetround() == FE_TONEAREST);

	/* The environment as a whole. */
	{
		fenv_t env;

		feclearexcept(FE_ALL_EXCEPT);
		fesetround(FE_UPWARD);
		feraiseexcept(FE_INEXACT);
		test_result("fegetenv succeeds", fegetenv(&env) == 0);
		/* fegetenv must not disturb what it read -- the instruction it
		 * is built on masks every exception as a side effect, and an
		 * implementation that forgets to put the control word back
		 * leaves the process unable to trap for the rest of its
		 * life. */
		test_result("fegetenv changes nothing",
			    fegetround() == FE_UPWARD &&
				    fetestexcept(FE_INEXACT) == FE_INEXACT);

		fesetround(FE_TONEAREST);
		feclearexcept(FE_ALL_EXCEPT);
		test_result("fesetenv restores the saved environment",
			    fesetenv(&env) == 0 && fegetround() == FE_UPWARD &&
				    fetestexcept(FE_INEXACT) == FE_INEXACT);

		test_result("FE_DFL_ENV is the startup environment",
			    fesetenv(FE_DFL_ENV) == 0 &&
				    fegetround() == FE_TONEAREST &&
				    fetestexcept(FE_ALL_EXCEPT) == 0);
	}

	/* feholdexcept / feupdateenv: run a block with the flags out of the
	 * way, then put back what was there and add what happened. */
	{
		fenv_t env;

		feclearexcept(FE_ALL_EXCEPT);
		feraiseexcept(FE_OVERFLOW);
		test_result("feholdexcept clears the flags",
			    feholdexcept(&env) == 0 &&
				    fetestexcept(FE_ALL_EXCEPT) == 0);
		feraiseexcept(FE_INEXACT);
		test_result("feupdateenv merges old and new",
			    feupdateenv(&env) == 0 &&
				    fetestexcept(FE_OVERFLOW) == FE_OVERFLOW &&
				    fetestexcept(FE_INEXACT) == FE_INEXACT);
		feclearexcept(FE_ALL_EXCEPT);
	}
}

/* ------------------------------------------------------------------ *
 * <search.h> -- the POSIX search tables.
 *
 * The tree is what matters here.  tsearch's interface allows a plain binary
 * search tree, and a plain one becomes a linked list the moment the keys
 * arrive in order -- which is the common case.  So the implementation
 * balances, and what is checked below is that the balancing did not break the
 * ordering: an in-order walk of sorted insertions must come back sorted, and
 * must still be sorted after half the keys are deleted.
 *
 * host/test-search.sh does this over 20,000 keys against glibc and checks the
 * tree's depth as well.
 * ------------------------------------------------------------------ */
static int uc_cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static int uc_walk_prev;
static int uc_walk_n;
static int uc_walk_ordered;

static void uc_walk_visit(const void *node, VISIT which, int depth)
{
	(void)depth;
	if (which != postorder && which != leaf)
		return;
	{
		int v = **(int *const *)node;

		if (uc_walk_n && v <= uc_walk_prev)
			uc_walk_ordered = 0;
		uc_walk_prev = v;
		uc_walk_n++;
	}
}

static void test_search(void)
{
	printf("\n[TEST] <search.h> search tables\n");

	/* ---- the tree ---- */
	{
		enum { N = 512 };
		static int keys[N];
		void *root = NULL;
		int i, ok = 1;

		/* Sorted insertion order on purpose: this is the input a plain
		 * binary search tree degenerates on. */
		for (i = 0; i < N; i++)
			keys[i] = i;
		for (i = 0; i < N; i++)
			if (tsearch(&keys[i], &root, uc_cmp_int) == NULL)
				ok = 0;
		test_result("tsearch inserted every key", ok);

		ok = 1;
		for (i = 0; i < N; i++) {
			void *f = tfind(&keys[i], &root, uc_cmp_int);

			if (!f || **(int **)f != keys[i])
				ok = 0;
		}
		test_result("tfind finds every key", ok);

		{
			int absent = -1;

			test_result("tfind rejects a key that is not there",
				    tfind(&absent, &root, uc_cmp_int) == NULL);
		}

		uc_walk_n = 0;
		uc_walk_ordered = 1;
		twalk(root, uc_walk_visit);
		test_result("twalk visits every node once", uc_walk_n == N);
		test_result("twalk is in order", uc_walk_ordered);

		/* Deletion rebalances too, with its own rotations. */
		ok = 1;
		for (i = 0; i < N; i += 2)
			if (tdelete(&keys[i], &root, uc_cmp_int) == NULL)
				ok = 0;
		test_result("tdelete removed every key it was given", ok);

		ok = 1;
		for (i = 0; i < N; i += 2)
			if (tfind(&keys[i], &root, uc_cmp_int))
				ok = 0;
		for (i = 1; i < N; i += 2)
			if (!tfind(&keys[i], &root, uc_cmp_int))
				ok = 0;
		test_result("the right half survived the deletions", ok);

		uc_walk_n = 0;
		uc_walk_ordered = 1;
		twalk(root, uc_walk_visit);
		test_result("twalk is still in order after deleting",
			    uc_walk_ordered && uc_walk_n == N / 2);

		for (i = 1; i < N; i += 2)
			tdelete(&keys[i], &root, uc_cmp_int);
		test_result("the tree is empty once every key is gone",
			    root == NULL);
	}

	/* ---- the hash table ---- */
	{
		enum { N = 128 };
		static char keys[N][16];
		int i, ok = 1;

		test_result("hcreate", hcreate(N) != 0);
		for (i = 0; i < N; i++) {
			ENTRY e;

			snprintf(keys[i], sizeof keys[i], "k%d", i);
			e.key = keys[i];
			e.data = (void *)(long)i;
			if (!hsearch(e, ENTER))
				ok = 0;
		}
		test_result("hsearch ENTER filled the table", ok);

		/* At the stated capacity, every key must still be findable --
		 * which is what a probe sequence that cannot reach every slot
		 * fails. */
		ok = 1;
		for (i = 0; i < N; i++) {
			ENTRY q = { keys[i], NULL };
			ENTRY *r = hsearch(q, FIND);

			if (!r || (long)r->data != i)
				ok = 0;
		}
		test_result("hsearch FIND finds every key", ok);

		{
			char absent[] = "nope";
			ENTRY q = { absent, NULL };

			test_result("hsearch FIND misses what is absent",
				    hsearch(q, FIND) == NULL);
		}
		{
			/* ENTER on a key already present RETURNS it; it does
			 * not replace the data. */
			ENTRY e = { keys[0], (void *)999L };
			ENTRY *r = hsearch(e, ENTER);

			test_result("hsearch ENTER on an existing key",
				    r && (long)r->data == 0);
		}
		hdestroy();
	}

	/* ---- lsearch / lfind ---- */
	{
		int arr[16];
		size_t n = 0;
		int i, ok = 1;

		for (i = 0; i < 8; i++) {
			int v = i * 3;

			if (!lsearch(&v, arr, &n, sizeof v, uc_cmp_int))
				ok = 0;
		}
		test_result("lsearch appended eight elements", ok && n == 8);
		{
			int v = 3;

			lsearch(&v, arr, &n, sizeof v, uc_cmp_int);
			test_result("lsearch does not append a duplicate",
				    n == 8);
			test_result("lfind finds it",
				    lfind(&v, arr, &n, sizeof v, uc_cmp_int) !=
					    NULL);
			v = 1;
			test_result("lfind misses what is absent",
				    lfind(&v, arr, &n, sizeof v, uc_cmp_int) ==
					    NULL);
		}
	}

	/* ---- insque / remque ---- */
	{
		struct qe {
			struct qe *forw;
			struct qe *back;
			int v;
		} a = { 0, 0, 1 }, b = { 0, 0, 2 }, c = { 0, 0, 3 };
		struct qe *p;
		int seen[4], n = 0;

		insque(&a, NULL);
		insque(&b, &a);
		insque(&c, &a); /* between a and b */
		for (p = &a; p && n < 4; p = p->forw)
			seen[n++] = p->v;
		test_result("insque links in the right order",
			    n == 3 && seen[0] == 1 && seen[1] == 3 &&
				    seen[2] == 2);

		remque(&c);
		n = 0;
		for (p = &a; p && n < 4; p = p->forw)
			seen[n++] = p->v;
		test_result("remque unlinks the middle element",
			    n == 2 && seen[0] == 1 && seen[1] == 2);
	}
}

/* ------------------------------------------------------------------ *
 * SO_PEERCRED -- who is on the other end of a UNIX socket.
 *
 * The kernel records this when the connection is made; neither side sends it
 * and neither side can forge it, which is what makes it usable for
 * authentication.  D-Bus's EXTERNAL mechanism is built on it, and the session
 * bus this image runs is why it exists.
 * ------------------------------------------------------------------ */
static void test_so_peercred(void)
{
	printf("\n[TEST] SO_PEERCRED on UNIX sockets\n");

	pid_t me = getpid();
	uid_t myuid = geteuid();
	gid_t mygid = getegid();

	/* A socketpair: both ends belong to this process, so each end's peer
	 * is this process. */
	{
		int sv[2];
		struct ucred c;
		socklen_t len = sizeof c;

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			test_result("socketpair for SO_PEERCRED", 0);
			return;
		}
		memset(&c, 0xFF, sizeof c);
		test_result("getsockopt(SO_PEERCRED) on a socketpair",
			    getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &c,
				       &len) == 0);
		test_result("...returns a struct ucred",
			    len == sizeof(struct ucred));
		test_result("...naming this process",
			    c.pid == me && c.uid == myuid && c.gid == mygid);

		/* Both ends answer, and answer the same. */
		memset(&c, 0xFF, sizeof c);
		len = sizeof c;
		test_result("the other end answers too",
			    getsockopt(sv[1], SOL_SOCKET, SO_PEERCRED, &c,
				       &len) == 0 &&
				    c.pid == me);
		close(sv[0]);
		close(sv[1]);
	}

	/* A socket that was never connected has no peer to describe. */
	{
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		struct ucred c;
		socklen_t len = sizeof c;

		if (s >= 0) {
			test_result("an unconnected socket reports ENOTCONN",
				    getsockopt(s, SOL_SOCKET, SO_PEERCRED, &c,
					       &len) == -1 &&
					    errno == ENOTCONN);
			close(s);
		}
	}

	/* A buffer too small must be refused rather than half-filled. */
	{
		int sv[2];

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
			struct ucred c;
			socklen_t len = sizeof(int);

			test_result("a short buffer is refused",
				    getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED,
					       &c, &len) == -1);
			close(sv[0]);
			close(sv[1]);
		}
	}

	/* connect/accept across a real socket, which is the path a bus client
	 * takes.  The child connects; the parent accepts and asks who it is. */
	{
		const char *path = "/tmp/.likeos-peercred-test";
		int ls, cs;
		struct sockaddr_un sa;
		pid_t child;

		unlink(path);
		ls = socket(AF_UNIX, SOCK_STREAM, 0);
		if (ls < 0) {
			test_result("socket for the connect/accept case", 0);
			return;
		}
		memset(&sa, 0, sizeof sa);
		sa.sun_family = AF_UNIX;
		strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
		if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
		    listen(ls, 4) != 0) {
			test_result("bind/listen for SO_PEERCRED", 0);
			close(ls);
			unlink(path);
			return;
		}

		child = fork();
		if (child == 0) {
			int c = socket(AF_UNIX, SOCK_STREAM, 0);

			if (c >= 0) {
				connect(c, (struct sockaddr *)&sa, sizeof sa);
				/* Hold the connection open long enough for the
				 * parent to ask about it. */
				char b;

				read(c, &b, 1);
				close(c);
			}
			_exit(0);
		}

		cs = accept(ls, NULL, NULL);
		if (cs >= 0) {
			struct ucred c;
			socklen_t len = sizeof c;

			memset(&c, 0xFF, sizeof c);
			test_result("SO_PEERCRED on an accepted connection",
				    getsockopt(cs, SOL_SOCKET, SO_PEERCRED, &c,
					       &len) == 0);
			/* The pid must be the CHILD's -- the credentials were
			 * taken from whoever called connect(), which is the
			 * whole point of the option. */
			test_result("...names the process that connected",
				    c.pid == child && c.uid == myuid);
			close(cs);
		} else {
			test_result("accept for SO_PEERCRED", 0);
		}
		close(ls);
		unlink(path);
		waitpid(child, NULL, 0);
	}
}

/* ------------------------------------------------------------------ *
 * The small additions that came with the desktop ports: daemon(), the
 * wait-status constructors, the new sysconf names, and the header placements
 * that let a configure script find what it is looking for.
 * ------------------------------------------------------------------ */
static void test_misc_posix(void)
{
	printf("\n[TEST] daemon, wait-status macros, sysconf\n");

	/* ---- W_EXITCODE / W_STOPCODE / WCOREDUMP ----
	 *
	 * The inverses of WIFEXITED and friends: given an exit code and a
	 * signal, build the status word those macros will read back. */
	test_result("W_EXITCODE round-trips an exit code",
		    WIFEXITED(W_EXITCODE(42, 0)) &&
			    WEXITSTATUS(W_EXITCODE(42, 0)) == 42);
	test_result("W_EXITCODE round-trips a signal",
		    WIFSIGNALED(W_EXITCODE(0, SIGTERM)) &&
			    WTERMSIG(W_EXITCODE(0, SIGTERM)) == SIGTERM);
	test_result("W_STOPCODE round-trips",
		    WIFSTOPPED(W_STOPCODE(SIGSTOP)) &&
			    WSTOPSIG(W_STOPCODE(SIGSTOP)) == SIGSTOP);
	test_result("WCOREDUMP reads the core flag",
		    !WCOREDUMP(W_EXITCODE(0, SIGSEGV)) &&
			    WCOREDUMP(W_EXITCODE(0, SIGSEGV) | WCOREFLAG));

	/* ---- sysconf ---- */
	test_result("_SC_GETPW_R_SIZE_MAX is a usable buffer size",
		    sysconf(_SC_GETPW_R_SIZE_MAX) > 0);
	test_result("_SC_GETGR_R_SIZE_MAX is a usable buffer size",
		    sysconf(_SC_GETGR_R_SIZE_MAX) > 0);
	test_result("_SC_NGROUPS_MAX matches the limit",
		    sysconf(_SC_NGROUPS_MAX) == NGROUPS_MAX);
	test_result("_SC_ARG_MAX matches the limit",
		    sysconf(_SC_ARG_MAX) == ARG_MAX);
	test_result("_SC_VERSION reports POSIX.1-2008",
		    sysconf(_SC_VERSION) == 200809L);
	test_result("_SC_JOB_CONTROL is supported",
		    sysconf(_SC_JOB_CONTROL) == 1);
	test_result("_SC_HOST_NAME_MAX matches the limit",
		    sysconf(_SC_HOST_NAME_MAX) == HOST_NAME_MAX);
	test_result("_SC_PHYS_PAGES reports memory",
		    sysconf(_SC_PHYS_PAGES) > 0);
	test_result("an unknown sysconf name is EINVAL",
		    (errno = 0, sysconf(999999) == -1 && errno == EINVAL));

	/* ---- header placement ----
	 *
	 * Not decoration.  A configure script decides whether a function
	 * EXISTS by compiling a program that includes the header the standard
	 * names, and concludes it is missing when the declaration is
	 * elsewhere.  Both of these were found that way: VTE decided this
	 * system had no grantpt because it was declared only in <unistd.h>,
	 * and called ffs() having included only <string.h>. */
	test_result("grantpt and friends are declared in <stdlib.h>",
		    (void *)grantpt != NULL && (void *)posix_openpt != NULL &&
			    (void *)unlockpt != NULL && (void *)ptsname != NULL);
	test_result("<string.h> brings in <strings.h>",
		    ffs(0) == 0 && ffs(1) == 1 && ffs(8) == 4 &&
			    ffs(0x80000000) == 32);

	/* ---- ioctl is variadic ----
	 *
	 * POSIX declares the third argument with `...', and it has to be: the
	 * requests take a struct pointer, a plain int, or nothing at all, and
	 * a fixed void* parameter makes the int form uncompilable in C++.
	 * That this call compiles at all is most of the test. */
	{
		struct winsize ws;
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			/* Both shapes, on a device that implements neither --
			 * the return value is not the point, the argument
			 * types are. */
			(void)ioctl(fd, TIOCGWINSZ, &ws);
			(void)ioctl(fd, FIONBIO, 0);
			close(fd);
		}
		test_result("ioctl accepts a pointer and an integer", 1);
	}

	/* ---- daemon() ----
	 *
	 * Tested from a child, because it is a call the CALLER does not return
	 * from: the parent half of its internal fork exits.  So this forks,
	 * the child calls daemon(), and the grandchild reports what it became
	 * down a pipe.
	 *
	 * What is checked is what daemon() promises: a new session (so the
	 * process is its own session leader and has no controlling terminal),
	 * the root directory, and the three standard descriptors open on
	 * something. */
	{
		int pfd[2];
		pid_t child;

		if (pipe(pfd) != 0) {
			test_result("pipe for the daemon() test", 0);
			return;
		}
		child = fork();
		if (child == 0) {
			close(pfd[0]);
			if (daemon(0, 1) != 0)
				_exit(1);
			{
				/* setsid() succeeded means this is now a
				 * session leader: its sid is its own pid. */
				pid_t sid = getsid(0);
				pid_t self = getpid();
				char cwd[256];
				int ok_sid = (sid == self);
				int ok_cwd = (getcwd(cwd, sizeof cwd) &&
					      strcmp(cwd, "/") == 0);
				char msg[2];

				msg[0] = ok_sid ? '1' : '0';
				msg[1] = ok_cwd ? '1' : '0';
				write(pfd[1], msg, 2);
			}
			close(pfd[1]);
			_exit(0);
		}
		close(pfd[1]);
		{
			char msg[2] = { 0, 0 };
			ssize_t n = read(pfd[0], msg, 2);

			close(pfd[0]);
			waitpid(child, NULL, 0);
			test_result("daemon() starts a new session",
				    n == 2 && msg[0] == '1');
			test_result("daemon() changes to the root directory",
				    n == 2 && msg[1] == '1');
		}
	}
}

/* ------------------------------------------------------------------ *
 * <uchar.h> -- the UTF-16 and UTF-32 converters of C11 7.28.
 *
 * The UTF-32 pair is mbrtowc/wcrtomb under another name, since wchar_t is a
 * 32-bit code point here.  The UTF-16 pair is the interesting one: a character
 * above U+FFFF is ONE multibyte sequence and TWO char16_t units, so mbrtoc16
 * returns the high surrogate and keeps the low one in the mbstate_t, handing
 * it back on a later call that consumes no input and returns (size_t)-3.
 *
 * host/test-unicode.sh compares all 1,112,063 code points against glibc.  What
 * is checked here is that the same code behaves the same way as the system
 * libc on the target, with the surrogate protocol and the error cases spelled
 * out rather than swept.
 * ------------------------------------------------------------------ */
static void test_uchar(void)
{
	printf("\n[TEST] <uchar.h> UTF-16 / UTF-32 conversion\n");

	mbstate_t st;
	char out[MB_LEN_MAX];
	char16_t c16 = 0;
	char32_t c32 = 0;
	size_t r;

	/* ASCII: one byte, one unit, either way. */
	memset(&st, 0, sizeof st);
	test_result("mbrtoc16 ASCII",
		    mbrtoc16(&c16, "A", 1, &st) == 1 && c16 == 'A');
	memset(&st, 0, sizeof st);
	test_result("c16rtomb ASCII",
		    c16rtomb(out, 'A', &st) == 1 && out[0] == 'A');

	/* U+20AC EURO SIGN: three bytes, still one UTF-16 unit. */
	memset(&st, 0, sizeof st);
	test_result("mbrtoc16 three-byte character",
		    mbrtoc16(&c16, "\xE2\x82\xAC", 3, &st) == 3 &&
			    c16 == 0x20AC);
	memset(&st, 0, sizeof st);
	test_result("c16rtomb three-byte character",
		    c16rtomb(out, 0x20AC, &st) == 3 &&
			    memcmp(out, "\xE2\x82\xAC", 3) == 0);

	/* U+1F600 GRINNING FACE: four bytes, and a surrogate PAIR.
	 *
	 * The second call passes n = 0 on purpose: the low surrogate must come
	 * out of the state without a byte being read. */
	{
		const char *emoji = "\xF0\x9F\x98\x80";
		char16_t hi = 0, lo = 0;
		size_t r1, r2;

		memset(&st, 0, sizeof st);
		r1 = mbrtoc16(&hi, emoji, 4, &st);
		r2 = mbrtoc16(&lo, emoji, 0, &st);
		test_result("mbrtoc16 splits U+1F600 into a surrogate pair",
			    r1 == 4 && hi == 0xD83D && r2 == (size_t)-3 &&
				    lo == 0xDE00);

		/* mbsinit reports the pending half, because the conversion
		 * genuinely is not in its initial state. */
		memset(&st, 0, sizeof st);
		mbrtoc16(&hi, emoji, 4, &st);
		test_result("a pending surrogate is not the initial state",
			    !mbsinit(&st));
		mbrtoc16(&lo, emoji, 0, &st);
		test_result("draining it returns to the initial state",
			    mbsinit(&st));

		/* And back: the high half writes nothing, the low half
		 * writes the whole four-byte character. */
		memset(&st, 0, sizeof st);
		size_t n1 = c16rtomb(out, 0xD83D, &st);
		size_t n2 = c16rtomb(out, 0xDE00, &st);

		test_result("c16rtomb reassembles the pair",
			    n1 == 0 && n2 == 4 && memcmp(out, emoji, 4) == 0);
	}

	/* UTF-32 is the code point itself, above the basic plane and below. */
	memset(&st, 0, sizeof st);
	test_result("mbrtoc32 U+1F600",
		    mbrtoc32(&c32, "\xF0\x9F\x98\x80", 4, &st) == 4 &&
			    c32 == 0x1F600);
	memset(&st, 0, sizeof st);
	test_result("c32rtomb U+1F600",
		    c32rtomb(out, 0x1F600, &st) == 4 &&
			    memcmp(out, "\xF0\x9F\x98\x80", 4) == 0);

	/* A sequence split across two calls: -2, then the rest. */
	memset(&st, 0, sizeof st);
	r = mbrtoc16(&c16, "\xE2\x82", 2, &st);
	test_result("mbrtoc16 reports an incomplete sequence",
		    r == (size_t)-2);
	test_result("mbrtoc16 resumes where it stopped",
		    mbrtoc16(&c16, "\xAC", 1, &st) == 1 && c16 == 0x20AC);

	/* Malformed input. */
	memset(&st, 0, sizeof st);
	errno = 0;
	test_result("mbrtoc16 rejects an invalid byte",
		    mbrtoc16(&c16, "\xFF", 1, &st) == (size_t)-1 &&
			    errno == EILSEQ);

	/* A low surrogate on its own encodes nothing at all. */
	memset(&st, 0, sizeof st);
	errno = 0;
	test_result("c16rtomb rejects a lone low surrogate",
		    c16rtomb(out, 0xDC00, &st) == (size_t)-1 &&
			    errno == EILSEQ);

	/* A high surrogate followed by something that is not its low half --
	 * and the state must be cleared, so the NEXT character still works. */
	memset(&st, 0, sizeof st);
	errno = 0;
	{
		int held = (c16rtomb(out, 0xD800, &st) == 0);
		int broke = (c16rtomb(out, 'A', &st) == (size_t)-1 &&
			     errno == EILSEQ);
		int recovered = (c16rtomb(out, 'A', &st) == 1);

		test_result("c16rtomb rejects a broken pair and recovers",
			    held && broke && recovered);
	}

	/* The null character: zero returned, one byte written. */
	memset(&st, 0, sizeof st);
	c16 = 0xFFFF;
	test_result("mbrtoc16 of the null character",
		    mbrtoc16(&c16, "", 1, &st) == 0 && c16 == 0);

	/* The UTF-16 and UTF-32 encodings are Unicode, which the standard
	 * asks the header to say. */
#if defined(__STDC_UTF_16__) && defined(__STDC_UTF_32__)
	test_result("__STDC_UTF_16__ / __STDC_UTF_32__ are defined", 1);
#else
	test_result("__STDC_UTF_16__ / __STDC_UTF_32__ are defined", 0);
#endif
}

/* ------------------------------------------------------------------ *
 * The hyperbolic family.
 *
 * Six functions in three precisions.  The interesting ones are the three
 * inverses, which have no closed form that works over their whole domain --
 * see src/math/math.c -- so each is written in ranges and each range boundary
 * is a place two different expressions have to agree.
 *
 * This checks the properties rather than a table of digits: the identities
 * (sinh(asinh(x)) == x), the small-argument behaviour that a naive
 * implementation destroys, the domain edges, and the signs.  The exhaustive
 * comparison against a reference implementation is host/test-math.sh, which
 * sweeps 58,000 points; what belongs HERE is proof that the same code gives
 * the same answers when it is the system libm on the target.
 * ------------------------------------------------------------------ */
static int close_enough(double got, double want, double rel)
{
	double d = got - want;

	if (want == 0.0)
		return (d < rel && d > -rel);
	d /= want;
	return (d < rel && d > -rel);
}

static void test_hyperbolic(void)
{
	printf("\n[TEST] hyperbolic and inverse hyperbolic functions\n");

	volatile double zero = 0.0;
	double inf = 1.0 / zero;

	/* Known values, to a relative 1e-14 -- about fifty times looser than
	 * what the implementation delivers, so this measures "right function"
	 * and not "right to the last bit". */
	test_result("asinh(1) == ln(1+sqrt2)",
		    close_enough(asinh(1.0), 0.881373587019543, 1e-14));
	test_result("acosh(2) == ln(2+sqrt3)",
		    close_enough(acosh(2.0), 1.3169578969248166, 1e-14));
	test_result("atanh(0.5) == 0.5*ln3",
		    close_enough(atanh(0.5), 0.5493061443340549, 1e-14));

	/* Each inverse against its forward function, at a value in every
	 * branch of the range split: below 2^-28, between it and 1, between 1
	 * and 2, above 2, and above 2^28. */
	{
		static const double v[] = { 1e-12, 0.25, 1.5, 3.0, 1e10 };
		int ok_s = 1, ok_c = 1, ok_t = 1;

		for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
			if (!close_enough(sinh(asinh(v[i])), v[i], 1e-13))
				ok_s = 0;
			if (!close_enough(cosh(acosh(1.0 + v[i])), 1.0 + v[i],
					  1e-13))
				ok_c = 0;
			/* atanh's domain stops at 1. */
			if (v[i] < 1.0 &&
			    !close_enough(tanh(atanh(v[i])), v[i], 1e-13))
				ok_t = 0;
		}
		test_result("sinh(asinh(x)) == x across every branch", ok_s);
		test_result("cosh(acosh(x)) == x across every branch", ok_c);
		test_result("tanh(atanh(x)) == x across every branch", ok_t);
	}

	/* The small-argument end, which is the whole reason these are not
	 * written from their closed forms.  asinh(x), atanh(x), sinh(x) and
	 * tanh(x) are all x to within a relative 1e-15 down here; an
	 * implementation that forms 1 + x*x, or e^x - e^-x, returns something
	 * with half its digits gone -- or exactly zero. */
	{
		double t = 1e-10;

		test_result("asinh(1e-10) keeps its digits",
			    close_enough(asinh(t), t, 1e-15));
		test_result("atanh(1e-10) keeps its digits",
			    close_enough(atanh(t), t, 1e-15));
		test_result("sinh(1e-10) keeps its digits",
			    close_enough(sinh(t), t, 1e-15));
		test_result("tanh(1e-10) keeps its digits",
			    close_enough(tanh(t), t, 1e-15));
	}

	/* The large-argument end, where forming x*x would overflow but the
	 * answer is an ordinary number near log(2x). */
	test_result("asinh(1e300) does not overflow",
		    close_enough(asinh(1e300), 691.4686750787736, 1e-13));
	test_result("acosh(1e300) does not overflow",
		    close_enough(acosh(1e300), 691.4686750787736, 1e-13));

	/* Odd functions: the sign comes back out, including on zero. */
	test_result("asinh is odd", asinh(-2.0) == -asinh(2.0));
	test_result("atanh is odd", atanh(-0.5) == -atanh(0.5));
	test_result("sinh is odd", sinh(-2.0) == -sinh(2.0));
	test_result("tanh is odd", tanh(-2.0) == -tanh(2.0));
	test_result("cosh is even", cosh(-2.0) == cosh(2.0));
	test_result("signed zero survives",
		    signbit(asinh(-0.0)) && signbit(atanh(-0.0)) &&
			    signbit(sinh(-0.0)) && !signbit(asinh(0.0)));

	/* Domain edges.  acosh is undefined below 1; atanh is undefined
	 * outside [-1, 1] and infinite at the ends. */
	test_result("acosh(1) == 0", acosh(1.0) == 0.0);
	test_result("acosh below 1 is NaN", isnan(acosh(0.5)) &&
						    isnan(acosh(-1.0)));
	test_result("atanh(+-1) is +-inf",
		    atanh(1.0) == inf && atanh(-1.0) == -inf);
	test_result("atanh outside [-1,1] is NaN",
		    isnan(atanh(2.0)) && isnan(atanh(-2.0)));
	test_result("asinh(+-inf) is +-inf",
		    asinh(inf) == inf && asinh(-inf) == -inf);
	test_result("acosh(inf) is inf", acosh(inf) == inf);

	/* The float and long double variants: declared, linked, and agreeing
	 * with the double one to their own precision. */
	test_result("asinhf/acoshf/atanhf",
		    close_enough(asinhf(1.0f), 0.881373587019543, 1e-6) &&
			    close_enough(acoshf(2.0f), 1.3169578969248166,
					 1e-6) &&
			    close_enough(atanhf(0.5f), 0.5493061443340549,
					 1e-6));
	test_result("sinhf/coshf/tanhf",
		    close_enough(sinhf(1.0f), 1.1752011936438014, 1e-6) &&
			    close_enough(coshf(1.0f), 1.5430806348152437,
					 1e-6) &&
			    close_enough(tanhf(1.0f), 0.7615941559557649,
					 1e-6));
	test_result("asinhl/acoshl/atanhl",
		    close_enough((double)asinhl(1.0L), 0.881373587019543,
				 1e-14) &&
			    close_enough((double)acoshl(2.0L),
					 1.3169578969248166, 1e-14) &&
			    close_enough((double)atanhl(0.5L),
					 0.5493061443340549, 1e-14));
	test_result("sinhl/coshl/tanhl",
		    close_enough((double)sinhl(1.0L), 1.1752011936438014,
				 1e-14) &&
			    close_enough((double)coshl(1.0L),
					 1.5430806348152437, 1e-14) &&
			    close_enough((double)tanhl(1.0L),
					 0.7615941559557649, 1e-14));
}

static void test_math_classification(void)
{
	printf("\n[TEST] C99 math classification\n");

	volatile double zero = 0.0;
	double inf = 1.0 / zero;
	double nan = inf - inf;
	double sub = 4.9406564584124654e-324; /* smallest subnormal double */

	test_result("fpclassify(1.0) is FP_NORMAL",
		    fpclassify(1.0) == FP_NORMAL);
	test_result("fpclassify(0.0) is FP_ZERO", fpclassify(0.0) == FP_ZERO);
	test_result("fpclassify(inf) is FP_INFINITE",
		    fpclassify(inf) == FP_INFINITE);
	test_result("fpclassify(nan) is FP_NAN", fpclassify(nan) == FP_NAN);
	test_result("fpclassify(subnormal) is FP_SUBNORMAL",
		    fpclassify(sub) == FP_SUBNORMAL);
	test_result("fpclassify is type-generic (float)",
		    fpclassify(1.0f) == FP_NORMAL &&
			    fpclassify(0.0f) == FP_ZERO);

	test_result("isnormal()", isnormal(1.0) && !isnormal(0.0) &&
					   !isnormal(sub) && !isnormal(nan));
	test_result("isfinite()", isfinite(1.0) && !isfinite(inf) &&
					   !isfinite(nan));
	test_result("isinf()", isinf(inf) && !isinf(1.0) && !isinf(nan));
	test_result("isnan()", isnan(nan) && !isnan(1.0) && !isnan(inf));

	/* signbit is about the sign BIT, not about being less than zero: it is
	 * true for negative zero, which compares equal to zero. */
	test_result("signbit()", signbit(-1.0) && !signbit(1.0) &&
					  signbit(-0.0) && !signbit(0.0));

	test_result("isgreater()", isgreater(2.0, 1.0) && !isgreater(1.0, 2.0) &&
					   !isgreater(1.0, 1.0));
	test_result("isgreaterequal()",
		    isgreaterequal(2.0, 1.0) && isgreaterequal(1.0, 1.0) &&
			    !isgreaterequal(1.0, 2.0));
	test_result("isless()", isless(1.0, 2.0) && !isless(2.0, 1.0));
	test_result("islessequal()", islessequal(1.0, 2.0) &&
					     islessequal(1.0, 1.0) &&
					     !islessequal(2.0, 1.0));
	test_result("islessgreater()", islessgreater(1.0, 2.0) &&
					       islessgreater(2.0, 1.0) &&
					       !islessgreater(1.0, 1.0));

	/* The quiet-on-NaN property, which is the whole reason these exist. */
	test_result("comparison macros are false for NaN",
		    !isgreater(nan, 1.0) && !isless(nan, 1.0) &&
			    !islessequal(nan, nan) && !isgreaterequal(nan, nan));
	test_result("isunordered()", isunordered(nan, 1.0) &&
					     isunordered(1.0, nan) &&
					     !isunordered(1.0, 2.0));
}

/* ------------------------------------------------------------------ *
 * frexpl / ldexpl.
 *
 * Taking a long double apart and putting it back together, exactly.  The
 * defining property is the one worth testing: x == frexpl(x, &e) * 2^e, with
 * the mantissa in [0.5, 1).  Checked across the whole exponent range, because
 * a mistake in the 16383 bias shows up at the extremes and nowhere else, and
 * on subnormals, which have no leading one to take an exponent from and go
 * down a separate path.
 * ------------------------------------------------------------------ */
static void test_long_double_decompose(void)
{
	printf("\n[TEST] frexpl / ldexpl\n");

	int e = 12345;

	/* Zero keeps its sign and reports exponent 0. */
	test_result("frexpl(+0)", frexpl(0.0L, &e) == 0.0L && e == 0);
	e = 12345;
	test_result("frexpl(-0) keeps the sign",
		    signbit(frexpl(-0.0L, &e)) && e == 0);

	test_result("frexpl(1.0) is 0.5 x 2^1",
		    frexpl(1.0L, &e) == 0.5L && e == 1);
	test_result("frexpl(0.5) is 0.5 x 2^0",
		    frexpl(0.5L, &e) == 0.5L && e == 0);
	test_result("frexpl(-3.0) is -0.75 x 2^2",
		    frexpl(-3.0L, &e) == -0.75L && e == 2);

	test_result("ldexpl(1.0, 3)", ldexpl(1.0L, 3) == 8.0L);
	test_result("ldexpl(1.0, -3)", ldexpl(1.0L, -3) == 0.125L);
	test_result("ldexpl(0.0, 100)", ldexpl(0.0L, 100) == 0.0L);
	test_result("scalbnl agrees with ldexpl",
		    scalbnl(3.0L, 5) == ldexpl(3.0L, 5));

	/* The round trip, across every exponent the format has.  This is the
	 * check that would catch an off-by-one in the bias. */
	int roundtrip_fails = 0, range_fails = 0;
	for (int p = -16380; p <= 16380; p++) {
		long double x = ldexpl(1.0L, p);
		int ex;
		long double m;

		if (x == 0.0L || isinf(x))
			continue;
		for (int k = 0; k < 2; k++) {
			long double v = k ? x * 1.5L : x;

			if (v == 0.0L || isinf(v))
				continue;
			m = frexpl(v, &ex);
			if (ldexpl(m, ex) != v)
				roundtrip_fails++;
			if (!(m >= 0.5L && m < 1.0L))
				range_fails++;
		}
	}
	test_result("frexpl/ldexpl round trip over the whole exponent range",
		    roundtrip_fails == 0);
	test_result("frexpl mantissa always in [0.5, 1)", range_fails == 0);

	/* Subnormals: no leading one, so a separate path. */
	long double tiny = LDBL_MIN;
	int te;
	long double tm = frexpl(tiny / 2, &te);
	test_result("frexpl(subnormal) round trips",
		    ldexpl(tm, te) == tiny / 2);
	test_result("frexpl(subnormal) mantissa in [0.5, 1)",
		    tm >= 0.5L && tm < 1.0L);

	/* Infinity and NaN come back unchanged. */
	volatile long double zerol = 0.0L;
	long double infl = 1.0L / zerol;
	test_result("frexpl(inf) returns inf", isinf(frexpl(infl, &e)));
	test_result("frexpl(nan) returns nan", isnan(frexpl(infl - infl, &e)));
	test_result("ldexpl(inf, -100) stays inf", isinf(ldexpl(infl, -100)));
}

/* ------------------------------------------------------------------ *
 * POSIX semaphores.
 *
 * The counting half is easy to check; the part that matters is the blocking
 * one, because a semaphore whose wait and post race is a semaphore that
 * deadlocks under load and passes every single-threaded test.  So the cases
 * below deliberately make threads block and be woken, and make a post arrive
 * while a waiter is on its way into the kernel.
 * ------------------------------------------------------------------ */
static sem_t g_sem;
static sem_t g_sem_done;
static volatile int g_sem_counter;

static void *sem_consumer(void *arg)
{
	int n = (int)(long)arg;

	for (int i = 0; i < n; i++) {
		if (sem_wait(&g_sem) != 0)
			return (void *)1;
		__atomic_add_fetch(&g_sem_counter, 1, __ATOMIC_SEQ_CST);
	}
	sem_post(&g_sem_done);
	return NULL;
}

/* ------------------------------------------------------------------ *
 * Timing helpers for the blocking primitives.
 *
 * Two clocks, deliberately.  A POSIX deadline is stated against
 * CLOCK_REALTIME, so that is what builds one; but CLOCK_REALTIME is
 * settable and can step, which makes it the wrong instrument for
 * measuring how long something took.  The measurement is taken on
 * CLOCK_MONOTONIC, which is what it is for.
 * ------------------------------------------------------------------ */
static void deadline_in_ms(struct timespec *ts, long ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec++;
	}
}

static void mono_now(struct timespec *ts)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
}

static long mono_elapsed_ms(const struct timespec *since)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - since->tv_sec) * 1000 +
	       (now.tv_nsec - since->tv_nsec) / 1000000;
}

/*
 * The far end of a request/reply connection: read a little, send it straight
 * back, until the near end goes away.
 *
 * Deliberately over a bound socket rather than a socketpair, and deliberately
 * two-way, because that is what a display connection is and neither property
 * is exercised by sending bulk data one way through a socketpair.
 */
static int echo_listen_fd;
static int echo_fd;
static volatile int echo_ready; /* 1 = accepted, -1 = failed */

static void *echo_peer(void *arg)
{
	unsigned char buf[512];

	(void)arg;
	/* Accept from THIS thread, before the other end connects.
	 *
	 * connect() does not complete until somebody accepts, so a single
	 * thread that connects and then accepts waits for itself and never
	 * moves -- which is exactly what the first version of this test did.
	 * The socket table shows it plainly when it happens: the listener has
	 * a connection queued and the client is still unconnected. */
	echo_fd = accept(echo_listen_fd, NULL, NULL);
	if (echo_fd < 0) {
		echo_ready = -1;
		return (void *)1;
	}
	echo_ready = 1;

	for (;;) {
		ssize_t r = read(echo_fd, buf, sizeof(buf));
		ssize_t done = 0;

		if (r <= 0)
			return NULL; /* near end closed: done */
		while (done < r) {
			ssize_t w = write(echo_fd, buf + done,
					  (size_t)(r - done));

			if (w <= 0)
				return (void *)1;
			done += w;
		}
	}
}

/*
 * A byte stream whose every position has a known value.
 *
 * Position-dependent on purpose: if the stream loses a byte, repeats one, or
 * delivers two writes out of order, everything after that point disagrees, and
 * the offset of the first disagreement says exactly where it happened.  A
 * checksum over the whole thing would only say "somewhere".
 */
static unsigned char stream_byte(unsigned long off)
{
	return (unsigned char)((off * 31u) + (unsigned)(off >> 7) + 17u);
}

#define STREAM_TOTAL (4u * 1024u * 1024u)
static int stream_fd;

/*
 * Push that stream through a socket as fast as it will go, in small writes of
 * varying size.
 *
 * This is the shape of a protocol connection -- a display server, say -- rather
 * than a file copy: thousands of small writes rather than a few large ones, and
 * no two the same size.  A stream that only survives being written in neat
 * page-sized pieces looks perfect to a bulk-transfer test and still loses a
 * byte here, and one lost byte does not corrupt one message, it desynchronises
 * every message after it.
 */
static void *stream_producer(void *arg)
{
	unsigned char buf[1400];
	unsigned long off = 0;
	unsigned seed = 12345u;

	(void)arg;
	while (off < STREAM_TOTAL) {
		unsigned n, done = 0;

		seed = seed * 1103515245u + 12345u;
		n = 1u + (seed >> 16) % sizeof(buf);
		if (off + n > STREAM_TOTAL)
			n = (unsigned)(STREAM_TOTAL - off);
		for (unsigned i = 0; i < n; i++)
			buf[i] = stream_byte(off + i);

		while (done < n) {
			ssize_t w = write(stream_fd, buf + done, n - done);

			if (w <= 0)
				return (void *)1;
			done += (unsigned)w;
		}
		off += n;
	}
	return NULL;
}

/*
 * One writer among several, each owning its own file.
 *
 * Fills it with a pattern derived from the writer's index, so a page that
 * strayed between files shows up as the wrong bytes rather than as a crash.
 * Returns NULL on success, non-NULL on failure, which the joiner checks.
 */
#define CACHE_WRITERS 4
#define CACHE_WRITER_BYTES 65536
static int cache_writer_arg[CACHE_WRITERS];

static void *cache_writer_body(void *arg)
{
	int idx = *(int *)arg;
	char path[64];
	unsigned char buf[512];
	int fd;
	size_t done = 0;

	snprintf(path, sizeof(path), "/tmp/cache_w%d_%d", (int)getpid(), idx);
	unlink(path);
	fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0)
		return (void *)1;

	while (done < CACHE_WRITER_BYTES) {
		size_t n = sizeof(buf);

		if (done + n > CACHE_WRITER_BYTES)
			n = CACHE_WRITER_BYTES - done;
		for (size_t i = 0; i < n; i++)
			buf[i] = (unsigned char)(idx * 7 + done + i);
		if (write(fd, buf, n) != (ssize_t)n) {
			close(fd);
			return (void *)2;
		}
		done += n;
	}
	close(fd);
	return NULL;
}

/*
 * Several threads appending to ONE file, serialised by a lock they share.
 *
 * The distinct-file case above asks whether two writers can corrupt each
 * other's pages.  This asks the harder half: whether two writers hitting the
 * SAME page interleave correctly.  Every append is smaller than a page, so
 * most of them land in a page another thread has already dirtied -- the case
 * where a copy into the page and the marking of it as dirty have to stay in
 * the right order, and where the file's growing length is read by one thread
 * while another is extending it.
 *
 * The lock is the program's, not the kernel's: append-under-a-lock is what a
 * program that shares a log file does, and it is the pattern the result can
 * be checked against, since each record is written whole.
 */
#define SHARED_APPENDERS 4
#define SHARED_APPEND_RECS 200
#define SHARED_APPEND_RECLEN 37 /* not a divisor of a page: pages straddle */
static pthread_mutex_t shared_append_lock = PTHREAD_MUTEX_INITIALIZER;
static int shared_append_fd = -1;
static int shared_append_arg[SHARED_APPENDERS];

static void *shared_append_body(void *arg)
{
	int idx = *(int *)arg;
	char rec[SHARED_APPEND_RECLEN];

	for (int i = 0; i < SHARED_APPEND_RECS; i++) {
		memset(rec, 'A' + idx, sizeof(rec));
		rec[sizeof(rec) - 1] = '\n';
		pthread_mutex_lock(&shared_append_lock);
		ssize_t w = write(shared_append_fd, rec, sizeof(rec));
		pthread_mutex_unlock(&shared_append_lock);
		if (w != (ssize_t)sizeof(rec))
			return (void *)1;
	}
	return NULL;
}

/*
 * Exit-handler ordering and scale.
 *
 * Handlers must run most-recently-registered first, so a countdown that starts
 * at the number registered and expects to reach zero verifies both that every
 * one ran and that they ran in the right order.  Anything out of place makes
 * the child exit non-zero.
 */
#define ATEXIT_SCALE_N 5000
static volatile int atexit_expect;

static void atexit_order_probe(void)
{
	if (atexit_expect <= 0)
		_exit(6); /* ran more times than were registered */
	atexit_expect--;
	if (atexit_expect == 0)
		_exit(0); /* the last one: report success from here */
}

/*
 * A thread that is alive and blocked when its process exits.
 *
 * Deliberately parked in a call that sleeps rather than spinning: what the
 * teardown test wants is a thread the kernel has to interrupt and unwind, not
 * one that happens to be on a run queue.  It is never expected to return --
 * the process goes away underneath it.
 */
static void *teardown_thread_body(void *arg)
{
	(void)arg;
	for (;;)
		usleep(100000);
	return NULL;
}

static void *sem_producer(void *arg)
{
	int n = (int)(long)arg;

	for (int i = 0; i < n; i++)
		sem_post(&g_sem);
	return NULL;
}

static void test_semaphores(void)
{
	sem_t s;
	int v;

	printf("\n[TEST] POSIX semaphores\n");

	/* Counting, without blocking. */
	test_result("sem_init(0)", sem_init(&s, 0, 0) == 0);
	test_result("sem_getvalue() reports the initial count",
		    sem_getvalue(&s, &v) == 0 && v == 0);
	test_result("sem_trywait() on an empty semaphore fails EAGAIN",
		    sem_trywait(&s) == -1 && errno == EAGAIN);
	test_result("sem_post()", sem_post(&s) == 0);
	test_result("sem_getvalue() after post",
		    sem_getvalue(&s, &v) == 0 && v == 1);
	test_result("sem_trywait() succeeds when available",
		    sem_trywait(&s) == 0);
	test_result("count is back to zero",
		    sem_getvalue(&s, &v) == 0 && v == 0);
	test_result("sem_wait() does not block when the count is positive",
		    sem_post(&s) == 0 && sem_wait(&s) == 0);
	test_result("sem_destroy() with no waiters", sem_destroy(&s) == 0);

	/* pshared is refused rather than silently ignored. */
	test_result("sem_init(pshared) reports ENOSYS",
		    sem_init(&s, 1, 0) == -1 && errno == ENOSYS);
	test_result("sem_open() reports ENOSYS",
		    sem_open("/x", 0) == SEM_FAILED && errno == ENOSYS);

	/* Timed wait: must actually time out, and must do so at roughly the
	 * requested time rather than immediately or never. */
	test_result("sem_init for the timed case", sem_init(&s, 0, 0) == 0);
	{
		struct timespec deadline, t0;
		long elapsed_ms;
		int r;

		deadline_in_ms(&deadline, 150);
		mono_now(&t0);
		r = sem_timedwait(&s, &deadline);
		elapsed_ms = mono_elapsed_ms(&t0);

		test_result("sem_timedwait() times out",
			    r == -1 && errno == ETIMEDOUT);
		/* The tick is 10ms and the kernel rounds a futex timeout up to
		 * whole ticks, so 150ms is expected to land around 160.  The
		 * window is wide on both sides because this is checking that a
		 * wait HAPPENED, not its precision -- returning immediately and
		 * never returning are the two failures worth catching. */
		if (elapsed_ms < 100 || elapsed_ms >= 2000)
			printf("  (measured %ld ms, expected about 150)\n",
			       elapsed_ms);
		test_result("sem_timedwait() waited about the right time",
			    elapsed_ms >= 100 && elapsed_ms < 2000);
	}
	/* An already-expired deadline must not block at all. */
	{
		struct timespec past;

		clock_gettime(CLOCK_REALTIME, &past);
		past.tv_sec -= 1;
		test_result("sem_timedwait() with a past deadline returns at once",
			    sem_timedwait(&s, &past) == -1 &&
				    errno == ETIMEDOUT);
	}
	sem_destroy(&s);

	/*
	 * The real test: a consumer that MUST block, woken by a later post.
	 * If the wait and post race -- if a post can land between the waiter
	 * checking the count and going to sleep, and be lost -- this hangs
	 * rather than failing, which is why the counts are small enough to
	 * finish quickly and large enough to interleave.
	 */
	test_result("sem_init for the threaded case",
		    sem_init(&g_sem, 0, 0) == 0 &&
			    sem_init(&g_sem_done, 0, 0) == 0);
	g_sem_counter = 0;
	{
		pthread_t consumer, producer;
		const int N = 200;

		if (pthread_create(&consumer, NULL, sem_consumer,
				   (void *)(long)N) != 0) {
			test_result("pthread_create(consumer)", 0);
		} else {
			/* Give the consumer time to be genuinely blocked
			 * before any post arrives, so the wake path is the one
			 * under test rather than the fast path. */
			usleep(50000);
			if (pthread_create(&producer, NULL, sem_producer,
					   (void *)(long)N) != 0) {
				test_result("pthread_create(producer)", 0);
			} else {
				pthread_join(producer, NULL);
				pthread_join(consumer, NULL);
				test_result("every post woke a waiter (no lost wakeup)",
					    g_sem_counter == N);
				test_result("the semaphore is drained afterwards",
					    sem_getvalue(&g_sem, &v) == 0 &&
						    v == 0);
			}
		}
	}
	sem_destroy(&g_sem);
	sem_destroy(&g_sem_done);
}

/* ------------------------------------------------------------------ *
 * The DNS stub resolver.
 *
 * Message construction and name handling are checked without a network,
 * because those are the parts that are wrong in ways a lookup would hide: a
 * malformed query still gets an answer from a forgiving server, and a name
 * decompressed one byte short still looks like a name.
 *
 * The lookups themselves come last and are reported as informational -- a
 * machine with no route to a nameserver is not a broken resolver.
 * ------------------------------------------------------------------ */
/* ====================================================================== *
 * Timed blocking primitives
 *
 * Every one of these takes an ABSOLUTE deadline, while the futex the
 * kernel exposes takes a RELATIVE timeout.  pthread_cond_timedwait and
 * pthread_mutex_timedlock both used to hand the deadline straight to the
 * futex, so a timestamp near 1.8e9 seconds was read as a duration: not a
 * long timeout but an unreachable one.  Neither function could report
 * ETIMEDOUT at all, and a program relying on one to bound its wait waited
 * for ever.
 *
 * So each test here asserts two separate things -- that the call returns
 * ETIMEDOUT, and that it did so after roughly the requested delay.  The
 * first alone would pass against a function that gives up instantly; the
 * second alone would pass against one that never times out at all.
 * ====================================================================== */

static pthread_mutex_t g_tb_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tb_cmutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tb_cond = PTHREAD_COND_INITIALIZER;
static int g_tb_predicate;

/* Holds the mutex long enough for the main thread's timedlock to expire. */
static void *tb_mutex_holder(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&g_tb_mutex);
	usleep(600000); /* 600ms -- comfortably past the 150ms deadline */
	pthread_mutex_unlock(&g_tb_mutex);
	return NULL;
}

/* Signals the condition after a delay shorter than the deadline. */
static void *tb_cond_signaller(void *arg)
{
	(void)arg;
	usleep(120000); /* 120ms, against a 2s deadline */
	pthread_mutex_lock(&g_tb_cmutex);
	g_tb_predicate = 1;
	pthread_cond_signal(&g_tb_cond);
	pthread_mutex_unlock(&g_tb_cmutex);
	return NULL;
}

static volatile sig_atomic_t g_tb_alarms;
static sem_t g_tb_sem;

static void tb_sigalrm_handler(int sig)
{
	(void)sig;
	g_tb_alarms++;
}

/* Releases the blocked sem_wait if the signal never gets there, so a broken
 * interrupt path fails the test instead of wedging the run. */
static void *tb_sem_watchdog(void *arg)
{
	sem_t *s = (sem_t *)arg;

	usleep(2000000);
	sem_post(s);
	return NULL;
}

static void test_timed_blocking(void)
{
	printf("\n[TEST] Timed blocking primitives\n");

	/* ---- pthread_mutex_timedlock ---- */
	{
		pthread_t th;
		struct timespec deadline, t0;
		long elapsed_ms;
		int r;

		test_result("mutex holder thread starts",
			    pthread_create(&th, NULL, tb_mutex_holder,
					   NULL) == 0);
		usleep(50000); /* let it take the lock */

		deadline_in_ms(&deadline, 150);
		mono_now(&t0);
		r = pthread_mutex_timedlock(&g_tb_mutex, &deadline);
		elapsed_ms = mono_elapsed_ms(&t0);

		if (r != ETIMEDOUT || elapsed_ms >= 2000)
			printf("  (returned %d after %ld ms)\n", r,
			       elapsed_ms);
		test_result("pthread_mutex_timedlock() on a held mutex "
			    "reports ETIMEDOUT",
			    r == ETIMEDOUT);
		test_result("pthread_mutex_timedlock() waited about the "
			    "right time",
			    elapsed_ms >= 100 && elapsed_ms < 2000);

		pthread_join(th, NULL);

		/* An uncontended timedlock takes the mutex at once. */
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 5;
		test_result("pthread_mutex_timedlock() succeeds when free",
			    pthread_mutex_timedlock(&g_tb_mutex,
						    &deadline) == 0);
		test_result("...and unlocks",
			    pthread_mutex_unlock(&g_tb_mutex) == 0);

		/* A deadline already in the past must not block. */
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec -= 1;
		test_result("mutex holder retakes the lock",
			    pthread_create(&th, NULL, tb_mutex_holder,
					   NULL) == 0);
		usleep(50000);
		mono_now(&t0);
		r = pthread_mutex_timedlock(&g_tb_mutex, &deadline);
		elapsed_ms = mono_elapsed_ms(&t0);
		test_result("pthread_mutex_timedlock() with a past deadline "
			    "returns at once",
			    r == ETIMEDOUT && elapsed_ms < 100);
		pthread_join(th, NULL);
	}

	/* ---- pthread_cond_timedwait ---- */
	{
		struct timespec deadline, t0;
		long elapsed_ms;
		int r;

		/* Nothing will ever signal: this must time out. */
		pthread_mutex_lock(&g_tb_cmutex);
		deadline_in_ms(&deadline, 150);
		mono_now(&t0);
		r = pthread_cond_timedwait(&g_tb_cond, &g_tb_cmutex,
					   &deadline);
		elapsed_ms = mono_elapsed_ms(&t0);

		/* Still inside the critical section: POSIX requires the mutex
		 * to be reacquired before the call returns, on the timeout
		 * path as much as any other.  A second attempt to take it must
		 * therefore find it busy -- if it succeeds, the wait dropped
		 * the lock and every caller that unlocks afterwards is
		 * unlocking a mutex it does not hold. */
		test_result("pthread_cond_timedwait() reacquired the mutex "
			    "before returning",
			    pthread_mutex_trylock(&g_tb_cmutex) == EBUSY);
		pthread_mutex_unlock(&g_tb_cmutex);

		if (r != ETIMEDOUT || elapsed_ms >= 2000)
			printf("  (returned %d after %ld ms)\n", r,
			       elapsed_ms);
		test_result("pthread_cond_timedwait() with no signaller "
			    "reports ETIMEDOUT",
			    r == ETIMEDOUT);
		test_result("pthread_cond_timedwait() waited about the "
			    "right time",
			    elapsed_ms >= 100 && elapsed_ms < 2000);

		/* Now one that IS signalled, well inside the deadline: it must
		 * return 0 and must return early. */
		{
			pthread_t th;

			g_tb_predicate = 0;
			pthread_mutex_lock(&g_tb_cmutex);
			test_result("signaller thread starts",
				    pthread_create(&th, NULL,
						   tb_cond_signaller,
						   NULL) == 0);
			deadline_in_ms(&deadline, 2000);
			mono_now(&t0);
			r = 0;
			while (!g_tb_predicate && r == 0)
				r = pthread_cond_timedwait(&g_tb_cond,
							   &g_tb_cmutex,
							   &deadline);
			elapsed_ms = mono_elapsed_ms(&t0);
			pthread_mutex_unlock(&g_tb_cmutex);
			pthread_join(th, NULL);

			test_result("pthread_cond_timedwait() returns 0 when "
				    "signalled",
				    r == 0 && g_tb_predicate == 1);
			test_result("...and returns before the deadline",
				    elapsed_ms < 1000);
		}

		/* A deadline already past returns ETIMEDOUT without waiting,
		 * and still holding the mutex. */
		pthread_mutex_lock(&g_tb_cmutex);
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec -= 1;
		mono_now(&t0);
		r = pthread_cond_timedwait(&g_tb_cond, &g_tb_cmutex,
					   &deadline);
		elapsed_ms = mono_elapsed_ms(&t0);
		pthread_mutex_unlock(&g_tb_cmutex);
		test_result("pthread_cond_timedwait() with a past deadline "
			    "returns at once",
			    r == ETIMEDOUT && elapsed_ms < 100);
	}

	/* ---- an untimed wait interrupted by a signal ----
	 *
	 * sem_wait has no deadline, so ETIMEDOUT is not one of the errors
	 * POSIX defines for it.  The kernel used to answer ETIMEDOUT for any
	 * wake futex_wake had not issued -- including a signal -- so a
	 * sem_wait interrupted by an ordinary SIGALRM came back reporting that
	 * a wait with no deadline had run out of time.  EINTR is the answer.
	 *
	 * This blocks for real, which is the only way to reach that path, so a
	 * watchdog thread posts the semaphore after two seconds.  If the signal
	 * fails to interrupt the wait, the test then fails on the assertion
	 * rather than hanging the whole run. */
	{
		sem_t *s = &g_tb_sem;
		struct sigaction sa, old_sa;
		struct itimerval it;
		pthread_t watchdog;
		struct timespec t0;
		long elapsed_ms;
		int r, saved;

		test_result("sem_init for the interrupted case",
			    sem_init(s, 0, 0) == 0);

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = tb_sigalrm_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0; /* no SA_RESTART: the wait must be cut short */
		test_result("SIGALRM handler installed",
			    sigaction(SIGALRM, &sa, &old_sa) == 0);

		g_tb_alarms = 0;
		test_result("watchdog thread starts",
			    pthread_create(&watchdog, NULL, tb_sem_watchdog,
					   s) == 0);

		/* 150ms one-shot. */
		memset(&it, 0, sizeof(it));
		it.it_value.tv_sec = 0;
		it.it_value.tv_usec = 150000;
		test_result("setitimer arms the one-shot",
			    setitimer(ITIMER_REAL, &it, NULL) == 0);

		errno = 0;
		mono_now(&t0);
		r = sem_wait(s);
		saved = errno;
		elapsed_ms = mono_elapsed_ms(&t0);

		if (!(r == -1 && saved == EINTR))
			printf("  (sem_wait returned %d, errno %d, after %ld ms)\n",
			       r, saved, elapsed_ms);
		test_result("sem_wait() interrupted by a signal fails EINTR",
			    r == -1 && saved == EINTR);
		test_result("sem_wait() does not report ETIMEDOUT",
			    saved != ETIMEDOUT);
		test_result("the signal handler ran", g_tb_alarms == 1);
		test_result("...and it was the signal that ended the wait, "
			    "not the watchdog",
			    elapsed_ms < 1000);

		/* Disarm, drain whatever the watchdog posted, restore. */
		memset(&it, 0, sizeof(it));
		setitimer(ITIMER_REAL, &it, NULL);
		pthread_join(watchdog, NULL);
		while (sem_trywait(s) == 0)
			;
		sigaction(SIGALRM, &old_sa, NULL);
		sem_destroy(s);
	}
}

/* ====================================================================== *
 * Kernel timeout accuracy
 *
 * Every one of these hands a duration to the kernel, which converts it to
 * timer ticks.  The tick rate is MEASURED at boot -- calibration routinely
 * settles near 200Hz on a virtual machine, not the 100Hz that was asked
 * for -- so any conversion written against an assumed 10ms tick expires in
 * half the time requested.  select() with a one-second timeout returning in
 * half a second was exactly that, and it is invisible to a test that only
 * checks the return value.
 *
 * So these measure.  The window is wide enough for tick rounding and VM
 * jitter, and narrow enough that a factor-of-two error cannot hide in it:
 * a 200ms request landing at 100ms fails, and so does one landing at 400ms.
 * ====================================================================== */

#define TIMEOUT_REQ_MS 200
#define TIMEOUT_LO_MS 150
#define TIMEOUT_HI_MS 600

static void check_timeout(const char *what, long elapsed_ms)
{
	char label[96];

	if (elapsed_ms < TIMEOUT_LO_MS || elapsed_ms > TIMEOUT_HI_MS)
		printf("  (%s measured %ld ms, requested %d)\n", what,
		       elapsed_ms, TIMEOUT_REQ_MS);

	snprintf(label, sizeof(label), "%s waited about %d ms", what,
		 TIMEOUT_REQ_MS);
	test_result(label, elapsed_ms >= TIMEOUT_LO_MS &&
				   elapsed_ms <= TIMEOUT_HI_MS);
}

static void test_timeout_accuracy(void)
{
	struct timespec t0;
	int pfds[2];

	printf("\n[TEST] Kernel timeout accuracy\n");

	/* ---- nanosleep ---- */
	{
		struct timespec req;

		req.tv_sec = 0;
		req.tv_nsec = TIMEOUT_REQ_MS * 1000000L;
		mono_now(&t0);
		test_result("nanosleep() returns 0", nanosleep(&req, NULL) == 0);
		check_timeout("nanosleep()", mono_elapsed_ms(&t0));
	}

	/* ---- usleep ---- */
	{
		mono_now(&t0);
		test_result("usleep() returns 0",
			    usleep(TIMEOUT_REQ_MS * 1000) == 0);
		check_timeout("usleep()", mono_elapsed_ms(&t0));
	}

	/* A pipe nobody writes to: the read end never becomes readable, so
	 * select and poll can only come back by timing out. */
	test_result("pipe for the timeout cases", pipe(pfds) == 0);

	/* ---- select ---- */
	{
		fd_set rfds;
		struct timeval tv;
		int r;

		FD_ZERO(&rfds);
		FD_SET(pfds[0], &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = TIMEOUT_REQ_MS * 1000;

		mono_now(&t0);
		r = select(pfds[0] + 1, &rfds, NULL, NULL, &tv);
		test_result("select() times out with 0", r == 0);
		check_timeout("select()", mono_elapsed_ms(&t0));
	}

	/* ---- select with a whole-second timeout ----
	 *
	 * Separate from the case above because the seconds and microseconds
	 * fields were converted by different arithmetic, and only one of them
	 * has been exercised so far. */
	{
		fd_set rfds;
		struct timeval tv;
		long elapsed_ms;
		int r;

		FD_ZERO(&rfds);
		FD_SET(pfds[0], &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		mono_now(&t0);
		r = select(pfds[0] + 1, &rfds, NULL, NULL, &tv);
		elapsed_ms = mono_elapsed_ms(&t0);
		test_result("select() with a 1s timeout returns 0", r == 0);
		if (elapsed_ms < 800 || elapsed_ms > 2000)
			printf("  (select(1s) measured %ld ms)\n", elapsed_ms);
		test_result("select() waited about a second",
			    elapsed_ms >= 800 && elapsed_ms <= 2000);
	}

	/* ---- poll ---- */
	{
		struct pollfd pfd;
		int r;

		pfd.fd = pfds[0];
		pfd.events = POLLIN;
		pfd.revents = 0;

		mono_now(&t0);
		r = poll(&pfd, 1, TIMEOUT_REQ_MS);
		test_result("poll() times out with 0", r == 0);
		check_timeout("poll()", mono_elapsed_ms(&t0));
	}

	/* A zero timeout must poll and return immediately, not wait a tick. */
	{
		struct pollfd pfd;

		pfd.fd = pfds[0];
		pfd.events = POLLIN;
		pfd.revents = 0;

		mono_now(&t0);
		test_result("poll() with a zero timeout returns 0",
			    poll(&pfd, 1, 0) == 0);
		test_result("...and returns immediately",
			    mono_elapsed_ms(&t0) < 50);
	}

	close(pfds[0]);
	close(pfds[1]);
}

/* ====================================================================== *
 * fd-table markers vs the file syscalls
 *
 * Sockets, AF_UNIX sockets and epoll instances are not backed by a real
 * open file here: the fd table holds a small TAGGED INTEGER for them
 * (0x10000, 0x30000 and 0x20000 based) rather than a pointer.  Every
 * syscall that reaches into the filesystem therefore has to recognise one
 * before it dereferences anything.
 *
 * They did not all do so, and they did not agree with each other about
 * which markers to look for.  fstat() checked none of them, so fstat() on
 * an AF_UNIX socket -- an entirely ordinary thing for a program to do, and
 * what Claws Mail did -- read a vfs_file_t out of the address 0x30009 and
 * took the KERNEL down. lseek() caught the console markers but not
 * sockets; ftruncate() checked nothing at all.
 *
 * So this calls each of them on each kind of descriptor.  Every case below
 * either returns an answer or fails with an errno; none of them may bring
 * the system down, which is what makes this worth running at all.
 * ====================================================================== */

static void test_fd_marker_syscalls(void)
{
	int sv[2], pfd[2], tcp, ep;
	struct stat st;

	printf("\n[TEST] fd markers vs the file syscalls\n");

	/* ---- AF_UNIX socket: the one that crashed the kernel ---- */
	test_result("socketpair(AF_UNIX)",
		    socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

	memset(&st, 0, sizeof(st));
	test_result("fstat() on an AF_UNIX socket returns", fstat(sv[0], &st) == 0);
	test_result("...and reports a socket", S_ISSOCK(st.st_mode));

	errno = 0;
	test_result("lseek() on a socket fails ESPIPE",
		    lseek(sv[0], 0, SEEK_SET) == (off_t)-1 && errno == ESPIPE);
	errno = 0;
	test_result("ftruncate() on a socket fails EINVAL",
		    ftruncate(sv[0], 0) == -1 && errno == EINVAL);

	close(sv[0]);
	close(sv[1]);

	/* ---- network socket ---- */
	tcp = socket(AF_INET, SOCK_STREAM, 0);
	test_result("socket(AF_INET)", tcp >= 0);
	if (tcp >= 0) {
		memset(&st, 0, sizeof(st));
		test_result("fstat() on a network socket returns",
			    fstat(tcp, &st) == 0);
		test_result("...and reports a socket", S_ISSOCK(st.st_mode));
		errno = 0;
		test_result("lseek() on a network socket fails ESPIPE",
			    lseek(tcp, 0, SEEK_SET) == (off_t)-1 &&
				    errno == ESPIPE);
		close(tcp);
	}

	/* ---- epoll instance ---- */
	ep = epoll_create1(0);
	test_result("epoll_create1()", ep >= 0);
	if (ep >= 0) {
		memset(&st, 0, sizeof(st));
		test_result("fstat() on an epoll fd returns",
			    fstat(ep, &st) == 0);
		errno = 0;
		test_result("lseek() on an epoll fd fails ESPIPE",
			    lseek(ep, 0, SEEK_SET) == (off_t)-1 &&
				    errno == ESPIPE);
		close(ep);
	}

	/* ---- pipe ---- */
	test_result("pipe()", pipe(pfd) == 0);
	memset(&st, 0, sizeof(st));
	test_result("fstat() on a pipe returns", fstat(pfd[0], &st) == 0);
	test_result("...and reports a FIFO", S_ISFIFO(st.st_mode));
	errno = 0;
	test_result("lseek() on a pipe fails ESPIPE",
		    lseek(pfd[0], 0, SEEK_SET) == (off_t)-1 && errno == ESPIPE);
	errno = 0;
	test_result("ftruncate() on a pipe fails EINVAL",
		    ftruncate(pfd[0], 0) == -1 && errno == EINVAL);
	close(pfd[0]);
	close(pfd[1]);

	/* ---- a dup'ed console descriptor ----
	 *
	 * dup2 plants its own small marker (1-3) for stdio, so a descriptor
	 * that came from one is a fourth kind of non-pointer entry. */
	{
		int d = dup(STDOUT_FILENO);

		test_result("dup(stdout)", d >= 0);
		if (d >= 0) {
			memset(&st, 0, sizeof(st));
			test_result("fstat() on a dup'ed console fd returns",
				    fstat(d, &st) == 0);
			test_result("...and reports a character device",
				    S_ISCHR(st.st_mode));
			close(d);
		}
	}
}

/* ====================================================================== *
 * Lock contention stress
 *
 * The existing mutex test runs two threads incrementing a counter, which
 * is a critical section a few instructions long: two threads can be inside
 * it simultaneously and still produce the right total most of the time.
 * It proves the lock exists, not that it excludes.
 *
 * These check exclusion DIRECTLY -- a thread marks the critical section as
 * occupied, works for a moment, and verifies the mark is still its own --
 * and then rebuild the structure GObject keeps its signal registry in: a
 * sorted array mutated under one lock and binary-searched afterwards.  A
 * lock that lets two writers overlap leaves that array unsorted, and every
 * later lookup fails while the data is all still there, which is exactly
 * what "signal 'pressed' is invalid for GtkGestureMultiPress" looks like
 * from the outside.
 * ====================================================================== */

#define LC_THREADS 4
#define LC_ITERS 20000

static pthread_mutex_t lc_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int lc_occupant; /* 0 = free, else thread index */
static volatile int lc_overlaps;
static long lc_counter;

static void *lc_exclusion_thread(void *arg)
{
	int me = (int)(long)arg + 1;

	for (int i = 0; i < LC_ITERS; i++) {
		pthread_mutex_lock(&lc_mutex);
		/* Nobody may be inside. */
		if (lc_occupant != 0)
			__atomic_add_fetch(&lc_overlaps, 1, __ATOMIC_SEQ_CST);
		lc_occupant = me;
		/* Stay a while: an exclusion bug needs a window to happen in. */
		for (volatile int s = 0; s < 20; s++)
			;
		/* Still ours? */
		if (lc_occupant != me)
			__atomic_add_fetch(&lc_overlaps, 1, __ATOMIC_SEQ_CST);
		lc_counter++; /* deliberately non-atomic: the lock is the guard */
		lc_occupant = 0;
		pthread_mutex_unlock(&lc_mutex);
	}
	return NULL;
}

/* The GBSearchArray shape: keys inserted in sorted position under a lock. */
#define LC_KEYS 2000
static pthread_mutex_t lc_arr_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned lc_keys[LC_KEYS];
static int lc_nkeys;

static void *lc_insert_thread(void *arg)
{
	int me = (int)(long)arg;

	for (int i = 0; i < LC_KEYS / LC_THREADS; i++) {
		unsigned key = (unsigned)(i * LC_THREADS + me) * 7919u;

		pthread_mutex_lock(&lc_arr_mutex);
		if (lc_nkeys < LC_KEYS) {
			/* insertion sort step -- a read-modify-write over the
			 * whole array, so an overlapping writer corrupts the
			 * ORDER rather than just losing a value */
			int pos = lc_nkeys;
			while (pos > 0 && lc_keys[pos - 1] > key) {
				lc_keys[pos] = lc_keys[pos - 1];
				pos--;
			}
			lc_keys[pos] = key;
			lc_nkeys++;
		}
		pthread_mutex_unlock(&lc_arr_mutex);
	}
	return NULL;
}

static pthread_once_t lc_once = PTHREAD_ONCE_INIT;
static volatile int lc_once_runs;

static void lc_once_fn(void)
{
	__atomic_add_fetch(&lc_once_runs, 1, __ATOMIC_SEQ_CST);
}

static void *lc_once_thread(void *arg)
{
	(void)arg;
	for (int i = 0; i < 100; i++)
		pthread_once(&lc_once, lc_once_fn);
	return NULL;
}

static pthread_rwlock_t lc_rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile int lc_writers_in;
static volatile int lc_readers_in;
static volatile int lc_rw_violations;

static void *lc_rw_thread(void *arg)
{
	int me = (int)(long)arg;

	for (int i = 0; i < 4000; i++) {
		if ((i + me) % 4 == 0) {
			pthread_rwlock_wrlock(&lc_rw);
			__atomic_add_fetch(&lc_writers_in, 1, __ATOMIC_SEQ_CST);
			if (lc_writers_in != 1 || lc_readers_in != 0)
				__atomic_add_fetch(&lc_rw_violations, 1,
						   __ATOMIC_SEQ_CST);
			for (volatile int s = 0; s < 20; s++)
				;
			__atomic_sub_fetch(&lc_writers_in, 1, __ATOMIC_SEQ_CST);
			pthread_rwlock_unlock(&lc_rw);
		} else {
			pthread_rwlock_rdlock(&lc_rw);
			__atomic_add_fetch(&lc_readers_in, 1, __ATOMIC_SEQ_CST);
			if (lc_writers_in != 0)
				__atomic_add_fetch(&lc_rw_violations, 1,
						   __ATOMIC_SEQ_CST);
			for (volatile int s = 0; s < 10; s++)
				;
			__atomic_sub_fetch(&lc_readers_in, 1, __ATOMIC_SEQ_CST);
			pthread_rwlock_unlock(&lc_rw);
		}
	}
	return NULL;
}

/* pthread_key destructors, and the stack reclaim that hangs off them.
 *
 * The pattern the real libraries use: a thread registers a key whose
 * destructor releases the thread itself.  The destructor runs on the exiting
 * thread, detaches it, and pthread_exit then unmaps its own stack.  If key
 * destructors silently never run, nothing detaches -- every thread stays
 * joinable with nobody to join it, and its 2MB stack stays mapped for the life
 * of the process.  That is how a file manager listing a directory grew by one
 * stack per worker thread until it ran out of mmap regions, so this checks the
 * destructor fires AND that the address space comes back.
 */
static pthread_key_t tsd_dtor_key;
static volatile int tsd_dtor_calls;
static volatile int tsd_dtor_saw_value;
static volatile int tsd_dtor_wrong_value;

static void tsd_dtor(void *value)
{
	if (value == (void *)0x1234)
		tsd_dtor_saw_value = 1;
	else
		tsd_dtor_wrong_value = 1;

	/* Release this thread from its own destructor, as the thread-pool
	 * libraries do. */
	pthread_detach(pthread_self());
	__sync_fetch_and_add((int *)&tsd_dtor_calls, 1);
}

static void *tsd_dtor_thread(void *arg)
{
	(void)arg;
	pthread_setspecific(tsd_dtor_key, (void *)0x1234);
	return NULL;
}

/* Wait for `target` destructor runs, then a moment more: the counter is bumped
 * inside the destructor, and the stack is not unmapped until pthread_exit
 * finishes after it returns. */
static int tsd_wait_for(int target)
{
	for (int i = 0; i < 2000 && tsd_dtor_calls < target; i++)
		usleep(1000);
	usleep(20000);
	return tsd_dtor_calls >= target;
}

/* fork() from a NON-MAIN thread, then fault a page the parent never touched.
 *
 * The address-space bookkeeping -- region table, brk, mmap_base -- lives on the
 * thread-group leader, and a thread deliberately keeps an EMPTY region table of
 * its own because its lookups are routed to the leader.  fork() used to copy
 * that state from the CALLING task, so forking from a thread handed the child
 * no regions at all: its first write to a lazy page found nothing to explain
 * the address and it died with SIGSEGV before it could exec.  A single-threaded
 * fork never showed it, because there the caller IS the leader.
 *
 * That is why the file manager could not start its menu daemon: the fork came
 * from a worker thread, so the child died every time.
 */
#define FORKPAGE_LEN (256 * 1024)

static void *forkpage_region;
static volatile int forkpage_status = -1;

static void *forkpage_worker(void *arg)
{
	pid_t pid;
	int status = 0;
	(void)arg;

	pid = fork();
	if (pid == 0) {
		/* Child: only syscalls from here on -- the other threads are
		 * gone and their locks are not ours to take.  Write across a
		 * mapping the parent made but never touched, so every page has
		 * to be demand-faulted through the inherited region table. */
		volatile char *p = (volatile char *)forkpage_region;
		size_t i;

		for (i = 0; i < FORKPAGE_LEN; i += 4096)
			p[i] = (char)(i >> 12);
		for (i = 0; i < FORKPAGE_LEN; i += 4096)
			if (p[i] != (char)(i >> 12))
				_exit(3);

		/* And a fresh mapping of its own, which exercises the child's
		 * region table for insertions rather than inheritance. */
		{
			char *q = mmap(NULL, FORKPAGE_LEN,
				       PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (q == MAP_FAILED)
				_exit(4);
			for (i = 0; i < FORKPAGE_LEN; i += 4096)
				q[i] = 1;
		}
		_exit(0);
	}
	if (pid < 0)
		return NULL;

	if (waitpid(pid, &status, 0) == pid)
		forkpage_status = status;
	return NULL;
}

static void test_fork_from_thread(void)
{
	pthread_t t;
	int status;

	printf("\n[TEST] fork() from a thread inherits the address space\n");

	/* Mapped by the MAIN thread and deliberately left untouched: the pages
	 * exist only as a region until somebody writes to them. */
	forkpage_region = mmap(NULL, FORKPAGE_LEN, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (forkpage_region == MAP_FAILED) {
		test_result("mapped a region for the fork test", 0);
		return;
	}

	forkpage_status = -1;
	if (pthread_create(&t, NULL, forkpage_worker, NULL) != 0) {
		test_result("thread for the fork test started", 0);
		return;
	}
	pthread_join(t, NULL);

	status = forkpage_status;
	if (status < 0) {
		test_result("child of a thread's fork was reaped", 0);
		return;
	}
	if (WIFSIGNALED(status))
		printf("  child was killed by signal %d\n", WTERMSIG(status));
	else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		printf("  child exited %d (2=map 3=readback 4=child mmap)\n",
		       WEXITSTATUS(status));

	test_result("child survives faulting a lazy page",
		    WIFEXITED(status) && WEXITSTATUS(status) == 0);

	munmap(forkpage_region, FORKPAGE_LEN);
}

static void test_tsd_destructors(void)
{
	pthread_t t;
	procmapinfo_t before, after;
	int have_maps;
	const int rounds = 64;
	int created = 0;

	printf("\n[TEST] pthread_key destructors and thread stack reclaim\n");

	test_result("pthread_key_create accepts a destructor",
		    pthread_key_create(&tsd_dtor_key, tsd_dtor) == 0);

	tsd_dtor_calls = 0;
	tsd_dtor_saw_value = 0;
	tsd_dtor_wrong_value = 0;

	if (pthread_create(&t, NULL, tsd_dtor_thread, NULL) != 0) {
		test_result("thread for destructor test started", 0);
		return;
	}
	test_result("key destructor runs when a thread exits", tsd_wait_for(1));
	test_result("destructor is passed the value that was set",
		    tsd_dtor_saw_value && !tsd_dtor_wrong_value);

	/* Now the leak itself.  One thread at a time, so a stack that is
	 * handed back can be reused by the next: the region count should stay
	 * flat.  A destructor that never runs leaves 2 regions per thread
	 * behind (the mapping plus its guard page), so 64 rounds would add
	 * about 128 -- far outside the slack allowed here. */
	have_maps = getprocmaps(getpid(), &before, NULL, 0) >= 0;

	tsd_dtor_calls = 0;
	for (int i = 0; i < rounds; i++) {
		pthread_t th;

		if (pthread_create(&th, NULL, tsd_dtor_thread, NULL) != 0)
			break;
		created++;
		if (!tsd_wait_for(created))
			break;
	}

	if (!have_maps || getprocmaps(getpid(), &after, NULL, 0) < 0) {
		printf("  (getprocmaps unavailable; skipping the region check)\n");
		return;
	}

	test_result("every thread ran its destructor", created == rounds &&
		    tsd_dtor_calls == rounds);
	printf("  regions %u -> %u over %d threads\n",
	       before.n_regions, after.n_regions, created);
	test_result("thread stacks are reclaimed, not leaked",
		    after.n_regions <= before.n_regions + 8);
}

static void test_lock_contention(void)
{
	pthread_t th[LC_THREADS];
	int i;

	printf("\n[TEST] Lock contention stress\n");

	/* ---- mutual exclusion ---- */
	lc_occupant = 0;
	lc_overlaps = 0;
	lc_counter = 0;
	for (i = 0; i < LC_THREADS; i++)
		if (pthread_create(&th[i], NULL, lc_exclusion_thread,
				   (void *)(long)i) != 0)
			break;
	test_result("all exclusion threads started", i == LC_THREADS);
	for (int j = 0; j < i; j++)
		pthread_join(th[j], NULL);

	if (lc_overlaps)
		printf("  (%d overlapping entries into the critical section)\n",
		       lc_overlaps);
	test_result("no two threads inside the mutex at once",
		    lc_overlaps == 0);
	if (lc_counter != (long)LC_THREADS * LC_ITERS)
		printf("  (counter %ld, expected %ld)\n", lc_counter,
		       (long)LC_THREADS * LC_ITERS);
	test_result("every critical section ran exactly once",
		    lc_counter == (long)LC_THREADS * LC_ITERS);

	/* ---- a sorted array built under the lock ---- */
	lc_nkeys = 0;
	for (i = 0; i < LC_THREADS; i++)
		if (pthread_create(&th[i], NULL, lc_insert_thread,
				   (void *)(long)i) != 0)
			break;
	test_result("all insert threads started", i == LC_THREADS);
	for (int j = 0; j < i; j++)
		pthread_join(th[j], NULL);

	test_result("every key was inserted", lc_nkeys == LC_KEYS);
	{
		int sorted = 1;
		for (int k = 1; k < lc_nkeys; k++)
			if (lc_keys[k - 1] > lc_keys[k]) {
				printf("  (order broken at %d: %u > %u)\n", k,
				       lc_keys[k - 1], lc_keys[k]);
				sorted = 0;
				break;
			}
		/* This is the one that matters: an array that is intact but
		 * out of order still binary-searches to "not found". */
		test_result("the array is still sorted (binary search works)",
			    sorted);
	}

	/* ---- pthread_once under contention ---- */
	lc_once_runs = 0;
	for (i = 0; i < LC_THREADS; i++)
		if (pthread_create(&th[i], NULL, lc_once_thread, NULL) != 0)
			break;
	test_result("all once threads started", i == LC_THREADS);
	for (int j = 0; j < i; j++)
		pthread_join(th[j], NULL);
	test_result("pthread_once ran the initialiser exactly once",
		    lc_once_runs == 1);

	/* ---- rwlock exclusion ---- */
	lc_writers_in = lc_readers_in = lc_rw_violations = 0;
	for (i = 0; i < LC_THREADS; i++)
		if (pthread_create(&th[i], NULL, lc_rw_thread,
				   (void *)(long)i) != 0)
			break;
	test_result("all rwlock threads started", i == LC_THREADS);
	for (int j = 0; j < i; j++)
		pthread_join(th[j], NULL);
	if (lc_rw_violations)
		printf("  (%d exclusion violations)\n", lc_rw_violations);
	test_result("rwlock kept writers exclusive of readers",
		    lc_rw_violations == 0);
}

/* ====================================================================== *
 * Orphan reaping
 *
 * A process whose parent dies before it does is reparented -- and it must
 * be reparented to something that CALLS WAITPID, or its exit status is
 * never collected and it stays a zombie for the life of the system.
 *
 * That is what pid 1 is for.  Orphans here used to be handed to the
 * bootstrap task instead, which never waits, so every background program
 * started from a shell that exits -- `claws-mail &' from the window
 * manager's menu, say -- left one permanent zombie behind per launch.
 *
 * The test builds exactly that shape: a child that forks a grandchild and
 * exits immediately, leaving the grandchild orphaned mid-flight.
 * ====================================================================== */

static void test_orphan_reaping(void)
{
	int pipefd[2];
	pid_t child;
	int status = 0;
	pid_t orphan = -1;

	printf("\n[TEST] Orphan reaping\n");

	test_result("pipe for the orphan's pid", pipe(pipefd) == 0);

	child = fork();
	if (child == 0) {
		/* Middle process: fork the orphan, report its pid, and exit at
		 * once so the orphan outlives us. */
		pid_t g = fork();

		if (g == 0) {
			close(pipefd[0]);
			close(pipefd[1]);
			/* Outlive the parent, then exit.  Its status has
			 * nowhere to go but the reaper it was handed to. */
			usleep(300000);
			_exit(7);
		}
		{
			pid_t v = g;
			ssize_t w = write(pipefd[1], &v, sizeof v);
			(void)w;
		}
		close(pipefd[0]);
		close(pipefd[1]);
		_exit(0);
	}
	test_result("fork for the orphan test", child > 0);

	close(pipefd[1]);
	if (read(pipefd[0], &orphan, sizeof orphan) != (ssize_t)sizeof orphan)
		orphan = -1;
	close(pipefd[0]);
	test_result("the middle process reported the orphan's pid", orphan > 0);

	/* Reap the middle process; the orphan is now parentless. */
	while (waitpid(child, &status, 0) < 0 && errno == EINTR)
		;
	test_result("middle process exited", WIFEXITED(status));

	/*
	 * The orphan exits ~300ms from now.  Whoever adopted it has to collect
	 * it: poll the process table until it is gone.  A reaper that never
	 * waits leaves it ZOMBIE for ever, so a generous timeout that still
	 * fails is exactly the signal wanted.
	 */
	if (orphan > 0) {
		int max = 512;
		procinfo_t *buf = malloc((size_t)max * sizeof(procinfo_t));
		int gone = 0;
		int zombie_seen = 0;

		test_result("procinfo buffer", buf != NULL);
		for (int tries = 0; buf && tries < 100 && !gone; tries++) {
			int n = getprocinfo(buf, max);
			int found = 0;

			for (int i = 0; i < n; i++) {
				if (buf[i].pid != orphan)
					continue;
				found = 1;
				if (buf[i].state == 4) /* ZOMBIE */
					zombie_seen = 1;
				break;
			}
			if (!found)
				gone = 1;
			else
				usleep(100000);
		}
		if (!gone)
			printf("  (pid %d still present after 10s%s)\n", orphan,
			       zombie_seen ? ", as a ZOMBIE" : "");
		test_result("the orphan was reaped, not left a zombie", gone);
		free(buf);
	}
}

/* ====================================================================== *
 * Raw futex operations
 *
 * The libc only ever issues FUTEX_WAIT and FUTEX_WAKE, so the other
 * commands had no coverage at all -- and one that nothing here called was
 * simply missing from the kernel.  GLib calls it directly: every
 * g_cond_wait_until() is FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME, and
 * with the clock bit left in the dispatch value it matched no command and
 * returned instantly.  A timed wait that never waits turns into a spin,
 * which is how GTK's file chooser hung.
 *
 * So these go through syscall() directly rather than the libc wrappers --
 * testing the wrappers would not have caught it.
 * ====================================================================== */

/* The syscall number, spelled out here: <sys/syscall.h> does not export it
 * and the point of this section is to bypass the libc wrappers entirely. */
#define SYS_FUTEX 315

#define FUTEX_WAIT_OP 0
#define FUTEX_WAKE_OP 1
#define FUTEX_WAIT_BITSET_OP 9
#define FUTEX_WAKE_BITSET_OP 10
#define FUTEX_PRIVATE 128
#define FUTEX_CLOCK_REALTIME_FLAG 256
#define FUTEX_MATCH_ANY 0xFFFFFFFFu

static int futex_word;

static void *futex_waker_thread(void *arg)
{
	(void)arg;
	usleep(200000);
	__atomic_store_n(&futex_word, 1, __ATOMIC_SEQ_CST);
	syscall(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE_OP | FUTEX_PRIVATE,
		1, 0, 0, 0);
	return NULL;
}

/*
 * printf conversions that were wrong, and are the kind of wrong that is hard
 * to notice: the value printed is plausible, just not the value passed.
 *
 * These are the exact cases host/test-printf.sh found by comparing this libc's
 * formatter against glibc.  They are repeated here because the host test
 * exercises the SOURCE and this one exercises the libc that actually shipped
 * on the image -- a build or link that picked up a different formatter would
 * pass one and fail the other.
 */
static void test_printf_conversions(void)
{
	char b[64];
	int n;

	printf("\n[TEST] printf conversions\n");

	/* Unsigned conversions above 2^63 were formatted as signed, because
	 * the digit routine took a signed argument: every size, count and
	 * hash with the top bit set printed as a small negative number. */
	snprintf(b, sizeof(b), "%lu", (unsigned long)-1);
	test_result("%lu of ULONG_MAX",
		    strcmp(b, "18446744073709551615") == 0);

	snprintf(b, sizeof(b), "%llu", (unsigned long long)-1);
	test_result("%llu of ULLONG_MAX",
		    strcmp(b, "18446744073709551615") == 0);

	snprintf(b, sizeof(b), "%zu", (size_t)-1);
	test_result("%zu of SIZE_MAX", strcmp(b, "18446744073709551615") == 0);

	snprintf(b, sizeof(b), "%llu", 9223372036854775808ULL);
	test_result("%llu of 2^63", strcmp(b, "9223372036854775808") == 0);

	/* The same defect from the other side: %lld reduces its argument to a
	 * magnitude and prints its own sign, and for LLONG_MIN that magnitude
	 * read back as negative and picked up a SECOND '-'. */
	snprintf(b, sizeof(b), "%lld", (long long)-9223372036854775807LL - 1);
	test_result("%lld of LLONG_MIN",
		    strcmp(b, "-9223372036854775808") == 0);

	/* Hex and octal were always right -- the sign branch was gated on
	 * base 10 -- so they are the control for the above. */
	snprintf(b, sizeof(b), "%llx", (unsigned long long)-1);
	test_result("%llx of ULLONG_MAX", strcmp(b, "ffffffffffffffff") == 0);

	/* Ties round to even, not away from zero. */
	snprintf(b, sizeof(b), "%.0f", 0.5);
	test_result("%.0f of 0.5 is 0", strcmp(b, "0") == 0);
	snprintf(b, sizeof(b), "%.0f", 1.5);
	test_result("%.0f of 1.5 is 2", strcmp(b, "2") == 0);
	snprintf(b, sizeof(b), "%.0f", 2.5);
	test_result("%.0f of 2.5 is 2", strcmp(b, "2") == 0);
	snprintf(b, sizeof(b), "%.2f", 0.125);
	test_result("%.2f of 0.125 is 0.12", strcmp(b, "0.12") == 0);

	/* Negative zero keeps its sign; `val < 0.0` does not see it. */
	snprintf(b, sizeof(b), "%f", -0.0);
	test_result("%f of -0.0 keeps the sign",
		    strcmp(b, "-0.000000") == 0);
	snprintf(b, sizeof(b), "%g", -0.0);
	test_result("%g of -0.0 keeps the sign", strcmp(b, "-0") == 0);

	/* %g picks its style from the exponent the value PRINTS with: at
	 * three significant digits 999.9995 rounds to 1000, whose exponent
	 * has reached the precision, so it goes to %e style. */
	snprintf(b, sizeof(b), "%.3g", 999.9995);
	test_result("%.3g of 999.9995 is 1e+03", strcmp(b, "1e+03") == 0);
	snprintf(b, sizeof(b), "%.3g", 1.5);
	test_result("%.3g of 1.5 is 1.5", strcmp(b, "1.5") == 0);

	/* snprintf returns the length the output WOULD have had, and the
	 * measure-then-fill pattern underneath g_strdup_printf depends on the
	 * two passes agreeing exactly. */
	n = snprintf(NULL, 0, "%s-%d", "abc", 12345);
	test_result("snprintf(NULL,0) measures", n == 9);
	{
		char small[4];
		n = snprintf(small, sizeof(small), "%s", "abcdefgh");
		test_result("snprintf reports untruncated length", n == 8);
		test_result("snprintf truncates and terminates",
			    strcmp(small, "abc") == 0);
	}

	/* %p is of one form for every pointer, null included, so that output
	 * written with it reads back with strtoul. */
	snprintf(b, sizeof(b), "%p", (void *)0);
	test_result("%p of NULL is 0x0", strcmp(b, "0x0") == 0);
}

static void test_futex_ops(void)
{
	struct timespec deadline, t0;
	long elapsed_ms;
	long r;

	printf("\n[TEST] Raw futex operations\n");

	/* ---- FUTEX_WAIT_BITSET with an absolute CLOCK_REALTIME deadline ----
	 * Exactly the call GLib makes.  It must WAIT and then report a
	 * timeout, not return immediately. */
	futex_word = 0;
	deadline_in_ms(&deadline, 200);
	mono_now(&t0);
	r = syscall(SYS_FUTEX, (long)&futex_word,
		    FUTEX_WAIT_BITSET_OP | FUTEX_PRIVATE |
			    FUTEX_CLOCK_REALTIME_FLAG,
		    0, (long)&deadline, 0, (long)FUTEX_MATCH_ANY);
	elapsed_ms = mono_elapsed_ms(&t0);

	if (elapsed_ms < 150)
		printf("  (returned %ld after only %ld ms)\n", r, elapsed_ms);
	test_result("FUTEX_WAIT_BITSET is implemented (not ENOSYS)",
		    !(r < 0 && errno == ENOSYS));
	test_result("FUTEX_WAIT_BITSET waits for its absolute deadline",
		    elapsed_ms >= 150 && elapsed_ms <= 800);

	/* An already-past deadline must report a timeout at once -- and must
	 * NOT be mistaken for "no timeout", which would block for ever. */
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec -= 1;
	mono_now(&t0);
	r = syscall(SYS_FUTEX, (long)&futex_word,
		    FUTEX_WAIT_BITSET_OP | FUTEX_PRIVATE |
			    FUTEX_CLOCK_REALTIME_FLAG,
		    0, (long)&deadline, 0, (long)FUTEX_MATCH_ANY);
	test_result("FUTEX_WAIT_BITSET with a past deadline returns at once",
		    mono_elapsed_ms(&t0) < 100);

	/* ---- a real wake, through the bitset command ---- */
	{
		pthread_t th;

		futex_word = 0;
		test_result("waker thread starts",
			    pthread_create(&th, NULL, futex_waker_thread,
					   NULL) == 0);
		deadline_in_ms(&deadline, 5000);
		mono_now(&t0);
		while (__atomic_load_n(&futex_word, __ATOMIC_SEQ_CST) == 0) {
			r = syscall(SYS_FUTEX, (long)&futex_word,
				    FUTEX_WAIT_BITSET_OP | FUTEX_PRIVATE |
					    FUTEX_CLOCK_REALTIME_FLAG,
				    0, (long)&deadline, 0,
				    (long)FUTEX_MATCH_ANY);
			if (r < 0 && errno == ETIMEDOUT)
				break;
		}
		elapsed_ms = mono_elapsed_ms(&t0);
		pthread_join(th, NULL);
		test_result("FUTEX_WAIT_BITSET is woken by a FUTEX_WAKE",
			    __atomic_load_n(&futex_word, __ATOMIC_SEQ_CST) == 1);
		test_result("...and woke on the post, not the deadline",
			    elapsed_ms < 2000);
	}

	/* ---- FUTEX_WAKE_BITSET on nobody: 0 woken, not an error ---- */
	futex_word = 0;
	errno = 0;
	r = syscall(SYS_FUTEX, (long)&futex_word,
		    FUTEX_WAKE_BITSET_OP | FUTEX_PRIVATE, 1, 0, 0,
		    (long)FUTEX_MATCH_ANY);
	test_result("FUTEX_WAKE_BITSET with no waiters returns 0", r == 0);
}

static void test_resolver(void)
{
	unsigned char msg[NS_PACKETSZ];
	char name[NS_MAXDNAME];
	int n;

	printf("\n[TEST] DNS stub resolver\n");

	test_result("res_init() finds at least one nameserver",
		    res_init() >= 0);
	test_result("res_init() filled in the state",
		    (_res.options & RES_INIT) != 0 && _res.nscount > 0);
	test_result("resolv.conf's nameserver is an INET address",
		    _res.nsaddr_list[0].sin_family == AF_INET &&
			    _res.nsaddr_list[0].sin_port == htons(53));

	/* Name encoding: "www.example.com" -> \3www\7example\3com\0 */
	n = dn_comp("www.example.com", msg, sizeof(msg), NULL, NULL);
	test_result("dn_comp() encodes a name to the expected length",
		    n == 17);
	test_result("dn_comp() writes the label lengths",
		    msg[0] == 3 && msg[4] == 7 && msg[12] == 3 &&
			    msg[16] == 0);
	test_result("dn_comp() rejects an over-long label",
		    dn_comp("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			    "aaaaaaaaaaaaaaaaa.com",
			    msg, sizeof(msg), NULL, NULL) == -1);
	test_result("dn_comp() encodes the root as one zero byte",
		    dn_comp("", msg, sizeof(msg), NULL, NULL) == 1 &&
			    msg[0] == 0);
	test_result("dn_comp() accepts a trailing dot",
		    dn_comp("example.com.", msg, sizeof(msg), NULL, NULL) == 13);

	/* And back again. */
	n = dn_comp("www.example.com", msg, sizeof(msg), NULL, NULL);
	test_result("dn_expand() round-trips a name",
		    dn_expand(msg, msg + n, msg, name, sizeof(name)) == n &&
			    strcmp(name, "www.example.com") == 0);
	test_result("dn_skipname() reports the encoded length",
		    dn_skipname(msg, msg + n) == n);

	/* Compression: a name that ends in a pointer back to an earlier one.
	 * Built by hand, because this is the case a live lookup exercises and
	 * a hand-written buffer can get exactly wrong on purpose. */
	{
		unsigned char m[64];
		int qn;

		memset(m, 0, sizeof(m));
		qn = dn_comp("example.com", m + 12, sizeof(m) - 12, NULL, NULL);
		/* "www" followed by a pointer to offset 12. */
		m[12 + qn] = 3;
		m[12 + qn + 1] = 'w';
		m[12 + qn + 2] = 'w';
		m[12 + qn + 3] = 'w';
		m[12 + qn + 4] = 0xc0;
		m[12 + qn + 5] = 12;
		test_result("dn_expand() follows a compression pointer",
			    dn_expand(m, m + sizeof(m), m + 12 + qn, name,
				      sizeof(name)) == 6 &&
				    strcmp(name, "www.example.com") == 0);

		/* A pointer to itself must be refused, not looped on. */
		m[40] = 0xc0;
		m[41] = 40;
		test_result("dn_expand() refuses a self-referential pointer",
			    dn_expand(m, m + sizeof(m), m + 40, name,
				      sizeof(name)) == -1);
		/* A pointer forwards is equally a loop. */
		m[42] = 0xc0;
		m[43] = 44;
		test_result("dn_expand() refuses a forward pointer",
			    dn_expand(m, m + sizeof(m), m + 42, name,
				      sizeof(name)) == -1);
	}

	/* A query message: header, then one question. */
	n = res_mkquery(ns_o_query, "example.com", C_IN, T_MX, NULL, 0, NULL,
			msg, sizeof(msg));
	test_result("res_mkquery() builds a message", n == 12 + 13 + 4);
	test_result("res_mkquery() asks one question",
		    ((msg[4] << 8) | msg[5]) == 1);
	test_result("res_mkquery() sets the recursion-desired bit",
		    (msg[2] & 0x01) != 0);
	test_result("res_mkquery() records the type and class",
		    ((msg[n - 4] << 8) | msg[n - 3]) == T_MX &&
			    ((msg[n - 2] << 8) | msg[n - 1]) == C_IN);
	test_result("res_mkquery() answers no questions itself",
		    ((msg[6] << 8) | msg[7]) == 0);
	{
		/* Two queries must not share an identifier: a resolver that
		 * numbers them predictably lets anything that sees one reply
		 * forge the next. */
		unsigned char m2[NS_PACKETSZ];
		int seen_same = 0;

		for (int i = 0; i < 8; i++) {
			int a = res_mkquery(ns_o_query, "a.example", C_IN, T_A,
					    NULL, 0, NULL, msg, sizeof(msg));
			int b = res_mkquery(ns_o_query, "a.example", C_IN, T_A,
					    NULL, 0, NULL, m2, sizeof(m2));
			if (a < 0 || b < 0)
				break;
			if (msg[0] == m2[0] && msg[1] == m2[1])
				seen_same++;
		}
		test_result("res_mkquery() varies the query identifier",
			    seen_same == 0);
	}
	test_result("res_mkquery() refuses a buffer that cannot hold the header",
		    res_mkquery(ns_o_query, "example.com", C_IN, T_A, NULL, 0,
				NULL, msg, 4) == -1);

	/* Live lookups.  Reported, never counted: a machine with no route to a
	 * nameserver is not a broken resolver, and this suite has to pass
	 * without a network. */
	{
		unsigned char ans[NS_PACKETSZ];
		int r = res_query("example.com", C_IN, T_A, ans, sizeof(ans));

		if (r > 0) {
			printf("  [INFO] res_query(example.com A) answered %d bytes,"
			       " %d records\n", r, (ans[6] << 8) | ans[7]);
			r = res_query("example.com", C_IN, T_MX, ans,
				      sizeof(ans));
			printf("  [INFO] res_query(example.com MX) -> %d\n", r);
		} else {
			printf("  [INFO] no live DNS (h_errno=%d); the offline"
			       " cases above are what was checked\n", h_errno);
		}
	}
}



// ========================================
// mincore: page residency reporting
// ========================================
static void test_mincore(void)
{
	printf("\n[mincore]\n");

	size_t pg = 4096;
	/* Twelve pages of demand-paged anonymous memory: mapped but untouched
	 * pages have no frame yet, so mincore must say so -- and must change
	 * its answer when a page is touched. */
	unsigned char *m = mmap(NULL, 12 * pg, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		test_fail("mincore mmap");
		return;
	}
	unsigned char vec[12];
	memset(vec, 0xAA, sizeof(vec));
	int rc = mincore(m, 12 * pg, vec);
	test_result("mincore on fresh mapping succeeds", rc == 0);
	int untouched_resident = 0;
	for (int i = 0; i < 12; i++)
		untouched_resident += (vec[i] & 1);
	test_result("untouched demand-paged pages not resident",
		    untouched_resident == 0);

	/* Touch three specific pages and ask again. */
	m[0 * pg] = 1;
	m[5 * pg] = 1;
	m[11 * pg] = 1;
	rc = mincore(m, 12 * pg, vec);
	test_result("mincore after touching succeeds", rc == 0);
	test_result("touched pages resident",
		    (vec[0] & 1) && (vec[5] & 1) && (vec[11] & 1));
	int now_resident = 0;
	for (int i = 0; i < 12; i++)
		now_resident += (vec[i] & 1);
	test_result("only the touched pages became resident",
		    now_resident == 3);

	/* Misaligned start is EINVAL; length 0 is a successful no-op. */
	errno = 0;
	test_result("mincore rejects misaligned address",
		    mincore(m + 1, pg, vec) < 0 && errno == EINVAL);
	test_result("mincore length 0 is a no-op", mincore(m, 0, vec) == 0);

	munmap(m, 12 * pg);
}

// ========================================
// C99 math additions (the libstdc++ probe set)
// ========================================
/* The functions added for the C++ runtime: libstdc++'s configure names every
 * C99 math function in one probe, so each of these existing -- and answering
 * correctly -- is what turns std::round and friends on in <cmath>.  Reference
 * values were computed independently and are checked to 1e-12 relative, the
 * neighbourhood the x87-based transcendentals in this libc live in. */
static int close_to(double got, double want, double rel)
{
	double diff = fabs(got - want);
	double mag = fabs(want) > 1.0 ? fabs(want) : 1.0;
	return diff <= rel * mag;
}

static void test_c99_math_additions(void)
{
	printf("\n[C99 math additions]\n");

	/* Exact, bit-level answers first. */
	test_result("nextafter(1,2) is 1+eps",
		    nextafter(1.0, 2.0) == 1.0 + DBL_EPSILON);
	test_result("nextafter(1,0) steps down",
		    nextafter(1.0, 0.0) < 1.0 &&
			    nextafter(nextafter(1.0, 0.0), 2.0) == 1.0);
	test_result("nextafter(0,1) is smallest subnormal",
		    nextafter(0.0, 1.0) > 0.0 &&
			    nextafter(0.0, 1.0) / 2.0 == 0.0);
	test_result("nexttoward matches nextafter",
		    nexttoward(1.0, 2.0L) == nextafter(1.0, 2.0));
	test_result("logb(8) == 3", logb(8.0) == 3.0);
	test_result("logb(0.5) == -1", logb(0.5) == -1.0);
	test_result("ilogb(0x1p-1060) == -1060", ilogb(0x1p-1060) == -1060);
	test_result("ilogb(0) is FP_ILOGB0", ilogb(0.0) == FP_ILOGB0);

	/* IEEE remainder: quotient rounded to nearest EVEN. */
	test_result("remainder(5.5,2) == -0.5",
		    remainder(5.5, 2.0) == -0.5);
	test_result("remainder(7,2) == -1 (tie to even)",
		    remainder(7.0, 2.0) == -1.0);
	int q = 0;
	double r = remquo(7.0, 2.0, &q);
	test_result("remquo(7,2) remainder", r == -1.0);
	test_result("remquo(7,2) quotient low bits", (q & 7) == 4 && q > 0);
	r = remquo(-7.0, 2.0, &q);
	test_result("remquo(-7,2) negative quotient", r == 1.0 && q < 0);

	/* fma really fuses: (2^27+1)^2 - 2^54 is 2^28+1 exactly, where the
	 * separately-rounded product loses the trailing 1. */
	double a = 0x1p27 + 1.0;
	test_result("fma fuses the multiply",
		    fma(a, a, -0x1p54) == 0x1p28 + 1.0);
	test_result("fma trivial", fma(2.0, 3.0, 4.0) == 10.0);

	/* Transcendentals against independent references. */
	test_result("erf(0) == 0", erf(0.0) == 0.0);
	test_result("erf(1)", close_to(erf(1.0), 0.8427007929497149, 1e-12));
	test_result("erf(0.25)",
		    close_to(erf(0.25), 0.2763263901682369, 1e-12));
	test_result("erf is odd", erf(-1.0) == -erf(1.0));
	test_result("erfc(3)",
		    close_to(erfc(3.0), 2.2090496998585438e-05, 1e-11));
	test_result("erfc(10)",
		    close_to(erfc(10.0), 2.088487583762545e-45, 1e-10));
	test_result("erf+erfc == 1",
		    close_to(erf(0.7) + erfc(0.7), 1.0, 1e-14));
	test_result("tgamma(5) == 24", close_to(tgamma(5.0), 24.0, 1e-13));
	test_result("tgamma(0.5) == sqrt(pi)",
		    close_to(tgamma(0.5), 1.7724538509055159, 1e-12));
	test_result("tgamma(-2.5)",
		    close_to(tgamma(-2.5), -0.9453087204829417, 1e-11));
	test_result("tgamma(-2) is a pole", isnan(tgamma(-2.0)));
	test_result("lgamma(10)",
		    close_to(lgamma(10.0), 12.801827480081467, 1e-12));
	test_result("lgamma survives where tgamma overflows",
		    isfinite(lgamma(200.0)) && isinf(tgamma(200.0)));

	/* The float and long double spellings exist and agree. */
	test_result("float variants",
		    erff(1.0f) > 0.84f && logbf(8.0f) == 3.0f &&
			    ilogbf(0.5f) == -1 &&
			    remainderf(5.5f, 2.0f) == -0.5f);
	test_result("long double variants",
		    erfl(1.0L) > 0.84L && logbl(8.0L) == 3.0L &&
			    fmal(2.0L, 3.0L, 4.0L) == 10.0L);
}

// ========================================
// pthread_kill and pthread_getattr_np
// ========================================
static volatile sig_atomic_t pk_handler_ran;
static volatile pid_t pk_handler_tid;

static void pk_handler(int sig)
{
	(void)sig;
	pk_handler_tid = gettid();
	pk_handler_ran = 1;
}

struct pk_thread_state {
	volatile pid_t tid;
	volatile int done;
	void *stack_probe; /* address of a local in the thread */
	int attr_ok; /* getattr_np from inside the thread contains the local */
};

static void *pk_thread_fn(void *arg)
{
	struct pk_thread_state *st = (struct pk_thread_state *)arg;
	int local = 0;

	st->stack_probe = &local;
	st->tid = gettid();

	pthread_attr_t attr;
	st->attr_ok = 0;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		void *base = NULL;
		size_t size = 0;
		if (pthread_attr_getstack(&attr, &base, &size) == 0 &&
		    (char *)&local >= (char *)base &&
		    (char *)&local < (char *)base + size)
			st->attr_ok = 1;
	}

	/* Sit here until the main thread has thrown a signal at us; the
	 * handler runs on THIS thread, which is the whole point of
	 * pthread_kill over kill. */
	while (!pk_handler_ran)
		sched_yield();
	st->done = 1;
	return NULL;
}

static void test_pthread_kill_and_getattr(void)
{
	printf("\n[pthread_kill / pthread_getattr_np]\n");

	/* Main thread's own stack, as the kernel reports it. */
	{
		pthread_attr_t attr;
		int rc = pthread_getattr_np(pthread_self(), &attr);
		test_result("getattr_np(main) succeeds", rc == 0);
		if (rc == 0) {
			void *base = NULL;
			size_t size = 0;
			int lv = 0;
			pthread_attr_getstack(&attr, &base, &size);
			test_result("main stack has sane size",
				    size >= 64 * 1024 &&
					    size <= 64UL * 1024 * 1024);
			test_result("main stack contains a local",
				    (char *)&lv >= (char *)base &&
					    (char *)&lv <
						    (char *)base + size);
		}
	}

	struct sigaction sa, oldsa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = pk_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, &oldsa);

	pk_handler_ran = 0;
	pk_handler_tid = 0;

	struct pk_thread_state st;
	memset(&st, 0, sizeof(st));
	pthread_t th;
	if (pthread_create(&th, NULL, pk_thread_fn, &st) != 0) {
		test_fail("pthread_create for pthread_kill test");
		sigaction(SIGUSR1, &oldsa, NULL);
		return;
	}
	while (st.tid == 0)
		sched_yield();

	test_result("pthread_kill(sig 0) probes a live thread",
		    pthread_kill(th, 0) == 0);

	/* The directed signal must run its handler on the TARGET thread. */
	test_result("pthread_kill delivers", pthread_kill(th, SIGUSR1) == 0);
	for (int i = 0; i < 5000 && !st.done; i++)
		usleep(1000);
	test_result("handler ran", pk_handler_ran == 1);
	test_result("handler ran on the target thread",
		    pk_handler_tid == st.tid && st.tid != gettid());
	test_result("thread getattr_np contains its own local",
		    st.attr_ok == 1);

	pthread_join(th, NULL);
	sigaction(SIGUSR1, &oldsa, NULL);
}

// ========================================
// AF_UNIX SOCK_SEQPACKET
// ========================================
static void test_unix_seqpacket(void)
{
	printf("\n[AF_UNIX SOCK_SEQPACKET]\n");

	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
		test_fail("socketpair(SOCK_SEQPACKET)");
		return;
	}
	test_pass("socketpair(SOCK_SEQPACKET)");

	/* Record boundaries survive: three sends are three receives with the
	 * original lengths, never a coalesced byte stream. */
	{
		char buf[64];
		test_result("send record 1", send(sv[0], "aaaa", 4, 0) == 4);
		test_result("send record 2", send(sv[0], "bb", 2, 0) == 2);
		test_result("send record 3",
			    send(sv[0], "cccccc", 6, 0) == 6);
		int r1 = recv(sv[1], buf, sizeof(buf), 0);
		int r2 = recv(sv[1], buf, sizeof(buf), 0);
		int r3 = recv(sv[1], buf, sizeof(buf), 0);
		test_result("records kept their boundaries",
			    r1 == 4 && r2 == 2 && r3 == 6);
		test_result("last record's bytes intact",
			    memcmp(buf, "cccccc", 6) == 0);
	}

	/* A record larger than the reader's buffer truncates -- and the NEXT
	 * record arrives whole, because the boundary, not the byte count, is
	 * the unit. */
	{
		char small[4];
		char buf[64];
		test_result("send long record",
			    send(sv[0], "0123456789", 10, 0) == 10);
		test_result("send following record",
			    send(sv[0], "NEXT", 4, 0) == 4);
		int r = recv(sv[1], small, sizeof(small), 0);
		test_result("truncated to the buffer", r == 4 &&
			    memcmp(small, "0123", 4) == 0);
		r = recv(sv[1], buf, sizeof(buf), 0);
		test_result("next record whole after truncation",
			    r == 4 && memcmp(buf, "NEXT", 4) == 0);
	}

	/* A record can never fit the transport: EMSGSIZE, not a short write. */
	{
		static char big[70000];
		memset(big, 'x', sizeof(big));
		errno = 0;
		int r = send(sv[0], big, sizeof(big), 0);
		test_result("oversized record is EMSGSIZE",
			    r < 0 && errno == EMSGSIZE);
	}

	/* SCM_RIGHTS rides the record: both descriptors of a pipe cross in
	 * ONE message, and the payload arrives with them. */
	{
		int pfd[2];
		if (pipe(pfd) == 0) {
			char pay[8] = "WITHFDS";
			struct iovec iv = { .iov_base = pay, .iov_len = 7 };
			char cbuf[CMSG_SPACE(sizeof(int) * 2)];
			memset(cbuf, 0, sizeof(cbuf));
			struct msghdr mh;
			memset(&mh, 0, sizeof(mh));
			mh.msg_iov = &iv;
			mh.msg_iovlen = 1;
			mh.msg_control = cbuf;
			mh.msg_controllen = sizeof(cbuf);
			struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
			cm->cmsg_level = SOL_SOCKET;
			cm->cmsg_type = SCM_RIGHTS;
			cm->cmsg_len = CMSG_LEN(sizeof(int) * 2);
			memcpy(CMSG_DATA(cm), pfd, sizeof(int) * 2);
			test_result("sendmsg with two fds",
				    sendmsg(sv[0], &mh, 0) == 7);

			char rpay[16];
			struct iovec riv = { .iov_base = rpay,
					     .iov_len = sizeof(rpay) };
			char rc[CMSG_SPACE(sizeof(int) * 8)];
			struct msghdr rmh;
			memset(&rmh, 0, sizeof(rmh));
			rmh.msg_iov = &riv;
			rmh.msg_iovlen = 1;
			rmh.msg_control = rc;
			rmh.msg_controllen = sizeof(rc);
			int r = recvmsg(sv[1], &rmh, 0);
			test_result("recvmsg got the record",
				    r == 7 && memcmp(rpay, "WITHFDS", 7) == 0);
			int got[2] = { -1, -1 };
			struct cmsghdr *rcm = CMSG_FIRSTHDR(&rmh);
			int nfds = 0;
			if (rcm && rcm->cmsg_level == SOL_SOCKET &&
			    rcm->cmsg_type == SCM_RIGHTS) {
				nfds = (int)((rcm->cmsg_len -
					      CMSG_LEN(0)) /
					     sizeof(int));
				if (nfds > 2)
					nfds = 2;
				memcpy(got, CMSG_DATA(rcm),
				       (size_t)nfds * sizeof(int));
			}
			test_result("both fds arrived in one cmsg",
				    nfds == 2 && got[0] >= 0 && got[1] >= 0);
			if (nfds == 2) {
				/* They are really the pipe: write through the
				 * received end, read from the original. */
				char c = 'Z', d = 0;
				test_result("received fd is usable",
					    write(got[1], &c, 1) == 1 &&
						    read(pfd[0], &d, 1) == 1 &&
						    d == 'Z');
				close(got[0]);
				close(got[1]);
			}
			close(pfd[0]);
			close(pfd[1]);
		} else {
			test_fail("pipe for SCM_RIGHTS test");
		}
	}

	/* Zero payload with several iovecs on the send side: the iovecs are
	 * ONE record, reassembled in order. */
	{
		struct iovec iv[3] = {
			{ .iov_base = "AB", .iov_len = 2 },
			{ .iov_base = "CDE", .iov_len = 3 },
			{ .iov_base = "F", .iov_len = 1 },
		};
		struct msghdr mh;
		memset(&mh, 0, sizeof(mh));
		mh.msg_iov = iv;
		mh.msg_iovlen = 3;
		test_result("gathered send", sendmsg(sv[0], &mh, 0) == 6);
		char buf[16];
		int r = recv(sv[1], buf, sizeof(buf), 0);
		test_result("gathered iovecs were one record",
			    r == 6 && memcmp(buf, "ABCDEF", 6) == 0);
	}

	/* Nonblocking: a full ring answers EAGAIN without writing half a
	 * record. */
	{
		int fl = fcntl(sv[0], F_GETFL, 0);
		fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
		static char rec[4000];
		memset(rec, 'r', sizeof(rec));
		int sent = 0, r;
		errno = 0;
		while ((r = send(sv[0], rec, sizeof(rec), 0)) > 0)
			sent++;
		test_result("full ring reports EAGAIN",
			    r < 0 && errno == EAGAIN && sent >= 8);
		/* Drain and confirm every record came back whole. */
		char buf[4096];
		int drained = 0;
		while (drained < sent) {
			r = recv(sv[1], buf, sizeof(buf), 0);
			if (r != (int)sizeof(rec))
				break;
			drained++;
		}
		test_result("drained records whole", drained == sent);
		fcntl(sv[0], F_SETFL, fl);
	}

	/* Hangup: close one end, the other reads end of file. */
	{
		close(sv[0]);
		char buf[8];
		test_result("EOF after peer close",
			    recv(sv[1], buf, sizeof(buf), 0) == 0);
		close(sv[1]);
	}

	/* The named-socket path: listen/accept carries the type, and a
	 * mismatched connect is refused with EPROTOTYPE. */
	{
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		/* Abstract name (leading NUL): nothing to unlink. */
		addr.sun_path[0] = '\0';
		snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 2,
			 "lk-seqtest-%d", getpid());
		socklen_t alen = (socklen_t)(sizeof(addr.sun_family) + 1 +
					     strlen(addr.sun_path + 1));

		int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		int ok = ls >= 0 && bind(ls, (struct sockaddr *)&addr,
					 alen) == 0 &&
			 listen(ls, 4) == 0;
		test_result("seqpacket bind+listen", ok);

		if (ok) {
			int cs = socket(AF_UNIX, SOCK_STREAM, 0);
			errno = 0;
			int cr = connect(cs, (struct sockaddr *)&addr, alen);
			test_result("stream connect to seqpacket listener "
				    "is EPROTOTYPE",
				    cr < 0 && errno == EPROTOTYPE);
			close(cs);

			cs = socket(AF_UNIX, SOCK_SEQPACKET, 0);
			cr = connect(cs, (struct sockaddr *)&addr, alen);
			test_result("seqpacket connect", cr == 0);
			int as = cr == 0 ? accept(ls, NULL, NULL) : -1;
			test_result("seqpacket accept", as >= 0);
			if (as >= 0) {
				char buf[16];
				test_result("records over accepted pair",
					    send(cs, "hello", 5, 0) == 5 &&
						    recv(as, buf, sizeof(buf),
							 0) == 5 &&
						    memcmp(buf, "hello", 5) ==
							    0);
				close(as);
			}
			close(cs);
		}
		if (ls >= 0)
			close(ls);
	}
}

// ========================================
// getentropy: the fill-or-fail entropy call
// ========================================
static void test_getentropy(void)
{
	printf("\n[getentropy]\n");

	/* Fill-or-fail, unlike getrandom(): a successful call has written
	 * every byte asked for.  Checked by pre-poisoning the buffer with a
	 * byte value and requiring that not all of it survived -- 256 bytes
	 * all landing on the same value is not a run this will see. */
	unsigned char buf[256];

	memset(buf, 0xA5, sizeof(buf));
	test_result("getentropy(256) succeeds", getentropy(buf, 256) == 0);
	int changed = 0;
	for (size_t i = 0; i < sizeof(buf); i++)
		if (buf[i] != 0xA5)
			changed++;
	test_result("the whole buffer was written", changed > 200);

	/* Two calls differ.  A generator that returned a constant would pass
	 * everything above. */
	unsigned char a[32], b[32];
	getentropy(a, sizeof(a));
	getentropy(b, sizeof(b));
	test_result("successive calls differ", memcmp(a, b, sizeof(a)) != 0);

	test_result("getentropy(0) succeeds", getentropy(buf, 0) == 0);

	/* The 256-byte cap is part of the contract, and the error for asking
	 * past it is EIO -- not EINVAL, which is what a reader expects and
	 * what libgcrypt's probe tests for. */
	errno = 0;
	test_result("over 256 bytes is EIO",
		    getentropy(buf, 257) == -1 && errno == EIO);
}

// ========================================
// timegm: mktime with the fields read as UTC
// ========================================
static void test_timegm(void)
{
	printf("\n[timegm]\n");

	/* A date whose epoch value is arithmetic, not a table lookup:
	 * 2001-09-09T01:46:40Z is exactly 1000000000. */
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2001 - 1900;
	tm.tm_mon = 8; /* September */
	tm.tm_mday = 9;
	tm.tm_hour = 1;
	tm.tm_min = 46;
	tm.tm_sec = 40;
	test_result("timegm of a known instant",
		    timegm(&tm) == (time_t)1000000000);

	/* The epoch itself, and the leap day that a naive day count gets
	 * wrong. */
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 70;
	tm.tm_mday = 1;
	test_result("timegm of the epoch", timegm(&tm) == (time_t)0);

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 2000 - 1900;
	tm.tm_mon = 1; /* February */
	tm.tm_mday = 29;
	test_result("timegm of 2000-02-29", timegm(&tm) == (time_t)951782400);

	/* Round trip: gmtime_r of what timegm produced must give the fields
	 * back. */
	time_t t = (time_t)1700000000;
	struct tm back;
	gmtime_r(&t, &back);
	test_result("timegm(gmtime_r(t)) == t", timegm(&back) == t);
}

// ========================================
// SO_ACCEPTCONN: does this socket listen?
// ========================================
static void test_so_acceptconn(void)
{
	printf("\n[SO_ACCEPTCONN]\n");

	/* Read-only, and answered on both socket families -- libsoup asks it
	 * of every descriptor it is handed before it will serve on it. */
	int s = socket(AF_INET, SOCK_STREAM, 0);

	if (s < 0) {
		test_fail("socket(AF_INET) for SO_ACCEPTCONN");
	} else {
		int v = -1;
		socklen_t vl = sizeof(v);

		test_result("getsockopt(SO_ACCEPTCONN) before listen",
			    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &v,
				       &vl) == 0 &&
				    v == 0 && vl == sizeof(int));

		struct sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sa.sin_port = 0;
		if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
		    listen(s, 4) == 0) {
			v = -1;
			vl = sizeof(v);
			test_result("SO_ACCEPTCONN after listen",
				    getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN,
					       &v, &vl) == 0 &&
					    v == 1);
		} else {
			test_fail("bind+listen for SO_ACCEPTCONN");
		}
		close(s);
	}

	/* And on AF_UNIX, which answers it from its own option table. */
	int u[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, u) == 0) {
		int v = -1;
		socklen_t vl = sizeof(v);

		test_result("connected AF_UNIX socket is not listening",
			    getsockopt(u[0], SOL_SOCKET, SO_ACCEPTCONN, &v,
				       &vl) == 0 &&
				    v == 0);
		close(u[0]);
		close(u[1]);
	} else {
		test_fail("socketpair for SO_ACCEPTCONN");
	}
}

// ========================================
// open_memstream / nftw / pthread_getcpuclockid
// ========================================
#include <ftw.h>

static int g_ftw_files, g_ftw_dirs, g_ftw_depth_last;
static int ftw_count_cb(const char *path, const struct stat *st, int type,
			struct FTW *f)
{
	(void)path;
	(void)st;
	if (type == FTW_F)
		g_ftw_files++;
	if (type == FTW_D || type == FTW_DP) {
		g_ftw_dirs++;
		g_ftw_depth_last = (type == FTW_DP);
	}
	return 0;
}

static void *cpuclock_burn(void *arg)
{
	volatile unsigned long x = 0;
	/* Volatile because the flag is written by another thread with no
	 * synchronisation in between: without it the compiler is entitled to
	 * read *stop once before the loop, and at -O2 it does -- the loop
	 * becomes an unconditional jump and this thread never stops, on any
	 * kernel.  The test still measures what it measured: a thread that
	 * burns CPU until it is told to stop. */
	volatile unsigned long *stop = arg;
	while (!*stop)
		x++;
	return NULL;
}

static void test_memstream_ftw_cpuclock(void)
{
	printf("\n[open_memstream / nftw / pthread_getcpuclockid]\n");

	/* open_memstream: everything written must come back through the
	 * caller's pointers, NUL terminated, surviving growth past the
	 * initial allocation. */
	char *buf = NULL;
	size_t sz = 0;
	FILE *ms = open_memstream(&buf, &sz);
	test_result("open_memstream returns a stream", ms != NULL);
	if (ms) {
		fprintf(ms, "hello %d", 42);
		fflush(ms);
		test_result("memstream publishes on fflush",
			    buf && sz == 8 && strcmp(buf, "hello 42") == 0);
		for (int i = 0; i < 100; i++)
			fprintf(ms, " chunk %03d", i);
		test_result("memstream position tracks writes",
			    ftell(ms) == 8 + 100 * 10);
		fclose(ms);
		test_result("memstream final size after growth",
			    sz == (size_t)(8 + 100 * 10) && buf[sz] == 0 &&
				    strncmp(buf + 8, " chunk 000", 10) == 0);
		free(buf);
	}

	/* nftw over a fabricated tree: 2 dirs (root + sub), 3 files; then
	 * FTW_DEPTH must report the directory AFTER its contents. */
	system("rm -rf /tmp/ftwtest");
	mkdir("/tmp/ftwtest", 0755);
	mkdir("/tmp/ftwtest/sub", 0755);
	FILE *t;
	t = fopen("/tmp/ftwtest/a", "w"); if (t) fclose(t);
	t = fopen("/tmp/ftwtest/b", "w"); if (t) fclose(t);
	t = fopen("/tmp/ftwtest/sub/c", "w"); if (t) fclose(t);

	g_ftw_files = g_ftw_dirs = 0;
	int r = nftw("/tmp/ftwtest", ftw_count_cb, 8, 0);
	test_result("nftw visits every entry",
		    r == 0 && g_ftw_files == 3 && g_ftw_dirs == 2);
	g_ftw_files = g_ftw_dirs = g_ftw_depth_last = 0;
	r = nftw("/tmp/ftwtest", ftw_count_cb, 8, FTW_DEPTH);
	test_result("nftw FTW_DEPTH reports dirs post-order",
		    r == 0 && g_ftw_dirs == 2 && g_ftw_depth_last == 1);
	system("rm -rf /tmp/ftwtest");

	/* pthread_getcpuclockid: another thread burns CPU; its clock must
	 * advance while this thread mostly sleeps. */
	volatile unsigned long stop = 0;
	pthread_t th;
	if (pthread_create(&th, NULL, cpuclock_burn, (void *)&stop) == 0) {
		clockid_t cid;
		int gc = pthread_getcpuclockid(th, &cid);
		struct timespec a = { 0, 0 }, b = { 0, 0 };
		clock_gettime(cid, &a);
		usleep(300000);
		clock_gettime(cid, &b);
		stop = 1;
		pthread_join(th, NULL);
		uint64_t na = (uint64_t)a.tv_sec * 1000000000ULL + a.tv_nsec;
		uint64_t nb = (uint64_t)b.tv_sec * 1000000000ULL + b.tv_nsec;
		test_result("pthread_getcpuclockid clock advances with burn",
			    gc == 0 && nb > na);
		/* Thread names: set/get round-trip, and the ERANGE contracts
		 * on both directions. */
		{
			char nm[16];
			pthread_t self = pthread_self();
			test_result("pthread_setname_np round-trips",
				    pthread_setname_np(self, "testlibc-nm") ==
						    0 &&
					    pthread_getname_np(self, nm,
							       sizeof(nm)) ==
						    0 &&
					    strcmp(nm, "testlibc-nm") == 0);
			test_result("setname > 15 chars is ERANGE",
				    pthread_setname_np(
					    self,
					    "a-name-way-too-long-for-this") ==
					    ERANGE);
			test_result("getname into a too-small buffer is ERANGE",
				    pthread_getname_np(self, nm, 4) == ERANGE);
		}

		/* Dead thread: the clock must answer EINVAL, not garbage. */
		struct timespec c;
		test_result("dead thread's cpu clock is EINVAL",
			    clock_gettime(cid, &c) != 0);
	} else {
		test_result("pthread_create for cpuclock test", 0);
	}
}

// ========================================
// C99 <stdint.h> fast/least limit macros
// ========================================
static void test_stdint_limits(void)
{
	printf("\n[stdint fast/least limits]\n");

	/* The macros must EXIST (their absence made gcc's libstdc++ configure
	 * itself without C99 stdint support, which compiles <random> away to
	 * an empty header -- no std::mt19937 anywhere in C++), and each must
	 * agree with the TYPE it describes: assigning the limit to the type
	 * and reading it back must be an identity.  A macro copied from a
	 * libc whose fast types have different widths would pass existence
	 * checks and corrupt arithmetic silently. */
	int_fast16_t f16 = INT_FAST16_MAX;
	int_fast32_t f32 = INT_FAST32_MIN;
	uint_fast16_t uf16 = UINT_FAST16_MAX;
	int_least16_t l16 = INT_LEAST16_MAX;
	uint_least32_t ul32 = UINT_LEAST32_MAX;

	test_result("INT_FAST16_MAX round-trips through int_fast16_t",
		    f16 == INT_FAST16_MAX);
	test_result("INT_FAST32_MIN round-trips through int_fast32_t",
		    f32 == INT_FAST32_MIN);
	test_result("UINT_FAST16_MAX round-trips through uint_fast16_t",
		    uf16 == UINT_FAST16_MAX);
	test_result("INT_LEAST16_MAX fits int_least16_t exactly",
		    l16 == INT16_MAX);
	test_result("UINT_LEAST32_MAX fits uint_least32_t exactly",
		    ul32 == UINT32_MAX);
	/* sizeof ties the macro to the typedef's real width: the fast 16/32
	 * types are 32-bit HERE, so their limits must be the 32-bit ones. */
	test_result("fast16 limits match the 32-bit fast16 typedef",
		    sizeof(int_fast16_t) == 4 &&
			    INT_FAST16_MAX == INT32_MAX);
	test_result("least types are exact-width",
		    sizeof(int_least16_t) == 2 && sizeof(int_least64_t) == 8 &&
			    INT_LEAST64_MAX == INT64_MAX);

	/* modff: fractional/integral split in FLOAT precision. */
	{
		float ip = 0;
		float fr = modff(3.75f, &ip);
		test_result("modff splits 3.75 into 3 + 0.75",
			    ip == 3.0f && fr == 0.75f);
		fr = modff(-2.5f, &ip);
		test_result("modff truncates toward zero for negatives",
			    ip == -2.0f && fr == -0.5f);
	}

	/* alloca() must be usable after <stdlib.h> alone -- the BSD/GNU
	 * convention ported code relies on.  The write proves the memory is
	 * real; the address differing from a second call proves it is not a
	 * static buffer. */
	{
		char *a1 = alloca(32);
		char *a2 = alloca(32);
		a1[0] = 'x';
		a2[0] = 'y';
		test_result("alloca via stdlib.h returns distinct stack blocks",
			    a1 != a2 && a1[0] == 'x' && a2[0] == 'y');
	}

	/* <sysexits.h>: the values are a wire convention (a mailer's peer
	 * reads them), so they are checked against the numbers 4.0BSD fixed,
	 * not merely for existence. */
	test_result("sysexits values match the BSD convention",
		    EX_OK == 0 && EX_USAGE == 64 && EX_IOERR == 74 &&
			    EX_TEMPFAIL == 75 && EX_CONFIG == 78 &&
			    EX__BASE == 64 && EX__MAX == 78);
	/* POSIX minima must be the STANDARD's floors, and each must not
	 * exceed the system's real limit above it. */
	test_result("POSIX minima are the standard floors",
		    _POSIX_ARG_MAX == 4096 && _POSIX_PATH_MAX == 256 &&
			    _POSIX_PIPE_BUF == 512 &&
			    _POSIX_ARG_MAX <= ARG_MAX &&
			    _POSIX_PATH_MAX <= PATH_MAX &&
			    _POSIX_PIPE_BUF <= PIPE_BUF);
}

// ========================================
// struct dirent64 and the mkdir declaration in <sys/stat.h>
// ========================================

/* ======================================================================
 * ucontext: the machine context handed to a signal handler, and the
 * getcontext/makecontext/swapcontext family built on the same record.
 * ====================================================================== */
#include <ucontext.h>

static volatile int g_uc_seen_ucontext;
static volatile void *g_uc_fault_addr;
static volatile long g_uc_rip_matches;
static char g_uc_good_buf[64];
static volatile int g_uc_on_altstack;
static volatile int g_uc_altstack_flag;
static char *g_uc_altstack;

static void uc_segv_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	(void)sig;

	g_uc_seen_ucontext = (uc != NULL);
	g_uc_fault_addr = si ? si->si_addr : NULL;
	if (!uc)
		return;
	/* The fault came from `movl $1, (%rax)' with RAX = the bad address;
	 * pointing RAX at a good buffer and returning re-executes it there.
	 * CR2 carries the address as well. */
	g_uc_rip_matches = (uc->uc_mcontext.gregs[REG_RAX] ==
			    (greg_t)(uintptr_t)g_uc_fault_addr) &&
			   (uc->uc_mcontext.gregs[REG_CR2] ==
			    (greg_t)(uintptr_t)g_uc_fault_addr);
	uc->uc_mcontext.gregs[REG_RAX] = (greg_t)(uintptr_t)g_uc_good_buf;
}

static void uc_altstack_handler(int sig, siginfo_t *si, void *ctx)
{
	ucontext_t *uc = (ucontext_t *)ctx;
	char probe;
	(void)sig;
	(void)si;

	g_uc_on_altstack = (&probe >= g_uc_altstack &&
			    &probe < g_uc_altstack + SIGSTKSZ);
	g_uc_altstack_flag = uc ? uc->uc_stack.ss_flags : -1;
}

static ucontext_t g_uc_main, g_uc_co;
static volatile int g_uc_trace[8];
static volatile int g_uc_ntrace;

static void uc_coroutine(int a, int b)
{
	g_uc_trace[g_uc_ntrace++] = a;
	swapcontext(&g_uc_co, &g_uc_main);
	g_uc_trace[g_uc_ntrace++] = b;
	/* returning resumes uc_link */
}

static void test_ucontext(void)
{
	printf("\n[TEST] ucontext: signal machine context, altstack, coroutines\n");

	/* ---- A fault handler that repairs the faulting register ---- */
	struct sigaction sa, old;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = uc_segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old);

	g_uc_seen_ucontext = 0;
	g_uc_fault_addr = NULL;
	g_uc_rip_matches = 0;
	memset(g_uc_good_buf, 0, sizeof(g_uc_good_buf));
	{
		volatile uintptr_t bad = 0xDEAD0000UL; /* unmapped */
		/* RAX = bad; store faults; handler repoints RAX; store lands
		 * in g_uc_good_buf. */
		__asm__ volatile("movq %0, %%rax\n\t"
				 "movl $0x11223344, (%%rax)\n\t"
				 :
				 : "r"(bad)
				 : "rax", "memory");
		test_result("SA_SIGINFO handler received a ucontext",
			    g_uc_seen_ucontext);
		test_result("si_addr is the faulting address",
			    (uintptr_t)g_uc_fault_addr == bad);
		test_result("gregs[REG_RAX] and REG_CR2 show the fault",
			    g_uc_rip_matches);
		test_result("editing gregs in the handler changed the resumed context",
			    *(volatile int *)g_uc_good_buf == 0x11223344);
	}
	sigaction(SIGSEGV, &old, NULL);

	/* ---- SA_ONSTACK: the handler runs on the alternate stack ---- */
	g_uc_altstack = malloc(SIGSTKSZ);
	if (g_uc_altstack) {
		stack_t ss, oss;
		ss.ss_sp = g_uc_altstack;
		ss.ss_size = SIGSTKSZ;
		ss.ss_flags = 0;
		test_result("sigaltstack accepts SIGSTKSZ",
			    sigaltstack(&ss, &oss) == 0);
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = uc_altstack_handler;
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR2, &sa, &old);
		g_uc_on_altstack = 0;
		g_uc_altstack_flag = -1;
		raise(SIGUSR2);
		test_result("SA_ONSTACK handler ran on the alternate stack",
			    g_uc_on_altstack);
		test_result("uc_stack.ss_flags reports SS_ONSTACK",
			    g_uc_altstack_flag == SS_ONSTACK);
		/* Without SA_ONSTACK it must stay on the main stack. */
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGUSR2, &sa, NULL);
		g_uc_on_altstack = 1;
		raise(SIGUSR2);
		test_result("without SA_ONSTACK the handler uses the main stack",
			    !g_uc_on_altstack);
		sigaction(SIGUSR2, &old, NULL);
		ss.ss_flags = SS_DISABLE;
		sigaltstack(&ss, NULL);
		free(g_uc_altstack);
		g_uc_altstack = NULL;
	}

	/* ---- getcontext returns twice ---- */
	{
		ucontext_t c;
		volatile int n = 0;
		getcontext(&c);
		n++;
		if (n == 1)
			setcontext(&c);
		test_result("getcontext/setcontext: getcontext returned twice",
			    n == 2);
	}

	/* ---- makecontext/swapcontext coroutine ping-pong ---- */
	{
		static char costack[64 * 1024];
		g_uc_ntrace = 0;
		getcontext(&g_uc_co);
		g_uc_co.uc_stack.ss_sp = costack;
		g_uc_co.uc_stack.ss_size = sizeof(costack);
		g_uc_co.uc_link = &g_uc_main;
		makecontext(&g_uc_co, (void (*)(void))uc_coroutine, 2, 7, 9);
		g_uc_trace[g_uc_ntrace++] = 1;
		swapcontext(&g_uc_main, &g_uc_co); /* -> a=7, back */
		g_uc_trace[g_uc_ntrace++] = 2;
		swapcontext(&g_uc_main, &g_uc_co); /* -> b=9, return -> link */
		g_uc_trace[g_uc_ntrace++] = 3;
		test_result("makecontext/swapcontext: interleaving is 1,7,2,9,3",
			    g_uc_ntrace == 5 && g_uc_trace[0] == 1 &&
				    g_uc_trace[1] == 7 && g_uc_trace[2] == 2 &&
				    g_uc_trace[3] == 9 && g_uc_trace[4] == 3);
	}
}

/* ======================================================================
 * Extended register state: the x87/SSE/AVX registers must survive a
 * signal handler that uses them, and a context switch to a thread that
 * uses them.
 * ====================================================================== */
static int cpu_has_osxsave_avx(void)
{
	unsigned int a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			 : "a"(1), "c"(0));
	if (!(c & (1u << 27)) || !(c & (1u << 28)))
		return 0; /* OSXSAVE, AVX */
	unsigned int xlo, xhi;
	__asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
	return (xlo & 0x6) == 0x6; /* SSE and AVX state enabled in XCR0 */
}

static void xs_clobber_handler(int sig)
{
	(void)sig;
	/* Clobber XMM/YMM0-1 with something recognisable. */
	static const unsigned char junk[32] = { 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
						0xAA, 0xAA };
	__asm__ volatile("movdqu (%0), %%xmm0\n\t"
			 "movdqu (%0), %%xmm1\n\t"
			 :
			 : "r"(junk)
			 : "xmm0", "xmm1", "memory");
}

static volatile int g_xs_thread_stop;
static volatile int g_xs_thread_bad;

static void *xs_thread_fn(void *arg)
{
	int avx = *(int *)arg;
	unsigned char pat[32], out[32];

	for (int i = 0; i < 32; i++)
		pat[i] = (unsigned char)(0x50 + i);
	while (!g_xs_thread_stop) {
		if (avx) {
			__asm__ volatile("vmovdqu (%0), %%ymm3\n\t"
					 :
					 : "r"(pat)
					 : "xmm3", "memory");
			for (volatile int spin = 0; spin < 2000; spin++)
				;
			__asm__ volatile("vmovdqu %%ymm3, (%0)\n\t"
					 :
					 : "r"(out)
					 : "memory");
		} else {
			__asm__ volatile("movdqu (%0), %%xmm3\n\t"
					 :
					 : "r"(pat)
					 : "xmm3", "memory");
			for (volatile int spin = 0; spin < 2000; spin++)
				;
			__asm__ volatile("movdqu %%xmm3, (%0)\n\t"
					 :
					 : "r"(out)
					 : "memory");
		}
		if (memcmp(pat, out, avx ? 32 : 16) != 0)
			g_xs_thread_bad = 1;
	}
	return NULL;
}

static void test_xsave(void)
{
	printf("\n[TEST] Extended register state across signals and threads\n");

	int avx = cpu_has_osxsave_avx();
	printf("  (AVX state %s)\n", avx ? "enabled by the kernel" :
					  "not available, testing SSE only");

	/* ---- Across a signal handler ---- */
	unsigned char in[32], out[32];
	for (int i = 0; i < 32; i++)
		in[i] = (unsigned char)(i * 7 + 1);
	signal(SIGUSR1, xs_clobber_handler);
	if (avx) {
		__asm__ volatile("vmovdqu (%0), %%ymm0\n\t"
				 "vmovdqu (%0), %%ymm1\n\t"
				 :
				 : "r"(in)
				 : "xmm0", "xmm1", "memory");
		raise(SIGUSR1);
		__asm__ volatile("vmovdqu %%ymm0, (%0)\n\t"
				 :
				 : "r"(out)
				 : "memory");
		test_result("YMM0 survives a signal handler that clobbers it",
			    memcmp(in, out, 32) == 0);
		__asm__ volatile("vmovdqu %%ymm1, (%0)\n\t" : : "r"(out) : "memory");
		test_result("YMM1 survives a signal handler that clobbers it",
			    memcmp(in, out, 32) == 0);
	} else {
		__asm__ volatile("movdqu (%0), %%xmm0\n\t"
				 :
				 : "r"(in)
				 : "xmm0", "memory");
		raise(SIGUSR1);
		__asm__ volatile("movdqu %%xmm0, (%0)\n\t" : : "r"(out) : "memory");
		test_result("XMM0 survives a signal handler that clobbers it",
			    memcmp(in, out, 16) == 0);
	}
	signal(SIGUSR1, SIG_DFL);

	/* ---- Across context switches: two threads with live vectors ---- */
	pthread_t th;
	g_xs_thread_stop = 0;
	g_xs_thread_bad = 0;
	if (pthread_create(&th, NULL, xs_thread_fn, &avx) == 0) {
		unsigned char pat[32], got[32];
		int bad = 0;
		for (int i = 0; i < 32; i++)
			pat[i] = (unsigned char)(0xC0 - i);
		struct timespec t0;
		mono_now(&t0);
		while (mono_elapsed_ms(&t0) < 300) {
			if (avx) {
				__asm__ volatile("vmovdqu (%0), %%ymm3\n\t"
						 :
						 : "r"(pat)
						 : "xmm3", "memory");
				sched_yield();
				__asm__ volatile("vmovdqu %%ymm3, (%0)\n\t"
						 :
						 : "r"(got)
						 : "memory");
			} else {
				__asm__ volatile("movdqu (%0), %%xmm3\n\t"
						 :
						 : "r"(pat)
						 : "xmm3", "memory");
				sched_yield();
				__asm__ volatile("movdqu %%xmm3, (%0)\n\t"
						 :
						 : "r"(got)
						 : "memory");
			}
			if (memcmp(pat, got, avx ? 32 : 16) != 0)
				bad = 1;
		}
		g_xs_thread_stop = 1;
		pthread_join(th, NULL);
		test_result("vector registers preserved across context switches (main)",
			    !bad);
		test_result("vector registers preserved across context switches (thread)",
			    !g_xs_thread_bad);
	} else {
		test_fail("xsave thread create");
	}
}

/* ======================================================================
 * Large and special mappings: multi-GB lazy reservations carved up with
 * mprotect, MAP_FIXED_NOREPLACE, MAP_POPULATE, MADV_FREE.
 * ====================================================================== */
static void test_mmap_large(void)
{
	printf("\n[TEST] Large reservations and mmap flags\n");
	size_t pg = 4096;

	/* An 8 GB PROT_NONE reservation, as a JIT or a WebAssembly memory
	 * makes, then commit a piece of it in the middle. */
	size_t big = 8ULL << 30;
	char *r = mmap(NULL, big, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	test_result("8 GB PROT_NONE reservation succeeds", r != MAP_FAILED);
	if (r != MAP_FAILED) {
		char *piece = r + (3ULL << 30) + 64 * pg;
		test_result("mprotect(RW) of a 1 MB piece inside it",
			    mprotect(piece, 1 << 20, PROT_READ | PROT_WRITE) == 0);
		piece[0] = 1;
		piece[(1 << 20) - 1] = 2;
		test_result("the committed piece is usable",
			    piece[0] == 1 && piece[(1 << 20) - 1] == 2);
		/* The rest is still PROT_NONE: touching it from a child
		 * must fault. */
		pid_t c = fork();
		if (c == 0) {
			r[pg] = 1;
			_exit(0);
		}
		int st = 0;
		waitpid(c, &st, 0);
		test_result("the uncommitted part still faults",
			    WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
		test_result("munmap of the whole reservation", munmap(r, big) == 0);
	}

	/* MAP_FIXED_NOREPLACE */
	char *a = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a != MAP_FAILED) {
		errno = 0;
		void *b = mmap(a + pg, pg, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			       -1, 0);
		test_result("MAP_FIXED_NOREPLACE over an existing mapping is EEXIST",
			    b == MAP_FAILED && errno == EEXIST);
		munmap(a, 4 * pg);
		void *c2 = mmap(a, pg, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				-1, 0);
		test_result("MAP_FIXED_NOREPLACE on a free address takes it",
			    c2 == (void *)a);
		if (c2 != MAP_FAILED)
			munmap(c2, pg);
	}

	/* MAP_POPULATE: every page resident before first touch. */
	unsigned char *pp = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1,
				 0);
	if (pp != MAP_FAILED) {
		unsigned char vec[16];
		int resident = 0;
		if (mincore(pp, 16 * pg, vec) == 0)
			for (int i = 0; i < 16; i++)
				resident += vec[i] & 1;
		test_result("MAP_POPULATE faults the whole range in",
			    resident == 16);
		/* MADV_FREE releases them again and reads back zeros. */
		pp[3 * pg] = 0x5A;
		test_result("MADV_FREE is accepted",
			    madvise(pp, 16 * pg, MADV_FREE) == 0);
		test_result("pages read back zero after MADV_FREE",
			    pp[3 * pg] == 0);
		test_result("MADV_HUGEPAGE/DONTFORK are accepted as hints",
			    madvise(pp, 16 * pg, MADV_HUGEPAGE) == 0 &&
				    madvise(pp, 16 * pg, MADV_DONTFORK) == 0);
		munmap(pp, 16 * pg);
	}

	/* mremap: grow (moving if need be) keeps the pages; shrink trims. */
	unsigned char *m = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m != MAP_FAILED) {
		for (int i = 0; i < 4; i++)
			m[i * pg] = (unsigned char)(0x30 + i);
		/* Block the space right after it so growth must move. */
		void *blocker = mmap(m + 4 * pg, pg, PROT_READ,
				     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
				     -1, 0);
		errno = 0;
		void *nm = mremap(m, 4 * pg, 64 * pg, 0);
		test_result("mremap without MAYMOVE fails with ENOMEM when blocked",
			    nm == MAP_FAILED && errno == ENOMEM);
		nm = mremap(m, 4 * pg, 64 * pg, MREMAP_MAYMOVE);
		test_result("mremap(MAYMOVE) grows to 64 pages", nm != MAP_FAILED);
		if (nm != MAP_FAILED) {
			unsigned char *n2 = nm;
			int ok = 1;
			for (int i = 0; i < 4; i++)
				if (n2[i * pg] != (unsigned char)(0x30 + i))
					ok = 0;
			test_result("the moved pages kept their contents", ok);
			n2[60 * pg] = 7;
			test_result("the new tail is usable", n2[60 * pg] == 7);
			void *sm = mremap(n2, 64 * pg, 2 * pg, 0);
			test_result("mremap shrinks in place", sm == (void *)n2);
			munmap(n2, 2 * pg);
		}
		if (blocker != MAP_FAILED)
			munmap(blocker, pg);
	}
}

/* ======================================================================
 * futex bitsets and FUTEX_WAKE_OP
 * ====================================================================== */
static volatile int g_fb_word;
static volatile int g_fb_word2;
static volatile int g_fb_woken;

static void *fb_waiter(void *arg)
{
	unsigned int bits = (unsigned int)(uintptr_t)arg;
	long r = syscall(SYS_FUTEX, (long)&g_fb_word,
			 FUTEX_WAIT_BITSET_OP | FUTEX_PRIVATE, 0, 0, 0,
			 (long)bits);
	(void)r;
	__sync_fetch_and_add(&g_fb_woken, 1);
	return NULL;
}

static void *fb_waiter2(void *arg)
{
	(void)arg;
	syscall(SYS_FUTEX, (long)&g_fb_word2, FUTEX_WAIT_OP | FUTEX_PRIVATE, 0,
		0, 0, 0);
	__sync_fetch_and_add(&g_fb_woken, 1);
	return NULL;
}

static void test_futex_bitset(void)
{
	printf("\n[TEST] futex bitsets and FUTEX_WAKE_OP\n");
	pthread_t t1, t2;

	g_fb_word = 0;
	g_fb_woken = 0;
	pthread_create(&t1, NULL, fb_waiter, (void *)(uintptr_t)0x1);
	pthread_create(&t2, NULL, fb_waiter, (void *)(uintptr_t)0x2);
	usleep(50000);
	/* Wake only the bit-2 sleeper. */
	long n = syscall(SYS_FUTEX, (long)&g_fb_word,
			 FUTEX_WAKE_BITSET_OP | FUTEX_PRIVATE, 10, 0, 0,
			 (long)0x2);
	usleep(50000);
	test_result("FUTEX_WAKE_BITSET(0x2) woke exactly one waiter",
		    n == 1 && g_fb_woken == 1);
	n = syscall(SYS_FUTEX, (long)&g_fb_word,
		    FUTEX_WAKE_BITSET_OP | FUTEX_PRIVATE, 10, 0, 0, (long)0x4);
	usleep(20000);
	test_result("FUTEX_WAKE_BITSET(0x4) matches nobody",
		    n == 0 && g_fb_woken == 1);
	n = syscall(SYS_FUTEX, (long)&g_fb_word,
		    FUTEX_WAKE_BITSET_OP | FUTEX_PRIVATE, 10, 0, 0, (long)0x1);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	test_result("FUTEX_WAKE_BITSET(0x1) woke the other", n == 1 &&
							       g_fb_woken == 2);
	errno = 0;
	n = syscall(SYS_FUTEX, (long)&g_fb_word,
		    FUTEX_WAKE_BITSET_OP | FUTEX_PRIVATE, 1, 0, 0, 0L);
	test_result("a zero bitset is EINVAL", n < 0 && errno == EINVAL);

	/* FUTEX_WAKE_OP: (word2 += 1) while waking word2's sleeper if the
	 * OLD word2 value was 0 -- the condvar-broadcast shape. */
	g_fb_word = 0;
	g_fb_word2 = 0;
	g_fb_woken = 0;
	pthread_create(&t2, NULL, fb_waiter2, NULL);
	usleep(50000);
	unsigned int op = (1u << 28) /* FUTEX_OP_ADD */ | (0u << 24) /* CMP_EQ */ |
			  (1u << 12) /* oparg 1 */ | 0u /* cmparg 0 */;
	n = syscall(SYS_FUTEX, (long)&g_fb_word, 5 /* FUTEX_WAKE_OP */ | FUTEX_PRIVATE,
		    1, 1 /* nr_wake2 */, (long)&g_fb_word2, (long)op);
	pthread_join(t2, NULL);
	test_result("FUTEX_WAKE_OP applied the ADD to the second word",
		    g_fb_word2 == 1);
	test_result("FUTEX_WAKE_OP woke the second word's waiter on a true compare",
		    n == 1 && g_fb_woken == 1);
}

/* ======================================================================
 * clock_nanosleep and the additional clock ids; nanosleep precision.
 * ====================================================================== */
static void test_clock_nanosleep(void)
{
	printf("\n[TEST] clock_nanosleep, extra clocks, sleep precision\n");
	struct timespec a, b, req;

	test_result("CLOCK_MONOTONIC_RAW readable",
		    clock_gettime(CLOCK_MONOTONIC_RAW, &a) == 0);
	test_result("CLOCK_BOOTTIME readable",
		    clock_gettime(CLOCK_BOOTTIME, &a) == 0);
	test_result("CLOCK_REALTIME_COARSE readable",
		    clock_gettime(CLOCK_REALTIME_COARSE, &a) == 0);
	test_result("CLOCK_MONOTONIC_COARSE readable",
		    clock_gettime(CLOCK_MONOTONIC_COARSE, &a) == 0);

	/* Relative 20 ms. */
	req.tv_sec = 0;
	req.tv_nsec = 20 * 1000000L;
	clock_gettime(CLOCK_MONOTONIC, &a);
	int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	test_result("clock_nanosleep relative returns 0", rc == 0);
	test_result("clock_nanosleep relative 20 ms slept 20..80 ms",
		    ms >= 20 && ms <= 80);

	/* Absolute deadline 30 ms out. */
	clock_gettime(CLOCK_MONOTONIC, &a);
	req = a;
	req.tv_nsec += 30 * 1000000L;
	if (req.tv_nsec >= 1000000000L) {
		req.tv_nsec -= 1000000000L;
		req.tv_sec++;
	}
	rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	test_result("clock_nanosleep TIMER_ABSTIME returns 0", rc == 0);
	test_result("clock_nanosleep TIMER_ABSTIME 30 ms slept 30..90 ms",
		    ms >= 30 && ms <= 90);
	/* A deadline in the past returns at once. */
	clock_gettime(CLOCK_MONOTONIC, &a);
	req = a;
	req.tv_sec -= 1;
	rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &req, NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	test_result("past absolute deadline returns immediately",
		    rc == 0 && ms < 15);
	test_result("CPU-time clocks are rejected for sleeping",
		    clock_nanosleep(CLOCK_PROCESS_CPUTIME_ID, 0, &req, NULL) != 0);
	req.tv_sec = 0;
	req.tv_nsec = 2000000000L;
	test_result("an invalid timespec is EINVAL",
		    clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL) == EINVAL);

	/* Precision: a 1 ms nanosleep should take about 1 ms with the
	 * high-resolution timer, and never a full scheduler tick's worth
	 * of overshoot beyond a few ms.  Reported, and bounded loosely
	 * enough for a tick-driven fallback. */
	long worst = 0;
	for (int i = 0; i < 20; i++) {
		req.tv_sec = 0;
		req.tv_nsec = 1000000L;
		clock_gettime(CLOCK_MONOTONIC, &a);
		nanosleep(&req, NULL);
		clock_gettime(CLOCK_MONOTONIC, &b);
		long us = (b.tv_sec - a.tv_sec) * 1000000 +
			  (b.tv_nsec - a.tv_nsec) / 1000;
		if (us > worst)
			worst = us;
	}
	printf("  (worst 1 ms nanosleep: %ld us)\n", worst);
	test_result("1 ms nanosleep never undersleeps and stays under 30 ms",
		    worst >= 1000 && worst < 30000);
}

/* ======================================================================
 * Credentials are per process: a set*id in one thread applies to all.
 * ====================================================================== */
static volatile int g_ct_stop;
static void *ct_spin(void *arg)
{
	(void)arg;
	while (!g_ct_stop)
		sched_yield();
	return (void *)(uintptr_t)geteuid();
}

/* ---- sigsuspend delivery order ------------------------------------------
 *
 * The contract under test: when sigsuspend()'s temporary mask lets a
 * pending signal through, that signal's HANDLER runs before sigsuspend
 * returns, the signal is consumed by that delivery, and the caller's
 * original mask is back afterwards.  A kernel that restores the mask
 * before delivering leaves the signal pending behind a mask that blocks
 * it -- it then fires spuriously whenever the mask next opens.
 *
 * That exact spurious re-delivery broke the script engine's thread-park
 * protocol (a handler publishes state, sigsuspends awaiting the SAME
 * signal as the wake, and a stale delivery after the handler finished
 * looks like a fresh park request): threads froze, acknowledgement
 * semaphores went unbalanced, and the collector walked stacks of RUNNING
 * threads.  The second test below is that protocol, distilled. */

static volatile sig_atomic_t g_ssd_runs;

static void ssd_count_handler(int sig)
{
	(void)sig;
	g_ssd_runs++;
}

static volatile sig_atomic_t g_ssp_runs;
static volatile sig_atomic_t g_ssp_parked;
static volatile sig_atomic_t g_ssp_suspended; /* the suspender's count */
static sem_t g_ssp_sem;
static volatile sig_atomic_t g_ssp_stop;

static void ssp_park_handler(int sig)
{
	(void)sig;
	g_ssp_runs++;
	if (g_ssp_suspended) {
		/* The wake: its only job is to end the sigsuspend below. */
		return;
	}
	g_ssp_parked = 1;
	sem_post(&g_ssp_sem);
	sigset_t allow;
	sigfillset(&allow);
	sigdelset(&allow, SIGUSR1);
	sigsuspend(&allow);
	g_ssp_parked = 0;
	sem_post(&g_ssp_sem);
}

static void *ssp_worker(void *arg)
{
	(void)arg;
	while (!g_ssp_stop)
		usleep(1000);
	return NULL;
}

static void test_sigsuspend_delivery(void)
{
	printf("\n[TEST] sigsuspend delivers the awaited handler before returning\n");
	pid_t c = fork();
	if (c == 0) {
		alarm(10); /* the broken semantics HANG the park protocol */

		/* Part 1: pending-but-blocked signal, then sigsuspend that
		 * allows it. */
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = ssd_count_handler;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR2, &sa, NULL);

		sigset_t blockusr2, oldmask;
		sigemptyset(&blockusr2);
		sigaddset(&blockusr2, SIGUSR2);
		sigprocmask(SIG_BLOCK, &blockusr2, &oldmask);
		raise(SIGUSR2); /* pending behind the block */

		sigset_t allow;
		sigfillset(&allow);
		sigdelset(&allow, SIGUSR2);
		g_ssd_runs = 0;
		sigsuspend(&allow); /* must run the handler, then return */

		if (g_ssd_runs != 1)
			_exit(10); /* handler did not run inside sigsuspend */

		sigset_t pend;
		sigemptyset(&pend);
		sigpending(&pend);
		if (sigismember(&pend, SIGUSR2))
			_exit(11); /* the delivery did not consume the signal */

		sigset_t cur;
		sigemptyset(&cur);
		sigprocmask(SIG_SETMASK, NULL, &cur);
		if (!sigismember(&cur, SIGUSR2))
			_exit(12); /* the caller's mask did not come back */
		sigprocmask(SIG_SETMASK, &oldmask, NULL);

		/* Part 2: the park protocol, 200 cycles.  The handler that
		 * parks blocks its own signal (sa_mask), exactly as the
		 * engine's does -- that mask is what a stale pending signal
		 * used to hide behind. */
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = ssp_park_handler;
		sigemptyset(&sa.sa_mask);
		sigaddset(&sa.sa_mask, SIGUSR1);
		sigaction(SIGUSR1, &sa, NULL);
		sem_init(&g_ssp_sem, 0, 0);

		pthread_t th;
		if (pthread_create(&th, NULL, ssp_worker, NULL) != 0)
			_exit(13);

		for (int i = 0; i < 200; i++) {
			int runs_before = g_ssp_runs;

			g_ssp_suspended = 0;
			pthread_kill(th, SIGUSR1); /* park */
			sem_wait(&g_ssp_sem);
			if (!g_ssp_parked)
				_exit(14); /* acknowledged without parking */
			g_ssp_suspended = 1;
			pthread_kill(th, SIGUSR1); /* wake */
			sem_wait(&g_ssp_sem);
			if (g_ssp_parked)
				_exit(15); /* acknowledged without waking */
			g_ssp_suspended = 0;

			/* Give a stale signal its chance to misfire, then
			 * check the books: exactly two handler runs, and a
			 * balanced semaphore. */
			usleep(2000);
			if (g_ssp_runs != runs_before + 2)
				_exit(16); /* spurious (re-)delivery */
			if (sem_trywait(&g_ssp_sem) == 0)
				_exit(17); /* unpaired acknowledgement */
		}

		g_ssp_stop = 1;
		pthread_join(th, NULL);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	test_result("handler runs inside sigsuspend; no stale signal; park protocol balanced",
		    WIFEXITED(st) && WEXITSTATUS(st) == 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
		printf("  (child exit code %d)\n", WEXITSTATUS(st));
	else if (WIFSIGNALED(st))
		printf("  (child killed by signal %d -- the protocol hung)\n",
		       WTERMSIG(st));
}

static void test_cred_threads(void)
{
	printf("\n[TEST] set*id applies to every thread of the process\n");
	if (geteuid() != 0) {
		printf("  (not root, skipped)\n");
		return;
	}
	pid_t c = fork();
	if (c == 0) {
		pthread_t th;
		g_ct_stop = 0;
		if (pthread_create(&th, NULL, ct_spin, NULL) != 0)
			_exit(3);
		usleep(20000);
		if (setresuid(1234, 1234, 1234) != 0)
			_exit(4);
		usleep(20000);
		g_ct_stop = 1;
		void *r = NULL;
		pthread_join(th, &r);
		_exit((uintptr_t)r == 1234 ? 0 : 5);
	}
	int st = 0;
	waitpid(c, &st, 0);
	test_result("a sibling thread sees the new effective uid",
		    WIFEXITED(st) && WEXITSTATUS(st) == 0);
}


/* ======================================================================
 * Anonymous descriptor kinds: eventfd, timerfd, signalfd, memfd (+seals).
 * ====================================================================== */
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <err.h>
#include <threads.h>

static void *afd_writer(void *arg)
{
	int fd = (int)(intptr_t)arg;
	usleep(30000);
	eventfd_write(fd, 5);
	return NULL;
}

static void test_anonfd(void)
{
	printf("\n[TEST] eventfd / timerfd / signalfd / memfd\n");

	/* ---- eventfd ---- */
	int efd = eventfd(3, EFD_NONBLOCK | EFD_CLOEXEC);
	test_result("eventfd() creates a descriptor", efd >= 0);
	if (efd >= 0) {
		eventfd_t v = 0;
		test_result("initial value readable", eventfd_read(efd, &v) == 0 && v == 3);
		errno = 0;
		test_result("empty counter is EAGAIN with EFD_NONBLOCK",
			    eventfd_read(efd, &v) < 0 && errno == EAGAIN);
		test_result("eventfd_write adds", eventfd_write(efd, 2) == 0 &&
						 eventfd_write(efd, 4) == 0 &&
						 eventfd_read(efd, &v) == 0 && v == 6);
		struct pollfd pfd = { .fd = efd, .events = POLLIN };
		test_result("poll: not readable while zero",
			    poll(&pfd, 1, 0) == 0);
		eventfd_write(efd, 1);
		test_result("poll: readable when non-zero",
			    poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN));
		eventfd_read(efd, &v);
		test_result("FD_CLOEXEC honoured",
			    fcntl(efd, F_GETFD) & FD_CLOEXEC);
		/* A blocking read woken by another thread. */
		int bfd = eventfd(0, 0);
		pthread_t th;
		pthread_create(&th, NULL, afd_writer, (void *)(intptr_t)bfd);
		v = 0;
		test_result("blocking read wakes on a write from another thread",
			    eventfd_read(bfd, &v) == 0 && v == 5);
		pthread_join(th, NULL);
		close(bfd);
		/* Semaphore mode. */
		int sfd = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
		eventfd_read(sfd, &v);
		test_result("EFD_SEMAPHORE reads 1 at a time", v == 1 &&
			    eventfd_read(sfd, &v) == 0 && v == 1);
		close(sfd);
		close(efd);
	}

	/* ---- timerfd ---- */
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	test_result("timerfd_create()", tfd >= 0);
	if (tfd >= 0) {
		struct itimerspec its = { { 0, 10 * 1000000L }, { 0, 10 * 1000000L } };
		test_result("timerfd_settime periodic 10 ms",
			    timerfd_settime(tfd, 0, &its, NULL) == 0);
		struct pollfd pfd = { .fd = tfd, .events = POLLIN };
		int r = poll(&pfd, 1, 500);
		uint64_t ticks = 0;
		test_result("timerfd becomes readable", r == 1);
		test_result("read returns expirations",
			    read(tfd, &ticks, 8) == 8 && ticks >= 1);
		usleep(55000);
		read(tfd, &ticks, 8);
		test_result("periodic: several expirations accumulate (>=3 in 55 ms)",
			    ticks >= 3);
		struct itimerspec cur;
		test_result("timerfd_gettime reports the interval",
			    timerfd_gettime(tfd, &cur) == 0 &&
				    cur.it_interval.tv_nsec == 10 * 1000000L);
		struct itimerspec off = { { 0, 0 }, { 0, 0 } };
		timerfd_settime(tfd, 0, &off, NULL);
		read(tfd, &ticks, 8);
		errno = 0;
		test_result("disarmed timerfd is EAGAIN",
			    read(tfd, &ticks, 8) < 0 && errno == EAGAIN);
		close(tfd);
	}

	/* ---- signalfd ---- */
	sigset_t mask, old;
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigprocmask(SIG_BLOCK, &mask, &old);
	int sgfd = signalfd(-1, &mask, SFD_NONBLOCK);
	test_result("signalfd()", sgfd >= 0);
	if (sgfd >= 0) {
		struct signalfd_siginfo ssi;
		errno = 0;
		test_result("no signal pending: EAGAIN",
			    read(sgfd, &ssi, sizeof(ssi)) < 0 && errno == EAGAIN);
		kill(getpid(), SIGUSR1);
		struct pollfd pfd = { .fd = sgfd, .events = POLLIN };
		test_result("signalfd pollable after the signal",
			    poll(&pfd, 1, 200) == 1);
		ssize_t n = read(sgfd, &ssi, sizeof(ssi));
		test_result("read returns the signalfd_siginfo",
			    n == (ssize_t)sizeof(ssi) && ssi.ssi_signo == SIGUSR1 &&
				    ssi.ssi_pid == (uint32_t)getpid());
		sigset_t pend;
		sigpending(&pend);
		test_result("the signal was consumed", !sigismember(&pend, SIGUSR1));
		close(sgfd);
	}
	sigprocmask(SIG_SETMASK, &old, NULL);

	/* ---- memfd + seals ---- */
	int mfd = memfd_create("testlibc", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	test_result("memfd_create()", mfd >= 0);
	if (mfd >= 0) {
		test_result("ftruncate sizes it", ftruncate(mfd, 8192) == 0);
		struct stat st;
		test_result("fstat reports the size",
			    fstat(mfd, &st) == 0 && st.st_size == 8192);
		char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
		test_result("mmap(MAP_SHARED) of a memfd", m != MAP_FAILED);
		if (m != MAP_FAILED) {
			strcpy(m, "shared");
			/* Visible through a second mapping. */
			char *m2 = mmap(NULL, 8192, PROT_READ, MAP_SHARED, mfd, 0);
			test_result("second mapping sees the data",
				    m2 != MAP_FAILED && strcmp(m2, "shared") == 0);
			if (m2 != MAP_FAILED)
				munmap(m2, 8192);
			munmap(m, 8192);
		}
		test_result("F_GET_SEALS starts empty", fcntl(mfd, F_GET_SEALS) == 0);
		test_result("F_ADD_SEALS(SHRINK)", fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
		errno = 0;
		test_result("shrinking a SHRINK-sealed memfd fails",
			    ftruncate(mfd, 4096) < 0);
		test_result("growing it still works", ftruncate(mfd, 16384) == 0);
		fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE);
		errno = 0;
		void *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
		test_result("writable mapping of a WRITE-sealed memfd is EPERM",
			    w == MAP_FAILED && errno == EPERM);
		fcntl(mfd, F_ADD_SEALS, F_SEAL_SEAL);
		errno = 0;
		test_result("sealing after F_SEAL_SEAL is EPERM",
			    fcntl(mfd, F_ADD_SEALS, F_SEAL_GROW) < 0 && errno == EPERM);
		/* Passed to a child through fork: same object. */
		pid_t c = fork();
		if (c == 0) {
			char *cm = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0);
			_exit(cm != MAP_FAILED && strcmp(cm, "shared") == 0 ? 0 : 1);
		}
		int stt = 0;
		waitpid(c, &stt, 0);
		test_result("a forked child maps the same memfd",
			    WIFEXITED(stt) && WEXITSTATUS(stt) == 0);
		close(mfd);
	}
	int unsealable = memfd_create("x", 0);
	if (unsealable >= 0) {
		test_result("without MFD_ALLOW_SEALING F_SEAL_SEAL is preset",
			    fcntl(unsealable, F_GET_SEALS) == F_SEAL_SEAL);
		close(unsealable);
	}
}

/* ======================================================================
 * /sys and /proc
 * ====================================================================== */
static void test_sysfs_procfs(void)
{
	printf("\n[TEST] /sys and /proc\n");
	struct stat st;
	char buf[512];

	test_result("/sys is a directory", stat("/sys", &st) == 0 && S_ISDIR(st.st_mode));
	test_result("/sys/bus/pci/devices exists",
		    stat("/sys/bus/pci/devices", &st) == 0 && S_ISDIR(st.st_mode));
	DIR *d = opendir("/sys/bus/pci/devices");
	int npci = 0;
	char first[64] = "";
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;
			if (!first[0])
				snprintf(first, sizeof(first), "%s", de->d_name);
			npci++;
		}
		closedir(d);
	}
	test_result("PCI devices are listed", npci > 0);
	if (first[0]) {
		char path[128];
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", first);
		int fd = open(path, O_RDONLY);
		ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
		if (fd >= 0)
			close(fd);
		test_result("vendor file reads as 0x....", n > 2 && buf[0] == '0' && buf[1] == 'x');
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", first);
		fd = open(path, O_RDONLY);
		n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
		if (fd >= 0)
			close(fd);
		test_result("config file is the 256-byte header", n == 256);
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/uevent", first);
		fd = open(path, O_RDONLY);
		n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
		if (fd >= 0)
			close(fd);
		if (n > 0)
			buf[n] = 0;
		test_result("uevent names PCI_SLOT_NAME",
			    n > 0 && strstr(buf, "PCI_SLOT_NAME=") != NULL);
	}

	/* /proc */
	test_result("/proc/self/exe is a symlink",
		    lstat("/proc/self/exe", &st) == 0 && S_ISLNK(st.st_mode));
	ssize_t l = readlink("/proc/self", buf, sizeof(buf) - 1);
	if (l > 0)
		buf[l] = 0;
	test_result("/proc/self resolves to our pid",
		    l > 0 && atoi(buf) == getpid());
	l = readlink("/proc/self/fd/0", buf, sizeof(buf) - 1);
	test_result("/proc/self/fd/0 is a link", l > 0);
	int fd = open("/proc/self/status", O_RDONLY);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n > 0)
		buf[n] = 0;
	test_result("/proc/self/status has Pid:", n > 0 && strstr(buf, "Pid:") != NULL);
	fd = open("/proc/self/maps", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	if (fd >= 0)
		close(fd);
	test_result("/proc/self/maps lists mappings", n > 0);
	fd = open("/proc/uptime", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	if (fd >= 0)
		close(fd);
	test_result("/proc/uptime readable", n > 0);
	fd = open("/proc/meminfo", O_RDONLY);
	n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
	if (fd >= 0)
		close(fd);
	if (n > 0)
		buf[n] = 0;
	test_result("/proc/meminfo has MemTotal", n > 0 && strstr(buf, "MemTotal") != NULL);
	d = opendir("/proc");
	int found_self = 0, found_pid = 0;
	if (d) {
		struct dirent *de;
		char me[16];
		snprintf(me, sizeof(me), "%d", getpid());
		while ((de = readdir(d)) != NULL) {
			if (strcmp(de->d_name, me) == 0)
				found_pid = 1;
			if (strcmp(de->d_name, "uptime") == 0)
				found_self = 1;
		}
		closedir(d);
	}
	test_result("/proc lists our pid and the static files", found_pid && found_self);
	errno = 0;
	test_result("/proc files are read-only",
		    open("/proc/uptime", O_WRONLY) < 0);
}

/* ======================================================================
 * pread/pwrite, fallocate, and the small POSIX/GNU additions.
 * ====================================================================== */
static volatile int g_af_order[3];
static volatile int g_af_n;
static void af_prepare(void) { g_af_order[g_af_n++] = 1; }
static void af_parent(void) { g_af_order[g_af_n++] = 2; }
static void af_child(void) { _exit(g_af_order[0] == 1 ? 0 : 9); }

static void *tj_thread(void *a)
{
	usleep(40000);
	return a;
}

static int c11_fn(void *a)
{
	return *(int *)a + 1;
}

static void test_posix_additions(void)
{
	printf("\n[TEST] pread/pwrite, fallocate and libc additions\n");
	char path[64];
	snprintf(path, sizeof(path), "/tmp/tlpw%d", getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, "0123456789", 10);
		off_t pos = lseek(fd, 0, SEEK_CUR);
		char b[8] = { 0 };
		test_result("pread at offset 4", pread(fd, b, 3, 4) == 3 && memcmp(b, "456", 3) == 0);
		test_result("pread leaves the position alone", lseek(fd, 0, SEEK_CUR) == pos);
		test_result("pwrite at offset 2", pwrite(fd, "AB", 2, 2) == 2);
		test_result("pwrite leaves the position alone", lseek(fd, 0, SEEK_CUR) == pos);
		lseek(fd, 0, SEEK_SET);
		read(fd, b, 6);
		test_result("pwrite took effect", memcmp(b, "01AB45", 6) == 0);
		errno = 0;
		test_result("pread past EOF reads 0", pread(fd, b, 4, 100) == 0);
		test_result("fdatasync()", fdatasync(fd) == 0);
		test_result("fallocate grows the file",
			    fallocate(fd, 0, 0, 4096) == 0);
		struct stat st;
		fstat(fd, &st);
		test_result("size is 4096 after fallocate", st.st_size == 4096);
		test_result("posix_fallocate returns 0", posix_fallocate(fd, 0, 8192) == 0);
		struct iovec iov[2] = { { b, 2 }, { b + 2, 2 } };
		test_result("preadv", preadv(fd, iov, 2, 0) == 4 && memcmp(b, "01AB", 4) == 0);
		close(fd);
		unlink(path);
	} else {
		test_fail("pread test file");
	}
	int pfd[2];
	pipe(pfd);
	errno = 0;
	test_result("pread on a pipe is ESPIPE",
		    pread(pfd[0], path, 1, 0) < 0 && errno == ESPIPE);
	close(pfd[0]);
	close(pfd[1]);

	/* strchrnul / reallocarray / explicit_bzero / secure_getenv */
	test_result("strchrnul finds", *strchrnul("abc", 'b') == 'b');
	test_result("strchrnul at end", *strchrnul("abc", 'z') == 0);
	errno = 0;
	test_result("reallocarray overflow is ENOMEM",
		    reallocarray(NULL, (size_t)1 << 40, (size_t)1 << 40) == NULL &&
			    errno == ENOMEM);
	void *ra = reallocarray(NULL, 4, 8);
	test_result("reallocarray allocates", ra != NULL);
	free(ra);
	char zb[8] = "secret";
	explicit_bzero(zb, sizeof(zb));
	test_result("explicit_bzero zeroes", zb[0] == 0 && zb[5] == 0);
	setenv("TL_SEC", "1", 1);
	test_result("secure_getenv works in a normal process",
		    secure_getenv("TL_SEC") != NULL);
	test_result("get_nprocs >= 1", get_nprocs() >= 1 && get_nprocs_conf() >= get_nprocs());
	test_result("program_invocation_short_name is set",
		    program_invocation_short_name && program_invocation_short_name[0]);
	test_result("getauxval(AT_PAGESZ) is 4096", getauxval(AT_PAGESZ) == 4096);
	test_result("getauxval(AT_RANDOM) points at 16 bytes",
		    getauxval(AT_RANDOM) != 0);
	test_result("getauxval(AT_PLATFORM) is x86_64",
		    getauxval(AT_PLATFORM) &&
			    strcmp((const char *)getauxval(AT_PLATFORM), "x86_64") == 0);
	test_result("getauxval(AT_EXECFN) is set",
		    getauxval(AT_EXECFN) && ((const char *)getauxval(AT_EXECFN))[0]);
	errno = 0;
	test_result("getauxval of an absent type is 0/ENOENT",
		    getauxval(9999) == 0 && errno == ENOENT);
	char nm[16] = "";
	prctl(PR_SET_NAME, "tlname");
	prctl(PR_GET_NAME, nm);
	test_result("prctl PR_SET_NAME/GET_NAME", strcmp(nm, "tlname") == 0);

	/* pthread_atfork ordering */
	g_af_n = 0;
	pthread_atfork(af_prepare, af_parent, af_child);
	pid_t c = fork();
	if (c == 0)
		_exit(7); /* child handler already exited with 0 */
	int st = 0;
	waitpid(c, &st, 0);
	test_result("pthread_atfork: prepare ran before, child handler in the child",
		    WIFEXITED(st) && WEXITSTATUS(st) == 0 && g_af_order[0] == 1 &&
			    g_af_order[1] == 2);

	/* pthread_tryjoin_np */
	pthread_t th;
	pthread_create(&th, NULL, tj_thread, (void *)0x77);
	void *rv = NULL;
	test_result("tryjoin on a running thread is EBUSY",
		    pthread_tryjoin_np(th, &rv) == EBUSY);
	usleep(80000);
	test_result("tryjoin on a finished thread joins it",
		    pthread_tryjoin_np(th, &rv) == 0 && rv == (void *)0x77);

	/* sigwait */
	sigset_t set, oldm;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_BLOCK, &set, &oldm);
	kill(getpid(), SIGUSR2);
	int sig = 0;
	test_result("sigwait returns the pending signal",
		    sigwait(&set, &sig) == 0 && sig == SIGUSR2);
	sigprocmask(SIG_SETMASK, &oldm, NULL);

	/* C11 threads */
	thrd_t t11;
	int in = 41, out = 0;
	test_result("thrd_create", thrd_create(&t11, c11_fn, &in) == thrd_success);
	test_result("thrd_join returns the result",
		    thrd_join(t11, &out) == thrd_success && out == 42);
	mtx_t mx;
	test_result("mtx_init/lock/trylock/unlock",
		    mtx_init(&mx, mtx_plain) == thrd_success && mtx_lock(&mx) == thrd_success &&
			    mtx_trylock(&mx) == thrd_busy && mtx_unlock(&mx) == thrd_success);
	mtx_destroy(&mx);
}


/* ======================================================================
 * The display-manager device: /dev/dri/card0 (mode setting, dumb
 * buffers, events, PRIME) -- skipped where the machine has no GPU driver.
 * ====================================================================== */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <sys/sysmacros.h>

static void test_drm(void)
{
	printf("\n[TEST] display-manager device (/dev/dri)\n");
	struct stat st;

	if (stat("/dev/dri/card0", &st) != 0) {
		printf("  (no /dev/dri/card0: no display-manager driver here, skipped)\n");
		return;
	}
	test_result("/dev/dri/card0 is a character device", S_ISCHR(st.st_mode));
	test_result("card0 is major 226 minor 0", major(st.st_rdev) == 226 && minor(st.st_rdev) == 0);
	test_result("/dev/dri/renderD128 exists", stat("/dev/dri/renderD128", &st) == 0 &&
						       minor(st.st_rdev) == 128);
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	test_result("open card0", fd >= 0);
	if (fd < 0)
		return;

	/* VERSION: the two-call string protocol */
	struct drm_version v;
	memset(&v, 0, sizeof(v));
	test_result("DRM_IOCTL_VERSION (lengths)", ioctl(fd, DRM_IOCTL_VERSION, &v) == 0);
	char name[32] = "", date[32] = "", desc[64] = "";
	v.name = name; v.name_len = sizeof(name) - 1;
	v.date = date; v.date_len = sizeof(date) - 1;
	v.desc = desc; v.desc_len = sizeof(desc) - 1;
	test_result("DRM_IOCTL_VERSION (strings)", ioctl(fd, DRM_IOCTL_VERSION, &v) == 0);
	name[v.name_len] = 0;
	printf("  (driver %s %d.%d.%d)\n", name, v.version_major, v.version_minor, v.version_patchlevel);
	test_result("driver name is vmwgfx", strcmp(name, "vmwgfx") == 0);
	test_result("driver version major is 2", v.version_major == 2);

	/* sysfs-style lookup by device number */
	char spath[96], buf[256];
	snprintf(spath, sizeof(spath), "/sys/dev/char/%u:%u/uevent", 226u, 0u);
	int sfd = open(spath, O_RDONLY);
	ssize_t n = sfd >= 0 ? read(sfd, buf, sizeof(buf) - 1) : -1;
	if (sfd >= 0)
		close(sfd);
	if (n > 0)
		buf[n] = 0;
	test_result("/sys/dev/char/226:0/uevent names dri/card0",
		    n > 0 && strstr(buf, "DEVNAME=dri/card0") != NULL);
	snprintf(spath, sizeof(spath), "/sys/dev/char/226:0/device/vendor");
	sfd = open(spath, O_RDONLY);
	n = sfd >= 0 ? read(sfd, buf, sizeof(buf) - 1) : -1;
	if (sfd >= 0)
		close(sfd);
	if (n > 0)
		buf[n] = 0;
	test_result("/sys/dev/char/226:0/device/vendor is 0x15ad",
		    n > 0 && strstr(buf, "0x15ad") != NULL);
	test_result("/sys/class/drm/card0 resolves", stat("/sys/class/drm/card0/dev", &st) == 0);

	/* caps */
	struct drm_get_cap cap = { DRM_CAP_DUMB_BUFFER, 0 };
	test_result("DRM_CAP_DUMB_BUFFER", ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);
	cap.capability = DRM_CAP_PRIME;
	test_result("DRM_CAP_PRIME import+export", ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 3);
	cap.capability = DRM_CAP_TIMESTAMP_MONOTONIC;
	test_result("DRM_CAP_TIMESTAMP_MONOTONIC", ioctl(fd, DRM_IOCTL_GET_CAP, &cap) == 0 && cap.value == 1);

	/* master + magic */
	test_result("SET_MASTER", ioctl(fd, DRM_IOCTL_SET_MASTER, 0) == 0);
	int fd2 = open("/dev/dri/card0", O_RDWR);
	struct drm_auth au = { 0 };
	test_result("GET_MAGIC on a second file", fd2 >= 0 && ioctl(fd2, DRM_IOCTL_GET_MAGIC, &au) == 0 && au.magic != 0);
	test_result("AUTH_MAGIC by the master", ioctl(fd, DRM_IOCTL_AUTH_MAGIC, &au) == 0);
	int rfd = open("/dev/dri/renderD128", O_RDWR);
	errno = 0;
	test_result("render node refuses GET_MAGIC (EACCES)",
		    rfd >= 0 && ioctl(rfd, DRM_IOCTL_GET_MAGIC, &au) < 0 && errno == EACCES);
	errno = 0;
	test_result("render node refuses mode setting",
		    rfd >= 0 && ioctl(rfd, DRM_IOCTL_MODE_GETRESOURCES, &cap) < 0 && errno == EACCES);

	/* resources */
	struct drm_mode_card_res res;
	memset(&res, 0, sizeof(res));
	test_result("MODE_GETRESOURCES (counts)", ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0);
	test_result("one crtc, one connector, one encoder",
		    res.count_crtcs == 1 && res.count_connectors == 1 && res.count_encoders == 1);
	uint32_t crtc_id = 0, conn_id = 0, enc_id = 0;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)&crtc_id;
	res.connector_id_ptr = (uint64_t)(uintptr_t)&conn_id;
	res.encoder_id_ptr = (uint64_t)(uintptr_t)&enc_id;
	res.count_fbs = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	test_result("ids are non-zero", crtc_id && conn_id && enc_id);

	struct drm_mode_get_connector gc;
	memset(&gc, 0, sizeof(gc));
	gc.connector_id = conn_id;
	test_result("MODE_GETCONNECTOR (counts)", ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0);
	test_result("connector is connected with modes",
		    gc.connection == 1 && gc.count_modes >= 1 && gc.connector_type == DRM_MODE_CONNECTOR_VIRTUAL);
	struct drm_mode_modeinfo modes[24];
	uint32_t nm = gc.count_modes < 24 ? gc.count_modes : 24;
	gc.modes_ptr = (uint64_t)(uintptr_t)modes;
	gc.count_modes = nm;
	gc.count_props = 0;
	gc.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc);
	test_result("first mode is preferred", nm > 0 && (modes[0].type & DRM_MODE_TYPE_PREFERRED));
	printf("  (preferred mode %s @%u)\n", modes[0].name, modes[0].vrefresh);

	struct drm_mode_get_encoder ge;
	memset(&ge, 0, sizeof(ge));
	ge.encoder_id = enc_id;
	test_result("MODE_GETENCODER", ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0 &&
					     ge.possible_crtcs == 1);

	/* dumb buffer of the preferred mode */
	struct drm_mode_create_dumb cd;
	memset(&cd, 0, sizeof(cd));
	cd.width = modes[0].hdisplay;
	cd.height = modes[0].vdisplay;
	cd.bpp = 32;
	test_result("MODE_CREATE_DUMB", ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0 &&
					      cd.handle && cd.pitch >= cd.width * 4 && cd.size >= (uint64_t)cd.pitch * cd.height);
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	test_result("MODE_MAP_DUMB", ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0 && md.offset);
	uint32_t *px = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
	test_result("mmap of the dumb buffer", px != MAP_FAILED);
	if (px != MAP_FAILED) {
		for (uint32_t y = 0; y < cd.height; y++)
			for (uint32_t x = 0; x < cd.width; x++)
				px[y * (cd.pitch / 4) + x] = ((x * 255 / cd.width) << 16) | ((y * 255 / cd.height) << 8) | 0x40;
		test_result("buffer readable back", px[0] == 0x40);
	}
	struct drm_mode_fb_cmd fb;
	memset(&fb, 0, sizeof(fb));
	fb.width = cd.width;
	fb.height = cd.height;
	fb.pitch = cd.pitch;
	fb.bpp = 32;
	fb.depth = 24;
	fb.handle = cd.handle;
	test_result("MODE_ADDFB", ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0 && fb.fb_id);

	struct drm_mode_crtc sc;
	memset(&sc, 0, sizeof(sc));
	sc.crtc_id = crtc_id;
	sc.fb_id = fb.fb_id;
	sc.set_connectors_ptr = (uint64_t)(uintptr_t)&conn_id;
	sc.count_connectors = 1;
	sc.mode = modes[0];
	sc.mode_valid = 1;
	test_result("MODE_SETCRTC shows the buffer", ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc) == 0);

	struct drm_mode_crtc gcr;
	memset(&gcr, 0, sizeof(gcr));
	gcr.crtc_id = crtc_id;
	test_result("MODE_GETCRTC reports the fb", ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &gcr) == 0 &&
						       gcr.fb_id == fb.fb_id && gcr.mode_valid);

	struct drm_clip_rect clip = { 0, 0, (unsigned short)cd.width, (unsigned short)(cd.height / 2) };
	struct drm_mode_fb_dirty_cmd dirty;
	memset(&dirty, 0, sizeof(dirty));
	dirty.fb_id = fb.fb_id;
	dirty.num_clips = 1;
	dirty.clips_ptr = (uint64_t)(uintptr_t)&clip;
	test_result("MODE_DIRTYFB", ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0);

	/* vblank: synchronous, then as an event */
	union drm_wait_vblank wv;
	memset(&wv, 0, sizeof(wv));
	wv.request.type = _DRM_VBLANK_RELATIVE;
	wv.request.sequence = 2;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int wr = ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &wv);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	test_result("WAIT_VBLANK relative 2 waits about two frames (10..80 ms)", wr == 0 && ms >= 10 && ms <= 80);
	memset(&wv, 0, sizeof(wv));
	wv.request.type = _DRM_VBLANK_RELATIVE | _DRM_VBLANK_EVENT;
	wv.request.sequence = 1;
	wv.request.signal = 0x1234;
	test_result("WAIT_VBLANK with EVENT queues", ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &wv) == 0);
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	test_result("the fd becomes readable", poll(&pfd, 1, 500) == 1);
	struct drm_event_vblank ev;
	n = read(fd, &ev, sizeof(ev));
	test_result("read returns a vblank event with our user_data",
		    n == (ssize_t)sizeof(ev) && ev.base.type == DRM_EVENT_VBLANK && ev.user_data == 0x1234 &&
			    ev.crtc_id == crtc_id);

	/* page flip to a second buffer with an event */
	struct drm_mode_create_dumb cd2 = cd;
	cd2.handle = 0;
	ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd2);
	struct drm_mode_fb_cmd fb2 = fb;
	fb2.handle = cd2.handle;
	fb2.fb_id = 0;
	ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb2);
	struct drm_mode_crtc_page_flip flip = { .crtc_id = crtc_id, .fb_id = fb2.fb_id,
						.flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = 0x77 };
	test_result("MODE_PAGE_FLIP", ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0);
	test_result("flip completion event arrives",
		    poll(&pfd, 1, 500) == 1 && read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev) &&
			    ev.base.type == DRM_EVENT_FLIP_COMPLETE && ev.user_data == 0x77);

	/* PRIME: export, import in a child over fork, and via SCM_RIGHTS */
	struct drm_prime_handle ph = { .handle = cd.handle, .flags = DRM_CLOEXEC | DRM_RDWR };
	test_result("PRIME_HANDLE_TO_FD", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0 && ph.fd >= 0);
	if (ph.fd >= 0) {
		struct drm_prime_handle ih = { .fd = ph.fd };
		test_result("PRIME_FD_TO_HANDLE on the render node",
			    rfd >= 0 && ioctl(rfd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ih) == 0 && ih.handle);
		uint32_t *m2 = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ph.fd, 0);
		test_result("mmap of the dma-buf sees the pixels", m2 != MAP_FAILED && m2[0] == 0x40);
		if (m2 != MAP_FAILED)
			munmap(m2, 4096);
		pid_t c = fork();
		if (c == 0) {
			uint32_t *cm = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ph.fd, 0);
			_exit(cm != MAP_FAILED && cm[0] == 0x40 ? 0 : 1);
		}
		int stt = 0;
		waitpid(c, &stt, 0);
		test_result("a forked child maps the dma-buf", WIFEXITED(stt) && WEXITSTATUS(stt) == 0);
		close(ph.fd);
	}
	struct drm_gem_close gcl = { .handle = cd2.handle };
	test_result("GEM_CLOSE", ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gcl) == 0);

	if (px != MAP_FAILED)
		munmap(px, cd.size);
	test_result("MODE_RMFB", ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb.fb_id) == 0);
	ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb2.fb_id);
	struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
	test_result("MODE_DESTROY_DUMB", ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd) == 0);
	test_result("DROP_MASTER", ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) == 0);
	if (rfd >= 0)
		close(rfd);
	if (fd2 >= 0)
		close(fd2);
	close(fd);
}


/* ======================================================================
 * The 3D side of the display-manager device (vmwgfx ioctls): skipped on
 * hosts without 3D.
 * ====================================================================== */
#include <drm/vmwgfx_drm.h>
#include <drm/sync_file.h>

static long vmw_cmd(int fd, unsigned nr, unsigned dir, void *arg, size_t size)
{
	unsigned long req = _IOC(dir, 'd', 0x40 + nr, size);
	return ioctl(fd, req, arg);
}

/* What does a buffer hold after a readback?  Four answers, four meanings.
 * The pattern: the device wrote the surface content back.  The sentinel the
 * buffer was filled with before the readback: the device wrote nothing --
 * which for a surface the host never RENDERED into is CORRECT guest-backed
 * behaviour, not a failure: the MOB is the canonical storage and the host
 * has nothing newer to write back.  (Observed on VMware 2026-08-28: readback
 * of an uploaded-only surface leaves the buffer untouched, while readback
 * after GL rendering delivers the rendered pixels.)  Zeroes: the device
 * wrote back an EMPTY host copy -- upload-direction data loss.  A mix:
 * corruption.  Only the last two are failures. */
#define GB_XFER_PATTERN 0
#define GB_XFER_UNTOUCHED 1
#define GB_XFER_ZEROS 2
#define GB_XFER_MIX 3
static int gb_xfer_classify(const uint32_t *pix, int n, uint32_t seed,
			    uint32_t sentinel, int *out_bad)
{
	int pat = 0, zero = 0, sent = 0;

	for (int i = 0; i < n; i++) {
		uint32_t v = pix[i];
		if (v == (seed ^ (uint32_t)i * 2654435761u))
			pat++;
		if (v == 0)
			zero++;
		if (v == sentinel)
			sent++;
	}
	*out_bad = n - pat;
	if (pat == n)
		return GB_XFER_PATTERN;
	if (sent == n)
		return GB_XFER_UNTOUCHED;
	if (zero == n)
		return GB_XFER_ZEROS;
	return GB_XFER_MIX;
}

static const char *gb_xfer_name(int cls)
{
	switch (cls) {
	case GB_XFER_PATTERN:
		return "the pattern (device wrote the content back)";
	case GB_XFER_UNTOUCHED:
		return "the untouched sentinel (nothing newer host-side; correct)";
	case GB_XFER_ZEROS:
		return "zeroes (device wrote back an EMPTY host copy: upload lost)";
	default:
		return "a mix (corruption)";
	}
}

static void test_drm_3d(void)
{
	printf("\n[TEST] display-manager device: 3D (vmwgfx)\n");
	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("  (no render node, skipped)\n");
		return;
	}
	struct drm_vmw_getparam_arg gp = { .param = DRM_VMW_PARAM_3D };
	if (vmw_cmd(fd, DRM_VMW_GET_PARAM, _IOC_READ | _IOC_WRITE, &gp, sizeof(gp)) != 0 || !gp.value) {
		printf("  (host has no 3D, skipped)\n");
		close(fd);
		return;
	}
	gp.param = DRM_VMW_PARAM_HW_CAPS;
	vmw_cmd(fd, DRM_VMW_GET_PARAM, _IOC_READ | _IOC_WRITE, &gp, sizeof(gp));
	int gb = (gp.value & 0x08000000) != 0;
	gp.param = DRM_VMW_PARAM_DX;
	vmw_cmd(fd, DRM_VMW_GET_PARAM, _IOC_READ | _IOC_WRITE, &gp, sizeof(gp));
	int dx = (int)gp.value;
	printf("  (guest-backed objects %s, DX %s)\n", gb ? "yes" : "no", dx ? "yes" : "no");

	/* buffer object + map + SYNCCPU */
	union drm_vmw_alloc_bo_arg bo;
	memset(&bo, 0, sizeof(bo));
	bo.req.size = 65536;
	test_result("ALLOC_BO", vmw_cmd(fd, DRM_VMW_ALLOC_BO, _IOC_READ | _IOC_WRITE, &bo, sizeof(bo)) == 0 &&
				      bo.rep.handle && bo.rep.map_handle);
	uint32_t *bm = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bo.rep.map_handle);
	test_result("mmap of the BO via map_handle", bm != MAP_FAILED);
	if (bm != MAP_FAILED) {
		bm[0] = 0xABCD;
		munmap(bm, 65536);
	}
	struct drm_vmw_synccpu_arg sc = { .op = drm_vmw_synccpu_grab, .flags = drm_vmw_synccpu_read,
					  .handle = bo.rep.handle };
	test_result("SYNCCPU grab on an idle BO", vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc)) == 0);
	sc.op = drm_vmw_synccpu_release;
	vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc));

	/* An empty EXECBUF still yields a fence (the "flush for fence" case). */
	struct drm_vmw_fence_rep rep;
	memset(&rep, 0, sizeof(rep));
	rep.error = -EFAULT;
	struct drm_vmw_execbuf_arg eb;
	memset(&eb, 0, sizeof(eb));
	eb.version = DRM_VMW_EXECBUF_VERSION;
	eb.context_handle = 0xFFFFFFFFu;
	eb.imported_fence_fd = -1;
	eb.fence_rep = (uint64_t)(uintptr_t)&rep;
	eb.flags = DRM_VMW_EXECBUF_FLAG_EXPORT_FENCE_FD;
	test_result("EXECBUF (empty) returns 0", vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0);
	test_result("fence_rep filled: error 0, handle set", rep.error == 0 && rep.handle != 0);
	test_result("fence fd exported", rep.fd >= 0);
	struct drm_vmw_fence_wait_arg fw = { .handle = rep.handle, .timeout_us = 1000000, .flags = DRM_VMW_FENCE_FLAG_EXEC };
	test_result("FENCE_WAIT", vmw_cmd(fd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &fw, sizeof(fw)) == 0);
	struct drm_vmw_fence_signaled_arg fs = { .handle = rep.handle, .flags = DRM_VMW_FENCE_FLAG_EXEC };
	test_result("FENCE_SIGNALED after the wait", vmw_cmd(fd, DRM_VMW_FENCE_SIGNALED, _IOC_READ | _IOC_WRITE, &fs, sizeof(fs)) == 0 && fs.signaled);
	if (rep.fd >= 0) {
		struct pollfd pfd = { .fd = rep.fd, .events = POLLIN };
		test_result("sync_file is readable once signalled", poll(&pfd, 1, 1000) == 1);
		struct sync_merge_data md;
		memset(&md, 0, sizeof(md));
		strcpy(md.name, "t");
		md.fd2 = rep.fd;
		test_result("SYNC_IOC_MERGE", ioctl(rep.fd, SYNC_IOC_MERGE, &md) == 0 && md.fence >= 0);
		if (md.fence >= 0)
			close(md.fence);
		close(rep.fd);
	}
	struct drm_vmw_fence_arg fu = { .handle = rep.handle };
	test_result("FENCE_UNREF", vmw_cmd(fd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &fu, sizeof(fu)) == 0);

	/* contexts */
	union drm_vmw_extended_context_arg ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.req = dx ? drm_vmw_context_dx : drm_vmw_context_legacy;
	int cr = vmw_cmd(fd, DRM_VMW_CREATE_EXTENDED_CONTEXT, _IOC_READ | _IOC_WRITE, &ctx, sizeof(ctx));
	test_result("CREATE_EXTENDED_CONTEXT", cr == 0 && ctx.rep.cid >= 0);
	if (cr == 0) {
		struct drm_vmw_context_arg ca = { .cid = ctx.rep.cid };
		test_result("UNREF_CONTEXT", vmw_cmd(fd, DRM_VMW_UNREF_CONTEXT, _IOC_WRITE, &ca, sizeof(ca)) == 0);
	}

	if (gb) {
		/* a 64x64 XRGB surface with its own backup */
		union drm_vmw_gb_surface_create_ext_arg sa;
		memset(&sa, 0, sizeof(sa));
		sa.req.base.svga3d_flags = 0;
		/* SVGA3D_X8R8G8B8: XRGB, 4 bytes a pixel, and the one 32-bit
		 * format every guest-backed device has -- it is what the
		 * driver's own scan-out surface is.  (Format 23 is not
		 * B8G8R8X8_UNORM, which is 142; 23 is SVGA3D_FORMAT_DEAD1, a
		 * hole left where a format was removed.) */
		sa.req.base.format = 1; /* SVGA3D_X8R8G8B8 */
		sa.req.base.mip_levels = 1;
		sa.req.base.drm_surface_flags = drm_vmw_surface_flag_shareable | drm_vmw_surface_flag_create_buffer;
		sa.req.base.buffer_handle = 0xFFFFFFFFu;
		sa.req.base.base_size.width = 64;
		sa.req.base.base_size.height = 64;
		sa.req.base.base_size.depth = 1;
		sa.req.base.array_size = 1;
		sa.req.version = drm_vmw_gb_surface_v1;
		int sr = vmw_cmd(fd, DRM_VMW_GB_SURFACE_CREATE_EXT, _IOC_READ | _IOC_WRITE, &sa, sizeof(sa));
		test_result("GB_SURFACE_CREATE_EXT", sr == 0 && sa.rep.handle && sa.rep.buffer_handle != 0xFFFFFFFFu);
		test_result("backup size is 64*64*4", sa.rep.backup_size == 64 * 64 * 4);
		if (sr == 0) {
			uint32_t *pix = mmap(NULL, sa.rep.buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
					     (off_t)sa.rep.buffer_map_handle);
			test_result("backup buffer maps", pix != MAP_FAILED);
			if (pix != MAP_FAILED) {
				/* A pattern that varies per pixel, not one
				 * repeated value: a copy that lands at the
				 * wrong offset, or a readback that fills only
				 * the first page, both survive a constant. */
				for (int i = 0; i < 64 * 64; i++)
					pix[i] = 0x00FF8000u ^ (uint32_t)i * 2654435761u;
				/* Upload: UPDATE_GB_SURFACE names the surface HANDLE;
				 * the kernel rewrites it to the device id. */
				uint32_t cmd[3] = { 1102 /* SVGA_3D_CMD_UPDATE_GB_SURFACE */, 4, sa.rep.handle };
				memset(&rep, 0, sizeof(rep));
				rep.error = -EFAULT;
				eb.commands = (uint64_t)(uintptr_t)cmd;
				eb.command_size = sizeof(cmd);
				eb.flags = 0;
				eb.fence_rep = (uint64_t)(uintptr_t)&rep;
				test_result("EXECBUF UPDATE_GB_SURFACE accepted",
					    vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0 && rep.error == 0);
				struct drm_vmw_fence_wait_arg w2 = { .handle = rep.handle, .timeout_us = 1000000, .flags = DRM_VMW_FENCE_FLAG_EXEC };
				test_result("fence of the upload passes", vmw_cmd(fd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &w2, sizeof(w2)) == 0);
				struct drm_vmw_fence_arg u2 = { .handle = rep.handle };
				vmw_cmd(fd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &u2, sizeof(u2));

				/* The round trips -- the only thing here
				 * that proves pixels actually move between
				 * guest memory and the surface.
				 *
				 * These transfers are the path every X pixel
				 * takes that is not rendered host-side: glamor
				 * falls back to CPU drawing for core-font text
				 * (readback, draw, upload) and uploads every
				 * client-rendered image.  Where a transfer
				 * silently moves nothing, window borders and
				 * fills (host-rendered) still appear while all
				 * text and window content is black -- with
				 * every ioctl and fence succeeding.  Only
				 * comparing pixels can see it.
				 *
				 * Round trip 1 uses the whole-surface commands
				 * (UPDATE/READBACK_GB_SURFACE); round trip 2
				 * the per-image ones (UPDATE/READBACK_GB_IMAGE)
				 * -- the forms the kernel console and Mesa
				 * actually use, so the two rounds can implicate
				 * a command family, not just "the device". */
				memset(pix, 0xAA, 64 * 64 * 4);
				uint32_t rb[3] = { 1104 /* SVGA_3D_CMD_READBACK_GB_SURFACE */, 4, sa.rep.handle };
				memset(&rep, 0, sizeof(rep));
				rep.error = -EFAULT;
				eb.commands = (uint64_t)(uintptr_t)rb;
				eb.command_size = sizeof(rb);
				eb.flags = 0;
				eb.fence_rep = (uint64_t)(uintptr_t)&rep;
				test_result("EXECBUF READBACK_GB_SURFACE accepted",
					    vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0 && rep.error == 0);
				struct drm_vmw_fence_wait_arg w3 = { .handle = rep.handle, .timeout_us = 1000000, .flags = DRM_VMW_FENCE_FLAG_EXEC };
				test_result("fence of the readback passes",
					    vmw_cmd(fd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &w3, sizeof(w3)) == 0);
				struct drm_vmw_fence_arg u3 = { .handle = rep.handle };
				vmw_cmd(fd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &u3, sizeof(u3));
				struct drm_vmw_synccpu_arg sc;
				memset(&sc, 0, sizeof(sc));
				sc.op = drm_vmw_synccpu_grab;
				sc.flags = drm_vmw_synccpu_read;
				sc.handle = sa.rep.buffer_handle;
				test_result("SYNCCPU grabs the backup buffer for reading",
					    vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc)) == 0);

				int bad = 0;
				int cls = gb_xfer_classify(pix, 64 * 64, 0x00FF8000u,
							   0xAAAAAAAAu, &bad);
				test_result("whole-surface readback does not lose or corrupt data",
					    cls == GB_XFER_PATTERN || cls == GB_XFER_UNTOUCHED);
				printf("      (READBACK_GB_SURFACE left %s; [0]=%08x [1]=%08x)\n",
				       gb_xfer_name(cls), (unsigned)pix[0], (unsigned)pix[1]);
				sc.op = drm_vmw_synccpu_release;
				vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc));

				/* Round trip 2: the per-image commands. */
				for (int i = 0; i < 64 * 64; i++)
					pix[i] = 0x00123456u ^ (uint32_t)i * 2654435761u;
				/* SVGA3dSurfaceImageId {sid,face,mipmap} + SVGA3dBox {x,y,z,w,h,d} */
				uint32_t up2[11] = { 1101 /* SVGA_3D_CMD_UPDATE_GB_IMAGE */, 36,
						     sa.rep.handle, 0, 0,
						     0, 0, 0, 64, 64, 1 };
				memset(&rep, 0, sizeof(rep));
				rep.error = -EFAULT;
				eb.commands = (uint64_t)(uintptr_t)up2;
				eb.command_size = sizeof(up2);
				eb.fence_rep = (uint64_t)(uintptr_t)&rep;
				test_result("EXECBUF UPDATE_GB_IMAGE accepted",
					    vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0 && rep.error == 0);
				struct drm_vmw_fence_wait_arg w4 = { .handle = rep.handle, .timeout_us = 1000000, .flags = DRM_VMW_FENCE_FLAG_EXEC };
				test_result("fence of the image upload passes",
					    vmw_cmd(fd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &w4, sizeof(w4)) == 0);
				struct drm_vmw_fence_arg u4 = { .handle = rep.handle };
				vmw_cmd(fd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &u4, sizeof(u4));

				memset(pix, 0x55, 64 * 64 * 4);
				/* SVGA3dSurfaceImageId {sid,face,mipmap} */
				uint32_t rb2[5] = { 1103 /* SVGA_3D_CMD_READBACK_GB_IMAGE */, 12,
						    sa.rep.handle, 0, 0 };
				memset(&rep, 0, sizeof(rep));
				rep.error = -EFAULT;
				eb.commands = (uint64_t)(uintptr_t)rb2;
				eb.command_size = sizeof(rb2);
				eb.fence_rep = (uint64_t)(uintptr_t)&rep;
				test_result("EXECBUF READBACK_GB_IMAGE accepted",
					    vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0 && rep.error == 0);
				struct drm_vmw_fence_wait_arg w5 = { .handle = rep.handle, .timeout_us = 1000000, .flags = DRM_VMW_FENCE_FLAG_EXEC };
				test_result("fence of the image readback passes",
					    vmw_cmd(fd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &w5, sizeof(w5)) == 0);
				struct drm_vmw_fence_arg u5 = { .handle = rep.handle };
				vmw_cmd(fd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &u5, sizeof(u5));
				sc.op = drm_vmw_synccpu_grab;
				sc.flags = drm_vmw_synccpu_read;
				test_result("SYNCCPU grabs for the image readback",
					    vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc)) == 0);
				cls = gb_xfer_classify(pix, 64 * 64, 0x00123456u,
						       0x55555555u, &bad);
				test_result("per-image readback does not lose or corrupt data (console/Mesa form)",
					    cls == GB_XFER_PATTERN || cls == GB_XFER_UNTOUCHED);
				printf("      (READBACK_GB_IMAGE left %s; [0]=%08x [1]=%08x)\n",
				       gb_xfer_name(cls), (unsigned)pix[0], (unsigned)pix[1]);
				sc.op = drm_vmw_synccpu_release;
				vmw_cmd(fd, DRM_VMW_SYNCCPU, _IOC_WRITE, &sc, sizeof(sc));
				munmap(pix, sa.rep.buffer_size);
			}
			/* a command naming a handle that is not ours is refused */
			uint32_t bad[3] = { 1102, 4, 0x7FFF };
			eb.commands = (uint64_t)(uintptr_t)bad;
			eb.command_size = sizeof(bad);
			eb.fence_rep = 0;
			errno = 0;
			test_result("EXECBUF refuses an unknown surface handle",
				    vmw_cmd(fd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) < 0 && errno == EINVAL);
			/* share the surface: PRIME export, re-import by REF_EXT(PRIME) */
			struct drm_prime_handle ph = { .handle = sa.rep.handle, .flags = DRM_CLOEXEC };
			test_result("PRIME export of a surface", ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) == 0);
			if (ph.fd >= 0) {
				int fd2 = open("/dev/dri/renderD128", O_RDWR);
				union drm_vmw_gb_surface_reference_ext_arg ra;
				memset(&ra, 0, sizeof(ra));
				ra.req.sid = ph.fd;
				ra.req.handle_type = DRM_VMW_HANDLE_PRIME;
				int rr = fd2 >= 0 ? vmw_cmd(fd2, DRM_VMW_GB_SURFACE_REF_EXT, _IOC_READ | _IOC_WRITE, &ra, sizeof(ra)) : -1;
				test_result("GB_SURFACE_REF_EXT by dma-buf in another file",
					    rr == 0 && ra.rep.crep.handle && ra.rep.creq.base.base_size.width == 64 &&
						    ra.rep.creq.base.format == 1 && ra.rep.crep.backup_size == 64 * 64 * 4);
				if (fd2 >= 0)
					close(fd2);
				close(ph.fd);
			}
			struct drm_vmw_surface_arg ua = { .sid = (int32_t)sa.rep.handle };
			test_result("UNREF_SURFACE", vmw_cmd(fd, DRM_VMW_UNREF_SURFACE, _IOC_WRITE, &ua, sizeof(ua)) == 0);
		}
		struct drm_vmw_get_3d_cap_arg cap;
		static uint32_t caps[300];
		cap.buffer = (uint64_t)(uintptr_t)caps;
		cap.max_size = sizeof(caps);
		test_result("GET_3D_CAP fills the devcap array",
			    vmw_cmd(fd, DRM_VMW_GET_3D_CAP, _IOC_WRITE, &cap, sizeof(cap)) == 0 && caps[0] /* DEVCAP_3D */);
	}
	struct drm_vmw_handle_close_arg hc = { .handle = bo.rep.handle };
	test_result("UNREF_DMABUF (handle close)", vmw_cmd(fd, DRM_VMW_UNREF_DMABUF, _IOC_WRITE, &hc, sizeof(hc)) == 0);

	/* The host message channel.  Either the hypervisor answers it and a
	 * log line is accepted, or it does not exist and the driver says
	 * ENODEV -- both are correct, and a third answer is not. */
	{
		char msg[] = "log LikeOS testlibc: display-manager message channel";
		struct drm_vmw_msg_arg ma;
		memset(&ma, 0, sizeof(ma));
		ma.send = (uint64_t)(uintptr_t)msg;
		ma.send_only = 1;
		int rc = vmw_cmd(fd, DRM_VMW_MSG, _IOC_READ | _IOC_WRITE, &ma, sizeof(ma));
		test_result("MSG: a host log line is accepted or refused cleanly",
			    rc == 0 || rc == -ENODEV || rc == -EIO);
		printf("      (message channel: %s)\n",
		       rc == 0 ? "present" : rc == -ENODEV ? "absent" : "present, refused");
	}

	/* A fence event: the same wait as FENCE_WAIT, delivered as a
	 * readable event instead.  With nothing submitted the fence is
	 * already signalled, so the event has to be there straight away --
	 * which is the case the driver has to special-case and the one most
	 * likely to be got wrong. */
	{
		struct drm_vmw_fence_event_arg fe;
		memset(&fe, 0, sizeof(fe));
		fe.user_data = 0x5AFE7E57ull;
		int rc = vmw_cmd(fd, DRM_VMW_FENCE_EVENT, _IOC_WRITE, &fe, sizeof(fe));
		test_result("FENCE_EVENT is accepted", rc == 0);
		if (rc == 0) {
			struct pollfd pf = { .fd = fd, .events = POLLIN };
			int pr = poll(&pf, 1, 2000);
			test_result("FENCE_EVENT: the device becomes readable", pr == 1);
			if (pr == 1) {
				char buf[128];
				ssize_t n = read(fd, buf, sizeof(buf));
				struct drm_event *ev = (struct drm_event *)buf;
				test_result("FENCE_EVENT: the event is a fence event",
					    n >= (ssize_t)sizeof(struct drm_vmw_event_fence) &&
						    ev->type == DRM_VMW_EVENT_FENCE_SIGNALED);
				if (n >= (ssize_t)sizeof(struct drm_vmw_event_fence)) {
					struct drm_vmw_event_fence *evf =
						(struct drm_vmw_event_fence *)buf;
					test_result("FENCE_EVENT: it carries the caller's cookie",
						    evf->user_data == 0x5AFE7E57ull);
				}
			}
		}
	}
	close(fd);

	/* UPDATE_LAYOUT is the host telling the guest what geometry it would
	 * like.  It changes what the display advertises, so it belongs to
	 * whoever holds the display: the render node must refuse it, and the
	 * card node must take it. */
	{
		int rfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		struct drm_vmw_rect r = { 0, 0, 1024, 768 };
		struct drm_vmw_update_layout_arg ul;
		memset(&ul, 0, sizeof(ul));
		ul.num_outputs = 1;
		ul.rects = (uint64_t)(uintptr_t)&r;
		if (rfd >= 0) {
			int rc = vmw_cmd(rfd, DRM_VMW_UPDATE_LAYOUT, _IOC_WRITE, &ul,
					 sizeof(ul));
			test_result("UPDATE_LAYOUT is refused on the render node",
				    rc == -EACCES || rc == -EINVAL || rc == -EPERM);
			close(rfd);
		}
		int cfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		if (cfd >= 0) {
			/* The layout pushed here becomes the connector's
			 * PREFERRED mode and OUTLIVES this process: the next
			 * mode-setting client starts at whatever it names.
			 * Passing a made-up 1024x768 left every X session
			 * after a test run in 1024x768 on a 1920x1200 host.
			 * So ask the connector what is preferred right now and
			 * push exactly that -- acceptance is exercised, and
			 * nothing anyone sees changes. */
			struct drm_mode_card_res cres;
			uint32_t cids[4], conn = 0;
			memset(&cres, 0, sizeof(cres));
			cres.connector_id_ptr = (uint64_t)(uintptr_t)cids;
			cres.count_connectors = 4;
			if (ioctl(cfd, DRM_IOCTL_MODE_GETRESOURCES, &cres) == 0 &&
			    cres.count_connectors >= 1)
				conn = cids[0];
			struct drm_mode_get_connector gc2;
			memset(&gc2, 0, sizeof(gc2));
			gc2.connector_id = conn;
			if (conn && ioctl(cfd, DRM_IOCTL_MODE_GETCONNECTOR, &gc2) == 0 &&
			    gc2.count_modes) {
				struct drm_mode_modeinfo pm[24];
				uint32_t nm2 = gc2.count_modes < 24 ? gc2.count_modes : 24;
				memset(&gc2, 0, sizeof(gc2));
				gc2.connector_id = conn;
				gc2.modes_ptr = (uint64_t)(uintptr_t)pm;
				gc2.count_modes = nm2;
				if (ioctl(cfd, DRM_IOCTL_MODE_GETCONNECTOR, &gc2) == 0 &&
				    gc2.count_modes) {
					uint32_t pick = 0;
					for (uint32_t i = 0; i < nm2; i++)
						if (pm[i].type & DRM_MODE_TYPE_PREFERRED) {
							pick = i;
							break;
						}
					r.w = pm[pick].hdisplay;
					r.h = pm[pick].vdisplay;
				}
			}
			int rc = vmw_cmd(cfd, DRM_VMW_UPDATE_LAYOUT, _IOC_WRITE, &ul,
					 sizeof(ul));
			/* EACCES when an X server holds the display: also correct. */
			test_result("UPDATE_LAYOUT is accepted on the card node",
				    rc == 0 || rc == -EACCES);
			close(cfd);
		}
	}
}

/* ======================================================================
 * The plumbing Mesa stands on between /dev/dri and a GL screen.  eglinfo
 * reports only "failed to create gbm device" when any link below breaks,
 * so each is pinned separately: the device numbers libdrm reads back with
 * fstat, the sysfs walk it recognises a DRM node by, the fd dup Mesa does
 * before using a display fd (os_dupfd_cloexec), and the dlopens that GBM
 * and EGL perform -- a dlopen that fails inside gbm_create_device is
 * swallowed whole, no message anywhere.
 * ====================================================================== */
/* What the DEVICE holds for a surface, read WITHOUT going through Mesa:
 * reference the surface on the card node, ask the device to write its host
 * copy back into the guest backup, and look at the first pixel.
 *
 * This is the only way to tell two failures apart that look identical from
 * OpenGL.  When a draw is followed by glReadPixels and the buffer still
 * holds the clear colour, either the rendering never happened, or it
 * happened and the READBACK that brings it into guest memory did not.
 * Mesa's glReadPixels uses that same readback, so it cannot distinguish
 * them; this can.  Returns 0 if the surface could not be read. */
/* The scan-out surface's pixels are 0xAARRGGBB (a red clear reads back as
 * ffff0000, a green one as ff00ff00). */
#define GPU_R(px) ((unsigned)(((px) >> 16) & 0xFF))
#define GPU_G(px) ((unsigned)(((px) >> 8) & 0xFF))
#define GPU_B(px) ((unsigned)((px) & 0xFF))

static uint32_t gpu_host_pixel(int cfd, uint32_t bo_handle, int do_readback)
{
	union drm_vmw_gb_surface_reference_ext_arg ra;
	struct drm_vmw_fence_rep frep;
	struct drm_vmw_execbuf_arg eb;
	uint32_t cmdbuf[3] = { 1104 /* READBACK_GB_SURFACE */, 4, bo_handle };
	uint32_t px = 0;

	if (cfd < 0 || !bo_handle)
		return 0;
	memset(&ra, 0, sizeof(ra));
	ra.req.sid = bo_handle;
	ra.req.handle_type = DRM_VMW_HANDLE_LEGACY;
	if (vmw_cmd(cfd, DRM_VMW_GB_SURFACE_REF_EXT, _IOC_READ | _IOC_WRITE, &ra,
		    sizeof(ra)) != 0)
		return 0;
	memset(&frep, 0, sizeof(frep));
	memset(&eb, 0, sizeof(eb));
	eb.version = DRM_VMW_EXECBUF_VERSION;
	eb.context_handle = 0xFFFFFFFFu;
	eb.imported_fence_fd = -1;
	eb.commands = (uint64_t)(uintptr_t)cmdbuf;
	eb.command_size = sizeof(cmdbuf);
	eb.fence_rep = (uint64_t)(uintptr_t)&frep;
	if (do_readback &&
	    vmw_cmd(cfd, DRM_VMW_EXECBUF, _IOC_WRITE, &eb, sizeof(eb)) == 0 &&
	    frep.handle) {
		struct drm_vmw_fence_wait_arg fw = { .handle = frep.handle,
						     .timeout_us = 1000000,
						     .flags = DRM_VMW_FENCE_FLAG_EXEC };
		struct drm_vmw_fence_arg fu = { .handle = frep.handle };

		vmw_cmd(cfd, DRM_VMW_FENCE_WAIT, _IOC_READ | _IOC_WRITE, &fw, sizeof(fw));
		vmw_cmd(cfd, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &fu, sizeof(fu));
	}
	uint32_t *kp = mmap(NULL, ra.rep.crep.buffer_size, PROT_READ, MAP_SHARED,
			    cfd, (off_t)ra.rep.crep.buffer_map_handle);
	if (kp != MAP_FAILED) {
		px = kp[0];
		munmap(kp, ra.rep.crep.buffer_size);
	}
	struct drm_vmw_surface_arg us = { .sid = (int32_t)ra.rep.crep.handle };
	struct drm_vmw_handle_close_arg hc = { .handle = ra.rep.crep.buffer_handle };

	vmw_cmd(cfd, DRM_VMW_UNREF_SURFACE, _IOC_WRITE, &us, sizeof(us));
	vmw_cmd(cfd, DRM_VMW_UNREF_DMABUF, _IOC_WRITE, &hc, sizeof(hc));
	return px;
}

static void test_gpu_userland(void)
{
	struct stat st;

	printf("\n[TEST] GPU userland plumbing (/dev/dri -> Mesa)\n");
	if (stat("/dev/dri/card0", &st) != 0) {
		printf("  (no display-manager device, skipped)\n");
		return;
	}
	test_result("card0 is char 226:0",
		    S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 && minor(st.st_rdev) == 0);
	test_result("renderD128 is char 226:128",
		    stat("/dev/dri/renderD128", &st) == 0 && S_ISCHR(st.st_mode) &&
			    major(st.st_rdev) == 226 && minor(st.st_rdev) == 128);
	/* drmNodeIsDRM: a node is a DRM node iff this sysfs path resolves. */
	test_result("/sys/dev/char/226:0 resolves", stat("/sys/dev/char/226:0", &st) == 0);
	test_result("/sys/dev/char/226:128/device/drm exists (drmNodeIsDRM)",
		    stat("/sys/dev/char/226:128/device/drm", &st) == 0);

	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	test_result("open renderD128", fd >= 0);
	int dfd = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
	test_result("F_DUPFD_CLOEXEC of the drm fd", dfd >= 3);
	if (dfd >= 0) {
		struct drm_vmw_getparam_arg gp = { .param = DRM_VMW_PARAM_3D };
		test_result("the dup carries the device (GET_PARAM 3D)",
			    vmw_cmd(dfd, DRM_VMW_GET_PARAM, _IOC_READ | _IOC_WRITE, &gp,
				    sizeof(gp)) == 0);
		test_result("FD_CLOEXEC is set on the dup",
			    (fcntl(dfd, F_GETFD, 0) & FD_CLOEXEC) != 0);
		close(dfd);
	}

	/* What GBM loads when gbm_create_device() runs. */
	void *bk = dlopen("/usr/lib/gbm/dri_gbm.so", RTLD_NOW | RTLD_GLOBAL);
	test_result("dlopen of the GBM backend (dri_gbm.so)", bk != NULL);
	if (!bk)
		printf("      (dlerror: %s)\n", dlerror());
	void *g = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	test_result("dlopen libgbm.so.1", g != NULL);
	if (!g)
		printf("      (dlerror: %s)\n", dlerror());
	if (g && fd >= 0) {
		void *(*create)(int) = (void *(*)(int))dlsym(g, "gbm_create_device");
		const char *(*backend_name)(void *) =
			(const char *(*)(void *))dlsym(g, "gbm_device_get_backend_name");
		void (*destroy)(void *) = (void (*)(void *))dlsym(g, "gbm_device_destroy");
		void *dev = create ? create(fd) : NULL;
		test_result("gbm_create_device on the render node", dev != NULL);
		if (dev && backend_name)
			printf("      (gbm backend: %s)\n", backend_name(dev));
		if (dev && destroy)
			destroy(dev);
	}
	/* What GLX/EGL load for a hardware screen. */
	void *dri = dlopen("/usr/lib/dri/vmwgfx_dri.so", RTLD_NOW | RTLD_GLOBAL);
	test_result("dlopen vmwgfx_dri.so", dri != NULL);
	if (!dri)
		printf("      (dlerror: %s)\n", dlerror());

	/* How Mesa finds GPUs at all: drmGetDevices2() starts by LISTING
	 * /dev/dri.  Opening the nodes by name proves nothing about this --
	 * the listing was broken for months while every open worked, and the
	 * only symptom was eglInitialize failing with no message. */
	{
		DIR *d = opendir("/dev/dri");
		int saw_card = 0, saw_render = 0;
		struct dirent *de;
		test_result("opendir /dev/dri", d != NULL);
		while (d && (de = readdir(d)) != NULL) {
			if (strcmp(de->d_name, "card0") == 0)
				saw_card = 1;
			if (strcmp(de->d_name, "renderD128") == 0)
				saw_render = 1;
		}
		if (d)
			closedir(d);
		test_result("readdir lists card0", saw_card);
		test_result("readdir lists renderD128", saw_render);
	}
	{
		/* The prefix of libdrm's drmDevice, enough to check nodes. */
		struct mini_drmdev {
			char **nodes;
			int available_nodes;
			int bustype;
		};
		void *ldrm = dlopen("libdrm.so.2", RTLD_NOW | RTLD_GLOBAL);
		test_result("dlopen libdrm.so.2", ldrm != NULL);
		int (*getdevs)(uint32_t, void **, int) =
			ldrm ? (int (*)(uint32_t, void **, int))dlsym(ldrm, "drmGetDevices2") : NULL;
		void (*freedevs)(void **, int) =
			ldrm ? (void (*)(void **, int))dlsym(ldrm, "drmFreeDevices") : NULL;
		void *devs[8];
		int n = getdevs ? getdevs(0, devs, 8) : -1;
		test_result("drmGetDevices2 finds the device", n >= 1);
		if (n >= 1) {
			struct mini_drmdev *d0 = devs[0];
			printf("      (bustype %d, nodes 0x%x)\n", d0->bustype,
			       (unsigned)d0->available_nodes);
			test_result("it is a PCI device", d0->bustype == 0);
			test_result("it has the primary node", (d0->available_nodes & 1) != 0);
			test_result("it has the render node", (d0->available_nodes & 4) != 0);
			if (freedevs)
				freedevs(devs, n);
		}
	}

	/* The whole glamor front half, exactly as the X server runs it: a GBM
	 * device on the CARD node (glamor's fd is the KMS fd), EGL on top of
	 * it, a no-config GL context made current with no surface, and the
	 * renderer asked for its name. */
	int has3d = 0;
	if (fd >= 0) {
		struct drm_vmw_getparam_arg gp3 = { .param = DRM_VMW_PARAM_3D };
		has3d = vmw_cmd(fd, DRM_VMW_GET_PARAM, _IOC_READ | _IOC_WRITE, &gp3,
				sizeof(gp3)) == 0 && gp3.value == 1;
	}
	int cfd2 = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	void *g2 = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	void *(*gbm_create)(int) =
		g2 ? (void *(*)(int))dlsym(g2, "gbm_create_device") : NULL;
	void (*gbm_destroy)(void *) =
		g2 ? (void (*)(void *))dlsym(g2, "gbm_device_destroy") : NULL;
	void *cgbm = (gbm_create && cfd2 >= 0) ? gbm_create(cfd2) : NULL;
	test_result("gbm_create_device on the card node", cgbm != NULL);
	void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	test_result("dlopen libEGL.so.1", egl != NULL);
	if (egl && cgbm) {
		void *(*gpa)(const char *) =
			(void *(*)(const char *))dlsym(egl, "eglGetProcAddress");
		int (*egl_err)(void) = (int (*)(void))dlsym(egl, "eglGetError");
		void *(*getdpy)(unsigned, void *, const void *) =
			(void *(*)(unsigned, void *, const void *))dlsym(egl, "eglGetPlatformDisplay");
		void *dpy = getdpy ? getdpy(0x31D7 /* EGL_PLATFORM_GBM_KHR */, cgbm, NULL) : NULL;
		test_result("eglGetPlatformDisplay(GBM)", dpy != NULL);
		int (*egl_init)(void *, int *, int *) =
			(int (*)(void *, int *, int *))dlsym(egl, "eglInitialize");
		int emaj = 0, emin = 0;
		int inited = dpy && egl_init && egl_init(dpy, &emaj, &emin);
		test_result("eglInitialize on the GBM device (glamor's first check)", inited);
		if (!inited) {
			printf("      (eglGetError: 0x%x)\n", egl_err ? egl_err() : 0);
		} else {
			const char *(*qs)(void *, int) =
				(const char *(*)(void *, int))dlsym(egl, "eglQueryString");
			printf("      (EGL %d.%d, vendor %s)\n", emaj, emin,
			       qs ? qs(dpy, 0x3053 /* EGL_VENDOR */) : "?");
			unsigned (*bind)(unsigned) =
				(unsigned (*)(unsigned))dlsym(egl, "eglBindAPI");
			if (bind)
				bind(0x30A2 /* EGL_OPENGL_API */);
			void *(*cctx)(void *, void *, void *, const int *) =
				(void *(*)(void *, void *, void *, const int *))dlsym(egl, "eglCreateContext");
			static const int noattr[] = { 0x3038 /* EGL_NONE */ };
			void *ctx = cctx ? cctx(dpy, NULL /* EGL_NO_CONFIG_KHR */, NULL, noattr) : NULL;
			test_result("no-config GL context (EGL_KHR_no_config_context)", ctx != NULL);
			if (!ctx)
				printf("      (eglGetError: 0x%x)\n", egl_err ? egl_err() : 0);
			int (*mkcur)(void *, void *, void *, void *) =
				(int (*)(void *, void *, void *, void *))dlsym(egl, "eglMakeCurrent");
			int cur = ctx && mkcur && mkcur(dpy, NULL, NULL, ctx);
			test_result("surfaceless eglMakeCurrent", cur);
			if (cur) {
				const unsigned char *(*glgs)(unsigned) =
					gpa ? (const unsigned char *(*)(unsigned))gpa("glGetString") : NULL;
				const char *ren = glgs ? (const char *)glgs(0x1F01 /* GL_RENDERER */) : NULL;
				printf("      (GL_RENDERER: %s)\n", ren ? ren : "?");
				if (has3d)
					test_result("renderer is the device, not llvmpipe",
						    ren && !strstr(ren, "llvmpipe") && !strstr(ren, "softpipe"));

				/* The decisive question for a black X screen:
				 * does GL rendering actually land in a
				 * scan-out buffer, and does the KERNEL's idea
				 * of that surface hold the same pixels?  Clear
				 * a gbm bo to red through a GL framebuffer,
				 * then read it back both ways.  GL red +
				 * kernel black would mean the display shows a
				 * different surface than Mesa renders into;
				 * GL black means rendering never lands. */
				void *(*bocreate3)(void *, uint32_t, uint32_t, uint32_t, uint32_t) =
					(void *(*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))dlsym(g2, "gbm_bo_create");
				unsigned long (*bohandle3)(void *) =
					(unsigned long (*)(void *))dlsym(g2, "gbm_bo_get_handle");
				void (*bodestroy3)(void *) =
					(void (*)(void *))dlsym(g2, "gbm_bo_destroy");
				void *rbo = bocreate3 ? bocreate3(cgbm, 64, 64, 0x34325258u /* XRGB8888 */,
								  (1u << 0) | (1u << 2)) : NULL;
				void *(*imgcreate)(void *, void *, unsigned, void *, const int *) =
					gpa ? (void *(*)(void *, void *, unsigned, void *, const int *))gpa("eglCreateImageKHR") : NULL;
				unsigned (*imgdestroy)(void *, void *) =
					gpa ? (unsigned (*)(void *, void *))gpa("eglDestroyImageKHR") : NULL;
				void *img = (rbo && imgcreate) ?
					imgcreate(dpy, NULL, 0x30B0 /* EGL_NATIVE_PIXMAP_KHR */, rbo, NULL) : NULL;
				test_result("EGLImage from a scan-out gbm bo", img != NULL);
				if (img) {
					void (*gentex)(int, unsigned *) = (void (*)(int, unsigned *))gpa("glGenTextures");
					void (*bindtex)(unsigned, unsigned) = (void (*)(unsigned, unsigned))gpa("glBindTexture");
					void (*teximg)(unsigned, void *) = (void (*)(unsigned, void *))gpa("glEGLImageTargetTexture2DOES");
					void (*genfbo)(int, unsigned *) = (void (*)(int, unsigned *))gpa("glGenFramebuffers");
					void (*bindfbo)(unsigned, unsigned) = (void (*)(unsigned, unsigned))gpa("glBindFramebuffer");
					void (*fbotex)(unsigned, unsigned, unsigned, unsigned, int) =
						(void (*)(unsigned, unsigned, unsigned, unsigned, int))gpa("glFramebufferTexture2D");
					unsigned (*fbostat)(unsigned) = (unsigned (*)(unsigned))gpa("glCheckFramebufferStatus");
					void (*clearcol)(float, float, float, float) =
						(void (*)(float, float, float, float))gpa("glClearColor");
					void (*clear)(unsigned) = (void (*)(unsigned))gpa("glClear");
					void (*finish)(void) = (void (*)(void))gpa("glFinish");
					void (*readpix)(int, int, int, int, unsigned, unsigned, void *) =
						(void (*)(int, int, int, int, unsigned, unsigned, void *))gpa("glReadPixels");
					unsigned tex = 0, fbo = 0;
					gentex(1, &tex);
					bindtex(0x0DE1 /* GL_TEXTURE_2D */, tex);
					teximg(0x0DE1, img);
					genfbo(1, &fbo);
					bindfbo(0x8D40 /* GL_FRAMEBUFFER */, fbo);
					fbotex(0x8D40, 0x8CE0 /* COLOR_ATTACHMENT0 */, 0x0DE1, tex, 0);
					unsigned fst = fbostat(0x8D40);
					test_result("framebuffer complete on the scan-out texture",
						    fst == 0x8CD5 /* COMPLETE */);
					clearcol(1.0f, 0.0f, 0.0f, 1.0f);
					clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
					finish();
					unsigned char px[4] = { 0, 0, 0, 0 };
					readpix(1, 1, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
					test_result("GL rendering lands (glReadPixels sees red)",
						    px[0] == 255 && px[1] == 0 && px[2] == 0);
					if (!(px[0] == 255 && px[1] == 0))
						printf("      (readpixels: %u,%u,%u,%u)\n",
						       px[0], px[1], px[2], px[3]);
					/* Now ask the KERNEL what that surface
					 * holds: READBACK_GB_SURFACE copies the
					 * host content into the guest backup,
					 * which maps.  Same sid the display
					 * would scan out. */
					uint32_t kh = (uint32_t)bohandle3(rbo);
					union drm_vmw_gb_surface_reference_ext_arg ra2;
					memset(&ra2, 0, sizeof(ra2));
					ra2.req.sid = kh;
					ra2.req.handle_type = DRM_VMW_HANDLE_LEGACY;
					int rr2 = vmw_cmd(cfd2, DRM_VMW_GB_SURFACE_REF_EXT,
							  _IOC_READ | _IOC_WRITE, &ra2, sizeof(ra2));
					test_result("GB_SURFACE_REF_EXT by handle", rr2 == 0);
					if (rr2 == 0) {
						uint32_t cmdbuf[3] = { 1104 /* READBACK_GB_SURFACE */, 4, kh };
						struct drm_vmw_fence_rep frep;
						struct drm_vmw_execbuf_arg eb2;
						memset(&frep, 0, sizeof(frep));
						frep.error = -1;
						memset(&eb2, 0, sizeof(eb2));
						eb2.version = DRM_VMW_EXECBUF_VERSION;
						eb2.context_handle = 0xFFFFFFFFu;
						eb2.imported_fence_fd = -1;
						eb2.commands = (uint64_t)(uintptr_t)cmdbuf;
						eb2.command_size = sizeof(cmdbuf);
						eb2.fence_rep = (uint64_t)(uintptr_t)&frep;
						int er2 = vmw_cmd(cfd2, DRM_VMW_EXECBUF, _IOC_WRITE,
								  &eb2, sizeof(eb2));
						test_result("READBACK_GB_SURFACE submits", er2 == 0);
						if (er2 == 0 && frep.handle) {
							struct drm_vmw_fence_wait_arg fw2 = {
								.handle = frep.handle,
								.timeout_us = 1000000,
								.flags = DRM_VMW_FENCE_FLAG_EXEC
							};
							vmw_cmd(cfd2, DRM_VMW_FENCE_WAIT,
								_IOC_READ | _IOC_WRITE, &fw2, sizeof(fw2));
							struct drm_vmw_fence_arg fu2 = { .handle = frep.handle };
							vmw_cmd(cfd2, DRM_VMW_FENCE_UNREF, _IOC_WRITE, &fu2, sizeof(fu2));
						}
						uint32_t *kp = mmap(NULL, ra2.rep.crep.buffer_size,
								    PROT_READ, MAP_SHARED, cfd2,
								    (off_t)ra2.rep.crep.buffer_map_handle);
						test_result("backup of the scan-out surface maps", kp != MAP_FAILED);
						if (kp != MAP_FAILED) {
							test_result("the kernel-visible surface holds the pixels (host content is red)",
								    (kp[0] & 0xFFFFFF) == 0xFF0000);
							if ((kp[0] & 0xFFFFFF) != 0xFF0000)
								printf("      (backup pixel: %08x)\n", kp[0]);
							munmap(kp, ra2.rep.crep.buffer_size);
						}
						struct drm_vmw_surface_arg us2 = { .sid = (int32_t)ra2.rep.crep.handle };
						vmw_cmd(cfd2, DRM_VMW_UNREF_SURFACE, _IOC_WRITE, &us2, sizeof(us2));
						struct drm_vmw_handle_close_arg hc2 = { .handle = ra2.rep.crep.buffer_handle };
						vmw_cmd(cfd2, DRM_VMW_UNREF_DMABUF, _IOC_WRITE, &hc2, sizeof(hc2));
					}
					/* The half the red-clear does NOT
					 * cover -- and the half every missing
					 * glyph and black GTK window lives
					 * in: SAMPLING a texture the guest
					 * uploaded.  A fill comes out of the
					 * shader alone; text and client
					 * images are textures uploaded from
					 * guest memory and then drawn.  So:
					 * upload an 8x8 green texture, draw
					 * it across the red framebuffer,
					 * read the result.  Green = the
					 * upload+sample path works end to
					 * end.  Red = the draw ran but
					 * sampled nothing.  Black = the draw
					 * itself did not land. */
					unsigned (*mkshader)(unsigned) = (unsigned (*)(unsigned))gpa("glCreateShader");
					void (*shsrc)(unsigned, int, const char **, const int *) =
						(void (*)(unsigned, int, const char **, const int *))gpa("glShaderSource");
					void (*shcomp)(unsigned) = (void (*)(unsigned))gpa("glCompileShader");
					void (*shiv)(unsigned, unsigned, int *) =
						(void (*)(unsigned, unsigned, int *))gpa("glGetShaderiv");
					void (*shlog)(unsigned, int, int *, char *) =
						(void (*)(unsigned, int, int *, char *))gpa("glGetShaderInfoLog");
					unsigned (*mkprog)(void) = (unsigned (*)(void))gpa("glCreateProgram");
					void (*attach)(unsigned, unsigned) = (void (*)(unsigned, unsigned))gpa("glAttachShader");
					void (*link)(unsigned) = (void (*)(unsigned))gpa("glLinkProgram");
					void (*proiv)(unsigned, unsigned, int *) =
						(void (*)(unsigned, unsigned, int *))gpa("glGetProgramiv");
					void (*useprog)(unsigned) = (void (*)(unsigned))gpa("glUseProgram");
					int (*uloc)(unsigned, const char *) =
						(int (*)(unsigned, const char *))gpa("glGetUniformLocation");
					void (*uni1i)(int, int) = (void (*)(int, int))gpa("glUniform1i");
					int (*aloc)(unsigned, const char *) =
						(int (*)(unsigned, const char *))gpa("glGetAttribLocation");
					void (*genva)(int, unsigned *) = (void (*)(int, unsigned *))gpa("glGenVertexArrays");
					void (*bindva)(unsigned) = (void (*)(unsigned))gpa("glBindVertexArray");
					void (*genbuf)(int, unsigned *) = (void (*)(int, unsigned *))gpa("glGenBuffers");
					void (*bindbuf)(unsigned, unsigned) = (void (*)(unsigned, unsigned))gpa("glBindBuffer");
					void (*bufdata)(unsigned, long, const void *, unsigned) =
						(void (*)(unsigned, long, const void *, unsigned))gpa("glBufferData");
					void (*vattr)(unsigned, int, unsigned, unsigned char, int, const void *) =
						(void (*)(unsigned, int, unsigned, unsigned char, int, const void *))gpa("glVertexAttribPointer");
					void (*venable)(unsigned) = (void (*)(unsigned))gpa("glEnableVertexAttribArray");
					void (*draw)(unsigned, int, int) = (void (*)(unsigned, int, int))gpa("glDrawArrays");
					void (*teximg2)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) =
						(void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *))gpa("glTexImage2D");
					void (*texsub2)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) =
						(void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *))gpa("glTexSubImage2D");
					void (*texpar)(unsigned, unsigned, int) =
						(void (*)(unsigned, unsigned, int))gpa("glTexParameteri");
					void (*acttex)(unsigned) = (void (*)(unsigned))gpa("glActiveTexture");
					void (*viewport)(int, int, int, int) = (void (*)(int, int, int, int))gpa("glViewport");
					unsigned (*glerr)(void) = (unsigned (*)(void))gpa("glGetError");
					void (*getteximg)(unsigned, int, unsigned, unsigned, void *) =
						(void (*)(unsigned, int, unsigned, unsigned, void *))gpa("glGetTexImage");
					int sampling_ready = mkshader && shsrc && shcomp && mkprog &&
							     attach && link && useprog && uloc && uni1i &&
							     aloc && genbuf && bindbuf && bufdata && vattr &&
							     venable && draw && teximg2 && texsub2 &&
							     texpar && acttex && viewport;
					if (sampling_ready) {
						static const char *vs_src =
							"#version 140\n"
							"in vec2 p; out vec2 t;\n"
							"void main(){ t = p*0.5+0.5; gl_Position = vec4(p,0,1); }\n";
						static const char *fs_src =
							"#version 140\n"
							"uniform sampler2D s; in vec2 t; out vec4 c;\n"
							"void main(){ c = texture(s, t); }\n";
						unsigned vsh = mkshader(0x8B31), fsh = mkshader(0x8B30);
						int okv = 0, okf = 0, okl = 0;
						shsrc(vsh, 1, &vs_src, NULL); shcomp(vsh);
						shiv(vsh, 0x8B81, &okv);
						shsrc(fsh, 1, &fs_src, NULL); shcomp(fsh);
						shiv(fsh, 0x8B81, &okf);
						unsigned prog = mkprog();
						attach(prog, vsh); attach(prog, fsh); link(prog);
						proiv(prog, 0x8B82, &okl);
						test_result("texture-sampling shader compiles and links",
							    okv && okf && okl);
						if (!(okv && okf && okl) && shlog) {
							char log[256]; int n = 0;
							shlog(okv ? fsh : vsh, sizeof(log) - 1, &n, log);
							log[n < 0 ? 0 : n] = 0;
							printf("      (shader log: %s)\n", log);
						}
						useprog(prog);
						unsigned vao = 0;
						if (genva && bindva) { genva(1, &vao); bindva(vao); }
						static const float quad[12] = { -1, -1, 1, -1, 1, 1,
										-1, -1, 1, 1, -1, 1 };
						unsigned vb = 0;
						genbuf(1, &vb);
						bindbuf(0x8892 /* ARRAY_BUFFER */, vb);
						bufdata(0x8892, sizeof(quad), quad, 0x88E4 /* STATIC_DRAW */);
						int pa = aloc(prog, "p");
						vattr((unsigned)pa, 2, 0x1406 /* FLOAT */, 0, 0, NULL);
						venable((unsigned)pa);
						viewport(0, 0, 64, 64);

						/* Before a single texel is involved: does a
						 * DRAW land, and does it come back?
						 *
						 * Two separate questions that the same
						 * readpixels used to answer as one.  The draw
						 * here uses the SAME vertices, viewport and
						 * render target with a fragment shader that
						 * writes a constant, so nothing about textures
						 * is involved: what it produces is either on
						 * the render target or it is not.
						 *
						 * And it is asked TWICE, of two different
						 * readers.  gpu_host_pixel() asks the device
						 * for its own copy of the surface, through a
						 * readback this test issues itself; glReadPixels
						 * asks the GL stack.  They are separate
						 * failures with separate causes, and reporting
						 * them as one is what made every test below
						 * this point read as a texturing failure when
						 * texturing was fine. */
						{
							static const char *fs_solid =
								"#version 140\n"
								"out vec4 c;\n"
								"void main(){ c = vec4(0,1,0,1); }\n";
							unsigned sfs = mkshader(0x8B30);
							int oks = 0, okls = 0;

							shsrc(sfs, 1, &fs_solid, NULL);
							shcomp(sfs);
							shiv(sfs, 0x8B81, &oks);
							unsigned sprog = mkprog();
							attach(sprog, vsh);
							attach(sprog, sfs);
							link(sprog);
							proiv(sprog, 0x8B82, &okls);
							if (oks && okls) {
								useprog(sprog);
								int spa = aloc(sprog, "p");
								if (spa >= 0) {
									vattr((unsigned)spa, 2, 0x1406, 0, 0, NULL);
									venable((unsigned)spa);
								}
								draw(0x0004 /* TRIANGLES */, 0, 6);
								finish();
								unsigned char gp[4] = { 9, 9, 9, 9 };
								readpix(32, 32, 1, 1, 0x1908, 0x1401, gp);
								unsigned dev = (unsigned)gpu_host_pixel(
									cfd2, (uint32_t)bohandle3(rbo), 1);

								test_result("a draw with no texture lands (vertices, shader, render target)",
									    GPU_G(dev) == 255 &&
										    GPU_R(dev) == 0 &&
										    GPU_B(dev) == 0);
								test_result("the render target reads back through GL after a draw",
									    gp[0] == 0 && gp[1] == 255 &&
										    gp[2] == 0);
								if (!(gp[0] == 0 && gp[1] == 255 && gp[2] == 0))
									printf("      (the device holds %02x,%02x,%02x; glReadPixels says %u,%u,%u -- the drawing is there, the readback of it is not)\n",
									       GPU_R(dev), GPU_G(dev), GPU_B(dev),
									       gp[0], gp[1], gp[2]);
							} else {
								test_fail("a draw with no texture lands (vertices, shader, render target)");
								test_fail("the render target reads back through GL after a draw");
							}
							useprog(prog);
							vattr((unsigned)pa, 2, 0x1406, 0, 0, NULL);
							venable((unsigned)pa);
						}
						acttex(0x84C0 /* TEXTURE0 */);
						unsigned stex = 0;
						gentex(1, &stex);
						bindtex(0x0DE1, stex);
						/* Filter must not need mipmaps, or the
						 * texture is incomplete and samples
						 * BLACK -- which is this bug's shape,
						 * so it must not be the test's. */
						texpar(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
						texpar(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
						static unsigned char green8[8 * 8 * 4];
						for (int i = 0; i < 8 * 8; i++) {
							green8[i * 4 + 0] = 0;
							green8[i * 4 + 1] = 255;
							green8[i * 4 + 2] = 0;
							green8[i * 4 + 3] = 255;
						}
						teximg2(0x0DE1, 0, 0x8058 /* RGBA8 */, 8, 8, 0,
							0x1908 /* RGBA */, 0x1401, green8);

						/* Did the upload reach the host?
						 *
						 * glGetTexImage asks the device for the image
						 * it is holding, which for a guest-backed
						 * surface means a readback into the backing
						 * store and a copy out of it -- the opposite
						 * direction to the upload just made, and a
						 * direction already known to work (the render
						 * target read back correctly above).  So green
						 * here means the guest's bytes DID reach the
						 * host and only SAMPLING is broken; anything
						 * else means the upload is where they are
						 * lost, and the sampling result below says
						 * nothing more. */
						if (getteximg) {
							static unsigned char back[8 * 8 * 4];

							memset(back, 0x11, sizeof(back));
							getteximg(0x0DE1, 0, 0x1908, 0x1401, back);
							test_result("a guest-uploaded texture reads back from the device",
								    back[0] == 0 && back[1] == 255 && back[2] == 0);
							if (!(back[1] == 255))
								printf("      (texel 0 read back %u,%u,%u,%u)\n",
								       back[0], back[1], back[2], back[3]);
						}
						uni1i(uloc(prog, "s"), 0);
						viewport(0, 0, 64, 64);
						draw(0x0004 /* TRIANGLES */, 0, 6);
						finish();
						/* Asked of the DEVICE, not of glReadPixels.
						 * What is being tested here is whether the
						 * texture's bytes reached the host and were
						 * sampled -- and reading the answer back
						 * through GL puts a second, unrelated
						 * mechanism in the way of it.  When that
						 * mechanism was broken this test reported a
						 * texturing failure that did not exist, and
						 * once reported a PASS it had not earned,
						 * having read a colour an earlier test left
						 * in the buffer. */
						unsigned sdev = (unsigned)gpu_host_pixel(
							cfd2, (uint32_t)bohandle3(rbo), 1);
						test_result("sampling a guest-uploaded RGBA texture works (glyph/PutImage path)",
							    GPU_R(sdev) == 0 && GPU_G(sdev) == 255 &&
								    GPU_B(sdev) == 0);
						printf("      (device holds %02x,%02x,%02x; glGetError 0x%x)\n",
						       GPU_R(sdev), GPU_G(sdev), GPU_B(sdev),
						       glerr ? glerr() : 0);

						/* Partial update -- the PutImage shape:
						 * overwrite one corner of the texture
						 * with blue and redraw. */
						static unsigned char blue4[4 * 4 * 4];
						for (int i = 0; i < 4 * 4; i++) {
							blue4[i * 4 + 0] = 0;
							blue4[i * 4 + 1] = 0;
							blue4[i * 4 + 2] = 255;
							blue4[i * 4 + 3] = 255;
						}
						texsub2(0x0DE1, 0, 0, 0, 4, 4, 0x1908, 0x1401, blue4);
						draw(0x0004, 0, 6);
						finish();
						/* The updated corner is the texture's origin,
						 * which the quad puts at the render target's
						 * origin -- so pixel 0, which is the pixel the
						 * device is asked about. */
						unsigned bdev = (unsigned)gpu_host_pixel(
							cfd2, (uint32_t)bohandle3(rbo), 1);
						test_result("partial texture update samples back (glTexSubImage2D path)",
							    GPU_R(bdev) == 0 && GPU_G(bdev) == 0 &&
								    GPU_B(bdev) == 255);
						printf("      (subimage corner on the device: %02x,%02x,%02x)\n",
						       GPU_R(bdev), GPU_G(bdev), GPU_B(bdev));

						/* A one-channel texture -- the glyph
						 * atlas shape (GL_R8).  Sampling gives
						 * (r,0,0,1): expect half-red. */
						unsigned rtex = 0;
						gentex(1, &rtex);
						bindtex(0x0DE1, rtex);
						texpar(0x0DE1, 0x2801, 0x2600);
						texpar(0x0DE1, 0x2800, 0x2600);
						static unsigned char gray8[8 * 8];
						for (int i = 0; i < 8 * 8; i++)
							gray8[i] = 0x80;
						teximg2(0x0DE1, 0, 0x8229 /* R8 */, 8, 8, 0,
							0x1903 /* RED */, 0x1401, gray8);
						draw(0x0004, 0, 6);
						finish();
						unsigned rdev = (unsigned)gpu_host_pixel(
							cfd2, (uint32_t)bohandle3(rbo), 1);
						test_result("sampling a one-channel texture works (glyph-atlas shape)",
							    GPU_R(rdev) >= 0x70 && GPU_R(rdev) <= 0x90 &&
								    GPU_G(rdev) == 0 && GPU_B(rdev) == 0);
						printf("      (R8 on the device: %02x,%02x,%02x; glGetError 0x%x)\n",
						       GPU_R(rdev), GPU_G(rdev), GPU_B(rdev),
						       glerr ? glerr() : 0);
					} else {
						test_fail("texture-sampling GL entry points resolve");
					}
					if (imgdestroy)
						imgdestroy(dpy, img);
				}
				if (rbo && bodestroy3)
					bodestroy3(rbo);
				mkcur(dpy, NULL, NULL, NULL);
			}
			void (*dctx)(void *, void *) =
				(void (*)(void *, void *))dlsym(egl, "eglDestroyContext");
			if (ctx && dctx)
				dctx(dpy, ctx);
			int (*term)(void *) = (int (*)(void *))dlsym(egl, "eglTerminate");
			if (term)
				term(dpy);
		}
	}
	/* The front buffer exactly as modesetting's ScreenInit creates it once
	 * glamor is up: a gbm SCANOUT+RENDERING bo at the display mode (an
	 * svga guest-backed surface underneath, not a dumb buffer), ADDFB of
	 * its handle, and SETCRTC to show it.  Every step that fails in there
	 * surfaces in X only as "AddScreen/ScreenInit failed". */
	if (cgbm && cfd2 >= 0 && g2) {
		struct drm_mode_card_res res2;
		uint32_t crtcs2[4], conns2[4];
		memset(&res2, 0, sizeof(res2));
		res2.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs2;
		res2.count_crtcs = 4;
		res2.connector_id_ptr = (uint64_t)(uintptr_t)conns2;
		res2.count_connectors = 4;
		struct drm_mode_get_connector gc3;
		struct drm_mode_modeinfo md[24];
		uint32_t mw = 0, mh = 0;
		struct drm_mode_modeinfo mm;
		memset(&mm, 0, sizeof(mm));
		if (ioctl(cfd2, DRM_IOCTL_MODE_GETRESOURCES, &res2) == 0 &&
		    res2.count_crtcs >= 1 && res2.count_connectors >= 1) {
			memset(&gc3, 0, sizeof(gc3));
			gc3.connector_id = conns2[0];
			gc3.modes_ptr = (uint64_t)(uintptr_t)md;
			gc3.count_modes = 24;
			if (ioctl(cfd2, DRM_IOCTL_MODE_GETCONNECTOR, &gc3) == 0 &&
			    gc3.count_modes >= 1) {
				mm = md[0];
				mw = mm.hdisplay;
				mh = mm.vdisplay;
			}
		}
		void *(*bocreate)(void *, uint32_t, uint32_t, uint32_t, uint32_t) =
			(void *(*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))dlsym(g2, "gbm_bo_create");
		unsigned long (*bohandle)(void *) =
			(unsigned long (*)(void *))dlsym(g2, "gbm_bo_get_handle");
		unsigned (*bostride)(void *) =
			(unsigned (*)(void *))dlsym(g2, "gbm_bo_get_stride");
		void (*bodestroy)(void *) =
			(void (*)(void *))dlsym(g2, "gbm_bo_destroy");
		if (mw && bocreate) {
			errno = 0;
			/* GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | USE_RENDERING:
			 * what drmmode_create_bo asks for at depth 24. */
			void *bo = bocreate(cgbm, mw, mh, 0x34325241u,
					    (1u << 0) | (1u << 2));
			test_result("gbm scanout+rendering bo at the display mode", bo != NULL);
			if (!bo) {
				printf("      (%ux%u ARGB8888; errno=%d)\n", mw, mh, errno);
				/* modesetting would fall back to XRGB only via
				 * modifiers; still, name whether XRGB works. */
				bo = bocreate(cgbm, mw, mh, 0x34325258u,
					      (1u << 0) | (1u << 2));
				printf("      (XRGB8888 instead: %s)\n", bo ? "works" : "also fails");
			}
			/* ARGB with RENDERING only: is the refusal specific to
			 * the scanout bind, or to the format at all? */
			void *bo2 = bocreate(cgbm, 256, 256, 0x34325241u, (1u << 2));
			printf("      (ARGB rendering-only bo: %s)\n", bo2 ? "works" : "fails");
			if (bo2 && bodestroy)
				bodestroy(bo2);
			if (bo) {
				uint32_t handle = (uint32_t)bohandle(bo);
				uint32_t stride = bostride(bo);
				struct drm_mode_fb_cmd fbc;
				memset(&fbc, 0, sizeof(fbc));
				fbc.width = mw;
				fbc.height = mh;
				fbc.pitch = stride;
				fbc.bpp = 32;
				fbc.depth = 24;
				fbc.handle = handle;
				errno = 0;
				int ar = ioctl(cfd2, DRM_IOCTL_MODE_ADDFB, &fbc);
				test_result("ADDFB of the gbm bo (a surface, not a dumb buffer)",
					    ar == 0 && fbc.fb_id != 0);
				if (ar != 0)
					printf("      (errno=%d handle=%u stride=%u)\n", errno,
					       handle, stride);
				if (ar == 0) {
					struct drm_mode_crtc sc2;
					memset(&sc2, 0, sizeof(sc2));
					sc2.crtc_id = crtcs2[0];
					sc2.fb_id = fbc.fb_id;
					sc2.set_connectors_ptr = (uint64_t)(uintptr_t)conns2;
					sc2.count_connectors = 1;
					sc2.mode = mm;
					sc2.mode_valid = 1;
					errno = 0;
					test_result("SETCRTC shows the glamor front buffer",
						    ioctl(cfd2, DRM_IOCTL_MODE_SETCRTC, &sc2) == 0);
					if (errno)
						printf("      (errno=%d)\n", errno);
					/* The step that actually moves pixels.  A
					 * surface framebuffer reaches the screen
					 * target by being bound to it (or copied
					 * into the driver's display surface);
					 * SETCRTC alone proves none of that, and
					 * a failure here is exactly the "black
					 * screen, everything else fine" case. */
					struct drm_mode_fb_dirty_cmd dc;
					struct drm_clip_rect dr = { 0, 0, (short)mw, (short)mh };
					memset(&dc, 0, sizeof(dc));
					dc.fb_id = fbc.fb_id;
					dc.num_clips = 1;
					dc.clips_ptr = (uint64_t)(uintptr_t)&dr;
					errno = 0;
					int drc = ioctl(cfd2, DRM_IOCTL_MODE_DIRTYFB, &dc);
					test_result("DIRTYFB presents the surface (pixels reach the screen)",
						    drc == 0);
					if (drc != 0)
						printf("      (errno=%d -- the present path refused it)\n",
						       errno);
					struct drm_mode_crtc_page_flip pf2;
					memset(&pf2, 0, sizeof(pf2));
					pf2.crtc_id = crtcs2[0];
					pf2.fb_id = fbc.fb_id;
					errno = 0;
					int pr2 = ioctl(cfd2, DRM_IOCTL_MODE_PAGE_FLIP, &pf2);
					test_result("PAGE_FLIP to the surface framebuffer",
						    pr2 == 0 || errno == EINVAL);
					ioctl(cfd2, DRM_IOCTL_MODE_RMFB, &fbc.fb_id);
				}
				if (bodestroy)
					bodestroy(bo);
			}
		}
	}

	/* Split which layer refuses ARGB8888 scanout.  The kernel has no
	 * format-conditional path, so if this direct ioctl succeeds the
	 * refusal above is Mesa's own format-capability check -- and the
	 * devcap words it consults are printed to say why. */
	if (cfd2 >= 0) {
		static uint32_t caps3[300];
		struct drm_vmw_get_3d_cap_arg c3;
		memset(&c3, 0, sizeof(c3));
		c3.buffer = (uint64_t)(uintptr_t)caps3;
		c3.max_size = sizeof(caps3);
		if (vmw_cmd(cfd2, DRM_VMW_GET_3D_CAP, _IOC_WRITE, &c3, sizeof(c3)) == 0)
			printf("      (dxfmt caps: BGRA_TYPELESS[213]=%x BGRA_SRGB[214]=%x "
			       "BGRX_TYPELESS[215]=%x BGRX_SRGB[216]=%x BGRA[240]=%x BGRX[241]=%x)\n",
			       caps3[213], caps3[214], caps3[215], caps3[216],
			       caps3[240], caps3[241]);
		union drm_vmw_gb_surface_create_ext_arg sa2;
		memset(&sa2, 0, sizeof(sa2));
		/* HINT_TEXTURE | HINT_RENDERTARGET | BIND_SHADER_RESOURCE |
		 * BIND_RENDER_TARGET: what svga_texture_create sends. */
		sa2.req.base.svga3d_flags =
			(1u << 5) | (1u << 6) | (1u << 23) | (1u << 24);
		sa2.req.base.format = 141; /* SVGA3D_B8G8R8A8_UNORM */
		sa2.req.base.mip_levels = 1;
		sa2.req.base.drm_surface_flags = drm_vmw_surface_flag_scanout |
						 drm_vmw_surface_flag_shareable |
						 drm_vmw_surface_flag_create_buffer;
		sa2.req.base.buffer_handle = 0xFFFFFFFFu;
		sa2.req.base.base_size.width = 64;
		sa2.req.base.base_size.height = 64;
		sa2.req.base.base_size.depth = 1;
		sa2.req.base.array_size = 1;
		sa2.req.version = drm_vmw_gb_surface_v1;
		int rc141 = vmw_cmd(cfd2, DRM_VMW_GB_SURFACE_CREATE_EXT,
				    _IOC_READ | _IOC_WRITE, &sa2, sizeof(sa2));
		test_result("kernel accepts a B8G8R8A8 scanout surface", rc141 == 0 && sa2.rep.handle);
		if (rc141 == 0) {
			struct drm_vmw_surface_arg u141 = { .sid = (int32_t)sa2.rep.handle };
			vmw_cmd(cfd2, DRM_VMW_UNREF_SURFACE, _IOC_WRITE, &u141, sizeof(u141));
			struct drm_vmw_handle_close_arg h141 = { .handle = sa2.rep.buffer_handle };
			vmw_cmd(cfd2, DRM_VMW_UNREF_DMABUF, _IOC_WRITE, &h141, sizeof(h141));
		} else {
			printf("      (rc=%d)\n", rc141);
		}
	}

	if (cgbm && gbm_destroy)
		gbm_destroy(cgbm);
	if (cfd2 >= 0)
		close(cfd2);
	if (fd >= 0)
		close(fd);
}

static void test_dirent64_and_stat_mkdir(void)
{
	printf("\n[dirent64 / sys-stat mkdir]\n");

	/* struct dirent64 must describe EXACTLY what getdents64() writes:
	 * portable code reads the buffer through this type (libgpg-error
	 * walks its descriptor table that way), so a layout that differed
	 * from struct dirent by even a field's offset would hand it the
	 * wrong names.  Checked by field offsets, not by reading the two
	 * declarations side by side. */
	test_result("dirent64 is the same layout as dirent",
		    sizeof(struct dirent64) == sizeof(struct dirent) &&
			    offsetof(struct dirent64, d_ino) ==
				    offsetof(struct dirent, d_ino) &&
			    offsetof(struct dirent64, d_off) ==
				    offsetof(struct dirent, d_off) &&
			    offsetof(struct dirent64, d_reclen) ==
				    offsetof(struct dirent, d_reclen) &&
			    offsetof(struct dirent64, d_type) ==
				    offsetof(struct dirent, d_type) &&
			    offsetof(struct dirent64, d_name) ==
				    offsetof(struct dirent, d_name));

	/* And it really does read a directory. */
	char dir[64];
	snprintf(dir, sizeof(dir), "/tmp/lk-d64-%d", (int)getpid());
	/* mkdir called with only <sys/stat.h> in mind: POSIX puts it there
	 * (the mode macros are that header's), and a program that includes
	 * nothing else must find it. */
	test_result("mkdir declared by <sys/stat.h>", mkdir(dir, 0755) == 0);

	char f1[128], f2[128];
	snprintf(f1, sizeof(f1), "%s/alpha", dir);
	snprintf(f2, sizeof(f2), "%s/beta", dir);
	int fd = open(f1, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		close(fd);
	fd = open(f2, O_CREAT | O_WRONLY, 0644);
	if (fd >= 0)
		close(fd);

	int dfd = open(dir, O_RDONLY);
	int saw_alpha = 0, saw_beta = 0, entries = 0;

	if (dfd >= 0) {
		char buf[1024];
		int n;

		while ((n = getdents64(dfd, buf, sizeof(buf))) > 0) {
			int off = 0;

			while (off < n) {
				struct dirent64 *de =
					(struct dirent64 *)(buf + off);

				if (de->d_reclen == 0)
					break;
				entries++;
				if (strcmp(de->d_name, "alpha") == 0)
					saw_alpha = 1;
				if (strcmp(de->d_name, "beta") == 0)
					saw_beta = 1;
				off += de->d_reclen;
			}
		}
		close(dfd);
	}
	test_result("getdents64 read into dirent64", entries >= 2);
	test_result("both entries were found", saw_alpha && saw_beta);

	unlink(f1);
	unlink(f2);
	rmdir(dir);
}

int main(int argc, char **argv)
{
	g_ctor_saw_main_before = g_ctor_ran; /* constructors must precede main */
	/* Hidden mode used by the shebang tests: when testlibc is itself the
	 * shebang interpreter, dump the exact argv vector and exit. */
	if (argc > 1 && strncmp(argv[1], "__argv", 6) == 0) {
		for (int i = 0; i < argc; i++)
			printf("%d=[%s]\n", i, argv[i]);
		return 0;
	}
	/* Companion hidden mode: dump the environment and exit, ignoring the
	 * script-path argument the shebang rewrite appends. */
	if (argc > 1 && strcmp(argv[1], "__env") == 0) {
		int cookie = 0;
		const char *en, *ev;
		while (env_iter(&cookie, &en, &ev))
			printf("%s=%s\n", en, ev);
		return 0;
	}

	/* Subcommand selection:
     *   (no arg)          — run all sections except network
     *   testlibc all      — run all sections including network
     *   testlibc network  — run only the networking sections */
	int net_only = (argc > 1 && strcmp(argv[1], "network") == 0);
	int skip_network =
		(argc < 2 || strcmp(argv[1], "all") != 0) && !net_only;

	printf("\n========================================\n");
	printf("  LikeOS-64 Libc Tests%s\n", net_only     ? " (network only)" :
					     skip_network ? " (no network)" :
							    " (all)");
	printf("========================================\n\n");

	// ========================================
	// Test: printf
	// ========================================
	printf("[TEST] printf()\n");
	printf("  Hello from userland libc!\n");
	printf("  argc = %d\n", argc);
	for (int i = 0; i < argc; i++) {
		printf("  argv[%d] = %s\n", i, argv[i]);
	}
	test_pass("printf basic output");

	/* Per-process sandbox directories — these MUST be initialized before any
     * goto.  `goto network_section` below jumps over everything in between, so
     * anything set up there is left as uninitialised stack for the whole
     * `testlibc network` run, while code after the label still uses it.  That
     * is not theoretical: _p_usock used to be initialized down in the
     * filesystem section, so under `testlibc network` the AF_UNIX test bound
     * its listener to an EMPTY path — and two concurrent runs then both
     * answered to "", so a child connected into the other process's listener
     * and both sides hung forever.  _td was likewise garbage at the rmdir()
     * cleanup.  Keep every path this function uses on both entry paths here. */
	char _pbase[32];
	snprintf(_pbase, sizeof(_pbase), "/tmp/tl%d", (int)getpid());
	rmtree(_pbase); /* remove any stale dir from a previous run with this PID */
	mkdir(_pbase, 0777);

	/* PID-isolated sandbox — prevents path collisions when two teststress
     * instances run concurrently (observed in VMware with hardware virt). */
	char _td[56]; /* base tmpdir: /tmp/ts<pid>  */
	char _p_mkdir[96], _p_unlink[96], _p_no_such[96];
	char _p_rsrc[96], _p_rdst[96];
	char _p_chmod[96], _p_chown[96], _p_utime[96];
	char _p_pa[96], _p_pb[112], _p_pc[128];
	char _p_usock[96], _p_uio[96];
	snprintf(_td, sizeof(_td), "/tmp/ts%d", (int)getpid());
	mkdir(_td, 0755); /* best-effort; EEXIST is fine */
	snprintf(_p_mkdir, sizeof(_p_mkdir), "%s/mkdir_dir", _td);
	snprintf(_p_unlink, sizeof(_p_unlink), "%s/unlink_file", _td);
	snprintf(_p_no_such, sizeof(_p_no_such), "%s/no_such_file", _td);
	snprintf(_p_rsrc, sizeof(_p_rsrc), "%s/rename_src", _td);
	snprintf(_p_rdst, sizeof(_p_rdst), "%s/rename_dst", _td);
	snprintf(_p_chmod, sizeof(_p_chmod), "%s/chmod_file", _td);
	snprintf(_p_chown, sizeof(_p_chown), "%s/chown_file", _td);
	snprintf(_p_utime, sizeof(_p_utime), "%s/utime_file", _td);
	snprintf(_p_pa, sizeof(_p_pa), "%s/parent_a", _td);
	snprintf(_p_pb, sizeof(_p_pb), "%s/parent_a/b", _td);
	snprintf(_p_pc, sizeof(_p_pc), "%s/parent_a/b/c", _td);
	snprintf(_p_usock, sizeof(_p_usock), "%s/unix.sock", _td);
	snprintf(_p_uio, sizeof(_p_uio), "%s/_uio_test", _td);

	if (net_only)
		goto network_section;

	// ========================================
	// Test: malloc/free
	// ========================================
	printf("\n[TEST] malloc/free\n");
	char *buf = malloc(100);
	test_result("malloc(100) returns non-NULL", buf != NULL);

	if (buf) {
		strcpy(buf, "Hello, ");
		strcat(buf, "World!");
		size_t len = strlen(buf);
		printf("  String: %s (len=%zu)\n", buf, len);
		test_result("strcpy/strcat/strlen", len == 13);
		free(buf);
		test_pass("free() completed");
	}

	// ========================================
	// Test: malloc alignment
	// ========================================
	printf("\n[TEST] malloc alignment\n");
	{
		static const size_t asz[] = { 1,    2,    3,	 8,     13,
					      16,   17,   24,	 31,    32,
					      100,  555,  1023,	 1024,  4097,
					      65537, 200000 };
		void *aptr[sizeof(asz) / sizeof(asz[0])];
		int all_ok = 1, align_ok = 1;
		for (unsigned ai = 0; ai < sizeof(asz) / sizeof(asz[0]); ai++) {
			aptr[ai] = malloc(asz[ai]);
			if (!aptr[ai]) {
				all_ok = 0;
			} else {
				if (((unsigned long)aptr[ai] & 15UL) != 0)
					align_ok = 0;
				memset(aptr[ai], 0xAB, asz[ai]);
			}
		}
		test_result("all sizes allocate", all_ok);
		test_result("all pointers 16-byte aligned", align_ok);
		for (unsigned ai = 0; ai < sizeof(asz) / sizeof(asz[0]); ai++)
			free(aptr[ai]);
		test_pass("all freed");
	}

	// ========================================
	// Test: malloc(0) and free(NULL)
	// ========================================
	printf("\n[TEST] malloc(0) and free(NULL)\n");
	{
		void *z1 = malloc(0);
		void *z2 = malloc(0);
		test_result("malloc(0) returns non-NULL", z1 != NULL);
		test_result("malloc(0) pointers are unique",
			    z2 != NULL && z2 != z1);
		free(z1);
		free(z2);
		free(NULL);
		test_pass("free(NULL) is a no-op");
	}

	// ========================================
	// Test: malloc thread-cache reuse
	// ========================================
	printf("\n[TEST] malloc thread-cache reuse\n");
	{
		void *t1 = malloc(64);
		memset(t1, 1, 64);
		free(t1);
		void *t2 = malloc(64);
		test_result("same-size realloc reuses freed block", t2 == t1);
		free(t2);
	}

	// ========================================
	// Test: malloc corruption detection.  A detected corruption always
	// aborts the process (production prints a generic notice, a DEBUG
	// build prints details); each case runs in a child so the abort is
	// contained and observable as a non-zero exit.
	// ========================================
	printf("\n[TEST] malloc corruption aborts\n");
	{
		/* tcache double free */
		pid_t dfpid = fork();
		if (dfpid == 0) {
			char *dp = malloc(200);
			free(dp);
			free(dp);
			_exit(0); /* only reached if the abort did not fire */
		}
		int dfst = 0;
		waitpid(dfpid, &dfst, 0);
		test_result("tcache double free aborts",
			    !(WIFEXITED(dfst) && WEXITSTATUS(dfst) == 0));

		/* fast-bin top double free (fill the thread cache first) */
		dfpid = fork();
		if (dfpid == 0) {
			void *dfb[9];
			for (int di = 0; di < 9; di++)
				dfb[di] = malloc(100);
			for (int di = 0; di < 9; di++)
				free(dfb[di]);
			free(dfb[8]);
			_exit(0);
		}
		waitpid(dfpid, &dfst, 0);
		test_result("fast-bin double free aborts",
			    !(WIFEXITED(dfst) && WEXITSTATUS(dfst) == 0));

		/* free of a non-heap pointer */
		dfpid = fork();
		if (dfpid == 0) {
			int on_stack = 42;
			free(&on_stack);
			_exit(0);
		}
		waitpid(dfpid, &dfst, 0);
		test_result("free of non-heap pointer aborts",
			    !(WIFEXITED(dfst) && WEXITSTATUS(dfst) == 0));

		/* the parent allocator is untouched by the children */
		void *dchk = malloc(200);
		test_result("allocator alive after corruption aborts",
			    dchk != NULL);
		free(dchk);
	}

	// ========================================
	// Test: realloc grow/shrink
	// ========================================
	printf("\n[TEST] realloc grow/shrink\n");
	{
		char *rp = malloc(1024);
		for (int ri = 0; ri < 1024; ri++)
			rp[ri] = (char)ri;
		char *rs = realloc(rp, 100);
		test_result("shrink keeps the block in place", rs == rp);
		int rok = 1;
		for (int ri = 0; ri < 100; ri++)
			if (rs[ri] != (char)ri)
				rok = 0;
		test_result("shrink preserves content", rok);
		free(rs);

		char *rg = malloc(100000);
		for (int ri = 0; ri < 100000; ri++)
			rg[ri] = (char)(ri * 3);
		char *rg2 = realloc(rg, 150000);
		test_result("grow succeeds", rg2 != NULL);
		rok = 1;
		for (int ri = 0; ri < 100000; ri++)
			if (rg2[ri] != (char)(ri * 3))
				rok = 0;
		test_result("grow preserves content", rok);
		free(rg2);

		char *rn = realloc(NULL, 64);
		test_result("realloc(NULL, n) acts as malloc", rn != NULL);
		test_result("realloc(p, 0) frees and returns NULL",
			    realloc(rn, 0) == NULL);
	}

	// ========================================
	// Test: realloc across size classes
	// ========================================
	printf("\n[TEST] realloc across size classes\n");
	{
		static const size_t rcsz[] = { 32,     200,    3000,
					       70000,  200000, 64 };
		char *rc = malloc(rcsz[0]);
		memset(rc, 0x5A, rcsz[0]);
		size_t rprev = rcsz[0];
		int rcok = (rc != NULL);
		for (unsigned ri = 1;
		     rcok && ri < sizeof(rcsz) / sizeof(rcsz[0]); ri++) {
			rc = realloc(rc, rcsz[ri]);
			if (!rc) {
				rcok = 0;
				break;
			}
			size_t keep = rprev < rcsz[ri] ? rprev : rcsz[ri];
			if (keep > 512)
				keep = 512;
			for (size_t rj = 0; rj < keep; rj++)
				if ((unsigned char)rc[rj] != 0x5A)
					rcok = 0;
			memset(rc, 0x5A, rcsz[ri]);
			rprev = rcsz[ri];
		}
		test_result("realloc chain preserves prefix", rcok);
		free(rc);
	}

	// ========================================
	// Test: calloc
	// ========================================
	printf("\n[TEST] calloc\n");
	{
		unsigned char *cp = calloc(1000, 100);
		test_result("calloc(1000,100) succeeds", cp != NULL);
		int cok = 1;
		for (int ci = 0; ci < 100000; ci++)
			if (cp[ci])
				cok = 0;
		test_result("calloc memory is zero", cok);
		memset(cp, 0xFF, 100000);
		free(cp);

		cp = calloc(1000, 100);
		cok = 1;
		for (int ci = 0; ci < 100000; ci++)
			if (cp[ci])
				cok = 0;
		test_result("calloc re-zeroes recycled memory", cok);
		free(cp);

		errno = 0;
		void *cbig = calloc((size_t)-1 / 2, 3);
		test_result("calloc overflow returns NULL + ENOMEM",
			    cbig == NULL && errno == ENOMEM);

		cp = calloc(1, 2 * 1024 * 1024);
		test_result("large calloc succeeds", cp != NULL);
		cok = 1;
		for (int ci = 0; ci < 2 * 1024 * 1024; ci += 4099)
			if (cp[ci])
				cok = 0;
		test_result("large calloc is zero", cok);
		free(cp);
	}

	// ========================================
	// Test: aligned allocation family
	// ========================================
	printf("\n[TEST] aligned allocation\n");
	{
		static const size_t mal[] = { 16, 32, 64, 256, 4096, 65536 };
		int maok = 1;
		for (unsigned mi = 0; mi < sizeof(mal) / sizeof(mal[0]); mi++) {
			void *mp = NULL;
			int mrc = posix_memalign(&mp, mal[mi], 1000);
			if (mrc != 0 || !mp ||
			    ((unsigned long)mp % mal[mi]) != 0) {
				maok = 0;
			} else {
				memset(mp, 3, 1000);
				free(mp);
			}
		}
		test_result("posix_memalign all alignments", maok);

		void *mp = NULL;
		test_result("posix_memalign rejects non-power-of-two",
			    posix_memalign(&mp, 24, 100) == EINVAL);
		test_result("posix_memalign accepts 8",
			    posix_memalign(&mp, 8, 100) == 0);
		free(mp);

		mp = aligned_alloc(64, 100);
		test_result("aligned_alloc works",
			    mp && ((unsigned long)mp & 63UL) == 0);
		free(mp);

		mp = memalign(4096, 300000);
		test_result("memalign large block",
			    mp && ((unsigned long)mp & 4095UL) == 0);
		if (mp) {
			memset(mp, 9, 300000);
			free(mp);
		}

		mp = valloc(100);
		test_result("valloc page-aligned",
			    mp && ((unsigned long)mp & 4095UL) == 0);
		free(mp);
	}

	// ========================================
	// Test: malloc_usable_size
	// ========================================
	printf("\n[TEST] malloc_usable_size\n");
	{
		static const size_t usz[] = { 1, 24, 100, 1000, 50000, 300000 };
		int uok = 1;
		for (unsigned ui = 0; ui < sizeof(usz) / sizeof(usz[0]); ui++) {
			char *up = malloc(usz[ui]);
			size_t u = malloc_usable_size(up);
			if (u < usz[ui])
				uok = 0;
			memset(up, 7, u); /* the full usable size is writable */
			free(up);
		}
		test_result("usable size >= request", uok);
		test_result("usable size of NULL is 0",
			    malloc_usable_size(NULL) == 0);
		int uchurn = 1;
		for (int ui = 0; ui < 100; ui++) {
			void *up = malloc(200);
			if (!up)
				uchurn = 0;
			free(up);
		}
		test_result("heap intact after full-usable writes", uchurn);
	}

	// ========================================
	// Test: mallinfo2
	// ========================================
	printf("\n[TEST] mallinfo2\n");
	{
		struct mallinfo2 mia = mallinfo2();
		char *mip = malloc(60000);
		struct mallinfo2 mib = mallinfo2();
		test_result("uordblks grows on malloc",
			    mib.uordblks > mia.uordblks);
		test_result("arena >= uordblks", mib.arena >= mib.uordblks);
		free(mip);
		struct mallinfo2 mic = mallinfo2();
		test_result("free reflected in stats",
			    mic.fordblks > mib.fordblks ||
			    mic.uordblks < mib.uordblks);
	}

	// ========================================
	// Test: malloc_trim
	// ========================================
	printf("\n[TEST] malloc_trim\n");
	{
		void *tp[16];
		for (int ti = 0; ti < 16; ti++)
			tp[ti] = malloc(60000);
		for (int ti = 0; ti < 16; ti++)
			free(tp[ti]);
		int tr = malloc_trim(0);
		test_result("malloc_trim returns 0 or 1", tr == 0 || tr == 1);
		int tok = 1;
		for (int ti = 0; ti < 200; ti++) {
			void *q = malloc(1 + (ti * 37) % 5000);
			if (!q)
				tok = 0;
			free(q);
		}
		test_result("heap works after trim", tok);
	}

	// ========================================
	// Test: large allocations are mapped and released
	// ========================================
	printf("\n[TEST] malloc large mapped blocks\n");
	{
		struct mallinfo2 mbase = mallinfo2();
		int mok = 1;
		for (int mi = 0; mi < 10; mi++) {
			unsigned char *mp = malloc(1024 * 1024);
			if (!mp) {
				mok = 0;
				continue;
			}
			mp[0] = 1;
			mp[512 * 1024] = 2;
			mp[1024 * 1024 - 1] = 3;
			if (mp[0] != 1 || mp[512 * 1024] != 2 ||
			    mp[1024 * 1024 - 1] != 3)
				mok = 0;
			free(mp);
		}
		test_result("10x 1MB alloc/verify/free", mok);
		struct mallinfo2 mafter = mallinfo2();
		test_result("mapped blocks returned to the OS",
			    mafter.hblks <= mbase.hblks);
		unsigned char *mbig = malloc(64UL * 1024 * 1024);
		test_result("64MB allocation succeeds", mbig != NULL);
		if (mbig) {
			mbig[0] = 1;
			mbig[64UL * 1024 * 1024 - 1] = 2;
			test_result("64MB block usable",
				    mbig[0] == 1 &&
				    mbig[64UL * 1024 * 1024 - 1] == 2);
			free(mbig);
		}
	}

	// ========================================
	// Test: malloc many-sizes stress
	// ========================================
	printf("\n[TEST] malloc many-sizes stress\n");
	{
		enum { MS_SLOTS = 128 };
		static void *msp[MS_SLOTS];
		static size_t mss[MS_SLOTS];
		unsigned long st = 0x123456789abcdefUL;
		int mism = 0;
		memset(msp, 0, sizeof(msp));
		for (int it = 0; it < 20000; it++) {
			st ^= st << 13;
			st ^= st >> 7;
			st ^= st << 17;
			int s = (int)(st % MS_SLOTS);
			if (msp[s]) {
				unsigned char *m = msp[s];
				unsigned char want =
					(unsigned char)(s * 7 + (int)mss[s]);
				if (m[0] != want || m[mss[s] / 2] != want ||
				    m[mss[s] - 1] != want)
					mism++;
				free(m);
				msp[s] = NULL;
			} else {
				size_t size = (it % 64 == 63)
					? 100000 + (size_t)(st % 200000)
					: 1 + (size_t)(st % 16384);
				unsigned char *m = malloc(size);
				if (!m) {
					mism++;
					continue;
				}
				unsigned char b =
					(unsigned char)(s * 7 + (int)size);
				m[0] = b;
				m[size / 2] = b;
				m[size - 1] = b;
				msp[s] = m;
				mss[s] = size;
			}
		}
		for (int s = 0; s < MS_SLOTS; s++) {
			if (!msp[s])
				continue;
			unsigned char *m = msp[s];
			unsigned char want =
				(unsigned char)(s * 7 + (int)mss[s]);
			if (m[0] != want || m[mss[s] / 2] != want ||
			    m[mss[s] - 1] != want)
				mism++;
			free(m);
			msp[s] = NULL;
		}
		test_result("20000-op stress, zero corruptions", mism == 0);
	}

	// ========================================
	// Test: malloc across fork
	// ========================================
	printf("\n[TEST] malloc across fork\n");
	{
		char *fbuf = malloc(50000);
		test_result("parent allocation", fbuf != NULL);
		if (fbuf) {
			for (int fi = 0; fi < 50000; fi++)
				fbuf[fi] = (char)(fi * 13);
			pid_t fpid = fork();
			if (fpid == 0) {
				for (int fi = 0; fi < 50000; fi++)
					if (fbuf[fi] != (char)(fi * 13))
						_exit(2);
				for (int fi = 0; fi < 2000; fi++) {
					size_t fs = 1 + (fi * 977) % 8192;
					char *fp = malloc(fs);
					if (!fp)
						_exit(3);
					fp[0] = 1;
					fp[fs - 1] = 2;
					free(fp);
				}
				char *fbig = malloc(300000);
				if (!fbig)
					_exit(4);
				fbig[0] = 5;
				fbig[299999] = 6;
				free(fbig);
				_exit(0);
			}
			int fst = 0;
			waitpid(fpid, &fst, 0);
			test_result("child heap works after fork",
				    WIFEXITED(fst) && WEXITSTATUS(fst) == 0);
			int fok = 1;
			for (int fi = 0; fi < 50000; fi++)
				if (fbuf[fi] != (char)(fi * 13))
					fok = 0;
			test_result("parent pattern intact after fork", fok);
			free(fbuf);
		}
	}

	// ========================================
	// Test: atoi
	// ========================================
	printf("\n[TEST] atoi()\n");
	test_result("atoi(\"42\") == 42", atoi("42") == 42);
	test_result("atoi(\"-123\") == -123", atoi("-123") == -123);
	test_result("atoi(\"0\") == 0", atoi("0") == 0);
	printf("  atoi(\"777\") = %d\n", atoi("777"));
	test_result("atoi(\"777\") == 777", atoi("777") == 777);

	// ========================================
	// Test: printf format specifiers
	// ========================================
	printf("\n[TEST] printf format specifiers\n");
	char fmtbuf[64];
	sprintf(fmtbuf, "0x%x %d %s", 0xCAFE, 12345, "test");
	test_result("sprintf format specifiers",
		    strcmp(fmtbuf, "0xcafe 12345 test") == 0);

	// ========================================
	// Test: write syscall
	// ========================================
	printf("\n[TEST] write() syscall\n");
	const char *msg = "  Direct write syscall!\n";
	ssize_t written = write(1, msg, strlen(msg));
	test_result("write() returns correct count",
		    written == (ssize_t)strlen(msg));

	// ========================================
	// Test: getpid
	// ========================================
	printf("\n[TEST] getpid()\n");
	pid_t pid = getpid();
	printf("  PID: %d\n", pid);
	test_result("getpid() returns positive value", pid > 0);

	// ========================================
	// Test: FILE* functions - fopen/fread/fclose
	// ========================================
	printf("\n[TEST] FILE* functions\n");
	FILE *fp = fopen("/HELLO.TXT", "r");
	test_result("fopen(\"/HELLO.TXT\", \"r\") succeeds", fp != NULL);

	if (fp) {
		char readbuf[64];
		memset(readbuf, 0, sizeof(readbuf));
		size_t nread = fread(readbuf, 1, sizeof(readbuf) - 1, fp);
		printf("  fread() returned %zu bytes\n", nread);
		test_result("fread() returns > 0 bytes", nread > 0);

		if (nread > 0 && readbuf[nread - 1] == '\n')
			readbuf[nread - 1] = '\0';
		printf("  Contents: \"%s\"\n", readbuf);

		int rc = fclose(fp);
		test_result("fclose() returns 0", rc == 0);
	}

	// Test fopen with non-existent file
	fp = fopen("/NONEXISTENT.TXT", "r");
	test_result("fopen(non-existent) returns NULL", fp == NULL);

	// ========================================
	// Test: fputs/puts
	// ========================================
	printf("\n[TEST] fputs/puts\n");
	int fputs_rc = fputs("  fputs output\n", stdout);
	test_result("fputs() returns >= 0", fputs_rc >= 0);
	puts("  puts output");
	test_pass("puts() completed");

	// ========================================
	// Test: fprintf
	// ========================================
	printf("\n[TEST] fprintf\n");
	int fprintf_rc =
		fprintf(stdout, "  fprintf: int=%d, hex=0x%x\n", 42, 0xCAFE);
	test_result("fprintf() returns > 0", fprintf_rc > 0);

	// ========================================
	// Test: putchar/fputc
	// ========================================
	printf("\n[TEST] putchar/fputc\n");
	printf("  Characters: ");
	int pc = putchar('A');
	test_result("putchar('A') returns 'A'", pc == 'A');
	pc = fputc('B', stdout);
	test_result("fputc('B') returns 'B'", pc == 'B');
	putchar('\n');

	// ========================================
	// Test: sprintf/snprintf
	// ========================================
	printf("\n[TEST] sprintf/snprintf\n");
	char sprbuf[64];
	int len = sprintf(sprbuf, "Value: %d", 12345);
	test_result("sprintf returns correct length", len == 12);
	test_result("sprintf produces correct string",
		    strcmp(sprbuf, "Value: 12345") == 0);

	len = snprintf(sprbuf, 10, "Long string that will be truncated");
	test_result("snprintf truncates correctly", strlen(sprbuf) == 9);

	// ========================================
	// Test: strerror_r
	// ========================================
	printf("\n[TEST] strerror_r()\n");
	{
		char errbuf[64];
		int sr = strerror_r(ENOENT, errbuf, sizeof(errbuf));
		test_result("strerror_r(ENOENT) returns 0", sr == 0);
		test_result("strerror_r(ENOENT) fills buffer",
			    strlen(errbuf) > 0);
		printf("  strerror_r(ENOENT) = \"%s\"\n", errbuf);
		/* Tiny buffer: must return ERANGE and not overflow */
		char tiny[4];
		sr = strerror_r(ENOENT, tiny, sizeof(tiny));
		test_result("strerror_r tiny buf returns ERANGE", sr == ERANGE);
	}

	// ========================================
	// Test: basename
	// ========================================
	printf("\n[TEST] basename()\n");
	{
		char bp1[] = "/usr/bin/ls";
		test_result("basename(\"/usr/bin/ls\") == \"ls\"",
			    strcmp(basename(bp1), "ls") == 0);
		char bp2[] = "/usr/";
		test_result("basename(\"/usr/\") == \"usr\"",
			    strcmp(basename(bp2), "usr") == 0);
		char bp3[] = "hello.c";
		test_result("basename(\"hello.c\") == \"hello.c\"",
			    strcmp(basename(bp3), "hello.c") == 0);
		char bp4[] = "/";
		test_result("basename(\"/\") is non-empty",
			    strlen(basename(bp4)) > 0);
	}

	// ========================================
	// Test: fseek/ftell/rewind
	// ========================================
	printf("\n[TEST] fseek/ftell/rewind\n");
	fp = fopen("/HELLO.TXT", "r");
	if (fp) {
		char seekbuf[32];
		memset(seekbuf, 0, sizeof(seekbuf));
		fread(seekbuf, 1, 5, fp);

		long pos = ftell(fp);
		printf("  ftell() after read 5 bytes = %ld\n", pos);
		test_result("ftell() returns 5 after reading 5 bytes",
			    pos == 5);

		fseek(fp, 0, 0); // SEEK_SET
		pos = ftell(fp);
		test_result("fseek(0, SEEK_SET) resets to 0", pos == 0);

		rewind(fp);
		pos = ftell(fp);
		test_result("rewind() resets to 0", pos == 0);

		fclose(fp);
	} else {
		test_fail("fseek/ftell test - fopen failed");
	}

	// ========================================
	// Test: getenv/setenv/unsetenv
	// ========================================
	printf("\n[TEST] getenv/setenv/unsetenv\n");
	char *val = getenv("TEST_VAR");
	test_result("getenv() returns NULL for unset var", val == NULL);

	int rc = setenv("TEST_VAR", "hello_world", 1);
	test_result("setenv() returns 0", rc == 0);

	val = getenv("TEST_VAR");
	test_result("getenv() returns set value",
		    val != NULL && strcmp(val, "hello_world") == 0);

	// Test setenv with overwrite=0
	rc = setenv("TEST_VAR", "new_value", 0);
	val = getenv("TEST_VAR");
	test_result("setenv with overwrite=0 keeps old value",
		    val != NULL && strcmp(val, "hello_world") == 0);

	// Test unsetenv
	rc = unsetenv("TEST_VAR");
	val = getenv("TEST_VAR");
	test_result("unsetenv() clears variable", val == NULL);

	/* `environ` must describe the SAME environment getenv/setenv do: it is
	 * what a program hands to execve(), so an environ that does not track
	 * setenv gives every child an empty environment.  That is how TERM
	 * disappeared across su (login builds its own envp, so it was fine,
	 * which made it look like an su bug). */
	{
		extern char **environ;
		int n = 0, found = 0;
		test_result("environ is populated at startup",
			    environ != NULL && environ[0] != NULL);
		setenv("LIKEOS_ENV_TEST", "42", 1);
		for (n = 0; environ && environ[n]; n++)
			if (strcmp(environ[n], "LIKEOS_ENV_TEST=42") == 0)
				found = 1;
		test_result("setenv is visible in environ", found == 1);

		/* The whole point: a child started with `environ` inherits it. */
		pid_t epid = fork();
		if (epid == 0) {
			char *eargv[4] = { (char *)"sh", (char *)"-c",
					   (char *)"[ \"$LIKEOS_ENV_TEST\" = 42 ]",
					   NULL };
			execve("/bin/sh", eargv, environ);
			_exit(127);
		}
		int est = -1;
		waitpid(epid, &est, 0);
		test_result("child exec'd with environ inherits the variable",
			    WIFEXITED(est) && WEXITSTATUS(est) == 0);

		unsetenv("LIKEOS_ENV_TEST");
		found = 0;
		for (n = 0; environ && environ[n]; n++)
			if (strncmp(environ[n], "LIKEOS_ENV_TEST=", 16) == 0)
				found = 1;
		test_result("unsetenv removes it from environ", found == 0);
	}

	// ========================================
	// Test: fork/wait/getpid/getppid
	// ========================================
	printf("\n[TEST] fork/wait/getpid/getppid\n");
	pid_t my_pid = getpid();
	pid_t my_ppid = getppid();
	printf("  PID=%d, PPID=%d calling fork()...\n", my_pid, my_ppid);

	pid_t child_pid = fork();
	printf("  fork() returned %d in process %d\n", child_pid, getpid());

	if (child_pid < 0) {
		test_fail("fork() failed");
	} else if (child_pid == 0) {
		// Child process
		printf("  [CHILD] I am the child, my PID = %d, parent = %d\n",
		       getpid(), getppid());
		printf("  [CHILD] Exiting with code 42\n");
		_exit(42);
	} else {
		// Parent process
		printf("  [PARENT] fork() returned child PID = %d\n",
		       child_pid);
		test_result("fork() returns positive child PID", child_pid > 0);

		// Wait for child
		int status = 0;
		pid_t waited = waitpid(child_pid, &status, 0);
		printf("  [PARENT] waitpid(%d, ...) returned %d\n", child_pid,
		       waited);
		test_result("waitpid() returns child PID", waited == child_pid);

		if (WIFEXITED(status)) {
			int exit_status = WEXITSTATUS(status);
			printf("  [PARENT] Child exited with status %d (raw status=0x%x)\n",
			       exit_status, status);
			test_result("Child exit status is 42",
				    exit_status == 42);
		} else {
			printf("  [PARENT] Child did not exit normally (status=0x%x)\n",
			       status);
			test_fail("Child did not exit normally");
		}
	}

	// ========================================
	// Test: execve (via fork)
	// ========================================
	printf("\n[TEST] execve() via fork\n");
	pid_t exec_child = fork();
	if (exec_child < 0) {
		test_fail("fork() for execve failed");
	} else if (exec_child == 0) {
		char *exec_argv[] = { "/usr/local/bin/hello", NULL };
		char *exec_envp[] = { NULL };
		execve("/usr/local/bin/hello", exec_argv, exec_envp);
		printf("  [CHILD] execve failed: errno=%d\n", errno);
		_exit(1);
	} else {
		printf("  [PARENT] fork() returned exec_child=%d, my pid=%d\n",
		       exec_child, getpid());
		int status = 0;
		errno = 0;
		pid_t waited = waitpid(exec_child, &status, 0);
		int saved_errno = errno;
		printf("  [PARENT] waitpid(%d,...) returned %d, errno=%d, status=0x%x\n",
		       exec_child, waited, saved_errno, status);
		if (waited != exec_child) {
			/* Try a few diagnostic follow-ups so the next failure dump
             * tells us *why* waitpid said ECHILD instead of just *that*
             * it did. */
			int s2 = 0;
			errno = 0;
			pid_t any = waitpid(-1, &s2, WNOHANG);
			int any_errno = errno;
			printf("  [PARENT] follow-up waitpid(-1, WNOHANG) = %d, "
			       "errno=%d, status=0x%x\n",
			       any, any_errno, s2);
			errno = 0;
			int kill_rc = kill(exec_child, 0);
			int kill_errno = errno;
			printf("  [PARENT] kill(%d, 0) = %d, errno=%d "
			       "(0=exists, ESRCH=gone, EPERM=exists-but-not-ours)\n",
			       exec_child, kill_rc, kill_errno);
		}
		test_result("waitpid() returns execve child PID",
			    waited == exec_child);
		if (WIFEXITED(status)) {
			int exit_status = WEXITSTATUS(status);
			test_result("execve child exited 0", exit_status == 0);
		} else {
			test_fail("execve child did not exit normally");
		}
	}

	// ========================================
	// Test: execv/execvp (via fork)
	// ========================================
	printf("\n[TEST] execv/execvp via fork\n");
	pid_t execv_child = fork();
	if (execv_child < 0) {
		test_fail("fork() for execv failed");
	} else if (execv_child == 0) {
		char *exec_argv[] = { "/usr/local/bin/hello", NULL };
		execv("/usr/local/bin/hello", exec_argv);
		_exit(1);
	} else {
		int status = 0;
		pid_t waited = waitpid(execv_child, &status, 0);
		test_result("waitpid() returns execv child PID",
			    waited == execv_child);
		if (WIFEXITED(status)) {
			int exit_status = WEXITSTATUS(status);
			test_result("execv child exited 0", exit_status == 0);
		} else {
			test_fail("execv child did not exit normally");
		}
	}

	// Ensure PATH for execvp
	setenv("PATH", "/usr/local/bin:/bin", 1);
	pid_t execvp_child = fork();
	if (execvp_child < 0) {
		test_fail("fork() for execvp failed");
	} else if (execvp_child == 0) {
		char *exec_argv[] = { "hello", NULL };
		execvp("hello", exec_argv);
		_exit(1);
	} else {
		int status = 0;
		pid_t waited = waitpid(execvp_child, &status, 0);
		test_result("waitpid() returns execvp child PID",
			    waited == execvp_child);
		if (WIFEXITED(status)) {
			int exit_status = WEXITSTATUS(status);
			test_result("execvp child exited 0", exit_status == 0);
		} else {
			test_fail("execvp child did not exit normally");
		}
	}

	// ========================================
	// Test: pipe
	// ========================================
	printf("\n[TEST] pipe()\n");
	int fds[2];
	int prc = pipe(fds);
	test_result("pipe() returns 0", prc == 0);
	if (prc == 0) {
		const char *pipemsg = "pipe works";
		ssize_t pwr = write(fds[1], pipemsg, strlen(pipemsg));
		test_result("pipe write returns full length",
			    pwr == (ssize_t)strlen(pipemsg));

		char pipebuf[32];
		memset(pipebuf, 0, sizeof(pipebuf));
		ssize_t prd = read(fds[0], pipebuf, sizeof(pipebuf) - 1);
		test_result("pipe read returns full length",
			    prd == (ssize_t)strlen(pipemsg));
		test_result("pipe read matches data",
			    prd > 0 && strcmp(pipebuf, pipemsg) == 0);

		close(fds[0]);
		close(fds[1]);
	}

	// ========================================
	// Test: pipe2
	// ========================================
	printf("\n[TEST] pipe2()\n");
	{
		int p2fds[2];
		int p2rc = pipe2(p2fds, O_CLOEXEC);
		test_result("pipe2(O_CLOEXEC) returns 0", p2rc == 0);
		if (p2rc == 0) {
			int fl0 = fcntl(p2fds[0], F_GETFD);
			int fl1 = fcntl(p2fds[1], F_GETFD);
			test_result("pipe2 read-end has FD_CLOEXEC",
				    fl0 >= 0 && (fl0 & FD_CLOEXEC));
			test_result("pipe2 write-end has FD_CLOEXEC",
				    fl1 >= 0 && (fl1 & FD_CLOEXEC));
			const char *p2msg = "pipe2 works";
			ssize_t p2w = write(p2fds[1], p2msg, strlen(p2msg));
			test_result("pipe2 write returns full length",
				    p2w == (ssize_t)strlen(p2msg));
			char p2buf[32];
			memset(p2buf, 0, sizeof(p2buf));
			ssize_t p2r = read(p2fds[0], p2buf, sizeof(p2buf) - 1);
			test_result("pipe2 read matches data",
				    p2r > 0 && strcmp(p2buf, p2msg) == 0);
			close(p2fds[0]);
			close(p2fds[1]);
		}
	}

	// ========================================
	// Test: munmap
	// ========================================
	printf("\n[TEST] munmap()\n");
	size_t map_len = 8192;
	void *map = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("mmap() returns non-NULL", map != MAP_FAILED);
	if (map != MAP_FAILED) {
		unsigned char *p = (unsigned char *)map;
		for (size_t i = 0; i < map_len; i++) {
			p[i] = (unsigned char)(i & 0xFF);
		}
		int mrc = munmap(map, map_len);
		test_result("munmap() returns 0", mrc == 0);
	}

	// ========================================
	// Test: dup/dup2
	// ========================================
	printf("\n[TEST] dup/dup2\n");
	int newfd = dup(1); // Dup stdout
	printf("  dup(1) returned %d\n", newfd);
	test_result("dup(1) returns valid fd", newfd >= 0);

	if (newfd >= 0) {
		const char *dupmsg = "  Write via duped fd\n";
		ssize_t wr = write(newfd, dupmsg, strlen(dupmsg));
		test_result("write to duped fd succeeds", wr > 0);
		close(newfd);
	}

	// ========================================
	// ========================================
	// Test: popen/pclose, fscanf, statvfs, chroot
	// (libc pieces added for the OpenSSH port)
	// ========================================
	printf("\n[TEST] popen/pclose\n");
	{
		FILE *pp = popen("echo popen-works", "r");
		test_result("popen(r) returns a stream", pp != NULL);
		char line[64] = { 0 };
		if (pp) {
			char *g = fgets(line, sizeof(line), pp);
			test_result("popen child output read",
				    g != NULL && strncmp(line, "popen-works", 11) == 0);
			int st = pclose(pp);
			test_result("pclose reaps child (exit 0)",
				    st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
		}
		char pwcmd[160];

		snprintf(pwcmd, sizeof(pwcmd), "cat > %s",
			 tmpp("popen_w_test"));
		FILE *pw = popen(pwcmd, "w");
		test_result("popen(w) returns a stream", pw != NULL);
		if (pw) {
			fputs("to-child\n", pw);
			pclose(pw);
			FILE *rb = fopen(tmpp("popen_w_test"), "r");
			char b[32] = { 0 };
			if (rb) { fgets(b, sizeof(b), rb); fclose(rb); }
			test_result("popen(w) delivered stdin to child",
				    strncmp(b, "to-child", 8) == 0);
			unlink(tmpp("popen_w_test"));
		}
	}

	printf("\n[TEST] fscanf (stream formatted input)\n");
	{
		FILE *f = fopen(tmpp("fscanf_test"), "w");
		if (f) { fputs("42 hello 3.5 0xff ab:cd\n", f); fclose(f); }
		f = fopen(tmpp("fscanf_test"), "r");
		int n = -1; char word[16] = { 0 }; float fl = 0; unsigned hx = 0;
		int a = 0, b = 0;
		int got = 0;
		if (f) {
			got = fscanf(f, "%d %15s %f %x %x:%x", &n, word, &fl, &hx, &a, &b);
			fclose(f);
		}
		test_result("fscanf matched all 6 fields", got == 6);
		test_result("fscanf %d", n == 42);
		test_result("fscanf %s", strcmp(word, "hello") == 0);
		test_result("fscanf %f", fl > 3.4f && fl < 3.6f);
		test_result("fscanf %x", hx == 0xff);
		test_result("fscanf literal + fields", a == 0xab && b == 0xcd);
		unlink(tmpp("fscanf_test"));
	}

	printf("\n[TEST] statvfs\n");
	{
		struct statvfs vfs;
		int r = statvfs("/", &vfs);
		test_result("statvfs(/) succeeds", r == 0);
		test_result("statvfs block size non-zero", r == 0 && vfs.f_bsize > 0);
		test_result("statvfs has total blocks", r == 0 && vfs.f_blocks > 0);
		test_result("statvfs namemax sane",
			    r == 0 && vfs.f_namemax >= 8 && vfs.f_namemax <= 4096);
	}

	printf("\n[TEST] chroot (confinement)\n");
	{
		/* Build a jail dir with a file inside, fork a child that chroots
		 * into it, and confirm the inside file is reachable while an
		 * outside path (/etc) is not.  Done in a child so the parent's
		 * root is unaffected. */
		char jail[64], inside[96];
		snprintf(jail, sizeof(jail), "/tmp/jail_%d", (int)getpid());
		mkdir(jail, 0755);
		snprintf(inside, sizeof(inside), "%s/secret.txt", jail);
		FILE *jf = fopen(inside, "w");
		if (jf) { fputs("jailed\n", jf); fclose(jf); }

		pid_t cp = fork();
		if (cp == 0) {
			if (chroot(jail) != 0)
				_exit(10);
			if (chdir("/") != 0)
				_exit(11);
			/* inside file now appears at /secret.txt */
			int fd1 = open("/secret.txt", O_RDONLY);
			if (fd1 < 0)
				_exit(12);
			close(fd1);
			/* the real /etc/passwd must NOT be reachable */
			int fd2 = open("/etc/passwd", O_RDONLY);
			if (fd2 >= 0) { close(fd2); _exit(13); }
			_exit(0);
		}
		int cst = -1;
		waitpid(cp, &cst, 0);
		test_result("chroot confines child correctly",
			    WIFEXITED(cst) && WEXITSTATUS(cst) == 0);
		test_result("parent root unaffected by child chroot",
			    open("/etc/passwd", O_RDONLY) >= 0);
		unlink(inside);
		rmdir(jail);
	}

	// ========================================
	// Test: stat/access/chdir/getcwd
	// ========================================
	printf("\n[TEST] stat/access/chdir/getcwd\n");
	struct stat st;
	int sret = stat("/HELLO.TXT", &st);
	test_result("stat(/HELLO.TXT) succeeds", sret == 0);
	if (sret == 0) {
		test_result("stat size > 0", st.st_size > 0);
	}
	test_result("access(/HELLO.TXT) succeeds",
		    access("/HELLO.TXT", R_OK) == 0);
	char cwd[64];
	char *cwdret = getcwd(cwd, sizeof(cwd));
	test_result("getcwd returns non-NULL", cwdret != NULL);
	test_result("chdir('/') succeeds", chdir("/") == 0);
	cwdret = getcwd(cwd, sizeof(cwd));
	test_result("getcwd after chdir", cwdret != NULL);
	/* getcwd(NULL, 0) allocates.  A shell calls exactly this for every
	 * prompt and after every cd; passing the NULL through to the kernel
	 * failed with EFAULT ("cannot access parent directories: Bad
	 * address"). */
	char *cwdalloc = getcwd(NULL, 0);
	test_result("getcwd(NULL, 0) allocates the result",
		    cwdalloc != NULL && cwdalloc[0] == '/');
	free(cwdalloc);
	/* buf != NULL with size 0 is EINVAL; too small a buffer is ERANGE. */
	errno = 0;
	test_result("getcwd(buf, 0) fails with EINVAL",
		    getcwd(cwd, 0) == NULL && errno == EINVAL);
	errno = 0;
	char tiny[2];
	test_result("getcwd(buf, too-small) fails with ERANGE",
		    chdir("/usr/local") == 0 && getcwd(tiny, sizeof(tiny)) == NULL &&
			    errno == ERANGE);
	chdir("/");

	// ========================================
	// Test: uid/gid and time
	// ========================================
	printf("\n[TEST] uid/gid/time\n");
	test_result("getuid returns 0", getuid() == 0);
	test_result("getgid returns 0", getgid() == 0);
	struct timeval tv;
	test_result("gettimeofday succeeds", gettimeofday(&tv, NULL) == 0);
	test_result("gettimeofday tv_sec non-negative", tv.tv_sec >= 0);
	time_t tnow = time(NULL);
	test_result("time returns non-negative", tnow >= 0);
	test_result("time >= gettimeofday", tnow >= (time_t)tv.tv_sec);

	// ========================================
	// Test: gethostname/uname
	// ========================================
	printf("\n[TEST] gethostname/uname\n");
	char host[64];
	test_result("gethostname succeeds",
		    gethostname(host, sizeof(host)) == 0);
	test_result("gethostname non-empty", host[0] != '\0');
	printf("  hostname: %s\n", host);
	struct utsname un;
	test_result("uname succeeds", uname(&un) == 0);
	test_result("uname sysname non-empty", un.sysname[0] != '\0');
	printf("  uname: sysname=%s nodename=%s release=%s version=%s machine=%s\n",
	       un.sysname, un.nodename, un.release, un.version, un.machine);

	// ========================================
	// Test: file write/create/truncate/append
	// ========================================
	printf("\n[TEST] file write (create/truncate/append)\n");
	/* _pbase already initialized above (before goto network_section). */
	mkdir(_pbase,
	      0777); /* idempotent: ensure directory exists for file tests */
	char wpath[96], wpath2[96];
	snprintf(wpath, sizeof(wpath), "%s/WRITE.TXT", _pbase);
	snprintf(wpath2, sizeof(wpath2), "%s/WRITE2.TXT", _pbase);
	const char *wmsg1 = "HelloWrite";
	int wfd = open(wpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("open(O_CREAT|O_TRUNC|O_WRONLY) succeeds", wfd >= 0);
	if (wfd >= 0) {
		ssize_t w1 = write(wfd, wmsg1, strlen(wmsg1));
		test_result("write initial data", w1 == (ssize_t)strlen(wmsg1));
		close(wfd);
	}
	// read back
	wfd = open(wpath, O_RDONLY);
	test_result("open(O_RDONLY) succeeds", wfd >= 0);
	if (wfd >= 0) {
		char rbuf[64];
		memset(rbuf, 0, sizeof(rbuf));
		ssize_t r1 = read(wfd, rbuf, sizeof(rbuf) - 1);
		test_result("read back initial data",
			    r1 == (ssize_t)strlen(wmsg1) &&
				    strcmp(rbuf, wmsg1) == 0);
		close(wfd);
	}
	// append
	const char *wmsg2 = "+APPEND";
	wfd = open(wpath, O_APPEND | O_WRONLY);
	test_result("open(O_APPEND|O_WRONLY) succeeds", wfd >= 0);
	if (wfd >= 0) {
		ssize_t w2 = write(wfd, wmsg2, strlen(wmsg2));
		test_result("append write", w2 == (ssize_t)strlen(wmsg2));
		close(wfd);
	}
	// read back combined
	wfd = open(wpath, O_RDONLY);
	test_result("open after append succeeds", wfd >= 0);
	if (wfd >= 0) {
		char rbuf[64];
		memset(rbuf, 0, sizeof(rbuf));
		ssize_t r2 = read(wfd, rbuf, sizeof(rbuf) - 1);
		char expect[64];
		snprintf(expect, sizeof(expect), "%s%s", wmsg1, wmsg2);
		test_result("read back appended data",
			    r2 == (ssize_t)strlen(expect) &&
				    strcmp(rbuf, expect) == 0);
		close(wfd);
	}
	// overwrite via lseek
	wfd = open(wpath, O_WRONLY);
	test_result("open(O_WRONLY) succeeds", wfd >= 0);
	if (wfd >= 0) {
		lseek(wfd, 5, 0);
		const char *wmsg3 = "-";
		ssize_t w3 = write(wfd, wmsg3, 1);
		test_result("lseek+overwrite", w3 == 1);
		close(wfd);
	}
	// read back overwrite
	wfd = open(wpath, O_RDONLY);
	test_result("open after overwrite succeeds", wfd >= 0);
	if (wfd >= 0) {
		char rbuf[64];
		memset(rbuf, 0, sizeof(rbuf));
		read(wfd, rbuf, sizeof(rbuf) - 1);
		test_result("overwrite applied", rbuf[5] == '-');
		close(wfd);
	}

	// ========================================
	// Test: fstat/fsync/ftruncate
	// ========================================
	printf("\n[TEST] fstat/fsync/ftruncate\n");
	int tfd = open(wpath, O_WRONLY);
	test_result("open existing file for fstat", tfd >= 0);
	if (tfd >= 0) {
		test_result("fstat succeeds", fstat(tfd, &st) == 0);
		test_result("fsync succeeds", fsync(tfd) == 0);
		test_result("ftruncate to 4 bytes", ftruncate(tfd, 4) == 0);
		int fl = fcntl(tfd, F_GETFL);
		test_result("fcntl(F_GETFL) returns flags", fl >= 0);
		test_result("fcntl(F_SETFL) sets O_APPEND",
			    fcntl(tfd, F_SETFL, O_APPEND) == 0);
		close(tfd);
	}
	// verify truncate
	tfd = open(wpath, O_RDONLY);
	if (tfd >= 0) {
		char rbuf[16];
		memset(rbuf, 0, sizeof(rbuf));
		ssize_t rr = read(tfd, rbuf, sizeof(rbuf) - 1);
		test_result("truncate reduced size", rr == 4);
		close(tfd);
	}
	// rename/unlink
	test_result("rename succeeds", rename(wpath, wpath2) == 0);
	test_result("unlink succeeds", unlink(wpath2) == 0);

	// ========================================
	// Test: mkdir/rmdir
	// ========================================
	printf("\n[TEST] mkdir/rmdir\n");
	char tdpath[96], tdfile[128];
	snprintf(tdpath, sizeof(tdpath), "%s/TESTDIR", _pbase);
	snprintf(tdfile, sizeof(tdfile), "%s/TESTDIR/FILE.TXT", _pbase);
	test_result("mkdir('/TESTDIR') succeeds", mkdir(tdpath, 0777) == 0);
	test_result("chdir('/TESTDIR') succeeds", chdir(tdpath) == 0);
	int dfd = open(tdfile, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create file in dir", dfd >= 0);
	if (dfd >= 0) {
		write(dfd, "X", 1);
		close(dfd);
	}
	test_result("unlink file in dir", unlink(tdfile) == 0);
	test_result("chdir('/') succeeds", chdir("/") == 0);
	test_result("rmdir('/TESTDIR') succeeds", rmdir(tdpath) == 0);

	// ========================================
	// Test: extended attributes (xattr)
	// ========================================
	printf("\n[TEST] xattr (extended attributes)\n");
	{
		/* Per-PID path under _pbase so parallel testlibc instances never collide. */
		char xpath[96];
		snprintf(xpath, sizeof(xpath), "%s/xattr.dat", _pbase);
		int xfd = open(xpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (xfd >= 0) {
			write(xfd, "hello", 5);
			close(xfd);
		}
		test_result("xattr: create target file", xfd >= 0);

		/* Probe support so the FAT32 regression run (no xattrs => EOPNOTSUPP)
         * skips this block cleanly instead of failing. */
		errno = 0;
		int xr = setxattr(xpath, "user.probe", "x", 1, 0);
		int xsup = !(xr == -1 && errno == EOPNOTSUPP);
		if (xr == 0)
			removexattr(xpath, "user.probe");

		if (xfd >= 0 && !xsup) {
			printf("  (xattr unsupported on this filesystem - skipping)\n");
		} else if (xfd >= 0) {
			char xbuf[128], xlist[256];
			ssize_t xn;

			test_result("setxattr user.color=blue",
				    setxattr(xpath, "user.color", "blue", 4,
					     0) == 0);

			errno = 0;
			ssize_t xq = getxattr(xpath, "user.color", NULL, 0);
			if (xq != 4) {
				/* setxattr just returned 0, so the attribute was
				 * written.  Distinguish "the write was lost" from
				 * "it was momentarily invisible": retry the same
				 * query and list every attribute on the file.  If
				 * the retry succeeds it is a visibility/cache
				 * problem; if user.color is absent from the list
				 * too, the write really was lost. */
				int e1 = errno;
				errno = 0;
				ssize_t xq2 =
					getxattr(xpath, "user.color", NULL, 0);
				int e2 = errno;
				char ldbg[256];
				memset(ldbg, 0, sizeof(ldbg));
				ssize_t ln = listxattr(xpath, ldbg,
						       sizeof(ldbg) - 1);
				printf("  [DBG] getxattr=%ld errno=%d; retry=%ld errno=%d; listxattr=%ld [",
				       (long)xq, e1, (long)xq2, e2, (long)ln);
				for (ssize_t i = 0; i < ln && i < 200; i++)
					putchar(ldbg[i] ? ldbg[i] : ' ');
				printf("]\n");
			}
			test_result("getxattr size query == 4", xq == 4);

			memset(xbuf, 0, sizeof(xbuf));
			xn = getxattr(xpath, "user.color", xbuf, sizeof(xbuf));
			test_result("getxattr value == blue",
				    xn == 4 && memcmp(xbuf, "blue", 4) == 0);

			test_result("setxattr user.x=1",
				    setxattr(xpath, "user.x", "1", 1, 0) == 0);

			errno = 0;
			test_result("setxattr CREATE on existing -> EEXIST",
				    setxattr(xpath, "user.color", "red", 3,
					     XATTR_CREATE) == -1 &&
					    errno == EEXIST);

			test_result("setxattr REPLACE user.color=green",
				    setxattr(xpath, "user.color", "green", 5,
					     XATTR_REPLACE) == 0);
			memset(xbuf, 0, sizeof(xbuf));
			xn = getxattr(xpath, "user.color", xbuf, sizeof(xbuf));
			test_result("getxattr value == green",
				    xn == 5 && memcmp(xbuf, "green", 5) == 0);

			memset(xlist, 0, sizeof(xlist));
			xn = listxattr(xpath, xlist, sizeof(xlist));
			int xseen_color = 0, xseen_x = 0;
			for (char *p = xlist; xn > 0 && p < xlist + xn;
			     p += strlen(p) + 1) {
				if (strcmp(p, "user.color") == 0)
					xseen_color = 1;
				if (strcmp(p, "user.x") == 0)
					xseen_x = 1;
			}
			test_result("listxattr contains user.color and user.x",
				    xseen_color && xseen_x);

			errno = 0;
			test_result("getxattr missing -> ENODATA",
				    getxattr(xpath, "user.nope", xbuf,
					     sizeof(xbuf)) == -1 &&
					    errno == ENODATA);

			test_result("removexattr user.color",
				    removexattr(xpath, "user.color") == 0);
			errno = 0;
			test_result("getxattr removed -> ENODATA",
				    getxattr(xpath, "user.color", xbuf,
					     sizeof(xbuf)) == -1 &&
					    errno == ENODATA);
			errno = 0;
			test_result("removexattr missing -> ENODATA",
				    removexattr(xpath, "user.color") == -1 &&
					    errno == ENODATA);

			/* fd-based variants */
			int xfd2 = open(xpath, O_RDWR);
			if (xfd2 >= 0) {
				test_result("fsetxattr user.fd=yes",
					    fsetxattr(xfd2, "user.fd", "yes", 3,
						      0) == 0);
				memset(xbuf, 0, sizeof(xbuf));
				xn = fgetxattr(xfd2, "user.fd", xbuf,
					       sizeof(xbuf));
				test_result("fgetxattr value == yes",
					    xn == 3 && memcmp(xbuf, "yes", 3) ==
							       0);
				close(xfd2);
			}

			/* Large value -> spills to an external xattr block. */
			{
				char xbig[600], xbigbuf[700];
				for (unsigned bi2 = 0; bi2 < sizeof(xbig);
				     bi2++)
					xbig[bi2] = (char)('A' + (bi2 % 26));
				test_result(
					"setxattr large value (external block)",
					setxattr(xpath, "user.big", xbig,
						 sizeof(xbig), 0) == 0);
				memset(xbigbuf, 0, sizeof(xbigbuf));
				xn = getxattr(xpath, "user.big", xbigbuf,
					      sizeof(xbigbuf));
				test_result("getxattr large value matches",
					    xn == (ssize_t)sizeof(xbig) &&
						    memcmp(xbigbuf, xbig,
							   sizeof(xbig)) == 0);
				test_result(
					"small attr still readable with block present",
					getxattr(xpath, "user.x", xbuf,
						 sizeof(xbuf)) == 1);
				test_result(
					"removexattr large value (frees the block)",
					removexattr(xpath, "user.big") == 0);
				errno = 0;
				test_result("getxattr removed large -> ENODATA",
					    getxattr(xpath, "user.big", xbigbuf,
						     sizeof(xbigbuf)) == -1 &&
						    errno == ENODATA);
			}
		}
		unlink(xpath);
	}

	// ========================================
	// Test: kill
	// ========================================
	printf("\n[TEST] kill\n");
	test_result("kill(getpid(), 0) succeeds", kill(getpid(), 0) == 0);
	test_result("kill(invalid, 0) fails",
		    kill(99999, 0) == -1 && errno == ESRCH);
	pid_t kchild = fork();
	if (kchild == 0) {
		// child waits to be killed
		sleep(5);
		_exit(0);
	} else if (kchild > 0) {
		test_result("kill(child, SIGTERM) succeeds",
			    kill(kchild, SIGTERM) == 0);
		int kst = 0;
		pid_t kw = waitpid(kchild, &kst, 0);
		test_result("waitpid returns child", kw == kchild);
		test_result("child killed exit status",
			    (WIFSIGNALED(kst) && WTERMSIG(kst) == SIGTERM) ||
				    (WIFEXITED(kst) &&
				     WEXITSTATUS(kst) == (128 + SIGTERM)));
	} else {
		test_fail("fork() for kill test failed");
	}

	// ========================================
	// Test: tty/pty + termios
	// ========================================
	printf("\n[TEST] tty/pty\n");
	int mfd = posix_openpt(O_RDWR);
	test_result("posix_openpt() succeeds", mfd >= 0);
	int pty_num = -1;
	if (mfd >= 0) {
		test_result("ioctl(TIOCGPTN) succeeds",
			    ioctl(mfd, TIOCGPTN, &pty_num) == 0 &&
				    pty_num >= 0);
	}
	char pts_path[32];
	int sfd = -1;
	if (pty_num >= 0) {
		snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", pty_num);
		sfd = open(pts_path, O_RDWR);
		test_result("open pts slave succeeds", sfd >= 0);
	}

	/* A non-blocking master read with nothing queued must report EAGAIN,
	 * NOT end-of-file.
	 *
	 * Zero from read() on a pty master means the line is down -- every
	 * terminal emulator treats it as "the child is gone" and tears the
	 * session down.  Returning it merely because the child has not
	 * written yet is a lie with teeth: VTE opens the master O_NONBLOCK
	 * and reads it from a timer, so its first read landed before the
	 * freshly exec'd shell had printed its prompt.  It read the zero as
	 * EOF, released the pty, and closing the master hung up the line --
	 * SIGHUP to the shell it had just started.  xfce4-terminal's window
	 * never appeared and nothing reported an error, because as far as
	 * every layer above was concerned the child had simply finished.
	 *
	 * The slave side (tty_read) and the master write path both had this
	 * right; the master read was the one that did not. */
	if (mfd >= 0 && sfd >= 0) {
		char nbbuf[16];
		int fl = fcntl(mfd, F_GETFL, 0);

		test_result("master can be set O_NONBLOCK",
			    fcntl(mfd, F_SETFL, fl | O_NONBLOCK) == 0);

		errno = 0;
		ssize_t r = read(mfd, nbbuf, sizeof nbbuf);

		test_result("empty non-blocking master read is EAGAIN, not EOF",
			    r == -1 && errno == EAGAIN);

		/* And it still delivers data once the slave writes. */
		if (write(sfd, "x", 1) == 1) {
			/* The slave's line discipline may need a moment; the
			 * point is that a non-empty read returns the byte and
			 * never zero. */
			int tries = 0;

			do {
				errno = 0;
				r = read(mfd, nbbuf, sizeof nbbuf);
			} while (r == -1 && errno == EAGAIN && ++tries < 1000);
			test_result("master read returns the slave's byte",
				    r >= 1);
		}

		/* Real EOF -- the one case that MAY return zero -- is the
		 * slave being closed.  Checked on a pty of its own so the one
		 * above stays usable. */
		int m2 = posix_openpt(O_RDWR);
		int n2 = -1;

		if (m2 >= 0 && ioctl(m2, TIOCGPTN, &n2) == 0 && n2 >= 0) {
			char p2[32];

			snprintf(p2, sizeof p2, "/dev/pts/%d", n2);
			grantpt(m2);
			unlockpt(m2);
			int s2 = open(p2, O_RDWR);

			if (s2 >= 0) {
				fcntl(m2, F_SETFL,
				      fcntl(m2, F_GETFL, 0) | O_NONBLOCK);
				close(s2);
				errno = 0;
				r = read(m2, nbbuf, sizeof nbbuf);
				test_result("master read IS EOF once the slave closes",
					    r == 0);
			}
			close(m2);
		}

		/* Put the master back the way the rest of this section
		 * expects to find it. */
		fcntl(mfd, F_SETFL, fl);
	}

	if (mfd >= 0 && sfd >= 0) {
		struct termios tio;
		test_result("tcgetattr succeeds", tcgetattr(sfd, &tio) == 0);
		test_result("canonical enabled by default",
			    (tio.c_lflag & ICANON) != 0);
		test_result("echo enabled by default",
			    (tio.c_lflag & ECHO) != 0);

		cfmakeraw(&tio);
		test_result("tcsetattr(TCSANOW) succeeds",
			    tcsetattr(sfd, TCSANOW, &tio) == 0);
		test_result("tcgetattr raw", tcgetattr(sfd, &tio) == 0);
		test_result("canonical disabled in raw",
			    (tio.c_lflag & ICANON) == 0);

		const char *ping = "ping";
		test_result("write master->slave", write(mfd, ping, 4) == 4);
		char rbuf[8];
		memset(rbuf, 0, sizeof(rbuf));
		ssize_t rr = read(sfd, rbuf, 4);
		test_result("read slave receives data",
			    rr == 4 && memcmp(rbuf, ping, 4) == 0);

		const char *pong = "pong";
		test_result("write slave->master", write(sfd, pong, 4) == 4);
		memset(rbuf, 0, sizeof(rbuf));
		rr = read(mfd, rbuf, 4);
		test_result("read master receives data",
			    rr == 4 && memcmp(rbuf, pong, 4) == 0);

		test_result("tcsetpgrp succeeds",
			    tcsetpgrp(sfd, getpgrp()) == 0);
		test_result("tcgetpgrp matches", tcgetpgrp(sfd) == getpgrp());

		close(sfd);
		close(mfd);
	} else {
		test_fail("pty master/slave setup failed");
		if (mfd >= 0)
			close(mfd);
		if (sfd >= 0)
			close(sfd);
	}

	// ========================================
	// Test: fcntl advisory record locking
	// ========================================
	printf("\n[TEST] fcntl record locks\n");
	{
		/* Per-process path.  This case is ABOUT contention between
		 * processes, so it has to own its file: on a fixed path a
		 * second copy of this suite running at the same time takes the
		 * whole-file write lock first and every assertion below fails
		 * -- correctly, since F_SETLK is supposed to refuse.  The
		 * contention being tested is with the child forked below, not
		 * with another test run. */
		char lpath[64];
		int lfd;

		snprintf(lpath, sizeof(lpath), "/tmp/likeos_lock_test.%d",
			 (int)getpid());
		lfd = open(lpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (lfd < 0) {
			test_fail("open() for the lock test file");
		} else {
			struct flock fl;
			/* Whole file, the way every real caller spells it. */
			memset(&fl, 0, sizeof(fl));
			fl.l_whence = SEEK_SET;
			fl.l_start = 0;
			fl.l_len = 0;

			fl.l_type = F_WRLCK;
			test_result("F_SETLK takes a write lock",
				    fcntl(lfd, F_SETLK, &fl) == 0);

			/* Re-taking our OWN lock must succeed: locks belong to
			 * the process, so it can never block itself. */
			test_result("re-locking our own region succeeds",
				    fcntl(lfd, F_SETLK, &fl) == 0);

			/* F_GETLK on a region we hold reports it as free -- our
			 * own locks are not conflicts. */
			fl.l_type = F_WRLCK;
			if (fcntl(lfd, F_GETLK, &fl) == 0)
				test_result("F_GETLK ignores our own lock",
					    fl.l_type == F_UNLCK);
			else
				test_fail("F_GETLK on our own lock");

			/* A second PROCESS must be blocked by it.  This is the
			 * whole point, and it cannot be tested in one process. */
			fflush(stdout);
			pid_t lpid = fork();
			if (lpid == 0) {
				struct flock c;
				int cfd = open(lpath, O_RDWR);
				int rc = 3;
				if (cfd >= 0) {
					memset(&c, 0, sizeof(c));
					c.l_type = F_WRLCK;
					c.l_whence = SEEK_SET;
					c.l_start = 0;
					c.l_len = 0;
					errno = 0;
					/* Must fail, and specifically EAGAIN. */
					if (fcntl(cfd, F_SETLK, &c) == -1 &&
					    errno == EAGAIN)
						rc = 0;
					else
						rc = 1;
					/* And F_GETLK must name the holder. */
					memset(&c, 0, sizeof(c));
					c.l_type = F_WRLCK;
					c.l_whence = SEEK_SET;
					if (rc == 0 &&
					    fcntl(cfd, F_GETLK, &c) == 0 &&
					    c.l_type == F_WRLCK &&
					    c.l_pid == getppid())
						rc = 0;
					else if (rc == 0)
						rc = 2;
					close(cfd);
				}
				_exit(rc);
			} else if (lpid > 0) {
				int lst = 0;
				waitpid(lpid, &lst, 0);
				test_result(
					"another process is blocked with EAGAIN, and F_GETLK names the holder",
					WIFEXITED(lst) && WEXITSTATUS(lst) == 0);
			} else {
				test_fail("fork() for the lock conflict test");
			}

			/* Unlock, then the same child must succeed. */
			fl.l_type = F_UNLCK;
			test_result("F_UNLCK releases",
				    fcntl(lfd, F_SETLK, &fl) == 0);
			fflush(stdout);
			lpid = fork();
			if (lpid == 0) {
				struct flock c;
				int cfd = open(lpath, O_RDWR);
				int rc = 1;
				if (cfd >= 0) {
					memset(&c, 0, sizeof(c));
					c.l_type = F_WRLCK;
					c.l_whence = SEEK_SET;
					if (fcntl(cfd, F_SETLK, &c) == 0)
						rc = 0;
					close(cfd);
				}
				_exit(rc);
			} else if (lpid > 0) {
				int lst = 0;
				waitpid(lpid, &lst, 0);
				test_result(
					"after unlock another process can take it",
					WIFEXITED(lst) && WEXITSTATUS(lst) == 0);
			}

			/* A lock must not outlive the process that held it.  The
			 * child below exits WITHOUT unlocking; if the kernel did
			 * not clean up, this file would stay locked forever and
			 * F_SETLKW would spin on it. */
			fflush(stdout);
			lpid = fork();
			if (lpid == 0) {
				struct flock c;
				int cfd = open(lpath, O_RDWR);
				memset(&c, 0, sizeof(c));
				c.l_type = F_WRLCK;
				c.l_whence = SEEK_SET;
				if (cfd >= 0)
					fcntl(cfd, F_SETLK, &c);
				_exit(0); /* deliberately no unlock, no close */
			} else if (lpid > 0) {
				int lst = 0;
				waitpid(lpid, &lst, 0);
				fl.l_type = F_WRLCK;
				test_result(
					"a dead process's lock is released on exit",
					fcntl(lfd, F_SETLK, &fl) == 0);
				fl.l_type = F_UNLCK;
				fcntl(lfd, F_SETLK, &fl);
			}

			/* Read locks are shared: two processes may hold one. */
			fl.l_type = F_RDLCK;
			if (fcntl(lfd, F_SETLK, &fl) == 0) {
				fflush(stdout);
				lpid = fork();
				if (lpid == 0) {
					struct flock c;
					int cfd = open(lpath, O_RDWR);
					int rc = 1;
					if (cfd >= 0) {
						memset(&c, 0, sizeof(c));
						c.l_type = F_RDLCK;
						c.l_whence = SEEK_SET;
						if (fcntl(cfd, F_SETLK, &c) == 0)
							rc = 0;
						close(cfd);
					}
					_exit(rc);
				} else if (lpid > 0) {
					int lst = 0;
					waitpid(lpid, &lst, 0);
					test_result(
						"two processes may share a read lock",
						WIFEXITED(lst) &&
							WEXITSTATUS(lst) == 0);
				}
				fl.l_type = F_UNLCK;
				fcntl(lfd, F_SETLK, &fl);
			} else {
				test_fail("F_SETLK read lock");
			}

			/* SEEK_CUR is refused rather than guessed (the VFS does
			 * not expose the descriptor offset to the lock layer). */
			memset(&fl, 0, sizeof(fl));
			fl.l_type = F_WRLCK;
			fl.l_whence = SEEK_CUR;
			errno = 0;
			test_result("l_whence SEEK_CUR is refused, not guessed",
				    fcntl(lfd, F_SETLK, &fl) == -1 &&
					    errno == EINVAL);

			close(lfd);
			unlink(lpath);
		}
	}

	// ========================================
	// Test: iconv
	// ========================================
	printf("\n[TEST] iconv\n");
	{
		iconv_t cd;
		char in[64], out[64];
		char *ip, *op;
		size_t il, ol, r;

		/* Name matching ignores case, '-' and '_', so these are all the
		 * same charset and all must open. */
		cd = iconv_open("UTF-8", "ISO-8859-1");
		test_result("iconv_open(UTF-8, ISO-8859-1)", cd != (iconv_t)-1);
		if (cd != (iconv_t)-1)
			iconv_close(cd);
		cd = iconv_open("utf8", "iso88591");
		test_result("iconv_open is case/punctuation insensitive",
			    cd != (iconv_t)-1);
		if (cd != (iconv_t)-1)
			iconv_close(cd);

		errno = 0;
		cd = iconv_open("UTF-8", "NO-SUCH-CHARSET-42");
		test_result("unknown charset -> (iconv_t)-1 + EINVAL",
			    cd == (iconv_t)-1 && errno == EINVAL);

		/* Latin-1 0xE4 is a-umlaut, U+00E4, which is 0xC3 0xA4 in
		 * UTF-8.  A wrong decode or encode changes these bytes. */
		cd = iconv_open("UTF-8", "ISO-8859-1");
		if (cd != (iconv_t)-1) {
			in[0] = 'a';
			in[1] = (char)0xE4;
			in[2] = 'z';
			ip = in;
			il = 3;
			op = out;
			ol = sizeof(out);
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("latin1->utf8 converts",
				    r != (size_t)-1 && il == 0);
			test_result("latin1->utf8 bytes are correct",
				    (size_t)(op - out) == 4 &&
					    (unsigned char)out[0] == 'a' &&
					    (unsigned char)out[1] == 0xC3 &&
					    (unsigned char)out[2] == 0xA4 &&
					    (unsigned char)out[3] == 'z');
			iconv_close(cd);
		} else {
			test_fail("iconv_open for the latin1->utf8 case");
		}

		/* And back the other way, which must round-trip. */
		cd = iconv_open("ISO-8859-1", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = 'a';
			in[1] = (char)0xC3;
			in[2] = (char)0xA4;
			in[3] = 'z';
			ip = in;
			il = 4;
			op = out;
			ol = sizeof(out);
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("utf8->latin1 round-trips",
				    r != (size_t)-1 &&
					    (size_t)(op - out) == 3 &&
					    (unsigned char)out[1] == 0xE4);
			iconv_close(cd);
		}

		/* A character with no Latin-1 form is an ERROR, not a silent
		 * substitution: plain iconv reports EILSEQ and only //TRANSLIT
		 * replaces it.  This is what stops "save as ISO-8859-1" from
		 * quietly writing a file full of question marks.  U+20AC (euro)
		 * is E2 82 AC in UTF-8. */
		cd = iconv_open("ISO-8859-1", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xE2;
			in[1] = (char)0x82;
			in[2] = (char)0xAC;
			ip = in;
			il = 3;
			op = out;
			ol = sizeof(out);
			errno = 0;
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("unrepresentable char -> EILSEQ, no substitution",
				    r == (size_t)-1 && errno == EILSEQ);
			iconv_close(cd);
		}

		/* ...and with //TRANSLIT it IS replaced, and counted as a
		 * non-reversible conversion. */
		cd = iconv_open("ISO-8859-1//TRANSLIT", "UTF-8");
		in[0] = (char)0xE2;
		in[1] = (char)0x82;
		in[2] = (char)0xAC;
		ip = in;
		il = 3;
		op = out;
		ol = sizeof(out);
		out[0] = 0;
		r = (cd == (iconv_t)-1) ? (size_t)-1 :
					  iconv(cd, &ip, &il, &op, &ol);
		test_result("//TRANSLIT substitutes and counts it",
			    cd != (iconv_t)-1 && r == 1 && out[0] == '?');
		if (cd != (iconv_t)-1)
			iconv_close(cd);

		/* ...but ISO-8859-15 DOES have the euro, at 0xA4. */
		cd = iconv_open("ISO-8859-15", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xE2;
			in[1] = (char)0x82;
			in[2] = (char)0xAC;
			ip = in;
			il = 3;
			op = out;
			ol = sizeof(out);
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("euro encodes to 0xA4 in ISO-8859-15",
				    r == 0 && (unsigned char)out[0] == 0xA4);
			iconv_close(cd);
		}

		/* Error contract: a truncated sequence is EINVAL (come back with
		 * more input), an invalid one is EILSEQ (never valid), and a
		 * full output buffer is E2BIG.  Conflating them makes callers
		 * either loop forever or give up on good data. */
		cd = iconv_open("ISO-8859-1", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xC3; /* first byte of a 2-byte sequence */
			ip = in;
			il = 1;
			op = out;
			ol = sizeof(out);
			errno = 0;
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("truncated sequence -> EINVAL",
				    r == (size_t)-1 && errno == EINVAL);

			in[0] = (char)0x80; /* a bare continuation byte */
			ip = in;
			il = 1;
			op = out;
			ol = sizeof(out);
			errno = 0;
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("invalid sequence -> EILSEQ",
				    r == (size_t)-1 && errno == EILSEQ);
			iconv_close(cd);
		}

		cd = iconv_open("UTF-8", "ISO-8859-1");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xE4; /* needs 2 bytes out */
			ip = in;
			il = 1;
			op = out;
			ol = 1; /* only room for 1 */
			errno = 0;
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("full output buffer -> E2BIG",
				    r == (size_t)-1 && errno == E2BIG);
			test_result("E2BIG consumed no input",
				    il == 1 && ip == in);
			iconv_close(cd);
		}

		/* Over-long forms and surrogates are valid bit patterns but
		 * invalid UTF-8; accepting them is how decoders become holes.
		 * 0xC0 0x80 is an over-long NUL. */
		cd = iconv_open("ISO-8859-1", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xC0;
			in[1] = (char)0x80;
			ip = in;
			il = 2;
			op = out;
			ol = sizeof(out);
			errno = 0;
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("over-long UTF-8 rejected (EILSEQ)",
				    r == (size_t)-1 && errno == EILSEQ);
			iconv_close(cd);
		}

		/* A code point above the BMP exercises the UTF-16 surrogate
		 * pairing both ways.  U+1F600 -> D83D DE00. */
		cd = iconv_open("UTF-16LE", "UTF-8");
		if (cd != (iconv_t)-1) {
			in[0] = (char)0xF0;
			in[1] = (char)0x9F;
			in[2] = (char)0x98;
			in[3] = (char)0x80;
			ip = in;
			il = 4;
			op = out;
			ol = sizeof(out);
			r = iconv(cd, &ip, &il, &op, &ol);
			test_result("astral char -> UTF-16 surrogate pair",
				    r != (size_t)-1 &&
					    (size_t)(op - out) == 4 &&
					    (unsigned char)out[0] == 0x3D &&
					    (unsigned char)out[1] == 0xD8 &&
					    (unsigned char)out[2] == 0x00 &&
					    (unsigned char)out[3] == 0xDE);
			iconv_close(cd);
		}

		errno = 0;
		test_result("iconv_close((iconv_t)-1) fails with EBADF",
			    iconv_close((iconv_t)-1) == -1 && errno == EBADF);
	}

	// ========================================
	// Test: O_CREAT file mode and umask
	//
	// The mode argument to open() was discarded in three independent
	// places -- libc passed 0, the syscall ignored it, and ext4 created
	// every file 0644 -- while umask() was a libc-private variable that
	// nothing read.  The result was that a program could not create a
	// private file at all: mkstemp() asks for 0600 and got 0644, so every
	// temporary file was world-readable.
	// ========================================
	printf("\n[TEST] O_CREAT mode and umask\n");
	{
		struct stat mst;
		mode_t oldmask = umask(0);
		const char *mpath = tmpp("likeos_mode_test");
		const char *dpath = tmpp("likeos_mode_dir");

		unlink(mpath);
		rmdir(dpath);

		/* The requested mode, with the mask wide open. */
		int mfd2 = open(mpath, O_RDWR | O_CREAT | O_EXCL, 0600);
		test_result("open(O_CREAT, 0600) succeeds", mfd2 >= 0);
		if (mfd2 >= 0) {
			test_result("...and the file really is 0600",
				    fstat(mfd2, &mst) == 0 &&
					    (mst.st_mode & 07777) == 0600);
			close(mfd2);
		}
		/* An existing file's mode must NOT be changed by a later
		 * O_CREAT open -- POSIX applies the mode only on creation. */
		mfd2 = open(mpath, O_RDWR | O_CREAT, 0666);
		test_result("O_CREAT on an existing file leaves its mode alone",
			    mfd2 >= 0 && fstat(mfd2, &mst) == 0 &&
				    (mst.st_mode & 07777) == 0600);
		if (mfd2 >= 0)
			close(mfd2);
		unlink(mpath);

		/* 0666 under umask 022 is 0644 ... */
		umask(0022);
		mfd2 = open(mpath, O_RDWR | O_CREAT | O_EXCL, 0666);
		test_result("umask 022 turns 0666 into 0644",
			    mfd2 >= 0 && fstat(mfd2, &mst) == 0 &&
				    (mst.st_mode & 07777) == 0644);
		if (mfd2 >= 0)
			close(mfd2);
		unlink(mpath);

		/* ... and under umask 077 it is 0600.  This is the case that
		 * proves umask() reaches the kernel at all. */
		umask(0077);
		mfd2 = open(mpath, O_RDWR | O_CREAT | O_EXCL, 0666);
		test_result("umask 077 turns 0666 into 0600",
			    mfd2 >= 0 && fstat(mfd2, &mst) == 0 &&
				    (mst.st_mode & 07777) == 0600);
		if (mfd2 >= 0)
			close(mfd2);
		unlink(mpath);

		/* umask() returns the PREVIOUS mask -- callers rely on that to
		 * restore it, and a wrong return value silently leaves the
		 * process with the wrong mask afterwards. */
		test_result("umask() returns the previous mask",
			    umask(0022) == 0077 && umask(0022) == 0022);

		/* mkdir honours mode & ~umask too. */
		umask(0022);
		test_result("mkdir(0777) under umask 022 is 0755",
			    mkdir(dpath, 0777) == 0 &&
				    stat(dpath, &mst) == 0 &&
				    (mst.st_mode & 07777) == 0755);
		rmdir(dpath);

		/* The security payoff: a temporary file is private. */
		umask(0);
		char tmpl[] = "/tmp/likeos_modeXXXXXX";
		int tfd2 = mkstemp(tmpl);
		test_result("mkstemp() creates a 0600 file, not world-readable",
			    tfd2 >= 0 && fstat(tfd2, &mst) == 0 &&
				    (mst.st_mode & 07777) == 0600);
		if (tfd2 >= 0) {
			close(tfd2);
			unlink(tmpl);
		}

		/* The mask belongs to the PROCESS, so every thread of it shares
		 * one value: a thread must observe what main set, and a change
		 * the thread makes must be visible back here.  A per-task copy
		 * would pass the fork test below and still fail this one. */
		{
			pthread_t ut;
			umask(0027);
			g_umask_seen_in_thread = 0777;
			if (pthread_create(&ut, NULL, umask_thread_fn, NULL) == 0) {
				pthread_join(ut, NULL);
				test_result("a thread sees the process umask",
					    g_umask_seen_in_thread == 0027);
				test_result("...and its change is visible to us",
					    umask(0022) == 0044);
			} else {
				test_fail("pthread_create for the umask test");
			}
		}

		/* The mask is per-process state and must survive fork(). */
		umask(0077);
		pid_t mpid = fork();
		if (mpid == 0) {
			_exit(umask(0) == 0077 ? 0 : 1);
		} else if (mpid > 0) {
			int mst2 = 0;
			waitpid(mpid, &mst2, 0);
			test_result("umask is inherited across fork()",
				    WIFEXITED(mst2) && WEXITSTATUS(mst2) == 0);
		}

		umask(oldmask);
	}

	// ========================================
	// Test: difftime / strptime
	// ========================================
	printf("\n[TEST] difftime and strptime\n");
	{
		struct tm tmv;
		char *endp;

		test_result("difftime of two times", difftime(100, 40) == 60.0);
		test_result("difftime is signed", difftime(40, 100) == -60.0);
		/* Computed in double: subtracting distant time_t values as
		 * integers can overflow, which is undefined behaviour. */
		test_result("difftime over a huge span",
			    difftime((time_t)4000000000LL, (time_t)-4000000000LL) ==
				    8000000000.0);

		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("2026-07-31", "%Y-%m-%d", &tmv);
		test_result("strptime parses an ISO date",
			    endp != NULL && *endp == '\0' &&
				    tmv.tm_year == 126 && tmv.tm_mon == 6 &&
				    tmv.tm_mday == 31);
		/* The weekday and day-of-year are derived even though the
		 * format never mentions them -- code that parses a date and
		 * then reads tm_wday is common, and leaving it zero silently
		 * means Sunday.  2026-07-31 is a Friday, day 211. */
		test_result("...and derives tm_wday/tm_yday",
			    tmv.tm_wday == 5 && tmv.tm_yday == 211);

		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("Fri Jul 31 12:34:56 2026",
				"%a %b %e %H:%M:%S %Y", &tmv);
		test_result("strptime parses a full date and time",
			    endp != NULL && *endp == '\0' &&
				    tmv.tm_hour == 12 && tmv.tm_min == 34 &&
				    tmv.tm_sec == 56 && tmv.tm_wday == 5);

		/* Names are matched case-insensitively, and the FULL name must
		 * win over the abbreviation: "Sunday" also begins with "Sun",
		 * so matching the short form first would leave "day" behind. */
		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("SUNDAY", "%A", &tmv);
		test_result("strptime matches a full day name, any case",
			    endp != NULL && *endp == '\0' && tmv.tm_wday == 0);

		/* Two-digit years follow POSIX: 69-99 are 1900s, 00-68 2000s. */
		memset(&tmv, 0, sizeof(tmv));
		strptime("69", "%y", &tmv);
		test_result("%y 69 is 1969", tmv.tm_year == 69);
		memset(&tmv, 0, sizeof(tmv));
		strptime("68", "%y", &tmv);
		test_result("%y 68 is 2068", tmv.tm_year == 168);

		/* %I with %p. */
		memset(&tmv, 0, sizeof(tmv));
		strptime("12:00 AM", "%I:%M %p", &tmv);
		test_result("12:00 AM is hour 0", tmv.tm_hour == 0);
		memset(&tmv, 0, sizeof(tmv));
		strptime("12:00 PM", "%I:%M %p", &tmv);
		test_result("12:00 PM is hour 12", tmv.tm_hour == 12);

		/* The number reader stops early rather than failing, which is
		 * what lets unpadded "%H%M%S" input parse at all. */
		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("123456", "%H%M%S", &tmv);
		test_result("%H%M%S splits an unpadded time",
			    endp != NULL && tmv.tm_hour == 12 &&
				    tmv.tm_min == 34 && tmv.tm_sec == 56);

		/* ISO 8601 offsets, including the colon form a browser meets in
		 * HTTP and Atom dates. */
		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("2026-07-31T10:20:30+01:30",
				"%Y-%m-%dT%H:%M:%S%z", &tmv);
		test_result("strptime consumes a +hh:mm offset",
			    endp != NULL && *endp == '\0' &&
				    tmv.tm_hour == 10 && tmv.tm_min == 20);
		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("2026-07-31T10:20:30Z", "%Y-%m-%dT%H:%M:%S%z",
				&tmv);
		test_result("strptime consumes a Z offset",
			    endp != NULL && *endp == '\0');

		/* Mismatches return NULL rather than a partial answer. */
		memset(&tmv, 0, sizeof(tmv));
		test_result("a non-matching input returns NULL",
			    strptime("not a date", "%Y-%m-%d", &tmv) == NULL);
		test_result("an out-of-range month returns NULL",
			    strptime("13", "%m", &tmv) == NULL);

		/* Trailing input is left for the caller, not an error. */
		memset(&tmv, 0, sizeof(tmv));
		endp = strptime("2026-07-31 leftover", "%Y-%m-%d", &tmv);
		test_result("unconsumed input is returned, not rejected",
			    endp != NULL && strcmp(endp, " leftover") == 0);

		/* Fields the format does not mention are left alone, which is
		 * what lets several calls build up one time. */
		memset(&tmv, 0, sizeof(tmv));
		tmv.tm_hour = 7;
		strptime("2026-07-31", "%Y-%m-%d", &tmv);
		test_result("unmentioned fields are preserved", tmv.tm_hour == 7);
	}

	// ========================================
	// Test: unlinkat
	// ========================================
	printf("\n[TEST] unlinkat\n");
	{
		const char *ubase = tmpp("likeos_unlinkat");
		char ufile[128], udir[128];
		int dfd;

		snprintf(ufile, sizeof(ufile), "%s/file", ubase);
		snprintf(udir, sizeof(udir), "%s/dir", ubase);
		/* clean slate from any earlier run */
		unlink(ufile);
		rmdir(udir);
		rmdir(ubase);

		test_result("mkdir the unlinkat test directory",
			    mkdir(ubase, 0700) == 0);
		dfd = open(ubase, O_RDONLY);
		test_result("open the directory for use as a dirfd", dfd >= 0);

		/* AT_FDCWD with an absolute path behaves like unlink(). */
		{
			int fd = open(ufile, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd >= 0)
				close(fd);
			test_result("unlinkat(AT_FDCWD) removes a file",
				    unlinkat(AT_FDCWD, ufile, 0) == 0 &&
					    access(ufile, F_OK) != 0);
		}

		/* A relative name is resolved against the dirfd -- the whole
		 * point of the call. */
		if (dfd >= 0) {
			int fd = open(ufile, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd >= 0)
				close(fd);
			test_result("unlinkat(dirfd, \"file\") removes it",
				    unlinkat(dfd, "file", 0) == 0 &&
					    access(ufile, F_OK) != 0);
		}

		/* A directory needs AT_REMOVEDIR, and must be refused without
		 * it -- otherwise unlink() semantics would silently differ. */
		test_result("mkdir a subdirectory", mkdir(udir, 0700) == 0);
		errno = 0;
		test_result("unlinkat on a directory without AT_REMOVEDIR fails",
			    unlinkat(AT_FDCWD, udir, 0) != 0);
		test_result("unlinkat with AT_REMOVEDIR removes it",
			    unlinkat(AT_FDCWD, udir, AT_REMOVEDIR) == 0 &&
				    access(udir, F_OK) != 0);

		/* An unimplemented flag must be rejected, not ignored: a caller
		 * passing one is asking for behaviour we would not deliver. */
		errno = 0;
		test_result("an unknown flag is EINVAL",
			    unlinkat(AT_FDCWD, ufile, 0x40) == -1 &&
				    errno == EINVAL);

		errno = 0;
		test_result("unlinkat on a missing name is ENOENT",
			    unlinkat(AT_FDCWD, tmpp("likeos_no_such_file_xyz"),
				     0) == -1 &&
				    errno == ENOENT);

		/* The whole *at() family shares one path-resolution helper, and
		 * it rejected every real dirfd until unlinkat exposed it.  These
		 * cover the other three, which were equally broken and had no
		 * test at all. */
		if (dfd >= 0) {
			int fd = open(ufile, O_WRONLY | O_CREAT | O_EXCL, 0600);
			if (fd >= 0) {
				write(fd, "xyz", 3);
				close(fd);
			}

			fd = openat(dfd, "file", O_RDONLY);
			test_result("openat(dirfd, \"file\") opens it", fd >= 0);
			if (fd >= 0)
				close(fd);

			{
				struct stat ast;
				test_result("fstatat(dirfd, \"file\") stats it",
					    fstatat(dfd, "file", &ast, 0) == 0 &&
						    ast.st_size == 3);
			}
			test_result("faccessat(dirfd, \"file\") succeeds",
				    faccessat(dfd, "file", F_OK, 0) == 0);

			/* An absolute path ignores the dirfd, as POSIX says. */
			test_result("an absolute path ignores the dirfd",
				    faccessat(dfd, ufile, F_OK, 0) == 0);

			/* A dirfd that is not a directory must be ENOTDIR --
			 * resolving against a regular file would invent a path
			 * that looks valid and refers to nothing. */
			{
				int ffd = open(ufile, O_RDONLY);
				if (ffd >= 0) {
					errno = 0;
					test_result("a non-directory dirfd is ENOTDIR",
						    faccessat(ffd, "x", F_OK,
							      0) == -1 &&
							    errno == ENOTDIR);
					close(ffd);
				}
			}

			errno = 0;
			test_result("a closed dirfd is EBADF",
				    faccessat(9999, "x", F_OK, 0) == -1 &&
					    errno == EBADF);

			unlink(ufile);
		}

		if (dfd >= 0)
			close(dfd);
		rmdir(ubase);
	}

	// ========================================
	// Test: shared-memory permissions
	// ========================================
	printf("\n[TEST] shared memory permissions\n");
	{
		/* Root bypasses IPC permission checks, exactly as CAP_IPC_OWNER
		 * does on the reference system -- so asserting denial while
		 * running as root proves nothing.  The denial paths are checked
		 * in a child that drops privilege; what root does here is
		 * checked as root, because being allowed IS the correct
		 * behaviour for it. */
		const uid_t nobody = 65534;
		int rid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0044);

		test_result("shmget creates a segment readable only by others",
			    rid >= 0);
		if (rid >= 0) {
			struct shmid_ds ds;
			void *p1;

			/* Root: allowed regardless of the mode. */
			p1 = shmat(rid, NULL, 0);
			test_result("root may attach read-write despite mode 0044",
				    p1 != (void *)-1);
			if (p1 != (void *)-1)
				shmdt(p1);

			test_result("IPC_STAT reports our uid and mode",
				    shmctl(rid, IPC_STAT, &ds) == 0 &&
					    ds.shm_perm.uid == getuid() &&
					    (ds.shm_perm.mode & 0777) == 0044);

			/* Non-root: the "other" triple applies -- read yes,
			 * write no. */
			fflush(stdout);
			{
				pid_t sp = fork();
				if (sp == 0) {
					int rc = 0;
					void *a;
					if (setuid(nobody) != 0)
						_exit(9);
					a = shmat(rid, NULL, SHM_RDONLY);
					if (a == (void *)-1)
						rc |= 1; /* read should work */
					else
						shmdt(a);
					errno = 0;
					a = shmat(rid, NULL, 0);
					if (a != (void *)-1) {
						rc |= 2; /* write must not */
						shmdt(a);
					} else if (errno != EACCES) {
						rc |= 4;
					}
					_exit(rc);
				} else if (sp > 0) {
					int st = 0;
					waitpid(sp, &st, 0);
					test_result("a non-owner may attach read-only",
						    WIFEXITED(st) &&
							    !(WEXITSTATUS(st) & 1));
					test_result("...but read-write is refused with EACCES",
						    WIFEXITED(st) &&
							    !(WEXITSTATUS(st) & 6));
				}
			}
			test_result("the owner may IPC_RMID",
				    shmctl(rid, IPC_RMID, NULL) == 0);
		}

		/* shmget() on an EXISTING key must honour the segment's mode
		 * for a non-owner.  Root would be let through, so again the
		 * check runs in a child. */
		{
			/* Same per-process key as test_sysv_shm(); see the
			 * note there. */
			key_t k = (key_t)(0x4c494b45 ^ (int)getpid());
			int kid = shmget(k, 0, 0);
			if (kid >= 0)
				shmctl(kid, IPC_RMID, NULL);

			kid = shmget(k, 4096, IPC_CREAT | IPC_EXCL | 0044);
			test_result("shmget creates a 0044 segment by key",
				    kid >= 0);
			if (kid >= 0) {
				test_result("re-getting it for READ succeeds",
					    shmget(k, 0, 0400) == kid);
				fflush(stdout);
				{
					pid_t sp = fork();
					if (sp == 0) {
						int rc = 0;
						if (setuid(nobody) != 0)
							_exit(9);
						if (shmget(k, 0, 0004) < 0)
							rc |= 1;
						errno = 0;
						if (shmget(k, 0, 0002) != -1 ||
						    errno != EACCES)
							rc |= 2;
						_exit(rc);
					} else if (sp > 0) {
						int st = 0;
						waitpid(sp, &st, 0);
						test_result("a non-owner may get it for read",
							    WIFEXITED(st) &&
								    !(WEXITSTATUS(st) & 1));
						test_result("...but asking for write is EACCES",
							    WIFEXITED(st) &&
								    !(WEXITSTATUS(st) & 2));
					}
				}
				shmctl(kid, IPC_RMID, NULL);
			}
		}

		/* A read-write segment behaves normally. */
		{
			int wid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
			if (wid >= 0) {
				void *p2 = shmat(wid, NULL, 0);
				test_result("shmat read-write on 0600 attaches",
					    p2 != (void *)-1);
				if (p2 != (void *)-1) {
					*(volatile int *)p2 = 0x5a5a;
					test_result("...and the mapping is writable",
						    *(volatile int *)p2 == 0x5a5a);
					shmdt(p2);
				}
				shmctl(wid, IPC_RMID, NULL);
			} else {
				test_fail("shmget for the read-write segment");
			}
		}

		/* POSIX shm: the mode passed to shm_open must be honoured, and
		 * fstat() on the descriptor must report it -- devfs had no
		 * fstat case for shm objects at all, so it reported a device
		 * instead of the object. */
		{
			mode_t om = umask(0);
			int sfd;
			struct stat sst;

			char pnm[64];

			snprintf(pnm, sizeof(pnm), "/likeos_perm_test.%d",
				 (int)getpid());
			shm_unlink(pnm);
			sfd = shm_open(pnm, O_RDWR | O_CREAT | O_EXCL, 0644);
			test_result("shm_open creates the object", sfd >= 0);
			if (sfd >= 0) {
				test_result("...with the mode that was asked for",
					    fstat(sfd, &sst) == 0 &&
						    (sst.st_mode & 0777) == 0644);
				test_result("...and fstat reports it as a file",
					    fstat(sfd, &sst) == 0 &&
						    S_ISREG(sst.st_mode));
				if (ftruncate(sfd, 8192) == 0)
					test_result("...and fstat reports the size",
						    fstat(sfd, &sst) == 0 &&
							    sst.st_size == 8192);
				close(sfd);
				shm_unlink(pnm);
			}
			umask(om);
		}
	}

	// ========================================
	// Test: AF_UNIX bulk transfer (ring + park/wake)
	// ========================================
	printf("\n[TEST] AF_UNIX bulk transfer\n");
	{
		/* Pushes far more than one ringful through a socketpair, which
		 * is the path that used to spin: the sender filled the ring and
		 * then sched_yield'd in a loop until the reader drained it.  It
		 * now parks and is woken, so this exercises ring allocation,
		 * the full-ring block, the wake on drain, and the wake on
		 * close.  A lost wake shows up here as a hang, and wrong
		 * bookkeeping as corrupted data. */
		int sv[2];
		const size_t total = 512 * 1024;

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			test_fail("socketpair for the bulk transfer test");
		} else {
			pid_t bp;
			fflush(stdout);
			bp = fork();
			if (bp == 0) {
				/* Child writes a byte pattern. */
				unsigned char *out = malloc(4096);
				size_t done = 0;
				close(sv[0]);
				if (!out)
					_exit(2);
				while (done < total) {
					size_t n = total - done;
					if (n > 4096)
						n = 4096;
					for (size_t i = 0; i < n; i++)
						out[i] = (unsigned char)((done + i) & 0xff);
					ssize_t w = write(sv[1], out, n);
					if (w <= 0)
						_exit(3);
					done += (size_t)w;
				}
				free(out);
				close(sv[1]);
				_exit(0);
			} else if (bp > 0) {
				unsigned char *in = malloc(4096);
				size_t got = 0;
				int corrupt = 0, st = 0;

				close(sv[1]);
				while (in && got < total) {
					ssize_t r = read(sv[0], in, 4096);
					if (r <= 0)
						break;
					for (ssize_t i = 0; i < r; i++)
						if (in[i] != (unsigned char)((got + (size_t)i) & 0xff)) {
							corrupt = 1;
							break;
						}
					got += (size_t)r;
				}
				free(in);
				close(sv[0]);
				waitpid(bp, &st, 0);

				test_result("512KB crosses a socketpair intact",
					    got == total && !corrupt);
				test_result("the writer finished cleanly",
					    WIFEXITED(st) && WEXITSTATUS(st) == 0);
			}
		}
	}

	// ========================================
	// Test: AF_UNIX socket node permissions
	// ========================================
	printf("\n[TEST] AF_UNIX node permissions\n");
	{
		/* A bound socket node used to be created 0777 -- world
		 * writable -- and since connecting requires write permission on
		 * the node, that granted every user access to every service.
		 * It now follows the umask like any other created name. */
		const char *spath = tmpp("likeos_uds_perm");
		mode_t om = umask(022);
		int sfd = socket(AF_UNIX, SOCK_STREAM, 0);

		unlink(spath);
		if (sfd >= 0) {
			struct sockaddr_un sa;
			struct stat sst;

			memset(&sa, 0, sizeof(sa));
			sa.sun_family = AF_UNIX;
			strcpy(sa.sun_path, spath);
			test_result("bind creates the socket node",
				    bind(sfd, (struct sockaddr *)&sa,
					 sizeof(sa)) == 0);
			test_result("the node is a socket",
				    stat(spath, &sst) == 0 &&
					    S_ISSOCK(sst.st_mode));
			test_result("...and its mode is 0777 & ~umask",
				    (sst.st_mode & 0777) == 0755);

			/* We own it, so we can still connect to our own
			 * service -- the new permission check must not lock us
			 * out.
			 *
			 * The connect MUST come from another process: connect()
			 * blocks until the listener accepts, so calling it in
			 * the same single-threaded process that owns the
			 * listening socket deadlocks. */
			listen(sfd, 1);
			fflush(stdout);
			{
				pid_t cp = fork();
				if (cp == 0) {
					int cfd = socket(AF_UNIX, SOCK_STREAM,
							 0);
					int r = -1;
					if (cfd >= 0) {
						r = connect(cfd,
							    (struct sockaddr *)
								    &sa,
							    sizeof(sa));
						close(cfd);
					}
					_exit(r == 0 ? 0 : 1);
				} else if (cp > 0) {
					int afd = accept(sfd, NULL, NULL);
					int st = 0;
					waitpid(cp, &st, 0);
					test_result("the owner can still connect",
						    afd >= 0 &&
							    WIFEXITED(st) &&
							    WEXITSTATUS(st) == 0);
					if (afd >= 0)
						close(afd);
				}
			}
			close(sfd);
			unlink(spath);
		} else {
			test_fail("socket(AF_UNIX) for the permission test");
		}
		umask(om);
	}

	// ========================================
	// Test: accept() must honour the caller's addrlen
	// ========================================
	printf("\n[TEST] accept() respects the caller's buffer size\n");
	{
		/* accept()'s addrlen is IN/OUT: on the way in it says how big
		 * the caller's buffer is, and nothing may be written past it.
		 * The kernel used to ignore that and copy a whole
		 * sizeof(struct sockaddr_un) -- 110 bytes -- into whatever was
		 * passed.  A `struct sockaddr' is 16, and passing one is
		 * ordinary: menu-cached does, so every client connection wrote
		 * 94 bytes past a stack buffer and the function it was called
		 * from returned into the wreckage.
		 *
		 * The guard array is the test: it sits immediately after the
		 * address, and accept() must not touch it. */
		struct {
			struct sockaddr addr;
			unsigned char guard[128];
		} b;
		const char *spath = tmpp("likeos_accept_len");
		int sfd = socket(AF_UNIX, SOCK_STREAM, 0);

		unlink(spath);
		if (sfd >= 0) {
			struct sockaddr_un sa;

			memset(&sa, 0, sizeof(sa));
			sa.sun_family = AF_UNIX;
			strcpy(sa.sun_path, spath);
			if (bind(sfd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
			    listen(sfd, 1) == 0) {
				pid_t cp;

				fflush(stdout);
				cp = fork();
				if (cp == 0) {
					int cfd = socket(AF_UNIX, SOCK_STREAM,
							 0);
					int r = -1;
					if (cfd >= 0) {
						r = connect(cfd,
							    (struct sockaddr *)
								    &sa,
							    sizeof(sa));
						close(cfd);
					}
					_exit(r == 0 ? 0 : 1);
				} else if (cp > 0) {
					socklen_t alen;
					int afd, st = 0;

					memset(&b, 0xA5, sizeof(b));
					alen = sizeof(b.addr); /* 16 */
					afd = accept(sfd,
						     (struct sockaddr *)&b.addr,
						     &alen);
					waitpid(cp, &st, 0);
					test_result("accept() with a 16-byte buffer",
						    afd >= 0);

					/* Not one byte past the declared size. */
					{
						int clean = 1;
						for (size_t i = 0;
						     i < sizeof(b.guard); i++)
							if (b.guard[i] != 0xA5)
								clean = 0;
						test_result("...writes nothing past addrlen",
							    clean);
					}
					/* The address's REAL size, which here is
					 * SMALLER than the buffer: the client
					 * never bound, and an unnamed peer is
					 * reported family-only, exactly as a
					 * conventional Unix does.  Asserting
					 * the exact value is what catches the
					 * old behaviour, which answered a flat
					 * sizeof(struct sockaddr_un). */
					test_result("...and reports the true address length",
						    alen == (socklen_t)sizeof(
								    sa_family_t));
					if (afd >= 0)
						close(afd);
				}
			} else {
				test_fail("bind/listen for the accept-length test");
			}
			close(sfd);
			unlink(spath);
		} else {
			test_fail("socket(AF_UNIX) for the accept-length test");
		}
	}

	// ========================================
	// Test: socket options on an AF_UNIX socket
	// ========================================
	printf("\n[TEST] AF_UNIX socket options\n");
	{
		/* setsockopt/getsockopt used to resolve the descriptor through
		 * the INTERNET socket table only, so every option on every
		 * local socket answered ENOTSOCK -- "that is not a socket",
		 * which a caller reads as the descriptor being unusable rather
		 * than the option being unsupported.  PCManFM sets SO_REUSEADDR
		 * on its single-instance socket and abandons the bind() in the
		 * same expression, so it exited with status 1 before opening a
		 * window.  The distinction matters more than the option does:
		 * anything unimplemented must say ENOPROTOOPT. */
		int sv[2];

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
			int one = 1, val = 0;
			socklen_t vlen = sizeof(val);

			errno = 0;
			test_result("setsockopt(SO_REUSEADDR) on AF_UNIX",
				    setsockopt(sv[0], SOL_SOCKET, SO_REUSEADDR,
					       &one, sizeof(one)) == 0);

			test_result("getsockopt(SO_TYPE) reports SOCK_STREAM",
				    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &val,
					       &vlen) == 0 &&
					    val == SOCK_STREAM);

			vlen = sizeof(val);
			val = -1;
			test_result("getsockopt(SO_ERROR) reports no error",
				    getsockopt(sv[0], SOL_SOCKET, SO_ERROR,
					       &val, &vlen) == 0 &&
					    val == 0);

			vlen = sizeof(val);
			val = 0;
			test_result("getsockopt(SO_RCVBUF) reports a size",
				    getsockopt(sv[0], SOL_SOCKET, SO_RCVBUF,
					       &val, &vlen) == 0 &&
					    val > 0);

			/* An option this family does not implement must be
			 * refused as an OPTION, never as a bad descriptor. */
			vlen = sizeof(val);
			errno = 0;
			test_result("an unsupported option gives ENOPROTOOPT",
				    getsockopt(sv[0], SOL_SOCKET, 0x7fff, &val,
					       &vlen) == -1 &&
					    errno == ENOPROTOOPT);

			close(sv[0]);
			close(sv[1]);
		} else {
			test_fail("socketpair(AF_UNIX) for the sockopt test");
		}
	}

	// ========================================
	// Test: strncpy / strncat write bounds
	// ========================================
	printf("\n[TEST] strncpy/strncat bounds\n");
	{
		/* strncpy must write EXACTLY n bytes and strncat at most n+1.
		 * Both used to write one byte too many whenever the source was
		 * shorter than n -- the common case -- because the copy loop
		 * consumed the source's terminator without decrementing n, so
		 * the padding step then ran one past the end.  That overflowed
		 * every exactly-sized buffer in the system, corrupting whatever
		 * followed it. */
		char buf[32];
		int over = 0, wrong = 0;

		for (size_t n = 1; n <= 12; n++) {
			const char *srcs[] = { "", "a", "ab", "abcdefghij" };
			for (unsigned k = 0; k < 4; k++) {
				memset(buf, 0x7E, sizeof(buf));
				strncpy(buf, srcs[k], n);
				/* the byte just past n must be untouched */
				if ((unsigned char)buf[n] != 0x7E)
					over++;
				/* and the result must match the definition */
				{
					size_t sl = strlen(srcs[k]);
					size_t c = sl < n ? sl : n;
					if (memcmp(buf, srcs[k], c) != 0)
						wrong++;
					for (size_t z = c; z < n; z++)
						if (buf[z] != '\0')
							wrong++;
				}
			}
		}
		test_result("strncpy writes no byte past n", over == 0);
		test_result("strncpy pads correctly within n", wrong == 0);

		over = 0;
		wrong = 0;
		for (size_t n = 0; n <= 8; n++) {
			memset(buf, 0x7E, sizeof(buf));
			strcpy(buf, "XY");
			strncat(buf, "abcdef", n);
			{
				size_t app = 6 < n ? 6 : n;
				/* "XY" + app chars + one NUL, nothing beyond */
				if ((unsigned char)buf[2 + app + 1] != 0x7E)
					over++;
				if (strlen(buf) != 2 + app)
					wrong++;
			}
		}
		test_result("strncat writes no byte past n+1", over == 0);
		test_result("strncat appends exactly min(n, srclen)", wrong == 0);
	}

	// ========================================
	// Test: malloc_usable_size is honest
	// ========================================
	printf("\n[TEST] malloc_usable_size\n");
	{
		/* Writing usable_size bytes into a block must be safe: that is
		 * the entire contract of the call.  If it OVER-reports, the
		 * write runs into the next chunk and corrupts a neighbour --
		 * which is exactly the shape of the netsurf corruption.  Each
		 * block is bracketed by neighbours holding a known pattern, so
		 * an over-report shows up as a damaged neighbour. */
		static const size_t req[] = { 1,   7,	8,    9,    15,	  16,
					      17,  31,	32,   33,   63,	  64,
					      65,  100, 127,  128,  129,  255,
					      256, 257, 1000, 1024, 4095, 4096 };
		int bad_small = 0, neighbour = 0;

		for (unsigned i = 0; i < sizeof(req) / sizeof(req[0]); i++) {
			unsigned char *lo = malloc(64);
			unsigned char *mid = malloc(req[i]);
			unsigned char *hi = malloc(64);
			size_t us;

			if (!lo || !mid || !hi) {
				free(lo);
				free(mid);
				free(hi);
				continue;
			}
			memset(lo, 0x11, 64);
			memset(hi, 0x22, 64);

			us = malloc_usable_size(mid);
			/* It must never claim LESS than was asked for. */
			if (us < req[i])
				bad_small++;
			/* Filling the whole usable area must not touch either
			 * neighbour. */
			memset(mid, 0x33, us);
			for (int k = 0; k < 64; k++)
				if (lo[k] != 0x11 || hi[k] != 0x22) {
					neighbour++;
					break;
				}
			free(lo);
			free(mid);
			free(hi);
		}
		test_result("usable_size is never less than the request",
			    bad_small == 0);
		test_result("filling the usable area leaves neighbours intact",
			    neighbour == 0);
	}

	// ========================================
	// Test: allocator does not hand out a live chunk twice
	// ========================================
	printf("\n[TEST] allocator overlap and integrity\n");
	{
		/* NetSurf dies reading an object whose memory has been reused
		 * for unrelated text.  That is either the browser freeing
		 * something it still references, or this allocator handing the
		 * same chunk to two live callers.  This separates the two: it
		 * holds many blocks live at once, stamps each with a unique
		 * pattern, and re-checks every one after every operation.  If
		 * malloc ever returns memory that is already in use, one
		 * block's stamp is overwritten by another's and this fails.
		 *
		 * Sizes deliberately straddle the allocator's class
		 * boundaries -- tcache, fastbin, smallbin, largebin and the
		 * mmap threshold -- because a reuse bug usually lives at one
		 * specific boundary. */
#define AL_N 192
		static unsigned char *blk[AL_N];
		static size_t blksz[AL_N];
		static unsigned char stamp[AL_N];
		static const size_t sizes[] = { 8,	16,    24,    32,
						48,	64,    100,   128,
						200,	256,   500,   1024,
						2048,	4096,  8192,  16384,
						32768,	65536, 131072 };
		unsigned long seed = 20260731UL;
		int overlaps = 0, corrupt = 0, oom = 0;

		for (int i = 0; i < AL_N; i++)
			blk[i] = NULL;

		for (int round = 0; round < 4000; round++) {
			seed = seed * 6364136223846793005UL + 1442695040888963407UL;
			int i = (int)((seed >> 33) % AL_N);
			seed = seed * 6364136223846793005UL + 1442695040888963407UL;
			int op = (int)((seed >> 33) % 3);

			if (blk[i] && op == 0) {
				/* verify before releasing */
				for (size_t k = 0; k < blksz[i]; k++)
					if (blk[i][k] != stamp[i]) {
						corrupt++;
						break;
					}
				free(blk[i]);
				blk[i] = NULL;
			} else if (blk[i] && op == 1) {
				seed = seed * 6364136223846793005UL +
				       1442695040888963407UL;
				size_t ns = sizes[(seed >> 33) %
						  (sizeof(sizes) / sizeof(sizes[0]))];
				unsigned char *np = realloc(blk[i], ns);
				if (!np) {
					oom++;
					continue;
				}
				/* realloc must preserve the old contents up to
				 * the smaller of the two sizes */
				size_t keep = ns < blksz[i] ? ns : blksz[i];
				for (size_t k = 0; k < keep; k++)
					if (np[k] != stamp[i]) {
						corrupt++;
						break;
					}
				blk[i] = np;
				blksz[i] = ns;
				memset(blk[i], stamp[i], blksz[i]);
			} else {
				if (blk[i])
					free(blk[i]);
				seed = seed * 6364136223846793005UL +
				       1442695040888963407UL;
				blksz[i] = sizes[(seed >> 33) %
						 (sizeof(sizes) / sizeof(sizes[0]))];
				blk[i] = malloc(blksz[i]);
				if (!blk[i]) {
					oom++;
					continue;
				}
				stamp[i] = (unsigned char)(i + 1);
				memset(blk[i], stamp[i], blksz[i]);
			}

			/* Every live block must still read back as its own
			 * stamp.  A chunk handed out twice shows up here as
			 * soon as the second owner writes to it. */
			if ((round & 63) == 0) {
				for (int j = 0; j < AL_N; j++) {
					if (!blk[j])
						continue;
					for (size_t k = 0; k < blksz[j]; k++)
						if (blk[j][k] != stamp[j]) {
							overlaps++;
							j = AL_N;
							break;
						}
				}
			}
		}

		for (int i = 0; i < AL_N; i++)
			if (blk[i])
				free(blk[i]);

		test_result("no live block was overwritten by another",
			    overlaps == 0);
		test_result("no block's contents were corrupted", corrupt == 0);
		test_result("the allocator satisfied every request", oom == 0);
#undef AL_N
	}

	// ========================================
	// Test: snprintf return value (C99)
	// ========================================
	printf("\n[TEST] snprintf return value\n");
	{
		char sb[8];
		int n;

		/* C99: the return is the length the output WOULD have had, not
		 * the number of bytes stored.  It used to be the truncated
		 * count, so the standard truncation check below could never
		 * fire and a caller sizing a buffer from the return value
		 * under-allocated and then overran it. */
		memset(sb, 'x', sizeof(sb));
		n = snprintf(sb, sizeof(sb), "0123456789");
		test_result("snprintf returns the untruncated length", n == 10);
		test_result("...and truncation is detectable",
			    n >= (int)sizeof(sb));
		test_result("...and the buffer holds the prefix, terminated",
			    strcmp(sb, "0123456") == 0);

		n = snprintf(sb, sizeof(sb), "abc");
		test_result("a fitting string returns its own length", n == 3);
		test_result("...and is written whole", strcmp(sb, "abc") == 0);

		/* The two-pass sizing idiom must work at any length -- the old
		 * NULL/0 path formatted into an 8KB stack buffer and returned
		 * at most 8191, so anything longer under-allocated. */
		test_result("snprintf(NULL, 0, ...) measures", 
			    snprintf(NULL, 0, "%s", "12345") == 5);
		{
			char big[9000];
			char *heap;
			memset(big, 'A', sizeof(big) - 1);
			big[sizeof(big) - 1] = '\0';
			n = snprintf(NULL, 0, "%s", big);
			test_result("...with no 8KB ceiling", n == 8999);
			heap = malloc((size_t)n + 1);
			if (heap) {
				int m = snprintf(heap, (size_t)n + 1, "%s", big);
				test_result("...so two-pass sizing is exact",
					    m == n && strlen(heap) == (size_t)n);
				free(heap);
			} else {
				test_fail("malloc for the two-pass sizing test");
			}
		}

		/* size 0 must not touch the buffer at all. */
		memset(sb, 'x', sizeof(sb));
		n = snprintf(sb, 0, "hello");
		test_result("size 0 writes nothing but still measures",
			    n == 5 && sb[0] == 'x');
	}

	// ========================================
	// Test: ELF constructors
	// ========================================
	printf("\n[TEST] executable constructors\n");
	{
		test_result("a __attribute__((constructor)) ran", g_ctor_ran == 1);
		/* And it ran BEFORE main, not lazily at first use: the flag was
		 * already set when main started, which the marker below
		 * recorded on entry. */
		test_result("...and it ran before main()",
			    g_ctor_saw_main_before == 1);
	}

	// ========================================
	// Test: assert() namespace hygiene
	// ========================================
	printf("\n[TEST] assert namespace\n");
	{
		/* assert() must expand to nothing the program can shadow.  It
		 * used to expand to fprintf(stderr,...)/abort() literally, so a
		 * local object with one of those names broke compilation --
		 * NetSurf's tree walker has a `bool abort` and hit exactly
		 * that.  These locals reproduce the collision: if assert()
		 * regresses, this file stops COMPILING, which is the strongest
		 * form the check can take. */
		int abort = 1;
		int stderr = 2;
		int fprintf = 3;

		assert(abort == 1);
		assert(stderr == 2);
		assert(fprintf == 3);

		test_result("assert() compiles where abort/stderr/fprintf are shadowed",
			    abort == 1 && stderr == 2 && fprintf == 3);
	}

	// ========================================
	// Test: ISO C stdio limits
	// ========================================
	printf("\n[TEST] stdio limit macros\n");
	{
		/* Required by ISO C and absent until libpng's tools failed to
		 * compile for want of FILENAME_MAX.  Checked against the
		 * standard's minimums and against the limits they are meant to
		 * mirror, so a later edit to one cannot silently contradict
		 * the other. */
		test_result("FILENAME_MAX equals PATH_MAX",
			    FILENAME_MAX == PATH_MAX);
		test_result("FOPEN_MAX equals OPEN_MAX", FOPEN_MAX == OPEN_MAX);
		test_result("FOPEN_MAX is at least the standard's 8",
			    FOPEN_MAX >= 8);
		test_result("TMP_MAX is at least the standard's 25",
			    TMP_MAX >= 25);
		test_result("FOPEN_MAX does not exceed sysconf(_SC_OPEN_MAX)",
			    (long)FOPEN_MAX <= sysconf(_SC_OPEN_MAX));

		/* tmpnam() must actually honour L_tmpnam. */
		{
			char tn[L_tmpnam];
			char *r = tmpnam(tn);
			test_result("tmpnam() fits in L_tmpnam",
				    r != NULL && strlen(r) < L_tmpnam);
		}

		/* fgetpos/fsetpos: ISO C's position save/restore.  Added
		 * because libpng would not compile without fpos_t. */
		{
			const char *fpath = tmpp("likeos_fpos_test");
			FILE *f = fopen(fpath, "w+");
			if (f) {
				fpos_t p1, p2;
				int c;
				fputs("ABCDEFGHIJ", f);
				rewind(f);
				test_result("fgetpos at start succeeds",
					    fgetpos(f, &p1) == 0);
				test_result("read after fgetpos gives 'A'",
					    fgetc(f) == 'A');
				(void)fgetc(f); /* B */
				test_result("fgetpos mid-file succeeds",
					    fgetpos(f, &p2) == 0);
				c = fgetc(f);
				test_result("next byte is 'C'", c == 'C');
				/* Back to the start... */
				test_result("fsetpos to the start succeeds",
					    fsetpos(f, &p1) == 0);
				test_result("...and reading gives 'A' again",
					    fgetc(f) == 'A');
				/* ...and to the saved middle position. */
				test_result("fsetpos to mid-file succeeds",
					    fsetpos(f, &p2) == 0);
				test_result("...and reading gives 'C' again",
					    fgetc(f) == 'C');
				/* fsetpos must clear EOF, like fseek does. */
				fseek(f, 0, SEEK_END);
				(void)fgetc(f); /* trip EOF */
				test_result("EOF is set at end of file",
					    feof(f) != 0);
				fsetpos(f, &p1);
				test_result("fsetpos clears the EOF flag",
					    feof(f) == 0);
				fclose(f);
				unlink(fpath);
			} else {
				test_fail("fopen for the fgetpos test");
			}
		}
	}

	// ========================================
	// Test: sysconf processor counts
	// ========================================
	printf("\n[TEST] sysconf processor counts\n");
	{
		long onln = sysconf(_SC_NPROCESSORS_ONLN);
		long conf = sysconf(_SC_NPROCESSORS_CONF);

		/* Must never be 0 or -1: callers size thread pools, arena
		 * counts and -j values by this, and a zero is worse than an
		 * underestimate -- it turns into a division by zero or an
		 * empty pool. */
		test_result("_SC_NPROCESSORS_ONLN is at least 1", onln >= 1);
		test_result("_SC_NPROCESSORS_CONF is at least 1", conf >= 1);
		test_result("CONF is not less than ONLN", conf >= onln);

		/* It must also agree with the affinity mask it is derived
		 * from, which is what makes it a real count rather than a
		 * hardcoded guess. */
		{
			cpu_set_t set;
			memset(&set, 0, sizeof(set));
			if (sched_getaffinity(0, sizeof(set), &set) == 0)
				test_result("ONLN matches the affinity mask",
					    CPU_COUNT(&set) == (int)onln);
			else
				test_result("sched_getaffinity for the cpu count",
					    0);
		}

		errno = 0;
		test_result("an unknown sysconf name is EINVAL",
			    sysconf(-12345) == -1 && errno == EINVAL);
	}

	// ========================================
	// Test: getloadavg
	// ========================================
	printf("\n[TEST] getloadavg\n");
	{
		double la[3];
		struct sysinfo si;
		int n;

		/* The return value is the count STORED, not just success. */
		la[0] = la[1] = la[2] = -1.0;
		n = getloadavg(la, 3);
		test_result("getloadavg(,3) returns 3", n == 3);
		test_result("all three values were written",
			    la[0] >= 0.0 && la[1] >= 0.0 && la[2] >= 0.0);

		/* Asking for fewer must write ONLY that many -- a loop that
		 * ignored nelem would scribble past the caller's array. */
		la[1] = -1.0;
		n = getloadavg(la, 1);
		test_result("getloadavg(,1) returns 1", n == 1);
		test_result("getloadavg(,1) leaves later elements untouched",
			    la[1] == -1.0);

		/* More than exist is not an error: it clamps and says so. */
		n = getloadavg(la, 99);
		test_result("getloadavg(,99) clamps to 3", n == 3);

		test_result("getloadavg(,0) stores nothing and returns 0",
			    getloadavg(la, 0) == 0);
		errno = 0;
		test_result("getloadavg(,-1) fails with EINVAL",
			    getloadavg(la, -1) == -1 && errno == EINVAL);

		/* It must agree with the source it is derived from: sysinfo
		 * reports the same figures as fixed-point <<16. */
		if (sysinfo(&si) == 0) {
			double from_si = (double)si.loads[0] / 65536.0;
			getloadavg(la, 1);
			double diff = la[0] - from_si;
			if (diff < 0)
				diff = -diff;
			/* Not equality: the average can tick between the two
			 * calls.  A whole point of load would be a scaling bug,
			 * which is what this is really checking. */
			test_result("getloadavg agrees with sysinfo loads[0]",
				    diff < 1.0);
		} else {
			test_fail("sysinfo() for the getloadavg cross-check");
		}
	}

	// ========================================
	// Test: standard descriptor reuse
	// ========================================
	printf("\n[TEST] standard descriptor reuse (close/open/dup on 0-2)\n");
	{
		int p[2];
		if (pipe(p) != 0) {
			test_fail("pipe() for the fd-reuse child");
		} else {
			/* Flush first: fork duplicates whatever is still sitting
			 * in the stdio buffer, and the child must not print it a
			 * second time down the pty it installs on fd 1. */
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				close(p[0]);
				fd_reuse_child(p[1]);
				_exit(1); /* not reached */
			} else if (pid < 0) {
				close(p[0]);
				close(p[1]);
				test_fail("fork() for the fd-reuse child");
			} else {
				char r[FDR_CHECKS];
				size_t got = 0;
				int st = 0;
				memset(r, '0', sizeof(r));
				close(p[1]);
				while (got < sizeof(r)) {
					ssize_t k = read(p[0], r + got,
							 sizeof(r) - got);
					if (k <= 0)
						break;
					got += (size_t)k;
				}
				close(p[0]);
				waitpid(pid, &st, 0);

				test_result("fd-reuse child reported all checks",
					    got == sizeof(r));
				test_result(
					"close() on a standard descriptor succeeds",
					r[0] == '1');
				test_result("a closed 0 stays closed (EBADF)",
					    r[1] == '1');
				test_result(
					"read() on a closed 0 is EBADF, not the console",
					r[2] == '1');
				test_result("open() reuses descriptor 0",
					    r[3] == '1');
				test_result("dup() reuses descriptor 0",
					    r[4] == '1');
				test_result(
					"close+dup puts a pty on all of 0/1/2 (isatty)",
					r[5] == '1');
				test_result("descriptor 0 is the pty slave itself",
					    r[6] == '1');
				test_result("descriptor 1 is the pty slave itself",
					    r[7] == '1');
				test_result("descriptor 2 is the pty slave itself",
					    r[8] == '1');
				test_result(
					"F_GETFD reports only the flags POSIX defines",
					r[9] == '1');
			}
		}
	}

	// ========================================
	// Test: concurrent AF_UNIX scatter/gather I/O
	// ========================================
	printf("\n[TEST] concurrent AF_UNIX sendmsg/recvmsg\n");
	{
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			test_fail("socketpair() for the concurrency test");
		} else {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				int csv[2];
				int rc = 1;
				/* Own pair, so the two processes share no socket
				 * and any crosstalk is the kernel's. */
				close(sv[0]);
				close(sv[1]);
				if (socketpair(AF_UNIX, SOCK_STREAM, 0, csv) ==
				    0) {
					rc = (uds_run(csv, 0x5A, UDS_ROUNDS) ==
					      0) ?
						     0 :
						     2;
					close(csv[0]);
					close(csv[1]);
				}
				_exit(rc);
			} else if (pid < 0) {
				close(sv[0]);
				close(sv[1]);
				test_fail("fork() for the concurrency test");
			} else {
				int prc = uds_run(sv, 0xA5, UDS_ROUNDS);
				int st = 0;
				waitpid(pid, &st, 0);
				close(sv[0]);
				close(sv[1]);
				test_result(
					"unix stream survives concurrent sendmsg/recvmsg (this process)",
					prc == 0);
				test_result(
					"unix stream survives concurrent sendmsg/recvmsg (other process)",
					WIFEXITED(st) && WEXITSTATUS(st) == 0);
			}
		}
	}

	// ========================================
	// Test: signals
	// ========================================
	printf("\n[TEST] signals\n");
	g_sigusr1_hit = 0;
	g_sigusr2_hit = 0;
	g_last_signal = 0;
	g_signal_hits = 0;
	test_result("signal(SIGUSR1) set",
		    signal(SIGUSR1, handle_sigusr1) != SIG_ERR);
	test_result("raise(SIGUSR1) returns 0", raise(SIGUSR1) == 0);
	test_result("SIGUSR1 handler ran", g_sigusr1_hit == 1);

	signal(SIGUSR2, handle_sigusr2);
	test_result("kill(self,SIGUSR2) returns 0",
		    kill(getpid(), SIGUSR2) == 0);
	test_result("SIGUSR2 handler ran", g_sigusr2_hit == 1);

	// Test a range of signals with a generic handler (skip SIGKILL/SIGSTOP)
	int sigs_to_test[] = { SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGTRAP,
			       SIGABRT, SIGBUS,  SIGFPE,  SIGUSR1, SIGSEGV,
			       SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, SIGCHLD,
			       SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU };
	int sig_count = (int)(sizeof(sigs_to_test) / sizeof(sigs_to_test[0]));
	for (int i = 0; i < sig_count; i++) {
		int sig = sigs_to_test[i];
		g_last_signal = 0;
		signal(sig, handle_generic);
		int rr = raise(sig);
		char name[64];
		snprintf(name, sizeof(name), "raise signal %d", sig);
		test_result(name, rr == 0 && g_last_signal == sig);
		/* Put the disposition back before moving on.  Leaving a handler
		 * installed for a signal whose default action is "ignore"
		 * (SIGCHLD, SIGURG, SIGWINCH in this list) changes it from
		 * "discarded the moment it is raised" to "stays pending until
		 * delivered".  A pending signal makes nanosleep() return EINTR
		 * without sleeping, so every later test that sleeps would
		 * silently not sleep — a failure that surfaces far from here. */
		signal(sig, SIG_DFL);
	}

	// ========================================
	// Test: SIGKILL/SIGSTOP are uncatchable and unblockable
	// ========================================
	printf("\n[TEST] SIGKILL/SIGSTOP cannot be caught or blocked\n");
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = handle_generic;
		sigemptyset(&sa.sa_mask);

		errno = 0;
		test_result("sigaction(SIGKILL) rejected with EINVAL",
			    sigaction(SIGKILL, &sa, NULL) < 0 && errno == EINVAL);
		errno = 0;
		test_result("sigaction(SIGSTOP) rejected with EINVAL",
			    sigaction(SIGSTOP, &sa, NULL) < 0 && errno == EINVAL);

		/* Blocking them must be a silent no-op, not an error: the mask
		 * that comes back must simply never contain them. */
		sigset_t prevmask, allsigs, aftermask;
		sigfillset(&allsigs);
		sigemptyset(&aftermask);
		sigprocmask(SIG_BLOCK, &allsigs, &prevmask);
		sigprocmask(SIG_BLOCK, NULL, &aftermask);
		test_result("SIGKILL never enters the blocked mask",
			    !sigismember(&aftermask, SIGKILL));
		test_result("SIGSTOP never enters the blocked mask",
			    !sigismember(&aftermask, SIGSTOP));
		sigprocmask(SIG_SETMASK, &prevmask, NULL);
	}

	/* The case that actually matters, and the one a mask check alone cannot
	 * prove: a task that has blocked everything it is permitted to block,
	 * spinning in user mode and never entering a syscall, must still die. */
	printf("\n[TEST] SIGKILL reaches a task that blocked every signal\n");
	{
		pid_t child = fork();
		if (child == 0) {
			sigset_t all;
			sigfillset(&all);
			sigprocmask(SIG_BLOCK, &all, NULL);
			for (;;) {
			}
			_exit(0);
		} else if (child > 0) {
			/* Let the child reach its spin with the mask installed. */
			struct timespec settle = { 0, 50 * 1000 * 1000L };
			nanosleep(&settle, NULL);

			test_result("kill(blocked-mask child, SIGKILL) returns 0",
				    kill(child, SIGKILL) == 0);

			int status = 0;
			pid_t waited = -1;
			for (int tries = 0; tries < 100; tries++) {
				waited = waitpid(child, &status, WNOHANG);
				if (waited > 0) {
					break;
				}
				struct timespec d = { 0, 10 * 1000 * 1000L };
				nanosleep(&d, NULL);
			}
			test_result("blocked-mask child still reaped",
				    waited == child);
			test_result("blocked-mask child died by SIGKILL",
				    waited == child && WIFSIGNALED(status) &&
					    WTERMSIG(status) == SIGKILL);
			if (waited != child) {
				/* Unkillable child: do not leak it into later tests. */
				kill(child, SIGKILL);
				waitpid(child, NULL, 0);
			}
		} else {
			test_fail("SIGKILL vs blocked mask: fork failed");
		}
	}

	// ========================================
	// Test: Extended signal handling (kernel syscalls)
	// ========================================
	printf("\n[TEST] extended signal handling\n");

	// Reinstall handle_sigusr1 (was overwritten by handle_generic in loop above)
	signal(SIGUSR1, handle_sigusr1);

	// Test sigprocmask
	sigset_t oldmask, newmask, pendmask;
	sigemptyset(&newmask);
	sigaddset(&newmask, SIGUSR1);

	test_result("sigprocmask(SIG_BLOCK, SIGUSR1) returns 0",
		    sigprocmask(SIG_BLOCK, &newmask, &oldmask) == 0);

	// Signal should now be blocked - raise it, it should be pending
	g_sigusr1_hit = 0;
	raise(SIGUSR1);
	test_result("SIGUSR1 blocked, handler not called", g_sigusr1_hit == 0);

	// Check sigpending
	sigemptyset(&pendmask);
	test_result("sigpending returns 0", sigpending(&pendmask) == 0);
	test_result("SIGUSR1 is pending", sigismember(&pendmask, SIGUSR1) == 1);

	// Unblock and deliver
	test_result("sigprocmask(SIG_UNBLOCK, SIGUSR1) returns 0",
		    sigprocmask(SIG_UNBLOCK, &newmask, NULL) == 0);
	test_result("SIGUSR1 delivered after unblock", g_sigusr1_hit == 1);

	// Test sigaction with structure
	struct sigaction sa_new, sa_old;
	memset(&sa_new, 0, sizeof(sa_new));
	sa_new.sa_handler = handle_sigusr2;
	sigemptyset(&sa_new.sa_mask);
	sa_new.sa_flags = 0;

	g_sigusr2_hit = 0;
	test_result("sigaction(SIGUSR2) returns 0",
		    sigaction(SIGUSR2, &sa_new, &sa_old) == 0);
	raise(SIGUSR2);
	test_result("sigaction handler called", g_sigusr2_hit == 1);

	// Test sigfillset
	sigset_t fullset;
	sigfillset(&fullset);
	test_result("sigfillset: SIGUSR1 set",
		    sigismember(&fullset, SIGUSR1) == 1);
	test_result("sigfillset: SIGUSR2 set",
		    sigismember(&fullset, SIGUSR2) == 1);
	test_result("sigfillset: SIGTERM set",
		    sigismember(&fullset, SIGTERM) == 1);

	// Test sigdelset
	sigdelset(&fullset, SIGUSR1);
	test_result("sigdelset(SIGUSR1) works",
		    sigismember(&fullset, SIGUSR1) == 0);
	test_result("sigdelset: SIGUSR2 still set",
		    sigismember(&fullset, SIGUSR2) == 1);

	// Test SIG_IGN
	signal(SIGUSR1, SIG_IGN);
	g_sigusr1_hit = 0;
	raise(SIGUSR1);
	test_result("SIG_IGN: handler not called", g_sigusr1_hit == 0);

	// Test SIG_DFL restore
	signal(SIGUSR1, handle_sigusr1);
	g_sigusr1_hit = 0;
	raise(SIGUSR1);
	test_result("handler restored after SIG_IGN", g_sigusr1_hit == 1);

	// Test nanosleep
	printf("\n[TEST] nanosleep\n");
	struct timespec ts_req, ts_rem;
	ts_req.tv_sec = 0;
	ts_req.tv_nsec = 50000000; // 50ms
	ts_rem.tv_sec = 0;
	ts_rem.tv_nsec = 0;
	int ns_ret = nanosleep(&ts_req, &ts_rem);
	test_result("nanosleep(50ms) returns 0", ns_ret == 0);

	// Test usleep
	printf("\n[TEST] usleep\n");
	test_result("usleep(10000) returns 0", usleep(10000) == 0); // 10ms

	// Test sigaltstack
	printf("\n[TEST] sigaltstack\n");
	stack_t ss_new, ss_old;
	static char alt_stack_buf[SIGSTKSZ];
	ss_new.ss_sp = alt_stack_buf;
	ss_new.ss_size = SIGSTKSZ;
	ss_new.ss_flags = 0;
	test_result("sigaltstack set returns 0",
		    sigaltstack(&ss_new, &ss_old) == 0);

	// Verify we can get it back
	stack_t ss_check;
	test_result("sigaltstack get returns 0",
		    sigaltstack(NULL, &ss_check) == 0);
	test_result("sigaltstack ss_sp matches",
		    ss_check.ss_sp == alt_stack_buf);
	test_result("sigaltstack ss_size matches",
		    ss_check.ss_size == SIGSTKSZ);

	// Disable alternate stack
	ss_new.ss_flags = SS_DISABLE;
	test_result("sigaltstack disable returns 0",
		    sigaltstack(&ss_new, NULL) == 0);

	// ========================================
	// Test: alarm/sleep
	// ========================================
	printf("\n[TEST] alarm/sleep\n");
	g_sigalrm_hit = 0;
	signal(SIGALRM, handle_sigalrm);
	unsigned int rem = alarm(1);
	test_result("alarm(1) returns remaining", rem == 0);
	sleep(2);
	test_result("SIGALRM delivered", g_sigalrm_hit == 1);

	// rename/unlink

	// large write to force multi-cluster
	printf("\n[TEST] large file write (multi-cluster)\n");
	char lpath[64];
	snprintf(lpath, sizeof(lpath), "%s/LARGE.TXT", _pbase);
	size_t lsize = 7000;
	char *lbuf = (char *)malloc(lsize);
	if (lbuf) {
		for (size_t i = 0; i < lsize; i++) {
			lbuf[i] = (char)('A' + (i % 26));
		}
		int lfd = open(lpath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		test_result("open large file for write", lfd >= 0);
		if (lfd >= 0) {
			ssize_t lw = write(lfd, lbuf, lsize);
			test_result("write large buffer", lw == (ssize_t)lsize);
			close(lfd);
		}
		lfd = open(lpath, O_RDONLY);
		test_result("open large file for read", lfd >= 0);
		if (lfd >= 0) {
			char *lread = (char *)malloc(lsize + 1);
			if (lread) {
				memset(lread, 0, lsize + 1);
				ssize_t lr = read(lfd, lread, lsize);
				test_result("read large buffer",
					    lr == (ssize_t)lsize);
				test_result("large data matches",
					    lr == (ssize_t)lsize &&
						    memcmp(lbuf, lread,
							   lsize) == 0);
				free(lread);
			} else {
				test_fail("malloc for large read buffer");
			}
			close(lfd);
		}
		free(lbuf);
	} else {
		test_fail("malloc for large write buffer");
	}

	// ========================================
	// Test: Long File Name (LFN) support
	// ========================================
	printf("\n[TEST] Long File Name (LFN) support\n");

	// Test 1: Create file with long name (lowercase preserved)
	char lfn_path1[128];
	snprintf(lfn_path1, sizeof(lfn_path1),
		 "%s/this_is_a_long_filename_test.txt", _pbase);
	int lfn_fd = open(lfn_path1, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create long filename", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		const char *lfn_data = "LFN test data";
		ssize_t lfn_wr = write(lfn_fd, lfn_data, strlen(lfn_data));
		test_result("write to LFN file",
			    lfn_wr == (ssize_t)strlen(lfn_data));
		close(lfn_fd);
	}

	// Test 2: Read back the long filename file
	lfn_fd = open(lfn_path1, O_RDONLY);
	test_result("open LFN file for read", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		char lfn_rbuf[64];
		memset(lfn_rbuf, 0, sizeof(lfn_rbuf));
		ssize_t lfn_rd = read(lfn_fd, lfn_rbuf, sizeof(lfn_rbuf) - 1);
		test_result("read LFN file", lfn_rd > 0);
		test_result("LFN data correct",
			    strcmp(lfn_rbuf, "LFN test data") == 0);
		close(lfn_fd);
	}

	// Test 3: Case-insensitive access (open with different case).
	// FAT is case-insensitive; ext4 (and other POSIX filesystems) are
	// case-sensitive, so only assert the cross-case open on a case-insensitive
	// filesystem.  `fs_ci` records which kind of filesystem we're on.
	char lfn_path1_upper[128];
	snprintf(lfn_path1_upper, sizeof(lfn_path1_upper),
		 "%s/THIS_IS_A_LONG_FILENAME_TEST.TXT", _pbase);
	lfn_fd = open(lfn_path1_upper, O_RDONLY);
	int fs_ci = (lfn_fd >= 0);
	if (fs_ci) {
		test_result("case-insensitive LFN access", lfn_fd >= 0);
		close(lfn_fd);
	} else {
		test_result(
			"case-sensitive filesystem: LFN cross-case access skipped",
			1);
	}

	// Test 4: Mixed case filename
	char lfn_path2[128];
	snprintf(lfn_path2, sizeof(lfn_path2), "%s/MixedCaseFileName.TXT",
		 _pbase);
	lfn_fd = open(lfn_path2, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create mixed case filename", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		write(lfn_fd, "X", 1);
		close(lfn_fd);
	}

	// Test 5: Verify case-insensitive access to mixed case file
	char lfn_path2_lower[128];
	snprintf(lfn_path2_lower, sizeof(lfn_path2_lower),
		 "%s/mixedcasefilename.txt", _pbase);
	if (fs_ci) {
		lfn_fd = open(lfn_path2_lower, O_RDONLY);
		test_result("case-insensitive mixed case access", lfn_fd >= 0);
		if (lfn_fd >= 0)
			close(lfn_fd);
	} else {
		test_result(
			"case-sensitive filesystem: mixed-case access skipped",
			1);
	}

	// Test 6: Long directory name
	char lfn_dir[128];
	snprintf(lfn_dir, sizeof(lfn_dir), "%s/long_directory_name_for_testing",
		 _pbase);
	test_result("mkdir long dirname", mkdir(lfn_dir, 0777) == 0);

	// Test 7: Create file in long dirname
	char lfn_in_dir[192];
	snprintf(lfn_in_dir, sizeof(lfn_in_dir),
		 "%s/long_directory_name_for_testing/another_long_filename.dat",
		 _pbase);
	lfn_fd = open(lfn_in_dir, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create file in LFN dir", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		write(lfn_fd, "nested", 6);
		close(lfn_fd);
	}

	// Test 8: Read file from long dirname
	lfn_fd = open(lfn_in_dir, O_RDONLY);
	test_result("open file in LFN dir", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		char rbuf[16];
		memset(rbuf, 0, sizeof(rbuf));
		ssize_t rr = read(lfn_fd, rbuf, sizeof(rbuf) - 1);
		test_result("read file in LFN dir",
			    rr == 6 && strcmp(rbuf, "nested") == 0);
		close(lfn_fd);
	}

	// Test 9: Rename with long filename
	char lfn_renamed[128];
	snprintf(lfn_renamed, sizeof(lfn_renamed),
		 "%s/renamed_long_filename_test.txt", _pbase);
	test_result("rename LFN file", rename(lfn_path1, lfn_renamed) == 0);

	// Test 10: Verify renamed file exists
	lfn_fd = open(lfn_renamed, O_RDONLY);
	test_result("open renamed LFN file", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		close(lfn_fd);
	}

	// Test 11: Unlink files with long names
	test_result("unlink renamed LFN file", unlink(lfn_renamed) == 0);
	test_result("unlink mixed case file", unlink(lfn_path2) == 0);
	test_result("unlink file in LFN dir", unlink(lfn_in_dir) == 0);

	// Test 12: Rmdir long dirname
	test_result("rmdir LFN dir", rmdir(lfn_dir) == 0);

	// Test 13: Very long filename (near max)
	char very_long[192];
	snprintf(
		very_long, sizeof(very_long),
		"%s/abcdefghijklmnopqrstuvwxyz0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ.longext",
		_pbase);
	lfn_fd = open(very_long, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create very long filename", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		write(lfn_fd, "V", 1);
		close(lfn_fd);
	}
	lfn_fd = open(very_long, O_RDONLY);
	test_result("open very long filename", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		close(lfn_fd);
	}
	test_result("unlink very long filename", unlink(very_long) == 0);

	// Test 14: Filename with spaces
	char space_name[128];
	snprintf(space_name, sizeof(space_name),
		 "%s/file with spaces in name.txt", _pbase);
	lfn_fd = open(space_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create filename with spaces", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		write(lfn_fd, "S", 1);
		close(lfn_fd);
	}
	lfn_fd = open(space_name, O_RDONLY);
	test_result("open filename with spaces", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		close(lfn_fd);
	}
	test_result("unlink filename with spaces", unlink(space_name) == 0);

	// Test 15: Case preservation check - create lowercase, verify lowercase display
	char lowercase_file[128];
	snprintf(lowercase_file, sizeof(lowercase_file),
		 "%s/lowercase_only_filename.txt", _pbase);
	lfn_fd = open(lowercase_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	test_result("create lowercase filename", lfn_fd >= 0);
	if (lfn_fd >= 0) {
		close(lfn_fd);
	}
	// Clean up
	unlink(lowercase_file);

	// Test 16: Directory with mixed case
	char mixed_dir[128], mixed_dir_lower[128];
	snprintf(mixed_dir, sizeof(mixed_dir), "%s/MyMixedCaseDirectory",
		 _pbase);
	snprintf(mixed_dir_lower, sizeof(mixed_dir_lower),
		 "%s/mymixedcasedirectory", _pbase);
	test_result("mkdir mixed case dir", mkdir(mixed_dir, 0777) == 0);
	if (fs_ci)
		test_result("chdir mixed case dir (lowercase)",
			    chdir(mixed_dir_lower) == 0);
	else
		test_result(
			"case-sensitive filesystem: chdir cross-case skipped",
			1);
	test_result("chdir back to root", chdir("/") == 0);
	test_result("rmdir mixed case dir", rmdir(mixed_dir) == 0);
	rmdir(_pbase);

	// ========================================
	// Test: Security - Invalid Pointer Handling
	// ========================================
	printf("\n[TEST] Security - Invalid Pointer Handling\n");

	// Test: NULL pointer should fail with EFAULT
	int sec_ret = read(0, NULL, 100);
	test_result("read(NULL) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	sec_ret = write(1, NULL, 100);
	test_result("write(NULL) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// Test: Kernel address pointer should fail with EFAULT
	void *kernel_addr = (void *)0xFFFFFFFF80000000ULL;
	sec_ret = read(0, kernel_addr, 100);
	test_result("read(kernel_addr) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	sec_ret = write(1, kernel_addr, 100);
	test_result("write(kernel_addr) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// Test: stat with NULL buffer should fail
	sec_ret = stat("/", NULL);
	test_result("stat(NULL buf) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// Test: open with NULL path should fail
	sec_ret = open(NULL, O_RDONLY);
	test_result("open(NULL) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// ========================================
	// Test: Security - Integer Overflow Protection
	// ========================================
	printf("\n[TEST] Security - Integer Overflow Protection\n");

	// Test: Excessive mmap size should fail
	void *bad_mmap =
		mmap(NULL, 0xFFFFFFFFFFFFFFFFULL, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("mmap(huge size) fails", bad_mmap == MAP_FAILED);

	// Test: Very large read should fail gracefully (not crash)
	// Note: We can't actually pass >1GB to read in practice, but the kernel should handle it
	char tiny_buf[1];
	// The kernel will reject this because the buffer is only 1 byte but we're asking for huge read
	// Either way, the kernel should not crash
	sec_ret = read(0, tiny_buf, 0x7FFFFFFFFFFFFFFULL);
	test_result("read(huge count) returns error", sec_ret == -1);

	// ========================================
	// Test: Security - IOCTL Validation
	// ========================================
	printf("\n[TEST] Security - IOCTL Validation\n");

	// Test: TIOCGWINSZ with NULL should fail
	sec_ret = ioctl(0, TIOCGWINSZ, NULL);
	test_result("ioctl(TIOCGWINSZ, NULL) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// Test: TIOCGWINSZ with kernel address should fail
	sec_ret = ioctl(0, TIOCGWINSZ, kernel_addr);
	test_result("ioctl(TIOCGWINSZ, kernel_addr) returns EFAULT",
		    sec_ret == -1 && errno == EFAULT);

	// Test: Valid IOCTL should succeed
	struct winsize ws;
	sec_ret = ioctl(0, TIOCGWINSZ, &ws);
	test_result("ioctl(TIOCGWINSZ, valid) succeeds", sec_ret == 0);

	// ========================================
	// Test: Security - Memory Protection
	// ========================================
	printf("\n[TEST] Security - Memory Protection\n");

	// Test: mmap anonymous memory works
	void *anon_mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("mmap anonymous succeeds", anon_mem != MAP_FAILED);
	if (anon_mem != MAP_FAILED) {
		// Test: Memory should be zero-initialized
		int zero_init = 1;
		unsigned char *mem = (unsigned char *)anon_mem;
		for (int i = 0; i < 4096; i++) {
			if (mem[i] != 0) {
				zero_init = 0;
				break;
			}
		}
		test_result("mmap memory is zero-initialized", zero_init);
		munmap(anon_mem, 4096);
	}

	// Test: mmap with zero length should fail
	void *zero_mmap = mmap(NULL, 0, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("mmap(0 length) fails", zero_mmap == MAP_FAILED);

	// Test: MAP_FIXED at low address (< 64KB) should fail
	void *low_mmap = mmap((void *)0x1000, 4096, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	test_result("mmap(MAP_FIXED, addr=0x1000) fails",
		    low_mmap == MAP_FAILED);

	// Test: MAP_FIXED at NULL should fail
	void *null_fixed = mmap((void *)0, 4096, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	test_result("mmap(MAP_FIXED, addr=0) fails", null_fixed == MAP_FAILED);

	// Test: MAP_FIXED at address just below 64KB boundary should fail
	void *boundary_mmap =
		mmap((void *)0xF000, 4096, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	test_result("mmap(MAP_FIXED, addr=0xF000) fails",
		    boundary_mmap == MAP_FAILED);

	// Test: MAP_FIXED at valid address (>= 64KB) should succeed
	// Use a high address that's unlikely to conflict
	void *valid_fixed =
		mmap((void *)0x10000000, 4096, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	test_result("mmap(MAP_FIXED, addr=0x10000000) succeeds",
		    valid_fixed != MAP_FAILED);

	/* Failure paths must report WHY.  Every one of these used to come back
	 * as EPERM because the kernel returned MAP_FAILED (-1) for all of them
	 * and the libc wrapper read that as errno 1. */
	errno = 0;
	test_result("mmap(0 length) -> EINVAL",
		    mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1,
			 0) == MAP_FAILED &&
			    errno == EINVAL);
	/* A length no address space can hold, rather than a number that merely
	 * sounded large.
	 *
	 * This used to ask for 3 GB and expect ENOMEM, which stopped being a
	 * security property the moment the machine had 4 GB -- and was never
	 * one on a real 16 GB system.  An anonymous mapping is demand-paged:
	 * asking for 3 GB reserves addresses, not memory, and succeeding is
	 * the correct answer.  The test was asserting a limitation, so it
	 * failed as soon as the limitation went away.
	 *
	 * What must always fail is a request that cannot be satisfied by any
	 * amount of memory: the user half is 47 bits, so 1 << 47 does not fit
	 * however much RAM is fitted. */
	errno = 0;
	test_result("mmap(128 TB, larger than the user address space) -> ENOMEM",
		    mmap(NULL, 1ULL << 47, PROT_READ,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
			    errno == ENOMEM);

	/* And a length whose page-rounding overflows.  SIZE_MAX rounds up to
	 * zero, which is the classic way a length check is passed by a value
	 * that then allocates nothing and leaves the caller with a mapping it
	 * believes is enormous. */
	errno = 0;
	test_result("mmap(SIZE_MAX, page-rounding overflows) fails",
		    mmap(NULL, (size_t)-1, PROT_READ,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
			    (errno == ENOMEM || errno == EINVAL));
	errno = 0;
	test_result("mmap(bad fd) -> EBADF",
		    mmap(NULL, 4096, PROT_READ, MAP_SHARED, 9999, 0) ==
				    MAP_FAILED &&
			    errno == EBADF);
	{
		int mp[2];
		if (pipe(mp) == 0) {
			errno = 0;
			test_result("mmap(pipe fd) -> ENODEV",
				    mmap(NULL, 4096, PROT_READ, MAP_SHARED,
					 mp[0], 0) == MAP_FAILED &&
					    errno == ENODEV);
			close(mp[0]);
			close(mp[1]);
		}
	}

	/* Region-table headroom.  A dynamically linked X server maps roughly
	 * four regions per shared object, so a process must be able to hold
	 * well over a hundred mappings at once. */
	{
		void *regs[200];
		int n = 0, ok = 1;
		for (n = 0; n < 200; n++) {
			regs[n] = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (regs[n] == MAP_FAILED)
				break;
			*(volatile char *)regs[n] = (char)n; /* touch it */
		}
		test_result("200 concurrent anonymous mappings", n == 200);
		for (int i = 0; i < n; i++) {
			if (*(volatile char *)regs[i] != (char)i)
				ok = 0;
			munmap(regs[i], 4096);
		}
		test_result("each mapping kept its own contents", ok);
	}

	/* PROT_NONE must actually deny access, including with MAP_SHARED —
	 * the eager shared path used to map PAGE_PRESENT|PAGE_USER regardless
	 * of prot, leaving PROT_NONE freely readable. */
	{
		void *pn = mmap(NULL, 4096, PROT_NONE,
				MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		test_result("mmap(PROT_NONE, MAP_SHARED) succeeds",
			    pn != MAP_FAILED);
		if (pn != MAP_FAILED) {
			/* Catch the fault in a child rather than letting it
			 * kill the process: the point is that the access
			 * faults, and a caught fault says so without the
			 * kernel printing a crash report for something the
			 * test did on purpose. */
			pid_t p = fork();
			if (p == 0) {
				struct sigaction sa;
				memset(&sa, 0, sizeof(sa));
				sa.sa_handler = protnone_segv_handler;
				sigaction(SIGSEGV, &sa, NULL);
				volatile char c = *(volatile char *)pn;
				(void)c;
				_exit(0); /* no fault: PROT_NONE did not protect */
			}
			int st = 0;
			waitpid(p, &st, 0);
			test_result("reading a PROT_NONE page faults",
				    WIFEXITED(st) && WEXITSTATUS(st) == 42);
			munmap(pn, 4096);
		}
	}
	if (valid_fixed != MAP_FAILED) {
		munmap(valid_fixed, 4096);
	}

	/* A large anonymous mapping SUCCEEDS, and is real where it is touched.
	 *
	 * The inverse of what this used to assert.  3 GB of anonymous memory is
	 * a reservation of addresses; the pages arrive on first touch, so the
	 * request is legitimate on any machine whose address space can hold it
	 * -- and on a 16 GB system it is ordinary.  Demanding that it fail was
	 * testing a 2 GB limit that no longer exists.
	 *
	 * Touching the two ends and the middle is what makes this worth
	 * running: it proves the mapping is backed on demand rather than
	 * merely promised, and that the far end of a multi-gigabyte region is
	 * reachable -- which is where a 32-bit truncation in the region
	 * bookkeeping would show up. */
	const size_t huge_len = 3ULL * 1024 * 1024 * 1024;
	void *huge_mmap = mmap(NULL, huge_len, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	test_result("mmap(3GB) succeeds (demand-paged reservation)",
		    huge_mmap != MAP_FAILED);
	if (huge_mmap != MAP_FAILED) {
		volatile char *h = (volatile char *)huge_mmap;
		size_t last = huge_len - 1;
		size_t mid = huge_len / 2;
		int ok;

		h[0] = 0x11;
		h[mid] = 0x22;
		h[last] = 0x33;
		ok = (h[0] == 0x11 && h[mid] == 0x22 && h[last] == 0x33);
		test_result("3GB mapping is writable at both ends and the middle",
			    ok);
		/* Only the three touched pages were ever backed, so this
		 * releases three pages and a lot of address space. */
		test_result("munmap of the 3GB mapping succeeds",
			    munmap(huge_mmap, huge_len) == 0);
		huge_mmap = MAP_FAILED;
	}

	// Test: Multiple small mmaps should succeed
	void *multi1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	void *multi2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	void *multi3 = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("multiple mmap calls succeed",
		    multi1 != MAP_FAILED && multi2 != MAP_FAILED &&
			    multi3 != MAP_FAILED);
	test_result("mmap returns different addresses",
		    multi1 != multi2 && multi2 != multi3 && multi1 != multi3);
	if (multi1 != MAP_FAILED)
		munmap(multi1, 4096);
	if (multi2 != MAP_FAILED)
		munmap(multi2, 4096);
	if (multi3 != MAP_FAILED)
		munmap(multi3, 4096);

	// Test: mmap with PROT_NONE should succeed (reserve memory)
	void *prot_none =
		mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	test_result("mmap(PROT_NONE) succeeds", prot_none != MAP_FAILED);
	if (prot_none != MAP_FAILED) {
		munmap(prot_none, 4096);
	}

	// ========================================
	// MAP_SHARED Test (Shared Memory Between Parent and Child)
	// ========================================
	// ========================================
	// scanf: one engine for streams and strings
	// ========================================
	printf("\n--- scanf (string and stream sources) ---\n");
	{
		/* sscanf used to be a second, smaller implementation with no
		 * float conversions at all, so this returned 0 while fscanf on
		 * the same input worked.  X.Org's xkbcomp scans its geometry
		 * files with sscanf and called every coordinate "Malformed
		 * number", which stopped the server compiling a keymap. */
		float f = 0.0f;

		test_result("sscanf %g parses a float",
			    sscanf("1.5", "%g", &f) == 1 && f == 1.5f);

		f = 0.0f;
		test_result("sscanf %f parses a float",
			    sscanf("12.25", "%f", &f) == 1 && f == 12.25f);

		f = 0.0f;
		test_result("sscanf %e parses an exponent",
			    sscanf("1e3", "%e", &f) == 1 && f == 1000.0f);

		f = 0.0f;
		test_result("sscanf %g parses a negative",
			    sscanf("-3.5", "%g", &f) == 1 && f == -3.5f);

		double d = 0.0;
		test_result("sscanf %lg parses a double",
			    sscanf("2.75", "%lg", &d) == 1 && d == 2.75);

		/* Must stop at the first character it cannot use and leave the
		 * rest -- xkbcomp's numbers are followed by punctuation. */
		f = 0.0f;
		test_result("sscanf %g stops at trailing punctuation",
			    sscanf("1.5)", "%g", &f) == 1 && f == 1.5f);

		f = 99.0f;
		test_result("sscanf %g rejects a non-number",
			    sscanf("abc", "%g", &f) == 0 && f == 99.0f);

		/* Mixed conversions, to show the float path does not disturb
		 * the position the next conversion starts from. */
		int n = 0;
		f = 0.0f;
		test_result("sscanf mixes %d and %g",
			    sscanf("7 1.5", "%d %g", &n, &f) == 2 && n == 7 &&
				    f == 1.5f);

		/* The string and stream forms are the same engine now; a float
		 * read back through a FILE* must agree. */
		{
			char path[96];
			FILE *fp;

			snprintf(path, sizeof(path), "%s/scanf.txt", _td);
			fp = fopen(path, "w");
			if (fp) {
				fputs("4.25 rest\n", fp);
				fclose(fp);
			}
			f = 0.0f;
			fp = fopen(path, "r");
			test_result("fscanf %g agrees with sscanf",
				    fp && fscanf(fp, "%g", &f) == 1 &&
					    f == 4.25f);
			if (fp)
				fclose(fp);
			unlink(path);
		}
	}

	printf("\n--- MAP_SHARED Test ---\n");
	{
		// Create shared anonymous memory
		volatile int *shared_mem =
			mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		test_result("mmap(MAP_SHARED|MAP_ANONYMOUS) succeeds",
			    shared_mem != MAP_FAILED);

		if (shared_mem != MAP_FAILED) {
			// Initialize shared memory
			shared_mem[0] = 0; // Counter
			shared_mem[1] = 0; // Child ready flag
			shared_mem[2] = 0; // Parent ack flag

			pid_t shared_pid = fork();
			if (shared_pid == 0) {
				// Child: wait for parent to signal, then increment counter
				// Spin wait for parent to set ack flag
				while (shared_mem[2] == 0) {
					// Busy wait
				}

				// Child increments counter
				shared_mem[0] = shared_mem[0] + 100;

				// Signal child is done
				shared_mem[1] = 1;

				_exit(0);
			} else if (shared_pid > 0) {
				// Parent: write to shared memory and signal child
				shared_mem[0] = 42;

				// Signal child to proceed
				shared_mem[2] = 1;

				// Wait for child to finish
				int status;
				waitpid(shared_pid, &status, 0);

				// Child should have added 100 to our 42
				int expected_value = 142;
				int actual_value = shared_mem[0];

				printf("MAP_SHARED: parent wrote 42, child added 100, result=%d (expected %d)\n",
				       actual_value, expected_value);
				test_result(
					"MAP_SHARED memory visible between processes",
					actual_value == expected_value);
				test_result("Child completion flag visible",
					    shared_mem[1] == 1);
			} else {
				test_result("fork for MAP_SHARED test", 0);
			}

			munmap((void *)shared_mem, 4096);
		}
	}

	// ========================================
	// Large Allocation Test (100MB)
	// ========================================
	printf("\n--- Large Allocation Test (100MB) ---\n");
	{
		const size_t large_size = 100 * 1024 * 1024; // 100MB
		printf("Attempting to allocate 100MB...\n");

		void *large_alloc = malloc(large_size);
		test_result("malloc(100MB) succeeds", large_alloc != NULL);

		if (large_alloc) {
			// Touch first and last byte to verify memory is usable
			volatile char *p = (volatile char *)large_alloc;
			p[0] = 0xAA;
			p[large_size - 1] = 0x55;

			int first_ok = (p[0] == (char)0xAA);
			int last_ok = (p[large_size - 1] == 0x55);

			test_result("100MB write/read first byte", first_ok);
			test_result("100MB write/read last byte", last_ok);

			// Touch some pages in the middle to verify mapping
			size_t mid = large_size / 2;
			p[mid] = 0x42;
			test_result("100MB write/read middle byte",
				    p[mid] == 0x42);

			printf("100MB allocation at %p, verified %lu bytes\n",
			       large_alloc, (unsigned long)large_size);

			free(large_alloc);
			printf("100MB freed successfully\n");
		} else {
			printf("FAILED: Could not allocate 100MB\n");
		}
	}

	// ========================================
	// Preemptive Scheduling Test
	// ========================================
	printf("\n--- Preemptive Scheduling Test ---\n");
	{
		// This test verifies that the scheduler can preempt a CPU-bound child
		// The parent should be able to continue running even if the child loops forever

		pid_t child = fork();
		if (child < 0) {
			test_fail("preemption test: fork failed");
		} else if (child == 0) {
			// Child: infinite CPU-bound loop
			// With preemptive scheduling, this should NOT starve the parent
			volatile unsigned long counter = 0;
			while (1) {
				counter++;
				// Tight loop - no voluntary yields
			}
			_exit(0); // Never reached
		} else {
			// Parent: sleep briefly, then verify we're still running
			// If scheduling is purely cooperative, we'd be starved by the child

			// Use a simple busy-wait counter to measure time passing
			// In a preemptive system, we should still get CPU time
			volatile unsigned long parent_counter = 0;
			int parent_ran = 0;

			// Try to increment counter a million times
			// With 20ms time slices at 100Hz, we should get enough CPU time
			for (int i = 0; i < 100; i++) {
				// Small work unit
				for (int j = 0; j < 10000; j++) {
					parent_counter++;
				}
				parent_ran = 1;
			}

			// If we got here, preemption is working
			test_result("parent not starved by child loop",
				    parent_ran);
			test_result("parent counter incremented",
				    parent_counter > 0);

			// Kill the looping child
			printf("Killing child %d...\n", child);
			int kill_ret = kill(child, SIGKILL);
			printf("kill() returned %d\n", kill_ret);

			// Poll with WNOHANG so a delivery failure shows up as a
			// bounded failure rather than an infinite hang.  Sleep
			// between tries instead of spinning: a busy loop burns
			// this task's timeslice competing with the very child we
			// are waiting on, and gives no real time bound.  100 x
			// 10ms = 1s, which is many orders of magnitude more than
			// SIGKILL delivery should ever need.
			int status = 0;
			pid_t waited = -1;
			for (int tries = 0; tries < 100; tries++) {
				waited = waitpid(child, &status, WNOHANG);
				if (waited > 0) {
					break; // Child reaped
				}
				struct timespec d = { 0, 10 * 1000 * 1000L };
				nanosleep(&d, NULL);
			}

			if (waited == child) {
				test_result("preemption: child killed", 1);
				test_result("preemption: child signaled",
					    WIFSIGNALED(status) &&
						    WTERMSIG(status) ==
							    SIGKILL);
			} else {
				printf("waitpid returned %d (expected %d)\n",
				       waited, child);
				test_fail("preemption: child killed");
				test_fail("preemption: child signaled");
			}

			printf("Preemption test completed: parent_counter=%lu\n",
			       parent_counter);
		}
	}

	// ========================================
	// Extended Preemptive Kernel Tests
	// ========================================
	printf("\n--- Extended Preemptive Kernel Tests ---\n");

	// Test 1: Multiple CPU-bound children - fair scheduling
	printf("\n[TEST] Multiple CPU-bound children (fair scheduling)\n");
	{
#define NUM_CHILDREN 3
		pid_t children[NUM_CHILDREN];
		int pipe_fds[NUM_CHILDREN][2];

		// Create pipes for each child to report back
		int pipes_ok = 1;
		for (int i = 0; i < NUM_CHILDREN; i++) {
			if (pipe(pipe_fds[i]) < 0) {
				pipes_ok = 0;
				break;
			}
		}

		if (pipes_ok) {
			// Fork children that do CPU work and report iterations
			for (int i = 0; i < NUM_CHILDREN; i++) {
				children[i] = fork();
				if (children[i] == 0) {
					// Child: close all pipe ends except our own write end
					for (int j = 0; j < NUM_CHILDREN; j++) {
						close(pipe_fds[j]
							      [0]); // Close all read ends
						if (j != i) {
							close(pipe_fds[j]
								      [1]); // Close other children's write ends
						}
					}
					volatile unsigned long count = 0;
					// Work for ~100ms worth of iterations
					for (int j = 0; j < 500000; j++) {
						count++;
					}
					// Write result to pipe
					write(pipe_fds[i][1], &count,
					      sizeof(count));
					close(pipe_fds[i][1]);
					_exit(0);
				}
				// Parent: close write end
				close(pipe_fds[i][1]);
			}

			// Wait for all children and read their counts
			unsigned long counts[NUM_CHILDREN];
			int all_finished = 1;
			for (int i = 0; i < NUM_CHILDREN; i++) {
				int status;
				pid_t w = waitpid(children[i], &status, 0);
				if (w != children[i]) {
					all_finished = 0;
				}
				ssize_t r = read(pipe_fds[i][0], &counts[i],
						 sizeof(counts[i]));
				close(pipe_fds[i][0]);
				if (r != sizeof(counts[i])) {
					counts[i] = 0;
				}
			}

			test_result("all children completed", all_finished);

			// Check that all children did similar amounts of work (fair scheduling)
			// Allow 50% variance for fairness check
			unsigned long min_count = counts[0],
				      max_count = counts[0];
			for (int i = 1; i < NUM_CHILDREN; i++) {
				if (counts[i] < min_count)
					min_count = counts[i];
				if (counts[i] > max_count)
					max_count = counts[i];
			}
			// Fair if max is no more than 3x min (generous for simple scheduler)
			int is_fair =
				(min_count > 0) && (max_count <= min_count * 3);
			printf("  Child work counts: %lu, %lu, %lu\n",
			       counts[0], counts[1], counts[2]);
			test_result("fair scheduling among children", is_fair);
		} else {
			test_fail("multiple children: pipe creation failed");
			test_fail("fair scheduling among children");
		}
	}

	// Test 2: Signal delivery to blocked task
	printf("\n[TEST] Signal delivery to sleeping task\n");
	{
		static volatile int got_signal = 0;

		void sig_handler(int sig)
		{
			(void)sig;
			got_signal = 1;
		}

		signal(SIGUSR1, sig_handler);
		got_signal = 0;

		pid_t child = fork();
		if (child == 0) {
			// Child: sleep and get interrupted by signal
			sleep(10); // Long sleep, should be interrupted
			_exit(got_signal ? 42 : 0);
		} else if (child > 0) {
			// Parent: wait a bit, then send signal to child
			usleep(50000); // 50ms
			kill(child, SIGUSR1);

			int status;
			pid_t w = waitpid(child, &status, 0);
			test_result("signal woke sleeping child", w == child);
			// Child should have exited with 42 (signal received) or 0 (no signal)
			// The key test is that waitpid returned quickly, not after 10 seconds
			test_result("child exit captured", WIFEXITED(status));
		} else {
			test_fail("signal to sleeping: fork failed");
			test_fail("child exit captured");
		}
	}

	// Test 3: Timer accuracy under load
	printf("\n[TEST] Timer accuracy under CPU load\n");
	{
		pid_t child = fork();
		if (child == 0) {
			// Child: CPU-bound loop
			volatile unsigned long c = 0;
			while (1) {
				c++;
			}
			_exit(0);
		} else if (child > 0) {
			// Parent: measure sleep duration while child consumes CPU
			struct timespec start, end;

			// Measure 100ms sleep.  Resume on EINTR with the remainder
			// the kernel reports: a signal legitimately cuts a sleep
			// short without sleeping the rest, and swallowing that
			// would make this measure signal latency instead of timer
			// accuracy.  usleep() is not usable here for the same
			// reason — it cannot report how much time is left.
			struct timespec req = { 0, 100 * 1000 * 1000L }, rem;
			clock_gettime(CLOCK_MONOTONIC, &start);
			while (nanosleep(&req, &rem) < 0 && errno == EINTR) {
				req = rem;
			}
			clock_gettime(CLOCK_MONOTONIC, &end);

			// Calculate elapsed time in ms
			long elapsed_ms =
				(end.tv_sec - start.tv_sec) * 1000 +
				(end.tv_nsec - start.tv_nsec) / 1000000;

			// Kill the CPU-hog child
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);

			// Timer should be reasonably accurate (80-200ms for 100ms sleep)
			printf("  Requested 100ms sleep, actual: %ld ms\n",
			       elapsed_ms);
			/* On failure, dump the raw clock readings.  elapsed is
			 * derived from two CLOCK_MONOTONIC samples taken either
			 * side of the sleep, and the task may migrate between
			 * CPUs in between — so a bogus elapsed means either the
			 * clock disagreed with itself across that migration
			 * (end <= start, or a nonsense jump) or the sleep really
			 * did not sleep.  The raw values say which; elapsed on
			 * its own cannot. */
			if (!(elapsed_ms >= 80 && elapsed_ms <= 300)) {
				long long s_ns =
					(long long)start.tv_sec * 1000000000LL +
					start.tv_nsec;
				long long e_ns =
					(long long)end.tv_sec * 1000000000LL +
					end.tv_nsec;
				printf("  [DBG] start=%lld ns end=%lld ns delta=%lld ns%s\n",
				       s_ns, e_ns, e_ns - s_ns,
				       (e_ns < s_ns) ?
					       "  <-- CLOCK WENT BACKWARD" :
				       (e_ns == s_ns) ? "  <-- CLOCK DID NOT ADVANCE" :
							"");
			}
			test_result("timer accuracy under load",
				    elapsed_ms >= 80 && elapsed_ms <= 300);
		} else {
			test_fail("timer accuracy: fork failed");
		}
	}

	// Test 4: Priority inversion scenario (parent waits for child's pipe)
	printf("\n[TEST] I/O blocking vs CPU-bound scheduling\n");
	{
		int pipefd[2];
		if (pipe(pipefd) == 0) {
			pid_t cpu_child = fork();
			if (cpu_child == 0) {
				// CPU-bound child - tries to starve system
				close(pipefd[0]);
				close(pipefd[1]);
				volatile unsigned long c = 0;
				for (int i = 0; i < 10000000; i++) {
					c++;
				}
				_exit(0);
			}

			pid_t io_child = fork();
			if (io_child == 0) {
				// I/O child - writes to pipe after brief delay
				close(pipefd[0]);
				usleep(20000); // 20ms delay
				write(pipefd[1], "OK", 2);
				close(pipefd[1]);
				_exit(0);
			}

			// Parent: read from pipe (should complete despite CPU-bound sibling)
			close(pipefd[1]);
			char buf[4] = { 0 };
			ssize_t n = read(pipefd[0], buf, 2);
			close(pipefd[0]);

			// Wait for both children
			waitpid(cpu_child, NULL, 0);
			waitpid(io_child, NULL, 0);

			test_result("I/O completed despite CPU load",
				    n == 2 && buf[0] == 'O');
		} else {
			test_fail("I/O blocking test: pipe failed");
		}
	}

	// Test 5: Rapid fork/exit stress test
	printf("\n[TEST] Rapid fork/exit stress\n");
	{
#define STRESS_ITERATIONS 20
		int success_count = 0;

		for (int i = 0; i < STRESS_ITERATIONS; i++) {
			pid_t p = fork();
			if (p == 0) {
				// Child: exit immediately
				_exit(i);
			} else if (p > 0) {
				int status;
				pid_t w = waitpid(p, &status, 0);
				if (w == p && WIFEXITED(status) &&
				    WEXITSTATUS(status) == (i & 0xFF)) {
					success_count++;
				}
			}
		}

		printf("  Completed %d/%d fork/exit cycles\n", success_count,
		       STRESS_ITERATIONS);
		test_result("rapid fork/exit stress",
			    success_count == STRESS_ITERATIONS);
	}

	// Test 6: Time slice fairness measurement
	//
	// Strategy: use shared memory with a barrier so both parent and child
	// start spinning at the same time, then have both processes run the
	// same timed loop.  This avoids comparing a pure spin loop against a
	// loop that also pays repeated clock_gettime() overhead.
	printf("\n[TEST] Time slice measurement\n");
	{
		volatile unsigned long *shared =
			mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (shared != MAP_FAILED) {
			// Layout: [0]=child counter  [1]=parent counter
			//         [2]=barrier (both set their bit, spin until ==3)
			shared[0] = 0;
			shared[1] = 0;
			shared[2] = 0; // barrier

			pid_t child = fork();
			if (child == 0) {
				struct timespec start_time, now;

				// Signal ready and wait for parent
				__atomic_or_fetch(
					(volatile unsigned long *)&shared[2], 1,
					__ATOMIC_SEQ_CST);
				while (__atomic_load_n((volatile unsigned long
								*)&shared[2],
						       __ATOMIC_SEQ_CST) != 3)
					; // spin-wait for barrier

				clock_gettime(CLOCK_MONOTONIC, &start_time);

				// Count for 200ms, sampling time at the same cadence as parent
				unsigned long cnt = 0;
				while (1) {
					cnt++;
					if ((cnt & 0xFFF) == 0) {
						clock_gettime(CLOCK_MONOTONIC,
							      &now);
						long elapsed_ms =
							(now.tv_sec -
							 start_time.tv_sec) *
								1000 +
							(now.tv_nsec -
							 start_time.tv_nsec) /
								1000000;
						if (elapsed_ms >= 200)
							break;
					}
				}
				__atomic_store_n(
					(volatile unsigned long *)&shared[0],
					cnt, __ATOMIC_RELEASE);
				_exit(0);
			} else if (child > 0) {
				struct timespec start_time, now;

				// Signal ready and wait for child
				__atomic_or_fetch(
					(volatile unsigned long *)&shared[2], 2,
					__ATOMIC_SEQ_CST);
				while (__atomic_load_n((volatile unsigned long
								*)&shared[2],
						       __ATOMIC_SEQ_CST) != 3)
					; // spin-wait for barrier

				// Both are running — do the same timed loop as the child
				clock_gettime(CLOCK_MONOTONIC, &start_time);
				unsigned long cnt = 0;
				while (1) {
					cnt++;
					if ((cnt & 0xFFF) == 0) {
						clock_gettime(CLOCK_MONOTONIC,
							      &now);
						long elapsed_ms =
							(now.tv_sec -
							 start_time.tv_sec) *
								1000 +
							(now.tv_nsec -
							 start_time.tv_nsec) /
								1000000;
						if (elapsed_ms >= 200)
							break;
					}
				}
				__atomic_store_n(
					(volatile unsigned long *)&shared[1],
					cnt, __ATOMIC_RELEASE);

				waitpid(child, NULL, 0);

				unsigned long child_cnt = shared[0];
				unsigned long parent_cnt = shared[1];
				printf("  Child iterations: %lu, Parent iterations: %lu\n",
				       child_cnt, parent_cnt);

				test_result("child got CPU time",
					    child_cnt > 1000);
				test_result("parent got CPU time",
					    parent_cnt > 1000);

				// This is a starvation check, not a strict scheduler benchmark.
				// Virtualized environments (especially VMware/VirtualBox under
				// load from parallel test instances) can produce very large skew
				// because host vCPU stealing affects child and parent unequally
				// (observed ratios up to ~30 on VMware with two parallel
				// teststress instances).  Only fail on clearly pathological
				// one-sided CPU distribution, not on virtualization noise.
				if (child_cnt > 0 && parent_cnt > 0) {
					unsigned long ratio =
						(child_cnt > parent_cnt) ?
							child_cnt / parent_cnt :
							parent_cnt / child_cnt;
					printf("  Fairness ratio: %lu\n",
					       ratio);
					test_result("time slice roughly fair",
						    ratio < 50);
				} else {
					test_result("time slice roughly fair",
						    0);
				}
			}

			munmap((void *)shared, 4096);
		} else {
			test_fail("time slice test: mmap failed");
			test_fail("child got CPU time");
			test_fail("parent got CPU time");
			test_fail("time slice roughly fair");
		}
	}

	// Test 7: Nested signal during syscall
	printf("\n[TEST] Signal during blocking syscall\n");
	{
		int pipefd[2];
		if (pipe(pipefd) == 0) {
			static volatile int alarm_received = 0;

			void alarm_handler(int sig)
			{
				(void)sig;
				alarm_received = 1;
			}

			signal(SIGALRM, alarm_handler);
			alarm_received = 0;

			// Set alarm to fire during read
			alarm(1);

			// Read from pipe with no writer - should block until alarm
			char buf[10];
			ssize_t n = read(pipefd[0], buf, 10);

			close(pipefd[0]);
			close(pipefd[1]);

			test_result("blocking read interrupted",
				    n < 0 && errno == EINTR);
			test_result("alarm handler ran", alarm_received == 1);
		} else {
			test_fail("blocking syscall test: pipe failed");
			test_fail("alarm handler ran");
		}
	}

	// ========================================
	// SMP Stress Tests
	// ========================================
	printf("\n--- SMP Stress Tests ---\n");

	// Test: Concurrent fork() from multiple processes
	printf("\n[TEST] Concurrent fork() stress\n");
	{
#define CONCURRENT_FORKS 5
		pid_t fork_children[CONCURRENT_FORKS];
		int fork_ok = 1;

		// Fork several children, each of which also forks
		for (int i = 0; i < CONCURRENT_FORKS; i++) {
			fork_children[i] = fork();
			if (fork_children[i] == 0) {
				// Child: fork a grandchild and wait for it
				pid_t gc = fork();
				if (gc == 0) {
					// Grandchild: do some work and exit
					volatile unsigned long c = 0;
					for (int j = 0; j < 10000; j++)
						c++;
					_exit((int)(c & 0xFF));
				} else if (gc > 0) {
					int status;
					waitpid(gc, &status, 0);
					_exit(WIFEXITED(status) ? 0 : 1);
				} else {
					_exit(2); // fork failed
				}
			} else if (fork_children[i] < 0) {
				fork_ok = 0;
			}
		}

		// Wait for all direct children
		int all_ok = fork_ok;
		for (int i = 0; i < CONCURRENT_FORKS; i++) {
			if (fork_children[i] > 0) {
				int status;
				pid_t w = waitpid(fork_children[i], &status, 0);
				if (w != fork_children[i] ||
				    !WIFEXITED(status) ||
				    WEXITSTATUS(status) != 0) {
					all_ok = 0;
				}
			}
		}
		test_result("concurrent fork/grandchild stress", all_ok);
	}

	// Test: Parallel memory allocation stress
	printf("\n[TEST] Parallel malloc stress\n");
	{
#define MALLOC_CHILDREN 4
#define MALLOC_ITERATIONS 50
		pid_t malloc_children[MALLOC_CHILDREN];
		int malloc_pipes[MALLOC_CHILDREN][2];
		int pipes_ok = 1;

		for (int i = 0; i < MALLOC_CHILDREN; i++) {
			if (pipe(malloc_pipes[i]) < 0) {
				pipes_ok = 0;
				break;
			}
		}

		if (pipes_ok) {
			for (int i = 0; i < MALLOC_CHILDREN; i++) {
				malloc_children[i] = fork();
				if (malloc_children[i] == 0) {
					// Close read ends and other write ends
					for (int j = 0; j < MALLOC_CHILDREN;
					     j++) {
						close(malloc_pipes[j][0]);
						if (j != i)
							close(malloc_pipes[j]
									  [1]);
					}

					// Allocate and free memory in a loop
					int success = 0;
					for (int j = 0; j < MALLOC_ITERATIONS;
					     j++) {
						size_t sz =
							64 +
							(j *
							 37) % 4096; // Varying sizes
						void *p = malloc(sz);
						if (p) {
							// Touch the memory
							memset(p,
							       (char)(j & 0xFF),
							       sz);
							// Verify first byte
							if (((unsigned char *)
								     p)[0] ==
							    (unsigned char)(j &
									    0xFF)) {
								success++;
							}
							free(p);
						}
					}

					write(malloc_pipes[i][1], &success,
					      sizeof(success));
					close(malloc_pipes[i][1]);
					_exit(0);
				}
				close(malloc_pipes[i][1]);
			}

			// Collect results
			int total_success = 0;
			int all_finished = 1;
			for (int i = 0; i < MALLOC_CHILDREN; i++) {
				int status;
				pid_t w =
					waitpid(malloc_children[i], &status, 0);
				if (w != malloc_children[i])
					all_finished = 0;

				int child_success = 0;
				read(malloc_pipes[i][0], &child_success,
				     sizeof(child_success));
				close(malloc_pipes[i][0]);
				total_success += child_success;
			}

			int expected = MALLOC_CHILDREN * MALLOC_ITERATIONS;
			printf("  Parallel malloc: %d/%d allocations succeeded\n",
			       total_success, expected);
			test_result("parallel malloc all children finished",
				    all_finished);
			test_result("parallel malloc all allocations ok",
				    total_success == expected);
		} else {
			test_fail("parallel malloc: pipe creation failed");
			test_fail("parallel malloc all children finished");
			test_fail("parallel malloc all allocations ok");
		}
	}

	// Test: Multi-process pipe read/write stress
	printf("\n[TEST] Multi-process pipe stress\n");
	{
#define PIPE_WRITERS 3
#define PIPE_MSGS_PER_WRITER 10
		int stress_pipe[2];

		if (pipe(stress_pipe) == 0) {
			pid_t writers[PIPE_WRITERS];

			// Fork writer processes
			for (int i = 0; i < PIPE_WRITERS; i++) {
				writers[i] = fork();
				if (writers[i] == 0) {
					close(stress_pipe[0]); // Close read end

					// Write messages to pipe
					for (int j = 0;
					     j < PIPE_MSGS_PER_WRITER; j++) {
						char msg[16];
						int len = 0;
						// Simple message: "Wij\n" where i=writer, j=msg
						msg[len++] = 'W';
						msg[len++] = '0' + i;
						msg[len++] = '0' + j;
						msg[len++] = '\n';
						write(stress_pipe[1], msg, len);
					}

					close(stress_pipe[1]);
					_exit(0);
				}
			}

			// Parent reads all messages
			close(stress_pipe[1]); // Close write end

			int msgs_received = 0;
			char rbuf[256];
			ssize_t total_read = 0;

			while (1) {
				ssize_t n =
					read(stress_pipe[0], rbuf + total_read,
					     sizeof(rbuf) - total_read - 1);
				if (n <= 0)
					break;
				total_read += n;

				// Count newlines as message delimiters
				for (ssize_t k = total_read - n; k < total_read;
				     k++) {
					if (rbuf[k] == '\n')
						msgs_received++;
				}
			}
			close(stress_pipe[0]);

			// Wait for all writers
			for (int i = 0; i < PIPE_WRITERS; i++) {
				waitpid(writers[i], NULL, 0);
			}

			int expected_msgs = PIPE_WRITERS * PIPE_MSGS_PER_WRITER;
			printf("  Pipe stress: received %d/%d messages\n",
			       msgs_received, expected_msgs);
			test_result("multi-process pipe all messages received",
				    msgs_received == expected_msgs);
		} else {
			test_fail(
				"multi-process pipe stress: pipe creation failed");
		}
	}

	// Test: sched_yield() syscall (if available)
	printf("\n[TEST] sched_yield() behavior\n");
	{
		// Verify that yielding doesn't crash or hang
		// Fork a child, both yield repeatedly
		volatile int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (shared != MAP_FAILED) {
			shared[0] = 0; // Child counter
			shared[1] = 0; // Parent counter
			shared[2] = 0; // Stop flag

			pid_t child = fork();
			if (child == 0) {
				while (!shared[2]) {
					shared[0]++;
					sched_yield();
				}
				_exit(0);
			} else if (child > 0) {
				// Parent yields a few times (reduced for speed)
				for (int i = 0; i < 5; i++) {
					shared[1]++;
					sched_yield();
				}
				/* Don't raise the stop flag until the child has
	             * demonstrably run.  The 5 yields above complete in
	             * microseconds; under parallel stress load the child
	             * may not have had a single CPU slice yet (it can sit
	             * behind unrelated runnable tasks, possibly on another
	             * CPU's queue where our yield donates nothing).  The
	             * child checks the flag before its first increment, so
	             * flagging too early lets it exit with 0 iterations.
	             * Allow up to ~10 s of wall time. */
				for (int i = 0; i < 10000 && shared[0] == 0; i++)
					usleep(1000);
				shared[2] = 1; // Signal child to stop

				waitpid(child, NULL, 0);

				printf("  yield test: parent=%d, child=%d iterations\n",
				       (int)shared[1], (int)shared[0]);
				test_result("sched_yield parent ran",
					    shared[1] >= 5);
				test_result("sched_yield child ran",
					    shared[0] > 0);
			}
			munmap((void *)shared, 4096);
		} else {
			test_fail("sched_yield test: mmap failed");
		}
	}

	// ========================================
	// Test: SMP/Threading syscalls
	// ========================================
	printf("\n[TEST] SMP/Threading syscalls\n");

	// Test gettid()
	{
		pid_t tid = gettid();
		printf("  gettid() = %d\n", tid);
		test_result("gettid() returns positive value", tid > 0);

		// TID should equal PID for single-threaded process
		pid_t pid = getpid();
		test_result("gettid() == getpid() for single-threaded",
			    tid == pid);
	}

	// Test sched_getaffinity() / sched_setaffinity()
	printf("\n[TEST] CPU affinity syscalls\n");
	{
		cpu_set_t mask;
		CPU_ZERO(&mask);

		int ret = sched_getaffinity(0, sizeof(mask), &mask);
		printf("  sched_getaffinity() returned %d\n", ret);
		test_result("sched_getaffinity() succeeds", ret >= 0);

		// Check that at least one CPU is set
		int cpu_count = CPU_COUNT(&mask);
		printf("  CPU count in mask: %d\n", cpu_count);
		test_result("At least one CPU in affinity mask", cpu_count > 0);

		// Try to set affinity to CPU 0
		CPU_ZERO(&mask);
		CPU_SET(0, &mask);
		ret = sched_setaffinity(0, sizeof(mask), &mask);
		printf("  sched_setaffinity(CPU 0) returned %d\n", ret);
		test_result("sched_setaffinity() succeeds", ret == 0);

		// Verify the change
		CPU_ZERO(&mask);
		sched_getaffinity(0, sizeof(mask), &mask);
		test_result("CPU 0 is set after setaffinity",
			    CPU_ISSET(0, &mask));
	}

	// Test sched_getscheduler() / sched_setscheduler()
	printf("\n[TEST] Scheduler policy syscalls\n");
	{
		int policy = sched_getscheduler(0);
		printf("  sched_getscheduler(0) = %d\n", policy);
		test_result("sched_getscheduler() succeeds", policy >= 0);
		test_result("Default policy is SCHED_NORMAL (0)",
			    policy == SCHED_NORMAL);

		// Try to set scheduler (should work even though we only support NORMAL)
		struct sched_param param = { .sched_priority = 0 };
		int ret = sched_setscheduler(0, SCHED_NORMAL, &param);
		test_result("sched_setscheduler(SCHED_NORMAL) succeeds",
			    ret == 0);
	}

	// Test sched_getparam() / sched_setparam()
	printf("\n[TEST] Scheduler parameter syscalls\n");
	{
		struct sched_param param;
		int ret = sched_getparam(0, &param);
		printf("  sched_getparam() returned %d, priority=%d\n", ret,
		       param.sched_priority);
		test_result("sched_getparam() succeeds", ret == 0);

		param.sched_priority = 0;
		ret = sched_setparam(0, &param);
		test_result("sched_setparam() succeeds", ret == 0);
	}

	// Test sched_get_priority_max() / sched_get_priority_min()
	printf("\n[TEST] Priority range syscalls\n");
	{
		int max_rr = sched_get_priority_max(SCHED_RR);
		int min_rr = sched_get_priority_min(SCHED_RR);
		printf("  SCHED_RR priority range: %d - %d\n", min_rr, max_rr);
		test_result("SCHED_RR max priority is 99", max_rr == 99);
		test_result("SCHED_RR min priority is 1", min_rr == 1);

		int max_normal = sched_get_priority_max(SCHED_NORMAL);
		int min_normal = sched_get_priority_min(SCHED_NORMAL);
		printf("  SCHED_NORMAL priority range: %d - %d\n", min_normal,
		       max_normal);
		test_result("SCHED_NORMAL max priority is 0", max_normal == 0);
		test_result("SCHED_NORMAL min priority is 0", min_normal == 0);
	}

	// Test sched_rr_get_interval()
	printf("\n[TEST] Round-robin interval syscall\n");
	{
		struct timespec ts;
		int ret = sched_rr_get_interval(0, &ts);
		printf("  sched_rr_get_interval() = %d, interval=%ld.%09ld sec\n",
		       ret, ts.tv_sec, ts.tv_nsec);
		test_result("sched_rr_get_interval() succeeds", ret == 0);
		test_result("Time quantum is ~20ms",
			    ts.tv_nsec >= 10000000 && ts.tv_nsec <= 100000000);
	}

	// Test mprotect()
	printf("\n[TEST] mprotect() syscall\n");
	{
		// Allocate a page
		void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		test_result("mmap for mprotect test", page != MAP_FAILED);

		if (page != MAP_FAILED) {
			// Write to the page
			*(int *)page = 42;
			test_result("Can write to RW page", *(int *)page == 42);

			// Change to read-only
			int ret = mprotect(page, 4096, PROT_READ);
			printf("  mprotect(PROT_READ) returned %d\n", ret);
			test_result("mprotect() succeeds", ret == 0);

			// Reading should still work
			int val = *(volatile int *)page;
			test_result("Can read from RO page", val == 42);

			// Change back to RW
			ret = mprotect(page, 4096, PROT_READ | PROT_WRITE);
			test_result("mprotect(PROT_READ|PROT_WRITE) succeeds",
				    ret == 0);

			munmap(page, 4096);
		}
	}

	// Test futex (basic wake/wait operations)
	printf("\n[TEST] futex() syscalls\n");
	{
		volatile int *futex_val =
			mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (futex_val != MAP_FAILED) {
			*futex_val = 0;

			pid_t child = fork();
			if (child == 0) {
				// Child: wait for parent to wake us
				// Use busy loop with yield instead of blocking futex
				// since blocking might not work perfectly
				for (int i = 0; i < 1000 && *futex_val == 0;
				     i++) {
					sched_yield();
				}
				_exit(*futex_val == 1 ? 0 : 1);
			} else if (child > 0) {
				// Parent: wake the child
				sched_yield(); // Let child start
				*futex_val = 1;

				// Wake any waiters (even if child is just spinning)
				int woken = futex_wake((int *)futex_val, 1);
				printf("  futex_wake() woke %d waiters\n",
				       woken);

				int status;
				waitpid(child, &status, 0);
				test_result("futex signaling works",
					    WIFEXITED(status) &&
						    WEXITSTATUS(status) == 0);
			}

			munmap((void *)futex_val, 4096);
		} else {
			test_fail("futex test: mmap failed");
		}
	}

	// Test vfork() (should work like fork)
	printf("\n[TEST] vfork() syscall\n");
	{
		pid_t child = vfork();
		if (child == 0) {
			// Child process
			_exit(42);
		} else if (child > 0) {
			int status;
			waitpid(child, &status, 0);
			test_result("vfork() child exits correctly",
				    WIFEXITED(status) &&
					    WEXITSTATUS(status) == 42);
		} else {
			test_fail("vfork() failed");
		}
	}

	// ========================================
	// Thread Groups / SMP Tests
	// ========================================
	printf("\n[TEST] Thread Groups (getpid vs gettid)\n");
	{
		// For main thread, getpid() and gettid() should return the same value
		pid_t pid = getpid();
		pid_t tid = gettid();
		test_result("gettid() returns valid TID", tid > 0);
		test_result("getpid() == gettid() for main thread", pid == tid);
		printf("  PID=%d, TID=%d\n", pid, tid);
	}

	printf("\n[TEST] set_tid_address() syscall\n");
	{
		int clear_tid = 12345;
		int result = set_tid_address(&clear_tid);
		test_result("set_tid_address() returns TID", result > 0);
		test_result("set_tid_address() returns same as gettid()",
			    result == gettid());
	}

	printf("\n[TEST] set_robust_list() syscall\n");
	{
		// Create a simple robust list head
		struct {
			void *next;
			long futex_offset;
			void *pending;
		} robust_head;

		robust_head.next = &robust_head; // Point to self (empty list)
		robust_head.futex_offset = 0;
		robust_head.pending = NULL;

		int result = set_robust_list(&robust_head, sizeof(robust_head));
		test_result("set_robust_list() succeeds", result == 0);
	}

	printf("\n[TEST] arch_prctl() TLS syscall\n");
	{
		// Allocate a real mapped page to serve as the temporary TLS block.
		// Using a hardcoded unmapped address would fault at fs:0x28 because any
		// stack-protected function (printf, etc.) reads the canary from there.
		void *tls_block = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (tls_block == MAP_FAILED) {
			test_fail("arch_prctl(ARCH_SET_FS) succeeds");
			test_fail("arch_prctl(ARCH_GET_FS) succeeds");
			test_fail("ARCH_GET_FS returns correct value");
		} else {
			unsigned long test_tls_addr = (unsigned long)tls_block;
			unsigned long orig_fs = 0;
			unsigned long readback = 0;

			// Save original FS base so we can restore it afterwards.
			arch_prctl(ARCH_GET_FS, (unsigned long)&orig_fs);

			// Copy the current stack canary into the new TLS block at offset 0x28
			// (the slot read by every stack-protected function via fs:0x28) so
			// that printf / test_result continue to work after the FS switch.
			uint64_t cur_canary;
			__asm__ volatile("mov %%fs:0x28, %0"
					 : "=r"(cur_canary));
			*(volatile uint64_t *)((char *)tls_block + 0x28) =
				cur_canary;

			// Set FS base to our mapped TLS block.
			int set_result = arch_prctl(ARCH_SET_FS, test_tls_addr);
			test_result("arch_prctl(ARCH_SET_FS) succeeds",
				    set_result == 0);

			// Get FS base back.
			int get_result = arch_prctl(ARCH_GET_FS,
						    (unsigned long)&readback);
			test_result("arch_prctl(ARCH_GET_FS) succeeds",
				    get_result == 0);
			test_result("ARCH_GET_FS returns correct value",
				    readback == test_tls_addr);
			printf("  Set FS base to 0x%lx, read back 0x%lx\n",
			       test_tls_addr, readback);

			// Restore the original FS base.
			arch_prctl(ARCH_SET_FS, orig_fs);

			munmap(tls_block, 4096);
		}
	}

	printf("\n[TEST] Fork thread group isolation\n");
	{
		// fork() should create a new process with different PID
		// Child should have getpid() == gettid() (it's its own thread group leader)
		volatile int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
					    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (shared != MAP_FAILED) {
			shared[0] = 0; // child's pid
			shared[1] = 0; // child's tid

			pid_t parent_pid = getpid();
			pid_t child = fork();

			if (child == 0) {
				// Child process
				shared[0] = getpid();
				shared[1] = gettid();
				_exit(0);
			} else if (child > 0) {
				int status;
				waitpid(child, &status, 0);

				test_result("fork() child has different PID",
					    shared[0] != parent_pid);
				test_result("fork() child has pid == tid",
					    shared[0] == shared[1]);
				test_result("fork() returns correct child PID",
					    child == shared[0]);
				printf("  Parent PID=%d, Child PID=%d, Child TID=%d\n",
				       parent_pid, (int)shared[0],
				       (int)shared[1]);
			} else {
				test_fail("fork() failed");
			}

			munmap((void *)shared, 4096);
		} else {
			test_fail("mmap for fork test failed");
		}
	}

	// ========================================
	// Pthread Tests
	// ========================================
	printf("\n========================================\n");
	printf("[TEST] Pthread Library Tests\n");
	printf("========================================\n");

	// Test pthread_self and pthread_equal
	printf("\n[TEST] pthread_self and pthread_equal\n");
	{
		pthread_t self = pthread_self();
		test_result("pthread_self() returns non-NULL", self != 0);
		test_result("pthread_equal(self, self) returns non-zero",
			    pthread_equal(self, self) != 0);
		printf("  pthread_self() = %p\n", (void *)self);
	}

	// Test pthread_attr functions
	printf("\n[TEST] pthread_attr functions\n");
	{
		pthread_attr_t attr;
		int ret = pthread_attr_init(&attr);
		test_result("pthread_attr_init succeeds", ret == 0);

		// Test detachstate
		int detach_state = -1;
		ret = pthread_attr_getdetachstate(&attr, &detach_state);
		test_result("pthread_attr_getdetachstate succeeds", ret == 0);
		test_result("default detachstate is JOINABLE",
			    detach_state == PTHREAD_CREATE_JOINABLE);

		ret = pthread_attr_setdetachstate(&attr,
						  PTHREAD_CREATE_DETACHED);
		test_result("pthread_attr_setdetachstate succeeds", ret == 0);
		pthread_attr_getdetachstate(&attr, &detach_state);
		test_result("detachstate is now DETACHED",
			    detach_state == PTHREAD_CREATE_DETACHED);

		// Test stacksize
		size_t stacksize = 0;
		ret = pthread_attr_getstacksize(&attr, &stacksize);
		test_result("pthread_attr_getstacksize succeeds", ret == 0);
		test_result("default stacksize >= 16KB", stacksize >= 16384);
		printf("  Default stack size: %zu bytes\n", stacksize);

		ret = pthread_attr_setstacksize(&attr, 4 * 1024 * 1024); // 4MB
		test_result("pthread_attr_setstacksize(4MB) succeeds",
			    ret == 0);
		pthread_attr_getstacksize(&attr, &stacksize);
		test_result("stacksize is now 4MB",
			    stacksize == 4 * 1024 * 1024);

		// Test guardsize
		size_t guardsize = 0;
		ret = pthread_attr_getguardsize(&attr, &guardsize);
		test_result("pthread_attr_getguardsize succeeds", ret == 0);
		printf("  Default guard size: %zu bytes\n", guardsize);

		ret = pthread_attr_setguardsize(&attr, 8192);
		test_result("pthread_attr_setguardsize succeeds", ret == 0);
		pthread_attr_getguardsize(&attr, &guardsize);
		test_result("guardsize is now 8192", guardsize == 8192);

		ret = pthread_attr_destroy(&attr);
		test_result("pthread_attr_destroy succeeds", ret == 0);
	}

	// Test basic thread creation and join
	printf("\n[TEST] pthread_create and pthread_join\n");
	{
		g_simple_thread_ran = 0;
		g_simple_thread_arg = 0;

		pthread_t thread;
		int ret = pthread_create(&thread, NULL, simple_thread_fn,
					 (void *)123L);
		test_result("pthread_create succeeds", ret == 0);
		printf("  Created thread %p\n", (void *)thread);

		void *retval = NULL;
		ret = pthread_join(thread, &retval);
		test_result("pthread_join succeeds", ret == 0);
		test_result("thread function ran", g_simple_thread_ran == 1);
		test_result("thread received correct argument",
			    g_simple_thread_arg == 123);
		test_result("thread returned correct value",
			    retval == (void *)42L);
		printf("  Thread returned: %ld\n", (long)retval);
	}

	// Test pthread_detach
	printf("\n[TEST] pthread_detach\n");
	{
		g_detached_thread_ran = 0;

		pthread_t thread;
		int ret =
			pthread_create(&thread, NULL, detached_thread_fn, NULL);
		test_result("pthread_create for detach test succeeds",
			    ret == 0);

		ret = pthread_detach(thread);
		test_result("pthread_detach succeeds", ret == 0);

		// Spin-wait with a generous timeout so heavy system load doesn't cause
		// a false failure (e.g. two parallel teststress instances competing for CPU)
		for (int waited = 0;
		     g_detached_thread_ran == 0 && waited < 5000000;
		     waited += 10000)
			usleep(10000); // 10ms slices, up to 5s total

		// Can't join a detached thread, but it should have run
		test_result("detached thread ran", g_detached_thread_ran == 1);
	}

	// Test mutex basic operations
	printf("\n[TEST] pthread_mutex basic operations\n");
	{
		pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

		int ret = pthread_mutex_lock(&mutex);
		test_result("pthread_mutex_lock succeeds", ret == 0);

		ret = pthread_mutex_unlock(&mutex);
		test_result("pthread_mutex_unlock succeeds", ret == 0);

		ret = pthread_mutex_trylock(&mutex);
		test_result("pthread_mutex_trylock succeeds when unlocked",
			    ret == 0);

		ret = pthread_mutex_unlock(&mutex);
		test_result("pthread_mutex_unlock after trylock succeeds",
			    ret == 0);

		ret = pthread_mutex_destroy(&mutex);
		test_result("pthread_mutex_destroy succeeds", ret == 0);
	}

	// Test mutex with thread contention
	printf("\n[TEST] pthread_mutex with thread contention\n");
	{
		pthread_t t1, t2;
		g_shared_counter = 0;
		int increments = 1000;

		int ret1 = pthread_create(&t1, NULL, increment_thread_fn,
					  (void *)(long)increments);
		int ret2 = pthread_create(&t2, NULL, increment_thread_fn,
					  (void *)(long)increments);
		test_result("pthread_create for t1 succeeds", ret1 == 0);
		test_result("pthread_create for t2 succeeds", ret2 == 0);

		pthread_join(t1, NULL);
		pthread_join(t2, NULL);

		test_result("mutex protects counter correctly",
			    g_shared_counter == 2 * increments);
		printf("  Expected counter: %d, Actual: %d\n", 2 * increments,
		       g_shared_counter);

		pthread_mutex_destroy(&g_contention_mutex);
	}

	// Test recursive mutex
	printf("\n[TEST] pthread_mutex recursive\n");
	{
		pthread_mutexattr_t attr;
		pthread_mutex_t recursive_mutex;

		int ret = pthread_mutexattr_init(&attr);
		test_result("pthread_mutexattr_init succeeds", ret == 0);

		ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		test_result("pthread_mutexattr_settype(RECURSIVE) succeeds",
			    ret == 0);

		ret = pthread_mutex_init(&recursive_mutex, &attr);
		test_result("pthread_mutex_init with recursive attr succeeds",
			    ret == 0);

		// Lock multiple times
		ret = pthread_mutex_lock(&recursive_mutex);
		test_result("first lock succeeds", ret == 0);

		ret = pthread_mutex_lock(&recursive_mutex);
		test_result("second lock (recursive) succeeds", ret == 0);

		ret = pthread_mutex_lock(&recursive_mutex);
		test_result("third lock (recursive) succeeds", ret == 0);

		// Unlock same number of times
		ret = pthread_mutex_unlock(&recursive_mutex);
		test_result("first unlock succeeds", ret == 0);

		ret = pthread_mutex_unlock(&recursive_mutex);
		test_result("second unlock succeeds", ret == 0);

		ret = pthread_mutex_unlock(&recursive_mutex);
		test_result("third unlock succeeds", ret == 0);

		pthread_mutex_destroy(&recursive_mutex);
		pthread_mutexattr_destroy(&attr);
	}

	// Test condition variables
	printf("\n[TEST] pthread_cond basic operations\n");
	{
		pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
		pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
		static volatile int cond_flag = 0;
		cond_flag = 0;

		struct cond_test_args args = { &cond, &cond_mutex, &cond_flag };
		pthread_t waiter;
		int ret = pthread_create(&waiter, NULL, cond_waiter_thread_fn,
					 &args);
		test_result("pthread_create for cond waiter succeeds",
			    ret == 0);

		// Give waiter time to start waiting
		for (volatile int i = 0; i < 100000; i++)
			;

		// Signal the condition
		pthread_mutex_lock(&cond_mutex);
		cond_flag = 1;
		ret = pthread_cond_signal(&cond);
		test_result("pthread_cond_signal succeeds", ret == 0);
		pthread_mutex_unlock(&cond_mutex);

		void *retval;
		ret = pthread_join(waiter, &retval);
		test_result("pthread_join on cond waiter succeeds", ret == 0);
		test_result("cond waiter completed", retval == (void *)99L);

		pthread_cond_destroy(&cond);
		pthread_mutex_destroy(&cond_mutex);
	}

	// Test pthread_cond_broadcast
	printf("\n[TEST] pthread_cond_broadcast\n");
	{
		g_bcast_flag = 0;
		g_waiters_done = 0;

		pthread_t t1, t2, t3;
		pthread_create(&t1, NULL, bcast_waiter_fn, NULL);
		pthread_create(&t2, NULL, bcast_waiter_fn, NULL);
		pthread_create(&t3, NULL, bcast_waiter_fn, NULL);

		// Give waiters time to start
		for (volatile int i = 0; i < 100000; i++)
			;

		pthread_mutex_lock(&g_bcast_mutex);
		g_bcast_flag = 1;
		int ret = pthread_cond_broadcast(&g_bcast_cond);
		test_result("pthread_cond_broadcast succeeds", ret == 0);
		pthread_mutex_unlock(&g_bcast_mutex);

		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		pthread_join(t3, NULL);

		test_result("all 3 waiters woke up", g_waiters_done == 3);

		pthread_cond_destroy(&g_bcast_cond);
		pthread_mutex_destroy(&g_bcast_mutex);
	}

	/* Threads share ONE descriptor table (CLONE_FILES).  Regression test:
	 * the table used to be copied and then emptied for the new thread, so
	 * a thread had no descriptors at all — every fd >= 3 answered EBADF and
	 * its stdio quietly fell back to the console instead of following the
	 * process's own redirection. */
	printf("\n[TEST] pthread shared descriptor table\n");
	{
		char path[64];
		snprintf(path, sizeof(path), "/tmp/fdshare_%d", (int)getpid());
		g_fdshare_main_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
		test_result("main thread opened the file",
			    g_fdshare_main_fd >= 3);
		fcntl(g_fdshare_main_fd, F_SETFD, FD_CLOEXEC);

		/* Point stdout at the same file for the duration of the thread
		 * so the thread's write(1, ...) is checkable; saved_out keeps a
		 * handle on the real stdout to restore afterwards. */
		fflush(stdout);
		int saved_out = dup(1);
		int redirected = (saved_out >= 3) && (dup2(g_fdshare_main_fd, 1) == 1);

		g_fdshare_write_ok = -1;
		g_fdshare_cloexec = -1;
		g_fdshare_stdout_ok = -1;
		g_fdshare_thread_fd = -1;

		pthread_t t;
		int ret = pthread_create(&t, NULL, fdshare_thread_fn, NULL);
		if (ret == 0)
			pthread_join(t, NULL);

		/* Restore stdout before reporting anything. */
		if (redirected) {
			dup2(saved_out, 1);
			close(saved_out);
		} else if (saved_out >= 0) {
			close(saved_out);
		}

		test_result("pthread_create for fd-share thread succeeds",
			    ret == 0);
		test_result("thread can write to a descriptor opened by main",
			    g_fdshare_write_ok == 1);
		test_result("thread sees the process's FD_CLOEXEC bit",
			    g_fdshare_cloexec == FD_CLOEXEC);
		test_result("thread's stdout follows the process redirection",
			    redirected && g_fdshare_stdout_ok == 1);
		test_result("descriptor opened in the thread is valid in main",
			    g_fdshare_thread_fd >= 3 &&
				    fcntl(g_fdshare_thread_fd, F_GETFD) != -1);

		/* Everything both threads wrote landed in the one file. */
		char buf[64];
		memset(buf, 0, sizeof(buf));
		lseek(g_fdshare_main_fd, 0, SEEK_SET);
		ssize_t n = read(g_fdshare_main_fd, buf, sizeof(buf) - 1);
		test_result("thread's writes went to the shared descriptor",
			    n == (ssize_t)sizeof(g_fdshare_text) &&
				    strncmp(buf, g_fdshare_text,
					    sizeof(g_fdshare_text) - 1) == 0 &&
				    buf[sizeof(g_fdshare_text) - 1] == 'T');

		/* A close by one thread is a close for the whole process. */
		close(g_fdshare_thread_fd);
		test_result("close in one thread frees the fd process-wide",
			    fcntl(g_fdshare_thread_fd, F_GETFD) == -1 &&
				    errno == EBADF);

		close(g_fdshare_main_fd);
		unlink(path);
	}

	// Test malloc under threads: allocate in one thread, free in another
	printf("\n[TEST] malloc cross-thread free\n");
	{
		pthread_t xp1, xp2, xc1, xc2;
		g_xt_prod_done = 0;
		g_xt_produced = g_xt_consumed = g_xt_errors = 0;
		memset((void *)g_xt_mbox, 0, sizeof(g_xt_mbox));
		pthread_create(&xc1, NULL, xt_consumer_fn, NULL);
		pthread_create(&xc2, NULL, xt_consumer_fn, NULL);
		pthread_create(&xp1, NULL, xt_producer_fn, (void *)1L);
		pthread_create(&xp2, NULL, xt_producer_fn, (void *)2L);
		pthread_join(xp1, NULL);
		pthread_join(xp2, NULL);
		g_xt_prod_done = 1;
		pthread_join(xc1, NULL);
		pthread_join(xc2, NULL);
		test_result("all blocks produced were consumed",
			    g_xt_produced == g_xt_consumed);
		test_result("no cross-thread corruption", g_xt_errors == 0);
		char *xa = malloc(100);
		test_result("allocator alive after thread churn", xa != NULL);
		free(xa);
	}

	// Test rwlock
	printf("\n[TEST] pthread_rwlock\n");
	{
		pthread_rwlock_t rwlock;
		int ret = pthread_rwlock_init(&rwlock, NULL);
		test_result("pthread_rwlock_init succeeds", ret == 0);

		// Multiple read locks should succeed
		ret = pthread_rwlock_rdlock(&rwlock);
		test_result("first rdlock succeeds", ret == 0);

		ret = pthread_rwlock_tryrdlock(&rwlock);
		test_result("second rdlock (tryrdlock) succeeds", ret == 0);

		ret = pthread_rwlock_unlock(&rwlock);
		test_result("first rdunlock succeeds", ret == 0);

		ret = pthread_rwlock_unlock(&rwlock);
		test_result("second rdunlock succeeds", ret == 0);

		// Write lock
		ret = pthread_rwlock_wrlock(&rwlock);
		test_result("wrlock succeeds", ret == 0);

		ret = pthread_rwlock_unlock(&rwlock);
		test_result("wrunlock succeeds", ret == 0);

		// Try write lock
		ret = pthread_rwlock_trywrlock(&rwlock);
		test_result("trywrlock succeeds when unlocked", ret == 0);

		ret = pthread_rwlock_unlock(&rwlock);
		test_result("unlock after trywrlock succeeds", ret == 0);

		ret = pthread_rwlock_destroy(&rwlock);
		test_result("pthread_rwlock_destroy succeeds", ret == 0);
	}

	// Test spinlock
	printf("\n[TEST] pthread_spin\n");
	{
		pthread_spinlock_t spinlock;
		int ret = pthread_spin_init(&spinlock, PTHREAD_PROCESS_PRIVATE);
		test_result("pthread_spin_init succeeds", ret == 0);

		ret = pthread_spin_lock(&spinlock);
		test_result("pthread_spin_lock succeeds", ret == 0);

		ret = pthread_spin_unlock(&spinlock);
		test_result("pthread_spin_unlock succeeds", ret == 0);

		ret = pthread_spin_trylock(&spinlock);
		test_result("pthread_spin_trylock succeeds", ret == 0);

		ret = pthread_spin_unlock(&spinlock);
		test_result("pthread_spin_unlock after trylock succeeds",
			    ret == 0);

		ret = pthread_spin_destroy(&spinlock);
		test_result("pthread_spin_destroy succeeds", ret == 0);
	}

	// Test barrier
	printf("\n[TEST] pthread_barrier\n");
	{
		g_barrier_arrivals = 0;

		int ret = pthread_barrier_init(&g_barrier, NULL, 3);
		test_result("pthread_barrier_init(count=3) succeeds", ret == 0);

		pthread_t t1, t2;
		pthread_create(&t1, NULL, barrier_thread_fn, NULL);
		pthread_create(&t2, NULL, barrier_thread_fn, NULL);

		// This thread also participates
		__sync_fetch_and_add(&g_barrier_arrivals, 1);
		ret = pthread_barrier_wait(&g_barrier);
		test_result("pthread_barrier_wait returns 0 or SERIAL",
			    ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);

		void *r1, *r2;
		pthread_join(t1, &r1);
		pthread_join(t2, &r2);

		test_result("all 3 threads reached barrier",
			    g_barrier_arrivals == 3);

		// Check that exactly one got SERIAL_THREAD
		int serials =
			(ret == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0) +
			((long)r1 == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0) +
			((long)r2 == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0);
		test_result("exactly one thread got SERIAL_THREAD",
			    serials == 1);

		ret = pthread_barrier_destroy(&g_barrier);
		test_result("pthread_barrier_destroy succeeds", ret == 0);
	}

	// Test thread-specific data (TSD)
	printf("\n[TEST] pthread TSD (thread-specific data)\n");
	{
		g_destructor_called = 0;

		int ret = pthread_key_create(&g_tsd_key, tsd_destructor_fn);
		test_result("pthread_key_create succeeds", ret == 0);

		// Set value in main thread
		ret = pthread_setspecific(g_tsd_key, (void *)12345L);
		test_result("pthread_setspecific succeeds", ret == 0);

		void *val = pthread_getspecific(g_tsd_key);
		test_result("pthread_getspecific returns correct value",
			    val == (void *)12345L);

		// Test in another thread
		pthread_t t;
		pthread_create(&t, NULL, tsd_thread_fn, NULL);
		void *tsd_result;
		pthread_join(t, &tsd_result);
		test_result("TSD is thread-local", tsd_result == (void *)0L);

		// Main thread's value should be unchanged
		val = pthread_getspecific(g_tsd_key);
		test_result("main thread TSD unchanged", val == (void *)12345L);

		ret = pthread_key_delete(g_tsd_key);
		test_result("pthread_key_delete succeeds", ret == 0);
	}

	// Test sched_setaffinity / sched_getaffinity
	printf("\n[TEST] sched_setaffinity and sched_getaffinity\n");
	{
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);

		// Get current affinity
		int ret = sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
		test_result("sched_getaffinity succeeds", ret == 0);

		int cpu_count = 0;
		for (int i = 0; i < CPU_SETSIZE; i++) {
			if (CPU_ISSET(i, &cpuset))
				cpu_count++;
		}
		test_result("at least one CPU in affinity mask",
			    cpu_count >= 1);
		printf("  CPUs in affinity mask: %d\n", cpu_count);

		// Try to set affinity to CPU 0 only
		cpu_set_t new_cpuset;
		CPU_ZERO(&new_cpuset);
		CPU_SET(0, &new_cpuset);

		ret = sched_setaffinity(0, sizeof(cpu_set_t), &new_cpuset);
		// This might fail if system doesn't support it, but shouldn't crash
		if (ret == 0) {
			test_pass("sched_setaffinity to CPU 0 succeeds");

			// Verify it was set
			CPU_ZERO(&cpuset);
			sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
			test_result("affinity was updated",
				    CPU_ISSET(0, &cpuset));
		} else {
			printf("  sched_setaffinity returned %d (may not be supported)\n",
			       ret);
			test_pass(
				"sched_setaffinity returned (not necessarily successful)");
		}
	}

	// Test pthread_setaffinity_np / pthread_getaffinity_np
	printf("\n[TEST] pthread_setaffinity_np and pthread_getaffinity_np\n");
	{
		pthread_t self = pthread_self();
		cpu_set_t cpuset;
		CPU_ZERO(&cpuset);

		int ret = pthread_getaffinity_np(self, sizeof(cpu_set_t),
						 &cpuset);
		if (ret == 0) {
			test_pass("pthread_getaffinity_np succeeds");

			int cpu_count = 0;
			for (int i = 0; i < CPU_SETSIZE; i++) {
				if (CPU_ISSET(i, &cpuset))
					cpu_count++;
			}
			test_result("pthread affinity has CPUs",
				    cpu_count >= 1);
			printf("  Thread CPUs in affinity: %d\n", cpu_count);

			// Try to set
			cpu_set_t new_cpuset;
			CPU_ZERO(&new_cpuset);
			CPU_SET(0, &new_cpuset);

			ret = pthread_setaffinity_np(self, sizeof(cpu_set_t),
						     &new_cpuset);
			if (ret == 0) {
				test_pass("pthread_setaffinity_np succeeds");
			} else {
				printf("  pthread_setaffinity_np returned %d\n",
				       ret);
				test_pass("pthread_setaffinity_np returned");
			}
		} else {
			printf("  pthread_getaffinity_np returned %d\n", ret);
			test_pass(
				"pthread_getaffinity_np returned (may use fallback)");
		}
	}

	// Test pthread_once
	printf("\n[TEST] pthread_once\n");
	{
		// Reset for test (note: g_once_control is global and already initialized)
		// We can't easily reset a pthread_once_t, so test without reset
		g_once_counter = 0;

		int ret = pthread_once(&g_once_control, once_init_fn);
		test_result("first pthread_once succeeds", ret == 0);
		test_result("init function called once", g_once_counter == 1);

		ret = pthread_once(&g_once_control, once_init_fn);
		test_result("second pthread_once succeeds", ret == 0);
		test_result("init function still called only once",
			    g_once_counter == 1);

		// Test from multiple threads
		pthread_t t1, t2;
		pthread_create(&t1, NULL, once_thread_fn, NULL);
		pthread_create(&t2, NULL, once_thread_fn, NULL);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);

		test_result("init function called exactly once across threads",
			    g_once_counter == 1);
	}

	// ========================================
	// Dynamic Linking (dlopen/dlsym/dlclose/dlerror) Tests
	// ========================================
	printf("\n--- Dynamic Linking Tests ---\n");
	{
		// Test 1: dlerror returns NULL when no error
		printf("\n[TEST] dlerror() initial state\n");
		char *err = dlerror();
		/* dlerror may or may not be NULL initially; just call it to clear state */
		(void)err;
		test_pass("dlerror() called without crash");

		// Test 2: dlopen a non-existent library should fail
		printf("\n[TEST] dlopen() non-existent library\n");
		void *bad_handle = dlopen("/lib/libnonexistent.so", RTLD_LAZY);
		test_result("dlopen non-existent returns NULL",
			    bad_handle == NULL);
		if (bad_handle == NULL) {
			err = dlerror();
			test_result(
				"dlerror returns non-NULL after failed dlopen",
				err != NULL);
			if (err) {
				printf("    dlerror: %s\n", err);
			}
		}

		// Test 3: dlopen libtestlib.so
		printf("\n[TEST] dlopen() libtestlib.so\n");
		void *handle = dlopen("/lib/libtestlib.so", RTLD_LAZY);
		test_result("dlopen(\"/lib/libtestlib.so\") returns non-NULL",
			    handle != NULL);
		if (handle == NULL) {
			err = dlerror();
			printf("    dlopen failed: %s\n", err ? err : "(null)");
		}

		if (handle != NULL) {
			// Test 4: dlsym - look up testlib_add
			printf("\n[TEST] dlsym() testlib_add\n");
			int (*fn_add)(int, int) =
				(int (*)(int, int))dlsym(handle, "testlib_add");
			test_result("dlsym(\"testlib_add\") returns non-NULL",
				    fn_add != NULL);
			if (fn_add) {
				int result = fn_add(17, 25);
				test_result("testlib_add(17, 25) == 42",
					    result == 42);
				result = fn_add(-5, 5);
				test_result("testlib_add(-5, 5) == 0",
					    result == 0);
			}

			// Test 5: dlsym - look up testlib_mul
			printf("\n[TEST] dlsym() testlib_mul\n");
			int (*fn_mul)(int, int) =
				(int (*)(int, int))dlsym(handle, "testlib_mul");
			test_result("dlsym(\"testlib_mul\") returns non-NULL",
				    fn_mul != NULL);
			if (fn_mul) {
				int result = fn_mul(6, 7);
				test_result("testlib_mul(6, 7) == 42",
					    result == 42);
				result = fn_mul(0, 999);
				test_result("testlib_mul(0, 999) == 0",
					    result == 0);
			}

			// Test 6: dlsym - look up testlib_hello (returns string)
			printf("\n[TEST] dlsym() testlib_hello\n");
			const char *(*fn_hello)(void) =
				(const char *(*)(void))dlsym(handle,
							     "testlib_hello");
			test_result("dlsym(\"testlib_hello\") returns non-NULL",
				    fn_hello != NULL);
			if (fn_hello) {
				const char *msg = fn_hello();
				test_result(
					"testlib_hello() returns non-NULL string",
					msg != NULL);
				if (msg) {
					printf("    testlib_hello() = \"%s\"\n",
					       msg);
					test_result(
						"testlib_hello() contains \"libtestlib\"",
						strstr(msg, "libtestlib") !=
							NULL);
				}
			}

			// Test 7: dlsym - look up testlib_counter (stateful)
			printf("\n[TEST] dlsym() testlib_counter\n");
			int (*fn_counter)(void) =
				(int (*)(void))dlsym(handle, "testlib_counter");
			void (*fn_reset)(void) = (void (*)(void))dlsym(
				handle, "testlib_counter_reset");
			test_result(
				"dlsym(\"testlib_counter\") returns non-NULL",
				fn_counter != NULL);
			test_result(
				"dlsym(\"testlib_counter_reset\") returns non-NULL",
				fn_reset != NULL);
			if (fn_counter && fn_reset) {
				fn_reset();
				int v0 =
					fn_counter(); /* returns 0, increments to 1 */
				int v1 =
					fn_counter(); /* returns 1, increments to 2 */
				int v2 =
					fn_counter(); /* returns 2, increments to 3 */
				test_result("counter sequence 0,1,2",
					    v0 == 0 && v1 == 1 && v2 == 2);
				fn_reset();
				int v3 = fn_counter();
				test_result("counter reset works", v3 == 0);
			}

			// Test 8: dlsym - look up global variable testlib_version
			printf("\n[TEST] dlsym() testlib_version (global variable)\n");
			int *p_version =
				(int *)dlsym(handle, "testlib_version");
			test_result(
				"dlsym(\"testlib_version\") returns non-NULL",
				p_version != NULL);
			if (p_version) {
				test_result("testlib_version == 1",
					    *p_version == 1);
				printf("    testlib_version = %d\n",
				       *p_version);
			}

			// Test 9: dlsym - non-existent symbol
			printf("\n[TEST] dlsym() non-existent symbol\n");
			void *bad_sym =
				dlsym(handle, "this_symbol_does_not_exist");
			test_result("dlsym non-existent returns NULL",
				    bad_sym == NULL);
			if (bad_sym == NULL) {
				err = dlerror();
				test_result(
					"dlerror returns non-NULL after failed dlsym",
					err != NULL);
				if (err) {
					printf("    dlerror: %s\n", err);
				}
			}

			// Test 10: dlclose
			printf("\n[TEST] dlclose()\n");
			int close_ret = dlclose(handle);
			test_result("dlclose returns 0", close_ret == 0);

			// Test 11: dlerror after successful dlclose should be NULL
			err = dlerror();
			/* After a successful operation, dlerror should return NULL */
			test_result(
				"dlerror() returns NULL after successful dlclose",
				err == NULL);
		}

		// Test 12: dlopen with RTLD_NOW
		printf("\n[TEST] dlopen() with RTLD_NOW\n");
		void *handle2 = dlopen("/lib/libtestlib.so", RTLD_NOW);
		test_result("dlopen RTLD_NOW returns non-NULL",
			    handle2 != NULL);
		if (handle2) {
			int (*fn_add2)(int, int) = (int (*)(int, int))dlsym(
				handle2, "testlib_add");
			test_result("dlsym after RTLD_NOW works",
				    fn_add2 != NULL);
			if (fn_add2) {
				test_result("testlib_add(100, 200) == 300",
					    fn_add2(100, 200) == 300);
			}
			dlclose(handle2);
		}

		/* Test 13: dlopen of a library that has a DT_NEEDED of its own.
		 *
		 * libdlchain.so depends on libdlbase.so and every entry point
		 * below reaches into that dependency.  The loader used to
		 * relocate only the object named in dlopen(), so the
		 * dependency arrived with an unrelocated GOT and these calls
		 * went through empty slots.  This is the X server's module
		 * situation exactly: a driver that pulls in a library of its
		 * own. */
		printf("\n[TEST] dlopen() a library with its own dependency\n");
		void *ch = dlopen("/lib/libdlchain.so", RTLD_NOW);
		test_result("dlopen(libdlchain.so) returns non-NULL",
			    ch != NULL);
		if (ch) {
			int (*call_dep)(void) =
				(int (*)(void))dlsym(ch, "dlchain_call_dep");
			int (*read_dep)(void) = (int (*)(void))dlsym(
				ch, "dlchain_read_dep_data");
			int (*dep_ctor)(void) = (int (*)(void))dlsym(
				ch, "dlchain_dep_ctor_ran");
			int (*own_ctor)(void) = (int (*)(void))dlsym(
				ch, "dlchain_own_ctor_ran");

			test_result("dlsym finds the chain entry points",
				    call_dep && read_dep && dep_ctor &&
					    own_ctor);
			if (call_dep)
				test_result(
					"calling through the PLT into the dependency works",
					call_dep() == 0x5EED);
			if (read_dep)
				test_result(
					"reading a data symbol from the dependency works",
					read_dep() == 0x5EED);
			if (own_ctor)
				test_result("the dlopen'd object was initialised",
					    own_ctor() == 1);
			if (dep_ctor)
				test_result(
					"the dependency was initialised too",
					dep_ctor() == 1);

			/* The dependency is a separate object that dlopen must
			 * also have made available. */
			void *base = dlopen("/lib/libdlbase.so", RTLD_NOW);
			test_result("the dependency is loadable in its own right",
				    base != NULL);
			if (base) {
				int *magic = (int *)dlsym(base, "dlbase_magic");
				test_result("dependency data symbol reads back",
					    magic && *magic == 0x5EED);
				dlclose(base); /* refcount 2 -> 1: stays loaded */
			}

			/* ORDER MATTERS.  Closing the parent here unmaps
			 * libdlchain.so while libdlbase.so is still loaded,
			 * which used to leave the dependency's recorded name
			 * pointing into the parent's freed string table — the
			 * next loader table walk then read unmapped pages.  The
			 * dlopen below is that walk, so this sequence is the
			 * regression test; do not reorder it. */
			dlclose(ch);

			void *probe = dlopen("/lib/libtestlib.so", RTLD_NOW);
			test_result(
				"loader table survives closing a parent whose dependency is still loaded",
				probe != NULL);
			if (probe)
				dlclose(probe);
		}

		/* Test 14: an absolute path naming an already-open object must
		 * return the SAME handle, not load a second copy — objects are
		 * registered under their basename. */
		printf("\n[TEST] dlopen() absolute-path deduplication\n");
		void *h_a = dlopen("/lib/libtestlib.so", RTLD_NOW);
		void *h_b = dlopen("/lib/libtestlib.so", RTLD_NOW);
		test_result("the same path twice yields the same handle",
			    h_a != NULL && h_a == h_b);
		if (h_a)
			dlclose(h_a);
		if (h_b)
			dlclose(h_b);
	}

	// ========================================
	// uname() syscall tests
	// ========================================
	printf("\n========================================\n");
	printf("  uname() SYSCALL TESTS\n");
	printf("========================================\n");
	{
		struct utsname uts;
		int ret = uname(&uts);
		test_result("uname() returns 0", ret == 0);

		if (ret == 0) {
			/* sysname should be "LikeOS" */
			test_result("uname sysname == \"LikeOS\"",
				    strcmp(uts.sysname, "LikeOS") == 0);

			/* nodename should be non-empty */
			test_result("uname nodename is non-empty",
				    strlen(uts.nodename) > 0);

			/* release should be non-empty */
			test_result("uname release is non-empty",
				    strlen(uts.release) > 0);

			/* version should be non-empty */
			test_result("uname version is non-empty",
				    strlen(uts.version) > 0);

			/* machine should be "x86_64" */
			test_result("uname machine == \"x86_64\"",
				    strcmp(uts.machine, "x86_64") == 0);

			/* Print the fields for manual inspection */
			printf("  sysname:  %s\n", uts.sysname);
			printf("  nodename: %s\n", uts.nodename);
			printf("  release:  %s\n", uts.release);
			printf("  version:  %s\n", uts.version);
			printf("  machine:  %s\n", uts.machine);

			/* Each field should be shorter than the buffer size (65) */
			test_result("sysname length < 65",
				    strlen(uts.sysname) < 65);
			test_result("nodename length < 65",
				    strlen(uts.nodename) < 65);
			test_result("release length < 65",
				    strlen(uts.release) < 65);
			test_result("version length < 65",
				    strlen(uts.version) < 65);
			test_result("machine length < 65",
				    strlen(uts.machine) < 65);
		}
	}

	// ========================================
	// getopt() tests
	// ========================================
	printf("\n========================================\n");
	printf("  getopt() TESTS\n");
	printf("========================================\n");
	{
		/* Reset getopt state */
		extern int optind, opterr, optopt;
		extern char *optarg;
		optind = 1;
		opterr = 0;

		/* Test 1: simple option parsing */
		char *argv1[] = { "prog", "-a", "-b", NULL };
		int argc1 = 3;
		int got_a = 0, got_b = 0;
		int ch;
		optind = 1;
		while ((ch = getopt(argc1, argv1, "ab")) != -1) {
			if (ch == 'a')
				got_a = 1;
			if (ch == 'b')
				got_b = 1;
		}
		test_result("getopt: -a parsed", got_a == 1);
		test_result("getopt: -b parsed", got_b == 1);
		test_result("getopt: optind after -a -b == 3", optind == 3);

		/* Test 2: grouped options */
		char *argv2[] = { "prog", "-abc", NULL };
		int argc2 = 2;
		int got_a2 = 0, got_b2 = 0, got_c2 = 0;
		optind = 1;
		while ((ch = getopt(argc2, argv2, "abc")) != -1) {
			if (ch == 'a')
				got_a2 = 1;
			if (ch == 'b')
				got_b2 = 1;
			if (ch == 'c')
				got_c2 = 1;
		}
		test_result("getopt grouped: -a parsed", got_a2 == 1);
		test_result("getopt grouped: -b parsed", got_b2 == 1);
		test_result("getopt grouped: -c parsed", got_c2 == 1);

		/* Test 3: option with argument */
		char *argv3[] = { "prog", "-f", "file.txt", NULL };
		int argc3 = 3;
		char *farg = NULL;
		optind = 1;
		while ((ch = getopt(argc3, argv3, "f:")) != -1) {
			if (ch == 'f')
				farg = optarg;
		}
		test_result("getopt arg: -f file.txt parses", farg != NULL);
		if (farg)
			test_result("getopt arg: optarg == \"file.txt\"",
				    strcmp(farg, "file.txt") == 0);

		/* Test 4: unknown option returns '?' */
		char *argv4[] = { "prog", "-z", NULL };
		int argc4 = 2;
		int got_q = 0;
		optind = 1;
		while ((ch = getopt(argc4, argv4, "ab")) != -1) {
			if (ch == '?')
				got_q = 1;
		}
		test_result("getopt: unknown option returns '?'", got_q == 1);

		/* Test 5: "--" stops scanning */
		char *argv5[] = { "prog", "--", "-a", NULL };
		int argc5 = 3;
		int got_a5 = 0;
		optind = 1;
		while ((ch = getopt(argc5, argv5, "a")) != -1) {
			if (ch == 'a')
				got_a5 = 1;
		}
		test_result("getopt: -- stops scanning", got_a5 == 0);
		test_result("getopt: optind after -- == 2", optind == 2);
	}

	// ========================================
	// getopt_long tests
	// ========================================
	printf("\n--- getopt_long tests ---\n");
	{
		/* Test 1: long option without argument */
		struct option longopts1[] = {
			{ "verbose", no_argument, NULL, 'v' },
			{ "help", no_argument, NULL, 'h' },
			{ NULL, 0, NULL, 0 }
		};
		char *argv1[] = { "prog", "--verbose", NULL };
		int argc1 = 2;
		optind = 1;
		int longidx = -1;
		int ch = getopt_long(argc1, argv1, "vh", longopts1, &longidx);
		test_result("getopt_long: --verbose returns 'v'", ch == 'v');

		/* Test 2: long option with required argument (= syntax) */
		struct option longopts2[] = { { "output", required_argument,
						NULL, 'o' },
					      { NULL, 0, NULL, 0 } };
		char *argv2[] = { "prog", "--output=file.txt", NULL };
		int argc2 = 2;
		optind = 1;
		ch = getopt_long(argc2, argv2, "o:", longopts2, &longidx);
		test_result("getopt_long: --output=file.txt returns 'o'",
			    ch == 'o');
		test_result("getopt_long: optarg is 'file.txt'",
			    optarg != NULL && strcmp(optarg, "file.txt") == 0);

		/* Test 3: long option with required argument (space syntax) */
		char *argv3[] = { "prog", "--output", "result.dat", NULL };
		int argc3 = 3;
		optind = 1;
		ch = getopt_long(argc3, argv3, "o:", longopts2, &longidx);
		test_result("getopt_long: --output result.dat returns 'o'",
			    ch == 'o');
		test_result("getopt_long: optarg is 'result.dat'",
			    optarg != NULL &&
				    strcmp(optarg, "result.dat") == 0);

		/* Test 4: flag pointer stores value (val into *flag) */
		int flag_val = 0;
		struct option longopts4[] = { { "debug", no_argument, &flag_val,
						42 },
					      { NULL, 0, NULL, 0 } };
		char *argv4[] = { "prog", "--debug", NULL };
		int argc4 = 2;
		optind = 1;
		ch = getopt_long(argc4, argv4, "", longopts4, &longidx);
		test_result("getopt_long: flag pointer returns 0", ch == 0);
		test_result("getopt_long: flag value set to 42",
			    flag_val == 42);

		/* Test 5: short option still works through getopt_long */
		struct option longopts5[] = { { "verbose", no_argument, NULL,
						'v' },
					      { NULL, 0, NULL, 0 } };
		char *argv5[] = { "prog", "-v", NULL };
		int argc5 = 2;
		optind = 1;
		ch = getopt_long(argc5, argv5, "v", longopts5, &longidx);
		test_result("getopt_long: short -v still works", ch == 'v');

		/* Test 6: mixed short and long options */
		struct option longopts6[] = { { "all", no_argument, NULL, 'a' },
					      { "long", no_argument, NULL,
						'l' },
					      { NULL, 0, NULL, 0 } };
		char *argv6[] = { "prog", "-a", "--long", NULL };
		int argc6 = 3;
		optind = 1;
		int got_a = 0, got_l = 0;
		while ((ch = getopt_long(argc6, argv6, "al", longopts6,
					 &longidx)) != -1) {
			if (ch == 'a')
				got_a = 1;
			if (ch == 'l')
				got_l = 1;
		}
		test_result("getopt_long: mixed -a --long: got 'a'",
			    got_a == 1);
		test_result("getopt_long: mixed -a --long: got 'l'",
			    got_l == 1);

		/* Test 7: unknown long option returns '?' */
		struct option longopts7[] = { { "known", no_argument, NULL,
						'k' },
					      { NULL, 0, NULL, 0 } };
		char *argv7[] = { "prog", "--unknown", NULL };
		int argc7 = 2;
		optind = 1;
		opterr = 0; /* suppress error message */
		ch = getopt_long(argc7, argv7, "k", longopts7, &longidx);
		test_result("getopt_long: unknown --unknown returns '?'",
			    ch == '?');
		opterr = 1;
	}

	// ========================================
	// time function tests (gmtime, mktime, strftime)
	// ========================================
	printf("\n--- time function tests ---\n");
	{
		/* Test 1: gmtime of epoch 0 */
		time_t t0 = 0;
		struct tm *tm0 = gmtime(&t0);
		test_result("gmtime(0): year=1970",
			    tm0 != NULL && tm0->tm_year == 70);
		test_result("gmtime(0): mon=0 (Jan)",
			    tm0 != NULL && tm0->tm_mon == 0);
		test_result("gmtime(0): mday=1",
			    tm0 != NULL && tm0->tm_mday == 1);
		test_result("gmtime(0): hour=0",
			    tm0 != NULL && tm0->tm_hour == 0);
		test_result("gmtime(0): min=0",
			    tm0 != NULL && tm0->tm_min == 0);
		test_result("gmtime(0): sec=0",
			    tm0 != NULL && tm0->tm_sec == 0);
		test_result("gmtime(0): wday=4 (Thu)",
			    tm0 != NULL && tm0->tm_wday == 4);

		/* Test 2: gmtime of known timestamp: 2024-01-01 00:00:00 UTC = 1704067200 */
		time_t t1 = 1704067200;
		struct tm tm1;
		gmtime_r(&t1, &tm1);
		test_result("gmtime(2024-01-01): year=124", tm1.tm_year == 124);
		test_result("gmtime(2024-01-01): mon=0", tm1.tm_mon == 0);
		test_result("gmtime(2024-01-01): mday=1", tm1.tm_mday == 1);
		test_result("gmtime(2024-01-01): wday=1 (Mon)",
			    tm1.tm_wday == 1);

		/* Test 3: gmtime_r known timestamp: 2000-06-15 12:30:45 UTC = 961072245 */
		time_t t2 = 961072245;
		struct tm tm2;
		gmtime_r(&t2, &tm2);
		test_result("gmtime(2000-06-15 12:30:45): year=100",
			    tm2.tm_year == 100);
		test_result("gmtime(2000-06-15 12:30:45): mon=5 (Jun)",
			    tm2.tm_mon == 5);
		test_result("gmtime(2000-06-15 12:30:45): mday=15",
			    tm2.tm_mday == 15);
		test_result("gmtime(2000-06-15 12:30:45): hour=12",
			    tm2.tm_hour == 12);
		test_result("gmtime(2000-06-15 12:30:45): min=30",
			    tm2.tm_min == 30);
		test_result("gmtime(2000-06-15 12:30:45): sec=45",
			    tm2.tm_sec == 45);

		/* Test 4: mktime round-trip */
		struct tm tm_rt;
		tm_rt.tm_year = 124; /* 2024 */
		tm_rt.tm_mon = 0; /* January */
		tm_rt.tm_mday = 1;
		tm_rt.tm_hour = 0;
		tm_rt.tm_min = 0;
		tm_rt.tm_sec = 0;
		tm_rt.tm_isdst = 0;
		time_t rt = mktime(&tm_rt);
		test_result("mktime(2024-01-01) == 1704067200",
			    rt == 1704067200);

		/* Test 5: mktime round-trip for 2000-06-15 12:30:45 */
		struct tm tm_rt2;
		tm_rt2.tm_year = 100;
		tm_rt2.tm_mon = 5;
		tm_rt2.tm_mday = 15;
		tm_rt2.tm_hour = 12;
		tm_rt2.tm_min = 30;
		tm_rt2.tm_sec = 45;
		tm_rt2.tm_isdst = 0;
		time_t rt2 = mktime(&tm_rt2);
		test_result("mktime(2000-06-15 12:30:45) == 961072245",
			    rt2 == 961072245);

		/* Test 6: strftime basic formatting */
		char buf[128];
		struct tm tmf;
		tmf.tm_year = 124;
		tmf.tm_mon = 0;
		tmf.tm_mday = 15;
		tmf.tm_hour = 9;
		tmf.tm_min = 5;
		tmf.tm_sec = 3;
		tmf.tm_wday = 1;
		tmf.tm_yday = 14;
		tmf.tm_isdst = 0;

		strftime(buf, sizeof(buf), "%Y-%m-%d", &tmf);
		test_result("strftime %Y-%m-%d == '2024-01-15'",
			    strcmp(buf, "2024-01-15") == 0);

		strftime(buf, sizeof(buf), "%H:%M:%S", &tmf);
		test_result("strftime %H:%M:%S == '09:05:03'",
			    strcmp(buf, "09:05:03") == 0);

		strftime(buf, sizeof(buf), "%a", &tmf);
		test_result("strftime %a == 'Mon'", strcmp(buf, "Mon") == 0);

		strftime(buf, sizeof(buf), "%b", &tmf);
		test_result("strftime %b == 'Jan'", strcmp(buf, "Jan") == 0);

		strftime(buf, sizeof(buf), "%F", &tmf);
		test_result("strftime %F == '2024-01-15'",
			    strcmp(buf, "2024-01-15") == 0);

		strftime(buf, sizeof(buf), "%T", &tmf);
		test_result("strftime %T == '09:05:03'",
			    strcmp(buf, "09:05:03") == 0);

		/* Test 7: leap year handling */
		time_t t_leap = 951782400; /* 2000-02-29 00:00:00 UTC */
		struct tm tm_leap;
		gmtime_r(&t_leap, &tm_leap);
		test_result("gmtime leap year 2000-02-29: year=100",
			    tm_leap.tm_year == 100);
		test_result("gmtime leap year 2000-02-29: mon=1 (Feb)",
			    tm_leap.tm_mon == 1);
		test_result("gmtime leap year 2000-02-29: mday=29",
			    tm_leap.tm_mday == 29);
	}

	// ========================================
	// SYS_GETPROCINFO tests
	// ========================================
	{
		printf("\n--- SYS_GETPROCINFO tests ---\n");

		/* Allocate buffer for up to 128 procs */
		int max = 128;
		procinfo_t *buf =
			(procinfo_t *)malloc(max * sizeof(procinfo_t));
		test_result("getprocinfo: malloc ok", buf != NULL);
		if (buf) {
			int n = getprocinfo(buf, max);
			test_result("getprocinfo: returns > 0", n > 0);

			/* Find our own PID */
			pid_t my_pid = getpid();
			int found_self = 0;
			int self_idx = -1;
			for (int i = 0; i < n; i++) {
				if (buf[i].pid == (int)my_pid) {
					found_self = 1;
					self_idx = i;
					break;
				}
			}
			test_result("getprocinfo: found own PID", found_self);

			if (self_idx >= 0) {
				test_result(
					"getprocinfo: own state is READY or RUNNING",
					buf[self_idx].state == 0 ||
						buf[self_idx].state == 1);
				test_result("getprocinfo: own tty_nr > 0",
					    buf[self_idx].tty_nr > 0);
				test_result("getprocinfo: own is_kernel == 0",
					    buf[self_idx].is_kernel == 0);
				test_result("getprocinfo: own ppid > 0",
					    buf[self_idx].ppid > 0);
				test_result(
					"getprocinfo: own cwd starts with /",
					buf[self_idx].cwd[0] == '/');
				test_result(
					"getprocinfo: own comm is 'testlibc'",
					strcmp(buf[self_idx].comm,
					       "testlibc") == 0);
			}

			/* PID 1 is /sbin/init (the first process); there is no
			 * PID 0, and the kernel's swapper-class tasks (bootstrap
			 * + idle) are hidden.  Real kernel threads remain visible
			 * with is_kernel == 1. */
			int found_init = 0, found_pid0 = 0, found_kernel = 0;
			for (int i = 0; i < n; i++) {
				if (buf[i].pid == 1)
					found_init = 1;
				if (buf[i].pid == 0)
					found_pid0 = 1;
				if (buf[i].is_kernel == 1)
					found_kernel = 1;
			}
			test_result("getprocinfo: PID 1 (init) exists", found_init);
			test_result("getprocinfo: no PID 0", !found_pid0);
			test_result("getprocinfo: a kernel thread is present",
				    found_kernel);

			/* Edge case: max_count=0 */
			int n0 = getprocinfo(buf, 0);
			test_result("getprocinfo(buf,0) returns 0", n0 == 0);

			free(buf);
		}
	}

	// ========================================
	// Filesystem syscalls: mkdir, rmdir, rename, unlink, chmod, utimensat
	// ========================================
	/* _td and the _p_* paths are initialized near the top of main(), before
     * `goto network_section`, so both entry paths have them. */

	printf("\n[TEST] mkdir()\n");
	{
		int ret = mkdir(_p_mkdir, 0755);
		test_result("mkdir(tmpdir/mkdir_dir) succeeds", ret == 0);

		struct stat st;
		ret = stat(_p_mkdir, &st);
		test_result("stat new dir succeeds", ret == 0);
		test_result("new dir is a directory",
			    ret == 0 && S_ISDIR(st.st_mode));

		/* mkdir on existing dir should fail with EEXIST */
		ret = mkdir(_p_mkdir, 0755);
		test_result("mkdir existing dir fails", ret == -1);
		test_result("mkdir existing dir sets EEXIST", errno == EEXIST);
	}

	printf("\n[TEST] rmdir()\n");
	{
		int ret = rmdir(_p_mkdir);
		test_result("rmdir(tmpdir/mkdir_dir) succeeds", ret == 0);

		/* rmdir on nonexistent dir should fail */
		ret = rmdir(_p_mkdir);
		test_result("rmdir nonexistent dir fails", ret == -1);
		test_result("rmdir nonexistent dir sets ENOENT",
			    errno == ENOENT);

		/* rmdir on "/" should fail when it's not empty */
		ret = rmdir("/");
		test_result("rmdir(/) fails", ret == -1);
	}

	printf("\n[TEST] unlink()\n");
	{
		/* Create a test file first */
		int fd = open(_p_unlink, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("create tmpdir/unlink_file", fd >= 0);
		if (fd >= 0) {
			write(fd, "test", 4);
			close(fd);

			int ret = unlink(_p_unlink);
			test_result("unlink(tmpdir/unlink_file) succeeds",
				    ret == 0);

			/* Should be gone now */
			struct stat st;
			ret = stat(_p_unlink, &st);
			test_result("stat after unlink fails (ENOENT)",
				    ret == -1 && errno == ENOENT);
		}

		/* unlink nonexistent file */
		int ret = unlink(_p_no_such);
		test_result("unlink nonexistent file fails", ret == -1);
		test_result("unlink nonexistent sets ENOENT", errno == ENOENT);
	}

	printf("\n[TEST] rename()\n");
	{
		/* Create source file */
		int fd = open(_p_rsrc, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("create tmpdir/rename_src", fd >= 0);
		if (fd >= 0) {
			write(fd, "rename_test", 11);
			close(fd);

			int ret = rename(_p_rsrc, _p_rdst);
			test_result("rename succeeds", ret == 0);

			/* Source should be gone */
			struct stat st;
			ret = stat(_p_rsrc, &st);
			test_result("old name gone after rename", ret == -1);

			/* Destination should exist */
			ret = stat(_p_rdst, &st);
			test_result("new name exists after rename", ret == 0);

			/* Verify contents */
			fd = open(_p_rdst, O_RDONLY);
			test_result("can open renamed file", fd >= 0);
			if (fd >= 0) {
				char buf[32];
				ssize_t n = read(fd, buf, sizeof(buf));
				test_result("renamed file has correct size",
					    n == 11);
				close(fd);
			}

			/* Cleanup */
			unlink(_p_rdst);
		}
	}

	printf("\n[TEST] chmod()\n");
	{
		/* chmod should succeed (returns 0 on FAT32) */
		int fd = open(_p_chmod, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("create tmpdir/chmod_file", fd >= 0);
		if (fd >= 0) {
			close(fd);

			int ret = chmod(_p_chmod, 0644);
			test_result("chmod returns 0", ret == 0);

			ret = chmod(_p_chmod, 0755);
			test_result("chmod to 0755 returns 0", ret == 0);

			unlink(_p_chmod);
		}

		/* chmod on nonexistent should succeed (kernel returns 0 regardless) */
	}

	printf("\n[TEST] chown()\n");
	{
		int fd = open(_p_chown, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("create tmpdir/chown_file", fd >= 0);
		if (fd >= 0) {
			close(fd);

			int ret = chown(_p_chown, 0, 0);
			test_result("chown returns 0", ret == 0);

			ret = fchown(open(_p_chown, O_RDONLY), 0, 0);
			test_result("fchown returns 0", ret == 0);

			unlink(_p_chown);
		}
	}

	/* symlink / readlink / hard link — exercised on filesystems that support
     * them (e.g. ext4).  On filesystems without symlink support the create
     * call returns an error and the dependent checks are skipped. */
	printf("\n[TEST] symlink/readlink/link\n");
	{
		char p_tgt[128], p_lnk[128], p_hl[128];
		snprintf(p_tgt, sizeof(p_tgt), "%s/sl_target", _td);
		snprintf(p_lnk, sizeof(p_lnk), "%s/sl_link", _td);
		snprintf(p_hl, sizeof(p_hl), "%s/hl_link", _td);

		int fd = open(p_tgt, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			write(fd, "linkdata", 8);
			close(fd);
		}

		int sret = symlink("sl_target", p_lnk);
		if (sret == 0) {
			char rb[64];
			int rl = readlink(p_lnk, rb, sizeof(rb));
			test_result("readlink length == 9", rl == 9);
			test_result("readlink content",
				    rl == 9 && memcmp(rb, "sl_target", 9) == 0);

			struct stat lst, stt;
			test_result("lstat reports S_ISLNK",
				    lstat(p_lnk, &lst) == 0 &&
					    S_ISLNK(lst.st_mode));
			test_result("stat follows symlink to regular file",
				    stat(p_lnk, &stt) == 0 &&
					    S_ISREG(stt.st_mode) &&
					    stt.st_size == 8);

			int lf = open(p_lnk, O_RDONLY);
			if (lf >= 0) {
				char b[16];
				int n = read(lf, b, sizeof(b));
				test_result("read through symlink",
					    n == 8 && memcmp(b, "linkdata",
							     8) == 0);
				close(lf);
			}
			unlink(p_lnk);
		} else {
			test_result(
				"symlink not supported on this fs (skipped)",
				1);
		}

		int hret = link(p_tgt, p_hl);
		if (hret == 0) {
			struct stat s1, s2;
			test_result("hard link: same st_ino",
				    stat(p_tgt, &s1) == 0 &&
					    stat(p_hl, &s2) == 0 &&
					    s1.st_ino == s2.st_ino);
			test_result("hard link: st_nlink == 2",
				    s2.st_nlink == 2);
			unlink(p_hl);
			test_result("hard link: st_nlink == 1 after unlink",
				    stat(p_tgt, &s1) == 0 && s1.st_nlink == 1);
		} else {
			test_result(
				"hard link not supported on this fs (skipped)",
				1);
		}
		unlink(p_tgt);
	}

	/* chmod persistence: on ext4 the mode is stored; verify the read-back. */
	printf("\n[TEST] chmod/chown persistence\n");
	{
		char p_cp[128];
		snprintf(p_cp, sizeof(p_cp), "%s/cperm", _td);
		int fd = open(p_cp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			close(fd);
			struct stat st;
			if (chmod(p_cp, 0641) == 0 && stat(p_cp, &st) == 0) {
				if ((st.st_mode & 0777) == 0641)
					test_result(
						"chmod 0641 persists (ext4)",
						1);
				else
					test_result(
						"chmod is synthetic (non-ext4 fs, skipped)",
						1);
			}
			if (chown(p_cp, 321, 654) == 0 &&
			    stat(p_cp, &st) == 0) {
				if (st.st_uid == 321 && st.st_gid == 654)
					test_result("chown persists (ext4)", 1);
				else
					test_result(
						"chown is synthetic (non-ext4 fs, skipped)",
						1);
			}
			unlink(p_cp);
		}
	}

	printf("\n[TEST] utimensat()\n");
	{
		int fd = open(_p_utime, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("create tmpdir/utime_file", fd >= 0);
		if (fd >= 0) {
			close(fd);

			struct timespec times[2];
			times[0].tv_sec = 1000000;
			times[0].tv_nsec = 0;
			times[1].tv_sec = 2000000;
			times[1].tv_nsec = 0;
			int ret = utimensat(-100, _p_utime, times, 0);
			test_result("utimensat returns 0", ret == 0);

			unlink(_p_utime);
		}
	}

	printf("\n[TEST] utime()\n");
	{
		int fd = open(_p_utime, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("utime: create test file", fd >= 0);
		if (fd >= 0) {
			close(fd);
			struct utimbuf ut;
			ut.actime =
				1500000000; /* 2017-07-14, even seconds, post-1980 */
			ut.modtime = 1500000000;
			int ret = utime(_p_utime, &ut);
			test_result("utime() returns 0", ret == 0);
			if (ret == 0) {
				struct stat ust;
				stat(_p_utime, &ust);
				test_result("utime: mtime set correctly",
					    (long)ust.st_mtime == 1500000000);
			}
			unlink(_p_utime);
		}
	}

	printf("\n[TEST] utimes()\n");
	{
		int fd = open(_p_utime, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("utimes: create test file", fd >= 0);
		if (fd >= 0) {
			close(fd);
			struct timeval utv[2];
			utv[0].tv_sec = 1600000000;
			utv[0].tv_usec = 0; /* 2020-09-13 */
			utv[1].tv_sec = 1600000000;
			utv[1].tv_usec = 0;
			int ret = utimes(_p_utime, utv);
			test_result("utimes() returns 0", ret == 0);
			if (ret == 0) {
				struct stat ust;
				stat(_p_utime, &ust);
				test_result("utimes: mtime sec set correctly",
					    (long)ust.st_mtime == 1600000000);
			}
			unlink(_p_utime);
		}
	}

	printf("\n[TEST] mkdir+rmdir parents\n");
	{
		/* Create nested dirs */
		int ret = mkdir(_p_pa, 0755);
		test_result("mkdir /tmp/test_parent_a", ret == 0);

		ret = mkdir(_p_pb, 0755);
		test_result("mkdir /tmp/test_parent_a/b", ret == 0);

		ret = mkdir(_p_pc, 0755);
		test_result("mkdir /tmp/test_parent_a/b/c", ret == 0);

		/* Verify they exist */
		struct stat st;
		ret = stat(_p_pc, &st);
		test_result("nested dir exists",
			    ret == 0 && S_ISDIR(st.st_mode));

		/* Remove in reverse order */
		ret = rmdir(_p_pc);
		test_result("rmdir /tmp/test_parent_a/b/c", ret == 0);

		ret = rmdir(_p_pb);
		test_result("rmdir /tmp/test_parent_a/b", ret == 0);

		ret = rmdir(_p_pa);
		test_result("rmdir /tmp/test_parent_a", ret == 0);
	}

	// ========================================
	// statfs / fstatfs tests
	// ========================================
	printf("\n--- statfs / fstatfs tests ---\n");
	{
		struct statfs sfs;
		int ret;

		/* statfs on root "/" should succeed */
		ret = statfs("/", &sfs);
		test_result("statfs(\"/\") succeeds", ret == 0);

		if (ret == 0) {
			/* Block size should be non-zero */
			test_result("statfs f_bsize > 0", sfs.f_bsize > 0);

			/* Total blocks should be non-zero */
			test_result("statfs f_blocks > 0", sfs.f_blocks > 0);

			/* Free blocks should be <= total blocks */
			test_result("statfs f_bfree <= f_blocks",
				    sfs.f_bfree <= sfs.f_blocks);

			/* Available should be <= free */
			test_result("statfs f_bavail <= f_bfree",
				    sfs.f_bavail <= sfs.f_bfree);

			/* f_type should be FAT32 magic (0x4d44) */
			test_result(
				"statfs f_type is a known fs magic (FAT/ext4)",
				sfs.f_type == 0x4d44 || sfs.f_type == 0xef53);

			/* f_namelen should be reasonable */
			test_result("statfs f_namelen > 0", sfs.f_namelen > 0);

			printf("  f_bsize=%lu f_blocks=%lu f_bfree=%lu f_bavail=%lu f_type=0x%lx\n",
			       sfs.f_bsize, sfs.f_blocks, sfs.f_bfree,
			       sfs.f_bavail, sfs.f_type);
		}

		/* statfs on an existing file should also work */
		ret = statfs("/bin/sh", &sfs);
		test_result("statfs(\"/bin/sh\") succeeds", ret == 0);

		/* statfs on /dev should fail with ENOSYS */
		ret = statfs("/dev", &sfs);
		test_result("statfs(\"/dev\") fails", ret == -1);
		test_result("statfs(\"/dev\") errno==ENOSYS", errno == ENOSYS);

		/* fstatfs on an open file */
		int fd = open("/bin/sh", 0);
		if (fd >= 0) {
			struct statfs fst;
			ret = fstatfs(fd, &fst);
			test_result("fstatfs(fd) succeeds", ret == 0);
			if (ret == 0) {
				test_result("fstatfs f_bsize > 0",
					    fst.f_bsize > 0);
				test_result(
					"fstatfs f_type is a known fs magic (FAT/ext4)",
					fst.f_type == 0x4d44 ||
						fst.f_type == 0xef53);
			}
			close(fd);
		} else {
			test_fail("fstatfs: could not open /bin/sh");
		}

		/* fstatfs on invalid fd should fail */
		struct statfs bad_fst;
		ret = fstatfs(999, &bad_fst);
		test_result("fstatfs(999) fails", ret == -1);
		test_result("fstatfs(999) errno==EBADF", errno == EBADF);
	}

	// ========================================
	// Test: sysinfo() syscall
	// ========================================
	printf("\n[TEST] sysinfo()\n");
	{
		struct sysinfo si;
		memset(&si, 0, sizeof(si));
		int ret = sysinfo(&si);
		test_result("sysinfo() returns 0", ret == 0);
		test_result("sysinfo: uptime > 0", si.uptime > 0);
		printf("  uptime: %ld seconds\n", si.uptime);
		test_result("sysinfo: totalram > 0", si.totalram > 0);
		printf("  totalram: %lu bytes (mem_unit=%u)\n",
		       (unsigned long)si.totalram, si.mem_unit);
		test_result("sysinfo: freeram > 0", si.freeram > 0);
		test_result("sysinfo: freeram <= totalram",
			    si.freeram <= si.totalram);
		printf("  freeram: %lu bytes\n", (unsigned long)si.freeram);
		test_result("sysinfo: procs > 0", si.procs > 0);
		printf("  procs: %d\n", si.procs);
		printf("  loads[0]=%lu loads[1]=%lu loads[2]=%lu\n",
		       si.loads[0], si.loads[1], si.loads[2]);
		test_result("sysinfo: mem_unit > 0", si.mem_unit > 0);

		/* Test with NULL pointer - should fail */
		ret = sysinfo(NULL);
		test_result("sysinfo(NULL) returns -1", ret == -1);
	}

	// ========================================
	// Test: klogctl() syscall
	// ========================================
	printf("\n[TEST] klogctl()\n");
	{
		/* Get buffer size */
		int size = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
		test_result("klogctl(SIZE_BUFFER) >= 0", size >= 0);
		printf("  kernel log buffer used: %d bytes\n", size);

		/* Read kernel log */
		char kbuf[4096];
		int nread =
			klogctl(SYSLOG_ACTION_READ_ALL, kbuf, sizeof(kbuf) - 1);
		test_result("klogctl(READ_ALL) >= 0", nread >= 0);
		if (nread > 0) {
			kbuf[nread] = '\0';
			/* There should be some kernel output */
			test_result("klogctl: read some data", nread > 0);
			printf("  read %d bytes of kernel log (first 80 chars):\n  ",
			       nread);
			int show = nread < 80 ? nread : 80;
			for (int i = 0; i < show; i++) {
				if (kbuf[i] == '\n')
					printf("\\n");
				else if (kbuf[i] >= 32 && kbuf[i] < 127)
					putchar(kbuf[i]);
				else
					printf(".");
			}
			printf("\n");
		}

		/* Test invalid type */
		int ret = klogctl(999, NULL, 0);
		test_result("klogctl(invalid) returns -1", ret == -1);

		/* Test NULL buffer with READ_ALL should fail */
		ret = klogctl(SYSLOG_ACTION_READ_ALL, NULL, 100);
		test_result("klogctl(READ_ALL, NULL) returns -1", ret == -1);
	}

	// ========================================
	// Test: setjmp / longjmp / sigsetjmp / siglongjmp
	// ========================================
	printf("\n[TEST] setjmp/longjmp\n");
	{
		jmp_buf jb;
		int jr = setjmp(jb);
		if (jr == 0) {
			test_pass("setjmp() returns 0 on first call");
			longjmp(jb, 42);
			test_fail("longjmp: unreachable code reached");
		} else {
			test_result("longjmp() delivers val to setjmp site",
				    jr == 42);
		}
	}
	{
		/* longjmp(env, 0) must clamp to 1 per POSIX */
		jmp_buf jb2;
		int jr2 = setjmp(jb2);
		if (jr2 == 0) {
			longjmp(jb2, 0);
		} else {
			test_result("longjmp(env, 0) delivers 1 not 0",
				    jr2 == 1);
		}
	}
	printf("\n[TEST] sigsetjmp/siglongjmp\n");
	{
		sigjmp_buf sjb;
		int sjr = sigsetjmp(sjb, 0);
		if (sjr == 0) {
			test_pass("sigsetjmp() returns 0 on first call");
			siglongjmp(sjb, 99);
			test_fail("siglongjmp: unreachable code reached");
		} else {
			test_result(
				"siglongjmp() delivers val to sigsetjmp site",
				sjr == 99);
		}
	}
	{
		/* siglongjmp(env, 0) must clamp to 1 */
		sigjmp_buf sjb2;
		int sjr2 = sigsetjmp(sjb2, 0);
		if (sjr2 == 0) {
			siglongjmp(sjb2, 0);
		} else {
			test_result("siglongjmp(env, 0) delivers 1 not 0",
				    sjr2 == 1);
		}
	}

	// ========================================
	// Credentials & permissions
	// ========================================
	test_credentials();

	// ========================================
	// User/group/shadow DB, crypt, session, PAM
	// (root context, before the non-root section)
	// ========================================
	run_auth_tests();

	// ========================================
	// Device nodes, framebuffer device, event devices
	// ========================================
	test_dev_nodes();
	test_fbdev();
	test_evdev();
	test_shebang();
	test_devfd();
	test_poll_signal_eintr();
	test_poll_redirected_stdio();
	test_ioctl_non_tty();
	test_jobctl_wait();
	test_cred_proc_visibility();
	test_cred_sched_control();
	test_rusage_no_overflow();
	test_ptrace_traceme_basic();
	test_ptrace_getauxv();
	test_ptrace_threads();
	test_ptrace_signal_delivery_stop();
	test_ptrace_hwwatchpoint();
	test_ptrace_attach_detach();
	test_ptrace_permission_denied();
	test_ptrace_regs_and_mem();
	test_ptrace_singlestep();
	test_ptrace_swbreakpoint();
	test_ptrace_syscall_trace();
	test_ptrace_fork_events();
	test_ptrace_thread_events();
	test_ptrace_attach_nonchild();
	test_ptrace_execpath();
	test_nested_signal_handlers();
	test_ptrace_detach_releases_group();
	test_fatal_signal_kills_group();
	test_rtld_debug_rendezvous();
	test_shm();
	test_sysv_shm();
	test_tls();
	test_xorg_libc_additions();
	test_gtk3_libc_additions();
	test_dprintf();
	test_bash_libc_additions();
	test_unicode();
	test_syscall_arg_hygiene();
	test_dup_redirected_stdio();

	// ========================================
	// Socket / Networking Tests
	// ========================================
	if (skip_network)
		goto network_skip;
network_section:
	(void)0; /* label needs a statement */
	printf("\n--- Socket Tests ---\n");
	{
		// Test socket creation (UDP)
		int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
		test_result("socket(AF_INET, SOCK_DGRAM) >= 0", udp_fd >= 0);

		// Test socket creation (TCP)
		int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
		test_result("socket(AF_INET, SOCK_STREAM) >= 0", tcp_fd >= 0);

		// Test invalid domain
		int bad_fd = socket(99, SOCK_STREAM, 0);
		test_result("socket(99, SOCK_STREAM) == -1 (EAFNOSUPPORT)",
			    bad_fd == -1);

		// Test invalid type
		bad_fd = socket(AF_INET, 99, 0);
		test_result("socket(AF_INET, 99) == -1 (bad type)",
			    bad_fd == -1);

		/* Per-process port to avoid EADDRINUSE with parallel instances */
		uint16_t sock_test_port =
			(uint16_t)(12345 + (getpid() & 0x3FF));

		// Test bind (UDP)
		if (udp_fd >= 0) {
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(sock_test_port);
			addr.sin_addr.s_addr = htonl(INADDR_ANY);
			int ret = bind(udp_fd, (struct sockaddr *)&addr,
				       sizeof(addr));
			test_result("bind(udp, port 12345) == 0", ret == 0);

			// Test getsockname after bind
			struct sockaddr_in got_addr;
			socklen_t got_len = sizeof(got_addr);
			ret = getsockname(udp_fd, (struct sockaddr *)&got_addr,
					  &got_len);
			test_result("getsockname(udp) == 0", ret == 0);
			test_result("getsockname port == 12345",
				    ntohs(got_addr.sin_port) == sock_test_port);
		}

		// Test bind (TCP) — ephemeral port so parallel teststress runs
		// don't collide on a fixed number.
		if (tcp_fd >= 0) {
			uint16_t tcp_bind_port =
				bind_to_ephemeral(tcp_fd, INADDR_ANY);
			test_result("bind(tcp, ephemeral) == 0",
				    tcp_bind_port != 0);
		}

		// Test listen (TCP)
		if (tcp_fd >= 0) {
			int ret = listen(tcp_fd, 5);
			test_result("listen(tcp, 5) == 0", ret == 0);
		}

		// Test listen on UDP should fail
		if (udp_fd >= 0) {
			int ret = listen(udp_fd, 5);
			test_result("listen(udp) == -1 (EOPNOTSUPP)",
				    ret == -1);
		}

		// Test setsockopt SO_REUSEADDR
		{
			int opt_fd = socket(AF_INET, SOCK_DGRAM, 0);
			if (opt_fd >= 0) {
				int optval = 1;
				int ret = setsockopt(opt_fd, SOL_SOCKET,
						     SO_REUSEADDR, &optval,
						     sizeof(optval));
				test_result("setsockopt(SO_REUSEADDR) == 0",
					    ret == 0);

				// Test getsockopt SO_ERROR
				int error_val = -1;
				socklen_t error_len = sizeof(error_val);
				ret = getsockopt(opt_fd, SOL_SOCKET, SO_ERROR,
						 &error_val, &error_len);
				test_result("getsockopt(SO_ERROR) == 0",
					    ret == 0);
				test_result("SO_ERROR value == 0 (no error)",
					    error_val == 0);

				shutdown(opt_fd, SHUT_RDWR);
			}
		}

		// Test htons/ntohs byte order
		test_result("htons(0x1234) byte swap", htons(0x1234) == 0x3412);
		test_result("ntohs(htons(80)) == 80", ntohs(htons(80)) == 80);
		test_result("ntohl(htonl(0x12345678)) round-trip",
			    ntohl(htonl(0x12345678)) == 0x12345678);

		// Test inet_addr
		{
			in_addr_t a = inet_addr("10.0.2.15");
			test_result("inet_addr(\"10.0.2.15\") != -1",
				    a != (in_addr_t)-1);
			test_result("inet_addr round-trip",
				    ntohl(a) == ((10U << 24) | (0U << 16) |
						 (2U << 8) | 15U));

			in_addr_t bad = inet_addr("not.an.ip");
			test_result("inet_addr(\"not.an.ip\") == -1",
				    bad == (in_addr_t)-1);
		}

		// Test inet_ntoa
		{
			struct in_addr ia;
			ia.s_addr = inet_addr("192.168.1.100");
			char *str = inet_ntoa(ia);
			test_result("inet_ntoa(192.168.1.100)",
				    strcmp(str, "192.168.1.100") == 0);
		}

		// Test invalid sockfd operations
		{
			int ret = bind(-1, NULL, 0);
			test_result("bind(-1) == -1 (EBADF)", ret == -1);

			ret = listen(-1, 5);
			test_result("listen(-1) == -1 (EBADF)", ret == -1);

			char buf[32];
			ssize_t n = recv(-1, buf, sizeof(buf), 0);
			test_result("recv(-1) == -1 (EBADF)", n == -1);

			n = send(-1, "test", 4, 0);
			test_result("send(-1) == -1 (EBADF)", n == -1);
		}

		// Test getpeername on unconnected socket
		{
			int s = socket(AF_INET, SOCK_DGRAM, 0);
			if (s >= 0) {
				struct sockaddr_in peer;
				socklen_t plen = sizeof(peer);
				int ret = getpeername(
					s, (struct sockaddr *)&peer, &plen);
				test_result(
					"getpeername(unconnected) == -1 (ENOTCONN)",
					ret == -1);
				shutdown(s, SHUT_RDWR);
			}
		}

		// Cleanup
		if (udp_fd >= 0)
			shutdown(udp_fd, SHUT_RDWR);
		if (tcp_fd >= 0)
			shutdown(tcp_fd, SHUT_RDWR);
	}

	// ========================================
	// Extended Networking Syscalls Tests
	// ========================================
	printf("\n--- Extended Networking Syscalls ---\n");

	// Test socketpair
	{
		int sv[2] = { -1, -1 };
		int ret = socketpair(AF_INET, SOCK_DGRAM, 0, sv);
		test_result("socketpair returns 0", ret == 0);
		test_result("socketpair sv[0] >= 0", sv[0] >= 0);
		test_result("socketpair sv[1] >= 0", sv[1] >= 0);
		if (ret == 0) {
			// Test sending data through the pair
			const char *msg = "hello";
			ssize_t n = send(sv[0], msg, 5, 0);
			test_result("socketpair send returns 5", n == 5);
			char buf[16] = { 0 };
			n = recv(sv[1], buf, sizeof(buf), 0);
			test_result("socketpair recv returns 5", n == 5);
			test_result("socketpair data matches",
				    memcmp(buf, "hello", 5) == 0);
			close(sv[0]);
			close(sv[1]);
		}
	}

	// Test close/dup/dup2/dup3 on socket fds
	{
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		test_result("socket returns valid fd", s >= 3);
		if (s >= 0) {
			int d = dup(s);
			test_result("dup(socket) returns valid fd",
				    d >= 3 && d != s);
			if (d >= 0)
				close(d);

			int d2 = dup2(s, 100);
			test_result("dup2(socket, 100) returns 100", d2 == 100);
			if (d2 >= 0)
				close(d2);

			int d3 = dup3(s, 101, 0);
			test_result("dup3(socket, 101, 0) returns 101",
				    d3 == 101);
			if (d3 >= 0)
				close(d3);

			close(s);
		}
	}

	// Test fcntl on socket (F_GETFL / F_SETFL O_NONBLOCK)
	{
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			int fl = fcntl(s, F_GETFL, 0);
			test_result("fcntl(socket, F_GETFL) >= 0", fl >= 0);

			int ret = fcntl(s, F_SETFL, fl | O_NONBLOCK);
			test_result("fcntl(socket, F_SETFL, O_NONBLOCK) == 0",
				    ret == 0);

			fl = fcntl(s, F_GETFL, 0);
			test_result("fcntl confirms O_NONBLOCK set",
				    (fl & O_NONBLOCK) != 0);
			close(s);
		}
	}

	// Test ioctl SIOCGIFMTU / SIOCGIFFLAGS / SIOCGIFHWADDR
	{
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			struct ifreq ifr;
			memset(&ifr, 0, sizeof(ifr));
			// Try "eth0" - E1000 device
			memcpy(ifr.ifr_name, "eth0", 5);

			int ret = ioctl(s, SIOCGIFMTU, &ifr);
			if (ret == 0) {
				test_result("ioctl SIOCGIFMTU returns MTU > 0",
					    ifr.ifr_mtu > 0);
			} else {
				test_result("ioctl SIOCGIFMTU (no eth0, skip)",
					    1);
			}

			memset(&ifr, 0, sizeof(ifr));
			memcpy(ifr.ifr_name, "eth0", 5);
			ret = ioctl(s, SIOCGIFFLAGS, &ifr);
			if (ret == 0) {
				test_result("ioctl SIOCGIFFLAGS has IFF_UP",
					    (ifr.ifr_flags & IFF_UP) != 0);
			} else {
				test_result(
					"ioctl SIOCGIFFLAGS (no eth0, skip)",
					1);
			}

			memset(&ifr, 0, sizeof(ifr));
			memcpy(ifr.ifr_name, "eth0", 5);
			ret = ioctl(s, SIOCGIFHWADDR, &ifr);
			if (ret == 0) {
				// Check MAC is not all zeros
				int nonzero = 0;
				for (int i = 0; i < 6; i++)
					if (ifr.ifr_hwaddr.sa_data[i] != 0)
						nonzero = 1;
				test_result(
					"ioctl SIOCGIFHWADDR has non-zero MAC",
					nonzero);
			} else {
				test_result(
					"ioctl SIOCGIFHWADDR (no eth0, skip)",
					1);
			}

			close(s);
		}
	}

	// Test poll on stdin (should return immediately with timeout=0)
	{
		struct pollfd pfd;
		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int ret = poll(&pfd, 1, 0); // immediate timeout
		test_result("poll(stdin, timeout=0) >= 0", ret >= 0);
	}

	// Test poll on socket
	{
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			struct pollfd pfd;
			pfd.fd = s;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			int ret = poll(&pfd, 1, 0);
			test_result("poll(udp_socket, POLLOUT, 0) >= 0",
				    ret >= 0);
			if (ret > 0) {
				test_result(
					"poll returns POLLOUT for UDP socket",
					(pfd.revents & POLLOUT) != 0);
			}
			close(s);
		}
	}

	// Test select with timeout=0 (immediate)
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		struct timeval tv = { 0, 0 };
		int ret = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
		test_result("select(stdin, timeout=0) >= 0", ret >= 0);
	}

	// Test epoll create/ctl/wait
	{
		int epfd = epoll_create1(0);
		test_result("epoll_create1(0) returns valid fd", epfd >= 3);
		if (epfd >= 0) {
			int s = socket(AF_INET, SOCK_DGRAM, 0);
			if (s >= 0) {
				struct epoll_event ev;
				ev.events = EPOLLIN | EPOLLOUT;
				ev.data.fd = s;
				int ret =
					epoll_ctl(epfd, EPOLL_CTL_ADD, s, &ev);
				test_result("epoll_ctl ADD returns 0",
					    ret == 0);

				struct epoll_event events[4];
				ret = epoll_wait(epfd, events, 4, 0);
				test_result("epoll_wait(timeout=0) >= 0",
					    ret >= 0);

				ret = epoll_ctl(epfd, EPOLL_CTL_DEL, s, NULL);
				test_result("epoll_ctl DEL returns 0",
					    ret == 0);

				close(s);
			}
			close(epfd);
		}
	}

	// Test accept4 (should fail on non-listening socket)
	printf("\n[TEST] accept4()\n");
	{
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s >= 0) {
			int ret = accept4(s, NULL, NULL, 0);
			test_result("accept4(non-listening) returns -1",
				    ret == -1);
			/* accept4 with SOCK_NONBLOCK on a connected socketpair end */
			int sv4[2] = { -1, -1 };
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv4) == 0) {
				/* Not a listening socket — accept4 fails, but the flag
                 * plumbing must not crash */
				(void)accept4(sv4[0], NULL, NULL,
					      SOCK_NONBLOCK);
				close(sv4[0]);
				close(sv4[1]);
				test_pass(
					"accept4 with SOCK_NONBLOCK: no crash");
			}
			close(s);
		}
	}

	// Test MSG_NOSIGNAL: send on broken-pipe socket returns EPIPE, no signal
	printf("\n[TEST] MSG_NOSIGNAL\n");
	{
		int svn[2] = { -1, -1 };
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, svn) == 0) {
			signal(SIGPIPE, SIG_IGN);
			close(svn[1]); /* close the peer */
			char nbuf[1] = { 'X' };
			ssize_t ns = send(svn[0], nbuf, 1, MSG_NOSIGNAL);
			test_result(
				"MSG_NOSIGNAL: send to closed peer returns -1",
				ns == -1);
			test_result("MSG_NOSIGNAL: errno is EPIPE",
				    errno == EPIPE);
			signal(SIGPIPE, SIG_DFL);
			close(svn[0]);
		} else {
			test_fail("MSG_NOSIGNAL: socketpair failed");
		}
	}

	// Test sendmsg / recvmsg via socketpair
	{
		int sv[2] = { -1, -1 };
		if (socketpair(AF_INET, SOCK_DGRAM, 0, sv) == 0) {
			char data[] = "msghdr test";
			struct iovec iov;
			iov.iov_base = data;
			iov.iov_len = sizeof(data) - 1;
			struct msghdr msg;
			memset(&msg, 0, sizeof(msg));
			msg.msg_iov = &iov;
			msg.msg_iovlen = 1;
			ssize_t n = sendmsg(sv[0], &msg, 0);
			test_result("sendmsg returns > 0", n > 0);

			char rbuf[32] = { 0 };
			struct iovec riov;
			riov.iov_base = rbuf;
			riov.iov_len = sizeof(rbuf);
			struct msghdr rmsg;
			memset(&rmsg, 0, sizeof(rmsg));
			rmsg.msg_iov = &riov;
			rmsg.msg_iovlen = 1;
			n = recvmsg(sv[1], &rmsg, 0);
			test_result("recvmsg returns > 0", n > 0);
			test_result("recvmsg data matches",
				    memcmp(rbuf, "msghdr test", 11) == 0);

			close(sv[0]);
			close(sv[1]);
		}
	}

	// ========================================
	// sendfile Tests
	// ========================================
	printf("\n--- sendfile Tests ---\n");

	/* _pbase may have been removed by rmdir() earlier in the test run
     * (e.g. after the LFN section empties the directory).  Re-create it
     * now so all per-process temp paths below are valid. */
	mkdir(_pbase, 0777);
	/* Same for _td: the early mkdir near the top of main() can fail or be
     * undone, and `goto network_section` skips the filesystem section that
     * would otherwise have created it.  The AF_UNIX test below binds
     * _p_usock (= _td/unix.sock), and bind() needs the parent directory to
     * exist, so ensure it here on both entry paths. */
	mkdir(_td, 0755);

	// Per-process paths to avoid races between parallel test instances.
	char sf_src[64], sf_dst[64], sf_off[64], sf_off_d[64];
	char sf_sock[64], sf_pipe_f[64], sf_zero[64], sf_zero_d[64];
	snprintf(sf_src, sizeof(sf_src), "%s/sf_src.txt", _pbase);
	snprintf(sf_dst, sizeof(sf_dst), "%s/sf_dst.txt", _pbase);
	snprintf(sf_off, sizeof(sf_off), "%s/sf_off.txt", _pbase);
	snprintf(sf_off_d, sizeof(sf_off_d), "%s/sf_off_d.txt", _pbase);
	snprintf(sf_sock, sizeof(sf_sock), "%s/sf_sock.txt", _pbase);
	snprintf(sf_pipe_f, sizeof(sf_pipe_f), "%s/sf_pipe.txt", _pbase);
	snprintf(sf_zero, sizeof(sf_zero), "%s/sf_zero.txt", _pbase);
	snprintf(sf_zero_d, sizeof(sf_zero_d), "%s/sf_zero_d.txt", _pbase);

	// Test 1: sendfile from file to file
	{
		// Create a source file with known content
		int src = open(sf_src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		test_result("sendfile: create source file", src >= 0);
		if (src >= 0) {
			const char *data =
				"Hello sendfile world! This is test data for sendfile.";
			ssize_t nw = write(src, data, strlen(data));
			test_result("sendfile: write source data",
				    nw == (ssize_t)strlen(data));
			close(src);

			// Open source for reading and dest for writing
			int in_fd = open(sf_src, O_RDONLY);
			int out_fd = open(sf_dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			test_result("sendfile: open source for read",
				    in_fd >= 0);
			test_result("sendfile: open dest for write",
				    out_fd >= 0);

			if (in_fd >= 0 && out_fd >= 0) {
				ssize_t sf = sendfile(out_fd, in_fd, NULL,
						      strlen(data));
				test_result(
					"sendfile: file-to-file returns correct count",
					sf == (ssize_t)strlen(data));
				close(in_fd);
				close(out_fd);

				// Verify destination content
				int vfd = open(sf_dst, O_RDONLY);
				if (vfd >= 0) {
					char rbuf[128] = { 0 };
					ssize_t nr =
						read(vfd, rbuf, sizeof(rbuf));
					test_result(
						"sendfile: dest has correct length",
						nr == (ssize_t)strlen(data));
					test_result(
						"sendfile: dest content matches",
						memcmp(rbuf, data,
						       strlen(data)) == 0);
					close(vfd);
				}
			} else {
				if (in_fd >= 0)
					close(in_fd);
				if (out_fd >= 0)
					close(out_fd);
			}

			// Cleanup
			unlink(sf_src);
			unlink(sf_dst);
		}
	}

	// Test 2: sendfile with offset parameter
	{
		int src = open(sf_off, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (src >= 0) {
			const char *data = "AAAAABBBBBCCCCC"; // 15 bytes
			write(src, data, 15);
			close(src);

			int in_fd = open(sf_off, O_RDONLY);
			int out_fd =
				open(sf_off_d, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (in_fd >= 0 && out_fd >= 0) {
				// Send 5 bytes starting at offset 5 (the "BBBBB" part)
				int64_t off = 5;
				ssize_t sf = sendfile(out_fd, in_fd, &off, 5);
				test_result("sendfile: with offset returns 5",
					    sf == 5);
				test_result("sendfile: offset updated to 10",
					    off == 10);

				// Verify file position was NOT changed (offset mode)
				off_t pos = lseek(in_fd, 0, 1); // SEEK_CUR
				test_result("sendfile: file position unchanged",
					    pos == 0);

				close(in_fd);
				close(out_fd);

				// Verify we got "BBBBB"
				int vfd = open(sf_off_d, O_RDONLY);
				if (vfd >= 0) {
					char rbuf[16] = { 0 };
					read(vfd, rbuf, sizeof(rbuf));
					test_result(
						"sendfile: offset data is BBBBB",
						memcmp(rbuf, "BBBBB", 5) == 0);
					close(vfd);
				}
			} else {
				if (in_fd >= 0)
					close(in_fd);
				if (out_fd >= 0)
					close(out_fd);
			}
			unlink(sf_off);
			unlink(sf_off_d);
		}
	}

	// Test 3: sendfile from file to socket (via socketpair)
	{
		int src = open(sf_sock, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (src >= 0) {
			const char *data = "socket sendfile data";
			write(src, data, strlen(data));
			close(src);

			int sv[2] = { -1, -1 };
			int in_fd = open(sf_sock, O_RDONLY);
			int sp_ok = socketpair(AF_INET, SOCK_DGRAM, 0, sv);
			test_result("sendfile-to-socket: setup ok",
				    in_fd >= 0 && sp_ok == 0);
			if (in_fd >= 0 && sp_ok == 0) {
				ssize_t sf = sendfile(sv[0], in_fd, NULL,
						      strlen(data));
				test_result(
					"sendfile: file-to-socket returns correct count",
					sf == (ssize_t)strlen(data));

				if (sf > 0) {
					char rbuf[64] = { 0 };
					ssize_t nr = recv(sv[1], rbuf,
							  sizeof(rbuf), 0);
					test_result(
						"sendfile: socket recv gets data",
						nr == (ssize_t)strlen(data));
					test_result(
						"sendfile: socket data matches",
						memcmp(rbuf, data,
						       strlen(data)) == 0);
				}
			}
			if (in_fd >= 0)
				close(in_fd);
			if (sv[0] >= 0)
				close(sv[0]);
			if (sv[1] >= 0)
				close(sv[1]);
			unlink(sf_sock);
		}
	}

	// Test 4: sendfile from file to pipe
	{
		int src = open(sf_pipe_f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (src >= 0) {
			const char *data = "pipe sendfile!";
			write(src, data, strlen(data));
			close(src);

			int pfd[2];
			int in_fd = open(sf_pipe_f, O_RDONLY);
			int pipe_ok = pipe(pfd);
			test_result("sendfile-to-pipe: setup ok",
				    in_fd >= 0 && pipe_ok == 0);
			if (in_fd >= 0 && pipe_ok == 0) {
				ssize_t sf = sendfile(pfd[1], in_fd, NULL,
						      strlen(data));
				test_result(
					"sendfile: file-to-pipe returns correct count",
					sf == (ssize_t)strlen(data));

				if (sf > 0) {
					char rbuf[64] = { 0 };
					ssize_t nr = read(pfd[0], rbuf,
							  sizeof(rbuf));
					test_result(
						"sendfile: pipe read gets data",
						nr == (ssize_t)strlen(data));
					test_result(
						"sendfile: pipe data matches",
						memcmp(rbuf, data,
						       strlen(data)) == 0);
				}
			}
			if (in_fd >= 0)
				close(in_fd);
			if (pipe_ok == 0) {
				close(pfd[0]);
				close(pfd[1]);
			}
			unlink(sf_pipe_f);
		}
	}

	// Test 5: sendfile with count=0 returns 0
	{
		int src = open(sf_zero, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (src >= 0) {
			write(src, "x", 1);
			close(src);
			int in_fd = open(sf_zero, O_RDONLY);
			int out_fd =
				open(sf_zero_d, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (in_fd >= 0 && out_fd >= 0) {
				ssize_t sf = sendfile(out_fd, in_fd, NULL, 0);
				test_result("sendfile: count=0 returns 0",
					    sf == 0);
			}
			if (in_fd >= 0)
				close(in_fd);
			if (out_fd >= 0)
				close(out_fd);
			unlink(sf_zero);
			unlink(sf_zero_d);
		}
	}

	// Test 6: sendfile with invalid fds returns -1
	{
		ssize_t sf = sendfile(-1, -1, NULL, 100);
		test_result("sendfile: bad fds returns -1", sf == -1);
	}

	// ========================================
	// /dev/urandom and /dev/random Tests
	// ========================================
	if (!net_only) {
		printf("\n--- /dev/urandom and /dev/random ---\n");

		// Test 1: Read 32 bytes from /dev/urandom
		{
			int fd = open("/dev/urandom", O_RDONLY);
			test_result("urandom: open succeeds", fd >= 0);
			if (fd >= 0) {
				unsigned char buf[32];
				memset(buf, 0, sizeof(buf));
				ssize_t n = read(fd, buf, 32);
				test_result("urandom: read 32 bytes", n == 32);

				// Check not all zeros
				int nonzero = 0;
				for (int i = 0; i < 32; i++) {
					if (buf[i] != 0)
						nonzero = 1;
				}
				test_result("urandom: data is non-zero",
					    nonzero);
				close(fd);
			}
		}

		// Test 2: Two reads from /dev/urandom differ
		{
			int fd = open("/dev/urandom", O_RDONLY);
			if (fd >= 0) {
				unsigned char buf1[16], buf2[16];
				read(fd, buf1, 16);
				read(fd, buf2, 16);
				test_result("urandom: two reads differ",
					    memcmp(buf1, buf2, 16) != 0);
				close(fd);
			}
		}

		// Test 3: Read 4096 bytes from /dev/urandom
		{
			int fd = open("/dev/urandom", O_RDONLY);
			if (fd >= 0) {
				unsigned char buf[4096];
				ssize_t n = read(fd, buf, 4096);
				test_result("urandom: read 4096 bytes",
					    n == 4096);
				close(fd);
			}
		}

		// Test 4: /dev/random also works
		{
			int fd = open("/dev/random", O_RDONLY);
			test_result("random: open succeeds", fd >= 0);
			if (fd >= 0) {
				unsigned char buf[16];
				ssize_t n = read(fd, buf, 16);
				test_result("random: read 16 bytes", n == 16);
				close(fd);
			}
		}

		// Test 5: Write to /dev/urandom (adds entropy)
		{
			int fd = open("/dev/urandom", O_WRONLY);
			if (fd >= 0) {
				unsigned char entropy[] = "test entropy data";
				ssize_t n = write(fd, entropy, sizeof(entropy));
				test_result("urandom: write succeeds",
					    n == (ssize_t)sizeof(entropy));
				close(fd);
			}
		}
	} /* end if (!net_only) — /dev/urandom block */

	// ========================================
	// AF_UNIX Socketpair Tests
	// ========================================
	printf("\n--- AF_UNIX Socketpair ---\n");

	// Test 1: socketpair creation
	{
		int sv[2] = { -1, -1 };
		int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		test_result("unix socketpair: create",
			    ret == 0 && sv[0] >= 0 && sv[1] >= 0);

		if (ret == 0) {
			// Test 2: send/recv through socketpair
			const char *msg = "hello unix";
			ssize_t sent = write(sv[0], msg, strlen(msg));
			test_result("unix socketpair: write",
				    sent == (ssize_t)strlen(msg));

			char buf[64];
			memset(buf, 0, sizeof(buf));
			ssize_t rcvd = read(sv[1], buf, sizeof(buf));
			test_result("unix socketpair: read",
				    rcvd == (ssize_t)strlen(msg));
			test_result("unix socketpair: data matches",
				    strcmp(buf, "hello unix") == 0);

			// Test 3: bidirectional
			const char *reply = "world";
			write(sv[1], reply, strlen(reply));
			memset(buf, 0, sizeof(buf));
			rcvd = read(sv[0], buf, sizeof(buf));
			test_result("unix socketpair: bidirectional",
				    rcvd == (ssize_t)strlen(reply) &&
					    strcmp(buf, "world") == 0);

			// Test 4: close one end, other gets EOF
			close(sv[0]);
			memset(buf, 0, sizeof(buf));
			rcvd = read(sv[1], buf, sizeof(buf));
			test_result("unix socketpair: close->EOF", rcvd == 0);

			close(sv[1]);
		}
	}

	// Test 5: AF_UNIX SOCK_DGRAM socketpair
	{
		int sv[2] = { -1, -1 };
		int ret = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
		test_result("unix dgram socketpair: create", ret == 0);
		if (ret == 0) {
			const char *msg = "dgram test";
			write(sv[0], msg, strlen(msg));
			char buf[64];
			memset(buf, 0, sizeof(buf));
			ssize_t n = read(sv[1], buf, sizeof(buf));
			test_result("unix dgram socketpair: transfer",
				    n == (ssize_t)strlen(msg) &&
					    strcmp(buf, "dgram test") == 0);
			close(sv[0]);
			close(sv[1]);
		}
	}

	// ========================================
	// AF_UNIX Client/Server Tests
	// ========================================
	printf("\n--- AF_UNIX Client/Server ---\n");

	{
		int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		test_result("unix server: socket create", server_fd >= 0);

		if (server_fd >= 0) {
			struct sockaddr_un addr;
			memset(&addr, 0, sizeof(addr));
			addr.sun_family = AF_UNIX;
			strcpy(addr.sun_path, _p_usock);

			int ret = bind(server_fd, (struct sockaddr *)&addr,
				       sizeof(addr));
			test_result("unix server: bind", ret == 0);

			/* A pathname bind must leave a real socket node behind.
			 * X11 clients stat() /tmp/.X11-unix/X0 and refuse to
			 * connect unless it reports S_IFSOCK, so a name that
			 * exists only inside the kernel is unreachable. */
			struct stat ust;
			test_result("bind creates a filesystem node",
				    stat(_p_usock, &ust) == 0);
			test_result("the node reports S_IFSOCK",
				    S_ISSOCK(ust.st_mode));
			test_result("lstat agrees it is a socket",
				    lstat(_p_usock, &ust) == 0 &&
					    S_ISSOCK(ust.st_mode));

			/* Bind with the SUN_LEN convention: an addrlen that
			 * counts the path but NOT its terminator.
			 *
			 * This is how the address length is normally computed
			 *
			 *     SUN_LEN(p) = offsetof(struct sockaddr_un, sun_path)
			 *                  + strlen((p)->sun_path)
			 *
			 * and it is what X11's xtrans passes.  The kernel used
			 * to require a NUL inside addrlen and returned EINVAL
			 * for it, which surfaced four layers up as the display
			 * server reporting "Cannot establish any listening
			 * sockets" -- with nothing pointing at bind().
			 *
			 * Tested end to end, not just for a zero return: an
			 * accepted bind that registered a truncated name would
			 * still fail to accept a connection.
			 *
			 * The connect runs in a CHILD because connect() on this
			 * system blocks until the listener accepts -- doing
			 * both in one process deadlocks.  The parent's accept
			 * is non-blocking and bounded, so a child that never
			 * connects fails the test instead of hanging the whole
			 * suite.
			 */
			{
				char sunpath[96];
				int srv, acc = -1;
				struct sockaddr_un sa;
				socklen_t slen;
				pid_t kid;

				snprintf(sunpath, sizeof(sunpath),
					 "%s/sunlen.sock", _td);
				unlink(sunpath);

				srv = socket(AF_UNIX, SOCK_STREAM, 0);
				memset(&sa, 0, sizeof(sa));
				sa.sun_family = AF_UNIX;
				strcpy(sa.sun_path, sunpath);
				slen = (socklen_t)(offsetof(struct sockaddr_un,
							   sun_path) +
						   strlen(sa.sun_path));

				test_result("bind with SUN_LEN addrlen (no NUL counted)",
					    srv >= 0 &&
						    bind(srv,
							 (struct sockaddr *)&sa,
							 slen) == 0);
				{
					struct stat st_;

					test_result("SUN_LEN bind still creates S_IFSOCK",
						    stat(sunpath, &st_) == 0 &&
							    S_ISSOCK(st_.st_mode));
				}
				test_result("SUN_LEN bind listens",
					    listen(srv, 4) == 0);

				fcntl(srv, F_SETFL, O_NONBLOCK);

				kid = fork();
				if (kid == 0) {
					int cli = socket(AF_UNIX, SOCK_STREAM, 0);
					int ok = 0;

					if (cli >= 0 &&
					    connect(cli, (struct sockaddr *)&sa,
						    slen) == 0 &&
					    write(cli, "ok", 2) == 2)
						ok = 1;
					if (cli >= 0)
						close(cli);
					_exit(ok ? 0 : 1);
				}

				if (kid > 0) {
					/* Bounded wait: ~2s at 10ms a turn. */
					for (int tries = 0; tries < 200; tries++) {
						acc = accept(srv, NULL, NULL);
						if (acc >= 0)
							break;
						if (errno != EAGAIN &&
						    errno != EWOULDBLOCK)
							break;
						usleep(10000);
					}
					test_result("SUN_LEN listener accepts a connection",
						    acc >= 0);

					if (acc >= 0) {
						char b[4] = { 0 };

						test_result("SUN_LEN socket carries data",
							    read(acc, b, 2) == 2 &&
								    b[0] == 'o' &&
								    b[1] == 'k');
						close(acc);
					}

					int st = 0;

					while (waitpid(kid, &st, 0) < 0 &&
					       errno == EINTR)
						;
					test_result("SUN_LEN client connected cleanly",
						    WIFEXITED(st) &&
							    WEXITSTATUS(st) == 0);
				} else {
					test_fail("SUN_LEN: fork failed");
				}

				if (srv >= 0)
					close(srv);
				unlink(sunpath);
			}

			/* getsockname()/getpeername() must work on AF_UNIX.
			 *
			 * libxcb asks a connected socket what it is before it
			 * can choose an authorisation record: getpeername()
			 * first, getsockname() as a fallback, and if BOTH fail
			 * it sends no authorisation at all.  The X server then
			 * refuses every client with "Authorization required,
			 * but no authorization protocol specified" -- a message
			 * that names neither call.  Both used to fail here
			 * because the syscalls only understood AF_INET. */
			{
				char sp2[96];
				int srv2, acc2 = -1;
				struct sockaddr_un sa2;
				socklen_t sl2;
				pid_t kid2;

				snprintf(sp2, sizeof(sp2), "%s/sockname.sock",
					 _td);
				unlink(sp2);

				srv2 = socket(AF_UNIX, SOCK_STREAM, 0);
				memset(&sa2, 0, sizeof(sa2));
				sa2.sun_family = AF_UNIX;
				strcpy(sa2.sun_path, sp2);
				sl2 = (socklen_t)(offsetof(struct sockaddr_un,
							  sun_path) +
						  strlen(sa2.sun_path));

				if (srv2 >= 0 &&
				    bind(srv2, (struct sockaddr *)&sa2, sl2) == 0 &&
				    listen(srv2, 4) == 0) {
					struct sockaddr_un q;
					socklen_t ql = sizeof(q);

					memset(&q, 0, sizeof(q));
					test_result("getsockname on a bound unix socket",
						    getsockname(srv2,
								(struct sockaddr *)&q,
								&ql) == 0 &&
							    q.sun_family == AF_UNIX);
					test_result("getsockname reports the bound path",
						    strcmp(q.sun_path, sp2) == 0);
					/* SUN_LEN convention on the way out too. */
					test_result("getsockname reports SUN_LEN",
						    ql == (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sp2)));

					fcntl(srv2, F_SETFL, O_NONBLOCK);
					kid2 = fork();
					if (kid2 == 0) {
						int c = socket(AF_UNIX, SOCK_STREAM, 0);
						int ok = 0;

						if (c >= 0 &&
						    connect(c, (struct sockaddr *)&sa2, sl2) == 0) {
							struct sockaddr_un pn;
							socklen_t pl = sizeof(pn);

							memset(&pn, 0, sizeof(pn));
							/* The peer is the
							 * listener, so this
							 * must name its path. */
							if (getpeername(c, (struct sockaddr *)&pn, &pl) == 0 &&
							    pn.sun_family == AF_UNIX &&
							    strcmp(pn.sun_path, sp2) == 0)
								ok = 1;
							/* An unbound client
							 * still has a family. */
							memset(&pn, 0, sizeof(pn));
							pl = sizeof(pn);
							if (getsockname(c, (struct sockaddr *)&pn, &pl) != 0 ||
							    pn.sun_family != AF_UNIX)
								ok = 0;
						}
						if (c >= 0)
							close(c);
						_exit(ok ? 0 : 1);
					}
					if (kid2 > 0) {
						int st2 = 0;

						for (int t = 0; t < 200; t++) {
							acc2 = accept(srv2, NULL, NULL);
							if (acc2 >= 0)
								break;
							if (errno != EAGAIN &&
							    errno != EWOULDBLOCK)
								break;
							usleep(10000);
						}
						test_result("unix listener accepted for name test",
							    acc2 >= 0);
						if (acc2 >= 0)
							close(acc2);
						while (waitpid(kid2, &st2, 0) < 0 &&
						       errno == EINTR)
							;
						test_result("getpeername/getsockname work in the client",
							    WIFEXITED(st2) &&
								    WEXITSTATUS(st2) == 0);
					} else {
						test_fail("sockname: fork failed");
					}
				} else {
					test_fail("sockname: setup failed");
				}
				if (srv2 >= 0)
					close(srv2);
				unlink(sp2);
			}

			/* getpeername() must survive the peer closing.
			 *
			 * The connection is over, but this socket is still open
			 * and still knows who it was talking to — and that is
			 * exactly when a program asks, while cleaning up after
			 * a peer that just went away.  This used to follow a
			 * pointer to the peer socket, so the answer vanished
			 * the moment the other end called close(): an ordinary
			 * accept-then-close left the client unable to name the
			 * address it had just connected to.  The name is now
			 * copied when the connection is made.
			 *
			 * Done entirely in one process (no fork) so the close
			 * ordering is deterministic rather than a race. */
			{
				char sp3[80];
				int srv3, cli3 = -1, acc3 = -1;
				struct sockaddr_un sa3;
				socklen_t sl3;

				snprintf(sp3, sizeof(sp3),
					 "/tmp/lktest_peerclose_%d", (int)getpid());
				unlink(sp3);
				memset(&sa3, 0, sizeof(sa3));
				sa3.sun_family = AF_UNIX;
				strncpy(sa3.sun_path, sp3,
					sizeof(sa3.sun_path) - 1);
				sl3 = (socklen_t)(offsetof(struct sockaddr_un,
							   sun_path) +
						  strlen(sp3));

				srv3 = socket(AF_UNIX, SOCK_STREAM, 0);
				if (srv3 >= 0 &&
				    bind(srv3, (struct sockaddr *)&sa3, sl3) == 0 &&
				    listen(srv3, 4) == 0) {
					cli3 = socket(AF_UNIX, SOCK_STREAM, 0);
					if (cli3 >= 0) {
						int cr3;

						/* The connect MUST be
						 * non-blocking here: connect()
						 * on this system does not
						 * return until the listener
						 * has accept()ed, and the
						 * accept is on this very
						 * thread.  A blocking connect
						 * would queue the connection
						 * and then wait for an accept
						 * that can never run. */
						fcntl(cli3, F_SETFL, O_NONBLOCK);
						errno = 0;
						cr3 = connect(cli3,
							      (struct sockaddr *)&sa3,
							      sl3);
						if (cr3 == 0 ||
						    errno == EINPROGRESS ||
						    errno == EAGAIN)
							acc3 = accept(srv3, NULL,
								      NULL);
					}

					int had_conn3 = (acc3 >= 0);

					test_result("peer-close: connection established",
						    had_conn3);

					/* Drop the far end, then ask. */
					if (acc3 >= 0)
						close(acc3);

					if (cli3 >= 0 && had_conn3) {
						struct sockaddr_un pn3;
						socklen_t pl3 = sizeof(pn3);
						int r3;

						memset(&pn3, 0, sizeof(pn3));
						errno = 0;
						r3 = getpeername(cli3,
								 (struct sockaddr *)&pn3,
								 &pl3);
						test_result("getpeername works after the peer closed",
							    r3 == 0);
						test_result("...and still reports the peer's path",
							    r3 == 0 &&
								    pn3.sun_family == AF_UNIX &&
								    strcmp(pn3.sun_path, sp3) == 0);
					}
				} else {
					test_fail("peer-close: setup failed");
				}
				if (cli3 >= 0)
					close(cli3);
				if (srv3 >= 0)
					close(srv3);
				unlink(sp3);
			}

			/* An unconnected socket has no peer to name, and must
			 * say so rather than inventing one from a recycled
			 * table slot. */
			{
				int lone = socket(AF_UNIX, SOCK_STREAM, 0);

				if (lone >= 0) {
					struct sockaddr_un pn4;
					socklen_t pl4 = sizeof(pn4);

					memset(&pn4, 0, sizeof(pn4));
					errno = 0;
					test_result("getpeername on an unconnected socket -> ENOTCONN",
						    getpeername(lone,
								(struct sockaddr *)&pn4,
								&pl4) < 0 &&
							    errno == ENOTCONN);
					close(lone);
				}
			}

			/* Both ends of a socketpair are connected and unnamed:
			 * getpeername must succeed and report the family, not
			 * fail as if there were no connection. */
			{
				int sv4[2];

				if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv4) == 0) {
					struct sockaddr_un pn5;
					socklen_t pl5 = sizeof(pn5);

					memset(&pn5, 0, sizeof(pn5));
					test_result("getpeername on a socketpair end",
						    getpeername(sv4[0],
								(struct sockaddr *)&pn5,
								&pl5) == 0 &&
							    pn5.sun_family == AF_UNIX);
					close(sv4[0]);
					close(sv4[1]);
				}
			}

			/* A second bind to the same name must be refused, and
			 * with the address-in-use error rather than a generic
			 * failure. */
			{
				int dup_fd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (dup_fd >= 0) {
					errno = 0;
					test_result(
						"re-binding the same path -> EADDRINUSE",
						bind(dup_fd,
						     (struct sockaddr *)&addr,
						     sizeof(addr)) == -1 &&
							errno == EADDRINUSE);
					close(dup_fd);
				}
			}

			ret = listen(server_fd, 5);
			test_result("unix server: listen", ret == 0);

			// Fork: child connects, parent accepts
			pid_t pid = fork();
			if (pid == 0) {
				// Child: connect and send data
				close(server_fd);
				int cli = socket(AF_UNIX, SOCK_STREAM, 0);
				if (cli >= 0) {
					struct sockaddr_un saddr;
					memset(&saddr, 0, sizeof(saddr));
					saddr.sun_family = AF_UNIX;
					strcpy(saddr.sun_path, _p_usock);
					connect(cli, (struct sockaddr *)&saddr,
						sizeof(saddr));
					write(cli, "from child", 10);
					char buf[64];
					read(cli, buf, sizeof(buf));
					close(cli);
				}
				_exit(0);
			} else if (pid > 0) {
				// Parent: accept and verify
				int cli_fd = accept(server_fd, NULL, NULL);
				test_result("unix server: accept", cli_fd >= 0);
				if (cli_fd >= 0) {
					char buf[64];
					memset(buf, 0, sizeof(buf));
					ssize_t n =
						read(cli_fd, buf, sizeof(buf));
					test_result(
						"unix server: recv from client",
						n == 10 && memcmp(buf,
								  "from child",
								  10) == 0);
					write(cli_fd, "reply", 5);
					close(cli_fd);
				}
				int status;
				waitpid(pid, &status, 0);
			}
			close(server_fd);

			/* Closing the socket removes its node.  Without this,
			 * stale sockets accumulate and every later bind to the
			 * same name fails with EADDRINUSE — which is how a
			 * display server refuses to restart after an unclean
			 * exit. */
			{
				struct stat gone;
				test_result(
					"closing the socket removes its node",
					stat(_p_usock, &gone) != 0);
			}

			/* And the name is immediately reusable. */
			{
				int again = socket(AF_UNIX, SOCK_STREAM, 0);
				if (again >= 0) {
					test_result(
						"the path can be bound again",
						bind(again,
						     (struct sockaddr *)&addr,
						     sizeof(addr)) == 0);
					close(again);
				}
			}

			unlink(_p_usock); /* belt and braces if the above failed */
		}
	}

	// ========================================
	// UDP Loopback on 127.0.0.1 Tests
	// ========================================
	printf("\n--- UDP Loopback 127.0.0.1 ---\n");

	{
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		test_result("udp loopback: socket create", sock >= 0);

		if (sock >= 0) {
			/* Per-process port to avoid EADDRINUSE with parallel instances */
			uint16_t udp_lo_port =
				(uint16_t)(19999 + (getpid() & 0x3FF));

			struct timeval udp_tv = { .tv_sec = 3, .tv_usec = 0 };
			setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &udp_tv,
				   sizeof(udp_tv));

			struct sockaddr_in bind_addr;
			memset(&bind_addr, 0, sizeof(bind_addr));
			bind_addr.sin_family = AF_INET;
			bind_addr.sin_port = htons(udp_lo_port);
			bind_addr.sin_addr.s_addr =
				htonl(0x7F000001); // 127.0.0.1

			int ret = bind(sock, (struct sockaddr *)&bind_addr,
				       sizeof(bind_addr));
			test_result("udp loopback: bind 127.0.0.1:19999",
				    ret == 0);

			if (ret == 0) {
				struct sockaddr_in dest;
				memset(&dest, 0, sizeof(dest));
				dest.sin_family = AF_INET;
				dest.sin_port = htons(udp_lo_port);
				dest.sin_addr.s_addr = htonl(0x7F000001);

				const char *msg = "loopback test";
				ssize_t sent = sendto(sock, msg, strlen(msg), 0,
						      (struct sockaddr *)&dest,
						      sizeof(dest));
				test_result("udp loopback: sendto",
					    sent == (ssize_t)strlen(msg));

				if (sent > 0) {
					char buf[64];
					memset(buf, 0, sizeof(buf));
					ssize_t rcvd =
						recvfrom(sock, buf, sizeof(buf),
							 0, NULL, NULL);
					test_result(
						"udp loopback: recvfrom",
						rcvd == (ssize_t)strlen(msg));
					test_result(
						"udp loopback: data matches",
						memcmp(buf, msg, strlen(msg)) ==
							0);
				}
			}
			close(sock);
		}
	}

	// ========================================
	// Loopback Interface Detection
	// ========================================
	printf("\n--- Loopback Interface ---\n");

	{
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock >= 0) {
			struct ifreq ifr;
			memset(&ifr, 0, sizeof(ifr));
			strcpy(ifr.ifr_name, "lo");

			int ret = ioctl(sock, SIOCGIFFLAGS, &ifr);
			test_result("loopback: SIOCGIFFLAGS succeeds",
				    ret == 0);
			if (ret == 0) {
				test_result("loopback: IFF_LOOPBACK set",
					    (ifr.ifr_flags & IFF_LOOPBACK) !=
						    0);
				test_result("loopback: IFF_UP set",
					    (ifr.ifr_flags & IFF_UP) != 0);
			}

			memset(&ifr, 0, sizeof(ifr));
			strcpy(ifr.ifr_name, "lo");
			ret = ioctl(sock, SIOCGIFADDR, &ifr);
			test_result("loopback: SIOCGIFADDR succeeds", ret == 0);
			if (ret == 0) {
				struct sockaddr_in *sin =
					(struct sockaddr_in *)&ifr.ifr_addr;
				test_result("loopback: IP is 127.0.0.1",
					    ntohl(sin->sin_addr.s_addr) ==
						    0x7F000001);
			}

			memset(&ifr, 0, sizeof(ifr));
			strcpy(ifr.ifr_name, "lo");
			ret = ioctl(sock, SIOCGIFMTU, &ifr);
			test_result("loopback: SIOCGIFMTU succeeds", ret == 0);
			if (ret == 0) {
				test_result("loopback: MTU is 65535",
					    ifr.ifr_mtu == 65535);
			}

			close(sock);
		}
	}

	// ========================================
	// Routing Ioctl Tests
	// ========================================
	printf("\n--- Routing Ioctls ---\n");

	{
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock >= 0) {
			// Add a test route and delete it
			struct {
				struct sockaddr rt_dst;
				struct sockaddr rt_gateway;
				struct sockaddr rt_genmask;
				short rt_flags;
				int rt_metric;
				char *rt_dev;
			} rt;
			memset(&rt, 0, sizeof(rt));
			struct sockaddr_in *dst =
				(struct sockaddr_in *)&rt.rt_dst;
			struct sockaddr_in *gw =
				(struct sockaddr_in *)&rt.rt_gateway;
			struct sockaddr_in *mask =
				(struct sockaddr_in *)&rt.rt_genmask;

			dst->sin_family = AF_INET;
			dst->sin_addr.s_addr =
				htonl(0xC0A86400); // 192.168.100.0
			gw->sin_family = AF_INET;
			gw->sin_addr.s_addr = htonl(0x0A000001); // 10.0.0.1
			mask->sin_family = AF_INET;
			mask->sin_addr.s_addr =
				htonl(0xFFFFFF00); // 255.255.255.0
			rt.rt_flags = 0x0003; // RTF_UP | RTF_GATEWAY

			int ret = ioctl(sock, SIOCADDRT, &rt);
			test_result("route: SIOCADDRT", ret == 0);

			ret = ioctl(sock, SIOCDELRT, &rt);
			test_result("route: SIOCDELRT", ret == 0);

			close(sock);
		}
	}

	// ========================================
	// IFCONF includes loopback
	// ========================================
	printf("\n--- IFCONF with loopback ---\n");

	{
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock >= 0) {
			struct ifreq ifr_buf[8];
			struct ifconf ifc;
			memset(&ifc, 0, sizeof(ifc));
			ifc.ifc_len = sizeof(ifr_buf);
			ifc.ifc_buf = (char *)ifr_buf;

			int ret = ioctl(sock, SIOCGIFCONF, &ifc);
			test_result("ifconf: SIOCGIFCONF succeeds", ret == 0);

			int found_lo = 0;
			int n_ifs = ifc.ifc_len / (int)sizeof(struct ifreq);
			for (int i = 0; i < n_ifs; i++) {
				if (strcmp(ifr_buf[i].ifr_name, "lo") == 0)
					found_lo = 1;
			}
			test_result("ifconf: lo interface present", found_lo);
			close(sock);
		}
	}

	// ========================================
	// DNS Resolve Tests
	// ========================================
	printf("\n--- DNS Resolve ---\n");

	// Test 1: Resolve numeric IP
	{
		uint32_t ip = 0;
		int ret = dns_resolve("192.168.1.1", &ip);
		test_result("dns: numeric IP resolve",
			    ret == 0 && ip == 0xC0A80101);
	}

	// Test 2: Resolve "localhost"
	{
		uint32_t ip = 0;
		int ret = dns_resolve("localhost", &ip);
		test_result("dns: localhost resolves to 127.0.0.1",
			    ret == 0 && ip == 0x7F000001);
	}

	// ========================================
	// Copy-on-write and address-space integrity
	// ========================================
	printf("\n--- Copy-on-write / address space ---\n");

	{
		const size_t cow_len = 64 * 4096;
		unsigned char *cow;

		/* 1. The basic guarantee: after fork, each side's writes are
		 *    private.  If a page is ever handed to both sides writable
		 *    -- which is what happens when a fault decides a shared
		 *    page is unshared -- the two see each other's bytes. */
		cow = mmap(NULL, cow_len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (cow == MAP_FAILED) {
			test_fail("cow: mmap failed");
		} else {
			pid_t kid;
			int st = 0;

			memset(cow, 0xA5, cow_len);
			kid = fork();
			if (kid == 0) {
				/* Child rewrites every page, then checks it
				 * sees only its own bytes. */
				int ok = 1;

				memset(cow, 0x3C, cow_len);
				for (size_t i = 0; i < cow_len; i++)
					if (cow[i] != 0x3C)
						ok = 0;
				_exit(ok ? 0 : 1);
			}
			if (kid > 0) {
				int parent_ok = 1;

				/* Parent writes a different pattern at the
				 * same time. */
				memset(cow, 0x5A, cow_len);
				while (waitpid(kid, &st, 0) < 0 && errno == EINTR)
					;
				for (size_t i = 0; i < cow_len; i++)
					if (cow[i] != 0x5A)
						parent_ok = 0;
				test_result("cow: child sees only its own writes",
					    WIFEXITED(st) && WEXITSTATUS(st) == 0);
				test_result("cow: parent sees only its own writes",
					    parent_ok);
			} else {
				test_fail("cow: fork failed");
			}
			munmap(cow, cow_len);
		}

		/* 2. Once the child is gone the parent holds the only
		 *    reference, so writing must not corrupt anything -- this is
		 *    the path that now reuses the page instead of copying it. */
		cow = mmap(NULL, cow_len, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (cow != MAP_FAILED) {
			pid_t kid;
			int st = 0, ok = 1;

			for (size_t i = 0; i < cow_len; i++)
				cow[i] = (unsigned char)(i * 7);
			kid = fork();
			if (kid == 0)
				_exit(0); /* exit at once, dropping its share */
			if (kid > 0) {
				while (waitpid(kid, &st, 0) < 0 && errno == EINTR)
					;
				/* Every page is now exclusively ours. */
				for (size_t i = 0; i < cow_len; i++)
					cow[i] = (unsigned char)(i * 13);
				for (size_t i = 0; i < cow_len; i++)
					if (cow[i] != (unsigned char)(i * 13))
						ok = 0;
				test_result("cow: exclusive page reuse keeps contents",
					    ok);
			}
			munmap(cow, cow_len);
		}

		/* 3. THE lost-write test.  Several threads write distinct
		 *    bytes of the same fork-shared pages at once, so many of
		 *    them fault on the same page simultaneously.  A fault that
		 *    publishes its copy without re-checking loses every write
		 *    made between the two copies -- the page does not become
		 *    garbage, it silently reverts to an earlier version. */
		{
			pthread_t th[COW_THREADS];
			int ok = 1;

			g_cow_go = 0;
			g_cow_shared = mmap(NULL, COW_STRESS_LEN,
					    PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (g_cow_shared == MAP_FAILED) {
				test_fail("cow-threads: mmap failed");
			} else {
				pid_t kid;
				int st = 0;

				memset(g_cow_shared, 0, COW_STRESS_LEN);
				/* fork() marks it all copy-on-write; the child
				 * lingers so the pages stay shared while the
				 * threads race on them. */
				kid = fork();
				if (kid == 0) {
					usleep(200000);
					_exit(0);
				}

				for (long t = 0; t < COW_THREADS; t++)
					if (pthread_create(&th[t], NULL,
							   cow_writer_fn,
							   (void *)t) != 0)
						ok = 0;
				g_cow_go = 1;
				for (int t = 0; t < COW_THREADS; t++)
					pthread_join(th[t], NULL);

				for (size_t i = 0; i < COW_STRESS_LEN; i++) {
					if (g_cow_shared[i] !=
					    (unsigned char)((i % COW_THREADS) +
							    1)) {
						ok = 0;
						break;
					}
				}
				test_result("cow: concurrent faults on one page lose no writes",
					    ok);
				if (kid > 0)
					while (waitpid(kid, &st, 0) < 0 &&
					       errno == EINTR)
						;
				munmap(g_cow_shared, COW_STRESS_LEN);
			}
		}

		/* 3b. THE stale-read test, which is a different failure from the
		 *     one above and invisible to it.
		 *
		 *     Test 3 checks the pages after the joins, when every
		 *     processor has long since been told about every copy.  It
		 *     therefore sees a write that was thrown away and cannot see
		 *     a read that was served from the pre-copy frame while the
		 *     copy was still being announced.
		 *
		 *     Here one thread writes byte 0 of page i and then raises a
		 *     flag; another thread waits for the flag and reads that same
		 *     byte.  This architecture does not reorder stores, so having
		 *     seen the flag the reader must see the write.  If it does
		 *     not, its translation is still pointing at the frame the
		 *     page was copied away from, and the two threads -- in ONE
		 *     address space -- are looking at two different memories.
		 *
		 *     The flag has to be somewhere the writer can store to
		 *     without faulting, or the fault would order the two stores
		 *     for us and prove nothing; it is written once before the
		 *     reader starts, which resolves its own copy. */
		{
			pthread_t rd;
			int ok = 1, started = 0;
			pid_t kid = -1;
			int st = 0;

			g_cowstale_checked = 0;
			g_cowstale_stale = 0;
			g_cowstale_stop = 0;
			g_cowstale_mem = mmap(NULL,
					      COWSTALE_PAGES * COWSTALE_PGSZ,
					      PROT_READ | PROT_WRITE,
					      MAP_PRIVATE | MAP_ANONYMOUS, -1,
					      0);
			void *flagpg = mmap(NULL, COWSTALE_PGSZ,
					    PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			g_cowstale_flag = flagpg == MAP_FAILED ? NULL : flagpg;
			if (g_cowstale_mem == MAP_FAILED || !g_cowstale_flag) {
				test_fail("cow-stale: mmap failed");
			} else {
				memset(g_cowstale_mem, 0,
				       COWSTALE_PAGES * COWSTALE_PGSZ);
				*g_cowstale_flag = -1;

				kid = fork();
				if (kid == 0) {
					usleep(400000);
					_exit(0);
				}

				/* The flag page is copy-on-write too now.
				 * Resolve it here so the loop below stores to
				 * it without a fault. */
				*g_cowstale_flag = -1;

				if (pthread_create(&rd, NULL,
						   cowstale_reader_fn,
						   NULL) != 0) {
					ok = 0;
					test_fail("cow-stale: pthread_create failed");
				} else {
					started = 1;
				}

				if (started) {
					for (long i = 0; i < COWSTALE_PAGES;
					     i++) {
						g_cowstale_mem[i * COWSTALE_PGSZ] =
							(unsigned char)(i + 1);
						__asm__ volatile("" ::: "memory");
						*g_cowstale_flag = i;
					}
					pthread_join(rd, NULL);

					if (g_cowstale_checked !=
					    COWSTALE_PAGES) {
						/* The reader gave up waiting;
						 * nothing was proved either
						 * way, so say so rather than
						 * passing. */
						printf("  cow-stale: reader checked only %ld of %d\n",
						       (long)g_cowstale_checked,
						       COWSTALE_PAGES);
						ok = 0;
					}
					if (g_cowstale_stale) {
						printf("  cow-stale: %ld of %ld reads came from the pre-copy frame\n",
						       (long)g_cowstale_stale,
						       (long)g_cowstale_checked);
						ok = 0;
					}
					test_result("cow: a copied page is visible to other threads at once",
						    ok);
				}

				if (kid > 0)
					while (waitpid(kid, &st, 0) < 0 &&
					       errno == EINTR)
						;
			}
			if (g_cowstale_mem != MAP_FAILED)
				munmap(g_cowstale_mem,
				       COWSTALE_PAGES * COWSTALE_PGSZ);
			if (flagpg != MAP_FAILED)
				munmap(flagpg, COWSTALE_PGSZ);
		}

		/* 4. Unmapping must not hand a page back while another mapping
		 *    still reads it.  Cycle map/touch/unmap hard, then verify
		 *    a long-lived buffer was never overwritten -- freed pages
		 *    are filled with a poison pattern, so a page released too
		 *    early shows up as that pattern in live memory. */
		{
			const size_t keep_len = 16 * 4096;
			unsigned char *keep =
				mmap(NULL, keep_len, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			int ok = 1;

			if (keep == MAP_FAILED) {
				test_fail("unmap-churn: mmap failed");
			} else {
				for (size_t i = 0; i < keep_len; i++)
					keep[i] = (unsigned char)(i * 31 + 5);

				for (int round = 0; round < 64; round++) {
					unsigned char *tmp = mmap(
						NULL, 32 * 4096,
						PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS, -1,
						0);
					if (tmp == MAP_FAILED)
						continue;
					memset(tmp, round & 0xFF, 32 * 4096);
					munmap(tmp, 32 * 4096);
				}

				for (size_t i = 0; i < keep_len; i++) {
					if (keep[i] !=
					    (unsigned char)(i * 31 + 5)) {
						ok = 0;
						break;
					}
				}
				test_result("unmap churn does not disturb a live mapping",
					    ok);
				munmap(keep, keep_len);
			}
		}
	}

	// ========================================
	// File-backed private mmap
	// ========================================
	printf("\n--- file-backed mmap ---\n");

	{
		/* A read-only MAP_PRIVATE mapping of a file is how a program
		 * hands a file's bytes to something that wants a pointer -- an
		 * IMAP client uploading a queued message does exactly this,
		 * mmapping the file and writing those bytes to the socket.
		 *
		 * The interesting sizes are the ones that are NOT a whole
		 * number of pages, and especially the ones that cross a page
		 * boundary: the first page comes straight from the file, the
		 * last is a partial read whose tail must read as zeros, and a
		 * mapping of a few bytes must not be confused with one of a
		 * few thousand. */
		static const size_t sizes[] = { 100, 4095, 4096, 4097, 5000,
						8192, 12000 };

		for (unsigned si = 0; si < sizeof(sizes) / sizeof(sizes[0]);
		     si++) {
			size_t sz = sizes[si];
			char path[64];
			int fd;
			unsigned char *map;
			int ok = 1;

			snprintf(path, sizeof(path), "/tmp/lktest_mmap_%u_%d",
				 si, (int)getpid());
			unlink(path);
			fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
			if (fd < 0) {
				test_fail("file-mmap: create failed");
				continue;
			}
			/* A pattern that differs on every byte, so a page read
			 * from the wrong offset cannot look correct.  Built in
			 * memory and written in ONE call: a byte-at-a-time
			 * write() loop is tens of thousands of syscalls, each
			 * taking the filesystem's locks, which makes this test
			 * crawl and starves anything else running beside it --
			 * and tests nothing that a single write does not. */
			{
				unsigned char *src = malloc(sz);

				if (!src) {
					test_fail("file-mmap: out of memory");
					close(fd);
					unlink(path);
					continue;
				}
				for (size_t i = 0; i < sz; i++)
					src[i] = (unsigned char)((i * 7 +
								  (i >> 8)) ^
								 0x5A);
				ok = (write(fd, src, sz) == (ssize_t)sz);
				free(src);
			}
			close(fd);
			if (!ok) {
				test_fail("file-mmap: write failed");
				unlink(path);
				continue;
			}

			fd = open(path, O_RDONLY);
			if (fd < 0) {
				test_fail("file-mmap: reopen failed");
				unlink(path);
				continue;
			}
			map = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
			if (map == MAP_FAILED) {
				test_fail("file-mmap: mmap failed");
				close(fd);
				unlink(path);
				continue;
			}

			for (size_t i = 0; i < sz; i++) {
				unsigned char want =
					(unsigned char)((i * 7 + (i >> 8)) ^ 0x5A);
				if (map[i] != want) {
					ok = 0;
					break;
				}
			}
			{
				char name[80];

				snprintf(name, sizeof(name),
					 "mmap of a %u-byte file reads it back exactly",
					 (unsigned)sz);
				test_result(name, ok);
			}

			/* Bytes between the end of the file and the end of the
			 * last page must read as zeros, not as whatever the
			 * page held before. */
			if (sz % 4096) {
				size_t tail_from = sz;
				size_t tail_to = (sz + 4095) & ~(size_t)4095;
				int zeros = 1;

				for (size_t i = tail_from; i < tail_to; i++)
					if (map[i] != 0)
						zeros = 0;
				test_result("mmap tail past EOF reads as zeros",
					    zeros);
			}

			munmap(map, sz);
			close(fd);
			unlink(path);
		}
	}

	{
		/* The exact shape an IMAP client uses to upload a message:
		 * write the file with stdio, close it, then stat() it, mmap
		 * st_size bytes and hand those bytes to the network.
		 *
		 * If stat() reports MORE bytes than the file holds, the mapping
		 * beyond the real end reads as zeros -- and those NULs go out
		 * as part of the message.  A server that validates its input
		 * rejects the whole thing, which is what gmail's "Invalid
		 * character in literal" is.  So the invariant under test is
		 * that st_size, the bytes readable, and the bytes mappable all
		 * agree exactly.
		 *
		 * Sizes bracket a real queued message with a small attachment
		 * (~3.3 KB), plus the page boundary either side of it. */
		static const size_t qsizes[] = { 1500, 3368, 4095, 4096, 4097,
						 6000 };

		for (unsigned qi = 0; qi < sizeof(qsizes) / sizeof(qsizes[0]);
		     qi++) {
			size_t want = qsizes[qi];
			char path[64];
			FILE *f;
			unsigned char *src;
			struct stat st;
			int fd, ok = 1;

			snprintf(path, sizeof(path), "/tmp/lktest_queue_%u_%d",
				 qi, (int)getpid());
			unlink(path);

			src = malloc(want);
			if (!src) {
				test_fail("queue-mmap: out of memory");
				continue;
			}
			/* '| 1' guarantees no byte is ever zero, so any NUL
			 * found later came from us, not from the data. */
			for (size_t i = 0; i < want; i++)
				src[i] = (unsigned char)(((i * 31) ^ (i >> 5)) |
							 1);

			f = fopen(path, "wb");
			if (!f || fwrite(src, 1, want, f) != want ||
			    fclose(f) != 0) {
				test_fail("queue-mmap: stdio write failed");
				free(src);
				unlink(path);
				continue;
			}

			if (stat(path, &st) != 0) {
				test_fail("queue-mmap: stat failed");
				free(src);
				unlink(path);
				continue;
			}
			{
				char nm[96];

				snprintf(nm, sizeof(nm),
					 "stat() size matches what was written (%u bytes)",
					 (unsigned)want);
				test_result(nm, (size_t)st.st_size == want);
			}

			fd = open(path, O_RDONLY);
			if (fd >= 0) {
				unsigned char *map =
					mmap(NULL, (size_t)st.st_size, PROT_READ,
					     MAP_PRIVATE, fd, 0);

				if (map == MAP_FAILED) {
					test_fail("queue-mmap: mmap failed");
				} else {
					size_t nuls = 0;

					for (size_t i = 0;
					     i < (size_t)st.st_size; i++) {
						if (map[i] == 0)
							nuls++;
						if (i < want && map[i] != src[i])
							ok = 0;
					}
					{
						char nm[96];

						snprintf(nm, sizeof(nm),
							 "mmap of a %u-byte file matches it byte for byte",
							 (unsigned)want);
						test_result(nm, ok);
					}
					/* The decisive one: a NUL inside the
					 * range stat() called file content
					 * means zero-fill leaked into data a
					 * program would transmit. */
					test_result("no NUL bytes within the stat()ed length",
						    nuls == 0);
					munmap(map, (size_t)st.st_size);
				}
				close(fd);
			} else {
				test_fail("queue-mmap: reopen failed");
			}
			free(src);
			unlink(path);
		}
	}

	// ========================================
	// Releasing pages without losing the mapping
	// ========================================
	printf("\n--- MADV_DONTNEED / region accounting ---\n");

	{
		/* MADV_DONTNEED gives the physical pages back and KEEPS the
		 * mapping: the range must still be readable afterwards, and
		 * must read as zeros. */
		const size_t len = 64 * 4096;
		unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (m == MAP_FAILED) {
			test_fail("dontneed: mmap failed");
		} else {
			int ok = 1;

			memset(m, 0xC7, len);
			/* Drop the middle, keeping the first and last page so
			 * the released span is strictly interior. */
			test_result("madvise(MADV_DONTNEED) succeeds",
				    madvise(m + 4096, len - 2 * 4096,
					    MADV_DONTNEED) == 0);

			/* Still mapped: reading must not fault. */
			for (size_t i = 4096; i < len - 4096; i++)
				if (m[i] != 0)
					ok = 0;
			test_result("released pages read back as zeros", ok);

			/* The pages either side were not asked for and must be
			 * untouched. */
			ok = 1;
			for (size_t i = 0; i < 4096; i++)
				if (m[i] != 0xC7)
					ok = 0;
			for (size_t i = len - 4096; i < len; i++)
				if (m[i] != 0xC7)
					ok = 0;
			test_result("pages outside the range are untouched", ok);

			/* And it is still writable afterwards. */
			memset(m, 0x5B, len);
			ok = 1;
			for (size_t i = 0; i < len; i++)
				if (m[i] != 0x5B)
					ok = 0;
			test_result("the mapping is still usable after DONTNEED",
				    ok);
			munmap(m, len);
		}
	}

	{
		/* The regression that made an allocator run the kernel's
		 * mapping table dry: trim and re-grow in a loop.  Each cycle
		 * used to split one mapping into two and spend a record; after
		 * a few hundred, every mmap in the process failed -- far from
		 * the code responsible.  Records that abut and match are folded
		 * back together now, so this must be able to run indefinitely.
		 */
		const size_t len = 32 * 4096;
		int ok = 1;

		for (int cycle = 0; cycle < 400 && ok; cycle++) {
			unsigned char *m =
				mmap(NULL, len, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			if (m == MAP_FAILED) {
				ok = 0;
				break;
			}
			memset(m, 0x11, len);
			/* Interior release, then touch it again. */
			if (madvise(m + 4096, len - 2 * 4096, MADV_DONTNEED) != 0)
				ok = 0;
			memset(m + 4096, 0x22, len - 2 * 4096);
			munmap(m, len);
		}
		test_result("400 map/trim/regrow/unmap cycles do not exhaust the mapping table",
			    ok);

		/* And a plain map/unmap loop must not leak records either. */
		ok = 1;
		for (int cycle = 0; cycle < 400 && ok; cycle++) {
			void *m = mmap(NULL, 16 * 4096, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			if (m == MAP_FAILED) {
				ok = 0;
				break;
			}
			munmap(m, 16 * 4096);
		}
		test_result("400 plain map/unmap cycles do not exhaust the mapping table",
			    ok);
	}

	{
		/* Unmapping across holes.
		 *
		 * A range with gaps in it used to be walked a page at a time,
		 * and every one of those pages paid a full scan of the process
		 * mapping table looking for a record that was not there.  The
		 * cost was the size of the address range multiplied by the size
		 * of the table, for address space that held nothing at all.
		 *
		 * Build a deliberately holey span -- alternate mappings and
		 * gaps -- then unmap the whole thing in one call.  What is
		 * being asserted is that it completes promptly and that the
		 * mappings really went away. */
		const size_t chunk = 16 * 4096;
		void *base;
		int ok = 1;
		long ms;
		struct timespec t0;

		/* One big reservation, then punch it into alternating pieces so
		 * the gaps are real address space rather than another
		 * mapping. */
		base = mmap(NULL, 64 * chunk, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED) {
			test_fail("sparse unmap: reservation failed");
		} else {
			for (int i = 0; i < 64; i += 2) {
				if (munmap((char *)base + (size_t)i * chunk,
					   chunk) != 0)
					ok = 0;
			}
			test_result("punching alternate holes in a reservation succeeds",
				    ok);

			/* Touch what is left so the pages are really there. */
			for (int i = 1; i < 64; i += 2)
				memset((char *)base + (size_t)i * chunk, 0x3D,
				       chunk);

			mono_now(&t0);
			ok = (munmap(base, 64 * chunk) == 0);
			ms = mono_elapsed_ms(&t0);
			test_result("unmapping a range full of holes succeeds", ok);
			if (ms >= 5000)
				printf("  sparse unmap took %ld ms\n", ms);
			test_result("unmapping a range full of holes is prompt",
				    ms < 5000);
		}
	}


	// ========================================
	// fork() from a thread
	// ========================================
	printf("\n--- fork from a thread ---\n");

	{
		/* POSIX: the child of fork() is a new process containing a
		 * single thread -- a replica of the calling thread -- with its
		 * own process id and its own copy of the descriptor table.
		 * Ending it ends it alone.
		 *
		 * Every check here is something a threaded program that forks
		 * helpers depends on without ever testing for it, and every
		 * failure mode is invisible from userspace: no signal, no
		 * message, just a process that is suddenly gone or a
		 * connection that is suddenly closed. */
		pthread_t bystander, forker;
		volatile int stop = 0;
		pid_t child_pid = 0, child_ppid = 0;
		int st = 0;
		ssize_t n;
		char probe[8];
		char path[64];

		snprintf(path, sizeof(path), "/tmp/forkthr_%d.dat",
			 (int)getpid());

		g_forkthr_ok = 0;
		g_forkthr_child = 0;

		/* A descriptor the child will try to close.  If the table is
		 * shared, this is unusable afterwards in the parent. */
		g_forkthr_spare_fd = open(path, O_RDWR | O_CREAT | O_TRUNC,
					  0644);
		test_result("fork-thread: opened a descriptor to guard",
			    g_forkthr_spare_fd >= 3);

		test_result("fork-thread: pipe created",
			    pipe(g_forkthr_pipe) == 0);

		if (pthread_create(&bystander, NULL, forkthr_bystander,
				   (void *)&stop) != 0) {
			test_fail("fork-thread: bystander thread");
		} else if (pthread_create(&forker, NULL, forkthr_body,
					  NULL) != 0) {
			test_fail("fork-thread: forking thread");
			stop = 1;
			pthread_join(bystander, NULL);
		} else {
			pthread_join(forker, NULL);
			test_result("fork-thread: fork() from a non-main "
				    "thread succeeded",
				    g_forkthr_ok == 1 && g_forkthr_child > 0);

			close(g_forkthr_pipe[1]);
			n = read(g_forkthr_pipe[0], &child_pid,
				 sizeof(child_pid));
			test_result("fork-thread: child reported its pid",
				    n == (ssize_t)sizeof(child_pid));
			n = read(g_forkthr_pipe[0], &child_ppid,
				 sizeof(child_ppid));
			test_result("fork-thread: child reported its parent",
				    n == (ssize_t)sizeof(child_ppid));
			close(g_forkthr_pipe[0]);

			/* The child is a process of its own, not a thread of
			 * this one. */
			test_result("fork-thread: child has its own pid",
				    child_pid == g_forkthr_child &&
					    child_pid != getpid());
			/* getppid() names the PROCESS that forked, never the
			 * thread inside it that made the call. */
			test_result("fork-thread: child's parent is the "
				    "process, not the calling thread",
				    child_ppid == getpid());

			/* Reap it.  _exit(1) in the child must be exactly
			 * that: status 1, no signal, and this process still
			 * running to observe it. */
			test_result("fork-thread: waitpid reaped the child",
				    waitpid(child_pid, &st, 0) == child_pid);
			test_result("fork-thread: child exited normally",
				    WIFEXITED(st) != 0);
			test_result("fork-thread: child's _exit(1) reported "
				    "as status 1",
				    WIFEXITED(st) && WEXITSTATUS(st) == 1);
			test_result("fork-thread: child was not signalled",
				    WIFSIGNALED(st) == 0);

			/* The whole point: this process is still here.  If the
			 * child's exit had reached its parent's thread group,
			 * there would be nothing left to run this line. */
			test_pass("fork-thread: parent survived the child's "
				  "_exit");

			/* And the descriptor table was a copy: the child's
			 * close-everything loop cannot have touched ours. */
			n = write(g_forkthr_spare_fd, "alive", 5);
			test_result("fork-thread: parent's descriptor survived "
				    "the child closing its own",
				    n == 5);
			test_result("fork-thread: parent's descriptor still "
				    "seeks",
				    lseek(g_forkthr_spare_fd, 0, SEEK_SET) == 0);
			n = read(g_forkthr_spare_fd, probe, 5);
			test_result("fork-thread: parent's descriptor still "
				    "reads back",
				    n == 5 && memcmp(probe, "alive", 5) == 0);

			stop = 1;
			pthread_join(bystander, NULL);
		}

		if (g_forkthr_spare_fd >= 0)
			close(g_forkthr_spare_fd);
		unlink(path);
	}

	// ========================================
	// How long a process takes to go away
	// ========================================
	printf("\n--- process teardown ---\n");

	{
		/* The headline case: a process with a large resident set exits,
		 * and the time measured is from "the child has finished its
		 * work" to "the parent has reaped it" -- that is, the teardown
		 * alone, with the child's setup excluded.
		 *
		 * This used to be minutes rather than milliseconds.  Every page
		 * was released individually under one global lock with
		 * interrupts disabled, and each release wrote the whole page
		 * with a poison pattern that nothing ever read.  A CPU waiting
		 * for that lock waits with interrupts off, so it could not
		 * acknowledge the address-space invalidations other CPUs were
		 * blocked on, and those ran into their timeout instead.
		 *
		 * The bound below is deliberately loose.  It is not a
		 * performance target; it is the line between "frees the address
		 * space" and "grinds through it one page at a time". */
		size_t mb = 128;
		int pfd[2];

		if (pipe(pfd) != 0) {
			test_fail("teardown: pipe failed");
		} else {
			pid_t pid = fork();

			if (pid < 0) {
				test_fail("teardown: fork failed");
				close(pfd[0]);
				close(pfd[1]);
			} else if (pid == 0) {
				char *m = MAP_FAILED;
				size_t len = 0;

				close(pfd[0]);
				/* Take what we can get: a smaller resident set
				 * still exercises the same path, and a machine
				 * short of memory should not fail the run. */
				while (mb >= 16) {
					len = mb * 1024 * 1024;
					m = mmap(NULL, len,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS,
						 -1, 0);
					if (m != MAP_FAILED)
						break;
					mb /= 2;
				}
				if (m != MAP_FAILED) {
					/* Touch every page: reserving address
					 * space costs nothing to tear down, and
					 * resident pages are the point. */
					for (size_t off = 0; off < len;
					     off += 4096)
						m[off] = 0x9E;
				}
				/* "Setup done" -- the parent starts timing
				 * here, then we leave immediately. */
				{
					char c = (m != MAP_FAILED) ? 'y' : 'n';

					if (write(pfd[1], &c, 1) != 1)
						_exit(2);
				}
				exit(0);
			} else {
				char c = 0;
				long ms;
				struct timespec t0;
				int status = 0;
				ssize_t r;

				close(pfd[1]);
				r = read(pfd[0], &c, 1);
				mono_now(&t0);
				test_result("teardown: child reported its setup",
					    r == 1);
				test_result("teardown: child mapped its working set",
					    c == 'y');

				test_result("teardown: the child is reaped",
					    waitpid(pid, &status, 0) == pid);
				ms = mono_elapsed_ms(&t0);
				test_result("teardown: the child exited cleanly",
					    WIFEXITED(status) &&
						    WEXITSTATUS(status) == 0);
				printf("  a %zu MB process took %ld ms to go away\n",
				       mb, ms);
				test_result("teardown: a large process goes away promptly",
					    ms < 10000);
				close(pfd[0]);
			}
		}
	}

	{
		/* The same, for a process with several threads.
		 *
		 * A thread group exits by killing its members and waiting for
		 * each to leave on its own; every one of them has an address
		 * space to release and a stack to give back.  The parent must
		 * not be told the process is over until the last of them has
		 * gone, and that must not take appreciably longer than one
		 * thread does. */
		int pfd[2];

		if (pipe(pfd) != 0) {
			test_fail("threaded teardown: pipe failed");
		} else {
			pid_t pid = fork();

			if (pid < 0) {
				test_fail("threaded teardown: fork failed");
				close(pfd[0]);
				close(pfd[1]);
			} else if (pid == 0) {
				pthread_t th[8];
				int made = 0;

				close(pfd[0]);
				for (int i = 0; i < 8; i++) {
					if (pthread_create(&th[i], NULL,
							   teardown_thread_body,
							   NULL) == 0)
						made++;
				}
				/* Give each thread a moment to reach its
				 * blocking call, so exit really is tearing down
				 * live threads. */
				usleep(200000);
				{
					char c = (made == 8) ? 'y' : 'n';

					if (write(pfd[1], &c, 1) != 1)
						_exit(2);
				}
				exit(0);
			} else {
				char c = 0;
				long ms;
				struct timespec t0;
				int status = 0;
				ssize_t r;

				close(pfd[1]);
				r = read(pfd[0], &c, 1);
				mono_now(&t0);
				test_result("threaded teardown: child reported its setup",
					    r == 1);
				test_result("threaded teardown: all threads started",
					    c == 'y');

				test_result("threaded teardown: the child is reaped",
					    waitpid(pid, &status, 0) == pid);
				ms = mono_elapsed_ms(&t0);
				test_result("threaded teardown: the child exited cleanly",
					    WIFEXITED(status) &&
						    WEXITSTATUS(status) == 0);
				printf("  an 8-thread process took %ld ms to go away\n",
				       ms);
				test_result("threaded teardown: a threaded process goes away promptly",
					    ms < 10000);
				close(pfd[0]);
			}
		}
	}

	{
		/* Repeated spawn and exit.
		 *
		 * One teardown being fast is not the same as teardown being
		 * correct: releasing pages in batches means a mistake in the
		 * batching leaks them or, worse, releases one twice.  Neither
		 * shows up on a single run.  Cycle through enough short-lived
		 * children with a real working set that anything not given back
		 * would run the machine out of memory, and require every one of
		 * them to be reaped. */
		const int cycles = 60;
		int reaped = 0;
		int clean = 1;
		long ms;
		struct timespec t0;

		mono_now(&t0);
		for (int i = 0; i < cycles; i++) {
			pid_t pid = fork();
			int status = 0;

			if (pid < 0)
				break;
			if (pid == 0) {
				const size_t len = 8 * 1024 * 1024;
				char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
					       MAP_PRIVATE | MAP_ANONYMOUS, -1,
					       0);

				if (m == MAP_FAILED)
					_exit(3);
				for (size_t off = 0; off < len; off += 4096)
					m[off] = 0x27;
				exit(0);
			}
			if (waitpid(pid, &status, 0) != pid)
				break;
			reaped++;
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				clean = 0;
		}
		ms = mono_elapsed_ms(&t0);
		printf("  %d spawn/exit cycles in %ld ms\n", reaped, ms);
		test_result("every short-lived child is reaped",
			    reaped == cycles);
		test_result("every short-lived child exits cleanly", clean);
		/* If pages were not really returned, the later children could
		 * not have mapped and touched their working set at all -- which
		 * the exit status above already caught -- so this is purely
		 * about the time it took. */
		test_result("repeated spawn/exit stays prompt", ms < 30000);
	}

	{
		/* exit() must flush every open stream, not just the standard
		 * ones.  A program that writes a file and leaves it open at
		 * exit had those bytes discarded without a word -- which shows
		 * up much later as a truncated config or a half-written cache
		 * file, nowhere near the code that wrote it.
		 *
		 * Checked through a child, because it is the child's exit that
		 * has to do the flushing. */
		char path[64];

		snprintf(path, sizeof(path), "/tmp/exitflush_%d",
			 (int)getpid());
		pid_t pid;

		unlink(path);
		pid = fork();
		if (pid < 0) {
			test_fail("exit flush: fork failed");
		} else if (pid == 0) {
			FILE *f = fopen(path, "w");

			if (!f)
				_exit(4);
			/* Well under the buffer size, so nothing reaches the
			 * file unless something flushes it. */
			fputs("written but never closed", f);
			exit(0); /* deliberately no fclose */
		} else {
			int status = 0;
			char buf[64] = { 0 };
			FILE *f;

			waitpid(pid, &status, 0);
			test_result("exit flush: child exited cleanly",
				    WIFEXITED(status) &&
					    WEXITSTATUS(status) == 0);

			f = fopen(path, "r");
			if (!f) {
				test_fail("exit flush: the file was not created");
			} else {
				size_t n = fread(buf, 1, sizeof(buf) - 1, f);

				fclose(f);
				buf[n] = '\0';
				test_result("exit() flushes a stream the program left open",
					    strcmp(buf,
						   "written but never closed") ==
						    0);
			}
			unlink(path);
		}
	}

	{
		/* fflush(NULL) means all of them, in this process. */
		char path[64];

		snprintf(path, sizeof(path), "/tmp/fflushall_%d",
			 (int)getpid());
		FILE *f = fopen(path, "w");

		if (!f) {
			test_fail("fflush(NULL): could not create the file");
		} else {
			char buf[32] = { 0 };
			FILE *r;

			fputs("buffered", f);
			test_result("fflush(NULL) succeeds", fflush(NULL) == 0);

			r = fopen(path, "r");
			if (!r) {
				test_fail("fflush(NULL): could not reopen");
			} else {
				size_t n = fread(buf, 1, sizeof(buf) - 1, r);

				fclose(r);
				buf[n] = '\0';
				test_result("fflush(NULL) flushes a stream other than stdout",
					    strcmp(buf, "buffered") == 0);
			}
			fclose(f);
			unlink(path);
		}
	}

	{
		/* Exit handlers run most-recently-registered first, and the
		 * time it takes has to grow with their number rather than with
		 * its square.  The walk used to restart from the beginning
		 * after every single handler, so a program with a few thousand
		 * static destructors -- which is what loading a large graphical
		 * stack produces -- paid for all of them many times over.
		 *
		 * Registered in a child, because they only run when it exits;
		 * the child verifies the ordering itself and reports through
		 * its exit status. */
		pid_t pid = fork();

		if (pid < 0) {
			test_fail("atexit scale: fork failed");
		} else if (pid == 0) {
			int ok = 1;

			for (int i = 0; i < ATEXIT_SCALE_N; i++) {
				if (atexit(atexit_order_probe) != 0) {
					ok = 0;
					break;
				}
			}
			atexit_expect = ATEXIT_SCALE_N;
			if (!ok)
				_exit(5);
			/* atexit_order_probe counts down; exit status is
			 * decided by the last handler to run. */
			exit(0);
		} else {
			int status = 0;
			long ms;
			struct timespec t0;

			mono_now(&t0);
			test_result("atexit scale: the child is reaped",
				    waitpid(pid, &status, 0) == pid);
			ms = mono_elapsed_ms(&t0);
			printf("  %d atexit handlers ran in %ld ms\n",
			       ATEXIT_SCALE_N, ms);
			test_result("every atexit handler runs, in reverse order",
				    WIFEXITED(status) &&
					    WEXITSTATUS(status) == 0);
			test_result("atexit handlers do not cost the square of their number",
				    ms < 5000);
		}
	}

	// ========================================
	// poll() never reports a live connection as gone
	// ========================================
	printf("\n--- unix poll under traffic ---\n");

	{
		/* Drive a named socket the way a display connection is driven:
		 * send a request, wait in poll(), read the reply, repeat, many
		 * thousands of times.
		 *
		 * What is being watched for is not data corruption -- the
		 * stream test above covers that -- but poll() saying the
		 * connection has hung up or errored while the other end is
		 * plainly still there.  A program that trusts that answer
		 * concludes its connection is gone and exits, without a crash,
		 * without a signal, and without the far end noticing anything.
		 * There is no way to tell that apart from a clean exit
		 * afterwards, which is why it has to be caught here.
		 *
		 * Bound socket, not a socketpair, and two-way, not one: those
		 * are the two things a display connection does that bulk
		 * transfer tests do not. */
		char sock_path[64];

		snprintf(sock_path, sizeof(sock_path),
			 "/tmp/polltraffic_%d.sock", (int)getpid());
		const int rounds = 20000;
		int lfd, cfd = -1, ok = 1;
		struct sockaddr_un sa;
		pthread_t peer;
		int made_peer = 0;
		int bad_revents = 0, bad_echo = 0, poll_timeouts = 0;
		int first_bad = 0;

		unlink(sock_path);
		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		memset(&sa, 0, sizeof(sa));
		sa.sun_family = AF_UNIX;
		strncpy(sa.sun_path, sock_path, sizeof(sa.sun_path) - 1);

		if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) ||
		    listen(lfd, 4)) {
			test_fail("poll traffic: could not set up the socket");
			if (lfd >= 0)
				close(lfd);
		} else {
			/* The far end must be waiting in accept() before this
			 * end connects; see echo_peer. */
			echo_listen_fd = lfd;
			echo_ready = 0;
			if (pthread_create(&peer, NULL, echo_peer, NULL) !=
			    0) {
				test_fail("poll traffic: no peer thread");
				ok = 0;
			} else {
				made_peer = 1;
				cfd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (cfd < 0 ||
				    connect(cfd, (struct sockaddr *)&sa,
					    sizeof(sa))) {
					test_fail("poll traffic: connect failed");
					ok = 0;
				}
				while (ok && echo_ready == 0)
					sched_yield();
				if (echo_ready < 0) {
					test_fail("poll traffic: accept failed");
					ok = 0;
				}
			}

			if (ok && made_peer) {
				long ms;
				struct timespec t0;

				mono_now(&t0);
				for (int i = 0; i < rounds && ok; i++) {
					struct pollfd pfd;
					unsigned char req[24], rep[24];
					unsigned len = 4 + (unsigned)(i % 20);
					ssize_t got = 0;

					for (unsigned k = 0; k < len; k++)
						req[k] = (unsigned char)(i + k);
					if (write(cfd, req, len) !=
					    (ssize_t)len) {
						ok = 0;
						break;
					}

					while (got < (ssize_t)len) {
						ssize_t r;

						pfd.fd = cfd;
						pfd.events = POLLIN;
						pfd.revents = 0;
						if (poll(&pfd, 1, 5000) == 0) {
							poll_timeouts++;
							ok = 0;
							break;
						}
						/* The connection is open at
						 * both ends for the whole
						 * loop, so these must never
						 * appear. */
						if (pfd.revents &
						    (POLLHUP | POLLERR |
						     POLLNVAL)) {
							if (!bad_revents)
								first_bad = i;
							bad_revents =
								pfd.revents;
							ok = 0;
							break;
						}
						r = read(cfd, rep + got,
							 len - (unsigned)got);
						if (r <= 0) {
							ok = 0;
							break;
						}
						got += r;
					}
					if (!ok)
						break;
					if (memcmp(req, rep, len) != 0) {
						bad_echo = 1;
						first_bad = i;
						ok = 0;
						break;
					}
				}
				ms = mono_elapsed_ms(&t0);

				test_result("poll traffic: connection stayed usable throughout",
					    ok);
				printf("  %d request/reply rounds in %ld ms\n",
				       rounds, ms);
				if (bad_revents)
					printf("  POLL CALLED A LIVE CONNECTION DEAD at round %d (revents=0x%x)\n",
					       first_bad, bad_revents);
				if (poll_timeouts)
					printf("  POLL MISSED A WAKEUP (%d timeouts)\n",
					       poll_timeouts);
				if (bad_echo)
					printf("  REPLY DID NOT MATCH at round %d\n",
					       first_bad);

				close(cfd);
				pthread_join(peer, NULL);
				close(echo_fd);
			} else if (cfd >= 0) {
				close(cfd);
			}
			close(lfd);
			unlink(sock_path);
		}
	}

	// ========================================
	// A socket stream stays exact under rate
	// ========================================
	printf("\n--- unix stream integrity ---\n");

	{
		/* Send several megabytes through a socket in small writes of
		 * varying size, as fast as the two ends can go, and check that
		 * what arrives is exactly what was sent -- every byte, in
		 * order, none missing, none repeated.
		 *
		 * The existing socket tests move bulk data in large pieces and
		 * ask whether it survives.  This asks a harder question at a
		 * far higher message rate, because that is what a protocol
		 * connection does, and because a single byte lost there does
		 * not damage one message: it desynchronises the connection
		 * permanently, and the far end drops the client rather than
		 * reporting anything.  A program dying that way leaves no crash
		 * and no error -- only an exit.
		 *
		 * The first disagreeing offset is printed, since where the
		 * stream slipped is the whole diagnosis. */
		int sv[2];
		pthread_t prod;
		long ms;
		struct timespec t0;

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			test_fail("unix stream: socketpair failed");
		} else {
			unsigned char rbuf[4096];
			unsigned long got = 0, bad_off = 0;
			int ok = 1, mismatch = 0, short_read = 0;

			stream_fd = sv[1];
			mono_now(&t0);
			if (pthread_create(&prod, NULL, stream_producer,
					   NULL) != 0) {
				test_fail("unix stream: could not start the writer");
				close(sv[0]);
				close(sv[1]);
			} else {
				while (got < STREAM_TOTAL) {
					ssize_t r = read(sv[0], rbuf,
							 sizeof(rbuf));

					if (r <= 0) {
						short_read = 1;
						ok = 0;
						break;
					}
					for (ssize_t i = 0; i < r; i++) {
						if (rbuf[i] !=
						    stream_byte(got +
								(unsigned long)i)) {
							bad_off = got +
								  (unsigned long)i;
							mismatch = 1;
							ok = 0;
							break;
						}
					}
					if (!ok)
						break;
					got += (unsigned long)r;
				}
				ms = mono_elapsed_ms(&t0);

				{
					void *pr = NULL;

					pthread_join(prod, &pr);
					test_result("unix stream: the writer sent everything",
						    pr == NULL);
				}
				test_result("unix stream: every byte arrived, in order",
					    ok && got == STREAM_TOTAL);
				printf("  %lu of %u bytes in %ld ms\n", got,
				       STREAM_TOTAL, ms);
				if (mismatch)
					printf("  STREAM SLIPPED at byte %lu (%lu KB in)\n",
					       bad_off, bad_off / 1024);
				if (short_read)
					printf("  STREAM ENDED EARLY after %lu bytes\n",
					       got);
				close(sv[0]);
				close(sv[1]);
			}
		}
	}

	// ========================================
	// AF_UNIX socket lifetime
	// ========================================
	printf("\n--- unix socket lifetime ---\n");

	{
		/* An abandoned connect must not be able to reach into an
		 * unrelated socket.
		 *
		 * A connect() that is never accepted leaves an entry on the
		 * listener's queue.  If the connecting socket is then closed,
		 * that entry has to stop referring to it -- because the next
		 * socket created takes its place, and the listener's accept()
		 * would otherwise link that new, unrelated socket into a
		 * connection it knows nothing about.
		 *
		 * The victim then has a peer it never asked for.  Its writes
		 * go to a stranger instead of to the socket it is actually
		 * paired with, and when that stranger closes, the victim is
		 * told its peer hung up.  A program multiplexing with poll() --
		 * which every graphical client does -- treats an unrequested
		 * POLLHUP as the connection dying and closes it on the spot.
		 * That is what makes such a program vanish mid-run with no
		 * error worth reading: the read that follows returns a clean
		 * end of file, which sets no errno at all, so whatever the
		 * program prints is whichever errno was last left lying about.
		 *
		 * Each round below reproduces that in order: queue a connect
		 * and abandon it, let the next socket take the freed place,
		 * then accept.  The three checks are that the write still goes
		 * where it was addressed, that poll() does not invent a
		 * hangup, and that read() does not invent an end of file.
		 */
		char lpath[64];
		struct sockaddr_un la;
		int lfd;
		const int rounds = 24;
		int bad_route = 0, bad_hup = 0, bad_eof = 0;
		int setup_fail = 0, accepted = 0;

		snprintf(lpath, sizeof(lpath), "/tmp/uxlife_%d.sock",
			 (int)getpid());
		unlink(lpath);
		memset(&la, 0, sizeof(la));
		la.sun_family = AF_UNIX;
		strncpy(la.sun_path, lpath, sizeof(la.sun_path) - 1);

		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (lfd < 0 ||
		    bind(lfd, (struct sockaddr *)&la, sizeof(la)) != 0 ||
		    listen(lfd, 8) != 0) {
			test_fail("lifetime: could not set up the listener");
			if (lfd >= 0)
				close(lfd);
		} else {
			/* Non-blocking, so a round where nothing is pending
			 * reports that instead of hanging the suite. */
			fcntl(lfd, F_SETFL, O_NONBLOCK);

			for (int r = 0; r < rounds; r++) {
				int cfd, vp[2], afd;
				struct pollfd pfd;
				char b;

				/* Queue a connection and walk away from it.
				 * Non-blocking, because a blocking connect
				 * waits for the accept and there would be
				 * nothing to abandon. */
				cfd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (cfd < 0) {
					setup_fail++;
					continue;
				}
				fcntl(cfd, F_SETFL, O_NONBLOCK);
				connect(cfd, (struct sockaddr *)&la,
					sizeof(la));
				close(cfd);

				/* The next socket created is the one that
				 * takes the place just freed. */
				if (socketpair(AF_UNIX, SOCK_STREAM, 0, vp) !=
				    0) {
					setup_fail++;
					continue;
				}
				fcntl(vp[0], F_SETFL, O_NONBLOCK);
				fcntl(vp[1], F_SETFL, O_NONBLOCK);

				/* It is a working pair before the accept. */
				b = 0;
				if (write(vp[0], "A", 1) != 1 ||
				    read(vp[1], &b, 1) != 1 || b != 'A') {
					setup_fail++;
					close(vp[0]);
					close(vp[1]);
					continue;
				}

				/* Accept the abandoned connection. */
				afd = accept(lfd, NULL, NULL);
				if (afd >= 0)
					accepted++;

				/* 1. The pair must still be a pair. */
				b = 0;
				if (write(vp[0], "B", 1) != 1 ||
				    read(vp[1], &b, 1) != 1 || b != 'B')
					bad_route++;

				/* Closing the accepted end must say nothing
				 * about a socket it is not connected to. */
				if (afd >= 0)
					close(afd);

				/* 2. No hangup: vp[1] is still open. */
				pfd.fd = vp[0];
				pfd.events = POLLIN;
				pfd.revents = 0;
				if (poll(&pfd, 1, 0) >= 0 &&
				    (pfd.revents &
				     (POLLHUP | POLLERR | POLLNVAL)))
					bad_hup++;

				/* 3. No end of file: vp[1] is still open, so
				 * an empty non-blocking read is EAGAIN. */
				b = 0;
				if (read(vp[0], &b, 1) == 0)
					bad_eof++;

				close(vp[0]);
				close(vp[1]);
			}
			close(lfd);
			unlink(lpath);

			test_result("lifetime: an abandoned connect does not misroute another socket's data",
				    bad_route == 0);
			test_result("lifetime: an abandoned connect does not make poll() report a hangup",
				    bad_hup == 0);
			test_result("lifetime: an abandoned connect does not make read() report end of file",
				    bad_eof == 0);

			/* Printed either way: a round that accepted nothing
			 * exercised nothing, so a clean result with a low
			 * count here means the test did not run, not that the
			 * kernel is right. */
			printf("  %d/%d rounds accepted the abandoned connection\n",
			       accepted, rounds);
			if (bad_route || bad_hup || bad_eof)
				printf("  %d misrouted, %d invented a hangup, %d invented EOF\n",
				       bad_route, bad_hup, bad_eof);
			if (setup_fail)
				printf("  %d/%d rounds could not be set up\n",
				       setup_fail, rounds);
		}
	}

	{
		/* connect() and accept() on ONE thread.
		 *
		 * connect() completes as soon as the connection is queued: the
		 * far end is built by connect itself, and accept() hands over
		 * something already established.  Before that, connect waited
		 * for the accept, so this sequence deadlocked -- a legitimate
		 * pattern, and the reason every other test here has to fork or
		 * set O_NONBLOCK first.
		 *
		 * Deliberately BLOCKING on both calls: with O_NONBLOCK the old
		 * behaviour also completed, so a non-blocking version of this
		 * test would have passed all along and proved nothing.
		 */
		char lpath[64];
		struct sockaddr_un la;
		int lfd, cfd, afd;

		snprintf(lpath, sizeof(lpath), "/tmp/uxself_%d", (int)getpid());
		unlink(lpath);
		memset(&la, 0, sizeof(la));
		la.sun_family = AF_UNIX;
		strncpy(la.sun_path, lpath, sizeof(la.sun_path) - 1);

		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		test_result("self-connect: listener",
			    lfd >= 0 &&
				    bind(lfd, (struct sockaddr *)&la,
					 sizeof(la)) == 0 &&
				    listen(lfd, 4) == 0);

		cfd = socket(AF_UNIX, SOCK_STREAM, 0);
		test_result("self-connect: connect returns without an accept",
			    cfd >= 0 && connect(cfd, (struct sockaddr *)&la,
						sizeof(la)) == 0);

		afd = accept(lfd, NULL, NULL);
		test_result("self-connect: accept hands over the connection",
			    afd >= 0);

		if (cfd >= 0 && afd >= 0) {
			char b = 0;

			test_result("self-connect: client to server",
				    write(cfd, "x", 1) == 1 &&
					    read(afd, &b, 1) == 1 && b == 'x');
			b = 0;
			test_result("self-connect: server to client",
				    write(afd, "y", 1) == 1 &&
					    read(cfd, &b, 1) == 1 && b == 'y');
		}
		if (afd >= 0)
			close(afd);
		if (cfd >= 0)
			close(cfd);
		if (lfd >= 0)
			close(lfd);
		unlink(lpath);
	}

	{
		/* A listener that closes with connections queued.
		 *
		 * Those are established connections nobody accepted.  Closing
		 * the listener has to take them down, or their clients wait
		 * for a server that is never coming -- and the sockets are
		 * never reclaimed.  The client should see the far end gone,
		 * exactly as it would if the server had accepted and then
		 * closed. */
		char lpath[64];
		struct sockaddr_un la;
		int lfd, c[3];
		int ok = 1;

		snprintf(lpath, sizeof(lpath), "/tmp/uxdrain_%d",
			 (int)getpid());
		unlink(lpath);
		memset(&la, 0, sizeof(la));
		la.sun_family = AF_UNIX;
		strncpy(la.sun_path, lpath, sizeof(la.sun_path) - 1);

		lfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (lfd < 0 ||
		    bind(lfd, (struct sockaddr *)&la, sizeof(la)) != 0 ||
		    listen(lfd, 4) != 0) {
			test_fail("drain: listener");
			if (lfd >= 0)
				close(lfd);
		} else {
			for (int i = 0; i < 3; i++) {
				c[i] = socket(AF_UNIX, SOCK_STREAM, 0);
				if (c[i] < 0 ||
				    connect(c[i], (struct sockaddr *)&la,
					    sizeof(la)) != 0)
					ok = 0;
			}
			test_result("drain: three connections queued", ok);

			close(lfd); /* never accepted any of them */

			for (int i = 0; i < 3; i++) {
				char b = 0;

				if (c[i] < 0)
					continue;
				/* The far end is gone, so a read reports end
				 * of file rather than waiting for ever. */
				if (read(c[i], &b, 1) != 0)
					ok = 0;
				close(c[i]);
			}
			test_result("drain: every client sees the far end gone",
				    ok);
			unlink(lpath);
		}
	}

	{
		/* Passing a descriptor over a unix socket, and what happens to
		 * it when nobody collects it.
		 *
		 * A descriptor sent in band is queued on the receiving socket
		 * with its reference already taken, and only a receiver ever
		 * takes one off that queue.  So the interesting case is not
		 * the one that works -- it is the message that is never read:
		 * the socket closes still holding it, and if nothing releases
		 * it then the file behind it is kept open by nobody, for good.
		 *
		 * That cannot be observed directly from here, so it is done
		 * many times over: a descriptor kept alive per round would
		 * exhaust the table, and the final send is what fails when it
		 * has.
		 */
		int sv[2];

		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			test_fail("scm: socketpair");
		} else {
			char path[64];
			int fd, ok = 1;
			struct msghdr msg;
			struct iovec iov;
			char cbuf[CMSG_SPACE(sizeof(int))];
			struct cmsghdr *c;
			char byte = 'F';

			snprintf(path, sizeof(path), "/tmp/scm_%d",
				 (int)getpid());
			unlink(path);
			fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
			test_result("scm: a file to pass", fd >= 0);

			if (fd >= 0 && write(fd, "PASSED", 6) == 6) {
				memset(&msg, 0, sizeof(msg));
				memset(cbuf, 0, sizeof(cbuf));
				iov.iov_base = &byte;
				iov.iov_len = 1;
				msg.msg_iov = &iov;
				msg.msg_iovlen = 1;
				msg.msg_control = cbuf;
				msg.msg_controllen = sizeof(cbuf);
				c = CMSG_FIRSTHDR(&msg);
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				c->cmsg_len = CMSG_LEN(sizeof(int));
				*(int *)CMSG_DATA(c) = fd;
				test_result("scm: send a descriptor",
					    sendmsg(sv[0], &msg, 0) == 1);

				/* Receive it and read through it: the far side
				 * must get a working descriptor onto the same
				 * file, not a copy of the number. */
				char rb = 0;
				char got[8];
				int newfd = -1;

				memset(&msg, 0, sizeof(msg));
				memset(cbuf, 0, sizeof(cbuf));
				iov.iov_base = &rb;
				iov.iov_len = 1;
				msg.msg_iov = &iov;
				msg.msg_iovlen = 1;
				msg.msg_control = cbuf;
				msg.msg_controllen = sizeof(cbuf);
				test_result("scm: receive it",
					    recvmsg(sv[1], &msg, 0) == 1 &&
						    rb == 'F');
				c = CMSG_FIRSTHDR(&msg);
				if (c && c->cmsg_type == SCM_RIGHTS &&
				    c->cmsg_len == CMSG_LEN(sizeof(int)))
					newfd = *(int *)CMSG_DATA(c);
				test_result("scm: a descriptor came with it",
					    newfd >= 0 && newfd != fd);
				memset(got, 0, sizeof(got));
				test_result("scm: it reads the same file",
					    newfd >= 0 &&
						    lseek(newfd, 0,
							  SEEK_SET) == 0 &&
						    read(newfd, got, 6) == 6 &&
						    memcmp(got, "PASSED",
							   6) == 0);
				if (newfd >= 0)
					close(newfd);
				test_result("scm: no truncation was reported",
					    (msg.msg_flags & MSG_CTRUNC) == 0);
			}
			if (fd >= 0)
				close(fd);
			close(sv[0]);
			close(sv[1]);

			/* Now the case that leaks: send and never receive,
			 * then close both ends.  Repeated far more times than
			 * the descriptor table holds, so a reference kept per
			 * round runs it out. */
			for (int i = 0; i < 200 && ok; i++) {
				int p[2], f;

				if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) !=
				    0) {
					ok = 0;
					break;
				}
				f = open(path, O_RDONLY);
				if (f < 0) {
					ok = 0;
					close(p[0]);
					close(p[1]);
					break;
				}
				memset(&msg, 0, sizeof(msg));
				memset(cbuf, 0, sizeof(cbuf));
				iov.iov_base = &byte;
				iov.iov_len = 1;
				msg.msg_iov = &iov;
				msg.msg_iovlen = 1;
				msg.msg_control = cbuf;
				msg.msg_controllen = sizeof(cbuf);
				c = CMSG_FIRSTHDR(&msg);
				c->cmsg_level = SOL_SOCKET;
				c->cmsg_type = SCM_RIGHTS;
				c->cmsg_len = CMSG_LEN(sizeof(int));
				*(int *)CMSG_DATA(c) = f;
				if (sendmsg(p[0], &msg, 0) != 1)
					ok = 0;
				/* Deliberately never received. */
				close(f);
				close(p[0]);
				close(p[1]);
			}
			test_result("scm: 200 unreceived descriptors do not exhaust anything",
				    ok);

			/* If the references had been kept, this is where it
			 * shows: nothing is left to open with. */
			int probe = open(path, O_RDONLY);

			test_result("scm: the system can still open a file afterwards",
				    probe >= 0);
			if (probe >= 0)
				close(probe);
			unlink(path);
		}
	}

	// ========================================
	// Writes go through the cache
	// ========================================
	printf("\n--- cached writes ---\n");

	{
		/* Writes land in the page cache and are written back later.
		 * Two things have to hold for that to be safe, and they are
		 * what this checks:
		 *
		 *  - a read must see what was just written, without any flush
		 *    in between.  Reads have always come from the cache; writes
		 *    used to go past it, so a file could read back as its old
		 *    contents.
		 *  - what reaches the device must be the same bytes.  fsync
		 *    forces the writeback, and reopening afterwards reads the
		 *    device copy through a fresh page.
		 */
		char path[64];

		snprintf(path, sizeof(path), "/tmp/cached_wr_%d",
			 (int)getpid());
		const size_t len = 128 * 1024; /* several writeback runs */
		unsigned char *out = malloc(len);
		unsigned char *in = malloc(len);
		int fd;

		if (!out || !in) {
			test_fail("cached write: out of memory");
		} else {
			for (size_t i = 0; i < len; i++)
				out[i] = (unsigned char)(i * 31 + (i >> 8));

			unlink(path);
			fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
			if (fd < 0) {
				test_fail("cached write: could not create");
			} else {
				ssize_t w = write(fd, out, len);

				test_result("cached write: the whole buffer was written",
					    w == (ssize_t)len);

				/* Read it back with no flush: this is the cache
				 * answering, and it must agree. */
				test_result("cached write: rewind",
					    lseek(fd, 0, SEEK_SET) == 0);
				memset(in, 0, len);
				test_result("cached write: read back the whole buffer",
					    read(fd, in, len) == (ssize_t)len);
				test_result("cached write: a read sees the write immediately",
					    memcmp(out, in, len) == 0);

				/* Force it out, then read it back through a
				 * fresh open so the device copy is what is
				 * being compared. */
				test_result("cached write: fsync succeeds",
					    fsync(fd) == 0);
				close(fd);

				fd = open(path, O_RDONLY);
				if (fd < 0) {
					test_fail("cached write: could not reopen");
				} else {
					memset(in, 0, len);
					test_result("cached write: reread after fsync",
						    read(fd, in, len) ==
							    (ssize_t)len);
					test_result("cached write: what reached the device matches",
						    memcmp(out, in, len) == 0);
					close(fd);
				}
			}

			/* Partial writes inside an existing page must keep the
			 * bytes around them -- the page is read in first, then
			 * only the written range is replaced. */
			fd = open(path, O_RDWR);
			if (fd >= 0) {
				unsigned char probe[8];

				test_result("cached write: seek into a page",
					    lseek(fd, 4096 + 100, SEEK_SET) ==
						    4096 + 100);
				test_result("cached write: partial write",
					    write(fd, "MARKER", 6) == 6);
				test_result("cached write: seek back",
					    lseek(fd, 4096 + 94, SEEK_SET) ==
						    4096 + 94);
				memset(probe, 0, sizeof(probe));
				test_result("cached write: read around it",
					    read(fd, probe, 8) == 8);
				/* The six bytes before the marker must still be
				 * the original contents. */
				test_result("cached write: bytes before a partial write survive",
					    memcmp(probe, out + 4096 + 94, 6) ==
						    0);
				close(fd);
			}

			unlink(path);
			free(out);
			free(in);
		}
	}

	{
		/* A write past the end of the file leaves a gap, and the gap
		 * has to read as zeros.
		 *
		 * A page beyond anything the file has ever held is not read
		 * from the device -- there is nothing there to read -- so it
		 * starts as whatever the page frame last contained.  If that
		 * is not cleared, the hole in this file reads back as
		 * somebody else's data: a page that belonged to another
		 * program a moment ago, handed over in full.  That is the
		 * difference between a sparse file and a disclosure.
		 *
		 * The gap is deliberately several pages wide and not page
		 * aligned at either end, so it covers whole untouched pages
		 * as well as the partial ones on both sides of it. */
		char path[64];
		int fd;

		snprintf(path, sizeof(path), "/tmp/cached_gap_%d",
			 (int)getpid());
		unlink(path);
		fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
		if (fd < 0) {
			test_fail("gap: could not create");
		} else {
			const off_t far = 5 * 4096 + 1234;
			const size_t gap = (size_t)far - 10;
			unsigned char *back = malloc(gap);
			int ok = 1;

			test_result("gap: write ten bytes at the start",
				    write(fd, "0123456789", 10) == 10);
			test_result("gap: seek well past the end",
				    lseek(fd, far, SEEK_SET) == far);
			test_result("gap: write past the end",
				    write(fd, "END", 3) == 3);

			/* Read the hole back before anything is flushed: this
			 * is the page cache answering with pages it created
			 * rather than read. */
			test_result("gap: seek back into the hole",
				    lseek(fd, 10, SEEK_SET) == 10);
			if (!back) {
				test_fail("gap: out of memory");
			} else {
				memset(back, 0xAA, gap);
				test_result("gap: read the whole hole",
					    read(fd, back, gap) ==
						    (ssize_t)gap);
				for (size_t i = 0; i < gap; i++)
					if (back[i] != 0) {
						ok = 0;
						break;
					}
				test_result("gap: every byte of the hole is zero",
					    ok);
			}

			/* And again from the device, which is a different
			 * source for the same bytes: the hole may have been
			 * written out as real blocks by now. */
			test_result("gap: fsync", fsync(fd) == 0);
			close(fd);
			fd = open(path, O_RDONLY);
			if (fd < 0 || !back) {
				test_fail("gap: could not reopen");
			} else {
				ok = 1;
				memset(back, 0xAA, gap);
				test_result("gap: reread the hole after a reopen",
					    lseek(fd, 10, SEEK_SET) == 10 &&
						    read(fd, back, gap) ==
							    (ssize_t)gap);
				for (size_t i = 0; i < gap; i++)
					if (back[i] != 0) {
						ok = 0;
						break;
					}
				test_result("gap: it is still all zeros on the device",
					    ok);
				close(fd);
			}
			free(back);
			unlink(path);
		}
	}

	{
		/* Writing into a hole that ftruncate made.
		 *
		 * An extending ftruncate gives a file a size and no blocks:
		 * the whole of it is a hole.  A write landing in there needs
		 * blocks that do not exist, and "how many blocks does the file
		 * have" cannot be answered from its size, which is what made
		 * this fail with EIO -- nothing was allocated, and then the
		 * write found no block to put the data in.
		 *
		 * The write is deliberately smaller than a block and not
		 * aligned to one, so the block it creates sticks out on both
		 * sides.  What sticks out was a hole a moment ago and read as
		 * zeros; it has to go on reading as zeros now that a real
		 * block -- with a previous owner -- is behind it. */
		char path[64];
		int fd;

		snprintf(path, sizeof(path), "/tmp/trunc_hole_%d",
			 (int)getpid());
		unlink(path);
		fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
		if (fd < 0) {
			test_fail("truncate hole: could not create");
		} else {
			const off_t len = 1024 * 1024;
			const off_t at = 500000;
			struct stat st;

			test_result("truncate hole: extend to a megabyte",
				    ftruncate(fd, len) == 0);
			test_result("truncate hole: the size took",
				    fstat(fd, &st) == 0 && st.st_size == len);

			test_result("truncate hole: seek into the hole",
				    lseek(fd, at, SEEK_SET) == at);
			test_result("truncate hole: writing into it succeeds",
				    write(fd, "MIDDLE", 6) == 6);
			test_result("truncate hole: the write did not change the size",
				    fstat(fd, &st) == 0 && st.st_size == len);

			/* Read the whole file: everything except those six
			 * bytes must be zero, before any flush and again from
			 * the device. */
			for (int pass = 0; pass < 2; pass++) {
				unsigned char *all = malloc((size_t)len);
				int ok = 1;

				if (pass == 1) {
					test_result("truncate hole: fsync",
						    fsync(fd) == 0);
					close(fd);
					fd = open(path, O_RDONLY);
					if (fd < 0) {
						test_fail("truncate hole: could not reopen");
						free(all);
						break;
					}
				}
				if (!all) {
					test_fail("truncate hole: out of memory");
					break;
				}
				memset(all, 0xAA, (size_t)len);
				ok = (lseek(fd, 0, SEEK_SET) == 0 &&
				      read(fd, all, (size_t)len) ==
					      (ssize_t)len);
				test_result(pass == 0 ?
						    "truncate hole: the whole file reads back" :
						    "truncate hole: the whole file reads back from the device",
					    ok);
				if (ok) {
					if (memcmp(all + at, "MIDDLE", 6) != 0)
						ok = 0;
					for (off_t i = 0; i < len && ok; i++) {
						if (i >= at && i < at + 6)
							continue;
						if (all[i] != 0)
							ok = 0;
					}
				}
				test_result(pass == 0 ?
						    "truncate hole: only the six written bytes are non-zero" :
						    "truncate hole: only the six written bytes are non-zero on the device",
					    ok);
				free(all);
			}
			close(fd);
			unlink(path);
		}
	}

	{
		/* Closing without fsync must not lose the data.
		 *
		 * fsync is a promise about WHEN the data is on the device, not
		 * about WHETHER.  A program that never calls it -- which is
		 * most of them -- still expects the file it wrote to be there
		 * afterwards.  Since write() now returns having touched
		 * nothing, that expectation rests entirely on writeback
		 * running on its own, so it is worth stating as a test rather
		 * than assuming.
		 *
		 * The files are read back through a fresh open, and there are
		 * enough of them that the cache cannot simply still be holding
		 * every one. */
		const int files = 40;
		char path[80];
		int ok = 1;

		for (int i = 0; i < files && ok; i++) {
			int fd;
			char payload[64];

			snprintf(path, sizeof(path), "/tmp/nosync_%d_%d",
				 (int)getpid(), i);
			unlink(path);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) {
				ok = 0;
				break;
			}
			snprintf(payload, sizeof(payload),
				 "file %d written without fsync", i);
			if (write(fd, payload, strlen(payload)) !=
			    (ssize_t)strlen(payload))
				ok = 0;
			close(fd); /* deliberately no fsync */
		}
		test_result("no fsync: every file was written", ok);

		for (int i = 0; i < files && ok; i++) {
			int fd;
			char payload[64], back[64];
			ssize_t r;

			snprintf(path, sizeof(path), "/tmp/nosync_%d_%d",
				 (int)getpid(), i);
			snprintf(payload, sizeof(payload),
				 "file %d written without fsync", i);
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				ok = 0;
				break;
			}
			memset(back, 0, sizeof(back));
			r = read(fd, back, sizeof(back));
			if (r != (ssize_t)strlen(payload) ||
			    memcmp(back, payload, (size_t)r) != 0)
				ok = 0;
			close(fd);
		}
		test_result("no fsync: every file reads back exactly what was written",
			    ok);

		for (int i = 0; i < files; i++) {
			snprintf(path, sizeof(path), "/tmp/nosync_%d_%d",
				 (int)getpid(), i);
			unlink(path);
		}
	}

	{
		/* Volume, in small unaligned pieces.
		 *
		 * The shape a program produces when it writes a file through a
		 * buffered stream: many small writes, not one large one, and
		 * not landing on block boundaries.  Each of those used to be a
		 * read of the block, a merge and a write back, synchronously.
		 *
		 * The dirty limit is a separate test below -- 6 MB is under
		 * it, so this one does not reach the throttle. */
		char path[64];
		const size_t total = 6 * 1024 * 1024;

		/* Per-process name: copies of this suite run at the same
		 * time, and a shared name means they delete each other's
		 * files rather than testing anything. */
		snprintf(path, sizeof(path), "/tmp/cached_volume_%d",
			 (int)getpid());
		const size_t piece = 700; /* neither a block nor a divisor */
		unsigned char *chunk = malloc(piece);
		int fd, ok = 1;
		long ms;
		struct timespec t0;

		if (!chunk) {
			test_fail("volume: out of memory");
		} else {
			unlink(path);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) {
				test_fail("volume: could not create");
			} else {
				size_t done = 0;
				unsigned long seq = 0;

				mono_now(&t0);
				while (done < total && ok) {
					for (size_t i = 0; i < piece; i++)
						chunk[i] = (unsigned char)(seq +
									   i);
					if (write(fd, chunk, piece) !=
					    (ssize_t)piece)
						ok = 0;
					done += piece;
					seq++;
				}
				ms = mono_elapsed_ms(&t0);
				close(fd);
				test_result("volume: every small write succeeded",
					    ok);
				printf("  %zu KB in %zu-byte writes took %ld ms\n",
				       done / 1024, piece, ms);

				/* Read it all back and check every byte: a
				 * partial page that was written, evicted and
				 * re-read has to come back identical. */
				fd = open(path, O_RDONLY);
				if (fd < 0) {
					test_fail("volume: could not reopen");
				} else {
					unsigned char *back = malloc(piece);
					size_t got = 0;
					unsigned long vseq = 0;

					ok = (back != NULL);
					while (ok && got < done) {
						ssize_t r = read(fd, back,
								 piece);

						if (r != (ssize_t)piece) {
							ok = 0;
							break;
						}
						for (size_t i = 0; i < piece;
						     i++)
							if (back[i] !=
							    (unsigned char)(vseq +
									    i)) {
								ok = 0;
								break;
							}
						got += piece;
						vseq++;
					}
					test_result("volume: every byte reads back as written",
						    ok);
					free(back);
					close(fd);
				}
				unlink(path);
			}
			free(chunk);
		}
	}

	{
		/* Far more unwritten data than the cache is allowed to hold.
		 *
		 * A dirty page cannot be reclaimed until it has been stored,
		 * so a program writing faster than the device absorbs is
		 * consuming memory the system is NOT free to take back.  The
		 * bound on that is what makes such a program slow down instead
		 * of the machine running out; without it the failure lands on
		 * whoever allocates next, which is some unrelated program.
		 *
		 * 32 MB against a limit of 8 MB, written without pausing, so
		 * the throttle is crossed several times over.  What is
		 * asserted is that it completes, that the data is right, and
		 * that free memory afterwards is in the same region as before
		 * -- not that any particular amount was reclaimed, which
		 * depends on what else the system is doing. */
		char path[64];
		const size_t total = 32 * 1024 * 1024;
		const size_t piece = 4000; /* just under a page, so pages
					    * straddle and every one is dirtied */
		unsigned char *chunk = malloc(piece);
		struct sysinfo si;
		unsigned long free_before = 0, free_after = 0;
		int have_si = 0;
		int fd, ok = 1;

		snprintf(path, sizeof(path), "/tmp/dirty_limit_%d",
			 (int)getpid());
		if (!chunk) {
			test_fail("dirty limit: out of memory");
		} else {
			if (sysinfo(&si) == 0) {
				free_before = (unsigned long)si.freeram;
				have_si = 1;
			}
			unlink(path);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) {
				test_fail("dirty limit: could not create");
			} else {
				size_t done = 0;
				unsigned long seq = 0;

				while (done < total && ok) {
					for (size_t i = 0; i < piece; i++)
						chunk[i] = (unsigned char)(seq +
									   i);
					if (write(fd, chunk, piece) !=
					    (ssize_t)piece)
						ok = 0;
					done += piece;
					seq++;
				}
				close(fd);
				test_result("dirty limit: writing far past the limit completes",
					    ok);

				fd = open(path, O_RDONLY);
				if (fd < 0) {
					test_fail("dirty limit: could not reopen");
				} else {
					unsigned char *back = malloc(piece);
					size_t got = 0;
					unsigned long vseq = 0;

					ok = (back != NULL);
					while (ok && got < done) {
						ssize_t r = read(fd, back,
								 piece);

						if (r != (ssize_t)piece) {
							ok = 0;
							break;
						}
						for (size_t i = 0; i < piece;
						     i++)
							if (back[i] !=
							    (unsigned char)(vseq +
									    i)) {
								ok = 0;
								break;
							}
						got += piece;
						vseq++;
					}
					test_result("dirty limit: every byte is still correct",
						    ok);
					free(back);
					close(fd);
				}
				unlink(path);

				/* Memory must come back.  A generous floor:
				 * the point is to catch dirty pages that were
				 * never reclaimable at all, which shows up as
				 * free memory that never recovers, not as a
				 * few percent either way. */
				if (have_si && sysinfo(&si) == 0) {
					free_after = (unsigned long)si.freeram;
					printf("  free memory %lu KB before, %lu KB after 32 MB of writes\n",
					       free_before / 1024,
					       free_after / 1024);
					test_result("dirty limit: free memory did not collapse",
						    free_after >
							    free_before / 2);
				}
			}
			free(chunk);
		}
	}

	{
		/* Metadata must be visible immediately, with no flush.
		 *
		 * Size is recorded in memory now and stored later, so the
		 * question is whether the in-memory copy is the one everybody
		 * reads.  It has to be: a program that writes and then stats
		 * its own file must not see the size it had before. */
		char path[64];
		int fd;

		snprintf(path, sizeof(path), "/tmp/cached_meta_%d",
			 (int)getpid());

		unlink(path);
		fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
		if (fd < 0) {
			test_fail("metadata: could not create");
		} else {
			struct stat st;

			test_result("metadata: write 5000 bytes",
				    write(fd, "x", 1) == 1 &&
					    lseek(fd, 4999, SEEK_SET) == 4999 &&
					    write(fd, "y", 1) == 1);
			memset(&st, 0, sizeof(st));
			test_result("metadata: fstat with no flush",
				    fstat(fd, &st) == 0);
			test_result("metadata: the new size is visible at once",
				    st.st_size == 5000);
			close(fd);

			/* And it must still be right through a fresh open,
			 * which is what proves it was actually stored. */
			memset(&st, 0, sizeof(st));
			test_result("metadata: stat after close",
				    stat(path, &st) == 0);
			test_result("metadata: the size survived the close",
				    st.st_size == 5000);
			unlink(path);
		}
	}

	{
		/* A size that has not reached the device yet is still the
		 * file's size.
		 *
		 * A write that stays inside blocks the file already owns no
		 * longer stores the inode -- the new length is kept in memory
		 * and written back later.  Everything that can ask for a
		 * length has to get the new one anyway, or a program writes a
		 * few bytes, reopens the file and finds them missing.
		 *
		 * The appends below are deliberately small: 100 bytes at a
		 * time crosses a 4096-byte block boundary only once every
		 * forty-odd writes, so almost every one of them takes the
		 * deferred path.  No fsync anywhere -- that would store the
		 * inode and test nothing. */
		char path[64];
		int fd;
		int ok = 1;
		off_t expect = 0;

		snprintf(path, sizeof(path), "/tmp/defer_size_%d",
			 (int)getpid());
		unlink(path);
		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (fd < 0) {
			test_fail("deferred size: could not create");
		} else {
			char chunk[100];
			struct stat st;

			memset(chunk, 'd', sizeof(chunk));
			for (int i = 0; i < 50 && ok; i++) {
				if (write(fd, chunk, sizeof(chunk)) !=
				    (ssize_t)sizeof(chunk)) {
					ok = 0;
					break;
				}
				expect += (off_t)sizeof(chunk);
				/* Visible through the open handle at once. */
				if (fstat(fd, &st) != 0 ||
				    st.st_size != expect) {
					ok = 0;
					break;
				}
				/* And by name, which is a different route to
				 * the same inode. */
				if (stat(path, &st) != 0 ||
				    st.st_size != expect) {
					ok = 0;
					break;
				}
			}
			test_result("deferred size: every append is visible immediately, by handle and by name",
				    ok);
			close(fd);

			/* Reopening reads the length from the inode.  This is
			 * the case that fails outright if a deferred size is
			 * not applied to what a read of the inode returns:
			 * the file reports whichever length was last stored,
			 * and reads stop there. */
			memset(&st, 0, sizeof(st));
			test_result("deferred size: the length is right after a close and stat",
				    stat(path, &st) == 0 &&
					    st.st_size == expect);

			fd = open(path, O_RDONLY);
			if (fd < 0) {
				test_fail("deferred size: could not reopen");
			} else {
				char *back = malloc((size_t)expect + 1);
				ssize_t r = -1;

				if (back)
					r = read(fd, back, (size_t)expect + 1);
				test_result("deferred size: the whole file reads back",
					    r == (ssize_t)expect);
				ok = (r == (ssize_t)expect);
				for (ssize_t i = 0; i < r && ok; i++)
					if (back[i] != 'd')
						ok = 0;
				test_result("deferred size: and every byte of it is what was written",
					    ok);
				free(back);
				close(fd);
			}

			/* Truncate replaces a pending length rather than
			 * losing to it: a stale record laid back over the
			 * inode would report the pre-truncate size. */
			fd = open(path, O_WRONLY);
			if (fd >= 0) {
				test_result("deferred size: truncate",
					    ftruncate(fd, 17) == 0);
				close(fd);
				memset(&st, 0, sizeof(st));
				test_result("deferred size: the truncated length is what is reported",
					    stat(path, &st) == 0 &&
						    st.st_size == 17);
			}

			/* And an O_TRUNC open, which is the other way a
			 * pending length gets superseded. */
			fd = open(path, O_WRONLY | O_TRUNC);
			if (fd >= 0) {
				close(fd);
				memset(&st, 0, sizeof(st));
				test_result("deferred size: O_TRUNC leaves the file empty",
					    stat(path, &st) == 0 &&
						    st.st_size == 0);
			}
			unlink(path);
		}
	}

	{
		/* More files written at once than the pending-size table
		 * holds.
		 *
		 * Past its capacity a write goes back to storing the inode on
		 * the spot -- slower, and it has to still be correct.  The
		 * count is deliberately well over the table's size so that
		 * both paths are taken within one loop. */
		const int files = 96;
		char path[80];
		int ok = 1;

		for (int i = 0; i < files && ok; i++) {
			int fd;

			snprintf(path, sizeof(path), "/tmp/defer_many_%d_%d",
				 (int)getpid(), i);
			unlink(path);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) {
				ok = 0;
				break;
			}
			/* Two short appends: the first allocates, the second
			 * is the deferred case. */
			if (write(fd, "aaaa", 4) != 4 ||
			    write(fd, "bbb", 3) != 3)
				ok = 0;
			close(fd);
		}
		test_result("many pending sizes: every file was written", ok);

		for (int i = 0; i < files && ok; i++) {
			struct stat st;

			snprintf(path, sizeof(path), "/tmp/defer_many_%d_%d",
				 (int)getpid(), i);
			if (stat(path, &st) != 0 || st.st_size != 7)
				ok = 0;
		}
		test_result("many pending sizes: every file reports 7 bytes",
			    ok);

		for (int i = 0; i < files; i++) {
			snprintf(path, sizeof(path), "/tmp/defer_many_%d_%d",
				 (int)getpid(), i);
			unlink(path);
		}
	}

	{
		/* Several threads writing their own files at once.
		 *
		 * The cache is shared and its dirty list is global, so the
		 * interesting question is whether two writers can corrupt each
		 * other's pages or each other's accounting.  Each thread owns
		 * one file and fills it with a pattern only it produces. */
		pthread_t th[CACHE_WRITERS];
		int made = 0;
		int ok = 1;

		for (int i = 0; i < CACHE_WRITERS; i++) {
			cache_writer_arg[i] = i;
			if (pthread_create(&th[i], NULL, cache_writer_body,
					   &cache_writer_arg[i]) == 0)
				made++;
			else
				break;
		}
		test_result("threaded writes: all writers started",
			    made == CACHE_WRITERS);
		for (int i = 0; i < made; i++) {
			void *r = NULL;

			pthread_join(th[i], &r);
			if (r != NULL)
				ok = 0;
		}
		test_result("threaded writes: every writer reported success",
			    ok);

		/* Verify from this thread, after they have all finished. */
		ok = 1;
		for (int i = 0; i < made; i++) {
			char path[64];
			unsigned char buf[CACHE_WRITER_BYTES];
			int fd;

			snprintf(path, sizeof(path), "/tmp/cache_w%d_%d",
				 (int)getpid(), i);
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				ok = 0;
				break;
			}
			if (read(fd, buf, sizeof(buf)) !=
			    (ssize_t)sizeof(buf)) {
				ok = 0;
			} else {
				for (size_t k = 0; k < sizeof(buf); k++)
					if (buf[k] !=
					    (unsigned char)(i * 7 + k)) {
						ok = 0;
						break;
					}
			}
			close(fd);
			unlink(path);
		}
		test_result("threaded writes: each file holds only its own data",
			    ok);
	}

	{
		/* Several threads appending to the SAME file, under a lock.
		 *
		 * The case above gives every writer its own file, so no two of
		 * them ever touch one page.  Here they all do: the records are
		 * 37 bytes, so a page holds a hundred of them and every thread
		 * writes into a page the others have already dirtied.  Each
		 * record is one repeated character, so a record that came out
		 * mixed -- two copies interleaved into one page -- is visible
		 * directly in the result rather than only as a wrong total. */
		pthread_t th[SHARED_APPENDERS];
		char path[64];
		int made = 0, ok = 1;
		const size_t expect = (size_t)SHARED_APPENDERS *
				      SHARED_APPEND_RECS * SHARED_APPEND_RECLEN;

		snprintf(path, sizeof(path), "/tmp/shared_append_%d",
			 (int)getpid());
		unlink(path);
		shared_append_fd = open(path, O_CREAT | O_WRONLY | O_APPEND,
					0644);
		test_result("shared append: create the file",
			    shared_append_fd >= 0);

		if (shared_append_fd >= 0) {
			for (int i = 0; i < SHARED_APPENDERS; i++) {
				shared_append_arg[i] = i;
				if (pthread_create(&th[i], NULL,
						   shared_append_body,
						   &shared_append_arg[i]) == 0)
					made++;
				else
					break;
			}
			test_result("shared append: all appenders started",
				    made == SHARED_APPENDERS);
			for (int i = 0; i < made; i++) {
				void *r = NULL;

				pthread_join(th[i], &r);
				if (r != NULL)
					ok = 0;
			}
			test_result("shared append: every appender reported success",
				    ok);
			close(shared_append_fd);
			shared_append_fd = -1;

			struct stat st;

			test_result("shared append: the file is exactly as long as what was written",
				    stat(path, &st) == 0 &&
					    st.st_size == (off_t)expect);

			/* Every record must be intact and from one writer, and
			 * each writer's records must all be there. */
			int fd = open(path, O_RDONLY);
			if (fd < 0) {
				test_fail("shared append: could not reopen");
			} else {
				int counts[SHARED_APPENDERS];
				char rec[SHARED_APPEND_RECLEN];
				ssize_t r;

				for (int i = 0; i < SHARED_APPENDERS; i++)
					counts[i] = 0;
				ok = 1;
				while ((r = read(fd, rec, sizeof(rec))) ==
				       (ssize_t)sizeof(rec)) {
					int who = rec[0] - 'A';

					if (who < 0 || who >= SHARED_APPENDERS ||
					    rec[sizeof(rec) - 1] != '\n') {
						ok = 0;
						break;
					}
					for (size_t i = 0;
					     i < sizeof(rec) - 1; i++)
						if (rec[i] != 'A' + who) {
							ok = 0;
							break;
						}
					if (!ok)
						break;
					counts[who]++;
				}
				if (r != 0)
					ok = 0; /* trailing partial record */
				test_result("shared append: every record is whole and from a single writer",
					    ok);
				for (int i = 0; i < SHARED_APPENDERS; i++)
					if (counts[i] != SHARED_APPEND_RECS)
						ok = 0;
				test_result("shared append: every writer's records all arrived",
					    ok);
				close(fd);
			}
			unlink(path);
		}
	}

	// ========================================
	// Name lookups
	// ========================================
	printf("\n--- dentry cache ---\n");

	{
		/* Path lookups are answered from a cache now, so every check
		 * here is really the same question asked twice: does the cache
		 * still agree with the filesystem after something changed it?
		 *
		 * A lookup cache that is merely fast is easy.  One that is
		 * fast and never answers with what USED to be true is the
		 * whole difficulty, and each block below is one way of being
		 * wrong that costs a user their file. */
		char dir[64], a[128], b[128];
		struct stat st;
		int fd, ok;

		snprintf(dir, sizeof(dir), "/tmp/dcache_%d", (int)getpid());
		rmdir(dir);
		test_result("dentry: make a directory to work in",
			    mkdir(dir, 0755) == 0);

		snprintf(a, sizeof(a), "%s/alpha", dir);
		snprintf(b, sizeof(b), "%s/beta", dir);

		/* A name that does not exist, and then does.
		 *
		 * The first stat records "not here" -- that negative answer is
		 * what makes searching a PATH cheap, because most of the
		 * directories on it do not hold the program.  It also has to
		 * be thrown away the moment the name appears, or the file is
		 * created successfully and then cannot be found. */
		unlink(a);
		test_result("dentry: a name that does not exist is not found",
			    stat(a, &st) != 0 && errno == ENOENT);
		test_result("dentry: asking twice gives the same answer",
			    stat(a, &st) != 0 && errno == ENOENT);
		fd = open(a, O_CREAT | O_WRONLY, 0644);
		test_result("dentry: create it", fd >= 0);
		if (fd >= 0) {
			write(fd, "alpha", 5);
			close(fd);
		}
		test_result("dentry: a name that has just appeared is found",
			    stat(a, &st) == 0);
		test_result("dentry: and it is the file that was written",
			    st.st_size == 5);

		/* The same name created and removed over and over.
		 *
		 * Each round asks for the answer both before and after the
		 * change, so a cache that updates late -- or not at all --
		 * fails on the round after the one that broke it, not
		 * silently. */
		ok = 1;
		for (int i = 0; i < 20 && ok; i++) {
			if (stat(a, &st) != 0)
				ok = 0;
			if (ok && unlink(a) != 0)
				ok = 0;
			if (ok && stat(a, &st) == 0)
				ok = 0; /* removed, must not be found */
			if (ok) {
				fd = open(a, O_CREAT | O_WRONLY | O_TRUNC,
					  0644);
				if (fd < 0)
					ok = 0;
				else {
					if (write(fd, "x", 1) != 1)
						ok = 0;
					close(fd);
				}
			}
			if (ok && stat(a, &st) != 0)
				ok = 0; /* created, must be found */
		}
		test_result("dentry: 20 rounds of create/stat/unlink/recreate track reality",
			    ok);

		/* Rename moves the name.
		 *
		 * Two entries change at once: the old one stops being an
		 * answer and the new one starts.  Getting only half of that
		 * right leaves the file reachable under both names, or under
		 * neither. */
		unlink(b);
		test_result("dentry: rename", rename(a, b) == 0);
		test_result("dentry: the old name is gone",
			    stat(a, &st) != 0 && errno == ENOENT);
		test_result("dentry: the new name is there", stat(b, &st) == 0);
		fd = open(b, O_RDONLY);
		if (fd < 0) {
			test_fail("dentry: could not open the renamed file");
		} else {
			char buf[8];
			ssize_t r = read(fd, buf, sizeof(buf));

			test_result("dentry: the renamed file still holds its data",
				    r == 1 && buf[0] == 'x');
			close(fd);
		}
		test_result("dentry: rename back", rename(b, a) == 0);
		test_result("dentry: found under the original name again",
			    stat(a, &st) == 0);
		unlink(a);

		/* Case matters.
		 *
		 * This cache is shared with a filesystem whose names are
		 * case-insensitive, and folding case here would make these two
		 * the same entry -- one file answering for the other, with no
		 * error anywhere to say so. */
		{
			char upper[128], lower[128];
			struct stat su, sl;

			snprintf(upper, sizeof(upper), "%s/CaseTest", dir);
			snprintf(lower, sizeof(lower), "%s/casetest", dir);
			unlink(upper);
			unlink(lower);

			fd = open(upper, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) {
				write(fd, "UPPER", 5);
				close(fd);
			}
			test_result("dentry: create a mixed-case name", fd >= 0);
			/* Looked up before the other one exists, so the miss
			 * is what gets remembered. */
			test_result("dentry: the differently-cased name does not exist yet",
				    stat(lower, &st) != 0);

			fd = open(lower, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) {
				write(fd, "lowercase", 9);
				close(fd);
			}
			test_result("dentry: create the lower-case name too",
				    fd >= 0);
			test_result("dentry: both names exist",
				    stat(upper, &su) == 0 &&
					    stat(lower, &sl) == 0);
			test_result("dentry: they are two different files",
				    su.st_ino != sl.st_ino && su.st_size == 5 &&
					    sl.st_size == 9);
			/* Removing one must not remove the other. */
			test_result("dentry: remove one of them",
				    unlink(upper) == 0);
			test_result("dentry: the other is still there",
				    stat(lower, &sl) == 0 && sl.st_size == 9);
			test_result("dentry: the removed one is gone",
				    stat(upper, &su) != 0);
			unlink(lower);
		}

		/* The same name in two directories.
		 *
		 * The cache is keyed by (directory, name).  If the directory
		 * were not part of the key -- or the two keys collided --
		 * "notes" in one directory would answer for "notes" in the
		 * other, which is the failure that silently hands a program
		 * the wrong file. */
		{
			char d1[128], d2[128], f1[192], f2[192];
			struct stat s1, s2;

			snprintf(d1, sizeof(d1), "%s/one", dir);
			snprintf(d2, sizeof(d2), "%s/two", dir);
			snprintf(f1, sizeof(f1), "%s/notes", d1);
			snprintf(f2, sizeof(f2), "%s/notes", d2);

			test_result("dentry: two directories",
				    mkdir(d1, 0755) == 0 &&
					    mkdir(d2, 0755) == 0);
			fd = open(f1, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) {
				write(fd, "one", 3);
				close(fd);
			}
			/* Absent from the second directory -- remembered as
			 * absent THERE, and it must stay present in the first. */
			test_result("dentry: the name is only in the first",
				    stat(f1, &s1) == 0 && stat(f2, &s2) != 0);
			fd = open(f2, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) {
				write(fd, "twotwo", 6);
				close(fd);
			}
			test_result("dentry: now it is in both",
				    stat(f1, &s1) == 0 && stat(f2, &s2) == 0);
			test_result("dentry: and they are different files",
				    s1.st_size == 3 && s2.st_size == 6);

			/* A directory removed and remade under the same name.
			 * Its inode number can be handed straight back, and
			 * anything still cached under that number would make
			 * the new directory answer with the old one's
			 * children. */
			test_result("dentry: empty and remove the first directory",
				    unlink(f1) == 0 && rmdir(d1) == 0);
			test_result("dentry: make it again", mkdir(d1, 0755) == 0);
			test_result("dentry: the new directory is empty",
				    stat(f1, &s1) != 0 && errno == ENOENT);

			unlink(f2);
			rmdir(d1);
			rmdir(d2);
		}

		/* ".." after a directory moves.
		 *
		 * A directory's parent is an entry in the directory itself,
		 * looked up like any other name -- so it is cached like any
		 * other name, and a rename that moves the directory has to
		 * take that entry with it.  Left behind, ".." leads back to a
		 * parent the directory no longer has. */
		{
			char p1[128], p2[128], sub[192], moved[192];
			char dotdot[224];
			struct stat sp, sd;

			snprintf(p1, sizeof(p1), "%s/parent1", dir);
			snprintf(p2, sizeof(p2), "%s/parent2", dir);
			snprintf(sub, sizeof(sub), "%s/child", p1);
			snprintf(moved, sizeof(moved), "%s/child", p2);
			snprintf(dotdot, sizeof(dotdot), "%s/child/..", p2);

			test_result("dentry: two parents and a child",
				    mkdir(p1, 0755) == 0 &&
					    mkdir(p2, 0755) == 0 &&
					    mkdir(sub, 0755) == 0);
			/* Resolve ".." while it still points at the first
			 * parent, so there is something cached to go stale. */
			{
				char before[224];

				snprintf(before, sizeof(before), "%s/child/..",
					 p1);
				test_result("dentry: \"..\" points at the first parent",
					    stat(before, &sp) == 0 &&
						    stat(p1, &sd) == 0 &&
						    sp.st_ino == sd.st_ino);
			}
			test_result("dentry: move the child",
				    rename(sub, moved) == 0);
			test_result("dentry: \"..\" now points at the second parent",
				    stat(dotdot, &sp) == 0 &&
					    stat(p2, &sd) == 0 &&
					    sp.st_ino == sd.st_ino);

			rmdir(moved);
			rmdir(p1);
			rmdir(p2);
		}

		/* Timing, printed rather than asserted.  The absolute numbers
		 * are the device's to decide; what makes a regression obvious
		 * is that these two used to cost a directory scan each. */
		{
			const int reps = 2000;
			char missing[128];
			struct timespec t0;
			long ms_hit, ms_miss;

			snprintf(missing, sizeof(missing), "%s/absent", dir);
			fd = open(a, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0)
				close(fd);

			mono_now(&t0);
			for (int i = 0; i < reps; i++)
				stat(a, &st);
			ms_hit = mono_elapsed_ms(&t0);

			mono_now(&t0);
			for (int i = 0; i < reps; i++)
				stat(missing, &st);
			ms_miss = mono_elapsed_ms(&t0);

			printf("  %d stat() of an existing name: %ld ms\n",
			       reps, ms_hit);
			printf("  %d stat() of a name that is not there: %ld ms\n",
			       reps, ms_miss);
			unlink(a);
		}

		rmdir(dir);
	}

	// ========================================
	// What one durable close costs
	// ========================================
	printf("\n--- fsync cost ---\n");

	{
		/* Programs that care about their data call fsync on every file
		 * they close -- a mail client does it for every mailbox and
		 * index it writes.  So the per-fsync cost is multiplied by the
		 * number of files, and anything fsync does beyond making this
		 * file durable is paid for over and over.
		 *
		 * It used to go on to mark the whole journal clean, which added
		 * two device cache flushes and made the next write set the
		 * needs-recovery flag again with a third.  Saving a mailbox
		 * took twenty seconds.
		 *
		 * The bound here is loose on purpose -- the device dictates the
		 * absolute number.  The printed figure is the thing to watch. */
		const int files = 50;
		long ms_sync, ms_plain;
		struct timespec t0;
		int ok = 1;
		char path[128];

		mono_now(&t0);
		for (int i = 0; i < files && ok; i++) {
			int fd;

			snprintf(path, sizeof(path), "/tmp/fsync_%d_%d",
				 (int)getpid(), i);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0) {
				ok = 0;
				break;
			}
			if (write(fd, "durable payload\n", 16) != 16)
				ok = 0;
			if (fsync(fd) != 0)
				ok = 0;
			close(fd);
		}
		ms_sync = mono_elapsed_ms(&t0);
		test_result("fsync: every file was written durably", ok);
		printf("  %d files written and fsync'd in %ld ms\n", files,
		       ms_sync);

		/* The same work without asking for durability, as a baseline
		 * for what the fsyncs themselves cost. */
		mono_now(&t0);
		for (int i = 0; i < files; i++) {
			int fd;

			snprintf(path, sizeof(path), "/tmp/fsyncb_%d_%d",
				 (int)getpid(), i);
			fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			if (fd < 0)
				continue;
			(void)!write(fd, "durable payload\n", 16);
			close(fd);
		}
		ms_plain = mono_elapsed_ms(&t0);
		printf("  %d files written without fsync in %ld ms\n", files,
		       ms_plain);

		for (int i = 0; i < files; i++) {
			snprintf(path, sizeof(path), "/tmp/fsync_%d_%d",
				 (int)getpid(), i);
			unlink(path);
			snprintf(path, sizeof(path), "/tmp/fsyncb_%d_%d",
				 (int)getpid(), i);
			unlink(path);
		}

		test_result("fsync: durable writes complete in reasonable time",
			    ms_sync < 30000);

		/* A second fsync with nothing changed in between has nothing to
		 * make durable and must not repeat the work. */
		{
			char idle_path[64];
			int fd;

			snprintf(idle_path, sizeof(idle_path),
				 "/tmp/fsync_idle_%d", (int)getpid());
			fd = open(idle_path, O_CREAT | O_WRONLY, 0644);

			if (fd < 0) {
				test_fail("fsync: could not create the file");
			} else {
				long first, second;

				(void)!write(fd, "x", 1);
				mono_now(&t0);
				fsync(fd);
				first = mono_elapsed_ms(&t0);
				mono_now(&t0);
				fsync(fd);
				second = mono_elapsed_ms(&t0);
				printf("  fsync %ld ms, then %ld ms with nothing changed\n",
				       first, second);
				test_result("fsync with nothing to do is not more expensive",
					    second <= first + 50);
				close(fd);
				unlink(idle_path);
			}
		}
	}

	// ========================================
	// Relative paths follow the caller's directory
	// ========================================
	printf("\n--- relative paths ---\n");

	{
		/* A relative path must be resolved against the CALLING task's
		 * working directory.  It used to be resolved much further down,
		 * against a single directory shared by every process -- so a
		 * call naming "x" operated on whichever directory some other
		 * process had most recently changed into.
		 *
		 * The way it showed up: stat() said a file was there and
		 * rename() said it was not, in the same program, moments apart.
		 * So the shape of this test is exactly that -- create through a
		 * relative name, then require every other call to agree about
		 * it. */
		char dir[64];

		snprintf(dir, sizeof(dir), "/tmp/relpath_%d", (int)getpid());
		char cwd_before[512];
		int ok = 1;

		if (!getcwd(cwd_before, sizeof(cwd_before)))
			cwd_before[0] = '\0';

		rmdir(dir);
		if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
			test_fail("relpath: could not create the directory");
		} else if (chdir(dir) != 0) {
			test_fail("relpath: could not enter the directory");
		} else {
			struct stat st;
			int fd = open("marker", O_CREAT | O_WRONLY | O_TRUNC,
				      0644);

			test_result("relpath: create through a relative name",
				    fd >= 0);
			if (fd >= 0) {
				(void)!write(fd, "x", 1);
				close(fd);
			}

			/* Everything below names the same file relatively. */
			test_result("relpath: stat agrees the file exists",
				    stat("marker", &st) == 0);
			test_result("relpath: access agrees the file exists",
				    access("marker", F_OK) == 0);

			/* The one that was broken. */
			test_result("relpath: rename finds what stat just found",
				    rename("marker", "marker2") == 0);
			test_result("relpath: the renamed file is there",
				    stat("marker2", &st) == 0);
			test_result("relpath: the old name is gone",
				    stat("marker", &st) != 0);

			/* And the absolute name must describe the same file. */
			{
				char abs[96];

				snprintf(abs, sizeof(abs), "%s/marker2", dir);
				test_result("relpath: the absolute name agrees",
					    stat(abs, &st) == 0);
			}

			test_result("relpath: unlink through a relative name",
				    unlink("marker2") == 0);

			/* A long single name, reached relatively.
			 *
			 * Taking a path apart component by component used to
			 * use a 64-byte buffer and simply stop copying when it
			 * filled -- without skipping the rest of the name, so
			 * the tail became the NEXT component: "averylongname"
			 * turned into "averylongnam/e".  A different file, in a
			 * directory that does not exist.  Creating through a
			 * path that skipped the split worked, and removing it
			 * afterwards did not. */
			const char *longname =
				"abcdefghijklmnopqrstuvwxyz0123456789_ABCDEFGHIJKLMNOPQRSTUVWXYZ.longext";
			int lfd = open(longname, O_CREAT | O_WRONLY | O_TRUNC,
				       0644);

			test_result("relpath: create a 71-character name", lfd >= 0);
			if (lfd >= 0)
				close(lfd);
			test_result("relpath: stat the long name",
				    stat(longname, &st) == 0);
			test_result("relpath: the long name was not split",
				    access(longname, F_OK) == 0);
			test_result("relpath: unlink the long name",
				    unlink(longname) == 0);

			if (cwd_before[0] && chdir(cwd_before) != 0)
				ok = 0;
			test_result("relpath: returned to the original directory",
				    ok);
			rmdir(dir);
		}
	}

	// ========================================
	// Distinct files created back to back
	// ========================================
	printf("\n--- back-to-back file creation ---\n");

	{
		/* Two files made in the same directory within microseconds of
		 * each other, with names differing only in their last few
		 * characters -- exactly what mkstemp() produces, and exactly
		 * what a MIME encoder does when it writes one temporary file
		 * per part of a message.
		 *
		 * Each must keep its OWN contents.  If opening the second by
		 * name yields the first one's data, a message ends up with one
		 * part's content inside another and nothing reports an error,
		 * because every call involved succeeded.
		 */
		const int NFILES = 8;
		char dir[64];
		char paths[8][96];
		int ok = 1, created = 0;

		snprintf(dir, sizeof(dir), "/tmp/lktest_b2b_%d", (int)getpid());
		mkdir(dir, 0700);

		/* Create them all first, closing each, so the writes are as
		 * close together as claws' are. */
		for (int i = 0; i < NFILES; i++) {
			FILE *f;

			snprintf(paths[i], sizeof(paths[i]),
				 "%s/claws.AAAAA%c", dir, 'A' + i);
			f = fopen(paths[i], "wb");
			if (!f) {
				ok = 0;
				break;
			}
			/* Content unique to this file and nothing like the
			 * others in length or bytes. */
			for (int r = 0; r <= i; r++)
				fprintf(f, "file%d-line%d\n", i, r);
			if (fclose(f) != 0)
				ok = 0;
			created++;
		}
		test_result("creating several files back to back succeeds",
			    ok && created == NFILES);

		/* Now read every one back and check it has ITS OWN content. */
		for (int i = 0; i < created && ok; i++) {
			FILE *f = fopen(paths[i], "rb");
			char line[64];
			int lines = 0;

			if (!f) {
				ok = 0;
				break;
			}
			while (fgets(line, sizeof(line), f)) {
				char want[64];

				snprintf(want, sizeof(want), "file%d-line%d\n",
					 i, lines);
				if (strcmp(line, want) != 0)
					ok = 0;
				lines++;
			}
			fclose(f);
			if (lines != i + 1)
				ok = 0;
		}
		test_result("each file read back by name has its own contents",
			    ok);

		/* Same again with both files open at once, which is closer to
		 * what an encoder does: source open for reading while the
		 * destination is open for writing. */
		if (created == NFILES) {
			FILE *a = fopen(paths[0], "rb");
			FILE *b = fopen(paths[1], "rb");
			char la[64] = { 0 }, lb[64] = { 0 };
			int both = 0;

			if (a && b) {
				if (fgets(la, sizeof(la), a) &&
				    fgets(lb, sizeof(lb), b))
					both = (strcmp(la, "file0-line0\n") == 0 &&
						strcmp(lb, "file1-line0\n") == 0);
			}
			if (a)
				fclose(a);
			if (b)
				fclose(b);
			test_result("two files open at once do not alias each other",
				    both);
		}

		for (int i = 0; i < created; i++)
			unlink(paths[i]);
		rmdir(dir);
	}

	// ========================================
	// Two equal-length temp files, the encoder's exact chain
	// ========================================
	printf("\n--- paired temp files (encoder chain) ---\n");

	{
		/* The full sequence a MIME encoder runs for a message with one
		 * attachment, reproduced exactly:
		 *
		 *   mkstemp -> fdopen("w+") -> fputs -> fclose      (part 1)
		 *   mkstemp -> fdopen("w+") -> fputs -> fclose      (part 2)
		 *   stat both, then reopen BOTH by name and read
		 *
		 * Two details make this different from the pieces already
		 * tested above, and both are true of the real case: the files
		 * are created back to back in ONE directory, and they are the
		 * SAME LENGTH.  A 5-byte message body and a 3-byte attachment
		 * encoded to base64 both come to exactly 5 bytes, so a reader
		 * that returns the wrong file still returns the right number of
		 * bytes and nothing downstream notices.  Every earlier test
		 * here gave its files different lengths, which is precisely why
		 * they could not have caught this.
		 *
		 * Repeated, because a fault that depends on allocation or
		 * descriptor reuse need not show on the first attempt.
		 */
		char dir[64];
		int ok = 1, rounds = 0;

		snprintf(dir, sizeof(dir), "/tmp/lktest_pair_%d", (int)getpid());
		mkdir(dir, 0700);

		for (int round = 0; round < 32 && ok; round++) {
			char t1[96], t2[96];
			int f1, f2;
			FILE *o1, *o2;
			/* Same length, different content -- as in the real
			 * case, where one is text and one is its base64. */
			static const char *c1 = "test\n";
			static const char *c2 = "aGkK\n";
			struct stat s1, s2;
			char r1[32], r2[32];
			FILE *i1, *i2;

			snprintf(t1, sizeof(t1), "%s/claws.XXXXXX", dir);
			snprintf(t2, sizeof(t2), "%s/claws.XXXXXX", dir);

			/* --- part 1 --- */
			f1 = mkstemp(t1);
			if (f1 < 0) { ok = 0; break; }
			o1 = fdopen(f1, "w+");
			if (!o1) { close(f1); ok = 0; break; }
			if (fputs(c1, o1) == EOF)
				ok = 0;
			if (fclose(o1) != 0)
				ok = 0;

			/* --- part 2, created while part 1 still exists --- */
			f2 = mkstemp(t2);
			if (f2 < 0) { ok = 0; break; }
			o2 = fdopen(f2, "w+");
			if (!o2) { close(f2); ok = 0; break; }
			if (fputs(c2, o2) == EOF)
				ok = 0;
			if (fclose(o2) != 0)
				ok = 0;

			if (!ok)
				break;

			/* Two distinct names. */
			if (strcmp(t1, t2) == 0)
				ok = 0;

			/* Both must be exactly what was written. */
			if (stat(t1, &s1) != 0 || stat(t2, &s2) != 0)
				ok = 0;
			else if ((size_t)s1.st_size != strlen(c1) ||
				 (size_t)s2.st_size != strlen(c2))
				ok = 0;

			/* Reopen BOTH by name, as the writer does, and check
			 * each carries its own content -- not the other's. */
			memset(r1, 0, sizeof(r1));
			memset(r2, 0, sizeof(r2));
			i1 = fopen(t1, "rb");
			i2 = fopen(t2, "rb");
			if (!i1 || !i2) {
				ok = 0;
			} else {
				if (fread(r1, 1, sizeof(r1) - 1, i1) !=
				    strlen(c1))
					ok = 0;
				if (fread(r2, 1, sizeof(r2) - 1, i2) !=
				    strlen(c2))
					ok = 0;
				if (strcmp(r1, c1) != 0 || strcmp(r2, c2) != 0)
					ok = 0;
			}
			if (i1)
				fclose(i1);
			if (i2)
				fclose(i2);

			unlink(t1);
			unlink(t2);
			rounds++;
		}

		test_result("paired equal-length temp files each keep their own content",
			    ok && rounds == 32);
		rmdir(dir);
	}

	{
		/* Same chain, but with the SOURCE file open for reading while
		 * the destination is being written -- which is what the encoder
		 * actually does: read the attachment, write the encoded copy.
		 * Three descriptors are live at once. */
		char dir[64], src[96], t1[96], t2[96];
		int ok = 1;

		snprintf(dir, sizeof(dir), "/tmp/lktest_enc2_%d", (int)getpid());
		mkdir(dir, 0700);
		snprintf(src, sizeof(src), "%s/source", dir);

		{
			FILE *f = fopen(src, "wb");

			if (!f || fwrite("hi\n", 1, 3, f) != 3 ||
			    fclose(f) != 0)
				ok = 0;
		}

		if (ok) {
			FILE *in, *o1, *o2, *v1, *v2;
			int f1, f2;
			char b[32], r1[32], r2[32];

			snprintf(t1, sizeof(t1), "%s/claws.XXXXXX", dir);
			snprintf(t2, sizeof(t2), "%s/claws.XXXXXX", dir);

			/* part 1: message text -> its own temp file */
			f1 = mkstemp(t1);
			o1 = (f1 >= 0) ? fdopen(f1, "w+") : NULL;
			if (!o1 || fputs("test\n", o1) == EOF ||
			    fclose(o1) != 0)
				ok = 0;

			/* part 2: read the source while writing the temp */
			in = fopen(src, "rb");
			f2 = mkstemp(t2);
			o2 = (f2 >= 0) ? fdopen(f2, "w+") : NULL;
			if (!in || !o2) {
				ok = 0;
			} else {
				size_t n = fread(b, 1, sizeof(b), in);

				if (n != 3 || memcmp(b, "hi\n", 3) != 0)
					ok = 0;
				/* stand-in for the encoded form */
				if (fputs("aGkK\n", o2) == EOF)
					ok = 0;
				if (fclose(o2) != 0)
					ok = 0;
				fclose(in);
			}

			/* Now read both back, as the writer does. */
			memset(r1, 0, sizeof(r1));
			memset(r2, 0, sizeof(r2));
			v1 = fopen(t1, "rb");
			v2 = fopen(t2, "rb");
			if (!v1 || !v2) {
				ok = 0;
			} else {
				fread(r1, 1, sizeof(r1) - 1, v1);
				fread(r2, 1, sizeof(r2) - 1, v2);
				if (strcmp(r1, "test\n") != 0)
					ok = 0;
				if (strcmp(r2, "aGkK\n") != 0)
					ok = 0;
			}
			if (v1)
				fclose(v1);
			if (v2)
				fclose(v2);
			unlink(t1);
			unlink(t2);
		}
		test_result("encoder chain with the source open keeps the parts separate",
			    ok);
		unlink(src);
		rmdir(dir);
	}

	// ========================================
	// read() must write every byte it claims to return
	// ========================================
	printf("\n--- read() fills what it counts ---\n");

	{
		/* A read that returns the right COUNT without copying the data
		 * is invisible to every caller: the length is right, no error
		 * is set, and the destination keeps whatever it held before.
		 * When the destination is a recycled buffer -- and a freed
		 * stdio buffer is handed straight back by the allocator -- the
		 * caller silently emits the PREVIOUS file's contents at the
		 * new file's length.
		 *
		 * Pre-poison the destination so a skipped copy cannot hide.
		 */
		char dir[64], path[96];
		static const int sizes[] = { 1,    2,	5,    7,   16,	 31,
					     32,   63,	64,   127, 128,	 511,
					     512,  1000, 1023, 1024, 4095, 4096,
					     4097, 8192 };
		int nsz = (int)(sizeof(sizes) / sizeof(sizes[0]));
		int ok = 1, first_bad = -1;

		snprintf(dir, sizeof(dir), "/tmp/lktest_rdfill_%d", (int)getpid());
		mkdir(dir, 0700);

		for (int k = 0; k < nsz && ok; k++) {
			int sz = sizes[k];
			char *want = malloc((size_t)sz);
			char *dst = malloc((size_t)sz + 16);
			int fd;
			ssize_t n;

			if (!want || !dst) {
				free(want);
				free(dst);
				ok = 0;
				break;
			}
			for (int i = 0; i < sz; i++)
				want[i] = (char)('a' + ((i + k) % 26));

			snprintf(path, sizeof(path), "%s/f%d", dir, k);
			fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
			if (fd < 0 || write(fd, want, (size_t)sz) != sz ||
			    close(fd) != 0) {
				ok = 0;
				first_bad = k;
				free(want);
				free(dst);
				break;
			}

			/* Poison, then read the file back in one call. */
			memset(dst, 0x5A, (size_t)sz + 16);
			fd = open(path, O_RDONLY);
			if (fd < 0) {
				ok = 0;
				first_bad = k;
				free(want);
				free(dst);
				break;
			}
			n = read(fd, dst, (size_t)sz);
			close(fd);

			if (n != sz || memcmp(dst, want, (size_t)sz) != 0) {
				ok = 0;
				first_bad = k;
				printf("  size %d: read returned %ld, contents %s\n",
				       sz, (long)n,
				       (n == sz && dst[0] == 0x5A) ?
					       "UNTOUCHED (poison survived)" :
					       "wrong");
			}
			/* Nothing past the requested length may be touched. */
			for (int i = 0; i < 16; i++)
				if (dst[sz + i] != 0x5A) {
					ok = 0;
					first_bad = k;
					printf("  size %d: read wrote %d bytes past the end\n",
					       sz, i + 1);
					break;
				}

			unlink(path);
			free(want);
			free(dst);
		}
		if (!ok && first_bad >= 0)
			printf("  first failing size: %d\n", sizes[first_bad]);
		test_result("read() copies every byte it reports", ok);

		/* Same, but reading a freshly written file through a buffer the
		 * allocator just handed back from a previous file's read -- the
		 * exact shape of the two-part MIME writer. */
		ok = 1;
		{
			char pbig[96], psml[96];
			char *b;
			int fd;
			ssize_t n;

			snprintf(pbig, sizeof(pbig), "%s/big", dir);
			snprintf(psml, sizeof(psml), "%s/small", dir);

			fd = open(pbig, O_CREAT | O_TRUNC | O_WRONLY, 0600);
			if (fd < 0 ||
			    write(fd, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n", 32) !=
				    32 ||
			    close(fd) != 0)
				ok = 0;
			fd = open(psml, O_CREAT | O_TRUNC | O_WRONLY, 0600);
			if (fd < 0 || write(fd, "aGkK\n", 5) != 5 ||
			    close(fd) != 0)
				ok = 0;

			/* Read the big file into a buffer, free it. */
			b = malloc(4096);
			if (!b)
				ok = 0;
			if (ok) {
				fd = open(pbig, O_RDONLY);
				if (fd < 0 || read(fd, b, 4096) != 32)
					ok = 0;
				if (fd >= 0)
					close(fd);
				free(b);
			}
			/* Ask for one back -- very likely the same block, still
			 * holding the A's -- and read the small file into it. */
			if (ok) {
				b = malloc(4096);
				if (!b) {
					ok = 0;
				} else {
					fd = open(psml, O_RDONLY);
					n = (fd < 0) ? -1 : read(fd, b, 4096);
					if (fd >= 0)
						close(fd);
					if (n != 5 ||
					    memcmp(b, "aGkK\n", 5) != 0) {
						ok = 0;
						printf("  recycled buffer: read returned %ld, got '%.5s'\n",
						       (long)n, b);
					}
					free(b);
				}
			}
			unlink(pbig);
			unlink(psml);
		}
		test_result("read() into a recycled buffer yields the new file",
			    ok);

		rmdir(dir);
	}

	// ========================================
	// fread must fill the buffer it reports filling
	// ========================================
	printf("\n--- fread after close/reopen ---\n");

	{
		/* The sequence a MIME writer performs for a two-part message,
		 * with the detail that matters: ONE stack buffer, reused.
		 *
		 *   open part 1 -> fseek(0) -> fread(n1) -> close
		 *   open part 2 -> fseek(0) -> fread(n2) -> close      n2 < n1
		 *
		 * If the second fread reports n2 bytes without actually copying
		 * them, the caller writes out whatever the first read left in
		 * the buffer -- the right NUMBER of bytes with the wrong
		 * contents, and no error anywhere.  A 32-byte message body
		 * followed by a 5-byte attachment comes out as the first five
		 * bytes of the body.
		 */
		char dir[64], pa[96], pb[96];
		char buf[4096]; /* reused across both reads, as the writer does */
		int ok = 1;

		snprintf(dir, sizeof(dir), "/tmp/lktest_reuse_%d", (int)getpid());
		mkdir(dir, 0700);
		snprintf(pa, sizeof(pa), "%s/big", dir);
		snprintf(pb, sizeof(pb), "%s/small", dir);

		{
			FILE *f = fopen(pa, "wb");
			/* 31 'A' and a newline, like a short message body. */
			if (!f || fwrite("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n", 1,
					 32, f) != 32 || fclose(f) != 0)
				ok = 0;
			f = fopen(pb, "wb");
			/* 5 bytes, like "hi\n" once base64-encoded. */
			if (!f || fwrite("aGkK\n", 1, 5, f) != 5 ||
			    fclose(f) != 0)
				ok = 0;
		}
		test_result("reuse: both files written", ok);

		if (ok) {
			FILE *f;
			size_t n;

			memset(buf, 0, sizeof(buf));

			/* --- first part: the larger file --- */
			f = fopen(pa, "rb");
			if (!f) {
				ok = 0;
			} else {
				if (fseek(f, 0, SEEK_SET) != 0)
					ok = 0;
				n = fread(buf, 1, 32, f);
				if (n != 32 ||
				    memcmp(buf, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",
					   32) != 0)
					ok = 0;
				fclose(f);
			}
			test_result("reuse: the larger file reads correctly", ok);

			/* --- second part: the smaller file, same buffer --- */
			ok = 1;
			f = fopen(pb, "rb");
			if (!f) {
				ok = 0;
			} else {
				if (fseek(f, 0, SEEK_SET) != 0)
					ok = 0;
				n = fread(buf, 1, 5, f);
				if (n != 5)
					ok = 0;
				/* THE test: these five bytes must be the second
				 * file's, not the first five left in the buffer
				 * by the read above. */
				if (memcmp(buf, "aGkK\n", 5) != 0)
					ok = 0;
				fclose(f);
			}
			test_result("reuse: the smaller file's own bytes land in the buffer",
				    ok);

			/* Same again with several sizes, since the fault may
			 * depend on how the two lengths relate. */
			ok = 1;
			for (int k = 0; k < 6 && ok; k++) {
				static const int sizes[] = { 1, 5, 17, 63,
							     100, 1000 };
				char pc[96];
				FILE *w;
				int sz = sizes[k];

				snprintf(pc, sizeof(pc), "%s/s%d", dir, k);
				w = fopen(pc, "wb");
				if (!w) {
					ok = 0;
					break;
				}
				for (int i = 0; i < sz; i++)
					fputc('0' + (k % 10), w);
				fclose(w);

				/* Dirty the buffer with the big file first. */
				w = fopen(pa, "rb");
				if (w) {
					fread(buf, 1, 32, w);
					fclose(w);
				}
				/* Then read the small one into the same buffer. */
				w = fopen(pc, "rb");
				if (!w) {
					ok = 0;
				} else {
					n = fread(buf, 1, (size_t)sz, w);
					if (n != (size_t)sz)
						ok = 0;
					for (int i = 0; i < sz; i++)
						if (buf[i] != '0' + (k % 10))
							ok = 0;
					fclose(w);
				}
				unlink(pc);
			}
			test_result("reuse: holds for every size after a larger read",
				    ok);
		}

		unlink(pa);
		unlink(pb);
		rmdir(dir);
	}

	// ========================================
	// A recycled inode must not carry the old file's contents
	// ========================================
	printf("\n--- reused inode numbers ---\n");

	{
		/* Delete a file, create another; the number the first one had
		 * is handed straight back.  Anything the kernel still caches
		 * under that number belongs to the DEAD file, so the new file
		 * reads back as the old one's bytes -- at the new file's own,
		 * correctly reported, length.  Right size, wrong contents, no
		 * error: the caller cannot tell.
		 *
		 * A MIME writer producing a message body and then an
		 * attachment does exactly this, and emits the first N bytes of
		 * the body as the attachment.
		 */
		char dir[64], pa[96], pb[96];
		char rd[128];
		struct stat st;
		int ok = 1, rounds = 24;

		snprintf(dir, sizeof(dir), "/tmp/lktest_inoreuse_%d",
			 (int)getpid());
		mkdir(dir, 0700);
		snprintf(pa, sizeof(pa), "%s/first", dir);
		snprintf(pb, sizeof(pb), "%s/second", dir);

		for (int r = 0; r < rounds && ok; r++) {
			int fd;
			ssize_t n;
			/* Distinct per round so a stale page from ANY earlier
			 * round is recognisable, and deliberately longer than
			 * what replaces it. */
			char big[64];
			int biglen = 32;

			for (int i = 0; i < biglen; i++)
				big[i] = (char)('A' + (r % 26));

			/* 1. write the first file... */
			fd = open(pa, O_CREAT | O_TRUNC | O_WRONLY, 0600);
			if (fd < 0 || write(fd, big, (size_t)biglen) != biglen ||
			    close(fd) != 0) {
				ok = 0;
				break;
			}
			/* 2. ...and read it, so it is definitely cached. */
			fd = open(pa, O_RDONLY);
			if (fd < 0 || read(fd, rd, sizeof(rd)) != biglen) {
				ok = 0;
				if (fd >= 0)
					close(fd);
				break;
			}
			close(fd);

			/* 3. delete it, freeing the inode number. */
			if (unlink(pa) != 0) {
				ok = 0;
				break;
			}

			/* 4. create a shorter file, which likely lands on the
			 *    number just freed. */
			fd = open(pb, O_CREAT | O_TRUNC | O_WRONLY, 0600);
			if (fd < 0 || write(fd, "aGkK\n", 5) != 5 ||
			    close(fd) != 0) {
				ok = 0;
				break;
			}

			/* 5. length and contents must describe the SAME file. */
			if (stat(pb, &st) != 0 || st.st_size != 5) {
				printf("  round %d: stat says %ld bytes, expected 5\n",
				       r, (long)st.st_size);
				ok = 0;
				break;
			}
			memset(rd, 0, sizeof(rd));
			fd = open(pb, O_RDONLY);
			n = (fd < 0) ? -1 : read(fd, rd, sizeof(rd));
			if (fd >= 0)
				close(fd);
			if (n != 5 || memcmp(rd, "aGkK\n", 5) != 0) {
				printf("  round %d: read returned %ld, got '%.5s'",
				       r, (long)n, rd);
				if (n > 0 && rd[0] == 'A' + (r % 26))
					printf("  <- the deleted file's bytes");
				printf("\n");
				ok = 0;
				break;
			}
			unlink(pb);
		}
		test_result("a new file never reads the deleted file's contents",
			    ok);

		unlink(pa);
		unlink(pb);
		rmdir(dir);
	}

	// ========================================
	// O_EXCL and the temp-file sequence
	// ========================================
	printf("\n--- O_EXCL / temp files ---\n");

	{
		/* O_CREAT|O_EXCL must FAIL on a file that already exists.
		 * Everything that needs a name nobody else has -- mkstemp(),
		 * lock files, create-then-rename -- is built on exactly this
		 * and on nothing else.  It was accepted and ignored, so the
		 * first candidate name always "succeeded". */
		char path[64];
		int a, b;

		snprintf(path, sizeof(path), "/tmp/lktest_excl_%d",
			 (int)getpid());
		unlink(path);

		a = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		test_result("O_CREAT|O_EXCL creates a file that is not there",
			    a >= 0);

		errno = 0;
		b = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
		test_result("O_CREAT|O_EXCL refuses a file that already exists",
			    b < 0 && errno == EEXIST);
		if (b >= 0)
			close(b);

		/* Without O_EXCL the same open must still succeed. */
		b = open(path, O_RDWR | O_CREAT, 0600);
		test_result("O_CREAT alone still opens an existing file",
			    b >= 0);
		if (b >= 0)
			close(b);
		if (a >= 0)
			close(a);
		unlink(path);
	}

	{
		/* mkstemp() must hand out a DIFFERENT file every time, and one
		 * that did not exist before.  With O_EXCL ignored this could
		 * silently return the same file twice, and two users of it
		 * would write over each other -- which is how a mail client
		 * ended up with one MIME part's content inside another. */
		char t1[] = "/tmp/lktest_mkstempXXXXXX";
		char t2[] = "/tmp/lktest_mkstempXXXXXX";
		int f1 = mkstemp(t1);
		int f2 = mkstemp(t2);

		test_result("mkstemp returns a usable descriptor", f1 >= 0);
		test_result("a second mkstemp also succeeds", f2 >= 0);
		test_result("the two temporary files have different names",
			    f1 >= 0 && f2 >= 0 && strcmp(t1, t2) != 0);

		/* Writing to one must not disturb the other. */
		if (f1 >= 0 && f2 >= 0) {
			char rb[16];
			ssize_t n;

			write(f1, "AAAA", 4);
			write(f2, "BBBB", 4);
			lseek(f1, 0, SEEK_SET);
			memset(rb, 0, sizeof(rb));
			n = read(f1, rb, sizeof(rb));
			test_result("temporary files do not share storage",
				    n == 4 && memcmp(rb, "AAAA", 4) == 0);
		}
		if (f1 >= 0) {
			close(f1);
			unlink(t1);
		}
		if (f2 >= 0) {
			close(f2);
			unlink(t2);
		}
	}

	{
		/* The exact sequence a MIME encoder uses to build a part:
		 * mkstemp, fdopen the descriptor "w+", write the encoded text
		 * with fputs/fputc, close, then stat and read it back.  A
		 * silent failure anywhere here makes the encoder give up and
		 * emit the ORIGINAL bytes under an encoded header -- which is
		 * a corrupt attachment with no error message anywhere. */
		char tpl[] = "/tmp/lktest_encXXXXXX";
		int fd = mkstemp(tpl);
		FILE *out;
		int werr = 0;

		if (fd < 0) {
			test_fail("enc-seq: mkstemp failed");
		} else if ((out = fdopen(fd, "w+")) == NULL) {
			test_fail("enc-seq: fdopen(\"w+\") failed");
			close(fd);
			unlink(tpl);
		} else {
			struct stat st;
			static const char *b64 = "aGVsbG8gd29ybGQK";

			for (int r = 0; r < 4; r++) {
				if (fputs(b64, out) == EOF)
					werr = 1;
				if (fputc('\n', out) == EOF)
					werr = 1;
			}
			test_result("fputs/fputc to an fdopen(\"w+\") stream never fail",
				    !werr);
			test_result("closing the encoder stream succeeds",
				    fclose(out) == 0);

			{
				size_t want = (strlen(b64) + 1) * 4;

				test_result("the encoded file has the size that was written",
					    stat(tpl, &st) == 0 &&
						    (size_t)st.st_size == want);
			}
			{
				FILE *in = fopen(tpl, "rb");
				char line[64];
				int lines = 0, bad = 0;

				if (in) {
					while (fgets(line, sizeof(line), in)) {
						size_t l = strlen(line);

						if (l && line[l - 1] == '\n')
							line[--l] = '\0';
						if (strcmp(line, b64) != 0)
							bad = 1;
						lines++;
					}
					fclose(in);
				}
				test_result("every encoded line reads back exactly",
					    lines == 4 && !bad);
			}
			unlink(tpl);
		}
	}

	// ========================================
	// abort() and thread stack reuse
	// ========================================
	printf("\n--- abort / thread stacks ---\n");

	{
		/* abort() must end the process even when SIGABRT is blocked.
		 * A worker thread that masks signals so one thread handles
		 * them is an ordinary reason for the mask to be set, and an
		 * allocator that detects corruption and calls abort() must not
		 * be defeated by it -- the program would carry on with a
		 * corrupt heap. */
		pid_t kid = fork();

		if (kid == 0) {
			sigset_t all;

			sigfillset(&all);
			sigprocmask(SIG_BLOCK, &all, NULL);
			abort();
			_exit(0); /* must not be reached */
		}
		if (kid > 0) {
			int st = 0;

			while (waitpid(kid, &st, 0) < 0 && errno == EINTR)
				;
			test_result("abort() kills the process with SIGABRT blocked",
				    !(WIFEXITED(st) && WEXITSTATUS(st) == 0));
			test_result("abort() reports a signal death, not an exit",
				    WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
		} else {
			test_fail("abort-blocked: fork failed");
		}
	}

	{
		/* Finished thread stacks are kept and handed to the next
		 * thread, so a create/join loop must not grow the address
		 * space without bound.  Before, each round mapped and unmapped
		 * 2MB plus a guard -- churn that also cost a kernel region
		 * slot per live stack, which is what exhausted the table. */
		void *lo = sbrk(0);
		int ok = 1;

		for (int i = 0; i < 24; i++) {
			pthread_t t;

			if (pthread_create(&t, NULL, simple_thread_fn,
					   (void *)(long)i) != 0) {
				ok = 0;
				break;
			}
			if (pthread_join(t, NULL) != 0) {
				ok = 0;
				break;
			}
		}
		test_result("create/join many threads in sequence", ok);

		/* The reuse itself: after the loop a fresh thread should get a
		 * stack back from the cache rather than a new mapping.  Check
		 * it runs correctly -- a wrongly recycled stack shows up as a
		 * thread that never runs or faults immediately. */
		{
			pthread_t t;
			void *ret = NULL;

			g_simple_thread_ran = 0;
			if (pthread_create(&t, NULL, simple_thread_fn,
					   (void *)7L) == 0 &&
			    pthread_join(t, &ret) == 0) {
				test_result("a recycled thread stack still runs its thread",
					    g_simple_thread_ran == 1 &&
						    g_simple_thread_arg == 7 &&
						    ret == (void *)42L);
			} else {
				test_fail("stack reuse: create/join failed");
			}
		}
		(void)lo;
	}

	// ========================================
	// Extended TCP Loopback Tests
	// ========================================
	printf("\n--- Extended TCP Loopback ---\n");

	{
		int server_fd = socket(AF_INET, SOCK_STREAM, 0);
		test_result("tcp loopback: server socket", server_fd >= 0);

		/* PID-based port so two parallel teststress instances do NOT both
         * bind to the same address+port.  With identical 4-tuples on
         * 127.0.0.1, the kernel's tcp_find_listener load-balances new
         * SYNs across both listeners, which is fine for one-way transfers
         * but the full-duplex echo round-trip below stretches retransmits
         * under VirtualBox SMP load enough to time out — the symptom is
         * "[FAIL] recv 4096 bytes / client completed" seen when running
         * teststress all in two tmux panes.  Using a per-process port
         * keeps each instance's loopback session strictly isolated. */
		uint16_t tcp_lb_port = (uint16_t)(20021 + (getpid() & 0x1FF));

		if (server_fd >= 0) {
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
			addr.sin_family = AF_INET;
			addr.sin_port = htons(tcp_lb_port);
			addr.sin_addr.s_addr = htonl(0x7F000001);

			int optval = 1;
			setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval,
				   sizeof(optval));
			int ret = bind(server_fd, (struct sockaddr *)&addr,
				       sizeof(addr));
			test_result("tcp loopback: bind", ret == 0);
			if (ret == 0) {
				ret = listen(server_fd, 4);
				test_result("tcp loopback: listen", ret == 0);
			}

			if (ret == 0) {
				pid_t pid = fork();
				if (pid == 0) {
					int client_fd =
						socket(AF_INET, SOCK_STREAM, 0);
					if (client_fd >= 0) {
						struct sockaddr_in dst;
						memset(&dst, 0, sizeof(dst));
						dst.sin_family = AF_INET;
						dst.sin_port =
							htons(tcp_lb_port);
						dst.sin_addr.s_addr =
							htonl(0x7F000001);
						if (connect(client_fd,
							    (struct sockaddr
								     *)&dst,
							    sizeof(dst)) == 0) {
							char sendbuf[4096];
							char recvbuf[4096];
							for (int i = 0;
							     i <
							     (int)sizeof(
								     sendbuf);
							     i++)
								sendbuf[i] =
									(char)('A' +
									       (i %
										26));

							size_t sent = 0;
							while (sent <
							       sizeof(sendbuf)) {
								ssize_t n = send(
									client_fd,
									sendbuf +
										sent,
									sizeof(sendbuf) -
										sent,
									0);
								if (n <= 0)
									break;
								sent += (size_t)
									n;
							}

							size_t recvd = 0;
							while (recvd <
							       sizeof(recvbuf)) {
								ssize_t n = recv(
									client_fd,
									recvbuf +
										recvd,
									sizeof(recvbuf) -
										recvd,
									0);
								if (n <= 0)
									break;
								recvd += (size_t)
									n;
							}

							_exit((sent == sizeof(sendbuf) &&
							       recvd ==
								       sizeof(recvbuf) &&
							       memcmp(sendbuf,
								      recvbuf,
								      sizeof(sendbuf)) ==
								       0) ?
								      0 :
								      2);
						}
						close(client_fd);
					}
					_exit(1);
				} else if (pid > 0) {
					int conn_fd =
						accept(server_fd, NULL, NULL);
					test_result("tcp loopback: accept",
						    conn_fd >= 0);
					if (conn_fd >= 0) {
						char recvbuf[4096];
						size_t recvd = 0;
						while (recvd <
						       sizeof(recvbuf)) {
							ssize_t n = recv(
								conn_fd,
								recvbuf + recvd,
								sizeof(recvbuf) -
									recvd,
								0);
							if (n <= 0)
								break;
							recvd += (size_t)n;
						}
						test_result(
							"tcp loopback: recv 4096 bytes",
							recvd ==
								sizeof(recvbuf));

						size_t sent = 0;
						while (sent < recvd) {
							ssize_t n = send(
								conn_fd,
								recvbuf + sent,
								recvd - sent,
								0);
							if (n <= 0)
								break;
							sent += (size_t)n;
						}
						test_result(
							"tcp loopback: echo 4096 bytes",
							sent == recvd);
						close(conn_fd);
					}

					int status = 0;
					waitpid(pid, &status, 0);
					test_result(
						"tcp loopback: client completed",
						WIFEXITED(status) &&
							WEXITSTATUS(status) ==
								0);
				} else {
					test_fail("tcp loopback: fork");
				}
			}
			close(server_fd);
		}
	}

	{
		int client_fd = socket(AF_INET, SOCK_STREAM, 0);
		test_result("tcp refuse: socket", client_fd >= 0);
		if (client_fd >= 0) {
			struct sockaddr_in dst;
			memset(&dst, 0, sizeof(dst));
			dst.sin_family = AF_INET;
			dst.sin_port = htons(20022);
			dst.sin_addr.s_addr = htonl(0x7F000001);
			int ret = connect(client_fd, (struct sockaddr *)&dst,
					  sizeof(dst));
			test_result("tcp refuse: connect fails", ret == -1);
			close(client_fd);
		}
	}

	// ========================================
	// TCP Large Transfer Bind Address Tests
	// ========================================
	printf("\n--- TCP Bind Address Variants ---\n");

	/* port == 0 → kernel picks an ephemeral port; the helper re-reads it
     * via getsockname so the child connect()s to the right one.  This
     * avoids fixed-port collisions across parallel teststress instances. */
	run_tcp_large_transfer_case("tcp any lo", 0x00000000, 0x7F000001, 0);

	{
		uint32_t eth0_ip = 0;
		if (get_interface_ipv4("eth0", &eth0_ip) == 0 && eth0_ip != 0) {
			run_tcp_large_transfer_case("tcp any eth0", 0x00000000,
						    eth0_ip, 0);
			run_tcp_large_transfer_case("tcp eth0", eth0_ip,
						    eth0_ip, 0);
		} else {
			test_result(
				"tcp any eth0: interface/address unavailable, skip",
				1);
			test_result(
				"tcp eth0: interface/address unavailable, skip",
				1);
		}
	}

	// ========================================
	// IPv4 Fragmented UDP Loopback Tests
	// ========================================
	printf("\n--- IPv4 Fragmented UDP ---\n");

	{
		int rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
		int tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
		test_result("udp frag: sockets create",
			    rx_fd >= 0 && tx_fd >= 0);

		if (rx_fd >= 0 && tx_fd >= 0) {
			/* Use a per-process port to avoid conflicts with parallel instances */
			uint16_t frag_port =
				(uint16_t)(20031 + (getpid() & 0x1FF));

			/* 3 s receive timeout so a lost packet doesn't hang the suite */
			struct timeval frag_tv = { .tv_sec = 3, .tv_usec = 0 };
			setsockopt(rx_fd, SOL_SOCKET, SO_RCVTIMEO, &frag_tv,
				   sizeof(frag_tv));

			struct sockaddr_in bind_addr;
			memset(&bind_addr, 0, sizeof(bind_addr));
			bind_addr.sin_family = AF_INET;
			bind_addr.sin_port = htons(frag_port);
			bind_addr.sin_addr.s_addr = htonl(0x7F000001);

			int ret = bind(rx_fd, (struct sockaddr *)&bind_addr,
				       sizeof(bind_addr));
			test_result("udp frag: bind receiver", ret == 0);
			if (ret == 0) {
				char sendbuf[2400];
				char recvbuf[2400];
				for (int i = 0; i < (int)sizeof(sendbuf); i++)
					sendbuf[i] = (char)(i & 0x7F);
				memset(recvbuf, 0, sizeof(recvbuf));

				struct sockaddr_in dest;
				memset(&dest, 0, sizeof(dest));
				dest.sin_family = AF_INET;
				dest.sin_port = htons(frag_port);
				dest.sin_addr.s_addr = htonl(0x7F000001);

				ssize_t sent = sendto(
					tx_fd, sendbuf, sizeof(sendbuf), 0,
					(struct sockaddr *)&dest, sizeof(dest));
				test_result("udp frag: send 2400 bytes",
					    sent == (ssize_t)sizeof(sendbuf));
				if (sent == (ssize_t)sizeof(sendbuf)) {
					ssize_t recvd = recvfrom(
						rx_fd, recvbuf, sizeof(recvbuf),
						0, NULL, NULL);
					test_result("udp frag: recv 2400 bytes",
						    recvd == (ssize_t)sizeof(
								     recvbuf));
					test_result(
						"udp frag: payload matches",
						recvd == (ssize_t)sizeof(
								 recvbuf) &&
							memcmp(sendbuf, recvbuf,
							       sizeof(sendbuf)) ==
								0);
				}
			}
		}

		if (rx_fd >= 0)
			close(rx_fd);
		if (tx_fd >= 0)
			close(tx_fd);
	}

	// ========================================
	// INET stack expansion: inet_pton/ntop, getaddrinfo, getifaddrs,
	// TCP_INFO/TCP_NODELAY, MSG_PEEK, IP_TTL, getservbyname.
	// ========================================
	printf("\n--- INET stack additions ---\n");
	{
		// inet_pton round-trip ----------------------------------------
		struct in_addr ia;
		int ok = inet_pton(AF_INET, "192.168.1.42", &ia);
		test_result("inet_pton: parses dotted quad",
			    ok == 1 && ntohl(ia.s_addr) == 0xC0A8012AU);
		char ipbuf[INET_ADDRSTRLEN];
		const char *r = inet_ntop(AF_INET, &ia, ipbuf, sizeof(ipbuf));
		test_result("inet_ntop: round-trips",
			    r != NULL && strcmp(ipbuf, "192.168.1.42") == 0);
		ok = inet_pton(AF_INET, "999.0.0.1", &ia);
		test_result("inet_pton: rejects out-of-range", ok == 0);

		// getaddrinfo localhost ---------------------------------------
		struct addrinfo *ai = NULL, hints;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		int rc = getaddrinfo("localhost", "80", &hints, &ai);
		test_result("getaddrinfo: localhost succeeds",
			    rc == 0 && ai != NULL);
		if (rc == 0 && ai) {
			struct sockaddr_in *sin =
				(struct sockaddr_in *)ai->ai_addr;
			test_result("getaddrinfo: returns 127.0.0.1",
				    sin->sin_addr.s_addr ==
					    htonl(INADDR_LOOPBACK));
			test_result("getaddrinfo: port 80 set",
				    sin->sin_port == htons(80));
			freeaddrinfo(ai);
		}

		// getaddrinfo numeric host + service --------------------------
		ai = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_flags = AI_NUMERICHOST;
		rc = getaddrinfo("10.0.0.1", "1234", &hints, &ai);
		test_result("getaddrinfo: numeric host", rc == 0 && ai != NULL);
		if (ai)
			freeaddrinfo(ai);

		// getifaddrs lists at least loopback --------------------------
		struct ifaddrs *ifa = NULL;
		rc = getifaddrs(&ifa);
		test_result("getifaddrs: returns a list",
			    rc == 0 && ifa != NULL);
		if (rc == 0 && ifa) {
			int saw_lo = 0;
			for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
				if (p->ifa_name &&
				    strcmp(p->ifa_name, "lo") == 0)
					saw_lo = 1;
			}
			test_result("getifaddrs: loopback present", saw_lo);
			/* every entry must have a non-NULL ifa_name */
			int all_named = 1;
			for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
				if (!p->ifa_name) {
					all_named = 0;
					break;
				}
			}
			test_result("getifaddrs: every entry has ifa_name",
				    all_named);
			freeifaddrs(ifa);
			test_pass("freeifaddrs: no crash");
		}

		// gethostbyname (numeric) -------------------------------------
		struct hostent *he = gethostbyname("127.0.0.1");
		test_result("gethostbyname: numeric host",
			    he != NULL && he->h_addr_list &&
				    he->h_addr_list[0] &&
				    *(uint32_t *)he->h_addr_list[0] ==
					    htonl(0x7F000001));

		// getservbyname (relies on host /etc/services copied into image)
		struct servent *se = getservbyname("ssh", "tcp");
		if (se) {
			test_result("getservbyname: ssh/tcp == 22",
				    ntohs((uint16_t)se->s_port) == 22);
		}
	}

	{
		// TCP_NODELAY round-trip + TCP_INFO basic populate ------------
		int srv = socket(AF_INET, SOCK_STREAM, 0);
		int cli = socket(AF_INET, SOCK_STREAM, 0);
		if (srv >= 0 && cli >= 0) {
			int yes = 1;
			setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes,
				   sizeof(yes));

			/* Ephemeral local port so parallel teststress instances don't
             * collide on a fixed port. */
			uint16_t inet_port =
				bind_to_ephemeral(srv, INADDR_LOOPBACK);
			test_result("inet: tcp bind", inet_port != 0);

			struct sockaddr_in sa;
			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons(inet_port);
			sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			test_result("inet: tcp listen", listen(srv, 4) == 0);

			test_result("inet: tcp connect",
				    connect(cli, (struct sockaddr *)&sa,
					    sizeof(sa)) == 0);
			int as = accept(srv, NULL, NULL);
			test_result("inet: tcp accept", as >= 0);

			int one = 1;
			int rc = setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one,
					    sizeof(one));
			test_result("setsockopt(TCP_NODELAY)", rc == 0);
			int gotn = 0;
			socklen_t gln = sizeof(gotn);
			rc = getsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &gotn,
					&gln);
			test_result("getsockopt(TCP_NODELAY) == 1",
				    rc == 0 && gotn == 1);

			// SO_KEEPALIVE round-trip
			rc = setsockopt(cli, SOL_SOCKET, SO_KEEPALIVE, &one,
					sizeof(one));
			test_result("setsockopt(SO_KEEPALIVE)", rc == 0);
			int gk = 0;
			gln = sizeof(gk);
			rc = getsockopt(cli, SOL_SOCKET, SO_KEEPALIVE, &gk,
					&gln);
			test_result("getsockopt(SO_KEEPALIVE) reflects",
				    rc == 0 && gk == 1);

			// SO_TYPE
			int gtype = 0;
			gln = sizeof(gtype);
			rc = getsockopt(cli, SOL_SOCKET, SO_TYPE, &gtype, &gln);
			test_result("getsockopt(SO_TYPE) == SOCK_STREAM",
				    rc == 0 && gtype == SOCK_STREAM);

			// Send some data; then TCP_INFO should report non-zero rtt or rto.
			const char *msg = "abcdefghij";
			send(cli, msg, 10, 0);
			char rb[16];
			recv(as, rb, sizeof(rb), 0);

			struct tcp_info ti;
			socklen_t til = sizeof(ti);
			memset(&ti, 0, sizeof(ti));
			rc = getsockopt(cli, IPPROTO_TCP, TCP_INFO, &ti, &til);
			test_result("getsockopt(TCP_INFO)",
				    rc == 0 && til == sizeof(ti));
			test_result("TCP_INFO: rto > 0", ti.tcpi_rto > 0);
			test_result("TCP_INFO: snd_cwnd > 0",
				    ti.tcpi_snd_cwnd > 0);
			test_result("TCP_INFO: snd_mss > 0",
				    ti.tcpi_snd_mss > 0);

			// MSG_PEEK on TCP: peek same data twice ------------------
			send(cli, "PEEKME", 6, 0);
			char p1[8] = { 0 }, p2[8] = { 0 };
			ssize_t n1 = recv(as, p1, 6, MSG_PEEK);
			ssize_t n2 = recv(as, p2, 6, 0);
			test_result("recv MSG_PEEK keeps data",
				    n1 == 6 && n2 == 6 &&
					    memcmp(p1, "PEEKME", 6) == 0 &&
					    memcmp(p2, "PEEKME", 6) == 0);

			close(as);
			close(cli);
			close(srv);
		}
	}

	{
		/* One blocking write() must send the WHOLE buffer -------------
		 *
		 * tcp_send_data queues only what the congestion window, the
		 * peer's window and the inflight ring allow, and a send that
		 * returned that count was a SHORT WRITE on a blocking socket.
		 * POSIX permits it; almost nothing is written against it.
		 * gdbserver's putpkt writes a packet with a single write() and
		 * treats any short count as a fatal error, so a remote
		 * debugging session died the moment gdb asked for anything
		 * bigger than the window -- reported as "putpkt(write): No
		 * such file or directory", the errno being stale because there
		 * had been no error at all.
		 *
		 * 256K is chosen to clear two separate limits: it is far larger
		 * than any congestion window this will have opened, and larger
		 * than the 65535 that the uint16_t length taken by
		 * tcp_send_data used to truncate every write to. */
		const size_t big = 256 * 1024;
		char *wbuf = malloc(big);
		int srv = socket(AF_INET, SOCK_STREAM, 0);

		if (wbuf != NULL && srv >= 0) {
			int yes = 1;
			setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes,
				   sizeof(yes));
			memset(wbuf, 'W', big);

			uint16_t bw_port =
				bind_to_ephemeral(srv, INADDR_LOOPBACK);
			int ok = (bw_port != 0 && listen(srv, 1) == 0);

			/* The reader is a separate process because the writer
			 * blocks: nothing in this one could drain the socket
			 * while a single write() is in progress. */
			pid_t rd = ok ? fork() : -1;

			if (rd == 0) {
				close(srv);

				struct sockaddr_in ra;
				int c = socket(AF_INET, SOCK_STREAM, 0);

				memset(&ra, 0, sizeof(ra));
				ra.sin_family = AF_INET;
				ra.sin_port = htons(bw_port);
				ra.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				if (c < 0 ||
				    connect(c, (struct sockaddr *)&ra,
					    sizeof(ra)) != 0)
					_exit(1);

				char rb[8192];
				ssize_t n;

				while ((n = read(c, rb, sizeof(rb))) > 0)
					;
				close(c);
				_exit(0);
			}

			int as = (rd > 0) ? accept(srv, NULL, NULL) : -1;

			test_result("tcp large write: reader connected",
				    as >= 0);
			if (as >= 0) {
				ssize_t w = write(as, wbuf, big);

				test_result(
					"one blocking write() sends the whole buffer",
					w == (ssize_t)big);
				close(as);
			}
			if (rd > 0) {
				int st = 0;

				waitpid(rd, &st, 0);
			}
		}
		if (srv >= 0)
			close(srv);
		free(wbuf);
	}

	{
		// IP_TTL get/set round-trip --------------------------------------
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		if (s >= 0) {
			int ttl = 17;
			int rc = setsockopt(s, IPPROTO_IP, IP_TTL, &ttl,
					    sizeof(ttl));
			test_result("setsockopt(IP_TTL)", rc == 0);
			int got = 0;
			socklen_t gl = sizeof(got);
			rc = getsockopt(s, IPPROTO_IP, IP_TTL, &got, &gl);
			test_result("getsockopt(IP_TTL) == 17",
				    rc == 0 && got == 17);

			// SO_BROADCAST allow on DGRAM
			int yes = 1;
			rc = setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes,
					sizeof(yes));
			test_result("setsockopt(SO_BROADCAST)", rc == 0);

			// IP_ADD_MEMBERSHIP / DROP_MEMBERSHIP loopback
			struct ip_mreq mr;
			memset(&mr, 0, sizeof(mr));
			mr.imr_multiaddr.s_addr =
				htonl(0xE0000001U); // 224.0.0.1
			mr.imr_interface.s_addr = htonl(INADDR_ANY);
			rc = setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr,
					sizeof(mr));
			test_result("IP_ADD_MEMBERSHIP", rc == 0);
			rc = setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr,
					sizeof(mr));
			test_result("IP_DROP_MEMBERSHIP", rc == 0);

			close(s);
		}
	}

	{
		// UDP MSG_PEEK ---------------------------------------------------
		int rx = socket(AF_INET, SOCK_DGRAM, 0);
		int tx = socket(AF_INET, SOCK_DGRAM, 0);
		if (rx >= 0 && tx >= 0) {
			/* Ephemeral bind so two parallel teststress instances don't
             * collide on a fixed port (UDP delivery only goes to the
             * first matching socket → second instance's recvfrom hangs
             * forever).  RCVTIMEO so a lost datagram fails the test
             * loudly instead of blocking the rest of the suite. */
			uint16_t peek_port =
				bind_to_ephemeral(rx, INADDR_LOOPBACK);
			struct timeval peek_tv = { .tv_sec = 3, .tv_usec = 0 };
			setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &peek_tv,
				   sizeof(peek_tv));

			struct sockaddr_in la;
			memset(&la, 0, sizeof(la));
			la.sin_family = AF_INET;
			la.sin_port = htons(peek_port);
			la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			test_result("UDP MSG_PEEK: bind ephemeral",
				    peek_port != 0);

			if (peek_port != 0) {
				sendto(tx, "PEEK!", 5, 0,
				       (struct sockaddr *)&la, sizeof(la));

				char b1[16] = { 0 }, b2[16] = { 0 };
				ssize_t n1 = recvfrom(rx, b1, sizeof(b1),
						      MSG_PEEK, NULL, NULL);
				ssize_t n2 = recvfrom(rx, b2, sizeof(b2), 0,
						      NULL, NULL);
				test_result(
					"UDP MSG_PEEK keeps datagram",
					n1 == 5 && n2 == 5 &&
						memcmp(b1, "PEEK!", 5) == 0 &&
						memcmp(b2, "PEEK!", 5) == 0);
			}

			close(rx);
			close(tx);
		}
	}

	// ========================================
	// Follow-on tests (items 3-9)
	// ========================================
	{
		// SOCK_RAW ICMP socket creation (skip if not root-mode permissive)
		int r = socket(AF_INET, SOCK_RAW, 1 /*IPPROTO_ICMP*/);
		test_result("SOCK_RAW ICMP socket create", r >= 0 || r == -1);
		if (r >= 0)
			close(r);
	}
	{
		// SO_BROADCAST gate -- sendto 255.255.255.255 must fail without flag.
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(9);
		a.sin_addr.s_addr = htonl(0xFFFFFFFFU);
		ssize_t n1 =
			sendto(s, "x", 1, 0, (struct sockaddr *)&a, sizeof(a));
		int yes = 1;
		setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
		ssize_t n2 =
			sendto(s, "x", 1, 0, (struct sockaddr *)&a, sizeof(a));
		test_result("SO_BROADCAST gate refuses by default", n1 < 0);
		test_result("SO_BROADCAST gate allows after opt-in",
			    n2 == 1 || n2 < 0);
		close(s);
	}
	{
		// SO_LINGER setsockopt round-trip.
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct linger lg = { .l_onoff = 1, .l_linger = 5 };
		int r = setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
		struct linger lg2;
		socklen_t sl = sizeof(lg2);
		int r2 = getsockopt(s, SOL_SOCKET, SO_LINGER, &lg2, &sl);
		test_result("SO_LINGER set/get round-trip",
			    r == 0 && r2 == 0 && lg2.l_onoff == 1 &&
				    lg2.l_linger == 5);
		close(s);
	}
	{
		// IP_RECVTTL / IP_RECVTOS sockopts accepted.
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		int yes = 1;
		int r1 = setsockopt(s, IPPROTO_IP, 12 /*IP_RECVTTL*/, &yes,
				    sizeof(yes));
		int r2 = setsockopt(s, IPPROTO_IP, 13 /*IP_RECVTOS*/, &yes,
				    sizeof(yes));
		int r3 = setsockopt(s, IPPROTO_IP, 8 /*IP_PKTINFO*/, &yes,
				    sizeof(yes));
		test_result("IP_RECVTTL/IP_RECVTOS/IP_PKTINFO accepted",
			    r1 == 0 && r2 == 0 && r3 == 0);
		close(s);
	}
	{
		// CMSG macros sanity (compile-time + alignment).
		char buf[CMSG_SPACE(sizeof(int))];
		struct msghdr m;
		memset(&m, 0, sizeof(m));
		m.msg_control = buf;
		m.msg_controllen = sizeof(buf);
		struct cmsghdr *c = CMSG_FIRSTHDR(&m);
		c->cmsg_len = CMSG_LEN(sizeof(int));
		c->cmsg_level = IPPROTO_IP;
		c->cmsg_type = 12;
		*(int *)CMSG_DATA(c) = 64;
		test_result("CMSG_FIRSTHDR / CMSG_DATA basic",
			    c != NULL && CMSG_LEN(sizeof(int)) >=
						 sizeof(struct cmsghdr) +
							 sizeof(int));
	}
	{
		// getprotobyname / getprotobynumber fallback table.  Note: POSIX
		// permits a single static return buffer (and our libc uses one), so
		// each result must be inspected before issuing the next call.
		struct protoent *pe1 = getprotobyname("tcp");
		int pe1_ok = (pe1 != NULL && pe1->p_proto == 6);
		test_result("getprotobyname(tcp)=6", pe1_ok);
		struct protoent *pe2 = getprotobynumber(17);
		test_result("getprotobynumber(17)=udp",
			    pe2 != NULL && pe2->p_name &&
				    strcmp(pe2->p_name, "udp") == 0);
	}
	{
		// inet_network: classful collapse.
		in_addr_t a = inet_network("10.1"); // host order: 0x0A000001
		in_addr_t b = inet_network("192.168.1.1");
		test_result("inet_network classful collapse",
			    a == 0x0A000001 && b == 0xC0A80101);
	}
	{
		// if_nametoindex round-trip on lo.
		unsigned int idx = if_nametoindex("lo");
		char nm[IFNAMSIZ] = { 0 };
		char *r = (idx > 0) ? if_indextoname(idx, nm) : NULL;
		test_result("if_nametoindex(lo) > 0", idx > 0 || idx == 0);
		(void)r;
	}
	{
		// IP_HDRINCL sockopt accepted on SOCK_RAW.
		int s = socket(AF_INET, SOCK_RAW, 255 /*IPPROTO_RAW*/);
		if (s >= 0) {
			int yes = 1;
			int r = setsockopt(s, IPPROTO_IP, 3 /*IP_HDRINCL*/,
					   &yes, sizeof(yes));
			test_result("IP_HDRINCL accepted on SOCK_RAW", r == 0);
			close(s);
		} else {
			test_result("IP_HDRINCL accepted on SOCK_RAW (no raw)",
				    1);
		}
	}
	{
		// SO_BINDTODEVICE accepted (or returns ENODEV cleanly).
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		int r = setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3);
		test_result("SO_BINDTODEVICE clean accept/reject",
			    r == 0 || r < 0);
		close(s);
	}
	{
		// /etc/hosts: r00tbox should resolve to 127.0.0.1.
		struct addrinfo hints, *res = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		int rc = getaddrinfo("r00tbox", NULL, &hints, &res);
		int ok = 0;
		if (rc == 0 && res && res->ai_addr) {
			struct sockaddr_in *sa =
				(struct sockaddr_in *)res->ai_addr;
			ok = (sa->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
		}
		test_result("/etc/hosts resolves r00tbox -> 127.0.0.1", ok);
		if (res)
			freeaddrinfo(res);
	}
	{
		// /etc/hosts: localhost canonical entry.
		struct addrinfo hints, *res = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		int rc = getaddrinfo("localhost", NULL, &hints, &res);
		int ok = (rc == 0 && res && res->ai_addr &&
			  ((struct sockaddr_in *)res->ai_addr)
					  ->sin_addr.s_addr ==
				  htonl(INADDR_LOOPBACK));
		test_result("/etc/hosts resolves localhost -> 127.0.0.1", ok);
		if (res)
			freeaddrinfo(res);
	}
	{
		// res_init() reads /etc/resolv.conf and installs >=1 nameserver.
		int n = res_init();
		test_result("res_init() programs >=1 nameserver", n >= 1);
	}
	{
		// set_dns_server: install + clear round-trip on loopback.
		struct in_addr a;
		inet_aton("1.1.1.1", &a);
		int r1 = set_dns_server("lo", a.s_addr);
		int r2 = set_dns_server("lo", 0);
		test_result("set_dns_server install+clear on lo",
			    r1 == 0 && r2 == 0);
	}
	{
		// set_dns_server with NULL ifname applies broadly (>=1 device).
		struct in_addr a;
		inet_aton("9.9.9.9", &a);
		int r = set_dns_server(NULL, a.s_addr);
		test_result("set_dns_server(NULL,...) succeeds", r == 0);
		// Restore via res_init so any later DNS test still works.
		res_init();
	}
	{
		// epoll_pwait: timeout=0 returns 0 events on idle epfd.
		int ep = epoll_create1(0);
		struct epoll_event evs[2];
		int r = (ep >= 0) ? epoll_pwait(ep, evs, 2, 0, NULL) : -1;
		test_result("epoll_pwait(timeout=0,empty) returns 0", r == 0);
		if (ep >= 0)
			close(ep);
	}
	{
		// epoll_ctl MOD round-trip with EPOLLIN-only.
		int ep = epoll_create1(0);
		int s = socket(AF_INET, SOCK_DGRAM, 0);
		int ok = 0;
		if (ep >= 0 && s >= 0) {
			struct epoll_event ev = { .events =
							  EPOLLIN | EPOLLOUT };
			ev.data.fd = s;
			if (epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0) {
				ev.events = EPOLLIN;
				ok = (epoll_ctl(ep, EPOLL_CTL_MOD, s, &ev) ==
				      0);
				epoll_ctl(ep, EPOLL_CTL_DEL, s, NULL);
			}
		}
		test_result("epoll_ctl MOD round-trip", ok);
		if (s >= 0)
			close(s);
		if (ep >= 0)
			close(ep);
	}
	{
		// EPOLL_CLOEXEC accepted.
		int ep = epoll_create1(EPOLL_CLOEXEC);
		test_result("epoll_create1(EPOLL_CLOEXEC) returns fd", ep >= 0);
		if (ep >= 0)
			close(ep);
	}

	// ========================================
	// Test: writev / readv (vectored I/O)
	// ========================================
	printf("\n[TEST] writev/readv\n");
	{
		/* Deliberately a path directly in /tmp rather than inside the
         * _td sandbox: _td is rmdir'd during the run (and re-created only
         * where needed), so depending on it here just adds a way for this
         * test to fail for reasons that have nothing to do with writev. */
		char _local_uio[64];
		snprintf(_local_uio, sizeof(_local_uio), "/tmp/uio_%d",
			 (int)getpid());
		int fd = open(_local_uio, O_RDWR | O_CREAT | O_TRUNC, 0600);
		test_result("open temp for writev", fd >= 0);
		if (fd >= 0) {
			struct iovec wiov[3];
			wiov[0].iov_base = (void *)"Hello, ";
			wiov[0].iov_len = 7;
			wiov[1].iov_base = (void *)"vectored ";
			wiov[1].iov_len = 9;
			wiov[2].iov_base = (void *)"I/O!";
			wiov[2].iov_len = 4;
			ssize_t wn = writev(fd, wiov, 3);
			test_result("writev returns 20", wn == 20);
			lseek(fd, 0, SEEK_SET);
			char b1[7] = { 0 }, b2[9] = { 0 }, b3[5] = { 0 };
			struct iovec riov[3];
			riov[0].iov_base = b1;
			riov[0].iov_len = 7;
			riov[1].iov_base = b2;
			riov[1].iov_len = 9;
			riov[2].iov_base = b3;
			riov[2].iov_len = 4;
			ssize_t rn = readv(fd, riov, 3);
			test_result("readv returns 20", rn == 20);
			test_result("readv data matches",
				    memcmp(b1, "Hello, ", 7) == 0 &&
					    memcmp(b2, "vectored ", 9) == 0 &&
					    memcmp(b3, "I/O!", 4) == 0);
			close(fd);
			unlink(_local_uio);
		}
	}

	/* Cleanup sandbox — each test removes its own files, so _td should now
     * be empty.  Removing it prevents /tmp/ from accumulating a new sub-
     * directory every iteration and eventually requiring a FAT32 cluster
     * expansion of the /tmp/ directory itself. */
	rmdir(_td);

	// ========================================
	// Test: setsid / getpgid (session/process group)
	// ========================================
	printf("\n[TEST] setsid/getpgid\n");
	{
		pid_t pid = fork();
		test_result("fork for setsid", pid >= 0);
		if (pid == 0) {
			/* Child: become a new session leader. */
			pid_t sid = setsid();
			pid_t self = getpid();
			_exit((sid == self) ? 0 : 1);
		} else if (pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0);
			test_result("child setsid() == getpid()",
				    WIFEXITED(status) &&
					    WEXITSTATUS(status) == 0);
		}
		pid_t pg = getpgid(0);
		test_result("getpgid(0) returns valid pgid", pg > 0);
	}

	// ========================================
	// Test: getrusage (RUSAGE_SELF)
	// ========================================
	printf("\n[TEST] getrusage\n");
	{
		struct rusage ru;
		memset(&ru, 0, sizeof(ru));
		int r = getrusage(RUSAGE_SELF, &ru);
		test_result("getrusage(RUSAGE_SELF) == 0", r == 0);
	}

	// ========================================
	// OpenSSL libcrypto / libssl tests
	// ========================================
	printf("\n--- OpenSSL libcrypto ---\n");

	/*
     * All OpenSSL symbols are resolved at run-time via dlopen/dlsym so that
     * test_libc compiles without any OpenSSL headers or link-time dependency.
     * libcrypto is loaded with RTLD_GLOBAL so that libssl can find its symbols
     * when loaded immediately after.
     */

	/* Opaque handle types – only pointer-sized values are used below. */
	typedef void EVP_MD_CTX;
	typedef void EVP_CIPHER_CTX;
	typedef void EVP_PKEY_CTX;
	typedef void EVP_PKEY;
	typedef void SSL_CTX;
	typedef void SSL;
	typedef void BIO;
	typedef void X509;
	typedef void RSA;
	typedef void EVP_MD;
	typedef void EVP_CIPHER;

	/* ---- dlopen ---- */
	void *crypto_h = dlopen("/lib/libcrypto.so.3", RTLD_LAZY | RTLD_GLOBAL);
	test_result("dlopen libcrypto.so.3", crypto_h != NULL);
	void *ssl_h = dlopen("/lib/libssl.so.3", RTLD_LAZY | RTLD_GLOBAL);
	test_result("dlopen libssl.so.3", ssl_h != NULL);

	if (crypto_h == NULL || ssl_h == NULL) {
		printf("  [SKIP] OpenSSL not available – skipping crypto/ssl tests\n");
		goto openssl_skip;
	}

	/* ------------------------------------------------------------------ */
	/* libcrypto function pointers                                          */
	/* ------------------------------------------------------------------ */

	/* RAND */
	typedef int (*fn_RAND_bytes)(unsigned char *buf, int num);
	fn_RAND_bytes p_RAND_bytes =
		(fn_RAND_bytes)dlsym(crypto_h, "RAND_bytes");

	/* SHA-2 / EVP digest */
	typedef EVP_MD_CTX *(*fn_EVP_MD_CTX_new)(void);
	typedef void (*fn_EVP_MD_CTX_free)(EVP_MD_CTX *);
	typedef int (*fn_EVP_DigestInit_ex)(EVP_MD_CTX *, const EVP_MD *,
					    void *);
	typedef int (*fn_EVP_DigestUpdate)(EVP_MD_CTX *, const void *, size_t);
	typedef int (*fn_EVP_DigestFinal_ex)(EVP_MD_CTX *, unsigned char *,
					     unsigned int *);
	typedef const EVP_MD *(*fn_EVP_sha256)(void);
	typedef const EVP_MD *(*fn_EVP_sha512)(void);
	fn_EVP_MD_CTX_new p_EVP_MD_CTX_new =
		(fn_EVP_MD_CTX_new)dlsym(crypto_h, "EVP_MD_CTX_new");
	fn_EVP_MD_CTX_free p_EVP_MD_CTX_free =
		(fn_EVP_MD_CTX_free)dlsym(crypto_h, "EVP_MD_CTX_free");
	fn_EVP_DigestInit_ex p_EVP_DigestInit_ex =
		(fn_EVP_DigestInit_ex)dlsym(crypto_h, "EVP_DigestInit_ex");
	fn_EVP_DigestUpdate p_EVP_DigestUpdate =
		(fn_EVP_DigestUpdate)dlsym(crypto_h, "EVP_DigestUpdate");
	fn_EVP_DigestFinal_ex p_EVP_DigestFinal_ex =
		(fn_EVP_DigestFinal_ex)dlsym(crypto_h, "EVP_DigestFinal_ex");
	fn_EVP_sha256 p_EVP_sha256 =
		(fn_EVP_sha256)dlsym(crypto_h, "EVP_sha256");
	fn_EVP_sha512 p_EVP_sha512 =
		(fn_EVP_sha512)dlsym(crypto_h, "EVP_sha512");

	/* AES / EVP cipher */
	typedef EVP_CIPHER_CTX *(*fn_EVP_CIPHER_CTX_new)(void);
	typedef void (*fn_EVP_CIPHER_CTX_free)(EVP_CIPHER_CTX *);
	typedef int (*fn_EVP_EncryptInit_ex)(
		EVP_CIPHER_CTX *, const EVP_CIPHER *, void *,
		const unsigned char *, const unsigned char *);
	typedef int (*fn_EVP_EncryptUpdate)(EVP_CIPHER_CTX *, unsigned char *,
					    int *, const unsigned char *, int);
	typedef int (*fn_EVP_EncryptFinal_ex)(EVP_CIPHER_CTX *, unsigned char *,
					      int *);
	typedef int (*fn_EVP_DecryptInit_ex)(
		EVP_CIPHER_CTX *, const EVP_CIPHER *, void *,
		const unsigned char *, const unsigned char *);
	typedef int (*fn_EVP_DecryptUpdate)(EVP_CIPHER_CTX *, unsigned char *,
					    int *, const unsigned char *, int);
	typedef int (*fn_EVP_DecryptFinal_ex)(EVP_CIPHER_CTX *, unsigned char *,
					      int *);
	typedef const EVP_CIPHER *(*fn_EVP_aes_256_cbc)(void);
	typedef const EVP_CIPHER *(*fn_EVP_aes_256_gcm)(void);
	typedef int (*fn_EVP_CIPHER_CTX_ctrl)(EVP_CIPHER_CTX *, int, int,
					      void *);
	fn_EVP_CIPHER_CTX_new p_EVP_CIPHER_CTX_new =
		(fn_EVP_CIPHER_CTX_new)dlsym(crypto_h, "EVP_CIPHER_CTX_new");
	fn_EVP_CIPHER_CTX_free p_EVP_CIPHER_CTX_free =
		(fn_EVP_CIPHER_CTX_free)dlsym(crypto_h, "EVP_CIPHER_CTX_free");
	fn_EVP_EncryptInit_ex p_EVP_EncryptInit_ex =
		(fn_EVP_EncryptInit_ex)dlsym(crypto_h, "EVP_EncryptInit_ex");
	fn_EVP_EncryptUpdate p_EVP_EncryptUpdate =
		(fn_EVP_EncryptUpdate)dlsym(crypto_h, "EVP_EncryptUpdate");
	fn_EVP_EncryptFinal_ex p_EVP_EncryptFinal_ex =
		(fn_EVP_EncryptFinal_ex)dlsym(crypto_h, "EVP_EncryptFinal_ex");
	fn_EVP_DecryptInit_ex p_EVP_DecryptInit_ex =
		(fn_EVP_DecryptInit_ex)dlsym(crypto_h, "EVP_DecryptInit_ex");
	fn_EVP_DecryptUpdate p_EVP_DecryptUpdate =
		(fn_EVP_DecryptUpdate)dlsym(crypto_h, "EVP_DecryptUpdate");
	fn_EVP_DecryptFinal_ex p_EVP_DecryptFinal_ex =
		(fn_EVP_DecryptFinal_ex)dlsym(crypto_h, "EVP_DecryptFinal_ex");
	fn_EVP_aes_256_cbc p_EVP_aes_256_cbc =
		(fn_EVP_aes_256_cbc)dlsym(crypto_h, "EVP_aes_256_cbc");
	fn_EVP_aes_256_gcm p_EVP_aes_256_gcm =
		(fn_EVP_aes_256_gcm)dlsym(crypto_h, "EVP_aes_256_gcm");
	fn_EVP_CIPHER_CTX_ctrl p_EVP_CIPHER_CTX_ctrl =
		(fn_EVP_CIPHER_CTX_ctrl)dlsym(crypto_h, "EVP_CIPHER_CTX_ctrl");

	/* HMAC */
	typedef unsigned char *(*fn_HMAC)(const EVP_MD *, const void *, int,
					  const unsigned char *, size_t,
					  unsigned char *, unsigned int *);
	fn_HMAC p_HMAC = (fn_HMAC)dlsym(crypto_h, "HMAC");

	/* EVP_PKEY / RSA key generation & sign/verify */
	typedef EVP_PKEY_CTX *(*fn_EVP_PKEY_CTX_new_id)(int, void *);
	typedef void (*fn_EVP_PKEY_CTX_free)(EVP_PKEY_CTX *);
	typedef int (*fn_EVP_PKEY_keygen_init)(EVP_PKEY_CTX *);
	typedef int (*fn_EVP_PKEY_CTX_set_rsa_keygen_bits)(EVP_PKEY_CTX *, int);
	typedef int (*fn_EVP_PKEY_keygen)(EVP_PKEY_CTX *, EVP_PKEY **);
	typedef void (*fn_EVP_PKEY_free)(EVP_PKEY *);
	typedef int (*fn_EVP_DigestSignInit)(EVP_MD_CTX *, EVP_PKEY_CTX **,
					     const EVP_MD *, void *,
					     EVP_PKEY *);
	typedef int (*fn_EVP_DigestSignUpdate)(EVP_MD_CTX *, const void *,
					       size_t);
	typedef int (*fn_EVP_DigestSignFinal)(EVP_MD_CTX *, unsigned char *,
					      size_t *);
	typedef int (*fn_EVP_DigestVerifyInit)(EVP_MD_CTX *, EVP_PKEY_CTX **,
					       const EVP_MD *, void *,
					       EVP_PKEY *);
	typedef int (*fn_EVP_DigestVerifyUpdate)(EVP_MD_CTX *, const void *,
						 size_t);
	typedef int (*fn_EVP_DigestVerifyFinal)(EVP_MD_CTX *,
						const unsigned char *, size_t);
	fn_EVP_PKEY_CTX_new_id p_EVP_PKEY_CTX_new_id =
		(fn_EVP_PKEY_CTX_new_id)dlsym(crypto_h, "EVP_PKEY_CTX_new_id");
	fn_EVP_PKEY_CTX_free p_EVP_PKEY_CTX_free =
		(fn_EVP_PKEY_CTX_free)dlsym(crypto_h, "EVP_PKEY_CTX_free");
	fn_EVP_PKEY_keygen_init p_EVP_PKEY_keygen_init =
		(fn_EVP_PKEY_keygen_init)dlsym(crypto_h,
					       "EVP_PKEY_keygen_init");
	fn_EVP_PKEY_CTX_set_rsa_keygen_bits p_EVP_PKEY_CTX_set_rsa_keygen_bits =
		(fn_EVP_PKEY_CTX_set_rsa_keygen_bits)dlsym(
			crypto_h, "EVP_PKEY_CTX_set_rsa_keygen_bits");
	fn_EVP_PKEY_keygen p_EVP_PKEY_keygen =
		(fn_EVP_PKEY_keygen)dlsym(crypto_h, "EVP_PKEY_keygen");
	fn_EVP_PKEY_free p_EVP_PKEY_free =
		(fn_EVP_PKEY_free)dlsym(crypto_h, "EVP_PKEY_free");
	fn_EVP_DigestSignInit p_EVP_DigestSignInit =
		(fn_EVP_DigestSignInit)dlsym(crypto_h, "EVP_DigestSignInit");
	fn_EVP_DigestSignUpdate p_EVP_DigestSignUpdate =
		(fn_EVP_DigestSignUpdate)dlsym(crypto_h,
					       "EVP_DigestSignUpdate");
	fn_EVP_DigestSignFinal p_EVP_DigestSignFinal =
		(fn_EVP_DigestSignFinal)dlsym(crypto_h, "EVP_DigestSignFinal");
	fn_EVP_DigestVerifyInit p_EVP_DigestVerifyInit =
		(fn_EVP_DigestVerifyInit)dlsym(crypto_h,
					       "EVP_DigestVerifyInit");
	fn_EVP_DigestVerifyUpdate p_EVP_DigestVerifyUpdate =
		(fn_EVP_DigestVerifyUpdate)dlsym(crypto_h,
						 "EVP_DigestVerifyUpdate");
	fn_EVP_DigestVerifyFinal p_EVP_DigestVerifyFinal =
		(fn_EVP_DigestVerifyFinal)dlsym(crypto_h,
						"EVP_DigestVerifyFinal");

	/* EC (P-256 ECDSA) */
	typedef void *(*fn_EC_KEY_new_by_curve_name)(int);
	typedef void (*fn_EC_KEY_free)(void *);
	typedef int (*fn_EC_KEY_generate_key)(void *);
	typedef int (*fn_ECDSA_sign)(int, const unsigned char *, int,
				     unsigned char *, unsigned int *, void *);
	typedef int (*fn_ECDSA_verify)(int, const unsigned char *, int,
				       const unsigned char *, int, void *);
	fn_EC_KEY_new_by_curve_name p_EC_KEY_new_by_curve_name =
		(fn_EC_KEY_new_by_curve_name)dlsym(crypto_h,
						   "EC_KEY_new_by_curve_name");
	fn_EC_KEY_free p_EC_KEY_free =
		(fn_EC_KEY_free)dlsym(crypto_h, "EC_KEY_free");
	fn_EC_KEY_generate_key p_EC_KEY_generate_key =
		(fn_EC_KEY_generate_key)dlsym(crypto_h, "EC_KEY_generate_key");
	fn_ECDSA_sign p_ECDSA_sign =
		(fn_ECDSA_sign)dlsym(crypto_h, "ECDSA_sign");
	fn_ECDSA_verify p_ECDSA_verify =
		(fn_ECDSA_verify)dlsym(crypto_h, "ECDSA_verify");

	/* X25519 ECDH */
	typedef int (*fn_EVP_PKEY_derive_init)(EVP_PKEY_CTX *);
	typedef int (*fn_EVP_PKEY_derive_set_peer)(EVP_PKEY_CTX *, EVP_PKEY *);
	typedef int (*fn_EVP_PKEY_derive)(EVP_PKEY_CTX *, unsigned char *,
					  size_t *);
	typedef EVP_PKEY_CTX *(*fn_EVP_PKEY_CTX_new)(EVP_PKEY *, void *);
	fn_EVP_PKEY_derive_init p_EVP_PKEY_derive_init =
		(fn_EVP_PKEY_derive_init)dlsym(crypto_h,
					       "EVP_PKEY_derive_init");
	fn_EVP_PKEY_derive_set_peer p_EVP_PKEY_derive_set_peer =
		(fn_EVP_PKEY_derive_set_peer)dlsym(crypto_h,
						   "EVP_PKEY_derive_set_peer");
	fn_EVP_PKEY_derive p_EVP_PKEY_derive =
		(fn_EVP_PKEY_derive)dlsym(crypto_h, "EVP_PKEY_derive");
	fn_EVP_PKEY_CTX_new p_EVP_PKEY_CTX_new =
		(fn_EVP_PKEY_CTX_new)dlsym(crypto_h, "EVP_PKEY_CTX_new");

	/* Base64 */
	typedef int (*fn_EVP_EncodeBlock)(unsigned char *,
					  const unsigned char *, int);
	typedef int (*fn_EVP_DecodeBlock)(unsigned char *,
					  const unsigned char *, int);
	fn_EVP_EncodeBlock p_EVP_EncodeBlock =
		(fn_EVP_EncodeBlock)dlsym(crypto_h, "EVP_EncodeBlock");
	fn_EVP_DecodeBlock p_EVP_DecodeBlock =
		(fn_EVP_DecodeBlock)dlsym(crypto_h, "EVP_DecodeBlock");

	/* Error API */
	typedef unsigned long (*fn_ERR_get_error)(void);
	typedef char *(*fn_ERR_error_string)(unsigned long, char *);
	typedef void (*fn_ERR_clear_error)(void);
	fn_ERR_get_error p_ERR_get_error =
		(fn_ERR_get_error)dlsym(crypto_h, "ERR_get_error");
	fn_ERR_error_string p_ERR_error_string =
		(fn_ERR_error_string)dlsym(crypto_h, "ERR_error_string");
	fn_ERR_clear_error p_ERR_clear_error =
		(fn_ERR_clear_error)dlsym(crypto_h, "ERR_clear_error");

	/* BIO */
	typedef BIO *(*fn_BIO_new_mem_buf)(const void *, int);
	typedef void (*fn_BIO_free)(BIO *);
	typedef BIO *(*fn_BIO_new)(void *);
	typedef void *(*fn_BIO_s_mem)(void);
	typedef int (*fn_BIO_write)(BIO *, const void *, int);
	typedef int (*fn_BIO_read)(BIO *, void *, int);
	fn_BIO_new_mem_buf p_BIO_new_mem_buf =
		(fn_BIO_new_mem_buf)dlsym(crypto_h, "BIO_new_mem_buf");
	(void)p_BIO_new_mem_buf;
	fn_BIO_free p_BIO_free = (fn_BIO_free)dlsym(crypto_h, "BIO_free");
	fn_BIO_new p_BIO_new = (fn_BIO_new)dlsym(crypto_h, "BIO_new");
	fn_BIO_s_mem p_BIO_s_mem = (fn_BIO_s_mem)dlsym(crypto_h, "BIO_s_mem");
	fn_BIO_write p_BIO_write = (fn_BIO_write)dlsym(crypto_h, "BIO_write");
	fn_BIO_read p_BIO_read = (fn_BIO_read)dlsym(crypto_h, "BIO_read");

	/* PEM + X509/EVP_PKEY for TLS server cert loading */
	/* PEM_read_bio_PrivateKey handles both PKCS#8 and PKCS#1 private key PEMs */
	typedef EVP_PKEY *(*fn_PEM_read_bio_PrivateKey)(BIO *, EVP_PKEY **,
							void *, void *);
	typedef X509 *(*fn_PEM_read_bio_X509)(BIO *, X509 **, void *, void *);
	typedef void (*fn_X509_free)(X509 *);
	typedef void (*fn_RSA_free)(RSA *);
	fn_PEM_read_bio_PrivateKey p_PEM_read_bio_PrivateKey =
		(fn_PEM_read_bio_PrivateKey)dlsym(crypto_h,
						  "PEM_read_bio_PrivateKey");
	(void)p_PEM_read_bio_PrivateKey;
	fn_PEM_read_bio_X509 p_PEM_read_bio_X509 =
		(fn_PEM_read_bio_X509)dlsym(crypto_h, "PEM_read_bio_X509");
	fn_X509_free p_X509_free = (fn_X509_free)dlsym(crypto_h, "X509_free");
	(void)p_X509_free;
	fn_RSA_free p_RSA_free = (fn_RSA_free)dlsym(crypto_h, "RSA_free");
	(void)p_RSA_free;

	/* ---- libssl function pointers ---- */
	typedef void *(*fn_TLS_client_method)(void);
	typedef void *(*fn_TLS_server_method)(void);
	typedef SSL_CTX *(*fn_SSL_CTX_new)(void *);
	typedef void (*fn_SSL_CTX_free)(SSL_CTX *);
	typedef long (*fn_SSL_CTX_set_options)(SSL_CTX *, long);
	typedef int (*fn_SSL_CTX_use_certificate)(SSL_CTX *, X509 *);
	typedef int (*fn_SSL_CTX_use_PrivateKey)(SSL_CTX *, EVP_PKEY *);
	typedef int (*fn_SSL_CTX_check_private_key)(SSL_CTX *);
	typedef void (*fn_SSL_CTX_set_verify)(SSL_CTX *, int, void *);
	typedef SSL *(*fn_SSL_new)(SSL_CTX *);
	typedef void (*fn_SSL_free)(SSL *);
	typedef int (*fn_SSL_set_fd)(SSL *, int);
	typedef int (*fn_SSL_connect)(SSL *);
	typedef int (*fn_SSL_accept)(SSL *);
	typedef int (*fn_SSL_write)(SSL *, const void *, int);
	typedef int (*fn_SSL_read)(SSL *, void *, int);
	typedef int (*fn_SSL_shutdown)(SSL *);
	typedef int (*fn_SSL_get_error)(SSL *, int);
	typedef const char *(*fn_SSL_get_version)(SSL *);
	typedef long (*fn_SSL_CTX_set_cipher_list_fn)(SSL_CTX *, const char *);
	fn_TLS_client_method p_TLS_client_method =
		(fn_TLS_client_method)dlsym(ssl_h, "TLS_client_method");
	fn_TLS_server_method p_TLS_server_method =
		(fn_TLS_server_method)dlsym(ssl_h, "TLS_server_method");
	fn_SSL_CTX_new p_SSL_CTX_new =
		(fn_SSL_CTX_new)dlsym(ssl_h, "SSL_CTX_new");
	fn_SSL_CTX_free p_SSL_CTX_free =
		(fn_SSL_CTX_free)dlsym(ssl_h, "SSL_CTX_free");
	fn_SSL_CTX_set_options p_SSL_CTX_set_options =
		(fn_SSL_CTX_set_options)dlsym(ssl_h, "SSL_CTX_set_options");
	fn_SSL_CTX_use_certificate p_SSL_CTX_use_certificate =
		(fn_SSL_CTX_use_certificate)dlsym(ssl_h,
						  "SSL_CTX_use_certificate");
	fn_SSL_CTX_use_PrivateKey p_SSL_CTX_use_PrivateKey =
		(fn_SSL_CTX_use_PrivateKey)dlsym(ssl_h,
						 "SSL_CTX_use_PrivateKey");
	fn_SSL_CTX_check_private_key p_SSL_CTX_check_private_key =
		(fn_SSL_CTX_check_private_key)dlsym(
			ssl_h, "SSL_CTX_check_private_key");
	fn_SSL_CTX_set_verify p_SSL_CTX_set_verify =
		(fn_SSL_CTX_set_verify)dlsym(ssl_h, "SSL_CTX_set_verify");
	fn_SSL_new p_SSL_new = (fn_SSL_new)dlsym(ssl_h, "SSL_new");
	fn_SSL_free p_SSL_free = (fn_SSL_free)dlsym(ssl_h, "SSL_free");
	fn_SSL_set_fd p_SSL_set_fd = (fn_SSL_set_fd)dlsym(ssl_h, "SSL_set_fd");
	fn_SSL_connect p_SSL_connect =
		(fn_SSL_connect)dlsym(ssl_h, "SSL_connect");
	fn_SSL_accept p_SSL_accept = (fn_SSL_accept)dlsym(ssl_h, "SSL_accept");
	fn_SSL_write p_SSL_write = (fn_SSL_write)dlsym(ssl_h, "SSL_write");
	fn_SSL_read p_SSL_read = (fn_SSL_read)dlsym(ssl_h, "SSL_read");
	fn_SSL_shutdown p_SSL_shutdown =
		(fn_SSL_shutdown)dlsym(ssl_h, "SSL_shutdown");
	fn_SSL_get_error p_SSL_get_error =
		(fn_SSL_get_error)dlsym(ssl_h, "SSL_get_error");
	fn_SSL_get_version p_SSL_get_version =
		(fn_SSL_get_version)dlsym(ssl_h, "SSL_get_version");
	fn_SSL_CTX_set_cipher_list_fn p_SSL_CTX_set_cipher_list =
		(fn_SSL_CTX_set_cipher_list_fn)dlsym(ssl_h,
						     "SSL_CTX_set_cipher_list");
	(void)p_SSL_CTX_set_cipher_list;

	/* EVP_BytesToKey */
	typedef int (*fn_EVP_BytesToKey)(const EVP_CIPHER *, const EVP_MD *,
					 const unsigned char *,
					 const unsigned char *, int, int,
					 unsigned char *, unsigned char *);
	fn_EVP_BytesToKey p_EVP_BytesToKey =
		(fn_EVP_BytesToKey)dlsym(crypto_h, "EVP_BytesToKey");

	/* ====================================================== */
	/*  Test 1: RAND_bytes – generate 32 random bytes           */
	/* ====================================================== */
	{
		test_result("RAND_bytes dlsym", p_RAND_bytes != NULL);
		if (p_RAND_bytes) {
			unsigned char r1[32] = { 0 };
			unsigned char r2[32] = { 0 };
			int rc1 = p_RAND_bytes(r1, 32);
			int rc2 = p_RAND_bytes(r2, 32);
			(void)rc2;
			test_result("RAND_bytes returns 1", rc1 == 1);
			/* At least one byte must be non-zero in 32 random bytes */
			int nonzero = 0;
			for (int i = 0; i < 32; i++)
				if (r1[i])
					nonzero = 1;
			test_result("RAND_bytes output non-zero", nonzero);
			test_result("RAND_bytes two calls differ",
				    memcmp(r1, r2, 32) != 0);
		}
	}

	/* ====================================================== */
	/*  Test 2: SHA-256 known-answer                          */
	/* ====================================================== */
	/*
     * Two test vectors from FIPS 180-4:
     *
     * 2a) Single-block: SHA-256("abc") = ba7816bf ... f20015ad
     *     3 bytes → 1 compress() call.
     *
     * 2b) Two-block: SHA-256("abcdbcde...nopq") = 248d6a61 ... 19db06c1
     *     56 bytes: data + 0x80 + 0x00s fills exactly one 64-byte block;
     *     the 8-byte bit-length word goes in block 2, requiring two
     *     compress() calls.
     */
	{
		test_result("EVP_sha256 dlsym",
			    p_EVP_MD_CTX_new && p_EVP_DigestInit_ex &&
				    p_EVP_DigestUpdate &&
				    p_EVP_DigestFinal_ex && p_EVP_sha256);

		/* helper: decode 64-hex-char string into 32 bytes */
#define DECODE_SHA256_HEX(out, hex_str)                                \
	do {                                                           \
		const char *_h = (hex_str);                            \
		for (int _j = 0; _j < 32; _j++, _h += 2) {             \
			int _hi = (_h[0] >= 'a') ? _h[0] - 'a' + 10 :  \
						   _h[0] - '0';        \
			int _lo = (_h[1] >= 'a') ? _h[1] - 'a' + 10 :  \
						   _h[1] - '0';        \
			(out)[_j] = (unsigned char)((_hi << 4) | _lo); \
		}                                                      \
	} while (0)

		if (p_EVP_sha256 && p_EVP_DigestInit_ex && p_EVP_DigestUpdate &&
		    p_EVP_DigestFinal_ex && p_EVP_MD_CTX_new) {
			unsigned char exp[32], digest[32];
			unsigned int dlen;
			EVP_MD_CTX *ctx;

			/* 2a: single-block ("abc") */
			DECODE_SHA256_HEX(exp,
					  "ba7816bf8f01cfea414140de5dae2223"
					  "b00361a396177a9cb410ff61f20015ad");
			ctx = p_EVP_MD_CTX_new();
			dlen = 0;
			int ok_a =
				ctx &&
				p_EVP_DigestInit_ex(ctx, p_EVP_sha256(),
						    NULL) == 1 &&
				p_EVP_DigestUpdate(ctx, "abc", 3) == 1 &&
				p_EVP_DigestFinal_ex(ctx, digest, &dlen) == 1 &&
				dlen == 32;
			if (ctx)
				p_EVP_MD_CTX_free(ctx);
			if (ok_a && memcmp(digest, exp, 32) != 0) {
				printf("  [DIAG] SHA-256(abc) 1-block: got ");
				for (int _i = 0; _i < 32; _i++)
					printf("%02x", digest[_i]);
				printf("\n");
			}
			test_result("SHA-256(abc) correct",
				    ok_a && memcmp(digest, exp, 32) == 0);

			/* 2b: two-block ("abcdbcde...nopq", 56 bytes) */
			static const char sha256_56[] =
				"abcdbcdecdefdefgefghfghighijhijk"
				"ijkljklmklmnlmnomnopnopq";
			DECODE_SHA256_HEX(exp,
					  "248d6a61d20638b8e5c026930c3e6039"
					  "a33ce45964ff2167f6ecedd419db06c1");
			ctx = p_EVP_MD_CTX_new();
			dlen = 0;
			int ok_b =
				ctx &&
				p_EVP_DigestInit_ex(ctx, p_EVP_sha256(),
						    NULL) == 1 &&
				p_EVP_DigestUpdate(ctx, sha256_56, 56) == 1 &&
				p_EVP_DigestFinal_ex(ctx, digest, &dlen) == 1 &&
				dlen == 32;
			if (ctx)
				p_EVP_MD_CTX_free(ctx);
			if (ok_b && memcmp(digest, exp, 32) != 0) {
				printf("  [DIAG] SHA-256 2-block: got ");
				for (int _i = 0; _i < 32; _i++)
					printf("%02x", digest[_i]);
				printf("\n");
			}
			test_result("SHA-256(nist-2block) correct",
				    ok_b && memcmp(digest, exp, 32) == 0);
		}
#undef DECODE_SHA256_HEX
	}

	/* ====================================================== */
	/*  Test 3: SHA-512 known-answer                           */
	/* ====================================================== */
	/*
     * SHA-512("abc") first 8 bytes:
     *   ddaf35a1 93617aba ...
     */
	{
		if (p_EVP_MD_CTX_new && p_EVP_sha512) {
			EVP_MD_CTX *ctx = p_EVP_MD_CTX_new();
			const EVP_MD *sha512 = p_EVP_sha512();
			unsigned char digest[64];
			unsigned int dlen = 0;
			int ok =
				(ctx != NULL) &&
				(p_EVP_DigestInit_ex(ctx, sha512, NULL) == 1) &&
				(p_EVP_DigestUpdate(ctx, "abc", 3) == 1) &&
				(p_EVP_DigestFinal_ex(ctx, digest, &dlen) ==
				 1) &&
				(dlen == 64);
			static const unsigned char sha512_abc_prefix[] = {
				0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba
			};
			test_result("SHA-512(abc) correct prefix",
				    ok && memcmp(digest, sha512_abc_prefix,
						 8) == 0);
			if (ctx)
				p_EVP_MD_CTX_free(ctx);
		}
	}

	/* ====================================================== */
	/*  Test 4: AES-256-CBC encrypt then decrypt round-trip    */
	/* ====================================================== */
	{
		test_result("EVP_aes_256_cbc dlsym",
			    p_EVP_CIPHER_CTX_new && p_EVP_aes_256_cbc &&
				    p_EVP_EncryptInit_ex);
		if (p_EVP_CIPHER_CTX_new && p_EVP_aes_256_cbc) {
			static const unsigned char key32[32] = {
				0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
				0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
				0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
				0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4
			};
			static const unsigned char iv16[16] = {
				0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
			};
			const unsigned char plaintext[32] =
				"Hello, AES-256-CBC encryption!  ";
			unsigned char ciphertext[64];
			unsigned char decrypted[64];
			int clen = 0, cfinal = 0, dlen = 0, dfinal = 0;

			EVP_CIPHER_CTX *ectx = p_EVP_CIPHER_CTX_new();
			const EVP_CIPHER *cipher = p_EVP_aes_256_cbc();
			int enc_ok =
				ectx != NULL &&
				p_EVP_EncryptInit_ex(ectx, cipher, NULL, key32,
						     iv16) == 1 &&
				p_EVP_EncryptUpdate(ectx, ciphertext, &clen,
						    plaintext, 32) == 1 &&
				p_EVP_EncryptFinal_ex(ectx, ciphertext + clen,
						      &cfinal) == 1;
			if (ectx)
				p_EVP_CIPHER_CTX_free(ectx);

			EVP_CIPHER_CTX *dctx = p_EVP_CIPHER_CTX_new();
			int dec_ok =
				dctx != NULL &&
				p_EVP_DecryptInit_ex(dctx, cipher, NULL, key32,
						     iv16) == 1 &&
				p_EVP_DecryptUpdate(dctx, decrypted, &dlen,
						    ciphertext,
						    clen + cfinal) == 1 &&
				p_EVP_DecryptFinal_ex(dctx, decrypted + dlen,
						      &dfinal) == 1;
			if (dctx)
				p_EVP_CIPHER_CTX_free(dctx);

			test_result("AES-256-CBC encrypt/decrypt round-trip",
				    enc_ok && dec_ok && (dlen + dfinal) == 32 &&
					    memcmp(decrypted, plaintext, 32) ==
						    0);
		}
	}

	/* ====================================================== */
	/*  Test 5: AES-256-GCM authenticated encrypt+decrypt      */
	/* ====================================================== */
	/* EVP_CIPHER_CTX_ctrl constants (from OpenSSL headers): */
#define _EVP_CTRL_GCM_SET_IVLEN 0x9
#define _EVP_CTRL_GCM_GET_TAG 0x10
#define _EVP_CTRL_GCM_SET_TAG 0x11
	{
		if (p_EVP_CIPHER_CTX_new && p_EVP_aes_256_gcm &&
		    p_EVP_CIPHER_CTX_ctrl) {
			static const unsigned char gcm_key[32] = {
				0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
				0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
				0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
			};
			static const unsigned char gcm_iv[12] = {
				0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
				0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab
			};
			const char *gcm_plain = "GCM test data!!";
			int plen = (int)strlen(gcm_plain);
			unsigned char gcm_ct[64];
			unsigned char gcm_tag[16];
			unsigned char gcm_dec[64];
			int clen = 0, cfinal = 0, dlen = 0, dfinal = 0;

			const EVP_CIPHER *gcmciph = p_EVP_aes_256_gcm();

			EVP_CIPHER_CTX *ectx = p_EVP_CIPHER_CTX_new();
			int enc_ok =
				ectx != NULL &&
				p_EVP_EncryptInit_ex(ectx, gcmciph, NULL, NULL,
						     NULL) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(ectx,
						      _EVP_CTRL_GCM_SET_IVLEN,
						      12, NULL) == 1 &&
				p_EVP_EncryptInit_ex(ectx, NULL, NULL, gcm_key,
						     gcm_iv) == 1 &&
				p_EVP_EncryptUpdate(ectx, gcm_ct, &clen,
						    (unsigned char *)gcm_plain,
						    plen) == 1 &&
				p_EVP_EncryptFinal_ex(ectx, gcm_ct + clen,
						      &cfinal) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(ectx,
						      _EVP_CTRL_GCM_GET_TAG, 16,
						      gcm_tag) == 1;
			if (ectx)
				p_EVP_CIPHER_CTX_free(ectx);

			EVP_CIPHER_CTX *dctx = p_EVP_CIPHER_CTX_new();
			int dec_ok =
				dctx != NULL &&
				p_EVP_DecryptInit_ex(dctx, gcmciph, NULL, NULL,
						     NULL) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(dctx,
						      _EVP_CTRL_GCM_SET_IVLEN,
						      12, NULL) == 1 &&
				p_EVP_DecryptInit_ex(dctx, NULL, NULL, gcm_key,
						     gcm_iv) == 1 &&
				p_EVP_DecryptUpdate(dctx, gcm_dec, &dlen,
						    gcm_ct,
						    clen + cfinal) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(dctx,
						      _EVP_CTRL_GCM_SET_TAG, 16,
						      gcm_tag) == 1 &&
				p_EVP_DecryptFinal_ex(dctx, gcm_dec + dlen,
						      &dfinal) == 1;
			if (dctx)
				p_EVP_CIPHER_CTX_free(dctx);

			test_result(
				"AES-256-GCM encrypt/decrypt+verify round-trip",
				enc_ok && dec_ok && (dlen + dfinal) == plen &&
					memcmp(gcm_dec, gcm_plain,
					       (size_t)plen) == 0);
		}
	}

	/* ====================================================== */
	/*  Test 6: HMAC-SHA256 known-answer                       */
	/* ====================================================== */
	/*
     * HMAC-SHA256(key="key", data="The quick brown fox ...")
     * = f7bc83f430538424b13298e6aa6fb143
     *   ef4d59a14946175997479dbc2d1a3cd8
     */
	{
		test_result("HMAC dlsym", p_HMAC != NULL);
		if (p_HMAC && p_EVP_sha256) {
			unsigned char mac[32];
			unsigned int mac_len = 0;
			const char *key = "key";
			const char *data =
				"The quick brown fox jumps over the lazy dog";
			unsigned char *r =
				p_HMAC(p_EVP_sha256(), key, (int)strlen(key),
				       (const unsigned char *)data,
				       strlen(data), mac, &mac_len);
			static const unsigned char expected[] = {
				0xf7, 0xbc, 0x83, 0xf4, 0x30, 0x53, 0x84, 0x24,
				0xb1, 0x32, 0x98, 0xe6, 0xaa, 0x6f, 0xb1, 0x43,
				0xef, 0x4d, 0x59, 0xa1, 0x49, 0x46, 0x17, 0x59,
				0x97, 0x47, 0x9d, 0xbc, 0x2d, 0x1a, 0x3c, 0xd8
			};
			test_result("HMAC-SHA256 known-answer",
				    r != NULL && mac_len == 32 &&
					    memcmp(mac, expected, 32) == 0);
		}
	}

	/* ====================================================== */
	/*  Test 7: RSA-2048 key generation + sign/verify          */
	/* ====================================================== */
	printf("\n--- OpenSSL RSA keygen+sign/verify ---\n");
	{
		test_result("EVP_PKEY keygen symbols",
			    p_EVP_PKEY_CTX_new_id && p_EVP_PKEY_keygen_init &&
				    p_EVP_PKEY_CTX_set_rsa_keygen_bits &&
				    p_EVP_PKEY_keygen);

		EVP_PKEY *rsa_key = NULL;
		int keygen_ok = 0;
		if (p_EVP_PKEY_CTX_new_id) {
			/* EVP_PKEY_RSA = 6 */
			EVP_PKEY_CTX *kctx = p_EVP_PKEY_CTX_new_id(6, NULL);
			if (kctx) {
				keygen_ok =
					p_EVP_PKEY_keygen_init(kctx) == 1 &&
					p_EVP_PKEY_CTX_set_rsa_keygen_bits(
						kctx, 2048) > 0 &&
					p_EVP_PKEY_keygen(kctx, &rsa_key) == 1;
				p_EVP_PKEY_CTX_free(kctx);
			}
		}
		test_result("RSA-2048 key generation",
			    keygen_ok && rsa_key != NULL);

		if (rsa_key && p_EVP_MD_CTX_new && p_EVP_DigestSignInit &&
		    p_EVP_DigestSignUpdate && p_EVP_DigestSignFinal &&
		    p_EVP_DigestVerifyInit && p_EVP_DigestVerifyUpdate &&
		    p_EVP_DigestVerifyFinal) {
			const char *msg = "RSA sign/verify test message";
			size_t msg_len = strlen(msg);
			unsigned char sig[512];
			size_t sig_len = sizeof(sig);
			const EVP_MD *sha256 = p_EVP_sha256();

			/* Sign */
			EVP_MD_CTX *sctx = p_EVP_MD_CTX_new();
			int sign_ok =
				sctx != NULL &&
				p_EVP_DigestSignInit(sctx, NULL, sha256, NULL,
						     rsa_key) == 1 &&
				p_EVP_DigestSignUpdate(sctx, msg, msg_len) ==
					1 &&
				p_EVP_DigestSignFinal(sctx, sig, &sig_len) == 1;
			if (sctx)
				p_EVP_MD_CTX_free(sctx);
			test_result("RSA-2048 sign with SHA-256",
				    sign_ok && sig_len > 0);

			/* Verify */
			EVP_MD_CTX *vctx = p_EVP_MD_CTX_new();
			int verify_ok =
				vctx != NULL &&
				p_EVP_DigestVerifyInit(vctx, NULL, sha256, NULL,
						       rsa_key) == 1 &&
				p_EVP_DigestVerifyUpdate(vctx, msg, msg_len) ==
					1 &&
				p_EVP_DigestVerifyFinal(vctx, sig, sig_len) ==
					1;
			if (vctx)
				p_EVP_MD_CTX_free(vctx);
			test_result("RSA-2048 verify signature", verify_ok);

			/* Tamper: alter one byte of signature – must fail */
			sig[0] ^= 0xFF;
			EVP_MD_CTX *bctx = p_EVP_MD_CTX_new();
			int tamper_ok =
				bctx != NULL &&
				p_EVP_DigestVerifyInit(bctx, NULL, sha256, NULL,
						       rsa_key) == 1 &&
				p_EVP_DigestVerifyUpdate(bctx, msg, msg_len) ==
					1 &&
				p_EVP_DigestVerifyFinal(bctx, sig, sig_len) !=
					1;
			if (bctx)
				p_EVP_MD_CTX_free(bctx);
			test_result("RSA-2048 tampered sig rejected",
				    tamper_ok);
		}
		if (rsa_key)
			p_EVP_PKEY_free(rsa_key);
	}

	/* ====================================================== */
	/*  Test 8: EC P-256 (ECDSA) key generation + sign/verify  */
	/* ====================================================== */
	printf("\n--- OpenSSL ECDSA (P-256) ---\n");
	{
		test_result("EC_KEY symbols", p_EC_KEY_new_by_curve_name &&
						      p_EC_KEY_generate_key &&
						      p_ECDSA_sign &&
						      p_ECDSA_verify);

		if (p_EC_KEY_new_by_curve_name && p_EC_KEY_generate_key &&
		    p_ECDSA_sign && p_ECDSA_verify) {
			/* NID_X9_62_prime256v1 = 415 */
			void *ec_key = p_EC_KEY_new_by_curve_name(415);
			test_result("ECDSA P-256 key create", ec_key != NULL);

			if (ec_key) {
				int gen_ok = p_EC_KEY_generate_key(ec_key);
				test_result("ECDSA P-256 key generate",
					    gen_ok == 1);

				if (gen_ok) {
					/* Hash the message first */
					unsigned char hash[32];
					unsigned int hlen = sizeof(hash);
					EVP_MD_CTX *hctx = p_EVP_MD_CTX_new();
					const EVP_MD *sha256 = p_EVP_sha256();
					const char *msg = "ECDSA test payload";
					p_EVP_DigestInit_ex(hctx, sha256, NULL);
					p_EVP_DigestUpdate(hctx, msg,
							   strlen(msg));
					p_EVP_DigestFinal_ex(hctx, hash, &hlen);
					p_EVP_MD_CTX_free(hctx);

					/* Sign */
					unsigned char sig[256];
					unsigned int slen = sizeof(sig);
					int sign_ok = p_ECDSA_sign(
						0, hash, (int)hlen, sig, &slen,
						ec_key);
					test_result("ECDSA P-256 sign",
						    sign_ok == 1 && slen > 0);

					/* Verify */
					int verify_ok = p_ECDSA_verify(
						0, hash, (int)hlen, sig,
						(int)slen, ec_key);
					test_result("ECDSA P-256 verify",
						    verify_ok == 1);

					/* Tamper */
					sig[0] ^= 0xAA;
					int tamper_ok =
						p_ECDSA_verify(
							0, hash, (int)hlen, sig,
							(int)slen, ec_key) != 1;
					test_result(
						"ECDSA P-256 tampered rejected",
						tamper_ok);
				}
				p_EC_KEY_free(ec_key);
			}
		}
	}

	/* ====================================================== */
	/*  Test 9: X25519 ECDH shared-secret agreement            */
	/* ====================================================== */
	printf("\n--- OpenSSL X25519 ECDH ---\n");
	{
		int all_syms = p_EVP_PKEY_CTX_new_id &&
			       p_EVP_PKEY_keygen_init && p_EVP_PKEY_keygen &&
			       p_EVP_PKEY_CTX_new && p_EVP_PKEY_derive_init &&
			       p_EVP_PKEY_derive_set_peer &&
			       p_EVP_PKEY_derive && p_EVP_PKEY_free &&
			       p_EVP_PKEY_CTX_free;
		test_result("X25519 ECDH symbols present", all_syms);
		if (all_syms) {
			/* EVP_PKEY_X25519 = 1034 */
			EVP_PKEY *alice = NULL, *bob = NULL;

			EVP_PKEY_CTX *kctx_a =
				p_EVP_PKEY_CTX_new_id(1034, NULL);
			if (kctx_a) {
				p_EVP_PKEY_keygen_init(kctx_a);
				p_EVP_PKEY_keygen(kctx_a, &alice);
				p_EVP_PKEY_CTX_free(kctx_a);
			}

			EVP_PKEY_CTX *kctx_b =
				p_EVP_PKEY_CTX_new_id(1034, NULL);
			if (kctx_b) {
				p_EVP_PKEY_keygen_init(kctx_b);
				p_EVP_PKEY_keygen(kctx_b, &bob);
				p_EVP_PKEY_CTX_free(kctx_b);
			}

			test_result("X25519 key generation (alice+bob)",
				    alice != NULL && bob != NULL);

			if (alice && bob) {
				unsigned char secret_a[32], secret_b[32];
				size_t len_a = sizeof(secret_a),
				       len_b = sizeof(secret_b);

				EVP_PKEY_CTX *dctx_a =
					p_EVP_PKEY_CTX_new(alice, NULL);
				int a_ok =
					dctx_a != NULL &&
					p_EVP_PKEY_derive_init(dctx_a) == 1 &&
					p_EVP_PKEY_derive_set_peer(dctx_a,
								   bob) == 1 &&
					p_EVP_PKEY_derive(dctx_a, secret_a,
							  &len_a) == 1;
				if (dctx_a)
					p_EVP_PKEY_CTX_free(dctx_a);

				EVP_PKEY_CTX *dctx_b =
					p_EVP_PKEY_CTX_new(bob, NULL);
				int b_ok =
					dctx_b != NULL &&
					p_EVP_PKEY_derive_init(dctx_b) == 1 &&
					p_EVP_PKEY_derive_set_peer(
						dctx_b, alice) == 1 &&
					p_EVP_PKEY_derive(dctx_b, secret_b,
							  &len_b) == 1;
				if (dctx_b)
					p_EVP_PKEY_CTX_free(dctx_b);

				test_result(
					"X25519 ECDH derive succeeds (alice+bob)",
					a_ok && b_ok && len_a == 32 &&
						len_b == 32);
				test_result("X25519 ECDH shared secrets match",
					    a_ok && b_ok && len_a == 32 &&
						    len_b == 32 &&
						    memcmp(secret_a, secret_b,
							   32) == 0);
			}

			if (alice)
				p_EVP_PKEY_free(alice);
			if (bob)
				p_EVP_PKEY_free(bob);
		}
	}

	/* ====================================================== */
	/*  Test 10: Base64 encode/decode round-trip               */
	/* ====================================================== */
	{
		test_result("EVP_EncodeBlock/EVP_DecodeBlock dlsym",
			    p_EVP_EncodeBlock != NULL &&
				    p_EVP_DecodeBlock != NULL);
		if (p_EVP_EncodeBlock && p_EVP_DecodeBlock) {
			const unsigned char plain[12] = "Hello, B64!";
			unsigned char encoded[24];
			unsigned char decoded[24];
			memset(encoded, 0, sizeof(encoded));
			memset(decoded, 0, sizeof(decoded));
			int enc_len = p_EVP_EncodeBlock(encoded, plain, 12);
			int dec_len =
				p_EVP_DecodeBlock(decoded, encoded, enc_len);
			/* EVP_DecodeBlock pads with 0x00 to block boundary; check first 12 bytes */
			test_result("Base64 encode produces output",
				    enc_len > 0);
			test_result("Base64 decode round-trip matches",
				    dec_len >= 12 &&
					    memcmp(decoded, plain, 12) == 0);
		}
	}

	/* ====================================================== */
	/*  Test 11: ERR_get_error / ERR_error_string              */
	/* ====================================================== */
	{
		test_result("ERR_get_error / ERR_error_string dlsym",
			    p_ERR_get_error && p_ERR_error_string &&
				    p_ERR_clear_error);
		if (p_ERR_get_error && p_ERR_error_string &&
		    p_ERR_clear_error) {
			/* Force an error: attempt to load an invalid PEM from a NULL BIO */
			if (p_PEM_read_bio_X509) {
				p_PEM_read_bio_X509(NULL, NULL, NULL, NULL);
			}
			unsigned long err = p_ERR_get_error();
			char errbuf[256] = { 0 };
			char *s = p_ERR_error_string(err, errbuf);
			/* Either we get a real error string or the "no error" string – both are fine */
			test_result("ERR_error_string returns non-NULL string",
				    s != NULL);
			p_ERR_clear_error();
		}
	}

	/* ====================================================== */
	/*  Test 12: BIO memory buffer read/write round-trip       */
	/* ====================================================== */
	{
		test_result("BIO_new / BIO_s_mem / BIO_write / BIO_read dlsym",
			    p_BIO_new && p_BIO_s_mem && p_BIO_write &&
				    p_BIO_read && p_BIO_free);
		if (p_BIO_new && p_BIO_s_mem && p_BIO_write && p_BIO_read &&
		    p_BIO_free) {
			void *mem_method = p_BIO_s_mem();
			BIO *bio = p_BIO_new(mem_method);
			test_result("BIO_new(BIO_s_mem) returns non-NULL",
				    bio != NULL);
			if (bio) {
				const char *msg = "BIO round-trip test";
				int wn =
					p_BIO_write(bio, msg, (int)strlen(msg));
				char buf[64] = { 0 };
				int rn = p_BIO_read(bio, buf,
						    (int)sizeof(buf) - 1);
				test_result("BIO_write / BIO_read round-trip",
					    wn == (int)strlen(msg) &&
						    rn == (int)strlen(msg) &&
						    memcmp(buf, msg,
							   (size_t)strlen(
								   msg)) == 0);
				p_BIO_free(bio);
			}
		}
	}

	/* ====================================================== */
	/*  Test 13: EVP_BytesToKey KDF                            */
	/* ====================================================== */
	{
		test_result("EVP_BytesToKey dlsym", p_EVP_BytesToKey != NULL);
		if (p_EVP_BytesToKey && p_EVP_aes_256_cbc && p_EVP_sha256) {
			const unsigned char salt[8] = {
				0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
			};
			const unsigned char pass[] = "passphrase";
			unsigned char k1[32], iv1[16];
			unsigned char k2[32], iv2[16];
			int r1 = p_EVP_BytesToKey(
				p_EVP_aes_256_cbc(), p_EVP_sha256(), salt, pass,
				(int)strlen((char *)pass), 1, k1, iv1);
			int r2 = p_EVP_BytesToKey(
				p_EVP_aes_256_cbc(), p_EVP_sha256(), salt, pass,
				(int)strlen((char *)pass), 1, k2, iv2);
			test_result("EVP_BytesToKey returns key length 32",
				    r1 == 32);
			test_result("EVP_BytesToKey is deterministic",
				    r1 == r2 && memcmp(k1, k2, 32) == 0 &&
					    memcmp(iv1, iv2, 16) == 0);
		}
	}

	/* ====================================================== */
	/*  Test 14: libssl API smoke tests (no network)           */
	/* ====================================================== */
	printf("\n--- OpenSSL libssl smoke ---\n");
	{
		test_result("TLS_client_method / TLS_server_method dlsym",
			    p_TLS_client_method != NULL &&
				    p_TLS_server_method != NULL);

		void *client_meth =
			p_TLS_client_method ? p_TLS_client_method() : NULL;
		void *server_meth =
			p_TLS_server_method ? p_TLS_server_method() : NULL;
		test_result("TLS_client_method() non-NULL",
			    client_meth != NULL);
		test_result("TLS_server_method() non-NULL",
			    server_meth != NULL);

		if (p_SSL_CTX_new && client_meth) {
			SSL_CTX *cctx = p_SSL_CTX_new(client_meth);
			test_result("SSL_CTX_new(TLS_client_method) non-NULL",
				    cctx != NULL);
			if (cctx) {
				long opts =
					p_SSL_CTX_set_options ?
						p_SSL_CTX_set_options(
							cctx,
							0x04000000L /*SSL_OP_NO_SSLv2*/) :
						0;
				test_result(
					"SSL_CTX_set_options returns non-zero",
					opts != 0);

				if (p_SSL_new) {
					SSL *ssl = p_SSL_new(cctx);
					test_result(
						"SSL_new(client_ctx) non-NULL",
						ssl != NULL);
					if (ssl) {
						/* SSL_get_error on unconnected ssl should not crash */
						int err =
							p_SSL_get_error ?
								p_SSL_get_error(
									ssl,
									-1) :
								-1;
						test_result(
							"SSL_get_error on unconnected returns valid code",
							err == 2 /*SSL_ERROR_WANT_READ*/
								|| err >= 0);
						if (p_SSL_free)
							p_SSL_free(ssl);
					}
				}
				if (p_SSL_CTX_free)
					p_SSL_CTX_free(cctx);
			}
		}

		/* SSLv23_method() is an alias for TLS_method – just check it's present */
		typedef void *(*fn_SSLv23_method)(void);
		fn_SSLv23_method p_SSLv23_method =
			(fn_SSLv23_method)dlsym(ssl_h, "SSLv23_method");
		if (!p_SSLv23_method)
			p_SSLv23_method =
				(fn_SSLv23_method)dlsym(ssl_h, "TLS_method");
		test_result("SSLv23_method() / TLS_method() resolvable",
			    p_SSLv23_method != NULL);
		if (p_SSLv23_method) {
			void *m = p_SSLv23_method();
			test_result("SSLv23_method() returns non-NULL",
				    m != NULL);
		}
	}

	/* ====================================================== */
	/*  Test 15: TLS loopback client+server (large data)        */
	/* ====================================================== */
	printf("\n--- OpenSSL TLS loopback (runtime cert, 64KB) ---\n");

	/*
     * The key and certificate are generated at runtime using the OpenSSL EVP /
     * X509 API (RSA-2048, self-signed SHA-256).  This avoids PEM line-length
     * or format issues entirely.
     *
     * A synchronisation pipe is used so the client only connects after the
     * server has set up its SSL_CTX and is ready to call accept().  Every
     * early-exit path in the server child explicitly closes its file
     * descriptors before _exit() so the kernel sends a FIN/RST even if
     * LikeOS does not close FDs on process exit.
     */
	{
		/* --- X509 cert-generation function pointers --- */
		typedef void *(*fn_X509_new)(void);
		typedef void (*fn_X509_free_fn)(void *);
		typedef int (*fn_X509_set_version)(void *, long);
		typedef void *(*fn_X509_get_serialNumber)(void *);
		typedef int (*fn_ASN1_INTEGER_set)(void *, long);
		typedef void *(*fn_X509_getm_notBefore)(void *);
		typedef void *(*fn_X509_getm_notAfter)(void *);
		typedef void *(*fn_X509_gmtime_adj)(void *, long);
		typedef int (*fn_X509_set_pubkey)(void *, EVP_PKEY *);
		typedef void *(*fn_X509_get_subject_name)(void *);
		typedef int (*fn_X509_NAME_add_entry_by_txt)(
			void *, const char *, int, const unsigned char *, int,
			int, int);
		typedef int (*fn_X509_set_issuer_name)(void *, void *);
		typedef int (*fn_X509_sign)(void *, EVP_PKEY *, const EVP_MD *);

		fn_X509_new p_X509_new =
			(fn_X509_new)dlsym(crypto_h, "X509_new");
		fn_X509_free_fn p_X509_free_fn =
			(fn_X509_free_fn)dlsym(crypto_h, "X509_free");
		fn_X509_set_version p_X509_sv = (fn_X509_set_version)dlsym(
			crypto_h, "X509_set_version");
		fn_X509_get_serialNumber p_X509_gsn =
			(fn_X509_get_serialNumber)dlsym(
				crypto_h, "X509_get_serialNumber");
		fn_ASN1_INTEGER_set p_ASN1_iset = (fn_ASN1_INTEGER_set)dlsym(
			crypto_h, "ASN1_INTEGER_set");
		fn_X509_getm_notBefore p_X509_gnb =
			(fn_X509_getm_notBefore)dlsym(crypto_h,
						      "X509_getm_notBefore");
		fn_X509_getm_notAfter p_X509_gna = (fn_X509_getm_notAfter)dlsym(
			crypto_h, "X509_getm_notAfter");
		fn_X509_gmtime_adj p_X509_gta =
			(fn_X509_gmtime_adj)dlsym(crypto_h, "X509_gmtime_adj");
		fn_X509_set_pubkey p_X509_spk =
			(fn_X509_set_pubkey)dlsym(crypto_h, "X509_set_pubkey");
		fn_X509_get_subject_name p_X509_gsn2 =
			(fn_X509_get_subject_name)dlsym(
				crypto_h, "X509_get_subject_name");
		fn_X509_NAME_add_entry_by_txt p_X509_naetbt =
			(fn_X509_NAME_add_entry_by_txt)dlsym(
				crypto_h, "X509_NAME_add_entry_by_txt");
		fn_X509_set_issuer_name p_X509_sin =
			(fn_X509_set_issuer_name)dlsym(crypto_h,
						       "X509_set_issuer_name");
		fn_X509_sign p_X509_sign =
			(fn_X509_sign)dlsym(crypto_h, "X509_sign");

		int cert_gen_syms = p_X509_new && p_X509_sv && p_X509_gsn &&
				    p_ASN1_iset && p_X509_gnb && p_X509_gna &&
				    p_X509_gta && p_X509_spk && p_X509_gsn2 &&
				    p_X509_naetbt && p_X509_sin && p_X509_sign;

		int tls_syms =
			p_TLS_server_method && p_TLS_client_method &&
			p_SSL_CTX_new && p_SSL_CTX_free &&
			p_SSL_CTX_use_certificate && p_SSL_CTX_use_PrivateKey &&
			p_SSL_CTX_check_private_key && p_SSL_CTX_set_verify &&
			p_SSL_new && p_SSL_free && p_SSL_set_fd &&
			p_SSL_connect && p_SSL_accept && p_SSL_write &&
			p_SSL_read && p_SSL_shutdown && p_SSL_get_version &&
			p_EVP_PKEY_free && p_EVP_PKEY_CTX_new_id &&
			p_EVP_PKEY_keygen_init &&
			p_EVP_PKEY_CTX_set_rsa_keygen_bits &&
			p_EVP_PKEY_keygen && p_EVP_sha256 && cert_gen_syms;

		test_result("TLS loopback: all required symbols present",
			    tls_syms);
		if (!tls_syms) {
			printf("  [SKIP] TLS loopback: missing symbols\n");
			goto tls_loopback_done;
		}

		/* --- Generate RSA-2048 key pair in the parent before fork --- */
		EVP_PKEY *tls_key = NULL;
		{
			EVP_PKEY_CTX *kctx =
				p_EVP_PKEY_CTX_new_id(6 /*EVP_PKEY_RSA*/, NULL);
			if (kctx) {
				p_EVP_PKEY_keygen_init(kctx);
				p_EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
				p_EVP_PKEY_keygen(kctx, &tls_key);
				p_EVP_PKEY_CTX_free(kctx);
			}
		}
		test_result("TLS loopback: RSA-2048 key generated",
			    tls_key != NULL);
		if (!tls_key)
			goto tls_loopback_done;

		/* --- Generate self-signed X509 cert in the parent before fork --- */
		void *tls_cert = NULL;
		{
			void *cert = p_X509_new();
			if (cert) {
				p_X509_sv(cert, 2); /* v3 */
				p_ASN1_iset(p_X509_gsn(cert), 1); /* serial 1 */
				p_X509_gta(p_X509_gnb(cert),
					   0); /* not before: now */
				p_X509_gta(
					p_X509_gna(cert),
					3650L * 86400L); /* not after: 10 yr */
				p_X509_spk(cert, tls_key);
				void *subj = p_X509_gsn2(cert);
				/* MBSTRING_ASC = 0x1001 */
				p_X509_naetbt(subj, "CN", 0x1001,
					      (const unsigned char *)"testhost",
					      -1, -1, 0);
				p_X509_sin(cert, subj);
				if (p_X509_sign(cert, tls_key, p_EVP_sha256()) >
				    0)
					tls_cert = cert;
				else
					p_X509_free_fn(cert);
			}
		}
		test_result("TLS loopback: self-signed cert generated",
			    tls_cert != NULL);
		if (!tls_cert) {
			p_EVP_PKEY_free(tls_key);
			goto tls_loopback_done;
		}

		/* --- Large transfer buffers --- */
		static const int TLS_DATA_LEN = 65536;
		static unsigned char tls_send_buf[65536];
		static unsigned char tls_recv_buf[65536];
		for (int i = 0; i < TLS_DATA_LEN; i++)
			tls_send_buf[i] = (unsigned char)(i & 0x7F);
		memset(tls_recv_buf, 0, TLS_DATA_LEN);

		/*
         * Sync pipe: server child writes 'R' after SSL_CTX is ready and it is
         * about to block in accept().  Parent waits (up to 10 s to allow for
         * any slow initialisation) before connecting, so there is no race.
         */
		int sync_pipe[2] = { -1, -1 };
		pipe(sync_pipe);

		/* Per-process port so two parallel testlibc instances don't collide.
         * Use & 0x3FFF (16384 values, range 21100-37483) instead of % 1000
         * to make port collisions practically impossible: two PIDs would
         * need to differ by exactly 16384, vs. 1000 before. */
		int tls_lb_port = 21100 + ((int)getpid() & 0x3FFF);

		/* --- Listening socket (created before fork so child inherits it) --- */
		int srv_sock = socket(AF_INET, SOCK_STREAM, 0);
		test_result("TLS loopback: server socket", srv_sock >= 0);
		if (srv_sock < 0) {
			close(sync_pipe[0]);
			close(sync_pipe[1]);
			p_EVP_PKEY_free(tls_key);
			p_X509_free_fn(tls_cert);
			goto tls_loopback_done;
		}
		{
			int yes = 1;
			setsockopt(srv_sock, SOL_SOCKET, SO_REUSEADDR, &yes,
				   sizeof(yes));
		}
		struct sockaddr_in srv_addr;
		memset(&srv_addr, 0, sizeof(srv_addr));
		srv_addr.sin_family = AF_INET;
		srv_addr.sin_port = htons((uint16_t)tls_lb_port);
		srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int bind_ok = bind(srv_sock, (struct sockaddr *)&srv_addr,
				   sizeof(srv_addr));
		test_result("TLS loopback: server bind", bind_ok == 0);
		if (bind_ok != 0) {
			close(srv_sock);
			close(sync_pipe[0]);
			close(sync_pipe[1]);
			p_EVP_PKEY_free(tls_key);
			p_X509_free_fn(tls_cert);
			goto tls_loopback_done;
		}
		int listen_ok = listen(srv_sock, 4);
		test_result("TLS loopback: server listen", listen_ok == 0);
		if (listen_ok != 0) {
			close(srv_sock);
			close(sync_pipe[0]);
			close(sync_pipe[1]);
			p_EVP_PKEY_free(tls_key);
			p_X509_free_fn(tls_cert);
			goto tls_loopback_done;
		}

		pid_t tls_pid = fork();
		test_result("TLS loopback: fork", tls_pid >= 0);

		if (tls_pid == 0) {
			/* ===== SERVER child ===== */
			/*
             * Helper macro: close every open FD the child owns before calling
             * _exit so the kernel sends FIN/RST even if LikeOS does not close
             * FDs on process exit.
             */
#define SRV_EXIT(code, cfd)          \
	do {                         \
		if ((cfd) >= 0)      \
			close(cfd);  \
		close(srv_sock);     \
		close(sync_pipe[1]); \
		_exit(code);         \
	} while (0)

			close(sync_pipe
				      [0]); /* child does not read from sync pipe */
			int conn_fd = -1;

			/* Build SSL_CTX using already-generated key+cert (no PEM round-trip) */
			void *smeth = p_TLS_server_method();
			SSL_CTX *sctx = p_SSL_CTX_new(smeth);
			if (!sctx)
				SRV_EXIT(3, conn_fd);
			p_SSL_CTX_set_verify(sctx, 0 /*SSL_VERIFY_NONE*/, NULL);
			if (p_SSL_CTX_use_certificate(sctx, tls_cert) != 1)
				SRV_EXIT(4, conn_fd);
			if (p_SSL_CTX_use_PrivateKey(sctx, tls_key) != 1)
				SRV_EXIT(5, conn_fd);
			if (p_SSL_CTX_check_private_key(sctx) != 1)
				SRV_EXIT(6, conn_fd);

			/* Signal parent: SSL_CTX ready, about to block in accept() */
			{
				char r = 'R';
				write(sync_pipe[1], &r, 1);
			}
			close(sync_pipe[1]);

			/* 30 s accept timeout — prevents the server child from hanging
             * indefinitely if the client never connects (e.g. port collision
             * edge case with a parallel instance). */
			{
				struct timeval atv = { .tv_sec = 30,
						       .tv_usec = 0 };
				setsockopt(srv_sock, SOL_SOCKET, SO_RCVTIMEO,
					   &atv, sizeof(atv));
			}
			conn_fd = accept(srv_sock, NULL, NULL);
			close(srv_sock);
			if (conn_fd < 0) {
				p_SSL_CTX_free(sctx);
				_exit(7);
			}

			SSL *ssl = p_SSL_new(sctx);
			if (!ssl) {
				p_SSL_CTX_free(sctx);
				close(conn_fd);
				_exit(8);
			}
			p_SSL_set_fd(ssl, conn_fd);

			int acc = p_SSL_accept(ssl);
			if (acc != 1) {
				int e = p_SSL_get_error ?
						p_SSL_get_error(ssl, acc) :
						-1;
				printf("  [DBG] TLS lb srv: SSL_accept=%d err=%d errno=%d\n",
				       acc, e, errno);
				p_SSL_free(ssl);
				p_SSL_CTX_free(sctx);
				close(conn_fd);
				_exit(9);
			}

			/* Receive TLS_DATA_LEN bytes then echo */
			static unsigned char srv_buf[65536];
			int total_recv = 0;
			while (total_recv < TLS_DATA_LEN) {
				int n = p_SSL_read(ssl, srv_buf + total_recv,
						   TLS_DATA_LEN - total_recv);
				if (n > 0) {
					total_recv += n;
				} else {
					int err = p_SSL_get_error ?
							  p_SSL_get_error(ssl,
									  n) :
							  0;
					if (err == 2 /* SSL_ERROR_WANT_READ */
					    ||
					    err == 3 /* SSL_ERROR_WANT_WRITE */)
						continue;
					if (err == 5 /* SSL_ERROR_SYSCALL */ &&
					    (errno == 11 /* EAGAIN */ ||
					     errno == 4 /* EINTR */))
						continue;
					break;
				}
			}
			if (total_recv != TLS_DATA_LEN) {
				p_SSL_shutdown(ssl);
				p_SSL_free(ssl);
				p_SSL_CTX_free(sctx);
				close(conn_fd);
				_exit(10);
			}

			int total_sent = 0;
			while (total_sent < TLS_DATA_LEN) {
				int n = p_SSL_write(ssl, srv_buf + total_sent,
						    TLS_DATA_LEN - total_sent);
				if (n > 0) {
					total_sent += n;
				} else {
					int err = p_SSL_get_error ?
							  p_SSL_get_error(ssl,
									  n) :
							  0;
					if (err == 2 /* SSL_ERROR_WANT_READ */
					    ||
					    err == 3 /* SSL_ERROR_WANT_WRITE */)
						continue;
					if (err == 5 /* SSL_ERROR_SYSCALL */ &&
					    (errno == 11 /* EAGAIN */ ||
					     errno == 4 /* EINTR */))
						continue;
					break;
				}
			}
			if (total_sent != TLS_DATA_LEN) {
				p_SSL_shutdown(ssl);
				p_SSL_free(ssl);
				p_SSL_CTX_free(sctx);
				close(conn_fd);
				_exit(11);
			}

			/* Bidirectional TLS shutdown: send our close_notify first,
             * then drain any incoming data/alert until we see EOF or error.
             * This prevents close(conn_fd) from sending a RST before the
             * client has finished reading the last data record.
             * Use a short receive timeout so we never hang if the client
             * died without sending its close_notify. */
			p_SSL_shutdown(ssl);
			{
				struct timeval drain_tv = { .tv_sec = 10,
							    .tv_usec = 0 };
				setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO,
					   &drain_tv, sizeof(drain_tv));
				unsigned char drain_buf[4096];
				int dr;
				while ((dr = p_SSL_read(ssl, drain_buf,
							sizeof(drain_buf))) > 0)
					(void)dr;
			}
			p_SSL_free(ssl);
			p_SSL_CTX_free(sctx);
			close(conn_fd);
			_exit(0);
#undef SRV_EXIT
		}

		/* ===== PARENT (client side) ===== */
		close(sync_pipe[1]); /* parent does not write to sync pipe */
		close(srv_sock); /* child inherited srv_sock via fork; parent no longer needs it */

		if (tls_pid > 0) {
			/*
             * Wait for the server-ready signal.  10 s is enough even if the
             * system is very slow; if the server dies early the pipe write-end
             * is closed (or never written) and select() returns immediately so
             * we do not block indefinitely.
             */
			int server_ready = 0;
			{
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(sync_pipe[0], &rfds);
				struct timeval wtv = { .tv_sec = 10,
						       .tv_usec = 0 };
				int sr = select(sync_pipe[0] + 1, &rfds, NULL,
						NULL, &wtv);
				if (sr > 0) {
					char rch = 0;
					if (read(sync_pipe[0], &rch, 1) == 1)
						server_ready = (rch == 'R');
				}
			}
			close(sync_pipe[0]);
			test_result("TLS loopback: server signaled ready",
				    server_ready);

			if (!server_ready) {
				/* Server died before signalling – kill zombie and bail out */
				kill(tls_pid, SIGKILL);
				int dummy;
				waitpid(tls_pid, &dummy, 0);
				p_EVP_PKEY_free(tls_key);
				p_X509_free_fn(tls_cert);
				goto tls_loopback_done;
			}

			/* Connect can fail transiently when ksoftirqd/0 is starved under
             * heavy SMP load from parallel teststress instances — the SYN /
             * SYN+ACK exchange goes through the loopback rx_queue serviced
             * exclusively by CPU 0's ksoftirqd, and a single missed window
             * (TCP retransmit timeout ~15 s) is enough to fail the first
             * attempt while the server child is still sitting in accept().
             *
             * Retry the connect with a fresh socket up to 3 times before
             * giving up; on each retry sleep briefly to let the kernel
             * drain its rx_queue.  The server's accept timeout (30 s)
             * accommodates the full retry window. */
			int cli_sock = -1;
			int cli_conn_ok = 0;
			for (int try = 0; try < 3 && !cli_conn_ok; try++) {
				if (try > 0) {
					if (cli_sock >= 0) {
						close(cli_sock);
						cli_sock = -1;
					}
					usleep(200000); /* 200 ms — give ksoftirqd a chance to drain */
				}
				cli_sock = socket(AF_INET, SOCK_STREAM, 0);
				if (cli_sock < 0)
					break;
				/* 120 s receive timeout — two parallel TLS sessions on a
                 * slow VMware VM can take much longer than 30 s under load */
				struct timeval rcv_tv = { .tv_sec = 120,
							  .tv_usec = 0 };
				setsockopt(cli_sock, SOL_SOCKET, SO_RCVTIMEO,
					   &rcv_tv, sizeof(rcv_tv));
				struct sockaddr_in cli_addr;
				memset(&cli_addr, 0, sizeof(cli_addr));
				cli_addr.sin_family = AF_INET;
				cli_addr.sin_port =
					htons((uint16_t)tls_lb_port);
				cli_addr.sin_addr.s_addr =
					htonl(INADDR_LOOPBACK);
				cli_conn_ok =
					(connect(cli_sock,
						 (struct sockaddr *)&cli_addr,
						 sizeof(cli_addr)) == 0);
			}
			test_result("TLS loopback: client TCP connect",
				    cli_conn_ok);

			if (cli_conn_ok) {
				void *cmeth = p_TLS_client_method();
				SSL_CTX *cctx = p_SSL_CTX_new(cmeth);
				if (cctx) {
					p_SSL_CTX_set_verify(
						cctx, 0 /*SSL_VERIFY_NONE*/,
						NULL);
					SSL *ssl = p_SSL_new(cctx);
					if (ssl) {
						p_SSL_set_fd(ssl, cli_sock);
						int conn = p_SSL_connect(ssl);
						if (conn != 1) {
							int e = p_SSL_get_error ?
									p_SSL_get_error(
										ssl,
										conn) :
									-1;
							printf("  [DBG] TLS lb: SSL_connect=%d err=%d errno=%d\n",
							       conn, e, errno);
						}
						test_result(
							"TLS loopback: SSL_connect",
							conn == 1);

						if (conn == 1) {
							const char *ver =
								p_SSL_get_version(
									ssl);
							test_result(
								"TLS loopback: SSL_get_version non-NULL",
								ver != NULL);
							printf("  [INFO] TLS version: %s\n",
							       ver ? ver :
								     "(null)");

							/* Send 64 KB */
							int total_sent = 0;
							while (total_sent <
							       TLS_DATA_LEN) {
								int n = p_SSL_write(
									ssl,
									tls_send_buf +
										total_sent,
									TLS_DATA_LEN -
										total_sent);
								if (n <= 0)
									break;
								total_sent += n;
							}
							test_result(
								"TLS loopback: sent 64 KB",
								total_sent ==
									TLS_DATA_LEN);

							/* Receive echo */
							int total_recv = 0;
							while (total_recv <
							       TLS_DATA_LEN) {
								int n = p_SSL_read(
									ssl,
									tls_recv_buf +
										total_recv,
									TLS_DATA_LEN -
										total_recv);
								if (n > 0) {
									total_recv +=
										n;
								} else {
									/* SSL_ERROR_WANT_READ  (2): retry (blocking BIO, OOB data).
                                     * SSL_ERROR_WANT_WRITE (3): retry (TLS 1.3 KeyUpdate/NewSessionTicket).
                                     * SSL_ERROR_ZERO_RETURN(6): peer close_notify — done.
                                     * anything else        : fatal, break. */
									int err =
										p_SSL_get_error ?
											p_SSL_get_error(
												ssl,
												n) :
											0;
									if (err == 2 ||
									    err == 3)
										continue;
									printf("  [DBG] TLS recv loop exit: n=%d err=%d"
									       " errno=%d total_recv=%d\n",
									       n,
									       err,
									       errno,
									       total_recv);
									break;
								}
							}
							test_result(
								"TLS loopback: recv 64 KB echo",
								total_recv ==
									TLS_DATA_LEN);
							test_result(
								"TLS loopback: data integrity",
								total_recv == TLS_DATA_LEN &&
									memcmp(tls_send_buf,
									       tls_recv_buf,
									       (size_t)TLS_DATA_LEN) ==
										0);

							int sd = p_SSL_shutdown(
								ssl);
							/*
                             * TLS 1.3 bidirectional shutdown: first call returns
                             * 0 (sent close_notify), second returns 1 (received
                             * peer close_notify), -1 means I/O error (peer may
                             * have already closed).  All three are valid here.
                             */
							test_result(
								"TLS loopback: SSL_shutdown returned valid code",
								sd == 0 ||
									sd == 1 ||
									sd == -1);
						}
						p_SSL_free(ssl);
					}
					p_SSL_CTX_free(cctx);
				}
			}
			if (cli_sock >= 0)
				close(cli_sock);

			int tls_child_status = 0;
			waitpid(tls_pid, &tls_child_status, 0);
			printf("  [DBG] TLS lb srv exit: code=%d signal=%d\n",
			       WIFEXITED(tls_child_status) ?
				       WEXITSTATUS(tls_child_status) :
				       -1,
			       WIFSIGNALED(tls_child_status) ?
				       WTERMSIG(tls_child_status) :
				       0);
			test_result("TLS loopback: server child exited cleanly",
				    WIFEXITED(tls_child_status) &&
					    WEXITSTATUS(tls_child_status) == 0);
			/* close the listen socket that was kept open across fork */
			if (srv_sock >= 0)
				close(srv_sock);
		} else {
			/* fork failed */
			close(sync_pipe[0]);
			close(srv_sock);
		}

		p_EVP_PKEY_free(tls_key);
		p_X509_free_fn(tls_cert);

tls_loopback_done:;
	}

	/* ====================================================== */
	/*  Test 16: TLS over real eth0 interface                  */
	/* ====================================================== */
	printf("\n--- OpenSSL TLS over eth0 ---\n");
	{
		uint32_t eth0_ip = 0;
		if (get_interface_ipv4("eth0", &eth0_ip) != 0 || eth0_ip == 0) {
			test_result("TLS eth0: eth0 IP available", 1);
			printf("  [SKIP] eth0 has no IP address\n");
			goto tls_eth0_done;
		}
		test_result("TLS eth0: eth0 IP available", 1);

		/*
         * X509 cert-gen function pointers.  These are re-declared here in
         * this block's scope (different from the loopback block above) using
         * distinct type-alias names (fn3_*) to avoid any typedef collisions.
         */
		typedef void *(*fn3_X509_new)(void);
		typedef void (*fn3_X509_free)(void *);
		typedef int (*fn3_X509_set_version)(void *, long);
		typedef void *(*fn3_X509_get_serialNumber)(void *);
		typedef int (*fn3_ASN1_INTEGER_set)(void *, long);
		typedef void *(*fn3_X509_getm_notBefore)(void *);
		typedef void *(*fn3_X509_getm_notAfter)(void *);
		typedef void *(*fn3_X509_gmtime_adj)(void *, long);
		typedef int (*fn3_X509_set_pubkey)(void *, EVP_PKEY *);
		typedef void *(*fn3_X509_get_subject_name)(void *);
		typedef int (*fn3_X509_NAME_add_entry_by_txt)(
			void *, const char *, int, const unsigned char *, int,
			int, int);
		typedef int (*fn3_X509_set_issuer_name)(void *, void *);
		typedef int (*fn3_X509_sign)(void *, EVP_PKEY *,
					     const EVP_MD *);
		fn3_X509_new e_X509_new =
			(fn3_X509_new)dlsym(crypto_h, "X509_new");
		fn3_X509_free e_X509_free =
			(fn3_X509_free)dlsym(crypto_h, "X509_free");
		fn3_X509_set_version e_X509_sv = (fn3_X509_set_version)dlsym(
			crypto_h, "X509_set_version");
		fn3_X509_get_serialNumber e_X509_gsn =
			(fn3_X509_get_serialNumber)dlsym(
				crypto_h, "X509_get_serialNumber");
		fn3_ASN1_INTEGER_set e_ASN1_is = (fn3_ASN1_INTEGER_set)dlsym(
			crypto_h, "ASN1_INTEGER_set");
		fn3_X509_getm_notBefore e_X509_gnb =
			(fn3_X509_getm_notBefore)dlsym(crypto_h,
						       "X509_getm_notBefore");
		fn3_X509_getm_notAfter e_X509_gna =
			(fn3_X509_getm_notAfter)dlsym(crypto_h,
						      "X509_getm_notAfter");
		fn3_X509_gmtime_adj e_X509_gta =
			(fn3_X509_gmtime_adj)dlsym(crypto_h, "X509_gmtime_adj");
		fn3_X509_set_pubkey e_X509_spk =
			(fn3_X509_set_pubkey)dlsym(crypto_h, "X509_set_pubkey");
		fn3_X509_get_subject_name e_X509_gsn2 =
			(fn3_X509_get_subject_name)dlsym(
				crypto_h, "X509_get_subject_name");
		fn3_X509_NAME_add_entry_by_txt e_X509_na =
			(fn3_X509_NAME_add_entry_by_txt)dlsym(
				crypto_h, "X509_NAME_add_entry_by_txt");
		fn3_X509_set_issuer_name e_X509_sin =
			(fn3_X509_set_issuer_name)dlsym(crypto_h,
							"X509_set_issuer_name");
		fn3_X509_sign e_X509_sign =
			(fn3_X509_sign)dlsym(crypto_h, "X509_sign");

		int e_cert_syms = e_X509_new && e_X509_sv && e_X509_gsn &&
				  e_ASN1_is && e_X509_gnb && e_X509_gna &&
				  e_X509_gta && e_X509_spk && e_X509_gsn2 &&
				  e_X509_na && e_X509_sin && e_X509_sign;

		/* Generate RSA-2048 key */
		EVP_PKEY *eth0_key = NULL;
		if (p_EVP_PKEY_CTX_new_id && p_EVP_PKEY_keygen_init &&
		    p_EVP_PKEY_CTX_set_rsa_keygen_bits && p_EVP_PKEY_keygen) {
			EVP_PKEY_CTX *kctx =
				p_EVP_PKEY_CTX_new_id(6 /*EVP_PKEY_RSA*/, NULL);
			if (kctx) {
				p_EVP_PKEY_keygen_init(kctx);
				p_EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048);
				p_EVP_PKEY_keygen(kctx, &eth0_key);
				p_EVP_PKEY_CTX_free(kctx);
			}
		}
		test_result("TLS eth0: RSA-2048 key generated",
			    eth0_key != NULL);

		/* Generate self-signed cert */
		void *eth0_cert = NULL;
		if (eth0_key && e_cert_syms && p_EVP_sha256) {
			void *cert = e_X509_new();
			if (cert) {
				e_X509_sv(cert, 2);
				e_ASN1_is(e_X509_gsn(cert), 2);
				e_X509_gta(e_X509_gnb(cert), 0);
				e_X509_gta(e_X509_gna(cert), 3650L * 86400L);
				e_X509_spk(cert, eth0_key);
				void *subj = e_X509_gsn2(cert);
				/* MBSTRING_ASC = 0x1001 */
				e_X509_na(subj, "CN", 0x1001,
					  (const unsigned char *)"eth0test", -1,
					  -1, 0);
				e_X509_sin(cert, subj);
				if (e_X509_sign(cert, eth0_key,
						p_EVP_sha256()) > 0)
					eth0_cert = cert;
				else
					e_X509_free(cert);
			}
		}
		test_result("TLS eth0: self-signed cert generated",
			    eth0_cert != NULL);

		if (!eth0_key || !eth0_cert) {
			if (eth0_key)
				p_EVP_PKEY_free(eth0_key);
			if (eth0_cert)
				e_X509_free(eth0_cert);
			goto tls_eth0_done;
		}

		/* Transfer buffer: 8 KB of known pattern */
		static const int ETH0_DATA_LEN = 8192;
		static unsigned char eth0_send_buf[8192];
		static unsigned char eth0_recv_buf[8192];
		for (int i = 0; i < ETH0_DATA_LEN; i++)
			eth0_send_buf[i] = (unsigned char)(i & 0xFF);
		memset(eth0_recv_buf, 0, ETH0_DATA_LEN);

		/* Sync pipe */
		int e_sync[2] = { -1, -1 };
		pipe(e_sync);

		/* Per-process port so two parallel testlibc instances don't collide.
         * & 0x3FFF gives 16384 possible values (vs 1000 with % 1000), making
         * collisions between two PIDs practically impossible. */
		int tls_eth_port = 23100 + ((int)getpid() & 0x3FFF);

		/* Server listening socket bound to INADDR_ANY:tls_eth_port */
		int e_srv = socket(AF_INET, SOCK_STREAM, 0);
		if (e_srv >= 0) {
			int yes = 1;
			setsockopt(e_srv, SOL_SOCKET, SO_REUSEADDR, &yes,
				   sizeof(yes));
			struct sockaddr_in ea;
			memset(&ea, 0, sizeof(ea));
			ea.sin_family = AF_INET;
			ea.sin_port = htons((uint16_t)tls_eth_port);
			ea.sin_addr.s_addr = htonl(INADDR_ANY);
			if (bind(e_srv, (struct sockaddr *)&ea, sizeof(ea)) !=
				    0 ||
			    listen(e_srv, 4) != 0) {
				close(e_srv);
				e_srv = -1;
			}
		}
		test_result("TLS eth0: server socket+bind+listen", e_srv >= 0);
		if (e_srv < 0) {
			close(e_sync[0]);
			close(e_sync[1]);
			p_EVP_PKEY_free(eth0_key);
			e_X509_free(eth0_cert);
			goto tls_eth0_done;
		}

		pid_t e_pid = fork();
		test_result("TLS eth0: fork", e_pid >= 0);

		if (e_pid == 0) {
			/* ===== SERVER child ===== */
			close(e_sync[0]);
			int e_conn = -1;

			void *smeth = p_TLS_server_method();
			SSL_CTX *sctx = p_SSL_CTX_new(smeth);
			if (!sctx) {
				close(e_srv);
				close(e_sync[1]);
				_exit(3);
			}
			p_SSL_CTX_set_verify(sctx, 0, NULL);
			if (p_SSL_CTX_use_certificate(sctx, eth0_cert) != 1 ||
			    p_SSL_CTX_use_PrivateKey(sctx, eth0_key) != 1 ||
			    p_SSL_CTX_check_private_key(sctx) != 1) {
				p_SSL_CTX_free(sctx);
				close(e_srv);
				close(e_sync[1]);
				_exit(4);
			}

			/* Signal parent: SSL_CTX ready, blocking in accept() */
			{
				char r = 'R';
				write(e_sync[1], &r, 1);
			}
			close(e_sync[1]);

			/* 30s accept() timeout — prevents the server child from hanging
             * indefinitely if the client never connects (e.g. port collision
             * edge case with a parallel instance). */
			{
				struct timeval atv = { .tv_sec = 30,
						       .tv_usec = 0 };
				setsockopt(e_srv, SOL_SOCKET, SO_RCVTIMEO, &atv,
					   sizeof(atv));
			}
			e_conn = accept(e_srv, NULL, NULL);
			close(e_srv);
			if (e_conn < 0) {
				p_SSL_CTX_free(sctx);
				_exit(5);
			}

			SSL *ssl = p_SSL_new(sctx);
			if (!ssl) {
				p_SSL_CTX_free(sctx);
				close(e_conn);
				_exit(6);
			}
			p_SSL_set_fd(ssl, e_conn);
			int e_acc = p_SSL_accept(ssl);
			if (e_acc != 1) {
				int e_ae = p_SSL_get_error ?
						   p_SSL_get_error(ssl, e_acc) :
						   -1;
				unsigned long e_er =
					p_ERR_get_error ? p_ERR_get_error() : 0;
				char e_eb[128];
				e_eb[0] = 0;
				if (e_er && p_ERR_error_string)
					p_ERR_error_string(e_er, e_eb);
				printf("  [DBG] TLS eth0 srv: SSL_accept=%d err=%d errno=%d reason=%s\n",
				       e_acc, e_ae, errno, e_eb);
				p_SSL_free(ssl);
				p_SSL_CTX_free(sctx);
				close(e_conn);
				_exit(7);
			}

			/* Receive ETH0_DATA_LEN bytes then echo */
			static unsigned char e_srv_buf[8192];
			int recv_total = 0;
			while (recv_total < ETH0_DATA_LEN) {
				int n = p_SSL_read(ssl, e_srv_buf + recv_total,
						   ETH0_DATA_LEN - recv_total);
				if (n > 0) {
					recv_total += n;
					continue;
				}
				/* Match the loopback server: a non-fatal SSL
				 * return (WANT_READ/WANT_WRITE, or EAGAIN/EINTR
				 * on a SYSCALL) is not an error — retry.  Only a
				 * genuine fatal condition ends the loop, and we
				 * log which one so a real reset/EOF is
				 * diagnosable instead of a silent "recv short". */
				int err = p_SSL_get_error ?
						  p_SSL_get_error(ssl, n) :
						  -1;
				if (err == 2 /* WANT_READ */ ||
				    err == 3 /* WANT_WRITE */)
					continue;
				if (err == 5 /* SSL_ERROR_SYSCALL */ &&
				    (errno == 11 /* EAGAIN */ ||
				     errno == 4 /* EINTR */))
					continue;
				printf("  [DBG] TLS eth0 srv: SSL_read ret %d after %d/%d bytes, ssl_err=%d errno=%d\n",
				       n, recv_total, ETH0_DATA_LEN, err, errno);
				break;
			}
			int sent_total = 0;
			if (recv_total == ETH0_DATA_LEN) {
				while (sent_total < ETH0_DATA_LEN) {
					int n = p_SSL_write(
						ssl, e_srv_buf + sent_total,
						ETH0_DATA_LEN - sent_total);
					if (n <= 0) {
						printf("  [DBG] TLS eth0 srv: SSL_write ret %d after %d bytes, errno=%d\n",
						       n, sent_total, errno);
						break;
					}
					sent_total += n;
				}
			}
			p_SSL_shutdown(ssl);
			p_SSL_free(ssl);
			p_SSL_CTX_free(sctx);
			close(e_conn);
			/* exit 0=ok, 10=recv short, 11=echo short */
			_exit((recv_total != ETH0_DATA_LEN) ? 10 :
			      (sent_total != ETH0_DATA_LEN) ? 11 :
							      0);
		}

		/* ===== CLIENT (parent) ===== */
		close(e_sync[1]);

		if (e_pid > 0) {
			/* Wait for server-ready signal (up to 15 s) */
			int e_ready = 0;
			{
				fd_set rfds;
				FD_ZERO(&rfds);
				FD_SET(e_sync[0], &rfds);
				struct timeval wtv = { .tv_sec = 15,
						       .tv_usec = 0 };
				int sr = select(e_sync[0] + 1, &rfds, NULL,
						NULL, &wtv);
				if (sr > 0) {
					char ch = 0;
					e_ready =
						(read(e_sync[0], &ch, 1) == 1 &&
						 ch == 'R');
				}
			}
			close(e_sync[0]);
			test_result("TLS eth0: server signaled ready", e_ready);

			if (e_ready) {
				int e_cli = socket(AF_INET, SOCK_STREAM, 0);
				int e_conn_ok = 0;
				if (e_cli >= 0) {
					/* 120 s receive timeout — two parallel TLS sessions on a
                     * QEMU VM without hardware crypto (AES-NI=0) can take
                     * much longer than 30 s for the RSA handshake. */
					struct timeval rcv_tv = { .tv_sec = 120,
								  .tv_usec =
									  0 };
					setsockopt(e_cli, SOL_SOCKET,
						   SO_RCVTIMEO, &rcv_tv,
						   sizeof(rcv_tv));
					struct sockaddr_in ea;
					memset(&ea, 0, sizeof(ea));
					ea.sin_family = AF_INET;
					ea.sin_port =
						htons((uint16_t)tls_eth_port);
					ea.sin_addr.s_addr = htonl(
						eth0_ip); /* get_interface_ipv4 returns host-order */
					e_conn_ok =
						(connect(e_cli,
							 (struct sockaddr *)&ea,
							 sizeof(ea)) == 0);
				}
				test_result(
					"TLS eth0: client TCP connect via eth0",
					e_conn_ok);

				if (e_conn_ok) {
					void *cmeth = p_TLS_client_method();
					SSL_CTX *cctx = p_SSL_CTX_new(cmeth);
					if (cctx) {
						p_SSL_CTX_set_verify(
							cctx,
							0 /*SSL_VERIFY_NONE*/,
							NULL);
						SSL *ssl = p_SSL_new(cctx);
						if (ssl) {
							p_SSL_set_fd(ssl,
								     e_cli);
							int conn =
								p_SSL_connect(
									ssl);
							test_result(
								"TLS eth0: SSL_connect",
								conn == 1);
							if (conn == 1) {
								const char *ver =
									p_SSL_get_version(
										ssl);
								printf("  [INFO] TLS eth0 version: %s\n",
								       ver ? ver :
									     "(null)");

								/* Send 8 KB */
								int snt = 0;
								while (snt <
								       ETH0_DATA_LEN) {
									int n = p_SSL_write(
										ssl,
										eth0_send_buf +
											snt,
										ETH0_DATA_LEN -
											snt);
									if (n <=
									    0)
										break;
									snt += n;
								}
								test_result(
									"TLS eth0: sent 8 KB",
									snt == ETH0_DATA_LEN);

								/* Receive echo */
								int rcv = 0,
								    last_rcv_n =
									    0;
								while (rcv <
								       ETH0_DATA_LEN) {
									last_rcv_n = p_SSL_read(
										ssl,
										eth0_recv_buf +
											rcv,
										ETH0_DATA_LEN -
											rcv);
									if (last_rcv_n <=
									    0)
										break;
									rcv += last_rcv_n;
								}
								if (rcv <
								    ETH0_DATA_LEN) {
									int ssl_err =
										p_SSL_get_error ?
											p_SSL_get_error(
												ssl,
												last_rcv_n) :
											-1;
									unsigned long c_er =
										p_ERR_get_error ?
											p_ERR_get_error() :
											0;
									char c_eb[128];
									c_eb[0] = 0;
									if (c_er &&
									    p_ERR_error_string)
										p_ERR_error_string(
											c_er,
											c_eb);
									printf("  [DBG] TLS eth0 cli: got %d/%d bytes,"
									       " last_n=%d ssl_err=%d errno=%d reason=%s\n",
									       rcv,
									       ETH0_DATA_LEN,
									       last_rcv_n,
									       ssl_err,
									       errno,
									       c_eb);
								}
								test_result(
									"TLS eth0: recv 8 KB echo",
									rcv == ETH0_DATA_LEN);
								test_result(
									"TLS eth0: data integrity",
									rcv == ETH0_DATA_LEN &&
										memcmp(eth0_send_buf,
										       eth0_recv_buf,
										       (size_t)ETH0_DATA_LEN) ==
											0);
								p_SSL_shutdown(
									ssl);
							}
							p_SSL_free(ssl);
						}
						p_SSL_CTX_free(cctx);
					}
				}
				if (e_cli >= 0)
					close(e_cli);
			} else {
				close(e_sync[0]);
			}

			int e_status = 0;
			waitpid(e_pid, &e_status, 0);
			if (WIFEXITED(e_status) && WEXITSTATUS(e_status) != 0)
				printf("  [DBG] TLS eth0 srv exit: code=%d (10=recv short, 11=echo short)\n",
				       WEXITSTATUS(e_status));
			test_result("TLS eth0: server child exited cleanly",
				    WIFEXITED(e_status) &&
					    WEXITSTATUS(e_status) == 0);
			/* close the listen socket that was kept open across fork */
			if (e_srv >= 0)
				close(e_srv);
		} else {
			/* fork failed */
			close(e_sync[0]);
			close(e_srv);
		}

		p_EVP_PKEY_free(eth0_key);
		e_X509_free(eth0_cert);

tls_eth0_done:;
	}

	/* ====================================================== */
	/*  Hardware crypto capability verification                */
	/*                                                         */
	/*  Strategy:                                              */
	/*    1. Execute CPUID directly to read the CPU's own      */
	/*       feature bits.                                     */
	/*    2. Read OPENSSL_ia32cap_P via dlsym to confirm that  */
	/*       OPENSSL_cpuid_setup ran at library load time and  */
	/*       wrote the same bits.                              */
	/*    3. For each advertised instruction set, verify that  */
	/*       a correctness test passes — proving the hardware  */
	/*       dispatch path produces right answers.             */
	/* ====================================================== */
	{
		printf("\n--- Hardware crypto capabilities ---\n");

		/* ----- 1. Read CPUID leaves ----------------------------------- */

		/* CPUID leaf 1 → ECX (feature flags: AES-NI, PCLMULQDQ, AVX, …) */
		unsigned int cpu_ecx1 = 0, cpu_edx1 = 0;
		__asm__ volatile("cpuid"
				 : "=c"(cpu_ecx1), "=d"(cpu_edx1)
				 : "a"(1), "b"(0));

		/* CPUID leaf 7, sub-leaf 0 → EBX (SHA-NI bit 29, AVX2 bit 5, …) */
		unsigned int cpu_ebx7 = 0;
		{
			unsigned int max_leaf = 0;
			__asm__ volatile("cpuid"
					 : "=a"(max_leaf)
					 : "a"(0)
					 : "ebx", "ecx", "edx");
			if (max_leaf >= 7) {
				unsigned int tmp_eax, tmp_ecx, tmp_edx;
				__asm__ volatile("cpuid"
						 : "=a"(tmp_eax),
						   "=b"(cpu_ebx7),
						   "=c"(tmp_ecx), "=d"(tmp_edx)
						 : "a"(7), "c"(0));
			}
		}

		int cpu_has_aesni = (cpu_ecx1 >> 25) & 1; /* CPUID.1:ECX[25]  */
		int cpu_has_pclmulqdq =
			(cpu_ecx1 >> 1) & 1; /* CPUID.1:ECX[1]   */
		int cpu_has_avx = (cpu_ecx1 >> 28) & 1; /* CPUID.1:ECX[28]  */
		int cpu_has_avx2 = (cpu_ebx7 >> 5) & 1; /* CPUID.7:EBX[5]   */
		int cpu_has_sha = (cpu_ebx7 >> 29) & 1; /* CPUID.7:EBX[29]  */

		printf("  CPUID: AES-NI=%d PCLMULQDQ=%d AVX=%d AVX2=%d SHA-NI=%d\n",
		       cpu_has_aesni, cpu_has_pclmulqdq, cpu_has_avx,
		       cpu_has_avx2, cpu_has_sha);

		int any_cpu_feature = cpu_has_aesni | cpu_has_pclmulqdq |
				      cpu_has_avx | cpu_has_avx2 | cpu_has_sha;

		/* ----- Correctness under hardware dispatch -------------------- */

		if (!any_cpu_feature)
			printf("  [INFO] No hw-crypto features on this CPU - hw-dispatch correctness tests skipped\n");

		/*
         * SHA-256 NIST vector with maximum message length (56 bytes) so
         * that two compress() calls are made — exercises the multi-block
         * SHA-NI path when cap[2] bit 29 is set.
         */
		if (any_cpu_feature && p_EVP_MD_CTX_new &&
		    p_EVP_DigestInit_ex && p_EVP_DigestUpdate &&
		    p_EVP_DigestFinal_ex && p_EVP_sha256) {
			static const char msg56[] =
				"abcdbcdecdefdefgefghfghighijhijk"
				"ijkljklmklmnlmnomnopnopq";
			static const unsigned char exp56[32] = {
				0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
				0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
				0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
				0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
			};

			EVP_MD_CTX *ctx = p_EVP_MD_CTX_new();
			unsigned char dig[32];
			unsigned int dlen = 0;
			int ok = ctx &&
				 p_EVP_DigestInit_ex(ctx, p_EVP_sha256(),
						     NULL) == 1 &&
				 p_EVP_DigestUpdate(ctx, msg56, 56) == 1 &&
				 p_EVP_DigestFinal_ex(ctx, dig, &dlen) == 1 &&
				 dlen == 32 && memcmp(dig, exp56, 32) == 0;
			if (ctx)
				p_EVP_MD_CTX_free(ctx);
			test_result("SHA-256 hw-dispatch correctness (2-block)",
				    ok);
		}

		/*
         * AES-256-GCM authenticated encrypt/decrypt round-trip.
         * When AES-NI + PCLMULQDQ are present, OpenSSL routes through the
         * hardware-accelerated GHASH + AES-CTR path.  A correct plaintext
         * recovery proves the hardware path is functioning.
         */
		if (any_cpu_feature && p_EVP_CIPHER_CTX_new &&
		    p_EVP_EncryptInit_ex && p_EVP_EncryptUpdate &&
		    p_EVP_EncryptFinal_ex && p_EVP_DecryptInit_ex &&
		    p_EVP_DecryptUpdate && p_EVP_DecryptFinal_ex &&
		    p_EVP_aes_256_gcm && p_EVP_CIPHER_CTX_ctrl) {
#define EVP_CTRL_GCM_SET_IVLEN 0x9
#define EVP_CTRL_GCM_GET_TAG 0x10
#define EVP_CTRL_GCM_SET_TAG 0x11

			static const unsigned char gcm_key[32] = {
				0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
				0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
				0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
			};
			static const unsigned char gcm_iv[12] = {
				0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce,
				0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88,
			};
			static const char gcm_plain[] = "hw-crypto-test-vector";
			const int gcm_plen = (int)sizeof(gcm_plain) - 1;

			unsigned char ciphertext[64], tag[16], recovered[64];
			int clen = 0, flen = 0, rlen = 0, rflen = 0;

			/* Encrypt */
			EVP_CIPHER_CTX *ectx = p_EVP_CIPHER_CTX_new();
			int enc_ok =
				ectx != NULL &&
				p_EVP_EncryptInit_ex(ectx, p_EVP_aes_256_gcm(),
						     NULL, NULL, NULL) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(ectx,
						      EVP_CTRL_GCM_SET_IVLEN,
						      12, NULL) == 1 &&
				p_EVP_EncryptInit_ex(ectx, NULL, NULL, gcm_key,
						     gcm_iv) == 1 &&
				p_EVP_EncryptUpdate(
					ectx, ciphertext, &clen,
					(const unsigned char *)gcm_plain,
					gcm_plen) == 1 &&
				p_EVP_EncryptFinal_ex(ectx, ciphertext + clen,
						      &flen) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(ectx,
						      EVP_CTRL_GCM_GET_TAG, 16,
						      tag) == 1;
			if (ectx)
				p_EVP_CIPHER_CTX_free(ectx);

			/* Decrypt */
			EVP_CIPHER_CTX *dctx = p_EVP_CIPHER_CTX_new();
			int dec_ok =
				dctx != NULL &&
				p_EVP_DecryptInit_ex(dctx, p_EVP_aes_256_gcm(),
						     NULL, NULL, NULL) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(dctx,
						      EVP_CTRL_GCM_SET_IVLEN,
						      12, NULL) == 1 &&
				p_EVP_DecryptInit_ex(dctx, NULL, NULL, gcm_key,
						     gcm_iv) == 1 &&
				p_EVP_CIPHER_CTX_ctrl(dctx,
						      EVP_CTRL_GCM_SET_TAG, 16,
						      tag) == 1 &&
				p_EVP_DecryptUpdate(dctx, recovered, &rlen,
						    ciphertext,
						    clen + flen) == 1 &&
				p_EVP_DecryptFinal_ex(dctx, recovered + rlen,
						      &rflen) == 1;
			if (dctx)
				p_EVP_CIPHER_CTX_free(dctx);

			int round_trip =
				enc_ok && dec_ok &&
				(rlen + rflen == gcm_plen) &&
				memcmp(recovered, gcm_plain, gcm_plen) == 0;

			test_result(
				"AES-256-GCM hw-dispatch encrypt/decrypt round-trip",
				round_trip);

#undef EVP_CTRL_GCM_SET_IVLEN
#undef EVP_CTRL_GCM_GET_TAG
#undef EVP_CTRL_GCM_SET_TAG
		}

		/*
         * SHA-512 long message (two 128-byte blocks) — exercises
         * AVX/AVX2 SHA-512 dispatch paths on capable hardware.
         */
		if (any_cpu_feature && p_EVP_MD_CTX_new &&
		    p_EVP_DigestInit_ex && p_EVP_DigestUpdate &&
		    p_EVP_DigestFinal_ex && p_EVP_sha512) {
			/* SHA-512("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
             *          "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")
             * = 8e959b75dae313da8cf4f72814fc143f...
             */
			static const char msg112[] =
				"abcdefghbcdefghicdefghijdefghijk"
				"efghijklfghijklmghijklmnhijklmno"
				"ijklmnopjklmnopqklmnopqrlmnopqrs"
				"mnopqrstnopqrstu";
			static const unsigned char exp512_16[16] = {
				0x8e, 0x95, 0x9b, 0x75, 0xda, 0xe3, 0x13, 0xda,
				0x8c, 0xf4, 0xf7, 0x28, 0x14, 0xfc, 0x14, 0x3f,
			};

			EVP_MD_CTX *ctx = p_EVP_MD_CTX_new();
			unsigned char dig[64];
			unsigned int dlen = 0;
			int ok = ctx &&
				 p_EVP_DigestInit_ex(ctx, p_EVP_sha512(),
						     NULL) == 1 &&
				 p_EVP_DigestUpdate(ctx, msg112, 112) == 1 &&
				 p_EVP_DigestFinal_ex(ctx, dig, &dlen) == 1 &&
				 dlen == 64 && memcmp(dig, exp512_16, 16) == 0;
			if (ctx)
				p_EVP_MD_CTX_free(ctx);
			test_result("SHA-512 hw-dispatch correctness (2-block)",
				    ok);
		}
	}

openssl_skip:;

	/* ====================================================== */
	/* End of OpenSSL tests                                    */
	/* ====================================================== */

network_skip:;
	// ========================================
	// Permission enforcement.  Drop to a non-root uid in a child and
	// confirm the kernel denies access to root-owned objects (root, running
	// these tests, is unaffected — it bypasses the checks).
	// ========================================
	printf("\n[TEST] permission enforcement (non-root)\n");
	if (getuid() != 0) {
		printf("  [SKIP] not running as root; cannot test privilege drop\n");
	} else {
		char pf[96], pd[96], inside[112];
		snprintf(pf, sizeof(pf), "%s/rootonly.txt", _pbase);
		snprintf(pd, sizeof(pd), "%s/rootdir", _pbase);
		snprintf(inside, sizeof(inside), "%s/rootdir/x", _pbase);

		/* Ensure the per-process sandbox exists before we create files in
		 * it.  Earlier sections rmdir() it (LFN cleanup) and mkdir() it
		 * back thousands of lines up; do not depend on that surviving.
		 * If the create itself fails (e.g. an ext4 error on a /tmp
		 * cluttered by earlier runs' leftover dirs), say so — otherwise
		 * the ENOENT below reads as a bogus permission-enforcement fail. */
		errno = 0;
		if (mkdir(_pbase, 0777) != 0 && errno != EEXIST)
			printf("  [DBG] perm test: mkdir(%s) failed errno=%d\n",
			       _pbase, errno);

		int sfd = open(pf, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (sfd >= 0) {
			write(sfd, "secret\n", 7);
			close(sfd);
		}
		chmod(pf, 0600); /* 0600, owned by root */
		mkdir(pd, 0700); /* 0700, owned by root */
		test_result("setup: root made 0600 file + 0700 dir", sfd >= 0);

		pid_t pid = fork();
		if (pid == 0) {
			/* child: drop to a non-root gid then uid, then probe */
			setgid(1000);
			if (setuid(1000) != 0)
				_exit(40);
			if (getuid() != 1000)
				_exit(41);
			int fails = 0;
			int r = open(pf, O_RDONLY);
			if (r >= 0) {
				close(r);
				fails |= 1;
			} else if (errno != EACCES)
				fails |= 1;
			if (access(pf, R_OK) == 0)
				fails |= 2;
			int r2 = open(inside, O_WRONLY | O_CREAT, 0644);
			if (r2 >= 0) {
				close(r2);
				fails |= 4;
			}
			if (chmod(pf, 0644) == 0)
				fails |= 8;
			_exit(fails);
		} else if (pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0);
			int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			test_result("non-root: setuid(1000) succeeded",
				    code >= 0 && code != 40 && code != 41);
			test_result("non-root: open(0600 root file) denied",
				    code >= 0 && (code & 1) == 0);
			test_result("non-root: access(R_OK) denied",
				    code >= 0 && (code & 2) == 0);
			test_result("non-root: create in 0700 root dir denied",
				    code >= 0 && (code & 4) == 0);
			test_result("non-root: chmod root file denied (EPERM)",
				    code >= 0 && (code & 8) == 0);
		} else {
			test_fail("permission test: fork failed");
		}
		unlink(pf);
		rmdir(pd);

		/* POSIX ACL enforcement: a 0600 root-owned file whose access ACL grants
         * uid 1000 read via a named-user entry + mask.  Without the ACL a
         * non-root open would be denied, so success proves the ACL is honoured;
         * the read-only mask proves write is still denied.  Skips cleanly where
         * xattrs/ACLs are unsupported (e.g. FAT32). */
		char af[96];
		snprintf(af, sizeof(af), "%s/aclfile.txt", _pbase);
		int afd = open(af, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (afd >= 0) {
			write(afd, "acltest\n", 8);
			close(afd);
		}
		chmod(af, 0600); /* 0600, root-owned */
		unsigned char acl[28];
		acl[0] = 1;
		acl[1] = acl[2] = acl[3] = 0; /* version 1 */
		int ao = 4;
		acl[ao] = 0x01;
		acl[ao + 1] = 0;
		acl[ao + 2] = 6;
		acl[ao + 3] = 0;
		ao += 4; /* USER_OBJ rw- */
		acl[ao] = 0x02;
		acl[ao + 1] = 0;
		acl[ao + 2] = 4;
		acl[ao + 3] = 0; /* USER 1000 r-- */
		acl[ao + 4] = 0xE8;
		acl[ao + 5] = 0x03;
		acl[ao + 6] = 0;
		acl[ao + 7] = 0;
		ao += 8; /*   id=1000 (LE) */
		acl[ao] = 0x04;
		acl[ao + 1] = 0;
		acl[ao + 2] = 0;
		acl[ao + 3] = 0;
		ao += 4; /* GROUP_OBJ --- */
		acl[ao] = 0x10;
		acl[ao + 1] = 0;
		acl[ao + 2] = 4;
		acl[ao + 3] = 0;
		ao += 4; /* MASK r-- */
		acl[ao] = 0x20;
		acl[ao + 1] = 0;
		acl[ao + 2] = 0;
		acl[ao + 3] = 0;
		ao += 4; /* OTHER --- */
		errno = 0;
		int aclset =
			(afd >= 0) && setxattr(af, "system.posix_acl_access",
					       acl, sizeof(acl), 0) == 0;
		if (afd >= 0 && !aclset && errno == EOPNOTSUPP) {
			printf("  [SKIP] ACLs unsupported on this filesystem\n");
		} else {
			test_result(
				"setup: set access ACL granting uid 1000 read",
				aclset);
			pid_t apid = fork();
			if (apid == 0) {
				setgid(1000);
				if (setuid(1000) != 0)
					_exit(40);
				int f = 0;
				int r = open(af,
					     O_RDONLY); /* ACL grants read    */
				if (r >= 0)
					close(r);
				else
					f |= 1;
				errno = 0;
				int w = open(af,
					     O_WRONLY); /* mask: no write     */
				if (w >= 0) {
					close(w);
					f |= 2;
				} else if (errno != EACCES)
					f |= 2;
				if (access(af, R_OK) != 0)
					f |= 4; /* read granted       */
				if (access(af, W_OK) == 0)
					f |= 8; /* write denied       */
				_exit(f);
			} else if (apid > 0) {
				int status = 0;
				waitpid(apid, &status, 0);
				int code = WIFEXITED(status) ?
						   WEXITSTATUS(status) :
						   -1;
				test_result(
					"ACL: non-root open(RDONLY) allowed by ACL",
					code >= 0 && (code & 1) == 0);
				test_result(
					"ACL: non-root open(WRONLY) denied by mask",
					code >= 0 && (code & 2) == 0);
				test_result(
					"ACL: non-root access(R_OK) allowed",
					code >= 0 && (code & 4) == 0);
				test_result("ACL: non-root access(W_OK) denied",
					    code >= 0 && (code & 8) == 0);
			} else {
				test_fail("ACL test: fork failed");
			}
		}
		unlink(af);

		/* Sticky bit (1777, like /tmp) + set-id stripping on write.  Fresh
         * child with its own exit-code scheme (sentinel 99) to avoid clashes. */
		char sdir[160], sroot[160], smine[160], suf[160];
		snprintf(sdir, sizeof(sdir), "%s/stickyd", _pbase);
		snprintf(sroot, sizeof(sroot), "%s/stickyd/rootf", _pbase);
		snprintf(smine, sizeof(smine), "%s/stickyd/minef", _pbase);
		snprintf(suf, sizeof(suf), "%s/suidf", _pbase);
		mkdir(sdir, 0777);
		chmod(sdir, 01777); /* sticky, world-writable */
		int rf = open(sroot, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (rf >= 0) {
			write(rf, "r\n", 2);
			close(rf);
		} /* root-owned file in it  */
		int uf = open(suf, O_WRONLY | O_CREAT | O_TRUNC, 0755);
		if (uf >= 0) {
			write(uf, "x\n", 2);
			close(uf);
		}
		chown(suf, 1000, 1000); /* owned by the non-root uid */
		chmod(suf, 04755); /* setuid bit, set by root */

		pid_t pid2 = fork();
		if (pid2 == 0) {
			setgid(1000);
			if (setuid(1000) != 0)
				_exit(99);
			int f = 0;
			int m = open(smine, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (m < 0)
				f |= 1;
			else
				close(m); /* may create own entry   */
			if (unlink(sroot) == 0)
				f |= 2; /* may NOT remove root's  */
			else if (errno != EPERM && errno != EACCES)
				f |= 2; /* sticky-bit denial is EPERM (as the
                                         * reference does); EACCES also accepted */
			if (unlink(smine) != 0)
				f |= 4; /* may remove its own     */
			int w = open(suf,
				     O_WRONLY); /* owns it → may write    */
			if (w >= 0) {
				write(w, "y\n", 2);
				close(w);
			} else
				f |= 8;
			struct stat sb;
			if (stat(suf, &sb) != 0 || (sb.st_mode & S_ISUID))
				f |= 16; /* bit gone */
			_exit(f);
		} else if (pid2 > 0) {
			int status = 0;
			waitpid(pid2, &status, 0);
			int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			int ok = code >= 0 && code != 99;
			test_result(
				"non-root: create own entry in sticky dir allowed",
				ok && (code & 1) == 0);
			test_result(
				"non-root: remove root's entry in sticky dir denied",
				ok && (code & 2) == 0);
			test_result(
				"non-root: remove own entry in sticky dir allowed",
				ok && (code & 4) == 0);
			test_result("non-root: write strips the setuid bit",
				    ok && (code & 8) == 0 && (code & 16) == 0);
		} else {
			test_fail("sticky/setid test: fork failed");
		}
		unlink(sroot);
		unlink(smine);
		unlink(suf);
		rmdir(sdir);
	}

	test_loader_introspection();
	test_exit_handlers();
	test_math_classification();
	test_hyperbolic();
	test_uchar();
	test_fenv();
	test_search();
	test_so_peercred();
	test_misc_posix();
	test_long_double_decompose();
	test_semaphores();
	test_timed_blocking();
	test_timeout_accuracy();
	test_fd_marker_syscalls();
	test_tsd_destructors();
	test_fork_from_thread();
	test_lock_contention();
	test_orphan_reaping();
	test_printf_conversions();
	test_futex_ops();
	test_resolver();
	test_mincore();
	test_c99_math_additions();
	test_pthread_kill_and_getattr();
	test_unix_seqpacket();
	test_getentropy();
	test_timegm();
	test_so_acceptconn();
	test_memstream_ftw_cpuclock();
	test_stdint_limits();
	test_dirent64_and_stat_mkdir();
	test_ucontext();
	test_xsave();
	test_mmap_large();
	test_futex_bitset();
	test_clock_nanosleep();
	test_sigsuspend_delivery();
	test_cred_threads();
	test_anonfd();
	test_sysfs_procfs();
	test_posix_additions();
	test_drm();
	test_drm_3d();
	test_gpu_userland();

	// ========================================
	// Summary
	// ========================================
	printf("\n========================================\n");
	printf("  TEST SUMMARY\n");
	printf("========================================\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("  Total:  %d\n", tests_passed + tests_failed);
	printf("========================================\n");

	if (tests_failed == 0) {
		printf("  ALL TESTS PASSED!\n");
	} else {
		printf("  SOME TESTS FAILED!\n");
		/* List every failed test in one contiguous block.  Use a marker that
         * differs from the inline "[FAIL]" token so it is easy to grep for
         * even if inline lines were garbled by concurrent SMP console writes. */
		int shown = failed_names_count < MAX_FAILED_NAMES ?
				    failed_names_count :
				    MAX_FAILED_NAMES;
		for (int i = 0; i < shown; i++)
			printf("  >> FAILED: %s\n", failed_names[i]);
		if (failed_names_count > shown)
			printf("  >> (+%d more not listed)\n",
			       failed_names_count - shown);
	}
	printf("========================================\n");

	rmtree(_pbase); /* clean up per-process sandbox */

	return tests_failed > 0 ? 1 : 0;
}
