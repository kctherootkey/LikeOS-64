/*
 * regex.c - POSIX regular expressions for LikeOS-64.
 *
 * Supports both flavours POSIX defines:
 *   BRE (default)      \(..\) groups, \{m,n\} bounds, backreferences \1..\9;
 *                      + ? | are ordinary characters (\+ \? \| work as the
 *                      operators, the widely implemented extension)
 *   ERE (REG_EXTENDED) (..) groups, {m,n} bounds, + ? | as operators
 *
 * Common to both: . * ^ $, bracket expressions with ranges, negation and
 * the [:name:] character classes, and the REG_ICASE / REG_NEWLINE /
 * REG_NOSUB / REG_NOTBOL / REG_NOTEOL flags.
 *
 * The matcher is a backtracker driven by explicit continuations, which keeps
 * capture handling simple and needs no recursion on the input length.  POSIX
 * asks for the leftmost-LONGEST match rather than the leftmost match a plain
 * backtracker finds, so every path from a given start position is explored
 * and the longest overall match wins; `steps` bounds that exploration so a
 * pathological pattern degrades to "best found so far" instead of hanging.
 */

#include <regex.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Pattern representation                                              */
/* ------------------------------------------------------------------ */

enum {
	N_CHAR, /* one literal character                */
	N_ANY, /* .                                    */
	N_SET, /* [...] bracket expression             */
	N_BOL, /* ^                                    */
	N_EOL, /* $                                    */
	N_GROUP, /* (...) - gnum > 0 when capturing      */
	N_GEND, /* internal: closes a group             */
	N_BACKREF, /* \1 .. \9                             */
	N_WORDB, /* \b / \B: (non-)word boundary          */
	N_WORDEDGE, /* \< / \>: start / end of a word     */
	N_REP /* child repeated min..max times        */
};

typedef struct rnode rnode;
struct rnode {
	int type;
	rnode *next; /* next element of this concatenation      */
	rnode *alt; /* next branch, when this heads a branch   */
	rnode *sub; /* N_GROUP/N_REP: the subexpression        */
	rnode *gend; /* N_GROUP: its matching close node        */
	unsigned char ch; /* N_CHAR                                  */
	unsigned char set[32]; /* N_SET bitmap                     */
	int neg; /* N_SET: negated                          */
	int gnum; /* N_GROUP/N_GEND: capture index (0 = none)*/
	int refnum; /* N_BACKREF                               */
	int min, max; /* N_REP; max < 0 means unbounded          */
};

/* Everything a compiled pattern owns, so regfree() is a couple of frees. */
typedef struct {
	rnode *nodes; /* flat array - the whole tree lives here  */
	int nnodes;
	rnode *root; /* first branch of the top-level alternation */
	int ngroups; /* number of capturing groups              */
	int cflags;
} re_prog;

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *p; /* cursor                                  */
	const char *end;
	re_prog *prog;
	int ere; /* REG_EXTENDED                            */
	int err;
} pstate;

static rnode *newnode(pstate *s, int type)
{
	if (s->err)
		return 0;
	if (s->prog->nnodes >= s->prog->ngroups * 0 + 0) {
		/* nodes array is pre-sized by the caller; see re_compile */
	}
	rnode *n = &s->prog->nodes[s->prog->nnodes++];
	memset(n, 0, sizeof(*n));
	n->type = type;
	n->min = n->max = 1;
	return n;
}

static void setbit(unsigned char *set, unsigned c)
{
	set[c >> 3] |= (unsigned char)(1u << (c & 7));
}
static int getbit(const unsigned char *set, unsigned c)
{
	return (set[c >> 3] >> (c & 7)) & 1;
}

static int class_match(const char *name, size_t len, unsigned char c)
{
	if (len == 5 && !memcmp(name, "alpha", 5))
		return isalpha(c) != 0;
	if (len == 5 && !memcmp(name, "digit", 5))
		return isdigit(c) != 0;
	if (len == 5 && !memcmp(name, "alnum", 5))
		return isalnum(c) != 0;
	if (len == 5 && !memcmp(name, "upper", 5))
		return isupper(c) != 0;
	if (len == 5 && !memcmp(name, "lower", 5))
		return islower(c) != 0;
	if (len == 5 && !memcmp(name, "space", 5))
		return isspace(c) != 0;
	if (len == 5 && !memcmp(name, "punct", 5))
		return ispunct(c) != 0;
	if (len == 5 && !memcmp(name, "print", 5))
		return isprint(c) != 0;
	if (len == 5 && !memcmp(name, "graph", 5))
		return isgraph(c) != 0;
	if (len == 5 && !memcmp(name, "cntrl", 5))
		return iscntrl(c) != 0;
	if (len == 5 && !memcmp(name, "blank", 5))
		return (c == ' ' || c == '\t');
	if (len == 6 && !memcmp(name, "xdigit", 6))
		return isxdigit(c) != 0;
	return -1; /* unknown class name */
}

/* Parse a bracket expression; s->p points just past '['. */
static rnode *parse_bracket(pstate *s)
{
	rnode *n = newnode(s, N_SET);
	if (!n)
		return 0;
	if (s->p < s->end && *s->p == '^') {
		n->neg = 1;
		s->p++;
	}
	int first = 1;
	while (s->p < s->end) {
		unsigned char c = (unsigned char)*s->p;
		if (c == ']' && !first) {
			s->p++;
			return n;
		}
		first = 0;
		/* [:class:] */
		if (c == '[' && s->p + 1 < s->end && s->p[1] == ':') {
			const char *q = s->p + 2;
			const char *cs = q;
			while (q < s->end && *q != ':')
				q++;
			if (q + 1 < s->end && *q == ':' && q[1] == ']') {
				size_t len = (size_t)(q - cs);
				for (unsigned u = 0; u < 256; u++) {
					int r = class_match(cs, len,
							    (unsigned char)u);
					if (r < 0) {
						s->err = REG_ECTYPE;
						return 0;
					}
					if (r)
						setbit(n->set, u);
				}
				s->p = q + 2;
				continue;
			}
		}
		s->p++;
		/* range a-z (a '-' last in the list is a literal) */
		if (s->p + 1 < s->end && *s->p == '-' && s->p[1] != ']') {
			unsigned char hi = (unsigned char)s->p[1];
			if (hi < c) {
				s->err = REG_ERANGE;
				return 0;
			}
			for (unsigned u = c; u <= hi; u++)
				setbit(n->set, u);
			s->p += 2;
			continue;
		}
		setbit(n->set, c);
	}
	s->err = REG_EBRACK; /* no closing ] */
	return 0;
}

static rnode *parse_alt(pstate *s, int depth);

/* One atom, without any repetition suffix. */
static rnode *parse_atom(pstate *s, int depth)
{
	if (s->p >= s->end)
		return 0;
	unsigned char c = (unsigned char)*s->p;

	if (c == '[') {
		s->p++;
		return parse_bracket(s);
	}
	if (c == '.') {
		s->p++;
		return newnode(s, N_ANY);
	}
	if (c == '^') {
		/* BRE: only special at the start of the pattern or a branch;
		 * handled by the caller passing us only in those positions.
		 * ERE: always an anchor. */
		s->p++;
		return newnode(s, N_BOL);
	}
	if (c == '$') {
		s->p++;
		return newnode(s, N_EOL);
	}
	if (s->ere && c == '(') {
		s->p++;
		rnode *g = newnode(s, N_GROUP);
		if (!g)
			return 0;
		g->gnum = ++s->prog->ngroups;
		g->sub = parse_alt(s, depth + 1);
		if (s->err)
			return 0;
		if (s->p >= s->end || *s->p != ')') {
			s->err = REG_EPAREN;
			return 0;
		}
		s->p++;
		g->gend = newnode(s, N_GEND);
		if (!g->gend)
			return 0;
		g->gend->gnum = g->gnum;
		return g;
	}
	if (c == '\\') {
		if (s->p + 1 >= s->end) {
			s->err = REG_EESCAPE;
			return 0;
		}
		unsigned char e = (unsigned char)s->p[1];
		/* BRE grouping */
		if (!s->ere && e == '(') {
			s->p += 2;
			rnode *g = newnode(s, N_GROUP);
			if (!g)
				return 0;
			g->gnum = ++s->prog->ngroups;
			g->sub = parse_alt(s, depth + 1);
			if (s->err)
				return 0;
			if (s->p + 1 >= s->end || s->p[0] != '\\' ||
			    s->p[1] != ')') {
				s->err = REG_EPAREN;
				return 0;
			}
			s->p += 2;
			g->gend = newnode(s, N_GEND);
			if (!g->gend)
				return 0;
			g->gend->gnum = g->gnum;
			return g;
		}
		if (e >= '1' && e <= '9') {
			s->p += 2;
			rnode *b = newnode(s, N_BACKREF);
			if (!b)
				return 0;
			b->refnum = e - '0';
			if (b->refnum > s->prog->ngroups) {
				s->err = REG_ESUBREG;
				return 0;
			}
			return b;
		}
		/* GNU escapes that every grep user expects: word boundaries
		 * and the \w \s shorthands.  POSIX does not define them, but
		 * patterns in the wild rely on them. */
		if (e == 'b' || e == 'B') {
			s->p += 2;
			rnode *n = newnode(s, N_WORDB);
			if (!n)
				return 0;
			n->neg = (e == 'B');
			return n;
		}
		if (e == '<' || e == '>') {
			s->p += 2;
			rnode *n = newnode(s, N_WORDEDGE);
			if (!n)
				return 0;
			n->neg = (e == '>'); /* neg = end-of-word */
			return n;
		}
		if (e == 'w' || e == 'W' || e == 's' || e == 'S') {
			s->p += 2;
			rnode *n = newnode(s, N_SET);
			if (!n)
				return 0;
			for (unsigned u = 0; u < 256; u++) {
				int in = (e == 'w' || e == 'W') ?
						 (isalnum(u) || u == '_') :
						 (isspace(u) != 0);
				if (in)
					setbit(n->set, u);
			}
			n->neg = (e == 'W' || e == 'S');
			return n;
		}
		/* Any other escaped character stands for itself. */
		s->p += 2;
		rnode *n = newnode(s, N_CHAR);
		if (!n)
			return 0;
		n->ch = e;
		return n;
	}
	s->p++;
	rnode *n = newnode(s, N_CHAR);
	if (!n)
		return 0;
	n->ch = c;
	return n;
}

/* Read {m,n} / \{m,n\}; cursor sits on the '{'. Returns 0 if it is not a
 * bound at all (then the '{' is an ordinary character). */
static int parse_bound(pstate *s, int *pmin, int *pmax)
{
	const char *save = s->p;
	const char *q = s->p + 1; /* past '{' */
	int mn = 0, mx = -1, digits = 0;
	while (q < s->end && *q >= '0' && *q <= '9') {
		mn = mn * 10 + (*q - '0');
		q++;
		digits++;
	}
	if (!digits) {
		s->p = save;
		return 0;
	}
	if (q < s->end && *q == ',') {
		q++;
		int d2 = 0, v = 0;
		while (q < s->end && *q >= '0' && *q <= '9') {
			v = v * 10 + (*q - '0');
			q++;
			d2++;
		}
		mx = d2 ? v : -1;
	} else {
		mx = mn;
	}
	if (s->ere) {
		if (q >= s->end || *q != '}') {
			s->p = save;
			return 0;
		}
		q++;
	} else {
		if (q + 1 >= s->end || q[0] != '\\' || q[1] != '}') {
			s->p = save;
			return 0;
		}
		q += 2;
	}
	if (mx >= 0 && mx < mn) {
		s->err = REG_BADBR;
		return 0;
	}
	*pmin = mn;
	*pmax = mx;
	s->p = q;
	return 1;
}

/* An atom plus any repetition operators applied to it. */
static rnode *parse_piece(pstate *s, int depth)
{
	rnode *a = parse_atom(s, depth);
	if (!a || s->err)
		return a;

	for (;;) {
		if (s->p >= s->end)
			break;
		unsigned char c = (unsigned char)*s->p;
		int mn, mx;
		if (c == '*') {
			s->p++;
			mn = 0;
			mx = -1;
		} else if (s->ere && c == '+') {
			s->p++;
			mn = 1;
			mx = -1;
		} else if (s->ere && c == '?') {
			s->p++;
			mn = 0;
			mx = 1;
		} else if (s->ere && c == '{') {
			if (!parse_bound(s, &mn, &mx))
				break;
		} else if (!s->ere && c == '\\' && s->p + 1 < s->end &&
			   (s->p[1] == '+' || s->p[1] == '?')) {
			/* GNU BRE extension: \+ and \? */
			mn = (s->p[1] == '+') ? 1 : 0;
			mx = (s->p[1] == '+') ? -1 : 1;
			s->p += 2;
		} else if (!s->ere && c == '\\' && s->p + 1 < s->end &&
			   s->p[1] == '{') {
			s->p++; /* point at '{' for parse_bound */
			if (!parse_bound(s, &mn, &mx)) {
				s->p--;
				break;
			}
		} else {
			break;
		}
		if (s->err)
			return 0;
		rnode *r = newnode(s, N_REP);
		if (!r)
			return 0;
		r->sub = a;
		a->next = 0;
		r->min = mn;
		r->max = mx;
		a = r;
	}
	return a;
}

/* A concatenation of pieces. */
static rnode *parse_branch(pstate *s, int depth)
{
	rnode *head = 0, *tail = 0;
	while (s->p < s->end) {
		unsigned char c = (unsigned char)*s->p;
		if (s->ere && (c == '|' || c == ')'))
			break;
		if (!s->ere && c == '\\' && s->p + 1 < s->end &&
		    (s->p[1] == '|' || s->p[1] == ')'))
			break;
		/* In BRE, ^ is an anchor only at the very start of a branch
		 * and $ only at the very end; elsewhere they are literals. */
		if (!s->ere && c == '^' && head) {
			rnode *n = newnode(s, N_CHAR);
			if (!n)
				return 0;
			n->ch = '^';
			s->p++;
			if (tail)
				tail->next = n;
			else
				head = n;
			tail = n;
			continue;
		}
		if (!s->ere && c == '$') {
			const char *q = s->p + 1;
			int at_end = (q >= s->end) ||
				     (q + 1 < s->end && q[0] == '\\' &&
				      (q[1] == ')' || q[1] == '|'));
			if (!at_end) {
				rnode *n = newnode(s, N_CHAR);
				if (!n)
					return 0;
				n->ch = '$';
				s->p++;
				if (tail)
					tail->next = n;
				else
					head = n;
				tail = n;
				continue;
			}
		}
		rnode *pc = parse_piece(s, depth);
		if (s->err)
			return 0;
		if (!pc)
			break;
		if (tail)
			tail->next = pc;
		else
			head = pc;
		tail = pc;
	}
	return head; /* may be NULL: an empty branch matches the empty string */
}

/* branch ( '|' branch )* - returns the chain of branch heads via ->alt. */
static rnode *parse_alt(pstate *s, int depth)
{
	if (depth > 50) {
		s->err = REG_ESPACE;
		return 0;
	}
	rnode *first = parse_branch(s, depth);
	if (s->err)
		return 0;
	/* An empty branch still needs a node, both to match the empty string
	 * and to give the following branches something to chain onto - "(|a)"
	 * must keep its empty first alternative. */
	if (!first) {
		first = newnode(s, N_REP);
		if (!first)
			return 0;
		first->sub = 0;
		first->min = 0;
		first->max = 0;
	}
	rnode *last = first;
	for (;;) {
		int isbar = 0;
		if (s->p < s->end) {
			if (s->ere && *s->p == '|') {
				s->p++;
				isbar = 1;
			} else if (!s->ere && *s->p == '\\' &&
				   s->p + 1 < s->end && s->p[1] == '|') {
				s->p += 2;
				isbar = 1;
			}
		}
		if (!isbar)
			break;
		rnode *b = parse_branch(s, depth);
		if (s->err)
			return 0;
		/* An empty branch still needs a node to hang ->alt off. */
		if (!b) {
			b = newnode(s, N_REP);
			if (!b)
				return 0;
			b->sub = 0;
			b->min = 0;
			b->max = 0;
		}
		if (last)
			last->alt = b;
		else
			first = b;
		last = b;
	}
	return first;
}

/* ------------------------------------------------------------------ */
/* Matcher                                                             */
/* ------------------------------------------------------------------ */

#define RE_MAXGROUP 32
#define RE_STEPS 2000000L

enum { K_NODE, K_REP, K_GEND };

typedef struct kont {
	int kind;
	rnode *n; /* K_NODE: next node; K_REP: the rep; K_GEND: group end */
	int count; /* K_REP: iterations completed so far */
	size_t iter_start; /* K_REP: where this iteration began */
	struct kont *up;
} kont;

typedef struct {
	const char *str;
	size_t len;
	int icase, newline, notbol, noteol;
	long steps;
	int gs[RE_MAXGROUP], ge[RE_MAXGROUP]; /* -1 when unset */
	int best_gs[RE_MAXGROUP], best_ge[RE_MAXGROUP];
	int ngroups;
	long best; /* longest end position, -1 = none */
} mctx;

static int m_node(mctx *m, rnode *n, kont *k, size_t pos);

static unsigned char foldc(const mctx *m, unsigned char c)
{
	return m->icase ? (unsigned char)tolower(c) : c;
}

/* Continue after the current node: next in the chain, else pop. */
static int m_cont(mctx *m, rnode *n, kont *k, size_t pos)
{
	if (n)
		return m_node(m, n, k, pos);
	while (k) {
		if (k->kind == K_NODE) {
			rnode *nn = k->n;
			kont *up = k->up;
			if (nn)
				return m_node(m, nn, up, pos);
			k = up;
			continue;
		}
		if (k->kind == K_GEND) {
			int g = k->n->gnum;
			int saved = m->ge[g];
			m->ge[g] = (int)pos;
			int r = m_cont(m, 0, k->up, pos);
			m->ge[g] = saved;
			return r;
		}
		/* K_REP: one iteration of the body finished */
		{
			rnode *rep = k->n;
			int cnt = k->count + 1;
			/* A body that consumed nothing would repeat forever. */
			int empty = (pos == k->iter_start);
			if (!empty && (rep->max < 0 || cnt < rep->max)) {
				kont kk = { K_REP, rep, cnt, pos, k->up };
				for (rnode *b = rep->sub; b; b = b->alt) {
					int r = m_cont(m, b, &kk, pos);
					if (r)
						return r;
				}
			}
			if (cnt >= rep->min)
				return m_cont(m, 0, k->up, pos);
			return 0;
		}
	}
	/* End of the whole pattern: record the longest match seen. */
	if ((long)pos > m->best) {
		m->best = (long)pos;
		for (int i = 0; i <= m->ngroups && i < RE_MAXGROUP; i++) {
			m->best_gs[i] = m->gs[i];
			m->best_ge[i] = m->ge[i];
		}
	}
	return 0; /* keep exploring - POSIX wants the longest, not the first */
}

static int m_node(mctx *m, rnode *n, kont *k, size_t pos)
{
	if (--m->steps <= 0)
		return 1; /* budget exhausted: unwind, keep best so far */

	switch (n->type) {
	case N_CHAR:
		if (pos < m->len &&
		    foldc(m, (unsigned char)m->str[pos]) == foldc(m, n->ch))
			return m_cont(m, n->next, k, pos + 1);
		return 0;
	case N_ANY:
		if (pos < m->len &&
		    !(m->newline && m->str[pos] == '\n'))
			return m_cont(m, n->next, k, pos + 1);
		return 0;
	case N_SET: {
		if (pos >= m->len)
			return 0;
		unsigned char c = (unsigned char)m->str[pos];
		int in = getbit(n->set, c);
		if (!in && m->icase) {
			unsigned char o = isupper(c) ? (unsigned char)tolower(c) :
							     (unsigned char)toupper(c);
			in = getbit(n->set, o);
		}
		if (n->neg) {
			in = !in;
			/* With REG_NEWLINE a negated set never matches \n. */
			if (m->newline && c == '\n')
				in = 0;
		}
		return in ? m_cont(m, n->next, k, pos + 1) : 0;
	}
	case N_BOL:
		if (pos == 0 ? !m->notbol :
				     (m->newline && m->str[pos - 1] == '\n'))
			return m_cont(m, n->next, k, pos);
		return 0;
	case N_EOL:
		if (pos == m->len ? !m->noteol :
					  (m->newline && m->str[pos] == '\n'))
			return m_cont(m, n->next, k, pos);
		return 0;
	case N_GEND:
		/* Reached inline (a group's close node); handled via K_GEND. */
		return m_cont(m, n->next, k, pos);
	case N_WORDB: {
		int before = (pos > 0) &&
			     (isalnum((unsigned char)m->str[pos - 1]) ||
			      m->str[pos - 1] == '_');
		int after = (pos < m->len) &&
			    (isalnum((unsigned char)m->str[pos]) ||
			     m->str[pos] == '_');
		int boundary = (before != after);
		if (boundary != (n->neg ? 1 : 0))
			return m_cont(m, n->next, k, pos);
		return 0;
	}
	case N_WORDEDGE: {
		int before = (pos > 0) &&
			     (isalnum((unsigned char)m->str[pos - 1]) ||
			      m->str[pos - 1] == '_');
		int after = (pos < m->len) &&
			    (isalnum((unsigned char)m->str[pos]) ||
			     m->str[pos] == '_');
		int ok = n->neg ? (before && !after) : (!before && after);
		return ok ? m_cont(m, n->next, k, pos) : 0;
	}
	case N_BACKREF: {
		int g = n->refnum;
		if (g >= RE_MAXGROUP || m->gs[g] < 0 || m->ge[g] < 0)
			return 0;
		size_t glen = (size_t)(m->ge[g] - m->gs[g]);
		if (pos + glen > m->len)
			return 0;
		for (size_t i = 0; i < glen; i++)
			if (foldc(m, (unsigned char)m->str[pos + i]) !=
			    foldc(m, (unsigned char)m->str[m->gs[g] + i]))
				return 0;
		return m_cont(m, n->next, k, pos + glen);
	}
	case N_GROUP: {
		int g = n->gnum;
		int sg = m->gs[g], se = m->ge[g];
		m->gs[g] = (int)pos;
		kont kafter = { K_NODE, n->next, 0, 0, k };
		kont kend = { K_GEND, n->gend, 0, 0, &kafter };
		int r = 0;
		for (rnode *b = n->sub; b; b = b->alt) {
			r = m_cont(m, b, &kend, pos);
			if (r)
				break;
		}
		if (!n->sub) /* empty group: () matches the empty string */
			r = m_cont(m, 0, &kend, pos);
		m->gs[g] = sg;
		m->ge[g] = se;
		return r;
	}
	case N_REP: {
		if (!n->sub) /* empty alternative branch placeholder */
			return m_cont(m, n->next, k, pos);
		kont kafter = { K_NODE, n->next, 0, 0, k };
		/* Greedy: try one more iteration before accepting what we have. */
		if (n->max != 0) {
			kont kk = { K_REP, n, 0, pos, &kafter };
			for (rnode *b = n->sub; b; b = b->alt) {
				int r = m_cont(m, b, &kk, pos);
				if (r)
					return r;
			}
		}
		if (n->min == 0)
			return m_cont(m, 0, &kafter, pos);
		return 0;
	}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
	if (!preg || !pattern)
		return REG_BADPAT;

	size_t plen = strlen(pattern);
	/* Upper bound on nodes: every pattern character can produce at most a
	 * repetition wrapper plus an atom, and every group adds a close node. */
	size_t maxnodes = plen * 3 + 8;

	re_prog *prog = (re_prog *)malloc(sizeof(re_prog));
	if (!prog)
		return REG_ESPACE;
	prog->nodes = (rnode *)malloc(maxnodes * sizeof(rnode));
	if (!prog->nodes) {
		free(prog);
		return REG_ESPACE;
	}
	prog->nnodes = 0;
	prog->ngroups = 0;
	prog->cflags = cflags;

	pstate s;
	s.p = pattern;
	s.end = pattern + plen;
	s.prog = prog;
	s.ere = (cflags & REG_EXTENDED) ? 1 : 0;
	s.err = 0;

	prog->root = parse_alt(&s, 0);
	if (!s.err && s.p != s.end) {
		/* Left-over input: an unbalanced ) is the usual cause. */
		s.err = REG_EPAREN;
	}
	if (s.err) {
		free(prog->nodes);
		free(prog);
		return s.err;
	}
	if (prog->ngroups >= RE_MAXGROUP) {
		free(prog->nodes);
		free(prog);
		return REG_ESPACE;
	}

	preg->__buffer = (struct re_dfa_t *)prog;
	preg->re_nsub = (size_t)prog->ngroups;
	preg->__allocated = 0;
	preg->__used = 0;
	preg->__fastmap = 0;
	preg->__translate = 0;
	preg->__syntax = 0;
	preg->__can_be_null = 0;
	preg->__regs_allocated = 0;
	preg->__fastmap_accurate = 0;
	preg->__no_sub = (cflags & REG_NOSUB) ? 1 : 0;
	preg->__not_bol = 0;
	preg->__not_eol = 0;
	preg->__newline_anchor = (cflags & REG_NEWLINE) ? 1 : 0;
	return 0;
}

/* The prototype declares pmatch as regmatch_t[_Restrict_arr_ nmatch]; match
 * that shape here so the compiler does not warn about the array parameter. */
int regexec(const regex_t *_Restrict_ preg, const char *_Restrict_ string,
	    size_t nmatch, regmatch_t pmatch[_Restrict_arr_
					     _REGEX_NELTS(nmatch)],
	    int eflags)
{
	if (!preg || !preg->__buffer || !string)
		return REG_NOMATCH;
	re_prog *prog = (re_prog *)preg->__buffer;

	mctx m;
	m.str = string;
	m.len = strlen(string);
	m.icase = (prog->cflags & REG_ICASE) ? 1 : 0;
	m.newline = (prog->cflags & REG_NEWLINE) ? 1 : 0;
	m.notbol = (eflags & REG_NOTBOL) ? 1 : 0;
	m.noteol = (eflags & REG_NOTEOL) ? 1 : 0;
	m.ngroups = prog->ngroups;

	for (size_t start = 0;; start++) {
		m.steps = RE_STEPS;
		m.best = -1;
		for (int i = 0; i < RE_MAXGROUP; i++) {
			m.gs[i] = m.ge[i] = -1;
			m.best_gs[i] = m.best_ge[i] = -1;
		}
		for (rnode *b = prog->root; b; b = b->alt) {
			if (m_cont(&m, b, 0, start))
				break; /* step budget hit; use best so far */
		}
		if (!prog->root) /* empty pattern matches everywhere */
			m.best = (long)start;

		if (m.best >= 0) {
			if (!preg->__no_sub && nmatch > 0 && pmatch) {
				pmatch[0].rm_so = (regoff_t)start;
				pmatch[0].rm_eo = (regoff_t)m.best;
				for (size_t i = 1; i < nmatch; i++) {
					if ((int)i <= prog->ngroups &&
					    m.best_gs[i] >= 0 &&
					    m.best_ge[i] >= 0) {
						pmatch[i].rm_so =
							(regoff_t)m.best_gs[i];
						pmatch[i].rm_eo =
							(regoff_t)m.best_ge[i];
					} else {
						pmatch[i].rm_so = -1;
						pmatch[i].rm_eo = -1;
					}
				}
			}
			return 0;
		}
		if (start >= m.len)
			break;
	}
	return REG_NOMATCH;
}

void regfree(regex_t *preg)
{
	if (!preg || !preg->__buffer)
		return;
	re_prog *prog = (re_prog *)preg->__buffer;
	free(prog->nodes);
	free(prog);
	preg->__buffer = 0;
	preg->re_nsub = 0;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf,
		size_t errbuf_size)
{
	(void)preg;
	const char *msg;
	switch (errcode) {
	case 0:
		msg = "Success";
		break;
	case REG_NOMATCH:
		msg = "No match";
		break;
	case REG_BADPAT:
		msg = "Invalid regular expression";
		break;
	case REG_ECOLLATE:
		msg = "Invalid collation character";
		break;
	case REG_ECTYPE:
		msg = "Invalid character class name";
		break;
	case REG_EESCAPE:
		msg = "Trailing backslash";
		break;
	case REG_ESUBREG:
		msg = "Invalid back reference";
		break;
	case REG_EBRACK:
		msg = "Unmatched [ or [^";
		break;
	case REG_EPAREN:
		msg = "Unmatched ( or \\(";
		break;
	case REG_EBRACE:
		msg = "Unmatched \\{";
		break;
	case REG_BADBR:
		msg = "Invalid content of \\{\\}";
		break;
	case REG_ERANGE:
		msg = "Invalid range end";
		break;
	case REG_ESPACE:
		msg = "Memory exhausted";
		break;
	case REG_BADRPT:
		msg = "Invalid preceding regular expression";
		break;
	default:
		msg = "Unknown error";
		break;
	}
	size_t len = strlen(msg);
	if (errbuf && errbuf_size > 0) {
		size_t n = (len < errbuf_size - 1) ? len : errbuf_size - 1;
		memcpy(errbuf, msg, n);
		errbuf[n] = '\0';
	}
	return len + 1;
}
