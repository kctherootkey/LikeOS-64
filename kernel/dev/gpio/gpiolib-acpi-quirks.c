// LikeOS GPIO - per-machine quirks of the ACPI GPIO glue
//
// Some firmware describes GPIO interrupts that must not be used as
// described: an _AEI event on a floating pin that storms, a wake flag on
// a touchpad line that wakes the machine at once, an event handler on a
// pin the embedded controller drives for an LED.  The machines are known
// by their SMBIOS strings (kernel/hal/dmi.h); for each, the table below
// says to skip the start-up run of edge events, to ignore the wake flag
// of a controller's pin, or to ignore an event entirely.  The command
// line overrides the table:
//
//   gpiolib_acpi.run_edge_events_on_boot=0|1
//   gpiolib_acpi.ignore_wake=<controller>@<pin>[,<controller>@<pin>...]
//   gpiolib_acpi.ignore_interrupt=<controller>@<pin>[,...]
//
// The table is decided once, on first use.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2018-2025 Hans de Goede and the authors of the
// quirk entries

#include <kernel/dev/gpio/gpiolib.h>
#include <kernel/dev/gpio/gpiolib-acpi.h>
#include <kernel/hal/dmi.h>
#include <kernel/ke/mutex.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

/* The boot command line (kernel/ke/storage.h, whose other declarations
 * need the boot information types). */
const char *storage_boot_cmdline(void);

#define ACPI_GPIO_PARAM_LEN 256

struct acpi_gpiolib_dmi_quirk {
	int no_edge_events_on_boot;
	const char *ignore_wake;
	const char *ignore_interrupt;
};

static const struct dmi_system_id gpiolib_acpi_quirks[] = {
	{
		/*
		 * The Minix Neo Z83-4 has a micro-USB-B id-pin handler for
		 * a non existing micro-USB-B connector which puts the HDMI
		 * DDC pins in GPIO mode, breaking HDMI support.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MINIX"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Z83-4"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.no_edge_events_on_boot = 1,
		},
	},
	{
		/*
		 * The Terra Pad 1061 has a micro-USB-B id-pin handler, which
		 * instead of controlling the actual micro-USB-B turns the 5V
		 * boost for its USB-A connector off. The actual micro-USB-B
		 * connector is wired for charging only.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Wortmann_AG"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TERRA_PAD_1061"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.no_edge_events_on_boot = 1,
		},
	},
	{
		/*
		 * The Dell Venue 10 Pro 5055, with Bay Trail SoC + TI PMIC uses an
		 * external embedded-controller connected via I2C + an ACPI GPIO
		 * event handler on INT33FFC:02 pin 12, causing spurious wakeups.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Venue 10 Pro 5055"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "INT33FC:02@12",
		},
	},
	{
		/*
		 * HP X2 10 models with Cherry Trail SoC + TI PMIC use an
		 * external embedded-controller connected via I2C + an ACPI GPIO
		 * event handler on INT33FF:01 pin 0, causing spurious wakeups.
		 * When suspending by closing the LID, the power to the USB
		 * keyboard is turned off, causing INT0002 ACPI events to
		 * trigger once the XHCI controller notices the keyboard is
		 * gone. So INT0002 events cause spurious wakeups too. Ignoring
		 * EC wakes breaks wakeup when opening the lid, the user needs
		 * to press the power-button to wakeup the system. The
		 * alternative is suspend simply not working, which is worse.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HP x2 Detachable 10-p0XX"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "INT33FF:01@0,INT0002:00@2",
		},
	},
	{
		/*
		 * HP X2 10 models with Bay Trail SoC + AXP288 PMIC use an
		 * external embedded-controller connected via I2C + an ACPI GPIO
		 * event handler on INT33FC:02 pin 28, causing spurious wakeups.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HP Pavilion x2 Detachable"),
			DMI_MATCH(DMI_BOARD_NAME, "815D"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "INT33FC:02@28",
		},
	},
	{
		/*
		 * HP X2 10 models with Cherry Trail SoC + AXP288 PMIC use an
		 * external embedded-controller connected via I2C + an ACPI GPIO
		 * event handler on INT33FF:01 pin 0, causing spurious wakeups.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "HP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HP Pavilion x2 Detachable"),
			DMI_MATCH(DMI_BOARD_NAME, "813E"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "INT33FF:01@0",
		},
	},
	{
		/*
		 * Interrupt storm caused from edge triggered floating pin
		 * Found in BIOS UX325UAZ.300
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "ZenBook UX325UAZ_UM325UAZ"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@18",
		},
	},
	{
		/*
		 * Spurious wakeups from TP_ATTN# pin
		 * Found in BIOS 1.7.8
		 */
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "NL5xNU"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "ELAN0415:00@9",
		},
	},
	{
		/*
		 * Spurious wakeups from TP_ATTN# pin
		 * Found in BIOS 1.7.8
		 */
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "NL5xRU"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "ELAN0415:00@9",
		},
	},
	{
		/*
		 * Spurious wakeups from TP_ATTN# pin
		 * Found in BIOS 1.7.7
		 */
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "NH5xAx"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "SYNA1202:00@16",
		},
	},
	{
		/*
		 * On the Peaq C1010 2-in-1 INT33FC:00 pin 3 is connected to
		 * a "dolby" button. At the ACPI level an _AEI event-handler
		 * is connected which sets an ACPI variable to 1 on both
		 * edges. This variable can be polled + cleared to 0 using
		 * WMI. But since the variable is set on both edges the WMI
		 * interface is pretty useless even when polling.
		 * Ignore the _AEI handler for the pin, so that it is not busy.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "PEAQ"),
			DMI_MATCH(DMI_PRODUCT_NAME, "PEAQ PMM C1010 MD99187"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "INT33FC:00@3",
		},
	},
	{
		/*
		 * Spurious wakeups from TP_ATTN# pin
		 * Found in BIOS 0.35
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-04"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "PNP0C50:00@8",
		},
	},
	{
		/*
		 * Same as G1619-04. New model.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "GPD"),
			DMI_MATCH(DMI_PRODUCT_NAME, "G1619-05"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "PNP0C50:00@8",
		},
	},
	{
		/*
		 * Spurious wakeups from GPIO 11
		 * Found in BIOS 1.04
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Acer Nitro V 14"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@11",
		},
	},
	{
		/*
		 * GPIO 11 is the power enable of the internal USB camera.
		 * The firmware drives it high but also lists it in _AEI with
		 * an empty handler. Requesting it as an input turns the camera
		 * off, so it never enumerates.
		 * Found in BIOS V1.14
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Acer Nitro V 16S AI"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@11",
		},
	},
	{
		/*
		 * Same camera power enable on GPIO 11 as the Acer Nitro V 16S AI
		 * above. It also powers the internal microphones.
		 * Found in BIOS V1.53
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Acer Nitro 16S AI"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@11",
		},
	},
	{
		/*
		 * Wakeup only works when keyboard backlight is turned off
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Acer Nitro V 15"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@8",
		},
	},
	{
		/*
		 * ACPI incorrectly declares GPIO 11 as an interrupt in _AEI,
		 * although the EC uses the pin to control the Caps Lock LED.
		 * Requesting the interrupt configures the pin as an input and
		 * prevents the LED from working.
		 * Found in BIOS R1QET54W (1.40).
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "21AB"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_interrupt = "AMDI0030:00@11",
		},
	},
	{
		/*
		 * Spurious wakeups from TP_ATTN# pin
		 * Found in BIOS 5.35
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "ProArt PX13"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "ASCP1A00:00@8",
		},
	},
	{
		/*
		 * Spurious wakeups, likely from touchpad controller
		 * Dell Precision 7780
		 * Found in BIOS 1.24.1
		 *
		 * Found in touchpad firmware, installed by Dell Touchpad Firmware
		 * Update Utility version 1160.4196.9, A01, released on 11 Jul 2024
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_FAMILY, "Precision"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Precision 7780"),
			DMI_MATCH(DMI_BOARD_NAME, "0C6JVW"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "VEN_0488:00@355",
		},
	},
	{
		/*
		 * Spurious wakeups from GPP3 PCIe bridge interrupt
		 * Found in BIOS UX425UA.301
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "ZenBook UX425UA_UM425UA"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "AMDI0030:00@24",
		},
	},
	{
		/*
		 * Lenovo IdeaPad Slim 3 15ABR8: the touchscreen wake IRQ fires as soon
		 * as the system enters suspend, causing an immediate spurious resume.
		 */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "82XM"),
		},
		.driver_data = &(struct acpi_gpiolib_dmi_quirk) {
			.ignore_wake = "GTCH7503:00@10",
		},
	},
	{ 0 } /* Terminating entry */
};

/* The decided parameters (g_params_lock until g_params_done). */
static int g_run_edge_events_on_boot = -1;
static char g_ignore_wake[ACPI_GPIO_PARAM_LEN];
static char g_ignore_interrupt[ACPI_GPIO_PARAM_LEN];
static volatile int g_params_done;
static DEFINE_MUTEX(g_params_lock);

/* "key=value" on the boot command line: 1 and the value, or 0. */
static int acpi_gpio_cmdline(const char *key, char *buf, size_t cap)
{
	const char *cl = storage_boot_cmdline();
	size_t klen = kstrlen(key);
	int found = 0;

	if (!cl)
		return 0;
	while (*cl) {
		const char *tok, *end;

		while (*cl == ' ' || *cl == '\t')
			cl++;
		if (!*cl)
			break;
		tok = cl;
		while (*cl && *cl != ' ' && *cl != '\t')
			cl++;
		end = cl;
		if ((size_t)(end - tok) <= klen || kstrncmp(tok, key, klen) ||
		    tok[klen] != '=')
			continue;
		/* The last one wins. */
		{
			const char *v = tok + klen + 1;
			size_t n = (size_t)(end - v);

			if (n >= cap)
				n = cap - 1;
			kmemcpy(buf, v, n);
			buf[n] = '\0';
		}
		found = 1;
	}
	return found;
}

static void acpi_gpio_copy(char *dst, const char *src)
{
	size_t i = 0;

	for (; src && src[i] && i + 1 < ACPI_GPIO_PARAM_LEN; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* Decide the parameters: the command line, else the machine's quirk. */
static void acpi_gpio_setup_params(void)
{
	const struct acpi_gpiolib_dmi_quirk *quirk = NULL;
	const struct dmi_system_id *id;
	char val[16];

	if (__atomic_load_n(&g_params_done, __ATOMIC_ACQUIRE))
		return;
	might_sleep();
	mutex_lock(&g_params_lock);
	if (g_params_done) {
		mutex_unlock(&g_params_lock);
		return;
	}
	id = dmi_first_match(gpiolib_acpi_quirks);
	if (id)
		quirk = id->driver_data;
	if (acpi_gpio_cmdline("gpiolib_acpi.run_edge_events_on_boot", val,
			      sizeof(val)))
		g_run_edge_events_on_boot = (val[0] == '0' && !val[1]) ? 0 :
					    (val[0] == '1' && !val[1]) ? 1 :
									 -1;
	if (g_run_edge_events_on_boot < 0)
		g_run_edge_events_on_boot =
			(quirk && quirk->no_edge_events_on_boot) ? 0 : 1;
	if (!acpi_gpio_cmdline("gpiolib_acpi.ignore_wake", g_ignore_wake,
			       sizeof(g_ignore_wake)) &&
	    quirk && quirk->ignore_wake)
		acpi_gpio_copy(g_ignore_wake, quirk->ignore_wake);
	if (!acpi_gpio_cmdline("gpiolib_acpi.ignore_interrupt",
			       g_ignore_interrupt, sizeof(g_ignore_interrupt)) &&
	    quirk && quirk->ignore_interrupt)
		acpi_gpio_copy(g_ignore_interrupt, quirk->ignore_interrupt);
	if (g_ignore_wake[0] || g_ignore_interrupt[0] ||
	    !g_run_edge_events_on_boot)
		kprintf("gpio: ACPI quirks: edge events at boot %s%s%s%s%s\n",
			g_run_edge_events_on_boot ? "on" : "off",
			g_ignore_wake[0] ? ", ignore wake " : "", g_ignore_wake,
			g_ignore_interrupt[0] ? ", ignore interrupt " : "",
			g_ignore_interrupt);
	__atomic_store_n(&g_params_done, 1, __ATOMIC_RELEASE);
	mutex_unlock(&g_params_lock);
}

int acpi_gpio_need_run_edge_events_on_boot(void)
{
	acpi_gpio_setup_params();
	return g_run_edge_events_on_boot;
}

static const char *acpi_gpio_list(enum acpi_gpio_ignore_list list)
{
	acpi_gpio_setup_params();
	switch (list) {
	case ACPI_GPIO_IGNORE_WAKE:
		return g_ignore_wake;
	case ACPI_GPIO_IGNORE_INTERRUPT:
		return g_ignore_interrupt;
	default:
		return "";
	}
}

int acpi_gpio_ignore_list_active(enum acpi_gpio_ignore_list list)
{
	return acpi_gpio_list(list)[0] != '\0';
}

int acpi_gpio_in_ignore_list(enum acpi_gpio_ignore_list list,
			     const char *controller_in, unsigned int pin_in)
{
	const char *ignore_list = acpi_gpio_list(list);
	const char *controller = ignore_list;
	static int warned;

	if (!controller_in)
		return 0;
	while (controller && *controller) {
		const char *pin_str = controller;
		size_t len;

		while (*pin_str && *pin_str != '@')
			pin_str++;
		if (*pin_str != '@')
			goto err;
		len = (size_t)(pin_str - controller);
		if (len == kstrlen(controller_in) &&
		    kstrncmp(controller, controller_in, len) == 0) {
			const char *p = pin_str + 1;
			unsigned long pin = 0;

			while (*p >= '0' && *p <= '9')
				pin = pin * 10 + (unsigned long)(*p++ - '0');
			if (*p != '\0' && *p != ',')
				goto err;
			if (pin == pin_in)
				return 1;
		}
		while (*controller && *controller != ',')
			controller++;
		if (*controller == ',')
			controller++;
	}
	return 0;
err:
	if (!warned) {
		warned = 1;
		kprintf("gpio: invalid ACPI GPIO ignore list: %s\n",
			ignore_list);
	}
	return 0;
}
