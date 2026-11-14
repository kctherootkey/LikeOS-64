/* libinstaller -- the installation, step by step.
 *
 *   prepare     check the choices, the image and the plan; unmount leftovers
 *   partition   write the partition table (no way back after this)
 *   format      mkfs.fat, mke2fs, e2fsck
 *   mount       the new root and its EFI system partition
 *   extract     unpack likeos-rootfs.tar.zst (three quarters of the time)
 *   configure   fstab, host name, keyboard, time zone, locale, accounts
 *   bootloader  the ESP files and the firmware boot entry
 *   finalize    the log into the new system, sync, unmount
 *
 * Progress is reported per step and weighted over the whole, at most ten
 * times a second (and at every step and every detail line); il_progress.c
 * models how far the unpacking is and how long everything still takes.
 * A failure is logged, whatever was mounted is unmounted again, and the
 * error returned.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_EMIT_INTERVAL_MS  100

static const char *const g_titles[IL_STEP_COUNT] = {
	"Getting ready",
	"Partitioning the disk",
	"Formatting",
	"Mounting the new system",
	"Copying LikeOS files",
	"Configuring LikeOS",
	"Installing the boot loader",
	"Finishing up",
};

const char *il_step_title(int step)
{
	if (step < 0 || step >= IL_STEP_COUNT)
		return "";
	return g_titles[step];
}

struct il_run {
	il_progress_cb cb;
	void *user;
	/* The detail line handed to the callback.  It is rewritten only on
	 * this thread and only between two callbacks, so it stays as it is
	 * for the whole of one; a front end that shows it later copies it. */
	char detail[512];
	uint64_t last_emit;
	struct il_track track;
};

static void il_emit(il_ctx *ctx, int force)
{
	struct il_run *r = ctx->run;
	struct il_progress p;
	uint64_t now;

	if (!r || !r->cb)
		return;
	now = il_now_ms();
	if (!force && now - r->last_emit < IL_EMIT_INTERVAL_MS)
		return;
	r->last_emit = now;
	memset(&p, 0, sizeof(p));
	il_track_report(&r->track, now, &p);
	p.detail = r->detail;
	r->cb(&p, r->user);
}

void il_step_progress(il_ctx *ctx, double fraction, const char *detail)
{
	struct il_run *r;

	if (!ctx || !(r = ctx->run))
		return;
	if (fraction < 0.0)
		fraction = 0.0;
	if (fraction > 1.0)
		fraction = 1.0;
	il_track_fraction(&r->track, fraction);
	if (detail) {
		il_strlcpy(r->detail, detail, sizeof(r->detail));
		il_log(ctx, IL_LOG_DEBUG, "%s", detail);
	}
	il_emit(ctx, fraction >= 1.0 || detail != NULL);
}

static void il_step_begin(il_ctx *ctx, int step)
{
	struct il_run *r = ctx->run;

	il_track_step(&r->track, step, il_now_ms());
	il_strlcpy(r->detail, il_step_title(step), sizeof(r->detail));
	il_log(ctx, IL_LOG_INFO, "step %d/%d: %s", step + 1, IL_STEP_COUNT,
	       il_step_title(step));
	il_emit(ctx, 1);
}

static void il_step_end(il_ctx *ctx)
{
	struct il_run *r = ctx->run;

	il_track_fraction(&r->track, 1.0);
	il_emit(ctx, 1);
}

/* ---- extract progress ------------------------------------------------- */

/* Simulate mode only: a pause per entry / per MiB, so that a simulation
 * on a fast host can unpack at the pace of a slow disk (the progress model
 * and the front ends are tested with it). */
static unsigned il_sim_delay(const char *name)
{
	const char *v = getenv(name);
	unsigned long n;
	char *end;

	if (!v || !*v)
		return 0;
	n = strtoul(v, &end, 10);
	if (*end || n > 10000000UL)
		return 0;
	return (unsigned)n;
}

static void il_extract_cb(const struct il_extract_progress *p, void *user)
{
	il_ctx *ctx = user;
	struct il_run *r = ctx->run;

	if (!r)
		return;
	if (p->compressed_total && !r->track.comp_total)
		r->track.comp_total = p->compressed_total;
	il_track_extract(&r->track, il_now_ms(), p->bytes_done, p->files_done,
			 p->compressed_done);
	if (p->path && p->path[0])
		snprintf(r->detail, sizeof(r->detail), "Copying /%s",
			 p->path[0] == '/' ? p->path + 1 : p->path);
	il_emit(ctx, 0);
}

/* ---- the steps -------------------------------------------------------- */

static void il_log_config(il_ctx *ctx, const struct il_config *cfg)
{
	char layouts[160] = "";

	for (int i = 0; i < cfg->nlayouts && i < IL_MAX_LAYOUTS; i++) {
		if (i)
			il_strlcat(layouts, ",", sizeof(layouts));
		il_strlcat(layouts, cfg->layouts[i], sizeof(layouts));
		if (cfg->variants[i][0]) {
			il_strlcat(layouts, "(", sizeof(layouts));
			il_strlcat(layouts, cfg->variants[i], sizeof(layouts));
			il_strlcat(layouts, ")", sizeof(layouts));
		}
	}
	il_log(ctx, IL_LOG_INFO, "language %s, formats %s, time zone %s, "
	       "keyboard %s%s%s, host name %s, account %s%s%s",
	       cfg->locale, cfg->time_format, cfg->timezone, layouts,
	       cfg->xkb_options[0] ? " options " : "", cfg->xkb_options,
	       cfg->hostname, cfg->user.username[0] ? cfg->user.username :
	       "(none)", cfg->user.fullname[0] ? " / " : "",
	       cfg->user.fullname);
}

static int il_prepare(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
		      struct il_source_info *src)
{
	char why[IL_ERROR_MAX], summary[4096];
	int rc;

	rc = il_config_validate(cfg, why, sizeof(why));
	if (rc)
		return il_set_error(ctx, -rc, "%s", why);
	il_log_config(ctx, cfg);
	il_step_progress(ctx, 0.2, "Looking at the system image");
	rc = il_source_probe(ctx, src);
	if (rc)
		return rc;
	if (!src->sha256[0])
		il_log(ctx, IL_LOG_WARN, "the system image has no checksum; it is "
		       "unpacked unchecked");
	il_step_progress(ctx, 0.4, "Checking the plan");
	rc = il_plan_validate(plan, src, why, sizeof(why));
	if (rc)
		return il_set_error(ctx, -rc, "%s", why);
	il_plan_summary(plan, summary, sizeof(summary));
	il_log_text(ctx, IL_LOG_INFO, "plan: ", summary, strlen(summary));
	if (!ctx->simulate && geteuid() != 0)
		il_log(ctx, IL_LOG_WARN, "not running as root: writing the disk "
		       "will probably fail");
	if (!ctx->dry_run) {
		il_step_progress(ctx, 0.7, "Unmounting what an earlier attempt "
				 "left");
		rc = il_umount_below(ctx, ctx->target);
		if (rc)
			return rc;
	}
	return 0;
}

static int il_finalize(il_ctx *ctx, const struct il_target *t)
{
	char logdir[IL_PATH_MAX], logdst[IL_PATH_MAX];
	int rc, fd;

	il_step_progress(ctx, 0.1, "Saving the installation log");
	if (il_path_in(logdir, sizeof(logdir), t->root_dir, "/var/log") == 0 &&
	    il_path_in(logdst, sizeof(logdst), t->root_dir, IL_TARGET_LOG) == 0 &&
	    il_mkdir_p(logdir, 0755) == 0) {
		il_log(ctx, IL_LOG_INFO, "LikeOS is installed; the log goes to %s",
		       IL_TARGET_LOG);
		il_log_flush(ctx);
		rc = il_copy_file(ctx->log_path, logdst, 0600);
		if (rc)
			il_log(ctx, IL_LOG_WARN, "the log could not be copied to "
			       "%s: %s", logdst, strerror(-rc));
		else if (chown(logdst, 0, 0) != 0 && errno != EPERM)
			il_log(ctx, IL_LOG_WARN, "%s: owner not set", logdst);
	}
	il_step_progress(ctx, 0.3, "Writing everything to the disk");
	fd = open(t->root_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		il_syncfs(fd);
		close(fd);
	}
	sync();
	il_step_progress(ctx, 0.7, "Unmounting the new system");
	rc = il_umount_all(ctx);
	if (rc)
		return rc;
	il_step_progress(ctx, 1.0, "Done");
	return 0;
}

int il_install(il_ctx *ctx, il_plan *plan, const struct il_config *cfg,
	       il_progress_cb cb, void *user)
{
	struct il_run run;
	struct il_source_info src;
	struct il_target t;
	struct il_extract_stats st;
	struct il_extract_opts xo;
	uint64_t t0, xt0, xt1;
	int rc;

	if (!ctx || !plan || !cfg)
		return -EINVAL;
	if (__atomic_load_n(&ctx->installing, __ATOMIC_SEQ_CST))
		return il_set_error(ctx, EBUSY, "An installation is already "
				    "running");
	il_clear_error(ctx);
	memset(&run, 0, sizeof(run));
	memset(&t, 0, sizeof(t));
	run.cb = cb;
	run.user = user;
	t0 = il_now_ms();
	il_track_init(&run.track, t0);
	ctx->run = &run;
	__atomic_store_n(&ctx->cancel_requested, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&ctx->cancel_allowed, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&ctx->installing, 1, __ATOMIC_SEQ_CST);
	il_log(ctx, IL_LOG_INFO, "installing LikeOS on %s%s", plan->disk.dev,
	       ctx->simulate ? " (simulation)" : "");

	/* 1. prepare */
	il_step_begin(ctx, IL_STEP_PREPARE);
	rc = il_prepare(ctx, plan, cfg, &src);
	if (rc)
		goto fail;
	il_track_totals(&run.track, src.uncompressed_bytes, src.file_count,
			src.compressed_bytes);
	if (ctx->dry_run) {
		il_log(ctx, IL_LOG_INFO, "dry run: the plan and the choices are "
		       "valid; nothing was written");
		/* Every step counts as done: the bar ends full. */
		il_track_finish(&run.track, il_now_ms());
		il_strlcpy(run.detail, "Dry run finished", sizeof(run.detail));
		il_emit(ctx, 1);
		rc = 0;
		goto done;
	}
	if (il_cancelled(ctx)) {
		rc = -ECANCELED;
		goto fail;
	}
	il_step_end(ctx);

	/* 2. partition */
	il_step_begin(ctx, IL_STEP_PARTITION);
	rc = il_partition_apply(ctx, plan, &t);
	/* From here on a cancel is ignored: a half-done disk is worse. */
	__atomic_store_n(&ctx->cancel_allowed, 0, __ATOMIC_SEQ_CST);
	if (rc)
		goto fail;
	il_step_end(ctx);

	/* 3. format */
	il_step_begin(ctx, IL_STEP_FORMAT);
	rc = il_format_esp(ctx, &t);
	if (!rc)
		rc = il_format_root(ctx, &t);
	if (!rc)
		rc = il_format_commit_sim(ctx, &t);
	if (!rc) {
		il_step_progress(ctx, 0.95, "Reading the new file systems");
		rc = il_format_probe(ctx, &t);
	}
	if (rc)
		goto fail;
	il_step_end(ctx);

	/* 4. mount */
	il_step_begin(ctx, IL_STEP_MOUNT);
	rc = il_mount_target(ctx, &t);
	if (rc)
		goto fail;
	il_step_end(ctx);

	/* 5. extract */
	il_step_begin(ctx, IL_STEP_EXTRACT);
	memset(&xo, 0, sizeof(xo));
	memset(&st, 0, sizeof(st));
	xo.archive = ctx->source;
	xo.target = t.root_dir;
	xo.expected_sha256 = src.sha256;
	xo.same_owner = 1;
	/* The real installation runs as root: a failed chown is an error.
	 * A simulation on the build host counts them only. */
	xo.owner_errors_fatal = (!ctx->simulate && geteuid() == 0) ? 1 : 0;
	xo.xattrs = 1;
	xo.acls = 1;
	xo.progress = il_extract_cb;
	xo.user = ctx;
	xo.progress_interval_ms = IL_EMIT_INTERVAL_MS;
	xo.sim_entry_delay_us = ctx->simulate ? il_sim_delay("IL_SIM_FILE_DELAY_US")
					       : 0;
	xo.sim_mib_delay_us = ctx->simulate ? il_sim_delay("IL_SIM_MIB_DELAY_US")
					     : 0;
	xt0 = il_now_ms();
	rc = il_extract_archive(ctx, &xo, &st);
	xt1 = il_now_ms();
	if (rc)
		goto fail;
	il_log(ctx, IL_LOG_INFO, "unpacked %llu entries (%llu bytes) in %llu s; "
	       "fitted cost: %.2f ms per MiB, %.2f ms per file",
	       (unsigned long long)st.entries, (unsigned long long)st.bytes,
	       (unsigned long long)((xt1 - xt0) / 1000),
	       run.track.cost_mib * 1000.0, run.track.cost_file * 1000.0);
	il_track_extract(&run.track, xt1, st.bytes, st.entries,
			 st.compressed_bytes);
	il_step_end(ctx);

	/* 6. configure */
	il_step_begin(ctx, IL_STEP_CONFIGURE);
	rc = il_configure_target(ctx, cfg, &t);
	if (!rc) {
		il_step_progress(ctx, 0.5, "Setting up the accounts");
		rc = il_accounts_apply(ctx, t.root_dir, cfg);
	}
	if (rc)
		goto fail;
	il_step_end(ctx);

	/* 7. boot loader */
	il_step_begin(ctx, IL_STEP_BOOTLOADER);
	rc = il_boot_install(ctx, &t);
	if (rc)
		goto fail;
	il_step_end(ctx);

	/* 8. finalize */
	il_step_begin(ctx, IL_STEP_FINALIZE);
	rc = il_finalize(ctx, &t);
	if (rc)
		goto fail;
	il_track_finish(&run.track, il_now_ms());
	il_step_end(ctx);
	il_log(ctx, IL_LOG_INFO, "installation finished in %llu s",
	       (unsigned long long)((il_now_ms() - t0) / 1000));
	goto done;

fail:
	if (rc == -ECANCELED) {
		il_set_error(ctx, ECANCELED, "The installation was cancelled; "
			     "nothing was written to the disk");
	} else {
		char err[IL_ERROR_MAX];

		il_strlcpy(err, ctx->err[0] ? ctx->err : strerror(-rc),
			   sizeof(err));
		il_log(ctx, IL_LOG_ERROR, "the installation failed in step "
		       "\"%s\": %s", il_step_title(run.track.step), err);
		/* The context's error stays the cause, not the cleanup. */
		il_strlcpy(ctx->err, err, sizeof(ctx->err));
	}
	sync();
	if (ctx->nmounts > 0) {
		char err[IL_ERROR_MAX];

		il_strlcpy(err, ctx->err, sizeof(err));
		il_umount_all(ctx);
		il_strlcpy(ctx->err, err, sizeof(ctx->err));
	}
	il_log_flush(ctx);
done:
	ctx->run = NULL;
	__atomic_store_n(&ctx->installing, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&ctx->cancel_allowed, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&ctx->cancel_requested, 0, __ATOMIC_SEQ_CST);
	return rc;
}

/* ---- the end ------------------------------------------------------------ */

static int il_power(il_ctx *ctx, int cmd, const char *what)
{
	if (!ctx)
		return -EINVAL;
	il_log(ctx, IL_LOG_INFO, "%s", what);
	sync();
	il_log_flush(ctx);
	if (ctx->simulate || ctx->dry_run) {
		il_log(ctx, IL_LOG_INFO, "%s: not done in a simulation or dry "
		       "run", what);
		return 0;
	}
	if (reboot(cmd) != 0)
		return il_set_error(ctx, errno, "%s failed: %s", what,
				    strerror(errno));
	return 0;
}

int il_reboot(il_ctx *ctx)
{
	return il_power(ctx, RB_AUTOBOOT, "restarting the computer");
}

int il_poweroff(il_ctx *ctx)
{
	return il_power(ctx, RB_POWER_OFF, "switching the computer off");
}
