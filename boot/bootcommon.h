/*
 * LikeOS boot loader -- what its parts share.
 *
 * bootloader.c is the loader proper; menu.c (the boot menu), gfx.c (its
 * renderer) and fallback.c (registering the firmware boot entry) are
 * compiled on their own and call back into it through these.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_BOOT_BOOTCOMMON_H
#define LIKEOS_BOOT_BOOTCOMMON_H

#include <efi.h>
#include <efilib.h>

/* The serial debug log (COM1, see bootloader.c). */
void bl_log(const char *s);
void bl_log_dec(UINT64 v);
void bl_log_hex(UINT64 v);

/* ---- the boot menu (menu.c) ---------------------------------------------- */

/* Where the menu's configuration lives on the loader's own partition.  The
 * live medium has it; an installed system does not, and starts straight
 * into LikeOS. */
#define BOOTMENU_CONF_PATH L"\\EFI\\LIKEOS\\bootmenu.conf"
#define BOOTMENU_MAX_ENTRIES 8
#define BOOTMENU_ARGS_MAX 256

/* Show the menu when the configuration exists and names at least one entry.
 * Returns the entry started (0-based) with its extra kernel command-line
 * words in `args' (printable ASCII, NUL-terminated, possibly empty), or -1
 * when there is no menu (args is then ""). */
INT32 bootmenu_run(EFI_HANDLE image, char *args, UINTN cap);

/* ---- the firmware boot entry (fallback.c) --------------------------------- */

/* Started from \EFI\BOOT\BOOTX64.EFI on a fixed disk whose system partition
 * has \EFI\LIKEOS\BOOTX64.CSV: make sure the firmware has a boot entry for
 * \EFI\LIKEOS\<file> on this partition, creating it (first in BootOrder)
 * when it is missing.  Never on removable media.  Logs what it did. */
void boot_fallback_register(EFI_HANDLE image);

#endif /* LIKEOS_BOOT_BOOTCOMMON_H */
