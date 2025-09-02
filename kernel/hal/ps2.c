// LikeOS-64 - PS/2 Controller (8042) Initialization
#include "../../include/kernel/console.h"
#include "../../include/kernel/ps2.h"
#include "../../include/kernel/interrupt.h" // for inb/outb

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

// Status bits
#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02

static int wait_input_clear(void) {
    for (int i=0; i<50000; ++i) {
        if ((inb(PS2_STATUS) & PS2_STATUS_IN_FULL) == 0)
            return 0;
    }
    return -1;
}
static int wait_output_full(void) {
    for (int i=0; i<50000; ++i) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT_FULL)
            return 0;
    }
    return -1;
}
static void flush_output(void) {
    for (int i=0; i<16; ++i) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT_FULL) {
            (void)inb(PS2_DATA);
        } else break;
    }
}
static int write_cmd(uint8_t cmd) {
    if (wait_input_clear() != 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}
static int write_data(uint8_t data) {
    if (wait_input_clear() != 0) return -1;
    outb(PS2_DATA, data);
    return 0;
}
static int read_data(uint8_t *out) {
    if (wait_output_full() != 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

int ps2_init(void) {
    kprintf("PS2: initializing controller...\n");
    // Disable both ports
    write_cmd(0xAD); // disable first
    write_cmd(0xA7); // disable second
    flush_output();
    // Attempt to read config byte (retry once if first attempt fails)
    uint8_t cfg=0; int attempt=0; int rc;
    for(attempt=0; attempt<2; ++attempt) {
        rc = write_cmd(0x20);
        if(rc==0 && read_data(&cfg)==0) break;
        // brief delay before retry
        for(volatile int d=0; d<100000; ++d) { __asm__ __volatile__("nop"); }
    }
    if(attempt==2) {
        kprintf("PS2: controller not present (cfg read failed)\n");
        return -1;
    }
    uint8_t original_cfg = cfg;
    cfg &= ~0x03; // clear IRQ1, IRQ12 enable bits
    cfg &= ~(1<<6); // clear translation during tests
    // Write config
    write_cmd(0x60);
    write_data(cfg);
    // Controller self-test
    write_cmd(0xAA);
    uint8_t self=0;
    if (read_data(&self)!=0 || self!=0x55) { kprintf("PS2: self-test failed (0x%02x)\n", self); }
    // Test first port
    write_cmd(0xAB);
    uint8_t t1=0xFF; read_data(&t1);
    if (t1!=0x00) { kprintf("PS2: first port test failed (0x%02x)\n", t1); }
    // Re-enable first port
    write_cmd(0xAE);
    // Restore config with IRQ1 on, keep translation disabled (or enable if bit needed)
    cfg = original_cfg | 0x01; // enable first port IRQ
    write_cmd(0x60);
    write_data(cfg);
    // Enable scanning (send 0xF4 to device)
    write_data(0xF4);
    uint8_t ack=0; read_data(&ack); // expect 0xFA
    kprintf("PS2: initialized (ack=0x%02x, cfg=0x%02x)\n", ack, cfg);
    return 0;
}
