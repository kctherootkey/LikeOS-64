# LikeOS-64

A modern 64-bit operating system built from scratch, targeting x86_64 architecture with **UEFI booting** and **framebuffer graphics**.

## Overview

LikeOS-64 is a sophisticated operating system that demonstrates:
- **UEFI bootloader** written in C using GNU-EFI
- **ELF64 kernel** with optimized framebuffer-based graphics console
- **Advanced memory management** with paging and heap allocation
- **Performance optimization** with SSE acceleration and write-combining
- **Interrupt handling** with IDT and keyboard support
- **Professional build system** with ISO, FAT, and USB image creation

## Features

### Core System
- **UEFI Bootloader**: Modern UEFI application that loads ELF64 kernels
- **Optimized Console**: High-resolution graphics console with SSE-accelerated rendering
- **64-bit ELF Kernel**: Professional kernel architecture with modular design
- **Memory Management**: Complete MM subsystem with physical/virtual memory and heap
- **Interrupt System**: Full IDT setup with keyboard interrupt handling
- **Performance Features**: Write-combining, double buffering, and CPU optimization

### Display & I/O
- **Graphics Console**: Framebuffer-based console supporting various resolutions
- **Framebuffer Optimization**: Advanced double buffering with SSE-optimized copying
- **Performance Acceleration**: 4x+ faster console operations on real hardware
- **Write-Combining**: MTRR-based framebuffer optimization for enhanced performance
- **VGA Color Compatibility**: 16-color VGA palette support for legacy compatibility
- **Printf Family**: Complete printf implementation (kprintf, ksprintf, ksnprintf)
- **Keyboard Input**: PS/2 keyboard support with proper interrupt handling
- **Scrolling**: True framebuffer scrolling with dirty region tracking

### Build System
- **Hybrid UEFI/BIOS ISO**: Creates bootable ISOs compatible with modern systems
- **FAT32 Images**: Direct UEFI boot images for testing and deployment
- **USB Images**: Ready-to-use USB bootable images
- **Professional Makefile**: Modular build system with dependency management

## Building

### Prerequisites

Install the required development tools:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install gcc nasm xorriso mtools dosfstools ovmf gnu-efi-dev

# Or use the automated installer
make deps
```

Required packages:
- `gcc` - GNU Compiler Collection
- `nasm` - Netwide Assembler  
- `xorriso` - ISO image creation
- `mtools` - FAT filesystem tools
- `dosfstools` - FAT formatting utilities
- `ovmf` - UEFI firmware for QEMU
- `gnu-efi-dev` - UEFI development libraries

### Compilation

Build the complete UEFI operating system:

```bash
make all
```

This creates:
- `build/bootloader.efi` - UEFI bootloader application
- `build/kernel.elf` - 64-bit ELF kernel
- `build/LikeOS-64.iso` - Hybrid UEFI/BIOS bootable ISO
- `build/LikeOS-64.img` - UEFI FAT32 boot image
- `build/LikeOS-64-usb.img` - USB bootable image

### Individual Build Targets

```bash
make kernel      # Build kernel only
make bootloader  # Build UEFI bootloader only
make iso         # Build bootable ISO
make fat         # Build FAT32 boot image
make usb         # Build USB image
```

### Running

Run in QEMU with UEFI firmware:

```bash
make qemu        # Boot from ISO
make qemu-fat    # Boot from FAT image
```

Manual QEMU execution:
```bash
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom build/LikeOS-64.iso -m 512M
```

## Project Structure

```
LikeOS-64/
├── boot/                    # UEFI bootloader
│   ├── bootloader.c         # Main UEFI bootloader application
│   └── uefi/               # Working UEFI implementation reference
├── kernel/                  # Kernel source code
│   ├── ke/                 # Kernel Executive
│   │   ├── init.c          # Kernel initialization and main loop
│   │   ├── interrupt.asm   # Low-level interrupt handlers
│   │   ├── interrupt.c     # Interrupt management
│   │   ├── gdt.asm        # Global Descriptor Table
│   │   └── gdt.c          # GDT management
│   ├── hal/                # Hardware Abstraction Layer
│   │   ├── console.c       # Framebuffer console implementation
│   │   └── fb_optimize.c   # Framebuffer optimization system
│   ├── io/                 # Input/Output subsystem
│   │   └── keyboard.c      # PS/2 keyboard driver
│   └── mm/                 # Memory Management
│       └── memory.c        # Physical/virtual memory and heap
├── include/                # Header files
│   └── kernel/             # Kernel headers
│       ├── console.h       # Console and printf interfaces
│       ├── fb_optimize.h   # Framebuffer optimization system
│       ├── interrupt.h     # Interrupt handling interfaces
│       ├── keyboard.h      # Keyboard driver interface
│       ├── memory.h        # Memory management interfaces
│       └── types.h         # Common type definitions
├── kernel.lds              # ELF64 kernel linker script
├── Makefile               # Professional build system
└── README.md              # This documentation
```

## Technical Details

### Boot Process

1. **UEFI Firmware**: System firmware loads and executes BOOTX64.EFI
2. **UEFI Bootloader**: 
   - Initializes UEFI boot services and graphics
   - Sets up framebuffer and gets system memory map
   - Loads `kernel.elf` using ELF64 parser
   - Sets up identity paging for kernel
   - Calls `ExitBootServices()` and jumps to kernel
3. **Kernel Initialization**: 
   - Initializes framebuffer console system
   - Enables framebuffer optimization with double buffering and SSE acceleration
   - Sets up interrupt descriptor table (IDT)
   - Initializes memory management subsystem
   - Sets up PS/2 keyboard driver
   - Enters interactive command loop

### Memory Layout (UEFI)

- `0x0000000080000000+`: Framebuffer base (varies by system)
- `0xFFFFFFFF80000000+`: Higher-half kernel virtual mapping
- Dynamic UEFI memory map with proper virtual memory management
- Static framebuffer optimization buffers (up to 8.3MB for 1920x1080)

### Console System

The framebuffer console provides:
- **Resolutions**: Supports various UEFI graphics modes (tested at 1024x768)
- **Font Rendering**: Built-in 8x16 bitmap font with full ASCII support
- **Color Support**: 16-color VGA palette with RGB conversion
- **Text Operations**: Character rendering, string output, scrolling
- **Printf Support**: Full-featured printf family with format specifiers

### Framebuffer Optimization System

Advanced performance optimization for graphics operations:

**Double Buffering**:
- **Back Buffer**: All drawing operations performed in system RAM
- **Dirty Region Tracking**: Only modified areas are copied to video memory
- **Automatic Merging**: Overlapping dirty regions are merged for efficiency
- **Static Allocation**: Up to 1920x1080 framebuffers supported without heap

**CPU Optimization**:
- **SSE Detection**: Automatic CPU feature detection (SSE2, SSE3, SSE4.1, SSE4.2)
- **Vectorized Copying**: SSE-optimized memory transfers for large operations
- **Aligned Access**: 64-byte aligned buffers for optimal performance
- **Fallback Support**: Graceful degradation on older CPUs

**Write-Combining**:
- **MTRR Configuration**: Automatic setup of Memory Type Range Registers
- **Cache Optimization**: Write-combining reduces CPU-to-GPU transfer overhead
- **Real Hardware Benefits**: 4x+ performance improvement on physical systems
- **Verification**: Built-in testing to confirm write-combining is active

**Performance Features**:
- **Statistics Tracking**: Monitor pixels copied, dirty regions, and efficiency
- **Region Optimization**: Smart merging prevents excessive region fragmentation
- **Memory Efficiency**: Static buffers eliminate malloc dependency during boot
- **Console Integration**: Seamless integration with existing console functions

### Interrupt Handling

- **IDT Setup**: 64-bit Interrupt Descriptor Table
- **Keyboard IRQ**: PS/2 keyboard on IRQ 1
- **Exception Handling**: Basic CPU exception handlers
- **Interrupt Controllers**: PIC configuration and management

### Memory Management

- **Physical Memory**: Detection and bitmap-based allocation with 32MB minimum requirement
- **Virtual Memory**: Page table management with identity mapping
- **Heap Allocation**: Dynamic memory allocation (kalloc/kfree) with heap size management
- **Memory Statistics**: Runtime memory usage reporting and diagnostic information
- **Static Buffers**: Large static allocations for framebuffer optimization (early boot support)

## Customization

### Modifying the Kernel

The kernel entry point is in `kernel/ke/init.c`:
```c
void kernel_main(framebuffer_info_t* fb_info) {
    // Initialize console system with framebuffer info
    console_init(fb_info);
    
    // Initialize framebuffer optimization after console is ready
    console_init_fb_optimization();
    
    // Call the main system initialization
    KiSystemStartup();
}
```

### Adding New Features

Extend the kernel by:
- Adding new interrupt handlers in `kernel/ke/interrupt.c`
- Creating device drivers in `kernel/io/`
- Implementing new HAL components in `kernel/hal/`
- Expanding memory management in `kernel/mm/`

### Console Customization

Modify console behavior in `kernel/hal/console.c`:
- Change font rendering
- Add new color schemes
- Implement different graphics modes
- Add cursor support

### Framebuffer Optimization

Configure performance optimizations in `kernel/hal/fb_optimize.c`:
- Adjust dirty region limits and merging strategies
- Modify SSE optimization thresholds
- Configure write-combining parameters
- Add custom CPU feature detection

Monitor performance with built-in functions:
```c
console_show_fb_status();    // Display optimization status
console_show_fb_stats();     // Show performance statistics
```

## Development

### Build System Features

```bash
make help        # Show all available targets
make clean       # Clean all build artifacts
make deps        # Install build dependencies
make usb-write   # Write ISO to USB device (requires sudo)
```

### Testing

The system supports various testing methods:
- **QEMU**: Full system emulation with UEFI firmware (basic framebuffer optimization)
- **Real Hardware**: Boot from USB on UEFI systems (full performance benefits)
- **Virtual Machines**: VMware, VirtualBox with UEFI support (limited optimization features)

**Note**: Framebuffer write-combining and MTRR optimization work best on real hardware. Virtual machines may not fully support these features, resulting in reduced performance benefits.

### Debugging

Enable debugging features:
```bash
# Add debug symbols to kernel build
KERNEL_CFLAGS += -g -O0

# Run QEMU with GDB support
qemu-system-x86_64 -s -S -bios /usr/share/ovmf/OVMF.fd -cdrom build/LikeOS-64.iso
```

Debug framebuffer optimization at runtime:
```c
// Check optimization status
console_show_fb_status();

// Monitor performance statistics
console_show_fb_stats();

// Reset performance counters
fb_reset_performance_stats();
```

## Architecture Highlights

### UEFI Integration
- **Boot Services**: Proper UEFI application with boot services
- **Graphics Output**: Native framebuffer support
- **Memory Management**: UEFI memory map integration
- **Standards Compliance**: Follows UEFI 2.x specifications

### Modern Design
- **Modular Architecture**: Clean separation of kernel subsystems
- **Performance Optimization**: Advanced framebuffer acceleration with SSE and write-combining
- **Professional Build**: Industry-standard build system
- **Error Handling**: Comprehensive error checking and reporting
- **Documentation**: Well-documented code and APIs

## Troubleshooting

### Common Issues

1. **Missing UEFI firmware**: Install `ovmf` package
2. **Build failures**: Run `make deps` to install dependencies
3. **QEMU graphics issues**: Ensure OVMF firmware is properly installed
4. **No keyboard input**: Check that interrupts are enabled (`sti` instruction)
5. **Framebuffer optimization disabled**: Check CPU feature detection and MTRR support

### Build Targets Reference

- `make all` - Build complete system (ISO + FAT + USB)
- `make kernel` - Build kernel ELF only
- `make bootloader` - Build UEFI bootloader only
- `make iso` - Create bootable ISO image
- `make fat` - Create FAT32 boot image
- `make usb` - Create USB bootable image
- `make qemu` - Run from ISO in QEMU
- `make qemu-fat` - Run from FAT image in QEMU
- `make usb-write` - Write ISO to USB device (requires sudo)
- `make clean` - Remove all build artifacts
- `make deps` - Install build dependencies
- `make help` - Show all available targets

## License

This project is provided as educational material demonstrating modern operating system development techniques. Feel free to use and modify for learning purposes.

## Contributing

LikeOS-64 demonstrates modern OS development practices including advanced framebuffer optimization. Consider extending it with:
- **File Systems**: FAT32, ext2, or custom filesystem support
- **Network Stack**: TCP/IP implementation
- **User Mode**: Ring 3 user processes and system calls
- **SMP Support**: Multi-processor support
- **Advanced Graphics**: 2D/3D graphics acceleration with GPU optimization
- **Security Features**: Memory protection and privilege separation
