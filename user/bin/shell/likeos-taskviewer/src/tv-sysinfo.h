// LikeOS Task Viewer -- the system sampler.
//
// One TvSysinfo keeps what the Performance page shows: the processor, the
// memory, every disk and every network adapter, each sampled at the update
// tick, with the rates worked out between two samples.  Where the figures
// come from:
//
//   processor    the CPUID instruction (name, caches, topology), /proc/cpuinfo
//                (processor count, clock) and getprocinfo(): the system has
//                no per-processor time counters and lists no idle tasks, so
//                the busy time of a processor is the time charged to the
//                threads that ran on it between two samples
//   memory       /proc/meminfo
//   disks        /proc/diskstats, /sys/block/<disk>/{size,removable,
//                queue/rotational,device/model}, /dev/disk/by-id (USB),
//                the device of "/" and /sys/dev/block (the system disk)
//   network      net_getinfo(NET_GET_IFACE_INFO)
//
// The text parsers below the sampler are plain functions over strings, so
// the tests (tests/test-sysinfo.c) run them on any machine.  So are the
// readers of /etc/inittab and of the X session script that the Services and
// Startup apps pages show.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef TV_SYSINFO_H
#define TV_SYSINFO_H

#include <glib.h>

G_BEGIN_DECLS

/* ---- the processor ------------------------------------------------------ */

typedef struct {
	char model[96]; /* the processor's own name, "" when it gives none */
	char vendor[16];
	double base_mhz; /* nominal clock, 0 when unknown */
	double speed_mhz; /* the clock the system reports, 0 when unknown */
	int sockets;
	int cores;
	int logical;
	gboolean virtual_machine; /* running under a hypervisor */
	guint64 l1_bytes, l2_bytes, l3_bytes; /* totals, 0 when unknown */
} TvCpuInfo;

typedef struct {
	gboolean valid; /* FALSE until two samples were taken */
	double total; /* percent of all processors together */
	int ncpu;
	const double *per_cpu; /* ncpu entries, percent */
	int processes;
	int threads;
	guint64 uptime_s;
} TvCpuLoad;

/* ---- memory ------------------------------------------------------------- */

typedef struct {
	guint64 total; /* bytes */
	guint64 free; /* not used for anything */
	guint64 available; /* what a program can still get */
	guint64 cached; /* file contents kept in memory, given back on demand */
	guint64 buffers;
	guint64 committed; /* promised to programs, valid if have_committed */
	guint64 in_use; /* total - available */
	gboolean have_available, have_cached, have_buffers, have_committed;
} TvMemInfo;

/* ---- disks -------------------------------------------------------------- */

typedef enum {
	TV_DISK_UNKNOWN,
	TV_DISK_SSD,
	TV_DISK_NVME,
	TV_DISK_HDD,
	TV_DISK_USB,
	TV_DISK_REMOVABLE,
	TV_DISK_OPTICAL,
} TvDiskKind;

typedef struct {
	char name[32]; /* "nvme0n1", "sda" */
	char model[96]; /* "" when the device gives none */
	int index; /* "Disk <index>": its place among the disks */
	guint64 size; /* bytes */
	TvDiskKind kind;
	gboolean removable;
	gboolean system; /* the root filesystem is on it */
	double active; /* percent of the interval it was busy */
	double read_bps, write_bps; /* bytes per second */
	double response_ms; /* average time a request took */
} TvDisk;

/* "SSD (NVMe)", "HDD", "USB", ... */
const char *tv_disk_kind_name(TvDiskKind kind);

/* ---- network ------------------------------------------------------------ */

typedef struct {
	char name[16];
	char ipv4[16]; /* "" without an address */
	char mac[18];
	gboolean wireless;
	gboolean up;
	double rx_bps, tx_bps; /* bits per second */
} TvNet;

/* ---- the sampler -------------------------------------------------------- */

typedef struct TvSysinfo TvSysinfo;

TvSysinfo *tv_sysinfo_new(void);
void tv_sysinfo_free(TvSysinfo *s);

/* Takes a sample of everything and works out the rates since the last. */
void tv_sysinfo_sample(TvSysinfo *s);

const TvCpuInfo *tv_sysinfo_cpu_info(TvSysinfo *s);
const TvCpuLoad *tv_sysinfo_cpu(TvSysinfo *s);
const TvMemInfo *tv_sysinfo_mem(TvSysinfo *s);
int tv_sysinfo_n_disks(TvSysinfo *s);
const TvDisk *tv_sysinfo_disk(TvSysinfo *s, int i);
int tv_sysinfo_n_nets(TvSysinfo *s);
const TvNet *tv_sysinfo_net(TvSysinfo *s, int i);

/* ---- running programs, for the Services and Startup apps pages ---------- */

typedef struct {
	int pid;
	int ppid;
	int uid;
	char name[64]; /* the program's name */
} TvProcName;

/* The user processes running now (threads folded away): a GArray of
 * TvProcName.  Empty where the system cannot list them. */
GArray *tv_proc_snapshot(void);

/* The pid of a process named `name' in a snapshot, of user `uid' (-1:
 * anyone's), a child of init preferred when `init_child'; 0 when none
 * runs. */
int tv_proc_find(GArray *procs, const char *name, int uid,
		 gboolean init_child);

/* A whole file as text (also the system's generated files, whose size
 * says nothing); NULL when unreadable.  g_free(). */
char *tv_read_text(const char *path);

/* ---- formatting --------------------------------------------------------- */

/* "3.4 GB", "512 MB", "12.0 KB" (powers of 1024). */
char *tv_format_bytes(guint64 bytes);
/* "1.2 MB/s", "0 KB/s". */
char *tv_format_byte_rate(double bps);
/* "8.0 Kbps", "1.2 Mbps", "0 Kbps". */
char *tv_format_bit_rate(double bps);
/* "2.70 GHz" or "800 MHz". */
char *tv_format_mhz(double mhz);
/* "1:02:03:04" (days:hours:minutes:seconds). */
char *tv_format_uptime(guint64 seconds);

/* ---- parsers (no system access; tested in tests/test-sysinfo.c) --------- */

gboolean tv_parse_meminfo(const char *text, TvMemInfo *out);

/* The number of "processor" entries; *mhz the mean of the "cpu MHz" lines
 * (0 when there are none). */
int tv_parse_cpuinfo(const char *text, double *mhz);

/* The first number of /proc/uptime, in seconds; < 0 when unreadable. */
double tv_parse_uptime(const char *text);

/* The clock in a processor name ("... @ 2.70GHz"), 0 when it names none. */
double tv_parse_brand_mhz(const char *brand);

/* Collapses runs of blanks and trims both ends, in place. */
void tv_squeeze_spaces(char *s);

/* Sockets and cores from the logical processor count and what CPUID says of
 * one package: `threads_per_core' and `logical_per_package', either 0 when
 * unknown. */
void tv_cpu_topology(int logical, int threads_per_core,
		     int logical_per_package, int *sockets, int *cores);

typedef struct {
	int tid;
	int cpu; /* the processor it is on or last ran on */
	guint64 ticks; /* user + system time, clock ticks */
	guint64 start; /* start time, clock ticks since boot */
} TvTaskTicks;

/* The busy share of every processor between two task listings, each sorted
 * by tid: `prev_time' is when the first was taken and `elapsed' the time
 * between the two, both in clock ticks.  per_cpu (ncpu entries) and *total
 * in percent. */
void tv_cpu_usage(const TvTaskTicks *prev, int nprev, const TvTaskTicks *cur,
		  int ncur, double prev_time, double elapsed, int ncpu,
		  double *per_cpu, double *total);

typedef struct {
	char name[32];
	guint64 rd_ios, rd_sectors, rd_ms;
	guint64 wr_ios, wr_sectors, wr_ms;
	guint64 io_ms;
} TvDiskStat;

/* Fills up to `max' records from /proc/diskstats; returns how many. */
int tv_parse_diskstats(const char *text, TvDiskStat *out, int max);

/* The rates of one disk between two of its records `secs' apart, into the
 * active, read_bps, write_bps and response_ms of *out. */
void tv_disk_rates(const TvDiskStat *a, const TvDiskStat *b, double secs,
		   TvDisk *out);

/* The source of the filesystem mounted at "/" in a /proc/mounts text. */
gboolean tv_parse_root_source(const char *mounts, char *out, gsize size);

/* The disk a /sys/class/block link target names ("../../block/sda/sda2"
 * -> "sda", "../../block/sda" -> "sda"); FALSE when it names none. */
gboolean tv_disk_of_link(const char *target, char *out, gsize size);

/* ---- /etc/inittab ------------------------------------------------------- */

typedef enum {
	TV_INIT_RESPAWN,
	TV_INIT_ONCE,
	TV_INIT_WAIT,
} TvInitAction;

typedef struct {
	TvInitAction action;
	gboolean console; /* keeps init's terminal */
	gboolean disabled; /* commented out */
	char *name; /* the program's basename */
	char *program; /* as written */
	char *command; /* the program and its arguments */
	char *description; /* the comment above it, first phrase; or NULL */
	int line; /* 1-based */
} TvInitEntry;

const char *tv_init_action_name(TvInitAction a);

/* The entries init starts, and those commented out (a line "# respawn
 * /path ..." closing a comment block), in file order.  A GPtrArray of
 * TvInitEntry, freed with g_ptr_array_unref. */
GPtrArray *tv_parse_inittab(const char *text);

/* ---- the X session script ----------------------------------------------- */

typedef struct {
	char *name; /* the program's basename */
	char *command; /* as written, without the trailing '&' */
	char *description; /* the comment above it, first phrase; or NULL */
	gboolean background; /* keeps running beside the session */
	gboolean session; /* the program whose end is the session's end */
	int line;
} TvSessionEntry;

/* The programs a session script (xinitrc) starts, in order.  A GPtrArray of
 * TvSessionEntry, freed with g_ptr_array_unref. */
GPtrArray *tv_parse_session_script(const char *text);

G_END_DECLS

#endif
