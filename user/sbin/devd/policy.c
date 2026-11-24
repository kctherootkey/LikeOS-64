// devd: what may be done with which device.
//
// Three kinds of disk, told apart by what the kernel says about them:
//
//   USB drive      (the kernel or a usb-* link in /dev/disk/by-id says USB)
//                  mount, unmount, eject, format -- format not when the
//                  drive is write-protected
//   CD/DVD drive   (SCSI peripheral type 5 in /sys/block/<disk>/device/type)
//                  mount (always read-only), unmount, eject (the tray
//                  opens); never format
//   built-in disk  (neither of the two)
//                  mount and unmount only: never eject, never format, and
//                  the volume agent never mounts one by itself
//
// and above all three: a disk the running system uses -- anything on it
// mounted by someone other than devd (the root, /boot/efi, a swap file's
// filesystem, a mount root made by hand) -- is a system disk, and nothing
// at all is done with any of its volumes.  Neither are the firmware's and
// the installed systems' own partitions of a built-in disk (EFI system,
// reserved, recovery, BIOS boot): they hold boot code, not people's files.
//
// The same rules hold for every caller, root included; who may ask at all
// (root or the user at the console) is auth.c's question.  Also here: what
// a cancelled format leaves and who may cancel one.  Pure functions:
// tested on the build machine.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>
#include <sys/mount.h>

#include "devd.h"

void dev_classify(struct dev *d, int usb, int optical, int disk_ro,
		  int part_ro)
{
	d->usb = usb ? 1 : 0;
	d->optical = optical ? 1 : 0;
	d->internal = !d->usb && !d->optical;
	/* a disc is read-only whatever the drive could write */
	d->readonly = disk_ro || part_ro || d->optical;
}

int dev_policy_check(const struct dev *d, enum dev_op op, const char **why)
{
	if (d->system || d->reserved) {
		*why = d->reserved ? "this partition belongs to the firmware "
				     "or to an installed system" :
				     "this is a system drive";
		return -EPERM;
	}
	if (d->optical) {
		if (op == OP_FORMAT) {
			*why = "a CD or DVD cannot be formatted";
			return -EPERM;
		}
		return 0;
	}
	if (d->usb && d->removable && !d->internal) {
		if (op == OP_FORMAT && d->readonly) {
			*why = "the drive is write-protected";
			return -EPERM;
		}
		return 0;
	}
	if (d->internal) {
		if (op == OP_MOUNT || op == OP_UNMOUNT)
			return 0;
		*why = op == OP_FORMAT ? "a built-in disk is not formatted "
					 "here" :
					 "a built-in disk cannot be ejected";
		return -EPERM;
	}
	/* a USB device that is not removable: nothing is known about it */
	*why = "not a removable drive";
	return -EPERM;
}

unsigned long dev_mount_flags(const struct dev *d, const struct fs_type *t)
{
	unsigned long fl = MS_NOSUID | MS_NODEV;

	if (d->readonly || d->optical || (t && t->always_ro))
		fl |= MS_RDONLY;
	return fl;
}

int dev_medium_changed(const struct dev *old, const struct dev *cur)
{
	/* The disk sequence number changes with every new medium, and
	 * otherwise only when a partition table is read again -- which the
	 * kernel refuses while anything on the disk is mounted.  So under a
	 * mount, a new number is a new medium. */
	if (cur->size == 0)
		return 1;
	return old->diskseq != cur->diskseq;
}

/* ---- cancelling a format ------------------------------------------------- */

int format_mark_parse(const char *line, int *pct)
{
	uint64_t v;

	if (line[0] == 'P' && devp_parse_u64(line + 1, 99, &v) == 0) {
		*pct = (int)v;
		return FMARK_PROGRESS;
	}
	if (strcmp(line, "W") == 0)
		return FMARK_WRITE;
	if (strcmp(line, "M") == 0)
		return FMARK_MKFS;
	return FMARK_NONE;
}

int format_mark_writes(int mark)
{
	/* The child says so before it writes, so a job stopped in between
	 * counts as written: the safe side. */
	return mark == FMARK_WRITE || mark == FMARK_MKFS;
}

int format_cancel_allowed(uint32_t uid, uint32_t owner)
{
	return uid == 0 || uid == owner;
}

const char *format_cancel_message(int written, int cleared)
{
	if (!written)
		return "Formatting was cancelled. The drive was not changed.";
	if (cleared)
		return "Formatting was cancelled. The drive must be formatted "
		       "before it can be used.";
	return "Formatting was cancelled. The drive could not be cleared; it "
	       "must be formatted before it can be used.";
}
