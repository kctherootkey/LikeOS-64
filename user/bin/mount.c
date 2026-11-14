/*
 * mount - mount filesystems, or list what is mounted.
 *
 *   mount [-l] [-t type]                    list the mounted filesystems
 *   mount -a [-t types] [-O opts] [-fvn]    mount everything in /etc/fstab
 *   mount [options] source target           mount source on target
 *   mount [options] target | source         the line of /etc/fstab for it
 *   mount -o remount[,opts] target          change a mounted filesystem's
 *                                           flags (ro <-> rw ...)
 *
 * The source is a block device (/dev/sda1), a tag that names one
 * (UUID=..., LABEL=..., PARTUUID=..., PARTLABEL=..., looked up in
 * /dev/disk/by-*), or a word for a filesystem that has no device
 * ("tmpfs").  Without -t a device is probed for its type.
 *
 * Options:
 *   -a, --all            mount every /etc/fstab line not marked noauto
 *   -t, --types LIST     the filesystem type; with -a or a listing, the
 *                        types to include (a "no" prefix excludes: notmpfs)
 *   -O, --test-opts LIST with -a, only lines having these options
 *   -o, --options LIST   mount options (comma-separated)
 *   -r, -R, --read-only  the same as -o ro
 *   -w, --rw             the same as -o rw
 *   -f, --fake           do everything but the mount itself
 *   -n, --no-mtab        accepted; there is no /etc/mtab to update
 *   -v, --verbose        say what is done
 *   -l, --show-labels    accepted with a listing
 *   -h, --help, -V, --version
 *
 * The generic options are ro, rw, defaults, (no)suid, (no)dev, (no)exec,
 * sync, async, dirsync, (no)atime, (no)diratime, relatime, strictatime,
 * lazytime, nosymfollow, remount, noauto, nofail and X-mount.mkdir[=mode];
 * anything else is the filesystem's own option and passed on (vfat:
 * uid=, gid=, umask=, fmask=, dmask=, shortname=; tmpfs: size=,
 * nr_inodes=, mode=, uid=, gid=).
 *
 * Exit status: 0 success, 1 wrong usage or no permission, 2 system error,
 * 32 the mount failed, 64 some of several mounts failed.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "fstab-util.h"

#define EX_OK_       0
#define EX_USAGE_    1
#define EX_SYSERR    2
#define EX_FAIL      32
#define EX_SOMEOK    64

static int verbose, fake;

static void usage(FILE *f)
{
	fprintf(f,
		"Usage:\n"
		" mount [-l] [-t <type>]\n"
		" mount -a [-fnv] [-t <types>] [-O <options>]\n"
		" mount [-fnrvw] [-o <options>] [-t <type>] <source> <directory>\n"
		" mount [-fnrvw] [-o <options>] <source> | <directory>\n"
		" mount -o remount[,<options>] <directory>\n"
		"\n"
		"Mount a filesystem, or list the mounted ones.\n"
		"\n"
		"Options:\n"
		" -a, --all              mount all filesystems of /etc/fstab\n"
		" -f, --fake             do everything but the mount itself\n"
		" -n, --no-mtab          accepted (there is no /etc/mtab)\n"
		" -o, --options <list>   comma-separated mount options\n"
		" -O, --test-opts <list> with -a: only lines with these options\n"
		" -r, --read-only        mount read-only (-o ro)\n"
		" -w, --rw, --read-write mount read-write (-o rw)\n"
		" -t, --types <list>     filesystem type(s)\n"
		" -l, --show-labels      accepted with a listing\n"
		" -v, --verbose          say what is being done\n"
		" -h, --help             this text\n"
		" -V, --version          the version\n"
		"\n"
		"Sources: /dev/<device>, UUID=<uuid>, LABEL=<label>,\n"
		"PARTUUID=<uuid>, PARTLABEL=<name>, or a word such as tmpfs.\n");
}

/* ---- type and option filters (-t, -O) ------------------------------------- */

/* Does `type' pass the -t list (NULL: everything)?  "nofoo" items exclude,
 * and a list that starts with "no" excludes every item of it. */
static int type_matches(const char *list, const char *type)
{
	char buf[256];
	int neg_all, any_pos = 0, hit = 0;

	if (!list || !list[0])
		return 1;
	snprintf(buf, sizeof(buf), "%s", list);
	neg_all = strncmp(buf, "no", 2) == 0;
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		int neg = neg_all || strncmp(t, "no", 2) == 0;
		const char *name = t;

		if (neg && strncmp(t, "no", 2) == 0)
			name = t + 2;
		if (!neg)
			any_pos = 1;
		if (strcmp(name, type) == 0)
			hit = neg ? -1 : 1;
		/* "vfat" also matches the FAT aliases and the other way round */
		if (!hit && fsu_type_is_fat(name) && fsu_type_is_fat(type))
			hit = neg ? -1 : 1;
	}
	if (hit < 0)
		return 0;
	if (hit > 0)
		return 1;
	return !any_pos;
}

/* Does the option string `opts' pass the -O list? */
static int opts_match(const char *list, const char *opts)
{
	char buf[256];

	if (!list || !list[0])
		return 1;
	snprintf(buf, sizeof(buf), "%s", list);
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		/* the option itself, or "no<option>": a line without it */
		if (fsu_has_opt(opts, t))
			continue;
		if (strncmp(t, "no", 2) == 0 && t[2] &&
		    !fsu_has_opt(opts, t + 2))
			continue;
		return 0;
	}
	return 1;
}

/* ---- listing ------------------------------------------------------------------ */

static int list_mounts(const char *types)
{
	struct fsu_table t;
	int rc = fsu_table_load(FSU_MOUNTS, &t);

	if (rc) {
		fprintf(stderr, "mount: %s: %s\n", FSU_MOUNTS, strerror(-rc));
		return EX_SYSERR;
	}
	for (int i = 0; i < t.n; i++) {
		if (!type_matches(types, t.e[i].type))
			continue;
		printf("%s on %s type %s (%s)\n", t.e[i].spec, t.e[i].dir,
		       t.e[i].type, t.e[i].opts);
	}
	fsu_table_free(&t);
	return EX_OK_;
}

/* ---- one mount ------------------------------------------------------------------- */

/* The error of a failed mount(2), the way people expect to read it. */
static void mount_error(int err, const char *src, const char *dir,
			const char *type, const char *data)
{
	switch (err) {
	case EPERM:
		fprintf(stderr, "mount: %s: permission denied.\n", dir);
		break;
	case ENOENT:
		fprintf(stderr, "mount: %s: special device %s does not exist.\n",
			dir, src);
		break;
	case ENOTDIR:
		fprintf(stderr, "mount: %s: mount point is not a directory.\n",
			dir);
		break;
	case EBUSY:
		fprintf(stderr, "mount: %s: %s already mounted or mount point "
			"busy.\n", dir, src);
		break;
	case ENODEV:
		fprintf(stderr, "mount: %s: unknown filesystem type '%s'.\n",
			dir, type ? type : "auto");
		break;
	case ENOTBLK:
		fprintf(stderr, "mount: %s: %s is not a block device.\n", dir,
			src);
		break;
	case ENXIO:
		fprintf(stderr, "mount: %s: %s is not a valid block device.\n",
			dir, src);
		break;
	case EROFS:
	case EACCES:
		fprintf(stderr, "mount: %s: cannot mount %s read-write, it is "
			"write-protected.\n", dir, src);
		break;
	case EINVAL:
		fprintf(stderr, "mount: %s: wrong fs type, bad option, bad "
			"superblock on %s, or other error%s%s%s.\n", dir, src,
			data && data[0] ? " (options: " : "",
			data && data[0] ? data : "", data && data[0] ? ")" : "");
		break;
	case ENOMEM:
		fprintf(stderr, "mount: %s: out of memory (or the mount table "
			"is full).\n", dir);
		break;
	default:
		fprintf(stderr, "mount: %s: mounting %s failed: %s.\n", dir, src,
			strerror(err));
		break;
	}
}

/* Mount `spec' on `dirarg' with type `type' (NULL/"auto": probe) and the
 * parsed options.  Says what went wrong; returns an exit status. */
static int do_mount(const char *spec, const char *dirarg, const char *type,
		    const struct fsu_opts *o)
{
	char dev[FSU_PATH_MAX], dir[FSU_PATH_MAX], words[160];
	struct stat st;
	unsigned long flags = o->flags;
	const char *t = type && type[0] ? type : "auto";
	int rc;

	if (o->bind || o->loop) {
		fprintf(stderr, "mount: %s: %s mounts are not offered.\n",
			dirarg, o->bind ? "bind" : "loop");
		return EX_FAIL;
	}
	rc = fsu_canon_dir(dirarg, dir, sizeof(dir));
	if (rc) {
		fprintf(stderr, "mount: %s: %s.\n", dirarg, strerror(-rc));
		return EX_FAIL;
	}
	if (stat(dir, &st) != 0) {
		if (errno == ENOENT && o->mkdir && !fake) {
			rc = fsu_mkdir_p(dir, o->mkdir_mode);
			if (rc) {
				fprintf(stderr, "mount: %s: cannot create the "
					"mount point: %s.\n", dir,
					strerror(-rc));
				return EX_FAIL;
			}
			if (verbose)
				printf("mount: %s: mount point created\n", dir);
		} else if (errno == ENOENT && !o->mkdir) {
			fprintf(stderr, "mount: %s: mount point does not "
				"exist.\n", dir);
			return EX_FAIL;
		} else if (!fake) {
			fprintf(stderr, "mount: %s: %s.\n", dir,
				strerror(errno));
			return EX_FAIL;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "mount: %s: mount point is not a directory.\n",
			dir);
		return EX_FAIL;
	}

	if (fsu_spec_is_device(spec)) {
		rc = fsu_resolve(spec, dev, sizeof(dev));
		if (rc == -ENOENT) {
			if (fsu_spec_tag(spec, NULL, 0) != FSU_TAG_NONE)
				fprintf(stderr, "mount: %s: can't find %s.\n",
					dir, spec);
			else
				fprintf(stderr, "mount: %s: special device %s "
					"does not exist.\n", dir, spec);
			return EX_FAIL;
		}
		if (rc) {
			fprintf(stderr, "mount: %s: %s: %s.\n", dir, spec,
				strerror(-rc));
			return EX_FAIL;
		}
	} else if (spec[0] == '/') {
		/* A file or directory: nothing here mounts those. */
		if (stat(spec, &st) != 0) {
			fprintf(stderr, "mount: %s: special device %s does not "
				"exist.\n", dir, spec);
			return EX_FAIL;
		}
		if (!S_ISBLK(st.st_mode)) {
			fprintf(stderr, "mount: %s: %s is not a block device%s.\n",
				dir, spec,
				S_ISDIR(st.st_mode) ? " (bind mounts are not "
						      "offered)" :
				S_ISREG(st.st_mode) ? " (loop devices are not "
						      "offered)" : "");
			return EX_FAIL;
		}
		{
			/* realpath() writes up to PATH_MAX bytes. */
			char real[PATH_MAX];

			snprintf(dev, sizeof(dev), "%s",
				 realpath(spec, real) ? real : spec);
		}
	} else {
		snprintf(dev, sizeof(dev), "%s", spec);
		if (!strcmp(t, "auto")) {
			if (!strcmp(spec, "tmpfs")) {
				t = "tmpfs";
			} else {
				fprintf(stderr, "mount: %s: you must specify the "
					"filesystem type.\n", dir);
				return EX_FAIL;
			}
		}
	}

	fsu_flags_text(flags & FSU_FLAG_MASK, words, sizeof(words));
	if (verbose)
		printf("mount: %s on %s type %s (%s%s%s)%s\n", dev, dir, t,
		       words, o->data[0] ? "," : "", o->data,
		       fake ? " -- not mounted (-f)" : "");
	if (fake)
		return EX_OK_;
	if (mount(dev, dir, t, flags, o->data[0] ? o->data : NULL) == 0)
		goto mounted;
	rc = errno;
	if ((rc == EROFS || rc == EACCES) && !(flags & MS_RDONLY) &&
	    !o->rw_given) {
		/* Write-protected and nobody insisted on rw: read-only. */
		if (mount(dev, dir, t, flags | MS_RDONLY,
			  o->data[0] ? o->data : NULL) == 0) {
			fprintf(stderr, "mount: %s: WARNING: source "
				"write-protected, mounted read-only.\n", dir);
			goto mounted;
		}
		rc = errno;
	}
	mount_error(rc, dev, dir, t, o->data);
	return rc == EPERM ? EX_USAGE_ : EX_FAIL;
mounted:
	if (verbose)
		printf("mount: %s mounted on %s.\n", dev, dir);
	return EX_OK_;
}

/* -o remount: the flags of the mounted filesystem, changed by `extra'. */
static int do_remount(const char *dirarg, const char *extra)
{
	struct fsu_table m;
	const struct fsu_entry *cur;
	struct fsu_opts o;
	char dir[FSU_PATH_MAX], words[160];
	int rc;

	rc = fsu_canon_dir(dirarg, dir, sizeof(dir));
	if (rc) {
		fprintf(stderr, "mount: %s: %s.\n", dirarg, strerror(-rc));
		return EX_FAIL;
	}
	rc = fsu_table_load(FSU_MOUNTS, &m);
	if (rc) {
		fprintf(stderr, "mount: %s: %s\n", FSU_MOUNTS, strerror(-rc));
		return EX_SYSERR;
	}
	cur = fsu_table_find_dir(&m, dir);
	if (!cur) {
		/* Given by its device? */
		char dev[FSU_PATH_MAX];

		if (fsu_spec_is_device(dirarg) &&
		    fsu_resolve(dirarg, dev, sizeof(dev)) == 0)
			cur = fsu_table_find_spec(&m, dev);
		if (cur)
			snprintf(dir, sizeof(dir), "%s", cur->dir);
	}
	if (!cur) {
		fprintf(stderr, "mount: %s: mount point not mounted or bad "
			"option.\n", dir);
		fsu_table_free(&m);
		return EX_FAIL;
	}
	/* The current flags, then what the command line changes; only the
	 * filesystem options given now are passed on (the kernel keeps the
	 * others). */
	fsu_opts_init(&o);
	o.flags = fsu_flags_of(cur->opts);
	rc = fsu_opts_parse(&o, extra);
	if (rc) {
		fprintf(stderr, "mount: %s: the options are too long.\n", dir);
		fsu_table_free(&m);
		return EX_USAGE_;
	}
	fsu_flags_text(o.flags & FSU_FLAG_MASK, words, sizeof(words));
	if (verbose)
		printf("mount: remounting %s (%s%s%s)%s\n", dir, words,
		       o.data[0] ? "," : "", o.data,
		       fake ? " -- not done (-f)" : "");
	if (!fake &&
	    mount(cur->spec, dir, NULL, MS_REMOUNT | (o.flags & FSU_FLAG_MASK),
		  o.data[0] ? o.data : NULL) != 0) {
		rc = errno;
		if (rc == EINVAL)
			fprintf(stderr, "mount: %s: mount point not mounted or "
				"bad option.\n", dir);
		else if (rc == EBUSY)
			fprintf(stderr, "mount: %s: cannot remount read-only: "
				"files are open for writing.\n", dir);
		else if (rc == EROFS)
			fprintf(stderr, "mount: %s: cannot remount %s "
				"read-write, is write-protected or has errors.\n",
				dir, cur->spec);
		else
			fprintf(stderr, "mount: %s: remount failed: %s.\n", dir,
				strerror(rc));
		fsu_table_free(&m);
		return rc == EPERM ? EX_USAGE_ : EX_FAIL;
	}
	if (verbose)
		printf("mount: %s remounted (%s).\n", dir, words);
	fsu_table_free(&m);
	return EX_OK_;
}

/* Join "a" and "b" as one option list in `out'. */
static int join_opts(char *out, size_t len, const char *a, const char *b)
{
	int n = snprintf(out, len, "%s%s%s", a ? a : "",
			 (a && a[0] && b && b[0]) ? "," : "", b ? b : "");

	return n < 0 || (size_t)n >= len ? -1 : 0;
}

/* ---- mount -a ------------------------------------------------------------------ */

static int mount_all(const char *types, const char *testopts,
		     const char *cmdopts)
{
	struct fsu_table tab, cur;
	int ok = 0, bad = 0, rc;

	rc = fsu_table_load(FSU_FSTAB, &tab);
	if (rc) {
		fprintf(stderr, "mount: %s: %s\n", FSU_FSTAB, strerror(-rc));
		return EX_SYSERR;
	}
	if (fsu_table_load(FSU_MOUNTS, &cur) != 0)
		memset(&cur, 0, sizeof(cur));
	for (int i = 0; i < tab.n; i++) {
		const struct fsu_entry *e = &tab.e[i];
		struct fsu_opts o;
		char dir[FSU_PATH_MAX], all[2 * FSU_DATA_MAX];

		if (!e->dir[0] || !e->type[0] || !strcmp(e->type, "swap") ||
		    !strcmp(e->type, "ignore") || !strcmp(e->dir, "none"))
			continue;
		if (!type_matches(types, e->type) || !opts_match(testopts, e->opts))
			continue;
		if (join_opts(all, sizeof(all), e->opts, cmdopts) != 0) {
			fprintf(stderr, "mount: %s: options too long\n", e->dir);
			bad++;
			continue;
		}
		fsu_opts_init(&o);
		if (fsu_opts_parse(&o, all) != 0) {
			fprintf(stderr, "mount: %s: options too long\n", e->dir);
			bad++;
			continue;
		}
		if (o.noauto)
			continue;
		if (fsu_canon_dir(e->dir, dir, sizeof(dir)) != 0)
			snprintf(dir, sizeof(dir), "%s", e->dir);
		if (!strcmp(dir, "/") || fsu_table_find_dir(&cur, dir)) {
			if (verbose)
				printf("mount: %s: already mounted\n", dir);
			continue;
		}
		if (fsu_type_is_kernel(e->type)) {
			if (verbose)
				printf("mount: %s: %s is the kernel's, skipped\n",
				       dir, e->type);
			continue;
		}
		rc = do_mount(e->spec, dir, e->type, &o);
		if (rc == EX_OK_)
			ok++;
		else if (!(o.nofail && rc == EX_FAIL))
			bad++;
	}
	fsu_table_free(&cur);
	fsu_table_free(&tab);
	if (!bad)
		return EX_OK_;
	return ok ? EX_SOMEOK : EX_FAIL;
}

/* ---- main ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "fake", no_argument, NULL, 'f' },
		{ "no-mtab", no_argument, NULL, 'n' },
		{ "options", required_argument, NULL, 'o' },
		{ "test-opts", required_argument, NULL, 'O' },
		{ "read-only", no_argument, NULL, 'r' },
		{ "rw", no_argument, NULL, 'w' },
		{ "read-write", no_argument, NULL, 'w' },
		{ "types", required_argument, NULL, 't' },
		{ "show-labels", no_argument, NULL, 'l' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};
	char cmdopts[FSU_DATA_MAX] = "", all[2 * FSU_DATA_MAX];
	const char *types = NULL, *testopts = NULL;
	int all_mode = 0, c, rw = -1;
	struct fsu_opts o;

	while ((c = getopt_long(argc, argv, "afno:O:rRwt:lvhV", longopts,
				NULL)) != -1) {
		switch (c) {
		case 'a':
			all_mode = 1;
			break;
		case 'f':
			fake = 1;
			break;
		case 'n':
		case 'l':
			break;
		case 'o':
			if (join_opts(all, sizeof(all), cmdopts, optarg) != 0 ||
			    strlen(all) >= sizeof(cmdopts)) {
				fprintf(stderr, "mount: the options are too "
					"long\n");
				return EX_USAGE_;
			}
			strcpy(cmdopts, all);
			break;
		case 'O':
			testopts = optarg;
			break;
		case 'r':
		case 'R':
			rw = 0;
			break;
		case 'w':
			rw = 1;
			break;
		case 't':
			types = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage(stdout);
			return EX_OK_;
		case 'V':
			printf("mount (LikeOS) 1.0\n");
			return EX_OK_;
		default:
			usage(stderr);
			return EX_USAGE_;
		}
	}
	/* -r / -w go after the -o list, so they win. */
	if (rw >= 0) {
		if (join_opts(all, sizeof(all), cmdopts, rw ? "rw" : "ro") != 0 ||
		    strlen(all) >= sizeof(cmdopts)) {
			fprintf(stderr, "mount: the options are too long\n");
			return EX_USAGE_;
		}
		strcpy(cmdopts, all);
	}

	if (!all_mode && optind == argc) {
		if (cmdopts[0]) {
			usage(stderr);
			return EX_USAGE_;
		}
		return list_mounts(types);
	}
	if (geteuid() != 0 && !fake) {
		fprintf(stderr, "mount: only root can do that.\n");
		return EX_USAGE_;
	}
	if (all_mode) {
		if (optind != argc) {
			usage(stderr);
			return EX_USAGE_;
		}
		return mount_all(types, testopts, cmdopts);
	}
	if (argc - optind > 2) {
		usage(stderr);
		return EX_USAGE_;
	}

	fsu_opts_init(&o);
	if (fsu_opts_parse(&o, cmdopts) != 0) {
		fprintf(stderr, "mount: the options are too long\n");
		return EX_USAGE_;
	}
	if (o.remount) {
		/* mount -o remount dir, or mount -o remount source dir */
		return do_remount(argv[argc - 1], cmdopts);
	}

	if (argc - optind == 2)
		return do_mount(argv[optind], argv[optind + 1], types, &o);

	/* One argument: the /etc/fstab line naming it as mount point or as
	 * source; the command line options go after the line's. */
	{
		const char *arg = argv[optind];
		struct fsu_table tab;
		const struct fsu_entry *e = NULL;
		char dir[FSU_PATH_MAX], dev[FSU_PATH_MAX], edev[FSU_PATH_MAX];
		int rc;

		rc = fsu_table_load(FSU_FSTAB, &tab);
		if (rc) {
			fprintf(stderr, "mount: %s: %s\n", FSU_FSTAB,
				strerror(-rc));
			return EX_SYSERR;
		}
		if (fsu_canon_dir(arg, dir, sizeof(dir)) != 0)
			dir[0] = '\0';
		for (int i = tab.n - 1; i >= 0 && !e; i--) {
			char ed[FSU_PATH_MAX];

			if (fsu_canon_dir(tab.e[i].dir, ed, sizeof(ed)) != 0)
				snprintf(ed, sizeof(ed), "%s", tab.e[i].dir);
			if ((dir[0] && !strcmp(ed, dir)) ||
			    !strcmp(tab.e[i].spec, arg))
				e = &tab.e[i];
		}
		/* A device given by another name than the line's. */
		if (!e && fsu_spec_is_device(arg) &&
		    fsu_resolve(arg, dev, sizeof(dev)) == 0) {
			for (int i = tab.n - 1; i >= 0 && !e; i--)
				if (fsu_spec_is_device(tab.e[i].spec) &&
				    fsu_resolve(tab.e[i].spec, edev,
						sizeof(edev)) == 0 &&
				    !strcmp(edev, dev))
					e = &tab.e[i];
		}
		if (!e) {
			fprintf(stderr, "mount: %s: can't find in %s.\n", arg,
				FSU_FSTAB);
			fsu_table_free(&tab);
			return EX_FAIL;
		}
		if (join_opts(all, sizeof(all), e->opts, cmdopts) != 0) {
			fprintf(stderr, "mount: the options are too long\n");
			fsu_table_free(&tab);
			return EX_USAGE_;
		}
		fsu_opts_init(&o);
		if (fsu_opts_parse(&o, all) != 0) {
			fprintf(stderr, "mount: the options are too long\n");
			fsu_table_free(&tab);
			return EX_USAGE_;
		}
		rc = do_mount(e->spec, e->dir, types ? types : e->type, &o);
		fsu_table_free(&tab);
		return rc;
	}
}
