#ifndef _GETOPT_H
#define _GETOPT_H

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char * const argv[], const char *optstring);

/* Long option support */
struct option {
    const char *name;   /* option name (without leading "--") */
    int has_arg;        /* no_argument, required_argument, optional_argument */
    int *flag;          /* if non-NULL, set *flag = val and return 0 */
    int val;            /* value to return (or store in *flag) */
};

#define no_argument        0
#define required_argument  1
#define optional_argument  2

int getopt_long(int argc, char * const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

#endif
