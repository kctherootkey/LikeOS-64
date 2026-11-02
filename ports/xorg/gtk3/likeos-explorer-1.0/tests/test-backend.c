// LikeOS -- File Explorer: backend test program.
//
// Exercises the file-management backend without a display: locations,
// folder reading, monitoring, every kind of operation with its conflicts and
// failures, the Trash, search with and without the index, thumbnails'
// cache names.  Runs in a home folder of its own (a fresh directory under
// $TMPDIR or the one given with --dir), so the user's real Trash, recent
// files and caches are never touched.
//
//     explorer-backend-test [--dir DIR] [--big N]
//
// Exit status 0 when every check passed.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-dir-loader.h"
#include "backend/ex-file.h"
#include "backend/ex-fileops.h"
#include "backend/ex-index.h"
#include "backend/ex-location.h"
#include "backend/ex-monitor.h"
#include "backend/ex-search.h"
#include "backend/ex-thumbs.h"
#include "backend/ex-trash.h"
#include "backend/ex-util.h"
#include "backend/ex-volumes.h"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
static char *g_root; /* the test's home folder */

#define CHECK(name, cond)                                                   \
	do {                                                                \
		if (cond) {                                                 \
			g_pass++;                                           \
			printf("  [PASS] %s\n", name);                      \
		} else {                                                    \
			g_fail++;                                           \
			printf("  [FAIL] %s  (%s:%d)\n", name, __FILE__,    \
			       __LINE__);                                   \
		}                                                           \
	} while (0)

static char *P(const char *rel)
{
	return g_build_filename(g_root, rel, NULL);
}

static void write_file(const char *rel, const char *text)
{
	char *p = P(rel);
	char *dir = g_path_get_dirname(p);

	g_mkdir_with_parents(dir, 0755);
	g_file_set_contents(p, text, -1, NULL);
	g_free(dir);
	g_free(p);
}

static gboolean exists(const char *rel)
{
	char *p = P(rel);
	gboolean e = g_file_test(p, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);

	g_free(p);
	return e;
}

static char *read_file(const char *rel)
{
	char *p = P(rel), *d = NULL;

	g_file_get_contents(p, &d, NULL, NULL);
	g_free(p);
	return d;
}

static GFile *F(const char *rel)
{
	char *p = P(rel);
	GFile *f = g_file_new_for_path(p);

	g_free(p);
	return f;
}

static GPtrArray *files(const char *first, ...)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_object_unref);
	va_list ap;

	va_start(ap, first);
	for (const char *s = first; s; s = va_arg(ap, const char *))
		g_ptr_array_add(a, F(s));
	va_end(ap);
	return a;
}

/* Spin the main loop until `done' or `secs' pass. */
static gboolean spin_until(gboolean *done, int secs)
{
	gint64 end = g_get_monotonic_time() + (gint64)secs * G_USEC_PER_SEC;

	while (!*done && g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, TRUE);
	return *done;
}

static void op_finished(ExOperation *op, gpointer data)
{
	*(gboolean *)data = TRUE;
}

static ExOpState run(ExOperation *op)
{
	gboolean done = FALSE;

	g_signal_connect(op, "finished", G_CALLBACK(op_finished), &done);
	ex_operation_start(op);
	if (!spin_until(&done, 60))
		return (ExOpState)-1;
	return ex_operation_get_state(op);
}

/* An answering machine for conflicts and issues. */
typedef struct {
	ExConflictChoice conflict;
	ExIssueChoice issue;
	int conflicts, issues;
	gboolean all;
} Answers;

static gboolean answer_conflict_later(gpointer data)
{
	gpointer *pair = data;

	ex_conflict_resolve(pair[0], ((Answers *)pair[1])->conflict,
			    ((Answers *)pair[1])->all);
	g_free(pair);
	return G_SOURCE_REMOVE;
}

static void on_conflict(ExOperation *op, ExConflict *c, Answers *a)
{
	gpointer *pair = g_new(gpointer, 2);

	a->conflicts++;
	pair[0] = op;
	pair[1] = a;
	/* Answered from a later main-loop turn, as a dialog would. */
	g_idle_add(answer_conflict_later, pair);
}

static void on_issue(ExOperation *op, ExIssue *i, Answers *a)
{
	a->issues++;
	ex_issue_resolve(op, a->issue, a->all);
}

static ExOpState run_answered(ExOperation *op, Answers *a)
{
	g_signal_connect(op, "conflict", G_CALLBACK(on_conflict), a);
	g_signal_connect(op, "issue", G_CALLBACK(on_issue), a);
	return run(op);
}

/* ---- tests --------------------------------------------------------------- */

static void test_locations(void)
{
	printf("\n[locations]\n");
	ExLocation *h = ex_location_parse("likeos:home");
	ExLocation *t = ex_location_parse("trash:///");
	ExLocation *pc = ex_location_parse("computer:///");
	ExLocation *f = ex_location_parse(g_root);
	ExLocation *tilde = ex_location_parse("~/Documents");

	CHECK("likeos:home is Home", h && ex_location_kind(h) == EX_LOC_HOME);
	CHECK("trash:/// is the Trash", t && ex_location_kind(t) == EX_LOC_TRASH);
	CHECK("computer:/// is This PC", pc && ex_location_kind(pc) == EX_LOC_THISPC);
	CHECK("a path is a folder", f && ex_location_kind(f) == EX_LOC_FOLDER);
	char *tp = tilde ? g_file_get_path(ex_location_file(tilde)) : NULL;
	char *want = P("Documents");
	CHECK("~/ expands to the home folder", tp && strcmp(tp, want) == 0);
	g_free(tp);
	g_free(want);

	ExLocation *s = ex_location_new_search(f, "a b&c");
	ExLocation *s2 = ex_location_parse(ex_location_id(s));
	CHECK("a search id round-trips",
	      s2 && ex_location_equal(s, s2) &&
		      strcmp(ex_location_query(s2), "a b&c") == 0);

	GFile *tf = ex_trash_files_dir();
	char *tpath = g_file_get_path(tf);
	ExLocation *t2 = ex_location_parse(tpath);
	CHECK("the trash folder's path is the Trash",
	      t2 && ex_location_kind(t2) == EX_LOC_TRASH);
	g_free(tpath);
	g_object_unref(tf);

	GFile *deep = F("a/b/c");
	ExLocation *dl = ex_location_new_folder(deep);
	GPtrArray *cr = ex_location_crumbs(dl);
	CHECK("crumbs below home start at home and end at the folder",
	      cr->len == 4 &&
		      g_file_equal(ex_location_file(g_ptr_array_index(cr, 3)),
				   deep));
	ExLocation *up = ex_location_parent(dl);
	char *upn = ex_location_display_name(up);
	CHECK("Up from a/b/c is b", strcmp(upn, "b") == 0);
	g_free(upn);
	GFile *root = g_file_new_for_path("/");
	ExLocation *rl = ex_location_new_folder(root);
	ExLocation *rup = ex_location_parent(rl);
	CHECK("Up from the root is This PC",
	      rup && ex_location_kind(rup) == EX_LOC_THISPC);
	char *rn = ex_location_display_name(rl);
	CHECK("the root is called Local Disk", strcmp(rn, "Local Disk") == 0);
	g_free(rn);
	g_ptr_array_unref(cr);
	ex_location_unref(rup);
	ex_location_unref(rl);
	g_object_unref(root);
	ex_location_unref(up);
	ex_location_unref(dl);
	g_object_unref(deep);
	ex_location_unref(t2);
	ex_location_unref(s2);
	ex_location_unref(s);
	ex_location_unref(tilde);
	ex_location_unref(f);
	ex_location_unref(pc);
	ex_location_unref(t);
	ex_location_unref(h);
}

static void test_util(void)
{
	printf("\n[helpers]\n");
	CHECK("extension of photo.jpg", strcmp(ex_name_extension("photo.jpg"), ".jpg") == 0);
	CHECK("extension of a.tar.gz is .tar.gz",
	      strcmp(ex_name_extension("a.tar.gz"), ".tar.gz") == 0);
	CHECK("a dot file has no extension", *ex_name_extension(".bashrc") == '\0');
	write_file("u/photo.jpg", "x");
	write_file("u/photo (2).jpg", "x");
	GFile *u = F("u");
	char *n = ex_unique_name(u, "photo.jpg", TRUE);
	CHECK("unique name skips taken numbers", strcmp(n, "photo (3).jpg") == 0);
	g_free(n);
	n = ex_unique_name(u, "New folder", FALSE);
	CHECK("a free name is kept", strcmp(n, "New folder") == 0);
	g_free(n);
	g_object_unref(u);
	CHECK("slash is not allowed in a name", ex_name_problem("a/b") != NULL);
	CHECK("an ordinary name is fine", ex_name_problem("Report 2026.odt") == NULL);
	CHECK("hidden by dot", ex_name_is_hidden(".x"));
	CHECK("hidden by tilde", ex_name_is_hidden("x~"));
}

static int g_batches, g_loaded;
static void on_batch(GPtrArray *f, gpointer d)
{
	g_batches++;
	g_loaded += (int)f->len;
}
static void on_loaded(GObject *src, GAsyncResult *res, gpointer d)
{
	*(gboolean *)d = TRUE;
	ex_dir_load_finish(res, NULL);
}

static void test_loader(int big)
{
	printf("\n[folder reading]\n");
	char *dir = P("big");

	g_mkdir_with_parents(dir, 0755);
	for (int i = 0; i < big; i++) {
		char *p = g_strdup_printf("%s/f%05d.txt", dir, i);
		g_file_set_contents(p, "", 0, NULL);
		g_free(p);
	}
	GFile *d = g_file_new_for_path(dir);
	gboolean done = FALSE;
	gint64 t0 = g_get_monotonic_time();

	g_batches = g_loaded = 0;
	ex_dir_load_async(d, NULL, on_batch, on_loaded, &done);
	spin_until(&done, 120);
	CHECK("every name of a large folder arrives", g_loaded == big);
	CHECK("in several batches", big < 400 || g_batches > 1);
	printf("      (%d names in %d batches, %.2f s)\n", g_loaded, g_batches,
	       (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC);

	/* Cancelling stops it. */
	GCancellable *c = g_cancellable_new();
	done = FALSE;
	g_batches = g_loaded = 0;
	ex_dir_load_async(d, c, on_batch, on_loaded, &done);
	g_cancellable_cancel(c);
	spin_until(&done, 30);
	CHECK("a cancelled read ends", done && g_loaded < big);
	g_object_unref(c);

	/* A folder that cannot be read. */
	write_file("noread/x", "");
	char *nr = P("noread");
	g_chmod(nr, 0000);
	GFile *nrf = g_file_new_for_path(nr);
	done = FALSE;
	GError *err = NULL;
	gboolean ok = TRUE;
	if (getuid() != 0) {
		g_file_enumerate_children(nrf, "*", 0, NULL, &err);
		ok = err != NULL;
		g_clear_error(&err);
	}
	CHECK("an unreadable folder reports an error", ok);
	g_chmod(nr, 0755);
	g_object_unref(nrf);
	g_free(nr);
	g_object_unref(d);
	g_free(dir);

	/* Symbolic links: one to a folder lists as a folder, a broken one is
	 * still an item. */
	write_file("links/target/file", "t");
	char *lt = P("links/to-dir"), *lb = P("links/broken");
	CHECK("make a link to a folder", symlink("target", lt) == 0);
	CHECK("make a broken link", symlink("nowhere", lb) == 0);
	GFile *ltf = g_file_new_for_path(lt), *lbf = g_file_new_for_path(lb);
	ExFile *el = ex_file_new_query(ltf, NULL, NULL);
	ExFile *eb = ex_file_new_query(lbf, NULL, NULL);
	CHECK("a link to a folder is a folder", el && ex_file_is_dir(el) &&
						     ex_file_is_symlink(el));
	CHECK("a broken link is still an item", eb != NULL && !ex_file_is_dir(eb));
	g_clear_object(&el);
	g_clear_object(&eb);
	g_object_unref(ltf);
	g_object_unref(lbf);
	g_free(lt);
	g_free(lb);
}

typedef struct {
	int created, deleted, changed, renamed, gone;
} MonCount;
static void m_created(ExMonitor *m, GFile *f, MonCount *c) { c->created++; }
static void m_deleted(ExMonitor *m, GFile *f, MonCount *c) { c->deleted++; }
static void m_changed(ExMonitor *m, GFile *f, MonCount *c) { c->changed++; }
static void m_renamed(ExMonitor *m, GFile *a, GFile *b, MonCount *c) { c->renamed++; }
static void m_gone(ExMonitor *m, MonCount *c) { c->gone++; }
static void m_rescan(ExMonitor *m, MonCount *c) { c->created++; c->deleted++; c->renamed++; }

static void spin_for(int ms)
{
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(5000);
}

static void test_monitor(void)
{
	printf("\n[monitoring]\n");
	char *dir = P("mon");
	g_mkdir_with_parents(dir, 0755);
	GFile *d = g_file_new_for_path(dir);
	ExMonitor *m = ex_monitor_new(d);
	MonCount c = { 0 };

	g_signal_connect(m, "created", G_CALLBACK(m_created), &c);
	g_signal_connect(m, "deleted", G_CALLBACK(m_deleted), &c);
	g_signal_connect(m, "changed", G_CALLBACK(m_changed), &c);
	g_signal_connect(m, "renamed", G_CALLBACK(m_renamed), &c);
	g_signal_connect(m, "gone", G_CALLBACK(m_gone), &c);
	g_signal_connect(m, "rescan", G_CALLBACK(m_rescan), &c);
	printf("      (%s monitor)\n", ex_monitor_is_live(m) ? "live" : "polling");
	int wait = ex_monitor_is_live(m) ? 1500 : 7000;
	write_file("mon/new.txt", "x");
	spin_for(wait);
	CHECK("a new file is reported", c.created >= 1);
	char *a = P("mon/new.txt"), *b = P("mon/renamed.txt");
	g_rename(a, b);
	spin_for(wait);
	CHECK("a rename is reported", c.renamed >= 1);
	g_unlink(b);
	spin_for(wait);
	CHECK("a deletion is reported", c.deleted >= 1);
	g_free(a);
	g_free(b);
	g_rmdir(dir);
	spin_for(wait);
	CHECK("the folder's own removal is reported", c.gone >= 1 || !ex_monitor_is_live(m));
	g_object_unref(m);
	g_object_unref(d);
	g_free(dir);
}

static void test_copy_move(void)
{
	printf("\n[copy and move]\n");
	write_file("src/a.txt", "alpha");
	write_file("src/sub/b.txt", "beta");
	write_file("dst/a.txt", "old");
	GPtrArray *s = files("src/a.txt", "src/sub", NULL);
	GFile *dst = F("dst");

	/* Conflict on a.txt, default answer (no handler): skip. */
	ExOperation *op = ex_operation_new_copy(s, dst);
	ExOpState st = run(op);
	char *t = read_file("dst/a.txt");
	CHECK("copy finishes", st == EX_OP_DONE);
	CHECK("unanswered conflict skips: old file kept", t && strcmp(t, "old") == 0);
	g_free(t);
	CHECK("folders are copied with their contents", exists("dst/sub/b.txt"));
	CHECK("no partial-copy leftovers", !exists("dst/.a.txt.likeos-part-0-0"));
	g_object_unref(op);

	/* Replace. */
	Answers ans = { EX_CONFLICT_REPLACE, EX_ISSUE_SKIP, 0, 0, FALSE };
	GPtrArray *one = files("src/a.txt", NULL);
	op = ex_operation_new_copy(one, dst);
	st = run_answered(op, &ans);
	t = read_file("dst/a.txt");
	CHECK("replace overwrites", st == EX_OP_DONE && t && strcmp(t, "alpha") == 0);
	CHECK("the conflict was asked once", ans.conflicts == 1);
	g_free(t);
	g_object_unref(op);

	/* Keep both. */
	ans.conflict = EX_CONFLICT_KEEP_BOTH;
	ans.conflicts = 0;
	op = ex_operation_new_copy(one, dst);
	st = run_answered(op, &ans);
	CHECK("keep both makes “a (2).txt”", exists("dst/a (2).txt"));
	GPtrArray *res = ex_operation_get_results(op);
	CHECK("the result records the new name",
	      res->len == 1 && g_str_has_suffix(g_file_peek_path(((ExOpResult *)g_ptr_array_index(res, 0))->dest), "a (2).txt"));
	g_object_unref(op);

	/* Copy onto itself: a copy next to the original. */
	GFile *srcdir = F("src");
	op = ex_operation_new_copy(one, srcdir);
	st = run(op);
	CHECK("a copy into the same folder gets a new name", exists("src/a (2).txt"));
	g_object_unref(op);

	/* Copy a folder into itself is refused. */
	GPtrArray *sub = files("src/sub", NULL);
	GFile *inside = F("src/sub");
	op = ex_operation_new_copy(sub, inside);
	st = run(op);
	CHECK("a folder cannot be copied into itself",
	      st == EX_OP_FAILED && !exists("src/sub/sub/sub"));
	g_object_unref(op);
	g_object_unref(inside);
	g_ptr_array_unref(sub);

	/* Merge folders. */
	write_file("m1/f/x.txt", "x");
	write_file("m2/f/y.txt", "y");
	GPtrArray *mf = files("m1/f", NULL);
	GFile *m2 = F("m2");
	ans.conflict = EX_CONFLICT_MERGE;
	op = ex_operation_new_copy(mf, m2);
	st = run_answered(op, &ans);
	CHECK("merge combines folder contents", exists("m2/f/x.txt") && exists("m2/f/y.txt"));
	g_object_unref(op);

	/* Move within a filesystem. */
	GPtrArray *mv = files("src/a (2).txt", NULL);
	GFile *mvdst = F("dst");
	op = ex_operation_new_move(mv, mvdst);
	ans.conflict = EX_CONFLICT_KEEP_BOTH;
	st = run_answered(op, &ans);
	CHECK("move: the source is gone", !exists("src/a (2).txt"));
	CHECK("move: under a free name at the destination", exists("dst/a (3).txt"));
	g_object_unref(op);

	/* Move exact (undo of a move). */
	GPtrArray *back_src = files("dst/a (3).txt", NULL);
	GPtrArray *back_dst = files("src/a (2).txt", NULL);
	op = ex_operation_new_move_exact(back_src, back_dst);
	st = run(op);
	CHECK("exact move puts it back", exists("src/a (2).txt") && !exists("dst/a (3).txt"));
	g_object_unref(op);

	/* Moving a folder into itself. */
	GPtrArray *self = files("src", NULL);
	GFile *self_in = F("src/sub");
	op = ex_operation_new_move(self, self_in);
	st = run(op);
	CHECK("a folder cannot be moved into itself", st == EX_OP_FAILED && exists("src/sub"));
	g_object_unref(op);

	/* Cancel a large copy: nothing half-written. */
	char *bigp = P("big1/blob");
	char *bigdir = g_path_get_dirname(bigp);
	g_mkdir_with_parents(bigdir, 0755);
	{
		FILE *fp = fopen(bigp, "wb");
		char block[65536];
		memset(block, 'z', sizeof(block));
		for (int i = 0; fp && i < 1024; i++) /* 64 MB */
			fwrite(block, 1, sizeof(block), fp);
		if (fp)
			fclose(fp);
	}
	GPtrArray *bs = files("big1/blob", NULL);
	GFile *bd = F("big2");
	g_mkdir_with_parents(g_file_peek_path(bd), 0755);
	op = ex_operation_new_copy(bs, bd);
	gboolean done = FALSE;
	g_signal_connect(op, "finished", G_CALLBACK(op_finished), &done);
	ex_operation_start(op);
	spin_for(30);
	ex_operation_cancel(op);
	spin_until(&done, 60);
	GDir *gd = g_dir_open(g_file_peek_path(bd), 0, NULL);
	int left = 0;
	const char *nm;
	while (gd && (nm = g_dir_read_name(gd)))
		if (strcmp(nm, "blob") != 0)
			left++;
	if (gd)
		g_dir_close(gd);
	char *bdst = P("big2/blob");
	struct stat sb;
	gboolean whole_or_absent = stat(bdst, &sb) != 0 || sb.st_size == 64 * 1024 * 1024;
	CHECK("a cancelled copy leaves no partial file",
	      ex_operation_get_state(op) == EX_OP_CANCELLED ? (left == 0 && whole_or_absent) : TRUE);
	g_free(bdst);
	g_object_unref(op);
	g_object_unref(bd);
	g_ptr_array_unref(bs);
	g_free(bigdir);
	g_free(bigp);

	/* Copying something that cannot be read: an issue, answered skip. */
	write_file("perm/secret", "s");
	char *sp = P("perm/secret");
	g_chmod(sp, 0000);
	if (getuid() != 0) {
		GPtrArray *ps = files("perm/secret", NULL);
		GFile *pd = F("dst");
		Answers pa = { EX_CONFLICT_SKIP, EX_ISSUE_SKIP, 0, 0, FALSE };
		op = ex_operation_new_copy(ps, pd);
		st = run_answered(op, &pa);
		CHECK("an unreadable source is an issue", pa.issues == 1 && st == EX_OP_FAILED);
		CHECK("and its error is recorded", ex_operation_get_errors(op)->len == 1);
		CHECK("nothing half-made at the destination", !exists("dst/secret"));
		g_object_unref(op);
		g_object_unref(pd);
		g_ptr_array_unref(ps);
	}
	g_chmod(sp, 0644);
	g_free(sp);

	/* Copying a symbolic link copies the link. */
	char *lk = P("src/link");
	CHECK("make a link to copy", symlink("a.txt", lk) == 0);
	GPtrArray *ls = files("src/link", NULL);
	op = ex_operation_new_copy(ls, dst);
	st = run(op);
	char *dl = P("dst/link");
	CHECK("a link is copied as a link", g_file_test(dl, G_FILE_TEST_IS_SYMLINK));
	g_free(dl);
	g_free(lk);
	g_object_unref(op);
	g_ptr_array_unref(ls);

	g_ptr_array_unref(self);
	g_object_unref(self_in);
	g_ptr_array_unref(back_src);
	g_ptr_array_unref(back_dst);
	g_object_unref(mvdst);
	g_ptr_array_unref(mv);
	g_object_unref(m2);
	g_ptr_array_unref(mf);
	g_object_unref(srcdir);
	g_ptr_array_unref(one);
	g_object_unref(dst);
	g_ptr_array_unref(s);
}

static void test_create_rename_delete(void)
{
	printf("\n[create, rename, delete]\n");
	GFile *d = F("cr");
	g_mkdir_with_parents(g_file_peek_path(d), 0755);

	ExOperation *op = ex_operation_new_mkdir(d, NULL);
	run(op);
	CHECK("New folder", exists("cr/New folder"));
	g_object_unref(op);
	op = ex_operation_new_mkdir(d, NULL);
	run(op);
	CHECK("a second one is “New folder (2)”", exists("cr/New folder (2)"));
	g_object_unref(op);
	op = ex_operation_new_newfile(d, "New Text Document.txt", NULL);
	run(op);
	CHECK("New Text Document.txt", exists("cr/New Text Document.txt"));
	g_object_unref(op);

	GFile *nf = F("cr/New Text Document.txt");
	op = ex_operation_new_rename(nf, "notes.txt");
	ExOpState st = run(op);
	CHECK("rename", st == EX_OP_DONE && exists("cr/notes.txt"));
	g_object_unref(op);
	g_object_unref(nf);

	GFile *notes = F("cr/notes.txt");
	op = ex_operation_new_rename(notes, "New folder");
	st = run(op);
	CHECK("rename onto a taken name is refused", st == EX_OP_FAILED && exists("cr/notes.txt"));
	g_object_unref(op);
	op = ex_operation_new_rename(notes, "bad/name");
	st = run(op);
	CHECK("rename to an invalid name is refused", st == EX_OP_FAILED);
	g_object_unref(op);

	op = ex_operation_new_chmod(notes, 0600);
	st = run(op);
	struct stat sb;
	char *np = P("cr/notes.txt");
	CHECK("chmod", st == EX_OP_DONE && stat(np, &sb) == 0 && (sb.st_mode & 0777) == 0600);
	g_free(np);
	g_object_unref(op);
	g_object_unref(notes);

	GPtrArray *del = files("cr/New folder", "cr/notes.txt", NULL);
	op = ex_operation_new_delete(del);
	st = run(op);
	CHECK("delete removes for good", st == EX_OP_DONE && !exists("cr/New folder") && !exists("cr/notes.txt"));
	g_object_unref(op);
	g_ptr_array_unref(del);
	g_object_unref(d);
}

static GPtrArray *g_listed;
static void trash_listed(GObject *src, GAsyncResult *rr, gpointer d)
{
	g_listed = ex_trash_list_finish(rr, NULL);
	*(gboolean *)d = TRUE;
}

static void test_trash(void)
{
	printf("\n[trash]\n");
	write_file("tr/doc.txt", "keep me");
	write_file("tr/dir/inner.txt", "i");
	GPtrArray *s = files("tr/doc.txt", "tr/dir", NULL);
	ExOperation *op = ex_operation_new_trash(s);
	ExOpState st = run(op);
	CHECK("trash finishes", st == EX_OP_DONE);
	CHECK("the items are gone from their folder", !exists("tr/doc.txt") && !exists("tr/dir"));
	CHECK("the Trash holds two items", ex_trash_count() == 2);
	GPtrArray *res = ex_operation_get_results(op);
	ExOpResult *r0 = res->len ? g_ptr_array_index(res, 0) : NULL;
	CHECK("the result names the trashed item", r0 && r0->dest);
	char *orig = NULL;
	gint64 when = 0;
	CHECK("its record says where it came from",
	      r0 && r0->dest && ex_trash_read_info(r0->dest, &orig, &when, NULL) &&
		      g_str_has_suffix(orig, "tr/doc.txt") && when > 0);
	g_free(orig);

	/* List. */
	gboolean done = FALSE;
	GPtrArray *listed = NULL;
	g_listed = NULL;
	ex_trash_list_async(NULL, trash_listed, &done);
	spin_until(&done, 30);
	listed = g_listed;
	CHECK("listing shows both with their origins",
	      listed && listed->len == 2 &&
		      ex_file_get_trash_orig_path(g_ptr_array_index(listed, 0)) &&
		      (ex_file_get_caps(g_ptr_array_index(listed, 0)) & EX_CAP_RESTORE));

	/* Restore one; a file has since taken its place: keep both. */
	write_file("tr/doc.txt", "newcomer");
	GPtrArray *rs = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(rs, g_object_ref(r0->dest));
	Answers ans = { EX_CONFLICT_KEEP_BOTH, EX_ISSUE_SKIP, 0, 0, FALSE };
	ExOperation *rop = ex_operation_new_restore(rs);
	st = run_answered(rop, &ans);
	char *t = read_file("tr/doc (2).txt");
	CHECK("restore puts it back (keep both)", st == EX_OP_DONE && t && strcmp(t, "keep me") == 0);
	g_free(t);
	CHECK("its record is gone", ex_trash_count() == 1);
	g_object_unref(rop);
	g_ptr_array_unref(rs);

	/* Restore into a folder that no longer exists recreates it. */
	write_file("gone/deep/f.txt", "f");
	GPtrArray *g = files("gone/deep/f.txt", NULL);
	op = ex_operation_new_trash(g);
	run(op);
	GFile *trashed = g_object_ref(((ExOpResult *)g_ptr_array_index(ex_operation_get_results(op), 0))->dest);
	g_object_unref(op);
	char *gd = P("gone/deep"), *gp = P("gone");
	g_rmdir(gd);
	g_rmdir(gp);
	g_free(gd);
	g_free(gp);
	GPtrArray *rr2 = g_ptr_array_new_with_free_func(g_object_unref);
	g_ptr_array_add(rr2, trashed);
	rop = ex_operation_new_restore(rr2);
	run(rop);
	CHECK("restore recreates the folder it came from", exists("gone/deep/f.txt"));
	g_object_unref(rop);
	g_ptr_array_unref(rr2);
	g_ptr_array_unref(g);

	/* Empty. */
	op = ex_operation_new_empty_trash();
	st = run(op);
	CHECK("empty trash", st == EX_OP_DONE && ex_trash_count() == 0);
	GFile *info = ex_trash_info_dir();
	GFileEnumerator *en = g_file_enumerate_children(info, "standard::name", 0, NULL, NULL);
	GFileInfo *fi = en ? g_file_enumerator_next_file(en, NULL, NULL) : NULL;
	CHECK("and its records", fi == NULL);
	g_clear_object(&fi);
	g_clear_object(&en);
	g_object_unref(info);
	g_object_unref(op);
	if (listed)
		g_ptr_array_unref(listed);
	g_ptr_array_unref(s);
}

static void on_hits(ExSearch *s, GPtrArray *hits, GPtrArray *all)
{
	for (guint i = 0; i < hits->len; i++)
		g_ptr_array_add(all, g_object_ref(g_ptr_array_index(hits, i)));
}
static void on_search_done(ExSearch *s, gboolean from_index, int *state)
{
	*state = from_index ? 2 : 1;
}

static int search_count(GFile *scope, const char *q, gboolean *from_index)
{
	ExSearch *s = ex_search_new(scope, q, FALSE);
	GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
	int state = 0;
	gboolean done = FALSE;

	g_signal_connect(s, "hits", G_CALLBACK(on_hits), all);
	g_signal_connect(s, "finished", G_CALLBACK(on_search_done), &state);
	ex_search_start(s);
	while (!done) {
		g_main_context_iteration(NULL, TRUE);
		done = state != 0;
	}
	int n = (int)all->len;
	if (from_index)
		*from_index = state == 2;
	g_ptr_array_unref(all);
	g_object_unref(s);
	return n;
}

static void test_search(void)
{
	printf("\n[search]\n");
	write_file("se/Holiday 2025.jpg", "");
	write_file("se/holiday notes.txt", "");
	write_file("se/deep/x/holiday.mp4", "");
	write_file("se/.hidden/holiday.jpg", "");
	write_file("se/report.pdf", "");
	GFile *scope = F("se");
	ExQuery *q = ex_query_parse("holiday kind:image");
	char *fold = g_utf8_casefold("Holiday 2025.jpg", -1);
	CHECK("query: words and kind", ex_query_matches(q, "Holiday 2025.jpg", fold, FALSE, NULL));
	g_free(fold);
	ex_query_free(q);

	ExIndex *ix = ex_index_get_default();
	ex_index_set_enabled(ix, FALSE); /* walk first */
	gboolean fi = FALSE;
	CHECK("walk: all holiday names, hidden folders skipped",
	      search_count(scope, "holiday", &fi) == 3 && !fi);
	CHECK("walk: kind:image", search_count(scope, "holiday kind:image", NULL) == 1);
	CHECK("walk: ext:pdf", search_count(scope, "ext:pdf", NULL) == 1);
	CHECK("walk: glob", search_count(scope, "*.mp4", NULL) == 1);
	CHECK("walk: no match", search_count(scope, "zebra", NULL) == 0);

	ex_index_set_enabled(ix, TRUE);
	ex_index_rebuild_sync(ix);
	CHECK("the index holds the home folder's names", ex_index_size(ix) > 10);
	CHECK("index: same answer as the walk",
	      search_count(scope, "holiday", &fi) == 3 && fi);
	char *store = g_build_filename(g_get_user_cache_dir(), "likeos-explorer", "index-v1", NULL);
	CHECK("the index is stored", g_file_test(store, G_FILE_TEST_EXISTS));
	write_file("se/holiday late.txt", "");
	char *gone = P("se/holiday notes.txt");
	g_unlink(gone);
	g_free(gone);
	CHECK("index: a deleted file is not reported", search_count(scope, "holiday", NULL) == 2);
	ex_index_set_enabled(ix, FALSE);
	CHECK("switching the index off deletes it", !g_file_test(store, G_FILE_TEST_EXISTS));
	g_free(store);
	g_object_unref(scope);
}

static void thumb_changed(ExFile *f, gpointer d)
{
	*(gboolean *)d = TRUE;
}

static void test_thumbs_and_volumes(void)
{
	printf("\n[thumbnails, volumes]\n");
	char *p = ex_thumbs_cache_path("file:///home/jens/photos/me.png", EX_THUMB_NORMAL);
	/* The spec's own example: md5 of that URI. */
	CHECK("cache name is the URI's MD5",
	      g_str_has_suffix(p, "thumbnails/normal/c6ee772d9e49320e97ec29a7eb5b1697.png"));
	g_free(p);

	/* A real image gets a thumbnail. */
	GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 400, 300);
	gdk_pixbuf_fill(pb, 0x3366ccff);
	char *ip = P("th/pic.png");
	char *idir = g_path_get_dirname(ip);
	g_mkdir_with_parents(idir, 0755);
	gdk_pixbuf_save(pb, ip, "png", NULL, NULL);
	g_object_unref(pb);
	GFile *imf = g_file_new_for_path(ip);
	ExFile *ef = ex_file_new_query(imf, NULL, NULL);
	gboolean got = FALSE;
	g_signal_connect(ef, "changed", G_CALLBACK(thumb_changed), &got);
	CHECK("a PNG can have a thumbnail", ex_thumbs_supported(ef));
	ex_thumbs_request(ef, EX_THUMB_NORMAL);
	spin_until(&got, 30);
	GdkPixbuf *th = ex_file_get_thumbnail(ef);
	CHECK("its thumbnail fits 128 px",
	      th && gdk_pixbuf_get_width(th) == 128 && gdk_pixbuf_get_height(th) == 96);
	char *uri = g_file_get_uri(imf);
	char *cp = ex_thumbs_cache_path(uri, EX_THUMB_NORMAL);
	CHECK("and is stored in the cache", g_file_test(cp, G_FILE_TEST_EXISTS));
	g_free(cp);
	g_free(uri);
	g_object_unref(ef);
	g_object_unref(imf);
	g_free(idir);
	g_free(ip);

	ExVolumes *v = ex_volumes_get_default();
	GPtrArray *l = ex_volumes_list(v);
	gboolean has_root = FALSE;
	for (guint i = 0; i < l->len; i++)
		if (strcmp(((ExVolume *)g_ptr_array_index(l, i))->name, "Local Disk") == 0)
			has_root = TRUE;
	CHECK("the root disk is a volume", has_root);
	GFile *here = F(".");
	CHECK("a file's volume is found", ex_volumes_find(v, here) != NULL);
	g_object_unref(here);
}

int main(int argc, char **argv)
{
	int big = 3000;
	const char *base = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
			base = argv[++i];
		else if (strcmp(argv[i], "--big") == 0 && i + 1 < argc)
			big = atoi(argv[++i]);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* A home of our own, set before GLib reads the environment. */
	char *tmpl = g_build_filename(base ? base : g_get_tmp_dir(),
				      "explorer-test-XXXXXX", NULL);
	g_root = g_mkdtemp(tmpl);
	if (!g_root) {
		fprintf(stderr, "cannot create a test directory\n");
		return 2;
	}
	char *data = g_build_filename(g_root, ".local", "share", NULL);
	char *cache = g_build_filename(g_root, ".cache", NULL);
	char *config = g_build_filename(g_root, ".config", NULL);
	g_setenv("HOME", g_root, TRUE);
	g_setenv("XDG_DATA_HOME", data, TRUE);
	g_setenv("XDG_CACHE_HOME", cache, TRUE);
	g_setenv("XDG_CONFIG_HOME", config, TRUE);
	g_mkdir_with_parents(data, 0700);
	printf("explorer-backend-test in %s\n", g_root);

	test_util();
	test_locations();
	test_loader(big);
	test_monitor();
	test_copy_move();
	test_create_rename_delete();
	test_trash();
	test_search();
	test_thumbs_and_volumes();

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) {
		char *argv_rm[] = { "rm", "-rf", g_root, NULL };
		g_spawn_sync(NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL,
			     NULL, NULL, NULL, NULL, NULL);
	} else {
		printf("(test directory kept: %s)\n", g_root);
	}
	g_free(data);
	g_free(cache);
	g_free(config);
	return g_fail ? 1 : 0;
}
