// devd: formatting a volume.
//
// A child process does the work: for a full format it first writes zeros
// over the whole volume (progress 0..90), then it becomes mkfs -- execve()
// of an absolute path with an environment of its own, no shell, every
// argument a value devd checked.  The child reports progress as "P<n>\n"
// lines on a pipe that closes when mkfs starts; mkfs's own output goes to a
// second pipe of which devd keeps the last line for an error message.  The
// device is opened with O_EXCL, so a volume that is mounted or in use by
// anyone fails instead of being overwritten under them.
//
// The client that asked gets PROGRESS lines (the last one with the word
// `busy' once mkfs runs, which reports no progress) and then OK FORMAT or
// ERR FORMAT; it sends nothing else meanwhile (devd reads its next request
// only after that reply), so replies stay in order.
//
// CANCEL <name>, on another connection, stops a format in any phase.  The
// child runs in a session of its own: its whole process group gets
// SIGTERM, and SIGKILL when it has not ended DEVD_CANCEL_GRACE_MS later
// (a deadline in the main loop's poll, devd never waits).  The child says
// "W" on the progress pipe before it first writes to the volume and "M"
// before mkfs starts; a quick format of a partition writes nothing before
// mkfs.  When nothing was written the volume is left as it was.  Else it
// holds a half-made file system that must never pass for a valid one: once
// the child is gone, a second child claims the volume the same way (O_EXCL)
// and overwrites its first and last ZERO_CHUNK with zeros, so it probes as
// unformatted.  Then the table is scanned again, the client that formatted
// gets ERR FORMAT <name> ECANCELED and every client that cancelled gets OK
// CANCEL <name>; they too wait for that reply.  A cancel after the child
// has ended on its own gets ENOENT, and the format its normal reply.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "devd.h"

#define EXIT_BUSY 70
#define EXIT_IO 71
#define EXIT_EXEC 127
#define ZERO_CHUNK (1024 * 1024)
#define TAIL_MAX 240
#define WIPE_TRIES 50          /* claiming the volume after a cancel, */
#define WIPE_RETRY_US 100000   /* while what is left of the job lets go */

enum { JOB_RUN = 0, JOB_WIPE = 1 };

struct job {
	int used;
	pid_t pid;               /* the format, then the wipe after a cancel */
	int reaped;              /* ended, found by format_cancel() */
	int status;
	int prog_fd, out_fd;
	int client_id;
	int whole_disk;
	int last_pct;
	uid_t uid;               /* who started it */
	uint32_t major, minor;   /* the volume, as it was at the start */
	uint64_t diskseq;
	int phase;               /* JOB_RUN, JOB_WIPE */
	int written;             /* the volume may have been written to */
	int cancelling;
	int cleared;             /* the wipe after the cancel succeeded */
	uint64_t kill_at;        /* SIGKILL at this time (ms); 0: none */
	int cancel_ids[DEVD_MAX_CANCELS]; /* the clients that cancelled */
	int ncancel;
	char name[DEVD_NAME_MAX];
	char disk[DEVD_NAME_MAX];
	char tool[32];
	char pbuf[32];
	size_t plen;
	char tail[TAIL_MAX + 1];
	size_t tlen;
	int tail_newline;
};

static struct job g_jobs[DEVD_MAX_JOBS];

int format_busy(const char *name)
{
	for (int i = 0; i < DEVD_MAX_JOBS; i++)
		if (g_jobs[i].used && strcmp(g_jobs[i].name, name) == 0)
			return 1;
	return 0;
}

int format_disk_busy(const char *disk)
{
	for (int i = 0; i < DEVD_MAX_JOBS; i++)
		if (g_jobs[i].used && strcmp(g_jobs[i].disk, disk) == 0)
			return 1;
	return 0;
}

/* ---- the child ------------------------------------------------------------------ */

static void child_progress(int fd, int pct)
{
	char b[16];
	int n = snprintf(b, sizeof(b), "P%d\n", pct);
	ssize_t w = write(fd, b, (size_t)n);
	(void)w;
}

/* "W" or "M" (format_mark_parse()), before what it announces */
static void child_mark(int fd, const char *mark)
{
	ssize_t w = write(fd, mark, strlen(mark));
	(void)w;
}

static int write_zeros(int fd, uint64_t off, uint64_t len, int pfd,
		       uint64_t total, uint64_t done0, int *last)
{
	static char zero[ZERO_CHUNK];
	uint64_t done = 0;

	while (done < len) {
		size_t n = len - done > ZERO_CHUNK ? ZERO_CHUNK :
						     (size_t)(len - done);
		ssize_t w = pwrite(fd, zero, n, (off_t)(off + done));

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		done += (size_t)w;
		if (pfd >= 0 && total) {
			int pct = (int)((done0 + done) * 90 / total);
			if (pct > *last) {
				*last = pct;
				child_progress(pfd, pct);
			}
		}
	}
	return 0;
}

static __attribute__((noreturn)) void
child_run(const char *dev, char **argv, int quick, int whole_disk, int pfd,
	  int ofd)
{
	static char *const envp[] = { "PATH=/sbin:/bin", "LANG=C", NULL };
	int maxfd, fd, last = 0, null;
	uint64_t size = 0;

	for (int s = 1; s < 32; s++)
		signal(s, SIG_DFL);
	setsid();
	null = open("/dev/null", O_RDWR);
	if (null < 0 || dup2(null, 0) < 0 || dup2(ofd, 1) < 0 ||
	    dup2(ofd, 2) < 0 || dup2(pfd, 3) < 0)
		_exit(EXIT_IO);
	fcntl(3, F_SETFD, FD_CLOEXEC);
	maxfd = getdtablesize();
	if (maxfd <= 0 || maxfd > 65536)
		maxfd = 4096;
	for (int i = 4; i < maxfd; i++)
		close(i);
	pfd = 3;

	/* claims the volume: fails while it is mounted or held */
	fd = open(dev, (quick && !whole_disk ? O_RDONLY : O_WRONLY) | O_EXCL |
			       O_CLOEXEC);
	if (fd < 0)
		_exit(errno == EBUSY ? EXIT_BUSY : EXIT_IO);
	if (ioctl(fd, BLKGETSIZE64, &size) < 0)
		size = 0;
	if (!quick) {
		if (!size)
			_exit(EXIT_IO);
		child_mark(pfd, "W\n");
		if (write_zeros(fd, 0, size, pfd, size, 0, &last) < 0)
			_exit(EXIT_IO);
	} else if (whole_disk && size >= 4 * ZERO_CHUNK) {
		/* an old partition table at either end must not outlive
		 * the new filesystem */
		child_mark(pfd, "W\n");
		if (write_zeros(fd, 0, ZERO_CHUNK, -1, 0, 0, &last) < 0 ||
		    write_zeros(fd, size - ZERO_CHUNK, ZERO_CHUNK, -1, 0, 0,
				&last) < 0)
			_exit(EXIT_IO);
	}
	if (!(quick && !whole_disk) && fsync(fd) < 0)
		_exit(EXIT_IO);
	close(fd);
	child_progress(pfd, quick ? 5 : 90);
	child_mark(pfd, "M\n");
	execve(argv[0], argv, envp);
	_exit(EXIT_EXEC);
}

/* After a cancel: the first and the last ZERO_CHUNK of the volume
 * overwritten with zeros, so that what the format left there is never
 * taken for a file system.  `want' is the device number the volume had. */
static __attribute__((noreturn)) void wipe_run(const char *dev, dev_t want)
{
	struct stat st;
	uint64_t size = 0, head;
	int maxfd, fd = -1, err = EIO, last = 0;

	for (int s = 1; s < 32; s++)
		signal(s, SIG_DFL);
	setsid();
	maxfd = getdtablesize();
	if (maxfd <= 0 || maxfd > 65536)
		maxfd = 4096;
	for (int i = 3; i < maxfd; i++)
		close(i);
	/* the same claim as the format's; what is left of the stopped job
	 * may hold the volume a moment longer */
	for (int i = 0; i < WIPE_TRIES; i++) {
		fd = open(dev, O_WRONLY | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
		if (fd >= 0)
			break;
		err = errno;
		if (err != EBUSY)
			break;
		usleep(WIPE_RETRY_US);
	}
	if (fd < 0)
		_exit(err == EBUSY ? EXIT_BUSY : EXIT_IO);
	if (fstat(fd, &st) < 0 || !S_ISBLK(st.st_mode) || st.st_rdev != want)
		_exit(EXIT_IO);
	if (ioctl(fd, BLKGETSIZE64, &size) < 0 || !size)
		_exit(EXIT_IO);
	head = size < ZERO_CHUNK ? size : ZERO_CHUNK;
	if (write_zeros(fd, 0, head, -1, 0, 0, &last) < 0)
		_exit(EXIT_IO);
	if (size > ZERO_CHUNK &&
	    write_zeros(fd, size - ZERO_CHUNK, ZERO_CHUNK, -1, 0, 0, &last) < 0)
		_exit(EXIT_IO);
	if (fsync(fd) < 0)
		_exit(EXIT_IO);
	close(fd);
	_exit(0);
}

/* ---- starting --------------------------------------------------------------------- */

static int parse_flag(const char *s, int *out)
{
	if (strcmp(s, "0") == 0)
		*out = 0;
	else if (strcmp(s, "1") == 0)
		*out = 1;
	else
		return -1;
	return 0;
}

static void send_progress(struct job *j, int pct)
{
	struct client *c = client_by_id(j->client_id);
	struct devp_line l;
	char num[8];

	if (pct <= j->last_pct || j->cancelling)
		return;
	j->last_pct = pct;
	if (!c)
		return;
	snprintf(num, sizeof(num), "%d", pct);
	devp_line_begin(&l, "PROGRESS");
	devp_line_enc(&l, j->name);
	devp_line_word(&l, num);
	if (devp_line_end(&l) > 0)
		client_send_line(c, &l);
}

/* mkfs is running and reports no progress of its own: the percentage
 * stays where it is, and `busy' says the step takes time of its own. */
static void send_busy(struct job *j)
{
	struct client *c = client_by_id(j->client_id);
	struct devp_line l;
	char num[8];

	/* a cancelled child's pipe closes too: no mkfs is starting */
	if (!c || j->cancelling)
		return;
	snprintf(num, sizeof(num), "%d", j->last_pct > 0 ? j->last_pct : 0);
	devp_line_begin(&l, "PROGRESS");
	devp_line_enc(&l, j->name);
	devp_line_word(&l, num);
	devp_line_word(&l, "busy");
	if (devp_line_end(&l) > 0)
		client_send_line(c, &l);
}

int format_start(struct client *c, struct dev *d, char *const *tok, int ntok,
		 const char **msg)
{
	static const char *const keys[] = { "fs", "label", "quick", "cluster" };
	const char *fs = NULL, *lab = NULL, *q = NULL, *cl = NULL;
	const struct fs_type *t;
	/* an NTFS label is up to 128 UTF-16 units: 384 bytes of UTF-8 */
	char raw[512], label[400], dev[DEVD_PATH_MAX];
	char store[1024];
	char *argv[24];
	int quick = 1, dup = 0, slot = -1, prog[2], out[2], sector = 512;
	long rawlen = 0;
	uint64_t cluster = 0;
	uid_t uid = c->uid;
	gid_t gid = c->gid;
	struct passwd *pw;
	struct job *j;
	pid_t pid;
	int r;

	/* key=value tokens: only the known keys, each once */
	for (int i = 2; i < ntok; i++) {
		int known = 0;
		for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			size_t kl = strlen(keys[k]);
			if (strncmp(tok[i], keys[k], kl) == 0 &&
			    tok[i][kl] == '=')
				known = 1;
		}
		if (!known) {
			*msg = "unknown argument";
			return -EINVAL;
		}
	}
	fs = devp_find(tok, ntok, 2, "fs", &dup);
	lab = devp_find(tok, ntok, 2, "label", &dup);
	q = devp_find(tok, ntok, 2, "quick", &dup);
	cl = devp_find(tok, ntok, 2, "cluster", &dup);
	if (dup || !fs) {
		*msg = "bad arguments";
		return -EINVAL;
	}
	t = fs_by_id(fs);
	if (!t || !t->mkfs || !t->mkfs_argv || !t->label_ok) {
		*msg = "cannot format with this file system";
		return -EINVAL;
	}
	if (lab) {
		rawlen = devp_decode(raw, sizeof(raw), lab);
		if (rawlen < 0 || (size_t)rawlen != strlen(raw)) {
			*msg = "bad label";
			return -EINVAL;
		}
	} else {
		raw[0] = '\0';
	}
	if (t->label_ok(raw, (size_t)rawlen, label, sizeof(label)) < 0) {
		*msg = "the label is not valid for this file system";
		return -EINVAL;
	}
	if (q && parse_flag(q, &quick) < 0) {
		*msg = "bad quick value";
		return -EINVAL;
	}
	if (cl && (devp_parse_u64(cl, 1u << 24, &cluster) < 0 ||
		   !fs_cluster_ok(t, (uint32_t)cluster))) {
		*msg = "allocation unit size not offered for this file system";
		return -EINVAL;
	}

	/* the volume (the caller checked the rules; once more here, so that
	 * nothing reaches mkfs past them) */
	{
		const char *why = "";

		r = dev_policy_check(d, OP_FORMAT, &why);
		if (r < 0 || d->readonly || d->optical || d->internal ||
		    !d->usb) {
			*msg = r < 0 ? why : "this drive is not formatted here";
			return -EPERM;
		}
	}
	if (!dev_is_volume(d)) {
		*msg = "the drive has partitions; format one of them";
		return -EINVAL;
	}
	if (d->state != DS_READY || format_busy(d->name)) {
		*msg = "the volume is being formatted";
		return -EBUSY;
	}
	if (d->ejected) {
		*msg = "the drive was ejected; unplug it and plug it in again";
		return -ENODEV;
	}
	if (d->foreign_mount) {
		*msg = "the volume is in use";
		return -EBUSY;
	}
	if (d->devd_mount) {
		char path[DEVD_PATH_MAX];

		if (uid != 0 && (long)uid != d->mountuid) {
			*msg = "mounted for another user";
			return -EBUSY;
		}
		strlcpy_d(path, d->mounted, sizeof(path));
		r = unmount_path(path, msg);
		if (r < 0)
			return r;
	}
	if (!dev_node_ok(d)) {
		*msg = "device node missing";
		return -ENODEV;
	}
	for (int i = 0; i < DEVD_MAX_JOBS; i++)
		if (!g_jobs[i].used) {
			slot = i;
			break;
		}
	if (slot < 0) {
		*msg = "too many formats at once";
		return -EAGAIN;
	}

	snprintf(dev, sizeof(dev), "/dev/%s", d->name);
	{
		int fd = open(dev, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		int ss = 0;

		if (fd >= 0) {
			if (ioctl(fd, BLKSSZGET, &ss) == 0 && ss >= 512 &&
			    ss <= 65536)
				sector = ss;
			close(fd);
		}
	}
	pw = getpwuid(uid);
	if (pw)
		gid = pw->pw_gid;
	r = t->mkfs_argv(argv, (int)(sizeof(argv) / sizeof(argv[0])), store,
			 sizeof(store), dev, label, (uint32_t)cluster,
			 (uint32_t)sector, d->kind == DEV_DISK, uid, gid);
	if (r < 0) {
		*msg = "allocation unit size not possible on this drive";
		return -EINVAL;
	}

	if (pipe2(prog, O_CLOEXEC) < 0) {
		*msg = "out of resources";
		return -errno;
	}
	if (pipe2(out, O_CLOEXEC) < 0) {
		int e = errno;
		close(prog[0]);
		close(prog[1]);
		*msg = "out of resources";
		return -e;
	}
	pid = fork();
	if (pid < 0) {
		int e = errno;
		close(prog[0]);
		close(prog[1]);
		close(out[0]);
		close(out[1]);
		*msg = "cannot start the format";
		return -e;
	}
	if (pid == 0)
		child_run(dev, argv, quick, d->kind == DEV_DISK, prog[1],
			  out[1]);
	close(prog[1]);
	close(out[1]);
	fcntl(prog[0], F_SETFL, O_NONBLOCK);
	fcntl(out[0], F_SETFL, O_NONBLOCK);

	j = &g_jobs[slot];
	memset(j, 0, sizeof(*j));
	j->used = 1;
	j->pid = pid;
	j->prog_fd = prog[0];
	j->out_fd = out[0];
	j->client_id = c->id;
	j->whole_disk = d->kind == DEV_DISK;
	j->last_pct = -1;
	j->uid = uid;
	j->major = d->major;
	j->minor = d->minor;
	j->diskseq = d->diskseq;
	j->phase = JOB_RUN;
	strlcpy_d(j->name, d->name, sizeof(j->name));
	strlcpy_d(j->disk, d->disk, sizeof(j->disk));
	{
		const char *b = strrchr(argv[0], '/');
		strlcpy_d(j->tool, b ? b + 1 : argv[0], sizeof(j->tool));
	}
	log_info("formatting %s as %s for uid %u (%s)", j->name, t->id,
		 (unsigned)uid, quick ? "quick" : "full");
	c->waiting = 1;
	send_progress(j, 0);
	/* state=formatting for everybody */
	tab_rescan(NULL, 0);
	return 0;
}

/* ---- running ----------------------------------------------------------------------- */

int format_pollfds(struct pollfd *pfd, int max)
{
	int n = 0;

	for (int i = 0; i < DEVD_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];

		if (!j->used)
			continue;
		if (j->prog_fd >= 0 && n < max) {
			pfd[n].fd = j->prog_fd;
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			n++;
		}
		if (j->out_fd >= 0 && n < max) {
			pfd[n].fd = j->out_fd;
			pfd[n].events = POLLIN;
			pfd[n].revents = 0;
			n++;
		}
	}
	return n;
}

/* Returns 1 when the progress pipe closed: mkfs started (or the child
 * ended). */
static int read_progress(struct job *j)
{
	char buf[128];

	for (;;) {
		ssize_t n = read(j->prog_fd, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (n <= 0) {
			close(j->prog_fd);
			j->prog_fd = -1;
			return 1;
		}
		for (ssize_t i = 0; i < n; i++) {
			char ch = buf[i];

			if (ch != '\n') {
				if (j->plen + 1 < sizeof(j->pbuf))
					j->pbuf[j->plen++] = ch;
				continue;
			}
			j->pbuf[j->plen] = '\0';
			{
				int pct = 0;
				int m = format_mark_parse(j->pbuf, &pct);

				if (m == FMARK_PROGRESS)
					send_progress(j, pct);
				else if (format_mark_writes(m))
					j->written = 1;
			}
			j->plen = 0;
		}
	}
}

/* mkfs's output: keep the last line, for the error message */
static void read_output(struct job *j)
{
	char buf[256];

	for (;;) {
		ssize_t n = read(j->out_fd, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n <= 0) {
			close(j->out_fd);
			j->out_fd = -1;
			return;
		}
		for (ssize_t i = 0; i < n; i++) {
			unsigned char ch = (unsigned char)buf[i];

			if (ch == '\n') {
				j->tail_newline = 1;
				continue;
			}
			if (j->tail_newline) {
				j->tlen = 0;
				j->tail_newline = 0;
			}
			if (ch < 0x20 || ch > 0x7e)
				ch = '?';
			if (j->tlen < TAIL_MAX)
				j->tail[j->tlen++] = (char)ch;
		}
		j->tail[j->tlen] = '\0';
	}
}

void format_handle(struct pollfd *pfd, int n)
{
	for (int k = 0; k < n; k++) {
		if (!pfd[k].revents)
			continue;
		for (int i = 0; i < DEVD_MAX_JOBS; i++) {
			struct job *j = &g_jobs[i];

			if (!j->used)
				continue;
			if (pfd[k].fd == j->prog_fd) {
				if (read_progress(j))
					send_busy(j);
			}
			else if (pfd[k].fd == j->out_fd)
				read_output(j);
		}
	}
}

/* SIGTERM, SIGKILL: the child's process group (it calls setsid() at once;
 * before that there is no group of that number) and the child itself. */
static void signal_job(struct job *j, int sig)
{
	if (j->reaped || j->pid <= 0)
		return;
	kill(-j->pid, sig);
	kill(j->pid, sig);
}

static void finish(struct job *j)
{
	struct client *c;
	char disk[DEVD_NAME_MAX], name[DEVD_NAME_MAX], msg[320];
	int cancel_ids[DEVD_MAX_CANCELS];
	int ok = 0, err = EIO, code = -1, cancelled, ncancel;

	if (WIFEXITED(j->status)) {
		code = WEXITSTATUS(j->status);
		ok = code == 0;
	}
	cancelled = j->cancelling;
	if (cancelled) {
		/* whatever the child got to: never OK after a cancel */
		ok = 0;
		err = ECANCELED;
		snprintf(msg, sizeof(msg), "%s",
			 format_cancel_message(j->written, j->cleared));
	} else if (ok) {
		msg[0] = '\0';
	} else if (code == EXIT_BUSY) {
		err = EBUSY;
		snprintf(msg, sizeof(msg), "the volume is in use");
	} else if (code == EXIT_IO) {
		snprintf(msg, sizeof(msg), "writing to the drive failed");
	} else if (code == EXIT_EXEC) {
		err = ENOENT;
		snprintf(msg, sizeof(msg), "%s could not be started", j->tool);
	} else if (code < 0) {
		err = ECANCELED;
		snprintf(msg, sizeof(msg), "the format was interrupted");
	} else {
		snprintf(msg, sizeof(msg), "%s failed%s%s", j->tool,
			 j->tlen ? ": " : "", j->tail);
	}
	if ((ok || (cancelled && j->cleared)) && j->whole_disk) {
		char dev[DEVD_PATH_MAX];
		int fd;

		/* the disk has no partition table now; the kernel must
		 * forget the partitions it knew */
		snprintf(dev, sizeof(dev), "/dev/%s", j->disk);
		fd = open(dev, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd >= 0) {
			if (ioctl(fd, BLKRRPART, 0) < 0)
				log_debug("BLKRRPART %s: %s", j->disk,
					  strerror(errno));
			close(fd);
		}
	}
	strlcpy_d(disk, j->disk, sizeof(disk));
	strlcpy_d(name, j->name, sizeof(name));
	ncancel = j->ncancel;
	memcpy(cancel_ids, j->cancel_ids, sizeof(cancel_ids));
	c = client_by_id(j->client_id);
	if (ok)
		send_progress(j, 100);
	j->used = 0;
	if (ok)
		log_info("%s formatted", name);
	else if (cancelled)
		log_info("formatting %s cancelled: %s", name,
			 !j->written ? "nothing was written" :
			 j->cleared  ? "the volume was cleared" :
				       "the volume could not be cleared");
	else
		log_warn("formatting %s failed: %s", name, msg);
	/* probe it again; CHANGED goes out before the reply */
	tab_rescan(disk, 0);
	if (c) {
		if (ok) {
			struct devp_line l;

			devp_line_begin(&l, "OK FORMAT");
			devp_line_enc(&l, name);
			if (devp_line_end(&l) > 0)
				client_send_line(c, &l);
		} else {
			client_err(c, "FORMAT", name, err, msg);
		}
		c->waiting = 0;
	}
	for (int i = 0; i < ncancel; i++) {
		struct client *k = client_by_id(cancel_ids[i]);
		struct devp_line l;

		if (!k)
			continue;
		devp_line_begin(&l, "OK CANCEL");
		devp_line_enc(&l, name);
		if (devp_line_end(&l) > 0)
			client_send_line(k, &l);
		k->waiting = 0;
	}
	/* the job's slot is free: what these clients ask next may take it */
	if (c)
		client_process(c);
	for (int i = 0; i < ncancel; i++) {
		struct client *k = client_by_id(cancel_ids[i]);

		if (k)
			client_process(k);
	}
}

/* After a cancel of a job that wrote to the volume: the wipe child.  0
 * when it runs, -1 when the volume cannot be cleared (it went, or is
 * another one now). */
static int wipe_start(struct job *j)
{
	struct dev *d = tab_find(j->name);
	char dev[DEVD_PATH_MAX];
	pid_t pid;

	if (!d || d->major != j->major || d->minor != j->minor ||
	    d->diskseq != j->diskseq || !dev_node_ok(d)) {
		log_warn("%s went or changed; it is not cleared", j->name);
		return -1;
	}
	snprintf(dev, sizeof(dev), "/dev/%s", j->name);
	pid = fork();
	if (pid < 0) {
		log_warn("cannot clear %s: %s", j->name, strerror(errno));
		return -1;
	}
	if (pid == 0)
		wipe_run(dev, makedev(j->major, j->minor));
	j->pid = pid;
	j->phase = JOB_WIPE;
	j->status = 0;
	log_info("clearing the start and the end of %s", j->name);
	return 0;
}

/* The job's child ended (its status in j->status). */
static void job_exited(struct job *j)
{
	if (j->phase == JOB_WIPE) {
		j->cleared = WIFEXITED(j->status) &&
			     WEXITSTATUS(j->status) == 0;
		finish(j);
		return;
	}
	/* everything the child said, the marks included */
	if (j->prog_fd >= 0)
		read_progress(j);
	if (j->out_fd >= 0)
		read_output(j);
	if (j->prog_fd >= 0)
		close(j->prog_fd);
	if (j->out_fd >= 0)
		close(j->out_fd);
	j->prog_fd = j->out_fd = -1;
	if (j->cancelling) {
		j->kill_at = 0;
		/* what is left of its process group (mkfs's own children):
		 * the group outlives its leader only while it has members,
		 * so this number is still theirs */
		kill(-j->pid, SIGKILL);
		if (j->written && wipe_start(j) == 0)
			return;
	}
	finish(j);
}

void format_reap(void)
{
	/* ended before a cancel could stop them (format_cancel()) */
	for (int i = 0; i < DEVD_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];

		if (j->used && j->reaped) {
			j->reaped = 0;
			job_exited(j);
		}
	}
	for (;;) {
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG);

		if (pid < 0 && errno == EINTR)
			continue;
		if (pid <= 0)
			break;
		for (int i = 0; i < DEVD_MAX_JOBS; i++) {
			struct job *j = &g_jobs[i];

			if (j->used && !j->reaped && j->pid == pid) {
				j->status = status;
				job_exited(j);
				break;
			}
		}
	}
}

int format_cancel(struct client *c, const char *name, const char **msg)
{
	struct job *j = NULL;

	for (int i = 0; i < DEVD_MAX_JOBS; i++)
		if (g_jobs[i].used && strcmp(g_jobs[i].name, name) == 0) {
			j = &g_jobs[i];
			break;
		}
	if (!j) {
		*msg = "no format of this drive is running";
		return -ENOENT;
	}
	if (!format_cancel_allowed(c->uid, (uint32_t)j->uid)) {
		*msg = "the format was started by another user";
		return -EPERM;
	}
	if (!j->cancelling && !j->reaped) {
		int st = 0;

		/* it may have ended just now: then it finished, and keeps
		 * its own result */
		if (waitpid(j->pid, &st, WNOHANG) == j->pid) {
			j->status = st;
			j->reaped = 1;
		}
	}
	if (!j->cancelling && j->reaped) {
		*msg = "the format has already finished";
		return -ENOENT;
	}
	if (j->ncancel >= DEVD_MAX_CANCELS) {
		*msg = "the format is already being cancelled";
		return -EBUSY;
	}
	j->cancel_ids[j->ncancel++] = c->id;
	c->waiting = 1;
	if (!j->cancelling) {
		j->cancelling = 1;
		log_info("cancelling the format of %s for uid %u", j->name,
			 (unsigned)c->uid);
		if (j->phase == JOB_RUN) {
			signal_job(j, SIGTERM);
			j->kill_at = now_ms() + DEVD_CANCEL_GRACE_MS;
		}
	}
	return 0;
}

int format_timeout(uint64_t now)
{
	int t = -1;

	for (int i = 0; i < DEVD_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];
		int ms;

		if (!j->used)
			continue;
		if (j->reaped)
			return 0;
		if (!j->kill_at)
			continue;
		ms = j->kill_at > now ? (int)(j->kill_at - now) : 0;
		if (t < 0 || ms < t)
			t = ms;
	}
	return t;
}

void format_tick(uint64_t now)
{
	for (int i = 0; i < DEVD_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];

		if (!j->used || !j->kill_at || now < j->kill_at)
			continue;
		j->kill_at = 0;
		if (j->phase != JOB_RUN || j->reaped)
			continue;
		log_warn("formatting %s did not stop; killed", j->name);
		signal_job(j, SIGKILL);
	}
}

void format_client_gone(int client_id)
{
	for (int i = 0; i < DEVD_MAX_JOBS; i++)
		if (g_jobs[i].used && g_jobs[i].client_id == client_id)
			g_jobs[i].client_id = 0;
}

void format_kill_all(void)
{
	for (int i = 0; i < DEVD_MAX_JOBS; i++) {
		struct job *j = &g_jobs[i];
		int st;

		if (!j->used)
			continue;
		if (!j->reaped) {
			kill(-j->pid, SIGKILL);
			kill(j->pid, SIGKILL);
			while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR)
				;
		}
		if (j->prog_fd >= 0)
			close(j->prog_fd);
		if (j->out_fd >= 0)
			close(j->out_fd);
		j->used = 0;
	}
}
