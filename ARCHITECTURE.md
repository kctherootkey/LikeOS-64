# LikeOS-64 Professional Kernel Architecture

## Directory Structure

LikeOS-64 now follows a professional operating system kernel structure similar to modern OS designs:

```
LikeOS-64/
├── boot/                    # Boot and initialization
│   ├── bootloader.asm      # 16-bit real mode bootloader
│   └── entry64.asm         # 64-bit kernel entry point
├── kernel/                  # Kernel subsystems
│   ├── ke/                 # Kernel Executive (core kernel)
│   │   ├── init.c          # System initialization (KiSystemStartup)
│   │   ├── interrupt.c     # Interrupt and exception management
│   │   ├── interrupt.asm   # Low-level interrupt handlers
│   │   ├── gdt.c           # Global Descriptor Table management
│   │   └── gdt.asm         # GDT loading routines
│   ├── hal/                # Hardware Abstraction Layer
│   │   └── console.c       # VGA console and printf implementation
│   ├── io/                 # I/O Subsystem
│   │   └── keyboard.c      # PS/2 keyboard driver
│   └── mm/                 # Memory Management (future expansion)
├── include/                # Header files
│   └── kernel/             # Kernel headers
│       ├── console.h       # HAL console interface
│       ├── interrupt.h     # Interrupt management interface
│       └── keyboard.h      # I/O keyboard driver interface
├── build/                  # Build output (generated)
└── *.ld, Makefile         # Build system
```

## Subsystem Architecture

### Kernel Executive (ke/)
The core kernel services including:
- **init.c**: `KiSystemStartup()` - Main system initialization
- **interrupt.c/asm**: IDT, IRQ, and exception handling
- **gdt.c/asm**: Global Descriptor Table and TSS management

### Hardware Abstraction Layer (hal/)
Platform-specific hardware interfaces:
- **console.c**: VGA text mode console with full printf implementation

### I/O Subsystem (io/)
Device drivers and I/O management:
- **keyboard.c**: PS/2 keyboard driver with scan code translation

### Memory Management (mm/)
Future expansion for virtual memory, paging, and heap management.

## Build System

Enhanced Makefile with proper dependency tracking:

```bash
make          # Build floppy disk image
make run      # Build and run in QEMU
make debug    # Build and run with GDB debugging
make iso      # Create bootable ISO image
make usb      # Create bootable USB image
make clean    # Clean build artifacts
make help     # Show all available targets
```

## Professional Naming Conventions

Following standard OS kernel conventions:
- **Ki**: Kernel Internal functions (KiSystemStartup)
- **Hal**: Hardware Abstraction Layer prefix
- **Io**: I/O subsystem prefix  
- **Mm**: Memory Management prefix (future)

## Header Organization

Proper include structure with:
- Include guards: `_KERNEL_SUBSYSTEM_H_`
- Modular dependencies
- Clean interfaces between subsystems

This structure provides a solid foundation for expanding LikeOS-64 into a full-featured operating system with proper separation of concerns and professional code organization.
