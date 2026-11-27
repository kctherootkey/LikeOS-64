/* Bus-world internals of the I2C-HID harness shared by kworld.c and
 * hidm.c (host/test-i2chid.sh).  Bus world only.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2CHID_KWORLD_INT_H
#define HOST_I2CHID_KWORLD_INT_H

struct dw_model;
struct kw_cfg;

/* Put the device model on the bus at `addr', its HID descriptor at
 * `desc_reg' (the other registers follow it), its line on interrupt `irq'
 * (> 0) or GSI `gsi' (>= 0). */
void hidm_attach(struct dw_model *m, uint16_t addr, const struct kw_cfg *cfg,
		 uint16_t desc_reg, int irq, int gsi);
void hidm_set_irq(int irq);
void hidm_detach(void);
void hidm_tick(void);

#endif
