// LikeOS I2C - DesignWare controllers on PCI
//
// Two families:
//
//   * Intel LPSS I2C functions (Sunrise Point and every PCH/SoC since,
//     Broxton/Apollo Lake/Gemini Lake, Lakefield, Elkhart/Jasper Lake ...
//     Panther and Nova Lake).  BAR0 (4 KB, 64-bit, often left unassigned
//     by firmware and placed by pci_assign_unassigned_bars) holds the
//     controller at 0x000, the LPSS private block at 0x200 and the
//     integrated DMA engine at 0x800.  Bring-up: memory decoding on, D0,
//     the private block's capabilities checked (an I2C function), the
//     function and its DMA put in and taken out of reset, the remap
//     address set to the BAR (the datasheets require it whether DMA is
//     used or not), bus mastering on.  The input clock is fixed per
//     platform (100, 120, 133 or 216 MHz) with platform SDA hold and
//     fall times; ACPI (the function's companion, e.g. \_SB.PC00.I2C0)
//     provides the bus speed and usually exact counts (FMCN etc.).
//     The private registers are saved and the function reset over a
//     runtime suspend, and restored on resume.
//   * Older PCI controllers without the private block (Medfield,
//     Merrifield, Bay Trail and Cherry Trail in PCI mode, Haswell's
//     PCI-mode LPSS, Elkhart Lake's PSE): fixed counts from tables.
//
// The interrupt is MSI where the function has it, else its INTx line,
// usually shared: the handler returns IRQ_NONE when not ours.  Bus
// numbers follow probe order (00:15.0 is i2c-0 when it is the first),
// except where the platform has fixed numbers (Medfield, Merrifield).
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2006 Texas Instruments, 2007 MontaVista Software
// Inc., 2009 Provigent Ltd., 2011-2016 Intel Corporation

#include <kernel/dev/i2c/busses/i2c-designware-core.h>
#include <kernel/dev/i2c/i2c-core.h>
#include <kernel/hal/pci.h>
#include <kernel/hal/pci_regs.h>
#include <kernel/hal/acpi_dev.h>
#include <kernel/ke/irq.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/uapi/bug.h>

#define DRIVER_NAME "i2c_designware"
#define LPSS_BAR_MIN_LEN 0x1000

enum dw_pci_kind {
	DW_PCI_LPSS = 1, /* LPSS function with the private block */
	DW_PCI_LEGACY, /* plain DesignWare on PCI */
};

/* The input clock and timings of an LPSS generation. */
struct dw_lpss_info {
	uint32_t clk_rate_hz;
	uint32_t sda_hold_ns;
	uint32_t sda_fall_ns;
	uint32_t scl_fall_ns;
};

static const struct dw_lpss_info lpss_spt_i2c = { 120000000, 230, 0, 0 };
static const struct dw_lpss_info lpss_bxt_i2c = { 133000000, 42, 171, 208 };
static const struct dw_lpss_info lpss_apl_i2c = { 133000000, 207, 171, 208 };
static const struct dw_lpss_info lpss_glk_i2c = { 133000000, 313, 171, 290 };
static const struct dw_lpss_info lpss_cnl_i2c = { 216000000, 230, 0, 0 };
static const struct dw_lpss_info lpss_ehl_i2c = { 100000000, 42, 171, 208 };

/* Counts of the older controllers (HCNT/LCNT/SDA hold). */
struct dw_scl_sda_cfg {
	uint16_t ss_hcnt;
	uint16_t fs_hcnt;
	uint16_t ss_lcnt;
	uint16_t fs_lcnt;
	uint32_t sda_hold_time;
};

static const struct dw_scl_sda_cfg mrfld_config = {
	.ss_hcnt = 0x2f8, .fs_hcnt = 0x87, .ss_lcnt = 0x37b, .fs_lcnt = 0x10a,
};
static const struct dw_scl_sda_cfg byt_config = {
	.ss_hcnt = 0x200, .fs_hcnt = 0x55, .ss_lcnt = 0x200, .fs_lcnt = 0x99,
	.sda_hold_time = 0x6,
};
static const struct dw_scl_sda_cfg hsw_config = {
	.ss_hcnt = 0x01b0, .fs_hcnt = 0x48, .ss_lcnt = 0x01fb, .fs_lcnt = 0xa0,
	.sda_hold_time = 0x9,
};

enum dw_legacy_model {
	DW_MEDFIELD = 1,
	DW_MERRIFIELD,
	DW_BAYTRAIL,
	DW_CHERRYTRAIL,
	DW_HASWELL,
	DW_ELKHARTLAKE_PSE,
};

struct dw_legacy_cfg {
	enum dw_legacy_model model;
	const struct dw_scl_sda_cfg *scl_sda_cfg;
	uint32_t clk_khz; /* 0: unknown */
};

static const struct dw_legacy_cfg legacy_medfield = { DW_MEDFIELD, NULL, 25000 };
static const struct dw_legacy_cfg legacy_merrifield = { DW_MERRIFIELD,
							&mrfld_config, 0 };
static const struct dw_legacy_cfg legacy_baytrail = { DW_BAYTRAIL, &byt_config,
						      0 };
static const struct dw_legacy_cfg legacy_cherrytrail = { DW_CHERRYTRAIL,
							 &byt_config, 0 };
static const struct dw_legacy_cfg legacy_haswell = { DW_HASWELL, &hsw_config,
						     0 };
static const struct dw_legacy_cfg legacy_ehl_pse = { DW_ELKHARTLAKE_PSE, NULL,
						     100000 };

struct dw_pci_cfg {
	enum dw_pci_kind kind;
	const struct dw_lpss_info *lpss;
	const struct dw_legacy_cfg *legacy;
	const char *platform;
};

/* Platform groups: which clock and timing defaults apply. */
static const struct dw_pci_cfg cfg_cml_lp = { DW_PCI_LPSS, &lpss_cnl_i2c, NULL, "Comet Lake-LP" };
static const struct dw_pci_cfg cfg_cml_h = { DW_PCI_LPSS, &lpss_cnl_i2c, NULL, "Comet Lake-H" };
static const struct dw_pci_cfg cfg_bxt_a = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Broxton A" };
static const struct dw_pci_cfg cfg_bxt_b = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Broxton B" };
static const struct dw_pci_cfg cfg_glk = { DW_PCI_LPSS, &lpss_glk_i2c, NULL, "Gemini Lake" };
static const struct dw_pci_cfg cfg_icl_lp = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Ice Lake-LP" };
static const struct dw_pci_cfg cfg_tgl_h = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Tiger Lake-H" };
static const struct dw_pci_cfg cfg_ehl = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Elkhart Lake" };
static const struct dw_pci_cfg cfg_wcl = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Wildcat Lake" };
static const struct dw_pci_cfg cfg_jsl = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Jasper Lake" };
static const struct dw_pci_cfg cfg_adl_p = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Alder Lake-P" };
static const struct dw_pci_cfg cfg_adl_m = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Alder Lake-M/N" };
static const struct dw_pci_cfg cfg_apl = { DW_PCI_LPSS, &lpss_apl_i2c, NULL, "Apollo Lake" };
static const struct dw_pci_cfg cfg_nvl_s = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Nova Lake-S" };
static const struct dw_pci_cfg cfg_arl_h = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Arrow Lake-H" };
static const struct dw_pci_cfg cfg_rpl_s = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Raptor Lake-S" };
static const struct dw_pci_cfg cfg_adl_s = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Alder Lake-S" };
static const struct dw_pci_cfg cfg_mtl_p = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Meteor Lake-P" };
static const struct dw_pci_cfg cfg_mtp_s = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Meteor Lake PCH-S" };
static const struct dw_pci_cfg cfg_lkf = { DW_PCI_LPSS, &lpss_bxt_i2c, NULL, "Lakefield" };
static const struct dw_pci_cfg cfg_spt_lp = { DW_PCI_LPSS, &lpss_spt_i2c, NULL, "Sunrise Point-LP" };
static const struct dw_pci_cfg cfg_cnl_lp = { DW_PCI_LPSS, &lpss_cnl_i2c, NULL, "Cannon Lake-LP" };
static const struct dw_pci_cfg cfg_tgl_lp = { DW_PCI_LPSS, &lpss_spt_i2c, NULL, "Tiger Lake-LP" };
static const struct dw_pci_cfg cfg_spt_h = { DW_PCI_LPSS, &lpss_spt_i2c, NULL, "Sunrise Point-H" };
static const struct dw_pci_cfg cfg_kbl_h = { DW_PCI_LPSS, &lpss_spt_i2c, NULL, "Kaby Lake-H" };
static const struct dw_pci_cfg cfg_cnl_h = { DW_PCI_LPSS, &lpss_cnl_i2c, NULL, "Cannon Lake-H" };
static const struct dw_pci_cfg cfg_cml_v = { DW_PCI_LPSS, &lpss_spt_i2c, NULL, "Comet Lake-V" };
static const struct dw_pci_cfg cfg_lnl_m = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Lunar Lake-M" };
static const struct dw_pci_cfg cfg_nvl_h = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Nova Lake-H" };
static const struct dw_pci_cfg cfg_ptl_h = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Panther Lake-H" };
static const struct dw_pci_cfg cfg_ptl_p = { DW_PCI_LPSS, &lpss_ehl_i2c, NULL, "Panther Lake-P" };

/* Older controllers without the private block. */
static const struct dw_pci_cfg cfg_medfield = { DW_PCI_LEGACY, NULL, &legacy_medfield, "Medfield" };
static const struct dw_pci_cfg cfg_merrifield = { DW_PCI_LEGACY, NULL, &legacy_merrifield, "Merrifield" };
static const struct dw_pci_cfg cfg_baytrail = { DW_PCI_LEGACY, NULL, &legacy_baytrail, "Bay Trail" };
static const struct dw_pci_cfg cfg_cherrytrail = { DW_PCI_LEGACY, NULL, &legacy_cherrytrail, "Cherry Trail" };
static const struct dw_pci_cfg cfg_haswell = { DW_PCI_LEGACY, NULL, &legacy_haswell, "Haswell" };
static const struct dw_pci_cfg cfg_ehl_pse = { DW_PCI_LEGACY, NULL, &legacy_ehl_pse, "Elkhart Lake PSE" };

/* Intel LPSS I2C functions first, then the older controllers. */
static const struct pci_device_id i2c_dw_pci_ids[] = {
	/* Comet Lake-LP */
	{ PCI_DEVICE(0x8086, 0x02c5), .driver_data = (unsigned long)&cfg_cml_lp },
	{ PCI_DEVICE(0x8086, 0x02c6), .driver_data = (unsigned long)&cfg_cml_lp },
	{ PCI_DEVICE(0x8086, 0x02e8), .driver_data = (unsigned long)&cfg_cml_lp },
	{ PCI_DEVICE(0x8086, 0x02e9), .driver_data = (unsigned long)&cfg_cml_lp },
	{ PCI_DEVICE(0x8086, 0x02ea), .driver_data = (unsigned long)&cfg_cml_lp },
	{ PCI_DEVICE(0x8086, 0x02eb), .driver_data = (unsigned long)&cfg_cml_lp },
	/* Comet Lake-H */
	{ PCI_DEVICE(0x8086, 0x06e8), .driver_data = (unsigned long)&cfg_cml_h },
	{ PCI_DEVICE(0x8086, 0x06e9), .driver_data = (unsigned long)&cfg_cml_h },
	{ PCI_DEVICE(0x8086, 0x06ea), .driver_data = (unsigned long)&cfg_cml_h },
	{ PCI_DEVICE(0x8086, 0x06eb), .driver_data = (unsigned long)&cfg_cml_h },
	/* Broxton A */
	{ PCI_DEVICE(0x8086, 0x0aac), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0aae), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0ab0), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0ab2), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0ab4), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0ab6), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0ab8), .driver_data = (unsigned long)&cfg_bxt_a },
	{ PCI_DEVICE(0x8086, 0x0aba), .driver_data = (unsigned long)&cfg_bxt_a },
	/* Broxton B */
	{ PCI_DEVICE(0x8086, 0x1aac), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1aae), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1ab0), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1ab2), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1ab4), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1ab6), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1ab8), .driver_data = (unsigned long)&cfg_bxt_b },
	{ PCI_DEVICE(0x8086, 0x1aba), .driver_data = (unsigned long)&cfg_bxt_b },
	/* Gemini Lake */
	{ PCI_DEVICE(0x8086, 0x31ac), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31ae), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31b0), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31b2), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31b4), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31b6), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31b8), .driver_data = (unsigned long)&cfg_glk },
	{ PCI_DEVICE(0x8086, 0x31ba), .driver_data = (unsigned long)&cfg_glk },
	/* Ice Lake-LP */
	{ PCI_DEVICE(0x8086, 0x34c5), .driver_data = (unsigned long)&cfg_icl_lp },
	{ PCI_DEVICE(0x8086, 0x34c6), .driver_data = (unsigned long)&cfg_icl_lp },
	{ PCI_DEVICE(0x8086, 0x34e8), .driver_data = (unsigned long)&cfg_icl_lp },
	{ PCI_DEVICE(0x8086, 0x34e9), .driver_data = (unsigned long)&cfg_icl_lp },
	{ PCI_DEVICE(0x8086, 0x34ea), .driver_data = (unsigned long)&cfg_icl_lp },
	{ PCI_DEVICE(0x8086, 0x34eb), .driver_data = (unsigned long)&cfg_icl_lp },
	/* Tiger Lake-H */
	{ PCI_DEVICE(0x8086, 0x43ad), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43ae), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43d8), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43e8), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43e9), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43ea), .driver_data = (unsigned long)&cfg_tgl_h },
	{ PCI_DEVICE(0x8086, 0x43eb), .driver_data = (unsigned long)&cfg_tgl_h },
	/* Elkhart Lake */
	{ PCI_DEVICE(0x8086, 0x4b44), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b45), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b4b), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b4c), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b78), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b79), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b7a), .driver_data = (unsigned long)&cfg_ehl },
	{ PCI_DEVICE(0x8086, 0x4b7b), .driver_data = (unsigned long)&cfg_ehl },
	/* Wildcat Lake */
	{ PCI_DEVICE(0x8086, 0x4d50), .driver_data = (unsigned long)&cfg_wcl },
	{ PCI_DEVICE(0x8086, 0x4d51), .driver_data = (unsigned long)&cfg_wcl },
	{ PCI_DEVICE(0x8086, 0x4d78), .driver_data = (unsigned long)&cfg_wcl },
	{ PCI_DEVICE(0x8086, 0x4d79), .driver_data = (unsigned long)&cfg_wcl },
	{ PCI_DEVICE(0x8086, 0x4d7a), .driver_data = (unsigned long)&cfg_wcl },
	{ PCI_DEVICE(0x8086, 0x4d7b), .driver_data = (unsigned long)&cfg_wcl },
	/* Jasper Lake */
	{ PCI_DEVICE(0x8086, 0x4dc5), .driver_data = (unsigned long)&cfg_jsl },
	{ PCI_DEVICE(0x8086, 0x4dc6), .driver_data = (unsigned long)&cfg_jsl },
	{ PCI_DEVICE(0x8086, 0x4de8), .driver_data = (unsigned long)&cfg_jsl },
	{ PCI_DEVICE(0x8086, 0x4de9), .driver_data = (unsigned long)&cfg_jsl },
	{ PCI_DEVICE(0x8086, 0x4dea), .driver_data = (unsigned long)&cfg_jsl },
	{ PCI_DEVICE(0x8086, 0x4deb), .driver_data = (unsigned long)&cfg_jsl },
	/* Alder Lake-P */
	{ PCI_DEVICE(0x8086, 0x51c5), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51c6), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51d8), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51d9), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51e8), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51e9), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51ea), .driver_data = (unsigned long)&cfg_adl_p },
	{ PCI_DEVICE(0x8086, 0x51eb), .driver_data = (unsigned long)&cfg_adl_p },
	/* Alder Lake-M/N */
	{ PCI_DEVICE(0x8086, 0x54c5), .driver_data = (unsigned long)&cfg_adl_m },
	{ PCI_DEVICE(0x8086, 0x54c6), .driver_data = (unsigned long)&cfg_adl_m },
	{ PCI_DEVICE(0x8086, 0x54e8), .driver_data = (unsigned long)&cfg_adl_m },
	{ PCI_DEVICE(0x8086, 0x54e9), .driver_data = (unsigned long)&cfg_adl_m },
	{ PCI_DEVICE(0x8086, 0x54ea), .driver_data = (unsigned long)&cfg_adl_m },
	{ PCI_DEVICE(0x8086, 0x54eb), .driver_data = (unsigned long)&cfg_adl_m },
	/* Apollo Lake */
	{ PCI_DEVICE(0x8086, 0x5aac), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5aae), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5ab0), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5ab2), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5ab4), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5ab6), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5ab8), .driver_data = (unsigned long)&cfg_apl },
	{ PCI_DEVICE(0x8086, 0x5aba), .driver_data = (unsigned long)&cfg_apl },
	/* Nova Lake-S */
	{ PCI_DEVICE(0x8086, 0x6e4c), .driver_data = (unsigned long)&cfg_nvl_s },
	{ PCI_DEVICE(0x8086, 0x6e4d), .driver_data = (unsigned long)&cfg_nvl_s },
	{ PCI_DEVICE(0x8086, 0x6e4e), .driver_data = (unsigned long)&cfg_nvl_s },
	{ PCI_DEVICE(0x8086, 0x6e4f), .driver_data = (unsigned long)&cfg_nvl_s },
	{ PCI_DEVICE(0x8086, 0x6e7a), .driver_data = (unsigned long)&cfg_nvl_s },
	{ PCI_DEVICE(0x8086, 0x6e7b), .driver_data = (unsigned long)&cfg_nvl_s },
	/* Arrow Lake-H */
	{ PCI_DEVICE(0x8086, 0x7750), .driver_data = (unsigned long)&cfg_arl_h },
	{ PCI_DEVICE(0x8086, 0x7751), .driver_data = (unsigned long)&cfg_arl_h },
	{ PCI_DEVICE(0x8086, 0x7778), .driver_data = (unsigned long)&cfg_arl_h },
	{ PCI_DEVICE(0x8086, 0x7779), .driver_data = (unsigned long)&cfg_arl_h },
	{ PCI_DEVICE(0x8086, 0x777a), .driver_data = (unsigned long)&cfg_arl_h },
	{ PCI_DEVICE(0x8086, 0x777b), .driver_data = (unsigned long)&cfg_arl_h },
	/* Raptor Lake-S */
	{ PCI_DEVICE(0x8086, 0x7a4c), .driver_data = (unsigned long)&cfg_rpl_s },
	{ PCI_DEVICE(0x8086, 0x7a4d), .driver_data = (unsigned long)&cfg_rpl_s },
	{ PCI_DEVICE(0x8086, 0x7a4e), .driver_data = (unsigned long)&cfg_rpl_s },
	{ PCI_DEVICE(0x8086, 0x7a4f), .driver_data = (unsigned long)&cfg_rpl_s },
	{ PCI_DEVICE(0x8086, 0x7a7c), .driver_data = (unsigned long)&cfg_rpl_s },
	{ PCI_DEVICE(0x8086, 0x7a7d), .driver_data = (unsigned long)&cfg_rpl_s },
	/* Alder Lake-S */
	{ PCI_DEVICE(0x8086, 0x7acc), .driver_data = (unsigned long)&cfg_adl_s },
	{ PCI_DEVICE(0x8086, 0x7acd), .driver_data = (unsigned long)&cfg_adl_s },
	{ PCI_DEVICE(0x8086, 0x7ace), .driver_data = (unsigned long)&cfg_adl_s },
	{ PCI_DEVICE(0x8086, 0x7acf), .driver_data = (unsigned long)&cfg_adl_s },
	{ PCI_DEVICE(0x8086, 0x7afc), .driver_data = (unsigned long)&cfg_adl_s },
	{ PCI_DEVICE(0x8086, 0x7afd), .driver_data = (unsigned long)&cfg_adl_s },
	/* Meteor Lake-P */
	{ PCI_DEVICE(0x8086, 0x7e50), .driver_data = (unsigned long)&cfg_mtl_p },
	{ PCI_DEVICE(0x8086, 0x7e51), .driver_data = (unsigned long)&cfg_mtl_p },
	{ PCI_DEVICE(0x8086, 0x7e78), .driver_data = (unsigned long)&cfg_mtl_p },
	{ PCI_DEVICE(0x8086, 0x7e79), .driver_data = (unsigned long)&cfg_mtl_p },
	{ PCI_DEVICE(0x8086, 0x7e7a), .driver_data = (unsigned long)&cfg_mtl_p },
	{ PCI_DEVICE(0x8086, 0x7e7b), .driver_data = (unsigned long)&cfg_mtl_p },
	/* Meteor Lake PCH-S */
	{ PCI_DEVICE(0x8086, 0x7f4c), .driver_data = (unsigned long)&cfg_mtp_s },
	{ PCI_DEVICE(0x8086, 0x7f4d), .driver_data = (unsigned long)&cfg_mtp_s },
	{ PCI_DEVICE(0x8086, 0x7f4e), .driver_data = (unsigned long)&cfg_mtp_s },
	{ PCI_DEVICE(0x8086, 0x7f4f), .driver_data = (unsigned long)&cfg_mtp_s },
	{ PCI_DEVICE(0x8086, 0x7f7a), .driver_data = (unsigned long)&cfg_mtp_s },
	{ PCI_DEVICE(0x8086, 0x7f7b), .driver_data = (unsigned long)&cfg_mtp_s },
	/* Lakefield */
	{ PCI_DEVICE(0x8086, 0x98c5), .driver_data = (unsigned long)&cfg_lkf },
	{ PCI_DEVICE(0x8086, 0x98c6), .driver_data = (unsigned long)&cfg_lkf },
	{ PCI_DEVICE(0x8086, 0x98e8), .driver_data = (unsigned long)&cfg_lkf },
	{ PCI_DEVICE(0x8086, 0x98e9), .driver_data = (unsigned long)&cfg_lkf },
	{ PCI_DEVICE(0x8086, 0x98ea), .driver_data = (unsigned long)&cfg_lkf },
	{ PCI_DEVICE(0x8086, 0x98eb), .driver_data = (unsigned long)&cfg_lkf },
	/* Sunrise Point-LP */
	{ PCI_DEVICE(0x8086, 0x9d60), .driver_data = (unsigned long)&cfg_spt_lp },
	{ PCI_DEVICE(0x8086, 0x9d61), .driver_data = (unsigned long)&cfg_spt_lp },
	{ PCI_DEVICE(0x8086, 0x9d62), .driver_data = (unsigned long)&cfg_spt_lp },
	{ PCI_DEVICE(0x8086, 0x9d63), .driver_data = (unsigned long)&cfg_spt_lp },
	{ PCI_DEVICE(0x8086, 0x9d64), .driver_data = (unsigned long)&cfg_spt_lp },
	{ PCI_DEVICE(0x8086, 0x9d65), .driver_data = (unsigned long)&cfg_spt_lp },
	/* Cannon Lake-LP */
	{ PCI_DEVICE(0x8086, 0x9dc5), .driver_data = (unsigned long)&cfg_cnl_lp },
	{ PCI_DEVICE(0x8086, 0x9dc6), .driver_data = (unsigned long)&cfg_cnl_lp },
	{ PCI_DEVICE(0x8086, 0x9de8), .driver_data = (unsigned long)&cfg_cnl_lp },
	{ PCI_DEVICE(0x8086, 0x9de9), .driver_data = (unsigned long)&cfg_cnl_lp },
	{ PCI_DEVICE(0x8086, 0x9dea), .driver_data = (unsigned long)&cfg_cnl_lp },
	{ PCI_DEVICE(0x8086, 0x9deb), .driver_data = (unsigned long)&cfg_cnl_lp },
	/* Tiger Lake-LP */
	{ PCI_DEVICE(0x8086, 0xa0c5), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0c6), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0d8), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0d9), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0e8), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0e9), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0ea), .driver_data = (unsigned long)&cfg_tgl_lp },
	{ PCI_DEVICE(0x8086, 0xa0eb), .driver_data = (unsigned long)&cfg_tgl_lp },
	/* Sunrise Point-H */
	{ PCI_DEVICE(0x8086, 0xa160), .driver_data = (unsigned long)&cfg_spt_h },
	{ PCI_DEVICE(0x8086, 0xa161), .driver_data = (unsigned long)&cfg_spt_h },
	{ PCI_DEVICE(0x8086, 0xa162), .driver_data = (unsigned long)&cfg_spt_h },
	/* Kaby Lake-H */
	{ PCI_DEVICE(0x8086, 0xa2e0), .driver_data = (unsigned long)&cfg_kbl_h },
	{ PCI_DEVICE(0x8086, 0xa2e1), .driver_data = (unsigned long)&cfg_kbl_h },
	{ PCI_DEVICE(0x8086, 0xa2e2), .driver_data = (unsigned long)&cfg_kbl_h },
	{ PCI_DEVICE(0x8086, 0xa2e3), .driver_data = (unsigned long)&cfg_kbl_h },
	/* Cannon Lake-H */
	{ PCI_DEVICE(0x8086, 0xa368), .driver_data = (unsigned long)&cfg_cnl_h },
	{ PCI_DEVICE(0x8086, 0xa369), .driver_data = (unsigned long)&cfg_cnl_h },
	{ PCI_DEVICE(0x8086, 0xa36a), .driver_data = (unsigned long)&cfg_cnl_h },
	{ PCI_DEVICE(0x8086, 0xa36b), .driver_data = (unsigned long)&cfg_cnl_h },
	/* Comet Lake-V */
	{ PCI_DEVICE(0x8086, 0xa3e0), .driver_data = (unsigned long)&cfg_cml_v },
	{ PCI_DEVICE(0x8086, 0xa3e1), .driver_data = (unsigned long)&cfg_cml_v },
	{ PCI_DEVICE(0x8086, 0xa3e2), .driver_data = (unsigned long)&cfg_cml_v },
	{ PCI_DEVICE(0x8086, 0xa3e3), .driver_data = (unsigned long)&cfg_cml_v },
	/* Lunar Lake-M */
	{ PCI_DEVICE(0x8086, 0xa850), .driver_data = (unsigned long)&cfg_lnl_m },
	{ PCI_DEVICE(0x8086, 0xa851), .driver_data = (unsigned long)&cfg_lnl_m },
	{ PCI_DEVICE(0x8086, 0xa878), .driver_data = (unsigned long)&cfg_lnl_m },
	{ PCI_DEVICE(0x8086, 0xa879), .driver_data = (unsigned long)&cfg_lnl_m },
	{ PCI_DEVICE(0x8086, 0xa87a), .driver_data = (unsigned long)&cfg_lnl_m },
	{ PCI_DEVICE(0x8086, 0xa87b), .driver_data = (unsigned long)&cfg_lnl_m },
	/* Nova Lake-H */
	{ PCI_DEVICE(0x8086, 0xd350), .driver_data = (unsigned long)&cfg_nvl_h },
	{ PCI_DEVICE(0x8086, 0xd351), .driver_data = (unsigned long)&cfg_nvl_h },
	{ PCI_DEVICE(0x8086, 0xd378), .driver_data = (unsigned long)&cfg_nvl_h },
	{ PCI_DEVICE(0x8086, 0xd379), .driver_data = (unsigned long)&cfg_nvl_h },
	{ PCI_DEVICE(0x8086, 0xd37a), .driver_data = (unsigned long)&cfg_nvl_h },
	{ PCI_DEVICE(0x8086, 0xd37b), .driver_data = (unsigned long)&cfg_nvl_h },
	/* Panther Lake-H */
	{ PCI_DEVICE(0x8086, 0xe350), .driver_data = (unsigned long)&cfg_ptl_h },
	{ PCI_DEVICE(0x8086, 0xe351), .driver_data = (unsigned long)&cfg_ptl_h },
	{ PCI_DEVICE(0x8086, 0xe378), .driver_data = (unsigned long)&cfg_ptl_h },
	{ PCI_DEVICE(0x8086, 0xe379), .driver_data = (unsigned long)&cfg_ptl_h },
	{ PCI_DEVICE(0x8086, 0xe37a), .driver_data = (unsigned long)&cfg_ptl_h },
	{ PCI_DEVICE(0x8086, 0xe37b), .driver_data = (unsigned long)&cfg_ptl_h },
	/* Panther Lake-P */
	{ PCI_DEVICE(0x8086, 0xe450), .driver_data = (unsigned long)&cfg_ptl_p },
	{ PCI_DEVICE(0x8086, 0xe451), .driver_data = (unsigned long)&cfg_ptl_p },
	{ PCI_DEVICE(0x8086, 0xe478), .driver_data = (unsigned long)&cfg_ptl_p },
	{ PCI_DEVICE(0x8086, 0xe479), .driver_data = (unsigned long)&cfg_ptl_p },
	{ PCI_DEVICE(0x8086, 0xe47a), .driver_data = (unsigned long)&cfg_ptl_p },
	{ PCI_DEVICE(0x8086, 0xe47b), .driver_data = (unsigned long)&cfg_ptl_p },
	/* Medfield */
	{ PCI_DEVICE(0x8086, 0x0817), .driver_data = (unsigned long)&cfg_medfield },
	{ PCI_DEVICE(0x8086, 0x0818), .driver_data = (unsigned long)&cfg_medfield },
	{ PCI_DEVICE(0x8086, 0x0819), .driver_data = (unsigned long)&cfg_medfield },
	{ PCI_DEVICE(0x8086, 0x082C), .driver_data = (unsigned long)&cfg_medfield },
	{ PCI_DEVICE(0x8086, 0x082D), .driver_data = (unsigned long)&cfg_medfield },
	{ PCI_DEVICE(0x8086, 0x082E), .driver_data = (unsigned long)&cfg_medfield },
	/* Merrifield */
	{ PCI_DEVICE(0x8086, 0x1195), .driver_data = (unsigned long)&cfg_merrifield },
	{ PCI_DEVICE(0x8086, 0x1196), .driver_data = (unsigned long)&cfg_merrifield },
	/* Bay Trail (PCI mode) */
	{ PCI_DEVICE(0x8086, 0x0F41), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F42), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F43), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F44), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F45), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F46), .driver_data = (unsigned long)&cfg_baytrail },
	{ PCI_DEVICE(0x8086, 0x0F47), .driver_data = (unsigned long)&cfg_baytrail },
	/* Haswell (PCI mode) */
	{ PCI_DEVICE(0x8086, 0x9c61), .driver_data = (unsigned long)&cfg_haswell },
	{ PCI_DEVICE(0x8086, 0x9c62), .driver_data = (unsigned long)&cfg_haswell },
	/* Braswell / Cherry Trail (PCI mode) */
	{ PCI_DEVICE(0x8086, 0x22C1), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C2), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C3), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C4), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C5), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C6), .driver_data = (unsigned long)&cfg_cherrytrail },
	{ PCI_DEVICE(0x8086, 0x22C7), .driver_data = (unsigned long)&cfg_cherrytrail },
	/* Elkhart Lake (PSE I2C) */
	{ PCI_DEVICE(0x8086, 0x4bb9), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bba), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bbb), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bbc), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bbd), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bbe), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bbf), .driver_data = (unsigned long)&cfg_ehl_pse },
	{ PCI_DEVICE(0x8086, 0x4bc0), .driver_data = (unsigned long)&cfg_ehl_pse },
	/* The I2C controllers inside AMD Navi GPUs (USB-C UCSI) need a
	 * polled transfer quirk and a UCSI client; they are left alone. */
	{ 0 },
};

/* Per controller: the BAR and the saved LPSS private registers. */
struct dw_pci_glue {
	pci_device_t *pdev;
	struct dw_i2c_dev *dev;
	const struct dw_pci_cfg *cfg;
	uint64_t bar_phys;
	uint32_t priv_ctx[LPSS_PRIV_REG_COUNT];
	int vectors;
};

static uint32_t dw_pci_clk_khz(struct dw_i2c_dev *dev)
{
	return dev->clk_khz;
}

/* ==========================================================================
 * Intel LPSS private block
 * ========================================================================== */

void i2c_dw_lpss_init_priv(struct dw_i2c_dev *dev, uint64_t bar_phys)
{
	/* The function into reset, then out with its DMA engine. */
	lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
	lpss_priv_writel(dev, LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA,
			 LPSS_PRIV_RESETS);
	/* The remap address is the BAR, low dword first. */
	lpss_priv_writel(dev, (uint32_t)bar_phys, LPSS_PRIV_REMAP_ADDR);
	lpss_priv_writel(dev, (uint32_t)(bar_phys >> 32),
			 LPSS_PRIV_REMAP_ADDR + 4);
}

void i2c_dw_lpss_suspend(struct dw_i2c_dev *dev)
{
	uint32_t *ctx = dev->glue;

	if (!dev->lpss_priv || !ctx)
		return;
	/* Save the block, then hold the function in reset. */
	for (unsigned int i = 0; i < LPSS_PRIV_REG_COUNT; i++)
		ctx[i] = lpss_priv_readl(dev, i * 4);
	lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
}

void i2c_dw_lpss_resume(struct dw_i2c_dev *dev)
{
	uint32_t *ctx = dev->glue;

	if (!dev->lpss_priv || !ctx)
		return;
	lpss_priv_writel(dev, LPSS_PRIV_RESETS_FUNC | LPSS_PRIV_RESETS_IDMA,
			 LPSS_PRIV_RESETS);
	for (unsigned int i = 0; i < LPSS_PRIV_REG_COUNT; i++)
		lpss_priv_writel(dev, ctx[i], i * 4);
}

/* ==========================================================================
 * Probe
 * ========================================================================== */

/* The fixed bus numbers of the phone SoCs. */
static int dw_legacy_bus_num(const pci_device_t *pdev,
			     const struct dw_legacy_cfg *lc,
			     struct dw_i2c_dev *dev)
{
	switch (lc->model) {
	case DW_MEDFIELD:
		switch (pdev->device_id) {
		case 0x0817:
			dev->timings.bus_freq_hz = I2C_MAX_STANDARD_MODE_FREQ;
			/* fall through */
		case 0x0818:
		case 0x0819:
			return pdev->device_id - 0x817 + 3;
		case 0x082C:
		case 0x082D:
		case 0x082E:
			return pdev->device_id - 0x82C + 0;
		}
		return -ENODEV;
	case DW_MERRIFIELD:
		/* buses 1..7: four functions in slot 8, three in slot 9 */
		switch (pdev->device) {
		case 8:
			return pdev->function + 0 + 1;
		case 9:
			return pdev->function + 4 + 1;
		}
		return -ENODEV;
	default:
		return -1;
	}
}

static int i2c_dw_pci_probe(pci_device_t *pdev, const struct pci_device_id *id)
{
	const struct dw_pci_cfg *cfg = (const struct dw_pci_cfg *)(unsigned long)
					       id->driver_data;
	struct dw_pci_glue *glue = NULL;
	struct dw_i2c_dev *dev = NULL;
	struct acpi_device *companion;
	volatile uint8_t *base = NULL;
	uint16_t cmd_before = 0, cmd_after = 0;
	uint64_t bar, bar_len;
	char pname[16];
	int ret, irq, nr = -1;

	might_sleep();
	if (!cfg)
		return -EINVAL;
	pci_name(pdev, pname, sizeof(pname));

	/* The ACPI companion first: power (_PS0) and the configuration
	 * the controller takes from firmware. */
	companion = acpi_pci_find_companion(pdev);
	if (companion && acpi_device_power_manageable(companion)) {
		ret = acpi_device_set_power(companion, ACPI_DEV_D0);
		if (ret)
			kprintf(DRIVER_NAME " %s: _PS0 failed (%d)\n", pname,
				ret);
	}

	(void)pci_read_config_word(pdev, PCI_COMMAND, &cmd_before);
	ret = pci_enable_device_mem(pdev);
	if (ret) {
		kprintf(DRIVER_NAME " %s: Failed to enable I2C PCI device (%d)\n",
			pname, ret);
		goto err_put;
	}
	(void)pci_read_config_word(pdev, PCI_COMMAND, &cmd_after);
	if ((cmd_before & 0x7) != (cmd_after & 0x7))
		kprintf(DRIVER_NAME " %s: enabling device (%04x -> %04x)\n",
			pname, cmd_before & 0x7, cmd_after & 0x7);
	if (!(cmd_after & PCI_COMMAND_MEMORY)) {
		kprintf(DRIVER_NAME " %s: memory decoding stays off\n", pname);
		ret = -EIO;
		goto err_put;
	}

	/* BAR0: firmware often leaves it at 0 on LPSS functions; it must
	 * have been placed by now. */
	bar = pci_resource_start(pdev, 0);
	bar_len = pci_resource_len(pdev, 0);
	if (!bar || (pci_resource_flags(pdev, 0) & PCI_BAR_IO)) {
		kprintf(DRIVER_NAME " %s: BAR0 not assigned\n", pname);
		ret = -ENODEV;
		goto err_put;
	}
	if (cfg->kind == DW_PCI_LPSS && bar_len && bar_len < LPSS_BAR_MIN_LEN) {
		kprintf(DRIVER_NAME " %s: BAR0 of %lu bytes is too small\n",
			pname, (unsigned long)bar_len);
		ret = -ENODEV;
		goto err_put;
	}
	(void)pci_set_power_state(pdev, PCI_D0);
	base = pci_iomap(pdev, 0, LPSS_BAR_MIN_LEN);
	if (!base) {
		kprintf(DRIVER_NAME " %s: I/O memory remapping failed\n",
			pname);
		ret = -ENOMEM;
		goto err_put;
	}

	dev = kcalloc(1, sizeof(*dev));
	glue = kcalloc(1, sizeof(*glue));
	if (!dev || !glue) {
		ret = -ENOMEM;
		goto err_unmap;
	}
	glue->pdev = pdev;
	glue->cfg = cfg;
	glue->bar_phys = bar;
	ksnprintf(dev->name, sizeof(dev->name), DRIVER_NAME " %s", pname);
	dev->base = base;
	dev->acpi_dev = companion;
	dev->adapter.nr = -1;

	if (cfg->kind == DW_PCI_LPSS) {
		uint32_t caps, type;

		dev->lpss_priv = base + LPSS_PRIV_OFFSET_PCI;
		caps = lpss_priv_readl(dev, LPSS_PRIV_CAPS);
		if (caps == 0xffffffffu) {
			kprintf(DRIVER_NAME " %s: not responding\n", pname);
			ret = -ENODEV;
			goto err_unmap;
		}
		type = (caps & LPSS_PRIV_CAPS_TYPE_MASK) >>
		       LPSS_PRIV_CAPS_TYPE_SHIFT;
		if (type != LPSS_DEV_I2C) {
			kprintf(DRIVER_NAME " %s: LPSS function of type %u, not I2C\n",
				pname, type);
			ret = -ENODEV;
			goto err_unmap;
		}
		i2c_dw_lpss_init_priv(dev, bar);
		dev->glue = glue->priv_ctx;
		dev->glue_suspend = i2c_dw_lpss_suspend;
		dev->glue_resume = i2c_dw_lpss_resume;
		dev->clk_khz = cfg->lpss->clk_rate_hz / 1000;
		dev->get_clk_rate_khz = dw_pci_clk_khz;
		dev->timings.sda_hold_ns = cfg->lpss->sda_hold_ns;
		dev->timings.sda_fall_ns = cfg->lpss->sda_fall_ns;
		dev->timings.scl_fall_ns = cfg->lpss->scl_fall_ns;
		dev->adapter.class = I2C_CLASS_DEPRECATED;
		kprintf(DRIVER_NAME " %s: [%04x:%04x] LPSS I2C (%s), BAR0 0x%lx, caps 0x%x%s\n",
			pname, pdev->vendor_id, pdev->device_id, cfg->platform,
			(unsigned long)bar, caps,
			(caps & LPSS_PRIV_CAPS_NO_IDMA) ? "" : ", iDMA");
	} else {
		const struct dw_legacy_cfg *lc = cfg->legacy;

		nr = dw_legacy_bus_num(pdev, lc, dev);
		if (nr == -ENODEV) {
			ret = -ENODEV;
			goto err_unmap;
		}
		dev->adapter.nr = nr;
		dev->clk_khz = lc->clk_khz;
		dev->get_clk_rate_khz = lc->clk_khz ? dw_pci_clk_khz : NULL;
		i2c_strlcpy(dev->adapter.name, "i2c-designware-pci",
			    sizeof(dev->adapter.name));
		kprintf(DRIVER_NAME " %s: [%04x:%04x] DesignWare I2C (%s)\n",
			pname, pdev->vendor_id, pdev->device_id,
			cfg->platform);
	}
	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (ret < 0) {
		kprintf(DRIVER_NAME " %s: no interrupt (%d), polled\n", pname,
			ret);
		dev->irq = 0;
	} else {
		glue->vectors = ret;
		irq = pci_irq_vector(pdev, 0);
		dev->irq = irq > 0 ? irq : 0;
		dev->irq_kind = pdev->irq_type == PCI_IRQ_MSIX ? "MSI-X" :
				pdev->irq_type == PCI_IRQ_MSI  ? "MSI" :
								 "INTx";
	}

	ret = i2c_dw_fw_parse_and_configure(dev);
	if (ret)
		goto err_irq;
	/* Bay Trail functions in PCI mode can still be the PMIC bus the
	 * P-Unit shares (their ACPI companion has _SEM): arbitrated, or no
	 * adapter. */
	dev->semaphore_idx = -1;
	if (cfg == &cfg_baytrail) {
		ret = i2c_dw_probe_lock_support(dev);
		if (ret)
			goto err_irq;
	}
	i2c_dw_configure_master(dev);

	if (cfg->kind == DW_PCI_LEGACY && cfg->legacy->scl_sda_cfg) {
		const struct dw_scl_sda_cfg *c = cfg->legacy->scl_sda_cfg;

		dev->ss_hcnt = c->ss_hcnt;
		dev->fs_hcnt = c->fs_hcnt;
		dev->ss_lcnt = c->ss_lcnt;
		dev->fs_lcnt = c->fs_lcnt;
		dev->sda_hold_time = c->sda_hold_time;
	}
	/* The platform's hold time in ns, unless firmware gave cycles. */
	if (dev->get_clk_rate_khz && !dev->sda_hold_time &&
	    dev->timings.sda_hold_ns)
		dev->sda_hold_time = (uint32_t)(((uint64_t)dev->clk_khz *
							 dev->timings.sda_hold_ns +
						 500000ull) /
						1000000ull);

	ret = i2c_dw_probe(dev);
	if (ret) {
		i2c_dw_remove_lock_support(dev);
		goto err_irq;
	}
	glue->dev = dev;
	pci_set_drvdata(pdev, glue);
	/* The P-Unit expects its bus powered: no runtime suspend. */
	if (!dev->shared_with_punit)
		i2c_dw_rpm_enable(dev, DW_IC_DEFAULT_AUTOSUSPEND_MS);
	return 0;

err_irq:
	if (glue && glue->vectors > 0)
		pci_free_irq_vectors(pdev);
err_unmap:
	if (dev && dev->lpss_priv)
		lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
	pci_iounmap(pdev, base);
	kfree(glue);
	kfree(dev);
err_put:
	if (companion)
		acpi_dev_put(companion);
	return ret;
}

static void i2c_dw_pci_remove(pci_device_t *pdev)
{
	struct dw_pci_glue *glue = pci_get_drvdata(pdev);
	struct dw_i2c_dev *dev;

	might_sleep();
	if (!glue)
		return;
	dev = glue->dev;
	i2c_dw_remove(dev);
	if (glue->vectors > 0)
		pci_free_irq_vectors(pdev);
	if (dev->lpss_priv && !dev->rpm_suspended)
		lpss_priv_writel(dev, 0, LPSS_PRIV_RESETS);
	pci_iounmap(pdev, dev->base);
	if (dev->acpi_dev)
		acpi_dev_put(dev->acpi_dev);
	pci_set_drvdata(pdev, NULL);
	kfree(dev);
	kfree(glue);
}

static void i2c_dw_pci_shutdown(pci_device_t *pdev)
{
	struct dw_pci_glue *glue = pci_get_drvdata(pdev);

	if (glue)
		i2c_dw_shutdown(glue->dev);
}

static int i2c_dw_pci_suspend(pci_device_t *pdev)
{
	struct dw_pci_glue *glue = pci_get_drvdata(pdev);

	return glue ? i2c_dw_suspend(glue->dev) : 0;
}

static int i2c_dw_pci_resume(pci_device_t *pdev)
{
	struct dw_pci_glue *glue = pci_get_drvdata(pdev);

	return glue ? i2c_dw_resume(glue->dev) : 0;
}

static struct pci_driver i2c_dw_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = i2c_dw_pci_ids,
	.probe = i2c_dw_pci_probe,
	.remove = i2c_dw_pci_remove,
	.shutdown = i2c_dw_pci_shutdown,
	.suspend = i2c_dw_pci_suspend,
	.resume = i2c_dw_pci_resume,
};

int i2c_dw_pci_init(void)
{
	return pci_register_driver(&i2c_dw_pci_driver);
}

void i2c_dw_pci_exit(void)
{
	pci_unregister_driver(&i2c_dw_pci_driver);
}
