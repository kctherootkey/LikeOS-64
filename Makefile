# LikeOS-64 UEFI Build System
# Professional UEFI kernel build with modular directory structure

# Pass DEBUG=1 on the command line to enable verbose stack-smash output in libc
# and kernel memory poisoning (freed slabs, freed pages, uninitialized allocs).
# DEBUG=1 also implies NO_STRIP=1 and adds -g3 so RIP-around-fault byte dumps
# can be disassembled post-mortem (objdump -d build/kernel.elf).
DEBUG ?= 0

# Pass CRASH_VERBOSE=1 to enable the detailed userspace + kernel crash reports
# (full register dump, decoded fault reason, page-table walk of the faulting
# VA, mmap region list, segment/MSR state, ...) in a build that is otherwise
# stripped and has NO memory poisoning — i.e. a production-like build.  This
# is the knob for debugging crashes that only reproduce in production.
#
# It deliberately does NOT enable the "bytes around RIP" hexdumps: those are
# only useful alongside the DWARF symbols and unstripped ELF that DEBUG=1
# provides, so they stay gated behind DEBUG.
#
# DEBUG=1 implies CRASH_VERBOSE=1 (a debug build is a superset).
CRASH_VERBOSE ?= 0
ifeq ($(DEBUG),1)
  override CRASH_VERBOSE := 1
endif

ifeq ($(DEBUG),1)
  KERNEL_DEBUG_CFLAGS = -DDEBUG=1 -DCRASH_VERBOSE=1 -g3 -gdwarf-4
  # Force preservation of the symbol table when DEBUG is set.  Without this
  # the post-mortem byte dump emitted by the oops handler is useless because
  # we cannot resolve RIP back to a function name.  Use `override` so a
  # stray `NO_STRIP=` on the command line cannot quietly turn symbols off.
  override NO_STRIP := 1
else ifeq ($(CRASH_VERBOSE),1)
  # Production-like build: verbose crash reports, but stripped, no -g3, no
  # symbol-table preservation and no memory poisoning.
  KERNEL_DEBUG_CFLAGS = -DCRASH_VERBOSE=1
else
  KERNEL_DEBUG_CFLAGS =
endif

# Codename for this release
CODENAME = blessed kitty

# Version string: override on command line with LIKEOS_VERSION=x.y.z
LIKEOS_VERSION ?= 0.2.5-HEAD

# Tools
GCC = gcc
LD = ld
OBJCOPY = objcopy
STRIP = strip

# Symbol retention for USERSPACE binaries, separate from the kernel's NO_STRIP.
#
# Every userspace program and library is stripped by default, which keeps the
# image small but leaves a debugger with nothing to work from: no function
# names, no line numbers, no way to place a breakpoint by anything but an
# address.  Building with NO_STRIP_USER=1 keeps the symbol tables, and pairing
# it with -g in the port's own flags keeps the DWARF too.
#
# Implied by DEBUG=1, for the same reason it implies the kernel's NO_STRIP: a
# debug build that cannot be debugged is not one.
#
# One exemption: gdb itself is always stripped, because its own symbols cannot
# help it debug anything else and they cost 148M.  See $(BUILD_DIR)/gdb.
#
# `true` rather than a conditional at all 100-odd call sites: it accepts and
# ignores the arguments, so the recipes do not have to change shape.
ifeq ($(DEBUG),1)
  override NO_STRIP_USER := 1
endif
ifdef NO_STRIP_USER
  USER_STRIP = true
else
  USER_STRIP = $(STRIP)
endif
DD = dd
QEMU = qemu-system-x86_64
XORRISO = xorriso

# RAM for all QEMU targets (override with e.g. QEMU_MEM=2G on the command line)
QEMU_MEM ?= 1024M

# SMP configuration for QEMU targets:
#   NUM_CPUS=N   - set number of CPUs (default: 4)
#   NO_SMP=1     - disable SMP entirely (omit -smp argument)
ifdef NO_SMP
  QEMU_SMP =
else
  NUM_CPUS ?= 4
  QEMU_SMP = -smp $(NUM_CPUS)
endif

# Serial console: pass SERIAL=1 on the command line to enable kprintf mirroring
# to COM1 and QEMU -serial stdio.  Default is off (no serial output).
ifdef SERIAL
  SERIAL_CFLAGS = -DSERIAL_ENABLED
  QEMU_SERIAL = -serial stdio
else
  SERIAL_CFLAGS =
  QEMU_SERIAL =
endif

# USB serial logging: pass USB_SERIAL=1 on the command line to enable
# USB CDC ACM serial detection and log mirroring on real USB boots.
ifeq ($(USB_SERIAL),1)
	USB_SERIAL_CFLAGS = -DUSB_SERIAL_ENABLED
else
	USB_SERIAL_CFLAGS =
endif

# USB HID: pass USB_HID=1 on the command line to add USB keyboard and mouse
# to QEMU targets (qemu-usb, qemu-usb-gdb).  Enables -device usb-kbd and
# -device usb-mouse on the xHCI controller.  Default is off.
ifdef USB_HID
  QEMU_USB_HID = -device usb-kbd -device usb-mouse
else
  QEMU_USB_HID =
endif

# Screen size: 1920x1200 preferred resolution by DEFAULT; pass
# SCREEN_SIZE=medium for 1280x800.
# The kernel gets the same define so the SVGA driver's boot-time best-fit
# mode selection uses the identical preferred-resolution table.
#
# Large is the default because both ends of the mode list are a preference
# rather than a demand: the bootloader walks the fallback chain down to
# 1024x768, so asking for 1920x1200 on a display that cannot do it costs
# nothing, while asking for 1280x800 on one that can caps it there.  It also
# matches the shipped wallpaper, which is 1920x1200.
ifeq ($(SCREEN_SIZE),medium)
  SCREEN_PREF_CFLAGS =
else
  SCREEN_PREF_CFLAGS = -DSCREEN_LARGE
endif

# Maximum screen size: pass MAX_SCREEN_SIZE=WIDTHxHEIGHT (for example
# MAX_SCREEN_SIZE=1920x1080) to put a ceiling on that table.  Entries above the
# ceiling are skipped, so the chosen mode is the largest one that both the
# SCREEN_SIZE list offers and the ceiling allows -- 1920x1080 rather than
# 1920x1200 for that example.  Unset means no ceiling (the previous behaviour).
#
# This exists because a virtual machine advertises the modes its emulated
# adapter can scan out, not the ones the host's panel can show: on a 1920x1080
# notebook, VirtualBox, VMware and QEMU all offer 1920x1200 and a default build
# takes it, leaving a guest screen the host has to shrink to fit -- which is
# exactly what full-screen mode then cannot do cleanly.
ifdef MAX_SCREEN_SIZE
  MAX_SCREEN_W := $(word 1,$(subst x, ,$(subst X,x,$(strip $(MAX_SCREEN_SIZE)))))
  MAX_SCREEN_H := $(word 2,$(subst x, ,$(subst X,x,$(strip $(MAX_SCREEN_SIZE)))))
  ifneq ($(shell printf '%s' '$(strip $(MAX_SCREEN_SIZE))' | grep -Eqi '^[0-9]+x[0-9]+$$' && echo ok),ok)
    $(error MAX_SCREEN_SIZE must be WIDTHxHEIGHT, e.g. MAX_SCREEN_SIZE=1920x1080)
  endif
  # Below the smallest entry of either table nothing is selectable, and the
  # firmware default -- which may well be larger -- is what you get instead.
  ifneq ($(shell test $(MAX_SCREEN_W) -ge 1024 && test $(MAX_SCREEN_H) -ge 768 && echo ok),ok)
    $(warning MAX_SCREEN_SIZE=$(MAX_SCREEN_SIZE) is below 1024x768, the smallest preferred mode: no mode will be selected and the firmware default is kept)
  endif
  SCREEN_MAX_CFLAGS = -DSCREEN_MAX_WIDTH=$(MAX_SCREEN_W)U -DSCREEN_MAX_HEIGHT=$(MAX_SCREEN_H)U
else
  SCREEN_MAX_CFLAGS =
endif

UEFI_SCREEN_CFLAGS = $(SCREEN_PREF_CFLAGS) $(SCREEN_MAX_CFLAGS)
KERNEL_SCREEN_CFLAGS = $(SCREEN_PREF_CFLAGS) $(SCREEN_MAX_CFLAGS)

# All QEMU run targets use the VMware SVGA II display adapter so the vmsvga2
# kernel driver is exercised; the GOP framebuffer remains the fallback path.
QEMU_VGA = -vga vmware

MKFS_FAT = mkfs.fat
MTOOLS = mcopy

# Directories
BUILD_DIR = build
KERNEL_DIR = kernel
INCLUDE_DIR = include
BOOT_DIR = boot
USER_DIR = user/bin

# UEFI/GNU-EFI paths
EFI_INCLUDES = -I/usr/include/efi -I/usr/include/efi/x86_64
EFI_LIBS = /usr/lib/crt0-efi-x86_64.o
EFI_LDS = /usr/lib/elf_x86_64_efi.lds

# Compiler flags for kernel
BUILD_DATE := $(shell LC_ALL=C date -u '+%a %b %-d %H:%M:%S UTC %Y')
# The kernel runs on the CURRENT task's FPU register file, and that file only
# reaches the task's save area at the next context switch -- so one
# compiler-generated XMM move in kernel code silently rewrites a running
# program's floating-point state, and the corrupted values are what get saved.
# Interrupt entry already saves XMM0-15, but the syscall path does not, so
# codegen is kept integer-only here.  fb.c is the one deliberate exception (see
# its per-file flags below): it uses SSE for the framebuffer blits and brackets
# them with kernel_fpu_begin()/kernel_fpu_end().
KERNEL_CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
			-mno-sse -mno-sse2 -mno-mmx -mno-80387 \
			-fstack-protector-strong -mstack-protector-guard=tls \
			-mstack-protector-guard-reg=gs -mstack-protector-guard-offset=104 \
			-mno-red-zone -mcmodel=large -fno-pic -Wall -Wextra \
			-I$(INCLUDE_DIR) -I$(KERNEL_DIR)/hal/acpica/include \
			-D__LIKEOS__ -D__LIKEOS_KERNEL__ -DACPI_USE_BUILTIN_STDARG \
			-U__linux__ -U_LINUX -Ulinux \
			-DXHCI_USE_INTERRUPTS=1 $(SERIAL_CFLAGS) $(USB_SERIAL_CFLAGS) \
			$(KERNEL_SCREEN_CFLAGS) \
			-DBUILD_DATE='"$(BUILD_DATE)"' \
			-DLIKEOS_VERSION='"$(LIKEOS_VERSION)"' \
			$(KERNEL_DEBUG_CFLAGS)

# Extra flags for ACPICA sources (suppress upstream warnings)
# -U__linux__ -U_LINUX: prevent ACPICA from selecting aclinux.h (GCC defines
#   __linux__ even with -ffreestanding; we want our aclikeos.h instead).
ACPICA_CFLAGS = $(KERNEL_CFLAGS) -DACPI_USE_BUILTIN_STDARG \
			-U__linux__ -U_LINUX -Ulinux \
			-Wno-unused-parameter -Wno-unused-variable \
			-Wno-implicit-fallthrough -Wno-sign-compare -Wno-missing-field-initializers \
			-Wno-type-limits -Wno-override-init

# ACPICA source files (auto-discovered, excluding debugger/disassembler/dump)
ACPICA_DIR = $(KERNEL_DIR)/hal/acpica
ACPICA_SRCS = $(shell find $(ACPICA_DIR) -name '*.c' \
			-not -path '*/debugger/*' -not -path '*/disassembler/*' \
			-not -name 'rsdump.c' -not -name 'rsdumpinfo.c' | sort)
ACPICA_OBJS = $(patsubst $(ACPICA_DIR)/%.c,$(BUILD_DIR)/acpica/%.o,$(ACPICA_SRCS))

# Compiler flags for userspace programs
USER_CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
			-fstack-protector-strong -mno-red-zone -mcmodel=small -fno-pic -Wall -Wextra \
			-I$(USER_DIR)

# Compiler flags for UEFI bootloader
UEFI_CFLAGS = -fstack-protector-strong -mstack-protector-guard=global -fpic -fshort-wchar -mno-red-zone \
              -maccumulate-outgoing-args $(EFI_INCLUDES) -DEFI_FUNCTION_WRAPPER \
              $(UEFI_SCREEN_CFLAGS)

# Linker flags
KERNEL_LDFLAGS = -nostdlib -static
UEFI_LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic

# Kernel object files
KERNEL_OBJS = $(BUILD_DIR)/init.o \
			  $(BUILD_DIR)/userinit.o \
			  $(BUILD_DIR)/xhci_boot.o \
			  $(BUILD_DIR)/storage.o \
              $(BUILD_DIR)/console.o \
              $(BUILD_DIR)/sysfont.o \
              $(BUILD_DIR)/unicode.o \
              $(BUILD_DIR)/cursor.o \
              $(BUILD_DIR)/fb.o \
              $(BUILD_DIR)/vmsvga2.o \
              $(BUILD_DIR)/fbdev.o \
              $(BUILD_DIR)/evdev.o \
              $(BUILD_DIR)/interrupt.o \
              $(BUILD_DIR)/interrupt_c.o \
              $(BUILD_DIR)/gdt.o \
              $(BUILD_DIR)/gdt_c.o \
              $(BUILD_DIR)/keyboard.o \
			  $(BUILD_DIR)/serial.o \
              $(BUILD_DIR)/mouse.o \
              $(BUILD_DIR)/memory.o \
			  $(BUILD_DIR)/stack_switch.o \
			  $(BUILD_DIR)/slab.o $(BUILD_DIR)/shm.o \
			  $(BUILD_DIR)/mm_rwsem.o \
			  $(BUILD_DIR)/scrollbar.o \
			  $(BUILD_DIR)/vfs.o \
			  $(BUILD_DIR)/frlock.o \
			  $(BUILD_DIR)/devfs.o \
			  $(BUILD_DIR)/tty.o \
			  $(BUILD_DIR)/vt.o \
			  $(BUILD_DIR)/pci.o \
			  $(BUILD_DIR)/block.o \
			  $(BUILD_DIR)/xhci.o \
			  $(BUILD_DIR)/fat32.o \
			  $(BUILD_DIR)/pagecache.o \
			  $(BUILD_DIR)/dcache.o \
			  $(BUILD_DIR)/icache.o \
			  $(BUILD_DIR)/ext4.o \
			  $(BUILD_DIR)/usb.o \
			  $(BUILD_DIR)/usb_serial.o \
			  $(BUILD_DIR)/usb_msd.o \
			  $(BUILD_DIR)/usb_hid.o \
			  $(BUILD_DIR)/ps2.o \
			  $(BUILD_DIR)/ioapic.o \
			  $(BUILD_DIR)/timer.o \
			  $(BUILD_DIR)/sched.o \
			  $(BUILD_DIR)/syscall.o \
			  $(BUILD_DIR)/syscall_c.o \
			  $(BUILD_DIR)/uaccess.o \
			  $(BUILD_DIR)/fs_read_write.o \
			  $(BUILD_DIR)/fs_file.o \
			  $(BUILD_DIR)/fs_namei.o \
			  $(BUILD_DIR)/fs_open.o \
			  $(BUILD_DIR)/fs_stat.o \
			  $(BUILD_DIR)/fs_readdir.o \
			  $(BUILD_DIR)/fs_xattr.o \
			  $(BUILD_DIR)/fs_ioctl.o \
			  $(BUILD_DIR)/fs_sync.o \
			  $(BUILD_DIR)/mm_mmap.o \
			  $(BUILD_DIR)/mm_mprotect.o \
			  $(BUILD_DIR)/mm_madvise.o \
			  $(BUILD_DIR)/ke_fork.o \
			  $(BUILD_DIR)/ke_exit.o \
			  $(BUILD_DIR)/ke_exec.o \
			  $(BUILD_DIR)/ke_process.o \
			  $(BUILD_DIR)/ke_time.o \
			  $(BUILD_DIR)/ke_system.o \
			  $(BUILD_DIR)/sched_syscalls.o \
			  $(BUILD_DIR)/futex_syscalls.o \
			  $(BUILD_DIR)/ke_ptrace.o \
			  $(BUILD_DIR)/net_syscalls.o \
			  $(BUILD_DIR)/elf_loader.o \
			  $(BUILD_DIR)/script_loader.o \
			  $(BUILD_DIR)/pipe.o \
			  $(BUILD_DIR)/stack_guard.o \
			  $(BUILD_DIR)/signal.o \
			  $(BUILD_DIR)/lapic.o \
			  $(BUILD_DIR)/cpu_pstate.o \
			  $(BUILD_DIR)/acpi.o \
			  $(ACPICA_OBJS) \
			  $(BUILD_DIR)/percpu.o \
			  $(BUILD_DIR)/smp.o \
			  $(BUILD_DIR)/ap_trampoline.o \
			  $(BUILD_DIR)/futex.o \
			  $(BUILD_DIR)/cred.o \
			  $(BUILD_DIR)/i2c_hid.o \
			  $(BUILD_DIR)/net.o \
			  $(BUILD_DIR)/e1000.o \
			  $(BUILD_DIR)/e1000e.o \
			  $(BUILD_DIR)/rtl8139.o \
			  $(BUILD_DIR)/pcnet32.o \
			  $(BUILD_DIR)/ne2k.o \
			  $(BUILD_DIR)/vmxnet3.o \
			  $(BUILD_DIR)/eepro100.o \
			  $(BUILD_DIR)/igb.o \
			  $(BUILD_DIR)/tulip.o \
			  $(BUILD_DIR)/ethernet.o \
			  $(BUILD_DIR)/arp.o \
			  $(BUILD_DIR)/ipv4.o \
			  $(BUILD_DIR)/icmp.o \
			  $(BUILD_DIR)/udp.o \
			  $(BUILD_DIR)/tcp.o \
			  $(BUILD_DIR)/dhcp.o \
			  $(BUILD_DIR)/socket.o \
			  $(BUILD_DIR)/poll.o \
			  $(BUILD_DIR)/random.o \
			  $(BUILD_DIR)/route.o \
			  $(BUILD_DIR)/dns.o \
			  $(BUILD_DIR)/igmp.o \
			  $(BUILD_DIR)/unix_socket.o \
			  $(BUILD_DIR)/skb.o \
			  $(BUILD_DIR)/softirq.o \
			  $(BUILD_DIR)/ratelimit.o \
			  $(BUILD_DIR)/netstats.o
# Target files
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
BOOTLOADER_EFI = $(BUILD_DIR)/bootloader.efi

# Single GPT USB disk = the one and only bootable image.  Layout:
#   P1: small FAT EFI System Partition carrying ONLY the bootloader
#       (/EFI/BOOT/BOOTX64.EFI) — FAT is mandated by UEFI for the ESP.
#   P2: ext4 root holding /boot/kernel.elf and the whole userland.
# The bootloader reads /boot/kernel.elf straight from the ext4 partition, so
# the complete OS lives on one stick with no FAT data filesystem anywhere.
EXT4_STAGING  = $(BUILD_DIR)/ext4_staging
EXT4_ROOT_IMG = $(BUILD_DIR)/ext4root.img
EXT4_ESP_IMG  = $(BUILD_DIR)/ext4esp.img
GPT_DISK      = $(BUILD_DIR)/likeos-ext4.img
ESP_MB       ?= 64
# ext4 root partition size in MB.  Leave empty to auto-size to the staged
# content (+50% slack, min 64M); override e.g. EXT4_MB=512 to force a size.
EXT4_MB      ?=
# mkfs.ext4 feature flags.  metadata_csum is ON by default: the driver maintains
# every metadata checksum on WRITE (P6 Step 2 — verified e2fsck-clean 2026-06-17).
# 64bit stays off (32-byte group descriptors), the verified configuration.
# Overrides:
#   make EXT4_MKFS_FEATURES=                       (also enable 64bit / 64-byte descs)
#   make EXT4_MKFS_FEATURES=^metadata_csum,^64bit  (legacy no-csum image)
EXT4_MKFS_FEATURES ?= ^64bit
LINUX_USB_DIR = host/linux-usb
LINUX_USB_BUILD_DIR = $(BUILD_DIR)/linux-usb
LINUX_USB_IMAGE = $(LINUX_USB_BUILD_DIR)/linux-usb.img

# ---------------------------------------------------------------------------
# Canonical userland file lists shared by the ext4 image build.
#   ROOT_BIN_PROGS    -> /bin/<name>           (dest basename == build basename)
#   ROOT_LIBS         -> /lib/<name>
#   ROOT_USRLOCAL_BINS-> /usr/local/bin/<name> (a few are renamed; see recipe)
# ---------------------------------------------------------------------------
ROOT_BIN_PROGS = bash ls cat cmp pwd stat uname shutdown poweroff reboot halt ps cp mv rm \
	mkdir rmdir ln chmod readlink touch more less clear env kill find df du hexdump \
	sleep strings file grep wc head tail echo printf free uptime nproc dmesg which date time \
	sort uniq cut tr sed expr tty yes true false top man hostname ping ifconfig netstat route arp \
	traceroute arping dhclient dig nslookup host nano tmux nc openssl curl login \
	id whoami groups su passwd adduser addgroup deluser delgroup kdump \
	gdb gdbserver
# System binaries -> /sbin/<name>
ROOT_SBIN_PROGS = init getty
ROOT_LIBS = ld-likeos.so libc.so ncurses.so libevent.so libcrypto.so.3 libssl.so.3 \
	libz.so.1 libnghttp2.so.14 libcurl.so.4 libtestlib.so libcrypt.so libpam.so \
	libdlbase.so libdlchain.so
ROOT_USRLOCAL_BINS = user_test.elf test_libc hello progerr testmem memstat teststress \
	netstress openssltest usbtest ext4test permbench fbtest pmap ttydump \
	cxxprobe
# Configuration and data files staged into the image, and the script that stages
# the X.Org tree.  These are prerequisites for exactly the same reason the
# binaries are: editing one and rebuilding has to CHANGE the image.  Without
# them make compared only build artifacts, found them all unchanged, declared
# the image up to date and wrote the PREVIOUS one to the device -- an edited
# config that never reached the running system, which looks identical to the
# edit not working.
RES_PREREQS = res/Uni2-Terminus16.psf res/left_ptr res/nanorc \
	res/LikeOS.png \
	res/etc/skel/.profile res/etc/skel/.bashrc \
	$(wildcard res/etc/skel/Desktop/*.desktop) \
	$(wildcard res/man/*.1) $(wildcard res/etc/*) \
	$(wildcard res/etc/ssl/certs/*) $(wildcard res/xorg/*) \
	$(wildcard res/xorg/applications/*) \
	$(wildcard res/xorg/gtk3/*) \
	$(wildcard res/xorg/gtk3/skel-claws-mail/*) \
	ports/xorg/stage.sh ports/xorg/gtk3/stage.sh

# Full prerequisite set for the ext4 image (every staged build artifact).
# The C++ runtime's test program, but only once there is a C++ runtime.
#
# Evaluated when this file is read: if the GTK3 port has installed libstdc++
# into the sysroot, testcxx becomes a prerequisite of the image and is built
# like anything else.  If it has not, the variable is empty and a tree without
# that port still builds a working image.
#
# It used to be neither -- built by hand with `make build/testcxx' and staged
# only if it happened to be there.  `make clean' removes everything under
# build/, so the next image quietly shipped without the one program that
# proves the C++ runtime works, and the staging script's existence check
# turned that into silence rather than an error.
# Sentinel for the GTK3 stack, on the same principle as the X server's below:
# a file that exists only once the port has built, so the image depends on the
# port having RUN rather than on a test of what happens to be lying around.
#
# This was a $(shell test -f .../libstdc++.so) assigned with `:=`, which make
# evaluates while PARSING the makefile -- necessarily before anything is built.
# After `make distclean` the sysroot is gone at that moment, so it expanded to
# nothing and the image was left with no GTK3 prerequisite at all: the port
# never ran, and gtk3/stage.sh, which is deliberately silent when there is
# nothing to stage, produced an image with no Claws Mail and said nothing.  It
# only appeared to work after `make clean`, which keeps the sysroot and so
# leaves the parse-time test looking at the PREVIOUS build's output.
#
# The file it names is the LAST program the manifest builds, which is what
# makes its presence mean "the whole stack".  That used to be Claws Mail and is
# now PCManFM: everything Claws needs is built before it, and PCManFM adds
# GtkSourceView, libxml2, Mousepad, the MIME database and libfm on top -- so a
# sentinel left at claws-mail would have gone on being satisfied by a port that
# stopped halfway through the new packages.
GTK3_SENTINEL = $(BUILD_DIR)/xorg-sysroot/usr/bin/pcmanfm
GTK3_TESTCXX = $(BUILD_DIR)/testcxx

GPT_PREREQS = $(addprefix $(BUILD_DIR)/,$(ROOT_BIN_PROGS) $(ROOT_SBIN_PROGS) $(ROOT_LIBS) $(ROOT_USRLOCAL_BINS)) \
	$(BUILD_DIR)/openssh/bin/ssh \
	$(BUILD_DIR)/xorg-sysroot/usr/bin/Xorg \
	$(GTK3_SENTINEL) \
	$(GTK3_TESTCXX) \
	$(RES_PREREQS)

# Default target: build the single ext4 GPT USB disk.
all: $(GPT_DISK)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# SCREEN_SIZE and MAX_SCREEN_SIZE are baked into the bootloader and the SVGA
# driver at compile time, and no source file changes when they do -- so make
# would happily keep yesterday's objects and boot the old resolution.  This
# stamp holds the current screen flags and is rewritten only when they differ,
# which rebuilds exactly the two objects that read them, without a full clean.
SCREEN_STAMP = $(BUILD_DIR)/.screen_flags

$(SCREEN_STAMP): FORCE | $(BUILD_DIR)
	@echo '$(SCREEN_PREF_CFLAGS) $(SCREEN_MAX_CFLAGS)' | cmp -s - $@ || \
		echo '$(SCREEN_PREF_CFLAGS) $(SCREEN_MAX_CFLAGS)' > $@

FORCE:
.PHONY: FORCE

# Compile kernel source files
$(BUILD_DIR)/init.o: $(KERNEL_DIR)/ke/init.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/userinit.o: $(KERNEL_DIR)/ke/userinit.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci_boot.o: $(KERNEL_DIR)/ke/xhci_boot.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/storage.o: $(KERNEL_DIR)/ke/storage.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.o: $(KERNEL_DIR)/io/console.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sysfont.o: $(KERNEL_DIR)/io/sysfont.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/unicode.o: $(KERNEL_DIR)/io/unicode.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cursor.o: $(KERNEL_DIR)/io/cursor.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# The one file allowed to touch the FPU registers: its SSE blits are the reason
# the framebuffer keeps up.  Every one of them is wrapped in
# kernel_fpu_begin()/kernel_fpu_end(), which parks the interrupted task's
# register file before borrowing it.
$(BUILD_DIR)/fb.o: KERNEL_CFLAGS += -msse -msse2 -mmmx

$(BUILD_DIR)/fb.o: $(KERNEL_DIR)/dev/video/fb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vmsvga2.o: $(KERNEL_DIR)/dev/video/vmsvga2.c $(SCREEN_STAMP) | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fbdev.o: $(KERNEL_DIR)/dev/video/fbdev.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/evdev.o: $(KERNEL_DIR)/dev/input/evdev.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERNEL_DIR)/ke/interrupt.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/interrupt_c.o: $(KERNEL_DIR)/ke/interrupt.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: $(KERNEL_DIR)/ke/gdt.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/gdt_c.o: $(KERNEL_DIR)/ke/gdt.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: $(KERNEL_DIR)/dev/input/keyboard.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: $(KERNEL_DIR)/hal/serial.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mouse.o: $(KERNEL_DIR)/dev/input/mouse.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: $(KERNEL_DIR)/mm/memory.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/stack_switch.o: $(KERNEL_DIR)/mm/stack_switch.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/slab.o: $(KERNEL_DIR)/mm/slab.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/shm.o: $(KERNEL_DIR)/mm/shm.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mm_rwsem.o: $(KERNEL_DIR)/mm/rwsem.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/scrollbar.o: $(KERNEL_DIR)/io/scrollbar.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(KERNEL_DIR)/fs/vfs.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/frlock.o: $(KERNEL_DIR)/fs/frlock.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/devfs.o: $(KERNEL_DIR)/fs/devfs.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci.o: $(KERNEL_DIR)/hal/pci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/block.o: $(KERNEL_DIR)/dev/block/block.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci.o: $(KERNEL_DIR)/dev/usb/xhci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: $(KERNEL_DIR)/fs/fat32.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pagecache.o: $(KERNEL_DIR)/fs/pagecache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/dcache.o: $(KERNEL_DIR)/fs/dcache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/icache.o: $(KERNEL_DIR)/fs/icache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ext4.o: $(KERNEL_DIR)/fs/ext4.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb.o: $(KERNEL_DIR)/dev/usb/usb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_serial.o: $(KERNEL_DIR)/dev/usb/usb_serial.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_msd.o: $(KERNEL_DIR)/dev/usb/usb_msd.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_hid.o: $(KERNEL_DIR)/dev/hid/usb_hid.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ps2.o: $(KERNEL_DIR)/dev/hid/ps2.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/i2c_hid.o: $(KERNEL_DIR)/dev/hid/i2c_hid.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# Networking stack
$(BUILD_DIR)/net.o: $(KERNEL_DIR)/net/net.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/e1000.o: $(KERNEL_DIR)/dev/nic/e1000.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/e1000e.o: $(KERNEL_DIR)/dev/nic/e1000e.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/rtl8139.o: $(KERNEL_DIR)/dev/nic/rtl8139.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pcnet32.o: $(KERNEL_DIR)/dev/nic/pcnet32.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ne2k.o: $(KERNEL_DIR)/dev/nic/ne2k.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vmxnet3.o: $(KERNEL_DIR)/dev/nic/vmxnet3.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/eepro100.o: $(KERNEL_DIR)/dev/nic/eepro100.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/igb.o: $(KERNEL_DIR)/dev/nic/igb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tulip.o: $(KERNEL_DIR)/dev/nic/tulip.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ethernet.o: $(KERNEL_DIR)/net/ethernet.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/arp.o: $(KERNEL_DIR)/net/arp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ipv4.o: $(KERNEL_DIR)/net/ipv4.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/icmp.o: $(KERNEL_DIR)/net/icmp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/udp.o: $(KERNEL_DIR)/net/udp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tcp.o: $(KERNEL_DIR)/net/tcp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/dhcp.o: $(KERNEL_DIR)/net/dhcp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/socket.o: $(KERNEL_DIR)/net/socket.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/poll.o: $(KERNEL_DIR)/net/poll.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/random.o: $(KERNEL_DIR)/dev/rand/random.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/route.o: $(KERNEL_DIR)/net/route.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/dns.o: $(KERNEL_DIR)/net/dns.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/igmp.o: $(KERNEL_DIR)/net/igmp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/unix_socket.o: $(KERNEL_DIR)/net/unix_socket.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/skb.o: $(KERNEL_DIR)/net/skb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/softirq.o: $(KERNEL_DIR)/net/softirq.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ratelimit.o: $(KERNEL_DIR)/net/ratelimit.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/netstats.o: $(KERNEL_DIR)/net/stats.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ioapic.o: $(KERNEL_DIR)/hal/ioapic.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/timer.o: $(KERNEL_DIR)/ke/timer.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sched.o: $(KERNEL_DIR)/ke/sched.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tty.o: $(KERNEL_DIR)/io/tty.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vt.o: $(KERNEL_DIR)/io/vt.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: $(KERNEL_DIR)/ke/syscall.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/syscall_c.o: $(KERNEL_DIR)/ke/syscall.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/uaccess.o: $(KERNEL_DIR)/ke/uaccess.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_read_write.o: $(KERNEL_DIR)/fs/read_write.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_file.o: $(KERNEL_DIR)/fs/file.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_namei.o: $(KERNEL_DIR)/fs/namei.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_open.o: $(KERNEL_DIR)/fs/open.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_stat.o: $(KERNEL_DIR)/fs/stat.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_readdir.o: $(KERNEL_DIR)/fs/readdir.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_xattr.o: $(KERNEL_DIR)/fs/xattr.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_ioctl.o: $(KERNEL_DIR)/fs/ioctl.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fs_sync.o: $(KERNEL_DIR)/fs/sync.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mm_mmap.o: $(KERNEL_DIR)/mm/mmap.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mm_mprotect.o: $(KERNEL_DIR)/mm/mprotect.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mm_madvise.o: $(KERNEL_DIR)/mm/madvise.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_fork.o: $(KERNEL_DIR)/ke/fork.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_exit.o: $(KERNEL_DIR)/ke/exit.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_exec.o: $(KERNEL_DIR)/ke/exec.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_process.o: $(KERNEL_DIR)/ke/process.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_time.o: $(KERNEL_DIR)/ke/time.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_system.o: $(KERNEL_DIR)/ke/system.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sched_syscalls.o: $(KERNEL_DIR)/ke/sched_syscalls.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/futex_syscalls.o: $(KERNEL_DIR)/ke/futex_syscalls.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ke_ptrace.o: $(KERNEL_DIR)/ke/ptrace.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/net_syscalls.o: $(KERNEL_DIR)/net/syscalls.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf_loader.o: $(KERNEL_DIR)/ke/elf_loader.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/script_loader.o: $(KERNEL_DIR)/ke/script_loader.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pipe.o: $(KERNEL_DIR)/ke/pipe.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/stack_guard.o: $(KERNEL_DIR)/ke/stack_guard.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/signal.o: $(KERNEL_DIR)/ke/signal.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lapic.o: $(KERNEL_DIR)/hal/lapic.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cpu_pstate.o: $(KERNEL_DIR)/hal/cpu_pstate.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/acpi.o: $(KERNEL_DIR)/hal/acpi.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# Pattern rule for all ACPICA source files
$(BUILD_DIR)/acpica/%.o: $(ACPICA_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(GCC) $(ACPICA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/percpu.o: $(KERNEL_DIR)/ke/percpu.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/smp.o: $(KERNEL_DIR)/ke/smp.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ap_trampoline.o: $(KERNEL_DIR)/ke/ap_trampoline.S | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/futex.o: $(KERNEL_DIR)/ke/futex.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cred.o: $(KERNEL_DIR)/ke/cred.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# Build userland C library
.PHONY: userland-libc
userland-libc:
	$(MAKE) -C user/lib/libc DEBUG=$(DEBUG)

# Build runtime linker
.PHONY: userland-rtld
userland-rtld:
	$(MAKE) -C user/lib/rtld DEBUG=$(DEBUG)

# Build test shared library
.PHONY: userland-testlib
userland-testlib:
	$(MAKE) -C user/lib/testlib

# Two-level DSO chain (libdlchain.so -> libdlbase.so) used only by testlibc to
# prove dlopen() relocates and initialises a dependency, not just the object
# it was handed.
.PHONY: userland-dlchain
userland-dlchain:
	$(MAKE) -C user/lib/dlchain

# Build password-hashing library (yescrypt-based crypt())
.PHONY: userland-libcrypt
userland-libcrypt: userland-libc
	$(MAKE) -C user/lib/libcrypt

# Build minimal PAM library (depends on libcrypt for crypt())
.PHONY: userland-libpam
userland-libpam: userland-libc userland-libcrypt
	$(MAKE) -C user/lib/libpam

# Copy shared libraries to build directory
$(BUILD_DIR)/ld-likeos.so: userland-rtld | $(BUILD_DIR)
	cp user/lib/rtld/ld-likeos.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libc.so: userland-libc | $(BUILD_DIR)
	cp user/lib/libc/libc.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libtestlib.so: userland-testlib | $(BUILD_DIR)
	cp user/lib/testlib/libtestlib.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libdlbase.so: userland-dlchain | $(BUILD_DIR)
	cp user/lib/dlchain/libdlbase.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libdlchain.so: userland-dlchain | $(BUILD_DIR)
	cp user/lib/dlchain/libdlchain.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libcrypt.so: userland-libcrypt | $(BUILD_DIR)
	cp user/lib/libcrypt/libcrypt.so $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/libpam.so: userland-libpam | $(BUILD_DIR)
	cp user/lib/libpam/libpam.so $@
	$(USER_STRIP) --strip-unneeded $@

# Build test programs using libc
$(BUILD_DIR)/user_test.elf: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/test_syscalls
	cp $(USER_DIR)/tests/test_syscalls $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/test_libc: userland-libc userland-rtld userland-libcrypt userland-libpam | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/test_libc
	cp $(USER_DIR)/tests/test_libc $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ext4test: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/ext4test
	cp $(USER_DIR)/tests/ext4test $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/hello: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hello
	cp $(USER_DIR)/hello $@
	$(USER_STRIP) --strip-unneeded $@

# Login stack: login authenticates via libpam/libcrypt; getty+init use libc only.
$(BUILD_DIR)/login: userland-libc userland-rtld userland-libcrypt userland-libpam | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) login
	cp $(USER_DIR)/login $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/getty: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) getty
	cp $(USER_DIR)/getty $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/init: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) init
	cp $(USER_DIR)/init $@
	$(USER_STRIP) --strip-unneeded $@

# Account/identity utilities.  id/whoami/groups/adduser/addgroup use libc only;
# su and passwd link against libpam/libcrypt.
$(BUILD_DIR)/id: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) id
	cp $(USER_DIR)/id $@
	$(USER_STRIP) --strip-unneeded $@

# /dev/fb0 exerciser (X.org fbdev-style ioctl+mmap sequence)
$(BUILD_DIR)/fbtest: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/fbtest
	cp $(USER_DIR)/tests/fbtest $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/whoami: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) whoami
	cp $(USER_DIR)/whoami $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/groups: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) groups
	cp $(USER_DIR)/groups $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/adduser: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) adduser
	cp $(USER_DIR)/adduser $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/addgroup: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) addgroup
	cp $(USER_DIR)/addgroup $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/deluser: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) deluser
	cp $(USER_DIR)/deluser $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/delgroup: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) delgroup
	cp $(USER_DIR)/delgroup $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/su: userland-libc userland-rtld userland-libcrypt userland-libpam | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) su
	cp $(USER_DIR)/su $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/passwd: userland-libc userland-rtld userland-libcrypt | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) passwd
	cp $(USER_DIR)/passwd $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ls: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ls
	cp $(USER_DIR)/ls $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/cat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cat
	cp $(USER_DIR)/cat $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/pwd: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) pwd
	cp $(USER_DIR)/pwd $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/stat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) stat
	cp $(USER_DIR)/stat $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/progerr: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) progerr
	cp $(USER_DIR)/progerr $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/testmem: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/testmem
	cp $(USER_DIR)/tests/testmem $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/memstat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) memstat
	cp $(USER_DIR)/memstat $@

# pmap: prints a process's region table and brk, and can watch them for
# movement.  ps reports one VSZ total, which cannot separate a table filling
# up from a few regions growing.
$(BUILD_DIR)/pmap: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/pmap
	cp $(USER_DIR)/tests/pmap $@

# cxxprobe: what a C++ MAIN EXECUTABLE needs from this system, step by step.
#
# Built the way gdb is -- likeos-c++, the X sysroot, and the compiler's own
# default link -- and not the way everything in user/ is, because that is the
# whole point: a program linked through user_dyn.lds comes out with neither a
# DT_INIT_ARRAY nor symbol versioning, and gdb has both.  It prints a marker
# around every step, so a crash names the feature that broke rather than
# leaving an 11 MB binary and a null RIP to explain themselves.
$(BUILD_DIR)/cxxprobe: user/tests/cxxprobe.cc userland-libc userland-rtld ports-gtk3 | $(BUILD_DIR)
	LIKEOS_SYSROOT=$(CURDIR)/build/xorg-sysroot \
		ports/xorg/toolchain/likeos-c++ -O2 -o $@ $<
	$(USER_STRIP) --strip-unneeded $@
	$(USER_STRIP) --strip-unneeded $@

# ttydump: shows what the terminal actually sends, and in which reads.  Every
# other reader on this image is canonical-mode, which hides any sequence that
# does not end in a newline; this one sets its own raw mode.
$(BUILD_DIR)/ttydump: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/ttydump
	cp $(USER_DIR)/tests/ttydump $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/teststress: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/teststress
	cp $(USER_DIR)/tests/teststress $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/netstress: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/netstress
	cp $(USER_DIR)/tests/netstress $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/openssltest: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/openssltest
	cp $(USER_DIR)/tests/openssltest $@
	$(USER_STRIP) --strip-unneeded $@

# openssltest is staged into the ext4 image via GPT_PREREQS (ROOT_USRLOCAL_BINS).

$(BUILD_DIR)/usbtest: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/usbtest
	cp $(USER_DIR)/tests/usbtest $@
	$(USER_STRIP) --strip-unneeded $@

# usbtest is staged into the ext4 image via GPT_PREREQS (ROOT_USRLOCAL_BINS).

$(BUILD_DIR)/permbench: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) permbench
	cp $(USER_DIR)/permbench $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/uname: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uname
	cp $(USER_DIR)/uname $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/shutdown: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) shutdown
	cp $(USER_DIR)/shutdown $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/poweroff: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) poweroff
	cp $(USER_DIR)/poweroff $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ps: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ps
	cp $(USER_DIR)/ps $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/cp: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cp
	cp $(USER_DIR)/cp $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/mv: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) mv
	cp $(USER_DIR)/mv $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/rm: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) rm
	cp $(USER_DIR)/rm $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/mkdir: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) mkdir
	cp $(USER_DIR)/mkdir $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ln: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ln
	cp $(USER_DIR)/ln $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/chmod: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) chmod
	cp $(USER_DIR)/chmod $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/readlink: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) readlink
	cp $(USER_DIR)/readlink $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/rmdir: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) rmdir
	cp $(USER_DIR)/rmdir $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/touch: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) touch
	cp $(USER_DIR)/touch $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/more: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) more
	cp $(USER_DIR)/more $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/less: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) less
	cp $(USER_DIR)/less $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/clear: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) clear
	cp $(USER_DIR)/clear $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/env: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) env
	cp $(USER_DIR)/env $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/kill: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) kill
	cp $(USER_DIR)/kill $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/find: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) find
	cp $(USER_DIR)/find $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/df: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) df
	cp $(USER_DIR)/df $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/du: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) du
	cp $(USER_DIR)/du $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/hexdump: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hexdump
	cp $(USER_DIR)/hexdump $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/cmp: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cmp
	cp $(USER_DIR)/cmp $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/sleep: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sleep
	cp $(USER_DIR)/sleep $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/strings: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) strings
	cp $(USER_DIR)/strings $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/file: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) file
	cp $(USER_DIR)/file $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/grep: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) grep
	cp $(USER_DIR)/grep $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/wc: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) wc
	cp $(USER_DIR)/wc $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/head: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) head
	cp $(USER_DIR)/head $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/tail: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tail
	cp $(USER_DIR)/tail $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/echo: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) echo
	cp $(USER_DIR)/echo $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/printf: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) printf
	cp $(USER_DIR)/printf $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/free: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) free
	cp $(USER_DIR)/free $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/uptime: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uptime
	cp $(USER_DIR)/uptime $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/nproc: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) nproc
	cp $(USER_DIR)/nproc $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/dmesg: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dmesg
	cp $(USER_DIR)/dmesg $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/kdump: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) kdump
	cp $(USER_DIR)/kdump $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/which: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) which
	cp $(USER_DIR)/which $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/date: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) date
	cp $(USER_DIR)/date $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/time: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) time
	cp $(USER_DIR)/time $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/sort: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sort
	cp $(USER_DIR)/sort $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/uniq: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uniq
	cp $(USER_DIR)/uniq $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/cut: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cut
	cp $(USER_DIR)/cut $@
	$(USER_STRIP) --strip-unneeded $@

# Text and expression utilities that scripts assume exist.  sed and expr were
# missing until startx needed them: it calls `tty` to find its terminal, `expr`
# to test that against /dev/ttyN, and `sed` to pull a cookie out of xauth's
# output.
$(BUILD_DIR)/sed: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sed
	cp $(USER_DIR)/sed $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/expr: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) expr
	cp $(USER_DIR)/expr $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/tty: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tty
	cp $(USER_DIR)/tty $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/tr: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tr
	cp $(USER_DIR)/tr $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/yes: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) yes
	cp $(USER_DIR)/yes $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/true: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) true
	cp $(USER_DIR)/true $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/false: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) false
	cp $(USER_DIR)/false $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/top: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) top
	cp $(USER_DIR)/top $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/man: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) man
	cp $(USER_DIR)/man $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/hostname: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hostname
	cp $(USER_DIR)/hostname $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ping: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ping
	cp $(USER_DIR)/ping $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/ifconfig: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ifconfig
	cp $(USER_DIR)/ifconfig $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/netstat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) netstat
	cp $(USER_DIR)/netstat $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/route: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) route
	cp $(USER_DIR)/route $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/arp: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) arp
	cp $(USER_DIR)/arp $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/traceroute: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) traceroute
	cp $(USER_DIR)/traceroute $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/arping: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) arping
	cp $(USER_DIR)/arping $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/dhclient: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dhclient
	cp $(USER_DIR)/dhclient $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/dig: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dig
	cp $(USER_DIR)/dig $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/nslookup: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) nslookup
	cp $(USER_DIR)/nslookup $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/host: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) host
	cp $(USER_DIR)/host $@
	$(USER_STRIP) --strip-unneeded $@

$(BUILD_DIR)/reboot: $(BUILD_DIR)/poweroff | $(BUILD_DIR)
	cp $(BUILD_DIR)/poweroff $@

$(BUILD_DIR)/halt: $(BUILD_DIR)/poweroff | $(BUILD_DIR)
	cp $(BUILD_DIR)/poweroff $@

# Build ncurses shared library (custom LikeOS ANSI escape implementation)
.PHONY: ports-ncurses
ports-ncurses: userland-libc
	$(MAKE) -C ports/lib/ncurses-likeos

$(BUILD_DIR)/ncurses.so: ports-ncurses | $(BUILD_DIR)
	cp ports/lib/ncurses-likeos/ncurses.so $@
	$(USER_STRIP) --strip-unneeded $@

# Build GNU nano (ported to LikeOS)
.PHONY: ports-nano
ports-nano: userland-libc userland-rtld ports-ncurses
	$(MAKE) -C ports/nano-8.3 -f Makefile.likeos

$(BUILD_DIR)/nano: ports-nano | $(BUILD_DIR)
	cp ports/nano-8.3/nano $@
	$(USER_STRIP) --strip-unneeded $@

# --- GNU bash ---------------------------------------------------------------
.PHONY: ports-bash
ports-bash: userland-libc userland-rtld ports-ncurses
	$(MAKE) -C ports/bash-5.2.37 -f Makefile.likeos

$(BUILD_DIR)/bash: ports-bash | $(BUILD_DIR)
	cp ports/bash-5.2.37/bash $@
	$(USER_STRIP) --strip-unneeded $@

# Build libevent (shared library used by tmux)
.PHONY: ports-libevent
ports-libevent: userland-libc userland-rtld
	$(MAKE) -C ports/lib/libevent-2.1.12 -f Makefile.likeos

$(BUILD_DIR)/libevent.so: ports-libevent | $(BUILD_DIR)
	cp ports/lib/libevent-2.1.12/libevent.so $@
	$(USER_STRIP) --strip-unneeded $@

# Build tmux (terminal multiplexer)
.PHONY: ports-tmux
ports-tmux: userland-libc userland-rtld ports-ncurses ports-libevent
	$(MAKE) -C ports/tmux-3.6a -f Makefile.likeos

$(BUILD_DIR)/tmux: ports-tmux | $(BUILD_DIR)
	cp ports/tmux-3.6a/tmux $@
	$(USER_STRIP) --strip-unneeded $@

# gdb.
#
# Depends on the GTK3 package set, which is what populates build/xorg-sysroot --
# gdb is C++ and links libstdc++, gmp, mpfr, expat and zlib out of there.  That
# is a heavier prerequisite than any other non-xorg port carries, and it is the
# reason this rule names it rather than only the libc.
#
# It also names ncurses, and build/ncurses.so specifically, which is not
# decoration.  gdb links curses for terminal capabilities even with the TUI
# disabled, and it finds it through a libncurses.so symlink in the sysroot that
# points at build/ncurses.so.  After `make clean' that file is gone and the
# symlink dangles -- whereupon gdb's configure quietly stops finding the
# system's curses and settles for the BUILD HOST's libtinfo instead, producing a
# link full of undefined glibc references and no hint that a missing
# prerequisite caused it.
.PHONY: ports-gdb
ports-gdb: userland-libc userland-rtld ports-ncurses $(BUILD_DIR)/ncurses.so ports-gtk3 | $(BUILD_DIR)
	$(MAKE) -C ports/gdb-17.2 -f Makefile.likeos

# $(STRIP), deliberately, where every other userspace binary uses $(USER_STRIP):
# gdb is the one program on the image exempt from NO_STRIP_USER.
#
# NO_STRIP_USER exists so that a DEBUGGEE has function names for gdb to resolve.
# gdb's own symbols do nothing for that -- a debugger reads its target's DWARF,
# not its own -- and gdb builds with autoconf's default -g -O2, so keeping them
# costs 148M: the binary is 159M unstripped against 10.4M stripped, and the root
# filesystem is sized at 1.5x its contents, so a DEBUG=1 image grew to ~530M to
# carry debug info nothing could use.  Distributions strip it and ship the debug
# info separately for the same reason.
#
# The unstripped binary is not lost: it stays at ports/gdb-17.2/build/gdb/gdb,
# which is what to point a debugger at to debug gdb itself.
$(BUILD_DIR)/gdb: ports-gdb | $(BUILD_DIR)
	cp ports/gdb-17.2/build/gdb/gdb $@
	$(STRIP) --strip-unneeded $@

# gdbserver, from the same tree and stripped for the same reason.
#
# What it is FOR is debugging a LikeOS program from a debugger running
# somewhere else: gdbserver holds the ptrace end here and speaks the remote
# protocol over TCP or a serial line, so the symbols, the source and the 166M
# of gdb stay on the developer's machine.
#
#   on LikeOS:   gdbserver :2345 /usr/local/bin/test_libc all
#                gdbserver --attach :2345 <pid>
#   elsewhere:   gdb <the same program>
#                (gdb) target remote <likeos-ip>:2345
#
# The debugger at the other end has to know this system's OS ABI to relocate a
# position-independent executable and to find shared libraries; a gdb built
# from this port's own source with --target=x86_64-unknown-likeos does, and a
# stock one gets most of the way with `set osabi GNU/Linux'.
$(BUILD_DIR)/gdbserver: ports-gdb | $(BUILD_DIR)
	cp ports/gdb-17.2/build/gdbserver/gdbserver $@
	$(STRIP) --strip-unneeded $@

# Build netcat (nc)
.PHONY: ports-netcat
ports-netcat: userland-libc userland-rtld
	$(MAKE) -C ports/netcat-OpenBSD -f Makefile.likeos

$(BUILD_DIR)/nc: ports-netcat | $(BUILD_DIR)
	cp ports/netcat-OpenBSD/nc $@
	$(USER_STRIP) --strip-unneeded $@

# Build OpenSSL (libcrypto.so, libssl.so, openssl binary)
.PHONY: ports-openssl
ports-openssl: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C ports/openssl-3.5.6 -f Makefile.likeos

$(BUILD_DIR)/openssl: ports-openssl | $(BUILD_DIR)
	@# copied by Makefile.likeos already

$(BUILD_DIR)/libcrypto.so.3: ports-openssl | $(BUILD_DIR)
	@# copied by Makefile.likeos already

$(BUILD_DIR)/libcrypto.so: ports-openssl | $(BUILD_DIR)
	@# symlink created by Makefile.likeos already

$(BUILD_DIR)/libssl.so.3: ports-openssl | $(BUILD_DIR)
	@# copied by Makefile.likeos already

$(BUILD_DIR)/libssl.so: ports-openssl | $(BUILD_DIR)
	@# symlink created by Makefile.likeos already

.PHONY: ports-zlib
ports-zlib: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C ports/lib/zlib-1.3.1 -f Makefile.likeos

$(BUILD_DIR)/libz.so.1: ports-zlib | $(BUILD_DIR)
	@# copied by Makefile.likeos already

.PHONY: ports-nghttp2
ports-nghttp2: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C ports/lib/nghttp2-1.65.0 -f Makefile.likeos

$(BUILD_DIR)/libnghttp2.so.14: ports-nghttp2 | $(BUILD_DIR)
	@# copied by Makefile.likeos already

.PHONY: ports-curl
ports-curl: userland-libc userland-rtld ports-openssl ports-zlib ports-nghttp2 | $(BUILD_DIR)
	$(MAKE) -C ports/curl-8.14.1 -f Makefile.likeos

$(BUILD_DIR)/curl: ports-curl | $(BUILD_DIR)
	@# copied by Makefile.likeos already

$(BUILD_DIR)/libcurl.so.4: ports-curl | $(BUILD_DIR)
	@# copied by Makefile.likeos already

# ---------------------------------------------------------------------------
# OpenSSH (ssh/scp/sftp/ssh-keygen + sshd server suite).  Links the LikeOS
# libc, ported OpenSSL (libcrypto/libssl) and zlib.  The port is configured
# and built by its own Makefile.likeos (see ports/openssh-10.4p1); this target
# also renders fresh host keys with the BUILD machine's ssh-keygen so the
# image ships a ready-to-run sshd.
# ---------------------------------------------------------------------------
OPENSSH_DIR   = ports/openssh-10.4p1
# /usr/bin clients + /usr/sbin daemon + /usr/libexec helpers
OPENSSH_UBIN  = ssh scp sftp ssh-keygen ssh-add ssh-agent ssh-keyscan
OPENSSH_SBIN  = sshd
OPENSSH_LIBX  = sshd-session sshd-auth sftp-server ssh-keysign \
                ssh-pkcs11-helper ssh-sk-helper
OPENSSH_ALL   = $(OPENSSH_UBIN) $(OPENSSH_SBIN) $(OPENSSH_LIBX)
OPENSSH_HOSTKEYS = ssh_host_rsa_key ssh_host_ecdsa_key ssh_host_ed25519_key

.PHONY: ports-openssh
ports-openssh: userland-libc userland-rtld ports-openssl ports-zlib | $(BUILD_DIR)
	$(MAKE) -C $(OPENSSH_DIR) -f Makefile.likeos
	mkdir -p $(BUILD_DIR)/openssh/bin $(BUILD_DIR)/openssh/etc
	for b in $(OPENSSH_ALL); do \
		cp $(OPENSSH_DIR)/$$b $(BUILD_DIR)/openssh/bin/$$b; \
		$(USER_STRIP) --strip-unneeded $(BUILD_DIR)/openssh/bin/$$b 2>/dev/null || true; \
	done
	cp $(OPENSSH_DIR)/sshd_config $(OPENSSH_DIR)/ssh_config \
	   $(OPENSSH_DIR)/moduli $(BUILD_DIR)/openssh/etc/
	# Fresh host keys, generated with the host's ssh-keygen (the target's own
	# ssh-keygen cannot run here).  -N '' = no passphrase (host keys never
	# have one); regenerated only when missing so rebuilds are stable.
	for t in rsa ecdsa ed25519; do \
		k=$(BUILD_DIR)/openssh/etc/ssh_host_$${t}_key; \
		[ -f $$k ] || ssh-keygen -q -t $$t -f $$k -N '' -C likeos-host; \
	done

# Sentinel the image build depends on (first client + the daemon + a helper).
$(BUILD_DIR)/openssh/bin/ssh: ports-openssh | $(BUILD_DIR)
	@# built and copied by ports-openssh above

# --------------------------------------------------------------------------
# X.Org: the display server, its drivers, the client libraries and a session.
#
# Nearly fifty upstream packages, so unlike the other ports this one is driven
# by scripts in ports/xorg/ rather than a Makefile.likeos: fetch.sh downloads
# and records checksums, unpack.sh extracts and applies patches/, build.sh
# builds each package in dependency order into build/xorg-sysroot.
#
# All three are idempotent -- fetch.sh skips tarballs it already has, unpack.sh
# skips trees that exist, build.sh skips packages with a stamp -- so running
# them on every build costs a few seconds and nothing else.
#
# The libraries it links against have to exist first: openssl (the server uses
# libcrypto for SHA1), zlib (libXfont2), ncurses (xterm's termcap) and curl
# (NetSurf fetches with it).
#
# Those four are ports in their own right and install nowhere near the X
# sysroot, so import-base-libs.sh copies their headers, libraries and
# pkg-config descriptions into it before anything is built against them.  It
# has to run HERE rather than by hand: `make distclean` deletes the sysroot,
# and without this step the next build got all the way to NetSurf before
# failing to find libcurl.
# --------------------------------------------------------------------------
XORG_SYSROOT = $(BUILD_DIR)/xorg-sysroot

.PHONY: ports-xorg
ports-xorg: userland-libc userland-rtld ports-openssl ports-zlib ports-ncurses \
		ports-curl | $(BUILD_DIR)
	ports/xorg/fetch.sh
	ports/xorg/unpack.sh
	ports/xorg/import-base-libs.sh
	ports/xorg/build.sh

# The GTK3 stack and Claws Mail.
#
# A sub-port of the X.Org one rather than a tree of its own: it cross-compiles
# with the same toolchain wrappers into the same sysroot, because a GTK program
# links against the very libX11 that port built.  Only the manifest differs, and
# the scripts below are four-line wrappers that say which one to read.
#
# Ordered after ports-xorg for the same reason its own manifest is ordered:
# GTK's X11 backend cannot be configured until the X libraries are installed.
.PHONY: ports-gtk3
ports-gtk3: ports-xorg | $(BUILD_DIR)
	@# Checked HERE rather than left to the package that needs it.  GLib is
	@# the fifth thing this target builds and the first to require a meson
	@# newer than Ubuntu 24.04 ships, so without this the build spends
	@# several minutes succeeding and then stops with meson's own message,
	@# which says what is wrong but not what to do about it.
	@have=$$(PATH="$$(pwd)/ports/xorg/.hosttools/bin:$$PATH" meson --version 2>/dev/null || echo 0); \
	need=$(MESON_MIN_VERSION); \
	if [ "$$(printf '%s\n%s\n' "$$need" "$$have" | sort -V | head -1)" != "$$need" ]; then \
		echo "ERROR: meson $$have is too old; GLib requires >= $$need."; \
		echo "  The apt package is not new enough on this distribution, and"; \
		echo "  installing one system-wide is refused from Ubuntu 24.04 on."; \
		echo "  This installs a current meson into the port's own venv,"; \
		echo "  without root and without touching anything outside the tree:"; \
		echo "      make deps"; \
		exit 1; \
	fi
	ports/xorg/gtk3/fetch.sh
	ports/xorg/gtk3/unpack.sh
	ports/xorg/gtk3/build.sh

# The C++ runtime's own test program.
#
# Built with the port's C++ driver rather than the userland one, because it is
# the only thing in user/bin that is C++ and because it has to link against the
# libstdc++ in the port sysroot -- which is where the C++ runtime for this
# system lives, and which does not exist until the GTK3 port has built it.
#
# Built as part of the image, and ordered AFTER the GTK3 sentinel rather than
# merely needing the same port: the two are independent entries in the image's
# prerequisite list, so under -j make is free to start this one first, and it
# would then fail on a libstdc++ that was still being built.
$(BUILD_DIR)/testcxx: user/bin/tests/testcxx.cpp $(GTK3_SENTINEL) | $(BUILD_DIR)
	@test -f $(XORG_SYSROOT)/usr/lib/libstdc++.so || { \
		echo "ERROR: no libstdc++ in $(XORG_SYSROOT)."; \
		echo "  The C++ runtime is built by the GTK3 port:"; \
		echo "      make ports-gtk3"; \
		exit 1; \
	}
	ports/xorg/toolchain/likeos-c++ -o $@ $<

# Sentinel the image build depends on.  The server is the last thing that can
# be produced without every library beneath it, so its presence means the whole
# stack built.
#
# The recipe CHECKS rather than assuming.  A rule whose recipe does not create
# its target is a rule make cannot verify: it runs the prerequisite, runs the
# (empty) recipe, and carries on regardless -- which is how a missing sysroot
# turned into an image without an X server and only a warning to show for it,
# thirty seconds before the image was written.
$(XORG_SYSROOT)/usr/bin/Xorg: ports-xorg | $(BUILD_DIR)
	@test -x $@ || { \
		echo "ERROR: $@ is missing after building the X.Org port."; \
		echo "  The per-package stamps in ports/xorg/.stamps say the"; \
		echo "  packages are built, but the sysroot they install into"; \
		echo "  is not there.  Rebuild it with:"; \
		echo "      ports/xorg/clean.sh && make"; \
		exit 1; \
	}

# The same sentinel for the GTK3 stack.  PCManFM is the last package its
# manifest builds and sits on top of nearly everything below it, so its presence
# means the whole stack -- GLib, GTK, Cairo, Pango, the C++ runtime, libetpan,
# GtkSourceView, libfm and menu-cache -- is installed in the sysroot.
#
# Checks rather than assumes, for the reason given above: without a recipe that
# verifies its own target, a rule reports success for a file that was never
# created and the missing program is not noticed until the image is booted.
$(GTK3_SENTINEL): ports-gtk3 | $(BUILD_DIR)
	@test -x $@ || { \
		echo "ERROR: $@ is missing after building the GTK3 port."; \
		echo "  The per-package stamps in ports/xorg/gtk3/.stamps say"; \
		echo "  the packages are built, but the sysroot they install"; \
		echo "  into is not there.  Rebuild it with:"; \
		echo "      ports/xorg/gtk3/clean.sh && make"; \
		exit 1; \
	}


$(KERNEL_ELF): $(KERNEL_OBJS) kernel.lds | $(BUILD_DIR)
	@echo "Building LikeOS-64 kernel as ELF64..."
	$(LD) $(KERNEL_LDFLAGS) -T kernel.lds $(KERNEL_OBJS) -o $(KERNEL_ELF)
ifndef NO_STRIP
	$(STRIP) $(KERNEL_ELF)
endif
	@echo "LikeOS-64 ELF64 kernel built: $(KERNEL_ELF)"

# Build UEFI bootloader
$(BOOTLOADER_EFI): $(BOOT_DIR)/bootloader.c $(BOOT_DIR)/boot_stack_chk.c $(BOOT_DIR)/trampoline.S $(SCREEN_STAMP) | $(BUILD_DIR)
	@echo "Building UEFI bootloader..."
	# Compile bootloader C code
	$(GCC) $(UEFI_CFLAGS) -c $(BOOT_DIR)/bootloader.c -o $(BUILD_DIR)/bootloader.o
	
	# Compile stack canary support
	$(GCC) $(UEFI_CFLAGS) -c $(BOOT_DIR)/boot_stack_chk.c -o $(BUILD_DIR)/boot_stack_chk.o
	
	# Assemble trampoline code
	$(GCC) $(UEFI_CFLAGS) -c $(BOOT_DIR)/trampoline.S -o $(BUILD_DIR)/trampoline.o
	
	# Link as shared object
	$(LD) $(UEFI_LDFLAGS) $(EFI_LIBS) $(BUILD_DIR)/bootloader.o $(BUILD_DIR)/boot_stack_chk.o $(BUILD_DIR)/trampoline.o \
		-o $(BUILD_DIR)/bootloader.so \
		/usr/lib/libgnuefi.a /usr/lib/libefi.a
	
	# Convert to EFI executable
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-x86_64 $(BUILD_DIR)/bootloader.so $(BOOTLOADER_EFI)
	@echo "UEFI bootloader built: $(BOOTLOADER_EFI)"

# ---------------------------------------------------------------------------
# Single ext4 GPT USB disk: FAT ESP (bootloader only) + ext4 root (everything).
# The ext4 root staging tree is assembled directly from the build artifacts
# (no intermediate FAT image), then populated with `mkfs.ext4 -d` under
# fakeroot (root-owned).  The ext4 partition is auto-sized to the staged
# content so the image stays small and builds fast; override with EXT4_MB=.
# ---------------------------------------------------------------------------
$(GPT_DISK): $(BOOTLOADER_EFI) $(KERNEL_ELF) $(GPT_PREREQS) | $(BUILD_DIR)
	@echo "Assembling ext4 root staging tree from build artifacts..."
	rm -rf $(EXT4_STAGING)
	mkdir -p $(EXT4_STAGING)/boot $(EXT4_STAGING)/bin $(EXT4_STAGING)/sbin \
		$(EXT4_STAGING)/lib $(EXT4_STAGING)/usr/bin $(EXT4_STAGING)/usr/sbin \
		$(EXT4_STAGING)/usr/libexec \
		$(EXT4_STAGING)/usr/local/bin $(EXT4_STAGING)/usr/share/man/man1 \
		$(EXT4_STAGING)/usr/share/nano $(EXT4_STAGING)/res \
		$(EXT4_STAGING)/etc/ssl/certs $(EXT4_STAGING)/etc/ssh \
		$(EXT4_STAGING)/root $(EXT4_STAGING)/home \
		$(EXT4_STAGING)/var/empty $(EXT4_STAGING)/var/run \
		$(EXT4_STAGING)/var/log $(EXT4_STAGING)/tmp
	# Kernel lives ONLY at /boot/kernel.elf (the bootloader loads it from ext4)
	cp $(KERNEL_ELF) $(EXT4_STAGING)/boot/kernel.elf
	# Userland programs -> /bin
	for p in $(ROOT_BIN_PROGS); do cp $(BUILD_DIR)/$$p $(EXT4_STAGING)/bin/$$p; done
	# /bin/sh is GNU bash (bash runs in POSIX-ish sh mode when invoked
	# as "sh"); the ext4 resolver follows the symlink for exec/open.
	ln -sfn bash $(EXT4_STAGING)/bin/sh
	# System programs (init, getty) -> /sbin
	for p in $(ROOT_SBIN_PROGS); do cp $(BUILD_DIR)/$$p $(EXT4_STAGING)/sbin/$$p; done
	# setuid-root helpers: passwd (write /etc/shadow) and su (switch user).
	# Owner is root (root_owner=0:0 below), so mode 4755 = -rwsr-xr-x root.
	# Each drops or confines privilege internally and decides on getuid().
	# ping does NOT get setuid: it uses the unprivileged ICMP echo syscall.
	chmod 4755 $(EXT4_STAGING)/bin/passwd $(EXT4_STAGING)/bin/su
	# Shared libraries -> /lib
	for l in $(ROOT_LIBS); do cp $(BUILD_DIR)/$$l $(EXT4_STAGING)/lib/$$l; done
	# /usr/bin/env so "#!/usr/bin/env <interp>" shebangs resolve (real copy;
	# the exec path does not depend on symlink support)
	cp $(BUILD_DIR)/env $(EXT4_STAGING)/usr/bin/env
	# Tests and demos -> /usr/local/bin (a few are renamed)
	cp $(BUILD_DIR)/user_test.elf $(EXT4_STAGING)/usr/local/bin/tests
	cp $(BUILD_DIR)/test_libc     $(EXT4_STAGING)/usr/local/bin/testlibc
	cp $(BUILD_DIR)/hello         $(EXT4_STAGING)/usr/local/bin/hello
	cp $(BUILD_DIR)/progerr       $(EXT4_STAGING)/usr/local/bin/progerr
	cp $(BUILD_DIR)/testmem       $(EXT4_STAGING)/usr/local/bin/testmem
	cp $(BUILD_DIR)/memstat       $(EXT4_STAGING)/usr/local/bin/memstat
	cp $(BUILD_DIR)/teststress    $(EXT4_STAGING)/usr/local/bin/teststress
	cp $(BUILD_DIR)/netstress     $(EXT4_STAGING)/usr/local/bin/netstress
	cp $(BUILD_DIR)/openssltest   $(EXT4_STAGING)/usr/local/bin/openssltest
	cp $(BUILD_DIR)/usbtest       $(EXT4_STAGING)/usr/local/bin/usbtest
	cp $(BUILD_DIR)/ext4test      $(EXT4_STAGING)/usr/local/bin/ext4test
	cp $(BUILD_DIR)/permbench     $(EXT4_STAGING)/usr/local/bin/permbench
	cp $(BUILD_DIR)/fbtest        $(EXT4_STAGING)/usr/local/bin/fbtest
	cp $(BUILD_DIR)/pmap          $(EXT4_STAGING)/usr/local/bin/pmap
	cp $(BUILD_DIR)/ttydump       $(EXT4_STAGING)/usr/local/bin/ttydump
	cp $(BUILD_DIR)/cxxprobe      $(EXT4_STAGING)/usr/local/bin/cxxprobe
	# Shebang smoke-test script (mode 755 propagates via fakeroot mkfs -d)
	cp user/bin/tests/scripttest.sh $(EXT4_STAGING)/usr/local/bin/scripttest.sh
	chmod 755 $(EXT4_STAGING)/usr/local/bin/scripttest.sh
	# bash language/builtin test suite (run: testbash.sh)
	cp user/bin/tests/testbash.sh $(EXT4_STAGING)/usr/local/bin/testbash.sh
	chmod 755 $(EXT4_STAGING)/usr/local/bin/testbash.sh
	# Resources, manpages, config
	cp res/Uni2-Terminus16.psf $(EXT4_STAGING)/res/Uni2-Terminus16.psf
	cp res/left_ptr          $(EXT4_STAGING)/res/left_ptr
	cp res/man/*.1           $(EXT4_STAGING)/usr/share/man/man1/
	# X cursor theme.
	#
	# Generated, not committed: the artwork is geometry, so it lives as the
	# program that draws it (host/gen-cursors.c) rather than as a directory
	# of binary files nobody can review.  Built with the HOST compiler
	# because it runs here, at image-build time.
	#
	# Without a theme installed libXcursor falls back to the X core cursor
	# font, whose "watch" is a 1-bit wristwatch; this replaces the busy
	# cursors with an animated hourglass and leaves every other cursor to
	# the fallback, which is why the theme is under a megabyte.
	mkdir -p $(BUILD_DIR)/hosttools
	$(GCC) -O2 -o $(BUILD_DIR)/hosttools/gen-cursors host/gen-cursors.c -lm
	mkdir -p $(EXT4_STAGING)/usr/share/icons/LikeOS/cursors
	$(BUILD_DIR)/hosttools/gen-cursors \
		$(EXT4_STAGING)/usr/share/icons/LikeOS
	# "man sh" shows the bash page: sh IS bash now (see /bin/sh symlink)
	ln -sfn bash.1 $(EXT4_STAGING)/usr/share/man/man1/sh.1
	cp ports/nano-8.3/syntax/*.nanorc $(EXT4_STAGING)/usr/share/nano/
	cp /etc/services         $(EXT4_STAGING)/etc/services
	# The valid login shells.  Consulted by chsh, by daemons that refuse a
	# login whose shell is not listed, and by xterm -- which falls back to
	# /bin/sh when the file is missing, whatever the password database says.
	cp res/etc/shells        $(EXT4_STAGING)/etc/shells
	cp res/etc/hosts         $(EXT4_STAGING)/etc/hosts
	# Media types.  Claws Mail reads this to set an attachment's
	# Content-Type; without it every attachment is sent as
	# application/octet-stream and the receiving client has to guess.
	cp res/etc/mime.types    $(EXT4_STAGING)/etc/mime.types
	cp res/etc/resolv.conf   $(EXT4_STAGING)/etc/resolv.conf
	# What init (process 1) starts and supervises at boot: the console getty
	# and the services listed there.
	cp res/etc/inittab       $(EXT4_STAGING)/etc/inittab
	cp res/nanorc            $(EXT4_STAGING)/etc/nanorc
	# --- OpenSSH: clients -> /usr/bin, daemon -> /usr/sbin, helpers -> /usr/libexec
	for b in $(OPENSSH_UBIN); do cp $(BUILD_DIR)/openssh/bin/$$b $(EXT4_STAGING)/usr/bin/$$b; done
	for b in $(OPENSSH_SBIN); do cp $(BUILD_DIR)/openssh/bin/$$b $(EXT4_STAGING)/usr/sbin/$$b; done
	for b in $(OPENSSH_LIBX); do cp $(BUILD_DIR)/openssh/bin/$$b $(EXT4_STAGING)/usr/libexec/$$b; done
	# OpenSSH config + moduli + host keys (private keys mode 0600)
	cp $(BUILD_DIR)/openssh/etc/sshd_config $(EXT4_STAGING)/etc/ssh/sshd_config
	cp $(BUILD_DIR)/openssh/etc/ssh_config  $(EXT4_STAGING)/etc/ssh/ssh_config
	cp $(BUILD_DIR)/openssh/etc/moduli      $(EXT4_STAGING)/etc/ssh/moduli
	for t in rsa ecdsa ed25519; do \
		cp $(BUILD_DIR)/openssh/etc/ssh_host_$${t}_key     $(EXT4_STAGING)/etc/ssh/; \
		cp $(BUILD_DIR)/openssh/etc/ssh_host_$${t}_key.pub $(EXT4_STAGING)/etc/ssh/; \
	done
	chmod 0600 $(EXT4_STAGING)/etc/ssh/ssh_host_*_key
	chmod 0644 $(EXT4_STAGING)/etc/ssh/ssh_host_*_key.pub $(EXT4_STAGING)/etc/ssh/sshd_config $(EXT4_STAGING)/etc/ssh/ssh_config
	chmod 0700 $(EXT4_STAGING)/var/empty
	# User/group/shadow databases (root:toor preconfigured, yescrypt hash)
	cp res/etc/passwd        $(EXT4_STAGING)/etc/passwd
	cp res/etc/group         $(EXT4_STAGING)/etc/group
	cp res/etc/shadow        $(EXT4_STAGING)/etc/shadow
	chmod 0644 $(EXT4_STAGING)/etc/passwd $(EXT4_STAGING)/etc/group
	chmod 0600 $(EXT4_STAGING)/etc/shadow
	# Shell startup files.  /etc/profile runs for login shells and sources
	# /etc/bash.bashrc; bash reads /etc/bash.bashrc directly for interactive
	# non-login shells (built with SYS_BASHRC).  /etc/skel is copied into
	# each new home by adduser, so new users get the same prompt.
	cp res/etc/profile       $(EXT4_STAGING)/etc/profile
	cp res/etc/bash.bashrc   $(EXT4_STAGING)/etc/bash.bashrc
	cp res/etc/root.profile  $(EXT4_STAGING)/root/.profile
	cp res/etc/root.bashrc   $(EXT4_STAGING)/root/.bashrc
	mkdir -p $(EXT4_STAGING)/etc/skel
	cp res/etc/skel/.profile $(EXT4_STAGING)/etc/skel/.profile
	cp res/etc/skel/.bashrc  $(EXT4_STAGING)/etc/skel/.bashrc
	# Desktop shortcuts.  Into /etc/skel/Desktop so every new account gets
	# them (adduser copies the skeleton recursively), and into /root/Desktop
	# as well because root already exists and is never created by adduser.
	# They are .desktop launchers for programs this image actually installs;
	# each Icon= name resolves to a PNG in the staged Adwaita/hicolor themes,
	# which matters because there is no librsvg here and the SVG variants
	# would not render.
	mkdir -p $(EXT4_STAGING)/etc/skel/Desktop $(EXT4_STAGING)/root/Desktop
	cp res/etc/skel/Desktop/*.desktop $(EXT4_STAGING)/etc/skel/Desktop/
	cp res/etc/skel/Desktop/*.desktop $(EXT4_STAGING)/root/Desktop/
	# The icons appear in modification-time order, oldest first: PCManFM's
	# desktop defaults to sorting by mtime (desktop_sort_by, app-config.c),
	# not by name.  cp gives every file the same timestamp, so the tie broke
	# on name and the order was alphabetical -- which put a newly added
	# Calculator at the very top.  Stamping them one second apart in the
	# order below is what actually decides the layout, so it is written here
	# rather than left to fall out of the filenames.
	i=0; for f in pcmanfm xterm mousepad xnedit netsurf claws-mail xcalc; do \
		for d in $(EXT4_STAGING)/etc/skel/Desktop \
		         $(EXT4_STAGING)/root/Desktop; do \
			[ -f "$$d/$$f.desktop" ] && \
				touch -d "2026-01-01 00:00:$$i UTC" \
				      "$$d/$$f.desktop"; \
		done; \
		i=$$((i+1)); \
	done; true
	chmod 0644 $(EXT4_STAGING)/etc/skel/Desktop/*.desktop \
	           $(EXT4_STAGING)/root/Desktop/*.desktop
	chmod 0700 $(EXT4_STAGING)/root
	# X11 puts its per-display listening socket at /tmp/.X11-unix/X<n>.  The
	# directory must exist before the server binds, and carries the same
	# world-writable + sticky mode as /tmp itself.
	mkdir -p $(EXT4_STAGING)/tmp/.X11-unix
	chmod 1777 $(EXT4_STAGING)/tmp/.X11-unix
	# World-writable + sticky /tmp (1777, like every Unix): lets any user create
	# their own /tmp/tmux-<uid> socket dir (tmux then makes it 0700); the sticky
	# bit keeps users from deleting each other's files.
	chmod 1777 $(EXT4_STAGING)/tmp
	# --- X.Org: server, drivers, libraries, keymaps, fonts, config.
	# Staged by a script rather than inline: it is a few hundred files
	# picked out of a build sysroot that also holds static archives,
	# headers and host tools, and the selection needs explaining.
	# ports-xorg is a prerequisite of the image, so the sysroot is normally
	# there.  The guard is for the case where it has been removed by hand
	# between the build and the staging step: an image without an X server is
	# a better outcome than a staging failure, as long as it says so.
	@# Refuses rather than warning: an image that silently has no X server
	@# is the failure this whole chain exists to prevent, and a warning in
	@# the middle of a long build is not seen.
	ports/xorg/stage.sh $(EXT4_STAGING)
	# --- GTK3 stack and Claws Mail, staged the same way and for the same
	# reasons.  Every item inside is guarded on having been built, so this
	# is a no-op until the port has produced something: an image without
	# GTK3 is a working image, one that half-contains it is not.
	ports/xorg/gtk3/stage.sh $(EXT4_STAGING)
	cp ports/openssl-3.5.6/apps/openssl.cnf $(EXT4_STAGING)/etc/ssl/openssl.cnf
	cp res/etc/ssl/certs/ca-certificates.crt $(EXT4_STAGING)/etc/ssl/certs/ca-certificates.crt
	# Signature file selects this device as the OS root; sample text file
	echo "THIS IS A DEVICE STORING LIKEOS" > $(EXT4_STAGING)/LIKEOS.SIG
	echo "Hello from USB mass storage" > $(EXT4_STAGING)/HELLO.TXT
	@set -e; \
	if [ -n "$(EXT4_MB)" ]; then ext4_mb=$(EXT4_MB); \
	else \
	  staged_kb=$$(du -sk $(EXT4_STAGING) | cut -f1); \
	  ext4_mb=$$(( staged_kb / 1024 * 3 / 2 + 32 )); \
	  if [ $$ext4_mb -lt 64 ]; then ext4_mb=64; fi; \
	fi; \
	echo "Building ext4 root: $${ext4_mb}M (staged $$(du -sh $(EXT4_STAGING) | cut -f1))"; \
	rm -f $(EXT4_ROOT_IMG); \
	feat="$(EXT4_MKFS_FEATURES)"; \
	: "resize=536870912 (4k blocks = 2TB) sizes the resize_inode so an offline"; \
	: "resize2fs can grow this small image to fill a large drive; the default"; \
	: "headroom (~1024x = ~64GB) is too small for >64GB drives and aborts with"; \
	: "an invalid-double-indirect-block error."; \
	fakeroot mkfs.ext4 -F -q -b 4096 $${feat:+-O $$feat} -E root_owner=0:0,resize=536870912 \
		-d $(EXT4_STAGING) $(EXT4_ROOT_IMG) $${ext4_mb}M; \
	e2fsck -fn $(EXT4_ROOT_IMG) || true; \
	rm -f $(EXT4_ESP_IMG); \
	truncate -s $(ESP_MB)M $(EXT4_ESP_IMG); \
	$(MKFS_FAT) -F32 -n LIKEOSESP $(EXT4_ESP_IMG) >/dev/null; \
	MTOOLS_SKIP_CHECK=1 mmd -i $(EXT4_ESP_IMG) ::/EFI ::/EFI/BOOT; \
	MTOOLS_SKIP_CHECK=1 mcopy -i $(EXT4_ESP_IMG) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI; \
	rm -f $(GPT_DISK); \
	truncate -s $$(( $(ESP_MB) + ext4_mb + 2 ))M $(GPT_DISK); \
	sgdisk -Z $(GPT_DISK) >/dev/null 2>&1; \
	sgdisk -a 2048 -n 1:2048:+$(ESP_MB)M -t 1:EF00 -c 1:"ESP"         $(GPT_DISK) >/dev/null; \
	sgdisk         -n 2:0:+$${ext4_mb}M  -t 2:8300 -c 2:"likeos-root" $(GPT_DISK) >/dev/null; \
	$(DD) if=$(EXT4_ESP_IMG)  of=$(GPT_DISK) bs=512 seek=2048 conv=sparse,notrunc status=none; \
	$(DD) if=$(EXT4_ROOT_IMG) of=$(GPT_DISK) bs=512 seek=$$(( 2048 + $(ESP_MB) * 2048 )) conv=sparse,notrunc status=none; \
	echo "ext4 GPT disk created: $(GPT_DISK)  (ESP=$(ESP_MB)M ext4=$${ext4_mb}M)"

# Run the single ext4 GPT USB disk in QEMU (UEFI).  The firmware runs the ESP's
# bootloader, which then loads /boot/kernel.elf from the ext4 root partition.
qemu: $(GPT_DISK)
	@echo "Running LikeOS-64 from the ext4 GPT USB disk in QEMU..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
		-machine type=pc,accel=kvm:tcg -device qemu-xhci,id=xhci \
		-drive if=none,id=ext4disk,file=$(GPT_DISK),format=raw,readonly=off \
		-device usb-storage,drive=ext4disk,bootindex=0 $(QEMU_USB_HID) \
		-device $(NIC_DEVICE),netdev=net0 -netdev user,id=net0

# Boot the single ext4 GPT USB disk with full SLIRP networking (UEFI).
#
# Optional NIC selection: override the QEMU `-device` model with NIC_DEVICE,
# e.g. `make qemu-usb NIC_DEVICE=e1000e` or `NIC_DEVICE=rtl8139`.
# Defaults to e1000 (Intel 82540EM).
NIC_DEVICE ?= e1000

qemu-usb: $(GPT_DISK)
	@echo "Running LikeOS-64 from the ext4 GPT USB disk + $(NIC_DEVICE) networking..."
	@# sudo is required so SLIRP can open a raw ICMP socket on the host;
	@# without it, external `ping` (e.g. ping 8.8.8.8) is silently dropped
	@# while the synthetic gateway reply (10.0.2.2) still works.
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
		-machine type=pc,accel=kvm:tcg -device qemu-xhci,id=xhci \
		-drive if=none,id=ext4disk,file=$(GPT_DISK),format=raw,readonly=off \
		-device usb-storage,drive=ext4disk,bootindex=0 $(QEMU_USB_HID) \
		-device $(NIC_DEVICE),netdev=net0 -netdev user,id=net0

# Boot the ext4 GPT USB disk with GDB support and debug symbols.
# Connect with: gdb build/kernel.elf -ex "target remote :1234"
qemu-usb-gdb:
	@echo "Rebuilding kernel with debug symbols (-g)..."
	$(MAKE) clean
	$(MAKE) KERNEL_CFLAGS="$(KERNEL_CFLAGS) -g" NO_STRIP=1 $(GPT_DISK)
	@echo "Running LikeOS-64 from the ext4 GPT USB disk + $(NIC_DEVICE) + GDB server on :1234..."
	@echo "Connect with: gdb build/kernel.elf -ex 'target remote :1234'"
	@# sudo: see qemu-usb target above (SLIRP raw ICMP socket).
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
		-machine type=pc,accel=kvm:tcg -device qemu-xhci,id=xhci \
		-drive if=none,id=ext4disk,file=$(GPT_DISK),format=raw,readonly=off \
		-device usb-storage,drive=ext4disk,bootindex=0 $(QEMU_USB_HID) \
		-device $(NIC_DEVICE),netdev=net0 -netdev user,id=net0 \
		-s -S -monitor telnet:127.0.0.1:5555,server,nowait -d int 2> /tmp/qemu_int_log

# Run with real USB device (like /dev/sdb) as xHCI USB mass storage - boots from USB only
# Usage: make qemu-realusb USB_DEVICE=/dev/sdb [SERIAL=1] [NIC_DEVICE=e1000e]
qemu-realusb:
ifndef USB_DEVICE
	$(error USB_DEVICE is not set. Usage: make qemu-realusb USB_DEVICE=/dev/sdb)
endif
	@echo "Running LikeOS-64 in QEMU booting from xHCI USB device $(USB_DEVICE) ($(NIC_DEVICE) NIC)..."
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file=$(USB_DEVICE) \
		-device usb-storage,bus=xhci.0,drive=stick,bootindex=1 -machine type=pc,accel=kvm:tcg \
		-device $(NIC_DEVICE),netdev=net0 -netdev user,id=net0

# Run with real USB device as xHCI USB mass storage, with GDB support and debug symbols
# Usage: make qemu-realusb-gdb USB_DEVICE=/dev/sdb [SERIAL=1] [NIC_DEVICE=e1000e]
# Connect with: gdb build/kernel.elf -ex "target remote :1234"
qemu-realusb-gdb:
ifndef USB_DEVICE
	$(error USB_DEVICE is not set. Usage: make qemu-realusb-gdb USB_DEVICE=/dev/sdb)
endif
	@echo "Rebuilding kernel with debug symbols (-g)..."
	$(MAKE) clean
	$(MAKE) KERNEL_CFLAGS="$(KERNEL_CFLAGS) -g" NO_STRIP=1 usb-write USB_DEVICE=$(USB_DEVICE)
	@echo "Running LikeOS-64 in QEMU booting from xHCI USB device $(USB_DEVICE) ($(NIC_DEVICE) NIC) + GDB server on :1234..."
	@echo "Connect with: gdb build/kernel.elf -ex 'target remote :1234'"
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file=$(USB_DEVICE) \
		-device usb-storage,bus=xhci.0,drive=stick,bootindex=1 -machine type=pc,accel=kvm:tcg \
		-device $(NIC_DEVICE),netdev=net0 -netdev user,id=net0 \
		-s -S -monitor telnet:127.0.0.1:5555,server,nowait -d int 2> /tmp/qemu_int_log

# Extended USB passthrough target: attach tablet + optional host devices (edit vendor/product)
qemu-usb-passthrough: $(GPT_DISK)
	@echo "Running LikeOS-64 from the ext4 GPT USB disk with host USB passthrough (if any)..."
	@echo "Autodetecting host USB devices via lsusb (override with PASSTHROUGH_FILTER=vid:pid,vid:pid)."
	@set -e; \
	devices=""; \
	if [ -n "$$PASSTHROUGH_FILTER" ]; then \
	  OLDIFS="$$IFS"; IFS=','; set -- $$PASSTHROUGH_FILTER; IFS="$$OLDIFS"; \
	  for spec in "$$@"; do \
	    vid=$${spec%%:*}; pid=$${spec##*:}; \
	    if [ -n "$$vid" ] && [ -n "$$pid" ]; then \
	      devices="$$devices -device usb-host,vendorid=0x$${vid},productid=0x$${pid}"; \
	    fi; \
	  done; \
	else \
	  devices=$$(lsusb 2>/dev/null | awk '!/root hub/ && !/Linux Foundation/ { id=$$6; split(id,a,":"); if(length(a[1])==4 && length(a[2])==4) printf(" -device usb-host,vendorid=0x%s,productid=0x%s", a[1], a[2]); }'); \
	fi; \
	if [ -z "$$devices" ]; then echo "(No host USB devices selected for passthrough. Set PASSTHROUGH_FILTER=vid:pid to choose explicitly.)"; fi; \
	echo "Passing through devices:$$devices"; \
	set -x; \
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd $(QEMU_VGA) -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) -machine q35 -device qemu-xhci,id=xhci -device usb-tablet -drive if=none,id=ext4disk,file=$(GPT_DISK),format=raw,readonly=off -device usb-storage,drive=ext4disk,bootindex=0 $$devices || echo "QEMU exited with status $$?"; \
	set +x || true

# Write the single ext4 GPT USB disk to a real USB device (like Rufus, but the
# whole GPT layout — FAT ESP + ext4 root — is just dd'd straight over).
# Usage: make usb-write USB_DEVICE=/dev/sdX
usb-write: $(GPT_DISK)
	@if [ -z "$(USB_DEVICE)" ]; then \
		echo "Error: USB_DEVICE not specified. Usage: make usb-write USB_DEVICE=/dev/sdX"; \
		echo "Available devices:"; \
		lsblk -d -o NAME,SIZE,TYPE | grep disk; \
		exit 1; \
	fi
	@echo "WARNING: This will completely erase $(USB_DEVICE) and write the ext4 GPT disk image!"
	@echo "Press Enter to continue or Ctrl+C to cancel..."
	@read confirm
	@echo "Unmounting any mounted partitions on $(USB_DEVICE)..."
	-sudo umount $(USB_DEVICE)* 2>/dev/null || true
	@echo "Writing $(GPT_DISK) to $(USB_DEVICE) (this may take a while)..."
	sudo dd if=$(GPT_DISK) of=$(USB_DEVICE) bs=4M conv=fsync status=progress
	sudo sync
	@# The image is small (auto-sized to the build).  Grow the ext4 root partition
	@# and filesystem to fill the WHOLE target drive, so the running system has the
	@# drive's full capacity (and contiguous free space -> fast writes), not ~64MB.
	@echo "Growing the ext4 root partition + filesystem to fill $(USB_DEVICE)..."
	@PART=$$(case "$(USB_DEVICE)" in *[0-9]) echo "$(USB_DEVICE)p2";; *) echo "$(USB_DEVICE)2";; esac); \
	 sudo umount $$PART 2>/dev/null || true; \
	 START=$$(sudo sgdisk -i 2 $(USB_DEVICE) | awk '/First sector/{print $$3}'); \
	 if [ -z "$$START" ]; then echo "ERROR: could not read partition 2 start; skipping grow"; else \
	   sudo sgdisk -e $(USB_DEVICE) >/dev/null; \
	   sudo sgdisk -d 2 $(USB_DEVICE) >/dev/null; \
	   sudo sgdisk -a 1 -n 2:$$START:0 -t 2:8300 -c 2:"likeos-root" $(USB_DEVICE) >/dev/null; \
	   DISKSZ=$$(sudo blockdev --getsize64 $(USB_DEVICE)); ok=0; PSZ=0; \
	   for n in 1 2 3 4 5 6; do \
	     sudo partprobe $(USB_DEVICE) 2>/dev/null || true; \
	     sudo blockdev --rereadpt $(USB_DEVICE) 2>/dev/null || true; \
	     sudo partx -u $(USB_DEVICE) 2>/dev/null || true; \
	     command -v udevadm >/dev/null 2>&1 && sudo udevadm settle 2>/dev/null || true; \
	     sleep 1; \
	     PSZ=$$(sudo blockdev --getsize64 $$PART 2>/dev/null || echo 0); \
	     if [ "$$PSZ" -gt $$((DISKSZ/2)) ]; then ok=1; break; fi; \
	   done; \
	   if [ "$$ok" != 1 ]; then \
	     echo "WARNING: the kernel did not re-read the new size of $$PART (still $$PSZ bytes)."; \
	     echo "         Unplug + replug the USB, then run:  sudo e2fsck -fy $$PART && sudo resize2fs $$PART"; \
	   else \
	     sudo e2fsck -fy $$PART || true; \
	     if sudo resize2fs $$PART; then \
	       sudo sync; \
	       echo "Grown: $$PART now fills the drive ($$(sudo blockdev --getsize64 $(USB_DEVICE) | awk '{printf "%.1f GiB", $$1/1073741824}'))."; \
	     else \
	       echo "ERROR: resize2fs failed — the filesystem was NOT grown."; \
	       echo "       Repair, then retry:  sudo e2fsck -fy $$PART && sudo resize2fs $$PART"; \
	     fi; \
	   fi; \
	 fi
	@echo "UEFI bootable USB drive created successfully on $(USB_DEVICE)"
	@echo "The USB drive boots on UEFI systems: ESP bootloader -> ext4 /boot/kernel.elf."

# Build a minimal Linux host that auto-starts LikeOS under QEMU/KVM
linux-usb: $(GPT_DISK)
	@echo "Building Linux host USB image with seamless LikeOS handoff..."
	$(LINUX_USB_DIR)/create-rootfs.sh

# Write the host Linux image to a USB device
# Usage: make linux-usb-write USB_DEVICE=/dev/sdX
linux-usb-write: linux-usb
	@if [ -z "$(USB_DEVICE)" ]; then \
		echo "Error: USB_DEVICE not specified. Usage: make linux-usb-write USB_DEVICE=/dev/sdX"; \
		echo "Available devices:"; \
		lsblk -d -o NAME,SIZE,TYPE | grep disk; \
		exit 1; \
	fi
	@echo "WARNING: This will overwrite $(USB_DEVICE) with the Linux host image!"
	@echo "Press Enter to continue or Ctrl+C to cancel..."
	@read confirm
	@echo "Writing $(LINUX_USB_IMAGE) to $(USB_DEVICE) (this may take a while)..."
	@sudo dd if=$(LINUX_USB_IMAGE) of=$(USB_DEVICE) bs=4M status=progress oflag=sync
	@sync
	@echo "Linux host USB written to $(USB_DEVICE). The stick will boot straight into X11 and launch LikeOS inside QEMU/KVM."

# Clean build files
clean:
	@# Everything in build/ EXCEPT the X.Org sysroot.
	@#
	@# build/xorg-sysroot is the X port's build state, not one of our
	@# outputs -- it is where the 51 packages install to, and it is the
	@# counterpart of the object files that `clean` leaves alone inside
	@# ports/openssh-10.4p1 and the others.  Deleting it here while
	@# ports/xorg/.stamps survived meant build.sh skipped every package as
	@# "already done" and never recreated it, so `make clean && make`
	@# produced an image with no X server at all.
	@#
	@# `make distclean` removes it, through ports/xorg/clean.sh.
	@if [ -d $(BUILD_DIR) ]; then \
		find $(BUILD_DIR) -mindepth 1 -maxdepth 1 \
			! -name xorg-sysroot -exec rm -rf {} + ; \
	fi
	$(MAKE) -C user/lib/libc clean
	$(MAKE) -C user/lib/rtld clean
	$(MAKE) -C user/lib/testlib clean
	$(MAKE) -C user/lib/libcrypt clean
	$(MAKE) -C user/lib/libpam clean
	$(MAKE) -C $(USER_DIR) clean

distclean: clean
	$(MAKE) -C ports/lib/ncurses-likeos clean
	$(MAKE) -C ports/lib/libevent-2.1.12 -f Makefile.likeos clean
	$(MAKE) -C ports/lib/zlib-1.3.1 -f Makefile.likeos clean
	$(MAKE) -C ports/lib/nghttp2-1.65.0 -f Makefile.likeos clean
	$(MAKE) -C ports/nano-8.3 -f Makefile.likeos clean
	$(MAKE) -C ports/bash-5.2.37 -f Makefile.likeos distclean
	$(MAKE) -C ports/tmux-3.6a -f Makefile.likeos clean
	$(MAKE) -C ports/netcat-OpenBSD -f Makefile.likeos clean
	$(MAKE) -C ports/openssl-3.5.6 -f Makefile.likeos clean
	$(MAKE) -C ports/curl-8.14.1 -f Makefile.likeos clean
	$(MAKE) -C ports/openssh-10.4p1 -f Makefile.likeos distclean
	rm -rf $(BUILD_DIR)/openssh
	# gdb: distclean, not clean.  Its src/ is an unpacked, patched tarball and
	# its build/ is a separate out-of-tree configure -- 940M between them, none
	# of it tracked, all of it reproducible from the tarball and patches/ that
	# stay.  The port's own `clean' only runs the configure-generated clean,
	# which would leave both trees in place.
	$(MAKE) -C ports/gdb-17.2 -f Makefile.likeos distclean
	# X.Org: the source trees are unpacked tarballs rather than checked-in
	# sources, so cleaning deletes them.  The tarballs themselves are kept --
	# they ARE this port's sources, and re-downloading forty-seven of them
	# would make `make distclean && make` need a network connection.
	# ports/xorg/clean.sh -a removes those too.
	ports/xorg/clean.sh

# Regenerate res/man/bash.1 from the port's troff source.  man(1) reads
# PRE-FORMATTED (catman) text, not troff, so the upstream doc/bash.1 has to be
# rendered to 78-column plain text first.  Deliberately NOT part of the normal
# build: the rendered page is checked in, and groff is not a build dependency.
.PHONY: bash-manpage
bash-manpage:
	GROFF_NO_SGR=1 groff -t -e -mandoc -Tascii -rLL=78n -rLT=78n \
		ports/bash-5.2.37/doc/bash.1 | col -bx > res/man/bash.1
	@echo "res/man/bash.1 regenerated ($$(wc -l < res/man/bash.1) lines)"

# gdb's manual comes from its own source tree, like every other ported
# program's.  Man 1 only: the debugger's full manual is Texinfo, which nothing
# on the image can read, and this system's manual is a flat directory of
# preformatted pages.  Requires ./unpack.sh in the port first.
.PHONY: gdb-manpage
gdb-manpage:
	GROFF_NO_SGR=1 groff -t -e -mandoc -Tascii -rLL=78n -rLT=78n \
		ports/gdb-17.2/src/gdb/doc/gdb.1 | col -bx > res/man/gdb.1
	@echo "res/man/gdb.1 regenerated ($$(wc -l < res/man/gdb.1) lines)"
	GROFF_NO_SGR=1 groff -t -e -mandoc -Tascii -rLL=78n -rLT=78n \
		ports/gdb-17.2/src/gdb/doc/gdbserver.1 | col -bx \
		> res/man/gdbserver.1
	@echo "res/man/gdbserver.1 regenerated ($$(wc -l < res/man/gdbserver.1) lines)"

# Regenerate the OpenSSH catman pages in res/man from the port's mdoc sources.
# Like bash-manpage this is a maintenance target, not part of the build: the
# rendered pages are checked in and groff is not a build dependency.  Every
# page is stored as NAME.1 because LikeOS keeps a single flat man1 catman dir.
# The footer of an mdoc page names an operating system.  Pages that leave .Os
# empty get groff's compiled-in default, which is the identity of whatever
# machine rendered them -- so pages built here came out stamped with the build
# host's distribution rather than this system's name.  That default is not
# settable per run (mdoc overwrites a pre-set string when its macros load), so
# it is detected once and rewritten in the output.  The replacement is padded to
# the original width to keep the footer's column alignment.
MANPAGE_HOST_OS = $(shell printf '.Dd x\n.Dt T 1\n.Os\n' \
	| GROFF_NO_SGR=1 groff -Tascii -mandoc 2>/dev/null | tail -1 | awk '{print $$1}')
MANPAGE_FIXUP = awk -v old="$(MANPAGE_HOST_OS)" -v new=LikeOS \
	'BEGIN { if (old == "") old = "\001" ; n = length(old); \
	         while (length(new) < n) new = new " "; new = substr(new, 1, n) } \
	 { if (index($$0, old) == 1) sub(/^[^ ]*/, new); print }'

.PHONY: openssh-manpages
openssh-manpages:
	@for m in ssh.1 scp.1 sftp.1 ssh-keygen.1 ssh-add.1 ssh-agent.1 \
		 ssh-keyscan.1 sshd.8 sftp-server.8 ssh-keysign.8 ssh-sk-helper.8 \
		 ssh-pkcs11-helper.8 ssh_config.5 sshd_config.5 moduli.5; do \
		base=$$(echo $$m | sed 's/\.[0-9]$$//'); \
		GROFF_NO_SGR=1 groff -Tascii -mandoc ports/openssh-10.4p1/$$m \
			| col -bx | $(MANPAGE_FIXUP) > res/man/$$base.1; \
		echo "  res/man/$$base.1 ($$(wc -l < res/man/$$base.1) lines)"; \
	done

# Render the X.Org manual pages, the same way openssh-manpages above does.
#
# Maintenance target, not part of the build: the rendered pages are checked in
# and groff is not a build dependency.  Sources are taken from the built
# sysroot rather than the source trees, because several are generated at build
# time from .man templates with paths substituted in -- rendering the template
# would ship a page full of unexpanded @variables@.
#
# ONLY section 1: programs someone can actually run.  Everything else is left
# out on purpose -- section 3 is 3631 pages of Xlib function reference (22 MB,
# for writing X clients, not for using this system), section 4 documents driver
# options and section 5 file formats.  This system's manual is a flat man1
# catman directory, and putting a file-format page in it produces entries like
# "Compose(5)" sitting among the commands.
#
# A few section-1 pages are skipped by name because their programs are build
# host tooling and are not staged onto the image; a manual page for a command
# that does not exist is worse than no page.
#
# Some packages install their page already gzipped, so the source is piped
# through zcat where needed rather than handed to groff by name.
#
# groff runs FROM the man root with a relative path, because some pages are a
# single .so line (an include of another page) resolved relative to the working
# directory.  A few spell it without the directory, so those get a second
# attempt from the page's own directory; between the two forms every page
# renders.
XORG_MAN_SKIP = bdftruncate ucs2any libevdev-tweak-device mouse-dpi-tool \
		touchpad-edge-detector koi8rxterm gtf

.PHONY: xorg-manpages
xorg-manpages:
	@set -e; \
	sysroot=$$(pwd)/$(BUILD_DIR)/xorg-sysroot; \
	if [ ! -d $$sysroot/usr/share/man ]; then \
		echo "no X.Org sysroot; run ports/xorg/build.sh first"; exit 1; \
	fi; \
	out=$$(cd res/man && pwd); \
	cd $$sysroot/usr/share/man; \
	n=0; \
	for m in $$(find man1 -type f \( -name '*.[0-9]' -o -name '*.[0-9].gz' \) \
			2>/dev/null | sort); do \
		base=$$(basename $$m); \
		base=$$(echo $$base | sed 's/\.gz$$//; s/\.[0-9]$$//'); \
		skip=0; \
		for x in $(XORG_MAN_SKIP); do \
			[ "$$base" = "$$x" ] && skip=1; \
		done; \
		[ $$skip = 1 ] && continue; \
		case $$m in \
		*.gz) cat="zcat" ;; \
		*)    cat="cat"  ;; \
		esac; \
		$$cat $$m 2>/dev/null | GROFF_NO_SGR=1 groff -Tascii -mandoc - \
			2>/dev/null | col -bx | $(MANPAGE_FIXUP) > $$out/$$base.1; \
		if [ ! -s $$out/$$base.1 ]; then \
			( cd $$(dirname $$m) && $$cat $$(basename $$m) \
				2>/dev/null | GROFF_NO_SGR=1 groff -Tascii \
				-mandoc - 2>/dev/null ) \
				| col -bx > $$out/$$base.1; \
		fi; \
		if [ -s $$out/$$base.1 ]; then \
			n=$$((n + 1)); \
		else \
			rm -f $$out/$$base.1; \
			echo "  SKIP $$base (rendered empty)"; \
		fi; \
	done; \
	echo "rendered $$n X.Org manual pages into res/man/"

# Render the manual pages of the GTK3 port, into the same catman directory.
#
# By SHIPPED BINARY rather than by what the sysroot contains, which is the
# opposite of xorg-manpages above and deliberate.  That target walks the
# sysroot's man1 and names the exceptions to skip; here the exceptions would be
# the rule -- building this stack leaves about fifty section 1 pages behind for
# the tiff, jpeg, pcre2, gettext and enchant command-line tools, none of which
# are staged.  A manual page for a command that does not exist is worse than no
# page, so the list below is exactly the programs gtk3/stage.sh installs.
#
# Most of them have no page at all: fontconfig and GLib are built with their
# documentation off, since it needs a toolchain whose output nothing here
# reads.  claws-mail, pcmanfm, libfm-pref-apps, lxshortcut and mousepad render;
# the rest are listed so that adding one to the image adds its page without
# anyone having to remember.
#
# mousepad's page is the one that did not come from its own tarball -- upstream
# ships none -- so the port supplies it (ports/xorg/gtk3/man/mousepad.1) and
# the build driver installs it into the sysroot's man1.  From here it is
# indistinguishable from the others, which is the point.
GTK3_MAN_PROGS = claws-mail pcmanfm mousepad libfm-pref-apps lxshortcut \
		 gio gsettings gdbus gapplication \
		 fc-list fc-match fc-cache

.PHONY: gtk3-manpages
gtk3-manpages:
	@set -e; \
	sysroot=$$(pwd)/$(BUILD_DIR)/xorg-sysroot; \
	if [ ! -d $$sysroot/usr/share/man/man1 ]; then \
		echo "no sysroot; run make ports-gtk3 first"; exit 1; \
	fi; \
	out=$$(cd res/man && pwd); \
	n=0; \
	for prog in $(GTK3_MAN_PROGS); do \
		src=""; \
		for cand in $$sysroot/usr/share/man/man1/$$prog.1 \
			    $$sysroot/usr/share/man/man1/$$prog.1.gz; do \
			[ -f $$cand ] && src=$$cand && break; \
		done; \
		[ -n "$$src" ] || continue; \
		case $$src in \
		*.gz) cat="zcat" ;; \
		*)    cat="cat"  ;; \
		esac; \
		$$cat $$src 2>/dev/null | GROFF_NO_SGR=1 groff -Tascii \
			-mandoc - 2>/dev/null | col -bx \
			| $(MANPAGE_FIXUP) > $$out/$$prog.1; \
		if [ -s $$out/$$prog.1 ]; then \
			n=$$((n + 1)); \
			echo "  res/man/$$prog.1 ($$(wc -l < $$out/$$prog.1) lines)"; \
		else \
			rm -f $$out/$$prog.1; \
			echo "  SKIP $$prog (rendered empty)"; \
		fi; \
	done; \
	echo "rendered $$n GTK3 manual pages into res/man/"

# Install dependencies (Ubuntu/Debian)
#
# The newest meson any package in the tree asks for: GLib's meson.build, which
# is well ahead of what Ubuntu 24.04 packages (1.3.2) -- hence the pip install
# in `deps` below.  Raise it when a package that needs more is added.
MESON_MIN_VERSION = 1.4.0

deps:
	@echo "Installing build dependencies..."
	sudo apt update
	# gdisk -> sgdisk, e2fsprogs -> mkfs.ext4/e2fsck, fakeroot -> root-owned ext4 -d, dosfstools -> mkfs.fat (ESP).
	# Ubuntu names the package gnu-efi (no -dev); Debian uses gnu-efi-dev. Try both.
	sudo apt install -y gcc nasm mtools dosfstools e2fsprogs fakeroot ovmf debootstrap parted gdisk qemu-utils qemu-system-x86 grub-efi-amd64-bin rsync || true
	sudo apt install -y gnu-efi-dev || sudo apt install -y gnu-efi
	# X.Org port build tooling.  meson/ninja are needed by the packages that
	# dropped autotools (pixman, libxkbcommon); libtool/gperf/xsltproc by the
	# autotools ones; xfonts-utils supplies bdftopcf and mkfontdir, which
	# render and index the bitmap fonts on the HOST at image-build time.
	# cmake is for ctwm, which is the one package here that uses neither
	# autotools nor meson.  groff/man-db render the manual pages
	# (maintenance target only, but the tools belong on the list).
	# shared-mime-info supplies update-mime-database, which the GTK3 port
	# runs on the BUILD host to turn the MIME database's XML into the binary
	# form every reader of it actually opens -- the file manager's icons,
	# descriptions and "open with" all come from it.  It is not a library
	# dependency and nothing links against it; the port needs the program.
	sudo apt install -y meson ninja-build libtool gperf xsltproc xfonts-utils \
		bison flex cmake groff python3-pip shared-mime-info || true
	# GTK3 port build tooling.  g++ builds the target's libstdc++ and the two
	# C++ packages above it (HarfBuzz, Enchant); gettext supplies the HOST's
	# msgfmt, which every package with translations runs at build time;
	# docbook-xsl is the stylesheet set xsltproc needs to turn GLib's and
	# GTK's manual-page sources into troff -- without it those packages
	# configure with man pages silently off.
	sudo apt install -y g++ gettext docbook-xsl docbook-xml itstool || true
	# The apt meson is not always new enough.  GLib's meson.build asks for
	# >= 1.4.0 and Ubuntu 24.04 ships 1.3.2, so the port stops there with
	#
	#     meson.build:1:0: ERROR: Meson version is 1.3.2 but project requires >= 1.4.0
	#
	# which is a property of the build machine, not of the port -- the same
	# tree builds on a newer distro.  meson is a Python program and upstream
	# publishes it on PyPI, so pip supplies a current one where apt cannot.
	#
	# Into a virtual environment belonging to the PORT, not system-wide.
	# `sudo pip3 install` is refused outright on Debian and Ubuntu from 24.04
	# (PEP 668, "externally-managed-environment"): the system Python belongs
	# to the distribution, and overriding that with --break-system-packages
	# risks the package manager's own Python tools for the sake of one build
	# dependency.  A venv under ports/xorg/.hosttools needs no root, changes
	# nothing outside this tree, and sits exactly where the port's other
	# build-host programs already live -- so `git clean` takes it away with
	# everything else derived, and nothing is left behind on the machine.
	#
	# ports/xorg/build.sh puts that bin directory ahead of $$PATH when it
	# invokes meson, so the venv copy is found without anything else needing
	# to know it exists.
	@have=$$(PATH="$$(pwd)/ports/xorg/.hosttools/bin:$$PATH" meson --version 2>/dev/null || echo 0); \
	need=$(MESON_MIN_VERSION); \
	if [ "$$(printf '%s\n%s\n' "$$need" "$$have" | sort -V | head -1)" = "$$need" ]; then \
		echo "meson $$have is new enough (need >= $$need)"; \
	else \
		echo "meson $$have is older than $$need -- installing into the port's own venv"; \
		venv=ports/xorg/.hosttools/venv; \
		mkdir -p ports/xorg/.hosttools/bin; \
		python3 -m venv $$venv 2>/dev/null || { \
			echo "  python3 -m venv failed; install python3-venv:"; \
			echo "      sudo apt install -y python3-venv"; \
			exit 1; \
		}; \
		$$venv/bin/pip install --quiet --upgrade "meson>=$$need" || { \
			echo "WARNING: could not install meson >= $$need into $$venv"; \
			exit 1; \
		}; \
		ln -sfn ../venv/bin/meson ports/xorg/.hosttools/bin/meson; \
		echo "  meson $$($$venv/bin/meson --version) installed in $$venv"; \
	fi
	# Same shape, for the same reason: xkeyboard-config's rules generator
	# imports StrEnum, which is in the standard library from Python 3.11.
	# Below that its meson.build requires the PyPI backport instead, and
	# stops with
	#
	#     rules/meson.build:159:19: ERROR: python3 is missing modules: strenum
	#
	# Installed through `python3 -m pip` rather than `pip3`, which on this
	# machine belongs to a different interpreter than the python3 on PATH --
	# so pip3 would report success while meson still could not import it.
	@ver=$$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])'); \
	if [ "$$(printf '%s\n%s\n' 3.11 "$$ver" | sort -V | head -1)" = "3.11" ]; then \
		echo "python3 $$ver has StrEnum in the standard library"; \
	else \
		echo "python3 $$ver predates StrEnum -- installing the strenum backport"; \
		python3 -m pip install --user strenum || \
			echo "WARNING: no strenum; the xkeyboard-config port will not configure"; \
	fi

# Help target
help:
	@echo "LikeOS-64 UEFI Build System"
	@echo "Available targets:"
	@echo "  all        - Build the single ext4 GPT USB disk (build/likeos-ext4.img)"
	@echo "  kernel     - Build kernel ELF only"
	@echo "  bootloader - Build UEFI bootloader only"
	@echo "  usb        - Build the ext4 GPT USB disk (alias for the default image)"
	@echo "  qemu       - Run the ext4 GPT USB disk in QEMU (UEFI, boots from ext4 root)"
	@echo "  qemu-usb   - Same as qemu plus SLIRP networking (sudo, for raw ICMP ping)"
	@echo "  qemu-usb-gdb - Same as qemu-usb but with GDB server on :1234 (rebuilds with -g)"
	@echo "  qemu-realusb - Run QEMU with real USB device as xHCI storage (requires USB_DEVICE=/dev/sdX)"
	@echo "  qemu-realusb-gdb - Same as qemu-realusb but with GDB server on :1234 (requires USB_DEVICE=/dev/sdX)"
	@echo "  qemu-usb-passthrough - Run QEMU with host USB device passthrough (experimental)"
	@echo "  usb-write  - dd the ext4 GPT USB disk to a real device (requires USB_DEVICE=/dev/sdX)"
	@echo "  linux-usb  - Build Debian-based host USB image that auto-launches LikeOS via QEMU/KVM"
	@echo "  linux-usb-write - Write the host Linux image to USB (requires USB_DEVICE=/dev/sdX)"
	@echo "  bash-manpage - Re-render res/man/bash.1 from the bash port's troff source (needs groff)"
	@echo "  clean      - Clean build files (ports are preserved for fast incremental rebuild)"
	@echo "  distclean  - Clean everything including ports (full rebuild next time)"
	@echo "  deps       - Install build dependencies"
	@echo ""
	@echo "Environment/Options:"
	@echo "  NUM_CPUS=N        - Set number of QEMU CPUs (default: 4)"
	@echo "  NO_SMP=1          - Disable SMP (omit -smp argument entirely)"
	@echo "  USB_HID=1         - Add USB keyboard and mouse to QEMU xHCI controller"
	@echo "  EXT4_MB=N         - Force the ext4 root partition size in MB (default: auto-size to content)"
	@echo "  ESP_MB=N          - FAT EFI System Partition size in MB (default: 64)"
	@echo "  SCREEN_SIZE=large or unset - Preferred bootloader resolution 1920x1200 (fallback: 1920x1080, 1280x800, 1280x768, 1024x768)"
	@echo "  SCREEN_SIZE=medium - Preferred bootloader resolution 1280x800 (fallback: 1280x768, 1024x768)"
	@echo "  MAX_SCREEN_SIZE=WxH - Ceiling on the above list, e.g. MAX_SCREEN_SIZE=1920x1080 picks 1920x1080 instead of 1920x1200 (default: no ceiling)"
	@echo "  LIKEOS_VERSION=x.y.z - Override the version in the boot banner and uname (default: $(LIKEOS_VERSION))"
	@echo "  CRASH_VERBOSE=1   - Emit detailed userspace and kernel crash reports (registers, fault decode, page-table walk) in an otherwise stripped, non-poisoned production-like build."
	@echo "  DEBUG=1           - Full debug build: kernel memory poisoning, DWARF symbols, unstripped kernel, libc stack-smash detail, and the RIP byte dumps (implies CRASH_VERBOSE=1)."
	@echo "  NO_STRIP=1        - Keep the kernel symbol table (do not strip kernel.elf) so a crash trace can be resolved to function names with nm/objdump (implied by DEBUG=1)."
	@echo "  NO_STRIP_USER=1   - Keep symbol tables in USERSPACE programs and libraries, so gdb can resolve function names and set breakpoints by name rather than by address (implied by DEBUG=1).  gdb itself is always stripped: its own symbols cannot help it debug anything else and they cost 148M.  The unstripped binary stays at ports/gdb-17.2/build/gdb/gdb."
	@echo ""
	@echo "Subsystem Notes:"
	@echo "  PS/2: Optional; modern hardware may lack controller (fallback to USB HID planned)."
	@echo "  IOAPIC: Minimal; ACPI parsing not yet implemented (polarity for IRQ1 forced low)."
	@echo ""
	@echo "Example USB write: make usb-write USB_DEVICE=/dev/sdb"

# Individual targets
kernel: $(KERNEL_ELF)
bootloader: $(BOOTLOADER_EFI)
usb: $(GPT_DISK)

.PHONY: all clean distclean qemu qemu-usb qemu-usb-gdb qemu-usb-passthrough qemu-realusb qemu-realusb-gdb usb-write deps help kernel bootloader usb linux-usb linux-usb-write
