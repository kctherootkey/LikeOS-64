/*
 * <sys/random.h> -- getrandom(2).
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spelled exactly as <unistd.h> spells them, so a file that includes both
 * sees one definition rather than a conflicting redefinition. */
#define GRND_NONBLOCK   0x0001u
#define GRND_RANDOM     0x0002u
#define GRND_INSECURE   0x0004u

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int getentropy(void *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RANDOM_H */
