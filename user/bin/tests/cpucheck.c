// cpucheck -- is the kernel's CPU-time accounting consistent with the clock?
//
// Takes two process listings (getprocinfo), a fixed interval apart, and
// prints how much CPU time the kernel says was consumed in between against
// how much wall-clock time passed.  Over N processors the sum can never
// exceed N seconds per second; anything above that is an accounting
// defect, and the per-task table shows where it lands.
//
//   cpucheck [-s seconds] [-n top] [-f]   (default 3 s, top 8)
//   -f folds threads into their processes first, exactly as top does, and
//      reports the folded rows: this is the computation behind top's %CPU
//      column, so a row above the ceiling here is a folding defect.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/procinfo.h>

#define MAXP 4096

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct row {
	int pid, tgid, state;
	char comm[32];
	unsigned long du, ds;
};

static int by_total(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	unsigned long tx = x->du + x->ds, ty = y->du + y->ds;

	return tx < ty ? 1 : tx > ty ? -1 : 0;
}

int main(int argc, char **argv)
{
	int secs = 3, top = 8, opt, fold = 0;

	while ((opt = getopt(argc, argv, "s:n:f")) != -1) {
		if (opt == 's')
			secs = atoi(optarg);
		else if (opt == 'n')
			top = atoi(optarg);
		else if (opt == 'f')
			fold = 1;
	}
	long hz = sysconf(_SC_CLK_TCK), ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	procinfo_t *a = malloc(MAXP * sizeof *a), *b = malloc(MAXP * sizeof *b);

	if (!a || !b)
		return 2;
	double t0 = now_s();
	int na = getprocinfo(a, MAXP);
	struct timespec ts = { secs, 0 };

	nanosleep(&ts, NULL);
	int nb = getprocinfo(b, MAXP);
	double t1 = now_s();

	if (na < 0 || nb < 0) {
		perror("getprocinfo");
		return 2;
	}
	if (fold) {
		na = procinfo_fold_threads(a, na);
		nb = procinfo_fold_threads(b, nb);
	}
	struct row *rows = calloc(nb, sizeof *rows);
	int nrows = 0;
	unsigned long sum_u = 0, sum_s = 0;

	for (int i = 0; i < nb; i++) {
		unsigned long pu = 0, ps = 0;
		int found = 0;

		for (int j = 0; j < na; j++) {
			if (a[j].pid == b[i].pid) {
				pu = a[j].utime_ticks;
				ps = a[j].stime_ticks;
				found = 1;
				break;
			}
		}
		if (!found)
			continue; /* born inside the interval: no baseline */
		struct row *r = &rows[nrows++];

		r->pid = b[i].pid;
		r->tgid = b[i].tgid;
		r->state = b[i].state;
		strncpy(r->comm, b[i].comm, sizeof r->comm - 1);
		r->du = b[i].utime_ticks >= pu ? b[i].utime_ticks - pu : 0;
		r->ds = b[i].stime_ticks >= ps ? b[i].stime_ticks - ps : 0;
		sum_u += r->du;
		sum_s += r->ds;
	}
	double dt = t1 - t0;
	double total = (double)(sum_u + sum_s) / (double)hz;

	printf("cpucheck: %ld processors, %ld ticks/s, %d %s, interval %.3f s\n",
	       ncpu, hz, nrows, fold ? "processes (threads folded)" : "tasks", dt);
	printf("cpucheck: kernel charged %.2f s user + %.2f s system = %.2f s of CPU in %.3f s wall\n",
	       (double)sum_u / hz, (double)sum_s / hz, total, dt);
	printf("cpucheck: that is %.1f%% of one processor; the ceiling is %ld00%%: %s\n",
	       total * 100.0 / dt, ncpu,
	       total > dt * (double)ncpu * 1.02 ? "IMPOSSIBLE" : "plausible");
	qsort(rows, nrows, sizeof *rows, by_total);
	printf("%7s %7s %5s %8s %8s %7s  %s\n", "pid", "tgid", "state", "user",
	       "sys", "%cpu", "comm");
	for (int i = 0; i < nrows && i < top; i++)
		printf("%7d %7d %5d %7.2fs %7.2fs %6.1f%%  %s\n", rows[i].pid,
		       rows[i].tgid, rows[i].state, (double)rows[i].du / hz,
		       (double)rows[i].ds / hz,
		       (double)(rows[i].du + rows[i].ds) * 100.0 / hz / dt,
		       rows[i].comm);
	return 0;
}
