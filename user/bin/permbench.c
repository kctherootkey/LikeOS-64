/*
 * permbench - temporary in-guest microbenchmark: times common file syscalls
 * first as root, then as uid 1000, to localize the non-root slowdown.
 * Output goes to stdout (console, mirrored to serial).  NOT part of the
 * shipped system; wired into init temporarily for measurement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

static long long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void report(const char *label, const char *who, long long us, int n)
{
	/* centi-microseconds per op to keep integer math */
	long long per100 = (us * 100) / n;
	printf("PB %-14s %-5s total=%-8lldus per-op=%lld.%02lldus n=%d\n",
	       label, who, us, per100 / 100, per100 % 100, n);
}

static void bench_stat(const char *who, int n)
{
	struct stat st;
	long long t0 = now_us();
	for (int i = 0; i < n; i++)
		stat("/bin/ls", &st);
	report("stat", who, now_us() - t0, n);
}

static void bench_open_close(const char *who, int n)
{
	long long t0 = now_us();
	for (int i = 0; i < n; i++) {
		int fd = open("/etc/profile", O_RDONLY);
		if (fd >= 0)
			close(fd);
	}
	report("open+close", who, now_us() - t0, n);
}

static void bench_open_read(const char *who, int n)
{
	char buf[512];
	long long t0 = now_us();
	for (int i = 0; i < n; i++) {
		int fd = open("/etc/profile", O_RDONLY);
		if (fd >= 0) {
			read(fd, buf, sizeof(buf));
			close(fd);
		}
	}
	report("open+read", who, now_us() - t0, n);
}

static void bench_readdir(const char *who, int n)
{
	long long t0 = now_us();
	int entries = 0;
	for (int i = 0; i < n; i++) {
		DIR *d = opendir("/bin");
		if (!d)
			continue;
		struct dirent *de;
		while ((de = readdir(d)) != 0)
			entries++;
		closedir(d);
	}
	report("readdir", who, now_us() - t0, n);
}

static void bench_ls_la(const char *who, int n)
{
	/* what `ls -la` does: readdir + stat every entry */
	char path[512];
	struct stat st;
	long long t0 = now_us();
	for (int i = 0; i < n; i++) {
		DIR *d = opendir("/bin");
		if (!d)
			continue;
		struct dirent *de;
		while ((de = readdir(d)) != 0) {
			snprintf(path, sizeof(path), "/bin/%s", de->d_name);
			stat(path, &st);
		}
		closedir(d);
	}
	report("ls-la", who, now_us() - t0, n);
}

static void bench_file_write(const char *who, int n)
{
	char buf[64];
	char path[64];
	memset(buf, 'x', sizeof(buf));
	/* per-uid file: the root run's file would not be writable by uid 1000 */
	snprintf(path, sizeof(path), "/tmp/permbench.%d.dat", getuid());
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		printf("PB file-write %s SKIP (open failed)\n", who);
		return;
	}
	long long t0 = now_us();
	for (int i = 0; i < n; i++)
		write(fd, buf, sizeof(buf));
	report("file-write", who, now_us() - t0, n);
	close(fd);
}

static void bench_devnull_write(const char *who, int n)
{
	char buf[64];
	memset(buf, 'x', sizeof(buf));
	int fd = open("/dev/null", O_WRONLY);
	if (fd < 0) {
		printf("PB null-write %s SKIP (open failed)\n", who);
		return;
	}
	long long t0 = now_us();
	for (int i = 0; i < n; i++)
		write(fd, buf, sizeof(buf));
	report("null-write", who, now_us() - t0, n);
	close(fd);
}

static void bench_tty_write(const char *who, int n)
{
	/* short escape-free string straight to the console, like nano redraws */
	const char *s = "\r                                        \r";
	long long t0 = now_us();
	for (int i = 0; i < n; i++)
		write(1, s, strlen(s));
	report("tty-write", who, now_us() - t0, n);
}

static void run_suite(const char *who)
{
	/* warmup: populate all kernel caches before timing */
	bench_stat(who, 50);
	bench_open_close(who, 50);
	printf("PB ---- timed runs (%s) ----\n", who);
	bench_stat(who, 1000);
	bench_open_close(who, 1000);
	bench_open_read(who, 500);
	bench_readdir(who, 50);
	bench_ls_la(who, 20);
	bench_file_write(who, 1000);
	bench_devnull_write(who, 1000);
	bench_tty_write(who, 200);
}

int main(void)
{
	printf("PB ================ permbench start ================\n");

	/* /tmp for the non-root write test */
	mkdir("/tmp", 0777);
	chmod("/tmp", 01777);

	run_suite("root");

	int g = 1000;
	setgroups(1, &g);
	if (setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
		printf("PB FAILED to drop privileges\n");
		return 1;
	}
	printf("PB dropped to uid=%d euid=%d\n", getuid(), geteuid());

	run_suite("user");

	printf("PB ================ permbench done ================\n");
	return 0;
}
