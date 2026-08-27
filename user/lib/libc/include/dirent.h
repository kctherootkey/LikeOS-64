#ifndef _DIRENT_H
#define _DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

struct dirent {
    uint64_t d_ino;
    uint64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

/* The explicit 64-bit spelling of the SAME structure.  This libc was 64-bit
 * from the start, so there is no distinct large-file variant to describe --
 * but the transitional LFS API names live on in portable code (libgpg-error
 * walks its descriptor table with getdents64 into struct dirent64), and a
 * program using that spelling must find a type whose layout is exactly what
 * getdents64() writes: this one. */
struct dirent64 {
    uint64_t d_ino;
    uint64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct {
    int fd;
    int buf_pos;
    int buf_len;
    char buf[1024];
    struct dirent current;
} DIR;

DIR* opendir(const char* name);
DIR* fdopendir(int fd);
struct dirent* readdir(DIR* dirp);
int readdir_r(DIR* dirp, struct dirent* entry, struct dirent** result);
int closedir(DIR* dirp);
void rewinddir(DIR* dirp);
int dirfd(DIR* dirp);
long telldir(DIR* dirp);
void seekdir(DIR* dirp, long loc);

/* scandir() reads a whole directory into a malloc'd array, optionally
 * filtering and sorting it.  Both the entries and the array itself are the
 * caller's to free().  alphasort/versionsort are the two standard
 * comparators. */
int scandir(const char* dirp, struct dirent*** namelist,
            int (*filter)(const struct dirent*),
            int (*compar)(const struct dirent**, const struct dirent**));
int alphasort(const struct dirent** a, const struct dirent** b);
int versionsort(const struct dirent** a, const struct dirent** b);

#ifdef __cplusplus
}
#endif

#endif
