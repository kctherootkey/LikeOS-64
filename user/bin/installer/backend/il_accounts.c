/* libinstaller -- accounts of the installed system.
 *
 * /etc/passwd, /etc/shadow and /etc/group (and /etc/gshadow when the image
 * has one) of the new root are edited as text: read, changed line by line,
 * written back atomically with their usual modes (0644, 0600, 0644, 0600)
 * and owner root.  Lines this code does not touch are kept byte for byte.
 *
 *  - root gets the chosen password (yescrypt, "$y$").
 *  - the new account gets a group of its own name as its primary group:
 *    one the image already has keeps its gid (a system group of that name
 *    is refused), with the same number as user id when that is free;
 *    otherwise the first id from 1000 on that is free in both files serves
 *    as both, failing that the first free of each.  Then the supplementary
 *    groups audio, render and users (users, gid 100, is created when the
 *    image has none), /bin/bash, and a home directory copied from
 *    /etc/skel, owned by it, mode 0700.
 *
 * Running it again (a retried installation) updates the account in place.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <crypt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_FIRST_UID     1000
#define IL_LAST_UID      59999
#define IL_USERS_GID     100
#define IL_MAX_LINES     4096

/* ---- hashing ------------------------------------------------------------ */

int il_hash_password(il_ctx *ctx, const char *pw, char *out, size_t len)
{
	char salt[CRYPT_GENSALT_OUTPUT_SIZE > 64 ? CRYPT_GENSALT_OUTPUT_SIZE : 64];
	struct crypt_data *cd;
	const char *h;
	size_t n;

	if (!pw)
		return il_set_error(ctx, EINVAL, "internal: no password to hash");
	if (!crypt_gensalt_rn("$y$", 0, NULL, 0, salt, (int)sizeof(salt)) ||
	    strncmp(salt, "$y$", 3) != 0)
		return il_set_error(ctx, errno ? errno : EINVAL, "No yescrypt "
				    "salt could be made: %s",
				    strerror(errno ? errno : EINVAL));
	cd = calloc(1, sizeof(*cd));
	if (!cd)
		return il_set_error(ctx, ENOMEM, "out of memory");
	h = crypt_r(pw, salt, cd);
	if (!h || h[0] == '*' || strncmp(h, "$y$", 3) != 0) {
		memset(cd, 0, sizeof(*cd));
		free(cd);
		return il_set_error(ctx, EINVAL, "The password cannot be hashed "
				    "with yescrypt");
	}
	n = il_strlcpy(out, h, len);
	memset(cd, 0, sizeof(*cd));
	free(cd);
	if (n >= len)
		return il_set_error(ctx, ENOSPC, "internal: password hash too "
				    "long");
	return 0;
}

/* A ready hash from an answer file: "$id$...", nothing that breaks the
 * colon-separated format. */
static int il_hash_ok(const char *h)
{
	if (!h || h[0] != '$' || strlen(h) < 8)
		return 0;
	for (const char *p = h; *p; p++)
		if (*p == ':' || *p == '\n' || *p == '\r' ||
		    (unsigned char)*p < 0x20)
			return 0;
	return 1;
}

static int il_password_field(il_ctx *ctx, const char *plain,
			     const char *hash, const char *who, char *out,
			     size_t len)
{
	if (plain && plain[0])
		return il_hash_password(ctx, plain, out, len);
	if (hash && hash[0]) {
		if (!il_hash_ok(hash))
			return il_set_error(ctx, EINVAL, "The password hash of %s "
					    "is not usable", who);
		il_strlcpy(out, hash, len);
		return 0;
	}
	/* No password: the account is locked (only reachable through
	 * another, e.g. root through su from nowhere -- or not at all). */
	il_log(ctx, IL_LOG_WARN, "no password for %s: the account is locked",
	       who);
	il_strlcpy(out, "!", len);
	return 0;
}

/* ---- account files ---------------------------------------------------- */

struct il_lines {
	char *path;
	char **line;
	int n;
	int cap;
	int changed;
	int present;
};

static void il_lines_free(struct il_lines *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->line[i]);
	free(l->line);
	free(l->path);
	memset(l, 0, sizeof(*l));
}

static int il_lines_add(struct il_lines *l, const char *s)
{
	char *c;

	if (l->n >= IL_MAX_LINES)
		return -EFBIG;
	if (l->n >= l->cap) {
		int nc = l->cap ? l->cap * 2 : 64;
		char **nl = realloc(l->line, (size_t)nc * sizeof(*nl));

		if (!nl)
			return -ENOMEM;
		l->line = nl;
		l->cap = nc;
	}
	c = strdup(s);
	if (!c)
		return -ENOMEM;
	l->line[l->n++] = c;
	l->changed = 1;
	return 0;
}

static int il_lines_load(il_ctx *ctx, const char *root, const char *rel,
			 struct il_lines *l, int required)
{
	char path[IL_PATH_MAX];
	char *text = NULL, *p;
	int rc;

	memset(l, 0, sizeof(*l));
	if (il_path_in(path, sizeof(path), root, rel))
		return il_set_error(ctx, ENAMETOOLONG, "%s: path too long", rel);
	l->path = strdup(path);
	if (!l->path)
		return il_set_error(ctx, ENOMEM, "out of memory");
	rc = il_read_file_alloc(path, &text, NULL);
	if (rc == -ENOENT && !required)
		return 0;
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be read: %s", path,
				    strerror(-rc));
	l->present = 1;
	p = text;
	while (*p) {
		char *nl = strchr(p, '\n');

		if (nl)
			*nl = '\0';
		rc = il_lines_add(l, p);
		if (rc) {
			free(text);
			return il_set_error(ctx, -rc, "%s: %s", path,
					    strerror(-rc));
		}
		if (!nl)
			break;
		p = nl + 1;
	}
	free(text);
	l->changed = 0;
	return 0;
}

static int il_lines_save(il_ctx *ctx, struct il_lines *l, mode_t mode)
{
	size_t total = 1;
	char *buf, *w;
	int rc;

	if (!l->changed)
		return 0;
	for (int i = 0; i < l->n; i++)
		total += strlen(l->line[i]) + 1;
	buf = malloc(total);
	if (!buf)
		return il_set_error(ctx, ENOMEM, "out of memory");
	w = buf;
	for (int i = 0; i < l->n; i++) {
		size_t n = strlen(l->line[i]);

		memcpy(w, l->line[i], n);
		w += n;
		*w++ = '\n';
	}
	*w = '\0';
	rc = il_write_file_atomic(l->path, buf, (size_t)(w - buf), mode, 0, 0);
	memset(buf, 0, total);
	free(buf);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be written: %s",
				    l->path, strerror(-rc));
	il_log(ctx, IL_LOG_INFO, "updated %s", l->path);
	l->changed = 0;
	return 0;
}

/* Field i (0-based) of a colon-separated line, into out. */
static void il_field(const char *line, int i, char *out, size_t len)
{
	const char *p = line, *e;

	for (int k = 0; k < i; k++) {
		p = strchr(p, ':');
		if (!p) {
			out[0] = '\0';
			return;
		}
		p++;
	}
	e = strchr(p, ':');
	if (!e)
		e = p + strlen(p);
	if ((size_t)(e - p) >= len)
		e = p + len - 1;
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = '\0';
}

/* The line whose first field is name, or -1. */
static int il_find(const struct il_lines *l, const char *name)
{
	size_t n = strlen(name);

	for (int i = 0; i < l->n; i++)
		if (strncmp(l->line[i], name, n) == 0 && l->line[i][n] == ':')
			return i;
	return -1;
}

/* 1 when id is the third field (uid / gid) of some line. */
static int il_id_used(const struct il_lines *l, long id)
{
	char f[32];

	for (int i = 0; i < l->n; i++) {
		char *end;
		long v;

		if (l->line[i][0] == '#' || !l->line[i][0])
			continue;
		il_field(l->line[i], 2, f, sizeof(f));
		v = strtol(f, &end, 10);
		if (end != f && v == id)
			return 1;
	}
	return 0;
}

/* Replaces field i of line idx with value. */
static int il_set_field(struct il_lines *l, int idx, int i, const char *value)
{
	const char *line = l->line[idx];
	const char *p = line, *e;
	char *nl;
	size_t need;

	for (int k = 0; k < i; k++) {
		p = strchr(p, ':');
		if (!p)
			return -EINVAL;
		p++;
	}
	e = strchr(p, ':');
	if (!e)
		e = p + strlen(p);
	need = (size_t)(p - line) + strlen(value) + strlen(e) + 1;
	nl = malloc(need);
	if (!nl)
		return -ENOMEM;
	memcpy(nl, line, (size_t)(p - line));
	strcpy(nl + (p - line), value);
	strcat(nl, e);
	free(l->line[idx]);
	l->line[idx] = nl;
	l->changed = 1;
	return 0;
}

/* Adds user to the member list (field 3) of group line idx. */
static int il_add_member(struct il_lines *l, int idx, int field,
			 const char *user)
{
	char members[2048];
	char *save = NULL, copy[2048];

	il_field(l->line[idx], field, members, sizeof(members));
	il_strlcpy(copy, members, sizeof(copy));
	for (char *m = strtok_r(copy, ",", &save); m;
	     m = strtok_r(NULL, ",", &save))
		if (strcmp(il_trim(m), user) == 0)
			return 0;
	if (members[0])
		il_strlcat(members, ",", sizeof(members));
	il_strlcat(members, user, sizeof(members));
	return il_set_field(l, idx, field, members);
}

/* GECOS: the full name without the characters that would break the line
 * or the field's own comma separators. */
static void il_gecos(const char *in, char *out, size_t len)
{
	size_t n = 0;

	for (const char *p = in; *p && n + 1 < len; p++) {
		if (*p == ':' || *p == ',' || *p == '\n' || *p == '\r' ||
		    (unsigned char)*p < 0x20)
			continue;
		out[n++] = *p;
	}
	out[n] = '\0';
}

struct il_acct {
	struct il_lines passwd, shadow, group, gshadow;
	long days;
};

static int il_set_shadow(il_ctx *ctx, struct il_acct *a, const char *name,
			 const char *hash)
{
	char line[512], days[32];
	int idx = il_find(&a->shadow, name), rc;

	snprintf(days, sizeof(days), "%ld", a->days);
	if (idx < 0) {
		snprintf(line, sizeof(line), "%s:%s:%ld:0:99999:7:::", name,
			 hash, a->days);
		rc = il_lines_add(&a->shadow, line);
	} else {
		rc = il_set_field(&a->shadow, idx, 1, hash);
		if (!rc)
			rc = il_set_field(&a->shadow, idx, 2, days);
	}
	memset(line, 0, sizeof(line));
	if (rc)
		return il_set_error(ctx, -rc, "/etc/shadow: %s", strerror(-rc));
	return 0;
}

static int il_ensure_group(il_ctx *ctx, struct il_acct *a, const char *name,
			   long want_gid, long *gid_out)
{
	char line[256], f[32];
	int idx = il_find(&a->group, name), rc;
	long gid = want_gid;

	if (idx >= 0) {
		il_field(a->group.line[idx], 2, f, sizeof(f));
		if (gid_out)
			*gid_out = strtol(f, NULL, 10);
		return 0;
	}
	while (il_id_used(&a->group, gid)) {
		gid++;
		if (gid >= IL_FIRST_UID && want_gid < IL_FIRST_UID)
			return il_set_error(ctx, ENOSPC, "No group id is free "
					    "for the group %s", name);
	}
	snprintf(line, sizeof(line), "%s:x:%ld:", name, gid);
	rc = il_lines_add(&a->group, line);
	if (!rc && a->gshadow.present && il_find(&a->gshadow, name) < 0) {
		snprintf(line, sizeof(line), "%s:!::", name);
		rc = il_lines_add(&a->gshadow, line);
	}
	if (rc)
		return il_set_error(ctx, -rc, "/etc/group: %s", strerror(-rc));
	il_log(ctx, IL_LOG_INFO, "created the group %s (gid %ld)", name, gid);
	if (gid_out)
		*gid_out = gid;
	return 0;
}

static int il_set_root(il_ctx *ctx, struct il_acct *a,
		       const struct il_config *cfg)
{
	char hash[256];
	int idx, rc;

	rc = il_password_field(ctx, cfg->root_password, cfg->root_password_hash,
			       "root", hash, sizeof(hash));
	if (rc)
		return rc;
	idx = il_find(&a->passwd, "root");
	if (idx < 0) {
		rc = il_lines_add(&a->passwd, "root:x:0:0:root:/root:/bin/bash");
		if (rc)
			return il_set_error(ctx, -rc, "/etc/passwd: %s",
					    strerror(-rc));
	} else {
		char pw[16];

		/* The password lives in /etc/shadow. */
		il_field(a->passwd.line[idx], 1, pw, sizeof(pw));
		if (strcmp(pw, "x") != 0) {
			rc = il_set_field(&a->passwd, idx, 1, "x");
			if (rc)
				return il_set_error(ctx, -rc, "/etc/passwd: %s",
						    strerror(-rc));
		}
	}
	rc = il_set_shadow(ctx, a, "root", hash);
	memset(hash, 0, sizeof(hash));
	if (!rc)
		il_log(ctx, IL_LOG_INFO, "set the password of root");
	return rc;
}

/* The home directory: a copy of /etc/skel, owned by the account. */
static int il_make_home(il_ctx *ctx, const char *root, const char *name,
			long uid, long gid)
{
	char home[IL_PATH_MAX], homes[IL_PATH_MAX], skel[IL_PATH_MAX];
	char rel[IL_USERNAME_MAX + 16];
	int rc;

	snprintf(rel, sizeof(rel), "/home/%s", name);
	if (il_path_in(home, sizeof(home), root, rel) ||
	    il_path_in(homes, sizeof(homes), root, "/home") ||
	    il_path_in(skel, sizeof(skel), root, "/etc/skel"))
		return il_set_error(ctx, ENAMETOOLONG, "home path too long");
	rc = il_mkdir_p(homes, 0755);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be created: %s", homes,
				    strerror(-rc));
	{
		struct stat hst;

		/* What the image has there must be a directory: a chmod
		 * below would follow a symbolic link out of the target. */
		if (lstat(home, &hst) == 0 && !S_ISDIR(hst.st_mode))
			return il_set_error(ctx, EEXIST, "%s exists and is not a "
					    "directory", rel);
	}
	if (!il_file_exists(home)) {
		if (il_is_dir(skel)) {
			rc = il_copy_tree(ctx, skel, home, (uid_t)uid, (gid_t)gid);
			if (rc)
				return rc;
		} else if (mkdir(home, 0700) != 0) {
			return il_set_error(ctx, errno, "%s cannot be created: %s",
					    home, strerror(errno));
		}
		il_log(ctx, IL_LOG_INFO, "created %s from /etc/skel", rel);
	}
	rc = il_chown_tree(ctx, home, (uid_t)uid, (gid_t)gid);
	if (rc)
		return rc;
	if (chmod(home, 0700) != 0)
		return il_set_error(ctx, errno, "%s: %s", home, strerror(errno));
	return 0;
}

static int il_add_user(il_ctx *ctx, const char *root, struct il_acct *a,
		       const struct il_config *cfg)
{
	const struct il_user *u = &cfg->user;
	char line[1024], gecos[IL_FULLNAME_MAX + 1], hash[256], f[32];
	long uid = -1, gid = -1, own_gid = -1;
	int idx, gidx, rc;

	il_gecos(u->fullname, gecos, sizeof(gecos));
	rc = il_password_field(ctx, u->password, cfg->user_password_hash,
			       u->username, hash, sizeof(hash));
	if (rc)
		return rc;
	/* A group of the account's name the image already has. */
	gidx = il_find(&a->group, u->username);
	if (gidx >= 0) {
		char *end;

		il_field(a->group.line[gidx], 2, f, sizeof(f));
		own_gid = strtol(f, &end, 10);
		if (end == f)
			own_gid = -1;
	}
	idx = il_find(&a->passwd, u->username);
	if (idx >= 0) {
		/* Already there (a retried installation): keep the ids. */
		il_field(a->passwd.line[idx], 2, f, sizeof(f));
		uid = strtol(f, NULL, 10);
		il_field(a->passwd.line[idx], 3, f, sizeof(f));
		gid = strtol(f, NULL, 10);
		if (uid < IL_FIRST_UID)
			return il_set_error(ctx, EEXIST, "The name %s belongs to "
					    "a system account", u->username);
		/* A primary gid without a group line, next to a group of
		 * the account's name: that group is the primary one. */
		if (gidx >= 0 && own_gid != gid && own_gid >= IL_FIRST_UID &&
		    own_gid <= IL_LAST_UID && !il_id_used(&a->group, gid)) {
			snprintf(f, sizeof(f), "%ld", own_gid);
			rc = il_set_field(&a->passwd, idx, 3, f);
			if (rc)
				return il_set_error(ctx, -rc, "/etc/passwd: %s",
						    strerror(-rc));
			gid = own_gid;
		}
		rc = il_set_field(&a->passwd, idx, 4, gecos);
		if (rc)
			return il_set_error(ctx, -rc, "/etc/passwd: %s",
					    strerror(-rc));
	} else if (gidx >= 0) {
		/* The image has a group of that name: it becomes the
		 * account's primary group -- unless it is a system group. */
		if (own_gid < IL_FIRST_UID || own_gid > IL_LAST_UID)
			return il_set_error(ctx, EEXIST, "The name %s belongs to "
					    "a system group; choose another "
					    "user name", u->username);
		gid = own_gid;
		/* The same number for the account when it is free. */
		if (!il_id_used(&a->passwd, gid))
			uid = gid;
		for (long id = IL_FIRST_UID; uid < 0 && id <= IL_LAST_UID; id++)
			if (!il_id_used(&a->passwd, id))
				uid = id;
		if (uid < 0)
			return il_set_error(ctx, ENOSPC, "No user id is free");
	} else {
		/* The same number for the account and its group when one
		 * is free in both files, else the first free of each. */
		for (long id = IL_FIRST_UID; id <= IL_LAST_UID; id++) {
			if (!il_id_used(&a->passwd, id) &&
			    !il_id_used(&a->group, id)) {
				uid = gid = id;
				break;
			}
		}
		for (long id = IL_FIRST_UID; uid < 0 && id <= IL_LAST_UID; id++)
			if (!il_id_used(&a->passwd, id))
				uid = id;
		for (long id = IL_FIRST_UID; gid < 0 && id <= IL_LAST_UID; id++)
			if (!il_id_used(&a->group, id))
				gid = id;
		if (uid < 0)
			return il_set_error(ctx, ENOSPC, "No user id is free");
		if (gid < 0)
			return il_set_error(ctx, ENOSPC, "No group id is free");
	}
	if (idx < 0) {
		snprintf(line, sizeof(line), "%s:x:%ld:%ld:%s:/home/%s:/bin/bash",
			 u->username, uid, gid, gecos, u->username);
		rc = il_lines_add(&a->passwd, line);
		if (rc)
			return il_set_error(ctx, -rc, "/etc/passwd: %s",
					    strerror(-rc));
	}
	/* The account's own group. */
	if (il_find(&a->group, u->username) < 0) {
		snprintf(line, sizeof(line), "%s:x:%ld:", u->username, gid);
		rc = il_lines_add(&a->group, line);
		if (!rc && a->gshadow.present &&
		    il_find(&a->gshadow, u->username) < 0) {
			snprintf(line, sizeof(line), "%s:!::", u->username);
			rc = il_lines_add(&a->gshadow, line);
		}
		if (rc)
			return il_set_error(ctx, -rc, "/etc/group: %s",
					    strerror(-rc));
	}
	rc = il_set_shadow(ctx, a, u->username, hash);
	memset(hash, 0, sizeof(hash));
	if (rc)
		return rc;
	if (cfg->user_extra_groups) {
		/* Sound, the GPU render node, and the common group.  Never
		 * video or input: those devices belong to the console user
		 * through their permissions, not through a group. */
		static const char *const extra[] = { "audio", "render", "users",
						     NULL };

		rc = il_ensure_group(ctx, a, "users", IL_USERS_GID, NULL);
		if (rc)
			return rc;
		for (int i = 0; extra[i]; i++) {
			int gi = il_find(&a->group, extra[i]);

			if (gi < 0) {
				il_log(ctx, IL_LOG_WARN, "the image has no group "
				       "%s; %s is not added to it", extra[i],
				       u->username);
				continue;
			}
			rc = il_add_member(&a->group, gi, 3, u->username);
			if (!rc && a->gshadow.present) {
				int si = il_find(&a->gshadow, extra[i]);

				if (si >= 0)
					rc = il_add_member(&a->gshadow, si, 3,
							   u->username);
			}
			if (rc)
				return il_set_error(ctx, -rc, "/etc/group: %s",
						    strerror(-rc));
		}
	}
	il_log(ctx, IL_LOG_INFO, "account %s (uid %ld, gid %ld)%s", u->username,
	       uid, gid, cfg->user_extra_groups ?
	       ", groups audio, render, users" : "");
	return il_make_home(ctx, root, u->username, uid, gid);
}

int il_accounts_apply(il_ctx *ctx, const char *root,
		      const struct il_config *cfg)
{
	struct il_acct a;
	int rc;

	memset(&a, 0, sizeof(a));
	a.days = il_days_since_epoch();
	rc = il_lines_load(ctx, root, "/etc/passwd", &a.passwd, 1);
	if (!rc)
		rc = il_lines_load(ctx, root, "/etc/shadow", &a.shadow, 0);
	if (!rc)
		rc = il_lines_load(ctx, root, "/etc/group", &a.group, 1);
	if (!rc)
		rc = il_lines_load(ctx, root, "/etc/gshadow", &a.gshadow, 0);
	if (!rc)
		rc = il_set_root(ctx, &a, cfg);
	if (!rc && cfg->user.username[0]) {
		il_step_progress(ctx, 0.7, "Creating your account");
		rc = il_add_user(ctx, root, &a, cfg);
	}
	/* Written in the order a reader can live with: groups and shadow
	 * entries before the passwd line that refers to them. */
	if (!rc)
		rc = il_lines_save(ctx, &a.group, 0644);
	if (!rc && a.gshadow.present)
		rc = il_lines_save(ctx, &a.gshadow, 0600);
	if (!rc) {
		a.shadow.changed = 1;
		rc = il_lines_save(ctx, &a.shadow, 0600);
	}
	if (!rc)
		rc = il_lines_save(ctx, &a.passwd, 0644);
	for (int i = 0; i < a.shadow.n; i++)
		memset(a.shadow.line[i], 0, strlen(a.shadow.line[i]));
	il_lines_free(&a.passwd);
	il_lines_free(&a.shadow);
	il_lines_free(&a.group);
	il_lines_free(&a.gshadow);
	return rc;
}
