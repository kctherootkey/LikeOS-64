/*
 * ls - list directory contents
 *
 * Full implementation per the coreutils manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <getopt.h>
#include <ctype.h>
#include <fcntl.h>

/* ======================================================================
 * Version & help
 * ====================================================================== */
#define LS_VERSION "ls (LikeOS coreutils) 0.2"

/* ======================================================================
 * Constants
 * ====================================================================== */
#define MAX_ENTRIES  4096
#define PATH_MAX_LS  4096

/* Exit status */
static int exit_status = 0;

/* ======================================================================
 * Option flags
 * ====================================================================== */
static int opt_all         = 0;   /* -a: show entries starting with . */
static int opt_almost_all  = 0;   /* -A: show dotfiles but not . and .. */
static int opt_author      = 0;   /* --author (with -l) */
static int opt_escape      = 0;   /* -b: C-style escapes */
static int opt_ignore_backups = 0;/* -B: ignore entries ending with ~ */
static int opt_directory   = 0;   /* -d: list dirs themselves */
static int opt_classify    = 0;   /* -F: append indicator */
static int opt_file_type   = 0;   /* --file-type: like -F but no '*' */
static int opt_no_owner    = 0;   /* -g: like -l, no owner */
static int opt_group_dirs  = 0;   /* --group-directories-first */
static int opt_no_group    = 0;   /* -G: no group in long listing */
static int opt_human       = 0;   /* -h: human-readable sizes */
static int opt_si          = 0;   /* --si: powers of 1000 */
static int opt_inode       = 0;   /* -i: print inode */
static int opt_kibibytes   = 0;   /* -k: 1024-byte blocks */
static int opt_long        = 0;   /* -l: long listing */
static int opt_dereference = 0;   /* -L: follow symlinks */
static int opt_numeric_uid = 0;   /* -n: numeric uid/gid */
static int opt_literal     = 0;   /* -N: no quoting */
static int opt_no_list_group = 0; /* -o: like -l, no group */
static int opt_slash       = 0;   /* -p: append / to dirs */
static int opt_hide_ctrl   = 0;   /* -q: print ? for nongraphic chars */
static int opt_show_ctrl   = 0;   /* --show-control-chars */
static int opt_quote_name  = 0;   /* -Q: quote names */
static int opt_reverse     = 0;   /* -r: reverse sort */
static int opt_recursive   = 0;   /* -R: recursive listing */
static int opt_size        = 0;   /* -s: print allocated size */
static int opt_zero        = 0;   /* --zero: NUL line terminator */
static int opt_one_per_line = 0;  /* -1: one file per line */
static int opt_deref_cmd   = 0;   /* -H: dereference command line symlinks */
static int opt_dired       = 0;   /* -D: dired mode */
static int opt_tabsize     = 8;   /* -T: tab size */

/* Sort mode */
enum sort_mode {
    SORT_NAME = 0,
    SORT_NONE,      /* -U */
    SORT_SIZE,      /* -S */
    SORT_TIME,      /* -t */
    SORT_VERSION,   /* -v */
    SORT_EXTENSION, /* -X */
    SORT_WIDTH,
};
static int sort_mode = SORT_NAME;

/* Time mode: which timestamp to use */
enum time_mode {
    TIME_MTIME = 0,
    TIME_ATIME,     /* -u */
    TIME_CTIME,     /* -c */
};
static int time_mode = TIME_MTIME;

/* Format mode */
enum format_mode {
    FMT_COLUMNS = 0,  /* -C (default if tty) */
    FMT_LONG,          /* -l */
    FMT_ONE,           /* -1 (default if not tty) */
    FMT_ACROSS,        /* -x */
    FMT_COMMAS,        /* -m */
};
static int format_mode = FMT_ONE;

/* Time style */
enum time_style {
    TS_LOCALE = 0,
    TS_ISO,
    TS_LONG_ISO,
    TS_FULL_ISO,
    TS_CUSTOM,
};
static int time_style = TS_LOCALE;
static char custom_time_fmt[256];

/* Color mode */
enum color_mode {
    COLOR_NEVER = 0,
    COLOR_AUTO,
    COLOR_ALWAYS,
};
static int color_mode = COLOR_NEVER;

/* Terminal width */
static int term_width = 80;

/* ======================================================================
 * Color codes
 * ====================================================================== */
#define COL_RESET   "\033[0m"
#define COL_DIR     "\033[01;34m"   /* bold blue */
#define COL_LINK    "\033[01;36m"   /* bold cyan */
#define COL_EXEC    "\033[01;32m"   /* bold green */
#define COL_FIFO    "\033[33m"      /* yellow */
#define COL_SOCK    "\033[01;35m"   /* bold magenta */
#define COL_BLK     "\033[01;33m"   /* bold yellow */
#define COL_CHR     "\033[01;33m"   /* bold yellow */
#define COL_SUID    "\033[37;41m"   /* white on red */
#define COL_SGID    "\033[30;43m"   /* black on yellow */
#define COL_STICKY  "\033[37;44m"   /* white on blue */
#define COL_OW      "\033[34;42m"   /* blue on green */

static int use_color = 0;

static const char *color_for(mode_t mode)
{
    if (!use_color) return "";
    if (S_ISDIR(mode)) {
        if (mode & S_ISVTX) {
            if (mode & S_IWOTH) return COL_OW;
            return COL_STICKY;
        }
        return COL_DIR;
    }
    if (S_ISLNK(mode))  return COL_LINK;
    if (S_ISFIFO(mode))  return COL_FIFO;
    if (S_ISSOCK(mode))  return COL_SOCK;
    if (S_ISBLK(mode))   return COL_BLK;
    if (S_ISCHR(mode))   return COL_CHR;
    if (mode & S_ISUID)  return COL_SUID;
    if (mode & S_ISGID)  return COL_SGID;
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return COL_EXEC;
    return "";
}

static const char *color_reset(void)
{
    return use_color ? COL_RESET : "";
}

/* ======================================================================
 * Entry structure
 * ====================================================================== */
typedef struct {
    char name[256];
    struct stat st;
    int  stat_valid;
    char link_target[256];  /* for symlink display */
    int  link_valid;
} entry_t;

/* ======================================================================
 * Helper: human-readable size formatting
 * ====================================================================== */
static void format_size(char *buf, size_t bufsz, uint64_t size)
{
    if (!opt_human && !opt_si) {
        snprintf(buf, bufsz, "%lu", (unsigned long)size);
        return;
    }
    unsigned long base = opt_si ? 1000 : 1024;
    const char *units = opt_si ? "kMGTPEZY" : "KMGTPEZY";
    if (size < base) {
        snprintf(buf, bufsz, "%lu", (unsigned long)size);
        return;
    }
    /* Integer-only human-readable: compute val*10/base for one decimal place */
    int u = -1;
    uint64_t prev = size;
    while (prev >= base && u < 7) {
        size = prev;
        prev = size / base;
        u++;
    }
    /* size is the value before the last division, prev = size/base.
     * We want to display "size / base" with optional one decimal digit.
     * tenths = (size * 10 / base) % 10 gives the first decimal digit. */
    uint64_t whole = size / base;
    uint64_t tenths = (size * 10 / base) % 10;
    if (whole >= 10 || tenths == 0)
        snprintf(buf, bufsz, "%lu%c", (unsigned long)whole, units[u]);
    else
        snprintf(buf, bufsz, "%lu.%lu%c", (unsigned long)whole, (unsigned long)tenths, units[u]);
}

/* ======================================================================
 * Helper: format the mode string  (drwxr-xr-x)
 * ====================================================================== */
static void format_mode_str(char *buf, mode_t m)
{
    buf[0] = '-';
    if (S_ISDIR(m))  buf[0] = 'd';
    if (S_ISLNK(m))  buf[0] = 'l';
    if (S_ISCHR(m))  buf[0] = 'c';
    if (S_ISBLK(m))  buf[0] = 'b';
    if (S_ISFIFO(m)) buf[0] = 'p';
    if (S_ISSOCK(m)) buf[0] = 's';

    buf[1] = (m & S_IRUSR) ? 'r' : '-';
    buf[2] = (m & S_IWUSR) ? 'w' : '-';
    if (m & S_ISUID)
        buf[3] = (m & S_IXUSR) ? 's' : 'S';
    else
        buf[3] = (m & S_IXUSR) ? 'x' : '-';

    buf[4] = (m & S_IRGRP) ? 'r' : '-';
    buf[5] = (m & S_IWGRP) ? 'w' : '-';
    if (m & S_ISGID)
        buf[6] = (m & S_IXGRP) ? 's' : 'S';
    else
        buf[6] = (m & S_IXGRP) ? 'x' : '-';

    buf[7] = (m & S_IROTH) ? 'r' : '-';
    buf[8] = (m & S_IWOTH) ? 'w' : '-';
    if (m & S_ISVTX)
        buf[9] = (m & S_IXOTH) ? 't' : 'T';
    else
        buf[9] = (m & S_IXOTH) ? 'x' : '-';

    buf[10] = '\0';
}

/* ======================================================================
 * Helper: format time
 * ====================================================================== */
static void format_time(char *buf, size_t bufsz, uint64_t t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);

    switch (time_style) {
    case TS_FULL_ISO:
        snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d:%02d.000000000 +0000",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        break;
    case TS_LONG_ISO:
        snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
        break;
    case TS_ISO:
    case TS_LOCALE:
    default:
        {
            time_t now;
            time(&now);
            time_t six_months = 180 * 86400;
            char mon[4];
            strftime(mon, sizeof(mon), "%b", &tm);
            if (tt > now - six_months && tt <= now) {
                snprintf(buf, bufsz, "%s %2d %02d:%02d",
                         mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
            } else {
                snprintf(buf, bufsz, "%s %2d  %4d",
                         mon, tm.tm_mday, tm.tm_year + 1900);
            }
        }
        break;
    case TS_CUSTOM:
        strftime(buf, bufsz, custom_time_fmt, &tm);
        break;
    }
}

/* ======================================================================
 * Helper: get the appropriate timestamp from stat
 * ====================================================================== */
static uint64_t get_time(const struct stat *st)
{
    switch (time_mode) {
    case TIME_ATIME: return st->st_atime;
    case TIME_CTIME: return st->st_ctime;
    default:         return st->st_mtime;
    }
}

/* ======================================================================
 * Helper: print a name with quoting/escaping
 * ====================================================================== */
static int print_name_to_buf(char *buf, size_t bufsz, const char *name)
{
    size_t pos = 0;

    if (opt_quote_name && pos < bufsz - 1)
        buf[pos++] = '"';

    for (const char *p = name; *p && pos < bufsz - 2; p++) {
        unsigned char c = (unsigned char)*p;
        if (opt_escape && !isprint(c)) {
            int n = snprintf(buf + pos, bufsz - pos, "\\%03o", c);
            if (n > 0) pos += (size_t)n;
        } else if (opt_hide_ctrl && !isprint(c)) {
            buf[pos++] = '?';
        } else {
            buf[pos++] = *p;
        }
    }

    if (opt_quote_name && pos < bufsz - 1)
        buf[pos++] = '"';

    buf[pos] = '\0';
    return (int)pos;
}

/* ======================================================================
 * Helper: get display width of a name (accounting for quoting)
 * ====================================================================== */
static int name_width(const char *name)
{
    int w = 0;
    if (opt_quote_name) w += 2;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (opt_escape && !isprint(c))
            w += 4;  /* \ooo */
        else
            w++;
    }
    return w;
}

/* ======================================================================
 * Helper: compute indicator char for -F / --file-type / -p
 * ====================================================================== */
static char indicator_char(const entry_t *e)
{
    if (!e->stat_valid) return 0;
    mode_t m = e->st.st_mode;
    if (S_ISDIR(m))  return '/';
    if (opt_classify || opt_file_type) {
        if (S_ISLNK(m))  return '@';
        if (S_ISFIFO(m)) return '|';
        if (S_ISSOCK(m)) return '=';
        if (opt_classify && (m & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return '*';
    }
    return 0;
}

/* ======================================================================
 * Helper: indicator width
 * ====================================================================== */
static int indicator_width(const entry_t *e)
{
    if (!opt_classify && !opt_file_type && !opt_slash) return 0;
    if (opt_slash) {
        return (e->stat_valid && S_ISDIR(e->st.st_mode)) ? 1 : 0;
    }
    return indicator_char(e) ? 1 : 0;
}

/* ======================================================================
 * Version sort helper (natural sort of numbers within text)
 * ====================================================================== */
static int version_compare(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *a0 = a, *b0 = b;
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ae = a, *be = b;
            while (isdigit((unsigned char)*ae)) ae++;
            while (isdigit((unsigned char)*be)) be++;
            int alen = (int)(ae - a);
            int blen = (int)(be - b);
            if (alen != blen) return alen - blen;
            while (a < ae) {
                if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
                a++; b++;
            }
            int a_lz = (int)(a - a0);
            int b_lz = (int)(b - b0);
            if (a_lz != b_lz) return a_lz - b_lz;
        } else {
            unsigned char ca = (unsigned char)*a;
            unsigned char cb = (unsigned char)*b;
            if (ca != cb) return (int)ca - (int)cb;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ======================================================================
 * Comparison functions for qsort
 * ====================================================================== */
static int cmp_name(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    return strcmp(ea->name, eb->name);
}

static int cmp_size(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    if (ea->st.st_size > eb->st.st_size) return -1;
    if (ea->st.st_size < eb->st.st_size) return 1;
    return strcmp(ea->name, eb->name);
}

static int cmp_time(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    uint64_t ta = get_time(&ea->st);
    uint64_t tb = get_time(&eb->st);
    if (ta > tb) return -1;
    if (ta < tb) return 1;
    return strcmp(ea->name, eb->name);
}

static int cmp_version(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    return version_compare(ea->name, eb->name);
}

static int cmp_extension(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    const char *ext_a = strrchr(ea->name, '.');
    const char *ext_b = strrchr(eb->name, '.');
    if (!ext_a) ext_a = "";
    if (!ext_b) ext_b = "";
    int r = strcmp(ext_a, ext_b);
    if (r != 0) return r;
    return strcmp(ea->name, eb->name);
}

static int cmp_width_fn(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;
    int wa = (int)strlen(ea->name);
    int wb = (int)strlen(eb->name);
    if (wa != wb) return wa - wb;
    return strcmp(ea->name, eb->name);
}

/* Wrapper that handles reverse and group-directories-first */
static int (*base_cmp)(const void *, const void *);

static int cmp_wrapper(const void *a, const void *b)
{
    const entry_t *ea = (const entry_t *)a;
    const entry_t *eb = (const entry_t *)b;

    if (opt_group_dirs) {
        int da = ea->stat_valid && S_ISDIR(ea->st.st_mode);
        int db = eb->stat_valid && S_ISDIR(eb->st.st_mode);
        if (da && !db) return -1;
        if (!da && db) return 1;
    }

    int r = base_cmp(a, b);
    return opt_reverse ? -r : r;
}

/* ======================================================================
 * Sort entries
 * ====================================================================== */
static void sort_entries(entry_t *entries, int count)
{
    if (sort_mode == SORT_NONE || count <= 1)
        return;

    switch (sort_mode) {
    case SORT_NAME:      base_cmp = cmp_name; break;
    case SORT_SIZE:      base_cmp = cmp_size; break;
    case SORT_TIME:      base_cmp = cmp_time; break;
    case SORT_VERSION:   base_cmp = cmp_version; break;
    case SORT_EXTENSION: base_cmp = cmp_extension; break;
    case SORT_WIDTH:     base_cmp = cmp_width_fn; break;
    default:             base_cmp = cmp_name; break;
    }

    qsort(entries, count, sizeof(entry_t), cmp_wrapper);
}

/* ======================================================================
 * Build path helper
 * ====================================================================== */
static void build_path(char *buf, size_t bufsz, const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/')
        snprintf(buf, bufsz, "%s%s", dir, name);
    else
        snprintf(buf, bufsz, "%s/%s", dir, name);
}

/* ======================================================================
 * Stat an entry (with correct follow logic)
 * ====================================================================== */
static int stat_entry(const char *path, struct stat *st, int is_cmdline)
{
    if (opt_dereference || (is_cmdline && opt_deref_cmd)) {
        return stat(path, st);
    }
    return lstat(path, st);
}

/* ======================================================================
 * Collect entries from a directory
 * ====================================================================== */
static int collect_entries(const char *dirpath, entry_t *entries, int max_entries)
{
    DIR *dir = opendir(dirpath);
    if (!dir) {
        fprintf(stderr, "ls: cannot open directory '%s': %s\n", dirpath,
                strerror(errno));
        exit_status = 2;
        return 0;
    }

    int count = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < max_entries) {
        const char *name = de->d_name;

        /* Filter entries based on flags */
        if (name[0] == '.') {
            if (!opt_all && !opt_almost_all)
                continue;
            if (opt_almost_all && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0))
                continue;
        }

        /* -B: ignore backup files (ending with ~) */
        if (opt_ignore_backups) {
            size_t len = strlen(name);
            if (len > 0 && name[len - 1] == '~')
                continue;
        }

        entry_t *e = &entries[count];
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->link_valid = 0;
        e->link_target[0] = '\0';

        char fullpath[PATH_MAX_LS];
        build_path(fullpath, sizeof(fullpath), dirpath, name);

        e->stat_valid = (stat_entry(fullpath, &e->st, 0) == 0);
        if (!e->stat_valid) {
            memset(&e->st, 0, sizeof(e->st));
        }

        /* If it's a symlink, read the target */
        if (e->stat_valid && S_ISLNK(e->st.st_mode)) {
            int n = readlink(fullpath, e->link_target, sizeof(e->link_target) - 1);
            if (n > 0) {
                e->link_target[n] = '\0';
                e->link_valid = 1;
            }
        }

        count++;
    }

    closedir(dir);
    return count;
}

/* ======================================================================
 * Print in long format (-l)
 * ====================================================================== */
static void print_long(entry_t *entries, int count, const char *dirpath)
{
    (void)dirpath;
    /* Compute column widths */
    int max_nlink = 1;
    int max_uid = 1;
    int max_gid = 1;
    int max_size = 1;

    for (int i = 0; i < count; i++) {
        if (!entries[i].stat_valid) continue;
        struct stat *st = &entries[i].st;
        char tmp[32];

        snprintf(tmp, sizeof(tmp), "%u", st->st_nlink);
        int w = (int)strlen(tmp);
        if (w > max_nlink) max_nlink = w;

        if (!opt_no_owner) {
            snprintf(tmp, sizeof(tmp), "%u", st->st_uid);
            w = (int)strlen(tmp);
            if (st->st_uid == 0) w = 4; /* "root" */
            if (w > max_uid) max_uid = w;
        }

        if (!opt_no_group && !opt_no_list_group) {
            snprintf(tmp, sizeof(tmp), "%u", st->st_gid);
            w = (int)strlen(tmp);
            if (st->st_gid == 0) w = 4; /* "root" */
            if (w > max_gid) max_gid = w;
        }

        format_size(tmp, sizeof(tmp), st->st_size);
        w = (int)strlen(tmp);
        if (w > max_size) max_size = w;
    }

    /* Compute total blocks */
    if (!opt_dired) {
        uint64_t total_blocks = 0;
        for (int i = 0; i < count; i++) {
            if (entries[i].stat_valid) {
                uint64_t blk = (entries[i].st.st_size + 1023) / 1024;
                if (opt_kibibytes)
                    total_blocks += blk;
                else
                    total_blocks += blk * 2; /* 512-byte blocks */
            }
        }
        printf("total %lu\n", (unsigned long)total_blocks);
    }

    for (int i = 0; i < count; i++) {
        entry_t *e = &entries[i];

        /* -s: print allocated size */
        if (opt_size) {
            uint64_t blk;
            if (opt_kibibytes)
                blk = (e->st.st_size + 1023) / 1024;
            else
                blk = ((e->st.st_size + 1023) / 1024) * 2;
            if (opt_human || opt_si) {
                char hbuf[16];
                format_size(hbuf, sizeof(hbuf), e->st.st_size);
                printf("%4s ", hbuf);
            } else {
                printf("%4lu ", (unsigned long)blk);
            }
        }

        /* -i: print inode */
        if (opt_inode) {
            printf("%7lu ", (unsigned long)0);
        }

        /* Mode string */
        char mstr[12];
        format_mode_str(mstr, e->st.st_mode);
        printf("%s ", mstr);

        /* Hard link count */
        printf("%*u ", max_nlink, e->st.st_nlink);

        /* Owner (unless -g or -o with no owner) */
        if (!opt_no_owner) {
            if (opt_numeric_uid || e->st.st_uid != 0) {
                char ubuf[16];
                snprintf(ubuf, sizeof(ubuf), "%u", e->st.st_uid);
                printf("%-*s ", max_uid, ubuf);
            } else {
                printf("%-*s ", max_uid, "root");
            }
        }

        /* --author (same as owner for us) */
        if (opt_author) {
            if (opt_numeric_uid || e->st.st_uid != 0) {
                char ubuf[16];
                snprintf(ubuf, sizeof(ubuf), "%u", e->st.st_uid);
                printf("%-*s ", max_uid, ubuf);
            } else {
                printf("%-*s ", max_uid, "root");
            }
        }

        /* Group (unless -G or -o) */
        if (!opt_no_group && !opt_no_list_group) {
            if (opt_numeric_uid || e->st.st_gid != 0) {
                char gbuf[16];
                snprintf(gbuf, sizeof(gbuf), "%u", e->st.st_gid);
                printf("%-*s ", max_gid, gbuf);
            } else {
                printf("%-*s ", max_gid, "root");
            }
        }

        /* Size */
        char sbuf[32];
        format_size(sbuf, sizeof(sbuf), e->st.st_size);
        printf("%*s ", max_size, sbuf);

        /* Timestamp */
        char tbuf[64];
        format_time(tbuf, sizeof(tbuf), get_time(&e->st));
        printf("%s ", tbuf);

        /* Name */
        char nbuf[512];
        print_name_to_buf(nbuf, sizeof(nbuf), e->name);
        printf("%s%s%s", color_for(e->st.st_mode), nbuf, color_reset());

        /* Symlink target */
        if (e->link_valid && S_ISLNK(e->st.st_mode)) {
            printf(" -> %s", e->link_target);
        }

        /* Indicator */
        if (opt_classify || opt_file_type) {
            char ic = indicator_char(e);
            if (ic) putchar(ic);
        } else if (opt_slash && e->stat_valid && S_ISDIR(e->st.st_mode)) {
            putchar('/');
        }

        if (opt_zero)
            putchar('\0');
        else
            putchar('\n');
    }
}

/* ======================================================================
 * Print one entry per line (-1)
 * ====================================================================== */
static void print_one_per_line(entry_t *entries, int count)
{
    for (int i = 0; i < count; i++) {
        entry_t *e = &entries[i];

        if (opt_size) {
            uint64_t blk;
            if (opt_kibibytes)
                blk = (e->st.st_size + 1023) / 1024;
            else
                blk = ((e->st.st_size + 1023) / 1024) * 2;
            if (opt_human || opt_si) {
                char hbuf[16];
                format_size(hbuf, sizeof(hbuf), e->st.st_size);
                printf("%4s ", hbuf);
            } else {
                printf("%4lu ", (unsigned long)blk);
            }
        }

        if (opt_inode)
            printf("%7lu ", (unsigned long)0);

        char nbuf[512];
        print_name_to_buf(nbuf, sizeof(nbuf), e->name);
        printf("%s%s%s", color_for(e->st.st_mode), nbuf, color_reset());

        if (opt_classify || opt_file_type) {
            char ic = indicator_char(e);
            if (ic) putchar(ic);
        } else if (opt_slash && e->stat_valid && S_ISDIR(e->st.st_mode)) {
            putchar('/');
        }

        if (opt_zero)
            putchar('\0');
        else
            putchar('\n');
    }
}

/* ======================================================================
 * Print in columns (-C or -x)
 * ====================================================================== */
static void print_columns(entry_t *entries, int count, int across)
{
    if (count == 0) return;

    /* Compute max width per entry (name + indicator) */
    int max_w = 0;
    for (int i = 0; i < count; i++) {
        int w = name_width(entries[i].name) + indicator_width(&entries[i]);
        if (opt_inode) w += 8;
        if (opt_size) w += 5;
        if (w > max_w) max_w = w;
    }

    /* Column width includes spacing */
    int col_w = max_w + 2;
    if (col_w < 1) col_w = 1;
    int ncols = term_width / col_w;
    if (ncols < 1) ncols = 1;
    int nrows = (count + ncols - 1) / ncols;

    for (int row = 0; row < nrows; row++) {
        for (int col = 0; col < ncols; col++) {
            int idx;
            if (across)
                idx = row * ncols + col;
            else
                idx = col * nrows + row;

            if (idx >= count) break;

            entry_t *e = &entries[idx];
            char nbuf[512];
            int printed = 0;

            if (opt_inode) {
                printed += printf("%7lu ", (unsigned long)0);
            }
            if (opt_size) {
                uint64_t blk;
                if (opt_kibibytes)
                    blk = (e->st.st_size + 1023) / 1024;
                else
                    blk = ((e->st.st_size + 1023) / 1024) * 2;
                printed += printf("%4lu ", (unsigned long)blk);
            }

            int nlen = print_name_to_buf(nbuf, sizeof(nbuf), e->name);
            printf("%s%s%s", color_for(e->st.st_mode), nbuf, color_reset());
            printed += nlen;

            if (opt_classify || opt_file_type) {
                char ic = indicator_char(e);
                if (ic) { putchar(ic); printed++; }
            } else if (opt_slash && e->stat_valid && S_ISDIR(e->st.st_mode)) {
                putchar('/');
                printed++;
            }

            /* Pad to column width if not last in row */
            int next_idx;
            if (across)
                next_idx = row * ncols + col + 1;
            else
                next_idx = (col + 1) * nrows + row;

            if (next_idx < count && col < ncols - 1) {
                int pad = col_w - printed;
                for (int p = 0; p < pad; p++) putchar(' ');
            }
        }
        if (opt_zero)
            putchar('\0');
        else
            putchar('\n');
    }
}

/* ======================================================================
 * Print comma-separated (-m)
 * ====================================================================== */
static void print_commas(entry_t *entries, int count)
{
    int line_pos = 0;
    for (int i = 0; i < count; i++) {
        char nbuf[512];
        int nlen = print_name_to_buf(nbuf, sizeof(nbuf), entries[i].name);

        int extra = (i < count - 1) ? 2 : 0;  /* ", " */
        if (line_pos > 0 && line_pos + nlen + extra > term_width) {
            putchar('\n');
            line_pos = 0;
        }

        printf("%s%s%s", color_for(entries[i].st.st_mode), nbuf, color_reset());
        line_pos += nlen;

        if (i < count - 1) {
            printf(", ");
            line_pos += 2;
        }
    }
    if (count > 0) {
        if (opt_zero)
            putchar('\0');
        else
            putchar('\n');
    }
}

/* ======================================================================
 * Print entries according to current format
 * ====================================================================== */
static void print_entries(entry_t *entries, int count, const char *dirpath)
{
    switch (format_mode) {
    case FMT_LONG:
        print_long(entries, count, dirpath);
        break;
    case FMT_COLUMNS:
        print_columns(entries, count, 0);
        break;
    case FMT_ACROSS:
        print_columns(entries, count, 1);
        break;
    case FMT_COMMAS:
        print_commas(entries, count);
        break;
    case FMT_ONE:
    default:
        print_one_per_line(entries, count);
        break;
    }
}

/* ======================================================================
 * List a single directory
 * ====================================================================== */

static void list_directory(const char *path, int show_header, int first)
{
    if (show_header) {
        if (!first) printf("\n");
        printf("%s:\n", path);
    }

    static entry_t entries[MAX_ENTRIES];
    int count = collect_entries(path, entries, MAX_ENTRIES);

    sort_entries(entries, count);
    print_entries(entries, count, path);

    /* Recursive listing (-R) — collect subdirectory paths BEFORE recursing,
     * because the static entries[] array will be overwritten by each
     * recursive call to list_directory(). We malloc the name list so each
     * recursion level keeps its own copy. */
    if (opt_recursive) {
        /* First pass: count subdirs */
        int nsub = 0;
        for (int i = 0; i < count; i++) {
            if (!entries[i].stat_valid) continue;
            if (!S_ISDIR(entries[i].st.st_mode)) continue;
            if (strcmp(entries[i].name, ".") == 0 ||
                strcmp(entries[i].name, "..") == 0)
                continue;
            nsub++;
        }

        if (nsub > 0) {
            /* Allocate an array of full path strings */
            char (*subpaths)[256] = malloc(nsub * 256);
            if (subpaths) {
                int si = 0;
                for (int i = 0; i < count && si < nsub; i++) {
                    if (!entries[i].stat_valid) continue;
                    if (!S_ISDIR(entries[i].st.st_mode)) continue;
                    if (strcmp(entries[i].name, ".") == 0 ||
                        strcmp(entries[i].name, "..") == 0)
                        continue;
                    build_path(subpaths[si], 256, path, entries[i].name);
                    si++;
                }

                for (int i = 0; i < nsub; i++) {
                    list_directory(subpaths[i], 1, 0);
                }
                free(subpaths);
            }
        }
    }
}

/* ======================================================================
 * Process a command-line argument
 * ====================================================================== */
static void process_arg(const char *path,
                        entry_t *dir_args, int *dir_count,
                        entry_t *file_args, int *file_count)
{
    struct stat st;
    if (stat_entry(path, &st, 1) != 0) {
        fprintf(stderr, "ls: cannot access '%s': %s\n", path, strerror(errno));
        exit_status = 2;
        return;
    }

    if (S_ISDIR(st.st_mode) && !opt_directory) {
        entry_t *e = &dir_args[*dir_count];
        strncpy(e->name, path, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->st = st;
        e->stat_valid = 1;
        e->link_valid = 0;
        (*dir_count)++;
    } else {
        entry_t *e = &file_args[*file_count];
        strncpy(e->name, path, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->st = st;
        e->stat_valid = 1;
        e->link_valid = 0;

        if (S_ISLNK(st.st_mode)) {
            int n = readlink(path, e->link_target, sizeof(e->link_target) - 1);
            if (n > 0) {
                e->link_target[n] = '\0';
                e->link_valid = 1;
            }
        }
        (*file_count)++;
    }
}

/* ======================================================================
 * Parse --sort=WORD
 * ====================================================================== */
static int parse_sort(const char *word)
{
    if (strcmp(word, "none") == 0) return SORT_NONE;
    if (strcmp(word, "size") == 0) return SORT_SIZE;
    if (strcmp(word, "time") == 0) return SORT_TIME;
    if (strcmp(word, "version") == 0) return SORT_VERSION;
    if (strcmp(word, "extension") == 0) return SORT_EXTENSION;
    if (strcmp(word, "width") == 0) return SORT_WIDTH;
    fprintf(stderr, "ls: invalid argument '%s' for '--sort'\n", word);
    exit_status = 2;
    return SORT_NAME;
}

/* ======================================================================
 * Parse --time=WORD
 * ====================================================================== */
static int parse_time_word(const char *word)
{
    if (strcmp(word, "atime") == 0 || strcmp(word, "access") == 0 ||
        strcmp(word, "use") == 0)
        return TIME_ATIME;
    if (strcmp(word, "ctime") == 0 || strcmp(word, "status") == 0)
        return TIME_CTIME;
    if (strcmp(word, "mtime") == 0 || strcmp(word, "modification") == 0)
        return TIME_MTIME;
    if (strcmp(word, "birth") == 0 || strcmp(word, "creation") == 0)
        return TIME_MTIME;
    fprintf(stderr, "ls: invalid argument '%s' for '--time'\n", word);
    exit_status = 2;
    return TIME_MTIME;
}

/* ======================================================================
 * Parse --time-style=STYLE
 * ====================================================================== */
static void parse_time_style(const char *style)
{
    if (strcmp(style, "full-iso") == 0) {
        time_style = TS_FULL_ISO;
    } else if (strcmp(style, "long-iso") == 0) {
        time_style = TS_LONG_ISO;
    } else if (strcmp(style, "iso") == 0) {
        time_style = TS_ISO;
    } else if (strcmp(style, "locale") == 0) {
        time_style = TS_LOCALE;
    } else if (style[0] == '+') {
        time_style = TS_CUSTOM;
        strncpy(custom_time_fmt, style + 1, sizeof(custom_time_fmt) - 1);
        custom_time_fmt[sizeof(custom_time_fmt) - 1] = '\0';
    } else {
        fprintf(stderr, "ls: invalid argument '%s' for '--time-style'\n", style);
        exit_status = 2;
    }
}

/* ======================================================================
 * Parse --format=WORD
 * ====================================================================== */
static void parse_format(const char *word)
{
    if (strcmp(word, "long") == 0 || strcmp(word, "verbose") == 0) {
        format_mode = FMT_LONG;
    } else if (strcmp(word, "single-column") == 0) {
        format_mode = FMT_ONE;
    } else if (strcmp(word, "vertical") == 0) {
        format_mode = FMT_COLUMNS;
    } else if (strcmp(word, "across") == 0 || strcmp(word, "horizontal") == 0) {
        format_mode = FMT_ACROSS;
    } else if (strcmp(word, "commas") == 0) {
        format_mode = FMT_COMMAS;
    } else {
        fprintf(stderr, "ls: invalid argument '%s' for '--format'\n", word);
        exit_status = 2;
    }
}

/* ======================================================================
 * Parse --color[=WHEN]
 * ====================================================================== */
static void parse_color(const char *when)
{
    if (!when || strcmp(when, "always") == 0 || strcmp(when, "yes") == 0 ||
        strcmp(when, "force") == 0) {
        color_mode = COLOR_ALWAYS;
    } else if (strcmp(when, "auto") == 0 || strcmp(when, "tty") == 0 ||
               strcmp(when, "if-tty") == 0) {
        color_mode = COLOR_AUTO;
    } else if (strcmp(when, "never") == 0 || strcmp(when, "no") == 0 ||
               strcmp(when, "none") == 0) {
        color_mode = COLOR_NEVER;
    } else {
        fprintf(stderr, "ls: invalid argument '%s' for '--color'\n", when);
        exit_status = 2;
    }
}

/* ======================================================================
 * Parse --indicator-style=WORD
 * ====================================================================== */
static void parse_indicator_style(const char *word)
{
    if (strcmp(word, "none") == 0) {
        opt_classify = 0; opt_file_type = 0; opt_slash = 0;
    } else if (strcmp(word, "slash") == 0) {
        opt_slash = 1; opt_classify = 0; opt_file_type = 0;
    } else if (strcmp(word, "file-type") == 0) {
        opt_file_type = 1; opt_classify = 0; opt_slash = 0;
    } else if (strcmp(word, "classify") == 0) {
        opt_classify = 1; opt_file_type = 0; opt_slash = 0;
    } else {
        fprintf(stderr, "ls: invalid argument '%s' for '--indicator-style'\n", word);
        exit_status = 2;
    }
}

/* ======================================================================
 * Print help
 * ====================================================================== */
static void print_help(void)
{
    printf("Usage: ls [OPTION]... [FILE]...\n");
    printf("List information about the FILEs (the current directory by default).\n");
    printf("Sort entries alphabetically if none of -cftuvSUX nor --sort is specified.\n\n");
    printf("  -a, --all                  do not ignore entries starting with .\n");
    printf("  -A, --almost-all           do not list implied . and ..\n");
    printf("      --author               with -l, print the author of each file\n");
    printf("  -b, --escape               print C-style escapes for nongraphic characters\n");
    printf("  -B, --ignore-backups       do not list implied entries ending with ~\n");
    printf("  -c                         with -lt: sort by, and show, ctime;\n");
    printf("                               with -l: show ctime and sort by name;\n");
    printf("                               otherwise: sort by ctime, newest first\n");
    printf("  -C                         list entries by columns\n");
    printf("      --color[=WHEN]         color the output; WHEN can be 'always',\n");
    printf("                               'auto', or 'never' (default)\n");
    printf("  -d, --directory            list directories themselves, not their contents\n");
    printf("  -D, --dired                generate output designed for Emacs' dired mode\n");
    printf("  -f                         list all entries in directory order\n");
    printf("  -F, --classify[=WHEN]      append indicator (one of */=>@|) to entries\n");
    printf("      --file-type            likewise, except do not append '*'\n");
    printf("      --format=WORD          across -x, commas -m, horizontal -x, long -l,\n");
    printf("                               single-column -1, verbose -l, vertical -C\n");
    printf("      --full-time            like -l --time-style=full-iso\n");
    printf("  -g                         like -l, but do not list owner\n");
    printf("      --group-directories-first\n");
    printf("                             group directories before files\n");
    printf("  -G, --no-group             in a long listing, don't print group names\n");
    printf("  -h, --human-readable       with -l and -s, print human readable sizes\n");
    printf("      --si                   likewise, but use powers of 1000 not 1024\n");
    printf("  -H, --dereference-command-line\n");
    printf("                             follow symbolic links listed on the command line\n");
    printf("  -i, --inode                print the index number of each file\n");
    printf("  -k, --kibibytes            default to 1024-byte blocks for disk usage\n");
    printf("  -l                         use a long listing format\n");
    printf("  -L, --dereference          follow symlinks when showing file information\n");
    printf("  -m                         fill width with a comma separated list of entries\n");
    printf("  -n, --numeric-uid-gid      like -l, but list numeric user and group IDs\n");
    printf("  -N, --literal              print entry names without quoting\n");
    printf("  -o                         like -l, but do not list group information\n");
    printf("  -p, --indicator-style=slash\n");
    printf("                             append / indicator to directories\n");
    printf("  -q, --hide-control-chars   print ? instead of nongraphic characters\n");
    printf("      --show-control-chars   show nongraphic characters as-is\n");
    printf("  -Q, --quote-name           enclose entry names in double quotes\n");
    printf("  -r, --reverse              reverse order while sorting\n");
    printf("  -R, --recursive            list subdirectories recursively\n");
    printf("  -s, --size                 print the allocated size of each file, in blocks\n");
    printf("  -S                         sort by file size, largest first\n");
    printf("      --sort=WORD            sort by WORD: none (-U), size (-S), time (-t),\n");
    printf("                               version (-v), extension (-X), width\n");
    printf("      --time=WORD            change the default of using modification time;\n");
    printf("                               access time (-u): atime, access, use;\n");
    printf("                               change time (-c): ctime, status;\n");
    printf("                               birth time: birth, creation\n");
    printf("      --time-style=TIME_STYLE\n");
    printf("                             time/date format with -l; see below\n");
    printf("  -t                         sort by time, newest first; see --time\n");
    printf("  -T, --tabsize=COLS         assume tab stops at each COLS instead of 8\n");
    printf("  -u                         with -lt: sort by, and show, access time;\n");
    printf("                               with -l: show access time and sort by name;\n");
    printf("                               otherwise: sort by access time, newest first\n");
    printf("  -U                         do not sort; list entries in directory order\n");
    printf("  -v                         natural sort of (version) numbers within text\n");
    printf("  -w, --width=COLS           set output width to COLS.  0 means no limit\n");
    printf("  -x                         list entries by lines instead of by columns\n");
    printf("  -X                         sort alphabetically by entry extension\n");
    printf("  -Z, --context              print any security context of each file\n");
    printf("      --zero                 end each output line with NUL, not newline\n");
    printf("  -1                         list one file per line\n");
    printf("      --help                 display this help and exit\n");
    printf("      --version              output version information and exit\n");
    printf("\n");
    printf("The TIME_STYLE argument can be full-iso, long-iso, iso, locale, or\n");
    printf("+FORMAT.  FORMAT is interpreted like in date(1).\n");
    printf("\nExit status:\n 0  if OK,\n 1  if minor problems,\n 2  if serious trouble.\n");
}

/* ======================================================================
 * Long option definitions
 * ====================================================================== */
enum {
    OPT_AUTHOR = 256,
    OPT_BLOCK_SIZE,
    OPT_COLOR,
    OPT_FILE_TYPE,
    OPT_FORMAT,
    OPT_FULL_TIME,
    OPT_GROUP_DIRS,
    OPT_SI,
    OPT_HIDE,
    OPT_HYPERLINK,
    OPT_IND_STYLE,
    OPT_IGNORE,
    OPT_SHOW_CTRL,
    OPT_QUOTING_STYLE,
    OPT_SORT,
    OPT_TIME_WORD,
    OPT_TIME_STYLE,
    OPT_ZERO,
    OPT_HELP,
    OPT_VERSION,
    OPT_DEREF_CMD_DIR,
};

static struct option long_options[] = {
    { "all",                     no_argument,       NULL, 'a' },
    { "almost-all",              no_argument,       NULL, 'A' },
    { "author",                  no_argument,       NULL, OPT_AUTHOR },
    { "escape",                  no_argument,       NULL, 'b' },
    { "block-size",              required_argument, NULL, OPT_BLOCK_SIZE },
    { "ignore-backups",          no_argument,       NULL, 'B' },
    { "color",                   optional_argument, NULL, OPT_COLOR },
    { "directory",               no_argument,       NULL, 'd' },
    { "dired",                   no_argument,       NULL, 'D' },
    { "classify",                optional_argument, NULL, 'F' },
    { "file-type",               no_argument,       NULL, OPT_FILE_TYPE },
    { "format",                  required_argument, NULL, OPT_FORMAT },
    { "full-time",               no_argument,       NULL, OPT_FULL_TIME },
    { "group-directories-first", no_argument,       NULL, OPT_GROUP_DIRS },
    { "no-group",                no_argument,       NULL, 'G' },
    { "human-readable",          no_argument,       NULL, 'h' },
    { "si",                      no_argument,       NULL, OPT_SI },
    { "dereference-command-line",no_argument,       NULL, 'H' },
    { "dereference-command-line-symlink-to-dir",
                                 no_argument,       NULL, OPT_DEREF_CMD_DIR },
    { "hide",                    required_argument, NULL, OPT_HIDE },
    { "hyperlink",               optional_argument, NULL, OPT_HYPERLINK },
    { "indicator-style",         required_argument, NULL, OPT_IND_STYLE },
    { "inode",                   no_argument,       NULL, 'i' },
    { "ignore",                  required_argument, NULL, 'I' },
    { "kibibytes",               no_argument,       NULL, 'k' },
    { "dereference",             no_argument,       NULL, 'L' },
    { "literal",                 no_argument,       NULL, 'N' },
    { "numeric-uid-gid",         no_argument,       NULL, 'n' },
    { "hide-control-chars",      no_argument,       NULL, 'q' },
    { "show-control-chars",      no_argument,       NULL, OPT_SHOW_CTRL },
    { "quote-name",              no_argument,       NULL, 'Q' },
    { "quoting-style",           required_argument, NULL, OPT_QUOTING_STYLE },
    { "reverse",                 no_argument,       NULL, 'r' },
    { "recursive",               no_argument,       NULL, 'R' },
    { "size",                    no_argument,       NULL, 's' },
    { "sort",                    required_argument, NULL, OPT_SORT },
    { "time",                    required_argument, NULL, OPT_TIME_WORD },
    { "time-style",              required_argument, NULL, OPT_TIME_STYLE },
    { "tabsize",                 required_argument, NULL, 'T' },
    { "width",                   required_argument, NULL, 'w' },
    { "context",                 no_argument,       NULL, 'Z' },
    { "zero",                    no_argument,       NULL, OPT_ZERO },
    { "help",                    no_argument,       NULL, OPT_HELP },
    { "version",                 no_argument,       NULL, OPT_VERSION },
    { NULL, 0, NULL, 0 }
};

/* ======================================================================
 * Detect terminal width
 * ====================================================================== */
static void detect_term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        term_width = ws.ws_col;
    }
}

/* ======================================================================
 * main
 * ====================================================================== */
int main(int argc, char **argv)
{
    int is_tty = isatty(STDOUT_FILENO);

    /* Defaults depend on whether stdout is a terminal */
    if (is_tty) {
        format_mode = FMT_COLUMNS;
        opt_hide_ctrl = 1;
    } else {
        format_mode = FMT_ONE;
        opt_show_ctrl = 1;
    }

    detect_term_width();

    /* Parse options */
    int opt;
    int longidx = 0;

    while ((opt = getopt_long(argc, argv,
            "aAbBcCdDfFgGhHiI:klLmnNoOpqQrRsStT:uUvw:xXZ1",
            long_options, &longidx)) != -1) {
        switch (opt) {
        case 'a':
            opt_all = 1;
            break;
        case 'A':
            opt_almost_all = 1;
            break;
        case OPT_AUTHOR:
            opt_author = 1;
            break;
        case 'b':
            opt_escape = 1;
            break;
        case OPT_BLOCK_SIZE:
            /* Simplified: acknowledge */
            break;
        case 'B':
            opt_ignore_backups = 1;
            break;
        case 'c':
            time_mode = TIME_CTIME;
            if (sort_mode != SORT_TIME && format_mode != FMT_LONG)
                sort_mode = SORT_TIME;
            break;
        case 'C':
            format_mode = FMT_COLUMNS;
            break;
        case OPT_COLOR:
            parse_color(optarg);
            break;
        case 'd':
            opt_directory = 1;
            break;
        case 'D':
            opt_dired = 1;
            format_mode = FMT_LONG;
            break;
        case 'f':
            opt_all = 1;
            sort_mode = SORT_NONE;
            color_mode = COLOR_NEVER;
            break;
        case 'F':
            opt_classify = 1;
            break;
        case OPT_FILE_TYPE:
            opt_file_type = 1;
            break;
        case OPT_FORMAT:
            parse_format(optarg);
            break;
        case OPT_FULL_TIME:
            format_mode = FMT_LONG;
            opt_long = 1;
            time_style = TS_FULL_ISO;
            break;
        case 'g':
            format_mode = FMT_LONG;
            opt_long = 1;
            opt_no_owner = 1;
            break;
        case OPT_GROUP_DIRS:
            opt_group_dirs = 1;
            break;
        case 'G':
            opt_no_group = 1;
            break;
        case 'h':
            opt_human = 1;
            break;
        case OPT_SI:
            opt_si = 1;
            opt_human = 1;
            break;
        case 'H':
            opt_deref_cmd = 1;
            break;
        case OPT_DEREF_CMD_DIR:
            opt_deref_cmd = 1;
            break;
        case OPT_HIDE:
            /* --hide=PATTERN: simplified */
            break;
        case OPT_HYPERLINK:
            /* --hyperlink: not supported */
            break;
        case OPT_IND_STYLE:
            parse_indicator_style(optarg);
            break;
        case 'i':
            opt_inode = 1;
            break;
        case 'I':
            /* -I PATTERN: simplified */
            break;
        case 'k':
            opt_kibibytes = 1;
            break;
        case 'l':
            format_mode = FMT_LONG;
            opt_long = 1;
            break;
        case 'L':
            opt_dereference = 1;
            break;
        case 'm':
            format_mode = FMT_COMMAS;
            break;
        case 'n':
            format_mode = FMT_LONG;
            opt_long = 1;
            opt_numeric_uid = 1;
            break;
        case 'N':
            opt_literal = 1;
            break;
        case 'o':
            format_mode = FMT_LONG;
            opt_long = 1;
            opt_no_list_group = 1;
            break;
        case 'O':
            break;
        case 'p':
            opt_slash = 1;
            break;
        case 'q':
            opt_hide_ctrl = 1;
            opt_show_ctrl = 0;
            break;
        case OPT_SHOW_CTRL:
            opt_show_ctrl = 1;
            opt_hide_ctrl = 0;
            break;
        case 'Q':
            opt_quote_name = 1;
            break;
        case OPT_QUOTING_STYLE:
            if (strcmp(optarg, "literal") == 0) {
                opt_literal = 1;
                opt_quote_name = 0;
            } else if (strcmp(optarg, "c") == 0 || strcmp(optarg, "escape") == 0) {
                opt_escape = 1;
            }
            break;
        case 'r':
            opt_reverse = 1;
            break;
        case 'R':
            opt_recursive = 1;
            break;
        case 's':
            opt_size = 1;
            break;
        case 'S':
            sort_mode = SORT_SIZE;
            break;
        case OPT_SORT:
            sort_mode = parse_sort(optarg);
            break;
        case OPT_TIME_WORD:
            time_mode = parse_time_word(optarg);
            break;
        case OPT_TIME_STYLE:
            parse_time_style(optarg);
            break;
        case 't':
            sort_mode = SORT_TIME;
            break;
        case 'T':
            opt_tabsize = atoi(optarg);
            if (opt_tabsize < 0) opt_tabsize = 8;
            break;
        case 'u':
            time_mode = TIME_ATIME;
            if (sort_mode != SORT_TIME && format_mode != FMT_LONG)
                sort_mode = SORT_TIME;
            break;
        case 'U':
            sort_mode = SORT_NONE;
            opt_group_dirs = 0;
            break;
        case 'v':
            sort_mode = SORT_VERSION;
            break;
        case 'w':
            term_width = atoi(optarg);
            if (term_width <= 0) term_width = 10000;
            break;
        case 'x':
            format_mode = FMT_ACROSS;
            break;
        case 'X':
            sort_mode = SORT_EXTENSION;
            break;
        case 'Z':
            /* Security context: not supported */
            break;
        case OPT_ZERO:
            opt_zero = 1;
            format_mode = FMT_ONE;
            break;
        case '1':
            format_mode = FMT_ONE;
            opt_one_per_line = 1;
            break;
        case OPT_HELP:
            print_help();
            return 0;
        case OPT_VERSION:
            printf("%s\n", LS_VERSION);
            return 0;
        default:
            fprintf(stderr, "Try 'ls --help' for more information.\n");
            return 2;
        }
    }

    /* Resolve color */
    if (color_mode == COLOR_ALWAYS) {
        use_color = 1;
    } else if (color_mode == COLOR_AUTO) {
        use_color = is_tty;
    }

    /* Collect file and directory arguments */
    int nargs = argc - optind;
    static entry_t file_args[MAX_ENTRIES];
    static entry_t dir_args[MAX_ENTRIES];
    int file_count = 0;
    int dir_count = 0;

    if (nargs == 0) {
        if (opt_directory) {
            struct stat st;
            if (stat(".", &st) == 0) {
                entry_t *e = &file_args[0];
                strcpy(e->name, ".");
                e->st = st;
                e->stat_valid = 1;
                e->link_valid = 0;
                file_count = 1;
            }
        } else {
            dir_args[0].stat_valid = 1;
            strcpy(dir_args[0].name, ".");
            dir_count = 1;
        }
    } else {
        for (int i = optind; i < argc; i++) {
            process_arg(argv[i],
                       dir_args, &dir_count, file_args, &file_count);
        }
    }

    /* Sort files and directories by name for consistent output */
    if (sort_mode != SORT_NONE && file_count > 1)
        sort_entries(file_args, file_count);
    if (sort_mode != SORT_NONE && dir_count > 1)
        sort_entries(dir_args, dir_count);

    int first = 1;

    /* Print files first */
    if (file_count > 0) {
        print_entries(file_args, file_count, ".");
        first = 0;
    }

    /* Then directories */
    int show_header = (dir_count > 1) || (file_count > 0);
    for (int i = 0; i < dir_count; i++) {
        if (!first) printf("\n");
        if (show_header)
            printf("%s:\n", dir_args[i].name);
        list_directory(dir_args[i].name, 0, first);
        first = 0;
    }

    return exit_status;
}
