// LikeOS - ACPI Support for SMP
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_ACPI_H_
#define _KERNEL_ACPI_H_

#include <kernel/uapi/types.h>

// Maximum number of CPUs supported
#define MAX_CPUS 64

// ============================================================================
// ACPI Table Signatures (guarded — ACPICA may define these)
// ============================================================================
#ifndef ACPI_SIG_RSDP
#define ACPI_SIG_RSDP "RSD PTR "
#endif
#ifndef ACPI_SIG_RSDT
#define ACPI_SIG_RSDT "RSDT"
#endif
#ifndef ACPI_SIG_XSDT
#define ACPI_SIG_XSDT "XSDT"
#endif
#ifndef ACPI_SIG_MADT
#define ACPI_SIG_MADT "APIC"
#endif
#ifndef ACPI_SIG_FADT
#define ACPI_SIG_FADT "FACP"
#endif
#ifndef ACPI_SIG_HPET
#define ACPI_SIG_HPET "HPET"
#endif
#ifndef ACPI_SIG_SSDT
#define ACPI_SIG_SSDT "SSDT"
#endif

// ============================================================================
// ACPI Table Structures
// ============================================================================

// Root System Description Pointer (RSDP)
typedef struct __attribute__((packed)) {
	char signature[8]; // "RSD PTR "
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision; // 0 = ACPI 1.0, 2 = ACPI 2.0+
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
	uint32_t entries[]; // Array of 32-bit physical addresses
} acpi_rsdt_t;

// Extended System Description Table (XSDT) - 64-bit pointers
typedef struct __attribute__((packed)) {
	acpi_sdt_header_t header;
	uint64_t entries[]; // Array of 64-bit physical addresses
} acpi_xsdt_t;

// ============================================================================
// MADT (Multiple APIC Description Table) Structures
// ============================================================================

// MADT header
typedef struct __attribute__((packed)) {
	acpi_sdt_header_t header;
	uint32_t lapic_address; // Physical address of LAPIC
	uint32_t flags; // bit 0: PC-AT compatible dual-8259 present
} acpi_madt_t;

// MADT entry types
#define MADT_TYPE_LAPIC 0 // Processor Local APIC
#define MADT_TYPE_IOAPIC 1 // I/O APIC
#define MADT_TYPE_ISO 2 // Interrupt Source Override
#define MADT_TYPE_NMI_SOURCE 3 // NMI Source
#define MADT_TYPE_LAPIC_NMI 4 // Local APIC NMI
#define MADT_TYPE_LAPIC_ADDR 5 // Local APIC Address Override
#define MADT_TYPE_IOSAPIC 6 // I/O SAPIC
#define MADT_TYPE_LSAPIC 7 // Local SAPIC
#define MADT_TYPE_PLATFORM_INT 8 // Platform Interrupt Sources
#define MADT_TYPE_LAPIC_X2 9 // Processor Local x2APIC
#define MADT_TYPE_LAPIC_X2_NMI 10 // Local x2APIC NMI

// Generic MADT entry header
typedef struct __attribute__((packed)) {
	uint8_t type;
	uint8_t length;
} madt_entry_header_t;

// MADT Local APIC entry (type 0)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint8_t acpi_processor_id; // ACPI processor unique ID
	uint8_t apic_id; // Processor's local APIC ID
	uint32_t flags; // bit 0: enabled, bit 1: online capable
} madt_lapic_t;

#define MADT_LAPIC_ENABLED 0x01
#define MADT_LAPIC_ONLINE_CAPABLE 0x02

// MADT I/O APIC entry (type 1)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint8_t ioapic_id;
	uint8_t reserved;
	uint32_t ioapic_address; // Physical address
	uint32_t gsi_base; // Global System Interrupt base
} madt_ioapic_t;

// MADT Interrupt Source Override entry (type 2)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint8_t bus; // Always 0 (ISA)
	uint8_t source; // Bus-relative IRQ source
	uint32_t gsi; // Global System Interrupt
	uint16_t flags; // MPS INTI flags
} madt_iso_t;

// MPS INTI flags for polarity and trigger mode
#define MPS_INTI_POLARITY_MASK 0x03
#define MPS_INTI_POLARITY_DEFAULT 0x00
#define MPS_INTI_POLARITY_HIGH 0x01
#define MPS_INTI_POLARITY_LOW 0x03

#define MPS_INTI_TRIGGER_MASK 0x0C
#define MPS_INTI_TRIGGER_DEFAULT 0x00
#define MPS_INTI_TRIGGER_EDGE 0x04
#define MPS_INTI_TRIGGER_LEVEL 0x0C

// MADT Local APIC NMI entry (type 4)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint8_t acpi_processor_id; // 0xFF means all processors
	uint16_t flags; // MPS INTI flags
	uint8_t lint; // LINT# (0 or 1)
} madt_lapic_nmi_t;

// MADT Local APIC Address Override entry (type 5)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint16_t reserved;
	uint64_t lapic_address; // 64-bit physical address
} madt_lapic_addr_t;

// MADT Local x2APIC entry (type 9)
typedef struct __attribute__((packed)) {
	madt_entry_header_t header;
	uint16_t reserved;
	uint32_t x2apic_id; // Processor's local x2APIC ID
	uint32_t flags; // bit 0: enabled, bit 1: online capable
	uint32_t acpi_processor_uid; // ACPI processor UID
} madt_x2apic_t;

// ============================================================================
// CPU Information Structure
// ============================================================================

typedef struct {
	uint32_t apic_id; // APIC ID (32-bit for x2APIC support)
	uint32_t acpi_processor_id; // ACPI processor ID / UID
	bool enabled; // CPU is enabled
	bool online_capable; // CPU can be brought online
	bool bsp; // Bootstrap processor
	bool started; // CPU has been started (for APs)
} cpu_info_t;

// ============================================================================
// I/O APIC Information Structure
// ============================================================================

typedef struct {
	uint8_t id; // I/O APIC ID
	uint32_t address; // Physical address
	uint32_t gsi_base; // Global System Interrupt base
} ioapic_info_t;

// Maximum I/O APICs
#define MAX_IOAPICS 8

// ============================================================================
// Interrupt Source Override Information
// ============================================================================

typedef struct {
	uint8_t bus_irq; // Bus-relative IRQ (ISA IRQ)
	uint32_t gsi; // Global System Interrupt
	uint8_t polarity; // 0=default, 1=high, 3=low
	uint8_t trigger_mode; // 0=default, 1=edge, 3=level
} irq_override_t;

#define MAX_IRQ_OVERRIDES 24

// ============================================================================
// ACPI Global State
// ============================================================================

typedef struct {
	// RSDP information
	bool rsdp_found;
	uint8_t acpi_revision; // 0 = ACPI 1.0, 2+ = ACPI 2.0+
	uint64_t rsdp_phys_addr; // RSDP physical address (for UEFI)

	// LAPIC information
	uint64_t lapic_address; // LAPIC physical address

	// CPU information
	cpu_info_t cpus[MAX_CPUS];
	uint32_t cpu_count;
	uint32_t bsp_apic_id; // BSP APIC ID

	// I/O APIC information
	ioapic_info_t ioapics[MAX_IOAPICS];
	uint32_t ioapic_count;

	// Interrupt Source Overrides
	irq_override_t irq_overrides[MAX_IRQ_OVERRIDES];
	uint32_t irq_override_count;

	// MADT flags
	bool dual_8259_present; // PC-AT compatible dual-8259 present
} acpi_info_t;

// ============================================================================
// ACPI AML Device Discovery
// ============================================================================

#define ACPI_AML_MAX_PATH 64
#define ACPI_AML_MAX_HID 16

typedef struct {
	char path[ACPI_AML_MAX_PATH];
	char hid[ACPI_AML_MAX_HID];
	char cid[ACPI_AML_MAX_HID];
	char power_path[ACPI_AML_MAX_PATH];
	uint8_t has_ps0;
	uint8_t has_ps3;
	uint8_t has_sta;
	uint8_t has_crs;
	uint8_t has_dep;
	uint8_t from_ssdt;
} acpi_aml_device_info_t;

// ============================================================================
// AML Tagged Value Type System
// ============================================================================

#define AML_VALUE_INTEGER 0
#define AML_VALUE_BUFFER 1
#define AML_VALUE_PACKAGE 2
#define AML_VALUE_STRING 3
#define AML_VALUE_REFERENCE 4
#define AML_VALUE_UNINITIALIZED 5

#define AML_VALUE_MAX_STRING 256
#define AML_VALUE_MAX_BUFFER 4096
#define AML_VALUE_MAX_PACKAGE 64

// Forward declaration
struct aml_value;

typedef struct aml_value {
	uint8_t type;
	union {
		uint64_t integer;
		struct {
			uint8_t *data;
			uint32_t len;
		} buffer;
		struct {
			struct aml_value *elements;
			uint32_t count;
		} package;
		char string[AML_VALUE_MAX_STRING];
		struct {
			uint32_t index; // For package/buffer index references
			struct aml_value
				*owner; // The package/buffer this indexes into
		} reference;
	};
} aml_value_t;

// Bump allocator for temporary AML value storage during method execution
#define AML_BUMP_POOL_SIZE (32 * 1024) // 32 KB per method frame

typedef struct {
	uint8_t *pool;
	uint32_t used;
	uint32_t capacity;
} aml_bump_alloc_t;

// ============================================================================
// ACPI Functions
// ============================================================================

// Initialize ACPI subsystem: find RSDP, parse tables
// rsdp_hint: Hint for RSDP location (e.g., from UEFI, or 0 to search BIOS area)
int acpi_init(uint64_t rsdp_hint);

// Get ACPI information structure
acpi_info_t *acpi_get_info(void);

// Get number of CPUs found
uint32_t acpi_get_cpu_count(void);

// Get CPU info by index
cpu_info_t *acpi_get_cpu(uint32_t index);

// Get BSP APIC ID
uint32_t acpi_get_bsp_apic_id(void);

// Get LAPIC physical address
uint64_t acpi_get_lapic_address(void);

// Get I/O APIC information by index
ioapic_info_t *acpi_get_ioapic(uint32_t index);

// Get IRQ override for a given ISA IRQ
// Returns NULL if no override exists
irq_override_t *acpi_get_irq_override(uint8_t isa_irq);

// Translate ISA IRQ to GSI (applies overrides)
uint32_t acpi_irq_to_gsi(uint8_t isa_irq);

// Debug: Print ACPI information
void acpi_print_info(void);

// ============================================================================
// FADT (Fixed ACPI Description Table) for Power Management
// ============================================================================
typedef struct __attribute__((packed)) {
	acpi_sdt_header_t header;
	uint32_t firmware_ctrl;
	uint32_t dsdt;
	uint8_t reserved0;
	uint8_t preferred_pm_profile;
	uint16_t sci_interrupt;
	uint32_t smi_command_port;
	uint8_t acpi_enable;
	uint8_t acpi_disable;
	uint8_t s4bios_req;
	uint8_t pstate_control;
	uint32_t pm1a_event_block;
	uint32_t pm1b_event_block;
	uint32_t pm1a_control_block;
	uint32_t pm1b_control_block;
	uint32_t pm2_control_block;
	uint32_t pm_timer_block;
	uint32_t gpe0_block;
	uint32_t gpe1_block;
	uint8_t pm1_event_length;
	uint8_t pm1_control_length;
	uint8_t pm2_control_length;
	uint8_t pm_timer_length;
	uint8_t gpe0_block_length;
	uint8_t gpe1_block_length;
	uint8_t gpe1_base;
	uint8_t cstate_control;
	uint16_t worst_c2_latency;
	uint16_t worst_c3_latency;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t duty_offset;
	uint8_t duty_width;
	uint8_t day_alarm;
	uint8_t month_alarm;
	uint8_t century;
	uint16_t boot_arch_flags;
	uint8_t reserved1;
	uint32_t flags;
	/* Generic Address Structure for reset register (ACPI 2.0+) */
	uint8_t reset_reg_addr_space;
	uint8_t reset_reg_bit_width;
	uint8_t reset_reg_bit_offset;
	uint8_t reset_reg_access_size;
	uint64_t reset_reg_address;
	uint8_t reset_value;
	uint16_t arm_boot_arch;
	uint8_t fadt_minor_version;
	uint64_t x_firmware_ctrl;
	uint64_t x_dsdt;
	/* Extended PM registers (ACPI 2.0+) */
	uint8_t x_pm1a_event_block[12];
	uint8_t x_pm1b_event_block[12];
	uint8_t x_pm1a_control_block[12];
	uint8_t x_pm1b_control_block[12];
	uint8_t x_pm2_control_block[12];
	uint8_t x_pm_timer_block[12];
	uint8_t x_gpe0_block[12];
	uint8_t x_gpe1_block[12];
} acpi_fadt_t;

// PM1 control register bits
#define ACPI_PM1_SLP_EN (1 << 13)
#define ACPI_PM1_SLP_TYP(x) ((x) << 10)

// Power management: shutdown and reset
void acpi_poweroff(void);
void acpi_reset(void);

// 1 once acpi_init() has loaded and initialised the ACPI namespace (the
// device layer, kernel/hal/acpi_dev.h, refuses to work before).
int acpi_namespace_ready(void);

// Initialize power management (call after acpi_init)
void acpi_pm_init(void);

// Find an ACPI table by its 4-character signature
acpi_sdt_header_t *acpi_find_table(const char *signature);

// Find the Nth ACPI table matching a 4-character signature.
acpi_sdt_header_t *acpi_find_table_index(const char *signature, uint32_t index);

// Count ACPI tables matching a 4-character signature.
uint32_t acpi_get_table_count(const char *signature);

// Get the AML payload of the Nth table matching a signature.
// Returns 0 on success, <0 on failure.
int acpi_get_table_aml(const char *signature, uint32_t index,
		       const uint8_t **aml, uint32_t *aml_len);

// Scan DSDT and SSDTs for devices exposing a given _HID.
// If hid is NULL or empty, all devices with a parsed _HID are returned.
// Returns the number of devices written to out[] (up to max_out).
int acpi_aml_find_devices_by_hid(const char *hid, acpi_aml_device_info_t *out,
				 int max_out);

// ============================================================================
// ACPI Enhanced Evaluation Functions (aml_value_t based)
// ============================================================================

// Initialize / destroy bump allocator for AML value storage
void aml_bump_init(aml_bump_alloc_t *alloc);
void aml_bump_destroy(aml_bump_alloc_t *alloc);
void *aml_bump_alloc(aml_bump_alloc_t *alloc, uint32_t size);

// Deep copy
void aml_value_copy(aml_value_t *dst, const aml_value_t *src,
		    aml_bump_alloc_t *alloc);
// Activate a PNP device by reading _CRS and passing it to _SRS.
// This is what a conventional PNP subsystem does to enable devices.
int acpi_aml_activate_dev(const char *device_path);

// Service pending ACPI/EC events once. Useful for firmware-driven device
// bring-up paths that require SCI/GPE work before legacy I/O responds.
// Thread context.  Once acpi_late_init() ran, the events are handed to
// the ACPI work queues.
void acpi_service_events(void);

// The ACPI work queues (kacpid, kacpi_notify, kec, kec_query): from here
// on no AML runs in interrupt context, the SCI handler's methods and the
// EC's queries are run by these.  Once, right after workqueue_init().
void acpi_late_init(void);

// Return the GSI used for ACPI SCI, or 0 if not yet installed.
uint32_t acpi_get_sci_gsi(void);

// Evaluate an EC query method directly (for example _Q66) when firmware uses
// a state machine that can require repeated method execution.
int acpi_ec_eval_qxx(uint8_t qval);

// Look up PCI interrupt routing via ACPI _PRT on the parent bridge.
// bridge_path: ACPI path of PCI root bridge (e.g. "\\_SB_.PC00")
// pci_device: PCI device number (0-31)
// pci_pin: PCI interrupt pin (0=INTA, 1=INTB, 2=INTC, 3=INTD)
// out_gsi: receives the IOAPIC GSI number on success
// Returns 0 on success, <0 on failure.
int acpi_pci_lookup_irq(const char *bridge_path, uint8_t pci_device,
			uint8_t pci_pin, uint32_t *out_gsi);

// Read the ACPI PM Timer (3.579545 MHz, global chipset counter).
// Returns current counter value, or 0 if PM Timer unavailable.
uint32_t acpi_read_pmtimer(void);

#endif // _KERNEL_ACPI_H_
