#include "../../include/kernel/storage.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/shell.h"
#include "../../include/kernel/memory.h"

void storage_fs_init(storage_fs_state_t* state) {
    if (!state) {
        return;
    }
    state->signature_found = 0;
    state->tested_mask = 0;
    for (int i = 0; i < BLOCK_MAX_DEVICES; ++i) {
        state->fs_instances[i].bdev = 0;
        state->ready_reads[i] = 0;
    }
}

void storage_fs_poll(storage_fs_state_t* state) {
    if (!state || state->signature_found) {
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
        xhci_controller_t* xh = (xhci_controller_t*)bdev->driver_data;
        if (!xh || !xh->msd_ready) {
            continue;
        }

        // Probe LBA0 to ensure the device is responding before mounting
        uint8_t* probe = (uint8_t*)kalloc(512);
        if (!probe) {
            continue;
        }
        int probe_st = bdev->read((block_device_t*)bdev, 0, 1, probe);
        kfree(probe);
        if (probe_st != ST_OK) {
            // Device not ready yet; retry on next poll without marking tested
            state->ready_reads[bi] = 0;
            continue;
        }

        // Require a few consecutive successful probe reads before mounting
        if (state->ready_reads[bi] < 3) {
            state->ready_reads[bi]++;
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
