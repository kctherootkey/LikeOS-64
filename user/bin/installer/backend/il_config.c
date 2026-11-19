/* libinstaller -- configuring the installed system.
 *
 * Writes into the new root (every file atomically: written beside, synced,
 * renamed over):
 *   /etc/fstab                  the root and the EFI system partition by UUID
 *   /etc/hostname, /etc/hosts   the computer's name
 *   /etc/likeos/keyboard.conf   the keyboard layouts (the format
 *                               likeos-keyboard-save writes)
 *   /etc/timezone               the Olson name, and /etc/localtime when the
 *                               zone file exists in the image
 *   /etc/locale.conf            LANG and LC_TIME
 *   /etc/likeos/display.conf    copied from the live system, when it has one
 *   /etc/machine-id             a new id (32 hex digits, version 4 layout)
 * and removes what only the live medium needs (the installer and its
 * session, /install, the live marker).  Running it twice gives the same
 * result.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "il_internal.h"

#define IL_TEXT_MAX (64 * 1024)

/* <root>/<rel> */
static int il_tpath(il_ctx *ctx, const char *root, const char *rel, char *buf,
		    size_t len)
{
	if (il_path_in(buf, len, root, rel))
		return il_set_error(ctx, ENAMETOOLONG, "%s%s: path too long",
				    root, rel);
	return 0;
}

static int il_put(il_ctx *ctx, const char *root, const char *rel,
		  const char *text, mode_t mode)
{
	char path[IL_PATH_MAX], dir[IL_PATH_MAX];
	char *slash;
	int rc;

	rc = il_tpath(ctx, root, rel, path, sizeof(path));
	if (rc)
		return rc;
	il_strlcpy(dir, path, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		rc = il_mkdir_p(dir, 0755);
		if (rc)
			return il_set_error(ctx, -rc, "%s cannot be created: %s",
					    dir, strerror(-rc));
	}
	rc = il_write_file_atomic(path, text, strlen(text), mode, 0, 0);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be written: %s", path,
				    strerror(-rc));
	il_log(ctx, IL_LOG_INFO, "wrote %s", rel);
	return 0;
}

/* Appends to a growing text buffer (truncates silently at the limit, which
 * no file written here comes near). */
struct il_text {
	char *buf;
	size_t len;
	size_t cap;
};

static int il_text_init(struct il_text *t)
{
	t->cap = IL_TEXT_MAX;
	t->len = 0;
	t->buf = malloc(t->cap);
	if (!t->buf)
		return -ENOMEM;
	t->buf[0] = '\0';
	return 0;
}

static void il_text_add(struct il_text *t, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void il_text_add(struct il_text *t, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (t->len + 1 >= t->cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	t->len += (size_t)n;
	if (t->len >= t->cap)
		t->len = t->cap - 1;
}

/* ---- /etc/fstab ------------------------------------------------------ */

/* The mount point (second field) of an fstab line, or "" for comments. */
static void il_fstab_dir(const char *line, char *out, size_t len)
{
	char a[512], b[512];

	out[0] = '\0';
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '#' || !*line)
		return;
	if (sscanf(line, "%511s %511s", a, b) == 2)
		il_strlcpy(out, b, len);
}

static int il_write_fstab(il_ctx *ctx, const struct il_target *t)
{
	struct il_text out;
	char path[IL_PATH_MAX];
	char *old = NULL;
	int rc;

	rc = il_tpath(ctx, t->root_dir, "/etc/fstab", path, sizeof(path));
	if (rc)
		return rc;
	if (il_text_init(&out))
		return il_set_error(ctx, ENOMEM, "out of memory");
	il_text_add(&out,
		    "# /etc/fstab -- the file systems of this LikeOS "
		    "installation\n"
		    "#\n"
		    "# Written by the LikeOS installer.  init mounts every line "
		    "but the root\n"
		    "# (which the boot loader and the kernel mount) at start-up, "
		    "checking\n"
		    "# the file systems with a pass number above 0 first.\n"
		    "#\n"
		    "# <file system>  <mount point>  <type>  <options>  <dump>  "
		    "<pass>\n"
		    "UUID=%s  /          ext4  defaults                    0  1\n"
		    "UUID=%s  /boot/efi  vfat  umask=0077,shortname=mixed  0  2\n",
		    t->root_fsuuid, t->esp_fsuuid);
	/* Other lines the image brings (not the root, not the ESP, not the
	 * placeholder lines) are kept. */
	if (il_read_file_alloc(path, &old, NULL) == 0) {
		char *save = NULL;
		int kept = 0;

		for (char *line = strtok_r(old, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			char dir[512];

			il_fstab_dir(line, dir, sizeof(dir));
			if (!dir[0] || strcmp(dir, "/") == 0 ||
			    strcmp(dir, "/boot/efi") == 0 ||
			    strstr(line, "@ROOT") || strstr(line, "@ESP"))
				continue;
			if (!kept)
				il_text_add(&out, "\n# Kept from the system "
					    "image:\n");
			il_text_add(&out, "%s\n", line);
			kept++;
		}
		free(old);
	}
	rc = il_put(ctx, t->root_dir, "/etc/fstab", out.buf, 0644);
	free(out.buf);
	return rc;
}

/* ---- host name ---------------------------------------------------------- */

static int il_is_loopback_line(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return il_starts_with(line, "127.") || il_starts_with(line, "::1") ||
	       il_starts_with(line, "0:0:0:0:0:0:0:1");
}

static int il_write_hostname(il_ctx *ctx, const struct il_config *cfg,
			     const struct il_target *t)
{
	struct il_text out;
	char line[IL_HOSTNAME_MAX + 2];
	char path[IL_PATH_MAX];
	char *old = NULL;
	int rc;

	snprintf(line, sizeof(line), "%s\n", cfg->hostname);
	rc = il_put(ctx, t->root_dir, "/etc/hostname", line, 0644);
	if (rc)
		return rc;
	if (il_text_init(&out))
		return il_set_error(ctx, ENOMEM, "out of memory");
	il_text_add(&out,
		    "# /etc/hosts -- static host names (RFC 952 / RFC 1123 "
		    "syntax)\n"
		    "# Format:  address  canonical-name  [aliases...]\n"
		    "\n"
		    "127.0.0.1   localhost localhost.localdomain\n"
		    "::1         localhost ip6-localhost ip6-loopback\n"
		    "127.0.1.1   %s\n",
		    cfg->hostname);
	rc = il_tpath(ctx, t->root_dir, "/etc/hosts", path, sizeof(path));
	if (!rc && il_read_file_alloc(path, &old, NULL) == 0) {
		char *save = NULL;
		int kept = 0;

		for (char *l = strtok_r(old, "\n", &save); l;
		     l = strtok_r(NULL, "\n", &save)) {
			char *s = l;

			while (*s == ' ' || *s == '\t')
				s++;
			if (!*s || *s == '#' || il_is_loopback_line(s))
				continue;
			if (!kept)
				il_text_add(&out, "\n");
			il_text_add(&out, "%s\n", l);
			kept++;
		}
		free(old);
	}
	rc = rc ? rc : il_put(ctx, t->root_dir, "/etc/hosts", out.buf, 0644);
	free(out.buf);
	return rc;
}

/* ---- keyboard --------------------------------------------------------- */

int il_keyboard_conf_text(const struct il_config *cfg, char *buf, size_t len)
{
	char layouts[IL_MAX_LAYOUTS * 33], variants[IL_MAX_LAYOUTS * 33];
	char options[256];
	int n = cfg->nlayouts, any_variant = 0, w;

	if (n < 1)
		n = 1;
	if (n > IL_MAX_LAYOUTS)
		n = IL_MAX_LAYOUTS;
	layouts[0] = variants[0] = '\0';
	for (int i = 0; i < n; i++) {
		if (i) {
			il_strlcat(layouts, ",", sizeof(layouts));
			il_strlcat(variants, ",", sizeof(variants));
		}
		il_strlcat(layouts, cfg->layouts[i][0] ? cfg->layouts[i] : "us",
			   sizeof(layouts));
		il_strlcat(variants, cfg->variants[i], sizeof(variants));
		if (cfg->variants[i][0])
			any_variant = 1;
	}
	/* Every layout's default variant is written as an empty VARIANT. */
	if (!any_variant)
		variants[0] = '\0';
	il_strlcpy(options, cfg->xkb_options, sizeof(options));
	/* More than one layout needs a way to switch between them. */
	if (n > 1 && !strstr(options, "grp:")) {
		if (options[0])
			il_strlcat(options, ",", sizeof(options));
		il_strlcat(options, "grp:alt_shift_toggle", sizeof(options));
	}
	w = snprintf(buf, len,
		     "# /etc/likeos/keyboard.conf - the keyboard layout of this "
		     "machine\n"
		     "#\n"
		     "# Written by Settings > Keyboard and by loadkeys (through\n"
		     "# /usr/libexec/likeos-keyboard-save), read by the X session\n"
		     "# (likeos-xkb-apply) and by init, which loads the console "
		     "keymap at boot.\n"
		     "# The values are XKB names, comma lists of equal length:\n"
		     "#\n"
		     "#   LAYOUT   the layouts, e.g. de or de,us; the first is the "
		     "console's\n"
		     "#   VARIANT  their variants, an empty entry for a layout's "
		     "default\n"
		     "#   OPTIONS  XKB options, e.g. grp:alt_shift_toggle\n"
		     "#\n"
		     "# The layouts and variants available are those of\n"
		     "# /usr/share/likeos/keyboard/layouts.tsv.\n"
		     "LAYOUT=%s\n"
		     "VARIANT=%s\n"
		     "OPTIONS=%s\n",
		     layouts, variants, options);
	if (w < 0 || (size_t)w >= len)
		return -ENOSPC;
	return 0;
}

static int il_write_keyboard(il_ctx *ctx, const struct il_config *cfg,
			     const struct il_target *t)
{
	char text[2048];

	if (il_keyboard_conf_text(cfg, text, sizeof(text)) != 0)
		return il_set_error(ctx, ENOSPC, "The keyboard settings are too "
				    "long");
	return il_put(ctx, t->root_dir, "/etc/likeos/keyboard.conf", text, 0644);
}

/* ---- time zone and locale --------------------------------------------- */

static int il_write_timezone(il_ctx *ctx, const struct il_config *cfg,
			     const struct il_target *t)
{
	char line[96], zonefile[IL_PATH_MAX], rel[128], link[IL_PATH_MAX];
	const char *zone = cfg->timezone[0] ? cfg->timezone : "UTC";
	int rc;

	snprintf(line, sizeof(line), "%s\n", zone);
	rc = il_put(ctx, t->root_dir, "/etc/timezone", line, 0644);
	if (rc)
		return rc;
	/* The compiled zone file, for programs that read /etc/localtime. */
	snprintf(rel, sizeof(rel), "/usr/share/zoneinfo/%s", zone);
	if (il_tpath(ctx, t->root_dir, rel, zonefile, sizeof(zonefile)) ||
	    il_tpath(ctx, t->root_dir, "/etc/localtime", link, sizeof(link)))
		return 0;
	unlink(link);
	if (il_file_exists(zonefile)) {
		if (symlink(rel, link) != 0)
			il_log(ctx, IL_LOG_WARN, "/etc/localtime cannot be "
			       "linked to %s: %s", rel, strerror(errno));
		else
			il_log(ctx, IL_LOG_INFO, "/etc/localtime -> %s", rel);
	}
	return 0;
}

static int il_write_locale(il_ctx *ctx, const struct il_config *cfg,
			   const struct il_target *t)
{
	char text[512];
	const char *lang = cfg->locale[0] ? cfg->locale : "en_US.UTF-8";
	const char *tf = cfg->time_format[0] ? cfg->time_format : lang;

	snprintf(text, sizeof(text),
		 "# /etc/locale.conf -- the language and formats of this "
		 "system\n"
		 "# Read by init at start-up; LANG is the language, LC_TIME the "
		 "way\n"
		 "# dates and times are written.\n"
		 "LANG=%s\n"
		 "LC_TIME=%s\n",
		 lang, tf);
	return il_put(ctx, t->root_dir, "/etc/locale.conf", text, 0644);
}

/* ---- live system leftovers -------------------------------------------- */

static int il_copy_display_conf(il_ctx *ctx, const struct il_target *t)
{
	char src[IL_PATH_MAX], dst[IL_PATH_MAX];
	int rc;

	il_sys_path(ctx, src, sizeof(src), "/etc/likeos/display.conf");
	if (!il_file_exists(src))
		return 0;
	rc = il_tpath(ctx, t->root_dir, "/etc/likeos/display.conf", dst,
		      sizeof(dst));
	if (rc)
		return rc;
	{
		char dir[IL_PATH_MAX];

		rc = il_tpath(ctx, t->root_dir, "/etc/likeos", dir, sizeof(dir));
		if (!rc)
			il_mkdir_p(dir, 0755);
	}
	rc = il_copy_file(src, dst, 0644);
	if (rc) {
		il_log(ctx, IL_LOG_WARN, "the display settings of the live "
		       "system were not taken over: %s", strerror(-rc));
		return 0;
	}
	if (chown(dst, 0, 0) != 0 && errno != EPERM)
		il_log(ctx, IL_LOG_WARN, "%s: owner not set: %s", dst,
		       strerror(errno));
	il_log(ctx, IL_LOG_INFO, "took over the display settings of the live "
	       "system");
	return 0;
}

static int il_write_machine_id(il_ctx *ctx, const struct il_target *t)
{
	unsigned char b[16];
	char id[40], line[48], dbus[IL_PATH_MAX], dir[IL_PATH_MAX];
	int rc;

	rc = il_random_bytes(b, sizeof(b));
	if (rc)
		return il_set_error(ctx, -rc, "No random numbers for the machine "
				    "id: %s", strerror(-rc));
	/* Laid out as a version 4 (random) UUID, the way init makes one. */
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	il_hex(b, sizeof(b), id);
	snprintf(line, sizeof(line), "%s\n", id);
	rc = il_put(ctx, t->root_dir, "/etc/machine-id", line, 0444);
	if (rc)
		return rc;
	/* D-Bus's copy is a link to it, as init would make it. */
	if (il_tpath(ctx, t->root_dir, "/var/lib/dbus", dir, sizeof(dir)) ||
	    il_tpath(ctx, t->root_dir, "/var/lib/dbus/machine-id", dbus,
		     sizeof(dbus)))
		return 0;
	if (il_mkdir_p(dir, 0755) == 0) {
		unlink(dbus);
		if (symlink("/etc/machine-id", dbus) != 0)
			il_log(ctx, IL_LOG_WARN, "%s cannot be linked: %s", dbus,
			       strerror(errno));
	}
	return 0;
}

static int il_remove_live_leftovers(il_ctx *ctx, const struct il_target *t)
{
	static const char *const files[] = {
		"/etc/inittab.install",
		"/usr/libexec/likeos-installer-session",
		"/usr/bin/likeos-installer",
		"/usr/sbin/likeos-install",
		"/LIKEOS.LIVE",
		"/etc/likeos/live",
		NULL,
	};
	static const char *const trees[] = {
		"/install",
		"/usr/share/likeos/installer",
		NULL,
	};
	char path[IL_PATH_MAX];

	for (int i = 0; files[i]; i++) {
		if (il_tpath(ctx, t->root_dir, files[i], path, sizeof(path)))
			continue;
		if (unlink(path) == 0)
			il_log(ctx, IL_LOG_INFO, "removed %s (live medium only)",
			       files[i]);
		else if (errno != ENOENT && errno != EISDIR)
			il_log(ctx, IL_LOG_WARN, "%s cannot be removed: %s",
			       files[i], strerror(errno));
	}
	for (int i = 0; trees[i]; i++) {
		if (il_tpath(ctx, t->root_dir, trees[i], path, sizeof(path)))
			continue;
		if (!il_file_exists(path))
			continue;
		if (il_remove_tree(ctx, path) == 0)
			il_log(ctx, IL_LOG_INFO, "removed %s (live medium only)",
			       trees[i]);
		else
			il_log(ctx, IL_LOG_WARN, "%s cannot be removed",
			       trees[i]);
	}
	il_clear_error(ctx);
	return 0;
}

int il_configure_target(il_ctx *ctx, const struct il_config *cfg,
			const struct il_target *t)
{
	int rc;

	il_log(ctx, IL_LOG_INFO, "configuring %s", t->root_dir);
	rc = il_write_fstab(ctx, t);
	if (!rc)
		rc = il_write_hostname(ctx, cfg, t);
	if (!rc) {
		il_step_progress(ctx, 0.15, "Saving the keyboard layout");
		rc = il_write_keyboard(ctx, cfg, t);
	}
	if (!rc) {
		il_step_progress(ctx, 0.25, "Setting the time zone");
		rc = il_write_timezone(ctx, cfg, t);
	}
	if (!rc)
		rc = il_write_locale(ctx, cfg, t);
	if (!rc)
		rc = il_copy_display_conf(ctx, t);
	if (!rc)
		rc = il_write_machine_id(ctx, t);
	if (!rc) {
		il_step_progress(ctx, 0.4, "Removing the live system's files");
		rc = il_remove_live_leftovers(ctx, t);
	}
	return rc;
}
