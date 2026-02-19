// LikeOS-64 - Block device registry
#include "../../include/kernel/block.h"
#include "../../include/kernel/sched.h"

// Spinlock for block device list access
static spinlock_t block_lock = SPINLOCK_INIT("block");

static block_device_t* g_block_devices[BLOCK_MAX_DEVICES];
static int g_block_count = 0;

int block_register(block_device_t* dev)
{
    if(!dev) {
        return ST_INVALID;
    }

    uint64_t flags;
    spin_lock_irqsave(&block_lock, &flags);

    if(g_block_count >= BLOCK_MAX_DEVICES) {
        spin_unlock_irqrestore(&block_lock, flags);
        return ST_ERR;
    }
    g_block_devices[g_block_count++] = dev;

    spin_unlock_irqrestore(&block_lock, flags);
    return ST_OK;
}

const block_device_t* block_get(int index)
{
    uint64_t flags;
    spin_lock_irqsave(&block_lock, &flags);

    if(index < 0 || index >= g_block_count) {
        spin_unlock_irqrestore(&block_lock, flags);
        return 0;
    }
    const block_device_t* dev = g_block_devices[index];

    spin_unlock_irqrestore(&block_lock, flags);
    return dev;
}

int block_count(void)
{
    return g_block_count;
}

int block_sync(block_device_t* dev)
{
    if (!dev) {
        return ST_INVALID;
    }
    if (dev->sync) {
        return dev->sync(dev);
    }
    return ST_OK;  // No sync function = no-op (success)
}
