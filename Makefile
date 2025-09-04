# LikeOS-64 UEFI Build System
# Professional UEFI kernel build with modular directory structure

# Tools
GCC = gcc
LD = ld
OBJCOPY = objcopy
DD = dd
QEMU = qemu-system-x86_64
XORRISO = xorriso
MKFS_FAT = mkfs.fat
MTOOLS = mcopy

# Directories
BUILD_DIR = build
KERNEL_DIR = kernel
INCLUDE_DIR = include
BOOT_DIR = boot

# UEFI/GNU-EFI paths
EFI_INCLUDES = -I/usr/include/efi -I/usr/include/efi/x86_64
EFI_LIBS = /usr/lib/crt0-efi-x86_64.o
EFI_LDS = /usr/lib/elf_x86_64_efi.lds

# Compiler flags for kernel
KERNEL_CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin \
			-fno-stack-protector -mno-red-zone -mcmodel=large -fno-pic -Wall -Wextra \
			-I$(INCLUDE_DIR) -DXHCI_USE_INTERRUPTS=1

# Compiler flags for UEFI bootloader
UEFI_CFLAGS = -fno-stack-protector -fpic -fshort-wchar -mno-red-zone \
              -maccumulate-outgoing-args $(EFI_INCLUDES) -DEFI_FUNCTION_WRAPPER

# Linker flags
KERNEL_LDFLAGS = -nostdlib -static
UEFI_LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic

# Kernel object files
KERNEL_OBJS = $(BUILD_DIR)/init.o \
              $(BUILD_DIR)/console.o \
              $(BUILD_DIR)/fb_optimize.o \
              $(BUILD_DIR)/interrupt.o \
              $(BUILD_DIR)/interrupt_c.o \
              $(BUILD_DIR)/gdt.o \
              $(BUILD_DIR)/gdt_c.o \
              $(BUILD_DIR)/keyboard.o \
			  $(BUILD_DIR)/serial.o \
              $(BUILD_DIR)/mouse.o \
              $(BUILD_DIR)/memory.o \
			  $(BUILD_DIR)/scrollbar.o \
			  $(BUILD_DIR)/vfs.o \
			  $(BUILD_DIR)/pci.o \
			  $(BUILD_DIR)/block.o \
			  $(BUILD_DIR)/xhci.o \
			  $(BUILD_DIR)/fat32.o \
			  $(BUILD_DIR)/usb.o \
			  $(BUILD_DIR)/usb_msd.o \
			  $(BUILD_DIR)/ps2.o \
			  $(BUILD_DIR)/ioapic.o
# Target files
KERNEL_ELF = $(BUILD_DIR)/kernel.elf
BOOTLOADER_EFI = $(BUILD_DIR)/bootloader.efi
ISO_IMAGE = $(BUILD_DIR)/LikeOS-64.iso
FAT_IMAGE = $(BUILD_DIR)/LikeOS-64.img
USB_IMAGE = $(BUILD_DIR)/LikeOS-64-usb.img
DATA_IMAGE = $(BUILD_DIR)/msdata.img

# Default target
all: $(ISO_IMAGE) $(FAT_IMAGE) $(USB_IMAGE)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile kernel source files
$(BUILD_DIR)/init.o: $(KERNEL_DIR)/ke/init.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/console.o: $(KERNEL_DIR)/hal/console.c | $(BUILD_DIR)
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

$(BUILD_DIR)/scrollbar.o: $(KERNEL_DIR)/hal/scrollbar.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(KERNEL_DIR)/fs/vfs.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/pci.o: $(KERNEL_DIR)/hal/pci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/block.o: $(KERNEL_DIR)/hal/block.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/xhci.o: $(KERNEL_DIR)/hal/xhci.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat32.o: $(KERNEL_DIR)/fs/fat32.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb.o: $(KERNEL_DIR)/hal/usb.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb_msd.o: $(KERNEL_DIR)/hal/usb_msd.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ps2.o: $(KERNEL_DIR)/hal/ps2.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/ioapic.o: $(KERNEL_DIR)/hal/ioapic.c | $(BUILD_DIR)
	$(GCC) $(KERNEL_CFLAGS) -c $< -o $@

# Build kernel ELF
$(KERNEL_ELF): $(KERNEL_OBJS) kernel.lds | $(BUILD_DIR)
	@echo "Building LikeOS-64 kernel as ELF64..."
	$(LD) $(KERNEL_LDFLAGS) -T kernel.lds $(KERNEL_OBJS) -o $(KERNEL_ELF)
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
$(FAT_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) | $(BUILD_DIR)
	@echo "Creating UEFI bootable FAT image..."
	
	# Create a 64MB FAT32 image
	$(DD) if=/dev/zero of=$(FAT_IMAGE) bs=1M count=64
	$(MKFS_FAT) -F32 -n "LikeOS-64" $(FAT_IMAGE)
	
	# Create EFI directory structure and copy files
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(FAT_IMAGE) ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(FAT_IMAGE) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(FAT_IMAGE) $(KERNEL_ELF) ::/kernel.elf
	
	@echo "UEFI bootable FAT image created: $(FAT_IMAGE)"

# Create USB bootable image
$(USB_IMAGE): $(FAT_IMAGE) | $(BUILD_DIR)
	@echo "Creating USB bootable image..."
	cp $(FAT_IMAGE) $(USB_IMAGE)
	@echo "USB bootable image created: $(USB_IMAGE)"

# Run in QEMU with UEFI firmware
qemu: $(ISO_IMAGE)
	@echo "Running LikeOS-64 in QEMU with UEFI firmware..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M -serial stdio

# Run from FAT image in QEMU
qemu-fat: $(FAT_IMAGE)
	@echo "Running LikeOS-64 from FAT image in QEMU with UEFI firmware..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=$(FAT_IMAGE) -m 512M -serial stdio

# Standalone USB mass storage data image (64MB FAT32) now mirrors usb-write target (UEFI bootable + signature files)
# Provides: EFI/BOOT/BOOTX64.EFI, kernel.elf, LIKEOS.SIG, HELLO.TXT
$(DATA_IMAGE): $(BOOTLOADER_EFI) $(KERNEL_ELF) | $(BUILD_DIR)
	@echo "Creating USB data FAT32 image (msdata.img, 64MB, UEFI bootable)..."
	$(DD) if=/dev/zero of=$(DATA_IMAGE) bs=1M count=64
	$(MKFS_FAT) -F32 -n "MSDATA" $(DATA_IMAGE)
	# Create EFI directory structure and copy boot components
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/EFI
	MTOOLS_SKIP_CHECK=1 mmd -i $(DATA_IMAGE) ::/EFI/BOOT
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(DATA_IMAGE) $(BOOTLOADER_EFI) ::/EFI/BOOT/BOOTX64.EFI
	MTOOLS_SKIP_CHECK=1 $(MTOOLS) -i $(DATA_IMAGE) $(KERNEL_ELF) ::/kernel.elf
	# Add signature + sample files
	echo "THIS IS A DEVICE STORING LIKEOS" > $(BUILD_DIR)/LIKEOS.SIG
	echo "Hello from USB mass storage" > $(BUILD_DIR)/HELLO.TXT
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/LIKEOS.SIG ::/LIKEOS.SIG
	MTOOLS_SKIP_CHECK=1 mcopy -i $(DATA_IMAGE) $(BUILD_DIR)/HELLO.TXT ::/HELLO.TXT
	rm -f $(BUILD_DIR)/LIKEOS.SIG $(BUILD_DIR)/HELLO.TXT || true
	@echo "Data image created (UEFI + signature files): $(DATA_IMAGE)"

# Run with ISO boot plus attached xHCI controller and USB mass storage device
qemu-usb: $(ISO_IMAGE) $(DATA_IMAGE)
	@echo "Running LikeOS-64 in QEMU with xHCI + USB mass storage..."
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M -serial stdio \
		-device qemu-xhci,id=xhci -drive if=none,id=usbdisk,file=$(DATA_IMAGE),format=raw,readonly=off \
		-device usb-storage,drive=usbdisk

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
	$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMAGE) -m 512M -serial stdio -machine q35 -device qemu-xhci,id=xhci -device usb-tablet -drive if=none,id=usbdisk,file=$$usbimg,format=raw,readonly=off -device usb-storage,drive=usbdisk $$devices || echo "QEMU exited with status $$?"; \
	set +x || true

# Write ISO to USB device with GPT partition table (like Rufus)
# Usage: make usb-write USB_DEVICE=/dev/sdX
usb-write: $(ISO_IMAGE)
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
	
	# Copy bootloader and kernel
	sudo cp $(BOOTLOADER_EFI) /tmp/likeos_usb_mount/EFI/BOOT/BOOTX64.EFI
	sudo cp $(KERNEL_ELF) /tmp/likeos_usb_mount/kernel.elf

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

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Install dependencies (Ubuntu/Debian)
deps:
	@echo "Installing build dependencies..."
	sudo apt update
	sudo apt install -y gcc nasm xorriso mtools dosfstools ovmf gnu-efi-dev

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
	@echo "  qemu-usb-passthrough - Run QEMU with host USB device passthrough (experimental)"
	@echo "  usb-write  - Write to USB device with GPT (requires USB_DEVICE=/dev/sdX)"
	@echo "  clean      - Clean build files"
	@echo "  deps       - Install build dependencies"
	@echo ""
	@echo "Environment/Options:"
	@echo "  USE_USB_BOOT=1    - For qemu-usb* targets, attempt USB mass storage boot path"
	@echo ""
	@echo "Subsystem Notes:"
	@echo "  PS/2: Optional; modern hardware may lack controller (fallback to USB HID planned)."
	@echo "  IOAPIC: Minimal; ACPI parsing not yet implemented (polarity for IRQ1 forced low)."
	@echo ""
	@echo "Example USB write: make usb-write USB_DEVICE=/dev/sdb"

# Individual targets
kernel: $(KERNEL_ELF)
bootloader: $(BOOTLOADER_EFI)
iso: $(ISO_IMAGE)
fat: $(FAT_IMAGE)
usb: $(USB_IMAGE)

.PHONY: all clean qemu qemu-fat usb-write deps help kernel bootloader iso fat usb
