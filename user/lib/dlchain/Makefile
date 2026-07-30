# libdlbase.so / libdlchain.so — a two-level DSO chain used only by testlibc.
#
# libdlchain.so carries a DT_NEEDED on libdlbase.so.  dlopen()ing the former
# must therefore load, relocate AND initialise the latter; the loader used to
# relocate only the object it was handed, leaving the dependency's GOT empty.
# Building these as a pair is the only way to exercise that from a test.
#
# NOTE: -Bsymbolic is deliberately NOT used here.  It would bind libdlchain's
# references to libdlbase at link time and defeat the entire point.

CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIC

LDFLAGS_COMMON = -shared -nostdlib --hash-style=gnu

all: libdlbase.so libdlchain.so

libdlbase.so: dlbase.o
	$(LD) $(LDFLAGS_COMMON) -soname libdlbase.so -o $@ $^

# -l:libdlbase.so records the DT_NEEDED; -rpath /lib matches where both land
# on the image.
libdlchain.so: dlchain.o libdlbase.so
	$(LD) $(LDFLAGS_COMMON) -soname libdlchain.so -L. -rpath /lib \
		-o $@ dlchain.o -l:libdlbase.so

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o libdlbase.so libdlchain.so

.PHONY: all clean
