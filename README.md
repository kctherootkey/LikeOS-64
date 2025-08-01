# LikeOS-64

A modern 64-bit operating system built from scratch, targeting x86_64 architecture with **UEFI booting** and **framebuffer graphics**.

## Overview

LikeOS-64 is a sophisticated operating system that demonstrates:
- **UEFI bootloader** written in C using GNU-EFI
- **ELF64 kernel** with framebuffer-based graphics console
- **Advanced memory management** with paging and heap allocation
- **Interrupt handling** with IDT and keyboard support
- **Professional build system** with ISO, FAT, and USB image creation

## Features

### Core System
- **UEFI Bootloader**: Modern UEFI application that loads ELF64 kernels
- **Framebuffer Console**: High-resolution graphics console with built-in 8x16 font
- **64-bit ELF Kernel**: Professional kernel architecture with modular design
- **Memory Management**: Complete MM subsystem with physical/virtual memory and heap
- **Interrupt System**: Full IDT setup with keyboard interrupt handling

### Display & I/O
- **Graphics Console**: Framebuffer-based console supporting various resolutions
- **VGA Color Compatibility**: 16-color VGA palette support for legacy compatibility
- **Printf Family**: Complete printf implementation (kprintf, ksprintf, ksnprintf)
- **Keyboard Input**: PS/2 keyboard support with proper interrupt handling
- **Scrolling**: True framebuffer scrolling (no screen clearing)

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
│   │   └── console.c       # Framebuffer console implementation
│   ├── io/                 # Input/Output subsystem
│   │   └── keyboard.c      # PS/2 keyboard driver
│   └── mm/                 # Memory Management
│       └── memory.c        # Physical/virtual memory and heap
├── include/                # Header files
│   └── kernel/             # Kernel headers
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
   - Sets up interrupt descriptor table (IDT)
   - Initializes memory management subsystem
   - Sets up PS/2 keyboard driver
   - Enters interactive command loop

### Memory Layout (UEFI)

- `0x0000000000100000`: Kernel load address (1MB)
- `0x0000000080000000+`: Framebuffer base (varies by system)
- Dynamic UEFI memory map with proper virtual memory management

### Console System

The framebuffer console provides:
- **Resolutions**: Supports various UEFI graphics modes (tested at 1024x768)
- **Font Rendering**: Built-in 8x16 bitmap font with full ASCII support
- **Color Support**: 16-color VGA palette with RGB conversion
- **Text Operations**: Character rendering, string output, scrolling
- **Printf Support**: Full-featured printf family with format specifiers

### Interrupt Handling

- **IDT Setup**: 64-bit Interrupt Descriptor Table
- **Keyboard IRQ**: PS/2 keyboard on IRQ 1
- **Exception Handling**: Basic CPU exception handlers
- **Interrupt Controllers**: PIC configuration and management

### Memory Management

- **Physical Memory**: Detection and bitmap-based allocation
- **Virtual Memory**: Page table management with identity mapping
- **Heap Allocation**: Dynamic memory allocation (kalloc/kfree)
- **Memory Statistics**: Runtime memory usage reporting

## Customization

### Modifying the Kernel

The kernel entry point is in `kernel/ke/init.c`:
```c
void kernel_main(framebuffer_info_t* fb_info) {
    console_init(fb_info);
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

## Development

### Build System Features

```bash
make help        # Show all available targets
make clean       # Clean all build artifacts
make deps        # Install build dependencies
```

### Testing

The system supports various testing methods:
- **QEMU**: Full system emulation with UEFI firmware
- **Real Hardware**: Boot from USB on UEFI systems
- **Virtual Machines**: VMware, VirtualBox with UEFI support

### Debugging

Enable debugging features:
```bash
# Add debug symbols to kernel build
KERNEL_CFLAGS += -g -O0

# Run QEMU with GDB support
qemu-system-x86_64 -s -S -bios /usr/share/ovmf/OVMF.fd -cdrom build/LikeOS-64.iso
```

## Architecture Highlights

### UEFI Integration
- **Boot Services**: Proper UEFI application with boot services
- **Graphics Output**: Native framebuffer support
- **Memory Management**: UEFI memory map integration
- **Standards Compliance**: Follows UEFI 2.x specifications

### Modern Design
- **Modular Architecture**: Clean separation of kernel subsystems
- **Professional Build**: Industry-standard build system
- **Error Handling**: Comprehensive error checking and reporting
- **Documentation**: Well-documented code and APIs

## Troubleshooting

### Common Issues

1. **Missing UEFI firmware**: Install `ovmf` package
2. **Build failures**: Run `make deps` to install dependencies
3. **QEMU graphics issues**: Ensure OVMF firmware is properly installed
4. **No keyboard input**: Check that interrupts are enabled (`sti` instruction)

### Build Targets Reference

- `make all` - Build complete system (ISO + FAT + USB)
- `make kernel` - Build kernel ELF only
- `make bootloader` - Build UEFI bootloader only
- `make iso` - Create bootable ISO image
- `make fat` - Create FAT32 boot image
- `make usb` - Create USB bootable image
- `make qemu` - Run from ISO in QEMU
- `make qemu-fat` - Run from FAT image in QEMU
- `make clean` - Remove all build artifacts
- `make deps` - Install build dependencies

## License

This project is provided as educational material demonstrating modern operating system development techniques. Feel free to use and modify for learning purposes.

## Contributing

LikeOS-64 demonstrates modern OS development practices. Consider extending it with:
- **File Systems**: FAT32, ext2, or custom filesystem support
- **Network Stack**: TCP/IP implementation
- **User Mode**: Ring 3 user processes and system calls
- **SMP Support**: Multi-processor support
- **Advanced Graphics**: 2D/3D graphics acceleration
- **Security Features**: Memory protection and privilege separation
