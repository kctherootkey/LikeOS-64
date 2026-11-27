// LikeOS HAL - AMD Platform Security Processor: platform access mailbox
// and doorbell
//
// See amd_psp.h.  The PSP is found by a PCI driver registered on demand
// (the first amd_psp_init); only generations with platform access are in
// its table.  At bind: memory decoding and bus mastering on (the PSP
// reads the request buffer from memory), BAR 2 mapped, the feature
// register read (all-ones: a firmware that hides the device -- the PSP is
// not used), the PSP's interrupts masked and acknowledged.
//
// Mailbox (one command at a time, mailbox_mutex): the command register
// must not report recovery and must show the response bit (the previous
// command done); the request is copied into a buffer of our own whose
// physical address goes into the address registers, the command into the
// command register; then the response bit is awaited, the address
// registers are checked to still be ours (another agent did not reuse the
// mailbox meanwhile), the status field folded into the request's header
// and the buffer copied back.
//
// Doorbell (doorbell_mutex): wait for the command register's response
// bit, write the message into the command register, ring the button, wait
// for the response bit, the status is in bits 7:0.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2013, 2019, 2023 Advanced Micro Devices, Inc.

#include <kernel/hal/amd_psp.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/io.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define PCI_VENDOR_ID_AMD_PSP 0x1022
#define PSP_BAR 2

/* MMIO and DMA buffer access; the host test harness substitutes its
 * PSP model and an ordinary buffer. */
#ifndef psp_mmio_readl
#define psp_mmio_readl(a) readl(a)
#define psp_mmio_writel(v, a) writel((v), (a))
#endif

#ifndef psp_dma_alloc
/* One page of memory the PSP can read: its virtual and physical
 * address. */
static void *psp_dma_alloc(uint64_t *phys)
{
	uint64_t p = mm_allocate_physical_page();

	if (!p)
		return NULL;
	*phys = p;
	kmemset(phys_to_virt(p), 0, PAGE_SIZE);
	return phys_to_virt(p);
}
#endif

/* ---- register layouts --------------------------------------------------- */

/* PSP v2 / v3: mailbox and doorbell. */
static const struct amd_psp_vdata pspv2_vdata = {
	.feature_reg = 0x109fc, /* C2PMSG_63 */
	.inten_reg = 0x10690, /* P2CMSG_INTEN */
	.intsts_reg = 0x10694, /* P2CMSG_INTSTS */
	.cmdresp_reg = 0x10570, /* C2PMSG_28 */
	.cmdbuff_addr_lo_reg = 0x10574, /* C2PMSG_29 */
	.cmdbuff_addr_hi_reg = 0x10578, /* C2PMSG_30 */
	.doorbell_button_reg = 0x10a24, /* C2PMSG_73 */
	.doorbell_cmd_reg = 0x10a40, /* C2PMSG_80 */
};

/* PSP v5: doorbell only. */
static const struct amd_psp_vdata pspv5_vdata = {
	.feature_reg = 0x109fc, /* C2PMSG_63 */
	.inten_reg = 0x10510, /* P2CMSG_INTEN */
	.intsts_reg = 0x10514, /* P2CMSG_INTSTS */
	.doorbell_button_reg = 0x10a24, /* C2PMSG_73 */
	.doorbell_cmd_reg = 0x10a40, /* C2PMSG_80 */
};

static const struct {
	uint16_t device;
	const struct amd_psp_vdata *vd;
} psp_ids[] = {
	{ 0x1486, &pspv2_vdata },
	{ 0x15DF, &pspv2_vdata },
	{ 0x15C7, &pspv2_vdata },
	{ 0x1649, &pspv2_vdata },
	{ 0x1134, &pspv5_vdata },
	{ 0x17E0, &pspv5_vdata },
};

const struct amd_psp_vdata *amd_psp_lookup_vdata(uint16_t device_id)
{
	for (unsigned int i = 0; i < sizeof(psp_ids) / sizeof(psp_ids[0]); i++)
		if (psp_ids[i].device == device_id)
			return psp_ids[i].vd;
	return NULL;
}

/* ---- the device ---------------------------------------------------------- */

struct amd_psp {
	volatile uint8_t *regs;
	const struct amd_psp_vdata *vd;
	uint32_t capability;
	struct mutex mailbox_mutex;
	struct mutex doorbell_mutex;
	/* the request buffer (mailbox_mutex) */
	void *cmdbuf;
	uint64_t cmdbuf_phys;
	char name[32];
};

/* Set once (attach), read by every user afterwards. */
static struct amd_psp *volatile g_psp;
static struct amd_psp g_psp_dev;
static DEFINE_MUTEX(g_psp_init_lock);
static int g_psp_driver_registered;

static inline uint32_t psp_rd(struct amd_psp *psp, uint32_t off)
{
	return psp_mmio_readl(psp->regs + off);
}

static inline void psp_wr(struct amd_psp *psp, uint32_t v, uint32_t off)
{
	psp_mmio_writel(v, psp->regs + off);
}

int amd_psp_attach(volatile uint8_t *regs, const struct amd_psp_vdata *vd,
		   const char *name)
{
	struct amd_psp *psp = &g_psp_dev;
	uint32_t val;

	if (!regs || !vd) {
		/* detach (tests): nobody may be using it */
		__atomic_store_n(&g_psp, NULL, __ATOMIC_RELEASE);
		kmemset(psp->name, 0, sizeof(psp->name));
		psp->regs = NULL;
		psp->vd = NULL;
		return 0;
	}
	if (g_psp)
		return 0;

	/* A firmware that does not let the OS reach the PSP reads
	 * all-ones here. */
	val = psp_mmio_readl(regs + vd->feature_reg);
	if (val == 0xffffffffu) {
		kprintf("amd_psp %s: unable to access the device: you might be running a broken BIOS\n",
			name ? name : "");
		return -EIO;
	}
	if (!psp->cmdbuf && vd->cmdresp_reg) {
		psp->cmdbuf = psp_dma_alloc(&psp->cmdbuf_phys);
		if (!psp->cmdbuf)
			return -ENOMEM;
	}
	psp->regs = regs;
	psp->vd = vd;
	psp->capability = val;
	mutex_init(&psp->mailbox_mutex, "psp_mailbox");
	mutex_init(&psp->doorbell_mutex, "psp_doorbell");
	ksnprintf(psp->name, sizeof(psp->name), "%s", name ? name : "psp");

	/* Interrupts off and acknowledged: platform access is polled and
	 * no other PSP service is driven here. */
	psp_wr(psp, 0, vd->inten_reg);
	psp_wr(psp, 0xffffffffu, vd->intsts_reg);

	__atomic_store_n(&g_psp, psp, __ATOMIC_RELEASE);
	kprintf("amd_psp %s: platform access enabled (%s%s), capabilities 0x%08x\n",
		psp->name, vd->cmdresp_reg ? "mailbox" : "",
		vd->doorbell_cmd_reg ? (vd->cmdresp_reg ? ", doorbell" :
							  "doorbell") :
				       "",
		val);
	return 0;
}

static int amd_psp_pci_probe(pci_device_t *pdev, const struct pci_device_id *id)
{
	const struct amd_psp_vdata *vd = (const void *)(unsigned long)id->driver_data;
	volatile void *regs;
	char name[32];
	int ret;

	if (g_psp)
		return -EEXIST; /* one PSP drives platform access */
	ret = pci_enable_device_mem(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);
	regs = pci_iomap(pdev, PSP_BAR, 0);
	if (!regs) {
		kprintf("amd_psp: BAR %d of %02x:%02x.%x not mapped\n", PSP_BAR,
			pdev->bus, pdev->device, pdev->function);
		pci_clear_master(pdev);
		return -ENOMEM;
	}
	ksnprintf(name, sizeof(name), "%04x:%02x:%02x.%x", pdev->segment,
		  pdev->bus, pdev->device, pdev->function);
	ret = amd_psp_attach((volatile uint8_t *)regs, vd, name);
	if (ret) {
		pci_iounmap(pdev, regs);
		pci_clear_master(pdev);
	}
	return ret;
}

static const struct pci_device_id amd_psp_pci_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x1486),
	  .driver_data = (uint64_t)(unsigned long)&pspv2_vdata },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x15DF),
	  .driver_data = (uint64_t)(unsigned long)&pspv2_vdata },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x15C7),
	  .driver_data = (uint64_t)(unsigned long)&pspv2_vdata },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x1649),
	  .driver_data = (uint64_t)(unsigned long)&pspv2_vdata },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x1134),
	  .driver_data = (uint64_t)(unsigned long)&pspv5_vdata },
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD_PSP, 0x17E0),
	  .driver_data = (uint64_t)(unsigned long)&pspv5_vdata },
	{ 0 },
};

static struct pci_driver amd_psp_pci_driver = {
	.name = "amd_psp",
	.id_table = amd_psp_pci_table,
	.probe = amd_psp_pci_probe,
};

int amd_psp_init(void)
{
	int ret = 0;

	might_sleep();
	mutex_lock(&g_psp_init_lock);
	if (!g_psp && !g_psp_driver_registered) {
		g_psp_driver_registered = 1;
		ret = pci_register_driver(&amd_psp_pci_driver);
	}
	mutex_unlock(&g_psp_init_lock);
	if (ret)
		return ret;
	return g_psp ? 0 : -ENODEV;
}

int psp_check_platform_access_status(void)
{
	struct amd_psp *psp = __atomic_load_n(&g_psp, __ATOMIC_ACQUIRE);

	return psp ? 0 : -ENODEV;
}

/* ---- mailbox ------------------------------------------------------------- */

/* Until the PSP sets the response bit: 0, or -ETIMEDOUT.  The first
 * 100 us are a tight loop (a command is usually consumed at once), then
 * short sleeps. */
static int psp_wait_cmd(struct amd_psp *psp, uint32_t reg)
{
	uint64_t start = hrtimer_now_ns();
	uint64_t deadline = start + (uint64_t)PSP_CMD_TIMEOUT_US * 1000ull;

	for (;;) {
		uint64_t now;

		if (psp_rd(psp, reg) & PSP_CMDRESP_RESP)
			return 0;
		now = hrtimer_now_ns();
		if (now > deadline)
			return (psp_rd(psp, reg) & PSP_CMDRESP_RESP) ?
				       0 :
				       -ETIMEDOUT;
		if (now - start < 100000ull)
			udelay(1);
		else
			usleep_range(20, 50);
	}
}

int psp_send_platform_access_msg(enum psp_platform_access_msg msg,
				 struct psp_request *req)
{
	struct amd_psp *psp = __atomic_load_n(&g_psp, __ATOMIC_ACQUIRE);
	const struct amd_psp_vdata *vd;
	struct psp_request *buf;
	uint32_t cmd_reg, size;
	int ret;

	if (!psp)
		return -ENODEV;
	vd = psp->vd;
	if (!vd->cmdresp_reg || !vd->cmdbuff_addr_lo_reg ||
	    !vd->cmdbuff_addr_hi_reg)
		return -ENODEV;
	size = req->header.payload_size;
	if (size < sizeof(req->header) || size > PSP_REQ_MAX)
		return -EINVAL;
	might_sleep();

	mutex_lock(&psp->mailbox_mutex);
	buf = psp->cmdbuf;
	if (psp_rd(psp, vd->cmdresp_reg) & PSP_CMDRESP_RECOVERY) {
		/* The mailbox recovers from an earlier error. */
		ret = -EBUSY;
		goto unlock;
	}
	if (psp_wait_cmd(psp, vd->cmdresp_reg)) {
		/* not done processing the command before */
		ret = -EBUSY;
		goto unlock;
	}

	/* The PSP maps the request through its physical address. */
	kmemcpy(buf, req, size);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	psp_wr(psp, (uint32_t)psp->cmdbuf_phys, vd->cmdbuff_addr_lo_reg);
	psp_wr(psp, (uint32_t)(psp->cmdbuf_phys >> 32),
	       vd->cmdbuff_addr_hi_reg);
	/* the command register starts the processing */
	cmd_reg = ((uint32_t)msg << PSP_CMDRESP_CMD_SHIFT) & PSP_CMDRESP_CMD;
	psp_wr(psp, cmd_reg, vd->cmdresp_reg);

	if (psp_wait_cmd(psp, vd->cmdresp_reg)) {
		ret = -ETIMEDOUT;
		goto unlock;
	}
	/* Still our buffer: nobody else used the mailbox meanwhile. */
	if (psp_rd(psp, vd->cmdbuff_addr_lo_reg) != (uint32_t)psp->cmdbuf_phys ||
	    psp_rd(psp, vd->cmdbuff_addr_hi_reg) !=
		    (uint32_t)(psp->cmdbuf_phys >> 32)) {
		ret = -EBUSY;
		goto unlock;
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	/* The answer: the buffer as the PSP left it (payload_size is
	 * ours and stays), a command status overrides the buffer's. */
	kmemcpy((uint8_t *)req + sizeof(req->header),
		(const uint8_t *)buf + sizeof(buf->header),
		size - sizeof(req->header));
	req->header.status = buf->header.status;
	cmd_reg = psp_rd(psp, vd->cmdresp_reg);
	if (cmd_reg & PSP_CMDRESP_STS)
		req->header.status = cmd_reg & PSP_CMDRESP_STS;
	/* A non-zero status: the command failed, or it ran and its result
	 * is in the payload; the caller tells which. */
	ret = req->header.status ? -EIO : 0;
unlock:
	mutex_unlock(&psp->mailbox_mutex);
	return ret;
}

int psp_ring_platform_doorbell(int msg, uint32_t *result)
{
	struct amd_psp *psp = __atomic_load_n(&g_psp, __ATOMIC_ACQUIRE);
	const struct amd_psp_vdata *vd;
	uint32_t val;
	int ret;

	if (!psp)
		return -ENODEV;
	vd = psp->vd;
	if (!vd->doorbell_cmd_reg || !vd->doorbell_button_reg)
		return -ENODEV;
	might_sleep();

	mutex_lock(&psp->doorbell_mutex);
	if (psp_wait_cmd(psp, vd->doorbell_cmd_reg)) {
		kprintf("amd_psp %s: doorbell command not done processing\n",
			psp->name);
		ret = -EBUSY;
		goto unlock;
	}
	psp_wr(psp, (uint32_t)msg & PSP_DOORBELL_CMDRESP_STS,
	       vd->doorbell_cmd_reg);
	psp_wr(psp, PSP_DRBL_RING, vd->doorbell_button_reg);
	if (psp_wait_cmd(psp, vd->doorbell_cmd_reg)) {
		ret = -ETIMEDOUT;
		goto unlock;
	}
	val = psp_rd(psp, vd->doorbell_cmd_reg) & PSP_DOORBELL_CMDRESP_STS;
	if (val) {
		if (result)
			*result = val;
		ret = -EIO;
		goto unlock;
	}
	ret = 0;
unlock:
	mutex_unlock(&psp->doorbell_mutex);
	return ret;
}
