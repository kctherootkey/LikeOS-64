#!/bin/bash
# Host harness for the kernel network stack.
#
# Compiles the REAL kernel/net sources on the host -- twice, as two
# independent stack instances whose global symbols are renamed (n0_*, n1_*)
# -- with AddressSanitizer and UndefinedBehaviorSanitizer, links them to a
# discrete-event simulator with a simulated wire between the two, and runs
# the tests.  See host/net/README.
#
#   host/test-net.sh [all|unit|interop|script|fuzz [N]|bench|drivers|epoll] [FILTER] [opts]
#       all       unit + interop + script tests (default), then epoll
#       epoll     kernel/net/eventpoll.c against test objects
#                 (host/net/epoll), ASan + UBSan
#       drivers   the NIC drivers' pure logic (kernel/dev/nic/nic_util.h):
#                 host/net/drivers, ASan + UBSan
#       fuzz [N]  N mutated frames (default 200000) into the receive path;
#                 --secs T runs for T seconds instead
#       bench     the benchmarks, compared with host/net/baseline.txt
#       bench --record   run them and rewrite host/net/baseline.txt
#   options are passed to the runner: --seed N, --hz N, --pcap DIR, -v,
#   --list, --timeout S, --script FILE (script group: run one file)
#   host/test-net.sh --build   build only, print the binaries
#
# Environment:
#   TESTNET_DIR      build directory (default $TMPDIR/likeos-test-net)
#   TESTNET_MEM      memory limit of a run (default 1500M)
#   TESTNET_TIMEOUT  wall-clock limit of a run in seconds (default 3600)
#   TESTNET_VERBOSE  1: kernel log and test trace to stderr
#   TESTNET_EXCLUDE  more kernel/net sources to leave out
#   TESTNET_REV      test the kernel sources of a git revision (e.g. HEAD)
#                    instead of the working tree; the harness itself is
#                    always the working tree's
# Run from anywhere; builds use -j2 at most.
#
# Copyright (C) 2026 The LikeOS Project

set -u
cd "$(dirname "$0")/.." || exit 2

TESTNET_DIR=${TESTNET_DIR:-${TMPDIR:-/tmp}/likeos-test-net}
TESTNET_MEM=${TESTNET_MEM:-1500M}
TESTNET_TIMEOUT=${TESTNET_TIMEOUT:-3600}
export TESTNET_DIR

# ---- the stack's sources: every .c file under kernel/net except these ----
# dhcp/dns: the glue configures addresses statically; poll/eventpoll/
# syscalls: the descriptor layer (the harness calls the socket layer by
# socket index; epoll has its own host test, host/net/epoll);
# unix_socket: not the IP stack; softirq: the harness pumps softirqs
# deterministically itself (kstubs.c).
EXCLUDE="dhcp.c dns.c poll.c eventpoll.c syscalls.c unix_socket.c softirq.c ${TESTNET_EXCLUDE:-}"
# kernel functions the stack references only from paths the harness never
# runs (sendfile's descriptor and file I/O): stubbed silently, abort if called
QUIET_STUBS="fdget fdput pipe_is_end unix_sock_is vfs_read vfs_write vfs_seek
	vfs_setid_clean strip_setid_file tty_get_console tty_printf tty_write"

# the kernel tree the stack is compiled from
KROOT=.
if [ -n "${TESTNET_REV:-}" ]; then
	REVID=$(git rev-parse --short "$TESTNET_REV") || exit 2
	KROOT="$TESTNET_DIR/tree-$REVID"
	if [ ! -d "$KROOT/kernel" ]; then
		mkdir -p "$KROOT" &&
			git archive "$REVID" kernel | tar -x -C "$KROOT" || exit 2
	fi
fi

net_sources() {
	local f b x skip
	for f in $(find "$KROOT/kernel/net" -name '*.c' | sort); do
		b=$(basename "$f")
		skip=0
		for x in $EXCLUDE; do [ "$b" = "$x" ] && skip=1; done
		[ $skip = 1 ] || echo "$f"
	done
}

SAN="-fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
KWARN="-Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-sign-compare
	-Wno-discarded-qualifiers -Wno-address-of-packed-member -Wno-comment
	-Werror=implicit-function-declaration"
KBASE="-std=gnu11 -g -fno-strict-aliasing -ffreestanding -nostdinc -fno-builtin
	-fno-common -I$KROOT -Ihost/net -D__LIKEOS__ -D__LIKEOS_KERNEL__ -DCRASH_VERBOSE=1
	-DDEBUG=1 -include host/net/prelude.h"
HWARN="-Wall -Wextra -Werror"

# build VARIANT: "san" (ASan+UBSan, -O1: the tests and the fuzzer) or
# "fast" (-O2, no sanitizers: the benchmarks; simulated results are the
# same, only the wall clock differs)
build_variant() {
	local v=$1 bdir opt san bin
	bdir="$TESTNET_DIR/$v${REVID:+-$REVID}"
	case $v in
	san) opt="-O1"; san="$SAN" ;;
	fast) opt="-O2"; san="" ;;
	esac
	bin="$bdir/test-net"
	mkdir -p "$bdir" || return 1
	if [ -x "$bin" ] && [ -z "$(find "$KROOT/kernel" host/net host/test-net.sh \
		-newer "$bin" \( -name '*.[ch]' -o -name '*.sh' \) -print -quit)" ]; then
		echo "$bin"
		return 0
	fi
	echo "test-net: building ($v) ..." >&2
	rm -f "$bdir"/*.o
	local kflags="$KBASE $opt $KWARN $san"
	# the kernel's own formatter, out of console.c
	{
		echo '#include <kernel/io/console.h>'
		echo '#define SIZE_MAX ((size_t)-1)'
		echo 'static void console_putchar_unlocked(char c) { (void)c; }'
		echo '#define va_start(ap, last) __builtin_va_start(ap, last)'
		echo '#define va_arg(ap, type) __builtin_va_arg(ap, type)'
		echo '#define va_end(ap) __builtin_va_end(ap)'
		awk -f host/fsimg/kfmt.awk "$KROOT/kernel/io/console.c"
	} > "$bdir/kfmt.c"
	local srcs f o objs=""
	srcs="$(net_sources) host/net/kstubs.c host/net/glue.c host/net/kt_core.c host/net/kt_ip.c host/net/kt_tcp.c host/net/kt_tcp2.c host/net/kt_tcp3.c $bdir/kfmt.c"
	# compile the instance once (-j2); the object is instantiated twice
	local pids="" fails=0 n=0
	for f in $srcs; do
		o="${f#"$KROOT"/}"
		o="$bdir/k_$(echo "${o%.c}" | tr '/' '_').o"
		objs="$objs $o"
		(
			extra=""
			case $f in host/net/*) extra="-Werror" ;; esac
			nice -n 10 gcc $kflags $extra -c "$f" -o "$o" 2> "$o.log" ||
				{ cat "$o.log" >&2; echo "test-net: $f does not compile" >&2; exit 1; }
			grep -E 'warning|error' "$o.log" | head -20 >&2
			exit 0
		) &
		pids="$pids $!"
		n=$((n + 1))
		if [ $n -ge 2 ]; then
			for p in $pids; do wait "$p" || fails=1; done
			pids="" n=0
		fi
	done
	for p in $pids; do wait "$p" || fails=1; done
	[ $fails = 0 ] || return 1
	ld -r --wrap=skb_alloc --wrap=skb_put -o "$bdir/stack.o" $objs || return 1
	# kernel functions nobody provides: generated stubs that abort
	local undef known sym
	known='^(hs_|__asan|__ubsan|__sanitizer|_GLOBAL_OFFSET_TABLE_$|mem(cpy|set|move|cmp)$|__stack_chk)'
	undef=$(nm -u "$bdir/stack.o" | awk '{print $2}' | grep -Ev "$known" | sort -u)
	{
		echo '/* generated by host/test-net.sh: kernel functions the harness does not provide */'
		echo 'void hs_unstubbed(const char *name) __attribute__((noreturn));'
		for sym in $undef; do
			echo "void $sym(void); void $sym(void) { hs_unstubbed(\"$sym\"); }"
		done
	} > "$bdir/autostub.c"
	local loud=""
	for sym in $undef; do
		case " $(echo $QUIET_STUBS) " in *" $sym "*) ;; *) loud="$loud $sym" ;; esac
	done
	[ -n "$loud" ] && echo "test-net: NOTE: stubbed (abort if called):$loud" >&2
	gcc -std=gnu11 $opt -g -c "$bdir/autostub.c" -o "$bdir/autostub.o" || return 1
	ld -r -o "$bdir/stack-full.o" "$bdir/stack.o" "$bdir/autostub.o" || return 1
	# two instances: rename every defined global symbol per instance
	local i
	nm --defined-only -g "$bdir/stack-full.o" | awk '{print $3}' | sort -u > "$bdir/syms"
	for i in 0 1; do
		awk -v p="n${i}_" '{print $1, p $1}' "$bdir/syms" > "$bdir/rename$i"
		objcopy --redefine-syms="$bdir/rename$i" "$bdir/stack-full.o" "$bdir/inst$i.o" || return 1
	done
	# the host side
	local hobjs=""
	for f in host/net/sim.c host/net/testlib.c host/net/pkt.c host/net/main.c \
		host/net/t_unit.c host/net/t_interop.c host/net/t_script.c \
		host/net/t_fuzz.c host/net/t_bench.c host/net/t_core.c \
		host/net/t_ip.c host/net/t_tcp.c host/net/t_tcp2.c; do
		o="$bdir/h_$(basename "$f" .c).o"
		nice -n 10 gcc -std=gnu11 $opt -g $HWARN $san -Ihost/net -c "$f" -o "$o" || return 1
		hobjs="$hobjs $o"
	done
	gcc $san -o "$bin.tmp" $hobjs "$bdir/inst0.o" "$bdir/inst1.o" -lm || return 1
	mv "$bin.tmp" "$bin"
	echo "$bin"
}

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:handle_abort=1:allocator_may_return_null=1:detect_odr_violation=0:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1:abort_on_error=1}

# One bounded run: memory and wall clock capped, low priority.
bounded() {
	if command -v systemd-run >/dev/null 2>&1 &&
	   systemd-run --user --scope -q -p MemoryMax=1M true >/dev/null 2>&1; then
		timeout -k 10 "$TESTNET_TIMEOUT" systemd-run --user --scope -q \
			-p MemoryMax="$TESTNET_MEM" -p MemorySwapMax=0 -- \
			nice -n 10 "$@"
	else
		timeout -k 10 "$TESTNET_TIMEOUT" nice -n 10 "$@"
	fi
}

MODE=${1:-all}
[ $# -gt 0 ] && shift

case $MODE in
--build)
	build_variant san || exit 2
	build_variant fast || exit 2
	exit 0
	;;
all | unit | interop | script)
	BIN=$(build_variant san) || exit 2
	bounded "$BIN" "$MODE" "$@"
	rc=$?
	if [ "$MODE" = all ] && [ $# -eq 0 ]; then
		"$0" epoll || rc=1
	fi
	exit $rc
	;;
epoll)
	EDIR="$TESTNET_DIR/epoll"
	mkdir -p "$EDIR" || exit 2
	EFLAGS="$KBASE -O1 $KWARN -Wno-unused-parameter $SAN -Werror"
	for f in "$KROOT/kernel/net/eventpoll.c" host/net/epoll/ep_kside.c; do
		nice -n 10 gcc $EFLAGS -c "$f" \
			-o "$EDIR/$(basename "$f" .c).o" || exit 2
	done
	nice -n 10 gcc -std=gnu11 -O1 -g $HWARN $SAN -Ihost/net/epoll \
		-c host/net/epoll/test_epoll.c -o "$EDIR/test_epoll.o" || exit 2
	gcc $SAN -o "$EDIR/test-epoll" "$EDIR/test_epoll.o" \
		"$EDIR/eventpoll.o" "$EDIR/ep_kside.o" || exit 2
	bounded "$EDIR/test-epoll" "$@"
	exit $?
	;;
drivers)
	DDIR="$TESTNET_DIR/drivers"
	mkdir -p "$DDIR" || exit 2
	nice -n 10 gcc -std=gnu11 -O1 -g $HWARN $SAN -DNIC_UTIL_HOST -I. \
		host/net/drivers/test_drivers.c -o "$DDIR/test-drivers" || exit 2
	bounded "$DDIR/test-drivers" "$@"
	exit $?
	;;
fuzz)
	BIN=$(build_variant san) || exit 2
	ITERS=200000
	case ${1:-} in [0-9]*) ITERS=$1; shift ;; esac
	bounded "$BIN" fuzz --iters "$ITERS" "$@"
	exit $?
	;;
bench)
	BIN=$(build_variant fast) || exit 2
	RECORD=0
	ARGS=()
	SEED=
	prev=
	for a in "$@"; do
		if [ "$a" = --record ]; then RECORD=1; else ARGS+=("$a"); fi
		[ "$prev" = --seed ] && SEED=$a
		prev=$a
	done
	OUT="$TESTNET_DIR/bench-results.txt"
	rm -f "$OUT"
	bounded "$BIN" bench --out "$OUT" "${ARGS[@]}" || exit 1
	if [ $RECORD = 1 ]; then
		{
			echo "# LikeOS network stack benchmark baseline (host/test-net.sh bench --record)"
			echo "# Copyright (C) 2026 The LikeOS Project"
			if [ -n "${TESTNET_REV:-}" ]; then
				echo "# kernel sources: git $(git rev-parse --short "$TESTNET_REV") ($TESTNET_REV), recorded $(date -u +%Y-%m-%dT%H:%MZ)"
			else
				echo "# kernel sources: working tree at $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- kernel 2>/dev/null || echo ' + uncommitted kernel changes'), recorded $(date -u +%Y-%m-%dT%H:%MZ)"
			fi
			echo "# seed ${SEED:-1}, tick rate 200 Hz; both ends are this stack (instance 0 client, 1 servers)"
			echo "# simulated time, deterministic: same sources + seed = same numbers"
			echo "# key value  (lower is better: *_ms, *_s, retrans, stalls, rebuffers; higher is better: *_mbps, *_pct)"
			cat "$OUT"
		} > host/net/baseline.txt
		echo "test-net: baseline written to host/net/baseline.txt"
		exit 0
	fi
	if [ -f host/net/baseline.txt ]; then
		awk '
		FNR == NR { if ($0 !~ /^#/ && NF >= 2) base[$1] = $2; next }
		$0 ~ /^#/ || NF < 2 { next }
		{
			k = $1; v = $2
			if (!(k in base)) { printf "%-44s %12s   (new)\n", k, v; next }
			b = base[k]; d = v - b
			pct = (b != 0) ? sprintf("%+.1f%%", 100 * d / b) : (d == 0 ? "0" : "n/a")
			better = (k ~ /(mbps|_pct)$/) ? (d > 0) : (d < 0)
			rel = (b != 0) ? d / b : d
			if (rel < 0) rel = -rel
			mark = (rel < 0.02) ? "" : (better ? "  better" : "  WORSE")
			printf "%-44s %12s   baseline %12s   %8s%s\n", k, v, b, pct, mark
		}' host/net/baseline.txt "$OUT"
	else
		cat "$OUT"
		echo "test-net: no host/net/baseline.txt yet (bench --record writes it)"
	fi
	exit 0
	;;
*)
	echo "usage: host/test-net.sh [all|unit|interop|script|fuzz [N]|bench [--record]|drivers|epoll|--build] [FILTER] [options]" >&2
	exit 2
	;;
esac
