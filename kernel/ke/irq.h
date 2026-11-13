// LikeOS -- interrupt registration.
//
// A driver asks for an interrupt line and gives a handler; the dispatcher
// calls every handler registered on the vector that fired and acknowledges
// the local APIC once one of them claims it.  Before this, adding a device
// meant an IDT stub, a reserved vector constant and a hand-written arm in
// irq_handler() -- which is why three NICs and two USB controllers each
// have one, and why they are still there: they keep working unchanged,
// and new drivers use this instead.
//
// Three ways in:
//   irq_request_vector()  a vector already routed (an MSI a driver set up
//                         itself, or one from irq_alloc_vector()).
//   irq_request_gsi()     a global system interrupt through the I/O APIC,
//                         for a device on a shared, level-triggered line.
//   irq_request_msi()     allocate a vector, enable MSI on the PCI device
//                         aimed at a CPU, register the handler.
//   irq_request_msix()    the same over an MSI-X table entry.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_IRQ_H
#define KERNEL_KE_IRQ_H

#include <kernel/uapi/types.h>
#include <kernel/ke/cpumask.h>

struct pci_device;

/* Return 1 if the device raised this interrupt (it has been serviced),
 * 0 if it was not this device's -- on a shared line the next handler is
 * tried.  Runs with interrupts disabled on the CPU that took the interrupt;
 * may wake tasks and touch hardware, may not sleep. */
typedef int (*irq_handler_t)(void *arg);

/* Vectors handed out by irq_alloc_vector(). */
#define IRQ_DYN_VECTOR_FIRST 63
#define IRQ_DYN_VECTOR_LAST 127

int irq_alloc_vector(void); /* -1 when exhausted */
void irq_free_vector(int vector);

int irq_request_vector(int vector, irq_handler_t fn, void *arg,
		       const char *name);
int irq_request_gsi(int gsi, irq_handler_t fn, void *arg, const char *name,
		    int active_low, int level);
/* An I/O APIC line is about to be routed to `vector' (its fixed vector,
 * 32 + GSI): take it on every CPU for good, so the per-CPU allocator never
 * hands it out.  0, or -EBUSY when a per-CPU interrupt holds it already.
 * Called by the I/O APIC routing itself; any context. */
int irq_reserve_legacy_vector(int vector);
/* MSI / MSI-X: `cpu' is a CPU index (0 = boot CPU); the vector chosen is
 * returned through *vector_out. */
int irq_request_msi(const struct pci_device *dev, int cpu, irq_handler_t fn,
		    void *arg, const char *name, int *vector_out);
int irq_request_msix(const struct pci_device *dev, int entry, int cpu,
		     irq_handler_t fn, void *arg, const char *name,
		     int *vector_out);
void irq_free(int vector, irq_handler_t fn, void *arg);

/* Dispatcher: 1 if a registered handler claimed the vector (EOI done). */
int irq_dispatch(uint64_t vector);

/* ==========================================================================
 * Interrupt numbers, per-CPU vectors and managed affinity
 * (kernel/ke/irq*.c, kernel/ke/interrupt.c -- Phase 1 agent P1)
 * ==========================================================================
 *
 * The functions above hand out raw IDT vectors from one global pool of 65
 * (IRQ_DYN_VECTOR_FIRST..LAST): enough for a few devices, not for a
 * controller with one queue -- and one MSI-X vector -- per CPU.  The
 * interface below separates the INTERRUPT NUMBER a driver holds from the
 * VECTOR the CPU sees:
 *
 *   * an interrupt number (int irq, >= IRQ_NR_FIRST) names one interrupt
 *     source: one MSI-X entry, one MSI message, one INTx line.  It comes
 *     from pci_irq_vector() (kernel/hal/pci.h) or irq_alloc_desc();
 *   * its vector is allocated per CPU from a per-CPU vector matrix
 *     (each CPU has its own handler table over vectors 128..239),
 *     on a CPU of its affinity mask -- so 64 CPUs can each have dozens of
 *     queue vectors at the same vector number;
 *   * MANAGED interrupts (the spread of pci_alloc_irq_vectors_affinity)
 *     keep their affinity for life; unmanaged ones may be moved
 *     (irq_set_affinity).
 *
 * Handlers: irq_handler_t (above) returns IRQ_NONE (0, not mine) or
 * IRQ_HANDLED (1); a threaded handler's primary handler may return
 * IRQ_WAKE_THREAD to run `thread_fn' in a per-interrupt kernel thread
 * (process context, may sleep), with the interrupt masked at the device
 * until the thread function returns (IRQF_ONESHOT semantics).
 */

#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQ_WAKE_THREAD 2

/* The first interrupt number of the new scheme (numbers below are not
 * used, so an IDT vector can never be mistaken for one). */
#define IRQ_NR_FIRST 256
#define IRQ_NR_MAX 4096 /* interrupt descriptors system-wide */

/* request_irq() flags */
#define IRQF_SHARED (1u << 0) /* INTx line shared with other devices */
#define IRQF_ONESHOT (1u << 1) /* keep masked until thread_fn returns */
#define IRQF_NO_AUTOEN (1u << 2) /* leave disabled: enable_irq() later */

/* Register `fn' on interrupt `irq' (and unmask it unless IRQF_NO_AUTOEN).
 * `arg' identifies the registration for free_irq (the "dev_id").  0,
 * -EINVAL, -EBUSY (not shared), -ENOMEM, -ENOSPC (no vector on the
 * affinity's CPUs).  May sleep. */
int request_irq(int irq, irq_handler_t fn, unsigned long flags,
		const char *name, void *arg);
int request_threaded_irq(int irq, irq_handler_t fn, irq_handler_t thread_fn,
			 unsigned long flags, const char *name, void *arg);
/* Unregister; waits for running handlers (synchronize_irq).  Returns the
 * name it was registered with.  May sleep. */
const char *free_irq(int irq, void *arg);
/* Stop the handlers from running and wait for running ones; nests
 * (counts).  May sleep.  disable_irq_nosync: without the wait (any
 * context).  An INTx line is masked at the source (INTx disable) at
 * once; an MSI/MSI-X vector only when a message comes while it is
 * disabled, and that message is sent to its CPU again by enable_irq(). */
void disable_irq(int irq);
void disable_irq_nosync(int irq);
void enable_irq(int irq);
/* Wait until no handler of `irq' runs on any CPU.  May sleep. */
void synchronize_irq(int irq);
/* Wait until no hard handler of `irq' runs on any CPU; the interrupt
 * threads are not waited for.  True when none of them is active either.
 * Spins (any context).  (int: a boolean.) */
int synchronize_hardirq(int irq);
/* Affinity: the mask (managed: fixed), the effective CPU now. */
const cpumask_t *irq_get_affinity_mask(int irq);
int irq_get_effective_cpu(int irq);
/* Move an unmanaged interrupt (-EINVAL for a managed one).  May sleep. */
int irq_set_affinity(int irq, const cpumask_t *mask);
int irq_is_managed(int irq);
/* Count of interrupts taken (all CPUs), for /proc/interrupts. */
uint64_t irq_get_count(int irq);

/* Descriptors for interrupt sources that are not PCI vectors (the PCI
 * core uses these internally).  ops->compose_msg is called with the vector
 * and destination APIC ID chosen whenever they change (activation,
 * affinity move), so the source can be (re)programmed; NULL for an
 * already-routed vector source.  Returns the interrupt number or
 * -errno.  May sleep. */
struct irq_chip_ops {
	void (*compose_msg)(int irq, void *data, uint8_t vector,
			    uint32_t apic_id);
	void (*mask)(int irq, void *data);
	void (*unmask)(int irq, void *data);
	/* Optional, process context, no lock held: the first handler was
	 * requested (after the vector is set up, source still masked) /
	 * the last one freed (source masked, before the vector goes). */
	void (*startup)(int irq, void *data);
	void (*shutdown)(int irq, void *data);
};
int irq_alloc_desc(const struct irq_chip_ops *ops, void *data,
		   const cpumask_t *affinity, int managed);
void irq_free_desc(int irq);

/* ---- affinity spreading ------------------------------------------------- */

#define IRQ_AFFINITY_MAX_SETS 4

/* How a driver wants its vectors spread (pci_alloc_irq_vectors_affinity):
 * pre_vectors/post_vectors at the start/end are not spread (default
 * affinity); the rest is split into nr_sets sets of set_size[i] vectors,
 * each set spread over all CPUs.  calc_sets(affd, nvecs), when set, is
 * called with the number of vectors to spread (excluding pre/post) to
 * fill nr_sets/set_size for that count (NVMe: default + read queues). */
struct irq_affinity {
	unsigned int pre_vectors;
	unsigned int post_vectors;
	unsigned int nr_sets;
	unsigned int set_size[IRQ_AFFINITY_MAX_SETS];
	void (*calc_sets)(struct irq_affinity *affd, unsigned int nvecs);
	void *priv;
};

struct irq_affinity_desc {
	cpumask_t mask;
	int is_managed;
};

/* One mask per vector for `nvec' vectors as described by `affd'
 * (kcalloc'd array the caller kfree()s), NULL on -ENOMEM or nothing to
 * spread.  May sleep. */
struct irq_affinity_desc *irq_create_affinity_masks(unsigned int nvec,
						    struct irq_affinity *affd);
/* How many vectors are worth allocating between minvec and maxvec for
 * this spread (pre + post + at most one per possible CPU per set). */
unsigned int irq_calc_affinity_vectors(unsigned int minvec,
				       unsigned int maxvec,
				       const struct irq_affinity *affd);
/* Split the online CPUs into `numgrps' groups as evenly as possible,
 * keeping CPUs that are close together (memory node, then the cluster of
 * CPUs sharing an L2, SMT siblings taken together): returns a kcalloc'd
 * array of numgrps masks, every CPU in exactly one group, NULL on
 * -ENOMEM.  With more groups than CPUs only the first `ncpus' masks are
 * filled (one CPU each) and the rest stay empty; *nummasks gets the
 * number of masks filled (min(numgrps, CPUs)).  May sleep. */
cpumask_t *group_cpus_evenly(unsigned int numgrps, unsigned int *nummasks);

#endif
