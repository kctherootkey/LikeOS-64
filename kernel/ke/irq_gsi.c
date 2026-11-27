// LikeOS -- which IDT vector an I/O APIC line (GSI) is routed to.
//
// The policy, kept free of any locking or table so it can be tested on
// its own; kernel/ke/irq.c holds the GSI -> vector table and calls this
// under its lock when a line is routed for the first time.
//
// The rule:
//
//   * a GSI keeps the vector it already has: every user of a shared line
//     -- a driver through irq_request_gsi(), a fixed-arm driver that
//     routed the line itself (ioapic_configure_legacy_irq) -- gets the
//     one vector the I/O APIC entry points at;
//   * GSIs 0..IRQ_GSI_NATURAL_LAST get their natural vector, 32 + GSI,
//     unless that vector has a fixed meaning (the local APIC timer and
//     error interrupts, the ACPI SCI, the NIC MSI vectors, the HPET
//     comparator: those arms in interrupt.c must never see a device
//     line, a handled line would skip them) or is taken already.  The
//     legacy INTx arms of interrupt.c (NICs, VMware SVGA, xHCI, PS/2)
//     recognise their interrupt by vector - 32 == line, so the low lines
//     must stay where they are;
//   * everything else -- a low GSI whose natural vector is fixed or busy,
//     and every GSI above IRQ_GSI_NATURAL_LAST -- gets a free vector of
//     the dynamic range (IRQ_DYN_VECTOR_FIRST..LAST, shared with the MSI
//     allocations of irq_alloc_vector), lowest first.
//
// The assignment is permanent: the I/O APIC entry keeps the vector, so
// the vector is never handed out again (a masked line can still latch).
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/ke/irq.h>

/* Vectors with a fixed meaning (interrupt.c, lapic.h).  irq.c checks
 * these against the constants of their owners at build time. */
static const int g_fixed_vectors[] = {
	IRQ_VEC_LAPIC_TIMER, /* LAPIC timer (and the PIT through ExtINT) */
	IRQ_VEC_LAPIC_ERROR, /* LAPIC error */
	IRQ_VEC_ACPI_SCI, /* ACPI SCI */
	IRQ_VEC_E1000_MSI, /* E1000 MSI */
	IRQ_VEC_E1000E_MSI, /* e1000e MSI */
	IRQ_VEC_VMXNET3_MSI, /* vmxnet3 MSI */
	IRQ_VEC_HRTIMER, /* HPET comparator */
};

int irq_vector_is_fixed(int vector)
{
	for (unsigned int i = 0;
	     i < sizeof(g_fixed_vectors) / sizeof(g_fixed_vectors[0]); i++)
		if (g_fixed_vectors[i] == vector)
			return 1;
	return 0;
}

int irq_gsi_choose_vector(int gsi, int (*busy)(int vector, void *ctx),
			  void *ctx)
{
	if (gsi < 0 || gsi >= IRQ_GSI_MAX || !busy)
		return -1;
	if (gsi <= IRQ_GSI_NATURAL_LAST && !irq_vector_is_fixed(32 + gsi) &&
	    !busy(32 + gsi, ctx))
		return 32 + gsi;
	for (int v = IRQ_DYN_VECTOR_FIRST; v <= IRQ_DYN_VECTOR_LAST; v++)
		if (!irq_vector_is_fixed(v) && !busy(v, ctx))
			return v;
	return -1;
}
