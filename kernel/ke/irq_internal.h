// LikeOS - interrupt core internals: the per-CPU vector matrix
//
// The matrix answers one question: which IDT vector of which CPU is free.
// Every CPU has its own handler table (kernel/ke/irq.c maps (cpu, vector)
// to an interrupt descriptor), so the same vector number can carry a
// different interrupt on every CPU.  Three kinds of entries:
//
//   * SYSTEM vectors are taken on every CPU at once: the fixed vectors of
//     the old drivers and the global pool of irq_alloc_vector().  Nobody
//     else may use the bit on any CPU.
//   * MANAGED reservations: a managed interrupt (one whose affinity never
//     changes) reserves one bit on EVERY CPU of its mask when it is set
//     up, so that its vector can always be allocated on whichever of those
//     CPUs it is started on.  The allocation then takes one reserved bit
//     on the CPU in the mask with the fewest managed allocations.
//   * REGULAR allocations: one bit on the CPU of the mask with the most
//     free vectors.
//
// Accounting per CPU: `available' counts the bits neither allocated
// regularly nor reserved for managed interrupts; global_available sums it
// over the online CPUs.
//
// Locking: the caller serialises every call (kernel/ke/irq.c's vector
// lock, taken with interrupts off).  No allocation happens in here.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_KE_IRQ_INTERNAL_H
#define KERNEL_KE_IRQ_INTERNAL_H

#include <kernel/uapi/types.h>
#include <kernel/ke/cpumask.h>
#include <kernel/ke/irq.h>

/* The IDT vectors the matrix manages: 128 .. 239.  32..62 are the fixed
 * legacy vectors (I/O APIC lines, the old drivers' MSI vectors, the HPET),
 * 63..127 irq_alloc_vector()'s system-wide vectors (legacy table only),
 * 240..255 the IPIs and the spurious vector. */
#define IRQ_MATRIX_BITS 256
#define IRQ_MATRIX_FIRST 128
#define IRQ_MATRIX_END 240 /* first vector past the range */
/* Regular and managed allocations start their search here. */
#define IRQ_MATRIX_SEARCH_START 128

#define IRQ_MATRIX_WORDS (IRQ_MATRIX_BITS / 64)

struct irq_cpumap {
	unsigned int available; /* free for regular allocation */
	unsigned int allocated; /* bits set in alloc_map */
	unsigned int managed; /* bits reserved in managed_map */
	unsigned int managed_allocated; /* managed bits also allocated */
	uint64_t alloc_map[IRQ_MATRIX_WORDS];
	uint64_t managed_map[IRQ_MATRIX_WORDS];
};

struct irq_matrix {
	unsigned int alloc_start, alloc_end; /* [start, end) */
	unsigned int search_start;
	unsigned int alloc_size;
	unsigned int systembits_inalloc;
	unsigned int total_allocated;
	unsigned int global_reserved;
	uint64_t system_map[IRQ_MATRIX_WORDS];
	/* Vectors an I/O APIC line is routed to (32 + GSI); inside the range
	 * they are system vectors as well, never released. */
	uint64_t legacy_map[IRQ_MATRIX_WORDS];
	struct irq_cpumap maps[MAX_CPUS];
};

void irq_matrix_setup(struct irq_matrix *m, unsigned int alloc_start,
		      unsigned int alloc_end, unsigned int search_start);
/* Take `bit' on every CPU (a fixed vector).  Fails (-EBUSY) when any CPU
 * has it allocated or reserved. */
int irq_matrix_assign_system(struct irq_matrix *m, unsigned int bit);
/* A system-wide vector from [first, last]: free on every CPU.  -ENOSPC. */
int irq_matrix_alloc_system(struct irq_matrix *m, unsigned int first,
			    unsigned int last);
/* A system-wide vector: released (unless an I/O APIC line holds it too,
 * see irq_matrix_assign_legacy). */
void irq_matrix_free_system(struct irq_matrix *m, unsigned int bit);
int irq_matrix_is_system(const struct irq_matrix *m, unsigned int bit);
/* The fixed vector of an I/O APIC line (32 + GSI), taken on every CPU the
 * moment the line is routed and kept for good -- the legacy interrupts the
 * system vectors are reserved for.  0 when taken (or already a system
 * vector of the pool, which then shares the vector-indexed table with the
 * line, or outside the managed range); -EBUSY when a per-CPU allocation
 * or a managed reservation holds the bit on some CPU. */
int irq_matrix_assign_legacy(struct irq_matrix *m, unsigned int bit);
int irq_matrix_is_legacy(const struct irq_matrix *m, unsigned int bit);

/* Reserve one managed bit on every CPU in msk (offline ones included):
 * 0 or -ENOSPC (nothing reserved then). */
int irq_matrix_reserve_managed(struct irq_matrix *m, const cpumask_t *msk);
/* Undo one reservation per CPU in msk (an unallocated reserved bit). */
void irq_matrix_remove_managed(struct irq_matrix *m, const cpumask_t *msk);
/* Allocate a reserved bit on the online CPU of msk with the fewest
 * managed allocations: the vector, *cpu; -EINVAL/-ENOSPC. */
int irq_matrix_alloc_managed(struct irq_matrix *m, const cpumask_t *msk,
			     unsigned int *cpu);
/* Regular allocation on the online CPU of msk with the most free bits. */
int irq_matrix_alloc(struct irq_matrix *m, const cpumask_t *msk, int reserved,
		     unsigned int *cpu);
/* `num' (a power of two) consecutive regular bits aligned to `num' on
 * one online CPU of msk (multi-message MSI): the first vector. */
int irq_matrix_alloc_block(struct irq_matrix *m, const cpumask_t *msk,
			   unsigned int num, unsigned int *cpu);
/* Release an allocation (regular or managed; a managed bit stays
 * reserved). */
void irq_matrix_free(struct irq_matrix *m, unsigned int cpu, unsigned int bit,
		     int managed);
/* Book-keeping of not-yet-allocated regular interrupts. */
void irq_matrix_reserve(struct irq_matrix *m);
void irq_matrix_remove_reserved(struct irq_matrix *m);
unsigned int irq_matrix_available(const struct irq_matrix *m);
unsigned int irq_matrix_allocated_on(const struct irq_matrix *m,
				     unsigned int cpu);
int irq_matrix_test_alloc(const struct irq_matrix *m, unsigned int cpu,
			  unsigned int bit);

/* ---- unhandled-interrupt ("nobody cared") accounting, pure ------------
 *
 * Per interrupt source.  After every hard interrupt the OR of the handlers'
 * results is noted: a source that nobody claimed 99,900 times out of
 * 100,000 is stuck and is to be disabled.  An unclaimed interrupt more
 * than 100 ms after the previous unclaimed one starts the count at 1 (an
 * odd stray interrupt must not add up to a disable over weeks); the
 * window only advances while unclaimed ones are being counted.  A result
 * of IRQ_WAKE_THREAD alone is judged at the NEXT hard interrupt: by
 * whether any thread function returned IRQ_HANDLED in between
 * (`threads_handled', a counter the threads increment). */
#define IRQ_SPURIOUS_WINDOW 100000u
#define IRQ_SPURIOUS_LIMIT 99900u
#define IRQ_SPURIOUS_RESET_NS (100ULL * 1000ULL * 1000ULL)
#define IRQ_SPURIOUS_DEFERRED 0x80000000u

struct irq_spurious {
	uint32_t irq_count; /* interrupts in the current window */
	uint32_t unhandled; /* of those, claimed by nobody */
	uint64_t last_unhandled_ns;
	uint32_t threads_handled_last; /* threads_handled seen | DEFERRED */
};

#define IRQ_SPURIOUS_OK 0
#define IRQ_SPURIOUS_BOGUS 1 /* a handler returned no IRQ_* value */
#define IRQ_SPURIOUS_STUCK 2 /* disable the source now */
/* `clock' gives monotonic nanoseconds (read only for unclaimed ones). */
int irq_spurious_note(struct irq_spurious *sp, int action_ret,
		      uint32_t threads_handled, uint64_t (*clock)(void));

/* Is `cpu' an online CPU (the matrix allocates only there). */
static inline int irq_matrix_cpu_online(unsigned int cpu)
{
	return cpu < cpumask_nr_online();
}

/* ---- interrupt descriptors: PCI-core-only entry points (kernel/ke/irq.c)
 *
 * irq_alloc_desc() (irq.h) makes a descriptor whose vector is allocated
 * when the first handler is requested and released with the last.  The
 * PCI core needs two more kinds:
 *
 *   * an INTx descriptor: the line is an I/O APIC pin (GSI) with its fixed
 *     vector 32+gsi, shared through the legacy handler table with the old
 *     drivers; chip->mask/unmask toggle the device's INTx disable bit;
 *   * a multi-message MSI block: n vectors that the device addresses as
 *     base+0 .. base+n-1, so they are allocated together, contiguous and
 *     aligned, on ONE CPU, at once (not at request time) -- `data[i]' is
 *     the chip data of message i; compose_msg is called for every message
 *     (the chip writes the capability for message 0).
 */
int irq_alloc_desc_gsi(const struct irq_chip_ops *ops, void *data, int gsi,
		       int active_low, int level);
int irq_alloc_desc_block(const struct irq_chip_ops *ops, void *const *data,
			 unsigned int n, const cpumask_t *affinity, int *irqs);
/* A descriptor with no CPU vector of its own: its source is delivered
 * through another interrupt whose handler calls irq_handle_virtual() (a
 * device behind a VMD domain with MSI remapping, a GPIO pad behind its
 * controller's shared line -- kernel/dev/gpio/gpiolib.c).  compose_msg is called
 * with vector 0 / APIC ID 0 at startup and on restore; the chip composes
 * its own message.  `affinity' and `managed' are recorded (and reported)
 * but delivery follows the parent interrupt. */
int irq_alloc_desc_virtual(const struct irq_chip_ops *ops, void *data,
			   const cpumask_t *affinity, int managed);
/* Run the handlers of a virtual descriptor (hard IRQ context, from the
 * parent's handler; the parent acknowledged the CPU). */
void irq_handle_virtual(int irq);
/* The (cpu, vector) an interrupt is delivered to now: 0, or -ENOENT when
 * it is not started (no vector). */
int irq_desc_target(int irq, unsigned int *cpu, unsigned int *vector,
		    uint32_t *apic_id);
/* Program the source again with its current vector and target (state
 * restore after a reset or D3): calls compose_msg when started. */
void irq_desc_recompose(int irq);
/* Is the interrupt masked by disable_irq()/oneshot right now. */
int irq_desc_masked(int irq);
/* Deliver the interrupt once more by IPI to its target (a pending
 * interrupt that a source without a mask bit could not hold). */
int irq_desc_retrigger(int irq);
/* The fixed system vectors (32..62) and the dynamic ones: one-time set-up,
 * called from idt_init() (any order relative to the first allocation). */
void irq_core_init(void);

#endif /* KERNEL_KE_IRQ_INTERNAL_H */
