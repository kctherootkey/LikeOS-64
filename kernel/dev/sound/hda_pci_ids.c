// LikeOS Intel High Definition Audio - the controller table.
//
// Every HD Audio controller the driver knows by PCI id, with the quirks it
// needs (HDA_F_* from hda.h, HDA_FX_* from hda_regs.h) and a chipset name
// for the log.  Anything else with the HD Audio class code (0x0403) still
// gets a working entry: a per-vendor default for the vendors whose parts
// share their quirks (Intel, NVIDIA, AMD/ATI, Creative, Zhaoxin, ...), and a
// plain specification-conforming default for everyone else.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_regs.h"

/* ---- quirk sets ------------------------------------------------------------- */

/* ICH6..ICH10 southbridges: stream sync at the old offset. */
#define F_ICH (HDA_FX_OLD_SSYNC)
/* Platform controller hubs from Ibex Peak on: snoop control in DEVC. */
#define F_PCH (HDA_F_INTEL_PCH | HDA_F_INTEL_SNOOP)
/* PCHs whose link also carries the display codec of the CPU graphics. */
#define F_PCH_DISP (F_PCH | HDA_F_PCH_PWR_WELL)
/* Skylake and later: extended capabilities, link power, display codec
 * powered from the graphics power well. */
#define F_SKL (F_PCH | HDA_F_SKL_PLUS | HDA_F_PCH_PWR_WELL)
/* Lunar Lake and later: the command ring is not used; immediate commands. */
#define F_LNL (F_SKL | HDA_F_SINGLE_CMD)
/* Haswell/Broadwell display HD Audio inside the graphics device. */
#define F_HSW (HDA_F_HSW_DISPLAY | HDA_F_INTEL_SNOOP | HDA_F_HDMI_ONLY | \
	       HDA_F_PCH_PWR_WELL | HDA_F_CTX_RESET_WAIT)
#define F_BDW (F_HSW | HDA_F_POSFIX_LPIB)
/* System controller hubs (Poulsbo, Oaktrail). */
#define F_SCH (HDA_F_SCH_SNOOP | HDA_F_INTEL_PCH)
/* Atom SoCs with a display codec (Bay Trail, Braswell). */
#define F_ATOM (F_PCH | HDA_F_PCH_PWR_WELL)

/* ATI/AMD southbridges (SB450..SB900, Hudson/Bolton FCH). */
#define F_ATI_SB (HDA_F_AMD | HDA_F_ATI_SNOOP | HDA_FX_NO_TCSEL | HDA_F_POSFIX_LPIB)
/* AMD Zen FCH and later. */
#define F_AMD_SB (HDA_F_AMD | HDA_F_ATI_SNOOP | HDA_FX_NO_TCSEL)
/* Radeon HDMI/DP audio functions. */
#define F_ATI_HDMI (HDA_F_AMD | HDA_FX_NO_TCSEL | HDA_F_POSFIX_LPIB | \
		    HDA_F_HDMI_ONLY | HDA_F_AMD_GCAP_FIX)
#define F_ATI_HDMI_NS (F_ATI_HDMI | HDA_FX_SNOOP_OFF)

/* NVIDIA: nForce MCP southbridges and GeForce/Quadro HDMI functions. */
#define F_NV_MCP (HDA_F_NVIDIA | HDA_F_NVIDIA_SNOOP | HDA_F_NO_MSI | \
		  HDA_FX_CORBRP_SELF_CLEAR)
#define F_NV_HDMI (F_NV_MCP | HDA_F_HDMI_ONLY)

#define F_VIA (HDA_F_POSFIX_COMBO)
#define F_VIA_HDMI (HDA_F_HDMI_ONLY)
#define F_SIS (0)
#define F_ULI (0)
#define F_CTHDA (HDA_F_NO_MSI | HDA_F_POSFIX_LPIB | HDA_F_NO_64BIT | \
		 HDA_F_ALIGN_BDLE_4K | HDA_FX_SNOOP_OFF)
#define F_CTX (HDA_F_CTX_RESET_WAIT | HDA_F_NO_64BIT | HDA_F_POSFIX_LPIB)
#define F_TERA (HDA_F_NO_64BIT)
#define F_CMEDIA (HDA_F_NO_MSI | HDA_F_POSFIX_LPIB | HDA_FX_SNOOP_OFF)
#define F_RDC (0)
#define F_VMW (HDA_F_VM)
#define F_ZX (0)
#define F_ZX_HDMI (HDA_F_POSFIX_LPIB | HDA_F_NO_MSI | HDA_F_NO_64BIT | \
		   HDA_FX_POLL_RIRB | HDA_F_HDMI_ONLY)
#define F_LOONGSON (HDA_FX_NO_TCSEL | HDA_FX_POLL_RIRB | HDA_FX_SDCTL_DWORD | \
		    HDA_F_NO_UNSOL)
#define F_GLENFLY (HDA_F_POSFIX_LPIB | HDA_F_NO_MSI | HDA_F_NO_64BIT | \
		   HDA_FX_POLL_RIRB | HDA_F_HDMI_ONLY)
#define F_HYGON (HDA_F_POSFIX_LPIB | HDA_F_NO_MSI | HDA_FX_SDCTL_DWORD)
#define F_LISUAN (0)

#define V_INTEL 0x8086
#define V_NVIDIA 0x10de
#define V_ATI 0x1002
#define V_AMD 0x1022
#define V_VIA 0x1106
#define V_SIS 0x1039
#define V_ULI 0x10b9
#define V_CREATIVE 0x1102
#define V_TERADICI 0x6549
#define V_CMEDIA 0x13f6
#define V_RDC 0x17f3
#define V_VMWARE 0x15ad
#define V_ZHAOXIN 0x1d17
#define V_LOONGSON 0x0014
#define V_HYGON 0x1d94
#define V_GLENFLY 0x6766
#define V_LISUAN 0x4c54
#define V_REDHAT 0x1af4
#define V_VBOX 0x80ee

static const struct hda_pci_id hda_ids[] = {
	/* ---- Intel I/O controller hubs --------------------------------------- */
	{ V_INTEL, 0x2668, F_ICH, "ICH6" },
	{ V_INTEL, 0x27d8, F_ICH, "ICH7" },
	{ V_INTEL, 0x269a, F_ICH, "ESB2" },
	{ V_INTEL, 0x284b, F_ICH, "ICH8" },
	{ V_INTEL, 0x293e, F_ICH, "ICH9" },
	{ V_INTEL, 0x293f, F_ICH, "ICH9" },
	{ V_INTEL, 0x3a3e, F_ICH, "ICH10" },
	{ V_INTEL, 0x3a6e, F_ICH, "ICH10" },

	/* ---- Intel system controller hubs ------------------------------------- */
	{ V_INTEL, 0x811b, F_SCH | HDA_F_POSFIX_LPIB, "Poulsbo SCH" },
	{ V_INTEL, 0x080a, F_SCH, "Oaktrail SCH" },

	/* ---- Intel platform controller hubs ----------------------------------- */
	{ V_INTEL, 0x3b56, F_PCH, "5 Series/3400 (Ibex Peak)" },
	{ V_INTEL, 0x3b57, F_PCH, "5 Series/3400 (Ibex Peak)" },
	{ V_INTEL, 0x1c20, F_PCH, "6 Series (Cougar Point)" },
	{ V_INTEL, 0x1d20, F_PCH, "C600/X79 (Patsburg)" },
	{ V_INTEL, 0x1e20, F_PCH, "7 Series (Panther Point)" },
	{ V_INTEL, 0x8c20, F_PCH, "8 Series (Lynx Point)" },
	{ V_INTEL, 0x9c20, F_PCH, "8 Series (Lynx Point-LP)" },
	{ V_INTEL, 0x9c21, F_PCH, "8 Series (Lynx Point-LP)" },
	{ V_INTEL, 0x8ca0, F_PCH, "9 Series (Wildcat Point)" },
	{ V_INTEL, 0x9ca0, F_PCH, "Wildcat Point-LP" },
	{ V_INTEL, 0x8d20, F_PCH, "C610/X99 (Wellsburg)" },
	{ V_INTEL, 0x8d21, F_PCH, "C610/X99 (Wellsburg)" },

	/* ---- Haswell/Broadwell display HD Audio -------------------------------- */
	{ V_INTEL, 0x0a0c, F_HSW, "Haswell-ULT display" },
	{ V_INTEL, 0x0c0c, F_HSW, "Haswell display" },
	{ V_INTEL, 0x0d0c, F_HSW, "Haswell (Crystal Well) display" },
	{ V_INTEL, 0x160c, F_BDW, "Broadwell display" },

	/* ---- Intel Atom SoCs ------------------------------------------------- */
	{ V_INTEL, 0x0f04, F_ATOM, "Bay Trail" },
	{ V_INTEL, 0x2284, F_ATOM, "Braswell/Cherry Trail" },
	{ V_INTEL, 0x5a98, F_SKL, "Apollo Lake" },
	{ V_INTEL, 0x1a98, F_SKL, "Broxton-M" },
	{ V_INTEL, 0x3198, F_SKL, "Gemini Lake" },
	{ V_INTEL, 0x4b55, F_SKL, "Elkhart Lake" },
	{ V_INTEL, 0x4b58, F_SKL, "Elkhart Lake" },
	{ V_INTEL, 0x38c8, F_SKL, "Jasper Lake" },
	{ V_INTEL, 0x4dc8, F_SKL, "Jasper Lake-N" },
	{ V_INTEL, 0x98c8, F_SKL, "Lakefield" },

	/* ---- Skylake and later PCHs and SoCs ----------------------------------- */
	{ V_INTEL, 0xa170, F_SKL, "100 Series (Sunrise Point-H)" },
	{ V_INTEL, 0x9d70, F_SKL, "Sunrise Point-LP" },
	{ V_INTEL, 0xa171, F_SKL, "C230 (Sunrise Point-H)" },
	{ V_INTEL, 0x9d71, F_SKL, "Kaby Lake-LP (Sunrise Point-LP)" },
	{ V_INTEL, 0xa2f0, F_SKL, "200 Series (Union Point-H)" },
	{ V_INTEL, 0xa1f0, F_SKL, "C620 (Lewisburg)" },
	{ V_INTEL, 0xa270, F_SKL, "C620 (Lewisburg)" },
	{ V_INTEL, 0xa348, F_SKL, "300 Series (Cannon Point-H)" },
	{ V_INTEL, 0x9dc8, F_SKL, "Cannon Point-LP" },
	{ V_INTEL, 0x02c8, F_SKL, "Comet Lake-LP" },
	{ V_INTEL, 0x06c8, F_SKL, "Comet Lake-H" },
	{ V_INTEL, 0xa3f0, F_SKL, "400 Series (Comet Lake-S)" },
	{ V_INTEL, 0xf0c8, F_SKL, "Comet Lake-R" },
	{ V_INTEL, 0xf1c8, F_SKL, "500 Series (Rocket Lake-S)" },
	{ V_INTEL, 0x34c8, F_SKL, "Ice Lake-LP" },
	{ V_INTEL, 0x3dc8, F_SKL, "Ice Lake-H" },
	{ V_INTEL, 0xa0c8, F_SKL, "Tiger Lake-LP" },
	{ V_INTEL, 0x43c8, F_SKL, "Tiger Lake-H" },
	{ V_INTEL, 0x7ad0, F_SKL, "600 Series (Alder Lake-S)" },
	{ V_INTEL, 0x51c8, F_SKL, "Alder Lake-P" },
	{ V_INTEL, 0x51c9, F_SKL, "Alder Lake-PS" },
	{ V_INTEL, 0x51cc, F_SKL, "Alder Lake-M" },
	{ V_INTEL, 0x51cd, F_SKL, "Alder Lake-PX" },
	{ V_INTEL, 0x54c8, F_SKL, "Alder Lake-N" },
	{ V_INTEL, 0x7a50, F_SKL, "700 Series (Raptor Lake-S)" },
	{ V_INTEL, 0x51ca, F_SKL, "Raptor Lake-P" },
	{ V_INTEL, 0x51cb, F_SKL, "Raptor Lake-P" },
	{ V_INTEL, 0x51ce, F_SKL, "Raptor Lake-M" },
	{ V_INTEL, 0x51cf, F_SKL, "Raptor Lake-PX" },
	{ V_INTEL, 0x7e28, F_SKL, "Meteor Lake" },
	{ V_INTEL, 0x7728, F_SKL, "Arrow Lake" },
	{ V_INTEL, 0x7f50, F_SKL, "800 Series (Arrow Lake-S)" },
	{ V_INTEL, 0xa828, F_LNL, "Lunar Lake" },
	{ V_INTEL, 0xe428, F_LNL, "Panther Lake" },
	{ V_INTEL, 0xe328, F_LNL, "Panther Lake-H" },
	{ V_INTEL, 0x4d28, F_LNL, "Wildcat Lake" },
	{ V_INTEL, 0xd328, F_LNL, "Nova Lake" },
	{ V_INTEL, 0x6e50, F_LNL, "Nova Lake-S" },

	/* ---- Intel discrete graphics HD Audio ------------------------------- */
	{ V_INTEL, 0x490d, F_SKL | HDA_F_HDMI_ONLY, "DG1 graphics" },
	{ V_INTEL, 0x4f90, F_SKL | HDA_F_HDMI_ONLY, "Arc (DG2) graphics" },
	{ V_INTEL, 0x4f91, F_SKL | HDA_F_HDMI_ONLY, "Arc (DG2) graphics" },
	{ V_INTEL, 0x4f92, F_SKL | HDA_F_HDMI_ONLY, "Arc (DG2) graphics" },
	{ V_INTEL, 0xe2f7, F_SKL | HDA_F_HDMI_ONLY, "Arc (Battlemage) graphics" },

	/* ---- ATI/AMD southbridges --------------------------------------------- */
	{ V_ATI, 0x437b, F_ATI_SB, "SB450" },
	{ V_ATI, 0x4383, F_ATI_SB, "SB600/SB700/SB800/SB900" },
	{ V_AMD, 0x780d, F_ATI_SB, "Hudson/Bolton FCH" },
	{ V_AMD, 0x157a, F_ATI_SB, "Stoney Ridge FCH" },
	{ V_AMD, 0x1457, F_AMD_SB, "Family 17h (Zen) FCH" },
	{ V_AMD, 0x1487, F_AMD_SB, "Family 17h (Zen 2) FCH" },
	{ V_AMD, 0x15e3, F_AMD_SB, "Family 17h/19h HD Audio" },

	/* ---- ATI/AMD Radeon HDMI/DP audio ----------------------------------- */
	{ V_ATI, 0x0002, F_ATI_HDMI_NS, "Radeon HDMI" },
	{ V_ATI, 0x1308, F_ATI_HDMI_NS, "Kaveri HDMI/DP" },
	{ V_ATI, 0x157a, F_ATI_HDMI_NS, "Carrizo/Stoney HDMI/DP" },
	{ V_ATI, 0x15b3, F_ATI_HDMI_NS, "Carrizo HDMI/DP" },
	{ V_ATI, 0x15de, F_ATI_HDMI_NS, "Raven HDMI/DP" },
	{ V_ATI, 0x1637, F_ATI_HDMI_NS, "Renoir HDMI/DP" },
	{ V_ATI, 0x1640, F_ATI_HDMI_NS, "Rembrandt HDMI/DP" },
	{ V_ATI, 0x793b, F_ATI_HDMI, "RS600 HDMI" },
	{ V_ATI, 0x7919, F_ATI_HDMI, "RS690 HDMI" },
	{ V_ATI, 0x960f, F_ATI_HDMI, "RS780 HDMI" },
	{ V_ATI, 0x970f, F_ATI_HDMI, "RS880 HDMI" },
	{ V_ATI, 0x9840, F_ATI_HDMI_NS, "Kabini/Mullins HDMI/DP" },
	{ V_ATI, 0x9902, F_ATI_HDMI_NS, "Trinity HDMI/DP" },
	{ V_ATI, 0xaa00, F_ATI_HDMI, "R600 HDMI" },
	{ V_ATI, 0xaa08, F_ATI_HDMI, "RV630 HDMI" },
	{ V_ATI, 0xaa10, F_ATI_HDMI, "RV610 HDMI" },
	{ V_ATI, 0xaa18, F_ATI_HDMI, "RV670 HDMI" },
	{ V_ATI, 0xaa20, F_ATI_HDMI, "RV635 HDMI" },
	{ V_ATI, 0xaa28, F_ATI_HDMI, "RV620 HDMI" },
	{ V_ATI, 0xaa30, F_ATI_HDMI, "RV770 HDMI" },
	{ V_ATI, 0xaa38, F_ATI_HDMI, "RV710 HDMI" },
	{ V_ATI, 0xaa40, F_ATI_HDMI, "RV730 HDMI" },
	{ V_ATI, 0xaa48, F_ATI_HDMI, "RV740 HDMI" },
	{ V_ATI, 0xaa50, F_ATI_HDMI, "Cypress HDMI" },
	{ V_ATI, 0xaa58, F_ATI_HDMI, "Juniper HDMI" },
	{ V_ATI, 0xaa60, F_ATI_HDMI, "Redwood HDMI" },
	{ V_ATI, 0xaa68, F_ATI_HDMI, "Cedar HDMI" },
	{ V_ATI, 0xaa80, F_ATI_HDMI, "Cayman HDMI" },
	{ V_ATI, 0xaa88, F_ATI_HDMI, "Barts HDMI" },
	{ V_ATI, 0xaa90, F_ATI_HDMI, "Turks HDMI" },
	{ V_ATI, 0xaa98, F_ATI_HDMI, "Caicos HDMI" },
	{ V_ATI, 0xaaa0, F_ATI_HDMI_NS, "Tahiti HDMI/DP" },
	{ V_ATI, 0xaaa8, F_ATI_HDMI_NS, "Pitcairn HDMI/DP" },
	{ V_ATI, 0xaab0, F_ATI_HDMI_NS, "Cape Verde/Oland HDMI/DP" },
	{ V_ATI, 0xaac0, F_ATI_HDMI_NS, "Tobago/Bonaire HDMI/DP" },
	{ V_ATI, 0xaac8, F_ATI_HDMI_NS, "Hawaii HDMI/DP" },
	{ V_ATI, 0xaad8, F_ATI_HDMI_NS, "Tonga HDMI/DP" },
	{ V_ATI, 0xaae0, F_ATI_HDMI_NS, "Radeon HDMI/DP" },
	{ V_ATI, 0xaae8, F_ATI_HDMI_NS, "Fiji HDMI/DP" },
	{ V_ATI, 0xaaf0, F_ATI_HDMI_NS, "Ellesmere (Polaris) HDMI/DP" },
	{ V_ATI, 0xaaf8, F_ATI_HDMI_NS, "Vega HDMI/DP" },
	{ V_ATI, 0xab00, F_ATI_HDMI_NS, "Baffin (Polaris) HDMI/DP" },
	{ V_ATI, 0xab08, F_ATI_HDMI_NS, "Polaris 12 HDMI/DP" },
	{ V_ATI, 0xab10, F_ATI_HDMI_NS, "Radeon HDMI/DP" },
	{ V_ATI, 0xab18, F_ATI_HDMI_NS, "Radeon HDMI/DP" },
	{ V_ATI, 0xab20, F_ATI_HDMI_NS, "Vega 20 HDMI/DP" },
	{ V_ATI, 0xab28, F_ATI_HDMI_NS, "Navi HDMI/DP" },
	{ V_ATI, 0xab30, F_ATI_HDMI_NS, "Navi HDMI/DP" },
	{ V_ATI, 0xab38, F_ATI_HDMI_NS, "Navi HDMI/DP" },
	{ V_ATI, 0xab40, F_ATI_HDMI_NS, "Navi HDMI/DP" },

	/* ---- NVIDIA nForce MCP ------------------------------------------------ */
	{ V_NVIDIA, 0x026c, F_NV_MCP, "MCP51" },
	{ V_NVIDIA, 0x0371, F_NV_MCP, "MCP55" },
	{ V_NVIDIA, 0x03e4, F_NV_MCP, "MCP61" },
	{ V_NVIDIA, 0x03f0, F_NV_MCP, "MCP61" },
	{ V_NVIDIA, 0x044a, F_NV_MCP, "MCP65" },
	{ V_NVIDIA, 0x044b, F_NV_MCP, "MCP65" },
	{ V_NVIDIA, 0x055c, F_NV_MCP, "MCP67" },
	{ V_NVIDIA, 0x055d, F_NV_MCP, "MCP67" },
	{ V_NVIDIA, 0x07fc, F_NV_MCP, "MCP73" },
	{ V_NVIDIA, 0x07fd, F_NV_MCP, "MCP73" },
	{ V_NVIDIA, 0x0774, F_NV_MCP, "MCP77/78" },
	{ V_NVIDIA, 0x0775, F_NV_MCP, "MCP77/78" },
	{ V_NVIDIA, 0x0776, F_NV_MCP, "MCP77/78" },
	{ V_NVIDIA, 0x0777, F_NV_MCP, "MCP77/78" },
	{ V_NVIDIA, 0x0ac0, F_NV_MCP, "MCP79" },
	{ V_NVIDIA, 0x0ac1, F_NV_MCP, "MCP79" },
	{ V_NVIDIA, 0x0ac2, F_NV_MCP, "MCP79" },
	{ V_NVIDIA, 0x0ac3, F_NV_MCP, "MCP79" },
	{ V_NVIDIA, 0x0d94, F_NV_MCP, "MCP89" },
	{ V_NVIDIA, 0x0d95, F_NV_MCP, "MCP89" },
	{ V_NVIDIA, 0x0d96, F_NV_MCP, "MCP89" },
	{ V_NVIDIA, 0x0d97, F_NV_MCP, "MCP89" },

	/* ---- NVIDIA GPU HDMI/DP audio functions ------------------------------ */
	{ V_NVIDIA, 0x0be2, F_NV_HDMI, "GT216 HDMI" },
	{ V_NVIDIA, 0x0be3, F_NV_HDMI, "GT218 HDMI" },
	{ V_NVIDIA, 0x0be4, F_NV_HDMI, "GT215 HDMI" },
	{ V_NVIDIA, 0x0be5, F_NV_HDMI, "GF100 HDMI" },
	{ V_NVIDIA, 0x0be9, F_NV_HDMI, "GF106 HDMI" },
	{ V_NVIDIA, 0x0bea, F_NV_HDMI, "GF108 HDMI" },
	{ V_NVIDIA, 0x0beb, F_NV_HDMI, "GF104 HDMI" },
	{ V_NVIDIA, 0x0bee, F_NV_HDMI, "GF116 HDMI" },
	{ V_NVIDIA, 0x0e08, F_NV_HDMI, "GF119 HDMI" },
	{ V_NVIDIA, 0x0e09, F_NV_HDMI, "GF110 HDMI" },
	{ V_NVIDIA, 0x0e0a, F_NV_HDMI, "GK104 HDMI/DP" },
	{ V_NVIDIA, 0x0e0b, F_NV_HDMI, "GK106 HDMI/DP" },
	{ V_NVIDIA, 0x0e0c, F_NV_HDMI, "GF114 HDMI" },
	{ V_NVIDIA, 0x0e0f, F_NV_HDMI, "GK208 HDMI/DP" },
	{ V_NVIDIA, 0x0e1a, F_NV_HDMI, "GK110 HDMI/DP" },
	{ V_NVIDIA, 0x0e1b, F_NV_HDMI, "GK107 HDMI/DP" },
	{ V_NVIDIA, 0x0fb0, F_NV_HDMI, "GM200 HDMI/DP" },
	{ V_NVIDIA, 0x0fb8, F_NV_HDMI, "GP108 HDMI/DP" },
	{ V_NVIDIA, 0x0fb9, F_NV_HDMI, "GP107 HDMI/DP" },
	{ V_NVIDIA, 0x0fba, F_NV_HDMI, "GM206 HDMI/DP" },
	{ V_NVIDIA, 0x0fbb, F_NV_HDMI, "GM204 HDMI/DP" },
	{ V_NVIDIA, 0x0fbc, F_NV_HDMI, "GM107 HDMI/DP" },
	{ V_NVIDIA, 0x10ef, F_NV_HDMI, "GP102 HDMI/DP" },
	{ V_NVIDIA, 0x10f0, F_NV_HDMI, "GP104 HDMI/DP" },
	{ V_NVIDIA, 0x10f1, F_NV_HDMI, "GP106 HDMI/DP" },
	{ V_NVIDIA, 0x10f7, F_NV_HDMI, "TU102 HDMI/DP" },
	{ V_NVIDIA, 0x10f8, F_NV_HDMI, "TU104 HDMI/DP" },
	{ V_NVIDIA, 0x10f9, F_NV_HDMI, "TU106 HDMI/DP" },
	{ V_NVIDIA, 0x1aeb, F_NV_HDMI, "TU116 HDMI/DP" },
	{ V_NVIDIA, 0x1aef, F_NV_HDMI, "GA102 HDMI/DP" },
	{ V_NVIDIA, 0x228b, F_NV_HDMI, "GA104 HDMI/DP" },
	{ V_NVIDIA, 0x228e, F_NV_HDMI, "GA106 HDMI/DP" },
	{ V_NVIDIA, 0x22ba, F_NV_HDMI, "AD102 HDMI/DP" },
	{ V_NVIDIA, 0x22bb, F_NV_HDMI, "AD103 HDMI/DP" },
	{ V_NVIDIA, 0x22bc, F_NV_HDMI, "AD104 HDMI/DP" },

	/* ---- VIA -------------------------------------------------------------- */
	{ V_VIA, 0x3288, F_VIA, "VT8251/VT8237A" },
	{ V_VIA, 0x9170, F_VIA_HDMI, "VX900 HDMI" },
	{ V_VIA, 0x9140, F_VIA_HDMI, "VX11 HDMI" },

	/* ---- others ------------------------------------------------------------ */
	{ V_SIS, 0x7502, F_SIS, "SiS966" },
	{ V_ULI, 0x5461, F_ULI, "ULi M5461" },
	{ V_CREATIVE, 0x0009, F_CTX, "X-Fi (CA0110)" },
	{ V_CREATIVE, 0x0010, F_CTHDA, "Sound Core3D (CA0132)" },
	{ V_CREATIVE, 0x0012, F_CTHDA, "Sound Core3D (CA0132)" },
	{ V_TERADICI, 0x1200, F_TERA, "Teradici" },
	{ V_TERADICI, 0x2200, F_TERA, "Teradici" },
	{ V_CMEDIA, 0x5011, F_CMEDIA, "CM8888" },
	{ V_RDC, 0x3010, F_RDC, "Vortex86MX" },
	{ V_VMWARE, 0x1977, F_VMW, "VMware HD Audio" },
	{ V_ZHAOXIN, 0x3288, F_ZX, "Zhaoxin" },
	{ V_ZHAOXIN, 0x9141, F_ZX_HDMI, "Zhaoxin HDMI" },
	{ V_ZHAOXIN, 0x9142, F_ZX_HDMI, "Zhaoxin HDMI" },
	{ V_ZHAOXIN, 0x9144, F_ZX_HDMI, "Zhaoxin HDMI" },
	{ V_ZHAOXIN, 0x9145, F_ZX_HDMI, "Zhaoxin HDMI" },
	{ V_ZHAOXIN, 0x9146, F_ZX_HDMI, "Zhaoxin HDMI" },
	{ V_LOONGSON, 0x7a07, F_LOONGSON, "Loongson 7A" },
	{ V_LOONGSON, 0x7a37, F_LOONGSON | HDA_F_HDMI_ONLY, "Loongson 7A HDMI" },
	{ V_LISUAN, 0x5010, F_LISUAN, "Lisuan" },
};

#define HDA_NUM_IDS (sizeof(hda_ids) / sizeof(hda_ids[0]))

/* Per-vendor defaults for class matches not in the table. */
static const struct hda_pci_id hda_generic_ids[] = {
	{ V_INTEL, 0xffff, F_PCH, "Intel HD Audio" },
	{ V_NVIDIA, 0xffff, F_NV_HDMI, "NVIDIA HD Audio" },
	{ V_ATI, 0xffff, F_ATI_HDMI, "ATI/AMD HD Audio" },
	{ V_AMD, 0xffff, F_ATI_HDMI, "AMD HD Audio" },
	{ V_CREATIVE, 0xffff, F_CTX, "Creative HD Audio" },
	{ V_ZHAOXIN, 0xffff, F_ZX, "Zhaoxin HD Audio" },
	{ V_GLENFLY, 0xffff, F_GLENFLY, "Glenfly HDMI" },
	{ V_HYGON, 0xffff, F_HYGON, "Hygon HD Audio" },
	{ V_LOONGSON, 0xffff, F_LOONGSON, "Loongson HD Audio" },
	{ V_VIA, 0xffff, F_VIA_HDMI, "VIA HD Audio" },
};

static const struct hda_pci_id hda_generic_any = { 0xffff, 0xffff, 0,
						   "HD Audio" };

#define HDA_NUM_GENERIC (sizeof(hda_generic_ids) / sizeof(hda_generic_ids[0]))

static int hda_class_match(const pci_device_t *pci)
{
	return pci->class_code == 0x04 && pci->subclass == 0x03;
}

int hda_pci_is_hda(const pci_device_t *pci)
{
	if (!pci || pci->vendor_id == 0xffff || pci->vendor_id == 0)
		return 0;
	if (hda_class_match(pci))
		return 1;
	/* A few old parts report the generic audio class (0x0401) or "other
	 * multimedia" (0x0480) while being HD Audio; the table knows them. */
	if (pci->class_code != 0x04)
		return 0;
	for (unsigned i = 0; i < HDA_NUM_IDS; i++)
		if (hda_ids[i].vendor == pci->vendor_id &&
		    hda_ids[i].device == pci->device_id)
			return 1;
	return 0;
}

const struct hda_pci_id *hda_pci_lookup(const pci_device_t *pci)
{
	if (!pci)
		return NULL;
	for (unsigned i = 0; i < HDA_NUM_IDS; i++)
		if (hda_ids[i].vendor == pci->vendor_id &&
		    hda_ids[i].device == pci->device_id)
			return &hda_ids[i];
	if (!hda_class_match(pci))
		return NULL;
	for (unsigned i = 0; i < HDA_NUM_GENERIC; i++)
		if (hda_generic_ids[i].vendor == pci->vendor_id)
			return &hda_generic_ids[i];
	return &hda_generic_any;
}

/* The card name the user sees: what kind of controller, not the exact
 * chipset (that goes into the log and the diagnostics). */
const char *hda_pci_card_name(const struct hda_pci_id *id,
			      const pci_device_t *pci)
{
	uint32_t f = id ? id->flags : 0;
	uint16_t vendor = pci ? pci->vendor_id : 0;

	switch (vendor) {
	case V_INTEL:
		if (f & HDA_F_HSW_DISPLAY)
			return "HDA Intel HDMI";
		if (f & HDA_F_HDMI_ONLY)
			return "HDA Intel Graphics";
		if (f & (HDA_F_INTEL_PCH | HDA_F_SKL_PLUS))
			return "HDA Intel PCH";
		return "HDA Intel";
	case V_NVIDIA:
		return "HDA NVidia";
	case V_ATI:
		if (f & HDA_F_HDMI_ONLY)
			return "HDA ATI HDMI";
		return "HDA ATI SB";
	case V_AMD:
		if (f & HDA_F_HDMI_ONLY)
			return "HDA ATI HDMI";
		return "HD-Audio Generic";
	case V_VIA:
		return (f & HDA_F_HDMI_ONLY) ? "HDA VIA HDMI" : "HDA VIA VT82xx";
	case V_SIS:
		return "HDA SIS966";
	case V_ULI:
		return "HDA ULI M5461";
	case V_CREATIVE:
		return "HDA Creative";
	case V_TERADICI:
		return "HDA Teradici";
	case V_CMEDIA:
		return "HDA C-Media";
	case V_RDC:
		return "HDA RDC";
	case V_VMWARE:
		return "HDA VMware";
	case V_ZHAOXIN:
		return (f & HDA_F_HDMI_ONLY) ? "HDA Zhaoxin HDMI" : "HDA Zhaoxin";
	case V_LOONGSON:
		return "HDA Loongson";
	case V_GLENFLY:
		return "HDA Glenfly HDMI";
	case V_HYGON:
		return "HDA Hygon";
	case V_REDHAT:
	case V_VBOX:
		return "HDA Virtual";
	default:
		return "HD-Audio Generic";
	}
}
