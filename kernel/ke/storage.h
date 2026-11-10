// LikeOS storage bootstrap: finding and mounting the root filesystem
//
// The root filesystem is chosen by IDENTITY, the way a production system
// boots, never by mounting whatever disk happens to answer first:
//
//   (a) an explicit root= on the boot command line (root=PARTUUID=...,
//       root=PARTLABEL=..., root=UUID=..., root=LABEL=..., root=/dev/sda2,
//       root=sda2, root=/dev/disk/by-*/..., root=MAJ:MIN), passed by the
//       bootloader from the boot entry's load options or from
//       \EFI\LIKEOS\cmdline.txt on the EFI System Partition;
//   (b) otherwise: the disk that holds the partition the bootloader was
//       started from (its PARTUUID, from the firmware's device path), and on
//       that disk the partition of GPT type 4f68bce3-e8cd-4db1-96e7-
//       fbcaf984b709 -- the standard x86-64 root type;
//   (c) only when the bootloader passed no identity at all (an old
//       bootloader): the legacy search for /LIKEOS.SIG, read-only, with a
//       warning that the image is old.
//
// Exactly one partition is mounted -- the root.  No other disk or partition
// is ever mounted automatically.  Optical drives (srN) are never candidates.
// Everything works on the registered block devices of the disk layer
// (kernel/dev/block/blkdev.h): AHCI/SATA disks, USB sticks, their partitions;
// no driver's private structure is ever looked at.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_STORAGE_H_
#define _KERNEL_STORAGE_H_

#include <kernel/fs/fat32.h>
#include <kernel/dev/block/block.h>
#include <kernel/mm/memory.h> /* boot_info_t */

/* State of the root search, owned by init.c (one instance) and driven by
 * storage_fs_poll() from the kdevpoll thread. */
typedef struct {
	/* Set once the root filesystem is mounted; the search then stops.
	 * (The name is historic: it meant "/LIKEOS.SIG found".) */
	int signature_found;
	/* storage_fs_set_ready() was called: the system is far enough up
	 * (scheduler, interrupts, disk drivers in interrupt mode) to mount. */
	int os_ready;
	/* timer_ticks() when the search started (first poll after ready) */
	uint64_t start_tick;
	/* timer_ticks() of the last candidate scan and of the last
	 * "still waiting" report */
	uint64_t last_scan_tick;
	uint64_t last_report_tick;
	/* number of candidate scans done */
	unsigned long scans;
	/* Which rule is active: STORAGE_RULE_* */
	int rule;
	/* The candidate chosen for mounting and how many consecutive polls it
	 * has been ready (it must stay ready STORAGE_SETTLE_POLLS polls before
	 * it is mounted, as removable media take a moment to settle). */
	const block_device_t *pending;
	unsigned int pending_ready_polls;
	/* timer_ticks() when `pending' was chosen (and ready) */
	uint64_t pending_since_tick;
} storage_fs_state_t;

/* storage_fs_state_t.rule */
#define STORAGE_RULE_NONE 0
#define STORAGE_RULE_ROOT_SPEC 1 /* (a) root= */
#define STORAGE_RULE_IDENTITY 2 /* (b) loader disk + x86-64 root type */
#define STORAGE_RULE_LEGACY 3 /* (c) /LIKEOS.SIG, no identity */

/* Copy what the bootloader passed about the boot partition and the command
 * line out of boot_info.  Must be called early in system_startup(), while
 * the bootloader's low-memory boot_info is still mapped (like the SMBIOS
 * and display fields); later the identity map is gone. */
void storage_boot_info_save(const boot_info_t *bi);

void storage_fs_init(storage_fs_state_t *state);
void storage_fs_set_ready(storage_fs_state_t *state);

/* Look for the root and mount it; called periodically by the kdevpoll
 * thread (process context, may sleep).  Cheap once the root is mounted. */
void storage_fs_poll(storage_fs_state_t *state);

/* The mounted root: its device name without "/dev/" ("sda2", "sdb1"; NULL
 * until mounted), its filesystem type ("ext4" or "vfat"; NULL until
 * mounted), its block device, and how it was chosen (STORAGE_RULE_*).  For
 * /proc/mounts and diagnostics.  Any context. */
const char *storage_root_device_name(void);
const char *storage_root_fs_type(void);
const block_device_t *storage_root_bdev(void);
int storage_root_rule(void);

/* The boot command line the bootloader passed ("" when none), for
 * /proc/cmdline-style consumers.  Any context. */
const char *storage_boot_cmdline(void);

#endif // _KERNEL_STORAGE_H_
