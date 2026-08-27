/*
 * Does GIO's TLS (glib-networking + gnutls) work on this system?
 *
 * curl fetches https://example.com and https://www.google.com fine, and
 * MiniBrowser fetches http:// fine but hangs on https:// -- including tiny
 * pages, and with SOUP_FORCE_HTTP1=1 so HTTP/2 is not involved.  curl uses
 * OpenSSL; WebKit reaches TLS through GIO, which loads glib-networking, which
 * uses gnutls.  Those are entirely separate stacks, and only the second one
 * fails, so the kernel's TCP is not the problem.
 *
 * This exercises exactly WebKit's path -- GSocketClient with TLS, which is
 * what SoupConnection uses -- and nothing else.  Every step announces itself
 * BEFORE it runs, so the last line printed names the step that hung.
 *
 * A timeout is set, because the symptom is a hang: without one this program
 * would reproduce the bug by never finishing, which tells nobody anything.
 */
#include <gio/gio.h>
#include <stdio.h>

#define HOST "example.com"
#define PORT 443

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("1. is there a TLS backend at all?\n");
    GTlsBackend *backend = g_tls_backend_get_default();
    if (!backend) {
        printf("   NO BACKEND -- glib-networking did not load.\n");
        printf("   (that would give errors, not a hang, but it must be ruled out)\n");
        return 1;
    }
    printf("   backend: %s\n", G_OBJECT_TYPE_NAME(backend));
    printf("   supports TLS: %s\n", g_tls_backend_supports_tls(backend) ? "yes" : "NO");
    printf("   default database: %s\n\n",
           g_tls_backend_get_default_database(backend) ? "present" : "MISSING (no CA store)");

    printf("2. plain TCP to " HOST ":80 (known good, as a control) ... ");
    {
        GSocketClient *c = g_socket_client_new();
        g_socket_client_set_timeout(c, 15);
        GError *e = NULL;
        GSocketConnection *conn = g_socket_client_connect_to_host(c, HOST, 80, NULL, &e);
        printf("%s\n", conn ? "ok" : e->message);
        if (conn) g_object_unref(conn);
        g_clear_error(&e);
        g_object_unref(c);
    }

    printf("3. TLS handshake to " HOST ":%d via GIO ...\n", PORT);
    printf("   (this is the step MiniBrowser hangs on; 20s timeout)\n   ");
    {
        GSocketClient *c = g_socket_client_new();
        g_socket_client_set_tls(c, TRUE);
        g_socket_client_set_timeout(c, 20);
        GError *e = NULL;
        GSocketConnection *conn = g_socket_client_connect_to_host(c, HOST, PORT, NULL, &e);
        if (!conn) {
            printf("FAILED: %s\n", e ? e->message : "(no error set)");
            printf("\n   ^^ GIO TLS is the bug.  curl (OpenSSL) succeeds on the same\n"
                   "      host, so TCP is fine and this is glib-networking/gnutls.\n");
            g_clear_error(&e);
            g_object_unref(c);
            return 1;
        }
        printf("handshake ok\n");

        printf("4. sending a request and reading the reply ... ");
        GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        const char *req = "GET / HTTP/1.1\r\nHost: " HOST "\r\nConnection: close\r\n\r\n";
        if (!g_output_stream_write_all(os, req, strlen(req), NULL, NULL, &e)) {
            printf("WRITE FAILED: %s\n", e ? e->message : "?");
            return 1;
        }
        char buf[512];
        gssize n = g_input_stream_read(is, buf, sizeof(buf) - 1, NULL, &e);
        if (n <= 0) {
            printf("READ FAILED: %s\n", e ? e->message : "connection closed");
            return 1;
        }
        buf[n] = '\0';
        char *eol = strchr(buf, '\r');
        if (eol) *eol = '\0';
        printf("ok\n   server said: %s\n", buf);
        g_object_unref(conn);
        g_object_unref(c);
    }

    /* ---------------------------------------------------------------
     * The step that matters, now that the synchronous path works.
     *
     * Everything above used the BLOCKING API.  libsoup does not: it drives
     * the connection asynchronously from a GMainContext, so readiness comes
     * from poll() through a GSource rather than from a blocking read.
     *
     * That distinction lines up exactly with the symptom.  A plain HTTP
     * request writes a few hundred bytes and then only ever waits to READ.
     * A TLS handshake alternates -- write ClientHello, read ServerHello,
     * write key exchange, and so on -- so it waits to WRITE as well.  A
     * poll() that never reports a connected TCP socket writable would hang
     * TLS and leave HTTP working, which is what MiniBrowser does.
     * --------------------------------------------------------------- */
    printf("\n5. the same TLS connection ASYNCHRONOUSLY, the way libsoup does it\n");
    printf("   (GMainContext + poll readiness, not blocking reads; 20s limit)\n   ");
    {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        GSocketClient *c = g_socket_client_new();
        g_socket_client_set_tls(c, TRUE);

        struct async_state { GMainLoop *loop; gboolean done; gboolean ok; char *err; } st;
        st.loop = loop; st.done = FALSE; st.ok = FALSE; st.err = NULL;

        void on_done(GObject *src, GAsyncResult *res, gpointer data) {
            struct async_state *s = data;
            GError *e = NULL;
            GSocketConnection *conn =
                g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, &e);
            s->done = TRUE;
            s->ok = (conn != NULL);
            if (!conn) s->err = g_strdup(e ? e->message : "(no error set)");
            if (conn) g_object_unref(conn);
            g_clear_error(&e);
            g_main_loop_quit(s->loop);
        }
        gboolean on_timeout(gpointer data) {
            struct async_state *s = data;
            g_main_loop_quit(s->loop);
            return G_SOURCE_REMOVE;
        }

        g_socket_client_connect_to_host_async(c, HOST, PORT, NULL, on_done, &st);
        g_timeout_add_seconds(20, on_timeout, &st);
        g_main_loop_run(loop);

        if (!st.done) {
            printf("TIMED OUT -- never completed\n");
            printf("\n   ^^ This is the bug, and it is NOT gnutls: the same handshake\n"
                   "      succeeded synchronously in step 3.  Only the async path hangs,\n"
                   "      so readiness reporting is at fault -- most likely poll() not\n"
                   "      signalling a connected TCP socket writable.  Plain HTTP only\n"
                   "      ever waits to read, which is why it works.\n");
            return 1;
        }
        printf("%s\n", st.ok ? "ok" : st.err);
        if (!st.ok) {
            printf("\n   async TLS FAILED (did not hang).  The error above is the cause.\n");
            return 1;
        }
    }

    printf("\nBoth sync and async GIO TLS work.  The hang is in WebKit's network\n"
           "process specifically, not in TLS or in poll() readiness.\n");
    return 0;
}
