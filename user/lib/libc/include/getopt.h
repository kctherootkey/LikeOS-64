/*
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _GETOPT_H
#define _GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

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
/* Like getopt_long(), but a single dash introduces a long option too
 * ("-name"); an argument that matches no long option falls back to the
 * short options. */
int getopt_long_only(int argc, char * const argv[], const char *optstring,
                     const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif
