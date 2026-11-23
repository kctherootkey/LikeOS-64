# Suite of the exFAT driver for host/test-fs.sh: reads every image
# variant (cluster sizes 4 KB / 128 KB / 32 MB, contiguous and FAT-chained
# files, many files, deep trees) against the source tree; proves that
# mounting read-only AND read-write, walking and unmounting writes nothing;
# checks the gate (dirty volume, broken main boot region); writes and
# stresses with fsck.exfat -n after each; fuzzes the metadata.
#
# Copyright (C) 2026 The LikeOS Project

suite() {
	local img c v

	for v in exfat-4k-basic exfat-128k-basic exfat-32m-basic exfat-4k-frag \
		exfat-4k-many exfat-128k-many exfat-4k-deep exfat-4k-big exfat-4k-empty; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		[ -f "$img.manifest" ] && tf_case "read-$v" tf_expect_manifest exfat "$img"
		tf_case "trap-$v" tf_trap_walk "$img"
		tf_drop_big "$v" "$img"
	done

	# the gate: a dirty volume or a broken main boot region is not
	# written (degrade to read-only or refuse; never a write)
	for v in exfat-4k-dirty exfat-4k-badboot; do
		img=$(tf_img $v) || { tf_skip $v "image"; continue; }
		tf_case "gate-$v" tf_run "$img" --cow --trap -c \
			"?mount; ?readall; ?mkdir /gate; ?write /gate/f text:x; ?umount; mount ro; =NOT_FOUND stat /gate; umount"
	done

	img=$(tf_img exfat-4k-basic) || return
	c=$(tf_copy "$img")
	tf_case write-basic tf_run "$c" -s "$FSIMG/script-write-basic.txt"
	tf_case write-basic-fsck tf_fsck exfat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case write-names tf_run "$c" -c "mount; write /Grossbuchstaben.TXT text:a; =EXISTS create /grossbuchstaben.txt; write /\xc3\xa4\xc3\xb6\xc3\xbc text:b; =EXISTS create /\xc3\x84\xc3\x96\xc3\x9c; rename /Grossbuchstaben.TXT /GROSSBUCHSTABEN.txt; cycle; ls /"
	tf_case write-names-fsck tf_fsck exfat "$c"
	tf_drop "$c"

	c=$(tf_copy "$img")
	tf_case stress tf_run "$c" -c "mount; stressopt names ci; stressopt utf8 1; stress 51 2000 400"
	tf_case stress-fsck tf_fsck exfat "$c"
	tf_drop "$c"

	img=$(tf_img exfat-4k-frag) || return
	c=$(tf_copy "$img")
	tf_case stress-frag tf_run "$c" -c "mount; stressopt names ci; stress 52 1000 250"
	tf_case stress-frag-fsck tf_fsck exfat "$c"
	tf_drop "$c"

	img=$(tf_img exfat-128k-basic) || return
	c=$(tf_copy "$img")
	tf_case stress-128k tf_run "$c" -c "mount; stressopt names ci; stress 53 800 200"
	tf_case stress-128k-fsck tf_fsck exfat "$c"
	tf_drop "$c"

	img=$(tf_img exfat-4k-basic) || return
	c=$(tf_copy "$img")
	tf_case power-cut tf_run "$c" -c "mount; mkdir /pc; fault cut 40; write /pc/a pat:1:300000; ?write /pc/b pat:2:100000; ?sync; ?umount"
	tf_case power-cut-mount tf_run "$c" --cow -c "?mount; ?readall; ?umount"
	tf_drop "$c"

	tf_case fuzz-ro tf_fuzz exfat "$img" "${TF_FUZZ_ITERS:-300}" 61
	tf_case fuzz-rw tf_fuzz exfat "$img" "${TF_FUZZ_ITERS:-300}" 62 --rw
}
