# LikeOS-64 Makefile
# Builds a minimal 64-bit operating system

# Tools
NASM = nasm
GCC = gcc
LD = ld
DD = dd
QEMU = qemu-system-x86_64

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
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
OS_IMAGE = $(BUILD_DIR)/LikeOS.img

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
$(KERNEL_C_OBJ): kernel.c | $(BUILD_DIR)
	$(GCC) $(CFLAGS) -c kernel.c -o $(KERNEL_C_OBJ)

# Link kernel
$(KERNEL_BIN): $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) linker.ld
	$(LD) $(LDFLAGS) $(KERNEL_ASM_OBJ) $(KERNEL_C_OBJ) -o $(KERNEL_BIN).elf
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

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Force rebuild
rebuild: clean all

# Install dependencies (for Ubuntu/Debian)
install-deps:
	sudo apt update
	sudo apt install -y nasm gcc qemu-system-x86 build-essential

# Show help
help:
	@echo "LikeOS-64 Build System"
	@echo "======================"
	@echo "Targets:"
	@echo "  all          - Build the complete OS image"
	@echo "  run          - Build and run in QEMU"
	@echo "  debug        - Build and run in QEMU with GDB debugging"
	@echo "  clean        - Remove build files"
	@echo "  rebuild      - Clean and build"
	@echo "  install-deps - Install required dependencies (Ubuntu/Debian)"
	@echo "  help         - Show this help message"

.PHONY: all run debug clean rebuild install-deps help
