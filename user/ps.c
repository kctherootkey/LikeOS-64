/*
 * ps - report a snapshot of the current processes
 *
 * Full implementation per the ps(1) manpage.
 * Supports UNIX (-), BSD (no dash), and GNU (--) option styles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/procinfo.h>

#define PS_VERSION "ps (LikeOS procps) 0.1"

/* Limits */
#define MAX_PROCS    4096
#define MAX_COLS     64
#define BUF_SZ       512
#define MAX_SELECT   256

/* Assumed kernel tick frequency (LikeOS always runs at 100 Hz) */
#define HZ 100

/* ======================================================================
 * Column IDs
 * ====================================================================== */
enum {
    COL_PID, COL_PPID, COL_PGID, COL_SID, COL_TGID,
    COL_UID, COL_USER, COL_GID, COL_EUID, COL_EGID,
    COL_COMM, COL_ARGS, COL_FNAME,
    COL_STAT, COL_STATE,
    COL_TTY,
    COL_TIME, COL_ETIME, COL_ETIMES,
    COL_PCPU, COL_C,
    COL_PMEM,
    COL_RSS, COL_VSZ, COL_SZ,
    COL_PRI, COL_NI,
    COL_NLWP, COL_LWP,
    COL_PSR,
    COL_CLS,
    COL_F,
    COL_WCHAN,
    COL_START, COL_LSTART,
    COL_ADDR,
    COL_LABEL,
    COL_PENDING, COL_BLOCKED, COL_IGNORED, COL_CAUGHT,
    COL__COUNT
};

/* ======================================================================
 * Column specification table  (sorted by name for O(n) lookup)
 * ====================================================================== */
typedef struct {
    const char *name;
    const char *hdr;
    int width;
    int right;          /* 1 = right-align */
    int id;
} col_spec_t;

static const col_spec_t col_specs[] = {
    { "%cpu",       "%CPU",     4, 1, COL_PCPU   },
    { "%mem",       "%MEM",     4, 1, COL_PMEM   },
    { "addr",       "ADDR",     4, 1, COL_ADDR   },
    { "args",       "COMMAND", 27, 0, COL_ARGS   },
    { "blocked",    "BLOCKED", 16, 0, COL_BLOCKED },
    { "bsdstart",   "START",    6, 1, COL_START  },
    { "bsdtime",    "TIME",     6, 1, COL_TIME   },
    { "c",          "C",        2, 1, COL_C      },
    { "caught",     "CAUGHT",  16, 0, COL_CAUGHT },
    { "class",      "CLS",      3, 0, COL_CLS   },
    { "cls",        "CLS",      3, 0, COL_CLS   },
    { "cmd",        "CMD",     27, 0, COL_ARGS   },
    { "comm",       "COMMAND", 16, 0, COL_COMM   },
    { "command",    "COMMAND", 27, 0, COL_ARGS   },
    { "cp",         "CP",       3, 1, COL_C      },
    { "cputime",    "TIME",    11, 1, COL_TIME   },
    { "egid",       "EGID",     5, 1, COL_EGID  },
    { "egroup",     "EGROUP",   8, 0, COL_EGID  },
    { "etime",      "ELAPSED", 11, 1, COL_ETIME  },
    { "etimes",     "ELAPSED",  7, 1, COL_ETIMES },
    { "euid",       "EUID",     5, 1, COL_EUID  },
    { "euser",      "EUSER",    8, 0, COL_EUID  },
    { "f",          "F",        1, 1, COL_F      },
    { "flag",       "F",        4, 1, COL_F      },
    { "flags",      "F",        4, 1, COL_F      },
    { "fname",      "COMMAND",  8, 0, COL_FNAME  },
    { "gid",        "GID",      5, 1, COL_GID   },
    { "group",      "GROUP",    8, 0, COL_GID   },
    { "ignored",    "IGNORED", 16, 0, COL_IGNORED},
    { "label",      "LABEL",   25, 0, COL_LABEL  },
    { "lstart",     "STARTED", 24, 0, COL_LSTART },
    { "lwp",        "LWP",      5, 1, COL_LWP   },
    { "ni",         "NI",       3, 1, COL_NI    },
    { "nice",       "NI",       3, 1, COL_NI    },
    { "nlwp",       "NLWP",     4, 1, COL_NLWP  },
    { "opri",       "PRI",      3, 1, COL_PRI   },
    { "pcpu",       "%CPU",     4, 1, COL_PCPU  },
    { "pending",    "PENDING", 16, 0, COL_PENDING},
    { "pgid",       "PGID",     5, 1, COL_PGID  },
    { "pgrp",       "PGRP",     5, 1, COL_PGID  },
    { "pid",        "PID",      5, 1, COL_PID   },
    { "pmem",       "%MEM",     4, 1, COL_PMEM  },
    { "ppid",       "PPID",     5, 1, COL_PPID  },
    { "pri",        "PRI",      3, 1, COL_PRI   },
    { "psr",        "PSR",      3, 1, COL_PSR   },
    { "rgid",       "RGID",     5, 1, COL_GID   },
    { "rgroup",     "RGROUP",   8, 0, COL_GID   },
    { "rss",        "RSS",      6, 1, COL_RSS   },
    { "rssize",     "RSS",      6, 1, COL_RSS   },
    { "rsz",        "RSZ",      6, 1, COL_RSS   },
    { "ruid",       "RUID",     5, 1, COL_UID   },
    { "ruser",      "RUSER",    8, 0, COL_USER  },
    { "s",          "S",        1, 0, COL_STATE  },
    { "sess",       "SESS",     5, 1, COL_SID   },
    { "session",    "SESS",     5, 1, COL_SID   },
    { "sid",        "SID",      5, 1, COL_SID   },
    { "sig",        "PENDING", 16, 0, COL_PENDING},
    { "sigcatch",   "CAUGHT",  16, 0, COL_CAUGHT },
    { "sigignore",  "IGNORED", 16, 0, COL_IGNORED},
    { "sigmask",    "BLOCKED", 16, 0, COL_BLOCKED},
    { "spid",       "SPID",     5, 1, COL_LWP   },
    { "start",      "STARTED",  8, 1, COL_START  },
    { "start_time", "START",    6, 1, COL_START  },
    { "stat",       "STAT",     4, 0, COL_STAT   },
    { "state",      "S",        1, 0, COL_STATE  },
    { "stime",      "STIME",    5, 1, COL_START  },
    { "sz",         "SZ",       6, 1, COL_SZ    },
    { "tgid",       "TGID",     5, 1, COL_TGID  },
    { "thcount",    "THCNT",    5, 1, COL_NLWP  },
    { "tid",        "TID",      5, 1, COL_LWP   },
    { "time",       "TIME",    11, 1, COL_TIME   },
    { "tname",      "TTY",      7, 0, COL_TTY   },
    { "tt",         "TT",       7, 0, COL_TTY   },
    { "tty",        "TT",       7, 0, COL_TTY   },
    { "ucmd",       "CMD",     16, 0, COL_COMM  },
    { "ucomm",      "COMMAND", 16, 0, COL_COMM  },
    { "uid",        "UID",      5, 1, COL_UID   },
    { "uname",      "USER",     8, 0, COL_USER  },
    { "user",       "USER",     8, 0, COL_USER  },
    { "vsize",      "VSZ",      7, 1, COL_VSZ   },
    { "vsz",        "VSZ",      7, 1, COL_VSZ   },
    { "wchan",      "WCHAN",    6, 0, COL_WCHAN  },
    { NULL, NULL, 0, 0, 0 }
};

/* ======================================================================
 * Active column list (the format to print)
 * ====================================================================== */
typedef struct {
    int  id;
    char hdr[64];
    int  width;
    int  right;
} active_col_t;

static active_col_t g_cols[MAX_COLS];
static int g_ncols = 0;

/* ======================================================================
 * Selection storage
 * ====================================================================== */
static int  sel_pids[MAX_SELECT],   n_sel_pids   = 0;
static int  sel_ppids[MAX_SELECT],  n_sel_ppids  = 0;
static int  sel_uids[MAX_SELECT],   n_sel_uids   = 0;
static int  sel_ruids[MAX_SELECT],  n_sel_ruids  = 0;
static int  sel_gids[MAX_SELECT],   n_sel_gids   = 0;
static int  sel_rgids[MAX_SELECT],  n_sel_rgids  = 0;
static int  sel_sids[MAX_SELECT],   n_sel_sids   = 0;
static int  sel_ttys[MAX_SELECT],   n_sel_ttys   = 0;
static char *sel_comms[MAX_SELECT]; static int n_sel_comms = 0;

/* ======================================================================
 * Option flags
 * ====================================================================== */
/* Simple selection */
static int opt_all          = 0;   /* -e / -A  or  BSD a+x            */
static int opt_all_tty      = 0;   /* -a  or  BSD a                   */
static int opt_bsd_x        = 0;   /* BSD x: include no-tty           */
static int opt_running      = 0;   /* r: only running                 */
static int opt_this_tty     = 0;   /* T: only current terminal        */
static int opt_negate       = 0;   /* -N: negate selection             */

/* Format selection */
static int opt_full         = 0;   /* -f                              */
static int opt_extfull      = 0;   /* -F                              */
static int opt_long         = 0;   /* -l                              */
static int opt_bsd_u        = 0;   /* BSD u                           */
static int opt_bsd_j        = 0;   /* BSD j                           */
static int opt_bsd_l        = 0;   /* BSD l                           */
static int opt_bsd_s        = 0;   /* BSD s (signal format)           */
static int opt_bsd_v        = 0;   /* BSD v (virtual memory format)   */
static int opt_custom_fmt   = 0;   /* -o was given                    */

/* Modifiers */
static int opt_forest       = 0;   /* --forest / BSD f / -H           */
static int opt_no_header    = 0;   /* BSD h / --no-headers             */
static int opt_show_header  = 1;   /* --headers (default)              */
static int opt_wide         = 0;   /* -w / BSD w (counter)             */
static int opt_numeric      = 0;   /* BSD n: numeric UID/GID           */
static int opt_cumulative   = 0;   /* BSD S: cumulative time           */
static int opt_true_cmd     = 0;   /* BSD c: true command name only    */
static int opt_show_env     = 0;   /* BSD e: show environment          */

/* Sort keys */
typedef struct { int col_id; int desc; } sort_key_t;
static sort_key_t g_sort_keys[16];
static int g_nsort = 0;

/* Timing */
static time_t   g_now;
static time_t   g_boot;
static uint64_t g_uptime;

/* Terminal width */
static int g_term_width = 80;

/* My own identity */
static int g_my_pid = -1;
static int g_my_sid = -1;
static int g_my_tty = -1;
static int g_my_uid = -1;

/* Forest display depth per output row */
static int g_cur_depth = 0;

/* ======================================================================
 * Utility: case-insensitive string compare
 * ====================================================================== */
static int strcasecmp_simple(const char *a, const char *b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ======================================================================
 * State character mapping
 *   TASK_READY=0 → R,  RUNNING=1 → R,  BLOCKED=2 → S,
 *   STOPPED=3 → T,  ZOMBIE=4 → Z
 * ====================================================================== */
static char state_char(int st) {
    switch (st) {
    case 0: case 1: return 'R';
    case 2:         return 'S';
    case 3:         return 'T';
    case 4:         return 'Z';
    default:        return '?';
    }
}

/* Build STAT string (BSD style): base + modifiers */
static void build_stat(const procinfo_t *p, char *buf, size_t sz) {
    if (sz < 8) return;
    int n = 0;
    buf[n++] = state_char(p->state);
    if (p->pid == p->sid)                  buf[n++] = 's'; /* session leader */
    if (p->nr_threads > 1)                 buf[n++] = 'l'; /* multi-threaded */
    if (p->nice < 0)                       buf[n++] = '<'; /* high priority  */
    else if (p->nice > 0)                  buf[n++] = 'N'; /* low priority   */
    if (p->tty_nr && p->pgid == p->sid)    buf[n++] = '+'; /* foreground     */
    buf[n] = '\0';
}

/* ======================================================================
 * Timing helpers
 * ====================================================================== */
static void init_timing(void) {
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    g_uptime = (uint64_t)mono.tv_sec;
    g_now    = time(NULL);
    g_boot   = g_now - (time_t)g_uptime;
}

/* Format as [[DD-]HH:]MM:SS */
static void fmt_time_hms(uint64_t sec, char *buf, size_t sz) {
    unsigned d = (unsigned)(sec / 86400);
    unsigned h = (unsigned)((sec % 86400) / 3600);
    unsigned m = (unsigned)((sec % 3600) / 60);
    unsigned s = (unsigned)(sec % 60);
    if (d > 0)
        snprintf(buf, sz, "%u-%02u:%02u:%02u", d, h, m, s);
    else
        snprintf(buf, sz, "%02u:%02u:%02u", h, m, s);
}

/* Format elapsed [[DD-]HH:]MM:SS (omit leading zero fields) */
static void fmt_etime(uint64_t sec, char *buf, size_t sz) {
    unsigned d = (unsigned)(sec / 86400);
    unsigned h = (unsigned)((sec % 86400) / 3600);
    unsigned m = (unsigned)((sec % 3600) / 60);
    unsigned s = (unsigned)(sec % 60);
    if (d > 0)
        snprintf(buf, sz, "%u-%02u:%02u:%02u", d, h, m, s);
    else if (h > 0)
        snprintf(buf, sz, "%02u:%02u:%02u", h, m, s);
    else
        snprintf(buf, sz, "%02u:%02u", m, s);
}

static int detect_term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* ======================================================================
 * Column lookup & management
 * ====================================================================== */
static const col_spec_t *find_col(const char *name) {
    for (const col_spec_t *c = col_specs; c->name; c++) {
        if (strcasecmp_simple(name, c->name) == 0)
            return c;
    }
    return NULL;
}

static void add_col(int id, const char *hdr, int width, int right) {
    if (g_ncols >= MAX_COLS) return;
    active_col_t *c = &g_cols[g_ncols++];
    c->id    = id;
    c->width = width;
    c->right = right;
    strncpy(c->hdr, hdr, sizeof(c->hdr) - 1);
    c->hdr[sizeof(c->hdr) - 1] = '\0';
}

static int add_col_by_name(const char *name, const char *custom_hdr) {
    const col_spec_t *sp = find_col(name);
    if (!sp) {
        fprintf(stderr, "ps: unknown format specifier \"%s\"\n", name);
        return -1;
    }
    add_col(sp->id,
            (custom_hdr != NULL) ? custom_hdr : sp->hdr,
            sp->width, sp->right);
    return 0;
}

/* Parse -o format string:  comma/space-separated, name[=header] */
static int parse_format(const char *fmt) {
    char work[1024];
    strncpy(work, fmt, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *p = work;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;

        char *name = p;
        while (*p && *p != ',' && *p != ' ' && *p != '=') p++;

        char *custom_hdr = NULL;
        if (*p == '=') {
            *p++ = '\0';
            custom_hdr = p;
            while (*p && *p != ',') p++;
            if (*p) *p++ = '\0';
            /* empty after '=' → suppress header for this column */
        } else {
            if (*p) *p++ = '\0';
        }

        if (add_col_by_name(name, custom_hdr) < 0)
            return -1;
    }
    return 0;
}

/* ======================================================================
 * Predefined format setups
 * ====================================================================== */
static void fmt_default(void) {
    /* PID TTY TIME CMD */
    add_col(COL_PID,  "PID",   5, 1);
    add_col(COL_TTY,  "TTY",   7, 0);
    add_col(COL_TIME, "TIME", 11, 1);
    add_col(COL_ARGS, "CMD",  27, 0);
}

static void fmt_full(void) {
    /* UID PID PPID C STIME TTY TIME CMD */
    add_col(COL_UID,   "UID",   5, 1);
    add_col(COL_PID,   "PID",   5, 1);
    add_col(COL_PPID,  "PPID",  5, 1);
    add_col(COL_C,     "C",     2, 1);
    add_col(COL_START, "STIME", 5, 1);
    add_col(COL_TTY,   "TTY",   7, 0);
    add_col(COL_TIME,  "TIME", 11, 1);
    add_col(COL_ARGS,  "CMD",  27, 0);
}

static void fmt_extfull(void) {
    /* UID PID PPID C SZ RSS PSR STIME TTY TIME CMD */
    add_col(COL_UID,   "UID",   5, 1);
    add_col(COL_PID,   "PID",   5, 1);
    add_col(COL_PPID,  "PPID",  5, 1);
    add_col(COL_C,     "C",     2, 1);
    add_col(COL_SZ,    "SZ",    6, 1);
    add_col(COL_RSS,   "RSS",   6, 1);
    add_col(COL_PSR,   "PSR",   3, 1);
    add_col(COL_START, "STIME", 5, 1);
    add_col(COL_TTY,   "TTY",   7, 0);
    add_col(COL_TIME,  "TIME", 11, 1);
    add_col(COL_ARGS,  "CMD",  27, 0);
}

static void fmt_long_unix(void) {
    /* F S UID PID PPID C PRI NI ADDR SZ WCHAN TTY TIME CMD */
    add_col(COL_F,     "F",     1, 1);
    add_col(COL_STATE, "S",     1, 0);
    add_col(COL_UID,   "UID",   5, 1);
    add_col(COL_PID,   "PID",   5, 1);
    add_col(COL_PPID,  "PPID",  5, 1);
    add_col(COL_C,     "C",     2, 1);
    add_col(COL_PRI,   "PRI",   3, 1);
    add_col(COL_NI,    "NI",    3, 1);
    add_col(COL_ADDR,  "ADDR",  4, 1);
    add_col(COL_SZ,    "SZ",    6, 1);
    add_col(COL_WCHAN, "WCHAN", 6, 0);
    add_col(COL_TTY,   "TTY",   7, 0);
    add_col(COL_TIME,  "TIME", 11, 1);
    add_col(COL_ARGS,  "CMD",  27, 0);
}

static void fmt_bsd_u(void) {
    /* USER PID %CPU %MEM VSZ RSS TT STAT START TIME COMMAND */
    add_col(COL_USER,  "USER",     8, 0);
    add_col(COL_PID,   "PID",      5, 1);
    add_col(COL_PCPU,  "%CPU",     4, 1);
    add_col(COL_PMEM,  "%MEM",     4, 1);
    add_col(COL_VSZ,   "VSZ",      7, 1);
    add_col(COL_RSS,   "RSS",      6, 1);
    add_col(COL_TTY,   "TT",       7, 0);
    add_col(COL_STAT,  "STAT",     4, 0);
    add_col(COL_START, "START",    5, 1);
    add_col(COL_TIME,  "TIME",     6, 1);
    add_col(COL_ARGS,  "COMMAND", 27, 0);
}

static void fmt_bsd_j(void) {
    /* USER PID PPID PGID SID STAT TT TIME COMMAND */
    add_col(COL_USER,  "USER",     8, 0);
    add_col(COL_PID,   "PID",      5, 1);
    add_col(COL_PPID,  "PPID",     5, 1);
    add_col(COL_PGID,  "PGID",     5, 1);
    add_col(COL_SID,   "SID",      5, 1);
    add_col(COL_STAT,  "STAT",     4, 0);
    add_col(COL_TTY,   "TT",       7, 0);
    add_col(COL_TIME,  "TIME",     6, 1);
    add_col(COL_ARGS,  "COMMAND", 27, 0);
}

static void fmt_bsd_l(void) {
    /* UID PID PPID F CPU PRI NI VSZ RSS WCHAN STAT TT TIME COMMAND */
    add_col(COL_UID,   "UID",      5, 1);
    add_col(COL_PID,   "PID",      5, 1);
    add_col(COL_PPID,  "PPID",     5, 1);
    add_col(COL_F,     "F",        4, 1);
    add_col(COL_PSR,   "CPU",      3, 1);
    add_col(COL_PRI,   "PRI",      3, 1);
    add_col(COL_NI,    "NI",       3, 1);
    add_col(COL_VSZ,   "VSZ",      7, 1);
    add_col(COL_RSS,   "RSS",      6, 1);
    add_col(COL_WCHAN, "WCHAN",    6, 0);
    add_col(COL_STAT,  "STAT",     4, 0);
    add_col(COL_TTY,   "TT",       7, 0);
    add_col(COL_TIME,  "TIME",     6, 1);
    add_col(COL_ARGS,  "COMMAND", 27, 0);
}

static void fmt_bsd_s(void) {
    /* UID PID PENDING BLOCKED IGNORED CAUGHT STAT TT TIME COMMAND */
    add_col(COL_UID,     "UID",      5, 1);
    add_col(COL_PID,     "PID",      5, 1);
    add_col(COL_PENDING, "PENDING", 16, 0);
    add_col(COL_BLOCKED, "BLOCKED", 16, 0);
    add_col(COL_IGNORED, "IGNORED", 16, 0);
    add_col(COL_CAUGHT,  "CAUGHT",  16, 0);
    add_col(COL_STAT,    "STAT",     4, 0);
    add_col(COL_TTY,     "TT",       7, 0);
    add_col(COL_TIME,    "TIME",     6, 1);
    add_col(COL_ARGS,    "COMMAND", 27, 0);
}

static void fmt_bsd_v(void) {
    /* PID STAT TIME SL RE PAGEIN VSZ RSS LIM TSIZ %CPU %MEM COMMAND */
    add_col(COL_PID,   "PID",      5, 1);
    add_col(COL_STAT,  "STAT",     4, 0);
    add_col(COL_TIME,  "TIME",     6, 1);
    add_col(COL_NI,    "SL",       2, 1);
    add_col(COL_NI,    "RE",       2, 1);
    add_col(COL_F,     "PAGEIN",   6, 1);
    add_col(COL_VSZ,   "VSZ",      7, 1);
    add_col(COL_RSS,   "RSS",      6, 1);
    add_col(COL_F,     "LIM",      5, 1);
    add_col(COL_SZ,    "TSIZ",     5, 1);
    add_col(COL_PCPU,  "%CPU",     4, 1);
    add_col(COL_PMEM,  "%MEM",     4, 1);
    add_col(COL_ARGS,  "COMMAND", 27, 0);
}

/* ======================================================================
 * Column value formatter
 *
 * Writes a human-readable value for column `id` into `buf`.
 * ====================================================================== */
static void format_value(const procinfo_t *p, int id, char *buf, size_t sz) {
    switch (id) {
    case COL_PID:   snprintf(buf, sz, "%d", p->pid); break;
    case COL_PPID:  snprintf(buf, sz, "%d", p->ppid); break;
    case COL_PGID:  snprintf(buf, sz, "%d", p->pgid); break;
    case COL_SID:   snprintf(buf, sz, "%d", p->sid); break;
    case COL_TGID:  snprintf(buf, sz, "%d", p->tgid); break;
    case COL_UID:   snprintf(buf, sz, "%d", p->uid); break;
    case COL_GID:   snprintf(buf, sz, "%d", p->gid); break;
    case COL_EUID:  snprintf(buf, sz, "%d", p->euid); break;
    case COL_EGID:  snprintf(buf, sz, "%d", p->egid); break;

    case COL_USER:
        if (opt_numeric || p->uid != 0)
            snprintf(buf, sz, "%d", p->uid);
        else
            snprintf(buf, sz, "root");
        break;

    case COL_COMM: {
        /* COMM = basename only. Kernel threads wrapped in [] */
        char namebuf[280];
        if (p->comm[0]) {
            if (p->is_kernel)
                snprintf(namebuf, sizeof(namebuf), "[%s]", p->comm);
            else
                snprintf(namebuf, sizeof(namebuf), "%s", p->comm);
        } else {
            snprintf(namebuf, sizeof(namebuf), p->is_kernel ? "[kernel]" : "?");
        }
        if (opt_forest && g_cur_depth > 0) {
            char prefix[128] = "";
            for (int i = 0; i < g_cur_depth; i++)
                strcat(prefix, " \\_ ");
            snprintf(buf, sz, "%s%s", prefix, namebuf);
        } else {
            snprintf(buf, sz, "%s", namebuf);
        }
        break;
    }
    case COL_ARGS: {
        /* ARGS/COMMAND = full cmdline if available, else comm. Kernel in [] */
        char namebuf[4096];
        if (p->is_kernel) {
            if (p->comm[0])
                snprintf(namebuf, sizeof(namebuf), "[%s]", p->comm);
            else
                snprintf(namebuf, sizeof(namebuf), "[kernel]");
        } else if (p->cmdline[0]) {
            snprintf(namebuf, sizeof(namebuf), "%s", p->cmdline);
        } else if (p->comm[0]) {
            snprintf(namebuf, sizeof(namebuf), "%s", p->comm);
        } else {
            snprintf(namebuf, sizeof(namebuf), "?");
        }
        /* BSD e: append environment variables after command */
        if (opt_show_env && !p->is_kernel && p->environ[0]) {
            size_t len = strlen(namebuf);
            snprintf(namebuf + len, sizeof(namebuf) - len, " %s", p->environ);
        }
        if (opt_forest && g_cur_depth > 0) {
            char prefix[128] = "";
            for (int i = 0; i < g_cur_depth; i++)
                strcat(prefix, " \\_ ");
            snprintf(buf, sz, "%s%s", prefix, namebuf);
        } else {
            snprintf(buf, sz, "%s", namebuf);
        }
        break;
    }
    case COL_FNAME: {
        char tmp[9];
        strncpy(tmp, p->comm, 8); tmp[8] = '\0';
        snprintf(buf, sz, "%s", tmp[0] ? tmp : "-");
        break;
    }

    case COL_STAT:  build_stat(p, buf, sz); break;
    case COL_STATE: buf[0] = state_char(p->state); buf[1] = '\0'; break;

    case COL_TTY:
        if (p->tty_nr == 1)
            snprintf(buf, sz, "console");
        else if (p->tty_nr > 1)
            snprintf(buf, sz, "pts/%d", p->tty_nr - 2);
        else
            snprintf(buf, sz, "?");
        break;

    case COL_TIME: {
        uint64_t s = (p->utime_ticks + p->stime_ticks) / HZ;
        fmt_time_hms(s, buf, sz);
        break;
    }
    case COL_ETIME: {
        uint64_t el = (g_uptime > p->start_tick / HZ)
                    ? g_uptime - p->start_tick / HZ : 0;
        fmt_etime(el, buf, sz);
        break;
    }
    case COL_ETIMES: {
        uint64_t el = (g_uptime > p->start_tick / HZ)
                    ? g_uptime - p->start_tick / HZ : 0;
        snprintf(buf, sz, "%lu", (unsigned long)el);
        break;
    }

    case COL_PCPU: {
        uint64_t el = (g_uptime > p->start_tick / HZ)
                    ? g_uptime - p->start_tick / HZ : 0;
        if (el > 0) {
            uint64_t cs = (p->utime_ticks + p->stime_ticks) * 100 / HZ;
            unsigned pct10 = (unsigned)(cs * 10 / el);
            snprintf(buf, sz, "%u.%u", pct10 / 10, pct10 % 10);
        } else {
            snprintf(buf, sz, "0.0");
        }
        break;
    }
    case COL_C: {
        uint64_t el = (g_uptime > p->start_tick / HZ)
                    ? g_uptime - p->start_tick / HZ : 0;
        if (el > 0) {
            uint64_t c = (p->utime_ticks + p->stime_ticks) * 99 / (HZ * el);
            if (c > 99) c = 99;
            snprintf(buf, sz, "%lu", (unsigned long)c);
        } else {
            snprintf(buf, sz, "0");
        }
        break;
    }

    case COL_PMEM:  snprintf(buf, sz, "0.0"); break; /* no total-RAM info */

    case COL_RSS:   snprintf(buf, sz, "%lu", (unsigned long)(p->rss * 4)); break;
    case COL_VSZ:   snprintf(buf, sz, "%lu", (unsigned long)(p->vsz / 1024)); break;
    case COL_SZ:    snprintf(buf, sz, "%lu", (unsigned long)(p->vsz / 4096)); break;

    case COL_PRI:   snprintf(buf, sz, "%d", 80 - p->nice); break;
    case COL_NI:    snprintf(buf, sz, "%d", p->nice); break;

    case COL_NLWP:  snprintf(buf, sz, "%d", p->nr_threads); break;
    case COL_LWP:   snprintf(buf, sz, "%d", p->pid); break;
    case COL_PSR:   snprintf(buf, sz, "%d", p->on_cpu); break;
    case COL_CLS:   snprintf(buf, sz, "RR"); break;

    case COL_F:     snprintf(buf, sz, "%d", p->is_kernel ? 1 : 0); break;
    case COL_WCHAN: snprintf(buf, sz, "-"); break;
    case COL_ADDR:  snprintf(buf, sz, "-"); break;
    case COL_LABEL: snprintf(buf, sz, "-"); break;

    /* Signal masks (not tracked in procinfo – show zeros) */
    case COL_PENDING: case COL_BLOCKED:
    case COL_IGNORED: case COL_CAUGHT:
        snprintf(buf, sz, "0000000000000000");
        break;

    case COL_START: {
        time_t st = g_boot + (time_t)(p->start_tick / HZ);
        struct tm tm;
        localtime_r(&st, &tm);
        struct tm now_tm;
        localtime_r(&g_now, &now_tm);
        if (tm.tm_year == now_tm.tm_year && tm.tm_yday == now_tm.tm_yday)
            strftime(buf, sz, "%H:%M", &tm);
        else
            strftime(buf, sz, "%b%d", &tm);
        break;
    }
    case COL_LSTART: {
        time_t st = g_boot + (time_t)(p->start_tick / HZ);
        struct tm tm;
        localtime_r(&st, &tm);
        strftime(buf, sz, "%a %b %d %H:%M:%S %Y", &tm);
        break;
    }

    default:
        snprintf(buf, sz, "-");
        break;
    }
}

/* ======================================================================
 * Selection / filtering
 * ====================================================================== */
static void parse_int_list(const char *arg, int *list, int *cnt) {
    char work[512];
    strncpy(work, arg, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    char *p = work;
    while (*p && *cnt < MAX_SELECT) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        list[(*cnt)++] = atoi(p);
        while (*p && *p != ',' && *p != ' ') p++;
    }
}

static void parse_comm_list(const char *arg) {
    char work[512];
    strncpy(work, arg, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    char *p = work;
    while (*p && n_sel_comms < MAX_SELECT) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        if (*p) *p++ = '\0';
        sel_comms[n_sel_comms++] = strdup(start);
    }
}

static int list_contains(const int *list, int cnt, int val) {
    for (int i = 0; i < cnt; i++)
        if (list[i] == val) return 1;
    return 0;
}

static int comm_matches(const procinfo_t *p) {
    for (int i = 0; i < n_sel_comms; i++)
        if (strcmp(p->comm, sel_comms[i]) == 0) return 1;
    return 0;
}

static int is_selected(const procinfo_t *p) {
    int has_list = n_sel_pids || n_sel_ppids || n_sel_comms
                || n_sel_uids || n_sel_ruids || n_sel_gids || n_sel_rgids
                || n_sel_sids || n_sel_ttys;
    int has_simple = opt_all || opt_all_tty || opt_running || opt_this_tty;

    /* ---- No selection options → default ---- */
    if (!has_list && !has_simple && !opt_bsd_x) {
        /* Standard POSIX default: processes with same controlling tty
         * and same session as the caller.  If the caller itself has
         * no tty (g_my_tty <= 0), fall back to same-session match
         * so that bare "ps" always shows at least itself.  */
        int ok;
        if (g_my_tty > 0)
            ok = (p->tty_nr == g_my_tty && p->sid == g_my_sid && !p->is_kernel);
        else
            ok = (p->sid == g_my_sid && !p->is_kernel);
        return opt_negate ? !ok : ok;
    }

    int match = 0;

    /* Simple selections */
    if (opt_all)                                              match = 1;
    if (opt_all_tty && p->tty_nr > 0 && p->pid != p->sid)    match = 1;
    if (opt_bsd_x && !opt_all_tty && !opt_all)                match = 1;
    if (opt_all_tty && opt_bsd_x)                             match = 1;
    if (opt_running && (p->state == 0 || p->state == 1))      match = 1;
    if (opt_this_tty && p->tty_nr == g_my_tty && g_my_tty > 0) match = 1;

    /* List selections (OR among all lists) */
    if (n_sel_pids  && list_contains(sel_pids,  n_sel_pids,  p->pid))   match = 1;
    if (n_sel_ppids && list_contains(sel_ppids, n_sel_ppids, p->ppid))  match = 1;
    if (n_sel_comms && comm_matches(p))                                  match = 1;
    if (n_sel_uids  && list_contains(sel_uids,  n_sel_uids,  p->euid)) match = 1;
    if (n_sel_ruids && list_contains(sel_ruids, n_sel_ruids, p->uid))   match = 1;
    if (n_sel_gids  && list_contains(sel_gids,  n_sel_gids,  p->egid)) match = 1;
    if (n_sel_rgids && list_contains(sel_rgids, n_sel_rgids, p->gid))   match = 1;
    if (n_sel_sids  && list_contains(sel_sids,  n_sel_sids,  p->sid))   match = 1;
    if (n_sel_ttys  && list_contains(sel_ttys,  n_sel_ttys,  p->tty_nr)) match = 1;

    return opt_negate ? !match : match;
}

/* ======================================================================
 * Sorting
 * ====================================================================== */
static void parse_sort_keys(const char *spec) {
    char work[256];
    strncpy(work, spec, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *p = work;
    while (*p && g_nsort < 16) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int desc = 0;
        if (*p == '-') { desc = 1; p++; }
        else if (*p == '+') p++;
        char *name = p;
        while (*p && *p != ',' && *p != ' ') p++;
        if (*p) *p++ = '\0';
        const col_spec_t *cs = find_col(name);
        if (!cs) {
            fprintf(stderr, "ps: unknown sort key \"%s\"\n", name);
            continue;
        }
        g_sort_keys[g_nsort].col_id = cs->id;
        g_sort_keys[g_nsort].desc   = desc;
        g_nsort++;
    }
}

static int cmp_field(const procinfo_t *a, const procinfo_t *b, int id) {
    switch (id) {
    case COL_PID:   return a->pid   - b->pid;
    case COL_PPID:  return a->ppid  - b->ppid;
    case COL_PGID:  return a->pgid  - b->pgid;
    case COL_SID:   return a->sid   - b->sid;
    case COL_TGID:  return a->tgid  - b->tgid;
    case COL_UID: case COL_USER: return a->uid - b->uid;
    case COL_GID:   return a->gid   - b->gid;
    case COL_EUID:  return a->euid  - b->euid;
    case COL_EGID:  return a->egid  - b->egid;
    case COL_COMM: case COL_ARGS: case COL_FNAME:
        return strcmp(a->comm, b->comm);
    case COL_STATE: case COL_STAT: return a->state - b->state;
    case COL_TTY:   return a->tty_nr - b->tty_nr;
    case COL_TIME: {
        int64_t ta = (int64_t)(a->utime_ticks + a->stime_ticks);
        int64_t tb = (int64_t)(b->utime_ticks + b->stime_ticks);
        return (ta > tb) ? 1 : (ta < tb) ? -1 : 0;
    }
    case COL_PCPU: case COL_C: {
        uint64_t ea = g_uptime - a->start_tick / HZ;
        uint64_t eb = g_uptime - b->start_tick / HZ;
        uint64_t ca = ea ? (a->utime_ticks + a->stime_ticks) * 100 / (HZ * ea) : 0;
        uint64_t cb = eb ? (b->utime_ticks + b->stime_ticks) * 100 / (HZ * eb) : 0;
        return (ca > cb) ? 1 : (ca < cb) ? -1 : 0;
    }
    case COL_RSS: {
        int64_t d = (int64_t)a->rss - (int64_t)b->rss;
        return (d > 0) ? 1 : (d < 0) ? -1 : 0;
    }
    case COL_VSZ: case COL_SZ: {
        int64_t d = (int64_t)a->vsz - (int64_t)b->vsz;
        return (d > 0) ? 1 : (d < 0) ? -1 : 0;
    }
    case COL_NI:    return a->nice - b->nice;
    case COL_PRI:   return b->nice - a->nice;
    case COL_NLWP:  return a->nr_threads - b->nr_threads;
    case COL_PSR:   return a->on_cpu - b->on_cpu;
    case COL_START: case COL_LSTART: {
        int64_t d = (int64_t)a->start_tick - (int64_t)b->start_tick;
        return (d > 0) ? 1 : (d < 0) ? -1 : 0;
    }
    default: return 0;
    }
}

static int compare_procs(const void *va, const void *vb) {
    const procinfo_t *a = (const procinfo_t *)va;
    const procinfo_t *b = (const procinfo_t *)vb;
    for (int k = 0; k < g_nsort; k++) {
        int r = cmp_field(a, b, g_sort_keys[k].col_id);
        if (g_sort_keys[k].desc) r = -r;
        if (r) return r;
    }
    /* Default tiebreaker: PID ascending */
    return a->pid - b->pid;
}

/* ======================================================================
 * Forest (tree) display
 * ====================================================================== */
typedef struct {
    int idx;     /* index into filtered array */
    int depth;
} tree_entry_t;

static tree_entry_t g_tree[MAX_PROCS];
static int g_tree_n = 0;

static void tree_walk(procinfo_t *procs, int n, int *used, int parent_pid, int depth) {
    for (int i = 0; i < n; i++) {
        if (used[i]) continue;
        if (procs[i].ppid != parent_pid) continue;
        used[i] = 1;
        g_tree[g_tree_n].idx   = i;
        g_tree[g_tree_n].depth = depth;
        g_tree_n++;
        tree_walk(procs, n, used, procs[i].pid, depth + 1);
    }
}

static void build_forest(procinfo_t *procs, int n) {
    int used[MAX_PROCS];
    memset(used, 0, n * sizeof(int));
    g_tree_n = 0;

    /* First pass: roots (parent not in list) */
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < n; j++) {
            if (j != i && procs[j].pid == procs[i].ppid) { found = 1; break; }
        }
        if (!found) {
            used[i] = 1;
            g_tree[g_tree_n].idx   = i;
            g_tree[g_tree_n].depth = 0;
            g_tree_n++;
            tree_walk(procs, n, used, procs[i].pid, 1);
        }
    }
    /* Any orphans (shouldn't happen, but safety) */
    for (int i = 0; i < n; i++) {
        if (!used[i]) {
            g_tree[g_tree_n].idx   = i;
            g_tree[g_tree_n].depth = 0;
            g_tree_n++;
        }
    }
}

/* ======================================================================
 * Output
 * ====================================================================== */
static void print_row(const procinfo_t *p, int is_last_col_unlimited) {
    char val[BUF_SZ];
    int pos = 0;

    for (int c = 0; c < g_ncols; c++) {
        active_col_t *col = &g_cols[c];
        format_value(p, col->id, val, sizeof(val));

        int is_last = (c == g_ncols - 1);
        int w = col->width;
        int vlen = (int)strlen(val);

        /* Widen column to fit value if possible */
        if (vlen > w) w = vlen;

        if (c > 0) { putchar(' '); pos++; }

        if (is_last && is_last_col_unlimited) {
            /* Last column: always print in full (never truncate).
             * The terminal will wrap or clip; truncating mid-word
             * (or worse, mid-bracket "[kernel idle") is confusing. */
            printf("%s", val);
            pos += vlen;
        } else {
            if (col->right)
                pos += printf("%*.*s", w, w, val);
            else
                pos += printf("%-*.*s", w, w, val);
        }
    }
    putchar('\n');
}

static void print_header(void) {
    if (!opt_show_header || opt_no_header) return;

    /* Check if all headers are empty (suppress header line) */
    int all_empty = 1;
    for (int c = 0; c < g_ncols; c++) {
        if (g_cols[c].hdr[0]) { all_empty = 0; break; }
    }
    if (all_empty) return;

    for (int c = 0; c < g_ncols; c++) {
        active_col_t *col = &g_cols[c];
        if (c > 0) putchar(' ');
        int is_last = (c == g_ncols - 1);
        if (is_last)
            printf("%s", col->hdr);
        else if (col->right)
            printf("%*s", col->width, col->hdr);
        else
            printf("%-*s", col->width, col->hdr);
    }
    putchar('\n');
}

/* ======================================================================
 * Help & version
 * ====================================================================== */
static void print_help(void) {
    printf(
"Usage:\n"
"  ps [options]\n"
"\n"
"Simple selection:\n"
"  -A, -e           select all processes\n"
"  -a               select all with tty except session leaders\n"
"  a                (BSD) all with tty, including other users\n"
"  x                (BSD) include processes without tty\n"
"  r                only running processes\n"
"  T                all processes on this terminal\n"
"  -N               negate selection\n"
"\n"
"Selection by list:\n"
"  -p, --pid PID    select by PID\n"
"  --ppid PID       select by parent PID\n"
"  -C CMD           select by command name\n"
"  -u USER          select by effective UID\n"
"  -U USER          select by real UID\n"
"  -g GID           select by session or effective GID\n"
"  -G GID           select by real GID\n"
"  -s SID           select by session ID\n"
"  -t TTY           select by tty number\n"
"\n"
"Output format:\n"
"  -f               full format\n"
"  -F               extra full format\n"
"  -l               long format\n"
"  u                (BSD) user format\n"
"  j                (BSD) jobs format\n"
"  l                (BSD) long format\n"
"  s                (BSD) signal format\n"
"  v                (BSD) virtual memory format\n"
"  -o, --format FMT user-defined format\n"
"  -O FMT           preloaded -o (pid,FMT,state,tty,time,args)\n"
"\n"
"Modifiers:\n"
"  -H, --forest, f  show process tree\n"
"  --sort KEY       sort (prefix - for descending)\n"
"  k KEY            (BSD) sort key\n"
"  -w, w            wide output (repeat for unlimited)\n"
"  --no-headers, h  suppress header\n"
"  --headers        force header\n"
"  n                numeric UID/GID\n"
"  c                true command name\n"
"  S                include child CPU time\n"
"\n"
"  --help           this help\n"
"  --version        version information\n"
"\n"
"Format specifiers for -o:\n"
"  pid ppid pgid sid tgid uid user gid euid egid comm args fname\n"
"  stat state s tty time etime etimes %%cpu pcpu c %%mem pmem\n"
"  rss vsz sz pri ni nlwp lwp psr cls f wchan start lstart addr\n"
"  pending blocked ignored caught label\n"
    );
}

static void print_version(void) {
    printf("%s\n", PS_VERSION);
}

/* ======================================================================
 * BSD option parsing (arguments without leading '-')
 * ====================================================================== */
static int parse_bsd_opts(int argc, char **argv, int *consumed) {
    /* consumed[i] is set to 1 for argv entries handled as BSD options */
    for (int i = 1; i < argc; i++) {
        consumed[i] = 0;
        if (argv[i][0] == '-' || argv[i][0] == '\0') continue;

        /* This arg looks like BSD options (no leading dash) */
        consumed[i] = 1;

        for (const char *p = argv[i]; *p; p++) {
            switch (*p) {
            case 'a': opt_all_tty = 1; break;
            case 'x': opt_bsd_x  = 1; break;
            case 'u': opt_bsd_u  = 1; break;
            case 'j': opt_bsd_j  = 1; break;
            case 'l': opt_bsd_l  = 1; break;
            case 's': opt_bsd_s  = 1; break;
            case 'v': opt_bsd_v  = 1; break;
            case 'f': opt_forest = 1; break;
            case 'e': opt_show_env = 1; break;
            case 'c': opt_true_cmd = 1; break;
            case 'h': opt_no_header = 1; break;
            case 'n': opt_numeric  = 1; break;
            case 'r': opt_running  = 1; break;
            case 'w': opt_wide++; break;
            case 'S': opt_cumulative = 1; break;
            case 'T': opt_this_tty = 1; break;
            case 'k':
                /* Rest of this arg or next arg is the sort key */
                if (*(p + 1)) {
                    parse_sort_keys(p + 1);
                } else if (i + 1 < argc) {
                    consumed[i + 1] = 1;
                    parse_sort_keys(argv[i + 1]);
                    i++;
                }
                goto next_arg;
            default:
                fprintf(stderr, "ps: unknown BSD option '%c'\n", *p);
                return -1;
            }
        }
        next_arg:;
    }
    return 0;
}

/* ======================================================================
 * UNIX/GNU option parsing with getopt_long
 * ====================================================================== */
enum {
    OPT_SORT = 1001,
    OPT_FOREST,
    OPT_NO_HEADERS,
    OPT_HEADERS,
    OPT_HELP,
    OPT_VERSION,
    OPT_PPID,
    OPT_COLS,
    OPT_LINES,
    OPT_PID,
};

static struct option long_opts[] = {
    { "sort",       required_argument, NULL, OPT_SORT       },
    { "forest",     no_argument,       NULL, OPT_FOREST     },
    { "format",     required_argument, NULL, 'o'            },
    { "no-headers", no_argument,       NULL, OPT_NO_HEADERS },
    { "headers",    no_argument,       NULL, OPT_HEADERS    },
    { "help",       no_argument,       NULL, OPT_HELP       },
    { "version",    no_argument,       NULL, OPT_VERSION    },
    { "pid",        required_argument, NULL, OPT_PID        },
    { "ppid",       required_argument, NULL, OPT_PPID       },
    { "cols",       required_argument, NULL, OPT_COLS       },
    { "columns",    required_argument, NULL, OPT_COLS       },
    { "width",      required_argument, NULL, OPT_COLS       },
    { "lines",      required_argument, NULL, OPT_LINES      },
    { "rows",       required_argument, NULL, OPT_LINES      },
    { NULL, 0, NULL, 0 }
};

static int parse_unix_opts(int argc, char **argv) {
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "eAadfFlp:C:G:g:u:U:s:t:o:O:Hwq:Nk:",
                            long_opts, NULL)) != -1)
    {
        switch (c) {
        case 'e': case 'A':
            opt_all = 1; break;
        case 'a':
            opt_all_tty = 1; break;
        case 'd':
            opt_all_tty = 1; break;  /* all except session leaders */
        case 'f':
            opt_full = 1; break;
        case 'F':
            opt_extfull = 1; break;
        case 'l':
            opt_long = 1; break;
        case 'p': case OPT_PID:
            parse_int_list(optarg, sel_pids, &n_sel_pids); break;
        case 'q':
            parse_int_list(optarg, sel_pids, &n_sel_pids); break;
        case 'C':
            parse_comm_list(optarg); break;
        case 'G':
            parse_int_list(optarg, sel_rgids, &n_sel_rgids); break;
        case 'g':
            parse_int_list(optarg, sel_gids, &n_sel_gids); break;
        case 'u':
            parse_int_list(optarg, sel_uids, &n_sel_uids); break;
        case 'U':
            parse_int_list(optarg, sel_ruids, &n_sel_ruids); break;
        case 's':
            parse_int_list(optarg, sel_sids, &n_sel_sids); break;
        case 't':
            parse_int_list(optarg, sel_ttys, &n_sel_ttys); break;
        case 'o':
            opt_custom_fmt = 1;
            if (parse_format(optarg) < 0) return -1;
            break;
        case 'O': {
            /* -O fmt → pid,<fmt>,state,tty,time,args */
            opt_custom_fmt = 1;
            add_col(COL_PID, "PID", 5, 1);
            if (parse_format(optarg) < 0) return -1;
            add_col(COL_STATE, "S",    1, 0);
            add_col(COL_TTY,   "TTY",  7, 0);
            add_col(COL_TIME,  "TIME",11, 1);
            add_col(COL_ARGS,  "CMD", 27, 0);
            break;
        }
        case 'H':
            opt_forest = 1; break;
        case 'w':
            opt_wide++; break;
        case 'N':
            opt_negate = 1; break;
        case 'k':
            parse_sort_keys(optarg); break;
        case OPT_SORT:
            parse_sort_keys(optarg); break;
        case OPT_FOREST:
            opt_forest = 1; break;
        case OPT_NO_HEADERS:
            opt_no_header = 1; opt_show_header = 0; break;
        case OPT_HEADERS:
            opt_show_header = 1; opt_no_header = 0; break;
        case OPT_HELP:
            print_help(); exit(0);
        case OPT_VERSION:
            print_version(); exit(0);
        case OPT_PPID:
            parse_int_list(optarg, sel_ppids, &n_sel_ppids); break;
        case OPT_COLS:
            g_term_width = atoi(optarg);
            if (g_term_width < 10) g_term_width = 10;
            break;
        case OPT_LINES:
            /* ignored – we don't paginate */
            break;
        case '?':
        default:
            fprintf(stderr, "Try 'ps --help' for more information.\n");
            return -1;
        }
    }
    return 0;
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char **argv) {
    /* ---- Initialise globals ---- */
    init_timing();
    g_term_width = detect_term_width();
    g_my_pid = getpid();
    g_my_uid = getuid();

    /* ---- Phase 1: BSD options (args without leading '-') ---- */
    int bsd_consumed[256];
    memset(bsd_consumed, 0, sizeof(bsd_consumed));
    if (parse_bsd_opts(argc, argv, bsd_consumed) < 0)
        return 1;

    /* Build a stripped argv for getopt (remove consumed BSD args) */
    int new_argc = 0;
    char *new_argv[256];
    new_argv[new_argc++] = argv[0];
    for (int i = 1; i < argc && i < 255; i++) {
        if (!bsd_consumed[i])
            new_argv[new_argc++] = argv[i];
    }
    new_argv[new_argc] = NULL;

    /* ---- Phase 2: UNIX / GNU long options ---- */
    if (parse_unix_opts(new_argc, new_argv) < 0)
        return 1;

    /* Convenience: BSD a+x → all */
    if (opt_all_tty && opt_bsd_x)
        opt_all = 1;

    /* ---- Choose format ---- */
    if (!opt_custom_fmt) {
        /* Priority: -F > -f > -l > BSD formats > default */
        if (opt_extfull)      fmt_extfull();
        else if (opt_full && opt_long) { fmt_long_unix(); /* -lf */ }
        else if (opt_full)    fmt_full();
        else if (opt_long)    fmt_long_unix();
        else if (opt_bsd_u)   fmt_bsd_u();
        else if (opt_bsd_j)   fmt_bsd_j();
        else if (opt_bsd_l)   fmt_bsd_l();
        else if (opt_bsd_s)   fmt_bsd_s();
        else if (opt_bsd_v)   fmt_bsd_v();
        else                  fmt_default();
    }

    /* ---- Fetch all processes ---- */
    procinfo_t *all = (procinfo_t *)malloc(MAX_PROCS * sizeof(procinfo_t));
    if (!all) {
        fprintf(stderr, "ps: out of memory\n");
        return 1;
    }

    int total = getprocinfo(all, MAX_PROCS);
    if (total < 0) {
        fprintf(stderr, "ps: getprocinfo failed (errno %d)\n", errno);
        free(all);
        return 1;
    }

    /* Discover my own session & tty */
    for (int i = 0; i < total; i++) {
        if (all[i].pid == g_my_pid) {
            g_my_sid = all[i].sid;
            g_my_tty = all[i].tty_nr;
            break;
        }
    }

    /* ---- Filter ---- */
    procinfo_t *filtered = (procinfo_t *)malloc(MAX_PROCS * sizeof(procinfo_t));
    if (!filtered) { free(all); return 1; }
    int nfilt = 0;

    for (int i = 0; i < total; i++) {
        if (is_selected(&all[i]))
            filtered[nfilt++] = all[i];
    }
    free(all);

    /* ---- Sort ---- */
    if (g_nsort > 0 && !opt_forest)
        qsort(filtered, nfilt, sizeof(procinfo_t), compare_procs);

    /* ---- Forest ---- */
    if (opt_forest) {
        build_forest(filtered, nfilt);
        /* Print in tree order */
        print_header();
        for (int i = 0; i < g_tree_n; i++) {
            g_cur_depth = g_tree[i].depth;
            print_row(&filtered[g_tree[i].idx], 1);
        }
    } else {
        /* ---- Normal output ---- */
        print_header();
        for (int i = 0; i < nfilt; i++) {
            g_cur_depth = 0;
            print_row(&filtered[i], 1);
        }
    }

    free(filtered);
    return 0;
}
