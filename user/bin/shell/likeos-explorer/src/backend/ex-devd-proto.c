// LikeOS Explorer: the device service's line protocol.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-devd-proto.h"

#include <string.h>

/* Bounds of the fields, after decoding. */
#define KEY_MAX 32
#define LABEL_MAX 255
#define UUID_MAX 64
#define TEXT_MAX 64 /* vendor, model */
#define PATH_MAX_BYTES 4095
#define FSTYPE_MAX 16
#define FSNAME_MAX 32
#define CLUSTERS_MAX 32
#define CLUSTER_LARGEST (64u * 1024 * 1024)

G_DEFINE_QUARK(ex-devd-error-quark, ex_devd_error)

/* ---- errors ------------------------------------------------------------- */

ExDevdErrorCode ex_devd_error_code(const char *errname)
{
	static const struct {
		const char *name;
		ExDevdErrorCode code;
	} map[] = {
		{ "EPERM", EX_DEVD_ERROR_PERM },     { "EACCES", EX_DEVD_ERROR_PERM },
		{ "EBUSY", EX_DEVD_ERROR_BUSY },     { "ENOENT", EX_DEVD_ERROR_NOENT },
		{ "EINVAL", EX_DEVD_ERROR_INVAL },   { "ENODEV", EX_DEVD_ERROR_NODEV },
		{ "ENXIO", EX_DEVD_ERROR_NODEV },    { "EIO", EX_DEVD_ERROR_IO },
		{ "ENOSPC", EX_DEVD_ERROR_NOSPC },   { "EAGAIN", EX_DEVD_ERROR_AGAIN },
		{ "ENOTSUP", EX_DEVD_ERROR_NOTSUP }, { "EOPNOTSUPP", EX_DEVD_ERROR_NOTSUP },
		{ "ENOSYS", EX_DEVD_ERROR_NOTSUP },  { "EROFS", EX_DEVD_ERROR_ROFS },
		{ "EUCLEAN", EX_DEVD_ERROR_UNCLEAN },
	};

	for (gsize i = 0; errname && i < G_N_ELEMENTS(map); i++)
		if (strcmp(errname, map[i].name) == 0)
			return map[i].code;
	return EX_DEVD_ERROR_FAILED;
}

const char *ex_devd_error_explain(ExDevdErrorCode code)
{
	switch (code) {
	case EX_DEVD_ERROR_PERM:
		return "You are not allowed to do this with this drive.";
	case EX_DEVD_ERROR_BUSY:
		return "The drive is in use. Close the files and folders that are "
		       "open on it, then try again.";
	case EX_DEVD_ERROR_NOENT:
	case EX_DEVD_ERROR_NODEV:
		return "The drive is no longer connected.";
	case EX_DEVD_ERROR_INVAL:
		return "One of the settings was not accepted.";
	case EX_DEVD_ERROR_IO:
		return "The drive could not be read or written.";
	case EX_DEVD_ERROR_NOSPC:
		return "There is not enough space.";
	case EX_DEVD_ERROR_AGAIN:
		return "Too many programs are talking to the device service. Try "
		       "again in a moment.";
	case EX_DEVD_ERROR_NOTSUP:
		return "This is not possible with this drive.";
	case EX_DEVD_ERROR_ROFS:
		return "The drive is write-protected.";
	case EX_DEVD_ERROR_NOT_RUNNING:
		return "The device service is not running.";
	case EX_DEVD_ERROR_PROTOCOL:
		return "The device service gave an answer that could not be read.";
	case EX_DEVD_ERROR_UNCLEAN:
		return "The drive's file system was not closed properly; its "
		       "journal must be replayed before it can be opened.";
	case EX_DEVD_ERROR_FAILED:
	default:
		return "Something went wrong.";
	}
}

/* ---- values ------------------------------------------------------------- */

char *ex_devd_encode(const char *value)
{
	static const char hex[] = "0123456789ABCDEF";
	GString *s = g_string_sized_new(value ? strlen(value) : 0);

	for (const guchar *p = (const guchar *)(value ? value : ""); *p; p++) {
		if (*p < 0x21 || *p > 0x7e || *p == '%' || *p == '=') {
			g_string_append_c(s, '%');
			g_string_append_c(s, hex[*p >> 4]);
			g_string_append_c(s, hex[*p & 15]);
		} else {
			g_string_append_c(s, (char)*p);
		}
	}
	return g_string_free(s, FALSE);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

char *ex_devd_decode(const char *text)
{
	gsize n = strlen(text);
	char *out = g_malloc(n + 1);
	gsize o = 0;

	for (gsize i = 0; i < n; i++) {
		guchar c = (guchar)text[i];

		if (c < 0x21 || c > 0x7e)
			goto bad;
		if (c == '%') {
			int hi, lo;

			if (i + 2 >= n)
				goto bad;
			hi = hexval(text[i + 1]);
			lo = hi < 0 ? -1 : hexval(text[i + 2]);
			if (hi < 0 || lo < 0 || (hi | lo) == 0)
				goto bad;
			out[o++] = (char)((hi << 4) | lo);
			i += 2;
		} else {
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
	return out;
bad:
	g_free(out);
	return NULL;
}

/* ---- lines -------------------------------------------------------------- */

void ex_devd_line_free(ExDevdLine *l)
{
	if (!l)
		return;
	g_ptr_array_unref(l->words);
	g_ptr_array_unref(l->keys);
	g_ptr_array_unref(l->vals);
	g_free(l);
}

static gboolean key_valid(const char *k, gsize len)
{
	if (len == 0 || len > KEY_MAX)
		return FALSE;
	for (gsize i = 0; i < len; i++)
		if (!g_ascii_islower(k[i]) && !g_ascii_isdigit(k[i]) && k[i] != '_')
			return FALSE;
	return TRUE;
}

ExDevdLine *ex_devd_line_parse(const char *line, gsize len)
{
	ExDevdLine *l;
	gsize start = 0;
	guint tokens = 0;

	if (!line || len == 0 || len > EX_DEVD_LINE_MAX)
		return NULL;
	for (gsize i = 0; i < len; i++)
		if ((guchar)line[i] < 0x20 || (guchar)line[i] > 0x7e)
			return NULL;
	l = g_new0(ExDevdLine, 1);
	l->words = g_ptr_array_new_with_free_func(g_free);
	l->keys = g_ptr_array_new_with_free_func(g_free);
	l->vals = g_ptr_array_new_with_free_func(g_free);
	while (start <= len) {
		const char *tok = line + start;
		const char *sp = memchr(tok, ' ', len - start);
		gsize tlen = sp ? (gsize)(sp - tok) : len - start;
		const char *eq = memchr(tok, '=', tlen);

		if (tlen == 0 || ++tokens > EX_DEVD_TOKENS_MAX)
			goto bad;
		if (eq) {
			gsize klen = (gsize)(eq - tok);
			char *raw, *val;

			if (!key_valid(tok, klen))
				goto bad;
			raw = g_strndup(eq + 1, tlen - klen - 1);
			/* A value holds no '=' of its own: that is encoded. */
			val = strchr(raw, '=') ? NULL : ex_devd_decode(raw);
			g_free(raw);
			if (!val)
				goto bad;
			g_ptr_array_add(l->keys, g_strndup(tok, klen));
			g_ptr_array_add(l->vals, val);
		} else {
			/* Plain words come before every pair. */
			if (l->keys->len)
				goto bad;
			g_ptr_array_add(l->words, g_strndup(tok, tlen));
		}
		if (!sp)
			break;
		start += tlen + 1;
		if (start == len) /* a space at the end: an empty token */
			goto bad;
	}
	if (l->words->len == 0)
		goto bad;
	return l;
bad:
	ex_devd_line_free(l);
	return NULL;
}

const char *ex_devd_line_word(const ExDevdLine *l, guint i)
{
	return i < l->words->len ? g_ptr_array_index(l->words, i) : NULL;
}

const char *ex_devd_line_get(const ExDevdLine *l, const char *key)
{
	for (guint i = 0; i < l->keys->len; i++)
		if (strcmp(g_ptr_array_index(l->keys, i), key) == 0)
			return g_ptr_array_index(l->vals, i);
	return NULL;
}

gboolean ex_devd_name_valid(const char *name)
{
	gsize n = name ? strlen(name) : 0;

	if (n == 0 || n > 31)
		return FALSE;
	for (gsize i = 0; i < n; i++)
		if (!g_ascii_isalnum(name[i]) && name[i] != '_' && name[i] != '-')
			return FALSE;
	return TRUE;
}

/* ---- fields -------------------------------------------------------------- */

static gboolean flag(const ExDevdLine *l, const char *key)
{
	const char *v = ex_devd_line_get(l, key);

	return v && strcmp(v, "1") == 0;
}

static guint64 number(const ExDevdLine *l, const char *key, guint64 max)
{
	const char *v = ex_devd_line_get(l, key);
	guint64 n = 0;

	if (!v || !g_ascii_string_to_unsigned(v, 10, 0, max, &n, NULL))
		return 0;
	return n;
}

/* Text to show: at most `max' bytes, valid UTF-8, no control
 * characters. */
static char *text(const ExDevdLine *l, const char *key, gsize max)
{
	const char *v = ex_devd_line_get(l, key);
	char *s, *o;

	if (!v || strlen(v) > max)
		return g_strdup("");
	s = g_utf8_make_valid(v, -1);
	for (o = s; *o; o++)
		if ((guchar)*o < 0x20 || *o == 0x7f)
			*o = ' ';
	return s;
}

/* A word of the given alphabet: lower-case letters, digits and '_'. */
static char *ident(const ExDevdLine *l, const char *key, gsize max,
		   const char *fallback)
{
	const char *v = ex_devd_line_get(l, key);
	gsize n = v ? strlen(v) : 0;

	if (n == 0 || n > max)
		return g_strdup(fallback);
	for (gsize i = 0; i < n; i++)
		if (!g_ascii_islower(v[i]) && !g_ascii_isdigit(v[i]) && v[i] != '_')
			return g_strdup(fallback);
	return g_strdup(v);
}

ExDevdDevice *ex_devd_device_from_line(const ExDevdLine *l)
{
	const char *name = ex_devd_line_get(l, "name");
	const char *disk = ex_devd_line_get(l, "disk");
	const char *kind = ex_devd_line_get(l, "kind");
	const char *m = ex_devd_line_get(l, "mounted");
	const char *uid = ex_devd_line_get(l, "mountuid");
	const char *uuid = ex_devd_line_get(l, "uuid");
	ExDevdDevice *d;
	gint64 u = -1;

	if (!ex_devd_name_valid(name) || !ex_devd_name_valid(disk) || !kind ||
	    (strcmp(kind, "disk") && strcmp(kind, "partition")))
		return NULL;
	d = g_new0(ExDevdDevice, 1);
	d->name = g_strdup(name);
	d->disk = g_strdup(disk);
	d->is_disk = strcmp(kind, "disk") == 0;
	d->size = number(l, "size", G_MAXINT64);
	d->removable = flag(l, "removable");
	d->usb = flag(l, "usb");
	d->system = flag(l, "system");
	d->fstype = ident(l, "fstype", FSTYPE_MAX, "unknown");
	d->supported = flag(l, "supported");
	d->label = text(l, "label", LABEL_MAX);
	d->vendor = text(l, "vendor", TEXT_MAX);
	d->model = text(l, "model", TEXT_MAX);
	/* A UUID is hex digits and dashes. */
	d->uuid = g_strdup("");
	if (uuid && strlen(uuid) <= UUID_MAX) {
		gboolean ok = TRUE;

		for (const char *c = uuid; *c && ok; c++)
			ok = g_ascii_isxdigit(*c) || *c == '-';
		if (ok) {
			g_free(d->uuid);
			d->uuid = g_strdup(uuid);
		}
	}
	/* A mount path is absolute and in bounds; anything else counts as not
	 * mounted. */
	d->mounted = m && m[0] == '/' && strlen(m) <= PATH_MAX_BYTES ?
			     g_strdup(m) :
			     g_strdup("");
	if (uid && *uid && !g_ascii_string_to_signed(uid, 10, -1, G_MAXUINT32,
						      &u, NULL))
		u = -1;
	d->mountuid = u;
	d->ejected = flag(l, "ejected");
	d->state = ident(l, "state", 16, "busy");
	d->readonly = flag(l, "readonly");
	d->optical = flag(l, "optical");
	/* An optical drive is never a built-in disk, whatever else the line
	 * says. */
	d->internal = flag(l, "internal") && !d->optical;
	return d;
}

ExDevdDevice *ex_devd_device_copy(const ExDevdDevice *s)
{
	ExDevdDevice *d = g_memdup2(s, sizeof(*s));

	d->name = g_strdup(s->name);
	d->disk = g_strdup(s->disk);
	d->fstype = g_strdup(s->fstype);
	d->label = g_strdup(s->label);
	d->uuid = g_strdup(s->uuid);
	d->vendor = g_strdup(s->vendor);
	d->model = g_strdup(s->model);
	d->mounted = g_strdup(s->mounted);
	d->state = g_strdup(s->state);
	return d;
}

void ex_devd_device_free(ExDevdDevice *d)
{
	if (!d)
		return;
	g_free(d->name);
	g_free(d->disk);
	g_free(d->fstype);
	g_free(d->label);
	g_free(d->uuid);
	g_free(d->vendor);
	g_free(d->model);
	g_free(d->mounted);
	g_free(d->state);
	g_free(d);
}

ExDevdFs *ex_devd_fs_from_line(const ExDevdLine *l)
{
	const char *id = ex_devd_line_get(l, "id");
	const char *cl = ex_devd_line_get(l, "clusters");
	ExDevdFs *f;

	if (!id)
		return NULL;
	f = g_new0(ExDevdFs, 1);
	f->id = ident(l, "id", FSTYPE_MAX, "");
	if (!*f->id) {
		ex_devd_fs_free(f);
		return NULL;
	}
	f->name = text(l, "name", FSNAME_MAX);
	if (!*f->name) {
		g_free(f->name);
		f->name = g_strdup(f->id);
	}
	f->is_default = flag(l, "default");
	f->labelmax = (guint)number(l, "labelmax", LABEL_MAX);
	f->defcluster = number(l, "defcluster", CLUSTER_LARGEST);
	f->can_mount = flag(l, "mount");
	f->can_format = flag(l, "format");
	f->clusters = g_array_new(FALSE, FALSE, sizeof(guint64));
	if (cl && *cl) {
		char **parts = g_strsplit(cl, ",", CLUSTERS_MAX + 1);

		for (int i = 0; parts[i] && i < CLUSTERS_MAX; i++) {
			guint64 n;

			if (g_ascii_string_to_unsigned(parts[i], 10, 1,
						       CLUSTER_LARGEST, &n, NULL))
				g_array_append_val(f->clusters, n);
		}
		g_strfreev(parts);
	}
	return f;
}

ExDevdFs *ex_devd_fs_copy(const ExDevdFs *s)
{
	ExDevdFs *f = g_memdup2(s, sizeof(*s));

	f->id = g_strdup(s->id);
	f->name = g_strdup(s->name);
	f->clusters = g_array_copy(s->clusters);
	return f;
}

void ex_devd_fs_free(ExDevdFs *f)
{
	if (!f)
		return;
	g_free(f->id);
	g_free(f->name);
	if (f->clusters)
		g_array_unref(f->clusters);
	g_free(f);
}

/* "EBUSY": 'E' and then upper-case letters and digits. */
static gboolean is_errname(const char *w)
{
	gsize n = w ? strlen(w) : 0;

	if (n < 2 || n > 16 || w[0] != 'E')
		return FALSE;
	for (gsize i = 1; i < n; i++)
		if (!g_ascii_isupper(w[i]) && !g_ascii_isdigit(w[i]))
			return FALSE;
	return TRUE;
}

gboolean ex_devd_parse_err(const ExDevdLine *l, char **verb, char **name,
			   char **errname, char **message)
{
	const char *w0 = ex_devd_line_word(l, 0);
	const char *v = ex_devd_line_word(l, 1);
	guint at = 2;
	const char *n = NULL, *e, *m;
	char *msg;

	if (!w0 || strcmp(w0, "ERR") || !v)
		return FALSE;
	/* "ERR FORMAT sdb1 EBUSY ...": a device name before the error
	 * name. */
	if (!is_errname(ex_devd_line_word(l, at)) &&
	    is_errname(ex_devd_line_word(l, at + 1))) {
		n = ex_devd_line_word(l, at);
		if (!ex_devd_name_valid(n))
			return FALSE;
		at++;
	}
	e = ex_devd_line_word(l, at);
	if (!is_errname(e))
		return FALSE;
	m = ex_devd_line_word(l, at + 1);
	msg = m ? ex_devd_decode(m) : g_strdup("");
	if (!msg)
		return FALSE;
	if (verb)
		*verb = g_strdup(v);
	if (name)
		*name = g_strdup(n);
	if (errname)
		*errname = g_strdup(e);
	if (message) {
		/* Shown in a dialog: valid UTF-8, one line of sensible
		 * length. */
		char *s = g_utf8_make_valid(msg, MIN(strlen(msg), 1024));

		for (char *o = s; *o; o++)
			if ((guchar)*o < 0x20 || *o == 0x7f)
				*o = ' ';
		*message = s;
	}
	g_free(msg);
	return TRUE;
}
