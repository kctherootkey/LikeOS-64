// LikeOS-64 storage bootstrap
// Handles FAT32 probing and signature selection for root storage

#ifndef _KERNEL_STORAGE_H_
#define _KERNEL_STORAGE_H_

#include "fat32.h"
#include "block.h"

typedef struct {
    int signature_found;
    unsigned int tested_mask;
    fat32_fs_t fs_instances[BLOCK_MAX_DEVICES];
    unsigned char ready_reads[BLOCK_MAX_DEVICES];
} storage_fs_state_t;

void storage_fs_init(storage_fs_state_t* state);

// Attempt mounts and signature checks; call periodically after XHCI init
void storage_fs_poll(storage_fs_state_t* state);

#endif // _KERNEL_STORAGE_H_
