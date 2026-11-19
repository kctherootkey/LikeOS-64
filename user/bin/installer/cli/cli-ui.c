/* likeos-install -- the text terminal.  See cli-ui.h.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "cli-ui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

volatile sig_atomic_t ui_interrupted;
int ui_error_shown;

static int g_in_tty, g_out_tty, g_color;
static int g_have_saved;
static struct termios g_saved;

static void ui_on_sigint(int sig)
{
	(void)sig;
	ui_interrupted++;
}

/* The terminal as it was found; only async-signal-safe calls (the fatal
 * signal handler uses it). */
static void ui_restore_tty(void)
{
	if (g_have_saved)
		tcsetattr(0, TCSANOW, &g_saved);
}

static void ui_at_exit(void)
{
	ui_restore_tty();
}

/* SIGTERM, SIGHUP, SIGQUIT: the terminal back as it was found, then the
 * signal's own action (the program ends by it, as without the handler). */
static void ui_on_fatal(int sig)
{
	int e = errno;

	ui_restore_tty();
	signal(sig, SIG_DFL);
	raise(sig);
	errno = e;
}

void ui_init(int claim_tty)
{
	struct sigaction sa;
	const char *term = getenv("TERM");

	g_in_tty = isatty(0);
	g_out_tty = isatty(1);
	g_color = g_out_tty && !getenv("NO_COLOR") &&
		  !(term && (!strcmp(term, "dumb") || !term[0]));
	if (g_in_tty && tcgetattr(0, &g_saved) == 0)
		g_have_saved = 1;

	/* Started by init on the console: no controlling terminal, so
	 * Ctrl-C would reach nobody.  Make the terminal ours (a new session
	 * first; that fails only for a process group leader, which already
	 * has its terminal through its session). */
	if (claim_tty && g_in_tty) {
		int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);

		if (fd >= 0) {
			close(fd);
		} else {
			if (getpid() != getpgrp())
				setsid();
#ifdef TIOCSCTTY
			ioctl(0, TIOCSCTTY, 0);
#endif
			tcsetpgrp(0, getpgrp());
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = ui_on_sigint;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: a prompt has to wake up */
	sigaction(SIGINT, &sa, NULL);
	/* A broken pipe on the output is an output error, not a death. */
	signal(SIGPIPE, SIG_IGN);

	/* However the program ends -- returning, exit(), or killed by a
	 * signal that can be caught -- the terminal is left as it was found
	 * (an installation turns off echo and line mode).  A signal that was
	 * ignored when the program started stays ignored. */
	if (g_have_saved) {
		static const int fatal[] = { SIGTERM, SIGHUP, SIGQUIT };

		atexit(ui_at_exit);
		for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
			struct sigaction old;

			if (sigaction(fatal[i], NULL, &old) != 0 ||
			    old.sa_handler == SIG_IGN)
				continue;
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = ui_on_fatal;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = 0;
			sigaction(fatal[i], &sa, NULL);
		}
	}
}

int ui_reset_tty(void)
{
	struct termios t;

	if (tcgetattr(0, &t) != 0)
		return -1;
	t.c_iflag |= ICRNL;
	t.c_oflag |= OPOST | ONLCR;
	t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	return tcsetattr(0, TCSANOW, &t);
}

void ui_restore(void)
{
	ui_restore_tty();
	fflush(stdout);
}

int ui_is_tty(void)
{
	return g_in_tty && g_out_tty;
}

int ui_out_tty(void)
{
	return g_out_tty;
}

int ui_cols(void)
{
	struct winsize ws;
	const char *c;

	if (g_out_tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20)
		return ws.ws_col;
	c = getenv("COLUMNS");
	if (c && atoi(c) >= 20)
		return atoi(c);
	return 80;
}

long long ui_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return (long long)time(NULL) * 1000;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- text ------------------------------------------------------------ */

const char *ui_bold(void)
{
	return g_color ? "\033[1m" : "";
}

const char *ui_dim(void)
{
	return g_color ? "\033[2m" : "";
}

const char *ui_reset(void)
{
	return g_color ? "\033[0m" : "";
}

int ui_width(const char *s)
{
	int w = 0;

	for (; s && *s; s++)
		if (((unsigned char)*s & 0xC0) != 0x80)
			w++;
	return w;
}

void ui_fit(char *out, size_t len, const char *s, int cols, int pad)
{
	size_t o = 0;
	int w = 0, total = ui_width(s);
	int room = total > cols ? cols - 3 : cols;

	if (!len)
		return;
	if (room < 0)
		room = 0;
	for (const char *p = s; *p && o + 1 < len; p++) {
		if (((unsigned char)*p & 0xC0) != 0x80) {
			if (w == room)
				break;
			w++;
		}
		out[o++] = *p;
	}
	if (total > cols)
		for (int i = 0; i < 3 && w < cols && o + 1 < len; i++, w++)
			out[o++] = '.';
	while (pad && w < cols && o + 1 < len) {
		out[o++] = ' ';
		w++;
	}
	out[o] = '\0';
}

void ui_clear(void)
{
	if (ui_is_tty())
		fputs("\033[H\033[2J", stdout);
	else
		fputc('\n', stdout);
}

void ui_header(const char *title, int step, int nsteps)
{
	int cols = ui_cols();
	char rule[512];
	int n = cols - 1 < (int)sizeof(rule) - 1 ? cols - 1 : (int)sizeof(rule) - 1;

	ui_clear();
	memset(rule, '=', (size_t)n);
	rule[n] = '\0';
	if (step > 0)
		printf("%sLikeOS Setup%s  --  step %d of %d\n", ui_bold(),
		       ui_reset(), step, nsteps);
	else
		printf("%sLikeOS Setup%s\n", ui_bold(), ui_reset());
	printf("%s\n", rule);
	printf("%s%s%s\n\n", ui_bold(), title, ui_reset());
	fflush(stdout);
}

/* Print `text' wrapped at the terminal width with `indent' blanks. */
static void ui_wrap(FILE *f, const char *text, int indent)
{
	int cols = ui_cols() - 1 - indent, col = 0;
	const char *p = text;

	if (cols < 20)
		cols = 20;
	while (*p) {
		const char *w = p;
		int wl;

		if (*p == '\n') {
			fputc('\n', f);
			col = 0;
			p++;
			continue;
		}
		if (*p == ' ') {
			p++;
			continue;
		}
		while (*w && *w != ' ' && *w != '\n')
			w++;
		{
			char word[512];
			size_t n = (size_t)(w - p);

			if (n >= sizeof(word))
				n = sizeof(word) - 1;
			memcpy(word, p, n);
			word[n] = '\0';
			wl = ui_width(word);
			if (col && col + 1 + wl > cols) {
				fputc('\n', f);
				col = 0;
			}
			if (col == 0)
				fprintf(f, "%*s", indent, "");
			else {
				fputc(' ', f);
				col++;
			}
			fputs(word, f);
			col += wl;
		}
		p = w;
	}
	if (col)
		fputc('\n', f);
}

static void ui_vmsg(FILE *f, const char *pre, const char *color,
		    const char *fmt, va_list ap)
{
	char buf[4096], line[4200];

	vsnprintf(buf, sizeof(buf), fmt, ap);
	fflush(stdout);
	if (pre) {
		snprintf(line, sizeof(line), "%s%s", pre, buf);
		if (g_color && color && f == stdout)
			fputs(color, f);
		ui_wrap(f, line, 0);
		if (g_color && color && f == stdout)
			fputs("\033[0m", f);
	} else {
		ui_wrap(f, buf, 0);
	}
	fflush(f);
}

void ui_para(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ui_vmsg(stdout, NULL, NULL, fmt, ap);
	va_end(ap);
}

void ui_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ui_vmsg(stdout, "", NULL, fmt, ap);
	va_end(ap);
}

void ui_warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ui_vmsg(stdout, "Note: ", "\033[1;33m", fmt, ap);
	va_end(ap);
}

void ui_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	ui_vmsg(stdout, "Error: ", "\033[1;31m", fmt, ap);
	va_end(ap);
	ui_error_shown = 1;
}

void ui_blank(void)
{
	fputc('\n', stdout);
	fflush(stdout);
}

/* ---- input ----------------------------------------------------------- */

/* One line from standard input, without its line end.  0, or UI_CANCEL at
 * the end of the input / on Ctrl-C. */
static int ui_read_raw(char *buf, size_t len)
{
	size_t n = 0;
	int overflow = 0;

	for (;;) {
		char c;
		ssize_t r;

		if (ui_interrupted)
			return UI_CANCEL;
		r = read(0, &c, 1);
		if (r < 0) {
			if (errno == EINTR) {
				if (ui_interrupted)
					return UI_CANCEL;
				continue;
			}
			return UI_CANCEL;
		}
		if (r == 0) {
			if (n == 0)
				return UI_CANCEL; /* end of the input */
			break;
		}
		if (c == '\n')
			break;
		if (c == '\r')
			continue;
		if (n + 1 < len)
			buf[n++] = c;
		else
			overflow = 1;
	}
	buf[n] = '\0';
	if (overflow && !g_in_tty)
		fprintf(stderr, "likeos-install: an input line was too long and "
			"was cut\n");
	return UI_OK;
}

static char *ui_trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	return s;
}

static int ui_is_back(const char *s)
{
	return !strcmp(s, "<") || !strcasecmp(s, "back");
}

int ui_line(const char *prompt, const char *def, char *buf, size_t len)
{
	char in[1024];
	char *t;
	int rc;

	if (def && def[0])
		printf("%s [%s]: ", prompt, def);
	else
		printf("%s: ", prompt);
	fflush(stdout);
	rc = ui_read_raw(in, sizeof(in));
	if (!g_in_tty && rc == UI_OK)
		printf("%s\n", in); /* a transcript reads like a session */
	if (rc != UI_OK) {
		fputc('\n', stdout);
		return rc;
	}
	t = ui_trim(in);
	if (ui_is_back(t))
		return UI_BACK;
	if (!t[0] && def)
		t = (char *)def;
	snprintf(buf, len, "%s", t);
	memset(in, 0, sizeof(in));
	return UI_OK;
}

int ui_secret(const char *prompt, char *buf, size_t len)
{
	struct termios t, quiet;
	int have = 0, rc;
	char in[1024];

	printf("%s: ", prompt);
	fflush(stdout);
	if (g_in_tty && tcgetattr(0, &t) == 0) {
		quiet = t;
		quiet.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK);
		quiet.c_lflag |= ICANON;
		if (tcsetattr(0, TCSAFLUSH, &quiet) == 0)
			have = 1;
	}
	rc = ui_read_raw(in, sizeof(in));
	if (have)
		tcsetattr(0, TCSAFLUSH, &t);
	fputc('\n', stdout);
	fflush(stdout);
	if (rc != UI_OK) {
		memset(in, 0, sizeof(in));
		return rc;
	}
	if (strlen(in) >= len) {
		memset(in, 0, sizeof(in));
		buf[0] = '\0';
		return UI_OK; /* too long: an empty answer the caller rejects */
	}
	snprintf(buf, len, "%s", in);
	memset(in, 0, sizeof(in));
	return UI_OK;
}

int ui_yes_no(const char *prompt, int def, int *answer)
{
	char buf[64];
	char full[512];

	snprintf(full, sizeof(full), "%s %s", prompt,
		 def == 1 ? "[Y/n]" : def == 0 ? "[y/N]" : "[y/n]");
	for (;;) {
		int rc = ui_line(full, NULL, buf, sizeof(buf));

		if (rc != UI_OK)
			return rc;
		if (!buf[0] && def >= 0) {
			*answer = def;
			return UI_OK;
		}
		if (!strcasecmp(buf, "y") || !strcasecmp(buf, "yes")) {
			*answer = 1;
			return UI_OK;
		}
		if (!strcasecmp(buf, "n") || !strcasecmp(buf, "no")) {
			*answer = 0;
			return UI_OK;
		}
		printf("Please answer y or n.\n");
	}
}

int ui_number(const char *prompt, int lo, int hi, int def, int *out)
{
	char buf[64], dbuf[16];

	if (def >= 0)
		snprintf(dbuf, sizeof(dbuf), "%d", def);
	for (;;) {
		char *end;
		long v;
		int rc = ui_line(prompt, def >= 0 ? dbuf : NULL, buf,
				 sizeof(buf));

		if (rc != UI_OK)
			return rc;
		v = strtol(buf, &end, 10);
		if (buf[0] && *end == '\0' && v >= lo && v <= hi) {
			*out = (int)v;
			return UI_OK;
		}
		printf("Please type a number from %d to %d.\n", lo, hi);
	}
}

int ui_menu(const char *prompt, const char *const *items, int n, int def,
	    int *choice)
{
	int v, rc;

	for (int i = 0; i < n; i++)
		printf("  %s%2d%s  %s\n", ui_bold(), i + 1, ui_reset(), items[i]);
	printf("\n");
	rc = ui_number(prompt, 1, n, def >= 0 ? def + 1 : -1, &v);
	if (rc == UI_OK)
		*choice = v - 1;
	return rc;
}

/* ---- progress ------------------------------------------------------------ */

void ui_progress_begin(struct ui_progress_state *s)
{
	memset(s, 0, sizeof(*s));
	s->last_step = -1;
	s->last_pct = -1;
}

/* The same words as the graphical installer's. */
void ui_eta_text(int eta, char *buf, size_t len)
{
	if (eta < 0)
		buf[0] = '\0';
	else if (eta < 60)
		snprintf(buf, len, "less than a minute left");
	else if (eta < 90)
		snprintf(buf, len, "about a minute left");
	else if (eta < 90 * 60)
		snprintf(buf, len, "about %d minutes left", (eta + 30) / 60);
	else
		snprintf(buf, len, "about %d hours left", (eta + 1800) / 3600);
}

void ui_group_digits(unsigned long long v, char *buf, size_t len)
{
	char raw[32];
	size_t n, o = 0;

	snprintf(raw, sizeof(raw), "%llu", v);
	n = strlen(raw);
	for (size_t i = 0; i < n && o + 2 < len; i++) {
		if (i && (n - i) % 3 == 0)
			buf[o++] = ',';
		buf[o++] = raw[i];
	}
	if (len)
		buf[o < len ? o : len - 1] = '\0';
}

void ui_progress_clear(struct ui_progress_state *s)
{
	if (s->drawn && g_out_tty) {
		fputs("\r\033[K", stdout);
		fflush(stdout);
	}
	s->drawn = 0;
}

void ui_progress_step_done(struct ui_progress_state *s, const char *title)
{
	ui_progress_clear(s);
	if (g_out_tty)
		printf("  [done] %s\n", title);
	else
		printf("[done] %s\n", title);
	fflush(stdout);
}

void ui_progress(struct ui_progress_state *s, int step, int nsteps,
		 const char *step_title, int percent, const char *counts,
		 const char *detail, int eta)
{
	int pct = percent;
	char eta_text[48];
	long long now = ui_now_ms();

	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	ui_eta_text(pct < 100 ? eta : -1, eta_text, sizeof(eta_text));
	/* a step starts with its own title as the detail */
	if (detail && strcmp(detail, step_title) == 0)
		detail = NULL;

	if (!g_out_tty) {
		/* A log: a line per step and per 5 per cent. */
		if (step != s->last_step || pct >= s->last_pct + 5 ||
		    (pct == 100 && s->last_pct != 100)) {
			printf("[%3d%%] step %d/%d: %s%s%s%s%s\n", pct, step + 1,
			       nsteps, step_title, counts ? ", " : "",
			       counts ? counts : "", eta_text[0] ? ", " : "",
			       eta_text);
			fflush(stdout);
			s->last_pct = pct;
		}
		s->last_step = step;
		return;
	}

	/* A terminal: one line redrawn in place, at most ten times a second
	 * unless the step changed. */
	if (step == s->last_step && now - s->last_draw_ms < 100 &&
	    !(pct == 100 && s->last_pct != 100))
		return;
	{
		int cols = ui_cols() - 1, barw, fill;
		char bar[128], head[256], tail[512], line[1024], fitted[1024];

		barw = cols >= 100 ? 40 : cols >= 70 ? 25 : 12;
		fill = barw * pct / 100;
		for (int i = 0; i < barw; i++)
			bar[i] = i < fill ? '#' : '-';
		bar[barw] = '\0';
		snprintf(head, sizeof(head), "  [%s] %3d%%  ", bar, pct);
		/* what would be cut on a narrow terminal goes last: the
		 * name of the file */
		snprintf(tail, sizeof(tail), "%s%s%s%s%s%s%s", step_title,
			 counts ? " (" : "", counts ? counts : "",
			 counts ? ")" : "",
			 eta_text[0] ? " -- " : "", eta_text,
			 detail && detail[0] ? " -- " : "");
		if (detail && detail[0])
			strncat(tail, detail, sizeof(tail) - strlen(tail) - 1);
		snprintf(line, sizeof(line), "%s%s", head, tail);
		ui_fit(fitted, sizeof(fitted), line, cols, 0);
		fputs("\r\033[K", stdout);
		fputs(fitted, stdout);
		fflush(stdout);
		s->drawn = 1;
		s->last_draw_ms = now;
	}
	s->last_step = step;
	s->last_pct = pct;
}

void ui_progress_end(struct ui_progress_state *s)
{
	ui_progress_clear(s);
}
