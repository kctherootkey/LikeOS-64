// LikeOS-64 - Minimal IOAPIC support (legacy IRQ routing)
#pragma once
typedef unsigned char uint8_t; 
typedef unsigned int  uint32_t;

// Polarity flags
#define IOAPIC_POLARITY_HIGH 0
#define IOAPIC_POLARITY_LOW  1

// Trigger mode flags
#define IOAPIC_TRIGGER_EDGE  0
#define IOAPIC_TRIGGER_LEVEL 1

// Configure a legacy IRQ (GSI) redirection
// gsi: global system interrupt (legacy PIC IRQ number typically)
// vector: IDT vector to deliver
int ioapic_configure_legacy_irq(uint8_t gsi, uint8_t vector, uint8_t polarity, uint8_t trigger_mode);
int ioapic_detect(void); // returns 0 if present
