/*
 * codecheck: does a process's executable memory still match the file it came
 * from?
 *
 * A crash whose RIP lands in a run of 0xff inside a file-backed r-x mapping
 * says the code the process is executing is not the code on disk -- but it
 * says it only after the process has already branched into it, which is far
 * too late to see what happened.  This asks the question directly, of a live
 * process, before anything goes wrong: walk every executable mapping, read
 * what the process actually has at each page, read what the file has at the
 * matching offset, and compare.
 *
 * Every mapping is checked to its end, page by page, so a single wrong page
 * anywhere in a hundred megabytes of library text is found and named.  A
 * mismatch prints the mapping, the file, the page's file offset, the first
 * differing byte, and both versions of the bytes around it -- which is enough
 * to tell "this page came from somewhere else in the file" (the bytes will
 * BE the file's, at a different offset) from "this page was never filled"
 * (zeros, or a repeated pattern).
 *
 *   codecheck <pid>          check every executable mapping
 *   codecheck -v <pid>       name each mapping as it is checked
 *   codecheck -a <pid>       check readable file-backed mappings too, not
 *                            just executable ones (rodata goes wrong the
 *                            same way; it just fails more quietly)
 *
 * Only your own processes, unless you are root: the bytes of another
 * process's memory are as private as the map that says where they are.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

#define PAGE 4096

static int g_verbose;
static int g_all;
static long g_pages_checked;
static long g_pages_bad;
static long g_maps_checked;

/* One line of /proc/<pid>/maps, parsed. */
struct mapping {
	uint64_t start, end, off;
	char perms[8];
	char path[256];
};

static int parse_map_line(const char *line, struct mapping *m)
{
	const char *p = line;
	char *endp;

	memset(m, 0, sizeof(*m));
	m->start = strtoull(p, &endp, 16);
	if (endp == p || *endp != '-')
		return 0;
	p = endp + 1;
	m->end = strtoull(p, &endp, 16);
	if (endp == p)
		return 0;
	p = endp;
	while (*p == ' ')
		p++;
	for (int i = 0; i < 4 && *p && *p != ' '; i++)
		m->perms[i] = *p++;
	while (*p == ' ')
		p++;
	m->off = strtoull(p, &endp, 16);
	if (endp == p)
		return 0;
	p = endp;
	/* dev and inode, then the path (which may be absent) */
	for (int f = 0; f < 2; f++) {
		while (*p == ' ')
			p++;
		while (*p && *p != ' ')
			p++;
	}
	while (*p == ' ')
		p++;
	size_t n = 0;
	while (*p && *p != '\n' && n < sizeof(m->path) - 1)
		m->path[n++] = *p++;
	m->path[n] = 0;
	return 1;
}

/* Both versions of the bytes around the first difference.  Printed together
 * so the two can be read against each other. */
static void dump_pair(const unsigned char *mem, const unsigned char *disk,
		      size_t at)
{
	size_t from = at >= 16 ? (at - 16) & ~(size_t)15 : 0;
	size_t to = from + 64;

	if (to > PAGE)
		to = PAGE;
	for (size_t row = from; row < to; row += 16) {
		printf("      %04zx  memory:", row);
		for (size_t i = row; i < row + 16 && i < PAGE; i++)
			printf(" %02x", mem[i]);
		printf("\n      %04zx  file:  ", row);
		for (size_t i = row; i < row + 16 && i < PAGE; i++)
			printf(" %02x", disk[i]);
		printf("\n");
	}
}

/* Is this page one the file cannot account for, i.e. past its end?  A mapping
 * whose last page runs past EOF is legitimate and reads as zeros there, so
 * only the bytes the file actually has are compared. */
static int check_mapping(int memfd, const struct mapping *m)
{
	int ffd = open(m->path, O_RDONLY);
	struct stat st;
	unsigned char mem[PAGE], disk[PAGE];
	int bad = 0;

	if (ffd < 0) {
		printf("  %012llx-%012llx %s  (cannot open %s: %s)\n",
		       (unsigned long long)m->start, (unsigned long long)m->end,
		       m->perms, m->path, strerror(errno));
		return 0;
	}
	if (fstat(ffd, &st) != 0)
		st.st_size = 0;
	if (g_verbose)
		printf("  %012llx-%012llx %s +%08llx  %s\n",
		       (unsigned long long)m->start, (unsigned long long)m->end,
		       m->perms, (unsigned long long)m->off, m->path);
	g_maps_checked++;

	for (uint64_t va = m->start; va < m->end; va += PAGE) {
		uint64_t fo = m->off + (va - m->start);
		ssize_t nd, nm;
		size_t cmp;

		if (fo >= (uint64_t)st.st_size)
			break; /* past the file: zero-fill, nothing to check */
		nd = pread(ffd, disk, PAGE, (off_t)fo);
		if (nd <= 0)
			break;
		nm = pread(memfd, mem, (size_t)nd, (off_t)va);
		if (nm != nd) {
			/* Not resident, or gone: not a mismatch.  A page that
			 * has never been faulted in has nothing to compare. */
			continue;
		}
		cmp = (size_t)nd;
		g_pages_checked++;
		if (memcmp(mem, disk, cmp) == 0)
			continue;

		size_t i = 0;
		while (i < cmp && mem[i] == disk[i])
			i++;
		g_pages_bad++;
		bad++;
		printf("MISMATCH %s\n", m->path);
		printf("    mapping %012llx-%012llx %s, file offset %08llx\n",
		       (unsigned long long)m->start, (unsigned long long)m->end,
		       m->perms, (unsigned long long)m->off);
		printf("    page at %012llx (file offset %08llx): first difference at byte %zu\n",
		       (unsigned long long)va, (unsigned long long)fo, i);
		dump_pair(mem, disk, i);
	}
	close(ffd);
	return bad;
}

int main(int argc, char **argv)
{
	const char *pidarg = NULL;
	char path[64];
	FILE *maps;
	int memfd;
	char line[512];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			g_verbose = 1;
		else if (!strcmp(argv[i], "-a"))
			g_all = 1;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: %s [-v] [-a] <pid>\n", argv[0]);
			return 2;
		} else
			pidarg = argv[i];
	}
	if (!pidarg) {
		fprintf(stderr, "usage: %s [-v] [-a] <pid>\n", argv[0]);
		return 2;
	}

	snprintf(path, sizeof(path), "/proc/%s/maps", pidarg);
	maps = fopen(path, "r");
	if (!maps) {
		fprintf(stderr, "codecheck: %s: %s\n", path, strerror(errno));
		return 1;
	}
	snprintf(path, sizeof(path), "/proc/%s/mem", pidarg);
	memfd = open(path, O_RDONLY);
	if (memfd < 0) {
		fprintf(stderr, "codecheck: %s: %s\n", path, strerror(errno));
		fclose(maps);
		return 1;
	}

	printf("codecheck: pid %s, comparing %s mappings against their files\n",
	       pidarg, g_all ? "all file-backed" : "executable");

	while (fgets(line, sizeof(line), maps)) {
		struct mapping m;

		if (!parse_map_line(line, &m))
			continue;
		if (!m.path[0] || m.path[0] != '/')
			continue; /* anonymous, or a name with no file */
		if (!g_all && m.perms[2] != 'x')
			continue;
		if (m.perms[0] != 'r')
			continue;
		/* A writable private mapping has every right to differ: the
		 * process owns its copy. */
		if (m.perms[1] == 'w')
			continue;
		check_mapping(memfd, &m);
	}
	fclose(maps);
	close(memfd);

	printf("codecheck: %ld mappings, %ld resident pages compared, %ld differ\n",
	       g_maps_checked, g_pages_checked, g_pages_bad);
	return g_pages_bad ? 1 : 0;
}
