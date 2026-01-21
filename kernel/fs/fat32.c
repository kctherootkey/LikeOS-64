// LikeOS-64 - FAT32 minimal implementation (root dir, 8.3 files, read/write)
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/block.h"
#include "../../include/kernel/syscall.h"

#ifndef FAT32_DEBUG_ENABLED
#define FAT32_DEBUG_ENABLED 0
#endif
#if FAT32_DEBUG_ENABLED
#define FAT32_LOG(fmt, ...) kprintf("FAT32 dbg: " fmt, ##__VA_ARGS__)
#else
#define FAT32_LOG(...) do { } while (0)
#endif

// BPB offsets (FAT32)
typedef struct __attribute__((packed)) {
    uint8_t	jmp[3];
    char	oem[8];
    uint16_t bytes_per_sector; /* 11 */
    uint8_t	sectors_per_cluster; /* 13 */
    uint16_t reserved_sector_count; /* 14 */
    uint8_t	num_fats; /* 16 */
    uint16_t root_entry_count; /* 17 (FAT12/16) */
    uint16_t total_sectors16; /* 19 */
    uint8_t	media; /* 21 */
    uint16_t fat_size16; /* 22 */
    uint16_t sectors_per_track; /* 24 */
    uint16_t num_heads; /* 26 */
    uint32_t hidden_sectors; /* 28 */
    uint32_t total_sectors32; /* 32 */
    uint32_t fat_size32; /* 36 */
    uint16_t ext_flags; /* 40 */
    uint16_t fs_version; /* 42 */
    uint32_t root_cluster; /* 44 */
    uint16_t fs_info; /* 48 */
    uint16_t bk_boot_sector; /* 50 */
    uint8_t	reserved[12];
    uint8_t	drive_number;
    uint8_t	reserved1;
    uint8_t	boot_signature;
    uint32_t volume_id;
    char	volume_label[11];
    char	fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crtTimeTenth;
    uint16_t crtTime;
    uint16_t crtDate;
    uint16_t lstAccDate;
    uint16_t fstClusHI;
    uint16_t wrtTime;
    uint16_t wrtDate;
    uint16_t fstClusLO;
    uint32_t fileSize;
} fat32_dirent_t;

// Long File Name entry
typedef struct __attribute__((packed)) {
    uint8_t seq; /* sequence number (bit 6 = last long entry) */
    uint16_t name1[5]; /* characters 1-5 */
    uint8_t attr; /* 0x0F */
    uint8_t type; /* 0 */
    uint8_t checksum; /* checksum of DOS name */
    uint16_t name2[6]; /* characters 6-11 */
    uint16_t fstClusLO; /* always 0 */
    uint16_t name3[2]; /* characters 12-13 */
} fat32_lfn_t;

#define FAT32_ATTR_LONG_NAME 0x0F

// Maximum sectors per single USB read (QEMU xHCI limitation)
#define MAX_SECTORS_PER_READ 1

static int read_sectors(const block_device_t *bdev, unsigned long lba, unsigned long count, void *buf)
{
    // Chunk large reads to work around QEMU xHCI DMA limitations
    unsigned long offset = 0;
    while (count > 0) {
        unsigned long chunk = (count > MAX_SECTORS_PER_READ) ? MAX_SECTORS_PER_READ : count;
        int st = ST_OK;
        int attempts = 5;
        while (attempts-- > 0) {
            st = bdev->read((block_device_t *)bdev, lba, chunk, (uint8_t *)buf + offset);
            if (st == ST_OK) {
                break;
            }
            // small backoff before retry
            for (volatile int spin = 0; spin < 10000; ++spin) {
                __asm__ __volatile__("pause");
            }
        }
        if (st != ST_OK) {
            kprintf("FAT32: read_sectors FAILED lba=%lu count=%lu st=%d\n", lba, chunk, st);
            return st;
        }
        lba += chunk;
        offset += chunk * 512;
        count -= chunk;
    }
    return ST_OK;
}

static int write_sectors(const block_device_t *bdev, unsigned long lba, unsigned long count, const void *buf)
{
    if (!bdev || !bdev->write) {
        return ST_UNSUPPORTED;
    }
    unsigned long offset = 0;
    while (count > 0) {
        unsigned long chunk = (count > MAX_SECTORS_PER_READ) ? MAX_SECTORS_PER_READ : count;
        int st = ST_OK;
        int attempts = 5;
        while (attempts-- > 0) {
            st = bdev->write((block_device_t *)bdev, lba, chunk, (const uint8_t *)buf + offset);
            if (st == ST_OK) {
                break;
            }
            for (volatile int spin = 0; spin < 10000; ++spin) {
                __asm__ __volatile__("pause");
            }
        }
        if (st != ST_OK) {
            kprintf("FAT32: write_sectors FAILED lba=%lu count=%lu st=%d\n", lba, chunk, st);
            return st;
        }
        lba += chunk;
        offset += chunk * 512;
        count -= chunk;
    }
    return ST_OK;
}

static unsigned long cluster_to_lba(fat32_fs_t *fs, unsigned long cluster)
{
    return fs->part_lba_offset + fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

// Forward declaration (defined later)
static unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster);

// Simple root directory cache (single cluster only for now)
static void* g_root_dir_cache = 0; // first cluster of root directory cached
static unsigned long g_root_dir_cluster = 0;
static void* g_fat_cache = 0; // cached first FAT
static unsigned long g_fat_cache_entries = 0;
static fat32_fs_t* g_root_fs = 0;
static fat32_fs_t g_static_fs; // internal singleton instance
static unsigned long g_cwd_cluster = 0; // 0 means root

static int fat32_ensure_fat_loaded(fat32_fs_t *fs)
{
    if (!fs)
        return ST_INVALID;
    if (g_fat_cache)
        return ST_OK;
    // trigger FAT cache load
    (void)fat32_next_cluster_cached(fs, 2);
    return g_fat_cache ? ST_OK : ST_IO;
}

static int fat32_fat_set(fat32_fs_t *fs, unsigned long cluster, uint32_t value)
{
    if (!fs || !g_fat_cache || cluster >= g_fat_cache_entries)
        return ST_INVALID;
    uint32_t *fat = (uint32_t *)g_fat_cache;
    fat[cluster] = value & 0x0FFFFFFF;

    unsigned long fat_byte = cluster * 4;
    unsigned long sector = fat_byte / fs->bytes_per_sector;
    unsigned long offset = fat_byte % fs->bytes_per_sector;
    unsigned long lba = fs->part_lba_offset + fs->fat_start_lba + sector;

    // write updated FAT sector (first FAT)
    if (write_sectors(fs->bdev, lba, 1, ((uint8_t *)g_fat_cache) + sector * fs->bytes_per_sector) != ST_OK) {
        return ST_IO;
    }

    // mirror to additional FATs if present
    for (unsigned int f = 1; f < fs->num_fats; ++f) {
        unsigned long lba2 = fs->part_lba_offset + fs->fat_start_lba + (f * fs->fat_size_sectors) + sector;
        if (write_sectors(fs->bdev, lba2, 1, ((uint8_t *)g_fat_cache) + sector * fs->bytes_per_sector) != ST_OK) {
            return ST_IO;
        }
    }

    (void)offset;
    return ST_OK;
}

static int fat32_alloc_cluster(fat32_fs_t *fs, unsigned long *out_cluster)
{
    if (!fs || !out_cluster)
        return ST_INVALID;
    if (fat32_ensure_fat_loaded(fs) != ST_OK)
        return ST_IO;
    uint32_t *fat = (uint32_t *)g_fat_cache;
    for (unsigned long c = 2; c < g_fat_cache_entries; ++c) {
        if ((fat[c] & 0x0FFFFFFF) == 0) {
            if (fat32_fat_set(fs, c, 0x0FFFFFFF) != ST_OK)
                return ST_IO;
            // zero new cluster on disk
            unsigned long lba = cluster_to_lba(fs, c);
            unsigned long sectors = fs->sectors_per_cluster;
            unsigned long bytes = sectors * fs->bytes_per_sector;
            void *zero = kcalloc(1, bytes);
            if (!zero)
                return ST_NOMEM;
            int st = write_sectors(fs->bdev, lba, sectors, zero);
            kfree(zero);
            if (st != ST_OK)
                return ST_IO;
            *out_cluster = c;
            return ST_OK;
        }
    }
    return ST_NOMEM;
}

static int fat32_free_chain(fat32_fs_t *fs, unsigned long start_cluster)
{
    if (!fs || start_cluster < 2)
        return ST_OK;
    if (fat32_ensure_fat_loaded(fs) != ST_OK)
        return ST_IO;
    unsigned long c = start_cluster;
    while (c >= 2 && c < 0x0FFFFFF8) {
        unsigned long next = fat32_next_cluster_cached(fs, c);
        if (fat32_fat_set(fs, c, 0) != ST_OK)
            return ST_IO;
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        c = next;
    }
    return ST_OK;
}

static int fat32_append_cluster(fat32_fs_t *fs, unsigned long last_cluster, unsigned long *out_new)
{
    unsigned long newc = 0;
    int st = fat32_alloc_cluster(fs, &newc);
    if (st != ST_OK)
        return st;
    if (last_cluster >= 2) {
        if (fat32_fat_set(fs, last_cluster, (uint32_t)newc) != ST_OK)
            return ST_IO;
    }
    *out_new = newc;
    return ST_OK;
}

static int fat32_make_83_name(const char *name, char out[11])
{
    if (!name || !out)
        return ST_INVALID;
    // skip path separators
    if (name[0] == '/') name++;
    if (!*name)
        return ST_INVALID;
    char base[9];
    char ext[4];
    int bi = 0, ei = 0;
    const char *dot = 0;
    for (const char *p = name; *p; ++p) {
        if (*p == '/')
            break;
        if (*p == '.') {
            if (!dot) dot = p;
            continue;
        }
        if (!dot) {
            if (bi >= 8) return ST_INVALID;
            base[bi++] = *p;
        } else {
            if (ei >= 3) return ST_INVALID;
            ext[ei++] = *p;
        }
    }
    for (int i = 0; i < 8; ++i) out[i] = ' ';
    for (int i = 0; i < 3; ++i) out[8 + i] = ' ';
    for (int i = 0; i < bi; ++i) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    for (int i = 0; i < ei; ++i) {
        char c = ext[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[8 + i] = c;
    }
    return ST_OK;
}

static int fat32_dir_find_entry(unsigned long start_cluster, const char *name83,
    unsigned *attr, unsigned long *first_cluster, unsigned long *size,
    unsigned long *out_cluster, unsigned int *out_index)
{
    if (!g_root_fs || !name83)
        return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    while (cluster < 0x0FFFFFF8) {
        void *buf = kalloc(cluster_size);
        if (!buf)
            return ST_NOMEM;
        if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        for (unsigned i = 0; i < entries; i++) {
            if (ents[i].name[0] == 0x00) {
                kfree(buf);
                return ST_NOT_FOUND;
            }
            if (ents[i].name[0] == 0xE5)
                continue;
            if (ents[i].attr == FAT32_ATTR_LONG_NAME)
                continue;
            int match = 1;
            for (int j = 0; j < 11; ++j) {
                if (ents[i].name[j] != (uint8_t)name83[j]) { match = 0; break; }
            }
            if (match) {
                if (attr) *attr = ents[i].attr;
                if (first_cluster) *first_cluster = ((unsigned long)ents[i].fstClusHI << 16) | ents[i].fstClusLO;
                if (size) *size = ents[i].fileSize;
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                kfree(buf);
                return ST_OK;
            }
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        cluster = next;
    }
    return ST_NOT_FOUND;
}

static int fat32_dir_find_free_entry(unsigned long start_cluster, unsigned long *out_cluster, unsigned int *out_index)
{
    if (!g_root_fs || !out_cluster || !out_index)
        return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    unsigned long last_cluster = start_cluster;
    while (cluster < 0x0FFFFFF8) {
        void *buf = kalloc(cluster_size);
        if (!buf)
            return ST_NOMEM;
        if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        for (unsigned i = 0; i < entries; i++) {
            if (ents[i].name[0] == 0x00 || ents[i].name[0] == 0xE5) {
                kfree(buf);
                *out_cluster = cluster;
                *out_index = i;
                return ST_OK;
            }
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0) {
            last_cluster = cluster;
            break;
        }
        cluster = next;
    }

    // allocate new directory cluster
    unsigned long newc = 0;
    if (fat32_append_cluster(g_root_fs, last_cluster, &newc) != ST_OK)
        return ST_IO;
    *out_cluster = newc;
    *out_index = 0;
    return ST_OK;
}

static int fat32_write_dirent(unsigned long dir_cluster, unsigned int index, const fat32_dirent_t *ent)
{
    if (!g_root_fs || !ent)
        return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
    if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    ents[index] = *ent;
    int st = write_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf);
    kfree(buf);
    return st;
}

static void fat32_init_dirent(fat32_dirent_t *ent, const char name83[11], uint8_t attr,
    unsigned long first_cluster, unsigned long size)
{
    mm_memset(ent, 0, sizeof(*ent));
    for (int i = 0; i < 11; ++i) {
        ent->name[i] = (uint8_t)name83[i];
    }
    ent->attr = attr;
    ent->fstClusHI = (uint16_t)((first_cluster >> 16) & 0xFFFF);
    ent->fstClusLO = (uint16_t)(first_cluster & 0xFFFF);
    ent->fileSize = (uint32_t)size;
}

static unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster)
{
    if (!g_fat_cache) {
        /* Derive FAT size (sectors) using difference to data_start and number of FATs (assume 2) approximated */
        unsigned long fat_total = fs->data_start_lba - fs->fat_start_lba; /* includes all FAT copies */
        if (fat_total == 0)
            return 0x0FFFFFFF;
        unsigned long first_fat_sectors = fat_total / 2;
        if (first_fat_sectors == 0)
            first_fat_sectors = fat_total; /* if only one FAT */

        /* Allocate FAT cache buffer */
        unsigned long fat_bytes = first_fat_sectors * fs->bytes_per_sector;

        /* Read FAT with a few retries for transient USB errors */
        for (int attempt = 0; attempt < 3 && !g_fat_cache; ++attempt) {
            g_fat_cache = kalloc(fat_bytes);
            if (!g_fat_cache) {
                kprintf("FAT32: FAT cache alloc failed!\n");
                return 0x0FFFFFFF;
            }

            /* Read FAT in chunks (max 1 sector = 512 bytes per read for QEMU xHCI compatibility) */
            #define FAT_READ_CHUNK 1
            unsigned long lba = fs->part_lba_offset + fs->fat_start_lba;
            unsigned long remaining = first_fat_sectors;
            uint8_t *dest = (uint8_t*)g_fat_cache;
            int read_failed = 0;

            while (remaining > 0) {
                unsigned long chunk = (remaining > FAT_READ_CHUNK) ? FAT_READ_CHUNK : remaining;
                int read_st = read_sectors(fs->bdev, lba, chunk, dest);
                if (read_st != ST_OK) {
                    kprintf("FAT32: FAT read failed at LBA %lu! st=%d\n", lba, read_st);
                    read_failed = 1;
                    break;
                }
                lba += chunk;
                dest += chunk * fs->bytes_per_sector;
                remaining -= chunk;
            }

            if (read_failed) {
                kfree(g_fat_cache);
                g_fat_cache = 0;
                g_fat_cache_entries = 0;
                for (volatile int spin = 0; spin < 50000; ++spin) {
                    __asm__ __volatile__("pause");
                }
                continue;
            }

            g_fat_cache_entries = fat_bytes / 4;
        }
    }
    if (!g_fat_cache)
        return 0x0FFFFFFF;
    if (cluster >= g_fat_cache_entries)
        return 0x0FFFFFFF;
    return ((uint32_t *)g_fat_cache)[cluster] & 0x0FFFFFFF;
}

// (globals moved above)
static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}
// (globals moved above)
// For now we don't maintain full parent map; parent of any dir except root collapses to root.

// Minimal BPB sanity check
static int fat32_validate_bpb(const fat32_bpb_t *bpb)
{
    if (!bpb) return 0;
    // Common jump opcodes: EB ?? 90  or  EB ?? EB  or  E9 ?? ??
    if (!( (bpb->jmp[0] == 0xEB && bpb->jmp[2] == 0x90) || bpb->jmp[0] == 0xE9 ))
        return 0;
    if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
        bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096)
        return 0;
    // Sectors per cluster must be power of two and <= 128 (FAT spec)
    if (bpb->sectors_per_cluster == 0 || (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) || bpb->sectors_per_cluster > 128)
        return 0;
    if (bpb->reserved_sector_count == 0)
        return 0;
    if (bpb->num_fats == 0 || bpb->num_fats > 2)
        return 0;
    if (bpb->root_cluster < 2)
        return 0;
    if (bpb->fat_size16 == 0 && bpb->fat_size32 == 0)
        return 0;
    // For FAT32 root_entry_count should be 0
    if (bpb->root_entry_count != 0)
        return 0;
    // fs_type field should contain 'FAT'
    if (!(bpb->fs_type[0] == 'F' && bpb->fs_type[1] == 'A' && bpb->fs_type[2] == 'T'))
        return 0;
    return 1;
}

static int fat32_mount_at_lba(const block_device_t *bdev, unsigned long base_lba, fat32_fs_t *out)
{
    // Invalidate old FAT cache (important for re-mounts or device changes)
    if (g_fat_cache) {
        kfree(g_fat_cache);
        g_fat_cache = 0;
        g_fat_cache_entries = 0;
    }
    
    void *sector = kalloc(512);
    if (!sector)
        return ST_NOMEM;
    if (read_sectors(bdev, base_lba, 1, sector) != ST_OK) {
        kfree(sector);
        return ST_IO;
    }
    fat32_bpb_t *bpb = (fat32_bpb_t *)sector;
    if (!fat32_validate_bpb(bpb)) {
        // Relaxed secondary acceptance (silent)
        if (!(bpb->bytes_per_sector == 512 || bpb->bytes_per_sector == 1024 || bpb->bytes_per_sector == 2048 || bpb->bytes_per_sector == 4096) ||
            bpb->sectors_per_cluster == 0 || bpb->reserved_sector_count == 0 || bpb->num_fats == 0 || bpb->root_cluster < 2 ||
            (bpb->fat_size16 == 0 && bpb->fat_size32 == 0)) {
            kfree(sector);
            return ST_ERR;
        }
    }
    unsigned long fat_sz = bpb->fat_size32 ? bpb->fat_size32 : bpb->fat_size16;
    unsigned long first_data_sector = bpb->reserved_sector_count + (bpb->num_fats * fat_sz);
    out->bdev = bdev;
    out->bytes_per_sector = bpb->bytes_per_sector;
    out->sectors_per_cluster = bpb->sectors_per_cluster;
    // Store fat_start_lba and data_start_lba relative to base (partition) so cluster_to_lba adds base once.
    out->fat_start_lba = bpb->reserved_sector_count;
    out->data_start_lba = first_data_sector;
    out->root_cluster = bpb->root_cluster;
    out->num_fats = bpb->num_fats;
    out->fat_size_sectors = fat_sz;
    out->part_lba_offset = base_lba;
    g_root_dir_cluster = out->root_cluster;
    g_root_fs = out;
    if (!g_root_dir_cache)
        g_root_dir_cache = kalloc(out->sectors_per_cluster * out->bytes_per_sector);
    if (g_root_dir_cache) {
        unsigned long root_lba = cluster_to_lba(out, out->root_cluster);
        unsigned long read_count = out->sectors_per_cluster ? out->sectors_per_cluster : 1;
        read_sectors(bdev, root_lba, read_count, g_root_dir_cache);
    }
    unsigned long abs_data = out->part_lba_offset + out->data_start_lba;
    unsigned long abs_fat = out->part_lba_offset + out->fat_start_lba;
    kprintf("FAT32: mounted %s base=%lu root=%lu\n", bdev->name, base_lba, out->root_cluster);
    kfree(sector);
    return ST_OK;
}

// Very small MBR partition entry structure
typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_first;
    uint32_t sectors;
} mbr_part_t;

int fat32_mount(const block_device_t *bdev, fat32_fs_t *out)
{
    if (!bdev || !out)
        return ST_INVALID;
    unsigned long candidates[16];
    int cand_count = 0;
    int protective_gpt = 0;

    void *mbr = kalloc(512);
    if (!mbr)
        return ST_NOMEM;
    if (read_sectors(bdev, 0, 1, mbr) != ST_OK) {
        kfree(mbr);
        return ST_IO;
    }
    uint8_t *m = (uint8_t *)mbr;
    int has_sig = (m[510] == 0x55 && m[511] == 0xAA);
    mbr_part_t *p = (mbr_part_t *)(m + 446);

    if (has_sig) {
        for (int i = 0; i < 4; i++) {
            if (p[i].type == 0xEE) protective_gpt = 1; // Protective GPT
        }
    }

    // Collect MBR partition starts first (skip protective entry itself)
    if (has_sig) {
        for (int i = 0; i < 4; i++) {
            if (p[i].lba_first && p[i].sectors && p[i].type != 0x00 && p[i].type != 0xEE) {
                int dup = 0; for (int j = 0; j < cand_count; j++) if (candidates[j] == p[i].lba_first) dup = 1;
                if (!dup && cand_count < (int)(sizeof(candidates)/sizeof(candidates[0]))) candidates[cand_count++] = p[i].lba_first;
            }
        }
    }

    // If GPT, parse GPT header to discover ESP (or any) partitions
    if (protective_gpt) {
        void *hdr = kalloc(512);
        if (hdr && read_sectors(bdev, 1, 1, hdr) == ST_OK) {
            uint8_t *h = (uint8_t *)hdr;
            if (h[0]=='E' && h[1]=='F' && h[2]=='I' && h[3]==' ' && h[4]=='P' && h[5]=='A' && h[6]=='R' && h[7]=='T') {
                uint32_t num_entries = *(uint32_t *)(h + 80);
                uint32_t entry_size = *(uint32_t *)(h + 84);
                uint64_t part_lba = *(uint64_t *)(h + 72); // partition entry starting LBA
                if (entry_size >= 128 && num_entries > 0 && num_entries < 512) {
                    uint32_t to_read = num_entries;
                    if (to_read > 32) to_read = 32; // cap
                    uint64_t bytes = (uint64_t)to_read * entry_size;
                    uint32_t sectors = (uint32_t)((bytes + 511) / 512);
                    void *pe = kalloc(sectors * 512);
                    if (pe && read_sectors(bdev, (unsigned long)part_lba, sectors, pe) == ST_OK) {
                        for (uint32_t i = 0; i < to_read; i++) {
                            uint8_t *e = (uint8_t *)pe + i * entry_size;
                            int empty = 1; for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
                            if (empty) continue;
                            // ESP type GUID little-endian: 28 73 2A C1 1F F8 D2 11 BA 4B 00 A0 C9 3E C9 3B
                            int is_esp = (e[0]==0x28&&e[1]==0x73&&e[2]==0x2A&&e[3]==0xC1&&e[4]==0x1F&&e[5]==0xF8&&e[6]==0xD2&&e[7]==0x11&&e[8]==0xBA&&e[9]==0x4B&&e[10]==0x00&&e[11]==0xA0&&e[12]==0xC9&&e[13]==0x3E&&e[14]==0xC9&&e[15]==0x3B);
                            uint64_t first_lba = *(uint64_t *)(e + 32);
                            if (first_lba && first_lba < 0xFFFFFFFFULL) {
                                // Add ESP first, others later
                                if (is_esp) {
                                    int dup = 0; for (int j = 0; j < cand_count; j++) if (candidates[j] == (unsigned long)first_lba) dup = 1;
                                    if (!dup && cand_count < (int)(sizeof(candidates)/sizeof(candidates[0]))) candidates[cand_count++] = (unsigned long)first_lba;
                                }
                            }
                        }
                        // Second pass: add any non-ESP we didn't add yet
                        for (uint32_t i = 0; i < to_read; i++) {
                            uint8_t *e = (uint8_t *)pe + i * entry_size;
                            int empty = 1; for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
                            if (empty) continue;
                            int is_esp = (e[0]==0x28&&e[1]==0x73&&e[2]==0x2A&&e[3]==0xC1&&e[4]==0x1F&&e[5]==0xF8&&e[6]==0xD2&&e[7]==0x11&&e[8]==0xBA&&e[9]==0x4B&&e[10]==0x00&&e[11]==0xA0&&e[12]==0xC9&&e[13]==0x3E&&e[14]==0xC9&&e[15]==0x3B);
                            if (is_esp) continue;
                            uint64_t first_lba = *(uint64_t *)(e + 32);
                            if (first_lba && first_lba < 0xFFFFFFFFULL) {
                                int dup = 0; for (int j = 0; j < cand_count; j++) if (candidates[j] == (unsigned long)first_lba) dup = 1;
                                if (!dup && cand_count < (int)(sizeof(candidates)/sizeof(candidates[0]))) candidates[cand_count++] = (unsigned long)first_lba;
                            }
                        }
                    }
                    if (pe) kfree(pe);
                }
            }
        }
        if (hdr) kfree(hdr);
    }

    // Only consider LBA0 as superfloppy last if NOT GPT and no conventional partitions found
    if (!protective_gpt) {
        int any_part = 0; for (int i = 0; i < 4; i++) if (p[i].type != 0) any_part = 1;
        if (!any_part) {
            // attempt superfloppy at 0 (add if not already from earlier logic)
            int dup = 0; for (int j = 0; j < cand_count; j++) if (candidates[j] == 0) dup = 1;
            if (!dup && cand_count < (int)(sizeof(candidates)/sizeof(candidates[0]))) candidates[cand_count++] = 0;
        }
    }
    kfree(mbr);

    // Also add sector 2048 heuristic if space left and not already included
    if (cand_count < (int)(sizeof(candidates)/sizeof(candidates[0]))) {
        int dup = 0; for (int j = 0; j < cand_count; j++) if (candidates[j] == 2048) dup = 1;
        if (!dup) candidates[cand_count++] = 2048;
    }

    /* (debug output removed) */
    fat32_fs_t best;
    int best_set = 0;
    int best_score = -999;
    for (int i = 0; i < cand_count; i++) {
        fat32_fs_t tmp;
        if (fat32_mount_at_lba(bdev, candidates[i], &tmp) == ST_OK) {
            int score = 0;
            if (candidates[i] != 0) score += 10; /* prefer partition */
            if (!best_set || score > best_score) { best = tmp; best_set = 1; best_score = score; }
        }
    }
    if (best_set) {
        *out = best;
        /* selection silent (summary printed by mount_at_lba) */
        return ST_OK;
    }
    kprintf("FAT32: no valid FAT32 volume found on %s\n", bdev->name);
    return ST_ERR;
}

// VFS bridge
static int fat32_match_name(const uint8_t *raw, const char *name)
{
    char temp[13];
    int p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (raw[i] == ' ')
            break;
        temp[p++] = raw[i];
    }
    if (raw[8] != ' ') {
        temp[p++] = '.';
        for (i = 8; i < 11; i++) {
            if (raw[i] == ' ')
                break;
            temp[p++] = raw[i];
        }
    }
    temp[p] = '\0';
    for (i = 0; temp[i]; ++i) {
        if (temp[i] >= 'a' && temp[i] <= 'z')
            temp[i] -= 32;
    }
    char canon[32];
    int ci = 0;
    for (; name[ci] && ci < 31; ++ci) {
        char c = name[ci];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        canon[ci] = c;
    }
    canon[ci] = '\0';
    const char *cmp = canon;
    if (canon[0] == '/' && canon[1])
        cmp = canon + 1;
    else if (canon[0] == '/' && !canon[1])
        return ST_INVALID;
    if (!g_root_dir_cache)
        return ST_NOT_FOUND;
    if (str_eq(cmp, temp))
        return 1;
    return str_eq(cmp, temp);
}

static int fat32_dir_list_internal(unsigned long start_cluster,
    void (*cb)(const char *, unsigned, unsigned long),
    unsigned *out_count)
{
    if (!g_root_fs || !cb)
        return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    FAT32_LOG("list start cluster=%lu cluster_size=%u bytes_per_sector=%u spc=%u\n",
        start_cluster, cluster_size, g_root_fs->bytes_per_sector, g_root_fs->sectors_per_cluster);
    unsigned long cluster = start_cluster;
    unsigned listed = 0;
    unsigned clusters = 0;
    while (cluster < 0x0FFFFFF8) {
        void *buf = kalloc(cluster_size);
        if (!buf)
            return ST_NOMEM;
        unsigned long lba = cluster_to_lba(g_root_fs, cluster);
        int st = read_sectors(g_root_fs->bdev, lba,
            g_root_fs->sectors_per_cluster, buf);
        if (st != ST_OK) {
            FAT32_LOG("list read err cluster=%lu lba=%lu st=%d\n", cluster, lba, st);
            kfree(buf);
            return ST_IO;
        }
        clusters++;
        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        FAT32_LOG("list cluster=%lu lba=%lu entries=%u\n", cluster, lba, entries);
        char lfn_buf[260];
        lfn_buf[0] = '\0';
        uint16_t lfn_tmp[260];
        int lfn_len = 0;
        for (unsigned i = 0; i < entries; i++) {
            if (ents[i].name[0] == 0x00) {
                FAT32_LOG("list end marker cluster=%lu index=%u listed=%u\n", cluster, i, listed);
                kfree(buf);
                if (out_count)
                    *out_count = listed;
                FAT32_LOG("list done start=%lu clusters=%u listed=%u\n", start_cluster, clusters, listed);
                return ST_OK;
            }
            if (ents[i].name[0] == 0xE5)
                continue;
            if (ents[i].attr == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
                int ord = (l->seq & 0x1F);
                int last = (l->seq & 0x40) ? 1 : 0;
                if (last) {
                    lfn_len = 0;
                    for (int j = 0; j < 260; j++)
                        lfn_tmp[j] = 0;
                }
                int pos = (ord - 1) * 13;
                if (pos < 260) {
                    uint16_t parts[13];
                    int j;
                    for (j = 0; j < 5; j++)
                        parts[j] = l->name1[j];
                    for (j = 0; j < 6; j++)
                        parts[5 + j] = l->name2[j];
                    for (j = 0; j < 2; j++)
                        parts[11 + j] = l->name3[j];
                    for (j = 0; j < 13 && (pos + j) < 259; j++) {
                        uint16_t c = parts[j];
                        if (c == 0x0000 || c == 0xFFFF) {
                            lfn_tmp[pos + j] = 0;
                            break;
                        }
                        lfn_tmp[pos + j] = (c & 0xFF);
                        if ((pos + j + 1) > lfn_len)
                            lfn_len = pos + j + 1;
                    }
                    if (last) {
                        int k;
                        for (k = 0; k < lfn_len && k < 259; ++k)
                            lfn_buf[k] = (char)lfn_tmp[k];
                        lfn_buf[k] = '\0';
                    }
                }
                continue;
            }
            char temp[13];
            int p = 0;
            for (int j = 0; j < 8; j++) {
                if (ents[i].name[j] == ' ')
                    break;
                temp[p++] = ents[i].name[j];
            }
            if (ents[i].name[8] != ' ') {
                temp[p++] = '.';
                for (int j = 8; j < 11; j++) {
                    if (ents[i].name[j] == ' ')
                        break;
                    temp[p++] = ents[i].name[j];
                }
            }
            temp[p] = '\0';
            cb((lfn_buf[0]) ? lfn_buf : temp, ents[i].attr, ents[i].fileSize);
            listed++;
            lfn_buf[0] = '\0';
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        cluster = next;
    }
    if (out_count)
        *out_count = listed;
    FAT32_LOG("list done start=%lu clusters=%u listed=%u last_cluster=%lu\n", start_cluster, clusters, listed, cluster);
    return ST_OK;
}

int fat32_dir_list(unsigned long cluster, void (*cb)(const char *, unsigned, unsigned long))
{
    return fat32_dir_list_internal(cluster, cb, 0);
}

int fat32_dir_find(unsigned long start_cluster, const char *name, unsigned *attr,
    unsigned long *first_cluster, unsigned long *size)
{
    if (!g_root_fs || !name)
        return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    char canon[32];
    int ci = 0;
    for (; name[ci] && ci < 31; ++ci) {
        char c = name[ci];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        canon[ci] = c;
    }
    canon[ci] = '\0';
    while (cluster < 0x0FFFFFF8) {
        void *buf = kalloc(cluster_size);
        if (!buf)
            return ST_NOMEM;
        if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        char lfn_buf[260];
        lfn_buf[0] = '\0';
        uint16_t lfn_tmp[260];
        int lfn_len = 0;
        for (unsigned i = 0; i < entries; i++) {
            if (ents[i].name[0] == 0x00) {
                kfree(buf);
                return ST_NOT_FOUND;
            }
            if (ents[i].name[0] == 0xE5)
                continue;
            if (ents[i].attr == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
                int ord = (l->seq & 0x1F);
                int last = (l->seq & 0x40) ? 1 : 0;
                if (last) {
                    lfn_len = 0;
                    for (int j = 0; j < 260; j++)
                        lfn_tmp[j] = 0;
                }
                int pos = (ord - 1) * 13;
                if (pos < 260) {
                    uint16_t parts[13];
                    int j;
                    for (j = 0; j < 5; j++)
                        parts[j] = l->name1[j];
                    for (j = 0; j < 6; j++)
                        parts[5 + j] = l->name2[j];
                    for (j = 0; j < 2; j++)
                        parts[11 + j] = l->name3[j];
                    for (j = 0; j < 13 && (pos + j) < 259; j++) {
                        uint16_t c = parts[j];
                        if (c == 0x0000 || c == 0xFFFF) {
                            lfn_tmp[pos + j] = 0;
                            break;
                        }
                        lfn_tmp[pos + j] = (c & 0xFF);
                        if ((pos + j + 1) > lfn_len)
                            lfn_len = pos + j + 1;
                    }
                    if (last) {
                        int k;
                        for (k = 0; k < lfn_len && k < 259; ++k)
                            lfn_buf[k] = (char)lfn_tmp[k];
                        lfn_buf[k] = '\0';
                    }
                }
                continue;
            }
            char temp[13];
            int p = 0;
            for (int j = 0; j < 8; j++) {
                if (ents[i].name[j] == ' ')
                    break;
                temp[p++] = ents[i].name[j];
            }
            if (ents[i].name[8] != ' ') {
                temp[p++] = '.';
                for (int j = 8; j < 11; j++) {
                    if (ents[i].name[j] == ' ')
                        break;
                    temp[p++] = ents[i].name[j];
                }
            }
            temp[p] = '\0';
            for (int j = 0; temp[j]; ++j) {
                if (temp[j] >= 'A' && temp[j] <= 'Z')
                    temp[j] += 32;
            }
            const char *cmpname = (lfn_buf[0]) ? lfn_buf : temp;
            char lname[260];
            int li = 0;
            for (; cmpname[li] && li < 259; ++li) {
                char c = cmpname[li];
                if (c >= 'A' && c <= 'Z')
                    c += 32;
                lname[li] = c;
            }
            lname[li] = '\0';
            if (str_eq(lname, canon)) {
                if (attr)
                    *attr = ents[i].attr;
                if (first_cluster)
                    *first_cluster = ((unsigned long)ents[i].fstClusHI << 16) | ents[i].fstClusLO;
                if (size)
                    *size = ents[i].fileSize;
                kfree(buf);
                return ST_OK;
            }
            lfn_buf[0] = '\0';
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        cluster = next;
    }
    return ST_NOT_FOUND;
}

static int fat32_path_resolve(const char *path, unsigned *attr,
    unsigned long *first_cluster, unsigned long *size)
{
    if (!g_root_fs || !path)
        return ST_INVALID;
    if (path[0] == '/')
        path++;
    if (!*path)
        return ST_INVALID;
    unsigned long current = g_root_dir_cluster;
    const char *seg = path;
    char part[64];
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < 63)
                part[pi++] = *seg;
            seg++;
        }
        part[pi] = '\0';
        unsigned a;
        unsigned long fc;
        unsigned long sz;
        if (fat32_dir_find(current, part, &a, &fc, &sz) != ST_OK)
            return ST_NOT_FOUND;
        if (*seg == '/') {
            if (!(a & FAT32_ATTR_DIRECTORY))
                return ST_INVALID;
            current = fc;
            seg++;
            if (!*seg) {
                if (attr)
                    *attr = a;
                if (first_cluster)
                    *first_cluster = fc;
                if (size)
                    *size = sz;
                return ST_OK;
            }
            continue;
        } else {
            if (attr)
                *attr = a;
            if (first_cluster)
                *first_cluster = fc;
            if (size)
                *size = sz;
            return ST_OK;
        }
    }
    return ST_NOT_FOUND;
}

// Extended resolution supporting relative paths, '.' and '..' ( '..' limited: returns root for now )
int fat32_resolve_path(unsigned long start_cluster, const char *path, unsigned *attr,
    unsigned long *first_cluster, unsigned long *size)
{
    if (!g_root_fs || !path)
        return ST_INVALID;
    unsigned long current = (start_cluster == 0) ? g_root_dir_cluster : start_cluster;
    if (path[0] == '/') {
        current = g_root_dir_cluster;
        path++;
    }
    if (!*path) {
        if (attr)
            *attr = FAT32_ATTR_DIRECTORY;
        if (first_cluster)
            *first_cluster = current;
        if (size)
            *size = 0;
        return ST_OK;
    }
    const char *seg = path;
    char part[64];
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < 63)
                part[pi++] = *seg;
            seg++;
        }
        part[pi] = '\0';
        if (pi == 0) {
            if (*seg == '/') {
                seg++;
                continue;
            } else {
                break;
            }
        }
        if (part[0] == '.' && part[1] == '\0') {
            /* stay */
        } else if (part[0] == '.' && part[1] == '.' && part[2] == '\0') {
            current = fat32_parent_cluster(current);
        } else {
            unsigned a;
            unsigned long fc;
            unsigned long sz;
            if (fat32_dir_find(current, part, &a, &fc, &sz) != ST_OK)
                return ST_NOT_FOUND;
            if (*seg == '/') {
                if (!(a & FAT32_ATTR_DIRECTORY))
                    return ST_INVALID;
                current = fc;
                seg++;
                if (!*seg) {
                    if (attr)
                        *attr = a;
                    if (first_cluster)
                        *first_cluster = fc;
                    if (size)
                        *size = sz;
                    return ST_OK;
                }
                continue;
            } else {
                if (attr)
                    *attr = a;
                if (first_cluster)
                    *first_cluster = fc;
                if (size)
                    *size = sz;
                return ST_OK;
            }
        }
        if (*seg == '/')
            seg++;
    }
    if (attr)
        *attr = FAT32_ATTR_DIRECTORY;
    if (first_cluster)
        *first_cluster = current;
    if (size)
        *size = 0;
    return ST_OK;
}

int fat32_stat(unsigned long start_cluster, const char *path, unsigned *attr,
    unsigned long *first_cluster, unsigned long *size)
{
    return fat32_resolve_path(start_cluster, path, attr, first_cluster, size);
}

unsigned long fat32_parent_cluster(unsigned long dir_cluster)
{
    if (dir_cluster == g_root_dir_cluster || dir_cluster == 0)
        return g_root_dir_cluster;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return g_root_dir_cluster;
    if (read_sectors(g_root_fs->bdev,
        cluster_to_lba(g_root_fs, dir_cluster),
        g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return g_root_dir_cluster;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    unsigned i;
    for (i = 0; i < (cluster_size / sizeof(fat32_dirent_t)); ++i) {
        if (ents[i].name[0] == 0x00)
            break;
        if (ents[i].attr == FAT32_ATTR_LONG_NAME)
            continue;
        if (ents[i].name[0] == '.') {
            if (ents[i].name[1] == '.') {
                unsigned long parent = ((unsigned long)ents[i].fstClusHI << 16) | ents[i].fstClusLO;
                if (parent == 0)
                    parent = g_root_dir_cluster;
                kfree(buf);
                return parent;
            }
        }
    }
    kfree(buf);
    return g_root_dir_cluster;
}

// Forward declare ops so open can assign pointer
static int fat32_open(const char* path, int flags, vfs_file_t** out);
static int fat32_stat_vfs(const char* path, struct kstat* st);
static long fat32_read(vfs_file_t* f, void* buf, long bytes);
static long fat32_write(vfs_file_t* f, const void* buf, long bytes);
static long fat32_seek(vfs_file_t* f, long offset, int whence);
static int fat32_truncate(vfs_file_t* f, unsigned long size);
static int fat32_close(vfs_file_t* f);
static int fat32_unlink(const char* path);
static int fat32_rename(const char* oldpath, const char* newpath);
static int fat32_mkdir(const char* path, unsigned int mode);
static int fat32_rmdir(const char* path);
static int fat32_chdir(const char* path);
static const vfs_ops_t fat32_vfs_ops = { fat32_open, fat32_stat_vfs, fat32_read, fat32_write, fat32_seek, fat32_truncate, fat32_unlink, fat32_rename, fat32_mkdir, fat32_rmdir, fat32_chdir, fat32_close };

static int fat32_resolve_parent(unsigned long start_cluster, const char *path,
    unsigned long *parent_cluster, char *name_out, unsigned name_out_len)
{
    if (!g_root_fs || !path || !parent_cluster || !name_out || name_out_len < 2)
        return ST_INVALID;
    unsigned long current = (start_cluster == 0) ? g_root_dir_cluster : start_cluster;
    if (path[0] == '/') {
        current = g_root_dir_cluster;
        path++;
    }
    if (!*path)
        return ST_INVALID;
    const char *seg = path;
    char part[64];
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < 63)
                part[pi++] = *seg;
            seg++;
        }
        part[pi] = '\0';
        if (*seg == '/') {
            unsigned a;
            unsigned long fc;
            unsigned long sz;
            if (fat32_dir_find(current, part, &a, &fc, &sz) != ST_OK)
                return ST_NOT_FOUND;
            if (!(a & FAT32_ATTR_DIRECTORY))
                return ST_INVALID;
            current = fc;
            seg++;
            if (!*seg)
                return ST_INVALID;
            continue;
        } else {
            mm_memset(name_out, 0, name_out_len);
            for (unsigned i = 0; i < name_out_len - 1 && part[i]; ++i) {
                name_out[i] = part[i];
            }
            *parent_cluster = current;
            return ST_OK;
        }
    }
    return ST_NOT_FOUND;
}

static int fat32_update_dirent(fat32_file_t *ff)
{
    if (!ff || !ff->fs)
        return ST_INVALID;
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    unsigned long lba = cluster_to_lba(ff->fs, ff->dirent_cluster);
    if (read_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    fat32_dirent_t *ent = &ents[ff->dirent_index];
    ent->fstClusHI = (uint16_t)((ff->start_cluster >> 16) & 0xFFFF);
    ent->fstClusLO = (uint16_t)(ff->start_cluster & 0xFFFF);
    ent->fileSize = (uint32_t)ff->size;
    int st = write_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster, buf);
    kfree(buf);
    return st;
}

static int fat32_set_position(fat32_file_t *ff, unsigned long target_pos)
{
    if (!ff || !ff->fs)
        return ST_INVALID;
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
    if (ff->start_cluster < 2 || ff->size == 0) {
        ff->current_cluster = ff->start_cluster;
        ff->pos = target_pos;
        return ST_OK;
    }
    unsigned long cluster = ff->start_cluster;
    unsigned long cluster_start = 0;
    while (cluster_start + cluster_size <= target_pos && target_pos < ff->size) {
        unsigned long next = fat32_next_cluster_cached(ff->fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0) {
            break;
        }
        cluster = next;
        cluster_start += cluster_size;
    }
    ff->current_cluster = cluster;
    ff->pos = target_pos;
    return ST_OK;
}

static int fat32_open(const char *path, int flags, vfs_file_t **out)
{
    if (!out)
        return ST_INVALID;
    unsigned long parent = 0;
    char name[64];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;

    char name83[11];
    if (fat32_make_83_name(name, name83) != ST_OK)
        return ST_INVALID;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    int found = (fat32_dir_find_entry(parent, name83, &attr, &fc, &size, &dir_cluster, &dir_index) == ST_OK);

    if (!found) {
        if (!(flags & O_CREAT))
            return ST_NOT_FOUND;
        // create new entry
        if (fat32_dir_find_free_entry(parent, &dir_cluster, &dir_index) != ST_OK)
            return ST_IO;
        fat32_dirent_t ent;
        mm_memset(&ent, 0, sizeof(ent));
        for (int i = 0; i < 11; ++i) ent.name[i] = name83[i];
        ent.attr = FAT32_ATTR_ARCHIVE;
        ent.fstClusHI = 0;
        ent.fstClusLO = 0;
        ent.fileSize = 0;
        if (fat32_write_dirent(dir_cluster, dir_index, &ent) != ST_OK)
            return ST_IO;
        attr = ent.attr;
        fc = 0;
        size = 0;
    } else {
        if (attr & FAT32_ATTR_DIRECTORY)
            return ST_UNSUPPORTED;
        if (flags & O_TRUNC) {
            if (fc >= 2) {
                fat32_free_chain(g_root_fs, fc);
            }
            fc = 0;
            size = 0;
        }
    }

    fat32_file_t *ff = (fat32_file_t *)kalloc(sizeof(fat32_file_t));
    if (!ff)
        return ST_NOMEM;
    mm_memset(ff, 0, sizeof(fat32_file_t));
    ff->fs = g_root_fs;
    ff->start_cluster = fc;
    ff->current_cluster = fc;
    ff->size = size;
    ff->pos = 0;
    ff->parent_cluster = parent;
    ff->dirent_cluster = dir_cluster;
    ff->dirent_index = dir_index;
    for (int i = 0; i < 11; ++i) ff->name83[i] = name83[i];
    ff->name83[11] = '\0';
    ff->vfs.ops = &fat32_vfs_ops;
    ff->vfs.fs_private = ff;
    *out = &ff->vfs;

    if ((flags & O_TRUNC) && found) {
        fat32_update_dirent(ff);
    }

    if (flags & O_APPEND) {
        fat32_set_position(ff, ff->size);
    }

    return ST_OK;
}

static int fat32_stat_vfs(const char* path, struct kstat* st)
{
    if (!st || !path)
        return ST_INVALID;
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    if (fat32_resolve_path(fat32_get_cwd(), path, &attr, &fc, &size) != ST_OK) {
        return ST_NOT_FOUND;
    }
    mm_memset(st, 0, sizeof(*st));
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_size = size;
    if (attr & FAT32_ATTR_DIRECTORY) {
        st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    } else {
        st->st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    }
    return ST_OK;
}

int fat32_unlink_path(const char* path)
{
    unsigned long parent = 0;
    char name[64];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    char name83[11];
    if (fat32_make_83_name(name, name83) != ST_OK)
        return ST_INVALID;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    if (fat32_dir_find_entry(parent, name83, &attr, &fc, &size, &dir_cluster, &dir_index) != ST_OK)
        return ST_NOT_FOUND;
    if (attr & FAT32_ATTR_DIRECTORY)
        return ST_UNSUPPORTED;

    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
    if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    ents[dir_index].name[0] = 0xE5;
    int st = write_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf);
    kfree(buf);
    if (st != ST_OK)
        return st;
    if (fc >= 2) {
        fat32_free_chain(g_root_fs, fc);
    }
    return ST_OK;
}

static int fat32_unlink(const char* path)
{
    return fat32_unlink_path(path);
}

int fat32_rename_path(const char* oldpath, const char* newpath)
{
    unsigned long old_parent = 0;
    char old_name[64];
    old_name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), oldpath, &old_parent, old_name, sizeof(old_name)) != ST_OK)
        return ST_NOT_FOUND;
    unsigned long new_parent = 0;
    char new_name[64];
    new_name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), newpath, &new_parent, new_name, sizeof(new_name)) != ST_OK)
        return ST_NOT_FOUND;
    if (old_parent != new_parent)
        return ST_UNSUPPORTED;

    char old83[11];
    char new83[11];
    if (fat32_make_83_name(old_name, old83) != ST_OK)
        return ST_INVALID;
    if (fat32_make_83_name(new_name, new83) != ST_OK)
        return ST_INVALID;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    if (fat32_dir_find_entry(old_parent, old83, &attr, &fc, &size, &dir_cluster, &dir_index) != ST_OK)
        return ST_NOT_FOUND;

    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
    if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    for (int i = 0; i < 11; ++i) {
        ents[dir_index].name[i] = (uint8_t)new83[i];
    }
    int st = write_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, buf);
    kfree(buf);
    return st;
}

static int fat32_rename(const char* oldpath, const char* newpath)
{
    return fat32_rename_path(oldpath, newpath);
}

int fat32_mkdir_path(const char* path)
{
    unsigned long parent = 0;
    char name[64];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    char name83[11];
    if (fat32_make_83_name(name, name83) != ST_OK)
        return ST_INVALID;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    if (fat32_dir_find_entry(parent, name83, &attr, &fc, &size, &dir_cluster, &dir_index) == ST_OK) {
        return ST_INVALID;
    }

    if (fat32_dir_find_free_entry(parent, &dir_cluster, &dir_index) != ST_OK)
        return ST_IO;

    unsigned long newc = 0;
    if (fat32_alloc_cluster(g_root_fs, &newc) != ST_OK)
        return ST_IO;

    fat32_dirent_t ent;
    fat32_init_dirent(&ent, name83, FAT32_ATTR_DIRECTORY, newc, 0);
    if (fat32_write_dirent(dir_cluster, dir_index, &ent) != ST_OK)
        return ST_IO;

    // Initialize directory cluster with . and ..
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kcalloc(1, cluster_size);
    if (!buf)
        return ST_NOMEM;
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    char dot[11] = {'.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    char dotdot[11] = {'.','.', ' ',' ',' ',' ',' ',' ',' ',' ',' '};
    fat32_init_dirent(&ents[0], dot, FAT32_ATTR_DIRECTORY, newc, 0);
    fat32_init_dirent(&ents[1], dotdot, FAT32_ATTR_DIRECTORY, parent, 0);
    int st = write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, newc),
        g_root_fs->sectors_per_cluster, buf);
    kfree(buf);
    if (st != ST_OK)
        return st;

    return ST_OK;
}

static int fat32_mkdir(const char* path, unsigned int mode)
{
    (void)mode;
    return fat32_mkdir_path(path);
}

int fat32_rmdir_path(const char* path)
{
    unsigned long parent = 0;
    char name[64];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_cwd(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    char name83[11];
    if (fat32_make_83_name(name, name83) != ST_OK)
        return ST_INVALID;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    if (fat32_dir_find_entry(parent, name83, &attr, &fc, &size, &dir_cluster, &dir_index) != ST_OK)
        return ST_NOT_FOUND;
    if (!(attr & FAT32_ATTR_DIRECTORY))
        return ST_NOT_FOUND;

    // Ensure directory is empty (only . and ..)
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, fc),
        g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    unsigned entries = cluster_size / sizeof(fat32_dirent_t);
    int empty = 1;
    for (unsigned i = 0; i < entries; ++i) {
        if (ents[i].name[0] == 0x00)
            break;
        if (ents[i].name[0] == 0xE5)
            continue;
        if (ents[i].attr == FAT32_ATTR_LONG_NAME)
            continue;
        if (ents[i].name[0] == '.' && (ents[i].name[1] == ' ' || ents[i].name[1] == '.'))
            continue;
        empty = 0;
        break;
    }
    kfree(buf);
    if (!empty)
        return ST_INVALID;

    // Delete directory entry in parent
    void *pbuf = kalloc(cluster_size);
    if (!pbuf)
        return ST_NOMEM;
    unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
    if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, pbuf) != ST_OK) {
        kfree(pbuf);
        return ST_IO;
    }
    fat32_dirent_t *pents = (fat32_dirent_t *)pbuf;
    pents[dir_index].name[0] = 0xE5;
    int st = write_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster, pbuf);
    kfree(pbuf);
    if (st != ST_OK)
        return st;

    if (fc >= 2)
        fat32_free_chain(g_root_fs, fc);
    return ST_OK;
}

static int fat32_rmdir(const char* path)
{
    return fat32_rmdir_path(path);
}

static int fat32_chdir(const char* path)
{
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    if (fat32_resolve_path(fat32_get_cwd(), path, &attr, &fc, &size) != ST_OK)
        return ST_NOT_FOUND;
    if (!(attr & FAT32_ATTR_DIRECTORY))
        return ST_INVALID;
    fat32_set_cwd(fc);
    return ST_OK;
}

static long fat32_read(vfs_file_t *f, void *buf, long bytes)
{
    if (!f || !buf)
        return ST_INVALID;
    fat32_file_t *ff = (fat32_file_t *)f->fs_private;
    if (!ff)
        return ST_INVALID;
    if (ff->pos >= ff->size)
        return 0;
    if (bytes < 0)
        return ST_INVALID;
    if ((unsigned long)bytes > ff->size - ff->pos)
        bytes = (long)(ff->size - ff->pos);
    unsigned long remaining = (unsigned long)bytes;
    unsigned long copied = 0;
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
    
    while (remaining) {
        unsigned cluster_offset = ff->pos % cluster_size;
        unsigned avail_in_cluster = cluster_size - cluster_offset;
        unsigned chunk = (remaining < avail_in_cluster) ? (unsigned)remaining : avail_in_cluster;
        void *tmp = kalloc(cluster_size);
        if (!tmp)
            return copied ? (long)copied : ST_NOMEM;
        if (read_sectors(ff->fs->bdev,
            cluster_to_lba(ff->fs, ff->current_cluster),
            ff->fs->sectors_per_cluster, tmp) != ST_OK) {
            kfree(tmp);
            return copied ? (long)copied : ST_IO;
        }
        for (unsigned i = 0; i < chunk; i++)
            ((uint8_t *)buf)[copied + i] = ((uint8_t *)tmp)[cluster_offset + i];
        kfree(tmp);
        ff->pos += chunk;
        copied += chunk;
        remaining -= chunk;
        if (ff->pos >= ff->size)
            break;
        if (ff->pos % cluster_size == 0) {
            unsigned long next = fat32_next_cluster_cached(ff->fs, ff->current_cluster);
            if (next >= 0x0FFFFFF8 || next == 0) {
                break;
            }
            ff->current_cluster = next;
        }
    }
    return (long)copied;
}

static long fat32_write(vfs_file_t *f, const void *buf, long bytes)
{
    if (!f || !buf)
        return ST_INVALID;
    fat32_file_t *ff = (fat32_file_t *)f->fs_private;
    if (!ff || !ff->fs)
        return ST_INVALID;
    if (bytes < 0)
        return ST_INVALID;

    if (f->flags & O_APPEND) {
        fat32_set_position(ff, ff->size);
    }

    unsigned long remaining = (unsigned long)bytes;
    unsigned long written = 0;
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;

    if (ff->start_cluster < 2) {
        unsigned long newc = 0;
        if (fat32_alloc_cluster(ff->fs, &newc) != ST_OK)
            return ST_IO;
        ff->start_cluster = newc;
        ff->current_cluster = newc;
    }

    while (remaining) {
        if (ff->current_cluster < 2) {
            ff->current_cluster = ff->start_cluster;
        }
        unsigned cluster_offset = ff->pos % cluster_size;
        unsigned avail_in_cluster = cluster_size - cluster_offset;
        unsigned chunk = (remaining < avail_in_cluster) ? (unsigned)remaining : avail_in_cluster;
        void *tmp = kalloc(cluster_size);
        if (!tmp)
            return written ? (long)written : ST_NOMEM;

        if (read_sectors(ff->fs->bdev,
            cluster_to_lba(ff->fs, ff->current_cluster),
            ff->fs->sectors_per_cluster, tmp) != ST_OK) {
            kfree(tmp);
            return written ? (long)written : ST_IO;
        }

        for (unsigned i = 0; i < chunk; i++)
            ((uint8_t *)tmp)[cluster_offset + i] = ((const uint8_t *)buf)[written + i];

        if (write_sectors(ff->fs->bdev,
            cluster_to_lba(ff->fs, ff->current_cluster),
            ff->fs->sectors_per_cluster, tmp) != ST_OK) {
            kfree(tmp);
            return written ? (long)written : ST_IO;
        }

        kfree(tmp);

        ff->pos += chunk;
        written += chunk;
        remaining -= chunk;

        if (ff->pos % cluster_size == 0 && remaining > 0) {
            unsigned long next = fat32_next_cluster_cached(ff->fs, ff->current_cluster);
            if (next >= 0x0FFFFFF8 || next == 0) {
                unsigned long newc = 0;
                if (fat32_append_cluster(ff->fs, ff->current_cluster, &newc) != ST_OK)
                    break;
                ff->current_cluster = newc;
            } else {
                ff->current_cluster = next;
            }
        }
    }

    if (ff->pos > ff->size) {
        ff->size = ff->pos;
        fat32_update_dirent(ff);
    }

    return (long)written;
}

static int fat32_get_cluster_at(fat32_fs_t *fs, unsigned long start_cluster,
    unsigned long index, unsigned long *out_cluster)
{
    if (!fs || !out_cluster || start_cluster < 2)
        return ST_INVALID;
    unsigned long cluster = start_cluster;
    unsigned long i = 0;
    while (i < index) {
        unsigned long next = fat32_next_cluster_cached(fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            return ST_NOT_FOUND;
        cluster = next;
        i++;
    }
    *out_cluster = cluster;
    return ST_OK;
}

static int fat32_truncate(vfs_file_t *f, unsigned long new_size)
{
    if (!f)
        return ST_INVALID;
    fat32_file_t *ff = (fat32_file_t *)f->fs_private;
    if (!ff || !ff->fs)
        return ST_INVALID;

    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;

    if (new_size == ff->size) {
        return ST_OK;
    }

    if (new_size == 0) {
        if (ff->start_cluster >= 2) {
            fat32_free_chain(ff->fs, ff->start_cluster);
        }
        ff->start_cluster = 0;
        ff->current_cluster = 0;
        ff->size = 0;
        if (ff->pos > 0) ff->pos = 0;
        return fat32_update_dirent(ff);
    }

    unsigned long needed_clusters = (new_size + cluster_size - 1) / cluster_size;

    if (ff->start_cluster < 2) {
        unsigned long newc = 0;
        if (fat32_alloc_cluster(ff->fs, &newc) != ST_OK)
            return ST_IO;
        ff->start_cluster = newc;
        ff->current_cluster = newc;
    }

    // Ensure chain length
    unsigned long last_cluster = ff->start_cluster;
    unsigned long index = 0;
    while (index + 1 < needed_clusters) {
        unsigned long next = fat32_next_cluster_cached(ff->fs, last_cluster);
        if (next >= 0x0FFFFFF8 || next == 0) {
            unsigned long newc = 0;
            if (fat32_append_cluster(ff->fs, last_cluster, &newc) != ST_OK)
                return ST_IO;
            last_cluster = newc;
        } else {
            last_cluster = next;
        }
        index++;
    }

    // Trim extra clusters if shrinking
    if (new_size < ff->size) {
        unsigned long cut_cluster = 0;
        if (fat32_get_cluster_at(ff->fs, ff->start_cluster, needed_clusters - 1, &cut_cluster) == ST_OK) {
            unsigned long next = fat32_next_cluster_cached(ff->fs, cut_cluster);
            if (next < 0x0FFFFFF8 && next != 0) {
                fat32_free_chain(ff->fs, next);
            }
            fat32_fat_set(ff->fs, cut_cluster, 0x0FFFFFFF);
        }
    }

    ff->size = new_size;
    if (ff->pos > ff->size) {
        ff->pos = ff->size;
        fat32_set_position(ff, ff->pos);
    }

    return fat32_update_dirent(ff);
}

// Seek to position in file
static long fat32_seek(vfs_file_t *f, long offset, int whence)
{
    if (!f)
        return -1;
    fat32_file_t *ff = (fat32_file_t *)f->fs_private;
    if (!ff)
        return -1;
    
    long new_pos;
    switch (whence) {
        case 0: // SEEK_SET
            new_pos = offset;
            break;
        case 1: // SEEK_CUR
            new_pos = (long)ff->pos + offset;
            break;
        case 2: // SEEK_END
            new_pos = (long)ff->size + offset;
            break;
        default:
            return -1;
    }
    
    if (new_pos < 0)
        return -1;
    if ((unsigned long)new_pos > ff->size)
        new_pos = (long)ff->size;
    
    // Update position and recalculate current cluster
    unsigned long target_pos = (unsigned long)new_pos;
    if (fat32_set_position(ff, target_pos) != ST_OK)
        return -1;
    return (long)ff->pos;
}

static int fat32_close(vfs_file_t *f)
{
    if (!f)
        return ST_INVALID;
    fat32_file_t *ff = (fat32_file_t *)f->fs_private;
    if (ff)
        kfree(ff);
    return ST_OK;
}

// (ops struct moved earlier)

// Accessor for root cluster
unsigned long fat32_root_cluster(void)
{
    return g_root_dir_cluster;
}
void fat32_set_cwd(unsigned long cluster)
{
    g_cwd_cluster = cluster;
}
unsigned long fat32_get_cwd(void)
{
    return g_cwd_cluster ? g_cwd_cluster : g_root_dir_cluster;
}

int fat32_vfs_register_root(fat32_fs_t *fs)
{
    if (!fs)
        return ST_INVALID;
    g_root_fs = fs;
    return vfs_register_root(&fat32_vfs_ops);
}

void fat32_list_root(void (*cb)(const char *name, unsigned attr, unsigned long size))
{
    if (!g_root_fs || !cb)
        return;
    unsigned listed = 0;
    FAT32_LOG("list_root start cluster=%lu\n", g_root_dir_cluster);
    // Reuse directory iterator over entire root cluster chain for multi-cluster roots
    fat32_dir_list_internal(g_root_dir_cluster, cb, &listed);
    FAT32_LOG("list_root done listed=%u\n", listed);
}

// Debug helper: dump first few raw directory entries (8.3) for diagnostics
void fat32_debug_dump_root(void)
{
    if (!g_root_fs || !g_root_dir_cache) {
        kprintf("FAT32 dbg: no root cache yet\n");
        return;
    }
    unsigned entries = (g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector) / sizeof(fat32_dirent_t);
    fat32_dirent_t *ents = (fat32_dirent_t *)g_root_dir_cache;
    kprintf("FAT32 dbg root first entries:\n");
    for (unsigned i = 0; i < entries && i < 8; i++) {
        if (ents[i].name[0] == 0x00) {
            kprintf("  [%u] end marker\n", i);
            break;
        }
        if (ents[i].name[0] == 0xE5) {
            kprintf("  [%u] deleted\n", i);
            continue;
        }
        if (ents[i].attr == FAT32_ATTR_LONG_NAME) {
            kprintf("  [%u] LFN seq=%02x\n", i, ((fat32_lfn_t *)&ents[i])->seq);
            continue;
        }
        char temp[13];
        int p = 0;
        for (int j = 0; j < 8; j++) {
            if (ents[i].name[j] == ' ')
                break;
            temp[p++] = ents[i].name[j];
        }
        if (ents[i].name[8] != ' ') {
            temp[p++] = '.';
            for (int j = 8; j < 11; j++) {
                if (ents[i].name[j] == ' ')
                    break;
                temp[p++] = ents[i].name[j];
            }
        }
        temp[p] = '\0';
        kprintf("  [%u] %s attr=%02x size=%lu\n", i, temp, ents[i].attr, ents[i].fileSize);
    }
}
