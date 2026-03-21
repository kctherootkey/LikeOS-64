// LikeOS-64 - Minimal PCI enumeration
#include "../../include/kernel/pci.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/lapic.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static int g_pci_count = 0;

// Spinlock for PCI config space access (address + data must be atomic)
static spinlock_t pci_lock = SPINLOCK_INIT("pci");

static inline void outl(unsigned short port, unsigned int val)
{
    __asm__ volatile("outl %0,%1" :: "a"(val), "Nd"(port));
}

static inline unsigned int inl(unsigned short port)
{
    unsigned int v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

unsigned int pci_cfg_read32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off)
{
    unsigned int address = (unsigned int)((1u << 31) |
        ((unsigned int)bus << 16) |
        ((unsigned int)dev << 11) |
        ((unsigned int)func << 8) |
        (off & 0xFC));
    uint64_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    outl(PCI_CONFIG_ADDRESS, address);
    unsigned int value = inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&pci_lock, flags);
    return value;
}

void pci_cfg_write32(unsigned char bus, unsigned char dev, unsigned char func, unsigned char off, unsigned int value)
{
    unsigned int address = (unsigned int)((1u << 31) |
        ((unsigned int)bus << 16) |
        ((unsigned int)dev << 11) |
        ((unsigned int)func << 8) |
        (off & 0xFC));
    uint64_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&pci_lock, flags);
}

void pci_enable_busmaster_mem(const pci_device_t* dev)
{
    if(!dev) {
        return;
    }
    unsigned int cmd = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
    unsigned int newcmd = cmd | 0x6; /* MEM + BUS MASTER */
    if(newcmd != cmd) {
        pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04, newcmd);
    }
}

void pci_init(void)
{
    g_pci_count = 0;
}

// Very simple BAR allocator: assigns incremental 1MB windows starting at 0xF2000000 downward for any 32/64-bit memory BAR that is zero/unassigned.
// Not spec-complete; just enough so devices like xHCI get a decodable MMIO region under 4GB.
static unsigned long g_next_bar_base = 0xF2000000UL; /* leave 0xF1000000 for earlier manual uses */
static unsigned long alloc_bar_region(unsigned long size)
{
    /* Align base downward by size */
    if(size == 0) {
        size = 0x1000;
    }
    if(size & (size - 1)) { /* round to power of two */
        unsigned long p = 1;
        while(p < size) {
            p <<= 1;
        }
        size = p;
    }
    if(g_next_bar_base < size) {
        return 0; /* out of space */
    }
    g_next_bar_base -= size;
    g_next_bar_base &= ~(size - 1);
    return g_next_bar_base;
}

void pci_assign_unassigned_bars(void)
{
    for(int i = 0; i < g_pci_count; i++) {
        pci_device_t *p = &g_pci_devices[i];
        for(int bar = 0; bar < 6; ++bar) {
            unsigned int off = 0x10 + bar * 4;
            unsigned int val = pci_cfg_read32(p->bus, p->device, p->function, off);
            /* Treat zero, 0xFFFFFFFF, or suspicious tiny (<4KB) mem BARs as unassigned */
            int looks_mem = (val & 0x1) == 0; /* memory BAR */
            unsigned int base_field = val & ~0xFUL;
            if(val == 0 || val == 0xFFFFFFFF || (looks_mem && base_field < 0x1000)) {
                /* Probe size */
                pci_cfg_write32(p->bus, p->device, p->function, off, 0xFFFFFFFF);
                unsigned int mask = pci_cfg_read32(p->bus, p->device, p->function, off);
                if(mask == 0 || mask == 0xFFFFFFFF) {
                    /* restore original and skip */
                    pci_cfg_write32(p->bus, p->device, p->function, off, val);
                    continue; /* cannot size */
                }
                if(mask & 0x1) {
                    continue; /* IO BAR skip */
                }
                int is64 = (mask & 0x4) ? 1 : 0;
                unsigned int mask_hi = 0;
                if(is64 && bar < 5) {
                    unsigned int off_hi = 0x10 + (bar + 1) * 4;
                    pci_cfg_write32(p->bus, p->device, p->function, off_hi, 0xFFFFFFFF);
                    mask_hi = pci_cfg_read32(p->bus, p->device, p->function, off_hi);
                }
                /* Restore original mask probe scribbles (write 0 before assigning new base to avoid transient decode) */
                pci_cfg_write32(p->bus, p->device, p->function, off, 0);
                if(is64 && bar < 5) {
                    unsigned int off_hi = 0x10 + (bar + 1) * 4;
                    pci_cfg_write32(p->bus, p->device, p->function, off_hi, 0);
                }
                unsigned long size_mask = (unsigned long)(mask & ~0xFUL);
                if(is64) {
                    size_mask |= ((unsigned long)mask_hi << 32);
                }
                unsigned long size = (~size_mask + 1);
                if(size == 0 || size > (1UL << 24)) {
                    size = 1UL << 16; /* clamp */
                }
                unsigned long base = alloc_bar_region(size);
                if(base) {
                    unsigned int low = (unsigned int)((base & 0xFFFFFFFFUL) | (mask & 0xF));
                    pci_cfg_write32(p->bus, p->device, p->function, off, low);
                    if(is64 && bar < 5) {
                        unsigned int off_hi = 0x10 + (bar + 1) * 4;
                        pci_cfg_write32(p->bus, p->device, p->function, off_hi, (unsigned int)(base >> 32));
                        /* consume next BAR slot for 64-bit */
                        p->bar[bar] = low;
                        p->bar[bar + 1] = (unsigned int)(base >> 32);
                        bar++;
                        continue;
                    }
                    /* Re-read and store */
                    p->bar[bar] = pci_cfg_read32(p->bus, p->device, p->function, off);
                }
            }
        }
    }
}

static void record_device(unsigned char bus, unsigned char dev, unsigned char func)
{
    if(g_pci_count >= PCI_MAX_DEVICES) {
        return;
    }
    unsigned int id = pci_cfg_read32(bus, dev, func, 0x00);
    unsigned short vendor = id & 0xFFFF;
    if(vendor == 0xFFFF) {
        return;
    }
    unsigned short device = (id >> 16) & 0xFFFF;
    unsigned int class_reg = pci_cfg_read32(bus, dev, func, 0x08);
    unsigned char class_code = (class_reg >> 24) & 0xFF;
    unsigned char subclass = (class_reg >> 16) & 0xFF;
    unsigned char prog_if = (class_reg >> 8) & 0xFF;
    pci_device_t *p = &g_pci_devices[g_pci_count++];
    p->bus = bus;
    p->device = dev;
    p->function = func;
    p->vendor_id = vendor;
    p->device_id = device;
    p->class_code = class_code;
    p->subclass = subclass;
    p->prog_if = prog_if;
    for(int i = 0; i < 6; i++) {
        p->bar[i] = pci_cfg_read32(bus, dev, func, 0x10 + i * 4);
    }
    unsigned int ilr = pci_cfg_read32(bus, dev, func, 0x3C);
    p->interrupt_line = ilr & 0xFF;
    p->interrupt_pin = (ilr >> 8) & 0xFF;
}

int pci_enumerate(void)
{
    g_pci_count = 0;
    for(unsigned int bus = 0; bus < 256; ++bus) {
        for(unsigned char dev = 0; dev < 32; ++dev) {
            unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
            if((id & 0xFFFF) == 0xFFFF) {
                continue;
            }
            unsigned int header = pci_cfg_read32(bus, dev, 0, 0x0C);
            unsigned char multifunction = ((header >> 16) & 0x80) ? 1 : 0;
            record_device(bus, dev, 0);
            if(multifunction) {
                for(unsigned char func = 1; func < 8; ++func) {
                    unsigned int idf = pci_cfg_read32(bus, dev, func, 0x00);
                    if((idf & 0xFFFF) == 0xFFFF) {
                        continue;
                    }
                    record_device(bus, dev, func);
                }
            }
        }
    }
    kprintf("PCI: %d devices found\n", g_pci_count);
    return g_pci_count;
}

const pci_device_t* pci_get_devices(int* count)
{
    if(count) {
        *count = g_pci_count;
    }
    return g_pci_devices;
}

const pci_device_t* pci_get_first_xhci(void)
{
    return pci_get_xhci(0);
}

const pci_device_t* pci_get_xhci(int index)
{
    int found = 0;
    for(int i = 0; i < g_pci_count; i++) {
        const pci_device_t *p = &g_pci_devices[i];
        if(p->class_code == 0x0C && p->subclass == 0x03 && p->prog_if == 0x30) {
            if (found == index) return p;
            found++;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  PCI Capability list walking
// ---------------------------------------------------------------------------

uint8_t pci_find_capability(const pci_device_t* dev, uint8_t cap_id)
{
    if (!dev) return 0;

    // Check Status register bit 4 (Capabilities List)
    uint32_t status = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
    if (!((status >> 16) & (1 << 4)))   // Status is upper 16 bits of reg 0x04
        return 0;

    // Capabilities pointer is at config offset 0x34, low 8 bits
    uint32_t raw = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x34);
    uint8_t ptr = (uint8_t)(raw & 0xFC);  // Must be DWORD-aligned

    // Walk the linked list (max 48 entries to avoid infinite loops)
    for (int i = 0; i < 48 && ptr != 0; i++) {
        uint32_t cap_hdr = pci_cfg_read32(dev->bus, dev->device, dev->function, ptr);
        uint8_t id   = (uint8_t)(cap_hdr & 0xFF);
        uint8_t next = (uint8_t)((cap_hdr >> 8) & 0xFC);

        if (id == cap_id)
            return ptr;

        ptr = next;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  PCI MSI setup
// ---------------------------------------------------------------------------

int pci_enable_msi(const pci_device_t* dev, uint8_t vector)
{
    uint8_t cap_off = pci_find_capability(dev, PCI_CAP_MSI);
    if (cap_off == 0) {
        kprintf("PCI MSI: no MSI capability found for %02x:%02x.%x\n",
                dev->bus, dev->device, dev->function);
        return -1;
    }

    // Read Message Control register (cap_off + 2, upper 16 bits of DWORD)
    uint32_t cap_dw0 = pci_cfg_read32(dev->bus, dev->device, dev->function, cap_off);
    uint16_t msg_ctrl = (uint16_t)(cap_dw0 >> 16);
    int is_64bit = (msg_ctrl >> 7) & 1;   // Bit 7: 64-bit address capable

    // Disable MSI while programming (clear bit 0 of Message Control)
    cap_dw0 &= ~(1U << 16);  // Clear MSI Enable bit (bit 0 of msg_ctrl, which is bit 16 of DWORD)
    pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off, cap_dw0);

    // Program Message Address (cap_off + 4)
    // Use CPUID-based APIC ID — safe to call before lapic_init(), unlike
    // lapic_get_id() which needs x2APIC mode detection to be set up first.
    uint32_t bsp_apic_id = lapic_get_id_cpuid();
    uint32_t msi_addr = MSI_ADDR_BASE | ((bsp_apic_id & 0xFF) << 12);
    pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off + 0x04, msi_addr);

    // If 64-bit capable, zero upper address and write data at cap_off + 0x0C
    // If 32-bit, data is at cap_off + 0x08
    uint8_t data_off;
    if (is_64bit) {
        pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off + 0x08, 0);
        data_off = cap_off + 0x0C;
    } else {
        data_off = cap_off + 0x08;
    }

    // Program Message Data: vector number, edge-triggered, fixed delivery
    // Data register is in the lower 16 bits of its DWORD
    uint32_t data_dw = pci_cfg_read32(dev->bus, dev->device, dev->function, data_off);
    data_dw = (data_dw & 0xFFFF0000) | (uint16_t)vector;
    pci_cfg_write32(dev->bus, dev->device, dev->function, data_off, data_dw);

    // Set Multiple Message Enable to 0 (allocate 1 vector) and enable MSI
    // Re-read cap DWORD, set bit 0 of msg_ctrl (MSI Enable), clear bits 4:6 (MME = 0)
    cap_dw0 = pci_cfg_read32(dev->bus, dev->device, dev->function, cap_off);
    cap_dw0 &= ~(0x0070U << 16);  // Clear MME bits (bits 4-6 of msg_ctrl)
    cap_dw0 |=  (1U << 16);       // Set MSI Enable (bit 0 of msg_ctrl)
    pci_cfg_write32(dev->bus, dev->device, dev->function, cap_off, cap_dw0);

    // Disable legacy INTx in PCI Command register (bit 10)
    uint32_t cmd = pci_cfg_read32(dev->bus, dev->device, dev->function, 0x04);
    cmd |= PCI_CMD_INTX_DISABLE;
    pci_cfg_write32(dev->bus, dev->device, dev->function, 0x04, cmd);

    kprintf("PCI MSI: enabled for %02x:%02x.%x - vector %d, addr 0x%x (APIC %d), %s\n",
            dev->bus, dev->device, dev->function, vector, msi_addr, bsp_apic_id,
            is_64bit ? "64-bit" : "32-bit");
    return 0;
}
