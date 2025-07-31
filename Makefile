# LikeOS-64 Makefile
# Professional kernel build system with modular directory structure

# Tools
NASM = nasm
GCC = gcc
LD = ld
DD = dd
QEMU = qemu-system-x86_64
GENISOIMAGE = genisoimage
MKISOFS = mkisofs

# Directories
BUILD_DIR = build
BOOT_DIR = boot
KERNEL_DIR = kernel
INCLUDE_DIR = include

# Compiler flags
CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -Wall -Wextra \
         -I$(INCLUDE_DIR)

# Linker flags
LDFLAGS = -T linker.ld -nostdlib

# Target files
BOOTLOADER = $(BUILD_DIR)/bootloader.bin
ENTRY_ASM_OBJ = $(BUILD_DIR)/entry64.o
INIT_OBJ = $(BUILD_DIR)/init.o
CONSOLE_OBJ = $(BUILD_DIR)/console.o
INTERRUPT_ASM_OBJ = $(BUILD_DIR)/interrupt.o
INTERRUPT_C_OBJ = $(BUILD_DIR)/interrupt_c.o
GDT_ASM_OBJ = $(BUILD_DIR)/gdt.o
GDT_C_OBJ = $(BUILD_DIR)/gdt_c.o
KEYBOARD_OBJ = $(BUILD_DIR)/keyboard.o
MEMORY_OBJ = $(BUILD_DIR)/memory.o
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_IMAGE = $(BUILD_DIR)/LikeOS.img
ISO_IMAGE = $(BUILD_DIR)/LikeOS.iso
USB_IMAGE = $(BUILD_DIR)/LikeOS-usb.img

# Default target
all: $(OS_IMAGE)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile bootloader
$(BOOTLOADER): $(BOOT_DIR)/bootloader.asm | $(BUILD_DIR)
	$(NASM) -f bin $(BOOT_DIR)/bootloader.asm -o $(BOOTLOADER)

# Compile 64-bit kernel entry point
$(ENTRY_ASM_OBJ): $(BOOT_DIR)/entry64.asm | $(BUILD_DIR)
	$(NASM) -f elf64 $(BOOT_DIR)/entry64.asm -o $(ENTRY_ASM_OBJ)

# Compile kernel executive initialization
$(INIT_OBJ): $(KERNEL_DIR)/ke/init.c $(INCLUDE_DIR)/kernel/console.h $(INCLUDE_DIR)/kernel/interrupt.h $(INCLUDE_DIR)/kernel/keyboard.h $(INCLUDE_DIR)/kernel/memory.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/ke/init.c -o $(INIT_OBJ)

# Compile HAL console
$(CONSOLE_OBJ): $(KERNEL_DIR)/hal/console.c $(INCLUDE_DIR)/kernel/console.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/hal/console.c -o $(CONSOLE_OBJ)

# Assemble interrupt handlers
$(INTERRUPT_ASM_OBJ): $(KERNEL_DIR)/ke/interrupt.asm | $(BUILD_DIR)
	$(NASM) -f elf64 $(KERNEL_DIR)/ke/interrupt.asm -o $(INTERRUPT_ASM_OBJ)

# Compile interrupt system
$(INTERRUPT_C_OBJ): $(KERNEL_DIR)/ke/interrupt.c $(INCLUDE_DIR)/kernel/interrupt.h $(INCLUDE_DIR)/kernel/console.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/ke/interrupt.c -o $(INTERRUPT_C_OBJ)

# Compile I/O keyboard driver
$(KEYBOARD_OBJ): $(KERNEL_DIR)/io/keyboard.c $(INCLUDE_DIR)/kernel/keyboard.h $(INCLUDE_DIR)/kernel/interrupt.h $(INCLUDE_DIR)/kernel/console.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/io/keyboard.c -o $(KEYBOARD_OBJ)

# Compile memory management
$(MEMORY_OBJ): $(KERNEL_DIR)/mm/memory.c $(INCLUDE_DIR)/kernel/memory.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/mm/memory.c -o $(MEMORY_OBJ)

# Assemble GDT
$(GDT_ASM_OBJ): $(KERNEL_DIR)/ke/gdt.asm | $(BUILD_DIR)
	$(NASM) -f elf64 $(KERNEL_DIR)/ke/gdt.asm -o $(GDT_ASM_OBJ)

# Compile GDT
$(GDT_C_OBJ): $(KERNEL_DIR)/ke/gdt.c $(INCLUDE_DIR)/kernel/interrupt.h $(INCLUDE_DIR)/kernel/console.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c $(KERNEL_DIR)/ke/gdt.c -o $(GDT_C_OBJ)

# Link kernel
$(KERNEL_BIN): $(ENTRY_ASM_OBJ) $(INIT_OBJ) $(CONSOLE_OBJ) $(INTERRUPT_ASM_OBJ) $(INTERRUPT_C_OBJ) $(GDT_ASM_OBJ) $(GDT_C_OBJ) $(KEYBOARD_OBJ) $(MEMORY_OBJ) linker.ld
	$(LD) $(LDFLAGS) $(ENTRY_ASM_OBJ) $(INIT_OBJ) $(CONSOLE_OBJ) $(INTERRUPT_ASM_OBJ) $(INTERRUPT_C_OBJ) $(GDT_ASM_OBJ) $(GDT_C_OBJ) $(KEYBOARD_OBJ) $(MEMORY_OBJ) -o $(KERNEL_BIN).elf
	objcopy -O binary $(KERNEL_BIN).elf $(KERNEL_BIN)

# Create OS image
$(OS_IMAGE): $(BOOTLOADER) $(KERNEL_BIN)
	# Create a 1.44MB floppy image (2880 sectors of 512 bytes)
	$(DD) if=/dev/zero of=$(OS_IMAGE) bs=512 count=2880
	
	# Write bootloader to first sector
	$(DD) if=$(BOOTLOADER) of=$(OS_IMAGE) bs=512 count=1 conv=notrunc
	
	# Write kernel starting from sector 2
	$(DD) if=$(KERNEL_BIN) of=$(OS_IMAGE) bs=512 seek=1 conv=notrunc

# Run in QEMU
run: $(OS_IMAGE)
	$(QEMU) -drive format=raw,file=$(OS_IMAGE) -m 128M

# Run in QEMU with debugging
debug: $(OS_IMAGE)
	$(QEMU) -drive format=raw,file=$(OS_IMAGE) -m 128M -s -S

# Create ISO image for CD/DVD booting
iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(OS_IMAGE)
	@echo "Creating ISO image with 1.44MB floppy emulation..."
	mkdir -p $(BUILD_DIR)/iso
	# Create exactly 1.44MB floppy image (1440KB = 2880 sectors)
	$(DD) if=/dev/zero of=$(BUILD_DIR)/iso/boot.img bs=512 count=2880
	# Copy our bootloader to the first sector
	$(DD) if=$(BOOTLOADER) of=$(BUILD_DIR)/iso/boot.img bs=512 count=1 conv=notrunc
	# Copy our kernel starting from sector 2
	$(DD) if=$(KERNEL_BIN) of=$(BUILD_DIR)/iso/boot.img bs=512 seek=1 conv=notrunc
	# Try genisoimage first, fallback to mkisofs
	if command -v $(GENISOIMAGE) >/dev/null 2>&1; then \
		$(GENISOIMAGE) -r -J -b boot.img -c boot.cat \
			-o $(ISO_IMAGE) $(BUILD_DIR)/iso/; \
	elif command -v $(MKISOFS) >/dev/null 2>&1; then \
		$(MKISOFS) -r -J -b boot.img -c boot.cat \
			-o $(ISO_IMAGE) $(BUILD_DIR)/iso/; \
	else \
		echo "Error: Neither genisoimage nor mkisofs found. Install cdrtools or genisoimage."; \
		exit 1; \
	fi
	rm -rf $(BUILD_DIR)/iso
	@echo "ISO image created: $(ISO_IMAGE)"
	@echo "ISO uses 1.44MB floppy emulation (standard floppy disk size)"

# Create USB bootable image
usb: $(USB_IMAGE)

$(USB_IMAGE): $(BOOTLOADER) $(KERNEL_BIN)
	@echo "Creating USB bootable image..."
	# Create a larger image for USB (16MB)
	$(DD) if=/dev/zero of=$(USB_IMAGE) bs=1M count=16
	
	# Write bootloader to first sector
	$(DD) if=$(BOOTLOADER) of=$(USB_IMAGE) bs=512 count=1 conv=notrunc
	
	# Write kernel starting from sector 2
	$(DD) if=$(KERNEL_BIN) of=$(USB_IMAGE) bs=512 seek=1 conv=notrunc
	@echo "USB image created: $(USB_IMAGE)"
	@echo "To write to USB stick: sudo dd if=$(USB_IMAGE) of=/dev/sdX bs=1M (replace sdX with your USB device)"

# Run ISO in QEMU
run-iso: $(ISO_IMAGE)
	$(QEMU) -cdrom $(ISO_IMAGE) -m 128M

# Run USB image in QEMU
run-usb: $(USB_IMAGE)
	$(QEMU) -drive format=raw,file=$(USB_IMAGE) -m 128M

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Show help
help:
	@echo "LikeOS-64 Build System"
	@echo "====================="
	@echo "Targets:"
	@echo "  all       - Build floppy disk image (default)"
	@echo "  run       - Build and run in QEMU"
	@echo "  debug     - Build and run in QEMU with GDB debugging"
	@echo "  iso       - Create bootable ISO image"
	@echo "  run-iso   - Build and run ISO in QEMU"
	@echo "  usb       - Create bootable USB image"
	@echo "  run-usb   - Build and run USB image in QEMU"
	@echo "  clean     - Remove all build artifacts"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Directory Structure:"
	@echo "  boot/          - Bootloader and initial entry point"
	@echo "  kernel/ke/     - Kernel Executive (core kernel)"
	@echo "  kernel/hal/    - Hardware Abstraction Layer"
	@echo "  kernel/io/     - I/O subsystem"
	@echo "  kernel/mm/     - Memory Management (future)"
	@echo "  include/kernel/ - Kernel headers"

.PHONY: all run debug iso run-iso usb run-usb clean help
