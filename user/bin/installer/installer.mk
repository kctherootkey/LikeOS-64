# LikeOS installer build rules
#
# Included at the end of the top-level Makefile, so every variable the main
# Makefile defines (BUILD_DIR, XORG_SYSROOT, EXT4_STAGING, ...) is visible
# here.  Prerequisites of the image are added with an extra rule line for
# $(GPT_DISK) (make merges the prerequisite lists of one target).
#
# Copyright (C) 2026 The LikeOS Project

# Sections are filled in by the installer work packages:
#   libgpt            user/lib/libgpt
#   libinstaller      user/installer/backend
#   likeos-installer  user/installer/frontend (GTK3)
#   likeos-install    user/installer/cli
#   tools             user/installer/tools (likeos-gpt, likeos-efiboot)
#   release           build/likeos-rootfs.tar.zst and the live USB staging

INSTALLER_PREREQS =

# --- backend begin ---
# libgpt (GUID partition tables), libinstaller (the installer backend) and
# the likeos-gpt tool.  Static libraries: the front ends link them in.
#
# INSTALLER_SERIAL_CFLAGS: a build for a serial console (SERIAL=1 or
# USB_SERIAL=1) makes the backend copy every log line to /dev/kmsg, so the
# installation can be followed on the serial line.  Make does not track
# compiler flags; the backend's own Makefile keeps them in a stamp file
# (.il-cflags) and rebuilds its objects when they change, and
# $(INSTALLER_FLAGS_STAMP) records them for the programs linking it.
ifneq ($(strip $(SERIAL)$(filter 1,$(USB_SERIAL))),)
  INSTALLER_SERIAL_CFLAGS = -DLIKEOS_SERIAL_LOG
else
  INSTALLER_SERIAL_CFLAGS =
endif

# The zstd port (ports/zstd-<version>/, built by its Makefile.likeos into
# build/include and build/lib, see the ports section).  The extractor
# needs its header; programs linking libinstaller need the library.  The
# decision is made on the port's directory, which is in the tree, not on
# its build output, which a fresh tree does not have yet when this file is
# read: the backend depends on the port's target (below), so the port is
# built first in the same make.  Only a tree without the port builds the
# backend with -DIL_NO_ZSTD: it then unpacks plain .tar archives only and
# says so for a .tar.zst.
ZSTD_PORT_SRC := $(patsubst %/Makefile.likeos,%,$(lastword $(sort \
	$(wildcard ports/zstd-*/Makefile.likeos))))
ifneq ($(ZSTD_PORT_SRC),)
  ZSTD_PORT_BUILD := $(ZSTD_PORT_SRC)/build
  ZSTD_CFLAGS = -I$(abspath $(ZSTD_PORT_BUILD)/include)
  ZSTD_LIBS = $(abspath $(ZSTD_PORT_BUILD)/lib/libzstd.a)
  ZSTD_PORT_TARGET = ports-zstd
else
  ZSTD_PORT_BUILD :=
  ZSTD_CFLAGS = -DIL_NO_ZSTD
  ZSTD_LIBS =
  ZSTD_PORT_TARGET =
endif

INSTALLER_BACKEND_DIR = user/installer/backend
LIBGPT_DIR = user/lib/libgpt
INSTALLER_TOOLS_DIR = user/installer/tools

# What a program linking the backend needs (front ends, CLI).
INSTALLER_BACKEND_CFLAGS = -I$(INSTALLER_BACKEND_DIR)/include \
	-I$(LIBGPT_DIR)/include
INSTALLER_BACKEND_LIBS = $(BUILD_DIR)/libinstaller.a $(BUILD_DIR)/libgpt.a \
	$(ZSTD_LIBS)
# libinstaller hashes passwords with libcrypt (crypt_r, crypt_gensalt_rn):
# link with -lcrypt as well.

INSTALLER_FLAGS_STAMP = $(BUILD_DIR)/.installer-backend-flags
INSTALLER_FLAGS_NOW = $(INSTALLER_SERIAL_CFLAGS) $(ZSTD_CFLAGS)
# Rewritten (at parse time) only when the flags changed, so its date says
# when they last did; made by the rule below when it does not exist yet.
$(shell test -d $(BUILD_DIR) && \
	{ printf '%s\n' '$(INSTALLER_FLAGS_NOW)' | \
	  cmp -s - $(INSTALLER_FLAGS_STAMP) || \
	  printf '%s\n' '$(INSTALLER_FLAGS_NOW)' > $(INSTALLER_FLAGS_STAMP); })

$(INSTALLER_FLAGS_STAMP): | $(BUILD_DIR)
	printf '%s\n' '$(INSTALLER_FLAGS_NOW)' > $@

.PHONY: installer-libgpt installer-backend installer-gpt-tool

installer-libgpt:
	$(MAKE) -C $(LIBGPT_DIR) libgpt.a

installer-backend: $(ZSTD_PORT_TARGET)
	$(if $(filter -DIL_NO_ZSTD,$(ZSTD_CFLAGS)),@echo "libinstaller: no zstd port (ports/zstd-*) in the tree: the extractor is built for plain .tar only")
	$(MAKE) -C $(INSTALLER_BACKEND_DIR) libinstaller.a \
		SERIAL_LOG_CFLAGS='$(INSTALLER_SERIAL_CFLAGS)' \
		ZSTD_CFLAGS='$(ZSTD_CFLAGS)'

installer-gpt-tool: userland-libc userland-rtld installer-libgpt
	$(MAKE) -C $(INSTALLER_TOOLS_DIR) likeos-gpt

# The copies below run on every make (their sub-make targets are phony),
# so they only replace the file when its content changed: its date then
# moves only with a real change, and what links or stages it (likeos-install,
# likeos-installer, the staging) is not redone for nothing.
# INSTALLER_COPY_STRIPPED <source>: the stripped program to $@ that way.
define INSTALLER_COPY_STRIPPED
cp $(1) $@.tmp
$(USER_STRIP) --strip-unneeded $@.tmp
if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi
endef

$(BUILD_DIR)/libgpt.a: installer-libgpt | $(BUILD_DIR)
	cmp -s $(LIBGPT_DIR)/libgpt.a $@ || cp $(LIBGPT_DIR)/libgpt.a $@

$(BUILD_DIR)/libinstaller.a: installer-backend $(INSTALLER_FLAGS_STAMP) | $(BUILD_DIR)
	cmp -s $(INSTALLER_BACKEND_DIR)/libinstaller.a $@ || \
		cp $(INSTALLER_BACKEND_DIR)/libinstaller.a $@

$(BUILD_DIR)/likeos-gpt: installer-gpt-tool | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(INSTALLER_TOOLS_DIR)/likeos-gpt)

# Host tests (the build host's gcc and glibc; image files, never devices).
.PHONY: installer-backend-check
installer-backend-check:
	$(MAKE) -C $(LIBGPT_DIR) -f Makefile.host check
	$(MAKE) -C $(INSTALLER_BACKEND_DIR)/tests -f Makefile.host check
# --- backend end ---

# --- frontend begin ---
# likeos-installer, the graphical installer (user/installer/frontend),
# built like the Settings window: the port toolchain's likeos-cc against the
# X sysroot's GTK 3, ordered after the GTK3 sentinel.  The source list is
# shared with the host build (Makefile.host) through sources.mk.  The
# program's style sheet, help text, logo and licence text are compiled into
# it as GResources with the build host's glib-compile-resources.
#
# INSTALLER_FE_BACKEND=real (the default) links libinstaller, libgpt and
# libzstd statically and libcrypt dynamically, as the text-mode installer
# does; the stand-in backend is compiled in as well, under names of its
# own, and chosen with --mock (made-up disks, nothing is written).
# INSTALLER_FE_BACKEND=mock builds the program with the stand-in only.
# Not part of the image yet (the release section stages it).
INSTALLER_FE_DIR := $(CURDIR)/user/installer/frontend
INSTALLER_FE_TOP := $(CURDIR)
INSTALLER_FE_BACKEND ?= real
include $(INSTALLER_FE_DIR)/sources.mk

INSTALLER_FE_GEN = $(BUILD_DIR)/installer-frontend
INSTALLER_FE_REAL = $(filter real,$(INSTALLER_FE_BACKEND))
INSTALLER_FE_CSRCS = $(addprefix $(INSTALLER_FE_DIR)/,$(INSTALLER_FE_SRCS) \
	$(INSTALLER_FE_MOCK_SRCS) \
	$(if $(INSTALLER_FE_REAL),$(INSTALLER_FE_REAL_SRCS)))
INSTALLER_FE_DEFS = $(if $(INSTALLER_FE_REAL),-DINSTALLER_HAVE_REAL)
# libinstaller.a libgpt.a [libzstd.a], then libcrypt (user/lib/libcrypt,
# staged as /lib/libcrypt.so).
INSTALLER_FE_LIBS = $(if $(INSTALLER_FE_REAL),$(abspath $(INSTALLER_BACKEND_LIBS)) \
	-L$(CURDIR)/user/lib/libcrypt -lcrypt)
INSTALLER_FE_LIBDEPS = $(if $(INSTALLER_FE_REAL),$(BUILD_DIR)/libinstaller.a \
	$(BUILD_DIR)/libgpt.a $(INSTALLER_FLAGS_STAMP))

.PHONY: installer-frontend
installer-frontend: $(BUILD_DIR)/likeos-installer

$(INSTALLER_FE_GEN)/resources.c: $(INSTALLER_FE_RES_DEPS) | $(BUILD_DIR)
	mkdir -p $(INSTALLER_FE_GEN)
	glib-compile-resources --target=$@ --generate-source \
		$(INSTALLER_FE_RES_DIRS) \
		$(INSTALLER_FE_DIR)/style/installer.gresource.xml

$(BUILD_DIR)/likeos-installer: $(INSTALLER_FE_CSRCS) $(INSTALLER_FE_HDRS) \
		$(KBDCONF_SRC) $(INSTALLER_FE_GEN)/resources.c \
		$(INSTALLER_FE_LIBDEPS) \
		| $(BUILD_DIR) $(GTK3_SENTINEL) \
		$(if $(INSTALLER_FE_REAL),userland-libcrypt)
	LIKEOS_SYSROOT=$(CURDIR)/$(XORG_SYSROOT) \
	ports/xorg/toolchain/likeos-cc $(GTK3_BAR_CFLAGS) -std=gnu11 \
		-Wall -Wextra -Wno-overlength-strings $(INSTALLER_FE_DEFS) \
		-I$(INSTALLER_FE_DIR) -I$(INSTALLER_FE_TOP)/user/installer/backend/include \
		-I$(INSTALLER_FE_TOP)/user/bin/shell \
		-o $@ $(INSTALLER_FE_CSRCS) user/bin/shell/keyboard-conf.c \
		$(INSTALLER_FE_GEN)/resources.c $(INSTALLER_FE_LIBS) $(GTK3_BAR_LIBS)
# --- frontend end ---

# --- efiboot begin ---
# likeos-efiboot, the firmware boot entry tool (user/installer/tools):
# reads and writes the firmware's Boot####/BootOrder/BootNext/Timeout
# variables through /sys/firmware/efi/efivars; the installer backend runs
# it as /usr/sbin/likeos-efiboot --create.  It links libgpt (for the
# partition's identity) and compiles boot/efi_loadopt.c, the device path
# and load option code the boot loader and the kernel share.
#
# installer-efiboot-check runs the host unit test of efi_loadopt.c
# (golden byte layouts plus a fuzz pass, under the sanitizers).
.PHONY: installer-efiboot-tool installer-efiboot-check

installer-efiboot-tool: userland-libc userland-rtld installer-libgpt
	$(MAKE) -C $(INSTALLER_TOOLS_DIR) likeos-efiboot

$(BUILD_DIR)/likeos-efiboot: installer-efiboot-tool | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(INSTALLER_TOOLS_DIR)/likeos-efiboot)

installer-efiboot-check:
	sh host/test-efi-loadopt.sh
# --- efiboot end ---

# --- cli begin ---
# likeos-install, the text-mode installer (user/installer/cli): compiled
# like the programs in user/bin, with libinstaller, libgpt and libzstd
# linked in statically and libcrypt dynamically.  Staged (by the release
# section) as /usr/sbin/likeos-install on the live medium, where the
# installer session falls back to it when the graphical installer cannot
# run.  installer-cli-check runs its host tests (simulated installations
# on image files).
INSTALLER_CLI_DIR = user/installer/cli

.PHONY: installer-cli installer-cli-check
installer-cli: $(BUILD_DIR)/likeos-install

$(BUILD_DIR)/likeos-install: userland-libc userland-rtld userland-libcrypt \
		$(BUILD_DIR)/libinstaller.a $(BUILD_DIR)/libgpt.a \
		$(INSTALLER_FLAGS_STAMP) | $(BUILD_DIR)
	$(MAKE) -C $(INSTALLER_CLI_DIR) likeos-install \
		BACKEND_LIBS='$(abspath $(INSTALLER_BACKEND_LIBS))'
	$(call INSTALLER_COPY_STRIPPED,$(INSTALLER_CLI_DIR)/likeos-install)

installer-cli-check:
	$(MAKE) -C $(INSTALLER_CLI_DIR) -f Makefile.host check
# --- cli end ---

# --- ports begin ---
# The ports of the installer work, built like the disk tools' ports
# (ports-disktools, ports-e2fsprogs ...): each one in its own directory
# with its Makefile.likeos, the programs copied into $(BUILD_DIR) and
# stripped.
#   zstd 1.5.7   libzstd.a + headers in ports/zstd-1.5.7/build/{lib,include}
#                (the installer back end's extractor links it, see the
#                backend section) and the zstd program (unzstd, zstdcat)
#   attr 2.6.0   getfattr, setfattr, attr; libattr.a; /etc/xattr.conf
#   acl 2.4.0    getfacl, setfacl, chacl; libacl.a (GNU tar links it)
#   tar 1.35     GNU tar with --acls, --xattrs, --zstd, pax format: it
#                makes likeos-rootfs.tar.zst and unpacks archives on the
#                installed system
# installer-manpages re-renders the manual pages of these and of the disk,
# file system and installer tools (res/man, res/man5, and res/man-live for
# the installers, which the live medium alone has); like bash-manpage it
# is a maintenance target, the rendered pages are checked in.
.PHONY: ports-zstd ports-attr ports-acl ports-tar installer-ports \
	installer-manpages

ZSTD_PORT_DIR = ports/zstd-1.5.7
ATTR_PORT_DIR = ports/attr-2.6.0
ACL_PORT_DIR = ports/acl-2.4.0
TAR_PORT_DIR = ports/tar-1.35

ports-zstd: userland-libc userland-rtld
	$(MAKE) -C $(ZSTD_PORT_DIR) -f Makefile.likeos

ports-attr: userland-libc userland-rtld
	$(MAKE) -C $(ATTR_PORT_DIR) -f Makefile.likeos

# acl takes <attr/error_context.h> from the attr port.
ports-acl: ports-attr
	$(MAKE) -C $(ACL_PORT_DIR) -f Makefile.likeos

# tar links the acl port's libacl.a.
ports-tar: ports-acl
	$(MAKE) -C $(TAR_PORT_DIR) -f Makefile.likeos

$(BUILD_DIR)/zstd: ports-zstd | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(ZSTD_PORT_DIR)/build/zstd)

ATTR_PROGS = getfattr setfattr attr
$(addprefix $(BUILD_DIR)/,$(ATTR_PROGS)): ports-attr | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(ATTR_PORT_DIR)/build/$(notdir $@))

ACL_PROGS = getfacl setfacl chacl
$(addprefix $(BUILD_DIR)/,$(ACL_PROGS)): ports-acl | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(ACL_PORT_DIR)/build/$(notdir $@))

$(BUILD_DIR)/tar: ports-tar | $(BUILD_DIR)
	$(call INSTALLER_COPY_STRIPPED,$(TAR_PORT_DIR)/build/tar)

# What the image gets.  The staging recipe (release section) installs
#   INSTALLER_PORT_BIN        -> /bin/<name>
#   INSTALLER_PORT_USR_BIN    -> /usr/bin/<name>
#   INSTALLER_PORT_USR_BIN_LINKS  <link>:<target> symbolic links in /usr/bin
#                             (the program acts on the name it is started
#                             under: unzstd decompresses, zstdcat writes to
#                             standard output)
#   INSTALLER_PORT_ETC        <file>:<path> configuration files
#   INSTALLER_MAN5            -> /usr/share/man/man5/<name>.5 (the section 1
#                             pages are res/man/*.1, staged with the others)
INSTALLER_PORT_BIN = tar
INSTALLER_PORT_USR_BIN = zstd $(ATTR_PROGS) $(ACL_PROGS)
INSTALLER_PORT_USR_BIN_LINKS = unzstd:zstd zstdcat:zstd
INSTALLER_PORT_ETC = $(ATTR_PORT_DIR)/build/etc/xattr.conf:/etc/xattr.conf
INSTALLER_MAN5 = $(wildcard res/man5/*.5)
INSTALLER_PORT_PROGS = $(addprefix $(BUILD_DIR)/,$(INSTALLER_PORT_BIN) \
	$(INSTALLER_PORT_USR_BIN))

installer-ports: $(INSTALLER_PORT_PROGS)

# Editing a page has to change the image, as for res/man (RES_PREREQS).
# (Prerequisites of the staging both systems share: the release section.)
$(EXT4_STAGING_STAMP): $(INSTALLER_PORT_PROGS) $(INSTALLER_MAN5)

# The upstream pages are rendered from the unpacked port trees.
INSTALLER_MAN_PORTS = tar-1.35 zstd-1.5.7 attr-2.6.0 acl-2.4.0 \
	e2fsprogs-1.47.4 dosfstools-4.2 util-linux-2.42.4 hdparm-9.65 \
	nvme-cli-2.16 smartmontools-7.5

installer-manpages:
	@for p in $(INSTALLER_MAN_PORTS); do \
		$(MAKE) --no-print-directory -C ports/$$p -f Makefile.likeos \
			unpack >/dev/null || exit 1; \
	done
	$(SHELL) user/installer/man/render-manpages.sh . "$(MANPAGE_HOST_OS)"
# --- ports end ---

# --- release begin ---
# The release build: two deliverables made from one staging.
#
#   build/likeos-rootfs.tar.zst      make rootfs-tarball
#     (+ .sha256, .info)             the root filesystem of an INSTALLED
#                                    LikeOS, which the installer unpacks
#                                    onto a disk: clean (no live medium
#                                    files, no state of a running system)
#                                    and reproducible (the same tree gives
#                                    the same bytes)
#   build/likeos-ext4.img            make, make all, make usb
#                                    the live USB medium: the live system,
#                                    the installer, the boot menu, and the
#                                    archive above in /install
#
# The kernel, the boot loader and every program are the same build in both,
# so SERIAL=1 and USB_SERIAL=1 apply to an installed system as well (and
# make the installer log to the serial line, see the backend section).
#
# The order (top-level Makefile, rules $(EXT4_STAGING_STAMP) and $(GPT_DISK)):
#   1. everything both systems have is staged into $(EXT4_STAGING), the
#      installer's tools among it (INSTALLER_STAGE_COMMON);
#   2. host/stage-rootfs.sh copies that tree to $(ROOTFS_STAGING) and makes
#      it the installed system: its own /etc/fstab (res/etc/fstab.rootfs),
#      no SSH host keys, no runtime state (INSTALLER_STAGE_SPLIT);
#   3. host/mk-rootfs-tarball.sh packs $(ROOTFS_STAGING), every file as
#      root's (tar's --owner=0 --group=0, no fakeroot), like mkfs.ext4 -d
#      does for the live medium ($(ROOTFS_TARBALL));
#   4. what only the live medium has goes into $(EXT4_STAGING)
#      (INSTALLER_STAGE_LIVE), then an ADD_USER account; mkfs.ext4 -d makes
#      the root filesystem, and the EFI system partition gets the boot menu
#      (INSTALLER_ESP_EXTRA).
#
# Both systems have the kernel's /ram tmpfs and luakit's RAM launcher
# /usr/local/bin/luakit (staged with the common tree), so luakit keeps its
# files in /ram on the live medium and on an installed system alike.
#
# Only the live medium has: its own /etc/fstab, the installer (likeos-installer, likeos-install), its session and
# /etc/inittab.install, the installers' manual pages (res/man-live),
# /install, the marker /etc/likeos/live, and \EFI\LIKEOS\bootmenu.conf
# (the menu with "Run LikeOS Live from USB" and "Install LikeOS from USB";
# an installed system boots without a menu).
#
# Variables:
#   ROOTFS_ZSTD_LEVEL    zstd level of the archive (default 19, the highest
#                        that keeps the 8 MiB window the installer reads;
#                        a lower one packs faster, for a quick test build)
#   ROOTFS_ZSTD_THREADS  zstd threads (default 0: one per CPU); the
#                        archive is the same for every count
#   SOURCE_DATE_EPOCH    the latest time stamp in the archive and its build
#                        date (default: the time of the last commit)
ROOTFS_STAGING = $(BUILD_DIR)/rootfs_staging
ROOTFS_TARBALL = $(BUILD_DIR)/likeos-rootfs.tar.zst
ROOTFS_ZSTD_LEVEL ?= 19
ROOTFS_ZSTD_THREADS ?= 0
# Later than every time stamp the staging sets on purpose (the desktop
# icons' order, 2026-01-01 00:00:0n UTC): the fixed fallback, for a tree
# without git, is 2026-11-01 00:00:00 UTC.
ifneq ($(strip $(SOURCE_DATE_EPOCH)),)
  ROOTFS_EPOCH := $(strip $(SOURCE_DATE_EPOCH))
else
  ROOTFS_EPOCH := $(shell git -C $(CURDIR) log -1 --format=%ct 2>/dev/null)
  ifeq ($(strip $(ROOTFS_EPOCH)),)
    ROOTFS_EPOCH := 1793491200
  endif
endif

# What the archive is made with: make does not track variables, so a
# change of these rebuilds it through this stamp (rewritten, at parse
# time, only when they changed).
ROOTFS_FLAGS_STAMP = $(BUILD_DIR)/.rootfs-tarball-flags
ROOTFS_FLAGS_NOW = level=$(ROOTFS_ZSTD_LEVEL) threads=$(ROOTFS_ZSTD_THREADS) \
	epoch=$(ROOTFS_EPOCH) version=$(LIKEOS_VERSION)
$(shell test -d $(BUILD_DIR) && \
	{ printf '%s\n' '$(ROOTFS_FLAGS_NOW)' | \
	  cmp -s - $(ROOTFS_FLAGS_STAMP) || \
	  printf '%s\n' '$(ROOTFS_FLAGS_NOW)' > $(ROOTFS_FLAGS_STAMP); })

$(ROOTFS_FLAGS_STAMP): | $(BUILD_DIR)
	printf '%s\n' '$(ROOTFS_FLAGS_NOW)' > $@

# Step 1, the part of mk/installer.mk: the archiving and file system tools
# (the ports section), the man pages of section 5, the partitioning and
# boot entry tools, and the boot loader, which the installer copies from
# the installed system's /usr/lib/likeos/efi to its EFI system partition.
# /mnt: the installer mounts the new system at /mnt/likeos.
define INSTALLER_STAGE_COMMON
mkdir -p $(EXT4_STAGING)/mnt $(EXT4_STAGING)/usr/share/man/man5 $(EXT4_STAGING)/usr/lib/likeos/efi
chmod 0755 $(EXT4_STAGING)/mnt $(EXT4_STAGING)/usr/share/man/man5 $(EXT4_STAGING)/usr/lib/likeos $(EXT4_STAGING)/usr/lib/likeos/efi
for p in $(INSTALLER_PORT_BIN); do install -m 0755 $(BUILD_DIR)/$$p $(EXT4_STAGING)/bin/$$p; done
for p in $(INSTALLER_PORT_USR_BIN); do install -m 0755 $(BUILD_DIR)/$$p $(EXT4_STAGING)/usr/bin/$$p; done
for l in $(INSTALLER_PORT_USR_BIN_LINKS); do ln -sfn $${l#*:} $(EXT4_STAGING)/usr/bin/$${l%%:*}; done
for e in $(INSTALLER_PORT_ETC); do install -m 0644 $${e%%:*} $(EXT4_STAGING)$${e#*:}; done
install -m 0644 $(INSTALLER_MAN5) $(EXT4_STAGING)/usr/share/man/man5/
install -m 0755 $(BUILD_DIR)/likeos-gpt $(BUILD_DIR)/likeos-efiboot $(EXT4_STAGING)/usr/sbin/
install -m 0644 $(BOOTLOADER_EFI) $(EXT4_STAGING)/usr/lib/likeos/efi/BOOTX64.EFI
endef

# Step 2.
INSTALLER_STAGE_SPLIT = sh host/stage-rootfs.sh -f res/etc/fstab.rootfs \
	$(EXT4_STAGING) $(ROOTFS_STAGING)

$(EXT4_STAGING_STAMP): $(BUILD_DIR)/likeos-gpt $(BUILD_DIR)/likeos-efiboot \
	host/stage-rootfs.sh res/etc/fstab.rootfs

# Step 3.
.PHONY: rootfs-tarball
rootfs-tarball: $(ROOTFS_TARBALL)

$(ROOTFS_TARBALL): $(EXT4_STAGING_STAMP) host/mk-rootfs-tarball.sh \
		$(ROOTFS_FLAGS_STAMP) | $(BUILD_DIR)
	@test -f $(ROOTFS_STAGING)/boot/kernel.elf || { \
		echo "$(ROOTFS_STAGING) is missing: delete $(EXT4_STAGING_STAMP) and run make again" >&2; \
		exit 1; }
	bash host/mk-rootfs-tarball.sh -e '$(ROOTFS_EPOCH)' \
		-l '$(ROOTFS_ZSTD_LEVEL)' -T '$(ROOTFS_ZSTD_THREADS)' \
		-V '$(LIKEOS_VERSION)' $(ROOTFS_STAGING) $@

# Step 4.  Every command can run twice on the same tree.
INSTALLER_LIVE_FILES = res/etc/fstab.live \
	res/etc/inittab.install user/installer/session/likeos-installer-session \
	user/installer/session/xinitrc res/boot/bootmenu.conf \
	$(INSTALLER_LIVE_MAN)
# The installers' manual pages, in /usr/share/man/man1 with the others.
INSTALLER_LIVE_MAN = res/man-live/likeos-install.1 \
	res/man-live/likeos-installer.1
INSTALLER_LIVE_PROGS = $(BUILD_DIR)/likeos-installer $(BUILD_DIR)/likeos-install

# luakit's RAM launcher and /ram are not in here: both systems have them
# (top-level Makefile, common staging; the kernel mounts /ram).
define INSTALLER_STAGE_LIVE
@echo "Adding the live medium's own files to $(EXT4_STAGING)..."
install -m 0644 res/etc/fstab.live $(EXT4_STAGING)/etc/fstab
install -m 0644 res/etc/inittab.install $(EXT4_STAGING)/etc/inittab.install
install -m 0755 user/installer/session/likeos-installer-session $(EXT4_STAGING)/usr/libexec/likeos-installer-session
mkdir -p $(EXT4_STAGING)/usr/share/likeos/installer
chmod 0755 $(EXT4_STAGING)/usr/share/likeos/installer
install -m 0755 user/installer/session/xinitrc $(EXT4_STAGING)/usr/share/likeos/installer/xinitrc
install -m 0755 $(BUILD_DIR)/likeos-installer $(EXT4_STAGING)/usr/bin/likeos-installer
install -m 0755 $(BUILD_DIR)/likeos-install $(EXT4_STAGING)/usr/sbin/likeos-install
install -m 0644 $(INSTALLER_LIVE_MAN) $(EXT4_STAGING)/usr/share/man/man1/
install -m 0644 /dev/null $(EXT4_STAGING)/etc/likeos/live
mkdir -p $(EXT4_STAGING)/install
chmod 0755 $(EXT4_STAGING)/install
install -m 0644 $(ROOTFS_TARBALL) $(ROOTFS_TARBALL).sha256 $(ROOTFS_TARBALL).info $(EXT4_STAGING)/install/
endef

# Inside the shell command that builds the EFI system partition image.
INSTALLER_ESP_EXTRA = MTOOLS_SKIP_CHECK=1 mmd -i $(EXT4_ESP_IMG) ::/EFI/LIKEOS; \
	MTOOLS_SKIP_CHECK=1 mcopy -i $(EXT4_ESP_IMG) res/boot/bootmenu.conf \
	::/EFI/LIKEOS/bootmenu.conf;

$(GPT_DISK): $(ROOTFS_TARBALL) $(INSTALLER_LIVE_PROGS) $(INSTALLER_LIVE_FILES)
# --- release end ---
