// LikeOS-64 - FAT32 read-only skeleton
#ifndef LIKEOS_FAT32_H
#define LIKEOS_FAT32_H

#include "status.h"
#include "block.h"
#include "vfs.h"

typedef struct {
    const block_device_t* bdev;
    unsigned long fat_start_lba;
    unsigned long data_start_lba;
    unsigned int sectors_per_cluster;
    unsigned int bytes_per_sector;
    unsigned int root_cluster;
} fat32_fs_t;

typedef struct {
    vfs_file_t vfs; // must be first for casting
    fat32_fs_t* fs;
    unsigned long start_cluster;
    unsigned long size;
    unsigned long pos;
    unsigned long current_cluster; // cluster we have loaded
} fat32_file_t;

int fat32_mount(const block_device_t* bdev, fat32_fs_t* out);
int fat32_vfs_register_root(fat32_fs_t* fs);
void fat32_list_root(void (*cb)(const char*, unsigned attr, unsigned long size));
void fat32_debug_dump_root(void);
int fat32_dir_list(unsigned long cluster, void (*cb)(const char*, unsigned attr, unsigned long size));
int fat32_dir_find(unsigned long start_cluster, const char* name, unsigned* attr, unsigned long* first_cluster, unsigned long* size);
unsigned long fat32_root_cluster(void);
void fat32_set_cwd(unsigned long cluster);
unsigned long fat32_get_cwd(void);
// Extended path resolution (supports relative, '.', '..')
int fat32_resolve_path(unsigned long start_cluster, const char* path, unsigned* attr, unsigned long* first_cluster, unsigned long* size);
// Metadata/stat helper
int fat32_stat(unsigned long start_cluster, const char* path, unsigned* attr, unsigned long* first_cluster, unsigned long* size);
// Get parent cluster of directory (returns same cluster for root)
unsigned long fat32_parent_cluster(unsigned long dir_cluster);

#endif // LIKEOS_FAT32_H
