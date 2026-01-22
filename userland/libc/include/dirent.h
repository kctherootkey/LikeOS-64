#ifndef _DIRENT_H
#define _DIRENT_H

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

typedef struct {
    int fd;
    int buf_pos;
    int buf_len;
    char buf[1024];
    struct dirent current;
} DIR;

DIR* opendir(const char* name);
struct dirent* readdir(DIR* dirp);
int closedir(DIR* dirp);

#endif
