// LikeOS-64 -- interrupt registration.
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
#ifndef KERNEL_KE_IRQ_H
#define KERNEL_KE_IRQ_H

#include <kernel/uapi/types.h>

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

#endif
