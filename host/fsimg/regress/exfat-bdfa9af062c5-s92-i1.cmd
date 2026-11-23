fstype=exfat
base=base-exfat-bdfa9af062c5.img.xz
sector=512
why=finding: unmount left
cmds=?mount; ?readall; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; ?append /fz_dir/f pat:8:5000; ?rename /fz_dir/f /fz_dir/g; ?unlink /fz_dir/g; ?rmdir /fz_dir; ?readall; ?umount; ?mount ro; ?readall; ?umount
