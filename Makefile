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
LIKEOS_VERSION ?= 0.2.3-HEAD

# Tools
GCC = gcc
LD = ld
OBJCOPY = objcopy
STRIP = strip
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

# Screen size: pass SCREEN_SIZE=large for 1920x1200 preferred resolution,
# SCREEN_SIZE=medium (or unset) for 1280x800 preferred resolution.
ifeq ($(SCREEN_SIZE),large)
  UEFI_SCREEN_CFLAGS = -DSCREEN_LARGE
else
  UEFI_SCREEN_CFLAGS =
endif

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
KERNEL_CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
			-fstack-protector-strong -mstack-protector-guard=tls \
			-mstack-protector-guard-reg=gs -mstack-protector-guard-offset=104 \
			-mno-red-zone -mcmodel=large -fno-pic -Wall -Wextra \
			-I$(INCLUDE_DIR) -I$(KERNEL_DIR)/hal/acpica/include \
			-D__LIKEOS__ -DACPI_USE_BUILTIN_STDARG \
			-U__linux__ -U_LINUX -Ulinux \
			-DXHCI_USE_INTERRUPTS=1 $(SERIAL_CFLAGS) $(USB_SERIAL_CFLAGS) \
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
			  $(BUILD_DIR)/shell.o \
			  $(BUILD_DIR)/xhci_boot.o \
			  $(BUILD_DIR)/storage.o \
              $(BUILD_DIR)/console.o \
              $(BUILD_DIR)/sysfont.o \
              $(BUILD_DIR)/cursor.o \
              $(BUILD_DIR)/fb_optimize.o \
              $(BUILD_DIR)/interrupt.o \
              $(BUILD_DIR)/interrupt_c.o \
              $(BUILD_DIR)/gdt.o \
              $(BUILD_DIR)/gdt_c.o \
              $(BUILD_DIR)/keyboard.o \
			  $(BUILD_DIR)/serial.o \
              $(BUILD_DIR)/mouse.o \
              $(BUILD_DIR)/memory.o \
			  $(BUILD_DIR)/stack_switch.o \
			  $(BUILD_DIR)/slab.o \
			  $(BUILD_DIR)/scrollbar.o \
			  $(BUILD_DIR)/vfs.o \
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
			  $(BUILD_DIR)/elf_loader.o \
			  $(BUILD_DIR)/pipe.o \
			  $(BUILD_DIR)/stack_guard.o \
			  $(BUILD_DIR)/signal.o \
			  $(BUILD_DIR)/lapic.o \
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
			  $(BUILD_DIR)/ratelimit.o
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
ROOT_BIN_PROGS = sh ls cat pwd stat uname shutdown poweroff reboot halt ps cp mv rm \
	mkdir rmdir ln chmod readlink touch more less clear env kill find df du hexdump \
	sleep strings file grep wc head tail echo printf free uptime dmesg which date time \
	sort uniq cut tr yes true false top man hostname ping ifconfig netstat route arp \
	traceroute arping dhclient dig nslookup host nano tmux nc openssl curl
ROOT_LIBS = ld-likeos.so libc.so ncurses.so libevent.so libcrypto.so.3 libssl.so.3 \
	libz.so.1 libnghttp2.so.14 libcurl.so.4 libtestlib.so
ROOT_USRLOCAL_BINS = user_test.elf test_libc hello progerr testmem memstat teststress \
	netstress openssltest usbtest ext4test
# Full prerequisite set for the ext4 image (every staged build artifact).
GPT_PREREQS = $(addprefix $(BUILD_DIR)/,$(ROOT_BIN_PROGS) $(ROOT_LIBS) $(ROOT_USRLOCAL_BINS))

# Default target: build the single ext4 GPT USB disk.
all: $(GPT_DISK)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile kernel source files
$(BUILD_DIR)/init.o: $(KERNEL_DIR)/ke/init.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell.o: $(KERNEL_DIR)/ke/shell.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci_boot.o: $(KERNEL_DIR)/ke/xhci_boot.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/storage.o: $(KERNEL_DIR)/ke/storage.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.o: $(KERNEL_DIR)/io/console.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sysfont.o: $(KERNEL_DIR)/io/sysfont.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cursor.o: $(KERNEL_DIR)/io/cursor.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fb_optimize.o: $(KERNEL_DIR)/dev/video/fb_optimize.c | $(BUILD_DIR)
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

$(BUILD_DIR)/scrollbar.o: $(KERNEL_DIR)/io/scrollbar.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(KERNEL_DIR)/fs/vfs.c | $(BUILD_DIR)
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

$(BUILD_DIR)/elf_loader.o: $(KERNEL_DIR)/ke/elf_loader.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pipe.o: $(KERNEL_DIR)/ke/pipe.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/stack_guard.o: $(KERNEL_DIR)/ke/stack_guard.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/signal.o: $(KERNEL_DIR)/ke/signal.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lapic.o: $(KERNEL_DIR)/hal/lapic.c | $(BUILD_DIR)
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

# Copy shared libraries to build directory
$(BUILD_DIR)/ld-likeos.so: userland-rtld | $(BUILD_DIR)
	cp user/lib/rtld/ld-likeos.so $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/libc.so: userland-libc | $(BUILD_DIR)
	cp user/lib/libc/libc.so $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/libtestlib.so: userland-testlib | $(BUILD_DIR)
	cp user/lib/testlib/libtestlib.so $@
	$(STRIP) --strip-unneeded $@

# Build test programs using libc
$(BUILD_DIR)/user_test.elf: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/test_syscalls
	cp $(USER_DIR)/tests/test_syscalls $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/test_libc: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/test_libc
	cp $(USER_DIR)/tests/test_libc $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ext4test: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/ext4test
	cp $(USER_DIR)/tests/ext4test $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/hello: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hello
	cp $(USER_DIR)/hello $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/sh: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sh
	cp $(USER_DIR)/sh $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ls: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ls
	cp $(USER_DIR)/ls $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/cat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cat
	cp $(USER_DIR)/cat $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/pwd: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) pwd
	cp $(USER_DIR)/pwd $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/stat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) stat
	cp $(USER_DIR)/stat $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/progerr: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) progerr
	cp $(USER_DIR)/progerr $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/testmem: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/testmem
	cp $(USER_DIR)/tests/testmem $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/memstat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) memstat
	cp $(USER_DIR)/memstat $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/teststress: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/teststress
	cp $(USER_DIR)/tests/teststress $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/netstress: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/netstress
	cp $(USER_DIR)/tests/netstress $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/openssltest: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/openssltest
	cp $(USER_DIR)/tests/openssltest $@
	$(STRIP) --strip-unneeded $@

# openssltest is staged into the ext4 image via GPT_PREREQS (ROOT_USRLOCAL_BINS).

$(BUILD_DIR)/usbtest: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tests/usbtest
	cp $(USER_DIR)/tests/usbtest $@
	$(STRIP) --strip-unneeded $@

# usbtest is staged into the ext4 image via GPT_PREREQS (ROOT_USRLOCAL_BINS).

$(BUILD_DIR)/uname: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uname
	cp $(USER_DIR)/uname $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/shutdown: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) shutdown
	cp $(USER_DIR)/shutdown $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/poweroff: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) poweroff
	cp $(USER_DIR)/poweroff $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ps: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ps
	cp $(USER_DIR)/ps $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/cp: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cp
	cp $(USER_DIR)/cp $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/mv: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) mv
	cp $(USER_DIR)/mv $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/rm: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) rm
	cp $(USER_DIR)/rm $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/mkdir: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) mkdir
	cp $(USER_DIR)/mkdir $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ln: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ln
	cp $(USER_DIR)/ln $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/chmod: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) chmod
	cp $(USER_DIR)/chmod $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/readlink: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) readlink
	cp $(USER_DIR)/readlink $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/rmdir: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) rmdir
	cp $(USER_DIR)/rmdir $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/touch: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) touch
	cp $(USER_DIR)/touch $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/more: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) more
	cp $(USER_DIR)/more $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/less: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) less
	cp $(USER_DIR)/less $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/clear: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) clear
	cp $(USER_DIR)/clear $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/env: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) env
	cp $(USER_DIR)/env $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/kill: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) kill
	cp $(USER_DIR)/kill $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/find: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) find
	cp $(USER_DIR)/find $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/df: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) df
	cp $(USER_DIR)/df $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/du: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) du
	cp $(USER_DIR)/du $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/hexdump: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hexdump
	cp $(USER_DIR)/hexdump $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/sleep: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sleep
	cp $(USER_DIR)/sleep $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/strings: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) strings
	cp $(USER_DIR)/strings $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/file: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) file
	cp $(USER_DIR)/file $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/grep: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) grep
	cp $(USER_DIR)/grep $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/wc: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) wc
	cp $(USER_DIR)/wc $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/head: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) head
	cp $(USER_DIR)/head $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/tail: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tail
	cp $(USER_DIR)/tail $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/echo: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) echo
	cp $(USER_DIR)/echo $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/printf: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) printf
	cp $(USER_DIR)/printf $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/free: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) free
	cp $(USER_DIR)/free $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/uptime: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uptime
	cp $(USER_DIR)/uptime $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/dmesg: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dmesg
	cp $(USER_DIR)/dmesg $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/which: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) which
	cp $(USER_DIR)/which $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/date: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) date
	cp $(USER_DIR)/date $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/time: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) time
	cp $(USER_DIR)/time $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/sort: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) sort
	cp $(USER_DIR)/sort $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/uniq: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) uniq
	cp $(USER_DIR)/uniq $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/cut: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) cut
	cp $(USER_DIR)/cut $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/tr: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) tr
	cp $(USER_DIR)/tr $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/yes: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) yes
	cp $(USER_DIR)/yes $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/true: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) true
	cp $(USER_DIR)/true $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/false: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) false
	cp $(USER_DIR)/false $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/top: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) top
	cp $(USER_DIR)/top $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/man: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) man
	cp $(USER_DIR)/man $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/hostname: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) hostname
	cp $(USER_DIR)/hostname $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ping: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ping
	cp $(USER_DIR)/ping $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/ifconfig: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) ifconfig
	cp $(USER_DIR)/ifconfig $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/netstat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) netstat
	cp $(USER_DIR)/netstat $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/route: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) route
	cp $(USER_DIR)/route $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/arp: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) arp
	cp $(USER_DIR)/arp $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/traceroute: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) traceroute
	cp $(USER_DIR)/traceroute $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/arping: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) arping
	cp $(USER_DIR)/arping $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/dhclient: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dhclient
	cp $(USER_DIR)/dhclient $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/dig: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) dig
	cp $(USER_DIR)/dig $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/nslookup: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) nslookup
	cp $(USER_DIR)/nslookup $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/host: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) host
	cp $(USER_DIR)/host $@
	$(STRIP) --strip-unneeded $@

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
	$(STRIP) --strip-unneeded $@

# Build GNU nano (ported to LikeOS)
.PHONY: ports-nano
ports-nano: userland-libc userland-rtld ports-ncurses
	$(MAKE) -C ports/nano-8.3 -f Makefile.likeos

$(BUILD_DIR)/nano: ports-nano | $(BUILD_DIR)
	cp ports/nano-8.3/nano $@
	$(STRIP) --strip-unneeded $@

# Build libevent (shared library used by tmux)
.PHONY: ports-libevent
ports-libevent: userland-libc userland-rtld
	$(MAKE) -C ports/lib/libevent-2.1.12 -f Makefile.likeos

$(BUILD_DIR)/libevent.so: ports-libevent | $(BUILD_DIR)
	cp ports/lib/libevent-2.1.12/libevent.so $@
	$(STRIP) --strip-unneeded $@

# Build tmux (terminal multiplexer)
.PHONY: ports-tmux
ports-tmux: userland-libc userland-rtld ports-ncurses ports-libevent
	$(MAKE) -C ports/tmux-3.6a -f Makefile.likeos

$(BUILD_DIR)/tmux: ports-tmux | $(BUILD_DIR)
	cp ports/tmux-3.6a/tmux $@
	$(STRIP) --strip-unneeded $@

# Build netcat (nc)
.PHONY: ports-netcat
ports-netcat: userland-libc userland-rtld
	$(MAKE) -C ports/netcat-OpenBSD -f Makefile.likeos

$(BUILD_DIR)/nc: ports-netcat | $(BUILD_DIR)
	cp ports/netcat-OpenBSD/nc $@
	$(STRIP) --strip-unneeded $@

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


$(KERNEL_ELF): $(KERNEL_OBJS) kernel.lds | $(BUILD_DIR)
	@echo "Building LikeOS-64 kernel as ELF64..."
	$(LD) $(KERNEL_LDFLAGS) -T kernel.lds $(KERNEL_OBJS) -o $(KERNEL_ELF)
ifndef NO_STRIP
	$(STRIP) $(KERNEL_ELF)
endif
	@echo "LikeOS-64 ELF64 kernel built: $(KERNEL_ELF)"

# Build UEFI bootloader
$(BOOTLOADER_EFI): $(BOOT_DIR)/bootloader.c $(BOOT_DIR)/boot_stack_chk.c $(BOOT_DIR)/trampoline.S | $(BUILD_DIR)
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
	mkdir -p $(EXT4_STAGING)/boot $(EXT4_STAGING)/bin $(EXT4_STAGING)/lib \
		$(EXT4_STAGING)/usr/local/bin $(EXT4_STAGING)/usr/share/man/man1 \
		$(EXT4_STAGING)/usr/share/nano $(EXT4_STAGING)/res \
		$(EXT4_STAGING)/etc/ssl/certs $(EXT4_STAGING)/tmp
	# Kernel lives ONLY at /boot/kernel.elf (the bootloader loads it from ext4)
	cp $(KERNEL_ELF) $(EXT4_STAGING)/boot/kernel.elf
	# Userland programs -> /bin
	for p in $(ROOT_BIN_PROGS); do cp $(BUILD_DIR)/$$p $(EXT4_STAGING)/bin/$$p; done
	# Shared libraries -> /lib
	for l in $(ROOT_LIBS); do cp $(BUILD_DIR)/$$l $(EXT4_STAGING)/lib/$$l; done
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
	# Resources, manpages, config
	cp res/Lat15-Fixed16.psf $(EXT4_STAGING)/res/Lat15-Fixed16.psf
	cp res/left_ptr          $(EXT4_STAGING)/res/left_ptr
	cp res/man/*.1           $(EXT4_STAGING)/usr/share/man/man1/
	cp ports/nano-8.3/syntax/*.nanorc $(EXT4_STAGING)/usr/share/nano/
	cp /etc/services         $(EXT4_STAGING)/etc/services
	cp res/etc/hosts         $(EXT4_STAGING)/etc/hosts
	cp res/etc/resolv.conf   $(EXT4_STAGING)/etc/resolv.conf
	cp res/nanorc            $(EXT4_STAGING)/etc/nanorc
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
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
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
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
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
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
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
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
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
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) \
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
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -m $(QEMU_MEM) $(QEMU_SERIAL) $(QEMU_SMP) -machine q35 -device qemu-xhci,id=xhci -device usb-tablet -drive if=none,id=ext4disk,file=$(GPT_DISK),format=raw,readonly=off -device usb-storage,drive=ext4disk,bootindex=0 $$devices || echo "QEMU exited with status $$?"; \
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
	rm -rf $(BUILD_DIR)
	$(MAKE) -C user/lib/libc clean
	$(MAKE) -C user/lib/rtld clean
	$(MAKE) -C user/lib/testlib clean
	$(MAKE) -C $(USER_DIR) clean

distclean: clean
	$(MAKE) -C ports/lib/ncurses-likeos clean
	$(MAKE) -C ports/lib/libevent-2.1.12 -f Makefile.likeos clean
	$(MAKE) -C ports/lib/zlib-1.3.1 -f Makefile.likeos clean
	$(MAKE) -C ports/lib/nghttp2-1.65.0 -f Makefile.likeos clean
	$(MAKE) -C ports/nano-8.3 -f Makefile.likeos clean
	$(MAKE) -C ports/tmux-3.6a -f Makefile.likeos clean
	$(MAKE) -C ports/netcat-OpenBSD -f Makefile.likeos clean
	$(MAKE) -C ports/openssl-3.5.6 -f Makefile.likeos clean
	$(MAKE) -C ports/curl-8.14.1 -f Makefile.likeos clean

# Install dependencies (Ubuntu/Debian)
deps:
	@echo "Installing build dependencies..."
	sudo apt update
	# gdisk -> sgdisk, e2fsprogs -> mkfs.ext4/e2fsck, fakeroot -> root-owned ext4 -d, dosfstools -> mkfs.fat (ESP).
	# Ubuntu names the package gnu-efi (no -dev); Debian uses gnu-efi-dev. Try both.
	sudo apt install -y gcc nasm mtools dosfstools e2fsprogs fakeroot ovmf debootstrap parted gdisk qemu-utils qemu-system-x86 grub-efi-amd64-bin rsync || true
	sudo apt install -y gnu-efi-dev || sudo apt install -y gnu-efi

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
	@echo "  SCREEN_SIZE=large - Preferred bootloader resolution 1920x1200 (fallback: 1920x1080, 1280x800, 1280x768, 1152x864, 1024x768)"
	@echo "  SCREEN_SIZE=medium or unset - Preferred bootloader resolution 1280x800 (fallback: 1280x768, 1024x768)"
	@echo "  LIKEOS_VERSION=x.y.z - Override kernel version string reported by uname (default: 0.2.1-HEAD)"
	@echo "  CRASH_VERBOSE=1   - Emit detailed userspace and kernel crash reports (registers, fault decode, page-table walk) in an otherwise stripped, non-poisoned production-like build."
	@echo "  DEBUG=1           - Full debug build: kernel memory poisoning, DWARF symbols, unstripped kernel, libc stack-smash detail, and the RIP byte dumps (implies CRASH_VERBOSE=1)."
	@echo "  NO_STRIP=1        - Keep the kernel symbol table (do not strip kernel.elf) so a crash trace can be resolved to function names with nm/objdump (implied by DEBUG=1)."
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
