// LikeOS Explorer: the device service's line protocol.
//
// devd, the system's device service, talks in lines of text: words
// separated by one space, the first naming what the line is (OK, ERR, DEV,
// FS, EVENT, PROGRESS), then plain words (a verb, a device name, a number)
// and key=value pairs.  A value carries every byte outside the printable
// range, and '%' and '=', as %XX.  This file only reads and writes such
// lines -- strictly: a line that breaks the rules is refused as a whole,
// keys it does not know are skipped, every field is bounded -- so it can be
// tested without a socket and without the service.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef EX_DEVD_PROTO_H
#define EX_DEVD_PROTO_H

#include <glib.h>

G_BEGIN_DECLS

/* Where the service listens. */
#ifndef EX_DEVD_SOCKET
#define EX_DEVD_SOCKET "/var/run/devd/socket"
#endif
/* The longest line either side may send, without its newline. */
#define EX_DEVD_LINE_MAX 4096
/* Plain words and pairs on one line, at most. */
#define EX_DEVD_TOKENS_MAX 64

/* ---- errors ------------------------------------------------------------- */

#define EX_DEVD_ERROR (ex_devd_error_quark())
GQuark ex_devd_error_quark(void);

typedef enum {
	EX_DEVD_ERROR_FAILED, /* an error name not listed below */
	EX_DEVD_ERROR_PERM, /* EPERM, EACCES: not allowed */
	EX_DEVD_ERROR_BUSY, /* EBUSY: in use */
	EX_DEVD_ERROR_NOENT, /* ENOENT: no such device */
	EX_DEVD_ERROR_INVAL, /* EINVAL: a value refused */
	EX_DEVD_ERROR_NODEV, /* ENODEV, ENXIO: the device went */
	EX_DEVD_ERROR_IO, /* EIO */
	EX_DEVD_ERROR_NOSPC, /* ENOSPC */
	EX_DEVD_ERROR_AGAIN, /* EAGAIN: too many connections */
	EX_DEVD_ERROR_NOTSUP, /* ENOTSUP, EOPNOTSUPP, ENOSYS */
	EX_DEVD_ERROR_ROFS, /* EROFS */
	EX_DEVD_ERROR_NOT_RUNNING, /* no service, or the connection was lost */
	EX_DEVD_ERROR_PROTOCOL, /* an answer that breaks the protocol */
	EX_DEVD_ERROR_UNCLEAN, /* EUCLEAN: the file system's journal must be
				  replayed before it is mounted (ask the
				  user, then mount with recovery) */
} ExDevdErrorCode;

/* The code for an error name ("EBUSY"). */
ExDevdErrorCode ex_devd_error_code(const char *errname);
/* A sentence saying what an error means to the person at the screen. */
const char *ex_devd_error_explain(ExDevdErrorCode code);

/* ---- values ------------------------------------------------------------- */

/* A value as it is written on a line: every byte outside 0x21..0x7e, and
 * '%' and '=', as %XX in upper-case hex. */
char *ex_devd_encode(const char *value);
/* The value back.  NULL when the text holds a '%' without two hex digits
 * after it, a byte outside 0x21..0x7e, or an encoded NUL. */
char *ex_devd_decode(const char *text);

/* ---- lines -------------------------------------------------------------- */

typedef struct {
	/* The plain words, in order, as written (not decoded): "OK",
	 * "MOUNT", "sdb1".  At least one. */
	GPtrArray *words;
	/* The pairs, in order: keys[i] (a-z, 0-9, '_', at most 32 bytes)
	 * and vals[i], decoded. */
	GPtrArray *keys, *vals;
} ExDevdLine;

/* One line, without its newline.  NULL when it is not one: empty, too
 * long, a byte outside 0x20..0x7e, an empty token (two spaces), a word
 * after the first pair, a bad key or value, too many tokens. */
ExDevdLine *ex_devd_line_parse(const char *line, gsize len);
void ex_devd_line_free(ExDevdLine *l);
/* The `i'th plain word, or NULL. */
const char *ex_devd_line_word(const ExDevdLine *l, guint i);
/* The value of `key' (the first one written), or NULL. */
const char *ex_devd_line_get(const ExDevdLine *l, const char *key);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ExDevdLine, ex_devd_line_free)

/* A device name as the service writes it: a-z, A-Z, 0-9, '_' and '-', one
 * to 31 bytes.  Nothing else is ever put in a request or a location. */
gboolean ex_devd_name_valid(const char *name);

/* ---- what the lines describe ---------------------------------------------- */

/* A DEV or EVENT line: a disk or a partition. */
typedef struct {
	char *name; /* "sdb1" */
	char *disk; /* "sdb" */
	gboolean is_disk; /* kind=disk, else a partition */
	guint64 size; /* bytes */
	gboolean removable, usb, system;
	char *fstype; /* "vfat", "ext4", "ntfs", ... "unknown", "none" */
	gboolean supported; /* the service can mount it */
	char *label, *uuid, *vendor, *model; /* "" when none */
	char *mounted; /* the mount path, "" when not mounted */
	gint64 mountuid; /* -1 when not mounted by the service */
	gboolean ejected;
	char *state; /* "ready", "busy", "formatting" */
	/* Added later, at the end of the line; a service that does not send
	 * them leaves them FALSE. */
	gboolean readonly; /* write-protected or a disc: mounted read-only */
	gboolean optical; /* a CD/DVD drive */
	gboolean internal; /* a built-in disk: mount and unmount only */
} ExDevdDevice;

/* The device a DEV or EVENT line describes, from its pairs; NULL when the
 * name, the disk or the kind is missing or wrong.  Other fields that are
 * missing or out of range take a safe value (0, "", "unknown"). */
ExDevdDevice *ex_devd_device_from_line(const ExDevdLine *l);
ExDevdDevice *ex_devd_device_copy(const ExDevdDevice *d);
void ex_devd_device_free(ExDevdDevice *d);

/* An FS line: a filesystem the service knows. */
typedef struct {
	char *id; /* "vfat" */
	char *name; /* "FAT32" */
	gboolean is_default;
	guint labelmax; /* bytes */
	GArray *clusters; /* guint64, allocation unit sizes in bytes */
	guint64 defcluster;
	gboolean can_mount, can_format;
} ExDevdFs;

ExDevdFs *ex_devd_fs_from_line(const ExDevdLine *l);
ExDevdFs *ex_devd_fs_copy(const ExDevdFs *f);
void ex_devd_fs_free(ExDevdFs *f);

/* An ERR line: "ERR <verb> [<name>] <ERRNAME> [<message>]".  FALSE when
 * it is not one; else the verb, the name (or NULL), the error name and the
 * decoded message ("" when none) -- the strings owned by the caller. */
gboolean ex_devd_parse_err(const ExDevdLine *l, char **verb, char **name,
			   char **errname, char **message);

G_END_DECLS

#endif
