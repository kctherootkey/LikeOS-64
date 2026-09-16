# Makefile for ld-likeos.so — LikeOS runtime dynamic linker
#
# Copyright (C) 2026 The LikeOS Project

CC      = gcc
AS      = gcc
LD      = ld

CFLAGS  = -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 \
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
