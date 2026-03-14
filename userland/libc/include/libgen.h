/*
 * libgen.h - pathname manipulation for LikeOS
 */
#ifndef _LIBGEN_H
#define _LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the directory component of a pathname.
 * NOTE: may modify the input string. */
static inline char *dirname(char *path)
{
    static char dot[] = ".";
    if (!path || !*path)
        return dot;

    /* Find last slash */
    char *last_slash = (char *)0;
    for (char *p = path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (!last_slash)
        return dot;

    /* Handle root */
    if (last_slash == path)
        return "/";

    *last_slash = '\0';
    return path;
}

/* Returns the filename component of a pathname.
 * NOTE: may modify the input string. */
static inline char *basename(char *path)
{
    static char dot[] = ".";
    if (!path || !*path)
        return dot;

    /* Remove trailing slashes */
    char *end = path + __builtin_strlen(path) - 1;
    while (end > path && *end == '/')
        *end-- = '\0';

    /* Find last slash */
    char *last_slash = (char *)0;
    for (char *p = path; *p; p++) {
        if (*p == '/')
            last_slash = p;
    }

    if (last_slash)
        return last_slash + 1;
    return path;
}

#ifdef __cplusplus
}
#endif

#endif /* _LIBGEN_H */
