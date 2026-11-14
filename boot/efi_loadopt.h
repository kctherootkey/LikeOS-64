/* efi_loadopt -- UEFI device paths, boot load options and BOOTX64.CSV.
 *
 * The byte formats a firmware boot entry is made of:
 *
 *   EFI_LOAD_OPTION   UINT32 Attributes, UINT16 FilePathListLength,
 *                     CHAR16 Description[] (NUL-terminated), the device
 *                     path list (FilePathListLength bytes), optional data
 *   device path       a chain of nodes { UINT8 Type, UINT8 SubType,
 *                     UINT16 Length, data }, ended by End Entire (7f ff)
 *   HD() node         the 42-byte Hard Drive media node naming a partition
 *                     by number, start, size and signature
 *   File() node       a NUL-terminated UCS-2 path
 *
 * plus the small CSV file (\EFI\<vendor>\BOOTX64.CSV) that describes the
 * boot entry a loader started from the removable-media path registers.
 *
 * One implementation for every user: the UEFI boot loader (gnu-efi flags),
 * the kernel (no C library, no SSE), the likeos-efiboot tool and the host
 * unit test.  So it includes nothing, defines its own integer types, reads
 * and writes every multi-byte field a byte at a time (device path nodes are
 * byte-packed and may sit at any address) and calls no library function.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_EFI_LOADOPT_H
#define LIKEOS_EFI_LOADOPT_H

typedef unsigned char elo_u8;
typedef unsigned short elo_u16;
typedef unsigned int elo_u32;
typedef unsigned long long elo_u64;
typedef __SIZE_TYPE__ elo_size;

/* Device path node types and the subtypes used here. */
#define ELO_DP_HARDWARE      0x01
#define ELO_DP_ACPI          0x02
#define ELO_DP_MESSAGING     0x03
#define ELO_DP_MEDIA         0x04
#define ELO_DP_BBS           0x05
#define ELO_DP_END           0x7f
#define ELO_DP_END_ENTIRE    0xff
#define ELO_DP_END_INSTANCE  0x01

#define ELO_MEDIA_HD         0x01
#define ELO_MEDIA_CDROM      0x02
#define ELO_MEDIA_VENDOR     0x03
#define ELO_MEDIA_FILE       0x04
#define ELO_MEDIA_PROTOCOL   0x05
#define ELO_MEDIA_FV_FILE    0x06
#define ELO_MEDIA_FV         0x07
#define ELO_MEDIA_OFFSET     0x08
#define ELO_MEDIA_RAMDISK    0x09

/* The Hard Drive media node: 4 header bytes, UINT32 PartitionNumber,
 * UINT64 PartitionStart, UINT64 PartitionSize, UINT8 Signature[16],
 * UINT8 MBRType, UINT8 SignatureType. */
#define ELO_HD_NODE_LEN      42
#define ELO_HD_MBR_TYPE_MBR  1
#define ELO_HD_MBR_TYPE_GPT  2
#define ELO_HD_SIG_NONE      0
#define ELO_HD_SIG_MBR       1
#define ELO_HD_SIG_GUID      2

/* EFI_LOAD_OPTION attribute bits. */
#define ELO_LOAD_OPTION_ACTIVE          0x00000001u
#define ELO_LOAD_OPTION_FORCE_RECONNECT 0x00000002u
#define ELO_LOAD_OPTION_HIDDEN          0x00000008u
#define ELO_LOAD_OPTION_CATEGORY        0x00001f00u
#define ELO_LOAD_OPTION_CATEGORY_BOOT   0x00000000u
#define ELO_LOAD_OPTION_CATEGORY_APP    0x00000100u

/* Bounds of what the builders and parsers here accept. */
#define ELO_PATH_MAX_UNITS   1024   /* UCS-2 units in a File() path */
#define ELO_DP_MAX_NODES     256    /* nodes walked in one device path */

/* A partition as a Hard Drive node describes it.  `signature' is the
 * unique partition GUID in EFI_GUID memory order (GPT) or the 32-bit MBR
 * disk signature, little-endian in bytes 0-3 (rest zero). */
struct elo_hd {
	elo_u32 part_number;   /* 1-based */
	elo_u64 part_start;    /* first LBA, in the disk's logical blocks */
	elo_u64 part_size;     /* length in blocks */
	elo_u8 signature[16];
	elo_u8 mbr_type;       /* ELO_HD_MBR_TYPE_* */
	elo_u8 sig_type;       /* ELO_HD_SIG_* */
};

/* ---- device paths --------------------------------------------------- */

/* HD(...)/File(path)/End.  `path' is UTF-8; '/' is taken as '\', and a
 * leading '\' is added when missing.  Returns the total bytes including
 * the End node; 0 when `cap' is too small or the input is invalid (an
 * empty path, invalid UTF-8, a code point beyond U+FFFF). */
elo_size elo_dp_hd_file(const struct elo_hd *hd, const char *path,
			elo_u8 *out, elo_size cap);

/* Bytes of `dp' up to and including its first End Entire node; 0 when it
 * is malformed (a node shorter than its header, running past `max', or no
 * End Entire node within `max' bytes). */
elo_size elo_dp_length(const elo_u8 *dp, elo_size max);

/* 1 when dp[0..len) is exactly one well-formed device path that ends in
 * End Entire at `len' (End Instance nodes allowed in between), else 0. */
int elo_dp_valid(const elo_u8 *dp, elo_size len);

/* The first Hard Drive media node of the path: 1 with *hd filled (and *off
 * its byte offset when off is not NULL), 0 when there is none. */
int elo_dp_find_hd(const elo_u8 *dp, elo_size len, struct elo_hd *hd,
		   elo_size *off);

/* 1 when the path -- short form (starting at the HD node) or long form
 * (the disk's full path in front) -- names the same partition as `hd'
 * (same signature type; GUID: same unique GUID and partition number; MBR:
 * same disk signature and partition number) followed by File node(s) whose
 * concatenated path equals `path', compared ASCII case-insensitively with
 * '/' and '\' alike. */
int elo_dp_matches(const elo_u8 *dp, elo_size len, const struct elo_hd *hd,
		   const char *path);

/* The usual text form of a device path: nodes separated by '/', instances
 * by ','.  For example
 *   PciRoot(0x0)/Pci(0x1f,0x2)/Sata(0x0,0xffff,0x0)/
 *   HD(1,GPT,0b1f...,0x800,0x100000)/File(\EFI\LIKEOS\BOOTX64.EFI)
 * Unknown nodes are shown as Path(type,subtype,hexdata).  Writes at most
 * cap-1 characters and a NUL (cap > 0); returns the length the full text
 * needs, as snprintf does. */
elo_size elo_dp_to_text(const elo_u8 *dp, elo_size len, char *out,
			elo_size cap);

/* ---- load options ---------------------------------------------------- */

/* An EFI_LOAD_OPTION from its parts.  `desc' is UTF-8 (stored as UCS-2);
 * `dp' must be a valid device path list of dp_len bytes; `opt' (may be
 * NULL when opt_len is 0) is copied as the optional data.  Returns the
 * bytes written, 0 on invalid input or when `cap' is too small. */
elo_size elo_load_option_build(elo_u32 attrs, const char *desc,
			       const elo_u8 *dp, elo_size dp_len,
			       const elo_u8 *opt, elo_size opt_len,
			       elo_u8 *out, elo_size cap);

/* A parsed load option; the pointers point into the parsed buffer. */
struct elo_load_option {
	elo_u32 attributes;
	elo_size dp_len;          /* FilePathListLength */
	const elo_u8 *desc;       /* UCS-2LE, NUL-terminated */
	elo_size desc_units;      /* without the NUL */
	const elo_u8 *dp;         /* the device path list */
	const elo_u8 *opt;        /* optional data (NULL when none) */
	elo_size opt_len;
};

/* 0 with *out filled when buf[0..len) is a well-formed load option: at
 * least 6 bytes, the description NUL-terminated inside the buffer, a
 * non-zero FilePathListLength inside the buffer whose first path is
 * well formed (elo_dp_length); -1 otherwise. */
int elo_load_option_parse(const elo_u8 *buf, elo_size len,
			  struct elo_load_option *out);

/* ---- text ------------------------------------------------------------ */

/* UCS-2LE (at most `units' units, stopping at a NUL) to UTF-8.  Surrogate
 * pairs are combined, a lone surrogate becomes U+FFFD.  `out' is always
 * NUL-terminated (cap > 0; the text is cut at a character boundary when it
 * does not fit); returns the bytes written without the NUL. */
elo_size elo_ucs2_to_utf8(const elo_u8 *ucs2le, elo_size units, char *out,
			  elo_size cap);

/* UTF-8 to UCS-2LE including the terminating NUL; returns the bytes
 * written including the NUL, 0 on invalid UTF-8, a code point beyond
 * U+FFFF (or a surrogate), or when `cap' is too small. */
elo_size elo_utf8_to_ucs2(const char *s, elo_u8 *out, elo_size cap);

/* EFI_GUID memory order (Data1..Data3 little-endian, Data4 as bytes) to
 * the canonical lower-case text "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx". */
void elo_guid_to_text(const elo_u8 g[16], char out[37]);

/* The reverse; upper or lower case, optionally in braces.  0 or -1. */
int elo_guid_from_text(const char *s, elo_u8 g[16]);

/* \EFI\<vendor>\BOOTX64.CSV: UTF-16LE with or without a byte order mark,
 * or a UTF-8 / ASCII file.  The first line that is neither empty nor a
 * '#' comment gives "filename,label,options,comment"; the first three are
 * stored NUL-terminated as UTF-8 (cut safely when they do not fit).  0, or
 * -1 when there is no such line or it has no file name. */
int elo_csv_parse(const elo_u8 *buf, elo_size len, char *file, elo_size fcap,
		  char *label, elo_size lcap, char *opts, elo_size ocap);

#endif /* LIKEOS_EFI_LOADOPT_H */
