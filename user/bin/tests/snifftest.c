/*
 * Reproduce WebKit's response-body read path outside WebKit.
 *
 * MiniBrowser gets exactly 512 bytes of a 558-byte page over https and then
 * stops, with brotli and with gzip alike -- the same cut point for two
 * different decoders, so the cut is not in a decoder.  512 is
 * BUFFER_SIZE in libsoup's content-sniffer stream
 * (libsoup/content-sniffer/soup-content-sniffer-stream.c), which reads the
 * first 512 bytes of a body to guess its MIME type, hands them to the caller
 * out of its own buffer, and only then reads from the stream below it.
 *
 * WebKit puts that stream in the chain and souptest did not:
 *
 *     SoupNetworkSession.cpp:130  soup_session_add_feature_by_type(
 *                                     session, SOUP_TYPE_CONTENT_SNIFFER)
 *
 * and libsoup adds only SOUP_TYPE_CONTENT_DECODER of its own accord
 * (soup-session.c:310).  souptest also used soup_session_send_and_read(),
 * which reads the body its own way, whereas WebKit drives the stream by hand:
 *
 *     NetworkDataTaskSoup::read()  g_input_stream_read_async(.., 8192, ..,
 *                                     RunLoopSourcePriority::AsyncIONetwork)
 *
 * So every earlier test passed while the browser failed, because none of them
 * assembled this chain.  This one does, and varies one element at a time:
 * the sniffer, the read priority, async against sync, https against http.
 * Whichever variants stop at 512 name the culprit between them.
 *
 * Every chunk is printed as it arrives, so a run that stops mid-body still
 * shows how far it got and whether the final read returned 0 (a premature
 * end-of-body) or never came back at all (a lost wakeup).
 */
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

#define READ_SIZE 8192   /* NetworkDataTaskSoup gDefaultReadBufferSize */
#define WK_PRIO   100    /* RunLoopSourcePriority::AsyncIONetwork */
#define TIMEOUT_S 20

typedef struct {
	const char *name;
	const char *url;
	gboolean sniffer;
	gboolean async;
	int prio;
	gboolean cancellable;   /* WebKit passes m_cancellable; NULL here before */
	int delay_ms;           /* stand-in for WebKit's IPC round trip */

	GMainLoop *loop;
	GInputStream *stream;
	GCancellable *cancel;
	gsize total;
	int chunks;
	int reads_issued;
	gboolean finished;   /* saw a clean 0-byte end */
	gboolean timed_out;
	char *failure;
	char buf[READ_SIZE];
} test_t;

static void finish(test_t *t)
{
	if (t->loop && g_main_loop_is_running(t->loop))
		g_main_loop_quit(t->loop);
}

static void fail(test_t *t, const char *stage, GError *e)
{
	g_free(t->failure);
	t->failure = g_strdup_printf("%s: %s", stage,
				     e ? e->message : "(no error set)");
	finish(t);
}

static void issue_read(test_t *t);
static void issue_read_maybe_delayed(test_t *t);

static void on_read(GObject *src, GAsyncResult *res, gpointer data)
{
	test_t *t = data;
	GError *e = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &e);

	if (n < 0) {
		printf("    read -> ERROR %s (domain %s code %d)\n",
		       e ? e->message : "?",
		       e ? g_quark_to_string(e->domain) : "?",
		       e ? e->code : 0);
		fail(t, "read", e);
		g_clear_error(&e);
		return;
	}
	printf("    read -> %zd bytes\n", (ssize_t)n);
	if (n == 0) {
		t->finished = TRUE;
		finish(t);
		return;
	}
	t->total += (gsize)n;
	t->chunks++;
	issue_read_maybe_delayed(t);
}

static void issue_read(test_t *t)
{
	t->reads_issued++;
	printf("    read_async(%d) #%d\n", READ_SIZE, t->reads_issued);
	g_input_stream_read_async(t->stream, t->buf, READ_SIZE, t->prio,
				  t->cancel, on_read, t);
}

/* WebKit does not re-read the instant a chunk lands: didRead() first ships the
 * data to the web process over IPC, and only then calls read().  That gap lets
 * the TLS layer finish draining the socket, so the next read must be satisfied
 * from glib-networking's own buffer with nothing left for poll() to report.
 * Reads issued back to back never reach that state, which is why a tight loop
 * can pass while the browser stalls. */
static gboolean issue_read_cb(gpointer data)
{
	issue_read((test_t *)data);
	return G_SOURCE_REMOVE;
}

static void issue_read_maybe_delayed(test_t *t)
{
	if (t->delay_ms > 0)
		g_timeout_add(t->delay_ms, issue_read_cb, t);
	else
		issue_read(t);
}

static void on_sent(GObject *src, GAsyncResult *res, gpointer data)
{
	test_t *t = data;
	GError *e = NULL;

	t->stream = soup_session_send_finish(SOUP_SESSION(src), res, &e);
	if (!t->stream) {
		fail(t, "send", e);
		g_clear_error(&e);
		return;
	}
	issue_read(t);
}

static gboolean on_timeout(gpointer data)
{
	test_t *t = data;
	t->timed_out = TRUE;
	finish(t);
	return G_SOURCE_REMOVE;
}

/* A read that never comes back is the whole point of this test, so the sync
 * variant leans on the session timeout rather than a main-loop source: a
 * blocking read gives the loop no chance to run. */
static void run_sync(test_t *t, SoupSession *s, SoupMessage *msg)
{
	GError *e = NULL;

	t->stream = soup_session_send(s, msg, NULL, &e);
	if (!t->stream) {
		fail(t, "send", e);
		g_clear_error(&e);
		return;
	}
	for (;;) {
		gssize n = g_input_stream_read(t->stream, t->buf, READ_SIZE,
					       NULL, &e);
		if (n < 0) {
			printf("    read -> ERROR %s\n",
			       e ? e->message : "?");
			fail(t, "read", e);
			g_clear_error(&e);
			return;
		}
		printf("    read -> %zd bytes\n", (ssize_t)n);
		if (n == 0) {
			t->finished = TRUE;
			return;
		}
		t->total += (gsize)n;
		t->chunks++;
	}
}

static void run(test_t *t)
{
	SoupSession *s = soup_session_new();
	SoupMessage *msg;

	printf("\n== %s\n   %s | sniffer=%s | %s | priority=%d"
	       " | cancellable=%s | delay=%dms\n",
	       t->name, t->url, t->sniffer ? "yes" : "no",
	       t->async ? "async" : "sync", t->prio,
	       t->cancellable ? "yes" : "no", t->delay_ms);

	/* What WebKit's SoupNetworkSession does, and what souptest never did. */
	if (t->sniffer)
		soup_session_add_feature_by_type(s, SOUP_TYPE_CONTENT_SNIFFER);
	soup_session_set_timeout(s, TIMEOUT_S);

	if (t->cancellable)
		t->cancel = g_cancellable_new();

	msg = soup_message_new("GET", t->url);
	if (!msg) {
		printf("   FAILED: bad url\n");
		g_object_unref(s);
		return;
	}

	if (t->async) {
		guint id;
		t->loop = g_main_loop_new(NULL, FALSE);
		id = g_timeout_add_seconds(TIMEOUT_S, on_timeout, t);
		soup_session_send_async(s, msg, t->prio, NULL, on_sent, t);
		g_main_loop_run(t->loop);
		g_source_remove(id);
		g_main_loop_unref(t->loop);
		t->loop = NULL;
	} else {
		run_sync(t, s, msg);
	}

	printf("   -> %zu bytes in %d chunk(s), %d read(s) issued",
	       (size_t)t->total, t->chunks, t->reads_issued);
	if (t->timed_out)
		printf("  [TIMED OUT -- last read never completed]");
	else if (t->failure)
		printf("  [%s]", t->failure);
	else if (!t->finished)
		printf("  [stopped without end-of-body]");
	else if (t->total == 512)
		printf("  [ENDED AT EXACTLY 512 -- the sniffer buffer]");
	else
		printf("  [complete]");
	printf("\n");

	g_clear_object(&t->cancel);
	g_clear_object(&t->stream);
	g_object_unref(msg);
	g_object_unref(s);
	g_free(t->failure);
	t->failure = NULL;
}

int main(int argc, char **argv)
{
	const char *https = argc > 1 ? argv[1] : "https://example.com/";
	const char *http = argc > 2 ? argv[2] : "http://example.com/";

	/* One element differs between each variant and the one above it. */
	test_t tests[] = {
		{ "A  sniffer, async, back to back", https, TRUE,  TRUE,  WK_PRIO,            FALSE, 0  },
		{ "B  without the sniffer",          https, FALSE, TRUE,  WK_PRIO,            FALSE, 0  },
		{ "C  sniffer, default prio",        https, TRUE,  TRUE,  G_PRIORITY_DEFAULT, FALSE, 0  },
		{ "D  sniffer, sync reads",          https, TRUE,  FALSE, 0,                  FALSE, 0  },
		{ "E  sniffer, async, http",         http,  TRUE,  TRUE,  WK_PRIO,            FALSE, 0  },
		/* The two things WebKit does that none of the above did. */
		{ "F  + GCancellable",               https, TRUE,  TRUE,  WK_PRIO,            TRUE,  0  },
		{ "G  + 20ms gap between reads",     https, TRUE,  TRUE,  WK_PRIO,            FALSE, 20 },
		{ "H  + cancellable AND gap",        https, TRUE,  TRUE,  WK_PRIO,            TRUE,  20 },
		{ "I  no sniffer, cancellable+gap",  https, FALSE, TRUE,  WK_PRIO,            TRUE,  20 },
	};
	gsize i;

	printf("snifftest: %s\n", https);
	for (i = 0; i < G_N_ELEMENTS(tests); i++)
		run(&tests[i]);

	printf("\nReading it:\n"
	       "  A-E all complete            -> a tight read loop hides the bug\n"
	       "  G or H stalls, A does not   -> THE BUG: once the TLS layer has\n"
	       "                                 drained the socket, a read waiting\n"
	       "                                 on poll() is never woken for data\n"
	       "                                 already buffered above it\n"
	       "  F stalls, A does not        -> the GCancellable's own source\n"
	       "  I stalls too                -> not the sniffer at all, any\n"
	       "                                 second read after a gap\n");
	return 0;
}
