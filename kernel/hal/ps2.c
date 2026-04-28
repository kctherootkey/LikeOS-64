// LikeOS-64 - PS/2 Controller (8042) Initialization
// Minimal init: BIOS/UEFI firmware already configured i8042 emulation on
// the eSPI bus.  We set up CTR for keyboard + mouse and program IOAPIC.
// No PNP _SRS (triggers EC GPE storms), no self-test (leaks stale 0x55
// on eSPI), no ECAM/PCH decode (preserved by pci.c bridge-skip fix).

#include "../../include/kernel/console.h"
#include "../../include/kernel/ps2.h"
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/ioapic.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define STR_OBF      0x01
#define STR_IBF      0x02

#define CTR_KBDINT   0x01
#define CTR_AUXINT   0x02
#define CTR_KBDDIS   0x10
#define CTR_AUXDIS   0x20
#define CTR_XLATE    0x40

#define CTL_TIMEOUT   10000
#define BUFFER_SIZE   16

#define CMD_CTL_RCTR  0x0120   // Read  CTR (cmd 0x20, read 1)
#define CMD_CTL_WCTR  0x1060   // Write CTR (cmd 0x60, write 1)

#define KBD_VECTOR    33       // IRQ1  -> vector 33
#define AUX_VECTOR    44       // IRQ12 -> vector 44

static inline void udelay50(void)
{
    for (int i = 0; i < 50; i++)
        __asm__ volatile("outb %%al, $0x80" ::: "memory");
}

static int i8042_wait_write(void)
{
    for (int i = 0; i < CTL_TIMEOUT; i++) {
        if (!(inb(PS2_STATUS) & STR_IBF))
            return 0;
        udelay50();
    }
    return -1;
}

static int i8042_wait_read(void)
{
    for (int i = 0; i < CTL_TIMEOUT; i++) {
        if (inb(PS2_STATUS) & STR_OBF)
            return 0;
        udelay50();
    }
    return -1;
}

static int i8042_flush(void)
{
    int count = 0;
    while (inb(PS2_STATUS) & STR_OBF) {
        if (count++ >= BUFFER_SIZE)
            return -1;
        udelay50();
        (void)inb(PS2_DATA);
    }
    return 0;
}

static int i8042_command(uint8_t *param, int command)
{
    int error;

    error = i8042_wait_write();
    if (error) return error;

    outb(PS2_CMD, command & 0xff);

    for (int i = 0; i < ((command >> 12) & 0xf); i++) {
        error = i8042_wait_write();
        if (error) return error;
        outb(PS2_DATA, param[i]);
    }

    for (int i = 0; i < ((command >> 8) & 0xf); i++) {
        error = i8042_wait_read();
        if (error) return error;
        param[i] = inb(PS2_DATA);
    }

    return 0;
}

int ps2_init(void)
{
    kprintf("PS2: initializing controller...\n");

    i8042_flush();

    uint8_t status = inb(PS2_STATUS);
    if (status == 0xFF) {
        kprintf("PS2: no controller found\n");
        return -1;
    }
    kprintf("PS2: status=0x%02x\n", status);

    // Read CTR twice for stability
    uint8_t ctr[2];
    int n = 0;
    do {
        if (n >= 10) return -1;
        if (n != 0) udelay50();
        if (i8042_command(&ctr[n % 2], CMD_CTL_RCTR))
            return -1;
        n++;
    } while (n < 2 || ctr[0] != ctr[1]);

    uint8_t i8042_ctr = ctr[0];
    kprintf("PS2: CTR=0x%02x\n", i8042_ctr);

    // Disable ports + interrupts during setup, force translation ON
    i8042_ctr |= (CTR_KBDDIS | CTR_AUXDIS);
    i8042_ctr &= ~(CTR_KBDINT | CTR_AUXINT);
    i8042_ctr |= CTR_XLATE;
    if (i8042_command(&i8042_ctr, CMD_CTL_WCTR))
        return -1;

    i8042_flush();

    // Bring up the keyboard path only here. Leave AUX disabled until
    // mouse_init() completes its polled reset/protocol sequence so real
    // movement cannot race the mouse command/ACK exchange during boot.
    i8042_ctr &= ~CTR_KBDDIS;
    i8042_ctr |= CTR_KBDINT;
    i8042_ctr |= CTR_AUXDIS;
    i8042_ctr &= ~CTR_AUXINT;
    if (i8042_command(&i8042_ctr, CMD_CTL_WCTR))
        return -1;

    // IOAPIC routing for keyboard and mouse
    if (ioapic_configure_legacy_irq(1, KBD_VECTOR,
                                     IOAPIC_POLARITY_HIGH,
                                     IOAPIC_TRIGGER_EDGE) == 0) {
        irq_disable(1);
    } else {
        irq_enable(1);
    }

    if (ioapic_configure_legacy_irq(12, AUX_VECTOR,
                                     IOAPIC_POLARITY_HIGH,
                                     IOAPIC_TRIGGER_EDGE) == 0) {
        irq_disable(12);
    } else {
        irq_enable(12);
    }

    i8042_flush();

    kprintf("PS2: initialized (CTR=0x%02x)\n", i8042_ctr);
    return 0;
}
