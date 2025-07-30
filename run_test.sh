#!/bin/bash
# LikeOS-64 Test Script
# This script builds and runs the OS in QEMU

echo "Building LikeOS-64..."
make clean
make all

if [ $? -eq 0 ]; then
    echo "Build successful! Starting QEMU..."
    echo "Press Ctrl+Alt+G to release mouse, Ctrl+Alt+Q to quit QEMU"
    echo "You should see 'LikeOS-64 Booting' on the screen"
    echo ""
    
    # Run with some nice QEMU options
    qemu-system-x86_64 \
        -drive format=raw,file=build/LikeOS.img \
        -m 128M \
        -display gtk \
        -name "LikeOS-64" \
        -boot a
else
    echo "Build failed!"
    exit 1
fi
