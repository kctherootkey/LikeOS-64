# Suite of the ISO 9660 driver for host/test-fs.sh: reads every image
# variant and compares with the source tree (Rock Ridge) or with the
# oracle's direct reading (Joliet, plain 9660), on 2048- and 512-byte
# sector devices; proves that mounting and reading writes nothing; fuzzes
# the metadata.  TF_BIG=1 adds the > 4 GB multi-extent image (~9 GB free).
#
# Copyright (C) 2026 The LikeOS Project

suite() {
	local img v s

	for v in iso-rrj iso-rr iso-zisofs iso-deep iso-deeprel iso-many iso-hybrid; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		for s in 2048 512; do
			tf_case "read-$v-$s" tf_expect_manifest iso "$img" --sector $s
		done
		tf_case "trap-$v" tf_run "$img" --cow --trap --sector 2048 -c \
			"mount ro; readall; statfs; umount; ?mount rw; ?readall; ?umount; mount ro; ls /; umount"
	done
	for v in iso-joliet iso-plain; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		TF_ORACLE_MANIFEST=1 tf_case "read-$v" tf_expect_manifest $v "$img" --sector 2048
		tf_case "trap-$v" tf_run "$img" --cow --trap --sector 2048 -c "mount ro; readall; umount"
	done

	img=$(tf_img iso-rrj) || return
	# Rock Ridge off: the Joliet names; Joliet off too: plain names
	tf_case opt-norock tf_run "$img" --sector 2048 -c "mount ro norock; readall; umount"
	tf_case opt-nojoliet tf_run "$img" --sector 2048 -c "mount ro norock,nojoliet; readall; umount"
	tf_case opt-uid tf_run "$img" --sector 2048 -c \
		"mount ro uid=1000,gid=1000,mode=0640,dmode=0750; stat /size_000001_01.bin; umount"
	tf_case read-chunks tf_run "$img" --sector 2048 --chunk 3000 -c "mount ro; readall; umount"
	tf_case statfs tf_run "$img" --sector 2048 -c "mount ro; statfs; umount"
	tf_case readonly-ops tf_run "$img" --sector 2048 --cow --trap -c \
		"mount ro; rawro on; ?mkdir /x; ?unlink /size_000001_01.bin; ?write /n text:x; umount"

	if [ "${TF_BIG:-0}" = 1 ]; then
		img=$(tf_img iso-big) && {
			tf_case multi-extent tf_run "$img" --sector 2048 -c \
				"mount ro; stat /huge.bin; pread /huge.bin 0 13; pread /huge.bin 4294967293 6; pread /huge.bin 4613734389 11; umount"
			tf_drop "$img"
		}
	fi

	img=$(tf_img iso-rrj) || return
	tf_case fuzz tf_fuzz iso "$img" "${TF_FUZZ_ITERS:-300}" 41
	img=$(tf_img iso-zisofs) || return
	tf_case fuzz-zisofs tf_fuzz iso "$img" "${TF_FUZZ_ITERS:-300}" 42
	img=$(tf_img iso-deeprel) || return
	tf_case fuzz-deeprel tf_fuzz iso "$img" "${TF_FUZZ_ITERS:-300}" 43
}
