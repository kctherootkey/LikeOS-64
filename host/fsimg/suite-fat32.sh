# Suite of the FAT driver for host/test-fs.sh: FAT32 images made by
# mkfs.fat + mtools read against their source trees, zero-write proofs for
# read-only and idle read-write mounts, write workloads (each followed by
# fsck.fat -n), the stress model and fuzzing.  Cases also cover the volume
# label (never listed as a file), non-Latin-1 names kept whole, short names
# in big directories, seeking at the end of a file, rename over a file and
# zeros after a truncate down and up.  FAT12/FAT16, power cuts, fault
# injection and the rest: host/test-fat32.sh.
#
# Copyright (C) 2026 The LikeOS Project

suite() {
	local img c v

	img=$(tf_img fat-deep) || return
	tf_case read-fat-deep tf_expect_manifest fat "$img"
	tf_case trap-fat-deep tf_trap_walk "$img"

	img=$(tf_img fat-basic) || return
	tf_case read-fat-basic tf_expect_manifest fat "$img"
	tf_case trap-fat-basic tf_trap_walk "$img"

	img=$(tf_img fat-many) || return
	tf_case read-fat-many tf_expect_manifest fat "$img"
	tf_case trap-fat-many tf_trap_walk "$img"

	# reads through the escaped names of the runner
	img=$(tf_img fat-basic) || return
	tf_case read-ascii tf_run "$img" --chunk 4096 -c "mount ro; sha /size_300000_22.bin; sha /dir0/with\x20space/and.dots.in.name; umount"

	c=$(tf_copy "$img")
	tf_case write-basic tf_run "$c" -s "$FSIMG/script-write-basic.txt"
	tf_case write-basic-fsck tf_fsck fat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case write-sequential tf_run "$c" --chunk 100000000 -c \
		"mount; mkdir /s; write /s/a pat:1:300000; write /s/b pat:2:4096; unlink /s/b; cycle; check /s/a pat:1:300000; =NOT_FOUND stat /s/b"
	tf_case write-sequential-fsck tf_fsck fat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case rename-replace tf_run "$c" -c "mount; write /aa text:A; write /bb text:B; rename /aa /bb; check /bb text:A; =NOT_FOUND stat /aa; cycle; check /BB text:A; =NOT_FOUND stat /aa"
	tf_case rename-replace-fsck tf_fsck fat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case truncate-extend tf_run "$c" --chunk 100000000 -c "mount; write /t pat:1:8192; truncate /t 100; truncate /t 8192; zeros /t 100 8092; cycle; zeros /t 100 8092; truncate /t 100; check /t pat:1:100"
	tf_case truncate-extend-fsck tf_fsck fat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case stress tf_run "$c" -c "mount; stressopt names dos; stress 13 1000 250"
	tf_case stress-fsck tf_fsck fat "$c"
	tf_drop "$c"

	tf_case idle-rw tf_idle_rw "$img"

	tf_case fuzz-ro tf_fuzz fat "$img" "${TF_FUZZ_ITERS:-150}" 37
	tf_case fuzz-rw tf_fuzz fat "$img" "${TF_FUZZ_ITERS:-150}" 31 --rw
}
