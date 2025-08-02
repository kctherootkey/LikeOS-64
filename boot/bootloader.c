#include <efi.h>
#include <efilib.h>

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

// Kernel entry point type (now takes framebuffer info)
typedef void (*kernel_entry_t)(void* framebuffer_info);

// Framebuffer information structure
typedef struct {
    void* framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t bytes_per_pixel;
} framebuffer_info_t;

// Paging structures (assuming 4-level paging)
typedef struct {
    UINT64 entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Higher half kernel constants (must match kernel.lds and memory.h)
#define KERNEL_OFFSET    0xFFFFFFFF80000000ULL
#define KERNEL_START     0x0

// Physical memory locations for page tables
#define PML4_ADDRESS    0x1000
#define PDPT_ADDRESS    0x2000
#define PD_ADDRESS      0x3000   // Page directories start here (need multiple for 4GB)
#define PT_ADDRESS      0x8000   // Page tables start here (after page directories)

// Higher half page tables  
#define PDPT_HIGH_ADDRESS  0x9000
#define PD_HIGH_ADDRESS    0xA000
#define PT_HIGH_ADDRESS    0xB000

// Trampoline address (will be allocated below kernel)
static EFI_PHYSICAL_ADDRESS trampoline_addr = 0;

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

// Trampoline function type
typedef void (*trampoline_func_t)(UINT64 kernel_entry, void* framebuffer_info, UINT64 pml4_addr);

// External trampoline function from trampoline.S
extern void trampoline_jump(UINT64 kernel_entry, void* framebuffer_info, UINT64 pml4_addr);

// We need the end symbol to calculate the size
extern char trampoline_jump_end[];

// Set up higher half kernel paging with identity mapping for low memory
static void setup_higher_half_paging(UINT64 kernel_phys_addr, UINT64 kernel_size) {
    page_table_t *pml4 = (page_table_t*)PML4_ADDRESS;
    page_table_t *pdpt_low = (page_table_t*)PDPT_ADDRESS;
    
    // Higher half page tables
    page_table_t *pdpt_high = (page_table_t*)PDPT_HIGH_ADDRESS;
    page_table_t *pd_high = (page_table_t*)PD_HIGH_ADDRESS;
    page_table_t *pt_high = (page_table_t*)PT_HIGH_ADDRESS;
    
    // Clear main page tables
    for (int i = 0; i < 512; i++) {
        pml4->entries[i] = 0;
        pdpt_low->entries[i] = 0;
        pdpt_high->entries[i] = 0;
        pd_high->entries[i] = 0;
        pt_high->entries[i] = 0;
    }
    
    // Set up PML4 entries
    // Entry 0: Low memory identity mapping (0x0 - 0x7FFFFFFFFF) - EXECUTABLE
    pml4->entries[0] = PDPT_ADDRESS | PAGE_RWX;
    
    // Entry 511: Higher half mapping (0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF)
    pml4->entries[511] = PDPT_HIGH_ADDRESS | PAGE_RWX;
    
    // Set up low memory identity mapping for first 4GB
    // 4GB = 4 PDPT entries (each covers 1GB)
    for (int pdpt_i = 0; pdpt_i < 4; pdpt_i++) {
        // Allocate page directory for this 1GB region
        EFI_PHYSICAL_ADDRESS pd_addr = PD_ADDRESS + (pdpt_i * 4096);
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
    pdpt_high->entries[pdpt_index] = PD_HIGH_ADDRESS | PAGE_RWX;
    pd_high->entries[pd_index] = PT_HIGH_ADDRESS | PAGE_RWX;
    
    // Map kernel pages in higher half
    UINT64 kernel_pages = (kernel_size + 4095) / 4096;
    
    // Map additional pages for kernel stack and data (at least 16 pages = 64KB extra)
    UINT64 extra_pages = 16;
    UINT64 total_pages = kernel_pages + extra_pages;
    
    if (total_pages > (512 - pt_index)) {
        total_pages = 512 - pt_index; // Don't overflow page table
    }
    
    for (UINT64 i = 0; i < total_pages; i++) {
        UINT64 phys_addr = kernel_phys_addr + (i * 4096);
        // Kernel pages should be executable (contains code)
        pt_high->entries[pt_index + i] = phys_addr | PAGE_RWX;
    }
    
    Print(L"Higher half paging configured:\r\n");
    Print(L"  Identity mapped: 0x0 - 0x100000000 (4GB)\r\n");
    Print(L"  Kernel virtual: 0x%lx -> 0x%lx (%lu total pages, %lu kernel + %lu extra)\r\n", 
          kernel_virt, kernel_phys_addr, total_pages, kernel_pages, extra_pages);
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

// UEFI bootloader entry point
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *file_system;
    EFI_FILE_PROTOCOL *root_dir, *kernel_file;
    EFI_FILE_INFO *file_info;
    UINTN info_size, kernel_size;
    VOID *kernel_buffer;
    Elf64_Ehdr *elf_header;
    Elf64_Phdr *program_headers;
    kernel_entry_t kernel_entry;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    framebuffer_info_t fb_info;
    
    // Initialize GNU-EFI library
    InitializeLib(ImageHandle, SystemTable);
    
    Print(L"LikeOS-64 Enhanced UEFI Bootloader\r\n");
    Print(L"===================================\r\n");
    Print(L"ELF64 Loader with Paging Support\r\n\r\n");

    // Dump UEFI memory map for debugging
    //dump_memory_map();
    //for (;;) {}
    
    // Get loaded image protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, 
                               &LoadedImageProtocol, (VOID**)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get loaded image protocol: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get file system protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_image->DeviceHandle, 
                               &FileSystemProtocol, (VOID**)&file_system);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file system protocol: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Open root directory
    status = uefi_call_wrapper(file_system->OpenVolume, 2, file_system, &root_dir);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open root directory: %r\r\n", status);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    Print(L"Loading kernel.elf...\r\n");
    
    // Get Graphics Output Protocol for framebuffer info
    status = uefi_call_wrapper(BS->LocateProtocol, 3, &GraphicsOutputProtocol, NULL, (VOID**)&gop);
    if (EFI_ERROR(status)) {
        Print(L"WARNING: Could not get Graphics Output Protocol: %r\r\n", status);
        // Set up fallback VGA mode info
        fb_info.framebuffer_base = (void*)0xB8000;
        fb_info.framebuffer_size = 4000;
        fb_info.horizontal_resolution = 80;
        fb_info.vertical_resolution = 25;
        fb_info.pixels_per_scanline = 80;
        fb_info.bytes_per_pixel = 2;
    } else {
        // Get current mode information
        fb_info.framebuffer_base = (void*)gop->Mode->FrameBufferBase;
        fb_info.framebuffer_size = (uint32_t)gop->Mode->FrameBufferSize;
        fb_info.horizontal_resolution = gop->Mode->Info->HorizontalResolution;
        fb_info.vertical_resolution = gop->Mode->Info->VerticalResolution;
        fb_info.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
        
        // Calculate bytes per pixel based on pixel format
        switch (gop->Mode->Info->PixelFormat) {
            case PixelRedGreenBlueReserved8BitPerColor:
            case PixelBlueGreenRedReserved8BitPerColor:
                fb_info.bytes_per_pixel = 4;
                break;
            case PixelBitMask:
                fb_info.bytes_per_pixel = 4; // Assume 32-bit
                break;
            default:
                fb_info.bytes_per_pixel = 4; // Safe default
                break;
        }
        
        Print(L"Framebuffer: 0x%lx, Size: %d bytes\r\n", fb_info.framebuffer_base, fb_info.framebuffer_size);
        Print(L"Resolution: %dx%d, BPP: %d\r\n", fb_info.horizontal_resolution, fb_info.vertical_resolution, fb_info.bytes_per_pixel);
    }
    
    // Open kernel file (now ELF format)
    status = uefi_call_wrapper(root_dir->Open, 5, root_dir, &kernel_file, 
                               L"kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open kernel.elf: %r\r\n", status);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Get file size
    info_size = sizeof(EFI_FILE_INFO) + 256;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID**)&file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory for file info: %r\r\n", status);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    status = uefi_call_wrapper(kernel_file->GetInfo, 4, kernel_file, 
                               &GenericFileInfo, &info_size, file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file info: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    kernel_size = (UINTN)file_info->FileSize;
    Print(L"ELF kernel size: %d bytes\r\n", kernel_size);
    
    // Allocate memory for the entire ELF file
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, kernel_size, &kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory for ELF file: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Read entire ELF file into memory
    status = uefi_call_wrapper(kernel_file->Read, 3, kernel_file, &kernel_size, kernel_buffer);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not read kernel file: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        Print(L"System halted. Press any key to continue...\r\n");
        uefi_call_wrapper(ST->ConIn->Reset, 2, ST->ConIn, FALSE);
        EFI_INPUT_KEY key;
        uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key);
        return status;
    }
    
    // Parse ELF header
    elf_header = (Elf64_Ehdr*)kernel_buffer;
    
    if (!validate_elf64(elf_header)) {
        Print(L"ERROR: Invalid ELF64 file\r\n");
        uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
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
            Print(L"Loading segment %d: vaddr=0x%lx, paddr=0x%lx, size=%ld\r\n", 
                  i, phdr->p_vaddr, phdr->p_paddr, phdr->p_memsz);
            
            // Allocate memory for this segment using AllocateAnyPages
            UINTN pages = (phdr->p_memsz + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS segment_addr = 0;
            
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages, 
                                       EfiLoaderCode, pages, &segment_addr);
            if (EFI_ERROR(status)) {
                Print(L"ERROR: Could not allocate memory: %r\r\n", status);
                uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
                uefi_call_wrapper(BS->FreePool, 1, file_info);
                uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
                uefi_call_wrapper(root_dir->Close, 1, root_dir);
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
    
    // Clean up
    uefi_call_wrapper(BS->FreePool, 1, file_info);
    uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
    uefi_call_wrapper(root_dir->Close, 1, root_dir);
    
    // Get memory map for exit boot services
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_key, descriptor_size;
    UINT32 descriptor_version;
    
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
    memory_map_size += 2 * descriptor_size;  // Add some extra space
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
    setup_higher_half_paging(kernel_phys_addr, kernel_size_total);
    
    Print(L"About to exit boot services. Kernel entry: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Trampoline at: 0x%lx\r\n", trampoline_addr);

    // Step 3: Exit boot services
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        // Try one more time in case the memory map changed
        uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                          &map_key, &descriptor_size, &descriptor_version);
        status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
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
    trampoline(elf_header->e_entry, &fb_info, PML4_ADDRESS);
    
    // Should never reach here
    return EFI_SUCCESS;
}
