# LikeOS-64

A modern 64-bit operating system built from scratch, featuring a Unix-like userland with process management, syscalls, multitasking, and currently a simple shell.

### Current state

* Real hardware support: The OS has been tested with one Lenovo Notebook by booting from an USB stick. For most of real hardware the operating system boots from Linux bootstrapper that runs qemu right after the boot sequence. This is accomplished by creating a bootable usb stick with the make linux-usb and make linux-usb-write targets.
* QEMU: The os can be also booted into qemu on for example windows or linux hosts by using the make qemu-usb target.
* VMWare: The os can also be run in VMware with USB 3.1 enabled through the make usb-write target (USB drive boot).
* VirtualBox: Is also supported with xHCI (USB 3) selected and the ICH9 chipset as a setting.

Be sure to always enable UEFI booting for the VM. Once the os is running you can type "help" to get the shell help text.

https://github.com/user-attachments/assets/69c8a79c-cc04-4bbe-8e2a-3067befd1708

LikeOS-64 running inside qemu on an Ubuntu host.

https://github.com/user-attachments/assets/4026c84f-08db-44f6-9677-a5a9447e4654

LikeOS-64 running on a Lenovo Notebook through the Linux bootstrapper and embedded qemu.

## Overview

LikeOS-64 is a Unix-like operating system demonstrating modern OS development:
- **Complete Userland**: Ring 3 processes with full syscall interface (fork, execve, wait, signals)
- **Process Management**: Multitasking scheduler
- **UEFI Bootloader**: Modern boot using GNU-EFI with ELF64 kernel loading
- **Filesystem Support**: FAT32 read/write filesystem with VFS layer and devfs
- **Hardware Drivers**: USB 3.0 (xHCI), USB mass storage, PS/2 keyboard/mouse, PCI enumeration
- **Advanced Memory**: Physical/virtual memory, SLAB allocator, heap management, SMEP/SMAP/NX
- **Signal System**: POSIX signal support with handlers and sigreturn
- **TTY Subsystem**: Terminal I/O with POSIX termios control
- **Performance**: SSE-optimized framebuffer, write-combining, double buffering

## Features

### Core Operating System
- **UEFI Bootloader**: Modern UEFI application using GNU-EFI, loads ELF64 kernels
- **64-bit Long Mode**: Full x86-64 architecture with higher-half kernel mapping
- **Boot Information**: Framebuffer info and UEFI memory map passed to kernel
- **Identity Paging**: Initial setup with transition to higher-half virtual memory

### Process Management & Userland
- **Multitasking Scheduler**: Multitasking scheduler
- **User Mode Execution**: Ring 3 processes with kernel/user separation
- **ELF Loader**: Dynamic ELF64 binary loading and execution
- **Process Control**: `fork()`, `execve()`, `exit()`, `wait4()` system calls
- **Process Groups**: `setpgid()`, `getpgrp()`, session management
- **Userland C Library**: Complete libc with malloc, stdio, string functions, crt0
- **User Programs**: Shell, cat, ls, pwd, stat, and test utilities
- **Bash Port**: Full Bash 5.2.21 shell ported to LikeOS-64 is planned

### System Calls & IPC
- **Syscall Interface**: Fast SYSCALL/SYSRET mechanism
- **File Operations**: `open()`, `read()`, `write()`, `close()`, `lseek()`, `dup()`, `dup2()`
- **Memory Management**: `brk()`, `mmap()`, `munmap()` for dynamic allocation
- **Process Info**: `getpid()`, `getppid()`, `getuid()`, `getgid()`
- **Pipes**: Anonymous pipes with `pipe()` syscall for IPC
- **Signals**: Full POSIX signal support (see below)

### Signal System
- **Signal Delivery**: Signal delivery to user processes
- **Signal Handlers**: User-space signal handler registration and execution
- **Signal Frame**: Stack-based signal context saving and restoration
- **Sigreturn**: `rt_sigreturn` for returning from signal handlers
- **Signal Actions**: `sigaction()`, `sigprocmask()`, `kill()`, `tkill()`
- **Timer Signals**: SIGALRM support with alarm() and interval timers
- **Core Signals**: SIGINT, SIGTERM, SIGCHLD, SIGSEGV, SIGILL, etc.
- **Signal Queuing**: Pending signal queue per process

### Filesystem & Storage
- **VFS Layer**: Virtual filesystem abstraction with mount point support
- **FAT32**: Read-only FAT32 driver for USB mass storage devices
- **DevFS**: Device filesystem (`/dev`) with character and block devices
- **Block Layer**: Generic block device interface for storage drivers
- **USB Mass Storage**: USB MSD class driver for USB thumb drives
- **File Descriptors**: Per-process file descriptor table (up to 1024 FDs)
- **File Operations**: Standard POSIX file I/O interface

### Hardware Abstraction Layer (HAL)
- **USB 3.0 (xHCI)**: Full xHCI host controller driver with interrupt support
- **USB Devices**: USB Mass Storage Device (MSD) class driver
- **PCI**: PCI/PCIe enumeration and configuration
- **PS/2**: Keyboard and mouse drivers via PS/2 controller
- **Serial Port**: Debug output via COM1 (useful for VMware/QEMU)
- **Timer**: Programmable Interval Timer (PIT) for scheduling
- **Framebuffer Console**: Graphics console
- **Mouse**: PS/2 mouse support with interrupt handling

### Memory Management
- **Physical Memory**: Bitmap-based page frame allocator
- **Virtual Memory**: 4-level page tables (PML4, PDPT, PD, PT)
- **Kernel Heap**: First-fit allocator with `kalloc()` / `kfree()`
- **SLAB Allocator**: Advanced kernel object caching (8 bytes to 8 KB)
- **DMA Allocation**: Low-memory allocator for device DMA buffers
- **Address Spaces**: Per-process page tables with COW (copy-on-write)
- **Memory Protection**: NX (No-Execute), SMEP, SMAP security features
- **User Memory**: Safe `copy_from_user()` / `copy_to_user()` with SMAP
- **Page Table Pool**: Pre-allocated page tables for performance

### TTY & Terminal
- **TTY Layer**: Terminal I/O abstraction with line discipline
- **Termios**: POSIX terminal control (canonical/raw mode, echo, signals)
- **Console TTY**: Primary console terminal device (`/dev/console`)
- **Job Control**: Foreground/background process groups
- **Cursor Support**: Blinking cursor with configurable visibility

### Display & Graphics
- **Framebuffer Console**: UEFI GOP-based graphics console
- **Double Buffering**: Back-buffer with dirty region tracking
- **SSE Optimization**: Vectorized memory copies (SSE2/SSE3/SSE4.1/SSE4.2)
- **Write-Combining**: MTRR-based framebuffer optimization (4x+ speedup)
- **VGA Colors**: 16-color VGA palette with RGB conversion
- **Font Rendering**: Built-in 8x16 bitmap font, full ASCII support
- **Scrolling**: Hardware scrolling with scrollback buffer
- **Printf Family**: Full `kprintf()`, `ksprintf()`, `ksnprintf()` implementation
- **Scrollbar**: Visual scrollbar widget for UI elements

### Interrupt & Exception Handling
- **IDT**: 64-bit Interrupt Descriptor Table with 256 entries
- **PIC**: 8259 PIC configuration and EOI handling
- **IRQ Routing**: IMCR routing to PIC mode
- **Exception Handlers**: CPU exception handling (page faults, GPF, etc.)
- **IRQ Handlers**: Device interrupt handling (keyboard, mouse, USB, timer)
- **TSS**: Task State Segment for kernel stack switching
- **Interrupt Safety**: Proper interrupt enable/disable and nesting

### Build System
- **Modular Makefile**: Professional build with dependency tracking
- **Multiple Outputs**: ISO (UEFI/BIOS hybrid), FAT32 image, USB image
- **User Programs**: Separate userland build with static libc linking
- **Linux USB Host**: Debian-based USB stick that auto-boots LikeOS via QEMU/KVM
- **Dependency Management**: Automated installation of build dependencies
- **Clean Targets**: Proper cleanup of all build artifacts

## Building

### Prerequisites

Install the required development tools:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install gcc nasm xorriso mtools dosfstools ovmf gnu-efi-dev \
                 debootstrap parted gdisk qemu-utils grub-efi-amd64-bin rsync

# Or use the automated installer
make deps
```

Required packages:
- `gcc` - GNU Compiler Collection (x86-64)
- `nasm` - Netwide Assembler for assembly code
- `xorriso` - ISO 9660 image creation
- `mtools` - FAT filesystem manipulation tools
- `dosfstools` - FAT formatting utilities (`mkfs.fat`)
- `ovmf` - UEFI firmware for QEMU testing
- `gnu-efi-dev` - UEFI development libraries and headers
- `debootstrap` - Debian bootstrap for Linux USB host (optional)
- `parted`, `gdisk` - Partition management for USB images
- `qemu-utils`, `grub-efi-amd64-bin`, `rsync` - Additional build tools

### Compilation

Build the complete operating system:

```bash
make all
```

This creates:
- `build/bootloader.efi` - UEFI bootloader (BOOTX64.EFI)
- `build/kernel.elf` - 64-bit ELF kernel with all drivers
- `build/*.o` - Kernel and user program object files
- `build/bin/` - User programs (sh, cat, ls, pwd, stat, etc.)
- `build/LikeOS-64.iso` - Bootable ISO (UEFI/BIOS hybrid)
- `build/LikeOS-64.img` - UEFI FAT32 boot image
- `build/LikeOS-64-usb.img` - USB bootable image with filesystem
- `build/msdata.img` - Data partition image for USB
- `userland/libc/libc.a` - Static C library for userland

### Individual Build Targets

```bash
make kernel          # Build kernel.elf only
make bootloader      # Build UEFI bootloader only
make user-programs   # Build all userland programs
make iso             # Build bootable ISO image
make fat             # Build FAT32 boot image
make usb             # Build USB image with data partition
make linux-usb       # Build Debian USB host that auto-launches LikeOS
```

### Running

#### QEMU/VMWare/VirtualBox with UEFI firmware

```bash
make qemu-usb        # Boot from USB image with FAT32 filesystem
make qemu-realusb    # Run QEMU with real USB device as xHCI storage (requires USB_DEVICE=/dev/sdX)
make usb-write       # Write to USB device with GPT (requires USB_DEVICE=/dev/sdX)
```

#### Linux USB Host

Build a minimal Debian USB stick that auto-launches LikeOS in QEMU/KVM:

```bash
make linux-usb                        # Build the host Linux image
make linux-usb-write USB_DEVICE=/dev/sdX  # Write to USB device
```

Boot the USB stick on real hardware: GRUB (timeout 0) → minimal X11 → full-screen QEMU with LikeOS ISO, giving the impression of booting directly into the hobby OS.

#### Manual QEMU Execution

```bash
make clean && make qemu-usb
```

#### Write to USB Device

```bash
make usb-write USB_DEVICE=/dev/sdX   # Write ISO to USB (requires sudo)
```

**Warning**: This will erase all data on the USB device!

## Project Structure

```
LikeOS-64/
├── boot/                           # UEFI bootloader
│   ├── bootloader.c                # Main UEFI bootloader (ELF64 loader)
│   ├── console.c/h                 # Early boot console
│   ├── trampoline.S                # Assembly trampoline for kernel jump
│   └── uefi/                       # Minimal UEFI reference implementation
│       ├── minimal_bootloader.c
│       ├── minimal_kernel.c
│       └── minimal_console.c/h
├── kernel/                         # Kernel source code
│   ├── ke/                         # Kernel Executive
│   │   ├── init.c                  # Kernel initialization and main loop
│   │   ├── sched.c                 # Process scheduler and context switching
│   │   ├── interrupt.asm/c         # Interrupt/exception handlers
│   │   ├── syscall.asm/c           # System call interface
│   │   ├── gdt.asm/c               # Global Descriptor Table setup
│   │   ├── timer.c                 # PIT timer for preemptive scheduling
│   │   ├── shell.c                 # Shell process management
│   │   ├── elf_loader.c            # ELF64 binary loader
│   │   ├── signal.c                # POSIX signal implementation
│   │   ├── pipe.c                  # Anonymous pipe implementation
│   │   ├── tty.c                   # TTY layer and termios
│   │   ├── storage.c               # Storage/filesystem bootstrap
│   │   ├── xhci_boot.c             # xHCI initialization
│   │   └── stack_guard.c           # Stack overflow protection
│   ├── hal/                        # Hardware Abstraction Layer
│   │   ├── console.c               # Framebuffer console driver
│   │   ├── fb_optimize.c           # Framebuffer optimization (SSE, MTRR)
│   │   ├── xhci.c                  # USB 3.0 xHCI controller driver
│   │   ├── usb.c                   # Generic USB device handling
│   │   ├── usb_msd.c               # USB Mass Storage driver
│   │   ├── pci.c                   # PCI/PCIe enumeration
│   │   ├── block.c                 # Block device layer
│   │   ├── ps2.c                   # PS/2 controller initialization
│   │   ├── mouse.c                 # PS/2 mouse driver
│   │   ├── serial.c                # Serial port debug output
│   │   ├── ioapic.c                # IO APIC interrupt controller
│   │   └── scrollbar.c             # UI scrollbar widget
│   ├── io/                         # Input/Output subsystem
│   │   └── keyboard.c              # PS/2 keyboard driver
│   ├── mm/                         # Memory Management
│   │   ├── memory.c                # Physical/virtual memory, paging, heap
│   │   ├── slab.c                  # SLAB allocator for kernel objects
│   │   └── stack_switch.asm        # Stack switching assembly
│   └── fs/                         # Filesystem drivers
│       ├── vfs.c                   # Virtual filesystem layer
│       ├── fat32.c                 # FAT32 read-only driver
│       └── devfs.c                 # Device filesystem (/dev)
├── include/kernel/                 # Kernel headers
│   ├── console.h                   # Console and printf interfaces
│   ├── fb_optimize.h               # Framebuffer optimization system
│   ├── interrupt.h                 # Interrupt handling interfaces
│   ├── keyboard.h, mouse.h         # Input device interfaces
│   ├── memory.h                    # Memory management interfaces
│   ├── slab.h                      # SLAB allocator interface
│   ├── sched.h                     # Scheduler and task structures
│   ├── syscall.h                   # System call numbers and interface
│   ├── signal.h                    # Signal definitions and structures
│   ├── elf.h                       # ELF64 format definitions
│   ├── tty.h                       # TTY and termios interfaces
│   ├── vfs.h                       # VFS layer interface
│   ├── fat32.h, devfs.h            # Filesystem interfaces
│   ├── xhci.h, usb.h, usb_msd.h    # USB stack interfaces
│   ├── pci.h, block.h, storage.h   # Hardware interfaces
│   ├── pipe.h                      # Pipe interface
│   ├── timer.h, ioapic.h, ps2.h    # Device driver interfaces
│   ├── scrollbar.h, serial.h       # UI and debug interfaces
│   └── types.h                     # Common type definitions
├── user/                           # User programs
│   ├── sh.c                        # Simple shell (fallback)
│   ├── cat.c, ls.c, pwd.c, stat.c  # Core utilities
│   ├── hello.c                     # Hello world example
│   ├── test_libc.c                 # C library test suite
│   ├── test_syscalls.c             # Syscall test suite
│   ├── testmem.c, memstat.c        # Memory testing
│   ├── progerr.c, teststress.c     # Error/stress testing
│   ├── Makefile                    # User program build
│   └── user.lds                    # User program linker script
├── userland/libc/                  # Userland C library
│   ├── src/
│   │   ├── crt0.S                  # Program startup (entry point)
│   │   ├── malloc/malloc.c         # Heap allocator (malloc, free, etc.)
│   │   ├── stdio/stdio.c           # Standard I/O (printf, FILE, etc.)
│   │   ├── string/string.c         # String functions (strlen, strcpy, etc.)
│   │   ├── stdlib/stdlib.c         # Standard utilities (atoi, strtol, etc.)
│   │   ├── ctype/ctype.c           # Character classification
│   │   └── syscalls/
│   │       ├── syscall.h           # Inline syscall wrappers
│   │       └── unistd.c            # POSIX syscalls (read, write, fork, etc.)
│   ├── include/                    # Public libc headers
│   │   ├── stdio.h, stdlib.h, string.h, unistd.h, errno.h
│   │   ├── stddef.h, stdint.h, stdarg.h, ctype.h
│   │   └── sys/types.h
│   ├── Makefile                    # Build static library
│   ├── libc.a                      # Output: static C library
│   └── README.md                   # Libc documentation
├── ports/                          # Third-party software ports
│   └── bash-5.2.21/                # Ported Bash shell
├── host/linux-usb/                 # Linux USB host environment
│   ├── create-rootfs.sh            # Debian debootstrap script
│   ├── rootfs-overlay/             # Custom files for USB host
│   └── README.md                   # USB host documentation
├── kernel.lds                      # Kernel linker script (higher-half)
├── Makefile                        # Main build system
├── coding-style.rst.txt            # Linux kernel coding style reference
└── README.md                       # This documentation
```

## Technical Details

### Boot Process

1. **UEFI Firmware**: System firmware loads `EFI/BOOT/BOOTX64.EFI`
2. **UEFI Bootloader** (`boot/bootloader.c`):
   - Initializes UEFI boot services and serial debug output
   - Configures Graphics Output Protocol (GOP) for framebuffer
   - Obtains UEFI memory map for kernel memory manager
   - Loads `kernel.elf` from filesystem using UEFI file protocols
   - Parses ELF64 headers and loads program segments
   - Sets up identity paging for kernel boot
   - Stores boot information (framebuffer, memory map) for kernel
   - Calls `ExitBootServices()` and jumps to kernel via trampoline
3. **Kernel Initialization** (`kernel/ke/init.c`):
   - `kernel_main()`: Receives boot info from bootloader
   - Initializes framebuffer console with GOP information
   - Enables framebuffer optimization (double buffer, SSE, MTRR)
   - Sets up interrupt system (IDT, PIC, IRQs)
   - Initializes memory management (physical, virtual, heap, SLAB)
   - Enables NX, SMEP, SMAP security features
   - Remaps kernel with proper page permissions
   - Switches to higher-half kernel stack
   - Initializes SYSCALL/SYSRET for fast system calls
   - Initializes PCI, VFS, devfs, TTY subsystems
   - Sets up PS/2 keyboard and mouse
   - Initializes USB 3.0 (xHCI) controller
   - Initializes scheduler and timer for multitasking
   - Spawns initial shell process (`/bin/sh`)
   - Enters main loop: shell tick, USB poll, storage poll, scheduler

### Memory Layout

**Physical Memory**:
- `0x0000000000000000 - 0x00000000000FFFFF`: Low 1 MB (reserved, UEFI)
- `0x0000000000100000+`: Kernel physical load address
- Framebuffer: Varies by system (typically high physical address)
- Page tables: Dynamically allocated from UEFI

**Virtual Memory** (Higher-Half Kernel):
- `0x0000000000000000 - 0x00007FFFFFFFFFFF`: User space (128 TB)
  - User programs, stacks, heaps
  - Per-process address space with isolated page tables
- `0xFFFF800000000000 - 0xFFFFBFFFFFFFFFFF`: Direct physical map (64 TB)
  - Direct mapping of all physical memory
  - Used for framebuffer access after identity mapping removed
  - Offset = physical address + 0xFFFF800000000000
- `0xFFFFFFFF80000000 - 0xFFFFFFFFFFFFFFFF`: Kernel space (2 GB)
  - Kernel code (.text) - Read-only, executable
  - Kernel rodata (.rodata) - Read-only, NX
  - Kernel data (.data) - Read-write, NX
  - Kernel BSS (.bss) - Read-write, NX
  - Kernel heap (dynamic, expands upward)
  - Kernel stacks (per-process kernel stacks)

**Memory Protection**:
- NX bit: Data pages are non-executable
- SMEP: Kernel cannot execute user-space code
- SMAP: Kernel cannot access user-space data without explicit enable
- Per-process isolation: Each process has its own page tables

### System Call Interface

**Mechanism**: Uses fast SYSCALL/SYSRET instructions (MSR-based)

**Entry Point**: `kernel/ke/syscall.asm` - Assembly stub that:
1. Saves user context (registers, stack pointer)
2. Switches to kernel stack (TSS-based)
3. Calls C handler `syscall_handler()` in `kernel/ke/syscall.c`
4. Handles system call dispatch based on syscall number
5. Returns result to user via SYSRET

**Syscall Numbers**: Linux-compatible where possible (see `include/kernel/syscall.h`)
- 0-60: Core syscalls (read, write, open, close, fork, execve, exit, wait, etc.)
- 200+: Extended syscalls (stat, access, chdir, getcwd, ioctl, etc.)

**Register Convention** (x86-64 System V ABI):
- `rax`: Syscall number (input), return value (output)
- `rdi, rsi, rdx, r10, r8, r9`: Arguments 1-6
- `rcx, r11`: Saved by SYSCALL instruction (user RIP, RFLAGS)

### Process Management

**Task Structure** (`include/kernel/sched.h`):
- Stack pointer, page table base (PML4), entry point
- Process ID (PID), parent PID (PPID)
- Process group ID (PGID), session ID (SID)
- State (running, ready, blocked, stopped, zombie)
- Privilege level (kernel/user)
- File descriptor table (1024 FDs per process)
- Signal state (pending signals, handlers, masks)
- Memory map regions (mmap tracking)

**Scheduler** (`kernel/ke/sched.c`):
- Multitasking scheduling
- Timer-based context switching (100 Hz)
- Separate kernel and user stacks per task
- Context switch assembly in `kernel/mm/stack_switch.asm`

**Process Creation**:
1. `fork()`: Duplicate current process with copy-on-write
2. `execve()`: Load new ELF binary, replace address space
3. ELF loader parses binary, maps pages, sets up stack with argc/argv/envp
4. User-space crt0 (`userland/libc/src/crt0.S`) calls `main()`

**Process Termination**:
1. `exit()`: Process terminates, becomes zombie
2. Parent `wait4()`: Reaps zombie, releases resources
3. Orphaned zombies: Reaped by scheduler

### Signal System

**Signal Delivery** (`kernel/ke/signal.c`):
- Signals delivered during return-to-user (syscall return, interrupt return)
- Signal handler executed in user space on user stack
- Signal frame pushed to user stack contains:
  - Saved context (registers, instruction pointer, stack pointer)
  - Signal number and siginfo structure
  - Return address to `__restore_rt` trampoline

**Signal Return**:
- User signal handler completes
- Calls `__restore_rt()` in libc (see `userland/libc/src/crt0.S`)
- Issues `rt_sigreturn` syscall
- Kernel restores saved context from signal frame
- Execution resumes at interrupted location

### Filesystem Architecture

**VFS Layer** (`kernel/fs/vfs.c`):
- Mount point management
- File operations: open, read, write, close, lseek
- Directory operations: readdir, stat
- File descriptor allocation and management

**FAT32 Driver** (`kernel/fs/fat32.c`):
- Read/Write FAT32 support
- Mounted from USB mass storage devices
- Boot sector parsing, cluster chain following
- Long filename (LFN) support

**DevFS** (`kernel/fs/devfs.c`):
- Virtual device filesystem mounted at `/dev`
- Character devices: console, tty, null, zero
- Block devices: USB storage devices (sd0, sd1, etc.)

### Framebuffer Optimization

**Double Buffering** (`kernel/hal/fb_optimize.c`):
- Back buffer: All drawing in system RAM
- Dirty region tracking: Only changed areas copied to VRAM
- Automatic region merging: Coalesces overlapping updates
- `fb_flush_dirty_regions()`: Copies back buffer to framebuffer

**SSE Acceleration**:
- CPU feature detection: Checks for SSE2, SSE3, SSE4.1, SSE4.2
- Vectorized memcpy for large transfers (16-byte aligned)
- Fallback to standard memcpy on older CPUs
- 64-byte aligned buffers for optimal cache performance

**Write-Combining** (`kernel/mm/memory.c`):
- MTRR (Memory Type Range Register) configuration
- Sets framebuffer region to write-combining type
- Reduces CPU-to-GPU transfer overhead
- Performance improvement: 4x+ on real hardware
- Limited benefit in virtualized environments

### USB 3.0 Stack

**xHCI Driver** (`kernel/hal/xhci.c`):
- PCI-based xHCI controller discovery
- Command, event, transfer ring management
- Device enumeration and configuration
- Interrupt-based event handling (if enabled)
- Bulk transfer support for mass storage

**USB Mass Storage** (`kernel/hal/usb_msd.c`):
- USB Mass Storage Device (MSD) class driver
- SCSI command encapsulation (CBW/CSW protocol)
- Read/write operations via bulk endpoints
- Integration with block layer

## Customization

### Kernel Entry Point

The kernel entry point is in [kernel/ke/init.c](kernel/ke/init.c):
```c
void kernel_main(boot_info_t* boot_info) {
    // Initialize console with framebuffer from bootloader
    console_init((framebuffer_info_t*)&boot_info->fb_info);
    console_init_fb_optimization();
    
    // Call main system initialization
    system_startup(boot_info);
}
```

### Adding System Calls

1. Define syscall number in [include/kernel/syscall.h](include/kernel/syscall.h)
2. Implement handler in [kernel/ke/syscall.c](kernel/ke/syscall.c) in `syscall_handler()`
3. Add userland wrapper in [userland/libc/src/syscalls/unistd.c](userland/libc/src/syscalls/unistd.c)
4. Add prototype to [userland/libc/include/unistd.h](userland/libc/include/unistd.h)

### Adding User Programs

1. Create source file in [user/](user/) directory (e.g., `myapp.c`)
2. Add program to `USER_PROGRAMS` in [user/Makefile](user/Makefile)
3. Link with libc: `gcc -o myapp myapp.o -L../userland/libc -lc`
4. Program will be included in `/bin` directory on filesystem

### Creating Device Drivers

**Character Device**:
1. Implement driver in [kernel/hal/](kernel/hal/) or [kernel/io/](kernel/io/)
2. Register device in devfs: `devfs_register_device(name, type, ops)`
3. Implement file operations: `open`, `read`, `write`, `close`

**Block Device**:
1. Implement driver in [kernel/hal/](kernel/hal/)
2. Register with block layer: `block_register_device()`
3. Implement block read/write operations
4. Device appears in `/dev` as `sd0`, `sd1`, etc.

### Modifying Console Behavior

Modify [kernel/hal/console.c](kernel/hal/console.c):
- Change font rendering (8x16 bitmap in code)
- Adjust color palette (16-color VGA)
- Implement cursor shapes
- Add new text effects

### Framebuffer Optimization Tuning

Configure in [kernel/hal/fb_optimize.c](kernel/hal/fb_optimize.c):
- Adjust dirty region limits (`FB_MAX_DIRTY_REGIONS`)
- Modify region merging strategy
- Change SSE threshold for optimization
- Tune write-combining parameters

Monitor performance:
```c
console_show_fb_status();    // Display optimization status
console_show_fb_stats();     // Show performance statistics
fb_reset_performance_stats(); // Reset counters
```

## Development

### Build System Features

```bash
make help              # Show all available targets
make clean             # Clean all build artifacts
make deps              # Install build dependencies (Ubuntu/Debian)
make usb-write         # Write image to USB device (requires sudo)
make linux-usb         # Build Debian host USB that auto-launches LikeOS
make user-programs     # Build all userland programs
make userland          # Build userland C library (libc.a)
```

### Available Make Targets

- `all` - Build complete system (ISO + FAT + USB images)
- `kernel` - Build kernel.elf only
- `bootloader` - Build UEFI bootloader only
- `user-programs` - Build all user programs in [user/](user/)
- `userland` - Build userland C library
- `iso` - Create bootable ISO image (UEFI/BIOS hybrid)
- `fat` - Create FAT32 boot image
- `usb` - Create USB image with data partition
- `data-img` - Create data partition image with user programs
- `qemu` - Run from ISO in QEMU
- `qemu-fat` - Run from FAT image in QEMU
- `qemu-usb` - Run from USB image in QEMU
- `usb-write` - Write ISO to USB device (set USB_DEVICE=/dev/sdX)
- `linux-usb` - Build Debian USB host with auto-launch
- `linux-usb-write` - Write Linux host image to USB
- `clean` - Remove all build artifacts
- `deps` - Install build dependencies
- `help` - Display help message

### Testing

The system supports various testing methods:

**QEMU/KVM** (Recommended for development):
```bash
make qemu              # Fast iteration, UEFI firmware
make qemu-usb          # Test with FAT32 filesystem
```
- Full system emulation with UEFI
- Fast boot times for rapid development
- Limited framebuffer optimization (no MTRR in VM)
- GDB support: `qemu-system-x86_64 -s -S ...`

**VirtualBox**:
- Enable UEFI boot in VM settings
- Select ICH9 chipset
- Enable USB 3.0 (xHCI) controller
- Attach ISO as optical drive or boot from USB

**VMware**:
- Enable UEFI boot
- Enable USB 3.1 support
- Good performance with USB pass-through
- Serial port output for debugging

**Real Hardware** (Best performance):
```bash
make usb-write USB_DEVICE=/dev/sdX
# Or use Linux USB host:
make linux-usb-write USB_DEVICE=/dev/sdX
```
- Full framebuffer optimization (SSE, MTRR, write-combining)
- 4x+ performance improvement over QEMU
- Requires USB 3.0 port for xHCI support
- UEFI boot mandatory

**User Program Testing**:
```bash
# Build and test individual programs
cd user
make cat               # Build cat utility
make test_libc         # Build libc test suite
make test_syscalls     # Build syscall test
```

### Debugging

**Enable Kernel Debug Symbols**:
```bash
# Edit Makefile, add to KERNEL_CFLAGS:
KERNEL_CFLAGS += -g -O0
make clean all
```

**Serial Debug Output**:
- Kernel outputs to COM1 (0x3F8)
- Enable in QEMU: `-serial stdio`
- Enable in VMware: Add serial port to VM
- View output: `tail -f /tmp/serial.log`

**Runtime Debugging**:
```c
// Memory statistics
mm_print_memory_stats();
mm_print_heap_stats();

// Framebuffer stats
console_show_fb_status();
console_show_fb_stats();

// SLAB allocator stats
slab_print_stats();

// Process information
kprintf("Current PID: %d\n", sched_current()->id);
```

**Common Debug Techniques**:
1. **Page Faults**: Check CR2 register, page table mappings
2. **GPF**: Check segment selectors, privilege levels
3. **Triple Fault**: Usually bad page tables or stack corruption
4. **Hang**: Check interrupt enable flag (STI), timer interrupts
5. **Memory Corruption**: Enable heap validation, use SLAB stats

## Architecture Highlights

### Unix-like Design
- **Process Model**: Fork/exec model with proper process isolation
- **System Calls**: SYSCALL/SYSRET fast path with Linux-compatible numbers
- **Signal System**: POSIX-compliant signals with user-space handlers
- **File Descriptors**: Per-process FD table with standard streams (0, 1, 2)
- **Job Control**: Process groups, sessions, controlling TTY
- **Pipes**: Anonymous pipes for inter-process communication

### UEFI Integration
- **Boot Services**: Proper UEFI application using GNU-EFI
- **Graphics Output Protocol**: Native framebuffer support
- **Memory Map**: UEFI memory map integration with kernel allocator
- **File System Protocol**: ELF kernel loading from UEFI filesystem
- **Standards Compliance**: UEFI 2.x specification conformance

### Modern Security Features
- **NX Bit**: Non-executable data pages (W^X enforcement)
- **SMEP**: Supervisor Mode Execution Prevention (kernel can't execute user code)
- **SMAP**: Supervisor Mode Access Prevention (kernel can't access user data)
- **Ring Separation**: Full privilege level isolation (Ring 0 kernel, Ring 3 user)
- **Stack Canaries**: Stack overflow detection with guard pages
- **Address Space Isolation**: Per-process page tables with COW

### Performance Optimizations
- **Framebuffer**: SSE2/SSE3/SSE4 vectorized memory operations
- **Write-Combining**: MTRR-based framebuffer caching (4x+ speedup)
- **Double Buffering**: Dirty region tracking for minimal VRAM writes
- **SLAB Allocator**: Fast kernel object allocation with caching
- **Page Table Pool**: Pre-allocated page tables for fast mapping
- **Fast Syscalls**: SYSCALL/SYSRET instead of INT 0x80

### Modular Architecture
- **Clean Separation**: HAL, filesystem, memory, process layers well-defined
- **Portable Drivers**: Hardware abstraction allows driver reuse
- **VFS Layer**: Filesystem-independent operations
- **Block Layer**: Generic block device abstraction
- **Professional Build**: Dependency tracking, modular compilation

## Troubleshooting

### Common Build Issues

**Missing UEFI firmware**:
```bash
sudo apt install ovmf
```

**Build failures**:
```bash
make deps    # Install all dependencies
make clean   # Clean and rebuild
make all
```

**GNU-EFI not found**:
```bash
sudo apt install gnu-efi-dev
```

**xorriso errors**:
```bash
sudo apt install xorriso
```

### Runtime Issues

**QEMU graphics issues**:
- Ensure OVMF firmware is installed: `/usr/share/ovmf/OVMF.fd`
- Try different graphics modes: `-vga std` or `-vga virtio`
- Check UEFI boot: Verify OVMF loads bootloader

**No keyboard input**:
- Interrupts may be disabled: Check `sti` instruction in kernel
- PS/2 controller not initialized: Check `ps2_init()` output
- Check IRQ1 is enabled: `irq_enable(1)`

**System hangs/triple faults**:
- Page table corruption: Check CR3 and page mappings
- Identity mapping removed too early: Check boot sequence
- Bad interrupt handler: Verify IDT entries

**Framebuffer optimization disabled**:
- Check CPU features: May not support SSE
- MTRR not available: Check CPU support
- Virtual machine: MTRR may not be exposed
- Use `console_show_fb_status()` to diagnose

**USB devices not detected**:
- Ensure USB 3.0 (xHCI) is enabled in VM settings
- Check PCI enumeration: `pci_enumerate()` output
- Verify xHCI controller found: Check debug output
- Try different USB port: Use USB 3.0 port

**File system not mounting**:
- Check FAT32 signature: Must be valid FAT32
- USB MSD not initialized: Check xHCI and USB MSD drivers
- Data partition: Ensure `msdata.img` is properly created
- Read errors: Check block device read operations

**Userland programs crash**:
- Missing libc: Ensure userland/libc is built
- Syscall errors: Check syscall numbers match kernel
- ELF loading: Verify ELF header and segments
- Stack issues: Check stack pointer initialization in crt0
- Signal handling: Verify signal frame setup

**Process won't start**:
- Check `/bin` directory exists on filesystem
- Verify ELF binary is valid: Use `readelf -h`
- Check file permissions: Must be readable
- Memory exhausted: Check available memory

### Debugging Tips

**Serial output not working**:
- QEMU: Add `-serial stdio` to command line
- VMware: Add serial port to VM configuration
- Check serial_init() and serial_puts() calls

**Memory corruption**:
```c
mm_validate_heap();          // Validate kernel heap
heap_validate("location");   // Check heap at specific point
slab_print_stats();          // Check SLAB allocator
```

**Process/scheduler issues**:
```c
task_t* current = sched_current();
kprintf("PID=%d, state=%d\n", current->id, current->state);
```

**Signal debugging**:
```c
// Check pending signals
task_t* t = sched_current();
kprintf("Pending: 0x%lx, Blocked: 0x%lx\n", 
        t->sig_state.pending, t->sig_state.blocked);
```

## Implementation Status

### Completed Features ✓
- [x] UEFI bootloader with ELF64 loading
- [x] Higher-half kernel with proper memory mapping
- [x] Physical and virtual memory management
- [x] Heap allocator and SLAB allocator
- [x] Interrupt handling (IDT, PIC, exceptions)
- [x] PS/2 keyboard and mouse drivers
- [x] Framebuffer console with SSE optimization
- [x] Write-combining and MTRR setup
- [x] PCI enumeration and BAR assignment
- [x] USB 3.0 (xHCI) host controller driver
- [x] USB Mass Storage Device class driver
- [x] FAT32 read-only filesystem
- [x] VFS layer with mount points
- [x] Device filesystem (devfs)
- [x] Process scheduler (preemptive multitasking)
- [x] System call interface (SYSCALL/SYSRET)
- [x] Fork and execve system calls
- [x] ELF64 binary loader
- [x] POSIX signal system with user handlers
- [x] TTY layer with termios support
- [x] Pipes for IPC
- [x] Memory protection (NX, SMEP, SMAP)
- [x] Per-process file descriptor tables
- [x] Userland C library (libc)
- [x] User programs (shell, cat, ls, pwd, stat)
- [x] Bash 5.2.21 port
- [x] Linux USB host for seamless booting

### Planned Features / Future Work
- [ ] **Writeable Filesystems**: ext2/ext3
- [ ] **Network Stack**: TCP/IP, UDP, Ethernet drivers
- [ ] **Advanced IPC**: Shared memory, message queues, semaphores
- [ ] **SMP Support**: Multi-core CPU support with per-CPU data
- [ ] **ACPI**: Advanced Configuration and Power Interface
- [ ] **Graphics**: 2D/3D graphics acceleration, framebuffer devices
- [ ] **Sound**: Audio drivers (AC'97, HDA)
- [ ] **More Drivers**: AHCI (SATA), NVMe, E1000 (network)
- [ ] **Security**: Sandboxing, capabilities, SELinux-like system
- [ ] **Dynamic Linking**: Shared libraries (.so)
- [ ] **GUI**: Window manager, desktop environment
- [ ] **POSIX Compliance**: Full POSIX.1-2017 compliance
- [ ] **Package Manager**: Software installation and updates
- [ ] **Self-hosting**: Compiler toolchain, build LikeOS on LikeOS

## Contributing

LikeOS-64 demonstrates a complete Unix-like OS with userland. Consider extending it with:

**Kernel Features**:
- Writeable filesystem support (ext2/ext3/ext4)
- Network stack (TCP/IP, ARP, DHCP)
- Additional device drivers (AHCI, NVMe, E1000)
- SMP support with APIC and per-CPU scheduler
- ACPI support for power management
- Dynamic module loading

**Userland Enhancements**:
- More shell built-ins and utilities (grep, sed, awk)
- Text editor (vi/nano clone)
- Package manager and software distribution
- Porting more GNU tools (coreutils, binutils, gcc)
- GUI/Window system (simple framebuffer GUI)
- Games and demo programs

**Performance**:
- Copy-on-write (COW) for fork optimization
- Page cache for filesystem reads
- Buffer cache for block devices
- More SLAB caches for common objects
- Lazy allocation and demand paging

**Security**:
- User authentication system
- File permissions and ownership
- Process capabilities
- Secure boot with verified kernel loading
- Encrypted filesystems

**Documentation**:
- API documentation for kernel interfaces
- Porting guide for applications
- Driver development guide
- Kernel internals documentation

## References & Resources

**UEFI & Bootloaders**:
- UEFI Specification 2.x
- GNU-EFI library documentation
- OSDev Wiki: UEFI

**x86-64 Architecture**:
- Intel 64 and IA-32 Architectures Software Developer Manuals
- AMD64 Architecture Programmer's Manual
- OSDev Wiki: x86-64

**Operating System Design**:
- "Operating Systems: Three Easy Pieces" by Remzi Arpaci-Dusseau
- "Operating System Concepts" by Silberschatz, Galvin, Gagne
- "The Design and Implementation of the FreeBSD Operating System"
- Linux Kernel Documentation

**USB & Device Drivers**:
- xHCI Specification (Intel)
- USB 3.0 Specification
- USB Mass Storage Class Specification
- OSDev Wiki: USB

**Filesystems**:
- FAT32 Specification (Microsoft)
- "Practical File System Design" by Dominic Giampaolo

**Build & Development**:
- GNU Make manual
- GCC documentation (freestanding environment)
- NASM documentation

## Credits

Developed as an educational operating system project demonstrating:
- Modern UEFI boot process
- 64-bit kernel architecture
- Unix-like process management
- System call interface design
- Device driver development
- Filesystem implementation
- Performance optimization techniques

Based on OS development best practices and community knowledge from OSDev.org and various OS development resources.

---

**LikeOS-64** - A Unix-like operating system built from scratch.

For questions, issues, or contributions, please refer to the project documentation and source code comments.
