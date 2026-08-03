/*
 * gen-unicode-tables - generate the libc's Unicode character tables.
 *
 * The classification, case-mapping and width data in
 * user/lib/libc/src/locale/unicode.c is not hand-written.  It is derived by
 * asking the HOST system's C library the same question for every code point
 * and recording the answers as interval tables.
 *
 * Reading it out of a reference implementation rather than out of the Unicode
 * Character Database is deliberate.  The POSIX character classes are not the
 * Unicode general categories, and the differences are the kind nobody guesses
 * right: the Arabic-Indic digits are `alpha` and not `digit`, a titlecase
 * letter is both `upper` and `lower`, private-use code points are `print` and
 * `punct`, and towlower(U+0130) is the simple mapping 'i' rather than the full
 * one that produces two characters.  Deriving from a working implementation
 * means those all come out right instead of accumulating as bug reports.
 *
 * Build, run and install the result:
 *
 *     cc -O2 -o gen-unicode-tables host/gen-unicode-tables.c
 *     ./gen-unicode-tables > user/lib/libc/src/locale/unicode.c
 *     ./gen-unicode-tables -k > kernel/io/unicode.c
 *
 * The kernel needs only the width data -- the console has to know how many
 * cells a code point occupies -- so -k emits that subset in kernel style.
 * Both outputs come from the same probe so the console and the programs
 * drawing on it cannot disagree about where the cursor ended up.
 *
 * The output is committed; this is how it is reproduced, not part of the build.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>

#define MAXCP 0x110000

/* Emit the code points for which pred() holds as sorted intervals. */
static void emit_ranges(const char *name, int (*pred)(unsigned),
			const char *comment)
{
	printf("/* %s */\n", comment);
	printf("static const struct range %s[] = {\n", name);

	char line[80];
	size_t linelen = 0;
	long start = -1;
	long count = 0;

	line[0] = '\0';
	for (unsigned cp = 0; cp <= MAXCP; cp++) {
		int in = (cp < MAXCP) && pred(cp);
		if (in && start < 0) {
			start = (long)cp;
		} else if (!in && start >= 0) {
			char item[32];
			snprintf(item, sizeof item, "{0x%04lX,0x%04X},", start,
				 cp - 1);
			if (linelen + strlen(item) > 76) {
				printf("\t%s\n", line);
				line[0] = '\0';
				linelen = 0;
			}
			strcat(line, item);
			linelen += strlen(item);
			start = -1;
			count++;
		}
	}
	if (linelen)
		printf("\t%s\n", line);
	printf("};\n\n");
	fprintf(stderr, "%-16s %5ld ranges\n", name, count);
}

/* Emit a case mapping as runs over which (mapped - cp) is constant. */
static void emit_deltas(const char *name, wint_t (*map)(wint_t),
			const char *comment)
{
	printf("/* %s */\n", comment);
	printf("static const struct delta_range %s[] = {\n", name);

	char line[80];
	size_t linelen = 0;
	long start = -1, prev = -1, delta = 0, count = 0;

	line[0] = '\0';
	for (unsigned cp = 0; cp <= MAXCP; cp++) {
		long d = 0;
		if (cp < MAXCP)
			d = (long)map((wint_t)cp) - (long)cp;
		int cont = (cp < MAXCP) && d != 0 && start >= 0 &&
			   d == delta && (long)cp == prev + 1;
		if (cont) {
			prev = (long)cp;
			continue;
		}
		if (start >= 0) {
			char item[48];
			snprintf(item, sizeof item, "{0x%04lX,0x%04lX,%ld},",
				 start, prev, delta);
			if (linelen + strlen(item) > 76) {
				printf("\t%s\n", line);
				line[0] = '\0';
				linelen = 0;
			}
			strcat(line, item);
			linelen += strlen(item);
			start = -1;
			count++;
		}
		if (cp < MAXCP && d != 0) {
			start = prev = (long)cp;
			delta = d;
		}
	}
	if (linelen)
		printf("\t%s\n", line);
	printf("};\n\n");
	fprintf(stderr, "%-16s %5ld ranges\n", name, count);
}

/* Same as emit_ranges, in kernel style: uint32_t fields, kernel comment form. */
static void emit_ranges_k(const char *name, int (*pred)(unsigned),
			  const char *comment)
{
	printf("\n// %s\n", comment);
	printf("static const struct urange %s[] = {\n", name);

	char line[80];
	size_t linelen = 0;
	long start = -1;
	long count = 0;

	line[0] = '\0';
	for (unsigned cp = 0; cp <= MAXCP; cp++) {
		int in = (cp < MAXCP) && pred(cp);
		if (in && start < 0) {
			start = (long)cp;
		} else if (!in && start >= 0) {
			char item[32];
			snprintf(item, sizeof item, "{0x%04lX,0x%04X},", start,
				 cp - 1);
			if (linelen + strlen(item) > 76) {
				printf("\t%s\n", line);
				line[0] = '\0';
				linelen = 0;
			}
			strcat(line, item);
			linelen += strlen(item);
			start = -1;
			count++;
		}
	}
	if (linelen)
		printf("\t%s\n", line);
	printf("};\n");
	fprintf(stderr, "%-16s %5ld ranges\n", name, count);
}

static int p_alpha(unsigned c) { return iswalpha(c) != 0; }
static int p_upper(unsigned c) { return iswupper(c) != 0; }
static int p_lower(unsigned c) { return iswlower(c) != 0; }
static int p_punct(unsigned c) { return iswpunct(c) != 0; }
static int p_print(unsigned c) { return iswprint(c) != 0; }
static int p_space(unsigned c) { return iswspace(c) != 0; }
static int p_cntrl(unsigned c) { return iswcntrl(c) != 0; }
static int p_blank(unsigned c) { return iswblank(c) != 0; }
static int p_graph(unsigned c) { return iswgraph(c) != 0; }
static int p_alnum(unsigned c) { return iswalnum(c) != 0; }
static int p_digit(unsigned c) { return iswdigit(c) != 0; }
static int p_xdigit(unsigned c) { return iswxdigit(c) != 0; }
static int p_w0(unsigned c) { return c != 0 && wcwidth((wchar_t)c) == 0; }
static int p_w2(unsigned c) { return wcwidth((wchar_t)c) == 2; }
static int p_wneg(unsigned c) { return wcwidth((wchar_t)c) < 0; }

/* Emit the width tables alone, in kernel style, for kernel/io/unicode.c. */
static void emit_kernel(void)
{
	printf("// Unicode character width table for the console.\n"
	       "//\n"
	       "// GENERATED by host/gen-unicode-tables.c -- do not edit by hand.\n"
	       "// Regenerate with:\n"
	       "//\n"
	       "//     cc -O2 -o gen host/gen-unicode-tables.c\n"
	       "//     ./gen -k > kernel/io/unicode.c\n"
	       "//\n"
	       "// The console needs one thing from Unicode: how many cells a code point\n"
	       "// occupies.  A combining mark occupies none -- it attaches to the character\n"
	       "// before it -- and an East Asian wide character occupies two.  Advancing one\n"
	       "// cell for each of those would put the cursor somewhere the program drawing on\n"
	       "// the console does not believe it is, and every column after it is then wrong.\n"
	       "//\n"
	       "// The same probe produces the width tables in the C library, so the two agree\n"
	       "// by construction rather than by inspection.\n"
	       "\n"
	       "#include <kernel/io/unicode.h>\n"
	       "\n"
	       "struct urange {\n"
	       "\tuint32_t lo, hi;\n"
	       "};\n"
	       "\n"
	       "static int in_range(const struct urange *tab, uint32_t n, uint32_t cp)\n"
	       "{\n"
	       "\tuint32_t lo = 0, hi = n;\n"
	       "\twhile (lo < hi) {\n"
	       "\t\tuint32_t mid = lo + (hi - lo) / 2;\n"
	       "\t\tif (cp < tab[mid].lo)\n"
	       "\t\t\thi = mid;\n"
	       "\t\telse if (cp > tab[mid].hi)\n"
	       "\t\t\tlo = mid + 1;\n"
	       "\t\telse\n"
	       "\t\t\treturn 1;\n"
	       "\t}\n"
	       "\treturn 0;\n"
	       "}\n");

	emit_ranges_k("uni_w0", p_w0, "Occupies no cell: combining marks and format characters");
	emit_ranges_k("uni_w2", p_w2, "Occupies two cells: East Asian Wide and Fullwidth");

	printf("\n"
	       "#define NELEM(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))\n"
	       "\n"
	       "// Cells occupied by a code point.  Controls never reach here -- the console\n"
	       "// filters them before it draws -- and anything unlisted takes one cell, which\n"
	       "// is also the honest answer for a code point the font has no glyph for: the\n"
	       "// replacement it draws instead is one cell wide.\n"
	       "uint32_t unicode_width(uint32_t cp)\n"
	       "{\n"
	       "\tif (cp < 0x80)\n"
	       "\t\treturn 1;\n"
	       "\tif (in_range(uni_w0, NELEM(uni_w0), cp))\n"
	       "\t\treturn 0;\n"
	       "\tif (in_range(uni_w2, NELEM(uni_w2), cp))\n"
	       "\t\treturn 2;\n"
	       "\treturn 1;\n"
	       "}\n");
}

int main(int argc, char **argv)
{
	int kernel = (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'k');

	if (!setlocale(LC_ALL, "C.UTF-8") &&
	    !setlocale(LC_ALL, "en_US.UTF-8")) {
		fprintf(stderr,
			"gen-unicode-tables: need a UTF-8 locale on the host\n");
		return 1;
	}
	fprintf(stderr, "generating from locale %s\n", setlocale(LC_ALL, NULL));

	if (kernel) {
		emit_kernel();
		return 0;
	}

	printf("/*\n"
	       " * Unicode character tables.\n"
	       " *\n"
	       " * GENERATED by host/gen-unicode-tables.c -- do not edit by hand.\n"
	       " * Regenerate with:\n"
	       " *\n"
	       " *     cc -O2 -o gen host/gen-unicode-tables.c\n"
	       " *     ./gen > user/lib/libc/src/locale/unicode.c\n"
	       " *\n"
	       " * Data is stored as sorted code point intervals and resolved by binary\n"
	       " * search.  A property that holds over a contiguous block costs one entry\n"
	       " * no matter how many code points the block spans, which is why the whole\n"
	       " * of Unicode fits in a few kilobytes here.\n"
	       " */\n"
	       "#include \"unicode.h\"\n"
	       "\n"
	       "int __uni_in_range(const struct range *tab, size_t n, unsigned cp)\n"
	       "{\n"
	       "\tsize_t lo = 0, hi = n;\n"
	       "\twhile (lo < hi) {\n"
	       "\t\tsize_t mid = lo + (hi - lo) / 2;\n"
	       "\t\tif (cp < tab[mid].lo)\n"
	       "\t\t\thi = mid;\n"
	       "\t\telse if (cp > tab[mid].hi)\n"
	       "\t\t\tlo = mid + 1;\n"
	       "\t\telse\n"
	       "\t\t\treturn 1;\n"
	       "\t}\n"
	       "\treturn 0;\n"
	       "}\n"
	       "\n"
	       "static int delta_of(const struct delta_range *tab, size_t n, unsigned cp)\n"
	       "{\n"
	       "\tsize_t lo = 0, hi = n;\n"
	       "\twhile (lo < hi) {\n"
	       "\t\tsize_t mid = lo + (hi - lo) / 2;\n"
	       "\t\tif (cp < tab[mid].lo)\n"
	       "\t\t\thi = mid;\n"
	       "\t\telse if (cp > tab[mid].hi)\n"
	       "\t\t\tlo = mid + 1;\n"
	       "\t\telse\n"
	       "\t\t\treturn tab[mid].delta;\n"
	       "\t}\n"
	       "\treturn 0;\n"
	       "}\n");

	emit_ranges("uni_alpha", p_alpha, "POSIX class: alpha");
	emit_ranges("uni_upper", p_upper, "POSIX class: upper");
	emit_ranges("uni_lower", p_lower, "POSIX class: lower");
	emit_ranges("uni_punct", p_punct, "POSIX class: punct");
	emit_ranges("uni_print", p_print, "POSIX class: print");
	emit_ranges("uni_space", p_space, "POSIX class: space");
	emit_ranges("uni_cntrl", p_cntrl, "POSIX class: cntrl");
	emit_ranges("uni_blank", p_blank, "POSIX class: blank");
	emit_ranges("uni_graph", p_graph, "POSIX class: graph");
	emit_ranges("uni_alnum", p_alnum, "POSIX class: alnum");
	emit_ranges("uni_digit", p_digit, "POSIX class: digit");
	emit_ranges("uni_xdigit", p_xdigit, "POSIX class: xdigit");

	emit_ranges("uni_w0", p_w0, "Occupies no terminal column");
	emit_ranges("uni_w2", p_w2, "Occupies two terminal columns");
	emit_ranges("uni_wneg", p_wneg, "Has no printable width at all");

	emit_deltas("uni_toupper", towupper,
		    "Simple uppercase mapping, as constant-delta runs");
	emit_deltas("uni_tolower", towlower,
		    "Simple lowercase mapping, as constant-delta runs");

	printf("#define IN(tab, cp) __uni_in_range(tab, sizeof(tab) / sizeof(*tab), (cp))\n"
	       "\n"
	       "int __uni_isalpha(unsigned cp) { return IN(uni_alpha, cp); }\n"
	       "int __uni_isupper(unsigned cp) { return IN(uni_upper, cp); }\n"
	       "int __uni_islower(unsigned cp) { return IN(uni_lower, cp); }\n"
	       "int __uni_ispunct(unsigned cp) { return IN(uni_punct, cp); }\n"
	       "int __uni_isprint(unsigned cp) { return IN(uni_print, cp); }\n"
	       "int __uni_isspace(unsigned cp) { return IN(uni_space, cp); }\n"
	       "int __uni_iscntrl(unsigned cp) { return IN(uni_cntrl, cp); }\n"
	       "int __uni_isblank(unsigned cp) { return IN(uni_blank, cp); }\n"
	       "int __uni_isgraph(unsigned cp) { return IN(uni_graph, cp); }\n"
	       "int __uni_isalnum(unsigned cp) { return IN(uni_alnum, cp); }\n"
	       "int __uni_isdigit(unsigned cp) { return IN(uni_digit, cp); }\n"
	       "int __uni_isxdigit(unsigned cp) { return IN(uni_xdigit, cp); }\n"
	       "\n"
	       "/* The NUL advances nothing; everything not listed occupies one column. */\n"
	       "int __uni_width(unsigned cp)\n"
	       "{\n"
	       "\tif (cp == 0)\n"
	       "\t\treturn 0;\n"
	       "\tif (IN(uni_wneg, cp))\n"
	       "\t\treturn -1;\n"
	       "\tif (IN(uni_w0, cp))\n"
	       "\t\treturn 0;\n"
	       "\tif (IN(uni_w2, cp))\n"
	       "\t\treturn 2;\n"
	       "\treturn 1;\n"
	       "}\n"
	       "\n"
	       "unsigned __uni_toupper(unsigned cp)\n"
	       "{\n"
	       "\treturn cp + (unsigned)delta_of(uni_toupper,\n"
	       "\t\t\t\t       sizeof(uni_toupper) / sizeof(*uni_toupper),\n"
	       "\t\t\t\t       cp);\n"
	       "}\n"
	       "\n"
	       "unsigned __uni_tolower(unsigned cp)\n"
	       "{\n"
	       "\treturn cp + (unsigned)delta_of(uni_tolower,\n"
	       "\t\t\t\t       sizeof(uni_tolower) / sizeof(*uni_tolower),\n"
	       "\t\t\t\t       cp);\n"
	       "}\n");

	return 0;
}
