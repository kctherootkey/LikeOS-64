# Makefile for ld-likeos.so — LikeOS-64 runtime dynamic linker

CC      = gcc
AS      = gcc
LD      = ld

CFLAGS  = -Wall -Wextra -O2 \
          -fPIC -fno-builtin -fstack-protector-strong \
          -nostdlib -nostdinc -ffreestanding \
          -mno-red-zone -mcmodel=small \
          -fvisibility=hidden

ASFLAGS = -c

LDFLAGS = -shared -nostdlib -T rtld.lds \
          -Bsymbolic --no-undefined \
          -z now \
          --hash-style=gnu

TARGET  = ld-likeos.so
OBJS    = rtld.o rtld_entry.o rtld_stack_chk.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

rtld.o: rtld.c rtld_syscall.h
	$(CC) $(CFLAGS) -c -o $@ rtld.c

rtld_stack_chk.o: rtld_stack_chk.c
	$(CC) $(CFLAGS) -c -o $@ rtld_stack_chk.c

rtld_entry.o: rtld_entry.S
	$(AS) $(ASFLAGS) -o $@ rtld_entry.S

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
