# LikeOS-64 Minimal UEFI System

This directory contains a completely separate, minimal implementation of a C kernel and UEFI bootloader for learning and testing purposes.

## Overview

The minimal system consists of:
- **minimal_bootloader.c** - Simple UEFI bootloader (~150 lines)
- **minimal_kernel.c** - Minimal kernel that prints one message (~50 lines)  
- **minimal_console.c/.h** - Basic VGA text output (~100 lines)
- **Makefile_minimal** - Independent build system

## Key Features

- **Completely separate** from main kernel code
- **No dependencies** on existing kernel headers
- **Self-contained** build system
- **Standard UEFI** boot process
- **Minimal complexity** for learning

## Files Created

```
boot/uefi/
├── minimal_bootloader.c      - UEFI bootloader (GNU-EFI)
├── minimal_kernel.c          - Kernel entry point  
├── minimal_console.c         - VGA text output
├── minimal_console.h         - Console interface
├── Makefile_minimal          - Build system
├── README_minimal.md         - This documentation
└── build_minimal/            - Build output directory
    ├── minimal_kernel.bin    - Compiled kernel
    ├── minimal_bootloader.efi- UEFI executable
    └── minimal_system.iso    - Bootable ISO
```

## Building

```bash
cd boot/uefi
make -f Makefile_minimal       # Build everything
make -f Makefile_minimal test  # Test in QEMU
```

## Requirements

- GNU-EFI development packages
- OVMF firmware for UEFI testing
- Standard build tools (gcc, ld, objcopy)
- xorriso for ISO creation
- QEMU for testing

## How It Works

1. **UEFI Firmware** loads `BOOTX64.EFI` (our bootloader)
2. **Bootloader** loads `minimal_kernel.bin` into memory at 0x100000
3. **Bootloader** exits UEFI boot services and jumps to kernel
4. **Kernel** initializes VGA console and prints message
5. **Kernel** enters infinite halt loop

## Testing

```bash
# Build and test
make -f Makefile_minimal test

# Expected output in QEMU:
# - UEFI bootloader messages
# - "LikeOS-64 Kernel loaded" in white text
# - System halts cleanly
```

## Architecture

### Bootloader (minimal_bootloader.c)
- Uses GNU-EFI library
- Implements essential UEFI protocols only
- Loads kernel from file system
- Allocates memory at 0x100000
- Exits boot services cleanly

### Kernel (minimal_kernel.c)  
- Single entry point: `kernel_main()`
- Initializes minimal console
- Prints required message
- Infinite halt loop

### Console (minimal_console.c)
- Direct VGA buffer access (0xB8000)
- White text on black background
- Basic string/character output
- Screen scrolling support

## Integration

This minimal system is completely independent:
- Does NOT modify existing kernel code
- Does NOT interfere with main Makefile
- Does NOT use existing headers
- Can be built alongside main system

## Educational Value

Perfect for:
- Learning UEFI boot process
- Understanding kernel loading
- VGA text mode programming
- Minimal system architecture
- UEFI development basics

## Troubleshooting

**Build Issues:**
- Install gnu-efi-dev package
- Check OVMF firmware path
- Verify xorriso installation

**Boot Issues:**
- Ensure UEFI mode in QEMU
- Check OVMF firmware path
- Verify ISO creation success

**No Output:**
- VGA buffer may not be available
- Check UEFI graphics mode
- Try alternative OVMF version
