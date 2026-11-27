/* Firmware bus arbitration models and the kernel services around them, for
 * the I2C harnesses (see arb.h).
 *
 * Copyright (C) 2026 The LikeOS Project */
#include <kernel/hal/pci.h>
#include <kernel/hal/iosf_mbi.h>
#include <kernel/hal/amd_psp.h>
#include <kernel/ke/cred.h>
#include <kernel/dev/gpio/gpiolib.h>
#include "host/i2c/arb.h"

#define PSP_WINDOW 0x11000u
/* The register offsets of the PSP generations the harness models (the
 * driver's tables, kernel/hal/amd_psp.c). */
#define R_FEATURE 0x109fc
#define R_CMDRESP 0x10570
#define R_LO 0x10574
#define R_HI 0x10578
#define R_DB_BUTTON 0x10a24
#define R_DB_CMD 0x10a40

struct arb_iosf g_arb_iosf;
struct arb_psp g_arb_psp;
int g_arb_capable = 1;
unsigned long g_arb_pinctrl_suspends, g_arb_pinctrl_resumes;
void (*g_arb_pinctrl_suspend_hook)(void);
void (*g_arb_pinctrl_resume_hook)(void);

static pci_device_t g_arb_pci[1];
static int g_arb_npci;
static spinlock_t g_arb_lock = SPINLOCK_INIT("arb");

/* ==========================================================================
 * PCI services
 * ========================================================================== */

static void arb_pci_refresh(void)
{
	g_arb_npci = g_arb_iosf.present ? 1 : 0;
	g_arb_pci[0].bus = 0;
	g_arb_pci[0].device = 0;
	g_arb_pci[0].function = 0;
	g_arb_pci[0].vendor_id = g_arb_iosf.vendor;
	g_arb_pci[0].device_id = g_arb_iosf.device;
	g_arb_pci[0].class_code = 0x06;
}

void arb_set_root_complex(uint16_t vendor, uint16_t device)
{
	g_arb_iosf.present = vendor != 0;
	g_arb_iosf.vendor = vendor;
	g_arb_iosf.device = device;
	arb_pci_refresh();
}

const pci_device_t *pci_get_devices(int *count)
{
	arb_pci_refresh();
	*count = g_arb_npci;
	return g_arb_pci;
}

const pci_device_t *pci_find_bdf(unsigned char bus, unsigned char device,
				 unsigned char function)
{
	arb_pci_refresh();
	if (g_arb_npci && !bus && !device && !function)
		return &g_arb_pci[0];
	return NULL;
}

void pci_clear_master(pci_device_t *dev)
{
	(void)dev;
}

int capable(void)
{
	return g_arb_capable;
}

/* ==========================================================================
 * IOSF sideband / P-Unit
 * ========================================================================== */

void arb_iosf_reset(void)
{
	iosf_mbi_attach(NULL, 0);
	kmemset(&g_arb_iosf, 0, sizeof(g_arb_iosf));
	arb_pci_refresh();
}

/* A register of the P-Unit's PMC port or any other unit. */
static uint32_t iosf_reg_read(uint8_t port, uint32_t off)
{
	struct arb_iosf *x = &g_arb_iosf;

	if (port == BT_MBI_UNIT_PMC && off == x->sem_reg) {
		x->polls++;
		if (x->requested && !x->granted && !x->never_grant &&
		    x->polls >= x->grant_after) {
			if (x->fw_hold_polls > 0)
				x->fw_hold_polls--;
			else {
				x->granted = 1;
				x->grants++;
			}
		}
		/* bit 1 is a request, not readable state */
		return x->granted ? PUNIT_SEMAPHORE_BIT : 0;
	}
	return off < 512 ? x->regs[port][off] : 0;
}

static void iosf_reg_write(uint8_t port, uint32_t off, uint32_t v)
{
	struct arb_iosf *x = &g_arb_iosf;

	if (port == BT_MBI_UNIT_PMC && off == x->sem_reg) {
		if (v & PUNIT_SEMAPHORE_ACQUIRE) {
			if (!x->requested) {
				x->requests++;
				x->polls = 0;
			}
			x->requested = 1;
		} else if (!(v & PUNIT_SEMAPHORE_BIT)) {
			if (x->granted)
				x->releases++;
			x->granted = 0;
			x->requested = 0;
		}
		return;
	}
	if (off < 512)
		x->regs[port][off] = v;
}

static void iosf_run(void)
{
	struct arb_iosf *x = &g_arb_iosf;
	uint8_t op = (uint8_t)(x->mcr >> 24), port = (uint8_t)(x->mcr >> 16);
	uint32_t off = ((x->mcr >> 8) & 0xff) | (x->mcrx & 0xffffff00u);

	T_ASSERT((x->mcr & 0xff) == MBI_ENABLE);
	x->msgs++;
	if (op & 1)
		iosf_reg_write(port, off, x->mdr);
	else
		x->mdr = iosf_reg_read(port, off);
	/* the extended offset belongs to one message */
	x->mcrx = 0;
}

int pci_read_config_dword(const pci_device_t *dev, int where, uint32_t *val)
{
	uint64_t fl;

	T_ASSERT(dev == &g_arb_pci[0]);
	if (g_arb_iosf.cfg_fail)
		return -1;
	spin_lock_irqsave(&g_arb_lock, &fl);
	switch (where) {
	case MBI_MDR_OFFSET:
		*val = g_arb_iosf.mdr;
		break;
	case MBI_MCR_OFFSET:
		*val = g_arb_iosf.mcr;
		break;
	case MBI_MCRX_OFFSET:
		*val = g_arb_iosf.mcrx;
		break;
	default:
		*val = 0xffffffffu;
		break;
	}
	spin_unlock_irqrestore(&g_arb_lock, fl);
	return 0;
}

int pci_write_config_dword(const pci_device_t *dev, int where, uint32_t val)
{
	uint64_t fl;

	T_ASSERT(dev == &g_arb_pci[0]);
	if (g_arb_iosf.cfg_fail)
		return -1;
	spin_lock_irqsave(&g_arb_lock, &fl);
	switch (where) {
	case MBI_MDR_OFFSET:
		g_arb_iosf.mdr = val;
		break;
	case MBI_MCRX_OFFSET:
		g_arb_iosf.mcrx = val;
		break;
	case MBI_MCR_OFFSET:
		g_arb_iosf.mcr = val;
		iosf_run();
		break;
	default:
		break;
	}
	spin_unlock_irqrestore(&g_arb_lock, fl);
	return 0;
}

/* ==========================================================================
 * PSP
 * ========================================================================== */

static inline uint32_t *psp_reg(uint32_t off)
{
	return (uint32_t *)(void *)(g_arb_psp.window + off);
}

void arb_psp_reset(void)
{
	uint8_t *w = g_arb_psp.window;

	amd_psp_attach(NULL, NULL, NULL);
	if (!w)
		w = kcalloc(1, PSP_WINDOW);
	T_ASSERT(w);
	kmemset(w, 0, PSP_WINDOW);
	kmemset(&g_arb_psp, 0, sizeof(g_arb_psp));
	g_arb_psp.window = w;
	g_arb_psp.feature = 0x1; /* some capability bits */
	*psp_reg(R_CMDRESP) = PSP_CMDRESP_RESP;
	*psp_reg(R_DB_CMD) = PSP_CMDRESP_RESP;
}

/* An arbitration request: the status to answer. */
static uint32_t psp_process(uint32_t type)
{
	struct arb_psp *p = &g_arb_psp;

	p->requests++;
	p->last_type = (int)type;
	if (type == 0) { /* acquire */
		if (p->busy > 0) {
			p->busy--;
			p->busy_answers++;
			return 1;
		}
		if (!p->held)
			p->grants++;
		p->held = 1;
		return 0;
	}
	if (type == 1) { /* release */
		if (p->held)
			p->releases++;
		p->held = 0;
		return 0;
	}
	return 3;
}

uint32_t arb_psp_readl(const volatile void *a)
{
	uint32_t off = (uint32_t)((const volatile uint8_t *)a -
				  g_arb_psp.window);
	uint32_t v;

	T_ASSERT(off + 4 <= PSP_WINDOW && !(off & 3));
	if (off == R_FEATURE)
		return g_arb_psp.feature;
	v = __atomic_load_n(psp_reg(off), __ATOMIC_ACQUIRE);
	if (off == R_CMDRESP && g_arb_psp.mode == PSP_MODE_RECOVERY)
		v |= PSP_CMDRESP_RECOVERY;
	return v;
}

void arb_psp_writel(uint32_t v, volatile void *a)
{
	uint32_t off = (uint32_t)((volatile uint8_t *)a - g_arb_psp.window);
	struct arb_psp *p = &g_arb_psp;

	T_ASSERT(off + 4 <= PSP_WINDOW && !(off & 3));
	if (off == R_CMDRESP && !p->doorbell) {
		uint64_t phys = (uint64_t)*psp_reg(R_LO) |
				((uint64_t)*psp_reg(R_HI) << 32);
		uint32_t *req = (uint32_t *)(unsigned long)phys;
		uint32_t sts = 0;

		T_EQ((v & PSP_CMDRESP_CMD) >> PSP_CMDRESP_CMD_SHIFT,
		     PSP_I2C_REQ_BUS_CMD);
		T_ASSERT(req && req[0] == 12);
		if (p->mode == PSP_MODE_STUCK) {
			__atomic_store_n(psp_reg(off), v, __ATOMIC_RELEASE);
			return;
		}
		/* the request buffer: size, status, type */
		req[1] = psp_process(req[2]);
		if (p->mode == PSP_MODE_STATUS_ERR)
			req[1] = 3;
		if (p->mode == PSP_MODE_CMD_STS_ERR)
			sts = 5;
		__atomic_store_n(psp_reg(off), PSP_CMDRESP_RESP | sts,
				 __ATOMIC_RELEASE);
		return;
	}
	if (off == R_DB_CMD && p->doorbell) {
		/* the message; the button starts it */
		__atomic_store_n(psp_reg(off), v & 0xff, __ATOMIC_RELEASE);
		return;
	}
	if (off == R_DB_BUTTON && p->doorbell) {
		uint32_t sts;

		T_EQ(v, PSP_DRBL_RING);
		if (p->mode == PSP_MODE_STUCK)
			return;
		sts = psp_process(*psp_reg(R_DB_CMD) & 0xff);
		if (p->mode == PSP_MODE_STATUS_ERR ||
		    p->mode == PSP_MODE_CMD_STS_ERR)
			sts = 3;
		__atomic_store_n(psp_reg(R_DB_CMD), PSP_CMDRESP_RESP | sts,
				 __ATOMIC_RELEASE);
		return;
	}
	__atomic_store_n(psp_reg(off), v, __ATOMIC_RELEASE);
}

void *arb_psp_dma_alloc(uint64_t *phys)
{
	void *p = kcalloc(1, 4096);

	*phys = (uint64_t)(unsigned long)p;
	return p;
}

/* ==========================================================================
 * Bus watch
 * ========================================================================== */

static void arb_on_start(struct dw_model *m)
{
	if (m->on_start_which == 1 && !g_arb_iosf.granted)
		g_arb_iosf.violations++;
	if (m->on_start_which == 2 && !g_arb_psp.held)
		g_arb_psp.violations++;
}

void arb_watch_bus(struct dw_model *m, int which)
{
	m->on_start_which = which;
	m->on_start = which ? arb_on_start : NULL;
}

/* ==========================================================================
 * GPIO / pin controller system sleep
 * ========================================================================== */

void gpio_pinctrl_suspend_all(void)
{
	might_sleep();
	g_arb_pinctrl_suspends++;
	if (g_arb_pinctrl_suspend_hook)
		g_arb_pinctrl_suspend_hook();
}

void gpio_pinctrl_resume_all(void)
{
	might_sleep();
	g_arb_pinctrl_resumes++;
	if (g_arb_pinctrl_resume_hook)
		g_arb_pinctrl_resume_hook();
}
