/* getline.c / getdelim.c - line-reading functions for LikeOS libc */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

ssize_t getdelim(char **lineptr, size_t *n, int delimiter, FILE *stream)
{
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }

    if (*lineptr == NULL || *n == 0) {
        *n = 120;
        char *newp = realloc(*lineptr, *n);
        if (!newp) {
            errno = ENOMEM;
            return -1;
        }
        *lineptr = newp;
    }

    ssize_t cur_len = 0;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (cur_len == 0) return -1;
            break;
        }

        /* Grow buffer if needed */
        if ((size_t)(cur_len + 1) >= *n) {
            size_t needed = *n * 2 + 1;
            if (needed > (size_t)SSIZE_MAX) needed = (size_t)SSIZE_MAX;
            if ((size_t)(cur_len + 1) >= needed) {
                errno = EOVERFLOW;
                return -1;
            }
            char *newp = realloc(*lineptr, needed);
            if (!newp) {
                errno = ENOMEM;
                return -1;
            }
            *lineptr = newp;
            *n = needed;
        }

        (*lineptr)[cur_len++] = (char)c;
        if (c == delimiter) break;
    }

    (*lineptr)[cur_len] = '\0';
    return cur_len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    return getdelim(lineptr, n, '\n', stream);
}
