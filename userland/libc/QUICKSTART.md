# LikeOS-64 libc Quick Reference

## Building User Programs

### 1. Simple Program

**hello.c:**
```c
#include <stdio.h>

int main(void) {
    printf("Hello, LikeOS-64!\n");
    return 0;
}
```

**Build:**
```bash
gcc -Wall -O2 -nostdlib -fno-builtin -fno-stack-protector \
    -mno-red-zone -ffreestanding -I../userland/libc/include \
    -c hello.c -o hello.o

ld -nostdlib -T user.lds \
    ../userland/libc/crt0.o hello.o ../userland/libc/libc.a \
    -o hello
```

### 2. Using the Makefile

Add to `user/Makefile`:
```makefile
hello: $(CRT0) hello.o $(LIBS)
    $(LD) $(LDFLAGS) $(CRT0) hello.o $(LIBS) -o $@
```

Build:
```bash
cd user && make hello
```

## Available Functions

### stdio.h
```c
// Standard streams
FILE* stdin, *stdout, *stderr;

// Output
int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int puts(const char* s);
int fputs(const char* s, FILE* stream);
int putchar(int c);
int fputc(int c, FILE* stream);

// Input
int getchar(void);
int fgetc(FILE* stream);
char* fgets(char* s, int size, FILE* stream);

// File operations (stubs)
FILE* fopen(const char* pathname, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);

// Format specifiers for printf:
// %d, %i - signed decimal
// %u - unsigned decimal
// %x, %X - hexadecimal
// %p - pointer
// %s - string
// %c - character
// %% - literal %
```

### stdlib.h
```c
// Memory allocation
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

// String conversion
int atoi(const char* nptr);
long atol(const char* nptr);
long strtol(const char* nptr, char** endptr, int base);
unsigned long strtoul(const char* nptr, char** endptr, int base);

// Math
int abs(int n);
long labs(long n);

// Sorting and searching
void qsort(void* base, size_t nmemb, size_t size,
           int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb,
              size_t size, int (*compar)(const void*, const void*));

// Process control
void exit(int status);
void abort(void);
```

### string.h
```c
// Memory operations
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
void* memchr(const void* s, int c, size_t n);

// String operations
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strstr(const char* haystack, const char* needle);
char* strdup(const char* s);
char* strtok(char* str, const char* delim);
```

### unistd.h
```c
// I/O
ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int close(int fd);

// Memory management
void* sbrk(intptr_t increment);
int brk(void* addr);

// Process
pid_t getpid(void);
void _exit(int status);
```

### ctype.h
```c
// Character classification
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int iscntrl(int c);
int isxdigit(int c);
int ispunct(int c);
int isgraph(int c);

// Character conversion
int toupper(int c);
int tolower(int c);
```

## Example Programs

### Memory Management
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    // Allocate memory
    char* buffer = malloc(256);
    if (!buffer) {
        printf("malloc failed\n");
        return 1;
    }
    
    // Use the buffer
    strcpy(buffer, "Hello from heap!");
    printf("%s\n", buffer);
    
    // Reallocate
    buffer = realloc(buffer, 512);
    strcat(buffer, " More data!");
    printf("%s\n", buffer);
    
    // Free memory
    free(buffer);
    return 0;
}
```

### String Processing
```c
#include <stdio.h>
#include <string.h>

int main(void) {
    char str[] = "one,two,three";
    char* token;
    
    token = strtok(str, ",");
    while (token) {
        printf("Token: %s\n", token);
        token = strtok(NULL, ",");
    }
    
    return 0;
}
```

### Formatted Output
```c
#include <stdio.h>

int main(void) {
    int num = 42;
    char* str = "test";
    void* ptr = &num;
    
    printf("Decimal: %d\n", num);
    printf("Hex: 0x%x\n", num);
    printf("String: %s\n", str);
    printf("Pointer: %p\n", ptr);
    printf("Multiple: %d %s %p\n", num, str, ptr);
    
    return 0;
}
```

### Number Conversion
```c
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char* numbers[] = {
        "42",      // decimal
        "-123",    // negative
        "0xFF",    // hex
        "0777"     // octal
    };
    
    for (int i = 0; i < 4; i++) {
        printf("atoi(\"%s\") = %d\n", 
               numbers[i], atoi(numbers[i]));
    }
    
    // With base detection
    char* end;
    long val = strtol("0xFF", &end, 0);
    printf("strtol(\"0xFF\", 0) = %ld\n", val);
    
    return 0;
}
```

### Sorting
```c
#include <stdio.h>
#include <stdlib.h>

int compare_ints(const void* a, const void* b) {
    return (*(int*)a - *(int*)b);
}

int main(void) {
    int arr[] = {5, 2, 8, 1, 9, 3};
    int n = sizeof(arr) / sizeof(arr[0]);
    
    qsort(arr, n, sizeof(int), compare_ints);
    
    printf("Sorted: ");
    for (int i = 0; i < n; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
    
    return 0;
}
```

### Binary Search
```c
#include <stdio.h>
#include <stdlib.h>

int compare_ints(const void* a, const void* b) {
    return (*(int*)a - *(int*)b);
}

int main(void) {
    int arr[] = {1, 2, 3, 5, 8, 9};  // Must be sorted
    int n = sizeof(arr) / sizeof(arr[0]);
    int key = 5;
    
    int* result = bsearch(&key, arr, n, sizeof(int), compare_ints);
    
    if (result) {
        printf("Found: %d\n", *result);
    } else {
        printf("Not found\n");
    }
    
    return 0;
}
```

## Direct Syscalls

For low-level operations, you can use syscalls directly:

```c
#include <unistd.h>

int main(void) {
    const char* msg = "Hello via syscall\n";
    write(1, msg, 18);  // fd 1 = stdout
    
    char buf[100];
    ssize_t n = read(0, buf, sizeof(buf));  // fd 0 = stdin
    write(1, buf, n);
    
    return 0;
}
```

## Debugging Tips

### Check Binary Size
```bash
ls -lh program
file program
```

### Inspect Symbols
```bash
nm program | grep ' T '  # List functions
```

### Check for Undefined Symbols
```bash
nm -u program  # Should be empty for fully static
```

## Limitations

1. **No file I/O**: fopen/fclose/fread/fwrite are stubs
2. **No buffering**: stdio uses direct syscalls
3. **Limited printf**: No floating point (%f, %e, %g)
4. **Simple allocator**: No advanced heap management
5. **No threads**: Not thread-safe
6. **No signals**: No signal handling

## Adding New Functions

### 1. Add to header (include/*.h)
```c
// include/mylib.h
void my_function(int arg);
```

### 2. Implement (src/mylib/mylib.c)
```c
// src/mylib/mylib.c
#include "../../include/mylib.h"

void my_function(int arg) {
    // implementation
}
```

### 3. Update Makefile
```makefile
MYLIB_SRC = src/mylib/mylib.c
MYLIB_OBJ = $(MYLIB_SRC:.c=.o)
ALL_OBJ = ... $(MYLIB_OBJ)
```

### 4. Rebuild
```bash
cd userland/libc && make clean && make
```

## Common Patterns

### Error Handling
```c
#include <stdio.h>
#include <stdlib.h>

void* safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        printf("Out of memory\n");
        exit(1);
    }
    return ptr;
}
```

### String Building
```c
#include <stdio.h>
#include <string.h>

char buffer[256];
snprintf(buffer, sizeof(buffer), "Value: %d", 42);
```

### Buffer Management
```c
char* create_string(const char* prefix, int num) {
    size_t len = strlen(prefix) + 20;
    char* str = malloc(len);
    snprintf(str, len, "%s: %d", prefix, num);
    return str;  // Caller must free()
}
```

## Performance Tips

1. **Avoid repeated strlen**: Cache the result
2. **Use memcpy for known sizes**: Faster than strcpy
3. **Pre-allocate buffers**: Avoid multiple realloc calls
4. **Use snprintf limits**: Prevent buffer overflows
5. **Batch write calls**: Reduce syscall overhead

## Integration with Kernel

The libc communicates with the kernel via syscalls:

1. **Program starts**: Kernel loads ELF, sets up stack with argc/argv
2. **crt0 runs**: Calls main(argc, argv, envp)
3. **Syscalls**: Library functions call kernel via syscall instruction
4. **Exit**: main() returns, crt0 calls _exit() syscall

## Building Into Kernel Image

The test_libc program is automatically included:

```bash
make              # Builds kernel + libc + test_libc
make qemu-usb     # Run in QEMU

# In LikeOS shell:
# ./testlibc       # Run the test program
```

## Next Steps

1. Try the example programs
2. Read [README.md](README.md) for architecture details
3. Examine test_libc.c for comprehensive examples
4. Add your own utility functions
5. Contribute enhancements!
