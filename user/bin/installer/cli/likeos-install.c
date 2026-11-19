/* likeos-install -- install LikeOS from a text terminal.
 *
 * The text-mode counterpart of LikeOS Setup (likeos-installer): the same
 * questions, asked one after the other on any terminal -- the console of a
 * machine whose graphics do not work, a serial line, an ssh session -- and
 * the same backend (libinstaller) doing the work.  No X server is needed.
 *
 *   likeos-install                       ask everything, then install
 *   likeos-install --answers FILE        install unattended from an answer
 *                                        file (likeos-install.conf(5))
 *   likeos-install --list-disks          show the disks and stop
 *   likeos-install --dry-run ...         plan and check, write nothing
 *   likeos-install --reset-tty           the terminal back into line mode
 *                                        (for the installer session)
 *
 * The steps: language and formats, keyboard, the disk, its partitions
 * (the whole disk, or a partition chosen or made by hand), time zone,
 * computer name, root password, an account for everyday use, a summary
 * that has to be confirmed by typing YES, and the installation with a
 * progress bar.  Nothing is written before that confirmation; Ctrl-C until
 * the partition table is written stops cleanly, and is refused after it.
 *
 * Exit status: 0 installed (or listed, or a dry run passed, or the terminal
 * reset), 1 wrong usage, 2 the installation failed, 3 cancelled before
 * anything was written, 4 cannot start (no system image, not root, no
 * terminal to ask on).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "installer.h"
#include "cli-ui.h"
#include "cli-keyboard.h"

#define PROG "likeos-install"
#define CLI_VERSION "1.0"

#define EXIT_INSTALLED 0
#define EXIT_USAGE     1
#define EXIT_FAILED    2
#define EXIT_CANCELLED 3
#define EXIT_CANNOT    4

#define MAX_SIM 16
#define KEYBOARD_CONF "/etc/likeos/keyboard.conf"
#define LOADKEYS "/bin/loadkeys"
#define SETXKBMAP "/usr/bin/setxkbmap"

/* The numbered steps of the wizard. */
enum {
	ST_WELCOME = 0,
	ST_LANGUAGE,
	ST_KEYBOARD,
	ST_DISK,
	ST_PARTITION,
	ST_TIMEZONE,
	ST_HOSTNAME,
	ST_ROOTPW,
	ST_USER,
	ST_SUMMARY,
	ST_COUNT
};
#define NUMBERED_STEPS (ST_COUNT - 1)

struct app {
	/* command line */
	int dry_run, yes, no_reboot, verbose, list_disks, simulate;
	const char *answers_path, *save_answers_path;
	const char *source, *target, *log_path;
	const char *sim[MAX_SIM + 1];
	int nsim;

	il_ctx *ctx;
	struct il_source_info src;
	int have_src;
	struct il_disk_list *disks;
	struct il_disk *view;          /* a plan's projected layout */
	int disk;                      /* index into disks, -1 none */
	il_plan *plan;
	int whole_disk;                /* the plan erases the disk */
	struct il_answers ans;         /* ans.cfg is the configuration */
	struct kbd_catalogue kbd;
	int have_kbd;

	/* installation */
	struct ui_progress_state prog;
	int last_step;
	int installing;
	int refused_said;
	int raw_tty;                   /* the terminal is in raw mode */
	struct termios tty_saved;
};

static struct app g_app;
static il_ctx *volatile g_sig_ctx;

/* ---- small helpers ---------------------------------------------------------- */

/* src into dst, cut to fit. */
static void copy_str(char *dst, size_t len, const char *src)
{
	size_t n = src ? strlen(src) : 0;

	if (!len)
		return;
	if (n >= len)
		n = len - 1;
	if (n)
		memmove(dst, src, n);
	dst[n] = '\0';
}

static const char *table_name(int t)
{
	return t == IL_TABLE_GPT ? "GPT" : t == IL_TABLE_MBR ? "MBR" : "none";
}

static const char *transport_name(int t)
{
	switch (t) {
	case IL_TRANSPORT_SATA:
		return "SATA";
	case IL_TRANSPORT_NVME:
		return "NVMe";
	case IL_TRANSPORT_USB:
		return "USB";
	case IL_TRANSPORT_FILE:
		return "image";
	default:
		return "-";
	}
}

static const char *strength_name(int s)
{
	static const char *const n[] = { "very weak", "weak", "fair", "good",
					 "strong" };

	return n[s < 0 ? 0 : s > 4 ? 4 : s];
}

static void size_text(uint64_t bytes, char *buf, size_t len)
{
	il_format_size(bytes, buf, len);
}

/* "20G", "500M", "1.5T", "20GiB", "all", "" -> bytes (0 = all of it).
 * Without a unit the number is in MB.  -1 when it is not a size. */
static long long parse_size(const char *s)
{
	char *end;
	double v;
	double mult = 1000.0 * 1000.0;

	while (*s == ' ')
		s++;
	if (!*s || !strcasecmp(s, "all") || !strcasecmp(s, "rest"))
		return 0;
	v = strtod(s, &end);
	if (end == s || v <= 0)
		return -1;
	while (*end == ' ')
		end++;
	if (!*end) {
		mult = 1000.0 * 1000.0;
	} else {
		char u = (char)toupper((unsigned char)end[0]);
		int bin = (end[1] == 'i' || end[1] == 'I');
		const char *rest = end + 1 + (bin ? 1 : 0);
		double base = bin ? 1024.0 : 1000.0;

		if (*rest == 'B' || *rest == 'b')
			rest++;
		if (*rest)
			return -1;
		switch (u) {
		case 'B':
			mult = 1;
			break;
		case 'K':
			mult = base;
			break;
		case 'M':
			mult = base * base;
			break;
		case 'G':
			mult = base * base * base;
			break;
		case 'T':
			mult = base * base * base * base;
			break;
		default:
			return -1;
		}
	}
	return (long long)(v * mult);
}

/* ---- the backend's log --------------------------------------------------------- */

static void on_log(int level, const char *line, void *user)
{
	struct app *a = user;
	int show;

	if (a->installing)
		show = level >= IL_LOG_WARN || (a->verbose && level >= IL_LOG_INFO) ||
		       a->verbose > 1;
	else
		show = (a->verbose && level >= IL_LOG_INFO) || a->verbose > 1;
	if (!show)
		return;
	if (a->installing)
		ui_progress_clear(&a->prog);
	printf("%s%s%s\n", level >= IL_LOG_WARN ? ui_bold() : ui_dim(), line,
	       ui_reset());
	fflush(stdout);
}

/* ---- disks ------------------------------------------------------------------------ */

static int disk_too_small(const struct app *a, const struct il_disk *d)
{
	return a->have_src && a->src.required_bytes &&
	       d->size_bytes < a->src.required_bytes + IL_ESP_SIZE_BYTES +
				       2 * IL_ALIGN_BYTES;
}

static const char *disk_note(const struct app *a, const struct il_disk *d)
{
	if (d->is_boot_medium)
		return "installation medium, not available";
	if (d->read_only)
		return "read-only, not available";
	if (disk_too_small(a, d))
		return "too small";
	if (d->removable)
		return "removable";
	return "";
}

static int disk_usable(const struct app *a, const struct il_disk *d)
{
	return !d->is_boot_medium && !d->read_only && !disk_too_small(a, d);
}

static void print_disk_table(const struct app *a, int numbered)
{
	int cols = ui_cols() - 1;
	int mw = cols - 2 - 4 - 18 - 11 - 7 - 6 - 6;
	char model[160], dev[64], size[32];

	if (mw < 10)
		mw = 10;
	if (mw > 40)
		mw = 40;
	ui_fit(model, sizeof(model), "Model", mw, 1);
	printf("  %s%-4s%-18s%s %-11s%-7s%-6s%s%s\n", ui_bold(),
	       numbered ? "#" : "", "Device", model, "Size", "Bus", "Table",
	       "Note", ui_reset());
	for (int i = 0; i < a->disks->n; i++) {
		const struct il_disk *d = &a->disks->disks[i];
		char num[16];

		ui_fit(model, sizeof(model), d->model[0] ? d->model : d->name,
		       mw, 1);
		ui_fit(dev, sizeof(dev), d->dev, 17, 1);
		size_text(d->size_bytes, size, sizeof(size));
		snprintf(num, sizeof(num), "%d", i + 1);
		printf("  %s%-4s%s %s %-11s%-7s%-6s%s%s\n",
		       disk_usable(a, d) ? "" : ui_dim(), numbered ? num : "",
		       dev, model, size, transport_name(d->transport),
		       table_name(d->table), disk_note(a, d), ui_reset());
	}
}

/* The partition's kind as the table shows it: the backend's name, put in
 * neutral words for the kinds named after another system's maker (the
 * same words as the graphical installer's). */
static const char *part_kind(const struct il_partition *p)
{
	if (!strcasecmp(p->type_guid, IL_GUID_MSR))
		return "Reserved (MSR)";
	if (!strcasecmp(p->type_guid, "de94bba4-06d1-4d40-a16a-bfd50179d6ac"))
		return "Recovery";
	return p->type_desc[0] ? p->type_desc : p->type_guid;
}

static void print_partitions(const struct il_disk *d, int root, int esp)
{
	char size[32], dev[64], type[64], label[64];

	if (d->nparts == 0)
		printf("  (no partitions)\n");
	else
		printf("  %s%-5s%-18s%-11s%-22s%-7s%-16s%s\n", ui_bold(), "Nr",
		       "Device", "Size", "Type", "FS", "Label", ui_reset());
	for (int i = 0; i < d->nparts; i++) {
		const struct il_partition *p = &d->parts[i];
		const char *mark = p->number == root ? "  <- LikeOS" :
				   p->number == esp ? "  <- boot (EFI)" : "";

		size_text(p->size_bytes, size, sizeof(size));
		ui_fit(dev, sizeof(dev), p->dev[0] ? p->dev : "(new)", 17, 1);
		ui_fit(type, sizeof(type), part_kind(p), 21, 1);
		ui_fit(label, sizeof(label), p->fs_label[0] ? p->fs_label :
			       p->name, 15, 1);
		printf("  %-5d%s %-11s%s %-7s%s%s\n", p->number, dev, size,
		       type, p->fstype[0] ? p->fstype : "-", label, mark);
	}
	if (d->nfree > 0) {
		printf("  %sFree space:%s\n", ui_bold(), ui_reset());
		for (int i = 0; i < d->nfree; i++) {
			char start[32];

			size_text(d->free[i].size_bytes, size, sizeof(size));
			size_text(d->free[i].start_bytes, start, sizeof(start));
			printf("  F%-4d%-11s(starts at %s)\n", i + 1, size,
			       start);
		}
	}
}

static int scan_disks(struct app *a)
{
	int rc = il_disks_scan(a->ctx, a->disks);

	if (rc) {
		ui_error("The disks cannot be listed: %s", il_last_error(a->ctx));
		return rc;
	}
	return 0;
}

/* ---- keyboard ----------------------------------------------------------------- */

/* The keyboard setting the live system has (Settings or loadkeys saved it). */
static void read_live_keyboard(struct il_config *cfg)
{
	char line[512];
	char lay[256] = "", var[256] = "", opt[160] = "";
	FILE *f = fopen(KEYBOARD_CONF, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "LAYOUT=", 7))
			copy_str(lay, sizeof(lay), line + 7);
		else if (!strncmp(line, "VARIANT=", 8))
			copy_str(var, sizeof(var), line + 8);
		else if (!strncmp(line, "OPTIONS=", 8))
			copy_str(opt, sizeof(opt), line + 8);
	}
	fclose(f);
	if (!lay[0])
		return;
	{
		char *ls = NULL, *vs = NULL;
		char *l = strtok_r(lay, ",", &ls);
		const char *vp = var;
		int n = 0;

		while (l && n < IL_MAX_LAYOUTS) {
			size_t vl = strcspn(vp, ",");

			copy_str(cfg->layouts[n], sizeof(cfg->layouts[n]), l);
			if (vl < sizeof(cfg->variants[n])) {
				memcpy(cfg->variants[n], vp, vl);
				cfg->variants[n][vl] = '\0';
			}
			vp += vl;
			if (*vp == ',')
				vp++;
			n++;
			l = strtok_r(NULL, ",", &ls);
		}
		(void)vs;
		if (n > 0)
			cfg->nlayouts = n;
	}
	copy_str(cfg->xkb_options, sizeof(cfg->xkb_options), opt);
}

/* Run argv[0] (absolute) and wait at most `secs'; 0 when it exited 0. */
static int run_quiet(char *const argv[], int secs)
{
	pid_t pid = fork();
	int status, waited = 0;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int fd = open("/dev/null", O_RDWR);

		if (fd >= 0) {
			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	for (;;) {
		pid_t w = waitpid(pid, &status, WNOHANG);

		if (w == pid)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		if (w < 0 && errno != EINTR)
			return -1;
		if (waited >= secs * 1000) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return -1;
		}
		usleep(50 * 1000);
		waited += 50;
	}
}

/* Make the keyboard type what was chosen, right away: the console keymap,
 * or the X layout when this runs in an X terminal. */
static void apply_keyboard(struct app *a)
{
	struct il_config *c = &a->ans.cfg;

	if (a->simulate || geteuid() != 0 || !ui_is_tty())
		return;
	if (getenv("DISPLAY")) {
		char lay[IL_MAX_LAYOUTS * 33] = "", var[IL_MAX_LAYOUTS * 33] = "";
		char *argv[8];
		int k = 0;

		if (access(SETXKBMAP, X_OK) != 0)
			return;
		for (int i = 0; i < c->nlayouts; i++) {
			if (i) {
				strcat(lay, ",");
				strcat(var, ",");
			}
			strcat(lay, c->layouts[i]);
			strcat(var, c->variants[i]);
		}
		argv[k++] = SETXKBMAP;
		argv[k++] = "-layout";
		argv[k++] = lay;
		argv[k++] = "-variant";
		argv[k++] = var;
		argv[k] = NULL;
		if (run_quiet(argv, 10) != 0)
			ui_warn("The keyboard layout could not be switched in "
				"this X session.");
		return;
	}
	if (a->have_kbd && access(LOADKEYS, X_OK) == 0) {
		int i = kbd_find_xkb(&a->kbd, c->layouts[0], c->variants[0]);
		char id[48];
		char *argv[] = { LOADKEYS, "-q", "--no-save", id, NULL };

		if (i < 0)
			return;
		copy_str(id, sizeof(id), a->kbd.l[i].id);
		if (run_quiet(argv, 10) != 0)
			ui_warn("The console keymap %s could not be loaded; the "
				"keyboard keeps its current layout here (the "
				"installed system gets the one chosen).", id);
	}
}

/* ---- defaults ------------------------------------------------------------------ */

static void live_defaults(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	const char *lang = getenv("LANG");
	const char *tz = getenv("TZ");
	char zone[96] = "";

	if (lang && il_locale_find(lang) >= 0) {
		copy_str(c->locale, sizeof(c->locale),
			 il_locale_name(il_locale_find(lang)));
		copy_str(c->time_format, sizeof(c->time_format), c->locale);
	}
	if (tz && tz[0] && tz[0] != ':')
		copy_str(zone, sizeof(zone), tz);
	if (!zone[0]) {
		FILE *f = fopen("/etc/timezone", "r");

		if (f) {
			if (fgets(zone, sizeof(zone), f))
				zone[strcspn(zone, "\r\n \t")] = '\0';
			fclose(f);
		}
	}
	if (zone[0] && il_timezone_find(zone) >= 0)
		copy_str(c->timezone, sizeof(c->timezone), zone);
	read_live_keyboard(c);
}

/* ---- wizard steps ----------------------------------------------------------- */

static void step_header(const char *title, int step)
{
	ui_header(title, step, NUMBERED_STEPS);
}

static int st_welcome(struct app *a)
{
	char buf[16], size[32];
	int rc;

	ui_header("Welcome", 0, NUMBERED_STEPS);
	ui_para("This program installs LikeOS on a disk of this computer. It "
		"asks a few questions first; nothing is written to any disk "
		"before you have seen a summary of what will happen and "
		"confirmed it.");
	ui_blank();
	if (a->have_src) {
		size_text(a->src.uncompressed_bytes, size, sizeof(size));
		printf("  System image:  %s%s%s, %s unpacked%s\n",
		       a->src.version[0] ? "LikeOS " : "",
		       a->src.version[0] ? a->src.version : a->src.path,
		       a->src.build_date[0] ? "" : "", size,
		       a->src.sha256[0] ? "" : " (no checksum)");
		size_text(a->src.required_bytes, size, sizeof(size));
		printf("  Space needed:  %s for LikeOS\n", size);
	}
	if (a->dry_run)
		printf("  %sDry run:%s nothing will be written.\n", ui_bold(),
		       ui_reset());
	if (a->simulate)
		printf("  %sSimulation:%s the disks are image files.\n",
		       ui_bold(), ui_reset());
	ui_blank();
	ui_para("At every question: Enter takes the value in [brackets], "
		"\"back\" returns to the previous step, Ctrl-C leaves setup.");
	ui_blank();
	rc = ui_line("Press Enter to begin", "", buf, sizeof(buf));
	return rc == UI_BACK ? UI_OK : rc;
}

static int st_language(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	const char *items[32];
	int n = il_locale_count(), choice, rc, def;

	if (n > 32)
		n = 32;
	step_header("Language and formats", ST_LANGUAGE);
	ui_para("The language of the installed system:");
	ui_blank();
	for (int i = 0; i < n; i++)
		items[i] = il_locale_label(i);
	def = il_locale_find(c->locale);
	rc = ui_menu("Language", items, n, def >= 0 ? def : 0, &choice);
	if (rc)
		return rc;
	copy_str(c->locale, sizeof(c->locale), il_locale_name(choice));
	ui_blank();
	ui_para("How dates and times are written (for example 11/13/2026 "
		"2:30 PM in the United States, 13/11/2026 14:30 in the United "
		"Kingdom):");
	ui_blank();
	def = il_locale_find(c->time_format[0] ? c->time_format : c->locale);
	rc = ui_menu("Time and date format", items, n, def >= 0 ? def : choice,
		     &choice);
	if (rc)
		return rc;
	copy_str(c->time_format, sizeof(c->time_format), il_locale_name(choice));
	return UI_OK;
}

/* Search the catalogue and let the user pick one; -1 in *pick when the
 * search is to be kept as it was (empty input). */
static int pick_layout(struct app *a, const char *prompt, const char *def_name,
		       int *pick)
{
	char text[96];
	int idx[40], total, rc, num;

	for (;;) {
		rc = ui_line(prompt, def_name ? "" : NULL, text, sizeof(text));
		if (rc)
			return rc;
		if (!text[0]) {
			*pick = -1;
			return UI_OK;
		}
		total = kbd_search(&a->kbd, text, idx, 40);
		if (total == 0) {
			printf("No layout matches \"%s\"; try a language, such as "
			       "German, or a country.\n", text);
			continue;
		}
		if (total == 1) {
			*pick = idx[0];
			printf("  -> %s\n", a->kbd.l[idx[0]].name);
			return UI_OK;
		}
		for (int i = 0; i < total && i < 40; i++)
			printf("  %s%2d%s  %-40s %s%s%s\n", ui_bold(), i + 1,
			       ui_reset(), a->kbd.l[idx[i]].name, ui_dim(),
			       a->kbd.l[idx[i]].language, ui_reset());
		if (total > 40)
			printf("  ... and %d more: type more of the name.\n",
			       total - 40);
		rc = ui_number("Layout number (Enter: search again)", 0,
			       total < 40 ? total : 40, 0, &num);
		if (rc)
			return rc;
		if (num == 0)
			continue;
		*pick = idx[num - 1];
		return UI_OK;
	}
}

static int st_keyboard(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	char cur[256], name[128];
	int pick, rc, more;

	step_header("Keyboard layout", ST_KEYBOARD);
	if (!a->have_kbd) {
		ui_warn("The keyboard catalogue (%s) cannot be read; the layout "
			"stays %s.", KBD_CATALOGUE, c->layouts[0]);
		ui_blank();
		rc = ui_line("Press Enter to go on", "", cur, sizeof(cur));
		return rc;
	}
	cur[0] = '\0';
	for (int i = 0; i < c->nlayouts; i++) {
		kbd_describe(&a->kbd, c->layouts[i], c->variants[i], name,
			     sizeof(name));
		if (i)
			strncat(cur, ", ", sizeof(cur) - strlen(cur) - 1);
		strncat(cur, name, sizeof(cur) - strlen(cur) - 1);
	}
	printf("  Now: %s%s%s\n\n", ui_bold(), cur, ui_reset());
	ui_para("Type part of the name of your keyboard layout or its language "
		"(for example \"german\", \"french\", \"dvorak\") to search, or "
		"press Enter to keep it.");
	ui_blank();
	rc = pick_layout(a, "Keyboard layout", "", &pick);
	if (rc)
		return rc;
	if (pick >= 0) {
		copy_str(c->layouts[0], sizeof(c->layouts[0]),
			 a->kbd.l[pick].layout);
		copy_str(c->variants[0], sizeof(c->variants[0]),
			 a->kbd.l[pick].variant);
		c->nlayouts = 1;
		apply_keyboard(a);
	}
	while (c->nlayouts < IL_MAX_LAYOUTS) {
		ui_blank();
		rc = ui_yes_no("Add another layout to switch to (Alt+Shift "
			       "switches between them)?", 0, &more);
		if (rc)
			return rc == UI_BACK ? UI_BACK : rc;
		if (!more)
			break;
		rc = pick_layout(a, "Additional layout", NULL, &pick);
		if (rc)
			return rc;
		if (pick < 0)
			continue;
		{
			int dup = 0;

			for (int i = 0; i < c->nlayouts; i++)
				if (!strcmp(c->layouts[i], a->kbd.l[pick].layout) &&
				    !strcmp(c->variants[i], a->kbd.l[pick].variant))
					dup = 1;
			if (dup) {
				printf("%s is chosen already.\n",
				       a->kbd.l[pick].name);
				continue;
			}
		}
		copy_str(c->layouts[c->nlayouts], sizeof(c->layouts[0]),
			 a->kbd.l[pick].layout);
		copy_str(c->variants[c->nlayouts], sizeof(c->variants[0]),
			 a->kbd.l[pick].variant);
		c->nlayouts++;
	}
	if (c->nlayouts > 1 && !strstr(c->xkb_options, "grp:")) {
		size_t l = strlen(c->xkb_options);

		snprintf(c->xkb_options + l, sizeof(c->xkb_options) - l, "%s%s",
			 l ? "," : "", "grp:alt_shift_toggle");
	}
	return UI_OK;
}

static int st_disk(struct app *a)
{
	char buf[32];
	int rc, usable = 0;

	for (;;) {
		step_header("Where should LikeOS be installed?", ST_DISK);
		if (scan_disks(a) != 0)
			return UI_CANCEL;
		usable = 0;
		for (int i = 0; i < a->disks->n; i++)
			if (disk_usable(a, &a->disks->disks[i]))
				usable++;
		if (a->disks->n == 0) {
			ui_para("No disk was found.");
		} else {
			print_disk_table(a, 1);
		}
		ui_blank();
		if (!usable) {
			ui_para("No disk can take LikeOS: the installation medium "
				"itself cannot, and a disk has to be writable and "
				"large enough. Connect a disk and type r to look "
				"again.");
			ui_blank();
		}
		rc = ui_line("Disk number (r: look again)", NULL, buf,
			     sizeof(buf));
		if (rc)
			return rc;
		if (!strcasecmp(buf, "r"))
			continue;
		{
			char *end;
			long v = strtol(buf, &end, 10);
			const struct il_disk *d;

			if (!buf[0] || *end || v < 1 || v > a->disks->n) {
				printf("Please type a disk number from the list.\n");
				ui_line("Press Enter", "", buf, sizeof(buf));
				continue;
			}
			d = &a->disks->disks[v - 1];
			if (!disk_usable(a, d)) {
				char need[32];

				size_text(a->src.required_bytes +
						  IL_ESP_SIZE_BYTES,
					  need, sizeof(need));
				ui_error("%s cannot be used: %s%s%s.", d->dev,
					 disk_note(a, d),
					 disk_too_small(a, d) ? ", LikeOS needs " :
								"",
					 disk_too_small(a, d) ? need : "");
				ui_line("Press Enter", "", buf, sizeof(buf));
				continue;
			}
			if (a->disk != (int)v - 1 || !a->plan) {
				il_plan_free(a->plan);
				a->plan = il_plan_new(a->ctx, d);
				if (!a->plan) {
					ui_error("No plan: %s",
						 il_last_error(a->ctx));
					return UI_CANCEL;
				}
				a->disk = (int)v - 1;
				a->whole_disk = 0;
			}
			return UI_OK;
		}
	}
}

/* A fresh plan for the chosen disk (the edits so far dropped). */
static int fresh_plan(struct app *a)
{
	il_plan_free(a->plan);
	a->plan = il_plan_new(a->ctx, &a->disks->disks[a->disk]);
	a->whole_disk = 0;
	if (!a->plan) {
		ui_error("No plan: %s", il_last_error(a->ctx));
		return -1;
	}
	return 0;
}

static void show_plan(struct app *a)
{
	char line[256];
	int n = il_plan_nops(a->plan);

	if (il_plan_view(a->plan, a->view) == 0)
		print_partitions(a->view, il_plan_root(a->plan),
				 il_plan_esp(a->plan));
	ui_blank();
	if (n == 0) {
		printf("  %sPlanned changes:%s none yet\n", ui_bold(), ui_reset());
	} else {
		printf("  %sPlanned changes:%s\n", ui_bold(), ui_reset());
		for (int i = 0; i < n; i++) {
			il_op_describe(il_plan_op(a->plan, i), line, sizeof(line));
			printf("    %d. %s\n", i + 1, line);
		}
	}
}

static void manual_help(void)
{
	printf("\n  Commands (nothing is written until the end of setup):\n"
	       "    root N         install LikeOS on partition N (it is "
	       "formatted)\n"
	       "    new F [SIZE]   make a partition in free space F (SIZE like "
	       "40G or 500M,\n"
	       "                   all of it when left out)\n"
	       "    delete N       delete partition N\n"
	       "    extend N SIZE  grow partition N to SIZE into the space "
	       "after it\n"
	       "    format N       format partition N\n"
	       "    erase          a new, empty partition table (erases the "
	       "whole disk)\n"
	       "    whole          use the whole disk the recommended way\n"
	       "    undo           forget every change\n"
	       "    done           go on with this layout\n"
	       "    back           choose between whole disk and by hand "
	       "again\n\n");
}

static int manual_partitioning(struct app *a)
{
	char cmd[128], why[IL_ERROR_MAX];
	int rc;

	for (;;) {
		step_header("Partitions (by hand)", ST_PARTITION);
		printf("  %s\n\n", a->disks->disks[a->disk].dev);
		show_plan(a);
		manual_help();
		rc = ui_line("Command", NULL, cmd, sizeof(cmd));
		if (rc)
			return rc;
		ui_error_shown = 0;
		{
			char word[32] = "", arg1[64] = "", arg2[64] = "";
			int n = sscanf(cmd, "%31s %63s %63s", word, arg1, arg2);
			int num = atoi(arg1);
			const char *f1 = arg1[0] == 'F' || arg1[0] == 'f' ?
						 arg1 + 1 : arg1;

			if (n <= 0)
				continue;
			rc = 0;
			if (!strcasecmp(word, "done")) {
				if (il_plan_root(a->plan) < 1) {
					ui_error("Choose the partition for LikeOS "
						 "first (root N, or new F to make "
						 "one).");
				} else if (il_plan_validate(a->plan,
						a->have_src ? &a->src : NULL,
						why, sizeof(why)) != 0) {
					ui_error("%s", why);
				} else {
					return UI_OK;
				}
			} else if (!strcasecmp(word, "root") && n >= 2) {
				rc = il_plan_select_root(a->plan, num);
			} else if (!strcasecmp(word, "new") && n >= 2) {
				long long sz = n >= 3 ? parse_size(arg2) : 0;
				int partno = -1;

				if (sz < 0) {
					ui_error("\"%s\" is not a size (examples: "
						 "40G, 500M, 1.5T).", arg2);
				} else {
					rc = il_plan_create(a->plan, atoi(f1) - 1,
							    (uint64_t)sz, &partno);
					if (rc == 0 && partno > 0) {
						int yes = 0;

						printf("Partition %d will be made.\n",
						       partno);
						if (ui_yes_no("Install LikeOS on it?",
							      1, &yes) == UI_OK &&
						    yes)
							rc = il_plan_select_root(
								a->plan, partno);
					}
				}
			} else if (!strcasecmp(word, "delete") && n >= 2) {
				rc = il_plan_delete(a->plan, num);
			} else if (!strcasecmp(word, "extend") && n >= 3) {
				long long sz = parse_size(arg2);

				if (sz <= 0)
					ui_error("\"%s\" is not a size.", arg2);
				else
					rc = il_plan_extend(a->plan, num,
							    (uint64_t)sz);
			} else if (!strcasecmp(word, "format") && n >= 2) {
				rc = il_plan_format(a->plan, num);
			} else if (!strcasecmp(word, "erase")) {
				rc = il_plan_new_table(a->plan);
			} else if (!strcasecmp(word, "whole")) {
				if (fresh_plan(a) != 0)
					return UI_CANCEL;
				rc = il_plan_use_whole_disk(a->plan);
				if (rc == 0) {
					a->whole_disk = 1;
					return UI_OK;
				}
			} else if (!strcasecmp(word, "undo")) {
				if (fresh_plan(a) != 0)
					return UI_CANCEL;
			} else if (!strcasecmp(word, "help") || !strcmp(word, "?")) {
				continue;
			} else {
				ui_error("Unknown command \"%s\" (or a number is "
					 "missing).", cmd);
			}
			if (rc)
				ui_error("%s", il_last_error(a->ctx));
			if (ui_error_shown) {
				/* Let it be read before the screen is drawn
				 * again. */
				char dummy[8];

				ui_error_shown = 0;
				rc = ui_line("Press Enter", "", dummy,
					     sizeof(dummy));
				if (rc == UI_CANCEL)
					return rc;
			}
		}
	}
}

static int st_partition(struct app *a)
{
	static const char *const items[] = {
		"Erase the whole disk and install LikeOS (recommended)",
		"Choose or make the partition for LikeOS by hand",
	};
	const struct il_disk *d = &a->disks->disks[a->disk];
	char why[IL_ERROR_MAX], size[32], buf[8];
	int choice, rc;

	for (;;) {
		step_header("How should the disk be used?", ST_PARTITION);
		size_text(d->size_bytes, size, sizeof(size));
		printf("  %s%s%s  %s, %s, partition table: %s\n\n", ui_bold(),
		       d->dev, ui_reset(), d->model[0] ? d->model : d->name,
		       size, table_name(d->table));
		print_partitions(d, -1, -1);
		ui_blank();
		rc = ui_menu("Choice", items, 2, a->whole_disk || !il_plan_nops(
								  a->plan) ? 0 : 1,
			     &choice);
		if (rc)
			return rc;
		if (choice == 0) {
			if (fresh_plan(a) != 0)
				return UI_CANCEL;
			rc = il_plan_use_whole_disk(a->plan);
			if (rc) {
				ui_error("%s", il_last_error(a->ctx));
				ui_line("Press Enter", "", buf, sizeof(buf));
				continue;
			}
			a->whole_disk = 1;
		} else {
			if (a->whole_disk && fresh_plan(a) != 0)
				return UI_CANCEL;
			rc = manual_partitioning(a);
			if (rc == UI_BACK)
				continue;
			if (rc)
				return rc;
		}
		if (il_plan_validate(a->plan, a->have_src ? &a->src : NULL, why,
				     sizeof(why)) != 0) {
			ui_error("%s", why);
			ui_line("Press Enter", "", buf, sizeof(buf));
			continue;
		}
		return UI_OK;
	}
}

static int st_timezone(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	char text[96], label[160];
	int cur = il_timezone_find(c->timezone), rc;

	step_header("Time zone", ST_TIMEZONE);
	copy_str(label, sizeof(label), cur >= 0 ? il_timezone_label(cur) :
						 c->timezone);
	printf("  Now: %s%s%s\n\n", ui_bold(), label, ui_reset());
	ui_para("Type part of the name of a city, a region or a country's "
		"capital (for example \"berlin\", \"new york\", \"tokyo\", "
		"\"utc\") to search, or press Enter to keep it.");
	ui_blank();
	for (;;) {
		int idx[60], k = 0, total = 0, num;

		rc = ui_line("Time zone", "", text, sizeof(text));
		if (rc)
			return rc;
		if (!text[0])
			return il_timezone_find(c->timezone) >= 0 ? UI_OK : UI_OK;
		for (int i = 0; i < il_timezone_count(); i++) {
			const char *n = il_timezone_name(i), *l = il_timezone_label(i);
			char alt[96];

			/* "new york" finds America/New_York */
			copy_str(alt, sizeof(alt), text);
			for (char *p = alt; *p; p++)
				if (*p == ' ')
					*p = '_';
			if (strcasestr(n, text) || strcasestr(l, text) ||
			    strcasestr(n, alt)) {
				if (!strcasecmp(n, text) && k < 60) {
					/* an exact name first */
					memmove(idx + 1, idx, sizeof(int) * (size_t)k);
					idx[0] = i;
					k++;
				} else if (k < 60) {
					idx[k++] = i;
				}
				total++;
			}
		}
		if (total == 0) {
			printf("No time zone matches \"%s\".\n", text);
			continue;
		}
		if (total == 1) {
			copy_str(c->timezone, sizeof(c->timezone),
				 il_timezone_name(idx[0]));
			printf("  -> %s\n", il_timezone_label(idx[0]));
			return UI_OK;
		}
		for (int i = 0; i < k; i++)
			printf("  %s%2d%s  %-36s %s%s%s\n", ui_bold(), i + 1,
			       ui_reset(), il_timezone_label(idx[i]), ui_dim(),
			       il_timezone_name(idx[i]), ui_reset());
		if (total > k)
			printf("  ... and %d more: type more of the name.\n",
			       total - k);
		rc = ui_number("Time zone number (Enter: search again)", 0, k, 0,
			       &num);
		if (rc)
			return rc;
		if (num == 0)
			continue;
		copy_str(c->timezone, sizeof(c->timezone),
			 il_timezone_name(idx[num - 1]));
		return UI_OK;
	}
}

static int st_hostname(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	char buf[128], why[256];
	int rc;

	step_header("Name this computer", ST_HOSTNAME);
	ui_para("The name other computers on the network know this one by: "
		"letters, digits and hyphens, at most %d characters, not "
		"starting or ending with a hyphen.", IL_HOSTNAME_MAX);
	ui_blank();
	if (!c->hostname[0])
		il_suggest_hostname(c->hostname, sizeof(c->hostname));
	for (;;) {
		rc = ui_line("Computer name", c->hostname, buf, sizeof(buf));
		if (rc)
			return rc;
		if (il_validate_hostname(buf, why, sizeof(why)) != 0) {
			ui_error("%s", why);
			continue;
		}
		copy_str(c->hostname, sizeof(c->hostname), buf);
		return UI_OK;
	}
}

/* A password with its confirmation; UI_BACK when "<" is typed. */
static int ask_password(const char *what, char *out, size_t len)
{
	char p1[IL_PASSWORD_MAX + 2], p2[IL_PASSWORD_MAX + 2], why[256];
	char warn[256], prompt[160];
	int rc, s, ok;

	for (;;) {
		snprintf(prompt, sizeof(prompt), "%s", what);
		rc = ui_secret(prompt, p1, sizeof(p1));
		if (rc)
			return rc;
		if (!strcmp(p1, "<"))
			return UI_BACK;
		if (il_validate_password(p1, why, sizeof(why)) != 0) {
			ui_error("%s", why);
			continue;
		}
		/* Valid, but maybe with a word of warning (too short). */
		copy_str(warn, sizeof(warn), why);
		rc = ui_secret("Type it again", p2, sizeof(p2));
		if (rc) {
			memset(p1, 0, sizeof(p1));
			return rc;
		}
		if (strcmp(p1, p2) != 0) {
			ui_error("The two passwords differ; please type them "
				 "again.");
			memset(p1, 0, sizeof(p1));
			memset(p2, 0, sizeof(p2));
			continue;
		}
		memset(p2, 0, sizeof(p2));
		s = il_password_strength(p1);
		printf("  Strength: %s%s%s\n", ui_bold(), strength_name(s),
		       ui_reset());
		if (warn[0])
			ui_warn("%s.", warn);
		if (s <= 1 || warn[0]) {
			rc = ui_yes_no(s <= 1 ? "This password is easy to "
				       "guess. Use it anyway?" :
				       "Use it anyway?", 0, &ok);
			if (rc) {
				memset(p1, 0, sizeof(p1));
				return rc;
			}
			if (!ok) {
				memset(p1, 0, sizeof(p1));
				continue;
			}
		}
		copy_str(out, len, p1);
		memset(p1, 0, sizeof(p1));
		return UI_OK;
	}
}

static int st_rootpw(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	int rc;

	step_header("Password of the administrator (root)", ST_ROOTPW);
	ui_para("root may change everything on this computer. Choose a "
		"password that is hard to guess and that you will remember. "
		"Nothing is shown while you type. (\"<\" alone goes back.)");
	ui_blank();
	rc = ask_password("Root password", c->root_password,
			  sizeof(c->root_password));
	if (rc == UI_OK)
		c->root_password_hash[0] = '\0';
	return rc;
}

static int st_user(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	char buf[IL_FULLNAME_MAX + 2], sugg[IL_USERNAME_MAX + 2], why[256];
	int rc, want;

	step_header("Your account", ST_USER);
	ui_para("An account for everyday use, so that root is needed only for "
		"changing the system. It is a member of the groups audio, "
		"render and users.");
	ui_blank();
	rc = ui_yes_no("Create an account?", 1, &want);
	if (rc)
		return rc;
	if (!want) {
		memset(&c->user, 0, sizeof(c->user));
		c->user_password_hash[0] = '\0';
		return UI_OK;
	}
	for (;;) {
		rc = ui_line("Your full name", c->user.fullname[0] ?
			     c->user.fullname : "", buf, sizeof(buf));
		if (rc)
			return rc;
		if (strchr(buf, ':')) {
			ui_error("The name cannot contain a colon.");
			continue;
		}
		copy_str(c->user.fullname, sizeof(c->user.fullname), buf);
		break;
	}
	il_suggest_username(c->user.fullname, sugg, sizeof(sugg));
	for (;;) {
		rc = ui_line("User name (to log in with)",
			     c->user.username[0] ? c->user.username :
			     sugg[0] ? sugg : NULL, buf, sizeof(buf));
		if (rc)
			return rc;
		if (il_validate_username(buf, why, sizeof(why)) != 0) {
			ui_error("%s", why);
			continue;
		}
		if (!strcmp(buf, "root")) {
			ui_error("root is the administrator already; choose "
				 "another name.");
			continue;
		}
		copy_str(c->user.username, sizeof(c->user.username), buf);
		break;
	}
	ui_blank();
	rc = ask_password("Password", c->user.password,
			  sizeof(c->user.password));
	if (rc == UI_OK)
		c->user_password_hash[0] = '\0';
	return rc;
}

static void print_summary(struct app *a)
{
	struct il_config *c = &a->ans.cfg;
	char buf[4096], name[128], kb[256] = "";
	int tz = il_timezone_find(c->timezone);
	int li = il_locale_find(c->locale), fi = il_locale_find(c->time_format);

	il_plan_summary(a->plan, buf, sizeof(buf));
	printf("  %sDisk%s\n", ui_bold(), ui_reset());
	for (char *s = buf, *nl; *s; s = nl + 1) {
		nl = strchr(s, '\n');
		if (!nl) {
			printf("    %s\n", s);
			break;
		}
		*nl = '\0';
		printf("    %s\n", s);
	}
	for (int i = 0; i < c->nlayouts; i++) {
		if (a->have_kbd)
			kbd_describe(&a->kbd, c->layouts[i], c->variants[i],
				     name, sizeof(name));
		else
			snprintf(name, sizeof(name), "%s%s%s%s", c->layouts[i],
				 c->variants[i][0] ? "(" : "", c->variants[i],
				 c->variants[i][0] ? ")" : "");
		if (i)
			strncat(kb, ", ", sizeof(kb) - strlen(kb) - 1);
		strncat(kb, name, sizeof(kb) - strlen(kb) - 1);
	}
	printf("\n  %sSettings%s\n", ui_bold(), ui_reset());
	printf("    Language:       %s\n", li >= 0 ? il_locale_label(li) :
						       c->locale);
	printf("    Formats:        %s\n", fi >= 0 ? il_locale_label(fi) :
						       c->time_format);
	printf("    Keyboard:       %s\n", kb);
	printf("    Time zone:      %s\n", tz >= 0 ? il_timezone_label(tz) :
						      c->timezone);
	printf("    Computer name:  %s\n", c->hostname);
	printf("    Accounts:       root");
	if (c->user.username[0])
		printf(", %s%s%s%s", c->user.username,
		       c->user.fullname[0] ? " (" : "", c->user.fullname,
		       c->user.fullname[0] ? ")" : "");
	printf("\n");
	if (a->have_src) {
		char size[32];

		size_text(a->src.uncompressed_bytes, size, sizeof(size));
		printf("    System image:   %s%s (%s)\n",
		       a->src.version[0] ? "LikeOS " : a->src.path,
		       a->src.version, size);
	}
}

/* "Type YES": UI_OK when confirmed. */
static int confirm_erase(struct app *a)
{
	const struct il_disk *d = il_plan_disk(a->plan);
	char buf[32];
	int rc;

	if (a->yes || a->dry_run)
		return UI_OK;
	ui_blank();
	if (a->whole_disk)
		printf("  %sEverything on %s (%s) will be erased.%s\n", ui_bold(),
		       d->dev, d->model[0] ? d->model : d->name, ui_reset());
	else
		printf("  %sThe partitions listed above on %s are changed and "
		       "formatted; what they hold is lost.%s\n", ui_bold(),
		       d->dev, ui_reset());
	for (;;) {
		rc = ui_line("Type YES to install LikeOS now", NULL, buf,
			     sizeof(buf));
		if (rc)
			return rc;
		if (!strcmp(buf, "YES"))
			return UI_OK;
		if (!strcasecmp(buf, "yes"))
			printf("Please type YES in capital letters.\n");
		else
			printf("Not confirmed. Type YES to install, \"back\" to "
			       "change something, or press Ctrl-C to leave.\n");
	}
}

static int st_summary(struct app *a)
{
	char why[IL_ERROR_MAX];

	step_header("Ready to install", ST_SUMMARY);
	print_summary(a);
	if (il_config_validate(&a->ans.cfg, why, sizeof(why)) != 0) {
		char buf[8];

		ui_blank();
		ui_error("%s", why);
		ui_line("Press Enter to go back", "", buf, sizeof(buf));
		return UI_BACK;
	}
	return confirm_erase(a);
}

/* ---- installing ----------------------------------------------------------------- */

static void on_sigint_install(int sig)
{
	(void)sig;
	ui_interrupted++;
	if (g_sig_ctx && il_can_cancel(g_sig_ctx))
		il_cancel(g_sig_ctx);
}

/* Ctrl-C while installing: the terminal is put in a mode where it does not
 * turn the key into a signal -- a signal would also reach mkfs and the
 * other helpers the backend runs, which share the terminal, and kill them
 * half-way.  The key is read here instead. */
static void tty_install_mode(struct app *a, int on)
{
	if (!ui_is_tty())
		return;
	if (on) {
		struct termios t;

		if (tcgetattr(0, &a->tty_saved) != 0)
			return;
		t = a->tty_saved;
		t.c_lflag &= ~(tcflag_t)(ISIG | ICANON | ECHO);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		if (tcsetattr(0, TCSANOW, &t) == 0)
			a->raw_tty = 1;
	} else if (a->raw_tty) {
		tcsetattr(0, TCSAFLUSH, &a->tty_saved);
		a->raw_tty = 0;
	}
}

static void poll_cancel_key(struct app *a)
{
	struct pollfd pfd;
	char buf[64];
	ssize_t n;

	if (!a->raw_tty)
		return;
	pfd.fd = 0;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
		return;
	n = read(0, buf, sizeof(buf));
	for (ssize_t i = 0; i < n; i++)
		if ((unsigned char)buf[i] == a->tty_saved.c_cc[VINTR] ||
		    buf[i] == 3) {
			ui_interrupted++;
			if (il_can_cancel(a->ctx))
				il_cancel(a->ctx);
		}
}

static void on_progress(const struct il_progress *p, void *user)
{
	struct app *a = user;
	const char *title = il_step_title(p->step);

	poll_cancel_key(a);
	if (ui_interrupted) {
		if (il_can_cancel(a->ctx)) {
			ui_progress_clear(&a->prog);
			if (!a->refused_said) {
				printf("Stopping the installation; nothing has "
				       "been written to the disk.\n");
				a->refused_said = 1;
			}
		} else if (a->refused_said < 2) {
			ui_progress_clear(&a->prog);
			printf("The disk is being written now: the "
			       "installation cannot be stopped any more. "
			       "Please wait until it has finished.\n");
			a->refused_said = 2;
		}
		ui_interrupted = 0;
	}
	if (a->last_step >= 0 && p->step != a->last_step)
		for (int s = a->last_step; s < p->step; s++)
			ui_progress_step_done(&a->prog, il_step_title(s));
	a->last_step = p->step;
	if (p->step == IL_STEP_EXTRACT && p->files_total) {
		/* files, not bytes: the big files come first, so the bytes
		 * are all written long before the last file */
		char done[32], total[32], counts[80];

		ui_group_digits(p->files_done, done, sizeof(done));
		ui_group_digits(p->files_total, total, sizeof(total));
		snprintf(counts, sizeof(counts), "%s of %s files", done, total);
		ui_progress(&a->prog, p->step, IL_STEP_COUNT, title, p->percent,
			    counts, p->detail, p->eta_seconds);
	} else {
		ui_progress(&a->prog, p->step, IL_STEP_COUNT, title, p->percent,
			    NULL, p->detail, p->eta_seconds);
	}
}

static int run_install(struct app *a)
{
	struct sigaction sa, old_int, old_hup;
	int rc;

	if (a->save_answers_path) {
		const struct il_disk *d = il_plan_disk(a->plan);

		copy_str(a->ans.disk, sizeof(a->ans.disk), d->dev);
		a->ans.whole_disk = a->whole_disk;
		a->ans.root_partno = a->whole_disk ? -1 : il_plan_root(a->plan);
		if (il_answers_save(a->ctx, a->save_answers_path, &a->ans, 0) != 0)
			ui_warn("The answers were not saved: %s",
				il_last_error(a->ctx));
		else
			ui_info("The answers were saved to %s%s.",
				a->save_answers_path,
				a->whole_disk ? "" :
				" (the partition chosen by hand is recorded as "
				"root_partition; partitions made by hand are "
				"not part of an answer file)");
	}

	ui_blank();
	printf("%s%s LikeOS on %s%s\n\n", ui_bold(),
	       a->dry_run ? "Checking the installation of" : "Installing",
	       il_plan_disk(a->plan)->dev, ui_reset());
	fflush(stdout);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigint_install;
	sigemptyset(&sa.sa_mask);
	/* The backend's reads and writes must not fail with EINTR. */
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, &old_int);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, &old_hup);
	ui_interrupted = 0;
	g_sig_ctx = a->ctx;
	tty_install_mode(a, 1);

	ui_progress_begin(&a->prog);
	a->last_step = -1;
	a->refused_said = 0;
	a->installing = 1;
	rc = il_install(a->ctx, a->plan, &a->ans.cfg, on_progress, a);
	a->installing = 0;
	if (rc == 0 && a->last_step >= 0)
		ui_progress_step_done(&a->prog, il_step_title(a->last_step));
	ui_progress_end(&a->prog);

	tty_install_mode(a, 0);
	g_sig_ctx = NULL;
	sigaction(SIGINT, &old_int, NULL);
	sigaction(SIGHUP, &old_hup, NULL);
	ui_interrupted = 0;
	return rc;
}

/* After the installation: the outcome, and the restart. */
enum { REBOOT_ASK, REBOOT_YES, REBOOT_NO };

static int finish(struct app *a, int rc, int reboot_mode)
{
	if (rc == -ECANCELED) {
		ui_blank();
		ui_info("Setup was cancelled. Nothing was written to the disk.");
		return EXIT_CANCELLED;
	}
	if (rc != 0) {
		ui_blank();
		ui_error("The installation failed: %s", il_last_error(a->ctx));
		ui_info("The log with every detail is %s.", il_log_path(a->ctx));
		return EXIT_FAILED;
	}
	ui_blank();
	if (a->dry_run) {
		ui_info("Dry run finished: the plan and the settings are valid. "
			"Nothing was written.");
		return EXIT_INSTALLED;
	}
	printf("%sLikeOS is installed on %s.%s\n", ui_bold(),
	       il_plan_disk(a->plan)->dev, ui_reset());
	ui_info("Remove the installation medium before the computer starts "
		"again, or choose the disk in the firmware's boot menu.");
	if (a->no_reboot || a->simulate || reboot_mode == REBOOT_NO)
		return EXIT_INSTALLED;
	if (reboot_mode == REBOOT_ASK) {
		int yes = 1;

		ui_blank();
		if (ui_yes_no("Restart the computer now?", 1, &yes) != UI_OK ||
		    !yes)
			return EXIT_INSTALLED;
	}
	ui_info("Restarting ...");
	fflush(stdout);
	ui_restore();
	if (il_reboot(a->ctx) != 0)
		ui_error("Restarting failed: %s", il_last_error(a->ctx));
	return EXIT_INSTALLED;
}

/* ---- the modes -------------------------------------------------------------------- */

static int mode_list_disks(struct app *a)
{
	if (scan_disks(a) != 0)
		return EXIT_FAILED;
	if (a->disks->n == 0) {
		printf("No disks.\n");
		return EXIT_INSTALLED;
	}
	print_disk_table(a, 1);
	for (int i = 0; i < a->disks->n; i++) {
		const struct il_disk *d = &a->disks->disks[i];
		char size[32];

		size_text(d->size_bytes, size, sizeof(size));
		printf("\n%s%s%s  %s, %s, %u-byte sectors, partition table %s%s%s\n",
		       ui_bold(), d->dev, ui_reset(),
		       d->model[0] ? d->model : d->name, size, d->sector_size,
		       table_name(d->table), d->disk_guid[0] ? " " : "",
		       d->disk_guid);
		print_partitions(d, -1, -1);
	}
	return EXIT_INSTALLED;
}

static int mode_answers(struct app *a)
{
	char why[IL_ERROR_MAX];
	int rc;

	if (il_answers_load(a->ctx, a->answers_path, &a->ans) != 0) {
		ui_error("%s", il_last_error(a->ctx));
		return EXIT_USAGE;
	}
	if (il_config_validate(&a->ans.cfg, why, sizeof(why)) != 0) {
		ui_error("%s: %s", a->answers_path, why);
		return EXIT_USAGE;
	}
	if (scan_disks(a) != 0)
		return EXIT_FAILED;
	if (il_answers_plan(a->ctx, &a->ans, a->disks, &a->plan) != 0) {
		ui_error("%s", il_last_error(a->ctx));
		if (!strcmp(a->ans.disk, "auto") || !a->ans.disk[0]) {
			printf("\n");
			print_disk_table(a, 0);
		}
		return EXIT_FAILED;
	}
	a->whole_disk = a->ans.whole_disk;
	if (il_plan_validate(a->plan, a->have_src ? &a->src : NULL, why,
			     sizeof(why)) != 0) {
		ui_error("%s", why);
		return EXIT_FAILED;
	}
	printf("%sLikeOS Setup%s -- unattended installation from %s\n\n",
	       ui_bold(), ui_reset(), a->answers_path);
	print_summary(a);
	if (!a->yes && !a->dry_run) {
		if (!ui_is_tty()) {
			ui_error("An unattended installation erases the disk "
				 "without asking only with --yes.");
			return EXIT_USAGE;
		}
		rc = confirm_erase(a);
		if (rc != UI_OK) {
			ui_info("Nothing was written.");
			return EXIT_CANCELLED;
		}
	}
	rc = run_install(a);
	return finish(a, rc, a->ans.reboot ? REBOOT_YES : REBOOT_NO);
}

static int mode_wizard(struct app *a)
{
	static int (*const steps[ST_COUNT])(struct app *) = {
		st_welcome, st_language, st_keyboard, st_disk, st_partition,
		st_timezone, st_hostname, st_rootpw, st_user, st_summary,
	};
	int step = 0, rc;

	if (!a->have_src) {
		ui_error("%s", il_last_error(a->ctx));
		ui_info("The system image LikeOS is installed from is missing; "
			"this medium cannot install LikeOS. (--source names "
			"another one.)");
		return EXIT_CANNOT;
	}
	while (step < ST_COUNT) {
		rc = steps[step](a);
		if (rc == UI_OK) {
			step++;
		} else if (rc == UI_BACK) {
			if (step > 0)
				step--;
		} else {
			ui_restore();
			ui_blank();
			ui_info("Setup was cancelled. Nothing was written to any "
				"disk.");
			return EXIT_CANCELLED;
		}
	}
	rc = run_install(a);
	return finish(a, rc, REBOOT_ASK);
}

/* ---- main ------------------------------------------------------------------------- */

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: " PROG " [options]\n"
		"\n"
		"Install LikeOS on a disk of this computer from a text terminal.\n"
		"Without options it asks everything it needs to know, shows a\n"
		"summary and installs once that is confirmed with YES.\n"
		"\n"
		"Options:\n"
		"  -a, --answers FILE       install unattended from an answer file\n"
		"                           (likeos-install.conf(5))\n"
		"  -s, --save-answers FILE  write the choices to an answer file\n"
		"                           (passwords as hashes)\n"
		"  -l, --list-disks         show the disks and their partitions\n"
		"  -n, --dry-run            plan and check everything, write nothing\n"
		"  -y, --yes                do not ask for the YES confirmation\n"
		"  -R, --no-reboot          do not restart when done\n"
		"      --source PATH        the system image (default %s)\n"
		"      --target DIR         where the new system is mounted while\n"
		"                           installing (default %s)\n"
		"      --log FILE           the log (default %s)\n"
		"      --simulate IMG       use the image file IMG as a disk (may be\n"
		"                           given several times; for testing)\n"
		"  -v, --verbose            show the backend's log (twice: debug lines)\n"
		"      --reset-tty          put the terminal on standard input back\n"
		"                           into line mode with echo, and exit\n"
		"  -h, --help               this text\n"
		"  -V, --version            the version\n"
		"\n"
		"Exit status: 0 installed (or listed, or the dry run passed, or\n"
		"the terminal reset), 1 wrong usage, 2 the installation failed,\n"
		"3 cancelled before anything was written, 4 cannot start.\n",
		IL_DEFAULT_SOURCE, IL_DEFAULT_TARGET, IL_DEFAULT_LOG);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "answers", required_argument, NULL, 'a' },
		{ "save-answers", required_argument, NULL, 's' },
		{ "list-disks", no_argument, NULL, 'l' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "yes", no_argument, NULL, 'y' },
		{ "no-reboot", no_argument, NULL, 'R' },
		{ "source", required_argument, NULL, 1 },
		{ "target", required_argument, NULL, 2 },
		{ "log", required_argument, NULL, 3 },
		{ "simulate", required_argument, NULL, 4 },
		{ "reset-tty", no_argument, NULL, 5 },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};
	struct app *a = &g_app;
	struct il_options o;
	int c, rc, reset_tty = 0;

	memset(a, 0, sizeof(*a));
	a->disk = -1;
	while ((c = getopt_long(argc, argv, "a:s:lnyRvhV", longopts, NULL)) !=
	       -1) {
		switch (c) {
		case 'a':
			a->answers_path = optarg;
			break;
		case 's':
			a->save_answers_path = optarg;
			break;
		case 'l':
			a->list_disks = 1;
			break;
		case 'n':
			a->dry_run = 1;
			break;
		case 'y':
			a->yes = 1;
			break;
		case 'R':
			a->no_reboot = 1;
			break;
		case 1:
			a->source = optarg;
			break;
		case 2:
			a->target = optarg;
			break;
		case 3:
			a->log_path = optarg;
			break;
		case 4:
			if (a->nsim >= MAX_SIM) {
				fprintf(stderr, PROG ": at most %d --simulate "
					"images\n", MAX_SIM);
				return EXIT_USAGE;
			}
			a->sim[a->nsim++] = optarg;
			a->simulate = 1;
			break;
		case 5:
			reset_tty = 1;
			break;
		case 'v':
			a->verbose++;
			break;
		case 'h':
			usage(stdout);
			return EXIT_INSTALLED;
		case 'V':
			printf(PROG " %s (LikeOS installer backend %s)\n",
			       CLI_VERSION, il_version());
			return EXIT_INSTALLED;
		default:
			usage(stderr);
			return EXIT_USAGE;
		}
	}
	if (optind != argc) {
		fprintf(stderr, PROG ": unexpected argument \"%s\"\n",
			argv[optind]);
		usage(stderr);
		return EXIT_USAGE;
	}
	if (reset_tty) {
		/* For the installer session: a likeos-install that was
		 * killed while installing leaves the console without echo
		 * and line mode. */
		if (ui_reset_tty() != 0) {
			fprintf(stderr, PROG ": --reset-tty: standard input is "
				"not a terminal (%s)\n", strerror(errno));
			return EXIT_CANNOT;
		}
		return EXIT_INSTALLED;
	}
	if (!a->simulate && !a->dry_run && !a->list_disks && geteuid() != 0) {
		fprintf(stderr, PROG ": installing needs root (it partitions "
			"and formats a disk); --dry-run and --list-disks work "
			"without\n");
		return EXIT_CANNOT;
	}
	if (!a->answers_path && !a->list_disks && !isatty(0) &&
	    !getenv("LIKEOS_INSTALL_PIPE")) {
		/* Neither end is a terminal: nobody to ask.  (Driving the
		 * questions from a program sets LIKEOS_INSTALL_PIPE=1.) */
		fprintf(stderr, PROG ": no terminal to ask on; use --answers "
			"FILE for an unattended installation\n");
		return EXIT_CANNOT;
	}

	ui_init(!a->answers_path && !a->list_disks);

	a->disks = calloc(1, sizeof(*a->disks));
	a->view = calloc(1, sizeof(*a->view));
	if (!a->disks || !a->view) {
		fprintf(stderr, PROG ": out of memory\n");
		return EXIT_CANNOT;
	}
	memset(&o, 0, sizeof(o));
	o.source = a->source;
	o.target = a->target;
	o.log_path = a->log_path;
	o.dry_run = a->dry_run;
	o.simulate = a->simulate;
	a->sim[a->nsim] = NULL;
	o.simulate_disks = a->simulate ? a->sim : NULL;
	o.verbose = 0; /* shown through on_log, not straight to stderr */
	o.log_cb = on_log;
	o.log_user = a;
	a->ctx = il_ctx_new(&o);
	if (!a->ctx) {
		fprintf(stderr, PROG ": the installer backend did not start\n");
		return EXIT_CANNOT;
	}
	il_log(a->ctx, IL_LOG_INFO, PROG " %s%s%s%s", CLI_VERSION,
	       a->answers_path ? ", answers from " : ", interactive",
	       a->answers_path ? a->answers_path : "",
	       a->dry_run ? ", dry run" : "");

	il_config_defaults(&a->ans.cfg);
	a->ans.whole_disk = 1;
	a->ans.reboot = 1;
	a->ans.root_partno = -1;
	copy_str(a->ans.disk, sizeof(a->ans.disk), "auto");
	if (!a->simulate)
		live_defaults(a);
	a->have_kbd = kbd_load(&a->kbd, NULL) == 0;

	if (!a->list_disks)
		a->have_src = il_source_probe(a->ctx, &a->src) == 0;

	if (a->list_disks)
		rc = mode_list_disks(a);
	else if (a->answers_path)
		rc = a->have_src ? mode_answers(a) :
		     (ui_error("%s", il_last_error(a->ctx)), EXIT_CANNOT);
	else
		rc = mode_wizard(a);

	ui_restore();
	il_plan_free(a->plan);
	kbd_free(&a->kbd);
	/* the passwords */
	memset(&a->ans, 0, sizeof(a->ans));
	il_ctx_free(a->ctx);
	free(a->disks);
	free(a->view);
	return rc;
}
