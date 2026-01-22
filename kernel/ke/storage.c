#include "../../include/kernel/storage.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb_msd.h"
#include "../../include/kernel/shell.h"
#include "../../include/kernel/memory.h"

void storage_fs_init(storage_fs_state_t* state) {
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

void storage_fs_set_ready(storage_fs_state_t* state) {
    if (!state) {
        return;
    }
    state->os_ready = 1;
}

void storage_fs_poll(storage_fs_state_t* state) {
    if (!state || state->signature_found) {
        return;
    }

    if (!state->os_ready) {
        return;
    }

    int nblk = block_count();
    for (int bi = 0; bi < nblk && !state->signature_found; ++bi) {
        if (state->tested_mask & (1u << bi)) {
            continue;
        }
        const block_device_t* bdev = block_get(bi);
        if (!bdev || !bdev->driver_data) {
            state->tested_mask |= (1u << bi);
            continue;
        }
        // driver_data now points to usb_msd_device_t
        usb_msd_device_t* msd = (usb_msd_device_t*)bdev->driver_data;
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
            if (state->ready_polls[bi] == 1 || state->ready_polls[bi] == 10 || state->ready_polls[bi] == 20) {
                /* quiet: ready poll log removed */
            }
            continue;
        }

        fat32_fs_t* fs = &state->fs_instances[bi];
        if (fat32_mount(bdev, fs) == ST_OK) {
            fat32_vfs_register_root(fs);
            kprintf("FAT32: mount succeeded on %s (checking signature)\n", bdev->name);
            vfs_file_t* sf = 0;
            if (vfs_open("/LIKEOS.SIG", 0, &sf) == ST_OK) {
                vfs_close(sf);
                state->signature_found = 1;
                kprintf("FAT32: signature /LIKEOS.SIG found on %s (root storage selected)\n", bdev->name);
                shell_redisplay_prompt();  // Redisplay prompt after mount messages
            } else {
                kprintf("FAT32: signature not found on %s\n", bdev->name);
                state->tested_mask |= (1u << bi);
                shell_redisplay_prompt();  // Redisplay prompt after mount messages
            }
        } else {
            kprintf("FAT32: mount failed on %s\n", bdev->name ? bdev->name : "(unnamed)");
            state->tested_mask |= (1u << bi);
            shell_redisplay_prompt();  // Redisplay prompt after mount messages
        }
    }
}
