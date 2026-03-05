CC = gcc
AR = ar
LD = ld
CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -I./include

# PIC flags for shared-library objects
PIC_CFLAGS = $(CFLAGS) -fPIC

# Source files
MALLOC_SRC = src/malloc/malloc.c
STDIO_SRC = src/stdio/stdio.c
STRING_SRC = src/string/string.c
STDLIB_SRC = src/stdlib/stdlib.c
CTYPE_SRC = src/ctype/ctype.c
DIR_SRC = src/dirent/dirent.c
SYSCALLS_SRC = src/syscalls/unistd.c src/syscalls/mman.c src/syscalls/signal.c src/syscalls/termios.c src/syscalls/pty.c src/syscalls/sched.c
DLFCN_SRC = src/dl/dlfcn.c
PTHREAD_SRC = src/pthread/pthread.c src/pthread/pthread_mutex.c src/pthread/pthread_cond.c src/pthread/pthread_sync.c src/pthread/pthread_tsd.c
PTHREAD_ASM = src/pthread/clone.S
CRT0_SRC = src/crt0.S
CRT1_SRC = src/crt1.S

# Static object files
MALLOC_OBJ = $(MALLOC_SRC:.c=.o)
STDIO_OBJ = $(STDIO_SRC:.c=.o)
STRING_OBJ = $(STRING_SRC:.c=.o)
STDLIB_OBJ = $(STDLIB_SRC:.c=.o)
CTYPE_OBJ = $(CTYPE_SRC:.c=.o)
DIR_OBJ = $(DIR_SRC:.c=.o)
SYSCALLS_OBJ = $(SYSCALLS_SRC:.c=.o)
DLFCN_OBJ = $(DLFCN_SRC:.c=.o)
PTHREAD_OBJ = $(PTHREAD_SRC:.c=.o) $(PTHREAD_ASM:.S=.o)
CRT0_OBJ = $(CRT0_SRC:.S=.o)
CRT1_OBJ = $(CRT1_SRC:.S=.o)

# All static objects (libc – everything except pthread)
LIBC_OBJ = $(MALLOC_OBJ) $(STDIO_OBJ) $(STRING_OBJ) $(STDLIB_OBJ) \
           $(CTYPE_OBJ) $(DIR_OBJ) $(SYSCALLS_OBJ)

ALL_OBJ = $(LIBC_OBJ) $(PTHREAD_OBJ)

# PIC object files (for shared libraries)  – include dlfcn in shared libc
DLFCN_PIC = $(DLFCN_OBJ:.o=.pic.o)
LIBC_PIC = $(LIBC_OBJ:.o=.pic.o) $(DLFCN_PIC)
PTHREAD_PIC = $(PTHREAD_OBJ:.o=.pic.o)

# Output
LIBC_A      = libc.a
LIBC_SO     = libc.so
LIBPTHREAD_SO = libpthread.so
CRT0        = crt0.o
CRT1        = crt1.o

all: $(LIBC_A) $(CRT0) $(CRT1) $(LIBC_SO) $(LIBPTHREAD_SO)

# ---- Static library (libc.a) — contains everything including pthread ----
$(LIBC_A): $(ALL_OBJ)
	$(AR) rcs $@ $^

# ---- Shared libraries ----
$(LIBC_SO): $(LIBC_PIC)
	$(LD) -shared -nostdlib -Bsymbolic-functions --hash-style=sysv \
	    -soname libc.so -o $@ $^

$(LIBPTHREAD_SO): $(PTHREAD_PIC)
	$(LD) -shared -nostdlib -Bsymbolic-functions --hash-style=sysv \
	    -soname libpthread.so -o $@ $^

# ---- CRT objects ----
$(CRT0): $(CRT0_OBJ)
	cp $< $@

$(CRT1): $(CRT1_OBJ)
	cp $< $@

# ---- Pattern rules ----
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.pic.o: %.c
	$(CC) $(PIC_CFLAGS) -c $< -o $@

%.pic.o: %.S
	$(CC) $(PIC_CFLAGS) -c $< -o $@

clean:
	rm -f $(ALL_OBJ) $(DLFCN_OBJ) $(CRT0_OBJ) $(CRT1_OBJ) $(LIBC_A) $(CRT0) $(CRT1)
	rm -f $(LIBC_PIC) $(DLFCN_PIC) $(PTHREAD_PIC) $(LIBC_SO) $(LIBPTHREAD_SO)

.PHONY: all clean
