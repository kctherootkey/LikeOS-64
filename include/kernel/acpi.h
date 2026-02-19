// LikeOS-64 - ACPI Support for SMP
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration

#ifndef _KERNEL_ACPI_H_
#define _KERNEL_ACPI_H_

#include "types.h"

// Maximum number of CPUs supported
#define MAX_CPUS            64

// ============================================================================
// ACPI Table Signatures
// ============================================================================
#define ACPI_SIG_RSDP       "RSD PTR "
#define ACPI_SIG_RSDT       "RSDT"
#define ACPI_SIG_XSDT       "XSDT"
#define ACPI_SIG_MADT       "APIC"
#define ACPI_SIG_FADT       "FACP"
#define ACPI_SIG_HPET       "HPET"

// ============================================================================
// ACPI Table Structures
// ============================================================================

// Root System Description Pointer (RSDP)
typedef struct __attribute__((packed)) {
    char signature[8];          // "RSD PTR "
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;           // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_address;
    // ACPI 2.0+ fields follow
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

// Common ACPI table header (SDT header)
typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// Root System Description Table (RSDT) - 32-bit pointers
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t entries[];         // Array of 32-bit physical addresses
} acpi_rsdt_t;

// Extended System Description Table (XSDT) - 64-bit pointers
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t entries[];         // Array of 64-bit physical addresses
} acpi_xsdt_t;

// ============================================================================
// MADT (Multiple APIC Description Table) Structures
// ============================================================================

// MADT header
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t lapic_address;     // Physical address of LAPIC
    uint32_t flags;             // bit 0: PC-AT compatible dual-8259 present
} acpi_madt_t;

// MADT entry types
#define MADT_TYPE_LAPIC             0   // Processor Local APIC
#define MADT_TYPE_IOAPIC            1   // I/O APIC
#define MADT_TYPE_ISO               2   // Interrupt Source Override
#define MADT_TYPE_NMI_SOURCE        3   // NMI Source
#define MADT_TYPE_LAPIC_NMI         4   // Local APIC NMI
#define MADT_TYPE_LAPIC_ADDR        5   // Local APIC Address Override
#define MADT_TYPE_IOSAPIC           6   // I/O SAPIC
#define MADT_TYPE_LSAPIC            7   // Local SAPIC
#define MADT_TYPE_PLATFORM_INT      8   // Platform Interrupt Sources
#define MADT_TYPE_LAPIC_X2          9   // Processor Local x2APIC
#define MADT_TYPE_LAPIC_X2_NMI      10  // Local x2APIC NMI

// Generic MADT entry header
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_entry_header_t;

// MADT Local APIC entry (type 0)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;  // ACPI processor unique ID
    uint8_t apic_id;            // Processor's local APIC ID
    uint32_t flags;             // bit 0: enabled, bit 1: online capable
} madt_lapic_t;

#define MADT_LAPIC_ENABLED          0x01
#define MADT_LAPIC_ONLINE_CAPABLE   0x02

// MADT I/O APIC entry (type 1)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;    // Physical address
    uint32_t gsi_base;          // Global System Interrupt base
} madt_ioapic_t;

// MADT Interrupt Source Override entry (type 2)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t bus;                // Always 0 (ISA)
    uint8_t source;             // Bus-relative IRQ source
    uint32_t gsi;               // Global System Interrupt
    uint16_t flags;             // MPS INTI flags
} madt_iso_t;

// MPS INTI flags for polarity and trigger mode
#define MPS_INTI_POLARITY_MASK      0x03
#define MPS_INTI_POLARITY_DEFAULT   0x00
#define MPS_INTI_POLARITY_HIGH      0x01
#define MPS_INTI_POLARITY_LOW       0x03

#define MPS_INTI_TRIGGER_MASK       0x0C
#define MPS_INTI_TRIGGER_DEFAULT    0x00
#define MPS_INTI_TRIGGER_EDGE       0x04
#define MPS_INTI_TRIGGER_LEVEL      0x0C

// MADT Local APIC NMI entry (type 4)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t acpi_processor_id;  // 0xFF means all processors
    uint16_t flags;             // MPS INTI flags
    uint8_t lint;               // LINT# (0 or 1)
} madt_lapic_nmi_t;

// MADT Local APIC Address Override entry (type 5)
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint16_t reserved;
    uint64_t lapic_address;     // 64-bit physical address
} madt_lapic_addr_t;

// ============================================================================
// CPU Information Structure
// ============================================================================

typedef struct {
    uint8_t apic_id;            // APIC ID
    uint8_t acpi_processor_id;  // ACPI processor ID
    bool enabled;               // CPU is enabled
    bool online_capable;        // CPU can be brought online
    bool bsp;                   // Bootstrap processor
    bool started;               // CPU has been started (for APs)
} cpu_info_t;

// ============================================================================
// I/O APIC Information Structure
// ============================================================================

typedef struct {
    uint8_t id;                 // I/O APIC ID
    uint32_t address;           // Physical address
    uint32_t gsi_base;          // Global System Interrupt base
} ioapic_info_t;

// Maximum I/O APICs
#define MAX_IOAPICS         8

// ============================================================================
// Interrupt Source Override Information
// ============================================================================

typedef struct {
    uint8_t bus_irq;            // Bus-relative IRQ (ISA IRQ)
    uint32_t gsi;               // Global System Interrupt
    uint8_t polarity;           // 0=default, 1=high, 3=low
    uint8_t trigger_mode;       // 0=default, 1=edge, 3=level
} irq_override_t;

#define MAX_IRQ_OVERRIDES   24

// ============================================================================
// ACPI Global State
// ============================================================================

typedef struct {
    // RSDP information
    bool rsdp_found;
    uint8_t acpi_revision;      // 0 = ACPI 1.0, 2+ = ACPI 2.0+
    uint64_t rsdp_phys_addr;    // RSDP physical address (for UEFI)
    
    // LAPIC information
    uint64_t lapic_address;     // LAPIC physical address
    
    // CPU information
    cpu_info_t cpus[MAX_CPUS];
    uint32_t cpu_count;
    uint32_t bsp_apic_id;       // BSP APIC ID
    
    // I/O APIC information
    ioapic_info_t ioapics[MAX_IOAPICS];
    uint32_t ioapic_count;
    
    // Interrupt Source Overrides
    irq_override_t irq_overrides[MAX_IRQ_OVERRIDES];
    uint32_t irq_override_count;
    
    // MADT flags
    bool dual_8259_present;     // PC-AT compatible dual-8259 present
} acpi_info_t;

// ============================================================================
// ACPI Functions
// ============================================================================

// Initialize ACPI subsystem: find RSDP, parse tables
// rsdp_hint: Hint for RSDP location (e.g., from UEFI, or 0 to search BIOS area)
int acpi_init(uint64_t rsdp_hint);

// Get ACPI information structure
acpi_info_t* acpi_get_info(void);

// Get number of CPUs found
uint32_t acpi_get_cpu_count(void);

// Get CPU info by index
cpu_info_t* acpi_get_cpu(uint32_t index);

// Get BSP APIC ID
uint32_t acpi_get_bsp_apic_id(void);

// Get LAPIC physical address
uint64_t acpi_get_lapic_address(void);

// Get I/O APIC information by index
ioapic_info_t* acpi_get_ioapic(uint32_t index);

// Get IRQ override for a given ISA IRQ
// Returns NULL if no override exists
irq_override_t* acpi_get_irq_override(uint8_t isa_irq);

// Translate ISA IRQ to GSI (applies overrides)
uint32_t acpi_irq_to_gsi(uint8_t isa_irq);

// Debug: Print ACPI information
void acpi_print_info(void);

#endif // _KERNEL_ACPI_H_
