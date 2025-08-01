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

// Physical memory locations for page tables
#define PML4_ADDRESS    0x1000
#define PDPT_ADDRESS    0x2000
#define PD_ADDRESS      0x3000
#define PT_ADDRESS      0x4000

// Page table entry flags
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)

// Helper function to set up identity paging for low memory + kernel mapping
static void setup_paging(UINT64 kernel_vaddr, UINT64 kernel_paddr, UINT64 kernel_size) {
    page_table_t *pml4 = (page_table_t*)PML4_ADDRESS;
    page_table_t *pdpt = (page_table_t*)PDPT_ADDRESS;
    page_table_t *pd = (page_table_t*)PD_ADDRESS;
    page_table_t *pt = (page_table_t*)PT_ADDRESS;
    
    // Clear page tables
    for (int i = 0; i < 512; i++) {
        pml4->entries[i] = 0;
        pdpt->entries[i] = 0;
        pd->entries[i] = 0;
        pt->entries[i] = 0;
    }
    
    // Set up PML4 -> PDPT
    pml4->entries[0] = PDPT_ADDRESS | PAGE_PRESENT | PAGE_WRITABLE;
    
    // Set up PDPT -> PD for first 1GB (identity mapping)
    pdpt->entries[0] = PD_ADDRESS | PAGE_PRESENT | PAGE_WRITABLE;
    
    // Set up PD -> PT for first 2MB (identity mapping)
    pd->entries[0] = PT_ADDRESS | PAGE_PRESENT | PAGE_WRITABLE;
    
    // Identity map first 2MB (0x0 - 0x200000)
    for (int i = 0; i < 512; i++) {
        pt->entries[i] = (i * 4096) | PAGE_PRESENT | PAGE_WRITABLE;
    }
    
    // If kernel is loaded above 2MB, we need additional mappings
    if (kernel_vaddr >= 0x200000) {
        UINT64 kernel_pde_index = kernel_vaddr / (2 * 1024 * 1024);  // 2MB pages
        if (kernel_pde_index < 512) {
            // Use 2MB pages for simplicity
            pd->entries[kernel_pde_index] = (kernel_paddr & ~0x1FFFFF) | PAGE_PRESENT | PAGE_WRITABLE | (1ULL << 7); // PS bit for 2MB pages
        }
    }
}

// Load CR3 with our page table
static void enable_paging(void) {
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r" ((UINT64)PML4_ADDRESS)
        : "memory"
    );
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
    
    // Get loaded image protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, 
                               &LoadedImageProtocol, (VOID**)&loaded_image);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get loaded image protocol: %r\r\n", status);
        return status;
    }
    
    // Get file system protocol
    status = uefi_call_wrapper(BS->HandleProtocol, 3, loaded_image->DeviceHandle, 
                               &FileSystemProtocol, (VOID**)&file_system);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file system protocol: %r\r\n", status);
        return status;
    }
    
    // Open root directory
    status = uefi_call_wrapper(file_system->OpenVolume, 2, file_system, &root_dir);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open root directory: %r\r\n", status);
        return status;
    }
    
    Print(L"Loading minimal_kernel.elf...\r\n");
    
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
                               L"minimal_kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not open minimal_kernel.elf: %r\r\n", status);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        return status;
    }
    
    // Get file size
    info_size = sizeof(EFI_FILE_INFO) + 256;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (VOID**)&file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory for file info: %r\r\n", status);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
        return status;
    }
    
    status = uefi_call_wrapper(kernel_file->GetInfo, 4, kernel_file, 
                               &GenericFileInfo, &info_size, file_info);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get file info: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
        uefi_call_wrapper(root_dir->Close, 1, root_dir);
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
        return EFI_INVALID_PARAMETER;
    }
    
    Print(L"Valid ELF64 x86-64 kernel\r\n");
    Print(L"Entry point: 0x%lx\r\n", elf_header->e_entry);
    Print(L"Program headers: %d\r\n", elf_header->e_phnum);
    
    // Get program headers
    program_headers = (Elf64_Phdr*)((UINT8*)kernel_buffer + elf_header->e_phoff);
    
    // Load program segments
    for (int i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr *phdr = &program_headers[i];
        
        if (phdr->p_type == PT_LOAD) {
            Print(L"Loading segment %d: vaddr=0x%lx, paddr=0x%lx, size=%ld\r\n", 
                  i, phdr->p_vaddr, phdr->p_paddr, phdr->p_memsz);
            
            // Allocate memory for this segment
            UINTN pages = (phdr->p_memsz + 4095) / 4096;
            EFI_PHYSICAL_ADDRESS segment_addr = phdr->p_paddr;
            
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, 
                                       EfiLoaderCode, pages, &segment_addr);
            if (EFI_ERROR(status)) {
                Print(L"ERROR: Could not allocate memory at 0x%lx: %r\r\n", phdr->p_paddr, status);
                uefi_call_wrapper(BS->FreePool, 1, kernel_buffer);
                uefi_call_wrapper(BS->FreePool, 1, file_info);
                uefi_call_wrapper(kernel_file->Close, 1, kernel_file);
                uefi_call_wrapper(root_dir->Close, 1, root_dir);
                return status;
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
        return status;
    }
    
    // Allocate buffer for memory map
    memory_map_size += 2 * descriptor_size;  // Add some extra space
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, 
                               memory_map_size, (VOID**)&memory_map);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not allocate memory map: %r\r\n", status);
        return status;
    }
    
    // Get the actual memory map
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                               &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Could not get memory map: %r\r\n", status);
        uefi_call_wrapper(BS->FreePool, 1, memory_map);
        return status;
    }
    
    // Set up our own paging tables BEFORE exiting boot services
    // setup_paging(elf_header->e_entry, elf_header->e_entry, 0x200000);
    
    Print(L"About to exit boot services. Kernel entry: 0x%lx\r\n", elf_header->e_entry);
    
    // Exit boot services
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        // Try one more time in case the memory map changed
        uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size, memory_map, 
                          &map_key, &descriptor_size, &descriptor_version);
        status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            return status;
        }
    }
    
    // Don't enable custom paging for now - use UEFI's paging
    // enable_paging();
    
    // Jump to kernel entry point
    kernel_entry = (kernel_entry_t)elf_header->e_entry;
    kernel_entry(&fb_info);
    
    // Should never reach here
    return EFI_SUCCESS;
}
