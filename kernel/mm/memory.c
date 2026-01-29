// LikeOS-64 Memory Management - Implementation
// Complete Physical Memory Manager, Virtual Memory Manager, and Kernel Heap Allocator

#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/slab.h"

// Enable SLAB allocator (comment out to use legacy fixed-size heap)
#define USE_SLAB_ALLOCATOR

// Magic numbers for heap validation
#define HEAP_MAGIC_ALLOCATED    0xDEADBEEF
#define HEAP_MAGIC_FREE         0xFEEDFACE
#define HEAP_MAGIC_HEADER       0xABCDEF12

// Memory management state
static struct {
    // Physical memory management
    uint32_t* physical_bitmap;      // Bitmap for physical pages
    uint64_t total_pages;           // Total number of pages
    uint64_t free_pages;            // Number of free pages
    uint64_t bitmap_size;           // Size of bitmap in bytes
    uint64_t memory_start;          // Start of manageable memory
    uint64_t memory_end;            // End of manageable memory
    
    // Page reference counting for COW
    uint16_t* page_refcounts;       // Reference count per physical page
    uint64_t refcount_array_size;   // Size in bytes
    
    // Virtual memory management
    uint64_t* pml4_table;           // Page Map Level 4 table
    uint64_t next_virtual_addr;     // Next available virtual address
    
    // Heap management
    heap_block_t* heap_start;       // Start of heap
    heap_block_t* heap_end;         // End of heap
    heap_block_t* free_list;        // Free blocks list
    uint64_t heap_size;             // Total heap size
    uint64_t heap_used;             // Used heap memory
    uint32_t allocation_count;      // Number of allocations
    uint32_t deallocation_count;    // Number of deallocations
    
    // Statistics
    memory_stats_t stats;           // Memory statistics
} mm_state = {0};

// UEFI memory map storage - saved from boot_info for later use
static memory_map_info_t g_uefi_memory_map = {0};

// I/O port functions
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Utility functions (non-static, declared in memory.h)
void mm_memset(void* dest, int val, size_t len) {
    uint8_t* ptr = (uint8_t*)dest;
    while (len--) {
        *ptr++ = val;
    }
}

void mm_memcpy(void* dest, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (len--) {
        *d++ = *s++;
    }
}

// Get current CR3 (page table base)
static uint64_t get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Set CR3 (page table base)
static void set_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// Flush TLB for specific address
void mm_flush_tlb(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

// Flush all TLB entries
void mm_flush_all_tlb(void) {
    uint64_t cr3 = get_cr3();
    set_cr3(cr3);
}

// Get dynamic kernel heap start address
uint64_t mm_get_kernel_heap_start(void) {
    // Align kernel_end to page boundary and add some padding
    uint64_t kernel_end_addr = (uint64_t)kernel_end;
    return PAGE_ALIGN(kernel_end_addr);
}

// PHYSICAL MEMORY MANAGER IMPLEMENTATION

// Forward declarations
static bool is_page_allocated(uint64_t page);
static void set_page_bit(uint64_t page);
static void clear_page_bit(uint64_t page);

// Page table pool - Reserved from start of physical memory range.
// All page tables are allocated from this pool, which is outside the bitmap range.
// Size is calculated dynamically based on RAM: ~1 PT page per 2MB of RAM (worst case)
static uint64_t pt_pool_size = 0;       // Calculated at runtime
static uint64_t pt_pool_phys_start = 0;
static uint64_t pt_pool_next = 0;
static int pt_pool_initialized = 0;

// ============================================================================
// EARLY PAGE TABLE WALKING (before full VM is initialized)
// Walk the bootloader's page tables to find physical addresses for virtual addresses
// This does NOT use mm_get_page_table (which needs allocation) - read only!
// ============================================================================

// Get physical address for virtual address by walking page tables (read-only, early boot)
// Returns 0 if the address is not mapped
static uint64_t early_virt_to_phys(uint64_t virtual_addr) {
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    // Get PML4 from CR3
    uint64_t pml4_phys = get_cr3() & ~0xFFF;
    uint64_t* pml4 = (uint64_t*)phys_to_virt(pml4_phys);
    
    // Check PML4 entry
    uint64_t pml4e = pml4[pml4_index];
    if (!(pml4e & PAGE_PRESENT)) {
        return 0;  // Not mapped
    }
    
    // Get PDPT
    uint64_t pdpt_phys = pml4e & ~0xFFF;
    uint64_t* pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    
    // Check PDPT entry
    uint64_t pdpte = pdpt[pdpt_index];
    if (!(pdpte & PAGE_PRESENT)) {
        return 0;  // Not mapped
    }
    
    // Check for 1GB page
    if (pdpte & PAGE_SIZE_FLAG) {
        // 1GB page - physical address is pdpte[51:30] + vaddr[29:0]
        return (pdpte & 0x000FFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFF);
    }
    
    // Get PD
    uint64_t pd_phys = pdpte & ~0xFFF;
    uint64_t* pd = (uint64_t*)phys_to_virt(pd_phys);
    
    // Check PD entry
    uint64_t pde = pd[pd_index];
    if (!(pde & PAGE_PRESENT)) {
        return 0;  // Not mapped
    }
    
    // Check for 2MB page
    if (pde & PAGE_SIZE_FLAG) {
        // 2MB page - physical address is pde[51:21] + vaddr[20:0]
        return (pde & 0x000FFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFF);
    }
    
    // Get PT
    uint64_t pt_phys = pde & ~0xFFF;
    uint64_t* pt = (uint64_t*)phys_to_virt(pt_phys);
    
    // Check PT entry
    uint64_t pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) {
        return 0;  // Not mapped
    }
    
    // 4KB page - physical address is pte[51:12] + vaddr[11:0]
    return (pte & 0x000FFFFFFFFFF000ULL) | (virtual_addr & 0xFFF);
}

// Reserve a physical page in the bitmap (mark as allocated)
// Used during init to mark bootloader-mapped pages as reserved
static void reserve_physical_page(uint64_t phys_addr) {
    if (phys_addr < mm_state.memory_start || phys_addr >= mm_state.memory_end) {
        return;  // Outside managed range
    }
    
    uint64_t page = (phys_addr - mm_state.memory_start) / PAGE_SIZE;
    if (page < mm_state.total_pages && !is_page_allocated(page)) {
        set_page_bit(page);
        mm_state.free_pages--;
    }
}

// Walk all kernel virtual addresses and reserve their backing physical pages
// This ensures we never allocate physical pages that are already in use by the kernel
static void reserve_bootloader_mapped_pages(void) {
    uint64_t kernel_start_virt = KERNEL_OFFSET;
    uint64_t heap_start_virt = mm_get_kernel_heap_start();
    uint64_t heap_end_virt = heap_start_virt + KERNEL_HEAP_SIZE;
    
    // Also reserve pages for bitmap and refcount array
    uint64_t bitmap_end_virt = (uint64_t)mm_state.physical_bitmap + mm_state.bitmap_size;
    uint64_t refcount_end_virt = (uint64_t)mm_state.page_refcounts + mm_state.refcount_array_size;
    
    // Find the highest virtual address we need to scan
    uint64_t scan_end = refcount_end_virt;
    if (bitmap_end_virt > scan_end) scan_end = bitmap_end_virt;
    if (heap_end_virt > scan_end) scan_end = heap_end_virt;
    
    uint64_t pages_reserved = 0;
    
    kprintf("  Scanning bootloader mappings: 0x%lx - 0x%lx\n",
            kernel_start_virt, scan_end);
    
    // Walk through all virtual pages in the kernel space
    for (uint64_t virt = kernel_start_virt; virt < scan_end; virt += PAGE_SIZE) {
        uint64_t phys = early_virt_to_phys(virt);
        if (phys != 0) {
            // Check if this physical page is in our managed range
            if (phys >= mm_state.memory_start && phys < mm_state.memory_end) {
                reserve_physical_page(phys);
                pages_reserved++;
            }
        }
    }
    
    kprintf("  Reserved %lu bootloader-mapped pages in managed range\n", pages_reserved);
}

// ============================================================================
// UEFI MEMORY MAP HANDLING
// Reserve all physical memory regions that are marked as non-usable by UEFI.
// This prevents us from allocating memory that is used by:
//   - UEFI Runtime Services (firmware callbacks)
//   - ACPI tables and NVS data
//   - Memory-mapped I/O regions
//   - Reserved firmware memory
// ============================================================================

static const char* mm_get_efi_memory_type_name(uint32_t type) {
    switch (type) {
        case EFI_RESERVED_MEMORY_TYPE:        return "Reserved";
        case EFI_LOADER_CODE:                 return "LoaderCode";
        case EFI_LOADER_DATA:                 return "LoaderData";
        case EFI_BOOT_SERVICES_CODE:          return "BootServicesCode";
        case EFI_BOOT_SERVICES_DATA:          return "BootServicesData";
        case EFI_RUNTIME_SERVICES_CODE:       return "RuntimeServicesCode";
        case EFI_RUNTIME_SERVICES_DATA:       return "RuntimeServicesData";
        case EFI_CONVENTIONAL_MEMORY:         return "ConventionalMemory";
        case EFI_UNUSABLE_MEMORY:             return "UnusableMemory";
        case EFI_ACPI_RECLAIM_MEMORY:         return "ACPIReclaimMemory";
        case EFI_ACPI_MEMORY_NVS:             return "ACPIMemoryNVS";
        case EFI_MEMORY_MAPPED_IO:            return "MemoryMappedIO";
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE: return "MMIOPortSpace";
        case EFI_PAL_CODE:                    return "PALCode";
        case EFI_PERSISTENT_MEMORY:           return "PersistentMemory";
        default:                              return "Unknown";
    }
}

static void reserve_uefi_memory_regions(void) {
    if (g_uefi_memory_map.entry_count == 0) {
        kprintf("  WARNING: No UEFI memory map available!\n");
        return;
    }
    
    uint64_t pages_reserved = 0;
    uint64_t reserved_regions = 0;
    
    kprintf("  Reserving UEFI non-usable memory regions:\n");
    
    for (uint32_t i = 0; i < g_uefi_memory_map.entry_count; i++) {
        memory_map_entry_t* entry = &g_uefi_memory_map.entries[i];
        
        // Skip usable memory types - these are safe to allocate from
        if (mm_is_usable_memory_type(entry->type)) {
            continue;
        }
        
        // This is a reserved/non-usable region - mark all pages as allocated
        uint64_t region_start = entry->physical_start;
        uint64_t region_end = region_start + (entry->number_of_pages * PAGE_SIZE);
        
        // Only process regions that overlap with our managed memory range
        if (region_end <= mm_state.memory_start || region_start >= mm_state.memory_end) {
            continue;  // Region is outside our managed range
        }
        
        // Clamp to our managed range
        if (region_start < mm_state.memory_start) {
            region_start = mm_state.memory_start;
        }
        if (region_end > mm_state.memory_end) {
            region_end = mm_state.memory_end;
        }
        
        uint64_t region_pages = (region_end - region_start) / PAGE_SIZE;
        
        kprintf("    [%02u] 0x%lx-0x%lx (%s): %lu pages\n",
                i, entry->physical_start, 
                entry->physical_start + (entry->number_of_pages * PAGE_SIZE),
                mm_get_efi_memory_type_name(entry->type),
                region_pages);
        
        // Reserve each page in this region
        for (uint64_t phys = region_start; phys < region_end; phys += PAGE_SIZE) {
            reserve_physical_page(phys);
            pages_reserved++;
        }
        reserved_regions++;
    }
    
    kprintf("  Reserved %lu pages across %lu UEFI reserved regions\n",
            pages_reserved, reserved_regions);
}

// Initialize memory manager with UEFI memory map from boot_info
// This should be called before mm_initialize_physical_memory if boot_info is available
void mm_initialize_from_boot_info(boot_info_t* boot_info) {
    if (!boot_info) {
        kprintf("WARNING: No boot_info provided, UEFI memory map not available\n");
        return;
    }
    
    // Copy the memory map to our static storage
    g_uefi_memory_map.entry_count = boot_info->mem_info.entry_count;
    g_uefi_memory_map.descriptor_size = boot_info->mem_info.descriptor_size;
    g_uefi_memory_map.total_memory = boot_info->mem_info.total_memory;
    
    for (uint32_t i = 0; i < boot_info->mem_info.entry_count && i < MAX_MEMORY_MAP_ENTRIES; i++) {
        g_uefi_memory_map.entries[i] = boot_info->mem_info.entries[i];
    }
    
    kprintf("Stored UEFI memory map: %u entries, %lu MB total\n",
            g_uefi_memory_map.entry_count,
            g_uefi_memory_map.total_memory / (1024 * 1024));
}

// Find first free bit in bitmap
static uint64_t find_free_page(void) {
    for (uint64_t i = 0; i < mm_state.bitmap_size / sizeof(uint32_t); i++) {
        if (mm_state.physical_bitmap[i] != 0xFFFFFFFF) {
            // Found a uint32_t with free bits
            for (int bit = 0; bit < 32; bit++) {
                if (!(mm_state.physical_bitmap[i] & (1 << bit))) {
                    return i * 32 + bit;
                }
            }
        }
    }
    return (uint64_t)-1; // No free pages
}

// Set bit in bitmap
static void set_page_bit(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        mm_state.physical_bitmap[index] |= (1 << bit);
    }
}

// Clear bit in bitmap
static void clear_page_bit(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        mm_state.physical_bitmap[index] &= ~(1 << bit);
    }
}

// Check if bit is set in bitmap
static bool is_page_allocated(uint64_t page) {
    uint64_t index = page / 32;
    uint64_t bit = page % 32;
    if (index < mm_state.bitmap_size / sizeof(uint32_t)) {
        return mm_state.physical_bitmap[index] & (1 << bit);
    }
    return true; // Assume allocated if out of range
}

// Initialize physical memory manager
void mm_initialize_physical_memory(uint64_t memory_size) {
    kprintf("Initializing Physical Memory Manager...\n");
    
    // =========================================================================
    // KEY INSIGHT: The bootloader uses AllocateAnyPages which allocates physical
    // pages at ARBITRARY locations, not contiguously after kernel_end.
    // 
    // We CANNOT assume physical addresses are linear with virtual addresses.
    // Instead, we:
    //   1. Start our managed physical memory well past any bootloader allocations
    //   2. Walk the page tables to find which physical pages are actually in use
    //   3. Mark those pages as reserved in our bitmap
    // =========================================================================
    
    uint64_t heap_start_virt = mm_get_kernel_heap_start();
    
    // Calculate PT pool size dynamically based on RAM:
    // - Each PT page can map 512 * 4KB = 2MB of memory
    // - For worst case (all 4KB pages), need memory_size / 2MB page tables
    // - Plus PD pages (memory_size / 1GB) and PDPT pages (small constant)
    // - Add 25% overhead for fragmentation and dynamic allocations
    uint64_t pt_pages_needed = memory_size / (2 * 1024 * 1024);  // 1 PT per 2MB
    uint64_t pd_pages_needed = memory_size / (1024 * 1024 * 1024) + 1;  // 1 PD per 1GB
    uint64_t pdpt_pages_needed = 4;  // Up to 512GB coverage
    pt_pool_size = pt_pages_needed + pd_pages_needed + pdpt_pages_needed;
    pt_pool_size = (pt_pool_size * 5) / 4;  // Add 25% overhead
    if (pt_pool_size < 256) pt_pool_size = 256;  // Minimum 1MB pool
    
    // Start at 32MB
    pt_pool_phys_start = 32 * 1024 * 1024;
    uint64_t pt_pool_size_bytes = pt_pool_size * PAGE_SIZE;
    mm_state.memory_start = pt_pool_phys_start + pt_pool_size_bytes;
    
    kprintf("  PT pool reserved: 0x%lx - 0x%lx (%lu pages, %lu MB)\n",
            pt_pool_phys_start, mm_state.memory_start,
            pt_pool_size, pt_pool_size_bytes / (1024*1024));
    
    // End of managed memory is total RAM
    mm_state.memory_end = memory_size;
    
    // Sanity check
    if (mm_state.memory_end <= mm_state.memory_start) {
        kprintf("ERROR: Not enough RAM! memory_start=0x%lx > memory_end=0x%lx\n",
                mm_state.memory_start, mm_state.memory_end);
        mm_state.memory_end = mm_state.memory_start + (64 * 1024 * 1024); // Fallback
    }
    
    mm_state.total_pages = (mm_state.memory_end - mm_state.memory_start) / PAGE_SIZE;
    mm_state.bitmap_size = PAGE_ALIGN((mm_state.total_pages + 7) / 8);
    
    // Place bitmap in kernel virtual space (after heap)
    // The bootloader mapped 32MB of virtual space starting at kernel_end
    mm_state.physical_bitmap = (uint32_t*)(heap_start_virt + KERNEL_HEAP_SIZE);
    
    kprintf("  Memory range: 0x%lx - 0x%lx (%lu MB)\n", 
           mm_state.memory_start, mm_state.memory_end,
           (mm_state.memory_end - mm_state.memory_start) / (1024*1024));
    kprintf("  Total pages: %lu\n", mm_state.total_pages);
    kprintf("  Heap: 0x%lx - 0x%lx\n", 
           heap_start_virt, heap_start_virt + KERNEL_HEAP_SIZE);
    kprintf("  Bitmap at: %p (size: %lu bytes)\n", 
           mm_state.physical_bitmap, mm_state.bitmap_size);

    // Clear bitmap (all pages free initially)
    mm_memset(mm_state.physical_bitmap, 0, mm_state.bitmap_size);
    mm_state.free_pages = mm_state.total_pages;
    
    // Initialize page reference count array
    mm_state.refcount_array_size = mm_state.total_pages * sizeof(uint16_t);
    mm_state.page_refcounts = (uint16_t*)((uint64_t)mm_state.physical_bitmap + mm_state.bitmap_size);
    mm_memset(mm_state.page_refcounts, 0, mm_state.refcount_array_size);
    
    kprintf("  Page refcounts at: %p (size: %lu bytes)\n",
           mm_state.page_refcounts, mm_state.refcount_array_size);
    
    // =========================================================================
    // CRITICAL: Walk the bootloader's page tables and reserve any physical pages
    // that are already mapped. This prevents us from allocating pages that are
    // backing kernel code, heap, bitmap, or refcount array.
    // =========================================================================
    reserve_bootloader_mapped_pages();
    
    // =========================================================================
    // CRITICAL: Reserve all UEFI non-usable memory regions (Runtime Services,
    // ACPI, MMIO, etc.). This prevents us from allocating memory that the
    // firmware still needs for runtime callbacks.
    // =========================================================================
    reserve_uefi_memory_regions();
    
    kprintf("  Free pages after reservations: %lu\n", mm_state.free_pages);
    kprintf("Physical Memory Manager initialized\n");
}

// Allocate a physical page
uint64_t mm_allocate_physical_page(void) {
    if (mm_state.free_pages == 0) {
        return 0; // Out of memory
    }
    
    uint64_t page = find_free_page();
    if (page == (uint64_t)-1) {
        return 0; // No free pages found
    }
    
    set_page_bit(page);
    mm_state.free_pages--;
    
    uint64_t phys = mm_state.memory_start + (page * PAGE_SIZE);
    
    // Clear refcount for newly allocated page
    if (mm_state.page_refcounts) {
        mm_state.page_refcounts[page] = 0;
    }
    
    return phys;
}

// Free a physical page
void mm_free_physical_page(uint64_t physical_address) {
    if (physical_address < mm_state.memory_start || physical_address >= mm_state.memory_end) {
        return; // Invalid address
    }
    
    uint64_t page = (physical_address - mm_state.memory_start) / PAGE_SIZE;
    if (!is_page_allocated(page)) {
        return; // Already free
    }
    
    clear_page_bit(page);
    mm_state.free_pages++;
    
    // Clear refcount
    if (mm_state.page_refcounts) {
        mm_state.page_refcounts[page] = 0;
    }
}

// Get free pages count
uint64_t mm_get_free_pages(void) {
    return mm_state.free_pages;
}

// Allocate contiguous physical pages
uint64_t mm_allocate_contiguous_pages(size_t page_count) {
    if (page_count == 0) {
        kprintf("mm_allocate_contiguous_pages: page_count is 0\n");
        return 0;
    }
    if (mm_state.free_pages < page_count) {
        kprintf("mm_allocate_contiguous_pages: not enough free pages (%lu free, need %lu)\n",
                (unsigned long)mm_state.free_pages, (unsigned long)page_count);
        return 0;
    }
    if (page_count > mm_state.total_pages) {
        kprintf("mm_allocate_contiguous_pages: page_count %lu > total_pages %lu\n",
                (unsigned long)page_count, (unsigned long)mm_state.total_pages);
        return 0;
    }
    
    // Find contiguous free pages
    for (uint64_t start_page = 0; start_page <= mm_state.total_pages - page_count; start_page++) {
        bool found = true;

        // Check if all pages in range are free
        for (size_t i = 0; i < page_count; i++) {
            if (is_page_allocated(start_page + i)) {
                found = false;
                break;
            }
        }

        if (found) {
            // Allocate all pages in range
            for (size_t i = 0; i < page_count; i++) {
                set_page_bit(start_page + i);
                mm_state.free_pages--;
            }
            return mm_state.memory_start + (start_page * PAGE_SIZE);
        }
    }
    
    return 0; // No contiguous block found
}

// Free contiguous physical pages
void mm_free_contiguous_pages(uint64_t physical_address, size_t page_count) {
    for (size_t i = 0; i < page_count; i++) {
        mm_free_physical_page(physical_address + (i * PAGE_SIZE));
    }
}

// VIRTUAL MEMORY MANAGER IMPLEMENTATION

// Debug flag for page table operations (non-static so slab.c can use it)
int mm_debug_pt = 0;

// Forward declarations
void* kalloc_dma(size_t size);
uint64_t mm_virt_to_phys_heap(uint64_t virt);

// Convert kernel heap virtual address to physical address
// The kernel heap is at KERNEL_OFFSET + kernel_end, mapped linearly
uint64_t mm_virt_to_phys_heap(uint64_t virt) {
    // Kernel virtual base: 0xFFFFFFFF80000000
    // The kernel is loaded at physical 0x100000 and mapped to virtual 0xFFFFFFFF80000000
    // So: phys = virt - KERNEL_OFFSET + 0x100000
    
    if (virt >= KERNEL_OFFSET && virt < 0xFFFFFFFF90000000ULL) {
        return (virt - KERNEL_OFFSET) + 0x100000ULL;  // KERNEL_START = 0x100000
    }
    
    // For direct-mapped addresses (PHYS_MAP_BASE + phys), extract physical address
    if (virt >= PHYS_MAP_BASE && virt < (PHYS_MAP_BASE + 0x100000000ULL)) {
        return virt - PHYS_MAP_BASE;
    }
    
    kprintf("ERROR: mm_virt_to_phys_heap: unknown address %p\n", (void*)virt);
    return 0;
}

// Allocate a page for page table structures
// Returns: physical address (for use in page table entries)
// Pages come from the reserved PT pool first, then fall back to physical allocator
static uint64_t allocate_pt_page(void) {
    if (!pt_pool_initialized) {
        kprintf("ERROR: PT pool not initialized!\n");
        return 0;
    }
    
    uint64_t phys;
    
    if (pt_pool_next < pt_pool_size) {
        // Use reserved pool (preferred - outside bitmap range)
        phys = pt_pool_phys_start + (pt_pool_next * PAGE_SIZE);
        pt_pool_next++;
    } else {
        // Pool exhausted - fall back to physical memory allocator
        // This is fine for large allocations after initial setup
        phys = mm_allocate_physical_page();
        if (!phys) {
            kprintf("ERROR: Cannot allocate PT page (pool exhausted, phys alloc failed)\n");
            return 0;
        }
    }
    
    // Access via direct map and zero it
    uint64_t* virt = (uint64_t*)phys_to_virt(phys);
    mm_memset(virt, 0, PAGE_SIZE);
    
    return phys;
}

// Initialize page table page pool by reserving physical memory
// Must be called BEFORE mm_init_physical_memory to reserve the region
void mm_init_pt_pool(void) {
    // The pool is already reserved during mm_init_physical_memory
    // This just marks it as ready to use
    if (pt_pool_phys_start == 0) {
        kprintf("ERROR: PT pool physical region not set!\n");
        return;
    }
    kprintf("Initializing page table pool (%lu pages at phys %p)...\n", 
            pt_pool_size, (void*)pt_pool_phys_start);
    pt_pool_initialized = 1;
    kprintf("  Page table pool ready\n");
}

// Get page table for virtual address
uint64_t* mm_get_page_table(uint64_t virtual_addr, bool create) {
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    // Get PML4 via direct map
    uint64_t pml4_phys = get_cr3() & ~0xFFF;
    mm_state.pml4_table = (uint64_t*)phys_to_virt(pml4_phys);
    
    if (mm_debug_pt) {
        kprintf("PT: vaddr=%p pml4=%lu pdpt=%lu pd=%lu pt=%lu\n",
                (void*)virtual_addr, pml4_index, pdpt_index, pd_index, pt_index);
        kprintf("PT: PML4 at %p, entry[%lu]=%p\n", 
                mm_state.pml4_table, pml4_index, (void*)mm_state.pml4_table[pml4_index]);
    }
    
    uint64_t pdpt_phys = mm_state.pml4_table[pml4_index] & ~0xFFF;
    uint64_t* pdpt = pdpt_phys ? (uint64_t*)phys_to_virt(pdpt_phys) : NULL;
    if (!pdpt && create) {
        pdpt_phys = allocate_pt_page();
        if (!pdpt_phys) {
            return NULL;
        }
        // Already zeroed by allocate_pt_page
        mm_state.pml4_table[pml4_index] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
        if (mm_debug_pt) kprintf("PT: Created new PDPT at %p\n", pdpt);
    }
    if (!pdpt) {
        return NULL;
    }
    
    if (mm_debug_pt) {
        kprintf("PT: PDPT at %p, entry[%lu]=%p\n", pdpt, pdpt_index, (void*)pdpt[pdpt_index]);
    }
    
    uint64_t pd_phys = pdpt[pdpt_index] & ~0xFFF;
    uint64_t* pd = pd_phys ? (uint64_t*)phys_to_virt(pd_phys) : NULL;
    if (!pd && create) {
        pd_phys = allocate_pt_page();
        if (!pd_phys) {
            return NULL;
        }
        // Already zeroed by allocate_pt_page
        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
        pd = (uint64_t*)phys_to_virt(pd_phys);
        if (mm_debug_pt) kprintf("PT: Created new PD at %p\n", pd);
    }
    if (!pd) {
        return NULL;
    }
    
    if (mm_debug_pt) {
        kprintf("PT: PD at %p, entry[%lu]=%p\n", pd, pd_index, (void*)pd[pd_index]);
    }
    
    uint64_t pt_phys = pd[pd_index] & ~0xFFF;
    uint64_t* pt = pt_phys ? (uint64_t*)phys_to_virt(pt_phys) : NULL;
    if (!pt && create) {
        pt_phys = allocate_pt_page();
        if (!pt_phys) {
            return NULL;
        }
        // Already zeroed by allocate_pt_page
        uint64_t new_entry = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;
        if (mm_debug_pt) {
            kprintf("PT: Creating PT at %p for pd[%lu]\n", (void*)pt_phys, pd_index);
        }
        pd[pd_index] = new_entry;
        if (mm_debug_pt) {
            kprintf("PT: Wrote pd[%lu] = 0x%lx, readback = 0x%lx\n", pd_index, new_entry, pd[pd_index]);
        }
        pt = (uint64_t*)phys_to_virt(pt_phys);
        if (mm_debug_pt) kprintf("PT: Created new PT at %p\n", pt);
    }

    if (mm_debug_pt && pt) {
        kprintf("PT: PT at %p, returning &pt[%lu] = %p\n", pt, pt_index, &pt[pt_index]);
    }
    
    return pt ? &pt[pt_index] : NULL;
}

// Initialize virtual memory manager
void mm_initialize_virtual_memory(void) {
    kprintf("Initializing Virtual Memory Manager...\n");
    
    mm_state.pml4_table = (uint64_t*)(get_cr3() & ~0xFFF);
    uint64_t heap_start = mm_get_kernel_heap_start();
    mm_state.next_virtual_addr = heap_start + KERNEL_HEAP_SIZE + mm_state.bitmap_size;
    mm_state.next_virtual_addr = PAGE_ALIGN(mm_state.next_virtual_addr);
    
    kprintf("  Page tables at: %p\n", mm_state.pml4_table);
    kprintf("  Next virtual address: %p\n", (void*)mm_state.next_virtual_addr);
    
    kprintf("Virtual Memory Manager initialized\n");
}

// Remap kernel sections with proper NX permissions
void mm_remap_kernel_with_nx(void) {
    kprintf("Remapping kernel with NX permissions...\n");
    
    uint64_t text_start = (uint64_t)kernel_text_start;
    uint64_t text_end = (uint64_t)kernel_text_end;
    uint64_t rodata_start = (uint64_t)kernel_rodata_start;
    uint64_t rodata_end = (uint64_t)kernel_rodata_end;
    uint64_t data_start = (uint64_t)kernel_data_start;
    uint64_t data_end = (uint64_t)kernel_data_end;
    uint64_t bss_start = (uint64_t)kernel_bss_start;
    uint64_t bss_end = (uint64_t)kernel_bss_end;
    
    kprintf("  .text:   %p - %p (R-X)\n", (void*)text_start, (void*)text_end);
    kprintf("  .rodata: %p - %p (R--)\n", (void*)rodata_start, (void*)rodata_end);
    kprintf("  .data:   %p - %p (RW-)\n", (void*)data_start, (void*)data_end);
    kprintf("  .bss:    %p - %p (RW-)\n", (void*)bss_start, (void*)bss_end);
    
    // Remap .text section: Present + Executable (no NX bit)
    for (uint64_t addr = PAGE_ALIGN_DOWN(text_start); addr < text_end; addr += PAGE_SIZE) {
        uint64_t phys = mm_get_physical_address(addr);
        if (phys) {
            mm_set_page_flags(addr, PAGE_PRESENT | PAGE_GLOBAL);
        }
    }
    
    // Remap .rodata section: Present + Non-Executable (NX bit set)
    for (uint64_t addr = PAGE_ALIGN_DOWN(rodata_start); addr < rodata_end; addr += PAGE_SIZE) {
        uint64_t phys = mm_get_physical_address(addr);
        if (phys) {
            mm_set_page_flags(addr, PAGE_PRESENT | PAGE_GLOBAL | PAGE_NO_EXECUTE);
        }
    }
    
    // Remap .data section: Present + Writable + Non-Executable
    for (uint64_t addr = PAGE_ALIGN_DOWN(data_start); addr < data_end; addr += PAGE_SIZE) {
        uint64_t phys = mm_get_physical_address(addr);
        if (phys) {
            mm_set_page_flags(addr, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE);
        }
    }
    
    // Remap .bss section: Present + Writable + Non-Executable
    for (uint64_t addr = PAGE_ALIGN_DOWN(bss_start); addr < bss_end; addr += PAGE_SIZE) {
        uint64_t phys = mm_get_physical_address(addr);
        if (phys) {
            mm_set_page_flags(addr, PAGE_PRESENT | PAGE_WRITABLE | PAGE_GLOBAL | PAGE_NO_EXECUTE);
        }
    }
    
    // Flush all TLB entries to apply new permissions
    mm_flush_all_tlb();
    
    kprintf("Kernel remapped with NX permissions\n");
}

// Map virtual page to physical page
bool mm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table(virtual_addr, true);
    if (!pte) {
        return false;
    }
    
    uint64_t entry = (physical_addr & ~0xFFF) | flags;
    if (mm_debug_pt) {
        kprintf("MAP: pte=%p writing entry=0x%lx\n", pte, entry);
    }
    *pte = entry;
    if (mm_debug_pt) {
        kprintf("MAP: readback *pte=0x%lx\n", *pte);
    }
    mm_flush_tlb(virtual_addr);
    return true;
}

// Unmap virtual page
void mm_unmap_page(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        *pte = 0;
        mm_flush_tlb(virtual_addr);
    }
}

// Unmap virtual page in a specific address space
void mm_unmap_page_in_address_space(uint64_t* pml4, uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        // Free the physical page
        uint64_t phys = *pte & ~0xFFFULL;
        if (phys) {
            mm_free_physical_page(phys);
        }
        *pte = 0;
        // Flush TLB if this is the current address space
        if (pml4 == mm_get_current_address_space()) {
            mm_flush_tlb(virtual_addr);
        }
    }
}

// Get physical address for virtual address
uint64_t mm_get_physical_address(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (mm_debug_pt) {
        kprintf("GET_PHYS: vaddr=%p pte=%p *pte=0x%lx\n", 
                (void*)virtual_addr, pte, pte ? *pte : 0);
    }
    if (pte && (*pte & PAGE_PRESENT)) {
        // Mask out NX bit (63) and other reserved bits, keep only physical address bits 12-51
        return (*pte & 0x000FFFFFFFFFF000ULL) | (virtual_addr & 0xFFF);
    }
    return 0;
}

// Check if page is mapped
bool mm_is_page_mapped(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    return pte && (*pte & PAGE_PRESENT);
}

// KERNEL HEAP ALLOCATOR IMPLEMENTATION

// Initialize heap
void mm_initialize_heap(void) {
    kprintf("Initializing Kernel Heap Allocator...\n");

    uint64_t heap_start = mm_get_kernel_heap_start();
    mm_state.heap_start = (heap_block_t*) heap_start;
    mm_state.heap_size = KERNEL_HEAP_SIZE;
    mm_state.heap_end = (heap_block_t*)(heap_start + KERNEL_HEAP_SIZE);    
    mm_state.heap_used = 0;
    mm_state.allocation_count = 0;
    mm_state.deallocation_count = 0;
    
    // Initialize first free block
    mm_state.heap_start->magic = HEAP_MAGIC_FREE;
    mm_state.heap_start->size = KERNEL_HEAP_SIZE - sizeof(heap_block_t);
    mm_state.heap_start->is_free = 1;
    mm_state.heap_start->next = NULL;
    mm_state.heap_start->prev = NULL;

    mm_state.free_list = mm_state.heap_start;
    
    kprintf("Kernel Heap Allocator initialized\n");
}

// Find suitable free block
static heap_block_t* find_free_block(size_t size) {
    heap_block_t* current = mm_state.free_list;

    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// Split block if it's too large
static void split_block(heap_block_t* block, size_t size) {
    if (block->size < size + sizeof(heap_block_t) + 32) {
        return; // Not worth splitting
    }
    
    heap_block_t* new_block = (heap_block_t*)((uint8_t*)block + sizeof(heap_block_t) + size);
    new_block->magic = HEAP_MAGIC_FREE;
    new_block->size = block->size - size - sizeof(heap_block_t);
    new_block->is_free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    
    if (block->next) {
        block->next->prev = new_block;
    }
    block->next = new_block;
    block->size = size;
}

// Validate a heap block pointer is within the heap
static int is_valid_heap_block(heap_block_t* block) {
    if (!block) return 1;  // NULL is valid (end of list)
    uint64_t addr = (uint64_t)block;
    uint64_t heap_start = (uint64_t)mm_state.heap_start;
    uint64_t heap_end = (uint64_t)mm_state.heap_end;
    if (addr < heap_start || addr >= heap_end) return 0;
    if (block->magic != HEAP_MAGIC_ALLOCATED && block->magic != HEAP_MAGIC_FREE) return 0;
    return 1;
}

// Coalesce adjacent free blocks
static void coalesce_blocks(heap_block_t* block) {
    // Validate current block
    if (!is_valid_heap_block(block)) {
        kprintf("coalesce_blocks: invalid block %p\n", block);
        return;
    }
    
    // Coalesce with next block
    if (block->next && block->next->is_free) {
        if (!is_valid_heap_block(block->next)) {
            kprintf("coalesce_blocks: invalid block->next %p (from %p)\n", block->next, block);
            return;
        }
        block->size += block->next->size + sizeof(heap_block_t);
        if (block->next->next) {
            if (!is_valid_heap_block(block->next->next)) {
                kprintf("coalesce_blocks: invalid block->next->next %p\n", block->next->next);
                return;
            }
            block->next->next->prev = block;
        }
        block->next = block->next->next;
    }
    
    // Coalesce with previous block
    if (block->prev && block->prev->is_free) {
        if (!is_valid_heap_block(block->prev)) {
            kprintf("coalesce_blocks: invalid block->prev %p (from %p)\n", block->prev, block);
            return;
        }
        block->prev->size += block->size + sizeof(heap_block_t);
        if (block->next) {
            if (!is_valid_heap_block(block->next)) {
                kprintf("coalesce_blocks: invalid block->next %p (in prev merge)\n", block->next);
                return;
            }
            block->next->prev = block->prev;
        }
        block->prev->next = block->next;
    }
}

// Validate heap integrity - returns 0 if OK, -1 if corrupted
int heap_validate(const char* caller) {
    heap_block_t* cur = mm_state.free_list;
    int count = 0;
    uint64_t heap_end_addr = (uint64_t)mm_state.heap_end;
    
    while (cur && count < 1000) {
        // Check magic
        if (cur->magic != HEAP_MAGIC_ALLOCATED && cur->magic != HEAP_MAGIC_FREE) {
            kprintf("HEAP CORRUPT at %s: block %p has bad magic 0x%08x\n", 
                    caller, cur, cur->magic);
            return -1;
        }
        // Check size is reasonable
        if (cur->size == 0 || cur->size > mm_state.heap_size) {
            kprintf("HEAP CORRUPT at %s: block %p has bad size %lu\n",
                    caller, cur, cur->size);
            return -1;
        }
        // Check next pointer is within heap or NULL
        if (cur->next) {
            uint64_t next_addr = (uint64_t)cur->next;
            if (next_addr < (uint64_t)mm_state.heap_start || next_addr >= heap_end_addr) {
                kprintf("HEAP CORRUPT at %s: block %p has bad next %p\n",
                        caller, cur, cur->next);
                return -1;
            }
        }
        cur = cur->next;
        count++;
    }
    return 0;
}

// Allocate memory
void* kalloc(size_t size) {
#ifdef USE_SLAB_ALLOCATOR
    return slab_alloc(size);
#else
    if (size == 0) {
        return NULL;
    }
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    heap_block_t* block = find_free_block(size);
    if (!block) {
        void* ra = __builtin_return_address(0);
        kprintf("kalloc FAILED for size %lu, heap used=%lu/%lu (caller=%p)\n", 
            (unsigned long)size, mm_state.heap_used, mm_state.heap_size, ra);
        return NULL; // Out of memory
    }
    
    // Split block if necessary
    split_block(block, size);
    
    // Mark block as allocated
    block->magic = HEAP_MAGIC_ALLOCATED;
    block->is_free = 0;
    
    // Update statistics
    mm_state.heap_used += size + sizeof(heap_block_t);
    mm_state.allocation_count++;

    return (uint8_t*)block + sizeof(heap_block_t);
#endif
}

// Free memory
void kfree(void* ptr) {
#ifdef USE_SLAB_ALLOCATOR
    slab_free(ptr);
#else
    if (!ptr) {
        return;
    }

    if (!mm_state.heap_start || !mm_state.heap_end) {
        void* ra = __builtin_return_address(0);
        kprintf("ERROR: kfree before heap init (%p, caller=%p)\n", ptr, ra);
        return;
    }

    uint8_t* heap_start = (uint8_t*)mm_state.heap_start;
    uint8_t* heap_end = (uint8_t*)mm_state.heap_end;
    if ((uint8_t*)ptr < heap_start + sizeof(heap_block_t) || (uint8_t*)ptr >= heap_end) {
        void* ra = __builtin_return_address(0);
        kprintf("ERROR: Invalid free() call for non-heap address %p (caller=%p)\n", ptr, ra);
        return;
    }
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    
    // Validate block
    if (block->magic != HEAP_MAGIC_ALLOCATED || block->is_free) {
        void* ra = __builtin_return_address(0);
        kprintf("ERROR: Invalid free() call for address %p (magic=0x%08x free=%d caller=%p)\n", 
            ptr, block->magic, block->is_free, ra);
        return;
    }
    
    // Mark as free
    block->magic = HEAP_MAGIC_FREE;
    block->is_free = 1;
    
    // Update statistics
    mm_state.heap_used -= block->size + sizeof(heap_block_t);
    mm_state.deallocation_count++;
    
    // Coalesce with adjacent free blocks
    coalesce_blocks(block);
#endif
}

// Reallocate memory
void* krealloc(void* ptr, size_t new_size) {
#ifdef USE_SLAB_ALLOCATOR
    return slab_realloc(ptr, new_size);
#else
    if (!ptr) {
        return kalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    if (block->magic != HEAP_MAGIC_ALLOCATED) {
        return NULL;
    }
    
    if (block->size >= new_size) {
        return ptr; // Current block is large enough
    }
    
    // Allocate new block and copy data
    void* new_ptr = kalloc(new_size);
    if (new_ptr) {
        mm_memcpy(new_ptr, ptr, block->size < new_size ? block->size : new_size);
        kfree(ptr);
    }
    
    return new_ptr;
#endif
}

// Allocate and zero memory
void* kcalloc(size_t count, size_t size) {
#ifdef USE_SLAB_ALLOCATOR
    return slab_calloc(count, size);
#else
    size_t total_size = count * size;
    void* ptr = kalloc(total_size);
    if (ptr) {
        mm_memset(ptr, 0, total_size);
    }
    return ptr;
#endif
}

// ============================================================================
// DMA-SAFE ALLOCATIONS
// These always use the legacy heap which is in low physical memory (< 4GB)
// Use for device DMA buffers (XHCI, USB, etc.)
// ============================================================================

// DMA-safe allocation (always uses legacy heap for low physical addresses)
void* kalloc_dma(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    heap_block_t* block = find_free_block(size);
    if (!block) {
        void* ra = __builtin_return_address(0);
        kprintf("kalloc_dma FAILED for size %lu, heap used=%lu/%lu (caller=%p)\n", 
            (unsigned long)size, mm_state.heap_used, mm_state.heap_size, ra);
        return NULL;
    }
    
    split_block(block, size);
    block->magic = HEAP_MAGIC_ALLOCATED;
    block->is_free = 0;
    mm_state.heap_used += size + sizeof(heap_block_t);
    mm_state.allocation_count++;

    return (uint8_t*)block + sizeof(heap_block_t);
}

// DMA-safe calloc
void* kcalloc_dma(size_t count, size_t size) {
    size_t total_size = count * size;
    void* ptr = kalloc_dma(total_size);
    if (ptr) {
        mm_memset(ptr, 0, total_size);
    }
    return ptr;
}

// Free DMA-safe allocation
void kfree_dma(void* ptr) {
    if (!ptr) {
        return;
    }

    if (!mm_state.heap_start || !mm_state.heap_end) {
        return;
    }

    uint8_t* heap_start = (uint8_t*)mm_state.heap_start;
    uint8_t* heap_end = (uint8_t*)mm_state.heap_end;
    if ((uint8_t*)ptr < heap_start + sizeof(heap_block_t) || (uint8_t*)ptr >= heap_end) {
        void* ra = __builtin_return_address(0);
        kprintf("ERROR: Invalid kfree_dma for address %p (caller=%p)\n", ptr, ra);
        return;
    }
    
    heap_block_t* block = (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
    
    if (block->magic != HEAP_MAGIC_ALLOCATED || block->is_free) {
        void* ra = __builtin_return_address(0);
        kprintf("ERROR: Invalid kfree_dma for address %p (magic=0x%08x free=%d caller=%p)\n", 
            ptr, block->magic, block->is_free, ra);
        return;
    }
    
    block->magic = HEAP_MAGIC_FREE;
    block->is_free = 1;
    mm_state.heap_used -= block->size + sizeof(heap_block_t);
    mm_state.deallocation_count++;
    coalesce_blocks(block);
}

// MEMORY STATISTICS AND DEBUGGING

// Get memory statistics
void mm_get_memory_stats(memory_stats_t* stats) {
    stats->total_memory = mm_state.memory_end - mm_state.memory_start;
    stats->free_memory = mm_state.free_pages * PAGE_SIZE;
    stats->used_memory = stats->total_memory - stats->free_memory;
    stats->total_pages = mm_state.total_pages;
    stats->free_pages = mm_state.free_pages;
    stats->used_pages = stats->total_pages - stats->free_pages;
    stats->heap_allocated = mm_state.heap_used;
    stats->heap_free = mm_state.heap_size - mm_state.heap_used;
    stats->allocations = mm_state.allocation_count;
    stats->deallocations = mm_state.deallocation_count;
}

// Print memory statistics
void mm_print_memory_stats(void) {
    memory_stats_t stats;
    mm_get_memory_stats(&stats);
    
    kprintf("\n=== Memory Statistics ===\n");
    kprintf("Physical Memory:\n");
    kprintf("  Total: %d MB (%d pages)\n", 
           stats.total_memory / (1024*1024), stats.total_pages);
    kprintf("  Used:  %d MB (%d pages)\n", 
           stats.used_memory / (1024*1024), stats.used_pages);
    kprintf("  Free:  %d MB (%d pages)\n", 
           stats.free_memory / (1024*1024), stats.free_pages);
    kprintf("========================\n\n");
}

// Validate heap integrity
bool mm_validate_heap(void) {
    heap_block_t* current = mm_state.heap_start;
    uint32_t block_count = 0;
    
    while (current && (uint8_t*)current < (uint8_t*)mm_state.heap_end) {
        // Check magic numbers
        if (current->magic != HEAP_MAGIC_ALLOCATED && current->magic != HEAP_MAGIC_FREE) {
            kprintf("ERROR: Invalid magic in heap block %d at %p\n", block_count, current);
            return false;
        }
        
        // Check bounds
        if ((uint8_t*)current + sizeof(heap_block_t) + current->size > (uint8_t*)mm_state.heap_end) {
            kprintf("ERROR: Heap block %d extends beyond heap end\n", block_count);
            return false;
        }
        
        block_count++;
        current = current->next;
        
        // Prevent infinite loop
        if (block_count > 1000) {
            kprintf("ERROR: Too many heap blocks, possible corruption\n");
            return false;
        }
    }
    
    return true;
}

// Print heap statistics
void mm_print_heap_stats(void) {
    kprintf("\n=== Heap Block Information ===\n");
    
    heap_block_t* current = mm_state.heap_start;
    uint32_t block_count = 0;
    uint32_t free_blocks = 0;
    uint32_t allocated_blocks = 0;
    
    while (current && (uint8_t*)current < (uint8_t*)mm_state.heap_end && block_count < 20) {
        kprintf("Block %d: %p, size=%d, %s\n", 
               block_count, current, current->size, 
               current->is_free ? "FREE" : "ALLOCATED");
        
        if (current->is_free) {
            free_blocks++;
        } else {
            allocated_blocks++;
        }
        
        block_count++;
        current = current->next;
    }
    
    kprintf("Total blocks shown: %d (Free: %d, Allocated: %d)\n", 
           block_count, free_blocks, allocated_blocks);
    kprintf("==============================\n\n");
}

// ============================================================================
// USER ADDRESS SPACE MANAGEMENT
// ============================================================================

// Get page table entry from a specific PML4, creating intermediate tables if needed
// Note: pml4 is expected to be a virtual address (via phys_to_virt)
uint64_t* mm_get_page_table_from_pml4(uint64_t* pml4, uint64_t virtual_addr, bool create) {
    if (!pml4) {
        return NULL;
    }
    
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    
    bool is_user_space = (virtual_addr < KERNEL_OFFSET);
    
    // Get PDPT
    uint64_t* pdpt;
    uint64_t pdpt_phys = pml4[pml4_index] & ~0xFFFULL;
    if (!(pml4[pml4_index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        pdpt_phys = allocate_pt_page();  // Use safe PT pool
        if (!pdpt_phys) return NULL;
        // Page already zeroed by allocate_pt_page
        // For user space, set PAGE_USER on all levels
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pml4[pml4_index] = pdpt_phys | flags;
        pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pml4[pml4_index] & PAGE_USER)) {
            pml4[pml4_index] |= PAGE_USER;
        }
        pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
    }
    
    // Get PD
    uint64_t* pd;
    uint64_t pd_phys = pdpt[pdpt_index] & ~0xFFFULL;
    if (!(pdpt[pdpt_index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        pd_phys = allocate_pt_page();  // Use safe PT pool
        if (!pd_phys) return NULL;
        // Page already zeroed by allocate_pt_page
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pdpt[pdpt_index] = pd_phys | flags;
        pd = (uint64_t*)phys_to_virt(pd_phys);
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pdpt[pdpt_index] & PAGE_USER)) {
            pdpt[pdpt_index] |= PAGE_USER;
        }
        pd = (uint64_t*)phys_to_virt(pd_phys);
    }
    
    // Get PT
    uint64_t* pt;
    uint64_t pd_entry = pd[pd_index];
    
    // Check if this is a 2MB large page (PS bit set)
    if ((pd_entry & PAGE_PRESENT) && (pd_entry & 0x80)) {
        // This is a 2MB page - we need to split it into 4KB pages
        if (!create) return NULL;
        
        // Get the base physical address of the 2MB page
        uint64_t large_page_base = pd_entry & ~0x1FFFFFULL;  // Mask off lower 21 bits
        uint64_t old_flags = pd_entry & 0xFFFULL;           // Keep flags (lower 12 bits)
        
        // Allocate a new page table from safe PT pool
        uint64_t pt_phys = allocate_pt_page();
        if (!pt_phys) return NULL;
        
        pt = (uint64_t*)phys_to_virt(pt_phys);
        
        // Fill the page table with 512 4KB pages covering the same 2MB region
        for (int i = 0; i < 512; i++) {
            uint64_t page_phys = large_page_base + (i * PAGE_SIZE);
            // Keep original flags but remove PS bit and add any user flags if needed
            uint64_t pt_flags = (old_flags & ~0x80) | PAGE_PRESENT;
            if (is_user_space) {
                pt_flags |= PAGE_USER;
            }
            pt[i] = page_phys | pt_flags;
        }
        
        // Update PD entry to point to the new page table
        uint64_t pd_flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            pd_flags |= PAGE_USER;
        }
        pd[pd_index] = pt_phys | pd_flags;
    } else if (!(pd_entry & PAGE_PRESENT)) {
        if (!create) return NULL;
        uint64_t pt_phys = allocate_pt_page();  // Use safe PT pool
        if (!pt_phys) return NULL;
        
        // Page already zeroed by allocate_pt_page
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (is_user_space) {
            flags |= PAGE_USER;
        }
        pd[pd_index] = pt_phys | flags;
        pt = (uint64_t*)phys_to_virt(pt_phys);
    } else {
        // Entry exists - but for user space mapping, we may need to add PAGE_USER
        if (is_user_space && create && !(pd[pd_index] & PAGE_USER)) {
            pd[pd_index] |= PAGE_USER;
        }
        uint64_t pt_phys = pd[pd_index] & ~0xFFFULL;
        pt = (uint64_t*)phys_to_virt(pt_phys);
    }
    
    return &pt[pt_index];
}

// Create a new user address space (PML4)
// No identity mapping - user space is clean.
// Shares:
//   - PML4[272]: Direct map (PHYS_MAP_BASE) for kernel physical memory access
//   - PML4[511]: Kernel higher-half mapping for kernel code/data
uint64_t* mm_create_user_address_space(void) {
    // Allocate PML4 from safe PT pool
    uint64_t pml4_phys = allocate_pt_page();
    if (!pml4_phys) {
        kprintf("mm_create_user_address_space: Failed to allocate PML4\n");
        return NULL;
    }
    
    // Access via direct map (already zeroed by allocate_pt_page)
    uint64_t* new_pml4 = (uint64_t*)phys_to_virt(pml4_phys);
    
    // Get kernel PML4 via direct map
    uint64_t kernel_pml4_phys = get_cr3() & ~0xFFFULL;
    uint64_t* kernel_pml4 = (uint64_t*)phys_to_virt(kernel_pml4_phys);
    
    // Copy PML4[272] - Direct map (PHYS_MAP_BASE = 0xFFFF880000000000)
    // This allows kernel code to access physical memory via phys_to_virt()
    if (kernel_pml4[PHYS_MAP_PML4_INDEX] & PAGE_PRESENT) {
        new_pml4[PHYS_MAP_PML4_INDEX] = kernel_pml4[PHYS_MAP_PML4_INDEX];
    }
    
    // Copy PML4[511] - Kernel higher-half mapping (supervisor only)
    // This allows kernel code to run while in user's address space
    // It also provides access to kernel heap, stacks, etc.
    if (kernel_pml4[511] & PAGE_PRESENT) {
        new_pml4[511] = kernel_pml4[511];
    }
    
    // User space mappings (PML4[0-255]) start empty
    // They will be filled in by mm_map_user_page() when loading ELF, etc.
    
    return new_pml4;
}

// Destroy an address space and free all user pages
void mm_destroy_address_space(uint64_t* pml4) {
    if (!pml4) return;
    
    // pml4 is a virtual address (via phys_to_virt)
    // Calculate the physical address for freeing
    uint64_t pml4_phys = virt_to_phys(pml4);
    
    // Don't free if it's the kernel PML4
    uint64_t kernel_pml4_phys = get_cr3() & ~0xFFFULL;
    if (pml4_phys == kernel_pml4_phys) {
        return;
    }
    
    // Free user-space page tables (entries 0-255, user space only)
    // Skip 256-511 (kernel space) and 272 (direct map)
    // Use 0x000FFFFFFFFFF000ULL mask to extract physical address (bits 12-51)
    for (int i = 0; i < 256; i++) {
        if (pml4[i] & PAGE_PRESENT) {
            uint64_t pdpt_phys = pml4[i] & 0x000FFFFFFFFFF000ULL;
            uint64_t* pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
            
            for (int j = 0; j < 512; j++) {
                if (pdpt[j] & PAGE_PRESENT) {
                    uint64_t pd_phys = pdpt[j] & 0x000FFFFFFFFFF000ULL;
                    uint64_t* pd = (uint64_t*)phys_to_virt(pd_phys);
                    
                    for (int k = 0; k < 512; k++) {
                        if (pd[k] & PAGE_PRESENT) {
                            // Check if it's a 2MB page or a page table
                            if (pd[k] & PAGE_SIZE_FLAG) {
                                // 2MB page - check COW refcount before freeing (mask 21 bits for 2MB alignment)
                                uint64_t phys = pd[k] & 0x000FFFFFFFE00000ULL;
                                if (pd[k] & PAGE_USER) {
                                    // User page - use refcount
                                    if (mm_decref_page(phys)) {
                                        mm_free_physical_page(phys);
                                    }
                                } else {
                                    mm_free_physical_page(phys);
                                }
                            } else {
                                uint64_t pt_phys = pd[k] & 0x000FFFFFFFFFF000ULL;
                                uint64_t* pt = (uint64_t*)phys_to_virt(pt_phys);
                                
                                // Free all physical pages in this PT
                                for (int l = 0; l < 512; l++) {
                                    if (pt[l] & PAGE_PRESENT) {
                                        uint64_t phys = pt[l] & 0x000FFFFFFFFFF000ULL;
                                        // Check if this is a user page (could be COW shared)
                                        if (pt[l] & PAGE_USER) {
                                            // Use refcount - only free if last reference
                                            if (mm_decref_page(phys)) {
                                                mm_free_physical_page(phys);
                                            }
                                        } else {
                                            mm_free_physical_page(phys);
                                        }
                                    }
                                }
                                
                                // Free the PT itself (page tables are not shared)
                                mm_free_physical_page(pt_phys);
                            }
                        }
                    }
                    
                    // Free the PD (page directories are not shared)
                    mm_free_physical_page(pd_phys);
                }
            }
            
            // Free the PDPT (not shared)
            mm_free_physical_page(pdpt_phys);
        }
    }
    
    // Free the PML4 itself
    mm_free_physical_page(pml4_phys);
}

// Switch to a different address space
// pml4 is a virtual address (from phys_to_virt)
void mm_switch_address_space(uint64_t* pml4) {
    if (pml4) {
        // Convert virtual address back to physical for CR3
        uint64_t pml4_phys = virt_to_phys(pml4);
        set_cr3(pml4_phys);
    }
}

// Get the current address space (returns virtual address via phys_to_virt)
uint64_t* mm_get_current_address_space(void) {
    uint64_t pml4_phys = get_cr3() & ~0xFFFULL;
    return (uint64_t*)phys_to_virt(pml4_phys);
}

// Map a page in a specific address space
bool mm_map_page_in_address_space(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table_from_pml4(pml4, virtual_addr, true);
    if (!pte) {
        return false;
    }
    
    *pte = (physical_addr & ~0xFFFULL) | flags;
    
    // Flush TLB if this is the current address space
    if (pml4 == mm_get_current_address_space()) {
        mm_flush_tlb(virtual_addr);
    }
    
    return true;
}

// Map a user page with appropriate flags
bool mm_map_user_page(uint64_t* pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    // Ensure user bit is set for user-space addresses
    if (virtual_addr < KERNEL_OFFSET) {
        flags |= PAGE_USER;
    }
    return mm_map_page_in_address_space(pml4, virtual_addr, physical_addr, flags);
}

// Map a user stack region
bool mm_map_user_stack(uint64_t* pml4, uint64_t stack_top, size_t stack_size) {
    if (!pml4 || stack_size == 0) {
        return false;
    }
    
    size_t pages = (stack_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t stack_bottom = stack_top - stack_size;
    
    for (size_t i = 0; i < pages; i++) {
        uint64_t vaddr = stack_bottom + (i * PAGE_SIZE);
        uint64_t phys = mm_allocate_physical_page();
        
        if (!phys) {
            // Unmap already-mapped pages on failure
            for (size_t j = 0; j < i; j++) {
                mm_unmap_page_in_address_space(pml4, stack_bottom + (j * PAGE_SIZE));
            }
            return false;
        }
        
        // Zero the stack page via direct map
        mm_memset(phys_to_virt(phys), 0, PAGE_SIZE);
        
        // Map with user, writable, non-executable flags (stack should not be executable)
        uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_NO_EXECUTE;
        if (!mm_map_page_in_address_space(pml4, vaddr, phys, flags)) {
            mm_free_physical_page(phys);
            // Unmap already-mapped pages on failure
            for (size_t j = 0; j < i; j++) {
                mm_unmap_page_in_address_space(pml4, stack_bottom + (j * PAGE_SIZE));
            }
            return false;
        }
    }
    
    return true;
}

// Get physical address from a specific PML4
uint64_t mm_get_physical_address_from_pml4(uint64_t* pml4, uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table_from_pml4(pml4, virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        // Physical address is in bits 12-51 (mask off flags at bits 0-11 and bit 63)
        #define PTE_PHYS_MASK_ADDR 0x000FFFFFFFFFF000ULL
        return (*pte & PTE_PHYS_MASK_ADDR) | (virtual_addr & 0xFFF);
    }
    return 0;
}

// Get page flags for a virtual address
uint64_t mm_get_page_flags(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte) {
        // Return flags: bits 0-11 (low flags) and bit 63 (NX)
        return (*pte & 0xFFFULL) | (*pte & PAGE_NO_EXECUTE);
    }
    return 0;
}

// Physical address mask: bits 12-51 contain the physical page frame
#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL

// Set page flags for a virtual address
bool mm_set_page_flags(uint64_t virtual_addr, uint64_t flags) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (pte && (*pte & PAGE_PRESENT)) {
        uint64_t phys = *pte & PTE_PHYS_MASK;  // Extract physical address (bits 12-51)
        *pte = phys | flags;
        mm_flush_tlb(virtual_addr);
        return true;
    }
    return false;
}

// ============================================================================
// COPY-ON-WRITE SUPPORT
// ============================================================================

// Mark a page as copy-on-write (make it read-only with COW flag)
bool mm_mark_page_cow(uint64_t virtual_addr) {
    uint64_t* pte = mm_get_page_table(virtual_addr, false);
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return false;
    }
    
    // Remove writable, add COW marker
    *pte = (*pte & ~PAGE_WRITABLE) | PAGE_COW;
    mm_flush_tlb(virtual_addr);
    return true;
}

// Handle a COW page fault - allocate new page and copy contents
bool mm_handle_cow_fault(uint64_t fault_addr) {
    uint64_t page_addr = fault_addr & ~0xFFFULL;
    uint64_t* pte = mm_get_page_table(page_addr, false);
    
    if (!pte || !(*pte & PAGE_PRESENT)) {
        kprintf("COW: no PTE for 0x%lx\n", fault_addr);
        return false;
    }
    
    // Check if this is a COW page
    if (!(*pte & PAGE_COW)) {
        kprintf("COW: PTE 0x%lx not COW (PTE=0x%lx)\n", fault_addr, *pte);
        return false;  // Not a COW fault
    }
    
    // Extract physical address (bits 12-51, mask off flags and NX bit)
    uint64_t old_phys = *pte & 0x000FFFFFFFFFF000ULL;
    
    // Check if we're the only reference - can just make writable
    uint16_t refcount = mm_get_page_refcount(old_phys);
    if (refcount <= 1) {
        // We're the only user, just make it writable again
        uint64_t flags = (*pte & 0xFFF) & ~PAGE_COW;
        flags |= PAGE_WRITABLE;
        *pte = (*pte & 0x8000000000000000ULL) | old_phys | flags; // Preserve NX bit
        mm_flush_tlb(page_addr);
        return true;
    }
    
    // Allocate a new physical page
    uint64_t new_phys = mm_allocate_physical_page();
    if (!new_phys) {
        kprintf("mm_handle_cow_fault: Failed to allocate new page\n");
        return false;
    }
    
    // Copy contents from old page to new page via direct map
    mm_memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);
    
    // Update PTE: remove COW, add writable, point to new page, preserve NX bit
    uint64_t flags = (*pte & 0xFFF) & ~PAGE_COW;
    flags |= PAGE_WRITABLE;
    *pte = (*pte & 0x8000000000000000ULL) | new_phys | flags;
    
    mm_flush_tlb(page_addr);
    
    // Decrement refcount on old page - free if it reaches 0
    if (mm_decref_page(old_phys)) {
        mm_free_physical_page(old_phys);
    }
    
    // New page starts with refcount of 0 (private to this process)
    // We don't need to track it until it's shared via fork again
    
    return true;
}

// Clone an address space for fork() - uses COW for efficiency
uint64_t* mm_clone_address_space(uint64_t* src_pml4) {
    if (!src_pml4) {
        return NULL;
    }
    
    // Create new address space (this sets up kernel mappings)
    uint64_t* new_pml4 = mm_create_user_address_space();
    if (!new_pml4) {
        return NULL;
    }
    
    // Clone user-space mappings with COW from source PML4
    // User code lives in PML4[0] around virtual address 0x400000
    // We need to find user pages (those with PAGE_USER flag) in the source
    // and add them to the child's address space with COW semantics.
    
    // Handle PML4 entries 0-255 (user space)
    // User space starts fresh and we copy user pages with COW semantics
    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & PAGE_PRESENT)) continue;
        
        uint64_t src_pdpt_phys = src_pml4[i] & ~0xFFFULL;
        uint64_t* src_pdpt = (uint64_t*)phys_to_virt(src_pdpt_phys);
        
        // Allocate PDPT for new address space from safe PT pool
        uint64_t pdpt_phys = allocate_pt_page();
        if (!pdpt_phys) goto fail;
        // Page already zeroed by allocate_pt_page
        new_pml4[i] = pdpt_phys | (src_pml4[i] & 0xFFF);
        uint64_t* new_pdpt = (uint64_t*)phys_to_virt(pdpt_phys);
        
        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & PAGE_PRESENT)) continue;
            
            uint64_t src_pd_phys = src_pdpt[j] & ~0xFFFULL;
            uint64_t* src_pd = (uint64_t*)phys_to_virt(src_pd_phys);
            
            uint64_t pd_phys = allocate_pt_page();
            if (!pd_phys) goto fail;
            // Page already zeroed by allocate_pt_page
            new_pdpt[j] = pd_phys | (src_pdpt[j] & 0xFFF);
            uint64_t* new_pd = (uint64_t*)phys_to_virt(pd_phys);
            
            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & PAGE_PRESENT)) continue;
                
                if (src_pd[k] & PAGE_SIZE_FLAG) {
                    // 2MB huge page - share with COW if it has user flag
                    if (src_pd[k] & PAGE_USER) {
                        uint64_t cow_flags = (src_pd[k] & ~PAGE_WRITABLE) | PAGE_COW;
                        src_pd[k] = cow_flags;
                        new_pd[k] = cow_flags;
                    } else {
                        // Kernel page - just share
                        new_pd[k] = src_pd[k];
                    }
                } else {
                    uint64_t src_pt_phys = src_pd[k] & ~0xFFFULL;
                    uint64_t* src_pt = (uint64_t*)phys_to_virt(src_pt_phys);
                    
                    uint64_t pt_phys = allocate_pt_page();
                    if (!pt_phys) goto fail;
                    // Page already zeroed by allocate_pt_page
                    new_pd[k] = pt_phys | (src_pd[k] & 0xFFF);
                    uint64_t* new_pt = (uint64_t*)phys_to_virt(pt_phys);
                    
                    for (int l = 0; l < 512; l++) {
                        if (!(src_pt[l] & PAGE_PRESENT)) continue;
                        
                        if (src_pt[l] & PAGE_USER) {
                            // User page - share with COW
                            uint64_t phys_page = src_pt[l] & 0x000FFFFFFFFFF000ULL;
                            uint64_t cow_flags = (src_pt[l] & ~PAGE_WRITABLE) | PAGE_COW;
                            src_pt[l] = cow_flags;
                            new_pt[l] = cow_flags;
                            
                            // Increment page reference count
                            if (mm_get_page_refcount(phys_page) == 0) {
                                mm_incref_page(phys_page);
                            }
                            mm_incref_page(phys_page);
                        } else {
                            // Kernel page - just copy mapping
                            new_pt[l] = src_pt[l];
                        }
                    }
                }
            }
        }
    }
    
    // Flush TLB for source address space
    mm_flush_all_tlb();
    
    return new_pml4;
    
fail:
    mm_destroy_address_space(new_pml4);
    return NULL;
}

// ============================================================================
// SYSCALL/SYSRET CONFIGURATION
// ============================================================================

// MSR definitions for SYSCALL/SYSRET
#define MSR_STAR        0xC0000081  // Segment selectors for SYSCALL
#define MSR_LSTAR       0xC0000082  // RIP for SYSCALL (64-bit)
#define MSR_CSTAR       0xC0000083  // RIP for SYSCALL (compat mode)
#define MSR_SFMASK      0xC0000084  // RFLAGS mask for SYSCALL
#define MSR_EFER        0xC0000080  // Extended Feature Enable Register

// Read MSR
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Syscall entry point - defined in syscall.asm
extern void syscall_entry(void);

// Global flag indicating SMAP is active (used by copy_from_user/copy_to_user)
bool g_smap_enabled = false;

// SMAP control: temporarily allow supervisor access to user pages
void smap_disable(void) {
    if (g_smap_enabled) {
        __asm__ volatile("stac" ::: "cc");
    }
}

// SMAP control: re-enable SMAP protection
void smap_enable(void) {
    if (g_smap_enabled) {
        __asm__ volatile("clac" ::: "cc");
    }
}

// Enable SMEP and SMAP if CPU supports them
void mm_enable_smep_smap(void) {
    // Check CPUID leaf 7, subleaf 0 for SMEP/SMAP support
    uint32_t eax, ebx, ecx, edx;
    eax = 7;
    ecx = 0;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    
    // SMEP: bit 7 of EBX from CPUID, enables CR4 bit 20
    if (ebx & (1 << 7)) {
        cr4 |= (1ULL << 20);
        kprintf("SMEP enabled (Supervisor Mode Execution Prevention)\n");
    } else {
        kprintf("SMEP not supported by CPU\n");
    }
    
    // SMAP: bit 20 of EBX from CPUID, enables CR4 bit 21
    if (ebx & (1 << 20)) {
        cr4 |= (1ULL << 21);
        g_smap_enabled = true;
        kprintf("SMAP enabled (Supervisor Mode Access Prevention)\n");
    } else {
        g_smap_enabled = false;
        kprintf("SMAP not supported by CPU\n");
    }
    
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
}

// Kernel stack for after identity mapping removal
// This needs to be in the kernel's .bss section (which is in higher-half)
#define KERNEL_INIT_STACK_SIZE (64 * 1024)  // 64KB kernel init stack
static uint8_t g_kernel_init_stack[KERNEL_INIT_STACK_SIZE] __attribute__((aligned(16)));

// Get the new kernel stack top address
uint64_t mm_get_kernel_stack_top(void) {
    // Stack grows down, return top of stack array minus some space for safety
    return (uint64_t)&g_kernel_init_stack[KERNEL_INIT_STACK_SIZE - 16] & ~0xFULL;
}

// Assembly helper to switch stacks - defined in stack_switch.asm
extern void switch_stack_and_call(uint64_t new_rsp, void (*func)(void));

// Internal function that runs on the new stack
static void continue_after_stack_switch(void);

// Switch to a kernel stack in higher-half space
// This must be called before mm_remove_identity_mapping() because the current
// stack is in low memory (set up by bootloader in identity-mapped region)
void mm_switch_to_kernel_stack(void) {
    uint64_t new_rsp = mm_get_kernel_stack_top();
    
    // This function switches the stack and calls continue_after_stack_switch
    // It will NOT return - execution continues from continue_after_stack_switch
    switch_stack_and_call(new_rsp, continue_after_stack_switch);
    
    // Never reached
}

// This function is called on the new stack
static void continue_after_stack_switch(void) {
    // Remove identity mapping now that we're on the new stack
    mm_remove_identity_mapping();
    
    // Continue with the rest of initialization
    extern void continue_system_startup(void);
    continue_system_startup();
    
    // Never returns
}

// Remove identity mapping from kernel PML4
// After this, physical memory can only be accessed via direct map at PHYS_MAP_BASE
void mm_remove_identity_mapping(void) {
    // Disable interrupts during this critical section
    // Interrupt handlers may access identity-mapped memory
    __asm__ volatile("cli");
    
    // Get current PML4 via direct map
    uint64_t pml4_phys = get_cr3() & ~0xFFFULL;
    uint64_t* pml4 = (uint64_t*)phys_to_virt(pml4_phys);
    
    // Clear PML4 entry 0 - this removes the 0-512GB identity mapping
    if (pml4[0] & PAGE_PRESENT) {
        pml4[0] = 0;
    }
    
    // Clear any other low entries that were identity-mapped
    for (int i = 1; i < 4; i++) {
        if (pml4[i] & PAGE_PRESENT) {
            pml4[i] = 0;
        }
    }
    
    // Flush entire TLB
    uint64_t cr3_val = get_cr3();
    set_cr3(cr3_val);
    
    // Re-enable interrupts now that identity mapping is removed
    __asm__ volatile("sti");
}

// Enable NX (No-Execute) bit support
void mm_enable_nx(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= (1ULL << 11);  // Set NXE (No-Execute Enable) bit
    wrmsr(MSR_EFER, efer);
    kprintf("NX bit enabled in EFER\n");
}

// Initialize SYSCALL/SYSRET
void mm_initialize_syscall(void) {
    // Enable SCE (System Call Extensions) in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;  // Set SCE bit
    wrmsr(MSR_EFER, efer);
    
    // Configure STAR register for SYSCALL/SYSRET segment switching
    // STAR layout:
    //   Bits 63:48 = User segment base (for SYSRET)
    //   Bits 47:32 = Kernel segment base (for SYSCALL)
    //
    // SYSCALL sets: CS = STAR[47:32], SS = STAR[47:32] + 8
    // SYSRET sets:  CS = STAR[63:48] + 16 | 3, SS = STAR[63:48] + 8 | 3
    //
    // Our GDT layout:
    //   0x00: null
    //   0x08: kernel code
    //   0x10: kernel data
    //   0x18: user code
    //   0x20: user data
    //
    // For SYSCALL (entering kernel):
    //   STAR[47:32] = 0x08 → CS = 0x08 (kernel code), SS = 0x10 (kernel data)
    //
    // For SYSRET (returning to user):
    //   STAR[63:48] = 0x10 → CS = 0x10+16|3 = 0x23, SS = 0x10+8|3 = 0x1B
    //   This means CS = 0x23 (GDT[4] = user data) and SS = 0x1B (GDT[3] = user code)
    //   Note: Segments are reversed but work because 64-bit mode ignores most segment attributes
    
    uint64_t star = 0;
    star |= ((uint64_t)0x10 << 48);  // User base: CS=0x23, SS=0x1B
    star |= ((uint64_t)0x08 << 32);  // Kernel base: CS=0x08, SS=0x10
    wrmsr(MSR_STAR, star);
    
    // Set LSTAR to syscall entry point (64-bit mode)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    
    // Set CSTAR for compatibility mode (unused in 64-bit only kernel)
    wrmsr(MSR_CSTAR, (uint64_t)syscall_entry);
    
    // Set SFMASK - RFLAGS bits to clear on SYSCALL
    // Clear IF (interrupt flag), TF (trap flag), DF (direction flag), and AC (SMAP bypass)
    // IF (bit 9) = 0x200, TF (bit 8) = 0x100, DF (bit 10) = 0x400, AC (bit 18) = 0x40000
    wrmsr(MSR_SFMASK, 0x200 | 0x100 | 0x400 | 0x40000);
    
    kprintf("SYSCALL/SYSRET initialized\n");
    kprintf("  STAR = 0x%016llx\n", star);
    kprintf("  LSTAR = 0x%016llx\n", (uint64_t)syscall_entry);
}

// ============================================================================
// PAGE REFERENCE COUNTING (for COW fork)
// ============================================================================

// Get page index from physical address
static inline uint64_t page_to_index(uint64_t phys_addr) {
    if (phys_addr < mm_state.memory_start || phys_addr >= mm_state.memory_end) {
        return (uint64_t)-1;  // Out of tracked range
    }
    return (phys_addr - mm_state.memory_start) / PAGE_SIZE;
}

// Initialize page reference counts (called after physical memory init)
void mm_init_page_refcounts(void) {
    // Already done in mm_initialize_physical_memory
    // This function exists for explicit re-initialization if needed
    if (mm_state.page_refcounts) {
        mm_memset(mm_state.page_refcounts, 0, mm_state.refcount_array_size);
    }
}

// Increment reference count for a physical page
void mm_incref_page(uint64_t phys_addr) {
    uint64_t idx = page_to_index(phys_addr);
    if (idx == (uint64_t)-1) return;
    
    if (mm_state.page_refcounts[idx] < 0xFFFF) {
        mm_state.page_refcounts[idx]++;
    }
}

// Decrement reference count - returns true if page should be freed
bool mm_decref_page(uint64_t phys_addr) {
    uint64_t idx = page_to_index(phys_addr);
    if (idx == (uint64_t)-1) return false;
    
    if (mm_state.page_refcounts[idx] > 0) {
        mm_state.page_refcounts[idx]--;
        return mm_state.page_refcounts[idx] == 0;
    }
    // refcount is 0 - this page was never shared, free it directly
    return true;
}

// Get reference count for a physical page
uint16_t mm_get_page_refcount(uint64_t phys_addr) {
    uint64_t idx = page_to_index(phys_addr);
    if (idx == (uint64_t)-1) return 0;
    
    return mm_state.page_refcounts[idx];
}
