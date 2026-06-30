// LikeOS-64 - Common status and error codes
#ifndef LIKEOS_STATUS_H
#define LIKEOS_STATUS_H

typedef enum {
	ST_OK = 0,
	ST_ERR = -1,
	ST_UNSUPPORTED = -2,
	ST_NO_DEVICE = -3,
	ST_TIMEOUT = -4,
	ST_IO = -5,
	ST_NOT_FOUND = -6,
	ST_EOF = -7,
	ST_INVALID = -8,
	ST_NOMEM = -9,
	ST_AGAIN = -10, // temporary condition, try again later
	ST_BUSY = -11,
	ST_EXISTS = -12,
	ST_NOTEMPTY = -13,
	ST_ROFS = -14, // read-only filesystem (e.g. latched after a csum error)
	ST_NOSPC =
		-15, // no space left (e.g. xattr value too big for the inode)
	ST_NODATA = -16, // no such attribute (xattr) — maps to ENODATA
	ST_RANGE = -17, // result buffer too small (xattr) — maps to ERANGE
	ST_ACCESS = -18, // permission denied by mode/ACL — maps to EACCES
	ST_PERM =
		-19, // operation not permitted (ownership/privilege) — maps to EPERM
} status_t;

#endif // LIKEOS_STATUS_H
