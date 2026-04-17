# LikeOS-64 UEFI Build System
# Professional UEFI kernel build with modular directory structure

# Codename for this release
CODENAME = blessed kitty

# Tools
GCC = gcc
LD = ld
OBJCOPY = objcopy
STRIP = strip
DD = dd
QEMU = qemu-system-x86_64
XORRISO = xorriso

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

MKFS_FAT = mkfs.fat
MTOOLS = mcopy

# Directories
BUILD_DIR = build
KERNEL_DIR = kernel
INCLUDE_DIR = include
BOOT_DIR = boot
USER_DIR = user

# UEFI/GNU-EFI paths
EFI_INCLUDES = -I/usr/include/efi -I/usr/include/efi/x86_64
EFI_LIBS = /usr/lib/crt0-efi-x86_64.o
EFI_LDS = /usr/lib/elf_x86_64_efi.lds

# Compiler flags for kernel
# Note: Stack protector disabled because __stack_chk_guard access can conflict
# with identity mapping removal during boot
BUILD_DATE := $(shell LC_ALL=C date -u '+%a %b %-d %H:%M:%S UTC %Y')
KERNEL_CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
			-fno-stack-protector -mno-red-zone -mcmodel=large -fno-pic -Wall -Wextra \
			-I$(INCLUDE_DIR) -I$(KERNEL_DIR)/hal/acpica/include \
			-D__LIKEOS__ -DACPI_USE_BUILTIN_STDARG \
			-U__linux__ -U_LINUX -Ulinux \
			-DXHCI_USE_INTERRUPTS=1 $(SERIAL_CFLAGS) $(USB_SERIAL_CFLAGS) \
			-DBUILD_DATE='"$(BUILD_DATE)"'

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
			-fno-stack-protector -mno-red-zone -mcmodel=small -fno-pic -Wall -Wextra \
			-I$(USER_DIR)

# Compiler flags for UEFI bootloader
UEFI_CFLAGS = -fno-stack-protector -fpic -fshort-wchar -mno-red-zone \
              -maccumulate-outgoing-args $(EFI_INCLUDES) -DEFI_FUNCTION_WRAPPER

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
			  $(BUILD_DIR)/pci.o \
			  $(BUILD_DIR)/block.o \
			  $(BUILD_DIR)/xhci.o \
			  $(BUILD_DIR)/fat32.o \
			  $(BUILD_DIR)/pagecache.o \
			  $(BUILD_DIR)/dcache.o \
			  $(BUILD_DIR)/icache.o \
			  $(BUILD_DIR)/usb.o \
			  $(BUILD_DIR)/usb_serial.o \
			  $(BUILD_DIR)/usb_msd.o \
			  $(BUILD_DIR)/usbhid.o \
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
			  $(BUILD_DIR)/i2c_hid.o \
			  $(BUILD_DIR)/net.o \
			  $(BUILD_DIR)/e1000.o \
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
			  $(BUILD_DIR)/unix_socket.o
# Target files
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
BOOTLOADER_EFI = $(BUILD_DIR)/bootloader.efi
ISO_IMAGE = $(BUILD_DIR)/LikeOS-64.iso
FAT_IMAGE = $(BUILD_DIR)/LikeOS-64.img
USB_IMAGE = $(BUILD_DIR)/LikeOS-64-usb.img
DATA_IMAGE = $(BUILD_DIR)/msdata.img
LINUX_USB_DIR = host/linux-usb
LINUX_USB_BUILD_DIR = $(BUILD_DIR)/linux-usb
LINUX_USB_IMAGE = $(LINUX_USB_BUILD_DIR)/linux-usb.img

# Default target
all: $(ISO_IMAGE) $(FAT_IMAGE) $(USB_IMAGE)

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

$(BUILD_DIR)/console.o: $(KERNEL_DIR)/hal/console.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sysfont.o: $(KERNEL_DIR)/hal/sysfont.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/cursor.o: $(KERNEL_DIR)/hal/cursor.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fb_optimize.o: $(KERNEL_DIR)/hal/fb_optimize.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERNEL_DIR)/ke/interrupt.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/interrupt_c.o: $(KERNEL_DIR)/ke/interrupt.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: $(KERNEL_DIR)/ke/gdt.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/gdt_c.o: $(KERNEL_DIR)/ke/gdt.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/keyboard.o: $(KERNEL_DIR)/io/keyboard.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/serial.o: $(KERNEL_DIR)/hal/serial.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/mouse.o: $(KERNEL_DIR)/hal/mouse.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: $(KERNEL_DIR)/mm/memory.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/stack_switch.o: $(KERNEL_DIR)/mm/stack_switch.asm | $(BUILD_DIR)
	nasm -f elf64 $< -o $@

$(BUILD_DIR)/slab.o: $(KERNEL_DIR)/mm/slab.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/scrollbar.o: $(KERNEL_DIR)/hal/scrollbar.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(KERNEL_DIR)/fs/vfs.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/devfs.o: $(KERNEL_DIR)/fs/devfs.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci.o: $(KERNEL_DIR)/hal/pci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/block.o: $(KERNEL_DIR)/hal/block.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci.o: $(KERNEL_DIR)/hal/xhci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: $(KERNEL_DIR)/fs/fat32.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pagecache.o: $(KERNEL_DIR)/fs/pagecache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/dcache.o: $(KERNEL_DIR)/fs/dcache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/icache.o: $(KERNEL_DIR)/fs/icache.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb.o: $(KERNEL_DIR)/hal/usb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_serial.o: $(KERNEL_DIR)/hal/usb_serial.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_msd.o: $(KERNEL_DIR)/hal/usb_msd.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usbhid.o: $(KERNEL_DIR)/hal/usbhid.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ps2.o: $(KERNEL_DIR)/hal/ps2.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/i2c_hid.o: $(KERNEL_DIR)/hal/i2c_hid.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# Networking stack
$(BUILD_DIR)/net.o: $(KERNEL_DIR)/net/net.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/e1000.o: $(KERNEL_DIR)/net/e1000.c | $(BUILD_DIR)
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

$(BUILD_DIR)/random.o: $(KERNEL_DIR)/hal/random.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/route.o: $(KERNEL_DIR)/net/route.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/dns.o: $(KERNEL_DIR)/net/dns.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/unix_socket.o: $(KERNEL_DIR)/net/unix_socket.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ioapic.o: $(KERNEL_DIR)/hal/ioapic.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/timer.o: $(KERNEL_DIR)/ke/timer.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/sched.o: $(KERNEL_DIR)/ke/sched.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/tty.o: $(KERNEL_DIR)/ke/tty.c | $(BUILD_DIR)
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

# Build userland C library
.PHONY: userland-libc
userland-libc:
	$(MAKE) -C userland/libc

# Build runtime linker
.PHONY: userland-rtld
userland-rtld:
	$(MAKE) -C userland/rtld

# Build test shared library
.PHONY: userland-testlib
userland-testlib:
	$(MAKE) -C userland/testlib

# Copy shared libraries to build directory
$(BUILD_DIR)/ld-likeos.so: userland-rtld | $(BUILD_DIR)
	cp userland/rtld/ld-likeos.so $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/libc.so: userland-libc | $(BUILD_DIR)
	cp userland/libc/libc.so $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/libtestlib.so: userland-testlib | $(BUILD_DIR)
	cp userland/testlib/libtestlib.so $@
	$(STRIP) --strip-unneeded $@

# Build test programs using libc
$(BUILD_DIR)/user_test.elf: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) test_syscalls
	cp $(USER_DIR)/test_syscalls $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/test_libc: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) test_libc
	cp $(USER_DIR)/test_libc $@
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
	$(MAKE) -C $(USER_DIR) testmem
	cp $(USER_DIR)/testmem $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/memstat: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) memstat
	cp $(USER_DIR)/memstat $@
	$(STRIP) --strip-unneeded $@

$(BUILD_DIR)/teststress: userland-libc userland-rtld | $(BUILD_DIR)
	$(MAKE) -C $(USER_DIR) teststress
	cp $(USER_DIR)/teststress $@
	$(STRIP) --strip-unneeded $@

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

# Build kernel ELF
$(KERNEL_ELF): $(KERNEL_OBJS) kernel.lds | $(BUILD_DIR)
	@echo "Building LikeOS-64 kernel as ELF64..."
	$(LD) $(KERNEL_LDFLAGS) -T kernel.lds $(KERNEL_OBJS) -o $(KERNEL_ELF)
ifndef NO_STRIP
	$(STRIP) $(KERNEL_ELF)
endif
	@echo "LikeOS-64 ELF64 kernel built: $(KERNEL_ELF)"

# Build UEFI bootloader
$(BOOTLOADER_EFI): $(BOOT_DIR)/bootloader.c $(BOOT_DIR)/trampoline.S | $(BUILD_DIR)
	@echo "Building UEFI bootloader..."
	# Compile bootloader C code
	$(GCC) $(UEFI_CFLAGS) -c $(BOOT_DIR)/bootloader.c -o $(BUILD_DIR)/bootloader.o
	
	# Assemble trampoline code
	$(GCC) $(UEFI_CFLAGS) -c $(BOOT_DIR)/trampoline.S -o $(BUILD_DIR)/trampoline.o
	
	# Link as shared object
	$(LD) $(UEFI_LDFLAGS) $(EFI_LIBS) $(BUILD_DIR)/bootloader.o $(BUILD_DIR)/trampoline.o \
		-o $(BUILD_DIR)/bootloader.so \
		/usr/lib/libgnuefi.a /usr/lib/libefi.a
	
	# Convert to EFI executable
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
		-j .dynsym -j .rel -j .rela -j .reloc \
		--target=efi-app-x86_64 $(BUILD_DIR)/bootloader.so $(BOOTLOADER_EFI)
	@echo "UEFI bootloader built: $(BOOTLOADER_EFI)"

# Create UEFI bootable ISO
$(ISO_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) | $(BUILD_DIR)
	@echo "Creating UEFI bootable ISO with FAT filesystem..."
	
	# Create temporary directory structure
	mkdir -p $(BUILD_DIR)/iso_temp
	
	# Create a FAT image for EFI boot (32MB for room)
	$(DD) if=/dev/zero of=$(BUILD_DIR)/iso_temp/efiboot.img bs=1M count=32
	$(MKFS_FAT) -F16 -n "EFIBOOT" $(BUILD_DIR)/iso_temp/efiboot.img
	
	# Copy files to the EFI boot image
	MTOOLS_SKIP_CHECK=1 mmd -i $(BUILD_DIR)/iso_temp/efiboot.img ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(BUILD_DIR)/iso_temp/efiboot.img ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(BUILD_DIR)/iso_temp/efiboot.img $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(BUILD_DIR)/iso_temp/efiboot.img $(KERNEL_ELF) ::/kernel.elf
	
	# Also copy kernel to ISO root
	cp $(KERNEL_ELF) $(BUILD_DIR)/iso_temp/kernel.elf
	
	# Create hybrid UEFI/BIOS bootable ISO
	$(XORRISO) -as mkisofs \
		-R -J -joliet-long -l \
		-iso-level 3 \
		-eltorito-alt-boot \
		-e efiboot.img \
		-no-emul-boot \
		-o $(ISO_IMAGE) $(BUILD_DIR)/iso_temp/
	
	# Clean up temporary files
	rm -rf $(BUILD_DIR)/iso_temp
	@echo "UEFI bootable ISO created: $(ISO_IMAGE)"

# Create UEFI bootable FAT image (for direct use)
$(FAT_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) $(BUILD_DIR)/sh $(BUILD_DIR)/ls $(BUILD_DIR)/cat $(BUILD_DIR)/pwd $(BUILD_DIR)/stat $(BUILD_DIR)/test_libc $(BUILD_DIR)/hello $(BUILD_DIR)/progerr $(BUILD_DIR)/testmem $(BUILD_DIR)/memstat $(BUILD_DIR)/teststress $(BUILD_DIR)/uname $(BUILD_DIR)/shutdown $(BUILD_DIR)/poweroff $(BUILD_DIR)/reboot $(BUILD_DIR)/halt $(BUILD_DIR)/ps $(BUILD_DIR)/cp $(BUILD_DIR)/mv $(BUILD_DIR)/rm $(BUILD_DIR)/mkdir $(BUILD_DIR)/rmdir $(BUILD_DIR)/touch $(BUILD_DIR)/more $(BUILD_DIR)/less $(BUILD_DIR)/clear $(BUILD_DIR)/env $(BUILD_DIR)/kill $(BUILD_DIR)/find $(BUILD_DIR)/df $(BUILD_DIR)/du $(BUILD_DIR)/hexdump $(BUILD_DIR)/sleep $(BUILD_DIR)/strings $(BUILD_DIR)/file $(BUILD_DIR)/grep $(BUILD_DIR)/wc $(BUILD_DIR)/head $(BUILD_DIR)/tail $(BUILD_DIR)/echo $(BUILD_DIR)/printf $(BUILD_DIR)/free $(BUILD_DIR)/uptime $(BUILD_DIR)/dmesg $(BUILD_DIR)/which $(BUILD_DIR)/date $(BUILD_DIR)/time $(BUILD_DIR)/sort $(BUILD_DIR)/uniq $(BUILD_DIR)/cut $(BUILD_DIR)/tr $(BUILD_DIR)/yes $(BUILD_DIR)/true $(BUILD_DIR)/false $(BUILD_DIR)/top $(BUILD_DIR)/man $(BUILD_DIR)/hostname $(BUILD_DIR)/ping $(BUILD_DIR)/ifconfig $(BUILD_DIR)/netstat $(BUILD_DIR)/route $(BUILD_DIR)/arp $(BUILD_DIR)/traceroute $(BUILD_DIR)/arping $(BUILD_DIR)/dhclient $(BUILD_DIR)/dig $(BUILD_DIR)/nslookup $(BUILD_DIR)/host $(BUILD_DIR)/nano $(BUILD_DIR)/ld-likeos.so $(BUILD_DIR)/libc.so $(BUILD_DIR)/ncurses.so $(BUILD_DIR)/libtestlib.so | $(BUILD_DIR)
	@echo "Creating UEFI bootable FAT image..."
	
	# Create a 64MB FAT32 image
	$(DD) if=/dev/zero of=$(FAT_IMAGE) bs=1M count=64
	$(MKFS_FAT) -F32 -n "LikeOS-64" $(FAT_IMAGE)
	
	# Create EFI directory structure and copy files
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(FAT_IMAGE) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(FAT_IMAGE) $(KERNEL_ELF) ::/kernel.elf
	# Add signature + hello files (align with data image so USB_BOOT tests see them)
	echo "THIS IS A DEVICE STORING LIKEOS" > $(BUILD_DIR)/LIKEOS.SIG
	echo "Hello from USB mass storage" > $(BUILD_DIR)/HELLO.TXT
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/bin || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/sh ::/bin/sh
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ls ::/bin/ls
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/cat ::/bin/cat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/pwd ::/bin/pwd
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/stat ::/bin/stat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/uname ::/bin/uname
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/shutdown ::/bin/shutdown
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/poweroff ::/bin/poweroff
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/reboot ::/bin/reboot
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/halt ::/bin/halt
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ps ::/bin/ps
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/cp ::/bin/cp
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/mv ::/bin/mv
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/rm ::/bin/rm
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/mkdir ::/bin/mkdir
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/rmdir ::/bin/rmdir
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/touch ::/bin/touch
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/more ::/bin/more
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/less ::/bin/less
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/clear ::/bin/clear
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/env ::/bin/env
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/kill ::/bin/kill
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/find ::/bin/find
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/df ::/bin/df
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/du ::/bin/du
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/hexdump ::/bin/hexdump
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/sleep ::/bin/sleep
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/strings ::/bin/strings
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/file ::/bin/file
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/grep ::/bin/grep
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/wc ::/bin/wc
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/head ::/bin/head
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/tail ::/bin/tail
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/echo ::/bin/echo
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/printf ::/bin/printf
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/free ::/bin/free
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/uptime ::/bin/uptime
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/dmesg ::/bin/dmesg
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/which ::/bin/which
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/date ::/bin/date
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/time ::/bin/time
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/sort ::/bin/sort
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/uniq ::/bin/uniq
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/cut ::/bin/cut
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/tr ::/bin/tr
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/yes ::/bin/yes
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/true ::/bin/true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/false ::/bin/false
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/top ::/bin/top
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/man ::/bin/man
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/hostname ::/bin/hostname
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ping ::/bin/ping
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ifconfig ::/bin/ifconfig
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/netstat ::/bin/netstat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/route ::/bin/route
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/arp ::/bin/arp
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/traceroute ::/bin/traceroute
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/arping ::/bin/arping
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/dhclient ::/bin/dhclient
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/dig ::/bin/dig
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/nslookup ::/bin/nslookup
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/host ::/bin/host
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/nano ::/bin/nano
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/local || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/local/bin || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/test_libc ::/usr/local/bin/testlibc
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/hello ::/usr/local/bin/hello
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/progerr ::/usr/local/bin/progerr
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/testmem ::/usr/local/bin/testmem
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/memstat ::/usr/local/bin/memstat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/teststress ::/usr/local/bin/teststress
	# Create /lib directory and copy shared libraries
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/lib || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ld-likeos.so ::/lib/ld-likeos.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/libc.so ::/lib/libc.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/ncurses.so ::/lib/ncurses.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/libtestlib.so ::/lib/libtestlib.so
	# Add system font resource
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/res || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) res/Lat15-Fixed16.psf ::/res/Lat15-Fixed16.psf
	# Add mouse cursor resource
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) res/left_ptr ::/res/left_ptr
	# Add manpages
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/share || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/share/man || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/share/man/man1 || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) res/man/*.1 ::/usr/share/man/man1/
	# Create /tmp and /etc directories
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/tmp || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/etc || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) /etc/services ::/etc/services
	# Add nano system config and syntax highlighting files
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) res/nanorc ::/etc/nanorc
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/usr/share/nano || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) ports/nano-8.3/syntax/*.nanorc ::/usr/share/nano/
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/LIKEOS.SIG ::/LIKEOS.SIG
	MTOOLS_SKIP_CHECK=1 mcopy -i $(FAT_IMAGE) $(BUILD_DIR)/HELLO.TXT ::/HELLO.TXT
	rm -f $(BUILD_DIR)/LIKEOS.SIG $(BUILD_DIR)/HELLO.TXT || true
	
	@echo "UEFI bootable FAT image created: $(FAT_IMAGE)"

# Create USB bootable image
$(USB_IMAGE): $(FAT_IMAGE) | $(BUILD_DIR)
	@echo "Creating USB bootable image..."
	cp $(FAT_IMAGE) $(USB_IMAGE)
	@echo "USB bootable image created: $(USB_IMAGE)"

# Run in QEMU with UEFI firmware
qemu: $(ISO_IMAGE)
	@echo "Running LikeOS-64 in QEMU with UEFI firmware..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M $(QEMU_SERIAL) $(QEMU_SMP)

# Run from FAT image in QEMU
qemu-fat: $(FAT_IMAGE)
	@echo "Running LikeOS-64 from FAT image in QEMU with UEFI firmware..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=$(FAT_IMAGE) -m 512M $(QEMU_SERIAL) $(QEMU_SMP)

# Standalone USB mass storage data image (64MB FAT32) now mirrors usb-write target (UEFI bootable + signature files)
# Provides: EFI/BOOT/BOOTX64.EFI, kernel.elf, LIKEOS.SIG, HELLO.TXT, tests
$(DATA_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) $(BUILD_DIR)/user_test.elf $(BUILD_DIR)/test_libc $(BUILD_DIR)/hello $(BUILD_DIR)/sh $(BUILD_DIR)/ls $(BUILD_DIR)/cat $(BUILD_DIR)/pwd $(BUILD_DIR)/stat $(BUILD_DIR)/progerr $(BUILD_DIR)/testmem $(BUILD_DIR)/memstat $(BUILD_DIR)/teststress $(BUILD_DIR)/uname $(BUILD_DIR)/shutdown $(BUILD_DIR)/poweroff $(BUILD_DIR)/reboot $(BUILD_DIR)/halt $(BUILD_DIR)/ps $(BUILD_DIR)/cp $(BUILD_DIR)/mv $(BUILD_DIR)/rm $(BUILD_DIR)/mkdir $(BUILD_DIR)/rmdir $(BUILD_DIR)/touch $(BUILD_DIR)/more $(BUILD_DIR)/less $(BUILD_DIR)/clear $(BUILD_DIR)/env $(BUILD_DIR)/kill $(BUILD_DIR)/find $(BUILD_DIR)/df $(BUILD_DIR)/du $(BUILD_DIR)/hexdump $(BUILD_DIR)/sleep $(BUILD_DIR)/strings $(BUILD_DIR)/file $(BUILD_DIR)/grep $(BUILD_DIR)/wc $(BUILD_DIR)/head $(BUILD_DIR)/tail $(BUILD_DIR)/echo $(BUILD_DIR)/printf $(BUILD_DIR)/free $(BUILD_DIR)/uptime $(BUILD_DIR)/dmesg $(BUILD_DIR)/which $(BUILD_DIR)/date $(BUILD_DIR)/time $(BUILD_DIR)/sort $(BUILD_DIR)/uniq $(BUILD_DIR)/cut $(BUILD_DIR)/tr $(BUILD_DIR)/yes $(BUILD_DIR)/true $(BUILD_DIR)/false $(BUILD_DIR)/top $(BUILD_DIR)/man $(BUILD_DIR)/hostname $(BUILD_DIR)/ping $(BUILD_DIR)/ifconfig $(BUILD_DIR)/netstat $(BUILD_DIR)/route $(BUILD_DIR)/arp $(BUILD_DIR)/traceroute $(BUILD_DIR)/arping $(BUILD_DIR)/dhclient $(BUILD_DIR)/dig $(BUILD_DIR)/nslookup $(BUILD_DIR)/host $(BUILD_DIR)/nano $(BUILD_DIR)/ld-likeos.so $(BUILD_DIR)/libc.so $(BUILD_DIR)/ncurses.so $(BUILD_DIR)/libtestlib.so | $(BUILD_DIR)
	@echo "Creating USB data FAT32 image (msdata.img, 64MB, UEFI bootable)..."
	$(DD) if=/dev/zero of=$(DATA_IMAGE) bs=1M count=64
	$(MKFS_FAT) -F32 -n "MSDATA" $(DATA_IMAGE)
	# Create EFI directory structure and copy boot components
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(DATA_IMAGE) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(DATA_IMAGE) $(KERNEL_ELF) ::/kernel.elf
	# Add test program ELF
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/local || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/local/bin || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/user_test.elf ::/usr/local/bin/tests
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/test_libc ::/usr/local/bin/testlibc
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/hello ::/usr/local/bin/hello
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/progerr ::/usr/local/bin/progerr
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/testmem ::/usr/local/bin/testmem
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/memstat ::/usr/local/bin/memstat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/teststress ::/usr/local/bin/teststress
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/bin || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/sh ::/bin/sh
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ls ::/bin/ls
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/cat ::/bin/cat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/pwd ::/bin/pwd
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/stat ::/bin/stat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/uname ::/bin/uname
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/shutdown ::/bin/shutdown
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/poweroff ::/bin/poweroff
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/reboot ::/bin/reboot
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/halt ::/bin/halt
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ps ::/bin/ps
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/cp ::/bin/cp
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/mv ::/bin/mv
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/rm ::/bin/rm
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/mkdir ::/bin/mkdir
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/rmdir ::/bin/rmdir
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/touch ::/bin/touch
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/more ::/bin/more
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/less ::/bin/less
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/clear ::/bin/clear
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/env ::/bin/env
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/kill ::/bin/kill
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/find ::/bin/find
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/df ::/bin/df
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/du ::/bin/du
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/hexdump ::/bin/hexdump
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/sleep ::/bin/sleep
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/strings ::/bin/strings
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/file ::/bin/file
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/grep ::/bin/grep
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/wc ::/bin/wc
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/head ::/bin/head
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/tail ::/bin/tail
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/echo ::/bin/echo
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/printf ::/bin/printf
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/free ::/bin/free
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/uptime ::/bin/uptime
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/dmesg ::/bin/dmesg
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/which ::/bin/which
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/date ::/bin/date
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/time ::/bin/time
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/sort ::/bin/sort
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/uniq ::/bin/uniq
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/cut ::/bin/cut
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/tr ::/bin/tr
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/yes ::/bin/yes
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/true ::/bin/true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/false ::/bin/false
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/top ::/bin/top
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/man ::/bin/man
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/hostname ::/bin/hostname
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ping ::/bin/ping
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ifconfig ::/bin/ifconfig
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/netstat ::/bin/netstat
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/route ::/bin/route
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/arp ::/bin/arp
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/traceroute ::/bin/traceroute
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/arping ::/bin/arping
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/dhclient ::/bin/dhclient
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/dig ::/bin/dig
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/nslookup ::/bin/nslookup
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/host ::/bin/host
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/nano ::/bin/nano
	# Create /lib directory and copy shared libraries
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/lib || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ld-likeos.so ::/lib/ld-likeos.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/libc.so ::/lib/libc.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/ncurses.so ::/lib/ncurses.so
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/libtestlib.so ::/lib/libtestlib.so
	# Add system font resource
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/res || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) res/Lat15-Fixed16.psf ::/res/Lat15-Fixed16.psf
	# Add mouse cursor resource
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) res/left_ptr ::/res/left_ptr
	# Add manpages
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/share || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/share/man || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/share/man/man1 || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) res/man/*.1 ::/usr/share/man/man1/
	# Create /tmp and /etc directories
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/tmp || true
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/etc || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) /etc/services ::/etc/services
	# Add nano system config and syntax highlighting files
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) res/nanorc ::/etc/nanorc
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/usr/share/nano || true
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) ports/nano-8.3/syntax/*.nanorc ::/usr/share/nano/
	# Add signature + sample files
	echo "THIS IS A DEVICE STORING LIKEOS" > $(BUILD_DIR)/LIKEOS.SIG
	echo "Hello from USB mass storage" > $(BUILD_DIR)/HELLO.TXT
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/LIKEOS.SIG ::/LIKEOS.SIG
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/HELLO.TXT ::/HELLO.TXT
	rm -f $(BUILD_DIR)/LIKEOS.SIG $(BUILD_DIR)/HELLO.TXT || true
	@echo "Data image created (UEFI + signature files + tests): $(DATA_IMAGE)"

# Run with ISO boot plus attached xHCI controller and USB mass storage device
qemu-usb: $(ISO_IMAGE) $(DATA_IMAGE)
	@echo "Running LikeOS-64 in QEMU with xHCI + USB mass storage + E1000 networking..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=usbdisk,file=$(DATA_IMAGE),format=raw,readonly=off \
		-device usb-storage,drive=usbdisk $(QEMU_USB_HID) -machine type=pc,accel=kvm:tcg \
		-device e1000,netdev=net0 -netdev user,id=net0

# Run with ISO boot plus xHCI + USB mass storage, with GDB support and debug symbols
# Connect with: gdb build/kernel.elf -ex "target remote :1234"
qemu-usb-gdb:
	@echo "Rebuilding kernel with debug symbols (-g)..."
	$(MAKE) clean
	$(MAKE) KERNEL_CFLAGS="$(KERNEL_CFLAGS) -g" NO_STRIP=1 $(ISO_IMAGE) $(DATA_IMAGE)
	@echo "Running LikeOS-64 in QEMU with xHCI + USB mass storage + GDB server on :1234..."
	@echo "Connect with: gdb build/kernel.elf -ex 'target remote :1234'"
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=usbdisk,file=$(DATA_IMAGE),format=raw,readonly=off \
		-device usb-storage,drive=usbdisk $(QEMU_USB_HID) -machine type=pc,accel=kvm:tcg -s -S -monitor telnet:127.0.0.1:5555,server,nowait -d int 2> /tmp/qemu_int_log

# Run with real USB device (like /dev/sdb) as xHCI USB mass storage - boots from USB only
# Usage: make qemu-realusb USB_DEVICE=/dev/sdb [SERIAL=1]
qemu-realusb:
ifndef USB_DEVICE
	$(error USB_DEVICE is not set. Usage: make qemu-realusb USB_DEVICE=/dev/sdb)
endif
	@echo "Running LikeOS-64 in QEMU booting from xHCI USB device $(USB_DEVICE)..."
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m 512M $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file=$(USB_DEVICE) \
		-device usb-storage,bus=xhci.0,drive=stick,bootindex=1 -machine type=pc,accel=kvm:tcg \
		-device e1000,netdev=net0 -netdev user,id=net0

# Run with real USB device as xHCI USB mass storage, with GDB support and debug symbols
# Usage: make qemu-realusb-gdb USB_DEVICE=/dev/sdb [SERIAL=1]
# Connect with: gdb build/kernel.elf -ex "target remote :1234"
qemu-realusb-gdb:
ifndef USB_DEVICE
	$(error USB_DEVICE is not set. Usage: make qemu-realusb-gdb USB_DEVICE=/dev/sdb)
endif
	@echo "Rebuilding kernel with debug symbols (-g)..."
	$(MAKE) clean
	$(MAKE) KERNEL_CFLAGS="$(KERNEL_CFLAGS) -g" NO_STRIP=1 usb-write USB_DEVICE=$(USB_DEVICE)
	@echo "Running LikeOS-64 in QEMU booting from xHCI USB device $(USB_DEVICE) + GDB server on :1234..."
	@echo "Connect with: gdb build/kernel.elf -ex 'target remote :1234'"
	sudo $(QEMU) -bios /usr/share/ovmf/OVMF.fd -m 512M $(QEMU_SERIAL) $(QEMU_SMP) \
		-device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file=$(USB_DEVICE) \
		-device usb-storage,bus=xhci.0,drive=stick,bootindex=1 -machine type=pc,accel=kvm:tcg \
		-device e1000,netdev=net0 -netdev user,id=net0 \
		-s -S -monitor telnet:127.0.0.1:5555,server,nowait -d int 2> /tmp/qemu_int_log

# Extended USB passthrough target: attach tablet + optional host devices (edit vendor/product)
qemu-usb-passthrough: $(ISO_IMAGE) $(DATA_IMAGE) $(FAT_IMAGE)
	@echo "Running LikeOS-64 with xHCI, virtual storage, and host USB passthrough (if any)..."
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
	if [ "$$USE_USB_BOOT" = "1" ]; then echo "Using bootable FAT image as USB device"; usbimg="$(FAT_IMAGE)"; else usbimg="$(DATA_IMAGE)"; fi; \
	echo "Passing through devices:$$devices"; \
	set -x; \
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M $(QEMU_SERIAL) $(QEMU_SMP) -machine q35 -device qemu-xhci,id=xhci -device usb-tablet -drive if=none,id=usbdisk,file=$$usbimg,format=raw,readonly=off -device usb-storage,drive=usbdisk $$devices || echo "QEMU exited with status $$?"; \
	set +x || true

# Write ISO to USB device with GPT partition table (like Rufus)
# Usage: make usb-write USB_DEVICE=/dev/sdX [USB_SERIAL=1]
usb-write: $(ISO_IMAGE) $(BUILD_DIR)/sh $(BUILD_DIR)/ls $(BUILD_DIR)/cat $(BUILD_DIR)/pwd $(BUILD_DIR)/stat $(BUILD_DIR)/hello $(BUILD_DIR)/test_libc $(BUILD_DIR)/user_test.elf $(BUILD_DIR)/progerr $(BUILD_DIR)/testmem $(BUILD_DIR)/memstat $(BUILD_DIR)/teststress $(BUILD_DIR)/uname $(BUILD_DIR)/shutdown $(BUILD_DIR)/poweroff $(BUILD_DIR)/reboot $(BUILD_DIR)/halt $(BUILD_DIR)/ps $(BUILD_DIR)/cp $(BUILD_DIR)/mv $(BUILD_DIR)/rm $(BUILD_DIR)/mkdir $(BUILD_DIR)/rmdir $(BUILD_DIR)/touch $(BUILD_DIR)/more $(BUILD_DIR)/less $(BUILD_DIR)/clear $(BUILD_DIR)/env $(BUILD_DIR)/kill $(BUILD_DIR)/find $(BUILD_DIR)/df $(BUILD_DIR)/du $(BUILD_DIR)/hexdump $(BUILD_DIR)/sleep $(BUILD_DIR)/strings $(BUILD_DIR)/file $(BUILD_DIR)/grep $(BUILD_DIR)/wc $(BUILD_DIR)/head $(BUILD_DIR)/tail $(BUILD_DIR)/echo $(BUILD_DIR)/printf $(BUILD_DIR)/free $(BUILD_DIR)/uptime $(BUILD_DIR)/dmesg $(BUILD_DIR)/which $(BUILD_DIR)/date $(BUILD_DIR)/time $(BUILD_DIR)/sort $(BUILD_DIR)/uniq $(BUILD_DIR)/cut $(BUILD_DIR)/tr $(BUILD_DIR)/yes $(BUILD_DIR)/true $(BUILD_DIR)/false $(BUILD_DIR)/top $(BUILD_DIR)/man $(BUILD_DIR)/hostname $(BUILD_DIR)/ping $(BUILD_DIR)/ifconfig $(BUILD_DIR)/netstat $(BUILD_DIR)/route $(BUILD_DIR)/arp $(BUILD_DIR)/traceroute $(BUILD_DIR)/arping $(BUILD_DIR)/dhclient $(BUILD_DIR)/dig $(BUILD_DIR)/nslookup $(BUILD_DIR)/host $(BUILD_DIR)/nano $(BUILD_DIR)/ld-likeos.so $(BUILD_DIR)/libc.so $(BUILD_DIR)/ncurses.so $(BUILD_DIR)/libtestlib.so
	@if [ -z "$(USB_DEVICE)" ]; then \
		echo "Error: USB_DEVICE not specified. Usage: make usb-write USB_DEVICE=/dev/sdX"; \
		echo "Available devices:"; \
		lsblk -d -o NAME,SIZE,TYPE | grep disk; \
		exit 1; \
	fi
	@echo "WARNING: This will completely erase $(USB_DEVICE)!"
	@echo "Press Enter to continue or Ctrl+C to cancel..."
	@read confirm
	@echo "Creating GPT-partitioned UEFI bootable USB drive on $(USB_DEVICE)..."
	
	# Unmount any mounted partitions
	-sudo umount $(USB_DEVICE)* 2>/dev/null || true
	
	# Create GPT partition table
	sudo parted $(USB_DEVICE) --script mklabel gpt
	
	# Create EFI System Partition (FAT32, bootable)
	sudo parted $(USB_DEVICE) --script mkpart primary fat32 1MiB 100%
	sudo parted $(USB_DEVICE) --script set 1 esp on
	sudo parted $(USB_DEVICE) --script set 1 boot on
	
	# Wait for partition to be recognized
	sleep 2
	
	# Format as FAT32
	sudo mkfs.fat -F32 -n "LikeOS-64" $(USB_DEVICE)1
	
	# Mount the partition
	mkdir -p /tmp/likeos_usb_mount
	sudo mount $(USB_DEVICE)1 /tmp/likeos_usb_mount
	
	# Create EFI directory structure
	sudo mkdir -p /tmp/likeos_usb_mount/EFI/BOOT
	sudo mkdir -p /tmp/likeos_usb_mount/bin
	sudo mkdir -p /tmp/likeos_usb_mount/lib
	sudo mkdir -p /tmp/likeos_usb_mount/res
	sudo mkdir -p /tmp/likeos_usb_mount/tmp
	sudo mkdir -p /tmp/likeos_usb_mount/etc
	sudo mkdir -p /tmp/likeos_usb_mount/usr/local/bin
	sudo cp /etc/services /tmp/likeos_usb_mount/etc/services
	
	# Add nano system config and syntax highlighting files
	sudo cp res/nanorc /tmp/likeos_usb_mount/etc/nanorc
	sudo mkdir -p /tmp/likeos_usb_mount/usr/share/nano
	sudo cp ports/nano-8.3/syntax/*.nanorc /tmp/likeos_usb_mount/usr/share/nano/
	
	# Strip all binaries and shared libraries
	sudo find /tmp/likeos_usb_mount/bin /tmp/likeos_usb_mount/lib /tmp/likeos_usb_mount/usr -type f -exec strip --strip-unneeded {} \; 2>/dev/null || true
	
	# Copy bootloader and kernel
	sudo cp $(BOOTLOADER_EFI) /tmp/likeos_usb_mount/EFI/BOOT/BOOTX64.EFI
	sudo cp $(KERNEL_ELF) /tmp/likeos_usb_mount/kernel.elf

	# Copy system font
	sudo cp res/Lat15-Fixed16.psf /tmp/likeos_usb_mount/res/Lat15-Fixed16.psf
	# Copy mouse cursor
	sudo cp res/left_ptr /tmp/likeos_usb_mount/res/left_ptr

	# Copy manpages
	sudo mkdir -p /tmp/likeos_usb_mount/usr/share/man/man1
	sudo cp res/man/*.1 /tmp/likeos_usb_mount/usr/share/man/man1/

	# Copy userland programs to /bin
	sudo cp $(BUILD_DIR)/sh /tmp/likeos_usb_mount/bin/sh
	sudo cp $(BUILD_DIR)/ls /tmp/likeos_usb_mount/bin/ls
	sudo cp $(BUILD_DIR)/cat /tmp/likeos_usb_mount/bin/cat
	sudo cp $(BUILD_DIR)/pwd /tmp/likeos_usb_mount/bin/pwd
	sudo cp $(BUILD_DIR)/stat /tmp/likeos_usb_mount/bin/stat
	sudo cp $(BUILD_DIR)/uname /tmp/likeos_usb_mount/bin/uname
	sudo cp $(BUILD_DIR)/shutdown /tmp/likeos_usb_mount/bin/shutdown
	sudo cp $(BUILD_DIR)/poweroff /tmp/likeos_usb_mount/bin/poweroff
	sudo cp $(BUILD_DIR)/reboot /tmp/likeos_usb_mount/bin/reboot
	sudo cp $(BUILD_DIR)/halt /tmp/likeos_usb_mount/bin/halt
	sudo cp $(BUILD_DIR)/ps /tmp/likeos_usb_mount/bin/ps
	sudo cp $(BUILD_DIR)/cp /tmp/likeos_usb_mount/bin/cp
	sudo cp $(BUILD_DIR)/mv /tmp/likeos_usb_mount/bin/mv
	sudo cp $(BUILD_DIR)/rm /tmp/likeos_usb_mount/bin/rm
	sudo cp $(BUILD_DIR)/mkdir /tmp/likeos_usb_mount/bin/mkdir
	sudo cp $(BUILD_DIR)/rmdir /tmp/likeos_usb_mount/bin/rmdir
	sudo cp $(BUILD_DIR)/touch /tmp/likeos_usb_mount/bin/touch
	sudo cp $(BUILD_DIR)/more /tmp/likeos_usb_mount/bin/more
	sudo cp $(BUILD_DIR)/less /tmp/likeos_usb_mount/bin/less
	sudo cp $(BUILD_DIR)/clear /tmp/likeos_usb_mount/bin/clear
	sudo cp $(BUILD_DIR)/env /tmp/likeos_usb_mount/bin/env
	sudo cp $(BUILD_DIR)/kill /tmp/likeos_usb_mount/bin/kill
	sudo cp $(BUILD_DIR)/find /tmp/likeos_usb_mount/bin/find
	sudo cp $(BUILD_DIR)/df /tmp/likeos_usb_mount/bin/df
	sudo cp $(BUILD_DIR)/du /tmp/likeos_usb_mount/bin/du
	sudo cp $(BUILD_DIR)/hexdump /tmp/likeos_usb_mount/bin/hexdump
	sudo cp $(BUILD_DIR)/sleep /tmp/likeos_usb_mount/bin/sleep
	sudo cp $(BUILD_DIR)/strings /tmp/likeos_usb_mount/bin/strings
	sudo cp $(BUILD_DIR)/file /tmp/likeos_usb_mount/bin/file
	sudo cp $(BUILD_DIR)/grep /tmp/likeos_usb_mount/bin/grep
	sudo cp $(BUILD_DIR)/wc /tmp/likeos_usb_mount/bin/wc
	sudo cp $(BUILD_DIR)/head /tmp/likeos_usb_mount/bin/head
	sudo cp $(BUILD_DIR)/tail /tmp/likeos_usb_mount/bin/tail
	sudo cp $(BUILD_DIR)/echo /tmp/likeos_usb_mount/bin/echo
	sudo cp $(BUILD_DIR)/printf /tmp/likeos_usb_mount/bin/printf
	sudo cp $(BUILD_DIR)/free /tmp/likeos_usb_mount/bin/free
	sudo cp $(BUILD_DIR)/uptime /tmp/likeos_usb_mount/bin/uptime
	sudo cp $(BUILD_DIR)/dmesg /tmp/likeos_usb_mount/bin/dmesg
	sudo cp $(BUILD_DIR)/which /tmp/likeos_usb_mount/bin/which
	sudo cp $(BUILD_DIR)/date /tmp/likeos_usb_mount/bin/date
	sudo cp $(BUILD_DIR)/time /tmp/likeos_usb_mount/bin/time
	sudo cp $(BUILD_DIR)/sort /tmp/likeos_usb_mount/bin/sort
	sudo cp $(BUILD_DIR)/uniq /tmp/likeos_usb_mount/bin/uniq
	sudo cp $(BUILD_DIR)/cut /tmp/likeos_usb_mount/bin/cut
	sudo cp $(BUILD_DIR)/tr /tmp/likeos_usb_mount/bin/tr
	sudo cp $(BUILD_DIR)/yes /tmp/likeos_usb_mount/bin/yes
	sudo cp $(BUILD_DIR)/true /tmp/likeos_usb_mount/bin/true
	sudo cp $(BUILD_DIR)/false /tmp/likeos_usb_mount/bin/false
	sudo cp $(BUILD_DIR)/top /tmp/likeos_usb_mount/bin/top
	sudo cp $(BUILD_DIR)/man /tmp/likeos_usb_mount/bin/man
	sudo cp $(BUILD_DIR)/hostname /tmp/likeos_usb_mount/bin/hostname
	sudo cp $(BUILD_DIR)/ping /tmp/likeos_usb_mount/bin/ping
	sudo cp $(BUILD_DIR)/ifconfig /tmp/likeos_usb_mount/bin/ifconfig
	sudo cp $(BUILD_DIR)/netstat /tmp/likeos_usb_mount/bin/netstat
	sudo cp $(BUILD_DIR)/route /tmp/likeos_usb_mount/bin/route
	sudo cp $(BUILD_DIR)/arp /tmp/likeos_usb_mount/bin/arp
	sudo cp $(BUILD_DIR)/traceroute /tmp/likeos_usb_mount/bin/traceroute
	sudo cp $(BUILD_DIR)/arping /tmp/likeos_usb_mount/bin/arping
	sudo cp $(BUILD_DIR)/dhclient /tmp/likeos_usb_mount/bin/dhclient
	sudo cp $(BUILD_DIR)/dig /tmp/likeos_usb_mount/bin/dig
	sudo cp $(BUILD_DIR)/nslookup /tmp/likeos_usb_mount/bin/nslookup
	sudo cp $(BUILD_DIR)/host /tmp/likeos_usb_mount/bin/host
	sudo cp $(BUILD_DIR)/nano /tmp/likeos_usb_mount/bin/nano
	sudo cp $(BUILD_DIR)/hello /tmp/likeos_usb_mount/usr/local/bin/hello
	sudo cp $(BUILD_DIR)/test_libc /tmp/likeos_usb_mount/usr/local/bin/testlibc
	sudo cp $(BUILD_DIR)/user_test.elf /tmp/likeos_usb_mount/usr/local/bin/tests
	sudo cp $(BUILD_DIR)/progerr /tmp/likeos_usb_mount/usr/local/bin/progerr
	sudo cp $(BUILD_DIR)/testmem /tmp/likeos_usb_mount/usr/local/bin/testmem
	sudo cp $(BUILD_DIR)/memstat /tmp/likeos_usb_mount/usr/local/bin/memstat
	sudo cp $(BUILD_DIR)/teststress /tmp/likeos_usb_mount/usr/local/bin/teststress

	# Copy shared libraries to /lib
	sudo cp $(BUILD_DIR)/ld-likeos.so /tmp/likeos_usb_mount/lib/ld-likeos.so
	sudo cp $(BUILD_DIR)/libc.so /tmp/likeos_usb_mount/lib/libc.so
	sudo cp $(BUILD_DIR)/ncurses.so /tmp/likeos_usb_mount/lib/ncurses.so
	sudo cp $(BUILD_DIR)/libtestlib.so /tmp/likeos_usb_mount/lib/libtestlib.so

	# Create signature file and sample hello on target (mirrors data image contents)
	sudo sh -c 'echo "THIS IS A DEVICE STORING LIKEOS" > /tmp/likeos_usb_mount/LIKEOS.SIG'
	sudo sh -c 'echo "Hello from USB mass storage" > /tmp/likeos_usb_mount/HELLO.TXT'
	sudo sync
	
	# Sync and unmount
	sudo sync
	sudo umount /tmp/likeos_usb_mount
	rmdir /tmp/likeos_usb_mount
	
	@echo "UEFI bootable USB drive created successfully on $(USB_DEVICE)"
	@echo "The USB drive should now boot on UEFI systems with GPT support."

# Build a minimal Linux host that auto-starts LikeOS under QEMU/KVM
linux-usb: $(ISO_IMAGE) $(DATA_IMAGE)
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
	$(MAKE) -C userland/libc clean
	$(MAKE) -C userland/rtld clean
	$(MAKE) -C userland/testlib clean
	$(MAKE) -C user clean
	$(MAKE) -C ports/lib/ncurses-likeos clean
	$(MAKE) -C ports/nano-8.3 -f Makefile.likeos clean

# Install dependencies (Ubuntu/Debian)
deps:
	@echo "Installing build dependencies..."
	sudo apt update
	# Ubuntu names the package gnu-efi (no -dev); Debian uses gnu-efi-dev. Try both.
	sudo apt install -y gcc nasm xorriso mtools dosfstools ovmf debootstrap parted gdisk qemu-utils grub-efi-amd64-bin rsync || true
	sudo apt install -y gnu-efi-dev || sudo apt install -y gnu-efi

# Help target
help:
	@echo "LikeOS-64 UEFI Build System"
	@echo "Available targets:"
	@echo "  all        - Build all targets (ISO, FAT, USB images)"
	@echo "  kernel     - Build kernel ELF only"
	@echo "  bootloader - Build UEFI bootloader only"
	@echo "  iso        - Build UEFI bootable ISO"
	@echo "  fat        - Build UEFI bootable FAT image"
	@echo "  usb        - Build USB bootable image"
	@echo "  data-image - Build standalone UEFI + data FAT image (msdata.img) with signature files"
	@echo "  qemu       - Run in QEMU from ISO"
	@echo "  qemu-fat   - Run in QEMU from FAT image"
	@echo "  qemu-usb   - Run QEMU attaching data image as USB mass storage"
	@echo "  qemu-realusb - Run QEMU with real USB device as xHCI storage (requires USB_DEVICE=/dev/sdX)"
	@echo "  qemu-realusb-gdb - Same as qemu-realusb but with GDB server on :1234 (requires USB_DEVICE=/dev/sdX)"
	@echo "  qemu-usb-passthrough - Run QEMU with host USB device passthrough (experimental)"
	@echo "  usb-write  - Write to USB device with GPT (requires USB_DEVICE=/dev/sdX, optional USB_SERIAL=1)"
	@echo "  linux-usb  - Build Debian-based host USB image that auto-launches LikeOS via QEMU/KVM"
	@echo "  linux-usb-write - Write the host Linux image to USB (requires USB_DEVICE=/dev/sdX)"
	@echo "  clean      - Clean build files"
	@echo "  deps       - Install build dependencies"
	@echo ""
	@echo "Environment/Options:"
	@echo "  NUM_CPUS=N        - Set number of QEMU CPUs (default: 4)"
	@echo "  NO_SMP=1          - Disable SMP (omit -smp argument entirely)"
	@echo "  USB_HID=1         - Add USB keyboard and mouse to QEMU xHCI controller"
	@echo "  USE_USB_BOOT=1    - For qemu-usb* targets, attempt USB mass storage boot path"
	@echo ""
	@echo "Subsystem Notes:"
	@echo "  PS/2: Optional; modern hardware may lack controller (fallback to USB HID planned)."
	@echo "  IOAPIC: Minimal; ACPI parsing not yet implemented (polarity for IRQ1 forced low)."
	@echo ""
	@echo "Example USB write: make usb-write USB_DEVICE=/dev/sdb USB_SERIAL=1"

# Individual targets
kernel: $(KERNEL_ELF)
bootloader: $(BOOTLOADER_EFI)
iso: $(ISO_IMAGE)
fat: $(FAT_IMAGE)
usb: $(USB_IMAGE)

.PHONY: all clean qemu qemu-fat usb-write deps help kernel bootloader iso fat usb linux-usb linux-usb-write
