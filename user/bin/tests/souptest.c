/*
 * Does libsoup fetch https:// on this system?
 *
 * This is the layer WebKit actually uses.  Earlier tests drove raw GIO --
 * GSocketClient with TLS, sync and async -- and every one of them passed,
 * while MiniBrowser still hangs on https://example.com.  That means the fault
 * is not in gnutls, not in glib-networking, and not in poll() readiness: it is
 * somewhere in libsoup, or in how libsoup drives the layers below it.  Raw GIO
 * was a reconstruction of libsoup's behaviour, and reconstructions test the
 * reconstruction.
 *
 * So this uses SoupSession directly, which is exactly what WebKit's network
 * process does:
 *
 *     WebKit -> SoupSession -> SoupConnection -> GTlsConnection -> gnutls
 *
 * http:// first as a control, because that path is known to work in
 * MiniBrowser.  Then https:// synchronously, then https:// asynchronously --
 * async is how WebKit issues every request.  Timeouts throughout, because the
 * symptom is a hang and a test that hangs reports nothing.
 */
#include <libsoup/soup.h>
#include <stdio.h>

#define TIMEOUT_S 20

static void report(const char *what, GBytes *body, GError *e, SoupMessage *msg)
{
    if (!body) {
        printf("FAILED: %s\n", e ? e->message : "(no error set)");
        return;
    }
    gsize n = g_bytes_get_size(body);
    printf("ok -- HTTP %u, %zu bytes\n",
           msg ? soup_message_get_status(msg) : 0, (size_t)n);
    g_bytes_unref(body);
    (void)what;
}

static SoupSession *new_session(void)
{
    SoupSession *s = soup_session_new();
    soup_session_set_timeout(s, TIMEOUT_S);
    soup_session_set_idle_timeout(s, TIMEOUT_S);
    return s;
}

struct async_state { GMainLoop *loop; gboolean done; SoupMessage *msg; };

static void on_read(GObject *src, GAsyncResult *res, gpointer data)
{
    struct async_state *st = data;
    GError *e = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &e);
    st->done = TRUE;
    report("async", body, e, st->msg);
    g_clear_error(&e);
    g_main_loop_quit(st->loop);
}

static gboolean on_timeout(gpointer data)
{
    struct async_state *st = data;
    g_main_loop_quit(st->loop);
    return G_SOURCE_REMOVE;
}

static int fetch_sync(const char *url)
{
    printf("   %-34s ", url);
    SoupSession *s = new_session();
    SoupMessage *msg = soup_message_new("GET", url);
    GError *e = NULL;
    GBytes *body = soup_session_send_and_read(s, msg, NULL, &e);
    int ok = body != NULL;
    report("sync", body, e, msg);
    g_clear_error(&e);
    g_object_unref(msg);
    g_object_unref(s);
    return ok;
}

static int fetch_async(const char *url)
{
    printf("   %-34s ", url);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    struct async_state st = { loop, FALSE, NULL };
    SoupSession *s = new_session();
    SoupMessage *msg = soup_message_new("GET", url);
    st.msg = msg;

    soup_session_send_and_read_async(s, msg, G_PRIORITY_DEFAULT, NULL, on_read, &st);
    g_timeout_add_seconds(TIMEOUT_S + 5, on_timeout, &st);
    g_main_loop_run(loop);

    if (!st.done)
        printf("TIMED OUT -- never completed\n");
    g_object_unref(msg);
    g_object_unref(s);
    g_main_loop_unref(loop);
    return st.done;
}

struct thread_state { const char *url; gboolean done; gboolean ok; };

static gpointer thread_fn(gpointer data)
{
    struct thread_state *ts = data;

    /* A private context, made thread-default -- what WebKit's WorkQueue does.
     * Anything inside GIO or libsoup that attaches a source to "the" context
     * gets this one, not the global default, and nothing on the main thread
     * is iterating it. */
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    struct async_state st = { loop, FALSE, NULL };
    SoupSession *s = new_session();
    SoupMessage *msg = soup_message_new("GET", ts->url);
    st.msg = msg;

    soup_session_send_and_read_async(s, msg, G_PRIORITY_DEFAULT, NULL, on_read, &st);
    g_timeout_add_seconds(TIMEOUT_S + 5, on_timeout, &st);
    g_main_loop_run(loop);

    ts->done = st.done;
    ts->ok = st.done;
    if (!st.done)
        printf("TIMED OUT -- never completed\n");

    g_object_unref(msg);
    g_object_unref(s);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return NULL;
}

static int fetch_on_thread(const char *url)
{
    printf("   %-34s ", url);
    struct thread_state ts = { url, FALSE, FALSE };
    GThread *t = g_thread_new("net", thread_fn, &ts);
    g_thread_join(t);
    return ts.ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("libsoup %u.%u.%u -- the stack WebKit's network process uses\n\n",
           soup_get_major_version(), soup_get_minor_version(), soup_get_micro_version());

    printf("1. synchronous\n");
    fetch_sync("http://example.com/");
    int s_https = fetch_sync("https://example.com/");

    printf("\n2. asynchronous (how WebKit issues every request)\n");
    fetch_async("http://example.com/");
    int a_https = fetch_async("https://example.com/");

    printf("\n3. asynchronous ON A WORKER THREAD with its own GMainContext\n");
    printf("   (this is WebKit's actual shape: its network process drives libsoup\n");
    printf("    from a WorkQueue thread, not from the main loop)\n");
    int t_https = fetch_on_thread("https://example.com/");
    fetch_on_thread("http://example.com/");

    printf("\n");
    if (!t_https) {
        printf("libsoup works on the main thread and hangs on a worker thread with a\n"
               "thread-default GMainContext.  That is exactly how WebKit's network\n"
               "process runs it, and it is why every earlier test passed.\n");
        return 1;
    }
    if (!a_https || !s_https) {
        printf("libsoup itself cannot do https here.  Raw GIO TLS passed every\n"
               "test, so the fault is in libsoup or in how it drives GIO --\n"
               "not in gnutls, glib-networking, or the kernel's sockets.\n");
        return 1;
    }
    printf("libsoup fetches https fine both ways.  Then the hang is above it,\n"
           "in WebKit's network process rather than in the HTTP stack.\n");
    return 0;
}
