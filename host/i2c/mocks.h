/* The I2C harness's kernel service models (host/i2c/mocks.c).
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_I2C_MOCKS_H
#define HOST_I2C_MOCKS_H

#include <kernel/hal/pci.h>

struct dw_model;
struct devfs_node;

/* User pointers in [MOCK_BAD_USER, +64K) and NULL fault. */
#define MOCK_BAD_USER 0xbad00000ul

/* A modelled PCI function. */
struct mock_pci {
	pci_device_t dev; /* first */
	uint16_t vendor, device_id;
	uint8_t slot, func;
	struct dw_model *m;
	uint64_t bar; /* 0: unassigned */
	uint16_t cmd;
	int msi;
	int irq;
	int decode_stuck_off;
	int pstate;
	int mapped;
	int probe_ret;
};

void mock_pci_add(struct mock_pci *mp);
void mock_pci_reset(void);

/* GpioInt interrupts by device id: `defer' makes the lookup answer
 * -EPROBE_DEFER (the GPIO controller missing). */
struct mock_gpioint {
	char hid[16];
	int defer;
	int wake;
	int irq;
	int calls;
};
extern struct mock_gpioint g_mock_gpioint[8];
/* Recovery GPIOs (_DSD scl/sda-gpios) are wired to this model. */
extern struct dw_model *g_mock_recovery_model;
extern int g_mock_recovery_defer;
extern int g_mock_devreg_fail;

int mock_irq_new(struct dw_model *m);
void mock_gsi_bind(int gsi, struct dw_model *m);
void mock_irq_set_ext(int irq, int level);
/* The level of a GSI line driven by a device model (not a controller). */
void mock_gsi_set_level(int gsi, int level);
/* Threaded-handler wake-ups of a line so far, and its disable depth. */
unsigned long mock_irq_threaded(int irq);
int mock_irq_depth(int irq);
unsigned long mock_irq_count(int irq, unsigned long *none);
int mock_irq_handlers(int irq);
int mock_irqs_in_use(void);
struct devfs_node *mock_devfs_find(const char *path);

#endif
