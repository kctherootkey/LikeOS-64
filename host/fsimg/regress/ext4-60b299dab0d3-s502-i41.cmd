fstype=ext4
base=base-ext4-60b299dab0d3.img.xz
sector=512
why=finding: beyond device end
cmds=?mount; ?readall; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; ?append /fz_dir/f pat:8:5000; ?rename /fz_dir/f /fz_dir/g; ?unlink /fz_dir/g; ?rmdir /fz_dir; ?readall; ?umount; ?mount ro; ?readall; ?umount
