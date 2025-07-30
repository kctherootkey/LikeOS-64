# LikeOS-64 Makefile
# Builds a minimal 64-bit operating system

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
SRC_DIR = .

# Compiler flags
CFLAGS = -m64 -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -fno-pic -Wall -Wextra

# Linker flags
LDFLAGS = -T linker.ld -nostdlib

# Target files
BOOTLOADER = $(BUILD_DIR)/boot.bin
KERNEL_ASM_OBJ = $(BUILD_DIR)/boot64.o
KERNEL_C_OBJ = $(BUILD_DIR)/kernel.o
KPRINTF_OBJ = $(BUILD_DIR)/kprintf.o
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
$(BOOTLOADER): boot.asm | $(BUILD_DIR)
	$(NASM) -f bin boot.asm -o $(BOOTLOADER)

# Compile kernel assembly entry point
$(KERNEL_ASM_OBJ): boot64.asm | $(BUILD_DIR)
	$(NASM) -f elf64 boot64.asm -o $(KERNEL_ASM_OBJ)

# Compile kernel C code
$(KERNEL_C_OBJ): kernel.c kprintf.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c kernel.c -o $(KERNEL_C_OBJ)

# Compile kprintf module
$(KPRINTF_OBJ): kprintf.c kprintf.h | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c kprintf.c -o $(KPRINTF_OBJ)

# Link kernel
$(KERNEL_BIN): $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) $(KPRINTF_OBJ) linker.ld
	$(LD) $(LDFLAGS) $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) $(KPRINTF_OBJ) -o $(KERNEL_BIN).elf
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

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Force rebuild
rebuild: clean all

# Install dependencies (for Ubuntu/Debian)
install-deps:
	sudo apt update
	sudo apt install -y nasm gcc qemu-system-x86 build-essential genisoimage

# Show help
help:
	@echo "LikeOS-64 Build System"
	@echo "======================"
	@echo "Targets:"
	@echo "  all          - Build the complete OS image"
	@echo "  iso          - Create bootable ISO image"
	@echo "  usb          - Create bootable USB image"
	@echo "  run          - Build and run in QEMU"
	@echo "  run-iso      - Build and run ISO in QEMU"
	@echo "  run-usb      - Build and run USB image in QEMU"
	@echo "  debug        - Build and run in QEMU with GDB debugging"
	@echo "  clean        - Remove build files"
	@echo "  rebuild      - Clean and build"
	@echo "  install-deps - Install required dependencies (Ubuntu/Debian)"
	@echo "  help         - Show this help message"

.PHONY: all iso usb run run-iso run-usb debug clean rebuild install-deps help
