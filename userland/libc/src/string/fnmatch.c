#include "../../include/fnmatch.h"
#include "../../include/string.h"

static int match_class(const char **pp, char c, int casefold)
{
    const char *p = *pp;  /* points just past '[' */
    int negate = 0;
    int found  = 0;

    if (*p == '!' || *p == '^') { negate = 1; p++; }

    for (int first = 1; *p && *p != ']'; first = 0) {
        char lo = *p++;
        char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;          /* skip '-' */
            hi = *p++;
        }
        unsigned char uc = (unsigned char)c;
        unsigned char ulo = (unsigned char)lo;
        unsigned char uhi = (unsigned char)hi;
        if (casefold) {
            if (uc >= 'A' && uc <= 'Z') uc += 32;
            if (ulo >= 'A' && ulo <= 'Z') ulo += 32;
            if (uhi >= 'A' && uhi <= 'Z') uhi += 32;
        }
        if (uc >= ulo && uc <= uhi) found = 1;
        (void)first;
    }
    if (*p == ']') p++;
    *pp = p;
    return negate ? !found : found;
}

static int do_fnmatch(const char *pat, const char *str, int flags)
{
    const char *p = pat;
    const char *s = str;
    int casefold = flags & FNM_CASEFOLD;

    while (*p) {
        if (*p == '*') {
            /* Skip consecutive '*' */
            while (*p == '*') p++;
            if (!*p) return 0;   /* trailing '*' matches everything */
            /* Try matching rest from each position */
            for (; *s; s++) {
                if ((flags & FNM_PATHNAME) && *s == '/') break;
                if (do_fnmatch(p, s, flags) == 0) return 0;
            }
            return do_fnmatch(p, s, flags);
        } else if (*p == '?') {
            if (!*s) return FNM_NOMATCH;
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            p++; s++;
        } else if (*p == '[') {
            if (!*s) return FNM_NOMATCH;
            p++;   /* skip '[' */
            if (!match_class(&p, *s, casefold)) return FNM_NOMATCH;
            s++;
        } else if (*p == '\\' && !(flags & FNM_NOESCAPE)) {
            p++;
            if (!*p) return FNM_NOMATCH;
            char pc = *p, sc = *s;
            if (casefold) {
                if (pc >= 'A' && pc <= 'Z') pc += 32;
                if (sc >= 'A' && sc <= 'Z') sc += 32;
            }
            if (pc != sc) return FNM_NOMATCH;
            p++; s++;
        } else {
            if (!*s) return FNM_NOMATCH;
            char pc = *p, sc = *s;
            if (casefold) {
                if (pc >= 'A' && pc <= 'Z') pc += 32;
                if (sc >= 'A' && sc <= 'Z') sc += 32;
            }
            if (pc != sc) return FNM_NOMATCH;
            p++; s++;
        }
    }
    return *s ? FNM_NOMATCH : 0;
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string) return FNM_NOMATCH;

    /* FNM_PERIOD: leading dot must be matched explicitly */
    if ((flags & FNM_PERIOD) && *string == '.') {
        if (*pattern != '.') return FNM_NOMATCH;
    }

    return do_fnmatch(pattern, string, flags);
}
