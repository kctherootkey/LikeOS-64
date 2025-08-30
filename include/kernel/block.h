// LikeOS-64 - Block device abstraction
#ifndef LIKEOS_BLOCK_H
#define LIKEOS_BLOCK_H

#include "status.h"

#define BLOCK_MAX_DEVICES 8

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(block_device_t* dev, unsigned long lba, unsigned long count, void* buf);

struct block_device {
    const char* name;
    unsigned int sector_size; // bytes per sector
    unsigned long total_sectors;
    block_read_fn read;
    void* driver_data; // pointer to underlying msd/scsi device
};

int block_register(block_device_t* dev);
const block_device_t* block_get(int index);
int block_count(void);

#endif // LIKEOS_BLOCK_H
