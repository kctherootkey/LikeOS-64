/* libinstaller -- the tar format: headers, pax records, sparse maps, ACLs.
 *
 * The pieces of the archive format that have nothing to do with files on
 * disk, kept apart from the extractor (il_extract.c) so they can be read
 * and tested on their own:
 *
 *   il_tar.c   512-byte headers (ustar, GNU, the old v7 layout), header
 *              checksums, octal and base-256 numbers, pax extended records
 *              (per entry 'x' and global 'g'), time stamps with fractions,
 *              base64 and URL decoding for the LIBARCHIVE.xattr records,
 *              the sparse maps of the GNU formats 0.0, 0.1, 1.0 and of the
 *              old 'S' header.
 *   il_acl.c   ACL text (SCHILY.acl.access / .default) to the binary value
 *              of the system.posix_acl_access / _default attributes.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef IL_TAR_H
#define IL_TAR_H

#include <stddef.h>
#include <stdint.h>

#define IL_TAR_BLOCK 512u

/* Type flags. */
#define IL_TAR_REG       '0'
#define IL_TAR_AREG      '\0'   /* pre-POSIX regular file */
#define IL_TAR_LNK       '1'
#define IL_TAR_SYM       '2'
#define IL_TAR_CHR       '3'
#define IL_TAR_BLK       '4'
#define IL_TAR_DIR       '5'
#define IL_TAR_FIFO      '6'
#define IL_TAR_CONT      '7'
#define IL_TAR_PAX       'x'
#define IL_TAR_PAX_G     'g'
#define IL_TAR_SUN_PAX   'X'    /* Solaris spelling of 'x' */
#define IL_TAR_GNU_DUMPDIR 'D'
#define IL_TAR_GNU_LONGLINK 'K'
#define IL_TAR_GNU_LONGNAME 'L'
#define IL_TAR_GNU_MULTIVOL 'M'
#define IL_TAR_GNU_NAMES 'N'
#define IL_TAR_GNU_SPARSE 'S'
#define IL_TAR_GNU_VOLHDR 'V'

/* The header block, field by field (all text, NUL-padded when shorter). */
struct il_tar_hdr_raw {
	char name[100];      /*   0 */
	char mode[8];        /* 100 */
	char uid[8];         /* 108 */
	char gid[8];         /* 116 */
	char size[12];       /* 124 */
	char mtime[12];      /* 136 */
	char chksum[8];      /* 148 */
	char typeflag;       /* 156 */
	char linkname[100];  /* 157 */
	char magic[6];       /* 257 */
	char version[2];     /* 263 */
	char uname[32];      /* 265 */
	char gname[32];      /* 297 */
	char devmajor[8];    /* 329 */
	char devminor[8];    /* 337 */
	union {
		char prefix[155];        /* 345, ustar */
		struct {
			char atime[12];      /* 345 */
			char ctime[12];      /* 357 */
			char offset[12];     /* 369 */
			char longnames[4];   /* 381 */
			char unused;         /* 385 */
			char sparse[4][24];  /* 386: offset[12] numbytes[12] */
			char isextended;     /* 482 */
			char realsize[12];   /* 483 */
			char pad[5];         /* 495 */
		} gnu;
	} u;
	char tail[12];       /* 500 */
} __attribute__((packed));

/* An extension block of an old GNU sparse header. */
struct il_tar_sparse_ext {
	char sparse[21][24];
	char isextended;
	char pad[7];
} __attribute__((packed));

enum il_tar_magic {
	IL_TAR_MAGIC_V7 = 0,     /* no magic: the original layout */
	IL_TAR_MAGIC_USTAR,      /* "ustar\0" "00": POSIX, prefix field */
	IL_TAR_MAGIC_GNU,        /* "ustar " " \0": GNU, atime/sparse fields */
};

int il_tar_is_zero_block(const uint8_t *blk);
/* 1 when the stored checksum matches the unsigned (or the historic signed)
 * sum of the block with the checksum field read as blanks. */
int il_tar_checksum_ok(const uint8_t *blk);
/* Fills the checksum field (tests write archives with it). */
void il_tar_checksum_set(uint8_t *blk);
int il_tar_magic(const struct il_tar_hdr_raw *h);

/* A numeric field: octal (blanks/NULs around it) or base-256 (first byte
 * 0x80 positive, 0xff negative).  0 or -EINVAL. */
int il_tar_number(const char *field, size_t len, int64_t *out);
int il_tar_unumber(const char *field, size_t len, uint64_t *out);
/* "1234", "1234.5678", "-12.5" -> seconds + nanoseconds (nsec always
 * 0..999999999, the seconds floored).  0 or -EINVAL. */
int il_tar_time(const char *s, size_t len, int64_t *sec, long *nsec);
/* A copy of a field up to its first NUL (malloc'd). */
char *il_tar_field_dup(const char *field, size_t len);

/* ---- pax --------------------------------------------------------- */

struct il_tar_xattr {
	char *name;
	uint8_t *value;
	size_t len;
};

struct il_tar_sparse {
	uint64_t offset;
	uint64_t len;
};

/* What pax records said.  `have' marks the fields that are set. */
#define IL_PAX_PATH      (1u << 0)
#define IL_PAX_LINKPATH  (1u << 1)
#define IL_PAX_SIZE      (1u << 2)
#define IL_PAX_UID       (1u << 3)
#define IL_PAX_GID       (1u << 4)
#define IL_PAX_MTIME     (1u << 5)
#define IL_PAX_ATIME     (1u << 6)
#define IL_PAX_UNAME     (1u << 7)
#define IL_PAX_GNAME     (1u << 8)
#define IL_PAX_DEVMAJOR  (1u << 9)
#define IL_PAX_DEVMINOR  (1u << 10)
#define IL_PAX_SP_MAJOR  (1u << 11)
#define IL_PAX_SP_MINOR  (1u << 12)
#define IL_PAX_SP_NAME   (1u << 13)
#define IL_PAX_SP_REAL   (1u << 14)
#define IL_PAX_SP_MAP    (1u << 15)   /* 0.0 / 0.1 map records */
#define IL_PAX_ACL_ACC   (1u << 16)
#define IL_PAX_ACL_DEF   (1u << 17)
#define IL_PAX_FILETYPE  (1u << 18)
#define IL_PAX_SP_BLOCKS (1u << 19)

struct il_tar_pax {
	unsigned have;
	char *path;
	char *linkpath;
	char *sp_name;
	char *acl_access;
	char *acl_default;
	char filetype[16];
	char uname[64];
	char gname[64];
	uint64_t size;
	uint64_t uid;
	uint64_t gid;
	int64_t mtime_sec;
	long mtime_nsec;
	int64_t atime_sec;
	long atime_nsec;
	uint64_t devmajor;
	uint64_t devminor;
	int sp_major;
	int sp_minor;
	uint64_t sp_realsize;
	uint64_t sp_numblocks;
	/* xattrs (SCHILY.xattr.*, LIBARCHIVE.xattr.*) */
	struct il_tar_xattr *xattrs;
	size_t nxattrs;
	size_t xattr_cap;
	/* the 0.0 / 0.1 sparse map */
	struct il_tar_sparse *map;
	size_t nmap;
	size_t map_cap;
	int map_pending;          /* 0.0: an offset waits for its numbytes */
	uint64_t map_pending_off;
	/* records not understood (counted for the log) */
	unsigned unknown;
};

void il_tar_pax_init(struct il_tar_pax *p);
void il_tar_pax_free(struct il_tar_pax *p);
/* Parses the records of a pax header into p (later records win).  On
 * failure err gets a sentence and -EINVAL comes back. */
int il_tar_pax_parse(struct il_tar_pax *p, const char *data, size_t len,
		     char *err, size_t errlen);
/* Adds/replaces one xattr (copies). */
int il_tar_pax_add_xattr(struct il_tar_pax *p, const char *name,
			 const uint8_t *value, size_t len);
int il_tar_pax_add_sparse(struct il_tar_pax *p, uint64_t off, uint64_t len);

/* ---- decoding helpers -------------------------------------------- */

/* Base64 (standard alphabet, padding optional, blanks ignored).  out must
 * hold 3 * (len / 4) + 3 bytes.  Returns the decoded length or -EINVAL. */
long il_base64_decode(const char *in, size_t len, uint8_t *out);
/* The attribute name of a SCHILY.xattr keyword ("%3D" -> '=', "%25" ->
 * '%'), malloc'd; NULL when out of memory. */
char *il_xattr_keyword_decode(const char *in, size_t len);
/* %XX decoding into a malloc'd string; NULL on bad input. */
char *il_url_decode(const char *in, size_t len);

/* ---- sparse maps ------------------------------------------------- */

/* The 0.1 GNU.sparse.map value "off,len,off,len,...". */
int il_tar_sparse_parse_list(const char *s, struct il_tar_pax *p);
/* The 4 (or 21) "offset[12] numbytes[12]" pairs of an old GNU header or
 * extension block; empty pairs end the list. */
int il_tar_sparse_parse_old(const char (*pairs)[24], int n,
			    struct il_tar_pax *p);

/* ---- ACLs (il_acl.c) ------------------------------------------- */

#define IL_ACL_XATTR_ACCESS  "system.posix_acl_access"
#define IL_ACL_XATTR_DEFAULT "system.posix_acl_default"

/* Turns ACL text ("user::rwx,user:1000:r-x,group::r-x,mask::r-x,other::r--"
 * in long or short tag spelling, separated by ',' or line ends, also the
 * "user:name:rwx:1000" form with the numeric id last) into the binary
 * attribute value.  *base_only is set when the ACL has nothing beyond the
 * owner/group/other entries (an access ACL like that equals the mode).
 * Returns 0 with a malloc'd *out, or -EINVAL with a sentence in err. */
int il_acl_from_text(const char *text, uint8_t **out, size_t *outlen,
		     int *base_only, char *err, size_t errlen);

#endif /* IL_TAR_H */
