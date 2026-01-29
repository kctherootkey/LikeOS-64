// LikeOS-64 - FAT32 implementation with Long File Name (LFN) support
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/block.h"
#include "../../include/kernel/syscall.h"
#include "../../include/kernel/dirent.h"
#include "../../include/kernel/sched.h"

#ifndef FAT32_DEBUG_ENABLED
#define FAT32_DEBUG_ENABLED 0
#endif
#if FAT32_DEBUG_ENABLED
#define FAT32_LOG(fmt, ...) kprintf("FAT32 dbg: " fmt, ##__VA_ARGS__)
#else
#define FAT32_LOG(...) do { } while (0)
#endif

// Maximum LFN length (255 chars) + 1 for null
#define FAT32_LFN_MAX 256
// Characters per LFN entry
#define FAT32_LFN_CHARS_PER_ENTRY 13

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
#define FAT32_ATTR_LONG_NAME_MASK 0x3F

// LFN sequence number flags
#define FAT32_LFN_LAST_ENTRY 0x40
#define FAT32_LFN_SEQ_MASK   0x1F

// Checksum for 8.3 short name (used to link LFN entries to short name entry)
static uint8_t fat32_lfn_checksum(const uint8_t name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i];
    }
    return sum;
}

// Convert character to lowercase
static char fat32_tolower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    return c;
}

// Convert character to uppercase
static char fat32_toupper(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}

// Case-insensitive string comparison
static int fat32_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = fat32_tolower(*a);
        char cb = fat32_tolower(*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return fat32_tolower(*a) - fat32_tolower(*b);
}

// Check if filename needs LFN (not valid 8.3)
static int fat32_needs_lfn(const char *name)
{
    if (!name || !*name)
        return 1;
    
    int len = 0;
    int base_len = 0;
    int ext_len = 0;
    int has_dot = 0;
    int has_lowercase = 0;
    int dot_count = 0;
    
    for (const char *p = name; *p && *p != '/'; p++) {
        len++;
        if (len > 255)
            return 1;  // Too long for LFN
        
        // Check for lowercase
        if (*p >= 'a' && *p <= 'z')
            has_lowercase = 1;
        
        // Check for dot
        if (*p == '.') {
            dot_count++;
            if (dot_count > 1)
                return 1;  // Multiple dots need LFN
            has_dot = 1;
            continue;
        }
        
        if (!has_dot) {
            base_len++;
        } else {
            ext_len++;
        }
        
        // Check for characters not allowed in 8.3
        char c = *p;
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' ||
            c == '[' || c == ']')
            return 1;
    }
    
    // Check 8.3 length limits
    if (base_len > 8 || ext_len > 3)
        return 1;
    
    // If has lowercase letters, needs LFN to preserve case
    // Actually for FAT32 we use the ntres field for case, but for simplicity
    // we'll use LFN for any mixed/lowercase names
    if (has_lowercase)
        return 1;
    
    return 0;
}

// Generate a unique short name from a long name
// Returns the number of LFN entries needed (0 if fits in 8.3)
static int fat32_generate_short_name(const char *lfn, char short_name[11], int index)
{
    if (!lfn || !short_name)
        return 0;
    
    // Initialize short name with spaces
    for (int i = 0; i < 11; i++)
        short_name[i] = ' ';
    
    // Find base and extension
    const char *dot = 0;
    int len = 0;
    for (const char *p = lfn; *p && *p != '/'; p++) {
        len++;
        if (*p == '.')
            dot = p;
    }
    
    // Handle extension
    if (dot && dot != lfn && dot[1] != '\0') {
        int ei = 0;
        for (const char *p = dot + 1; *p && *p != '/' && ei < 3; p++) {
            if (*p != '.') {
                short_name[8 + ei] = fat32_toupper(*p);
                ei++;
            }
        }
    }
    
    // Handle base name
    int bi = 0;
    int end = (dot && dot > lfn) ? (int)(dot - lfn) : len;
    for (int i = 0; i < end && bi < 8; i++) {
        char c = lfn[i];
        if (c == '.' || c == ' ')
            continue;
        // Convert invalid chars to underscore
        if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            c = '_';
        short_name[bi++] = fat32_toupper(c);
    }
    
    // If index > 0, add numeric tail (~n)
    if (index > 0) {
        char tail[8];
        int tail_len = 0;
        tail[tail_len++] = '~';
        if (index >= 10000) {
            tail[tail_len++] = '0' + (index / 10000) % 10;
        }
        if (index >= 1000) {
            tail[tail_len++] = '0' + (index / 1000) % 10;
        }
        if (index >= 100) {
            tail[tail_len++] = '0' + (index / 100) % 10;
        }
        if (index >= 10) {
            tail[tail_len++] = '0' + (index / 10) % 10;
        }
        tail[tail_len++] = '0' + index % 10;
        
        // Insert tail at appropriate position
        int insert_pos = 8 - tail_len;
        if (insert_pos < 0)
            insert_pos = 0;
        if (bi < insert_pos)
            insert_pos = bi;
        for (int i = 0; i < tail_len && insert_pos + i < 8; i++) {
            short_name[insert_pos + i] = tail[i];
        }
    }
    
    // Calculate how many LFN entries needed
    int lfn_entries = (len + FAT32_LFN_CHARS_PER_ENTRY - 1) / FAT32_LFN_CHARS_PER_ENTRY;
    return lfn_entries;
}

// Create an LFN entry
static void fat32_create_lfn_entry(fat32_lfn_t *entry, const char *lfn, int seq, uint8_t checksum, int is_last)
{
    mm_memset(entry, 0, sizeof(*entry));
    
    entry->seq = seq;
    if (is_last)
        entry->seq |= FAT32_LFN_LAST_ENTRY;
    entry->attr = FAT32_ATTR_LONG_NAME;
    entry->type = 0;
    entry->checksum = checksum;
    entry->fstClusLO = 0;
    
    // Fill with 0xFFFF padding
    for (int i = 0; i < 5; i++)
        entry->name1[i] = 0xFFFF;
    for (int i = 0; i < 6; i++)
        entry->name2[i] = 0xFFFF;
    for (int i = 0; i < 2; i++)
        entry->name3[i] = 0xFFFF;
    
    // Calculate starting character position
    int start = (seq - 1) * FAT32_LFN_CHARS_PER_ENTRY;
    int lfn_len = 0;
    for (const char *p = lfn; *p && *p != '/'; p++)
        lfn_len++;
    
    // Fill in characters
    int char_idx = 0;
    for (int i = start; i < start + FAT32_LFN_CHARS_PER_ENTRY; i++) {
        uint16_t ch;
        if (i < lfn_len) {
            ch = (uint8_t)lfn[i];  // Simple ASCII to UTF-16
        } else if (i == lfn_len) {
            ch = 0x0000;  // Null terminator
        } else {
            ch = 0xFFFF;  // Padding
        }
        
        if (char_idx < 5) {
            entry->name1[char_idx] = ch;
        } else if (char_idx < 11) {
            entry->name2[char_idx - 5] = ch;
        } else {
            entry->name3[char_idx - 11] = ch;
        }
        char_idx++;
    }
}

// Extract long filename from LFN entries buffer
static void fat32_extract_lfn(const fat32_lfn_t *entries, int count, char *out, int out_size)
{
    if (!entries || !out || count <= 0 || out_size <= 0) {
        if (out && out_size > 0)
            out[0] = '\0';
        return;
    }
    
    int pos = 0;
    // LFN entries are in reverse order, so process from last to first
    for (int i = count - 1; i >= 0 && pos < out_size - 1; i--) {
        const fat32_lfn_t *e = &entries[i];
        
        // Extract 13 characters from this entry
        uint16_t chars[13];
        for (int j = 0; j < 5; j++)
            chars[j] = e->name1[j];
        for (int j = 0; j < 6; j++)
            chars[5 + j] = e->name2[j];
        for (int j = 0; j < 2; j++)
            chars[11 + j] = e->name3[j];
        
        for (int j = 0; j < 13 && pos < out_size - 1; j++) {
            if (chars[j] == 0x0000 || chars[j] == 0xFFFF)
                break;
            out[pos++] = (char)(chars[j] & 0xFF);
        }
    }
    out[pos] = '\0';
}

// Maximum sectors per single USB read (1 sector = 512 bytes)
#define MAX_SECTORS_PER_READ 8

static int read_sectors(const block_device_t *bdev, unsigned long lba, unsigned long count, void *buf)
{
    // Chunk large reads to work around QEMU xHCI DMA limitations
    unsigned long offset = 0;
    while (count > 0) {
        unsigned long chunk = (count > MAX_SECTORS_PER_READ) ? MAX_SECTORS_PER_READ : count;
        int st = ST_OK;
        int attempts = 1;
        while (attempts-- > 0) {
            st = bdev->read((block_device_t *)bdev, lba, chunk, (uint8_t *)buf + offset);
            if (st == ST_OK) {
                break;
            }
        }
        if (st != ST_OK) {
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
        int attempts = 1;
        while (attempts-- > 0) {
            st = bdev->write((block_device_t *)bdev, lba, chunk, (const uint8_t *)buf + offset);
            if (st == ST_OK) {
                break;
            }
        }
        if (st != ST_OK) {
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
static unsigned long fat32_get_task_cwd_cluster(void);
static int fat32_load_fat_window(fat32_fs_t *fs, unsigned long start_entry);

// Simple root directory cache (single cluster only for now)
static void* g_root_dir_cache = 0; // first cluster of root directory cached
static unsigned long g_root_dir_cluster = 0;
static void* g_fat_cache = 0; // cached FAT window
static unsigned long g_fat_cache_entries = 0; // number of entries in cache window
static unsigned long g_fat_cache_start = 0;   // first cluster entry in cache window
static unsigned long g_fat_total_entries = 0; // total FAT entries on disk
static unsigned long g_fat_first_sectors = 0; // first FAT size in sectors
#define FAT_CACHE_MAX_BYTES (1024 * 1024) // 1MB max FAT cache
#define FAT_CACHE_MAX_ENTRIES (FAT_CACHE_MAX_BYTES / 4) // 262144 entries
static fat32_fs_t* g_root_fs = 0;
static fat32_fs_t g_static_fs; // internal singleton instance
static unsigned long g_cwd_cluster = 0; // 0 means root

static const char* fat32_normalize_start(const char* path, unsigned long* start_cluster)
{
    if (!path || !start_cluster) return path;
    
    // Handle absolute paths starting with /
    if (path[0] == '/') {
        *start_cluster = g_root_dir_cluster;
        while (*path == '/') path++;
        return path;
    }
    
    // Handle relative paths starting with ./
    if (path[0] == '.' && path[1] == '/') {
        // Stay in current directory, skip the ./
        path += 2;
        while (*path == '/') path++;
        return path;
    }
    
    // Handle relative paths starting with ../
    if (path[0] == '.' && path[1] == '.' && path[2] == '/') {
        // Move to parent directory
        unsigned long cur = *start_cluster;
        *start_cluster = fat32_parent_cluster(cur);
        path += 3;
        while (*path == '/') path++;
        return path;
    }
    
    return path;
}

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
    if (!fs || cluster >= g_fat_total_entries)
        return ST_INVALID;
    
    /* Ensure cluster is in cache window */
    if (!g_fat_cache || cluster < g_fat_cache_start || 
        cluster >= g_fat_cache_start + g_fat_cache_entries) {
        /* Load window containing this cluster */
        unsigned long window_start = 0;
        if (cluster >= FAT_CACHE_MAX_ENTRIES / 2)
            window_start = cluster - FAT_CACHE_MAX_ENTRIES / 2;
        unsigned long entries_per_sector = fs->bytes_per_sector / 4;
        window_start = (window_start / entries_per_sector) * entries_per_sector;
        if (fat32_load_fat_window(fs, window_start) != 0)
            return ST_IO;
    }
    
    /* Update in cache */
    unsigned long cache_index = cluster - g_fat_cache_start;
    uint32_t *fat = (uint32_t *)g_fat_cache;
    fat[cache_index] = value & 0x0FFFFFFF;

    /* Calculate sector for this FAT entry and write to disk */
    unsigned long fat_byte = cluster * 4;
    unsigned long sector = fat_byte / fs->bytes_per_sector;
    unsigned long lba = fs->part_lba_offset + fs->fat_start_lba + sector;
    
    /* Get the sector data from cache */
    unsigned long cache_sector = (cluster - g_fat_cache_start) * 4 / fs->bytes_per_sector;
    uint8_t *sector_data = ((uint8_t *)g_fat_cache) + cache_sector * fs->bytes_per_sector;

    // write updated FAT sector (first FAT)
    if (write_sectors(fs->bdev, lba, 1, sector_data) != ST_OK) {
        return ST_IO;
    }

    // mirror to additional FATs if present
    for (unsigned int f = 1; f < fs->num_fats; ++f) {
        unsigned long lba2 = fs->part_lba_offset + fs->fat_start_lba + (f * fs->fat_size_sectors) + sector;
        if (write_sectors(fs->bdev, lba2, 1, sector_data) != ST_OK) {
            return ST_IO;
        }
    }

    return ST_OK;
}

static int fat32_alloc_cluster(fat32_fs_t *fs, unsigned long *out_cluster)
{
    if (!fs || !out_cluster)
        return ST_INVALID;
    if (fat32_ensure_fat_loaded(fs) != ST_OK)
        return ST_IO;
    
    /* Search all FAT entries for a free cluster */
    for (unsigned long c = 2; c < g_fat_total_entries; ++c) {
        unsigned long next = fat32_next_cluster_cached(fs, c);
        if (next == 0) {
            /* Found free cluster */
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
    
    // Initialize with spaces
    for (int i = 0; i < 11; ++i) out[i] = ' ';
    
    // Find the base name and extension
    int name_len = 0;
    const char *dot = 0;
    for (const char *p = name; *p && *p != '/'; ++p) {
        name_len++;
        if (*p == '.')
            dot = p;
    }
    
    // If name has no dot or dot is first/last char
    const char *base_end = dot ? dot : name + name_len;
    const char *ext_start = (dot && dot[1]) ? dot + 1 : 0;
    
    // Copy base (up to 8 chars)
    int bi = 0;
    for (const char *p = name; p < base_end && bi < 8; ++p) {
        char c = *p;
        if (c == '.')
            continue;
        // Skip invalid characters
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            c = '_';
        if (c >= 'a' && c <= 'z')
            c -= 32;  // uppercase
        out[bi++] = c;
    }
    
    // Copy extension (up to 3 chars)
    if (ext_start) {
        int ei = 0;
        for (const char *p = ext_start; *p && *p != '/' && ei < 3; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            out[8 + ei++] = c;
        }
    }
    
    return ST_OK;
}

// Extended name preparation for LFN - stores the original name and generates short alias
typedef struct {
    char lfn[FAT32_LFN_MAX];       // Long filename (case preserved)
    char short_name[11];           // 8.3 alias
    int lfn_entries;               // Number of LFN entries needed (0 if pure 8.3)
    uint8_t checksum;              // Checksum of short name
} fat32_name_t;

static int fat32_prepare_name(const char *name, fat32_name_t *out)
{
    if (!name || !out)
        return ST_INVALID;
    
    mm_memset(out, 0, sizeof(*out));
    
    // Skip leading slash
    if (name[0] == '/') name++;
    if (!*name)
        return ST_INVALID;
    
    // Copy the original name (preserving case)
    int i = 0;
    for (const char *p = name; *p && *p != '/' && i < FAT32_LFN_MAX - 1; p++) {
        out->lfn[i++] = *p;
    }
    out->lfn[i] = '\0';
    
    // Check if we need LFN
    if (!fat32_needs_lfn(out->lfn)) {
        // Simple 8.3 name, no LFN needed
        out->lfn_entries = 0;
        return fat32_make_83_name(name, out->short_name);
    }
    
    // Generate short name alias
    out->lfn_entries = fat32_generate_short_name(out->lfn, out->short_name, 1);
    out->checksum = fat32_lfn_checksum((uint8_t *)out->short_name);
    
    return ST_OK;
}

// Find entry by name (supports both LFN and 8.3, case-insensitive)
// Also returns the index of the first LFN entry if present
static int fat32_dir_find_entry_lfn(unsigned long start_cluster, const char *name,
    unsigned *attr, unsigned long *first_cluster, unsigned long *size,
    unsigned long *out_cluster, unsigned int *out_index,
    unsigned long *lfn_start_cluster, unsigned int *lfn_start_index)
{
    if (!g_root_fs || !name)
        return ST_INVALID;
    
    // Prepare the name we're looking for
    fat32_name_t search_name;
    if (fat32_prepare_name(name, &search_name) != ST_OK)
        return ST_INVALID;
    
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    
    // LFN accumulation state
    char lfn_buf[FAT32_LFN_MAX];
    lfn_buf[0] = '\0';
    uint16_t lfn_tmp[FAT32_LFN_MAX];
    int lfn_len = 0;
    unsigned long first_lfn_cluster = 0;
    unsigned int first_lfn_index = 0;
    int lfn_seq_expected = 0;
    uint8_t lfn_checksum = 0;
    
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
            if (ents[i].name[0] == 0xE5) {
                lfn_buf[0] = '\0';
                lfn_len = 0;
                lfn_seq_expected = 0;
                continue;
            }
            
            // Handle LFN entry
            if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
                int ord = (l->seq & FAT32_LFN_SEQ_MASK);
                int is_last = (l->seq & FAT32_LFN_LAST_ENTRY) ? 1 : 0;
                
                if (ord <= 0 || ord > 20) {
                    lfn_buf[0] = '\0';
                    lfn_len = 0;
                    lfn_seq_expected = 0;
                    continue;
                }
                
                if (is_last) {
                    // Start of new LFN sequence
                    lfn_len = 0;
                    for (int j = 0; j < FAT32_LFN_MAX; j++)
                        lfn_tmp[j] = 0;
                    lfn_seq_expected = ord;
                    lfn_checksum = l->checksum;
                    first_lfn_cluster = cluster;
                    first_lfn_index = i;
                }
                
                if (ord != lfn_seq_expected || l->checksum != lfn_checksum) {
                    lfn_buf[0] = '\0';
                    lfn_len = 0;
                    lfn_seq_expected = 0;
                    continue;
                }
                
                lfn_seq_expected--;
                
                int pos = (ord - 1) * FAT32_LFN_CHARS_PER_ENTRY;
                if (pos >= 0 && pos < FAT32_LFN_MAX - 13) {
                    uint16_t parts[13];
                    for (int j = 0; j < 5; j++)
                        parts[j] = l->name1[j];
                    for (int j = 0; j < 6; j++)
                        parts[5 + j] = l->name2[j];
                    for (int j = 0; j < 2; j++)
                        parts[11 + j] = l->name3[j];
                    
                    for (int j = 0; j < 13 && (pos + j) < FAT32_LFN_MAX - 1; j++) {
                        uint16_t c = parts[j];
                        if (c == 0x0000 || c == 0xFFFF) {
                            lfn_tmp[pos + j] = 0;
                            break;
                        }
                        lfn_tmp[pos + j] = c;
                        if ((pos + j + 1) > lfn_len)
                            lfn_len = pos + j + 1;
                    }
                    
                    if (lfn_seq_expected == 0) {
                        // Complete LFN - convert to string
                        for (int k = 0; k < lfn_len && k < FAT32_LFN_MAX - 1; ++k)
                            lfn_buf[k] = (char)(lfn_tmp[k] & 0xFF);
                        lfn_buf[lfn_len] = '\0';
                    }
                }
                continue;
            }
            
            // This is a short name entry - check if it matches
            // First verify LFN checksum if we have an LFN
            if (lfn_buf[0] != '\0') {
                uint8_t calc_checksum = fat32_lfn_checksum(ents[i].name);
                if (calc_checksum != lfn_checksum) {
                    lfn_buf[0] = '\0';  // Invalid LFN
                }
            }
            
            // Build short name for comparison
            char short_name[13];
            int p = 0;
            for (int j = 0; j < 8; j++) {
                if (ents[i].name[j] == ' ')
                    break;
                short_name[p++] = ents[i].name[j];
            }
            if (ents[i].name[8] != ' ') {
                short_name[p++] = '.';
                for (int j = 8; j < 11; j++) {
                    if (ents[i].name[j] == ' ')
                        break;
                    short_name[p++] = ents[i].name[j];
                }
            }
            short_name[p] = '\0';
            
            // Compare with search name (case-insensitive)
            const char *cmp_name = (lfn_buf[0]) ? lfn_buf : short_name;
            int match = (fat32_strcasecmp(cmp_name, search_name.lfn) == 0);
            
            // Also try matching by short name directly
            if (!match && search_name.lfn_entries == 0) {
                match = (fat32_strcasecmp(short_name, search_name.lfn) == 0);
            }
            
            if (match) {
                if (attr) *attr = ents[i].attr;
                if (first_cluster) *first_cluster = ((unsigned long)ents[i].fstClusHI << 16) | ents[i].fstClusLO;
                if (size) *size = ents[i].fileSize;
                if (out_cluster) *out_cluster = cluster;
                if (out_index) *out_index = i;
                if (lfn_start_cluster) *lfn_start_cluster = (lfn_buf[0]) ? first_lfn_cluster : cluster;
                if (lfn_start_index) *lfn_start_index = (lfn_buf[0]) ? first_lfn_index : i;
                kfree(buf);
                return ST_OK;
            }
            
            // Reset LFN state
            lfn_buf[0] = '\0';
            lfn_len = 0;
            lfn_seq_expected = 0;
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        cluster = next;
    }
    return ST_NOT_FOUND;
}

// Legacy wrapper for 8.3 name search (still used internally)
static int fat32_dir_find_entry(unsigned long start_cluster, const char *name83,
    unsigned *attr, unsigned long *first_cluster, unsigned long *size,
    unsigned long *out_cluster, unsigned int *out_index)
{
    // Convert 8.3 to filename for the new function
    char filename[13];
    int p = 0;
    for (int j = 0; j < 8; j++) {
        if (name83[j] == ' ')
            break;
        filename[p++] = name83[j];
    }
    if (name83[8] != ' ') {
        filename[p++] = '.';
        for (int j = 8; j < 11; j++) {
            if (name83[j] == ' ')
                break;
            filename[p++] = name83[j];
        }
    }
    filename[p] = '\0';
    
    return fat32_dir_find_entry_lfn(start_cluster, filename, attr, first_cluster, size,
        out_cluster, out_index, 0, 0);
}

// Find contiguous free entries for LFN (need lfn_entries + 1 entries)
static int fat32_dir_find_free_entries(unsigned long start_cluster, int count,
    unsigned long *out_cluster, unsigned int *out_index)
{
    if (!g_root_fs || !out_cluster || !out_index || count <= 0)
        return ST_INVALID;
    
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);
    unsigned long cluster = start_cluster;
    unsigned long last_cluster = start_cluster;
    
    // Track contiguous free entries
    unsigned long start_clust = 0;
    unsigned int start_idx = 0;
    int contiguous = 0;
    
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
        
        for (unsigned i = 0; i < entries_per_cluster; i++) {
            if (ents[i].name[0] == 0x00 || ents[i].name[0] == 0xE5) {
                if (contiguous == 0) {
                    start_clust = cluster;
                    start_idx = i;
                }
                contiguous++;
                
                if (contiguous >= count) {
                    kfree(buf);
                    *out_cluster = start_clust;
                    *out_index = start_idx;
                    return ST_OK;
                }
                
                // If entry is 0x00 (end of directory), all following entries are also free
                if (ents[i].name[0] == 0x00) {
                    // Check if we have enough entries in this cluster
                    int remaining_in_cluster = entries_per_cluster - i;
                    if (contiguous + remaining_in_cluster - 1 >= count) {
                        kfree(buf);
                        *out_cluster = start_clust;
                        *out_index = start_idx;
                        return ST_OK;
                    }
                }
            } else {
                contiguous = 0;
            }
        }
        kfree(buf);
        last_cluster = cluster;
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            break;
        cluster = next;
    }
    
    // Need to allocate new cluster
    unsigned long newc = 0;
    if (fat32_append_cluster(g_root_fs, last_cluster, &newc) != ST_OK)
        return ST_IO;
    
    // If we had some contiguous entries at the end, they might span into new cluster
    if (contiguous > 0 && contiguous < count) {
        *out_cluster = start_clust;
        *out_index = start_idx;
    } else {
        *out_cluster = newc;
        *out_index = 0;
    }
    return ST_OK;
}

// Legacy single entry find
static int fat32_dir_find_free_entry(unsigned long start_cluster, unsigned long *out_cluster, unsigned int *out_index)
{
    return fat32_dir_find_free_entries(start_cluster, 1, out_cluster, out_index);
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

// Write LFN entries + short name entry
// start_cluster/start_index: first entry position (for first LFN entry)
// fat32_name: prepared name structure
// short_ent: the short directory entry to write
static int fat32_write_lfn_entries(unsigned long start_cluster, unsigned int start_index,
    const fat32_name_t *nm, const fat32_dirent_t *short_ent)
{
    if (!g_root_fs || !nm || !short_ent)
        return ST_INVALID;
    
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);
    (void)entries_per_cluster;  // Used later in cluster navigation
    
    // Read starting cluster
    void *buf = kalloc(cluster_size);
    if (!buf)
        return ST_NOMEM;
    
    unsigned long current_cluster = start_cluster;
    unsigned int current_index = start_index;
    
    if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
        g_root_fs->sectors_per_cluster, buf) != ST_OK) {
        kfree(buf);
        return ST_IO;
    }
    fat32_dirent_t *ents = (fat32_dirent_t *)buf;
    
    // Calculate checksum for the short name
    uint8_t checksum = fat32_lfn_checksum((uint8_t *)nm->short_name);
    
    // Write LFN entries in reverse order (highest sequence number first)
    for (int lfn_idx = nm->lfn_entries; lfn_idx >= 1; lfn_idx--) {
        if (current_index >= entries_per_cluster) {
            // Write current cluster and move to next
            if (write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
                g_root_fs->sectors_per_cluster, buf) != ST_OK) {
                kfree(buf);
                return ST_IO;
            }
            unsigned long next = fat32_next_cluster_cached(g_root_fs, current_cluster);
            if (next >= 0x0FFFFFF8 || next == 0) {
                kfree(buf);
                return ST_IO;
            }
            current_cluster = next;
            current_index = 0;
            if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
                g_root_fs->sectors_per_cluster, buf) != ST_OK) {
                kfree(buf);
                return ST_IO;
            }
            ents = (fat32_dirent_t *)buf;
        }
        
        fat32_lfn_t *lfn_ent = (fat32_lfn_t *)&ents[current_index];
        int is_last = (lfn_idx == nm->lfn_entries) ? 1 : 0;
        fat32_create_lfn_entry(lfn_ent, nm->lfn, lfn_idx, checksum, is_last);
        current_index++;
    }
    
    // Write short name entry
    if (current_index >= entries_per_cluster) {
        if (write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        unsigned long next = fat32_next_cluster_cached(g_root_fs, current_cluster);
        if (next >= 0x0FFFFFF8 || next == 0) {
            kfree(buf);
            return ST_IO;
        }
        current_cluster = next;
        current_index = 0;
        if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        ents = (fat32_dirent_t *)buf;
    }
    
    ents[current_index] = *short_ent;
    
    // Write final cluster
    int st = write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
        g_root_fs->sectors_per_cluster, buf);
    kfree(buf);
    return st;
}

// Delete directory entries from lfn_start to short_entry
static int fat32_delete_entries(unsigned long start_cluster, unsigned int start_index,
    unsigned long end_cluster, unsigned int end_index)
{
    if (!g_root_fs)
        return ST_INVALID;
    
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);
    
    unsigned long current_cluster = start_cluster;
    unsigned int current_index = start_index;
    
    while (1) {
        void *buf = kalloc(cluster_size);
        if (!buf)
            return ST_NOMEM;
        
        if (read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
            g_root_fs->sectors_per_cluster, buf) != ST_OK) {
            kfree(buf);
            return ST_IO;
        }
        fat32_dirent_t *ents = (fat32_dirent_t *)buf;
        
        int modified = 0;
        while (current_index < entries_per_cluster) {
            ents[current_index].name[0] = 0xE5;  // Mark as deleted
            modified = 1;
            
            if (current_cluster == end_cluster && current_index == end_index) {
                // Done
                if (modified) {
                    write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
                        g_root_fs->sectors_per_cluster, buf);
                }
                kfree(buf);
                return ST_OK;
            }
            current_index++;
        }
        
        if (modified) {
            write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, current_cluster),
                g_root_fs->sectors_per_cluster, buf);
        }
        kfree(buf);
        
        // Move to next cluster
        unsigned long next = fat32_next_cluster_cached(g_root_fs, current_cluster);
        if (next >= 0x0FFFFFF8 || next == 0)
            return ST_IO;
        current_cluster = next;
        current_index = 0;
    }
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

static int fat32_load_fat_window(fat32_fs_t *fs, unsigned long start_entry)
{
    /* Calculate how many sectors we need and can cache */
    unsigned long entries_per_sector = fs->bytes_per_sector / 4;
    unsigned long start_sector = start_entry / entries_per_sector;
    
    /* Limit cache to FAT_CACHE_MAX_ENTRIES or remaining FAT entries */
    unsigned long entries_to_load = g_fat_total_entries - start_entry;
    if (entries_to_load > FAT_CACHE_MAX_ENTRIES)
        entries_to_load = FAT_CACHE_MAX_ENTRIES;
    
    unsigned long sectors_to_load = (entries_to_load * 4 + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    unsigned long bytes_to_load = sectors_to_load * fs->bytes_per_sector;
    
    /* Allocate cache if not already done */
    if (!g_fat_cache) {
        g_fat_cache = kalloc(bytes_to_load);
        if (!g_fat_cache) {
            return -1;
        }
    }
    
    /* Read FAT window */
    unsigned long lba = fs->part_lba_offset + fs->fat_start_lba + start_sector;
    uint8_t *dest = (uint8_t*)g_fat_cache;
    unsigned long remaining = sectors_to_load;
    
    while (remaining > 0) {
        unsigned long chunk = (remaining > 8) ? 8 : remaining; /* read up to 8 sectors at once */
        int read_st = read_sectors(fs->bdev, lba, chunk, dest);
        if (read_st != ST_OK) {
            return -1;
        }
        lba += chunk;
        dest += chunk * fs->bytes_per_sector;
        remaining -= chunk;
    }
    
    g_fat_cache_start = start_sector * entries_per_sector;
    g_fat_cache_entries = sectors_to_load * entries_per_sector;
    
    return 0;
}

static unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster)
{
    /* First time: calculate total FAT entries */
    if (g_fat_total_entries == 0) {
        unsigned long fat_total = fs->data_start_lba - fs->fat_start_lba;
        if (fat_total == 0)
            return 0x0FFFFFFF;
        g_fat_first_sectors = fat_total / fs->num_fats;
        if (g_fat_first_sectors == 0)
            g_fat_first_sectors = fat_total;
        g_fat_total_entries = (g_fat_first_sectors * fs->bytes_per_sector) / 4;
    }
    
    if (cluster >= g_fat_total_entries)
        return 0x0FFFFFFF;
    
    /* Check if cluster is in current cache window */
    if (!g_fat_cache || cluster < g_fat_cache_start || 
        cluster >= g_fat_cache_start + g_fat_cache_entries) {
        /* Need to load a new window centered around cluster */
        unsigned long window_start = 0;
        if (cluster >= FAT_CACHE_MAX_ENTRIES / 2)
            window_start = cluster - FAT_CACHE_MAX_ENTRIES / 2;
        /* Align to sector boundary */
        unsigned long entries_per_sector = fs->bytes_per_sector / 4;
        window_start = (window_start / entries_per_sector) * entries_per_sector;
        
        if (fat32_load_fat_window(fs, window_start) != 0)
            return 0x0FFFFFFF;
    }
    
    unsigned long cache_index = cluster - g_fat_cache_start;
    return ((uint32_t *)g_fat_cache)[cache_index] & 0x0FFFFFFF;
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
    char canon[FAT32_LFN_MAX];
    int ci = 0;
    for (; name[ci] && ci < FAT32_LFN_MAX - 1; ++ci) {
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
    // LFN state must persist across cluster boundaries
    char lfn_buf[260];
    lfn_buf[0] = '\0';
    uint16_t lfn_tmp[260];
    int lfn_len = 0;
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
            if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
                int ord = (l->seq & 0x1F);
                int last = (l->seq & 0x40) ? 1 : 0;
                if (ord <= 0 || ord > 20) {
                    lfn_buf[0] = '\0';
                    continue;
                }
                if (last) {
                    lfn_len = 0;
                    for (int j = 0; j < 260; j++)
                        lfn_tmp[j] = 0;
                }
                int pos = (ord - 1) * 13;
                if (pos >= 0 && pos < 260) {
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
                }
                continue;
            }
            // We hit a short name entry - copy accumulated LFN to lfn_buf now
            if (lfn_len > 0) {
                int k;
                for (k = 0; k < lfn_len && k < 259; ++k)
                    lfn_buf[k] = (char)lfn_tmp[k];
                lfn_buf[k] = '\0';
            }
            char temp[13];
            int p = 0;
            for (int j = 0; j < 8; j++) {
                if (ents[i].name[j] == ' ')
                    break;
                temp[p++] = fat32_tolower(ents[i].name[j]);  // Convert to lowercase
            }
            if (ents[i].name[8] != ' ') {
                temp[p++] = '.';
                for (int j = 8; j < 11; j++) {
                    if (ents[i].name[j] == ' ')
                        break;
                    temp[p++] = fat32_tolower(ents[i].name[j]);  // Convert to lowercase
                }
            }
            temp[p] = '\0';
            cb((lfn_buf[0]) ? lfn_buf : temp, ents[i].attr, ents[i].fileSize);
            listed++;
            lfn_buf[0] = '\0';
            lfn_len = 0;  // Reset LFN length for next entry
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
    char canon[FAT32_LFN_MAX];
    int ci = 0;
    for (; name[ci] && ci < FAT32_LFN_MAX - 1; ++ci) {
        char c = name[ci];
        if (c >= 'A' && c <= 'Z')
            c += 32;
        canon[ci] = c;
    }
    canon[ci] = '\0';
    // LFN state must persist across cluster boundaries
    char lfn_buf[260];
    lfn_buf[0] = '\0';
    uint16_t lfn_tmp[260];
    int lfn_len = 0;
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
            if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
                int ord = (l->seq & 0x1F);
                int last = (l->seq & 0x40) ? 1 : 0;
                if (ord <= 0 || ord > 20) {
                    lfn_buf[0] = '\0';
                    continue;
                }
                if (last) {
                    lfn_len = 0;
                    for (int j = 0; j < 260; j++)
                        lfn_tmp[j] = 0;
                }
                int pos = (ord - 1) * 13;
                if (pos >= 0 && pos < 260) {
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
                }
                continue;
            }
            // We hit a short name entry - copy accumulated LFN to lfn_buf now
            if (lfn_len > 0) {
                int k;
                for (k = 0; k < lfn_len && k < 259; ++k)
                    lfn_buf[k] = (char)lfn_tmp[k];
                lfn_buf[k] = '\0';
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
            lfn_len = 0;  // Reset LFN length for next entry
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
    char part[FAT32_LFN_MAX];
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < FAT32_LFN_MAX - 1)
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
    char part[FAT32_LFN_MAX];
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < FAT32_LFN_MAX - 1)
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
        if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME)
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
static long fat32_readdir(vfs_file_t* f, void* buf, long bytes);
static int fat32_truncate(vfs_file_t* f, unsigned long size);
static int fat32_close(vfs_file_t* f);
static int fat32_unlink(const char* path);
static int fat32_rename(const char* oldpath, const char* newpath);
static int fat32_mkdir(const char* path, unsigned int mode);
static int fat32_rmdir(const char* path);
static int fat32_chdir(const char* path);
static const vfs_ops_t fat32_vfs_ops = { fat32_open, fat32_stat_vfs, fat32_read, fat32_write, fat32_seek, fat32_readdir, fat32_truncate, fat32_unlink, fat32_rename, fat32_mkdir, fat32_rmdir, fat32_chdir, fat32_close };

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
    char part[FAT32_LFN_MAX];  // Increased for LFN support
    while (*seg) {
        int pi = 0;
        while (*seg && *seg != '/') {
            if (pi < FAT32_LFN_MAX - 1)
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
    if (!path || !path[0])
        return ST_INVALID;

    unsigned long start_cluster = fat32_get_task_cwd_cluster();
    const char* rpath = fat32_normalize_start(path, &start_cluster);

    // Directory open handling
    if (kstrcmp(path, "/") == 0 || kstrcmp(path, ".") == 0 || kstrcmp(path, "..") == 0 || (rpath && rpath[0] == '\0')) {
        unsigned long dir_cluster = fat32_root_cluster();
        if (kstrcmp(path, ".") == 0) {
            dir_cluster = fat32_get_task_cwd_cluster();
        } else if (kstrcmp(path, "..") == 0) {
            unsigned long cur = fat32_get_task_cwd_cluster();
            dir_cluster = fat32_parent_cluster(cur);
        }
        fat32_file_t *ff = (fat32_file_t *)kalloc(sizeof(fat32_file_t));
        if (!ff)
            return ST_NOMEM;
        mm_memset(ff, 0, sizeof(fat32_file_t));
        ff->fs = g_root_fs;
        ff->start_cluster = dir_cluster;
        ff->current_cluster = dir_cluster;
        ff->size = 0;
        ff->pos = 0;
        ff->is_dir = 1;
        ff->dir_iter_cluster = dir_cluster;
        ff->dir_iter_index = 0;
        ff->vfs.ops = &fat32_vfs_ops;
        ff->vfs.fs_private = ff;
        *out = &ff->vfs;
        return ST_OK;
    }

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    int res = fat32_resolve_path(start_cluster, rpath, &attr, &fc, &size);
    if (res == ST_OK && (attr & FAT32_ATTR_DIRECTORY)) {
        fat32_file_t *ff = (fat32_file_t *)kalloc(sizeof(fat32_file_t));
        if (!ff)
            return ST_NOMEM;
        mm_memset(ff, 0, sizeof(fat32_file_t));
        ff->fs = g_root_fs;
        ff->start_cluster = fc;
        ff->current_cluster = fc;
        ff->size = 0;
        ff->pos = 0;
        ff->is_dir = 1;
        ff->dir_iter_cluster = fc;
        ff->dir_iter_index = 0;
        ff->vfs.ops = &fat32_vfs_ops;
        ff->vfs.fs_private = ff;
        *out = &ff->vfs;
        return ST_OK;
    }
    unsigned long parent = 0;
    char name[256];  // Increased for LFN support
    name[0] = '\0';
    if (fat32_resolve_parent(start_cluster, rpath, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;

    // Prepare the name (handles both LFN and 8.3)
    fat32_name_t fname;
    if (fat32_prepare_name(name, &fname) != ST_OK)
        return ST_INVALID;

    attr = 0;
    fc = 0;
    size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    unsigned long lfn_start_cluster = 0;
    unsigned int lfn_start_index = 0;
    int found = (fat32_dir_find_entry_lfn(parent, name, &attr, &fc, &size, 
        &dir_cluster, &dir_index, &lfn_start_cluster, &lfn_start_index) == ST_OK);

    if (!found) {
        if (!(flags & O_CREAT))
            return ST_NOT_FOUND;
        
        // Create new entry with LFN if needed
        int entries_needed = fname.lfn_entries + 1;  // LFN entries + short entry
        if (fat32_dir_find_free_entries(parent, entries_needed, &dir_cluster, &dir_index) != ST_OK)
            return ST_IO;
        
        fat32_dirent_t ent;
        mm_memset(&ent, 0, sizeof(ent));
        for (int i = 0; i < 11; ++i) ent.name[i] = fname.short_name[i];
        ent.attr = FAT32_ATTR_ARCHIVE;
        ent.fstClusHI = 0;
        ent.fstClusLO = 0;
        ent.fileSize = 0;
        
        if (fname.lfn_entries > 0) {
            // Write LFN entries + short entry
            if (fat32_write_lfn_entries(dir_cluster, dir_index, &fname, &ent) != ST_OK)
                return ST_IO;
            // Update dir_index to point to the short entry
            unsigned entries_per_cluster = (g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector) / sizeof(fat32_dirent_t);
            unsigned total_offset = dir_index + fname.lfn_entries;
            while (total_offset >= entries_per_cluster) {
                unsigned long next = fat32_next_cluster_cached(g_root_fs, dir_cluster);
                if (next >= 0x0FFFFFF8 || next == 0)
                    break;
                dir_cluster = next;
                total_offset -= entries_per_cluster;
            }
            dir_index = total_offset;
        } else {
            if (fat32_write_dirent(dir_cluster, dir_index, &ent) != ST_OK)
                return ST_IO;
        }
        attr = ent.attr;
        fc = 0;
        size = 0;
        lfn_start_cluster = dir_cluster;
        lfn_start_index = dir_index;
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
    for (int i = 0; i < 11; ++i) ff->name83[i] = fname.short_name[i];
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
    unsigned long start_cluster = fat32_get_task_cwd_cluster();
    const char* rpath = fat32_normalize_start(path, &start_cluster);
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    if (fat32_resolve_path(start_cluster, rpath, &attr, &fc, &size) != ST_OK) {
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
    char name[256];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    unsigned long lfn_start_cluster = 0;
    unsigned int lfn_start_index = 0;
    
    if (fat32_dir_find_entry_lfn(parent, name, &attr, &fc, &size, 
        &dir_cluster, &dir_index, &lfn_start_cluster, &lfn_start_index) != ST_OK)
        return ST_NOT_FOUND;
    if (attr & FAT32_ATTR_DIRECTORY)
        return ST_UNSUPPORTED;

    // Delete all entries from LFN start to short entry
    int st = fat32_delete_entries(lfn_start_cluster, lfn_start_index, dir_cluster, dir_index);
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
    char old_name[256];
    old_name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), oldpath, &old_parent, old_name, sizeof(old_name)) != ST_OK)
        return ST_NOT_FOUND;
    unsigned long new_parent = 0;
    char new_name[256];
    new_name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), newpath, &new_parent, new_name, sizeof(new_name)) != ST_OK)
        return ST_NOT_FOUND;
    if (old_parent != new_parent)
        return ST_UNSUPPORTED;

    // Find old entry (with LFN info)
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long old_dir_cluster = 0;
    unsigned int old_dir_index = 0;
    unsigned long old_lfn_cluster = 0;
    unsigned int old_lfn_index = 0;
    
    if (fat32_dir_find_entry_lfn(old_parent, old_name, &attr, &fc, &size, 
        &old_dir_cluster, &old_dir_index, &old_lfn_cluster, &old_lfn_index) != ST_OK)
        return ST_NOT_FOUND;

    // Prepare new name
    fat32_name_t new_fname;
    if (fat32_prepare_name(new_name, &new_fname) != ST_OK)
        return ST_INVALID;
    
    // Delete old entries
    int st = fat32_delete_entries(old_lfn_cluster, old_lfn_index, old_dir_cluster, old_dir_index);
    if (st != ST_OK)
        return st;
    
    // Find space for new entry
    unsigned long new_dir_cluster = 0;
    unsigned int new_dir_index = 0;
    int entries_needed = new_fname.lfn_entries + 1;
    if (fat32_dir_find_free_entries(new_parent, entries_needed, &new_dir_cluster, &new_dir_index) != ST_OK)
        return ST_IO;
    
    // Create new entry
    fat32_dirent_t ent;
    mm_memset(&ent, 0, sizeof(ent));
    for (int i = 0; i < 11; ++i) ent.name[i] = new_fname.short_name[i];
    ent.attr = (uint8_t)attr;
    ent.fstClusHI = (uint16_t)((fc >> 16) & 0xFFFF);
    ent.fstClusLO = (uint16_t)(fc & 0xFFFF);
    ent.fileSize = (uint32_t)size;
    
    if (new_fname.lfn_entries > 0) {
        st = fat32_write_lfn_entries(new_dir_cluster, new_dir_index, &new_fname, &ent);
    } else {
        st = fat32_write_dirent(new_dir_cluster, new_dir_index, &ent);
    }
    
    return st;
}

static int fat32_rename(const char* oldpath, const char* newpath)
{
    return fat32_rename_path(oldpath, newpath);
}

int fat32_mkdir_path(const char* path)
{
    unsigned long parent = 0;
    char name[256];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;
    
    // Prepare the name (handles both LFN and 8.3)
    fat32_name_t fname;
    if (fat32_prepare_name(name, &fname) != ST_OK)
        return ST_INVALID;

    // Check if directory already exists
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    unsigned long lfn_start_cluster = 0;
    unsigned int lfn_start_index = 0;
    
    if (fat32_dir_find_entry_lfn(parent, name, &attr, &fc, &size, 
        &dir_cluster, &dir_index, &lfn_start_cluster, &lfn_start_index) == ST_OK) {
        return ST_INVALID;  // Already exists
    }

    // Find space for new entries
    int entries_needed = fname.lfn_entries + 1;
    if (fat32_dir_find_free_entries(parent, entries_needed, &dir_cluster, &dir_index) != ST_OK)
        return ST_IO;

    unsigned long newc = 0;
    if (fat32_alloc_cluster(g_root_fs, &newc) != ST_OK)
        return ST_IO;

    fat32_dirent_t ent;
    fat32_init_dirent(&ent, fname.short_name, FAT32_ATTR_DIRECTORY, newc, 0);
    
    int st;
    if (fname.lfn_entries > 0) {
        st = fat32_write_lfn_entries(dir_cluster, dir_index, &fname, &ent);
    } else {
        st = fat32_write_dirent(dir_cluster, dir_index, &ent);
    }
    if (st != ST_OK)
        return st;

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
    st = write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, newc),
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
    char name[256];
    name[0] = '\0';
    if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent, name, sizeof(name)) != ST_OK)
        return ST_NOT_FOUND;

    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    unsigned long dir_cluster = 0;
    unsigned int dir_index = 0;
    unsigned long lfn_start_cluster = 0;
    unsigned int lfn_start_index = 0;
    
    if (fat32_dir_find_entry_lfn(parent, name, &attr, &fc, &size, 
        &dir_cluster, &dir_index, &lfn_start_cluster, &lfn_start_index) != ST_OK)
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
        if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME)
            continue;
        if (ents[i].name[0] == '.' && (ents[i].name[1] == ' ' || ents[i].name[1] == '.'))
            continue;
        empty = 0;
        break;
    }
    kfree(buf);
    if (!empty)
        return ST_INVALID;

    // Delete all entries (LFN + short entry)
    int st = fat32_delete_entries(lfn_start_cluster, lfn_start_index, dir_cluster, dir_index);
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
    unsigned long start_cluster = fat32_get_task_cwd_cluster();
    const char* rpath = fat32_normalize_start(path, &start_cluster);
    if (fat32_resolve_path(start_cluster, rpath, &attr, &fc, &size) != ST_OK)
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
        if (!tmp) {
            kprintf("fat32_read: kalloc(%u) failed, remaining=%lu copied=%lu\n", cluster_size, remaining, copied);
            return copied ? (long)copied : ST_NOMEM;
        }
        int st = read_sectors(ff->fs->bdev,
            cluster_to_lba(ff->fs, ff->current_cluster),
            ff->fs->sectors_per_cluster, tmp);
        if (st != ST_OK) {
            kfree(tmp);
            return copied ? (long)copied : ST_IO;
        }
        // SMAP-aware copy to user buffer
        smap_disable();
        for (unsigned i = 0; i < chunk; i++)
            ((uint8_t *)buf)[copied + i] = ((uint8_t *)tmp)[cluster_offset + i];
        smap_enable();
        kfree(tmp);
        ff->pos += chunk;
        copied += chunk;
        remaining -= chunk;
        if (ff->pos >= ff->size)
            break;
        if (ff->pos % cluster_size == 0) {
            unsigned long next = fat32_next_cluster_cached(ff->fs, ff->current_cluster);
            if (next >= 0x0FFFFFF8) {
                // Valid end-of-chain marker
                break;
            }
            if (next == 0 && remaining > 0) {
                // FAT entry is 0 (free) but file still has data - filesystem corruption
                // Try to continue with next sequential cluster as fallback
                next = ff->current_cluster + 1;
            }
            if (next == 0) {
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

        // SMAP-aware copy from user buffer
        smap_disable();
        for (unsigned i = 0; i < chunk; i++)
            ((uint8_t *)tmp)[cluster_offset + i] = ((const uint8_t *)buf)[written + i];
        smap_enable();

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

static unsigned fat32_write_dirent64(char* out, unsigned out_size, unsigned* out_off,
                                     const char* name, uint64_t ino, uint8_t type) {
    if (!out || !out_off || !name) {
        return 0;
    }
    unsigned name_len = 0;
    while (name[name_len] && name_len < 255) {
        name_len++;
    }
    unsigned reclen = (unsigned)sizeof(struct linux_dirent64) + name_len + 1;
    reclen = (reclen + 7u) & ~7u;
    if (*out_off + reclen > out_size) {
        return 0;
    }
    // SMAP-aware write to user buffer
    smap_disable();
    struct linux_dirent64* d = (struct linux_dirent64*)(out + *out_off);
    d->d_ino = ino;
    d->d_off = 0;
    d->d_reclen = (uint16_t)reclen;
    d->d_type = type;
    char* dn = (char*)d->d_name;
    for (unsigned i = 0; i < name_len; ++i) {
        dn[i] = name[i];
    }
    dn[name_len] = '\0';
    smap_enable();
    *out_off += reclen;
    return 1;
}

static long fat32_readdir(vfs_file_t* f, void* buf, long bytes)
{
    if (!f || !buf || bytes <= 0) {
        return ST_INVALID;
    }
    fat32_file_t* ff = (fat32_file_t*)f->fs_private;
    if (!ff || !ff->fs || !ff->is_dir) {
        return -ENOTDIR;
    }
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
    unsigned out_off = 0;
    unsigned long cluster = ff->dir_iter_cluster;
    unsigned int idx = ff->dir_iter_index;

    // LFN accumulation state
    char lfn_buf[FAT32_LFN_MAX];
    lfn_buf[0] = '\0';
    uint16_t lfn_tmp[FAT32_LFN_MAX];
    int lfn_len = 0;
    int lfn_seq_expected = 0;
    uint8_t lfn_checksum = 0;

    while (out_off + sizeof(struct linux_dirent64) + 2 < (unsigned)bytes) {
        if (cluster == 0 || cluster >= 0x0FFFFFF8) {
            ff->dir_iter_cluster = cluster;
            ff->dir_iter_index = idx;
            break;
        }
        void* tmp = kalloc(cluster_size);
        if (!tmp) {
            return out_off ? (long)out_off : ST_NOMEM;
        }
        unsigned long lba = cluster_to_lba(ff->fs, cluster);
        if (read_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster, tmp) != ST_OK) {
            kfree(tmp);
            return out_off ? (long)out_off : ST_IO;
        }
        fat32_dirent_t* ents = (fat32_dirent_t*)tmp;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        for (; idx < entries; ++idx) {
            if (ents[idx].name[0] == 0x00) {
                ff->dir_iter_cluster = 0x0FFFFFFF;
                ff->dir_iter_index = 0;
                kfree(tmp);
                return (long)out_off;
            }
            if (ents[idx].name[0] == 0xE5) {
                lfn_buf[0] = '\0';
                lfn_len = 0;
                lfn_seq_expected = 0;
                continue;
            }
            
            // Handle LFN entry
            if ((ents[idx].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t *l = (fat32_lfn_t *)&ents[idx];
                int ord = (l->seq & FAT32_LFN_SEQ_MASK);
                int is_last = (l->seq & FAT32_LFN_LAST_ENTRY) ? 1 : 0;
                
                if (ord <= 0 || ord > 20) {
                    lfn_buf[0] = '\0';
                    lfn_len = 0;
                    lfn_seq_expected = 0;
                    continue;
                }
                
                if (is_last) {
                    lfn_len = 0;
                    for (int j = 0; j < FAT32_LFN_MAX; j++)
                        lfn_tmp[j] = 0;
                    lfn_seq_expected = ord;
                    lfn_checksum = l->checksum;
                }
                
                if (ord != lfn_seq_expected || l->checksum != lfn_checksum) {
                    lfn_buf[0] = '\0';
                    lfn_len = 0;
                    lfn_seq_expected = 0;
                    continue;
                }
                
                lfn_seq_expected--;
                
                int pos = (ord - 1) * FAT32_LFN_CHARS_PER_ENTRY;
                if (pos >= 0 && pos < FAT32_LFN_MAX - 13) {
                    uint16_t parts[13];
                    for (int j = 0; j < 5; j++)
                        parts[j] = l->name1[j];
                    for (int j = 0; j < 6; j++)
                        parts[5 + j] = l->name2[j];
                    for (int j = 0; j < 2; j++)
                        parts[11 + j] = l->name3[j];
                    
                    for (int j = 0; j < 13 && (pos + j) < FAT32_LFN_MAX - 1; j++) {
                        uint16_t c = parts[j];
                        if (c == 0x0000 || c == 0xFFFF) {
                            lfn_tmp[pos + j] = 0;
                            break;
                        }
                        lfn_tmp[pos + j] = c;
                        if ((pos + j + 1) > lfn_len)
                            lfn_len = pos + j + 1;
                    }
                    
                    if (lfn_seq_expected == 0) {
                        for (int k = 0; k < lfn_len && k < FAT32_LFN_MAX - 1; ++k)
                            lfn_buf[k] = (char)(lfn_tmp[k] & 0xFF);
                        lfn_buf[lfn_len] = '\0';
                    }
                }
                continue;
            }
            
            // This is a short name entry
            // Verify LFN checksum
            if (lfn_buf[0] != '\0') {
                uint8_t calc_checksum = fat32_lfn_checksum(ents[idx].name);
                if (calc_checksum != lfn_checksum) {
                    lfn_buf[0] = '\0';
                }
            }
            
            // Build display name
            char display_name[FAT32_LFN_MAX];
            if (lfn_buf[0] != '\0') {
                // Use LFN (case preserved)
                int k;
                for (k = 0; lfn_buf[k] && k < FAT32_LFN_MAX - 1; k++)
                    display_name[k] = lfn_buf[k];
                display_name[k] = '\0';
            } else {
                // Use short name, convert to lowercase for display
                int p = 0;
                for (int j = 0; j < 8; j++) {
                    if (ents[idx].name[j] == ' ')
                        break;
                    display_name[p++] = fat32_tolower(ents[idx].name[j]);
                }
                if (ents[idx].name[8] != ' ') {
                    display_name[p++] = '.';
                    for (int j = 8; j < 11; j++) {
                        if (ents[idx].name[j] == ' ')
                            break;
                        display_name[p++] = fat32_tolower(ents[idx].name[j]);
                    }
                }
                display_name[p] = '\0';
            }
            
            uint64_t ino = ((uint64_t)ents[idx].fstClusHI << 16) | ents[idx].fstClusLO;
            uint8_t dtype = (ents[idx].attr & FAT32_ATTR_DIRECTORY) ? 4 : 8; /* DT_DIR=4, DT_REG=8 */
            if (!fat32_write_dirent64((char*)buf, (unsigned)bytes, &out_off, display_name, ino, dtype)) {
                ff->dir_iter_cluster = cluster;
                ff->dir_iter_index = idx;
                kfree(tmp);
                return (long)out_off;
            }
            
            // Reset LFN state
            lfn_buf[0] = '\0';
            lfn_len = 0;
            lfn_seq_expected = 0;
        }
        kfree(tmp);
        unsigned long next = fat32_next_cluster_cached(ff->fs, cluster);
        if (next >= 0x0FFFFFF8 || next == 0) {
            ff->dir_iter_cluster = 0x0FFFFFFF;
            ff->dir_iter_index = 0;
            break;
        }
        cluster = next;
        idx = 0;
        ff->dir_iter_cluster = cluster;
        ff->dir_iter_index = idx;
    }
    return (long)out_off;
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

// Get current task's cwd cluster (not the global one)
static unsigned long fat32_get_task_cwd_cluster(void)
{
    // Get current task's cwd path
    task_t* cur = sched_current();
    if (!cur || cur->cwd[0] == '\0' || kstrcmp(cur->cwd, "/") == 0) {
        return g_root_dir_cluster;
    }
    
    // Resolve the task's cwd path to get cluster
    const char* path = cur->cwd;
    if (path[0] == '/') path++; // Skip leading /
    
    unsigned attr = 0;
    unsigned long fc = 0;
    unsigned long size = 0;
    
    if (path[0] == '\0') {
        return g_root_dir_cluster; // Root directory
    }
    
    int res = fat32_resolve_path(g_root_dir_cluster, path, &attr, &fc, &size);
    if (res == ST_OK && (attr & FAT32_ATTR_DIRECTORY)) {
        return fc;
    }
    
    // Fallback to root if resolution fails
    return g_root_dir_cluster;
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
        if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) == FAT32_ATTR_LONG_NAME) {
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
