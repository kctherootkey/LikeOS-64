#!/usr/bin/env bash
# Build a minimal Debian-based USB image that boots straight into X11 and launches LikeOS in QEMU/KVM.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OVERLAY_DIR="$SCRIPT_DIR/rootfs-overlay"
BUILD_DIR="$PROJECT_ROOT/build/linux-usb"
ROOTFS_DIR="$BUILD_DIR/rootfs"
MNT_ROOT="$BUILD_DIR/mnt-root"
MNT_EFI="$BUILD_DIR/mnt-efi"
IMAGE_PATH="$BUILD_DIR/linux-usb.img"
ISO_PATH="$PROJECT_ROOT/build/LikeOS-64.iso"
DATA_IMG_PATH="$PROJECT_ROOT/build/msdata.img"
DEBIAN_CODENAME="bookworm"
IMAGE_SIZE_MB=${IMAGE_SIZE_MB:-}
EFI_SIZE_MB=${EFI_SIZE_MB:-256}
MARGIN_MB=${MARGIN_MB:-400}
MIN_IMAGE_MB=${MIN_IMAGE_MB:-1200}
DEBIAN_MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}

REQUIRED_TOOLS=(debootstrap sgdisk parted qemu-img rsync mkfs.vfat mkfs.ext4 losetup grub-install chroot)

usage() {
    cat <<EOF
Usage: IMAGE_SIZE_MB=4096 EFI_SIZE_MB=512 DEBIAN_MIRROR=http://deb.debian.org/debian $0
Builds: $IMAGE_PATH (bootable USB image)
Requires: build/LikeOS-64.iso to exist.
EOF
}

for tool in "${REQUIRED_TOOLS[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required tool: $tool" >&2
        exit 1
    fi
done

if [ ! -f "$ISO_PATH" ]; then
    echo "LikeOS ISO missing at $ISO_PATH. Build it first (make iso)." >&2
    exit 1
fi
if [ ! -f "$DATA_IMG_PATH" ]; then
    echo "LikeOS data image missing at $DATA_IMG_PATH. Build it first (make data-image)." >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" "$ROOTFS_DIR" "$MNT_ROOT" "$MNT_EFI"

cleanup() {
    set +e
    sudo umount "$MNT_ROOT/run" 2>/dev/null || true
    sudo umount "$MNT_ROOT/sys" 2>/dev/null || true
    sudo umount "$MNT_ROOT/proc" 2>/dev/null || true
    sudo umount "$MNT_ROOT/dev/pts" 2>/dev/null || true
    sudo umount "$MNT_ROOT/dev" 2>/dev/null || true
    sudo umount "$MNT_EFI" 2>/dev/null || true
    sudo umount "$MNT_ROOT/boot/efi" 2>/dev/null || true
    sudo umount "$MNT_ROOT" 2>/dev/null || true
    if [ -n "${LOOPDEV:-}" ] && losetup "$LOOPDEV" >/dev/null 2>&1; then
        sudo losetup -d "$LOOPDEV" || true
    fi
}
trap cleanup EXIT

# 1) Bootstrap a minimal Debian rootfs
if [ ! -f "$ROOTFS_DIR/.bootstrapped" ]; then
    sudo debootstrap \
        --arch=amd64 \
        --variant=minbase \
        --include="linux-image-amd64,systemd-sysv,grub-efi-amd64,sudo,dbus,net-tools,ifupdown,iproute2" \
        "$DEBIAN_CODENAME" "$ROOTFS_DIR" "$DEBIAN_MIRROR"
    sudo touch "$ROOTFS_DIR/.bootstrapped"
fi

# 2) Inject overlay files and LikeOS ISO
sudo rsync -a "$OVERLAY_DIR/" "$ROOTFS_DIR/"
sudo mkdir -p "$ROOTFS_DIR/usr/local/share/likeos"
sudo cp "$ISO_PATH" "$ROOTFS_DIR/usr/local/share/likeos/LikeOS-64.iso"
sudo cp "$DATA_IMG_PATH" "$ROOTFS_DIR/usr/local/share/likeos/msdata.img"

# 3) Configure inside chroot
sudo chroot "$ROOTFS_DIR" /bin/bash <<'EOF'
set -e
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
    xserver-xorg xserver-xorg-legacy xinit x11-xserver-utils openbox dbus-x11 xterm wmctrl \
    qemu-system-x86 qemu-system-gui qemu-utils ovmf socat \
    grub-efi-amd64-bin grub2-common \
    locales curl ca-certificates

# Locale and timezone defaults
if [ -x /usr/sbin/locale-gen ]; then
    sed -i 's/^# en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen
    locale-gen
    update-locale LANG=en_US.UTF-8
fi
ln -sf /usr/share/zoneinfo/UTC /etc/localtime

echo "likeos-linux" > /etc/hostname

a="$(getent passwd likeos || true)"
if [ -z "$a" ]; then
    useradd -m -s /bin/bash likeos
fi
passwd -d likeos
usermod -aG sudo,kvm,video,input likeos || true
mkdir -p /etc/sudoers.d
cat > /etc/sudoers.d/likeos <<'SUDO'
likeos ALL=(ALL) NOPASSWD:ALL
SUDO
chmod 440 /etc/sudoers.d/likeos

# Ensure home files belong to the user
chown -R likeos:likeos /home/likeos
# Ensure bundled images are readable by the likeos user
chown -R likeos:likeos /usr/local/share/likeos || true

# Enable the autostart unit and disable tty1 getty to keep a clean handoff
systemctl enable likeos-autostart.service
systemctl disable getty@tty1.service || true

# Permit Xorg to access IO ports when running rootless
if [ -x /usr/lib/xorg/Xorg ]; then
    chmod u+s /usr/lib/xorg/Xorg || true
fi

update-initramfs -u
EOF

# 3b) Compute minimal image size if not provided
if [ -z "${IMAGE_SIZE_MB}" ]; then
    ROOT_MB=$(sudo du -s --block-size=1M "$ROOTFS_DIR" | awk '{print $1}')
    ISO_MB=$(du -s --block-size=1M "$ISO_PATH" | awk '{print $1}')
    CALC_MB=$((ROOT_MB + ISO_MB + EFI_SIZE_MB + MARGIN_MB))
    if [ $CALC_MB -lt $MIN_IMAGE_MB ]; then
        CALC_MB=$MIN_IMAGE_MB
    fi
    IMAGE_SIZE_MB=$CALC_MB
    echo "Auto-sizing image: root=${ROOT_MB}MB iso=${ISO_MB}MB efi=${EFI_SIZE_MB}MB margin=${MARGIN_MB}MB -> IMAGE_SIZE_MB=${IMAGE_SIZE_MB}"
else
    echo "Using provided IMAGE_SIZE_MB=${IMAGE_SIZE_MB}"
fi

# 4) Build the USB disk image with EFI + root partitions
sudo rm -f "$IMAGE_PATH"
qemu-img create "$IMAGE_PATH" ${IMAGE_SIZE_MB}M >/dev/null

LOOPDEV=$(sudo losetup -f --show "$IMAGE_PATH")

sudo sgdisk --zap-all "$LOOPDEV"
sudo sgdisk -n1:1MiB:+${EFI_SIZE_MB}MiB -t1:ef00 -c1:"EFI System" "$LOOPDEV"
sudo sgdisk -n2:0:0 -t2:8300 -c2:"LikeOS Host" "$LOOPDEV"
sudo partprobe "$LOOPDEV"

EFI_PART=${LOOPDEV}p1
ROOT_PART=${LOOPDEV}p2

sudo mkfs.vfat -F32 -n LINUXEFI "$EFI_PART"
sudo mkfs.ext4 -L likeosroot "$ROOT_PART"

sudo mount "$ROOT_PART" "$MNT_ROOT"
sudo mkdir -p "$MNT_ROOT/boot/efi"
sudo mount "$EFI_PART" "$MNT_EFI"
sudo mount --bind "$MNT_EFI" "$MNT_ROOT/boot/efi"

# Bind mounts required for grub-install inside chroot
sudo mkdir -p "$MNT_ROOT/dev" "$MNT_ROOT/dev/pts" "$MNT_ROOT/proc" "$MNT_ROOT/sys" "$MNT_ROOT/run"
sudo mount --bind /dev "$MNT_ROOT/dev"
sudo mount --bind /dev/pts "$MNT_ROOT/dev/pts"
sudo mount --bind /proc "$MNT_ROOT/proc"
sudo mount --bind /sys "$MNT_ROOT/sys"
sudo mount --bind /run "$MNT_ROOT/run"

sudo rsync -aHAX "$ROOTFS_DIR"/ "$MNT_ROOT"/

# Install GRUB into the image
sudo chroot "$MNT_ROOT" /bin/bash <<'EOF'
set -e
export DEBIAN_FRONTEND=noninteractive
update-grub
# Prevent NVRAM writes (safe for removable media)
grub-install --target=x86_64-efi --efi-directory=/boot/efi --bootloader-id=LikeOSHost --removable --no-nvram --recheck
EOF

sync

cat <<EOF
USB image ready: $IMAGE_PATH
Write it to a USB device with:
  sudo dd if=$IMAGE_PATH of=/dev/sdX bs=4M status=progress oflag=sync
EOF
