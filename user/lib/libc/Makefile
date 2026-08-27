CC = gcc
AR = ar
LD = ld
DEBUG ?= 0
CFLAGS = -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -I./include
ifeq ($(DEBUG),1)
CFLAGS += -DSTACK_SMASH_DEBUG -DDEBUG=1
endif

# PIC flags for shared-library objects
PIC_CFLAGS = $(CFLAGS) -fPIC

# Source files
MALLOC_SRC = src/malloc/malloc.c
STDIO_SRC = src/stdio/stdio.c
STRING_SRC = src/string/string.c src/string/fnmatch.c src/string/strnlen.c src/string/glob.c
STDLIB_SRC = src/crt/initfini.c src/stdlib/stdlib.c src/stdlib/cxa_atexit.c src/stdlib/getopt.c src/stdlib/mkstemps.c src/stdlib/random.c src/stdlib/spawn.c src/stdlib/getloadavg.c src/stdlib/assert.c src/stdlib/search.c
CTYPE_SRC = src/ctype/ctype.c
TIME_SRC = src/time/time.c
DIR_SRC = src/dirent/dirent.c src/dirent/ftw.c
MATH_SRC = src/math/math.c src/math/fenv.c
REGEX_SRC = src/regex/regex.c
ICONV_SRC = src/iconv/iconv.c
# Locale and wide-character support.  unicode.c is generated -- see
# host/gen-unicode-tables.py.
LOCALE_SRC = src/locale/locale.c src/locale/multibyte.c src/locale/wctype.c \
             src/locale/wcwidth.c src/locale/wcstr.c src/locale/unicode.c \
             src/locale/uchar.c
EXTRA_STDIO_SRC = src/stdio/getline.c src/stdio/err.c src/stdio/popen.c src/stdio/vfscanf.c src/stdio/wstdio.c src/stdio/stdio_ext.c
SYSCALLS_SRC = src/syscalls/unistd.c src/syscalls/mman.c src/syscalls/signal.c src/syscalls/termios.c src/syscalls/pty.c src/syscalls/shm.c src/syscalls/sysvshm.c src/syscalls/sched.c src/syscalls/socket.c src/syscalls/uio.c src/syscalls/resource.c src/syscalls/pty_util.c src/syscalls/ptrace.c
DLFCN_SRC = src/dl/dlfcn.c
PWD_SRC = src/pwd/pwd.c src/pwd/grp.c src/pwd/shadow.c src/pwd/creds.c
NET_SRC = src/net/inet.c src/net/getaddrinfo.c src/net/getifaddrs.c src/net/netdb_extra.c src/net/resolv.c
PTHREAD_SRC = src/pthread/pthread.c src/pthread/pthread_mutex.c src/pthread/pthread_cond.c src/pthread/pthread_sync.c src/pthread/pthread_tsd.c src/pthread/semaphore.c
STACK_CHK_SRC = src/stack_chk.c
PTHREAD_ASM = src/pthread/clone.S
SETJMP_ASM  = src/setjmp.S
CRT0_SRC = src/crt0.S
CRT1_SRC = src/crt1.S
CRTI_SRC = src/crti.S
CRTN_SRC = src/crtn.S

# Static object files
MALLOC_OBJ = $(MALLOC_SRC:.c=.o)
STDIO_OBJ = $(STDIO_SRC:.c=.o) $(EXTRA_STDIO_SRC:.c=.o)
STRING_OBJ = $(STRING_SRC:.c=.o)
STDLIB_OBJ = $(STDLIB_SRC:.c=.o)
CTYPE_OBJ = $(CTYPE_SRC:.c=.o)
TIME_OBJ = $(TIME_SRC:.c=.o)
DIR_OBJ = $(DIR_SRC:.c=.o)
MATH_OBJ = $(MATH_SRC:.c=.o)
REGEX_OBJ = $(REGEX_SRC:.c=.o)
ICONV_OBJ = $(ICONV_SRC:.c=.o)
LOCALE_OBJ = $(LOCALE_SRC:.c=.o)
SYSCALLS_OBJ = $(SYSCALLS_SRC:.c=.o)
DLFCN_OBJ = $(DLFCN_SRC:.c=.o)
PWD_OBJ = $(PWD_SRC:.c=.o)
NET_OBJ = $(NET_SRC:.c=.o)
PTHREAD_OBJ = $(PTHREAD_SRC:.c=.o) $(PTHREAD_ASM:.S=.o)
SETJMP_OBJ  = $(SETJMP_ASM:.S=.o)
STACK_CHK_OBJ = $(STACK_CHK_SRC:.c=.o)
CRT0_OBJ = $(CRT0_SRC:.S=.o)
CRT1_OBJ = $(CRT1_SRC:.S=.o)
CRTI_OBJ = $(CRTI_SRC:.S=.o)
CRTN_OBJ = $(CRTN_SRC:.S=.o)

# All static objects (libc – everything except pthread)
LIBC_OBJ = $(MALLOC_OBJ) $(STDIO_OBJ) $(STRING_OBJ) $(STDLIB_OBJ) \
           $(CTYPE_OBJ) $(TIME_OBJ) $(DIR_OBJ) $(MATH_OBJ) $(REGEX_OBJ) $(ICONV_OBJ) $(LOCALE_OBJ) $(SYSCALLS_OBJ) \
           $(NET_OBJ) $(SETJMP_OBJ) $(STACK_CHK_OBJ) $(PWD_OBJ)

ALL_OBJ = $(LIBC_OBJ) $(PTHREAD_OBJ)

# PIC object files (for shared libraries)  – include dlfcn + pthread in shared libc
DLFCN_PIC = $(DLFCN_OBJ:.o=.pic.o)
PTHREAD_PIC = $(PTHREAD_OBJ:.o=.pic.o)
LIBC_PIC = $(LIBC_OBJ:.o=.pic.o) $(DLFCN_PIC) $(PTHREAD_PIC)

# Output
LIBC_A      = libc.a
LIBC_SO     = libc.so
CRT0        = crt0.o
CRT1        = crt1.o
CRTI        = crti.o
CRTN        = crtn.o

all: $(LIBC_A) $(CRT0) $(CRT1) $(CRTI) $(CRTN) $(LIBC_SO) libm.a

# ---- Static library (libc.a) — contains everything including pthread ----
$(LIBC_A): $(ALL_OBJ)
	$(AR) rcs $@ $^

# ---- Shared library (libc.so — includes pthread) ----
# --eh-frame-hdr builds the .eh_frame_hdr section and the PT_GNU_EH_FRAME
# program header that names it.  gcc passes it when IT drives the link, but this
# one calls ld directly, so it has to be stated: without it a C++ exception
# thrown through a libc callback -- a qsort comparator, a signal handler -- has
# no unwind tables for these frames and reaches std::terminate instead of the
# handler waiting for it.
$(LIBC_SO): $(LIBC_PIC)
	$(LD) -shared -nostdlib -Bsymbolic-functions --hash-style=gnu \
		-z relro \
	    --eh-frame-hdr -soname libc.so -o $@ $^

# ---- CRT objects ----
$(CRT0): $(CRT0_OBJ)
	cp $< $@

$(CRT1): $(CRT1_OBJ)
	cp $< $@

$(CRTI): $(CRTI_OBJ)
	cp $< $@

$(CRTN): $(CRTN_OBJ)
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
	rm -f $(ALL_OBJ) $(DLFCN_OBJ) $(CRT0_OBJ) $(CRT1_OBJ) $(CRTI_OBJ) $(CRTN_OBJ) $(LIBC_A) $(CRT0) $(CRT1) $(CRTI) $(CRTN)
	rm -f $(LIBC_PIC) $(DLFCN_PIC) $(PTHREAD_PIC) $(LIBC_SO)

.PHONY: all clean

# libm.a — deliberately EMPTY.
#
# The math functions live in libc here, as they do on any system that has
# merged the two.  But an enormous amount of software passes -lm
# unconditionally, and configure scripts routinely probe for it (libXfont2
# refuses to build without one).  An empty archive satisfies the link, adds
# nothing to the binary, and creates no runtime dependency: the symbols still
# resolve from libc.so.
libm.a:
	$(AR) rcs $@
