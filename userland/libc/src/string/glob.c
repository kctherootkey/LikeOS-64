/*
 * glob.c - minimal glob() implementation for LikeOS
 *
 * Supports basic wildcard patterns (* and ?) in filenames.
 * Used by GNU nano for nanorc "include" directives.
 */

#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fnmatch.h>

/* Simple glob: split pattern into directory + filename parts,
 * opendir the directory, and fnmatch each entry. */

static char *_glob_dirname(const char *path, char *buf, size_t bufsz)
{
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash) {
        buf[0] = '.';
        buf[1] = '\0';
    } else if (last_slash == path) {
        buf[0] = '/';
        buf[1] = '\0';
    } else {
        size_t len = (size_t)(last_slash - path);
        if (len >= bufsz) len = bufsz - 1;
        memcpy(buf, path, len);
        buf[len] = '\0';
    }
    return buf;
}

static const char *_glob_basename(const char *path)
{
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    return last_slash ? last_slash + 1 : path;
}

static int _has_glob_chars(const char *s)
{
    while (*s) {
        if (*s == '*' || *s == '?' || *s == '[')
            return 1;
        s++;
    }
    return 0;
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *epath, int eerrno),
         glob_t *pglob)
{
    if (!pglob) return GLOB_NOSPACE;

    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
    }

    /* If no glob characters, just check if the file exists */
    if (!_has_glob_chars(pattern)) {
        struct stat st;
        if (stat(pattern, &st) == 0) {
            /* File exists - add it */
            size_t newcount = pglob->gl_pathc + 1;
            char **newv = realloc(pglob->gl_pathv, (newcount + 1) * sizeof(char *));
            if (!newv) return GLOB_NOSPACE;
            pglob->gl_pathv = newv;
            pglob->gl_pathv[pglob->gl_pathc] = strdup(pattern);
            if (!pglob->gl_pathv[pglob->gl_pathc]) return GLOB_NOSPACE;
            pglob->gl_pathc = newcount;
            pglob->gl_pathv[newcount] = NULL;
            return 0;
        }
        if (flags & GLOB_NOCHECK) {
            /* Return the pattern itself */
            size_t newcount = pglob->gl_pathc + 1;
            char **newv = realloc(pglob->gl_pathv, (newcount + 1) * sizeof(char *));
            if (!newv) return GLOB_NOSPACE;
            pglob->gl_pathv = newv;
            pglob->gl_pathv[pglob->gl_pathc] = strdup(pattern);
            if (!pglob->gl_pathv[pglob->gl_pathc]) return GLOB_NOSPACE;
            pglob->gl_pathc = newcount;
            pglob->gl_pathv[newcount] = NULL;
            return 0;
        }
        return GLOB_NOMATCH;
    }

    char dirpath[4096];
    _glob_dirname(pattern, dirpath, sizeof(dirpath));
    const char *base_pattern = _glob_basename(pattern);

    DIR *dir = opendir(dirpath);
    if (!dir) {
        if (errfunc && (flags & GLOB_ERR)) {
            errfunc(dirpath, errno);
        }
        if (flags & GLOB_NOCHECK) {
            size_t newcount = pglob->gl_pathc + 1;
            char **newv = realloc(pglob->gl_pathv, (newcount + 1) * sizeof(char *));
            if (!newv) return GLOB_NOSPACE;
            pglob->gl_pathv = newv;
            pglob->gl_pathv[pglob->gl_pathc] = strdup(pattern);
            if (!pglob->gl_pathv[pglob->gl_pathc]) return GLOB_NOSPACE;
            pglob->gl_pathc = newcount;
            pglob->gl_pathv[newcount] = NULL;
            return 0;
        }
        return GLOB_ABORTED;
    }

    struct dirent *ent;
    int matched = 0;
    while ((ent = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        /* Skip hidden files unless pattern starts with . */
        if (ent->d_name[0] == '.' && base_pattern[0] != '.')
            continue;

        if (fnmatch(base_pattern, ent->d_name, 0) == 0) {
            /* Match found - construct full path */
            char fullpath[4096];
            if (dirpath[0] == '.' && dirpath[1] == '\0')
                snprintf(fullpath, sizeof(fullpath), "%s", ent->d_name);
            else
                snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);

            size_t newcount = pglob->gl_pathc + 1;
            char **newv = realloc(pglob->gl_pathv, (newcount + 1) * sizeof(char *));
            if (!newv) {
                closedir(dir);
                return GLOB_NOSPACE;
            }
            pglob->gl_pathv = newv;
            pglob->gl_pathv[pglob->gl_pathc] = strdup(fullpath);
            if (!pglob->gl_pathv[pglob->gl_pathc]) {
                closedir(dir);
                return GLOB_NOSPACE;
            }
            pglob->gl_pathc = newcount;
            pglob->gl_pathv[newcount] = NULL;
            matched = 1;
        }
    }
    closedir(dir);

    if (!matched) {
        if (flags & GLOB_NOCHECK) {
            size_t newcount = pglob->gl_pathc + 1;
            char **newv = realloc(pglob->gl_pathv, (newcount + 1) * sizeof(char *));
            if (!newv) return GLOB_NOSPACE;
            pglob->gl_pathv = newv;
            pglob->gl_pathv[pglob->gl_pathc] = strdup(pattern);
            if (!pglob->gl_pathv[pglob->gl_pathc]) return GLOB_NOSPACE;
            pglob->gl_pathc = newcount;
            pglob->gl_pathv[newcount] = NULL;
            return 0;
        }
        return GLOB_NOMATCH;
    }

    return 0;
}

void globfree(glob_t *pglob)
{
    if (!pglob) return;
    if (pglob->gl_pathv) {
        for (size_t i = 0; i < pglob->gl_pathc; i++)
            free(pglob->gl_pathv[i]);
        free(pglob->gl_pathv);
    }
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}
