/* Host test driver for the libinstaller tar.zst extractor.
 *
 *   extract_test [options] ARCHIVE TARGET
 *     --sha HEX          expected SHA-256 of the archive file
 *     --no-owner         do not chown (same_owner = 0)
 *     --owner-fatal      chown failures are errors
 *     --no-xattrs        leave extended attributes out
 *     --no-acls          leave ACLs out
 *     --cancel-after N   call il_cancel() once N entries are done
 *     --expect ERRNAME   the extraction must fail with this error
 *                        (EBADMSG, EPERM, ELOOP, ECANCELED, ENOTDIR,
 *                        ENOSYS, EINVAL)
 *     --log PATH         the libinstaller log file
 *
 * Prints the statistics; exits 0 when the outcome is the expected one.
 * Every progress report is checked on the way: the entry it names must be
 * a readable path (a name read from freed memory shows up as bytes no
 * archive name has), whatever the outcome.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "il_internal.h"

/* il_ctx.c undoes mounts when a context is freed; nothing is mounted by
 * this test, so the call only has to exist. */
int il_umount_all(il_ctx *ctx)
{
	(void)ctx;
	return 0;
}

struct state {
	il_ctx *ctx;
	long cancel_after;
	unsigned calls;
	unsigned bad_paths;
	uint64_t last_comp;
};

/* A name the archives of these tests can carry: UTF-8 text without control
 * characters. */
static int readable_path(const char *s)
{
	const unsigned char *u = (const unsigned char *)s;

	while (*u) {
		int n = *u < 0x80 ? 0 : (*u & 0xe0) == 0xc0 ? 1 :
			(*u & 0xf0) == 0xe0 ? 2 : (*u & 0xf8) == 0xf0 ? 3 : -1;

		if (*u < 0x20 || *u == 0x7f || n < 0)
			return 0;
		u++;
		for (; n > 0; n--, u++)
			if ((*u & 0xc0) != 0x80)
				return 0;
	}
	return 1;
}

static void progress(const struct il_extract_progress *p, void *user)
{
	struct state *s = user;

	struct timespec pause = { 0, 1100000 };

	s->calls++;
	if (!p->path || !readable_path(p->path)) {
		if (s->bad_paths++ < 5)
			fprintf(stderr, "extract_test: progress names an "
				"unreadable path after %llu entries\n",
				(unsigned long long)p->files_done);
	}
	/* past the 1 ms interval: the next entry reports again */
	nanosleep(&pause, NULL);
	if (p->compressed_done < s->last_comp) {
		fprintf(stderr, "extract_test: progress went backwards\n");
		exit(3);
	}
	s->last_comp = p->compressed_done;
	if (s->cancel_after >= 0 && p->files_done >= (uint64_t)s->cancel_after)
		il_cancel(s->ctx);
}

static int errno_by_name(const char *n)
{
	static const struct {
		const char *name;
		int value;
	} t[] = {
		{ "EBADMSG", EBADMSG }, { "EPERM", EPERM }, { "ELOOP", ELOOP },
		{ "ECANCELED", ECANCELED }, { "ENOTDIR", ENOTDIR },
		{ "ENOSYS", ENOSYS }, { "EINVAL", EINVAL },
		{ "ENAMETOOLONG", ENAMETOOLONG }, { "ENOENT", ENOENT },
	};

	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (strcmp(t[i].name, n) == 0)
			return t[i].value;
	fprintf(stderr, "extract_test: unknown error name %s\n", n);
	exit(2);
}

int main(int argc, char **argv)
{
	struct il_options o;
	struct il_extract_opts xo;
	struct il_extract_stats st;
	struct state s;
	const char *sha = NULL, *log = NULL;
	int expect = 0, same_owner = 1, fatal = 0, xattrs = 1, acls = 1;
	int i, rc;

	memset(&s, 0, sizeof(s));
	s.cancel_after = -1;
	for (i = 1; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--sha") && i + 1 < argc)
			sha = argv[++i];
		else if (!strcmp(argv[i], "--no-owner"))
			same_owner = 0;
		else if (!strcmp(argv[i], "--owner-fatal"))
			fatal = 1;
		else if (!strcmp(argv[i], "--no-xattrs"))
			xattrs = 0;
		else if (!strcmp(argv[i], "--no-acls"))
			acls = 0;
		else if (!strcmp(argv[i], "--cancel-after") && i + 1 < argc)
			s.cancel_after = atol(argv[++i]);
		else if (!strcmp(argv[i], "--expect") && i + 1 < argc)
			expect = errno_by_name(argv[++i]);
		else if (!strcmp(argv[i], "--log") && i + 1 < argc)
			log = argv[++i];
		else {
			fprintf(stderr, "extract_test: bad option %s\n", argv[i]);
			return 2;
		}
	}
	if (argc - i != 2) {
		fprintf(stderr, "usage: extract_test [options] ARCHIVE TARGET\n");
		return 2;
	}
	memset(&o, 0, sizeof(o));
	o.log_path = log ? log : "/dev/null";
	o.simulate = 1;
	s.ctx = il_ctx_new(&o);
	if (!s.ctx) {
		fprintf(stderr, "extract_test: no context\n");
		return 2;
	}
	memset(&xo, 0, sizeof(xo));
	xo.archive = argv[i];
	xo.target = argv[i + 1];
	xo.expected_sha256 = sha;
	xo.same_owner = same_owner;
	xo.owner_errors_fatal = fatal;
	xo.xattrs = xattrs;
	xo.acls = acls;
	xo.progress = progress;
	xo.user = &s;
	xo.progress_interval_ms = 1;
	rc = il_extract_archive(s.ctx, &xo, &st);
	printf("rc=%d (%s) entries=%llu files=%llu dirs=%llu symlinks=%llu "
	       "hardlinks=%llu devices=%llu fifos=%llu bytes=%llu "
	       "xattrs=%llu xattr_fail=%llu acl_fail=%llu owner_fail=%llu "
	       "time_fail=%llu skipped=%llu progress_calls=%u\nsha256=%s\n",
	       rc, rc ? strerror(-rc) : "ok",
	       (unsigned long long)st.entries, (unsigned long long)st.files,
	       (unsigned long long)st.dirs, (unsigned long long)st.symlinks,
	       (unsigned long long)st.hardlinks,
	       (unsigned long long)st.devices, (unsigned long long)st.fifos,
	       (unsigned long long)st.bytes, (unsigned long long)st.xattrs_set,
	       (unsigned long long)st.xattr_failures,
	       (unsigned long long)st.acl_failures,
	       (unsigned long long)st.owner_failures,
	       (unsigned long long)st.time_failures,
	       (unsigned long long)st.skipped, s.calls, st.sha256);
	if (rc)
		printf("error: %s\n", il_last_error(s.ctx));
	il_ctx_free(s.ctx);
	if (s.bad_paths) {
		printf("FAIL: %u progress reports named an unreadable path\n",
		       s.bad_paths);
		return 4;
	}
	if (expect) {
		if (rc == -expect) {
			printf("expected failure: ok\n");
			return 0;
		}
		printf("FAIL: expected -%d, got %d\n", expect, rc);
		return 1;
	}
	return rc ? 1 : 0;
}
