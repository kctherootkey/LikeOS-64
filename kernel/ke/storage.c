#include <kernel/ke/storage.h>
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/dev/usb/xhci.h>
#include <kernel/dev/usb/usb_msd.h>
#include <kernel/ke/userinit.h>
#include <kernel/mm/memory.h>
#include <kernel/io/sysfont.h>
#include <kernel/io/cursor.h>
#include <kernel/dev/input/mouse.h>
#include <kernel/fs/pagecache.h>
#include <kernel/fs/dcache.h>
#include <kernel/fs/icache.h>
#include <kernel/fs/ext4.h>
#include <kernel/uapi/bug.h>

void storage_fs_init(storage_fs_state_t *state)
{
	if (!state) {
		return;
	}
	state->signature_found = 0;
	state->tested_mask = 0;
	state->os_ready = 0;
	for (int i = 0; i < BLOCK_MAX_DEVICES; ++i) {
		state->fs_instances[i].bdev = 0;
		state->ready_reads[i] = 0;
		state->ready_polls[i] = 0;
	}
}

void storage_fs_set_ready(storage_fs_state_t *state)
{
	if (!state) {
		return;
	}
	state->os_ready = 1;
}

void storage_fs_poll(storage_fs_state_t *state)
{
	BUG_ON(state == NULL);
	if (!state || state->signature_found) {
		return;
	}

	if (!state->os_ready) {
		return;
	}

	int nblk = block_count();
	WARN_ON_ONCE(
		nblk <=
		0); /* block_count() returned 0 or negative: no block devices registered before storage_fs_poll */
	for (int bi = 0; bi < nblk && !state->signature_found; ++bi) {
		if (state->tested_mask & (1u << bi)) {
			continue;
		}
		const block_device_t *bdev = block_get(bi);
		if (!bdev || !bdev->driver_data) {
			state->tested_mask |= (1u << bi);
			continue;
		}
		// driver_data now points to usb_msd_device_t
		usb_msd_device_t *msd = (usb_msd_device_t *)bdev->driver_data;
		if (!msd || !msd->ready) {
			if (state->ready_polls[bi] == 0) {
				/* quiet: waiting for MSD ready log removed */
			}
			state->ready_polls[bi] = 0;
			continue;
		}
		// Wait for controller to remain ready for a minimum number of polls
		if (state->ready_polls[bi] < 20) {
			state->ready_polls[bi]++;
			if (state->ready_polls[bi] == 1 ||
			    state->ready_polls[bi] == 10 ||
			    state->ready_polls[bi] == 20) {
				/* quiet: ready poll log removed */
			}
			continue;
		}

		/* Probe ext4 first (the new default root).  ext4_mount returns
         * ST_NOT_FOUND on a non-ext4 device, in which case we fall through
         * to the existing FAT32 path unchanged (regression-safe). */
		static ext4_fs_t s_ext4_root;
		if (ext4_mount(bdev, &s_ext4_root) == ST_OK) {
			ext4_vfs_register_root(&s_ext4_root);
			kprintf("EXT4: mount succeeded on %s (checking signature)\n",
				bdev->name);
			pagecache_init();
			dcache_init();
			icache_init();
			vfs_file_t *sf = 0;
			if (vfs_open("/LIKEOS.SIG", 0, &sf) == ST_OK) {
				vfs_close(sf);
				state->signature_found = 1;
				kprintf("EXT4: signature /LIKEOS.SIG found on %s (root storage selected)\n",
					bdev->name);
				if (sysfont_load("/res/Lat15-Fixed16.psf") ==
				    0) {
					console_apply_sysfont();
				}
				if (cursor_load("/res/left_ptr") == 0) {
					mouse_apply_cursor();
				}
			} else {
				kprintf("EXT4: signature not found on %s\n",
					bdev->name);
				state->tested_mask |= (1u << bi);
			}
			userinit_redisplay_prompt();
			continue; /* ext4-formatted device handled; skip FAT32 */
		}

		fat32_fs_t *fs = &state->fs_instances[bi];
		if (fat32_mount(bdev, fs) == ST_OK) {
			fat32_vfs_register_root(fs);
			kprintf("FAT32: mount succeeded on %s (checking signature)\n",
				bdev->name);
			// Initialize the page cache now that FAT32 is mounted
			pagecache_init();
			dcache_init();
			icache_init();
			vfs_file_t *sf = 0;
			if (vfs_open("/LIKEOS.SIG", 0, &sf) == ST_OK) {
				vfs_close(sf);
				state->signature_found = 1;
				kprintf("FAT32: signature /LIKEOS.SIG found on %s (root storage selected)\n",
					bdev->name);

				// Load system console font from /res/Lat15-Fixed16.psf
				if (sysfont_load("/res/Lat15-Fixed16.psf") ==
				    0) {
					console_apply_sysfont();
				}

				// Load mouse cursor from /res/left_ptr
				if (cursor_load("/res/left_ptr") == 0) {
					mouse_apply_cursor();
				}

				userinit_redisplay_prompt(); // Redisplay prompt after mount messages
			} else {
				kprintf("FAT32: signature not found on %s\n",
					bdev->name);
				state->tested_mask |= (1u << bi);
				userinit_redisplay_prompt(); // Redisplay prompt after mount messages
			}
		} else {
			kprintf("FAT32: mount failed on %s\n",
				bdev->name ? bdev->name : "(unnamed)");
			state->tested_mask |= (1u << bi);
			userinit_redisplay_prompt(); // Redisplay prompt after mount messages
		}
	}
}
