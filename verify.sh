#!/bin/bash
# LikeOS-64 Verification Script
# Checks that the OS image is properly built

IMAGE="build/LikeOS.img"

echo "LikeOS-64 Image Verification"
echo "============================"

if [ ! -f "$IMAGE" ]; then
    echo "❌ Error: $IMAGE not found"
    echo "   Run 'make' to build the OS first"
    exit 1
fi

# Check image size
SIZE=$(stat -c%s "$IMAGE")
EXPECTED_SIZE=1474560  # 1.44MB floppy
echo "📊 Image size: $SIZE bytes (expected: $EXPECTED_SIZE)"
if [ "$SIZE" -eq "$EXPECTED_SIZE" ]; then
    echo "✅ Image size correct"
else
    echo "⚠️  Image size unexpected"
fi

# Check boot signature
SIGNATURE=$(hexdump -C "$IMAGE" | grep "000001f0" | awk '{print $17$16}')
echo "🔍 Boot signature: 0x$SIGNATURE (expected: 0xAA55 in little-endian)"
if [ "$SIGNATURE" = "aa55" ]; then
    echo "✅ Boot signature correct"
else
    echo "❌ Boot signature incorrect"
fi

# Check that bootloader is present
BOOTLOADER_SIZE=$(stat -c%s "build/boot.bin")
echo "💾 Bootloader size: $BOOTLOADER_SIZE bytes (expected: 512)"
if [ "$BOOTLOADER_SIZE" -eq 512 ]; then
    echo "✅ Bootloader size correct"
else
    echo "❌ Bootloader size incorrect"
fi

# Check that kernel is present
if [ -f "build/kernel.bin" ]; then
    KERNEL_SIZE=$(stat -c%s "build/kernel.bin")
    echo "🎯 Kernel size: $KERNEL_SIZE bytes"
    echo "✅ Kernel present"
else
    echo "❌ Kernel not found"
fi

echo ""
echo "🚀 To test the OS, run:"
echo "   make run"
echo "   or"
echo "   qemu-system-x86_64 -drive format=raw,file=$IMAGE -m 128M"
echo ""
echo "Expected output: 'LikeOS-64 Booting' should appear on screen"
