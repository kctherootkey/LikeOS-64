/* Kernel services the I2C code uses, for the I2C harness: an interrupt
 * core with a thread that delivers level-triggered interrupts from the
 * controller models (and from a second "device" sharing a line, or a
 * device model driving a GSI), with threaded one-shot handlers run by a
 * thread of their own while the line stays masked, a PCI
 * core over a table of modelled functions, devfs/chrdev, user memory
 * copies with a faulting range, gpiolib (GpioInt interrupts with
 * deferral, recovery GPIOs wired to a model's SCL/SDA), completions.
 *
 * Copyright (C) 2026 The LikeOS Project */
#include "host/i2c/model.h"
#include "host/i2c/mocks.h"
#include <kernel/ke/completion.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/uaccess.h>
#include <kernel/fs/device.h>
#include <kernel/fs/chrdev.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/dev/gpio/gpiolib.h>

/* ==========================================================================
 * Text
 * ========================================================================== */

int kvsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
	return h_vsnprintf(buf, size, fmt, ap);
}

/* ==========================================================================
 * Completions
 * ========================================================================== */

void init_completion(struct completion *x)
{
	__atomic_store_n(&x->done, 0, __ATOMIC_RELEASE);
	wq_head_init(&x->wait, "completion");
}

void reinit_completion(struct completion *x)
{
	__atomic_store_n(&x->done, 0, __ATOMIC_RELEASE);
}

void complete(struct completion *x)
{
	uint32_t d = __atomic_load_n(&x->done, __ATOMIC_ACQUIRE);

	if (d != COMPLETION_DONE_ALL)
		__atomic_add_fetch(&x->done, 1, __ATOMIC_ACQ_REL);
	h_wq_wake();
}

void complete_all(struct completion *x)
{
	__atomic_store_n(&x->done, COMPLETION_DONE_ALL, __ATOMIC_RELEASE);
	h_wq_wake();
}

int try_wait_for_completion(struct completion *x)
{
	uint32_t d = __atomic_load_n(&x->done, __ATOMIC_ACQUIRE);

	while (d) {
		if (d == COMPLETION_DONE_ALL)
			return 1;
		if (__atomic_compare_exchange_n(&x->done, &d, d - 1, 0,
						__ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return 1;
	}
	return 0;
}

int completion_done(struct completion *x)
{
	return __atomic_load_n(&x->done, __ATOMIC_ACQUIRE) != 0;
}

long wait_for_completion_timeout(struct completion *x, long timeout_ms)
{
	uint64_t deadline = h_now_ns() + (uint64_t)timeout_ms * 1000000ull;

	might_sleep();
	for (;;) {
		uint64_t now;

		if (try_wait_for_completion(x)) {
			now = h_now_ns();
			return now >= deadline ? 1 :
						 (long)((deadline - now) / 1000000ull) +
							 1;
		}
		if (h_now_ns() >= deadline)
			return 0;
		h_usleep(20);
	}
}

void wait_for_completion(struct completion *x)
{
	while (!wait_for_completion_timeout(x, 1000))
		;
}

/* ==========================================================================
 * Interrupts
 * ========================================================================== */

#define IRQS_MAX 64
#define IRQS_ACT 4

struct irqs_action {
	irq_handler_t fn;
	irq_handler_t thread_fn;
	void *arg;
	unsigned long flags;
	const char *name;
	struct irqs_thread *thr; /* the handler thread (thread_fn only) */
};

/* The thread of a threaded handler: woken by the delivery thread with the
 * line masked (one-shot), runs thread_fn with interrupts on, unmasks. */
struct irqs_thread {
	irq_handler_t thread_fn;
	void *arg;
	volatile int pending; /* woken, thread_fn not yet run */
	volatile int running;
	volatile int stop, exited;
	struct irqs_desc *desc;
};

struct irqs_desc {
	int used;
	int gsi;
	struct dw_model *model; /* the line's source */
	volatile int ext_level; /* a second device asserting the line */
	struct irqs_action act[IRQS_ACT];
	int nact;
	int depth;
	volatile int in_handler;
	volatile int thread_busy; /* one-shot: masked until the thread ran */
	unsigned long count, none, threaded;
	int wake;
	unsigned int trigger; /* IRQ_TYPE_* */
};

static struct irqs_desc g_irqs[IRQS_MAX];
static spinlock_t g_irqs_lock = SPINLOCK_INIT("irqs");
static volatile int g_irq_thread;
/* GSI -> the model raising it (platform controllers). */
static struct dw_model *g_gsi_model[256];
/* GSI -> a device model's line level (devices on an APIC interrupt). */
static volatile int g_gsi_level[256];

static struct irqs_desc *idesc(int irq)
{
	int i = irq - IRQ_NR_FIRST;

	if (i < 0 || i >= IRQS_MAX || !g_irqs[i].used)
		return NULL;
	return &g_irqs[i];
}

static void irq_thread(void *arg)
{
	(void)arg;
	for (;;) {
		int fired = 0;

		for (int i = 0; i < IRQS_MAX; i++) {
			struct irqs_desc *d = &g_irqs[i];
			struct irqs_action act[IRQS_ACT];
			int n, level, ret = 0;
			uint64_t fl;

			spin_lock_irqsave(&g_irqs_lock, &fl);
			if (!d->used || !d->nact || d->depth) {
				spin_unlock_irqrestore(&g_irqs_lock, fl);
				continue;
			}
			n = d->nact;
			kmemcpy(act, d->act, sizeof(act));
			d->in_handler = 1;
			spin_unlock_irqrestore(&g_irqs_lock, fl);

			level = d->ext_level ||
				(d->model && dwm_irq_line(d->model)) ||
				(d->gsi >= 0 && g_gsi_level[d->gsi & 255]);
			if (level &&
			    !__atomic_load_n(&d->thread_busy, __ATOMIC_ACQUIRE)) {
				uint64_t f = local_irq_save();
				int wake_thread = 0;

				for (int k = 0; k < n; k++) {
					int r = act[k].fn ? act[k].fn(act[k].arg) :
							    IRQ_WAKE_THREAD;

					if (r == IRQ_WAKE_THREAD && act[k].thr) {
						/* masked until the thread ran */
						__atomic_store_n(&d->thread_busy, 1,
								 __ATOMIC_RELEASE);
						__atomic_store_n(&act[k].thr->pending,
								 1,
								 __ATOMIC_RELEASE);
						wake_thread = 1;
						r = IRQ_HANDLED;
					}
					ret |= r;
				}
				local_irq_restore(f);
				d->count++;
				if (!ret)
					d->none++;
				if (wake_thread) {
					d->threaded++;
					h_wq_wake();
				}
				fired = 1;
			}
			__atomic_store_n(&d->in_handler, 0, __ATOMIC_RELEASE);
		}
		h_usleep(fired ? 2 : 10);
	}
}

/* The thread of one threaded handler. */
static void irq_handler_thread(void *arg)
{
	struct irqs_thread *t = arg;

	for (;;) {
		unsigned long seq = h_wq_seq();

		if (__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE))
			break;
		if (!__atomic_load_n(&t->pending, __ATOMIC_ACQUIRE)) {
			h_wq_wait(seq);
			continue;
		}
		__atomic_store_n(&t->running, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&t->pending, 0, __ATOMIC_RELEASE);
		/* Process context, interrupts on: the handler may sleep. */
		(void)t->thread_fn(t->arg);
		__atomic_store_n(&t->running, 0, __ATOMIC_RELEASE);
		/* Unmask (one-shot). */
		__atomic_store_n(&t->desc->thread_busy, 0, __ATOMIC_RELEASE);
		h_wq_wake();
	}
	/* A wake-up that came after the stop: unmask for the next owner. */
	__atomic_store_n(&t->desc->thread_busy, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&t->exited, 1, __ATOMIC_RELEASE);
	h_wq_wake();
}

static void irq_thread_start(void)
{
	if (!__atomic_exchange_n(&g_irq_thread, 1, __ATOMIC_ACQ_REL))
		T_ASSERT(sched_add_task(irq_thread, NULL, NULL, 0));
}

static int irqs_new(struct dw_model *m, int gsi)
{
	uint64_t fl;

	spin_lock_irqsave(&g_irqs_lock, &fl);
	for (int i = 0; i < IRQS_MAX; i++) {
		if (!g_irqs[i].used) {
			kmemset(&g_irqs[i], 0, sizeof(g_irqs[i]));
			g_irqs[i].used = 1;
			g_irqs[i].model = m;
			g_irqs[i].gsi = gsi;
			spin_unlock_irqrestore(&g_irqs_lock, fl);
			return IRQ_NR_FIRST + i;
		}
	}
	spin_unlock_irqrestore(&g_irqs_lock, fl);
	return -ENOSPC;
}

int mock_irq_new(struct dw_model *m)
{
	return irqs_new(m, -1);
}

void mock_gsi_bind(int gsi, struct dw_model *m)
{
	g_gsi_model[gsi & 255] = m;
}

void mock_irq_set_ext(int irq, int level)
{
	struct irqs_desc *d = idesc(irq);

	T_ASSERT(d);
	d->ext_level = level;
}

void mock_gsi_set_level(int gsi, int level)
{
	g_gsi_level[gsi & 255] = level;
}

unsigned long mock_irq_threaded(int irq)
{
	struct irqs_desc *d = idesc(irq);

	return d ? d->threaded : 0;
}

int mock_irq_depth(int irq)
{
	struct irqs_desc *d = idesc(irq);

	return d ? d->depth : -1;
}

unsigned long mock_irq_count(int irq, unsigned long *none)
{
	struct irqs_desc *d = idesc(irq);

	if (!d)
		return 0;
	if (none)
		*none = d->none;
	return d->count;
}

int mock_irq_handlers(int irq)
{
	struct irqs_desc *d = idesc(irq);

	return d ? d->nact : -1;
}

int mock_irqs_in_use(void)
{
	int n = 0;

	for (int i = 0; i < IRQS_MAX; i++)
		n += g_irqs[i].used;
	return n;
}

int irq_alloc_desc_gsi(const struct irq_chip_ops *ops, void *data, int gsi,
		       int active_low, int level)
{
	(void)ops;
	(void)data;
	(void)active_low;
	(void)level;
	might_sleep();
	if (gsi < 0 || gsi > 223)
		return -EINVAL;
	return irqs_new(g_gsi_model[gsi], gsi);
}

void irq_free_desc(int irq)
{
	struct irqs_desc *d = idesc(irq);
	uint64_t fl;

	T_ASSERT(d);
	spin_lock_irqsave(&g_irqs_lock, &fl);
	if (d->nact)
		test_bug("irq_free_desc with handlers", __FILE__, __LINE__,
			 __func__);
	d->used = 0;
	spin_unlock_irqrestore(&g_irqs_lock, fl);
}

int request_threaded_irq(int irq, irq_handler_t fn, irq_handler_t thread_fn,
			 unsigned long flags, const char *name, void *arg)
{
	struct irqs_desc *d = idesc(irq);
	struct irqs_thread *thr = NULL;
	uint64_t fl;

	might_sleep();
	if (!d || (!fn && !thread_fn))
		return -EINVAL;
	/* A level interrupt with only a thread must stay masked until the
	 * thread ran: IRQF_ONESHOT is mandatory then. */
	if (!fn && !(flags & IRQF_ONESHOT))
		return -EINVAL;
	irq_thread_start();
	if (thread_fn) {
		thr = kcalloc(1, sizeof(*thr));
		if (!thr)
			return -ENOMEM;
		thr->thread_fn = thread_fn;
		thr->arg = arg;
		thr->desc = d;
	}
	spin_lock_irqsave(&g_irqs_lock, &fl);
	if (d->nact && (!(flags & IRQF_SHARED) ||
			!(d->act[0].flags & IRQF_SHARED))) {
		spin_unlock_irqrestore(&g_irqs_lock, fl);
		kfree(thr);
		return -EBUSY;
	}
	if (d->nact >= IRQS_ACT) {
		spin_unlock_irqrestore(&g_irqs_lock, fl);
		kfree(thr);
		return -ENOSPC;
	}
	d->act[d->nact].fn = fn;
	d->act[d->nact].thread_fn = thread_fn;
	d->act[d->nact].thr = thr;
	d->act[d->nact].arg = arg;
	d->act[d->nact].flags = flags;
	d->act[d->nact].name = name;
	if (flags & IRQF_NO_AUTOEN)
		d->depth++;
	d->nact++;
	spin_unlock_irqrestore(&g_irqs_lock, fl);
	if (thr)
		T_ASSERT(sched_add_task(irq_handler_thread, thr, NULL, 0));
	return 0;
}

int request_irq(int irq, irq_handler_t fn, unsigned long flags,
		const char *name, void *arg)
{
	return request_threaded_irq(irq, fn, NULL, flags, name, arg);
}

void synchronize_irq(int irq)
{
	struct irqs_desc *d = idesc(irq);

	might_sleep();
	/* The delivery thread, then a handler thread woken by it. */
	while (d && (__atomic_load_n(&d->in_handler, __ATOMIC_ACQUIRE) ||
		     __atomic_load_n(&d->thread_busy, __ATOMIC_ACQUIRE)))
		h_yield();
}

const char *free_irq(int irq, void *arg)
{
	struct irqs_desc *d = idesc(irq);
	const char *name = NULL;
	uint64_t fl;

	might_sleep();
	if (!d)
		return NULL;
	struct irqs_thread *thr = NULL;
	int no_autoen = 0;

	spin_lock_irqsave(&g_irqs_lock, &fl);
	for (int i = 0; i < d->nact; i++) {
		if (d->act[i].arg == arg) {
			name = d->act[i].name;
			thr = d->act[i].thr;
			no_autoen = (d->act[i].flags & IRQF_NO_AUTOEN) != 0;
			d->act[i] = d->act[--d->nact];
			break;
		}
	}
	spin_unlock_irqrestore(&g_irqs_lock, fl);
	if (!name)
		test_bug("free_irq of an unknown handler", __FILE__, __LINE__,
			 __func__);
	(void)no_autoen;
	/* A delivery pass that copied the action before it was removed may
	 * still wake its thread: let it end first. */
	while (__atomic_load_n(&d->in_handler, __ATOMIC_ACQUIRE))
		h_yield();
	if (thr) {
		/* Let a woken thread finish, then stop it. */
		__atomic_store_n(&thr->stop, 1, __ATOMIC_RELEASE);
		h_wq_wake();
		while (!__atomic_load_n(&thr->exited, __ATOMIC_ACQUIRE))
			h_usleep(50);
		kfree(thr);
	}
	synchronize_irq(irq);
	/* The line's disable depth starts over with the next owner. */
	if (!d->nact)
		d->depth = 0;
	return name;
}

void disable_irq_nosync(int irq)
{
	struct irqs_desc *d = idesc(irq);

	if (d)
		__atomic_add_fetch(&d->depth, 1, __ATOMIC_ACQ_REL);
}

void disable_irq(int irq)
{
	disable_irq_nosync(irq);
	synchronize_irq(irq);
}

void enable_irq(int irq)
{
	struct irqs_desc *d = idesc(irq);

	if (d && __atomic_sub_fetch(&d->depth, 1, __ATOMIC_ACQ_REL) < 0)
		test_bug("unbalanced enable_irq", __FILE__, __LINE__, __func__);
}

int irq_set_irq_wake(int irq, unsigned int on)
{
	struct irqs_desc *d = idesc(irq);

	if (!d)
		return -EINVAL;
	d->wake += on ? 1 : -1;
	T_ASSERT(d->wake >= 0);
	return 0;
}

int irq_set_irq_type(int irq, unsigned int type)
{
	struct irqs_desc *d = idesc(irq);

	if (!d)
		return -EINVAL;
	d->trigger = type & IRQ_TYPE_SENSE_MASK;
	return 0;
}

unsigned int irq_get_trigger_type(int irq)
{
	struct irqs_desc *d = idesc(irq);

	return d ? d->trigger : IRQ_TYPE_NONE;
}

int irq_is_wake_enabled(int irq)
{
	struct irqs_desc *d = idesc(irq);

	return d ? d->wake > 0 : 0;
}

/* ==========================================================================
 * PCI
 * ========================================================================== */

static struct mock_pci *g_pci[8];
static int g_npci;

void mock_pci_add(struct mock_pci *mp)
{
	T_ASSERT(g_npci < 8);
	mp->dev.vendor_id = mp->vendor;
	mp->dev.device_id = mp->device_id;
	mp->dev.bus = 0;
	mp->dev.device = mp->slot;
	mp->dev.function = mp->func;
	mp->dev.segment = 0;
	mp->dev.class_code = 0x0c;
	mp->dev.subclass = 0x80;
	g_pci[g_npci++] = mp;
}

void mock_pci_reset(void)
{
	g_npci = 0;
}

static struct mock_pci *mpci(const pci_device_t *dev)
{
	for (int i = 0; i < g_npci; i++)
		if (&g_pci[i]->dev == dev)
			return g_pci[i];
	test_bug("unknown PCI device", __FILE__, __LINE__, __func__);
}

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
					 const pci_device_t *dev)
{
	for (; ids && (ids->vendor || ids->device || ids->class_mask); ids++) {
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor_id) &&
		    (ids->device == PCI_ANY_ID ||
		     ids->device == dev->device_id))
			return ids;
	}
	return NULL;
}

int pci_register_driver(struct pci_driver *drv)
{
	for (int i = 0; i < g_npci; i++) {
		struct mock_pci *mp = g_pci[i];
		const struct pci_device_id *id;

		if (mp->dev.driver)
			continue;
		id = pci_match_id(drv->id_table, &mp->dev);
		if (!id)
			continue;
		mp->probe_ret = drv->probe(&mp->dev, id);
		if (!mp->probe_ret)
			mp->dev.driver = drv;
	}
	return 0;
}

void pci_unregister_driver(struct pci_driver *drv)
{
	for (int i = 0; i < g_npci; i++) {
		struct mock_pci *mp = g_pci[i];

		if (mp->dev.driver == drv) {
			drv->remove(&mp->dev);
			mp->dev.driver = NULL;
		}
	}
}

int pci_read_config_word(const pci_device_t *dev, int where, uint16_t *val)
{
	struct mock_pci *mp = mpci(dev);

	*val = where == PCI_COMMAND ? mp->cmd : 0xffff;
	return 0;
}

int pci_enable_device_mem(pci_device_t *dev)
{
	struct mock_pci *mp = mpci(dev);

	if (!mp->decode_stuck_off)
		mp->cmd |= PCI_COMMAND_MEMORY;
	return 0;
}

void pci_set_master(pci_device_t *dev)
{
	mpci(dev)->cmd |= 0x4;
}

int pci_set_power_state(pci_device_t *dev, pci_power_t state)
{
	mpci(dev)->pstate = state;
	return 0;
}

uint64_t pci_resource_start(const pci_device_t *dev, int bar)
{
	return bar == 0 ? mpci(dev)->bar : 0;
}

uint64_t pci_resource_len(const pci_device_t *dev, int bar)
{
	return bar == 0 && mpci(dev)->bar ? 0x1000 : 0;
}

uint32_t pci_resource_flags(const pci_device_t *dev, int bar)
{
	(void)dev;
	return bar == 0 ? PCI_BAR_MEM64 : 0;
}

volatile void *pci_iomap(pci_device_t *dev, int bar, uint64_t maxlen)
{
	struct mock_pci *mp = mpci(dev);

	(void)maxlen;
	if (bar != 0 || !mp->bar)
		return NULL;
	mp->mapped++;
	return mp->m->window;
}

void pci_iounmap(pci_device_t *dev, volatile void *addr)
{
	struct mock_pci *mp = mpci(dev);

	T_ASSERT(addr == mp->m->window);
	mp->mapped--;
}

int pci_alloc_irq_vectors_affinity(pci_device_t *dev, unsigned int min_vecs,
				   unsigned int max_vecs, unsigned int flags,
				   struct irq_affinity *affd)
{
	struct mock_pci *mp = mpci(dev);

	(void)min_vecs;
	(void)max_vecs;
	(void)flags;
	(void)affd;
	T_ASSERT(mp->irq <= 0);
	mp->irq = irqs_new(mp->m, -1);
	if (mp->irq < 0)
		return mp->irq;
	mp->m->irq = mp->irq;
	dev->irq_type = mp->msi ? PCI_IRQ_MSI : PCI_IRQ_INTX;
	dev->nr_irq_vecs = 1;
	return 1;
}

int pci_irq_vector(const pci_device_t *dev, unsigned int nr)
{
	struct mock_pci *mp = mpci(dev);

	return nr == 0 && mp->irq > 0 ? mp->irq : -EINVAL;
}

void pci_free_irq_vectors(pci_device_t *dev)
{
	struct mock_pci *mp = mpci(dev);

	if (mp->irq > 0)
		irq_free_desc(mp->irq);
	mp->irq = 0;
	dev->irq_type = 0;
	dev->nr_irq_vecs = 0;
}

void pci_name(const pci_device_t *dev, char *out, size_t cap)
{
	ksnprintf(out, cap, "%04x:%02x:%02x.%x", dev->segment, dev->bus,
		  dev->device, dev->function);
}

/* ==========================================================================
 * devfs, majors, user memory
 * ========================================================================== */

#define MAX_NODES 64
static struct devfs_node *g_nodes[MAX_NODES];
static struct {
	vfs_file_t *f;
	void *priv;
} g_fpriv[64];
int g_mock_devreg_fail;

int device_register(struct devfs_node *node)
{
	if (g_mock_devreg_fail)
		return -ENOMEM;
	for (int i = 0; i < MAX_NODES; i++)
		if (!g_nodes[i]) {
			g_nodes[i] = node;
			node->registered = 1;
			return 0;
		}
	return -ENOSPC;
}

int device_unregister(struct devfs_node *node)
{
	for (int i = 0; i < MAX_NODES; i++)
		if (g_nodes[i] == node) {
			g_nodes[i] = NULL;
			node->registered = 0;
			return 0;
		}
	return -ENOENT;
}

unsigned devfs_node_in_use(const struct devfs_node *node)
{
	(void)node;
	return 0;
}

struct devfs_node *mock_devfs_find(const char *path)
{
	for (int i = 0; i < MAX_NODES; i++)
		if (g_nodes[i] && !kstrcmp(g_nodes[i]->path, path))
			return g_nodes[i];
	return NULL;
}

void *device_file_priv(vfs_file_t *file)
{
	for (int i = 0; i < 64; i++)
		if (g_fpriv[i].f == file)
			return g_fpriv[i].priv;
	return NULL;
}

void device_file_set_priv(vfs_file_t *file, void *priv)
{
	int free_slot = -1;

	for (int i = 0; i < 64; i++) {
		if (g_fpriv[i].f == file) {
			g_fpriv[i].priv = priv;
			if (!priv)
				g_fpriv[i].f = NULL;
			return;
		}
		if (!g_fpriv[i].f && free_slot < 0)
			free_slot = i;
	}
	if (!priv)
		return;
	T_ASSERT(free_slot >= 0);
	g_fpriv[free_slot].f = file;
	g_fpriv[free_slot].priv = priv;
}

int chrdev_register(uint32_t major, const char *name)
{
	(void)name;
	return (int)major;
}

static int user_bad(const void *p)
{
	unsigned long a = (unsigned long)p;

	return !a || (a >= MOCK_BAD_USER && a < MOCK_BAD_USER + 0x10000);
}

bool validate_user_ptr(uint64_t ptr, size_t len)
{
	(void)len;
	return !user_bad((const void *)ptr);
}

int copy_from_user(void *dst, const void *src, size_t len)
{
	if (user_bad(src))
		return (int)len;
	kmemcpy(dst, src, len);
	return 0;
}

int copy_to_user(void *dst, const void *src, size_t len)
{
	if (user_bad(dst))
		return (int)len;
	kmemcpy(dst, src, len);
	return 0;
}

/* ==========================================================================
 * gpiolib
 * ========================================================================== */

struct gpio_desc {
	int which; /* 0 SCL, 1 SDA */
	int out;
	int active_low;
	int requested;
};

static struct gpio_desc g_scl = { .which = 0 }, g_sda = { .which = 1 };
struct dw_model *g_mock_recovery_model;
int g_mock_recovery_defer;
struct mock_gpioint g_mock_gpioint[8];

int acpi_dev_gpio_irq_wake_get(struct acpi_device *adev, int index,
			       int *wake_capable)
{
	if (index != 0)
		return -ENOENT;
	for (int i = 0; i < 8; i++) {
		struct mock_gpioint *g = &g_mock_gpioint[i];

		if (!g->hid[0] || kstrcmp(g->hid, adev->hid))
			continue;
		g->calls++;
		if (g->defer)
			return -EPROBE_DEFER;
		if (g->irq <= 0)
			g->irq = irqs_new(NULL, -1);
		if (wake_capable)
			*wake_capable = g->wake;
		return g->irq;
	}
	return -ENOENT;
}

int acpi_dev_gpiod_get_index(struct acpi_device *adev, int index,
			     const char *label, enum gpiod_flags flags,
			     struct gpio_desc **desc)
{
	(void)adev;
	(void)label;
	if (!g_mock_recovery_model)
		return -ENOENT;
	if (g_mock_recovery_defer)
		return -EPROBE_DEFER;
	if (index > 1)
		return -ENOENT;
	*desc = index == 0 ? &g_scl : &g_sda;
	T_ASSERT(!(*desc)->requested);
	(*desc)->requested = 1;
	(*desc)->out = flags == GPIOD_OUT_HIGH || flags == GPIOD_OUT_LOW;
	return 0;
}

void gpiod_free(struct gpio_desc *desc)
{
	T_ASSERT(desc->requested);
	desc->requested = 0;
}

int gpiod_get_value(const struct gpio_desc *desc)
{
	int v = desc->which ? dwm_get_sda(g_mock_recovery_model) :
			      dwm_get_scl(g_mock_recovery_model);

	return desc->active_low ? !v : v;
}

void gpiod_set_value(struct gpio_desc *desc, int value)
{
	int v = desc->active_low ? !value : value;

	if (desc->which)
		dwm_set_sda(g_mock_recovery_model, v);
	else
		dwm_set_scl(g_mock_recovery_model, v);
}

int gpiod_direction_output(struct gpio_desc *desc, int value)
{
	desc->out = 1;
	gpiod_set_value(desc, value);
	return 0;
}

int gpiod_get_direction(struct gpio_desc *desc)
{
	return desc->out ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

int gpiod_set_config(struct gpio_desc *desc, enum pin_config_param param,
		     uint32_t arg)
{
	(void)desc;
	(void)param;
	(void)arg;
	return 0;
}

int gpiod_is_active_low(const struct gpio_desc *desc)
{
	return desc->active_low;
}

void gpiod_toggle_active_low(struct gpio_desc *desc)
{
	desc->active_low = !desc->active_low;
}
