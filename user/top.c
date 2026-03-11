/*
 * top - display processes sorted by resource usage
 *
 * Full implementation per the top(1) manpage.
 * Supports batch mode, interactive commands, filtering,
 * sorting, memory/CPU scaling, forest view, and more.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/procinfo.h>
#include <sys/sysinfo.h>

#define TOP_VERSION "top (LikeOS procps) 0.1"

/* ======================================================================
 * Limits and constants
 * ====================================================================== */
#define MAX_PROCS     4096
#define MAX_FIELDS    64
#define MAX_FILTERS   32
#define PAGE_SIZE     4096
#define HZ            100

/* ======================================================================
 * Field IDs (the displayable columns)
 * ====================================================================== */
enum {
    FLD_PID,       /* Process Id */
    FLD_PPID,      /* Parent Process Id */
    FLD_UID,       /* User Id */
    FLD_USER,      /* User Name */
    FLD_RUID,      /* Real User Id */
    FLD_RUSER,     /* Real User Name */
    FLD_GID,       /* Group Id */
    FLD_GROUP,     /* Group Name */
    FLD_PGRP,      /* Process Group Id */
    FLD_SID,       /* Session Id */
    FLD_TGID,      /* Thread Group Id */
    FLD_TTY,       /* Controlling Tty */
    FLD_PR,        /* Priority */
    FLD_NI,        /* Nice Value */
    FLD_NTH,       /* Number of Threads */
    FLD_P,         /* Last used CPU (SMP) */
    FLD_PCPU,      /* %CPU */
    FLD_PMEM,      /* %MEM */
    FLD_VIRT,      /* Virtual Memory Size (KiB) */
    FLD_RES,       /* Resident Memory Size (KiB) */
    FLD_SHR,       /* Shared Memory Size (KiB) */
    FLD_S,         /* Process Status */
    FLD_TIME,      /* CPU Time */
    FLD_TIMEP,     /* CPU Time, hundredths */
    FLD_COMMAND,   /* Command name / Command line */
    FLD_WCHAN,     /* Sleeping in Function */
    FLD_FLAGS,     /* Task Flags */
    FLD_ENVIRON,   /* Environment variables */
    FLD_SWAP,      /* Swapped Size (KiB) */
    FLD_CODE,      /* Code Size (KiB) */
    FLD_DATA,      /* Data + Stack Size (KiB) */
    FLD_USED,      /* Memory in Use (KiB) */
    FLD_ELAPSED,   /* Elapsed Running Time */
    FLD_STARTED,   /* Start Time Interval */
    FLD__COUNT
};

/* ======================================================================
 * Field specification table
 * ====================================================================== */
typedef struct {
    const char *name;
    const char *header;
    int width;
    int right;          /* 1 = right-align */
    int id;
} field_spec_t;

static const field_spec_t field_specs[] = {
    { "PID",      "PID",       7, 1, FLD_PID      },
    { "PPID",     "PPID",      7, 1, FLD_PPID     },
    { "UID",      "UID",       5, 1, FLD_UID      },
    { "USER",     "USER",      8, 0, FLD_USER     },
    { "RUID",     "RUID",      5, 1, FLD_RUID     },
    { "RUSER",    "RUSER",     8, 0, FLD_RUSER    },
    { "GID",      "GID",       5, 1, FLD_GID      },
    { "GROUP",    "GROUP",     8, 0, FLD_GROUP    },
    { "PGRP",     "PGRP",      7, 1, FLD_PGRP     },
    { "SID",      "SID",       7, 1, FLD_SID      },
    { "TGID",     "TGID",      7, 1, FLD_TGID     },
    { "TTY",      "TTY",       7, 0, FLD_TTY      },
    { "PR",       "PR",        3, 1, FLD_PR       },
    { "NI",       "NI",        3, 1, FLD_NI       },
    { "nTH",      "nTH",       4, 1, FLD_NTH      },
    { "P",        "P",         3, 1, FLD_P        },
    { "%CPU",     "%CPU",      5, 1, FLD_PCPU     },
    { "%MEM",     "%MEM",      5, 1, FLD_PMEM     },
    { "VIRT",     "VIRT",      7, 1, FLD_VIRT     },
    { "RES",      "RES",       7, 1, FLD_RES      },
    { "SHR",      "SHR",       7, 1, FLD_SHR      },
    { "S",        "S",         1, 0, FLD_S        },
    { "TIME",     "TIME",      9, 1, FLD_TIME     },
    { "TIME+",    "TIME+",    11, 1, FLD_TIMEP    },
    { "COMMAND",  "COMMAND",  -1, 0, FLD_COMMAND  },  /* variable width */
    { "WCHAN",    "WCHAN",    10, 0, FLD_WCHAN    },
    { "Flags",    "Flags",     8, 1, FLD_FLAGS    },
    { "ENVIRON",  "ENVIRON",  -1, 0, FLD_ENVIRON  },  /* variable width */
    { "SWAP",     "SWAP",      7, 1, FLD_SWAP     },
    { "CODE",     "CODE",      7, 1, FLD_CODE     },
    { "DATA",     "DATA",      7, 1, FLD_DATA     },
    { "USED",     "USED",      7, 1, FLD_USED     },
    { "ELAPSED",  "ELAPSED",  10, 1, FLD_ELAPSED  },
    { "STARTED",  "STARTED",   8, 1, FLD_STARTED  },
    { NULL, NULL, 0, 0, 0 }
};

/* ======================================================================
 * Active field list
 * ====================================================================== */
typedef struct {
    int id;
    char header[32];
    int width;
    int right;
    int visible;
} active_field_t;

static active_field_t g_fields[FLD__COUNT];
static int g_nfields = 0;

/* Default field order (like top's default display) */
static const int default_fields[] = {
    FLD_PID, FLD_USER, FLD_PR, FLD_NI, FLD_VIRT, FLD_RES,
    FLD_SHR, FLD_S, FLD_PCPU, FLD_PMEM, FLD_TIMEP, FLD_COMMAND
};
#define N_DEFAULT_FIELDS (int)(sizeof(default_fields)/sizeof(default_fields[0]))

/* ======================================================================
 * Process snapshot structures
 * ====================================================================== */
typedef struct {
    procinfo_t info;

    /* Calculated values */
    double pcpu;             /* %CPU */
    double pmem;             /* %MEM */
    uint64_t total_ticks;    /* utime + stime (possibly cumulative) */
} proc_entry_t;

/* Two snapshots for delta calculation */
static proc_entry_t *g_procs = NULL;       /* current snapshot */
static proc_entry_t *g_prev_procs = NULL;  /* previous snapshot */
static int g_nprocs = 0;
static int g_prev_nprocs = 0;

/* Raw procinfo buffer (allocated once) */
static procinfo_t *g_raw = NULL;

/* ======================================================================
 * Global options and state
 * ====================================================================== */

/* Command-line options */
static double  opt_delay      = 3.0;    /* -d: seconds between updates */
static int     opt_batch      = 0;      /* -b: batch mode */
static int     opt_cmdline    = 0;      /* -c: command line toggle */
static int     opt_idle       = 1;      /* toggle: show idle processes */
static int     opt_iterations = -1;     /* -n: number of iterations (-1=unlimited) */
static int     opt_secure     = 0;      /* -s: secure mode */
static int     opt_threads    = 0;      /* -H: threads mode */
static int     opt_cumulative = 0;      /* -S: cumulative time mode */
static int     opt_irix       = 1;      /* Irix vs Solaris mode (default Irix) */
static int     opt_width      = 0;      /* -w: output width (0=auto) */
static int     opt_single_cpu = 1;      /* -1: single CPU summary (default on) */

/* Memory scaling: 0=KiB,1=MiB,2=GiB,3=TiB,4=PiB,5=EiB */
static int     opt_mem_summary_scale = 0;  /* E: summary area scale */
static int     opt_mem_task_scale    = 0;  /* e: task area scale */

/* Sort field */
static int     opt_sort_field = FLD_PCPU;  /* default sort by %CPU */
static int     opt_sort_reverse = 1;       /* 1=high-to-low (default), 0=low-to-high */

/* Filter by PID(s) (-p) */
#define MAX_PIDS 20
static int     opt_filter_pids[MAX_PIDS];
static int     opt_n_filter_pids = 0;

/* Filter by user (-u/-U) */
static char    opt_filter_user[256] = "";
static int     opt_filter_user_effective = 0;  /* 1=-u (effective), 0=-U (any) */
static int     opt_filter_user_negate = 0;     /* 1=exclude matching user */
static int     opt_filter_user_active = 0;

/* Other filters (o/O interactive) */
typedef struct {
    char field_name[32];
    char op;          /* '=', '<', '>' */
    char value[128];
    int  case_sensitive;
    int  negate;       /* 1 = exclude */
} filter_t;

static filter_t g_filters[MAX_FILTERS];
static int      g_nfilters = 0;

/* Display toggles */
static int     show_uptime   = 1;  /* l: load/uptime line */
static int     show_task_cpu = 1;  /* t: task/CPU states (0-3 = 4 modes) */
static int     show_memory   = 1;  /* m: memory display (0-3 = 4 modes) */
static int     show_bold     = 1;  /* B: bold enable */
static int     show_colors   = 1;  /* z: color toggle */
static int     show_hlight_x = 0;  /* x: sort column highlight */
static int     show_hlight_y = 0;  /* y: running task highlight */
static int     show_zero     = 1;  /* 0: zero suppress toggle (1=show zeros) */
static int     show_forest   = 0;  /* V: forest view mode */
static int     show_scroll_coords = 0; /* C: scroll coordinates */

/* Scroll position */
static int     g_scroll_y = 0;
static int     g_scroll_x = 0;

/* Max tasks to display (-n interactive, NOT -n cmdline) */
static int     g_max_tasks = 0;  /* 0 = unlimited (screen rows) */

/* Terminal dimensions */
static int     g_term_rows = 24;
static int     g_term_cols = 80;

/* System info */
static unsigned long g_total_mem = 0;

/* Iteration counter */
static int     g_iteration = 0;

/* Terminal state for raw mode */
static struct termios g_orig_termios;
static int g_raw_mode = 0;

/* Signal flag for SIGWINCH */
static volatile int g_winch = 0;

/* ======================================================================
 * ANSI escape sequences
 * ====================================================================== */
#define ESC_CLEAR       "\033[2J"
#define ESC_HOME        "\033[H"
#define ESC_CLEAR_EOL   "\033[K"
#define ESC_BOLD        "\033[1m"
#define ESC_REVERSE     "\033[7m"
#define ESC_RESET       "\033[0m"
#define ESC_CURSOR_HIDE ""
#define ESC_CURSOR_SHOW ""

/* Color codes */
#define CLR_RED     "\033[31m"
#define CLR_GREEN   "\033[32m"
#define CLR_YELLOW  "\033[33m"
#define CLR_BLUE    "\033[34m"
#define CLR_MAGENTA "\033[35m"
#define CLR_CYAN    "\033[36m"
#define CLR_WHITE   "\033[37m"
#define CLR_BG_BLUE "\033[44m"

/* ======================================================================
 * Utility functions
 * ====================================================================== */

static int my_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int my_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = my_tolower((unsigned char)*a);
        int cb = my_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int my_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n && *a && *b; i++, a++, b++) {
        int ca = my_tolower((unsigned char)*a);
        int cb = my_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
    }
    return 0;
}

/* ======================================================================
 * Terminal handling
 * ====================================================================== */

static void detect_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) g_term_rows = ws.ws_row;
        if (ws.ws_col > 0) g_term_cols = ws.ws_col;
    }
    if (opt_width > 0 && opt_width > g_term_cols)
        g_term_cols = opt_width;
}

static void enter_raw_mode(void) {
    if (opt_batch || g_raw_mode) return;
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_raw_mode = 1;
    printf("%s", ESC_CURSOR_HIDE);
    fflush(stdout);
}

static void leave_raw_mode(void) {
    if (!g_raw_mode) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_raw_mode = 0;
    printf("%s%s", ESC_CURSOR_SHOW, ESC_RESET);
    fflush(stdout);
}

static void sigwinch_handler(int sig) {
    (void)sig;
    g_winch = 1;
}

static void sigterm_handler(int sig) {
    (void)sig;
    /* Move cursor to bottom of screen so shell prompt appears below top output */
    char esc[32];
    int len = 0;
    esc[len++] = '\033'; esc[len++] = '[';
    /* Format g_term_rows as decimal */
    int r = g_term_rows;
    char digits[8];
    int nd = 0;
    if (r == 0) digits[nd++] = '0';
    else { while (r > 0) { digits[nd++] = '0' + (r % 10); r /= 10; } }
    for (int i = nd - 1; i >= 0; i--) esc[len++] = digits[i];
    esc[len++] = ';'; esc[len++] = '1'; esc[len++] = 'H';
    esc[len++] = '\n';
    write(STDOUT_FILENO, esc, len);
    /* Restore terminal settings and exit */
    leave_raw_mode();
    _exit(128 + sig);
}

static void cleanup_and_exit(int code) {
    /* Move cursor to bottom of screen so shell prompt appears below top output */
    printf("\033[%d;1H\n", g_term_rows);
    fflush(stdout);
    leave_raw_mode();
    if (g_procs) free(g_procs);
    if (g_prev_procs) free(g_prev_procs);
    if (g_raw) free(g_raw);
    exit(code);
}

/* Blocking read of a single char */
static int read_char_blocking(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
        return c;
    return -1;
}

/* Read a line in raw mode (for prompts). Returns chars read. */
static int read_line_raw(char *buf, int maxlen) {
    int pos = 0;
    printf("%s", ESC_CURSOR_SHOW);
    fflush(stdout);
    while (pos < maxlen - 1) {
        int c = read_char_blocking();
        if (c < 0) break;
        if (c == 27) { /* Escape = cancel */
            buf[0] = '\0';
            printf("%s", ESC_CURSOR_HIDE);
            fflush(stdout);
            return -1;
        }
        if (c == '\n' || c == '\r') break;
        if (c == 127 || c == 8) { /* Backspace */
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            buf[pos++] = (char)c;
            write(STDOUT_FILENO, &c, 1);
        }
    }
    buf[pos] = '\0';
    printf("%s", ESC_CURSOR_HIDE);
    fflush(stdout);
    return pos;
}

/* ======================================================================
 * State character mapping
 *   TASK_READY=0 → R,  RUNNING=1 → R,  BLOCKED=2 → S,
 *   STOPPED=3 → T,  ZOMBIE=4 → Z
 * ====================================================================== */
static char state_char(int st) {
    switch (st) {
    case 0: return 'R';
    case 1: return 'R';
    case 2: return 'S';
    case 3: return 'T';
    case 4: return 'Z';
    default: return '?';
    }
}

/* ======================================================================
 * Field management
 * ====================================================================== */

static const field_spec_t *find_field_spec(int id) {
    for (const field_spec_t *f = field_specs; f->name; f++) {
        if (f->id == id) return f;
    }
    return NULL;
}

static const field_spec_t *find_field_by_name(const char *name) {
    for (const field_spec_t *f = field_specs; f->name; f++) {
        if (my_strcasecmp(name, f->name) == 0) return f;
    }
    return NULL;
}

static void setup_default_fields(void) {
    g_nfields = 0;
    for (int i = 0; i < N_DEFAULT_FIELDS; i++) {
        const field_spec_t *sp = find_field_spec(default_fields[i]);
        if (!sp) continue;
        active_field_t *af = &g_fields[g_nfields++];
        af->id = sp->id;
        strncpy(af->header, sp->header, sizeof(af->header) - 1);
        af->header[sizeof(af->header) - 1] = '\0';
        af->width = sp->width;
        af->right = sp->right;
        af->visible = 1;
    }
}

/* Find active field index by id. Returns -1 if not found. */
static int find_active_field(int id) {
    for (int i = 0; i < g_nfields; i++) {
        if (g_fields[i].id == id) return i;
    }
    return -1;
}

/* ======================================================================
 * Memory scaling
 * ====================================================================== */

static const char *mem_scale_label[] = {
    "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"
};

static double mem_scale_bytes(uint64_t bytes, int scale) {
    double val = (double)bytes;
    switch (scale) {
    case 0: return val / 1024.0;           /* KiB */
    case 1: return val / (1024.0*1024.0);  /* MiB */
    case 2: return val / (1024.0*1024.0*1024.0);        /* GiB */
    case 3: return val / (1024.0*1024.0*1024.0*1024.0);  /* TiB */
    case 4: return val / (1024.0*1024.0*1024.0*1024.0*1024.0); /* PiB */
    case 5: return val / (1024.0*1024.0*1024.0*1024.0*1024.0*1024.0); /* EiB */
    default: return val / 1024.0;
    }
}

/* Scale for task columns with appropriate suffix */
static void format_mem_task(uint64_t bytes, char *buf, size_t bufsz, int scale) {
    double val = mem_scale_bytes(bytes, scale);
    if (val >= 100000.0)
        snprintf(buf, bufsz, "%.0f", val);
    else if (val >= 10000.0)
        snprintf(buf, bufsz, "%.0f", val);
    else if (val >= 1000.0)
        snprintf(buf, bufsz, "%.0f", val);
    else if (val >= 100.0)
        snprintf(buf, bufsz, "%.1f", val);
    else if (val >= 10.0)
        snprintf(buf, bufsz, "%.2f", val);
    else
        snprintf(buf, bufsz, "%.3f", val);
}

/* ======================================================================
 * Time formatting
 * ====================================================================== */

static void format_time(uint64_t total_ticks, char *buf, size_t bufsz) {
    uint64_t total_sec = total_ticks / HZ;
    unsigned min = (unsigned)(total_sec / 60);
    unsigned sec = (unsigned)(total_sec % 60);
    snprintf(buf, bufsz, "%u:%02u", min, sec);
}

static void format_time_plus(uint64_t total_ticks, char *buf, size_t bufsz) {
    uint64_t total_hundredths = total_ticks * 100 / HZ;
    uint64_t total_sec = total_hundredths / 100;
    unsigned hundredths = (unsigned)(total_hundredths % 100);
    unsigned min = (unsigned)(total_sec / 60);
    unsigned sec = (unsigned)(total_sec % 60);
    if (min >= 100)
        snprintf(buf, bufsz, "%u:%02u.%02u", min, sec, hundredths);
    else
        snprintf(buf, bufsz, "%u:%02u.%02u", min, sec, hundredths);
}

static void format_elapsed(uint64_t uptime_ticks, uint64_t start_tick, char *buf, size_t bufsz) {
    uint64_t elapsed_ticks;
    if (uptime_ticks > start_tick)
        elapsed_ticks = uptime_ticks - start_tick;
    else
        elapsed_ticks = 0;
    uint64_t elapsed_sec = elapsed_ticks / HZ;
    unsigned h = (unsigned)(elapsed_sec / 3600);
    unsigned m = (unsigned)((elapsed_sec % 3600) / 60);
    if (elapsed_sec >= 86400) {
        unsigned d = (unsigned)(elapsed_sec / 86400);
        h = (unsigned)((elapsed_sec % 86400) / 3600);
        snprintf(buf, bufsz, "%u+%02u", d, h);
    } else {
        snprintf(buf, bufsz, "%02u,%02u", h, m);
    }
}

static void format_started(uint64_t start_tick, char *buf, size_t bufsz) {
    uint64_t sec = start_tick / HZ;
    unsigned m = (unsigned)(sec / 60);
    unsigned s = (unsigned)(sec % 60);
    if (sec >= 3600) {
        unsigned h = (unsigned)(sec / 3600);
        m = (unsigned)((sec % 3600) / 60);
        snprintf(buf, bufsz, "%02u,%02u", h, m);
    } else {
        snprintf(buf, bufsz, "%02u:%02u", m, s);
    }
}

/* ======================================================================
 * Process data collection
 * ====================================================================== */

static void collect_processes(void) {
    if (!g_raw) {
        g_raw = malloc(MAX_PROCS * sizeof(procinfo_t));
        if (!g_raw) {
            fprintf(stderr, "top: out of memory\n");
            cleanup_and_exit(1);
        }
    }

    int n = getprocinfo(g_raw, MAX_PROCS);
    if (n < 0) {
        fprintf(stderr, "top: getprocinfo failed: %s\n", strerror(errno));
        cleanup_and_exit(1);
    }

    /* Swap previous and current */
    if (g_prev_procs) free(g_prev_procs);
    g_prev_procs = g_procs;
    g_prev_nprocs = g_nprocs;
    g_procs = NULL;
    g_nprocs = 0;

    g_procs = malloc(n * sizeof(proc_entry_t));
    if (!g_procs) {
        fprintf(stderr, "top: out of memory\n");
        cleanup_and_exit(1);
    }

    /* Get system info for memory calculations */
    struct sysinfo si;
    sysinfo(&si);
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    g_total_mem = si.totalram * unit;

    /* Get uptime in ticks for elapsed time */
    /* Timing info available via struct fields */

    g_nprocs = 0;
    for (int i = 0; i < n; i++) {
        procinfo_t *raw = &g_raw[i];

        proc_entry_t *pe = &g_procs[g_nprocs];
        memcpy(&pe->info, raw, sizeof(procinfo_t));

        /* Calculate total ticks */
        pe->total_ticks = raw->utime_ticks + raw->stime_ticks;

        /* Calculate %MEM (kernel tasks show 0) */
        if (raw->is_kernel) {
            pe->pmem = 0.0;
        } else if (g_total_mem > 0) {
            uint64_t rss_bytes = raw->rss * PAGE_SIZE;
            pe->pmem = (double)rss_bytes * 100.0 / (double)g_total_mem;
        } else {
            pe->pmem = 0.0;
        }

        /* Calculate %CPU using delta from previous snapshot */
        pe->pcpu = 0.0;
        if (g_prev_procs && g_iteration > 0) {
            /* Find this PID in previous snapshot */
            for (int j = 0; j < g_prev_nprocs; j++) {
                if (g_prev_procs[j].info.pid == raw->pid) {
                    uint64_t prev_total = g_prev_procs[j].total_ticks;
                    uint64_t cur_total = pe->total_ticks;
                    uint64_t delta_ticks = (cur_total > prev_total) ? (cur_total - prev_total) : 0;
                    double elapsed_ticks_d = opt_delay * (double)HZ;
                    if (elapsed_ticks_d > 0.0) {
                        pe->pcpu = (double)delta_ticks * 100.0 / elapsed_ticks_d;
                        if (!opt_irix) {
                            /* Solaris mode: divide by number of CPUs */
                            /* For now use 1 CPU */
                            pe->pcpu /= 1.0;
                        }
                    }
                    break;
                }
            }
        }

        g_nprocs++;
    }
}

/* ======================================================================
 * Process filtering
 * ====================================================================== */

static int pass_pid_filter(const proc_entry_t *pe) {
    if (opt_n_filter_pids == 0) return 1;
    for (int i = 0; i < opt_n_filter_pids; i++) {
        if (pe->info.pid == opt_filter_pids[i]) return 1;
    }
    return 0;
}

static int pass_user_filter(const proc_entry_t *pe) {
    if (!opt_filter_user_active) return 1;

    /* For simplicity with numeric UID comparison */
    int target_uid = atoi(opt_filter_user);
    int match = 0;
    if (target_uid > 0 || strcmp(opt_filter_user, "0") == 0) {
        /* Numeric UID */
        if (opt_filter_user_effective) {
            match = (pe->info.euid == target_uid);
        } else {
            match = (pe->info.uid == target_uid ||
                     pe->info.euid == target_uid);
        }
    } else {
        /* Name-based: only "root" for uid 0 */
        if (strcmp(opt_filter_user, "root") == 0) {
            if (opt_filter_user_effective)
                match = (pe->info.euid == 0);
            else
                match = (pe->info.uid == 0 || pe->info.euid == 0);
        }
        /* In this OS we don't have /etc/passwd, so accept numeric UIDs */
    }

    if (opt_filter_user_negate)
        return !match;
    return match;
}

static int pass_idle_filter(const proc_entry_t *pe) {
    if (opt_idle) return 1;
    /* When idle filter is off, only show tasks that have used CPU */
    return (pe->pcpu > 0.0 || pe->info.state == 0 || pe->info.state == 1);
}

static int my_strstr_ci(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (my_strncasecmp(haystack + i, needle, nlen) == 0)
            return 1;
    }
    return 0;
}

static int pass_other_filters(const proc_entry_t *pe) {
    for (int f = 0; f < g_nfilters; f++) {
        filter_t *flt = &g_filters[f];
        /* Get the value for the given field */
        char val_buf[256];
        val_buf[0] = '\0';

        if (my_strcasecmp(flt->field_name, "PID") == 0)
            snprintf(val_buf, sizeof(val_buf), "%d", pe->info.pid);
        else if (my_strcasecmp(flt->field_name, "USER") == 0)
            snprintf(val_buf, sizeof(val_buf), "%d", pe->info.euid);
        else if (my_strcasecmp(flt->field_name, "COMMAND") == 0)
            snprintf(val_buf, sizeof(val_buf), "%s",
                     opt_cmdline ? pe->info.cmdline : pe->info.comm);
        else if (my_strcasecmp(flt->field_name, "NI") == 0)
            snprintf(val_buf, sizeof(val_buf), "%d", pe->info.nice);
        else if (my_strcasecmp(flt->field_name, "S") == 0)
            snprintf(val_buf, sizeof(val_buf), "%c", state_char(pe->info.state));
        else if (my_strcasecmp(flt->field_name, "RES") == 0)
            snprintf(val_buf, sizeof(val_buf), "%lu", (unsigned long)(pe->info.rss * PAGE_SIZE / 1024));
        else if (my_strcasecmp(flt->field_name, "VIRT") == 0)
            snprintf(val_buf, sizeof(val_buf), "%lu", (unsigned long)(pe->info.vsz / 1024));
        else if (my_strcasecmp(flt->field_name, "%CPU") == 0)
            snprintf(val_buf, sizeof(val_buf), "%.1f", pe->pcpu);
        else if (my_strcasecmp(flt->field_name, "%MEM") == 0)
            snprintf(val_buf, sizeof(val_buf), "%.1f", pe->pmem);
        else if (my_strcasecmp(flt->field_name, "nTH") == 0)
            snprintf(val_buf, sizeof(val_buf), "%d", pe->info.nr_threads);
        else
            continue;  /* Unknown field, skip */

        int match = 0;
        if (flt->op == '=') {
            /* Partial match */
            if (flt->case_sensitive) {
                match = (strstr(val_buf, flt->value) != NULL);
            } else {
                match = my_strstr_ci(val_buf, flt->value);
            }
        } else if (flt->op == '>') {
            match = (strcmp(val_buf, flt->value) > 0);
        } else if (flt->op == '<') {
            match = (strcmp(val_buf, flt->value) < 0);
        }

        if (flt->negate) match = !match;
        if (!match) return 0;
    }
    return 1;
}

static int pass_all_filters(const proc_entry_t *pe) {
    return pass_pid_filter(pe) &&
           pass_user_filter(pe) &&
           pass_idle_filter(pe) &&
           pass_other_filters(pe);
}

/* ======================================================================
 * Sorting
 * ====================================================================== */

static int sort_field_id = FLD_PCPU;
static int sort_reverse = 1;

static int compare_procs(const void *a, const void *b) {
    const proc_entry_t *pa = (const proc_entry_t *)a;
    const proc_entry_t *pb = (const proc_entry_t *)b;
    int cmp = 0;

    switch (sort_field_id) {
    case FLD_PID:
        cmp = pa->info.pid - pb->info.pid;
        break;
    case FLD_PPID:
        cmp = pa->info.ppid - pb->info.ppid;
        break;
    case FLD_UID:
    case FLD_USER:
        cmp = pa->info.euid - pb->info.euid;
        break;
    case FLD_PR:
        cmp = pa->info.nice - pb->info.nice;
        break;
    case FLD_NI:
        cmp = pa->info.nice - pb->info.nice;
        break;
    case FLD_NTH:
        cmp = pa->info.nr_threads - pb->info.nr_threads;
        break;
    case FLD_P:
        cmp = pa->info.on_cpu - pb->info.on_cpu;
        break;
    case FLD_PCPU:
        if (pa->pcpu > pb->pcpu) cmp = 1;
        else if (pa->pcpu < pb->pcpu) cmp = -1;
        else cmp = 0;
        break;
    case FLD_PMEM:
        if (pa->pmem > pb->pmem) cmp = 1;
        else if (pa->pmem < pb->pmem) cmp = -1;
        else cmp = 0;
        break;
    case FLD_VIRT:
        if (pa->info.vsz > pb->info.vsz) cmp = 1;
        else if (pa->info.vsz < pb->info.vsz) cmp = -1;
        else cmp = 0;
        break;
    case FLD_RES:
        if (pa->info.rss > pb->info.rss) cmp = 1;
        else if (pa->info.rss < pb->info.rss) cmp = -1;
        else cmp = 0;
        break;
    case FLD_S:
        cmp = pa->info.state - pb->info.state;
        break;
    case FLD_TIME:
    case FLD_TIMEP:
        if (pa->total_ticks > pb->total_ticks) cmp = 1;
        else if (pa->total_ticks < pb->total_ticks) cmp = -1;
        else cmp = 0;
        break;
    case FLD_COMMAND:
        cmp = strcmp(pa->info.comm, pb->info.comm);
        break;
    default:
        cmp = pa->info.pid - pb->info.pid;
        break;
    }

    return sort_reverse ? -cmp : cmp;
}

static void sort_processes(void) {
    sort_field_id = opt_sort_field;
    sort_reverse = opt_sort_reverse;
    qsort(g_procs, g_nprocs, sizeof(proc_entry_t), compare_procs);
}

/* ======================================================================
 * Field value formatting (per-process)
 * ====================================================================== */

static void format_field_value(const proc_entry_t *pe, int field_id,
                               char *buf, size_t bufsz) {
    struct timespec mono;

    switch (field_id) {
    case FLD_PID:
        snprintf(buf, bufsz, "%d", pe->info.pid);
        break;
    case FLD_PPID:
        snprintf(buf, bufsz, "%d", pe->info.ppid);
        break;
    case FLD_UID:
        snprintf(buf, bufsz, "%d", pe->info.euid);
        break;
    case FLD_USER:
        if (pe->info.euid == 0)
            snprintf(buf, bufsz, "root");
        else
            snprintf(buf, bufsz, "%d", pe->info.euid);
        break;
    case FLD_RUID:
        snprintf(buf, bufsz, "%d", pe->info.uid);
        break;
    case FLD_RUSER:
        if (pe->info.uid == 0)
            snprintf(buf, bufsz, "root");
        else
            snprintf(buf, bufsz, "%d", pe->info.uid);
        break;
    case FLD_GID:
        snprintf(buf, bufsz, "%d", pe->info.egid);
        break;
    case FLD_GROUP:
        if (pe->info.egid == 0)
            snprintf(buf, bufsz, "root");
        else
            snprintf(buf, bufsz, "%d", pe->info.egid);
        break;
    case FLD_PGRP:
        snprintf(buf, bufsz, "%d", pe->info.pgid);
        break;
    case FLD_SID:
        snprintf(buf, bufsz, "%d", pe->info.sid);
        break;
    case FLD_TGID:
        snprintf(buf, bufsz, "%d", pe->info.tgid);
        break;
    case FLD_TTY:
        if (pe->info.tty_nr == 0)
            snprintf(buf, bufsz, "?");
        else
            snprintf(buf, bufsz, "tty%d", pe->info.tty_nr);
        break;
    case FLD_PR:
        /* Priority = 20 + nice */
        snprintf(buf, bufsz, "%d", 20 + pe->info.nice);
        break;
    case FLD_NI:
        snprintf(buf, bufsz, "%d", pe->info.nice);
        break;
    case FLD_NTH:
        snprintf(buf, bufsz, "%d", pe->info.nr_threads);
        break;
    case FLD_P:
        snprintf(buf, bufsz, "%d", pe->info.on_cpu);
        break;
    case FLD_PCPU:
        snprintf(buf, bufsz, "%.1f", pe->pcpu);
        break;
    case FLD_PMEM:
        snprintf(buf, bufsz, "%.1f", pe->pmem);
        break;
    case FLD_VIRT:
        format_mem_task(pe->info.is_kernel ? 0 : pe->info.vsz, buf, bufsz, opt_mem_task_scale);
        break;
    case FLD_RES:
        format_mem_task(pe->info.is_kernel ? 0 : pe->info.rss * PAGE_SIZE, buf, bufsz, opt_mem_task_scale);
        break;
    case FLD_SHR:
        /* No SHR info available, show 0 */
        format_mem_task(0, buf, bufsz, opt_mem_task_scale);
        break;
    case FLD_S:
        snprintf(buf, bufsz, "%c", state_char(pe->info.state));
        break;
    case FLD_TIME:
        format_time(pe->total_ticks, buf, bufsz);
        break;
    case FLD_TIMEP:
        format_time_plus(pe->total_ticks, buf, bufsz);
        break;
    case FLD_COMMAND:
        if (pe->info.is_kernel) {
            /* Kernel threads shown in brackets like Linux */
            if (pe->info.comm[0])
                snprintf(buf, bufsz, "[%s]", pe->info.comm);
            else
                snprintf(buf, bufsz, "[kernel]");
        } else if (opt_cmdline && pe->info.cmdline[0]) {
            snprintf(buf, bufsz, "%s", pe->info.cmdline);
        } else {
            snprintf(buf, bufsz, "%s", pe->info.comm);
        }
        break;
    case FLD_WCHAN:
        snprintf(buf, bufsz, "-");
        break;
    case FLD_FLAGS:
        snprintf(buf, bufsz, "%08x", 0);
        break;
    case FLD_ENVIRON:
        snprintf(buf, bufsz, "%s", pe->info.environ);
        break;
    case FLD_SWAP:
        snprintf(buf, bufsz, "0");
        break;
    case FLD_CODE:
        snprintf(buf, bufsz, "0");
        break;
    case FLD_DATA:
        format_mem_task(pe->info.vsz, buf, bufsz, opt_mem_task_scale);
        break;
    case FLD_USED:
        format_mem_task(pe->info.rss * PAGE_SIZE, buf, bufsz, opt_mem_task_scale);
        break;
    case FLD_ELAPSED: {
        clock_gettime(CLOCK_MONOTONIC, &mono);
        uint64_t uptime_ticks = (uint64_t)mono.tv_sec * HZ;
        format_elapsed(uptime_ticks, pe->info.start_tick, buf, bufsz);
        break;
    }
    case FLD_STARTED:
        format_started(pe->info.start_tick, buf, bufsz);
        break;
    default:
        snprintf(buf, bufsz, "?");
        break;
    }
}

/* ======================================================================
 * Screen output - summary area
 * ====================================================================== */
static int g_summary_lines = 0;

static void print_limited(const char *str, int maxcols) {
    int len = (int)strlen(str);
    if (len > maxcols) len = maxcols;
    fwrite(str, 1, len, stdout);
}

/* Print uptime / load average line */
static void print_summary_uptime(const struct sysinfo *si) {
    if (!show_uptime) return;
    g_summary_lines++;

    char line[256];
    int pos = 0;

    /* Current time */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    pos += snprintf(line + pos, sizeof(line) - pos, "top - %02d:%02d:%02d up ",
                    tm->tm_hour, tm->tm_min, tm->tm_sec);

    /* Uptime */
    long up = si->uptime;
    if (up >= 86400) {
        int days = (int)(up / 86400);
        up %= 86400;
        pos += snprintf(line + pos, sizeof(line) - pos, "%d day%s, ",
                        days, days != 1 ? "s" : "");
    }
    int hrs = (int)(up / 3600);
    int mins = (int)((up % 3600) / 60);
    if (hrs > 0)
        pos += snprintf(line + pos, sizeof(line) - pos, "%d:%02d, ", hrs, mins);
    else
        pos += snprintf(line + pos, sizeof(line) - pos, "%d min, ", mins);

    /* Number of users (we show 1) */
    pos += snprintf(line + pos, sizeof(line) - pos, "%d user,  ", 1);

    /* Load averages */
    double la1 = (double)si->loads[0] / 65536.0;
    double la5 = (double)si->loads[1] / 65536.0;
    double la15 = (double)si->loads[2] / 65536.0;
    pos += snprintf(line + pos, sizeof(line) - pos, "load average: %.2f, %.2f, %.2f",
                    la1, la5, la15);

    if (show_colors)
        printf("%s%s", ESC_BOLD, CLR_WHITE);
    print_limited(line, g_term_cols);
    printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);
}

/* Print task states line */
static void print_summary_tasks(void) {
    if (show_task_cpu == 0) return;
    g_summary_lines++;

    int total = 0, running = 0, sleeping = 0, stopped = 0, zombie = 0;
    for (int i = 0; i < g_nprocs; i++) {
        if (!pass_all_filters(&g_procs[i])) continue;
        total++;
        switch (g_procs[i].info.state) {
        case 0: case 1: running++; break;
        case 2: sleeping++; break;
        case 3: stopped++; break;
        case 4: zombie++; break;
        }
    }

    char line[256];
    snprintf(line, sizeof(line),
             "%s: %d total, %d running, %d sleeping, %d stopped, %d zombie",
             opt_threads ? "Threads" : "Tasks",
             total, running, sleeping, stopped, zombie);

    if (show_colors)
        printf("%s%s", ESC_BOLD, CLR_WHITE);
    print_limited(line, g_term_cols);
    printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);
}

/* Print CPU states line */
static void print_summary_cpu(void) {
    if (show_task_cpu == 0) return;
    g_summary_lines++;

    /* Calculate aggregate CPU from all processes */
    double total_us = 0.0, total_sy = 0.0, total_id = 100.0;

    if (g_prev_procs && g_iteration > 0) {
        double elapsed_ticks = opt_delay * (double)HZ;
        if (elapsed_ticks > 0.0) {
            for (int i = 0; i < g_nprocs; i++) {
                /* Find in previous */
                for (int j = 0; j < g_prev_nprocs; j++) {
                    if (g_procs[i].info.pid == g_prev_procs[j].info.pid) {
                        uint64_t du = (g_procs[i].info.utime_ticks > g_prev_procs[j].info.utime_ticks) ?
                                      (g_procs[i].info.utime_ticks - g_prev_procs[j].info.utime_ticks) : 0;
                        uint64_t ds = (g_procs[i].info.stime_ticks > g_prev_procs[j].info.stime_ticks) ?
                                      (g_procs[i].info.stime_ticks - g_prev_procs[j].info.stime_ticks) : 0;
                        total_us += (double)du * 100.0 / elapsed_ticks;
                        total_sy += (double)ds * 100.0 / elapsed_ticks;
                        break;
                    }
                }
            }
        }
        total_id = 100.0 - total_us - total_sy;
        if (total_id < 0.0) total_id = 0.0;
    }

    char line[256];
    if (show_task_cpu == 1) {
        /* Detailed percentages */
        snprintf(line, sizeof(line),
                 "%%Cpu(s): %5.1f us, %5.1f sy,  0.0 ni, %5.1f id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st",
                 total_us, total_sy, total_id);
    } else if (show_task_cpu == 2) {
        /* Bar graph */
        double used = total_us + total_sy;
        int bar_width = g_term_cols - 20;
        if (bar_width < 10) bar_width = 10;
        if (bar_width > 60) bar_width = 60;
        int filled = (int)(used * bar_width / 100.0);
        if (filled > bar_width) filled = bar_width;
        char bar[128];
        int bpos = 0;
        for (int i = 0; i < bar_width && bpos < (int)sizeof(bar) - 1; i++)
            bar[bpos++] = (i < filled) ? '|' : ' ';
        bar[bpos] = '\0';
        snprintf(line, sizeof(line), "%%Cpu(s) [%s] %5.1f%%", bar, used);
    } else if (show_task_cpu == 3) {
        /* Block graph */
        double used = total_us + total_sy;
        int bar_width = g_term_cols - 20;
        if (bar_width < 10) bar_width = 10;
        if (bar_width > 60) bar_width = 60;
        int filled = (int)(used * bar_width / 100.0);
        if (filled > bar_width) filled = bar_width;
        char bar[128];
        int bpos = 0;
        for (int i = 0; i < bar_width && bpos < (int)sizeof(bar) - 4; i++) {
            if (i < filled) {
                /* Full block character (using '#' since we may not have unicode) */
                bar[bpos++] = '#';
            } else {
                bar[bpos++] = ' ';
            }
        }
        bar[bpos] = '\0';
        snprintf(line, sizeof(line), "%%Cpu(s) [%s] %5.1f%%", bar, used);
    } else {
        return; /* mode 0: off */
    }

    if (show_colors)
        printf("%s%s", ESC_BOLD, CLR_WHITE);
    print_limited(line, g_term_cols);
    printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);
}

/* Print memory lines */
static void print_summary_memory(const struct sysinfo *si) {
    if (show_memory == 0) return;

    unsigned long unit = si->mem_unit ? si->mem_unit : 1;
    uint64_t total     = (uint64_t)si->totalram * unit;
    uint64_t free_mem  = (uint64_t)si->freeram * unit;
    uint64_t buffers   = (uint64_t)si->bufferram * unit;
    uint64_t cached    = (uint64_t)si->cached * unit;
    uint64_t available = (uint64_t)si->available * unit;
    uint64_t used      = total - free_mem - buffers - cached;
    if (used > total) used = 0;

    uint64_t swap_total = (uint64_t)si->totalswap * unit;
    uint64_t swap_free  = (uint64_t)si->freeswap * unit;
    uint64_t swap_used  = swap_total - swap_free;

    int sc = opt_mem_summary_scale;
    const char *lbl = mem_scale_label[sc];

    char line[256];

    if (show_memory == 1) {
        /* Detailed numbers */
        g_summary_lines++;
        snprintf(line, sizeof(line),
                 "%s Mem : %9.1f total, %9.1f free, %9.1f used, %9.1f buff/cache",
                 lbl,
                 mem_scale_bytes(total, sc),
                 mem_scale_bytes(free_mem, sc),
                 mem_scale_bytes(used, sc),
                 mem_scale_bytes(buffers + cached, sc));
        if (show_colors)
            printf("%s%s", ESC_BOLD, CLR_WHITE);
        print_limited(line, g_term_cols);
        printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);

        g_summary_lines++;
        snprintf(line, sizeof(line),
                 "%s Swap: %9.1f total, %9.1f free, %9.1f used. %9.1f avail Mem",
                 lbl,
                 mem_scale_bytes(swap_total, sc),
                 mem_scale_bytes(swap_free, sc),
                 mem_scale_bytes(swap_used, sc),
                 mem_scale_bytes(available, sc));
        if (show_colors)
            printf("%s%s", ESC_BOLD, CLR_WHITE);
        print_limited(line, g_term_cols);
        printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);
    } else if (show_memory == 2 || show_memory == 3) {
        /* Bar/Block graph for memory */
        double mem_pct = (total > 0) ? ((double)used * 100.0 / (double)total) : 0.0;
        double swap_pct = (swap_total > 0) ? ((double)swap_used * 100.0 / (double)swap_total) : 0.0;
        int bar_width = g_term_cols - 30;
        if (bar_width < 10) bar_width = 10;
        if (bar_width > 50) bar_width = 50;
        char fill_ch = (show_memory == 2) ? '|' : '#';

        /* Mem bar */
        g_summary_lines++;
        int mfill = (int)(mem_pct * bar_width / 100.0);
        if (mfill > bar_width) mfill = bar_width;
        char bar[128];
        int bp = 0;
        for (int i = 0; i < bar_width && bp < (int)sizeof(bar) - 1; i++)
            bar[bp++] = (i < mfill) ? fill_ch : ' ';
        bar[bp] = '\0';
        snprintf(line, sizeof(line), "%s Mem [%s] %5.1f%% of %.1f%s",
                 lbl, bar, mem_pct, mem_scale_bytes(total, sc), lbl);
        if (show_colors)
            printf("%s%s", ESC_BOLD, CLR_WHITE);
        print_limited(line, g_term_cols);
        printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);

        /* Swap bar */
        g_summary_lines++;
        int sfill = (int)(swap_pct * bar_width / 100.0);
        if (sfill > bar_width) sfill = bar_width;
        bp = 0;
        for (int i = 0; i < bar_width && bp < (int)sizeof(bar) - 1; i++)
            bar[bp++] = (i < sfill) ? fill_ch : ' ';
        bar[bp] = '\0';
        snprintf(line, sizeof(line), "%s Swp [%s] %5.1f%% of %.1f%s",
                 lbl, bar, swap_pct, mem_scale_bytes(swap_total, sc), lbl);
        if (show_colors)
            printf("%s%s", ESC_BOLD, CLR_WHITE);
        print_limited(line, g_term_cols);
        printf("%s%s\n", ESC_CLEAR_EOL, ESC_RESET);
    }
}

/* Print the full summary area */
static void print_summary(void) {
    struct sysinfo si;
    sysinfo(&si);

    g_summary_lines = 0;
    print_summary_uptime(&si);
    print_summary_tasks();
    print_summary_cpu();
    print_summary_memory(&si);

    /* Blank separator line (message line) */
    g_summary_lines++;
    if (show_scroll_coords) {
        printf("scroll coordinates: y = %d/%d (tasks), x = %d/%d (fields)",
               g_scroll_y, g_nprocs, g_scroll_x, g_nfields);
    }
    printf("%s\n", ESC_CLEAR_EOL);
}

/* ======================================================================
 * Screen output - task area (column header + process rows)
 * ====================================================================== */

static void print_task_header(void) {
    if (show_colors) {
        printf("%s%s%s", ESC_BOLD, ESC_REVERSE, CLR_WHITE);
    } else {
        printf("%s", ESC_REVERSE);
    }

    int col_pos = 0;
    for (int f = 0; f < g_nfields; f++) {
        if (!g_fields[f].visible) continue;
        if (f > 0 && g_fields[f - 1].visible) {
            putchar(' ');
            col_pos++;
        }

        /* Skip fields before scroll_x */
        /* (simplified: we just show from scroll_x column onward) */

        int w = g_fields[f].width;
        if (w < 0) {
            /* Variable width: use remaining space */
            w = g_term_cols - col_pos;
            if (w < 1) w = 1;
        }

        /* Highlight sort column */
        if (show_hlight_x && g_fields[f].id == opt_sort_field && show_colors) {
            printf("%s", CLR_YELLOW);
        }

        if (g_fields[f].right)
            printf("%*s", w, g_fields[f].header);
        else
            printf("%-*s", w, g_fields[f].header);

        col_pos += w;
        if (col_pos >= g_term_cols) break;
    }

    /* Fill rest of line (for full-width reverse-video bar) */
    while (col_pos < g_term_cols) {
        putchar(' ');
        col_pos++;
    }

    printf("%s", ESC_RESET);
    /* If we filled to g_term_cols, cursor already auto-wrapped; no \n needed */
    if (col_pos < g_term_cols)
        putchar('\n');
}

static void print_task_row(const proc_entry_t *pe, int last_row) {
    int is_running = (pe->info.state == 0 || pe->info.state == 1);

    /* Row highlight for running tasks */
    if (show_hlight_y && is_running && show_colors)
        printf("%s", ESC_BOLD);

    int col_pos = 0;
    char val[512];

    for (int f = 0; f < g_nfields; f++) {
        if (!g_fields[f].visible) continue;
        if (f > 0 && g_fields[f - 1].visible) {
            putchar(' ');
            col_pos++;
        }

        int w = g_fields[f].width;
        if (w < 0) {
            /* Variable width */
            w = g_term_cols - col_pos;
            if (w < 1) w = 1;
        }

        format_field_value(pe, g_fields[f].id, val, sizeof(val));

        /* Sort column highlighting */
        if (show_hlight_x && g_fields[f].id == opt_sort_field && show_colors)
            printf("%s", ESC_BOLD);

        /* Zero suppress */
        if (!show_zero && g_fields[f].id != FLD_PID && g_fields[f].id != FLD_UID &&
            g_fields[f].id != FLD_USER && g_fields[f].id != FLD_NI &&
            g_fields[f].id != FLD_PR && g_fields[f].id != FLD_P &&
            g_fields[f].id != FLD_S && g_fields[f].id != FLD_COMMAND) {
            /* Check if value is "0" or "0.0" */
            if (strcmp(val, "0") == 0 || strcmp(val, "0.0") == 0 ||
                strcmp(val, "0.000") == 0 || strcmp(val, "0:00") == 0 ||
                strcmp(val, "0:00.00") == 0) {
                val[0] = '\0';
            }
        }

        if (g_fields[f].right)
            printf("%*.*s", w, w, val);
        else
            printf("%-*.*s", w, w, val);

        if (show_hlight_x && g_fields[f].id == opt_sort_field && show_colors)
            printf("%s", ESC_RESET);

        col_pos += w;
        if (col_pos >= g_term_cols) break;
    }

    if (show_hlight_y && is_running && show_colors)
        printf("%s", ESC_RESET);

    if (col_pos >= g_term_cols) {
        /* Cursor already auto-wrapped to next line; no \n needed */
    } else if (last_row) {
        printf("%s", ESC_CLEAR_EOL);
    } else {
        printf("%s\n", ESC_CLEAR_EOL);
    }
}

/* ======================================================================
 * Full screen repaint
 * ====================================================================== */

static void repaint_screen(void) {
    if (!opt_batch) {
        printf("%s%s", ESC_HOME, ESC_CLEAR);
    }

    print_summary();
    print_task_header();

    /* Available rows for tasks */
    int avail_rows = g_term_rows - g_summary_lines - 1; /* -1 for header row */
    if (avail_rows < 0) avail_rows = 0;
    if (g_max_tasks > 0 && g_max_tasks < avail_rows)
        avail_rows = g_max_tasks;

    int displayed = 0;
    int skipped = 0;
    int rows_left = avail_rows;   /* counts down to 0 */
    for (int i = 0; i < g_nprocs && displayed < avail_rows; i++) {
        if (!pass_all_filters(&g_procs[i])) continue;

        /* Apply vertical scroll */
        if (skipped < g_scroll_y) {
            skipped++;
            continue;
        }

        rows_left = avail_rows - displayed - 1;
        print_task_row(&g_procs[i], rows_left == 0);
        displayed++;
    }

    /* In non-batch mode, the screen was already cleared by ESC_CLEAR
       so we don't need to emit blank lines.  Just leave the rest blank. */

    fflush(stdout);
}

/* ======================================================================
 * Help screen
 * ====================================================================== */

static void show_help_screen(void) {
    printf("%s%s", ESC_HOME, ESC_CLEAR);
    printf("%sHelp for Interactive Commands%s - %s\n", ESC_BOLD, ESC_RESET, TOP_VERSION);
    printf("Window 1:Def: Cumulative mode %s.  System: Delay %.1f secs; Secure mode %s.\n\n",
           opt_cumulative ? "On" : "Off", opt_delay, opt_secure ? "On" : "Off");

    printf("  Z%s,%sB%s,%sE%s,%se%s   Global: 'Z' colors; 'B' bold; 'E'/'e' summary/task memory scale\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD, ESC_RESET, ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("  l%s,%st%s,%sm%s,%s1%s   Toggle: 'l' load avg; 't' task/cpu stats; 'm' memory info; '1' cpus\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD, ESC_RESET, ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("%s\n", ESC_RESET);
    printf("  f%s,%sF%s        Fields: 'f' add/remove/order/sort; 'F' focus toggle\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("  <,>%s        Fields: move sort column left/right\n", ESC_RESET);
    printf("  R%s,%sH%s,%sV%s   View:  'R' Sort-pids; 'H' threads; 'V' forest view\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("%s\n", ESC_RESET);
    printf("  L%s,%s&%s        Find: 'L' locate a string; '&' locate next\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("  o%s,%sO%s        Filter: 'o'/'O' other filtering criteria\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("  =%s          Reset Filters (in current window)\n", ESC_RESET);
    printf("%s\n", ESC_RESET);
    printf("  c%s          Toggle command name/line\n", ESC_RESET);
    printf("  i%s          Idle tasks toggle\n", ESC_RESET);
    printf("  S%s          Cumulative time mode toggle\n", ESC_RESET);
    printf("  U/u%s        Show user filter\n", ESC_RESET);
    printf("  x%s,%sy%s        Highlight: 'x' sort field; 'y' running tasks\n",
           ESC_BOLD, ESC_RESET, ESC_BOLD);
    printf("  z%s          Color/Monochrome toggle\n", ESC_RESET);
    printf("  b%s          Bold/Reverse toggle\n", ESC_RESET);
    printf("  0%s          Zero suppress toggle\n", ESC_RESET);
    printf("  n%s          Set max tasks\n", ESC_RESET);
    printf("  d/s%s        Change delay time interval\n", ESC_RESET);
    printf("  k%s          Kill a task\n", ESC_RESET);
    printf("  r%s          Renice a task\n", ESC_RESET);
    printf("  C%s          Scroll coordinates toggle\n", ESC_RESET);
    printf("  W%s          Write configuration file\n", ESC_RESET);
    printf("  q%s          Quit\n", ESC_RESET);
    printf("\nPress any key to return...");
    fflush(stdout);
    read_char_blocking();
}

/* ======================================================================
 * Fields management screen
 * ====================================================================== */

static void show_fields_screen(void) {
    printf("%s%s", ESC_HOME, ESC_CLEAR);
    printf("%sFields Management%s for window 1:Def, sorted by %s\n\n",
           ESC_BOLD, ESC_RESET, field_specs[opt_sort_field].name);
    printf("Navigate with Up/Down, toggle with 'd'/space, sort with 's'\n");
    printf("Press 'q' or Esc to return\n\n");

    int cursor = 0;
    int done = 0;

    while (!done) {
        printf("\033[5;1H"); /* Move to line 5 */
        for (int i = 0; i < FLD__COUNT; i++) {
            const field_spec_t *sp = find_field_spec(i);
            if (!sp) continue;

            /* Check if active */
            int active = (find_active_field(i) >= 0);
            int is_sort = (i == opt_sort_field);

            if (i == cursor)
                printf("%s", ESC_REVERSE);

            printf("  %c %c %-10s - %s",
                   active ? '*' : ' ',
                   is_sort ? 's' : ' ',
                   sp->name,
                   sp->header);

            if (i == cursor)
                printf("%s", ESC_RESET);

            printf("%s\n", ESC_CLEAR_EOL);
        }
        fflush(stdout);

        int ch = read_char_blocking();
        if (ch == 'q' || ch == 27) {
            done = 1;
        } else if (ch == 27) {
            /* Arrow keys: ESC [ A/B */
            int ch2 = read_char_blocking();
            if (ch2 == '[') {
                int ch3 = read_char_blocking();
                if (ch3 == 'A' && cursor > 0) cursor--;
                if (ch3 == 'B' && cursor < FLD__COUNT - 1) cursor++;
            }
        } else if (ch == 'd' || ch == ' ') {
            /* Toggle field visibility */
            int idx = find_active_field(cursor);
            if (idx >= 0) {
                /* Remove field */
                for (int j = idx; j < g_nfields - 1; j++)
                    g_fields[j] = g_fields[j + 1];
                g_nfields--;
            } else {
                /* Add field */
                const field_spec_t *sp = find_field_spec(cursor);
                if (sp && g_nfields < MAX_FIELDS) {
                    active_field_t *af = &g_fields[g_nfields++];
                    af->id = sp->id;
                    strncpy(af->header, sp->header, sizeof(af->header) - 1);
                    af->header[sizeof(af->header) - 1] = '\0';
                    af->width = sp->width;
                    af->right = sp->right;
                    af->visible = 1;
                }
            }
        } else if (ch == 's') {
            /* Set sort field */
            opt_sort_field = cursor;
        }
    }
}

/* ======================================================================
 * Interactive command processing
 * ====================================================================== */

/* Display a message on the bottom line */
static void show_message(const char *msg) {
    /* Position to message line (after summary); clear rest of line */
    printf("\033[%d;1H%s%s%s%s", g_summary_lines, ESC_BOLD, msg, ESC_RESET, ESC_CLEAR_EOL);
    fflush(stdout);
}

/* Prompt for input on message line, returns chars or -1 on escape */
static int prompt_input(const char *prompt, char *buf, int maxlen) {
    printf("\033[%d;1H%s%s%s %s", g_summary_lines, ESC_BOLD, prompt, ESC_RESET, ESC_CLEAR_EOL);
    fflush(stdout);
    return read_line_raw(buf, maxlen);
}

static void process_interactive_command(int ch) {
    char buf[256];

    switch (ch) {
    case 'q':
        cleanup_and_exit(0);
        break;

    case '?':
    case 'h':
        show_help_screen();
        break;

    case ' ':
    case '\n':
    case '\r':
        /* Refresh */
        break;

    case '=':
        /* Reset filters and display limits */
        opt_idle = 1;
        g_max_tasks = 0;
        show_forest = 0;
        opt_filter_user_active = 0;
        g_nfilters = 0;
        g_scroll_y = 0;
        g_scroll_x = 0;
        break;

    case '0':
        /* Zero suppress toggle */
        show_zero = !show_zero;
        break;

    case 'B':
        /* Bold toggle */
        show_bold = !show_bold;
        break;

    case 'd':
    case 's':
        if (opt_secure) break;
        if (prompt_input("Change delay from %.1f to", buf, sizeof(buf)) >= 0 && buf[0]) {
            double d = strtod(buf, NULL);
            if (d >= 0.0) opt_delay = d;
        }
        break;

    case 'E':
        /* Cycle summary memory scale */
        opt_mem_summary_scale = (opt_mem_summary_scale + 1) % 6;
        break;

    case 'e':
        /* Cycle task memory scale */
        opt_mem_task_scale = (opt_mem_task_scale + 1) % 5;
        break;

    case 'f':
        show_fields_screen();
        break;

    case 'H':
        /* Threads toggle */
        opt_threads = !opt_threads;
        show_message(opt_threads ? "Threads mode On" : "Threads mode Off");
        break;

    case 'I':
        /* Irix/Solaris mode toggle */
        opt_irix = !opt_irix;
        show_message(opt_irix ? "Irix mode On" : "Solaris mode On");
        break;

    case 'i':
        /* Idle process toggle */
        opt_idle = !opt_idle;
        break;

    case 'k':
        /* Kill a task */
        if (opt_secure) break;
        if (prompt_input("PID to signal/kill [default pid = first displayed]", buf, sizeof(buf)) >= 0) {
            int pid = 0;
            if (buf[0])
                pid = atoi(buf);
            else if (g_nprocs > 0)
                pid = g_procs[0].info.pid;

            if (pid > 0) {
                char sig_buf[64];
                char sig_prompt[128];
                snprintf(sig_prompt, sizeof(sig_prompt), "Send pid %d signal [15/sigterm]", pid);
                if (prompt_input(sig_prompt, sig_buf, sizeof(sig_buf)) >= 0) {
                    int sig = SIGTERM;
                    if (sig_buf[0]) {
                        sig = atoi(sig_buf);
                        if (sig == 0) {
                            /* Try signal name */
                            if (my_strcasecmp(sig_buf, "sigkill") == 0 || my_strcasecmp(sig_buf, "kill") == 0)
                                sig = SIGKILL;
                            else if (my_strcasecmp(sig_buf, "sigterm") == 0 || my_strcasecmp(sig_buf, "term") == 0)
                                sig = SIGTERM;
                            else if (my_strcasecmp(sig_buf, "sigint") == 0 || my_strcasecmp(sig_buf, "int") == 0)
                                sig = SIGINT;
                            else if (my_strcasecmp(sig_buf, "sighup") == 0 || my_strcasecmp(sig_buf, "hup") == 0)
                                sig = SIGHUP;
                            else if (my_strcasecmp(sig_buf, "sigstop") == 0 || my_strcasecmp(sig_buf, "stop") == 0)
                                sig = SIGSTOP;
                            else if (my_strcasecmp(sig_buf, "sigcont") == 0 || my_strcasecmp(sig_buf, "cont") == 0)
                                sig = SIGCONT;
                            else if (my_strcasecmp(sig_buf, "sigusr1") == 0 || my_strcasecmp(sig_buf, "usr1") == 0)
                                sig = SIGUSR1;
                            else if (my_strcasecmp(sig_buf, "sigusr2") == 0 || my_strcasecmp(sig_buf, "usr2") == 0)
                                sig = SIGUSR2;
                            else {
                                show_message("Invalid signal");
                                break;
                            }
                        }
                    }
                    /* If killing ourselves, clean up first */
                    if (pid == getpid()) {
                        cleanup_and_exit(0);
                    }
                    if (kill(pid, sig) < 0) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Kill failed: %s", strerror(errno));
                        show_message(msg);
                    }
                }
            }
        }
        break;

    case 'r':
        /* Renice a task */
        if (opt_secure) break;
        if (prompt_input("PID to renice [default pid = first displayed]", buf, sizeof(buf)) >= 0) {
            int pid = 0;
            if (buf[0])
                pid = atoi(buf);
            else if (g_nprocs > 0)
                pid = g_procs[0].info.pid;

            if (pid > 0) {
                char nice_buf[64];
                if (prompt_input("Renice PID to value", nice_buf, sizeof(nice_buf)) >= 0 && nice_buf[0]) {
                    /* No renice syscall available in this OS, just show message */
                    show_message("Renice not supported");
                }
            }
        }
        break;

    case 'l':
        /* Load/uptime toggle */
        show_uptime = !show_uptime;
        break;

    case 't':
        /* Task/CPU toggle (4-way: 1->2->3->0->1) */
        show_task_cpu = (show_task_cpu + 1) % 4;
        if (show_task_cpu == 0) show_task_cpu = 0; /* Off: handled in display */
        break;

    case 'm':
        /* Memory toggle (4-way: 1->2->3->0->1) */
        show_memory = (show_memory + 1) % 4;
        break;

    case '1':
        /* Single/Separate CPU toggle */
        opt_single_cpu = !opt_single_cpu;
        break;

    case 'c':
        /* Command name/line toggle */
        opt_cmdline = !opt_cmdline;
        break;

    case 'S':
        /* Cumulative time mode toggle */
        opt_cumulative = !opt_cumulative;
        show_message(opt_cumulative ? "Cumulative mode On" : "Cumulative mode Off");
        break;

    case 'u':
    case 'U': {
        /* User filter */
        char user_buf[128];
        int r = prompt_input("Which user (blank for all)", user_buf, sizeof(user_buf));
        if (r >= 0) {
            if (user_buf[0] == '\0') {
                opt_filter_user_active = 0;
            } else {
                opt_filter_user_negate = 0;
                char *p = user_buf;
                if (*p == '!') {
                    opt_filter_user_negate = 1;
                    p++;
                }
                strncpy(opt_filter_user, p, sizeof(opt_filter_user) - 1);
                opt_filter_user[sizeof(opt_filter_user) - 1] = '\0';
                opt_filter_user_effective = (ch == 'u') ? 1 : 0;
                opt_filter_user_active = 1;
            }
        }
        break;
    }

    case 'V':
        /* Forest view toggle */
        show_forest = !show_forest;
        break;

    case 'x':
        /* Sort column highlight */
        show_hlight_x = !show_hlight_x;
        break;

    case 'y':
        /* Running task highlight */
        show_hlight_y = !show_hlight_y;
        break;

    case 'z':
        /* Color toggle */
        show_colors = !show_colors;
        break;

    case 'b':
        /* Bold/Reverse toggle for highlights */
        /* This cycles the highlighting style */
        break;

    case 'n':
    case '#': {
        /* Set maximum tasks */
        char n_buf[32];
        if (prompt_input("Maximum tasks = 0 is unlimited", n_buf, sizeof(n_buf)) >= 0 && n_buf[0]) {
            int n = atoi(n_buf);
            if (n >= 0) g_max_tasks = n;
        }
        break;
    }

    case 'o': {
        /* Other filter (case insensitive) */
        char filter_buf[256];
        if (prompt_input("add filter #1 (ignstrcase) as: [!]FLD?VAL", filter_buf, sizeof(filter_buf)) >= 0 && filter_buf[0]) {
            if (g_nfilters < MAX_FILTERS) {
                filter_t *flt = &g_filters[g_nfilters];
                memset(flt, 0, sizeof(*flt));
                flt->case_sensitive = 0;
                char *p = filter_buf;
                if (*p == '!') { flt->negate = 1; p++; }
                /* Parse field name */
                char *fn = p;
                while (*p && *p != '=' && *p != '<' && *p != '>') p++;
                if (*p) {
                    flt->op = *p;
                    *p = '\0';
                    p++;
                    strncpy(flt->field_name, fn, sizeof(flt->field_name) - 1);
                    strncpy(flt->value, p, sizeof(flt->value) - 1);
                    g_nfilters++;
                }
            }
        }
        break;
    }

    case 'O': {
        /* Other filter (case sensitive) */
        char filter_buf[256];
        if (prompt_input("add filter #1 (case sensitive) as: [!]FLD?VAL", filter_buf, sizeof(filter_buf)) >= 0 && filter_buf[0]) {
            if (g_nfilters < MAX_FILTERS) {
                filter_t *flt = &g_filters[g_nfilters];
                memset(flt, 0, sizeof(*flt));
                flt->case_sensitive = 1;
                char *p = filter_buf;
                if (*p == '!') { flt->negate = 1; p++; }
                char *fn = p;
                while (*p && *p != '=' && *p != '<' && *p != '>') p++;
                if (*p) {
                    flt->op = *p;
                    *p = '\0';
                    p++;
                    strncpy(flt->field_name, fn, sizeof(flt->field_name) - 1);
                    strncpy(flt->value, p, sizeof(flt->value) - 1);
                    g_nfilters++;
                }
            }
        }
        break;
    }

    case 'R':
        /* Reverse sort toggle */
        opt_sort_reverse = !opt_sort_reverse;
        break;

    case '<':
        /* Move sort field left */
        for (int i = 0; i < g_nfields; i++) {
            if (g_fields[i].id == opt_sort_field && i > 0) {
                opt_sort_field = g_fields[i - 1].id;
                break;
            }
        }
        break;

    case '>':
        /* Move sort field right */
        for (int i = 0; i < g_nfields; i++) {
            if (g_fields[i].id == opt_sort_field && i < g_nfields - 1) {
                opt_sort_field = g_fields[i + 1].id;
                break;
            }
        }
        break;

    case 'M':
        /* Sort by %MEM (compatibility) */
        opt_sort_field = FLD_PMEM;
        opt_sort_reverse = 1;
        break;

    case 'N':
        /* Sort by PID (compatibility) */
        opt_sort_field = FLD_PID;
        opt_sort_reverse = 1;
        break;

    case 'P':
        /* Sort by %CPU (compatibility) */
        opt_sort_field = FLD_PCPU;
        opt_sort_reverse = 1;
        break;

    case 'T':
        /* Sort by TIME+ (compatibility) */
        opt_sort_field = FLD_TIMEP;
        opt_sort_reverse = 1;
        break;

    case 'C':
        /* Scroll coordinates toggle */
        show_scroll_coords = !show_scroll_coords;
        break;

    case 'W':
        /* Write configuration (no-op in this implementation) */
        show_message("Configuration not saved (not implemented)");
        break;

    case 'L': {
        /* Locate string */
        char search_buf[128];
        if (prompt_input("Locate string", search_buf, sizeof(search_buf)) >= 0 && search_buf[0]) {
            /* Search through processes for the string */
            for (int i = 0; i < g_nprocs; i++) {
                if (!pass_all_filters(&g_procs[i])) continue;
                char cmd[1024];
                if (opt_cmdline && g_procs[i].info.cmdline[0])
                    snprintf(cmd, sizeof(cmd), "%s", g_procs[i].info.cmdline);
                else
                    snprintf(cmd, sizeof(cmd), "%s", g_procs[i].info.comm);

                if (strstr(cmd, search_buf)) {
                    g_scroll_y = i;
                    break;
                }
            }
        }
        break;
    }

    case 'j':
        /* Justify character columns toggle (no-op for now) */
        break;
    case 'J':
        /* Justify numeric columns toggle (no-op for now) */
        break;

    case 'g': {
        /* Choose window/field group */
        char g_buf[8];
        if (prompt_input("Choose field group (1-4)", g_buf, sizeof(g_buf)) >= 0) {
            /* Only window 1 is supported */
        }
        break;
    }

    default:
        /* Check for arrow keys (ESC [ A/B/C/D) */
        if (ch == 27) {
            int ch2 = read_char_blocking();
            if (ch2 == '[') {
                int ch3 = read_char_blocking();
                switch (ch3) {
                case 'A': /* Up */
                    if (g_scroll_y > 0) g_scroll_y--;
                    break;
                case 'B': /* Down */
                    g_scroll_y++;
                    break;
                case 'C': /* Right */
                    g_scroll_x++;
                    break;
                case 'D': /* Left */
                    if (g_scroll_x > 0) g_scroll_x--;
                    break;
                case '5': /* PgUp */
                    read_char_blocking(); /* consume '~' */
                    g_scroll_y -= (g_term_rows - g_summary_lines - 2);
                    if (g_scroll_y < 0) g_scroll_y = 0;
                    break;
                case '6': /* PgDn */
                    read_char_blocking(); /* consume '~' */
                    g_scroll_y += (g_term_rows - g_summary_lines - 2);
                    break;
                case 'H': /* Home */
                    g_scroll_y = 0;
                    g_scroll_x = 0;
                    break;
                case 'F': /* End */
                    g_scroll_y = g_nprocs - 1;
                    break;
                }
            }
        }
        break;
    }
}

/* ======================================================================
 * Print field names for -O option
 * ====================================================================== */

static void print_field_names(void) {
    for (const field_spec_t *f = field_specs; f->name; f++) {
        printf("  %-12s %s\n", f->name, f->header);
    }
}

/* ======================================================================
 * Command-line option parsing
 * ====================================================================== */

static void print_usage(void) {
    printf("Usage:\n");
    printf("  top -hv | -bcEeHisS1 -d secs -n max -u|U user -p pid(s) -o field -w [cols]\n");
}

static void print_version(void) {
    printf("%s\n", TOP_VERSION);
}

static int parse_options(int argc, char *argv[]) {
    int opt;
    /* Note: getopt doesn't handle all top conventions perfectly,
       but we parse as best we can */
    while ((opt = getopt(argc, argv, "bcd:E:e:Hhin:O:o:p:SsU:u:Vw::1")) != -1) {
        switch (opt) {
        case 'b':
            opt_batch = 1;
            break;
        case 'c':
            opt_cmdline = !opt_cmdline;
            break;
        case 'd':
            opt_delay = strtod(optarg, NULL);
            if (opt_delay < 0.0) opt_delay = 0.0;
            break;
        case 'E':
            /* Summary memory scale */
            switch (optarg[0]) {
            case 'k': opt_mem_summary_scale = 0; break;
            case 'm': opt_mem_summary_scale = 1; break;
            case 'g': opt_mem_summary_scale = 2; break;
            case 't': opt_mem_summary_scale = 3; break;
            case 'p': opt_mem_summary_scale = 4; break;
            case 'e': opt_mem_summary_scale = 5; break;
            default:
                fprintf(stderr, "top: invalid memory scale '%c'\n", optarg[0]);
                return -1;
            }
            break;
        case 'e':
            /* Task memory scale */
            switch (optarg[0]) {
            case 'k': opt_mem_task_scale = 0; break;
            case 'm': opt_mem_task_scale = 1; break;
            case 'g': opt_mem_task_scale = 2; break;
            case 't': opt_mem_task_scale = 3; break;
            case 'p': opt_mem_task_scale = 4; break;
            default:
                fprintf(stderr, "top: invalid memory scale '%c'\n", optarg[0]);
                return -1;
            }
            break;
        case 'H':
            opt_threads = 1;
            break;
        case 'h':
            print_usage();
            exit(0);
        case 'i':
            opt_idle = !opt_idle;
            break;
        case 'n':
            opt_iterations = atoi(optarg);
            break;
        case 'O':
            /* Print field names and exit */
            print_field_names();
            exit(0);
        case 'o': {
            /* Set sort field */
            const char *name = optarg;
            int rev = 1;
            if (*name == '+') { rev = 1; name++; }
            else if (*name == '-') { rev = 0; name++; }

            const field_spec_t *sp = find_field_by_name(name);
            if (sp) {
                opt_sort_field = sp->id;
                opt_sort_reverse = rev;
            } else {
                fprintf(stderr, "top: unknown field '%s'\n", name);
                fprintf(stderr, "Available fields:\n");
                print_field_names();
                return -1;
            }
            break;
        }
        case 'p': {
            /* Filter by PID */
            char *tok = strtok(optarg, ",");
            while (tok && opt_n_filter_pids < MAX_PIDS) {
                opt_filter_pids[opt_n_filter_pids++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
            /* When -p is used, delay between updates defaults to 0 for first update */
            break;
        }
        case 'S':
            opt_cumulative = 1;
            break;
        case 's':
            opt_secure = 1;
            break;
        case 'U':
            strncpy(opt_filter_user, optarg, sizeof(opt_filter_user) - 1);
            opt_filter_user[sizeof(opt_filter_user) - 1] = '\0';
            opt_filter_user_effective = 0;
            opt_filter_user_active = 1;
            if (opt_filter_user[0] == '!') {
                opt_filter_user_negate = 1;
                memmove(opt_filter_user, opt_filter_user + 1, strlen(opt_filter_user));
            }
            break;
        case 'u':
            strncpy(opt_filter_user, optarg, sizeof(opt_filter_user) - 1);
            opt_filter_user[sizeof(opt_filter_user) - 1] = '\0';
            opt_filter_user_effective = 1;
            opt_filter_user_active = 1;
            if (opt_filter_user[0] == '!') {
                opt_filter_user_negate = 1;
                memmove(opt_filter_user, opt_filter_user + 1, strlen(opt_filter_user));
            }
            break;
        case 'V':
            print_version();
            exit(0);
        case 'w':
            if (optarg)
                opt_width = atoi(optarg);
            else
                opt_width = 512;
            break;
        case '1':
            opt_single_cpu = !opt_single_cpu;
            break;
        default:
            print_usage();
            return -1;
        }
    }
    return 0;
}

/* ======================================================================
 * Delay with interruptibility
 *
 * In interactive mode we need to be able to break out of the delay
 * when the user presses a key. We use alarm() + SIGALRM to implement
 * a timed wait on stdin read.
 * ====================================================================== */

static volatile int g_alarm_fired = 0;

static void sigalrm_handler(int sig) {
    (void)sig;
    g_alarm_fired = 1;
}

static int interruptible_delay(double seconds) {
    if (opt_batch) {
        /* In batch mode just sleep */
        if (seconds >= 1.0) {
            struct timespec ts;
            ts.tv_sec = (time_t)seconds;
            ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
            nanosleep(&ts, NULL);
        } else if (seconds > 0.0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = (long)(seconds * 1000000000.0);
            nanosleep(&ts, NULL);
        }
        return -1;
    }

    /* Set up SIGALRM */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, &old_sa);

    g_alarm_fired = 0;

    /* Set alarm */
    unsigned int alarm_sec = (unsigned int)seconds;
    if (alarm_sec < 1) alarm_sec = 1;
    alarm(alarm_sec);

    /* Block on read - will be interrupted by SIGALRM or actual input */
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);

    /* Cancel alarm */
    alarm(0);
    sigaction(SIGALRM, &old_sa, NULL);

    if (n == 1) return (int)c;
    return -1;
}

/* ======================================================================
 * Main loop
 * ====================================================================== */

int main(int argc, char *argv[]) {
    /* Parse command line */
    if (parse_options(argc, argv) < 0)
        return 1;

    /* Set up signal handler for SIGWINCH */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigwinch_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Clean up terminal on SIGTERM / SIGINT */
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);
    signal(SIGHUP, sigterm_handler);

    /* Setup default field display */
    setup_default_fields();

    /* Detect terminal size */
    detect_term_size();

    /* Enter raw mode for interactive use */
    if (!opt_batch) {
        enter_raw_mode();
    }

    /* Main loop */
    while (1) {
        /* Check for terminal resize */
        if (g_winch) {
            g_winch = 0;
            detect_term_size();
        }

        /* Collect process data */
        collect_processes();

        /* Sort processes */
        sort_processes();

        /* Repaint screen */
        repaint_screen();

        g_iteration++;

        /* Check iteration limit */
        if (opt_iterations > 0 && g_iteration >= opt_iterations)
            break;

        /* Wait for delay or user input */
        int ch = interruptible_delay(opt_delay);
        if (ch >= 0) {
            process_interactive_command(ch);

            /* After processing a command, there might be more input
               (e.g. escape sequences). Give a brief moment to collect. */
        }
    }

    cleanup_and_exit(0);
    return 0;
}
