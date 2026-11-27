// LikeOS GPIO - general-purpose I/O lines and their interrupts
//
// A GPIO controller (a "chip": the pin controller of an Intel PCH, an AMD
// FCH, a Bay Trail SoC) registers a struct gpio_chip with operations to
// read, drive and configure its lines.  Consumers -- a touchpad driver
// that needs its interrupt line, firmware AML that toggles a reset pin --
// never see the controller: they get a line descriptor (struct gpio_desc)
// by firmware description (gpiolib-acpi.c: the GpioInt/GpioIo resources
// of an ACPI device) or by number, request it, and use the consumer API.
//
// Numbering.  Every chip covers `ngpio' lines, numbered 0..ngpio-1 within
// the chip; those are the numbers ACPI GpioInt/GpioIo descriptors use (the
// pin numbers of the controller named in their ResourceSource).  A chip
// also gets a base in one global number space (gpio_to_desc), assigned at
// registration in registration order, for diagnostics.  A chip may leave
// holes in its numbering (`valid' returns 0 there): requesting them fails.
//
// Interrupts.  A chip with irq operations is also an interrupt controller
// behind one parent interrupt (its own I/O APIC line, usually shared).
// Each line that is used as an interrupt gets an interrupt number of the
// kernel's interrupt core (a virtual descriptor, kernel/ke/irq_internal.h),
// created when a consumer first asks (gpiod_to_irq), so the consumer uses
// request_threaded_irq(), disable_irq(), enable_irq_wake() and
// irq_set_irq_type() on it like on any other interrupt.  The chip's parent
// handler finds the lines whose status is pending and calls
// gpiochip_handle_irq() for each; gpiolib runs the flow:
//
//   * edge lines: acknowledge (clear the latched status), then run the
//     handlers.  An edge arriving while the interrupt is disabled is
//     replayed from process context when it is enabled again;
//   * level lines: mask and acknowledge, run the handlers, unmask unless
//     the interrupt core keeps the line masked (disable_irq, or a oneshot
//     thread that has not finished: the line stays masked until the thread
//     function returns, so a level that the device holds asserted until
//     the thread has talked to it cannot storm).
//
// The line's hardware mask is the OR of two reasons -- the interrupt
// core's (mask/unmask operations) and the level flow's -- kept by gpiolib
// so the two never undo each other.
//
// Locking.  Chip operations are atomic (MMIO under the chip's own
// spinlock; no sleeping chips exist here) and may be called from any
// context.  Request/free and the interrupt mapping are process context.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_GPIO_GPIOLIB_H
#define KERNEL_DEV_GPIO_GPIOLIB_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/mutex.h>
#include <kernel/ke/workqueue.h>
#include <kernel/ke/list.h>

#ifndef EPROBE_DEFER
#define EPROBE_DEFER 517 /* a resource's provider is not there yet */
#endif
#ifndef ENOTSUPP
#define ENOTSUPP 524 /* operation not supported by this hardware */
#endif

struct acpi_device;
struct gpio_chip;
struct gpio_desc;

#define GPIO_LINE_DIRECTION_IN 1
#define GPIO_LINE_DIRECTION_OUT 0

/* Pin configuration for set_config (`arg' as noted). */
enum pin_config_param {
	PIN_CONFIG_BIAS_DISABLE = 1, /* arg ignored */
	PIN_CONFIG_BIAS_PULL_UP, /* arg: ohms, 1 = the chip's default */
	PIN_CONFIG_BIAS_PULL_DOWN, /* arg: ohms, 1 = the chip's default */
	PIN_CONFIG_INPUT_DEBOUNCE, /* arg: microseconds, 0 = off */
	PIN_CONFIG_DRIVE_OPEN_DRAIN, /* arg ignored */
	PIN_CONFIG_DRIVE_PUSH_PULL, /* arg ignored */
	PIN_CONFIG_DRIVE_OPEN_SOURCE, /* arg ignored */
	PIN_CONFIG_DRIVE_STRENGTH, /* arg: mA */
	PIN_CONFIG_BIAS_HIGH_IMPEDANCE, /* arg ignored: input and output off */
};

/* Interrupt operations of a chip (`hwirq' is the line offset).  Called
 * with interrupts off and gpiolib's per-chip interrupt lock held; must
 * not sleep. */
struct gpio_irq_chip_ops {
	void (*irq_mask)(struct gpio_chip *gc, unsigned int hwirq);
	void (*irq_unmask)(struct gpio_chip *gc, unsigned int hwirq);
	/* Clear the latched status of the line. */
	void (*irq_ack)(struct gpio_chip *gc, unsigned int hwirq);
	/* Program the trigger (IRQ_TYPE_*, kernel/ke/irq.h). */
	int (*irq_set_type)(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int type);
	/* Optional: arm/disarm the line as a wake source. */
	int (*irq_set_wake)(struct gpio_chip *gc, unsigned int hwirq,
			    unsigned int on);
	/* Optional: may the line be used as an interrupt at all (pad
	 * routed to the host, not to firmware); 0 or -errno. */
	int (*irq_request)(struct gpio_chip *gc, unsigned int hwirq);
};

/* Per-line state (gpiolib's).  Consumers use it only as an opaque
 * struct gpio_desc. */
struct gpio_line;

struct gpio_chip {
	/* ---- filled by the driver ---- */
	const char *label;
	void *acpi_handle; /* the controller's namespace device, or NULL */
	unsigned int ngpio;
	void *data; /* the driver's */
	/* 0 or -errno; the line is the caller's after a 0. */
	int (*request)(struct gpio_chip *gc, unsigned int offset);
	void (*free)(struct gpio_chip *gc, unsigned int offset);
	/* GPIO_LINE_DIRECTION_IN/OUT or -errno. */
	int (*get_direction)(struct gpio_chip *gc, unsigned int offset);
	int (*direction_input)(struct gpio_chip *gc, unsigned int offset);
	int (*direction_output)(struct gpio_chip *gc, unsigned int offset,
				int value);
	/* The raw level: 0/1 or -errno. */
	int (*get)(struct gpio_chip *gc, unsigned int offset);
	void (*set)(struct gpio_chip *gc, unsigned int offset, int value);
	/* 0, -ENOTSUPP (the chip cannot), -EINVAL (bad argument). */
	int (*set_config)(struct gpio_chip *gc, unsigned int offset,
			  enum pin_config_param param, uint32_t arg);
	/* NULL: every line exists. */
	int (*valid)(struct gpio_chip *gc, unsigned int offset);
	/* Optional: a one-line description of the line's hardware state
	 * (pad configuration) for the diagnostics dump. */
	void (*describe)(struct gpio_chip *gc, unsigned int offset, char *buf,
			 size_t len);
	const struct gpio_irq_chip_ops *irq; /* NULL: no interrupts */

	/* ---- gpiolib's ---- */
	int base; /* global number of line 0 */
	struct gpio_line *lines;
	spinlock_t lock; /* line flags and labels */
	spinlock_t irq_lock; /* line interrupt masks, under desc->lock */
	struct mutex irq_map_mutex; /* creating interrupt descriptors */
	struct work_struct resend_work; /* replay of lost edges */
	struct list_head node; /* the registry */
	void *acpi_data; /* gpiolib-acpi.c's per-chip state */
	int registered;
};

/* ---- providers ------------------------------------------------------------ */

/* Register a chip; it must stay valid until gpiochip_remove().  Also
 * hooks the chip into ACPI (operation region pins, the controller's
 * readiness for _DEP).  0 or -errno.  May sleep. */
int gpiochip_add(struct gpio_chip *gc);
/* Unregister.  Lines still requested or used as interrupts are a bug
 * (warned; their descriptors are released).  May sleep. */
void gpiochip_remove(struct gpio_chip *gc);
/* The chip's parent interrupt handler found `offset' pending: run the
 * line's flow (hard interrupt context).  A pending line that nobody
 * mapped is masked and acknowledged. */
void gpiochip_handle_irq(struct gpio_chip *gc, unsigned int offset);
/* The interrupt number of a line, creating it on first use (process
 * context then); -errno. */
int gpiochip_irq_map(struct gpio_chip *gc, unsigned int offset);
/* The interrupt number already mapped for a line, 0 when none. */
int gpiochip_irq_lookup(struct gpio_chip *gc, unsigned int offset);
/* Is the line in use as an interrupt right now. */
int gpiochip_line_is_irq(struct gpio_chip *gc, unsigned int offset);
/* Has a consumer requested the line. */
int gpiochip_line_is_requested(struct gpio_chip *gc, unsigned int offset);
/* The controller's interrupt enables were written back from a saved copy
 * (system resume): put every mapped line's mask back the way gpiolib
 * holds it, so the hardware and the cached state agree again (a line
 * masked or unmasked between the save and the restore).  Process
 * context. */
void gpiochip_irq_resync_masks(struct gpio_chip *gc);

/* ---- lookup ----------------------------------------------------------------- */

/* The registered chip of a controller device, or NULL. */
struct gpio_chip *gpiochip_find_by_acpi_handle(void *handle);
struct gpio_desc *gpiochip_get_desc(struct gpio_chip *gc,
				    unsigned int offset);
struct gpio_desc *gpio_to_desc(unsigned int gpio);
int desc_to_gpio(const struct gpio_desc *desc);
struct gpio_chip *gpiod_to_chip(const struct gpio_desc *desc);
unsigned int gpiod_offset(const struct gpio_desc *desc);

/* ---- consumers -------------------------------------------------------------- */

/* Initial configuration at request time. */
enum gpiod_flags {
	GPIOD_ASIS = 0, /* leave direction and level */
	GPIOD_IN = 1,
	GPIOD_OUT_LOW = 2, /* logical low */
	GPIOD_OUT_HIGH = 3, /* logical high */
};

/* Lookup flags (from firmware): polarity and pulls. */
#define GPIO_LOOKUP_ACTIVE_LOW (1u << 0)
#define GPIO_LOOKUP_PULL_UP (1u << 1)
#define GPIO_LOOKUP_PULL_DOWN (1u << 2)
#define GPIO_LOOKUP_PULL_DISABLE (1u << 3)
#define GPIO_LOOKUP_OPEN_DRAIN (1u << 4)

/* Take the line for `label': -EBUSY when somebody has it, -EINVAL for a
 * hole in the chip's numbering, the chip's error.  May sleep. */
int gpiod_request(struct gpio_desc *desc, const char *label);
void gpiod_free(struct gpio_desc *desc);
/* Request and configure (lookup flags, then direction/level). */
int gpiod_request_config(struct gpio_desc *desc, const char *label,
			 unsigned int lookup_flags, enum gpiod_flags flags);
/* Apply lookup flags and gpiod_flags to a requested (or interrupt-only)
 * line. */
int gpiod_configure_flags(struct gpio_desc *desc, unsigned int lookup_flags,
			  enum gpiod_flags flags);
int gpiod_set_consumer_name(struct gpio_desc *desc, const char *name);

int gpiod_get_direction(struct gpio_desc *desc);
int gpiod_direction_input(struct gpio_desc *desc);
/* `value' is logical (active-low lines are inverted); _raw is the level. */
int gpiod_direction_output(struct gpio_desc *desc, int value);
int gpiod_direction_output_raw(struct gpio_desc *desc, int value);
int gpiod_get_value(const struct gpio_desc *desc);
int gpiod_get_raw_value(const struct gpio_desc *desc);
void gpiod_set_value(struct gpio_desc *desc, int value);
void gpiod_set_raw_value(struct gpio_desc *desc, int value);
/* The same (no chip here sleeps); named for callers that may. */
#define gpiod_get_value_cansleep gpiod_get_value
#define gpiod_get_raw_value_cansleep gpiod_get_raw_value
#define gpiod_set_value_cansleep gpiod_set_value
#define gpiod_set_raw_value_cansleep gpiod_set_raw_value
int gpiod_is_active_low(const struct gpio_desc *desc);
void gpiod_toggle_active_low(struct gpio_desc *desc);
/* Debounce in microseconds (0 off): 0, -ENOTSUPP, -EINVAL. */
int gpiod_set_debounce(struct gpio_desc *desc, unsigned int usec);
int gpiod_set_config(struct gpio_desc *desc, enum pin_config_param param,
		     uint32_t arg);
/* The interrupt number of the line (created on first use; process
 * context), or -errno. */
int gpiod_to_irq(const struct gpio_desc *desc);

/* ---- diagnostics ------------------------------------------------------------ */

/* Every chip and every requested/interrupt line, as text (a /proc-style
 * show function): bytes written. */
long gpiolib_show(char *buf, long cap);
/* The same into the kernel log. */
void gpiolib_dump(void);

/* ---- ACPI (gpiolib-acpi.c) ---------------------------------------------------- */

/* What a GPIO resource said about its line. */
struct acpi_gpio_info {
	struct acpi_device *adev;
	int gpioint; /* a GpioInt (else GpioIo) */
	uint8_t triggering; /* ACPI_RES_LEVEL / EDGE */
	uint8_t polarity; /* ACPI_RES_ACTIVE_* */
	uint8_t pin_config; /* ACPI_RES_PULL_* */
	uint8_t io_restriction;
	uint8_t wake_capable;
	uint16_t debounce; /* hundredths of a millisecond */
	unsigned int lookup_flags; /* GPIO_LOOKUP_* derived */
	enum gpiod_flags flags; /* direction derived */
};

/* The line described by the `index'-th GPIO resource (GpioInt and GpioIo
 * counted together, in _CRS order; its first pin).  0 with *desc set,
 * -ENOENT (no such resource), -EPROBE_DEFER (its controller has not
 * registered yet), -EINVAL.  May sleep. */
int acpi_get_gpiod_by_index(struct acpi_device *adev, int index,
			    struct gpio_desc **desc,
			    struct acpi_gpio_info *info);
/* The interrupt of the `index'-th GpioInt resource of the device: the
 * line is configured as input with the resource's pull and debounce, the
 * interrupt's trigger is programmed from the resource unless the
 * interrupt is in use already, and the interrupt number is returned
 * (> 0) -- or -ENOENT, -EPROBE_DEFER, -errno.  *wake_capable (may be
 * NULL) says whether firmware marked it a wake source.  May sleep. */
int acpi_dev_gpio_irq_wake_get(struct acpi_device *adev, int index,
			       int *wake_capable);
static inline int acpi_dev_gpio_irq_get(struct acpi_device *adev, int index)
{
	return acpi_dev_gpio_irq_wake_get(adev, index, NULL);
}
/* Request the line of the `index'-th GPIO resource for `label' and set it
 * up as the resource says (GpioIo: IoRestriction, pull, polarity), with
 * `flags' overriding the direction when not GPIOD_ASIS.  May sleep. */
int acpi_dev_gpiod_get_index(struct acpi_device *adev, int index,
			     const char *label, enum gpiod_flags flags,
			     struct gpio_desc **desc);
/* Pins accessed by AML through a GeneralPurposeIo operation region:
 * `pin_index'/`bits' select pins of the Connection() descriptor
 * `conn'/`conn_len' (raw AML resource template).  0, or -errno (no
 * controller registered: -ENODEV) -- the caller falls back to its stub
 * behaviour on error.  Process context (AML). */
int acpi_gpio_opregion_access(int write, uint32_t pin_index, uint32_t bits,
			      uint64_t *value, const uint8_t *conn,
			      uint16_t conn_len);
/* Chip registration hooks (gpiolib.c calls them). */
void acpi_gpiochip_add(struct gpio_chip *gc);
void acpi_gpiochip_remove(struct gpio_chip *gc);
/* ACPI GPIO-signalled events (_AEI with _Exx/_Lxx/_EVT): armed only when
 * enabled (gpiolib_acpi_events_enable(1) before the chips register, or
 * acpi_gpiochip_request_interrupts() afterwards).  Off by default: a
 * platform whose embedded controller signals through _AEI would start
 * running event AML that has not run on this system before. */
void gpiolib_acpi_events_enable(int on);
int acpi_gpiochip_request_interrupts(struct gpio_chip *gc);
void acpi_gpiochip_free_interrupts(struct gpio_chip *gc);

/* Probe every GPIO/pin controller driver (Intel, Bay Trail, Cherry View,
 * AMD) against the ACPI namespace.  Once, after ACPI, PCI and the
 * interrupt core are up and before the drivers that need GPIO interrupts
 * (I2C clients).  Process context with interrupts on. */
void gpio_pinctrl_init(void);
/* System sleep of every GPIO/pin controller: pad configuration and
 * interrupt enables saved (and wake pads left armed) on suspend, restored
 * on resume.  Suspend runs after every device that uses a GPIO line went
 * down, resume before any comes back.  Process context. */
void gpio_pinctrl_suspend_all(void);
void gpio_pinctrl_resume_all(void);

#endif /* KERNEL_DEV_GPIO_GPIOLIB_H */
