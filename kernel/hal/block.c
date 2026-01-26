// LikeOS-64 - Block device registry
#include "../../include/kernel/block.h"

static block_device_t* g_block_devices[BLOCK_MAX_DEVICES];
static int g_block_count = 0;

int block_register(block_device_t* dev)
{
    if(!dev) {
        return ST_INVALID;
    }
    if(g_block_count >= BLOCK_MAX_DEVICES) {
        return ST_ERR;
    }
    g_block_devices[g_block_count++] = dev;
    return ST_OK;
}

const block_device_t* block_get(int index)
{
    if(index < 0 || index >= g_block_count) {
        return 0;
    }
    return g_block_devices[index];
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
