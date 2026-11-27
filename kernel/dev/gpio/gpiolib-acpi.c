// LikeOS GPIO - ACPI glue: GpioInt/GpioIo resources, operation regions,
// GPIO-signalled ACPI events
//
// Firmware describes the GPIO lines a device uses as GpioInt and GpioIo
// descriptors in its _CRS: the controller (ResourceSource, a namespace
// path) and pin numbers in that controller's numbering, plus trigger,
// polarity, pull, debounce, wake and I/O restriction.  This file turns
// such a descriptor into a line of a registered chip:
//
//   * acpi_get_gpiod_by_index: the index counts GPIO descriptors of both
//     kinds in _CRS order, and the first pin is used.  A controller that
//     has not registered yet gives -EPROBE_DEFER, so a consumer probed
//     early can retry;
//   * acpi_dev_gpio_irq_wake_get: walks the GPIO descriptors in order and
//     takes the index-th GpioInt.  A descriptor before it whose
//     controller does not exist (or whose pin is out of range) fails the
//     lookup; one whose controller has not registered yet does not.  The
//     line is configured as an input with the descriptor's pull and
//     debounce (debounce is in units of 10 us; a value the controller
//     cannot do fails, one it has no debouncer for is skipped), its
//     interrupt number is created, and the trigger is programmed from the
//     descriptor unless the interrupt is in use already (shared), when
//     the first user's trigger stands.  The line counts as a wake source
//     when the descriptor says so (ExclusiveAndWake/SharedAndWake), the
//     platform sleeps through low-power S0 (FADT), and no quirk says to
//     ignore it;
//   * GeneralPurposeIo operation regions: AML reads and writes pins named
//     by a field's Connection() descriptor.  Each pin is requested once,
//     as "ACPI:OpRegion", configured from the descriptor (an output-only
//     pin starts at the level its pull implies, otherwise the firmware's
//     setting is kept; the descriptor's debounce is applied) and stays
//     requested while the chip exists.  A shared pin that is also an
//     event's is read through the event's line;
//   * _AEI: GPIO-signalled ACPI events.  Every GpioInt in the controller's
//     _AEI is an event; its handler is the method _Exx (edge) or _Lxx
//     (level) with xx the pin number in hex, or _EVT(pin) for pins above
//     255 or without their own method.  The method runs in the interrupt's
//     thread.  At start-up an ActiveBoth event whose line is already low
//     (asserted) is run once.  Disarmed by default (see gpiolib.h); an
//     event that fires more than ACPI_EVENT_STORM_LIMIT times within a
//     second is disabled.
//
// Per-machine quirks (gpiolib-acpi-quirks.c) and the command line
// (gpiolib_acpi.run_edge_events_on_boot=0|1,
//  gpiolib_acpi.ignore_wake=<controller>@<pin>[,...],
//  gpiolib_acpi.ignore_interrupt=<controller>@<pin>[,...]) can turn off
// the start-up run of edge events, the wake flag of a line, or an event.
// <controller> is the ACPI device name "<_HID>:<instance>", numbered as
// the ACPI scan numbers its device objects (acpi_dev_name: every device
// of that id, present or not, in namespace order, _DEP consumers after
// the rest), two hex digits ("INT33FC:02", "ELAN0415:00").
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2012, 2019 Intel Corporation

#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/dev/gpio/gpiolib-acpi.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/hrtimer.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define ACPI_EVENT_STORM_LIMIT 2000u /* events per second before off */

struct acpi_gpio_conn {
	struct list_head node;
	unsigned int pin;
	struct gpio_desc *desc;
};

struct acpi_gpio_event {
	struct list_head node;
	struct gpio_chip *gc;
	struct gpio_desc *desc;
	unsigned int pin;
	int irq;
	unsigned int irq_type;
	int wake;
	int use_evt; /* _EVT(pin) rather than an own method */
	char method[5];
	uint64_t window_ns;
	uint32_t window_count;
	int storm_disabled;
};

struct acpi_gpio_chip {
	struct gpio_chip *gc;
	struct mutex conn_lock; /* conns, and events against the regions */
	struct list_head conns;
	struct list_head events;
	int events_requested;
};

static volatile int g_events_enabled;

void gpiolib_acpi_events_enable(int on)
{
	__atomic_store_n(&g_events_enabled, on ? 1 : 0, __ATOMIC_RELEASE);
}

/* ==========================================================================
 * Device names
 * ========================================================================== */

int acpi_gpio_dev_name(const struct acpi_device *adev, char *buf, size_t len)
{
	if (!adev || !buf || !len || !adev->has_hid)
		return -EINVAL;
	/* Numbered as the ACPI scan numbers device objects (present or
	 * not), like every other ACPI device name (acpi_dev_name). */
	return acpi_dev_name(adev->handle, buf, len);
}

/* Is `pin' of the device at `adev' on an ignore list. */
static int acpi_gpio_ignored(enum acpi_gpio_ignore_list list,
			     const struct acpi_device *adev, unsigned int pin)
{
	char name[ACPI_ID_LEN + 4];

	if (!acpi_gpio_ignore_list_active(list) ||
	    acpi_gpio_dev_name(adev, name, sizeof(name)) != 0)
		return 0;
	return acpi_gpio_in_ignore_list(list, name, pin);
}

/* The wake flag of a GpioInt, unless a quirk says to ignore it. */
static int acpi_gpio_irq_is_wake(const struct acpi_device *adev,
				 const struct acpi_res_gpio *g)
{
	unsigned int pin;

	if (g->npins == 0)
		return 0;
	pin = g->pins[0];
	if (!g->wake_capable)
		return 0;
	if (adev && acpi_gpio_ignored(ACPI_GPIO_IGNORE_WAKE, adev, pin)) {
		kprintf("gpio: %s: ignoring wakeup on pin %u\n", adev->path,
			pin);
		return 0;
	}
	return 1;
}

/* ==========================================================================
 * Descriptor to line
 * ========================================================================== */

/* The direction a descriptor implies (`active_low': the line's
 * polarity). */
static enum gpiod_flags gpio_res_to_flags(const struct acpi_res_gpio *g,
					  int active_low)
{
	/* GpioInt is an input. */
	if (g->conn_type == ACPI_RES_GPIO_INT)
		return GPIOD_IN;
	switch (g->io_restriction) {
	case ACPI_RES_IO_RESTRICT_INPUT:
		return GPIOD_IN;
	case ACPI_RES_IO_RESTRICT_OUTPUT:
		/* There is no initial value in the descriptor: a pulled-up
		 * output starts high, a pulled-down one low (logically,
		 * so an active-low line inverts), any other is left as
		 * the firmware set it. */
		if (g->pin_config == ACPI_RES_PULL_UP)
			return active_low ? GPIOD_OUT_LOW : GPIOD_OUT_HIGH;
		if (g->pin_config == ACPI_RES_PULL_DOWN)
			return active_low ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW;
		return GPIOD_ASIS;
	default:
		/* The firmware has set direction and pull. */
		return GPIOD_ASIS;
	}
}

static void gpio_res_to_info(struct acpi_device *adev,
			     const struct acpi_res_gpio *g,
			     struct acpi_gpio_info *info)
{
	kmemset(info, 0, sizeof(*info));
	info->adev = adev;
	info->gpioint = g->conn_type == ACPI_RES_GPIO_INT;
	info->pin_config = g->pin_config;
	info->io_restriction = g->io_restriction;
	info->debounce = g->debounce_timeout;
	info->wake_capable = (uint8_t)acpi_gpio_irq_is_wake(adev, g);
	/* Polarity and trigger exist for GpioInt only; a GpioIo line is
	 * active high (no _DSD says otherwise here). */
	if (info->gpioint) {
		info->triggering = g->triggering;
		info->polarity = g->polarity;
	} else {
		info->triggering = ACPI_RES_LEVEL;
		info->polarity = ACPI_RES_ACTIVE_HIGH;
	}
	switch (g->pin_config) {
	case ACPI_RES_PULL_UP:
		info->lookup_flags |= GPIO_LOOKUP_PULL_UP;
		break;
	case ACPI_RES_PULL_DOWN:
		info->lookup_flags |= GPIO_LOOKUP_PULL_DOWN;
		break;
	case ACPI_RES_PULL_NONE:
		info->lookup_flags |= GPIO_LOOKUP_PULL_DISABLE;
		break;
	default:
		break;
	}
	if (info->polarity == ACPI_RES_ACTIVE_LOW)
		info->lookup_flags |= GPIO_LOOKUP_ACTIVE_LOW;
	info->flags = gpio_res_to_flags(g, info->polarity == ACPI_RES_ACTIVE_LOW);
}

/* The line `pin' of the controller at `path' (resolved from `scope'):
 * 0, -ENODEV (no such controller in the namespace), -EPROBE_DEFER (not
 * registered yet), -EINVAL (no such line). */
static int resolve_line(void *scope, const char *path, unsigned int pin,
			struct gpio_desc **desc)
{
	struct gpio_chip *gc;
	void *ctrl;

	if (!path || !path[0])
		return -ENODEV;
	ctrl = acpi_get_handle_by_path(scope, path);
	if (!ctrl)
		return -ENODEV;
	gc = gpiochip_find_by_acpi_handle(ctrl);
	if (!gc)
		return -EPROBE_DEFER;
	if (pin >= gc->ngpio)
		return -EINVAL;
	*desc = gpiochip_get_desc(gc, pin);
	return *desc ? 0 : -EINVAL;
}

int acpi_get_gpiod_by_index(struct acpi_device *adev, int index,
			    struct gpio_desc **desc,
			    struct acpi_gpio_info *info)
{
	struct acpi_res_list list;
	const struct acpi_res_gpio *g;
	struct acpi_res_gpio copy;
	int rc;

	might_sleep();
	if (!adev || !desc || index < 0)
		return -EINVAL;
	*desc = NULL;
	rc = acpi_dev_get_resources(adev, &list);
	if (rc)
		return rc;
	g = acpi_res_find_gpio(&list, -1, index);
	if (!g) {
		acpi_dev_free_resource_list(&list);
		return -ENOENT;
	}
	copy = *g;
	acpi_dev_free_resource_list(&list);
	if (copy.npins == 0)
		return -ENOENT;
	if (info)
		gpio_res_to_info(adev, &copy, info);
	return resolve_line(adev->handle, copy.source, copy.pins[0], desc);
}

int acpi_dev_gpio_irq_wake_get(struct acpi_device *adev, int index,
			       int *wake_capable)
{
	struct acpi_res_list list;
	struct acpi_gpio_info info;
	struct gpio_desc *desc = NULL;
	struct acpi_res_gpio g;
	unsigned int type;
	char label[32];
	int rc, irq, idx = 0, found = 0;

	might_sleep();
	if (!adev || index < 0)
		return -EINVAL;
	rc = acpi_dev_get_resources(adev, &list);
	if (rc)
		return rc;
	/* Every GPIO descriptor in order, GpioIo ones too: a descriptor
	 * naming a controller that does not exist ends the lookup even
	 * before the one asked for; one whose controller is still to
	 * come matters only if it is that one. */
	for (int i = 0; !found; i++) {
		const struct acpi_res_gpio *gp = acpi_res_find_gpio(&list, -1, i);
		struct gpio_desc *d = NULL;

		if (!gp || gp->npins == 0) {
			acpi_dev_free_resource_list(&list);
			return -ENOENT;
		}
		rc = resolve_line(adev->handle, gp->source, gp->pins[0], &d);
		if (rc && rc != -EPROBE_DEFER) {
			acpi_dev_free_resource_list(&list);
			return rc;
		}
		if (gp->conn_type == ACPI_RES_GPIO_INT && idx++ == index) {
			if (rc) {
				acpi_dev_free_resource_list(&list);
				return rc;
			}
			g = *gp;
			desc = d;
			found = 1;
		}
	}
	acpi_dev_free_resource_list(&list);
	gpio_res_to_info(adev, &g, &info);

	irq = gpiod_to_irq(desc);
	if (irq < 0)
		return irq;
	ksnprintf(label, sizeof(label), "%s GpioInt(%d)",
		  adev->hid[0] ? adev->hid : "ACPI", index);
	if (!gpiochip_line_is_requested(gpiod_to_chip(desc),
					gpiod_offset(desc)))
		(void)gpiod_set_consumer_name(desc, label);
	rc = gpiod_configure_flags(desc, info.lookup_flags, info.flags);
	if (rc)
		return rc;
	/* ACPI counts debounce in hundredths of a millisecond; a chip
	 * without a debouncer is fine, a value it cannot do is not. */
	rc = gpiod_set_debounce(desc, (unsigned int)info.debounce * 10u);
	if (rc && rc != -ENOTSUPP)
		return rc;
	type = acpi_res_irq_type(info.triggering, info.polarity);
	/* If the interrupt is not in use yet, set the trigger when the
	 * descriptor gives one and it differs; a shared interrupt in use
	 * keeps its first user's. */
	if (!gpiochip_line_is_irq(gpiod_to_chip(desc), gpiod_offset(desc))) {
		if (type != IRQ_TYPE_NONE && type != irq_get_trigger_type(irq))
			(void)irq_set_irq_type(irq, type);
	}
	/* Wake-up from a GPIO only where the platform sleeps through
	 * low-power S0; with S3 it brings suspend trouble. */
	if (wake_capable)
		*wake_capable = acpi_fadt_low_power_s0() ? info.wake_capable :
							   0;
	return irq;
}

int acpi_dev_gpiod_get_index(struct acpi_device *adev, int index,
			     const char *label, enum gpiod_flags flags,
			     struct gpio_desc **desc)
{
	struct acpi_gpio_info info;
	int rc;

	rc = acpi_get_gpiod_by_index(adev, index, desc, &info);
	if (rc)
		return rc;
	/* A GpioInt is no output. */
	if (info.gpioint && (flags == GPIOD_OUT_LOW || flags == GPIOD_OUT_HIGH)) {
		*desc = NULL;
		return -ENOENT;
	}
	/* An IoRestriction in the descriptor decides the direction. */
	if (info.flags != GPIOD_ASIS)
		flags = info.flags;
	rc = gpiod_request_config(*desc, label, info.lookup_flags, flags);
	if (rc) {
		*desc = NULL;
		return rc;
	}
	rc = gpiod_set_debounce(*desc, (unsigned int)info.debounce * 10u);
	if (rc && rc != -ENOTSUPP)
		kprintf("gpio: %s: debounce %u0 us of GPIO %d not applied (%d)\n",
			adev->path, info.debounce, index, rc);
	return 0;
}

/* ==========================================================================
 * Lines the ACPI layer owns (events, operation regions)
 * ========================================================================== */

/* Request pin `pin' of the chip for AML: the direction the descriptor
 * implies (active high), no pull from the descriptor, its debounce. */
static struct gpio_desc *acpi_request_own_gpiod(struct gpio_chip *gc,
						const struct acpi_res_gpio *g,
						unsigned int pin,
						const char *label)
{
	struct gpio_desc *desc;
	int rc;

	if (pin >= gc->ngpio || (gc->valid && !gc->valid(gc, pin)))
		return NULL;
	desc = gpiochip_get_desc(gc, pin);
	if (!desc)
		return NULL;
	rc = gpiod_request_config(desc, label, 0, gpio_res_to_flags(g, 0));
	if (rc) {
		kprintf("gpio: %s pin %u for %s refused (%d)\n",
			gc->label ? gc->label : "chip", pin, label, rc);
		return NULL;
	}
	rc = gpiod_set_debounce(desc, (unsigned int)g->debounce_timeout * 10u);
	if (rc && rc != -ENOTSUPP)
		kprintf("gpio: %s pin %u: debounce %u0 us not applied (%d)\n",
			gc->label ? gc->label : "chip", pin,
			g->debounce_timeout, rc);
	return desc;
}

/* ==========================================================================
 * Operation regions
 * ========================================================================== */

/* The line of `pin' for AML, requesting it on first use; a shared pin
 * that an event already holds is borrowed for reads. */
static struct gpio_desc *opregion_line(struct acpi_gpio_chip *achip,
				       unsigned int pin,
				       const struct acpi_res_gpio *g, int write)
{
	struct acpi_gpio_conn *c;
	struct list_head *p;
	struct gpio_desc *desc;

	mutex_assert_held(&achip->conn_lock);
	for (p = achip->conns.next; p != &achip->conns; p = p->next) {
		c = container_of(p, struct acpi_gpio_conn, node);
		if (c->pin == pin)
			return c->desc;
	}
	if (g->shareable && !write) {
		for (p = achip->events.next; p != &achip->events; p = p->next) {
			struct acpi_gpio_event *ev =
				container_of(p, struct acpi_gpio_event, node);

			if (ev->pin == pin)
				return ev->desc;
		}
	}
	desc = acpi_request_own_gpiod(achip->gc, g, pin, "ACPI:OpRegion");
	if (!desc)
		return NULL;
	c = kcalloc(1, sizeof(*c));
	if (!c) {
		gpiod_free(desc);
		return NULL;
	}
	c->pin = pin;
	c->desc = desc;
	list_add_tail(&c->node, &achip->conns);
	return desc;
}

int acpi_gpio_opregion_access(int write, uint32_t pin_index, uint32_t bits,
			      uint64_t *value, const uint8_t *conn,
			      uint16_t conn_len)
{
	struct acpi_res res;
	struct acpi_gpio_chip *achip;
	struct gpio_chip *gc;
	void *ctrl;
	uint32_t end;
	int rc = 0;

	if (!value || !conn || !conn_len || bits == 0 || bits > 64)
		return -EINVAL;
	if (acpi_res_decode_buffer(conn, conn_len, &res) != 0 ||
	    WARN_ON_ONCE(res.type != ACPI_RES_GPIO))
		return -EINVAL;
	if (WARN_ON_ONCE(write && res.gpio.io_restriction ==
					  ACPI_RES_IO_RESTRICT_INPUT))
		return -EINVAL;
	/* Address is the pin index in the connection's pin table. */
	if (pin_index >= res.gpio.npins)
		return -EINVAL;
	ctrl = acpi_get_handle_by_path(NULL, res.gpio.source);
	gc = ctrl ? gpiochip_find_by_acpi_handle(ctrl) : NULL;
	if (!gc || !gc->acpi_data)
		return -ENODEV;
	achip = gc->acpi_data;
	end = pin_index + bits;
	if (end > res.gpio.npins)
		end = res.gpio.npins;
	if (end > ACPI_RES_GPIO_MAX_PINS)
		end = ACPI_RES_GPIO_MAX_PINS;
	if (pin_index >= end)
		return -EINVAL;
	if (!write)
		*value = 0;
	mutex_lock(&achip->conn_lock);
	for (uint32_t i = pin_index; i < end; i++) {
		struct gpio_desc *desc = opregion_line(achip, res.gpio.pins[i],
						       &res.gpio, write);
		uint32_t bit = i - pin_index;

		if (!desc) {
			rc = -EIO;
			break;
		}
		if (write) {
			gpiod_set_raw_value(desc, (int)((*value >> bit) & 1));
		} else {
			int v = gpiod_get_raw_value(desc);

			if (v < 0) {
				rc = v;
				break;
			}
			*value |= (uint64_t)(v & 1) << bit;
		}
	}
	mutex_unlock(&achip->conn_lock);
	return rc;
}

/* ==========================================================================
 * GPIO-signalled ACPI events (_AEI)
 * ========================================================================== */

static int acpi_event_thread(void *arg)
{
	struct acpi_gpio_event *ev = arg;
	struct acpi_value pin;
	int rc;

	if (ev->use_evt) {
		kmemset(&pin, 0, sizeof(pin));
		pin.type = ACPI_VALUE_INTEGER;
		pin.integer = ev->pin;
		rc = acpi_evaluate(ev->gc->acpi_handle, "_EVT", &pin, 1, NULL);
	} else {
		rc = acpi_evaluate(ev->gc->acpi_handle, ev->method, NULL, 0,
				   NULL);
	}
	if (rc)
		WARN_RATELIMIT(1, "gpio: event %s of pin %u failed (%d)",
			       ev->use_evt ? "_EVT" : ev->method, ev->pin, rc);
	return IRQ_HANDLED;
}

/* Hard handler: storm accounting, then the thread runs the AML. */
static int acpi_event_hard(void *arg)
{
	struct acpi_gpio_event *ev = arg;
	uint64_t now = hrtimer_now_ns();

	if (now - ev->window_ns > 1000000000ull) {
		ev->window_ns = now;
		ev->window_count = 0;
	}
	if (++ev->window_count > ACPI_EVENT_STORM_LIMIT &&
	    !ev->storm_disabled) {
		ev->storm_disabled = 1;
		disable_irq_nosync(ev->irq);
		kprintf("gpio: ACPI event on pin %u storms, disabled\n",
			ev->pin);
		return IRQ_HANDLED;
	}
	return IRQ_WAKE_THREAD;
}

/* Set up the event of one _AEI GpioInt (not yet listening). */
static struct acpi_gpio_event *alloc_event(struct acpi_gpio_chip *achip,
					   struct acpi_device *ctrl,
					   const struct acpi_res_gpio *g)
{
	struct gpio_chip *gc = achip->gc;
	struct acpi_gpio_event *ev;
	struct gpio_desc *desc;
	unsigned int pin = g->pins[0];
	int edge = g->triggering == ACPI_RES_EDGE;
	int irq;

	ev = kcalloc(1, sizeof(*ev));
	if (!ev)
		return NULL;
	ev->gc = gc;
	ev->pin = pin;
	if (pin <= 255) {
		static const char hex[] = "0123456789ABCDEF";

		ev->method[0] = '_';
		ev->method[1] = edge ? 'E' : 'L';
		ev->method[2] = hex[(pin >> 4) & 0xf];
		ev->method[3] = hex[pin & 0xf];
		ev->method[4] = '\0';
	}
	if (!ev->method[0] || !acpi_has_method(gc->acpi_handle, ev->method)) {
		if (!acpi_has_method(gc->acpi_handle, "_EVT")) {
			kfree(ev);
			return NULL; /* no handler in the firmware */
		}
		ev->use_evt = 1;
	}
	if (ctrl && acpi_gpio_ignored(ACPI_GPIO_IGNORE_INTERRUPT, ctrl, pin)) {
		kprintf("gpio: %s: ignoring interrupt on pin %u\n",
			gc->label ? gc->label : "chip", pin);
		kfree(ev);
		return NULL;
	}
	desc = acpi_request_own_gpiod(gc, g, pin, "ACPI:Event");
	if (!desc) {
		kfree(ev);
		return NULL;
	}
	irq = gpiod_to_irq(desc);
	if (irq < 0) {
		kprintf("gpio: %s: no interrupt for pin %u (%d)\n",
			gc->label ? gc->label : "chip", pin, irq);
		gpiod_free(desc);
		kfree(ev);
		return NULL;
	}
	ev->desc = desc;
	ev->irq = irq;
	if (g->triggering == ACPI_RES_LEVEL)
		ev->irq_type = g->polarity == ACPI_RES_ACTIVE_HIGH ?
				       IRQ_TYPE_LEVEL_HIGH :
				       IRQ_TYPE_LEVEL_LOW;
	else if (g->polarity == ACPI_RES_ACTIVE_HIGH)
		ev->irq_type = IRQ_TYPE_EDGE_RISING;
	else if (g->polarity == ACPI_RES_ACTIVE_LOW)
		ev->irq_type = IRQ_TYPE_EDGE_FALLING;
	else
		ev->irq_type = IRQ_TYPE_EDGE_BOTH;
	ev->wake = acpi_gpio_irq_is_wake(ctrl, g);
	return ev;
}

/* Listen to an event; an ActiveBoth one whose line is already asserted
 * (low) runs once. */
static void request_event_irq(struct acpi_gpio_chip *achip,
			      struct acpi_gpio_event *ev)
{
	struct gpio_chip *gc = achip->gc;
	int rc;

	rc = irq_set_irq_type(ev->irq, ev->irq_type);
	if (!rc)
		rc = request_threaded_irq(ev->irq, acpi_event_hard,
					  acpi_event_thread, IRQF_ONESHOT,
					  "ACPI:Event", ev);
	if (rc) {
		kprintf("gpio: %s: ACPI event pin %u: no interrupt (%d)\n",
			gc->label ? gc->label : "chip", ev->pin, rc);
		ev->irq = 0;
		return;
	}
	if (ev->wake && enable_irq_wake(ev->irq) != 0)
		ev->wake = 0;
	kprintf("gpio: %s: ACPI event on pin %u -> %s\n",
		gc->label ? gc->label : "chip", ev->pin,
		ev->use_evt ? "_EVT" : ev->method);
	if (acpi_gpio_need_run_edge_events_on_boot() &&
	    ev->irq_type == IRQ_TYPE_EDGE_BOTH &&
	    gpiod_get_raw_value(ev->desc) == 0)
		(void)acpi_event_thread(ev);
}

int acpi_gpiochip_request_interrupts(struct gpio_chip *gc)
{
	struct acpi_gpio_chip *achip;
	struct acpi_res_list list;
	struct acpi_device *ctrl;
	struct list_head *p;
	int rc, n = 0;

	might_sleep();
	if (!gc || !gc->acpi_data || !gc->irq || !gc->acpi_handle)
		return -EINVAL;
	achip = gc->acpi_data;
	if (achip->events_requested)
		return 0;
	if (!acpi_has_method(gc->acpi_handle, "_AEI"))
		return 0;
	rc = acpi_get_resources(gc->acpi_handle, "_AEI", &list);
	if (rc)
		return rc;
	if (!__atomic_load_n(&g_events_enabled, __ATOMIC_ACQUIRE)) {
		kprintf("gpio: %s: %d ACPI event(s) in _AEI not armed\n",
			gc->label ? gc->label : "chip",
			acpi_res_count(&list, ACPI_RES_GPIO));
		acpi_dev_free_resource_list(&list);
		return 0;
	}
	achip->events_requested = 1;
	/* Set up without holding conn_lock: requesting lines and naming the
	 * controller may run AML (_STA), and AML may come back through an
	 * operation region of this very chip. */
	ctrl = acpi_dev_from_handle(gc->acpi_handle);
	for (int i = 0; i < list.count; i++) {
		const struct acpi_res *r = &list.res[i];
		struct acpi_gpio_event *ev;

		if (r->type != ACPI_RES_GPIO ||
		    r->gpio.conn_type != ACPI_RES_GPIO_INT || !r->gpio.npins)
			continue;
		ev = alloc_event(achip, ctrl, &r->gpio);
		if (!ev)
			continue;
		mutex_lock(&achip->conn_lock);
		list_add_tail(&ev->node, &achip->events);
		mutex_unlock(&achip->conn_lock);
	}
	acpi_dev_free_resource_list(&list);
	if (ctrl)
		acpi_dev_put(ctrl);
	/* Every event set up first, then listening: a handler may touch
	 * pins of another event (operation regions). */
	for (p = achip->events.next; p != &achip->events; p = p->next) {
		request_event_irq(achip,
				  container_of(p, struct acpi_gpio_event, node));
		n++;
	}
	return n;
}

void acpi_gpiochip_free_interrupts(struct gpio_chip *gc)
{
	struct acpi_gpio_chip *achip;

	might_sleep();
	if (!gc || !gc->acpi_data)
		return;
	achip = gc->acpi_data;
	for (;;) {
		struct acpi_gpio_event *ev;

		mutex_lock(&achip->conn_lock);
		if (list_empty(&achip->events)) {
			mutex_unlock(&achip->conn_lock);
			break;
		}
		/* Newest first */
		ev = container_of(achip->events.prev, struct acpi_gpio_event,
				  node);
		list_del(&ev->node);
		mutex_unlock(&achip->conn_lock);
		if (ev->irq > 0) {
			if (ev->wake)
				(void)disable_irq_wake(ev->irq);
			(void)free_irq(ev->irq, ev);
		}
		gpiod_free(ev->desc);
		kfree(ev);
	}
	achip->events_requested = 0;
}

/* ==========================================================================
 * Chip hooks
 * ========================================================================== */

void acpi_gpiochip_add(struct gpio_chip *gc)
{
	struct acpi_gpio_chip *achip;

	if (!gc || !gc->acpi_handle)
		return;
	achip = kcalloc(1, sizeof(*achip));
	if (!achip) {
		kprintf("gpio: %s: no memory for ACPI state\n",
			gc->label ? gc->label : "chip");
		return;
	}
	achip->gc = gc;
	mutex_init(&achip->conn_lock, "gpio_acpi_conn");
	INIT_LIST_HEAD(&achip->conns);
	INIT_LIST_HEAD(&achip->events);
	gc->acpi_data = achip;
	/* Consumers that list the controller in _DEP may go ahead. */
	acpi_dev_supplier_ready(gc->acpi_handle);
	if (gc->irq)
		(void)acpi_gpiochip_request_interrupts(gc);
}

void acpi_gpiochip_remove(struct gpio_chip *gc)
{
	struct acpi_gpio_chip *achip;

	if (!gc || !gc->acpi_data)
		return;
	achip = gc->acpi_data;
	acpi_gpiochip_free_interrupts(gc);
	mutex_lock(&achip->conn_lock);
	while (!list_empty(&achip->conns)) {
		struct acpi_gpio_conn *c = container_of(
			achip->conns.prev, struct acpi_gpio_conn, node);

		list_del(&c->node);
		gpiod_free(c->desc);
		kfree(c);
	}
	mutex_unlock(&achip->conn_lock);
	gc->acpi_data = NULL;
	kfree(achip);
}
