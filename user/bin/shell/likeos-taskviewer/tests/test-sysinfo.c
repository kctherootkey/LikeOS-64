// LikeOS Task Viewer -- tests of the system sampler's parsers and of the
// readers of /etc/inittab and the X session script (src/tv-sysinfo.c).
//
// Plain GLib tests on text samples in the form LikeOS writes them; nothing
// here reads the running system.
//
// Copyright (C) 2026 The LikeOS Project

#include <string.h>

#include "../src/tv-sysinfo.h"

static void test_meminfo(void)
{
	static const char text[] = "MemTotal:        8000000 kB\n"
				   "MemFree:         2000000 kB\n"
				   "MemAvailable:    5000000 kB\n"
				   "Cached:          3000000 kB\n"
				   "SwapTotal:               0 kB\n"
				   "SwapFree:                0 kB\n";
	TvMemInfo m;

	g_assert_true(tv_parse_meminfo(text, &m));
	g_assert_cmpuint(m.total, ==, 8000000ULL * 1024);
	g_assert_cmpuint(m.free, ==, 2000000ULL * 1024);
	g_assert_cmpuint(m.available, ==, 5000000ULL * 1024);
	g_assert_cmpuint(m.cached, ==, 3000000ULL * 1024);
	g_assert_cmpuint(m.in_use, ==, 3000000ULL * 1024);
	g_assert_true(m.have_available && m.have_cached);
	g_assert_false(m.have_committed || m.have_buffers);

	/* Without MemAvailable: free + cache + buffers. */
	g_assert_true(tv_parse_meminfo("MemTotal: 100 kB\nMemFree: 10 kB\n"
				       "Buffers: 5 kB\nCached: 20 kB\n"
				       "Committed_AS: 70 kB\n",
				       &m));
	g_assert_cmpuint(m.available, ==, 35 * 1024);
	g_assert_cmpuint(m.in_use, ==, 65 * 1024);
	g_assert_true(m.have_committed);
	g_assert_cmpuint(m.committed, ==, 70 * 1024);

	g_assert_false(tv_parse_meminfo("SwapTotal: 0 kB\n", &m));
	g_assert_false(tv_parse_meminfo("", &m));
}

static void test_cpuinfo(void)
{
	static const char text[] = "processor\t: 0\n"
				   "vendor_id\t: GenuineIntel\n"
				   "cpu MHz\t\t: 2700\n\n"
				   "processor\t: 1\n"
				   "vendor_id\t: GenuineIntel\n"
				   "cpu MHz\t\t: 2900\n\n";
	double mhz = -1;

	g_assert_cmpint(tv_parse_cpuinfo(text, &mhz), ==, 2);
	g_assert_cmpfloat_with_epsilon(mhz, 2800.0, 1e-9);
	g_assert_cmpint(tv_parse_cpuinfo("", &mhz), ==, 0);
	g_assert_cmpfloat(mhz, ==, 0.0);
	g_assert_cmpfloat_with_epsilon(tv_parse_uptime("1234.56 1234.56\n"),
				       1234.56, 1e-9);
	g_assert_cmpfloat(tv_parse_uptime("x"), <, 0.0);
}

static void test_brand(void)
{
	char s[64];

	g_assert_cmpfloat_with_epsilon(
		tv_parse_brand_mhz("Intel(R) Core(TM) i7-6820HQ CPU @ 2.70GHz"),
		2700.0, 1e-6);
	g_assert_cmpfloat_with_epsilon(tv_parse_brand_mhz("Some CPU @ 800 MHz"),
				       800.0, 1e-6);
	g_assert_cmpfloat(tv_parse_brand_mhz("AMD Ryzen 7 5800X 8-Core"), ==,
			  0.0);
	strcpy(s, "   Intel(R)  Xeon(R)   CPU  ");
	tv_squeeze_spaces(s);
	g_assert_cmpstr(s, ==, "Intel(R) Xeon(R) CPU");
	s[0] = '\0';
	tv_squeeze_spaces(s);
	g_assert_cmpstr(s, ==, "");
}

static void test_topology(void)
{
	int sockets, cores;

	/* 4 cores with 2 threads each, one package. */
	tv_cpu_topology(8, 2, 8, &sockets, &cores);
	g_assert_cmpint(sockets, ==, 1);
	g_assert_cmpint(cores, ==, 4);
	/* Two packages of 4. */
	tv_cpu_topology(8, 1, 4, &sockets, &cores);
	g_assert_cmpint(sockets, ==, 2);
	g_assert_cmpint(cores, ==, 8);
	/* A virtual machine given 2 of a 16-thread package. */
	tv_cpu_topology(2, 2, 16, &sockets, &cores);
	g_assert_cmpint(sockets, ==, 1);
	g_assert_cmpint(cores, ==, 1);
	/* Nothing known. */
	tv_cpu_topology(6, 0, 0, &sockets, &cores);
	g_assert_cmpint(sockets, ==, 1);
	g_assert_cmpint(cores, ==, 6);
	tv_cpu_topology(0, 0, 0, &sockets, &cores);
	g_assert_cmpint(sockets, ==, 1);
	g_assert_cmpint(cores, ==, 1);
}

static void test_cpu_usage(void)
{
	/* Interval: 100 ticks, 2 processors, the first listing at tick
	 * 1000. */
	const TvTaskTicks prev[] = {
		{ .tid = 1, .cpu = 0, .ticks = 10, .start = 0 },
		{ .tid = 5, .cpu = 1, .ticks = 100, .start = 50 },
		{ .tid = 9, .cpu = 1, .ticks = 7, .start = 60 },
	};
	const TvTaskTicks cur[] = {
		{ .tid = 1, .cpu = 0, .ticks = 60, .start = 0 }, /* +50 */
		{ .tid = 5, .cpu = 1, .ticks = 120, .start = 50 }, /* +20 */
		/* 9 exited; 9 reused by a thread started before -- ignored */
		{ .tid = 12, .cpu = 1, .ticks = 10, .start = 1050 }, /* new */
		{ .tid = 13, .cpu = 0, .ticks = 400, .start = 1010 }, /* capped */
		{ .tid = 14, .cpu = 7, .ticks = 0, .start = 1020 }, /* no cpu */
	};
	double per[2], total;

	tv_cpu_usage(prev, 3, cur, 5, 1000, 100, 2, per, &total);
	g_assert_cmpfloat_with_epsilon(per[0], 100.0, 1e-9); /* 50+100 */
	g_assert_cmpfloat_with_epsilon(per[1], 30.0, 1e-9); /* 20+10 */
	g_assert_cmpfloat_with_epsilon(total, 90.0, 1e-9); /* 180/200 */

	/* A tid that came back with another start time is another thread:
	 * new since the last listing, all of its time counts. */
	{
		const TvTaskTicks p2[] = { { 4, 0, 500, 10 } };
		const TvTaskTicks c2[] = { { 4, 0, 30, 1005 } };
		tv_cpu_usage(p2, 1, c2, 1, 1000, 100, 1, per, &total);
		g_assert_cmpfloat_with_epsilon(per[0], 30.0, 1e-9);
	}
	/* No interval: zeros. */
	tv_cpu_usage(prev, 3, cur, 5, 1000, 0, 2, per, &total);
	g_assert_cmpfloat(total, ==, 0.0);
	g_assert_cmpfloat(per[1], ==, 0.0);
}

static void test_diskstats(void)
{
	static const char a_text[] =
		" 259       0 nvme0n1 100 0 2000 50 10 0 800 30 0 70 80 0 0 0 0 0 0\n"
		" 259       1 nvme0n1p1 5 0 40 1 0 0 0 0 0 1 1 0 0 0 0 0 0\n"
		"   8       0 sda 1 2 3 4 5 6 7 8 9 10 11\n"
		"garbage\n";
	static const char b_text[] =
		" 259       0 nvme0n1 150 0 4048 150 30 0 1824 130 1 570 900 0 0 0 0 0 0\n";
	TvDiskStat a[8], b[8];
	TvDisk d;
	int n = tv_parse_diskstats(a_text, a, 8);

	g_assert_cmpint(n, ==, 3);
	g_assert_cmpstr(a[0].name, ==, "nvme0n1");
	g_assert_cmpstr(a[1].name, ==, "nvme0n1p1");
	g_assert_cmpstr(a[2].name, ==, "sda");
	g_assert_cmpuint(a[0].rd_ios, ==, 100);
	g_assert_cmpuint(a[0].rd_sectors, ==, 2000);
	g_assert_cmpuint(a[0].wr_ms, ==, 30);
	g_assert_cmpuint(a[0].io_ms, ==, 70);
	g_assert_cmpuint(a[2].io_ms, ==, 10);
	g_assert_cmpint(tv_parse_diskstats(a_text, a, 1), ==, 1);

	g_assert_cmpint(tv_parse_diskstats(b_text, b, 8), ==, 1);
	tv_parse_diskstats(a_text, a, 8);
	/* 2 seconds: busy 500 ms of 2000 ms, 2048 sectors read, 1024
	 * written, 70 requests in 200 ms. */
	tv_disk_rates(&a[0], &b[0], 2.0, &d);
	g_assert_cmpfloat_with_epsilon(d.active, 25.0, 1e-9);
	g_assert_cmpfloat_with_epsilon(d.read_bps, 2048.0 * 512 / 2, 1e-9);
	g_assert_cmpfloat_with_epsilon(d.write_bps, 1024.0 * 512 / 2, 1e-9);
	g_assert_cmpfloat_with_epsilon(d.response_ms, 200.0 / 70.0, 1e-9);
	/* Counters going backwards (a disk replaced): no rates. */
	tv_disk_rates(&b[0], &a[0], 2.0, &d);
	g_assert_cmpfloat(d.active, ==, 0.0);
	g_assert_cmpfloat(d.read_bps, ==, 0.0);
	g_assert_cmpfloat(d.response_ms, ==, 0.0);
}

static void test_mounts(void)
{
	char out[64];

	g_assert_true(tv_parse_root_source(
		"rootfs / rootfs rw 0 0\n/dev/nvme0n1p2 / ext4 rw 0 0\n"
		"proc /proc proc rw 0 0\n",
		out, sizeof(out)));
	g_assert_cmpstr(out, ==, "/dev/nvme0n1p2");
	g_assert_false(tv_parse_root_source("proc /proc proc rw 0 0\n", out,
					    sizeof(out)));
	g_assert_true(tv_disk_of_link("../../block/sda/sda2", out, sizeof(out)));
	g_assert_cmpstr(out, ==, "sda");
	g_assert_true(tv_disk_of_link("../../block/nvme0n1", out, sizeof(out)));
	g_assert_cmpstr(out, ==, "nvme0n1");
	g_assert_false(tv_disk_of_link("../../devices/x", out, sizeof(out)));
}

static void test_format(void)
{
	char *s;

	s = tv_format_bytes(512);
	g_assert_cmpstr(s, ==, "512 bytes");
	g_free(s);
	s = tv_format_bytes(3ULL * 1024 * 1024 * 1024 / 2);
	g_assert_cmpstr(s, ==, "1.5 GB");
	g_free(s);
	s = tv_format_bit_rate(8000);
	g_assert_cmpstr(s, ==, "8.0 Kbps");
	g_free(s);
	s = tv_format_bit_rate(0);
	g_assert_cmpstr(s, ==, "0 Kbps");
	g_free(s);
	s = tv_format_bit_rate(2.5e6);
	g_assert_cmpstr(s, ==, "2.5 Mbps");
	g_free(s);
	s = tv_format_byte_rate(3 * 1024 * 1024);
	g_assert_cmpstr(s, ==, "3.0 MB/s");
	g_free(s);
	s = tv_format_mhz(2700);
	g_assert_cmpstr(s, ==, "2.70 GHz");
	g_free(s);
	s = tv_format_uptime(90061);
	g_assert_cmpstr(s, ==, "1:01:01:01");
	g_free(s);
}

static const char inittab[] =
	"# /etc/inittab - programs started and supervised by init.\n"
	"#\n"
	"# keep init's terminal instead:\n"
	"#\n"
	"#\trespawn console /usr/sbin/sshd -D\n"
	"#\n"
	"# Failing to start is reported.\n"
	"\n"
	"# Login prompt on the system console.  respawn is what puts a\n"
	"# fresh prompt up.\n"
	"respawn\t/sbin/getty /dev/console\n"
	"\n"
	"# SSH host keys: makes the ones missing.\n"
	"once\t/usr/bin/ssh-keygen -A\n"
	"\n"
	"# Secure shell daemon.  -D keeps it in the foreground.\n"
	"# Comment this line out to stop starting sshd at boot.\n"
	"# respawn\t/usr/sbin/sshd -D\n"
	"\n"
	"respawn\t/usr/sbin/audiod -f   # trailing comment\n"
	"wait console /bin/echo hello\n"
	"respawn\n"
	"/usr/sbin/devd -f\n";

static void test_inittab(void)
{
	GPtrArray *l = tv_parse_inittab(inittab);
	TvInitEntry *e;

	g_assert_cmpuint(l->len, ==, 6);

	e = l->pdata[0];
	g_assert_cmpstr(e->name, ==, "getty");
	g_assert_cmpstr(e->program, ==, "/sbin/getty");
	g_assert_cmpstr(e->command, ==, "/sbin/getty /dev/console");
	g_assert_cmpstr(e->description, ==, "Login prompt on the system console");
	g_assert_cmpint(e->action, ==, TV_INIT_RESPAWN);
	g_assert_false(e->disabled);
	g_assert_cmpint(e->line, ==, 11);

	e = l->pdata[1];
	g_assert_cmpstr(e->name, ==, "ssh-keygen");
	g_assert_cmpint(e->action, ==, TV_INIT_ONCE);
	g_assert_cmpstr(e->description, ==, "SSH host keys");

	e = l->pdata[2];
	g_assert_cmpstr(e->name, ==, "sshd");
	g_assert_true(e->disabled);
	g_assert_false(e->console);
	g_assert_cmpstr(e->command, ==, "/usr/sbin/sshd -D");
	g_assert_cmpstr(e->description, ==, "Secure shell daemon");

	e = l->pdata[3];
	g_assert_cmpstr(e->name, ==, "audiod");
	g_assert_cmpstr(e->command, ==, "/usr/sbin/audiod -f");
	g_assert_null(e->description);

	e = l->pdata[4];
	g_assert_cmpstr(e->name, ==, "echo");
	g_assert_cmpint(e->action, ==, TV_INIT_WAIT);
	g_assert_true(e->console);

	/* A bare "respawn" starts nothing; a bare command respawns. */
	e = l->pdata[5];
	g_assert_cmpstr(e->name, ==, "devd");
	g_assert_cmpint(e->action, ==, TV_INIT_RESPAWN);
	g_ptr_array_unref(l);

	/* A switched-off entry whose program runs anyway is not listed. */
	l = tv_parse_inittab("# off\n# respawn /usr/sbin/sshd\n\n"
			     "respawn /usr/sbin/sshd -D -p 2222\n");
	g_assert_cmpuint(l->len, ==, 1);
	g_assert_false(((TvInitEntry *)l->pdata[0])->disabled);
	g_ptr_array_unref(l);

	l = tv_parse_inittab("");
	g_assert_cmpuint(l->len, ==, 0);
	g_ptr_array_unref(l);
	g_assert_cmpstr(tv_init_action_name(TV_INIT_WAIT), ==, "wait");
}

static const char xinitrc[] =
	"#!/bin/sh\n"
	"# Default X session.\n"
	"if [ -r /etc/X11/Xresources ]; then\n"
	"\tXENVIRONMENT=/etc/X11/Xresources\n"
	"\texport XENVIRONMENT\n"
	"fi\n"
	"LIKEOS_GPU_DRIVER=$(drminfo --driver /dev/dri/card0 2>/dev/null)\n"
	"if [ -e /dev/dri/card0 ] && drminfo --has-display /dev/dri/card0 &&\n"
	"\tdrminfo --has-3d /dev/dri/card0 && \n"
	"\t[ -r /dev/dri/renderD128 ] |\n"
	"\tcat; then\n"
	"\tLIKEOS_GPU_3D=1\n"
	"fi\n"
	"# Keyboard layout: the one chosen in Settings.\n"
	"if [ -x /usr/bin/likeos-xkb-apply ]; then\n"
	"\tlikeos-xkb-apply\n"
	"fi\n"
	"if [ -x /usr/bin/dbus-launch ] && [ -z \"$X\" ]; then\n"
	"\teval \"$(dbus-launch --sh-syntax --exit-with-session)\"\n"
	"fi\n"
	"\n"
	"xsetroot -solid \"#000000\" \\\n"
	"\t-cursor_name left_ptr\n"
	"# USB drives: likeos-volume-agent asks the device manager.\n"
	"if [ -x /usr/bin/likeos-volume-agent ]; then\n"
	"\tlikeos-volume-agent &\n"
	"fi\n"
	"# The desktop bar.\n"
	"#\n"
	"# taskbar(1) is this system's own program.\n"
	"if [ -x /usr/bin/taskbar ]; then\n"
	"\ttaskbar&\n"
	"fi\n"
	"taskbar &\n"
	"pcmanfm --desktop & # the desktop\n"
	"# The window manager runs in the foreground.\n"
	"ctwm &\n"
	"ctwm_pid=$!\n"
	"echo \"$ctwm_pid\" > \"$HOME/.ctwm.pid\"\n"
	"while kill -0 \"$ctwm_pid\" 2>/dev/null; do\n"
	"\twait \"$ctwm_pid\"\n"
	"done\n"
	"rm -f \"$HOME/.ctwm.pid\"\n";

static void test_session(void)
{
	GPtrArray *l = tv_parse_session_script(xinitrc);
	TvSessionEntry *e;

	g_assert_cmpuint(l->len, ==, 6);

	e = l->pdata[0];
	g_assert_cmpstr(e->name, ==, "likeos-xkb-apply");
	g_assert_false(e->background);
	g_assert_cmpstr(e->description, ==, "Keyboard layout");

	e = l->pdata[1];
	g_assert_cmpstr(e->name, ==, "xsetroot");
	g_assert_cmpstr(e->command, ==,
			"xsetroot -solid \"#000000\" -cursor_name left_ptr");
	g_assert_false(e->background);
	g_assert_null(e->description);

	e = l->pdata[2];
	g_assert_cmpstr(e->name, ==, "likeos-volume-agent");
	g_assert_true(e->background);
	g_assert_cmpstr(e->description, ==, "USB drives");

	e = l->pdata[3];
	g_assert_cmpstr(e->name, ==, "taskbar");
	g_assert_cmpstr(e->command, ==, "taskbar");
	g_assert_true(e->background);
	g_assert_cmpstr(e->description, ==, "The desktop bar");

	e = l->pdata[4];
	g_assert_cmpstr(e->name, ==, "pcmanfm");
	g_assert_cmpstr(e->command, ==, "pcmanfm --desktop");
	g_assert_true(e->background);

	e = l->pdata[5];
	g_assert_cmpstr(e->name, ==, "ctwm");
	g_assert_true(e->session);
	g_assert_false(e->background);
	g_assert_cmpstr(e->description, ==,
			"The window manager runs in the foreground");
	g_ptr_array_unref(l);

	l = tv_parse_session_script("exec /usr/bin/ctwm -W\n");
	g_assert_cmpuint(l->len, ==, 1);
	e = l->pdata[0];
	g_assert_cmpstr(e->name, ==, "ctwm");
	g_assert_cmpstr(e->command, ==, "/usr/bin/ctwm -W");
	g_assert_true(e->session);
	g_ptr_array_unref(l);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/sysinfo/meminfo", test_meminfo);
	g_test_add_func("/sysinfo/cpuinfo", test_cpuinfo);
	g_test_add_func("/sysinfo/brand", test_brand);
	g_test_add_func("/sysinfo/topology", test_topology);
	g_test_add_func("/sysinfo/cpu-usage", test_cpu_usage);
	g_test_add_func("/sysinfo/diskstats", test_diskstats);
	g_test_add_func("/sysinfo/mounts", test_mounts);
	g_test_add_func("/sysinfo/format", test_format);
	g_test_add_func("/sysinfo/inittab", test_inittab);
	g_test_add_func("/sysinfo/session", test_session);
	return g_test_run();
}
