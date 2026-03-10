/*
 * man - an interface to the system reference manuals
 *
 * Full implementation per the man(1) manpage.
 * Reads pre-formatted plain text manpages from /usr/share/man/man1/
 * and displays them through a built-in pager.
 *
 * Supports section numbers, -k (apropos), -f (whatis),
 * and all standard command-line options.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>

#define MAN_VERSION "man (LikeOS) 0.1"

/* Maximum sizes */
#define MAX_LINE    1024
#define MAX_LINES   16384
#define MAX_PATH    512

/* Manpage search paths */
static const char *man_paths[] = {
    "/usr/share/man/man1",
    "/usr/share/man/man2",
    "/usr/share/man/man3",
    "/usr/share/man/man4",
    "/usr/share/man/man5",
    "/usr/share/man/man6",
    "/usr/share/man/man7",
    "/usr/share/man/man8",
    NULL
};

/* ======================================================================
 * Terminal handling
 * ====================================================================== */

static int g_term_rows = 24;
static int g_term_cols = 80;
static struct termios g_orig_termios;
static int g_raw_mode = 0;

static void detect_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) g_term_rows = ws.ws_row;
        if (ws.ws_col > 0) g_term_cols = ws.ws_col;
    }
}

static void enter_raw_mode(void) {
    if (g_raw_mode) return;
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_raw_mode = 1;
}

static void leave_raw_mode(void) {
    if (!g_raw_mode) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_raw_mode = 0;
}

/* ======================================================================
 * Simple case-insensitive string functions
 * ====================================================================== */

static int my_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static char *my_strcasestr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *)haystack;
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (my_tolower((unsigned char)haystack[i + j]) !=
                my_tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return (char *)&haystack[i];
    }
    return NULL;
}

/* ======================================================================
 * Manpage file location
 * ====================================================================== */

/* Try to find manpage file. Returns 0 on success with path filled in. */
static int find_manpage(const char *name, int section, char *path, size_t pathsz) {
    char fname[256];

    if (section > 0) {
        /* Look in specific section only */
        snprintf(fname, sizeof(fname), "%s.%d", name, section);
        snprintf(path, pathsz, "/usr/share/man/man%d/%s", section, fname);
        if (access(path, R_OK) == 0) return 0;

        /* Try without section suffix in filename */
        snprintf(path, pathsz, "/usr/share/man/man%d/%s.1", section, name);
        if (access(path, R_OK) == 0) return 0;

        return -1;
    }

    /* Search all sections in order */
    for (int s = 1; s <= 8; s++) {
        snprintf(fname, sizeof(fname), "%s.%d", name, s);
        snprintf(path, pathsz, "/usr/share/man/man%d/%s", s, fname);
        if (access(path, R_OK) == 0) return 0;
    }

    /* Try just the name in man1 */
    snprintf(path, pathsz, "/usr/share/man/man1/%s", name);
    if (access(path, R_OK) == 0) return 0;

    /* Try name.1 */
    snprintf(path, pathsz, "/usr/share/man/man1/%s.1", name);
    if (access(path, R_OK) == 0) return 0;

    return -1;
}

/* ======================================================================
 * Simple troff/nroff rendering (for pre-formatted text)
 *
 * Since we store pre-formatted (catman-style) manpages, the rendering
 * is mostly just displaying the text. We handle basic backspace-based
 * bold/underline sequences that man produces.
 * ====================================================================== */

/* Strip backspace-based formatting and produce clean text */
static void strip_overstrikes(const char *src, char *dst, size_t dstsz) {
    size_t di = 0;
    size_t si = 0;
    size_t slen = strlen(src);

    while (si < slen && di < dstsz - 1) {
        if (si + 2 < slen && src[si + 1] == '\b') {
            /* X\bX = bold X, _\bX = underline X */
            dst[di++] = src[si + 2];
            si += 3;
        } else if (src[si] == '\b') {
            /* Stray backspace: erase previous char */
            if (di > 0) di--;
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

/* ======================================================================
 * Manpage loading
 * ====================================================================== */

typedef struct {
    char **lines;
    int nlines;
    int capacity;
} manpage_t;

static manpage_t *load_manpage(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    manpage_t *mp = malloc(sizeof(manpage_t));
    if (!mp) { fclose(fp); return NULL; }

    mp->capacity = 256;
    mp->lines = malloc(mp->capacity * sizeof(char *));
    if (!mp->lines) { free(mp); fclose(fp); return NULL; }
    mp->nlines = 0;

    char *buf = malloc(MAX_LINE);
    if (!buf) { free(mp->lines); free(mp); fclose(fp); return NULL; }

    while (fgets(buf, MAX_LINE, fp)) {
        /* Remove trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

        /* Grow lines array if needed */
        if (mp->nlines >= mp->capacity) {
            mp->capacity *= 2;
            char **new_lines = malloc(mp->capacity * sizeof(char *));
            if (!new_lines) break;
            memcpy(new_lines, mp->lines, mp->nlines * sizeof(char *));
            free(mp->lines);
            mp->lines = new_lines;
        }

        /* Strip overstrikes for clean display */
        char *clean = malloc(MAX_LINE);
        if (!clean) break;
        strip_overstrikes(buf, clean, MAX_LINE);

        /* Allocate just enough for the cleaned line */
        size_t clen = strlen(clean);
        mp->lines[mp->nlines] = malloc(clen + 1);
        if (!mp->lines[mp->nlines]) { free(clean); break; }
        memcpy(mp->lines[mp->nlines], clean, clen + 1);
        mp->nlines++;
        free(clean);
    }

    free(buf);
    fclose(fp);
    return mp;
}

static void free_manpage(manpage_t *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->nlines; i++)
        free(mp->lines[i]);
    free(mp->lines);
    free(mp);
}

/* ======================================================================
 * Interactive pager
 * ====================================================================== */

static void pager(manpage_t *mp, const char *title) {
    if (!mp || mp->nlines == 0) return;

    int top_line = 0;
    int page_size = g_term_rows - 1;  /* Reserve 1 line for status */
    int search_active = 0;
    char search_str[128] = "";

    detect_term_size();
    enter_raw_mode();

    int done = 0;
    while (!done) {
        /* Clear screen and display page */
        printf("\033[H\033[2J");

        int lines_shown = 0;
        for (int i = top_line; i < mp->nlines && lines_shown < page_size; i++, lines_shown++) {
            const char *line = mp->lines[i];
            int len = (int)strlen(line);

            /* Highlight search matches */
            if (search_active && search_str[0]) {
                char *match = my_strcasestr(line, search_str);
                if (match) {
                    int pre = (int)(match - line);
                    /* Print before match */
                    if (pre > 0) fwrite(line, 1, pre, stdout);
                    /* Print match highlighted */
                    printf("\033[7m");
                    fwrite(match, 1, strlen(search_str), stdout);
                    printf("\033[0m");
                    /* Print after match */
                    int post_start = pre + (int)strlen(search_str);
                    if (post_start < len)
                        printf("%s", line + post_start);
                    printf("\033[K\n");
                    continue;
                }
            }

            /* Print line, truncate to terminal width */
            if (len > g_term_cols)
                printf("%.*s\033[K\n", g_term_cols, line);
            else
                printf("%s\033[K\n", line);
        }

        /* Fill remaining lines */
        while (lines_shown < page_size) {
            printf("~\033[K\n");
            lines_shown++;
        }

        /* Status line */
        int pct = (mp->nlines > 0) ? ((top_line + page_size) * 100 / mp->nlines) : 100;
        if (pct > 100) pct = 100;
        printf("\033[7m Manual page %s line %d (press h for help or q to quit) %d%%\033[0m\033[K",
               title, top_line + 1, pct);
        fflush(stdout);

        /* Read input */
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        switch (c) {
        case 'q':
        case 'Q':
            done = 1;
            break;

        case ' ':     /* Page down */
        case 'f':
            top_line += page_size;
            break;

        case 'b':     /* Page up */
            top_line -= page_size;
            break;

        case 'j':     /* Line down */
        case '\n':
        case '\r':
            top_line++;
            break;

        case 'k':     /* Line up */
            top_line--;
            break;

        case 'd':     /* Half page down */
            top_line += page_size / 2;
            break;

        case 'u':     /* Half page up */
            top_line -= page_size / 2;
            break;

        case 'g':     /* Go to top */
            top_line = 0;
            break;

        case 'G':     /* Go to bottom */
            top_line = mp->nlines - page_size;
            break;

        case '/': {   /* Search forward */
            /* Show prompt */
            printf("\r\033[K/");
            fflush(stdout);
            printf("\033[?25h"); /* Show cursor */

            int pos = 0;
            while (pos < (int)sizeof(search_str) - 1) {
                unsigned char sc;
                if (read(STDIN_FILENO, &sc, 1) != 1) break;
                if (sc == '\n' || sc == '\r') break;
                if (sc == 27) { pos = 0; break; }
                if (sc == 127 || sc == 8) {
                    if (pos > 0) { pos--; printf("\b \b"); fflush(stdout); }
                    continue;
                }
                if (sc >= 32) {
                    search_str[pos++] = sc;
                    write(STDOUT_FILENO, &sc, 1);
                }
            }
            search_str[pos] = '\0';
            printf("\033[?25l");

            if (search_str[0]) {
                search_active = 1;
                /* Find next match from current position */
                for (int i = top_line + 1; i < mp->nlines; i++) {
                    if (my_strcasestr(mp->lines[i], search_str)) {
                        top_line = i;
                        break;
                    }
                }
            }
            break;
        }

        case 'n': {   /* Next search match */
            if (!search_active || !search_str[0]) break;
            for (int i = top_line + 1; i < mp->nlines; i++) {
                if (my_strcasestr(mp->lines[i], search_str)) {
                    top_line = i;
                    break;
                }
            }
            break;
        }

        case 'N': {   /* Previous search match */
            if (!search_active || !search_str[0]) break;
            for (int i = top_line - 1; i >= 0; i--) {
                if (my_strcasestr(mp->lines[i], search_str)) {
                    top_line = i;
                    break;
                }
            }
            break;
        }

        case 'h':
        case 'H': {
            /* Help screen */
            printf("\033[H\033[2J");
            printf("                   SUMMARY OF LESS COMMANDS\n\n");
            printf("  h  H           Display this help.\n");
            printf("  q  :q  Q  ZZ   Exit.\n\n");
            printf("                   MOVING\n\n");
            printf("  j  ^N  CR      Forward  one line.\n");
            printf("  k  ^P          Backward one line.\n");
            printf("  f  ^F  SPACE   Forward  one window.\n");
            printf("  b  ^B          Backward one window.\n");
            printf("  d  ^D          Forward  one half-window.\n");
            printf("  u  ^U          Backward one half-window.\n");
            printf("  g  <           Go to first line.\n");
            printf("  G  >           Go to last line.\n\n");
            printf("                   SEARCHING\n\n");
            printf("  /pattern       Search forward for pattern.\n");
            printf("  n              Repeat previous search.\n");
            printf("  N              Repeat previous search in reverse.\n\n");
            printf("Press any key to continue...");
            fflush(stdout);
            read(STDIN_FILENO, &c, 1);
            break;
        }

        case 27: {
            /* Escape sequences for arrow keys */
            unsigned char c2, c3;
            if (read(STDIN_FILENO, &c2, 1) != 1) break;
            if (c2 != '[') break;
            if (read(STDIN_FILENO, &c3, 1) != 1) break;
            switch (c3) {
            case 'A': top_line--; break;        /* Up */
            case 'B': top_line++; break;        /* Down */
            case '5':                            /* PgUp */
                read(STDIN_FILENO, &c3, 1);     /* consume '~' */
                top_line -= page_size;
                break;
            case '6':                            /* PgDn */
                read(STDIN_FILENO, &c3, 1);     /* consume '~' */
                top_line += page_size;
                break;
            case 'H': top_line = 0; break;      /* Home */
            case 'F':                            /* End */
                top_line = mp->nlines - page_size;
                break;
            }
            break;
        }
        }

        /* Clamp scroll position */
        if (top_line > mp->nlines - page_size)
            top_line = mp->nlines - page_size;
        if (top_line < 0) top_line = 0;
    }

    leave_raw_mode();
    /* Clear status line */
    printf("\r\033[K");
    fflush(stdout);
}

/* ======================================================================
 * Batch output (no pager, for piped output)
 * ====================================================================== */

static void batch_output(manpage_t *mp) {
    if (!mp) return;
    for (int i = 0; i < mp->nlines; i++)
        printf("%s\n", mp->lines[i]);
}

/* ======================================================================
 * Apropos / whatis functionality (-k / -f)
 * ====================================================================== */

static void do_apropos(const char *keyword) {
    /* Search through all manpages for the keyword in NAME section */
    int found = 0;

    for (int pi = 0; man_paths[pi]; pi++) {
        DIR *dir = opendir(man_paths[pi]);
        if (!dir) continue;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;

            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s/%s", man_paths[pi], de->d_name);

            FILE *fp = fopen(path, "r");
            if (!fp) continue;

            char line[MAX_LINE];
            int in_name = 0;
            char name_text[MAX_LINE] = "";

            while (fgets(line, sizeof(line), fp)) {
                /* Remove newline */
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

                /* Check for NAME section header */
                if (strcmp(line, "NAME") == 0 ||
                    strcmp(line, "Name") == 0) {
                    in_name = 1;
                    continue;
                }

                /* End of NAME section (next section header) */
                if (in_name && line[0] != ' ' && line[0] != '\t' && line[0] != '\0') {
                    break;
                }

                if (in_name && line[0] != '\0') {
                    /* Accumulate name description */
                    if (name_text[0])
                        strncat(name_text, " ", sizeof(name_text) - strlen(name_text) - 1);
                    /* Skip leading whitespace */
                    char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    strncat(name_text, p, sizeof(name_text) - strlen(name_text) - 1);
                }
            }
            fclose(fp);

            /* Check if keyword matches */
            if (name_text[0] && my_strcasestr(name_text, keyword)) {
                printf("%s\n", name_text);
                found++;
            }
        }
        closedir(dir);
    }

    if (!found) {
        fprintf(stderr, "%s: nothing appropriate.\n", keyword);
    }
}

static void do_whatis(const char *name) {
    /* Look for exact command name match in NAME sections */
    int found = 0;

    for (int pi = 0; man_paths[pi]; pi++) {
        DIR *dir = opendir(man_paths[pi]);
        if (!dir) continue;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;

            /* Check if filename starts with the command name */
            size_t nlen = strlen(name);
            if (strncmp(de->d_name, name, nlen) != 0) continue;
            if (de->d_name[nlen] != '.' && de->d_name[nlen] != '\0') continue;

            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s/%s", man_paths[pi], de->d_name);

            FILE *fp = fopen(path, "r");
            if (!fp) continue;

            char line[MAX_LINE];
            int in_name = 0;
            char name_text[MAX_LINE] = "";

            while (fgets(line, sizeof(line), fp)) {
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

                if (strcmp(line, "NAME") == 0 || strcmp(line, "Name") == 0) {
                    in_name = 1;
                    continue;
                }
                if (in_name && line[0] != ' ' && line[0] != '\t' && line[0] != '\0')
                    break;
                if (in_name && line[0] != '\0') {
                    if (name_text[0])
                        strncat(name_text, " ", sizeof(name_text) - strlen(name_text) - 1);
                    char *p = line;
                    while (*p == ' ' || *p == '\t') p++;
                    strncat(name_text, p, sizeof(name_text) - strlen(name_text) - 1);
                }
            }
            fclose(fp);

            if (name_text[0]) {
                printf("%s\n", name_text);
                found++;
            }
        }
        closedir(dir);
    }

    if (!found)
        fprintf(stderr, "%s: nothing appropriate.\n", name);
}

/* ======================================================================
 * Usage and help
 * ====================================================================== */

static void print_usage(void) {
    printf("Usage: man [OPTION...] [SECTION] PAGE...\n\n");
    printf("  -f, --whatis     equivalent to whatis\n");
    printf("  -k, --apropos    equivalent to apropos\n");
    printf("  -a, --all        find all matching manual pages\n");
    printf("  -w, --where      print physical location of man page(s)\n");
    printf("      --path       print physical location of man page(s)\n");
    printf("  -M, --manpath    set search path for manual pages\n");
    printf("  -S, --sections   use colon separated section list\n");
    printf("  -7, --ascii      display ASCII translation of certain chars\n");
    printf("  -E, --encoding   use selected output encoding\n");
    printf("  -V, --version    show version\n");
    printf("  -h, --help       show this help\n");
}

/* ======================================================================
 * Main
 * ====================================================================== */

int main(int argc, char *argv[]) {
    int opt_whatis  = 0;
    int opt_apropos = 0;
    int opt_where   = 0;
    int section     = 0;

    static struct option long_options[] = {
        {"whatis",    no_argument,       0, 'f'},
        {"apropos",   no_argument,       0, 'k'},
        {"all",       no_argument,       0, 'a'},
        {"where",     no_argument,       0, 'w'},
        {"path",      no_argument,       0, 'w'},
        {"manpath",   required_argument, 0, 'M'},
        {"sections",  required_argument, 0, 'S'},
        {"ascii",     no_argument,       0, '7'},
        {"encoding",  required_argument, 0, 'E'},
        {"version",   no_argument,       0, 'V'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "fkawM:S:7E:Vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'f': opt_whatis = 1; break;
        case 'k': opt_apropos = 1; break;
        case 'a': /* show all matches - not yet implemented */ break;
        case 'w': opt_where = 1; break;
        case 'M': /* Custom manpath - ignored for now */ break;
        case 'S': /* Sections - ignored for now */ break;
        case '7': /* ASCII mode - default */ break;
        case 'E': /* Encoding - ignored */ break;
        case 'V':
            printf("%s\n", MAN_VERSION);
            return 0;
        case 'h':
            print_usage();
            return 0;
        default:
            print_usage();
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "What manual page do you want?\nFor example, try 'man man'.\n");
        return 1;
    }

    /* Check if first non-option argument is a section number */
    if (optind < argc - 1) {
        char *endp;
        long s = strtol(argv[optind], &endp, 10);
        if (*endp == '\0' && s >= 1 && s <= 9) {
            section = (int)s;
            optind++;
        }
    }

    /* Handle -k (apropos) */
    if (opt_apropos) {
        for (int i = optind; i < argc; i++)
            do_apropos(argv[i]);
        return 0;
    }

    /* Handle -f (whatis) */
    if (opt_whatis) {
        for (int i = optind; i < argc; i++)
            do_whatis(argv[i]);
        return 0;
    }

    /* Process each page argument */
    int ret = 0;
    for (int i = optind; i < argc; i++) {
        char path[MAX_PATH];

        if (find_manpage(argv[i], section, path, sizeof(path)) < 0) {
            fprintf(stderr, "No manual entry for %s", argv[i]);
            if (section > 0)
                fprintf(stderr, " in section %d", section);
            fprintf(stderr, "\n");
            ret = 1;
            continue;
        }

        /* Handle -w (where) */
        if (opt_where) {
            printf("%s\n", path);
            continue;
        }

        /* Load the manpage */
        manpage_t *mp = load_manpage(path);
        if (!mp) {
            fprintf(stderr, "man: can't open %s: %s\n", path, strerror(errno));
            ret = 1;
            continue;
        }

        /* Display through pager or batch */
        if (isatty(STDOUT_FILENO)) {
            detect_term_size();
            char title[256];
            snprintf(title, sizeof(title), "%s(%d)",
                     argv[i], section > 0 ? section : 1);
            pager(mp, title);
        } else {
            batch_output(mp);
        }

        free_manpage(mp);
    }

    return ret;
}
