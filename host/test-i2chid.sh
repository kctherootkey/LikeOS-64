#!/bin/bash
# Host harness for the I2C-HID transport (kernel/dev/hid/i2c-hid) together
# with everything around it, all of it the real kernel sources:
#
#   bus world   the I2C core, the DesignWare driver and its PCI glue, the
#               ACPI device layer, the DMI matcher, the work queues -- over
#               the DesignWare register/bus model and the interrupt, PCI,
#               GPIO and ACPICA namespace models of the I2C harness
#               (host/i2c, host/kshim), with an I2C-HID device model on the
#               bus (host/i2chid/hidm.c) and HP- and Dell-shaped machines
#               (host/i2chid/kworld.c);
#   HID world   the I2C-HID transport, the HID core, hid-multitouch, the
#               input core, the event devices and the console pointer
#               bridge -- over the input and HID harnesses' services
#               (host/input, host/hid, host/i2chid/hworld.c), driven by
#               host/i2chid/tests.c.
#
# The two worlds stub the kernel's locking headers differently, so the bus
# world is linked into one relocatable object whose symbols are made local
# except its boundary (host/i2chid/kworld.h); the transport's calls into it
# are renamed to the boundary's wrappers at compile time.  Everything runs
# under AddressSanitizer and UndefinedBehaviorSanitizer.
#
#   host/test-i2chid.sh [GROUP]    run all groups, or one of:
#                                  probe protocol faults openclose pm
#                                  suspendtest remove mousedev ps2pref
#                                  stress
#   host/test-i2chid.sh --build    build only, print the binary
#
# Environment:
#   TESTI2CHID_DIR           build directory (default $TMPDIR/likeos-test-i2chid)
#   TESTI2CHID_STRESS_SECS   length of the stress phase (default 20)
#   TESTINPUT_VERBOSE        1: the HID world's kernel log to stderr
# Run it boxed (tmp/boxed.sh), from anywhere.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2

D=${TESTI2CHID_DIR:-${TMPDIR:-/tmp}/likeos-test-i2chid}
BIN="$D/test-i2chid"
mkdir -p "$D/k" "$D/h" || exit 2

SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"

# ---- the bus world (the I2C harness's flags) ----
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
KSRCS="kernel/dev/i2c/i2c-core-base.c kernel/dev/i2c/i2c-core-acpi.c
       kernel/dev/i2c/i2c-core-smbus.c kernel/dev/i2c/i2c-dev.c
       kernel/dev/i2c/busses/i2c-designware-common.c
       kernel/dev/i2c/busses/i2c-designware-master.c
       kernel/dev/i2c/busses/i2c-designware-pcidrv.c
       kernel/dev/i2c/busses/i2c-designware-platdrv.c
       kernel/dev/i2c/busses/i2c-designware-baytrail.c
       kernel/dev/i2c/busses/i2c-designware-amdpsp.c
       kernel/hal/iosf_mbi.c kernel/hal/amd_psp.c
       kernel/hal/acpi_dev.c kernel/hal/dmi.c kernel/ke/workqueue.c
       host/kshim/kshim.c host/kshim/acpica_mock.c
       host/i2c/model.c host/i2c/mocks.c host/i2c/arb.c
       host/i2chid/kworld.c host/i2chid/hidm.c"

# ---- the HID world (the HID harness's flags) ----
HFLAGS_K="-std=gnu11 -O1 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-I. -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1 -DDEBUG=1
	-Wall -Wextra -Wno-unused-function -Wno-unused-parameter
	-Werror=implicit-function-declaration
	$SAN -include host/input/prelude.h"
# The transport's calls into the bus world.
RENAME="-Di2c_transfer=kwb_i2c_transfer
	-Di2c_transfer_buffer_flags=kwb_i2c_transfer_buffer_flags
	-Di2c_smbus_read_byte=kwb_i2c_smbus_read_byte
	-Di2c_register_driver=kwb_i2c_register_driver
	-Di2c_del_driver=kwb_i2c_del_driver
	-Di2c_client_devpath=kwb_i2c_client_devpath
	-Drequest_threaded_irq=kwb_request_threaded_irq
	-Dfree_irq=kwb_free_irq -Ddisable_irq=kwb_disable_irq
	-Ddisable_irq_nosync=kwb_disable_irq_nosync
	-Denable_irq=kwb_enable_irq -Dsynchronize_irq=kwb_synchronize_irq
	-Dirq_get_trigger_type=kwb_irq_get_trigger_type
	-Dirq_set_irq_type=kwb_irq_set_irq_type
	-Dirq_set_irq_wake=kwb_irq_set_irq_wake
	-Dacpi_uuid_parse=kwb_acpi_uuid_parse
	-Dacpi_evaluate_dsm_typed=kwb_acpi_evaluate_dsm_typed
	-Dacpi_value_free=kwb_acpi_value_free
	-Dacpi_match_device_ids=kwb_acpi_match_device_ids
	-Dacpi_evaluate_integer=kwb_acpi_evaluate_integer
	-Dacpi_evaluate=kwb_acpi_evaluate
	-Dacpi_device_set_power=kwb_acpi_device_set_power
	-Dacpi_dev_get=kwb_acpi_dev_get -Dacpi_dev_put=kwb_acpi_dev_put
	-Ddmi_first_match=kwb_dmi_first_match"
HSRCS_T="kernel/dev/hid/i2c-hid/i2c-hid-core.c kernel/dev/hid/i2c-hid/i2c-hid-acpi.c
	 kernel/dev/hid/i2c-hid/i2c-hid-dmi-quirks.c"
HSRCS="kernel/dev/hid/core/hid-core.c kernel/dev/hid/core/hid-input.c
       kernel/dev/hid/core/hid-quirks.c kernel/dev/hid/hid-multitouch.c
       kernel/dev/input/input.c kernel/dev/input/input-mt.c
       kernel/dev/input/evdev.c kernel/dev/input/mousedev.c
       kernel/dev/input/keyboard.c kernel/dev/input/keymap.c
       host/input/kstubs.c host/input/kbstubs.c
       host/hid/wqshim.c host/i2chid/hworld.c
       host/i2chid/tests.c"
HOSTFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror $SAN"

compile() { # flags... -- file out
	local args=() f o
	while [ "$1" != "--" ]; do args+=("$1"); shift; done
	shift
	f=$1 o=$2
	timeout 600 nice -n 10 gcc ${args[@]} -c "$f" -o "$o" 2> "$o.log" || {
		cat "$o.log" >&2
		echo "test-i2chid: $f does not compile" >&2
		return 1
	}
	if grep -q 'warning:' "$o.log"; then
		cat "$o.log" >&2
		case $f in
		kernel/*) echo "test-i2chid: warnings in $f" >&2; return 1 ;;
		esac
	fi
}

build() {
	local f kobjs="" hobjs="" syms undef

	if [ -x "$BIN" ] && [ -z "$(find kernel/dev/hid kernel/dev/input \
		kernel/dev/i2c kernel/hal/acpi_dev.c kernel/hal/acpi_dev.h \
		kernel/hal/dmi.c kernel/hal/dmi.h kernel/uapi kernel/ke \
		host/input host/hid host/i2c host/kshim host/i2chid \
		host/test-i2chid.sh -newer "$BIN" -print -quit)" ]; then
		return 0
	fi
	echo "test-i2chid: building ..." >&2
	for f in $KSRCS; do
		compile $KFLAGS $MMIO -- "$f" "$D/k/$(basename "$f" .c).o" || return 1
		kobjs="$kobjs $D/k/$(basename "$f" .c).o"
	done
	timeout 600 gcc $HOSTFLAGS -c host/kshim/hrt.c -o "$D/k/hrt.o" || return 1
	kobjs="$kobjs $D/k/hrt.o"
	# One relocatable object; only the boundary stays global.
	ld -r -o "$D/kworld-all.o" $kobjs || return 1
	nm -g --defined-only "$D/kworld-all.o" | awk '{print $3}' |
		grep -E '^(kwb_|kw_|hidm_)' > "$D/kworld.syms"
	objcopy --keep-global-symbols="$D/kworld.syms" "$D/kworld-all.o" \
		"$D/kworld.o" || return 1
	# Whatever the bus world still needs must come from the C library or
	# the sanitizer runtime, never from the HID world.
	undef=$(nm -u "$D/kworld.o" | awk '{print $2}' |
		grep -vE '^(__asan|__ubsan|__sanitizer|__lsan|_GLOBAL_OFFSET_TABLE_|__stack_chk|__[a-z_]+_chk$)' |
		grep -vE '^(pthread_|printf|vsnprintf|vfprintf|fprintf|fflush|calloc|free|malloc|abort|exit|_exit|sched_yield|usleep|nanosleep|clock_gettime|rand|srand|rand_r|backtrace|backtrace_symbols_fd|stderr|stdout|memset|memcpy|memmove|strlen|fputs|puts|putchar|fwrite|getenv|__errno_location|sigaction|raise|time)' || true)
	if [ -n "$undef" ]; then
		echo "test-i2chid: the bus world needs symbols it does not define:" >&2
		echo "$undef" >&2
		return 1
	fi
	for f in $HSRCS_T; do
		compile $HFLAGS_K $RENAME -- "$f" "$D/h/$(basename "$f" .c).o" || return 1
		hobjs="$hobjs $D/h/$(basename "$f" .c).o"
	done
	for f in $HSRCS; do
		compile $HFLAGS_K -- "$f" "$D/h/$(basename "$f" .c).o" || return 1
		hobjs="$hobjs $D/h/$(basename "$f" .c).o"
	done
	timeout 600 gcc $HOSTFLAGS -c host/input/hostlib.c -o "$D/h/hostlib.o" || return 1
	timeout 600 gcc $SAN -pthread -rdynamic -o "$BIN.tmp" $hobjs \
		"$D/h/hostlib.o" "$D/kworld.o" || return 1
	mv "$BIN.tmp" "$BIN"
}

build || exit 1
if [ "${1:-}" = "--build" ]; then
	echo "$BIN"
	exit 0
fi

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:handle_abort=1:allocator_may_return_null=1:exitcode=4:detect_stack_use_after_return=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}
ulimit -c 0
timeout --kill-after=10 1200 "$BIN" "${1:-all}"
