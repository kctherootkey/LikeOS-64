// LikeOS VMware vmxnet3 paravirtualized NIC Driver
//
// Implements UPT/Vmxnet3 revision 1, the lowest-common-denominator that
// every supporting host (ESXi, Workstation, Fusion, QEMU) accepts.  We
// run with a single TX queue and a single RX queue.  Interrupts are
// delivered as legacy INTx.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/nic/vmxnet3.h>
#include <kernel/dev/nic/nic_util.h>
#include <kernel/net/net.h>
#include <kernel/net/netdevice.h>
#include <kernel/net/skb.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/interrupt.h>
#include <kernel/mm/slab.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/ioapic.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/timer.h>
#include <kernel/uapi/bug.h>

static vmxnet3_dev_t g_vmxnet3;
int g_vmxnet3_initialized = 0;
int g_vmxnet3_legacy_irq = -1;

// ============================================================================
// MMIO accessors
// ============================================================================
static inline uint32_t vm_r32(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}
static inline void vm_w32(volatile uint8_t *base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

// ============================================================================
// PM Timer microsecond delay
// ============================================================================
static void vm_delay_us(uint32_t us)
{
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
// The command register: a "set" command (CAFE...) is a write, a "get"
// command (F00D...) a write and a read of the result.  One channel for all
// CPUs, so every use holds cmd_lock.
// ============================================================================
static uint32_t vm_cmd_get(vmxnet3_dev_t *dev, uint32_t cmd)
{
	uint64_t f;
	uint32_t v;

	spin_lock_irqsave(&dev->cmd_lock, &f);
	vm_w32(dev->bar1, VMXNET3_REG_CMD, cmd);
	v = vm_r32(dev->bar1, VMXNET3_REG_CMD);
	spin_unlock_irqrestore(&dev->cmd_lock, f);
	return v;
}
static void vm_cmd_set(vmxnet3_dev_t *dev, uint32_t cmd)
{
	uint64_t f;

	spin_lock_irqsave(&dev->cmd_lock, &f);
	vm_w32(dev->bar1, VMXNET3_REG_CMD, cmd);
	spin_unlock_irqrestore(&dev->cmd_lock, f);
}

// ============================================================================
// Map a memory BAR (bar0 or bar1, 4 KiB each), strictly uncached.
// Returns physical address through *phys_out and a kernel-virtual pointer.
// ============================================================================
static volatile uint8_t *vm_map_bar(const pci_device_t *pd, int bar_idx,
				    uint64_t *phys_out)
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

	uint64_t virt = mm_map_device_mmio(phys, 1);
	if (!virt) {
		kprintf("VMXNET3: cannot map BAR%d\n", bar_idx);
		return NULL;
	}
	return (volatile uint8_t *)virt;
}

// ============================================================================
// Allocate one DMA-capable page (must be below 4 GiB for 32-bit BAR-routed
// hosts; on QEMU vmxnet3 supports 64-bit DMA but we conservatively use
// the same identity-mapped allocator we use everywhere else).
// ============================================================================
static void *vm_alloc_dma(uint64_t *phys_out, size_t bytes)
{
	uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	uint64_t phys = mm_allocate_contiguous_pages(pages);
	if (!phys)
		return NULL;
	*phys_out = phys;
	void *virt = phys_to_virt(phys);
	// Zero
	uint64_t *p = (uint64_t *)virt;
	for (size_t i = 0; i < (pages * PAGE_SIZE) / 8; i++)
		p[i] = 0;
	return virt;
}

// ============================================================================
// Read MAC via host-visible registers (set after RESET_DEV by the host)
// ============================================================================
static void vm_read_mac(vmxnet3_dev_t *dev)
{
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
// Descriptor rings + buffers
// ============================================================================

/* Put every ring in its initial state: RX ring 0 all the device's
 * (generation 1), ring 2 all software's, the TX ring and both completion
 * rings empty.  At init and when the device is reset. */
static void vm_rings_reset(vmxnet3_dev_t *dev)
{
	for (int i = 0; i < VMXNET3_NUM_RX_DESC; i++) {
		dev->rx_ring[i].addr = dev->rx_bufs_phys[i];
		// val = len[0..13] | btype[14] | dtype[15] | gen[31]
		dev->rx_ring[i].val = (VMXNET3_RX_BUF_SIZE & 0x3FFF) |
				      (0u << 14) | // btype = head
				      (1u << 31);
		dev->rx_ring[i].reserved = 0;
	}
	/* All N descriptors were just filled with generation 1, so the fill
	 * position has gone once round the ring: the next refill -- of
	 * descriptor 0, after the device consumed it -- is for the device's
	 * SECOND pass, which expects generation 0.  The generation flips
	 * with every wrap of the fill position (vm_rx_clean). */
	dev->rx_gen = 0;
	dev->rx_prod = 0;

	// Ring 2 — required by the host (validated for size & alignment) but
	// we don't use it.  Mark every entry software-owned (generation 0;
	// the device starts at 1) so the device skips them.
	for (int i = 0; i < VMXNET3_NUM_RX2_DESC; i++) {
		dev->rx_ring2[i].addr = 0;
		dev->rx_ring2[i].val = 0;
		dev->rx_ring2[i].reserved = 0;
	}
	for (int i = 0; i < VMXNET3_NUM_RX_COMP; i++) {
		dev->rx_comp_ring[i].word0 = 0;
		dev->rx_comp_ring[i].word1 = 0;
		dev->rx_comp_ring[i].word2 = 0;
		dev->rx_comp_ring[i].word3 = 0;
	}

	// TX descriptors — software-owned (gen flips on first post)
	for (int i = 0; i < VMXNET3_NUM_TX_DESC; i++) {
		dev->tx_ring[i].addr = 0;
		dev->tx_ring[i].val1 = 0;
		dev->tx_ring[i].val2 = 0;
	}
	for (int i = 0; i < VMXNET3_NUM_TX_COMP; i++) {
		dev->tx_comp_ring[i].word0 = 0;
		dev->tx_comp_ring[i].word1 = 0;
		dev->tx_comp_ring[i].word2 = 0;
		dev->tx_comp_ring[i].word3 = 0;
	}
	dev->tx_gen = 1;
	dev->tx_prod = 0;
	dev->tx_cons = 0;
	dev->tx_inflight = 0;
	dev->tx_comp_next = 0;
	dev->tx_comp_gen = 1;
	dev->rx_comp_next = 0;
	dev->rx_comp_gen = 1;
}

static int vm_alloc_rings(vmxnet3_dev_t *dev)
{
	dev->tx_ring = (vmxnet3_tx_desc_t *)vm_alloc_dma(
		&dev->tx_ring_phys,
		sizeof(vmxnet3_tx_desc_t) * VMXNET3_NUM_TX_DESC);
	if (!dev->tx_ring)
		return -1;
	dev->tx_comp_ring = (vmxnet3_tx_comp_desc_t *)vm_alloc_dma(
		&dev->tx_comp_ring_phys,
		sizeof(vmxnet3_tx_comp_desc_t) * VMXNET3_NUM_TX_COMP);
	if (!dev->tx_comp_ring)
		return -1;
	dev->rx_ring = (vmxnet3_rx_desc_t *)vm_alloc_dma(
		&dev->rx_ring_phys,
		sizeof(vmxnet3_rx_desc_t) * VMXNET3_NUM_RX_DESC);
	if (!dev->rx_ring)
		return -1;
	// RX ring 1: required by the device, never used
	dev->rx_ring2 = (vmxnet3_rx_desc_t *)vm_alloc_dma(
		&dev->rx_ring2_phys,
		sizeof(vmxnet3_rx_desc_t) * VMXNET3_NUM_RX2_DESC);
	if (!dev->rx_ring2)
		return -1;
	dev->rx_comp_ring = (vmxnet3_rx_comp_desc_t *)vm_alloc_dma(
		&dev->rx_comp_ring_phys,
		sizeof(vmxnet3_rx_comp_desc_t) * VMXNET3_NUM_RX_COMP);
	if (!dev->rx_comp_ring)
		return -1;

	// Per-slot TX/RX buffers (one page each — overkill but simple)
	for (int i = 0; i < VMXNET3_NUM_TX_DESC; i++) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys)
			return -1;
		dev->tx_bufs[i] = (uint8_t *)phys_to_virt(phys);
		dev->tx_bufs_phys[i] = phys;
	}
	for (int i = 0; i < VMXNET3_NUM_RX_DESC; i++) {
		uint64_t phys = mm_allocate_physical_page();
		if (!phys)
			return -1;
		dev->rx_bufs[i] = (uint8_t *)phys_to_virt(phys);
		dev->rx_bufs_phys[i] = phys;
	}

	// Multicast filter table: NETDEV_MC_MAX addresses of 6 bytes
	dev->mf_table = (uint8_t *)vm_alloc_dma(&dev->mf_table_phys,
						NETDEV_MC_MAX * 6);
	if (!dev->mf_table)
		return -1;

	vm_rings_reset(dev);
	return 0;
}

// ============================================================================
// Build DriverShared, TxQueueDesc, RxQueueDesc
// ============================================================================
static int vm_setup_shared(vmxnet3_dev_t *dev)
{
	// Allocate one page for DriverShared + queue descs (each well under 1KB)
	uint64_t phys;
	void *p = vm_alloc_dma(&phys, PAGE_SIZE);
	if (!p)
		return -1;

	dev->shared = (vmxnet3_driver_shared_t *)p;
	dev->shared_phys = phys;

	// Lay TxQueueDesc and RxQueueDesc immediately after DriverShared.
	// The host walks the table using sizeof(Vmxnet3_TxQueueDesc) /
	// sizeof(Vmxnet3_RxQueueDesc), so they MUST be packed back-to-back
	// with no inter-queue padding.  queueDescPA itself must be 128-byte
	// aligned.
	uint64_t off = (sizeof(vmxnet3_driver_shared_t) + 127) & ~127ULL;
	dev->txqd = (vmxnet3_tx_queue_desc_t *)((uint8_t *)p + off);
	dev->txqd_phys = phys + off;

	off += sizeof(vmxnet3_tx_queue_desc_t);
	dev->rxqd = (vmxnet3_rx_queue_desc_t *)((uint8_t *)p + off);
	dev->rxqd_phys = phys + off;

	// -- DriverShared -------------------------------------------------------
	vmxnet3_driver_shared_t *s = dev->shared;
	s->magic = VMXNET3_REV1_MAGIC;
	s->pad = 0;

	vmxnet3_misc_conf_t *mc = &s->devRead.misc;
	mc->driverInfo.version = 1;
	// gos packed: bits 0-1 = bits, bits 2-5 = type, bits 6-21 = ver, 22-31 misc
	// 64-bit guest of the conventional flavour: bits=2, type=1
	mc->driverInfo.gos = 2u | (1u << 2);
	mc->driverInfo.vmxnet3RevSpt = 0x1; // rev1
	mc->driverInfo.uptVerSpt = 0x1; // UPT1

	// the device verifies receive checksums (vm_rx_clean); no LRO/RSS
	mc->uptFeatures = VMXNET3_UPT1_F_RXCSUM;
	mc->ddPA = dev->shared_phys; // back-pointer to ourselves
	mc->ddLen = sizeof(vmxnet3_driver_shared_t);
	mc->queueDescPA = dev->txqd_phys;
	mc->queueDescLen = sizeof(vmxnet3_tx_queue_desc_t) +
			   sizeof(vmxnet3_rx_queue_desc_t);
	mc->mtu = NET_MTU_DEFAULT;
	mc->maxNumRxSG = 1;
	mc->numTxQueues = 1;
	mc->numRxQueues = 1;

	vmxnet3_intr_conf_t *ic = &s->devRead.intrConf;
	ic->autoMask = 0;
	ic->numIntrs = 1; // single legacy/MSI vector for everything
	ic->eventIntrIdx = 0;
	/* Adaptive interrupt moderation: the device paces its interrupts
	 * by the load. */
	for (int i = 0; i < VMXNET3_MAX_INTRS; i++)
		ic->modLevels[i] = VMXNET3_IML_ADAPTIVE;
	ic->intrCtrl = VMXNET3_IC_DISABLE_ALL; // start masked; we'll enable

	vmxnet3_rx_filter_conf_t *rc = &s->devRead.rxFilterConf;
	rc->rxMode = VMXNET3_RXM_UCAST | VMXNET3_RXM_BCAST;
	rc->mfTableLen = 0;
	rc->mfTablePA = dev->mf_table_phys;
	for (int i = 0; i < VMXNET3_VFT_SIZE; i++)
		rc->vfTable[i] = 0xFFFFFFFF; // accept all VLANs

	s->devRead.rssConfDesc.confLen = 0;
	s->devRead.rssConfDesc.confPA = 0;
	s->devRead.pmConfDesc.confLen = 0;
	s->devRead.pmConfDesc.confPA = 0;
	s->devRead.pluginConfDesc.confLen = 0;
	s->devRead.pluginConfDesc.confPA = 0;

	// -- Tx Queue Desc ------------------------------------------------------
	vmxnet3_tx_queue_desc_t *tq = dev->txqd;
	tq->ctrl.txNumDeferred = 0;
	tq->ctrl.txThreshold = 1; // the device raises it while it polls
	tq->conf.txRingBasePA = dev->tx_ring_phys;
	tq->conf.dataRingBasePA = 0;
	tq->conf.compRingBasePA = dev->tx_comp_ring_phys;
	tq->conf.ddPA = 0;
	tq->conf.txRingSize = VMXNET3_NUM_TX_DESC;
	tq->conf.dataRingSize = 0;
	tq->conf.compRingSize = VMXNET3_NUM_TX_COMP;
	tq->conf.ddLen = 0;
	tq->conf.intrIdx = 0;
	tq->conf.txDataRingDescSize = 0;

	// -- Rx Queue Desc ------------------------------------------------------
	vmxnet3_rx_queue_desc_t *rq = dev->rxqd;
	rq->conf.rxRingBasePA[0] = dev->rx_ring_phys;
	rq->conf.rxRingBasePA[1] = dev->rx_ring2_phys;
	rq->conf.compRingBasePA = dev->rx_comp_ring_phys;
	rq->conf.ddPA = 0;
	rq->conf.rxRingSize[0] = VMXNET3_NUM_RX_DESC;
	rq->conf.rxRingSize[1] = VMXNET3_NUM_RX2_DESC;
	rq->conf.compRingSize = VMXNET3_NUM_RX_COMP;
	rq->conf.ddLen = 0;
	rq->conf.intrIdx = 0;
	return 0;
}

/* Hand DriverShared to the device and activate it. */
static int vm_activate(vmxnet3_dev_t *dev)
{
	uint64_t f;
	uint32_t status;

	// CRITICAL: fence BEFORE the MMIO writes so that all the cacheable
	// (WB) stores into *shared / *txqd / *rxqd are globally observable
	// before the device starts looking at them.  UC stores to BAR1 are
	// otherwise allowed to reorder ahead of pending WB stores.
	__asm__ volatile("mfence" ::: "memory");

	vm_w32(dev->bar1, VMXNET3_REG_DSAL,
	       (uint32_t)(dev->shared_phys & 0xFFFFFFFF));
	vm_w32(dev->bar1, VMXNET3_REG_DSAH, (uint32_t)(dev->shared_phys >> 32));

	__asm__ volatile("mfence" ::: "memory");

	// ACTIVATE_DEV: host validates the entire DriverShared blob; result
	// is read back from VMXNET3_REG_CMD (0 = success, non-zero = error).
	spin_lock_irqsave(&dev->cmd_lock, &f);
	vm_w32(dev->bar1, VMXNET3_REG_CMD, VMXNET3_CMD_ACTIVATE_DEV);
	status = vm_r32(dev->bar1, VMXNET3_REG_CMD);
	spin_unlock_irqrestore(&dev->cmd_lock, f);
	WARN_ON_ONCE(
		status !=
		0); /* VMXNET3 ACTIVATE_DEV failed - DriverShared validation error */
	if (status != 0) {
		kprintf("VMXNET3: ACTIVATE_DEV failed (status=0x%x)\n", status);
		return -1;
	}

	// Kick the RX producer so the device knows we have buffers ready.
	// RXPROD register is at BAR0 offset 0x800, indexed by queue (stride 8).
	vm_w32(dev->bar0, VMXNET3_REG_RXPROD, VMXNET3_NUM_RX_DESC - 1);
	vm_w32(dev->bar0, VMXNET3_REG_RXPROD2, 0);
	return 0;
}

// ============================================================================
// Transmit (net_device_t xmit hook)
// ============================================================================
#define VMXNET3_TX_WAKE_THRESH 32

static uint32_t vm_tx_free(const vmxnet3_dev_t *dev)
{
	// one slot always stays empty
	return VMXNET3_NUM_TX_DESC - 1u - dev->tx_inflight;
}

static void vm_tx_clean_locked(vmxnet3_dev_t *dev);

/* Ring the transmit doorbell for what was deferred.  Caller holds
 * tx_lock. */
static void vm_tx_kick_locked(vmxnet3_dev_t *dev)
{
	volatile vmxnet3_tx_queue_ctrl_t *c = &dev->txqd->ctrl;

	c->txNumDeferred = 0;
	vm_w32(dev->bar0, VMXNET3_REG_TXPROD, dev->tx_prod);
}

static int vmxnet3_xmit(sk_buff_t *skb, net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;
	uint32_t len = skb->len;
	uint64_t f;

	BUG_ON(dev == NULL);
	if (len == 0 || len > VMXNET3_TX_BUF_SIZE) {
		ndev->tx_dropped++;
		skb_put(skb);
		return NETDEV_TX_OK;
	}
	uint32_t out_len = (len < 60) ? 60 : len;

	spin_lock_irqsave(&dev->tx_lock, &f);
	if (vm_tx_free(dev) == 0) {
		vm_tx_clean_locked(dev);
		if (vm_tx_free(dev) == 0) {
			netif_stop_queue(ndev);
			spin_unlock_irqrestore(&dev->tx_lock, f);
			return NETDEV_TX_BUSY;
		}
	}

	uint16_t slot = dev->tx_prod;
	vmxnet3_tx_desc_t *td = &dev->tx_ring[slot];

	mm_memcpy(dev->tx_bufs[slot], skb->data, len);
	if (out_len > len)
		mm_memset(dev->tx_bufs[slot] + len, 0, out_len - len);

	td->addr = dev->tx_bufs_phys[slot];
	// val1 (LE bitfields, low->high): len[0:13] | gen[14] | rsvd[15] |
	//                                  dtype[16] | ext1[17] | msscof[18:31]
	uint32_t val1 =
		((uint32_t)(out_len & 0x3FFF)) | ((uint32_t)dev->tx_gen << 14);
	// val2 (LE bitfields, low->high): hlen[0:9] | om[10:11] | eop[12] |
	//                                   cq[13] | ext2[14] | ti[15] | tci[16:31]
	uint32_t val2 = (1u << 12) | (1u << 13);

	// Per spec, val1 must be written LAST (the gen bit transfers ownership)
	td->val2 = val2;
	__asm__ volatile("" ::: "memory");
	*(volatile uint32_t *)&td->val1 = val1;
	__asm__ volatile("mfence" ::: "memory");

	dev->tx_inflight++;
	if (++dev->tx_prod >= VMXNET3_NUM_TX_DESC) {
		dev->tx_prod = 0;
		dev->tx_gen ^= 1;
	}

	/* The doorbell only once the device asks for it: it raises
	 * txThreshold while it is polling the ring by itself. */
	{
		volatile vmxnet3_tx_queue_ctrl_t *c = &dev->txqd->ctrl;
		uint32_t deferred = c->txNumDeferred + 1;

		c->txNumDeferred = deferred;
		if (deferred >= c->txThreshold)
			vm_tx_kick_locked(dev);
	}

	ndev->tx_packets++;
	ndev->tx_bytes += out_len;
	if (vm_tx_free(dev) == 0)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	skb_put(skb);
	return NETDEV_TX_OK;
}

// ============================================================================
// Drain the TX completion ring.  Caller holds dev->tx_lock.
// ============================================================================
static void vm_tx_clean_locked(vmxnet3_dev_t *dev)
{
	while (1) {
		vmxnet3_tx_comp_desc_t *cd =
			&dev->tx_comp_ring[dev->tx_comp_next];
		// Device-written: volatile, gen word first.
		uint8_t gen = (*(volatile uint32_t *)&cd->word3 >> 31) & 1;
		if (gen != dev->tx_comp_gen)
			break;
		__asm__ volatile("" ::: "memory");

		/* txdIdx is the last descriptor of the completed frame; every
		 * descriptor up to it is the device's no longer. */
		uint16_t idx = (uint16_t)(*(volatile uint32_t *)&cd->word0 &
					  0xFFF);
		if (idx < VMXNET3_NUM_TX_DESC) {
			uint16_t freed = (uint16_t)((idx + VMXNET3_NUM_TX_DESC -
						     dev->tx_cons) %
							    VMXNET3_NUM_TX_DESC +
						    1);
			WARN_ON_ONCE(freed > dev->tx_inflight);
			dev->tx_inflight = freed > dev->tx_inflight ?
						   0 :
						   (uint16_t)(dev->tx_inflight -
							      freed);
			dev->tx_cons = (uint16_t)((idx + 1) %
						  VMXNET3_NUM_TX_DESC);
			dev->tx_progress_tick = timer_ticks();
		}

		if (++dev->tx_comp_next >= VMXNET3_NUM_TX_COMP) {
			dev->tx_comp_next = 0;
			dev->tx_comp_gen ^= 1;
		}
	}
	if (dev->tx_inflight == 0)
		dev->tx_progress_tick = timer_ticks();
	if (netif_queue_stopped(&dev->net_dev) &&
	    vm_tx_free(dev) >= VMXNET3_TX_WAKE_THRESH)
		netif_wake_queue(&dev->net_dev);
}

// ============================================================================
// Receive (NAPI poll)
// ============================================================================
static void vm_rx_deliver(vmxnet3_dev_t *dev, const uint8_t *buf,
			  uint32_t len, uint32_t w0, uint32_t w3)
{
	net_device_t *nd = &dev->net_dev;
	sk_buff_t *skb = skb_alloc(len);

	if (!skb) {
		nd->rx_dropped++;
		return;
	}
	skb->dev = nd;
	mm_memcpy(skb_append(skb, len), buf, len);
	if ((nd->features & NETIF_F_RXCSUM) && nic_vmxnet3_rx_csum_ok(w0, w3))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	nd->rx_packets++;
	nd->rx_bytes += len;
	if (nic_is_multicast(buf))
		nd->multicast++;
	napi_gro_receive(&dev->napi, skb);
}

static int vm_rx_clean(vmxnet3_dev_t *dev, int budget)
{
	int work = 0, refilled = 0;

	while (work < budget) {
		vmxnet3_rx_comp_desc_t *cd =
			&dev->rx_comp_ring[dev->rx_comp_next];

		// Generation bit is bit 31 of word 3.  The device writes the
		// completion behind the compiler's back: read the gen word
		// through volatile, and the other words only after it.
		uint32_t w3 = *(volatile uint32_t *)&cd->word3;
		if (((w3 >> 31) & 1) != dev->rx_comp_gen)
			break; // not ours yet
		__asm__ volatile("" ::: "memory");

		uint32_t w0 = *(volatile uint32_t *)&cd->word0;
		uint32_t w2 = *(volatile uint32_t *)&cd->word2;
		uint16_t rxd_idx = (uint16_t)(w0 & 0xFFF);
		// word0 LE bitfields: rxdIdx[0:11] | ext1[12:13] | eop[14] |
		//                      sop[15] | rqID[16:25] | rssType[26:29] | cnc[30] | ext2[31]
		uint8_t eop = (w0 >> 14) & 1;
		uint8_t sop = (w0 >> 15) & 1;
		// word2 LE bitfields: len[0:13] | err[14] | ts[15] | tci[16:31]
		uint16_t length = (uint16_t)(w2 & 0x3FFF);
		uint8_t err = (w2 >> 14) & 1;

		if (err || !eop || !sop || length == 0 ||
		    length > VMXNET3_RX_BUF_SIZE) {
			dev->net_dev.rx_errors++;
		} else if (rxd_idx < VMXNET3_NUM_RX_DESC) {
			vm_rx_deliver(dev, dev->rx_bufs[rxd_idx], length, w0,
				      w3);
		}

		// Re-arm the corresponding RX descriptor with the fill
		// generation (which flips when the fill position wraps, below).
		if (rxd_idx < VMXNET3_NUM_RX_DESC) {
			/* The device consumes ring 0 in order and we refill each
			 * descriptor as it completes, so the completed index IS
			 * the fill position. */
			WARN_ON_ONCE(rxd_idx != dev->rx_prod);
			dev->rx_ring[rxd_idx].addr = dev->rx_bufs_phys[rxd_idx];
			dev->rx_ring[rxd_idx].reserved = 0;
			__asm__ volatile("" ::: "memory");
			*(volatile uint32_t *)&dev->rx_ring[rxd_idx].val =
				(VMXNET3_RX_BUF_SIZE & 0x3FFF) |
				((uint32_t)dev->rx_gen << 31);
			refilled = 1;
			if (++dev->rx_prod >= VMXNET3_NUM_RX_DESC) {
				dev->rx_prod = 0;
				dev->rx_gen ^= 1;
			}
		}

		// Advance comp ring pointer; flip expected gen on wrap
		if (++dev->rx_comp_next >= VMXNET3_NUM_RX_COMP) {
			dev->rx_comp_next = 0;
			dev->rx_comp_gen ^= 1;
		}
		work++;
	}

	/* The producer register only when the device asks for it
	 * (updateRxProd); otherwise it finds the refilled descriptors by
	 * their generation bit. */
	if (refilled &&
	    *(volatile uint8_t *)&dev->rxqd->ctrl.updateRxProd) {
		__asm__ volatile("mfence" ::: "memory");
		vm_w32(dev->bar0, VMXNET3_REG_RXPROD, dev->rx_prod);
	}
	return work;
}

/* Device events: acknowledged here, acted on by the watchdog. */
static void vm_process_events(vmxnet3_dev_t *dev)
{
	uint32_t ev = *(volatile uint32_t *)&dev->shared->ecr;

	if (!ev)
		return;
	vm_w32(dev->bar1, VMXNET3_REG_ECR, ev); // W1C
	if (ev & VMXNET3_ECR_LINK)
		__atomic_store_n(&dev->link_change_pending, 1,
				 __ATOMIC_RELEASE);
	if (ev & (VMXNET3_ECR_TQERR | VMXNET3_ECR_RQERR))
		__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

static int vmxnet3_poll(struct napi_struct *n, int budget)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)n->dev->driver_data;
	uint64_t f;
	int work;

	vm_process_events(dev);
	spin_lock_irqsave(&dev->tx_lock, &f);
	vm_tx_clean_locked(dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	work = vm_rx_clean(dev, budget);
	if (work < budget && napi_complete_done(n, work))
		vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 0); // unmask vector 0
	return work;
}

// ============================================================================
// Link, receive filter, reset, statistics, watchdog
// ============================================================================
static void vm_link_update(vmxnet3_dev_t *dev)
{
	uint32_t link = vm_cmd_get(dev, VMXNET3_CMD_GET_LINK);
	int up = (link & 0x1) ? 1 : 0;

	dev->link_speed = up ? link >> 16 : 0;
	if (up == dev->link_up)
		return;
	dev->link_up = up;
	if (up) {
		kprintf("VMXNET3: Link UP (%u Mb/s)\n", dev->link_speed);
		netif_carrier_on(&dev->net_dev);
	} else {
		kprintf("VMXNET3: Link DOWN\n");
		netif_carrier_off(&dev->net_dev);
		dhcp_invalidate(&dev->net_dev);
	}
}

static int vmxnet3_link_status(net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;
	int up = (vm_cmd_get(dev, VMXNET3_CMD_GET_LINK) & 0x1) ? 1 : 0;

	if (up != dev->link_up) {
		__atomic_store_n(&dev->link_change_pending, 1,
				 __ATOMIC_RELEASE);
		net_housekeeping_kick();
	}
	return up;
}

/* The receive filter from the multicast list and the device flags: the
 * multicast addresses go to the device's exact-match table, more than fit
 * (or IFF_ALLMULTI) receive all multicast. */
static void vm_apply_rx_mode(vmxnet3_dev_t *dev)
{
	uint8_t mc[NETDEV_MC_MAX][6];
	int all = 0;
	int n = dev_mc_snapshot(&dev->net_dev, mc, NETDEV_MC_MAX, &all);
	uint32_t flags = __atomic_load_n(&dev->net_dev.flags, __ATOMIC_ACQUIRE);
	vmxnet3_rx_filter_conf_t *rc = &dev->shared->devRead.rxFilterConf;
	uint32_t mode = VMXNET3_RXM_UCAST | VMXNET3_RXM_BCAST;

	for (int i = 0; i < n; i++)
		for (int j = 0; j < 6; j++)
			dev->mf_table[i * 6 + j] = mc[i][j];
	if (flags & IFF_PROMISC)
		mode |= VMXNET3_RXM_PROMISC;
	if (all || (flags & IFF_ALLMULTI))
		mode |= VMXNET3_RXM_ALL_MULTI;
	else if (n > 0)
		mode |= VMXNET3_RXM_MCAST;
	rc->mfTableLen = (uint16_t)(n * 6);
	rc->mfTablePA = dev->mf_table_phys;
	rc->rxMode = mode;
	__asm__ volatile("mfence" ::: "memory");
	vm_cmd_set(dev, VMXNET3_CMD_UPDATE_RX_MODE);
	vm_cmd_set(dev, VMXNET3_CMD_UPDATE_MAC_FILTERS);
}

static void vmxnet3_set_rx_mode(net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->rx_mode_pending, 1, __ATOMIC_RELEASE);
	net_housekeeping_kick();
}

/* Quiesce and reset the device, put every ring back to its initial state
 * and activate it again: after a queue error event or a transmit hang.
 * Watchdog context (CPU 0, which also runs the poll, so the poll is not
 * running); tx_lock keeps the transmit hook out, the vector stays masked
 * until the device is back. */
static void vm_reset_device(vmxnet3_dev_t *dev)
{
	uint64_t f;
	uint32_t dropped;

	dev->st_resets++;
	vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 1);
	spin_lock_irqsave(&dev->tx_lock, &f);
	dropped = dev->tx_inflight;
	vm_cmd_set(dev, VMXNET3_CMD_QUIESCE_DEV);
	vm_cmd_set(dev, VMXNET3_CMD_RESET_DEV);
	vm_rings_reset(dev);
	dev->txqd->ctrl.txNumDeferred = 0;
	dev->txqd->ctrl.txThreshold = 1;
	dev->shared->devRead.intrConf.intrCtrl = 0;
	if (vm_activate(dev) < 0)
		kprintf("VMXNET3: reactivation after reset failed\n");
	dev->net_dev.tx_errors += dropped;
	dev->tx_progress_tick = timer_ticks();
	if (netif_queue_stopped(&dev->net_dev))
		netif_wake_queue(&dev->net_dev);
	spin_unlock_irqrestore(&dev->tx_lock, f);
	// the filter and the link as they are now
	vm_apply_rx_mode(dev);
	__atomic_store_n(&dev->link_change_pending, 1, __ATOMIC_RELEASE);
	vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 0);
	// frames that completed meanwhile: let the poll look
	napi_schedule(&dev->napi);
}

static void vm_update_stats(vmxnet3_dev_t *dev)
{
	vm_cmd_set(dev, VMXNET3_CMD_GET_STATS);
	__asm__ volatile("mfence" ::: "memory");
	// UPT1 counters: TX [8] errors [9] discards; RX [8] out of buffers
	// [9] errors -- cumulative, kept by the device
	dev->st_tx_err = ((volatile uint64_t *)dev->txqd->stats.s)[8];
	dev->st_tx_discard = ((volatile uint64_t *)dev->txqd->stats.s)[9];
	dev->st_rx_nobuf = ((volatile uint64_t *)dev->rxqd->stats.s)[8];
	dev->st_rx_err = ((volatile uint64_t *)dev->rxqd->stats.s)[9];
}

static void vmxnet3_get_stats64(net_device_t *ndev,
				struct net_device_stats64 *s)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;

	s->rx_missed_errors = dev->st_rx_nobuf;
	s->rx_errors += dev->st_rx_err;
	s->tx_errors += dev->st_tx_err;
	s->tx_dropped += dev->st_tx_discard;
}

static void vmxnet3_tx_timeout(net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;

	__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
}

// Network housekeeping softirq, CPU 0, ~10 Hz and when kicked.
static void vmxnet3_watchdog(net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;
	uint64_t now = timer_ticks(), f;
	int hang = 0;

	if (!dev || !g_vmxnet3_initialized)
		return;
	if (__atomic_exchange_n(&dev->link_change_pending, 0,
				__ATOMIC_ACQ_REL) ||
	    now - dev->link_poll_tick >= net_ms_to_ticks(1000)) {
		dev->link_poll_tick = now;
		vm_link_update(dev);
	}
	if (__atomic_exchange_n(&dev->rx_mode_pending, 0, __ATOMIC_ACQ_REL))
		vm_apply_rx_mode(dev);

	spin_lock_irqsave(&dev->tx_lock, &f);
	vm_tx_clean_locked(dev);
	// deferred frames the device has not picked up: ring the doorbell
	if (dev->txqd->ctrl.txNumDeferred)
		vm_tx_kick_locked(dev);
	if (dev->tx_inflight && dev->link_up &&
	    now - dev->tx_progress_tick > net_ms_to_ticks(5000))
		hang = 1;
	spin_unlock_irqrestore(&dev->tx_lock, f);
	if (hang) {
		kprintf("VMXNET3: transmit stalled, resetting the device\n");
		__atomic_store_n(&dev->reset_pending, 1, __ATOMIC_RELEASE);
	}
	if (__atomic_exchange_n(&dev->reset_pending, 0, __ATOMIC_ACQ_REL))
		vm_reset_device(dev);
	if (now - dev->stats_tick >= net_ms_to_ticks(2000)) {
		dev->stats_tick = now;
		vm_update_stats(dev);
	}
}

// Quiesce the controller ahead of an ACPI S5 transition.  The vmxnet3
// control plane exposes explicit QUIESCE_DEV and RESET_DEV commands;
// QUIESCE halts RX/TX rings and stops the device from issuing further
// DMA, RESET puts the device into the post-power-on state.  Combined
// with masking BAR0 IMR[0] and dropping PCI bus-master, the hypervisor
// (or the platform when running on bare-metal-style passthrough) will
// not see any pending PCI activity at sleep time.
static void vmxnet3_shutdown(net_device_t *ndev)
{
	vmxnet3_dev_t *dev = (vmxnet3_dev_t *)ndev->driver_data;
	if (!dev || !dev->bar1)
		return;

	if (dev->shared) {
		dev->shared->devRead.intrConf.intrCtrl = VMXNET3_IC_DISABLE_ALL;
	}
	if (dev->bar0) {
		vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 1); // mask vector 0
	}
	vm_cmd_set(dev, VMXNET3_CMD_QUIESCE_DEV);
	vm_cmd_set(dev, VMXNET3_CMD_RESET_DEV);

	if (dev->pci_dev) {
		const pci_device_t *pdev = dev->pci_dev;
		uint32_t cmd = pci_cfg_read32(pdev->bus, pdev->device,
					      pdev->function, 0x04);
		cmd &= ~0x0004u; // clear Bus Master Enable
		pci_cfg_write32(pdev->bus, pdev->device, pdev->function, 0x04,
				cmd);
	}
}

// ============================================================================
// Top-half IRQ handler
// ============================================================================
/* Mask the vector and hand everything -- completions and events -- to the
 * NAPI poll, which unmasks it when it is done.  Returns 1 when the
 * interrupt was this device's, 0 otherwise (shared legacy line); the
 * dispatcher acknowledges the local APIC. */
int vmxnet3_irq_handler(void)
{
	if (!g_vmxnet3_initialized)
		return 0;

	// Feed entropy from NIC IRQ timing
	{
		extern void entropy_add_interrupt_timing(uint64_t extra);
		uint32_t lo, hi;
		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		entropy_add_interrupt_timing(((uint64_t)hi << 32) | lo);
	}

	vmxnet3_dev_t *dev = &g_vmxnet3;
	BUG_ON(dev == NULL);

	// For legacy INTx, reading ICR both clears and tells us "is this
	// ours?".  With MSI the vector is ours alone.
	if (dev->msi_vector == 0) {
		uint32_t icr = vm_r32(dev->bar1, VMXNET3_REG_ICR);
		if (icr == 0)
			return 0; // not ours — line is shared in legacy mode
	}
	vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 1); // mask vector 0
	napi_schedule(&dev->napi);
	return 1;
}

// ============================================================================
// Driver entry point
// ============================================================================
void vmxnet3_init(void)
{
	int count;
	const pci_device_t *devs = pci_get_devices(&count);

	for (int i = 0; i < count; i++) {
		if (devs[i].vendor_id != VMXNET3_VENDOR_ID)
			continue;
		if (devs[i].device_id != VMXNET3_DEV_ID)
			continue;

		kprintf("VMXNET3: Found vmxnet3 NIC (PCI %02x:%02x.%x)\n",
			devs[i].bus, devs[i].device, devs[i].function);

		vmxnet3_dev_t *dev = &g_vmxnet3;
		dev->pci_dev = &devs[i];
		dev->cmd_lock = (spinlock_t)SPINLOCK_INIT("vmxnet3_cmd");
		dev->tx_lock = (spinlock_t)SPINLOCK_INIT("vmxnet3_tx");

		pci_enable_busmaster_mem(dev->pci_dev);

		dev->bar0 = vm_map_bar(dev->pci_dev, 0, &dev->bar0_phys);
		dev->bar1 = vm_map_bar(dev->pci_dev, 1, &dev->bar1_phys);
		if (!dev->bar0 || !dev->bar1)
			return;

		// Negotiate revisions:
		//   Read VRRS to discover supported revs, write back the highest
		//   bit we support (rev 1).  Same for UVRS / UPT.
		uint32_t vrrs = vm_r32(dev->bar1, VMXNET3_REG_VRRS);
		if (!(vrrs & VMXNET3_VERSION_SELECT)) {
			kprintf("VMXNET3: revision 1 not supported (VRRS=0x%x)\n",
				vrrs);
			return;
		}
		vm_w32(dev->bar1, VMXNET3_REG_VRRS, VMXNET3_VERSION_SELECT);

		uint32_t uvrs = vm_r32(dev->bar1, VMXNET3_REG_UVRS);
		if (!(uvrs & VMXNET3_UPT_VERSION_SELECT)) {
			kprintf("VMXNET3: UPT1 not supported (UVRS=0x%x)\n",
				uvrs);
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
		if (vm_setup_shared(dev) < 0) {
			kprintf("VMXNET3: failed to allocate the shared area\n");
			return;
		}
		if (vm_activate(dev) < 0)
			return;

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
					const pci_device_t *bridge =
						pci_find_bridge_for_bus(
							dev->pci_dev->bus);
					if (bridge) {
						lookup_pin =
							(acpi_pin +
							 dev->pci_dev->device) %
							4;
						lookup_dev = bridge->device;
					}
				}
				if (acpi_pci_lookup_irq("\\\\_SB_.PCI0",
							lookup_dev, lookup_pin,
							&gsi) == 0 &&
				    gsi <= 23) {
					irq = (uint8_t)gsi;
					kprintf("VMXNET3: ACPI _PRT resolved INT%c -> GSI %u\n",
						'A' + acpi_pin, gsi);
				}
			}
			if (irq == 0xFF) {
				irq = dev->pci_dev->interrupt_line;
				if (irq != 0xFF && irq <= 23) {
					kprintf("VMXNET3: ACPI _PRT lookup failed, falling back to "
						"PCI interrupt_line = %d\n",
						irq);
				} else {
					kprintf("VMXNET3: WARNING: no valid IRQ\n");
				}
			}
			kprintf("VMXNET3: Using legacy IRQ %d\n", irq);
			g_vmxnet3_legacy_irq = irq;

			// Clear PCI Command INTx Disable if set
			{
				uint32_t cmd = pci_cfg_read32(
					dev->pci_dev->bus, dev->pci_dev->device,
					dev->pci_dev->function, 0x04);
				if (cmd & PCI_CMD_INTX_DISABLE) {
					cmd &= ~PCI_CMD_INTX_DISABLE;
					pci_cfg_write32(dev->pci_dev->bus,
							dev->pci_dev->device,
							dev->pci_dev->function,
							0x04, cmd);
					kprintf("VMXNET3: cleared PCI Command INTx Disable bit\n");
				}
			}

			if (irq <= 23) {
				uint8_t vector = 32 + irq;
				ioapic_configure_legacy_irq(
					irq, vector, IOAPIC_POLARITY_LOW,
					IOAPIC_TRIGGER_LEVEL);
			}
		} else {
			kprintf("VMXNET3: MSI enabled (vector %d)\n",
				dev->msi_vector);
		}

		// Initialise net_device fields BEFORE enabling device IRQs.
		dev->net_dev.lock = (spinlock_t)SPINLOCK_INIT("vmxnet3");
		dev->net_dev.rx_packets = 0;
		dev->net_dev.tx_packets = 0;
		dev->net_dev.rx_bytes = 0;
		dev->net_dev.tx_bytes = 0;
		dev->net_dev.rx_errors = 0;
		dev->net_dev.tx_errors = 0;
		dev->net_dev.rx_dropped = 0;
		dev->net_dev.driver_data = dev;
		// the poll exists before the handler can be called
		netif_napi_add(&dev->net_dev, &dev->napi, vmxnet3_poll,
			       NAPI_POLL_WEIGHT);
		napi_enable(&dev->napi);
		g_vmxnet3_initialized = 1;

		// Initial link status
		uint32_t link = vm_cmd_get(dev, VMXNET3_CMD_GET_LINK);
		dev->link_up = (link & 0x1) ? 1 : 0;
		dev->link_speed = dev->link_up ? link >> 16 : 0;
		if (dev->link_up)
			kprintf("VMXNET3: Link UP (%u Mb/s)\n",
				dev->link_speed);
		else
			kprintf("VMXNET3: Link DOWN\n");
		dev->link_poll_tick = dev->stats_tick = dev->tx_progress_tick =
			timer_ticks();

		// Wire up net_device_t and register
		dev->net_dev.name = "eth0";
		for (int m = 0; m < ETH_ALEN; m++)
			dev->net_dev.mac_addr[m] = dev->mac_addr[m];
		dev->net_dev.mtu = NET_MTU_DEFAULT;
		dev->net_dev.ip_addr = 0;
		dev->net_dev.netmask = 0;
		dev->net_dev.gateway = 0;
		dev->net_dev.dns_server = 0;
		dev->net_dev.send = NULL;
		dev->net_dev.xmit = vmxnet3_xmit;
		dev->net_dev.link_status = vmxnet3_link_status;
		dev->net_dev.shutdown = vmxnet3_shutdown;
		dev->net_dev.watchdog = vmxnet3_watchdog;
		dev->net_dev.tx_timeout = vmxnet3_tx_timeout;
		dev->net_dev.set_rx_mode = vmxnet3_set_rx_mode;
		dev->net_dev.get_stats64 = vmxnet3_get_stats64;
		dev->net_dev.features |= NETIF_F_RXCSUM;

		net_register(&dev->net_dev);
		if (!dev->link_up)
			netif_carrier_off(&dev->net_dev);

		// Unmask interrupts at the device level.  IMR[0] = 0 enables
		// vector 0; intrCtrl=0 globally enables.
		dev->shared->devRead.intrConf.intrCtrl = 0;
		__asm__ volatile("mfence" ::: "memory");
		vm_w32(dev->bar0, VMXNET3_REG_IMR + 0 * 8, 0);
		kprintf("VMXNET3: Driver initialized successfully\n");
		return; // first device only
	}
}
