/*
 * LikeOS boot loader -- registering the firmware boot entry from
 * \EFI\BOOT\BOOTX64.EFI.
 *
 * The installer copies this loader twice onto the target's system
 * partition: as \EFI\LIKEOS\BOOTX64.EFI, the file the firmware boot entry
 * "LikeOS" names, and as \EFI\BOOT\BOOTX64.EFI, the path every firmware
 * tries on a disk it has no entry for.  It also writes
 * \EFI\LIKEOS\BOOTX64.CSV, one line describing that entry:
 *
 *     BOOTX64.EFI,LikeOS,,This is the boot entry for LikeOS
 *     (file in \EFI\LIKEOS, label, options, comment; UTF-16LE)
 *
 * and asks the firmware for the entry through the variable services
 * (likeos-efiboot).  When that did not happen -- the firmware refused, the
 * machine forgot its entries (a firmware update, a reset to defaults, a
 * disk moved to another machine) -- the firmware ends up starting
 * \EFI\BOOT\BOOTX64.EFI, and this file puts the entry back:
 *
 *   1. only when this loader was started as \EFI\BOOT\BOOTX64.EFI, from a
 *      partition of a FIXED disk (never removable media, never a disk the
 *      firmware reached over USB -- the live medium must not register
 *      itself), whose system partition has \EFI\LIKEOS\BOOTX64.CSV and
 *      the loader file it names, and no boot menu configuration;
 *   2. the entry's device path is this partition's Hard Drive node and a
 *      File node \EFI\LIKEOS\<file> (the short form, which the firmware
 *      expands to the disk wherever it is attached);
 *   3. every Boot#### is compared with it (partition signature and number,
 *      file path without regard to case); one that matches is put back at
 *      the front of BootOrder if it is missing from there, and otherwise
 *      left alone;
 *   4. without one, the lowest free Boot#### is written -- attributes
 *      ACTIVE, the label as description, the options (UCS-2) as optional
 *      data -- and put first in BootOrder.
 *
 * Booting then simply goes on: this is the same loader, so the system
 * starts as it would have from the entry.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "bootcommon.h"
#include "efi_loadopt.h"

#define FB_CSV_PATH L"\\EFI\\LIKEOS\\BOOTX64.CSV"
#define FB_VENDOR_DIR "\\EFI\\LIKEOS\\"
#define FB_REMOVABLE_PATH "\\EFI\\BOOT\\BOOTX64.EFI"
#define FB_CSV_MAX 4096
#define FB_FIELD_MAX 256
#define FB_DP_MAX 1024
#define FB_OPT_MAX 4096

#define FB_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
		 EFI_VARIABLE_RUNTIME_ACCESS)

static EFI_GUID g_fb_global = { 0x8be4df61, 0x93ca, 0x11d2,
				{ 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } };

static void fb_say(const char *s)
{
	bl_log("fallback: ");
	bl_log(s);
	bl_log("\n");
	Print(L"Boot entry: %a\r\n", s);
}

static char fb_upper(char c)
{
	return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* The file path of a device path (its File nodes joined), ASCII-folded
 * and upper-cased, '/' as '\'; FALSE when it has none or it is too long. */
static BOOLEAN fb_image_path(const EFI_DEVICE_PATH *fp, char *out, UINTN cap)
{
	const UINT8 *p = (const UINT8 *)fp;
	UINTN o = 0, off = 0;
	BOOLEAN any = FALSE;

	if (!fp || cap < 2)
		return FALSE;
	for (UINTN n = 0; n < ELO_DP_MAX_NODES; n++) {
		UINT8 type = p[off] & 0x7f, sub = p[off + 1];
		UINTN len = (UINTN)p[off + 2] | ((UINTN)p[off + 3] << 8);

		if (len < 4 || off + len > 4096)
			return FALSE;
		if (type == ELO_DP_END && sub == ELO_DP_END_ENTIRE)
			break;
		if (type == ELO_DP_MEDIA && sub == ELO_MEDIA_FILE && len > 4) {
			/* one separator between nodes, whatever each says */
			if (o > 0 && out[o - 1] != '\\' && p[off + 4] != '\\' &&
			    p[off + 4] != '/') {
				if (o + 1 >= cap)
					return FALSE;
				out[o++] = '\\';
			}
			for (UINTN i = 4; i + 1 < len; i += 2) {
				UINT16 c = (UINT16)(p[off + i] | (p[off + i + 1] << 8));

				if (!c)
					break;
				if (o + 1 >= cap)
					return FALSE;
				c = c == '/' ? '\\' : c;
				if (c == '\\' && o > 0 && out[o - 1] == '\\')
					continue;
				out[o++] = c < 0x80 ? fb_upper((char)c) : '?';
			}
			any = TRUE;
		}
		off += len;
	}
	out[o] = 0;
	return any;
}

static BOOLEAN fb_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* The bytes of \EFI\LIKEOS\BOOTX64.CSV (or of another file) on the loader's
 * file system; *len 0 when missing. */
static BOOLEAN fb_file_exists(EFI_FILE_HANDLE root, CHAR16 *path)
{
	EFI_FILE_HANDLE f = NULL;

	if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, path,
					EFI_FILE_MODE_READ, 0ULL)) ||
	    !f)
		return FALSE;
	uefi_call_wrapper(f->Close, 1, f);
	return TRUE;
}

static UINTN fb_read_file(EFI_FILE_HANDLE root, CHAR16 *path, UINT8 *buf,
			  UINTN cap)
{
	EFI_FILE_HANDLE f = NULL;
	UINTN got = 0;

	if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, path,
					EFI_FILE_MODE_READ, 0ULL)) ||
	    !f)
		return 0;
	while (got < cap) {
		UINTN chunk = cap - got;

		if (EFI_ERROR(uefi_call_wrapper(f->Read, 3, f, &chunk, buf + got)) ||
		    chunk == 0)
			break;
		got += chunk;
	}
	uefi_call_wrapper(f->Close, 1, f);
	return got;
}

static void fb_boot_name(CHAR16 *name, UINT16 num)
{
	static const char hex[] = "0123456789ABCDEF";

	name[0] = 'B';
	name[1] = 'o';
	name[2] = 'o';
	name[3] = 't';
	name[4] = hex[(num >> 12) & 15];
	name[5] = hex[(num >> 8) & 15];
	name[6] = hex[(num >> 4) & 15];
	name[7] = hex[num & 15];
	name[8] = 0;
}

/* Boot#### under the global GUID: its number, or -1. */
static INT32 fb_boot_number(const CHAR16 *n, const EFI_GUID *g)
{
	INT32 v = 0;

	if (CompareMem((VOID *)g, &g_fb_global, sizeof(EFI_GUID)) != 0)
		return -1;
	if (n[0] != 'B' || n[1] != 'o' || n[2] != 'o' || n[3] != 't')
		return -1;
	for (int i = 4; i < 8; i++) {
		CHAR16 c = n[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= c - '0';
		else if (c >= 'A' && c <= 'F')
			v |= c - 'A' + 10;
		else
			return -1;
	}
	return n[8] == 0 ? v : -1;
}

/* A variable in a pool buffer (caller frees); NULL when missing. */
static UINT8 *fb_get_var(CHAR16 *name, UINTN *size, UINT32 *attr)
{
	UINT8 *buf = NULL;
	UINTN sz = 0;
	EFI_STATUS st;

	*size = 0;
	st = uefi_call_wrapper(RT->GetVariable, 5, name, &g_fb_global, attr, &sz,
			       NULL);
	if (st != EFI_BUFFER_TOO_SMALL || sz == 0 || sz > 65536)
		return NULL;
	if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, sz,
					(VOID **)&buf)) ||
	    !buf)
		return NULL;
	st = uefi_call_wrapper(RT->GetVariable, 5, name, &g_fb_global, attr, &sz,
			       buf);
	if (EFI_ERROR(st)) {
		uefi_call_wrapper(BS->FreePool, 1, buf);
		return NULL;
	}
	*size = sz;
	return buf;
}

static void fb_log_entry(const char *what, UINT16 num)
{
	CHAR16 name[9];
	char a[9];

	fb_boot_name(name, num);
	for (int i = 0; i < 9; i++)
		a[i] = (char)name[i];
	bl_log("fallback: ");
	bl_log(what);
	bl_log(" ");
	bl_log(a);
	bl_log("\n");
	Print(L"Boot entry: %a %a\r\n", what, a);
}

/* Put `num' first in BootOrder (dropping any other occurrence of it). */
static EFI_STATUS fb_order_first(UINT16 num)
{
	UINT16 *order = NULL, *fresh = NULL;
	UINTN size = 0, n, o = 0;
	UINT32 attr = 0;
	EFI_STATUS st;

	order = (UINT16 *)fb_get_var(L"BootOrder", &size, &attr);
	if (!order) {
		/* Missing is fine (a new order of one); present but unreadable
		 * must not be replaced by an order of one. */
		UINTN probe = 0;

		st = uefi_call_wrapper(RT->GetVariable, 5, L"BootOrder",
				       &g_fb_global, &attr, &probe, NULL);
		if (st != EFI_NOT_FOUND)
			return EFI_ERROR(st) && st != EFI_BUFFER_TOO_SMALL ?
				       st : EFI_DEVICE_ERROR;
	}
	n = order ? size / 2 : 0;
	if (n && order[0] == num) {
		uefi_call_wrapper(BS->FreePool, 1, order);
		return EFI_SUCCESS;
	}
	st = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
			       (n + 1) * sizeof(UINT16), (VOID **)&fresh);
	if (EFI_ERROR(st) || !fresh) {
		if (order)
			uefi_call_wrapper(BS->FreePool, 1, order);
		return EFI_OUT_OF_RESOURCES;
	}
	fresh[o++] = num;
	for (UINTN i = 0; i < n; i++)
		if (order[i] != num)
			fresh[o++] = order[i];
	st = uefi_call_wrapper(RT->SetVariable, 5, L"BootOrder", &g_fb_global,
			       FB_ATTR, o * sizeof(UINT16), fresh);
	uefi_call_wrapper(BS->FreePool, 1, fresh);
	if (order)
		uefi_call_wrapper(BS->FreePool, 1, order);
	return st;
}

static BOOLEAN fb_in_order(UINT16 num)
{
	UINTN size = 0;
	UINT32 attr = 0;
	UINT16 *order = (UINT16 *)fb_get_var(L"BootOrder", &size, &attr);
	BOOLEAN found = FALSE;

	for (UINTN i = 0; order && i < size / 2; i++)
		if (order[i] == num)
			found = TRUE;
	if (order)
		uefi_call_wrapper(BS->FreePool, 1, order);
	return found;
}

/* Does the device path reach the disk over USB? */
static BOOLEAN fb_dp_has_usb(const UINT8 *dp, UINTN len)
{
	UINTN off = 0;

	for (UINTN n = 0; n < ELO_DP_MAX_NODES && off + 4 <= len; n++) {
		UINT8 type = dp[off] & 0x7f, sub = dp[off + 1];
		UINTN nl = (UINTN)dp[off + 2] | ((UINTN)dp[off + 3] << 8);

		if (nl < 4 || type == ELO_DP_END)
			break;
		/* USB, USB WWID, USB class, USB logical unit */
		if (type == ELO_DP_MESSAGING &&
		    (sub == 0x05 || sub == 0x0f || sub == 0x10 || sub == 0x11))
			return TRUE;
		off += nl;
	}
	return FALSE;
}

void boot_fallback_register(EFI_HANDLE image)
{
	EFI_GUID blk_guid = BLOCK_IO_PROTOCOL;
	EFI_LOADED_IMAGE *li = NULL;
	EFI_DEVICE_PATH *ddp = NULL;
	EFI_BLOCK_IO_PROTOCOL *blk = NULL;
	EFI_FILE_IO_INTERFACE *fs = NULL;
	EFI_FILE_HANDLE root = NULL;
	char started[512];
	static UINT8 csv[FB_CSV_MAX];
	static UINT8 dp[FB_DP_MAX];
	static UINT8 opt[FB_OPT_MAX];
	static UINT8 optdata[FB_FIELD_MAX * 2 + 2];
	char file[FB_FIELD_MAX], label[FB_FIELD_MAX], opts[FB_FIELD_MAX];
	char path[FB_FIELD_MAX + 16];
	CHAR16 wpath[FB_FIELD_MAX + 16];
	struct elo_hd hd;
	UINTN dlen, csvlen, dplen, optlen, optdlen = 0;
	INT32 match = -1;
	BOOLEAN walked = FALSE; /* every variable was seen */
	static UINT8 used[65536 / 8]; /* Boot#### numbers in use */
	EFI_STATUS st;

	/* 1. Started as the removable-media loader? */
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, image,
					&LoadedImageProtocol, (VOID **)&li)) ||
	    !li || !li->DeviceHandle)
		return;
	if (!fb_image_path(li->FilePath, started, sizeof(started))) {
		bl_log("fallback: the loader's file path is unknown, nothing to do\n");
		return;
	}
	if (!fb_streq(started, FB_REMOVABLE_PATH)) {
		bl_log("fallback: started as ");
		bl_log(started);
		bl_log(", not from \\EFI\\BOOT: nothing to do\n");
		return;
	}
	/* ... from a fixed disk, not over USB */
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
					&blk_guid, (VOID **)&blk)) ||
	    !blk || !blk->Media) {
		bl_log("fallback: no block device under the loader, nothing to do\n");
		return;
	}
	if (blk->Media->RemovableMedia) {
		bl_log("fallback: removable medium, no boot entry is registered\n");
		return;
	}
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
					&DevicePathProtocol, (VOID **)&ddp)) ||
	    !ddp)
		return;
	dlen = elo_dp_length((const elo_u8 *)ddp, 4096);
	if (!dlen || !elo_dp_find_hd((const elo_u8 *)ddp, dlen, &hd, NULL)) {
		bl_log("fallback: the loader is not on a disk partition, nothing to do\n");
		return;
	}
	if (fb_dp_has_usb((const UINT8 *)ddp, dlen)) {
		bl_log("fallback: disk attached over USB, no boot entry is registered\n");
		return;
	}
	if (hd.sig_type != ELO_HD_SIG_GUID && hd.sig_type != ELO_HD_SIG_MBR) {
		bl_log("fallback: the partition has no signature, nothing to do\n");
		return;
	}

	/* ... with the CSV, its loader file, and no boot menu */
	if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
					&FileSystemProtocol, (VOID **)&fs)) ||
	    !fs || EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root)) ||
	    !root)
		return;
	csvlen = fb_read_file(root, FB_CSV_PATH, csv, sizeof(csv));
	if (!csvlen) {
		bl_log("fallback: no \\EFI\\LIKEOS\\BOOTX64.CSV, nothing to do\n");
		goto out;
	}
	if (fb_file_exists(root, BOOTMENU_CONF_PATH)) {
		bl_log("fallback: this partition has a boot menu (live medium), no boot entry is registered\n");
		goto out;
	}
	if (elo_csv_parse(csv, csvlen, file, sizeof(file), label, sizeof(label),
			  opts, sizeof(opts))) {
		fb_say("BOOTX64.CSV has no usable line");
		goto out;
	}
	for (UINTN i = 0; file[i]; i++) {
		if (file[i] == '\\' || file[i] == '/' || file[i] == ':') {
			fb_say("BOOTX64.CSV names a file outside \\EFI\\LIKEOS");
			goto out;
		}
	}
	{
		UINTN o = 0;
		const char *pre = FB_VENDOR_DIR;

		for (UINTN i = 0; pre[i]; i++)
			path[o++] = pre[i];
		for (UINTN i = 0; file[i] && o + 1 < sizeof(path); i++)
			path[o++] = file[i];
		path[o] = 0;
		for (UINTN i = 0; i <= o; i++)
			wpath[i] = (CHAR16)(unsigned char)path[i];
	}
	if (!fb_file_exists(root, wpath)) {
		bl_log("fallback: ");
		bl_log(path);
		bl_log(" (named by BOOTX64.CSV) is missing, nothing registered\n");
		goto out;
	}
	if (!label[0]) {
		static const char dflt[] = "LikeOS";

		for (UINTN i = 0; i < sizeof(dflt); i++)
			label[i] = dflt[i];
	}

	/* 2. The entry's device path and load option. */
	dplen = elo_dp_hd_file(&hd, path, dp, sizeof(dp));
	if (!dplen) {
		fb_say("cannot build the device path");
		goto out;
	}
	if (opts[0]) {
		optdlen = elo_utf8_to_ucs2(opts, optdata, sizeof(optdata));
		if (!optdlen) {
			fb_say("BOOTX64.CSV options are not valid text, left out");
		}
	}
	optlen = elo_load_option_build(ELO_LOAD_OPTION_ACTIVE, label, dp, dplen,
				       optdlen ? optdata : NULL, optdlen, opt,
				       sizeof(opt));
	if (!optlen) {
		fb_say("cannot build the load option");
		goto out;
	}

	/* 3. Is there one already?  Every Boot#### variable is looked at,
	 * not only those in BootOrder: the firmware may have dropped one from
	 * the order and kept the variable. */
	SetMem(used, sizeof(used), 0);
	{
		CHAR16 *name = NULL;
		UINTN cap = 1024;
		EFI_GUID guid;

		if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3,
						EfiLoaderData, cap,
						(VOID **)&name)) ||
		    !name)
			goto out;
		name[0] = 0;
		SetMem(&guid, sizeof(guid), 0);
		for (UINTN iter = 0; iter < 8192; iter++) {
			UINTN sz = cap;
			INT32 num;

			st = uefi_call_wrapper(RT->GetNextVariableName, 3, &sz, name,
					       &guid);
			if (st == EFI_BUFFER_TOO_SMALL && sz > cap && sz <= 65536) {
				/* a long name: a bigger buffer, same name */
				CHAR16 *bigger = NULL;

				if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3,
								EfiLoaderData, sz,
								(VOID **)&bigger)) ||
				    !bigger)
					break;
				CopyMem(bigger, name, cap);
				uefi_call_wrapper(BS->FreePool, 1, name);
				name = bigger;
				cap = sz;
				iter--;
				continue;
			}
			if (st == EFI_NOT_FOUND) {
				walked = TRUE;
				break;
			}
			if (EFI_ERROR(st)) {
				/* a longer name than we have room for: the
				 * walk cannot go on past it */
				bl_log("fallback: variable walk stopped early\n");
				break;
			}
			num = fb_boot_number(name, &guid);
			if (num < 0)
				continue;
			used[num / 8] |= (UINT8)(1u << (num % 8));
			if (match < 0) {
				UINTN vs = 0;
				UINT32 va = 0;
				CHAR16 vn[9];
				UINT8 *v;
				struct elo_load_option lo;

				fb_boot_name(vn, (UINT16)num);
				v = fb_get_var(vn, &vs, &va);
				if (v && !elo_load_option_parse(v, vs, &lo) &&
				    elo_dp_matches(lo.dp, lo.dp_len, &hd, path))
					match = num;
				if (v)
					uefi_call_wrapper(BS->FreePool, 1, v);
			}
		}
		uefi_call_wrapper(BS->FreePool, 1, name);
	}

	if (match >= 0) {
		if (fb_in_order((UINT16)match)) {
			fb_log_entry("present as", (UINT16)match);
			goto out;
		}
		st = fb_order_first((UINT16)match);
		fb_log_entry(EFI_ERROR(st) ? "could not put back in BootOrder:" :
					     "put back first in BootOrder:",
			     (UINT16)match);
		goto out;
	}

	/* 4. A new one -- only when every Boot#### was seen: the lowest free
	 * number of a partial walk may be another system's entry. */
	if (!walked) {
		fb_say("the firmware's variables could not all be read, no entry created");
		goto out;
	}
	{
		INT32 num = -1;
		CHAR16 vn[9];

		for (INT32 i = 0; i < 65536; i++) {
			if (!(used[i / 8] & (1u << (i % 8)))) {
				num = i;
				break;
			}
		}
		if (num < 0) {
			fb_say("no free Boot#### number");
			goto out;
		}
		fb_boot_name(vn, (UINT16)num);
		st = uefi_call_wrapper(RT->SetVariable, 5, vn, &g_fb_global,
				       FB_ATTR, optlen, opt);
		if (EFI_ERROR(st)) {
			bl_log("fallback: SetVariable failed, status ");
			bl_log_hex((UINT64)st);
			bl_log("\n");
			Print(L"Boot entry: the firmware refused the new entry: %r\r\n",
			      st);
			goto out;
		}
		st = fb_order_first((UINT16)num);
		if (EFI_ERROR(st)) {
			bl_log("fallback: BootOrder could not be updated, status ");
			bl_log_hex((UINT64)st);
			bl_log("\n");
		}
		bl_log("fallback: ");
		bl_log(label);
		bl_log(" -> ");
		bl_log(path);
		bl_log("\n");
		fb_log_entry("created", (UINT16)num);
	}
out:
	uefi_call_wrapper(root->Close, 1, root);
}
