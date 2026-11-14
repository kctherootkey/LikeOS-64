/*
 * umount - unmount filesystems.
 *
 *   umount [-flrv] target|source ...   unmount these (a device, UUID=...
 *                                      and the like name what is mounted
 *                                      from it)
 *   umount -a [-t types] [-flrv]       unmount everything that can be,
 *                                      the most recent mount first
 *
 * Options:
 *   -a, --all            every mount but the root and the kernel's own
 *                        (/proc, /sys, /dev), in reverse order of mounting
 *   -t, --types LIST     with -a, the types to include ("no" excludes)
 *   -f, --force          MNT_FORCE (accepted by the kernel; files open on
 *                        the filesystem still keep it busy)
 *   -l, --lazy           detach now, finish when the last file is closed
 *   -r, --read-only      when unmounting fails as busy, remount read-only
 *   -n, --no-mtab        accepted; there is no /etc/mtab
 *   -v, --verbose        say what is done
 *   -h, --help, -V, --version
 *
 * Exit status: 0 success, 1 wrong usage or no permission, 2 system error,
 * 32 the unmount failed, 64 some of several failed.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "fstab-util.h"

#define EX_OK_    0
#define EX_USAGE_ 1
#define EX_SYSERR 2
#define EX_FAIL   32
#define EX_SOMEOK 64

static int verbose, lazy, force, ro_fallback;

static void usage(FILE *f)
{
	fprintf(f,
		"Usage:\n"
		" umount [-flnrv] <directory>|<source> ...\n"
		" umount -a [-flnrv] [-t <types>]\n"
		"\n"
		"Unmount filesystems.\n"
		"\n"
		"Options:\n"
		" -a, --all             unmount all filesystems that can be\n"
		" -t, --types <list>    with -a: only these types\n"
		" -f, --force           force the unmount\n"
		" -l, --lazy            detach now, clean up when no longer busy\n"
		" -r, --read-only       if unmounting fails, remount read-only\n"
		" -n, --no-mtab         accepted (there is no /etc/mtab)\n"
		" -v, --verbose         say what is being done\n"
		" -h, --help            this text\n"
		" -V, --version         the version\n"
		"\n"
		"<source> is /dev/<device>, UUID=<uuid>, LABEL=<label>,\n"
		"PARTUUID=<uuid> or PARTLABEL=<name>.\n");
}

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
		if (strcmp(name, type) == 0 ||
		    (fsu_type_is_fat(name) && fsu_type_is_fat(type)))
			hit = neg ? -1 : 1;
	}
	if (hit)
		return hit > 0;
	return !any_pos;
}

/* Mounts the kernel made at boot and keeps for good. */
static int is_permanent(const struct fsu_entry *e)
{
	return !strcmp(e->dir, "/") || fsu_type_is_kernel(e->type);
}

/* Unmount the filesystem mounted at `dir' (from `spec').  Returns an exit
 * status. */
static int unmount_one(const char *dir, const char *spec)
{
	int flags = 0, err;

	if (lazy)
		flags |= MNT_DETACH;
	if (force)
		flags |= MNT_FORCE;
	if (umount2(dir, flags) == 0) {
		if (verbose)
			printf("umount: %s unmounted\n", dir);
		return EX_OK_;
	}
	err = errno;
	if (err == EBUSY && ro_fallback) {
		if (mount(spec, dir, NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) {
			fprintf(stderr, "umount: %s busy -- remounted "
				"read-only\n", dir);
			return EX_OK_;
		}
		fprintf(stderr, "umount: %s: target is busy and cannot be "
			"remounted read-only: %s.\n", dir, strerror(errno));
		return EX_FAIL;
	}
	switch (err) {
	case EBUSY:
		fprintf(stderr, "umount: %s: target is busy%s.\n", dir,
			!strcmp(dir, "/") ? " (the root filesystem)" :
			lazy ? "" : " (files are open on it, a working "
				    "directory is inside it, or something is "
				    "mounted below it; -l detaches it anyway)");
		break;
	case EINVAL:
		fprintf(stderr, "umount: %s: not mounted.\n", dir);
		break;
	case EPERM:
		fprintf(stderr, "umount: %s: must be superuser to unmount.\n",
			dir);
		return EX_USAGE_;
	case ENOENT:
		fprintf(stderr, "umount: %s: no mount point specified.\n", dir);
		break;
	default:
		fprintf(stderr, "umount: %s: %s.\n", dir, strerror(err));
		break;
	}
	return EX_FAIL;
}

/* One command line argument: a mount point, or the source of a mount. */
static int unmount_arg(const char *arg, const struct fsu_table *m)
{
	char dir[FSU_PATH_MAX], dev[FSU_PATH_MAX];
	const struct fsu_entry *e = NULL;

	if (fsu_canon_dir(arg, dir, sizeof(dir)) == 0)
		e = fsu_table_find_dir(m, dir);
	if (!e)
		e = fsu_table_find_spec(m, arg);
	if (!e && (fsu_spec_is_device(arg) || arg[0] == '/') &&
	    fsu_resolve(arg, dev, sizeof(dev)) == 0) {
		e = fsu_table_find_spec(m, dev);
		/* a source shown under another name of the same device */
		for (int i = m->n - 1; i >= 0 && !e; i--) {
			char real[FSU_PATH_MAX];

			if (fsu_spec_is_device(m->e[i].spec) &&
			    fsu_resolve(m->e[i].spec, real, sizeof(real)) == 0 &&
			    !strcmp(real, dev))
				e = &m->e[i];
		}
	}
	if (!e) {
		if (fsu_spec_tag(arg, NULL, 0) != FSU_TAG_NONE &&
		    fsu_resolve(arg, dev, sizeof(dev)) != 0) {
			fprintf(stderr, "umount: %s: no such device.\n", arg);
			return EX_FAIL;
		}
		/* Let the kernel decide; it says "not mounted". */
		return unmount_one(fsu_canon_dir(arg, dir, sizeof(dir)) == 0 ?
				   dir : arg, arg);
	}
	if (is_permanent(e) && verbose)
		printf("umount: %s is mounted by the kernel at boot\n", e->dir);
	return unmount_one(e->dir, e->spec);
}

static int unmount_all(const char *types, const struct fsu_table *m)
{
	int ok = 0, bad = 0;

	/* The table lists a mount after the one it is mounted on, so the
	 * reverse order unmounts children first. */
	for (int i = m->n - 1; i >= 0; i--) {
		const struct fsu_entry *e = &m->e[i];

		if (is_permanent(e) || !type_matches(types, e->type))
			continue;
		if (unmount_one(e->dir, e->spec) == EX_OK_)
			ok++;
		else
			bad++;
	}
	if (!bad)
		return EX_OK_;
	return ok ? EX_SOMEOK : EX_FAIL;
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "all", no_argument, NULL, 'a' },
		{ "types", required_argument, NULL, 't' },
		{ "force", no_argument, NULL, 'f' },
		{ "lazy", no_argument, NULL, 'l' },
		{ "read-only", no_argument, NULL, 'r' },
		{ "no-mtab", no_argument, NULL, 'n' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};
	struct fsu_table m;
	const char *types = NULL;
	int all = 0, c, rc, ok = 0, bad = 0;

	while ((c = getopt_long(argc, argv, "at:flrnvhV", longopts, NULL)) !=
	       -1) {
		switch (c) {
		case 'a':
			all = 1;
			break;
		case 't':
			types = optarg;
			break;
		case 'f':
			force = 1;
			break;
		case 'l':
			lazy = 1;
			break;
		case 'r':
			ro_fallback = 1;
			break;
		case 'n':
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage(stdout);
			return EX_OK_;
		case 'V':
			printf("umount (LikeOS) 1.0\n");
			return EX_OK_;
		default:
			usage(stderr);
			return EX_USAGE_;
		}
	}
	if (!all && optind == argc) {
		fprintf(stderr, "umount: bad usage\n");
		usage(stderr);
		return EX_USAGE_;
	}
	if (all && optind != argc) {
		usage(stderr);
		return EX_USAGE_;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "umount: only root can do that.\n");
		return EX_USAGE_;
	}
	rc = fsu_table_load(FSU_MOUNTS, &m);
	if (rc) {
		fprintf(stderr, "umount: %s: %s\n", FSU_MOUNTS, strerror(-rc));
		return EX_SYSERR;
	}
	if (all) {
		rc = unmount_all(types, &m);
		fsu_table_free(&m);
		return rc;
	}
	for (int i = optind; i < argc; i++) {
		if (unmount_arg(argv[i], &m) == EX_OK_)
			ok++;
		else
			bad++;
	}
	fsu_table_free(&m);
	if (!bad)
		return EX_OK_;
	if (argc - optind == 1)
		return EX_FAIL;
	return ok ? EX_SOMEOK : EX_FAIL;
}
