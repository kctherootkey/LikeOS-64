// LikeOS-64 - FAT32 implementation with Long File Name (LFN) support
#include <kernel/fs/fat32.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/memory.h>
#include <kernel/dev/block/block.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/dirent.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/timer.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/uapi/bug.h>

// Spinlock for FAT32 filesystem access
static spinlock_t fat32_lock = SPINLOCK_INIT("fat32");

// Sleeping mutex for FAT32 file I/O operations (SMP safety).
// The FAT cache (g_fat_cache, g_fat_cache_start, g_fat_cache_entries) is a
// single global window.  Concurrent fat32_read/fat32_write from different CPUs
// race on the cache: one CPU can evict the window while another is mid-traversal,
// reading garbage cluster numbers.  This causes infinite loops or XHCI hangs.
// A spinlock is unsuitable because USB I/O takes milliseconds; this sleeping
// mutex blocks the calling task via the scheduler with IRQs enabled.
// The mutex is REENTRANT: the same task can lock it multiple times (e.g.
// fat32_read holds the outer lock while pagecache_get takes it again internally).
/* Non-static so the debug dump (kernel/ke/sched.c) can print lock state. */
volatile int fat32_io_locked = 0;
volatile int fat32_io_depth = 0; // recursion depth
volatile uint64_t fat32_io_owner = (uint64_t)-1; // owning task id (-1 = none)
static spinlock_t fat32_io_wait_lock = SPINLOCK_INIT("fat32_io_wait");

void fat32_io_lock(void)
{
	might_sleep();
	task_t *cur = sched_current();
	uint64_t my_id = cur ? cur->id : 0;

	while (1) {
		uint64_t flags;
		spin_lock_irqsave(&fat32_io_wait_lock, &flags);
		if (!fat32_io_locked) {
			fat32_io_locked = 1;
			fat32_io_owner = my_id;
			fat32_io_depth = 1;
			spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
			return;
		}
		// Reentrant: same task already holds it
		if (fat32_io_owner == my_id) {
			fat32_io_depth++;
			spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
			return;
		}
		if (cur) {
			cur->state = TASK_BLOCKED;
			cur->wait_channel = (void *)&fat32_io_locked;
		}
		spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
		sched_schedule();
	}
}

void fat32_io_unlock(void)
{
	uint64_t flags;
	spin_lock_irqsave(&fat32_io_wait_lock, &flags);
	WARN_ON(!fat32_io_locked); /* unlock without prior lock */
	WARN_ON(fat32_io_depth <=
		0); /* depth underflow: double-unlock or mismatched lock/unlock */
	if (fat32_io_depth > 1) {
		fat32_io_depth--;
		spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
		return;
	}
	fat32_io_locked = 0;
	fat32_io_owner = (uint64_t)-1;
	fat32_io_depth = 0;
	spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
	sched_wake_channel((void *)&fat32_io_locked);
}

/* Force-release the FAT32 I/O mutex if it is currently owned by the given
 * task id.  Used by the task-exit path (vfs_release_locks_for_task →
 * dead_thread_reap) to recover from a task that was killed mid-FS-write
 * (e.g. Ctrl+C on curl while inside fat32_write_impl).  Without this
 * recovery the lock would stay held forever: the dying task can never
 * call fat32_io_unlock, and every subsequent FS operation (including
 * execve reading /bin/<cmd>) would block on it.  Confirmed via the
 * scheduler debug dump: `FAT32 io_lock: held=1 depth=1 owner=<dead-tid>`.
 * Returns 1 if the lock was released by this call, 0 otherwise. */
int fat32_io_release_if_owner(uint64_t task_id)
{
	uint64_t flags;
	int released = 0;
	spin_lock_irqsave(&fat32_io_wait_lock, &flags);
	if (fat32_io_locked && fat32_io_owner == task_id) {
		fat32_io_locked = 0;
		fat32_io_owner = (uint64_t)-1;
		fat32_io_depth = 0;
		released = 1;
	}
	spin_unlock_irqrestore(&fat32_io_wait_lock, flags);
	if (released)
		sched_wake_channel((void *)&fat32_io_locked);
	return released;
}

/* ========================================================================
 * VFS superblock dispatch — generic cache <-> FAT32 plumbing.
 *
 * The dcache / icache / pagecache reach FAT32 only through these ops.
 * Forward-declare the helpers FAT32 needs to implement them; the actual
 * bodies live further down in this file. */
#include <kernel/fs/vfs_sb.h>
#include <kernel/fs/icache.h>

static unsigned long cluster_to_lba(fat32_fs_t *fs, unsigned long cluster);
unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster);
static int fat32_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *ino);

/* Sentinel that FAT32 chain-walk returns at end-of-file.  Anything >= this
 * value (and 0) is treated as "no more blocks" by the cache code. */
#define FAT32_EOC_MARKER 0x0FFFFFF8UL

static unsigned long fat32_sb_block_size(vfs_superblock_t *sb)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	return (unsigned long)fs->sectors_per_cluster * fs->bytes_per_sector;
}
static const block_device_t *fat32_sb_bdev(vfs_superblock_t *sb)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	return fs->bdev;
}
static unsigned long fat32_sb_sector_size(vfs_superblock_t *sb)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	return fs->bytes_per_sector;
}
static unsigned long fat32_sb_block_to_lba(vfs_superblock_t *sb,
					   unsigned long block_id)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	if (block_id < 2 || block_id >= FAT32_EOC_MARKER)
		return 0;
	return cluster_to_lba(fs, block_id);
}
static unsigned long fat32_sb_end_of_chain(vfs_superblock_t *sb)
{
	(void)sb;
	return FAT32_EOC_MARKER;
}
static unsigned long fat32_sb_next_block(vfs_superblock_t *sb,
					 unsigned long cur_block)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	return fat32_next_cluster_cached(fs, cur_block);
}
static void fat32_sb_lock_io(vfs_superblock_t *sb)
{
	(void)sb;
	fat32_io_lock();
}
static void fat32_sb_unlock_io(vfs_superblock_t *sb)
{
	(void)sb;
	fat32_io_unlock();
}

static unsigned long fat32_sb_reserved_meta_block(vfs_superblock_t *sb)
{
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	return (unsigned long)fs->root_cluster;
}

static const vfs_sb_ops_t fat32_sb_ops = {
	.block_size = fat32_sb_block_size,
	.bdev = fat32_sb_bdev,
	.sector_size = fat32_sb_sector_size,
	.block_to_lba = fat32_sb_block_to_lba,
	.end_of_chain_marker = fat32_sb_end_of_chain,
	.next_block = fat32_sb_next_block,
	.write_inode = fat32_sb_write_inode,
	.lock_io = fat32_sb_lock_io,
	.unlock_io = fat32_sb_unlock_io,
	.reserved_meta_block = fat32_sb_reserved_meta_block,
};

/* Wire up sb on a freshly-mounted fat32_fs_t and register it as the
 * filesystem-independent root.  Caches see only g_root_sb from here on. */
static void fat32_sb_attach(fat32_fs_t *fs)
{
	fs->sb.ops = &fat32_sb_ops;
	fs->sb.fs_private = fs;
	g_root_sb = &fs->sb;
}

/* fat32_sb_write_inode: persist ic_inode_t's size + mtime into the on-disk
 * dirent at (dirent_cluster, dirent_index).  Called by icache_flush via the
 * generic vfs_sb dispatch — icache itself has no FAT32 knowledge. */
static int fat32_sb_write_inode(vfs_superblock_t *sb, ic_inode_t *inode)
{
	if (!inode || inode->dirent_cluster < 2)
		return 0;
	fat32_fs_t *fs = (fat32_fs_t *)sb->fs_private;
	if (!fs)
		return -1;

	unsigned cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
	void *buf = kalloc(cluster_size);
	if (!buf)
		return -1;

	fat32_io_lock();

	unsigned long lba =
		fs->part_lba_offset + fs->data_start_lba +
		(inode->dirent_cluster - 2) * fs->sectors_per_cluster;
	if (fs->bdev->read((block_device_t *)fs->bdev, lba,
			   fs->sectors_per_cluster, buf) != 0) {
		fat32_io_unlock();
		kfree(buf);
		return -1;
	}

	typedef struct {
		uint8_t name[11];
		uint8_t attr;
		uint8_t ntRes;
		uint8_t crtTimeTenth;
		uint16_t crtTime;
		uint16_t crtDate;
		uint16_t lstAccDate;
		uint16_t fstClusHI;
		uint16_t wrtTime;
		uint16_t wrtDate;
		uint16_t fstClusLO;
		uint32_t fileSize;
	} __attribute__((packed)) fat32_dirent_raw_t;

	fat32_dirent_raw_t *ents = (fat32_dirent_raw_t *)buf;
	unsigned max_entries = cluster_size / sizeof(fat32_dirent_raw_t);

	if (inode->dirent_index < max_entries) {
		fat32_dirent_raw_t *de = &ents[inode->dirent_index];
		de->fileSize = (uint32_t)inode->size;
		de->wrtTime = inode->wrt_time;
		de->wrtDate = inode->wrt_date;
		if (fs->bdev->write) {
			fs->bdev->write((block_device_t *)fs->bdev, lba,
					fs->sectors_per_cluster, buf);
		}
	}

	fat32_io_unlock();
	kfree(buf);
	return 0;
}
/* ====================================================================== */

#ifndef FAT32_DEBUG_ENABLED
#define FAT32_DEBUG_ENABLED 0
#endif
#if FAT32_DEBUG_ENABLED
#define FAT32_LOG(fmt, ...) kprintf("FAT32 dbg: " fmt, ##__VA_ARGS__)
#else
#define FAT32_LOG(...) \
	do {           \
	} while (0)
#endif

// Maximum LFN length (255 chars) + 1 for null
#define FAT32_LFN_MAX 256
// Characters per LFN entry
#define FAT32_LFN_CHARS_PER_ENTRY 13

// BPB offsets (FAT32)
typedef struct __attribute__((packed)) {
	uint8_t jmp[3];
	char oem[8];
	uint16_t bytes_per_sector; /* 11 */
	uint8_t sectors_per_cluster; /* 13 */
	uint16_t reserved_sector_count; /* 14 */
	uint8_t num_fats; /* 16 */
	uint16_t root_entry_count; /* 17 (FAT12/16) */
	uint16_t total_sectors16; /* 19 */
	uint8_t media; /* 21 */
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
	uint8_t reserved[12];
	uint8_t drive_number;
	uint8_t reserved1;
	uint8_t boot_signature;
	uint32_t volume_id;
	char volume_label[11];
	char fs_type[8];
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
#define FAT32_LFN_SEQ_MASK 0x1F

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
			return 1; // Too long for LFN

		// Check for lowercase
		if (*p >= 'a' && *p <= 'z')
			has_lowercase = 1;

		// Check for dot
		if (*p == '.') {
			dot_count++;
			if (dot_count > 1)
				return 1; // Multiple dots need LFN
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
static int fat32_generate_short_name(const char *lfn, char short_name[11],
				     int index)
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
		if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' ||
		    c == ']')
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
	int lfn_entries = (len + FAT32_LFN_CHARS_PER_ENTRY - 1) /
			  FAT32_LFN_CHARS_PER_ENTRY;
	return lfn_entries;
}

// Create an LFN entry
static void fat32_create_lfn_entry(fat32_lfn_t *entry, const char *lfn, int seq,
				   uint8_t checksum, int is_last)
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
			ch = (uint8_t)lfn[i]; // Simple ASCII to UTF-16
		} else if (i == lfn_len) {
			ch = 0x0000; // Null terminator
		} else {
			ch = 0xFFFF; // Padding
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
static void fat32_extract_lfn(const fat32_lfn_t *entries, int count, char *out,
			      int out_size)
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
// 128 sectors = 64KB — optimal for USB mass storage sequential I/O.
#define MAX_SECTORS_PER_READ 128

static int read_sectors(const block_device_t *bdev, unsigned long lba,
			unsigned long count, void *buf)
{
	BUG_ON(bdev == NULL);
	BUG_ON(buf == NULL);
	might_sleep();
	// Chunk large reads to work around QEMU xHCI DMA limitations
	unsigned long offset = 0;
	while (count > 0) {
		unsigned long chunk = (count > MAX_SECTORS_PER_READ) ?
					      MAX_SECTORS_PER_READ :
					      count;
		int st = ST_OK;
		int attempts = 1;
		while (attempts-- > 0) {
			st = bdev->read((block_device_t *)bdev, lba, chunk,
					(uint8_t *)buf + offset);
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

static int write_sectors(const block_device_t *bdev, unsigned long lba,
			 unsigned long count, const void *buf)
{
	if (!bdev || !bdev->write) {
		return ST_UNSUPPORTED;
	}
	unsigned long offset = 0;
	while (count > 0) {
		unsigned long chunk = (count > MAX_SECTORS_PER_READ) ?
					      MAX_SECTORS_PER_READ :
					      count;
		int st = ST_OK;
		int attempts = 1;
		while (attempts-- > 0) {
			st = bdev->write((block_device_t *)bdev, lba, chunk,
					 (const uint8_t *)buf + offset);
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
	WARN_ON(cluster < 2);
	WARN_ON(cluster >= 0x0FFFFFF8);
	return fs->part_lba_offset + fs->data_start_lba +
	       (cluster - 2) * fs->sectors_per_cluster;
}

// Forward declaration (defined later)
unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster);
static unsigned long fat32_get_task_cwd_cluster(void);
static int fat32_load_fat_window(fat32_fs_t *fs, unsigned long start_entry);

// Simple root directory cache (single cluster only for now)
static void *g_root_dir_cache = 0; // first cluster of root directory cached
static unsigned long g_root_dir_cluster = 0;
static void *g_fat_cache = 0; // cached FAT window
static unsigned long g_fat_cache_entries =
	0; // number of entries in cache window
static unsigned long g_fat_cache_start =
	0; // first cluster entry in cache window
static unsigned long g_fat_total_entries = 0; // total FAT entries on disk
static unsigned long g_fat_first_sectors = 0; // first FAT size in sectors
#define FAT_CACHE_MAX_BYTES (1024 * 1024) // 1MB max FAT cache
#define FAT_CACHE_MAX_ENTRIES (FAT_CACHE_MAX_BYTES / 4) // 262144 entries
fat32_fs_t *g_root_fs = 0;
/* Filesystem-independent root superblock pointer.  Set by fat32_mount() once
 * the FAT32 sb-ops table is wired up; consumed by the dcache / icache /
 * pagecache for all back-into-FS dispatch.  Lives in this TU so the FAT32
 * driver owns its publication; ext4_mount() will overwrite it the same way. */
vfs_superblock_t *g_root_sb = 0;
static fat32_fs_t g_static_fs; // internal singleton instance
static unsigned long g_cwd_cluster = 0; // 0 means root

/* Cached free cluster count — avoids scanning the entire FAT every statfs call.
 * Initialised from the FSInfo sector at mount time; kept in sync by alloc/free.
 * A value of (unsigned long)-1 means "not yet known, do a full scan once". */
static unsigned long g_free_cluster_count = (unsigned long)-1;
static unsigned long g_fsinfo_sector = 0; // absolute LBA of FSInfo sector

/* Deferred FAT-sector writes: each fat32_fat_set used to write the
 * single FAT sector containing the updated entry SYNCHRONOUSLY for every
 * call.  A streaming 100 MB write allocates ~3200 clusters → ~6400 such
 * one-sector writes (×2 with FAT mirrors), dominating the USB pipe and
 * capping write throughput at ~1 MB/s.  Consecutive cluster allocations
 * touch the SAME 512-byte FAT sector (128 entries per sector), so just
 * remember the last dirty sector and flush it only when we move to a
 * different one (or at file close / sync). */
static unsigned long g_fat_dirty_sector = (unsigned long)-1;
static fat32_fs_t *g_fat_dirty_fs = NULL;
static int fat32_fat_flush_pending(void);

static const char *fat32_normalize_start(const char *path,
					 unsigned long *start_cluster)
{
	if (!path || !start_cluster)
		return path;

	// Handle absolute paths starting with /
	if (path[0] == '/') {
		*start_cluster = g_root_dir_cluster;
		while (*path == '/')
			path++;
		return path;
	}

	// Handle relative paths starting with ./
	if (path[0] == '.' && path[1] == '/') {
		// Stay in current directory, skip the ./
		path += 2;
		while (*path == '/')
			path++;
		return path;
	}

	// Handle relative paths starting with ../
	if (path[0] == '.' && path[1] == '.' && path[2] == '/') {
		// Move to parent directory
		unsigned long cur = *start_cluster;
		*start_cluster = fat32_parent_cluster(cur);
		path += 3;
		while (*path == '/')
			path++;
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
	WARN_ON(cluster <
		2); /* clusters 0 and 1 are FAT reserved entries - never write via fat_set */
	WARN_ON((value & 0x0FFFFFFF) ==
		cluster); /* FAT self-loop: FAT[cluster] = cluster creates an infinite chain */
	WARN_ON((value & 0x0FFFFFFF) ==
		1); /* FAT entry value 1 is reserved and invalid - FAT is corrupted */
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
		window_start = (window_start / entries_per_sector) *
			       entries_per_sector;
		if (fat32_load_fat_window(fs, window_start) != 0)
			return ST_IO;
	}

	/* Update in cache. */
	unsigned long cache_index = cluster - g_fat_cache_start;
	WARN_ON(cache_index >= g_fat_cache_entries);
	uint32_t *fat = (uint32_t *)g_fat_cache;
	fat[cache_index] = value & 0x0FFFFFFF;

	/* Determine which FAT sector this entry lives in.  Defer the disk
     * write — most consecutive cluster allocations land in the same
     * 512-byte FAT sector (128 entries each), so flushing only on
     * sector change collapses thousands of per-allocation writes into
     * a handful per file. */
	unsigned long fat_byte = cluster * 4;
	unsigned long sector = fat_byte / fs->bytes_per_sector;

	if (g_fat_dirty_sector != (unsigned long)-1 &&
	    (g_fat_dirty_sector != sector || g_fat_dirty_fs != fs)) {
		/* We're about to dirty a different sector — flush the previous
         * one so we don't lose its updates. */
		int rc = fat32_fat_flush_pending();
		if (rc != ST_OK)
			return rc;
	}
	g_fat_dirty_sector = sector;
	g_fat_dirty_fs = fs;
	return ST_OK;
}

/* Flush the single pending dirty FAT sector to disk (and mirror FATs).
 * Called whenever a different FAT sector gets dirtied, at file close,
 * or at filesystem sync points.  Caller MUST hold fat32_io_lock. */
static int fat32_fat_flush_pending(void)
{
	if (g_fat_dirty_sector == (unsigned long)-1 || !g_fat_dirty_fs ||
	    !g_fat_cache)
		return ST_OK;

	fat32_fs_t *fs = g_fat_dirty_fs;
	unsigned long sector = g_fat_dirty_sector;

	/* Compute the byte offset in g_fat_cache for this sector.  The cache
     * window starts at g_fat_cache_start (in FAT entries) — convert to
     * the equivalent sector and subtract. */
	unsigned long entries_per_sector = fs->bytes_per_sector / 4;
	unsigned long window_first_sector =
		(fs->fat_start_lba + g_fat_cache_start / entries_per_sector) -
		fs->fat_start_lba;
	if (sector < window_first_sector) {
		/* Sector not in the loaded window — stale dirty marker (window
         * was changed without us being asked to flush).  Clear and bail. */
		g_fat_dirty_sector = (unsigned long)-1;
		g_fat_dirty_fs = NULL;
		return ST_OK;
	}
	unsigned long cache_sector_off = sector - window_first_sector;
	if (cache_sector_off * entries_per_sector >= g_fat_cache_entries) {
		g_fat_dirty_sector = (unsigned long)-1;
		g_fat_dirty_fs = NULL;
		return ST_OK;
	}
	uint8_t *sector_data = ((uint8_t *)g_fat_cache) +
			       cache_sector_off * fs->bytes_per_sector;

	unsigned long lba = fs->part_lba_offset + fs->fat_start_lba + sector;
	if (write_sectors(fs->bdev, lba, 1, sector_data) != ST_OK) {
		g_fat_dirty_sector = (unsigned long)-1;
		g_fat_dirty_fs = NULL;
		return ST_IO;
	}
	for (unsigned int f = 1; f < fs->num_fats; ++f) {
		unsigned long lba2 = fs->part_lba_offset + fs->fat_start_lba +
				     (f * fs->fat_size_sectors) + sector;
		if (write_sectors(fs->bdev, lba2, 1, sector_data) != ST_OK) {
			/* Mirror diverged; primary is authoritative.  Continue. */
			break;
		}
	}
	g_fat_dirty_sector = (unsigned long)-1;
	g_fat_dirty_fs = NULL;
	return ST_OK;
}

static int fat32_alloc_cluster(fat32_fs_t *fs, unsigned long *out_cluster)
{
	might_sleep();
	if (!fs || !out_cluster)
		return ST_INVALID;
	if (fat32_ensure_fat_loaded(fs) != ST_OK)
		return ST_IO;

	/* Search all FAT entries for a free cluster */
	for (unsigned long c = 2; c < g_fat_total_entries; ++c) {
		if (c == g_root_dir_cluster) {
			/* Defensive guard: never allocate the root directory cluster.
             * If it appears free (FAT[c]==0) something has already gone wrong. */
			unsigned long root_val =
				fat32_next_cluster_cached(fs, c);
			WARN_ON(root_val ==
				0); /* root cluster appears free in FAT - filesystem is corrupted */
			continue;
		}
		unsigned long next = fat32_next_cluster_cached(fs, c);
		if (next == 0) {
			if (fat32_fat_set(fs, c, 0x0FFFFFFF) != ST_OK)
				return ST_IO;
			/* Do NOT zero-fill the freshly-allocated cluster.  The
             * previous code wrote an entire cluster of zeros to disk on
             * every alloc — for a 100 MB streaming download this was
             * 100 MB of extra USB write traffic in addition to the
             * actual data (which would overwrite those zeros milliseconds
             * later), capping write throughput at ~500 KB/s while reads
             * (no alloc, no zero-fill) ran at ~20 MB/s.
             *
             * Correctness of skipping the zero-fill:
             *   - fat32_write_impl already zero-fills any partial-sector
             *     gap that lies past current ff->size before writing the
             *     sector (see head_in_file/tail_in_file logic).
             *   - fat32_read returns at most ff->size bytes, so unwritten
             *     bytes past EOF in an alloc-but-not-yet-fully-written
             *     cluster are never returned to userspace.
             *   - The "lseek past EOF, then write" sparse pattern leaves
             *     genuinely unwritten cluster regions; readers see
             *     whatever was previously on those sectors.  This matches
             *     ext2/3/4 with discard disabled and Linux's vfat driver
             *     defaults; if strict zero-on-allocate is required for
             *     security, add a mount option that re-enables the
             *     write_sectors call below. */
			*out_cluster = c;
			WARN_ON(*out_cluster < 2);
			if (g_free_cluster_count != (unsigned long)-1 &&
			    g_free_cluster_count > 0)
				g_free_cluster_count--;
			return ST_OK;
		}
	}
	return ST_NOMEM;
}

static int fat32_free_chain(fat32_fs_t *fs, unsigned long start_cluster)
{
	WARN_ON(start_cluster ==
		1); /* cluster 1 is reserved and must never appear in any file's chain */
	if (!fs || start_cluster < 2)
		return ST_OK;
	/* Defensive guard: never free the root directory cluster. */
	if (start_cluster == g_root_dir_cluster) {
		kprintf("FAT32: BUG: fat32_free_chain called with root cluster %lu!\n",
			start_cluster);
		return ST_INVALID;
	}
	if (fat32_ensure_fat_loaded(fs) != ST_OK)
		return ST_IO;
	unsigned long c = start_cluster;
	unsigned long loop_count = 0;
	while (c >= 2 && c < 0x0FFFFFF8) {
		WARN_ON_ONCE(
			++loop_count >
			g_fat_total_entries); /* FAT chain longer than total clusters: circular chain or FAT corruption */
		/* Defensive guard: abort if the chain somehow reaches the root cluster. */
		if (c == g_root_dir_cluster) {
			kprintf("FAT32: BUG: free_chain reached root cluster %lu (chain from %lu)!\n",
				c, start_cluster);
			break;
		}
		unsigned long next = fat32_next_cluster_cached(fs, c);
		WARN_ON(next == c); /* self-loop: corrupted FAT chain */
		if (fat32_fat_set(fs, c, 0) != ST_OK)
			return ST_IO;
		if (g_free_cluster_count != (unsigned long)-1)
			g_free_cluster_count++;
		if (next >= 0x0FFFFFF8 || next == 0)
			break;
		c = next;
	}
	return ST_OK;
}

static int fat32_append_cluster(fat32_fs_t *fs, unsigned long last_cluster,
				unsigned long *out_new)
{
	WARN_ON(last_cluster ==
		1); /* last_cluster == 1 is the reserved FAT entry, must never appear in a file chain */
	unsigned long newc = 0;
	int st = fat32_alloc_cluster(fs, &newc);
	if (st != ST_OK)
		return st;
	if (last_cluster >= 2) {
		if (fat32_fat_set(fs, last_cluster, (uint32_t)newc) != ST_OK)
			return ST_IO;
	}
	*out_new = newc;
	WARN_ON(newc < 2);
	return ST_OK;
}

static int fat32_make_83_name(const char *name, char out[11])
{
	if (!name || !out)
		return ST_INVALID;
	// skip path separators
	if (name[0] == '/')
		name++;
	if (!*name)
		return ST_INVALID;

	// Initialize with spaces
	for (int i = 0; i < 11; ++i)
		out[i] = ' ';

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
		if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' ||
		    c == '[' || c == ']')
			c = '_';
		if (c >= 'a' && c <= 'z')
			c -= 32; // uppercase
		out[bi++] = c;
	}

	// Copy extension (up to 3 chars)
	if (ext_start) {
		int ei = 0;
		for (const char *p = ext_start; *p && *p != '/' && ei < 3;
		     ++p) {
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
	char lfn[FAT32_LFN_MAX]; // Long filename (case preserved)
	char short_name[11]; // 8.3 alias
	int lfn_entries; // Number of LFN entries needed (0 if pure 8.3)
	uint8_t checksum; // Checksum of short name
} fat32_name_t;

static int fat32_prepare_name(const char *name, fat32_name_t *out)
{
	if (!name || !out)
		return ST_INVALID;

	mm_memset(out, 0, sizeof(*out));

	// Skip leading slash
	if (name[0] == '/')
		name++;
	if (!*name)
		return ST_INVALID;

	// Copy the original name (preserving case)
	int i = 0;
	for (const char *p = name; *p && *p != '/' && i < FAT32_LFN_MAX - 1;
	     p++) {
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
	out->lfn_entries =
		fat32_generate_short_name(out->lfn, out->short_name, 1);
	out->checksum = fat32_lfn_checksum((uint8_t *)out->short_name);

	return ST_OK;
}

// Find entry by name (supports both LFN and 8.3, case-insensitive)
// Also returns the index of the first LFN entry if present
static int fat32_dir_find_entry_lfn(
	unsigned long start_cluster, const char *name, unsigned *attr,
	unsigned long *first_cluster, unsigned long *size,
	unsigned long *out_cluster, unsigned int *out_index,
	unsigned long *lfn_start_cluster, unsigned int *lfn_start_index)
{
	BUG_ON(name == NULL);
	might_sleep();
	if (!g_root_fs || !name)
		return ST_INVALID;

	// Prepare the name we're looking for
	fat32_name_t search_name;
	if (fat32_prepare_name(name, &search_name) != ST_OK)
		return ST_INVALID;

	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
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
		if (read_sectors(
			    g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
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
			if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
			    FAT32_ATTR_LONG_NAME) {
				fat32_lfn_t *l = (fat32_lfn_t *)&ents[i];
				int ord = (l->seq & FAT32_LFN_SEQ_MASK);
				int is_last =
					(l->seq & FAT32_LFN_LAST_ENTRY) ? 1 : 0;

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

				if (ord != lfn_seq_expected ||
				    l->checksum != lfn_checksum) {
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

					for (int j = 0;
					     j < 13 &&
					     (pos + j) < FAT32_LFN_MAX - 1;
					     j++) {
						uint16_t c = parts[j];
						if (c == 0x0000 ||
						    c == 0xFFFF) {
							lfn_tmp[pos + j] = 0;
							break;
						}
						lfn_tmp[pos + j] = c;
						if ((pos + j + 1) > lfn_len)
							lfn_len = pos + j + 1;
					}

					if (lfn_seq_expected == 0) {
						// Complete LFN - convert to string
						for (int k = 0;
						     k < lfn_len &&
						     k < FAT32_LFN_MAX - 1;
						     ++k)
							lfn_buf[k] =
								(char)(lfn_tmp[k] &
								       0xFF);
						lfn_buf[lfn_len] = '\0';
					}
				}
				continue;
			}

			// This is a short name entry - check if it matches
			// First verify LFN checksum if we have an LFN
			if (lfn_buf[0] != '\0') {
				uint8_t calc_checksum =
					fat32_lfn_checksum(ents[i].name);
				if (calc_checksum != lfn_checksum) {
					lfn_buf[0] = '\0'; // Invalid LFN
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
			const char *cmp_name =
				(lfn_buf[0]) ? lfn_buf : short_name;
			int match = (fat32_strcasecmp(cmp_name,
						      search_name.lfn) == 0);

			// Also try matching by short name directly
			if (!match && search_name.lfn_entries == 0) {
				match = (fat32_strcasecmp(short_name,
							  search_name.lfn) ==
					 0);
			}

			if (match) {
				if (attr)
					*attr = ents[i].attr;
				if (first_cluster)
					*first_cluster = ((unsigned long)ents[i]
								  .fstClusHI
							  << 16) |
							 ents[i].fstClusLO;
				if (size)
					*size = ents[i].fileSize;
				if (out_cluster)
					*out_cluster = cluster;
				if (out_index)
					*out_index = i;
				if (lfn_start_cluster)
					*lfn_start_cluster =
						(lfn_buf[0]) ?
							first_lfn_cluster :
							cluster;
				if (lfn_start_index)
					*lfn_start_index =
						(lfn_buf[0]) ? first_lfn_index :
							       i;
				kfree(buf);
				return ST_OK;
			}

			// Reset LFN state
			lfn_buf[0] = '\0';
			lfn_len = 0;
			lfn_seq_expected = 0;
		}
		kfree(buf);
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, cluster);
		if (next >= 0x0FFFFFF8 || next == 0)
			break;
		cluster = next;
	}
	return ST_NOT_FOUND;
}

// Legacy wrapper for 8.3 name search (still used internally)
static int fat32_dir_find_entry(unsigned long start_cluster, const char *name83,
				unsigned *attr, unsigned long *first_cluster,
				unsigned long *size, unsigned long *out_cluster,
				unsigned int *out_index)
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

	return fat32_dir_find_entry_lfn(start_cluster, filename, attr,
					first_cluster, size, out_cluster,
					out_index, 0, 0);
}

// Find contiguous free entries for LFN (need lfn_entries + 1 entries)
static int fat32_dir_find_free_entries(unsigned long start_cluster, int count,
				       unsigned long *out_cluster,
				       unsigned int *out_index)
{
	if (!g_root_fs || !out_cluster || !out_index || count <= 0)
		return ST_INVALID;

	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
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
		if (read_sectors(
			    g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
			    g_root_fs->sectors_per_cluster, buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		fat32_dirent_t *ents = (fat32_dirent_t *)buf;

		for (unsigned i = 0; i < entries_per_cluster; i++) {
			if (ents[i].name[0] == 0x00 ||
			    ents[i].name[0] == 0xE5) {
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
					int remaining_in_cluster =
						entries_per_cluster - i;
					if (contiguous + remaining_in_cluster -
						    1 >=
					    count) {
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
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, cluster);
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
static int fat32_dir_find_free_entry(unsigned long start_cluster,
				     unsigned long *out_cluster,
				     unsigned int *out_index)
{
	return fat32_dir_find_free_entries(start_cluster, 1, out_cluster,
					   out_index);
}

static int fat32_write_dirent(unsigned long dir_cluster, unsigned int index,
			      const fat32_dirent_t *ent)
{
	if (!g_root_fs || !ent)
		return ST_INVALID;
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	void *buf = kalloc(cluster_size);
	if (!buf)
		return ST_NOMEM;
	unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
	if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster,
			 buf) != ST_OK) {
		kfree(buf);
		return ST_IO;
	}
	fat32_dirent_t *ents = (fat32_dirent_t *)buf;
	ents[index] = *ent;
	int st = write_sectors(g_root_fs->bdev, lba,
			       g_root_fs->sectors_per_cluster, buf);
	kfree(buf);
	return st;
}

// Write LFN entries + short name entry
// start_cluster/start_index: first entry position (for first LFN entry)
// fat32_name: prepared name structure
// short_ent: the short directory entry to write
static int fat32_write_lfn_entries(unsigned long start_cluster,
				   unsigned int start_index,
				   const fat32_name_t *nm,
				   const fat32_dirent_t *short_ent)
{
	if (!g_root_fs || !nm || !short_ent)
		return ST_INVALID;

	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);
	(void)entries_per_cluster; // Used later in cluster navigation

	// Read starting cluster
	void *buf = kalloc(cluster_size);
	if (!buf)
		return ST_NOMEM;

	unsigned long current_cluster = start_cluster;
	unsigned int current_index = start_index;

	if (read_sectors(g_root_fs->bdev,
			 cluster_to_lba(g_root_fs, current_cluster),
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
			if (write_sectors(
				    g_root_fs->bdev,
				    cluster_to_lba(g_root_fs, current_cluster),
				    g_root_fs->sectors_per_cluster,
				    buf) != ST_OK) {
				kfree(buf);
				return ST_IO;
			}
			unsigned long next = fat32_next_cluster_cached(
				g_root_fs, current_cluster);
			if (next >= 0x0FFFFFF8 || next == 0) {
				kfree(buf);
				return ST_IO;
			}
			current_cluster = next;
			current_index = 0;
			if (read_sectors(
				    g_root_fs->bdev,
				    cluster_to_lba(g_root_fs, current_cluster),
				    g_root_fs->sectors_per_cluster,
				    buf) != ST_OK) {
				kfree(buf);
				return ST_IO;
			}
			ents = (fat32_dirent_t *)buf;
		}

		fat32_lfn_t *lfn_ent = (fat32_lfn_t *)&ents[current_index];
		int is_last = (lfn_idx == nm->lfn_entries) ? 1 : 0;
		fat32_create_lfn_entry(lfn_ent, nm->lfn, lfn_idx, checksum,
				       is_last);
		current_index++;
	}

	// Write short name entry
	if (current_index >= entries_per_cluster) {
		if (write_sectors(g_root_fs->bdev,
				  cluster_to_lba(g_root_fs, current_cluster),
				  g_root_fs->sectors_per_cluster,
				  buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, current_cluster);
		if (next >= 0x0FFFFFF8 || next == 0) {
			kfree(buf);
			return ST_IO;
		}
		current_cluster = next;
		current_index = 0;
		if (read_sectors(g_root_fs->bdev,
				 cluster_to_lba(g_root_fs, current_cluster),
				 g_root_fs->sectors_per_cluster,
				 buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		ents = (fat32_dirent_t *)buf;
	}

	ents[current_index] = *short_ent;

	// Write final cluster
	int st = write_sectors(g_root_fs->bdev,
			       cluster_to_lba(g_root_fs, current_cluster),
			       g_root_fs->sectors_per_cluster, buf);
	kfree(buf);
	return st;
}

// Delete directory entries from lfn_start to short_entry
static int fat32_delete_entries(unsigned long start_cluster,
				unsigned int start_index,
				unsigned long end_cluster,
				unsigned int end_index)
{
	if (!g_root_fs)
		return ST_INVALID;

	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);

	unsigned long current_cluster = start_cluster;
	unsigned int current_index = start_index;

	while (1) {
		void *buf = kalloc(cluster_size);
		if (!buf)
			return ST_NOMEM;

		if (read_sectors(g_root_fs->bdev,
				 cluster_to_lba(g_root_fs, current_cluster),
				 g_root_fs->sectors_per_cluster,
				 buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		fat32_dirent_t *ents = (fat32_dirent_t *)buf;

		int modified = 0;
		while (current_index < entries_per_cluster) {
			ents[current_index].name[0] = 0xE5; // Mark as deleted
			modified = 1;

			if (current_cluster == end_cluster &&
			    current_index == end_index) {
				// Done
				if (modified) {
					write_sectors(
						g_root_fs->bdev,
						cluster_to_lba(g_root_fs,
							       current_cluster),
						g_root_fs->sectors_per_cluster,
						buf);
				}
				kfree(buf);
				return ST_OK;
			}
			current_index++;
		}

		if (modified) {
			write_sectors(
				g_root_fs->bdev,
				cluster_to_lba(g_root_fs, current_cluster),
				g_root_fs->sectors_per_cluster, buf);
		}
		kfree(buf);

		// Move to next cluster
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, current_cluster);
		if (next >= 0x0FFFFFF8 || next == 0)
			return ST_IO;
		current_cluster = next;
		current_index = 0;
	}
}

static void fat32_init_dirent(fat32_dirent_t *ent, const char name83[11],
			      uint8_t attr, unsigned long first_cluster,
			      unsigned long size)
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
	might_sleep();
	BUG_ON(fs == NULL);
	/* If we have a pending dirty FAT sector in the current window, flush
     * it BEFORE we overwrite the cache window with a different range —
     * otherwise the update is silently lost. */
	if (g_fat_dirty_sector != (unsigned long)-1)
		fat32_fat_flush_pending();
	/* Calculate how many sectors we need and can cache */
	unsigned long entries_per_sector = fs->bytes_per_sector / 4;
	unsigned long start_sector = start_entry / entries_per_sector;

	/* Limit cache to FAT_CACHE_MAX_ENTRIES or remaining FAT entries */
	unsigned long entries_to_load = g_fat_total_entries - start_entry;
	if (entries_to_load > FAT_CACHE_MAX_ENTRIES)
		entries_to_load = FAT_CACHE_MAX_ENTRIES;

	unsigned long sectors_to_load =
		(entries_to_load * 4 + fs->bytes_per_sector - 1) /
		fs->bytes_per_sector;
	unsigned long bytes_to_load = sectors_to_load * fs->bytes_per_sector;

	/* Allocate cache if not already done */
	if (!g_fat_cache) {
		g_fat_cache = kalloc(bytes_to_load);
		if (!g_fat_cache) {
			return -1;
		}
	}

	/* Read FAT window */
	unsigned long lba =
		fs->part_lba_offset + fs->fat_start_lba + start_sector;
	uint8_t *dest = (uint8_t *)g_fat_cache;
	unsigned long remaining = sectors_to_load;

	while (remaining > 0) {
		unsigned long chunk =
			(remaining > 128) ?
				128 :
				remaining; /* read up to 64KB at once */
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

unsigned long fat32_next_cluster_cached(fat32_fs_t *fs, unsigned long cluster)
{
	WARN_ON(cluster <
		2); /* cluster 0 and 1 are reserved in FAT32 and never valid data clusters */
	/* First time: calculate total FAT entries */
	if (g_fat_total_entries == 0) {
		unsigned long fat_total =
			fs->data_start_lba - fs->fat_start_lba;
		if (fat_total == 0)
			return 0x0FFFFFFF;
		g_fat_first_sectors = fat_total / fs->num_fats;
		if (g_fat_first_sectors == 0)
			g_fat_first_sectors = fat_total;
		g_fat_total_entries =
			(g_fat_first_sectors * fs->bytes_per_sector) / 4;
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
		window_start = (window_start / entries_per_sector) *
			       entries_per_sector;

		if (fat32_load_fat_window(fs, window_start) != 0)
			return 0x0FFFFFFF;
	}

	unsigned long cache_index = cluster - g_fat_cache_start;
	WARN_ON(cache_index >= g_fat_cache_entries);
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
	if (!bpb)
		return 0;
	// Common jump opcodes: EB ?? 90  or  EB ?? EB  or  E9 ?? ??
	if (!((bpb->jmp[0] == 0xEB && bpb->jmp[2] == 0x90) ||
	      bpb->jmp[0] == 0xE9))
		return 0;
	if (bpb->bytes_per_sector != 512 && bpb->bytes_per_sector != 1024 &&
	    bpb->bytes_per_sector != 2048 && bpb->bytes_per_sector != 4096)
		return 0;
	// Sectors per cluster must be power of two and <= 128 (FAT spec)
	if (bpb->sectors_per_cluster == 0 ||
	    (bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) ||
	    bpb->sectors_per_cluster > 128)
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
	if (!(bpb->fs_type[0] == 'F' && bpb->fs_type[1] == 'A' &&
	      bpb->fs_type[2] == 'T'))
		return 0;
	return 1;
}

static int fat32_mount_at_lba(const block_device_t *bdev,
			      unsigned long base_lba, fat32_fs_t *out)
{
	BUG_ON(bdev == NULL);
	BUG_ON(out == NULL);
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
		if (!(bpb->bytes_per_sector == 512 ||
		      bpb->bytes_per_sector == 1024 ||
		      bpb->bytes_per_sector == 2048 ||
		      bpb->bytes_per_sector == 4096) ||
		    bpb->sectors_per_cluster == 0 ||
		    bpb->reserved_sector_count == 0 || bpb->num_fats == 0 ||
		    bpb->root_cluster < 2 ||
		    (bpb->fat_size16 == 0 && bpb->fat_size32 == 0)) {
			kfree(sector);
			return ST_ERR;
		}
	}
	unsigned long fat_sz =
		bpb->fat_size32 ? bpb->fat_size32 : bpb->fat_size16;
	unsigned long first_data_sector =
		bpb->reserved_sector_count + (bpb->num_fats * fat_sz);
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
	fat32_sb_attach(out); /* publish vfs_superblock_t for the caches */
	if (!g_root_dir_cache)
		g_root_dir_cache = kalloc(out->sectors_per_cluster *
					  out->bytes_per_sector);
	if (g_root_dir_cache) {
		unsigned long root_lba = cluster_to_lba(out, out->root_cluster);
		unsigned long read_count =
			out->sectors_per_cluster ? out->sectors_per_cluster : 1;
		read_sectors(bdev, root_lba, read_count, g_root_dir_cache);
	}
	unsigned long abs_data = out->part_lba_offset + out->data_start_lba;
	unsigned long abs_fat = out->part_lba_offset + out->fat_start_lba;

	/* ---- Read FSInfo sector to get cached free cluster count ---- */
	g_free_cluster_count = (unsigned long)-1; /* assume unknown */
	g_fsinfo_sector = 0;
	{
		uint16_t fsinfo_sec = bpb->fs_info;
		if (fsinfo_sec >= 1 &&
		    fsinfo_sec < bpb->reserved_sector_count) {
			void *fi_buf = kalloc(512);
			if (fi_buf) {
				unsigned long fi_lba = base_lba + fsinfo_sec;
				if (read_sectors(bdev, fi_lba, 1, fi_buf) ==
				    ST_OK) {
					uint8_t *fb = (uint8_t *)fi_buf;
					/* Validate FSInfo signatures: 0x41615252 at 0, 0x61417272 at 484 */
					uint32_t sig1 = *(uint32_t *)(fb + 0);
					uint32_t sig2 = *(uint32_t *)(fb + 484);
					if (sig1 == 0x41615252 &&
					    sig2 == 0x61417272) {
						uint32_t free_cnt =
							*(uint32_t *)(fb + 488);
						if (free_cnt != 0xFFFFFFFF) {
							g_free_cluster_count =
								free_cnt;
							g_fsinfo_sector =
								fi_lba;
							kprintf("FAT32: FSInfo free clusters = %lu\n",
								g_free_cluster_count);
						}
					}
				}
				kfree(fi_buf);
			}
		}
	}

	kprintf("FAT32: mounted %s base=%lu root=%lu\n", bdev->name, base_lba,
		out->root_cluster);
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
			if (p[i].type == 0xEE)
				protective_gpt = 1; // Protective GPT
		}
	}

	// Collect MBR partition starts first (skip protective entry itself)
	if (has_sig) {
		for (int i = 0; i < 4; i++) {
			if (p[i].lba_first && p[i].sectors &&
			    p[i].type != 0x00 && p[i].type != 0xEE) {
				int dup = 0;
				for (int j = 0; j < cand_count; j++)
					if (candidates[j] == p[i].lba_first)
						dup = 1;
				if (!dup &&
				    cand_count < (int)(sizeof(candidates) /
						       sizeof(candidates[0])))
					candidates[cand_count++] =
						p[i].lba_first;
			}
		}
	}

	// If GPT, parse GPT header to discover ESP (or any) partitions
	if (protective_gpt) {
		void *hdr = kalloc(512);
		if (hdr && read_sectors(bdev, 1, 1, hdr) == ST_OK) {
			uint8_t *h = (uint8_t *)hdr;
			if (h[0] == 'E' && h[1] == 'F' && h[2] == 'I' &&
			    h[3] == ' ' && h[4] == 'P' && h[5] == 'A' &&
			    h[6] == 'R' && h[7] == 'T') {
				uint32_t num_entries = *(uint32_t *)(h + 80);
				uint32_t entry_size = *(uint32_t *)(h + 84);
				uint64_t part_lba = *(
					uint64_t
						*)(h +
						   72); // partition entry starting LBA
				if (entry_size >= 128 && num_entries > 0 &&
				    num_entries < 512) {
					uint32_t to_read = num_entries;
					if (to_read > 32)
						to_read = 32; // cap
					uint64_t bytes =
						(uint64_t)to_read * entry_size;
					uint32_t sectors =
						(uint32_t)((bytes + 511) / 512);
					void *pe = kalloc(sectors * 512);
					if (pe &&
					    read_sectors(
						    bdev,
						    (unsigned long)part_lba,
						    sectors, pe) == ST_OK) {
						for (uint32_t i = 0;
						     i < to_read; i++) {
							uint8_t *e =
								(uint8_t *)pe +
								i * entry_size;
							int empty = 1;
							for (int k = 0; k < 16;
							     k++)
								if (e[k]) {
									empty = 0;
									break;
								}
							if (empty)
								continue;
							// ESP type GUID little-endian: 28 73 2A C1 1F F8 D2 11 BA 4B 00 A0 C9 3E C9 3B
							int is_esp =
								(e[0] == 0x28 &&
								 e[1] == 0x73 &&
								 e[2] == 0x2A &&
								 e[3] == 0xC1 &&
								 e[4] == 0x1F &&
								 e[5] == 0xF8 &&
								 e[6] == 0xD2 &&
								 e[7] == 0x11 &&
								 e[8] == 0xBA &&
								 e[9] == 0x4B &&
								 e[10] ==
									 0x00 &&
								 e[11] ==
									 0xA0 &&
								 e[12] ==
									 0xC9 &&
								 e[13] ==
									 0x3E &&
								 e[14] ==
									 0xC9 &&
								 e[15] == 0x3B);
							uint64_t first_lba = *(
								uint64_t *)(e +
									    32);
							if (first_lba &&
							    first_lba <
								    0xFFFFFFFFULL) {
								// Add ESP first, others later
								if (is_esp) {
									int dup =
										0;
									for (int j = 0;
									     j <
									     cand_count;
									     j++)
										if (candidates
											    [j] ==
										    (unsigned long)
											    first_lba)
											dup = 1;
									if (!dup &&
									    cand_count <
										    (int)(sizeof(candidates) /
											  sizeof(candidates
													 [0])))
										candidates[cand_count++] =
											(unsigned long)
												first_lba;
								}
							}
						}
						// Second pass: add any non-ESP we didn't add yet
						for (uint32_t i = 0;
						     i < to_read; i++) {
							uint8_t *e =
								(uint8_t *)pe +
								i * entry_size;
							int empty = 1;
							for (int k = 0; k < 16;
							     k++)
								if (e[k]) {
									empty = 0;
									break;
								}
							if (empty)
								continue;
							int is_esp =
								(e[0] == 0x28 &&
								 e[1] == 0x73 &&
								 e[2] == 0x2A &&
								 e[3] == 0xC1 &&
								 e[4] == 0x1F &&
								 e[5] == 0xF8 &&
								 e[6] == 0xD2 &&
								 e[7] == 0x11 &&
								 e[8] == 0xBA &&
								 e[9] == 0x4B &&
								 e[10] ==
									 0x00 &&
								 e[11] ==
									 0xA0 &&
								 e[12] ==
									 0xC9 &&
								 e[13] ==
									 0x3E &&
								 e[14] ==
									 0xC9 &&
								 e[15] == 0x3B);
							if (is_esp)
								continue;
							uint64_t first_lba = *(
								uint64_t *)(e +
									    32);
							if (first_lba &&
							    first_lba <
								    0xFFFFFFFFULL) {
								int dup = 0;
								for (int j = 0;
								     j <
								     cand_count;
								     j++)
									if (candidates
										    [j] ==
									    (unsigned long)
										    first_lba)
										dup = 1;
								if (!dup &&
								    cand_count <
									    (int)(sizeof(candidates) /
										  sizeof(candidates
												 [0])))
									candidates[cand_count++] =
										(unsigned long)
											first_lba;
							}
						}
					}
					if (pe)
						kfree(pe);
				}
			}
		}
		if (hdr)
			kfree(hdr);
	}

	// Only consider LBA0 as superfloppy last if NOT GPT and no conventional partitions found
	if (!protective_gpt) {
		int any_part = 0;
		for (int i = 0; i < 4; i++)
			if (p[i].type != 0)
				any_part = 1;
		if (!any_part) {
			// attempt superfloppy at 0 (add if not already from earlier logic)
			int dup = 0;
			for (int j = 0; j < cand_count; j++)
				if (candidates[j] == 0)
					dup = 1;
			if (!dup && cand_count < (int)(sizeof(candidates) /
						       sizeof(candidates[0])))
				candidates[cand_count++] = 0;
		}
	}
	kfree(mbr);

	// Also add sector 2048 heuristic if space left and not already included
	if (cand_count < (int)(sizeof(candidates) / sizeof(candidates[0]))) {
		int dup = 0;
		for (int j = 0; j < cand_count; j++)
			if (candidates[j] == 2048)
				dup = 1;
		if (!dup)
			candidates[cand_count++] = 2048;
	}

	/* (debug output removed) */
	fat32_fs_t best;
	int best_set = 0;
	int best_score = -999;
	for (int i = 0; i < cand_count; i++) {
		fat32_fs_t tmp;
		if (fat32_mount_at_lba(bdev, candidates[i], &tmp) == ST_OK) {
			int score = 0;
			if (candidates[i] != 0)
				score += 10; /* prefer partition */
			if (!best_set || score > best_score) {
				best = tmp;
				best_set = 1;
				best_score = score;
			}
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
				   void (*cb)(const char *, unsigned,
					      unsigned long),
				   unsigned *out_count)
{
	if (!g_root_fs || !cb)
		return ST_INVALID;
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	FAT32_LOG(
		"list start cluster=%lu cluster_size=%u bytes_per_sector=%u spc=%u\n",
		start_cluster, cluster_size, g_root_fs->bytes_per_sector,
		g_root_fs->sectors_per_cluster);
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
			FAT32_LOG("list read err cluster=%lu lba=%lu st=%d\n",
				  cluster, lba, st);
			kfree(buf);
			return ST_IO;
		}
		clusters++;
		fat32_dirent_t *ents = (fat32_dirent_t *)buf;
		unsigned entries = cluster_size / sizeof(fat32_dirent_t);
		FAT32_LOG("list cluster=%lu lba=%lu entries=%u\n", cluster, lba,
			  entries);
		for (unsigned i = 0; i < entries; i++) {
			if (ents[i].name[0] == 0x00) {
				FAT32_LOG(
					"list end marker cluster=%lu index=%u listed=%u\n",
					cluster, i, listed);
				kfree(buf);
				if (out_count)
					*out_count = listed;
				FAT32_LOG(
					"list done start=%lu clusters=%u listed=%u\n",
					start_cluster, clusters, listed);
				return ST_OK;
			}
			if (ents[i].name[0] == 0xE5)
				continue;
			if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
			    FAT32_ATTR_LONG_NAME) {
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
					for (j = 0; j < 13 && (pos + j) < 259;
					     j++) {
						uint16_t c = parts[j];
						if (c == 0x0000 ||
						    c == 0xFFFF) {
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
				temp[p++] = fat32_tolower(
					ents[i].name
						[j]); // Convert to lowercase
			}
			if (ents[i].name[8] != ' ') {
				temp[p++] = '.';
				for (int j = 8; j < 11; j++) {
					if (ents[i].name[j] == ' ')
						break;
					temp[p++] = fat32_tolower(
						ents[i].name
							[j]); // Convert to lowercase
				}
			}
			temp[p] = '\0';
			cb((lfn_buf[0]) ? lfn_buf : temp, ents[i].attr,
			   ents[i].fileSize);
			listed++;
			lfn_buf[0] = '\0';
			lfn_len = 0; // Reset LFN length for next entry
		}
		kfree(buf);
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, cluster);
		if (next >= 0x0FFFFFF8 || next == 0)
			break;
		cluster = next;
	}
	if (out_count)
		*out_count = listed;
	FAT32_LOG(
		"list done start=%lu clusters=%u listed=%u last_cluster=%lu\n",
		start_cluster, clusters, listed, cluster);
	return ST_OK;
}

int fat32_dir_list(unsigned long cluster,
		   void (*cb)(const char *, unsigned, unsigned long))
{
	return fat32_dir_list_internal(cluster, cb, 0);
}

/* Convert Unix epoch seconds to FAT32 date/time fields.
 * Date: bits 15-9 = year-1980, 8-5 = month(1-12), 4-0 = day(1-31)
 * Time: bits 15-11 = hours, 10-5 = minutes, 4-0 = seconds/2 */
static void fat32_epoch_to_datetime(uint64_t epoch, uint16_t *date_out,
				    uint16_t *time_out)
{
	uint32_t days = (uint32_t)(epoch / 86400);
	uint32_t tod = (uint32_t)(epoch % 86400);
	int hours = (int)(tod / 3600);
	int mins = (int)((tod % 3600) / 60);
	int secs = (int)(tod % 60);

	/* Walk years from 1970 */
	int year = 1970;
	while (1) {
		int lp =
			(year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
		uint32_t yd = lp ? 366u : 365u;
		if (days < yd)
			break;
		days -= yd;
		year++;
	}
	static const int mdays[] = { 0,  31, 28, 31, 30, 31, 30,
				     31, 31, 30, 31, 30, 31 };
	int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
	int month = 1;
	for (; month <= 12; month++) {
		int md = mdays[month] + (month == 2 && leap ? 1 : 0);
		if (days < (uint32_t)md)
			break;
		days -= (uint32_t)md;
	}
	int day = (int)days + 1;

	int fat_year = year - 1980;
	if (fat_year < 0)
		fat_year = 0;
	if (fat_year > 127)
		fat_year = 127;

	*date_out = (uint16_t)(((fat_year & 0x7F) << 9) |
			       ((month & 0x0F) << 5) | (day & 0x1F));
	*time_out = (uint16_t)(((hours & 0x1F) << 11) | ((mins & 0x3F) << 5) |
			       ((secs / 2) & 0x1F));
}

/* Convert FAT32 date/time to Unix epoch seconds.
 * Date: bits 15-9 = year-1980, 8-5 = month(1-12), 4-0 = day(1-31)
 * Time: bits 15-11 = hours, 10-5 = minutes, 4-0 = seconds/2 */
static uint64_t fat32_datetime_to_epoch(uint16_t date, uint16_t time_val)
{
	if (date == 0)
		return 0;
	int year = ((date >> 9) & 0x7F) + 1980;
	int month = (date >> 5) & 0x0F;
	int day = date & 0x1F;
	int hour = (time_val >> 11) & 0x1F;
	int min = (time_val >> 5) & 0x3F;
	int sec = (time_val & 0x1F) * 2;
	if (month < 1)
		month = 1;
	if (day < 1)
		day = 1;
	static const int mdays[] = { 0,  31, 28, 31, 30, 31, 30,
				     31, 31, 30, 31, 30, 31 };
	uint64_t days = 0;
	for (int y = 1970; y < year; y++) {
		int lp = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		days += lp ? 366 : 365;
	}
	int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
	for (int m = 1; m < month && m <= 12; m++) {
		days += mdays[m];
		if (m == 2 && leap)
			days++;
	}
	days += day - 1;
	return days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
}

// Lock-free core of fat32_dir_find — caller must hold fat32_lock or ensure
// exclusive access.  Used by internal helpers (fat32_resolve_parent, etc.)
// that are already called under fat32_lock.
// out_wrt_time/out_wrt_date are optional — pass NULL if not needed.
static int fat32_dir_find_nolock(unsigned long start_cluster, const char *name,
				 unsigned *attr, unsigned long *first_cluster,
				 unsigned long *size, uint16_t *out_wrt_time,
				 uint16_t *out_wrt_date)
{
	BUG_ON(name == NULL);
	if (!g_root_fs || !name)
		return ST_INVALID;

	// --- Dentry cache lookup ---
	dc_entry_t *cached = dcache_lookup(start_cluster, name);
	if (cached) {
		if (cached->flags & DC_NEGATIVE)
			return ST_NOT_FOUND;
		if (attr)
			*attr = cached->attr;
		if (first_cluster)
			*first_cluster = cached->start_cluster;
		if (size)
			*size = cached->size;
		if (out_wrt_time)
			*out_wrt_time = cached->wrt_time;
		if (out_wrt_date)
			*out_wrt_date = cached->wrt_date;
		return ST_OK;
	}

	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
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
		if (read_sectors(
			    g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster),
			    g_root_fs->sectors_per_cluster, buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		fat32_dirent_t *ents = (fat32_dirent_t *)buf;
		unsigned entries = cluster_size / sizeof(fat32_dirent_t);
		for (unsigned i = 0; i < entries; i++) {
			if (ents[i].name[0] == 0x00) {
				kfree(buf);
				dcache_insert_negative(start_cluster, name);
				return ST_NOT_FOUND;
			}
			if (ents[i].name[0] == 0xE5)
				continue;
			if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
			    FAT32_ATTR_LONG_NAME) {
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
					for (j = 0; j < 13 && (pos + j) < 259;
					     j++) {
						uint16_t c = parts[j];
						if (c == 0x0000 ||
						    c == 0xFFFF) {
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
				unsigned int found_attr = ents[i].attr;
				unsigned long found_fc =
					((unsigned long)ents[i].fstClusHI
					 << 16) |
					ents[i].fstClusLO;
				unsigned long found_sz = ents[i].fileSize;
				uint16_t found_wt = ents[i].wrtTime;
				uint16_t found_wd = ents[i].wrtDate;
				kfree(buf);
				// Insert into dentry cache
				dcache_insert(start_cluster, name, found_fc,
					      found_sz, found_attr, found_wt,
					      found_wd, cluster, i, 0, 0);
				if (attr)
					*attr = found_attr;
				if (first_cluster)
					*first_cluster = found_fc;
				if (size)
					*size = found_sz;
				if (out_wrt_time)
					*out_wrt_time = found_wt;
				if (out_wrt_date)
					*out_wrt_date = found_wd;
				return ST_OK;
			}
			lfn_buf[0] = '\0';
			lfn_len = 0; // Reset LFN length for next entry
		}
		kfree(buf);
		unsigned long next =
			fat32_next_cluster_cached(g_root_fs, cluster);
		if (next >= 0x0FFFFFF8 || next == 0)
			break;
		cluster = next;
	}
	dcache_insert_negative(start_cluster, name);
	return ST_NOT_FOUND;
}

// Public API: acquires fat32_lock, then delegates to nolock version.
int fat32_dir_find(unsigned long start_cluster, const char *name,
		   unsigned *attr, unsigned long *first_cluster,
		   unsigned long *size)
{
	uint64_t flags;
	spin_lock_irqsave(&fat32_lock, &flags);
	int ret = fat32_dir_find_nolock(start_cluster, name, attr,
					first_cluster, size, NULL, NULL);
	spin_unlock_irqrestore(&fat32_lock, flags);
	return ret;
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
		if (fat32_dir_find_nolock(current, part, &a, &fc, &sz, NULL,
					  NULL) != ST_OK)
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
int fat32_resolve_path(unsigned long start_cluster, const char *path,
		       unsigned *attr, unsigned long *first_cluster,
		       unsigned long *size)
{
	if (!g_root_fs || !path)
		return ST_INVALID;
	unsigned long current =
		(start_cluster == 0) ? g_root_dir_cluster : start_cluster;
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
		} else if (part[0] == '.' && part[1] == '.' &&
			   part[2] == '\0') {
			current = fat32_parent_cluster(current);
		} else {
			unsigned a;
			unsigned long fc;
			unsigned long sz;
			if (fat32_dir_find_nolock(current, part, &a, &fc, &sz,
						  NULL, NULL) != ST_OK)
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
	return fat32_resolve_path(start_cluster, path, attr, first_cluster,
				  size);
}

unsigned long fat32_parent_cluster(unsigned long dir_cluster)
{
	if (dir_cluster == g_root_dir_cluster || dir_cluster == 0)
		return g_root_dir_cluster;
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
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
		if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
		    FAT32_ATTR_LONG_NAME)
			continue;
		if (ents[i].name[0] == '.') {
			if (ents[i].name[1] == '.') {
				unsigned long parent =
					((unsigned long)ents[i].fstClusHI
					 << 16) |
					ents[i].fstClusLO;
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

// Forward declare _impl functions (unlocked, internal)
static int fat32_open_impl(const char *path, int flags, vfs_file_t **out);
static int fat32_stat_vfs_impl(const char *path, struct kstat *st);
static long fat32_read_impl(vfs_file_t *f, void *buf, long bytes);
static long fat32_write_impl(vfs_file_t *f, const void *buf, long bytes);
static long fat32_seek_impl(vfs_file_t *f, long offset, int whence);
static long fat32_readdir_impl(vfs_file_t *f, void *buf, long bytes);
static int fat32_truncate_impl(vfs_file_t *f, unsigned long size);
/* close() acquires fat32_io_lock now since it does disk I/O (deferred
 * dirent update + FAT flush).  Forward decl of the locked wrapper. */
static int fat32_close(vfs_file_t *f);
static int fat32_unlink_impl(const char *path);
static int fat32_rename_impl(const char *oldpath, const char *newpath);
static int fat32_mkdir_impl(const char *path, unsigned int mode);
static int fat32_rmdir_impl(const char *path);
static int fat32_chdir_impl(const char *path);
static int fat32_resolve_parent(unsigned long start_cluster, const char *path,
				unsigned long *parent_cluster, char *name_out,
				unsigned name_out_len);

/* fat32_utimensat_impl - Update mtime (wrtTime/wrtDate) of a file on FAT32.
 * times[0] = atime (FAT32 has no atime field, ignored)
 * times[1] = mtime
 * If times is NULL, set mtime to current time (use timer_get_epoch()).
 * tv_nsec == KRN_UTIME_OMIT: leave that timestamp unchanged.
 * tv_nsec == KRN_UTIME_NOW: set to current time.
 */
static int fat32_utimensat_impl(const char *path, int64_t mtime_sec,
				long mtime_nsec)
{
	if (!g_root_fs || !path)
		return ST_INVALID;

	/* Resolve parent cluster + final component name */
	unsigned long start_cluster = fat32_get_task_cwd_cluster();
	const char *rpath = fat32_normalize_start(path, &start_cluster);

	unsigned long parent_cluster = 0;
	char name[FAT32_LFN_MAX];
	if (fat32_resolve_parent(start_cluster, rpath, &parent_cluster, name,
				 sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;

	/* Find the short-name directory entry location */
	unsigned attr = 0;
	unsigned long fc = 0, sz = 0;
	unsigned long dir_cluster = 0;
	unsigned int dir_index = 0;
	if (fat32_dir_find_entry_lfn(parent_cluster, name, &attr, &fc, &sz,
				     &dir_cluster, &dir_index, NULL,
				     NULL) != ST_OK)
		return ST_NOT_FOUND;

	/* Determine the new mtime */
	uint16_t new_date, new_time;
	if (mtime_nsec == KRN_UTIME_OMIT) {
		/* Caller wants to leave mtime unchanged — nothing to do */
		return ST_OK;
	} else if (mtime_nsec == KRN_UTIME_NOW) {
		uint64_t now = timer_get_epoch();
		fat32_epoch_to_datetime(now, &new_date, &new_time);
	} else {
		fat32_epoch_to_datetime((uint64_t)mtime_sec, &new_date,
					&new_time);
	}

	/* Read the cluster containing the dirent, update timestamps, write back */
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	void *buf = kalloc(cluster_size);
	if (!buf)
		return ST_NOMEM;

	unsigned long lba = cluster_to_lba(g_root_fs, dir_cluster);
	if (read_sectors(g_root_fs->bdev, lba, g_root_fs->sectors_per_cluster,
			 buf) != ST_OK) {
		kfree(buf);
		return ST_IO;
	}

	fat32_dirent_t *ents = (fat32_dirent_t *)buf;
	ents[dir_index].wrtTime = new_time;
	ents[dir_index].wrtDate = new_date;
	ents[dir_index].lstAccDate = new_date;

	int st = write_sectors(g_root_fs->bdev, lba,
			       g_root_fs->sectors_per_cluster, buf);
	kfree(buf);

	/* Invalidate dentry cache so next stat re-reads from disk */
	dcache_invalidate(parent_cluster, name);

	return st;
}

// Locked wrappers: serialize all FAT32 VFS operations via the sleeping mutex
// to protect the shared FAT cache (g_fat_cache) from SMP races.
static int fat32_open(const char *path, int flags, vfs_file_t **out)
{
	fat32_io_lock();
	int r = fat32_open_impl(path, flags, out);
	fat32_io_unlock();
	return r;
}
static int fat32_stat_vfs(const char *path, struct kstat *st)
{
	fat32_io_lock();
	int r = fat32_stat_vfs_impl(path, st);
	fat32_io_unlock();
	return r;
}
/* fstat on an open handle: report size + type straight from the in-memory file
 * object (no path lookup, no disk I/O).  Lets the VFS read a file's size in a
 * filesystem-independent way (vfs_size -> ops->fstat) without casting to a
 * driver-specific struct. */
static int fat32_fstat(vfs_file_t *f, struct kstat *st)
{
	if (!f || !st)
		return ST_INVALID;
	fat32_file_t *ff =
		(fat32_file_t *)f; /* vfs_file_t is the first member */
	mm_memset(st, 0, sizeof(*st));
	st->st_nlink = 1;
	st->st_blksize = g_root_fs ? (uint64_t)g_root_fs->sectors_per_cluster *
					     g_root_fs->bytes_per_sector :
				     4096;
	st->st_ino = ff->start_cluster;
	if (ff->is_dir) {
		st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
					 S_IXGRP | S_IROTH | S_IXOTH);
	} else {
		st->st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		st->st_size = (long)ff->size;
		st->st_blocks = (long)((ff->size + 511) / 512);
	}
	uint64_t now = timer_get_epoch();
	st->st_mtime = now;
	st->st_atime = now;
	st->st_ctime = now;
	return ST_OK;
}
static long fat32_read(vfs_file_t *f, void *buf, long bytes)
{
	fat32_io_lock();
	long r = fat32_read_impl(f, buf, bytes);
	fat32_io_unlock();
	return r;
}
static long fat32_write(vfs_file_t *f, const void *buf, long bytes)
{
	fat32_io_lock();
	long r = fat32_write_impl(f, buf, bytes);
	fat32_io_unlock();
	return r;
}
static long fat32_seek(vfs_file_t *f, long offset, int whence)
{
	fat32_io_lock();
	long r = fat32_seek_impl(f, offset, whence);
	fat32_io_unlock();
	return r;
}
static long fat32_readdir(vfs_file_t *f, void *buf, long bytes)
{
	fat32_io_lock();
	long r = fat32_readdir_impl(f, buf, bytes);
	fat32_io_unlock();
	return r;
}
static int fat32_truncate(vfs_file_t *f, unsigned long size)
{
	fat32_io_lock();
	int r = fat32_truncate_impl(f, size);
	fat32_io_unlock();
	return r;
}
static int fat32_unlink(const char *path)
{
	fat32_io_lock();
	int r = fat32_unlink_impl(path);
	fat32_io_unlock();
	return r;
}
static int fat32_rename(const char *oldpath, const char *newpath)
{
	fat32_io_lock();
	int r = fat32_rename_impl(oldpath, newpath);
	fat32_io_unlock();
	return r;
}
static int fat32_mkdir(const char *path, unsigned int mode)
{
	fat32_io_lock();
	int r = fat32_mkdir_impl(path, mode);
	fat32_io_unlock();
	return r;
}
static int fat32_rmdir(const char *path)
{
	fat32_io_lock();
	int r = fat32_rmdir_impl(path);
	fat32_io_unlock();
	return r;
}
static int fat32_chdir(const char *path)
{
	fat32_io_lock();
	int r = fat32_chdir_impl(path);
	fat32_io_unlock();
	return r;
}
int fat32_utimensat(const char *path, int64_t mtime_sec, long mtime_nsec)
{
	fat32_io_lock();
	int r = fat32_utimensat_impl(path, mtime_sec, mtime_nsec);
	fat32_io_unlock();
	return r;
}

static int fat32_release_locks_for_task(uint64_t task_id)
{
	int released = fat32_io_release_if_owner(task_id);
	/* Also release any per-device block-layer sleeping locks the dying
     * task may have been holding (nested inside fat32_io_lock — e.g.
     * usb_msd's io_locked acquired by read_sectors/write_sectors).
     * Without this, the FAT lock recovers but every subsequent USB I/O
     * still hangs on the orphaned MSD mutex. */
	if (g_root_fs && g_root_fs->bdev &&
	    g_root_fs->bdev->release_locks_for_task) {
		released |= g_root_fs->bdev->release_locks_for_task(
			(block_device_t *)g_root_fs->bdev, task_id);
	}
	return released;
}

static int fat32_close_impl(vfs_file_t *f);
static int fat32_close(vfs_file_t *f)
{
	/* close() now does disk I/O (deferred dirent update + FAT-sector
     * flush) so it needs the same lock the other ops hold.  Without
     * this, the read-modify-write inside fat32_update_dirent races
     * against concurrent fat32_open / fat32_read / fat32_write on
     * other CPUs, and the dirent we just modified can be re-read by
     * a sibling operation that then writes back its own version,
     * silently dropping our update — observed as "ls shows old size
     * after curl re-downloaded over an existing file". */
	fat32_io_lock();
	int r = fat32_close_impl(f);
	fat32_io_unlock();
	return r;
}

/* fsync: flush this file's cached data pages and dirty inode metadata. */
static int fat32_fsync(vfs_file_t *f)
{
	if (!f)
		return 0;
	fat32_file_t *ff = (fat32_file_t *)f->fs_private;
	if (ff && ff->start_cluster >= 2) {
		pagecache_flush_file(ff->start_cluster);
		if (ff->inode)
			icache_flush((ic_inode_t *)ff->inode);
	}
	return 0;
}

/* Fill the generic statfs struct from the FAT32-native one. */
static int fat32_statfs_op(struct vfs_statfs *out)
{
	if (!out)
		return ST_INVALID;
	fat32_statfs_t k;
	mm_memset(&k, 0, sizeof(k));
	if (fat32_get_statfs(&k) != 0)
		return ST_IO;
	out->f_type = k.f_type;
	out->f_bsize = k.f_bsize;
	out->f_frsize = k.f_frsize;
	out->f_blocks = k.f_blocks;
	out->f_bfree = k.f_bfree;
	out->f_bavail = k.f_bavail;
	out->f_files = k.f_files;
	out->f_ffree = k.f_ffree;
	out->f_fsid = k.f_fsid;
	out->f_namelen = k.f_namelen;
	return ST_OK;
}

/* FAT32 stores no ownership or permission bits, so it emulates a fully
 * permissive policy: every user may read and traverse (and, lacking any owner,
 * write) any file or directory.  A future mount option could instead stamp the
 * files with the mounting user's uid/gid.  Returning ST_OK makes the VFS
 * permission check allow the access outright, before the generic mode-bit
 * check the emulated metadata would otherwise drive. */
static int fat32_permission_op(const char *path, unsigned long ino, int want)
{
	(void)path;
	(void)ino;
	(void)want;
	return ST_OK; /* no on-disk perms: allow everyone */
}

/* FAT32 has no symlinks, hard links, permission bits or ownership, so those
 * ops stay NULL — the vfs_* wrappers then apply the legacy fallbacks (chmod/
 * chown succeed silently, symlink/link report unsupported, lstat == stat).
 * The permission op above makes access permissive for all users. */
static const vfs_ops_t fat32_vfs_ops = {
	fat32_open,       fat32_stat_vfs,
	fat32_read,       fat32_write,
	fat32_seek,       fat32_readdir,
	fat32_truncate,   fat32_unlink,
	fat32_rename,     fat32_mkdir,
	fat32_rmdir,      fat32_chdir,
	fat32_close,      fat32_release_locks_for_task,
	fat32_fsync,
	/* lstat */ 0,    /* symlink */ 0,
	/* readlink */ 0, /* link */ 0,
	/* chmod */ 0,    /* chown */ 0,
	/* fchmod */ 0,   /* fchown */ 0,
	fat32_utimensat,  fat32_statfs_op,
	fat32_fstat,
	.permission = fat32_permission_op,
};

static int fat32_resolve_parent(unsigned long start_cluster, const char *path,
				unsigned long *parent_cluster, char *name_out,
				unsigned name_out_len)
{
	if (!g_root_fs || !path || !parent_cluster || !name_out ||
	    name_out_len < 2)
		return ST_INVALID;
	unsigned long current =
		(start_cluster == 0) ? g_root_dir_cluster : start_cluster;
	if (path[0] == '/') {
		current = g_root_dir_cluster;
		path++;
	}
	if (!*path)
		return ST_INVALID;
	const char *seg = path;
	char part[FAT32_LFN_MAX]; // Increased for LFN support
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
			if (fat32_dir_find_nolock(current, part, &a, &fc, &sz,
						  NULL, NULL) != ST_OK)
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
			for (unsigned i = 0; i < name_out_len - 1 && part[i];
			     ++i) {
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
	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
	void *buf = kalloc(cluster_size);
	if (!buf)
		return ST_NOMEM;
	unsigned long lba = cluster_to_lba(ff->fs, ff->dirent_cluster);
	if (read_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster, buf) !=
	    ST_OK) {
		kfree(buf);
		return ST_IO;
	}
	fat32_dirent_t *ents = (fat32_dirent_t *)buf;
	fat32_dirent_t *ent = &ents[ff->dirent_index];
	ent->fstClusHI = (uint16_t)((ff->start_cluster >> 16) & 0xFFFF);
	ent->fstClusLO = (uint16_t)(ff->start_cluster & 0xFFFF);
	ent->fileSize = (uint32_t)ff->size;
	int st = write_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster,
			       buf);
	kfree(buf);
	/* The dentry cache holds (start_cluster, size, ...) snapshotted from
     * the on-disk dirent at lookup time.  After mutating that dirent we
     * must invalidate the parent's dcache entries so the next stat()
     * re-reads from disk; otherwise ls keeps showing the pre-truncate
     * size even though the dirent on disk is correct. */
	if (ff->parent_cluster)
		dcache_invalidate_dir(ff->parent_cluster);
	return st;
}

static int fat32_set_position(fat32_file_t *ff, unsigned long target_pos)
{
	if (!ff || !ff->fs)
		return ST_INVALID;
	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
	if (ff->start_cluster < 2 || ff->size == 0) {
		ff->current_cluster = ff->start_cluster;
		ff->pos = target_pos;
		return ST_OK;
	}
	unsigned long cluster = ff->start_cluster;
	unsigned long cluster_start = 0;
	/* Walk to the cluster containing target_pos.  Do NOT gate on
     * `target_pos < ff->size`: that bug made set_position(ff, ff->size)
     * return start_cluster for multi-cluster files (which the write path
     * relies on when re-deriving after a boundary-aligned exit). */
	while (cluster_start + cluster_size <= target_pos) {
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

static int fat32_open_impl(const char *path, int flags, vfs_file_t **out)
{
	if (!out)
		return ST_INVALID;
	if (!path || !path[0])
		return ST_INVALID;
	unsigned long start_cluster = fat32_get_task_cwd_cluster();
	const char *rpath = fat32_normalize_start(path, &start_cluster);

	// Directory open handling
	if (kstrcmp(path, "/") == 0 || kstrcmp(path, ".") == 0 ||
	    kstrcmp(path, "..") == 0 || (rpath && rpath[0] == '\0')) {
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
	char name[256]; // Increased for LFN support
	name[0] = '\0';
	if (fat32_resolve_parent(start_cluster, rpath, &parent, name,
				 sizeof(name)) != ST_OK)
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
					      &dir_cluster, &dir_index,
					      &lfn_start_cluster,
					      &lfn_start_index) == ST_OK);

	if (!found) {
		if (!(flags & O_CREAT))
			return ST_NOT_FOUND;

		// Create new entry with LFN if needed
		int entries_needed =
			fname.lfn_entries + 1; // LFN entries + short entry
		if (fat32_dir_find_free_entries(parent, entries_needed,
						&dir_cluster,
						&dir_index) != ST_OK)
			return ST_IO;

		fat32_dirent_t ent;
		mm_memset(&ent, 0, sizeof(ent));
		for (int i = 0; i < 11; ++i)
			ent.name[i] = fname.short_name[i];
		ent.attr = FAT32_ATTR_ARCHIVE;
		ent.fstClusHI = 0;
		ent.fstClusLO = 0;
		ent.fileSize = 0;

		if (fname.lfn_entries > 0) {
			// Write LFN entries + short entry
			if (fat32_write_lfn_entries(dir_cluster, dir_index,
						    &fname, &ent) != ST_OK)
				return ST_IO;
			// Update dir_index to point to the short entry
			unsigned entries_per_cluster =
				(g_root_fs->sectors_per_cluster *
				 g_root_fs->bytes_per_sector) /
				sizeof(fat32_dirent_t);
			unsigned total_offset = dir_index + fname.lfn_entries;
			while (total_offset >= entries_per_cluster) {
				unsigned long next = fat32_next_cluster_cached(
					g_root_fs, dir_cluster);
				if (next >= 0x0FFFFFF8 || next == 0)
					break;
				dir_cluster = next;
				total_offset -= entries_per_cluster;
			}
			dir_index = total_offset;
		} else {
			if (fat32_write_dirent(dir_cluster, dir_index, &ent) !=
			    ST_OK)
				return ST_IO;
		}
		attr = ent.attr;
		fc = 0;
		size = 0;
		lfn_start_cluster = dir_cluster;
		lfn_start_index = dir_index;
		dcache_invalidate_dir(parent);
	} else {
		if (attr & FAT32_ATTR_DIRECTORY)
			return ST_UNSUPPORTED;
		if (flags & O_TRUNC) {
			if (fc >= 2) {
				pagecache_invalidate_file(fc);
				icache_chain_invalidate(fc);
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
	for (int i = 0; i < 11; ++i)
		ff->name83[i] = fname.short_name[i];
	ff->name83[11] = '\0';
	ff->vfs.ops = &fat32_vfs_ops;
	ff->vfs.fs_private = ff;

	// Attach inode cache entry (increments refcount)
	if (fc >= 2 && !(attr & FAT32_ATTR_DIRECTORY))
		ff->inode = icache_get(fc, size, attr, parent, dir_cluster,
				       dir_index, 0, 0);
	else
		ff->inode = NULL;

	*out = &ff->vfs;

	if ((flags & O_TRUNC) && found) {
		fat32_update_dirent(ff);
	}

	if (flags & O_APPEND) {
		fat32_set_position(ff, ff->size);
	}

	return ST_OK;
}

static int fat32_stat_vfs_impl(const char *path, struct kstat *st)
{
	if (!st || !path)
		return ST_INVALID;
	unsigned long start_cluster = fat32_get_task_cwd_cluster();
	const char *rpath = fat32_normalize_start(path, &start_cluster);

	/* Strip trailing slashes so "/tmp/" resolves the same as "/tmp".
       We work on a small local copy to avoid modifying the caller's string. */
	char clean[512];
	{
		int ci = 0;
		while (rpath[ci] && ci < 511) {
			clean[ci] = rpath[ci];
			ci++;
		}
		clean[ci] = '\0';
		while (ci > 0 && clean[ci - 1] == '/') {
			clean[--ci] = '\0';
		}
		rpath = clean;
	}

	mm_memset(st, 0, sizeof(*st));
	st->st_dev = 0;
	st->st_rdev = 0;
	st->st_nlink = 1;
	st->st_uid = 0;
	st->st_gid = 0;
	st->st_blksize = g_root_fs ? (uint64_t)g_root_fs->sectors_per_cluster *
					     g_root_fs->bytes_per_sector :
				     4096;

	/* Root directory — special case, no parent entry */
	if (rpath[0] == '\0') {
		st->st_ino = g_root_fs ? g_root_fs->root_cluster : 2;
		st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
					 S_IXGRP | S_IROTH | S_IXOTH);
		st->st_size = 0;
		st->st_blocks = 0;
		uint64_t now = timer_get_epoch();
		st->st_mtime = now;
		st->st_atime = now;
		st->st_ctime = now;
		return ST_OK;
	}

	/* Split path into parent directory and final name component.
     * Resolve parent to its cluster, then do ONE dir_find_nolock to get
     * attr + first_cluster + size + timestamps — all in a single pass. */
	const char *last_slash = 0;
	for (const char *p = rpath; *p; p++)
		if (*p == '/')
			last_slash = p;

	unsigned long parent_cluster;
	char final_name[256];

	if (!last_slash) {
		/* Single component: parent is start_cluster */
		parent_cluster = start_cluster;
		int i;
		for (i = 0; rpath[i] && i < 255; i++)
			final_name[i] = rpath[i];
		final_name[i] = '\0';
	} else {
		/* Multi-component: resolve parent path */
		char parent_path[512];
		int plen = (int)(last_slash - rpath);
		for (int i = 0; i < plen && i < 511; i++)
			parent_path[i] = rpath[i];
		parent_path[plen] = '\0';

		unsigned pattr;
		unsigned long pfc;
		unsigned long psz;
		if (fat32_resolve_path(start_cluster, parent_path, &pattr, &pfc,
				       &psz) != ST_OK)
			return ST_NOT_FOUND;
		parent_cluster = pfc;
		const char *fn = last_slash + 1;
		int i;
		for (i = 0; fn[i] && i < 255; i++)
			final_name[i] = fn[i];
		final_name[i] = '\0';
	}

	/* Single directory lookup: get attr, cluster, size AND timestamps */
	unsigned attr = 0;
	unsigned long fc = 0;
	unsigned long size = 0;
	uint16_t wrt_time = 0, wrt_date = 0;
	if (fat32_dir_find_nolock(parent_cluster, final_name, &attr, &fc, &size,
				  &wrt_time, &wrt_date) != ST_OK)
		return ST_NOT_FOUND;

	st->st_size = size;
	st->st_ino = fc; /* use first cluster as inode number */
	st->st_blocks = (size + 511) / 512;
	if (attr & FAT32_ATTR_DIRECTORY) {
		st->st_mode = S_IFDIR | (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP |
					 S_IXGRP | S_IROTH | S_IXOTH);
	} else {
		st->st_mode = S_IFREG | (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}

	uint64_t epoch = fat32_datetime_to_epoch(wrt_date, wrt_time);
	st->st_mtime = epoch;
	st->st_atime = epoch;
	st->st_ctime = epoch;

	return ST_OK;
}

int fat32_unlink_path(const char *path)
{
	/* fat32_unlink_path is always called from fat32_unlink_impl which is
     * wrapped by fat32_io_lock() (sleeping mutex).  The old fat32_lock
     * spinlock here was redundant AND harmful: it disabled IRQs on the
     * calling CPU while performing multiple USB read/write operations.
     * On VMware, the XHCI completion interrupt can land on the same vCPU
     * that holds the spinlock, causing the USB write to stall.  A stalled
     * write leaves the FAT table partially updated in memory but not on
     * disk — a subsequent fat32_alloc_cluster then considers those clusters
     * free and zeros them, silently destroying whichever file or directory
     * owned them.  Remove the spinlock; fat32_io_lock alone is sufficient. */

	unsigned long parent = 0;
	char name[256];
	name[0] = '\0';
	if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent,
				 name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;

	unsigned attr = 0;
	unsigned long fc = 0;
	unsigned long size = 0;
	unsigned long dir_cluster = 0;
	unsigned int dir_index = 0;
	unsigned long lfn_start_cluster = 0;
	unsigned int lfn_start_index = 0;

	if (fat32_dir_find_entry_lfn(
		    parent, name, &attr, &fc, &size, &dir_cluster, &dir_index,
		    &lfn_start_cluster, &lfn_start_index) != ST_OK)
		return ST_NOT_FOUND;

	if (attr & FAT32_ATTR_DIRECTORY)
		return ST_UNSUPPORTED;

	// Delete all entries from LFN start to short entry
	int st = fat32_delete_entries(lfn_start_cluster, lfn_start_index,
				      dir_cluster, dir_index);
	if (st != ST_OK)
		return st;

	if (fc >= 2) {
		pagecache_invalidate_file(fc);
		icache_chain_invalidate(fc);
		icache_remove(fc);
		fat32_free_chain(g_root_fs, fc);
	}
	dcache_invalidate_dir(parent);
	return ST_OK;
}

static int fat32_unlink_impl(const char *path)
{
	return fat32_unlink_path(path);
}

int fat32_rename_path(const char *oldpath, const char *newpath)
{
	unsigned long old_parent = 0;
	char old_name[256];
	old_name[0] = '\0';
	if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), oldpath,
				 &old_parent, old_name,
				 sizeof(old_name)) != ST_OK)
		return ST_NOT_FOUND;
	unsigned long new_parent = 0;
	char new_name[256];
	new_name[0] = '\0';
	if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), newpath,
				 &new_parent, new_name,
				 sizeof(new_name)) != ST_OK)
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
				     &old_dir_cluster, &old_dir_index,
				     &old_lfn_cluster, &old_lfn_index) != ST_OK)
		return ST_NOT_FOUND;

	// Prepare new name
	fat32_name_t new_fname;
	if (fat32_prepare_name(new_name, &new_fname) != ST_OK)
		return ST_INVALID;

	// Delete old entries
	int st = fat32_delete_entries(old_lfn_cluster, old_lfn_index,
				      old_dir_cluster, old_dir_index);
	if (st != ST_OK)
		return st;

	// Find space for new entry
	unsigned long new_dir_cluster = 0;
	unsigned int new_dir_index = 0;
	int entries_needed = new_fname.lfn_entries + 1;
	if (fat32_dir_find_free_entries(new_parent, entries_needed,
					&new_dir_cluster,
					&new_dir_index) != ST_OK)
		return ST_IO;

	// Create new entry
	fat32_dirent_t ent;
	mm_memset(&ent, 0, sizeof(ent));
	for (int i = 0; i < 11; ++i)
		ent.name[i] = new_fname.short_name[i];
	ent.attr = (uint8_t)attr;
	ent.fstClusHI = (uint16_t)((fc >> 16) & 0xFFFF);
	ent.fstClusLO = (uint16_t)(fc & 0xFFFF);
	ent.fileSize = (uint32_t)size;

	if (new_fname.lfn_entries > 0) {
		st = fat32_write_lfn_entries(new_dir_cluster, new_dir_index,
					     &new_fname, &ent);
	} else {
		st = fat32_write_dirent(new_dir_cluster, new_dir_index, &ent);
	}
	if (st == ST_OK) {
		dcache_invalidate_dir(old_parent);
		if (new_parent != old_parent)
			dcache_invalidate_dir(new_parent);
	}

	return st;
}

static int fat32_rename_impl(const char *oldpath, const char *newpath)
{
	return fat32_rename_path(oldpath, newpath);
}

int fat32_mkdir_path(const char *path)
{
	unsigned long parent = 0;
	char name[256];
	name[0] = '\0';
	if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent,
				 name, sizeof(name)) != ST_OK)
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

	if (fat32_dir_find_entry_lfn(
		    parent, name, &attr, &fc, &size, &dir_cluster, &dir_index,
		    &lfn_start_cluster, &lfn_start_index) == ST_OK) {
		return ST_EXISTS; // Already exists
	}

	// Find space for new entries
	int entries_needed = fname.lfn_entries + 1;
	if (fat32_dir_find_free_entries(parent, entries_needed, &dir_cluster,
					&dir_index) != ST_OK)
		return ST_IO;

	unsigned long newc = 0;
	if (fat32_alloc_cluster(g_root_fs, &newc) != ST_OK)
		return ST_IO;

	fat32_dirent_t ent;
	fat32_init_dirent(&ent, fname.short_name, FAT32_ATTR_DIRECTORY, newc,
			  0);

	int st;
	if (fname.lfn_entries > 0) {
		st = fat32_write_lfn_entries(dir_cluster, dir_index, &fname,
					     &ent);
	} else {
		st = fat32_write_dirent(dir_cluster, dir_index, &ent);
	}
	if (st != ST_OK)
		return st;

	// Initialize directory cluster with . and ..
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	void *buf = kcalloc(1, cluster_size);
	if (!buf)
		return ST_NOMEM;
	fat32_dirent_t *ents = (fat32_dirent_t *)buf;
	char dot[11] = {
		'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
	};
	char dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ',
			    ' ', ' ', ' ', ' ', ' ' };
	fat32_init_dirent(&ents[0], dot, FAT32_ATTR_DIRECTORY, newc, 0);
	fat32_init_dirent(&ents[1], dotdot, FAT32_ATTR_DIRECTORY, parent, 0);
	st = write_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, newc),
			   g_root_fs->sectors_per_cluster, buf);
	kfree(buf);
	if (st != ST_OK)
		return st;

	dcache_invalidate_dir(parent);
	return ST_OK;
}

static int fat32_mkdir_impl(const char *path, unsigned int mode)
{
	(void)mode;
	return fat32_mkdir_path(path);
}

int fat32_rmdir_path(const char *path)
{
	unsigned long parent = 0;
	char name[256];
	name[0] = '\0';
	if (fat32_resolve_parent(fat32_get_task_cwd_cluster(), path, &parent,
				 name, sizeof(name)) != ST_OK)
		return ST_NOT_FOUND;

	unsigned attr = 0;
	unsigned long fc = 0;
	unsigned long size = 0;
	unsigned long dir_cluster = 0;
	unsigned int dir_index = 0;
	unsigned long lfn_start_cluster = 0;
	unsigned int lfn_start_index = 0;

	if (fat32_dir_find_entry_lfn(
		    parent, name, &attr, &fc, &size, &dir_cluster, &dir_index,
		    &lfn_start_cluster, &lfn_start_index) != ST_OK)
		return ST_NOT_FOUND;
	if (!(attr & FAT32_ATTR_DIRECTORY))
		return ST_NOT_FOUND;

	// Ensure directory is empty (only . and ..) — walk the full cluster chain
	unsigned cluster_size =
		g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
	unsigned entries_per_cluster = cluster_size / sizeof(fat32_dirent_t);
	void *buf = kalloc(cluster_size);
	if (!buf)
		return ST_NOMEM;
	int empty = 1;
	int done = 0;
	unsigned long scan = fc;
	while (!done && scan >= 2 && scan < 0x0FFFFFF8) {
		if (read_sectors(
			    g_root_fs->bdev, cluster_to_lba(g_root_fs, scan),
			    g_root_fs->sectors_per_cluster, buf) != ST_OK) {
			kfree(buf);
			return ST_IO;
		}
		fat32_dirent_t *ents = (fat32_dirent_t *)buf;
		for (unsigned i = 0; i < entries_per_cluster; ++i) {
			if (ents[i].name[0] == 0x00) {
				done = 1;
				break;
			} // end of directory
			if (ents[i].name[0] == 0xE5)
				continue; // deleted entry
			if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
			    FAT32_ATTR_LONG_NAME)
				continue; // LFN fragment
			if (ents[i].name[0] == '.' &&
			    (ents[i].name[1] == ' ' || ents[i].name[1] == '.'))
				continue; // "." or ".."
			empty = 0;
			done = 1;
			break;
		}
		if (!done) {
			unsigned long next =
				fat32_next_cluster_cached(g_root_fs, scan);
			if (next >= 0x0FFFFFF8 || next == 0)
				break;
			scan = next;
		}
	}
	kfree(buf);
	if (!empty)
		return ST_NOTEMPTY;

	// Delete all entries (LFN + short entry)
	int st = fat32_delete_entries(lfn_start_cluster, lfn_start_index,
				      dir_cluster, dir_index);
	if (st != ST_OK)
		return st;

	if (fc >= 2) {
		icache_chain_invalidate(fc);
		fat32_free_chain(g_root_fs, fc);
	}
	dcache_invalidate_dir(parent);
	return ST_OK;
}

static int fat32_rmdir_impl(const char *path)
{
	return fat32_rmdir_path(path);
}

static int fat32_chdir_impl(const char *path)
{
	unsigned attr = 0;
	unsigned long fc = 0;
	unsigned long size = 0;
	unsigned long start_cluster = fat32_get_task_cwd_cluster();
	const char *rpath = fat32_normalize_start(path, &start_cluster);
	if (fat32_resolve_path(start_cluster, rpath, &attr, &fc, &size) !=
	    ST_OK)
		return ST_NOT_FOUND;
	if (!(attr & FAT32_ATTR_DIRECTORY))
		return ST_INVALID;
	fat32_set_cwd(fc);
	return ST_OK;
}

static long fat32_read_impl(vfs_file_t *f, void *buf, long bytes)
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
	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;

	// Use page cache for regular file reads
	smap_disable();
	while (remaining) {
		// Determine which cache page this file position maps to
		unsigned long page_idx = ff->pos / PAGE_SIZE;
		unsigned page_offset = ff->pos % PAGE_SIZE;
		unsigned avail_in_page = PAGE_SIZE - page_offset;
		unsigned chunk = (remaining < avail_in_page) ?
					 (unsigned)remaining :
					 avail_in_page;

		pc_page_t *pg =
			pagecache_get(ff->start_cluster, page_idx, ff->size,
				      &ff->fs->sb, ff->start_cluster);
		if (pg && pg->data) {
			// Cache hit (or successful disk read on miss)
			mm_memcpy(((uint8_t *)buf) + copied,
				  pg->data + page_offset, chunk);

			// Trigger read-ahead on sequential access
			pc_readahead_t ra_state;
			ra_state.last_page_index = ff->ra_last_page;
			ra_state.sequential_count = ff->ra_seq_count;
			ra_state.ra_pages = ff->ra_pages;
			pagecache_readahead(&ra_state, ff->start_cluster,
					    page_idx, ff->size, &ff->fs->sb,
					    ff->start_cluster);
			ff->ra_last_page = ra_state.last_page_index;
			ff->ra_seq_count = ra_state.sequential_count;
			ff->ra_pages = ra_state.ra_pages;
		} else {
			// Cache miss and disk read failed — fall back to direct read
			// This can happen if the page cache is not yet initialized
			// or if the file's cluster chain is corrupt.
			void *tmp = kalloc(cluster_size);
			if (!tmp) {
				smap_enable();
				return copied ? (long)copied : ST_NOMEM;
			}
			fat32_io_lock();
			int st = read_sectors(
				ff->fs->bdev,
				cluster_to_lba(ff->fs, ff->current_cluster),
				ff->fs->sectors_per_cluster, tmp);
			fat32_io_unlock();
			if (st != ST_OK) {
				smap_enable();
				kfree(tmp);
				return copied ? (long)copied : ST_IO;
			}
			unsigned cluster_offset = ff->pos % cluster_size;
			unsigned avail_in_cluster =
				cluster_size - cluster_offset;
			if (chunk > avail_in_cluster)
				chunk = avail_in_cluster;
			mm_memcpy(((uint8_t *)buf) + copied,
				  ((uint8_t *)tmp) + cluster_offset, chunk);
			kfree(tmp);
		}

		ff->pos += chunk;
		copied += chunk;
		remaining -= chunk;
		if (ff->pos >= ff->size)
			break;

		// Keep current_cluster in sync for the fallback path and seek
		if (ff->pos % cluster_size == 0) {
			fat32_io_lock();
			unsigned long next = fat32_next_cluster_cached(
				ff->fs, ff->current_cluster);
			fat32_io_unlock();
			if (next >= 0x0FFFFFF8) {
				break;
			}
			if (next == 0 && remaining > 0) {
				next = ff->current_cluster + 1;
			}
			if (next == 0) {
				break;
			}
			ff->current_cluster = next;
		}
	}
	smap_enable();
	return (long)copied;
}

static long fat32_write_impl(vfs_file_t *f, const void *buf, long bytes)
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
	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;

	if (ff->start_cluster < 2) {
		unsigned long newc = 0;
		if (fat32_alloc_cluster(ff->fs, &newc) != ST_OK)
			return ST_IO;
		ff->start_cluster = newc;
		ff->current_cluster = newc;
	}

	while (remaining) {
		if (ff->current_cluster < 2) {
			/* Sentinel from a previous boundary-aligned exit (see the
             * advance block below).  Walk the chain to the cluster that
             * holds ff->pos, allocating a new cluster if pos is exactly
             * at the end of the current chain. */
			unsigned long target_idx = ff->pos / cluster_size;
			unsigned long cluster = ff->start_cluster;
			for (unsigned long i = 0; i < target_idx; i++) {
				unsigned long next = fat32_next_cluster_cached(
					ff->fs, cluster);
				if (next >= 0x0FFFFFF8 || next == 0) {
					unsigned long newc = 0;
					if (fat32_append_cluster(
						    ff->fs, cluster, &newc) !=
					    ST_OK)
						return written ? (long)written :
								 ST_IO;
					next = newc;
				}
				cluster = next;
			}
			ff->current_cluster = cluster;
		}
		/* BUG GUARD: writing file data to root cluster would corrupt root dir */
		if (ff->current_cluster == g_root_dir_cluster) {
			kprintf("FAT32: BUG: write_impl current_cluster==root(%lu)! "
				"start=%lu dirent_clust=%lu dirent_idx=%u name=%s\n",
				g_root_dir_cluster, ff->start_cluster,
				ff->dirent_cluster, ff->dirent_index,
				ff->name83);
			return written ? (long)written : ST_IO;
		}
		unsigned cluster_offset = ff->pos % cluster_size;
		unsigned avail_in_cluster = cluster_size - cluster_offset;
		unsigned chunk = (remaining < avail_in_cluster) ?
					 (unsigned)remaining :
					 avail_in_cluster;

		// Try to update the page cache: look up cached page for this position
		unsigned long page_idx = ff->pos / PAGE_SIZE;
		unsigned page_offset = ff->pos % PAGE_SIZE;

		pc_page_t *pg = pagecache_lookup(ff->start_cluster, page_idx);
		if (pg) {
			/* Cached page hit — update in place.  Stay within the page so
             * pagecache_mark_dirty's per-page semantics are preserved. */
			unsigned avail_in_page = PAGE_SIZE - page_offset;
			if (chunk > avail_in_page)
				chunk = avail_in_page;
			smap_disable();
			mm_memcpy(pg->data + page_offset,
				  ((const uint8_t *)buf) + written, chunk);
			smap_enable();
			pagecache_mark_dirty(pg);
		} else {
			/* No cache hit on the first page.  Extend `chunk` through the
             * run of consecutive UN-cached pages within this cluster so we
             * can write up to the full cluster in ONE USB MSC transaction
             * instead of one transaction per 4 KiB page.  Reads coalesce
             * up to 64 KB via pc_coalesced_read and run at ~20 MB/s; the
             * write path was capped at PAGE_SIZE per transaction and ran
             * at ~500 KB/s — ~40× the read latency for the same hardware
             * because of CBW/CSW round-trip overhead per transaction.
             *
             * If a LATER page in `chunk` IS cached, we MUST trim before
             * it: writing past a cached page would leave a stale cache
             * entry with the OLD value (subsequent reads return stale
             * data).  Bypass the trim entirely if no later pages are
             * cached (the common case for streaming writes — curl/dd
             * never populate the cache via writes). */
			unsigned long end_byte = ff->pos + chunk; // exclusive
			unsigned long last_page_idx =
				(end_byte - 1) / PAGE_SIZE;
			for (unsigned long p = page_idx + 1; p <= last_page_idx;
			     p++) {
				if (pagecache_lookup(ff->start_cluster, p)) {
					/* Truncate at the start byte of this cached page. */
					chunk = (unsigned)(p * PAGE_SIZE -
							   ff->pos);
					break;
				}
			}

			/* If `chunk` fully fills the current cluster AND there's more
             * data to write, try to extend the chunk through CONSECUTIVE
             * physically-contiguous next clusters.  Reads coalesce
             * up to PC_COALESCE_MAX pages (~64 KB) per USB transaction via
             * pc_coalesced_read — without similar cross-cluster batching
             * here, writes do one BOT per cluster (32 KB → 8 MB/s vs
             * reads' 22 MB/s at 64 KB BOTs for the same hardware). */
			unsigned long run_last_cluster = ff->current_cluster;
			if (chunk == avail_in_cluster && remaining > chunk) {
				unsigned long walk = ff->current_cluster;
				unsigned long expected_next_lba =
					cluster_to_lba(ff->fs, walk) +
					ff->fs->sectors_per_cluster;
				/* Cap at one MAX_SECTORS_PER_READ batch (64 KB).
                 * write_sectors splits at that boundary anyway, but the
                 * kalloc/memcpy/mm_allocate_contiguous_pages chain runs
                 * once per call to write_sectors — bigger calls amortise
                 * those overheads. */
				unsigned max_extend_bytes =
					128u * ff->fs->bytes_per_sector;
				while (chunk + cluster_size <=
					       max_extend_bytes &&
				       remaining > chunk) {
					unsigned long next =
						fat32_next_cluster_cached(
							ff->fs, walk);
					if (next == 0 || next >= 0x0FFFFFF8) {
						unsigned long newc = 0;
						if (fat32_append_cluster(
							    ff->fs, walk,
							    &newc) != ST_OK)
							break;
						next = newc;
					}
					unsigned long next_lba =
						cluster_to_lba(ff->fs, next);
					if (next_lba != expected_next_lba)
						break; // non-contiguous — stop run
					/* Don't span over cached pages — would leave a stale
                     * cache entry with the OLD data. */
					unsigned long start_p =
						(ff->pos + chunk) / PAGE_SIZE;
					unsigned long end_p =
						(ff->pos + chunk +
						 cluster_size - 1) /
						PAGE_SIZE;
					int cached_in_next = 0;
					for (unsigned long p = start_p;
					     p <= end_p; p++) {
						if (pagecache_lookup(
							    ff->start_cluster,
							    p)) {
							cached_in_next = 1;
							break;
						}
					}
					if (cached_in_next)
						break;
					unsigned add =
						(remaining - chunk <
						 cluster_size) ?
							(unsigned)(remaining -
								   chunk) :
							cluster_size;
					chunk += add;
					walk = next;
					run_last_cluster = walk;
					expected_next_lba +=
						ff->fs->sectors_per_cluster;
					if (add < cluster_size)
						break; // partial last cluster
				}
			}

			/* Page not cached — write to disk.  Touch ONLY the sectors that
             * this chunk lands in, not the entire cluster.  Previous code did
             * a full-cluster read-modify-write on every 4 KiB user write,
             * which for a 32 KiB cluster amplified 100 MiB of curl writes
             * into >1.6 GiB of USB traffic (8 KB read + 32 KB write per 4 KB
             * user write).  By writing only the affected sectors, an aligned
             * 4 KiB user write becomes a single 4 KiB sector-batch write
             * with no preceding read — a ~16× reduction in USB transfers
             * for streaming downloads. */
			unsigned sector_size = ff->fs->bytes_per_sector;
			unsigned first_sector = cluster_offset / sector_size;
			unsigned last_byte = cluster_offset + chunk;
			unsigned last_sector =
				(last_byte + sector_size - 1) / sector_size;
			unsigned num_sectors = last_sector - first_sector;
			unsigned head_off = cluster_offset % sector_size;
			int head_partial = (head_off != 0);
			int tail_partial = (last_byte % sector_size) != 0;
			/* Compute, per-side, whether the partial bytes lie WITHIN the
             * current file (and therefore must be preserved with a read)
             * or PAST current EOF (and may be safely zero-filled).  The
             * previous broad `appending = pos+chunk>size` check was wrong
             * for partial-sector writes where the head bytes [sector_start,
             * cluster_offset) live BELOW pos and were already written by
             * an earlier write — zero-filling them silently overwrote that
             * data (caught by the writev/readv testcase: write 1 places
             * "Hello, " at [0..7); write 2 at offset 7 saw head_partial
             * true and zeroed bytes [0..7), trashing "Hello, "). */
			unsigned long file_offset_first =
				(unsigned long)(ff->pos - cluster_offset) +
				(unsigned long)first_sector * sector_size;
			unsigned long file_offset_last_sec =
				(unsigned long)(ff->pos - cluster_offset) +
				(unsigned long)(last_sector - 1) * sector_size;
			int head_in_file =
				head_partial && file_offset_first < ff->size;
			int tail_in_file =
				tail_partial && file_offset_last_sec < ff->size;
			unsigned long base_lba =
				cluster_to_lba(ff->fs, ff->current_cluster) +
				first_sector;
			unsigned tmp_bytes = num_sectors * sector_size;

			void *tmp = kalloc(tmp_bytes);
			if (!tmp) {
				return written ? (long)written : ST_NOMEM;
			}

			if (head_in_file || tail_in_file) {
				/* At least one partial-sector edge lies within existing
                 * file data — read the sector range to preserve it. */
				if (read_sectors(ff->fs->bdev, base_lba,
						 num_sectors, tmp) != ST_OK) {
					kfree(tmp);
					return written ? (long)written : ST_IO;
				}
			} else if (head_partial || tail_partial) {
				/* Partial sectors but the gaps are past EOF — zero so the
                 * on-disk content is well-defined (visible if the file
                 * later grows). */
				mm_memset(tmp, 0, tmp_bytes);
			}
			/* Fully-aligned case (no partial sectors): the upcoming memcpy
             * fills every byte of tmp; no read or memset needed. */

			smap_disable();
			mm_memcpy(((uint8_t *)tmp) + head_off,
				  ((const uint8_t *)buf) + written, chunk);
			smap_enable();

			if (write_sectors(ff->fs->bdev, base_lba, num_sectors,
					  tmp) != ST_OK) {
				kfree(tmp);
				return written ? (long)written : ST_IO;
			}
			kfree(tmp);

			/* If we batched across multiple contiguous clusters, advance
             * ff->current_cluster to the LAST cluster of the run so the
             * boundary-cross advance below correctly hops to the cluster
             * AFTER the run (rather than to the cluster after the first
             * one). */
			if (run_last_cluster != ff->current_cluster)
				ff->current_cluster = run_last_cluster;
		}

		ff->pos += chunk;
		written += chunk;
		remaining -= chunk;

		if (ff->pos % cluster_size == 0) {
			/* Always advance current_cluster when we cross a cluster boundary,
             * not only when there is more data in this call.  Otherwise
             * the next write call enters with ff->pos at the boundary but
             * ff->current_cluster still pointing at the just-filled cluster
             * — and the next write at cluster_offset 0 silently overwrites
             * it.  TLS over FAT32 hit this constantly because TLS records
             * (16 KiB) are an integer divisor of the typical USB cluster
             * size (32 KiB), so HTTPS writes regularly land exactly on a
             * cluster boundary. */
			unsigned long next = fat32_next_cluster_cached(
				ff->fs, ff->current_cluster);
			if (next >= 0x0FFFFFF8 || next == 0) {
				if (remaining > 0) {
					unsigned long newc = 0;
					if (fat32_append_cluster(
						    ff->fs, ff->current_cluster,
						    &newc) != ST_OK)
						break;
					ff->current_cluster = newc;
				} else {
					/* Boundary hit at the very end of this write call.
                     * Don't append a phantom trailing cluster — instead
                     * mark current_cluster stale so the next write
                     * re-derives it from ff->pos (extending the chain
                     * only when there is actual data to write). */
					ff->current_cluster = 0;
				}
			} else {
				ff->current_cluster = next;
			}
		}
	}

	if (ff->pos > ff->size) {
		ff->size = ff->pos;
		/* Defer the on-disk dirent update to fat32_close().  Calling
         * fat32_update_dirent() here would do a full read-modify-write
         * of the parent directory cluster (~32 KB read + 32 KB write)
         * on EVERY write() syscall — for a 100 MB streaming download
         * with 64 KB user-write chunks that's ~3 GB of metadata
         * traffic, dwarfing the actual data and capping write
         * throughput at ~500 KB/s while the read path runs at ~20 MB/s.
         * The icache (when attached) and the dirent_dirty flag below
         * carry the new size in memory; close-time flush persists it. */
		ff->dirent_dirty = 1;
		// Update inode cache with new size
		if (ff->inode)
			icache_update_size((ic_inode_t *)ff->inode, ff->size);
	}

	return (long)written;
}

static unsigned fat32_write_dirent64(char *out, unsigned out_size,
				     unsigned *out_off, const char *name,
				     uint64_t ino, uint8_t type)
{
	if (!out || !out_off || !name) {
		return 0;
	}
	unsigned name_len = 0;
	while (name[name_len] && name_len < 255) {
		name_len++;
	}
	unsigned reclen =
		(unsigned)sizeof(struct dirent64) + name_len + 1;
	reclen = (reclen + 7u) & ~7u;
	if (*out_off + reclen > out_size) {
		return 0;
	}
	// SMAP-aware write to user buffer
	smap_disable();
	struct dirent64 *d = (struct dirent64 *)(out + *out_off);
	d->d_ino = ino;
	d->d_off = 0;
	d->d_reclen = (uint16_t)reclen;
	d->d_type = type;
	char *dn = (char *)d->d_name;
	for (unsigned i = 0; i < name_len; ++i) {
		dn[i] = name[i];
	}
	dn[name_len] = '\0';
	smap_enable();
	*out_off += reclen;
	return 1;
}

static long fat32_readdir_impl(vfs_file_t *f, void *buf, long bytes)
{
	if (!f || !buf || bytes <= 0) {
		return ST_INVALID;
	}
	fat32_file_t *ff = (fat32_file_t *)f->fs_private;
	if (!ff || !ff->fs || !ff->is_dir) {
		return -ENOTDIR;
	}
	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
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

	// Track start of current LFN sequence so we can rewind on buffer-full.
	// If the output buffer is exhausted at a short-name entry whose preceding
	// LFN entries were already consumed in THIS call, the next call would lose
	// the local LFN state and fall back to the 8.3 name.  By saving the
	// position of the first LFN entry (the one with FAT32_LFN_LAST_ENTRY set)
	// we can rewind there instead, so the next call re-processes the whole
	// LFN sequence.
	int have_lfn_start = 0;
	unsigned long lfn_start_cluster = cluster;
	unsigned int lfn_start_idx = idx;

	while (out_off + sizeof(struct dirent64) + 2 < (unsigned)bytes) {
		if (cluster == 0 || cluster >= 0x0FFFFFF8) {
			ff->dir_iter_cluster = cluster;
			ff->dir_iter_index = idx;
			break;
		}
		void *tmp = kalloc(cluster_size);
		if (!tmp) {
			return out_off ? (long)out_off : ST_NOMEM;
		}
		unsigned long lba = cluster_to_lba(ff->fs, cluster);
		if (read_sectors(ff->fs->bdev, lba, ff->fs->sectors_per_cluster,
				 tmp) != ST_OK) {
			kfree(tmp);
			return out_off ? (long)out_off : ST_IO;
		}
		fat32_dirent_t *ents = (fat32_dirent_t *)tmp;
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
				have_lfn_start = 0;
				continue;
			}

			// Handle LFN entry
			if ((ents[idx].attr & FAT32_ATTR_LONG_NAME_MASK) ==
			    FAT32_ATTR_LONG_NAME) {
				fat32_lfn_t *l = (fat32_lfn_t *)&ents[idx];
				int ord = (l->seq & FAT32_LFN_SEQ_MASK);
				int is_last =
					(l->seq & FAT32_LFN_LAST_ENTRY) ? 1 : 0;

				if (ord <= 0 || ord > 20) {
					lfn_buf[0] = '\0';
					lfn_len = 0;
					lfn_seq_expected = 0;
					have_lfn_start = 0;
					continue;
				}

				if (is_last) {
					lfn_len = 0;
					for (int j = 0; j < FAT32_LFN_MAX; j++)
						lfn_tmp[j] = 0;
					lfn_seq_expected = ord;
					lfn_checksum = l->checksum;
					// Remember where this LFN sequence starts so we can
					// rewind here if the output buffer fills up before the
					// corresponding short-name entry is written.
					lfn_start_cluster = cluster;
					lfn_start_idx = idx;
					have_lfn_start = 1;
				}

				if (ord != lfn_seq_expected ||
				    l->checksum != lfn_checksum) {
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

					for (int j = 0;
					     j < 13 &&
					     (pos + j) < FAT32_LFN_MAX - 1;
					     j++) {
						uint16_t c = parts[j];
						if (c == 0x0000 ||
						    c == 0xFFFF) {
							lfn_tmp[pos + j] = 0;
							break;
						}
						lfn_tmp[pos + j] = c;
						if ((pos + j + 1) > lfn_len)
							lfn_len = pos + j + 1;
					}

					if (lfn_seq_expected == 0) {
						for (int k = 0;
						     k < lfn_len &&
						     k < FAT32_LFN_MAX - 1;
						     ++k)
							lfn_buf[k] =
								(char)(lfn_tmp[k] &
								       0xFF);
						lfn_buf[lfn_len] = '\0';
					}
				}
				continue;
			}

			// This is a short name entry
			// For standalone short entries (no preceding LFN), record the
			// rewind position as this entry itself.
			if (!have_lfn_start) {
				lfn_start_cluster = cluster;
				lfn_start_idx = idx;
			}

			// Verify LFN checksum
			if (lfn_buf[0] != '\0') {
				uint8_t calc_checksum =
					fat32_lfn_checksum(ents[idx].name);
				if (calc_checksum != lfn_checksum) {
					lfn_buf[0] = '\0';
				}
			}

			// Build display name
			char display_name[FAT32_LFN_MAX];
			if (lfn_buf[0] != '\0') {
				// Use LFN (case preserved)
				int k;
				for (k = 0; lfn_buf[k] && k < FAT32_LFN_MAX - 1;
				     k++)
					display_name[k] = lfn_buf[k];
				display_name[k] = '\0';
			} else {
				// Use short name, convert to lowercase for display
				int p = 0;
				for (int j = 0; j < 8; j++) {
					if (ents[idx].name[j] == ' ')
						break;
					display_name[p++] = fat32_tolower(
						ents[idx].name[j]);
				}
				if (ents[idx].name[8] != ' ') {
					display_name[p++] = '.';
					for (int j = 8; j < 11; j++) {
						if (ents[idx].name[j] == ' ')
							break;
						display_name[p++] =
							fat32_tolower(
								ents[idx].name
									[j]);
					}
				}
				display_name[p] = '\0';
			}

			uint64_t ino = ((uint64_t)ents[idx].fstClusHI << 16) |
				       ents[idx].fstClusLO;
			uint8_t dtype =
				(ents[idx].attr & FAT32_ATTR_DIRECTORY) ?
					4 :
					8; /* DT_DIR=4, DT_REG=8 */
			if (!fat32_write_dirent64((char *)buf, (unsigned)bytes,
						  &out_off, display_name, ino,
						  dtype)) {
				// Buffer full – rewind to the start of the LFN sequence
				// (or this entry itself for standalone short entries) so
				// that the next readdir call re-processes the LFN entries.
				ff->dir_iter_cluster = lfn_start_cluster;
				ff->dir_iter_index = lfn_start_idx;
				kfree(tmp);
				return (long)out_off;
			}

			// Reset LFN state
			lfn_buf[0] = '\0';
			lfn_len = 0;
			lfn_seq_expected = 0;
			have_lfn_start = 0;
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

static int fat32_truncate_impl(vfs_file_t *f, unsigned long new_size)
{
	if (!f)
		return ST_INVALID;
	fat32_file_t *ff = (fat32_file_t *)f->fs_private;
	if (!ff || !ff->fs)
		return ST_INVALID;

	unsigned cluster_size =
		ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;

	if (new_size == ff->size) {
		return ST_OK;
	}

	if (new_size == 0) {
		// Invalidate all cached pages and cluster chain for this file
		pagecache_invalidate_file(ff->start_cluster);
		icache_chain_invalidate(ff->start_cluster);
		if (ff->start_cluster >= 2) {
			fat32_free_chain(ff->fs, ff->start_cluster);
		}
		ff->start_cluster = 0;
		ff->current_cluster = 0;
		ff->size = 0;
		if (ff->pos > 0)
			ff->pos = 0;
		return fat32_update_dirent(ff);
	}

	unsigned long needed_clusters =
		(new_size + cluster_size - 1) / cluster_size;

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
		unsigned long next =
			fat32_next_cluster_cached(ff->fs, last_cluster);
		if (next >= 0x0FFFFFF8 || next == 0) {
			unsigned long newc = 0;
			if (fat32_append_cluster(ff->fs, last_cluster, &newc) !=
			    ST_OK)
				return ST_IO;
			last_cluster = newc;
		} else {
			last_cluster = next;
		}
		index++;
	}

	// Trim extra clusters if shrinking
	if (new_size < ff->size) {
		// Invalidate cached pages beyond new size and reset cluster chain cache
		pagecache_invalidate_range(ff->start_cluster, new_size);
		icache_chain_invalidate(ff->start_cluster);
		unsigned long cut_cluster = 0;
		if (fat32_get_cluster_at(ff->fs, ff->start_cluster,
					 needed_clusters - 1,
					 &cut_cluster) == ST_OK) {
			unsigned long next =
				fat32_next_cluster_cached(ff->fs, cut_cluster);
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
static long fat32_seek_impl(vfs_file_t *f, long offset, int whence)
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
	if (!ff->is_dir && (unsigned long)new_pos > ff->size)
		new_pos = (long)ff->size;

	// For directories, reset the readdir iterator state so that
	// rewinddir() (lseek to 0) causes the next getdents to start over.
	if (ff->is_dir) {
		ff->dir_iter_cluster = ff->start_cluster;
		ff->dir_iter_index = 0;
		ff->pos = 0;
		return 0;
	}

	// Update position and recalculate current cluster
	unsigned long target_pos = (unsigned long)new_pos;
	if (fat32_set_position(ff, target_pos) != ST_OK)
		return -1;
	return (long)ff->pos;
}

static int fat32_close_impl(vfs_file_t *f)
{
	if (!f)
		return ST_INVALID;
	fat32_file_t *ff = (fat32_file_t *)f->fs_private;
	if (ff) {
		// Flush any dirty cached pages for this file before closing
		if (ff->start_cluster >= 2)
			pagecache_flush_file(ff->start_cluster);
		/* Persist any FAT entries that were deferred during the writes
         * to this file.  Without this, a power loss right after close
         * would lose the chain (data clusters are on disk but the FAT
         * still says they are free). */
		fat32_fat_flush_pending();
		// Release inode cache reference
		if (ff->inode) {
			ic_inode_t *ic = (ic_inode_t *)ff->inode;
			/* If write() bumped ff->size, propagate the new value into
             * the icache entry so icache_flush below persists the
             * updated dirent.  (For files opened anew with no existing
             * cluster, ff->inode was NULL during the writes, so
             * icache_update_size in write_impl was a no-op — pick it
             * up here.) */
			if (ff->dirent_dirty) {
				icache_update_size(ic, ff->size);
				ff->dirent_dirty = 0;
			}
			if (ic->flags & IC_DIRTY)
				icache_flush(ic);
			icache_unref(ic);
		} else if (ff->dirent_dirty && ff->start_cluster >= 2) {
			/* No icache entry was ever attached (newly created file:
             * fat32_open ran when fc == 0 and skipped icache_get).
             * Persist the size+start_cluster directly. */
			fat32_update_dirent(ff);
			ff->dirent_dirty = 0;
		}
		kfree(ff);
	}
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
	task_t *cur = sched_current();
	if (!cur || cur->cwd[0] == '\0' || kstrcmp(cur->cwd, "/") == 0) {
		return g_root_dir_cluster;
	}

	// Resolve the task's cwd path to get cluster
	const char *path = cur->cwd;
	if (path[0] == '/')
		path++; // Skip leading /

	unsigned attr = 0;
	unsigned long fc = 0;
	unsigned long size = 0;

	if (path[0] == '\0') {
		return g_root_dir_cluster; // Root directory
	}

	int res =
		fat32_resolve_path(g_root_dir_cluster, path, &attr, &fc, &size);
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
	fat32_sb_attach(fs);
	return vfs_register_root(&fat32_vfs_ops);
}

void fat32_list_root(void (*cb)(const char *name, unsigned attr,
				unsigned long size))
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
	unsigned entries =
		(g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector) /
		sizeof(fat32_dirent_t);
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
		if ((ents[i].attr & FAT32_ATTR_LONG_NAME_MASK) ==
		    FAT32_ATTR_LONG_NAME) {
			kprintf("  [%u] LFN seq=%02x\n", i,
				((fat32_lfn_t *)&ents[i])->seq);
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
		kprintf("  [%u] %s attr=%02x size=%lu\n", i, temp, ents[i].attr,
			ents[i].fileSize);
	}
}

/* ========================================================================
 * fat32_get_statfs — fill filesystem statistics
 * ======================================================================== */
int fat32_get_statfs(fat32_statfs_t *info)
{
	if (!info || !g_root_fs)
		return -1;

	unsigned long bsize = (unsigned long)g_root_fs->sectors_per_cluster *
			      (unsigned long)g_root_fs->bytes_per_sector;

	/* Calculate total FAT entries if not yet known */
	if (g_fat_total_entries == 0) {
		if (fat32_ensure_fat_loaded(g_root_fs) != ST_OK)
			return -1;
	}
	unsigned long total =
		(g_fat_total_entries > 2) ? g_fat_total_entries - 2 : 0;

	/* Use cached free cluster count (from FSInfo or previous scan).
     * Only do a full FAT scan if FSInfo was unavailable or invalid. */
	unsigned long free_count = g_free_cluster_count;
	if (free_count == (unsigned long)-1) {
		/* One-time scan — result is cached for all future calls */
		if (fat32_ensure_fat_loaded(g_root_fs) != ST_OK)
			return -1;
		free_count = 0;
		for (unsigned long c = 2; c < g_fat_total_entries; ++c) {
			unsigned long v =
				fat32_next_cluster_cached(g_root_fs, c);
			if (v == 0)
				free_count++;
		}
		g_free_cluster_count = free_count;
	}

	info->f_bsize = bsize;
	info->f_frsize = bsize;
	info->f_blocks = total;
	info->f_bfree = free_count;
	info->f_bavail = free_count;
	info->f_files = 0; /* FAT32 has no fixed inode table */
	info->f_ffree = 0;
	info->f_fsid = 0;
	info->f_namelen = 255; /* LFN maximum */
	info->f_type = 0x4d44; /* MSDOS_SUPER_MAGIC */
	return 0;
}
