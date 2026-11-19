/* libinstaller -- the partitioning plan.
 *
 * A plan is a list of operations on one disk, made while a person clicks
 * through the install location page; nothing touches the disk until
 * il_install() applies it.  Every operation stores explicit geometry (byte
 * offsets and sizes worked out when it was added), so replaying the list
 * gives the same layout in the view and on the disk.
 *
 * The projected layout (il_plan_view) is the scanned disk, turned into an
 * in-memory GPT, with the operations replayed on it by libgpt -- the same
 * code that later writes the real table.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "il_internal.h"

/* Operations an il_plan_select_root() added on its own; they go again when
 * another partition is chosen. */
struct il_plan_priv {
	struct il_plan plan;
	unsigned char op_auto[IL_MAX_OPS];
};

static unsigned char *il_plan_auto(il_plan *plan)
{
	return ((struct il_plan_priv *)plan)->op_auto;
}

static void il_why(char *why, size_t len, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void il_why(char *why, size_t len, const char *fmt, ...)
{
	va_list ap;

	if (!why || !len)
		return;
	va_start(ap, fmt);
	vsnprintf(why, len, fmt, ap);
	va_end(ap);
}

static uint64_t il_ss(const struct il_disk *d)
{
	return d->sector_size ? d->sector_size : 512;
}

/* ------------------------------------------------------------------ */
/* Plan objects                                                        */
/* ------------------------------------------------------------------ */

il_plan *il_plan_new(il_ctx *ctx, const struct il_disk *disk)
{
	struct il_plan_priv *p;

	if (!disk)
		return NULL;
	p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->plan.ctx = ctx;
	p->plan.disk = *disk;
	p->plan.root_partno = -1;
	if (ctx)
		il_log(ctx, IL_LOG_DEBUG, "new plan for %s", disk->dev);
	return &p->plan;
}

void il_plan_free(il_plan *plan)
{
	free(plan);
}

const struct il_disk *il_plan_disk(const il_plan *plan)
{
	return plan ? &plan->disk : NULL;
}

int il_plan_nops(const il_plan *plan)
{
	return plan ? plan->nops : 0;
}

const struct il_op *il_plan_op(const il_plan *plan, int i)
{
	if (!plan || i < 0 || i >= plan->nops)
		return NULL;
	return &plan->ops[i];
}

int il_plan_root(const il_plan *plan)
{
	return plan ? plan->root_partno : -1;
}

static int il_plan_err(il_plan *plan, int err, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static int il_plan_err(il_plan *plan, int err, const char *fmt, ...)
{
	char msg[IL_ERROR_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (err < 0)
		err = -err;
	if (err == 0)
		err = EIO;
	if (plan->quiet) {
		il_strlcpy(plan->qerr, msg, sizeof(plan->qerr));
		return -err;
	}
	if (plan->ctx)
		return il_set_error(plan->ctx, err, "%s", msg);
	return -err;
}

static int il_plan_push(il_plan *plan, const struct il_op *op, int is_auto)
{
	if (plan->nops >= IL_MAX_OPS)
		return il_plan_err(plan, ENOSPC, "Too many changes are planned "
				   "for this disk");
	plan->ops[plan->nops] = *op;
	il_plan_auto(plan)[plan->nops] = (unsigned char)(is_auto ? 1 : 0);
	plan->nops++;
	return 0;
}

static void il_plan_remove_op(il_plan *plan, int i)
{
	unsigned char *a = il_plan_auto(plan);

	if (i < 0 || i >= plan->nops)
		return;
	memmove(&plan->ops[i], &plan->ops[i + 1],
		(size_t)(plan->nops - i - 1) * sizeof(plan->ops[0]));
	memmove(&a[i], &a[i + 1], (size_t)(plan->nops - i - 1));
	plan->nops--;
}

int il_plan_erases_disk(const il_plan *plan)
{
	for (int i = 0; plan && i < plan->nops; i++)
		if (plan->ops[i].kind == IL_OP_NEW_TABLE)
			return 1;
	return 0;
}

int il_plan_creates(const il_plan *plan, int partno)
{
	for (int i = 0; plan && i < plan->nops; i++)
		if (plan->ops[i].kind == IL_OP_CREATE &&
		    plan->ops[i].partno == partno)
			return 1;
	return 0;
}

const struct il_op *il_plan_format_op(const il_plan *plan, int partno)
{
	const struct il_op *found = NULL;

	for (int i = 0; plan && i < plan->nops; i++) {
		const struct il_op *op = &plan->ops[i];

		if (op->partno != partno)
			continue;
		if (op->kind == IL_OP_FORMAT)
			found = op;
		else if (op->kind == IL_OP_DELETE)
			found = NULL;
	}
	return found;
}

/* The last CREATE of partno (the one that is still in effect), or -1. */
static int il_plan_create_index(const il_plan *plan, int partno)
{
	int idx = -1;

	for (int i = 0; i < plan->nops; i++) {
		const struct il_op *op = &plan->ops[i];

		if (op->kind == IL_OP_NEW_TABLE)
			idx = -1;
		else if (op->kind == IL_OP_CREATE && op->partno == partno)
			idx = i;
		else if (op->kind == IL_OP_DELETE && op->partno == partno)
			idx = -1;
	}
	return idx;
}

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

int il_plan_base_table(const struct il_disk *d, struct gpt_table *t)
{
	uint64_t ss = il_ss(d);
	uint64_t nsec = d->size_bytes / ss;
	uint64_t lo = UINT64_MAX, hi = 0;
	int rc;

	rc = gpt_table_new(t, (uint32_t)ss, nsec);
	if (rc)
		return rc;
	if (d->table != IL_TABLE_GPT)
		return 0;
	if (d->disk_guid[0])
		gpt_guid_parse(d->disk_guid, &t->disk_guid);
	/* A table written by another tool may use a wider usable range
	 * (fewer entry sectors); widen ours so no partition falls out. */
	for (int i = 0; i < d->nparts; i++) {
		uint64_t f = d->parts[i].start_bytes / ss;
		uint64_t l = f + d->parts[i].size_bytes / ss - 1;

		if (f < lo)
			lo = f;
		if (l > hi)
			hi = l;
	}
	if (lo < t->first_usable && lo >= 2)
		t->first_usable = lo;
	if (hi > t->last_usable && hi < nsec - 1)
		t->last_usable = hi;
	for (int i = 0; i < d->nparts; i++) {
		const struct il_partition *p = &d->parts[i];
		struct gpt_part_spec spec;
		int got = 0;

		memset(&spec, 0, sizeof(spec));
		spec.partno = p->number;
		spec.first_lba = p->start_bytes / ss;
		spec.size_lba = p->size_bytes / ss;
		if (gpt_guid_parse(p->type_guid, &spec.type) != 0)
			gpt_guid_parse(GPT_TYPE_GENERIC_DATA, &spec.type);
		if (p->part_guid[0])
			gpt_guid_parse(p->part_guid, &spec.uuid);
		spec.name = p->name;
		rc = gpt_part_add(t, &spec, &got);
		if (rc) {
			gpt_table_free(t);
			return rc;
		}
	}
	return 0;
}

int il_plan_replay(const il_plan *plan, struct gpt_table *t, int real_guids,
		   char *why, size_t len)
{
	uint64_t ss = il_ss(&plan->disk);
	int rc = 0;

	for (int i = 0; i < plan->nops; i++) {
		const struct il_op *op = &plan->ops[i];
		struct gpt_part_spec spec;
		struct gpt_guid type;
		int got = 0;

		switch (op->kind) {
		case IL_OP_NEW_TABLE:
			gpt_table_free(t);
			rc = gpt_table_new(t, (uint32_t)ss,
					   plan->disk.size_bytes / ss);
			if (rc) {
				il_why(why, len, "A new partition table does not "
				       "fit on this disk");
				return rc;
			}
			break;
		case IL_OP_CREATE:
			memset(&spec, 0, sizeof(spec));
			spec.partno = op->partno;
			spec.first_lba = op->start_bytes / ss;
			spec.size_lba = op->size_bytes / ss;
			if (gpt_guid_parse(op->type_guid, &spec.type) != 0)
				gpt_guid_parse(GPT_TYPE_GENERIC_DATA, &spec.type);
			/* Zero = a random unique GUID (libgpt). */
			spec.name = op->name[0] ? op->name : NULL;
			rc = gpt_part_add(t, &spec, &got);
			if (rc) {
				il_why(why, len, "Partition %d cannot be "
				       "created: %s", op->partno, t->errmsg);
				return rc;
			}
			break;
		case IL_OP_DELETE:
			rc = gpt_part_delete(t, op->partno);
			if (rc) {
				il_why(why, len, "Partition %d cannot be "
				       "deleted: %s", op->partno, t->errmsg);
				return rc;
			}
			break;
		case IL_OP_EXTEND:
			rc = gpt_part_resize(t, op->partno, op->size_bytes / ss);
			if (rc) {
				il_why(why, len, "Partition %d cannot be "
				       "extended: %s", op->partno, t->errmsg);
				return rc;
			}
			break;
		case IL_OP_SET_TYPE:
			if (gpt_guid_parse(op->type_guid, &type) != 0) {
				il_why(why, len, "Bad partition type %s",
				       op->type_guid);
				return -EINVAL;
			}
			rc = gpt_part_set_type(t, op->partno, &type);
			if (!rc && op->name[0])
				rc = gpt_part_set_name(t, op->partno, op->name);
			if (rc) {
				il_why(why, len, "Partition %d cannot be "
				       "changed: %s", op->partno, t->errmsg);
				return rc;
			}
			break;
		case IL_OP_FORMAT:
			if (!gpt_part_get(t, op->partno)) {
				il_why(why, len, "Partition %d to be formatted "
				       "does not exist", op->partno);
				return -ENOENT;
			}
			break;
		default:
			il_why(why, len, "Unknown operation %d", op->kind);
			return -EINVAL;
		}
	}
	(void)real_guids;
	return 0;
}

/* The plan's projected table; -errno with a reason in plan->ctx. */
static int il_plan_table(const il_plan *plan, struct gpt_table *t, char *why,
			 size_t len)
{
	int rc;

	memset(t, 0, sizeof(*t));
	rc = il_plan_base_table(&plan->disk, t);
	if (rc) {
		il_why(why, len, "The partition table of %s cannot be "
		       "represented: %s", plan->disk.dev, strerror(-rc));
		return rc;
	}
	rc = il_plan_replay(plan, t, 0, why, len);
	if (rc)
		gpt_table_free(t);
	return rc;
}

/* 1 when the disk can only be changed after a new table. */
static int il_plan_needs_table(const il_plan *plan)
{
	return plan->disk.table != IL_TABLE_GPT && !il_plan_erases_disk(plan);
}

static int il_plan_check_disk(il_plan *plan)
{
	if (plan->disk.is_boot_medium)
		return il_plan_err(plan, EBUSY, "%s holds the running LikeOS "
				   "(the installation medium) and cannot be "
				   "changed", plan->disk.dev);
	if (plan->disk.read_only)
		return il_plan_err(plan, EROFS, "%s is read-only",
				   plan->disk.dev);
	return 0;
}

/* After an edit: the plan must still replay; else it is rolled back to
 * `keep' operations. */
static int il_plan_settle(il_plan *plan, int keep)
{
	struct gpt_table t;
	char why[256];
	int rc = il_plan_table(plan, &t, why, sizeof(why));

	if (rc) {
		plan->nops = keep;
		return il_plan_err(plan, -rc, "%s", why);
	}
	gpt_table_free(&t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Editing                                                             */
/* ------------------------------------------------------------------ */

int il_plan_new_table(il_plan *plan)
{
	struct il_op op;
	int rc;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	/* Everything planned before is moot on an erased disk. */
	plan->nops = 0;
	plan->root_partno = -1;
	memset(&op, 0, sizeof(op));
	op.kind = IL_OP_NEW_TABLE;
	op.size_bytes = plan->disk.size_bytes;
	rc = il_plan_push(plan, &op, 0);
	if (rc)
		return rc;
	return il_plan_settle(plan, 0);
}

/* Adds a CREATE for [first_lba, +size_lba) (size 0 = rest of the region
 * that holds first_lba) with libgpt's own placement, so the stored
 * geometry is exactly what libgpt makes of it. */
static int il_plan_add_create(il_plan *plan, struct gpt_table *t, int partno,
			      uint64_t first_lba, uint64_t size_lba,
			      const char *type, const char *name, int is_auto,
			      int *partno_out)
{
	struct gpt_part_spec spec;
	const struct gpt_part *gp;
	struct il_op op;
	uint64_t ss = il_ss(&plan->disk);
	int got = 0, rc;

	memset(&spec, 0, sizeof(spec));
	spec.partno = partno;
	spec.first_lba = first_lba;
	spec.size_lba = size_lba;
	gpt_guid_parse(type, &spec.type);
	spec.name = name;
	rc = gpt_part_add(t, &spec, &got);
	if (rc)
		return il_plan_err(plan, -rc, "The partition cannot be created: "
				   "%s", t->errmsg);
	gp = gpt_part_get(t, got);
	if (!gp)
		return il_plan_err(plan, EIO, "internal: the new partition %d "
				   "is missing", got);
	memset(&op, 0, sizeof(op));
	op.kind = IL_OP_CREATE;
	op.partno = got;
	op.start_bytes = gp->first_lba * ss;
	op.size_bytes = (gp->last_lba - gp->first_lba + 1) * ss;
	il_strlcpy(op.type_guid, type, sizeof(op.type_guid));
	if (name)
		il_strlcpy(op.name, name, sizeof(op.name));
	rc = il_plan_push(plan, &op, is_auto);
	if (rc)
		return rc;
	if (partno_out)
		*partno_out = got;
	return 0;
}

static int il_plan_add_format(il_plan *plan, int partno, const char *fstype,
			      int is_auto)
{
	struct il_op op;
	const struct il_op *have = il_plan_format_op(plan, partno);

	if (have && strcmp(have->fstype, fstype) == 0)
		return 0;
	if (have) {
		/* A format to another type: replace it. */
		for (int i = plan->nops - 1; i >= 0; i--)
			if (plan->ops[i].kind == IL_OP_FORMAT &&
			    plan->ops[i].partno == partno) {
				il_plan_remove_op(plan, i);
				break;
			}
	}
	memset(&op, 0, sizeof(op));
	op.kind = IL_OP_FORMAT;
	op.partno = partno;
	il_strlcpy(op.fstype, fstype, sizeof(op.fstype));
	return il_plan_push(plan, &op, is_auto);
}

int il_plan_use_whole_disk(il_plan *plan)
{
	struct gpt_table t;
	uint64_t ss, align, esp_lba;
	char why[256];
	int rc, esp = 0, root = 0;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	ss = il_ss(&plan->disk);
	if (plan->disk.size_bytes < IL_ALIGN_BYTES * 2 + IL_ESP_SIZE_BYTES +
				    IL_ALIGN_BYTES * 16)
		return il_plan_err(plan, ENOSPC, "%s is too small for LikeOS",
				   plan->disk.dev);
	rc = il_plan_new_table(plan);
	if (rc)
		return rc;
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc) {
		plan->nops = 0;
		return il_plan_err(plan, -rc, "%s", why);
	}
	align = gpt_align_sectors(&t);
	esp_lba = IL_ESP_SIZE_BYTES / ss;
	rc = il_plan_add_create(plan, &t, 1, align, esp_lba, IL_GUID_ESP,
				IL_ESP_PARTNAME, 0, &esp);
	if (!rc)
		rc = il_plan_add_create(plan, &t, 2,
					gpt_align_up(align + esp_lba, align), 0,
					IL_GUID_LIKEOS_ROOT, IL_ROOT_PARTNAME, 0,
					&root);
	gpt_table_free(&t);
	if (!rc)
		rc = il_plan_add_format(plan, esp, "vfat", 0);
	if (!rc)
		rc = il_plan_add_format(plan, root, "ext4", 0);
	if (rc) {
		plan->nops = 0;
		plan->root_partno = -1;
		return rc;
	}
	plan->root_partno = root;
	if (plan->ctx && !plan->quiet) {
		char size[32];

		il_format_size(plan->disk.size_bytes, size, sizeof(size));
		il_log(plan->ctx, IL_LOG_INFO, "plan: whole disk %s (%s): ESP "
		       "partition %d, root partition %d", plan->disk.dev, size,
		       esp, root);
	}
	return il_plan_settle(plan, 0);
}

/* The ESP of a table: one being formatted by the plan first, else the
 * first partition of the ESP type. */
static int il_plan_esp_in(const il_plan *plan, const struct gpt_table *t)
{
	struct gpt_guid esp;
	int first = -1;

	gpt_guid_parse(IL_GUID_ESP, &esp);
	for (uint32_t i = 0; i < t->num_entries; i++) {
		const struct gpt_part *p = &t->parts[i];

		if (!p->used || !gpt_guid_equal(&p->type, &esp))
			continue;
		if (il_plan_format_op(plan, (int)i + 1) ||
		    il_plan_creates(plan, (int)i + 1))
			return (int)i + 1;
		if (first < 0)
			first = (int)i + 1;
	}
	return first;
}

int il_plan_esp(const il_plan *plan)
{
	struct gpt_table t;
	int esp;

	if (!plan)
		return -1;
	if (plan->disk.table == IL_TABLE_MBR && !il_plan_erases_disk(plan)) {
		for (int i = 0; i < plan->disk.nparts; i++)
			if (plan->disk.parts[i].is_esp)
				return plan->disk.parts[i].number;
		return -1;
	}
	if (il_plan_table(plan, &t, NULL, 0) != 0)
		return -1;
	esp = il_plan_esp_in(plan, &t);
	gpt_table_free(&t);
	return esp;
}

int il_plan_create(il_plan *plan, int free_index, uint64_t size_bytes,
		   int *partno_out)
{
	struct gpt_table t;
	struct gpt_free fr[IL_MAX_FREE];
	uint64_t ss, align, first, last, avail, want = 0;
	int keep, rc, nf, esp = -1, got = 0;
	char why[256];

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	if (plan->disk.table == IL_TABLE_MBR && !il_plan_erases_disk(plan))
		return il_plan_err(plan, EMEDIUMTYPE, "%s uses the older MBR "
				   "partitioning; LikeOS needs a GPT disk.  "
				   "Erase the disk (new partition table) first",
				   plan->disk.dev);
	keep = plan->nops;
	if (plan->disk.table == IL_TABLE_NONE && !il_plan_erases_disk(plan)) {
		/* A blank disk: the table it needs costs nothing. */
		struct il_op op;

		memset(&op, 0, sizeof(op));
		op.kind = IL_OP_NEW_TABLE;
		op.size_bytes = plan->disk.size_bytes;
		rc = il_plan_push(plan, &op, 0);
		if (rc)
			return rc;
	}
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc) {
		plan->nops = keep;
		return il_plan_err(plan, -rc, "%s", why);
	}
	ss = il_ss(&plan->disk);
	align = gpt_align_sectors(&t);
	nf = gpt_free_regions(&t, fr, IL_MAX_FREE, 1);
	if (free_index < 0 || free_index >= nf) {
		gpt_table_free(&t);
		plan->nops = keep;
		return il_plan_err(plan, EINVAL, "There is no free space number "
				   "%d on this disk", free_index);
	}
	first = fr[free_index].first_lba;
	last = fr[free_index].last_lba;
	if (il_plan_esp_in(plan, &t) < 0) {
		uint64_t esp_lba = IL_ESP_SIZE_BYTES / ss;

		if (last - first + 1 < esp_lba + align) {
			gpt_table_free(&t);
			plan->nops = keep;
			return il_plan_err(plan, ENOSPC, "This space is too small "
					   "for the EFI system partition LikeOS "
					   "needs in front of it");
		}
		rc = il_plan_add_create(plan, &t, 0, first, esp_lba, IL_GUID_ESP,
					IL_ESP_PARTNAME, 0, &esp);
		if (!rc)
			rc = il_plan_add_format(plan, esp, "vfat", 0);
		if (rc) {
			gpt_table_free(&t);
			plan->nops = keep;
			return rc;
		}
		first = gpt_align_up(first + esp_lba, align);
	}
	avail = (last >= first) ? (last - first + 1) * ss : 0;
	if (size_bytes) {
		want = size_bytes / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
		if (want == 0)
			want = IL_ALIGN_BYTES;
		/* "All of it" typed as a number is taken as all of it. */
		if (want > avail && want - avail < IL_ALIGN_BYTES)
			want = 0;
		else if (want > avail) {
			char a[32], w[32];

			gpt_table_free(&t);
			plan->nops = keep;
			il_format_size(avail, a, sizeof(a));
			il_format_size(size_bytes, w, sizeof(w));
			return il_plan_err(plan, ENOSPC, "%s does not fit: only "
					   "%s are free here", w, a);
		}
	}
	if (avail < IL_ALIGN_BYTES) {
		gpt_table_free(&t);
		plan->nops = keep;
		return il_plan_err(plan, ENOSPC, "No room is left in this space");
	}
	rc = il_plan_add_create(plan, &t, 0, first, want / ss,
				IL_GUID_GENERIC_DATA, NULL, 0, &got);
	gpt_table_free(&t);
	if (rc) {
		plan->nops = keep;
		return rc;
	}
	rc = il_plan_settle(plan, keep);
	if (rc)
		return rc;
	if (partno_out)
		*partno_out = got;
	if (plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: create partition %d%s", got,
		       esp > 0 ? " (with an EFI system partition before it)"
			       : "");
	return 0;
}

int il_plan_delete(il_plan *plan, int partno)
{
	struct gpt_table t;
	const struct gpt_part *gp;
	struct il_op op;
	char why[256];
	int keep, rc, ci;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	if (il_plan_needs_table(plan))
		return il_plan_err(plan, EMEDIUMTYPE, "Partitions of a disk "
				   "without a GPT cannot be deleted one by one; "
				   "erase the disk instead");
	keep = plan->nops;
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc)
		return il_plan_err(plan, -rc, "%s", why);
	gp = gpt_part_get(&t, partno);
	if (!gp) {
		gpt_table_free(&t);
		return il_plan_err(plan, ENOENT, "There is no partition %d",
				   partno);
	}
	ci = il_plan_create_index(plan, partno);
	if (ci >= 0) {
		/* Created by this plan: forget it, and what was planned for
		 * it afterwards. */
		for (int i = plan->nops - 1; i > ci; i--)
			if (plan->ops[i].partno == partno &&
			    (plan->ops[i].kind == IL_OP_FORMAT ||
			     plan->ops[i].kind == IL_OP_EXTEND ||
			     plan->ops[i].kind == IL_OP_SET_TYPE))
				il_plan_remove_op(plan, i);
		il_plan_remove_op(plan, ci);
	} else {
		/* Plans for the partition as it is are moot. */
		for (int i = plan->nops - 1; i >= 0; i--)
			if (plan->ops[i].partno == partno &&
			    (plan->ops[i].kind == IL_OP_FORMAT ||
			     plan->ops[i].kind == IL_OP_EXTEND ||
			     plan->ops[i].kind == IL_OP_SET_TYPE))
				il_plan_remove_op(plan, i);
		memset(&op, 0, sizeof(op));
		op.kind = IL_OP_DELETE;
		op.partno = partno;
		op.start_bytes = gp->first_lba * il_ss(&plan->disk);
		op.size_bytes = (gp->last_lba - gp->first_lba + 1) *
				il_ss(&plan->disk);
		{
			char g[GPT_GUID_STR_LEN];

			gpt_guid_format(&gp->type, g);
			il_strlcpy(op.type_guid, g, sizeof(op.type_guid));
		}
		il_strlcpy(op.name, gp->name, sizeof(op.name));
		rc = il_plan_push(plan, &op, 0);
		if (rc) {
			gpt_table_free(&t);
			return rc;
		}
	}
	gpt_table_free(&t);
	if (plan->root_partno == partno)
		plan->root_partno = -1;
	rc = il_plan_settle(plan, keep);
	if (!rc && plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: delete partition %d",
		       partno);
	return rc;
}

int il_plan_extend(il_plan *plan, int partno, uint64_t new_size_bytes)
{
	struct gpt_table t;
	const struct gpt_part *gp;
	uint64_t ss, cur, max, want;
	char why[256];
	int keep, rc, ci;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	if (il_plan_needs_table(plan))
		return il_plan_err(plan, EMEDIUMTYPE, "Only partitions of a GPT "
				   "disk can be extended");
	keep = plan->nops;
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc)
		return il_plan_err(plan, -rc, "%s", why);
	gp = gpt_part_get(&t, partno);
	if (!gp) {
		gpt_table_free(&t);
		return il_plan_err(plan, ENOENT, "There is no partition %d",
				   partno);
	}
	ss = il_ss(&plan->disk);
	cur = (gp->last_lba - gp->first_lba + 1) * ss;
	max = gpt_part_max_size(&t, partno) * ss;
	gpt_table_free(&t);
	if (new_size_bytes == 0 || new_size_bytes >= max ||
	    max - new_size_bytes < IL_ALIGN_BYTES)
		want = max;
	else
		want = new_size_bytes / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
	gp = NULL;
	ci = il_plan_create_index(plan, partno);
	if (ci >= 0) {
		/* A partition of this plan can take any size. */
		uint64_t old = plan->ops[ci].size_bytes;

		if (want < IL_ALIGN_BYTES)
			return il_plan_err(plan, EINVAL, "A partition needs at "
					   "least 1 MB");
		plan->ops[ci].size_bytes = want;
		rc = il_plan_settle(plan, plan->nops);
		if (rc) {
			plan->ops[ci].size_bytes = old;
			return rc;
		}
	} else {
		struct il_op *saved;
		unsigned char saved_auto[IL_MAX_OPS];
		struct il_op op;

		if (want <= cur)
			return il_plan_err(plan, EINVAL, "Partition %d can only "
					   "grow (it has %llu bytes; the free "
					   "space after it allows %llu)", partno,
					   (unsigned long long)cur,
					   (unsigned long long)max);
		saved = malloc(sizeof(plan->ops));
		if (!saved)
			return il_plan_err(plan, ENOMEM, "out of memory");
		memcpy(saved, plan->ops, sizeof(plan->ops));
		memcpy(saved_auto, il_plan_auto(plan), sizeof(saved_auto));
		/* One EXTEND per partition: a second one replaces it. */
		for (int i = plan->nops - 1; i >= 0; i--)
			if (plan->ops[i].kind == IL_OP_EXTEND &&
			    plan->ops[i].partno == partno)
				il_plan_remove_op(plan, i);
		memset(&op, 0, sizeof(op));
		op.kind = IL_OP_EXTEND;
		op.partno = partno;
		op.size_bytes = want;
		rc = il_plan_push(plan, &op, 0);
		if (!rc)
			rc = il_plan_settle(plan, plan->nops);
		if (rc) {
			memcpy(plan->ops, saved, sizeof(plan->ops));
			memcpy(il_plan_auto(plan), saved_auto,
			       sizeof(saved_auto));
			plan->nops = keep;
			free(saved);
			return rc;
		}
		free(saved);
	}
	if (plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: partition %d to %llu "
		       "bytes", partno, (unsigned long long)want);
	return 0;
}

int il_plan_format(il_plan *plan, int partno)
{
	struct gpt_table t;
	const struct gpt_part *gp;
	struct gpt_guid esp;
	char why[256];
	int keep, rc, is_esp;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	if (il_plan_needs_table(plan))
		return il_plan_err(plan, EMEDIUMTYPE, "%s has no GPT; LikeOS can "
				   "only be installed on a GPT disk",
				   plan->disk.dev);
	keep = plan->nops;
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc)
		return il_plan_err(plan, -rc, "%s", why);
	gp = gpt_part_get(&t, partno);
	gpt_guid_parse(IL_GUID_ESP, &esp);
	is_esp = gp && gpt_guid_equal(&gp->type, &esp);
	gpt_table_free(&t);
	if (!gp)
		return il_plan_err(plan, ENOENT, "There is no partition %d",
				   partno);
	rc = il_plan_add_format(plan, partno, is_esp ? "vfat" : "ext4", 0);
	if (rc)
		return rc;
	rc = il_plan_settle(plan, keep);
	if (!rc && plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: format partition %d (%s)",
		       partno, is_esp ? "FAT32" : "ext4");
	return rc;
}

int il_plan_select_root(il_plan *plan, int partno)
{
	struct il_op *saved;
	unsigned char saved_auto[IL_MAX_OPS];
	struct gpt_table t;
	const struct gpt_part *gp;
	struct gpt_guid esp, root;
	unsigned char *a;
	char why[256];
	int saved_n, rc, ci, already_root, is_esp, prev;

	if (!plan)
		return -EINVAL;
	rc = il_plan_check_disk(plan);
	if (rc)
		return rc;
	if (il_plan_needs_table(plan))
		return il_plan_err(plan, EMEDIUMTYPE, "%s has no GPT; erase the "
				   "disk or choose another one", plan->disk.dev);
	saved = malloc(sizeof(plan->ops));
	if (!saved)
		return il_plan_err(plan, ENOMEM, "out of memory");
	a = il_plan_auto(plan);
	memcpy(saved, plan->ops, sizeof(plan->ops));
	memcpy(saved_auto, a, sizeof(saved_auto));
	saved_n = plan->nops;
	prev = plan->root_partno;

	/* What an earlier choice added goes first; a partition the plan
	 * created for it is a plain data partition again. */
	for (int i = plan->nops - 1; i >= 0; i--)
		if (a[i])
			il_plan_remove_op(plan, i);
	if (prev > 0 && prev != partno) {
		int pi = il_plan_create_index(plan, prev);

		if (pi >= 0) {
			il_strlcpy(plan->ops[pi].type_guid, IL_GUID_GENERIC_DATA,
				   sizeof(plan->ops[pi].type_guid));
			plan->ops[pi].name[0] = '\0';
		}
	}

	/* The partition as the plan leaves it without that choice. */
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc) {
		rc = il_plan_err(plan, -rc, "%s", why);
		goto undo;
	}
	gp = gpt_part_get(&t, partno);
	gpt_guid_parse(IL_GUID_ESP, &esp);
	gpt_guid_parse(IL_GUID_LIKEOS_ROOT, &root);
	if (!gp) {
		gpt_table_free(&t);
		rc = il_plan_err(plan, ENOENT, "There is no partition %d", partno);
		goto undo;
	}
	is_esp = gpt_guid_equal(&gp->type, &esp);
	already_root = gpt_guid_equal(&gp->type, &root) &&
		       strcmp(gp->name, IL_ROOT_PARTNAME) == 0;
	gp = NULL;
	gpt_table_free(&t);
	if (is_esp) {
		rc = il_plan_err(plan, EINVAL, "Partition %d is the EFI system "
				 "partition; LikeOS needs a partition of its own",
				 partno);
		goto undo;
	}

	ci = il_plan_create_index(plan, partno);
	if (ci >= 0) {
		/* Created by the plan: give it the LikeOS type and name right
		 * away instead of a separate change. */
		il_strlcpy(plan->ops[ci].type_guid, IL_GUID_LIKEOS_ROOT,
			   sizeof(plan->ops[ci].type_guid));
		il_strlcpy(plan->ops[ci].name, IL_ROOT_PARTNAME,
			   sizeof(plan->ops[ci].name));
	} else if (!already_root) {
		struct il_op op;

		memset(&op, 0, sizeof(op));
		op.kind = IL_OP_SET_TYPE;
		op.partno = partno;
		il_strlcpy(op.type_guid, IL_GUID_LIKEOS_ROOT, sizeof(op.type_guid));
		il_strlcpy(op.name, IL_ROOT_PARTNAME, sizeof(op.name));
		rc = il_plan_push(plan, &op, 1);
		if (rc)
			goto undo;
	}
	{
		const struct il_op *f = il_plan_format_op(plan, partno);

		if (!f || strcmp(f->fstype, "ext4") != 0) {
			rc = il_plan_add_format(plan, partno, "ext4", 1);
			if (rc)
				goto undo;
		}
	}
	rc = il_plan_settle(plan, plan->nops);
	if (rc)
		goto undo;
	free(saved);
	plan->root_partno = partno;
	if (plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: LikeOS goes on partition "
		       "%d", partno);
	return 0;

undo:
	memcpy(plan->ops, saved, sizeof(plan->ops));
	memcpy(a, saved_auto, sizeof(saved_auto));
	plan->nops = saved_n;
	plan->root_partno = prev;
	free(saved);
	return rc;
}

int il_plan_clear_root(il_plan *plan)
{
	unsigned char *a;
	int prev;

	if (!plan)
		return -EINVAL;
	prev = plan->root_partno;
	a = il_plan_auto(plan);
	/* What the choice added goes, as when another partition is chosen;
	 * a partition the plan created for it is a plain data partition
	 * again. */
	for (int i = plan->nops - 1; i >= 0; i--)
		if (a[i])
			il_plan_remove_op(plan, i);
	if (prev > 0) {
		int pi = il_plan_create_index(plan, prev);

		if (pi >= 0) {
			il_strlcpy(plan->ops[pi].type_guid, IL_GUID_GENERIC_DATA,
				   sizeof(plan->ops[pi].type_guid));
			plan->ops[pi].name[0] = '\0';
		}
	}
	plan->root_partno = -1;
	if (prev > 0 && plan->ctx && !plan->quiet)
		il_log(plan->ctx, IL_LOG_INFO, "plan: partition %d is no longer "
		       "chosen for LikeOS", prev);
	return 0;
}

int il_plan_can_select_root(const il_plan *plan, int partno,
			    const struct il_source_info *src, char *why,
			    size_t len)
{
	struct il_plan_priv *copy;
	int rc;

	if (why && len)
		why[0] = '\0';
	if (!plan) {
		il_why(why, len, "No disk has been chosen");
		return -EINVAL;
	}
	/* The question is answered on a copy: the choice is made there and
	 * the result validated, quietly, and the copy thrown away. */
	copy = malloc(sizeof(*copy));
	if (!copy) {
		il_why(why, len, "Out of memory");
		return -ENOMEM;
	}
	memcpy(copy, (const struct il_plan_priv *)plan, sizeof(*copy));
	copy->plan.quiet = 1;
	copy->plan.qerr[0] = '\0';
	rc = il_plan_select_root(&copy->plan, partno);
	if (rc)
		il_why(why, len, "%s", copy->plan.qerr[0] ? copy->plan.qerr :
		       strerror(-rc));
	else
		rc = il_plan_validate(&copy->plan, src, why, len);
	free(copy);
	return rc;
}

/* ------------------------------------------------------------------ */
/* The projected layout                                                */
/* ------------------------------------------------------------------ */

int il_plan_view(const il_plan *plan, struct il_disk *out)
{
	struct gpt_table t;
	struct gpt_free fr[IL_MAX_FREE];
	const struct il_disk *d;
	char why[256];
	uint64_t ss;
	int rc, nf, erased;

	if (!plan || !out)
		return -EINVAL;
	d = &plan->disk;
	if (plan->nops == 0 || (d->table == IL_TABLE_MBR &&
				!il_plan_erases_disk(plan))) {
		*out = *d;
		return 0;
	}
	rc = il_plan_table(plan, &t, why, sizeof(why));
	if (rc) {
		if (plan->ctx && !plan->quiet)
			il_set_error(plan->ctx, -rc, "%s", why);
		*out = *d;
		return rc;
	}
	erased = il_plan_erases_disk(plan);
	ss = il_ss(d);
	*out = *d;
	out->table = IL_TABLE_GPT;
	if (erased)
		out->disk_guid[0] = '\0';
	out->nparts = 0;
	for (uint32_t i = 0; i < t.num_entries && out->nparts < IL_MAX_PARTS;
	     i++) {
		const struct gpt_part *g = &t.parts[i];
		const struct il_partition *old = NULL;
		const struct il_op *fmt;
		struct il_partition *p;
		char guid[GPT_GUID_STR_LEN];
		int num = (int)i + 1;
		int is_new = erased || il_plan_creates(plan, num);

		if (!g->used)
			continue;
		p = &out->parts[out->nparts++];
		memset(p, 0, sizeof(*p));
		p->number = num;
		il_part_devname(d->dev, num, p->dev, sizeof(p->dev));
		p->start_bytes = g->first_lba * ss;
		p->size_bytes = (g->last_lba - g->first_lba + 1) * ss;
		gpt_guid_format(&g->type, guid);
		il_strlcpy(p->type_guid, guid, sizeof(p->type_guid));
		if (!is_new) {
			gpt_guid_format(&g->uuid, guid);
			il_strlcpy(p->part_guid, guid, sizeof(p->part_guid));
		}
		il_strlcpy(p->name, g->name, sizeof(p->name));
		il_strlcpy(p->type_desc, gpt_type_name(&g->type),
			   sizeof(p->type_desc));
		p->is_esp = strcmp(p->type_guid, IL_GUID_ESP) == 0;
		p->is_likeos_root = strcmp(p->type_guid, IL_GUID_LIKEOS_ROOT) == 0;
		if (!is_new)
			old = il_disk_part(d, num);
		fmt = il_plan_format_op(plan, num);
		if (fmt) {
			il_strlcpy(p->fstype, fmt->fstype, sizeof(p->fstype));
			il_strlcpy(p->fs_label,
				   strcmp(fmt->fstype, "vfat") == 0 ?
				   IL_ESP_LABEL : IL_ROOT_LABEL,
				   sizeof(p->fs_label));
		} else if (old) {
			il_strlcpy(p->fstype, old->fstype, sizeof(p->fstype));
			il_strlcpy(p->fs_label, old->fs_label,
				   sizeof(p->fs_label));
			il_strlcpy(p->fs_uuid, old->fs_uuid, sizeof(p->fs_uuid));
			p->fs_free_bytes = old->fs_free_bytes;
		}
	}
	nf = gpt_free_regions(&t, fr, IL_MAX_FREE, 1);
	out->nfree = 0;
	for (int i = 0; i < nf && out->nfree < IL_MAX_FREE; i++) {
		out->free[out->nfree].start_bytes = fr[i].first_lba * ss;
		out->free[out->nfree].size_bytes =
			(fr[i].last_lba - fr[i].first_lba + 1) * ss;
		out->nfree++;
	}
	gpt_table_free(&t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Descriptions                                                        */
/* ------------------------------------------------------------------ */

static const char *il_type_text(const char *guid)
{
	struct gpt_guid g;

	if (!guid || !guid[0] || gpt_guid_parse(guid, &g) != 0)
		return "Unknown";
	return gpt_type_name(&g);
}

void il_op_describe(const struct il_op *op, char *buf, size_t len)
{
	char size[32];

	if (!op || !buf || !len)
		return;
	il_format_size(op->size_bytes, size, sizeof(size));
	switch (op->kind) {
	case IL_OP_NEW_TABLE:
		snprintf(buf, len, "Erase the whole disk and create a new GPT "
			 "partition table");
		break;
	case IL_OP_DELETE:
		if (op->name[0])
			snprintf(buf, len, "Delete partition %d \"%s\" (%s)",
				 op->partno, op->name, size);
		else
			snprintf(buf, len, "Delete partition %d (%s)",
				 op->partno, size);
		break;
	case IL_OP_CREATE:
		snprintf(buf, len, "Create partition %d (%s, %s)", op->partno,
			 size, il_type_text(op->type_guid));
		break;
	case IL_OP_EXTEND:
		snprintf(buf, len, "Extend partition %d to %s", op->partno,
			 size);
		break;
	case IL_OP_FORMAT:
		snprintf(buf, len, "Format partition %d as %s", op->partno,
			 strcmp(op->fstype, "vfat") == 0 ? "FAT32" : op->fstype);
		break;
	case IL_OP_SET_TYPE:
		snprintf(buf, len, "Mark partition %d as %s", op->partno,
			 il_type_text(op->type_guid));
		break;
	default:
		snprintf(buf, len, "Unknown change %d", op->kind);
		break;
	}
}

void il_plan_summary(const il_plan *plan, char *buf, size_t len)
{
	char line[256], desc[192], size[32];
	int esp;

	if (!buf || !len)
		return;
	buf[0] = '\0';
	if (!plan)
		return;
	il_format_size(plan->disk.size_bytes, size, sizeof(size));
	snprintf(line, sizeof(line), "Disk %s (%s%s%s)\n", plan->disk.dev,
		 plan->disk.model[0] ? plan->disk.model : "disk",
		 ", ", size);
	il_strlcat(buf, line, len);
	if (plan->nops == 0)
		il_strlcat(buf, "  No changes to the partitions\n", len);
	for (int i = 0; i < plan->nops; i++) {
		il_op_describe(&plan->ops[i], desc, sizeof(desc));
		snprintf(line, sizeof(line), "  %d. %s\n", i + 1, desc);
		il_strlcat(buf, line, len);
	}
	if (plan->root_partno > 0) {
		snprintf(line, sizeof(line), "LikeOS is installed on partition "
			 "%d\n", plan->root_partno);
		il_strlcat(buf, line, len);
	}
	esp = il_plan_esp(plan);
	if (esp > 0) {
		snprintf(line, sizeof(line), "The boot loader goes to the EFI "
			 "system partition %d\n", esp);
		il_strlcat(buf, line, len);
	}
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

/* The smallest partition mkfs.fat makes a real FAT32 on with this logical
 * sector size: FAT32 needs at least 65525 clusters and the smallest
 * cluster is one sector; mkfs.fat adds 32 reserved sectors and two FATs
 * of four bytes per cluster (and two entries).  Rounded up to whole MiB
 * and never below IL_ESP_MIN_BYTES: with 512-byte sectors that is about
 * 33 MiB (the floor applies), with 4096-byte sectors 257 MiB.  Below it
 * mkfs.fat still writes a "FAT32" with too few clusters, which firmware
 * takes for FAT16. */
static uint64_t il_fat32_min_bytes(uint64_t ss)
{
	const uint64_t clusters = 65525, reserved = 32, nfats = 2;
	uint64_t fat = ((clusters + 2) * 4 + ss - 1) / ss;
	uint64_t bytes = (reserved + nfats * fat + clusters) * ss;

	bytes = (bytes + IL_ALIGN_BYTES - 1) / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
	return bytes < IL_ESP_MIN_BYTES ? IL_ESP_MIN_BYTES : bytes;
}

int il_plan_validate(const il_plan *plan, const struct il_source_info *src,
		     char *why, size_t len)
{
	struct il_disk view;
	const struct il_partition *root, *esp;
	const struct il_op *rf, *ef;
	uint64_t need = il_required_root_bytes(src);
	int espno, rc;

	if (why && len)
		why[0] = '\0';
	if (!plan) {
		il_why(why, len, "No disk has been chosen");
		return -EINVAL;
	}
	if (plan->disk.is_boot_medium) {
		il_why(why, len, "%s holds the running LikeOS (the installation "
		       "medium); choose another disk", plan->disk.dev);
		return -EBUSY;
	}
	if (plan->disk.read_only) {
		il_why(why, len, "%s is read-only", plan->disk.dev);
		return -EROFS;
	}
	if (plan->disk.sector_size != 512 && plan->disk.sector_size != 4096) {
		il_why(why, len, "%s has %u-byte sectors; LikeOS supports 512 "
		       "and 4096", plan->disk.dev, plan->disk.sector_size);
		return -EINVAL;
	}
	if (il_plan_needs_table(plan)) {
		if (plan->disk.table == IL_TABLE_MBR)
			il_why(why, len, "%s uses the older MBR partitioning.  "
			       "LikeOS needs a GPT disk: erase the disk first",
			       plan->disk.dev);
		else
			il_why(why, len, "%s has no partition table yet; create "
			       "a partition or use the entire disk",
			       plan->disk.dev);
		return -EINVAL;
	}
	rc = il_plan_view(plan, &view);
	if (rc) {
		il_why(why, len, "The planned changes do not fit together: %s",
		       plan->ctx && !plan->quiet ? il_last_error(plan->ctx) :
		       strerror(-rc));
		return rc;
	}
	if (plan->root_partno <= 0 ||
	    !(root = il_disk_part(&view, plan->root_partno))) {
		il_why(why, len, "Choose the partition LikeOS is installed on");
		return -EINVAL;
	}
	rf = il_plan_format_op(plan, plan->root_partno);
	if (!rf || strcmp(rf->fstype, "ext4") != 0) {
		il_why(why, len, "Partition %d must be formatted for LikeOS",
		       plan->root_partno);
		return -EINVAL;
	}
	if (root->size_bytes < need) {
		char have[32], want[32];

		il_format_size(root->size_bytes, have, sizeof(have));
		il_format_size(need, want, sizeof(want));
		il_why(why, len, "Partition %d is too small: it has %s, LikeOS "
		       "needs at least %s", plan->root_partno, have, want);
		return -ENOSPC;
	}
	espno = il_plan_esp(plan);
	if (espno <= 0 || !(esp = il_disk_part(&view, espno))) {
		il_why(why, len, "The disk has no EFI system partition; create "
		       "one (a new partition in free space adds it) or use the "
		       "entire disk");
		return -EINVAL;
	}
	if (espno == plan->root_partno) {
		il_why(why, len, "LikeOS and the EFI system partition need "
		       "separate partitions");
		return -EINVAL;
	}
	if (esp->size_bytes < IL_ESP_MIN_BYTES) {
		char have[32];

		il_format_size(esp->size_bytes, have, sizeof(have));
		il_why(why, len, "The EFI system partition %d is too small (%s; "
		       "at least 100 MB are needed)", espno, have);
		return -ENOSPC;
	}
	ef = il_plan_format_op(plan, espno);
	if (ef && strcmp(ef->fstype, "vfat") == 0 &&
	    esp->size_bytes < il_fat32_min_bytes(il_ss(&plan->disk))) {
		char have[32], want[32];

		il_format_size(esp->size_bytes, have, sizeof(have));
		il_format_size(il_fat32_min_bytes(il_ss(&plan->disk)), want,
			       sizeof(want));
		il_why(why, len, "The EFI system partition %d is too small to be "
		       "formatted with FAT32 on a disk with %u-byte sectors "
		       "(%s; at least %s are needed)", espno,
		       (unsigned)il_ss(&plan->disk), have, want);
		return -ENOSPC;
	}
	if (!(ef && strcmp(ef->fstype, "vfat") == 0) &&
	    strcmp(esp->fstype, "vfat") != 0) {
		il_why(why, len, "The EFI system partition %d has no FAT file "
		       "system; format it", espno);
		return -EINVAL;
	}
	/* Nothing the plan changes may be in use. */
	if (plan->ctx && !plan->ctx->simulate) {
		for (int i = 0; i < plan->disk.nparts; i++) {
			const struct il_partition *p = &plan->disk.parts[i];
			int touched = il_plan_erases_disk(plan);

			for (int j = 0; !touched && j < plan->nops; j++)
				if (plan->ops[j].partno == p->number)
					touched = 1;
			if (touched && il_is_mounted(plan->ctx, p->dev)) {
				il_why(why, len, "%s is in use (mounted); it "
				       "cannot be changed", p->dev);
				return -EBUSY;
			}
		}
	}
	return 0;
}
