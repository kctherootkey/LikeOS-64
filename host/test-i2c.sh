#!/bin/sh
# Host test of the I2C core (kernel/dev/i2c: adapters, clients, drivers,
# ACPI enumeration, SMBus, /dev/i2c-N) and the DesignWare controller
# driver with its PCI and ACPI glue (kernel/dev/i2c/busses): the real
# sources, the real ACPI device layer and work queues, over a register
# and bus model of the controller (host/i2c/model.c), models of the
# interrupt core, PCI, devfs, user memory and gpiolib (host/i2c/mocks.c),
# of the firmware bus arbitration hardware (the IOSF sideband / P-Unit and
# the AMD PSP, host/i2c/arb.c) and of the ACPICA namespace (host/kshim),
# with AddressSanitizer and
# UndefinedBehaviorSanitizer.  Run from the repository root, through
# tmp/boxed.sh.  Arguments: an optional test name filter ("all" or
# nothing: every test), then extra
# compiler options after "--".
#
# Copyright (C) 2026 The LikeOS Project

set -e
cd "$(dirname "$0")/.."
FILTER=""
if [ $# -gt 0 ] && [ "$1" != "--" ]; then
	FILTER="$1"
	shift
fi
[ "$1" = "--" ] && shift
[ "$FILTER" = "all" ] && FILTER=
TMP=${TMPDIR:-/tmp}/likeos-test-i2c.$$
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"
SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KFLAGS="-std=gnu11 -O1 -g -ffreestanding -nostdinc -fno-builtin -I.
	-Ikernel/hal/acpica/include -D__LIKEOS__ -D__LIKEOS_KERNEL__
	-DACPI_USE_BUILTIN_STDARG -U__linux__ -U_LINUX -Ulinux -DKSHIM_REAL_PCI
	-Wall -Wextra -Werror -Wno-unused-function -Wno-sign-compare $SAN
	-include host/kshim/prelude.h -include host/i2c/model.h
	-include host/i2c/arb.h"
MMIO="-Ddw_mmio_readl(a)=model_readl(a) -Ddw_mmio_writel(v,a)=model_writel((v),(a))
	-Ddw_mmio_readw(a)=model_readw(a) -Ddw_mmio_writew(v,a)=model_writew((v),(a))
	-Dpsp_mmio_readl(a)=arb_psp_readl(a) -Dpsp_mmio_writel(v,a)=arb_psp_writel((v),(a))
	-Dpsp_dma_alloc=arb_psp_dma_alloc -DPSP_CMD_TIMEOUT_US=20000
	-DPSP_I2C_REQ_RETRY_CNT=10 -DPSP_I2C_REQ_RETRY_DELAY_US=2000"
SRCS="kernel/dev/i2c/i2c-core-base.c kernel/dev/i2c/i2c-core-acpi.c
      kernel/dev/i2c/i2c-core-smbus.c kernel/dev/i2c/i2c-dev.c
      kernel/dev/i2c/busses/i2c-designware-common.c
      kernel/dev/i2c/busses/i2c-designware-master.c
      kernel/dev/i2c/busses/i2c-designware-pcidrv.c
      kernel/dev/i2c/busses/i2c-designware-platdrv.c
      kernel/dev/i2c/busses/i2c-designware-baytrail.c
      kernel/dev/i2c/busses/i2c-designware-amdpsp.c
      kernel/hal/iosf_mbi.c kernel/hal/amd_psp.c kernel/hal/dmi.c
      kernel/hal/acpi_dev.c kernel/ke/workqueue.c
      host/kshim/kshim.c host/kshim/acpica_mock.c
      host/i2c/model.c host/i2c/mocks.c host/i2c/arb.c host/i2c/test_i2c.c"
for f in $SRCS; do
	timeout 300 nice -n 10 cc $KFLAGS $MMIO "-DTEST_FILTER=\"$FILTER\"" $* \
		-c "$f" -o "$TMP/$(basename "$f" .c).o"
done
timeout 300 nice -n 10 cc -std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN \
	-c host/kshim/hrt.c -o "$TMP/hrt.o"
timeout 300 cc $SAN -pthread -rdynamic -o "$TMP/test-i2c" "$TMP"/*.o
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
timeout 600 nice -n 10 "$TMP/test-i2c"
