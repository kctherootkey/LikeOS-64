// LikeOS-64 - ACPI Implementation using ACPICA Reference Implementation
// RSDP, RSDT/XSDT, and MADT parsing for CPU enumeration
// AML evaluation delegated to ACPICA

// ACPICA headers (must be included before kernel acpi.h to avoid macro conflicts)
#include "acpica/include/acpi.h"

#include "../../include/kernel/acpi.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/memory.h"

#define ACPI_DEBUG 0
#if ACPI_DEBUG
#define acpi_dbg(fmt, ...) kprintf(fmt, ##__VA_ARGS__)
#else
#define acpi_dbg(fmt, ...) do {} while(0)
#endif

// ============================================================================
// Global ACPI State
// ============================================================================

static acpi_info_t g_acpi_info = {0};

// ACPICA RSDP address setter (defined in oslikeos.c)
extern void acpica_set_rsdp(uint64_t phys_addr);

// ============================================================================
// Helper Functions
// ============================================================================

static void my_memset(void* dst, int val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void my_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static int my_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void my_strncpy(char* dst, const char* src, int n) {
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ============================================================================
// MADT Parsing (raw table walk - not AML, kept from original)
// ============================================================================

static void acpi_parse_madt(void) {
    ACPI_TABLE_HEADER *hdr = NULL;
    ACPI_STATUS status = AcpiGetTable(ACPI_SIG_MADT, 1, &hdr);
    if (ACPI_FAILURE(status) || !hdr) {
        kprintf("ACPI: MADT not found\n");
        return;
    }

    acpi_madt_t* madt = (acpi_madt_t*)hdr;
    acpi_dbg("ACPI: MADT found, length=%u\n", madt->header.length);

    g_acpi_info.lapic_address = madt->lapic_address;
    g_acpi_info.dual_8259_present = (madt->flags & 1) != 0;

    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        if (entry->length == 0) break;

        switch (entry->type) {
        case MADT_TYPE_LAPIC: {
            madt_lapic_t* lapic = (madt_lapic_t*)entry;
            if (g_acpi_info.cpu_count < MAX_CPUS) {
                cpu_info_t* cpu = &g_acpi_info.cpus[g_acpi_info.cpu_count];
                cpu->apic_id = lapic->apic_id;
                cpu->acpi_processor_id = lapic->acpi_processor_id;
                cpu->enabled = (lapic->flags & MADT_LAPIC_ENABLED) != 0;
                cpu->online_capable = (lapic->flags & MADT_LAPIC_ONLINE_CAPABLE) != 0;
                cpu->bsp = false;
                cpu->started = false;
                if (cpu->enabled || cpu->online_capable)
                    g_acpi_info.cpu_count++;
            }
            break;
        }
        case MADT_TYPE_IOAPIC: {
            madt_ioapic_t* ioapic = (madt_ioapic_t*)entry;
            if (g_acpi_info.ioapic_count < MAX_IOAPICS) {
                ioapic_info_t* info = &g_acpi_info.ioapics[g_acpi_info.ioapic_count];
                info->id = ioapic->ioapic_id;
                info->address = ioapic->ioapic_address;
                info->gsi_base = ioapic->gsi_base;
                g_acpi_info.ioapic_count++;
            }
            break;
        }
        case MADT_TYPE_ISO: {
            madt_iso_t* iso = (madt_iso_t*)entry;
            if (g_acpi_info.irq_override_count < MAX_IRQ_OVERRIDES) {
                irq_override_t* override = &g_acpi_info.irq_overrides[g_acpi_info.irq_override_count];
                override->bus_irq = iso->source;
                override->gsi = iso->gsi;
                override->polarity = iso->flags & MPS_INTI_POLARITY_MASK;
                override->trigger_mode = (iso->flags & MPS_INTI_TRIGGER_MASK) >> 2;
                g_acpi_info.irq_override_count++;
            }
            break;
        }
        case MADT_TYPE_LAPIC_ADDR: {
            madt_lapic_addr_t* addr = (madt_lapic_addr_t*)entry;
            g_acpi_info.lapic_address = addr->lapic_address;
            break;
        }
        case MADT_TYPE_LAPIC_X2: {
            madt_x2apic_t* x2 = (madt_x2apic_t*)entry;
            if (g_acpi_info.cpu_count < MAX_CPUS) {
                cpu_info_t* cpu = &g_acpi_info.cpus[g_acpi_info.cpu_count];
                cpu->apic_id = x2->x2apic_id;
                cpu->acpi_processor_id = x2->acpi_processor_uid;
                cpu->enabled = (x2->flags & MADT_LAPIC_ENABLED) != 0;
                cpu->online_capable = (x2->flags & MADT_LAPIC_ONLINE_CAPABLE) != 0;
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
// ACPI Initialization (ACPICA-based)
// ============================================================================

int acpi_init(uint64_t rsdp_hint) {
    ACPI_STATUS status;

    acpi_dbg("ACPI: Initializing via ACPICA...\n");

    // Pass RSDP address to OSL
    acpica_set_rsdp(rsdp_hint);

    g_acpi_info.rsdp_found = true;
    g_acpi_info.rsdp_phys_addr = rsdp_hint;

    // Detect ACPI revision from RSDP
    if (rsdp_hint) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)phys_to_virt(rsdp_hint);
        g_acpi_info.acpi_revision = rsdp->revision;
    }

    // Initialize ACPICA subsystem
    status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
        kprintf("ACPI: AcpiInitializeSubsystem failed: %d\n", (int)status);
        return -1;
    }

    // Initialize table manager and load RSDP/RSDT/XSDT
    status = AcpiInitializeTables(NULL, 32, FALSE);
    if (ACPI_FAILURE(status)) {
        kprintf("ACPI: AcpiInitializeTables failed: %d\n", (int)status);
        return -1;
    }

    // Load all ACPI tables (DSDT, SSDTs, etc.)
    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
        kprintf("ACPI: AcpiLoadTables failed: %d\n", (int)status);
        return -1;
    }

    // Enable ACPI subsystem  
    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        kprintf("ACPI: AcpiEnableSubsystem failed: %d\n", (int)status);
        // Non-fatal — continue without hardware enable
    } else {
        kprintf("ACPI: EnableSub ok\n");
    }

    // Initialize ACPI objects (run _INI methods, etc.)
    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        kprintf("ACPI: InitObjects failed: %d\n", (int)status);
    } else {
        kprintf("ACPI: InitObjects ok\n");
    }

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

acpi_info_t* acpi_get_info(void) {
    return &g_acpi_info;
}

uint32_t acpi_get_cpu_count(void) {
    return g_acpi_info.cpu_count;
}

cpu_info_t* acpi_get_cpu(uint32_t index) {
    if (index >= g_acpi_info.cpu_count) return NULL;
    return &g_acpi_info.cpus[index];
}

uint32_t acpi_get_bsp_apic_id(void) {
    return g_acpi_info.bsp_apic_id;
}

uint64_t acpi_get_lapic_address(void) {
    return g_acpi_info.lapic_address;
}

ioapic_info_t* acpi_get_ioapic(uint32_t index) {
    if (index >= g_acpi_info.ioapic_count) return NULL;
    return &g_acpi_info.ioapics[index];
}

irq_override_t* acpi_get_irq_override(uint8_t isa_irq) {
    for (uint32_t i = 0; i < g_acpi_info.irq_override_count; i++) {
        if (g_acpi_info.irq_overrides[i].bus_irq == isa_irq)
            return &g_acpi_info.irq_overrides[i];
    }
    return NULL;
}

uint32_t acpi_irq_to_gsi(uint8_t isa_irq) {
    irq_override_t* override = acpi_get_irq_override(isa_irq);
    if (override) return override->gsi;
    return isa_irq;
}

void acpi_print_info(void) {
    acpi_dbg("ACPI: LAPIC address = 0x%lx\n", g_acpi_info.lapic_address);
    acpi_dbg("ACPI: %u CPU(s) found:\n", g_acpi_info.cpu_count);
    for (uint32_t i = 0; i < g_acpi_info.cpu_count; i++) {
        acpi_dbg("  CPU %u: APIC ID=%u, %s%s%s\n",
                i, g_acpi_info.cpus[i].apic_id,
                g_acpi_info.cpus[i].enabled ? "enabled" : "disabled",
                g_acpi_info.cpus[i].bsp ? ", BSP" : "",
                g_acpi_info.cpus[i].online_capable ? ", online-capable" : "");
    }
    acpi_dbg("ACPI: %u I/O APIC(s) found:\n", g_acpi_info.ioapic_count);
    for (uint32_t i = 0; i < g_acpi_info.ioapic_count; i++) {
        acpi_dbg("  I/O APIC %u: ID=%u, addr=0x%x, GSI base=%u\n",
                i, g_acpi_info.ioapics[i].id,
                g_acpi_info.ioapics[i].address,
                g_acpi_info.ioapics[i].gsi_base);
    }
    if (g_acpi_info.irq_override_count > 0) {
        acpi_dbg("ACPI: %u IRQ override(s):\n", g_acpi_info.irq_override_count);
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

acpi_sdt_header_t* acpi_find_table(const char* signature) {
    return acpi_find_table_index(signature, 0);
}

acpi_sdt_header_t* acpi_find_table_index(const char* signature, uint32_t index) {
    ACPI_TABLE_HEADER *hdr = NULL;
    ACPI_STATUS status = AcpiGetTable((char*)signature, index + 1, &hdr);
    if (ACPI_FAILURE(status) || !hdr)
        return NULL;
    return (acpi_sdt_header_t*)hdr;
}

uint32_t acpi_get_table_count(const char* signature) {
    uint32_t count = 0;
    while (acpi_find_table_index(signature, count) != NULL)
        count++;
    return count;
}

int acpi_get_table_aml(const char* signature, uint32_t index,
                       const uint8_t** aml, uint32_t* aml_len)
{
    if (!aml || !aml_len) return -1;
    acpi_sdt_header_t* table = acpi_find_table_index(signature, index);
    if (!table) return -1;
    *aml = (const uint8_t*)table + sizeof(acpi_sdt_header_t);
    *aml_len = table->length - sizeof(acpi_sdt_header_t);
    return 0;
}

// ============================================================================
// AML Value helpers (stub implementations for API compat)
// ============================================================================

void aml_bump_init(aml_bump_alloc_t* alloc) {
    if (!alloc) return;
    alloc->pool = (uint8_t*)kalloc(AML_BUMP_POOL_SIZE);
    alloc->used = 0;
    alloc->capacity = alloc->pool ? AML_BUMP_POOL_SIZE : 0;
}

void aml_bump_destroy(aml_bump_alloc_t* alloc) {
    if (alloc && alloc->pool) { kfree(alloc->pool); alloc->pool = NULL; }
}

void* aml_bump_alloc(aml_bump_alloc_t* alloc, uint32_t size) {
    if (!alloc || !alloc->pool || alloc->used + size > alloc->capacity) return NULL;
    void* p = alloc->pool + alloc->used;
    alloc->used += size;
    return p;
}

void aml_value_init(aml_value_t* val) {
    if (val) my_memset(val, 0, sizeof(*val));
}

void aml_value_copy(aml_value_t* dst, const aml_value_t* src,
                    aml_bump_alloc_t* alloc) {
    (void)alloc;
    if (dst && src) my_memcpy(dst, src, sizeof(*dst));
}

uint64_t aml_value_to_integer(const aml_value_t* val) {
    if (!val) return 0;
    if (val->type == AML_VALUE_INTEGER) return val->integer;
    return 0;
}

// ============================================================================
// AML Device Discovery — via AcpiGetDevices
// ============================================================================

typedef struct {
    const char* hid;
    acpi_aml_device_info_t* out;
    int max_out;
    int count;
} find_devices_ctx_t;

static ACPI_STATUS
find_devices_callback(ACPI_HANDLE Object, UINT32 NestingLevel, void *Context,
                      void **ReturnValue)
{
    find_devices_ctx_t* ctx = (find_devices_ctx_t*)Context;
    ACPI_DEVICE_INFO *info = NULL;
    ACPI_STATUS status;
    ACPI_BUFFER path_buf = {ACPI_ALLOCATE_BUFFER, NULL};
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

    acpi_aml_device_info_t* dev = &ctx->out[ctx->count];
    my_memset(dev, 0, sizeof(*dev));

    // Copy path
    my_strncpy(dev->path, (char*)path_buf.Pointer, ACPI_AML_MAX_PATH);

    // Copy HID
    if (info->Valid & ACPI_VALID_HID)
        my_strncpy(dev->hid, info->HardwareId.String, ACPI_AML_MAX_HID);

    // Copy CID (first compatible ID)
    if (info->Valid & ACPI_VALID_CID && info->CompatibleIdList.Count > 0)
        my_strncpy(dev->cid, info->CompatibleIdList.Ids[0].String, ACPI_AML_MAX_HID);

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

int acpi_aml_find_devices_by_hid(const char* hid,
                                 acpi_aml_device_info_t* out,
                                 int max_out)
{
    find_devices_ctx_t ctx;
    if (!out || max_out <= 0) return 0;

    ctx.hid = hid;
    ctx.out = out;
    ctx.max_out = max_out;
    ctx.count = 0;

    AcpiGetDevices((char*)hid, find_devices_callback, &ctx, NULL);
    return ctx.count;
}

// Walk direct children (depth = 1) of the given ACPI path and populate info.
int acpi_aml_find_children(const char* parent_path,
                           acpi_aml_device_info_t* out,
                           int max_out)
{
    if (!parent_path || !out || max_out <= 0) return 0;

    ACPI_HANDLE parent;
    ACPI_STATUS status = AcpiGetHandle(NULL, (char*)parent_path, &parent);
    if (ACPI_FAILURE(status)) return 0;

    find_devices_ctx_t ctx;
    ctx.hid = NULL;
    ctx.out = out;
    ctx.max_out = max_out;
    ctx.count = 0;

    // Walk depth=1 under the parent to get immediate children only
    AcpiWalkNamespace(ACPI_TYPE_DEVICE, parent, 1,
                      find_devices_callback, NULL, &ctx, NULL);
    return ctx.count;
}

// ============================================================================
// _STA Evaluation
// ============================================================================

uint32_t acpi_aml_eval_sta(const char* device_path) {
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};
    uint32_t sta_value = 0x0F;  // Default: present + enabled + functioning + visible

    if (!device_path) return 0x0F;

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status))
        return 0x0F;  // Not found = assume present

    status = AcpiEvaluateObject(handle, "_STA", NULL, &buf);
    if (status == AE_NOT_FOUND)
        return 0x0F;  // No _STA = assume present & enabled

    if (ACPI_SUCCESS(status) && buf.Pointer) {
        ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
        if (obj->Type == ACPI_TYPE_INTEGER)
            sta_value = (uint32_t)obj->Integer.Value;
        AcpiOsFree(buf.Pointer);
    }

    return sta_value;
}

// ============================================================================
// _DEP Evaluation
// ============================================================================

int acpi_aml_eval_dep(const char* device_path,
                      char dep_paths[][ACPI_AML_MAX_PATH],
                      int max_deps)
{
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};
    int count = 0;

    if (!device_path || !dep_paths || max_deps <= 0) return 0;

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status)) return 0;

    status = AcpiEvaluateObject(handle, "_DEP", NULL, &buf);
    if (ACPI_FAILURE(status) || !buf.Pointer) {
        if (buf.Pointer) AcpiOsFree(buf.Pointer);
        return 0;
    }

    ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
    if (obj->Type == ACPI_TYPE_PACKAGE) {
        for (UINT32 i = 0; i < obj->Package.Count && count < max_deps; i++) {
            ACPI_OBJECT *elem = &obj->Package.Elements[i];
            if (elem->Type == ACPI_TYPE_LOCAL_REFERENCE && elem->Reference.Handle) {
                ACPI_BUFFER name_buf = {ACPI_ALLOCATE_BUFFER, NULL};
                if (AcpiGetName(elem->Reference.Handle, ACPI_FULL_PATHNAME,
                                &name_buf) == AE_OK) {
                    my_strncpy(dep_paths[count], (char*)name_buf.Pointer,
                               ACPI_AML_MAX_PATH);
                    AcpiOsFree(name_buf.Pointer);
                    count++;
                }
            }
        }
    }

    AcpiOsFree(buf.Pointer);
    return count;
}

// ============================================================================
// _CRS Evaluation and Resource Parsing
// ============================================================================

static ACPI_STATUS
crs_walk_callback(ACPI_RESOURCE *Resource, void *Context)
{
    acpi_crs_result_t* result = (acpi_crs_result_t*)Context;

    switch (Resource->Type) {
    case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        if (result->mmio_base == 0) {
            result->mmio_base = Resource->Data.FixedMemory32.Address;
            result->mmio_len = Resource->Data.FixedMemory32.AddressLength;
        }
        if (result->mmio_range_count < ACPI_CRS_MAX_MMIO_RANGES) {
            uint8_t idx = result->mmio_range_count;
            result->mmio_ranges[idx].base = Resource->Data.FixedMemory32.Address;
            result->mmio_ranges[idx].length = Resource->Data.FixedMemory32.AddressLength;
            result->mmio_range_count++;
        }
        break;

    case ACPI_RESOURCE_TYPE_ADDRESS32: {
        ACPI_RESOURCE_ADDRESS32 *a32 = &Resource->Data.Address32;
        if (a32->ResourceType == ACPI_MEMORY_RANGE && result->mmio_base == 0) {
            result->mmio_base = a32->Address.Minimum;
            result->mmio_len = a32->Address.AddressLength;
        }
        if (a32->ResourceType == ACPI_MEMORY_RANGE &&
            result->mmio_range_count < ACPI_CRS_MAX_MMIO_RANGES) {
            uint8_t idx = result->mmio_range_count;
            result->mmio_ranges[idx].base = a32->Address.Minimum;
            result->mmio_ranges[idx].length = a32->Address.AddressLength;
            result->mmio_range_count++;
        }
        break;
    }

    case ACPI_RESOURCE_TYPE_ADDRESS64: {
        ACPI_RESOURCE_ADDRESS64 *a64 = &Resource->Data.Address64;
        if (a64->ResourceType == ACPI_MEMORY_RANGE && result->mmio_base == 0) {
            result->mmio_base = a64->Address.Minimum;
            result->mmio_len = (uint32_t)a64->Address.AddressLength;
        }
        if (a64->ResourceType == ACPI_MEMORY_RANGE &&
            result->mmio_range_count < ACPI_CRS_MAX_MMIO_RANGES) {
            uint8_t idx = result->mmio_range_count;
            result->mmio_ranges[idx].base = a64->Address.Minimum;
            result->mmio_ranges[idx].length = (uint32_t)a64->Address.AddressLength;
            result->mmio_range_count++;
        }
        break;
    }

    case ACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
        ACPI_RESOURCE_EXTENDED_IRQ *irq = &Resource->Data.ExtendedIrq;
        result->irq_triggering = (irq->Triggering == ACPI_EDGE_SENSITIVE) ? 1 : 0;
        result->irq_polarity = (irq->Polarity == ACPI_ACTIVE_LOW) ? 1 : 0;
        result->irq_sharing = irq->Shareable ? 1 : 0;
        for (UINT8 j = 0; j < irq->InterruptCount &&
             result->irq_count < ACPI_CRS_MAX_IRQS; j++) {
            result->irqs[result->irq_count++] = irq->Interrupts[j];
        }
        break;
    }

    case ACPI_RESOURCE_TYPE_SERIAL_BUS: {
        ACPI_RESOURCE_I2C_SERIALBUS *i2c;
        if (Resource->Data.CommonSerialBus.Type != ACPI_RESOURCE_SERIAL_TYPE_I2C)
            break;
        if (result->i2c_device_count >= ACPI_CRS_MAX_I2C_DEVICES)
            break;
        i2c = &Resource->Data.I2cSerialBus;
        {
            uint8_t idx = result->i2c_device_count;
            result->i2c_devices[idx].slave_addr = i2c->SlaveAddress;
            result->i2c_devices[idx].connection_speed = i2c->ConnectionSpeed;
            result->i2c_devices[idx].addr_mode =
                (i2c->AccessMode == ACPI_I2C_10BIT_MODE) ? 1 : 0;
            result->i2c_device_count++;
        }
        break;
    }

    case ACPI_RESOURCE_TYPE_GPIO: {
        ACPI_RESOURCE_GPIO *gpio = &Resource->Data.Gpio;
        if (gpio->ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT &&
            gpio->PinTableLength > 0 && gpio->PinTable &&
            !result->gpio_int.valid) {
            result->gpio_int.pin = gpio->PinTable[0];
            result->gpio_int.triggering =
                (gpio->Triggering == ACPI_EDGE_SENSITIVE) ? 1 : 0;
            result->gpio_int.polarity =
                (gpio->Polarity == ACPI_ACTIVE_LOW) ? 1 :
                (gpio->Polarity == ACPI_ACTIVE_BOTH) ? 2 : 0;
            result->gpio_int.sharing = gpio->Shareable ? 1 : 0;
            result->gpio_int.valid = 1;
            if (gpio->ResourceSource.StringPtr) {
                const char *src = gpio->ResourceSource.StringPtr;
                int i = 0;
                while (src[i] && i < ACPI_CRS_GPIO_SOURCE_LEN - 1) {
                    result->gpio_int.resource_source[i] = src[i];
                    i++;
                }
                result->gpio_int.resource_source[i] = '\0';
            }
        }
        break;
    }

    case ACPI_RESOURCE_TYPE_END_TAG:
        return AE_OK;

    default:
        break;
    }

    return AE_OK;
}

int acpi_aml_eval_crs(const char* device_path, acpi_crs_result_t* result) {
    ACPI_HANDLE handle;
    ACPI_STATUS status;

    if (!device_path || !result) return -1;
    my_memset(result, 0, sizeof(*result));

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status)) {
        kprintf("[CRS] %s not found(%d)\n", device_path, (int)status);
        return -1;
    }

    // Check _CRS exists + try eval + walk — one diagnostic line
    {
        ACPI_HANDLE crs_h;
        ACPI_STATUS ck = AcpiGetHandle(handle, "_CRS", &crs_h);
        ACPI_BUFFER eb = {ACPI_ALLOCATE_BUFFER, NULL};
        ACPI_STATUS ev = AcpiEvaluateObject(handle, "_CRS", NULL, &eb);
        int blen = 0;
        if (ACPI_SUCCESS(ev) && eb.Pointer) {
            ACPI_OBJECT *o = (ACPI_OBJECT*)eb.Pointer;
            if (o->Type == ACPI_TYPE_BUFFER) blen = (int)o->Buffer.Length;
            AcpiOsFree(eb.Pointer);
        }
        kprintf("[CRS] %s exist=%d eval=%d blen=%d\n",
                device_path, (int)ck, (int)ev, blen);
    }

    status = AcpiWalkResources(handle, "_CRS", crs_walk_callback, result);
    if (ACPI_FAILURE(status)) {
        kprintf("[CRS] %s walk fail=%d\n", device_path, (int)status);
        return -1;
    }

    return 0;
}

// ============================================================================
// _DSM Call (Device Specific Method)
// ============================================================================

int acpi_aml_call_dsm(const char* device_path,
                      const uint8_t uuid[16],
                      uint64_t revision,
                      uint64_t func_index,
                      aml_value_t* result)
{
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    ACPI_OBJECT_LIST arg_list;
    ACPI_OBJECT args[4];
    ACPI_BUFFER ret_buf = {ACPI_ALLOCATE_BUFFER, NULL};

    if (!device_path || !uuid) return -1;
    if (result) aml_value_init(result);

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status)) return -1;

    // Arg0: UUID buffer (16 bytes, byte-swapped as per ACPI spec)
    uint8_t uuid_buf[16];
    // ACPI UUID byte order: first 3 components swapped, rest as-is
    uuid_buf[0] = uuid[3]; uuid_buf[1] = uuid[2];
    uuid_buf[2] = uuid[1]; uuid_buf[3] = uuid[0];
    uuid_buf[4] = uuid[5]; uuid_buf[5] = uuid[4];
    uuid_buf[6] = uuid[7]; uuid_buf[7] = uuid[6];
    my_memcpy(&uuid_buf[8], &uuid[8], 8);

    args[0].Type = ACPI_TYPE_BUFFER;
    args[0].Buffer.Length = 16;
    args[0].Buffer.Pointer = uuid_buf;

    // Arg1: Revision
    args[1].Type = ACPI_TYPE_INTEGER;
    args[1].Integer.Value = revision;

    // Arg2: Function Index
    args[2].Type = ACPI_TYPE_INTEGER;
    args[2].Integer.Value = func_index;

    // Arg3: Empty package
    args[3].Type = ACPI_TYPE_PACKAGE;
    args[3].Package.Count = 0;
    args[3].Package.Elements = NULL;

    arg_list.Count = 4;
    arg_list.Pointer = args;

    status = AcpiEvaluateObject(handle, "_DSM", &arg_list, &ret_buf);
    if (ACPI_FAILURE(status)) return -1;

    if (result && ret_buf.Pointer) {
        ACPI_OBJECT *obj = (ACPI_OBJECT*)ret_buf.Pointer;
        if (obj->Type == ACPI_TYPE_INTEGER) {
            result->type = AML_VALUE_INTEGER;
            result->integer = obj->Integer.Value;
        } else if (obj->Type == ACPI_TYPE_BUFFER) {
            result->type = AML_VALUE_BUFFER;
            // Note: caller must not use buffer after this function returns
            // since we free it here. For simplicity, just return integer 0.
            result->type = AML_VALUE_INTEGER;
            result->integer = 0;
            if (obj->Buffer.Length >= 1)
                result->integer = obj->Buffer.Pointer[0];
        }
    }

    if (ret_buf.Pointer)
        AcpiOsFree(ret_buf.Pointer);
    return 0;
}

// ============================================================================
// PCI-to-ACPI Path Mapping
// ============================================================================

typedef struct {
    uint32_t target_adr;
    char* out_path;
    int out_size;
    int found;
} find_pci_ctx_t;

static ACPI_STATUS
find_pci_callback(ACPI_HANDLE Object, UINT32 NestingLevel, void *Context,
                  void **ReturnValue)
{
    find_pci_ctx_t* ctx = (find_pci_ctx_t*)Context;
    ACPI_STATUS status;
    uint64_t adr_val = 0;
    ACPI_BUFFER name_buf = {ACPI_ALLOCATE_BUFFER, NULL};
    (void)NestingLevel;
    (void)ReturnValue;

    if (ctx->found) return AE_CTRL_TERMINATE;

    // Try to get _ADR via public API
    {
        ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};
        status = AcpiEvaluateObject(Object, "_ADR", NULL, &buf);
        if (ACPI_FAILURE(status)) return AE_OK;
        if (buf.Pointer) {
            ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
            if (obj->Type == ACPI_TYPE_INTEGER)
                adr_val = obj->Integer.Value;
            AcpiOsFree(buf.Pointer);
        } else {
            return AE_OK;
        }
    }

    if ((uint32_t)adr_val == ctx->target_adr) {
        status = AcpiGetName(Object, ACPI_FULL_PATHNAME, &name_buf);
        if (ACPI_SUCCESS(status) && name_buf.Pointer) {
            my_strncpy(ctx->out_path, (char*)name_buf.Pointer, ctx->out_size);
            AcpiOsFree(name_buf.Pointer);
            ctx->found = 1;
            return AE_CTRL_TERMINATE;
        }
    }

    return AE_OK;
}

int acpi_find_pci_acpi_path(uint8_t bus, uint8_t device, uint8_t function,
                            char* out_path, int out_size)
{
    find_pci_ctx_t ctx;
    ACPI_STATUS status;

    (void)bus;
    if (!out_path || out_size <= 0) return -1;

    ctx.target_adr = ((uint32_t)device << 16) | function;
    ctx.out_path = out_path;
    ctx.out_size = out_size;
    ctx.found = 0;

    // Walk entire namespace looking for matching _ADR
    status = AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT,
                               ACPI_UINT32_MAX, find_pci_callback,
                               NULL, &ctx, NULL);
    (void)status;

    if (ctx.found) return 0;

    // Fallback: for Intel LPSS I2C, try I2Cn naming convention
    {
        char i2c_name[16];
        for (int n = 0; n < 8; n++) {
            ACPI_HANDLE handle;

            // Build candidate name like "\\_SB.PC00.I2Cn"
            // Try finding I2Cn anywhere in namespace
            i2c_name[0] = 'I';
            i2c_name[1] = '2';
            i2c_name[2] = 'C';
            i2c_name[3] = '0' + (char)n;
            i2c_name[4] = 0;

            // Walk namespace for this name
            ACPI_HANDLE root;
            AcpiGetHandle(NULL, "\\_SB", &root);
            if (!root) break;

            // Use AcpiGetDevices with NULL HID to find all, then match name
            // Simpler: try common paths
            char paths_to_try[4][32] = {
                "\\_SB.PC00.",
                "\\_SB.PCI0.",
                "\\_SB.",
                "\\."
            };
            int matched = 0;
            for (int p = 0; p < 4 && !matched; p++) {
                char full[64];
                int plen = my_strlen(paths_to_try[p]);
                my_memcpy(full, paths_to_try[p], plen);
                my_memcpy(full + plen, i2c_name, 5);
                if (AcpiGetHandle(NULL, full, &handle) == AE_OK) {
                    int fmatch = 0;
                    if (device == 0x15 && function == (uint8_t)n && n <= 3)
                        fmatch = 1;
                    else if (device == 0x19 && function == (uint8_t)(n - 4) && n >= 4)
                        fmatch = 1;
                    if (fmatch) {
                        my_strncpy(out_path, full, out_size);
                        return 0;
                    }
                }
            }
        }
    }

    return -1;
}

// ============================================================================
// Power On Device with Dependencies
// ============================================================================

static int acpi_eval_method_on_path(const char* path, const char* method) {
    ACPI_HANDLE handle;
    ACPI_STATUS status;

    status = AcpiGetHandle(NULL, (char*)path, &handle);
    if (ACPI_FAILURE(status)) return -1;

    status = AcpiEvaluateObject(handle, (char*)method, NULL, NULL);
    return ACPI_SUCCESS(status) ? 0 : -1;
}

int acpi_power_on_device_with_deps(const char* device_path)
{
    char dep_paths[ACPI_DEP_MAX_DEPS][ACPI_AML_MAX_PATH];
    int dep_count;
    int dep_failures = 0;

    if (!device_path) return ACPI_FW_STATUS_NOT_FOUND;

    // Step 1: Evaluate _DEP
    dep_count = acpi_aml_eval_dep(device_path, dep_paths, ACPI_DEP_MAX_DEPS);

    // Step 2: Power on dependencies
    for (int i = 0; i < dep_count; i++) {
        if (acpi_eval_method_on_path(dep_paths[i], "_PS0") == 0) {
            kprintf("[ACPI-PWR] dep %s _PS0 ok\n", dep_paths[i]);
        } else if (acpi_eval_method_on_path(dep_paths[i], "_ON") == 0) {
            kprintf("[ACPI-PWR] dep %s _ON ok\n", dep_paths[i]);
        } else {
            dep_failures++;
        }
    }

    // Step 3: Power on _PR0 power resources (evaluate _PR0 package)
    {
        ACPI_HANDLE handle;
        ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};
        ACPI_STATUS status = AcpiGetHandle(NULL, (char*)device_path, &handle);
        if (ACPI_SUCCESS(status)) {
            status = AcpiEvaluateObject(handle, "_PR0", NULL, &buf);
            if (ACPI_SUCCESS(status) && buf.Pointer) {
                ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
                if (obj->Type == ACPI_TYPE_PACKAGE) {
                    for (UINT32 i = 0; i < obj->Package.Count; i++) {
                        ACPI_OBJECT *elem = &obj->Package.Elements[i];
                        if (elem->Type == ACPI_TYPE_LOCAL_REFERENCE &&
                            elem->Reference.Handle) {
                            ACPI_BUFFER nb = {ACPI_ALLOCATE_BUFFER, NULL};
                            if (AcpiGetName(elem->Reference.Handle,
                                            ACPI_FULL_PATHNAME, &nb) == AE_OK) {
                                char pr_path[ACPI_AML_MAX_PATH];
                                my_strncpy(pr_path, (char*)nb.Pointer,
                                           ACPI_AML_MAX_PATH);
                                AcpiOsFree(nb.Pointer);
                                if (acpi_eval_method_on_path(pr_path, "_ON") == 0)
                                    kprintf("[ACPI-PWR] pr0 %s _ON ok\n", pr_path);
                            }
                        }
                    }
                }
                AcpiOsFree(buf.Pointer);
            }
        }
    }

    // Step 4: Check _STA
    uint32_t dev_sta = acpi_aml_eval_sta(device_path);
    kprintf("[ACPI-PWR] %s _STA=0x%x\n", device_path, dev_sta);

    // Step 5: Call _PS0 on target device
    {
        int ps0_rc = acpi_eval_method_on_path(device_path, "_PS0");
        kprintf("[ACPI-PWR] %s _PS0=%s dep=%d/%d\n",
                device_path, ps0_rc == 0 ? "ok" : "fail/absent",
                dep_count - dep_failures, dep_count);
    }

    return ACPI_FW_STATUS_OK;
}

// ============================================================================
// Firmware Power On by HID
// ============================================================================

int acpi_fw_power_on_device(const char* hid, acpi_aml_device_info_t* out)
{
    acpi_aml_device_info_t info;
    int found;

    if (!hid || !hid[0]) return ACPI_FW_STATUS_NOT_FOUND;

    found = acpi_aml_find_devices_by_hid(hid, &info, 1);
    if (found <= 0) return ACPI_FW_STATUS_NOT_FOUND;

    if (out) my_memcpy(out, &info, sizeof(info));

    int rc = acpi_power_on_device_with_deps(info.path);
    return rc;
}

// ============================================================================
// Firmware Power On by PCI BDF
// ============================================================================

int acpi_fw_power_on_pci_device(uint8_t bus, uint8_t device, uint8_t function)
{
    char acpi_path[ACPI_AML_MAX_PATH];
    int rc;

    rc = acpi_find_pci_acpi_path(bus, device, function,
                                 acpi_path, sizeof(acpi_path));
    if (rc != 0) {
        kprintf("[ACPI-PCI] no ACPI path for PCI %02x:%02x.%x\n",
                bus, device, function);
        return ACPI_FW_STATUS_NOT_FOUND;
    }

    kprintf("[ACPI-PCI] PCI %02x:%02x.%x => %s\n",
            bus, device, function, acpi_path);
    return acpi_power_on_device_with_deps(acpi_path);
}

// ============================================================================
// Execute a named method on a device
// ============================================================================

int acpi_aml_exec_device_method(const char* device_path,
                                const char* method_name,
                                uint64_t* ret_value)
{
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};

    if (!device_path || !method_name) return -1;

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status)) return -1;

    status = AcpiEvaluateObject(handle, (char*)method_name, NULL, &buf);
    if (ACPI_FAILURE(status)) return -1;

    if (ret_value && buf.Pointer) {
        ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
        if (obj->Type == ACPI_TYPE_INTEGER)
            *ret_value = obj->Integer.Value;
        else
            *ret_value = 0;
    }

    if (buf.Pointer) AcpiOsFree(buf.Pointer);
    return 0;
}

int acpi_aml_exec_device_method_pkg3(const char* device_path,
                                     const char* method_name,
                                     uint64_t out_values[3])
{
    ACPI_HANDLE handle;
    ACPI_STATUS status;
    ACPI_BUFFER buf = {ACPI_ALLOCATE_BUFFER, NULL};
    int rc = -1;

    if (!device_path || !method_name || !out_values)
        return -1;

    status = AcpiGetHandle(NULL, (char*)device_path, &handle);
    if (ACPI_FAILURE(status))
        return -1;

    status = AcpiEvaluateObject(handle, (char*)method_name, NULL, &buf);
    if (ACPI_FAILURE(status))
        return -1;

    if (buf.Pointer) {
        ACPI_OBJECT *obj = (ACPI_OBJECT*)buf.Pointer;
        if (obj->Type == ACPI_TYPE_PACKAGE && obj->Package.Count >= 3) {
            ACPI_OBJECT *elems = obj->Package.Elements;
            if (elems[0].Type == ACPI_TYPE_INTEGER &&
                elems[1].Type == ACPI_TYPE_INTEGER &&
                elems[2].Type == ACPI_TYPE_INTEGER) {
                out_values[0] = elems[0].Integer.Value;
                out_values[1] = elems[1].Integer.Value;
                out_values[2] = elems[2].Integer.Value;
                rc = 0;
            }
        }
        AcpiOsFree(buf.Pointer);
    }

    return rc;
}

// ============================================================================
// ACPI Power Management — S5 (poweroff) and reset
// ============================================================================

static uint32_t g_pm1a_cnt_blk = 0;
static uint32_t g_pm1b_cnt_blk = 0;
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;
static uint8_t  g_reset_reg_space = 0;
static uint64_t g_reset_reg_addr = 0;
static uint8_t  g_reset_value = 0;
static int      g_pm_initialized = 0;

static inline void acpi_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static inline void acpi_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint16_t acpi_inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static int acpi_parse_s5(uint8_t *aml, uint32_t length) {
    for (uint32_t i = 0; i + 4 < length; i++) {
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            for (uint32_t j = i + 4; j < i + 32 && j + 5 < length; j++) {
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
                        if (aml[k] == 0x0A && k + 1 < length) {
                            g_slp_typa = aml[k + 1]; k += 2;
                        } else {
                            g_slp_typa = aml[k]; k += 1;
                        }
                    }
                    if (k < length) {
                        if (aml[k] == 0x0A && k + 1 < length) {
                            g_slp_typb = aml[k + 1]; k += 2;
                        } else {
                            g_slp_typb = aml[k]; k += 1;
                        }
                    }
                    return 1;
                }
            }
        }
    }
    return 0;
}

void acpi_pm_init(void) {
    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table(ACPI_SIG_FADT);
    if (!fadt) {
        kprintf("ACPI PM: FADT not found, power management unavailable\n");
        return;
    }

    g_pm1a_cnt_blk = fadt->pm1a_control_block;
    g_pm1b_cnt_blk = fadt->pm1b_control_block;

    if (fadt->header.length >= 129) {
        g_reset_reg_space = fadt->reset_reg_addr_space;
        g_reset_reg_addr = fadt->reset_reg_address;
        g_reset_value = fadt->reset_value;
    }

    // Use ACPICA to get DSDT for S5 parsing
    uint64_t dsdt_addr = 0;
    if (fadt->header.length >= 148 && fadt->x_dsdt != 0)
        dsdt_addr = fadt->x_dsdt;
    else
        dsdt_addr = fadt->dsdt;

    if (dsdt_addr) {
        acpi_sdt_header_t *dsdt = (acpi_sdt_header_t *)phys_to_virt(dsdt_addr);
        uint8_t *aml = (uint8_t *)dsdt + sizeof(acpi_sdt_header_t);
        uint32_t aml_len = dsdt->length - sizeof(acpi_sdt_header_t);
        if (acpi_parse_s5(aml, aml_len)) {
            kprintf("ACPI PM: S5 sleep type: SLP_TYPa=%u SLP_TYPb=%u\n",
                    g_slp_typa, g_slp_typb);
        } else {
            kprintf("ACPI PM: \\_S5 not found in DSDT, using defaults\n");
            g_slp_typa = 5;
            g_slp_typb = 0;
        }
    }

    g_pm_initialized = 1;
    kprintf("ACPI PM: initialized (PM1a=0x%x, PM1b=0x%x, reset=0x%lx)\n",
            g_pm1a_cnt_blk, g_pm1b_cnt_blk, (unsigned long)g_reset_reg_addr);
}

void acpi_poweroff(void) {
    if (!g_pm_initialized) {
        kprintf("ACPI: power management not initialized, trying QEMU exit\n");
        acpi_outw(0x604, 0x2000);
        for (;;) __asm__ volatile("cli; hlt");
    }

    kprintf("ACPI: powering off...\n");
    __asm__ volatile("cli");

    uint16_t val = ACPI_PM1_SLP_TYP(g_slp_typa) | ACPI_PM1_SLP_EN;
    acpi_outw((uint16_t)g_pm1a_cnt_blk, val);

    if (g_pm1b_cnt_blk) {
        val = ACPI_PM1_SLP_TYP(g_slp_typb) | ACPI_PM1_SLP_EN;
        acpi_outw((uint16_t)g_pm1b_cnt_blk, val);
    }

    acpi_outw(0x604, 0x2000);
    for (;;) __asm__ volatile("hlt");
}

// ============================================================================
// PCI Interrupt Routing (_PRT) Lookup
// ============================================================================

int acpi_pci_lookup_irq(const char* bridge_path,
                        uint8_t pci_device, uint8_t pci_pin,
                        uint32_t* out_gsi)
{
    ACPI_HANDLE handle;
    ACPI_STATUS status;

    if (!bridge_path || !out_gsi) return -1;

    status = AcpiGetHandle(NULL, (char*)bridge_path, &handle);
    if (ACPI_FAILURE(status)) {
        kprintf("[PRT] bridge %s not found (%d)\n", bridge_path, (int)status);
        return -1;
    }

    // Get _PRT routing table
    ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
    status = AcpiGetIrqRoutingTable(handle, &buf);
    if (ACPI_FAILURE(status)) {
        kprintf("[PRT] %s _PRT failed (%d)\n", bridge_path, (int)status);
        return -2;
    }

    // Walk the routing table entries.
    // Address encodes the device number: high word of high dword = device,
    // function is 0xFFFF (any).  Format: (device << 16) | 0xFFFF.
    UINT64 match_addr = ((UINT64)pci_device << 16) | 0xFFFFULL;

    ACPI_PCI_ROUTING_TABLE *entry = (ACPI_PCI_ROUTING_TABLE *)buf.Pointer;
    int found = 0;

    // Dump all _PRT entries for diagnostics (first call only)
    {
        static int prt_dumped = 0;
        if (!prt_dumped) {
            prt_dumped = 1;
            ACPI_PCI_ROUTING_TABLE *e = entry;
            int n = 0;
            while (e && e->Length > 0 && n < 64) {
                kprintf("[PRT]   #%d addr=0x%llx pin=%u src='%s' idx=%u\n",
                        n, (unsigned long long)e->Address, e->Pin,
                        e->Source[0] ? e->Source : "(hw)",
                        e->SourceIndex);
                e = (ACPI_PCI_ROUTING_TABLE *)((char *)e + e->Length);
                n++;
            }
        }
    }

    while (entry && entry->Length > 0) {
        if (entry->Address == match_addr && entry->Pin == pci_pin) {
            if (entry->Source[0] == '\0') {
                // Hardwired GSI (no link device) — most common on modern PCH
                *out_gsi = entry->SourceIndex;
                found = 1;
            } else {
                // Link device (e.g. LNKA) — evaluate _CRS to get GSI
                ACPI_HANDLE link;
                if (ACPI_SUCCESS(AcpiGetHandle(handle, entry->Source, &link)) ||
                    ACPI_SUCCESS(AcpiGetHandle(NULL, entry->Source, &link))) {
                    ACPI_BUFFER lbuf = { ACPI_ALLOCATE_BUFFER, NULL };
                    if (ACPI_SUCCESS(AcpiGetCurrentResources(link, &lbuf))) {
                        ACPI_RESOURCE *res = (ACPI_RESOURCE *)lbuf.Pointer;
                        while (res && res->Type != ACPI_RESOURCE_TYPE_END_TAG) {
                            if (res->Type == ACPI_RESOURCE_TYPE_IRQ &&
                                res->Data.Irq.InterruptCount > 0) {
                                *out_gsi = res->Data.Irq.Interrupts[0];
                                found = 1;
                                break;
                            }
                            if (res->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ &&
                                res->Data.ExtendedIrq.InterruptCount > 0) {
                                *out_gsi = res->Data.ExtendedIrq.Interrupts[0];
                                found = 1;
                                break;
                            }
                            res = ACPI_NEXT_RESOURCE(res);
                        }
                        AcpiOsFree(lbuf.Pointer);
                    }
                }
            }
            break;
        }
        entry = (ACPI_PCI_ROUTING_TABLE *)((char *)entry + entry->Length);
    }

    AcpiOsFree(buf.Pointer);

    if (found) {
        kprintf("[PRT] %s dev=%u pin=%u -> GSI %u\n",
                bridge_path, pci_device, pci_pin, *out_gsi);
        return 0;
    }

    kprintf("[PRT] %s dev=%u pin=%u: no matching entry\n",
            bridge_path, pci_device, pci_pin);
    return -3;
}

void acpi_reset(void) {
    kprintf("ACPI: resetting system...\n");
    __asm__ volatile("cli");

    if (g_reset_reg_addr != 0) {
        if (g_reset_reg_space == 1)
            acpi_outb((uint16_t)g_reset_reg_addr, g_reset_value);
        for (volatile int i = 0; i < 1000000; i++);
    }

    // Keyboard controller reset
    uint8_t good = 0x02;
    while (good & 0x02)
        __asm__ volatile("inb $0x64, %0" : "=a"(good));
    __asm__ volatile("outb %0, $0x64" :: "a"((uint8_t)0xFE));

    for (volatile int i = 0; i < 1000000; i++);

    // Triple fault
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int $3" :: "m"(null_idt));

    for (;;) __asm__ volatile("hlt");
}
