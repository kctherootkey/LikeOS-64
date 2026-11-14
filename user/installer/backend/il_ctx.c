/* libinstaller -- the context: options, errors, cancellation.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_internal.h"

const char *il_version(void)
{
	return IL_VERSION_STRING;
}

/* A tool path: the environment variable when it names something, else the
 * built-in default. */
static void il_tool(char *dst, size_t len, const char *env, const char *def)
{
	const char *v = getenv(env);

	if (v && v[0] == '/')
		il_strlcpy(dst, v, len);
	else
		il_strlcpy(dst, def, len);
}

static void il_copy_opt(char *dst, size_t len, const char *v, const char *def)
{
	if (v && v[0])
		il_strlcpy(dst, v, len);
	else
		il_strlcpy(dst, def ? def : "", len);
	/* No trailing slash (except for "/" itself): paths are joined. */
	{
		size_t n = strlen(dst);

		while (n > 1 && dst[n - 1] == '/')
			dst[--n] = '\0';
	}
}

il_ctx *il_ctx_new(const struct il_options *opt)
{
	il_ctx *ctx = calloc(1, sizeof(*ctx));
	struct il_options none;

	if (!ctx)
		return NULL;
	if (!opt) {
		memset(&none, 0, sizeof(none));
		opt = &none;
	}
	il_copy_opt(ctx->source, sizeof(ctx->source), opt->source,
		    IL_DEFAULT_SOURCE);
	il_copy_opt(ctx->target, sizeof(ctx->target), opt->target,
		    IL_DEFAULT_TARGET);
	il_copy_opt(ctx->log_path, sizeof(ctx->log_path), opt->log_path,
		    IL_DEFAULT_LOG);
	il_copy_opt(ctx->sysroot, sizeof(ctx->sysroot), opt->sysroot, "");
	if (strcmp(ctx->sysroot, "/") == 0)
		ctx->sysroot[0] = '\0';
	ctx->dry_run = opt->dry_run ? 1 : 0;
	ctx->simulate = opt->simulate ? 1 : 0;
	ctx->verbose = opt->verbose ? 1 : 0;
	ctx->log_cb = opt->log_cb;
	ctx->log_user = opt->log_user;
	ctx->log_fd = -1;
	ctx->kmsg_fd = -1;
	ctx->cancel_allowed = 1;
	if (opt->simulate_disks) {
		for (int i = 0; opt->simulate_disks[i] &&
				ctx->nsim_disks < IL_MAX_SIM_DISKS; i++) {
			char *s = strdup(opt->simulate_disks[i]);

			if (!s) {
				il_ctx_free(ctx);
				return NULL;
			}
			ctx->sim_disks[ctx->nsim_disks++] = s;
		}
	}
	il_tool(ctx->tool_mkfs_fat, sizeof(ctx->tool_mkfs_fat), "IL_MKFS_FAT",
		IL_TOOL_MKFS_FAT);
	il_tool(ctx->tool_mke2fs, sizeof(ctx->tool_mke2fs), "IL_MKE2FS",
		IL_TOOL_MKE2FS);
	il_tool(ctx->tool_e2fsck, sizeof(ctx->tool_e2fsck), "IL_E2FSCK",
		IL_TOOL_E2FSCK);
	il_tool(ctx->tool_tune2fs, sizeof(ctx->tool_tune2fs), "IL_TUNE2FS",
		IL_TOOL_TUNE2FS);
	il_tool(ctx->tool_efiboot, sizeof(ctx->tool_efiboot), "IL_EFIBOOT",
		IL_TOOL_EFIBOOT);

	il_log_open(ctx);
	il_log(ctx, IL_LOG_INFO, "libinstaller %s (API %d)%s%s",
	       IL_VERSION_STRING, IL_API_VERSION,
	       ctx->simulate ? ", simulating on image files" : "",
	       ctx->dry_run ? ", dry run" : "");
	il_log(ctx, IL_LOG_DEBUG, "source %s, target %s, log %s, sysroot '%s'",
	       ctx->source, ctx->target, ctx->log_path, ctx->sysroot);
	return ctx;
}

void il_ctx_free(il_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->nmounts > 0)
		il_umount_all(ctx);
	il_log_close(ctx);
	for (int i = 0; i < ctx->nsim_disks; i++)
		free(ctx->sim_disks[i]);
	memset(ctx, 0, sizeof(*ctx));
	free(ctx);
}

const char *il_last_error(const il_ctx *ctx)
{
	if (!ctx)
		return "no installer context";
	return ctx->err[0] ? ctx->err : "no error";
}

const char *il_log_path(const il_ctx *ctx)
{
	return ctx ? ctx->log_path : IL_DEFAULT_LOG;
}

int il_set_error(il_ctx *ctx, int err, const char *fmt, ...)
{
	va_list ap;

	if (err < 0)
		err = -err;
	if (err == 0)
		err = EIO;
	if (!ctx)
		return -err;
	va_start(ap, fmt);
	vsnprintf(ctx->err, sizeof(ctx->err), fmt, ap);
	va_end(ap);
	ctx->last_errno = err;
	il_log(ctx, IL_LOG_ERROR, "%s", ctx->err);
	return -err;
}

void il_clear_error(il_ctx *ctx)
{
	if (ctx) {
		ctx->err[0] = '\0';
		ctx->last_errno = 0;
	}
}

void il_sys_path(const il_ctx *ctx, char *buf, size_t len, const char *path)
{
	if (ctx && ctx->sysroot[0])
		snprintf(buf, len, "%s%s", ctx->sysroot, path);
	else
		il_strlcpy(buf, path, len);
}

/* ---- cancellation ------------------------------------------------- */

void il_cancel(il_ctx *ctx)
{
	if (!ctx)
		return;
	__atomic_store_n(&ctx->cancel_requested, 1, __ATOMIC_SEQ_CST);
}

int il_can_cancel(const il_ctx *ctx)
{
	if (!ctx)
		return 0;
	if (!__atomic_load_n(&ctx->installing, __ATOMIC_SEQ_CST))
		return 1;
	return __atomic_load_n(&ctx->cancel_allowed, __ATOMIC_SEQ_CST);
}

int il_cancelled(const il_ctx *ctx)
{
	if (!ctx)
		return 0;
	return __atomic_load_n(&ctx->cancel_requested, __ATOMIC_SEQ_CST) &&
	       __atomic_load_n(&ctx->cancel_allowed, __ATOMIC_SEQ_CST);
}
