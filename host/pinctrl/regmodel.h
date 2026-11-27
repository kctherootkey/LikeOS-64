/* Register model of the GPIO controllers for host/test-pinctrl.sh.
 *
 * The drivers' register accessors (pinctrl_readl/pinctrl_writel) are
 * routed here.  A window is a block of host memory standing for a
 * controller's MMIO; each dword has an access type -- plain, read-only,
 * write-1-to-clear, or an AMD pin register (status bits write-1-to-clear,
 * the rest plain) -- and every access is logged, so a test can tell what
 * the driver wrote where.  Pad inputs are modelled for the Intel
 * community design: a test drives a pad's input level, the model updates
 * the pad's RX state and latches GPI_IS by the pad's RXEVCFG/RXINV (a
 * level keeps latching while active, as the hardware does).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_PINCTRL_REGMODEL_H
#define HOST_PINCTRL_REGMODEL_H

#include <kernel/uapi/types.h>

uint32_t model_readl(const volatile void *addr);
void model_writel(uint32_t v, volatile void *addr);

#define RM_RW 0
#define RM_RO 1
#define RM_W1C 2
#define RM_AMDPIN 3

void rm_reset(void);
/* A window of `size' bytes (zeroed, page aligned). */
uint8_t *rm_window(size_t size);
void rm_set_attr(void *addr, int attr);
void rm_set_attr_range(void *addr, size_t bytes, int attr);
/* Without semantics or logging. */
uint32_t rm_peek(const void *addr);
void rm_poke(void *addr, uint32_t v);
/* Write log. */
void rm_log_clear(void);
unsigned long rm_writes_in(const void *addr, size_t bytes);
unsigned long rm_reads(void);

/* Intel pads: register the pad's PADCFG0 and its GPI_IS register/bit. */
int rm_intel_pad(void *padcfg0, void *is_reg, uint32_t bit);
/* Drive the pad's input level (0/1). */
void rm_intel_drive(int pad, int level);

/* AMD pins: the pin register; level input and edge events. */
void rm_amd_set_base(uint8_t *base);
void rm_amd_drive(unsigned int pin, int level);

#endif /* HOST_PINCTRL_REGMODEL_H */
