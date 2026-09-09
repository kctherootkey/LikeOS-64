/*
 * apload -- load apnews.com the way luakit does, without luakit, and say
 * where the time goes.
 *
 * WHY
 *   luakit takes far longer than other systems' browsers to finish loading
 *   apnews.com, and sometimes stalls outright.  A browser is a poor
 *   instrument for that: it runs JavaScript, lays out, paints and forks,
 *   and none of that is the network stack.  This program keeps only the
 *   part of luakit that talks to the network, reproduces it faithfully,
 *   and measures every phase of every request.  The same run on another
 *   system gives a baseline; a stall here is a stall in the stack (or in
 *   the libraries the browser drives it through), not in the browser.
 *
 * WHAT IT REPRODUCES, AND WHERE EACH NUMBER COMES FROM
 *   luakit is a UI process, one WebProcess per tab and ONE network process.
 *   Every byte of HTTP goes through the network process, on ONE thread,
 *   through libsoup, in a GLib main loop.  The WebProcess asks for loads
 *   and receives the bytes over a SOCK_SEQPACKET socketpair.  So:
 *
 *   process "web" (this binary as started)            = WebKitWebProcess
 *     reads the URL list, issues loads in file order (the browser's
 *     discovery order) keeping -w of them outstanding, receives
 *     RESPONSE / DATA / FINISH over IPC, copies and touches every byte the
 *     way WebResourceLoader::didReceiveData does, prints the end-to-end
 *     report.
 *
 *   process "net" (this binary, --network-process)    = WebKitNetworkProcess
 *     SoupSession as SoupNetworkSession.cpp builds it: max-conns 256,
 *       max-conns-per-host 6, idle-timeout 115 s, timeout 0,
 *       SoupContentSniffer, SoupHSTSEnforcer, SoupWebsocketExtensionManager,
 *       an in-memory SoupCookieJar with the no-third-party policy
 *       (NetworkStorageSessionSoup.cpp), Accept-Language from the locale.
 *     Each request as NetworkDataTaskSoup.cpp drives it:
 *       SOUP_MESSAGE_NO_REDIRECT | SOUP_MESSAGE_COLLECT_METRICS, an Accept
 *       header, the message priority WebKit gives that resource type,
 *       soup_session_send_async() at priority 100
 *       (RunLoopSourcePriority::AsyncIONetwork), then
 *       g_input_stream_read_async() in 8192-byte reads
 *       (gDefaultReadBufferSize), ONE read outstanding per request;
 *       redirect bodies drained with g_input_stream_skip_async() and the
 *       redirect re-issued as a fresh SoupMessage, at most 20 deep.
 *     Delivery as NetworkResourceLoader.cpp does it
 *       (WebLoaderStrategy::maximumBufferingTime): scripts, stylesheets and
 *       fonts are held whole and delivered when complete; images are
 *       flushed every 500 ms; the document and everything else go out at
 *       every read.
 *
 *   IPC as Platform/IPC/unix/ConnectionUnix.cpp does it:
 *     a SOCK_SEQPACKET socketpair; a dedicated IPC thread per process with
 *     its own GMainContext does every sendmsg()/recvmsg(); a message whose
 *     body would exceed 4096 bytes carries it out of line in a memfd
 *     (SharedMemoryUnix.cpp), the descriptor passed with SCM_RIGHTS,
 *     mapped and copied by the receiver; received messages are handed to
 *     the main loop with g_main_context_invoke() (RunLoop::dispatch).  A
 *     send that hits EAGAIN waits in poll(POLLOUT) on the IPC thread and
 *     nothing throttles the main thread behind it -- WebKit has no flow
 *     control there either.
 *
 *   Threads: nothing on WebKit's network path scales with the CPU count,
 *     so neither does this.  Per process: the main loop and the IPC thread;
 *     GLib adds its name-resolver pool (up to 20 threads,
 *     gthreadedresolver.c) and the GTask pool that glib-networking runs TLS
 *     handshakes in (10 threads, growing when they block).  This program
 *     links the same libsoup, GIO and glib-networking as the browser, so it
 *     gets exactly those threads with no emulation.
 *
 *   NOT here: JavaScript, layout, painting, the disk cache, the UI
 *   process.  The URL list is the order a browser discovered them in, so
 *   "-w" (loads the web side keeps outstanding) stands in for the
 *   parser/script cascade that spaced them out.  "-w 0" issues everything
 *   at once and lets libsoup's 256/6 limits do the queueing.
 *
 * READING THE OUTPUT
 *   Live: one timeline line per second (done, in flight, bytes, rate, new
 *   connections), and STALL lines from both sides whenever nothing has
 *   progressed for -s seconds.  A net-side STALL lists every in-flight
 *   request with the phase it is stuck in, read from libsoup's live
 *   metrics: queued (no connection slot), DNS, TCP connect, TLS handshake,
 *   waiting for headers, reading body.  That phase names the subsystem.
 *   A web-side STALL with no matching net-side STALL means the network
 *   process main loop itself is not running (blocked in a syscall, not
 *   woken) -- the case a second process exists to catch.
 *
 *   At the end: the net report (per-phase percentiles, connection reuse,
 *   HTTP/2 share, per-class and per-host tables, the slowest loads, the
 *   failures) and the web report (wall time, document TTFB, end-to-end
 *   latency percentiles, IPC volume and delivery latency, and the completion
 *   curve: when 50/90/95/99/100 % of the loads had finished, with the last
 *   five to finish).  Compare systems on the 99 % point and the STALL lines;
 *   the 100 % point is usually one ad-network host that never answers its
 *   SYN, sitting out the connect timeout because WebKit sets none.
 *
 * OPTIONS
 *   -f FILE  URL list (default: apnews-urls.txt beside the binary, else
 *            /usr/local/bin/apnews-urls.txt); one URL per line, # comments;
 *            http(s) and ws(s) are loaded, other schemes counted and skipped
 *   -w N     loads kept outstanding on the web side (40; 0 = all at once)
 *   -n N     only the first N URLs
 *   -s SEC   call it a stall after SEC without progress (2)
 *   -T SEC   give up after SEC and report what was in flight (300)
 *   -1       single process, no IPC (bisect the IPC layer out)
 *   -k       accept any TLS certificate
 *   -v       one line per finished load
 *   -q       no per-second timeline
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <libsoup/soup.h>
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif

/* ---- numbers copied from WebKit, each with its source file ------------- */
#define WK_MAX_CONNS            256   /* SoupNetworkSession.cpp                 */
#define WK_MAX_CONNS_PER_HOST   6     /* SoupNetworkSession.cpp                 */
#define WK_IDLE_TIMEOUT_S       115   /* SoupNetworkSession.cpp                 */
#define WK_READ_SIZE            8192  /* NetworkDataTaskSoup.cpp                */
#define WK_IO_PRIORITY          100   /* RunLoopSourcePriority::AsyncIONetwork  */
#define WK_DISPATCH_PRIORITY    100   /* RunLoopSourcePriority::RunLoopDispatcher */
#define WK_MAX_REDIRECTS        20    /* NetworkDataTaskSoup.cpp                */
#define WK_IMAGE_BUFFER_MS      500   /* WebLoaderStrategy.cpp                  */
#define IPC_MESSAGE_MAX         4096  /* ConnectionUnix.cpp messageMaxSize      */
#define IPC_ATTACHMENT_MAX      254   /* ConnectionUnix.cpp attachmentMaxAmount */
#define WK_URI_FLAGS (SOUP_HTTP_URI_FLAGS | G_URI_FLAGS_PARSE_RELAXED) /* URLGLib.cpp */

#define URL_FILE_NAME     "apnews-urls.txt"
#define DEFAULT_URL_FILE  "/usr/local/bin/" URL_FILE_NAME

static struct {
	int window;
	int limit;
	double stall_s;
	double timeout_s;
	int timeline;
	int verbose;
	int single;
	int insecure;
	const char *file;
} opt = { .window = 40, .stall_s = 2.0, .timeout_s = 300.0, .timeline = 1 };

static double g_t0;

static double now_s(void)
{
	return (double)g_get_monotonic_time() / 1e6;
}
#define REL(t) ((t) - g_t0)

static const char *fmt_bytes(guint64 b, char *buf, size_t sz)
{
	if (b >= 1000000000ULL)
		snprintf(buf, sz, "%.2f GB", b / 1e9);
	else if (b >= 1000000ULL)
		snprintf(buf, sz, "%.2f MB", b / 1e6);
	else if (b >= 1000ULL)
		snprintf(buf, sz, "%.1f KB", b / 1e3);
	else
		snprintf(buf, sz, "%llu B", (unsigned long long)b);
	return buf;
}

static const char *short_url(const char *url, char *buf, size_t sz)
{
	size_t n = strlen(url);
	if (n < sz)
		return url;
	memcpy(buf, url, sz - 4);
	strcpy(buf + sz - 4, "...");
	return buf;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void pct_header(const char *what)
{
	printf("  %-22s %6s %9s %9s %9s %9s %9s\n", what, "n", "avg", "p50",
	       "p90", "p99", "max");
}

static void pct_row(const char *name, GArray *a)
{
	if (!a->len) {
		printf("  %-22s %6u\n", name, 0);
		return;
	}
	double *v = (double *)a->data;
	qsort(v, a->len, sizeof(double), cmp_double);
	double sum = 0;
	for (guint i = 0; i < a->len; i++)
		sum += v[i];
	guint last = a->len - 1;
	printf("  %-22s %6u %9.1f %9.1f %9.1f %9.1f %9.1f\n", name, a->len,
	       sum / a->len, v[last * 50 / 100], v[last * 90 / 100],
	       v[last * 99 / 100], v[last]);
}

/* ---- resource classes: what WebKit would make of each URL --------------- */
/* Priority: CachedResourceLoader::defaultPriorityForResourceType.  Accept:
 * CachedResourceRequest.  Buffering: WebLoaderStrategy::maximumBufferingTime
 * (-1 = hold the whole body, 0 = deliver every read).  The class is guessed
 * from the URL; the browser knows it from the tag that referenced it. */
enum rclass { RC_DOCUMENT, RC_SCRIPT, RC_STYLE, RC_FONT, RC_IMAGE, RC_OTHER,
	      RC_WEBSOCKET, RC_COUNT };

static const struct {
	const char *name;
	SoupMessagePriority prio;
	const char *accept;
	int buffer_ms;
} rc_info[RC_COUNT] = {
	[RC_DOCUMENT]  = { "document", SOUP_MESSAGE_PRIORITY_VERY_HIGH,
			   "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", 0 },
	[RC_SCRIPT]    = { "script", SOUP_MESSAGE_PRIORITY_HIGH, "*/*", -1 },
	[RC_STYLE]     = { "style", SOUP_MESSAGE_PRIORITY_HIGH, "text/css,*/*;q=0.1", -1 },
	[RC_FONT]      = { "font", SOUP_MESSAGE_PRIORITY_NORMAL, "*/*", -1 },
	[RC_IMAGE]     = { "image", SOUP_MESSAGE_PRIORITY_LOW,
			   "image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5",
			   WK_IMAGE_BUFFER_MS },
	[RC_OTHER]     = { "other", SOUP_MESSAGE_PRIORITY_NORMAL, "*/*", 0 },
	[RC_WEBSOCKET] = { "websocket", SOUP_MESSAGE_PRIORITY_NORMAL, "*/*", 0 },
};

static enum rclass classify(const char *url, int index)
{
	if (g_str_has_prefix(url, "wss://") || g_str_has_prefix(url, "ws://"))
		return RC_WEBSOCKET;
	if (index == 0)
		return RC_DOCUMENT;
	if (strstr(url, "/format/webp/") || strstr(url, "/format/jpeg/") ||
	    strstr(url, "/format/png/"))
		return RC_IMAGE;
	const char *end = strpbrk(url, "?#");
	size_t plen = end ? (size_t)(end - url) : strlen(url);
	const char *slash = NULL, *dot = NULL;
	for (size_t i = 0; i < plen; i++) {
		if (url[i] == '/')
			slash = url + i;
		else if (url[i] == '.')
			dot = url + i;
	}
	if (!dot || (slash && dot < slash))
		return RC_OTHER;
	char ext[16];
	size_t elen = plen - (size_t)(dot + 1 - url);
	if (elen == 0 || elen >= sizeof ext)
		return RC_OTHER;
	for (size_t i = 0; i < elen; i++)
		ext[i] = g_ascii_tolower(dot[1 + i]);
	ext[elen] = 0;
	if (!strcmp(ext, "js") || !strcmp(ext, "mjs"))
		return RC_SCRIPT;
	if (!strcmp(ext, "css"))
		return RC_STYLE;
	if (!strcmp(ext, "woff") || !strcmp(ext, "woff2") || !strcmp(ext, "ttf") ||
	    !strcmp(ext, "otf"))
		return RC_FONT;
	if (!strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg") ||
	    !strcmp(ext, "gif") || !strcmp(ext, "webp") || !strcmp(ext, "svg") ||
	    !strcmp(ext, "ico") || !strcmp(ext, "avif"))
		return RC_IMAGE;
	if (!strcmp(ext, "html") || !strcmp(ext, "htm"))
		return RC_DOCUMENT;
	return RC_OTHER;
}

/* ======================================================================= */
/* IPC: ConnectionUnix.cpp in miniature                                     */
/* ======================================================================= */

enum { MSG_HANGUP = 0, MSG_LOAD, MSG_RESPONSE, MSG_DATA, MSG_FINISH,
       MSG_SHUTDOWN, MSG_BYE };
#define IPC_F_OUT_OF_LINE 1u

struct ipc_hdr {
	uint32_t type, id, body_len, flags;
	uint64_t a[4];
};

struct link;

struct ipc_msg {
	struct ipc_hdr h;
	void *body;      /* inline: heap copy; out of line: the mapping        */
	size_t len;
	int fd;          /* out-of-line descriptor, -1 otherwise               */
	struct link *target;
};

typedef void (*msg_handler)(struct ipc_msg *m, void *user);

struct ipc {
	int fd;
	GMainContext *ctx;
	GMainLoop *loop;
	GThread *thread;
	GMutex lock;
	GQueue out;
	gboolean flush_pending;
	struct link *link;
	guint64 sent, sent_ool, sent_bytes, pollout_waits;
	guint64 recv, recv_ool, recv_bytes;
	gboolean closed;
};

/* One side of the web<->net conversation.  With IPC the message crosses the
 * socketpair; without (-1) it is dispatched to the peer through the same
 * main loop, which is what RunLoop::dispatch would do in-process. */
struct link {
	struct ipc *ipc;
	struct link *local_peer;
	msg_handler on_receive;
	void *user;
	guint64 shm_segments;
};

static struct ipc_msg *msg_new(uint32_t type, uint32_t id, const uint64_t a[4],
			       const void *body, size_t len)
{
	struct ipc_msg *m = g_new0(struct ipc_msg, 1);
	m->h.type = type;
	m->h.id = id;
	m->h.body_len = (uint32_t)len;
	if (a)
		memcpy(m->h.a, a, sizeof m->h.a);
	m->fd = -1;
	if (len) {
		m->body = g_memdup2(body, len);
		m->len = len;
	}
	return m;
}

static void msg_free(struct ipc_msg *m)
{
	if (m->fd >= 0) {
		if (m->body)
			munmap(m->body, m->len);
		close(m->fd);
	} else {
		g_free(m->body);
	}
	g_free(m);
}

static gboolean deliver_cb(gpointer data)
{
	struct ipc_msg *m = data;
	m->target->on_receive(m, m->target->user);
	msg_free(m);
	return G_SOURCE_REMOVE;
}

/* SharedMemory::allocate(): memfd first, a named-and-unlinked shm object if
 * the system has no memfd. */
static int shm_alloc(size_t size)
{
	static unsigned long counter;
	int fd = memfd_create("apload-ipc", MFD_CLOEXEC);
	if (fd < 0) {
		char name[64];
		snprintf(name, sizeof name, "/apload-%d-%lu", (int)getpid(),
			 ++counter);
		fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
		if (fd >= 0)
			shm_unlink(name);
	}
	if (fd < 0)
		return -1;
	while (ftruncate(fd, (off_t)size) == -1) {
		if (errno == EINTR)
			continue;
		close(fd);
		return -1;
	}
	return fd;
}

static void ipc_write(struct ipc *c, struct ipc_msg *m)
{
	struct iovec iov[2];
	int niov = 1;
	iov[0].iov_base = &m->h;
	iov[0].iov_len = sizeof m->h;
	if (m->fd < 0 && m->len) {
		iov[1].iov_base = m->body;
		iov[1].iov_len = m->len;
		niov = 2;
	}
	struct msghdr mh;
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = iov;
	mh.msg_iovlen = niov;
	char cbuf[CMSG_SPACE(sizeof(int))];
	if (m->fd >= 0) {
		memset(cbuf, 0, sizeof cbuf);
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof cbuf;
		struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &m->fd, sizeof(int));
	}
	while (sendmsg(c->fd, &mh, MSG_NOSIGNAL) == -1) {
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct pollfd p = { .fd = c->fd, .events = POLLOUT };
			c->pollout_waits++;
			poll(&p, 1, -1);
			continue;
		}
		if (errno != EPIPE && errno != ECONNRESET)
			fprintf(stderr, "apload: sendmsg: %s\n", strerror(errno));
		c->closed = TRUE;
		return;
	}
	c->sent++;
	c->sent_bytes += m->len;
	if (m->fd >= 0)
		c->sent_ool++;
}

static gboolean ipc_flush_cb(gpointer data)
{
	struct ipc *c = data;
	for (;;) {
		g_mutex_lock(&c->lock);
		struct ipc_msg *m = g_queue_pop_head(&c->out);
		if (!m) {
			c->flush_pending = FALSE;
			g_mutex_unlock(&c->lock);
			break;
		}
		g_mutex_unlock(&c->lock);
		if (!c->closed)
			ipc_write(c, m);
		msg_free(m);
	}
	return G_SOURCE_REMOVE;
}

static void ipc_send(struct ipc *c, struct ipc_msg *m)
{
	if (sizeof m->h + m->len > IPC_MESSAGE_MAX) {
		int fd = shm_alloc(m->len);
		if (fd < 0) {
			fprintf(stderr, "apload: shared memory for a %zu-byte "
				"message: %s\n", m->len, strerror(errno));
			exit(1);
		}
		void *map = mmap(NULL, m->len, PROT_READ | PROT_WRITE, MAP_SHARED,
				 fd, 0);
		if (map == MAP_FAILED) {
			fprintf(stderr, "apload: mmap %zu bytes: %s\n", m->len,
				strerror(errno));
			exit(1);
		}
		memcpy(map, m->body, m->len);
		munmap(map, m->len);
		g_free(m->body);
		m->body = NULL;
		m->fd = fd;
		m->h.flags |= IPC_F_OUT_OF_LINE;
		c->link->shm_segments++;
	}
	g_mutex_lock(&c->lock);
	g_queue_push_tail(&c->out, m);
	gboolean kick = !c->flush_pending;
	c->flush_pending = TRUE;
	g_mutex_unlock(&c->lock);
	if (kick)
		g_main_context_invoke_full(c->ctx, G_PRIORITY_DEFAULT,
					   ipc_flush_cb, c, NULL);
}

static void ipc_hangup(struct ipc *c)
{
	if (c->closed)
		return;
	c->closed = TRUE;
	struct ipc_msg *m = msg_new(MSG_HANGUP, 0, NULL, NULL, 0);
	m->target = c->link;
	g_main_context_invoke_full(NULL, WK_DISPATCH_PRIORITY, deliver_cb, m,
				   NULL);
}

static gboolean ipc_readable(GSocket *sock, GIOCondition cond, gpointer data)
{
	struct ipc *c = data;
	char buf[IPC_MESSAGE_MAX];
	char cbuf[CMSG_SPACE(sizeof(int) * IPC_ATTACHMENT_MAX)];
	(void)sock;
	(void)cond;
	for (;;) {
		struct iovec iov = { buf, sizeof buf };
		struct msghdr mh;
		memset(&mh, 0, sizeof mh);
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof cbuf;
		ssize_t r = recvmsg(c->fd, &mh, MSG_NOSIGNAL);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return G_SOURCE_CONTINUE;
			fprintf(stderr, "apload: recvmsg: %s\n", strerror(errno));
			ipc_hangup(c);
			return G_SOURCE_REMOVE;
		}
		if (r == 0) {
			ipc_hangup(c);
			return G_SOURCE_REMOVE;
		}
		int fds[IPC_ATTACHMENT_MAX];
		int nfds = 0;
		for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm;
		     cm = CMSG_NXTHDR(&mh, cm)) {
			if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
				continue;
			size_t k = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			if (k > (size_t)(IPC_ATTACHMENT_MAX - nfds))
				k = IPC_ATTACHMENT_MAX - nfds;
			memcpy(fds + nfds, CMSG_DATA(cm), k * sizeof(int));
			nfds += (int)k;
		}
		if ((size_t)r < sizeof(struct ipc_hdr)) {
			fprintf(stderr, "apload: short IPC message (%zd bytes)\n", r);
			for (int i = 0; i < nfds; i++)
				close(fds[i]);
			continue;
		}
		struct ipc_msg *m = g_new0(struct ipc_msg, 1);
		memcpy(&m->h, buf, sizeof m->h);
		m->fd = -1;
		m->target = c->link;
		if (m->h.flags & IPC_F_OUT_OF_LINE) {
			if (nfds < 1) {
				fprintf(stderr, "apload: out-of-line message without "
					"a descriptor\n");
				g_free(m);
				continue;
			}
			m->fd = fds[0];
			m->len = m->h.body_len;
			m->body = mmap(NULL, m->len, PROT_READ, MAP_SHARED, m->fd, 0);
			if (m->body == MAP_FAILED) {
				fprintf(stderr, "apload: mmap of a %zu-byte "
					"out-of-line body: %s\n", m->len,
					strerror(errno));
				exit(1);
			}
			for (int i = 1; i < nfds; i++)
				close(fds[i]);
			c->recv_ool++;
		} else {
			m->len = m->h.body_len;
			if (sizeof m->h + m->len != (size_t)r) {
				fprintf(stderr, "apload: IPC message length %zd does "
					"not match its header (%zu)\n", r,
					sizeof m->h + m->len);
				m->len = (size_t)r - sizeof m->h;
			}
			if (m->len)
				m->body = g_memdup2(buf + sizeof m->h, m->len);
			for (int i = 0; i < nfds; i++)
				close(fds[i]);
		}
		c->recv++;
		c->recv_bytes += m->len;
		g_main_context_invoke_full(NULL, WK_DISPATCH_PRIORITY, deliver_cb,
					   m, NULL);
	}
}

static gpointer ipc_thread(gpointer data)
{
	struct ipc *c = data;
	g_main_context_push_thread_default(c->ctx);
	GError *err = NULL;
	GSocket *sock = g_socket_new_from_fd(c->fd, &err);
	if (!sock) {
		fprintf(stderr, "apload: g_socket_new_from_fd: %s\n", err->message);
		exit(1);
	}
	GSource *src = g_socket_create_source(sock, G_IO_IN | G_IO_HUP | G_IO_ERR,
					      NULL);
	g_source_set_callback(src, G_SOURCE_FUNC(ipc_readable), c, NULL);
	g_source_attach(src, c->ctx);
	g_source_unref(src);
	g_main_loop_run(c->loop);
	g_main_context_pop_thread_default(c->ctx);
	return NULL;
}

static struct ipc *ipc_start(int fd, struct link *link)
{
	struct ipc *c = g_new0(struct ipc, 1);
	c->fd = fd;
	c->link = link;
	link->ipc = c;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	g_mutex_init(&c->lock);
	g_queue_init(&c->out);
	c->ctx = g_main_context_new();
	c->loop = g_main_loop_new(c->ctx, FALSE);
	c->thread = g_thread_new("IPC", ipc_thread, c);
	return c;
}

static gboolean quit_loop_cb(gpointer loop)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

/* Queued sends go out first (they sit at default priority), then the loop
 * quits. */
static void ipc_stop(struct ipc *c)
{
	g_main_context_invoke_full(c->ctx, G_PRIORITY_LOW, quit_loop_cb, c->loop,
				   NULL);
	g_thread_join(c->thread);
}

static void link_send(struct link *l, uint32_t type, uint32_t id,
		      const uint64_t a[4], const void *body, size_t len)
{
	struct ipc_msg *m = msg_new(type, id, a, body, len);
	if (l->ipc) {
		ipc_send(l->ipc, m);
	} else {
		m->target = l->local_peer;
		g_main_context_invoke_full(NULL, WK_DISPATCH_PRIORITY, deliver_cb,
					   m, NULL);
	}
}

/* ======================================================================= */
/* The network process: libsoup driven as NetworkDataTaskSoup does           */
/* ======================================================================= */

enum lstate { LS_QUEUED, LS_STARTED, LS_HEADERS, LS_BODY, LS_REDIRECT, LS_WS,
	      LS_DONE };
enum phase { PH_QUEUE, PH_DNS, PH_CONNECT, PH_TLS, PH_TTFB, PH_BODY, PH_TOTAL,
	     PH_COUNT };
static const char *phase_name[PH_COUNT] = {
	"queue wait", "DNS", "TCP connect", "TLS handshake", "TTFB (req->hdr)",
	"body", "total (LOAD->FINISH)",
};

struct rec {
	char *url, *host, *error;
	int status, h2, newconn, redirects, rc;
	guint64 bytes, wire;
	double ph[PH_COUNT];
};

struct hstat {
	char *host;
	int n, ok, fail, newconn, h2;
	guint64 bytes;
	double sum_total, max_total, sum_ttfb;
	int n_ttfb;
};

struct net;

struct load {
	struct net *net;
	uint32_t id;
	int index;
	enum rclass rc;
	char *url;
	SoupMessage *msg;
	GInputStream *stream;
	char buf[WK_READ_SIZE];
	enum lstate state;
	double t_queued, t_start, t_headers, t_first, t_done;
	guint64 bytes;
	int redirects, status;
	GByteArray *pending;
	guint buffer_timer;
	SoupWebsocketConnection *ws;
};

struct net {
	struct link link;
	SoupSession *session;
	char *ua;
	char *site;
	GUri *site_uri;
	GHashTable *loads;
	GMainLoop *loop;
	double t_first_load, t_last_done, last_progress, stall_since, stall_printed;
	double tl_last;
	guint64 tl_bytes, tl_conns;
	guint64 n_load, n_ok, n_http_err, n_fail, n_redirect, n_restart, n_ws;
	guint64 body_bytes, wire_bytes, conns_new, conns_tls, reused, h1, h2;
	guint64 data_msgs;
	int inflight, peak, stalls;
	double longest_stall;
	GArray *ph[PH_COUNT];
	GHashTable *hosts;
	GPtrArray *recs;
	struct {
		int n;
		guint64 bytes;
		double sum;
	} cls[RC_COUNT];
	int shutdown;
	guint tick_id, timeline_id;
};

static void net_progress(struct net *n)
{
	n->last_progress = now_s();
}

static void load_free(gpointer p)
{
	struct load *l = p;
	if (l->buffer_timer)
		g_source_remove(l->buffer_timer);
	if (l->msg) {
		g_signal_handlers_disconnect_by_data(l->msg, l);
		g_object_unref(l->msg);
	}
	if (l->stream)
		g_object_unref(l->stream);
	if (l->ws)
		g_object_unref(l->ws);
	if (l->pending)
		g_byte_array_unref(l->pending);
	g_free(l->url);
	g_free(l);
}

static struct hstat *host_stat(struct net *n, const char *host)
{
	struct hstat *h = g_hash_table_lookup(n->hosts, host);
	if (!h) {
		h = g_new0(struct hstat, 1);
		h->host = g_strdup(host);
		g_hash_table_insert(n->hosts, h->host, h);
	}
	return h;
}

/* Connection accounting for one SoupMessage (a redirect hop or the final
 * request): libsoup's metrics say whether it opened a connection. */
static void account_connection(struct net *n, SoupMessage *msg, struct rec *r,
			       struct hstat *h)
{
	SoupMessageMetrics *m = soup_message_get_metrics(msg);
	int newconn = m && soup_message_metrics_get_connect_start(m) != 0;
	int tls = m && soup_message_metrics_get_tls_start(m) != 0;
	int h2 = soup_message_get_http_version(msg) == SOUP_HTTP_2_0;
	if (newconn) {
		n->conns_new++;
		if (tls)
			n->conns_tls++;
	} else {
		n->reused++;
	}
	if (h2)
		n->h2++;
	else
		n->h1++;
	if (r) {
		r->newconn = newconn;
		r->h2 = h2;
	}
	if (h) {
		h->newconn += newconn;
		h->h2 += h2;
	}
}

static void send_data(struct net *n, struct load *l, const void *p, size_t len)
{
	uint64_t a[4] = { (uint64_t)g_get_monotonic_time(), 0, 0, 0 };
	link_send(&n->link, MSG_DATA, l->id, a, p, len);
	n->data_msgs++;
}

static void flush_pending(struct net *n, struct load *l)
{
	if (l->pending && l->pending->len) {
		send_data(n, l, l->pending->data, l->pending->len);
		g_byte_array_set_size(l->pending, 0);
	}
}

static gboolean buffer_timer_cb(gpointer data)
{
	struct load *l = data;
	l->buffer_timer = 0;
	flush_pending(l->net, l);
	return G_SOURCE_REMOVE;
}

static void finish(struct net *n, struct load *l, const char *error)
{
	l->t_done = now_s();
	l->state = LS_DONE;
	if (l->buffer_timer) {
		g_source_remove(l->buffer_timer);
		l->buffer_timer = 0;
	}
	if (!error)
		flush_pending(n, l);

	struct rec *r = g_new0(struct rec, 1);
	r->url = g_strdup(l->url);
	r->status = l->status;
	r->bytes = l->bytes;
	r->rc = l->rc;
	r->redirects = l->redirects;
	r->error = error ? g_strdup(error) : NULL;
	for (int i = 0; i < PH_COUNT; i++)
		r->ph[i] = -1;
	r->ph[PH_TOTAL] = (l->t_done - l->t_queued) * 1000;

	if (l->msg) {
		GUri *u = soup_message_get_uri(l->msg);
		const char *host = u ? g_uri_get_host(u) : NULL;
		r->host = g_strdup(host ? host : "?");
		SoupMessageMetrics *m = soup_message_get_metrics(l->msg);
		if (m) {
			guint64 fs = soup_message_metrics_get_fetch_start(m);
			guint64 ds = soup_message_metrics_get_dns_start(m);
			guint64 de = soup_message_metrics_get_dns_end(m);
			guint64 cs = soup_message_metrics_get_connect_start(m);
			guint64 ce = soup_message_metrics_get_connect_end(m);
			guint64 ts = soup_message_metrics_get_tls_start(m);
			guint64 rs = soup_message_metrics_get_request_start(m);
			guint64 ps = soup_message_metrics_get_response_start(m);
			guint64 pe = soup_message_metrics_get_response_end(m);
			if (fs)
				r->ph[PH_QUEUE] = fs / 1000.0 - l->t_queued * 1000;
			if (ds && de)
				r->ph[PH_DNS] = (de - ds) / 1000.0;
			if (cs && ce) {
				r->ph[PH_CONNECT] = ((ts ? ts : ce) - cs) / 1000.0;
				if (ts)
					r->ph[PH_TLS] = (ce - ts) / 1000.0;
			}
			if (rs && ps)
				r->ph[PH_TTFB] = (ps - rs) / 1000.0;
			if (ps)
				r->ph[PH_BODY] = ((pe ? pe / 1e6 : l->t_done) - ps / 1e6) * 1000;
			r->wire = soup_message_metrics_get_response_header_bytes_received(m) +
				  soup_message_metrics_get_response_body_bytes_received(m);
		}
	} else {
		r->host = g_strdup("?");
	}

	int ok = !error;
	struct hstat *h = host_stat(n, r->host);
	h->n++;
	h->bytes += r->bytes;
	h->sum_total += r->ph[PH_TOTAL];
	if (r->ph[PH_TOTAL] > h->max_total)
		h->max_total = r->ph[PH_TOTAL];
	if (r->ph[PH_TTFB] >= 0) {
		h->sum_ttfb += r->ph[PH_TTFB];
		h->n_ttfb++;
	}
	if (l->msg && l->rc != RC_WEBSOCKET)
		account_connection(n, l->msg, r, h);
	if (ok) {
		h->ok++;
		n->n_ok++;
		if (r->status >= 400)
			n->n_http_err++;
		for (int i = 0; i < PH_COUNT; i++)
			if (r->ph[i] >= 0)
				g_array_append_val(n->ph[i], r->ph[i]);
	} else {
		h->fail++;
		n->n_fail++;
	}
	n->wire_bytes += r->wire;
	n->cls[l->rc].n++;
	n->cls[l->rc].bytes += r->bytes;
	n->cls[l->rc].sum += r->ph[PH_TOTAL];
	g_ptr_array_add(n->recs, r);

	n->inflight--;
	n->t_last_done = l->t_done;
	net_progress(n);

	if (opt.verbose) {
		char sb[80];
		printf("  [net] #%-4u %6.0f ms  %3d %9llu B  %s%s%s  %s\n", l->id,
		       r->ph[PH_TOTAL], r->status, (unsigned long long)r->bytes,
		       r->h2 ? "h2" : "h1", r->newconn ? " new" : "    ",
		       error ? "  FAILED" : "",
		       error ? error : short_url(l->url, sb, sizeof sb));
	}

	uint64_t a[4] = { (uint64_t)r->status, r->bytes, (uint64_t)ok,
			  (uint64_t)g_get_monotonic_time() };
	link_send(&n->link, MSG_FINISH, l->id, a, error,
		  error ? strlen(error) + 1 : 0);
	g_hash_table_remove(n->loads, GUINT_TO_POINTER(l->id));
}

static void start_request(struct net *n, struct load *l);

static void read_cb(GObject *src, GAsyncResult *res, gpointer data);

static void issue_read(struct load *l)
{
	g_input_stream_read_async(l->stream, l->buf, WK_READ_SIZE, WK_IO_PRIORITY,
				  NULL, read_cb, l);
}

static void did_read(struct net *n, struct load *l, gsize len)
{
	if (!l->t_first)
		l->t_first = now_s();
	l->bytes += len;
	n->body_bytes += len;
	net_progress(n);
	int bm = rc_info[l->rc].buffer_ms;
	if (bm == 0) {
		send_data(n, l, l->buf, len);
		return;
	}
	if (!l->pending)
		l->pending = g_byte_array_new();
	g_byte_array_append(l->pending, (const guint8 *)l->buf, (guint)len);
	if (bm > 0 && !l->buffer_timer)
		l->buffer_timer = g_timeout_add((guint)bm, buffer_timer_cb, l);
}

static void read_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct load *l = data;
	struct net *n = l->net;
	GError *err = NULL;
	gssize r = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
	if (r < 0) {
		char *msg = g_strdup_printf("read: %s", err->message);
		g_error_free(err);
		finish(n, l, msg);
		g_free(msg);
		return;
	}
	if (r > 0) {
		did_read(n, l, (gsize)r);
		issue_read(l);
		return;
	}
	g_input_stream_close(l->stream, NULL, NULL);
	finish(n, l, NULL);
}

static void redirect(struct net *n, struct load *l)
{
	SoupMessageHeaders *rh = soup_message_get_response_headers(l->msg);
	const char *loc = soup_message_headers_get_one(rh, "Location");
	GError *err = NULL;
	GUri *next = loc ? g_uri_parse_relative(soup_message_get_uri(l->msg), loc,
						 WK_URI_FLAGS, &err) : NULL;
	if (!next) {
		g_clear_error(&err);
		finish(n, l, "redirect without a usable Location");
		return;
	}
	char *s = g_uri_to_string(next);
	g_uri_unref(next);
	if (++l->redirects > WK_MAX_REDIRECTS) {
		g_free(s);
		finish(n, l, "too many redirects");
		return;
	}
	n->n_redirect++;
	net_progress(n);
	account_connection(n, l->msg, NULL, NULL);
	if (opt.verbose) {
		char sb[70];
		printf("  [net] #%-4u %3d -> %s\n", l->id, l->status,
		       short_url(s, sb, sizeof sb));
	}
	g_signal_handlers_disconnect_by_data(l->msg, l);
	g_object_unref(l->msg);
	l->msg = NULL;
	if (l->stream) {
		g_object_unref(l->stream);
		l->stream = NULL;
	}
	g_free(l->url);
	l->url = s;
	start_request(n, l);
}

static void skip_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct load *l = data;
	struct net *n = l->net;
	GError *err = NULL;
	gssize r = g_input_stream_skip_finish(G_INPUT_STREAM(src), res, &err);
	if (r < 0) {
		char *msg = g_strdup_printf("redirect body: %s", err->message);
		g_error_free(err);
		finish(n, l, msg);
		g_free(msg);
		return;
	}
	if (r > 0) {
		net_progress(n);
		g_input_stream_skip_async(l->stream, WK_READ_SIZE, WK_IO_PRIORITY,
					  NULL, skip_cb, l);
		return;
	}
	g_input_stream_close(l->stream, NULL, NULL);
	redirect(n, l);
}

static void send_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct load *l = data;
	struct net *n = l->net;
	GError *err = NULL;
	GInputStream *s = soup_session_send_finish(SOUP_SESSION(src), res, &err);
	if (!s) {
		char *msg = g_strdup_printf("send: %s", err->message);
		g_error_free(err);
		finish(n, l, msg);
		g_free(msg);
		return;
	}
	l->stream = s;
	l->status = (int)soup_message_get_status(l->msg);
	net_progress(n);
	SoupMessageHeaders *rh = soup_message_get_response_headers(l->msg);
	if (SOUP_STATUS_IS_REDIRECTION(l->status) && l->status != 304 &&
	    soup_message_headers_get_one(rh, "Location")) {
		l->state = LS_REDIRECT;
		g_input_stream_skip_async(l->stream, WK_READ_SIZE, WK_IO_PRIORITY,
					  NULL, skip_cb, l);
		return;
	}
	l->state = LS_BODY;
	goffset clen = soup_message_headers_get_content_length(rh);
	uint64_t a[4] = { (uint64_t)l->status, clen > 0 ? (uint64_t)clen : 0,
			  (uint64_t)soup_message_get_http_version(l->msg),
			  (uint64_t)g_get_monotonic_time() };
	link_send(&n->link, MSG_RESPONSE, l->id, a, NULL, 0);
	issue_read(l);
}

static void on_starting(SoupMessage *msg, gpointer data)
{
	struct load *l = data;
	(void)msg;
	l->t_start = now_s();
	if (l->state == LS_QUEUED)
		l->state = LS_STARTED;
	net_progress(l->net);
}

static void on_got_headers(SoupMessage *msg, gpointer data)
{
	struct load *l = data;
	l->t_headers = now_s();
	l->status = (int)soup_message_get_status(msg);
	if (l->state == LS_STARTED)
		l->state = LS_HEADERS;
	net_progress(l->net);
}

static void on_restarted(SoupMessage *msg, gpointer data)
{
	struct load *l = data;
	(void)msg;
	l->net->n_restart++;
	net_progress(l->net);
}

static gboolean accept_any_cert(SoupMessage *msg, GTlsCertificate *cert,
				GTlsCertificateFlags flags, gpointer data)
{
	(void)msg;
	(void)cert;
	(void)flags;
	(void)data;
	return TRUE;
}

static void ws_closed(SoupWebsocketConnection *ws, gpointer data)
{
	struct load *l = data;
	(void)ws;
	if (l->state != LS_DONE)
		finish(l->net, l, NULL);
}

static void ws_cb(GObject *src, GAsyncResult *res, gpointer data)
{
	struct load *l = data;
	struct net *n = l->net;
	GError *err = NULL;
	SoupWebsocketConnection *ws =
		soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
	if (!ws) {
		char *msg = g_strdup_printf("websocket: %s", err->message);
		g_error_free(err);
		finish(n, l, msg);
		g_free(msg);
		return;
	}
	l->ws = ws;
	l->status = 101;
	l->state = LS_WS;
	n->n_ws++;
	net_progress(n);
	g_signal_connect(ws, "closed", G_CALLBACK(ws_closed), l);
	soup_websocket_connection_close(ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
}

static void start_request(struct net *n, struct load *l)
{
	/* URL::createGUri() (URLGLib.cpp): the same flags, relaxed parsing
	 * included, so a stray "%" in an ad-network macro is tolerated exactly
	 * as far as the browser tolerates it. */
	GUri *uri = g_uri_parse(l->url, WK_URI_FLAGS, NULL);
	if (!uri) {
		finish(n, l, "URL rejected by g_uri_parse");
		return;
	}
	SoupMessage *msg = soup_message_new_from_uri("GET", uri);
	g_uri_unref(uri);
	if (!msg) {
		finish(n, l, "URL rejected by libsoup");
		return;
	}
	l->msg = msg;
	soup_message_set_flags(msg, soup_message_get_flags(msg) |
				    SOUP_MESSAGE_NO_REDIRECT |
				    SOUP_MESSAGE_COLLECT_METRICS);
	soup_message_set_priority(msg, rc_info[l->rc].prio);
	SoupMessageHeaders *h = soup_message_get_request_headers(msg);
	soup_message_headers_append(h, "Accept", rc_info[l->rc].accept);
	soup_message_headers_append(h, "User-Agent", n->ua);
	if (n->site && l->index != 0)
		soup_message_headers_append(h, "Referer", n->site);
	if (n->site_uri)
		soup_message_set_site_for_cookies(msg, n->site_uri);
	soup_message_set_is_top_level_navigation(msg, l->index == 0);
	g_signal_connect(msg, "starting", G_CALLBACK(on_starting), l);
	g_signal_connect(msg, "got-headers", G_CALLBACK(on_got_headers), l);
	g_signal_connect(msg, "restarted", G_CALLBACK(on_restarted), l);
	if (opt.insecure)
		g_signal_connect(msg, "accept-certificate",
				 G_CALLBACK(accept_any_cert), l);
	l->state = LS_QUEUED;
	if (l->rc == RC_WEBSOCKET) {
		soup_session_websocket_connect_async(n->session, msg, NULL, NULL,
						     WK_IO_PRIORITY, NULL, ws_cb, l);
		return;
	}
	soup_session_send_async(n->session, msg, WK_IO_PRIORITY, NULL, send_cb, l);
}

static const char *live_phase(struct load *l, char *buf, size_t sz)
{
	if (l->state == LS_DONE)
		return "done";
	if (l->state == LS_WS)
		return "websocket closing";
	if (l->state == LS_REDIRECT)
		return "draining redirect body";
	SoupMessageMetrics *m = l->msg ? soup_message_get_metrics(l->msg) : NULL;
	if (!m || !soup_message_metrics_get_fetch_start(m))
		return "queued in libsoup (no connection slot)";
	if (soup_message_metrics_get_dns_start(m) &&
	    !soup_message_metrics_get_dns_end(m))
		return "DNS";
	if (soup_message_metrics_get_connect_start(m) &&
	    !soup_message_metrics_get_connect_end(m))
		return soup_message_metrics_get_tls_start(m) ? "TLS handshake"
							     : "TCP connect";
	if (!soup_message_metrics_get_request_start(m))
		return "connected, request not yet sent";
	if (!soup_message_metrics_get_response_start(m))
		return "request sent, waiting for headers";
	snprintf(buf, sz, "reading body (%llu bytes so far)",
		 (unsigned long long)l->bytes);
	return buf;
}

/* The peer's ip:port, which is what the kernel's TCP connection dump
 * lists, so a stuck load can be matched to its connection there. */
static const char *load_remote(struct load *l, char *buf, size_t sz)
{
	GSocketAddress *a = l->msg ? soup_message_get_remote_address(l->msg) : NULL;
	if (!a || !G_IS_INET_SOCKET_ADDRESS(a))
		return "-";
	GInetSocketAddress *ia = G_INET_SOCKET_ADDRESS(a);
	char *ip = g_inet_address_to_string(g_inet_socket_address_get_address(ia));
	snprintf(buf, sz, "%s:%u", ip, g_inet_socket_address_get_port(ia));
	g_free(ip);
	return buf;
}

static void print_inflight(struct net *n, int max)
{
	GHashTableIter it;
	gpointer k, v;
	int shown = 0;
	double now = now_s();
	g_hash_table_iter_init(&it, n->loads);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		struct load *l = v;
		char pb[64], ub[56], rb[48];
		if (shown++ >= max) {
			printf("      ... and %d more\n", n->inflight - max);
			break;
		}
		printf("      #%-4u %6.1fs  %-38s %-21s %s\n", l->id,
		       now - l->t_queued, live_phase(l, pb, sizeof pb),
		       load_remote(l, rb, sizeof rb), short_url(l->url, ub, sizeof ub));
	}
}

static void net_close_stall(struct net *n, double now);

static gboolean net_tick(gpointer data)
{
	struct net *n = data;
	double now = now_s();
	if (n->inflight > 0 && !n->shutdown) {
		double idle = now - n->last_progress;
		if (idle >= opt.stall_s) {
			if (!n->stall_since) {
				n->stall_since = n->last_progress;
				n->stalls++;
				n->stall_printed = now;
				printf("  STALL[net] t=%.1fs: no progress for %.1f s, %d in "
				       "flight:\n", REL(now), idle, n->inflight);
				print_inflight(n, 12);
			} else if (now - n->stall_printed >= 2.0) {
				n->stall_printed = now;
				printf("  STALL[net] t=%.1fs: still nothing, %.1f s\n",
				       REL(now), idle);
			}
		}
	}
	net_close_stall(n, now);
	return G_SOURCE_CONTINUE;
}

static gboolean net_timeline(gpointer data)
{
	struct net *n = data;
	double now = now_s();
	if (!n->tl_last) {
		n->tl_last = now;
		return G_SOURCE_CONTINUE;
	}
	if (n->inflight == 0 && n->tl_bytes == n->body_bytes &&
	    n->tl_conns == n->conns_new) {
		n->tl_last = now;
		return G_SOURCE_CONTINUE;
	}
	double dt = now - n->tl_last;
	char b1[32], b2[32];
	printf("  t=%6.1fs  done=%-5llu inflight=%-4d body=%-10s %8s/s  "
	       "new-conns=%-4llu h2=%-4llu stalls=%d\n", REL(now),
	       (unsigned long long)(n->n_ok + n->n_fail), n->inflight,
	       fmt_bytes(n->body_bytes, b1, sizeof b1),
	       fmt_bytes((guint64)((n->body_bytes - n->tl_bytes) / (dt > 0 ? dt : 1)),
			 b2, sizeof b2),
	       (unsigned long long)n->conns_new, (unsigned long long)n->h2,
	       n->stalls);
	n->tl_last = now;
	n->tl_bytes = n->body_bytes;
	n->tl_conns = n->conns_new;
	return G_SOURCE_CONTINUE;
}

static int cmp_rec_total(gconstpointer a, gconstpointer b)
{
	const struct rec *x = *(struct rec *const *)a, *y = *(struct rec *const *)b;
	return (y->ph[PH_TOTAL] > x->ph[PH_TOTAL]) - (y->ph[PH_TOTAL] < x->ph[PH_TOTAL]);
}

static int cmp_host_total(gconstpointer a, gconstpointer b)
{
	const struct hstat *x = *(struct hstat *const *)a,
			   *y = *(struct hstat *const *)b;
	return (y->sum_total > x->sum_total) - (y->sum_total < x->sum_total);
}

static void ms_or_dash(double v, char *buf, size_t sz)
{
	if (v < 0)
		snprintf(buf, sz, "%6s", "-");
	else
		snprintf(buf, sz, "%6.0f", v);
}

static void net_close_stall(struct net *n, double now)
{
	if (!n->stall_since)
		return;
	double end = n->last_progress > n->stall_since ? n->last_progress : now;
	double d = end - n->stall_since;
	if (d > n->longest_stall)
		n->longest_stall = d;
	if (n->last_progress > n->stall_since) {
		printf("  STALL[net] t=%.1fs: progress resumed after %.1f s\n",
		       REL(now), d);
		n->stall_since = 0;
	}
}

static void net_report(struct net *n)
{
	char b1[32], b2[32];
	net_close_stall(n, now_s());
	double wall = n->t_last_done - n->t_first_load;
	printf("\n=== apload: network process report (libsoup side) ===\n");
	printf("loads: %llu received, %llu completed (%llu with HTTP status >= 400), "
	       "%llu failed; %llu redirects followed, %llu restarts, %llu websockets\n",
	       (unsigned long long)n->n_load, (unsigned long long)n->n_ok,
	       (unsigned long long)n->n_http_err, (unsigned long long)n->n_fail,
	       (unsigned long long)n->n_redirect, (unsigned long long)n->n_restart,
	       (unsigned long long)n->n_ws);
	printf("bytes: %s of body delivered (decoded), %s on the wire\n",
	       fmt_bytes(n->body_bytes, b1, sizeof b1),
	       fmt_bytes(n->wire_bytes, b2, sizeof b2));
	printf("connections: %llu opened (%llu TLS), %llu requests on reused "
	       "connections; HTTP/2 %llu, HTTP/1.x %llu\n",
	       (unsigned long long)n->conns_new, (unsigned long long)n->conns_tls,
	       (unsigned long long)n->reused, (unsigned long long)n->h2,
	       (unsigned long long)n->h1);
	printf("wall (first LOAD -> last FINISH): %.2f s, %s/s; peak in flight %d; "
	       "DATA messages %llu\n", wall,
	       fmt_bytes((guint64)(n->body_bytes / (wall > 0 ? wall : 1)), b1,
			 sizeof b1), n->peak, (unsigned long long)n->data_msgs);
	printf("stalls (>= %.1f s without progress): %d, longest %.1f s\n",
	       opt.stall_s, n->stalls, n->longest_stall);
	if (n->link.ipc) {
		struct ipc *c = n->link.ipc;
		g_mutex_lock(&c->lock);
		printf("IPC: %llu messages sent (%llu out of line = %llu shm segments), "
		       "%s, %llu POLLOUT waits; %llu received\n",
		       (unsigned long long)c->sent, (unsigned long long)c->sent_ool,
		       (unsigned long long)n->link.shm_segments,
		       fmt_bytes(c->sent_bytes, b1, sizeof b1),
		       (unsigned long long)c->pollout_waits,
		       (unsigned long long)c->recv);
		g_mutex_unlock(&c->lock);
	}

	printf("\nphases of completed loads, ms (libsoup metrics; DNS/connect/TLS "
	       "only for loads that opened a connection):\n");
	pct_header("phase");
	for (int i = 0; i < PH_COUNT; i++)
		pct_row(phase_name[i], n->ph[i]);

	printf("\nby resource class (guessed from the URL):\n");
	printf("  %-10s %6s %12s %10s\n", "class", "n", "bytes", "avg ms");
	for (int i = 0; i < RC_COUNT; i++) {
		if (!n->cls[i].n)
			continue;
		printf("  %-10s %6d %12s %10.0f\n", rc_info[i].name, n->cls[i].n,
		       fmt_bytes(n->cls[i].bytes, b1, sizeof b1),
		       n->cls[i].sum / n->cls[i].n);
	}

	printf("\nhosts, top 20 by summed load time:\n");
	printf("  %-36s %5s %4s %4s %5s %4s %10s %8s %8s\n", "host", "n", "fail",
	       "conn", "h2", "", "bytes", "avg ttfb", "max ms");
	GPtrArray *hosts = g_ptr_array_new();
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, n->hosts);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_ptr_array_add(hosts, v);
	g_ptr_array_sort(hosts, cmp_host_total);
	for (guint i = 0; i < hosts->len && i < 20; i++) {
		struct hstat *h = hosts->pdata[i];
		char hb[40];
		printf("  %-36s %5d %4d %4d %5d %4s %10s %8.0f %8.0f\n",
		       short_url(h->host, hb, 37), h->n, h->fail, h->newconn, h->h2,
		       "", fmt_bytes(h->bytes, b1, sizeof b1),
		       h->n_ttfb ? h->sum_ttfb / h->n_ttfb : 0, h->max_total);
	}
	printf("  (%u hosts in total)\n", hosts->len);
	g_ptr_array_free(hosts, TRUE);

	printf("\nslowest 20 loads (ms):\n");
	printf("  %6s %6s %6s %6s %6s %6s  %3s %9s  %s\n", "total", "queue",
	       "dns", "conn", "tls", "ttfb", "st", "bytes", "url");
	g_ptr_array_sort(n->recs, cmp_rec_total);
	for (guint i = 0; i < n->recs->len && i < 20; i++) {
		struct rec *r = n->recs->pdata[i];
		char c[PH_COUNT][8], ub[60];
		for (int p = 0; p < PH_COUNT; p++)
			ms_or_dash(r->ph[p], c[p], sizeof c[p]);
		printf("  %s %s %s %s %s %s  %3d %9llu  %s%s%s\n", c[PH_TOTAL],
		       c[PH_QUEUE], c[PH_DNS], c[PH_CONNECT], c[PH_TLS], c[PH_TTFB],
		       r->status, (unsigned long long)r->bytes,
		       short_url(r->url, ub, sizeof ub),
		       r->error ? "  FAILED: " : "", r->error ? r->error : "");
	}

	int nfail = 0;
	for (guint i = 0; i < n->recs->len; i++)
		nfail += ((struct rec *)n->recs->pdata[i])->error != NULL;
	if (nfail) {
		printf("\nfailures (%d, first 30):\n", nfail);
		int shown = 0;
		for (guint i = 0; i < n->recs->len && shown < 30; i++) {
			struct rec *r = n->recs->pdata[i];
			char ub[70];
			if (!r->error)
				continue;
			shown++;
			printf("  %6.0f ms  %s  %s\n", r->ph[PH_TOTAL],
			       short_url(r->url, ub, sizeof ub), r->error);
		}
	}
	fflush(stdout);
}

static void net_handle(struct ipc_msg *m, void *user)
{
	struct net *n = user;
	switch (m->h.type) {
	case MSG_LOAD: {
		struct load *l = g_new0(struct load, 1);
		l->net = n;
		l->id = m->h.id;
		l->rc = (enum rclass)m->h.a[0];
		l->index = (int)m->h.a[1];
		l->url = g_strndup(m->body, m->len);
		l->t_queued = now_s();
		if (!n->t_first_load)
			n->t_first_load = l->t_queued;
		if (l->index == 0 && !n->site) {
			n->site = g_strdup(l->url);
			n->site_uri = g_uri_parse(l->url, WK_URI_FLAGS, NULL);
		}
		g_hash_table_insert(n->loads, GUINT_TO_POINTER(l->id), l);
		n->n_load++;
		n->inflight++;
		if (n->inflight > n->peak)
			n->peak = n->inflight;
		net_progress(n);
		start_request(n, l);
		break;
	}
	case MSG_SHUTDOWN:
		n->shutdown = 1;
		if (n->inflight)
			printf("  [net] shutdown with %d loads still in flight:\n",
			       n->inflight);
		if (n->inflight)
			print_inflight(n, 30);
		net_report(n);
		link_send(&n->link, MSG_BYE, 0, NULL, NULL, 0);
		if (n->loop)
			g_main_loop_quit(n->loop);
		break;
	case MSG_HANGUP:
		fprintf(stderr, "apload[net]: the web process went away\n");
		if (n->loop)
			g_main_loop_quit(n->loop);
		break;
	default:
		fprintf(stderr, "apload[net]: unexpected message %u\n", m->h.type);
	}
}

static void net_init(struct net *n)
{
	struct utsname u;
	memset(&u, 0, sizeof u);
	uname(&u);
	/* UserAgentGLib.cpp: platform, then "sysname machine" from uname. */
	n->ua = g_strdup_printf("Mozilla/5.0 (X11; %s %s) AppleWebKit/605.1.15 "
				"(KHTML, like Gecko) Version/60.5 Safari/605.1.15",
				u.sysname, u.machine);

	n->session = soup_session_new_with_options(
		"max-conns", WK_MAX_CONNS,
		"max-conns-per-host", WK_MAX_CONNS_PER_HOST,
		"idle-timeout", WK_IDLE_TIMEOUT_S,
		"timeout", 0,
		NULL);
	soup_session_add_feature_by_type(n->session, SOUP_TYPE_CONTENT_SNIFFER);
	soup_session_add_feature_by_type(n->session, SOUP_TYPE_HSTS_ENFORCER);
	soup_session_add_feature_by_type(n->session,
					 SOUP_TYPE_WEBSOCKET_EXTENSION_MANAGER);
	SoupCookieJar *jar = soup_cookie_jar_new();
	soup_cookie_jar_set_accept_policy(jar, SOUP_COOKIE_JAR_ACCEPT_NO_THIRD_PARTY);
	soup_session_add_feature(n->session, SOUP_SESSION_FEATURE(jar));
	g_object_unref(jar);
	soup_session_set_accept_language_auto(n->session, TRUE);

	n->loads = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
					 load_free);
	n->hosts = g_hash_table_new(g_str_hash, g_str_equal);
	n->recs = g_ptr_array_new();
	for (int i = 0; i < PH_COUNT; i++)
		n->ph[i] = g_array_new(FALSE, FALSE, sizeof(double));
	n->last_progress = now_s();
	n->link.on_receive = net_handle;
	n->link.user = n;
	n->tick_id = g_timeout_add(250, net_tick, n);
	if (opt.timeline)
		n->timeline_id = g_timeout_add(1000, net_timeline, n);
}

static int net_main(int fd)
{
	struct net *n = g_new0(struct net, 1);
	net_init(n);
	ipc_start(fd, &n->link);
	n->loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(n->loop);
	ipc_stop(n->link.ipc);
	return 0;
}

/* ======================================================================= */
/* The web process: issues loads, consumes bytes, keeps the clock            */
/* ======================================================================= */

struct wload {
	char *url;
	enum rclass rc;
	double t_sent, t_resp, t_fin;
	guint64 bytes;
	int status, ok, done, issued;
};

struct web {
	struct link link;
	GPtrArray *loads;
	GHashTable *skipped;
	int n_file, n_encoded, next, inflight, done, peak, doc_done;
	double t_first, t_end, last_msg, stall_since, stall_printed, t_shutdown;
	int stalls, shutdown_sent, timed_out, child_gone;
	double longest;
	guint64 bytes, data_msgs, data_inline, data_ool, checksum;
	double lat_sum;
	guint64 lat_n;
	GArray *e2e, *ttfb;
	GMainLoop *loop;
	pid_t child;
};

static void web_issue(struct web *w, int i)
{
	struct wload *wl = w->loads->pdata[i];
	wl->t_sent = now_s();
	wl->issued = 1;
	if (!w->t_first)
		w->t_first = wl->t_sent;
	w->inflight++;
	if (w->inflight > w->peak)
		w->peak = w->inflight;
	uint64_t a[4] = { (uint64_t)wl->rc, (uint64_t)i, 0, 0 };
	link_send(&w->link, MSG_LOAD, (uint32_t)i, a, wl->url, strlen(wl->url));
}

static void web_shutdown(struct web *w)
{
	if (w->shutdown_sent)
		return;
	w->shutdown_sent = 1;
	w->t_shutdown = now_s();
	if (!w->t_end)
		w->t_end = w->t_shutdown;
	link_send(&w->link, MSG_SHUTDOWN, 0, NULL, NULL, 0);
}

/* The document goes first and alone -- nothing is discovered before it
 * arrives -- then the rest in file order, -w at a time. */
static void web_pump(struct web *w)
{
	while (w->next < (int)w->loads->len &&
	       (opt.window == 0 || w->inflight < opt.window)) {
		if (w->next > 0 && !w->doc_done)
			break;
		web_issue(w, w->next++);
	}
	if (w->next >= (int)w->loads->len && w->inflight == 0 && !w->shutdown_sent) {
		w->t_end = now_s();
		printf("  [web] all loads finished at t=%.2fs\n", REL(w->t_end));
		web_shutdown(w);
	}
}

static void web_handle(struct ipc_msg *m, void *user)
{
	struct web *w = user;
	double now = now_s();
	w->last_msg = now;
	struct wload *wl = m->h.id < w->loads->len ? w->loads->pdata[m->h.id] : NULL;
	switch (m->h.type) {
	case MSG_RESPONSE:
		if (!wl)
			break;
		wl->t_resp = now;
		wl->status = (int)m->h.a[0];
		{
			double t = (now - wl->t_sent) * 1000;
			g_array_append_val(w->ttfb, t);
		}
		if (m->h.id == 0)
			printf("  [web] document: HTTP %d after %.0f ms (%s)\n",
			       wl->status, (now - wl->t_sent) * 1000,
			       m->h.a[2] == SOUP_HTTP_2_0 ? "h2" : "h1");
		break;
	case MSG_DATA: {
		/* WebResourceLoader::didReceiveData: the bytes are copied out of
		 * the IPC buffer or the shared segment and handed to the parser.
		 * Copy and touch every byte, so the mapping is really read. */
		guint8 *copy = g_malloc(m->len ? m->len : 1);
		memcpy(copy, m->body, m->len);
		guint64 sum = 0;
		for (size_t i = 0; i < m->len; i += 64)
			sum += copy[i];
		w->checksum += sum;
		g_free(copy);
		w->bytes += m->len;
		w->data_msgs++;
		if (m->fd >= 0)
			w->data_ool++;
		else
			w->data_inline++;
		w->lat_sum += now - m->h.a[0] / 1e6;
		w->lat_n++;
		if (wl)
			wl->bytes += m->len;
		break;
	}
	case MSG_FINISH:
		if (!wl || wl->done)
			break;
		wl->done = 1;
		wl->t_fin = now;
		wl->status = (int)m->h.a[0];
		wl->ok = (int)m->h.a[2];
		w->lat_sum += now - m->h.a[3] / 1e6;
		w->lat_n++;
		{
			double t = (now - wl->t_sent) * 1000;
			g_array_append_val(w->e2e, t);
		}
		w->done++;
		w->inflight--;
		if (m->h.id == 0) {
			w->doc_done = 1;
			printf("  [web] document complete after %.0f ms, %llu bytes%s\n",
			       (now - wl->t_sent) * 1000, (unsigned long long)wl->bytes,
			       wl->ok ? "" : " (FAILED)");
		}
		web_pump(w);
		break;
	case MSG_BYE:
		g_main_loop_quit(w->loop);
		break;
	case MSG_HANGUP:
		fprintf(stderr, "apload[web]: the network process went away\n");
		w->child_gone = 1;
		g_main_loop_quit(w->loop);
		break;
	default:
		fprintf(stderr, "apload[web]: unexpected message %u\n", m->h.type);
	}
}

static void web_print_inflight(struct web *w, int max)
{
	double now = now_s();
	int shown = 0;
	for (guint i = 0; i < w->loads->len; i++) {
		struct wload *wl = w->loads->pdata[i];
		char ub[70];
		if (!wl->issued || wl->done)
			continue;
		if (shown++ >= max) {
			printf("      ... and %d more\n", w->inflight - max);
			break;
		}
		printf("      #%-4u %6.1fs  %-22s %s\n", i, now - wl->t_sent,
		       wl->t_resp ? "body" : "no response yet",
		       short_url(wl->url, ub, sizeof ub));
	}
}

static void web_close_stall(struct web *w, double now);

/* ^C: the first one ends the run the orderly way, so both reports still
 * print with whatever was in flight listed; the second one is the default
 * SIGINT.  The network process ignores SIGINT and waits for SHUTDOWN. */
static volatile sig_atomic_t g_sigint;

static void on_sigint(int sig)
{
	if (g_sigint++) {
		signal(sig, SIG_DFL);
		raise(sig);
	}
}

static gboolean web_tick(gpointer data)
{
	struct web *w = data;
	double now = now_s();
	if (g_sigint && !w->shutdown_sent) {
		printf("  [web] interrupted at t=%.1fs with %d in flight, %d of %u "
		       "done; shutting down (^C again to kill)\n", REL(now),
		       w->inflight, w->done, w->loads->len);
		web_print_inflight(w, 30);
		w->timed_out = 1;
		web_shutdown(w);
	}
	if (w->inflight > 0 && !w->shutdown_sent) {
		double idle = now - w->last_msg;
		if (idle >= opt.stall_s) {
			if (!w->stall_since) {
				w->stall_since = w->last_msg;
				w->stalls++;
				w->stall_printed = now;
				printf("  STALL[web] t=%.1fs: nothing from the network "
				       "process for %.1f s, %d in flight:\n", REL(now),
				       idle, w->inflight);
				web_print_inflight(w, 12);
			} else if (now - w->stall_printed >= 2.0) {
				w->stall_printed = now;
				printf("  STALL[web] t=%.1fs: still nothing, %.1f s\n",
				       REL(now), idle);
			}
		}
	}
	web_close_stall(w, now);
	if (!w->shutdown_sent && now - g_t0 > opt.timeout_s) {
		w->timed_out = 1;
		printf("  TIMEOUT[web] t=%.1fs: giving up with %d in flight, %d of %u "
		       "done:\n", REL(now), w->inflight, w->done, w->loads->len);
		web_print_inflight(w, 30);
		web_shutdown(w);
	}
	if (w->shutdown_sent && now - w->t_shutdown > 15.0) {
		fprintf(stderr, "apload[web]: no report from the network process "
			"15 s after SHUTDOWN\n");
		if (w->child > 0)
			kill(w->child, SIGKILL);
		g_main_loop_quit(w->loop);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static int cmp_wload_fin(gconstpointer a, gconstpointer b)
{
	const struct wload *x = *(struct wload *const *)a,
			   *y = *(struct wload *const *)b;
	return (y->t_fin > x->t_fin) - (y->t_fin < x->t_fin);
}

static void web_close_stall(struct web *w, double now)
{
	if (!w->stall_since)
		return;
	double end = w->last_msg > w->stall_since ? w->last_msg : now;
	double d = end - w->stall_since;
	if (d > w->longest)
		w->longest = d;
	if (w->last_msg > w->stall_since) {
		printf("  STALL[web] t=%.1fs: messages resumed after %.1f s\n",
		       REL(now), d);
		w->stall_since = 0;
	}
}

static void web_report(struct web *w)
{
	char b1[32];
	web_close_stall(w, now_s());
	double wall = (w->t_end ? w->t_end : now_s()) - w->t_first;
	int ok = 0, failed = 0, http_err = 0;
	for (guint i = 0; i < w->loads->len; i++) {
		struct wload *wl = w->loads->pdata[i];
		if (!wl->done)
			continue;
		if (wl->ok) {
			ok++;
			if (wl->status >= 400)
				http_err++;
		} else {
			failed++;
		}
	}
	printf("\n=== apload: web process report (end to end, as the WebProcess "
	       "sees it) ===\n");
	printf("URLs: %d in the file, %u loaded", w->n_file, w->loads->len);
	if (g_hash_table_size(w->skipped)) {
		GHashTableIter it;
		gpointer k, v;
		printf(", skipped:");
		g_hash_table_iter_init(&it, w->skipped);
		while (g_hash_table_iter_next(&it, &k, &v))
			printf(" %d %s", GPOINTER_TO_INT(v), (const char *)k);
	}
	printf("\n");
	printf("mode: %s, window %d, %d completed (%d with HTTP status >= 400), "
	       "%d failed, %d never finished%s\n",
	       opt.single ? "single process" : "web + network process", opt.window,
	       ok, http_err, failed, (int)w->loads->len - ok - failed,
	       w->timed_out ? " (TIMED OUT)" : "");
	printf("WALL TIME: %.2f s from the first LOAD to the last FINISH\n", wall);
	printf("bytes consumed: %s in %llu DATA messages (%llu inline, %llu out of "
	       "line via shared memory), %s/s\n",
	       fmt_bytes(w->bytes, b1, sizeof b1), (unsigned long long)w->data_msgs,
	       (unsigned long long)w->data_inline, (unsigned long long)w->data_ool,
	       fmt_bytes((guint64)(w->bytes / (wall > 0 ? wall : 1)), b1, sizeof b1));
	printf("net -> web delivery latency: avg %.2f ms over %llu messages "
	       "(IPC thread + main-loop dispatch)\n",
	       w->lat_n ? w->lat_sum / w->lat_n * 1000 : 0,
	       (unsigned long long)w->lat_n);
	printf("peak in flight %d; stalls seen from here: %d, longest %.1f s\n",
	       w->peak, w->stalls, w->longest);
	printf("\nend-to-end per load, ms (LOAD sent -> ...):\n");
	pct_header("measure");
	pct_row("response headers", w->ttfb);
	pct_row("FINISH received", w->e2e);

	/* The completion curve is the browser's view: a page is usable long
	 * before the last tracking pixel gives up, and one server that never
	 * answers its SYN must not hide that in the wall time. */
	GArray *fin = g_array_new(FALSE, FALSE, sizeof(double));
	GPtrArray *order = g_ptr_array_new();
	for (guint i = 0; i < w->loads->len; i++) {
		struct wload *wl = w->loads->pdata[i];
		if (!wl->done)
			continue;
		double t = wl->t_fin - w->t_first;
		g_array_append_val(fin, t);
		g_ptr_array_add(order, wl);
	}
	if (fin->len) {
		double *v = (double *)fin->data;
		qsort(v, fin->len, sizeof(double), cmp_double);
		guint last = fin->len - 1;
		printf("\ncompletion curve (seconds after the first LOAD, of %u loads "
		       "that finished): 50%% %.2f, 90%% %.2f, 95%% %.2f, 99%% %.2f, "
		       "100%% %.2f\n", fin->len, v[last * 50 / 100], v[last * 90 / 100],
		       v[last * 95 / 100], v[last * 99 / 100], v[last]);
		g_ptr_array_sort(order, cmp_wload_fin);
		printf("last 5 to finish:\n");
		for (guint i = 0; i < order->len && i < 5; i++) {
			struct wload *wl = order->pdata[i];
			char ub[72];
			printf("  %7.2f s  %3d  %6.1f s in flight  %s%s\n",
			       wl->t_fin - w->t_first, wl->status, wl->t_fin - wl->t_sent,
			       short_url(wl->url, ub, sizeof ub), wl->ok ? "" : "  FAILED");
		}
	}
	g_array_free(fin, TRUE);
	g_ptr_array_free(order, TRUE);
	fflush(stdout);
}

/* ---------------------------------------------------------------------- */

static char *find_url_file(const char *argv0)
{
	if (opt.file)
		return g_strdup(opt.file);
	const char *slash = strrchr(argv0, '/');
	if (slash) {
		char *dir = g_strndup(argv0, (gsize)(slash - argv0));
		char *p = g_build_filename(dir, URL_FILE_NAME, NULL);
		g_free(dir);
		if (g_file_test(p, G_FILE_TEST_EXISTS))
			return p;
		g_free(p);
	}
	if (g_file_test(URL_FILE_NAME, G_FILE_TEST_EXISTS))
		return g_strdup(URL_FILE_NAME);
	return g_strdup(DEFAULT_URL_FILE);
}

/* WebCore's URL parser percent-encodes what a browser tolerates and RFC 3986
 * does not -- spaces, quotes, angle brackets, braces, backslash, caret,
 * backtick, pipe, controls, non-ASCII -- before libsoup ever sees the URL;
 * g_uri_parse() would reject them.  Same here. */
static char *canonicalize_url(const char *url, int *changed)
{
	GString *s = g_string_sized_new(strlen(url) + 16);
	for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
		if (*p <= 0x20 || *p >= 0x7f || strchr("\"<>\\^`{|}", *p)) {
			g_string_append_printf(s, "%%%02X", *p);
			*changed = 1;
		} else {
			g_string_append_c(s, (char)*p);
		}
	}
	return g_string_free(s, FALSE);
}

static int read_urls(struct web *w, const char *path)
{
	char *text = NULL;
	GError *err = NULL;
	if (!g_file_get_contents(path, &text, NULL, &err)) {
		fprintf(stderr, "apload: %s: %s\n", path, err->message);
		g_error_free(err);
		return -1;
	}
	char **lines = g_strsplit(text, "\n", -1);
	g_free(text);
	for (int i = 0; lines[i]; i++) {
		char *s = g_strstrip(lines[i]);
		if (!*s || *s == '#')
			continue;
		w->n_file++;
		if (opt.limit && (int)w->loads->len >= opt.limit)
			continue;
		if (g_str_has_prefix(s, "http://") || g_str_has_prefix(s, "https://") ||
		    g_str_has_prefix(s, "ws://") || g_str_has_prefix(s, "wss://")) {
			struct wload *wl = g_new0(struct wload, 1);
			int changed = 0;
			wl->url = canonicalize_url(s, &changed);
			w->n_encoded += changed;
			wl->rc = classify(s, (int)w->loads->len);
			g_ptr_array_add(w->loads, wl);
		} else {
			const char *colon = strchr(s, ':');
			char *scheme = colon ? g_strndup(s, (gsize)(colon - s + 1))
					     : g_strdup("(no scheme)");
			int c = GPOINTER_TO_INT(g_hash_table_lookup(w->skipped, scheme));
			g_hash_table_insert(w->skipped, scheme, GINT_TO_POINTER(c + 1));
		}
	}
	g_strfreev(lines);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: apload [-f FILE] [-w N] [-n N] [-s SEC] [-T SEC] [-1] [-k] [-v] [-q]\n"
		"  -f FILE  URL list (default: " URL_FILE_NAME " beside the binary, else\n"
		"           " DEFAULT_URL_FILE ")\n"
		"  -w N     loads kept outstanding on the web side (40; 0 = all at once)\n"
		"  -n N     only the first N URLs\n"
		"  -s SEC   call it a stall after SEC without progress (2)\n"
		"  -T SEC   give up after SEC and report what was in flight (300)\n"
		"  -1       single process, no IPC (bisects the IPC layer out)\n"
		"  -k       accept any TLS certificate\n"
		"  -v       one line per finished load\n"
		"  -q       no per-second timeline\n");
	exit(2);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	g_t0 = now_s();

	int net_fd = -1;
	if (argc >= 3 && !strcmp(argv[1], "--network-process")) {
		net_fd = atoi(argv[2]);
		argv[2] = argv[0];
		argv += 2;
		argc -= 2;
	}

	int c;
	while ((c = getopt(argc, argv, "f:w:n:s:T:1kvqh")) != -1) {
		switch (c) {
		case 'f': opt.file = optarg; break;
		case 'w': opt.window = atoi(optarg); break;
		case 'n': opt.limit = atoi(optarg); break;
		case 's': opt.stall_s = atof(optarg); break;
		case 'T': opt.timeout_s = atof(optarg); break;
		case '1': opt.single = 1; break;
		case 'k': opt.insecure = 1; break;
		case 'v': opt.verbose = 1; break;
		case 'q': opt.timeline = 0; break;
		default: usage();
		}
	}
	if (opt.stall_s <= 0)
		opt.stall_s = 0.25;

	if (net_fd >= 0) {
		signal(SIGINT, SIG_IGN);
		return net_main(net_fd);
	}
	signal(SIGINT, on_sigint);

	struct web *w = g_new0(struct web, 1);
	w->loads = g_ptr_array_new();
	w->skipped = g_hash_table_new(g_str_hash, g_str_equal);
	w->e2e = g_array_new(FALSE, FALSE, sizeof(double));
	w->ttfb = g_array_new(FALSE, FALSE, sizeof(double));
	w->link.on_receive = web_handle;
	w->link.user = w;

	char *path = find_url_file(argv[0]);
	if (read_urls(w, path) < 0)
		return 1;
	if (!w->loads->len) {
		fprintf(stderr, "apload: %s: no http(s) URLs\n", path);
		return 1;
	}
	int by_class[RC_COUNT] = { 0 };
	for (guint i = 0; i < w->loads->len; i++)
		by_class[((struct wload *)w->loads->pdata[i])->rc]++;

	printf("apload: %u URLs from %s (%d lines, %d percent-encoded); window %d; "
	       "stall after %.1f s; give up after %.0f s\n", w->loads->len, path,
	       w->n_file, w->n_encoded, opt.window, opt.stall_s, opt.timeout_s);
	printf("  classes:");
	for (int i = 0; i < RC_COUNT; i++)
		if (by_class[i])
			printf(" %s %d", rc_info[i].name, by_class[i]);
	printf("\n  session: max-conns %d, per host %d, idle-timeout %d s, read size "
	       "%d, IPC message max %d (larger bodies via shared memory)\n",
	       WK_MAX_CONNS, WK_MAX_CONNS_PER_HOST, WK_IDLE_TIMEOUT_S, WK_READ_SIZE,
	       IPC_MESSAGE_MAX);
	printf("  libsoup %u.%u.%u, glib %u.%u.%u\n", soup_get_major_version(),
	       soup_get_minor_version(), soup_get_micro_version(), glib_major_version,
	       glib_minor_version, glib_micro_version);
	g_free(path);

	struct net *local_net = NULL;
	if (opt.single) {
		local_net = g_new0(struct net, 1);
		net_init(local_net);
		local_net->link.local_peer = &w->link;
		w->link.local_peer = &local_net->link;
		printf("  single process: libsoup in this main loop, no IPC\n");
	} else {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) {
			perror("apload: socketpair(SOCK_SEQPACKET)");
			return 1;
		}
		fcntl(sv[0], F_SETFD, FD_CLOEXEC);
		char fdstr[16];
		snprintf(fdstr, sizeof fdstr, "%d", sv[1]);
		GPtrArray *cargv = g_ptr_array_new();
		g_ptr_array_add(cargv, argv[0]);
		g_ptr_array_add(cargv, "--network-process");
		g_ptr_array_add(cargv, fdstr);
		for (int i = 1; i < argc; i++)
			g_ptr_array_add(cargv, argv[i]);
		g_ptr_array_add(cargv, NULL);
		/* ProcessLauncherGLib.cpp spawns with GSubprocess, which uses
		 * posix_spawn when it can; this does the same directly. */
		int rc = posix_spawnp(&w->child, argv[0], NULL, NULL,
				      (char *const *)cargv->pdata, environ);
		if (rc != 0) {
			fprintf(stderr, "apload: cannot spawn the network process "
				"(%s): %s\n", argv[0], strerror(rc));
			return 1;
		}
		close(sv[1]);
		g_ptr_array_free(cargv, TRUE);
		ipc_start(sv[0], &w->link);
		printf("  network process: pid %d over a SOCK_SEQPACKET socketpair\n",
		       (int)w->child);
	}

	w->loop = g_main_loop_new(NULL, FALSE);
	w->last_msg = now_s();
	g_timeout_add(250, web_tick, w);
	web_pump(w);
	g_main_loop_run(w->loop);

	if (w->link.ipc)
		ipc_stop(w->link.ipc);
	if (w->child > 0) {
		int status = 0;
		waitpid(w->child, &status, 0);
		if (WIFSIGNALED(status))
			printf("  [web] network process killed by signal %d\n",
			       WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			printf("  [web] network process exited with %d\n",
			       WEXITSTATUS(status));
	}
	web_report(w);
	return w->timed_out || w->child_gone;
}
