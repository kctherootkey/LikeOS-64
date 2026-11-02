// LikeOS -- File Explorer: which application opens what.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-apps.h"

#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Explorer's own entry: offered for nothing, since opening a folder "with
 * the file manager" from inside the file manager is just navigating. */
#define EX_SELF_ID "likeos-explorer.desktop"

GAppInfo *ex_apps_default_for(const char *content_type)
{
	return g_app_info_get_default_for_type(content_type, FALSE);
}

static gboolean already(GPtrArray *a, GAppInfo *app)
{
	for (guint i = 0; i < a->len; i++)
		if (g_app_info_equal(g_ptr_array_index(a, i), app))
			return TRUE;
	return FALSE;
}

GPtrArray *ex_apps_for(const char *content_type)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GAppInfo *def = ex_apps_default_for(content_type);
	GList *rec = g_app_info_get_recommended_for_type(content_type);

	if (def) {
		if (g_strcmp0(g_app_info_get_id(def), EX_SELF_ID) != 0)
			g_ptr_array_add(out, def);
		else
			g_object_unref(def);
	}
	for (GList *l = rec; l; l = l->next) {
		GAppInfo *a = l->data;

		if (g_strcmp0(g_app_info_get_id(a), EX_SELF_ID) == 0 ||
		    already(out, a) || !g_app_info_should_show(a))
			continue;
		g_ptr_array_add(out, g_object_ref(a));
	}
	g_list_free_full(rec, g_object_unref);
	return out;
}

const char *ex_apps_common_type(GPtrArray *types)
{
	const char *first;

	if (!types || types->len == 0)
		return NULL;
	first = g_ptr_array_index(types, 0);
	for (guint i = 1; i < types->len; i++)
		if (strcmp(first, g_ptr_array_index(types, i)) != 0)
			return NULL;
	return first;
}

gboolean ex_apps_set_default(GAppInfo *app, const char *content_type,
			     GError **error)
{
	return g_app_info_set_as_default_for_type(app, content_type, error);
}

static GList *to_list(GPtrArray *files)
{
	GList *l = NULL;

	for (guint i = files->len; i > 0; i--)
		l = g_list_prepend(l, g_ptr_array_index(files, i - 1));
	return l;
}

gboolean ex_apps_launch(GAppInfo *app, GPtrArray *files,
			GAppLaunchContext *ctx, GError **error)
{
	if (app) {
		GList *l = to_list(files);
		gboolean ok = g_app_info_launch(app, l, ctx, error);

		g_list_free(l);
		return ok;
	}
	/* Each file with its own default, grouped so an application gets all
	 * of its files in one launch. */
	GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal,
						   g_free, NULL);
	GHashTable *apps = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, g_object_unref);
	gboolean ok = TRUE;

	for (guint i = 0; i < files->len; i++) {
		GFile *f = g_ptr_array_index(files, i);
		GAppInfo *a = g_file_query_default_handler(f, NULL, NULL);

		if (!a) {
			char *b = g_file_get_basename(f);

			if (error && !*error)
				g_set_error(error, G_IO_ERROR,
					    G_IO_ERROR_NOT_SUPPORTED,
					    "No application is set to open “%s”",
					    b);
			g_free(b);
			ok = FALSE;
			continue;
		}
		const char *id = g_app_info_get_id(a);
		char *key = g_strdup(id ? id : g_app_info_get_executable(a));
		GList *l = g_hash_table_lookup(groups, key);

		if (!g_hash_table_contains(apps, key))
			g_hash_table_insert(apps, g_strdup(key), a);
		else
			g_object_unref(a);
		g_hash_table_insert(groups, key, g_list_append(l, f));
	}
	GHashTableIter it;
	gpointer k, v;

	g_hash_table_iter_init(&it, groups);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		GAppInfo *a = g_hash_table_lookup(apps, k);
		GError *e = NULL;

		if (!g_app_info_launch(a, v, ctx, &e)) {
			ok = FALSE;
			if (error && !*error)
				*error = e;
			else
				g_clear_error(&e);
		}
		g_list_free(v);
	}
	g_hash_table_unref(groups);
	g_hash_table_unref(apps);
	return ok;
}

/* Libraries only a program with windows of its own links against. */
static const char *const windowing_libs[] = {
	"libX11.so", "libxcb.so", "libwayland-client.so", "libgtk-",
	"libgdk-",   "libSDL",
};

static gboolean read_at(int fd, void *buf, size_t len, off_t at)
{
	return pread(fd, buf, len, at) == (ssize_t)len;
}

/* The file offset where the address `va' of a loaded image is stored. */
static gboolean va_to_offset(const Elf64_Phdr *ph, int n, Elf64_Addr va,
			     off_t *out)
{
	for (int i = 0; i < n; i++) {
		if (ph[i].p_type == PT_LOAD && va >= ph[i].p_vaddr &&
		    va - ph[i].p_vaddr < ph[i].p_filesz) {
			*out = (off_t)(ph[i].p_offset + (va - ph[i].p_vaddr));
			return TRUE;
		}
	}
	return FALSE;
}

/* An ELF file: a program, windowed or console, or a library (no program at
 * all).  A program the headers say nothing clear about counts as console:
 * its output is then seen. */
static ExProgramKind elf_kind(int fd)
{
	Elf64_Ehdr eh;
	Elf64_Phdr *ph = NULL;
	Elf64_Dyn *dyn = NULL;
	char *strs = NULL;
	gboolean interp = FALSE;
	Elf64_Addr strtab = 0;
	Elf64_Xword strsz = 0, flags1 = 0;
	size_t ndyn = 0;
	off_t at;
	ExProgramKind kind = EX_PROGRAM_CONSOLE;
	int dyn_i = -1;

	if (!read_at(fd, &eh, sizeof(eh), 0) ||
	    memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
		return EX_PROGRAM_NONE;
	if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN)
		return EX_PROGRAM_NONE;
	if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh.e_ident[EI_DATA] != ELFDATA2LSB ||
	    eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 ||
	    eh.e_phnum > 128)
		return EX_PROGRAM_CONSOLE;
	ph = g_new(Elf64_Phdr, eh.e_phnum);
	if (!read_at(fd, ph, sizeof(*ph) * eh.e_phnum, (off_t)eh.e_phoff))
		goto out;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP)
			interp = TRUE;
		else if (ph[i].p_type == PT_DYNAMIC)
			dyn_i = i;
	}
	if (dyn_i < 0)
		goto out; /* static: no libraries at all */
	ndyn = MIN(ph[dyn_i].p_filesz, 64 * 1024) / sizeof(Elf64_Dyn);
	dyn = g_new(Elf64_Dyn, ndyn ? ndyn : 1);
	if (!read_at(fd, dyn, ndyn * sizeof(Elf64_Dyn),
		     (off_t)ph[dyn_i].p_offset))
		goto out;
	for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++) {
		if (dyn[i].d_tag == DT_STRTAB)
			strtab = dyn[i].d_un.d_ptr;
		else if (dyn[i].d_tag == DT_STRSZ)
			strsz = dyn[i].d_un.d_val;
		else if (dyn[i].d_tag == DT_FLAGS_1)
			flags1 = dyn[i].d_un.d_val;
	}
	/* A shared object with neither an interpreter nor the PIE flag is
	 * a library, not something to run. */
	if (eh.e_type == ET_DYN && !interp && !(flags1 & DF_1_PIE)) {
		kind = EX_PROGRAM_NONE;
		goto out;
	}
	if (!strtab || !strsz || strsz > 1024 * 1024 ||
	    !va_to_offset(ph, eh.e_phnum, strtab, &at))
		goto out;
	strs = g_malloc(strsz + 1);
	if (!read_at(fd, strs, strsz, at))
		goto out;
	strs[strsz] = '\0';
	for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL; i++) {
		const char *lib;

		if (dyn[i].d_tag != DT_NEEDED || dyn[i].d_un.d_val >= strsz)
			continue;
		lib = strs + dyn[i].d_un.d_val;
		for (guint j = 0; j < G_N_ELEMENTS(windowing_libs); j++)
			if (g_str_has_prefix(lib, windowing_libs[j]))
				kind = EX_PROGRAM_WINDOWED;
	}
out:
	g_free(strs);
	g_free(dyn);
	g_free(ph);
	return kind;
}

ExProgramKind ex_apps_program_kind(GFile *f, gboolean can_execute)
{
	ExProgramKind kind = EX_PROGRAM_NONE;
	char *path;
	char head[SELFMAG];
	int fd;

	if (!can_execute || !g_file_is_native(f))
		return EX_PROGRAM_NONE;
	path = g_file_get_path(f);
	fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
	g_free(path);
	if (fd < 0)
		return EX_PROGRAM_NONE;
	/* By what the file starts with, not its type's name: the type
	 * database files scripts under executables too. */
	if (read_at(fd, head, 2, 0) && head[0] == '#' && head[1] == '!')
		kind = EX_PROGRAM_CONSOLE; /* a script, run like a .bat */
	else if (read_at(fd, head, SELFMAG, 0) &&
		 memcmp(head, ELFMAG, SELFMAG) == 0)
		kind = elf_kind(fd);
	close(fd);
	return kind;
}
