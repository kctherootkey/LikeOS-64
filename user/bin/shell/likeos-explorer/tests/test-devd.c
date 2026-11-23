// LikeOS Explorer: test program for the device service's client.
//
// The line protocol on its own -- encoding, decoding, lines, devices,
// filesystems, errors, and lines that break the rules -- and then the
// client against a stand-in service on a socket of its own (in a fresh
// directory under $TMPDIR): the first requests, the table of devices and
// filesystems, events, a mount, a mount refused until its journal is
// replayed and asked for again with recovery, a refused eject, lines to
// drop, the connection lost and found again.  Needs neither the real
// service nor a display.
//
//     explorer-devd-test
//
// Exit status 0 when every check passed.
//
// Copyright (C) 2026 The LikeOS Project

#include "backend/ex-devd.h"
#include "backend/ex-devd-proto.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int g_pass, g_fail;

#define CHECK(name, cond)                                                   \
	do {                                                                \
		if (cond) {                                                 \
			g_pass++;                                           \
			printf("  [PASS] %s\n", name);                      \
		} else {                                                    \
			g_fail++;                                           \
			printf("  [FAIL] %s  (%s:%d)\n", name, __FILE__,    \
			       __LINE__);                                   \
		}                                                           \
	} while (0)

static ExDevdLine *parse(const char *s)
{
	return ex_devd_line_parse(s, strlen(s));
}

/* ---- the protocol --------------------------------------------------------- */

static void test_values(void)
{
	char *e, *d;

	printf("values\n");
	e = ex_devd_encode("MY STICK");
	CHECK("space encoded", strcmp(e, "MY%20STICK") == 0);
	g_free(e);
	e = ex_devd_encode("a=b%c\n\xc3\xa4");
	CHECK("= % newline and UTF-8 encoded",
	      strcmp(e, "a%3Db%25c%0A%C3%A4") == 0);
	d = ex_devd_decode(e);
	CHECK("round trip", d && strcmp(d, "a=b%c\n\xc3\xa4") == 0);
	g_free(d);
	g_free(e);
	e = ex_devd_encode("");
	CHECK("empty value", strcmp(e, "") == 0);
	g_free(e);
	d = ex_devd_decode("%7e%7E");
	CHECK("hex in either case", d && strcmp(d, "~~") == 0);
	g_free(d);
	CHECK("lone % refused", ex_devd_decode("abc%") == NULL);
	CHECK("% with one digit refused", ex_devd_decode("abc%4") == NULL);
	CHECK("% with no hex refused", ex_devd_decode("%G0") == NULL);
	CHECK("encoded NUL refused", ex_devd_decode("a%00b") == NULL);
	CHECK("raw space refused", ex_devd_decode("a b") == NULL);
	CHECK("raw control byte refused", ex_devd_decode("a\tb") == NULL);
	CHECK("raw high byte refused", ex_devd_decode("a\xc3\xa4") == NULL);
}

static void test_lines(void)
{
	ExDevdLine *l;
	char *big;

	printf("lines\n");
	l = parse("OK MOUNT sdb1 path=/media/niko/MY%20STICK");
	CHECK("words and a pair", l && l->words->len == 3 && l->keys->len == 1);
	CHECK("word read", l && strcmp(ex_devd_line_word(l, 1), "MOUNT") == 0);
	CHECK("value decoded",
	      l && strcmp(ex_devd_line_get(l, "path"), "/media/niko/MY STICK") == 0);
	CHECK("missing key", l && ex_devd_line_get(l, "nope") == NULL);
	ex_devd_line_free(l);
	l = parse("DEV name=sdb label= x=1");
	CHECK("empty value", l && strcmp(ex_devd_line_get(l, "label"), "") == 0);
	ex_devd_line_free(l);
	l = parse("DEV name=a name=b");
	CHECK("first of a repeated key", l && !strcmp(ex_devd_line_get(l, "name"), "a"));
	ex_devd_line_free(l);
	CHECK("empty line refused", parse("") == NULL);
	CHECK("two spaces refused", parse("OK  HELLO") == NULL);
	CHECK("leading space refused", parse(" OK") == NULL);
	CHECK("trailing space refused", parse("OK HELLO ") == NULL);
	CHECK("word after a pair refused", parse("DEV a=1 b") == NULL);
	CHECK("raw = in a value refused", parse("DEV a=1=2") == NULL);
	CHECK("bad key refused", parse("DEV Name=sdb") == NULL);
	CHECK("empty key refused", parse("DEV =sdb") == NULL);
	CHECK("bad escape refused", parse("DEV label=%ZZ") == NULL);
	CHECK("control byte refused", parse("DEV label=a\x01") == NULL);
	CHECK("carriage return refused", parse("OK HELLO 1\r") == NULL);
	CHECK("only pairs refused", parse("a=1") == NULL);
	big = g_strnfill(EX_DEVD_LINE_MAX + 1, 'A');
	CHECK("too long refused", ex_devd_line_parse(big, strlen(big)) == NULL);
	big[EX_DEVD_LINE_MAX] = '\0';
	l = ex_devd_line_parse(big, strlen(big));
	CHECK("longest line taken", l != NULL);
	ex_devd_line_free(l);
	g_free(big);
	{
		GString *s = g_string_new("DEV");

		for (int i = 0; i < EX_DEVD_TOKENS_MAX; i++)
			g_string_append_printf(s, " k%d=1", i);
		CHECK("too many tokens refused", parse(s->str) == NULL);
		g_string_free(s, TRUE);
	}
}

static const char *DEV_SDB1 =
	"DEV name=sdb1 disk=sdb kind=partition size=15931539456 removable=1 "
	"usb=1 system=0 fstype=vfat supported=1 label=MY%20STICK "
	"uuid=1A2B-3C4D vendor=Sand%20Co model=Cruzer mounted= mountuid=-1 "
	"ejected=0 state=ready";

static void test_devices(void)
{
	ExDevdLine *l = parse(DEV_SDB1);
	ExDevdDevice *d = l ? ex_devd_device_from_line(l) : NULL, *c;

	printf("devices\n");
	CHECK("device parsed", d != NULL);
	if (d) {
		CHECK("name and disk", !strcmp(d->name, "sdb1") && !strcmp(d->disk, "sdb"));
		CHECK("partition", !d->is_disk);
		CHECK("size", d->size == G_GUINT64_CONSTANT(15931539456));
		CHECK("flags", d->removable && d->usb && !d->system && d->supported &&
				       !d->ejected);
		CHECK("label decoded", !strcmp(d->label, "MY STICK"));
		CHECK("uuid", !strcmp(d->uuid, "1A2B-3C4D"));
		CHECK("not mounted", !strcmp(d->mounted, "") && d->mountuid == -1);
		CHECK("state", !strcmp(d->state, "ready"));
		c = ex_devd_device_copy(d);
		CHECK("copy", !strcmp(c->label, d->label) && c->size == d->size);
		ex_devd_device_free(c);
	}
	ex_devd_device_free(d);
	ex_devd_line_free(l);

	l = parse("DEV name=..%2Fetc disk=sdb kind=disk");
	CHECK("path in a name refused", l && !ex_devd_device_from_line(l));
	ex_devd_line_free(l);
	l = parse("DEV name=sdb kind=disk");
	CHECK("no disk refused", l && !ex_devd_device_from_line(l));
	ex_devd_line_free(l);
	l = parse("DEV name=sdb disk=sdb kind=tape");
	CHECK("bad kind refused", l && !ex_devd_device_from_line(l));
	ex_devd_line_free(l);
	CHECK("long name refused",
	      !ex_devd_name_valid("abcdefghijklmnopqrstuvwxyz0123456"));
	CHECK("name with a space refused", !ex_devd_name_valid("sd b"));

	l = parse("DEV name=sdc disk=sdc kind=disk size=-5 removable=yes usb=1 "
		  "fstype=NT%2FFS label=%01%02bad mounted=relative/path "
		  "mountuid=99999999999 uuid=..%2F.. state=WEIRD");
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("hostile fields: device kept", d != NULL);
	if (d) {
		CHECK("bad size is 0", d->size == 0);
		CHECK("bad flag is 0", !d->removable);
		CHECK("bad fstype is unknown", !strcmp(d->fstype, "unknown"));
		CHECK("control characters blanked", !strcmp(d->label, "  bad"));
		CHECK("relative mount path ignored", !strcmp(d->mounted, ""));
		CHECK("bad uid is -1", d->mountuid == -1);
		CHECK("bad uuid dropped", !strcmp(d->uuid, ""));
		CHECK("bad state is busy", !strcmp(d->state, "busy"));
	}
	ex_devd_device_free(d);
	ex_devd_line_free(l);

	/* the keys added later: absent (an older service) means 0 */
	l = parse(DEV_SDB1);
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("older line: not read-only, optical or internal",
	      d && !d->readonly && !d->optical && !d->internal);
	ex_devd_device_free(d);
	ex_devd_line_free(l);
	l = parse("DEV name=sr0 disk=sr0 kind=disk size=734003200 removable=1 "
		  "usb=0 system=0 fstype=iso9660 supported=1 label=MY%20CD "
		  "uuid=2026-11-20-12-34-56-00 mounted= mountuid=-1 ejected=0 "
		  "state=ready readonly=1 optical=1 internal=0");
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("CD/DVD drive parsed", d != NULL);
	if (d) {
		CHECK("optical, read-only, not internal",
		      d->optical && d->readonly && !d->internal && !d->usb);
		CHECK("ISO 9660 uuid kept",
		      !strcmp(d->uuid, "2026-11-20-12-34-56-00"));
		c = ex_devd_device_copy(d);
		CHECK("copy keeps the new flags",
		      c->optical && c->readonly && !c->internal);
		ex_devd_device_free(c);
	}
	ex_devd_device_free(d);
	ex_devd_line_free(l);
	l = parse("DEV name=nvme0n1p3 disk=nvme0n1 kind=partition "
		  "size=500000000000 removable=0 usb=0 system=0 fstype=ntfs "
		  "supported=1 label= uuid=1122334455667788 mounted= "
		  "mountuid=-1 ejected=0 state=ready readonly=0 optical=0 "
		  "internal=1 future=whatever");
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("built-in disk parsed, unknown key ignored", d != NULL);
	if (d)
		CHECK("internal, not optical, writable",
		      d->internal && !d->optical && !d->readonly);
	ex_devd_device_free(d);
	ex_devd_line_free(l);
	l = parse("DEV name=sr1 disk=sr1 kind=disk optical=1 internal=1 "
		  "readonly=yes");
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("an optical drive is never internal; a bad flag is 0",
	      d && d->optical && !d->internal && !d->readonly);
	ex_devd_device_free(d);
	ex_devd_line_free(l);

	l = parse("DEV name=sdd disk=sdd kind=disk label=%FF%FE");
	d = l ? ex_devd_device_from_line(l) : NULL;
	CHECK("invalid UTF-8 label made valid",
	      d && g_utf8_validate(d->label, -1, NULL));
	ex_devd_device_free(d);
	ex_devd_line_free(l);
}

static void test_fs(void)
{
	ExDevdLine *l = parse("FS id=vfat name=FAT32 default=1 labelmax=11 "
			      "clusters=512,1024,x,4096,0 defcluster=4096 "
			      "mount=1 format=1");
	ExDevdFs *f = l ? ex_devd_fs_from_line(l) : NULL, *c;

	printf("filesystems\n");
	CHECK("fs parsed", f != NULL);
	if (f) {
		CHECK("id and name", !strcmp(f->id, "vfat") && !strcmp(f->name, "FAT32"));
		CHECK("default", f->is_default);
		CHECK("labelmax", f->labelmax == 11);
		CHECK("clusters: bad ones dropped", f->clusters->len == 3);
		CHECK("cluster values",
		      f->clusters->len == 3 &&
			      g_array_index(f->clusters, guint64, 2) == 4096);
		CHECK("defcluster", f->defcluster == 4096);
		CHECK("mount and format", f->can_mount && f->can_format);
		c = ex_devd_fs_copy(f);
		CHECK("copy", c->clusters->len == 3 && !strcmp(c->id, "vfat"));
		ex_devd_fs_free(c);
	}
	ex_devd_fs_free(f);
	ex_devd_line_free(l);
	l = parse("FS id=ext4");
	f = l ? ex_devd_fs_from_line(l) : NULL;
	CHECK("minimal fs: name falls back to id", f && !strcmp(f->name, "ext4") &&
							   f->clusters->len == 0);
	ex_devd_fs_free(f);
	ex_devd_line_free(l);
	l = parse("FS id=EXT4");
	CHECK("bad id refused", l && !ex_devd_fs_from_line(l));
	ex_devd_line_free(l);
	l = parse("FS name=x");
	CHECK("no id refused", l && !ex_devd_fs_from_line(l));
	ex_devd_line_free(l);
}

static void test_errors(void)
{
	ExDevdLine *l;
	char *verb, *name, *errname, *msg;

	printf("errors\n");
	l = parse("ERR EJECT EBUSY device%20is%20busy");
	CHECK("ERR parsed", l && ex_devd_parse_err(l, &verb, &name, &errname, &msg));
	if (l) {
		CHECK("ERR fields", !strcmp(verb, "EJECT") && name == NULL &&
					    !strcmp(errname, "EBUSY") &&
					    !strcmp(msg, "device is busy"));
		g_free(verb);
		g_free(name);
		g_free(errname);
		g_free(msg);
	}
	ex_devd_line_free(l);
	l = parse("ERR FORMAT sdb1 EINVAL label%20too%20long");
	CHECK("ERR with a name parsed",
	      l && ex_devd_parse_err(l, &verb, &name, &errname, &msg));
	if (l) {
		CHECK("ERR with a name: fields",
		      !strcmp(verb, "FORMAT") && name && !strcmp(name, "sdb1") &&
			      !strcmp(errname, "EINVAL") &&
			      !strcmp(msg, "label too long"));
		g_free(verb);
		g_free(name);
		g_free(errname);
		g_free(msg);
	}
	ex_devd_line_free(l);
	l = parse("ERR MOUNT EPERM");
	CHECK("ERR without a message",
	      l && ex_devd_parse_err(l, NULL, NULL, NULL, &msg) && !strcmp(msg, ""));
	if (l)
		g_free(msg);
	ex_devd_line_free(l);
	l = parse("ERR MOUNT notanerror text");
	CHECK("ERR without an error name refused",
	      l && !ex_devd_parse_err(l, NULL, NULL, NULL, NULL));
	ex_devd_line_free(l);
	l = parse("ERR MOUNT EIO bad%ZZ");
	CHECK("ERR with a bad escape in its message refused",
	      l && !ex_devd_parse_err(l, NULL, NULL, NULL, NULL));
	ex_devd_line_free(l);
	CHECK("error codes", ex_devd_error_code("EBUSY") == EX_DEVD_ERROR_BUSY &&
				     ex_devd_error_code("EACCES") == EX_DEVD_ERROR_PERM &&
				     ex_devd_error_code("EWHAT") == EX_DEVD_ERROR_FAILED);
	CHECK("journal error code",
	      ex_devd_error_code("EUCLEAN") == EX_DEVD_ERROR_UNCLEAN &&
		      *ex_devd_error_explain(EX_DEVD_ERROR_UNCLEAN));
}

/* ---- the client against a stand-in service ----------------------------------- */

typedef struct {
	int listen_fd;
	char *path;
	gint handshakes; /* first requests answered, in order */
	gint script; /* the first connection's script ran through */
	char *failed; /* the request that did not come as expected */
} Server;

static gboolean expect(Server *s, FILE *in, const char *want)
{
	char buf[EX_DEVD_LINE_MAX + 2];
	gsize n;

	if (!fgets(buf, sizeof(buf), in)) {
		s->failed = g_strdup_printf("no request, wanted \"%s\"", want);
		return FALSE;
	}
	n = strlen(buf);
	if (n && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	if (strcmp(buf, want) != 0) {
		s->failed = g_strdup_printf("got \"%s\", wanted \"%s\"", buf, want);
		return FALSE;
	}
	return TRUE;
}

static void say(FILE *out, const char *line)
{
	fputs(line, out);
	fputc('\n', out);
	fflush(out);
}

static gboolean handshake(FILE *in, FILE *out, Server *s,
			  const char *const *devs)
{
	/* The client sends its first four requests at once. */
	if (!expect(s, in, "HELLO 1") || !expect(s, in, "SUBSCRIBE") ||
	    !expect(s, in, "LIST") || !expect(s, in, "FSTYPES"))
		return FALSE;
	g_atomic_int_inc(&s->handshakes);
	say(out, "OK HELLO 1");
	say(out, "OK SUBSCRIBE");
	for (int i = 0; devs && devs[i]; i++)
		say(out, devs[i]);
	say(out, "OK LIST");
	say(out, "FS id=ext4 name=ext4 default=0 labelmax=16 clusters=1024,2048,4096 "
		 "defcluster=4096 mount=1 format=1");
	say(out, "FS id=vfat name=FAT32 default=1 labelmax=11 "
		 "clusters=512,1024,2048,4096,8192,16384,32768 defcluster=0 "
		 "mount=1 format=1");
	say(out, "OK FSTYPES");
	return TRUE;
}

static gpointer server_main(gpointer data)
{
	Server *s = data;
	static const char *const first[] = {
		"DEV name=sdb disk=sdb kind=disk size=15931539456 removable=1 usb=1 "
		"system=0 fstype=none supported=0 label= uuid= vendor= model= "
		"mounted= mountuid=-1 ejected=0 state=ready",
		NULL, /* DEV_SDB1 */
		"DEV name=sdc disk=sdc kind=disk size=1000000 removable=1 usb=1 "
		"system=0 fstype=ntfs supported=0 label=WIN uuid= vendor= model= "
		"mounted= mountuid=-1 ejected=0 state=ready",
		"DEV name=..%2Fx disk=sdx kind=disk",
		NULL,
	};
	const char *devs[G_N_ELEMENTS(first)];
	int fd;
	FILE *in, *out;

	memcpy(devs, first, sizeof(first));
	devs[1] = DEV_SDB1;

	/* The first connection. */
	fd = accept(s->listen_fd, NULL, NULL);
	if (fd < 0)
		return NULL;
	in = fdopen(fd, "r");
	out = fdopen(dup(fd), "w");
	if (!handshake(in, out, s, devs))
		goto done;
	say(out, "EVENT CHANGED name=sdb1 disk=sdb kind=partition "
		 "size=15931539456 removable=1 usb=1 system=0 fstype=vfat "
		 "supported=1 label=MY%20STICK uuid=1A2B-3C4D vendor= model= "
		 "mounted=/media/u/MY%20STICK mountuid=1000 ejected=0 state=ready");
	if (!expect(s, in, "MOUNT sdb1"))
		goto done;
	say(out, "PROGRESS sdb1 50");
	say(out, "OK MOUNT sdb1 path=/media/u/MY%20STICK");
	/* A journal to replay: refused, then asked for with recovery. */
	if (!expect(s, in, "MOUNT sdb1"))
		goto done;
	say(out, "ERR MOUNT EUCLEAN the%20file%20system%20journal%20must%20be"
		 "%20replayed%20first");
	if (!expect(s, in, "MOUNT sdb1 recover"))
		goto done;
	say(out, "OK MOUNT sdb1 path=/media/u/MY%20STICK");
	if (!expect(s, in, "EJECT sdb"))
		goto done;
	say(out, "ERR EJECT EBUSY The%20drive%20is%20busy");
	/* Lines to drop, the connection kept. */
	say(out, "OK");
	say(out, "OK UNMOUNT sdz");
	say(out, "DEV name=zz disk=zz kind=disk");
	say(out, "EVENT ADDED name=a b");
	say(out, "EVENT ADDED name=sdq disk=sdq kind=disk mounted=%ZZ");
	say(out, "WHAT is this");
	say(out, "  ");
	fputs("EVENT ADDED name=sdr\x01 disk=sdr kind=disk\n", out);
	fflush(out);
	g_atomic_int_set(&s->script, 1);
	say(out, "EVENT REMOVED name=sdc disk=sdc kind=disk size=1000000 "
		 "removable=1 usb=1 system=0 fstype=ntfs supported=0 label=WIN "
		 "uuid= vendor= model= mounted= mountuid=-1 ejected=0 state=ready");
	/* The client says when it has seen all that. */
	if (expect(s, in, "UNMOUNT sdb1"))
		say(out, "OK UNMOUNT sdb1");
done:
	fclose(in);
	fclose(out);
	if (!g_atomic_int_get(&s->script))
		return NULL;

	/* The second: the client comes back by itself. */
	fd = accept(s->listen_fd, NULL, NULL);
	if (fd < 0)
		return NULL;
	in = fdopen(fd, "r");
	out = fdopen(dup(fd), "w");
	if (handshake(in, out, s, NULL)) {
		/* A line longer than the protocol allows: the client hangs
		 * up. */
		char *big = g_strnfill(EX_DEVD_LINE_MAX + 10, 'A');

		say(out, big);
		g_free(big);
		/* Wait for the client to close. */
		while (fgetc(in) != EOF)
			;
	}
	fclose(in);
	fclose(out);
	return NULL;
}

typedef struct {
	gboolean done;
	char *path;
	GError *error;
	gboolean ok;
} Result;

static gboolean timed_out(gpointer data)
{
	*(gboolean *)data = TRUE;
	return G_SOURCE_REMOVE;
}

/* Run the main loop until `cond' holds, or `ms' passed. */
#define RUN_UNTIL(cond, ms)                                                   \
	({                                                                    \
		gboolean _late = FALSE;                                       \
		guint _id = g_timeout_add((ms), timed_out, &_late);           \
		while (!(cond) && !_late)                                     \
			g_main_context_iteration(NULL, TRUE);                 \
		if (!_late)                                                   \
			g_source_remove(_id);                                 \
		(cond);                                                       \
	})

static void mounted(GObject *src, GAsyncResult *res, gpointer data)
{
	Result *r = data;

	r->path = ex_devd_mount_finish(EX_DEVD(src), res, &r->error);
	r->done = TRUE;
}

static void ejected(GObject *src, GAsyncResult *res, gpointer data)
{
	Result *r = data;

	r->ok = ex_devd_eject_finish(EX_DEVD(src), res, &r->error);
	r->done = TRUE;
}

static void unmounted(GObject *src, GAsyncResult *res, gpointer data)
{
	Result *r = data;

	r->ok = ex_devd_unmount_finish(EX_DEVD(src), res, &r->error);
	r->done = TRUE;
}

static guint g_changes;

static void changed(ExDevd *d, gpointer data)
{
	g_changes++;
}

static void test_client(void)
{
	char *dir = g_dir_make_tmp("explorer-devd-XXXXXX", NULL);
	Server s = { -1 };
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	GThread *th;
	ExDevd *d;
	Result r = { 0 };
	const ExDevdDevice *dev;
	GPtrArray *fs;

	printf("client\n");
	if (!dir) {
		CHECK("temporary directory", FALSE);
		return;
	}
	s.path = g_build_filename(dir, "socket", NULL);
	/* A socket's path is short: a long one is reached from inside its
	 * directory. */
	if (strlen(s.path) >= sizeof(sa.sun_path)) {
		if (chdir(dir) < 0) {
			CHECK("temporary directory", FALSE);
			return;
		}
		g_free(s.path);
		s.path = g_strdup("socket");
	}
	g_strlcpy(sa.sun_path, s.path, sizeof(sa.sun_path));
	s.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s.listen_fd < 0 ||
	    bind(s.listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    listen(s.listen_fd, 4) < 0) {
		CHECK("stand-in service listens", FALSE);
		return;
	}
	th = g_thread_new("devd", server_main, &s);

	d = ex_devd_new_for_path(s.path);
	g_signal_connect(d, "changed", G_CALLBACK(changed), NULL);
	CHECK("connected, table filled",
	      RUN_UNTIL(ex_devd_is_connected(d) &&
				ex_devd_fstypes(d)->len == 2,
			5000));
	CHECK("handshake in order", g_atomic_int_get(&s.handshakes) == 1);
	CHECK("three devices, the bad one dropped", ex_devd_devices(d)->len == 3);
	dev = ex_devd_find(d, "sdb1");
	CHECK("sdb1 known", dev && !strcmp(dev->label, "MY STICK"));
	fs = ex_devd_fstypes(d);
	CHECK("default filesystem first",
	      fs->len == 2 && !strcmp(((ExDevdFs *)g_ptr_array_index(fs, 0))->id,
				      "vfat"));
	CHECK("filesystem names", ex_devd_find_fs(d, "ext4") != NULL);
	{
		char *n = ex_devd_fs_display_name(d, "vfat");
		char *m = ex_devd_fs_display_name(d, "ntfs");

		CHECK("display names", !strcmp(n, "FAT32") && !strcmp(m, "NTFS"));
		g_free(n);
		g_free(m);
	}

	CHECK("event applied",
	      RUN_UNTIL((dev = ex_devd_find(d, "sdb1")) && *dev->mounted, 5000) &&
		      !strcmp(dev->mounted, "/media/u/MY STICK") &&
		      dev->mountuid == 1000);

	ex_devd_mount_async(d, "sdb1", NULL, mounted, &r);
	CHECK("mount answered", RUN_UNTIL(r.done, 5000));
	CHECK("mount path", r.path && !strcmp(r.path, "/media/u/MY STICK"));
	g_free(r.path);
	g_clear_error(&r.error);

	memset(&r, 0, sizeof(r));
	ex_devd_mount_async(d, "sdb1", NULL, mounted, &r);
	CHECK("journal mount answered", RUN_UNTIL(r.done, 5000));
	CHECK("journal mount refused as unclean",
	      !r.path &&
		      g_error_matches(r.error, EX_DEVD_ERROR, EX_DEVD_ERROR_UNCLEAN) &&
		      !strcmp(r.error->message,
			      "the file system journal must be replayed first"));
	g_clear_error(&r.error);
	memset(&r, 0, sizeof(r));
	ex_devd_mount_recover_async(d, "sdb1", NULL, mounted, &r);
	CHECK("recovery mount answered", RUN_UNTIL(r.done, 5000));
	CHECK("recovery mount path",
	      r.path && !strcmp(r.path, "/media/u/MY STICK") && !r.error);
	g_free(r.path);
	g_clear_error(&r.error);
	memset(&r, 0, sizeof(r));
	ex_devd_mount_recover_async(d, "../etc", NULL, mounted, &r);
	CHECK("recovery mount: bad name refused at once",
	      RUN_UNTIL(r.done, 1000) &&
		      g_error_matches(r.error, EX_DEVD_ERROR, EX_DEVD_ERROR_NOENT));
	g_clear_error(&r.error);

	memset(&r, 0, sizeof(r));
	ex_devd_eject_async(d, "sdb", NULL, ejected, &r);
	CHECK("eject answered", RUN_UNTIL(r.done, 5000));
	CHECK("eject refused with the service's message",
	      !r.ok && g_error_matches(r.error, EX_DEVD_ERROR, EX_DEVD_ERROR_BUSY) &&
		      !strcmp(r.error->message, "The drive is busy"));
	g_clear_error(&r.error);

	memset(&r, 0, sizeof(r));
	ex_devd_mount_async(d, "../etc", NULL, mounted, &r);
	CHECK("bad name refused at once",
	      RUN_UNTIL(r.done, 1000) &&
		      g_error_matches(r.error, EX_DEVD_ERROR, EX_DEVD_ERROR_NOENT));
	g_clear_error(&r.error);

	CHECK("bad lines dropped, removal applied",
	      RUN_UNTIL(!ex_devd_find(d, "sdc"), 5000) && ex_devd_is_connected(d) &&
		      ex_devd_devices(d)->len == 2 && !ex_devd_find(d, "zz") &&
		      !ex_devd_find(d, "sdq") && !ex_devd_find(d, "sdr"));
	CHECK("service script ran through", g_atomic_int_get(&s.script) == 1);
	memset(&r, 0, sizeof(r));
	ex_devd_unmount_async(d, "sdb1", NULL, unmounted, &r);
	CHECK("unmount answered", RUN_UNTIL(r.done, 5000) && r.ok);
	g_clear_error(&r.error);

	CHECK("connection lost: table empty",
	      RUN_UNTIL(!ex_devd_is_connected(d), 5000) &&
		      ex_devd_devices(d)->len == 0);
	memset(&r, 0, sizeof(r));
	ex_devd_mount_async(d, "sdb1", NULL, mounted, &r);
	CHECK("request without a service fails",
	      RUN_UNTIL(r.done, 1000) &&
		      g_error_matches(r.error, EX_DEVD_ERROR,
				      EX_DEVD_ERROR_NOT_RUNNING));
	g_clear_error(&r.error);

	CHECK("connected again by itself", RUN_UNTIL(ex_devd_is_connected(d), 5000));
	CHECK("second handshake", g_atomic_int_get(&s.handshakes) == 2);
	CHECK("too long a line: connection dropped",
	      RUN_UNTIL(!ex_devd_is_connected(d), 5000));
	CHECK("changes signalled", g_changes >= 4);

	g_object_unref(d);
	g_thread_join(th);
	if (s.failed)
		printf("  service: %s\n", s.failed);
	close(s.listen_fd);
	g_unlink(s.path);
	g_rmdir(dir);
	g_free(s.path);
	g_free(s.failed);
	g_free(dir);
}

int main(void)
{
	test_values();
	test_lines();
	test_devices();
	test_fs();
	test_errors();
	test_client();
	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
