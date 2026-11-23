# Helpers for the driver suites (host/fsimg/suite-<driver>.sh), sourced by
# host/test-fs.sh after it built the runner ($BIN) and made $TF_TMP.
#
#   tf_img VARIANT            path of a generated image (made once per
#                             suite run with mkimg.sh); status 77 = the
#                             host lacks a tool (use: img=$(tf_img X) || ...)
#   tf_copy IMG               a private writable copy (path printed)
#   tf_run IMG ARGS...        the runner, bounded (timeout, MemoryMax)
#   tf_fsck FSTYPE IMG        oracle.py fsck (0 clean)
#   tf_expect_manifest FSTYPE IMG [RUNNER ARGS...]
#                             runner "mount ro; manifest" compared with
#                             IMG.manifest (or the oracle's view when there
#                             is none / TF_ORACLE_MANIFEST=1)
#   tf_trap_walk IMG [mount options] [RUNNER ARGS...]
#                             mount (ro, then rw), walk, read all, unmount
#                             in write-trap mode: proves zero writes
#   tf_trap_walk_ro IMG [mount options] [RUNNER ARGS...]
#                             the same for read-only mounts only
#   tf_idle_rw IMG [mount options]
#                             second read-write mount + walk: zero writes
#   tf_fuzz FSTYPE IMG ITERS SEED [fuzz.py options]
#   tf_drop IMG               delete an image (and its .manifest/.info)
#
# Copyright (C) 2026 The LikeOS Project

FSIMG="$ROOT/host/fsimg"
TF_IMGDIR="$TF_TMP/img"
mkdir -p "$TF_IMGDIR"

tf_img() {
	local v=$1 out="$TF_IMGDIR/$1.img" rc
	if [ ! -f "$out" ]; then
		TMPDIR="$TF_TMP" bash "$FSIMG/mkimg.sh" "$v" "$out" > "$TF_TMP/mkimg-$v.log" 2>&1
		rc=$?
		if [ $rc -ne 0 ]; then
			cat "$TF_TMP/mkimg-$v.log" >&2
			rm -f "$out"
			return $rc
		fi
	fi
	echo "$out"
}

tf_copy() {
	local c="$TF_TMP/copy-$(basename "$1")-$RANDOM"
	cp --sparse=always "$1" "$c" || return 1
	[ -f "$1.manifest" ] && cp "$1.manifest" "$c.manifest"
	echo "$c"
}

tf_drop() {
	rm -f "$1" "$1.manifest" "$1.info"
}

# Large images are not kept for later cases: tf_drop_big VARIANT IMG
tf_drop_big() {
	case $1 in
	*-32m-*|*-big) tf_drop "$2" ;;
	esac
}

tf_run() {
	tf_bin "$@"
}

tf_fsck() {
	python3 "$FSIMG/oracle.py" fsck "$@"
}

tf_sector() {
	# the sector size an image's .info names (512 default)
	local s
	s=$(awk '{print $2}' "$1.info" 2>/dev/null)
	echo "${s:-512}"
}

tf_expect_manifest() {
	local fst=$1 img=$2
	shift 2
	local exp="$TF_TMP/exp.$$" act="$TF_TMP/act.$$"
	if [ -f "$img.manifest" ] && [ "${TF_ORACLE_MANIFEST:-0}" != 1 ]; then
		cp "$img.manifest" "$exp"
	else
		python3 "$FSIMG/oracle.py" manifest "$fst" "$img" > "$exp" || return 1
	fi
	tf_run "$img" --sector "$(tf_sector "$img")" "$@" -c "mount ro; manifest; umount" > "$act" || {
		cat "$act"
		return 1
	}
	python3 "$FSIMG/mktree.py" compare "$exp" "$act" ${TF_COMPARE_OPTS:-}
}

tf_trap_walk() {
	local img=$1 opts=${2:-}
	shift
	[ $# -gt 0 ] && shift
	local sec
	sec=$(tf_sector "$img")
	tf_run "$img" --cow --trap --sector "$sec" "$@" -c \
		"mount ro $opts; readall; statfs; umount; mount rw $opts; readall; ?statfs; ls /; umount; mount rw $opts; cycle; umount"
}

# Write-trap walk of a read-only mount only (drivers that may write on a
# read-write mount by design, like the ext4 journal upgrade).
tf_trap_walk_ro() {
	local img=$1 opts=${2:-}
	shift
	[ $# -gt 0 ] && shift
	tf_run "$img" --cow --trap --sector "$(tf_sector "$img")" "$@" -c \
		"mount ro $opts; readall; statfs; umount; mount ro $opts; ls /; umount"
}

# An idle read-write mount after a first one: no writes at all.
tf_idle_rw() {
	local img=$1 opts=${2:-}
	tf_run "$img" --cow --sector "$(tf_sector "$img")" -c \
		"mount rw $opts; umount; trap on; mount rw $opts; readall; statfs; umount"
}

tf_fuzz() {
	local fst=$1 img=$2 iters=$3 seed=$4
	shift 4
	python3 "$FSIMG/fuzz.py" --bin "$BIN" --fstype "$fst" --image "$img" \
		--iters "$iters" --seed "$seed" --regress "${TF_REGRESS:-$FSIMG/regress}" "$@"
}
