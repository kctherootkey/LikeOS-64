# Suite of the NTFS driver for host/test-fs.sh: reads every image variant
# (cluster sizes 512 / 4 KB / 64 KB; compressed, sparse, ADS, EA, reparse
# points, many files with index allocation, deep trees, big files) and
# compares with what ntfs-3g's own tools read; proves that mounting read-
# only AND read-write, walking and unmounting writes nothing, including
# dirty, hibernated, unclean-$LogFile and $MFTMirr-mismatch volumes (the
# rw gate); writes and stresses with ntfsfix -n and an ntfs-3g read-back
# after each; fuzzes the metadata.
#
# Copyright (C) 2026 The LikeOS Project

ntfs_readback() {
	# the driver's view after a remount equals ntfs-3g's
	local img=$1 exp="$TF_TMP/nr-exp.$$" act="$TF_TMP/nr-act.$$"
	python3 "$FSIMG/oracle.py" manifest ntfs "$img" > "$exp" || return 1
	tf_run "$img" -c "mount ro; manifest; umount" > "$act" || return 1
	python3 "$FSIMG/mktree.py" compare "$exp" "$act"
}

ntfs_ads() {
	local img=$1 want got
	want=$(ntfscat -a 0x80 -n stream.one "$img" /extra/plain 2>/dev/null | sha256sum | cut -d' ' -f1)
	got=$(tf_run "$img" -c "mount ro; sha /extra/plain:stream.one; umount" 2>/dev/null | awk '/^sha /{print $2}')
	echo "ntfs-3g $want driver $got"
	[ -n "$want" ] && [ "$want" = "$got" ]
}

suite() {
	local img c v

	for v in ntfs-512-basic ntfs-4k-basic ntfs-64k-basic ntfs-4k-full \
		ntfs-4k-many ntfs-4k-deep ntfs-4k-big ntfs-4k-empty; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		TF_ORACLE_MANIFEST=1 tf_case "read-$v" tf_expect_manifest ntfs "$img"
		tf_case "trap-$v" tf_trap_walk "$img"
	done
	img=$(tf_img ntfs-4k-full) || return
	tf_case read-ads ntfs_ads "$img"
	tf_case read-xattr tf_run "$img" -c "mount ro; listxattr /extra/plain; umount"
	tf_case read-sector4096 tf_run "$img" --sector 4096 -c "mount ro; readall; umount"

	# the read-write gate: never a write on these volumes
	for v in ntfs-4k-dirty ntfs-4k-hiber ntfs-4k-logdirty ntfs-4k-mirrbad; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		tf_case "gate-$v" tf_run "$img" --cow --trap -c \
			"mount; readall; ?mkdir /gate; ?write /gate/f text:x; umount; mount ro; =NOT_FOUND stat /gate; umount"
	done

	img=$(tf_img ntfs-4k-basic) || return
	c=$(tf_copy "$img")
	tf_case write-basic tf_run "$c" -s "$FSIMG/script-write-basic.txt"
	tf_case write-basic-fsck tf_fsck ntfs "$c"
	tf_case write-basic-readback ntfs_readback "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case write-posix tf_run "$c" -s "$FSIMG/script-write-posix.txt"
	tf_case write-posix-fsck tf_fsck ntfs "$c"
	tf_drop "$c"

	for v in ntfs-4k-basic ntfs-512-basic ntfs-64k-basic ntfs-4k-full; do
		img=$(tf_img $v) || continue
		c=$(tf_copy "$img")
		tf_case "stress-$v" tf_run "$c" -c "mount; stressopt utf8 1; stress 71 1500 300"
		tf_case "stress-$v-fsck" tf_fsck ntfs "$c"
		tf_case "stress-$v-readback" ntfs_readback "$c"
		tf_drop "$c"
	done

	img=$(tf_img ntfs-4k-basic) || return
	c=$(tf_copy "$img")
	tf_case discard tf_run "$c" --log -c "mount discard; write /d pat:1:1m; sync; unlink /d; sync; umount; log dump"
	tf_case power-cut tf_run "$c" -c "mount; mkdir /pc; fault cut 60; write /pc/a pat:1:300000; ?write /pc/b pat:2:100000; ?sync; ?umount"
	tf_case power-cut-mount tf_run "$c" --cow -c "?mount; ?readall; ?umount"
	tf_drop "$c"

	img=$(tf_img ntfs-4k-full) || return
	tf_case fuzz-ro tf_fuzz ntfs "$img" "${TF_FUZZ_ITERS:-300}" 81
	tf_case fuzz-rw tf_fuzz ntfs "$img" "${TF_FUZZ_ITERS:-300}" 82 --rw
}
