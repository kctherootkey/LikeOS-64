fstype=fat
base=base-fat-ac224b967d88.img.xz
sector=512
why=crash: AddressSanitizer
cmds=?mount; ?readall; ?mkdir /fz_dir; ?write /fz_dir/f pat:7:70000; ?append /fz_dir/f pat:8:5000; ?rename /fz_dir/f /fz_dir/g; ?unlink /fz_dir/g; ?rmdir /fz_dir; ?readall; ?umount; ?mount ro; ?readall; ?umount
