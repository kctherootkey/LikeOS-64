// LikeOS-64 - USB Mass Storage (Bulk-Only Transport) Driver
// Clean implementation following USB Mass Storage Class specification
//
// BOT Protocol:
// 1. Send Command Block Wrapper (CBW) via bulk OUT
// 2. Transfer data (if any) via bulk IN or OUT
// 3. Receive Command Status Wrapper (CSW) via bulk IN

#include "../../include/kernel/usb_msd.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"

// Debug output control
#define MSD_DEBUG 0
#if MSD_DEBUG
    #define msd_dbg(fmt, ...) kprintf("[MSD] " fmt, ##__VA_ARGS__)
#else
    #define msd_dbg(fmt, ...) ((void)0)
#endif

// Spinlock for MSD device list access
static spinlock_t msd_lock = SPINLOCK_INIT("usb_msd");

// Global MSD device list
usb_msd_device_t* g_msd_devices[8] = {0};
int g_msd_count = 0;

// Memory helpers
static void msd_memset(void* dst, int val, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = (uint8_t)val;
}

static void msd_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

static int msd_strlen(const char* s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

//=============================================================================
// BOT Protocol Core
//=============================================================================

int usb_msd_bot_transfer(usb_msd_device_t* msd, usb_msd_cbw_t* cbw,
                         void* data_buf, uint32_t data_len, usb_msd_csw_t* csw) {
    if (!msd || !msd->usb_dev || !msd->ctrl) return ST_INVALID;
    
    xhci_controller_t* ctrl = msd->ctrl;
    usb_device_t* dev = msd->usb_dev;
    uint32_t transferred;
    int st;
    
    // Allocate DMA-safe PAGE-ALIGNED buffer for CBW and CSW
    // kalloc only provides 8-byte alignment, so we allocate extra and align manually
    uint8_t* raw_cbw = (uint8_t*)kcalloc_dma(1, 4096 + 4096);  // Extra page for alignment
    uint8_t* raw_csw = (uint8_t*)kcalloc_dma(1, 4096 + 4096);  // Extra page for alignment
    if (!raw_cbw || !raw_csw) {
        if (raw_cbw) kfree_dma(raw_cbw);
        if (raw_csw) kfree_dma(raw_csw);
        return ST_NOMEM;
    }
    
    // Align to page boundary
    uint8_t* dma_cbw = (uint8_t*)(((uint64_t)raw_cbw + 4095) & ~4095ULL);
    uint8_t* dma_csw = (uint8_t*)(((uint64_t)raw_csw + 4095) & ~4095ULL);
    
    msd_memset(dma_cbw, 0, 4096);
    msd_memset(dma_csw, 0, 4096);
    
    // Copy CBW to DMA buffer
    msd_memcpy(dma_cbw, cbw, CBW_SIZE);
    
    // Phase 1: Send CBW
    msd_dbg("Sending CBW: tag=%08x, len=%d, flags=%02x, cmd=%02x\n",
            cbw->tag, cbw->data_transfer_len, cbw->flags, cbw->cb[0]);
    
    st = xhci_bulk_transfer_out(ctrl, dev, dma_cbw, CBW_SIZE, &transferred);
    if (st != ST_OK) {
        msd_dbg("CBW send failed: st=%d\n", st);
        kfree_dma(raw_cbw);
        kfree_dma(raw_csw);
        return st;
    }
    
    // Phase 2: Data transfer (if any)
    // Use PAGE-ALIGNED DMA buffer for data phase
    int data_st = ST_OK;
    uint8_t* raw_data = NULL;
    if (data_len > 0 && data_buf) {
        // Allocate with extra page for alignment
        uint32_t alloc_size = ((data_len + 4095) & ~4095U) + 4096;
        raw_data = (uint8_t*)kcalloc_dma(1, alloc_size);
        if (!raw_data) {
            msd_dbg("Failed to allocate DMA buffer for data\n");
            kfree_dma(raw_cbw);
            kfree_dma(raw_csw);
            return ST_NOMEM;
        }
        
        // Align to page boundary
        uint8_t* dma_data = (uint8_t*)(((uint64_t)raw_data + 4095) & ~4095ULL);
        msd_memset(dma_data, 0, data_len);
        
        if (cbw->flags & CBW_FLAG_DATA_IN) {
            // Data IN
            msd_dbg("Data IN: %d bytes\n", data_len);
            data_st = xhci_bulk_transfer_in(ctrl, dev, dma_data, data_len, &transferred);
            if (data_st == ST_OK) {
                // Compiler barrier to prevent compiler from reordering the memcpy
                // before the DMA transfer completes.  On x86, DMA is cache-coherent
                // so mfence is not needed — the transfer_in already returned.
                __asm__ volatile("" ::: "memory");
                // Copy received data back to caller
                msd_memcpy(data_buf, dma_data, data_len);
            }
        } else {
            // Data OUT - copy data to DMA buffer first
            msd_memcpy(dma_data, data_buf, data_len);
            msd_dbg("Data OUT: %d bytes\n", data_len);
            data_st = xhci_bulk_transfer_out(ctrl, dev, dma_data, data_len, &transferred);
        }
        
        kfree_dma(raw_data);
        raw_data = NULL;
        
        if (data_st != ST_OK) {
            msd_dbg("Data transfer failed: st=%d\n", data_st);
            // Try to recover by reading CSW anyway
        }
    }
    
    // Phase 3: Receive CSW
    msd_memset(dma_csw, 0, CSW_SIZE);
    st = xhci_bulk_transfer_in(ctrl, dev, dma_csw, CSW_SIZE, &transferred);
    if (st != ST_OK) {
        msd_dbg("CSW receive failed: st=%d\n", st);
        kfree_dma(raw_cbw);
        kfree_dma(raw_csw);
        return st;
    }
    
    // Copy CSW back to caller's buffer
    msd_memcpy(csw, dma_csw, CSW_SIZE);
    kfree_dma(raw_cbw);
    kfree_dma(raw_csw);
    
    // Validate CSW
    if (csw->signature != CSW_SIGNATURE) {
        msd_dbg("Invalid CSW signature: %08x\n", csw->signature);
        return ST_IO;
    }
    
    if (csw->tag != cbw->tag) {
        msd_dbg("CSW tag mismatch: expected %08x, got %08x\n", cbw->tag, csw->tag);
        return ST_IO;
    }
    
    msd_dbg("CSW: status=%d, residue=%d\n", csw->status, csw->data_residue);
    
    if (csw->status == CSW_STATUS_PASSED) {
        return ST_OK;
    } else if (csw->status == CSW_STATUS_FAILED) {
        return ST_IO;
    } else {
        return ST_ERR;  // Phase error
    }
}

//=============================================================================
// SCSI Commands
//=============================================================================

int usb_msd_test_unit_ready(usb_msd_device_t* msd) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = 0;
    cbw.flags = 0;
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_TEST_UNIT_READY;
    
    return usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
}

int usb_msd_inquiry(usb_msd_device_t* msd, scsi_inquiry_data_t* data) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = 36;  // Standard inquiry response
    cbw.flags = CBW_FLAG_DATA_IN;
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_INQUIRY;
    cbw.cb[4] = 36;  // Allocation length
    
    msd_memset(data, 0, sizeof(*data));
    
    int st = usb_msd_bot_transfer(msd, &cbw, data, 36, &csw);
    
    if (st == ST_OK) {
        msd_dbg("Inquiry: DevType=%02x, Vendor=%.8s, Product=%.16s\n",
                data->device_type, data->vendor, data->product);
    }
    
    return st;
}

int usb_msd_request_sense(usb_msd_device_t* msd, scsi_sense_data_t* data) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = 18;
    cbw.flags = CBW_FLAG_DATA_IN;
    cbw.lun = 0;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_REQUEST_SENSE;
    cbw.cb[4] = 18;  // Allocation length
    
    msd_memset(data, 0, sizeof(*data));
    
    return usb_msd_bot_transfer(msd, &cbw, data, 18, &csw);
}

int usb_msd_read_capacity(usb_msd_device_t* msd, uint32_t* block_count, uint32_t* block_size) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    scsi_read_capacity_data_t data;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = 8;
    cbw.flags = CBW_FLAG_DATA_IN;
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_READ_CAPACITY_10;
    
    msd_memset(&data, 0, sizeof(data));
    
    int st = usb_msd_bot_transfer(msd, &cbw, &data, 8, &csw);
    
    if (st == ST_OK) {
        // Convert from big-endian
        *block_count = bswap32(data.last_lba) + 1;
        *block_size = bswap32(data.block_size);
        msd_dbg("Capacity: %u blocks, %u bytes/block\n", *block_count, *block_size);
    }
    
    return st;
}

int usb_msd_read(usb_msd_device_t* msd, uint32_t lba, uint32_t count, void* buf) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    if (count == 0 || count > 128) {
        msd_dbg("Invalid read count: %u\n", count);
        return ST_INVALID;
    }
    
    uint32_t transfer_len = count * msd->block_size;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = transfer_len;
    cbw.flags = CBW_FLAG_DATA_IN;
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_READ_10;
    // LBA (big-endian)
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    // Transfer length in blocks (big-endian)
    cbw.cb[7] = (count >> 8) & 0xFF;
    cbw.cb[8] = count & 0xFF;
    
    msd_dbg("Read: LBA=%u, Count=%u, Len=%u\n", lba, count, transfer_len);
    
    return usb_msd_bot_transfer(msd, &cbw, buf, transfer_len, &csw);
}

int usb_msd_write(usb_msd_device_t* msd, uint32_t lba, uint32_t count, const void* buf) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    if (count == 0 || count > 128) {
        return ST_INVALID;
    }
    
    uint32_t transfer_len = count * msd->block_size;
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = transfer_len;
    cbw.flags = CBW_FLAG_DATA_OUT;
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_WRITE_10;
    // LBA (big-endian)
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    // Transfer length in blocks (big-endian)
    cbw.cb[7] = (count >> 8) & 0xFF;
    cbw.cb[8] = count & 0xFF;
    
    return usb_msd_bot_transfer(msd, &cbw, (void*)buf, transfer_len, &csw);
}

int usb_msd_sync(usb_msd_device_t* msd) {
    usb_msd_cbw_t cbw;
    usb_msd_csw_t csw;
    
    if (!msd || !msd->ready) {
        return ST_INVALID;
    }
    
    msd_memset(&cbw, 0, sizeof(cbw));
    cbw.signature = CBW_SIGNATURE;
    cbw.tag = ++msd->next_tag;
    cbw.data_transfer_len = 0;  // No data transfer
    cbw.flags = CBW_FLAG_DATA_IN;  // No data but use IN for status
    cbw.lun = 0;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_SYNCHRONIZE_CACHE_10;
    // All other bytes are 0: sync entire device
    
    msd_dbg("Sync: sending SYNCHRONIZE_CACHE_10\n");
    
    return usb_msd_bot_transfer(msd, &cbw, NULL, 0, &csw);
}

//=============================================================================
// Block Device Interface
//=============================================================================

int usb_msd_block_read(block_device_t* dev, unsigned long lba, unsigned long count, void* buf) {
    usb_msd_device_t* msd = (usb_msd_device_t*)dev->driver_data;
    
    if (!msd || !msd->ready) {
        return ST_NO_DEVICE;
    }
    
    // Read in 4KB chunks (8 sectors) for reliable USB transfers on real hardware
    uint8_t* ptr = (uint8_t*)buf;
    unsigned long remaining = count;
    unsigned long current_lba = lba;
    
    while (remaining > 0) {
        unsigned long chunk = (remaining > 8) ? 8 : remaining;
        
        int st = usb_msd_read(msd, (uint32_t)current_lba, (uint32_t)chunk, ptr);
        if (st != ST_OK) {
            msd_dbg("Block read failed at LBA %lu: st=%d\n", current_lba, st);
            return st;
        }
        
        ptr += chunk * msd->block_size;
        current_lba += chunk;
        remaining -= chunk;
    }
    
    return ST_OK;
}

int usb_msd_block_write(block_device_t* dev, unsigned long lba, unsigned long count, const void* buf) {
    usb_msd_device_t* msd = (usb_msd_device_t*)dev->driver_data;
    
    if (!msd || !msd->ready) {
        return ST_NO_DEVICE;
    }
    
    const uint8_t* ptr = (const uint8_t*)buf;
    unsigned long remaining = count;
    unsigned long current_lba = lba;
    
    while (remaining > 0) {
        unsigned long chunk = (remaining > 256) ? 256 : remaining;
        
        int st = usb_msd_write(msd, (uint32_t)current_lba, (uint32_t)chunk, ptr);
        if (st != ST_OK) {
            return st;
        }
        
        ptr += chunk * msd->block_size;
        current_lba += chunk;
        remaining -= chunk;
    }
    
    return ST_OK;
}

int usb_msd_block_sync(block_device_t* dev) {
    usb_msd_device_t* msd = (usb_msd_device_t*)dev->driver_data;
    
    if (!msd || !msd->ready) {
        return ST_NO_DEVICE;
    }
    
    return usb_msd_sync(msd);
}

//=============================================================================
// MSD Initialization
//=============================================================================

int usb_msd_init(usb_msd_device_t* msd, usb_device_t* dev, xhci_controller_t* ctrl) {
    if (!msd || !dev || !ctrl) return ST_INVALID;
    
    msd_memset(msd, 0, sizeof(*msd));
    msd->usb_dev = dev;
    msd->ctrl = ctrl;
    msd->next_tag = 0x12340000;
    
    msd_dbg("Initializing MSD device...\n");
    
    // Send Inquiry command
    scsi_inquiry_data_t inquiry;
    int st = usb_msd_inquiry(msd, &inquiry);
    if (st != ST_OK) {
        msd_dbg("Inquiry failed: st=%d\n", st);
        // Continue anyway, some devices don't respond to inquiry immediately
    } else {
        // Copy vendor and product strings
        msd_memcpy(msd->vendor, inquiry.vendor, 8);
        msd->vendor[8] = '\0';
        msd_memcpy(msd->product, inquiry.product, 16);
        msd->product[16] = '\0';
        msd->removable = (inquiry.rmb & 0x80) ? 1 : 0;
    }
    
    // Wait for device to be ready
    int ready_attempts = 0;
    for (int i = 0; i < 10; i++) {
        st = usb_msd_test_unit_ready(msd);
        if (st == ST_OK) {
            msd_dbg("Device ready after %d attempts\n", i + 1);
            ready_attempts = i + 1;
            break;
        }
        
        // Request sense to clear any pending condition
        scsi_sense_data_t sense;
        usb_msd_request_sense(msd, &sense);
        
        // Small delay
        for (volatile int j = 0; j < 100000; j++);
    }
    
    if (ready_attempts == 0) {
        msd_dbg("Device not ready, continuing anyway...\n");
    }
    
    // Get capacity
    st = usb_msd_read_capacity(msd, &msd->block_count, &msd->block_size);
    if (st != ST_OK) {
        msd_dbg("Read capacity failed: st=%d\n", st);
        // Use defaults
        msd->block_size = 512;
        msd->block_count = 0;
    }
    
    // Validate block size
    if (msd->block_size == 0 || msd->block_size > 4096) {
        msd_dbg("Invalid block size %u, defaulting to 512\n", msd->block_size);
        msd->block_size = 512;
    }
    
    msd->ready = 1;
    
    // Setup block device
    msd->blk.name = "usb0";
    msd->blk.sector_size = msd->block_size;
    msd->blk.total_sectors = msd->block_count;
    msd->blk.read = usb_msd_block_read;
    msd->blk.write = usb_msd_block_write;
    msd->blk.sync = usb_msd_block_sync;
    msd->blk.driver_data = msd;
    
    // Register block device
    if (block_register(&msd->blk) == ST_OK) {
        msd_dbg("Block device registered: %s\n", msd->blk.name);
    }
    
    // Add to global list (protected by spinlock)
    uint64_t flags;
    spin_lock_irqsave(&msd_lock, &flags);
    if (g_msd_count < 8) {
        g_msd_devices[g_msd_count++] = msd;
    }
    spin_unlock_irqrestore(&msd_lock, flags);
    
    return ST_OK;
}
