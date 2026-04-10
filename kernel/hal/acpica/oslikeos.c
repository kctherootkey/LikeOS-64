/*
 * oslikeos.c - ACPICA OS Services Layer for LikeOS-64
 *
 * Based on the Zephyr RTOS ACPICA OSL (oszephyr.c) by Intel Corp.
 * Adapted for the LikeOS-64 freestanding 64-bit kernel.
 *
 * Copyright (c) 1999 - 2025, Intel Corp. (ACPICA)
 * See kernel/hal/acpica/LICENSE for ACPICA licensing terms.
 */

#include "acpi.h"
#include "accommon.h"

/* LikeOS kernel headers */
#include <kernel/memory.h>
#include <kernel/console.h>
#include <kernel/interrupt.h>
#include <kernel/sched.h>
#include <kernel/timer.h>

/* Forward declarations for PCI config access (kernel/hal/pci.c) */
extern unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev,
                                   unsigned char func, unsigned char off);
extern void pci_cfg_write32(unsigned char bus, unsigned char dev,
                           unsigned char func, unsigned char off,
                           unsigned int value);

/* RSDP physical address — set by acpi_init() before ACPICA is started */
static ACPI_PHYSICAL_ADDRESS g_acpica_rsdp_address;

void acpica_set_rsdp(uint64_t phys_addr)
{
    g_acpica_rsdp_address = (ACPI_PHYSICAL_ADDRESS)phys_addr;
}

/* Required globals for ACPICA when not linking the compiler/disassembler */
BOOLEAN AslGbl_DoTemplates = FALSE;
BOOLEAN AslGbl_VerboseTemplates = FALSE;

/*
 * Initialization / Termination
 */
ACPI_STATUS
AcpiOsInitialize(void)
{
    return (AE_OK);
}

ACPI_STATUS
AcpiOsTerminate(void)
{
    return (AE_OK);
}

/*
 * RSDP
 */
ACPI_PHYSICAL_ADDRESS
AcpiOsGetRootPointer(void)
{
    return g_acpica_rsdp_address;
}

/*
 * Memory Allocation
 */
void *
AcpiOsAllocate(ACPI_SIZE Size)
{
    return kalloc((size_t)Size);
}

void
AcpiOsFree(void *Mem)
{
    if (Mem)
        kfree(Mem);
}

#ifdef USE_NATIVE_ALLOCATE_ZEROED
void *
AcpiOsAllocateZeroed(ACPI_SIZE Size)
{
    void *mem = kalloc((size_t)Size);
    if (mem)
        kmemset(mem, 0, (size_t)Size);
    return mem;
}
#endif

/*
 * Memory Mapping — ACPICA needs to access ACPI tables in physical memory
 */
void *
AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    /* LikeOS direct-maps physical memory below 16GB at PHYS_MAP_BASE.
     * ACPI tables are always in low physical memory, so phys_to_virt works. */
    return phys_to_virt((uint64_t)Where);
}

void
AcpiOsUnmapMemory(void *Where, ACPI_SIZE Length)
{
    /* Direct-map: nothing to undo */
    (void)Where;
    (void)Length;
}

ACPI_STATUS
AcpiOsGetPhysicalAddress(void *LogicalAddress,
                         ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    if (!LogicalAddress || !PhysicalAddress)
        return (AE_BAD_PARAMETER);
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)virt_to_phys(LogicalAddress);
    return (AE_OK);
}

BOOLEAN
AcpiOsReadable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return (TRUE);
}

BOOLEAN
AcpiOsWritable(void *Pointer, ACPI_SIZE Length)
{
    (void)Pointer;
    (void)Length;
    return (TRUE);
}

/*
 * I/O Port Access
 */
static inline uint16_t likeos_inw(uint16_t port)
{
    uint16_t val;
    __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t likeos_inl(uint16_t port)
{
    uint32_t val;
    __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void likeos_outw(uint16_t port, uint16_t val)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void likeos_outl(uint16_t port, uint32_t val)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

ACPI_STATUS
AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    if (!Value)
        return (AE_BAD_PARAMETER);

    switch (Width) {
    case 8:
        *Value = (UINT32)inb((uint16_t)Address);
        break;
    case 16:
        *Value = (UINT32)likeos_inw((uint16_t)Address);
        break;
    case 32:
        *Value = (UINT32)likeos_inl((uint16_t)Address);
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    switch (Width) {
    case 8:
        outb((uint16_t)Address, (uint8_t)Value);
        break;
    case 16:
        likeos_outw((uint16_t)Address, (uint16_t)Value);
        break;
    case 32:
        likeos_outl((uint16_t)Address, (uint32_t)Value);
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

/*
 * Physical Memory Read / Write
 */
ACPI_STATUS
AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width)
{
    volatile void *ptr = phys_to_virt((uint64_t)Address);

    if (!Value)
        return (AE_BAD_PARAMETER);

    switch (Width) {
    case 8:
        *Value = *(volatile uint8_t *)ptr;
        break;
    case 16:
        *Value = *(volatile uint16_t *)ptr;
        break;
    case 32:
        *Value = *(volatile uint32_t *)ptr;
        break;
    case 64:
        *Value = *(volatile uint64_t *)ptr;
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width)
{
    volatile void *ptr = phys_to_virt((uint64_t)Address);

    switch (Width) {
    case 8:
        *(volatile uint8_t *)ptr = (uint8_t)Value;
        break;
    case 16:
        *(volatile uint16_t *)ptr = (uint16_t)Value;
        break;
    case 32:
        *(volatile uint32_t *)ptr = (uint32_t)Value;
        break;
    case 64:
        *(volatile uint64_t *)ptr = (uint64_t)Value;
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

/*
 * PCI Configuration Access
 */
ACPI_STATUS
AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register,
                           UINT64 *Value, UINT32 Width)
{
    UINT32 val32;

    if (!PciId || !Value)
        return (AE_BAD_PARAMETER);

    val32 = pci_cfg_read32(PciId->Bus, PciId->Device,
                           PciId->Function, Register & 0xFC);

    switch (Width) {
    case 8:
        *Value = (val32 >> ((Register & 3) * 8)) & 0xFF;
        break;
    case 16:
        *Value = (val32 >> ((Register & 2) * 8)) & 0xFFFF;
        break;
    case 32:
        *Value = val32;
        break;
    case 64:
        *Value = val32;
        *Value |= ((UINT64)pci_cfg_read32(PciId->Bus, PciId->Device,
                                           PciId->Function,
                                           (Register + 4) & 0xFC)) << 32;
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Register,
                            UINT64 Value, UINT32 Width)
{
    UINT32 val32;

    if (!PciId)
        return (AE_BAD_PARAMETER);

    switch (Width) {
    case 8:
        val32 = pci_cfg_read32(PciId->Bus, PciId->Device,
                               PciId->Function, Register & 0xFC);
        val32 &= ~(0xFF << ((Register & 3) * 8));
        val32 |= ((UINT32)(Value & 0xFF)) << ((Register & 3) * 8);
        pci_cfg_write32(PciId->Bus, PciId->Device,
                       PciId->Function, Register & 0xFC, val32);
        break;
    case 16:
        val32 = pci_cfg_read32(PciId->Bus, PciId->Device,
                               PciId->Function, Register & 0xFC);
        val32 &= ~(0xFFFF << ((Register & 2) * 8));
        val32 |= ((UINT32)(Value & 0xFFFF)) << ((Register & 2) * 8);
        pci_cfg_write32(PciId->Bus, PciId->Device,
                       PciId->Function, Register & 0xFC, val32);
        break;
    case 32:
        pci_cfg_write32(PciId->Bus, PciId->Device,
                       PciId->Function, Register & 0xFC, (UINT32)Value);
        break;
    case 64:
        pci_cfg_write32(PciId->Bus, PciId->Device,
                       PciId->Function, Register & 0xFC, (UINT32)Value);
        pci_cfg_write32(PciId->Bus, PciId->Device,
                       PciId->Function, (Register + 4) & 0xFC,
                       (UINT32)(Value >> 32));
        break;
    default:
        return (AE_BAD_PARAMETER);
    }
    return (AE_OK);
}

/*
 * Overrides — allow table/predefined overrides (we don't override anything)
 */
ACPI_STATUS
AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal,
                         ACPI_STRING *NewVal)
{
    if (!InitVal || !NewVal)
        return (AE_BAD_PARAMETER);
    *NewVal = NULL;
    return (AE_OK);
}

ACPI_STATUS
AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                    ACPI_TABLE_HEADER **NewTable)
{
    if (!ExistingTable || !NewTable)
        return (AE_BAD_PARAMETER);
    *NewTable = NULL;
    return (AE_OK);
}

ACPI_STATUS
AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable,
                            ACPI_PHYSICAL_ADDRESS *NewAddress,
                            UINT32 *NewTableLength)
{
    if (!ExistingTable || !NewAddress || !NewTableLength)
        return (AE_BAD_PARAMETER);
    *NewAddress = 0;
    *NewTableLength = 0;
    return (AE_OK);
}

/*
 * Timing
 */
void
AcpiOsStall(UINT32 Microseconds)
{
    /* Busy-wait using I/O port delay (~1.2µs per iteration) */
    while (Microseconds > 0) {
        outb(0x80, 0);
        if (Microseconds > 1)
            Microseconds -= 1;
        else
            break;
    }
}

void
AcpiOsSleep(UINT64 Milliseconds)
{
    /* Busy-wait — no scheduler sleep in early kernel init */
    UINT64 us = Milliseconds * 1000;
    while (us > 0) {
        outb(0x80, 0);
        if (us > 1)
            us -= 1;
        else
            break;
    }
}

UINT64
AcpiOsGetTimer(void)
{
    /* Return 100ns units. timer_ticks() returns ~1000 Hz ticks. */
    return (UINT64)timer_ticks() * 10000;
}

ACPI_STATUS
AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue)
{
    (void)SleepState;
    (void)RegaValue;
    (void)RegbValue;
    return (AE_OK);
}

/*
 * Locking (single-threaded stubs)
 */
ACPI_STATUS
AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    *OutHandle = (ACPI_SPINLOCK)1;  /* Non-NULL dummy */
    return (AE_OK);
}

void
AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
}

ACPI_CPU_FLAGS
AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    (void)Handle;
    return (0);
}

void
AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    (void)Handle;
    (void)Flags;
}

ACPI_STATUS
AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits,
                      ACPI_HANDLE *OutHandle)
{
    (void)MaxUnits;
    (void)InitialUnits;
    *OutHandle = (ACPI_HANDLE)1;
    return (AE_OK);
}

ACPI_STATUS
AcpiOsDeleteSemaphore(ACPI_HANDLE Handle)
{
    (void)Handle;
    return (AE_OK);
}

ACPI_STATUS
AcpiOsWaitSemaphore(ACPI_HANDLE Handle, UINT32 Units, UINT16 Timeout)
{
    (void)Handle;
    (void)Units;
    (void)Timeout;
    return (AE_OK);
}

ACPI_STATUS
AcpiOsSignalSemaphore(ACPI_HANDLE Handle, UINT32 Units)
{
    (void)Handle;
    (void)Units;
    return (AE_OK);
}

/*
 * Mutex functions are NOT defined here because ACPI_MUTEX_TYPE defaults to
 * ACPI_BINARY_SEMAPHORE, which causes actypes.h to #define AcpiOsCreateMutex
 * etc. as macros redirecting to the semaphore functions above.
 */

ACPI_THREAD_ID
AcpiOsGetThreadId(void)
{
    return (ACPI_THREAD_ID)1;
}

ACPI_STATUS
AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function,
              void *Context)
{
    (void)Type;
    /* Single-threaded: just call it directly */
    if (Function)
        Function(Context);
    return (AE_OK);
}

void
AcpiOsWaitEventsComplete(void)
{
    /* Nothing — single-threaded */
}

/*
 * Interrupt Handling — ACPI SCI support
 *
 * ACPICA calls AcpiOsInstallInterruptHandler during AcpiEnableSubsystem()
 * to register the SCI (System Control Interrupt) handler.  We save the
 * callback and wire it into the kernel's IDT via IOAPIC.
 *
 * The SCI is typically GSI 9 (level-triggered, active-high) on Intel
 * platforms.  It must be routed through IOAPIC, not the legacy PIC.
 */

#include <kernel/ioapic.h>
#include <kernel/lapic.h>

// SCI vector in the IDT — pick an unused vector
#define ACPI_SCI_VECTOR  58

// Saved ACPICA SCI handler
static ACPI_OSD_HANDLER acpi_sci_handler;
static void            *acpi_sci_context;
static UINT32           acpi_sci_gsi;

UINT32
AcpiOsInstallInterruptHandler(UINT32 InterruptNumber,
                              ACPI_OSD_HANDLER ServiceRoutine,
                              void *Context)
{
    if (!ServiceRoutine)
        return (AE_BAD_PARAMETER);

    acpi_sci_handler = ServiceRoutine;
    acpi_sci_context = Context;
    acpi_sci_gsi     = InterruptNumber;

    kprintf("ACPI SCI: installing handler for GSI %d -> vector %d\n",
            InterruptNumber, ACPI_SCI_VECTOR);

    // Route SCI through IOAPIC: level-triggered, active-high (Dell Arrow Lake)
    int rc = ioapic_configure_legacy_irq((uint8_t)InterruptNumber,
                                          ACPI_SCI_VECTOR,
                                          IOAPIC_POLARITY_HIGH,
                                          IOAPIC_TRIGGER_LEVEL);
    if (rc) {
        kprintf("ACPI SCI: IOAPIC route failed (%d), trying active-low\n", rc);
        // Some platforms use active-low level-triggered
        ioapic_configure_legacy_irq((uint8_t)InterruptNumber,
                                     ACPI_SCI_VECTOR,
                                     IOAPIC_POLARITY_LOW,
                                     IOAPIC_TRIGGER_LEVEL);
    }

    // Mask the PIC line — IOAPIC delivers now
    irq_disable((uint8_t)InterruptNumber);

    return (AE_OK);
}

ACPI_STATUS
AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber,
                             ACPI_OSD_HANDLER ServiceRoutine)
{
    (void)ServiceRoutine;
    if (InterruptNumber == acpi_sci_gsi) {
        ioapic_mask_gsi((uint8_t)InterruptNumber);
        acpi_sci_handler = NULL;
        acpi_sci_context = NULL;
    }
    return (AE_OK);
}

// Called from the kernel's irq_handler dispatch for the SCI vector.
// Returns 1 if handled, 0 otherwise.
static int acpi_sci_count = 0;
int acpi_sci_dispatch(void)
{
    if (acpi_sci_handler) {
        acpi_sci_count++;
        if (acpi_sci_count <= 10)
            kprintf("ACPI SCI: dispatch #%d\n", acpi_sci_count);
        acpi_sci_handler(acpi_sci_context);
        return 1;
    }
    return 0;
}

// Return the GSI used for ACPI SCI, or 0 if not yet installed.
uint32_t acpi_get_sci_gsi(void)
{
    return acpi_sci_gsi;
}

// Manually invoke ACPICA's SCI handler to process pending GPE/PM events.
// Use when SCI interrupt delivery may not work (x2APIC IOAPIC routing issues).
void acpi_poll_events(void)
{
    if (acpi_sci_handler) {
        UINT32 result = acpi_sci_handler(acpi_sci_context);
        if (result)
            kprintf("ACPI SCI: polled, result=%d\n", result);
    }
}

/*
 * Output
 */
void ACPI_INTERNAL_VAR_XFACE
AcpiOsPrintf(const char *Fmt, ...)
{
    va_list args;
    va_start(args, Fmt);
    kvprintf(Fmt, args);
    va_end(args);
}

void
AcpiOsVprintf(const char *Fmt, va_list Args)
{
    kvprintf(Fmt, Args);
}

void
AcpiOsRedirectOutput(void *Destination)
{
    (void)Destination;
}

ACPI_STATUS
AcpiOsGetLine(char *Buffer, UINT32 BufferLength, UINT32 *BytesRead)
{
    (void)Buffer;
    (void)BufferLength;
    (void)BytesRead;
    return (AE_NOT_EXIST);
}

ACPI_STATUS
AcpiOsSignal(UINT32 Function, void *Info)
{
    (void)Function;
    (void)Info;
    return (AE_OK);
}

/*
 * Debug — not compiled in our config, but provide stubs
 */
ACPI_STATUS
AcpiOsInitializeDebugger(void)
{
    return (AE_OK);
}

void
AcpiOsTerminateDebugger(void)
{
}

ACPI_STATUS
AcpiOsWaitCommandReady(void)
{
    return (AE_NOT_EXIST);
}

ACPI_STATUS
AcpiOsNotifyCommandComplete(void)
{
    return (AE_OK);
}

void
AcpiOsTracePoint(ACPI_TRACE_EVENT_TYPE Type, BOOLEAN Begin,
                 UINT8 *Aml, char *Pathname)
{
    (void)Type;
    (void)Begin;
    (void)Aml;
    (void)Pathname;
}
