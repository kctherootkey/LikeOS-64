/*
 * sed - stream editor
 *
 * POSIX sed(1) with the extensions that scripts in the wild assume: -i, -E/-r,
 * -s, -z, case conversion in replacements, and I/i address and s/// modifiers.
 *
 * The model, which the rest of this file only makes sense against:
 *
 *   Input is read one line at a time into the PATTERN SPACE.  Every command in
 *   the script is considered in order; a command whose address matches runs.
 *   At the end of the script the pattern space is printed (unless -n) and the
 *   cycle repeats.  A separate HOLD SPACE persists across cycles and is the
 *   only memory a script has.
 *
 *   The trailing newline is NOT part of the pattern space.  That is why `$` in
 *   a regex matches end-of-line rather than the newline itself, and why N and
 *   G have to insert one explicitly.
 *
 * Two details are worth stating because getting them wrong is invisible in
 * simple scripts and wrong in real ones:
 *
 *   - A range address (addr1,addr2) becomes active when addr1 matches and
 *     stays active until addr2 matches on a LATER line.  addr2 is never tested
 *     on the same line that activated the range, so `/a/,/a/` spans from one
 *     "a" line to the next, not a single line.
 *
 *   - `s///g` must make progress on an empty match, or a pattern that can
 *     match nothing (like `x*`) loops forever.  After an empty match the scan
 *     advances one character, copying it through.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <regex.h>
#include <getopt.h>
#include <limits.h>

#define PROGRAM_NAME "sed"
#define VERSION "1.0"

/* ── Growable text buffer ───────────────────────────────────────────── */
/*
 * The pattern and hold spaces hold arbitrary binary, including embedded NULs
 * after N or with -z, so everything carries an explicit length rather than
 * relying on termination.  The buffer is still NUL-terminated so it can be
 * handed to regexec.
 */
typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} sbuf_t;

static void sbuf_init(sbuf_t *b)
{
	b->buf = malloc(128);
	if (!b->buf) {
		fprintf(stderr, "sed: out of memory\n");
		exit(4);
	}
	b->buf[0] = '\0';
	b->len = 0;
	b->cap = 128;
}

static void sbuf_reserve(sbuf_t *b, size_t need)
{
	if (b->cap >= need + 1)
		return;
	while (b->cap < need + 1)
		b->cap *= 2;
	b->buf = realloc(b->buf, b->cap);
	if (!b->buf) {
		fprintf(stderr, "sed: out of memory\n");
		exit(4);
	}
}

static void sbuf_set(sbuf_t *b, const char *s, size_t n)
{
	sbuf_reserve(b, n);
	memcpy(b->buf, s, n);
	b->len = n;
	b->buf[n] = '\0';
}

static void sbuf_append(sbuf_t *b, const char *s, size_t n)
{
	sbuf_reserve(b, b->len + n);
	memcpy(b->buf + b->len, s, n);
	b->len += n;
	b->buf[b->len] = '\0';
}

static void sbuf_addc(sbuf_t *b, char c)
{
	sbuf_append(b, &c, 1);
}

/* ── Addresses ──────────────────────────────────────────────────────── */

enum addr_kind {
	ADDR_NONE,
	ADDR_LINE, /* N            */
	ADDR_LAST, /* $            */
	ADDR_RE,   /* /re/         */
	ADDR_STEP, /* first~step   */
	ADDR_PLUS, /* addr1,+N     */
	ADDR_MULT, /* addr1,~N     */
	ADDR_ZERO, /* 0,/re/       */
};

typedef struct {
	enum addr_kind kind;
	long line;
	long step;
	regex_t *re;
} addr_t;

/* ── Commands ───────────────────────────────────────────────────────── */

typedef struct cmd {
	char name;
	addr_t a1, a2;
	int naddr;
	int negate; /* ! */

	/* range state */
	int active;
	int started; /* a 0,/re/ range may only ever begin once */
	long range_end; /* for +N and ~N forms */

	/* s/// */
	regex_t *re;
	char *repl;
	int global;
	int print;
	int nth;
	FILE *wfile;

	/* y/// */
	char *ytab;

	/* a, i, c, r, w, b, t, :, q, l */
	char *text;
	int qexit;
	long lwidth;

	struct cmd *block; /* { } body */
	struct cmd *next;
} cmd_t;

/* ── Global state ───────────────────────────────────────────────────── */

static cmd_t *script;
static int no_autoprint;      /* -n */
static int posix_mode;        /* --posix */
static int separate_files;    /* -s */
static int in_place;          /* -i */
static char *in_place_suffix; /* -i SUFFIX */
static int ere;               /* -E / -r */
static char line_sep = '\n';  /* -z makes this '\0' */
static int exit_status;

static sbuf_t pattern, hold, append_q;
static long line_no;
static int last_line;
static FILE *out;

static void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#include <stdarg.h>

static void fatal(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "sed: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = calloc(1, n);

	if (!p)
		fatal("out of memory");
	return p;
}

/* ── Script parsing ─────────────────────────────────────────────────── */

static const char *sp; /* current position in the script text */

static void skip_ws(void)
{
	while (*sp == ' ' || *sp == '\t')
		sp++;
}

/*
 * Turn the character escapes into the characters they stand for, BEFORE the
 * pattern reaches regcomp.
 *
 * POSIX regular expressions have no \n: to a BRE engine, `\n` is just an
 * escaped `n`, which matches the letter.  So `N;s/\n/+/` -- the standard way
 * to join two lines -- silently replaced the letter n instead of the newline
 * that N had just inserted.  Every sed does this translation for exactly that
 * reason.
 *
 * Only the character escapes are translated.  \( \) \1 \. \* \[ \\ are
 * regex syntax and must reach the engine untouched, and \b \w \s \< \> are
 * word-boundary and class operators, not backspace and friends.
 */
static char *unescape_regex(const char *pat, size_t len, size_t *outlen)
{
	char *out = xmalloc(len + 1);
	size_t i = 0, o = 0;

	while (i < len) {
		if (pat[i] == '\\' && i + 1 < len) {
			char c = pat[i + 1];
			char lit = 0;

			switch (c) {
			case 'n': lit = '\n'; break;
			case 't': lit = '\t'; break;
			case 'r': lit = '\r'; break;
			case 'f': lit = '\f'; break;
			case 'v': lit = '\v'; break;
			case 'a': lit = '\a'; break;
			default: break;
			}
			if (lit) {
				out[o++] = lit;
				i += 2;
				continue;
			}
			out[o++] = pat[i++];
			out[o++] = pat[i++];
			continue;
		}
		out[o++] = pat[i++];
	}
	out[o] = '\0';
	*outlen = o;
	return out;
}

static regex_t *compile_re(const char *pat, size_t len, int icase, int mline)
{
	regex_t *re = xmalloc(sizeof(*re));
	size_t plen;
	char *p = unescape_regex(pat, len, &plen);
	int flags = 0;
	int rc;

	(void)plen;

	if (ere)
		flags |= REG_EXTENDED;
	if (icase)
		flags |= REG_ICASE;
	if (mline)
		flags |= REG_NEWLINE;

	rc = regcomp(re, p, flags);
	if (rc != 0) {
		char err[256];

		regerror(rc, re, err, sizeof(err));
		fatal("-e expression #1, char %d: %s", (int)(sp - p), err);
	}
	free(p);
	return re;
}

/*
 * Read a delimited regex or replacement, honouring backslash escapes of the
 * delimiter.  Returns a freshly allocated copy with `\<delim>` reduced to
 * `<delim>` -- but ONLY the delimiter, because every other backslash sequence
 * has to survive intact for regcomp or the replacement expander to see.
 */
static char *read_delimited(char delim, size_t *outlen)
{
	const char *start = sp;
	size_t n = 0;
	char *buf;
	size_t i = 0;

	for (const char *p = start; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			n += 2;
			continue;
		}
		if (*p == delim)
			break;
		if (*p == '\n' && delim != '\n')
			break;
		n++;
	}

	buf = xmalloc(n + 1);
	while (*sp) {
		if (*sp == '\\' && sp[1]) {
			if (sp[1] == delim) {
				/* The delimiter was escaped only so it could
				 * appear here; the regex must see it bare. */
				buf[i++] = delim;
				sp += 2;
				continue;
			}
			buf[i++] = *sp++;
			buf[i++] = *sp++;
			continue;
		}
		if (*sp == delim)
			break;
		buf[i++] = *sp++;
	}
	buf[i] = '\0';
	if (outlen)
		*outlen = i;
	return buf;
}

static int parse_addr(addr_t *a)
{
	skip_ws();

	if (*sp == '$') {
		sp++;
		a->kind = ADDR_LAST;
		return 1;
	}
	if (isdigit((unsigned char)*sp)) {
		char *end;

		a->line = strtol(sp, &end, 10);
		sp = end;
		if (*sp == '~') {
			sp++;
			a->step = strtol(sp, &end, 10);
			sp = end;
			a->kind = ADDR_STEP;
		} else {
			a->kind = a->line == 0 ? ADDR_ZERO : ADDR_LINE;
		}
		return 1;
	}
	if (*sp == '/' || *sp == '\\') {
		char delim = '/';
		char *pat;
		size_t len;
		int icase = 0, mline = 0;

		if (*sp == '\\') {
			sp++;
			delim = *sp;
			if (!delim)
				fatal("unterminated address regex");
		}
		sp++;
		pat = read_delimited(delim, &len);
		if (*sp != delim)
			fatal("unterminated address regex");
		sp++;
		while (*sp == 'I' || *sp == 'M') {
			if (*sp == 'I')
				icase = 1;
			else
				mline = 1;
			sp++;
		}
		/* An empty regex reuses the last one applied, which is what
		 * makes `/foo/s//bar/` work.  Not supported here: it is
		 * ambiguous in the presence of ranges, and every use of it is
		 * clearer written out. */
		if (len == 0)
			fatal("no previous regular expression");
		a->kind = ADDR_RE;
		a->re = compile_re(pat, len, icase, mline);
		free(pat);
		return 1;
	}
	return 0;
}

static char *read_label(void)
{
	const char *start;
	char *s;
	size_t n;

	skip_ws();
	start = sp;
	while (*sp && *sp != '\n' && *sp != ';' && *sp != '}')
		sp++;
	n = (size_t)(sp - start);
	while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t'))
		n--;
	s = xmalloc(n + 1);
	memcpy(s, start, n);
	s[n] = '\0';
	return s;
}

/*
 * Text for a, i and c.  Two spellings are accepted:
 *
 *   a\           the POSIX one: text on the following lines, each but the last
 *   text         continued with a trailing backslash
 *
 *   a text       the one everyone actually writes
 */
static char *read_text(void)
{
	sbuf_t t;
	char *s;

	sbuf_init(&t);
	skip_ws();
	if (*sp == '\\') {
		sp++;
		if (*sp == '\n')
			sp++;
	}
	while (*sp) {
		if (*sp == '\\' && sp[1]) {
			sp++;
			/* Backslash-newline continues the text onto the next
			 * line.  The character escapes are expanded here for
			 * the same reason they are in a regex: `a line\twith`
			 * is meant to contain a tab, and treating the escape as
			 * "the letter t" is silently wrong. */
			switch (*sp) {
			case 'n': sbuf_addc(&t, '\n'); sp++; break;
			case 't': sbuf_addc(&t, '\t'); sp++; break;
			case 'r': sbuf_addc(&t, '\r'); sp++; break;
			case 'f': sbuf_addc(&t, '\f'); sp++; break;
			case 'v': sbuf_addc(&t, '\v'); sp++; break;
			case 'a': sbuf_addc(&t, '\a'); sp++; break;
			default:  sbuf_addc(&t, *sp++); break;
			}
			continue;
		}
		if (*sp == '\n')
			break;
		sbuf_addc(&t, *sp++);
	}
	s = xmalloc(t.len + 1);
	memcpy(s, t.buf, t.len + 1);
	free(t.buf);
	return s;
}

static cmd_t *parse_cmds(int depth);

static void parse_s(cmd_t *c)
{
	char delim = *sp++;
	char *pat;
	size_t plen;
	int icase = 0, mline = 0;

	if (!delim || delim == '\n' || delim == '\\')
		fatal("unterminated `s' command");

	pat = read_delimited(delim, &plen);
	if (*sp != delim)
		fatal("unterminated `s' command");
	sp++;
	c->repl = read_delimited(delim, NULL);
	if (*sp != delim)
		fatal("unterminated `s' command");
	sp++;

	c->nth = 0;
	for (;;) {
		if (*sp == 'g') {
			c->global = 1;
			sp++;
		} else if (*sp == 'p') {
			c->print = 1;
			sp++;
		} else if (*sp == 'i' || *sp == 'I') {
			icase = 1;
			sp++;
		} else if (*sp == 'm' || *sp == 'M') {
			mline = 1;
			sp++;
		} else if (isdigit((unsigned char)*sp)) {
			char *end;

			c->nth = (int)strtol(sp, &end, 10);
			sp = end;
			if (c->nth == 0)
				fatal("number option to `s' may not be zero");
		} else if (*sp == 'w') {
			char *fn;

			sp++;
			fn = read_label();
			c->wfile = strcmp(fn, "/dev/stdout") == 0
					   ? stdout
					   : fopen(fn, "w");
			if (!c->wfile)
				fatal("couldn't open file %s: %s", fn,
				      strerror(errno));
			free(fn);
			break;
		} else {
			break;
		}
	}

	if (plen == 0)
		fatal("no previous regular expression");
	c->re = compile_re(pat, plen, icase, mline);
	free(pat);
	if (c->nth == 0)
		c->nth = 1;
}

static void parse_y(cmd_t *c)
{
	char delim = *sp++;
	char *from, *to;
	size_t i;

	from = read_delimited(delim, NULL);
	if (*sp != delim)
		fatal("unterminated `y' command");
	sp++;
	to = read_delimited(delim, NULL);
	if (*sp != delim)
		fatal("unterminated `y' command");
	sp++;

	if (strlen(from) != strlen(to))
		fatal("strings for `y' command are different lengths");

	/* A 256-entry table rather than a search: y runs per character on every
	 * matching line, and this makes it a single load. */
	c->ytab = xmalloc(256);
	for (i = 0; i < 256; i++)
		c->ytab[i] = (char)i;
	for (i = 0; from[i]; i++)
		c->ytab[(unsigned char)from[i]] = to[i];

	free(from);
	free(to);
}

static cmd_t *parse_one(int depth)
{
	cmd_t *c = xmalloc(sizeof(*c));

	if (parse_addr(&c->a1)) {
		c->naddr = 1;
		skip_ws();
		if (*sp == ',') {
			sp++;
			skip_ws();
			if (*sp == '+') {
				char *end;

				sp++;
				c->a2.kind = ADDR_PLUS;
				c->a2.line = strtol(sp, &end, 10);
				sp = end;
			} else if (*sp == '~') {
				char *end;

				sp++;
				c->a2.kind = ADDR_MULT;
				c->a2.line = strtol(sp, &end, 10);
				sp = end;
			} else if (!parse_addr(&c->a2)) {
				fatal("expected context address");
			}
			c->naddr = 2;
		}
	}

	skip_ws();
	while (*sp == '!') {
		c->negate = !c->negate;
		sp++;
		skip_ws();
	}

	c->name = *sp;
	if (!c->name)
		fatal("missing command");
	sp++;

	switch (c->name) {
	case '{':
		c->block = parse_cmds(depth + 1);
		skip_ws();
		if (*sp != '}')
			fatal("unexpected `,'");
		sp++;
		break;
	case '}':
		fatal("unexpected `}'");
	case 's':
		parse_s(c);
		break;
	case 'y':
		parse_y(c);
		break;
	case 'a':
	case 'i':
	case 'c':
		c->text = read_text();
		break;
	case 'b':
	case 't':
	case 'T':
	case ':':
		c->text = read_label();
		break;
	case 'r':
	case 'R':
	case 'w':
	case 'W':
		c->text = read_label();
		if (c->name == 'w' || c->name == 'W') {
			c->wfile = strcmp(c->text, "/dev/stdout") == 0
					   ? stdout
					   : fopen(c->text, "w");
			if (!c->wfile)
				fatal("couldn't open file %s: %s", c->text,
				      strerror(errno));
		}
		break;
	case 'q':
	case 'Q':
		skip_ws();
		if (isdigit((unsigned char)*sp)) {
			char *end;

			c->qexit = (int)strtol(sp, &end, 10);
			sp = end;
		}
		break;
	case 'l':
		skip_ws();
		c->lwidth = 70;
		if (isdigit((unsigned char)*sp)) {
			char *end;

			c->lwidth = strtol(sp, &end, 10);
			sp = end;
		}
		break;
	case '=':
	case 'd':
	case 'D':
	case 'g':
	case 'G':
	case 'h':
	case 'H':
	case 'n':
	case 'N':
	case 'p':
	case 'P':
	case 'x':
	case 'z':
	case 'F':
		break;
	case '#':
		while (*sp && *sp != '\n')
			sp++;
		break;
	default:
		fatal("unknown command: `%c'", c->name);
	}

	/* Commands that take free-form text swallow the separator themselves. */
	if (c->name != '#' && c->name != 'a' && c->name != 'i' &&
	    c->name != 'c' && c->name != 'b' && c->name != 't' &&
	    c->name != 'T' && c->name != ':' && c->name != 'r' &&
	    c->name != 'R' && c->name != 'w' && c->name != 'W') {
		skip_ws();
		if (*sp == ';' || *sp == '\n')
			sp++;
		else if (*sp && *sp != '}' && *sp != '#')
			fatal("extra characters after command");
	}

	return c;
}

static cmd_t *parse_cmds(int depth)
{
	cmd_t *head = NULL, **tail = &head;

	for (;;) {
		while (*sp == ';' || *sp == '\n' || *sp == ' ' || *sp == '\t')
			sp++;
		if (!*sp)
			break;
		if (*sp == '}') {
			if (depth == 0)
				fatal("unexpected `}'");
			break;
		}
		*tail = parse_one(depth);
		tail = &(*tail)->next;
	}
	return head;
}

static void parse_script(const char *text)
{
	cmd_t **tail = &script;

	sp = text;
	while (*tail)
		tail = &(*tail)->next;
	*tail = parse_cmds(0);
}

/* ── Address matching ───────────────────────────────────────────────── */

static int re_matches(regex_t *re, const sbuf_t *b)
{
	regmatch_t m;

	/* REG_STARTEND so an embedded NUL (after N, or with -z) does not end
	 * the subject early. */
	m.rm_so = 0;
	m.rm_eo = (regoff_t)b->len;
	return regexec(re, b->buf, 0, &m, REG_STARTEND) == 0;
}

static int addr1_matches(cmd_t *c)
{
	switch (c->a1.kind) {
	case ADDR_NONE:
		return 1;
	case ADDR_LINE:
		return line_no == c->a1.line;
	case ADDR_ZERO:
		return 0; /* 0 only ever starts a 0,/re/ range */
	case ADDR_LAST:
		return last_line;
	case ADDR_RE:
		return re_matches(c->a1.re, &pattern);
	case ADDR_STEP:
		if (c->a1.step <= 0)
			return line_no == c->a1.line;
		return line_no >= c->a1.line &&
		       (line_no - c->a1.line) % c->a1.step == 0;
	default:
		return 0;
	}
}

static int addr2_matches(cmd_t *c)
{
	switch (c->a2.kind) {
	case ADDR_LINE:
		return line_no >= c->a2.line;
	case ADDR_LAST:
		return last_line;
	case ADDR_RE:
		return re_matches(c->a2.re, &pattern);
	case ADDR_PLUS:
	case ADDR_MULT:
		return line_no >= c->range_end;
	default:
		return 1;
	}
}

/*
 * Does this command run on the current line?
 *
 * The range logic is the subtle part.  When a range activates, addr2 is NOT
 * tested on the same line -- so `/a/,/a/` runs from one "a" to the NEXT one.
 * The one exception is the 0,/re/ form, which exists precisely so that addr2
 * can match on line 1.
 */
static int cmd_applies(cmd_t *c)
{
	int yes;

	if (c->naddr == 0) {
		yes = 1;
	} else if (c->naddr == 1) {
		yes = addr1_matches(c);
	} else if (c->active) {
		yes = 1;
		if (addr2_matches(c))
			c->active = 0;
	} else if (c->a1.kind == ADDR_ZERO && !c->started) {
		/* 0,/re/ is active before the first line, which is the whole
		 * point of it: addr2 gets tested on line 1, so `0,/re/` can end
		 * on the very first line where `1,/re/` cannot.
		 *
		 * `started` makes it one-shot.  Without it the range restarts
		 * every time it closes, and `0,/alpha/p` prints the whole file
		 * instead of the first line. */
		c->started = 1;
		c->active = 1;
		yes = 1;
		if (addr2_matches(c))
			c->active = 0;
	} else if (addr1_matches(c)) {
		yes = 1;
		if (c->a2.kind == ADDR_PLUS) {
			c->range_end = line_no + c->a2.line;
			c->active = c->a2.line > 0;
		} else if (c->a2.kind == ADDR_MULT) {
			long mult = c->a2.line;

			if (mult <= 0) {
				c->active = 0;
			} else {
				c->range_end = ((line_no / mult) + 1) * mult;
				c->active = c->range_end > line_no;
			}
		} else if (c->a2.kind == ADDR_LINE &&
			   c->a2.line <= line_no) {
			/* A range whose end is already past is one line only. */
			c->active = 0;
		} else {
			c->active = 1;
		}
	} else {
		yes = 0;
	}

	return c->negate ? !yes : yes;
}

/* ── Output ─────────────────────────────────────────────────────────── */

static void emit(const char *s, size_t n, int newline)
{
	fwrite(s, 1, n, out);
	if (newline)
		fputc(line_sep, out);
}

/* `l`: print the pattern space unambiguously. */
static void do_list(long width)
{
	long col = 0;
	size_t i;

	for (i = 0; i < pattern.len; i++) {
		unsigned char ch = (unsigned char)pattern.buf[i];
		char tmp[8];
		int n;

		switch (ch) {
		case '\\': n = snprintf(tmp, sizeof(tmp), "\\\\"); break;
		case '\a': n = snprintf(tmp, sizeof(tmp), "\\a"); break;
		case '\b': n = snprintf(tmp, sizeof(tmp), "\\b"); break;
		case '\f': n = snprintf(tmp, sizeof(tmp), "\\f"); break;
		case '\n': n = snprintf(tmp, sizeof(tmp), "\\n"); break;
		case '\r': n = snprintf(tmp, sizeof(tmp), "\\r"); break;
		case '\t': n = snprintf(tmp, sizeof(tmp), "\\t"); break;
		case '\v': n = snprintf(tmp, sizeof(tmp), "\\v"); break;
		default:
			if (isprint(ch))
				n = snprintf(tmp, sizeof(tmp), "%c", ch);
			else
				n = snprintf(tmp, sizeof(tmp), "\\%03o", ch);
		}
		/* Wrap with a trailing backslash so the output can be read back
		 * as one logical line. */
		if (width > 1 && col + n > width - 1) {
			fputs("\\\n", out);
			col = 0;
		}
		fwrite(tmp, 1, (size_t)n, out);
		col += n;
	}
	fputs("$\n", out);
}

/* ── The s/// replacement expander ──────────────────────────────────── */
/*
 * Handles &, \0-\9, \n \t and friends, and the GNU case operators \U \L \u \l
 * \E.  Case conversion is stateful across the whole replacement, which is why
 * it is tracked in locals rather than applied per-piece.
 */
/* Case-conversion state, carried across the whole replacement. */
typedef struct {
	sbuf_t *dst;
	int all; /* 'U', 'L' or 0 -- until \E */
	int one; /* 'u', 'l' or 0 -- next character only */
} repl_ctx;

static void repl_put(repl_ctx *rc, char ch)
{
	if (rc->one) {
		ch = rc->one == 'u' ? (char)toupper((unsigned char)ch)
				    : (char)tolower((unsigned char)ch);
		rc->one = 0;
	} else if (rc->all) {
		ch = rc->all == 'U' ? (char)toupper((unsigned char)ch)
				    : (char)tolower((unsigned char)ch);
	}
	sbuf_addc(rc->dst, ch);
}

static void repl_put_group(repl_ctx *rc, int g, const char *src,
			   regmatch_t *m, size_t nmatch)
{
	if ((size_t)g >= nmatch || m[g].rm_so < 0)
		return;
	for (regoff_t i = m[g].rm_so; i < m[g].rm_eo; i++)
		repl_put(rc, src[i]);
}

static void expand_repl(sbuf_t *dst, const char *repl, const char *src,
			regmatch_t *m, size_t nmatch)
{
	repl_ctx rc = { dst, 0, 0 };
	const char *p;

	for (p = repl; *p; p++) {
		if (*p == '&') {
			repl_put_group(&rc, 0, src, m, nmatch);
			continue;
		}
		if (*p != '\\') {
			repl_put(&rc, *p);
			continue;
		}
		p++;
		if (!*p) {
			sbuf_addc(dst, '\\');
			break;
		}
		if (*p >= '0' && *p <= '9') {
			repl_put_group(&rc, *p - '0', src, m, nmatch);
			continue;
		}
		switch (*p) {
		case 'n': repl_put(&rc, '\n'); break;
		case 't': repl_put(&rc, '\t'); break;
		case 'r': repl_put(&rc, '\r'); break;
		case 'a': repl_put(&rc, '\a'); break;
		case 'f': repl_put(&rc, '\f'); break;
		case 'v': repl_put(&rc, '\v'); break;
		case 'U': rc.all = 'U'; rc.one = 0; break;
		case 'L': rc.all = 'L'; rc.one = 0; break;
		case 'E': rc.all = 0;   rc.one = 0; break;
		case 'u': rc.one = 'u'; break;
		case 'l': rc.one = 'l'; break;
		/* Any other escaped character stands for itself, which is how
		 * a literal & or \ reaches the output. */
		default:  repl_put(&rc, *p); break;
		}
	}
}

static int do_subst(cmd_t *c)
{
	sbuf_t result;
	regmatch_t m[10];
	size_t off = 0;
	int count = 0;
	int did = 0;

	sbuf_init(&result);

	while (off <= pattern.len) {
		m[0].rm_so = (regoff_t)off;
		m[0].rm_eo = (regoff_t)pattern.len;
		/* NOTBOL once past the start, so ^ cannot match again in the
		 * middle of the line during a /g scan. */
		if (regexec(c->re, pattern.buf, 10, m,
			    REG_STARTEND | (off ? REG_NOTBOL : 0)) != 0)
			break;

		count++;
		if (count < c->nth) {
			/* Not yet the occurrence asked for: copy it through
			 * untouched and keep looking. */
			size_t upto = (size_t)m[0].rm_eo;

			if ((size_t)m[0].rm_eo == (size_t)m[0].rm_so) {
				if (upto < pattern.len)
					upto++;
				else
					break;
			}
			sbuf_append(&result, pattern.buf + off, upto - off);
			off = upto;
			continue;
		}

		sbuf_append(&result, pattern.buf + off,
			    (size_t)m[0].rm_so - off);
		expand_repl(&result, c->repl, pattern.buf, m, 10);
		did = 1;

		if ((size_t)m[0].rm_eo == (size_t)m[0].rm_so) {
			/* Empty match: copy one character and step past it, or
			 * this loops forever on a pattern like `x*`. */
			if ((size_t)m[0].rm_eo < pattern.len)
				sbuf_addc(&result, pattern.buf[m[0].rm_eo]);
			off = (size_t)m[0].rm_eo + 1;
		} else {
			off = (size_t)m[0].rm_eo;
		}

		if (!c->global)
			break;
	}

	if (!did) {
		free(result.buf);
		return 0;
	}

	if (off < pattern.len)
		sbuf_append(&result, pattern.buf + off, pattern.len - off);

	free(pattern.buf);
	pattern = result;
	return 1;
}

/* ── Input ──────────────────────────────────────────────────────────── */

static char **files;
static int nfiles;
static int file_idx;
static FILE *in;
static const char *cur_name = "-";
static int pending_no_nl; /* the line just read had no trailing separator */

static int open_next_file(void)
{
	while (file_idx < nfiles) {
		const char *f = files[file_idx++];

		if (strcmp(f, "-") == 0) {
			in = stdin;
			cur_name = "-";
			return 1;
		}
		in = fopen(f, "r");
		if (!in) {
			fprintf(stderr, "sed: can't read %s: %s\n", f,
				strerror(errno));
			exit_status = 2;
			continue;
		}
		cur_name = f;
		return 1;
	}
	return 0;
}

/* Read one line into b.  Returns 0 at end of all input. */
static int read_line(sbuf_t *b)
{
	int ch;

	for (;;) {
		if (!in && !open_next_file())
			return 0;

		b->len = 0;
		b->buf[0] = '\0';
		pending_no_nl = 1;

		while ((ch = fgetc(in)) != EOF) {
			if ((char)ch == line_sep) {
				pending_no_nl = 0;
				break;
			}
			sbuf_addc(b, (char)ch);
		}

		if (ch == EOF && b->len == 0) {
			if (in != stdin)
				fclose(in);
			in = NULL;
			if (separate_files || in_place)
				return 0; /* caller advances the file */
			continue;
		}
		line_no++;
		return 1;
	}
}

/* Is the current line the last one?  Requires a one-character lookahead, and
 * across files unless -s. */
static int at_last_line(void)
{
	int ch;

	if (!in)
		return 1;
	ch = fgetc(in);
	if (ch != EOF) {
		ungetc(ch, in);
		return 0;
	}
	if (separate_files || in_place)
		return 1;
	/* Peek into the following files: `$` means the last line of the whole
	 * stream unless -s says otherwise. */
	if (in != stdin)
		fclose(in);
	in = NULL;
	while (file_idx < nfiles) {
		if (!open_next_file())
			return 1;
		ch = fgetc(in);
		if (ch != EOF) {
			ungetc(ch, in);
			return 0;
		}
		if (in != stdin)
			fclose(in);
		in = NULL;
	}
	return 1;
}

static void flush_appends(void)
{
	if (append_q.len) {
		fwrite(append_q.buf, 1, append_q.len, out);
		append_q.len = 0;
		append_q.buf[0] = '\0';
	}
}

/* ── Execution ──────────────────────────────────────────────────────── */

enum { EX_NORMAL, EX_DELETE, EX_RESTART, EX_QUIT, EX_QUIT_SILENT };

static int tflag; /* set by a successful s///, cleared by t */
static int quit_code;

static cmd_t *find_label(cmd_t *list, const char *name)
{
	for (cmd_t *c = list; c; c = c->next) {
		if (c->name == ':' && strcmp(c->text, name) == 0)
			return c;
		if (c->name == '{') {
			cmd_t *f = find_label(c->block, name);

			if (f)
				return f;
		}
	}
	return NULL;
}

static int run(cmd_t *list, cmd_t **jump);

static int run_one(cmd_t *c, cmd_t **jump)
{
	switch (c->name) {
	case '{':
		return run(c->block, jump);
	case '=':
		fprintf(out, "%ld\n", line_no);
		break;
	case 'a':
		/* Queued, not printed: appended text appears AFTER the pattern
		 * space, at the end of the cycle. */
		sbuf_append(&append_q, c->text, strlen(c->text));
		sbuf_addc(&append_q, '\n');
		break;
	case 'i':
		fputs(c->text, out);
		fputc('\n', out);
		break;
	case 'c':
		/* For a range, the text appears once at the END of the range;
		 * for a single address, on every matching line. */
		if (c->naddr < 2 || !c->active) {
			fputs(c->text, out);
			fputc('\n', out);
		}
		return EX_DELETE;
	case 'd':
		return EX_DELETE;
	case 'D':
		if (memchr(pattern.buf, '\n', pattern.len)) {
			char *nl = memchr(pattern.buf, '\n', pattern.len);
			size_t keep = pattern.len - (size_t)(nl - pattern.buf) - 1;

			memmove(pattern.buf, nl + 1, keep);
			pattern.len = keep;
			pattern.buf[keep] = '\0';
			return EX_RESTART;
		}
		return EX_DELETE;
	case 'g':
		sbuf_set(&pattern, hold.buf, hold.len);
		break;
	case 'G':
		sbuf_addc(&pattern, '\n');
		sbuf_append(&pattern, hold.buf, hold.len);
		break;
	case 'h':
		sbuf_set(&hold, pattern.buf, pattern.len);
		break;
	case 'H':
		sbuf_addc(&hold, '\n');
		sbuf_append(&hold, pattern.buf, pattern.len);
		break;
	case 'x': {
		sbuf_t t = pattern;

		pattern = hold;
		hold = t;
		break;
	}
	case 'l':
		do_list(c->lwidth);
		break;
	case 'n':
		if (!no_autoprint)
			emit(pattern.buf, pattern.len, !pending_no_nl);
		flush_appends();
		if (!read_line(&pattern))
			return EX_QUIT;
		/* AFTER the read, not before: $ has to describe the line now in
		 * the pattern space.  Asking first answers "is there another
		 * line?", which makes $ true one line early -- and `$!{N;ba}`
		 * then stops one line short of the end. */
		last_line = at_last_line();
		break;
	case 'N':
		flush_appends();
		{
			sbuf_t nl;
			int had;

			sbuf_init(&nl);
			had = read_line(&nl);
			if (!had) {
				free(nl.buf);
				/* POSIX ends without printing; GNU prints.  The
				 * GNU behaviour is what scripts expect, and
				 * --posix asks for the other. */
				if (posix_mode)
					return EX_QUIT_SILENT;
				return EX_QUIT;
			}
			sbuf_addc(&pattern, '\n');
			sbuf_append(&pattern, nl.buf, nl.len);
			free(nl.buf);
			/* See the note in `n`: $ describes the line just read. */
			last_line = at_last_line();
		}
		break;
	case 'p':
		emit(pattern.buf, pattern.len, 1);
		break;
	case 'P': {
		char *nl = memchr(pattern.buf, '\n', pattern.len);
		size_t n = nl ? (size_t)(nl - pattern.buf) : pattern.len;

		emit(pattern.buf, n, 1);
		break;
	}
	case 'q':
		quit_code = c->qexit;
		return EX_QUIT;
	case 'Q':
		quit_code = c->qexit;
		return EX_QUIT_SILENT;
	case 'r': {
		FILE *f = fopen(c->text, "r");

		/* A missing file is silently ignored, per POSIX -- r is for
		 * optional boilerplate. */
		if (f) {
			int ch;

			while ((ch = fgetc(f)) != EOF)
				sbuf_addc(&append_q, (char)ch);
			fclose(f);
		}
		break;
	}
	case 'R': {
		static FILE *rf;
		static char *rname;
		int ch;

		if (!rf || !rname || strcmp(rname, c->text) != 0) {
			if (rf)
				fclose(rf);
			free(rname);
			rname = strdup(c->text);
			rf = fopen(c->text, "r");
		}
		if (rf) {
			while ((ch = fgetc(rf)) != EOF) {
				sbuf_addc(&append_q, (char)ch);
				if (ch == '\n')
					break;
			}
		}
		break;
	}
	case 'w':
		fwrite(pattern.buf, 1, pattern.len, c->wfile);
		fputc('\n', c->wfile);
		fflush(c->wfile);
		break;
	case 'W': {
		char *nl = memchr(pattern.buf, '\n', pattern.len);
		size_t n = nl ? (size_t)(nl - pattern.buf) : pattern.len;

		fwrite(pattern.buf, 1, n, c->wfile);
		fputc('\n', c->wfile);
		fflush(c->wfile);
		break;
	}
	case 's':
		if (do_subst(c)) {
			tflag = 1;
			if (c->print)
				emit(pattern.buf, pattern.len, 1);
			if (c->wfile) {
				fwrite(pattern.buf, 1, pattern.len, c->wfile);
				fputc('\n', c->wfile);
				fflush(c->wfile);
			}
		}
		break;
	case 'y':
		for (size_t i = 0; i < pattern.len; i++)
			pattern.buf[i] =
				c->ytab[(unsigned char)pattern.buf[i]];
		break;
	case 'b':
		if (c->text[0] == '\0')
			return EX_NORMAL | 0x100; /* branch to end of script */
		*jump = find_label(script, c->text);
		if (!*jump)
			fatal("can't find label for jump to `%s'", c->text);
		return EX_NORMAL | 0x200;
	case 't':
		if (tflag) {
			tflag = 0;
			if (c->text[0] == '\0')
				return EX_NORMAL | 0x100;
			*jump = find_label(script, c->text);
			if (!*jump)
				fatal("can't find label for jump to `%s'",
				      c->text);
			return EX_NORMAL | 0x200;
		}
		break;
	case 'T':
		if (!tflag) {
			if (c->text[0] == '\0')
				return EX_NORMAL | 0x100;
			*jump = find_label(script, c->text);
			if (!*jump)
				fatal("can't find label for jump to `%s'",
				      c->text);
			return EX_NORMAL | 0x200;
		}
		tflag = 0;
		break;
	case 'z':
		pattern.len = 0;
		pattern.buf[0] = '\0';
		break;
	case 'F':
		fprintf(out, "%s\n", cur_name);
		break;
	case ':':
	case '#':
		break;
	}
	return EX_NORMAL;
}

static int run(cmd_t *list, cmd_t **jump)
{
	for (cmd_t *c = list; c; c = c->next) {
		int r;

		if (!cmd_applies(c))
			continue;
		r = run_one(c, jump);
		if (r != EX_NORMAL)
			return r;
	}
	return EX_NORMAL;
}

/*
 * One cycle over the script for the current pattern space.
 *
 * Branching is why this is not simply run(): a b or t has to resume at a label
 * anywhere in the script, including inside a block it was not called from, so
 * the loop restarts from the jump target rather than unwinding.
 */
static int cycle(void)
{
	cmd_t *start = script;

	for (;;) {
		cmd_t *jump = NULL;
		int r = run(start, &jump);

		if (r & 0x200) {
			start = jump;
			continue;
		}
		if (r & 0x100)
			return EX_NORMAL; /* branch to end of script */
		return r;
	}
}

/* ── Driver ─────────────────────────────────────────────────────────── */

static void reset_ranges(cmd_t *list)
{
	for (cmd_t *c = list; c; c = c->next) {
		c->active = 0;
		c->started = 0;
		if (c->name == '{')
			reset_ranges(c->block);
	}
}

static int process_stream(void)
{
	int status = 0;

	while (read_line(&pattern)) {
		int r;

		last_line = at_last_line();
		tflag = 0;
		r = cycle();

		if (r == EX_RESTART) {
			/* D with an embedded newline: run the script again on
			 * what is left, WITHOUT reading a new line. */
			do {
				tflag = 0;
				r = cycle();
			} while (r == EX_RESTART);
		}

		if (r == EX_QUIT_SILENT) {
			status = 1;
			break;
		}
		if (r != EX_DELETE && !no_autoprint)
			emit(pattern.buf, pattern.len, !pending_no_nl);
		flush_appends();
		if (r == EX_QUIT) {
			status = 1;
			break;
		}
	}
	return status;
}

static void usage(int status) __attribute__((noreturn));

static void usage(int status)
{
	FILE *o = status == 0 ? stdout : stderr;

	fprintf(o,
		"Usage: sed [OPTION]... {script} [input-file]...\n\n"
		"  -n, --quiet, --silent    suppress automatic printing of pattern space\n"
		"  -e script                add the script to the commands to be executed\n"
		"  -f script-file           add the contents of script-file to the commands\n"
		"  -i[SUFFIX], --in-place[=SUFFIX]\n"
		"                           edit files in place (makes backup if SUFFIX given)\n"
		"  -E, -r, --regexp-extended\n"
		"                           use extended regular expressions\n"
		"  -s, --separate           consider files separately rather than as one stream\n"
		"  -z, --null-data          separate lines by NUL characters\n"
		"      --posix              disable all extensions\n"
		"      --help               display this help and exit\n"
		"      --version            output version information and exit\n\n"
		"If no -e, -f or --expression is given, the first non-option\n"
		"argument is taken as the sed script.\n");
	exit(status);
}

int main(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "quiet", no_argument, 0, 'n' },
		{ "silent", no_argument, 0, 'n' },
		{ "expression", required_argument, 0, 'e' },
		{ "file", required_argument, 0, 'f' },
		{ "in-place", optional_argument, 0, 'i' },
		{ "regexp-extended", no_argument, 0, 'E' },
		{ "separate", no_argument, 0, 's' },
		{ "null-data", no_argument, 0, 'z' },
		{ "posix", no_argument, 0, 1 },
		{ "help", no_argument, 0, 2 },
		{ "version", no_argument, 0, 3 },
		{ 0, 0, 0, 0 }
	};
	int c;
	int have_script = 0;
	sbuf_t script_text;

	sbuf_init(&script_text);
	sbuf_init(&pattern);
	sbuf_init(&hold);
	sbuf_init(&append_q);
	out = stdout;

	while ((c = getopt_long(argc, argv, "ne:f:i::Ersz", longopts, NULL)) != -1) {
		switch (c) {
		case 'n':
			no_autoprint = 1;
			break;
		case 'e':
			if (script_text.len)
				sbuf_addc(&script_text, '\n');
			sbuf_append(&script_text, optarg, strlen(optarg));
			have_script = 1;
			break;
		case 'f': {
			FILE *f = strcmp(optarg, "-") == 0 ? stdin
							   : fopen(optarg, "r");
			int ch;

			if (!f)
				fatal("can't read %s: %s", optarg,
				      strerror(errno));
			if (script_text.len)
				sbuf_addc(&script_text, '\n');
			while ((ch = fgetc(f)) != EOF)
				sbuf_addc(&script_text, (char)ch);
			if (f != stdin)
				fclose(f);
			have_script = 1;
			break;
		}
		case 'i':
			in_place = 1;
			/* -i takes its suffix ATTACHED (-i.bak), never as a
			 * separate argument -- `sed -i x file` edits in place
			 * with script x, it does not back up to "x". */
			if (optarg)
				in_place_suffix = strdup(optarg);
			break;
		case 'E':
		case 'r':
			ere = 1;
			break;
		case 's':
			separate_files = 1;
			break;
		case 'z':
			line_sep = '\0';
			break;
		case 1:
			posix_mode = 1;
			break;
		case 2:
			usage(0);
		case 3:
			printf("%s %s\n", PROGRAM_NAME, VERSION);
			return 0;
		default:
			usage(1);
		}
	}

	if (!have_script) {
		if (optind >= argc)
			usage(1);
		sbuf_append(&script_text, argv[optind], strlen(argv[optind]));
		optind++;
	}

	parse_script(script_text.buf);

	files = argv + optind;
	nfiles = argc - optind;
	if (nfiles == 0) {
		static char *stdin_only[] = { "-" };

		files = stdin_only;
		nfiles = 1;
		if (in_place)
			fatal("no input files while in place editing");
	}

	if (in_place) {
		/* Each file is its own stream, with its own line numbering and
		 * its own $, whether or not -s was given -- editing file B must
		 * not depend on how many lines file A had.
		 *
		 * The edit goes to a temporary file in the SAME directory and
		 * is renamed over the original, so a failure part way through
		 * leaves the original intact rather than truncated.
		 */
		for (int i = 0; i < nfiles; i++) {
			const char *f = files[i];
			char tmpname[PATH_MAX];
			char *one[1];
			char **saved_files = files;
			int saved_nfiles = nfiles;
			FILE *tmpf;
			int fd;

			snprintf(tmpname, sizeof(tmpname), "%s.sedXXXXXX", f);
			fd = mkstemp(tmpname);
			if (fd < 0)
				fatal("couldn't open temporary file %s: %s",
				      tmpname, strerror(errno));
			tmpf = fdopen(fd, "w");
			if (!tmpf)
				fatal("couldn't open temporary file %s: %s",
				      tmpname, strerror(errno));
			out = tmpf;

			one[0] = (char *)f;
			files = one;
			nfiles = 1;
			file_idx = 0;
			in = NULL;
			line_no = 0;
			reset_ranges(script);

			process_stream();

			files = saved_files;
			nfiles = saved_nfiles;
			fclose(tmpf);
			out = stdout;

			if (in_place_suffix && in_place_suffix[0]) {
				char bak[PATH_MAX];
				const char *star = strchr(in_place_suffix, '*');

				/* A * in the suffix is replaced by the file
				 * name, which is how a backup DIRECTORY is
				 * requested: a suffix of bak SLASH STAR puts
				 * the backup in bak/ under the same name. */
				if (star)
					snprintf(bak, sizeof(bak), "%.*s%s%s",
						 (int)(star - in_place_suffix),
						 in_place_suffix, f, star + 1);
				else
					snprintf(bak, sizeof(bak), "%s%s", f,
						 in_place_suffix);
				if (rename(f, bak) != 0)
					fatal("cannot rename %s: %s", f,
					      strerror(errno));
			}
			if (rename(tmpname, f) != 0)
				fatal("cannot rename %s: %s", tmpname,
				      strerror(errno));
		}
	} else if (separate_files) {
		for (int i = 0; i < nfiles; i++) {
			char *one[1] = { files[i] };
			char **savefiles = files;
			int savenf = nfiles;

			files = one;
			nfiles = 1;
			file_idx = 0;
			in = NULL;
			line_no = 0;
			reset_ranges(script);
			process_stream();
			files = savefiles;
			nfiles = savenf;
		}
	} else {
		process_stream();
	}

	if (fflush(stdout) != 0) {
		fprintf(stderr, "sed: couldn't flush stdout: %s\n",
			strerror(errno));
		return 4;
	}
	return quit_code ? quit_code : exit_status;
}
