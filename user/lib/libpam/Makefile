CC = gcc
LD = ld

LIBC_DIR = ../libc
LIBCRYPT_DIR = ../libcrypt

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIC \
         -Iinclude -I$(LIBC_DIR)/include

# DT_NEEDED libc.so (malloc/string/stdio, pwd/shadow db) and libcrypt.so (crypt).
LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -soname libpam.so \
          -L$(LIBC_DIR) -lc -L$(LIBCRYPT_DIR) -lcrypt

SRC = pam.c
OBJ = $(SRC:.c=.o)
OUT = libpam.so

all: $(OUT)

$(OUT): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean
