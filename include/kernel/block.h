// LikeOS-64 - Block device abstraction
#ifndef LIKEOS_BLOCK_H
#define LIKEOS_BLOCK_H

#include "status.h"

#define BLOCK_MAX_DEVICES 8

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(block_device_t* dev, unsigned long lba, unsigned long count, void* buf);
typedef int (*block_write_fn)(block_device_t* dev, unsigned long lba, unsigned long count, const void* buf);
typedef int (*block_sync_fn)(block_device_t* dev);

struct block_device {
    const char* name;
    unsigned int sector_size; // bytes per sector
    unsigned long total_sectors;
    block_read_fn read;
    block_write_fn write;
    block_sync_fn sync;  // Optional: flush write cache to media
    void* driver_data; // pointer to underlying msd/scsi device
};

int block_register(block_device_t* dev);
const block_device_t* block_get(int index);
int block_count(void);
int block_sync(block_device_t* dev);

#endif // LIKEOS_BLOCK_H
