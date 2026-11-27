// LikeOS I2C - DesignWare: AMD PSP bus arbitration
//
// On some AMD platforms (Cezanne, Mendocino and later; ACPI id AMDI0019)
// one I2C bus is shared with the Platform Security Processor.  The OS asks
// the PSP for the bus before using it and gives it back afterwards, over
// the PSP's platform access interface (kernel/hal/amd_psp.c): on Cezanne
// (root complex 1022:1630) a mailbox command carrying a small request
// buffer, on later parts a doorbell.  The PSP answers "busy" while it
// uses the bus itself; the request is repeated every 25 ms for up to 10 s.
//
// Granted bus time is kept for a reservation period of 100 ms: further
// acquires within it cost nothing, and a work item gives the bus back
// when the period ends and nobody holds it (or, when the period already
// ended, the last release does).  Holders are counted (psp_i2c_access_
// count): the adapter's bus lock operations acquire before taking the
// bus lock and release after dropping it, so a client's multi-transfer
// sequence (write, wait, read) keeps the bus, and the controller's own
// acquire_lock/release_lock hooks nest inside.
//
// Probe: the controller only gets an adapter when the PSP's platform
// access is up and one request/release round trip succeeds; otherwise
// the glue refuses the adapter.  A mailbox that fails later (no answer
// for 10 s, a communication error) is reported once and from then on the
// bus is treated as the host's alone -- the arbitration protocol defines
// no other way out, and the PSP gave up the bus when it stopped
// answering.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2022 Google Inc., 2023 Advanced Micro Devices, Inc.

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/hal/amd_psp.h>
#include <kernel/hal/pci.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/delay.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#ifndef PSP_I2C_RESERVATION_TIME_MS
#define PSP_I2C_RESERVATION_TIME_MS 100
#endif
#ifndef PSP_I2C_REQ_RETRY_CNT
#define PSP_I2C_REQ_RETRY_CNT 400
#endif
#ifndef PSP_I2C_REQ_RETRY_DELAY_US
#define PSP_I2C_REQ_RETRY_DELAY_US (25u * 1000u)
#endif
#define PSP_I2C_REQ_STS_OK 0x0
#define PSP_I2C_REQ_STS_BUS_BUSY 0x1
#define PSP_I2C_REQ_STS_INV_PARAM 0x3

#define AMD_ROOT_COMPLEX_CEZANNE 0x1630

enum psp_i2c_req_type {
	PSP_I2C_REQ_ACQUIRE,
	PSP_I2C_REQ_RELEASE,
	PSP_I2C_REQ_MAX
};

struct psp_i2c_req {
	struct psp_req_buffer_hdr hdr;
	uint32_t type; /* enum psp_i2c_req_type */
} __attribute__((packed));

static DEFINE_MUTEX(psp_i2c_access_mutex);
/* All under psp_i2c_access_mutex. */
static uint64_t psp_i2c_sem_acquired; /* ns of the grant, 0: not held */
static uint32_t psp_i2c_access_count;
static int psp_i2c_mbox_fail;
/* The probe's round trip: a failure refuses the adapter instead. */
static int psp_i2c_probing;
static struct dw_i2c_dev *psp_i2c_dev; /* the one bus */
static struct delayed_work release_queue;
static struct amd_psp_i2c_stats {
	unsigned long acquires, releases, busy_retries, failures,
		deferred_releases;
	uint64_t max_wait_us;
} psp_i2c_st;

static int (*_psp_send_i2c_req)(struct psp_i2c_req *req);

/* The status the PSP left in the request. */
static int check_i2c_req_sts(struct psp_i2c_req *req)
{
	/* written by the PSP (mailbox) or copied in by the doorbell path */
	uint32_t status = req->hdr.status;

	switch (status) {
	case PSP_I2C_REQ_STS_OK:
		return 0;
	case PSP_I2C_REQ_STS_BUS_BUSY:
		return -EBUSY;
	case PSP_I2C_REQ_STS_INV_PARAM:
	default:
		return -EIO;
	}
}

/* Mailbox errors (the PSP not working) come back as they are; -EIO means
 * the request itself was answered, its status decides. */
static int psp_send_i2c_req_cezanne(struct psp_i2c_req *req)
{
	int ret;

	ret = psp_send_platform_access_msg(PSP_I2C_REQ_BUS_CMD,
					   (struct psp_request *)req);
	if (ret == -EIO)
		return check_i2c_req_sts(req);
	return ret;
}

static int psp_send_i2c_req_doorbell(struct psp_i2c_req *req)
{
	uint32_t result = 0;
	int ret;

	ret = psp_ring_platform_doorbell((int)req->type, &result);
	if (ret == -EIO) {
		req->hdr.status = result;
		return check_i2c_req_sts(req);
	}
	return ret;
}

static const char *psp_req_name(enum psp_i2c_req_type t)
{
	return t == PSP_I2C_REQ_ACQUIRE ? "acquire" : "release";
}

/* One request, repeated while the PSP says busy: 0 or -errno.  A failure
 * marks the mailbox failed (the bus is the host's from then on). */
static int psp_send_i2c_req(enum psp_i2c_req_type i2c_req_type)
{
	struct psp_i2c_req req;
	uint64_t start, deadline;
	int status, ret;

	mutex_assert_held(&psp_i2c_access_mutex);
	start = hrtimer_now_ns();
	deadline = start + (uint64_t)PSP_I2C_REQ_RETRY_CNT *
				   PSP_I2C_REQ_RETRY_DELAY_US * 1000ull;
	for (;;) {
		kmemset(&req, 0, sizeof(req));
		req.hdr.payload_size = sizeof(req);
		req.type = i2c_req_type;
		status = _psp_send_i2c_req(&req);
		if (status != -EBUSY)
			break;
		if (hrtimer_now_ns() > deadline)
			break;
		psp_i2c_st.busy_retries++;
		usleep_range(PSP_I2C_REQ_RETRY_DELAY_US / 2,
			     PSP_I2C_REQ_RETRY_DELAY_US);
	}
	if (status == -EBUSY) {
		kprintf("%s: Timed out waiting for PSP to %s I2C bus\n",
			psp_i2c_dev ? psp_i2c_dev->name : "i2c_designware",
			i2c_req_type == PSP_I2C_REQ_ACQUIRE ? "release" :
							      "acquire");
		ret = -ETIMEDOUT;
	} else {
		ret = status;
		if (ret)
			kprintf("%s: PSP communication error (%d) on %s\n",
				psp_i2c_dev ? psp_i2c_dev->name :
					      "i2c_designware",
				ret, psp_req_name(i2c_req_type));
	}
	if (!ret) {
		uint64_t us = (hrtimer_now_ns() - start) / 1000;

		if (us > psp_i2c_st.max_wait_us)
			psp_i2c_st.max_wait_us = us;
		return 0;
	}
	psp_i2c_st.failures++;
	if (psp_i2c_probing)
		return ret;
	kprintf("%s: Assume i2c bus is for exclusive host usage\n",
		psp_i2c_dev ? psp_i2c_dev->name : "i2c_designware");
	psp_i2c_mbox_fail = 1;
	return ret;
}

static void release_bus(void)
{
	mutex_assert_held(&psp_i2c_access_mutex);
	if (!psp_i2c_sem_acquired)
		return;
	if (psp_send_i2c_req(PSP_I2C_REQ_RELEASE))
		return;
	psp_i2c_st.releases++;
	psp_i2c_sem_acquired = 0;
}

static void psp_release_i2c_bus_deferred(struct work_struct *work)
{
	(void)work;
	mutex_lock(&psp_i2c_access_mutex);
	/* A holder still in: its release gives the bus back. */
	if (!psp_i2c_access_count) {
		if (psp_i2c_sem_acquired)
			psp_i2c_st.deferred_releases++;
		release_bus();
	}
	mutex_unlock(&psp_i2c_access_mutex);
}

static void psp_acquire_i2c_bus(void)
{
	might_sleep();
	mutex_lock(&psp_i2c_access_mutex);
	/* After a mailbox failure the bus is the host's. */
	if (psp_i2c_mbox_fail)
		goto out;
	psp_i2c_access_count++;
	/* Inside the reservation period nothing has to be asked. */
	if (psp_i2c_sem_acquired)
		goto out;
	if (psp_send_i2c_req(PSP_I2C_REQ_ACQUIRE))
		goto out; /* mbox_fail is set: the bus is granted anyway */
	psp_i2c_sem_acquired = hrtimer_now_ns() ? hrtimer_now_ns() : 1;
	psp_i2c_st.acquires++;
	(void)schedule_delayed_work(&release_queue,
				    PSP_I2C_RESERVATION_TIME_MS);
out:
	mutex_unlock(&psp_i2c_access_mutex);
}

static void psp_release_i2c_bus(void)
{
	mutex_lock(&psp_i2c_access_mutex);
	if (psp_i2c_mbox_fail)
		goto out;
	if (WARN_ON(!psp_i2c_access_count))
		goto out;
	psp_i2c_access_count--;
	if (psp_i2c_access_count)
		goto out;
	/* The reservation period is over and the host still has the bus:
	 * give it back now. */
	if (!delayed_work_pending(&release_queue))
		release_bus();
out:
	mutex_unlock(&psp_i2c_access_mutex);
}

/* The adapter's bus lock with the PSP's bus around it: a client holding
 * the bus lock over several transfers keeps the PSP out as long. */
static void i2c_adapter_dw_psp_lock_bus(struct i2c_adapter *adapter,
					unsigned int flags)
{
	psp_acquire_i2c_bus();
	i2c_adapter_lock_bus(adapter, flags);
}

static int i2c_adapter_dw_psp_trylock_bus(struct i2c_adapter *adapter,
					  unsigned int flags)
{
	/* Asking the PSP sleeps: an atomic transfer cannot have this bus
	 * (and the controller refuses atomic transfers with a semaphore). */
	if (i2c_in_atomic_xfer_mode())
		return 0;
	if (!i2c_adapter_trylock_bus(adapter, flags))
		return 0;
	psp_acquire_i2c_bus();
	return 1;
}

static void i2c_adapter_dw_psp_unlock_bus(struct i2c_adapter *adapter,
					  unsigned int flags)
{
	psp_release_i2c_bus();
	i2c_adapter_unlock_bus(adapter, flags);
}

static const struct i2c_lock_operations i2c_dw_psp_lock_ops = {
	.lock_bus = i2c_adapter_dw_psp_lock_bus,
	.trylock_bus = i2c_adapter_dw_psp_trylock_bus,
	.unlock_bus = i2c_adapter_dw_psp_unlock_bus,
};

static int psp_acquire_lock(struct dw_i2c_dev *dev)
{
	(void)dev;
	psp_acquire_i2c_bus();
	return 0;
}

static void psp_release_lock(struct dw_i2c_dev *dev)
{
	(void)dev;
	psp_release_i2c_bus();
}

int i2c_dw_amdpsp_probe_lock_support(struct dw_i2c_dev *dev)
{
	const pci_device_t *rdev;
	int ret;

	if (!dev)
		return -ENODEV;
	if (!(dev->flags & ARBITRATION_SEMAPHORE))
		return -ENODEV;
	/* From here the bus is shared with the PSP. */
	dev->needs_arbitration = 1;
	/* One PSP-arbitrated bus per platform. */
	if (psp_i2c_dev)
		return -EEXIST;

	/* Cezanne has the platform mailbox, Mendocino and later the
	 * doorbell. */
	rdev = pci_find_bdf(0, 0, 0);
	if (rdev && rdev->vendor_id == 0x1022 &&
	    rdev->device_id == AMD_ROOT_COMPLEX_CEZANNE)
		_psp_send_i2c_req = psp_send_i2c_req_cezanne;
	else
		_psp_send_i2c_req = psp_send_i2c_req_doorbell;

	(void)amd_psp_init();
	if (psp_check_platform_access_status())
		return -EPROBE_DEFER;

	INIT_DELAYED_WORK(&release_queue, psp_release_i2c_bus_deferred);
	mutex_lock(&psp_i2c_access_mutex);
	psp_i2c_dev = dev;
	psp_i2c_mbox_fail = 0;
	psp_i2c_access_count = 0;
	psp_i2c_sem_acquired = 0;
	/* One acquire/release round trip: a PSP that does not answer now
	 * would leave the bus unarbitrated. */
	psp_i2c_probing = 1;
	ret = psp_send_i2c_req(PSP_I2C_REQ_ACQUIRE);
	if (!ret) {
		psp_i2c_sem_acquired = 1;
		psp_i2c_st.acquires++;
		ret = psp_send_i2c_req(PSP_I2C_REQ_RELEASE);
		if (!ret) {
			psp_i2c_st.releases++;
			psp_i2c_sem_acquired = 0;
		}
	}
	psp_i2c_probing = 0;
	if (ret) {
		psp_i2c_dev = NULL;
		psp_i2c_mbox_fail = 0;
		psp_i2c_sem_acquired = 0;
		mutex_unlock(&psp_i2c_access_mutex);
		return ret == -EBUSY || ret == -ETIMEDOUT ? -ETIMEDOUT : -EIO;
	}
	mutex_unlock(&psp_i2c_access_mutex);

	kprintf("%s: I2C bus managed by AMD PSP (%s)\n", dev->name,
		_psp_send_i2c_req == psp_send_i2c_req_cezanne ? "mailbox" :
								 "doorbell");
	/* The adapter's bus lock and the controller's own register
	 * accesses both hold the PSP's bus. */
	dev->adapter.lock_ops = &i2c_dw_psp_lock_ops;
	dev->acquire_lock = psp_acquire_lock;
	dev->release_lock = psp_release_lock;
	return 0;
}

void i2c_dw_amdpsp_remove_lock_support(struct dw_i2c_dev *dev)
{
	if (!dev || psp_i2c_dev != dev)
		return;
	might_sleep();
	(void)cancel_delayed_work_sync(&release_queue);
	mutex_lock(&psp_i2c_access_mutex);
	/* Nobody holds the bus once the adapter is gone (after a mailbox
	 * failure the count is no longer kept). */
	WARN_ON(!psp_i2c_mbox_fail && psp_i2c_access_count);
	if (!psp_i2c_mbox_fail)
		release_bus();
	psp_i2c_dev = NULL;
	psp_i2c_mbox_fail = 0;
	psp_i2c_access_count = 0;
	psp_i2c_sem_acquired = 0;
	mutex_unlock(&psp_i2c_access_mutex);
	dev->adapter.lock_ops = NULL;
	dev->acquire_lock = NULL;
	dev->release_lock = NULL;
}

long i2c_dw_amdpsp_show(struct dw_i2c_dev *dev, char *buf, long cap, long pos)
{
	struct amd_psp_i2c_stats s;
	int held, fail;
	uint32_t count;

	if (psp_i2c_dev != dev)
		return pos;
	mutex_lock(&psp_i2c_access_mutex);
	s = psp_i2c_st;
	held = psp_i2c_sem_acquired != 0;
	fail = psp_i2c_mbox_fail;
	count = psp_i2c_access_count;
	mutex_unlock(&psp_i2c_access_mutex);
	return i2c_show_printf(buf, cap, pos,
			       "  dw: psp arbitration %s%s holders %u acquires %lu releases %lu (deferred %lu) busy-retries %lu failures %lu max-wait %lu us\n",
			       held ? "granted" : "released",
			       fail ? " mailbox-failed (host-exclusive)" : "",
			       count, s.acquires, s.releases,
			       s.deferred_releases, s.busy_retries, s.failures,
			       (unsigned long)s.max_wait_us);
}
