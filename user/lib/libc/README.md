# LikeOS-64 Userland C Library (libc)

## Overview

A comprehensive userland C library implementation for LikeOS-64, providing standard C library functionality for user programs through a clean syscall interface.

## Architecture

### Directory Structure
```
userland/libc/
├── include/              # Public headers
│   ├── stddef.h
│   ├── stdint.h
│   ├── stdarg.h
│   ├── string.h
│   ├── stdlib.h
│   ├── stdio.h
│   ├── unistd.h
│   ├── ctype.h
│   ├── errno.h
│   └── sys/
│       └── types.h
├── src/                  # Implementation
│   ├── crt0.S           # Program startup code
│   ├── malloc/
│   │   └── malloc.c     # Heap allocator
│   ├── stdio/
│   │   └── stdio.c      # Standard I/O
│   ├── string/
│   │   └── string.c     # String manipulation
│   ├── stdlib/
│   │   └── stdlib.c     # Standard utilities
│   ├── ctype/
│   │   └── ctype.c      # Character classification
│   └── syscalls/
│       ├── syscall.h    # Syscall wrappers (inline asm)
│       └── unistd.c     # POSIX system calls
├── Makefile
├── libc.a               # Static library (output)
└── crt0.o               # Startup object (output)
```

## Components

### 1. Syscall Interface (`src/syscalls/`)

**syscall.h**: Provides inline assembly wrappers for system calls
- `syscall0-6`: Functions for 0-6 argument syscalls
- Uses x86-64 syscall instruction
- Registers: rax (syscall number), rdi, rsi, rdx, r10, r8, r9 (arguments)
- Handles errno on negative returns

**unistd.c**: POSIX system call wrappers
- `read()`, `write()`, `close()`
- `sbrk()`, `brk()` - Memory management
- `getpid()` - Process ID
- `_exit()` - Process termination
- `lseek()` - File positioning (stub)

### 2. Memory Management (`src/malloc/`)

**malloc.c**: Simple heap allocator
- Block-based allocator using `sbrk()` for heap expansion
- Functions:
  - `malloc()` - Allocate memory
  - `free()` - Free memory
  - `calloc()` - Allocate and zero memory
  - `realloc()` - Resize allocation
- Uses block headers: `{ size, is_free, next }`
- First-fit allocation strategy

### 3. String Functions (`src/string/`)

**string.c**: Complete string.h implementation
- Memory operations:
  - `memcpy()`, `memmove()`, `memset()`, `memcmp()`, `memchr()`
- String operations:
  - `strlen()`, `strcpy()`, `strncpy()`
  - `strcat()`, `strncat()`
  - `strcmp()`, `strncmp()`
  - `strchr()`, `strrchr()`, `strstr()`
  - `strdup()`, `strtok()`

### 4. Standard I/O (`src/stdio/`)

**stdio.c**: FILE-based I/O and formatted output
- Standard streams: `stdin`, `stdout`, `stderr`
- File operations (stubs):
  - `fopen()`, `fclose()`, `fread()`, `fwrite()`
  - `fseek()`, `ftell()`, `rewind()`
- Character I/O:
  - `fgetc()`, `getchar()`, `fgets()`
  - `fputc()`, `putchar()`, `fputs()`, `puts()`
- Formatted output:
  - `printf()`, `fprintf()`, `sprintf()`, `snprintf()`
  - `vfprintf()`, `vsnprintf()`
  - Supports: %d, %i, %u, %x, %X, %p, %s, %c, %%
  - Length modifiers: l, ll
  - Width and precision support

### 5. Standard Library (`src/stdlib/`)

**stdlib.c**: Utility functions
- String conversions:
  - `atoi()`, `atol()`, `atoll()`
  - `strtol()`, `strtoul()`, `strtoll()`, `strtoull()`
  - Supports bases 2-36, auto-detect (0x for hex, 0 for octal)
- Math:
  - `abs()`, `labs()`
- Sorting and searching:
  - `qsort()` - Generic sort (bubble sort implementation)
  - `bsearch()` - Binary search
- Process:
  - `exit()`, `abort()`
  - `getenv()` (stub)

### 6. Character Classification (`src/ctype/`)

**ctype.c**: Character type testing and conversion
- Classification:
  - `isalpha()`, `isdigit()`, `isalnum()`
  - `isspace()`, `isprint()`, `iscntrl()`
  - `isupper()`, `islower()`
  - `isxdigit()`, `ispunct()`, `isgraph()`
- Conversion:
  - `toupper()`, `tolower()`

### 7. Program Startup (`src/crt0.S`)

**crt0.S**: Assembly startup code
- Entry point: `_start`
- Sets up stack frame
- Receives argc, argv, envp from kernel
- Calls `main(argc, argv, envp)`
- Exits with return value via `_exit()`

## Build System

### libc Makefile

```makefile
CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -I./include

all: libc.a crt0.o
```

**Output files:**
- `libc.a` - Static library archive (~28KB)
- `crt0.o` - Startup object file (~768 bytes)

### User Program Makefile

```makefile
LIBC_DIR = ../userland/libc
CFLAGS = -I$(LIBC_DIR)/include
LDFLAGS = -nostdlib -T user.lds
LIBS = $(LIBC_DIR)/libc.a
CRT0 = $(LIBC_DIR)/crt0.o

program: $(CRT0) program.o $(LIBS)
    $(LD) $(LDFLAGS) $(CRT0) program.o $(LIBS) -o $@
```

## Integration

### Main Makefile Integration

The userland libc is integrated into the main build system:

1. **Build libc**: `.PHONY: userland-libc` target builds the library
2. **Build test program**: `build/test_libc` depends on `userland-libc`
3. **Include in image**: test_libc copied to `/testlibc` on FAT32 image

### Usage in User Programs

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    printf("Hello, world!\n");
    
    char* buf = malloc(100);
    strcpy(buf, "Test string");
    printf("String: %s\n", buf);
    free(buf);
    
    return 0;
}
```

**Compile and link:**
```bash
gcc -I../userland/libc/include -c program.c -o program.o
ld -nostdlib -T user.lds ../userland/libc/crt0.o program.o ../userland/libc/libc.a -o program
```

## Test Program

### test_libc.c

Comprehensive test demonstrating libc functionality:
- printf with format specifiers
- malloc/free memory management
- String operations (strcpy, strcat, strlen)
- String conversions (atoi)
- Direct syscall access (write, getpid)

## Syscall Numbers (Kernel Interface)

The libc uses these syscall numbers (defined in kernel):
- SYS_read = 0
- SYS_write = 1
- SYS_open = 2
- SYS_close = 3
- SYS_getpid = 39
- SYS_exit = 60
- SYS_brk = 12

## Features

### Implemented
✓ Complete string.h implementation
✓ Heap allocator (malloc/free/calloc/realloc)
✓ Formatted output (printf family)
✓ String conversions (atoi, strtol families)
✓ Character classification (ctype.h)
✓ Basic POSIX syscalls (read/write/close/brk/sbrk)
✓ Program startup code (crt0)
✓ Standard streams (stdin/stdout/stderr)
✓ Sorting and searching (qsort/bsearch)

### Limitations
- No file I/O (fopen/fread/etc are stubs - needs kernel VFS syscalls)
- No buffered I/O (stdio uses direct syscalls)
- Simple allocator (no free list coalescing)
- Minimal printf implementation (no floating point)
- No environment variables (getenv stub)
- No fork/exec/wait syscalls yet

## Future Enhancements

1. **File System Support**
   - Implement open/close/read/write syscalls in kernel
   - Complete fopen/fclose/fread/fwrite implementation
   - Add directory operations (opendir/readdir)

2. **Process Management**
   - fork(), exec(), wait() syscalls
   - Signal handling
   - Process groups and sessions

3. **Advanced Memory**
   - mmap/munmap syscalls
   - Shared memory
   - Memory-mapped files

4. **Networking**
   - Socket syscalls
   - Network I/O

5. **Math Library**
   - Floating point support in printf/scanf
   - Math functions (sin, cos, sqrt, etc.)

6. **Improved Allocator**
   - Free list coalescing
   - Multiple heap sizes
   - Thread-safe malloc

## Performance

**Binary Sizes:**
- libc.a: ~28KB
- crt0.o: 768 bytes
- test_libc: ~15KB (statically linked)

**Memory:**
- Heap grows via sbrk() on demand
- No global data except standard streams
- Minimal stack usage in library functions

## Conclusion

The LikeOS-64 userland C library provides a solid foundation for user programs with:
- Clean separation from kernel
- Standard C library interface
- Efficient syscall mechanism
- Comprehensive string and I/O support
- Extensible architecture for future enhancements

This implementation enables writing portable C programs for LikeOS-64 using familiar standard library functions.
