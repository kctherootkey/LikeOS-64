CC = gcc
LD = ld

LIBC_DIR = ../libc

# -Wno-cpp silences yescrypt-ref.c's deliberate "use opt.c" #warning.
# The reference implementation is byte-identical to the optimized one and has
# no SIMD/platform dependencies, which is what we want for a freestanding port.
CFLAGS = -Wall -Wextra -Wno-cpp -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIC \
         -I. -I$(LIBC_DIR)/include

# Link against libc.so so DT_NEEDED records it and the rtld resolves
# malloc/memcpy/strlen/etc. at load time.
LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -soname libcrypt.so \
          -L$(LIBC_DIR) -lc

SRC = crypt.c yescrypt-common.c yescrypt-ref.c sha256.c insecure_memzero.c
OBJ = $(SRC:.c=.o)
OUT = libcrypt.so

all: $(OUT)

$(OUT): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean
