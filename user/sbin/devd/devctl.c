// devctl - talk to the device manager (devd) from a shell.
//
//   devctl list                         the disks and volumes
//   devctl monitor                      print device events as they come
//   devctl fstypes                      the filesystems devd knows
//   devctl mount [--recover] <name>     mount a volume (prints the path);
//                                       --recover replays a journal that
//                                       must be replayed first
//   devctl unmount <name>
//   devctl eject <disk>
//   devctl format <name> <fs> [--label L] [--full] [--cluster N]
//
// For testing devd without a desktop.  Mounting, ejecting and formatting
// are allowed for root and for the user logged in at the console.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "devproto.h"

static int g_fd = -1;
static char g_in[DEVP_LINE_MAX * 2];
static size_t g_inlen;
static char g_errname[32]; /* of the last ERR line */

static void die(const char *msg)
{
	fprintf(stderr, "devctl: %s\n", msg);
	exit(1);
}

static void connect_devd(void)
{
	struct sockaddr_un sa;

	g_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (g_fd < 0)
		die("socket failed");
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	strcpy(sa.sun_path, DEVP_SOCKET_PATH);
	if (connect(g_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "devctl: cannot reach devd at %s: %s\n",
			DEVP_SOCKET_PATH, strerror(errno));
		exit(1);
	}
}

static void send_line(const char *s)
{
	size_t n = strlen(s), off = 0;

	while (off < n) {
		ssize_t w = write(g_fd, s + off, n - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			die("lost the connection to devd");
		off += (size_t)w;
	}
}

/* The next line from devd (no '\n'), in `out'. */
static void recv_line(char *out, size_t cap)
{
	for (;;) {
		char *nl = memchr(g_in, '\n', g_inlen);
		ssize_t n;

		if (nl) {
			size_t len = (size_t)(nl - g_in);
			if (len >= cap)
				die("reply line too long");
			memcpy(out, g_in, len);
			out[len] = '\0';
			memmove(g_in, nl + 1, g_inlen - len - 1);
			g_inlen -= len + 1;
			return;
		}
		if (g_inlen == sizeof(g_in))
			die("reply line too long");
		n = read(g_fd, g_in + g_inlen, sizeof(g_in) - g_inlen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			die("devd closed the connection");
		g_inlen += (size_t)n;
	}
}

/* Print one key=value token decoded. */
static void print_kv(const char *tok)
{
	char val[DEVP_LINE_MAX];
	const char *eq = strchr(tok, '=');

	if (!eq || devp_decode(val, sizeof(val), eq + 1) < 0) {
		printf(" %s", tok);
		return;
	}
	for (char *p = val; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			*p = '?';
	printf(" %.*s=\"%s\"", (int)(eq - tok), tok, val);
}

static const char *kv(char **tok, int n, const char *key, char *buf,
		      size_t cap)
{
	const char *v = devp_find(tok, n, 1, key, NULL);

	if (!v || devp_decode(buf, cap, v) < 0)
		return "";
	for (char *p = buf; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f)
			*p = '?';
	return buf;
}

/* The final OK/ERR line of a reply: 0 for OK; prints the error. */
static int final_line(char **tok, int n)
{
	char msg[DEVP_LINE_MAX];

	if (strcmp(tok[0], "OK") == 0)
		return 0;
	/* ERR <verb> [<name>] <ERRNAME> <message> */
	if (n >= 4 && strlen(tok[n - 2]) < sizeof(g_errname))
		strcpy(g_errname, tok[n - 2]);
	if (n >= 4 && devp_decode(msg, sizeof(msg), tok[n - 1]) >= 0)
		fprintf(stderr, "devctl: %s: %s (%s)\n", tok[1], msg,
			tok[n - 2]);
	else
		fprintf(stderr, "devctl: request failed\n");
	return 1;
}

static int is_final(const char *first)
{
	return strcmp(first, "OK") == 0 || strcmp(first, "ERR") == 0;
}

static void human_size(uint64_t b, char *out, size_t cap)
{
	static const char *const u[] = { "B", "K", "M", "G", "T", "P" };
	uint64_t whole = b, rem = 0;
	int i = 0;

	while (whole >= 1000 && i < 5) {
		rem = whole % 1000;
		whole /= 1000;
		i++;
	}
	if (i == 0)
		snprintf(out, cap, "%lluB", (unsigned long long)b);
	else
		snprintf(out, cap, "%llu.%llu%s", (unsigned long long)whole,
			 (unsigned long long)(rem / 100), u[i]);
}

static int cmd_list(void)
{
	char line[DEVP_LINE_MAX];

	send_line("LIST\n");
	printf("%-10s %-9s %8s %-8s %-5s %-3s %-3s %-3s %-16s %s\n", "NAME",
	       "KIND", "SIZE", "FSTYPE", "CLASS", "RO", "SYS", "MNT", "LABEL",
	       "MOUNTED");
	for (;;) {
		char *tok[DEVP_MAX_TOKENS];
		char a[128], b[16], c[16], d[200], e[260], f[16], g[8], h[8],
			sz[16], ro[8], cls[8];
		uint64_t size = 0;
		int n;

		recv_line(line, sizeof(line));
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		if (n <= 0)
			continue;
		if (is_final(tok[0]))
			return final_line(tok, n);
		if (strcmp(tok[0], "DEV") != 0)
			continue;
		devp_parse_u64(kv(tok, n, "size", f, sizeof(f)), UINT64_MAX,
			       &size);
		human_size(size, sz, sizeof(sz));
		kv(tok, n, "name", a, sizeof(a));
		kv(tok, n, "kind", b, sizeof(b));
		kv(tok, n, "fstype", c, sizeof(c));
		kv(tok, n, "label", d, sizeof(d));
		kv(tok, n, "mounted", e, sizeof(e));
		kv(tok, n, "system", h, sizeof(h));
		/* keys an older devd does not send read as "" */
		if (strcmp(kv(tok, n, "optical", g, sizeof(g)), "1") == 0)
			strcpy(cls, "cd");
		else if (strcmp(kv(tok, n, "internal", g, sizeof(g)), "1") ==
			 0)
			strcpy(cls, "int");
		else if (strcmp(kv(tok, n, "usb", g, sizeof(g)), "1") == 0)
			strcpy(cls, "usb");
		else
			strcpy(cls, "-");
		strcpy(ro, strcmp(kv(tok, n, "readonly", g, sizeof(g)), "1") ==
				   0 ?
				   "1" :
				   "0");
		printf("%-10s %-9s %8s %-8s %-5s %-3s %-3s %-3s %-16s %s\n", a,
		       b, sz, c, cls, ro, h,
		       strcmp(kv(tok, n, "supported", f, sizeof(f)), "1") == 0 ?
			       "yes" :
			       "no",
		       d, e);
	}
}

static int cmd_simple(const char *req, int print_all)
{
	char line[DEVP_LINE_MAX];

	send_line(req);
	for (;;) {
		char *tok[DEVP_MAX_TOKENS];
		int n;

		recv_line(line, sizeof(line));
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		if (n <= 0)
			continue;
		if (is_final(tok[0])) {
			int r = final_line(tok, n);

			if (r == 0 && n >= 4 && strcmp(tok[1], "MOUNT") == 0) {
				char p[DEVP_LINE_MAX];
				printf("%s\n", kv(tok, n, "path", p, sizeof(p)));
			}
			return r;
		}
		if (print_all) {
			printf("%s", tok[0]);
			for (int i = 1; i < n; i++)
				print_kv(tok[i]);
			printf("\n");
		}
	}
}

static int cmd_monitor(void)
{
	char line[DEVP_LINE_MAX];

	send_line("SUBSCRIBE\n");
	for (;;) {
		char *tok[DEVP_MAX_TOKENS];
		int n;

		recv_line(line, sizeof(line));
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		if (n <= 0)
			continue;
		if (strcmp(tok[0], "ERR") == 0)
			return final_line(tok, n);
		if (strcmp(tok[0], "EVENT") != 0)
			continue;
		printf("%s", n > 1 ? tok[1] : "?");
		for (int i = 2; i < n; i++)
			print_kv(tok[i]);
		printf("\n");
		fflush(stdout);
	}
}

static int cmd_format(int argc, char **argv)
{
	char req[DEVP_LINE_MAX], enc[DEVP_LINE_MAX], line[DEVP_LINE_MAX];
	const char *label = "", *cluster = "0";
	int quick = 1;

	if (argc < 4)
		return -1;
	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--full") == 0)
			quick = 0;
		else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc)
			label = argv[++i];
		else if (strcmp(argv[i], "--cluster") == 0 && i + 1 < argc)
			cluster = argv[++i];
		else
			return -1;
	}
	/* the longest label devd takes (NTFS: 384 bytes) all escaped */
	if (devp_encode(enc, sizeof(enc), label, strlen(label)) >= 1536)
		die("label too long");
	{
		char n1[256], n2[64], n3[64];
		if (devp_encode(n1, sizeof(n1), argv[2], strlen(argv[2])) >=
			    sizeof(n1) ||
		    devp_encode(n2, sizeof(n2), argv[3], strlen(argv[3])) >=
			    sizeof(n2) ||
		    devp_encode(n3, sizeof(n3), cluster, strlen(cluster)) >=
			    sizeof(n3))
			die("argument too long");
		snprintf(req, sizeof(req),
			 "FORMAT %s fs=%s label=%s quick=%d cluster=%s\n", n1,
			 n2, enc, quick, n3);
	}
	send_line(req);
	for (;;) {
		char *tok[DEVP_MAX_TOKENS];
		int n;

		recv_line(line, sizeof(line));
		n = devp_split(line, tok, DEVP_MAX_TOKENS);
		if (n <= 0)
			continue;
		if (strcmp(tok[0], "PROGRESS") == 0 && n == 3) {
			printf("\r%s: %s%%", argv[2], tok[2]);
			fflush(stdout);
			continue;
		}
		if (strcmp(tok[0], "PROGRESS") == 0 && n == 4 &&
		    strcmp(tok[3], "busy") == 0) {
			printf("\r%s: %s%%, writing the file system", argv[2],
			       tok[2]);
			fflush(stdout);
			continue;
		}
		if (is_final(tok[0])) {
			printf("\n");
			return final_line(tok, n);
		}
	}
}

static void usage(void)
{
	fprintf(stderr,
		"usage: devctl list | monitor | fstypes\n"
		"       devctl mount [--recover] <name> | unmount <name> | "
		"eject <disk>\n"
		"       devctl format <name> <fs> [--label L] [--full] "
		"[--cluster N]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	char req[DEVP_LINE_MAX], enc[256], line[DEVP_LINE_MAX];
	int r;

	signal(SIGPIPE, SIG_IGN);
	if (argc < 2)
		usage();
	connect_devd();
	send_line("HELLO 1\n");
	recv_line(line, sizeof(line));
	if (strcmp(line, "OK HELLO 1") != 0) {
		fprintf(stderr, "devctl: devd refused: %.200s\n", line);
		return 1;
	}
	if (strcmp(argv[1], "list") == 0 && argc == 2) {
		r = cmd_list();
	} else if (strcmp(argv[1], "monitor") == 0 && argc == 2) {
		r = cmd_monitor();
	} else if (strcmp(argv[1], "fstypes") == 0 && argc == 2) {
		r = cmd_simple("FSTYPES\n", 1);
	} else if (strcmp(argv[1], "mount") == 0 && argc == 4 &&
		   strcmp(argv[2], "--recover") == 0) {
		if (devp_encode(enc, sizeof(enc), argv[3], strlen(argv[3])) >=
		    sizeof(enc))
			usage();
		snprintf(req, sizeof(req), "MOUNT %s recover\n", enc);
		r = cmd_simple(req, 0);
	} else if ((strcmp(argv[1], "mount") == 0 ||
		    strcmp(argv[1], "unmount") == 0 ||
		    strcmp(argv[1], "eject") == 0) &&
		   argc == 3) {
		const char *verb = argv[1][0] == 'm' ? "MOUNT" :
				   argv[1][0] == 'u' ? "UNMOUNT" :
						       "EJECT";
		if (devp_encode(enc, sizeof(enc), argv[2], strlen(argv[2])) >=
		    sizeof(enc))
			usage();
		snprintf(req, sizeof(req), "%s %s\n", verb, enc);
		r = cmd_simple(req, 0);
		if (r && verb[0] == 'M' && strcmp(g_errname, "EUCLEAN") == 0)
			fprintf(stderr,
				"devctl: 'devctl mount --recover %s' replays "
				"the journal and mounts it\n",
				argv[2]);
	} else if (strcmp(argv[1], "format") == 0) {
		r = cmd_format(argc, argv);
		if (r < 0)
			usage();
	} else {
		usage();
		r = 2;
	}
	close(g_fd);
	return r;
}
