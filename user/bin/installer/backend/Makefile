# libinstaller -- the LikeOS installer backend (static library for LikeOS).
#
# Built like the other LikeOS libraries (see ../../../lib/libcrypt/Makefile and
# ../../../lib/libgpt/Makefile): the host gcc, freestanding, against the LikeOS
# C library headers.  The result, libinstaller.a, is linked into the GTK
# front end (likeos-installer) and the text-mode installer (likeos-install)
# together with ../../../lib/libgpt/libgpt.a, libcrypt and libzstd.
#
# Variables the top-level build (user/bin/installer/installer.mk) passes in:
#   SERIAL_LOG_CFLAGS  -DLIKEOS_SERIAL_LOG for SERIAL=1 / USB_SERIAL=1 builds:
#                      every log line also goes to /dev/kmsg
#   ZSTD_CFLAGS        -I<zstd port headers>, or -DIL_NO_ZSTD while the
#                      zstd port does not exist yet (plain .tar only then)
#
# The compiler flags are kept in a stamp file: a change of them (another
# SERIAL setting, zstd appearing, a libc call appearing) rebuilds every
# object, since make itself does not track flags.
#
# Copyright (C) 2026 The LikeOS Project

CC = gcc
AR = ar

LIBC_DIR = ../../../lib/libc
LIBGPT_DIR = ../../../lib/libgpt
LIBC_INC = $(LIBC_DIR)/include

SERIAL_LOG_CFLAGS ?=
ZSTD_CFLAGS ?= -DIL_NO_ZSTD

DEPFLAGS = -MMD -MP

# The calls of il_compat.h the LikeOS C library declares already: a line
# of a header that starts a declaration ("int mkdirat(") counts, a comment
# does not.
LIBC_HEADERS = $(LIBC_INC)/unistd.h $(LIBC_INC)/fcntl.h $(LIBC_INC)/stdio.h \
	$(LIBC_INC)/sys/stat.h $(LIBC_INC)/sys/mount.h
LP := (
il_have = $(if $(shell grep -qsE '^[A-Za-z_].*[ *]$(1)[[:space:]]*[$(LP)]' \
	$(LIBC_HEADERS) && echo y),-DIL_HAVE_$(2)=1)
HAVE_CFLAGS = $(call il_have,mount,MOUNT) $(call il_have,umount2,UMOUNT2) \
	$(call il_have,mkdirat,MKDIRAT) $(call il_have,symlinkat,SYMLINKAT) \
	$(call il_have,linkat,LINKAT) $(call il_have,fchownat,FCHOWNAT) \
	$(call il_have,lchown,LCHOWN) $(call il_have,fchmodat,FCHMODAT) \
	$(call il_have,renameat,RENAMEAT) $(call il_have,syncfs,SYNCFS) \
	$(call il_have,mknodat,MKNODAT) $(call il_have,utimensat,UTIMENSAT) \
	$(call il_have,futimens,FUTIMENS)

CFLAGS = $(DEPFLAGS) -Wall -Wextra -Werror=implicit-function-declaration \
	-Werror=misleading-indentation -O2 -nostdlib -fno-builtin \
	-fstack-protector-strong -mno-red-zone -ffreestanding -fPIC \
	-fno-tree-vectorize -fno-strict-aliasing -D_GNU_SOURCE \
	-Iinclude -I. -I$(LIBGPT_DIR)/include -I$(LIBC_INC) \
	$(HAVE_CFLAGS) $(SERIAL_LOG_CFLAGS) $(ZSTD_CFLAGS)

SRC = il_ctx.c il_log.c il_util.c il_sha256.c il_disk.c il_source.c \
	il_plan.c il_partition.c il_format.c il_mount.c il_extract.c il_tar.c \
	il_acl.c il_config.c il_accounts.c il_boot.c il_install.c il_progress.c \
	il_answers.c il_validate.c il_tz.c il_locale.c
OBJ = $(SRC:.c=.o)
OUT = libinstaller.a

FLAGS_STAMP = .il-cflags

all: $(OUT)

# Rewritten only when the flags differ from the last build's.
$(shell printf '%s\n' '$(CFLAGS)' | cmp -s - $(FLAGS_STAMP) || \
	printf '%s\n' '$(CFLAGS)' > $(FLAGS_STAMP))

$(OUT): $(OBJ)
	rm -f $@
	$(AR) rcs $@ $(OBJ)

%.o: %.c $(FLAGS_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

# What this build could not use yet (shown once per build of the library).
.PHONY: report
report:
	@echo "libinstaller: libc calls found: $(HAVE_CFLAGS)"
	@echo "libinstaller: zstd: $(ZSTD_CFLAGS)"
	@echo "libinstaller: serial log: $(if $(SERIAL_LOG_CFLAGS),yes,no)"

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(OUT) $(FLAGS_STAMP)

-include $(OBJ:.o=.d)

.PHONY: all clean
