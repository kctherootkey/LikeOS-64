/*
 * Differential test of libc's string functions against the host's glibc.
 *
 * Runs on the BUILD machine, not the target: it compiles user/lib/libc's
 * string.c under renamed symbols and, for every function, compares the result
 * and the exact bytes written against glibc given identical inputs.
 *
 * The point is the second half of that.  A string function that returns the
 * right value while writing one byte too many is a corruptor of whatever
 * happens to sit after the destination -- a heap chunk header, the next field
 * of a struct -- and it shows up arbitrarily far away, as a crash in code that
 * did nothing wrong.  Exactly that bug lived in strncpy and strncat here (they
 * wrote a byte past the end whenever the source was shorter than n), and it was
 * this comparison that found it.  So every destination is placed against a
 * guard page with a poison run in front of it: a write past the end faults on
 * the instruction that makes it, and a write before the start is caught by the
 * poison check.
 *
 * Build and run:  ./host/test-string.sh
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- the implementation under test -------------------------------------
 *
 * string.c is included with every public name redefined to a lk_ prefix, so
 * that both implementations are callable from one program.  It is the real
 * source file, not a copy -- a copy would drift.
 */
#define memcpy lk_memcpy
#define memmove lk_memmove
#define memset lk_memset
#define memcmp lk_memcmp
#define memchr lk_memchr
#define strlen lk_strlen
#define strcpy lk_strcpy
#define strncpy lk_strncpy
#define strcat lk_strcat
#define strncat lk_strncat
#define strcmp lk_strcmp
#define strncmp lk_strncmp
#define strchr lk_strchr
#define strrchr lk_strrchr
#define strstr lk_strstr
#define strdup lk_strdup
#define strtok_r lk_strtok_r
#define strtok lk_strtok
#define strerror lk_strerror
#define strlcpy lk_strlcpy
#define strlcat lk_strlcat
#define strsep lk_strsep
#define strsignal lk_strsignal
#define strerror_r lk_strerror_r
#define memmem lk_memmem
#define strverscmp lk_strverscmp
#define strcoll lk_strcoll
#define strxfrm lk_strxfrm
#define strnlen lk_strnlen
#define ffs lk_ffs
#define ffsl lk_ffsl
#define ffsll lk_ffsll
#define strcasecmp lk_strcasecmp
#define strncasecmp lk_strncasecmp
#define strchrnul lk_strchrnul
#define stpcpy lk_stpcpy
#define stpncpy lk_stpncpy
#define strpbrk lk_strpbrk
#define strspn lk_strspn
#define strcspn lk_strcspn
#define rawmemchr lk_rawmemchr
#define memrchr lk_memrchr
#define bcopy lk_bcopy
#define bzero lk_bzero
#define bcmp lk_bcmp
#define index lk_index
#define rindex lk_rindex
#define mempcpy lk_mempcpy

#include "../user/lib/libc/src/string/string.c"
/* strnlen.c carries the rest of the family: strnlen, strspn, strcspn,
 * strpbrk, memrchr, stpcpy, stpncpy, strcasecmp, strncasecmp. */
#include "../user/lib/libc/src/string/strnlen.c"

#undef memcpy
#undef memmove
#undef memset
#undef memcmp
#undef memchr
#undef strlen
#undef strcpy
#undef strncpy
#undef strcat
#undef strncat
#undef strcmp
#undef strncmp
#undef strchr
#undef strrchr
#undef strstr
#undef strdup
#undef strtok_r
#undef strtok
#undef strerror
#undef strlcpy
#undef strlcat
#undef strsep
#undef strsignal
#undef strerror_r
#undef memmem
#undef strverscmp
#undef strcoll
#undef strxfrm
#undef strnlen
#undef ffs
#undef ffsl
#undef ffsll
#undef strcasecmp
#undef strncasecmp
#undef strchrnul
#undef stpcpy
#undef stpncpy
#undef strpbrk
#undef strspn
#undef strcspn
#undef rawmemchr
#undef memrchr
#undef bcopy
#undef bzero
#undef bcmp
#undef index
#undef rindex
#undef mempcpy

int likeos_errno; /* see the -Derrno= in host/test-string.sh */

/* ---- scoreboard -------------------------------------------------------- */

static int g_checks;
static int g_fail;
static const char *g_fn = "?";

static void fail(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...)
{
	va_list ap;
	g_fail++;
	fprintf(stderr, "  FAIL %-12s ", g_fn);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define CHECK(cond, ...)                     \
	do {                                 \
		g_checks++;                  \
		if (!(cond))                 \
			fail(__VA_ARGS__);   \
	} while (0)

/* ---- guarded buffers ---------------------------------------------------
 *
 * Two pages, the second one unmapped.  A buffer of n bytes is placed so that
 * its last byte is the last byte of the first page: a write to buf[n] lands in
 * the guard page and dies immediately, with the faulting instruction in the
 * offending function rather than a corrupted structure noticed later.
 *
 * The bytes ahead of the buffer are filled with a poison pattern and checked
 * afterwards, which catches the other direction -- a write before the start.
 */
#define GUARD_POISON 0xA5
#define GUARD_LEAD 64

typedef struct {
	unsigned char *base; /* start of the two-page mapping */
	unsigned char *buf; /* the n usable bytes */
	size_t n;
} guarded_t;

static size_t g_pagesz;

static void guarded_alloc(guarded_t *g, size_t n)
{
	size_t span = 2 * g_pagesz;
	unsigned char *p = mmap(NULL, span, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(p != MAP_FAILED);
	/* Make the second page inaccessible: this is the wall. */
	assert(mprotect(p + g_pagesz, g_pagesz, PROT_NONE) == 0);
	assert(n + GUARD_LEAD <= g_pagesz);

	g->base = p;
	g->n = n;
	g->buf = p + g_pagesz - n; /* flush against the guard */
	memset(p, GUARD_POISON, g_pagesz);
}

static void guarded_check_lead(guarded_t *g)
{
	for (size_t i = 1; i <= GUARD_LEAD; i++)
		CHECK(g->buf[-(ptrdiff_t)i] == GUARD_POISON,
		      "wrote %zu byte(s) BEFORE the destination", i);
}

static void guarded_free(guarded_t *g)
{
	munmap(g->base, 2 * g_pagesz);
}

/*
 * Run one destination-writing operation twice -- once through glibc, once
 * through ours -- each into its own guarded buffer of exactly `dstlen` bytes,
 * and compare every byte of the result.
 *
 * `op` receives the buffer and reports the function's return value; a
 * mismatched return is reported separately from mismatched bytes, because they
 * mean different things: the first is a wrong answer, the second is damage.
 */
typedef unsigned long (*writer_fn)(char *dst, size_t dstlen, void *ctx);

static void diff_write(const char *name, size_t dstlen, unsigned char fillbyte,
		       writer_fn ref, writer_fn ours, void *ctx)
{
	guarded_t a, b;
	g_fn = name;

	guarded_alloc(&a, dstlen);
	guarded_alloc(&b, dstlen);
	memset(a.buf, fillbyte, dstlen);
	memset(b.buf, fillbyte, dstlen);

	unsigned long ra = ref((char *)a.buf, dstlen, ctx);
	unsigned long rb = ours((char *)b.buf, dstlen, ctx);

	CHECK(ra == rb, "return %lu, glibc %lu", rb, ra);

	for (size_t i = 0; i < dstlen; i++)
		if (a.buf[i] != b.buf[i]) {
			fail("byte %zu of %zu: 0x%02x, glibc 0x%02x", i, dstlen,
			     b.buf[i], a.buf[i]);
			break;
		}

	guarded_check_lead(&a);
	guarded_check_lead(&b);
	guarded_free(&a);
	guarded_free(&b);
}

/* ---- the operations ---------------------------------------------------- */

struct sn {
	const char *src;
	size_t n;
};

static unsigned long op_strncpy_ref(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	(void)dl;
	strncpy(d, s->src, s->n);
	return 0;
}
static unsigned long op_strncpy_lk(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	(void)dl;
	lk_strncpy(d, s->src, s->n);
	return 0;
}

static unsigned long op_stpncpy_ref(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	(void)dl;
	return (unsigned long)(stpncpy(d, s->src, s->n) - d);
}
static unsigned long op_stpncpy_lk(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	(void)dl;
	return (unsigned long)(lk_stpncpy(d, s->src, s->n) - d);
}

/* strncat/strlcat append, so the destination starts as a real string. */
struct cat {
	const char *pre;
	const char *src;
	size_t n;
};

static unsigned long op_strncat_ref(char *d, size_t dl, void *c)
{
	struct cat *s = c;
	memset(d, 0, dl);
	strcpy(d, s->pre);
	strncat(d, s->src, s->n);
	return 0;
}
static unsigned long op_strncat_lk(char *d, size_t dl, void *c)
{
	struct cat *s = c;
	memset(d, 0, dl);
	strcpy(d, s->pre);
	lk_strncat(d, s->src, s->n);
	return 0;
}

static unsigned long op_strlcpy_lk(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	(void)dl;
	return lk_strlcpy(d, s->src, s->n);
}
static unsigned long op_strlcat_lk(char *d, size_t dl, void *c)
{
	struct cat *s = c;
	memset(d, 0, dl);
	strcpy(d, s->pre);
	return lk_strlcat(d, s->src, s->n);
}

/* BSD strlcpy/strlcat are not in glibc; model them exactly as the manual
 * specifies so there is still a reference to differ against. */
static unsigned long op_strlcpy_ref(char *d, size_t dl, void *c)
{
	struct sn *s = c;
	size_t srclen = strlen(s->src);
	(void)dl;
	if (s->n) {
		size_t copy = srclen < s->n - 1 ? srclen : s->n - 1;
		memcpy(d, s->src, copy);
		d[copy] = '\0';
	}
	return srclen;
}
static unsigned long op_strlcat_ref(char *d, size_t dl, void *c)
{
	struct cat *s = c;
	memset(d, 0, dl);
	strcpy(d, s->pre);
	size_t dlen = strnlen(d, s->n);
	size_t srclen = strlen(s->src);
	if (dlen == s->n)
		return s->n + srclen;
	size_t room = s->n - dlen - 1;
	size_t copy = srclen < room ? srclen : room;
	memcpy(d + dlen, s->src, copy);
	d[dlen + copy] = '\0';
	return dlen + srclen;
}

static unsigned long op_memset_ref(char *d, size_t dl, void *c)
{
	memset(d, *(int *)c, dl);
	return 0;
}
static unsigned long op_memset_lk(char *d, size_t dl, void *c)
{
	lk_memset(d, *(int *)c, dl);
	return 0;
}

static unsigned long op_memcpy_ref(char *d, size_t dl, void *c)
{
	memcpy(d, c, dl);
	return 0;
}
static unsigned long op_memcpy_lk(char *d, size_t dl, void *c)
{
	lk_memcpy(d, c, dl);
	return 0;
}

/* ---- comparison-only functions ----------------------------------------- */

static int sgn(int v)
{
	return v < 0 ? -1 : v > 0 ? 1 : 0;
}

static const char *const WORDS[] = {
	"",	   "a",	     "A",	 "ab",	     "abc",
	"abcd",	   "abcde",  "abcdef",	 "abcdefg",  "abcdefgh",
	"abcdefghi", "aXc",  "abd",	 "abC",	     "zzz",
	"ab\177",  "\200\201", "\xff\xfe", "hello world", "hello",
	"world",   "  ",     "\t\n",	 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab",
};
#define NWORDS (sizeof(WORDS) / sizeof(WORDS[0]))

static void test_compares(void)
{
	for (size_t i = 0; i < NWORDS; i++) {
		const char *a = WORDS[i];

		g_fn = "strlen";
		CHECK(lk_strlen(a) == strlen(a), "\"%s\": %zu, glibc %zu", a,
		      lk_strlen(a), strlen(a));

		for (size_t n = 0; n <= 12; n++) {
			g_fn = "strnlen";
			CHECK(lk_strnlen(a, n) == strnlen(a, n),
			      "\"%s\",%zu: %zu, glibc %zu", a, n,
			      lk_strnlen(a, n), strnlen(a, n));
		}

		for (int ch = 0; ch < 256; ch += 17) {
			g_fn = "strchr";
			CHECK(lk_strchr(a, ch) == strchr(a, ch),
			      "\"%s\",%d differs", a, ch);
			g_fn = "strrchr";
			CHECK(lk_strrchr(a, ch) == strrchr(a, ch),
			      "\"%s\",%d differs", a, ch);
			g_fn = "memchr";
			CHECK(lk_memchr(a, ch, strlen(a)) ==
				      memchr(a, ch, strlen(a)),
			      "\"%s\",%d differs", a, ch);
			g_fn = "memrchr";
			CHECK(lk_memrchr(a, ch, strlen(a)) ==
				      memrchr(a, ch, strlen(a)),
			      "\"%s\",%d differs", a, ch);
		}

		for (size_t j = 0; j < NWORDS; j++) {
			const char *b = WORDS[j];

			g_fn = "strcmp";
			CHECK(sgn(lk_strcmp(a, b)) == sgn(strcmp(a, b)),
			      "\"%s\" vs \"%s\": %d, glibc %d", a, b,
			      sgn(lk_strcmp(a, b)), sgn(strcmp(a, b)));

			for (size_t n = 0; n <= 10; n++) {
				g_fn = "strncmp";
				CHECK(sgn(lk_strncmp(a, b, n)) ==
					      sgn(strncmp(a, b, n)),
				      "\"%s\" vs \"%s\",%zu: %d, glibc %d", a,
				      b, n, sgn(lk_strncmp(a, b, n)),
				      sgn(strncmp(a, b, n)));
			}

			g_fn = "strstr";
			CHECK(lk_strstr(a, b) == strstr(a, b),
			      "\"%s\" in \"%s\" differs", b, a);

			g_fn = "memmem";
			size_t la = strlen(a), lb = strlen(b);
			CHECK(lk_memmem(a, la, b, lb) == memmem(a, la, b, lb),
			      "\"%s\" in \"%s\" differs", b, a);

			g_fn = "memcmp";
			size_t n = la < lb ? la : lb;
			CHECK(sgn(lk_memcmp(a, b, n)) == sgn(memcmp(a, b, n)),
			      "\"%s\" vs \"%s\",%zu differs", a, b, n);

			g_fn = "strspn";
			CHECK(lk_strspn(a, b) == strspn(a, b),
			      "\"%s\"/\"%s\": %zu, glibc %zu", a, b,
			      lk_strspn(a, b), strspn(a, b));

			g_fn = "strcspn";
			CHECK(lk_strcspn(a, b) == strcspn(a, b),
			      "\"%s\"/\"%s\": %zu, glibc %zu", a, b,
			      lk_strcspn(a, b), strcspn(a, b));

			g_fn = "strpbrk";
			CHECK(lk_strpbrk(a, b) == strpbrk(a, b),
			      "\"%s\"/\"%s\" differs", a, b);

			g_fn = "strcasecmp";
			CHECK(sgn(lk_strcasecmp(a, b)) == sgn(strcasecmp(a, b)),
			      "\"%s\" vs \"%s\": %d, glibc %d", a, b,
			      sgn(lk_strcasecmp(a, b)), sgn(strcasecmp(a, b)));

			for (size_t k = 0; k <= 10; k++) {
				g_fn = "strncasecmp";
				CHECK(sgn(lk_strncasecmp(a, b, k)) ==
					      sgn(strncasecmp(a, b, k)),
				      "\"%s\" vs \"%s\",%zu: %d, glibc %d", a,
				      b, k, sgn(lk_strncasecmp(a, b, k)),
				      sgn(strncasecmp(a, b, k)));
			}
		}
	}
}

/* memmove must be correct for every overlap, in both directions. */
static void test_memmove(void)
{
	enum { N = 64 };
	unsigned char ref[N], ours[N];
	g_fn = "memmove";

	for (int d = -16; d <= 16; d++) {
		for (size_t len = 0; len <= 24; len++) {
			size_t base = 20;
			if ((ptrdiff_t)base + d < 0)
				continue;
			for (int i = 0; i < N; i++)
				ref[i] = ours[i] = (unsigned char)(i * 7 + 3);

			memmove(ref + base + d, ref + base, len);
			lk_memmove(ours + base + d, ours + base, len);

			if (memcmp(ref, ours, N) != 0) {
				fail("overlap d=%d len=%zu differs", d, len);
				break;
			}
		}
	}
}

static void test_writers(void)
{
	static const char *srcs[] = { "",	"a",	   "abc",
				      "abcdefg", "abcdefgh",   "abcdefghi",
				      "abcdefghijklmnop" };

	for (size_t s = 0; s < sizeof(srcs) / sizeof(srcs[0]); s++) {
		for (size_t n = 0; n <= 20; n++) {
			/* The destination is EXACTLY n bytes: anything the
			 * function writes at dst[n] hits the guard page.  That
			 * is the whole contract of strncpy and friends, and
			 * where the off-by-one lived. */
			struct sn sn = { srcs[s], n };
			if (n > 0)
				diff_write("strncpy", n, 0xCC, op_strncpy_ref,
					   op_strncpy_lk, &sn);
			if (n > 0)
				diff_write("stpncpy", n, 0xCC, op_stpncpy_ref,
					   op_stpncpy_lk, &sn);
			if (n > 0)
				diff_write("strlcpy", n, 0xCC, op_strlcpy_ref,
					   op_strlcpy_lk, &sn);

			for (size_t p = 0; p < 4 && p + 1 < n; p++) {
				static const char *pres[] = { "", "x", "xy",
							      "xyz" };
				struct cat cat = { pres[p], srcs[s], n };
				/* n here is the size of the whole buffer, which
				 * is what strlcat is given and what strncat is
				 * NOT -- strncat's n counts only the appended
				 * bytes, so its buffer needs room for the
				 * prefix and the terminator as well. */
				diff_write("strlcat", n, 0xCC, op_strlcat_ref,
					   op_strlcat_lk, &cat);

				size_t need = strlen(pres[p]) + n + 1;
				diff_write("strncat", need, 0xCC,
					   op_strncat_ref, op_strncat_lk, &cat);
			}
		}
	}

	for (int c = 0; c < 256; c += 37)
		for (size_t n = 1; n <= 40; n++)
			diff_write("memset", n, 0x11, op_memset_ref,
				   op_memset_lk, &c);

	static char pattern[64];
	for (size_t i = 0; i < sizeof(pattern); i++)
		pattern[i] = (char)(i * 13 + 1);
	for (size_t n = 1; n <= 40; n++)
		diff_write("memcpy", n, 0x22, op_memcpy_ref, op_memcpy_lk,
			   pattern);
}

/* strtok_r and strsep rewrite their input in place; check the buffer as well
 * as the sequence of tokens. */
static void test_tokenizers(void)
{
	static const char *inputs[] = {
		"",	    "a",	"a,b,c",     ",a,,b,",	 ",,,",
		"a,,b",	    "  a  b  ",	"a",	     "a,",	 ",a",
	};
	static const char *delims[] = { ",", " ", ",;", "" };

	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		for (size_t d = 0; d < sizeof(delims) / sizeof(delims[0]); d++) {
			char ba[64], bb[64];
			char *sa = NULL, *sb = NULL;
			snprintf(ba, sizeof(ba), "%s", inputs[i]);
			snprintf(bb, sizeof(bb), "%s", inputs[i]);

			g_fn = "strtok_r";
			char *pa = ba, *pb = bb;
			for (int k = 0; k < 8; k++) {
				char *ta = strtok_r(pa, delims[d], &sa);
				char *tb = lk_strtok_r(pb, delims[d], &sb);
				pa = pb = NULL;
				if (!ta || !tb) {
					CHECK(!ta == !tb,
					      "\"%s\"/\"%s\" token %d: %s vs glibc %s",
					      inputs[i], delims[d], k,
					      tb ? tb : "(null)",
					      ta ? ta : "(null)");
					break;
				}
				CHECK(strcmp(ta, tb) == 0,
				      "\"%s\"/\"%s\" token %d: \"%s\", glibc \"%s\"",
				      inputs[i], delims[d], k, tb, ta);
			}

			g_fn = "strsep";
			char ca[64], cb[64];
			snprintf(ca, sizeof(ca), "%s", inputs[i]);
			snprintf(cb, sizeof(cb), "%s", inputs[i]);
			char *qa = ca, *qb = cb;
			for (int k = 0; k < 8; k++) {
				char *ta = strsep(&qa, delims[d]);
				char *tb = lk_strsep(&qb, delims[d]);
				if (!ta || !tb) {
					CHECK(!ta == !tb,
					      "\"%s\"/\"%s\" field %d differs",
					      inputs[i], delims[d], k);
					break;
				}
				CHECK(strcmp(ta, tb) == 0,
				      "\"%s\"/\"%s\" field %d: \"%s\", glibc \"%s\"",
				      inputs[i], delims[d], k, tb, ta);
			}
		}
	}
}

int main(void)
{
	g_pagesz = (size_t)sysconf(_SC_PAGESIZE);

	printf("libc string functions vs glibc\n");
	test_compares();
	test_memmove();
	test_writers();
	test_tokenizers();

	printf("%d checks, %d failure%s\n", g_checks, g_fail,
	       g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
