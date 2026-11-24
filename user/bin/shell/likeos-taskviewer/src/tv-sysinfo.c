// LikeOS Task Viewer -- the system sampler.
//
// See tv-sysinfo.h for what is read from where.  The parsers come first and
// touch nothing but their arguments; the sampler after them reads the
// files and calls the system.  What only LikeOS has (the process listing,
// the interface counters) is behind __LIKEOS__, so the parsers and the file
// readers build and run anywhere for the tests.
//
// Copyright (C) 2026 The LikeOS Project

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define TV_HAVE_CPUID 1
#endif

#ifdef __LIKEOS__
#include <net/if.h>
#include <netdb.h>
#include <sys/procinfo.h>
#endif

#include "tv-sysinfo.h"

/* ==========================================================================
 * Formatting
 * ========================================================================== */

char *tv_format_bytes(guint64 bytes)
{
	static const char *const unit[] = { "bytes", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;

	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		return g_strdup_printf("%" G_GUINT64_FORMAT " bytes", bytes);
	if (v >= 100.0)
		return g_strdup_printf("%.0f %s", v, unit[u]);
	return g_strdup_printf("%.1f %s", v, unit[u]);
}

char *tv_format_byte_rate(double bps)
{
	if (bps < 0)
		bps = 0;
	if (bps < 1024.0 * 1024.0)
		return g_strdup_printf("%.0f KB/s", bps / 1024.0);
	if (bps < 1024.0 * 1024.0 * 1024.0)
		return g_strdup_printf("%.1f MB/s", bps / (1024.0 * 1024.0));
	return g_strdup_printf("%.1f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
}

char *tv_format_bit_rate(double bps)
{
	if (bps < 0)
		bps = 0;
	if (bps < 1000.0 * 1000.0) {
		double k = bps / 1000.0;
		return k > 0 && k < 10.0 ? g_strdup_printf("%.1f Kbps", k) :
					   g_strdup_printf("%.0f Kbps", k);
	}
	if (bps < 1000.0 * 1000.0 * 1000.0)
		return g_strdup_printf("%.1f Mbps", bps / 1e6);
	return g_strdup_printf("%.2f Gbps", bps / 1e9);
}

char *tv_format_mhz(double mhz)
{
	if (mhz <= 0)
		return g_strdup("Unknown");
	if (mhz >= 1000.0)
		return g_strdup_printf("%.2f GHz", mhz / 1000.0);
	return g_strdup_printf("%.0f MHz", mhz);
}

char *tv_format_uptime(guint64 s)
{
	return g_strdup_printf("%" G_GUINT64_FORMAT ":%02u:%02u:%02u",
			       s / 86400, (unsigned)(s / 3600 % 24),
			       (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

const char *tv_disk_kind_name(TvDiskKind kind)
{
	switch (kind) {
	case TV_DISK_SSD:
		return "SSD";
	case TV_DISK_NVME:
		return "SSD (NVMe)";
	case TV_DISK_HDD:
		return "HDD";
	case TV_DISK_USB:
		return "USB";
	case TV_DISK_REMOVABLE:
		return "Removable";
	case TV_DISK_OPTICAL:
		return "CD/DVD";
	case TV_DISK_UNKNOWN:
		break;
	}
	return "Disk";
}

const char *tv_init_action_name(TvInitAction a)
{
	switch (a) {
	case TV_INIT_ONCE:
		return "once";
	case TV_INIT_WAIT:
		return "wait";
	case TV_INIT_RESPAWN:
		break;
	}
	return "respawn";
}

/* ==========================================================================
 * Parsers
 * ========================================================================== */

/* The value of a "Key:   <number> kB" line; FALSE when `line' is another. */
static gboolean meminfo_value(const char *line, const char *key, guint64 *out)
{
	size_t n = strlen(key);
	const char *p;
	char *end;
	guint64 v;

	if (strncmp(line, key, n) != 0 || line[n] != ':')
		return FALSE;
	p = line + n + 1;
	while (*p == ' ' || *p == '\t')
		p++;
	errno = 0;
	v = g_ascii_strtoull(p, &end, 10);
	if (end == p || errno)
		return FALSE;
	while (*end == ' ' || *end == '\t')
		end++;
	if (end[0] == 'k' || end[0] == 'K')
		v *= 1024;
	*out = v;
	return TRUE;
}

gboolean tv_parse_meminfo(const char *text, TvMemInfo *out)
{
	gboolean have_total = FALSE, have_free = FALSE;
	const char *line = text;

	memset(out, 0, sizeof(*out));
	while (line && *line) {
		guint64 v;

		if (meminfo_value(line, "MemTotal", &v)) {
			out->total = v;
			have_total = TRUE;
		} else if (meminfo_value(line, "MemFree", &v)) {
			out->free = v;
			have_free = TRUE;
		} else if (meminfo_value(line, "MemAvailable", &v)) {
			out->available = v;
			out->have_available = TRUE;
		} else if (meminfo_value(line, "Cached", &v)) {
			out->cached = v;
			out->have_cached = TRUE;
		} else if (meminfo_value(line, "Buffers", &v)) {
			out->buffers = v;
			out->have_buffers = TRUE;
		} else if (meminfo_value(line, "Committed_AS", &v)) {
			out->committed = v;
			out->have_committed = TRUE;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	if (!have_total || !have_free)
		return FALSE;
	if (!out->have_available) {
		out->available = out->free + out->cached + out->buffers;
		if (out->available > out->total)
			out->available = out->total;
	}
	out->in_use =
		out->total > out->available ? out->total - out->available : 0;
	return TRUE;
}

int tv_parse_cpuinfo(const char *text, double *mhz)
{
	const char *line = text;
	int n = 0, nmhz = 0;
	double sum = 0;

	while (line && *line) {
		if (strncmp(line, "processor", 9) == 0 &&
		    (line[9] == ' ' || line[9] == '\t' || line[9] == ':')) {
			n++;
		} else if (strncmp(line, "cpu MHz", 7) == 0) {
			const char *c = strchr(line, ':');
			const char *nl = strchr(line, '\n');
			if (c && (!nl || c < nl)) {
				double v = g_ascii_strtod(c + 1, NULL);
				if (v > 0) {
					sum += v;
					nmhz++;
				}
			}
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	if (mhz)
		*mhz = nmhz ? sum / nmhz : 0;
	return n;
}

double tv_parse_uptime(const char *text)
{
	char *end;
	double v;

	if (!text)
		return -1;
	v = g_ascii_strtod(text, &end);
	return end == text ? -1 : v;
}

double tv_parse_brand_mhz(const char *brand)
{
	const char *at = strrchr(brand, '@');
	char *end;
	double v;

	if (!at)
		return 0;
	at++;
	while (*at == ' ')
		at++;
	v = g_ascii_strtod(at, &end);
	if (end == at || v <= 0)
		return 0;
	while (*end == ' ')
		end++;
	if (g_ascii_strncasecmp(end, "GHz", 3) == 0)
		return v * 1000.0;
	if (g_ascii_strncasecmp(end, "MHz", 3) == 0)
		return v;
	return 0;
}

void tv_squeeze_spaces(char *s)
{
	char *w = s;
	gboolean space = TRUE; /* drops leading blanks */

	for (const char *r = s; *r; r++) {
		if (*r == ' ' || *r == '\t') {
			if (!space)
				*w++ = ' ';
			space = TRUE;
		} else {
			*w++ = *r;
			space = FALSE;
		}
	}
	if (w > s && w[-1] == ' ')
		w--;
	*w = '\0';
}

void tv_cpu_topology(int logical, int threads_per_core,
		     int logical_per_package, int *sockets, int *cores)
{
	int lpp, tpc;

	if (logical < 1)
		logical = 1;
	/* A virtual machine may describe a bigger package than it gives. */
	lpp = logical_per_package > 0 ? logical_per_package : logical;
	if (lpp > logical)
		lpp = logical;
	tpc = threads_per_core > 0 ? threads_per_core : 1;
	if (tpc > lpp)
		tpc = 1;
	*sockets = (logical + lpp - 1) / lpp;
	*cores = logical / tpc;
	if (*cores < 1)
		*cores = 1;
}

static int task_cmp(const void *a, const void *b)
{
	const TvTaskTicks *x = a, *y = b;
	return (x->tid > y->tid) - (x->tid < y->tid);
}

void tv_cpu_usage(const TvTaskTicks *prev, int nprev, const TvTaskTicks *cur,
		  int ncur, double prev_time, double elapsed, int ncpu,
		  double *per_cpu, double *total)
{
	double sum = 0;

	for (int c = 0; c < ncpu; c++)
		per_cpu[c] = 0;
	*total = 0;
	if (elapsed <= 0 || ncpu < 1)
		return;
	for (int i = 0; i < ncur; i++) {
		const TvTaskTicks *t = &cur[i];
		const TvTaskTicks *p =
			bsearch(t, prev, (size_t)nprev, sizeof(*prev), task_cmp);
		double d;

		if (p && p->start == t->start) {
			d = t->ticks > p->ticks ? (double)(t->ticks - p->ticks) :
						  0;
		} else if ((double)t->start + 1 >= prev_time) {
			/* Started since the last listing: all of its time
			 * falls in the interval. */
			d = (double)t->ticks;
		} else {
			continue;
		}
		if (d > elapsed)
			d = elapsed; /* one thread, one processor */
		sum += d;
		if (t->cpu >= 0 && t->cpu < ncpu)
			per_cpu[t->cpu] += d;
	}
	for (int c = 0; c < ncpu; c++) {
		per_cpu[c] = per_cpu[c] * 100.0 / elapsed;
		if (per_cpu[c] > 100.0)
			per_cpu[c] = 100.0;
	}
	*total = sum * 100.0 / (elapsed * ncpu);
	if (*total > 100.0)
		*total = 100.0;
}

int tv_parse_diskstats(const char *text, TvDiskStat *out, int max)
{
	const char *line = text;
	int n = 0;

	while (line && *line && n < max) {
		unsigned major, minor;
		char name[32];
		unsigned long long v[11];
		int k = sscanf(line,
			       "%u %u %31s %llu %llu %llu %llu %llu %llu %llu "
			       "%llu %llu %llu %llu",
			       &major, &minor, name, &v[0], &v[1], &v[2], &v[3],
			       &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]);

		if (k >= 13) {
			TvDiskStat *d = &out[n++];

			memset(d, 0, sizeof(*d));
			g_strlcpy(d->name, name, sizeof(d->name));
			d->rd_ios = v[0];
			d->rd_sectors = v[2];
			d->rd_ms = v[3];
			d->wr_ios = v[4];
			d->wr_sectors = v[6];
			d->wr_ms = v[7];
			d->io_ms = v[9];
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return n;
}

static guint64 delta(guint64 a, guint64 b)
{
	return b > a ? b - a : 0;
}

void tv_disk_rates(const TvDiskStat *a, const TvDiskStat *b, double secs,
		   TvDisk *out)
{
	guint64 ios, ms;

	out->active = out->read_bps = out->write_bps = out->response_ms = 0;
	if (secs <= 0)
		return;
	out->active = (double)delta(a->io_ms, b->io_ms) / (secs * 10.0);
	if (out->active > 100.0)
		out->active = 100.0;
	out->read_bps = (double)delta(a->rd_sectors, b->rd_sectors) * 512.0 /
			secs;
	out->write_bps = (double)delta(a->wr_sectors, b->wr_sectors) * 512.0 /
			 secs;
	ios = delta(a->rd_ios, b->rd_ios) + delta(a->wr_ios, b->wr_ios);
	ms = delta(a->rd_ms, b->rd_ms) + delta(a->wr_ms, b->wr_ms);
	out->response_ms = ios ? (double)ms / (double)ios : 0;
}

gboolean tv_parse_root_source(const char *mounts, char *out, gsize size)
{
	const char *line = mounts;

	while (line && *line) {
		char src[256], dir[256];

		if (sscanf(line, "%255s %255s", src, dir) == 2 &&
		    strcmp(dir, "/") == 0 && strcmp(src, "rootfs") != 0) {
			g_strlcpy(out, src, size);
			return TRUE;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return FALSE;
}

gboolean tv_disk_of_link(const char *target, char *out, gsize size)
{
	const char *p = strstr(target, "block/");
	const char *end;

	if (!p)
		return FALSE;
	p += 6;
	end = strchr(p, '/');
	if (!end)
		end = p + strlen(p);
	if (end == p || (gsize)(end - p) >= size)
		return FALSE;
	memcpy(out, p, (size_t)(end - p));
	out[end - p] = '\0';
	return TRUE;
}

/* ---- comments, shared by the inittab and session readers ---------------- */

/* The first phrase of a comment block: up to the first ':', ',' or '(',
 * or the end of the first sentence, on one line. */
static char *first_phrase(GString *block)
{
	const char *s = block->str;
	GString *r = g_string_new(NULL);

	while (*s == ' ')
		s++;
	for (; *s; s++) {
		if (*s == ':' && (s[1] == ' ' || s[1] == '\0'))
			break;
		if (*s == '.' && (s[1] == ' ' || s[1] == '\0'))
			break;
		if (*s == ',' && s[1] == ' ')
			break;
		if (*s == '(' && s > block->str && s[-1] == ' ')
			break;
		g_string_append_c(r, *s);
	}
	tv_squeeze_spaces(r->str);
	if (!r->str[0]) {
		g_string_free(r, TRUE);
		return NULL;
	}
	return g_string_free(r, FALSE);
}

/* Adds a comment line's text to the block (the '#' and one blank
 * dropped). */
static void block_add(GString *block, const char *line)
{
	line++; /* '#' */
	while (*line == ' ' || *line == '\t')
		line++;
	if (block->len)
		g_string_append_c(block, ' ');
	g_string_append(block, line);
}

static char *base_name(const char *path)
{
	const char *b = strrchr(path, '/');
	return g_strdup(b ? b + 1 : path);
}

/* ---- /etc/inittab ------------------------------------------------------ */

static void init_entry_free(gpointer p)
{
	TvInitEntry *e = p;

	if (!e)
		return; /* moved to the result */
	g_free(e->name);
	g_free(e->program);
	g_free(e->command);
	g_free(e->description);
	g_free(e);
}

static gboolean action_word(const char *w, TvInitAction *a)
{
	if (strcmp(w, "respawn") == 0)
		*a = TV_INIT_RESPAWN;
	else if (strcmp(w, "once") == 0)
		*a = TV_INIT_ONCE;
	else if (strcmp(w, "wait") == 0)
		*a = TV_INIT_WAIT;
	else
		return FALSE;
	return TRUE;
}

/* An inittab line's words into an entry, the way init reads them; NULL for
 * a line that starts nothing.  `commented' lines must begin with an
 * action word and name a program by its path. */
static TvInitEntry *init_line(const char *text, gboolean commented)
{
	char **w = g_strsplit_set(text, " \t", -1);
	GPtrArray *words = g_ptr_array_new();
	TvInitEntry *e = NULL;
	TvInitAction action = TV_INIT_RESPAWN;
	guint first = 0;
	gboolean console = FALSE;

	for (int i = 0; w[i]; i++) {
		if (w[i][0] == '#')
			break;
		if (w[i][0])
			g_ptr_array_add(words, w[i]);
	}
	if (words->len == 0)
		goto out;
	if (action_word(words->pdata[0], &action))
		first = 1;
	else if (commented)
		goto out;
	if (first < words->len && strcmp(words->pdata[first], "console") == 0) {
		console = TRUE;
		first++;
	}
	if (first >= words->len)
		goto out;
	if (commented && ((char *)words->pdata[first])[0] != '/')
		goto out;

	e = g_new0(TvInitEntry, 1);
	e->action = action;
	e->console = console;
	e->disabled = commented;
	e->program = g_strdup(words->pdata[first]);
	e->name = base_name(e->program);
	{
		GString *cmd = g_string_new(NULL);
		for (guint i = first; i < words->len; i++) {
			if (cmd->len)
				g_string_append_c(cmd, ' ');
			g_string_append(cmd, words->pdata[i]);
		}
		e->command = g_string_free(cmd, FALSE);
	}
out:
	g_ptr_array_free(words, TRUE);
	g_strfreev(w);
	return e;
}

static gboolean same_program(GPtrArray *list, const char *program)
{
	for (guint i = 0; i < list->len; i++) {
		TvInitEntry *e = list->pdata[i];
		if (strcmp(e->program, program) == 0)
			return TRUE;
	}
	return FALSE;
}

GPtrArray *tv_parse_inittab(const char *text)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(init_entry_free);
	GPtrArray *off = g_ptr_array_new_with_free_func(init_entry_free);
	char **lines = g_strsplit(text ? text : "", "\n", -1);
	GString *block = g_string_new(NULL);

	for (int i = 0; lines[i]; i++) {
		char *l = lines[i];
		size_t n = strlen(l);

		if (n && l[n - 1] == '\r')
			l[n - 1] = '\0';
		while (*l == ' ' || *l == '\t')
			l++;
		if (*l == '\0') {
			g_string_truncate(block, 0);
			continue;
		}
		if (*l == '#') {
			/* "# respawn /usr/sbin/sshd -D" closing a comment
			 * block is an entry switched off.  Inside a block it
			 * is an example in the prose. */
			const char *next = lines[i + 1];
			TvInitEntry *e = NULL;

			while (next && (*next == ' ' || *next == '\t'))
				next++;
			if (!next || *next != '#')
				e = init_line(l + 1, TRUE);
			if (e) {
				e->line = i + 1;
				e->description = first_phrase(block);
				g_ptr_array_add(off, e);
				g_string_truncate(block, 0);
			} else {
				block_add(block, l);
			}
			continue;
		}
		{
			TvInitEntry *e = init_line(l, FALSE);
			if (e) {
				e->line = i + 1;
				e->description = first_phrase(block);
				g_ptr_array_add(list, e);
			}
		}
		g_string_truncate(block, 0);
	}

	/* The switched-off entries, in file order among the others, unless
	 * the same program is started anyway. */
	for (guint i = 0; i < off->len; i++) {
		TvInitEntry *e = off->pdata[i];
		guint at = 0;

		if (same_program(list, e->program))
			continue;
		while (at < list->len &&
		       ((TvInitEntry *)list->pdata[at])->line < e->line)
			at++;
		off->pdata[i] = NULL;
		g_ptr_array_insert(list, (gint)at, e);
	}
	g_ptr_array_unref(off);
	g_string_free(block, TRUE);
	g_strfreev(lines);
	return list;
}

/* ---- the X session script --------------------------------------------- */

static void session_entry_free(gpointer p)
{
	TvSessionEntry *e = p;

	g_free(e->name);
	g_free(e->command);
	g_free(e->description);
	g_free(e);
}

/* Words that start a line of shell that is not a program being started. */
static gboolean shell_word(const char *w)
{
	static const char *const words[] = {
		"if",	 "then",  "else", "elif",     "fi",	  "while",
		"until", "do",	  "done", "for",      "case",	  "esac",
		"in",	 "export", "eval", "wait",    "echo",	  "printf",
		"rm",	 "[",	  "[[",	  "test",     "kill",	  "local",
		"set",	 "unset", "return", "exit",   "cd",	  "true",
		"false", "read",  ":",	  "shift",    "trap",	  "umask",
		"ulimit", ".",	  "source", "{",      "}",	  "sleep",
		"mkdir", "cat",	  "cp",	  "mv",	      "chmod",	  "ln",
		"touch", "readonly", "break", "continue", "command", "type",
		"hash",	 "!",	  "&&",	  "||",	      "then:",	  NULL
	};

	for (int i = 0; words[i]; i++)
		if (strcmp(w, words[i]) == 0)
			return TRUE;
	return FALSE;
}

/* A variable assignment ("NAME=value") rather than a command. */
static gboolean assignment(const char *w)
{
	const char *eq = strchr(w, '=');

	if (!eq || eq == w)
		return FALSE;
	for (const char *p = w; p < eq; p++)
		if (!(g_ascii_isalnum(*p) || *p == '_'))
			return FALSE;
	return TRUE;
}

GPtrArray *tv_parse_session_script(const char *text)
{
	GPtrArray *list = g_ptr_array_new_with_free_func(session_entry_free);
	char **lines = g_strsplit(text ? text : "", "\n", -1);
	GString *block = g_string_new(NULL);
	GString *stmt = g_string_new(NULL);
	char *about = NULL; /* the last comment block's first phrase */
	TvSessionEntry *last_bg = NULL;
	int stmt_line = 0;

	for (int i = 0; lines[i]; i++) {
		char *l = lines[i];
		size_t n;
		char **w;
		int k = 0;
		gboolean exec;
		TvSessionEntry *added_bg = NULL;

		while (*l == ' ' || *l == '\t')
			l++;
		if (*l == '#' && !stmt->len) {
			if (i == 0 && l[1] == '!')
				continue;
			block_add(block, l);
			continue;
		}
		if (block->len) {
			g_free(about);
			about = first_phrase(block);
			g_string_truncate(block, 0);
		}
		if (*l == '\0' && !stmt->len) {
			/* A blank line ends what a comment describes. */
			g_free(about);
			about = NULL;
			last_bg = NULL;
			continue;
		}

		/* Joined across backslash-newline. */
		if (!stmt->len)
			stmt_line = i + 1;
		n = strlen(l);
		while (n && (l[n - 1] == ' ' || l[n - 1] == '\t'))
			l[--n] = '\0';
		if (n && l[n - 1] == '\\') {
			g_string_append_len(stmt, l, (gssize)n - 1);
			g_string_append_c(stmt, ' ');
			continue;
		}
		/* A list or pipeline goes on after "&&", "||" or "|". */
		if (n && (l[n - 1] == '|' ||
			  (n > 1 && l[n - 1] == '&' && l[n - 2] == '&'))) {
			g_string_append(stmt, l);
			g_string_append_c(stmt, ' ');
			continue;
		}
		g_string_append(stmt, l);

		w = g_strsplit_set(stmt->str, " \t", -1);
		while (w[k] && !w[k][0])
			k++;
		exec = w[k] && strcmp(w[k], "exec") == 0;
		if (exec) {
			k++;
			while (w[k] && !w[k][0])
				k++;
		}

		if (w[k] && last_bg && g_str_has_suffix(stmt->str, "$!") &&
		    assignment(w[k])) {
			/* "wm_pid=$!": the program just started is the one
			 * the script waits for, the session's own. */
			last_bg->session = TRUE;
			last_bg->background = FALSE;
		} else if (w[k] && !shell_word(w[k]) && !assignment(w[k]) &&
			   w[k][0] != '$' && w[k][0] != '"' && w[k][0] != '(' &&
			   w[k][0] != '#') {
			TvSessionEntry *e = g_new0(TvSessionEntry, 1);
			GString *cmd = g_string_new(NULL);
			gboolean bg = FALSE;

			for (int j = k; w[j]; j++) {
				char *word = w[j];
				size_t wl;

				if (!word[0])
					continue;
				if (word[0] == '#' || word[0] == ';' ||
				    word[0] == '|' || word[0] == '>' ||
				    word[0] == '<' || strcmp(word, "&&") == 0)
					break;
				wl = strlen(word);
				if (strcmp(word, "&") == 0) {
					bg = TRUE;
					break;
				}
				if (word[wl - 1] == '&' &&
				    (wl < 2 || word[wl - 2] != '&')) {
					word[wl - 1] = '\0';
					bg = TRUE;
				}
				if (cmd->len)
					g_string_append_c(cmd, ' ');
				g_string_append(cmd, word);
				if (bg)
					break;
			}
			{
				/* Named for the first word of the command. */
				char *first = g_strndup(
					cmd->str, strcspn(cmd->str, " "));
				e->name = base_name(first);
				g_free(first);
			}
			e->command = g_string_free(cmd, FALSE);
			e->description = g_strdup(about);
			e->background = bg;
			e->session = exec;
			e->line = stmt_line;

			/* One row per program. */
			for (guint j = 0; j < list->len; j++) {
				TvSessionEntry *o = list->pdata[j];
				if (strcmp(o->name, e->name) == 0) {
					session_entry_free(e);
					e = NULL;
					break;
				}
			}
			if (e) {
				g_ptr_array_add(list, e);
				if (bg)
					added_bg = e;
			}
		}
		last_bg = added_bg;
		g_strfreev(w);
		g_string_truncate(stmt, 0);
	}
	g_free(about);
	g_string_free(block, TRUE);
	g_string_free(stmt, TRUE);
	g_strfreev(lines);
	return list;
}

/* ==========================================================================
 * Reading the system
 * ========================================================================== */

/* A whole file, for the pseudo files whose size says nothing. */
char *tv_read_text(const char *path)
{
	GString *s;
	char buf[8192];
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	s = g_string_new(NULL);
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		g_string_append_len(s, buf, n);
		if (s->len > 4 * 1024 * 1024)
			break;
	}
	close(fd);
	return g_string_free(s, FALSE);
}

/* The first line of a small file, trimmed; NULL when unreadable. */
static char *read_line(const char *path)
{
	char *t = tv_read_text(path);

	if (t) {
		char *nl = strchr(t, '\n');
		if (nl)
			*nl = '\0';
		g_strstrip(t);
	}
	return t;
}

static gint64 read_number(const char *path, gint64 fallback)
{
	char *t = read_line(path);
	char *end;
	gint64 v;

	if (!t)
		return fallback;
	v = g_ascii_strtoll(t, &end, 10);
	if (end == t)
		v = fallback;
	g_free(t);
	return v;
}

/* ---- the processor ------------------------------------------------------ */

#ifdef TV_HAVE_CPUID
static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
	__cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
}

/* Adds the caches one CPUID leaf (4, or 0x8000001D) describes. */
static void cpu_caches(TvCpuInfo *ci, unsigned leaf)
{
	for (unsigned sub = 0; sub < 16; sub++) {
		unsigned r[4];
		unsigned type, level, sharing;
		guint64 size, instances;

		cpuid(leaf, sub, r);
		type = r[0] & 0x1f;
		if (type == 0)
			break;
		level = (r[0] >> 5) & 7;
		sharing = ((r[0] >> 14) & 0xfff) + 1;
		size = (guint64)((r[1] >> 22) + 1) *
		       (((r[1] >> 12) & 0x3ff) + 1) * ((r[1] & 0xfff) + 1) *
		       ((guint64)r[2] + 1);
		instances = ((guint64)ci->logical + sharing - 1) / sharing;
		if (instances < 1)
			instances = 1;
		if (level == 1)
			ci->l1_bytes += size * instances;
		else if (level == 2)
			ci->l2_bytes += size * instances;
		else if (level == 3)
			ci->l3_bytes += size * instances;
	}
}

static void cpu_identify(TvCpuInfo *ci)
{
	unsigned r[4], max, ext;
	int tpc = 0, lpp = 0;

	cpuid(0, 0, r);
	max = r[0];
	memcpy(ci->vendor, &r[1], 4);
	memcpy(ci->vendor + 4, &r[3], 4);
	memcpy(ci->vendor + 8, &r[2], 4);
	ci->vendor[12] = '\0';

	cpuid(0x80000000, 0, r);
	ext = r[0];
	if (ext >= 0x80000004) {
		char brand[49];
		for (unsigned i = 0; i < 3; i++) {
			cpuid(0x80000002 + i, 0, r);
			memcpy(brand + 16 * i, r, 16);
		}
		brand[48] = '\0';
		tv_squeeze_spaces(brand);
		g_strlcpy(ci->model, brand, sizeof(ci->model));
	}

	if (max >= 1) {
		cpuid(1, 0, r);
		ci->virtual_machine = (r[2] >> 31) & 1;
		if ((r[3] >> 28) & 1)
			lpp = (r[1] >> 16) & 0xff;
	}
	if (max >= 0xb) {
		for (unsigned sub = 0; sub < 8; sub++) {
			unsigned type;
			cpuid(0xb, sub, r);
			type = (r[2] >> 8) & 0xff;
			if (type == 0)
				break;
			if (type == 1)
				tpc = r[1] & 0xffff;
			else if (type == 2)
				lpp = r[1] & 0xffff;
		}
	} else if (max >= 4 && strcmp(ci->vendor, "GenuineIntel") == 0 &&
		   lpp > 0) {
		cpuid(4, 0, r);
		tpc = lpp / (int)(((r[0] >> 26) & 0x3f) + 1);
	}
	tv_cpu_topology(ci->logical, tpc, lpp, &ci->sockets, &ci->cores);

	if (max >= 0x16) {
		cpuid(0x16, 0, r);
		ci->base_mhz = r[0] & 0xffff;
	}
	if (ci->base_mhz <= 0)
		ci->base_mhz = tv_parse_brand_mhz(ci->model);

	if (max >= 4 && strcmp(ci->vendor, "GenuineIntel") == 0)
		cpu_caches(ci, 4);
	else if (ext >= 0x8000001d)
		cpu_caches(ci, 0x8000001d);
}
#else
static void cpu_identify(TvCpuInfo *ci)
{
	tv_cpu_topology(ci->logical, 0, 0, &ci->sockets, &ci->cores);
}
#endif

/* ---- the sampler -------------------------------------------------------- */

typedef struct {
	char name[32];
	char model[96];
	TvDiskKind kind;
	gboolean removable;
} DiskStatic;

typedef struct {
	char name[16];
	guint64 rx, tx;
} NetPrev;

struct TvSysinfo {
	TvCpuInfo info;
	TvCpuLoad load;
	double *per_cpu;
	long hz;

	TvTaskTicks *ticks; /* the previous listing, sorted */
	int nticks;
	double prev_time; /* when it was taken, clock ticks since boot */
	gint64 prev_mono; /* and on the monotonic clock, microseconds */
	gboolean have_prev;
#ifdef __LIKEOS__
	procinfo_t *procs;
	int proc_cap;
#endif

	TvMemInfo mem;

	GArray *disks; /* TvDisk */
	GArray *disk_prev; /* TvDiskStat */
	GHashTable *disk_static; /* name -> DiskStatic */
	char root_disk[32];
	gint64 disk_mono;

	GArray *nets; /* TvNet */
	GArray *net_prev; /* NetPrev */
	gint64 net_mono;
};

static void cpu_static(TvSysinfo *s)
{
	char *t = tv_read_text("/proc/cpuinfo");
	double mhz = 0;
	int n = t ? tv_parse_cpuinfo(t, &mhz) : 0;

	g_free(t);
	if (n < 1)
		n = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (n < 1)
		n = 1;
	s->info.logical = n;
	s->info.speed_mhz = mhz;
	cpu_identify(&s->info);
	if (s->info.base_mhz <= 0)
		s->info.base_mhz = mhz;
	if (s->info.sockets < 1)
		s->info.sockets = 1;
	s->per_cpu = g_new0(double, n);
	s->load.ncpu = n;
	s->load.per_cpu = s->per_cpu;
}

TvSysinfo *tv_sysinfo_new(void)
{
	TvSysinfo *s = g_new0(TvSysinfo, 1);

	s->hz = sysconf(_SC_CLK_TCK);
	if (s->hz <= 0)
		s->hz = 100;
	cpu_static(s);
	s->disks = g_array_new(FALSE, TRUE, sizeof(TvDisk));
	s->disk_prev = g_array_new(FALSE, TRUE, sizeof(TvDiskStat));
	s->disk_static = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
					       g_free);
	s->nets = g_array_new(FALSE, TRUE, sizeof(TvNet));
	s->net_prev = g_array_new(FALSE, TRUE, sizeof(NetPrev));
	return s;
}

void tv_sysinfo_free(TvSysinfo *s)
{
	if (!s)
		return;
	g_free(s->per_cpu);
	g_free(s->ticks);
#ifdef __LIKEOS__
	g_free(s->procs);
#endif
	g_array_unref(s->disks);
	g_array_unref(s->disk_prev);
	g_hash_table_unref(s->disk_static);
	g_array_unref(s->nets);
	g_array_unref(s->net_prev);
	g_free(s);
}

/* ---- processor load ---- */

#ifdef __LIKEOS__
/* The whole process listing, the buffer grown until it holds it. */
static int list_tasks(procinfo_t **buf, int *cap)
{
	for (;;) {
		int n;

		if (!*buf) {
			*cap = 512;
			*buf = g_new(procinfo_t, *cap);
		}
		n = getprocinfo(*buf, *cap);
		if (n < 0)
			return 0;
		if (n < *cap || *cap >= 32768)
			return n;
		*cap *= 2;
		g_free(*buf);
		*buf = g_new(procinfo_t, *cap);
	}
}
#endif

static void sample_cpu(TvSysinfo *s, double now_ticks, gint64 mono)
{
	TvCpuLoad *l = &s->load;
	TvTaskTicks *cur = NULL;
	int ncur = 0;

	l->processes = l->threads = 0;
#ifdef __LIKEOS__
	{
		int n = list_tasks(&s->procs, &s->proc_cap);

		cur = g_new(TvTaskTicks, n > 0 ? n : 1);
		for (int i = 0; i < n; i++) {
			const procinfo_t *p = &s->procs[i];

			if (p->state == 4) /* a zombie runs nothing */
				continue;
			cur[ncur].tid = p->pid;
			cur[ncur].cpu = p->on_cpu;
			cur[ncur].ticks = p->utime_ticks + p->stime_ticks;
			cur[ncur].start = p->start_tick;
			ncur++;
			l->threads++;
			if (p->pid == p->tgid)
				l->processes++;
		}
	}
#endif
	if (cur)
		qsort(cur, (size_t)ncur, sizeof(*cur), task_cmp);
	if (s->have_prev && cur) {
		double elapsed = (double)(mono - s->prev_mono) / 1e6 *
				 (double)s->hz;
		/* Too short an interval to measure in 100 Hz ticks keeps the
		 * last figures. */
		if (elapsed >= 0.2 * (double)s->hz) {
			tv_cpu_usage(s->ticks, s->nticks, cur, ncur,
				     s->prev_time, elapsed, l->ncpu, s->per_cpu,
				     &l->total);
			l->valid = TRUE;
		} else {
			g_free(cur);
			return;
		}
	}
	g_free(s->ticks);
	s->ticks = cur;
	s->nticks = ncur;
	s->prev_time = now_ticks;
	s->prev_mono = mono;
	s->have_prev = cur != NULL;
}

/* ---- disks ---- */

/* The device names the usb-* links of /dev/disk/by-id lead to. */
static GHashTable *usb_disks(void)
{
	GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, NULL);
	DIR *d = opendir("/dev/disk/by-id");
	struct dirent *e;

	if (!d)
		return set;
	while ((e = readdir(d))) {
		char path[PATH_MAX], target[PATH_MAX];
		ssize_t n;

		if (strncmp(e->d_name, "usb-", 4) != 0)
			continue;
		g_snprintf(path, sizeof(path), "/dev/disk/by-id/%s", e->d_name);
		n = readlink(path, target, sizeof(target) - 1);
		if (n <= 0)
			continue;
		target[n] = '\0';
		g_hash_table_add(set, base_name(target));
	}
	closedir(d);
	return set;
}

/* The disk a device name belongs to (itself when it is a disk). */
static gboolean disk_of(const char *dev, char *out, gsize size)
{
	char path[PATH_MAX], target[PATH_MAX];
	ssize_t n;

	g_snprintf(path, sizeof(path), "/sys/class/block/%s", dev);
	n = readlink(path, target, sizeof(target) - 1);
	if (n <= 0)
		return FALSE;
	target[n] = '\0';
	return tv_disk_of_link(target, out, size);
}

/* The disk the root filesystem is on: by the device number "/" reports,
 * through /sys/dev/block; by the source /proc/mounts names when that
 * does not lead to one. */
static void find_root_disk(TvSysinfo *s)
{
	struct stat st;
	char *mounts;
	char src[256];

	s->root_disk[0] = '\0';
	if (stat("/", &st) == 0 && major(st.st_dev) != 0) {
		char path[64], target[PATH_MAX];
		ssize_t n;

		g_snprintf(path, sizeof(path), "/sys/dev/block/%u:%u",
			   major(st.st_dev), minor(st.st_dev));
		n = readlink(path, target, sizeof(target) - 1);
		if (n > 0) {
			target[n] = '\0';
			if (tv_disk_of_link(target, s->root_disk,
					    sizeof(s->root_disk)))
				return;
		}
	}

	mounts = tv_read_text("/proc/mounts");
	if (mounts && tv_parse_root_source(mounts, src, sizeof(src)) &&
	    g_str_has_prefix(src, "/dev/")) {
		char real[PATH_MAX];
		char *name;

		if (!realpath(src, real))
			g_strlcpy(real, src, sizeof(real));
		name = base_name(real);
		if (!disk_of(name, s->root_disk, sizeof(s->root_disk)))
			g_strlcpy(s->root_disk, name, sizeof(s->root_disk));
		g_free(name);
	}
	g_free(mounts);
}

static gboolean skipped_disk(const char *name)
{
	return g_str_has_prefix(name, "loop") || g_str_has_prefix(name, "ram") ||
	       g_str_has_prefix(name, "zram") || g_str_has_prefix(name, "dm-");
}

static DiskStatic *disk_static(TvSysinfo *s, const char *name,
			       GHashTable **usb)
{
	DiskStatic *d = g_hash_table_lookup(s->disk_static, name);
	char path[PATH_MAX];
	char *t;

	if (d)
		return d;
	if (!*usb)
		*usb = usb_disks();
	d = g_new0(DiskStatic, 1);
	g_strlcpy(d->name, name, sizeof(d->name));
	g_snprintf(path, sizeof(path), "/sys/block/%s/device/model", name);
	t = read_line(path);
	if (t) {
		g_strlcpy(d->model, t, sizeof(d->model));
		tv_squeeze_spaces(d->model);
		g_free(t);
	}
	g_snprintf(path, sizeof(path), "/sys/block/%s/removable", name);
	d->removable = read_number(path, 0) == 1;
	g_snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", name);
	if (g_str_has_prefix(name, "sr"))
		d->kind = TV_DISK_OPTICAL;
	else if (g_hash_table_contains(*usb, name))
		d->kind = TV_DISK_USB;
	else if (d->removable)
		d->kind = TV_DISK_REMOVABLE;
	else if (g_str_has_prefix(name, "nvme"))
		d->kind = TV_DISK_NVME;
	else
		switch (read_number(path, -1)) {
		case 0:
			d->kind = TV_DISK_SSD;
			break;
		case 1:
			d->kind = TV_DISK_HDD;
			break;
		default:
			d->kind = TV_DISK_UNKNOWN;
		}
	g_hash_table_insert(s->disk_static, d->name, d);
	return d;
}

static const TvDiskStat *prev_stat(GArray *prev, const char *name)
{
	for (guint i = 0; i < prev->len; i++) {
		const TvDiskStat *d = &g_array_index(prev, TvDiskStat, i);
		if (strcmp(d->name, name) == 0)
			return d;
	}
	return NULL;
}

static void sample_disks(TvSysinfo *s, gint64 mono)
{
	char *text = tv_read_text("/proc/diskstats");
	TvDiskStat st[64];
	int n = text ? tv_parse_diskstats(text, st, 64) : 0;
	double secs = s->disk_mono ? (double)(mono - s->disk_mono) / 1e6 : 0;
	GHashTable *usb = NULL;
	GArray *now = g_array_new(FALSE, TRUE, sizeof(TvDiskStat));

	g_free(text);
	find_root_disk(s);
	g_array_set_size(s->disks, 0);
	for (int i = 0; i < n; i++) {
		char path[PATH_MAX];
		struct stat sb;
		TvDisk disk;
		const DiskStatic *ds;
		const TvDiskStat *p;

		/* Whole disks only: the partitions are not under
		 * /sys/block. */
		g_snprintf(path, sizeof(path), "/sys/block/%s", st[i].name);
		if (skipped_disk(st[i].name) || stat(path, &sb) != 0)
			continue;
		g_array_append_val(now, st[i]);
		ds = disk_static(s, st[i].name, &usb);
		memset(&disk, 0, sizeof(disk));
		g_strlcpy(disk.name, ds->name, sizeof(disk.name));
		g_strlcpy(disk.model, ds->model, sizeof(disk.model));
		disk.kind = ds->kind;
		disk.removable = ds->removable;
		disk.index = (int)s->disks->len;
		g_snprintf(path, sizeof(path), "/sys/block/%s/size", ds->name);
		disk.size = (guint64)MAX(read_number(path, 0), 0) * 512;
		disk.system = strcmp(ds->name, s->root_disk) == 0;
		p = prev_stat(s->disk_prev, ds->name);
		if (p && secs > 0)
			tv_disk_rates(p, &st[i], secs, &disk);
		g_array_append_val(s->disks, disk);
	}
	if (usb)
		g_hash_table_unref(usb);

	/* Forget what was learnt of disks that went away, so a different
	 * one plugged in under the same name is looked at afresh. */
	{
		GHashTableIter it;
		gpointer key;

		g_hash_table_iter_init(&it, s->disk_static);
		while (g_hash_table_iter_next(&it, &key, NULL))
			if (!prev_stat(now, key))
				g_hash_table_iter_remove(&it);
	}
	g_array_unref(s->disk_prev);
	s->disk_prev = now;
	s->disk_mono = mono;
}

/* ---- network ---- */

static void sample_nets(TvSysinfo *s, gint64 mono)
{
	GArray *now = g_array_new(FALSE, TRUE, sizeof(NetPrev));

	g_array_set_size(s->nets, 0);
#ifdef __LIKEOS__
	{
		double secs = s->net_mono ?
				      (double)(mono - s->net_mono) / 1e6 :
				      0;
		net_iface_info_t ifs[16];
		int n = net_getinfo(NET_GET_IFACE_INFO, ifs, 16);

		for (int i = 0; i < n; i++) {
			const net_iface_info_t *f = &ifs[i];
			TvNet net;
			NetPrev np;
			guint32 ip = f->ip_addr;

			if (f->flags & IFF_LOOPBACK)
				continue;
			memset(&net, 0, sizeof(net));
			memset(&np, 0, sizeof(np));
			g_strlcpy(net.name, f->name, sizeof(net.name));
			if (ip)
				g_snprintf(net.ipv4, sizeof(net.ipv4),
					   "%u.%u.%u.%u", ip >> 24,
					   (ip >> 16) & 255, (ip >> 8) & 255,
					   ip & 255);
			g_snprintf(net.mac, sizeof(net.mac),
				   "%02x:%02x:%02x:%02x:%02x:%02x", f->mac[0],
				   f->mac[1], f->mac[2], f->mac[3], f->mac[4],
				   f->mac[5]);
			net.wireless = g_str_has_prefix(f->name, "wl");
			net.up = (f->flags & IFF_UP) != 0;
			g_strlcpy(np.name, f->name, sizeof(np.name));
			np.rx = f->rx_bytes;
			np.tx = f->tx_bytes;
			for (guint j = 0; j < s->net_prev->len && secs > 0;
			     j++) {
				const NetPrev *o =
					&g_array_index(s->net_prev, NetPrev, j);
				if (strcmp(o->name, np.name) != 0)
					continue;
				net.rx_bps = (double)delta(o->rx, np.rx) * 8.0 /
					     secs;
				net.tx_bps = (double)delta(o->tx, np.tx) * 8.0 /
					     secs;
			}
			g_array_append_val(s->nets, net);
			g_array_append_val(now, np);
		}
	}
#endif
	g_array_unref(s->net_prev);
	s->net_prev = now;
	s->net_mono = mono;
}

void tv_sysinfo_sample(TvSysinfo *s)
{
	gint64 mono = g_get_monotonic_time();
	char *t;
	double up;

	t = tv_read_text("/proc/uptime");
	up = tv_parse_uptime(t);
	g_free(t);
	if (up < 0)
		up = (double)mono / 1e6;
	s->load.uptime_s = (guint64)up;

	/* The clock may change with the load where the system reports it. */
	t = tv_read_text("/proc/cpuinfo");
	if (t) {
		double mhz = 0;
		tv_parse_cpuinfo(t, &mhz);
		if (mhz > 0)
			s->info.speed_mhz = mhz;
		g_free(t);
	}

	sample_cpu(s, up * (double)s->hz, mono);

	t = tv_read_text("/proc/meminfo");
	if (!t || !tv_parse_meminfo(t, &s->mem))
		memset(&s->mem, 0, sizeof(s->mem));
	g_free(t);

	sample_disks(s, mono);
	sample_nets(s, mono);
}

const TvCpuInfo *tv_sysinfo_cpu_info(TvSysinfo *s)
{
	return &s->info;
}

const TvCpuLoad *tv_sysinfo_cpu(TvSysinfo *s)
{
	return &s->load;
}

const TvMemInfo *tv_sysinfo_mem(TvSysinfo *s)
{
	return &s->mem;
}

int tv_sysinfo_n_disks(TvSysinfo *s)
{
	return (int)s->disks->len;
}

const TvDisk *tv_sysinfo_disk(TvSysinfo *s, int i)
{
	return &g_array_index(s->disks, TvDisk, i);
}

int tv_sysinfo_n_nets(TvSysinfo *s)
{
	return (int)s->nets->len;
}

const TvNet *tv_sysinfo_net(TvSysinfo *s, int i)
{
	return &g_array_index(s->nets, TvNet, i);
}

/* ---- running programs ---- */

GArray *tv_proc_snapshot(void)
{
	GArray *a = g_array_new(FALSE, TRUE, sizeof(TvProcName));
#ifdef __LIKEOS__
	/* Kept for the next call: the listing is large and asked for at
	 * every tick of the pages that use it (all on the main thread). */
	static procinfo_t *buf;
	static int cap;
	int n = list_tasks(&buf, &cap);

	for (int i = 0; i < n; i++) {
		const procinfo_t *p = &buf[i];
		TvProcName pn;

		if (p->is_kernel || p->pid != p->tgid || p->state == 4)
			continue;
		memset(&pn, 0, sizeof(pn));
		pn.pid = p->pid;
		pn.ppid = p->ppid;
		pn.uid = p->uid;
		g_strlcpy(pn.name, p->comm, sizeof(pn.name));
		g_array_append_val(a, pn);
	}
#endif
	return a;
}

int tv_proc_find(GArray *procs, const char *name, int uid,
		 gboolean init_child)
{
	int any = 0;

	for (guint i = 0; procs && i < procs->len; i++) {
		const TvProcName *p = &g_array_index(procs, TvProcName, i);

		if (strcmp(p->name, name) != 0 || (uid >= 0 && p->uid != uid))
			continue;
		if (!init_child || p->ppid == 1)
			return p->pid;
		if (!any)
			any = p->pid;
	}
	return any;
}
