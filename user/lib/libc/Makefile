# Copyright (C) 2026 The LikeOS Project

CC = gcc
AR = ar
LD = ld
DEBUG ?= 0
# Header dependencies.  Without them a change to a header in include/ rebuilt
# nothing at all -- only the objects whose .c also changed saw it -- so the
# library could be assembled from objects compiled against two different
# layouts of FILE, pthread_t or struct stat.  See the same note in the top
# Makefile.  -MMD writes the header list beside each object, -MP keeps a
# deleted header from becoming an error, and the -include at the bottom feeds
# them back.
DEPFLAGS = -MMD -MP

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -I./include
ifeq ($(DEBUG),1)
CFLAGS += -DSTACK_SMASH_DEBUG -DDEBUG=1
endif

# PIC flags for shared-library objects
PIC_CFLAGS = $(CFLAGS) -fPIC

# C++ for the vendored LLVM libc math (see LLVM_MATH_SRC).  Freestanding: no
# libstdc++ headers, no exceptions, no RTTI, no guard variables -- the code
# needs none of them and libc.so links no C++ runtime.  Overlay mode, so its
# proxy headers take FP_*, FE_* and errno from this library's own <math.h>,
# <fenv.h> and <errno.h>.  LIBC_COPT_PUBLIC_PACKAGING is what makes each
# function a plain C symbol rather than a namespaced one.  The sources are
# pristine third-party code, so their warnings are not this tree's to fix.
CXX = g++
CXXFLAGS = $(DEPFLAGS) -std=c++20 -w -O2 -nostdlib -nostdinc++ -fno-builtin \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fstack-protector-strong \
	-mno-red-zone -ffreestanding -I./include -I./src/math/llvm-libc \
	-DLIBC_NAMESPACE=__llvm_libc_likeos -DLIBC_COPT_PUBLIC_PACKAGING
PIC_CXXFLAGS = $(CXXFLAGS) -fPIC

# Source files
MALLOC_SRC = src/malloc/malloc.c
STDIO_SRC = src/stdio/stdio.c
STRING_SRC = src/string/string.c src/string/fnmatch.c src/string/strnlen.c src/string/glob.c
STDLIB_SRC = src/crt/initfini.c src/stdlib/stdlib.c src/stdlib/cxa_atexit.c src/stdlib/getopt.c src/stdlib/mkstemps.c src/stdlib/random.c src/stdlib/spawn.c src/stdlib/getloadavg.c src/stdlib/assert.c src/stdlib/search.c
CTYPE_SRC = src/ctype/ctype.c
TIME_SRC = src/time/time.c
DIR_SRC = src/dirent/dirent.c src/dirent/ftw.c
MATH_SRC = src/math/math.c src/math/fenv.c src/math/hyperbolic.c
# The double and float transcendentals come from LLVM libc's correctly rounded
# implementations, vendored pristine under src/math/llvm-libc (C++, built
# freestanding, no runtime beyond errno).  math.c keeps only what LLVM does not
# provide: the long double forms, erf/gamma and fmodl.
LLVM_MATH_SRC = $(wildcard src/math/llvm-libc/src/math/generic/*.cpp)
REGEX_SRC = src/regex/regex.c
ICONV_SRC = src/iconv/iconv.c
# Locale and wide-character support.  unicode.c is generated -- see
# host/gen-unicode-tables.py.
LOCALE_SRC = src/locale/locale.c src/locale/multibyte.c src/locale/wctype.c \
             src/locale/wcwidth.c src/locale/wcstr.c src/locale/unicode.c \
             src/locale/uchar.c
EXTRA_STDIO_SRC = src/stdio/getline.c src/stdio/err.c src/stdio/popen.c src/stdio/vfscanf.c src/stdio/wstdio.c src/stdio/stdio_ext.c
SYSCALLS_SRC = src/syscalls/unistd.c src/syscalls/mman.c src/syscalls/signal.c src/syscalls/termios.c src/syscalls/pty.c src/syscalls/shm.c src/syscalls/sysvshm.c src/syscalls/sysvsem.c src/syscalls/sched.c src/syscalls/socket.c src/syscalls/uio.c src/syscalls/resource.c src/syscalls/pty_util.c src/syscalls/ptrace.c src/syscalls/anonfd.c
DLFCN_SRC = src/dl/dlfcn.c
PWD_SRC = src/pwd/pwd.c src/pwd/grp.c src/pwd/shadow.c src/pwd/creds.c
NET_SRC = src/net/inet.c src/net/getaddrinfo.c src/net/getifaddrs.c src/net/netdb_extra.c src/net/resolv.c
PTHREAD_SRC = src/pthread/pthread.c src/pthread/pthread_mutex.c src/pthread/pthread_cond.c src/pthread/pthread_sync.c src/pthread/pthread_tsd.c src/pthread/semaphore.c
STACK_CHK_SRC = src/stack_chk.c
PTHREAD_ASM = src/pthread/clone.S
# vfork() cannot be written in C: see src/syscalls/vfork.S.
SYSCALLS_ASM = src/syscalls/vfork.S
SETJMP_ASM  = src/setjmp.S
UCONTEXT_ASM = src/misc/ucontext.S
MISC_SRC = src/misc/makecontext.c src/misc/threads.c
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
MATH_OBJ = $(MATH_SRC:.c=.o) $(LLVM_MATH_SRC:.cpp=.o)
REGEX_OBJ = $(REGEX_SRC:.c=.o)
ICONV_OBJ = $(ICONV_SRC:.c=.o)
LOCALE_OBJ = $(LOCALE_SRC:.c=.o)
SYSCALLS_OBJ = $(SYSCALLS_SRC:.c=.o) $(SYSCALLS_ASM:.S=.o)
DLFCN_OBJ = $(DLFCN_SRC:.c=.o)
PWD_OBJ = $(PWD_SRC:.c=.o)
NET_OBJ = $(NET_SRC:.c=.o)
PTHREAD_OBJ = $(PTHREAD_SRC:.c=.o) $(PTHREAD_ASM:.S=.o)
SETJMP_OBJ  = $(SETJMP_ASM:.S=.o)
UCONTEXT_OBJ = $(UCONTEXT_ASM:.S=.o) $(MISC_SRC:.c=.o)
STACK_CHK_OBJ = $(STACK_CHK_SRC:.c=.o)
CRT0_OBJ = $(CRT0_SRC:.S=.o)
CRT1_OBJ = $(CRT1_SRC:.S=.o)
CRTI_OBJ = $(CRTI_SRC:.S=.o)
CRTN_OBJ = $(CRTN_SRC:.S=.o)

# All static objects (libc – everything except pthread)
LIBC_OBJ = $(MALLOC_OBJ) $(STDIO_OBJ) $(STRING_OBJ) $(STDLIB_OBJ) \
           $(CTYPE_OBJ) $(TIME_OBJ) $(DIR_OBJ) $(MATH_OBJ) $(REGEX_OBJ) $(ICONV_OBJ) $(LOCALE_OBJ) $(SYSCALLS_OBJ) \
           $(NET_OBJ) $(SETJMP_OBJ) $(UCONTEXT_OBJ) $(STACK_CHK_OBJ) $(PWD_OBJ)

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

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.pic.o: %.cpp
	$(CXX) $(PIC_CXXFLAGS) -c $< -o $@

# `clean' leaves the LLVM math objects alone.  They are 130 C++ files of
# pristine third-party code that take far longer to compile than the rest of
# the library put together, and nothing a `make clean' is normally run for
# reaches them: CXXFLAGS does not see DEBUG, and their header dependencies are
# tracked, so their .d files are kept with them -- deleting those while the
# objects stayed would leave objects that no header change could rebuild.
# `distclean' removes them.
LLVM_MATH_OBJ = $(LLVM_MATH_SRC:.cpp=.o)
LLVM_MATH_PIC = $(LLVM_MATH_SRC:.cpp=.pic.o)

clean:
	rm -f $(filter-out $(LLVM_MATH_OBJ),$(ALL_OBJ)) $(DLFCN_OBJ) $(CRT0_OBJ) $(CRT1_OBJ) $(CRTI_OBJ) $(CRTN_OBJ) $(LIBC_A) $(CRT0) $(CRT1) $(CRTI) $(CRTN)
	rm -f $(filter-out $(LLVM_MATH_PIC),$(LIBC_PIC)) $(DLFCN_PIC) $(PTHREAD_PIC) $(LIBC_SO)
	find src -path src/math/llvm-libc -prune -o -name '*.d' -exec rm -f {} +

distclean: clean
	rm -f $(LLVM_MATH_OBJ) $(LLVM_MATH_PIC)
	find src/math/llvm-libc -name '*.d' -delete

.PHONY: all clean distclean

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

# The header dependencies recorded by -MMD above.  `-include' because they do
# not exist before the first build of each object, and their absence is not an
# error.  Both spellings: %.o and %.pic.o each write their own.
-include $(wildcard src/*/*.d) $(wildcard src/math/llvm-libc/src/math/generic/*.d)
