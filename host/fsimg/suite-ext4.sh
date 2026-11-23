# Suite of the ext4 driver for host/test-fs.sh (acceptance test of the
# harness itself: an existing driver on images made by mkfs.ext4).
#
# Copyright (C) 2026 The LikeOS Project

suite() {
	local img c v

	for v in ext4-basic ext4-1k ext4-many ext4-deep; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		tf_case "read-$v" tf_expect_manifest ext4 "$img"
		# a first rw mount upgrades the journal to csum-v3 (a write by
		# design): the trap covers ro mounts and later idle rw mounts
		tf_case "trap-ro-$v" tf_trap_walk_ro "$img"
		tf_case "idle-rw-$v" tf_idle_rw "$img"
	done

	img=$(tf_img ext4-basic) || return
	# 4096-byte sectors under a 4 KB-block filesystem
	tf_case read-sector4096 tf_expect_manifest ext4 "$img" --sector 4096

	c=$(tf_copy "$img")
	tf_case write-basic tf_run "$c" -s "$FSIMG/script-write-basic.txt"
	tf_case write-fsck tf_fsck ext4 "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case write-posix tf_run "$c" -s "$FSIMG/script-write-posix.txt"
	tf_case write-posix-fsck tf_fsck ext4 "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case stress tf_run "$c" -c "mount; stress 11 1500 300"
	tf_case stress-fsck tf_fsck ext4 "$c"
	tf_drop "$c"

	img=$(tf_img ext4-1k) || return
	c=$(tf_copy "$img")
	tf_case stress-1k tf_run "$c" -c "mount; stressopt budget 4M; stress 12 800 200"
	tf_case stress-1k-fsck tf_fsck ext4 "$c"
	tf_drop "$c"

	img=$(tf_img ext4-basic) || return
	tf_case fuzz-ro tf_fuzz ext4 "$img" "${TF_FUZZ_ITERS:-150}" 21
}
