/*
 * likeos-hash.h - the message digests nvme-cli's RPMB commands need.
 *
 * nvme-rpmb.c computes the RPMB request MAC (HMAC-SHA256 over the frame
 * with the device's authentication key) and its nonces (MD5 of a random
 * number).  Elsewhere it asks the kernel's crypto socket interface for
 * them; LikeOS has no such interface, so patch 0002 computes them here, in
 * the program.  Standard algorithms: SHA-256 (FIPS 180-4), HMAC (RFC 2104),
 * MD5 (RFC 1321).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef LIKEOS_HASH_H
#define LIKEOS_HASH_H

#include <stddef.h>
#include <stdint.h>

#define LIKEOS_SHA256_SIZE 32
#define LIKEOS_MD5_SIZE 16

void likeos_sha256(const void *data, size_t len, uint8_t out[LIKEOS_SHA256_SIZE]);
void likeos_hmac_sha256(const void *key, size_t keylen, const void *data,
			size_t len, uint8_t out[LIKEOS_SHA256_SIZE]);
void likeos_md5(const void *data, size_t len, uint8_t out[LIKEOS_MD5_SIZE]);

#endif /* LIKEOS_HASH_H */
