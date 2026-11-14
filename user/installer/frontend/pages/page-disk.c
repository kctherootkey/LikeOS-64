/* LikeOS installer -- page 4: where LikeOS goes.
 *
 * A tree of the computer's disks with their partitions and unallocated
 * space (name, total size, free space, type), and the tools to prepare
 * them: refresh, delete, format, new (with an inline "Size in MB" row),
 * extend, use the entire disk, convert to GPT, undo, and the details of a
 * disk.  Nothing is written here: every tool edits the disk's plan in the
 * backend (one step of plans.c each, so Undo takes the last one back), the
 * tree shows the plan's projected layout, and the changes are made only
 * when the installation starts.  The medium setup was started from is
 * shown greyed out and cannot be chosen.
 *
 * Whether a partition can take LikeOS is the backend's answer
 * (il_plan_can_select_root, which checks the whole plan as the
 * installation would); Next makes the choice (il_plan_select_root), and
 * coming back to the page takes it back (il_plan_clear_root).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "pages.h"

#include <errno.h>
#include <string.h>

#define MIB (1024ULL * 1024ULL)

enum {
	COL_ICON,
	COL_NAME,
	COL_TOTAL,
	COL_FREE,
	COL_TYPE,
	COL_DISK,
	COL_KIND,
	COL_PARTNO,
	COL_FREEIDX,
	COL_SENSITIVE,
	COL_WEIGHT,
	COL_START,
	N_COLS
};

enum { ROW_DISK, ROW_PART, ROW_FREE };

enum { SIZE_NONE, SIZE_NEW, SIZE_EXTEND };

enum {
	ACT_REFRESH,
	ACT_DELETE,
	ACT_FORMAT,
	ACT_NEW,
	ACT_EXTEND,
	ACT_WHOLE,
	ACT_CONVERT,
	ACT_UNDO,
	ACT_DETAILS,
	N_ACTS
};

typedef struct {
	GtkWidget *grid;
	GtkWidget *acts[N_ACTS];
	GtkWidget *size_revealer;
	GtkWidget *size_label;
	GtkWidget *size_spin;
	GtkWidget *size_range;
	GtkWidget *size_apply;
	int size_mode;
	GtkWidget *tree;
	GtkTreeStore *store;
	GtkTreeViewColumn *col_free;
	GtkTreeViewColumn *col_type;
	GtkWidget *notice;
	GtkWidget *changes;
	gboolean scanning;
	gboolean quiet;

	/* the selected row */
	int sel_disk, sel_kind, sel_partno, sel_free;
	guint64 sel_start;
	/* the selected disk by device and size: a new scan may number the
	 * disks differently */
	char sel_dev[64];
	guint64 sel_size;
	char *message;           /* a message to show after the next refresh */
	NoticeKind message_kind;
} DiskPage;

/* ---- helpers ------------------------------------------------------- */

static il_plan *ensure_plan(App *app, int di)
{
	return plans_get(app, di);
}

/* The partition's kind as the tree shows it: the backend's name, put in
 * neutral words for the kinds named after another system's maker. */
static const char *part_kind(const struct il_partition *p)
{
	if (!g_ascii_strcasecmp(p->type_guid, IL_GUID_MSR))
		return "Reserved (MSR)";
	if (!g_ascii_strcasecmp(p->type_guid,
				"de94bba4-06d1-4d40-a16a-bfd50179d6ac"))
		return "Recovery";
	return p->type_desc[0] ? p->type_desc : "Partition";
}

/* The disk as the plan sees it (or as scanned, when it has no plan). */
static gboolean disk_view(App *app, int di, struct il_disk *out)
{
	il_plan *plan;

	if (!app->disks || di < 0 || di >= app->disks->n)
		return FALSE;
	plan = ensure_plan(app, di);
	if (plan && il_plan_view(plan, out) == 0)
		return TRUE;
	*out = app->disks->disks[di];
	return TRUE;
}

static gboolean op_for(il_plan *plan, int kind, int partno)
{
	int n = plan ? il_plan_nops(plan) : 0;

	for (int i = 0; i < n; i++) {
		const struct il_op *op = il_plan_op(plan, i);

		if (op && op->kind == kind && op->partno == partno)
			return TRUE;
	}
	return FALSE;
}

static gboolean plan_has_table_op(il_plan *plan)
{
	return op_for(plan, IL_OP_NEW_TABLE, 0);
}

static const char *fs_name(const char *fstype)
{
	if (!fstype || !*fstype)
		return NULL;
	if (strcmp(fstype, "vfat") == 0)
		return "FAT32";
	if (strcmp(fstype, "ntfs") == 0)
		return "NTFS";
	if (strcmp(fstype, "swap") == 0)
		return "swap";
	return fstype;
}

static const char *table_name(int table)
{
	switch (table) {
	case IL_TABLE_GPT: return "GPT";
	case IL_TABLE_MBR: return "MBR (legacy)";
	default:           return "Not initialized";
	}
}

static const char *transport_name(int t)
{
	switch (t) {
	case IL_TRANSPORT_SATA: return "SATA";
	case IL_TRANSPORT_NVME: return "NVMe";
	case IL_TRANSPORT_USB:  return "USB";
	case IL_TRANSPORT_FILE: return "Image file";
	default:                return "Unknown";
	}
}

static guint64 required_bytes(App *app)
{
	return app->src_ok ? app->src.required_bytes : 0;
}

static const struct il_partition *find_part(const struct il_disk *d, int partno)
{
	for (int i = 0; i < d->nparts; i++)
		if (d->parts[i].number == partno)
			return &d->parts[i];
	return NULL;
}

static guint64 free_after(const struct il_disk *d, const struct il_partition *p)
{
	guint64 end = p->start_bytes + p->size_bytes;

	end = (end + IL_ALIGN_BYTES - 1) / IL_ALIGN_BYTES * IL_ALIGN_BYTES;
	for (int i = 0; i < d->nfree; i++)
		if (d->free[i].start_bytes == end)
			return d->free[i].size_bytes;
	return 0;
}

static gboolean has_esp(const struct il_disk *d)
{
	for (int i = 0; i < d->nparts; i++)
		if (d->parts[i].is_esp)
			return TRUE;
	return FALSE;
}

static void show_error(DiskPage *dp, App *app, const char *fallback)
{
	const char *e = il_last_error(app->ctx);
	char *s = app_sentence(e && *e ? e : fallback);
	char *m = pages_escape(s);

	app_log(app, IL_LOG_INFO, "ui: disk page shows: %s", s);
	notice_set(dp->notice, NOTICE_ERROR, m);
	g_free(m);
	g_free(s);
}

static void set_message(DiskPage *dp, NoticeKind kind, const char *markup)
{
	g_free(dp->message);
	dp->message = g_strdup(markup);
	dp->message_kind = kind;
}

/* ---- the tree ------------------------------------------------------ */

static GdkPixbuf *row_icon(IconKind kind, guint32 rgb)
{
	GdkRGBA c = { ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
		      (rgb & 0xff) / 255.0, 1.0 };

	return icon_pixbuf(kind, 18, &c);
}

typedef struct {
	gboolean is_free;
	guint64 start;
	int index;
} Slot;

static int slot_cmp(gconstpointer a, gconstpointer b)
{
	const Slot *x = a, *y = b;

	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	return 0;
}

static void add_disk_rows(DiskPage *dp, App *app, int di)
{
	struct il_disk *v = g_new0(struct il_disk, 1);
	const struct il_disk *d = &app->disks->disks[di];
	il_plan *plan = ensure_plan(app, di);
	GtkTreeIter it, child;
	GdkPixbuf *icon;
	GArray *slots;
	guint64 free_total = 0;
	char *name, *total, *freestr, *type;
	gboolean sens = !d->is_boot_medium && !d->read_only;

	disk_view(app, di, v);
	for (int i = 0; i < v->nfree; i++)
		free_total += v->free[i].size_bytes;

	icon = row_icon(d->transport == IL_TRANSPORT_USB ? ICON_USB : ICON_DISK,
			sens ? 0x3b4558 : 0x9aa3b2);
	name = g_strdup_printf("Disk %d \xc2\xb7 %s", di, d->model[0] ? d->model :
			       d->name);
	total = app_size_text(d->size_bytes);
	freestr = app_size_text(free_total);
	if (d->is_boot_medium)
		type = g_strdup("Installation medium");
	else if (d->read_only)
		type = g_strdup("Write-protected");
	else if (plan && plan_has_table_op(plan))
		type = g_strdup("GPT (new)");
	else
		type = g_strdup(table_name(v->table));

	gtk_tree_store_append(dp->store, &it, NULL);
	gtk_tree_store_set(dp->store, &it, COL_ICON, icon, COL_NAME, name,
			   COL_TOTAL, total, COL_FREE, freestr, COL_TYPE, type,
			   COL_DISK, di, COL_KIND, ROW_DISK, COL_PARTNO, -1,
			   COL_FREEIDX, -1, COL_SENSITIVE, sens,
			   COL_WEIGHT, PANGO_WEIGHT_SEMIBOLD, COL_START, (guint64)0,
			   -1);
	g_object_unref(icon);
	g_free(name);
	g_free(total);
	g_free(freestr);
	g_free(type);

	slots = g_array_new(FALSE, FALSE, sizeof(Slot));
	for (int i = 0; i < v->nparts; i++) {
		Slot s = { FALSE, v->parts[i].start_bytes, i };

		g_array_append_val(slots, s);
	}
	for (int i = 0; i < v->nfree; i++) {
		Slot s = { TRUE, v->free[i].start_bytes, i };

		g_array_append_val(slots, s);
	}
	g_array_sort(slots, slot_cmp);

	for (guint k = 0; k < slots->len; k++) {
		Slot *s = &g_array_index(slots, Slot, k);

		gtk_tree_store_append(dp->store, &child, &it);
		if (s->is_free) {
			const struct il_free_region *r = &v->free[s->index];

			icon = row_icon(ICON_FREE, sens ? 0x6b7486 : 0xaab2bf);
			name = g_strdup_printf("Disk %d Unallocated space", di);
			total = app_size_text(r->size_bytes);
			gtk_tree_store_set(dp->store, &child, COL_ICON, icon,
					   COL_NAME, name, COL_TOTAL, total,
					   COL_FREE, total, COL_TYPE,
					   "Unallocated space", COL_DISK, di,
					   COL_KIND, ROW_FREE, COL_PARTNO, -1,
					   COL_FREEIDX, s->index, COL_SENSITIVE, sens,
					   COL_WEIGHT, PANGO_WEIGHT_NORMAL,
					   COL_START, r->start_bytes, -1);
			g_free(total);
		} else {
			const struct il_partition *p = &v->parts[s->index];
			gboolean created = op_for(plan, IL_OP_CREATE, p->number);
			gboolean fmt = op_for(plan, IL_OP_FORMAT, p->number);
			const char *fs = fs_name(p->fstype);
			const char *label = p->fs_label[0] ? p->fs_label : p->name;
			GString *t = g_string_new(part_kind(p));

			icon = row_icon(ICON_PARTITION, !sens ? 0xaab2bf :
					p->is_esp ? 0x17b8a6 : 0x2a5bd7);
			if (label[0])
				name = g_strdup_printf("Disk %d Partition %d \xc2\xb7 %s",
						       di, p->number, label);
			else
				name = g_strdup_printf("Disk %d Partition %d", di,
						       p->number);
			if (fs)
				g_string_append_printf(t, " \xc2\xb7 %s", fs);
			if (created)
				g_string_append(t, " (new)");
			else if (fmt)
				g_string_append(t, " (will be formatted)");
			total = app_size_text(p->size_bytes);
			freestr = p->fs_free_bytes ? app_size_text(p->fs_free_bytes) :
				created ? app_size_text(p->size_bytes) :
				g_strdup("\xe2\x80\x94");
			gtk_tree_store_set(dp->store, &child, COL_ICON, icon,
					   COL_NAME, name, COL_TOTAL, total,
					   COL_FREE, freestr, COL_TYPE, t->str,
					   COL_DISK, di, COL_KIND, ROW_PART,
					   COL_PARTNO, p->number, COL_FREEIDX, -1,
					   COL_SENSITIVE, sens,
					   COL_WEIGHT, created ? PANGO_WEIGHT_SEMIBOLD :
					   PANGO_WEIGHT_NORMAL,
					   COL_START, p->start_bytes, -1);
			g_string_free(t, TRUE);
			g_free(total);
			g_free(freestr);
		}
		g_object_unref(icon);
		g_free(name);
	}
	g_array_free(slots, TRUE);
	g_free(v);
}

static gboolean select_matching(DiskPage *dp, int di, int kind, int partno,
				guint64 start)
{
	GtkTreeModel *m = GTK_TREE_MODEL(dp->store);
	GtkTreeIter it, child;
	gboolean ok = gtk_tree_model_get_iter_first(m, &it);

	for (; ok; ok = gtk_tree_model_iter_next(m, &it)) {
		int d;

		gtk_tree_model_get(m, &it, COL_DISK, &d, -1);
		if (d != di)
			continue;
		if (kind == ROW_DISK) {
			gtk_tree_selection_select_iter(gtk_tree_view_get_selection(
				GTK_TREE_VIEW(dp->tree)), &it);
			return TRUE;
		}
		for (gboolean c = gtk_tree_model_iter_children(m, &child, &it); c;
		     c = gtk_tree_model_iter_next(m, &child)) {
			int k, pn;
			guint64 st;

			gtk_tree_model_get(m, &child, COL_KIND, &k, COL_PARTNO, &pn,
					   COL_START, &st, -1);
			if (k != kind)
				continue;
			if ((kind == ROW_PART && pn == partno) ||
			    (kind == ROW_FREE && st == start)) {
				GtkTreePath *path = gtk_tree_model_get_path(m, &child);

				gtk_tree_selection_select_iter(
					gtk_tree_view_get_selection(
						GTK_TREE_VIEW(dp->tree)), &child);
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(dp->tree),
							     path, NULL, FALSE, 0, 0);
				gtk_tree_path_free(path);
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* The first row someone would install on: unallocated space or a disk
 * without partitions on the first eligible disk. */
static void select_default(DiskPage *dp, App *app)
{
	for (int di = 0; app->disks && di < app->disks->n; di++) {
		struct il_disk v;

		if (app->disks->disks[di].is_boot_medium ||
		    app->disks->disks[di].read_only)
			continue;
		disk_view(app, di, &v);
		if (v.nparts == 0) {
			select_matching(dp, di, ROW_DISK, -1, 0);
			return;
		}
		for (int i = 0; i < v.nfree; i++) {
			if (v.free[i].size_bytes >= required_bytes(app)) {
				select_matching(dp, di, ROW_FREE, -1,
						v.free[i].start_bytes);
				return;
			}
		}
		select_matching(dp, di, ROW_DISK, -1, 0);
		return;
	}
}

static void update_state(Page *p);

/* Notes which disk sel_disk is, for finding it again after a scan. */
static void remember_disk(DiskPage *dp, App *app)
{
	if (app->disks && dp->sel_disk >= 0 && dp->sel_disk < app->disks->n) {
		g_strlcpy(dp->sel_dev, app->disks->disks[dp->sel_disk].dev,
			  sizeof(dp->sel_dev));
		dp->sel_size = app->disks->disks[dp->sel_disk].size_bytes;
	} else {
		dp->sel_dev[0] = '\0';
		dp->sel_size = 0;
	}
}

static void populate(Page *p)
{
	DiskPage *dp = p->priv;
	App *app = p->app;
	int sd = -1, sk = dp->sel_kind, sp = dp->sel_partno;
	guint64 ss = dp->sel_start;
	gboolean had;

	/* the selection's disk in the list as it is now */
	for (int i = 0; dp->sel_disk >= 0 && dp->sel_dev[0] && app->disks &&
	     i < app->disks->n; i++)
		if (strcmp(app->disks->disks[i].dev, dp->sel_dev) == 0 &&
		    app->disks->disks[i].size_bytes == dp->sel_size)
			sd = i;
	had = sd >= 0;

	dp->quiet = TRUE;
	gtk_tree_store_clear(dp->store);
	for (int di = 0; app->disks && di < app->disks->n; di++)
		add_disk_rows(dp, app, di);
	gtk_tree_view_expand_all(GTK_TREE_VIEW(dp->tree));
	dp->quiet = FALSE;

	if (!had || !select_matching(dp, sd, sk, sp, ss)) {
		if (!(had && select_matching(dp, sd, ROW_DISK, -1, 0)))
			select_default(dp, app);
	}
	update_state(p);
}

/* ---- the state of the tools and the notice ------------------------- */

static void read_selection(DiskPage *dp)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(dp->tree));
	GtkTreeModel *m;
	GtkTreeIter it;

	dp->sel_disk = -1;
	dp->sel_kind = ROW_DISK;
	dp->sel_partno = -1;
	dp->sel_free = -1;
	dp->sel_start = 0;
	if (!gtk_tree_selection_get_selected(sel, &m, &it))
		return;
	gtk_tree_model_get(m, &it, COL_DISK, &dp->sel_disk, COL_KIND, &dp->sel_kind,
			   COL_PARTNO, &dp->sel_partno, COL_FREEIDX, &dp->sel_free,
			   COL_START, &dp->sel_start, -1);
}

static void set_act(DiskPage *dp, int a, gboolean on)
{
	gtk_widget_set_sensitive(dp->acts[a], on && !dp->scanning);
}

static void update_changes_line(DiskPage *dp, App *app)
{
	int total = 0, disks = 0;

	for (int i = 0; app->disks && i < app->disks->n; i++) {
		int n = app->plans[i] ? il_plan_nops(app->plans[i]) : 0;

		if (n) {
			total += n;
			disks++;
		}
	}
	if (!total) {
		gtk_label_set_text(GTK_LABEL(dp->changes),
				   "No changes yet. Nothing is written to a disk "
				   "until you choose Install.");
	} else {
		char *t = g_strdup_printf(total == 1 ?
					  "%d change is waiting on %d %s. It is made "
					  "when the installation starts." :
					  "%d changes are waiting on %d %s. They are "
					  "made when the installation starts.",
					  total, disks, disks == 1 ? "disk" : "disks");

		gtk_label_set_text(GTK_LABEL(dp->changes), t);
		g_free(t);
	}
}

static void update_state(Page *p)
{
	DiskPage *dp = p->priv;
	App *app = p->app;
	struct il_disk *v = g_new0(struct il_disk, 1);
	const struct il_disk *d = NULL;
	const struct il_partition *part = NULL;
	gboolean eligible = FALSE, ok = FALSE;
	guint64 need = required_bytes(app);
	char *need_s = app_size_text(need);
	char *msg = NULL;
	NoticeKind kind = NOTICE_INFO;
	il_plan *plan = NULL;

	read_selection(dp);
	remember_disk(dp, app);
	/* while the disks are scanned the plans belong to nobody: the
	 * worker has the context */
	if (dp->sel_disk >= 0 && app->disks && !dp->scanning) {
		d = &app->disks->disks[dp->sel_disk];
		disk_view(app, dp->sel_disk, v);
		eligible = !d->is_boot_medium && !d->read_only;
		plan = eligible ? ensure_plan(app, dp->sel_disk) : NULL;
		if (dp->sel_kind == ROW_PART)
			part = find_part(v, dp->sel_partno);
	}

	set_act(dp, ACT_REFRESH, TRUE);
	set_act(dp, ACT_DELETE, eligible && part);
	set_act(dp, ACT_FORMAT, eligible && part);
	set_act(dp, ACT_NEW, eligible && dp->sel_kind == ROW_FREE &&
		v->table != IL_TABLE_MBR);
	set_act(dp, ACT_EXTEND, eligible && part && free_after(v, part) > 0);
	set_act(dp, ACT_WHOLE, eligible);
	set_act(dp, ACT_CONVERT, eligible && v->table == IL_TABLE_MBR);
	set_act(dp, ACT_UNDO, eligible && plans_nsteps(app, dp->sel_disk) > 0);
	set_act(dp, ACT_DETAILS, d != NULL);

	if (dp->scanning) {
		msg = g_strdup("Looking for disks\xe2\x80\xa6");
	} else if (!app->disks || app->disks->n == 0 || !d) {
		gboolean any = FALSE;

		for (int i = 0; app->disks && i < app->disks->n; i++)
			if (!app->disks->disks[i].is_boot_medium)
				any = TRUE;
		if (!any) {
			kind = NOTICE_WARNING;
			msg = g_strdup("<b>No disk was found to install LikeOS on.</b>\n"
				       "Connect a disk, then choose Refresh.");
		} else {
			msg = g_strdup("Select the partition or the unallocated space "
				       "you want to install LikeOS on.");
		}
	} else if (d->is_boot_medium) {
		msg = g_strdup("This is the medium setup was started from. LikeOS "
			       "cannot be installed on it.");
	} else if (d->read_only) {
		kind = NOTICE_WARNING;
		msg = g_strdup("This disk is write-protected.");
	} else if (v->table == IL_TABLE_MBR) {
		kind = NOTICE_WARNING;
		msg = g_strdup("<b>This disk uses the older MBR partition table.</b>\n"
			       "LikeOS starts from disks with a GPT partition table. "
			       "Choose <i>Convert to GPT</i> or <i>Use entire disk</i> "
			       "\xe2\x80\x94 both erase everything on the disk.");
	} else if (dp->sel_kind == ROW_DISK) {
		if (v->nparts == 0) {
			if (d->size_bytes >= need + IL_ESP_SIZE_BYTES) {
				ok = TRUE;
				msg = g_strdup_printf(
					"LikeOS will use the whole disk: a 512 MiB EFI "
					"system partition and the rest for the system. "
					"LikeOS needs at least %s.", need_s);
			} else {
				kind = NOTICE_WARNING;
				msg = g_strdup_printf("This disk is too small. LikeOS "
						      "needs at least %s.", need_s);
			}
		} else {
			msg = g_strdup("Select a partition or unallocated space on "
				       "this disk, or choose <i>Use entire disk</i>.");
		}
	} else if (dp->sel_kind == ROW_FREE && dp->sel_free >= 0 &&
		   dp->sel_free < v->nfree) {
		guint64 room = v->free[dp->sel_free].size_bytes;
		gboolean esp = has_esp(v);
		guint64 want = need + (esp ? 0 : IL_ESP_SIZE_BYTES);

		if (room < want) {
			char *have = app_size_text(room), *w = app_size_text(want);

			kind = NOTICE_WARNING;
			msg = g_strdup_printf("This space is too small: LikeOS needs "
					      "at least %s here, it has %s.", w, have);
			g_free(have);
			g_free(w);
		} else {
			ok = TRUE;
			msg = g_strdup_printf("LikeOS creates a new partition in this "
					      "space%s. Nothing else on the disk is "
					      "touched.", esp ? "" :
					      ", and a 512 MiB EFI system partition "
					      "in front of it to start the computer");
		}
	} else if (part) {
		/* the backend's answer: the choice tried on a copy of the plan
		 * and checked as the installation will check it */
		char why[IL_ERROR_MAX];
		int rc = plan ? il_plan_can_select_root(plan, part->number,
							app->src_ok ? &app->src :
							NULL, why, sizeof(why)) :
			-EINVAL;

		if (rc == 0) {
			ok = TRUE;
			/* a partition that is there now, whatever it holds (an
			 * unknown file system is no empty one) */
			if (!op_for(plan, IL_OP_CREATE, part->number)) {
				kind = NOTICE_WARNING;
				msg = g_strdup_printf("<b>Partition %d will be formatted.</b> "
						      "Everything on it is erased when "
						      "the installation starts.",
						      part->number);
			} else {
				msg = g_strdup_printf("LikeOS is installed on partition "
						      "%d.", part->number);
			}
		} else if (part->is_esp) {
			msg = g_strdup("This is the EFI system partition. LikeOS uses "
				       "it to start, but is not installed on it.");
		} else {
			char *w = app_sentence(why[0] ? why :
					       "LikeOS cannot be installed there");

			kind = NOTICE_WARNING;
			msg = pages_escape(w);
			g_free(w);
		}
	}

	if (dp->message) {
		notice_set(dp->notice, dp->message_kind, dp->message);
		g_clear_pointer(&dp->message, g_free);
	} else {
		notice_set(dp->notice, kind, msg);
	}
	if (!dp->scanning)
		update_changes_line(dp, app);

	p->next_ok = ok && !dp->scanning;
	page_update(p);
	g_free(msg);
	g_free(need_s);
	g_free(v);
}

static void on_selection(GtkTreeSelection *sel, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)sel;
	if (dp->quiet)
		return;
	if (dp->size_mode != SIZE_NONE) {
		dp->size_mode = SIZE_NONE;
		gtk_revealer_set_reveal_child(GTK_REVEALER(dp->size_revealer), FALSE);
	}
	update_state(p);
}

static gboolean select_func(GtkTreeSelection *sel, GtkTreeModel *m,
			    GtkTreePath *path, gboolean selected, gpointer data)
{
	GtkTreeIter it;
	gboolean sens = TRUE;
	int kind = ROW_DISK;

	(void)sel;
	(void)selected;
	(void)data;
	if (gtk_tree_model_get_iter(m, &it, path))
		gtk_tree_model_get(m, &it, COL_SENSITIVE, &sens, COL_KIND, &kind, -1);
	/* The boot medium's disk row can be selected (to read its details),
	 * its partitions cannot. */
	return sens || kind == ROW_DISK;
}

/* ---- tools --------------------------------------------------------- */

static void scan_done(App *app, int rc, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)rc;
	dp->scanning = FALSE;
	gtk_widget_set_sensitive(dp->tree, TRUE);
	if (!app->disks_ok) {
		char *m = pages_escape(app->disks_error);

		set_message(dp, NOTICE_ERROR, m);
		g_free(m);
	}
	populate(p);
}

static void do_refresh(Page *p)
{
	DiskPage *dp = p->priv;

	app_log(p->app, IL_LOG_INFO, "ui: looking for disks again");
	if (!bridge_scan_disks(p->app->bridge, scan_done, p))
		return;
	dp->scanning = TRUE;
	gtk_widget_set_sensitive(dp->tree, FALSE);
	update_state(p);
}

static void refresh_confirmed(Sheet *s, int response, gpointer data)
{
	(void)s;
	if (response == SHEET_OK)
		do_refresh(data);
}

static void on_refresh(GtkButton *b, gpointer data)
{
	Page *p = data;
	App *app = p->app;
	gboolean pending = FALSE;

	(void)b;
	for (int i = 0; app->disks && i < app->disks->n; i++)
		if (plans_nsteps(app, i) ||
		    (app->plans[i] && il_plan_nops(app->plans[i])))
			pending = TRUE;
	if (pending)
		sheet_confirm(app, "Discard your changes?",
			      "Looking for disks again forgets the changes you "
			      "made on this page. Nothing was written to a disk yet.",
			      "_Discard and refresh", TRUE, ART_DISK,
			      refresh_confirmed, p);
	else
		do_refresh(p);
}

/* One tool's change: a step of the disk's plan (plans.c). */
static int apply_step(Page *p, PlanStepKind kind, int partno, guint64 start,
		      guint64 size, int *partno_out)
{
	DiskPage *dp = p->priv;
	PlanStep st = { kind, partno, start, size };
	char *what = plans_step_describe(&st);
	int rc = plans_apply(p->app, dp->sel_disk, &st, partno_out);

	app_log(p->app, IL_LOG_INFO, "ui: disk %d (%s): %s%s", dp->sel_disk,
		p->app->disks->disks[dp->sel_disk].dev, what,
		rc ? " was refused" : "");
	g_free(what);
	return rc;
}

static void delete_confirmed(Sheet *s, int response, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)s;
	if (response != SHEET_OK || !ensure_plan(p->app, dp->sel_disk))
		return;
	if (apply_step(p, PLAN_STEP_DELETE, dp->sel_partno, 0, 0, NULL) != 0) {
		show_error(dp, p->app, "The partition could not be deleted.");
		return;
	}
	dp->sel_kind = ROW_DISK;
	populate(p);
}

static void on_delete(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	struct il_disk v;
	const struct il_partition *part;
	il_plan *plan;
	char *size, *t;

	(void)b;
	read_selection(dp);
	plan = ensure_plan(p->app, dp->sel_disk);
	if (!plan || !disk_view(p->app, dp->sel_disk, &v) ||
	    !(part = find_part(&v, dp->sel_partno)))
		return;
	if (op_for(plan, IL_OP_CREATE, part->number)) {
		/* a partition of the plan: nothing to lose, no question */
		delete_confirmed(NULL, SHEET_OK, p);
		return;
	}
	size = app_size_text(part->size_bytes);
	t = g_strdup_printf("Partition %d of Disk %d (%s%s%s) may hold files and "
			    "programs. When the installation starts it is deleted "
			    "and everything on it is lost.", part->number,
			    dp->sel_disk, size, part->fs_label[0] ? ", " : "",
			    part->fs_label);
	sheet_confirm(p->app, "Delete this partition?", t, "_Delete", TRUE,
		      ART_WARNING, delete_confirmed, p);
	g_free(t);
	g_free(size);
}

static void format_confirmed(Sheet *s, int response, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)s;
	if (response != SHEET_OK || !ensure_plan(p->app, dp->sel_disk))
		return;
	if (apply_step(p, PLAN_STEP_FORMAT, dp->sel_partno, 0, 0, NULL) != 0) {
		show_error(dp, p->app, "The partition could not be formatted.");
		return;
	}
	populate(p);
}

static void on_format(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	char *t;

	(void)b;
	read_selection(dp);
	t = g_strdup_printf("Partition %d of Disk %d gets a new, empty file system "
			    "when the installation starts. Everything on it is "
			    "lost.", dp->sel_partno, dp->sel_disk);
	sheet_confirm(p->app, "Format this partition?", t, "_Format", TRUE,
		      ART_WARNING, format_confirmed, p);
	g_free(t);
}

static void size_row_show(Page *p, int mode, guint64 min_mib, guint64 max_mib,
			  guint64 value_mib)
{
	DiskPage *dp = p->priv;
	char *range;

	dp->size_mode = mode;
	gtk_label_set_text_with_mnemonic(GTK_LABEL(dp->size_label),
					 mode == SIZE_NEW ? "_Size in MB:" :
					 "New _size in MB:");
	gtk_spin_button_set_range(GTK_SPIN_BUTTON(dp->size_spin), (double)min_mib,
				  (double)max_mib);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dp->size_spin), (double)value_mib);
	range = g_strdup_printf("at most %" G_GUINT64_FORMAT, max_mib);
	gtk_widget_set_tooltip_text(dp->size_spin, NULL);
	gtk_label_set_text(GTK_LABEL(dp->size_range), range);
	g_free(range);
	range = g_strdup_printf("From %" G_GUINT64_FORMAT " to %" G_GUINT64_FORMAT
				" MB", min_mib, max_mib);
	gtk_widget_set_tooltip_text(dp->size_spin, range);
	g_free(range);
	gtk_revealer_set_reveal_child(GTK_REVEALER(dp->size_revealer), TRUE);
	page_focus_later(dp->size_spin);
}

static void on_new(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	struct il_disk v;
	guint64 mib;

	(void)b;
	read_selection(dp);
	if (!disk_view(p->app, dp->sel_disk, &v) || dp->sel_free < 0 ||
	    dp->sel_free >= v.nfree)
		return;
	mib = v.free[dp->sel_free].size_bytes / MIB;
	size_row_show(p, SIZE_NEW, 1, mib, mib);
}

static void on_extend(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	struct il_disk v;
	const struct il_partition *part;
	guint64 cur, max;

	(void)b;
	read_selection(dp);
	if (!disk_view(p->app, dp->sel_disk, &v) ||
	    !(part = find_part(&v, dp->sel_partno)))
		return;
	cur = part->size_bytes / MIB;
	max = cur + free_after(&v, part) / MIB;
	size_row_show(p, SIZE_EXTEND, cur + 1, max, max);
}

static void on_size_apply(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	il_plan *plan = ensure_plan(p->app, dp->sel_disk);
	guint64 mib;
	double upper;
	int rc = -EINVAL, partno = -1;
	struct il_disk before;

	(void)b;
	if (!plan)
		return;
	/* Enter in the field comes here before the spin button has read
	 * what was typed */
	gtk_spin_button_update(GTK_SPIN_BUTTON(dp->size_spin));
	mib = (guint64)gtk_spin_button_get_value_as_int(
		GTK_SPIN_BUTTON(dp->size_spin));
	gtk_spin_button_get_range(GTK_SPIN_BUTTON(dp->size_spin), NULL, &upper);
	disk_view(p->app, dp->sel_disk, &before);
	if (dp->size_mode == SIZE_NEW) {
		guint64 bytes = mib >= (guint64)upper ? 0 : mib * MIB;

		rc = apply_step(p, PLAN_STEP_CREATE, 0, dp->sel_start, bytes,
				&partno);
		if (rc == 0) {
			dp->sel_kind = ROW_PART;
			dp->sel_partno = partno;
			if (!has_esp(&before))
				set_message(dp, NOTICE_INFO,
					    "Setup also added a 512 MiB <b>EFI system "
					    "partition</b> in front of the new one. "
					    "The computer starts LikeOS from it.");
		}
	} else if (dp->size_mode == SIZE_EXTEND) {
		rc = apply_step(p, PLAN_STEP_EXTEND, dp->sel_partno, 0, mib * MIB,
				NULL);
	}
	if (rc != 0) {
		show_error(dp, p->app, "The change could not be made.");
		return;
	}
	dp->size_mode = SIZE_NONE;
	gtk_revealer_set_reveal_child(GTK_REVEALER(dp->size_revealer), FALSE);
	populate(p);
}

static void on_size_cancel(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)b;
	dp->size_mode = SIZE_NONE;
	gtk_revealer_set_reveal_child(GTK_REVEALER(dp->size_revealer), FALSE);
	gtk_widget_grab_focus(dp->tree);
}

static void whole_confirmed(Sheet *s, int response, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)s;
	if (response != SHEET_OK || !ensure_plan(p->app, dp->sel_disk))
		return;
	if (apply_step(p, PLAN_STEP_WHOLE, 0, 0, 0, NULL) != 0) {
		show_error(dp, p->app, "The disk could not be prepared.");
		return;
	}
	dp->sel_kind = ROW_PART;
	dp->sel_partno = il_plan_root(ensure_plan(p->app, dp->sel_disk));
	set_message(dp, NOTICE_SUCCESS,
		    "The whole disk is used: a 512 MiB EFI system partition and "
		    "one partition for LikeOS. The disk is erased when the "
		    "installation starts.");
	populate(p);
}

static void on_whole(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	const struct il_disk *d;
	char *size, *t;

	(void)b;
	read_selection(dp);
	if (!p->app->disks || dp->sel_disk < 0)
		return;
	d = &p->app->disks->disks[dp->sel_disk];
	size = app_size_text(d->size_bytes);
	t = g_strdup_printf("Disk %d (%s, %s) gets a new partition table with an EFI "
			    "system partition and one partition for LikeOS. "
			    "<b>Every partition on it, and everything they hold, "
			    "is erased</b> when the installation starts.",
			    dp->sel_disk, d->model[0] ? d->model : d->name, size);
	sheet_confirm(p->app, "Use the entire disk?", t, "_Use entire disk", TRUE,
		      ART_DISK, whole_confirmed, p);
	g_free(t);
	g_free(size);
}

static void convert_confirmed(Sheet *s, int response, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;

	(void)s;
	if (response != SHEET_OK || !ensure_plan(p->app, dp->sel_disk))
		return;
	if (apply_step(p, PLAN_STEP_NEW_TABLE, 0, 0, 0, NULL) != 0) {
		show_error(dp, p->app, "The disk could not be converted.");
		return;
	}
	dp->sel_kind = ROW_DISK;
	set_message(dp, NOTICE_INFO,
		    "The disk will get an empty GPT partition table. Select the "
		    "unallocated space to install LikeOS on it.");
	populate(p);
	select_matching(dp, dp->sel_disk, ROW_FREE, -1, IL_ALIGN_BYTES);
}

static void on_convert(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	char *t;

	(void)b;
	read_selection(dp);
	t = g_strdup_printf("Disk %d gets a new, empty GPT partition table. "
			    "<b>Every partition on it, and everything they hold, "
			    "is erased</b> when the installation starts.",
			    dp->sel_disk);
	sheet_confirm(p->app, "Convert to GPT?", t, "_Convert", TRUE, ART_DISK,
		      convert_confirmed, p);
	g_free(t);
}

/* Takes back the last change to the disk: the plan is made again from
 * the scan with every change but that one (plans.c). */
static void on_undo(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	App *app = p->app;
	char *what = NULL, *t;
	int rc, left;

	(void)b;
	read_selection(dp);
	if (dp->sel_disk < 0 || plans_nsteps(app, dp->sel_disk) == 0)
		return;
	rc = plans_undo(app, dp->sel_disk, &what);
	left = plans_nsteps(app, dp->sel_disk);
	app_log(app, IL_LOG_INFO, "ui: disk %d: undo %s%s", dp->sel_disk,
		what ? what : "", rc ? " (the later changes went too)" : "");
	t = g_strdup_printf("Undone: %s.%s", what ? what : "the last change",
			    left == 0 ? " The disk is as it was." :
			    left == 1 ? " One more change can be undone." :
			    " Earlier changes can be undone too.");
	set_message(dp, rc ? NOTICE_WARNING : NOTICE_SUCCESS, t);
	g_free(t);
	g_free(what);
	dp->sel_kind = ROW_DISK;
	populate(p);
}

static void grid_pair(GtkWidget *grid, int row, const char *key, const char *value)
{
	GtkWidget *k = ui_label(key, "il-detail-key");
	GtkWidget *v = ui_label(value && *value ? value : "\xe2\x80\x94",
				"il-detail-value");

	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	gtk_widget_set_can_focus(v, FALSE);
	gtk_label_set_line_wrap(GTK_LABEL(k), FALSE);
	gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v, 1, row, 1, 1);
}

static void on_details(GtkButton *b, gpointer data)
{
	Page *p = data;
	DiskPage *dp = p->priv;
	App *app = p->app;
	const struct il_disk *d;
	struct il_disk v;
	GtkWidget *grid, *parts;
	Sheet *s;
	char *t, buf[64];
	int row = 0;

	(void)b;
	read_selection(dp);
	if (!app->disks || dp->sel_disk < 0)
		return;
	d = &app->disks->disks[dp->sel_disk];
	disk_view(app, dp->sel_disk, &v);
	t = g_strdup_printf("Disk %d \xc2\xb7 %s", dp->sel_disk,
			    d->model[0] ? d->model : d->name);
	s = sheet_new(app, t, ART_DISK);
	g_free(t);
	sheet_set_wide(s, TRUE);

	grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	ui_add_class(grid, "il-details");
	grid_pair(grid, row++, "Device", d->dev);
	grid_pair(grid, row++, "Model", d->model);
	grid_pair(grid, row++, "Serial number", d->serial);
	grid_pair(grid, row++, "Connection", transport_name(d->transport));
	t = app_size_text(d->size_bytes);
	g_snprintf(buf, sizeof(buf), "%s (%" G_GUINT64_FORMAT " bytes)", t,
		   d->size_bytes);
	g_free(t);
	grid_pair(grid, row++, "Size", buf);
	g_snprintf(buf, sizeof(buf), "%u bytes logical, %u bytes physical",
		   d->sector_size, d->phys_sector_size);
	grid_pair(grid, row++, "Sectors", buf);
	grid_pair(grid, row++, "Partition table", table_name(d->table));
	grid_pair(grid, row++, "Disk GUID", d->disk_guid);
	grid_pair(grid, row++, "Removable", d->removable ? "Yes" : "No");
	grid_pair(grid, row++, "Status", d->is_boot_medium ?
		  "Installation medium (in use)" : d->read_only ?
		  "Write-protected" : "Available");
	sheet_set_body(s, grid);

	if (v.nparts) {
		GtkWidget *h = ui_label(app->plans[dp->sel_disk] &&
					il_plan_nops(app->plans[dp->sel_disk]) ?
					"Partitions (with your changes)" :
					"Partitions", "il-section");

		sheet_set_body(s, h);
		parts = gtk_grid_new();
		gtk_grid_set_column_spacing(GTK_GRID(parts), 18);
		gtk_grid_set_row_spacing(GTK_GRID(parts), 4);
		ui_add_class(parts, "il-details");
		{
			static const char *const heads[] = {
				"#", "Start", "Size", "Type", "File system",
				"Label", "Partition GUID",
			};

			for (int c = 0; c < 7; c++)
				gtk_grid_attach(GTK_GRID(parts),
						ui_label(heads[c], "il-detail-key"),
						c, 0, 1, 1);
		}
		for (int i = 0; i < v.nparts; i++) {
			const struct il_partition *pt = &v.parts[i];
			char num[8], *start, *size;
			const char *fs = fs_name(pt->fstype);

			g_snprintf(num, sizeof(num), "%d", pt->number);
			start = app_size_text(pt->start_bytes);
			size = app_size_text(pt->size_bytes);
			gtk_grid_attach(GTK_GRID(parts), ui_label(num, "il-detail-value"),
					0, i + 1, 1, 1);
			gtk_grid_attach(GTK_GRID(parts),
					ui_label(start, "il-detail-value"), 1, i + 1,
					1, 1);
			gtk_grid_attach(GTK_GRID(parts), ui_label(size, "il-detail-value"),
					2, i + 1, 1, 1);
			gtk_grid_attach(GTK_GRID(parts),
					ui_label(part_kind(pt), "il-detail-value"), 3,
					i + 1, 1, 1);
			gtk_grid_attach(GTK_GRID(parts),
					ui_label(fs ? fs : "\xe2\x80\x94",
						 "il-detail-value"), 4, i + 1, 1, 1);
			gtk_grid_attach(GTK_GRID(parts),
					ui_label(pt->fs_label[0] ? pt->fs_label :
						 "\xe2\x80\x94", "il-detail-value"),
					5, i + 1, 1, 1);
			gtk_grid_attach(GTK_GRID(parts),
					ui_label(pt->part_guid[0] ? pt->part_guid :
						 "\xe2\x80\x94", "il-detail-mono"),
					6, i + 1, 1, 1);
			g_free(start);
			g_free(size);
		}
		{
			GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
						       GTK_POLICY_AUTOMATIC,
						       GTK_POLICY_NEVER);
			gtk_scrolled_window_set_propagate_natural_height(
				GTK_SCROLLED_WINDOW(sw), TRUE);
			gtk_container_add(GTK_CONTAINER(sw), parts);
			sheet_set_body(s, sw);
		}
	}
	sheet_add_button(s, "_Close", SHEET_OK, "primary");
	sheet_present(s, NULL, NULL);
}

/* ---- the page ------------------------------------------------------ */

static const struct {
	const char *label;
	IconKind icon;
	const char *tip;
	void (*cb)(GtkButton *, gpointer);
	const char *style;
} g_acts[N_ACTS] = {
	[ACT_REFRESH] = { "_Refresh", ICON_REFRESH,
			  "Look for disks again", on_refresh, "accent" },
	[ACT_DELETE] = { "_Delete", ICON_DELETE,
			 "Delete the selected partition", on_delete, "danger" },
	[ACT_FORMAT] = { "_Format", ICON_FORMAT,
			 "Give the selected partition a new file system",
			 on_format, NULL },
	[ACT_NEW] = { "Ne_w", ICON_NEW,
		      "Create a partition in the selected unallocated space",
		      on_new, "accent" },
	[ACT_EXTEND] = { "E_xtend", ICON_EXTEND,
			 "Grow the selected partition into the space after it",
			 on_extend, NULL },
	[ACT_WHOLE] = { "Use _entire disk", ICON_WHOLE_DISK,
			"Erase the disk and use all of it for LikeOS",
			on_whole, "accent" },
	[ACT_CONVERT] = { "Con_vert to GPT", ICON_PARTITION,
			  "Replace an MBR partition table with an empty GPT",
			  on_convert, NULL },
	[ACT_UNDO] = { "_Undo", ICON_UNDO,
		       "Take back the last change to this disk", on_undo, NULL },
	[ACT_DETAILS] = { "Disk deta_ils", ICON_DETAILS,
			  "Model, serial number, sectors and partitions",
			  on_details, NULL },
};

static void grid_actions(DiskPage *dp, int cols)
{
	for (int i = 0; i < N_ACTS; i++) {
		GtkWidget *a = dp->acts[i];

		g_object_ref(a);
		if (gtk_widget_get_parent(a))
			gtk_container_remove(GTK_CONTAINER(dp->grid), a);
		gtk_grid_attach(GTK_GRID(dp->grid), a, i % cols, i / cols, 1, 1);
		g_object_unref(a);
	}
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path,
			     GtkTreeViewColumn *col, gpointer data)
{
	Page *p = data;

	(void)tv;
	(void)path;
	(void)col;
	if (p->next_ok)
		wizard_next(pages_wizard());
}

static void type_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell,
			   GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
	gboolean sens;
	int weight;

	(void)col;
	(void)data;
	gtk_tree_model_get(m, it, COL_SENSITIVE, &sens, COL_WEIGHT, &weight, -1);
	g_object_set(cell, "sensitive", sens, "weight", weight, NULL);
}

static GtkTreeViewColumn *text_column(GtkWidget *tree, const char *title,
				      int col, gboolean expand, int min_width)
{
	GtkCellRenderer *r = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
		title, r, "text", col, NULL);

	g_object_set(r, "ellipsize", expand ? PANGO_ELLIPSIZE_END :
		     PANGO_ELLIPSIZE_NONE, "ypad", 5, NULL);
	gtk_tree_view_column_set_cell_data_func(c, r, type_cell_data, NULL, NULL);
	gtk_tree_view_column_set_expand(c, expand);
	gtk_tree_view_column_set_resizable(c, TRUE);
	if (min_width > 0)
		gtk_tree_view_column_set_min_width(c, min_width);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
	return c;
}

static GtkWidget *disk_build(Page *p)
{
	DiskPage *dp = p->priv;
	GtkWidget *body, *root, *row, *sw, *b;
	GtkTreeViewColumn *c;
	GtkCellRenderer *r;

	root = setup_page_new("Where do you want to install LikeOS?", &body);

	dp->grid = gtk_grid_new();
	gtk_grid_set_column_homogeneous(GTK_GRID(dp->grid), TRUE);
	gtk_grid_set_row_spacing(GTK_GRID(dp->grid), 2);
	gtk_grid_set_column_spacing(GTK_GRID(dp->grid), 8);
	ui_add_class(dp->grid, "il-toolbar");
	for (int i = 0; i < N_ACTS; i++) {
		dp->acts[i] = toolbar_action_new(g_acts[i].label, g_acts[i].icon,
						 g_acts[i].tip);
		toolbar_action_set_style(dp->acts[i], g_acts[i].style);
		g_signal_connect(dp->acts[i], "clicked", G_CALLBACK(g_acts[i].cb), p);
	}
	grid_actions(dp, 3);
	gtk_box_pack_start(GTK_BOX(body), dp->grid, FALSE, FALSE, 0);

	/* the inline size row */
	dp->size_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(dp->size_revealer),
					 GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	ui_add_class(row, "il-size-row");
	dp->size_label = gtk_label_new_with_mnemonic("_Size in MB:");
	dp->size_spin = gtk_spin_button_new_with_range(1, 1, 1);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(dp->size_spin), 0);
	gtk_spin_button_set_increments(GTK_SPIN_BUTTON(dp->size_spin), 1024, 10240);
	gtk_entry_set_activates_default(GTK_ENTRY(dp->size_spin), FALSE);
	gtk_entry_set_width_chars(GTK_ENTRY(dp->size_spin), 9);
	ui_add_class(dp->size_spin, "il-entry");
	gtk_label_set_mnemonic_widget(GTK_LABEL(dp->size_label), dp->size_spin);
	dp->size_apply = ui_button("_Apply", "il-primary");
	g_signal_connect(dp->size_apply, "clicked", G_CALLBACK(on_size_apply), p);
	g_signal_connect_swapped(dp->size_spin, "activate",
				 G_CALLBACK(gtk_button_clicked), dp->size_apply);
	b = ui_button("_Cancel", NULL);
	g_signal_connect(b, "clicked", G_CALLBACK(on_size_cancel), p);
	dp->size_range = ui_label("", "il-caption");
	gtk_label_set_line_wrap(GTK_LABEL(dp->size_range), FALSE);
	gtk_label_set_ellipsize(GTK_LABEL(dp->size_range), PANGO_ELLIPSIZE_END);
	gtk_widget_set_valign(dp->size_range, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(dp->size_label, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(dp->size_spin, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(dp->size_apply, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_box_pack_start(GTK_BOX(row), dp->size_label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), dp->size_spin, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), dp->size_apply, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), dp->size_range, TRUE, TRUE, 0);
	gtk_container_add(GTK_CONTAINER(dp->size_revealer), row);
	gtk_box_pack_start(GTK_BOX(body), dp->size_revealer, FALSE, FALSE, 0);

	/* the tree */
	dp->store = gtk_tree_store_new(N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING,
				       G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				       G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
				       G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_UINT64);
	dp->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(dp->store));
	g_object_unref(dp->store);
	ui_add_class(dp->tree, "il-disk-tree");
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(dp->tree), FALSE);
	gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(dp->tree), FALSE);
	gtk_tree_view_set_level_indentation(GTK_TREE_VIEW(dp->tree), 22);

	c = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(c, "Name");
	r = gtk_cell_renderer_pixbuf_new();
	g_object_set(r, "xpad", 4, NULL);
	gtk_tree_view_column_pack_start(c, r, FALSE);
	gtk_tree_view_column_add_attribute(c, r, "pixbuf", COL_ICON);
	r = gtk_cell_renderer_text_new();
	g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 5, NULL);
	gtk_tree_view_column_pack_start(c, r, TRUE);
	gtk_tree_view_column_add_attribute(c, r, "text", COL_NAME);
	gtk_tree_view_column_set_cell_data_func(c, r, type_cell_data, NULL, NULL);
	gtk_tree_view_column_set_expand(c, TRUE);
	gtk_tree_view_column_set_resizable(c, TRUE);
	gtk_tree_view_column_set_min_width(c, 200);
	gtk_tree_view_append_column(GTK_TREE_VIEW(dp->tree), c);
	text_column(dp->tree, "Total size", COL_TOTAL, FALSE, 90);
	dp->col_free = text_column(dp->tree, "Free space", COL_FREE, FALSE, 90);
	dp->col_type = text_column(dp->tree, "Type", COL_TYPE, FALSE, 150);

	gtk_tree_selection_set_select_function(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(dp->tree)), select_func,
		NULL, NULL);
	g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(dp->tree)),
			 "changed", G_CALLBACK(on_selection), p);
	g_signal_connect(dp->tree, "row-activated", G_CALLBACK(on_row_activated), p);

	sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 170);
	gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(sw), FALSE);
	gtk_container_add(GTK_CONTAINER(sw), dp->tree);
	ui_add_class(sw, "il-list-frame");
	gtk_box_pack_start(GTK_BOX(body), sw, TRUE, TRUE, 0);

	dp->notice = notice_new();
	gtk_box_pack_start(GTK_BOX(body), dp->notice, FALSE, FALSE, 0);
	dp->changes = ui_label("", "il-caption");
	gtk_box_pack_start(GTK_BOX(body), dp->changes, FALSE, FALSE, 0);
	return root;
}

static void disk_enter(Page *p, int direction)
{
	DiskPage *dp = p->priv;
	App *app = p->app;

	if (dp->sel_disk < 0 && app->target_disk >= 0) {
		dp->sel_disk = app->target_disk;
		remember_disk(dp, app);
	}
	/* The choice Next made is taken back: what the tree shows is what
	 * the person did with the tools, and Next chooses again. */
	plans_clear_roots(app);
	if (app->disk_message) {
		set_message(dp, NOTICE_WARNING, app->disk_message);
		g_clear_pointer(&app->disk_message, g_free);
	}
	populate(p);
	page_set_focus(p, dp->tree);
	if (app->disk_rescan) {
		app->disk_rescan = FALSE;
		dp->sel_disk = -1;
		do_refresh(p);
	}
	(void)direction;
}

static gboolean disk_next(Page *p)
{
	DiskPage *dp = p->priv;
	App *app = p->app;
	il_plan *plan;
	char why[IL_ERROR_MAX];
	int partno = -1, rc = 0, di, keep;
	const char *what;

	read_selection(dp);
	di = dp->sel_disk;
	plan = ensure_plan(app, di);
	if (!plan)
		return FALSE;
	/* what Next adds to the plan goes again when it does not work */
	keep = plans_nsteps(app, di);
	if (dp->sel_kind == ROW_DISK) {
		what = "the whole disk";
		rc = apply_step(p, PLAN_STEP_WHOLE, 0, 0, 0, NULL);
		if (rc == 0)
			partno = il_plan_root(ensure_plan(app, di));
	} else if (dp->sel_kind == ROW_FREE) {
		what = "a new partition in the unallocated space";
		rc = apply_step(p, PLAN_STEP_CREATE, 0, dp->sel_start, 0, &partno);
	} else {
		what = "an existing partition";
		partno = dp->sel_partno;
	}
	if (rc == 0)
		rc = il_plan_select_root(ensure_plan(app, di), partno);
	if (rc != 0) {
		show_error(dp, app, "LikeOS cannot be installed there.");
		plans_truncate(app, di, keep);
		populate(p);
		return FALSE;
	}
	if (il_plan_validate(ensure_plan(app, di), app->src_ok ? &app->src : NULL,
			     why, sizeof(why)) != 0) {
		char *w = app_sentence(why), *m = pages_escape(w);

		plans_truncate(app, di, keep);
		populate(p);
		notice_set(dp->notice, NOTICE_ERROR, m);
		app_log(app, IL_LOG_INFO, "ui: disk page shows: %s", w);
		g_free(m);
		g_free(w);
		return FALSE;
	}
	/* one disk gets LikeOS: no other plan keeps a choice */
	for (int i = 0; i < IL_MAX_DISKS; i++)
		if (i != di && app->plans[i] && il_plan_root(app->plans[i]) > 0)
			il_plan_clear_root(app->plans[i]);
	app->target_disk = di;
	app_log(app, IL_LOG_INFO, "ui: LikeOS goes on %s, partition %d (%s)",
		app->disks->disks[di].dev, partno, what);
	/* the selection follows the root partition, for a later Back */
	dp->sel_kind = ROW_PART;
	dp->sel_partno = partno;
	return TRUE;
}

/* The answers' disk: the whole of it, or a partition of it. */
static int disk_autofill(Page *p, const struct il_answers *a)
{
	DiskPage *dp = p->priv;
	App *app = p->app;
	const struct il_disk *d;
	int di;

	if (dp->scanning || bridge_busy(app->bridge))
		return AUTOFILL_WAIT;
	d = app->disks ? il_disks_find(app->disks, a->disk[0] ? a->disk :
				       "auto") : NULL;
	if (!d) {
		app_log(app, IL_LOG_WARN, "ui: autopilot: no disk \"%s\" to "
			"install on", a->disk[0] ? a->disk : "auto");
		return AUTOFILL_FAIL;
	}
	di = (int)(d - app->disks->disks);
	dp->sel_disk = di;
	remember_disk(dp, app);
	if (a->whole_disk) {
		/* as "Use entire disk" and its question answered Yes */
		dp->sel_kind = ROW_DISK;
		whole_confirmed(NULL, SHEET_OK, p);
	} else {
		dp->sel_kind = ROW_PART;
		dp->sel_partno = a->root_partno;
		populate(p);
	}
	return p->next_ok ? AUTOFILL_NEXT : AUTOFILL_FAIL;
}

static void disk_relayout(Page *p)
{
	DiskPage *dp = p->priv;
	App *app = p->app;

	grid_actions(dp, app->screen_w < 720 ? 2 : 3);
	gtk_tree_view_column_set_visible(dp->col_free, app->screen_w >= 720);
	gtk_tree_view_column_set_min_width(dp->col_type, app->narrow ? 120 : 170);
}

void page_disk_init(Page *p)
{
	DiskPage *dp = g_new0(DiskPage, 1);

	dp->sel_disk = -1;
	p->id = "disk";
	p->scene = SCENE_SETUP;
	p->build = disk_build;
	p->enter = disk_enter;
	p->go_next = disk_next;
	p->autofill = disk_autofill;
	p->relayout = disk_relayout;
	p->next_ok = FALSE;
	p->priv = dp;
}
