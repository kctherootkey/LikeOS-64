// LikeOS-64 VMware vmxnet3 paravirtualized NIC Driver
//
// Implements UPT/Vmxnet3 revision 1, the lowest-common-denominator that
// every supporting host (ESXi, Workstation, Fusion, QEMU) accepts.  We
// run with a single TX queue and a single RX queue.  Interrupts are
// delivered as legacy INTx.

#include "../../../include/kernel/vmxnet3.h"
#include "../../../include/kernel/net.h"
#include "../../../include/kernel/pci.h"
#include "../../../include/kernel/memory.h"
#include "../../../include/kernel/console.h"
#include "../../../include/kernel/interrupt.h"
#include "../../../include/kernel/slab.h"
#include "../../../include/kernel/lapic.h"
#include "../../../include/kernel/ioapic.h"
#include "../../../include/kernel/acpi.h"
#include "../../../include/kernel/timer.h"

static vmxnet3_dev_t g_vmxnet3;
int g_vmxnet3_initialized = 0;
int g_vmxnet3_legacy_irq = -1;

// ============================================================================
// MMIO accessors
// ============================================================================
static inline uint32_t vm_r32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void vm_w32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void vm_delay_us(uint32_t us) {
    uint32_t t0 = timer_pmtimer_read_raw();
    if (t0 == 0) {
        for (volatile uint32_t i = 0; i < us * 4; i++)
            __asm__ volatile("pause");
        return;
    }
    while (timer_pmtimer_delta_us(t0, timer_pmtimer_read_raw()) < us)
        __asm__ volatile("pause");
}

// ============================================================================
// Issue a "get" command to VMXNET3_REG_CMD and read the result back.
// "Set" commands (CAFE...) just write — no read required.
// ============================================================================
static inline uint32_t vm_cmd_get(vmxnet3_dev_t* dev, uint32_t cmd) {
    vm_w32(dev->bar1, VMXNET3_REG_CMD, cmd);
    return vm_r32(dev->bar1, VMXNET3_REG_CMD);
}
static inline void vm_cmd_set(vmxnet3_dev_t* dev, uint32_t cmd) {
    vm_w32(dev->bar1, VMXNET3_REG_CMD, cmd);
}

// ============================================================================
// Map a memory BAR (bar0 or bar1, 4 KiB each).
// Returns physical address through *phys_out and a kernel-virtual pointer.
// ============================================================================
static volatile uint8_t* vm_map_bar(const pci_device_t* pd, int bar_idx,
                                    uint64_t* phys_out)
{
    uint32_t b = pd->bar[bar_idx];
    if (b & 1) {
        kprintf("VMXNET3: BAR%d is I/O port, expected MMIO\n", bar_idx);
        return NULL;
    }
    uint64_t phys = b & 0xFFFFFFF0ULL;
    if ((b & 0x06) == 0x04) {
        // 64-bit BAR: high half lives in the next BAR slot
        phys |= ((uint64_t)pd->bar[bar_idx + 1]) << 32;
    }
    *phys_out = phys;

    uint64_t virt = (uint64_t)phys_to_virt(phys);
    if (!mm_is_page_mapped(virt)) {
        mm_map_page(virt, phys,
                    PAGE_PRESENT | PAGE_WRITABLE |
                    PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
    }
    return (volatile uint8_t*)virt;
}

// ============================================================================
// Allocate one DMA-capable page (must be below 4 GiB for 32-bit BAR-routed
// hosts; on QEMU vmxnet3 supports 64-bit DMA but we conservatively use
// the same identity-mapped allocator we use everywhere else).
// ============================================================================
static void* vm_alloc_dma(uint64_t* phys_out, size_t bytes) {
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = mm_allocate_contiguous_pages(pages);
    if (!phys) return NULL;
    *phys_out = phys;
    void* virt = phys_to_virt(phys);
    // Zero
    uint64_t* p = (uint64_t*)virt;
    for (size_t i = 0; i < (pages * PAGE_SIZE) / 8; i++) p[i] = 0;
    return virt;
}

// ============================================================================
// Read MAC via host-visible registers (set after RESET_DEV by the host)
// ============================================================================
static void vm_read_mac(vmxnet3_dev_t* dev) {
    // Prefer the "permanent MAC" command (returns BIOS/host MAC even if
    // we haven't activated yet).  Fall back to MACL/MACH for older
    // emulations.
    uint32_t lo = vm_cmd_get(dev, VMXNET3_CMD_GET_PERM_MAC_LO);
    uint32_t hi = vm_cmd_get(dev, VMXNET3_CMD_GET_PERM_MAC_HI);
    if ((lo | hi) == 0) {
        lo = vm_r32(dev->bar1, VMXNET3_REG_MACL);
        hi = vm_r32(dev->bar1, VMXNET3_REG_MACH);
    }
    dev->mac_addr[0] = (uint8_t)(lo);
    dev->mac_addr[1] = (uint8_t)(lo >> 8);
    dev->mac_addr[2] = (uint8_t)(lo >> 16);
    dev->mac_addr[3] = (uint8_t)(lo >> 24);
    dev->mac_addr[4] = (uint8_t)(hi);
    dev->mac_addr[5] = (uint8_t)(hi >> 8);
}

// ============================================================================
// Allocate descriptor rings + buffers
// ============================================================================
static int vm_alloc_rings(vmxnet3_dev_t* dev) {
    // TX ring (32 * 16 = 512 bytes)
    dev->tx_ring = (vmxnet3_tx_desc_t*)
        vm_alloc_dma(&dev->tx_ring_phys, sizeof(vmxnet3_tx_desc_t) * VMXNET3_NUM_TX_DESC);
    if (!dev->tx_ring) return -1;

    // TX completion ring (32 * 16 = 512 bytes)
    dev->tx_comp_ring = (vmxnet3_tx_comp_desc_t*)
        vm_alloc_dma(&dev->tx_comp_ring_phys,
                     sizeof(vmxnet3_tx_comp_desc_t) * VMXNET3_NUM_TX_COMP);
    if (!dev->tx_comp_ring) return -1;

    // RX ring 0 (32 * 16 = 512 bytes)
    dev->rx_ring = (vmxnet3_rx_desc_t*)
        vm_alloc_dma(&dev->rx_ring_phys,
                     sizeof(vmxnet3_rx_desc_t) * VMXNET3_NUM_RX_DESC);
    if (!dev->rx_ring) return -1;

    // RX ring 1 (size 1; required by spec, never used)
    dev->rx_ring2 = (vmxnet3_rx_desc_t*)
        vm_alloc_dma(&dev->rx_ring2_phys,
                     sizeof(vmxnet3_rx_desc_t) * VMXNET3_NUM_RX2_DESC);
    if (!dev->rx_ring2) return -1;

    // RX completion ring (33 * 16 = 528 bytes)
    dev->rx_comp_ring = (vmxnet3_rx_comp_desc_t*)
        vm_alloc_dma(&dev->rx_comp_ring_phys,
                     sizeof(vmxnet3_rx_comp_desc_t) * VMXNET3_NUM_RX_COMP);
    if (!dev->rx_comp_ring) return -1;

    // Per-slot TX/RX buffers (one page each — overkill but simple)
    for (int i = 0; i < VMXNET3_NUM_TX_DESC; i++) {
        uint64_t phys = mm_allocate_physical_page();
        if (!phys) return -1;
        dev->tx_bufs[i] = (uint8_t*)phys_to_virt(phys);
        dev->tx_bufs_phys[i] = phys;
    }
    for (int i = 0; i < VMXNET3_NUM_RX_DESC; i++) {
        uint64_t phys = mm_allocate_physical_page();
        if (!phys) return -1;
        dev->rx_bufs[i] = (uint8_t*)phys_to_virt(phys);
        dev->rx_bufs_phys[i] = phys;
    }

    // Init RX descriptors — initially HW-owned so the device can fill
    // them with incoming packets.  gen=1 is the initial "host-owned"
    // value; we flip it on each subsequent re-post.
    dev->rx_gen = 1;
    for (int i = 0; i < VMXNET3_NUM_RX_DESC; i++) {
        dev->rx_ring[i].addr = dev->rx_bufs_phys[i];
        // val = len[0..13] | btype[14] | dtype[15] | gen[31]
        dev->rx_ring[i].val =
            (VMXNET3_RX_BUF_SIZE & 0x3FFF) |
            (0u << 14) |              // btype = head
            ((uint32_t)dev->rx_gen << 31);
        dev->rx_ring[i].reserved = 0;
    }
    // Ring 2 — required by the host (validated for size & alignment) but
    // we don't use it.  Mark every entry software-owned (gen = ~rx_gen)
    // so the device skips them.
    for (int i = 0; i < VMXNET3_NUM_RX2_DESC; i++) {
        dev->rx_ring2[i].addr = 0;
        dev->rx_ring2[i].val = 0;          // gen=0, opposite of rx_gen=1
        dev->rx_ring2[i].reserved = 0;
    }

    // Init TX descriptors — software-owned (gen flips on first post)
    dev->tx_gen = 1;
    dev->tx_comp_gen = 1;
    dev->rx_comp_gen = 1;
    for (int i = 0; i < VMXNET3_NUM_TX_DESC; i++) {
        dev->tx_ring[i].addr = 0;
        dev->tx_ring[i].val1 = 0;
        dev->tx_ring[i].val2 = 0;
    }

    // Multicast filter table — one zero byte (we accept all multicast
    // via rxMode anyway, but mfTablePA must be non-null per the spec).
    dev->mf_table = (uint8_t*)vm_alloc_dma(&dev->mf_table_phys, 64);
    if (!dev->mf_table) return -1;

    return 0;
}

// ============================================================================
// Build DriverShared, TxQueueDesc, RxQueueDesc and ACTIVATE the device
// ============================================================================
static int vm_activate(vmxnet3_dev_t* dev) {
    // Allocate one page for DriverShared + queue descs (each well under 1KB)
    uint64_t phys;
    void* p = vm_alloc_dma(&phys, PAGE_SIZE);
    if (!p) return -1;

    dev->shared = (vmxnet3_driver_shared_t*)p;
    dev->shared_phys = phys;

    // Lay TxQueueDesc and RxQueueDesc immediately after DriverShared.
    // The host walks the table using sizeof(Vmxnet3_TxQueueDesc) /
    // sizeof(Vmxnet3_RxQueueDesc), so they MUST be packed back-to-back
    // with no inter-queue padding.  queueDescPA itself must be 128-byte
    // aligned.
    uint64_t off = (sizeof(vmxnet3_driver_shared_t) + 127) & ~127ULL;
    dev->txqd = (vmxnet3_tx_queue_desc_t*)((uint8_t*)p + off);
    dev->txqd_phys = phys + off;

    off += sizeof(vmxnet3_tx_queue_desc_t);
    dev->rxqd = (vmxnet3_rx_queue_desc_t*)((uint8_t*)p + off);
    dev->rxqd_phys = phys + off;

    // -- DriverShared -------------------------------------------------------
    vmxnet3_driver_shared_t* s = dev->shared;
    s->magic = VMXNET3_REV1_MAGIC;
    s->pad = 0;

    vmxnet3_misc_conf_t* mc = &s->devRead.misc;
    mc->driverInfo.version = 1;
    // gos packed: bits 0-1 = bits, bits 2-5 = type, bits 6-21 = ver, 22-31 misc
    // 64-bit Linux-flavoured guest: bits=2, type=1
    mc->driverInfo.gos = 2u | (1u << 2);
    mc->driverInfo.vmxnet3RevSpt = 0x1;       // rev1
    mc->driverInfo.uptVerSpt     = 0x1;       // UPT1

    mc->uptFeatures   = 0;                    // no LRO/RSS/checksum offload
    mc->ddPA          = dev->shared_phys;     // back-pointer to ourselves
    mc->ddLen         = sizeof(vmxnet3_driver_shared_t);
    mc->queueDescPA   = dev->txqd_phys;
    mc->queueDescLen  = sizeof(vmxnet3_tx_queue_desc_t)
                      + sizeof(vmxnet3_rx_queue_desc_t);
    mc->mtu           = NET_MTU_DEFAULT;
    mc->maxNumRxSG    = 1;
    mc->numTxQueues   = 1;
    mc->numRxQueues   = 1;

    vmxnet3_intr_conf_t* ic = &s->devRead.intrConf;
    ic->autoMask     = 0;
    ic->numIntrs     = 1;             // single legacy/MSI vector for everything
    ic->eventIntrIdx = 0;
    for (int i = 0; i < VMXNET3_MAX_INTRS; i++) ic->modLevels[i] = 0;
    ic->intrCtrl     = VMXNET3_IC_DISABLE_ALL;   // start masked; we'll enable

    vmxnet3_rx_filter_conf_t* rc = &s->devRead.rxFilterConf;
    rc->rxMode      = VMXNET3_RXM_UCAST | VMXNET3_RXM_BCAST | VMXNET3_RXM_ALL_MULTI;
    rc->mfTableLen  = 0;
    rc->mfTablePA   = dev->mf_table_phys;
    for (int i = 0; i < VMXNET3_VFT_SIZE; i++) rc->vfTable[i] = 0xFFFFFFFF;   // accept all VLANs

    s->devRead.rssConfDesc.confLen    = 0;
    s->devRead.rssConfDesc.confPA     = 0;
    s->devRead.pmConfDesc.confLen     = 0;
    s->devRead.pmConfDesc.confPA      = 0;
    s->devRead.pluginConfDesc.confLen = 0;
    s->devRead.pluginConfDesc.confPA  = 0;

    // -- Tx Queue Desc ------------------------------------------------------
    vmxnet3_tx_queue_desc_t* tq = dev->txqd;
    tq->conf.txRingBasePA   = dev->tx_ring_phys;
    tq->conf.dataRingBasePA = 0;
    tq->conf.compRingBasePA = dev->tx_comp_ring_phys;
    tq->conf.ddPA           = 0;
    tq->conf.txRingSize     = VMXNET3_NUM_TX_DESC;
    tq->conf.dataRingSize   = 0;
    tq->conf.compRingSize   = VMXNET3_NUM_TX_COMP;
    tq->conf.ddLen          = 0;
    tq->conf.intrIdx        = 0;
    tq->conf.txDataRingDescSize = 0;

    // -- Rx Queue Desc ------------------------------------------------------
    vmxnet3_rx_queue_desc_t* rq = dev->rxqd;
    rq->conf.rxRingBasePA[0] = dev->rx_ring_phys;
    rq->conf.rxRingBasePA[1] = dev->rx_ring2_phys;
    rq->conf.compRingBasePA  = dev->rx_comp_ring_phys;
    rq->conf.ddPA            = 0;
    rq->conf.rxRingSize[0]   = VMXNET3_NUM_RX_DESC;
    rq->conf.rxRingSize[1]   = VMXNET3_NUM_RX2_DESC;
    rq->conf.compRingSize    = VMXNET3_NUM_RX_COMP;
    rq->conf.ddLen           = 0;
    rq->conf.intrIdx         = 0;

    // -- Hand off DriverShared physical address -----------------------------
    // CRITICAL: fence BEFORE the MMIO writes so that all the cacheable
    // (WB) stores into *shared / *txqd / *rxqd are globally observable
    // before the device starts looking at them.  UC stores to BAR1 are
    // otherwise allowed to reorder ahead of pending WB stores.
    __asm__ volatile("mfence" ::: "memory");

    vm_w32(dev->bar1, VMXNET3_REG_DSAL, (uint32_t)(dev->shared_phys & 0xFFFFFFFF));
    vm_w32(dev->bar1, VMXNET3_REG_DSAH, (uint32_t)(dev->shared_phys >> 32));

    __asm__ volatile("mfence" ::: "memory");

    // ACTIVATE_DEV: host validates the entire DriverShared blob; result
    // is read back from VMXNET3_REG_CMD (0 = success, non-zero = error).
    vm_w32(dev->bar1, VMXNET3_REG_CMD, VMXNET3_CMD_ACTIVATE_DEV);
    uint32_t status = vm_r32(dev->bar1, VMXNET3_REG_CMD);
    if (status != 0) {
        kprintf("VMXNET3: ACTIVATE_DEV failed (status=0x%x)\n", status);
        return -1;
    }

    // Kick the RX producer so the device knows we have buffers ready.
    // RXPROD register is at BAR0 offset 0x800, indexed by queue (stride 8).
    vm_w32(dev->bar0, VMXNET3_REG_RXPROD,  VMXNET3_NUM_RX_DESC - 1);
    vm_w32(dev->bar0, VMXNET3_REG_RXPROD2, 0);

    return 0;
}

// ============================================================================
// Send a single Ethernet frame
// ============================================================================
static int vmxnet3_send(net_device_t* ndev, const uint8_t* data, uint16_t len) {
    vmxnet3_dev_t* dev = (vmxnet3_dev_t*)ndev->driver_data;

    if (len > VMXNET3_TX_BUF_SIZE) return -1;
    uint16_t out_len = (len < 60) ? 60 : len;

    // Serialize across CPUs: tx_prod/tx_gen, descriptor slot, TXPROD MMIO.
    uint64_t txflags;
    spin_lock_irqsave(&dev->tx_lock, &txflags);

    uint16_t slot = dev->tx_prod;
    vmxnet3_tx_desc_t* td = &dev->tx_ring[slot];

    // Copy payload into the slot buffer
    for (uint16_t i = 0; i < len; i++) dev->tx_bufs[slot][i] = data[i];
    for (uint16_t i = len; i < out_len; i++) dev->tx_bufs[slot][i] = 0;

    td->addr = dev->tx_bufs_phys[slot];
    // val1 (LE bitfields, low->high): len[0:13] | gen[14] | rsvd[15] |
    //                                  dtype[16] | ext1[17] | msscof[18:31]
    uint32_t val1 = ((uint32_t)(out_len & 0x3FFF)) |
                    ((uint32_t)dev->tx_gen << 14);
    // val2 (LE bitfields, low->high): hlen[0:9] | om[10:11] | eop[12] |
    //                                   cq[13] | ext2[14] | ti[15] | tci[16:31]
    uint32_t val2 = (1u << 12) | (1u << 13);

    // Per spec, val1 must be written LAST (the gen bit transfers ownership)
    td->val2 = val2;
    __asm__ volatile("" ::: "memory");
    td->val1 = val1;
    __asm__ volatile("mfence" ::: "memory");

    // Advance and flip generation if we wrap
    dev->tx_prod = slot + 1;
    if (dev->tx_prod >= VMXNET3_NUM_TX_DESC) {
        dev->tx_prod = 0;
        dev->tx_gen ^= 1;
    }

    // Doorbell — write the producer index to BAR0 + TXPROD
    vm_w32(dev->bar0, VMXNET3_REG_TXPROD, dev->tx_prod);

    ndev->tx_packets++;
    ndev->tx_bytes += out_len;
    spin_unlock_irqrestore(&dev->tx_lock, txflags);
    return 0;
}

// ============================================================================
// Link status — issue GET_LINK; bit 0 of the result = link up
// ============================================================================
static int vmxnet3_link_status(net_device_t* ndev) {
    vmxnet3_dev_t* dev = (vmxnet3_dev_t*)ndev->driver_data;
    uint32_t link = vm_cmd_get(dev, VMXNET3_CMD_GET_LINK);
    int now_up = (link & 0x1) ? 1 : 0;
    int was_up = dev->link_up;
    if (now_up != was_up) {
        dev->link_up = now_up;
        kprintf("VMXNET3: Link %s\n", now_up ? "UP" : "DOWN");
        if (!now_up) dhcp_invalidate(ndev);
    }
    return now_up;
}

// Quiesce the controller ahead of an ACPI S5 transition.  The vmxnet3
// control plane exposes explicit QUIESCE_DEV and RESET_DEV commands;
// QUIESCE halts RX/TX rings and stops the device from issuing further
// DMA, RESET puts the device into the post-power-on state.  Combined
// with masking BAR0 IMR[0] and dropping PCI bus-master, the hypervisor
// (or the platform when running on bare-metal-style passthrough) will
// not see any pending PCI activity at sleep time.
static void vmxnet3_shutdown(net_device_t* ndev) {
    vmxnet3_dev_t* dev = (vmxnet3_dev_t*)ndev->driver_data;
    if (!dev || !dev->bar1) return;

    if (dev->shared) {
        dev->shared->devRead.intrConf.intrCtrl = VMXNET3_IC_DISABLE_ALL;
    }
    if (dev->bar0) {
        vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 1); // mask vector 0
    }
    vm_cmd_set(dev, VMXNET3_CMD_QUIESCE_DEV);
    vm_cmd_set(dev, VMXNET3_CMD_RESET_DEV);

    if (dev->pci_dev) {
        const pci_device_t* pdev = dev->pci_dev;
        uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device, pdev->function, 0x04);
        cmd &= ~0x0004u; // clear Bus Master Enable
        pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04, cmd);
    }
}

// ============================================================================
// Drain the RX completion ring
// ============================================================================
static void vm_drain_rx(vmxnet3_dev_t* dev) {
    while (1) {
        vmxnet3_rx_comp_desc_t* cd = &dev->rx_comp_ring[dev->rx_comp_next];

        // Generation bit is bit 31 of word 3
        uint32_t w3 = cd->word3;
        uint8_t gen = (w3 >> 31) & 1;
        if (gen != dev->rx_comp_gen) break;   // not ours yet

        uint32_t w0 = cd->word0;
        uint32_t w2 = cd->word2;
        uint16_t rxd_idx = (uint16_t)(w0 & 0xFFF);
        // word0 LE bitfields: rxdIdx[0:11] | ext1[12:13] | eop[14] |
        //                      sop[15] | rqID[16:25] | rssType[26:29] | cnc[30] | ext2[31]
        uint8_t  eop     = (w0 >> 14) & 1;
        // word2 LE bitfields: len[0:13] | err[14] | ts[15] | tci[16:31]
        uint16_t length  = (uint16_t)(w2 & 0x3FFF);
        uint8_t  err     = (w2 >> 14) & 1;

        if (err || !eop || length == 0 || length > VMXNET3_RX_BUF_SIZE) {
            dev->net_dev.rx_errors++;
        } else if (rxd_idx < VMXNET3_NUM_RX_DESC) {
            net_rx_packet(&dev->net_dev, dev->rx_bufs[rxd_idx], length);
        }

        // Re-arm the corresponding RX descriptor with the *current*
        // ring-side gen bit (which flips when we wrap the RX ring).
        if (rxd_idx < VMXNET3_NUM_RX_DESC) {
            dev->rx_ring[rxd_idx].addr = dev->rx_bufs_phys[rxd_idx];
            dev->rx_ring[rxd_idx].val =
                (VMXNET3_RX_BUF_SIZE & 0x3FFF) |
                ((uint32_t)dev->rx_gen << 31);
            dev->rx_ring[rxd_idx].reserved = 0;
        }
        // Tell HW we've replenished a buffer (RXPROD = last filled index)
        vm_w32(dev->bar0, VMXNET3_REG_RXPROD, rxd_idx);

        // Advance comp ring pointer; flip expected gen on wrap
        dev->rx_comp_next++;
        if (dev->rx_comp_next >= VMXNET3_NUM_RX_COMP) {
            dev->rx_comp_next = 0;
            dev->rx_comp_gen ^= 1;
        }

        // Track when the RX desc ring itself wraps so the next post uses
        // the flipped gen bit (HW signals "owned" with the value last
        // written, which we keep in dev->rx_gen).
        if (rxd_idx == VMXNET3_NUM_RX_DESC - 1)
            dev->rx_gen ^= 1;
    }
}

// ============================================================================
// Drain the TX completion ring (just advance pointers; we don't track
// per-descriptor completions other than for housekeeping).
// ============================================================================
static void vm_drain_tx(vmxnet3_dev_t* dev) {
    while (1) {
        vmxnet3_tx_comp_desc_t* cd = &dev->tx_comp_ring[dev->tx_comp_next];
        uint8_t gen = (cd->word3 >> 31) & 1;
        if (gen != dev->tx_comp_gen) break;

        dev->tx_comp_next++;
        if (dev->tx_comp_next >= VMXNET3_NUM_TX_COMP) {
            dev->tx_comp_next = 0;
            dev->tx_comp_gen ^= 1;
        }
    }
}

// ============================================================================
// Top-half IRQ handler
// ============================================================================
void vmxnet3_irq_handler(void) {
    if (!g_vmxnet3_initialized) {
        lapic_eoi();
        return;
    }

    // Feed entropy from NIC IRQ timing
    {
        extern void entropy_add_interrupt_timing(uint64_t extra);
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
    }

    vmxnet3_dev_t* dev = &g_vmxnet3;

    // For legacy INTx, reading ICR both clears and tells us "is this
    // ours?".  For MSI, ICR reads as 0 but doing the read is harmless.
    if (dev->msi_vector == 0) {
        uint32_t icr = vm_r32(dev->bar1, VMXNET3_REG_ICR);
        if (icr == 0) {
            // Not ours — line is shared in legacy mode
            lapic_eoi();
            return;
        }
    }

    // Mask interrupts while we drain (intrCtrl bit 0 = disable)
    dev->shared->devRead.intrConf.intrCtrl = VMXNET3_IC_DISABLE_ALL;
    __asm__ volatile("mfence" ::: "memory");

    // Read & ack ECR (event cause); 0 means just queue work (RX/TX)
    uint32_t ecr = vm_r32(dev->bar1, VMXNET3_REG_ECR);
    if (ecr) {
        vm_w32(dev->bar1, VMXNET3_REG_ECR, ecr);   // W1C
        if (ecr & 0x4) {  // RQERR or LINK changed — re-query link
            uint32_t link = vm_cmd_get(dev, VMXNET3_CMD_GET_LINK);
            int up = (link & 0x1) ? 1 : 0;
            if (up != dev->link_up) {
                dev->link_up = up;
                kprintf("VMXNET3: Link %s\n", up ? "UP" : "DOWN");
                if (!up) dhcp_invalidate(&dev->net_dev);
            }
        }
    }

    vm_drain_rx(dev);
    vm_drain_tx(dev);

    // Re-enable interrupts
    dev->shared->devRead.intrConf.intrCtrl = 0;
    __asm__ volatile("mfence" ::: "memory");

    lapic_eoi();
}

// ============================================================================
// Driver entry point
// ============================================================================
void vmxnet3_init(void) {
    int count;
    const pci_device_t* devs = pci_get_devices(&count);

    for (int i = 0; i < count; i++) {
        if (devs[i].vendor_id != VMXNET3_VENDOR_ID) continue;
        if (devs[i].device_id != VMXNET3_DEV_ID) continue;

        kprintf("VMXNET3: Found vmxnet3 NIC (PCI %02x:%02x.%x)\n",
                devs[i].bus, devs[i].device, devs[i].function);

        vmxnet3_dev_t* dev = &g_vmxnet3;
        dev->pci_dev = &devs[i];

        pci_enable_busmaster_mem(dev->pci_dev);

        dev->bar0 = vm_map_bar(dev->pci_dev, 0, &dev->bar0_phys);
        dev->bar1 = vm_map_bar(dev->pci_dev, 1, &dev->bar1_phys);
        if (!dev->bar0 || !dev->bar1) return;

        // Negotiate revisions:
        //   Read VRRS to discover supported revs, write back the highest
        //   bit we support (rev 1).  Same for UVRS / UPT.
        uint32_t vrrs = vm_r32(dev->bar1, VMXNET3_REG_VRRS);
        if (!(vrrs & VMXNET3_VERSION_SELECT)) {
            kprintf("VMXNET3: revision 1 not supported (VRRS=0x%x)\n", vrrs);
            return;
        }
        vm_w32(dev->bar1, VMXNET3_REG_VRRS, VMXNET3_VERSION_SELECT);

        uint32_t uvrs = vm_r32(dev->bar1, VMXNET3_REG_UVRS);
        if (!(uvrs & VMXNET3_UPT_VERSION_SELECT)) {
            kprintf("VMXNET3: UPT1 not supported (UVRS=0x%x)\n", uvrs);
            return;
        }
        vm_w32(dev->bar1, VMXNET3_REG_UVRS, VMXNET3_UPT_VERSION_SELECT);

        // Reset the device — clears any prior state from BIOS/firmware.
        vm_cmd_set(dev, VMXNET3_CMD_RESET_DEV);
        vm_delay_us(1000);

        // Query MAC.  Must be done after RESET_DEV because the host only
        // populates MACL/MACH and PERM_MAC after reset.
        vm_read_mac(dev);
        kprintf("VMXNET3: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                dev->mac_addr[0], dev->mac_addr[1], dev->mac_addr[2],
                dev->mac_addr[3], dev->mac_addr[4], dev->mac_addr[5]);

        if (vm_alloc_rings(dev) < 0) {
            kprintf("VMXNET3: failed to allocate rings\n");
            return;
        }
        if (vm_activate(dev) < 0) return;

        // ------------------------------------------------------------------
        // MSI first; legacy INTx with full ACPI _PRT lookup as fallback.
        //
        // The kernel does not yet implement MSI-X, so we never try it
        // here even though vmxnet3 advertises an MSI-X capability — MSI
        // is sufficient for our single-vector, single-queue setup.
        // ------------------------------------------------------------------
        dev->msi_vector = VMXNET3_MSI_VECTOR;
        int msi_ret = pci_enable_msi(dev->pci_dev, dev->msi_vector);
        if (msi_ret < 0) {
            dev->msi_vector = 0;

            uint8_t irq = 0xFF;
            uint32_t gsi = 0;
            uint8_t pin = dev->pci_dev->interrupt_pin;
            if (pin >= 1 && pin <= 4) {
                uint8_t acpi_pin = pin - 1;
                uint8_t lookup_dev = dev->pci_dev->device;
                uint8_t lookup_pin = acpi_pin;
                if (dev->pci_dev->bus != 0) {
                    const pci_device_t* bridge =
                        pci_find_bridge_for_bus(dev->pci_dev->bus);
                    if (bridge) {
                        lookup_pin = (acpi_pin + dev->pci_dev->device) % 4;
                        lookup_dev = bridge->device;
                    }
                }
                if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
                                        lookup_dev, lookup_pin, &gsi) == 0
                    && gsi <= 23) {
                    irq = (uint8_t)gsi;
                    kprintf("VMXNET3: ACPI _PRT resolved INT%c -> GSI %u\n",
                            'A' + acpi_pin, gsi);
                }
            }
            if (irq == 0xFF) {
                irq = dev->pci_dev->interrupt_line;
                if (irq != 0xFF && irq <= 23) {
                    kprintf("VMXNET3: ACPI _PRT lookup failed, falling back to "
                            "PCI interrupt_line = %d\n", irq);
                } else {
                    kprintf("VMXNET3: WARNING: no valid IRQ\n");
                }
            }
            kprintf("VMXNET3: Using legacy IRQ %d\n", irq);
            g_vmxnet3_legacy_irq = irq;

            // Clear PCI Command INTx Disable if set
            {
                uint32_t cmd = pci_cfg_read32(dev->pci_dev->bus,
                                              dev->pci_dev->device,
                                              dev->pci_dev->function, 0x04);
                if (cmd & PCI_CMD_INTX_DISABLE) {
                    cmd &= ~PCI_CMD_INTX_DISABLE;
                    pci_cfg_write32(dev->pci_dev->bus,
                                    dev->pci_dev->device,
                                    dev->pci_dev->function, 0x04, cmd);
                    kprintf("VMXNET3: cleared PCI Command INTx Disable bit\n");
                }
            }

            if (irq <= 23) {
                uint8_t vector = 32 + irq;
                ioapic_configure_legacy_irq(irq, vector,
                                            IOAPIC_POLARITY_LOW,
                                            IOAPIC_TRIGGER_LEVEL);
            }
        } else {
            kprintf("VMXNET3: MSI enabled (vector %d)\n", dev->msi_vector);
        }

        // Initialise net_device fields BEFORE enabling device IRQs.
        dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("vmxnet3");
        dev->tx_lock = (spinlock_t)SPINLOCK_INIT("vmxnet3_tx");
        dev->net_dev.rx_packets = 0;
        dev->net_dev.tx_packets = 0;
        dev->net_dev.rx_bytes = 0;
        dev->net_dev.tx_bytes = 0;
        dev->net_dev.rx_errors = 0;
        dev->net_dev.tx_errors = 0;
        dev->net_dev.rx_dropped = 0;
        g_vmxnet3_initialized = 1;

        // Unmask interrupts at the device level.  IMR[0] = 0 enables
        // vector 0; intrCtrl=0 globally enables.
        vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 0);
        dev->shared->devRead.intrConf.intrCtrl = 0;
        __asm__ volatile("mfence" ::: "memory");

        // Initial link status
        uint32_t link = vm_cmd_get(dev, VMXNET3_CMD_GET_LINK);
        dev->link_up = (link & 0x1) ? 1 : 0;
        kprintf("VMXNET3: Link %s\n", dev->link_up ? "UP" : "DOWN");

        // Wire up net_device_t and register
        dev->net_dev.name = "eth0";
        for (int m = 0; m < ETH_ALEN; m++)
            dev->net_dev.mac_addr[m] = dev->mac_addr[m];
        dev->net_dev.mtu = NET_MTU_DEFAULT;
        dev->net_dev.ip_addr = 0;
        dev->net_dev.netmask = 0;
        dev->net_dev.gateway = 0;
        dev->net_dev.dns_server = 0;
        dev->net_dev.send = vmxnet3_send;
        dev->net_dev.link_status = vmxnet3_link_status;
        dev->net_dev.shutdown = vmxnet3_shutdown;
        dev->net_dev.driver_data = dev;

        net_register(&dev->net_dev);
        kprintf("VMXNET3: Driver initialized successfully\n");
        return;  // first device only
    }
}
