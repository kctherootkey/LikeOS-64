// LikeOS pinctrl - Intel PCH/SoC GPIO controller (the "community" design)
//
// From Sunrise Point (Skylake PCH) on, Intel GPIO controllers share one
// register design; the SoC-specific files (pinctrl-<soc>.c) only give
// tables, and pinctrl-intel-platform.c builds the table of newer parts
// from the firmware's _DSD.  The hardware:
//
//   * pads are grouped into COMMUNITIES, each with its own register
//     window (one memory resource of the controller's _CRS, in table
//     order: `barno').  A community's window starts with a header:
//       0x000 REVID   revision in bits 31:16 (>= 0x092: PADCFG2 with
//                     debounce exists, and 1 kOhm pull-downs; >= 0x110:
//                     one PAD_OWN register per pad, 3 bits)
//       0x004 CAPLIST capability list (id 23:16, next 15:0)
//       0x00c PADBAR  offset of the pad configuration array
//     followed, at SoC-specific offsets, by per-group register arrays:
//       PAD_OWN    4 bits per pad: 0 = host, else ME/ISH/... (read-only
//                  for the host); packed 8 pads per register, the
//                  registers of one group following each other
//       PADCFGLOCK two registers per group: PADCFGLOCK (DW0/DW1 locked)
//                  and PADCFGLOCKTX (TX state locked), 1 bit per pad
//       HOSTSW_OWN 1 bit per pad per group: 1 = the GPIO driver owns the
//                  pad's interrupt (GPI_IS/GPI_IE), 0 = ACPI mode (the
//                  pad signals through GPE/SCI and GPI_IS is not set)
//       GPI_IS     interrupt status, 1 bit per pad (write 1 to clear)
//       GPI_IE     interrupt enable, 1 bit per pad
//   * pads are grouped into GROUPS ("GPP_A", ...) of at most 32 pads;
//     a pad's bit in the per-group registers is its index in the group,
//     the group's register index is its `reg_num';
//   * each pad has 2 (revision < 0x092) or 4 dwords of configuration at
//     PADBAR + pad index in community * 8 or 16:
//       PADCFG0 (DW0): 31:30 reset, 26:25 RXEVCFG (0 level, 1 edge,
//         2 disabled, 3 both edges), 24 PREGFRXSEL (glitch filter),
//         23 RXINV, 20 GPIROUTIOXAPIC, 19 GPIROUTSCI, 18 GPIROUTSMI,
//         17 GPIROUTNMI, 13:10 PMODE (0 = GPIO), 9 GPIORXDIS,
//         8 GPIOTXDIS, 1 GPIORXSTATE, 0 GPIOTXSTATE
//       PADCFG1 (DW1): 13 pull up, 12:10 termination strength
//       PADCFG2 (DW2): 4:1 debounce period (2^n * 31.25 us), 0 enable
//
// Numbering.  Pads have a pin number (communities one after the other).
// GPIO numbers -- what ACPI GpioInt/GpioIo descriptors and the gpio_chip
// use -- are given per group by `gpio_base': the firmware numbers each
// group from its own base (often a multiple of 32, with gaps after short
// groups), INTEL_GPIO_BASE_MATCH numbers a group like its pins,
// INTEL_GPIO_BASE_ZERO from 0, and INTEL_GPIO_BASE_NOMAP gives it no GPIO
// numbers at all.  A GPIO number maps to the pin of the group whose
// [gpio_base, gpio_base + size) contains it.
//
// The table structures and macros keep the layout of the published Intel
// tables so those can be taken over unchanged.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2015 Intel Corporation

#ifndef KERNEL_DEV_PINCTRL_INTEL_PINCTRL_INTEL_H
#define KERNEL_DEV_PINCTRL_INTEL_PINCTRL_INTEL_H

#include <kernel/uapi/types.h>
#include <kernel/ke/sched.h>
#include <kernel/ke/io.h>
#include <kernel/ke/list.h>
#include <kernel/dev/gpio/gpiolib.h>

struct acpi_device;

/* Register access; the host test harness substitutes a register model. */
#ifndef pinctrl_readl
#define pinctrl_readl(addr) readl(addr)
#define pinctrl_writel(val, addr) writel((val), (addr))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ---- pins ------------------------------------------------------------------ */

struct pinctrl_pin_desc {
	unsigned int number;
	const char *name;
};

#define PINCTRL_PIN(a, b) { .number = (a), .name = (b) }

/* ---- pad groups -------------------------------------------------------------- */

#define INTEL_PINCTRL_MAX_GPP_SIZE 32

/* A hardware pad group: GPI_IS register `reg_num', pins [base, base +
 * size), GPIO numbers from `gpio_base' (or one of the specials below);
 * `padown_num' is the group's first PAD_OWN register (set by the core). */
struct intel_padgroup {
	unsigned int reg_num;
	unsigned int base;
	unsigned int size;
	int gpio_base;
	unsigned int padown_num;
};

/* gpio_base specials */
enum {
	INTEL_GPIO_BASE_ZERO = -2, /* GPIO numbers from 0 */
	INTEL_GPIO_BASE_NOMAP = -1, /* no GPIO numbers */
	INTEL_GPIO_BASE_MATCH = 0, /* GPIO numbers = pin numbers */
};

#define INTEL_GPP(r, s, e, g)                                              \
	{                                                                  \
		.reg_num = (r), .base = (s), .size = ((e) - (s) + 1),      \
		.gpio_base = (g),                                          \
	}

/* ---- communities ----------------------------------------------------------- */

/*
 * A community.  The SoC tables fill the first block; the core works on a
 * copy and fills the rest.  An offset of 0 means the register does not
 * exist: no PAD_OWN (host owns every pad), no PADCFGLOCK (nothing
 * locked), no HOSTSW_OWN (no pad is in ACPI mode).
 *
 * Older controllers have groups of one size (except the last): the table
 * gives `gpp_size' and `gpp_num_padown_regs' and the core makes the
 * groups.  Newer ones have groups of different sizes, given as `gpps'.
 */
struct intel_community {
	unsigned int barno;
	unsigned int padown_offset;
	unsigned int padcfglock_offset;
	unsigned int hostown_offset;
	unsigned int is_offset;
	unsigned int ie_offset;
	unsigned int features;
	unsigned int pin_base;
	size_t npins;
	unsigned int gpp_size;
	unsigned int gpp_num_padown_regs;
	const struct intel_padgroup *gpps;
	size_t ngpps;

	/* Reserved for the core */
	uint8_t *regs; /* the community window */
	uint8_t *pad_regs; /* PADBAR */
	uint64_t length; /* of the window */
	unsigned int revision;
	uint16_t cap_offset[6]; /* by capability id */
	uint32_t *ctx_intmask; /* GPI_IE over suspend, per group */
	uint32_t *ctx_hostown; /* HOSTSW_OWN over suspend, per group */
};

/* Features (detected from REVID and CAPLIST, or set by the table) */
#define PINCTRL_FEATURE_DEBOUNCE (1u << 0)
#define PINCTRL_FEATURE_1K_PD (1u << 1)
#define PINCTRL_FEATURE_GPIO_HW_INFO (1u << 2)
#define PINCTRL_FEATURE_PWM (1u << 3)
#define PINCTRL_FEATURE_BLINK (1u << 4)
#define PINCTRL_FEATURE_EXP (1u << 5)
#define PINCTRL_FEATURE_3BIT_PAD_OWN (1u << 6)

#define __INTEL_COMMUNITY(b, s, e, g, n, gs, gn, soc)                      \
	{                                                                  \
		.barno = (b), .padown_offset = soc##_PAD_OWN,              \
		.padcfglock_offset = soc##_PADCFGLOCK,                     \
		.hostown_offset = soc##_HOSTSW_OWN,                        \
		.is_offset = soc##_GPI_IS, .ie_offset = soc##_GPI_IE,      \
		.gpp_size = (gs), .gpp_num_padown_regs = (gn),             \
		.pin_base = (s), .npins = ((e) - (s) + 1), .gpps = (g),    \
		.ngpps = (n),                                              \
	}

#define INTEL_COMMUNITY_GPPS(b, s, e, g, soc)                              \
	__INTEL_COMMUNITY(b, s, e, g, ARRAY_SIZE(g), 0, 0, soc)

#define INTEL_COMMUNITY_SIZE(b, s, e, gs, gn, soc)                         \
	__INTEL_COMMUNITY(b, s, e, NULL, 0, gs, gn, soc)

/* ---- SoC data and ACPI ids ------------------------------------------------------ */

/* One controller (one ACPI device).  `uid' selects the entry of a
 * per-_UID table (SoCs with one ACPI device per community). */
struct intel_pinctrl_soc_data {
	const char *uid;
	const struct pinctrl_pin_desc *pins;
	size_t npins;
	const struct intel_community *communities;
	size_t ncommunities;
};

/* An ACPI id and the controller it means: one soc data for every device
 * of the id, or a NULL-terminated table chosen from by _UID. */
struct intel_pinctrl_match {
	const char *hid;
	const struct intel_pinctrl_soc_data *data;
	const struct intel_pinctrl_soc_data *const *uid_table;
};

/* ---- runtime ------------------------------------------------------------------------ */

/* A pad's configuration over system sleep. */
struct intel_pad_context {
	uint32_t padcfg0;
	uint32_t padcfg1;
	uint32_t padcfg2;
	uint8_t saved;
};

struct intel_pinctrl {
	struct gpio_chip chip;
	const struct intel_pinctrl_soc_data *soc;
	struct acpi_device *adev;
	struct intel_community *communities; /* the core's copies */
	size_t ncommunities;
	struct intel_pad_context *pads; /* per soc->pins entry */
	spinlock_t lock; /* the registers */
	int irq; /* the parent interrupt, 0 = none */
	uint32_t gsi;
	uint64_t irq_count;
	/* Built from firmware (_DSD): freed with the controller. */
	struct intel_pinctrl_soc_data *owned_soc;
	struct list_head node; /* every probed controller */
	char label[48];
};

/* Pad lock state */
#define INTEL_PAD_UNLOCKED 0
#define INTEL_PAD_LOCKED (1 << 0)
#define INTEL_PAD_LOCKED_TX (1 << 1)
#define INTEL_PAD_LOCKED_FULL (INTEL_PAD_LOCKED | INTEL_PAD_LOCKED_TX)

/* ---- the core (pinctrl-intel.c) ----------------------------------------------------- */

/* Build the runtime state of `soc' over already-mapped community windows
 * (`windows[i]' is the virtual address of memory resource i, `lengths[i]'
 * its size).  Reads the community headers, writes nothing.  0 or -errno;
 * on error nothing is left allocated.  Used by the probe and by the host
 * test harness. */
int intel_pinctrl_setup(struct intel_pinctrl *pctrl,
			const struct intel_pinctrl_soc_data *soc,
			uint8_t *const *windows, const uint64_t *lengths,
			unsigned int nwindows);
void intel_pinctrl_teardown(struct intel_pinctrl *pctrl);
/* Mask and clear every pad interrupt (probe; resume). */
void intel_pinctrl_irq_init_hw(struct intel_pinctrl *pctrl);
/* The parent interrupt handler (IRQ_HANDLED when a pad was pending). */
int intel_pinctrl_irq_handler(void *arg);
/* Save / restore the pads in use and the interrupt masks over system
 * sleep (interrupts off, after / before the consumers). */
void intel_pinctrl_suspend(struct intel_pinctrl *pctrl);
void intel_pinctrl_resume(struct intel_pinctrl *pctrl);

const struct intel_community *
intel_get_community(const struct intel_pinctrl *pctrl, unsigned int pin);
/* GPIO number -> pin (and its community/group); -EINVAL when unmapped. */
int intel_gpio_to_pin(const struct intel_pinctrl *pctrl, unsigned int offset,
		      const struct intel_community **community,
		      const struct intel_padgroup **padgrp);
/* Pin -> GPIO number, -EINVAL when the pin has none. */
int intel_pin_to_gpio(const struct intel_pinctrl *pctrl, unsigned int pin);
/* The address of a pad register (INTEL_PADCFGx) of `pin', NULL when the
 * pad or the register does not exist. */
uint8_t *intel_get_padcfg(const struct intel_pinctrl *pctrl, unsigned int pin,
			  unsigned int reg);
int intel_pad_owned_by_host(const struct intel_pinctrl *pctrl,
			    unsigned int pin);
int intel_pad_acpi_mode(const struct intel_pinctrl *pctrl, unsigned int pin);
int intel_pad_locked(const struct intel_pinctrl *pctrl, unsigned int pin);

#define INTEL_PADCFG0 0x000
#define INTEL_PADCFG1 0x004
#define INTEL_PADCFG2 0x008

/* Probe one controller of `soc' at `adev' (map, set up, register the
 * chip and its interrupt).  The platform driver passes a soc it built
 * (`owned': freed with the controller).  0 or -errno. */
int intel_pinctrl_probe(struct acpi_device *adev,
			const struct intel_pinctrl_soc_data *soc,
			struct intel_pinctrl_soc_data *owned);
/* The soc data for a device of a match table, NULL when none fits. */
const struct intel_pinctrl_soc_data *
intel_pinctrl_lookup(const struct intel_pinctrl_match *match,
		     const struct acpi_device *adev);

/* Probe every Intel community controller in the ACPI namespace. */
void intel_pinctrl_init(void);
/* System sleep of every probed Intel controller. */
void intel_pinctrl_suspend_all(void);
void intel_pinctrl_resume_all(void);

/* The _DSD-described controllers (pinctrl-intel-platform.c): build the
 * soc data of the device at `handle' from its properties.  0 or -errno;
 * free with intel_platform_free_soc_data. */
int intel_platform_prepare_soc_data(void *handle,
				    struct intel_pinctrl_soc_data **out);
void intel_platform_free_soc_data(struct intel_pinctrl_soc_data *data);
void intel_platform_pinctrl_init(void);

/* The SoC tables (pinctrl-<soc>.c). */
extern const struct intel_pinctrl_match intel_sunrisepoint_match[];
extern const struct intel_pinctrl_match intel_cannonlake_match[];
extern const struct intel_pinctrl_match intel_cedarfork_match[];
extern const struct intel_pinctrl_match intel_denverton_match[];
extern const struct intel_pinctrl_match intel_lewisburg_match[];
extern const struct intel_pinctrl_match intel_emmitsburg_match[];
extern const struct intel_pinctrl_match intel_icelake_match[];
extern const struct intel_pinctrl_match intel_lakefield_match[];
extern const struct intel_pinctrl_match intel_tigerlake_match[];
extern const struct intel_pinctrl_match intel_jasperlake_match[];
extern const struct intel_pinctrl_match intel_elkhartlake_match[];
extern const struct intel_pinctrl_match intel_alderlake_match[];
extern const struct intel_pinctrl_match intel_meteorlake_match[];
extern const struct intel_pinctrl_match intel_meteorpoint_match[];
extern const struct intel_pinctrl_match intel_broxton_match[];
extern const struct intel_pinctrl_match intel_geminilake_match[];

/* Every table above, NULL-terminated (pinctrl-intel.c). */
extern const struct intel_pinctrl_match *const intel_pinctrl_tables[];

/* Lynx Point / Wildcat Point (pinctrl-lynxpoint.c: its own design, the
 * pin table in the common format for the checks of the harness).
 * lp_gpio_create builds a chip over a register window (I/O port base in
 * the kernel) without registering it; the probe registers it. */
extern const struct intel_pinctrl_match intel_lynxpoint_match[];
void lp_gpio_init(void);
struct gpio_chip *lp_gpio_create(unsigned long regs, uint64_t length,
				 const char *label);
void lp_gpio_destroy(struct gpio_chip *gc);
int lp_gpio_handle_parent(struct gpio_chip *gc);
void lp_gpio_resume(struct gpio_chip *gc);
void lp_gpio_resume_all(void);

/* Bay Trail / Cherry View (their own designs). */
void byt_pinctrl_init(void);
void chv_pinctrl_init(void);
/* System sleep of every probed Bay Trail / Cherry View controller. */
void byt_pinctrl_suspend_all(void);
void byt_pinctrl_resume_all(void);
void chv_pinctrl_suspend_all(void);
void chv_pinctrl_resume_all(void);

#endif /* KERNEL_DEV_PINCTRL_INTEL_PINCTRL_INTEL_H */
