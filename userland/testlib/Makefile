CC = gcc
LD = ld

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIC

LDFLAGS = -shared -nostdlib -Bsymbolic --hash-style=gnu -soname libtestlib.so

SRC = testlib.c
OBJ = testlib.o
OUT = libtestlib.so

all: $(OUT)

$(OUT): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(OUT)

.PHONY: all clean
