/*
 * free - Display amount of free and used memory in the system
 *
 * Usage: free [options]
 *
 * Full implementation per the free(1) manpage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/sysinfo.h>

#define FREE_VERSION "free (LikeOS procps) 0.1"

/* Unit modes */
enum unit_mode {
    UNIT_BYTES,
    UNIT_KIBI,    /* KiB - default */
    UNIT_MEBI,    /* MiB */
    UNIT_GIBI,    /* GiB */
    UNIT_TEBI,    /* TiB */
    UNIT_PEBI,    /* PiB */
    UNIT_KILO,    /* KB (1000-based) */
    UNIT_MEGA,    /* MB */
    UNIT_GIGA,    /* GB */
    UNIT_TERA,    /* TB */
    UNIT_PETA,    /* PB */
    UNIT_HUMAN,   /* auto human-readable (1024-based) */
    UNIT_SI_HUMAN /* auto human-readable (1000-based) */
};

static void print_help(void)
{
    printf("Usage:\n free [options]\n\n");
    printf("Options:\n");
    printf(" -b, --bytes         show output in bytes\n");
    printf("     --kilo          show output in kilobytes\n");
    printf("     --mega          show output in megabytes\n");
    printf("     --giga          show output in gigabytes\n");
    printf("     --tera          show output in terabytes\n");
    printf("     --peta          show output in petabytes\n");
    printf(" -k, --kibi          show output in kibibytes (default)\n");
    printf(" -m, --mebi          show output in mebibytes\n");
    printf(" -g, --gibi          show output in gibibytes\n");
    printf("     --tebi          show output in tebibytes\n");
    printf("     --pebi          show output in pebibytes\n");
    printf(" -h, --human         show human-readable output\n");
    printf("     --si            use powers of 1000 not 1024\n");
    printf(" -l, --lohi          show detailed low and high memory statistics\n");
    printf(" -L, --line          show output on a single line\n");
    printf(" -t, --total         show total for RAM + swap\n");
    printf(" -v, --committed     show committed memory and commit limit\n");
    printf(" -s N, --seconds N   repeat printing every N seconds\n");
    printf(" -c N, --count N     repeat printing N times, then exit\n");
    printf(" -w, --wide          wide output\n");
    printf("     --help          display this help and exit\n");
    printf(" -V, --version       output version information and exit\n");
}

static void human_readable(unsigned long bytes, char *buf, int buf_len, int si)
{
    const char *suffixes_bin[] = {"B", "Ki", "Mi", "Gi", "Ti", "Pi"};
    const char *suffixes_si[] = {"B", "K", "M", "G", "T", "P"};
    const char **suffixes = si ? suffixes_si : suffixes_bin;
    unsigned long divisor = si ? 1000 : 1024;

    double val = (double)bytes;
    int idx = 0;
    while (val >= (double)divisor && idx < 5) {
        val /= (double)divisor;
        idx++;
    }

    if (idx == 0)
        snprintf(buf, buf_len, "%lu%s", bytes, suffixes[0]);
    else if (val >= 100.0)
        snprintf(buf, buf_len, "%.0f%s", val, suffixes[idx]);
    else if (val >= 10.0)
        snprintf(buf, buf_len, "%.1f%s", val, suffixes[idx]);
    else
        snprintf(buf, buf_len, "%.1f%s", val, suffixes[idx]);
}

static unsigned long scale_value(unsigned long bytes, enum unit_mode mode)
{
    switch (mode) {
    case UNIT_BYTES: return bytes;
    case UNIT_KIBI:  return bytes / 1024UL;
    case UNIT_MEBI:  return bytes / (1024UL * 1024UL);
    case UNIT_GIBI:  return bytes / (1024UL * 1024UL * 1024UL);
    case UNIT_TEBI:  return bytes / (1024UL * 1024UL * 1024UL * 1024UL);
    case UNIT_PEBI:  return bytes / (1024UL * 1024UL * 1024UL * 1024UL * 1024UL);
    case UNIT_KILO:  return bytes / 1000UL;
    case UNIT_MEGA:  return bytes / (1000UL * 1000UL);
    case UNIT_GIGA:  return bytes / (1000UL * 1000UL * 1000UL);
    case UNIT_TERA:  return bytes / (1000UL * 1000UL * 1000UL * 1000UL);
    case UNIT_PETA:  return bytes / (1000UL * 1000UL * 1000UL * 1000UL * 1000UL);
    default:         return bytes / 1024UL;
    }
}

static void print_val(unsigned long bytes, enum unit_mode mode, int si_flag, int field_width)
{
    if (mode == UNIT_HUMAN || mode == UNIT_SI_HUMAN) {
        char buf[64];
        human_readable(bytes, buf, sizeof(buf), (mode == UNIT_SI_HUMAN) || si_flag);
        printf("%*s", field_width, buf);
    } else {
        printf("%*lu", field_width, scale_value(bytes, mode));
    }
}

int main(int argc, char *argv[])
{
    enum unit_mode mode = UNIT_KIBI;
    int show_lohi = 0;
    int show_total = 0;
    int show_committed = 0;
    int line_mode = 0;
    int wide = 0;
    int si_flag = 0;
    int repeat_seconds = 0;
    int repeat_count = 0;

    enum {
        OPT_KILO = 256,
        OPT_MEGA,
        OPT_GIGA,
        OPT_TERA,
        OPT_PETA,
        OPT_TEBI,
        OPT_PEBI,
        OPT_SI,
        OPT_HELP,
    };

    static struct option long_options[] = {
        {"bytes",      no_argument,       0, 'b'},
        {"kilo",       no_argument,       0, OPT_KILO},
        {"mega",       no_argument,       0, OPT_MEGA},
        {"giga",       no_argument,       0, OPT_GIGA},
        {"tera",       no_argument,       0, OPT_TERA},
        {"peta",       no_argument,       0, OPT_PETA},
        {"kibi",       no_argument,       0, 'k'},
        {"mebi",       no_argument,       0, 'm'},
        {"gibi",       no_argument,       0, 'g'},
        {"tebi",       no_argument,       0, OPT_TEBI},
        {"pebi",       no_argument,       0, OPT_PEBI},
        {"human",      no_argument,       0, 'h'},
        {"si",         no_argument,       0, OPT_SI},
        {"lohi",       no_argument,       0, 'l'},
        {"line",       no_argument,       0, 'L'},
        {"total",      no_argument,       0, 't'},
        {"committed",  no_argument,       0, 'v'},
        {"wide",       no_argument,       0, 'w'},
        {"seconds",    required_argument, 0, 's'},
        {"count",      required_argument, 0, 'c'},
        {"version",    no_argument,       0, 'V'},
        {"help",       no_argument,       0, OPT_HELP},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "bkmghltLvws:c:V", long_options, NULL)) != -1) {
        switch (opt) {
        case 'b': mode = UNIT_BYTES; break;
        case 'k': mode = UNIT_KIBI; break;
        case 'm': mode = UNIT_MEBI; break;
        case 'g': mode = UNIT_GIBI; break;
        case 'h': mode = UNIT_HUMAN; break;
        case 'l': show_lohi = 1; break;
        case 'L': line_mode = 1; break;
        case 't': show_total = 1; break;
        case 'v': show_committed = 1; break;
        case 'w': wide = 1; break;
        case 's': repeat_seconds = atoi(optarg); break;
        case 'c': repeat_count = atoi(optarg); break;
        case 'V': printf("%s\n", FREE_VERSION); return 0;
        case OPT_KILO: mode = UNIT_KILO; break;
        case OPT_MEGA: mode = UNIT_MEGA; break;
        case OPT_GIGA: mode = UNIT_GIGA; break;
        case OPT_TERA: mode = UNIT_TERA; break;
        case OPT_PETA: mode = UNIT_PETA; break;
        case OPT_TEBI: mode = UNIT_TEBI; break;
        case OPT_PEBI: mode = UNIT_PEBI; break;
        case OPT_SI: si_flag = 1; if (mode == UNIT_HUMAN) mode = UNIT_SI_HUMAN; break;
        case OPT_HELP: print_help(); return 0;
        default:
            fprintf(stderr, "Try 'free --help' for more information.\n");
            return 1;
        }
    }

    if (si_flag && mode == UNIT_HUMAN)
        mode = UNIT_SI_HUMAN;

    int count_done = 0;
    int fw = 11; /* field width */

    do {
        struct sysinfo info;
        if (sysinfo(&info) < 0) {
            fprintf(stderr, "free: sysinfo failed\n");
            return 1;
        }

        unsigned long unit = info.mem_unit ? info.mem_unit : 1;
        unsigned long total = info.totalram * unit;
        unsigned long free_mem = info.freeram * unit;
        unsigned long shared = info.sharedram * unit;
        unsigned long buffers = info.bufferram * unit;
        unsigned long cached = info.cached * unit;
        unsigned long available = info.available * unit;
        unsigned long used = total - free_mem - buffers - cached;
        if (used > total) used = 0; /* overflow protection */

        unsigned long swap_total = info.totalswap * unit;
        unsigned long swap_free = info.freeswap * unit;
        unsigned long swap_used = swap_total - swap_free;

        if (line_mode) {
            /* Single line mode */
            printf("Mem:");
            printf(" total="); print_val(total, mode, si_flag, 0);
            printf(" used="); print_val(used, mode, si_flag, 0);
            printf(" free="); print_val(free_mem, mode, si_flag, 0);
            printf(" shared="); print_val(shared, mode, si_flag, 0);
            printf(" buff/cache="); print_val(buffers + cached, mode, si_flag, 0);
            printf(" available="); print_val(available, mode, si_flag, 0);
            printf("  Swap:");
            printf(" total="); print_val(swap_total, mode, si_flag, 0);
            printf(" used="); print_val(swap_used, mode, si_flag, 0);
            printf(" free="); print_val(swap_free, mode, si_flag, 0);
            if (show_total) {
                printf("  Total:");
                printf(" total="); print_val(total + swap_total, mode, si_flag, 0);
                printf(" used="); print_val(used + swap_used, mode, si_flag, 0);
                printf(" free="); print_val(free_mem + swap_free, mode, si_flag, 0);
            }
            printf("\n");
        } else if (wide) {
            /* Wide output: buffers and cache on separate columns */
            printf("               total        used        free      shared     buffers       cache   available\n");
            printf("Mem:    ");
            print_val(total, mode, si_flag, fw);
            print_val(used, mode, si_flag, fw+1);
            print_val(free_mem, mode, si_flag, fw+1);
            print_val(shared, mode, si_flag, fw+1);
            print_val(buffers, mode, si_flag, fw+1);
            print_val(cached, mode, si_flag, fw+1);
            print_val(available, mode, si_flag, fw+1);
            printf("\n");
            printf("Swap:   ");
            print_val(swap_total, mode, si_flag, fw);
            print_val(swap_used, mode, si_flag, fw+1);
            print_val(swap_free, mode, si_flag, fw+1);
            printf("\n");
            if (show_total) {
                printf("Total:  ");
                print_val(total + swap_total, mode, si_flag, fw);
                print_val(used + swap_used, mode, si_flag, fw+1);
                print_val(free_mem + swap_free, mode, si_flag, fw+1);
                printf("\n");
            }
        } else {
            /* Normal output */
            printf("               total        used        free      shared  buff/cache   available\n");
            printf("Mem:    ");
            print_val(total, mode, si_flag, fw);
            print_val(used, mode, si_flag, fw+1);
            print_val(free_mem, mode, si_flag, fw+1);
            print_val(shared, mode, si_flag, fw+1);
            print_val(buffers + cached, mode, si_flag, fw+1);
            print_val(available, mode, si_flag, fw+1);
            printf("\n");

            if (show_lohi) {
                unsigned long hi_total = info.totalhigh * unit;
                unsigned long hi_free = info.freehigh * unit;
                unsigned long lo_total = total - hi_total;
                unsigned long lo_free = free_mem - hi_free;
                printf("Low:    ");
                print_val(lo_total, mode, si_flag, fw);
                print_val(lo_total - lo_free, mode, si_flag, fw+1);
                print_val(lo_free, mode, si_flag, fw+1);
                printf("\n");
                printf("High:   ");
                print_val(hi_total, mode, si_flag, fw);
                print_val(hi_total - hi_free, mode, si_flag, fw+1);
                print_val(hi_free, mode, si_flag, fw+1);
                printf("\n");
            }

            printf("Swap:   ");
            print_val(swap_total, mode, si_flag, fw);
            print_val(swap_used, mode, si_flag, fw+1);
            print_val(swap_free, mode, si_flag, fw+1);
            printf("\n");

            if (show_total) {
                printf("Total:  ");
                print_val(total + swap_total, mode, si_flag, fw);
                print_val(used + swap_used, mode, si_flag, fw+1);
                print_val(free_mem + swap_free, mode, si_flag, fw+1);
                printf("\n");
            }

            if (show_committed) {
                printf("Comm:   ");
                /* No commit info available in this OS */
                print_val(0, mode, si_flag, fw);
                print_val(0, mode, si_flag, fw+1);
                printf("\n");
            }
        }

        count_done++;
        if (repeat_seconds > 0 && (repeat_count == 0 || count_done < repeat_count)) {
            struct timespec ts;
            ts.tv_sec = repeat_seconds;
            ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
        } else if (repeat_count > 0 && count_done >= repeat_count) {
            break;
        } else {
            break;
        }
    } while (1);

    return 0;
}
