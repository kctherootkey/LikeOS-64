# LikeOS-64

A minimal 64-bit operating system built from scratch, targeting x86_64 architecture with BIOS booting.

## Overview

LikeOS-64 is a simple operating system that demonstrates:
- Custom bootloader written in x86 assembly
- Transition from 16-bit real mode → 32-bit protected mode → 64-bit long mode
- Basic memory management with paging
- VGA text mode output
- Minimal C kernel

## Features

- **Custom Bootloader**: 512-byte bootloader that sets up GDT, enables paging, and switches to 64-bit mode
- **64-bit Kernel**: Minimal kernel written in C that displays text using VGA buffer
- **Memory Management**: Identity mapping of first 2MB using page tables
- **BIOS Compatible**: Works with legacy BIOS systems (not UEFI)

## Building

### Prerequisites

Make sure you have the following tools installed:
- `nasm` (Netwide Assembler)
- `gcc` (GNU Compiler Collection)
- `qemu-system-x86_64` (for testing)
- `make`

On Ubuntu/Debian:
```bash
sudo apt update
sudo apt install nasm gcc qemu-system-x86 build-essential
```

Or use the provided target:
```bash
make install-deps
```

### Compilation

Build the complete OS:
```bash
make
```

This will create:
- `build/boot.bin` - The bootloader
- `build/kernel.bin` - The 64-bit kernel
- `build/LikeOS.img` - The complete bootable OS image

### Running

Run the OS in QEMU:
```bash
make run
```

Or manually:
```bash
qemu-system-x86_64 -drive format=raw,file=build/LikeOS.img -m 128M
```

## Project Structure

```
LikeOS-64/
├── boot.asm      # Bootloader assembly code
├── boot64.asm    # 64-bit kernel entry point
├── kernel.c      # Main kernel code in C
├── linker.ld     # Linker script for kernel
├── Makefile      # Build system
└── README.md     # This file
```

## Technical Details

### Boot Process

1. **BIOS Stage**: BIOS loads the first 512 bytes (bootloader) to `0x7C00`
2. **Bootloader Stage**: 
   - Sets up segments and stack
   - Loads kernel from disk to `0x10000`
   - Sets up GDT for protected and long mode
   - Enables A20 line for >1MB memory access
   - Switches to 32-bit protected mode
   - Sets up page tables for long mode
   - Enables paging and enters 64-bit long mode
   - Copies kernel to `0x100000` (1MB mark)
   - Jumps to kernel entry point
3. **Kernel Stage**: 
   - Sets up stack at 2MB mark
   - Calls C kernel main function
   - Displays "LikeOS-64 Booting" message
   - Enters infinite loop with `hlt` instruction

### Memory Layout

- `0x00000000 - 0x000003FF`: Interrupt Vector Table
- `0x00000400 - 0x000004FF`: BIOS Data Area
- `0x00007C00 - 0x00007DFF`: Bootloader (512 bytes)
- `0x00010000 - 0x0001FFFF`: Kernel temporary location
- `0x00070000 - 0x00072FFF`: Page tables (12KB)
- `0x00100000 - 0x001FFFFF`: Kernel final location (1MB+)
- `0x00200000+`: Stack grows down from 2MB

### GDT Layout

- Null Descriptor
- 32-bit Code Segment (for protected mode)
- 32-bit Data Segment (for protected mode)
- 64-bit Code Segment (for long mode)
- 64-bit Data Segment (for long mode)

## Customization

### Changing the Boot Message

Edit `kernel.c` and modify the string in the `kernel_main()` function:
```c
print_string("Your Custom Message", 30, 12);
```

### Adding More Kernel Features

The kernel can be extended with:
- Interrupt handling (IDT setup)
- Keyboard input
- More sophisticated memory management
- File system support
- Process scheduling

## Debugging

Run with GDB debugging support:
```bash
make debug
```

Then in another terminal:
```bash
gdb
(gdb) target remote :1234
(gdb) set architecture i386:x86-64
```

## Troubleshooting

### Common Issues

1. **QEMU not found**: Install qemu-system-x86_64
2. **NASM not found**: Install nasm assembler
3. **Boot loops**: Check that the bootloader is exactly 512 bytes with proper signature
4. **No output**: Ensure VGA text mode is properly initialized

### Build Targets

- `make all` - Build everything
- `make run` - Build and run in QEMU
- `make clean` - Clean build files
- `make rebuild` - Clean and rebuild
- `make debug` - Run with debugging support
- `make help` - Show available targets

## License

This project is provided as educational material. Feel free to use and modify as needed.

## Contributing

This is a minimal educational OS. For learning purposes, try extending it with:
- Better error handling
- More CPU features (SSE, etc.)
- Hardware abstraction layer
- Device drivers
- User mode support
