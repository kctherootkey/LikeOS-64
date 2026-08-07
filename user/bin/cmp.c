/*
 * cmp - compare two files byte by byte
 *
 * Reports the offset of the first difference, or that one file is a prefix of
 * the other.  Matching sizes prove nothing on their own -- a file copied
 * through a filesystem that returns the wrong bytes has exactly the right
 * length and the wrong contents -- which is precisely the case this exists to
 * settle.
 *
 * Exit status follows the usual convention: 0 identical, 1 differing, 2 error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define CMP_BUF 8192

static int usage(void)
{
	fprintf(stderr, "usage: cmp [-l] [-s] file1 file2\n");
	fprintf(stderr, "  -l  list every differing byte (offset, then each value in octal)\n");
	fprintf(stderr, "  -s  say nothing; report only through the exit status\n");
	return 2;
}

int main(int argc, char **argv)
{
	int list = 0, silent = 0;
	int ai = 1;
	const char *n1, *n2;
	int f1, f2;
	unsigned char b1[CMP_BUF], b2[CMP_BUF];
	long long off = 1; /* byte numbering is 1-based, as line numbering is */
	long long line = 1;
	int differed = 0;

	while (ai < argc && argv[ai][0] == '-' && argv[ai][1] != '\0') {
		const char *p = argv[ai] + 1;

		while (*p) {
			if (*p == 'l')
				list = 1;
			else if (*p == 's')
				silent = 1;
			else
				return usage();
			p++;
		}
		ai++;
	}
	if (argc - ai != 2)
		return usage();

	n1 = argv[ai];
	n2 = argv[ai + 1];

	f1 = open(n1, O_RDONLY);
	if (f1 < 0) {
		if (!silent)
			fprintf(stderr, "cmp: %s: %s\n", n1, strerror(errno));
		return 2;
	}
	f2 = open(n2, O_RDONLY);
	if (f2 < 0) {
		if (!silent)
			fprintf(stderr, "cmp: %s: %s\n", n2, strerror(errno));
		close(f1);
		return 2;
	}

	for (;;) {
		ssize_t r1 = read(f1, b1, sizeof(b1));
		ssize_t r2 = read(f2, b2, sizeof(b2));
		ssize_t n, i;

		if (r1 < 0 || r2 < 0) {
			if (!silent)
				fprintf(stderr, "cmp: read error: %s\n",
					strerror(errno));
			close(f1);
			close(f2);
			return 2;
		}
		n = (r1 < r2) ? r1 : r2;

		for (i = 0; i < n; i++) {
			if (b1[i] != b2[i]) {
				differed = 1;
				if (silent) {
					close(f1);
					close(f2);
					return 1;
				}
				if (list) {
					printf("%lld %o %o\n", off + i,
					       b1[i], b2[i]);
				} else {
					printf("%s %s differ: byte %lld, line %lld\n",
					       n1, n2, off + i, line);
					close(f1);
					close(f2);
					return 1;
				}
			}
			if (b1[i] == '\n')
				line++;
		}
		off += n;

		/* One ran out before the other: same prefix, different length. */
		if (r1 != r2) {
			if (!silent)
				fprintf(stderr, "cmp: EOF on %s\n",
					(r1 < r2) ? n1 : n2);
			close(f1);
			close(f2);
			return 1;
		}
		if (r1 == 0)
			break; /* both at end */
	}

	close(f1);
	close(f2);
	return differed ? 1 : 0;
}
