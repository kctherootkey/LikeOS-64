/* libinstaller -- the system image (likeos-rootfs.tar.zst).
 *
 * Next to the archive the release build puts two small files:
 *   <archive>.sha256   "<64 hex digits>  likeos-rootfs.tar.zst"
 *   <archive>.info     key=value lines: version=, build=, bytes=
 *                      (uncompressed size), files= (entries)
 * Both are optional; without .info the root partition must have the
 * default minimum size, without .sha256 the archive is not checked.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "il_internal.h"

uint64_t il_required_root_bytes(const struct il_source_info *src)
{
	uint64_t need;

	if (!src || !src->uncompressed_bytes)
		return IL_ROOT_DEFAULT_MIN;
	/* A quarter more than the files need (block rounding, inodes, the
	 * journal) and a gigabyte of room to work in. */
	need = src->uncompressed_bytes + src->uncompressed_bytes / 4 +
	       IL_ROOT_SLACK_BYTES;
	/* Whole MiB. */
	need = (need + IL_ALIGN_BYTES - 1) / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
	return need;
}

static uint64_t il_parse_u64(const char *s)
{
	char *end;
	unsigned long long v;

	while (isspace((unsigned char)*s))
		s++;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || end == s)
		return 0;
	return (uint64_t)v;
}

/* Strips one pair of quotes around a value. */
static char *il_unquote(char *v)
{
	size_t n;

	v = il_trim(v);
	n = strlen(v);
	if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') ||
		       (v[0] == '\'' && v[n - 1] == '\''))) {
		v[n - 1] = '\0';
		v++;
	}
	return v;
}

static void il_source_read_info(il_ctx *ctx, const char *path,
				struct il_source_info *out)
{
	char *text = NULL, *save = NULL;

	if (il_read_file_alloc(path, &text, NULL) != 0) {
		il_log(ctx, IL_LOG_INFO, "%s: not found; the image's size is "
		       "unknown", path);
		return;
	}
	for (char *line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq, *key, *val;

		line = il_trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = il_trim(line);
		val = il_unquote(eq + 1);
		if (strcmp(key, "version") == 0)
			il_strlcpy(out->version, val, sizeof(out->version));
		else if (strcmp(key, "build") == 0 ||
			 strcmp(key, "build_date") == 0)
			il_strlcpy(out->build_date, val, sizeof(out->build_date));
		else if (strcmp(key, "bytes") == 0 ||
			 strcmp(key, "uncompressed_bytes") == 0)
			out->uncompressed_bytes = il_parse_u64(val);
		else if (strcmp(key, "files") == 0 ||
			 strcmp(key, "file_count") == 0)
			out->file_count = il_parse_u64(val);
	}
	free(text);
}

static void il_source_read_sha(il_ctx *ctx, const char *path,
			       struct il_source_info *out)
{
	char buf[512];
	char *p;
	int n = 0;

	if (il_read_file(path, buf, sizeof(buf)) < 0) {
		il_log(ctx, IL_LOG_INFO, "%s: not found; the image will not be "
		       "checked", path);
		return;
	}
	p = il_trim(buf);
	while (n < 64 && isxdigit((unsigned char)p[n]))
		n++;
	if (n != 64 || (p[n] && !isspace((unsigned char)p[n]))) {
		il_log(ctx, IL_LOG_WARN, "%s: not a SHA-256 sum; ignored", path);
		return;
	}
	for (int i = 0; i < 64; i++)
		out->sha256[i] = (char)tolower((unsigned char)p[i]);
	out->sha256[64] = '\0';
}

int il_source_probe(il_ctx *ctx, struct il_source_info *out)
{
	char path[IL_PATH_MAX + 16];
	struct stat st;

	if (!ctx || !out)
		return -EINVAL;
	il_clear_error(ctx);
	memset(out, 0, sizeof(*out));
	il_strlcpy(out->path, ctx->source, sizeof(out->path));
	if (stat(ctx->source, &st) != 0)
		return il_set_error(ctx, errno, "The LikeOS system image %s "
				    "cannot be found: %s", ctx->source,
				    strerror(errno));
	if (!S_ISREG(st.st_mode))
		return il_set_error(ctx, EINVAL, "The LikeOS system image %s is "
				    "not a file", ctx->source);
	out->present = 1;
	out->compressed_bytes = (uint64_t)st.st_size;
	snprintf(path, sizeof(path), "%s.info", ctx->source);
	il_source_read_info(ctx, path, out);
	snprintf(path, sizeof(path), "%s.sha256", ctx->source);
	il_source_read_sha(ctx, path, out);
	out->required_bytes = il_required_root_bytes(out);
	{
		char c[32], u[32], r[32];

		il_format_size(out->compressed_bytes, c, sizeof(c));
		il_format_size(out->uncompressed_bytes, u, sizeof(u));
		il_format_size(out->required_bytes, r, sizeof(r));
		il_log(ctx, IL_LOG_INFO, "system image %s: version %s, built %s, "
		       "%s (%s unpacked, %llu files), root needs %s%s",
		       out->path, out->version[0] ? out->version : "?",
		       out->build_date[0] ? out->build_date : "?", c,
		       out->uncompressed_bytes ? u : "size unknown",
		       (unsigned long long)out->file_count, r,
		       out->sha256[0] ? ", SHA-256 known" : "");
	}
	return 0;
}

int il_source_verify(il_ctx *ctx, void (*progress)(double fraction, void *user),
		     void *user)
{
	struct il_source_info info;
	char sum[65];
	int rc;

	if (!ctx)
		return -EINVAL;
	rc = il_source_probe(ctx, &info);
	if (rc)
		return rc;
	if (!info.sha256[0])
		return il_set_error(ctx, ENOENT, "There is no checksum (%s.sha256) "
				    "to check the system image against",
				    ctx->source);
	il_log(ctx, IL_LOG_INFO, "checking %s", ctx->source);
	rc = il_sha256_file(ctx->source, sum, progress, user,
			    &ctx->cancel_requested);
	if (rc == -ECANCELED) {
		ctx->cancel_requested = 0;
		return il_set_error(ctx, ECANCELED, "The check was cancelled");
	}
	if (rc)
		return il_set_error(ctx, -rc, "The system image cannot be read: "
				    "%s", strerror(-rc));
	if (strcmp(sum, info.sha256) != 0)
		return il_set_error(ctx, EBADMSG, "The system image is damaged: "
				    "its SHA-256 is %s, but %s was expected", sum,
				    info.sha256);
	il_log(ctx, IL_LOG_INFO, "the system image is intact (SHA-256 %s)", sum);
	return 0;
}
