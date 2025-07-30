# LikeOS-64 Technical Documentation

## Overview

LikeOS-64 is a minimal 64-bit operating system that demonstrates the complete boot process from BIOS to a functioning kernel. This document explains the technical details of how each component works.

## Boot Process Flow

### Stage 1: BIOS (Basic Input/Output System)
1. Power-on self-test (POST)
2. Load Master Boot Record (MBR) from first sector of bootable device
3. Transfer control to bootloader at physical address 0x7C00

### Stage 2: Bootloader (boot.asm)
1. **Initialize Environment** (Real Mode - 16-bit)
   - Clear interrupts
   - Set up segment registers (DS, ES, SS)
   - Initialize stack pointer

2. **Display Boot Message**
   - Use BIOS interrupt 0x10 to print to screen
   - Show "LikeOS-64 Bootloader Starting..."

3. **Load Kernel from Disk**
   - Use BIOS interrupt 0x13 (disk services)
   - Load 32 sectors (16KB) starting from sector 2
   - Load to temporary location 0x10000

4. **Prepare for Protected Mode**
   - Set up Global Descriptor Table (GDT)
   - Enable A20 line for >1MB memory access

5. **Enter Protected Mode** (32-bit)
   - Set CR0.PE bit to enter protected mode
   - Jump to 32-bit code segment

6. **Prepare for Long Mode**
   - Set up page tables for identity mapping
   - Enable Physical Address Extension (PAE)
   - Enable long mode in EFER MSR
   - Enable paging to activate long mode

7. **Enter Long Mode** (64-bit)
   - Jump to 64-bit code segment
   - Copy kernel to final location (1MB)
   - Transfer control to kernel

### Stage 3: Kernel (kernel.c + boot64.asm)
1. **Assembly Entry Point** (boot64.asm)
   - Set up 64-bit stack at 2MB
   - Call C kernel main function

2. **C Kernel** (kernel.c)
   - Clear VGA text buffer
   - Display "LikeOS-64 Booting" message
   - Enter infinite loop with HLT instruction

## Memory Layout

```
Physical Memory Layout:
0x00000000 - 0x000003FF   Interrupt Vector Table (IVT)
0x00000400 - 0x000004FF   BIOS Data Area (BDA)
0x00000500 - 0x00007BFF   Free conventional memory
0x00007C00 - 0x00007DFF   Bootloader (loaded by BIOS)
0x00007E00 - 0x0000FFFF   Free space
0x00010000 - 0x0001FFFF   Kernel temporary location
0x00020000 - 0x0006FFFF   Free space
0x00070000 - 0x00072FFF   Page tables (12KB)
0x00073000 - 0x0007FFFF   Free space
0x00080000 - 0x0009FFFF   Extended BIOS Data Area
0x000A0000 - 0x000BFFFF   Video memory
0x000C0000 - 0x000FFFFF   BIOS ROM
0x00100000 - 0x001FFFFF   Kernel final location (1MB+)
0x00200000+               Stack (grows downward from 2MB)
```

## Global Descriptor Table (GDT)

The GDT contains descriptors for different memory segments:

1. **Null Descriptor** (0x00)
   - Required first entry, all zeros

2. **32-bit Code Segment** (0x08)
   - Used during protected mode transition
   - Base: 0x00000000, Limit: 0xFFFFF
   - Access: Present, Ring 0, Code, Execute/Read

3. **32-bit Data Segment** (0x10)
   - Used during protected mode
   - Base: 0x00000000, Limit: 0xFFFFF
   - Access: Present, Ring 0, Data, Read/Write

4. **64-bit Code Segment** (0x18)
   - Used in long mode
   - Long mode bit set, compatibility mode disabled

5. **64-bit Data Segment** (0x20)
   - Used in long mode for data segments

## Page Table Structure

For 64-bit long mode, we use 4-level paging:

1. **PML4 (Page Map Level 4)** at 0x70000
   - Points to PDPT

2. **PDPT (Page Directory Pointer Table)** at 0x71000
   - Points to PD

3. **PD (Page Directory)** at 0x72000
   - Contains 2MB page entry for identity mapping

The page tables identity map the first 2MB of physical memory, meaning virtual address equals physical address for this range.

## CPU Mode Transitions

### Real Mode → Protected Mode
1. Set up GDT
2. Enable A20 line
3. Set CR0.PE = 1
4. Far jump to reload CS with protected mode selector

### Protected Mode → Long Mode
1. Set up page tables
2. Enable PAE (CR4.PAE = 1)
3. Load CR3 with page table base
4. Set EFER.LME = 1 (Long Mode Enable)
5. Set CR0.PG = 1 (enable paging, activates long mode)
6. Far jump to 64-bit code segment

## VGA Text Mode

The kernel uses VGA text mode for output:
- Buffer location: 0xB8000
- Format: 80x25 characters
- Each character: 2 bytes (character + attribute)
- Attribute format: 4-bit background + 4-bit foreground color

## Compilation Process

1. **Bootloader**: NASM assembles boot.asm to raw binary
2. **Kernel Entry**: NASM assembles boot64.asm to ELF64 object
3. **Kernel C Code**: GCC compiles kernel.c to ELF64 object
4. **Linking**: LD links objects using custom linker script
5. **Image Creation**: DD combines bootloader and kernel into disk image

## Build Tools Used

- **NASM**: Netwide Assembler for x86/x64 assembly
- **GCC**: GNU Compiler Collection for C compilation
- **LD**: GNU Linker for linking object files
- **DD**: Unix tool for copying and converting files
- **QEMU**: x86 emulator for testing

## Debugging Tips

1. **QEMU Monitor**: Press Ctrl+Alt+2 for QEMU monitor
2. **GDB**: Use `make debug` to run with GDB support
3. **Hexdump**: Check binary files with `hexdump -C file.bin`
4. **Objdump**: Disassemble with `objdump -d file.o`

## Error Troubleshooting

### Boot Loops
- Check boot signature (0x55AA at offset 510-511)
- Verify bootloader size is exactly 512 bytes
- Check GDT setup and segment selectors

### Triple Fault
- Usually caused by invalid page tables or GDT
- Check page table alignment (4KB boundaries)
- Verify long mode transition sequence

### No Kernel Output
- Check kernel loading from disk
- Verify memory copy from temporary to final location
- Check VGA buffer access (0xB8000)

### Build Errors
- Ensure all tools are installed (nasm, gcc, ld)
- Check compiler flags for 64-bit compilation
- Verify linker script syntax

## Extensions and Improvements

This minimal OS can be extended with:

1. **Interrupt Handling**
   - Set up IDT (Interrupt Descriptor Table)
   - Handle keyboard, timer, and system call interrupts

2. **Memory Management**
   - Dynamic page allocation
   - Virtual memory management
   - Heap implementation

3. **Hardware Support**
   - PCI device enumeration
   - Disk drivers (ATA/SATA)
   - Network card drivers

4. **User Mode**
   - Task State Segment (TSS) setup
   - System call interface
   - Process management

5. **File System**
   - FAT32 or custom file system
   - Virtual File System (VFS) layer

## Standards and References

- Intel 64 and IA-32 Architectures Software Developer's Manual
- AMD64 Architecture Programmer's Manual
- Multiboot Specification (for bootloader compatibility)
- System V ABI for x86-64 (calling conventions)

This OS follows standard PC architecture conventions and should work on any x86_64 system with BIOS firmware.
