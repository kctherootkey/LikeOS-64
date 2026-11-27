#!/bin/sh
# Host test of the GPIO core, its ACPI glue and the GPIO / pin controller
# drivers (kernel/dev/gpio, kernel/dev/gpio/pinctrl): the real driver
# sources, the real ACPI device layer, platform identification and work
# queues, over a register model of the controllers
# (host/pinctrl/regmodel.c), a model of the interrupt core and of the
# ACPICA namespace (host/kshim), with AddressSanitizer and
# UndefinedBehaviorSanitizer.  Run from the repository root, through
# tmp/boxed.sh.  Extra compiler options may be given as arguments.
#
# Copyright (C) 2026 The LikeOS Project

set -e
cd "$(dirname "$0")/.."
TMP=${TMPDIR:-/tmp}/likeos-test-pinctrl.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -ffreestanding -nostdinc -fno-builtin -I.
	-Ikernel/hal/acpica/include -D__LIKEOS__ -D__LIKEOS_KERNEL__
	-DACPI_USE_BUILTIN_STDARG -U__linux__ -U_LINUX -Ulinux
	-Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare $SAN
	-include host/kshim/prelude.h -include host/pinctrl/regmodel.h"
SRCS="kernel/dev/gpio/gpiolib.c kernel/dev/gpio/gpiolib-acpi.c
      kernel/dev/gpio/gpiolib-acpi-quirks.c kernel/hal/dmi.c
      kernel/hal/acpi_dev.c kernel/ke/workqueue.c
      host/kshim/kshim.c host/kshim/acpica_mock.c host/kshim/irq_model.c
      host/pinctrl/regmodel.c host/pinctrl/test_pinctrl.c
      host/pinctrl/test_atom_amd.c
      $(ls kernel/dev/gpio/pinctrl/intel/pinctrl-*.c | grep -v 'baytrail\|cherryview')"
for f in $SRCS; do
	timeout 300 nice -n 10 cc $KFLAGS "-Dpinctrl_readl(a)=model_readl(a)" \
		"-Dpinctrl_writel(v,a)=model_writel((v),(a))" \
		"-Dlp_ioread32(a)=model_readl((void *)(a))" \
		"-Dlp_iowrite32(v,a)=model_writel((v),(void *)(a))" $* \
		-c "$f" -o "$TMP/$(basename "$f" .c).o"
done
timeout 300 nice -n 10 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN \
	-c host/kshim/hrt.c -o "$TMP/hrt.o"
timeout 300 cc $SAN -pthread -rdynamic -o "$TMP/test-pinctrl" "$TMP"/*.o
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
timeout 300 nice -n 10 "$TMP/test-pinctrl"
