/*
 * likeos-crypt - hash a password on the build host the way LikeOS's
 * passwd(1) does: yescrypt, LikeOS's own implementation (this directory),
 * a fresh random salt and the same default cost.
 *
 * Built for the HOST, not for LikeOS: the image recipe uses it to give an
 * account it creates at build time (make ADD_USER=<name>) a password the
 * system can check, without depending on whatever crypt the build machine
 * happens to have.  The password is read from standard input, not argv, so
 * it never shows in a process listing.
 *
 * Usage: printf %s secret | likeos-crypt   -> prints the hash
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "crypt.h"

int main(void)
{
	char pass[256], setting[CRYPT_GENSALT_OUTPUT_SIZE];
	struct crypt_data cd;
	size_t n = 0;
	ssize_t r;
	char *hash;

	while (n < sizeof(pass) - 1 &&
	       (r = read(0, pass + n, sizeof(pass) - 1 - n)) > 0)
		n += (size_t)r;
	pass[n] = '\0';
	if (n && pass[n - 1] == '\n')
		pass[--n] = '\0';
	if (!n) {
		fprintf(stderr, "likeos-crypt: empty password\n");
		return 1;
	}
	if (!crypt_gensalt_rn(NULL, 0, NULL, 0, setting, sizeof(setting))) {
		fprintf(stderr, "likeos-crypt: cannot make a salt\n");
		return 1;
	}
	memset(&cd, 0, sizeof(cd));
	hash = crypt_r(pass, setting, &cd);
	memset(pass, 0, sizeof(pass));
	if (!hash || hash[0] != '$') {
		fprintf(stderr, "likeos-crypt: hashing failed\n");
		return 1;
	}
	puts(hash);
	return 0;
}
