/* libinstaller -- answer files (likeos-install.conf).
 *
 * One setting per line, key=value; '#' starts a comment line; a value may
 * be put in double quotes (then \" and \\ stand for a quote and a
 * backslash, and blanks and '#' are kept).  Keys:
 *
 *   disk=/dev/sda | sda | auto        the disk (auto: the only eligible one)
 *   whole_disk=yes|no                 erase the disk (default yes)
 *   root_partition=N                  the partition for LikeOS (whole_disk=no)
 *   reboot=yes|no                     restart when done (default yes)
 *   locale=en_US.UTF-8                language
 *   time_format=en_GB.UTF-8           date and time format
 *   timezone=Europe/Berlin
 *   keyboard=de,us                    XKB layouts (up to four)
 *   keyboard_variant=nodeadkeys,      their variants
 *   keyboard_options=grp:alt_shift_toggle
 *   hostname=likeos-3fa2
 *   root_password= / root_password_hash=
 *   user_fullname= / user_name=
 *   user_password= / user_password_hash=
 *   user_extra_groups=yes|no          audio, render, users (default yes)
 *
 * Saved files have mode 0600: they may hold passwords.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "il_internal.h"

static int il_parse_bool(const char *v, int *out)
{
	if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") ||
	    !strcasecmp(v, "on") || !strcmp(v, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(v, "no") || !strcasecmp(v, "false") ||
	    !strcasecmp(v, "off") || !strcmp(v, "0")) {
		*out = 0;
		return 0;
	}
	return -EINVAL;
}

/* Value text after '=': unquoted (in place).  -EINVAL for an unclosed
 * quote. */
static int il_parse_value(char *v, char **out)
{
	char *w, *r;

	while (*v == ' ' || *v == '\t')
		v++;
	if (*v != '"') {
		*out = il_trim(v);
		return 0;
	}
	r = v + 1;
	w = v;
	while (*r && *r != '"') {
		if (*r == '\\' && (r[1] == '"' || r[1] == '\\')) {
			*w++ = r[1];
			r += 2;
		} else if (*r == '\\' && r[1] == 'n') {
			*w++ = '\n';
			r += 2;
		} else {
			*w++ = *r++;
		}
	}
	if (*r != '"')
		return -EINVAL;
	*w = '\0';
	/* Only blanks and a comment may follow the closing quote. */
	r++;
	while (*r == ' ' || *r == '\t')
		r++;
	if (*r && *r != '#')
		return -EINVAL;
	*out = v;
	return 0;
}

/* "de,us" -> the list; at most IL_MAX_LAYOUTS. */
static int il_split_list(const char *v, char out[][32], int max, int *n)
{
	const char *p = v;
	int k = 0;

	for (;;) {
		size_t len = strcspn(p, ",");

		if (k >= max)
			return -E2BIG;
		if (len >= 32)
			return -ENAMETOOLONG;
		memcpy(out[k], p, len);
		out[k][len] = '\0';
		{
			char *t = il_trim(out[k]);

			if (t != out[k])
				memmove(out[k], t, strlen(t) + 1);
		}
		k++;
		if (!p[len])
			break;
		p += len + 1;
	}
	*n = k;
	return 0;
}

static int il_set_str(char *dst, size_t len, const char *v)
{
	if (strlen(v) >= len)
		return -ENAMETOOLONG;
	il_strlcpy(dst, v, len);
	return 0;
}

int il_answers_load(il_ctx *ctx, const char *path, struct il_answers *out)
{
	char *text = NULL, *p;
	int rc, lineno = 0, nvar = -1;
	char variants[IL_MAX_LAYOUTS][32];

	if (!out || !path)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	il_strlcpy(out->disk, "auto", sizeof(out->disk));
	out->whole_disk = 1;
	out->root_partno = -1;
	out->reboot = 1;
	il_config_defaults(&out->cfg);
	memset(variants, 0, sizeof(variants));
	rc = il_read_file_alloc(path, &text, NULL);
	if (rc)
		return il_set_error(ctx, -rc, "The answer file %s cannot be "
				    "read: %s", path, strerror(-rc));
	p = text;
	while (p && *p) {
		char *nl = strchr(p, '\n'), *line, *eq, *key, *val;

		if (nl)
			*nl = '\0';
		line = il_trim(p);
		p = nl ? nl + 1 : NULL;
		lineno++;
		if (!line[0] || line[0] == '#')
			continue;
		eq = strchr(line, '=');
		if (!eq) {
			rc = il_set_error(ctx, EINVAL, "%s, line %d: expected "
					  "key=value", path, lineno);
			break;
		}
		*eq = '\0';
		key = il_trim(line);
		if (il_parse_value(eq + 1, &val) != 0) {
			rc = il_set_error(ctx, EINVAL, "%s, line %d: the quote "
					  "of %s is not closed", path, lineno,
					  key);
			break;
		}
		rc = 0;
		if (!strcmp(key, "disk")) {
			rc = il_set_str(out->disk, sizeof(out->disk), val);
		} else if (!strcmp(key, "whole_disk")) {
			rc = il_parse_bool(val, &out->whole_disk);
		} else if (!strcmp(key, "root_partition")) {
			char *end;
			long v = strtol(val, &end, 10);

			if (end == val || *end || v < 1 || v > IL_MAX_PARTS)
				rc = -EINVAL;
			else
				out->root_partno = (int)v;
		} else if (!strcmp(key, "reboot")) {
			rc = il_parse_bool(val, &out->reboot);
		} else if (!strcmp(key, "locale")) {
			rc = il_set_str(out->cfg.locale, sizeof(out->cfg.locale),
					val);
		} else if (!strcmp(key, "time_format")) {
			rc = il_set_str(out->cfg.time_format,
					sizeof(out->cfg.time_format), val);
		} else if (!strcmp(key, "timezone")) {
			rc = il_set_str(out->cfg.timezone,
					sizeof(out->cfg.timezone), val);
		} else if (!strcmp(key, "keyboard")) {
			rc = il_split_list(val, out->cfg.layouts, IL_MAX_LAYOUTS,
					   &out->cfg.nlayouts);
		} else if (!strcmp(key, "keyboard_variant")) {
			rc = val[0] ? il_split_list(val, variants,
						    IL_MAX_LAYOUTS, &nvar) : 0;
			if (!val[0])
				nvar = 0;
		} else if (!strcmp(key, "keyboard_options")) {
			rc = il_set_str(out->cfg.xkb_options,
					sizeof(out->cfg.xkb_options), val);
		} else if (!strcmp(key, "hostname")) {
			rc = il_set_str(out->cfg.hostname,
					sizeof(out->cfg.hostname), val);
		} else if (!strcmp(key, "root_password")) {
			rc = il_set_str(out->cfg.root_password,
					sizeof(out->cfg.root_password), val);
		} else if (!strcmp(key, "root_password_hash")) {
			rc = il_set_str(out->cfg.root_password_hash,
					sizeof(out->cfg.root_password_hash), val);
		} else if (!strcmp(key, "user_fullname")) {
			rc = il_set_str(out->cfg.user.fullname,
					sizeof(out->cfg.user.fullname), val);
		} else if (!strcmp(key, "user_name")) {
			rc = il_set_str(out->cfg.user.username,
					sizeof(out->cfg.user.username), val);
		} else if (!strcmp(key, "user_password")) {
			rc = il_set_str(out->cfg.user.password,
					sizeof(out->cfg.user.password), val);
		} else if (!strcmp(key, "user_password_hash")) {
			rc = il_set_str(out->cfg.user_password_hash,
					sizeof(out->cfg.user_password_hash), val);
		} else if (!strcmp(key, "user_extra_groups")) {
			rc = il_parse_bool(val, &out->cfg.user_extra_groups);
		} else {
			rc = il_set_error(ctx, EINVAL, "%s, line %d: unknown "
					  "setting \"%s\"", path, lineno, key);
			break;
		}
		if (rc) {
			rc = il_set_error(ctx, -rc, "%s, line %d: bad value for "
					  "%s", path, lineno, key);
			break;
		}
	}
	if (text) {
		/* It may have held passwords. */
		memset(text, 0, strlen(text));
		free(text);
	}
	if (rc)
		return rc;
	if (nvar > 0) {
		if (nvar != out->cfg.nlayouts)
			return il_set_error(ctx, EINVAL, "%s: %d keyboard layouts "
					    "but %d variants", path,
					    out->cfg.nlayouts, nvar);
		memcpy(out->cfg.variants, variants, sizeof(variants));
	}
	if (!out->whole_disk && out->root_partno < 1)
		return il_set_error(ctx, EINVAL, "%s: whole_disk=no needs "
				    "root_partition=", path);
	il_log(ctx, IL_LOG_INFO, "answers from %s: disk %s, %s", path,
	       out->disk, out->whole_disk ? "erase the whole disk" :
	       "install on one partition");
	return 0;
}

/* The text of an answer file being made.  It holds passwords: a bigger
 * buffer is a fresh one and the old one is wiped before it is freed.  Once
 * a buffer cannot be had, nomem is set, everything after is dropped and
 * the file is not written at all. */
struct il_abuf {
	char *buf;
	size_t len;
	size_t cap;
	int nomem;
};

/* Room for need more bytes (and the terminator); 0 when there is none. */
static int il_abuf_room(struct il_abuf *b, size_t need)
{
	size_t nc;
	char *nb;

	if (b->nomem)
		return 0;
	if (b->len + need < b->cap)
		return 1;
	nc = (b->cap + need) * 2;
	nb = malloc(nc);
	if (!nb) {
		b->nomem = 1;
		return 0;
	}
	memcpy(nb, b->buf, b->len + 1);
	memset(b->buf, 0, b->cap);
	free(b->buf);
	b->buf = nb;
	b->cap = nc;
	return 1;
}

/* A value as it goes into the file: quoted when it has to be. */
static void il_put_kv(struct il_abuf *b, const char *key, const char *val)
{
	int quote = 0;
	size_t need = strlen(key) + 2 * strlen(val) + 8;
	char *w;

	for (const char *p = val; *p; p++)
		if (*p == ' ' || *p == '\t' || *p == '#' || *p == '"' ||
		    *p == '\\' || *p == '\n' || *p == '=')
			quote = 1;
	if (val[0] && (isspace((unsigned char)val[0]) ||
		       isspace((unsigned char)val[strlen(val) - 1])))
		quote = 1;
	if (!il_abuf_room(b, need))
		return;
	w = b->buf + b->len;
	w += sprintf(w, "%s=", key);
	if (quote)
		*w++ = '"';
	for (const char *p = val; *p; p++) {
		if (quote && (*p == '"' || *p == '\\')) {
			*w++ = '\\';
			*w++ = *p;
		} else if (quote && *p == '\n') {
			*w++ = '\\';
			*w++ = 'n';
		} else {
			*w++ = *p;
		}
	}
	if (quote)
		*w++ = '"';
	*w++ = '\n';
	*w = '\0';
	b->len = (size_t)(w - b->buf);
}

static void il_put_raw(struct il_abuf *b, const char *s)
{
	size_t n = strlen(s);

	if (!il_abuf_room(b, n + 1))
		return;
	memcpy(b->buf + b->len, s, n + 1);
	b->len += n;
}

int il_answers_save(il_ctx *ctx, const char *path, const struct il_answers *a,
		    int include_passwords)
{
	const struct il_config *c;
	struct il_abuf b;
	char tmp[512];
	int rc;

	if (!a || !path)
		return -EINVAL;
	c = &a->cfg;
	memset(&b, 0, sizeof(b));
	b.cap = 1024;
	b.buf = malloc(b.cap);
	if (!b.buf)
		return il_set_error(ctx, ENOMEM, "The answer file %s cannot be "
				    "written: out of memory", path);
	b.buf[0] = '\0';
	il_put_raw(&b,
		   "# likeos-install.conf -- answers for an unattended LikeOS "
		   "installation\n"
		   "# (likeos-install --answers FILE).  See likeos-install.conf"
		   "(5).\n\n");
	il_put_kv(&b, "disk", a->disk[0] ? a->disk : "auto");
	il_put_kv(&b, "whole_disk", a->whole_disk ? "yes" : "no");
	if (!a->whole_disk && a->root_partno > 0) {
		snprintf(tmp, sizeof(tmp), "%d", a->root_partno);
		il_put_kv(&b, "root_partition", tmp);
	}
	il_put_kv(&b, "reboot", a->reboot ? "yes" : "no");
	il_put_raw(&b, "\n# Language, formats, time zone\n");
	il_put_kv(&b, "locale", c->locale);
	il_put_kv(&b, "time_format", c->time_format);
	il_put_kv(&b, "timezone", c->timezone);
	il_put_raw(&b, "\n# Keyboard\n");
	{
		char lay[IL_MAX_LAYOUTS * 33] = "", var[IL_MAX_LAYOUTS * 33] = "";
		int any = 0;

		for (int i = 0; i < c->nlayouts && i < IL_MAX_LAYOUTS; i++) {
			if (i) {
				il_strlcat(lay, ",", sizeof(lay));
				il_strlcat(var, ",", sizeof(var));
			}
			il_strlcat(lay, c->layouts[i], sizeof(lay));
			il_strlcat(var, c->variants[i], sizeof(var));
			if (c->variants[i][0])
				any = 1;
		}
		il_put_kv(&b, "keyboard", lay);
		il_put_kv(&b, "keyboard_variant", any ? var : "");
	}
	il_put_kv(&b, "keyboard_options", c->xkb_options);
	il_put_raw(&b, "\n# This computer\n");
	il_put_kv(&b, "hostname", c->hostname);
	if (include_passwords && c->root_password[0]) {
		il_put_kv(&b, "root_password", c->root_password);
	} else if (c->root_password[0]) {
		char h[256];

		rc = il_hash_password(ctx, c->root_password, h, sizeof(h));
		if (rc) {
			memset(b.buf, 0, b.cap);
			free(b.buf);
			return rc;
		}
		il_put_kv(&b, "root_password_hash", h);
	} else if (c->root_password_hash[0]) {
		il_put_kv(&b, "root_password_hash",
			  c->root_password_hash);
	}
	il_put_raw(&b, "\n# Your account\n");
	il_put_kv(&b, "user_fullname", c->user.fullname);
	il_put_kv(&b, "user_name", c->user.username);
	if (include_passwords && c->user.password[0]) {
		il_put_kv(&b, "user_password", c->user.password);
	} else if (c->user.password[0]) {
		char h[256];

		rc = il_hash_password(ctx, c->user.password, h, sizeof(h));
		if (rc) {
			memset(b.buf, 0, b.cap);
			free(b.buf);
			return rc;
		}
		il_put_kv(&b, "user_password_hash", h);
	} else if (c->user_password_hash[0]) {
		il_put_kv(&b, "user_password_hash",
			  c->user_password_hash);
	}
	il_put_kv(&b, "user_extra_groups",
		  c->user_extra_groups ? "yes" : "no");
	/* All or nothing: a text with a part missing is never written (the
	 * file is replaced only when the whole text is there). */
	if (b.nomem) {
		memset(b.buf, 0, b.cap);
		free(b.buf);
		return il_set_error(ctx, ENOMEM, "The answer file %s cannot be "
				    "written: out of memory", path);
	}
	rc = il_write_file_atomic(path, b.buf, b.len, 0600, (uid_t)-1,
				  (gid_t)-1);
	memset(b.buf, 0, b.cap);
	free(b.buf);
	if (rc)
		return il_set_error(ctx, -rc, "The answer file %s cannot be "
				    "written: %s", path, strerror(-rc));
	il_log(ctx, IL_LOG_INFO, "answers saved to %s%s", path,
	       include_passwords ? " (with passwords)" : "");
	return 0;
}

int il_answers_plan(il_ctx *ctx, const struct il_answers *a,
		    const struct il_disk_list *disks, il_plan **out)
{
	const struct il_disk *d;
	il_plan *plan;
	int rc;

	if (!a || !disks || !out)
		return -EINVAL;
	*out = NULL;
	d = il_disks_find(disks, a->disk[0] ? a->disk : "auto");
	if (!d) {
		if (!a->disk[0] || !strcmp(a->disk, "auto"))
			return il_set_error(ctx, ENODEV, "disk=auto needs exactly "
					    "one disk LikeOS can be installed on; "
					    "name the disk");
		return il_set_error(ctx, ENODEV, "There is no disk %s", a->disk);
	}
	if (d->is_boot_medium)
		return il_set_error(ctx, EBUSY, "%s holds the running LikeOS and "
				    "cannot be the target", d->dev);
	plan = il_plan_new(ctx, d);
	if (!plan)
		return il_set_error(ctx, ENOMEM, "out of memory");
	if (a->whole_disk)
		rc = il_plan_use_whole_disk(plan);
	else
		rc = il_plan_select_root(plan, a->root_partno);
	if (rc) {
		il_plan_free(plan);
		return rc;
	}
	*out = plan;
	return 0;
}
