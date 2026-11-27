// LikeOS GPIO - chip registry, line descriptors and the GPIO interrupt flow
//
// See gpiolib.h for the model.  Implementation notes:
//
// Registry.  Chips are kept on one list under g_chips_mutex for
// registration and under g_chips_lock (a spinlock) for lookups that may
// come from any context (gpio_to_desc).  The global numbers are handed
// out in registration order and never reused while a chip is registered.
//
// Lines.  struct gpio_line is the descriptor: its flags (requested,
// output, active-low, used as interrupt, pulls) and label are under the
// chip's `lock'.  Chip operations are called without gpiolib locks held:
// they are atomic and serialise on the controller's own lock.
//
// Interrupts.  A line's interrupt number is a virtual descriptor of the
// interrupt core with gpiolib's irq_chip_ops; its chip data is the line.
// The interrupt core calls mask/unmask/set_type/set_wake/retrigger with the
// descriptor's lock held and interrupts off; gpiolib forwards them to the
// chip under the chip's `irq_lock' (lock order: descriptor lock, then the
// chip's irq_lock, then the controller's own register lock).  The flow
// handler (gpiochip_handle_irq) takes irq_lock only around the hardware
// mask/ack and never holds it while running the handlers, which take the
// descriptor lock.
//
// Hardware mask = core reason | flow reason.  The core masks on
// disable_irq and while a oneshot thread runs; the level flow masks
// around the hard handlers.  Only a change of the OR touches the chip.
//
// Lost edges.  An edge that arrives while the interrupt is disabled has
// already been acknowledged (its status cleared) by the time the core
// finds it disabled; the core marks it pending and calls retrigger when
// the interrupt is enabled.  gpiolib then replays it from a work item:
// the handlers run with interrupts off, as they would from the parent
// interrupt.  Level lines need no replay -- the controller latches the
// level again as soon as the line is unmasked.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/irq_internal.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define GPIO_LABEL_LEN 32

/* Line flags (chip->lock). */
#define GPIOD_FL_REQUESTED (1u << 0)
#define GPIOD_FL_IS_OUT (1u << 1)
#define GPIOD_FL_ACTIVE_LOW (1u << 2)
#define GPIOD_FL_USED_AS_IRQ (1u << 3)
#define GPIOD_FL_PULL_UP (1u << 4)
#define GPIOD_FL_PULL_DOWN (1u << 5)
#define GPIOD_FL_PULL_DISABLE (1u << 6)
#define GPIOD_FL_OPEN_DRAIN (1u << 7)

/* Mask reasons (chip->irq_lock). */
#define GPIO_MASK_CORE (1u << 0)
#define GPIO_MASK_FLOW (1u << 1)

struct gpio_line {
	struct gpio_chip *gc;
	unsigned int offset;
	uint32_t flags;
	char label[GPIO_LABEL_LEN];
	/* interrupt */
	int irq; /* 0: not mapped */
	unsigned int irq_type; /* IRQ_TYPE_* as programmed */
	uint8_t mask_reasons;
	uint8_t hw_masked;
	volatile uint8_t sw_pending; /* an edge to replay */
	uint64_t irq_events;
};

static DEFINE_MUTEX(g_chips_mutex);
static spinlock_t g_chips_lock = SPINLOCK_INIT("gpio_chips");
static struct list_head g_chips = LIST_HEAD_INIT(g_chips);
static int g_next_base;

/* ==========================================================================
 * Helpers
 * ========================================================================== */

static inline struct gpio_line *desc_line(const struct gpio_desc *desc)
{
	return (struct gpio_line *)desc;
}

static int line_valid(struct gpio_chip *gc, unsigned int offset)
{
	if (offset >= gc->ngpio)
		return 0;
	return gc->valid ? gc->valid(gc, offset) : 1;
}

static void label_set(char *dst, const char *src)
{
	size_t i = 0;

	if (src)
		for (; src[i] && i + 1 < GPIO_LABEL_LEN; i++)
			dst[i] = src[i];
	dst[i] = '\0';
}

static uint32_t line_flags(struct gpio_line *l)
{
	uint64_t fl;
	uint32_t f;

	spin_lock_irqsave(&l->gc->lock, &fl);
	f = l->flags;
	spin_unlock_irqrestore(&l->gc->lock, fl);
	return f;
}

static void line_flags_update(struct gpio_line *l, uint32_t set,
			      uint32_t clear)
{
	uint64_t fl;

	spin_lock_irqsave(&l->gc->lock, &fl);
	l->flags = (l->flags & ~clear) | set;
	spin_unlock_irqrestore(&l->gc->lock, fl);
}

/* ==========================================================================
 * Registry
 * ========================================================================== */

static void gpio_resend_work(struct work_struct *work);

int gpiochip_add(struct gpio_chip *gc)
{
	struct gpio_line *lines;
	uint64_t fl;

	might_sleep();
	if (!gc || gc->ngpio == 0 || gc->ngpio > 65535 || !gc->get)
		return -EINVAL;
	if (gc->irq && (!gc->irq->irq_mask || !gc->irq->irq_unmask ||
			!gc->irq->irq_ack || !gc->irq->irq_set_type))
		return -EINVAL;
	lines = kcalloc(gc->ngpio, sizeof(*lines));
	if (!lines)
		return -ENOMEM;
	for (unsigned int i = 0; i < gc->ngpio; i++) {
		lines[i].gc = gc;
		lines[i].offset = i;
		/* Interrupts start masked (the chip masks them all at
		 * probe; gpiolib's view agrees). */
		lines[i].mask_reasons = GPIO_MASK_CORE;
		lines[i].hw_masked = 1;
	}
	gc->lines = lines;
	spinlock_init(&gc->lock, "gpio_chip");
	spinlock_init(&gc->irq_lock, "gpio_irq");
	mutex_init(&gc->irq_map_mutex, "gpio_irq_map");
	INIT_WORK(&gc->resend_work, gpio_resend_work);
	INIT_LIST_HEAD(&gc->node);
	gc->acpi_data = NULL;

	mutex_lock(&g_chips_mutex);
	gc->base = g_next_base;
	g_next_base += (int)gc->ngpio;
	spin_lock_irqsave(&g_chips_lock, &fl);
	list_add_tail(&gc->node, &g_chips);
	gc->registered = 1;
	spin_unlock_irqrestore(&g_chips_lock, fl);
	mutex_unlock(&g_chips_mutex);

	kprintf("gpio: %s: %u lines (global %d-%d)%s\n",
		gc->label ? gc->label : "chip", gc->ngpio, gc->base,
		gc->base + (int)gc->ngpio - 1, gc->irq ? ", interrupts" : "");
	acpi_gpiochip_add(gc);
	return 0;
}

void gpiochip_remove(struct gpio_chip *gc)
{
	uint64_t fl;

	might_sleep();
	if (!gc || !gc->registered)
		return;
	acpi_gpiochip_remove(gc);
	mutex_lock(&g_chips_mutex);
	spin_lock_irqsave(&g_chips_lock, &fl);
	list_del(&gc->node);
	gc->registered = 0;
	spin_unlock_irqrestore(&g_chips_lock, fl);
	mutex_unlock(&g_chips_mutex);
	(void)cancel_work_sync(&gc->resend_work);
	for (unsigned int i = 0; i < gc->ngpio; i++) {
		struct gpio_line *l = &gc->lines[i];

		WARN(l->flags & (GPIOD_FL_REQUESTED | GPIOD_FL_USED_AS_IRQ),
		     "gpio: %s line %u still in use (%s) at removal",
		     gc->label ? gc->label : "chip", i, l->label);
		if (l->irq > 0) {
			irq_free_desc(l->irq);
			l->irq = 0;
		}
	}
	kfree(gc->lines);
	gc->lines = NULL;
}

struct gpio_chip *gpiochip_find_by_acpi_handle(void *handle)
{
	struct gpio_chip *found = NULL;
	struct list_head *p;
	uint64_t fl;

	if (!handle)
		return NULL;
	spin_lock_irqsave(&g_chips_lock, &fl);
	for (p = g_chips.next; p != &g_chips; p = p->next) {
		struct gpio_chip *gc = container_of(p, struct gpio_chip, node);

		if (gc->acpi_handle == handle) {
			found = gc;
			break;
		}
	}
	spin_unlock_irqrestore(&g_chips_lock, fl);
	return found;
}

struct gpio_desc *gpiochip_get_desc(struct gpio_chip *gc, unsigned int offset)
{
	if (!gc || !gc->lines || offset >= gc->ngpio)
		return NULL;
	return (struct gpio_desc *)&gc->lines[offset];
}

struct gpio_desc *gpio_to_desc(unsigned int gpio)
{
	struct gpio_desc *d = NULL;
	struct list_head *p;
	uint64_t fl;

	spin_lock_irqsave(&g_chips_lock, &fl);
	for (p = g_chips.next; p != &g_chips; p = p->next) {
		struct gpio_chip *gc = container_of(p, struct gpio_chip, node);

		if ((int)gpio >= gc->base &&
		    (int)gpio < gc->base + (int)gc->ngpio) {
			d = (struct gpio_desc *)&gc->lines[gpio - (unsigned)gc->base];
			break;
		}
	}
	spin_unlock_irqrestore(&g_chips_lock, fl);
	return d;
}

int desc_to_gpio(const struct gpio_desc *desc)
{
	const struct gpio_line *l = (const struct gpio_line *)desc;

	if (!l)
		return -EINVAL;
	return l->gc->base + (int)l->offset;
}

struct gpio_chip *gpiod_to_chip(const struct gpio_desc *desc)
{
	return desc ? ((const struct gpio_line *)desc)->gc : NULL;
}

unsigned int gpiod_offset(const struct gpio_desc *desc)
{
	return desc ? ((const struct gpio_line *)desc)->offset : 0;
}

/* ==========================================================================
 * Consumer API
 * ========================================================================== */

int gpiod_request(struct gpio_desc *desc, const char *label)
{
	struct gpio_line *l = desc_line(desc);
	struct gpio_chip *gc;
	uint64_t fl;
	int rc, dir;

	might_sleep();
	if (!l)
		return -EINVAL;
	gc = l->gc;
	if (!line_valid(gc, l->offset))
		return -EINVAL;
	spin_lock_irqsave(&gc->lock, &fl);
	if (l->flags & GPIOD_FL_REQUESTED) {
		spin_unlock_irqrestore(&gc->lock, fl);
		return -EBUSY;
	}
	l->flags |= GPIOD_FL_REQUESTED;
	label_set(l->label, label ? label : "?");
	spin_unlock_irqrestore(&gc->lock, fl);
	if (gc->request) {
		rc = gc->request(gc, l->offset);
		if (rc) {
			line_flags_update(l, 0, GPIOD_FL_REQUESTED);
			return rc;
		}
	}
	/* Learn the direction the line is in. */
	if (gc->get_direction) {
		dir = gc->get_direction(gc, l->offset);
		if (dir == GPIO_LINE_DIRECTION_OUT)
			line_flags_update(l, GPIOD_FL_IS_OUT, 0);
		else if (dir == GPIO_LINE_DIRECTION_IN)
			line_flags_update(l, 0, GPIOD_FL_IS_OUT);
	}
	return 0;
}

void gpiod_free(struct gpio_desc *desc)
{
	struct gpio_line *l = desc_line(desc);
	struct gpio_chip *gc;
	uint64_t fl;
	int was;

	might_sleep();
	if (!l)
		return;
	gc = l->gc;
	spin_lock_irqsave(&gc->lock, &fl);
	was = (l->flags & GPIOD_FL_REQUESTED) != 0;
	l->flags &= ~(GPIOD_FL_REQUESTED | GPIOD_FL_ACTIVE_LOW |
		      GPIOD_FL_PULL_UP | GPIOD_FL_PULL_DOWN |
		      GPIOD_FL_PULL_DISABLE | GPIOD_FL_OPEN_DRAIN);
	if (!(l->flags & GPIOD_FL_USED_AS_IRQ))
		l->label[0] = '\0';
	spin_unlock_irqrestore(&gc->lock, fl);
	if (WARN(!was, "gpio: freeing line %u of %s that is not requested",
		 l->offset, gc->label ? gc->label : "chip"))
		return;
	if (gc->free)
		gc->free(gc, l->offset);
}

int gpiod_set_consumer_name(struct gpio_desc *desc, const char *name)
{
	struct gpio_line *l = desc_line(desc);
	uint64_t fl;

	if (!l)
		return -EINVAL;
	spin_lock_irqsave(&l->gc->lock, &fl);
	label_set(l->label, name);
	spin_unlock_irqrestore(&l->gc->lock, fl);
	return 0;
}

int gpiod_get_direction(struct gpio_desc *desc)
{
	struct gpio_line *l = desc_line(desc);
	int dir;

	if (!l)
		return -EINVAL;
	if (!l->gc->get_direction)
		return -ENOTSUPP;
	dir = l->gc->get_direction(l->gc, l->offset);
	if (dir == GPIO_LINE_DIRECTION_OUT)
		line_flags_update(l, GPIOD_FL_IS_OUT, 0);
	else if (dir == GPIO_LINE_DIRECTION_IN)
		line_flags_update(l, 0, GPIOD_FL_IS_OUT);
	return dir;
}

/* Apply the pull the line's flags ask for (input lines). */
static int line_apply_bias(struct gpio_line *l)
{
	uint32_t f = line_flags(l);
	enum pin_config_param p;
	int rc;

	if (f & GPIOD_FL_PULL_UP)
		p = PIN_CONFIG_BIAS_PULL_UP;
	else if (f & GPIOD_FL_PULL_DOWN)
		p = PIN_CONFIG_BIAS_PULL_DOWN;
	else if (f & GPIOD_FL_PULL_DISABLE)
		p = PIN_CONFIG_BIAS_DISABLE;
	else
		return 0;
	if (!l->gc->set_config)
		return 0;
	/* 1 = the chip's default strength.  A chip that cannot pull just
	 * leaves the firmware setting. */
	rc = l->gc->set_config(l->gc, l->offset, p, 1);
	return rc == -ENOTSUPP ? 0 : rc;
}

int gpiod_direction_input(struct gpio_desc *desc)
{
	struct gpio_line *l = desc_line(desc);
	int rc;

	if (!l)
		return -EINVAL;
	if (l->gc->direction_input) {
		rc = l->gc->direction_input(l->gc, l->offset);
	} else if (l->gc->get_direction &&
		   l->gc->get_direction(l->gc, l->offset) ==
			   GPIO_LINE_DIRECTION_IN) {
		rc = 0; /* input-only chip */
	} else {
		rc = -EIO;
	}
	if (rc)
		return rc;
	line_flags_update(l, 0, GPIOD_FL_IS_OUT);
	return line_apply_bias(l);
}

int gpiod_direction_output_raw(struct gpio_desc *desc, int value)
{
	struct gpio_line *l = desc_line(desc);
	int rc;

	if (!l)
		return -EINVAL;
	if (line_flags(l) & GPIOD_FL_USED_AS_IRQ) {
		kprintf("gpio: %s line %u is an interrupt, not driving it\n",
			l->gc->label ? l->gc->label : "chip", l->offset);
		return -EIO;
	}
	if (!l->gc->direction_output) {
		if (l->gc->get_direction &&
		    l->gc->get_direction(l->gc, l->offset) ==
			    GPIO_LINE_DIRECTION_OUT && l->gc->set) {
			l->gc->set(l->gc, l->offset, !!value);
			rc = 0;
		} else {
			rc = -EIO;
		}
	} else {
		rc = l->gc->direction_output(l->gc, l->offset, !!value);
	}
	if (rc == 0)
		line_flags_update(l, GPIOD_FL_IS_OUT, 0);
	return rc;
}

int gpiod_direction_output(struct gpio_desc *desc, int value)
{
	struct gpio_line *l = desc_line(desc);

	if (!l)
		return -EINVAL;
	if (line_flags(l) & GPIOD_FL_ACTIVE_LOW)
		value = !value;
	return gpiod_direction_output_raw(desc, value);
}

int gpiod_get_raw_value(const struct gpio_desc *desc)
{
	const struct gpio_line *l = (const struct gpio_line *)desc;
	int v;

	if (!l)
		return -EINVAL;
	v = l->gc->get(l->gc, l->offset);
	return v < 0 ? v : !!v;
}

int gpiod_get_value(const struct gpio_desc *desc)
{
	const struct gpio_line *l = (const struct gpio_line *)desc;
	int v = gpiod_get_raw_value(desc);

	if (v < 0)
		return v;
	return (l->flags & GPIOD_FL_ACTIVE_LOW) ? !v : v;
}

void gpiod_set_raw_value(struct gpio_desc *desc, int value)
{
	struct gpio_line *l = desc_line(desc);

	if (!l || !l->gc->set)
		return;
	/* The output level of an input line is latched by the controller
	 * and driven once the line turns into an output, so setting it is
	 * not an error (AML presets levels this way). */
	l->gc->set(l->gc, l->offset, !!value);
}

void gpiod_set_value(struct gpio_desc *desc, int value)
{
	struct gpio_line *l = desc_line(desc);

	if (!l)
		return;
	if (line_flags(l) & GPIOD_FL_ACTIVE_LOW)
		value = !value;
	gpiod_set_raw_value(desc, value);
}

int gpiod_is_active_low(const struct gpio_desc *desc)
{
	const struct gpio_line *l = (const struct gpio_line *)desc;

	return l ? (l->flags & GPIOD_FL_ACTIVE_LOW) != 0 : 0;
}

void gpiod_toggle_active_low(struct gpio_desc *desc)
{
	struct gpio_line *l = desc_line(desc);
	uint64_t fl;

	if (!l)
		return;
	spin_lock_irqsave(&l->gc->lock, &fl);
	l->flags ^= GPIOD_FL_ACTIVE_LOW;
	spin_unlock_irqrestore(&l->gc->lock, fl);
}

int gpiod_set_config(struct gpio_desc *desc, enum pin_config_param param,
		     uint32_t arg)
{
	struct gpio_line *l = desc_line(desc);

	if (!l)
		return -EINVAL;
	if (!l->gc->set_config)
		return -ENOTSUPP;
	return l->gc->set_config(l->gc, l->offset, param, arg);
}

int gpiod_set_debounce(struct gpio_desc *desc, unsigned int usec)
{
	return gpiod_set_config(desc, PIN_CONFIG_INPUT_DEBOUNCE, usec);
}

int gpiod_configure_flags(struct gpio_desc *desc, unsigned int lookup_flags,
			  enum gpiod_flags flags)
{
	struct gpio_line *l = desc_line(desc);
	uint32_t set = 0;

	if (!l)
		return -EINVAL;
	if (lookup_flags & GPIO_LOOKUP_ACTIVE_LOW)
		set |= GPIOD_FL_ACTIVE_LOW;
	if (lookup_flags & GPIO_LOOKUP_PULL_UP)
		set |= GPIOD_FL_PULL_UP;
	else if (lookup_flags & GPIO_LOOKUP_PULL_DOWN)
		set |= GPIOD_FL_PULL_DOWN;
	else if (lookup_flags & GPIO_LOOKUP_PULL_DISABLE)
		set |= GPIOD_FL_PULL_DISABLE;
	if (lookup_flags & GPIO_LOOKUP_OPEN_DRAIN)
		set |= GPIOD_FL_OPEN_DRAIN;
	line_flags_update(l, set,
			  GPIOD_FL_ACTIVE_LOW | GPIOD_FL_PULL_UP |
				  GPIOD_FL_PULL_DOWN | GPIOD_FL_PULL_DISABLE |
				  GPIOD_FL_OPEN_DRAIN);
	switch (flags) {
	case GPIOD_IN:
		return gpiod_direction_input(desc);
	case GPIOD_OUT_LOW:
	case GPIOD_OUT_HIGH:
		if (set & GPIOD_FL_OPEN_DRAIN) {
			int rc = gpiod_set_config(desc,
						  PIN_CONFIG_DRIVE_OPEN_DRAIN,
						  0);
			if (rc && rc != -ENOTSUPP)
				return rc;
		}
		return gpiod_direction_output(desc, flags == GPIOD_OUT_HIGH);
	case GPIOD_ASIS:
	default:
		/* Direction as found; an input still gets its pull. */
		if (gpiod_get_direction(desc) == GPIO_LINE_DIRECTION_IN)
			return line_apply_bias(l);
		return 0;
	}
}

int gpiod_request_config(struct gpio_desc *desc, const char *label,
			 unsigned int lookup_flags, enum gpiod_flags flags)
{
	int rc = gpiod_request(desc, label);

	if (rc)
		return rc;
	rc = gpiod_configure_flags(desc, lookup_flags, flags);
	if (rc) {
		gpiod_free(desc);
		return rc;
	}
	return 0;
}

/* ==========================================================================
 * Interrupt domain
 * ========================================================================== */

/* Make the hardware mask agree with the reasons (irq_lock held). */
static void line_update_hw_mask(struct gpio_line *l)
{
	struct gpio_chip *gc = l->gc;
	uint8_t want = l->mask_reasons != 0;

	lockdep_assert_held(&gc->irq_lock);
	if (want == l->hw_masked)
		return;
	if (want)
		gc->irq->irq_mask(gc, l->offset);
	else
		gc->irq->irq_unmask(gc, l->offset);
	l->hw_masked = want;
}

/* irq_chip_ops of the line descriptors (descriptor lock held, interrupts
 * off). */
static void gpio_irq_mask(int irq, void *data)
{
	struct gpio_line *l = data;

	(void)irq;
	spin_lock(&l->gc->irq_lock);
	l->mask_reasons |= GPIO_MASK_CORE;
	line_update_hw_mask(l);
	spin_unlock(&l->gc->irq_lock);
}

static void gpio_irq_unmask(int irq, void *data)
{
	struct gpio_line *l = data;

	(void)irq;
	spin_lock(&l->gc->irq_lock);
	l->mask_reasons &= (uint8_t)~GPIO_MASK_CORE;
	line_update_hw_mask(l);
	spin_unlock(&l->gc->irq_lock);
}

static int gpio_irq_set_type(int irq, void *data, unsigned int type)
{
	struct gpio_line *l = data;
	int rc;

	(void)irq;
	spin_lock(&l->gc->irq_lock);
	rc = l->gc->irq->irq_set_type(l->gc, l->offset, type);
	if (rc == 0)
		l->irq_type = type;
	spin_unlock(&l->gc->irq_lock);
	return rc;
}

static int gpio_irq_set_wake(int irq, void *data, unsigned int on)
{
	struct gpio_line *l = data;
	int rc = 0;

	(void)irq;
	if (!l->gc->irq->irq_set_wake)
		return 0;
	spin_lock(&l->gc->irq_lock);
	rc = l->gc->irq->irq_set_wake(l->gc, l->offset, on);
	spin_unlock(&l->gc->irq_lock);
	return rc;
}

static void gpio_irq_retrigger(int irq, void *data)
{
	struct gpio_line *l = data;

	(void)irq;
	/* Level lines latch again by themselves once unmasked. */
	if (l->irq_type & IRQ_TYPE_LEVEL_MASK)
		return;
	__atomic_store_n(&l->sw_pending, 1, __ATOMIC_RELEASE);
	(void)queue_work(system_highpri_wq, &l->gc->resend_work);
}

static void gpio_irq_startup(int irq, void *data)
{
	struct gpio_line *l = data;

	(void)irq;
	if (line_flags(l) & GPIOD_FL_IS_OUT)
		kprintf("gpio: %s line %u is an output used as interrupt\n",
			l->gc->label ? l->gc->label : "chip", l->offset);
	line_flags_update(l, GPIOD_FL_USED_AS_IRQ, 0);
}

static void gpio_irq_shutdown(int irq, void *data)
{
	struct gpio_line *l = data;

	(void)irq;
	line_flags_update(l, 0, GPIOD_FL_USED_AS_IRQ);
}

static const struct irq_chip_ops g_gpio_irq_ops = {
	.compose_msg = NULL,
	.mask = gpio_irq_mask,
	.unmask = gpio_irq_unmask,
	.startup = gpio_irq_startup,
	.shutdown = gpio_irq_shutdown,
	.set_type = gpio_irq_set_type,
	.set_wake = gpio_irq_set_wake,
	.retrigger = gpio_irq_retrigger,
	.flags = IRQ_CHIP_ONESHOT_REQUIRED,
};

int gpiochip_irq_lookup(struct gpio_chip *gc, unsigned int offset)
{
	if (!gc || !gc->lines || offset >= gc->ngpio)
		return 0;
	return __atomic_load_n(&gc->lines[offset].irq, __ATOMIC_ACQUIRE);
}

int gpiochip_irq_map(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_line *l;
	int irq, rc;

	if (!gc || !gc->lines || offset >= gc->ngpio)
		return -EINVAL;
	if (!gc->irq)
		return -ENXIO;
	l = &gc->lines[offset];
	irq = __atomic_load_n(&l->irq, __ATOMIC_ACQUIRE);
	if (irq > 0)
		return irq;
	if (!line_valid(gc, offset))
		return -EINVAL;
	might_sleep();
	if (WARN_ON_ONCE(irqs_disabled()))
		return -EINVAL;
	if (gc->irq->irq_request) {
		rc = gc->irq->irq_request(gc, offset);
		if (rc)
			return rc;
	}
	mutex_lock(&gc->irq_map_mutex);
	irq = l->irq;
	if (irq <= 0) {
		irq = irq_alloc_desc_virtual(&g_gpio_irq_ops, l, NULL, 0);
		if (irq > 0)
			__atomic_store_n(&l->irq, irq, __ATOMIC_RELEASE);
	}
	mutex_unlock(&gc->irq_map_mutex);
	return irq;
}

int gpiod_to_irq(const struct gpio_desc *desc)
{
	const struct gpio_line *l = (const struct gpio_line *)desc;

	if (!l)
		return -EINVAL;
	return gpiochip_irq_map(l->gc, l->offset);
}

int gpiochip_line_is_irq(struct gpio_chip *gc, unsigned int offset)
{
	if (!gc || !gc->lines || offset >= gc->ngpio)
		return 0;
	return (line_flags(&gc->lines[offset]) & GPIOD_FL_USED_AS_IRQ) != 0;
}

int gpiochip_line_is_requested(struct gpio_chip *gc, unsigned int offset)
{
	if (!gc || !gc->lines || offset >= gc->ngpio)
		return 0;
	return (line_flags(&gc->lines[offset]) & GPIOD_FL_REQUESTED) != 0;
}

void gpiochip_handle_irq(struct gpio_chip *gc, unsigned int offset)
{
	struct gpio_line *l;
	uint64_t fl;
	int irq, level;

	if (!gc || !gc->lines || offset >= gc->ngpio || !gc->irq)
		return;
	l = &gc->lines[offset];
	irq = __atomic_load_n(&l->irq, __ATOMIC_ACQUIRE);
	if (irq <= 0) {
		/* Nobody asked for this line's interrupt, yet it fired:
		 * silence it at the controller. */
		spin_lock_irqsave(&gc->irq_lock, &fl);
		l->mask_reasons |= GPIO_MASK_CORE;
		l->hw_masked = 0;
		line_update_hw_mask(l);
		gc->irq->irq_ack(gc, offset);
		spin_unlock_irqrestore(&gc->irq_lock, fl);
		WARN_RATELIMIT(1, "gpio: %s: unexpected interrupt on line %u",
			       gc->label ? gc->label : "chip", offset);
		return;
	}
	l->irq_events++;
	spin_lock_irqsave(&gc->irq_lock, &fl);
	level = (l->irq_type & IRQ_TYPE_LEVEL_MASK) != 0;
	if (level) {
		/* Mask, then acknowledge: the level cannot latch again
		 * before the handlers dealt with the device. */
		l->mask_reasons |= GPIO_MASK_FLOW;
		line_update_hw_mask(l);
	}
	gc->irq->irq_ack(gc, offset);
	spin_unlock_irqrestore(&gc->irq_lock, fl);

	irq_handle_virtual(irq);

	if (level) {
		/* Unmask unless the interrupt core holds the line masked
		 * (disabled, or a oneshot thread still to finish: its
		 * unmask comes when the thread returns). */
		spin_lock_irqsave(&gc->irq_lock, &fl);
		l->mask_reasons &= (uint8_t)~GPIO_MASK_FLOW;
		line_update_hw_mask(l);
		spin_unlock_irqrestore(&gc->irq_lock, fl);
	}
}

void gpiochip_irq_resync_masks(struct gpio_chip *gc)
{
	if (!gc || !gc->lines || !gc->irq)
		return;
	for (unsigned int i = 0; i < gc->ngpio; i++) {
		struct gpio_line *l = &gc->lines[i];
		uint64_t fl;

		if (__atomic_load_n(&l->irq, __ATOMIC_ACQUIRE) <= 0)
			continue;
		/* Written whatever the cache says: the cache is what may be
		 * wrong. */
		spin_lock_irqsave(&gc->irq_lock, &fl);
		if (l->mask_reasons)
			gc->irq->irq_mask(gc, l->offset);
		else
			gc->irq->irq_unmask(gc, l->offset);
		l->hw_masked = l->mask_reasons != 0;
		spin_unlock_irqrestore(&gc->irq_lock, fl);
	}
}

/* Replay edges lost while their interrupt was disabled (process context:
 * the handlers run with interrupts off, as from the parent interrupt). */
static void gpio_resend_work(struct work_struct *work)
{
	struct gpio_chip *gc = container_of(work, struct gpio_chip,
					    resend_work);

	for (unsigned int i = 0; i < gc->ngpio; i++) {
		struct gpio_line *l = &gc->lines[i];
		uint64_t fl;
		int irq;

		if (!__atomic_exchange_n(&l->sw_pending, 0, __ATOMIC_ACQ_REL))
			continue;
		irq = __atomic_load_n(&l->irq, __ATOMIC_ACQUIRE);
		if (irq <= 0)
			continue;
		fl = local_irq_save();
		irq_handle_virtual(irq);
		local_irq_restore(fl);
	}
}

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */

static long show_append(char *buf, long cap, long pos, const char *fmt_line)
{
	long n = 0;

	while (fmt_line[n] && pos + n < cap - 1) {
		buf[pos + n] = fmt_line[n];
		n++;
	}
	if (cap > 0)
		buf[pos + n < cap ? pos + n : cap - 1] = '\0';
	return pos + n;
}

long gpiolib_show(char *buf, long cap)
{
	char line[192], hw[96];
	struct list_head *p;
	long pos = 0;

	if (!buf || cap <= 0)
		return 0;
	buf[0] = '\0';
	mutex_lock(&g_chips_mutex);
	for (p = g_chips.next; p != &g_chips; p = p->next) {
		struct gpio_chip *gc = container_of(p, struct gpio_chip, node);

		ksnprintf(line, sizeof(line), "gpiochip %d-%d: %s%s\n",
			  gc->base, gc->base + (int)gc->ngpio - 1,
			  gc->label ? gc->label : "?",
			  gc->irq ? " (irq)" : "");
		pos = show_append(buf, cap, pos, line);
		for (unsigned int i = 0; i < gc->ngpio; i++) {
			struct gpio_line *l = &gc->lines[i];
			uint32_t f = line_flags(l);

			if (!(f & (GPIOD_FL_REQUESTED | GPIOD_FL_USED_AS_IRQ)) &&
			    l->irq <= 0)
				continue;
			hw[0] = '\0';
			if (gc->describe)
				gc->describe(gc, i, hw, sizeof(hw));
			ksnprintf(line, sizeof(line),
				  " line %3u (%-16s) %s%s%s irq %d type %x events %lu %s\n",
				  i, l->label[0] ? l->label : "-",
				  (f & GPIOD_FL_IS_OUT) ? "out" : "in ",
				  (f & GPIOD_FL_ACTIVE_LOW) ? " active-low" : "",
				  (f & GPIOD_FL_USED_AS_IRQ) ? " [irq]" : "",
				  l->irq, l->irq_type,
				  (unsigned long)l->irq_events, hw);
			pos = show_append(buf, cap, pos, line);
		}
	}
	mutex_unlock(&g_chips_mutex);
	return pos;
}

void gpiolib_dump(void)
{
	long cap = 8192, n;
	char *buf = kalloc((size_t)cap);

	if (!buf)
		return;
	n = gpiolib_show(buf, cap);
	if (n > 0)
		kprintf("%s", buf);
	kfree(buf);
}
