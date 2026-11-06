/*
 * likeos-keyboard-save - save the keyboard layout of the machine
 *
 * Usage: likeos-keyboard-save LAYOUT VARIANT OPTIONS
 *        likeos-keyboard-save --console ID
 *
 * Writes /etc/likeos/keyboard.conf, the one keyboard setting of the system:
 * the X session applies it (likeos-xkb-apply, run at session start and by
 * the taskbar whenever the file changes) and init loads its console keymap
 * at boot.  Installed setuid root, so that Settings and loadkeys can save
 * the setting for the user at the console.
 *
 * The first form is what Settings > Keyboard runs.  LAYOUT and VARIANT are
 * comma lists of equal length (VARIANT may be empty for "every layout's
 * default") naming XKB layouts; OPTIONS is a comma list of XKB options, or
 * empty.  Once the file is written the console follows: the first layout's
 * console keymap is loaded with loadkeys.
 *
 * The second form is what loadkeys runs after loading a keymap of the
 * catalogue (or an alias of one) on the console: the layout becomes that
 * keymap's, the options are kept, and nothing is loaded -- loadkeys already
 * did that.  The X session follows through the taskbar.
 *
 * Nothing it is given is trusted.  A layout/variant pair has to be a line of
 * the layout catalogue, an option has to be one the XKB rules define, and
 * the file it writes is made of nothing else, so that is all a caller can
 * put into a file the X session and init act on.  It acts for root and for
 * the user logged in at the console (power_policy.h), the one person whose
 * keyboard this is; a remote login cannot change it under them.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "power_policy.h"

#define CONF_DIR "/etc/likeos"
#define CONF_PATH CONF_DIR "/keyboard.conf"
#define CONF_TMP CONF_DIR "/keyboard.conf.tmp"
#define CATALOGUE "/usr/share/likeos/keyboard/layouts.tsv"
#define KEYMAP_DIR "/usr/share/keymaps"
#define XKB_RULES_LIST "/usr/share/X11/xkb/rules/evdev.lst"
#define LOADKEYS "/bin/loadkeys"

/* XKB itself switches between at most four layouts. */
#define MAX_LAYOUTS 4
#define MAX_ARG 256
#define MAX_NAME 64

static const char *prog = "likeos-keyboard-save";

/* One line of the catalogue. */
struct layout {
	char id[MAX_NAME];
	char layout[MAX_NAME];
	char variant[MAX_NAME];
};

/* Characters a catalogue id, alias or XKB name may be made of. */
static int name_ok(const char *s, size_t max)
{
	size_t n = 0;

	for (; *s; s++, n++) {
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
		      (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return 0;
	}
	return n < max;
}

/* Copy field `idx' (TAB-separated) of `line' into out; 0 if it has none or
 * it does not fit. */
static int field(const char *line, int idx, char *out, size_t cap)
{
	const char *p = line, *end;
	size_t n;

	for (int i = 0; i < idx; i++) {
		p = strchr(p, '\t');
		if (!p)
			return 0;
		p++;
	}
	end = strchr(p, '\t');
	n = end ? (size_t)(end - p) : strcspn(p, "\r\n");
	if (n >= cap)
		return 0;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

/*
 * Look a layout up in the catalogue, by id (id != NULL) or by its XKB
 * layout and variant.  1 when found, with *out filled in.
 */
static int catalogue_find(const char *id, const char *layout,
			  const char *variant, struct layout *out)
{
	char line[512];
	FILE *f = fopen(CATALOGUE, "r");
	int found = 0;

	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", prog, CATALOGUE,
			strerror(errno));
		return 0;
	}
	while (!found && fgets(line, sizeof(line), f)) {
		struct layout l;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (!field(line, 0, l.id, sizeof(l.id)) ||
		    !field(line, 3, l.layout, sizeof(l.layout)) ||
		    !field(line, 4, l.variant, sizeof(l.variant)))
			continue;
		if (id ? strcmp(l.id, id) == 0
		       : strcmp(l.layout, layout) == 0 &&
				 strcmp(l.variant, variant) == 0) {
			*out = l;
			found = 1;
		}
	}
	fclose(f);
	return found;
}

/* A catalogue id, or an alias of one: an alias is a link in the keymap
 * directory, <alias>.map -> <id>.map. */
static int console_id_find(const char *id, struct layout *out)
{
	char path[PATH_MAX], target[MAX_NAME + 8];
	ssize_t n;

	if (catalogue_find(id, NULL, NULL, out))
		return 1;
	snprintf(path, sizeof(path), KEYMAP_DIR "/%s.map", id);
	n = readlink(path, target, sizeof(target) - 1);
	if (n <= 4)
		return 0;
	target[n] = '\0';
	if (strcmp(target + n - 4, ".map") != 0)
		return 0;
	target[n - 4] = '\0';
	return name_ok(target, MAX_NAME) &&
	       catalogue_find(target, NULL, NULL, out);
}

/* Is `opt' an option the XKB rules define?  evdev.lst lists them as
 * "  group:name   description" lines in its "! option" section. */
static int xkb_option_known(const char *opt)
{
	char line[512];
	size_t len = strlen(opt);
	int in_options = 0, found = 0;
	FILE *f = fopen(XKB_RULES_LIST, "r");

	if (!f) {
		fprintf(stderr, "%s: %s: %s\n", prog, XKB_RULES_LIST,
			strerror(errno));
		return 0;
	}
	while (!found && fgets(line, sizeof(line), f)) {
		const char *p = line;

		if (line[0] == '!') {
			in_options = strncmp(line, "! option", 8) == 0;
			continue;
		}
		if (!in_options)
			continue;
		while (*p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, opt, len) == 0 &&
		    (p[len] == ' ' || p[len] == '\t' || p[len] == '\n'))
			found = 1;
	}
	fclose(f);
	return found;
}

/* Split a comma list into at most `max' items of out[] (each MAX_NAME);
 * returns the count, or -1 if there are too many or one is too long.  An
 * empty string is one empty item. */
static int split(const char *s, char out[][MAX_NAME], int max)
{
	int n = 0;

	for (;;) {
		size_t len = strcspn(s, ",");

		if (n == max || len >= MAX_NAME)
			return -1;
		memcpy(out[n], s, len);
		out[n][len] = '\0';
		n++;
		if (!s[len])
			return n;
		s += len + 1;
	}
}

/* OPTIONS: the characters XKB option names are made of, and every option one
 * the rules know.  Empty is valid. */
static int options_ok(const char *opts)
{
	const char *p;

	if (strlen(opts) >= MAX_ARG) {
		fprintf(stderr, "%s: XKB options too long\n", prog);
		return 0;
	}
	for (p = opts; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
		      *p == '_' || *p == ':' || *p == ',' || *p == '+' ||
		      *p == '-')) {
			fprintf(stderr, "%s: invalid XKB options '%s'\n", prog,
				opts);
			return 0;
		}
	}
	p = opts;
	while (*p) {
		char opt[MAX_ARG];
		size_t len = strcspn(p, ",");

		memcpy(opt, p, len);
		opt[len] = '\0';
		if (!len || !strchr(opt, ':') || !xkb_option_known(opt)) {
			fprintf(stderr, "%s: unknown XKB option '%s'\n", prog,
				opt);
			return 0;
		}
		p += len;
		if (*p == ',' && !*++p) {
			fprintf(stderr, "%s: empty XKB option\n", prog);
			return 0; /* a trailing comma */
		}
	}
	return 1;
}

/* The OPTIONS of the file as it is now, or "" when there are none or they
 * are not valid any more. */
static void current_options(char *out, size_t cap)
{
	char line[512];
	FILE *f = fopen(CONF_PATH, "r");

	out[0] = '\0';
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *v, *e;

		if (strncmp(line, "OPTIONS=", 8) != 0)
			continue;
		v = line + 8;
		v[strcspn(v, "#\r\n")] = '\0';
		while (*v == ' ' || *v == '\t' || *v == '"' || *v == '\'')
			v++;
		e = v + strlen(v);
		while (e > v && (e[-1] == ' ' || e[-1] == '\t' ||
				 e[-1] == '"' || e[-1] == '\''))
			*--e = '\0';
		if (strlen(v) < cap && options_ok(v))
			memcpy(out, v, strlen(v) + 1);
	}
	fclose(f);
}

static int write_conf(const char *layout, const char *variant,
		      const char *options)
{
	char text[1024];
	int len, fd;

	if (mkdir(CONF_DIR, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "%s: %s: %s\n", prog, CONF_DIR,
			strerror(errno));
		return -1;
	}
	len = snprintf(text, sizeof(text),
		       "# The keyboard layout, saved by likeos-keyboard-save "
		       "(Settings > Keyboard,\n"
		       "# loadkeys).  XKB names; see "
		       "/usr/share/likeos/keyboard/layouts.tsv.\n"
		       "LAYOUT=%s\n"
		       "VARIANT=%s\n"
		       "OPTIONS=%s\n",
		       layout, variant, options);
	if (len < 0 || (size_t)len >= sizeof(text))
		return -1;
	unlink(CONF_TMP);
	fd = open(CONF_TMP, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, CONF_TMP,
			strerror(errno));
		return -1;
	}
	if (write(fd, text, (size_t)len) != len || fsync(fd) != 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, CONF_TMP,
			strerror(errno));
		close(fd);
		unlink(CONF_TMP);
		return -1;
	}
	close(fd);
	/* Replaced, never rewritten in place: a reader sees the old file or
	 * the new one, and the taskbar's watch on the directory sees the
	 * rename. */
	if (rename(CONF_TMP, CONF_PATH) != 0) {
		fprintf(stderr, "%s: %s: %s\n", prog, CONF_PATH,
			strerror(errno));
		unlink(CONF_TMP);
		return -1;
	}
	return 0;
}

/* Settings: LAYOUT VARIANT OPTIONS. */
static int save_xkb(const char *layout_arg, const char *variant_arg,
		    const char *options)
{
	char layouts[MAX_LAYOUTS][MAX_NAME], variants[MAX_LAYOUTS][MAX_NAME];
	struct layout first;
	int nl, nv;

	memset(&first, 0, sizeof(first));

	if (strlen(layout_arg) >= MAX_ARG || strlen(variant_arg) >= MAX_ARG) {
		fprintf(stderr, "%s: argument too long\n", prog);
		return 2;
	}
	nl = split(layout_arg, layouts, MAX_LAYOUTS);
	nv = split(variant_arg, variants, MAX_LAYOUTS);
	if (nl < 0 || nv < 0) {
		fprintf(stderr, "%s: at most %d layouts\n", prog, MAX_LAYOUTS);
		return 2;
	}
	/* An empty VARIANT is every layout's default. */
	if (nv == 1 && !variants[0][0]) {
		for (int i = 1; i < nl; i++)
			variants[i][0] = '\0';
		nv = nl;
	}
	if (nv != nl) {
		fprintf(stderr, "%s: %d layouts but %d variants\n", prog, nl,
			nv);
		return 2;
	}
	for (int i = 0; i < nl; i++) {
		struct layout l;

		if (!layouts[i][0] || !name_ok(layouts[i], MAX_NAME) ||
		    (variants[i][0] && !name_ok(variants[i], MAX_NAME)) ||
		    !catalogue_find(NULL, layouts[i], variants[i], &l)) {
			fprintf(stderr, "%s: unknown layout '%s%s%s%s'\n", prog,
				layouts[i], variants[i][0] ? "(" : "",
				variants[i], variants[i][0] ? ")" : "");
			return 2;
		}
		if (i == 0)
			first = l;
	}
	if (!options_ok(options))
		return 2;
	/* Written from what was checked, not from the arguments: an empty
	 * VARIANT goes back out as it came in. */
	if (write_conf(layout_arg, variant_arg, options) != 0)
		return 1;

	/* The console follows.  Run as root outright, with an environment of
	 * our own: loadkeys is handed nothing but a catalogue id. */
	if (setgid(0) != 0 || setuid(0) != 0) {
		fprintf(stderr, "%s: cannot become root: %s\n", prog,
			strerror(errno));
		return 1;
	}
	clearenv();
	setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
	setenv("LANG", "en_US.UTF-8", 1);
	execl(LOADKEYS, "loadkeys", "-q", "--no-save", first.id, (char *)NULL);
	fprintf(stderr, "%s: saved, but the console keymap was not loaded: "
		"%s: %s\n", prog, LOADKEYS, strerror(errno));
	return 1;
}

/* loadkeys: --console ID. */
static int save_console(const char *id)
{
	char options[MAX_ARG];
	struct layout l;

	if (!name_ok(id, MAX_NAME) || !console_id_find(id, &l)) {
		fprintf(stderr, "%s: '%s' is not a keymap of %s\n", prog, id,
			CATALOGUE);
		return 2;
	}
	current_options(options, sizeof(options));
	return write_conf(l.layout, l.variant, options) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
	umask(022);
	power_sanitize_env();
	if (!power_caller_allowed()) {
		fprintf(stderr,
			"%s: only root and the user at the console may change "
			"the keyboard layout\n",
			prog);
		return 1;
	}
	if (argc == 3 && strcmp(argv[1], "--console") == 0)
		return save_console(argv[2]);
	if (argc == 4 && argv[1][0] != '-')
		return save_xkb(argv[1], argv[2], argv[3]);
	fprintf(stderr, "usage: %s LAYOUT VARIANT OPTIONS\n"
			"       %s --console ID\n",
		prog, prog);
	return 2;
}
