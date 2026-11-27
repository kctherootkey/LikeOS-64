// LikeOS HAL - IOSF sideband mailbox and the P-Unit PMIC bus semaphore
//
// See iosf_mbi.h.  The mailbox registers are one shared resource of the
// host bridge: every message (write MCRX, write MCR, read MDR -- or write
// MDR first for a write) runs under one spinlock with interrupts off, so
// messages never interleave.
//
// The semaphore side keeps two counts under iosf_mbi_pmic_access_mutex:
// the P-Unit acquires in progress (iosf_mbi_punit_acquire) and the OS's
// blocked periods (iosf_mbi_block_punit_i2c_access).  Either side waits
// until the other's count is zero, so the OS never asks for the
// semaphore while a P-Unit request that ignores it runs, and P-Unit
// requests wait while the OS holds the bus.  The outermost block runs the
// notifier chain (BEGIN), sets the C-state constraint, requests the
// semaphore and polls for the grant (1-2 ms steps, 500 ms at most); on a
// timeout or mailbox error the request is withdrawn (semaphore bit
// cleared, constraint dropped, END notified) and the error returned, so
// the caller does not touch the bus.  The last unblock releases it the
// same way.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2013-2014 Intel Corporation

#include <kernel/hal/iosf_mbi.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/waitq.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define PCI_VENDOR_ID_INTEL_IOSF 0x8086

/* The mailbox device and the semaphore register of this SoC.  Written
 * once by iosf_mbi_attach before any user (boot, or a test between
 * cases), read without the lock afterwards. */
static const pci_device_t *volatile mbi_pdev;
static uint32_t iosf_mbi_sem_address;
static spinlock_t iosf_mbi_lock = SPINLOCK_INIT("iosf_mbi");

static const struct {
	uint16_t device;
	uint32_t sem_address;
	const char *name;
} iosf_mbi_pci_ids[] = {
	{ PCI_DEVICE_ID_INTEL_BAYTRAIL, PUNIT_SEMAPHORE_BYT, "Bay Trail" },
	{ PCI_DEVICE_ID_INTEL_BRASWELL, PUNIT_SEMAPHORE_CHT,
	  "Cherry Trail/Braswell" },
	{ PCI_DEVICE_ID_INTEL_QUARK_X1000, 0, "Quark X1000" },
	{ PCI_DEVICE_ID_INTEL_TANGIER, 0, "Tangier" },
};

/* ==========================================================================
 * Mailbox access
 * ========================================================================== */

static inline uint32_t iosf_mbi_form_mcr(uint8_t op, uint8_t port,
					 uint8_t offset)
{
	return ((uint32_t)op << 24) | ((uint32_t)port << 16) |
	       ((uint32_t)offset << 8) | MBI_ENABLE;
}

static int iosf_mbi_pci_read_mdr(uint32_t mcrx, uint32_t mcr, uint32_t *mdr)
{
	const pci_device_t *pdev = mbi_pdev;
	int result;

	if (!pdev)
		return -ENODEV;
	if (mcrx) {
		result = pci_write_config_dword(pdev, MBI_MCRX_OFFSET, mcrx);
		if (result < 0)
			goto fail_read;
	}
	result = pci_write_config_dword(pdev, MBI_MCR_OFFSET, mcr);
	if (result < 0)
		goto fail_read;
	result = pci_read_config_dword(pdev, MBI_MDR_OFFSET, mdr);
	if (result < 0)
		goto fail_read;
	return 0;

fail_read:
	kprintf("iosf_mbi: PCI config access failed with %d\n", result);
	return -EIO;
}

static int iosf_mbi_pci_write_mdr(uint32_t mcrx, uint32_t mcr, uint32_t mdr)
{
	const pci_device_t *pdev = mbi_pdev;
	int result;

	if (!pdev)
		return -ENODEV;
	result = pci_write_config_dword(pdev, MBI_MDR_OFFSET, mdr);
	if (result < 0)
		goto fail_write;
	if (mcrx) {
		result = pci_write_config_dword(pdev, MBI_MCRX_OFFSET, mcrx);
		if (result < 0)
			goto fail_write;
	}
	result = pci_write_config_dword(pdev, MBI_MCR_OFFSET, mcr);
	if (result < 0)
		goto fail_write;
	return 0;

fail_write:
	kprintf("iosf_mbi: PCI config access failed with %d\n", result);
	return -EIO;
}

int iosf_mbi_read(uint8_t port, uint8_t opcode, uint32_t offset,
		  uint32_t *mdr)
{
	uint32_t mcr, mcrx;
	uint64_t flags;
	int ret;

	/* The GPU's port belongs to the graphics driver's own sideband. */
	if (WARN_ON(port == BT_MBI_UNIT_GFX))
		return -EPERM;
	mcr = iosf_mbi_form_mcr(opcode, port, (uint8_t)(offset & MBI_MASK_LO));
	mcrx = offset & MBI_MASK_HI;

	spin_lock_irqsave(&iosf_mbi_lock, &flags);
	ret = iosf_mbi_pci_read_mdr(mcrx, mcr, mdr);
	spin_unlock_irqrestore(&iosf_mbi_lock, flags);
	return ret;
}

int iosf_mbi_write(uint8_t port, uint8_t opcode, uint32_t offset,
		   uint32_t mdr)
{
	uint32_t mcr, mcrx;
	uint64_t flags;
	int ret;

	if (WARN_ON(port == BT_MBI_UNIT_GFX))
		return -EPERM;
	mcr = iosf_mbi_form_mcr(opcode, port, (uint8_t)(offset & MBI_MASK_LO));
	mcrx = offset & MBI_MASK_HI;

	spin_lock_irqsave(&iosf_mbi_lock, &flags);
	ret = iosf_mbi_pci_write_mdr(mcrx, mcr, mdr);
	spin_unlock_irqrestore(&iosf_mbi_lock, flags);
	return ret;
}

int iosf_mbi_modify(uint8_t port, uint8_t opcode, uint32_t offset,
		    uint32_t mdr, uint32_t mask)
{
	uint32_t mcr, mcrx, value;
	uint64_t flags;
	int ret;

	if (WARN_ON(port == BT_MBI_UNIT_GFX))
		return -EPERM;
	mcr = iosf_mbi_form_mcr(opcode, port, (uint8_t)(offset & MBI_MASK_LO));
	mcrx = offset & MBI_MASK_HI;

	spin_lock_irqsave(&iosf_mbi_lock, &flags);
	/* the current value (the opcode turned into its read form) */
	ret = iosf_mbi_pci_read_mdr(mcrx, mcr & MBI_RD_MASK, &value);
	if (ret < 0) {
		spin_unlock_irqrestore(&iosf_mbi_lock, flags);
		return ret;
	}
	value &= ~mask;
	mdr &= mask;
	value |= mdr;
	/* written back with the write form of the opcode */
	ret = iosf_mbi_pci_write_mdr(mcrx, mcr | MBI_WR_MASK, value);
	spin_unlock_irqrestore(&iosf_mbi_lock, flags);
	return ret;
}

bool iosf_mbi_available(void)
{
	/* The mailbox is part of the SoC: once found it never goes. */
	return mbi_pdev != NULL;
}

bool iosf_mbi_punit_semaphore_available(void)
{
	return mbi_pdev != NULL && iosf_mbi_sem_address != 0;
}

void iosf_mbi_attach(const pci_device_t *dev, uint32_t sem_address)
{
	iosf_mbi_sem_address = dev ? sem_address : 0;
	__atomic_store_n(&mbi_pdev, dev, __ATOMIC_RELEASE);
}

int iosf_mbi_init(void)
{
	const pci_device_t *devs;
	int count = 0;

	might_sleep();
	if (mbi_pdev)
		return 0;
	devs = pci_get_devices(&count);
	for (int i = 0; devs && i < count; i++) {
		if (devs[i].vendor_id != PCI_VENDOR_ID_INTEL_IOSF)
			continue;
		for (unsigned int k = 0; k < sizeof(iosf_mbi_pci_ids) /
						     sizeof(iosf_mbi_pci_ids[0]);
		     k++) {
			if (devs[i].device_id != iosf_mbi_pci_ids[k].device)
				continue;
			/* The mailbox lives in configuration space: nothing to
			 * enable. */
			iosf_mbi_attach(&devs[i],
					iosf_mbi_pci_ids[k].sem_address);
			kprintf("iosf_mbi: %s sideband mailbox at %02x:%02x.%x%s\n",
				iosf_mbi_pci_ids[k].name, devs[i].bus,
				devs[i].device, devs[i].function,
				iosf_mbi_pci_ids[k].sem_address ?
					", P-Unit PMIC bus semaphore" :
					"");
			return 0;
		}
	}
	return -ENODEV;
}

/* ==========================================================================
 * P-Unit / OS shared PMIC bus arbitration
 * ========================================================================== */

static DEFINE_MUTEX(iosf_mbi_pmic_access_mutex);
static DECLARE_WAIT_QUEUE_HEAD(iosf_mbi_pmic_access_waitq);
/* Both under iosf_mbi_pmic_access_mutex; read without it only by the
 * wait conditions (which re-check under the mutex). */
static volatile uint32_t iosf_mbi_pmic_punit_access_count;
static volatile uint32_t iosf_mbi_pmic_i2c_access_count;
static uint64_t iosf_mbi_sem_acquired_ns;
/* The notifier chain (under the mutex). */
static struct iosf_mbi_notifier *iosf_mbi_pmic_bus_access_notifier;
/* Blocked periods that forbid C6/C7 (the "CPU latency 0" request). */
static volatile int iosf_mbi_cstate_block;
static struct iosf_mbi_stats iosf_mbi_st;

static uint32_t punit_count(void)
{
	return __atomic_load_n(&iosf_mbi_pmic_punit_access_count,
			       __ATOMIC_ACQUIRE);
}

static uint32_t i2c_count(void)
{
	return __atomic_load_n(&iosf_mbi_pmic_i2c_access_count,
			       __ATOMIC_ACQUIRE);
}

void iosf_mbi_punit_acquire(void)
{
	might_sleep();
	/* Wait for the OS's own PMIC accesses to finish. */
	mutex_lock(&iosf_mbi_pmic_access_mutex);
	while (i2c_count() != 0) {
		mutex_unlock(&iosf_mbi_pmic_access_mutex);
		wait_event(iosf_mbi_pmic_access_waitq, i2c_count() == 0);
		mutex_lock(&iosf_mbi_pmic_access_mutex);
	}
	/* Blocking the OS's accesses is all the P-Unit needs. */
	__atomic_store_n(&iosf_mbi_pmic_punit_access_count, punit_count() + 1,
			 __ATOMIC_RELEASE);
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
}

void iosf_mbi_punit_release(void)
{
	int do_wakeup;

	mutex_lock(&iosf_mbi_pmic_access_mutex);
	if (WARN_ON(punit_count() == 0)) {
		mutex_unlock(&iosf_mbi_pmic_access_mutex);
		return;
	}
	__atomic_store_n(&iosf_mbi_pmic_punit_access_count, punit_count() - 1,
			 __ATOMIC_RELEASE);
	do_wakeup = punit_count() == 0;
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
	if (do_wakeup)
		wake_up_all(&iosf_mbi_pmic_access_waitq);
}

void iosf_mbi_assert_punit_acquired(void)
{
	WARN_ON(punit_count() == 0);
}

static int iosf_mbi_get_sem(uint32_t *sem)
{
	int ret;

	ret = iosf_mbi_read(BT_MBI_UNIT_PMC, MBI_REG_READ, iosf_mbi_sem_address,
			    sem);
	if (ret) {
		kprintf("iosf_mbi: Error P-Unit semaphore read failed\n");
		return ret;
	}
	*sem &= PUNIT_SEMAPHORE_BIT;
	return 0;
}

int iosf_mbi_punit_semaphore_held(void)
{
	uint32_t sem;
	int ret;

	if (!iosf_mbi_punit_semaphore_available())
		return -ENXIO;
	ret = iosf_mbi_get_sem(&sem);
	return ret ? ret : (int)sem;
}

/* Run the notifier chain (mutex held). */
static void iosf_mbi_call_notifiers(unsigned long action)
{
	mutex_assert_held(&iosf_mbi_pmic_access_mutex);
	for (struct iosf_mbi_notifier *nb = iosf_mbi_pmic_bus_access_notifier;
	     nb; nb = nb->next)
		(void)nb->fn(nb, action);
}

static void iosf_mbi_cstate_block_set(int on)
{
	/* Nested blocked periods never reach here: only the outermost one
	 * sets the constraint and only its end drops it. */
	if (on)
		WARN_ON_ONCE(__atomic_add_fetch(&iosf_mbi_cstate_block, 1,
						__ATOMIC_ACQ_REL) != 1);
	else
		WARN_ON_ONCE(__atomic_sub_fetch(&iosf_mbi_cstate_block, 1,
						__ATOMIC_ACQ_REL) != 0);
}

bool iosf_mbi_deep_cstates_allowed(void)
{
	return __atomic_load_n(&iosf_mbi_cstate_block, __ATOMIC_ACQUIRE) == 0;
}

/* Withdraw the request / give the semaphore back (mutex held). */
static void iosf_mbi_reset_semaphore(void)
{
	if (iosf_mbi_modify(BT_MBI_UNIT_PMC, MBI_REG_READ,
			    iosf_mbi_sem_address, 0, PUNIT_SEMAPHORE_BIT))
		kprintf("iosf_mbi: Error P-Unit semaphore reset failed\n");
	iosf_mbi_cstate_block_set(0);
	iosf_mbi_call_notifiers(MBI_PMIC_BUS_ACCESS_END);
}

int iosf_mbi_block_punit_i2c_access(void)
{
	uint64_t start, deadline, now;
	uint32_t sem = 0;
	int ret = 0;

	if (WARN_ON(!iosf_mbi_punit_semaphore_available()))
		return -ENXIO;
	might_sleep();

	mutex_lock(&iosf_mbi_pmic_access_mutex);
	/* P-Unit requests in progress first (they ignore the semaphore). */
	while (punit_count() != 0) {
		mutex_unlock(&iosf_mbi_pmic_access_mutex);
		wait_event(iosf_mbi_pmic_access_waitq, punit_count() == 0);
		mutex_lock(&iosf_mbi_pmic_access_mutex);
	}

	if (i2c_count() > 0) {
		iosf_mbi_st.nested++;
		goto success;
	}

	/* Users that need P-Unit resources from atomic context take them
	 * now, before the bus is blocked. */
	iosf_mbi_call_notifiers(MBI_PMIC_BUS_ACCESS_BEGIN);
	/* No C6/C7 while the semaphore is held: entering them makes the
	 * P-Unit talk to the PMIC, which hangs the SoC meanwhile. */
	iosf_mbi_cstate_block_set(1);

	/* Ask the P-Unit for the semaphore ... */
	ret = iosf_mbi_write(BT_MBI_UNIT_PMC, MBI_REG_WRITE,
			     iosf_mbi_sem_address, PUNIT_SEMAPHORE_ACQUIRE);
	if (ret) {
		kprintf("iosf_mbi: Error P-Unit semaphore request failed\n");
		iosf_mbi_st.errors++;
		goto error;
	}

	/* ... and wait until it says the bus is ours (bit 0). */
	start = hrtimer_now_ns();
	deadline = start + (uint64_t)PUNIT_SEMAPHORE_TIMEOUT_MS * 1000000ull;
	do {
		ret = iosf_mbi_get_sem(&sem);
		if (!ret && sem) {
			now = hrtimer_now_ns();
			iosf_mbi_sem_acquired_ns = now;
			iosf_mbi_st.acquires++;
			if ((now - start) / 1000 > iosf_mbi_st.max_wait_us)
				iosf_mbi_st.max_wait_us = (now - start) / 1000;
			goto success;
		}
		usleep_range(1000, 2000);
	} while (hrtimer_now_ns() < deadline);

	ret = -ETIMEDOUT;
	iosf_mbi_st.timeouts++;
	kprintf("iosf_mbi: Error P-Unit semaphore timed out, resetting\n");
error:
	iosf_mbi_reset_semaphore();
	if (!iosf_mbi_get_sem(&sem))
		kprintf("iosf_mbi: P-Unit semaphore: %u\n", sem);
success:
	if (!ret)
		__atomic_store_n(&iosf_mbi_pmic_i2c_access_count,
				 i2c_count() + 1, __ATOMIC_RELEASE);
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
	return ret;
}

void iosf_mbi_unblock_punit_i2c_access(void)
{
	int do_wakeup = 0;

	mutex_lock(&iosf_mbi_pmic_access_mutex);
	if (WARN_ON(i2c_count() == 0)) {
		mutex_unlock(&iosf_mbi_pmic_access_mutex);
		return;
	}
	__atomic_store_n(&iosf_mbi_pmic_i2c_access_count, i2c_count() - 1,
			 __ATOMIC_RELEASE);
	if (i2c_count() == 0) {
		uint64_t held = (hrtimer_now_ns() - iosf_mbi_sem_acquired_ns) /
				1000;

		iosf_mbi_reset_semaphore();
		iosf_mbi_st.releases++;
		if (held > iosf_mbi_st.max_hold_us)
			iosf_mbi_st.max_hold_us = held;
		do_wakeup = 1;
	}
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
	if (do_wakeup)
		wake_up_all(&iosf_mbi_pmic_access_waitq);
}

int iosf_mbi_register_pmic_bus_access_notifier(struct iosf_mbi_notifier *nb)
{
	if (!nb || !nb->fn)
		return -EINVAL;
	/* Wait for the bus to go inactive before registering. */
	iosf_mbi_punit_acquire();
	mutex_lock(&iosf_mbi_pmic_access_mutex);
	nb->next = iosf_mbi_pmic_bus_access_notifier;
	iosf_mbi_pmic_bus_access_notifier = nb;
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
	iosf_mbi_punit_release();
	return 0;
}

static int iosf_mbi_unlink_notifier(struct iosf_mbi_notifier *nb)
{
	struct iosf_mbi_notifier **pp;
	int ret = -ENOENT;

	mutex_lock(&iosf_mbi_pmic_access_mutex);
	for (pp = &iosf_mbi_pmic_bus_access_notifier; *pp; pp = &(*pp)->next)
		if (*pp == nb) {
			*pp = nb->next;
			nb->next = NULL;
			ret = 0;
			break;
		}
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
	return ret;
}

int iosf_mbi_unregister_pmic_bus_access_notifier_unlocked(
	struct iosf_mbi_notifier *nb)
{
	iosf_mbi_assert_punit_acquired();
	return iosf_mbi_unlink_notifier(nb);
}

int iosf_mbi_unregister_pmic_bus_access_notifier(struct iosf_mbi_notifier *nb)
{
	int ret;

	iosf_mbi_punit_acquire();
	ret = iosf_mbi_unlink_notifier(nb);
	iosf_mbi_punit_release();
	return ret;
}

void iosf_mbi_get_stats(struct iosf_mbi_stats *out)
{
	mutex_lock(&iosf_mbi_pmic_access_mutex);
	*out = iosf_mbi_st;
	mutex_unlock(&iosf_mbi_pmic_access_mutex);
}
