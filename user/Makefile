CC = gcc
LD = ld

LIBC_DIR = ../userland/libc
RTLD_DIR = ../userland/rtld

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fno-stack-protector \
         -mno-red-zone -ffreestanding -fPIE -I$(LIBC_DIR)/include

# Dynamic PIE linking against libc.so (includes pthread)
LDFLAGS = -pie -nostdlib -T user_dyn.lds --hash-style=gnu \
          -z now \
          -rpath /lib -dynamic-linker /lib/ld-likeos.so \
          -L$(LIBC_DIR) -L$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o

# Shared libraries to link against (ld-likeos.so provides _rtld_* symbols for dlfcn)
LIBS = -lc -l:ld-likeos.so

# Programs
PROGRAMS = test_syscalls test_libc hello sh ls cat pwd stat progerr testmem memstat teststress uname shutdown poweroff ps cp mv rm mkdir rmdir touch more less clear env

all: $(PROGRAMS) reboot halt

# Generic link rule: crt1.o + program.o → PIE binary linked against libc.so + libpthread.so
define LINK_RULE
$(1): $(CRT1) $(1).o
	$(LD) $(LDFLAGS) $(CRT1) $(1).o $(LIBS) -o $$@
endef

$(foreach prog,$(PROGRAMS),$(eval $(call LINK_RULE,$(prog))))

# reboot and halt are copies of poweroff (determines action from argv[0])
reboot: poweroff
	cp poweroff reboot

halt: poweroff
	cp poweroff halt

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o $(PROGRAMS) reboot halt

.PHONY: all clean
