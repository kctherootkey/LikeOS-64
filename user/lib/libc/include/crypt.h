/*
 * crypt.h - password hashing for LikeOS
 *
 * Declares crypt()/crypt_r().  The implementation lives in libcrypt.so and
 * hashes with yescrypt ($y$).
 */
#ifndef _CRYPT_H
#define _CRYPT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reentrant scratch/output storage for crypt_r().  Only `output` is meaningful
 * to callers (it holds the returned NUL-terminated hash string); the remaining
 * fields exist so a struct crypt_data may be reused across calls.  Set
 * `initialized` to 0 before the first use.
 */
struct crypt_data {
    char output[256];   /* returned hash string */
    char setting[256];  /* scratch */
    char input[256];    /* scratch */
    char reserved[64];  /* scratch */
    int  initialized;
};

/*
 * Hash `key` using the algorithm and salt encoded in `setting`.  `setting` may
 * be a bare salt prefix (e.g. "$y$j9T$<salt>") or a full stored hash string;
 * in the latter case the trailing digest is ignored, so crypt() can be used to
 * verify a password by comparing its result against the stored hash.
 *
 * Returns a pointer to the NUL-terminated result (static storage for crypt(),
 * `data->output` for crypt_r()), or NULL on error with errno set.
 */
char *crypt(const char *key, const char *setting);
char *crypt_r(const char *key, const char *setting, struct crypt_data *data);

/*
 * Generate a fresh salt/setting string for a new password hash.
 *
 * `prefix` selects the hashing method ("$y$" or NULL for the yescrypt default).
 * `count` is a cost hint (0 = default).  `rbytes`/`nrbytes` optionally supply
 * caller entropy; if NULL/insufficient, the kernel CSPRNG (getrandom) is used,
 * so every generated salt is cryptographically random and unique.
 *
 * Returns a NUL-terminated setting string (e.g. "$y$j9T$<random-salt>") to be
 * passed to crypt(); NULL on error with errno set.  crypt_gensalt() uses static
 * storage; crypt_gensalt_rn() writes into the caller's buffer.
 */
#define CRYPT_GENSALT_OUTPUT_SIZE 192

char *crypt_gensalt(const char *prefix, unsigned long count,
                    const char *rbytes, int nrbytes);
char *crypt_gensalt_rn(const char *prefix, unsigned long count,
                       const char *rbytes, int nrbytes,
                       char *output, int output_size);

#ifdef __cplusplus
}
#endif

#endif /* _CRYPT_H */
