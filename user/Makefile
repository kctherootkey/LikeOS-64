CC = gcc
LD = ld
LIBC_DIR = ../userland/libc
CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -I$(LIBC_DIR)/include

LDFLAGS = -nostdlib -T user.lds
LIBS = $(LIBC_DIR)/libc.a
CRT0 = $(LIBC_DIR)/crt0.o

# Programs
PROGRAMS = test_syscalls test_libc hello sh ls cat pwd stat progerr testmem memstat teststress

all: $(PROGRAMS)

test_syscalls: $(CRT0) test_syscalls.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) test_syscalls.o $(LIBS) -o $@

test_libc: $(CRT0) test_libc.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) test_libc.o $(LIBS) -o $@

hello: $(CRT0) hello.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) hello.o $(LIBS) -o $@

sh: $(CRT0) sh.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) sh.o $(LIBS) -o $@

ls: $(CRT0) ls.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) ls.o $(LIBS) -o $@

cat: $(CRT0) cat.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) cat.o $(LIBS) -o $@

pwd: $(CRT0) pwd.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) pwd.o $(LIBS) -o $@

stat: $(CRT0) stat.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) stat.o $(LIBS) -o $@

progerr: $(CRT0) progerr.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) progerr.o $(LIBS) -o $@

testmem: $(CRT0) testmem.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) testmem.o $(LIBS) -o $@

memstat: $(CRT0) memstat.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) memstat.o $(LIBS) -o $@

teststress: $(CRT0) teststress.o $(LIBS)
	$(LD) $(LDFLAGS) $(CRT0) teststress.o $(LIBS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROGRAMS)

.PHONY: all clean
