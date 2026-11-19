/* libinstaller -- the boot loader on the EFI system partition.
 *
 *   \EFI\LIKEOS\BOOTX64.EFI   the LikeOS boot loader (from the image's
 *                             /usr/lib/likeos/efi/BOOTX64.EFI)
 *   \EFI\LIKEOS\cmdline.txt   root=PARTUUID=<root> rootfstype=ext4
 *   \EFI\LIKEOS\BOOTX64.CSV   UTF-16 description of the firmware boot entry
 *                             (the loader's fallback path reads it)
 *   \EFI\BOOT\BOOTX64.EFI     the removable-media path every firmware tries
 *
 * Then likeos-efiboot creates the firmware boot entry "LikeOS" and puts it
 * first in BootOrder.  When that tool is missing or fails the installation
 * still succeeds: the loader, started from \EFI\BOOT on the first boot,
 * registers the entry itself from BOOTX64.CSV.
 *
 * No bootmenu.conf is ever written here: the boot menu belongs to the live
 * medium; an installed system starts straight into LikeOS.
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

#define IL_CSV_TEXT "BOOTX64.EFI,LikeOS,,This is the boot entry for LikeOS\r\n"

static int il_esp_path(il_ctx *ctx, const struct il_target *t, const char *rel,
		       char *buf, size_t len)
{
	if (il_path_join(buf, len, t->esp_dir, rel))
		return il_set_error(ctx, ENAMETOOLONG, "%s/%s: path too long",
				    t->esp_dir, rel);
	return 0;
}

/* Words of the running system's command line worth keeping on the
 * installed one (console and debugging switches); the root, its type and
 * the live medium's mode are the installer's to decide. */
static void il_inherited_cmdline(il_ctx *ctx, char *out, size_t len)
{
	char path[IL_PATH_MAX], buf[1024];
	char *save = NULL;

	out[0] = '\0';
	if (ctx->simulate)
		return;
	il_sys_path(ctx, path, sizeof(path), "/proc/cmdline");
	if (il_read_file(path, buf, sizeof(buf)) < 0)
		return;
	for (char *w = strtok_r(buf, " \t\r\n", &save); w;
	     w = strtok_r(NULL, " \t\r\n", &save)) {
		int printable = 1;

		if (il_starts_with(w, "root=") || il_starts_with(w, "rootfstype=") ||
		    il_starts_with(w, "likeos.mode=") ||
		    il_starts_with(w, "BOOT_IMAGE=") ||
		    il_starts_with(w, "initrd="))
			continue;
		for (const char *p = w; *p; p++)
			if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7e)
				printable = 0;
		if (!printable)
			continue;
		if (out[0])
			il_strlcat(out, " ", len);
		il_strlcat(out, w, len);
	}
}

static int il_write_cmdline(il_ctx *ctx, const struct il_target *t)
{
	char path[IL_PATH_MAX], extra[512], text[1024];
	int rc;

	rc = il_esp_path(ctx, t, "EFI/LIKEOS/cmdline.txt", path, sizeof(path));
	if (rc)
		return rc;
	il_inherited_cmdline(ctx, extra, sizeof(extra));
	snprintf(text, sizeof(text),
		 "# LikeOS kernel command line\n"
		 "# Read by the LikeOS boot loader when the firmware boot entry "
		 "passes no\n"
		 "# options of its own.  Lines starting with '#' are comments; "
		 "the other\n"
		 "# lines are joined into one command line.\n"
		 "root=PARTUUID=%s rootfstype=ext4%s%s\n",
		 t->root_partuuid, extra[0] ? " " : "", extra);
	rc = il_write_file_atomic(path, text, strlen(text), 0644, (uid_t)-1,
				  (gid_t)-1);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be written: %s", path,
				    strerror(-rc));
	il_log(ctx, IL_LOG_INFO, "boot command line: root=PARTUUID=%s "
	       "rootfstype=ext4%s%s", t->root_partuuid, extra[0] ? " " : "",
	       extra);
	return 0;
}

static int il_write_csv(il_ctx *ctx, const struct il_target *t)
{
	char path[IL_PATH_MAX];
	const char *s = IL_CSV_TEXT;
	size_t n = strlen(s);
	unsigned char *buf;
	int rc;

	rc = il_esp_path(ctx, t, "EFI/LIKEOS/BOOTX64.CSV", path, sizeof(path));
	if (rc)
		return rc;
	/* UTF-16LE with a byte order mark; the text is plain ASCII. */
	buf = malloc(2 + 2 * n);
	if (!buf)
		return il_set_error(ctx, ENOMEM, "out of memory");
	buf[0] = 0xFF;
	buf[1] = 0xFE;
	for (size_t i = 0; i < n; i++) {
		buf[2 + 2 * i] = (unsigned char)s[i];
		buf[3 + 2 * i] = 0;
	}
	rc = il_write_file_atomic(path, buf, 2 + 2 * n, 0644, (uid_t)-1,
				  (gid_t)-1);
	free(buf);
	if (rc)
		return il_set_error(ctx, -rc, "%s cannot be written: %s", path,
				    strerror(-rc));
	return 0;
}

/* 1 when both files have the same contents. */
static int il_same_file(const char *a, const char *b)
{
	char *x = NULL, *y = NULL;
	size_t lx = 0, ly = 0;
	int same = 0;

	if (il_read_file_alloc(a, &x, &lx) == 0 &&
	    il_read_file_alloc(b, &y, &ly) == 0)
		same = lx == ly && memcmp(x, y, lx) == 0;
	free(x);
	free(y);
	return same;
}

static void il_firmware_entry(il_ctx *ctx, const struct il_target *t)
{
	char part[16];
	const char *argv[16];
	int n = 0, status = -1;

	if (ctx->simulate) {
		il_log(ctx, IL_LOG_INFO, "simulation: no firmware boot entry");
		return;
	}
	if (access(ctx->tool_efiboot, X_OK) != 0) {
		il_log(ctx, IL_LOG_WARN, "%s is not available; the boot loader "
		       "registers its firmware boot entry itself on the first "
		       "start", ctx->tool_efiboot);
		return;
	}
	snprintf(part, sizeof(part), "%d", t->esp_partno);
	argv[n++] = ctx->tool_efiboot;
	argv[n++] = "--create";
	argv[n++] = "--disk";
	argv[n++] = t->disk_dev;
	argv[n++] = "--part";
	argv[n++] = part;
	argv[n++] = "--loader";
	argv[n++] = "\\EFI\\LIKEOS\\BOOTX64.EFI";
	argv[n++] = "--label";
	argv[n++] = "LikeOS";
	argv[n++] = "--first";
	argv[n] = NULL;
	if (il_spawn(ctx, argv, &status, NULL, 0) != 0 || status != 0) {
		il_clear_error(ctx);
		il_log(ctx, IL_LOG_WARN, "the firmware boot entry could not be "
		       "created (status %d); the boot loader registers it "
		       "itself on the first start", status);
		return;
	}
	il_log(ctx, IL_LOG_INFO, "firmware boot entry \"LikeOS\" created and "
	       "put first");
}

int il_boot_install(il_ctx *ctx, const struct il_target *t)
{
	char src[IL_PATH_MAX], dir[IL_PATH_MAX], dst[IL_PATH_MAX];
	char fallback[IL_PATH_MAX], menu[IL_PATH_MAX];
	int rc;

	if (il_path_in(src, sizeof(src), t->root_dir, IL_EFI_LOADER_IN_IMAGE))
		return il_set_error(ctx, ENAMETOOLONG, "path too long");
	if (!il_file_exists(src))
		return il_set_error(ctx, ENOENT, "The system image has no boot "
				    "loader (%s is missing)",
				    IL_EFI_LOADER_IN_IMAGE);
	rc = il_esp_path(ctx, t, "EFI/LIKEOS", dir, sizeof(dir));
	if (!rc && (rc = il_mkdir_p(dir, 0755)) != 0)
		return il_set_error(ctx, -rc, "%s cannot be created: %s", dir,
				    strerror(-rc));
	if (rc)
		return rc;
	rc = il_esp_path(ctx, t, "EFI/BOOT", dir, sizeof(dir));
	if (!rc && (rc = il_mkdir_p(dir, 0755)) != 0)
		return il_set_error(ctx, -rc, "%s cannot be created: %s", dir,
				    strerror(-rc));
	if (rc)
		return rc;

	il_step_progress(ctx, 0.1, "Copying the boot loader");
	rc = il_esp_path(ctx, t, "EFI/LIKEOS/BOOTX64.EFI", dst, sizeof(dst));
	if (rc)
		return rc;
	rc = il_copy_file(src, dst, 0644);
	if (rc)
		return il_set_error(ctx, -rc, "The boot loader cannot be copied "
				    "to %s: %s", dst, strerror(-rc));
	rc = il_esp_path(ctx, t, "EFI/BOOT/BOOTX64.EFI", fallback,
			 sizeof(fallback));
	if (rc)
		return rc;
	/* The removable-media path of an ESP LikeOS did not format may hold
	 * another system's loader: it stays.  The firmware boot entry (or
	 * the boot menu of the firmware) starts LikeOS then. */
	if (!t->format_esp && il_file_exists(fallback) &&
	    !il_same_file(fallback, dst)) {
		il_log(ctx, IL_LOG_WARN, "\\EFI\\BOOT\\BOOTX64.EFI belongs to "
		       "another system and is kept; LikeOS starts through its "
		       "firmware boot entry");
	} else {
		rc = il_copy_file(src, fallback, 0644);
		if (rc)
			return il_set_error(ctx, -rc, "The boot loader cannot be "
					    "copied to %s: %s", fallback,
					    strerror(-rc));
	}
	il_log(ctx, IL_LOG_INFO, "boot loader copied to the EFI system "
	       "partition");

	il_step_progress(ctx, 0.4, "Writing the boot settings");
	rc = il_write_cmdline(ctx, t);
	if (!rc)
		rc = il_write_csv(ctx, t);
	if (rc)
		return rc;
	/* A boot menu is for the live medium only. */
	if (il_esp_path(ctx, t, "EFI/LIKEOS/bootmenu.conf", menu,
			sizeof(menu)) == 0 && unlink(menu) == 0)
		il_log(ctx, IL_LOG_INFO, "removed a boot menu configuration from "
		       "the EFI system partition");
	{
		int fd = open(t->esp_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

		if (fd >= 0) {
			il_syncfs(fd);
			close(fd);
		}
	}
	il_step_progress(ctx, 0.7, "Creating the firmware boot entry");
	il_firmware_entry(ctx, t);
	return 0;
}
