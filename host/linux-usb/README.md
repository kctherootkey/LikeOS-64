# Host Linux USB Launcher

A minimal Debian-based environment that boots from USB, starts X11, and immediately launches LikeOS inside QEMU with KVM for a seamless "single OS" experience.

## What it does
- Boots via GRUB EFI into a lightweight Debian userspace.
- Skips the shell and auto-starts an X11 session on tty1.
- Runs QEMU with `-enable-kvm`, full-screen, loading `LikeOS-64.iso` bundled on the stick.

## Build

```bash
make iso              # builds build/LikeOS-64.iso
make linux-usb        # builds build/linux-usb/linux-usb.img
```

The builder uses `debootstrap` and installs Xorg + QEMU into the image. Tunables:
- `IMAGE_SIZE_MB` (default 4096)
- `EFI_SIZE_MB` (default 512)
- `DEBIAN_MIRROR` (default http://deb.debian.org/debian)

## Write to USB

```bash
make linux-usb-write USB_DEVICE=/dev/sdX
```
This runs `dd` to copy the prepared image. **All data on the device is overwritten.**

## Runtime details
- User `likeos` (passwordless sudo) owns the X session.
- Systemd unit `likeos-autostart.service` launches `/usr/local/bin/likeos-startx` on tty1.
- `.xinitrc` runs `/usr/local/bin/likeos-qemu` which starts QEMU full-screen.
- The LikeOS ISO is copied into `/usr/local/share/likeos/LikeOS-64.iso` inside the image; override at runtime with `HOBBY_ISO=/path/to.iso`.

## Requirements
Host build tools: `debootstrap`, `parted`, `gdisk`, `qemu-utils`, `grub-efi-amd64-bin`, `rsync`, plus the existing LikeOS build deps. Install via `make deps` on Debian/Ubuntu.

## Notes
- KVM requires virtualization support on the hardware running this USB stick.
- GRUB timeout is 0 for a seamless boot; keep a spare console (e.g., another VT) for debugging if needed.
