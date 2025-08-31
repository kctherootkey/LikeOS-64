// LikeOS-64 - FAT32 read-only minimal implementation (root dir, 8.3 files)
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/block.h"

// BPB offsets (FAT32)
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector; // 11
    uint8_t  sectors_per_cluster; // 13
    uint16_t reserved_sector_count; // 14
    uint8_t  num_fats; // 16
    uint16_t root_entry_count; // 17 (FAT12/16)
    uint16_t total_sectors16; // 19
    uint8_t  media; // 21
    uint16_t fat_size16; // 22
    uint16_t sectors_per_track; // 24
    uint16_t num_heads; // 26
    uint32_t hidden_sectors; // 28
    uint32_t total_sectors32; // 32
    uint32_t fat_size32; // 36
    uint16_t ext_flags; // 40
    uint16_t fs_version; // 42
    uint32_t root_cluster; // 44
    uint16_t fs_info; // 48
    uint16_t bk_boot_sector; // 50
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
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
    uint8_t seq;        // sequence number (bit 6 = last long entry)
    uint16_t name1[5];  // characters 1-5
    uint8_t attr;       // 0x0F
    uint8_t type;       // 0
    uint8_t checksum;   // checksum of DOS name
    uint16_t name2[6];  // characters 6-11
    uint16_t fstClusLO; // always 0
    uint16_t name3[2];  // characters 12-13
} fat32_lfn_t;

#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_LONG_NAME 0x0F

static int read_sectors(const block_device_t* bdev, unsigned long lba, unsigned long count, void* buf){
    return bdev->read((block_device_t*)bdev, lba, count, buf);
}

static unsigned long cluster_to_lba(fat32_fs_t* fs, unsigned long cluster){
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

// Simple root directory cache (single cluster only for now)
static void* g_root_dir_cache = 0; // first cluster of root directory cached
static unsigned long g_root_dir_cluster = 0;
static void* g_fat_cache = 0; // cached first FAT
static unsigned long g_fat_cache_entries = 0;

static unsigned long fat32_next_cluster_cached(fat32_fs_t* fs, unsigned long cluster){
    if(!g_fat_cache){
        // Derive FAT size (sectors) using difference to data_start and number of FATs (assume 2) approximated
        unsigned long fat_total = fs->data_start_lba - fs->fat_start_lba; // includes all FAT copies
        if(fat_total == 0) return 0x0FFFFFFF;
        unsigned long first_fat_sectors = fat_total / 2; if(first_fat_sectors==0) first_fat_sectors = fat_total; // if only one FAT
        g_fat_cache = kalloc(first_fat_sectors * fs->bytes_per_sector);
        if(g_fat_cache){
            read_sectors(fs->bdev, fs->fat_start_lba, first_fat_sectors, g_fat_cache);
            g_fat_cache_entries = (first_fat_sectors * fs->bytes_per_sector)/4;
        }
    }
    if(!g_fat_cache) return 0x0FFFFFFF;
    if(cluster >= g_fat_cache_entries) return 0x0FFFFFFF;
    uint32_t val = ((uint32_t*)g_fat_cache)[cluster] & 0x0FFFFFFF;
    return val;
}

static fat32_fs_t* g_root_fs = 0;
static fat32_fs_t g_static_fs; // internal singleton instance
static int str_eq(const char* a, const char* b){ while(*a && *b){ if(*a!=*b) return 0; ++a; ++b; } return *a==0 && *b==0; }
static unsigned long g_cwd_cluster = 0; // 0 means root
// For now we don't maintain full parent map; parent of any dir except root collapses to root.

int fat32_mount(const block_device_t* bdev, fat32_fs_t* out){
    if(!bdev || !out) return ST_INVALID;
    void* sector = kalloc(512);
    if(!sector) return ST_NOMEM;
    if(read_sectors(bdev, 0, 1, sector) != ST_OK){ kfree(sector); return ST_IO; }
    fat32_bpb_t* bpb = (fat32_bpb_t*)sector;
    if(bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0){ kfree(sector); return ST_ERR; }
    unsigned long total_sectors = bpb->total_sectors32 ? bpb->total_sectors32 : bpb->total_sectors16;
    unsigned long fat_sz = bpb->fat_size32 ? bpb->fat_size32 : bpb->fat_size16;
    unsigned long first_data_sector = bpb->reserved_sector_count + (bpb->num_fats * fat_sz);
    out->bdev = bdev;
    out->bytes_per_sector = bpb->bytes_per_sector;
    out->sectors_per_cluster = bpb->sectors_per_cluster;
    out->fat_start_lba = bpb->reserved_sector_count;
    out->data_start_lba = first_data_sector;
    out->root_cluster = bpb->root_cluster;
    g_root_dir_cluster = out->root_cluster;
    g_root_fs = out; // register internal pointer
    // Load first root cluster
    if(!g_root_dir_cache) g_root_dir_cache = kalloc(out->sectors_per_cluster * out->bytes_per_sector);
    if(g_root_dir_cache){
        unsigned long root_lba = cluster_to_lba(out, out->root_cluster);
        unsigned long read_count = out->sectors_per_cluster;
        if(read_count==0) read_count=1; // safety
        read_sectors(bdev, root_lba, read_count, g_root_dir_cache);
    }
    kprintf("FAT32: mount %s bytes/sector=%u spc=%u root_cluster=%lu data_lba=%lu\n", bdev->name, out->bytes_per_sector, out->sectors_per_cluster, out->root_cluster, out->data_start_lba);
    kfree(sector);
    return ST_OK;
}

// VFS bridge
static int fat32_match_name(const uint8_t* raw, const char* name){
    // Convert raw 11-byte 8.3 to temp
    char temp[13]; int p=0; for(int i=0;i<8;i++){ if(raw[i]==' ') break; temp[p++]=raw[i]; }
    int has_ext = (raw[8] != ' ');
    if(has_ext){ temp[p++]='.'; for(int i=8;i<11;i++){ if(raw[i]==' ') break; temp[p++]=raw[i]; } }
    temp[p]='\0';
    // Uppercase compare
    for(int i=0; temp[i]; ++i){ if(temp[i]>='a'&&temp[i]<='z') temp[i]-=32; }
    char canon[32]; int ci=0; for(; name[ci] && ci<31; ++ci){ canon[ci]= (name[ci]>='a'&&name[ci]<='z')? (name[ci]-32):name[ci]; } canon[ci]='\0';
    // Skip leading '/'
    const char* cmp = canon; if(canon[0]=='/' && canon[1]) cmp = canon+1; else if(canon[0]=='/' && !canon[1]) return ST_INVALID;
    // Only root-level support now
    if(!g_root_dir_cache) return ST_NOT_FOUND;
        if(str_eq(cmp, temp)) return 1; // quick path
        return str_eq(cmp,temp);
}

static int fat32_dir_list_internal(unsigned long start_cluster, void (*cb)(const char*, unsigned, unsigned long)){
    if(!g_root_fs || !cb) return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    while(cluster < 0x0FFFFFF8){
        void* buf = kalloc(cluster_size);
        if(!buf) return ST_NOMEM;
        if(read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster), g_root_fs->sectors_per_cluster, buf)!=ST_OK){ kfree(buf); return ST_IO; }
        fat32_dirent_t* ents = (fat32_dirent_t*)buf;
        unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        char lfn_buf[260]; lfn_buf[0]='\0';
        for(unsigned i=0;i<entries;i++){
            if(ents[i].name[0]==0x00) { kfree(buf); return ST_OK; }
            if(ents[i].name[0]==0xE5) continue;
            if(ents[i].attr == FAT32_ATTR_LONG_NAME){
                // accumulate LFN entries
                fat32_lfn_t* l = (fat32_lfn_t*)&ents[i];
                int ord = (l->seq & 0x1F); // sequence number (1-based)
                int last = (l->seq & 0x40)?1:0;
                static uint16_t lfn_tmp[260];
                static int lfn_len = 0;
                if(last){ lfn_len = 0; for(int j=0;j<260;j++) lfn_tmp[j]=0; }
                int pos = (ord-1)*13;
                if(pos < 260){
                    uint16_t* dst = lfn_tmp+pos;
                    uint16_t parts[13];
                    for(int j=0;j<5;j++) parts[j]=l->name1[j];
                    for(int j=0;j<6;j++) parts[5+j]=l->name2[j];
                    for(int j=0;j<2;j++) parts[11+j]=l->name3[j];
                    for(int j=0;j<13 && (pos+j)<259; j++){
                        uint16_t c = parts[j];
                        if(c==0x0000 || c==0xFFFF){ lfn_tmp[pos+j]=0; break; }
                        lfn_tmp[pos+j]= (c & 0xFF);
                        if((pos+j+1) > lfn_len) lfn_len = pos+j+1;
                    }
                    if(last){
                        // finalize to lfn_buf
                        int k=0; for(; k<lfn_len && k<259; ++k){ lfn_buf[k]=(char)lfn_tmp[k]; }
                        lfn_buf[k]='\0';
                    }
                }
                continue;
            }
            char temp[13]; int p=0; for(int j=0;j<8;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; }
            if(ents[i].name[8] != ' '){ temp[p++]='.'; for(int j=8;j<11;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; } }
            temp[p]='\0';
            const char* name_out = (lfn_buf[0])? lfn_buf : temp;
            cb(name_out, ents[i].attr, ents[i].fileSize);
            lfn_buf[0]='\0';
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if(next >= 0x0FFFFFF8 || next == 0) break;
        cluster = next;
    }
    return ST_OK;
}

int fat32_dir_list(unsigned long cluster, void (*cb)(const char*, unsigned, unsigned long)){ return fat32_dir_list_internal(cluster, cb); }

int fat32_dir_find(unsigned long start_cluster, const char* name, unsigned* attr, unsigned long* first_cluster, unsigned long* size){
    if(!g_root_fs || !name) return ST_INVALID;
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    unsigned long cluster = start_cluster;
    char canon[32]; int ci=0; for(; name[ci] && ci<31; ++ci){ char c=name[ci]; if(c>='A'&&c<='Z') c+=32; canon[ci]=c; } canon[ci]='\0';
    while(cluster < 0x0FFFFFF8){
        void* buf = kalloc(cluster_size); if(!buf) return ST_NOMEM;
        if(read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, cluster), g_root_fs->sectors_per_cluster, buf)!=ST_OK){ kfree(buf); return ST_IO; }
        fat32_dirent_t* ents = (fat32_dirent_t*)buf; unsigned entries = cluster_size / sizeof(fat32_dirent_t);
        char lfn_buf[260]; lfn_buf[0]='\0';
        static uint16_t lfn_tmp[260]; static int lfn_len=0;
        for(unsigned i=0;i<entries;i++){
            if(ents[i].name[0]==0x00){ kfree(buf); return ST_NOT_FOUND; }
            if(ents[i].name[0]==0xE5) continue; 
            if(ents[i].attr == FAT32_ATTR_LONG_NAME){
                fat32_lfn_t* l = (fat32_lfn_t*)&ents[i];
                int ord = (l->seq & 0x1F); int last = (l->seq & 0x40)?1:0;
                if(last){ lfn_len=0; for(int j=0;j<260;j++) lfn_tmp[j]=0; }
                int pos = (ord-1)*13;
                if(pos<260){
                    uint16_t parts[13];
                    for(int j=0;j<5;j++) parts[j]=l->name1[j];
                    for(int j=0;j<6;j++) parts[5+j]=l->name2[j];
                    for(int j=0;j<2;j++) parts[11+j]=l->name3[j];
                    for(int j=0;j<13 && (pos+j)<259; j++){
                        uint16_t c = parts[j];
                        if(c==0x0000 || c==0xFFFF){ lfn_tmp[pos+j]=0; break; }
                        lfn_tmp[pos+j] = (c & 0xFF);
                        if((pos+j+1) > lfn_len) lfn_len = pos+j+1;
                    }
                    if(last){ int k=0; for(; k<lfn_len && k<259; ++k) lfn_buf[k]=(char)lfn_tmp[k]; lfn_buf[k]='\0'; }
                }
                continue;
            }
            char temp[13]; int p=0; for(int j=0;j<8;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; }
            if(ents[i].name[8] != ' '){ temp[p++]='.'; for(int j=8;j<11;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; } }
            temp[p]='\0'; for(int j=0; temp[j]; ++j){ if(temp[j]>='A'&&temp[j]<='Z') temp[j]+=32; }
            const char* cmpname = (lfn_buf[0])? lfn_buf : temp;
            // lowercase lfn temp compare
            char lname[260]; int li=0; for(; cmpname[li] && li<259; ++li){ char c=cmpname[li]; if(c>='A'&&c<='Z') c+=32; lname[li]=c; } lname[li]='\0';
            if(str_eq(lname, canon)){
                if(attr) *attr = ents[i].attr;
                if(first_cluster) *first_cluster = ((unsigned long)ents[i].fstClusHI<<16)|ents[i].fstClusLO;
                if(size) *size = ents[i].fileSize;
                kfree(buf); return ST_OK;
            }
            lfn_buf[0]='\0';
        }
        kfree(buf);
        unsigned long next = fat32_next_cluster_cached(g_root_fs, cluster);
        if(next >= 0x0FFFFFF8 || next == 0) break; cluster = next;
    }
    return ST_NOT_FOUND;
}

static int fat32_path_resolve(const char* path, unsigned* attr, unsigned long* first_cluster, unsigned long* size){
    if(!g_root_fs || !path) return ST_INVALID;
    // Skip leading '/'
    if(path[0]=='/') path++;
    if(!*path) return ST_INVALID;
    unsigned long current = g_root_dir_cluster;
    const char* seg = path;
    char part[64];
    while(*seg){
        int pi=0; while(*seg && *seg!='/'){ if(pi<63) part[pi++]=*seg; seg++; }
        part[pi]='\0';
        unsigned a; unsigned long fc; unsigned long sz;
        if(fat32_dir_find(current, part, &a, &fc, &sz)!=ST_OK) return ST_NOT_FOUND;
        if(*seg == '/'){
            if(!(a & FAT32_ATTR_DIRECTORY)) return ST_INVALID; // not a dir
            current = fc; seg++; if(!*seg){ // trailing slash
                if(attr) *attr = a; if(first_cluster) *first_cluster = fc; if(size) *size = sz; return ST_OK; }
            continue;
        } else {
            if(attr) *attr = a; if(first_cluster) *first_cluster = fc; if(size) *size = sz; return ST_OK;
        }
    }
    return ST_NOT_FOUND;
}

// Extended resolution supporting relative paths, '.' and '..' ( '..' limited: returns root for now )
int fat32_resolve_path(unsigned long start_cluster, const char* path, unsigned* attr, unsigned long* first_cluster, unsigned long* size){
    if(!g_root_fs || !path) return ST_INVALID;
    unsigned long current = (start_cluster==0)? g_root_dir_cluster : start_cluster;
    // Absolute path
    if(path[0]=='/'){
        current = g_root_dir_cluster;
        path++;
    }
    if(!*path){ if(attr) *attr = FAT32_ATTR_DIRECTORY; if(first_cluster) *first_cluster=current; if(size) *size = 0; return ST_OK; }
    const char* seg = path; char part[64];
    while(*seg){
        int pi=0; while(*seg && *seg!='/'){ if(pi<63) part[pi++]=*seg; seg++; }
        part[pi]='\0';
        if(pi==0){ if(*seg=='/') { seg++; continue; } else break; }
        if(part[0]=='.' && part[1]=='\0'){ /* stay */ }
        else if(part[0]=='.' && part[1]=='.' && part[2]=='\0'){
            current = fat32_parent_cluster(current);
        } else {
            unsigned a; unsigned long fc; unsigned long sz;
            if(fat32_dir_find(current, part, &a, &fc, &sz)!=ST_OK) return ST_NOT_FOUND;
            if(*seg=='/'){
                if(!(a & FAT32_ATTR_DIRECTORY)) return ST_INVALID;
                current = fc; seg++; if(!*seg){ if(attr) *attr=a; if(first_cluster) *first_cluster=fc; if(size) *size=sz; return ST_OK; }
                continue;
            } else {
                if(attr) *attr=a; if(first_cluster) *first_cluster=fc; if(size) *size=sz; return ST_OK;
            }
        }
        if(*seg=='/') seg++;
    }
    if(attr) *attr = FAT32_ATTR_DIRECTORY; if(first_cluster) *first_cluster=current; if(size) *size = 0; return ST_OK;
}

int fat32_stat(unsigned long start_cluster, const char* path, unsigned* attr, unsigned long* first_cluster, unsigned long* size){
    return fat32_resolve_path(start_cluster, path, attr, first_cluster, size);
}

unsigned long fat32_parent_cluster(unsigned long dir_cluster){
    if(dir_cluster == g_root_dir_cluster || dir_cluster==0) return g_root_dir_cluster;
    // Read first cluster of directory to find '..'
    unsigned cluster_size = g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector;
    void* buf = kalloc(cluster_size); if(!buf) return g_root_dir_cluster;
    if(read_sectors(g_root_fs->bdev, cluster_to_lba(g_root_fs, dir_cluster), g_root_fs->sectors_per_cluster, buf)!=ST_OK){ kfree(buf); return g_root_dir_cluster; }
    fat32_dirent_t* ents = (fat32_dirent_t*)buf;
    for(unsigned i=0; i< (cluster_size/sizeof(fat32_dirent_t)); ++i){
        if(ents[i].name[0]==0x00) break;
        if(ents[i].attr==FAT32_ATTR_LONG_NAME) continue;
        if(ents[i].name[0]=='.'){
            // '.' or '..'
            if(ents[i].name[1]=='.'){
                unsigned long parent = ((unsigned long)ents[i].fstClusHI<<16)|ents[i].fstClusLO;
                if(parent==0) parent = g_root_dir_cluster; // root special case
                kfree(buf); return parent;
            }
        }
    }
    kfree(buf);
    return g_root_dir_cluster;
}

// Forward declare ops so open can assign pointer
static int fat32_open(const char* path, vfs_file_t** out);
static long fat32_read(vfs_file_t* f, void* buf, long bytes);
static int fat32_close(vfs_file_t* f);
static const vfs_ops_t fat32_vfs_ops = { fat32_open, fat32_read, fat32_close };

static int fat32_open(const char* path, vfs_file_t** out){
    if(!out) return ST_INVALID;
    unsigned attr; unsigned long fc; unsigned long size;
    if(fat32_resolve_path(fat32_get_cwd(), path, &attr, &fc, &size)!=ST_OK) return ST_NOT_FOUND;
    if(attr & FAT32_ATTR_DIRECTORY) return ST_UNSUPPORTED;
    fat32_file_t* ff = (fat32_file_t*)kalloc(sizeof(fat32_file_t)); if(!ff) return ST_NOMEM;
    ff->fs = g_root_fs; ff->start_cluster = fc; ff->current_cluster = fc; ff->size = size; ff->pos = 0; ff->vfs.ops=&fat32_vfs_ops; ff->vfs.fs_private=ff;
    *out = &ff->vfs;
    kprintf("FAT32: open %s size=%lu cluster=%lu\n", path, ff->size, ff->start_cluster);
    return ST_OK;
}

static long fat32_read(vfs_file_t* f, void* buf, long bytes){
    if(!f || !buf) return ST_INVALID;
    fat32_file_t* ff = (fat32_file_t*)f->fs_private;
    if(!ff) return ST_INVALID;
    if(ff->pos >= ff->size) return 0; // EOF
    if(bytes < 0) return ST_INVALID;
    if((unsigned long)bytes > ff->size - ff->pos) bytes = (long)(ff->size - ff->pos);
    unsigned long remaining = (unsigned long)bytes;
    unsigned long copied = 0;
    unsigned cluster_size = ff->fs->sectors_per_cluster * ff->fs->bytes_per_sector;
    while(remaining){
        unsigned cluster_offset = ff->pos % cluster_size;
        unsigned avail_in_cluster = cluster_size - cluster_offset;
        unsigned chunk = (remaining < avail_in_cluster)? (unsigned)remaining : avail_in_cluster;
        void* tmp = kalloc(cluster_size);
        if(!tmp) return (copied? (long)copied : ST_NOMEM);
        if(read_sectors(ff->fs->bdev, cluster_to_lba(ff->fs, ff->current_cluster), ff->fs->sectors_per_cluster, tmp)!=ST_OK){ kfree(tmp); return (copied? (long)copied : ST_IO); }
        for(unsigned i=0;i<chunk;i++) ((uint8_t*)buf)[copied+i] = ((uint8_t*)tmp)[cluster_offset + i];
        kfree(tmp);
        ff->pos += chunk; copied += chunk; remaining -= chunk;
        if(ff->pos >= ff->size) break;
        if(ff->pos % cluster_size == 0){
            unsigned long next = fat32_next_cluster_cached(ff->fs, ff->current_cluster);
            if(next >= 0x0FFFFFF8 || next == 0) break; // end chain / invalid
            ff->current_cluster = next;
        }
    }
    return (long)copied;
}
static int fat32_close(vfs_file_t* f){ if(!f) return ST_INVALID; fat32_file_t* ff=(fat32_file_t*)f->fs_private; if(ff) kfree(ff); return ST_OK; }

// (ops struct moved earlier)

// Accessor for root cluster
unsigned long fat32_root_cluster(void){ return g_root_dir_cluster; }
void fat32_set_cwd(unsigned long cluster){ g_cwd_cluster = cluster; }
unsigned long fat32_get_cwd(void){ return g_cwd_cluster? g_cwd_cluster : g_root_dir_cluster; }

int fat32_vfs_register_root(fat32_fs_t* fs){ if(!fs) return ST_INVALID; g_root_fs = fs; return vfs_register_root(&fat32_vfs_ops); }

void fat32_list_root(void (*cb)(const char* name, unsigned attr, unsigned long size)){
    if(!g_root_fs || !g_root_dir_cache || !cb) return;
    unsigned entries = (g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector)/sizeof(fat32_dirent_t);
    fat32_dirent_t* ents = (fat32_dirent_t*)g_root_dir_cache;
    // debug removed: root list header
    for(unsigned i=0;i<entries;i++){
        if(ents[i].name[0]==0x00) break;            // end marker
        if(ents[i].name[0]==0xE5) continue;         // deleted
    if(ents[i].attr == FAT32_ATTR_LONG_NAME) { continue; }
    if(ents[i].attr & 0x08) { continue; }
        char temp[13]; int p=0; for(int j=0;j<8;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; }
        if(ents[i].name[8] != ' '){ temp[p++]='.'; for(int j=8;j<11;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; } }
        temp[p]='\0';
        if(p==0) continue; // defensively skip empty
        cb(temp, ents[i].attr, ents[i].fileSize);
    }
}

// Debug helper: dump first few raw directory entries (8.3) for diagnostics
void fat32_debug_dump_root(void){
    if(!g_root_fs || !g_root_dir_cache) { kprintf("FAT32 dbg: no root cache yet\n"); return; }
    unsigned entries = (g_root_fs->sectors_per_cluster * g_root_fs->bytes_per_sector)/sizeof(fat32_dirent_t);
    fat32_dirent_t* ents = (fat32_dirent_t*)g_root_dir_cache;
    kprintf("FAT32 dbg root first entries:\n");
    for(unsigned i=0;i<entries && i<8;i++){
        if(ents[i].name[0]==0x00){ kprintf("  [%u] end marker\n", i); break; }
        if(ents[i].name[0]==0xE5){ kprintf("  [%u] deleted\n", i); continue; }
        if(ents[i].attr == FAT32_ATTR_LONG_NAME){ kprintf("  [%u] LFN seq=%02x\n", i, ((fat32_lfn_t*)&ents[i])->seq); continue; }
        char temp[13]; int p=0; for(int j=0;j<8;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; }
        if(ents[i].name[8] != ' '){ temp[p++]='.'; for(int j=8;j<11;j++){ if(ents[i].name[j]==' ') break; temp[p++]=ents[i].name[j]; } }
        temp[p]='\0';
        kprintf("  [%u] %s attr=%02x size=%lu\n", i, temp, ents[i].attr, ents[i].fileSize);
    }
}
