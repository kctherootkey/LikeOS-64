/*
 * find - search for files in a directory hierarchy
 *
 * Supports:
 *   Starting points (paths)
 *   Global options: -maxdepth, -mindepth, -depth, -xdev, -help, -version
 *   Tests:  -name, -iname, -path, -ipath, -regex, -type, -size, -empty,
 *           -perm, -newer, -mtime, -atime, -ctime, -mmin, -amin, -cmin,
 *           -links, -readable, -writable, -executable, -true, -false
 *   Actions: -print, -print0, -printf, -exec, -execdir, -delete, -ls, -prune, -quit
 *   Operators: !, -not, -a, -and, -o, -or, (, )
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fnmatch.h>
#include <unistd.h>
#include <ctype.h>

/* ================================================================
 * Expression tree
 * ================================================================ */
enum expr_type {
    /* Tests */
    E_NAME, E_INAME, E_PATH, E_IPATH,
    E_TYPE, E_SIZE, E_EMPTY, E_PERM,
    E_NEWER, E_MTIME, E_ATIME, E_CTIME,
    E_MMIN, E_AMIN, E_CMIN,
    E_LINKS,
    E_READABLE, E_WRITABLE, E_EXECUTABLE,
    E_TRUE, E_FALSE,
    /* Actions */
    E_PRINT, E_PRINT0, E_PRINTF, E_EXEC, E_EXECDIR,
    E_DELETE, E_LS, E_PRUNE, E_QUIT,
    /* Operators */
    E_AND, E_OR, E_NOT,
};

/* Size comparison */
enum cmp_type { CMP_EXACT, CMP_LESS, CMP_GREATER };

struct expr {
    enum expr_type type;
    /* For tests with string arg */
    const char *sval;
    /* For numeric comparison (-size, -mtime, -links, etc) */
    long nval;
    enum cmp_type cmp;
    /* For -type */
    int type_mask;  /* DT_xxx */
    /* For -perm */
    unsigned perm_val;
    int perm_mode;  /* 0 = exact, 1 = all bits (-perm -xxx), 2 = any bit (-perm /xxx) */
    /* For -exec / -execdir */
    char **exec_argv;
    int exec_argc;
    int exec_has_plus; /* {} + vs {} ; */
    /* For -printf */
    const char *fmt;
    /* Children for AND/OR/NOT */
    struct expr *left;
    struct expr *right;
};

/* ================================================================
 * Globals
 * ================================================================ */
static int g_maxdepth = -1;    /* -1 = unlimited */
static int g_mindepth = 0;
static int g_depth_first = 0;  /* -depth flag */
static int g_xdev = 0;         /* -xdev flag (not really used, single FS) */
static int g_quit = 0;         /* set by -quit action */

/* Time reference (for -mtime etc) */
static unsigned long g_now = 0;

/* Default action: -print if no explicit action */
static int g_has_action = 0;

/* Return value */
static int g_retval = 0;

/* ================================================================
 * Expression node allocation
 * ================================================================ */
#define MAX_EXPRS 512
static struct expr g_expr_pool[MAX_EXPRS];
static int g_expr_count = 0;

static struct expr *alloc_expr(void)
{
    if (g_expr_count >= MAX_EXPRS) {
        fprintf(stderr, "find: expression too complex\n");
        exit(1);
    }
    struct expr *e = &g_expr_pool[g_expr_count++];
    memset(e, 0, sizeof(*e));
    return e;
}

/* ================================================================
 * Parse numeric argument: +n, -n, n
 * ================================================================ */
static int parse_num(const char *s, long *val, enum cmp_type *cmp)
{
    if (!s || !*s) return -1;
    *cmp = CMP_EXACT;
    if (*s == '+') { *cmp = CMP_GREATER; s++; }
    else if (*s == '-') { *cmp = CMP_LESS; s++; }
    char *end;
    *val = strtol(s, &end, 10);
    if (*end != '\0') return -1;
    return 0;
}

static int num_match(long actual, long target, enum cmp_type cmp)
{
    switch (cmp) {
    case CMP_EXACT:   return actual == target;
    case CMP_LESS:    return actual < target;
    case CMP_GREATER: return actual > target;
    }
    return 0;
}

/* ================================================================
 * Parse -size argument: [+|-]n[bcwkMG]
 * Returns size in bytes for comparison
 * ================================================================ */
static int parse_size(const char *s, long *val, enum cmp_type *cmp)
{
    if (!s || !*s) return -1;
    *cmp = CMP_EXACT;
    if (*s == '+') { *cmp = CMP_GREATER; s++; }
    else if (*s == '-') { *cmp = CMP_LESS; s++; }

    char *end;
    long n = strtol(s, &end, 10);
    long unit = 512; /* default is 512-byte blocks */
    if (*end) {
        switch (*end) {
        case 'b': unit = 512; end++; break;
        case 'c': unit = 1; end++; break;
        case 'w': unit = 2; end++; break;
        case 'k': unit = 1024; end++; break;
        case 'M': unit = 1024*1024; end++; break;
        case 'G': unit = 1024*1024*1024L; end++; break;
        default: return -1;
        }
    }
    if (*end != '\0') return -1;
    *val = n * unit;
    return 0;
}

/* ================================================================
 * Parse -perm argument: mode, -mode, /mode
 * ================================================================ */
static int parse_perm(const char *s, unsigned *val, int *mode)
{
    if (!s || !*s) return -1;
    *mode = 0;
    if (*s == '-') { *mode = 1; s++; }
    else if (*s == '/') { *mode = 2; s++; }

    char *end;
    *val = (unsigned)strtol(s, &end, 8);
    if (*end != '\0') return -1;
    return 0;
}

/* ================================================================
 * Parse -type argument: b,c,d,p,f,l,s
 * ================================================================ */
static int parse_type(const char *s)
{
    if (!s || !*s) return -1;
    switch (*s) {
    case 'b': return DT_BLK;
    case 'c': return DT_CHR;
    case 'd': return DT_DIR;
    case 'p': return DT_FIFO;
    case 'f': return DT_REG;
    case 'l': return DT_LNK;
    case 's': return DT_SOCK;
    default:  return -1;
    }
}

/* ================================================================
 * Parse expression from argv
 * ================================================================ */
static int g_parse_i;
static int g_parse_argc;
static char **g_parse_argv;

static struct expr *parse_or(void);

static const char *next_arg(void)
{
    if (g_parse_i >= g_parse_argc) return NULL;
    return g_parse_argv[g_parse_i];
}

static const char *consume_arg(void)
{
    if (g_parse_i >= g_parse_argc) return NULL;
    return g_parse_argv[g_parse_i++];
}

static struct expr *parse_primary(void)
{
    const char *tok = next_arg();
    if (!tok) return NULL;

    /* Parenthesized expression */
    if (strcmp(tok, "(") == 0) {
        consume_arg();
        struct expr *e = parse_or();
        if (!e) {
            fprintf(stderr, "find: empty expression after '('\n");
            exit(1);
        }
        tok = consume_arg();
        if (!tok || strcmp(tok, ")") != 0) {
            fprintf(stderr, "find: missing ')'\n");
            exit(1);
        }
        return e;
    }

    /* NOT */
    if (strcmp(tok, "!") == 0 || strcmp(tok, "-not") == 0) {
        consume_arg();
        struct expr *child = parse_primary();
        if (!child) {
            fprintf(stderr, "find: expected expression after '%s'\n", tok);
            exit(1);
        }
        struct expr *e = alloc_expr();
        e->type = E_NOT;
        e->left = child;
        return e;
    }

    /* Tests and actions */
    if (tok[0] != '-')
        return NULL; /* Not a predicate - must be end of expression */

    /* -name PATTERN */
    if (strcmp(tok, "-name") == 0) {
        consume_arg();
        const char *pat = consume_arg();
        if (!pat) { fprintf(stderr, "find: -name requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_NAME;
        e->sval = pat;
        return e;
    }
    if (strcmp(tok, "-iname") == 0) {
        consume_arg();
        const char *pat = consume_arg();
        if (!pat) { fprintf(stderr, "find: -iname requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_INAME;
        e->sval = pat;
        return e;
    }
    if (strcmp(tok, "-path") == 0 || strcmp(tok, "-wholename") == 0) {
        consume_arg();
        const char *pat = consume_arg();
        if (!pat) { fprintf(stderr, "find: -path requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_PATH;
        e->sval = pat;
        return e;
    }
    if (strcmp(tok, "-ipath") == 0 || strcmp(tok, "-iwholename") == 0) {
        consume_arg();
        const char *pat = consume_arg();
        if (!pat) { fprintf(stderr, "find: -ipath requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_IPATH;
        e->sval = pat;
        return e;
    }

    /* -type c */
    if (strcmp(tok, "-type") == 0) {
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: -type requires argument\n"); exit(1); }
        int t = parse_type(arg);
        if (t < 0) { fprintf(stderr, "find: unknown type '%s'\n", arg); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_TYPE;
        e->type_mask = t;
        return e;
    }

    /* -size n[bcwkMG] */
    if (strcmp(tok, "-size") == 0) {
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: -size requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_SIZE;
        if (parse_size(arg, &e->nval, &e->cmp) < 0) {
            fprintf(stderr, "find: invalid size '%s'\n", arg);
            exit(1);
        }
        return e;
    }

    /* -empty */
    if (strcmp(tok, "-empty") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_EMPTY;
        return e;
    }

    /* -perm [-/]mode */
    if (strcmp(tok, "-perm") == 0) {
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: -perm requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_PERM;
        if (parse_perm(arg, &e->perm_val, &e->perm_mode) < 0) {
            fprintf(stderr, "find: invalid mode '%s'\n", arg);
            exit(1);
        }
        return e;
    }

    /* -newer FILE */
    if (strcmp(tok, "-newer") == 0) {
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: -newer requires argument\n"); exit(1); }
        struct stat st;
        if (stat(arg, &st) != 0) {
            fprintf(stderr, "find: cannot stat '%s': %s\n", arg, strerror(errno));
            exit(1);
        }
        struct expr *e = alloc_expr();
        e->type = E_NEWER;
        e->nval = (long)st.st_mtime;
        return e;
    }

    /* -mtime, -atime, -ctime (days) */
    if (strcmp(tok, "-mtime") == 0 || strcmp(tok, "-atime") == 0 ||
        strcmp(tok, "-ctime") == 0) {
        enum expr_type et = (tok[1] == 'm') ? E_MTIME :
                            (tok[1] == 'a') ? E_ATIME : E_CTIME;
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: %s requires argument\n", tok); exit(1); }
        struct expr *e = alloc_expr();
        e->type = et;
        if (parse_num(arg, &e->nval, &e->cmp) < 0) {
            fprintf(stderr, "find: invalid argument '%s'\n", arg);
            exit(1);
        }
        return e;
    }

    /* -mmin, -amin, -cmin (minutes) */
    if (strcmp(tok, "-mmin") == 0 || strcmp(tok, "-amin") == 0 ||
        strcmp(tok, "-cmin") == 0) {
        enum expr_type et = (tok[1] == 'm') ? E_MMIN :
                            (tok[1] == 'a') ? E_AMIN : E_CMIN;
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: %s requires argument\n", tok); exit(1); }
        struct expr *e = alloc_expr();
        e->type = et;
        if (parse_num(arg, &e->nval, &e->cmp) < 0) {
            fprintf(stderr, "find: invalid argument '%s'\n", arg);
            exit(1);
        }
        return e;
    }

    /* -links n */
    if (strcmp(tok, "-links") == 0) {
        consume_arg();
        const char *arg = consume_arg();
        if (!arg) { fprintf(stderr, "find: -links requires argument\n"); exit(1); }
        struct expr *e = alloc_expr();
        e->type = E_LINKS;
        if (parse_num(arg, &e->nval, &e->cmp) < 0) {
            fprintf(stderr, "find: invalid argument '%s'\n", arg);
            exit(1);
        }
        return e;
    }

    /* -readable, -writable, -executable */
    if (strcmp(tok, "-readable") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_READABLE;
        return e;
    }
    if (strcmp(tok, "-writable") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_WRITABLE;
        return e;
    }
    if (strcmp(tok, "-executable") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_EXECUTABLE;
        return e;
    }

    /* -true, -false */
    if (strcmp(tok, "-true") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_TRUE;
        return e;
    }
    if (strcmp(tok, "-false") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_FALSE;
        return e;
    }

    /* ---- Actions ---- */
    if (strcmp(tok, "-print") == 0) {
        consume_arg();
        g_has_action = 1;
        struct expr *e = alloc_expr();
        e->type = E_PRINT;
        return e;
    }
    if (strcmp(tok, "-print0") == 0) {
        consume_arg();
        g_has_action = 1;
        struct expr *e = alloc_expr();
        e->type = E_PRINT0;
        return e;
    }
    if (strcmp(tok, "-printf") == 0) {
        consume_arg();
        const char *fmt = consume_arg();
        if (!fmt) { fprintf(stderr, "find: -printf requires argument\n"); exit(1); }
        g_has_action = 1;
        struct expr *e = alloc_expr();
        e->type = E_PRINTF;
        e->fmt = fmt;
        return e;
    }

    /* -exec command {} ; or -exec command {} + */
    if (strcmp(tok, "-exec") == 0 || strcmp(tok, "-execdir") == 0) {
        enum expr_type et = (tok[5] == '\0') ? E_EXEC : E_EXECDIR;
        consume_arg();
        g_has_action = 1;
        /* Collect argv until ';' or '+' */
        int start = g_parse_i;
        int count = 0;
        int has_plus = 0;
        while (g_parse_i < g_parse_argc) {
            if (strcmp(g_parse_argv[g_parse_i], ";") == 0) {
                g_parse_i++;
                break;
            }
            if (strcmp(g_parse_argv[g_parse_i], "+") == 0) {
                has_plus = 1;
                g_parse_i++;
                break;
            }
            count++;
            g_parse_i++;
        }
        struct expr *e = alloc_expr();
        e->type = et;
        e->exec_argv = &g_parse_argv[start];
        e->exec_argc = count;
        e->exec_has_plus = has_plus;
        return e;
    }

    if (strcmp(tok, "-delete") == 0) {
        consume_arg();
        g_has_action = 1;
        g_depth_first = 1; /* -delete implies -depth */
        struct expr *e = alloc_expr();
        e->type = E_DELETE;
        return e;
    }

    if (strcmp(tok, "-ls") == 0) {
        consume_arg();
        g_has_action = 1;
        struct expr *e = alloc_expr();
        e->type = E_LS;
        return e;
    }

    if (strcmp(tok, "-prune") == 0) {
        consume_arg();
        struct expr *e = alloc_expr();
        e->type = E_PRUNE;
        return e;
    }

    if (strcmp(tok, "-quit") == 0) {
        consume_arg();
        g_has_action = 1;
        struct expr *e = alloc_expr();
        e->type = E_QUIT;
        return e;
    }

    return NULL; /* Unknown token */
}

/* AND has higher precedence */
static struct expr *parse_and(void)
{
    struct expr *left = parse_primary();
    if (!left) return NULL;

    while (1) {
        const char *tok = next_arg();
        if (!tok) break;
        /* Implicit AND between two primaries, or explicit -a/-and */
        if (strcmp(tok, "-a") == 0 || strcmp(tok, "-and") == 0) {
            consume_arg();
        }
        /* Check if next token is a primary (not an operator like -o, ), etc) */
        tok = next_arg();
        if (!tok) break;
        if (strcmp(tok, "-o") == 0 || strcmp(tok, "-or") == 0 ||
            strcmp(tok, ")") == 0)
            break;

        struct expr *right = parse_primary();
        if (!right) break;

        struct expr *and = alloc_expr();
        and->type = E_AND;
        and->left = left;
        and->right = right;
        left = and;
    }
    return left;
}

static struct expr *parse_or(void)
{
    struct expr *left = parse_and();
    if (!left) return NULL;

    while (1) {
        const char *tok = next_arg();
        if (!tok) break;
        if (strcmp(tok, "-o") != 0 && strcmp(tok, "-or") != 0)
            break;
        consume_arg();
        struct expr *right = parse_and();
        if (!right) {
            fprintf(stderr, "find: expected expression after '-o'\n");
            exit(1);
        }
        struct expr *or = alloc_expr();
        or->type = E_OR;
        or->left = left;
        or->right = right;
        left = or;
    }
    return left;
}

/* ================================================================
 * Get basename from path
 * ================================================================ */
static const char *path_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ================================================================
 * Format mode string (drwxrwxrwx)
 * ================================================================ */
static void format_mode(uint32_t mode, char *buf)
{
    buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' :
             S_ISCHR(mode) ? 'c' : S_ISBLK(mode) ? 'b' :
             S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

/* ================================================================
 * -printf format processing
 * ================================================================ */
static void do_printf(const char *fmt, const char *path, const struct stat *st)
{
    const char *base = path_basename(path);
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': putchar('\n'); break;
            case 't': putchar('\t'); break;
            case '\\': putchar('\\'); break;
            case '0': putchar('\0'); break;
            default: putchar('\\'); putchar(*p); break;
            }
        } else if (*p == '%') {
            p++;
            switch (*p) {
            case 'p': printf("%s", path); break;
            case 'f': printf("%s", base); break;
            case 'h': {
                /* Directory portion */
                const char *slash = strrchr(path, '/');
                if (slash && slash != path) {
                    int len = (int)(slash - path);
                    printf("%.*s", len, path);
                } else if (slash == path) {
                    putchar('/');
                } else {
                    putchar('.');
                }
                break;
            }
            case 's': printf("%lu", (unsigned long)st->st_size); break;
            case 'k': printf("%lu", ((unsigned long)st->st_size + 1023) / 1024); break;
            case 'm': printf("%03o", st->st_mode & 07777); break;
            case 'M': {
                char mb[12];
                format_mode(st->st_mode, mb);
                printf("%s", mb);
                break;
            }
            case 'u': printf("%u", st->st_uid); break;
            case 'g': printf("%u", st->st_gid); break;
            case 'n': printf("%u", st->st_nlink); break;
            case 'T': {
                p++;
                if (*p == '@') printf("%lu", (unsigned long)st->st_mtime);
                else { putchar('%'); putchar('T'); putchar(*p); }
                break;
            }
            case 'A': {
                p++;
                if (*p == '@') printf("%lu", (unsigned long)st->st_atime);
                else { putchar('%'); putchar('A'); putchar(*p); }
                break;
            }
            case 'C': {
                p++;
                if (*p == '@') printf("%lu", (unsigned long)st->st_ctime);
                else { putchar('%'); putchar('C'); putchar(*p); }
                break;
            }
            case 'y': {
                char c = S_ISDIR(st->st_mode) ? 'd' :
                         S_ISLNK(st->st_mode) ? 'l' :
                         S_ISBLK(st->st_mode) ? 'b' :
                         S_ISCHR(st->st_mode) ? 'c' :
                         S_ISFIFO(st->st_mode) ? 'p' :
                         S_ISSOCK(st->st_mode) ? 's' : 'f';
                putchar(c);
                break;
            }
            case 'd': /* depth - not tracked here, print 0 */
                putchar('0');
                break;
            case '%': putchar('%'); break;
            default: putchar('%'); putchar(*p); break;
            }
        } else {
            putchar(*p);
        }
    }
}

/* ================================================================
 * Execute -exec / -execdir
 * ================================================================ */
static int do_exec(struct expr *e, const char *path, const char *base)
{
    /* Build argv, replacing {} with path (or base for execdir) */
    const char *replacement = (e->type == E_EXECDIR) ? base : path;
    /* We need to fork+exec */
    char *argv[256];
    int ac = 0;
    for (int i = 0; i < e->exec_argc && ac < 254; i++) {
        if (strcmp(e->exec_argv[i], "{}") == 0) {
            argv[ac++] = (char*)replacement;
        } else {
            argv[ac++] = e->exec_argv[i];
        }
    }
    argv[ac] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "find: fork failed\n");
        return 0;
    }
    if (pid == 0) {
        /* Child */
        execvp(argv[0], argv);
        fprintf(stderr, "find: exec '%s': %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    /* Parent: wait */
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0;
    return 0;
}

/* ================================================================
 * Evaluate expression tree
 * Returns: 1 = match, 0 = no match
 * prune is set to 1 if -prune action fires
 * ================================================================ */
static int eval_expr(struct expr *e, const char *path, const struct stat *st,
                     int depth, int *prune)
{
    if (!e) return 1; /* No expression => match (default -print) */

    switch (e->type) {
    /* Tests */
    case E_NAME:
        return fnmatch(e->sval, path_basename(path), 0) == 0;
    case E_INAME:
        return fnmatch(e->sval, path_basename(path), FNM_CASEFOLD) == 0;
    case E_PATH:
        return fnmatch(e->sval, path, 0) == 0;
    case E_IPATH:
        return fnmatch(e->sval, path, FNM_CASEFOLD) == 0;

    case E_TYPE: {
        int dt;
        if (S_ISDIR(st->st_mode))       dt = DT_DIR;
        else if (S_ISREG(st->st_mode))   dt = DT_REG;
        else if (S_ISLNK(st->st_mode))   dt = DT_LNK;
        else if (S_ISCHR(st->st_mode))   dt = DT_CHR;
        else if (S_ISBLK(st->st_mode))   dt = DT_BLK;
        else if (S_ISFIFO(st->st_mode))  dt = DT_FIFO;
        else if (S_ISSOCK(st->st_mode))  dt = DT_SOCK;
        else dt = DT_UNKNOWN;
        return dt == e->type_mask;
    }

    case E_SIZE:
        return num_match((long)st->st_size, e->nval, e->cmp);

    case E_EMPTY:
        if (S_ISDIR(st->st_mode)) {
            /* Check if directory is empty */
            DIR *d = opendir(path);
            if (!d) return 0;
            struct dirent *de;
            int empty = 1;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                empty = 0;
                break;
            }
            closedir(d);
            return empty;
        }
        return st->st_size == 0;

    case E_PERM:
        switch (e->perm_mode) {
        case 0: return (st->st_mode & 07777) == e->perm_val;
        case 1: return (st->st_mode & e->perm_val) == e->perm_val;
        case 2: return (st->st_mode & e->perm_val) != 0;
        }
        return 0;

    case E_NEWER:
        return (long)st->st_mtime > e->nval;

    case E_MTIME: {
        long days = (long)(g_now - st->st_mtime) / 86400;
        return num_match(days, e->nval, e->cmp);
    }
    case E_ATIME: {
        long days = (long)(g_now - st->st_atime) / 86400;
        return num_match(days, e->nval, e->cmp);
    }
    case E_CTIME: {
        long days = (long)(g_now - st->st_ctime) / 86400;
        return num_match(days, e->nval, e->cmp);
    }

    case E_MMIN: {
        long mins = (long)(g_now - st->st_mtime) / 60;
        return num_match(mins, e->nval, e->cmp);
    }
    case E_AMIN: {
        long mins = (long)(g_now - st->st_atime) / 60;
        return num_match(mins, e->nval, e->cmp);
    }
    case E_CMIN: {
        long mins = (long)(g_now - st->st_ctime) / 60;
        return num_match(mins, e->nval, e->cmp);
    }

    case E_LINKS:
        return num_match((long)st->st_nlink, e->nval, e->cmp);

    case E_READABLE:
        return access(path, R_OK) == 0;
    case E_WRITABLE:
        return access(path, W_OK) == 0;
    case E_EXECUTABLE:
        return access(path, X_OK) == 0;

    case E_TRUE:  return 1;
    case E_FALSE: return 0;

    /* Actions */
    case E_PRINT:
        printf("%s\n", path);
        return 1;
    case E_PRINT0:
        printf("%s", path);
        putchar('\0');
        return 1;
    case E_PRINTF:
        do_printf(e->fmt, path, st);
        return 1;
    case E_EXEC:
    case E_EXECDIR:
        return do_exec(e, path, path_basename(path));
    case E_DELETE:
        if (S_ISDIR(st->st_mode)) {
            if (rmdir(path) != 0) {
                fprintf(stderr, "find: cannot delete '%s': %s\n", path, strerror(errno));
                g_retval = 1;
                return 0;
            }
        } else {
            if (unlink(path) != 0) {
                fprintf(stderr, "find: cannot delete '%s': %s\n", path, strerror(errno));
                g_retval = 1;
                return 0;
            }
        }
        return 1;
    case E_LS: {
        char mb[12];
        format_mode(st->st_mode, mb);
        printf("%8lu %s %u %u %8lu %s\n",
               0UL, mb, st->st_uid, st->st_gid,
               (unsigned long)st->st_size, path);
        return 1;
    }
    case E_PRUNE:
        *prune = 1;
        return 1;
    case E_QUIT:
        g_quit = 1;
        return 1;

    /* Operators */
    case E_AND: {
        int r = eval_expr(e->left, path, st, depth, prune);
        if (!r) return 0;
        return eval_expr(e->right, path, st, depth, prune);
    }
    case E_OR: {
        int r = eval_expr(e->left, path, st, depth, prune);
        if (r) return 1;
        return eval_expr(e->right, path, st, depth, prune);
    }
    case E_NOT:
        return !eval_expr(e->left, path, st, depth, prune);
    }
    return 0;
}

/* ================================================================
 * Walk directory tree
 * ================================================================ */
static void find_walk(const char *path, struct expr *expr, int depth)
{
    if (g_quit) return;

    /* Depth limit */
    if (g_maxdepth >= 0 && depth > g_maxdepth)
        return;

    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
        g_retval = 1;
        return;
    }

    int is_dir = S_ISDIR(st.st_mode);
    int prune = 0;

    /* Pre-order: evaluate on the way down (unless -depth) */
    if (!g_depth_first) {
        if (depth >= g_mindepth) {
            int matched = eval_expr(expr, path, &st, depth, &prune);
            if (!g_has_action && matched && depth >= g_mindepth) {
                printf("%s\n", path);
            }
        }
        if (g_quit) return;
    }

    /* Recurse into directories */
    if (is_dir && !prune) {
        if (g_maxdepth < 0 || depth < g_maxdepth) {
            DIR *d = opendir(path);
            if (!d) {
                fprintf(stderr, "find: '%s': %s\n", path, strerror(errno));
                g_retval = 1;
            } else {
                struct dirent *de;
                while ((de = readdir(d)) != NULL) {
                    if (g_quit) break;
                    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                        continue;
                    char child[4096];
                    int plen = strlen(path);
                    if (plen > 0 && path[plen-1] == '/') {
                        snprintf(child, sizeof(child), "%s%s", path, de->d_name);
                    } else {
                        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
                    }
                    find_walk(child, expr, depth + 1);
                }
                closedir(d);
            }
        }
    }

    /* Post-order: evaluate after recursing (-depth mode) */
    if (g_depth_first) {
        if (depth >= g_mindepth) {
            int matched = eval_expr(expr, path, &st, depth, &prune);
            if (!g_has_action && matched) {
                printf("%s\n", path);
            }
        }
    }
}

/* ================================================================
 * Usage
 * ================================================================ */
static void usage(void)
{
    fprintf(stderr,
        "Usage: find [path...] [expression]\n"
        "\nGlobal options:\n"
        "  -maxdepth LEVELS  Descend at most LEVELS levels\n"
        "  -mindepth LEVELS  Do not apply tests at levels less than LEVELS\n"
        "  -depth            Process directory contents before directory itself\n"
        "  -xdev             Do not descend into other filesystems\n"
        "  -help             Display this help\n"
        "  -version          Display version\n"
        "\nTests:\n"
        "  -name PATTERN     Base name matches shell PATTERN\n"
        "  -iname PATTERN    Like -name but case insensitive\n"
        "  -path PATTERN     Full path matches shell PATTERN\n"
        "  -ipath PATTERN    Like -path but case insensitive\n"
        "  -type [bcdpfls]   File type\n"
        "  -size N[bcwkMG]   File size\n"
        "  -empty             Empty file or directory\n"
        "  -perm [-/]MODE    Permissions\n"
        "  -newer FILE       Modified more recently than FILE\n"
        "  -mtime N          Modified N*24 hours ago\n"
        "  -atime N          Accessed N*24 hours ago\n"
        "  -ctime N          Changed N*24 hours ago\n"
        "  -mmin N           Modified N minutes ago\n"
        "  -amin N           Accessed N minutes ago\n"
        "  -cmin N           Changed N minutes ago\n"
        "  -links N          Has N hard links\n"
        "  -readable         Readable by current user\n"
        "  -writable         Writable by current user\n"
        "  -executable       Executable by current user\n"
        "  -true             Always true\n"
        "  -false            Always false\n"
        "\nActions:\n"
        "  -print            Print full path (default)\n"
        "  -print0           Print full path followed by NUL\n"
        "  -printf FORMAT    Print according to FORMAT\n"
        "  -exec CMD {} ;    Execute CMD\n"
        "  -execdir CMD {} ; Execute CMD in file's directory\n"
        "  -delete           Delete file or empty directory\n"
        "  -ls               List in ls -dils format\n"
        "  -prune            Do not descend into directory\n"
        "  -quit             Exit immediately\n"
        "\nOperators:\n"
        "  ! EXPR / -not EXPR    Negate\n"
        "  EXPR -a EXPR          Logical AND (default)\n"
        "  EXPR -o EXPR          Logical OR\n"
        "  ( EXPR )              Grouping\n");
}

/* ================================================================
 * Main
 * ================================================================ */
int main(int argc, char **argv)
{
    /* Get current time for -mtime etc */
    g_now = (unsigned long)time(NULL);

    /* Separate starting points from expression.
     * Starting points are arguments before the first expression token. */
    int paths_start = 1;
    int paths_end = 1;

    /* Scan for global options and find where expressions start */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && strcmp(argv[i], "!") != 0 &&
            strcmp(argv[i], "(") != 0) {
            paths_end = i + 1;
            continue;
        }
        /* Global options */
        if (strcmp(argv[i], "-maxdepth") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "find: -maxdepth requires argument\n"); return 1; }
            g_maxdepth = atoi(argv[++i]);
            /* Remove from argv by shifting */
            for (int j = i - 1; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i -= 2;
            continue;
        }
        if (strcmp(argv[i], "-mindepth") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "find: -mindepth requires argument\n"); return 1; }
            g_mindepth = atoi(argv[++i]);
            for (int j = i - 1; j < argc - 2; j++) argv[j] = argv[j + 2];
            argc -= 2; i -= 2;
            continue;
        }
        if (strcmp(argv[i], "-depth") == 0) {
            g_depth_first = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
            continue;
        }
        if (strcmp(argv[i], "-xdev") == 0 || strcmp(argv[i], "-mount") == 0) {
            g_xdev = 1;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--; i--;
            continue;
        }
        if (strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[i], "-version") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("find (LikeOS) 1.0\n");
            return 0;
        }
        /* This is an expression token; everything before it is paths */
        break;
    }

    /* Determine starting points */
    const char *default_paths[] = { "." };
    const char **paths = default_paths;
    int npaths = 1;

    /* Re-scan: paths are non-option args before the first expression */
    int first_expr = argc;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' || strcmp(argv[i], "!") == 0 ||
            strcmp(argv[i], "(") == 0) {
            first_expr = i;
            break;
        }
    }
    if (first_expr > 1) {
        paths = (const char **)&argv[1];
        npaths = first_expr - 1;
    }

    /* Parse expression */
    g_parse_i = first_expr;
    g_parse_argc = argc;
    g_parse_argv = argv;

    struct expr *expr = parse_or();
    if (g_parse_i < argc) {
        fprintf(stderr, "find: unexpected argument '%s'\n", argv[g_parse_i]);
        return 1;
    }

    /* Walk each starting path */
    for (int i = 0; i < npaths && !g_quit; i++) {
        find_walk(paths[i], expr, 0);
    }

    return g_retval;
}
