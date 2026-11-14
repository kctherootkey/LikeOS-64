/* LikeOS installer -- the plans of the disks (see app.h).
 *
 * Every change made on the disk page is one PlanStep, kept per disk.  The
 * plan of a disk is the backend's: made from the scan result with
 * il_plan_new(), each step applied with the matching il_plan_* call.  To
 * take a step back, the plan is made afresh from the scan and every step
 * but the last is applied again -- the backend's plan has no undo of its
 * own, and replaying gives exactly the layout the steps made before.
 *
 * The choice of the partition LikeOS goes on is not a step: the disk page
 * makes it when the person presses Next (il_plan_select_root) and drops it
 * again when they come back (il_plan_clear_root), so the steps are what
 * the person did with the tools.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "app.h"
#include "backend-bridge.h"

#include <errno.h>
#include <string.h>

static gboolean eligible(App *app, int di)
{
	const struct il_disk *d;

	if (!app->disks || di < 0 || di >= app->disks->n)
		return FALSE;
	d = &app->disks->disks[di];
	return !d->is_boot_medium && !d->read_only;
}

il_plan *plans_get(App *app, int di)
{
	if (!eligible(app, di))
		return NULL;
	/* the context belongs to the worker while a job runs */
	if (!app->plans[di] && !bridge_busy(app->bridge))
		app->plans[di] = il_plan_new(app->ctx, &app->disks->disks[di]);
	return app->plans[di];
}

/* The index of the free region of the plan's view that starts at start. */
static int free_index_at(il_plan *plan, guint64 start)
{
	struct il_disk *v = g_new0(struct il_disk, 1);
	int idx = -1;

	if (il_plan_view(plan, v) == 0)
		for (int i = 0; i < v->nfree; i++)
			if (v->free[i].start_bytes == start)
				idx = i;
	g_free(v);
	return idx;
}

static int run_step(App *app, il_plan *plan, const PlanStep *st, int *partno_out)
{
	int idx;

	switch (st->kind) {
	case PLAN_STEP_NEW_TABLE:
		return il_plan_new_table(plan);
	case PLAN_STEP_WHOLE:
		return il_plan_use_whole_disk(plan);
	case PLAN_STEP_DELETE:
		return il_plan_delete(plan, st->partno);
	case PLAN_STEP_CREATE:
		idx = free_index_at(plan, st->start);
		if (idx < 0) {
			app_log(app, IL_LOG_WARN, "ui: no free space starts at %"
				G_GUINT64_FORMAT " any more", st->start);
			return -ENOENT;
		}
		return il_plan_create(plan, idx, st->size, partno_out);
	case PLAN_STEP_EXTEND:
		return il_plan_extend(plan, st->partno, st->size);
	case PLAN_STEP_FORMAT:
		return il_plan_format(plan, st->partno);
	}
	return -EINVAL;
}

static GArray *steps_of(App *app, int di)
{
	if (!app->steps[di])
		app->steps[di] = g_array_new(FALSE, FALSE, sizeof(PlanStep));
	return app->steps[di];
}

int plans_apply(App *app, int di, const PlanStep *step, int *partno_out)
{
	il_plan *plan = plans_get(app, di);
	int rc;

	if (!plan)
		return -EINVAL;
	rc = run_step(app, plan, step, partno_out);
	if (rc == 0)
		g_array_append_val(steps_of(app, di), *step);
	return rc;
}

int plans_nsteps(App *app, int di)
{
	if (di < 0 || di >= IL_MAX_DISKS || !app->steps[di])
		return 0;
	return (int)app->steps[di]->len;
}

const PlanStep *plans_step(App *app, int di, int i)
{
	if (i < 0 || i >= plans_nsteps(app, di))
		return NULL;
	return &g_array_index(app->steps[di], PlanStep, i);
}

int plans_truncate(App *app, int di, int n)
{
	GArray *steps;
	il_plan *plan;
	int rc = 0;

	if (!eligible(app, di) || bridge_busy(app->bridge))
		return -EBUSY;
	steps = steps_of(app, di);
	if (n < 0)
		n = 0;
	if ((guint)n > steps->len)
		n = (int)steps->len;
	plan = il_plan_new(app->ctx, &app->disks->disks[di]);
	if (!plan)
		return -ENOMEM;
	for (int i = 0; i < n; i++) {
		rc = run_step(app, plan, &g_array_index(steps, PlanStep, i), NULL);
		if (rc) {
			/* It worked once on the same scan; should it not now,
			 * the plan keeps what still works. */
			app_log(app, IL_LOG_WARN, "ui: step %d of %s cannot be "
				"made again (%s); the later ones are dropped", i + 1,
				app->disks->disks[di].dev, il_last_error(app->ctx));
			n = i;
			break;
		}
	}
	g_array_set_size(steps, (guint)n);
	if (app->plans[di])
		il_plan_free(app->plans[di]);
	app->plans[di] = plan;
	return rc;
}

int plans_undo(App *app, int di, char **what)
{
	int n = plans_nsteps(app, di);

	if (what)
		*what = NULL;
	if (n == 0)
		return -ENOENT;
	if (what)
		*what = plans_step_describe(plans_step(app, di, n - 1));
	return plans_truncate(app, di, n - 1);
}

char *plans_step_describe(const PlanStep *st)
{
	char *size;
	char *t;

	if (!st)
		return g_strdup("");
	switch (st->kind) {
	case PLAN_STEP_NEW_TABLE:
		return g_strdup("the conversion to GPT");
	case PLAN_STEP_WHOLE:
		return g_strdup("the use of the entire disk");
	case PLAN_STEP_DELETE:
		return g_strdup_printf("the deletion of partition %d", st->partno);
	case PLAN_STEP_CREATE:
		if (!st->size)
			return g_strdup("the new partition");
		size = app_size_text(st->size);
		t = g_strdup_printf("the new partition of %s", size);
		g_free(size);
		return t;
	case PLAN_STEP_EXTEND:
		size = app_size_text(st->size);
		t = g_strdup_printf("the extension of partition %d to %s",
				    st->partno, size);
		g_free(size);
		return t;
	case PLAN_STEP_FORMAT:
		return g_strdup_printf("the formatting of partition %d",
				       st->partno);
	}
	return g_strdup("the last change");
}

void plans_free_all(App *app)
{
	for (int i = 0; i < IL_MAX_DISKS; i++) {
		if (app->plans[i]) {
			il_plan_free(app->plans[i]);
			app->plans[i] = NULL;
		}
		if (app->steps[i]) {
			g_array_free(app->steps[i], TRUE);
			app->steps[i] = NULL;
		}
	}
}

void plans_clear_roots(App *app)
{
	if (bridge_busy(app->bridge))
		return;
	for (int i = 0; i < IL_MAX_DISKS; i++)
		if (app->plans[i] && il_plan_root(app->plans[i]) > 0)
			il_plan_clear_root(app->plans[i]);
}
