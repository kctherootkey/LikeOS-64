#include "../../include/getopt.h"
#include "../../include/string.h"
#include "../../include/stdio.h"
#include <stddef.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

/* Position within the current argument string (for grouped short opts) */
static int _optpos = 0;

int getopt(int argc, char * const argv[], const char *optstring)
{
    const char *p;

    optarg = NULL;

    if (optind >= argc)
        return -1;

    /* Skip non-option arguments (those not starting with '-') */
    if (argv[optind] == NULL || argv[optind][0] != '-' || argv[optind][1] == '\0')
        return -1;

    /* "--" terminates option scanning */
    if (argv[optind][0] == '-' && argv[optind][1] == '-' && argv[optind][2] == '\0') {
        optind++;
        return -1;
    }

    if (_optpos == 0)
        _optpos = 1;

    optopt = argv[optind][_optpos];

    /* Look up the option character in optstring */
    p = strchr(optstring, optopt);
    if (p == NULL || optopt == ':') {
        /* Unknown option */
        if (opterr && optstring[0] != ':')
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], optopt);
        /* Advance past this character */
        _optpos++;
        if (argv[optind][_optpos] == '\0') {
            optind++;
            _optpos = 0;
        }
        return '?';
    }

    /* Check if this option requires an argument */
    if (p[1] == ':') {
        /* Argument required */
        if (argv[optind][_optpos + 1] != '\0') {
            /* Argument is the rest of this argv element */
            optarg = &argv[optind][_optpos + 1];
        } else if (optind + 1 < argc) {
            /* Argument is the next argv element */
            optarg = argv[optind + 1];
            optind++;
        } else {
            /* Missing argument */
            if (opterr && optstring[0] != ':')
                fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                        argv[0], optopt);
            optind++;
            _optpos = 0;
            return (optstring[0] == ':') ? ':' : '?';
        }
        optind++;
        _optpos = 0;
    } else {
        /* No argument needed – advance within grouped options or to next arg */
        _optpos++;
        if (argv[optind][_optpos] == '\0') {
            optind++;
            _optpos = 0;
        }
    }

    return optopt;
}

int getopt_long(int argc, char * const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
{
    optarg = NULL;

    if (optind >= argc)
        return -1;

    if (argv[optind] == NULL)
        return -1;

    /* Check for long option (--name or --name=value) */
    if (argv[optind][0] == '-' && argv[optind][1] == '-' && argv[optind][2] != '\0') {
        const char *arg = &argv[optind][2];
        size_t namelen;
        const char *eq = strchr(arg, '=');

        if (eq)
            namelen = (size_t)(eq - arg);
        else
            namelen = strlen(arg);

        /* Search longopts for a match */
        int match = -1;
        int ambiguous = 0;
        for (int i = 0; longopts[i].name != NULL; i++) {
            if (strncmp(longopts[i].name, arg, namelen) == 0) {
                if (strlen(longopts[i].name) == namelen) {
                    /* Exact match */
                    match = i;
                    ambiguous = 0;
                    break;
                }
                if (match >= 0) {
                    ambiguous = 1;
                } else {
                    match = i;
                }
            }
        }

        if (ambiguous) {
            if (opterr)
                fprintf(stderr, "%s: option '--%.*s' is ambiguous\n",
                        argv[0], (int)namelen, arg);
            optind++;
            return '?';
        }

        if (match >= 0) {
            const struct option *o = &longopts[match];
            optind++;

            if (eq) {
                /* --name=value */
                if (o->has_arg == no_argument) {
                    if (opterr)
                        fprintf(stderr,
                                "%s: option '--%s' doesn't allow an argument\n",
                                argv[0], o->name);
                    return '?';
                }
                optarg = (char *)(eq + 1);
            } else if (o->has_arg == required_argument) {
                if (optind < argc) {
                    optarg = argv[optind++];
                } else {
                    if (opterr)
                        fprintf(stderr,
                                "%s: option '--%s' requires an argument\n",
                                argv[0], o->name);
                    return (optstring && optstring[0] == ':') ? ':' : '?';
                }
            }

            if (longindex)
                *longindex = match;
            if (o->flag) {
                *o->flag = o->val;
                return 0;
            }
            return o->val;
        }

        /* Unknown long option */
        if (opterr)
            fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                    argv[0], (int)namelen, arg);
        optind++;
        return '?';
    }

    /* Fall through to short option processing */
    return getopt(argc, argv, optstring);
}
