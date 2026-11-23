# devd host unit tests, built with the build machine's compiler and the
# address and undefined-behaviour sanitizers.  The pure parts of the daemon
# (protocol, names, probes, filesystem table, authorization decision) and
# probes of small filesystem images made in $(OUT) with the build machine's
# mkfs tools when they are there.  No devices, sockets or mounts.
#
# Copyright (C) 2026 The LikeOS Project

HOSTCC ?= cc
OUT ?= .

SRC_DIR = ..

HOSTCFLAGS = -std=gnu11 -O1 -g -D_GNU_SOURCE -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -I$(SRC_DIR)

UNITS = $(SRC_DIR)/devproto.c $(SRC_DIR)/names.c $(SRC_DIR)/probe.c \
	$(SRC_DIR)/fstab.c $(SRC_DIR)/policy.c $(SRC_DIR)/auth.c \
	$(SRC_DIR)/util.c

all: $(OUT)/test_devd

$(OUT)/test_devd: test_devd.c $(UNITS) $(wildcard $(SRC_DIR)/*.h)
	$(HOSTCC) $(HOSTCFLAGS) test_devd.c $(UNITS) -o $@

run: $(OUT)/test_devd
	rm -rf $(OUT)/img && mkdir -p $(OUT)/img
	timeout 120 nice -n 19 $(OUT)/test_devd $(OUT)/img

# The exFAT and NTFS formats, end to end: the ported mkfs.exfat and mkntfs
# (the patched sources of ports/exfatprogs-1.4.5 and ports/ntfs-3g-2026.9.28)
# built for the build machine, with the LikeOS libc's getopt() linked in so
# that they parse their arguments as they do on LikeOS, run with exactly the
# argument vectors devd builds; the images they make are then checked with
# the build machine's fsck.exfat -n and ntfsfix -n.  Needs the build
# machine's libblkid and libuuid headers.
ROOT = ../../../..
EXFAT = $(ROOT)/ports/exfatprogs-1.4.5
NTFS = $(ROOT)/ports/ntfs-3g-2026.9.28
HT = $(OUT)/hosttools
HT_GETOPT = $(ROOT)/user/lib/libc/src/stdlib/getopt.c
HT_CFLAGS = -O1 -g -w -D__LIKEOS__
HT_EXFAT_CFLAGS = $(HT_CFLAGS) -include $(EXFAT)/config.h \
	-I$(EXFAT)/src/include -I/usr/include/blkid
HT_NTFS_CFLAGS = $(HT_CFLAGS) -DHAVE_CONFIG_H -I$(NTFS) \
	-I$(NTFS)/src/include/ntfs-3g
HT_EXFAT_SRC = $(addprefix $(EXFAT)/src/,lib/libexfat.c lib/exfat_fs.c \
	lib/exfat_dir.c lib/utils.c lib/crc.c mkfs/mkfs.c mkfs/upcase.c)
HT_NTFS_SRC = $(addprefix $(NTFS)/src/libntfs-3g/,acls.c attrib.c \
	attrlist.c bitmap.c bootsect.c cache.c collate.c compat.c compress.c \
	debug.c device.c dir.c ea.c efs.c index.c inode.c lcnalloc.c \
	logfile.c logging.c mft.c misc.c mst.c object_id.c realpath.c \
	reparse.c runlist.c security.c unistr.c volume.c xattrs.c unix_io.c) \
	$(addprefix $(NTFS)/src/ntfsprogs/,attrdef.c boot.c sd.c mkntfs.c \
	utils.c)

$(EXFAT)/.likeos-unpacked $(NTFS)/.likeos-unpacked:
	$(MAKE) -C $(dir $@) -f Makefile.likeos unpack

$(HT)/mkfs.exfat: $(EXFAT)/.likeos-unpacked $(EXFAT)/config.h $(HT_GETOPT)
	@mkdir -p $(HT)
	$(HOSTCC) $(HT_EXFAT_CFLAGS) $(HT_GETOPT) $(HT_EXFAT_SRC) -lblkid -o $@

$(HT)/mkntfs: $(NTFS)/.likeos-unpacked $(NTFS)/config.h $(HT_GETOPT)
	@mkdir -p $(HT)
	$(HOSTCC) $(HT_NTFS_CFLAGS) $(HT_GETOPT) $(HT_NTFS_SRC) -luuid -o $@

run-ports: $(OUT)/test_devd $(HT)/mkfs.exfat $(HT)/mkntfs
	rm -rf $(OUT)/img && mkdir -p $(OUT)/img
	DEVD_TEST_MKFS_DIR=$(HT) timeout 300 nice -n 19 $(OUT)/test_devd \
		$(OUT)/img

clean:
	rm -f $(OUT)/test_devd
	rm -rf $(OUT)/img $(HT)

.PHONY: all run run-ports clean
