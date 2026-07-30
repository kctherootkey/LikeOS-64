/*
 * expr - evaluate an expression and print the result
 *
 * POSIX expr(1), plus the string functions (length, substr, index, match) that
 * every implementation has carried for decades and that scripts use freely.
 *
 * Two things about this utility surprise people, and both are deliberate:
 *
 *   - The expression arrives as SEPARATE ARGUMENTS, not one string.  `expr 1 +
 *     2` is three arguments; the shell has already done the splitting, which
 *     is why every operator has to be quoted or spaced away from the shell's
 *     own meaning (`\*`, `\|`).  This program never tokenises anything.
 *
 *   - The EXIT STATUS is inverted relative to the value: 0 when the result is
 *     neither null nor zero, 1 when it is.  So `expr "$s" : '/dev/tty[0-9]*$'`
 *     succeeds when the string matches, which is exactly how startx uses it.
 *     2 means an invalid expression and 3 an error -- both distinct from a
 *     valid expression that evaluated to zero.
 *
 * Values are integers or strings, and which one a token is depends on context
 * rather than on the token: "10" is a string until something asks it to be a
 * number.  That is modelled below by keeping the string always and converting
 * on demand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <regex.h>
#include <errno.h>

#define PROGRAM_NAME "expr"
#define VERSION "1.0"

/* Exit codes are part of the interface, so they get names. */
#define EXPR_TRUE    0 /* result is neither null nor zero */
#define EXPR_FALSE   1 /* result is null or zero          */
#define EXPR_INVALID 2 /* the expression is not valid     */
#define EXPR_ERROR   3 /* something went wrong            */

/* ── Values ─────────────────────────────────────────────────────────── */
/*
 * A value is always available as a string; `is_num` records whether it is also
 * known to be an integer.  Keeping both avoids the classic expr bug where
 * `expr 010 + 0` prints 10 but `expr 010 = 010` compares as strings and the
 * two disagree about what 010 meant.
 */
typedef struct {
	char *s; /* always valid, always owned */
	long long n;
	int is_num;
} value_t;

static char **args; /* the argument vector being parsed */
static int nargs;
static int pos; /* index of the next token */

static void die(int status, const char *fmt, ...)
	__attribute__((noreturn, format(printf, 2, 3)));

#include <stdarg.h>

static void die(int status, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "expr: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(status);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);

	if (!p)
		die(EXPR_ERROR, "out of memory");
	return p;
}

static char *xstrdup(const char *s)
{
	char *p = strdup(s);

	if (!p)
		die(EXPR_ERROR, "out of memory");
	return p;
}

/* Is the whole string an integer, with optional sign and surrounding blanks?
 * Partial matches do NOT count: "12abc" is a string, not 12. */
static int looks_numeric(const char *s, long long *out)
{
	const char *p = s;
	char *end;
	long long v;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0')
		return 0;

	errno = 0;
	v = strtoll(p, &end, 10);
	if (end == p)
		return 0;
	if (errno == ERANGE)
		return 0;
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end != '\0')
		return 0;

	if (out)
		*out = v;
	return 1;
}

static value_t make_str(const char *s)
{
	value_t v;

	v.s = xstrdup(s);
	v.is_num = looks_numeric(s, &v.n);
	if (!v.is_num)
		v.n = 0;
	return v;
}

static value_t make_num(long long n)
{
	value_t v;
	char buf[32];

	snprintf(buf, sizeof(buf), "%lld", n);
	v.s = xstrdup(buf);
	v.n = n;
	v.is_num = 1;
	return v;
}

static void free_value(value_t *v)
{
	free(v->s);
	v->s = NULL;
}

/* Require an integer.  POSIX says a non-integer where one is needed is an
 * invalid expression, not a zero. */
static long long need_num(const value_t *v)
{
	if (!v->is_num)
		die(EXPR_INVALID, "non-integer argument");
	return v->n;
}

/* The truth of a value, used by | and &.  Null string or the integer zero is
 * false; everything else is true.  Note "00" is false (it is the number zero)
 * but "0x" is true (it is not a number at all). */
static int value_true(const value_t *v)
{
	if (v->is_num)
		return v->n != 0;
	return v->s[0] != '\0';
}

/* ── Tokens ─────────────────────────────────────────────────────────── */

static const char *peek(void)
{
	return pos < nargs ? args[pos] : NULL;
}

static const char *next(void)
{
	return pos < nargs ? args[pos++] : NULL;
}

static int accept_tok(const char *t)
{
	const char *p = peek();

	if (p && strcmp(p, t) == 0) {
		pos++;
		return 1;
	}
	return 0;
}

static value_t parse_or(void);

/* ── The regex operator ─────────────────────────────────────────────── */
/*
 * `STRING : BRE` anchors the BRE at the start of STRING (implicitly -- the
 * pattern is not written with ^) and yields either:
 *
 *   - the text matched by the first \( \) group, if the pattern has one, or
 *     the empty string if the group exists but did not participate; or
 *   - the NUMBER of characters matched, if it has no group.
 *
 * A failed match yields the empty string when there is a group and 0 when
 * there is not -- the two failure values differ, which is what lets a caller
 * distinguish "matched nothing" from "matched an empty group".
 */
static value_t do_match(const value_t *str, const value_t *pat)
{
	regex_t re;
	regmatch_t m[2];
	char *anchored;
	size_t len;
	int rc;
	value_t out;

	/* Anchor by construction rather than by asking regexec for a prefix
	 * match: a leading ^ in the user's pattern is then just a redundant
	 * anchor rather than a syntax error, which matches every other expr. */
	len = strlen(pat->s);
	anchored = xmalloc(len + 2);
	if (pat->s[0] == '^') {
		memcpy(anchored, pat->s, len + 1);
	} else {
		anchored[0] = '^';
		memcpy(anchored + 1, pat->s, len + 1);
	}

	rc = regcomp(&re, anchored, 0); /* BRE: POSIX requires basic here */
	if (rc != 0) {
		char err[256];

		regerror(rc, &re, err, sizeof(err));
		free(anchored);
		die(EXPR_INVALID, "invalid regular expression: %s", err);
	}
	free(anchored);

	rc = regexec(&re, str->s, 2, m, 0);

	if (re.re_nsub >= 1) {
		if (rc == 0 && m[1].rm_so >= 0) {
			size_t n = (size_t)(m[1].rm_eo - m[1].rm_so);
			char *sub = xmalloc(n + 1);

			memcpy(sub, str->s + m[1].rm_so, n);
			sub[n] = '\0';
			out = make_str(sub);
			free(sub);
		} else {
			out = make_str("");
		}
	} else {
		out = make_num(rc == 0 ? (long long)(m[0].rm_eo - m[0].rm_so)
					: 0);
	}

	regfree(&re);
	return out;
}

/* ── Grammar ────────────────────────────────────────────────────────── */
/*
 * Lowest to highest precedence:
 *
 *   |            or
 *   &            and
 *   = > >= < <= !=   comparison
 *   + -
 *   * / %
 *   :            regex match
 *   ( )  and the string functions
 *
 * Each level is a function that parses one tighter level and then loops on its
 * own operators, which is what makes them left-associative.
 */

static value_t parse_primary(void)
{
	const char *t = peek();

	if (!t)
		die(EXPR_INVALID, "syntax error: unexpected end of expression");

	if (accept_tok("(")) {
		value_t v = parse_or();

		if (!accept_tok(")"))
			die(EXPR_INVALID, "syntax error: expected ')'");
		return v;
	}

	/* `+ TOKEN` forces TOKEN to be read as a string even when it is also an
	 * operator name -- the only way to compare against a literal "match" or
	 * "index". */
	if (strcmp(t, "+") == 0 && pos + 1 < nargs) {
		pos++;
		return make_str(next());
	}

	if (strcmp(t, "length") == 0 && pos + 1 < nargs) {
		value_t s;
		long long n;

		pos++;
		s = parse_primary();
		n = (long long)strlen(s.s);
		free_value(&s);
		return make_num(n);
	}

	if (strcmp(t, "index") == 0 && pos + 2 < nargs) {
		value_t s, chars;
		long long idx = 0;
		const char *p;

		pos++;
		s = parse_primary();
		chars = parse_primary();
		/* 1-based position of the first character of s that appears
		 * anywhere in chars; 0 if none does. */
		for (p = s.s; *p; p++) {
			if (strchr(chars.s, *p)) {
				idx = (long long)(p - s.s) + 1;
				break;
			}
		}
		free_value(&s);
		free_value(&chars);
		return make_num(idx);
	}

	if (strcmp(t, "substr") == 0 && pos + 3 < nargs) {
		value_t s, vp, vl;
		long long start, len, slen;
		value_t out;

		pos++;
		s = parse_primary();
		vp = parse_primary();
		vl = parse_primary();
		slen = (long long)strlen(s.s);

		/* Out-of-range requests yield the empty string rather than an
		 * error -- that is what callers expect, and it keeps
		 * `substr "$s" 1 3` safe on a short string. */
		if (!vp.is_num || !vl.is_num) {
			out = make_str("");
		} else {
			start = vp.n;
			len = vl.n;
			if (start < 1 || start > slen || len <= 0) {
				out = make_str("");
			} else {
				if (start - 1 + len > slen)
					len = slen - (start - 1);
				char *sub = xmalloc((size_t)len + 1);

				memcpy(sub, s.s + start - 1, (size_t)len);
				sub[len] = '\0';
				out = make_str(sub);
				free(sub);
			}
		}
		free_value(&s);
		free_value(&vp);
		free_value(&vl);
		return out;
	}

	if (strcmp(t, "match") == 0 && pos + 2 < nargs) {
		value_t s, p, out;

		pos++;
		s = parse_primary();
		p = parse_primary();
		out = do_match(&s, &p);
		free_value(&s);
		free_value(&p);
		return out;
	}

	return make_str(next());
}

static value_t parse_regex(void)
{
	value_t left = parse_primary();

	while (peek() && strcmp(peek(), ":") == 0) {
		value_t right, out;

		pos++;
		right = parse_primary();
		out = do_match(&left, &right);
		free_value(&left);
		free_value(&right);
		left = out;
	}
	return left;
}

static value_t parse_mul(void)
{
	value_t left = parse_regex();

	for (;;) {
		const char *op = peek();

		if (!op || (strcmp(op, "*") && strcmp(op, "/") && strcmp(op, "%")))
			break;
		pos++;
		value_t right = parse_regex();
		long long a = need_num(&left), b = need_num(&right);
		long long r;

		if (op[0] == '*') {
			r = a * b;
		} else {
			if (b == 0)
				die(EXPR_INVALID, "division by zero");
			/* LLONG_MIN / -1 overflows; it is the one division that
			 * traps rather than returning a wrong answer. */
			if (a == LLONG_MIN && b == -1)
				die(EXPR_INVALID, "integer overflow");
			r = op[0] == '/' ? a / b : a % b;
		}
		free_value(&left);
		free_value(&right);
		left = make_num(r);
	}
	return left;
}

static value_t parse_add(void)
{
	value_t left = parse_mul();

	for (;;) {
		const char *op = peek();

		if (!op || (strcmp(op, "+") && strcmp(op, "-")))
			break;
		pos++;
		value_t right = parse_mul();
		long long a = need_num(&left), b = need_num(&right);

		free_value(&left);
		free_value(&right);
		left = make_num(op[0] == '+' ? a + b : a - b);
	}
	return left;
}

static value_t parse_cmp(void)
{
	value_t left = parse_add();

	for (;;) {
		const char *op = peek();
		int r;

		if (!op)
			break;
		if (strcmp(op, "=") && strcmp(op, "!=") && strcmp(op, "<") &&
		    strcmp(op, "<=") && strcmp(op, ">") && strcmp(op, ">="))
			break;
		pos++;
		value_t right = parse_add();

		/* Numeric comparison only when BOTH sides are integers;
		 * otherwise lexical.  So `expr 10 '>' 9` is 1 but
		 * `expr 10 '>' 9a` compares "10" against "9a" and is 0. */
		if (left.is_num && right.is_num) {
			long long a = left.n, b = right.n;

			r = a < b ? -1 : (a > b ? 1 : 0);
		} else {
			int c = strcmp(left.s, right.s);

			r = c < 0 ? -1 : (c > 0 ? 1 : 0);
		}

		int res;

		if (!strcmp(op, "="))
			res = r == 0;
		else if (!strcmp(op, "!="))
			res = r != 0;
		else if (!strcmp(op, "<"))
			res = r < 0;
		else if (!strcmp(op, "<="))
			res = r <= 0;
		else if (!strcmp(op, ">"))
			res = r > 0;
		else
			res = r >= 0;

		free_value(&left);
		free_value(&right);
		left = make_num(res);
	}
	return left;
}

static value_t parse_and(void)
{
	value_t left = parse_cmp();

	while (peek() && strcmp(peek(), "&") == 0) {
		pos++;
		value_t right = parse_cmp();

		/* Both sides are evaluated -- expr has no short-circuit, and a
		 * syntax error on the right is reported even when the left is
		 * already false. */
		if (value_true(&left) && value_true(&right)) {
			free_value(&right);
			continue; /* left is the result, unchanged */
		}
		free_value(&left);
		free_value(&right);
		left = make_num(0);
	}
	return left;
}

/*
 * `|` is looser than `&`, so an or-expression is a sequence of and-expressions.
 *
 * Neither operator short-circuits: expr evaluates both sides, so a syntax
 * error on the right is reported even when the left already decides the
 * result.  That is POSIX, and it is what makes `expr 1 \| bogus` an error
 * rather than 1.
 */
static value_t parse_or(void)
{
	value_t left = parse_and();

	while (peek() && strcmp(peek(), "|") == 0) {
		pos++;
		value_t right = parse_and();

		if (value_true(&left)) {
			free_value(&right);
			continue;
		}
		/* `a | b` yields b when a is false -- whatever b is, including
		 * a false b, which is how the whole expression ends up false. */
		free_value(&left);
		left = right;
	}
	return left;
}

int main(int argc, char *argv[])
{
	value_t result;
	int status;

	if (argc == 2 && strcmp(argv[1], "--help") == 0) {
		printf("Usage: expr EXPRESSION\n"
		       "   or: expr OPTION\n\n"
		       "      --help      display this help and exit\n"
		       "      --version   output version information and exit\n\n"
		       "Print the value of EXPRESSION to standard output.\n"
		       "Exit status is 0 if EXPRESSION is neither null nor 0, 1 if it is,\n"
		       "2 if EXPRESSION is syntactically invalid, and 3 if an error occurred.\n");
		return EXPR_TRUE;
	}
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("%s %s\n", PROGRAM_NAME, VERSION);
		return EXPR_TRUE;
	}
	if (argc < 2)
		die(EXPR_INVALID, "missing operand");

	args = argv + 1;
	nargs = argc - 1;

	result = parse_or();

	if (pos != nargs)
		die(EXPR_INVALID, "syntax error: unexpected argument '%s'",
		    args[pos]);

	puts(result.s);
	status = value_true(&result) ? EXPR_TRUE : EXPR_FALSE;
	free_value(&result);

	if (fflush(stdout) != 0) {
		fprintf(stderr, "expr: write error: %s\n", strerror(errno));
		return EXPR_ERROR;
	}
	return status;
}
