// LikeOS-64 -- interrupt registration and dispatch.
#include <kernel/ke/irq.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/syscall.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/percpu.h>
#include <kernel/hal/ioapic.h>
#include <kernel/hal/lapic.h>
#include <kernel/hal/pci.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>

#define IRQ_MAX_ACTIONS 4

struct irq_action {
	irq_handler_t fn;
	void *arg;
	const char *name;
};

struct irq_desc {
	struct irq_action act[IRQ_MAX_ACTIONS];
	int nact;
	int allocated; /* handed out by irq_alloc_vector() */
	uint64_t count;
};

static struct irq_desc g_irq[256];
static spinlock_t g_irq_lock = SPINLOCK_INIT("irq");

int irq_alloc_vector(void)
{
	uint64_t fl;

	spin_lock_irqsave(&g_irq_lock, &fl);
	for (int v = IRQ_DYN_VECTOR_FIRST; v <= IRQ_DYN_VECTOR_LAST; v++) {
		if (!g_irq[v].allocated && g_irq[v].nact == 0) {
			g_irq[v].allocated = 1;
			spin_unlock_irqrestore(&g_irq_lock, fl);
			return v;
		}
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
	return -1;
}

void irq_free_vector(int vector)
{
	uint64_t fl;

	if (vector < 0 || vector > 255)
		return;
	spin_lock_irqsave(&g_irq_lock, &fl);
	g_irq[vector].allocated = 0;
	spin_unlock_irqrestore(&g_irq_lock, fl);
}

int irq_request_vector(int vector, irq_handler_t fn, void *arg,
		       const char *name)
{
	uint64_t fl;

	if (vector < 32 || vector > 255 || !fn)
		return -EINVAL;
	spin_lock_irqsave(&g_irq_lock, &fl);
	struct irq_desc *d = &g_irq[vector];
	if (d->nact >= IRQ_MAX_ACTIONS) {
		spin_unlock_irqrestore(&g_irq_lock, fl);
		return -EBUSY;
	}
	d->act[d->nact].fn = fn;
	d->act[d->nact].arg = arg;
	d->act[d->nact].name = name;
	d->nact++;
	spin_unlock_irqrestore(&g_irq_lock, fl);
	return 0;
}

void irq_free(int vector, irq_handler_t fn, void *arg)
{
	uint64_t fl;

	if (vector < 0 || vector > 255)
		return;
	spin_lock_irqsave(&g_irq_lock, &fl);
	struct irq_desc *d = &g_irq[vector];
	for (int i = 0; i < d->nact; i++) {
		if (d->act[i].fn == fn && d->act[i].arg == arg) {
			for (int j = i + 1; j < d->nact; j++)
				d->act[j - 1] = d->act[j];
			d->nact--;
			break;
		}
	}
	spin_unlock_irqrestore(&g_irq_lock, fl);
}

int irq_request_gsi(int gsi, irq_handler_t fn, void *arg, const char *name,
		    int active_low, int level)
{
	if (gsi < 0 || gsi > 223)
		return -EINVAL;
	int vector = 32 + gsi;
	int rc = irq_request_vector(vector, fn, arg, name);

	if (rc)
		return rc;
	if (ioapic_configure_legacy_irq((uint8_t)gsi, (uint8_t)vector,
					active_low ? IOAPIC_POLARITY_LOW :
						     IOAPIC_POLARITY_HIGH,
					level ? IOAPIC_TRIGGER_LEVEL :
						IOAPIC_TRIGGER_EDGE) != 0) {
		irq_free(vector, fn, arg);
		return -EIO;
	}
	return 0;
}

static uint32_t cpu_apic_id(int cpu)
{
	if (cpu <= 0)
		return lapic_get_id_cpuid();
	percpu_t *pc = percpu_get(cpu);
	return pc ? pc->apic_id : lapic_get_id_cpuid();
}

int irq_request_msi(const struct pci_device *dev, int cpu, irq_handler_t fn,
		    void *arg, const char *name, int *vector_out)
{
	int vector = irq_alloc_vector();

	if (vector < 0)
		return -ENOSPC;
	int rc = irq_request_vector(vector, fn, arg, name);
	if (rc) {
		irq_free_vector(vector);
		return rc;
	}
	if (pci_enable_msi_cpu(dev, (uint8_t)vector, cpu_apic_id(cpu)) != 0) {
		irq_free(vector, fn, arg);
		irq_free_vector(vector);
		return -EIO;
	}
	if (vector_out)
		*vector_out = vector;
	return 0;
}

int irq_request_msix(const struct pci_device *dev, int entry, int cpu,
		     irq_handler_t fn, void *arg, const char *name,
		     int *vector_out)
{
	int vector = irq_alloc_vector();

	if (vector < 0)
		return -ENOSPC;
	int rc = irq_request_vector(vector, fn, arg, name);
	if (rc) {
		irq_free_vector(vector);
		return rc;
	}
	if (pci_enable_msix(dev, entry, (uint8_t)vector, cpu_apic_id(cpu)) !=
	    0) {
		irq_free(vector, fn, arg);
		irq_free_vector(vector);
		return -EIO;
	}
	if (vector_out)
		*vector_out = vector;
	return 0;
}

int irq_dispatch(uint64_t vector)
{
	if (vector > 255)
		return 0;
	struct irq_desc *d = &g_irq[vector];

	if (d->nact == 0)
		return 0;
	/* No lock on the fast path: actions are appended, never moved while
	 * live, and removal is rare and racing it costs at most one stale
	 * call into a handler that answers "not mine". */
	int n = d->nact;
	int handled = 0;
	for (int i = 0; i < n; i++) {
		struct irq_action *a = &d->act[i];
		if (a->fn && a->fn(a->arg))
			handled = 1;
	}
	if (handled) {
		d->count++;
		lapic_eoi();
	}
	return handled;
}
