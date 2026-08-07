/*
 * Differential test of libc's printf formatting against the host's glibc.
 *
 * Runs on the BUILD machine.  host/test-printf.sh compiles the real
 * user/lib/libc/src/stdio/stdio.c, renames every symbol it defines to an lk_
 * one so it can be linked alongside glibc, and this program then formats the
 * same thing both ways and compares the output byte for byte.
 *
 * Two separate things are being checked, and they fail differently:
 *
 *   - the OUTPUT, which is a correctness question.  A %g that rounds
 *     differently or a %#x missing its prefix is a wrong answer.
 *
 *   - the BYTES WRITTEN, which is a memory-safety question.  snprintf is given
 *     a destination of exactly n bytes ending flush against a guard page, so a
 *     formatter that writes one byte past the limit dies on the instruction
 *     that does it.  That matters more than it sounds: g_strdup_printf and
 *     every GLib and GTK string built with it goes through vsnprintf twice --
 *     once with a NULL buffer to measure, once to fill an allocation of exactly
 *     that size -- so a formatter that reports one length and writes another
 *     corrupts the heap on a completely ordinary call.
 *
 * The return value is checked as strictly as the bytes.  C99 says snprintf
 * returns the length the output WOULD have had, and a caller that sizes an
 * allocation from a return value that is short by even one byte then overruns
 * it, arbitrarily far from here.
 *
 * Build and run:  ./host/test-printf.sh
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Provided by the renamed stdio.o (see host/test-printf.sh). */
int lk_snprintf(char *str, size_t size, const char *format, ...);
int lk_vsnprintf(char *str, size_t size, const char *format, va_list ap);

static int g_checks;
static int g_fail;
static size_t g_pagesz;

/* ---- guarded destination ------------------------------------------------
 *
 * Same arrangement as host/test-string.c: two pages, the second unmapped, and
 * the n usable bytes placed flush against the wall.
 */
static unsigned char *guard_alloc(size_t n, unsigned char **out_base)
{
	unsigned char *p = mmap(NULL, 2 * g_pagesz, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(p != MAP_FAILED);
	assert(mprotect(p + g_pagesz, g_pagesz, PROT_NONE) == 0);
	memset(p, 0xA5, g_pagesz);
	*out_base = p;
	return p + g_pagesz - n;
}

/*
 * Format `fmt` both ways into buffers of exactly `n` bytes and compare.
 *
 * Called once per (format, size) pair; the caller sweeps n across the
 * interesting range so that every truncation boundary of every conversion gets
 * exercised, including n == 0 and n == 1.
 */
static void cmp(size_t n, const char *label, const char *fmt, ...)
{
	unsigned char *abase, *bbase;
	unsigned char *a = guard_alloc(n, &abase);
	unsigned char *b = guard_alloc(n, &bbase);
	va_list ap;
	int ra, rb;

	g_checks++;

	va_start(ap, fmt);
	ra = vsnprintf((char *)a, n, fmt, ap);
	va_end(ap);

	va_start(ap, fmt);
	rb = lk_vsnprintf((char *)b, n, fmt, ap);
	va_end(ap);

	if (ra != rb) {
		fprintf(stderr, "  FAIL %-18s n=%zu returned %d, glibc %d\n",
			label, n, rb, ra);
		g_fail++;
	} else if (n > 0 && memcmp(a, b, n) != 0) {
		fprintf(stderr,
			"  FAIL %-18s n=%zu wrote \"%.*s\", glibc \"%.*s\"\n",
			label, n, (int)n, (const char *)b, (int)n,
			(const char *)a);
		g_fail++;
	}

	/* Nothing may be written ahead of the destination either. */
	for (size_t i = 1; i <= 32 && (size_t)(b - bbase) >= i; i++)
		if (b[-(ptrdiff_t)i] != 0xA5) {
			fprintf(stderr,
				"  FAIL %-18s n=%zu wrote %zu byte(s) BEFORE the buffer\n",
				label, n, i);
			g_fail++;
			break;
		}

	munmap(abase, 2 * g_pagesz);
	munmap(bbase, 2 * g_pagesz);
}

/* Sweep the destination size across every truncation boundary. */
#define SWEEP(label, ...)                                    \
	do {                                                 \
		for (size_t n = 0; n <= 40; n++)             \
			cmp(n, label, __VA_ARGS__);          \
		cmp(200, label, __VA_ARGS__);                \
	} while (0)

int main(void)
{
	g_pagesz = (size_t)sysconf(_SC_PAGESIZE);
	printf("libc printf formatting vs glibc\n");

	/* ---- integers ---- */
	static const int ints[] = { 0,	 1,   -1,  9,	 10,   -10,  99,
				    100, 255, -255, 32767, -32768, 1000000,
				    -1000000 };
	for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); i++) {
		SWEEP("%d", "%d", ints[i]);
		SWEEP("%i", "%i", ints[i]);
		SWEEP("%5d", "%5d", ints[i]);
		SWEEP("%-5d", "%-5d", ints[i]);
		SWEEP("%05d", "%05d", ints[i]);
		SWEEP("%+d", "%+d", ints[i]);
		SWEEP("% d", "% d", ints[i]);
		SWEEP("%.5d", "%.5d", ints[i]);
		SWEEP("%8.3d", "%8.3d", ints[i]);
		SWEEP("%-8.3d", "%-8.3d", ints[i]);
		SWEEP("%*d", "%*d", 7, ints[i]);
		SWEEP("%.*d", "%.*d", 4, ints[i]);
		SWEEP("%u", "%u", (unsigned)ints[i]);
		SWEEP("%x", "%x", (unsigned)ints[i]);
		SWEEP("%X", "%X", (unsigned)ints[i]);
		SWEEP("%#x", "%#x", (unsigned)ints[i]);
		SWEEP("%#o", "%#o", (unsigned)ints[i]);
		SWEEP("%o", "%o", (unsigned)ints[i]);
		SWEEP("%08x", "%08x", (unsigned)ints[i]);
		SWEEP("%hd", "%hd", (short)ints[i]);
		SWEEP("%hhd", "%hhd", (signed char)ints[i]);
	}

	static const long long lls[] = { 0LL,
					 -1LL,
					 4294967295LL,
					 4294967296LL,
					 -4294967296LL,
					 9223372036854775807LL,
					 -9223372036854775807LL - 1 };
	for (size_t i = 0; i < sizeof(lls) / sizeof(lls[0]); i++) {
		SWEEP("%lld", "%lld", lls[i]);
		SWEEP("%llu", "%llu", (unsigned long long)lls[i]);
		SWEEP("%llx", "%llx", (unsigned long long)lls[i]);
		SWEEP("%ld", "%ld", (long)lls[i]);
		SWEEP("%lu", "%lu", (unsigned long)lls[i]);
		SWEEP("%lx", "%lx", (unsigned long)lls[i]);
		SWEEP("%zu", "%zu", (size_t)lls[i]);
		SWEEP("%zd", "%zd", (ssize_t)lls[i]);
		SWEEP("%020lld", "%020lld", lls[i]);
	}

	/* ---- strings and chars ---- */
	static const char *strs[] = { "",     "a",	"ab",  "abcdef",
				      "hello world, a longer one", "%",
				      "\t\n" };
	for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
		SWEEP("%s", "%s", strs[i]);
		SWEEP("%10s", "%10s", strs[i]);
		SWEEP("%-10s", "%-10s", strs[i]);
		SWEEP("%.3s", "%.3s", strs[i]);
		SWEEP("%10.3s", "%10.3s", strs[i]);
		SWEEP("%-10.3s", "%-10.3s", strs[i]);
		SWEEP("%.0s", "%.0s", strs[i]);
		SWEEP("%*.*s", "%*.*s", 9, 4, strs[i]);
	}
	for (int c = 1; c < 128; c += 13) {
		SWEEP("%c", "%c", c);
		SWEEP("%5c", "%5c", c);
		SWEEP("%-5c", "%-5c", c);
	}

	/* ---- pointers ----
	 *
	 * A null pointer is NOT compared against glibc.  %p is
	 * implementation-defined and glibc renders NULL as "(nil)", which this
	 * libc deliberately does not copy: "0x0" is of the same form as every
	 * other pointer it prints, so output written with %p reads back with
	 * strtoul, and "(nil)" does not.  Everything else about %p is compared
	 * exactly.
	 */
	SWEEP("%p", "%p", (void *)0xdeadbeef);
	SWEEP("%p high", "%p", (void *)0x7fffffffffffULL);
	{
		char nb[16];
		int n = lk_snprintf(nb, sizeof(nb), "%p", (void *)0);
		g_checks++;
		if (n != 3 || strcmp(nb, "0x0") != 0) {
			fprintf(stderr,
				"  FAIL %%p null           \"%s\"(%d), expected \"0x0\"(3)\n",
				nb, n);
			g_fail++;
		}
	}

	/* ---- floating point ---- */
	static const double dbls[] = { 0.0,	 -0.0,	   1.0,	    -1.0,
				       0.5,	 0.125,	   3.14159265358979,
				       -2.718281828, 1e-5,	 1e5,
				       1e15,	 1e-15,	   123456789.123456,
				       0.1,	 100.0,	   999.9995 };
	for (size_t i = 0; i < sizeof(dbls) / sizeof(dbls[0]); i++) {
		SWEEP("%f", "%f", dbls[i]);
		SWEEP("%.0f", "%.0f", dbls[i]);
		SWEEP("%.1f", "%.1f", dbls[i]);
		SWEEP("%.10f", "%.10f", dbls[i]);
		SWEEP("%12.4f", "%12.4f", dbls[i]);
		SWEEP("%-12.4f", "%-12.4f", dbls[i]);
		SWEEP("%012.4f", "%012.4f", dbls[i]);
		SWEEP("%+.2f", "%+.2f", dbls[i]);
		SWEEP("%e", "%e", dbls[i]);
		SWEEP("%.3e", "%.3e", dbls[i]);
		SWEEP("%E", "%E", dbls[i]);
		SWEEP("%g", "%g", dbls[i]);
		SWEEP("%.3g", "%.3g", dbls[i]);
		SWEEP("%G", "%G", dbls[i]);
	}

	/* ---- mixed, and the shapes real code actually uses ---- */
	SWEEP("mixed1", "%s=%d (%s)", "key", 42, "value");
	SWEEP("mixed2", "[%5.2f%%] %s", 12.3456, "done");
	SWEEP("mixed3", "%s:%d: %s: %s", "file.c", 123, "func", "message");
	SWEEP("mixed4", "%02d:%02d:%02d", 1, 2, 3);
	SWEEP("mixed5", "0x%08lx-0x%08lx %s", 0x1000UL, 0x2000UL, "r-xp");
	SWEEP("literal", "no conversions at all here");
	SWEEP("percent", "100%% sure");

	/* ---- the measure-then-fill pattern, exactly as GLib uses it ----
	 *
	 * g_strdup_printf asks for the length with no buffer, allocates that
	 * many bytes plus one, and formats into it.  If the two disagree by so
	 * much as a byte the allocation is overrun on an entirely ordinary
	 * call, so check the pair against each other rather than only against
	 * glibc.
	 */
	{
		/* Each case supplies its own complete argument list, because a
		 * format and a mismatched vararg is undefined behaviour in the
		 * test rather than a finding about libc. */
#define MEASURE_FILL(fmt, ...)                                                 \
		do {                                                           \
			int need = lk_snprintf(NULL, 0, fmt, __VA_ARGS__);     \
			g_checks++;                                            \
			if (need < 0) {                                        \
				fprintf(stderr,                                \
					"  FAIL measure           \"%s\" reported %d\n", \
					fmt, need);                            \
				g_fail++;                                      \
				break;                                         \
			}                                                      \
			unsigned char *base;                                   \
			unsigned char *buf =                                   \
				guard_alloc((size_t)need + 1, &base);          \
			int wrote = lk_snprintf((char *)buf,                   \
						(size_t)need + 1, fmt,         \
						__VA_ARGS__);                  \
			if (wrote != need) {                                   \
				fprintf(stderr,                                \
					"  FAIL measure/fill      \"%s\" measured %d, wrote %d\n", \
					fmt, need, wrote);                     \
				g_fail++;                                      \
			} else if (buf[need] != '\0') {                        \
				fprintf(stderr,                                \
					"  FAIL terminator        \"%s\" not terminated at %d\n", \
					fmt, need);                            \
				g_fail++;                                      \
			}                                                      \
			munmap(base, 2 * g_pagesz);                            \
		} while (0)

		MEASURE_FILL("%s", "hello");
		MEASURE_FILL("%s-%d", "x", 12345);
		MEASURE_FILL("%.3f", 3.14159);
		MEASURE_FILL("%-20s|", "pad");
		MEASURE_FILL("%s%s%s", "abc", "def", "ghi");
		MEASURE_FILL("%c%d%s", 'A', 65, "tail");
		MEASURE_FILL("%lu", (unsigned long)-1);
		MEASURE_FILL("%lld", (long long)-9223372036854775807LL - 1);
		MEASURE_FILL("%20.8e", 1234.5678);
	}

	printf("%d checks, %d failure%s\n", g_checks, g_fail,
	       g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
