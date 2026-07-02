CC = gcc
LD = ld

LIBC_DIR = ../lib/libc
RTLD_DIR = ../lib/rtld
LIBPAM_DIR = ../lib/libpam
LIBCRYPT_DIR = ../lib/libcrypt

CFLAGS = -Wall -Wextra -O2 -nostdlib -fno-builtin -fstack-protector-strong \
         -mno-red-zone -ffreestanding -fPIE -I$(LIBC_DIR)/include \
         -I$(LIBPAM_DIR)/include \
         -fno-tree-vectorize -fno-strict-aliasing

# Dynamic PIE linking against libc.so (includes pthread)
LDFLAGS = -pie -nostdlib -T user_dyn.lds --hash-style=gnu \
          -z now \
          -rpath /lib -dynamic-linker /lib/ld-likeos.so \
          -L$(LIBC_DIR) -L$(RTLD_DIR)

CRT1 = $(LIBC_DIR)/crt1.o

# Shared libraries to link against (ld-likeos.so provides _rtld_* symbols for dlfcn)
LIBS = -lc -l:ld-likeos.so

# Extra flags for auth programs that pull in libpam/libcrypt.
# -rpath-link lets ld resolve the *transitive* DT_NEEDED of our shared libs
# (libpam -> libcrypt -> libc) from the LikeOS build tree instead of falling
# through to the host's /usr/lib libraries.
AUTH_LDFLAGS = -L$(LIBPAM_DIR) -L$(LIBCRYPT_DIR) \
               -rpath-link $(LIBC_DIR):$(LIBCRYPT_DIR):$(LIBPAM_DIR):$(RTLD_DIR)

# Programs (sources live alongside this Makefile in user/bin/)
# getty/init link only against libc; login needs libpam+libcrypt (custom rule below).
PROGRAMS = hello sh ls cat pwd stat progerr memstat uname shutdown poweroff ps cp mv rm mkdir rmdir touch more less clear env kill find df du hexdump sleep strings file grep wc head tail echo printf free uptime dmesg which date time sort uniq cut tr yes true false top man hostname ping ifconfig netstat route arp traceroute arping dhclient dig nslookup host ln chmod readlink getty init id whoami groups adduser deluser permbench

# Test programs (sources live in user/bin/tests/, built as tests/<name>)
# test_libc has a custom rule (needs libpam+libcrypt for the auth tests).
TESTS = test_syscalls testmem teststress netstress openssltest usbtest ext4test
TEST_BINS = $(addprefix tests/,$(TESTS))

all: $(PROGRAMS) $(TEST_BINS) tests/test_libc login passwd su addgroup delgroup reboot halt

# Generic link rule: crt1.o + program.o → PIE binary linked against libc.so + libpthread.so
define LINK_RULE
$(1): $(CRT1) $(1).o
	$(LD) $(LDFLAGS) $(CRT1) $(1).o $(LIBS) -o $$@
endef

$(foreach prog,$(PROGRAMS),$(eval $(call LINK_RULE,$(prog))))
$(foreach prog,$(TEST_BINS),$(eval $(call LINK_RULE,$(prog))))

# login/su authenticate via libpam (which pulls in libcrypt transitively).
login: $(CRT1) login.o
	$(LD) $(LDFLAGS) $(AUTH_LDFLAGS) $(CRT1) login.o -lpam $(LIBS) -o $@

su: $(CRT1) su.o
	$(LD) $(LDFLAGS) $(AUTH_LDFLAGS) $(CRT1) su.o -lpam $(LIBS) -o $@

# passwd hashes with libcrypt (crypt + crypt_gensalt) directly.
passwd: $(CRT1) passwd.o
	$(LD) $(LDFLAGS) $(AUTH_LDFLAGS) $(CRT1) passwd.o -lcrypt $(LIBS) -o $@

# addgroup/delgroup are copies of adduser/deluser (mode chosen from argv[0]).
addgroup: adduser
	cp adduser addgroup

delgroup: deluser
	cp deluser delgroup

# test_libc: exercises pam_* and crypt() directly, so link both explicitly.
tests/test_libc: $(CRT1) tests/test_libc.o
	$(LD) $(LDFLAGS) $(AUTH_LDFLAGS) $(CRT1) tests/test_libc.o -lpam -lcrypt $(LIBS) -o $@

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
	rm -f *.o $(PROGRAMS) login passwd su addgroup delgroup reboot halt tests/*.o $(TEST_BINS) tests/test_libc

.PHONY: all clean
