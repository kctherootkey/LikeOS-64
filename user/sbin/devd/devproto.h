// devd protocol helpers: the text-line encoding shared by devd, devctl and
// the session's volume agent.
//
// A line is at most DEVP_LINE_MAX bytes including its '\n'.  Tokens are
// separated by exactly one space; key/value tokens are `key=value', and a
// value has every byte outside 0x21..0x7e, and '%' and '=', written as %XX
// (two uppercase hex digits).  Nothing here allocates or touches a file:
// these functions only compute, so they are tested on the build machine.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef DEVD_DEVPROTO_H
#define DEVD_DEVPROTO_H

#include <stddef.h>
#include <stdint.h>

#define DEVP_SOCKET_PATH "/var/run/devd/socket"
#define DEVP_LINE_MAX 4096 /* bytes, the '\n' included */
#define DEVP_MAX_TOKENS 32
#define DEVP_VERSION 1

/* Encode `n' bytes of `src' into `dst' (NUL-terminated).  Returns the length
 * the encoded text needs (without the NUL); when that is >= cap the output
 * is cut at a whole escape and the caller should treat it as too long. */
size_t devp_encode(char *dst, size_t cap, const char *src, size_t n);

/* Decode a value.  Returns the decoded length (dst NUL-terminated; it may
 * contain NUL bytes in between, which callers that want a C string must
 * reject), or -1 when the text is malformed (a bad escape, a byte that
 * should have been escaped) or does not fit in `cap' - 1 bytes. */
long devp_decode(char *dst, size_t cap, const char *src);

/* Split a line (no '\n') into tokens in place.  Returns the count, or -1 when
 * the line has an empty token (two spaces, a leading or trailing space), a
 * NUL or control byte, or more than `max' tokens. */
int devp_split(char *line, char **tok, int max);

/* For `key=value' tokens: the value of `key' among tok[first..n-1], or NULL.
 * A token with that key appearing twice makes *dup 1. */
const char *devp_find(char *const *tok, int n, int first, const char *key,
		      int *dup);

/* A line under construction.  The add functions do nothing once the line
 * overflowed; devp_line_end() then returns -1. */
struct devp_line {
	char buf[DEVP_LINE_MAX];
	size_t len;
	int overflow;
};

void devp_line_begin(struct devp_line *l, const char *word);
void devp_line_word(struct devp_line *l, const char *word);
void devp_line_kv(struct devp_line *l, const char *key, const char *value);
void devp_line_kv_u64(struct devp_line *l, const char *key, uint64_t v);
void devp_line_kv_int(struct devp_line *l, const char *key, long long v);
/* Append the encoded form of `value' as a bare token (an encoded message). */
void devp_line_enc(struct devp_line *l, const char *value);
/* Terminate with '\n'; returns the length, or -1 when it did not fit. */
long devp_line_end(struct devp_line *l);

/* A MOUNT request, split: "MOUNT <name>", or "MOUNT <name> recover" to
 * have a journal replayed that must be (the user agreed).  0 and *recover,
 * or -1 when it is neither. */
int devp_mount_parse(char *const *tok, int n, int *recover);

/* A CANCEL request, split: "CANCEL <name>" (stop the format of the volume
 * <name>).  0, or -1 when it is not one. */
int devp_cancel_parse(char *const *tok, int n);

/* The conventional name of an errno value ("EPERM"), "EIO" when unknown. */
const char *devp_errname(int err);

/* Strict unsigned decimal: digits only, no sign, no blanks; 0 on success. */
int devp_parse_u64(const char *s, uint64_t max, uint64_t *out);

/* The flags of a DEV/EVENT line that decide what a client may do with a
 * device by itself.  A line without one of the keys (an older devd) counts
 * it as 0. */
struct devp_flags {
	int usb, removable, system, supported, ejected;
	int readonly, optical, internal;
};

/* Mount it without being asked?  A USB drive or a CD/DVD that devd can
 * mount, not ejected, not the system's -- never a built-in disk.  The rule
 * the session's volume agent follows. */
int devp_automount(const struct devp_flags *f);

#endif
