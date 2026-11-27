/* Firmware bus arbitration models for the I2C harnesses (host/i2c/arb.c):
 *
 *   the host bridge     a PCI function at 0:0.0 whose configuration space
 *                       holds the IOSF sideband mailbox (MCR/MDR/MCRX) of a
 *                       Bay Trail or Cherry Trail SoC, with the P-Unit
 *                       behind it: the PMIC bus semaphore register in the
 *                       PMC port (0x7 on Bay Trail, 0x10e on Cherry
 *                       Trail), granted after a number of polls, held by
 *                       the firmware for a while, or never; config
 *                       accesses that fail;
 *   the AMD PSP         the platform access registers of a PSP (mailbox
 *                       with a request buffer, or doorbell), answering the
 *                       I2C arbitration requests: granted, busy for N
 *                       requests, a status error, a mailbox stuck or in
 *                       recovery, a hidden device (all-ones);
 *   kernel services     pci_get_devices / pci_find_bdf / config dwords /
 *                       pci_clear_master, capable(), and the GPIO/pin
 *                       controller system sleep calls (counted, with a
 *                       hook for the suspend test).
 *
 * Both models count grants and releases and check that every transfer on
 * the bus they arbitrate happens while the OS holds the bus (an address
 * phase while the firmware owns the bus is a violation).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2C_ARB_H
#define HOST_I2C_ARB_H

struct dw_model;

/* ---- the host bridge / IOSF / P-Unit ----------------------------------- */

struct arb_iosf {
	int present; /* the host bridge is listed */
	uint16_t vendor, device; /* 8086:0F00 BYT, 8086:2280 CHT, ... */
	uint32_t sem_reg; /* where the P-Unit has its semaphore */
	int grant_after; /* polls of the semaphore before it is granted */
	int never_grant; /* the P-Unit never answers */
	int fw_hold_polls; /* the firmware still uses the bus for N polls */
	int cfg_fail; /* config accesses fail */
	/* state */
	int requested, granted;
	int polls;
	unsigned long grants, releases, requests, violations, msgs;
	uint32_t regs[256][512]; /* port, offset (PMC port up to 0x1ff) */
	uint32_t mcr, mdr, mcrx;
};
extern struct arb_iosf g_arb_iosf;
/* Reset the model (and forget the mailbox: iosf_mbi_attach(NULL)). */
void arb_iosf_reset(void);

/* ---- the PSP ---------------------------------------------------------------- */

enum { PSP_MODE_OK = 0, PSP_MODE_STUCK, PSP_MODE_RECOVERY,
       PSP_MODE_STATUS_ERR, PSP_MODE_CMD_STS_ERR };

struct arb_psp {
	uint8_t *window; /* 0x11000 bytes of registers */
	uint32_t feature; /* 0xffffffff: hidden */
	int mode;
	int busy; /* answer "bus busy" to the next N acquires */
	int doorbell; /* the doorbell layout (else the mailbox) */
	/* state */
	int held;
	unsigned long grants, releases, busy_answers, requests, violations;
	int last_type;
};
extern struct arb_psp g_arb_psp;
void arb_psp_reset(void);
uint32_t arb_psp_readl(const volatile void *a);
void arb_psp_writel(uint32_t v, volatile void *a);
void *arb_psp_dma_alloc(uint64_t *phys);

/* A bus model slave hook: count the address phases on `m' that happen
 * while the arbitrated owner is the firmware (which = 1 IOSF, 2 PSP). */
void arb_watch_bus(struct dw_model *m, int which);

/* ---- services ------------------------------------------------------------- */

/* The root complex / host bridge at 0:0.0 the PSP probe looks at. */
void arb_set_root_complex(uint16_t vendor, uint16_t device);
extern int g_arb_capable;
extern unsigned long g_arb_pinctrl_suspends, g_arb_pinctrl_resumes;
/* Called by the GPIO sleep stubs (the suspend test's view inside). */
extern void (*g_arb_pinctrl_suspend_hook)(void);
extern void (*g_arb_pinctrl_resume_hook)(void);

#endif
