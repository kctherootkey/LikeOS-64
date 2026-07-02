/*
 * crypt.c - crypt()/crypt_r() for LikeOS (libcrypt.so)
 *
 * Thin dispatcher over the vendored Openwall yescrypt reference implementation.
 * Recognizes the yescrypt ($y$) and classic scrypt ($7$) settings; both are
 * handled by yescrypt_r().
 */
#include <crypt.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "yescrypt.h"

/* Default cost for newly generated yescrypt salts.  These parameters encode to
 * the "$y$j9T$" prefix (flags=YESCRYPT_DEFAULTS, N=4096, r=32, p=1). */
#define GENSALT_RANDOM_BYTES 16

char *crypt_r(const char *key, const char *setting, struct crypt_data *data)
{
	if (!key || !setting || !data) {
		errno = EINVAL;
		return NULL;
	}

	/* yescrypt ($y$) and classic scrypt ($7$) share yescrypt_r(). */
	if (setting[0] == '$' && (setting[1] == 'y' || setting[1] == '7') &&
	    setting[2] == '$') {
		yescrypt_local_t local;
		uint8_t *retval;

		if (yescrypt_init_local(&local)) {
			errno = ENOMEM;
			return NULL;
		}
		retval = yescrypt_r(NULL, &local,
		    (const uint8_t *)key, strlen(key),
		    (const uint8_t *)setting, NULL,
		    (uint8_t *)data->output, sizeof(data->output));
		if (yescrypt_free_local(&local) || !retval) {
			errno = EINVAL;
			return NULL;
		}
		return data->output;
	}

	/* Unsupported hashing method. */
	errno = EINVAL;
	return NULL;
}

char *crypt(const char *key, const char *setting)
{
	static struct crypt_data d;
	return crypt_r(key, setting, &d);
}

/* Fill `buf` with `n` cryptographically secure random bytes from the kernel
 * CSPRNG (getrandom).  Returns 0 on success, -1 on error. */
static int secure_random(unsigned char *buf, size_t n)
{
	size_t off = 0;
	while (off < n) {
		ssize_t got = getrandom(buf + off, n - off, 0);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (got == 0)
			return -1;
		off += (size_t)got;
	}
	return 0;
}

char *crypt_gensalt_rn(const char *prefix, unsigned long count,
                       const char *rbytes, int nrbytes,
                       char *output, int output_size)
{
	/* Default yescrypt parameters -> "$y$j9T$" prefix. */
	yescrypt_params_t params = {
		.flags = YESCRYPT_DEFAULTS,
		.N = 4096,
		.r = 32,
		.p = 1,
		.t = 0,
		.g = 0,
		.NROM = 0,
	};
	unsigned char randbuf[GENSALT_RANDOM_BYTES];
	uint8_t *s;

	(void)count; /* single supported cost for now */

	if (!output || output_size <= 0) {
		errno = EINVAL;
		return NULL;
	}
	/* Only yescrypt is supported; a NULL prefix defaults to it. */
	if (prefix && strncmp(prefix, "$y$", 3) != 0) {
		errno = EINVAL;
		return NULL;
	}

	/* Salt entropy: use the caller's bytes if they supplied enough,
	 * otherwise draw fresh bytes from the kernel CSPRNG. */
	if (!rbytes || nrbytes < GENSALT_RANDOM_BYTES) {
		if (secure_random(randbuf, sizeof(randbuf)) != 0) {
			errno = EIO;
			return NULL;
		}
		rbytes = (const char *)randbuf;
		nrbytes = sizeof(randbuf);
	}

	s = yescrypt_encode_params_r(&params, (const uint8_t *)rbytes,
	                             (size_t)nrbytes, (uint8_t *)output,
	                             (size_t)output_size);
	if (!s) {
		errno = EINVAL;
		return NULL;
	}
	return output;
}

char *crypt_gensalt(const char *prefix, unsigned long count,
                    const char *rbytes, int nrbytes)
{
	static char output[CRYPT_GENSALT_OUTPUT_SIZE];
	return crypt_gensalt_rn(prefix, count, rbytes, nrbytes, output,
	                        sizeof(output));
}
