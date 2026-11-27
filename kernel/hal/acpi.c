// LikeOS - ACPI Implementation using ACPICA Reference Implementation
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration
// AML evaluation delegated to ACPICA
//
// Copyright (C) 2026 The LikeOS Project

// ACPICA headers (must be included before kernel acpi.h to avoid macro conflicts)
#include "acpica/include/acpi.h"

#include <kernel/hal/acpi.h>
#include <kernel/io/console.h>
#include <kernel/mm/memory.h>
#include <kernel/ke/interrupt.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/workqueue.h>
#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/uapi/bug.h>
/* AcpiHwClearAcpiStatus() is internal to ACPICA and reachable through no
 * public header; achware.h, where it is declared, needs most of ACPICA's
 * private types to parse.  So the one prototype is repeated here rather than
 * assumed -- an assumed function returns int, and one that in fact returns
 * something wider is silently truncated at the call. */
ACPI_STATUS AcpiHwClearAcpiStatus(void);

#define ACPI_DEBUG 0
#if ACPI_DEBUG
#define acpi_dbg(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#else
#define acpi_dbg(fmt, ...) \
	do {               \
	} while (0)
#endif

// ============================================================================
// Global ACPI State
// ============================================================================

static acpi_info_t g_acpi_info = { 0 };

/* The namespace is loaded and initialised (acpi_init got through). */
static volatile int g_acpi_namespace_ready;

int acpi_namespace_ready(void)
{
	return __atomic_load_n(&g_acpi_namespace_ready, __ATOMIC_ACQUIRE);
}

// ACPICA RSDP address setter (defined in oslikeos.c)
extern void acpica_set_rsdp(uint64_t phys_addr);

// ============================================================================
// Helper Functions
// ============================================================================

static void my_memset(void *dst, int val, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	for (size_t i = 0; i < n; i++)
		d[i] = (uint8_t)val;
}

static void my_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
}

static void my_strncpy(char *dst, const char *src, int n)
{
	int i;
	for (i = 0; i < n - 1 && src[i]; i++)
		dst[i] = src[i];
	dst[i] = 0;
}

// ============================================================================
// MADT Parsing (raw table walk - not AML, kept from original)
// ============================================================================

static void acpi_parse_madt(void)
{
	ACPI_TABLE_HEADER *hdr = NULL;
	ACPI_STATUS status = AcpiGetTable(ACPI_SIG_MADT, 1, &hdr);
	if (ACPI_FAILURE(status) || !hdr) {
		acpi_dbg("ACPI: MADT not found\n");
		return;
	}

	acpi_madt_t *madt = (acpi_madt_t *)hdr;
	acpi_dbg("ACPI: MADT found, length=%u\n", madt->header.length);

	g_acpi_info.lapic_address = madt->lapic_address;
	WARN_ON(g_acpi_info.lapic_address &
		(PAGE_SIZE - 1)); /* LAPIC MMIO base not page-aligned */
	g_acpi_info.dual_8259_present = (madt->flags & 1) != 0;

	uint8_t *ptr = (uint8_t *)madt + sizeof(acpi_madt_t);
	uint8_t *end = (uint8_t *)madt + madt->header.length;

	while (ptr < end) {
		madt_entry_header_t *entry = (madt_entry_header_t *)ptr;
		if (entry->length == 0)
			break;
		WARN_ON(entry->length <
			2); /* MADT entry length < 2 is malformed */

		switch (entry->type) {
		case MADT_TYPE_LAPIC: {
			madt_lapic_t *lapic = (madt_lapic_t *)entry;
			if (g_acpi_info.cpu_count < MAX_CPUS) {
				cpu_info_t *cpu =
					&g_acpi_info
						 .cpus[g_acpi_info.cpu_count];
				cpu->apic_id = lapic->apic_id;
				cpu->acpi_processor_id =
					lapic->acpi_processor_id;
				cpu->enabled = (lapic->flags &
						MADT_LAPIC_ENABLED) != 0;
				cpu->online_capable =
					(lapic->flags &
					 MADT_LAPIC_ONLINE_CAPABLE) != 0;
				cpu->bsp = false;
				cpu->started = false;
				if (cpu->enabled || cpu->online_capable)
					g_acpi_info.cpu_count++;
			}
			break;
		}
		case MADT_TYPE_IOAPIC: {
			madt_ioapic_t *ioapic = (madt_ioapic_t *)entry;
			if (g_acpi_info.ioapic_count < MAX_IOAPICS) {
				ioapic_info_t *info =
					&g_acpi_info.ioapics
						 [g_acpi_info.ioapic_count];
				info->id = ioapic->ioapic_id;
				info->address = ioapic->ioapic_address;
				WARN_ON(info->address &
					(PAGE_SIZE -
					 1)); /* IOAPIC MMIO base not page-aligned */
				info->gsi_base = ioapic->gsi_base;
				g_acpi_info.ioapic_count++;
			}
			break;
		}
		case MADT_TYPE_ISO: {
			madt_iso_t *iso = (madt_iso_t *)entry;
			if (g_acpi_info.irq_override_count <
			    MAX_IRQ_OVERRIDES) {
				irq_override_t *override =
					&g_acpi_info.irq_overrides
						 [g_acpi_info
							  .irq_override_count];
				override->bus_irq = iso->source;
				override->gsi = iso->gsi;
				override->polarity =
					iso->flags & MPS_INTI_POLARITY_MASK;
				override->trigger_mode =
					(iso->flags & MPS_INTI_TRIGGER_MASK) >>
					2;
				g_acpi_info.irq_override_count++;
			}
			break;
		}
		case MADT_TYPE_LAPIC_ADDR: {
			madt_lapic_addr_t *addr = (madt_lapic_addr_t *)entry;
			g_acpi_info.lapic_address = addr->lapic_address;
			break;
		}
		case MADT_TYPE_LAPIC_X2: {
			madt_x2apic_t *x2 = (madt_x2apic_t *)entry;
			if (g_acpi_info.cpu_count < MAX_CPUS) {
				cpu_info_t *cpu =
					&g_acpi_info
						 .cpus[g_acpi_info.cpu_count];
				cpu->apic_id = x2->x2apic_id;
				cpu->acpi_processor_id = x2->acpi_processor_uid;
				cpu->enabled =
					(x2->flags & MADT_LAPIC_ENABLED) != 0;
				cpu->online_capable =
					(x2->flags &
					 MADT_LAPIC_ONLINE_CAPABLE) != 0;
				cpu->bsp = false;
				cpu->started = false;
				if (cpu->enabled || cpu->online_capable)
					g_acpi_info.cpu_count++;
			}
			break;
		}
		default:
			break;
		}
		ptr += entry->length;
	}
}

// ============================================================================
// Embedded Controller (EC) Address Space Handler
//
// The EC uses two IO ports (data + command/status) with a standard protocol.
// AML methods (e.g. _SRS for PS/2 devices) access EC registers through this.
// Without this handler, ACPICA cannot evaluate AML that touches EC OpRegions.
//
// Serialisation: every transaction (one operation region access, one
// query) holds ec_mutex -- AML runs in many tasks at once, and ACPICA
// leaves its interpreter while an operation region handler runs -- and,
// when the EC's _GLK says so, the ACPI global lock inside it (with a
// timeout, as a holder of the global lock may be waiting for ec_mutex).
//
// Events: the EC's GPE handler (interrupt context, or the boot-time
// poll) only looks at SCI_EVT.  The queries run in an ordered work queue
// (kec), with the GPE left disabled until they are done, and each event's
// _Qxx method in a second one (kec_query), in arrival order.  Before the
// work queues exist (acpi_init), the boot-time poll queries and runs the
// methods itself, after the SCI handler returned.
// ============================================================================

// EC status register bits
#define EC_FLAG_OBF 0x01 // Output buffer full (data ready)
#define EC_FLAG_IBF 0x02 // Input buffer full  (busy)

// EC commands
#define EC_CMD_READ 0x80
#define EC_CMD_WRITE 0x81

// EC IO ports — populated from ECDT or PNP0C09 _CRS
static uint16_t ec_data_port = 0;
static uint16_t ec_cmd_port = 0;
static int ec_initialized = 0;
static ACPI_HANDLE ec_device_handle = NULL; // saved for deferred _REG call
static DEFINE_MUTEX(ec_mutex); // one transaction at a time
static int ec_use_global_lock; // _GLK: transactions take the global lock
#define EC_GLK_TIMEOUT_MS 1000

/* Begin a transaction: 0, or -1 when the global lock could not be had.
 * Thread context (the mutex spins where sleeping is impossible: before
 * the scheduler, with interrupts off). */
static int ec_lock(UINT32 *glk)
{
	*glk = 0;
	mutex_lock(&ec_mutex);
	if (ec_use_global_lock &&
	    ACPI_FAILURE(AcpiAcquireGlobalLock(EC_GLK_TIMEOUT_MS, glk))) {
		mutex_unlock(&ec_mutex);
		return -1;
	}
	return 0;
}

static void ec_unlock(UINT32 glk)
{
	if (ec_use_global_lock)
		(void)AcpiReleaseGlobalLock(glk);
	mutex_unlock(&ec_mutex);
}

static int ec_wait_ibf_clear(void)
{
	for (int i = 0; i < 20000; i++) { // ~1s at 50µs per iter
		if (!(inb(ec_cmd_port) & EC_FLAG_IBF))
			return 0;
		for (int d = 0; d < 50; d++)
			__asm__ volatile("outb %%al, $0x80" ::: "memory");
	}
	return -1;
}

static int ec_wait_obf_set(void)
{
	for (int i = 0; i < 20000; i++) {
		if (inb(ec_cmd_port) & EC_FLAG_OBF)
			return 0;
		for (int d = 0; d < 50; d++)
			__asm__ volatile("outb %%al, $0x80" ::: "memory");
	}
	return -1;
}

static int ec_read_byte(uint8_t address, uint8_t *data)
{
	if (ec_wait_ibf_clear())
		return -1;
	outb(ec_cmd_port, EC_CMD_READ);
	if (ec_wait_ibf_clear())
		return -1;
	outb(ec_data_port, address);
	if (ec_wait_obf_set())
		return -1;
	*data = inb(ec_data_port);
	return 0;
}

static int ec_write_byte(uint8_t address, uint8_t data)
{
	if (ec_wait_ibf_clear())
		return -1;
	outb(ec_cmd_port, EC_CMD_WRITE);
	if (ec_wait_ibf_clear())
		return -1;
	outb(ec_data_port, address);
	if (ec_wait_ibf_clear())
		return -1;
	outb(ec_data_port, data);
	return 0;
}

// ACPICA address space handler for EC operations.
// Called when AML accesses an EmbeddedControl OpRegion.
static ACPI_STATUS ec_space_handler(UINT32 Function,
				    ACPI_PHYSICAL_ADDRESS Address,
				    UINT32 BitWidth, UINT64 *Value,
				    void *HandlerContext, void *RegionContext)
{
	int bytes = BitWidth / 8;
	uint8_t *val = (uint8_t *)Value;
	ACPI_STATUS st = AE_OK;
	UINT32 glk;

	(void)HandlerContext;
	(void)RegionContext;

	if (Address > 0xFF || !Value)
		return AE_BAD_PARAMETER;
	if (Function != ACPI_READ && Function != ACPI_WRITE)
		return AE_BAD_PARAMETER;
	if (ec_lock(&glk))
		return AE_NOT_FOUND;

	/* The whole access is one transaction: a multi-byte field is not
	 * torn by another task's access in between. */
	for (int i = 0; i < bytes; i++) {
		int rc;
		if (Function == ACPI_READ) {
			rc = ec_read_byte((uint8_t)(Address + i), &val[i]);
			acpi_dbg("ACPI EC: RD [0x%02x] = 0x%02x%s\n",
				 (unsigned)(Address + i), val[i],
				 rc ? " FAIL" : "");
		} else {
			acpi_dbg("ACPI EC: WR [0x%02x] <- 0x%02x\n",
				 (unsigned)(Address + i), val[i]);
			rc = ec_write_byte((uint8_t)(Address + i), val[i]);
			if (rc)
				acpi_dbg("ACPI EC: WR FAIL\n");
		}
		if (rc) {
			st = AE_TIME;
			break;
		}
	}
	ec_unlock(glk);
	return st;
}

// Parse IO ports from EC _CRS — first IO resource = data, second = command
typedef struct {
	uint16_t data_port;
	uint16_t cmd_port;
} ec_ports_t;

static ACPI_STATUS ec_parse_io(ACPI_RESOURCE *Resource, void *Context)
{
	ec_ports_t *ports = (ec_ports_t *)Context;

	if (Resource->Type == ACPI_RESOURCE_TYPE_IO) {
		if (ports->data_port == 0)
			ports->data_port = Resource->Data.Io.Minimum;
		else if (ports->cmd_port == 0)
			ports->cmd_port = Resource->Data.Io.Minimum;
	}
	return AE_OK;
}

// Find the EC device and install address space handler.
// Must be called BEFORE AcpiEnableSubsystem/AcpiInitializeObjects so that
// _INI and _SRS methods can access EC registers.
static void acpi_ec_init(void)
{
	ACPI_STATUS status;
	ACPI_HANDLE ec_handle = NULL;
	ec_ports_t ports = { 0, 0 };

	// 1. Try ECDT table first (available before namespace init)
	ACPI_TABLE_ECDT *ecdt = NULL;
	status = AcpiGetTable(ACPI_SIG_ECDT, 1, (ACPI_TABLE_HEADER **)&ecdt);
	if (ACPI_SUCCESS(status) && ecdt && ecdt->Control.Address &&
	    ecdt->Data.Address) {
		ports.cmd_port = (uint16_t)ecdt->Control.Address;
		ports.data_port = (uint16_t)ecdt->Data.Address;
		// Get the EC handle from ECDT namespace path
		if (ecdt->Id[0])
			AcpiGetHandle(NULL, (char *)ecdt->Id, &ec_handle);
		acpi_dbg("ACPI EC: ECDT data=0x%x cmd=0x%x\n", ports.data_port,
			 ports.cmd_port);
	}

	// 2. Try finding PNP0C09 (EC device) in namespace
	if (!ports.data_port || !ports.cmd_port) {
		ACPI_HANDLE dev = NULL;
		status = AcpiGetDevices("PNP0C09", NULL, NULL, &dev);
		// AcpiGetDevices with NULL callback won't work — use AcpiGetHandle scan.
		// Instead, walk namespace looking for PNP0C09
		acpi_aml_device_info_t ec_info;
		if (acpi_aml_find_devices_by_hid("PNP0C09", &ec_info, 1) > 0) {
			status = AcpiGetHandle(NULL, ec_info.path, &ec_handle);
			if (ACPI_SUCCESS(status)) {
				// Parse _CRS for IO ports
				status = AcpiWalkResources(ec_handle, "_CRS",
							   ec_parse_io, &ports);
				if (ACPI_SUCCESS(status) && ports.data_port &&
				    ports.cmd_port)
					acpi_dbg(
						"ACPI EC: PNP0C09 at %s data=0x%x cmd=0x%x\n",
						ec_info.path, ports.data_port,
						ports.cmd_port);
			}
		}
	}

	if (!ports.data_port || !ports.cmd_port) {
		acpi_dbg("ACPI EC: no EC found\n");
		return;
	}

	ec_data_port = ports.data_port;
	ec_cmd_port = ports.cmd_port;

	// _GLK: the firmware wants the global lock around EC transactions.
	if (ec_handle) {
		ACPI_OBJECT glk;
		ACPI_BUFFER gbuf = { sizeof(glk), &glk };

		if (ACPI_SUCCESS(AcpiEvaluateObject(ec_handle, "_GLK", NULL,
						    &gbuf)) &&
		    glk.Type == ACPI_TYPE_INTEGER && glk.Integer.Value)
			ec_use_global_lock = 1;
	}

	// 3. Install EC address space handler at root.
	// Use No_Reg variant — _REG must be called AFTER AcpiInitializeObjects
	// so the AML namespace is fully ready. Proper sequence:
	//   acpi_install_address_space_handler_no_reg() first,
	//   acpi_execute_reg_methods() later.
	status = AcpiInstallAddressSpaceHandlerNo_Reg(
		ACPI_ROOT_OBJECT, ACPI_ADR_SPACE_EC, ec_space_handler,
		NULL, // No setup handler
		NULL); // No context

	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI EC: handler install failed (%d)\n", (int)status);
		return;
	}

	ec_initialized = 1;
	ec_device_handle = ec_handle;
	acpi_dbg("ACPI EC: handler installed (data=0x%x cmd=0x%x)\n",
		 ec_data_port, ec_cmd_port);

	// Verify EC ports are accessible — if they read 0xFF, eSPI is dead
	uint8_t ec_sts = inb(ec_cmd_port);
	acpi_dbg("ACPI EC: status port reads 0x%02x%s\n", ec_sts,
		 ec_sts == 0xFF ? " (WARNING: eSPI may be non-functional)" :
				  "");
}

// ============================================================================
// ACPICA Exception Handler — catch AML execution failures
// ============================================================================

static ACPI_STATUS acpi_exception_handler(ACPI_STATUS AmlStatus, ACPI_NAME Name,
					  UINT16 Opcode, UINT32 AmlOffset,
					  void *Context)
{
	(void)Context;
	char name_str[5];
	my_memcpy(name_str, &Name, 4);
	name_str[4] = 0;
	acpi_dbg(
		"ACPI AML EXCEPTION: status=%d name=%s opcode=0x%04x offset=0x%x\n",
		(int)AmlStatus, name_str, Opcode, AmlOffset);
	return AmlStatus; // propagate the error
}

// ============================================================================
// Stub OpRegion Handlers — prevent silent AML failures
// ============================================================================

// GPIO handler — Dell DSDTs commonly use GPIO OpRegions.
//
// ACPICA hands the field's Connection() resource template to the handler
// through the handler context (it fills Connection/Length/AccessLength in
// the ACPI_CONNECTION_INFO given at install time, under the handler's
// context mutex), Address is the pin index within that template and
// BitWidth the number of pins.  The pins are driven through the GPIO
// layer once their controller has registered; before that (AML run at
// _INI time) and for pins the GPIO layer refuses, reads return 0 and
// writes are dropped, as they always were.
//
// The context is shared by every GPIO field access: ACPICA fills it and
// calls the handler under the handler's context mutex (a real mutex, see
// oslikeos_sync.c), so the Connection read here is the one of this very
// access.  AML runs in thread context only (the SCI's methods are
// deferred to work queues); the GPIO layer sleeps and allocates, so an
// access from anywhere else gets the old stub behaviour.
static ACPI_CONNECTION_INFO g_gpio_conn_info;

static ACPI_STATUS gpio_space_handler(UINT32 Function,
				      ACPI_PHYSICAL_ADDRESS Address,
				      UINT32 BitWidth, UINT64 *Value,
				      void *HandlerContext, void *RegionContext)
{
	ACPI_CONNECTION_INFO *ci = HandlerContext;
	int rc = -1;

	(void)RegionContext;
	acpi_dbg("ACPI GPIO: %s addr=0x%llx bits=%d\n",
		 Function == ACPI_READ ? "RD" : "WR",
		 (unsigned long long)Address, BitWidth);
	if (!Value)
		return AE_BAD_PARAMETER;
	/* Before the scheduler (AML of acpi_init) there is no GPIO
	 * controller yet; with interrupts off the GPIO layer cannot be
	 * used. */
	if (ci && ci->Connection && ci->Length && sched_current() &&
	    !WARN_ON_ONCE(irqs_disabled()))
		rc = acpi_gpio_opregion_access(Function == ACPI_WRITE,
					       (uint32_t)Address, BitWidth,
					       Value, ci->Connection,
					       ci->Length);
	if (rc != 0 && Function == ACPI_READ)
		*Value = 0; // no controller (yet) or pin refused: low
	return AE_OK;
}

// Run _REG(EmbeddedControl, CONNECT) starting from ACPI_ROOT_OBJECT.
// This tells ALL AML code that the EC OpRegion handler is now available.
// Must be called BETWEEN AcpiEnableSubsystem and AcpiInitializeObjects:
//   - _REG must be called after EnableSubsystem,
//     BEFORE InitializeObjects, so that _INI methods can access EC.
//   - _INI methods (run during InitializeObjects) write EC registers
//     to enable KBC emulation on 0x60/0x64.
static void acpi_ec_call_reg(void)
{
	if (!ec_initialized)
		return;

	// Propagate _REG to ALL devices in the namespace, not just the EC
	// device.  Other devices may have EC OpRegions too.
	ACPI_STATUS status =
		AcpiExecuteRegMethods(ACPI_ROOT_OBJECT, ACPI_ADR_SPACE_EC);
	if (ACPI_FAILURE(status))
		acpi_dbg("ACPI EC: _REG failed (%d)\n", (int)status);
	else
		acpi_dbg("ACPI EC: _REG(EC, 1) propagated from root\n");

	// Probe i8042 ports after _REG to see if EC enabled KBC emulation
	uint8_t ps2_sts = inb(0x64);
	acpi_dbg("ACPI EC: post-_REG i8042 status=0x%02x%s\n", ps2_sts,
		 ps2_sts == 0xFF ? " (still dead)" : " (alive!)");
}

// ============================================================================
// EC GPE + Event Handling
// ============================================================================

// EC query command (QR_SMB) — read event number from EC
#define EC_CMD_QUERY 0x84
#define EC_FLAG_SCI 0x20 // SCI_EVT pending

static uint32_t ec_gpe_number = 0;

// The event work queues (acpi_late_init); NULL before.
static struct workqueue_struct *ec_wq; // the queries (ordered)
static struct workqueue_struct *ec_query_wq; // the _Qxx methods (ordered)
static struct work_struct ec_event_work;
#define EC_QUERIES_PER_PASS 8

struct ec_query_item {
	struct work_struct work;
	uint8_t qval;
};

// One query transaction: the number of the next pending event in *qval,
// 0 when there is none.  0, or -1 when the EC did not answer.
static int ec_query(uint8_t *qval)
{
	UINT32 glk;
	int rc = -1;

	*qval = 0;
	if (!ec_initialized || !ec_cmd_port)
		return -1;
	if (ec_lock(&glk))
		return -1;
	if (!(inb(ec_cmd_port) & EC_FLAG_SCI)) {
		rc = 0;
		goto out;
	}
	if (ec_wait_ibf_clear())
		goto out;
	outb(ec_cmd_port, EC_CMD_QUERY);
	if (ec_wait_obf_set())
		goto out;
	*qval = inb(ec_data_port);
	rc = 0;
out:
	ec_unlock(glk);
	return rc;
}

static void ec_query_work_fn(struct work_struct *work)
{
	struct ec_query_item *qi =
		container_of(work, struct ec_query_item, work);

	(void)acpi_ec_eval_qxx(qi->qval);
	kfree(qi);
}

// Run the event's _Qxx: queued once the queues exist, here before.
static void ec_dispatch_event(uint8_t qval)
{
	struct workqueue_struct *q =
		__atomic_load_n(&ec_query_wq, __ATOMIC_ACQUIRE);
	struct ec_query_item *qi;

	if (!q) {
		(void)acpi_ec_eval_qxx(qval);
		return;
	}
	qi = kcalloc(1, sizeof(*qi));
	if (!qi) {
		kprintf("ACPI EC: no memory, event 0x%02x dropped\n", qval);
		return;
	}
	INIT_WORK(&qi->work, ec_query_work_fn);
	qi->qval = qval;
	(void)queue_work(q, &qi->work);
}

// The queries of one GPE: until the EC has no more events.  Returns
// how many it had.
static int ec_drain_events(void)
{
	int n = 0;

	for (int pass = 0; pass < EC_QUERIES_PER_PASS; pass++) {
		uint8_t qval;

		if (ec_query(&qval) || qval == 0)
			break;
		acpi_dbg("ACPI EC: GPE event 0x%02x\n", qval);
		ec_dispatch_event(qval);
		n++;
	}
	return n;
}

static void ec_event_work_fn(struct work_struct *work)
{
	int n;

	(void)work;
	n = ec_drain_events();
	// The handler left the GPE disabled for us (or it was enabled
	// all along: a conditional enable then changes nothing).
	(void)AcpiFinishGpe(NULL, ec_gpe_number);
	// Events that came in meanwhile (the GPE was off): once more.
	if (n && (inb(ec_cmd_port) & EC_FLAG_SCI))
		(void)queue_work(ec_wq, &ec_event_work);
}

// GPE handler for EC events (called by ACPICA when GPE fires), in the
// SCI handler or the boot-time poll, under ACPICA's GPE lock: it must not
// run a transaction (that sleeps) nor AML.
static UINT32 ec_gpe_handler_cb(ACPI_HANDLE gpe_dev, UINT32 gpe_num, void *ctx)
{
	struct workqueue_struct *q = __atomic_load_n(&ec_wq, __ATOMIC_ACQUIRE);

	(void)gpe_dev;
	(void)gpe_num;
	(void)ctx;

	if (!ec_initialized || !ec_cmd_port)
		return ACPI_REENABLE_GPE;
	if (!(inb(ec_cmd_port) & EC_FLAG_SCI))
		return ACPI_REENABLE_GPE;
	if (!q) {
		// acpi_init: the poll that called us queries next.
		return ACPI_REENABLE_GPE;
	}
	// The GPE stays disabled until the work has queried the EC empty
	// (ec_event_work_fn finishes it): a level that stays up while the
	// events wait cannot storm.
	(void)queue_work(q, &ec_event_work);
	return ACPI_INTERRUPT_HANDLED;
}

// Set up EC GPE: read _GPE, install handler, enable GPE in hardware.
// The EC firmware checks the host's GPE enable register before
// activating KBC emulation on ports 0x60/0x64.
static void ec_setup_gpe(void)
{
	if (!ec_device_handle)
		return;

	// Read _GPE from EC device to get GPE number
	ACPI_OBJECT obj;
	ACPI_BUFFER buf = { sizeof(obj), &obj };
	ACPI_STATUS st =
		AcpiEvaluateObject(ec_device_handle, "_GPE", NULL, &buf);
	if (ACPI_SUCCESS(st) && obj.Type == ACPI_TYPE_INTEGER) {
		ec_gpe_number = (uint32_t)obj.Integer.Value;
	} else {
		ec_gpe_number = 0x6E;
		acpi_dbg("ACPI EC: _GPE eval failed, using 0x%x\n",
			 ec_gpe_number);
	}
	acpi_dbg("ACPI EC: GPE=0x%x\n", ec_gpe_number);

	// Print FADT GPE block info for diagnostics
	acpi_dbg("ACPI: FADT GPE0=0x%llx len=%d GPE1=0x%llx len=%d\n",
		 (unsigned long long)AcpiGbl_FADT.XGpe0Block.Address,
		 AcpiGbl_FADT.Gpe0BlockLength,
		 (unsigned long long)AcpiGbl_FADT.XGpe1Block.Address,
		 AcpiGbl_FADT.Gpe1BlockLength);

	// Install GPE handler
	st = AcpiInstallGpeHandler(NULL, ec_gpe_number, ACPI_GPE_EDGE_TRIGGERED,
				   ec_gpe_handler_cb, NULL);
	if (ACPI_FAILURE(st)) {
		acpi_dbg("ACPI EC: GPE handler install failed: %d\n", (int)st);
		return;
	}

	// Enable GPE — sets the enable bit in hardware PM registers
	st = AcpiEnableGpe(NULL, ec_gpe_number);
	if (ACPI_FAILURE(st)) {
		acpi_dbg("ACPI EC: GPE enable failed: %d\n", (int)st);
		return;
	}

	acpi_dbg("ACPI EC: GPE 0x%x handler installed and enabled\n",
		 ec_gpe_number);
}

// Process pending EC events by sending query commands.
// Fallback when SCI interrupt doesn't deliver GPE events.  Thread
// context; at boot (acpi_init) the _Qxx methods run right here.
static void ec_process_pending_events(void)
{
	if (!ec_initialized || !ec_cmd_port)
		return;

	// Also call ACPICA's SCI handler to process PM/GPE status registers
	// (and, at boot, the GPE methods it deferred).
	extern void acpi_poll_events(void);
	acpi_poll_events();

	for (int pass = 0; pass < EC_QUERIES_PER_PASS; pass++) {
		uint8_t qval;

		if (ec_query(&qval)) {
			acpi_dbg("ACPI EC: query timeout (pass %d)\n", pass);
			break;
		}
		acpi_dbg("ACPI EC: query returned 0x%02x\n", qval);
		if (qval == 0) {
			if (pass)
				acpi_dbg(
					"ACPI EC: SCI_EVT cleared after %d queries\n",
					pass);
			break; // No event pending
		}

		// Evaluate _Qxx method on EC device
		ec_dispatch_event(qval);

		// Check i8042 after each event
		uint8_t ps2 = inb(0x64);
		if (ps2 != 0xFF) {
			acpi_dbg(
				"ACPI EC: i8042 ALIVE after event 0x%02x! status=0x%02x\n",
				qval, ps2);
			return;
		}
	}
}

void acpi_service_events(void)
{
	struct workqueue_struct *q = __atomic_load_n(&ec_wq, __ATOMIC_ACQUIRE);

	if (!q) {
		ec_process_pending_events();
		return;
	}
	// The queues run the events: the SCI handler's GPE methods, the
	// EC's queries.
	extern void acpi_poll_events(void);
	acpi_poll_events();
	if (ec_initialized && ec_cmd_port && (inb(ec_cmd_port) & EC_FLAG_SCI))
		(void)queue_work(q, &ec_event_work);
}

void acpi_late_init(void)
{
	extern int acpi_os_initialize1(void);
	struct workqueue_struct *ev, *qr;

	(void)acpi_os_initialize1();
	if (!ec_initialized)
		return;
	INIT_WORK(&ec_event_work, ec_event_work_fn);
	ev = alloc_ordered_workqueue("kec", 0);
	qr = alloc_ordered_workqueue("kec_query", 0);
	if (!ev || !qr) {
		kprintf("ACPI EC: no work queues, events are not serviced\n");
		if (ev)
			destroy_workqueue(ev);
		if (qr)
			destroy_workqueue(qr);
		return;
	}
	__atomic_store_n(&ec_query_wq, qr, __ATOMIC_RELEASE);
	__atomic_store_n(&ec_wq, ev, __ATOMIC_RELEASE);
}

int acpi_ec_eval_qxx(uint8_t qval)
{
	if (!ec_device_handle)
		return -1;

	char method[5];
	method[0] = '_';
	method[1] = 'Q';
	method[2] = "0123456789ABCDEF"[(qval >> 4) & 0xF];
	method[3] = "0123456789ABCDEF"[qval & 0xF];
	method[4] = '\0';

	ACPI_STATUS status =
		AcpiEvaluateObject(ec_device_handle, method, NULL, NULL);
	if (ACPI_FAILURE(status))
		return -1;
	return 0;
}

// Evaluate _STA on a device path and return the status integer.
static uint32_t acpi_eval_sta(const char *path)
{
	ACPI_HANDLE handle;
	if (ACPI_FAILURE(AcpiGetHandle(NULL, (char *)path, &handle)))
		return 0;

	ACPI_OBJECT obj;
	ACPI_BUFFER buf = { sizeof(obj), &obj };
	ACPI_STATUS status = AcpiEvaluateObject(handle, "_STA", NULL, &buf);
	if (ACPI_FAILURE(status))
		return 0xFFFFFFFF; // no _STA means "present and functioning"
	if (obj.Type != ACPI_TYPE_INTEGER)
		return 0xFFFFFFFF;
	return (uint32_t)obj.Integer.Value;
}

// ============================================================================
// ACPI Initialization (ACPICA-based)
// ============================================================================

int acpi_init(uint64_t rsdp_hint)
{
	ACPI_STATUS status;

	acpi_dbg("ACPI: Initializing via ACPICA...\n");

	// Pass RSDP address to OSL
	acpica_set_rsdp(rsdp_hint);

	g_acpi_info.rsdp_found = true;
	g_acpi_info.rsdp_phys_addr = rsdp_hint;

	// Detect ACPI revision from RSDP
	if (rsdp_hint) {
		acpi_rsdp_t *rsdp = (acpi_rsdp_t *)phys_to_virt(rsdp_hint);
		g_acpi_info.acpi_revision = rsdp->revision;
	}

	// Initialize ACPICA subsystem
	status = AcpiInitializeSubsystem();
	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI: AcpiInitializeSubsystem failed: %d\n",
			 (int)status);
		return -1;
	}

	// Initialize table manager and load RSDP/RSDT/XSDT
	status = AcpiInitializeTables(NULL, 32, FALSE);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI: AcpiInitializeTables failed: %d\n",
			 (int)status);
		return -1;
	}

	// Load all ACPI tables (DSDT, SSDTs, etc.)
	status = AcpiLoadTables();
	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI: AcpiLoadTables failed: %d\n", (int)status);
		return -1;
	}

	// Initialize EC address space handler BEFORE AcpiEnableSubsystem.
	// Required so AML _INI/_SRS methods can access EC OpRegions.
	acpi_ec_init();

	// Install exception handler to catch AML errors
	AcpiInstallExceptionHandler(acpi_exception_handler);

	// Install stub GPIO handler — Dell DSDTs use GPIO OpRegions for
	// keyboard/touchpad enable.  Without a handler, AML methods that
	// access GPIO silently abort.
	status = AcpiInstallAddressSpaceHandler(ACPI_ROOT_OBJECT,
						ACPI_ADR_SPACE_GPIO,
						gpio_space_handler, NULL,
						&g_gpio_conn_info);
	if (ACPI_FAILURE(status))
		acpi_dbg("ACPI: GPIO handler install failed (%d)\n",
			 (int)status);

	// Enable ACPI subsystem
	status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI: AcpiEnableSubsystem failed: %d\n", (int)status);
		// Non-fatal — continue without hardware enable
	} else {
		acpi_dbg("ACPI: EnableSub ok\n");
	}

	// Call _REG for EC BEFORE InitializeObjects.
	// Sequence: EnableSubsystem → _REG(EC) → InitializeObjects.
	// _INI methods (run during InitializeObjects) need EC OpRegion access
	// to configure KBC emulation on 0x60/0x64.  Without _REG first,
	// EC OpRegion writes silently fail and KBC stays disabled.
	acpi_ec_call_reg();

	// Initialize ACPI objects (run _INI methods, etc.)
	// _INI methods can now access EC because _REG was called above.
	status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("ACPI: InitObjects failed: %d\n", (int)status);
	} else {
		acpi_dbg("ACPI: InitObjects ok\n");
	}

	// Tell firmware we are using IOAPIC mode (not legacy PIC).
	// This sets the DSDT variable PICM=1 so that _PRT methods
	// return hardwired IOAPIC GSIs instead of PIC link devices.
	{
		ACPI_OBJECT_LIST pic_args;
		ACPI_OBJECT pic_arg;
		pic_arg.Type = ACPI_TYPE_INTEGER;
		pic_arg.Integer.Value = 1; // 1 = APIC/IOAPIC mode
		pic_args.Count = 1;
		pic_args.Pointer = &pic_arg;
		ACPI_STATUS ps =
			AcpiEvaluateObject(NULL, "\\_PIC", &pic_args, NULL);
		if (ACPI_SUCCESS(ps)) {
			kprintf("ACPI: _PIC(1) -> IOAPIC mode selected\n");
		} else {
			acpi_dbg(
				"ACPI: _PIC not found (status %d), assuming hardwired\n",
				(int)ps);
		}
	}

	// Probe i8042 again after _INI methods have run — they may have
	// told the EC to enable KBC emulation on 0x60/0x64.
	{
		uint8_t ps2_sts = inb(0x64);
		acpi_dbg("ACPI: post-InitObjects i8042 status=0x%02x%s\n",
			 ps2_sts,
			 ps2_sts == 0xFF ? " (still dead)" : " (alive!)");
	}

	// Evaluate _STA on key devices for diagnostics
	{
		uint32_t sta;
		sta = acpi_eval_sta("\\_SB_.PC00.LPCB.ECDV");
		acpi_dbg("ACPI: ECDV _STA=0x%x\n", sta);
		sta = acpi_eval_sta("\\_SB_.PC00.LPCB.PS2K");
		acpi_dbg("ACPI: PS2K _STA=0x%x\n", sta);
		sta = acpi_eval_sta("\\_SB_.PC00.LPCB.PS2M");
		acpi_dbg("ACPI: PS2M _STA=0x%x\n", sta);
	}

	// Set up EC GPE: install handler and enable GPE bit in hardware.
	// The EC firmware checks the host's GPE enable register to know
	// that the OS EC driver is ready.  Only then does it activate
	// KBC emulation on ports 0x60/0x64.
	ec_setup_gpe();

	// Finalize all GPE handlers.
	// after installing early GPE handlers.
	AcpiUpdateAllGpes();

	// Process pending EC events (manual query fallback)
	ec_process_pending_events();

	// Give EC firmware time to react to GPE enable + event processing
	for (volatile int d = 0; d < 1000000; d++)
		;

	// Check if i8042 came alive after GPE setup + event processing
	{
		uint8_t ps2_sts = inb(0x64);
		acpi_dbg("ACPI: post-GPE i8042 status=0x%02x%s\n", ps2_sts,
			 ps2_sts == 0xFF ? " (still dead)" : " (ALIVE!)");
	}

	// Also probe wider range of legacy IO to understand which decode works
	{
		acpi_dbg("ACPI: IO decode test: ");
		acpi_dbg("0x61=%02x ", inb(0x61)); // NMI status (PCH internal)
		acpi_dbg("0x71=%02x ", inb(0x71)); // RTC data
		acpi_dbg("0x62=%02x ", inb(0x62)); // EC data (ME1 decode)
		acpi_dbg("0x66=%02x ", inb(0x66)); // EC status (ME1 decode)
		acpi_dbg("0x2E=%02x ", inb(0x2E)); // SuperIO (SE decode)
		acpi_dbg("0x60=%02x ", inb(0x60)); // KBC data
		acpi_dbg("0x64=%02x\n", inb(0x64)); // KBC status
	}

	// The namespace is usable from here on (acpi_dev.c).
	__atomic_store_n(&g_acpi_namespace_ready, 1, __ATOMIC_RELEASE);

	// Parse MADT for CPU/IOAPIC information
	acpi_parse_madt();

	// Determine BSP
	if (g_acpi_info.cpu_count > 0) {
		g_acpi_info.cpus[0].bsp = true;
		g_acpi_info.cpus[0].started = true;
		g_acpi_info.bsp_apic_id = g_acpi_info.cpus[0].apic_id;
	}

	acpi_print_info();
	return 0;
}

// ============================================================================
// Public API — Getters
// ============================================================================

acpi_info_t *acpi_get_info(void)
{
	return &g_acpi_info;
}

uint32_t acpi_get_cpu_count(void)
{
	return g_acpi_info.cpu_count;
}

cpu_info_t *acpi_get_cpu(uint32_t index)
{
	if (index >= g_acpi_info.cpu_count)
		return NULL;
	return &g_acpi_info.cpus[index];
}

uint32_t acpi_get_bsp_apic_id(void)
{
	return g_acpi_info.bsp_apic_id;
}

uint64_t acpi_get_lapic_address(void)
{
	return g_acpi_info.lapic_address;
}

ioapic_info_t *acpi_get_ioapic(uint32_t index)
{
	if (index >= g_acpi_info.ioapic_count)
		return NULL;
	return &g_acpi_info.ioapics[index];
}

irq_override_t *acpi_get_irq_override(uint8_t isa_irq)
{
	for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
		if (g_acpi_info.irq_overrides[i].bus_irq == isa_irq)
			return &g_acpi_info.irq_overrides[i];
	}
	return NULL;
}

uint32_t acpi_irq_to_gsi(uint8_t isa_irq)
{
	irq_override_t *override = acpi_get_irq_override(isa_irq);
	if (override)
		return override->gsi;
	return isa_irq;
}

void acpi_print_info(void)
{
	acpi_dbg("ACPI: LAPIC address = 0x%lx\n", g_acpi_info.lapic_address);
	acpi_dbg("ACPI: %u CPU(s) found:\n", g_acpi_info.cpu_count);
	for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
		acpi_dbg("  CPU %u: APIC ID=%u, %s%s%s\n", i,
			 g_acpi_info.cpus[i].apic_id,
			 g_acpi_info.cpus[i].enabled ? "enabled" : "disabled",
			 g_acpi_info.cpus[i].bsp ? ", BSP" : "",
			 g_acpi_info.cpus[i].online_capable ?
				 ", online-capable" :
				 "");
	}
	acpi_dbg("ACPI: %u I/O APIC(s) found:\n", g_acpi_info.ioapic_count);
	for (uint32_t i = 0; i < g_acpi_info.ioapic_count; i++) {
		acpi_dbg("  I/O APIC %u: ID=%u, addr=0x%x, GSI base=%u\n", i,
			 g_acpi_info.ioapics[i].id,
			 g_acpi_info.ioapics[i].address,
			 g_acpi_info.ioapics[i].gsi_base);
	}
	if (g_acpi_info.irq_override_count > 0) {
		acpi_dbg("ACPI: %u IRQ override(s):\n",
			 g_acpi_info.irq_override_count);
		for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
			acpi_dbg("  IRQ %u -> GSI %u (pol=%u, trig=%u)\n",
				 g_acpi_info.irq_overrides[i].bus_irq,
				 g_acpi_info.irq_overrides[i].gsi,
				 g_acpi_info.irq_overrides[i].polarity,
				 g_acpi_info.irq_overrides[i].trigger_mode);
		}
	}
	if (g_acpi_info.dual_8259_present)
		acpi_dbg("ACPI: PC-AT compatible dual-8259 present\n");
}

// ============================================================================
// ACPI Table Access — delegates to ACPICA
// ============================================================================

acpi_sdt_header_t *acpi_find_table(const char *signature)
{
	return acpi_find_table_index(signature, 0);
}

acpi_sdt_header_t *acpi_find_table_index(const char *signature, uint32_t index)
{
	ACPI_TABLE_HEADER *hdr = NULL;
	ACPI_STATUS status = AcpiGetTable((char *)signature, index + 1, &hdr);
	if (ACPI_FAILURE(status) || !hdr)
		return NULL;
	return (acpi_sdt_header_t *)hdr;
}

uint32_t acpi_get_table_count(const char *signature)
{
	uint32_t count = 0;
	while (acpi_find_table_index(signature, count) != NULL)
		count++;
	return count;
}

int acpi_get_table_aml(const char *signature, uint32_t index,
		       const uint8_t **aml, uint32_t *aml_len)
{
	if (!aml || !aml_len)
		return -1;
	acpi_sdt_header_t *table = acpi_find_table_index(signature, index);
	if (!table)
		return -1;
	*aml = (const uint8_t *)table + sizeof(acpi_sdt_header_t);
	*aml_len = table->length - sizeof(acpi_sdt_header_t);
	return 0;
}

// ============================================================================
// AML Value helpers (stub implementations for API compat)
// ============================================================================

void aml_bump_init(aml_bump_alloc_t *alloc)
{
	if (!alloc)
		return;
	alloc->pool = (uint8_t *)kalloc(AML_BUMP_POOL_SIZE);
	alloc->used = 0;
	alloc->capacity = alloc->pool ? AML_BUMP_POOL_SIZE : 0;
}

void aml_bump_destroy(aml_bump_alloc_t *alloc)
{
	if (alloc && alloc->pool) {
		kfree(alloc->pool);
		alloc->pool = NULL;
	}
}

void *aml_bump_alloc(aml_bump_alloc_t *alloc, uint32_t size)
{
	if (!alloc || !alloc->pool || alloc->used + size > alloc->capacity)
		return NULL;
	void *p = alloc->pool + alloc->used;
	alloc->used += size;
	return p;
}

void aml_value_copy(aml_value_t *dst, const aml_value_t *src,
		    aml_bump_alloc_t *alloc)
{
	(void)alloc;
	if (dst && src)
		my_memcpy(dst, src, sizeof(*dst));
}

// ============================================================================
// AML Device Discovery — via AcpiGetDevices
// ============================================================================

typedef struct {
	const char *hid;
	acpi_aml_device_info_t *out;
	int max_out;
	int count;
} find_devices_ctx_t;

static ACPI_STATUS find_devices_callback(ACPI_HANDLE Object,
					 UINT32 NestingLevel, void *Context,
					 void **ReturnValue)
{
	find_devices_ctx_t *ctx = (find_devices_ctx_t *)Context;
	ACPI_DEVICE_INFO *info = NULL;
	ACPI_STATUS status;
	ACPI_BUFFER path_buf = { ACPI_ALLOCATE_BUFFER, NULL };
	(void)NestingLevel;
	(void)ReturnValue;

	if (ctx->count >= ctx->max_out)
		return AE_CTRL_TERMINATE;

	status = AcpiGetObjectInfo(Object, &info);
	if (ACPI_FAILURE(status) || !info)
		return AE_OK;

	status = AcpiGetName(Object, ACPI_FULL_PATHNAME, &path_buf);
	if (ACPI_FAILURE(status)) {
		AcpiOsFree(info);
		return AE_OK;
	}

	acpi_aml_device_info_t *dev = &ctx->out[ctx->count];
	my_memset(dev, 0, sizeof(*dev));

	// Copy path
	my_strncpy(dev->path, (char *)path_buf.Pointer, ACPI_AML_MAX_PATH);

	// Copy HID
	if (info->Valid & ACPI_VALID_HID)
		my_strncpy(dev->hid, info->HardwareId.String, ACPI_AML_MAX_HID);

	// Copy CID (first compatible ID)
	if (info->Valid & ACPI_VALID_CID && info->CompatibleIdList.Count > 0)
		my_strncpy(dev->cid, info->CompatibleIdList.Ids[0].String,
			   ACPI_AML_MAX_HID);

	// Check for control methods
	ACPI_HANDLE tmp;
	dev->has_ps0 = (AcpiGetHandle(Object, "_PS0", &tmp) == AE_OK) ? 1 : 0;
	dev->has_ps3 = (AcpiGetHandle(Object, "_PS3", &tmp) == AE_OK) ? 1 : 0;
	dev->has_sta = (AcpiGetHandle(Object, "_STA", &tmp) == AE_OK) ? 1 : 0;
	dev->has_crs = (AcpiGetHandle(Object, "_CRS", &tmp) == AE_OK) ? 1 : 0;
	dev->has_dep = (AcpiGetHandle(Object, "_DEP", &tmp) == AE_OK) ? 1 : 0;

	AcpiOsFree(path_buf.Pointer);
	AcpiOsFree(info);

	ctx->count++;
	return AE_OK;
}

int acpi_aml_find_devices_by_hid(const char *hid, acpi_aml_device_info_t *out,
				 int max_out)
{
	find_devices_ctx_t ctx;
	if (!out || max_out <= 0)
		return 0;

	ctx.hid = hid;
	ctx.out = out;
	ctx.max_out = max_out;
	ctx.count = 0;

	AcpiGetDevices((char *)hid, find_devices_callback, &ctx, NULL);
	return ctx.count;
}

// ============================================================================
// PNP Device Activation via _SRS (Set Resource Settings)
// Reads the _CRS buffer and passes it to _SRS to activate the device.
// This triggers firmware AML that enables the device in the EC/PCH.
// ============================================================================

int acpi_aml_activate_dev(const char *device_path)
{
	ACPI_HANDLE handle;
	ACPI_STATUS status;
	ACPI_BUFFER crs_buf = { ACPI_ALLOCATE_BUFFER, NULL };

	if (!device_path)
		return -1;

	status = AcpiGetHandle(NULL, (char *)device_path, &handle);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("[SRS] %s: device not found\n", device_path);
		return -1;
	}

	// Read _CRS raw buffer
	status = AcpiGetCurrentResources(handle, &crs_buf);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("[SRS] %s: _CRS failed (%d)\n", device_path,
			 (int)status);
		return -1;
	}

	// Pass it to _SRS to activate the device
	status = AcpiSetCurrentResources(handle, &crs_buf);
	AcpiOsFree(crs_buf.Pointer);

	if (ACPI_FAILURE(status)) {
		acpi_dbg("[SRS] %s: _SRS failed (%d)\n", device_path,
			 (int)status);
		return -1;
	}

	acpi_dbg("[SRS] %s: activated via _SRS\n", device_path);
	return 0;
}

// ============================================================================
// ACPI Power Management — S5 (poweroff) and reset
// ============================================================================

static uint32_t g_pm1a_cnt_blk = 0;
static uint32_t g_pm1b_cnt_blk = 0;
static ACPI_GENERIC_ADDRESS g_pm1a_cnt_gas = { 0 };
static ACPI_GENERIC_ADDRESS g_pm1b_cnt_gas = { 0 };
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;
static uint32_t g_smi_cmd_port = 0;
static uint8_t g_acpi_enable = 0;
static uint8_t g_pm1_cnt_len = 0;
static uint8_t g_reset_reg_space = 0;
static uint64_t g_reset_reg_addr = 0;
static uint8_t g_reset_value = 0;
static int g_pm_initialized = 0;

static inline void acpi_outw(uint16_t port, uint16_t val)
{
	__asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}

static inline void acpi_outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}

static inline void acpi_outl(uint16_t port, uint32_t val)
{
	__asm__ volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint8_t acpi_inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline uint16_t acpi_inw(uint16_t port)
{
	uint16_t val;
	__asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline uint32_t acpi_inl(uint16_t port)
{
	uint32_t val;
	__asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static void acpi_copy_gas(ACPI_GENERIC_ADDRESS *dst, const uint8_t raw[12])
{
	my_memset(dst, 0, sizeof(*dst));
	my_memcpy(dst, raw, sizeof(*dst));
}

static int acpi_gas_is_usable(const ACPI_GENERIC_ADDRESS *gas)
{
	return gas && gas->Address != 0;
}

static uint8_t acpi_gas_access_bits(const ACPI_GENERIC_ADDRESS *gas,
				    uint8_t legacy_len)
{
	if (gas) {
		switch (gas->AccessWidth) {
		case 1:
			return 8;
		case 2:
			return 16;
		case 3:
			return 32;
		case 4:
			return 64;
		default:
			break;
		}
		if (gas->BitWidth)
			return gas->BitWidth;
	}
	if (legacy_len == 1)
		return 8;
	if (legacy_len >= 4)
		return 32;
	return 16;
}

static uint64_t acpi_gas_map_and_get_ptr(const ACPI_GENERIC_ADDRESS *gas,
					 uint8_t access_bits,
					 volatile uint8_t **out_ptr)
{
	uint64_t page_base = PAGE_ALIGN_DOWN(gas->Address);
	uint64_t offset = gas->Address - page_base;
	size_t width_bytes = (access_bits <= 8)  ? 1 :
			     (access_bits <= 16) ? 2 :
			     (access_bits <= 32) ? 4 :
						   8;
	size_t num_pages = PAGE_ALIGN(offset + width_bytes) / PAGE_SIZE;
	uint64_t mapped = mm_map_device_mmio(page_base, num_pages);
	if (!mapped) {
		*out_ptr = NULL;
		return 0;
	}
	*out_ptr = (volatile uint8_t *)(mapped + offset);
	return mapped;
}

static uint64_t acpi_gas_read(const ACPI_GENERIC_ADDRESS *gas,
			      uint8_t legacy_len)
{
	if (!acpi_gas_is_usable(gas))
		return 0;

	uint8_t access_bits = acpi_gas_access_bits(gas, legacy_len);
	if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO) {
		uint16_t port = (uint16_t)gas->Address;
		if (access_bits <= 8)
			return acpi_inb(port);
		if (access_bits <= 16)
			return acpi_inw(port);
		return acpi_inl(port);
	}

	if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
		volatile uint8_t *ptr;
		if (!acpi_gas_map_and_get_ptr(gas, access_bits, &ptr) || !ptr)
			return 0;
		if (access_bits <= 8)
			return *(volatile uint8_t *)ptr;
		if (access_bits <= 16)
			return *(volatile uint16_t *)ptr;
		if (access_bits <= 32)
			return *(volatile uint32_t *)ptr;
		return *(volatile uint64_t *)ptr;
	}

	return 0;
}

static void acpi_gas_write(const ACPI_GENERIC_ADDRESS *gas, uint8_t legacy_len,
			   uint64_t value)
{
	if (!acpi_gas_is_usable(gas))
		return;

	uint8_t access_bits = acpi_gas_access_bits(gas, legacy_len);
	if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_IO) {
		uint16_t port = (uint16_t)gas->Address;
		if (access_bits <= 8) {
			acpi_outb(port, (uint8_t)value);
			(void)acpi_inb(port);
			return;
		}
		if (access_bits <= 16) {
			acpi_outw(port, (uint16_t)value);
			(void)acpi_inw(port);
			return;
		}
		acpi_outl(port, (uint32_t)value);
		(void)acpi_inl(port);
		return;
	}

	if (gas->SpaceId == ACPI_ADR_SPACE_SYSTEM_MEMORY) {
		volatile uint8_t *ptr;
		if (!acpi_gas_map_and_get_ptr(gas, access_bits, &ptr) || !ptr)
			return;
		if (access_bits <= 8) {
			*(volatile uint8_t *)ptr = (uint8_t)value;
		} else if (access_bits <= 16) {
			*(volatile uint16_t *)ptr = (uint16_t)value;
		} else if (access_bits <= 32) {
			*(volatile uint32_t *)ptr = (uint32_t)value;
		} else {
			*(volatile uint64_t *)ptr = (uint64_t)value;
		}
		__asm__ volatile("mfence" ::: "memory");
		(void)acpi_gas_read(gas, legacy_len);
	}
}

static uint64_t acpi_build_sleep_control(const ACPI_GENERIC_ADDRESS *gas,
					 uint8_t legacy_len, uint8_t sleep_type,
					 int set_sleep_enable)
{
	uint64_t current = acpi_gas_read(gas, legacy_len);
	uint64_t value = current & ~((uint64_t)0x1C00u | ACPI_PM1_SLP_EN);
	value |= ACPI_PM1_SLP_TYP(sleep_type);
	if (set_sleep_enable)
		value |= ACPI_PM1_SLP_EN;
	return value;
}

static void acpi_enable_via_fadt(void)
{
	if (!g_smi_cmd_port || !g_acpi_enable)
		return;

	acpi_outb((uint16_t)g_smi_cmd_port, g_acpi_enable);
	for (int i = 0; i < 1000000; i++) {
		if (g_pm1a_cnt_blk &&
		    (acpi_inw((uint16_t)g_pm1a_cnt_blk) & 0x0001))
			break;
		__asm__ volatile("pause");
	}
}

static void acpi_ensure_enabled(void)
{
	UINT32 sci_enabled = 0;
	ACPI_STATUS st =
		AcpiReadBitRegister(ACPI_BITREG_SCI_ENABLE, &sci_enabled);
	if (ACPI_SUCCESS(st) && sci_enabled)
		return;

	st = AcpiEnable();
	if (ACPI_SUCCESS(st))
		return;

	acpi_dbg("ACPI PM: AcpiEnable failed (%d), using FADT enable path\n",
		 (int)st);
	acpi_enable_via_fadt();
}

static int acpi_parse_s5(uint8_t *aml, uint32_t length)
{
	for (uint32_t i = 0; i + 4 < length; i++) {
		if (aml[i] == '_' && aml[i + 1] == 'S' && aml[i + 2] == '5' &&
		    aml[i + 3] == '_') {
			for (uint32_t j = i + 4; j < i + 32 && j + 5 < length;
			     j++) {
				if (aml[j] == 0x12) {
					uint32_t k = j + 1;
					uint8_t pkg_lead = aml[k];
					if (pkg_lead & 0xC0) {
						int extra = (pkg_lead >> 6) & 3;
						k += 1 + extra;
					} else {
						k += 1;
					}
					k++;
					if (k < length) {
						if (aml[k] == 0x0A &&
						    k + 1 < length) {
							g_slp_typa = aml[k + 1];
							k += 2;
						} else {
							g_slp_typa = aml[k];
							k += 1;
						}
					}
					if (k < length) {
						if (aml[k] == 0x0A &&
						    k + 1 < length) {
							g_slp_typb = aml[k + 1];
							k += 2;
						} else {
							g_slp_typb = aml[k];
							k += 1;
						}
					}
					return 1;
				}
			}
		}
	}
	return 0;
}

void acpi_pm_init(void)
{
	acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table(ACPI_SIG_FADT);
	if (!fadt) {
		acpi_dbg(
			"ACPI PM: FADT not found, power management unavailable\n");
		return;
	}

	g_pm1a_cnt_blk = fadt->pm1a_control_block;
	g_pm1b_cnt_blk = fadt->pm1b_control_block;
	g_smi_cmd_port = fadt->smi_command_port;
	g_acpi_enable = fadt->acpi_enable;
	g_pm1_cnt_len = fadt->pm1_control_length;

	if (fadt->header.length >= 196)
		acpi_copy_gas(&g_pm1a_cnt_gas, fadt->x_pm1a_control_block);
	if (fadt->header.length >= 208)
		acpi_copy_gas(&g_pm1b_cnt_gas, fadt->x_pm1b_control_block);

	if (!acpi_gas_is_usable(&g_pm1a_cnt_gas) && g_pm1a_cnt_blk) {
		g_pm1a_cnt_gas.SpaceId = ACPI_ADR_SPACE_SYSTEM_IO;
		g_pm1a_cnt_gas.Address = g_pm1a_cnt_blk;
		g_pm1a_cnt_gas.BitWidth =
			g_pm1_cnt_len ? (uint8_t)(g_pm1_cnt_len * 8) : 16;
	}
	if (!acpi_gas_is_usable(&g_pm1b_cnt_gas) && g_pm1b_cnt_blk) {
		g_pm1b_cnt_gas.SpaceId = ACPI_ADR_SPACE_SYSTEM_IO;
		g_pm1b_cnt_gas.Address = g_pm1b_cnt_blk;
		g_pm1b_cnt_gas.BitWidth =
			g_pm1_cnt_len ? (uint8_t)(g_pm1_cnt_len * 8) : 16;
	}

	if (fadt->header.length >= 129) {
		g_reset_reg_space = fadt->reset_reg_addr_space;
		g_reset_reg_addr = fadt->reset_reg_address;
		g_reset_value = fadt->reset_value;
	}

	ACPI_STATUS st = AcpiGetSleepTypeData(
		ACPI_STATE_S5, (UINT8 *)&g_slp_typa, (UINT8 *)&g_slp_typb);
	if (ACPI_SUCCESS(st)) {
		acpi_dbg(
			"ACPI PM: ACPICA S5 sleep type: SLP_TYPa=%u SLP_TYPb=%u\n",
			g_slp_typa, g_slp_typb);
	} else {
		acpi_dbg("ACPI PM: AcpiGetSleepTypeData(S5) failed: %d\n",
			 (int)st);

		// Use ACPICA to get DSDT for S5 parsing
		uint64_t dsdt_addr = 0;
		if (fadt->header.length >= 148 && fadt->x_dsdt != 0)
			dsdt_addr = fadt->x_dsdt;
		else
			dsdt_addr = fadt->dsdt;

		if (dsdt_addr) {
			acpi_sdt_header_t *dsdt =
				(acpi_sdt_header_t *)phys_to_virt(dsdt_addr);
			uint8_t *aml =
				(uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
			uint32_t aml_len =
				dsdt->length - sizeof(acpi_sdt_header_t);
			if (acpi_parse_s5(aml, aml_len)) {
				acpi_dbg(
					"ACPI PM: fallback S5 sleep type: SLP_TYPa=%u SLP_TYPb=%u\n",
					g_slp_typa, g_slp_typb);
			} else {
				acpi_dbg(
					"ACPI PM: \\_S5 not found in DSDT, using defaults\n");
				g_slp_typa = 5;
				g_slp_typb = 0;
			}
		}
	}

	g_pm_initialized = 1;
	acpi_dbg(
		"ACPI PM: initialized (PM1a=0x%x, PM1b=0x%x, xPM1a=0x%llx, xPM1b=0x%llx, reset=0x%lx)\n",
		g_pm1a_cnt_blk, g_pm1b_cnt_blk,
		(unsigned long long)g_pm1a_cnt_gas.Address,
		(unsigned long long)g_pm1b_cnt_gas.Address,
		(unsigned long)g_reset_reg_addr);
}

// Quiesce all NICs before S5: clear bus-master / wake-enable / interrupt
// enables on every probed PCI network device.  On real hardware (notably
// PCH-LAN integrated I219 LOMs on Lenovo business laptops) the firmware
// will refuse the S5 transition if the NIC still has APME / WUC bits
// asserted or PCI command.MSE | command.BME live, leaving the platform
// in a "screen-off, fans-off, but power-rail-still-hot" half-state from
// which only a manual power-button press recovers.
//
// Implemented as a weak hook here — the network subsystem provides the
// strong override that walks its registered devices.
__attribute__((weak)) void net_quiesce_for_poweroff(void)
{
}

/* kernel/ke/system.c: flush and park the disks, stop the storage
 * controllers; once per boot (a no-op when sys_reboot() did it already).
 * The argument is an enum blk_shutdown_mode (1 = power-off). */
void system_storage_shutdown(int mode);

void acpi_poweroff(void)
{
	acpi_dbg("ACPI: powering off...\n");

	/* The disks first: data in their write caches would be lost with the
	 * power.  Normally sys_reboot() has done this already, with
	 * interrupts on; this covers any other way into the power-off. */
	system_storage_shutdown(1 /* BLK_SHUTDOWN_POWEROFF */);

	// Quiesce all NICs first: stop bus-master DMA, mask interrupts, clear
	// any wake-on-LAN enables.  Without this the platform's S5 path can
	// be blocked by an asserted PME# from the NIC.
	net_quiesce_for_poweroff();

	acpi_ensure_enabled();

	// Run _PTS(5) first so EC / firmware prepare-for-sleep methods still
	// execute on real hardware.  On the P50 this already generates the EC
	// transactions seen in the shutdown log.  The direct PM1 write below is
	// then used as the actual S5 transition so we are not dependent on
	// AcpiEnterSleepState() completing on this platform.
	ACPI_STATUS st = AcpiEnterSleepStatePrep(ACPI_STATE_S5);
	if (ACPI_FAILURE(st)) {
		acpi_dbg("ACPI: AcpiEnterSleepStatePrep(S5) failed: %d\n",
			 (int)st);
	}

	// Known-good S5 path: write SLP_TYP first, then SLP_TYP|SLP_EN.
	// This mirrors ACPICA's legacy PM1 sequence without depending on the
	// rest of AcpiEnterSleepState() to return on this machine.
	if (g_pm_initialized) {
		uint64_t pm1a = 0;
		uint64_t pm1b = 0;
		uint64_t pm1a_before = 0;
		uint64_t pm1b_before = 0;
		UINT32 sci_enabled = 0;
		ACPI_STATUS sci_st = AcpiReadBitRegister(ACPI_BITREG_SCI_ENABLE,
							 &sci_enabled);

		(void)AcpiWriteBitRegister(ACPI_BITREG_WAKE_STATUS,
					   ACPI_CLEAR_STATUS);
		(void)AcpiDisableAllGpes();
		(void)AcpiHwClearAcpiStatus();
		(void)AcpiEnableAllWakeupGpes();

		pm1a_before = acpi_gas_read(&g_pm1a_cnt_gas, g_pm1_cnt_len);
		if (acpi_gas_is_usable(&g_pm1b_cnt_gas))
			pm1b_before =
				acpi_gas_read(&g_pm1b_cnt_gas, g_pm1_cnt_len);

		acpi_dbg(
			"ACPI PM: final S5 handoff SCI_EN=%u (status=%d) PM1a=0x%llx PM1b=0x%llx len=%u SLP_TYPa=%u SLP_TYPb=%u PM1a_before=0x%llx PM1b_before=0x%llx\n",
			(unsigned)sci_enabled, (int)sci_st,
			(unsigned long long)g_pm1a_cnt_gas.Address,
			(unsigned long long)g_pm1b_cnt_gas.Address,
			(unsigned)g_pm1_cnt_len, (unsigned)g_slp_typa,
			(unsigned)g_slp_typb, (unsigned long long)pm1a_before,
			(unsigned long long)pm1b_before);

		__asm__ volatile("cli");

		st = AcpiEnterSleepState(ACPI_STATE_S5);
		if (ACPI_FAILURE(st)) {
			acpi_dbg(
				"ACPI: AcpiEnterSleepState(S5) failed: %d; using direct PM1 fallback\n",
				(int)st);
		} else {
			acpi_dbg(
				"ACPI: AcpiEnterSleepState(S5) returned; using direct PM1 fallback\n");
		}

		pm1a = acpi_build_sleep_control(&g_pm1a_cnt_gas, g_pm1_cnt_len,
						(uint8_t)g_slp_typa, 0);
		if (acpi_gas_is_usable(&g_pm1b_cnt_gas))
			pm1b = acpi_build_sleep_control(&g_pm1b_cnt_gas,
							g_pm1_cnt_len,
							(uint8_t)g_slp_typb, 0);

		acpi_dbg("ACPI PM: S5 write #1 PM1a=0x%llx PM1b=0x%llx\n",
			 (unsigned long long)pm1a, (unsigned long long)pm1b);

		acpi_gas_write(&g_pm1a_cnt_gas, g_pm1_cnt_len, pm1a);
		if (acpi_gas_is_usable(&g_pm1b_cnt_gas))
			acpi_gas_write(&g_pm1b_cnt_gas, g_pm1_cnt_len, pm1b);

		pm1a |= ACPI_PM1_SLP_EN;
		acpi_dbg("ACPI PM: S5 write #2 PM1a=0x%llx\n",
			 (unsigned long long)pm1a);

		acpi_gas_write(&g_pm1a_cnt_gas, g_pm1_cnt_len, pm1a);

		if (acpi_gas_is_usable(&g_pm1b_cnt_gas)) {
			pm1b |= ACPI_PM1_SLP_EN;
			acpi_dbg("ACPI PM: S5 write #2 PM1b=0x%llx\n",
				 (unsigned long long)pm1b);
			acpi_gas_write(&g_pm1b_cnt_gas, g_pm1_cnt_len, pm1b);
		}

		if (g_slp_typa >= 4) {
			acpi_dbg(
				"ACPI PM: still running after S5 write, stalling before retry\n");
			AcpiOsStall(10 * ACPI_USEC_PER_SEC);

			acpi_dbg("ACPI PM: S5 retry write PM1a=0x%llx\n",
				 (unsigned long long)ACPI_PM1_SLP_EN);
			acpi_gas_write(&g_pm1a_cnt_gas, g_pm1_cnt_len,
				       ACPI_PM1_SLP_EN);

			if (acpi_gas_is_usable(&g_pm1b_cnt_gas)) {
				acpi_dbg(
					"ACPI PM: S5 retry write PM1b=0x%llx\n",
					(unsigned long long)ACPI_PM1_SLP_EN);
				acpi_gas_write(&g_pm1b_cnt_gas, g_pm1_cnt_len,
					       ACPI_PM1_SLP_EN);
			}
		}
	}

	// Fallback path #2: QEMU/Bochs ACPI shutdown port.
	acpi_outw(0x604, 0x2000);
	for (;;)
		__asm__ volatile("cli; hlt");
}

// ============================================================================
// PCI Interrupt Routing (_PRT) Lookup
// ============================================================================

int acpi_pci_lookup_irq(const char *bridge_path, uint8_t pci_device,
			uint8_t pci_pin, uint32_t *out_gsi)
{
	ACPI_HANDLE handle;
	ACPI_STATUS status;

	if (!bridge_path || !out_gsi)
		return -1;

	status = AcpiGetHandle(NULL, (char *)bridge_path, &handle);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("[PRT] bridge %s not found (%d)\n", bridge_path,
			 (int)status);
		return -1;
	}

	// Get _PRT routing table
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	status = AcpiGetIrqRoutingTable(handle, &buf);
	if (ACPI_FAILURE(status)) {
		acpi_dbg("[PRT] %s _PRT failed (%d)\n", bridge_path,
			 (int)status);
		return -2;
	}

	// Walk the routing table entries.
	// Address encodes the device number: high word of high dword = device,
	// function is 0xFFFF (any).  Format: (device << 16) | 0xFFFF.
	UINT64 match_addr = ((UINT64)pci_device << 16) | 0xFFFFULL;

	ACPI_PCI_ROUTING_TABLE *entry = (ACPI_PCI_ROUTING_TABLE *)buf.Pointer;
	int found = 0;

	while (entry && entry->Length > 0) {
		if (entry->Address == match_addr && entry->Pin == pci_pin) {
			if (entry->Source[0] == '\0') {
				// Hardwired GSI (no link device) — most common on modern PCH
				*out_gsi = entry->SourceIndex;
				found = 1;
			} else {
				// Link device (e.g. LNKA) — evaluate _CRS to get GSI
				ACPI_HANDLE link = NULL;
				if (!ACPI_SUCCESS(AcpiGetHandle(
					    handle, entry->Source, &link)))
					AcpiGetHandle(NULL, entry->Source,
						      &link);

				if (link) {
					ACPI_BUFFER lbuf = {
						ACPI_ALLOCATE_BUFFER, NULL
					};
					if (ACPI_SUCCESS(
						    AcpiGetCurrentResources(
							    link, &lbuf))) {
						ACPI_RESOURCE *res =
							(ACPI_RESOURCE *)
								lbuf.Pointer;
						while (res &&
						       res->Type !=
							       ACPI_RESOURCE_TYPE_END_TAG) {
							if (res->Type ==
								    ACPI_RESOURCE_TYPE_IRQ &&
							    res->Data.Irq.InterruptCount >
								    0) {
								*out_gsi =
									res->Data
										.Irq
										.Interrupts
											[0];
								found = 1;
								break;
							}
							if (res->Type ==
								    ACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
							    res->Data.ExtendedIrq
									    .InterruptCount >
								    0) {
								*out_gsi =
									res->Data
										.ExtendedIrq
										.Interrupts
											[0];
								found = 1;
								break;
							}
							res = ACPI_NEXT_RESOURCE(
								res);
						}
						AcpiOsFree(lbuf.Pointer);
					}

					// If _CRS returned 0 (unconfigured link), try _PRS + _SRS
					// to activate the link device with a valid IRQ
					if (found && *out_gsi == 0) {
						found = 0; // reject GSI 0 (PIT timer)
						ACPI_BUFFER pbuf = {
							ACPI_ALLOCATE_BUFFER,
							NULL
						};
						if (ACPI_SUCCESS(
							    AcpiGetPossibleResources(
								    link,
								    &pbuf))) {
							ACPI_RESOURCE *pres =
								(ACPI_RESOURCE
									 *)pbuf
									.Pointer;
							uint32_t chosen_irq = 0;
							while (pres &&
							       pres->Type !=
								       ACPI_RESOURCE_TYPE_END_TAG) {
								if (pres->Type ==
									    ACPI_RESOURCE_TYPE_IRQ &&
								    pres->Data.Irq.InterruptCount >
									    0) {
									// Pick first IRQ >= 1 from the possible list
									for (int k = 0;
									     k <
									     pres->Data
										     .Irq
										     .InterruptCount;
									     k++) {
										if (pres->Data
											    .Irq
											    .Interrupts
												    [k] >=
										    1) {
											chosen_irq =
												pres->Data
													.Irq
													.Interrupts
														[k];
											break;
										}
									}
									break;
								}
								if (pres->Type ==
									    ACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
								    pres->Data.ExtendedIrq
										    .InterruptCount >
									    0) {
									for (int k = 0;
									     k <
									     (int)pres
										     ->Data
										     .ExtendedIrq
										     .InterruptCount;
									     k++) {
										if (pres->Data
											    .ExtendedIrq
											    .Interrupts
												    [k] >=
										    1) {
											chosen_irq =
												pres->Data
													.ExtendedIrq
													.Interrupts
														[k];
											break;
										}
									}
									break;
								}
								pres = ACPI_NEXT_RESOURCE(
									pres);
							}

							if (chosen_irq > 0) {
								// Build a resource buffer to program via _SRS:
								// IRQ descriptor + end tag, in storage typed
								// (and sized) as resources
								ACPI_RESOURCE srs_res[2];
								uint8_t *srs_buf =
									(uint8_t *)srs_res;
								ACPI_RESOURCE *sr =
									(ACPI_RESOURCE
										 *)
										srs_buf;
								sr->Type =
									ACPI_RESOURCE_TYPE_IRQ;
								sr->Length =
									(UINT16)(sizeof(ACPI_RESOURCE) -
										 sizeof(sr->Data) +
										 sizeof(sr->Data.Irq));
								sr->Data.Irq
									.InterruptCount =
									1;
								sr->Data.Irq
									.Triggering =
									ACPI_LEVEL_SENSITIVE;
								sr->Data.Irq
									.Polarity =
									ACPI_ACTIVE_LOW;
								sr->Data.Irq
									.Shareable =
									ACPI_SHARED;
								sr->Data.Irq.Interrupts
									[0] = (UINT8)
									chosen_irq;
								sr->Data.Irq
									.DescriptorLength =
									2;
								// End tag
								ACPI_RESOURCE *et =
									ACPI_NEXT_RESOURCE(
										sr);
								et->Type =
									ACPI_RESOURCE_TYPE_END_TAG;
								et->Length =
									(UINT16)(sizeof(ACPI_RESOURCE) -
										 sizeof(et->Data) +
										 sizeof(et->Data.EndTag));
								et->Data.EndTag
									.Checksum =
									0;

								ACPI_BUFFER
									sbuf;
								sbuf.Pointer =
									srs_buf;
								sbuf.Length =
									(char *)ACPI_NEXT_RESOURCE(
										et) -
									(char *)srs_buf;

								if (ACPI_SUCCESS(AcpiSetCurrentResources(
									    link,
									    &sbuf))) {
									*out_gsi =
										chosen_irq;
									found = 1;
									acpi_dbg(
										"[PRT] Activated link '%s' -> IRQ %u\n",
										entry->Source,
										chosen_irq);
								}
							}
							AcpiOsFree(
								pbuf.Pointer);
						}
					}
				}
			}
			break;
		}
		entry = (ACPI_PCI_ROUTING_TABLE *)((char *)entry +
						   entry->Length);
	}

	AcpiOsFree(buf.Pointer);

	if (found) {
		acpi_dbg("[PRT] %s dev=%u pin=%u -> GSI %u\n", bridge_path,
			 pci_device, pci_pin, *out_gsi);
		return 0;
	}

	acpi_dbg("[PRT] %s dev=%u pin=%u: no matching entry\n", bridge_path,
		 pci_device, pci_pin);
	return -3;
}

void acpi_reset(void)
{
	acpi_dbg("ACPI: resetting system...\n");
	__asm__ volatile("cli");

	if (g_reset_reg_addr != 0) {
		if (g_reset_reg_space == 1)
			acpi_outb((uint16_t)g_reset_reg_addr, g_reset_value);
		for (volatile int i = 0; i < 1000000; i++)
			;
	}

	// Keyboard controller reset
	uint8_t good = 0x02;
	while (good & 0x02)
		__asm__ volatile("inb $0x64, %0" : "=a"(good));
	__asm__ volatile("outb %0, $0x64" ::"a"((uint8_t)0xFE));

	for (volatile int i = 0; i < 1000000; i++)
		;

	// Triple fault
	struct {
		uint16_t limit;
		uint64_t base;
	} __attribute__((packed)) null_idt = { 0, 0 };
	__asm__ volatile("lidt %0; int $3" ::"m"(null_idt));

	for (;;)
		__asm__ volatile("hlt");
}

/* Read ACPI PM Timer (3.579545 MHz, chipset-based global counter).
 * Returns the 32-bit (or 24-bit) free-running counter value.
 * Returns 0 if PM Timer is not available. */
uint32_t acpi_read_pmtimer(void)
{
	uint32_t ticks = 0;
	ACPI_STATUS status = AcpiGetTimer(&ticks);
	if (ACPI_FAILURE(status))
		return 0;
	return ticks;
}
