/*
 * regex.c - very small POSIX regex shim for LikeOS-64.
 *
 * Tmux and a few other ports want regcomp() / regexec() / regfree() but a
 * full POSIX regex engine is far too large for this libc.  This shim
 * compiles the pattern as a literal string, so basic searches still work
 * and metacharacters are matched verbatim.  REG_ICASE is honoured.
 *
 * It is good enough to keep tmux's incremental copy-mode search and
 * other "find this word" features functional - applications that rely on
 * full POSIX BRE/ERE semantics will simply see substring matches.
 */

#include "../../include/regex.h"
#include "../../include/string.h"
#include "../../include/stdlib.h"
#include "../../include/ctype.h"

struct re_dfa_t {
    char *pattern;
    size_t len;
    int icase;
};

int regcomp(regex_t *preg, const char *pattern, int cflags) {
    if (!preg || !pattern) return REG_BADPAT;
    struct re_dfa_t *d = (struct re_dfa_t *)malloc(sizeof(*d));
    if (!d) return REG_ESPACE;
    size_t n = strlen(pattern);
    d->pattern = (char *)malloc(n + 1);
    if (!d->pattern) { free(d); return REG_ESPACE; }
    memcpy(d->pattern, pattern, n + 1);
    d->len = n;
    d->icase = (cflags & REG_ICASE) ? 1 : 0;

    /* Stash inside the opaque regex_t fields. */
    preg->__buffer = d;
    preg->re_nsub = 0;
    preg->__allocated = (size_t)sizeof(*d);
    preg->__used = n;
    preg->__syntax = 0;
    preg->__fastmap = 0;
    preg->__translate = 0;
    return 0;
}

static int ci_eq(unsigned char a, unsigned char b, int icase) {
    if (a == b) return 1;
    if (!icase) return 0;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
    return a == b;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags) {
    (void)eflags;
    if (!preg || !string) return REG_NOMATCH;
    struct re_dfa_t *d = (struct re_dfa_t *)preg->__buffer;
    if (!d) return REG_NOMATCH;

    size_t slen = strlen(string);
    if (d->len == 0) {
        if (nmatch >= 1) { pmatch[0].rm_so = 0; pmatch[0].rm_eo = 0; }
        return 0;
    }
    for (size_t i = 0; i + d->len <= slen; i++) {
        size_t k = 0;
        while (k < d->len &&
               ci_eq((unsigned char)string[i + k],
                     (unsigned char)d->pattern[k], d->icase)) {
            k++;
        }
        if (k == d->len) {
            if (nmatch >= 1) {
                pmatch[0].rm_so = (regoff_t)i;
                pmatch[0].rm_eo = (regoff_t)(i + d->len);
            }
            for (size_t j = 1; j < nmatch; j++) {
                pmatch[j].rm_so = -1;
                pmatch[j].rm_eo = -1;
            }
            return 0;
        }
    }
    return REG_NOMATCH;
}

void regfree(regex_t *preg) {
    if (!preg) return;
    struct re_dfa_t *d = (struct re_dfa_t *)preg->__buffer;
    if (d) {
        free(d->pattern);
        free(d);
        preg->__buffer = 0;
    }
}

size_t regerror(int errcode, const regex_t *preg,
                char *errbuf, size_t errbuf_size) {
    (void)preg;
    const char *m;
    switch (errcode) {
    case 0:            m = "Success"; break;
    case REG_NOMATCH:  m = "No match"; break;
    case REG_BADPAT:   m = "Invalid regular expression"; break;
    case REG_ESPACE:   m = "Out of memory"; break;
    default:           m = "Regex error"; break;
    }
    size_t mlen = strlen(m);
    if (errbuf && errbuf_size > 0) {
        size_t copy = mlen < errbuf_size - 1 ? mlen : errbuf_size - 1;
        memcpy(errbuf, m, copy);
        errbuf[copy] = '\0';
    }
    return mlen + 1;
}
