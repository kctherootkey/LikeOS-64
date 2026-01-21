CC = gcc
LD = ld
LIBC_DIR = ../userland/libc
CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -I$(LIBC_DIR)/include

LDFLAGS = -nostdlib -T user.lds
LIBS = $(LIBC_DIR)/libc.a
CRT0 = $(LIBC_DIR)/crt0.o

# Programs
PROGRAMS = test_syscalls test_libc hello

all: $(PROGRAMS)

test_syscalls: $(CRT0) test_syscalls.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) test_syscalls.o $(LIBS) -o $@

test_libc: $(CRT0) test_libc.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) test_libc.o $(LIBS) -o $@

hello: $(CRT0) hello.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) hello.o $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROGRAMS)

.PHONY: all clean
