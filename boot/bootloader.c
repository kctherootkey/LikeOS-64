/*
 * Copyright (C) 2026 The LikeOS Project
 */

#include <efi.h>
#include <efilib.h>

#include "bootcommon.h"

// ELF64 structures
#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

// ELF constants
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EM_X86_64 62
#define PT_LOAD 1

// Kernel entry point type (now takes boot info)
typedef void (*kernel_entry_t)(void* boot_info);

// Trampoline function type
typedef void (*trampoline_func_t)(UINT64 kernel_entry, void* boot_info, UINT64 pml4_addr);

// Framebuffer information structure
typedef struct {
    void* framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
} framebuffer_info_t;

// Build-time ceiling on the preferred-resolution search (make MAX_SCREEN_SIZE=WxH).
// Entries of the preferred table larger than this are never considered, so a
// build for a 1920x1080 panel does not land on 1920x1200 just because the
// firmware -- a virtual machine's in particular -- happens to offer it.  Unset
// means no ceiling; the kernel's SVGA driver applies the same two limits to the
// same table (kernel/dev/video/vmsvga2.c).
#ifndef SCREEN_MAX_WIDTH
#define SCREEN_MAX_WIDTH  0xFFFFFFFFU
#endif
#ifndef SCREEN_MAX_HEIGHT
#define SCREEN_MAX_HEIGHT 0xFFFFFFFFU
#endif

// The standard list of screen sizes, shared with the kernel's display drivers.
#include "../kernel/dev/video/screen_modes.h"

// The display setting the Display settings dialog saves, on the ext4 root.
#define DISPLAY_CONF_PATH "/etc/likeos/display.conf"

// UEFI memory map entry (matching EFI_MEMORY_DESCRIPTOR)
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} memory_map_entry_t;

// Memory map information passed to kernel
#define MAX_MEMORY_MAP_ENTRIES 256
typedef struct {
    uint32_t entry_count;
    uint32_t descriptor_size;
    uint64_t total_memory;
    memory_map_entry_t entries[MAX_MEMORY_MAP_ENTRIES];
} memory_map_info_t;

// The firmware's display modes passed on to the kernel (32 bpp, distinct sizes)
#define BOOT_GOP_MODES_MAX 64

// The firmware's EFI_MEMORY_RUNTIME regions passed on to the kernel
#define BOOT_EFI_RT_MAX 128

// Boot information passed to kernel
typedef struct {
    framebuffer_info_t fb_info;
    memory_map_info_t mem_info;
    uint64_t rsdp_address;        // ACPI RSDP physical address from UEFI
    uint64_t smp_trampoline_addr; // Reserved SMP AP trampoline address (4KB aligned, < 1MB)
    uint64_t boot_epoch;          // Unix epoch seconds at boot (from UEFI GetTime before ExitBootServices)
    uint64_t direct_map_bytes;    // Bytes of physical RAM covered by the PML4[272] direct map
    uint64_t smbios_magic;        // BOOT_SMBIOS_MAGIC when the two fields below are valid
    uint64_t smbios_address;      // SMBIOS 2.x entry point ("_SM_") from UEFI, 0 = none
    uint64_t smbios3_address;     // SMBIOS 3.x entry point ("_SM3_") from UEFI, 0 = none
    uint64_t display_magic;       // BOOT_DISPLAY_MAGIC when the fields below are valid
    uint32_t display_saved_width; // the saved display setting, 0 = none
    uint32_t display_saved_height;
    uint32_t hypervisor;          // 1 when CPUID reports a hypervisor
    uint32_t gop_mode_count;      // entries used in gop_modes[]
    struct { uint32_t width; uint32_t height; } gop_modes[BOOT_GOP_MODES_MAX];

    // ---- Root identity (valid when rootid_magic == BOOT_ROOTID_MAGIC) ----
    // The kernel's copy is kernel/mm/memory.h; keep both identical.  See
    // "Boot identity" below for how each field is obtained.
    uint64_t rootid_magic;          // BOOT_ROOTID_MAGIC when the fields below are valid
    uint32_t loader_part_sig_type;  // BOOT_PART_SIG_*: how the loader partition is identified
    uint32_t loader_part_number;    // its number in the partition table (1-based), 0 unknown
    uint64_t loader_part_start;     // its first LBA (the firmware's block size)
    uint64_t loader_part_size;      // its length in blocks
    uint8_t  loader_part_sig[16];   // GPT: unique partition GUID, on-disk byte order;
                                    // MBR: bytes 0-3 the disk signature (LE), rest 0
    char     loader_partuuid[40];   // the same as text: GPT canonical, MBR "xxxxxxxx-nn"
    uint32_t loader_disk_bus;       // BOOT_DISK_BUS_*: the bus the firmware reached the disk by
    uint32_t loader_disk_port;      // SATA HBA port, USB parent port, NVMe namespace; ~0u unknown
    char     loader_device_path[256]; // the loader partition's device path as text
    char     kernel_partuuid[40];   // PARTUUID of the partition kernel.elf was read from
    char     root_spec[128];        // root= value of the command line, "" none
    char     cmdline[512];          // the whole boot command line (ASCII, one line)
    uint32_t cmdline_source;        // BOOT_CMDLINE_*: where the command line came from
    uint32_t rootid_reserved;

    // ---- UEFI runtime services (valid when efi_magic == BOOT_EFI_MAGIC) ----
    // What the kernel needs to call the firmware's runtime services after
    // ExitBootServices (kernel/hal/efi.c).  SetVirtualAddressMap is never
    // called: the addresses are physical and stay so.  The runtime regions
    // are collected on their own so the cap on mem_info.entries cannot lose
    // one.  Appended at the end; the kernel's copy is kernel/mm/memory.h.
    uint64_t efi_magic;             // BOOT_EFI_MAGIC when the fields below are valid
    uint64_t efi_system_table;      // physical address of EFI_SYSTEM_TABLE
    uint64_t efi_runtime_services;  // physical address of EFI_RUNTIME_SERVICES
    uint32_t efi_memdesc_version;   // the firmware's memory descriptor version
    uint32_t efi_memdesc_size;      // the firmware's memory descriptor size (bytes)
    uint32_t efi_fw_revision;       // EFI_SYSTEM_TABLE.FirmwareRevision
    uint32_t efi_spec_revision;     // EFI_SYSTEM_TABLE.Hdr.Revision
    char     efi_fw_vendor[64];     // EFI_SYSTEM_TABLE.FirmwareVendor, folded to ASCII
    uint32_t efi_rt_count;          // entries used in efi_rt_regions[]
    uint32_t efi_rt_dropped;        // runtime regions that did not fit (0 normally)
    struct {
        uint32_t type;              // EFI memory type
        uint32_t pad;
        uint64_t phys;              // first byte
        uint64_t pages;             // length in 4 KiB pages
        uint64_t attribute;         // EFI_MEMORY_* bits
    } efi_rt_regions[BOOT_EFI_RT_MAX];
    uint32_t bootmenu_entry;        // boot menu entry started (0-based), BOOT_MENU_NONE
    uint32_t efi_reserved;
} boot_info_t;

#define BOOT_EFI_MAGIC 0x56524553544e5552ULL /* "RUNTSERV" */
#define BOOT_MENU_NONE 0xffffffffu
#define BL_EFI_MEMORY_RUNTIME 0x8000000000000000ULL

#define BOOT_SMBIOS_MAGIC 0x534d42494f53424cULL
#define BOOT_DISPLAY_MAGIC 0x5953494459414c50ULL
#define BOOT_ROOTID_MAGIC 0x44494f544f4f524cULL /* "LROOTOID" */

#define BOOT_PART_SIG_NONE 0
#define BOOT_PART_SIG_MBR  1
#define BOOT_PART_SIG_GPT  2

#define BOOT_DISK_BUS_UNKNOWN 0
#define BOOT_DISK_BUS_USB     1
#define BOOT_DISK_BUS_SATA    2
#define BOOT_DISK_BUS_NVME    3
#define BOOT_DISK_BUS_SCSI    4
#define BOOT_DISK_BUS_ATAPI   5
#define BOOT_DISK_BUS_SD      6
#define BOOT_DISK_BUS_VIRTUAL 7

#define BOOT_CMDLINE_NONE         0
#define BOOT_CMDLINE_LOAD_OPTIONS 1
#define BOOT_CMDLINE_ESP_FILE     2

// The optional boot command line on the EFI System Partition: one or more
// lines of "key=value" words (root=PARTUUID=..., root=UUID=..., root=LABEL=...,
// root=/dev/sda2, rootfstype=ext4); '#' starts a comment line.
#define BOOT_CMDLINE_PATH L"\\EFI\\LIKEOS\\cmdline.txt"

// Global boot info to pass to kernel
static boot_info_t g_boot_info;

// Paging structures (assuming 4-level paging)
typedef struct {
    UINT64 entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Higher half kernel constants (must match kernel.lds and memory.h)
#define KERNEL_OFFSET    0xFFFFFFFF80000000ULL
#define KERNEL_START     0x0

// Physical memory locations for page tables
// These will be dynamically allocated from UEFI at low addresses
// to avoid conflicts with VMware's EfiRuntimeServicesData regions.
// All page tables are allocated using AllocateMaxAddress to get low addresses.

// Dynamic page table addresses (set by init_page_tables())
static EFI_PHYSICAL_ADDRESS g_pml4_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pdpt_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pd_addr[4] = {0};    // 4 page directories for identity map (4GB)
static EFI_PHYSICAL_ADDRESS g_pdpt_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pd_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pt_high_addr = 0;
static EFI_PHYSICAL_ADDRESS g_pdpt_physmap_addr = 0;
// Direct map ("physmap") page directories.  One PD maps 1 GB, so the array is
// sized for the theoretical max of a single PML4 slot (512 * 1 GB = 512 GB).
// The number actually allocated/filled is g_num_physmap_pds, computed at boot
// from the real memory map — see detect_physmap_size().
#define PHYSMAP_MAX_PDS 512
#define PHYSMAP_MIN_PDS 16 // floor: keep <=16 GB MMIO/framebuffer covered
static EFI_PHYSICAL_ADDRESS g_pd_physmap_addr[PHYSMAP_MAX_PDS] = {0};
static UINTN g_num_physmap_pds = PHYSMAP_MIN_PDS; // GB of RAM the direct map covers

// Trampoline address (will be allocated below kernel)
static EFI_PHYSICAL_ADDRESS trampoline_addr = 0;

// Serial port I/O for debug output (COM1 = 0x3F8)
#define COM1_PORT 0x3F8

static inline void outb(UINT16 port, UINT8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00);    // Disable interrupts
    outb(COM1_PORT + 3, 0x80);    // Enable DLAB
    outb(COM1_PORT + 0, 0x03);    // 38400 baud (low byte)
    outb(COM1_PORT + 1, 0x00);    // (high byte)
    outb(COM1_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1_PORT + 2, 0xC7);    // Enable FIFO
    outb(COM1_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static void serial_putc(char c) {
    while ((inb(COM1_PORT + 5) & 0x20) == 0);  // Wait for transmit buffer empty
    outb(COM1_PORT, c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_puthex(UINT64 val) {
    const char hex[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    serial_puts("0x");
    serial_puts(buf);
}

static void serial_putdec(UINT64 val) {
    char buf[21];
    buf[20] = 0;
    int i = 19;
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0 && i >= 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    serial_puts(&buf[i+1]);
}

/* The serial log for the loader's other parts (bootcommon.h). */
void bl_log(const char *s) {
    serial_puts(s);
}

void bl_log_dec(UINT64 v) {
    serial_putdec(v);
}

void bl_log_hex(UINT64 v) {
    serial_puthex(v);
}

/*
 * is_leap_year - Check if a year is a leap year
 */
static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/*
 * days_in_month - Get the number of days in a given month
 */
static int days_in_month(int month, int year) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
        return 29;
    return days[month - 1];
}

/*
 * efi_time_to_epoch - Convert EFI_TIME to Unix epoch seconds
 *
 * This is called BEFORE ExitBootServices to get the system time from UEFI
 * since on some platforms (e.g., Intel Alder Lake with eSPI) the CMOS RTC
 * may not be accessible after ExitBootServices.
 */
static UINT64 efi_time_to_epoch(EFI_TIME *t) {
    UINT64 days = 0;
    int year, month;
    
    /* Count days from 1970 to the given year */
    for (year = 1970; year < t->Year; year++) {
        days += is_leap_year(year) ? 366 : 365;
    }
    
    /* Add days for months in the current year */
    for (month = 1; month < t->Month; month++) {
        days += days_in_month(month, t->Year);
    }
    
    /* Add the day of month (1-based, so subtract 1) */
    days += t->Day - 1;
    
    /* Convert to seconds and add hours, minutes, seconds */
    return (days * 86400) + (t->Hour * 3600) + (t->Minute * 60) + t->Second;
}

/*
 * read_boot_time - Read system time from UEFI before ExitBootServices
 *
 * This MUST be called before ExitBootServices() because UEFI Runtime
 * Services may not be usable after that point on all platforms.
 * The time is stored in g_boot_info.boot_epoch as Unix epoch seconds.
 */
static void read_boot_time(void) {
    EFI_TIME time;
    EFI_STATUS status;
    
    status = uefi_call_wrapper(RT->GetTime, 2, &time, NULL);
    if (EFI_ERROR(status)) {
        serial_puts("WARNING: GetTime failed, using epoch 0\n");
        g_boot_info.boot_epoch = 0;
        return;
    }
    
    g_boot_info.boot_epoch = efi_time_to_epoch(&time);
    
    serial_puts("Boot time: ");
    serial_putdec(time.Year);
    serial_putc('-');
    serial_putdec(time.Month);
    serial_putc('-');
    serial_putdec(time.Day);
    serial_putc(' ');
    serial_putdec(time.Hour);
    serial_putc(':');
    serial_putdec(time.Minute);
    serial_putc(':');
    serial_putdec(time.Second);
    serial_puts(" UTC -> epoch ");
    serial_putdec(g_boot_info.boot_epoch);
    serial_putc('\n');
}

// Memory type names for debug output
static const char* efi_memory_type_name(UINT32 type) {
    switch (type) {
        case 0: return "EfiReservedMemoryType";
        case 1: return "EfiLoaderCode";
        case 2: return "EfiLoaderData";
        case 3: return "EfiBootServicesCode";
        case 4: return "EfiBootServicesData";
        case 5: return "EfiRuntimeServicesCode";
        case 6: return "EfiRuntimeServicesData";
        case 7: return "EfiConventionalMemory";
        case 8: return "EfiUnusableMemory";
        case 9: return "EfiACPIReclaimMemory";
        case 10: return "EfiACPIMemoryNVS";
        case 11: return "EfiMemoryMappedIO";
        case 12: return "EfiMemoryMappedIOPortSpace";
        case 13: return "EfiPalCode";
        case 14: return "EfiPersistentMemory";
        default: return "Unknown";
    }
}

// Page table entry flags
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_SIZE       (1ULL << 7)    // PS bit for 2MB pages
#define PAGE_NX         (1ULL << 63)   // No-Execute bit

// Common page combinations
#define PAGE_RW         (PAGE_PRESENT | PAGE_WRITABLE)
#define PAGE_RWX        (PAGE_PRESENT | PAGE_WRITABLE)  // Executable (NX=0)
#define PAGE_RW_NX      (PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX)  // Non-executable

// Page table pool allocator - allocates from reserved low memory to avoid conflicts
// We'll allocate a contiguous block from UEFI in low memory during init
static EFI_PHYSICAL_ADDRESS pt_pool_base = 0;
static EFI_PHYSICAL_ADDRESS pt_pool_next = 0;
// Pool sizing: the fixed tables (PML4, low/high PDPT+PD+PT, physmap PDPT) plus
// dynamic kernel PTs need well under 112 pages; the physmap PDs add one page
// per GB mapped.  112 + g_num_physmap_pds reproduces the historical 128-page
// pool exactly at the 16-PD floor and scales up with RAM.  Set in
// detect_physmap_size(), consumed by init_page_table_pool().
static UINTN pt_pool_pages = 128;

// Round a byte count up to whole GiB, i.e. the number of 1 GB physmap PDs
// needed to cover it.
#define BYTES_TO_GB_CEIL(b) (((b) + (1024ULL*1024*1024) - 1) / (1024ULL*1024*1024))

// Walk the UEFI memory map to find the highest usable physical address, and
// from it decide how many 1 GB direct-map page directories to build.  Clamped
// to [PHYSMAP_MIN_PDS, PHYSMAP_MAX_PDS]: the floor keeps small-RAM machines'
// MMIO/framebuffer (typically < 16 GB) covered, the ceiling is one PML4 slot
// (512 GB).  Must run before init_page_table_pool so the pool is sized to fit
// the resulting PDs.  On any failure we keep the safe default (16 PDs).
static void detect_physmap_size(void) {
    UINTN map_size = 0, map_key, desc_size;
    UINT32 desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;

    EFI_STATUS status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map,
                                          &map_key, &desc_size, &desc_ver);
    if (status != EFI_BUFFER_TOO_SMALL)
        return; // keep default 16 PDs
    map_size += 4 * desc_size; // headroom: the pool alloc below grows the map
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, map_size,
                               (VOID**)&map);
    if (EFI_ERROR(status))
        return;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &map_size, map, &map_key,
                               &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, map);
        return;
    }

    UINT64 highest_usable_end = 0;
    UINTN entries = map_size / desc_size;
    for (UINTN i = 0; i < entries; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR*)((UINT8*)map + i * desc_size);
        // Only conventional/usable RAM extends the direct map; MMIO/reserved
        // holes above RAM are handled by the kernel's separate MMIO mapper.
        int usable = (d->Type == EfiConventionalMemory ||
                      d->Type == EfiLoaderCode || d->Type == EfiLoaderData ||
                      d->Type == EfiBootServicesCode ||
                      d->Type == EfiBootServicesData);
        if (!usable)
            continue;
        UINT64 end = d->PhysicalStart + d->NumberOfPages * 4096ULL;
        if (end > highest_usable_end)
            highest_usable_end = end;
    }
    uefi_call_wrapper(BS->FreePool, 1, map);

    UINTN pds = (UINTN)BYTES_TO_GB_CEIL(highest_usable_end);
    if (pds < PHYSMAP_MIN_PDS)
        pds = PHYSMAP_MIN_PDS;
    if (pds > PHYSMAP_MAX_PDS)
        pds = PHYSMAP_MAX_PDS;
    g_num_physmap_pds = pds;

    // Size the page-table pool to fit these PDs plus all the fixed tables.
    pt_pool_pages = 112 + g_num_physmap_pds;

    Print(L"Direct map: RAM ends at %lu GB -> %lu physmap PD(s)\r\n",
          (UINT64)BYTES_TO_GB_CEIL(highest_usable_end), (UINT64)g_num_physmap_pds);
}

static EFI_STATUS init_page_table_pool(void) {
    // Allocate page table pool in low memory (below 1MB to be safe)
    EFI_PHYSICAL_ADDRESS max_addr = 0x100000 - 1;  // Below 1MB
    
    EFI_STATUS status = uefi_call_wrapper(BS->AllocatePages, 4,
                                          AllocateMaxAddress,
                                          EfiLoaderData,
                                          pt_pool_pages,
                                          &max_addr);
    
    if (EFI_ERROR(status)) {
        // Try anywhere below kernel memory pool start
        max_addr = 0x1b00000 - 1;  // ~27MB, well below memory pool
        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateMaxAddress,
                                   EfiLoaderData,
                                   pt_pool_pages,
                                   &max_addr);
    }
    
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate page table pool: %r\r\n", status);
        return status;
    }
    
    pt_pool_base = max_addr;
    pt_pool_next = max_addr;
    
    // Zero the entire pool
    uefi_call_wrapper(BS->SetMem, 3, (VOID*)pt_pool_base, pt_pool_pages * 4096, 0);
    
    Print(L"Page table pool allocated at 0x%lx (%lu pages, %lu KB)\r\n", 
          pt_pool_base, pt_pool_pages, (pt_pool_pages * 4096) / 1024);
    
    return EFI_SUCCESS;
}

static EFI_PHYSICAL_ADDRESS allocate_page_table(void) {
    if (!pt_pool_base) {
        Print(L"ERROR: Page table pool not initialized!\r\n");
        return 0;
    }
    
    if (pt_pool_next >= pt_pool_base + (pt_pool_pages * 4096)) {
        Print(L"ERROR: Page table pool exhausted!\r\n");
        return 0;
    }
    
    EFI_PHYSICAL_ADDRESS addr = pt_pool_next;
    pt_pool_next += 4096;
    
    // Memory is already zeroed during pool init
    return addr;
}

// Allocate all initial page tables from the pool
static EFI_STATUS init_page_tables(void) {
    // Size the direct map to actual RAM before the pool is allocated, so the
    // pool is big enough to hold the physmap page directories.
    detect_physmap_size();

    // First allocate the pool
    EFI_STATUS status = init_page_table_pool();
    if (EFI_ERROR(status)) {
        return status;
    }
    
    // Allocate PML4 (one page)
    g_pml4_addr = allocate_page_table();
    if (!g_pml4_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate PDPT for identity mapping (one page)
    g_pdpt_addr = allocate_page_table();
    if (!g_pdpt_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate 4 page directories for identity mapping (4GB)
    for (int i = 0; i < 4; i++) {
        g_pd_addr[i] = allocate_page_table();
        if (!g_pd_addr[i]) return EFI_OUT_OF_RESOURCES;
    }
    
    // Allocate higher half page tables
    g_pdpt_high_addr = allocate_page_table();
    if (!g_pdpt_high_addr) return EFI_OUT_OF_RESOURCES;
    
    g_pd_high_addr = allocate_page_table();
    if (!g_pd_high_addr) return EFI_OUT_OF_RESOURCES;
    
    g_pt_high_addr = allocate_page_table();
    if (!g_pt_high_addr) return EFI_OUT_OF_RESOURCES;
    
    // Allocate physmap page tables
    g_pdpt_physmap_addr = allocate_page_table();
    if (!g_pdpt_physmap_addr) return EFI_OUT_OF_RESOURCES;

    // Allocate one page directory per GB of direct map (g_num_physmap_pds,
    // sized to actual RAM by detect_physmap_size()).
    for (UINTN i = 0; i < g_num_physmap_pds; i++) {
        g_pd_physmap_addr[i] = allocate_page_table();
        if (!g_pd_physmap_addr[i]) return EFI_OUT_OF_RESOURCES;
    }
    
    Print(L"Page tables allocated dynamically:\r\n");
    Print(L"  PML4:         0x%lx\r\n", g_pml4_addr);
    Print(L"  PDPT (low):   0x%lx\r\n", g_pdpt_addr);
    Print(L"  PD (low):     0x%lx - 0x%lx\r\n", g_pd_addr[0], g_pd_addr[3]);
    Print(L"  PDPT (high):  0x%lx\r\n", g_pdpt_high_addr);
    Print(L"  PD (high):    0x%lx\r\n", g_pd_high_addr);
    Print(L"  PT (high):    0x%lx\r\n", g_pt_high_addr);
    Print(L"  PDPT (phys):  0x%lx\r\n", g_pdpt_physmap_addr);
    Print(L"  PD (phys):    0x%lx - 0x%lx (%lu PD(s))\r\n",
          g_pd_physmap_addr[0], g_pd_physmap_addr[g_num_physmap_pds - 1],
          (UINT64)g_num_physmap_pds);
    
    serial_puts("Page tables allocated:\n");
    serial_puts("  PML4:       "); serial_puthex(g_pml4_addr); serial_puts("\n");
    serial_puts("  PDPT (low): "); serial_puthex(g_pdpt_addr); serial_puts("\n");
    serial_puts("  PDPT (high):"); serial_puthex(g_pdpt_high_addr); serial_puts("\n");
    serial_puts("  PDPT (phys):"); serial_puthex(g_pdpt_physmap_addr); serial_puts("\n");
    
    return EFI_SUCCESS;
}

// Reserve memory for SMP AP trampoline
// Must be below 1MB and 4KB aligned (SIPI vector constraint)
// Preferred address: 0x8000 (avoids real-mode IVT at 0x0-0x400 and BDA at 0x400-0x500)
static EFI_STATUS reserve_smp_trampoline(void) {
    EFI_STATUS status;
    
    // Preferred address for SMP trampoline (must be 4KB aligned, < 1MB)
    // We try several addresses to handle different UEFI memory layouts
    EFI_PHYSICAL_ADDRESS preferred_addrs[] = {
        0x8000,   // Traditional address (32KB)
        0x10000,  // 64KB
        0x20000,  // 128KB  
        0x40000,  // 256KB
        0x70000,  // 448KB (below EBDA)
    };
    
    for (UINTN i = 0; i < sizeof(preferred_addrs) / sizeof(preferred_addrs[0]); i++) {
        EFI_PHYSICAL_ADDRESS addr = preferred_addrs[i];
        
        // Try to allocate exactly at this address
        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateAddress,
                                   EfiLoaderData,
                                   1,  // 1 page = 4KB
                                   &addr);
        
        if (!EFI_ERROR(status)) {
            g_boot_info.smp_trampoline_addr = addr;
            Print(L"SMP trampoline reserved at: 0x%lx\r\n", addr);
            serial_puts("SMP trampoline at: ");
            serial_puthex(addr);
            serial_puts("\n");
            return EFI_SUCCESS;
        }
    }
    
    // Fallback: allocate any address below 1MB
    EFI_PHYSICAL_ADDRESS max_addr = 0xA0000 - 1;  // Below VGA memory
    status = uefi_call_wrapper(BS->AllocatePages, 4,
                               AllocateMaxAddress,
                               EfiLoaderData,
                               1,
                               &max_addr);
    
    if (!EFI_ERROR(status)) {
        g_boot_info.smp_trampoline_addr = max_addr;
        Print(L"SMP trampoline allocated at: 0x%lx (fallback)\r\n", max_addr);
        serial_puts("SMP trampoline (fallback): ");
        serial_puthex(max_addr);
        serial_puts("\n");
        return EFI_SUCCESS;
    }
    
    Print(L"WARNING: Could not reserve SMP trampoline memory: %r\r\n", status);
    g_boot_info.smp_trampoline_addr = 0;
    return status;
}

// External trampoline function from trampoline.S
extern void trampoline_jump(UINT64 kernel_entry, void* boot_info, UINT64 pml4_addr);

// We need the end symbol to calculate the size
extern char trampoline_jump_end[];

#define BL_PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define BL_IDENTITY_LOW_END 0x100000000ULL // the fixed identity map: 4 GB
// The kernel drops PML4 entries 0-3 with the identity map, so a mapping
// made here must stay below 4 * 512 GB.
#define BL_IDENTITY_MAX_END (4 * 0x8000000000ULL)

// Identity-map the GOP framebuffer when it lies above the fixed 4 GB identity
// map.  Firmware is free to place it high (a 64-bit BAR at 256 GB, say); the
// kernel's console writes to the address it is given from its first
// instruction on, before it has any page fault handler, and maps the
// framebuffer itself before the identity map is removed (see
// console_remap_to_direct_map).  2 MB pages from the page-table pool.
static void bl_identity_map_framebuffer(page_table_t *pml4) {
    UINT64 base = (UINT64)(UINTN)g_boot_info.fb_info.framebuffer_base;
    UINT64 size = g_boot_info.fb_info.framebuffer_size;
    UINT64 start, end, a;

    if (!base || !size)
        return;
    start = base & ~0x1FFFFFULL;
    end = (base + size + 0x1FFFFFULL) & ~0x1FFFFFULL;
    if (end <= BL_IDENTITY_LOW_END)
        return; // already inside the fixed identity map
    if (end > BL_IDENTITY_MAX_END) {
        Print(L"WARNING: framebuffer at 0x%lx is beyond the identity map range\r\n",
              base);
        return;
    }
    for (a = start; a < end; a += 0x200000ULL) {
        UINTN l4 = (a >> 39) & 0x1FF;
        UINTN l3 = (a >> 30) & 0x1FF;
        UINTN l2 = (a >> 21) & 0x1FF;
        page_table_t *pdpt, *pd;

        if (a < BL_IDENTITY_LOW_END)
            continue;
        if (!(pml4->entries[l4] & PAGE_PRESENT)) {
            EFI_PHYSICAL_ADDRESS t = allocate_page_table();
            if (!t)
                return;
            pml4->entries[l4] = t | PAGE_RWX;
        }
        pdpt = (page_table_t *)(UINTN)(pml4->entries[l4] & BL_PTE_ADDR_MASK);
        if (!(pdpt->entries[l3] & PAGE_PRESENT)) {
            EFI_PHYSICAL_ADDRESS t = allocate_page_table();
            if (!t)
                return;
            pdpt->entries[l3] = t | PAGE_RWX;
        }
        pd = (page_table_t *)(UINTN)(pdpt->entries[l3] & BL_PTE_ADDR_MASK);
        pd->entries[l2] = a | PAGE_RW | PAGE_SIZE; // no NX: EFER.NXE may be off
    }
    Print(L"  Framebuffer identity mapped: 0x%lx - 0x%lx\r\n", start, end);
}

// Set up higher half kernel paging with identity mapping for low memory
static void setup_higher_half_paging(UINT64 kernel_phys_addr, UINT64 kernel_size) {
    page_table_t *pml4 = (page_table_t*)g_pml4_addr;
    page_table_t *pdpt_low = (page_table_t*)g_pdpt_addr;
    
    // Higher half page tables
    page_table_t *pdpt_high = (page_table_t*)g_pdpt_high_addr;
    page_table_t *pd_high = (page_table_t*)g_pd_high_addr;
    page_table_t *pt_high = (page_table_t*)g_pt_high_addr;
    
    // Clear main page tables (they should already be zeroed from pool, but be safe)
    for (int i = 0; i < 512; i++) {
        pml4->entries[i] = 0;
        pdpt_low->entries[i] = 0;
        pdpt_high->entries[i] = 0;
        pd_high->entries[i] = 0;
        pt_high->entries[i] = 0;
    }
    
    // Set up PML4 entries
    // Entry 0: Low memory identity mapping (0x0 - 0x7FFFFFFFFF) - EXECUTABLE
    pml4->entries[0] = g_pdpt_addr | PAGE_RWX;
    
    // Entry 511: Higher half mapping (0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF)
    pml4->entries[511] = g_pdpt_high_addr | PAGE_RWX;
    
    // Set up low memory identity mapping for first 4GB
    // 4GB = 4 PDPT entries (each covers 1GB)
    for (int pdpt_i = 0; pdpt_i < 4; pdpt_i++) {
        // Use pre-allocated page directory for this 1GB region
        EFI_PHYSICAL_ADDRESS pd_addr = g_pd_addr[pdpt_i];
        page_table_t *pd = (page_table_t*)pd_addr;
        
        // Clear page directory
        for (int i = 0; i < 512; i++) {
            pd->entries[i] = 0;
        }
        
        // Set PDPT entry to point to this page directory
        pdpt_low->entries[pdpt_i] = pd_addr | PAGE_RWX;
        
        // Each page directory covers 512 * 2MB = 1GB using 2MB pages
        // Set up 2MB page entries for this 1GB region
        for (int pd_i = 0; pd_i < 512; pd_i++) {
            // Calculate physical address for this 2MB page
            UINT64 phys_addr = (UINT64)pdpt_i * 0x40000000ULL + (UINT64)pd_i * 0x200000ULL;
            
            // Create executable 2MB pages for identity mapping
            // CRITICAL: These pages must be executable for trampoline to work
            pd->entries[pd_i] = phys_addr | PAGE_RWX | PAGE_SIZE;
        }
    }
    
    // The framebuffer, wherever the firmware put it: the kernel's console
    // writes to it through this identity map until it maps it itself.
    bl_identity_map_framebuffer(pml4);

    // Set up higher half mapping for kernel
    // KERNEL_OFFSET = 0xFFFFFFFF80000000
    // Virtual address: KERNEL_OFFSET + KERNEL_START = 0xFFFFFFFF80000000
    // Physical address: kernel_phys_addr
    
    // Calculate indices for higher half mapping
    UINT64 kernel_virt = KERNEL_OFFSET + KERNEL_START;
    UINT64 pml4_index = (kernel_virt >> 39) & 0x1FF;  // Should be 511
    UINT64 pdpt_index = (kernel_virt >> 30) & 0x1FF;  // Should be 256 for 0xFFFFFFFF80000000
    UINT64 pd_index = (kernel_virt >> 21) & 0x1FF;    // Should be 0
    UINT64 pt_index = (kernel_virt >> 12) & 0x1FF;    // Should be 0
    
    // Set up higher half page table hierarchy
    pdpt_high->entries[pdpt_index] = g_pd_high_addr | PAGE_RWX;
    pd_high->entries[pd_index] = g_pt_high_addr | PAGE_RWX;
    
    // Map kernel pages in higher half
    UINT64 kernel_pages = (kernel_size + 4095) / 4096;
    
    // Calculate how much virtual memory we need to map
    // Must cover:
    //   - Kernel code/data/BSS (BSS includes static arrays like
    //     tcp_connections[] — at 128 conns × 128 inflight segs × 1480 B
    //     ≈ 24 MB, plus other per-conn ring buffers, BSS alone is ~70 MB)
    //   - Kernel heap: 8 MB
    //   - Physical page bitmap: total_pages/8 bytes (e.g. 500 KB for 16 GB RAM)
    //   - Page refcount array: total_pages*2 bytes (e.g. 8 MB for 16 GB RAM)
    // Use 128 MB to give plenty of headroom — 64 MB was just barely large
    // enough for the previous BSS and overflowed the moment the TCP inflight
    // ring grew, leaving the bitmap (placed at heap_end) unmapped.
    UINT64 min_virtual_size = 128 * 1024 * 1024; // 128 MB
    UINT64 total_pages_needed = min_virtual_size / 4096; // Convert to 4KB pages
    
    Print(L"Mapping %lu MB (%lu pages) of virtual memory starting at 0x%lx...\r\n", 
          min_virtual_size / (1024 * 1024), total_pages_needed, kernel_virt);
    
    // Calculate how many page tables we need
    // Each page table can map 512 pages (2MB of virtual memory)
    UINT64 page_tables_needed = (total_pages_needed + 511) / 512;
    
    Print(L"Allocating %lu additional page tables for extended mapping...\r\n", page_tables_needed - 1);
    
    // First page table is already allocated at PT_HIGH_ADDRESS
    // Map what we can with the first page table
    UINT64 first_pt_pages = (total_pages_needed < 512) ? total_pages_needed : 512;
    
    // Map kernel pages first with real physical addresses
    for (UINT64 i = 0; i < kernel_pages && i < first_pt_pages; i++) {
        UINT64 phys_addr = kernel_phys_addr + (i * 4096);
        pt_high->entries[pt_index + i] = phys_addr | PAGE_RWX;
    }
    
    // For pages beyond the kernel, allocate new physical memory
    for (UINT64 i = kernel_pages; i < first_pt_pages; i++) {
        // Allocate physical memory for this page
        UINTN pages_to_allocate = 1;
        EFI_PHYSICAL_ADDRESS phys_addr = 0;
        
        EFI_STATUS alloc_status = uefi_call_wrapper(BS->AllocatePages, 4, 
                                                    AllocateAnyPages, 
                                                    EfiLoaderData, 
                                                    pages_to_allocate, 
                                                    &phys_addr);
        
        if (EFI_ERROR(alloc_status)) {
            Print(L"ERROR: Could not allocate physical memory for page %lu: %r\r\n", i, alloc_status);
            break;
        }
        
        // Map the allocated physical memory to virtual address
        pt_high->entries[pt_index + i] = phys_addr | PAGE_RWX;
        
        // Zero out the allocated memory
        uefi_call_wrapper(BS->SetMem, 3, (VOID*)phys_addr, 4096, 0);
    }
    
    // If we need more page tables, allocate and set them up
    UINT64 pages_mapped = first_pt_pages;
    
    for (UINT64 pt_i = 1; pt_i < page_tables_needed && pages_mapped < total_pages_needed; pt_i++) {
        // Allocate page table from our reserved pool (not AllocateAnyPages!)
        EFI_PHYSICAL_ADDRESS pt_phys_addr = allocate_page_table();
        
        if (!pt_phys_addr) {
            Print(L"ERROR: Could not allocate page table %lu from pool\r\n", pt_i);
            break;
        }
        
        // Page table is already zeroed by allocate_page_table()
        
        // Set up page directory entry to point to this page table
        UINT64 pd_entry_index = pd_index + pt_i;
        if (pd_entry_index < 512) {
            pd_high->entries[pd_entry_index] = pt_phys_addr | PAGE_RWX;

            // Get pointer to the new page table
            page_table_t *current_pt = (page_table_t*)pt_phys_addr;
            
            // Map pages in this page table
            UINT64 pages_in_this_pt = (total_pages_needed - pages_mapped < 512) ? 
                                     (total_pages_needed - pages_mapped) : 512;
            
            for (UINT64 i = 0; i < pages_in_this_pt; i++) {
                // Check if this page is still within the kernel's pre-allocated physical memory
                if (pages_mapped < kernel_pages) {
                    // Map to the kernel's physical memory
                    UINT64 phys_addr = kernel_phys_addr + (pages_mapped * 4096);
                    current_pt->entries[i] = phys_addr | PAGE_RWX;
                } else {
                    // Allocate physical memory for this page (beyond kernel)
                    UINTN pages_to_allocate = 1;
                    EFI_PHYSICAL_ADDRESS phys_addr = 0;
                    
                    EFI_STATUS alloc_status = uefi_call_wrapper(BS->AllocatePages, 4, 
                                                                AllocateAnyPages, 
                                                                EfiLoaderData, 
                                                                pages_to_allocate, 
                                                                &phys_addr);
                    
                    if (EFI_ERROR(alloc_status)) {
                        Print(L"ERROR: Could not allocate physical memory for page %lu in PT %lu: %r\r\n", 
                              i, pt_i, alloc_status);
                        break;
                    }
                    
                    // Map the allocated physical memory to virtual address
                    current_pt->entries[i] = phys_addr | PAGE_RWX;
                    
                    // Zero out the allocated memory
                    uefi_call_wrapper(BS->SetMem, 3, (VOID*)phys_addr, 4096, 0);
                }
                
                pages_mapped++;
            }
        }
    }
    
    // ===================================================================
    // Set up DIRECT MAP region at PHYS_MAP_BASE = 0xFFFF880000000000
    // Maps physical memory 0 .. g_num_physmap_pds GB (sized to actual RAM by
    // detect_physmap_size()) at virt 0xFFFF880000000000+, using 2MB pages.
    // PML4 index 272 = (0xFFFF880000000000 >> 39) & 0x1FF
    // ===================================================================
    {
        page_table_t *pdpt_physmap = (page_table_t*)g_pdpt_physmap_addr;

        // Clear the PDPT for direct map
        for (int i = 0; i < 512; i++) {
            pdpt_physmap->entries[i] = 0;
        }

        // Set up PML4 entry 272 to point to direct map PDPT
        pml4->entries[272] = g_pdpt_physmap_addr | PAGE_RWX;

        // Map one 1GB region per allocated physmap PD.
        for (UINTN pdpt_i = 0; pdpt_i < g_num_physmap_pds; pdpt_i++) {
            // Use pre-allocated page directory for this 1GB region
            EFI_PHYSICAL_ADDRESS pd_addr = g_pd_physmap_addr[pdpt_i];
            page_table_t *pd = (page_table_t*)pd_addr;

            // Clear page directory
            for (int i = 0; i < 512; i++) {
                pd->entries[i] = 0;
            }

            // Set PDPT entry to point to this page directory (supervisor-only, RWX)
            pdpt_physmap->entries[pdpt_i] = pd_addr | PAGE_RWX;

            // Map each 2MB page in this 1GB region
            for (int pd_i = 0; pd_i < 512; pd_i++) {
                UINT64 phys_addr = (UINT64)pdpt_i * 0x40000000ULL + (UINT64)pd_i * 0x200000ULL;
                // Use 2MB pages (PAGE_SIZE bit set), supervisor-only (no PAGE_USER)
                pd->entries[pd_i] = phys_addr | PAGE_RWX | PAGE_SIZE;
            }
        }

        // Record the covered extent for the kernel so it can size its managed
        // memory to exactly what is mapped, independent of the 16GB legacy cap.
        g_boot_info.direct_map_bytes =
            (UINT64)g_num_physmap_pds * 0x40000000ULL;

        Print(L"  Direct map: 0xFFFF880000000000 -> phys 0x0 (%lu GB, 2MB pages)\r\n",
              (UINT64)g_num_physmap_pds);
    }
    
    Print(L"Higher half paging configured:\r\n");
    Print(L"  Identity mapped: 0x0 - 0x100000000 (4GB) [for boot only]\r\n");
    Print(L"  Kernel virtual: 0x%lx -> 0x%lx (%lu total pages mapped)\r\n", 
          kernel_virt, kernel_phys_addr, pages_mapped);
    Print(L"  Virtual memory covers: 0x%lx - 0x%lx (%lu MB)\r\n", 
          kernel_virt, kernel_virt + (pages_mapped * 4096), (pages_mapped * 4096) / (1024 * 1024));
}

// Allocate and setup trampoline below kernel space
static EFI_STATUS allocate_trampoline(UINT64 kernel_phys_addr) {
    EFI_STATUS status;
    
    // Calculate the actual size of the trampoline function
    UINTN trampoline_size = (UINTN)trampoline_jump_end - (UINTN)trampoline_jump;
    
    // Ensure we have at least some reasonable size
    if (trampoline_size < 16 || trampoline_size > 4096) {
        trampoline_size = 256; // Safe fallback
    }
    
    // Allocate a page below the kernel's physical address
    // This ensures the trampoline is in low memory and identity-mapped
    EFI_PHYSICAL_ADDRESS max_addr = kernel_phys_addr - 1; // Just below kernel
    UINTN pages = 1; // One page for trampoline
    
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, 
                               EfiLoaderCode, pages, &max_addr);
    if (EFI_ERROR(status)) {
        // If that fails, try to allocate anywhere below 1MB
        max_addr = 0x100000 - 1;
        status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateMaxAddress, 
                                   EfiLoaderCode, pages, &max_addr);
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Could not allocate trampoline memory: %r\r\n", status);
            return status;
        }
    }
    
    trampoline_addr = max_addr;
    Print(L"Trampoline allocated at: 0x%lx\r\n", trampoline_addr);
    
    // Copy trampoline function code to allocated memory
    uefi_call_wrapper(BS->CopyMem, 3, (VOID*)trampoline_addr, 
                      (VOID*)trampoline_jump, trampoline_size);
    
    Print(L"Trampoline code copied (%lu bytes)\r\n", trampoline_size);
    
    return EFI_SUCCESS;
}

// Validate ELF64 header
static BOOLEAN validate_elf64(Elf64_Ehdr *elf_header) {
    // Check ELF magic
    if (elf_header->e_ident[0] != ELFMAG0 ||
        elf_header->e_ident[1] != ELFMAG1 ||
        elf_header->e_ident[2] != ELFMAG2 ||
        elf_header->e_ident[3] != ELFMAG3) {
        return FALSE;
    }
    
    // Check 64-bit, little-endian, x86-64
    if (elf_header->e_ident[4] != ELFCLASS64 ||
        elf_header->e_ident[5] != ELFDATA2LSB ||
        elf_header->e_machine != EM_X86_64) {
        return FALSE;
    }
    
    return TRUE;
}

/* ===========================================================================
 *  Minimal read-only ext4 reader for booting from a single GPT USB disk
 * ---------------------------------------------------------------------------
 *  Loads /boot/kernel.elf straight from the ext4 root partition via
 *  EFI_BLOCK_IO.  This is the only supported boot path: the kernel always
 *  lives on the ext4 root, never on the FAT ESP (which carries the bootloader
 *  alone).  Standalone: uses only BS->AllocatePool and blkio->ReadBlocks (no
 *  kernel allocator, no caches).
 *
 *  On-disk fields are read by byte offset (little-endian) to avoid struct
 *  packing concerns in the freestanding EFI environment.  Mirrors the read
 *  logic in kernel/fs/ext4.c (superblock, group desc, inode, extent + classic
 *  indirect mapping, linear directory scan).
 * =========================================================================== */

#define E4_SUPER_MAGIC      0xEF53
#define E4_ROOT_INO         2
#define E4_EXT_MAGIC        0xF30A
#define E4_NDIR_BLOCKS      12
#define E4_EXTENTS_FL       0x00080000u
#define E4_INLINE_DATA_FL   0x10000000u
#define E4_INCOMPAT_64BIT   0x0080u

static UINT16 e4_rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
static UINT32 e4_rd32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *blk;
    UINT32  media_id;
    UINT32  dev_bs;             /* device logical block size (Media->BlockSize) */
    UINT32  block_size;         /* ext4 block size                             */
    UINT32  spb;               /* device sectors per ext4 block                */
    UINT32  inodes_per_group;
    UINT32  inode_size;
    UINT32  desc_size;          /* group descriptor size (32 or 64)            */
    UINT64  gdt_block;          /* first block of the group descriptor table   */
    UINT8  *scratch;            /* one ext4-block scratch buffer               */
    UINT8  *node;               /* one ext4-block extent-node buffer           */
} e4_ctx;

/* Read `count` device sectors starting at device LBA `lba`. */
static EFI_STATUS e4_read_dev(e4_ctx *c, UINT64 lba, UINT32 count, VOID *buf) {
    return uefi_call_wrapper(c->blk->ReadBlocks, 5, c->blk, c->media_id,
                             (EFI_LBA)lba, (UINTN)count * c->dev_bs, buf);
}

/* Read one ext4 filesystem block (block_size bytes) into buf. */
static EFI_STATUS e4_read_block(e4_ctx *c, UINT64 fsblock, VOID *buf) {
    return e4_read_dev(c, fsblock * c->spb, c->spb, buf);
}

/* Read an inode's raw bytes (inode_size) into out. */
static EFI_STATUS e4_read_inode(e4_ctx *c, UINT32 ino, UINT8 *out) {
    if (ino == 0) return EFI_NOT_FOUND;
    UINT32 group = (ino - 1) / c->inodes_per_group;
    UINT32 index = (ino - 1) % c->inodes_per_group;

    /* Group descriptor lives at gdt_block + (group*desc_size)/block_size. */
    UINT64 gd_byte = (UINT64)group * c->desc_size;
    UINT64 gd_blk  = c->gdt_block + gd_byte / c->block_size;
    UINT32 gd_off  = (UINT32)(gd_byte % c->block_size);
    if (EFI_ERROR(e4_read_block(c, gd_blk, c->scratch))) return EFI_DEVICE_ERROR;

    UINT64 itbl = e4_rd32(c->scratch + gd_off + 8);          /* bg_inode_table_lo */
    if (c->desc_size >= 64)
        itbl |= (UINT64)e4_rd32(c->scratch + gd_off + 40) << 32; /* _hi */

    UINT64 byte = (UINT64)index * c->inode_size;
    UINT64 iblk = itbl + byte / c->block_size;
    UINT32 ioff = (UINT32)(byte % c->block_size);
    if (EFI_ERROR(e4_read_block(c, iblk, c->scratch))) return EFI_DEVICE_ERROR;

    UINT32 n = c->inode_size; if (n > 256) n = 256;
    for (UINT32 i = 0; i < n; i++) out[i] = c->scratch[ioff + i];
    return EFI_SUCCESS;
}

/* Map a file logical block index to a physical block number (0 == hole). */
static UINT64 e4_bmap(e4_ctx *c, const UINT8 *inode, UINT64 lidx) {
    UINT32 flags = e4_rd32(inode + 32);
    const UINT8 *iblock = inode + 40;       /* i_block[15] (60 bytes) */

    if (flags & E4_EXTENTS_FL) {
        UINT8 nodebuf[60];
        const UINT8 *node = iblock;
        for (UINT32 i = 0; i < 60; i++) nodebuf[i] = iblock[i];
        node = nodebuf;
        for (;;) {
            if (e4_rd16(node + 0) != E4_EXT_MAGIC) return 0;
            UINT16 entries = e4_rd16(node + 2);
            UINT16 depth   = e4_rd16(node + 6);
            if (depth == 0) {
                for (UINT16 e = 0; e < entries; e++) {
                    const UINT8 *ex = node + 12 + (UINT32)e * 12;
                    UINT32 ee_block = e4_rd32(ex + 0);
                    UINT16 ee_len   = e4_rd16(ex + 4);
                    UINT16 len = ee_len > 32768 ? (UINT16)(ee_len - 32768) : ee_len;
                    if (lidx >= ee_block && lidx < (UINT64)ee_block + len) {
                        UINT64 start = (UINT64)e4_rd32(ex + 8)
                                     | ((UINT64)e4_rd16(ex + 6) << 32);
                        return start + (lidx - ee_block);
                    }
                }
                return 0;       /* hole / not present */
            }
            /* interior node: pick the index entry whose ei_block <= lidx */
            UINT32 child_lo = 0; UINT32 child_hi = 0; int found = 0;
            for (UINT16 e = 0; e < entries; e++) {
                const UINT8 *ix = node + 12 + (UINT32)e * 12;
                UINT32 ei_block = e4_rd32(ix + 0);
                if ((UINT64)ei_block <= lidx) {
                    child_lo = e4_rd32(ix + 4);
                    child_hi = e4_rd16(ix + 8);
                    found = 1;
                } else break;
            }
            if (!found) return 0;
            UINT64 child = (UINT64)child_lo | ((UINT64)child_hi << 32);
            if (EFI_ERROR(e4_read_block(c, child, c->node))) return 0;
            node = c->node;
        }
    }

    /* classic ext2/3 indirect mapping */
    UINT32 ppb = c->block_size / 4;     /* pointers per block */
    if (lidx < E4_NDIR_BLOCKS)
        return e4_rd32(iblock + (UINT32)lidx * 4);
    lidx -= E4_NDIR_BLOCKS;
    if (lidx < ppb) {                   /* single indirect */
        UINT64 ind = e4_rd32(iblock + 12 * 4);
        if (!ind) return 0;
        if (EFI_ERROR(e4_read_block(c, ind, c->node))) return 0;
        return e4_rd32(c->node + lidx * 4);
    }
    lidx -= ppb;
    if (lidx < (UINT64)ppb * ppb) {     /* double indirect */
        UINT64 dind = e4_rd32(iblock + 13 * 4);
        if (!dind) return 0;
        if (EFI_ERROR(e4_read_block(c, dind, c->node))) return 0;
        UINT64 mid = e4_rd32(c->node + (lidx / ppb) * 4);
        if (!mid) return 0;
        if (EFI_ERROR(e4_read_block(c, mid, c->node))) return 0;
        return e4_rd32(c->node + (lidx % ppb) * 4);
    }
    return 0;                           /* triple indirect: not needed for boot */
}

static UINT64 e4_isize(const UINT8 *inode) {
    return (UINT64)e4_rd32(inode + 4) | ((UINT64)e4_rd32(inode + 108) << 32);
}

static int e4_name_eq(const UINT8 *p, UINT8 nlen, const char *s) {
    UINT32 i = 0;
    for (; i < nlen; i++) { if (s[i] == 0 || (char)p[i] != s[i]) return 0; }
    return s[i] == 0;
}

/* Find `name` in directory inode; return child inode number or 0. */
static UINT32 e4_dir_lookup(e4_ctx *c, const UINT8 *dir_inode, const char *name) {
    UINT64 size = e4_isize(dir_inode);
    UINT64 nblocks = (size + c->block_size - 1) / c->block_size;
    for (UINT64 b = 0; b < nblocks; b++) {
        UINT64 pbn = e4_bmap(c, dir_inode, b);
        if (!pbn) continue;
        if (EFI_ERROR(e4_read_block(c, pbn, c->scratch))) continue;
        UINT32 off = 0;
        while (off + 8 <= c->block_size) {
            UINT32 child = e4_rd32(c->scratch + off + 0);
            UINT16 rlen  = e4_rd16(c->scratch + off + 4);
            UINT8  nlen  = c->scratch[off + 6];
            if (rlen < 8) break;             /* corrupt: avoid infinite loop */
            if (child != 0 && nlen != 0 &&
                e4_name_eq(c->scratch + off + 8, nlen, name))
                return child;
            off += rlen;
        }
    }
    return 0;
}

/* Resolve an absolute path to an inode number (0 on failure). */
static UINT32 e4_resolve(e4_ctx *c, const char *path, UINT8 *inode_out) {
    UINT32 ino = E4_ROOT_INO;
    if (EFI_ERROR(e4_read_inode(c, ino, inode_out))) return 0;
    const char *p = path;
    while (*p == '/') p++;
    char comp[256];
    while (*p) {
        UINT32 ci = 0;
        while (*p && *p != '/' && ci < sizeof(comp) - 1) comp[ci++] = *p++;
        comp[ci] = 0;
        while (*p == '/') p++;
        if (ci == 0) continue;
        UINT32 child = e4_dir_lookup(c, inode_out, comp);
        if (!child) return 0;
        if (EFI_ERROR(e4_read_inode(c, child, inode_out))) return 0;
        ino = child;
    }
    return ino;
}

/* Open an ext4 filesystem on `blk' (a partition, or a whole disk holding a
 * filesystem without a partition table): read and check the superblock,
 * build the reader context and allocate its two block-sized buffers.  When
 * `sb_out' is non-NULL the first 1024 bytes of the superblock are copied
 * there as well (the root selection reads the UUID and label from it).
 * EFI_NOT_FOUND when there is no ext4 superblock; e4_ctx_close() releases
 * what a successful open allocated. */
static EFI_STATUS e4_ctx_open(EFI_BLOCK_IO_PROTOCOL *blk, e4_ctx *c, UINT8 *sb_out) {
    if (!blk || !blk->Media || !blk->Media->MediaPresent) return EFI_NO_MEDIA;
    UINT32 dev_bs = blk->Media->BlockSize;
    if (dev_bs == 0 || dev_bs > 4096 || (dev_bs & (dev_bs - 1))) return EFI_UNSUPPORTED;

    /* Read enough to cover the superblock at byte offset 1024. */
    UINT32 probe_bytes = dev_bs > 2048 ? dev_bs : 2048;
    UINT8 *probe = NULL;
    if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                    probe_bytes, (VOID **)&probe)) || !probe)
        return EFI_OUT_OF_RESOURCES;
    if (EFI_ERROR(uefi_call_wrapper(blk->ReadBlocks, 5, blk,
                                    blk->Media->MediaId, (EFI_LBA)0,
                                    (UINTN)probe_bytes, probe))) {
        uefi_call_wrapper(BS->FreePool, 1, probe);
        return EFI_DEVICE_ERROR;
    }
    const UINT8 *sb = probe + 1024;
    if (e4_rd16(sb + 56) != E4_SUPER_MAGIC) {
        uefi_call_wrapper(BS->FreePool, 1, probe);
        return EFI_NOT_FOUND;
    }
    if (sb_out) {
        for (UINT32 i = 0; i < 1024; i++) sb_out[i] = sb[i];
    }

    /* Build the context from the superblock. */
    c->blk = blk;
    c->media_id = blk->Media->MediaId;
    c->dev_bs = dev_bs;
    UINT32 log_bs = e4_rd32(sb + 24);                       /* s_log_block_size */
    c->block_size = log_bs <= 6 ? (1024u << log_bs) : 0;    /* 1 KiB .. 64 KiB */
    c->inodes_per_group = e4_rd32(sb + 40);
    c->inode_size = e4_rd16(sb + 88);
    if (c->inode_size < 128) c->inode_size = 128;
    UINT32 incompat = e4_rd32(sb + 96);
    c->desc_size = 32;
    if (incompat & E4_INCOMPAT_64BIT) {
        UINT16 ds = e4_rd16(sb + 254);
        c->desc_size = ds >= 32 ? ds : 32;
    }
    UINT32 first_data = e4_rd32(sb + 20);                   /* s_first_data_block */
    uefi_call_wrapper(BS->FreePool, 1, probe);
    if (c->block_size == 0 || c->inodes_per_group == 0 ||
        c->block_size < c->dev_bs || (c->block_size % c->dev_bs) != 0)
        return EFI_UNSUPPORTED;
    c->spb = c->block_size / c->dev_bs;
    c->gdt_block = (UINT64)first_data + 1;

    /* Allocate the two reusable ext4-block scratch buffers. */
    c->scratch = NULL; c->node = NULL;
    if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                    c->block_size, (VOID **)&c->scratch)) ||
        EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                    c->block_size, (VOID **)&c->node))) {
        if (c->scratch) uefi_call_wrapper(BS->FreePool, 1, c->scratch);
        if (c->node)    uefi_call_wrapper(BS->FreePool, 1, c->node);
        c->scratch = NULL; c->node = NULL;
        return EFI_OUT_OF_RESOURCES;
    }
    return EFI_SUCCESS;
}

static void e4_ctx_close(e4_ctx *c) {
    if (c->scratch) uefi_call_wrapper(BS->FreePool, 1, c->scratch);
    if (c->node)    uefi_call_wrapper(BS->FreePool, 1, c->node);
    c->scratch = NULL;
    c->node = NULL;
}

/* Load the file at `path' (at most `max_size' bytes) from an opened ext4
 * context into a freshly allocated pool buffer. */
static EFI_STATUS e4_load_file(e4_ctx *c, const char *path, UINT64 max_size,
                               VOID **out_buf, UINTN *out_size) {
    UINT8 inode[256];
    UINT32 ino = e4_resolve(c, path, inode);
    if (!ino) return EFI_NOT_FOUND;
    UINT64 size = e4_isize(inode);
    if (size == 0 || size > max_size) return EFI_BAD_BUFFER_SIZE;

    /* A file small enough to live in the inode itself (inline
     * data): its bytes are where the block map would be. */
    if ((e4_rd32(inode + 32) & E4_INLINE_DATA_FL) && size <= 60) {
        VOID *ibuf = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                        (UINTN)size, &ibuf)) || !ibuf)
            return EFI_OUT_OF_RESOURCES;
        for (UINT64 z = 0; z < size; z++)
            ((UINT8 *)ibuf)[z] = inode[40 + z];
        *out_buf = ibuf;
        *out_size = (UINTN)size;
        return EFI_SUCCESS;
    }

    /* Allocate rounded up to whole blocks so bulk reads of full blocks
     * never overflow (the ELF loader only consumes `size` bytes). */
    UINT64 nblk = (size + c->block_size - 1) / c->block_size;
    UINTN  alloc = (UINTN)(nblk * c->block_size);
    VOID *kbuf = NULL;
    if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                    alloc, &kbuf)) || !kbuf)
        return EFI_OUT_OF_RESOURCES;
    /* Read contiguous physical block runs in a single ReadBlocks call
     * instead of one transfer per 4 KB block — turns a ~1.3 MB kernel
     * load from ~300 USB transactions into a handful. */
    UINT64 b = 0;
    while (b < nblk) {
        UINT8 *dst = (UINT8 *)kbuf + b * c->block_size;
        UINT64 pbn = e4_bmap(c, inode, b);
        if (!pbn) {                                  /* sparse hole */
            for (UINT64 z = 0; z < c->block_size; z++) dst[z] = 0;
            b++;
            continue;
        }
        UINT64 run = 1;
        while (b + run < nblk && run < 1024) {        /* cap run at 4 MB */
            if (e4_bmap(c, inode, b + run) != pbn + run) break;
            run++;
        }
        if (EFI_ERROR(e4_read_dev(c, pbn * c->spb, (UINT32)(run * c->spb), dst))) {
            uefi_call_wrapper(BS->FreePool, 1, kbuf);
            return EFI_DEVICE_ERROR;
        }
        b += run;
    }
    *out_buf = kbuf;
    *out_size = (UINTN)size;
    return EFI_SUCCESS;
}

/* ===========================================================================
 *  Boot identity: the partition this loader was started from
 * ---------------------------------------------------------------------------
 *  The kernel must mount the root filesystem of THIS installation -- the one
 *  on the disk the firmware booted -- and never another disk that merely
 *  happens to carry an ext4 filesystem (an internal SATA disk next to the
 *  USB stick, a second installation, a data disk).  The established way to
 *  learn which disk that is:
 *
 *    1. EFI_LOADED_IMAGE_PROTOCOL on our own ImageHandle gives DeviceHandle,
 *       the handle of the partition the firmware read this program from (the
 *       EFI System Partition).
 *    2. EFI_DEVICE_PATH_PROTOCOL on that handle is the path from the root
 *       bridge to the partition, e.g.
 *         PciRoot(0x0)/Pci(0x1F,0x2)/Sata(0x0,0xFFFF,0x0)/HD(1,GPT,<guid>,...)
 *    3. The Hard Drive media node -- type 4 (MEDIA_DEVICE_PATH), subtype 1
 *       (MEDIA_HARDDRIVE_DP) -- names the partition: its number, start and
 *       size, and its signature.  SignatureType 2 means the signature is the
 *       GPT unique partition GUID (what a running system calls the
 *       PARTUUID); SignatureType 1 means an MBR disk, the signature then being
 *       the 32-bit disk signature, and the PARTUUID is written
 *       "<signature>-<partition number>".
 *
 *  Everything in front of the HD() node names the DISK.  Two device paths
 *  that share that prefix are partitions of the same disk, which is how the
 *  root partition is found below: on the loader's disk, the partition whose
 *  GPT type is the x86-64 root type (4f68bce3-e8cd-4db1-96e7-fbcaf984b709).
 *  The kernel repeats that decision with its own drivers (kernel/ke/storage.c)
 *  from the loader PARTUUID passed in boot_info; the PARTUUID of the
 *  partition kernel.elf was actually read from is passed too, so both sides
 *  can confirm they agree.
 *
 *  An explicit root= on the boot command line overrides the search; the
 *  command line comes from the boot entry's load options (a UEFI shell, an
 *  efibootmgr entry) or, when those are empty, from \EFI\LIKEOS\cmdline.txt
 *  on the ESP.
 * =========================================================================== */

/* Device path node types and subtypes used here (UEFI specification,
 * "Device Path Protocol").  Spelled out under our own names: the GNU-EFI
 * headers define only some of them, under names that vary by version. */
#define BL_DP_HARDWARE        0x01
#define BL_DP_MESSAGING       0x03
#define BL_DP_MEDIA           0x04
#define BL_DP_END             0x7f
#define BL_DP_END_ENTIRE      0xff
#define BL_HW_VENDOR          0x04
#define BL_MSG_ATAPI          0x01
#define BL_MSG_SCSI           0x02
#define BL_MSG_USB            0x05
#define BL_MSG_VENDOR         0x0a
#define BL_MSG_USB_CLASS      0x0f
#define BL_MSG_USB_WWID       0x10
#define BL_MSG_SATA           0x12
#define BL_MSG_NVME           0x17
#define BL_MSG_UFS            0x19
#define BL_MSG_SD             0x1a
#define BL_MSG_EMMC           0x1d
#define BL_MEDIA_HARDDRIVE    0x01
#define BL_MEDIA_CDROM        0x02
#define BL_MEDIA_RAMDISK      0x09
#define BL_HD_SIG_MBR         0x01
#define BL_HD_SIG_GUID        0x02
/* Sanity bounds for walking a firmware device path */
#define BL_DP_MAX_NODES       64
#define BL_DP_MAX_BYTES       4096

/* The Hard Drive media node exactly as the specification lays it out: 42
 * bytes, no padding.  Device path nodes are byte-packed and may sit at any
 * address, so a node is always copied into one of these before its
 * multi-byte fields are read. */
typedef struct __attribute__((packed)) {
    UINT8  Type;
    UINT8  SubType;
    UINT8  Length[2];
    UINT32 PartitionNumber;
    UINT64 PartitionStart;
    UINT64 PartitionSize;
    UINT8  Signature[16];
    UINT8  MBRType;
    UINT8  SignatureType;
} bl_hd_node_t;

/* GPT type GUID of the x86-64 root partition, 4f68bce3-e8cd-4db1-96e7-
 * fbcaf984b709, in the on-disk (EFI_GUID) byte order. */
static const UINT8 g_gpt_type_root_x86_64[16] = {
    0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
    0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09
};

/* What the boot identity code learned about the loader partition. */
typedef struct {
    BOOLEAN          have_hd;      /* an HD() node was found */
    EFI_HANDLE       device;       /* LoadedImage->DeviceHandle */
    EFI_DEVICE_PATH *dp;           /* its device path (firmware memory) */
    UINTN            prefix_len;   /* bytes of dp in front of the HD() node */
    bl_hd_node_t     hd;           /* copy of the HD() node */
} bl_loader_t;

static bl_loader_t g_loader;

/* The GPT of the loader's disk, as far as root selection needs it. */
#define BL_GPT_MAX_ENTRIES 128
typedef struct {
    BOOLEAN valid;
    UINT32  nr;                                  /* entries read */
    UINT8   type[BL_GPT_MAX_ENTRIES][16];        /* type GUIDs (on-disk order) */
    UINT8   uuid[BL_GPT_MAX_ENTRIES][16];        /* unique GUIDs */
    char    name[BL_GPT_MAX_ENTRIES][37];        /* names, ASCII-folded */
} bl_gpt_t;

static bl_gpt_t g_loader_gpt;

/* The root partition the bootloader chose (kernel.elf and display.conf are
 * read from it alone); NULL when the choice could not be made, in which case
 * the old any-ext4-partition search is used. */
static EFI_BLOCK_IO_PROTOCOL *g_root_blk = NULL;

static BOOLEAN bl_mem_eq(const void *a, const void *b, UINTN n) {
    const UINT8 *x = (const UINT8 *)a, *y = (const UINT8 *)b;
    for (UINTN i = 0; i < n; i++)
        if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static void bl_mem_copy(void *dst, const void *src, UINTN n) {
    UINT8 *d = (UINT8 *)dst;
    const UINT8 *s = (const UINT8 *)src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
}

static char bl_lower(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

/* Case-insensitive ASCII string equality. */
static BOOLEAN bl_str_eq_nocase(const char *a, const char *b) {
    while (*a && *b) {
        if (bl_lower(*a) != bl_lower(*b)) return FALSE;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static BOOLEAN bl_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static BOOLEAN bl_str_prefix(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return FALSE;
        s++; prefix++;
    }
    return TRUE;
}

/* Bounded copy that always terminates (cap >= 1). */
static void bl_str_copy(char *dst, UINTN cap, const char *src) {
    UINTN i = 0;
    if (cap == 0) return;
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void bl_hex_bytes(char **p, const UINT8 *b, UINTN n, BOOLEAN reverse) {
    static const char hx[] = "0123456789abcdef";
    for (UINTN i = 0; i < n; i++) {
        UINT8 v = b[reverse ? n - 1 - i : i];
        *(*p)++ = hx[v >> 4];
        *(*p)++ = hx[v & 15];
    }
}

/* GUID in on-disk (mixed-endian EFI_GUID) byte order -> canonical lowercase
 * text "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee" (37 bytes with the NUL). */
static void bl_guid_to_str(const UINT8 *g, char *out) {
    char *p = out;
    bl_hex_bytes(&p, g + 0, 4, TRUE);  *p++ = '-';
    bl_hex_bytes(&p, g + 4, 2, TRUE);  *p++ = '-';
    bl_hex_bytes(&p, g + 6, 2, TRUE);  *p++ = '-';
    bl_hex_bytes(&p, g + 8, 2, FALSE); *p++ = '-';
    bl_hex_bytes(&p, g + 10, 6, FALSE);
    *p = 0;
}

/* ext4 UUID (16 raw bytes, printed in storage order) -> text. */
static void bl_uuid_to_str(const UINT8 *u, char *out) {
    char *p = out;
    bl_hex_bytes(&p, u + 0, 4, FALSE);  *p++ = '-';
    bl_hex_bytes(&p, u + 4, 2, FALSE);  *p++ = '-';
    bl_hex_bytes(&p, u + 6, 2, FALSE);  *p++ = '-';
    bl_hex_bytes(&p, u + 8, 2, FALSE);  *p++ = '-';
    bl_hex_bytes(&p, u + 10, 6, FALSE);
    *p = 0;
}

/* The PARTUUID text of a partition from its HD() node: the GPT GUID, or for
 * an MBR disk "<disk signature as 8 hex digits>-<partition number as 2>". */
static BOOLEAN bl_hd_partuuid(const bl_hd_node_t *hd, char *out) {
    if (hd->SignatureType == BL_HD_SIG_GUID) {
        bl_guid_to_str(hd->Signature, out);
        return TRUE;
    }
    if (hd->SignatureType == BL_HD_SIG_MBR) {
        char *p = out;
        UINT8 num = (UINT8)hd->PartitionNumber;
        bl_hex_bytes(&p, hd->Signature, 4, TRUE);
        *p++ = '-';
        bl_hex_bytes(&p, &num, 1, FALSE);
        *p = 0;
        return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

/* Length of one node, 0 when it is malformed (shorter than its header, or
 * running past what is left of the walk budget). */
static UINTN bl_dp_node_len(const EFI_DEVICE_PATH *n, UINTN budget) {
    UINTN len = (UINTN)n->Length[0] | ((UINTN)n->Length[1] << 8);
    if (len < 4 || len > budget) return 0;
    return len;
}

static BOOLEAN bl_dp_is_end(const EFI_DEVICE_PATH *n) {
    return (n->Type & 0x7f) == BL_DP_END && n->SubType == BL_DP_END_ENTIRE;
}

/* Find the first Hard Drive media node of `dp'.  Returns TRUE and copies it
 * to *hd, with *prefix_len = the bytes in front of it (the disk's path). */
static BOOLEAN bl_dp_find_hd(const EFI_DEVICE_PATH *dp, UINTN *prefix_len,
                             bl_hd_node_t *hd) {
    const UINT8 *p = (const UINT8 *)dp;
    UINTN off = 0;
    if (!dp) return FALSE;
    for (UINTN i = 0; i < BL_DP_MAX_NODES; i++) {
        const EFI_DEVICE_PATH *n = (const EFI_DEVICE_PATH *)(p + off);
        UINTN len = bl_dp_node_len(n, BL_DP_MAX_BYTES - off);
        if (!len || bl_dp_is_end(n)) return FALSE;
        if ((n->Type & 0x7f) == BL_DP_MEDIA && n->SubType == BL_MEDIA_HARDDRIVE &&
            len >= sizeof(bl_hd_node_t)) {
            bl_mem_copy(hd, n, sizeof(*hd));
            *prefix_len = off;
            return TRUE;
        }
        off += len;
        if (off >= BL_DP_MAX_BYTES) return FALSE;
    }
    return FALSE;
}

/* Does `dp' start with the loader disk's path, followed directly by `next'
 * (an HD() node when `want_hd', the end node otherwise)?  That is: is it a
 * partition of, or the whole of, the loader's disk? */
static BOOLEAN bl_dp_on_loader_disk(const EFI_DEVICE_PATH *dp, BOOLEAN want_hd) {
    if (!dp || !g_loader.have_hd || g_loader.prefix_len == 0 ||
        g_loader.prefix_len >= BL_DP_MAX_BYTES)
        return FALSE;
    /* Walk dp node by node up to prefix_len so a short or malformed path
     * is never read past its end. */
    const UINT8 *p = (const UINT8 *)dp;
    UINTN off = 0;
    for (UINTN i = 0; i < BL_DP_MAX_NODES && off < g_loader.prefix_len; i++) {
        const EFI_DEVICE_PATH *n = (const EFI_DEVICE_PATH *)(p + off);
        UINTN len = bl_dp_node_len(n, BL_DP_MAX_BYTES - off);
        if (!len || bl_dp_is_end(n)) return FALSE;
        off += len;
    }
    if (off != g_loader.prefix_len) return FALSE;
    if (!bl_mem_eq(dp, g_loader.dp, g_loader.prefix_len)) return FALSE;
    const EFI_DEVICE_PATH *next = (const EFI_DEVICE_PATH *)(p + off);
    if (!bl_dp_node_len(next, BL_DP_MAX_BYTES - off)) return FALSE;
    if (want_hd)
        return (next->Type & 0x7f) == BL_DP_MEDIA && next->SubType == BL_MEDIA_HARDDRIVE;
    return bl_dp_is_end(next);
}

/* The bus the firmware reached the loader's disk through: the LAST
 * messaging node in front of the HD() node decides (a USB stick behind a
 * hub has several USB nodes; a SATA disk behind a RAID-mode controller
 * shows Sata()), with its port number where the node has one. */
static void bl_dp_classify_bus(const EFI_DEVICE_PATH *dp, UINTN prefix_len,
                               UINT32 *bus, UINT32 *port) {
    const UINT8 *p = (const UINT8 *)dp;
    UINTN off = 0;
    *bus = BOOT_DISK_BUS_UNKNOWN;
    *port = ~0u;
    for (UINTN i = 0; i < BL_DP_MAX_NODES && off < prefix_len; i++) {
        const EFI_DEVICE_PATH *n = (const EFI_DEVICE_PATH *)(p + off);
        UINTN len = bl_dp_node_len(n, BL_DP_MAX_BYTES - off);
        if (!len || bl_dp_is_end(n)) break;
        const UINT8 *b = (const UINT8 *)n;
        UINT8 type = n->Type & 0x7f;
        if (type == BL_DP_MESSAGING) {
            switch (n->SubType) {
            case BL_MSG_USB:
                *bus = BOOT_DISK_BUS_USB;
                *port = len >= 5 ? b[4] : ~0u;          /* ParentPortNumber */
                break;
            case BL_MSG_USB_CLASS:
            case BL_MSG_USB_WWID:
                *bus = BOOT_DISK_BUS_USB;
                break;
            case BL_MSG_SATA:
                *bus = BOOT_DISK_BUS_SATA;              /* HBAPortNumber */
                *port = len >= 6 ? (UINT32)(b[4] | (b[5] << 8)) : ~0u;
                break;
            case BL_MSG_NVME:                           /* NamespaceId */
                *bus = BOOT_DISK_BUS_NVME;
                *port = len >= 8 ? (UINT32)b[4] | ((UINT32)b[5] << 8) |
                                   ((UINT32)b[6] << 16) | ((UINT32)b[7] << 24) : ~0u;
                break;
            case BL_MSG_SCSI:                           /* Pun (target) */
            case BL_MSG_UFS:
                *bus = BOOT_DISK_BUS_SCSI;
                *port = len >= 6 ? (UINT32)(b[4] | (b[5] << 8)) : ~0u;
                break;
            case BL_MSG_ATAPI:                          /* primary/secondary */
                *bus = BOOT_DISK_BUS_ATAPI;
                *port = len >= 6 ? (UINT32)(b[4] * 2 + b[5]) : ~0u;
                break;
            case BL_MSG_SD:
            case BL_MSG_EMMC:
                *bus = BOOT_DISK_BUS_SD;
                break;
            default:
                break;
            }
        } else if ((type == BL_DP_MEDIA && n->SubType == BL_MEDIA_RAMDISK) ||
                   (type == BL_DP_HARDWARE && n->SubType == BL_HW_VENDOR &&
                    *bus == BOOT_DISK_BUS_UNKNOWN)) {
            *bus = BOOT_DISK_BUS_VIRTUAL;
        }
        off += len;
    }
}

static const char *bl_bus_name(UINT32 bus) {
    switch (bus) {
    case BOOT_DISK_BUS_USB:     return "usb";
    case BOOT_DISK_BUS_SATA:    return "sata";
    case BOOT_DISK_BUS_NVME:    return "nvme";
    case BOOT_DISK_BUS_SCSI:    return "scsi";
    case BOOT_DISK_BUS_ATAPI:   return "ata";
    case BOOT_DISK_BUS_SD:      return "sd/mmc";
    case BOOT_DISK_BUS_VIRTUAL: return "virtual";
    default:                    return "unknown";
    }
}

/* Find the loader partition and record its identity in boot_info. */
static void boot_identity_init(EFI_HANDLE image) {
    EFI_LOADED_IMAGE *li = NULL;
    EFI_DEVICE_PATH *dp = NULL;

    g_loader.have_hd = FALSE;
    g_boot_info.rootid_magic = BOOT_ROOTID_MAGIC;
    g_boot_info.loader_part_sig_type = BOOT_PART_SIG_NONE;
    g_boot_info.loader_disk_bus = BOOT_DISK_BUS_UNKNOWN;
    g_boot_info.loader_disk_port = ~0u;

    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, image,
                                    &LoadedImageProtocol, (VOID **)&li)) || !li) {
        serial_puts("boot-id: no LoadedImage protocol on our image handle\n");
        return;
    }
    g_loader.device = li->DeviceHandle;
    if (!li->DeviceHandle ||
        EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle,
                                    &DevicePathProtocol, (VOID **)&dp)) || !dp) {
        serial_puts("boot-id: the loader's device has no device path\n");
        return;
    }
    g_loader.dp = dp;

    /* The device path as text, for the logs on both sides. */
    {
        CHAR16 *s = DevicePathToStr(dp);
        if (s) {
            UINTN i = 0;
            for (; s[i] && i + 1 < sizeof(g_boot_info.loader_device_path); i++)
                g_boot_info.loader_device_path[i] =
                    (s[i] >= 0x20 && s[i] < 0x7f) ? (char)s[i] : '?';
            g_boot_info.loader_device_path[i] = 0;
            uefi_call_wrapper(BS->FreePool, 1, s);
        }
        serial_puts("boot-id: loader device path ");
        serial_puts(g_boot_info.loader_device_path[0] ?
                    g_boot_info.loader_device_path : "(no text form)");
        serial_putc('\n');
    }

    if (!bl_dp_find_hd(dp, &g_loader.prefix_len, &g_loader.hd)) {
        /* A loader started from a whole-disk filesystem, a CD image or a
         * network: there is no partition to identify the disk by. */
        serial_puts("boot-id: no HD() node -- loader not on a disk partition\n");
        return;
    }
    g_loader.have_hd = TRUE;
    g_boot_info.loader_part_number = g_loader.hd.PartitionNumber;
    g_boot_info.loader_part_start = g_loader.hd.PartitionStart;
    g_boot_info.loader_part_size = g_loader.hd.PartitionSize;
    bl_mem_copy(g_boot_info.loader_part_sig, g_loader.hd.Signature, 16);
    if (g_loader.hd.SignatureType == BL_HD_SIG_GUID)
        g_boot_info.loader_part_sig_type = BOOT_PART_SIG_GPT;
    else if (g_loader.hd.SignatureType == BL_HD_SIG_MBR)
        g_boot_info.loader_part_sig_type = BOOT_PART_SIG_MBR;
    if (g_boot_info.loader_part_sig_type == BOOT_PART_SIG_MBR) {
        /* only the 32-bit disk signature is meaningful */
        for (int i = 4; i < 16; i++) g_boot_info.loader_part_sig[i] = 0;
    }
    bl_hd_partuuid(&g_loader.hd, g_boot_info.loader_partuuid);
    bl_dp_classify_bus(dp, g_loader.prefix_len, &g_boot_info.loader_disk_bus,
                       &g_boot_info.loader_disk_port);

    serial_puts("boot-id: loader partition ");
    serial_putdec(g_loader.hd.PartitionNumber);
    serial_puts(g_boot_info.loader_part_sig_type == BOOT_PART_SIG_GPT ? " GPT" :
                g_boot_info.loader_part_sig_type == BOOT_PART_SIG_MBR ? " MBR" :
                " (no signature)");
    serial_puts(" PARTUUID=");
    serial_puts(g_boot_info.loader_partuuid[0] ? g_boot_info.loader_partuuid : "-");
    serial_puts(" start=");
    serial_putdec(g_loader.hd.PartitionStart);
    serial_puts(" size=");
    serial_putdec(g_loader.hd.PartitionSize);
    serial_puts(" bus=");
    serial_puts(bl_bus_name(g_boot_info.loader_disk_bus));
    serial_putc('\n');
    Print(L"Loader partition: %d, PARTUUID=%a (%a)\r\n",
          (int)g_loader.hd.PartitionNumber,
          g_boot_info.loader_partuuid[0] ? g_boot_info.loader_partuuid : "-",
          bl_bus_name(g_boot_info.loader_disk_bus));
}

/* ---- Boot command line -------------------------------------------------- */

/* Normalise raw command-line text into boot_info.cmdline: lines starting
 * with '#' are comments, every run of blanks, tabs and line ends becomes one
 * space, anything outside printable ASCII is dropped, and the result is
 * trimmed.  `skip_first' drops the first word (the program's own name, which
 * a UEFI shell puts in front of the options). */
static void bl_cmdline_set(const char *raw, UINTN len, BOOLEAN skip_first) {
    char *out = g_boot_info.cmdline;
    UINTN cap = sizeof(g_boot_info.cmdline), o = 0;
    BOOLEAN line_start = TRUE, in_comment = FALSE, pending_space = FALSE;
    BOOLEAN skipping = skip_first;

    for (UINTN i = 0; i < len; i++) {
        char ch = raw[i];
        if (ch == 0) break;
        if (ch == '\r' || ch == '\n') {
            line_start = TRUE;
            in_comment = FALSE;
            pending_space = o > 0;
            skipping = FALSE;
            continue;
        }
        if (in_comment) continue;
        if (line_start && ch == '#') { in_comment = TRUE; continue; }
        if (ch == ' ' || ch == '\t') {
            if (!line_start) pending_space = o > 0;
            if (skipping && !line_start) skipping = FALSE;
            continue;
        }
        line_start = FALSE;
        if (ch < 0x20 || ch > 0x7e) continue;
        if (skipping) continue;
        if (pending_space) {
            if (o + 1 >= cap) break;
            out[o++] = ' ';
            pending_space = FALSE;
        }
        if (o + 1 >= cap) break;
        out[o++] = ch;
    }
    out[o] = 0;
}

/* The value of the word `key=' of boot_info.cmdline (the last one wins, as
 * with repeated kernel options); FALSE when absent. */
static BOOLEAN bl_cmdline_get(const char *key, char *val, UINTN cap) {
    const char *s = g_boot_info.cmdline;
    BOOLEAN found = FALSE;
    UINTN klen = 0;
    while (key[klen]) klen++;
    while (*s) {
        while (*s == ' ') s++;
        const char *w = s;
        while (*s && *s != ' ') s++;
        if ((UINTN)(s - w) > klen && bl_str_prefix(w, key) && w[klen] == '=') {
            UINTN n = 0;
            const char *v = w + klen + 1;
            while (v < s && n + 1 < cap) val[n++] = *v++;
            val[n] = 0;
            found = TRUE;
        }
    }
    return found;
}

/* The boot entry's load options as a command line: UCS-2 text, accepted
 * only when it really is printable text (firmware passes binary data there
 * for its own boot entries).  A UEFI shell includes the program name as the
 * first word; it is dropped when it names an .efi file. */
static BOOLEAN bl_cmdline_from_load_options(EFI_HANDLE image) {
    EFI_LOADED_IMAGE *li = NULL;
    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, image,
                                    &LoadedImageProtocol, (VOID **)&li)) || !li)
        return FALSE;
    if (!li->LoadOptions || li->LoadOptionsSize < 2 || (li->LoadOptionsSize & 1))
        return FALSE;
    const CHAR16 *w = (const CHAR16 *)li->LoadOptions;
    UINTN n = li->LoadOptionsSize / 2;
    char tmp[512];
    UINTN o = 0;
    for (UINTN i = 0; i < n && w[i]; i++) {
        CHAR16 ch = w[i];
        if (ch != '\t' && ch != '\r' && ch != '\n' && (ch < 0x20 || ch > 0x7e))
            return FALSE;                     /* not text: firmware data */
        if (o + 1 < sizeof(tmp)) tmp[o++] = (char)ch;
    }
    tmp[o] = 0;
    if (o == 0) return FALSE;
    /* Is the first word a program name? */
    BOOLEAN skip = FALSE;
    {
        UINTN i = 0, end;
        while (i < o && tmp[i] == ' ') i++;
        end = i;
        while (end < o && tmp[end] != ' ') end++;
        if (end - i >= 4 && tmp[end - 4] == '.' && bl_lower(tmp[end - 3]) == 'e' &&
            bl_lower(tmp[end - 2]) == 'f' && bl_lower(tmp[end - 1]) == 'i')
            skip = TRUE;
    }
    bl_cmdline_set(tmp, o, skip);
    if (!g_boot_info.cmdline[0]) return FALSE;
    g_boot_info.cmdline_source = BOOT_CMDLINE_LOAD_OPTIONS;
    return TRUE;
}

/* \EFI\LIKEOS\cmdline.txt on the partition the loader came from. */
static BOOLEAN bl_cmdline_from_esp(void) {
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_FILE_HANDLE root = NULL, file = NULL;
    char buf[1024];
    UINTN size = sizeof(buf) - 1;
    BOOLEAN ok = FALSE;

    if (!g_loader.device ||
        EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, g_loader.device,
                                    &FileSystemProtocol, (VOID **)&fs)) || !fs)
        return FALSE;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root)) || !root)
        return FALSE;
    if (!EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &file,
                                     BOOT_CMDLINE_PATH, EFI_FILE_MODE_READ, 0ULL)) &&
        file) {
        if (!EFI_ERROR(uefi_call_wrapper(file->Read, 3, file, &size, buf)) && size > 0) {
            if (size >= sizeof(buf)) size = sizeof(buf) - 1;
            buf[size] = 0;
            bl_cmdline_set(buf, size, FALSE);
            if (g_boot_info.cmdline[0]) {
                g_boot_info.cmdline_source = BOOT_CMDLINE_ESP_FILE;
                ok = TRUE;
            }
        }
        uefi_call_wrapper(file->Close, 1, file);
    }
    uefi_call_wrapper(root->Close, 1, root);
    return ok;
}

/* The command line (load options first, then the ESP file) and its root=. */
static void boot_cmdline_init(EFI_HANDLE image) {
    g_boot_info.cmdline[0] = 0;
    g_boot_info.root_spec[0] = 0;
    g_boot_info.cmdline_source = BOOT_CMDLINE_NONE;
    if (!bl_cmdline_from_load_options(image))
        bl_cmdline_from_esp();
    if (g_boot_info.cmdline[0]) {
        serial_puts("boot-id: command line (");
        serial_puts(g_boot_info.cmdline_source == BOOT_CMDLINE_LOAD_OPTIONS ?
                    "load options" : "ESP cmdline.txt");
        serial_puts("): ");
        serial_puts(g_boot_info.cmdline);
        serial_putc('\n');
        Print(L"Boot command line: %a\r\n", g_boot_info.cmdline);
    }
    if (bl_cmdline_get("root", g_boot_info.root_spec, sizeof(g_boot_info.root_spec))) {
        serial_puts("boot-id: root=");
        serial_puts(g_boot_info.root_spec);
        serial_putc('\n');
    }
}

/* The boot menu's choice: its words go after the command line's own (so a
 * word given twice is decided by the menu -- the last one wins), and root=
 * is looked up again in case the entry names one.  Whole words only: one
 * that does not fit any more is left out, never cut. */
static void bl_cmdline_append(const char *words) {
    char *out = g_boot_info.cmdline;
    UINTN cap = sizeof(g_boot_info.cmdline), o = 0;
    BOOLEAN dropped = FALSE;

    while (out[o]) o++;
    while (*words) {
        const char *w;
        UINTN n = 0;

        while (*words == ' ') words++;
        w = words;
        while (*words && *words != ' ') words++;
        n = (UINTN)(words - w);
        if (!n) break;
        if (o + (o ? 1 : 0) + n + 1 > cap) {
            dropped = TRUE;
            continue;
        }
        if (o) out[o++] = ' ';
        for (UINTN i = 0; i < n; i++) out[o++] = w[i];
        out[o] = 0;
    }
    if (dropped) {
        serial_puts("boot-id: command line full, menu words left out\n");
        Print(L"WARNING: the boot command line is full; menu options left out\r\n");
    }
    g_boot_info.root_spec[0] = 0;
    if (bl_cmdline_get("root", g_boot_info.root_spec, sizeof(g_boot_info.root_spec))) {
        serial_puts("boot-id: root=");
        serial_puts(g_boot_info.root_spec);
        serial_putc('\n');
    }
    serial_puts("boot-id: command line with the menu's words: ");
    serial_puts(out);
    serial_putc('\n');
}

/* The boot menu, when this partition has one (the live medium). */
static void boot_menu(EFI_HANDLE image) {
    char words[BOOTMENU_ARGS_MAX];
    INT32 choice;

    g_boot_info.bootmenu_entry = BOOT_MENU_NONE;
    choice = bootmenu_run(image, words, sizeof(words));
    if (choice < 0)
        return;
    g_boot_info.bootmenu_entry = (uint32_t)choice;
    if (words[0])
        bl_cmdline_append(words);
    if (g_boot_info.cmdline[0])
        Print(L"Boot command line: %a\r\n", g_boot_info.cmdline);
}

/* Memory the kernel may allocate from once boot services are gone: the
 * kernel treats all of these types alike (mm_is_usable_memory_type). */
static int bl_mem_type_usable(UINT32 type) {
    return type == EfiLoaderCode || type == EfiLoaderData ||
           type == EfiBootServicesCode || type == EfiBootServicesData ||
           type == EfiConventionalMemory;
}

/* May desc be folded into the stored entry prev?  Only when it continues
 * prev without a gap and with the same attributes, and either has the same
 * type or both are allocatable RAM the firmware no longer needs: the kernel
 * makes no distinction between those types, and a firmware's map is mostly
 * such fragments (a 403-entry map would otherwise not fit). */
static int bl_mem_entry_mergeable(const memory_map_entry_t *prev,
                                  const EFI_MEMORY_DESCRIPTOR *desc) {
    if (prev->physical_start + prev->number_of_pages * 4096 != desc->PhysicalStart)
        return 0;
    if (prev->attribute != desc->Attribute)
        return 0;
    if (prev->type == desc->Type)
        return 1;
    return bl_mem_type_usable(prev->type) && bl_mem_type_usable(desc->Type) &&
           !(desc->Attribute & BL_EFI_MEMORY_RUNTIME);
}

/* The memory map for the kernel (boot_info_t.mem_info) from a map read
 * with GetMemoryMap: adjacent entries the kernel would treat alike are
 * folded into one (bl_mem_entry_mergeable), and at most
 * MAX_MEMORY_MAP_ENTRIES entries are kept.  Plain memory work, no firmware
 * call: also run once more after ExitBootServices with the final map.
 * dump: the whole map to the serial port as well.  Returns the number of
 * entries that did not fit. */
static UINTN bl_store_mem_info(const EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
                               UINTN desc_size, int dump) {
    UINTN num_entries = desc_size ? map_size / desc_size : 0;
    UINTN dropped = 0;

    g_boot_info.mem_info.entry_count = 0;
    g_boot_info.mem_info.descriptor_size = (uint32_t)desc_size;
    g_boot_info.mem_info.total_memory = 0;

    if (dump) {
        // Dump full memory map to serial for VMware debugging
        serial_puts("\n=== UEFI MEMORY MAP ===\n");
        serial_puts("Entries: ");
        serial_putdec(num_entries);
        serial_puts(", Descriptor size: ");
        serial_putdec(desc_size);
        serial_puts("\n\n");
    }

    for (UINTN i = 0; i < num_entries; i++) {
        const EFI_MEMORY_DESCRIPTOR *desc =
            (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)map + i * desc_size);

        if (dump) {
            serial_puts("[");
            serial_putdec(i);
            serial_puts("] ");
            serial_puthex(desc->PhysicalStart);
            serial_puts(" - ");
            serial_puthex(desc->PhysicalStart + desc->NumberOfPages * 4096);
            serial_puts(" (");
            serial_putdec(desc->NumberOfPages);
            serial_puts(" pages, ");
            serial_putdec(desc->NumberOfPages * 4096 / 1024);
            serial_puts(" KB) Type=");
            serial_putdec(desc->Type);
            serial_puts(" ");
            serial_puts(efi_memory_type_name(desc->Type));
            serial_puts("\n");
        }

        // Count usable memory (EfiConventionalMemory type = 7)
        if (desc->Type == 7) {
            g_boot_info.mem_info.total_memory += desc->NumberOfPages * 4096;
        }

        if (!desc->NumberOfPages)
            continue;

        // Fold into the previous entry where the kernel cannot tell them apart
        if (g_boot_info.mem_info.entry_count) {
            memory_map_entry_t *prev =
                &g_boot_info.mem_info.entries[g_boot_info.mem_info.entry_count - 1];
            if (bl_mem_entry_mergeable(prev, desc)) {
                if (prev->type != desc->Type)
                    prev->type = EfiConventionalMemory;
                prev->number_of_pages += desc->NumberOfPages;
                continue;
            }
        }

        if (g_boot_info.mem_info.entry_count >= MAX_MEMORY_MAP_ENTRIES) {
            dropped++;
            continue;
        }

        // Copy entry to boot_info
        memory_map_entry_t *entry = &g_boot_info.mem_info.entries[g_boot_info.mem_info.entry_count];
        entry->type = desc->Type;
        entry->physical_start = desc->PhysicalStart;
        entry->virtual_start = desc->VirtualStart;
        entry->number_of_pages = desc->NumberOfPages;
        entry->attribute = desc->Attribute;

        g_boot_info.mem_info.entry_count++;
    }

    if (dump) {
        serial_puts("\nTotal usable memory: ");
        serial_putdec(g_boot_info.mem_info.total_memory / (1024 * 1024));
        serial_puts(" MB\n");
        serial_puts("Stored entries: ");
        serial_putdec(g_boot_info.mem_info.entry_count);
        serial_puts(", dropped: ");
        serial_putdec(dropped);
        serial_puts("\n=== END MEMORY MAP ===\n\n");
    }
    return dropped;
}

/* What the kernel needs for the runtime services: the tables, and every
 * region of the memory map marked EFI_MEMORY_RUNTIME.  Plain memory work,
 * no firmware call: also run once more after ExitBootServices with the
 * final map. */
static void bl_collect_runtime(const EFI_MEMORY_DESCRIPTOR *map, UINTN map_size,
                               UINTN desc_size, UINT32 desc_version) {
    UINTN n = desc_size ? map_size / desc_size : 0;

    g_boot_info.efi_rt_count = 0;
    g_boot_info.efi_rt_dropped = 0;
    g_boot_info.efi_memdesc_size = (uint32_t)desc_size;
    g_boot_info.efi_memdesc_version = desc_version;
    for (UINTN i = 0; i < n; i++) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)((const UINT8 *)map + i * desc_size);
        UINT32 k;

        if (!(d->Attribute & BL_EFI_MEMORY_RUNTIME) || !d->NumberOfPages)
            continue;
        k = g_boot_info.efi_rt_count;
        if (k >= BOOT_EFI_RT_MAX) {
            g_boot_info.efi_rt_dropped++;
            continue;
        }
        g_boot_info.efi_rt_regions[k].type = d->Type;
        g_boot_info.efi_rt_regions[k].pad = 0;
        g_boot_info.efi_rt_regions[k].phys = d->PhysicalStart;
        g_boot_info.efi_rt_regions[k].pages = d->NumberOfPages;
        g_boot_info.efi_rt_regions[k].attribute = d->Attribute;
        g_boot_info.efi_rt_count = k + 1;
    }
}

/* Descriptors of room kept free in the buffer ExitBootServices' map is read
 * into: what the firmware may add between that read and the call (events
 * signalled by a first, failed ExitBootServices), when the retry reads the
 * map once more into the same buffer and nothing may be allocated. */
#define BL_MMAP_SLACK 64

/* The memory map ExitBootServices is called with, read after the last
 * allocation the loader makes so that its key is current.  The buffer is
 * replaced by a larger one (still allowed: ExitBootServices has not been
 * called yet) whenever the map plus BL_MMAP_SLACK descriptors no longer
 * fits it; *cap is the size of the buffer. */
static EFI_STATUS bl_read_final_memory_map(EFI_MEMORY_DESCRIPTOR **map,
                                           UINTN *map_size, UINTN *cap,
                                           UINTN *map_key, UINTN *desc_size,
                                           UINT32 *desc_version) {
    EFI_STATUS status = EFI_BUFFER_TOO_SMALL;

    for (int tries = 0; tries < 4 && status == EFI_BUFFER_TOO_SMALL; tries++) {
        UINTN need = 0;

        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &need, NULL, map_key,
                                   desc_size, desc_version);
        if (status != EFI_BUFFER_TOO_SMALL)
            return EFI_ERROR(status) ? status : EFI_LOAD_ERROR;
        if (need + BL_MMAP_SLACK * *desc_size > *cap) {
            EFI_MEMORY_DESCRIPTOR *grown = NULL;
            UINTN grown_cap = need + 2 * BL_MMAP_SLACK * *desc_size;

            status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                       grown_cap, (VOID**)&grown);
            if (EFI_ERROR(status))
                return status;
            uefi_call_wrapper(BS->FreePool, 1, *map);
            *map = grown;
            *cap = grown_cap;
        }
        *map_size = *cap;
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, map_size, *map,
                                   map_key, desc_size, desc_version);
    }
    return status;
}

static void bl_efi_tables(void) {
    UINTN i = 0;

    g_boot_info.efi_system_table = (uint64_t)(UINTN)ST;
    g_boot_info.efi_runtime_services = (uint64_t)(UINTN)ST->RuntimeServices;
    g_boot_info.efi_fw_revision = ST->FirmwareRevision;
    g_boot_info.efi_spec_revision = ST->Hdr.Revision;
    if (ST->FirmwareVendor) {
        for (; ST->FirmwareVendor[i] && i + 1 < sizeof(g_boot_info.efi_fw_vendor); i++) {
            CHAR16 c = ST->FirmwareVendor[i];
            g_boot_info.efi_fw_vendor[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    }
    g_boot_info.efi_fw_vendor[i] = 0;
    g_boot_info.efi_magic = BOOT_EFI_MAGIC;
}

/* ---- The loader disk's partition table ----------------------------------- */

/* Read the GPT of the loader's disk through the whole-disk BLOCK_IO handle
 * (the one whose device path is exactly the disk prefix).  Only the entries'
 * type and unique GUIDs and their names are kept.  The header CRC is not
 * re-checked: the firmware already accepted this table to produce the HD()
 * nodes; the entry array bounds are checked. */
static void bl_read_loader_gpt(void) {
    EFI_GUID blk_guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE *handles = NULL;
    UINTN nhandles = 0;

    g_loader_gpt.valid = FALSE;
    g_loader_gpt.nr = 0;
    if (!g_loader.have_hd || g_loader.hd.SignatureType != BL_HD_SIG_GUID) return;
    if (EFI_ERROR(uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                                    &blk_guid, NULL, &nhandles, &handles)) || !handles)
        return;
    for (UINTN h = 0; h < nhandles && !g_loader_gpt.valid; h++) {
        EFI_BLOCK_IO_PROTOCOL *blk = NULL;
        EFI_DEVICE_PATH *dp = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[h],
                                        &blk_guid, (VOID **)&blk)) || !blk)
            continue;
        if (!blk->Media || !blk->Media->MediaPresent || blk->Media->LogicalPartition)
            continue;
        if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[h],
                                        &DevicePathProtocol, (VOID **)&dp)) || !dp)
            continue;
        if (!bl_dp_on_loader_disk(dp, FALSE))
            continue;
        UINT32 bs = blk->Media->BlockSize;
        if (bs < 512 || bs > 4096 || (bs & (bs - 1))) continue;

        UINT8 *hdr = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, bs,
                                        (VOID **)&hdr)) || !hdr)
            break;
        if (EFI_ERROR(uefi_call_wrapper(blk->ReadBlocks, 5, blk, blk->Media->MediaId,
                                        (EFI_LBA)1, (UINTN)bs, hdr)) ||
            !bl_mem_eq(hdr, "EFI PART", 8)) {
            uefi_call_wrapper(BS->FreePool, 1, hdr);
            serial_puts("boot-id: loader disk has no readable GPT header\n");
            break;
        }
        UINT64 ent_lba = 0;
        UINT32 num = 0, esz = 0;
        bl_mem_copy(&ent_lba, hdr + 72, 8);
        bl_mem_copy(&num, hdr + 80, 4);
        bl_mem_copy(&esz, hdr + 84, 4);
        uefi_call_wrapper(BS->FreePool, 1, hdr);
        if (esz < 128 || esz > 1024 || (esz & 7) || num == 0 || ent_lba < 2 ||
            ent_lba > blk->Media->LastBlock) {
            serial_puts("boot-id: loader disk GPT header out of range\n");
            break;
        }
        if (num > BL_GPT_MAX_ENTRIES) num = BL_GPT_MAX_ENTRIES;
        UINTN bytes = (UINTN)num * esz;
        UINTN blocks = (bytes + bs - 1) / bs;
        UINT8 *arr = NULL;
        if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                        blocks * bs, (VOID **)&arr)) || !arr)
            break;
        if (!EFI_ERROR(uefi_call_wrapper(blk->ReadBlocks, 5, blk, blk->Media->MediaId,
                                         (EFI_LBA)ent_lba, blocks * bs, arr))) {
            for (UINT32 i = 0; i < num; i++) {
                const UINT8 *e = arr + (UINTN)i * esz;
                bl_mem_copy(g_loader_gpt.type[i], e + 0, 16);
                bl_mem_copy(g_loader_gpt.uuid[i], e + 16, 16);
                /* name: 36 UTF-16LE units at offset 56, folded to ASCII */
                UINTN k = 0;
                for (; k < 36; k++) {
                    UINT16 u = (UINT16)(e[56 + 2 * k] | (e[57 + 2 * k] << 8));
                    if (!u) break;
                    g_loader_gpt.name[i][k] = (u >= 0x20 && u < 0x7f) ? (char)u : '?';
                }
                g_loader_gpt.name[i][k] = 0;
            }
            g_loader_gpt.nr = num;
            g_loader_gpt.valid = TRUE;
        }
        uefi_call_wrapper(BS->FreePool, 1, arr);
    }
    uefi_call_wrapper(BS->FreePool, 1, handles);
    serial_puts(g_loader_gpt.valid ? "boot-id: loader disk GPT read, entries="
                                   : "boot-id: loader disk GPT not available\n");
    if (g_loader_gpt.valid) {
        serial_putdec(g_loader_gpt.nr);
        serial_putc('\n');
    }
}

/* ---- Root partition selection --------------------------------------------- */

/* Candidate ranks, best first.  What the kernel does with its own drivers
 * is the same ladder (kernel/ke/storage.c). */
#define BL_RANK_ROOT_SPEC   0   /* named by root= */
#define BL_RANK_ROOT_TYPE   1   /* loader disk, GPT type x86-64 root */
#define BL_RANK_LOADER_DISK 2   /* loader disk, any other ext4 partition */
#define BL_RANK_ANY         3   /* any other ext4 (the old search) */
#define BL_RANK_NONE        4

#define BL_MAX_CANDIDATES 64

typedef struct {
    EFI_BLOCK_IO_PROTOCOL *blk;
    UINT32  rank;
    UINT32  partno;              /* 0 = whole device */
    char    partuuid[40];
    char    fs_uuid[40];
    char    label[17];
} bl_cand_t;

/* Does candidate `c' match the root= specification? */
static BOOLEAN bl_root_spec_matches(const bl_cand_t *c, const char *gpt_name) {
    const char *spec = g_boot_info.root_spec;
    if (!spec[0]) return FALSE;
    if (bl_str_prefix(spec, "PARTUUID="))
        return c->partuuid[0] && bl_str_eq_nocase(spec + 9, c->partuuid);
    if (bl_str_prefix(spec, "UUID="))
        return c->fs_uuid[0] && bl_str_eq_nocase(spec + 5, c->fs_uuid);
    if (bl_str_prefix(spec, "LABEL="))          /* labels compare exactly */
        return c->label[0] && bl_str_eq(spec + 6, c->label);
    if (bl_str_prefix(spec, "PARTLABEL="))
        return gpt_name && gpt_name[0] && bl_str_eq_nocase(spec + 10, gpt_name);
    /* /dev/sdXN and other kernel device names mean nothing to the firmware;
     * the kernel resolves them itself. */
    return FALSE;
}

/* Choose the ext4 partition to read kernel.elf (and the display setting)
 * from, by the ranks above, and only among partitions that actually hold
 * /boot/kernel.elf.  Sets g_root_blk and boot_info.kernel_partuuid. */
static void boot_select_root(void) {
    EFI_GUID blk_guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE *handles = NULL;
    UINTN nhandles = 0;
    bl_cand_t *cands = NULL;
    UINT32 ncand = 0;
    UINT8 *sb = NULL;

    g_root_blk = NULL;
    g_boot_info.kernel_partuuid[0] = 0;
    if (EFI_ERROR(uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                                    &blk_guid, NULL, &nhandles, &handles)) || !handles)
        return;
    if (EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                    sizeof(bl_cand_t) * BL_MAX_CANDIDATES,
                                    (VOID **)&cands)) || !cands ||
        EFI_ERROR(uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, 1024,
                                    (VOID **)&sb)) || !sb) {
        if (cands) uefi_call_wrapper(BS->FreePool, 1, cands);
        uefi_call_wrapper(BS->FreePool, 1, handles);
        return;
    }

    for (UINTN h = 0; h < nhandles && ncand < BL_MAX_CANDIDATES; h++) {
        EFI_BLOCK_IO_PROTOCOL *blk = NULL;
        EFI_DEVICE_PATH *dp = NULL;
        bl_hd_node_t hd;
        UINTN prefix = 0;
        e4_ctx c;

        if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[h],
                                        &blk_guid, (VOID **)&blk)) || !blk)
            continue;
        if (EFI_ERROR(e4_ctx_open(blk, &c, sb)))
            continue;                               /* not ext4 */
        e4_ctx_close(&c);

        bl_cand_t *cd = &cands[ncand];
        cd->blk = blk;
        cd->rank = BL_RANK_ANY;
        cd->partno = 0;
        cd->partuuid[0] = 0;
        bl_uuid_to_str(sb + 104, cd->fs_uuid);       /* s_uuid */
        bl_mem_copy(cd->label, sb + 120, 16);        /* s_volume_name */
        cd->label[16] = 0;

        const char *gpt_name = NULL;
        BOOLEAN is_root_type = FALSE, same_disk = FALSE;
        if (!EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[h],
                                         &DevicePathProtocol, (VOID **)&dp)) && dp &&
            bl_dp_find_hd(dp, &prefix, &hd)) {
            cd->partno = hd.PartitionNumber;
            bl_hd_partuuid(&hd, cd->partuuid);
            same_disk = bl_dp_on_loader_disk(dp, TRUE);
            if (same_disk && g_loader_gpt.valid && hd.SignatureType == BL_HD_SIG_GUID) {
                for (UINT32 i = 0; i < g_loader_gpt.nr; i++) {
                    if (!bl_mem_eq(g_loader_gpt.uuid[i], hd.Signature, 16)) continue;
                    is_root_type = bl_mem_eq(g_loader_gpt.type[i],
                                             g_gpt_type_root_x86_64, 16);
                    gpt_name = g_loader_gpt.name[i];
                    break;
                }
            }
        }
        if (bl_root_spec_matches(cd, gpt_name))
            cd->rank = BL_RANK_ROOT_SPEC;
        else if (is_root_type)
            cd->rank = BL_RANK_ROOT_TYPE;
        else if (same_disk)
            cd->rank = BL_RANK_LOADER_DISK;

        serial_puts("boot-root: ext4 candidate part=");
        serial_putdec(cd->partno);
        serial_puts(" PARTUUID=");
        serial_puts(cd->partuuid[0] ? cd->partuuid : "-");
        serial_puts(" UUID=");
        serial_puts(cd->fs_uuid);
        serial_puts(" rank=");
        serial_putdec(cd->rank);
        serial_puts(is_root_type ? " (x86-64 root type)" : "");
        serial_puts(same_disk ? " (loader disk)\n" : "\n");
        ncand++;
    }

    /* Best rank first, firmware order within a rank; the first candidate
     * that holds /boot/kernel.elf is the root. */
    for (UINT32 rank = BL_RANK_ROOT_SPEC; rank < BL_RANK_NONE && !g_root_blk; rank++) {
        for (UINT32 i = 0; i < ncand && !g_root_blk; i++) {
            if (cands[i].rank != rank) continue;
            e4_ctx c;
            UINT8 inode[256];
            if (EFI_ERROR(e4_ctx_open(cands[i].blk, &c, NULL))) continue;
            UINT32 ino = e4_resolve(&c, "/boot/kernel.elf", inode);
            e4_ctx_close(&c);
            if (!ino) continue;
            g_root_blk = cands[i].blk;
            bl_str_copy(g_boot_info.kernel_partuuid, sizeof(g_boot_info.kernel_partuuid),
                        cands[i].partuuid);
            serial_puts("boot-root: selected part=");
            serial_putdec(cands[i].partno);
            serial_puts(" PARTUUID=");
            serial_puts(cands[i].partuuid[0] ? cands[i].partuuid : "-");
            serial_puts(rank == BL_RANK_ROOT_SPEC ? " (root= on the command line)\n" :
                        rank == BL_RANK_ROOT_TYPE ? " (x86-64 root type on the loader disk)\n" :
                        rank == BL_RANK_LOADER_DISK ? " (ext4 on the loader disk; partition type is not the x86-64 root type)\n" :
                        " (first ext4 partition found: no loader identity match)\n");
            Print(L"Root partition: %d, PARTUUID=%a\r\n", (int)cands[i].partno,
                  cands[i].partuuid[0] ? cands[i].partuuid : "-");
        }
    }
    if (!g_root_blk)
        serial_puts("boot-root: no ext4 partition with /boot/kernel.elf found\n");
    if (g_boot_info.root_spec[0] && ncand) {
        BOOLEAN matched = FALSE;
        for (UINT32 i = 0; i < ncand; i++)
            if (cands[i].rank == BL_RANK_ROOT_SPEC) matched = TRUE;
        if (!matched)
            serial_puts("boot-root: root= names no ext4 partition the firmware can see "
                        "(the kernel resolves it again with its own drivers)\n");
    }

    uefi_call_wrapper(BS->FreePool, 1, sb);
    uefi_call_wrapper(BS->FreePool, 1, cands);
    uefi_call_wrapper(BS->FreePool, 1, handles);
}

/* Load the file at `path' (at most `max_size' bytes) from the ext4 root into
 * a freshly allocated pool buffer: /boot/kernel.elf, and the saved display
 * setting before it.  Once boot_select_root() has chosen the root partition
 * only that partition is read -- a display setting or a kernel from another
 * disk's installation must never be mixed into this boot.  Without a choice
 * (no partition held /boot/kernel.elf when it looked) every ext4 partition
 * is tried in firmware order, as this loader always did. */
static EFI_STATUS try_load_file_ext4(const char *path, UINT64 max_size,
                                     VOID **out_buf, UINTN *out_size) {
    if (g_root_blk) {
        e4_ctx c;
        EFI_STATUS st = e4_ctx_open(g_root_blk, &c, NULL);
        if (EFI_ERROR(st)) return st;
        st = e4_load_file(&c, path, max_size, out_buf, out_size);
        e4_ctx_close(&c);
        return st;
    }

    EFI_GUID blk_guid = BLOCK_IO_PROTOCOL;
    EFI_HANDLE *handles = NULL;
    UINTN nhandles = 0;
    EFI_STATUS st = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
                                      &blk_guid, NULL, &nhandles, &handles);
    if (EFI_ERROR(st) || !handles) return EFI_NOT_FOUND;

    EFI_STATUS result = EFI_NOT_FOUND;
    for (UINTN h = 0; h < nhandles && EFI_ERROR(result); h++) {
        EFI_BLOCK_IO_PROTOCOL *blk = NULL;
        e4_ctx c;
        if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, handles[h],
                                        &blk_guid, (VOID **)&blk)) || !blk)
            continue;
        if (EFI_ERROR(e4_ctx_open(blk, &c, NULL)))
            continue;
        result = e4_load_file(&c, path, max_size, out_buf, out_size);
        e4_ctx_close(&c);
    }

    uefi_call_wrapper(BS->FreePool, 1, handles);
    return result;
}

/* ===========================================================================
 *  Display mode selection
 * =========================================================================== */

/* CPUID.1:ECX[31]: the processor is a hypervisor's virtual one. */
static BOOLEAN under_hypervisor(void) {
    UINT32 a = 1, b, c = 0, d;

    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)b; (void)d;
    return (c >> 31) & 1;
}

/* Parse decimal digits at *p (before end); 0 digits gives FALSE. */
static BOOLEAN parse_dec(const char **p, const char *end, UINT32 *out) {
    UINT32 v = 0;
    int digits = 0;

    while (*p < end && **p >= '0' && **p <= '9' && digits < 6) {
        v = v * 10 + (UINT32)(**p - '0');
        (*p)++;
        digits++;
    }
    *out = v;
    return digits > 0;
}

/* The saved display setting: the "resolution=WIDTHxHEIGHT" line of
 * DISPLAY_CONF_PATH, written by the Display settings dialog.  FALSE when
 * there is no such file or line, or the size is implausible. */
static BOOLEAN read_saved_resolution(UINT32 *w, UINT32 *h) {
    static const char key[] = "resolution=";
    VOID *buf = NULL;
    UINTN size = 0;
    BOOLEAN ok = FALSE;

    if (EFI_ERROR(try_load_file_ext4(DISPLAY_CONF_PATH, 4096, &buf, &size)) || !buf)
        return FALSE;
    const char *p = (const char *)buf, *end = p + size;
    while (p < end && !ok) {
        const char *line = p, *le;
        UINT32 vw, vh;
        UINTN k = 0;

        while (p < end && *p != '\n') p++;
        le = p;
        if (p < end) p++;
        while (key[k] && line + k < le && line[k] == key[k]) k++;
        if (key[k]) continue;
        const char *q = line + k;
        if (!parse_dec(&q, le, &vw) || q >= le || (*q != 'x' && *q != 'X')) continue;
        q++;
        if (!parse_dec(&q, le, &vh)) continue;
        if (vw < 640 || vh < 480 || vw > 16384 || vh > 16384) continue;
        *w = vw;
        *h = vh;
        ok = TRUE;
    }
    uefi_call_wrapper(BS->FreePool, 1, buf);
    return ok;
}

/* A GOP mode this can hand on: a 32-bit pixel format.  Some firmware (VMware
 * SVGA, for one) exposes its larger modes only as PixelBitMask with
 * ReservedMask=0, so the channel masks need not cover all 32 bits; any
 * bitmask mode whose three color channels are non-zero and do not overlap
 * is taken. */
static BOOLEAN gop_mode_usable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi) {
    if (mi->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
        mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
        return TRUE;
    if (mi->PixelFormat == PixelBitMask) {
        UINT32 r = mi->PixelInformation.RedMask;
        UINT32 g = mi->PixelInformation.GreenMask;
        UINT32 b = mi->PixelInformation.BlueMask;

        return r && g && b && !(r & g) && !(r & b) && !(g & b);
    }
    return FALSE;
}

/* The number of the GOP mode of exactly w x h, or -1. */
static INT32 gop_find_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, UINT32 w, UINT32 h) {
    for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        UINTN mi_size;

        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &mi_size, &mi)))
            continue;
        if (gop_mode_usable(mi) && mi->HorizontalResolution == w &&
            mi->VerticalResolution == h)
            return (INT32)mode;
    }
    return -1;
}

// UEFI bootloader entry point
__attribute__((no_stack_protector))
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    extern void boot_init_canary(void);
    boot_init_canary();   /* randomise __stack_chk_guard before any other call */

    EFI_STATUS status;
    UINTN kernel_size;
    VOID *kernel_buffer;
    Elf64_Ehdr *elf_header;
    Elf64_Phdr *program_headers;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    
    // Initialize GNU-EFI library
    InitializeLib(ImageHandle, SystemTable);
    
    // Initialize serial port for debug output (VMware)
    serial_init();
    serial_puts("\n\n=== LikeOS UEFI Bootloader starting ===\n");
    
    Print(L"LikeOS Enhanced UEFI Bootloader\r\n");
    Print(L"===================================\r\n");
    Print(L"ELF64 Loader with Paging Support\r\n\r\n");

    // Dump UEFI memory map for debugging
    //dump_memory_map();
    //for (;;) {}
    
    Print(L"Loading /boot/kernel.elf from ext4 root...\r\n");

    // Which partition we were started from, the boot command line, and from
    // those the ext4 root partition: everything read from ext4 below (the
    // saved display setting, the kernel) comes from that partition alone,
    // and the kernel receives the same identity to find its root by.
    boot_identity_init(ImageHandle);
    // Started from \EFI\BOOT on an installed disk whose firmware has lost
    // (or never got) the LikeOS boot entry: put it back.
    boot_fallback_register(ImageHandle);
    boot_cmdline_init(ImageHandle);
    // The live medium's boot menu: its choice adds to the command line
    // before the root partition is chosen from it.
    boot_menu(ImageHandle);
    bl_read_loader_gpt();
    boot_select_root();

    // Get Graphics Output Protocol for framebuffer info
    status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID**)&gop);
    if (EFI_ERROR(status)) {
        Print(L"WARNING: Could not get Graphics Output Protocol: %r\r\n", status);
        // Set up fallback VGA mode info
        g_boot_info.fb_info.framebuffer_base = (void*)0xB8000;
        g_boot_info.fb_info.framebuffer_size = 4000;
        g_boot_info.fb_info.horizontal_resolution = 80;
        g_boot_info.fb_info.vertical_resolution = 25;
        g_boot_info.fb_info.pixels_per_scanline = 80;
        g_boot_info.fb_info.bytes_per_pixel = 2;
    } else {
        UINT32 saved_w = 0, saved_h = 0;
        BOOLEAN have_saved = read_saved_resolution(&saved_w, &saved_h);
        BOOLEAN vm = under_hypervisor();
        UINT32 selected_mode = gop->Mode->Mode;  // Default to current mode
        INT32 pick = -1;

        Print(L"Enumerating %d GOP modes for preferred resolution...\r\n", gop->Mode->MaxMode);
        serial_puts("GOP modes:\n");

        // Log every mode once, and note the distinct 32-bpp sizes for the
        // kernel: they are what a saved setting can name when no display
        // driver takes over from this framebuffer.
        g_boot_info.gop_mode_count = 0;
        for (UINT32 mode = 0; mode < gop->Mode->MaxMode; mode++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;
            UINTN mode_info_size;

            status = uefi_call_wrapper(gop->QueryMode, 4, gop, mode, &mode_info_size, &mode_info);
            if (EFI_ERROR(status)) continue;

            serial_puts("  mode "); serial_putdec(mode);
            serial_puts(": "); serial_putdec(mode_info->HorizontalResolution);
            serial_putc('x'); serial_putdec(mode_info->VerticalResolution);
            serial_puts(" fmt="); serial_putdec(mode_info->PixelFormat);
            if (mode_info->PixelFormat == 2) { // PixelBitMask
                serial_puts(" R="); serial_puthex(mode_info->PixelInformation.RedMask);
                serial_puts(" G="); serial_puthex(mode_info->PixelInformation.GreenMask);
                serial_puts(" B="); serial_puthex(mode_info->PixelInformation.BlueMask);
                serial_puts(" X="); serial_puthex(mode_info->PixelInformation.ReservedMask);
            }
            serial_putc('\n');

            if (gop_mode_usable(mode_info)) {
                UINT32 n = g_boot_info.gop_mode_count, k;

                for (k = 0; k < n; k++)
                    if (g_boot_info.gop_modes[k].width == mode_info->HorizontalResolution &&
                        g_boot_info.gop_modes[k].height == mode_info->VerticalResolution)
                        break;
                if (k == n && n < BOOT_GOP_MODES_MAX) {
                    g_boot_info.gop_modes[n].width = mode_info->HorizontalResolution;
                    g_boot_info.gop_modes[n].height = mode_info->VerticalResolution;
                    g_boot_info.gop_mode_count = n + 1;
                }
            }
        }

        // The size the user chose in Display settings comes first, wherever
        // this runs, and above any build-time ceiling: it was asked for.
        if (have_saved) {
            pick = gop_find_mode(gop, saved_w, saved_h);
            serial_puts("GOP: saved setting "); serial_putdec(saved_w);
            serial_putc('x'); serial_putdec(saved_h);
            serial_puts(pick >= 0 ? " offered\n" : " not offered\n");
        }

        if (pick < 0 && vm) {
            // Under a hypervisor nothing tells the guest how large the
            // display in front of the user is, and the firmware offers
            // whatever its device can scan out -- up to 7680x4320 on some.
            // So a fixed size the window is expected to hold, the one a
            // virtual machine's display driver comes up in as well
            // (kernel/dev/video/vmsvga2.c, gpu/vmwgfx/vmw_drv.c).
            pick = gop_find_mode(gop, SCREEN_VM_WIDTH, SCREEN_VM_HEIGHT);
            serial_puts(pick >= 0 ? "GOP: hypervisor, fixed size\n"
                                  : "GOP: hypervisor, fixed size not offered\n");
        } else if (pick < 0) {
            // Real hardware: a preference table, largest first.  The large
            // build tries every standard size above 1920x1200 first, so a
            // panel that can show more gets it, then the sizes this always
            // preferred.
            static const struct { UINT32 width; UINT32 height; } base[] = {
#if defined(SCREEN_LARGE)
                {1920, 1200},
                {1920, 1080},
#endif
                {1280, 800},
                {1280, 768},
                {1024, 768}
            };
            struct { UINT32 width; UINT32 height; } preferred[SCREEN_MODE_COUNT + 5];
            UINTN num_preferred = 0;

#if defined(SCREEN_LARGE)
            for (UINTN i = 0; i < SCREEN_MODE_COUNT; i++) {
                if ((UINT64)screen_mode_list[i].width * screen_mode_list[i].height <=
                    1920ULL * 1200ULL)
                    continue;
                preferred[num_preferred].width = screen_mode_list[i].width;
                preferred[num_preferred].height = screen_mode_list[i].height;
                num_preferred++;
            }
#endif
            for (UINTN i = 0; i < sizeof(base) / sizeof(base[0]); i++) {
                preferred[num_preferred].width = base[i].width;
                preferred[num_preferred].height = base[i].height;
                num_preferred++;
            }

            // Search for preferred resolutions in priority order, skipping the ones
            // the build-time ceiling (MAX_SCREEN_SIZE) rules out
            for (UINTN pref = 0; pref < num_preferred && pick < 0; pref++) {
                if (preferred[pref].width > SCREEN_MAX_WIDTH ||
                    preferred[pref].height > SCREEN_MAX_HEIGHT) {
                    serial_puts("  skipping "); serial_putdec(preferred[pref].width);
                    serial_putc('x'); serial_putdec(preferred[pref].height);
                    serial_puts(": above MAX_SCREEN_SIZE\n");
                    continue;
                }
                pick = gop_find_mode(gop, preferred[pref].width, preferred[pref].height);
            }
        }

        if (pick < 0) {
            Print(L"No preferred resolution found, using firmware default\r\n");
            serial_puts("GOP: no preferred resolution found, using firmware default\n");
        } else {
            selected_mode = (UINT32)pick;
            Print(L"Found preferred resolution (mode %d)\r\n", selected_mode);
            serial_puts("GOP: selected mode "); serial_putdec(selected_mode); serial_putc('\n');
        }

        g_boot_info.display_magic = BOOT_DISPLAY_MAGIC;
        g_boot_info.display_saved_width = have_saved ? saved_w : 0;
        g_boot_info.display_saved_height = have_saved ? saved_h : 0;
        g_boot_info.hypervisor = vm ? 1 : 0;
        
        // Set the selected mode if different from current
        if (selected_mode != gop->Mode->Mode) {
            status = uefi_call_wrapper(gop->SetMode, 2, gop, selected_mode);
            if (EFI_ERROR(status)) {
                Print(L"WARNING: SetMode(%d) failed: %r, using default\r\n", selected_mode, status);
            } else {
                Print(L"Set graphics mode to %d\r\n", selected_mode);
            }
        }
        
        // Get (possibly updated) mode information
        g_boot_info.fb_info.framebuffer_base = (void*)gop->Mode->FrameBufferBase;
        g_boot_info.fb_info.framebuffer_size = (uint32_t)gop->Mode->FrameBufferSize;
        g_boot_info.fb_info.horizontal_resolution = gop->Mode->Info->HorizontalResolution;
        g_boot_info.fb_info.vertical_resolution = gop->Mode->Info->VerticalResolution;
        g_boot_info.fb_info.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        
        // Calculate bytes per pixel based on pixel format
        switch (gop->Mode->Info->PixelFormat) {
            case PixelRedGreenBlueReserved8BitPerColor:
            case PixelBlueGreenRedReserved8BitPerColor:
                g_boot_info.fb_info.bytes_per_pixel = 4;
                break;
            case PixelBitMask:
                g_boot_info.fb_info.bytes_per_pixel = 4; // Assume 32-bit
                break;
            default:
                g_boot_info.fb_info.bytes_per_pixel = 4; // Safe default
                break;
        }
        
        Print(L"Framebuffer: 0x%lx, Size: %d bytes\r\n", g_boot_info.fb_info.framebuffer_base, g_boot_info.fb_info.framebuffer_size);
        Print(L"Resolution: %dx%d, BPP: %d\r\n", g_boot_info.fb_info.horizontal_resolution, g_boot_info.fb_info.vertical_resolution, g_boot_info.fb_info.bytes_per_pixel);
    }
    
    /* Load the kernel directly from the ext4 root partition (single GPT USB
     * disk).  This is the only supported path — there is no FAT fallback. */
    kernel_buffer = NULL;
    kernel_size = 0;
    status = try_load_file_ext4("/boot/kernel.elf", 0x10000000ULL, /* <256MB */
                                &kernel_buffer, &kernel_size);
    if (EFI_ERROR(status) || !kernel_buffer) {
        Print(L"ERROR: Could not load /boot/kernel.elf from ext4 root: %r\r\n", status);
        serial_puts("boot: FATAL - ext4 kernel load failed (no FAT fallback)\n");
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return EFI_ERROR(status) ? status : EFI_NOT_FOUND;
    }
    Print(L"Loaded kernel from ext4 root partition (%d bytes)\r\n", (int)kernel_size);
    serial_puts("boot: kernel loaded from ext4 root partition\n");

    // Parse ELF header
    elf_header = (Elf64_Ehdr*)kernel_buffer;
    
    if (!validate_elf64(elf_header)) {
        Print(L"ERROR: Invalid ELF64 file\r\n");
        uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return EFI_INVALID_PARAMETER;
    }
    
    Print(L"Valid ELF64 x86-64 kernel\r\n");
    Print(L"Entry point: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Program headers: %d\r\n", elf_header->e_phnum);
    
    // Get program headers
    program_headers = (Elf64_Phdr*)((UINT8*)kernel_buffer + elf_header->e_phoff);
    
    // Track kernel physical location for paging setup
    EFI_PHYSICAL_ADDRESS kernel_phys_addr = 0;
    UINT64 kernel_size_total = 0;
    
    // Load program segments
    for (int i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr *phdr = &program_headers[i];
        
        if (phdr->p_type == PT_LOAD) {
            Print(L"Loading segment %d: vaddr=0x%lx, size=%ld\r\n", 
                  i, phdr->p_vaddr, phdr->p_memsz);
            
            // Allocate memory for this segment using AllocateAnyPages
            UINTN pages = (phdr->p_memsz + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS segment_addr = 0;
            
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, 
                                       EfiLoaderCode, pages, &segment_addr);
            if (EFI_ERROR(status)) {
                Print(L"ERROR: Could not allocate memory: %r\r\n", status);
                uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
                Print(L"System halted. Press any key to continue...\r\n");
                uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
                EFI_INPUT_KEY key;
                uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
                return status;
            }
            
            // Track the kernel's physical location (use first segment)
            if (kernel_phys_addr == 0) {
                kernel_phys_addr = segment_addr;
            }
            
            Print(L"Allocated at physical address: 0x%lx\r\n", segment_addr);
            
            // Calculate total kernel size
            UINT64 segment_end = segment_addr + phdr->p_memsz;
            if (segment_end > (kernel_phys_addr + kernel_size_total)) {
                kernel_size_total = segment_end - kernel_phys_addr;
            }
            
            // Copy segment data
            if (phdr->p_filesz > 0) {
                uefi_call_wrapper(BS->CopyMem, 3, (VOID*)segment_addr, 
                                  (UINT8*)kernel_buffer + phdr->p_offset, phdr->p_filesz);
            }
            
            // Zero out any extra space (BSS)
            if (phdr->p_memsz > phdr->p_filesz) {
                uefi_call_wrapper(BS->SetMem, 3, (VOID*)(segment_addr + phdr->p_filesz), 
                                  phdr->p_memsz - phdr->p_filesz, 0);
            }
        }
    }
    
    Print(L"Kernel physical location: 0x%lx (total size: %ld bytes)\r\n", 
          kernel_phys_addr, kernel_size_total);
    
    Print(L"Kernel loaded successfully!\r\n");
    Print(L"Setting up paging and exiting UEFI...\r\n");
    
    // NOTE: deliberately do NOT free kernel_buffer here.  elf_header points
    // into it and elf_header->e_entry is dereferenced below (after
    // ExitBootServices) to jump to the kernel; freeing it would let the pool
    // be reused and zero the entry point.  ExitBootServices follows almost
    // immediately and the kernel reclaims all memory, so the buffer is not
    // leaked in any meaningful sense.

    // Get memory map for exit boot services
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_key, descriptor_size;
    UINT32 descriptor_version;
    UINTN memory_map_cap;
    
    // Get memory map size
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                               &map_key, &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL) {
        Print(L"ERROR: Unexpected error getting memory map size: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Allocate buffer for memory map
    // Extra room: the trampoline, the page tables and the SMP trampoline
    // are allocated after the map is read, and ExitBootServices reads it
    // once more into this same buffer.
    memory_map_size += 16 * descriptor_size;
    memory_map_cap = memory_map_size;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, 
                               memory_map_size, (VOID**)&memory_map);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory map: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get the actual memory map
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                               &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get memory map: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, memory_map);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Store memory map info for kernel
    {
        UINTN num_entries = memory_map_size / descriptor_size;
        UINTN dropped;

        Print(L"Processing %lu memory map entries...\r\n", num_entries);
        dropped = bl_store_mem_info(memory_map, memory_map_size, descriptor_size, 1);
        Print(L"Stored %d memory entries, total usable: %lu MB\r\n", 
              g_boot_info.mem_info.entry_count, 
              g_boot_info.mem_info.total_memory / (1024 * 1024));
        if (dropped)
            Print(L"WARNING: %lu memory map entries did not fit\r\n", dropped);
    }

    // The runtime services for the kernel (boot variables, clock).
    bl_efi_tables();
    bl_collect_runtime(memory_map, memory_map_size, descriptor_size, descriptor_version);
    serial_puts("EFI runtime: ");
    serial_putdec(g_boot_info.efi_rt_count);
    serial_puts(" runtime regions, RuntimeServices at ");
    serial_puthex(g_boot_info.efi_runtime_services);
    serial_puts(g_boot_info.efi_rt_dropped ? " (some regions did not fit)\n" : "\n");
    
    // Find ACPI RSDP from UEFI Configuration Tables
    g_boot_info.rsdp_address = 0;
    {
        // ACPI 2.0+ GUID: 8868e871-e4f1-11d3-bc22-0080c73c8881
        EFI_GUID acpi20_guid = {0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
        // ACPI 1.0 GUID: eb9d2d30-2d88-11d3-9a16-0090273fc14d
        EFI_GUID acpi10_guid = {0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
        
        for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *table = &ST->ConfigurationTable[i];
            // Check for ACPI 2.0+ first (preferred)
            if (table->VendorGuid.Data1 == acpi20_guid.Data1 &&
                table->VendorGuid.Data2 == acpi20_guid.Data2 &&
                table->VendorGuid.Data3 == acpi20_guid.Data3) {
                g_boot_info.rsdp_address = (uint64_t)table->VendorTable;
                serial_puts("Found ACPI 2.0+ RSDP at 0x");
                serial_puthex(g_boot_info.rsdp_address);
                serial_puts("\n");
                Print(L"Found ACPI 2.0+ RSDP at 0x%lx\r\n", g_boot_info.rsdp_address);
                break;
            }
            // Check for ACPI 1.0
            if (table->VendorGuid.Data1 == acpi10_guid.Data1 &&
                table->VendorGuid.Data2 == acpi10_guid.Data2 &&
                table->VendorGuid.Data3 == acpi10_guid.Data3) {
                if (g_boot_info.rsdp_address == 0) {
                    g_boot_info.rsdp_address = (uint64_t)table->VendorTable;
                    serial_puts("Found ACPI 1.0 RSDP at 0x");
                    serial_puthex(g_boot_info.rsdp_address);
                    serial_puts("\n");
                    Print(L"Found ACPI 1.0 RSDP at 0x%lx\r\n", g_boot_info.rsdp_address);
                }
            }
        }
        if (g_boot_info.rsdp_address == 0) {
            serial_puts("WARNING: ACPI RSDP not found in UEFI tables!\n");
            Print(L"WARNING: ACPI RSDP not found in UEFI tables!\r\n");
        }
    }

    // SMBIOS entry points from the UEFI configuration tables: on machines
    // without a legacy BIOS area they are the only way to find the
    // firmware's identification (vendor, product), which drivers use to
    // pick per-machine settings.
    g_boot_info.smbios_address = 0;
    g_boot_info.smbios3_address = 0;
    {
        // SMBIOS: eb9d2d31-2d88-11d3-9a16-0090273fc14d
        EFI_GUID smbios_guid = {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
        // SMBIOS 3: f2fd1544-9794-4a2c-992e-e5bbcf20e394
        EFI_GUID smbios3_guid = {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};

        for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
            EFI_CONFIGURATION_TABLE *table = &ST->ConfigurationTable[i];
            EFI_GUID *g = &table->VendorGuid;
            int is_smbios = 1, is_smbios3 = 1;

            for (int b = 0; b < 8; b++) {
                if (g->Data4[b] != smbios_guid.Data4[b])
                    is_smbios = 0;
                if (g->Data4[b] != smbios3_guid.Data4[b])
                    is_smbios3 = 0;
            }
            if (g->Data1 != smbios_guid.Data1 || g->Data2 != smbios_guid.Data2 ||
                g->Data3 != smbios_guid.Data3)
                is_smbios = 0;
            if (g->Data1 != smbios3_guid.Data1 || g->Data2 != smbios3_guid.Data2 ||
                g->Data3 != smbios3_guid.Data3)
                is_smbios3 = 0;
            if (is_smbios)
                g_boot_info.smbios_address = (uint64_t)table->VendorTable;
            if (is_smbios3)
                g_boot_info.smbios3_address = (uint64_t)table->VendorTable;
        }
        g_boot_info.smbios_magic = BOOT_SMBIOS_MAGIC;
        if (g_boot_info.smbios3_address || g_boot_info.smbios_address) {
            serial_puts("Found SMBIOS entry point at 0x");
            serial_puthex(g_boot_info.smbios3_address ? g_boot_info.smbios3_address :
                                                        g_boot_info.smbios_address);
            serial_puts("\n");
        }
    }
    
    // Step 1: Allocate trampoline below kernel space
    Print(L"Allocating trampoline below kernel space...\r\n");
    status = allocate_trampoline(kernel_phys_addr);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, memory_map);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Step 2: Set up higher half paging (including identity mapping for low memory)
    Print(L"Setting up higher half kernel paging...\r\n");
    
    // Initialize and allocate all page tables from UEFI at low addresses
    status = init_page_tables();
    if (EFI_ERROR(status)) {
        Print(L"Failed to initialize page tables\r\n");
        return status;
    }
    
    // Reserve memory for SMP AP trampoline (must be done before ExitBootServices)
    status = reserve_smp_trampoline();
    if (EFI_ERROR(status)) {
        Print(L"WARNING: SMP trampoline reservation failed, SMP may not work\r\n");
        // Continue anyway - single CPU will still work
    }
    
    setup_higher_half_paging(kernel_phys_addr, kernel_size_total);
    
    // Read system time from UEFI BEFORE ExitBootServices
    // This is critical for platforms where CMOS RTC is not accessible after boot
    // (e.g., Intel Alder Lake with eSPI where I/O ports 0x70/0x71 return 0xFF)
    read_boot_time();
    
    Print(L"About to exit boot services. Kernel entry: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Trampoline at: 0x%lx\r\n", trampoline_addr);

    // The map as it is now, after every allocation above (the page tables,
    // the trampolines): its key is the one ExitBootServices checks, and it
    // is the map the kernel gets.
    status = bl_read_final_memory_map(&memory_map, &memory_map_size,
                                      &memory_map_cap, &map_key,
                                      &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get the final memory map: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    bl_store_mem_info(memory_map, memory_map_size, descriptor_size, 0);
    bl_collect_runtime(memory_map, memory_map_size, descriptor_size,
                       descriptor_version);

    // Step 3: Exit boot services
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        // Try one more time in case the memory map changed: the whole
        // buffer, which keeps BL_MMAP_SLACK descriptors of room for what
        // the firmware added since.  Only GetMemoryMap and
        // ExitBootServices may be called now.
        memory_map_size = memory_map_cap;
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                                   memory_map, &map_key, &descriptor_size,
                                   &descriptor_version);
        if (!EFI_ERROR(status))
            status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle,
                                       map_key);
        if (!EFI_ERROR(status)) {
            // The map the firmware left the machine with: the kernel's
            // memory map and the runtime regions from it (memory work
            // only, no firmware call).
            bl_store_mem_info(memory_map, memory_map_size, descriptor_size, 0);
            bl_collect_runtime(memory_map, memory_map_size, descriptor_size,
                               descriptor_version);
            serial_puts("ExitBootServices on the re-read map: ");
            serial_putdec(g_boot_info.mem_info.entry_count);
            serial_puts(" memory entries, ");
            serial_putdec(g_boot_info.mem_info.total_memory / (1024 * 1024));
            serial_puts(" MB usable\n");
        }
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Could not exit boot services: %r\r\n", status);
            Print(L"System halted. Press any key to continue...\r\n");
            uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
            EFI_INPUT_KEY key;
            uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
            return status;
        }
    }
    
    // After ExitBootServices, UEFI services are no longer available
    
    // Step 4: Call the copied trampoline function in low memory
    // The trampoline will:
    // - Load CR3 with our page tables  
    // - Jump to the kernel's higher half virtual address
    trampoline_func_t trampoline = (trampoline_func_t)trampoline_addr;
    trampoline(elf_header->e_entry, &g_boot_info, g_pml4_addr);
    
    // Should never reach here
    return EFI_SUCCESS;
}
