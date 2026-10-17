// LikeOS Intel High Definition Audio - codec identification and the text
// description of a codec (AUDIO_DIAG): vendor and chip name tables, decoders
// for widget capabilities, pin capabilities, the pin default configuration,
// amplifier caps and power states, and the dump of the parsed paths.
//
// Copyright (C) 2026 The LikeOS Project

#include "hda_codec_priv.h"
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>

/* ======================================================================== */
/* names                                                                    */
/* ======================================================================== */

struct hda_vendor_name {
	uint16_t vid;
	const char *name;
};

static const struct hda_vendor_name hda_vendor_names[] = {
	{ 0x0014, "Loongson" },
	{ 0x1002, "ATI" },
	{ 0x1013, "Cirrus Logic" },
	{ 0x1022, "AMD" },
	{ 0x1057, "Motorola" },
	{ 0x1095, "Silicon Image" },
	{ 0x10b9, "ULi" },
	{ 0x10de, "NVIDIA" },
	{ 0x10ec, "Realtek" },
	{ 0x1102, "Creative" },
	{ 0x1106, "VIA" },
	{ 0x111d, "IDT" },
	{ 0x11c1, "LSI" },
	{ 0x11d4, "Analog Devices" },
	{ 0x13f6, "C-Media" },
	{ 0x14f1, "Conexant" },
	{ 0x15ad, "VMware" },
	{ 0x17e8, "Chrontel" },
	{ 0x1854, "LG" },
	{ 0x1aec, "Wolfson" },
	{ 0x1af4, "QEMU" },
	{ 0x1d17, "Zhaoxin" },
	{ 0x434d, "C-Media" },
	{ 0x80ee, "VirtualBox" },
	{ 0x8086, "Intel" },
	{ 0x8384, "SigmaTel" },
};

struct hda_chip_name {
	uint32_t id;
	const char *name;
};

/* Full-id names.  Realtek, VIA, Analog Devices and Cirrus chips without an
 * entry are named from the device id by rule (see hda_chip_by_rule). */
static const struct hda_chip_name hda_chip_names[] = {
	/* Realtek: the ids that do not spell their own name */
	{ 0x10ec0862, "ALC861-VD" },
	{ 0x10ec0660, "ALC660-VD" },
	{ 0x10ec0867, "ALC891" },
	{ 0x10ec0899, "ALC898" },
	{ 0x10ec0900, "ALC1150" },
	{ 0x10ec0b00, "ALCS1200A" },
	{ 0x10ec1168, "ALC1220" },
	{ 0x10ec1220, "ALC1220" },
	{ 0x10ec0283, "ALC283" },
	{ 0x10ec0298, "ALC298" },

	/* Conexant / Synaptics */
	{ 0x14f11f86, "CX8070" },
	{ 0x14f11f87, "SN6140" },
	{ 0x14f120d0, "CX11970" },
	{ 0x14f120d1, "SN6180" },
	{ 0x14f15045, "CX20549 (Venice)" },
	{ 0x14f15047, "CX20551 (Waikiki)" },
	{ 0x14f15051, "CX20561 (Hermosa)" },
	{ 0x14f15066, "CX20582 (Pebble)" },
	{ 0x14f15067, "CX20583 (Pebble HSF)" },
	{ 0x14f15068, "CX20584" },
	{ 0x14f15069, "CX20585" },
	{ 0x14f1506c, "CX20588" },
	{ 0x14f1506e, "CX20590" },
	{ 0x14f15097, "CX20631" },
	{ 0x14f15098, "CX20632" },
	{ 0x14f150a1, "CX20641" },
	{ 0x14f150a2, "CX20642" },
	{ 0x14f150ab, "CX20651" },
	{ 0x14f150ac, "CX20652" },
	{ 0x14f150b8, "CX20664" },
	{ 0x14f150b9, "CX20665" },
	{ 0x14f150f1, "CX21722" },
	{ 0x14f150f2, "CX20722" },
	{ 0x14f150f3, "CX21724" },
	{ 0x14f150f4, "CX20724" },
	{ 0x14f1510f, "CX20751/2" },
	{ 0x14f15110, "CX20751/2" },
	{ 0x14f15111, "CX20753/4" },
	{ 0x14f15113, "CX20755" },
	{ 0x14f15114, "CX20756" },
	{ 0x14f15115, "CX20757" },
	{ 0x14f151d7, "CX20952" },

	/* SigmaTel */
	{ 0x83847690, "STAC9200" },
	{ 0x83847880, "STAC9220 A1" },
	{ 0x83847882, "STAC9220 A2" },
	{ 0x83847680, "STAC9221 A1" },
	{ 0x83847681, "STAC9220D/9223D A2" },
	{ 0x83847682, "STAC9221 A2" },
	{ 0x83847683, "STAC9221D A2" },
	{ 0x83847618, "STAC9227" },
	{ 0x83847619, "STAC9227" },
	{ 0x83847638, "STAC92HD700" },
	{ 0x83847616, "STAC9228" },
	{ 0x83847617, "STAC9228" },
	{ 0x83847614, "STAC9229" },
	{ 0x83847615, "STAC9229" },
	{ 0x83847620, "STAC9274" },
	{ 0x83847621, "STAC9274D" },
	{ 0x83847622, "STAC9273X" },
	{ 0x83847623, "STAC9273D" },
	{ 0x83847624, "STAC9272X" },
	{ 0x83847625, "STAC9272D" },
	{ 0x83847626, "STAC9271X" },
	{ 0x83847627, "STAC9271D" },
	{ 0x83847628, "STAC9274X5NH" },
	{ 0x83847629, "STAC9274D5NH" },
	{ 0x83847632, "STAC9202" },
	{ 0x83847633, "STAC9202D" },
	{ 0x83847634, "STAC9250" },
	{ 0x83847635, "STAC9250D" },
	{ 0x83847636, "STAC9251" },
	{ 0x83847637, "STAC9250D" },
	{ 0x83847645, "92HD206X" },
	{ 0x83847646, "92HD206D" },
	{ 0x83847661, "CXD9872RD/K" },
	{ 0x83847662, "STAC9872AK" },
	{ 0x83847664, "CXD9872AKD" },
	{ 0x83847698, "STAC9205" },
	{ 0x838476a0, "STAC9205" },
	{ 0x838476a1, "STAC9205D" },
	{ 0x838476a2, "STAC9204" },
	{ 0x838476a3, "STAC9204D" },
	{ 0x838476a4, "STAC9255" },
	{ 0x838476a5, "STAC9255D" },
	{ 0x838476a6, "STAC9254" },
	{ 0x838476a7, "STAC9254D" },

	/* IDT */
	{ 0x111d7603, "92HD75B3X5" },
	{ 0x111d7604, "92HD83C1X5" },
	{ 0x111d76d4, "92HD83C1C5" },
	{ 0x111d7605, "92HD81B1X5" },
	{ 0x111d76d5, "92HD81B1C5" },
	{ 0x111d76d1, "92HD87B1/3" },
	{ 0x111d76d9, "92HD87B2/4" },
	{ 0x111d7666, "92HD88B3" },
	{ 0x111d7667, "92HD88B1" },
	{ 0x111d7668, "92HD88B2" },
	{ 0x111d7669, "92HD88B4" },
	{ 0x111d7608, "92HD75B2X5" },
	{ 0x111d7674, "92HD73D1X5" },
	{ 0x111d7675, "92HD73C1X5" },
	{ 0x111d7676, "92HD73E1X5" },
	{ 0x111d7695, "92HD95" },
	{ 0x111d76b0, "92HD71B8X" },
	{ 0x111d76b1, "92HD71B8X" },
	{ 0x111d76b2, "92HD71B7X" },
	{ 0x111d76b3, "92HD71B7X" },
	{ 0x111d76b4, "92HD71B6X" },
	{ 0x111d76b5, "92HD71B6X" },
	{ 0x111d76b6, "92HD71B5X" },
	{ 0x111d76b7, "92HD71B5X" },
	{ 0x111d76c0, "92HD89C3" },
	{ 0x111d76c1, "92HD89C2" },
	{ 0x111d76c2, "92HD89C1" },
	{ 0x111d76c3, "92HD89B3" },
	{ 0x111d76c4, "92HD89B2" },
	{ 0x111d76c5, "92HD89B1" },
	{ 0x111d76c6, "92HD89E3" },
	{ 0x111d76c7, "92HD89E2" },
	{ 0x111d76c8, "92HD89E1" },
	{ 0x111d76c9, "92HD89D3" },
	{ 0x111d76ca, "92HD89D2" },
	{ 0x111d76cb, "92HD89D1" },
	{ 0x111d76cc, "92HD89F3" },
	{ 0x111d76cd, "92HD89F2" },
	{ 0x111d76ce, "92HD89F1" },
	{ 0x111d76df, "92HD93BXX" },
	{ 0x111d76e0, "92HD91BXX" },
	{ 0x111d76e3, "92HD98BXX" },
	{ 0x111d76e5, "92HD99BXX" },
	{ 0x111d76e7, "92HD90BXX" },
	{ 0x111d76e8, "92HD66B1X5" },
	{ 0x111d76e9, "92HD66B2X5" },
	{ 0x111d76ea, "92HD66B3X5" },
	{ 0x111d76eb, "92HD66C1X5" },
	{ 0x111d76ec, "92HD66C2X5" },
	{ 0x111d76ed, "92HD66C3X5" },
	{ 0x111d76ee, "92HD66B1X3" },
	{ 0x111d76ef, "92HD66C3/65" },
	{ 0x111d76f3, "92HD66B3X3" },
	{ 0x111d76f4, "92HD66C1X3" },
	{ 0x111d76f5, "92HD66C3X3_A" },
	{ 0x111d76f6, "92HD66C3X3" },

	/* Analog Devices */
	{ 0x11d41882, "AD1882" },
	{ 0x11d4882a, "AD1882A" },
	{ 0x11d41883, "AD1883" },
	{ 0x11d41884, "AD1884" },
	{ 0x11d4184a, "AD1884A" },
	{ 0x11d41981, "AD1981" },
	{ 0x11d41983, "AD1983" },
	{ 0x11d41984, "AD1984" },
	{ 0x11d4194a, "AD1984A" },
	{ 0x11d4194b, "AD1984B" },
	{ 0x11d41986, "AD1986A" },
	{ 0x11d41987, "AD1987" },
	{ 0x11d41988, "AD1988" },
	{ 0x11d4198b, "AD1988B" },
	{ 0x11d4989a, "AD1989A" },
	{ 0x11d4989b, "AD1989B" },

	/* Cirrus Logic */
	{ 0x10134206, "CS4206" },
	{ 0x10134207, "CS4207" },
	{ 0x10134208, "CS4208" },
	{ 0x10134210, "CS4210" },
	{ 0x10134213, "CS4213" },
	{ 0x10138409, "CS8409/CS42L42" },

	/* VIA */
	{ 0x11061708, "VT1708" },
	{ 0x11061709, "VT1708" },
	{ 0x1106170a, "VT1708" },
	{ 0x1106170b, "VT1708" },
	{ 0x1106e710, "VT1709 10-Ch" },
	{ 0x1106e711, "VT1709 10-Ch" },
	{ 0x1106e712, "VT1709 10-Ch" },
	{ 0x1106e713, "VT1709 10-Ch" },
	{ 0x1106e714, "VT1709 6-Ch" },
	{ 0x1106e715, "VT1709 6-Ch" },
	{ 0x1106e716, "VT1709 6-Ch" },
	{ 0x1106e717, "VT1709 6-Ch" },
	{ 0x1106e720, "VT1708B 8-Ch" },
	{ 0x1106e721, "VT1708B 8-Ch" },
	{ 0x1106e722, "VT1708B 8-Ch" },
	{ 0x1106e723, "VT1708B 8-Ch" },
	{ 0x1106e724, "VT1708B 4-Ch" },
	{ 0x1106e725, "VT1708B 4-Ch" },
	{ 0x1106e726, "VT1708B 4-Ch" },
	{ 0x1106e727, "VT1708B 4-Ch" },
	{ 0x11060397, "VT1708S" },
	{ 0x11061397, "VT1708S" },
	{ 0x11062397, "VT1708S" },
	{ 0x11063397, "VT1708S" },
	{ 0x11064397, "VT1705" },
	{ 0x11065397, "VT1708S" },
	{ 0x11066397, "VT1708S" },
	{ 0x11067397, "VT1708S" },
	{ 0x11060398, "VT1702" },
	{ 0x11061398, "VT1702" },
	{ 0x11062398, "VT1702" },
	{ 0x11063398, "VT1702" },
	{ 0x11064398, "VT1702" },
	{ 0x11065398, "VT1702" },
	{ 0x11066398, "VT1702" },
	{ 0x11067398, "VT1702" },
	{ 0x11060428, "VT1718S" },
	{ 0x11064428, "VT1718S" },
	{ 0x11060441, "VT2020" },
	{ 0x11064441, "VT1828S" },
	{ 0x11060433, "VT1716S" },
	{ 0x1106a721, "VT1716S" },
	{ 0x11060438, "VT2002P" },
	{ 0x11064438, "VT2002P" },
	{ 0x11060448, "VT1812" },
	{ 0x11060440, "VT1818S" },
	{ 0x11060446, "VT1802" },
	{ 0x11068446, "VT1802" },
	{ 0x11064760, "VT1705CF" },
	{ 0x11064761, "VT1708S" },
	{ 0x11064762, "VT1808" },
	{ 0x11069f80, "VX900 HDMI/DP" },
	{ 0x11069f81, "VX900 HDMI/DP" },
	{ 0x11069f84, "VX11 HDMI/DP" },
	{ 0x11069f85, "VX11 HDMI/DP" },

	/* Creative */
	{ 0x1102000a, "CA0110-IBG" },
	{ 0x1102000b, "CA0110-IBG" },
	{ 0x1102000d, "SB0880 X-Fi" },
	{ 0x11020010, "CA0132" },
	{ 0x11020011, "CA0132" },
	{ 0x11020018, "CA0132" },

	/* C-Media */
	{ 0x434d4980, "CMI9880" },
	{ 0x13f69880, "CMI9880" },

	/* Intel display audio */
	{ 0x80862802, "G45 HDMI" },
	{ 0x80862803, "Eaglelake HDMI" },
	{ 0x80862804, "IbexPeak HDMI" },
	{ 0x80862805, "CougarPoint HDMI" },
	{ 0x80862806, "PantherPoint HDMI" },
	{ 0x80862807, "Haswell HDMI" },
	{ 0x80862808, "Broadwell HDMI" },
	{ 0x80862809, "Skylake HDMI" },
	{ 0x8086280a, "Broxton HDMI" },
	{ 0x8086280b, "Kabylake HDMI" },
	{ 0x8086280c, "Cannonlake HDMI" },
	{ 0x8086280d, "Geminilake HDMI" },
	{ 0x8086280f, "Icelake HDMI" },
	{ 0x80862812, "Tigerlake HDMI" },
	{ 0x80862814, "DG1 HDMI" },
	{ 0x80862815, "Alderlake HDMI" },
	{ 0x80862816, "Rocketlake HDMI" },
	{ 0x80862818, "Raptorlake HDMI" },
	{ 0x80862819, "DG2 HDMI" },
	{ 0x8086281a, "Jasperlake HDMI" },
	{ 0x8086281b, "Elkhartlake HDMI" },
	{ 0x8086281c, "Alderlake-P HDMI" },
	{ 0x8086281d, "Meteorlake HDMI" },
	{ 0x8086281e, "Battlemage HDMI" },
	{ 0x8086281f, "Raptorlake-P HDMI" },
	{ 0x80862820, "Lunarlake HDMI" },
	{ 0x80862822, "Pantherlake HDMI" },
	{ 0x80862880, "CedarTrail HDMI" },
	{ 0x80862882, "Valleyview2 HDMI" },
	{ 0x80862883, "Braswell HDMI" },

	/* NVIDIA chipsets of the HDMI-on-southbridge era */
	{ 0x10de0002, "MCP77/78 HDMI" },
	{ 0x10de0003, "MCP77/78 HDMI" },
	{ 0x10de0005, "MCP77/78 HDMI" },
	{ 0x10de0006, "MCP77/78 HDMI" },
	{ 0x10de0007, "MCP79/7A HDMI" },
	{ 0x10de0067, "MCP67 HDMI" },
	{ 0x10de8001, "MCP73 HDMI" },
	{ 0x10de8067, "MCP67/68 HDMI" },

	/* AMD/ATI */
	{ 0x1002793c, "RS690/780 HDMI" },
	{ 0x1002791a, "RS690/780 HDMI" },
	{ 0x1002aa01, "R6xx HDMI" },
	{ 0x10021308, "Kaveri DP" },
	{ 0x10029902, "Kabini HDMI" },

	/* others */
	{ 0x10951390, "SiI1390 HDMI" },
	{ 0x10951392, "SiI1392 HDMI" },
	{ 0x17e80047, "Chrontel HDMI" },
};

/* Chips whose name follows from the device id. */
static int hda_chip_by_rule(struct hda_codec *c, char *out, size_t n)
{
	uint16_t vid = (uint16_t)(c->vendor_id >> 16);
	uint16_t did = (uint16_t)(c->vendor_id & 0xffff);

	switch (vid) {
	case 0x10ec: /* Realtek: device 0x0269 is the ALC269 */
		if (did < 0x1000) {
			ksnprintf(out, n, "ALC%x", did);
			if (did == 0x0662 && c->revision_id == 0x100002)
				hda_strlcpy(out, "ALC662 rev2", n);
			return 1;
		}
		ksnprintf(out, n, "ALC%04x", did);
		return 1;
	case 0x1106:
		ksnprintf(out, n, "VT%04x", did);
		return 1;
	case 0x11d4:
		ksnprintf(out, n, "AD%04x", did);
		return 1;
	case 0x1013:
		ksnprintf(out, n, "CS%04x", did);
		return 1;
	case 0x10de:
		ksnprintf(out, n, "GPU %02x HDMI/DP", did & 0xff);
		return 1;
	case 0x1002:
		ksnprintf(out, n, "Radeon HDMI/DP %04x", did);
		return 1;
	case 0x8086:
		ksnprintf(out, n, "HDMI/DP %04x", did);
		return 1;
	case 0x1d17:
		ksnprintf(out, n, "ZX HDMI/DP %04x", did);
		return 1;
	case 0x1af4:
		/* QEMU's emulated codecs: output only, duplex, micro
		 * (with or without the mixer emulation). */
		switch (did & 0xf0) {
		case 0x10:
			hda_strlcpy(out, "hda-output", n);
			return 1;
		case 0x20:
			hda_strlcpy(out, "hda-duplex", n);
			return 1;
		case 0x30:
			hda_strlcpy(out, "hda-micro", n);
			return 1;
		default:
			ksnprintf(out, n, "hda-codec %04x", did);
			return 1;
		}
	case 0x15ad:
		ksnprintf(out, n, "virtual codec %04x", did);
		return 1;
	case 0x80ee:
		ksnprintf(out, n, "virtual codec %04x", did);
		return 1;
	case 0x14f1:
		ksnprintf(out, n, "CX %04x", did);
		return 1;
	case 0x8384:
	case 0x111d:
		ksnprintf(out, n, "ID %04x", did);
		return 1;
	default:
		return 0;
	}
}

void hda_codec_set_names(struct hda_codec *c)
{
	uint16_t vid = (uint16_t)(c->vendor_id >> 16);
	const char *vn = 0;

	c->vendor_name[0] = '\0';
	c->chip_name[0] = '\0';
	for (unsigned i = 0;
	     i < sizeof(hda_vendor_names) / sizeof(hda_vendor_names[0]); i++)
		if (hda_vendor_names[i].vid == vid) {
			vn = hda_vendor_names[i].name;
			break;
		}
	if (vn)
		hda_strlcpy(c->vendor_name, vn, sizeof(c->vendor_name));
	else
		ksnprintf(c->vendor_name, sizeof(c->vendor_name),
			  "Vendor 0x%04x", vid);

	for (unsigned i = 0;
	     i < sizeof(hda_chip_names) / sizeof(hda_chip_names[0]); i++)
		if (hda_chip_names[i].id == c->vendor_id) {
			hda_strlcpy(c->chip_name, hda_chip_names[i].name,
				    sizeof(c->chip_name));
			return;
		}
	if (hda_chip_by_rule(c, c->chip_name, sizeof(c->chip_name)))
		return;
	ksnprintf(c->chip_name, sizeof(c->chip_name), "device 0x%04x",
		  c->vendor_id & 0xffff);
}

/* ======================================================================== */
/* field decoders                                                           */
/* ======================================================================== */

const char *hda_widget_type_name(uint32_t type)
{
	static const char *const names[16] = {
		"Audio Output", "Audio Input", "Audio Mixer",
		"Audio Selector", "Pin Complex", "Power Widget",
		"Volume Knob Widget", "Beep Generator Widget", 0, 0, 0, 0, 0,
		0, 0, "Vendor Defined Widget"
	};

	if (type < 16 && names[type])
		return names[type];
	return "Unknown Widget";
}

const char *hda_pincfg_port_name(uint32_t cfg)
{
	static const char *const n[4] = { "Jack", "N/A", "Fixed", "Both" };

	return n[hda_cfg_port(cfg) & 3];
}

const char *hda_pincfg_location_name(uint32_t cfg)
{
	uint32_t loc = hda_cfg_location(cfg);

	/* the combinations with a name of their own */
	switch (loc) {
	case 0x07:
		return "Ext Rear Panel";
	case 0x08:
		return "Ext Drive Bay";
	case 0x17:
		return "Int Riser";
	case 0x18:
		return "Int Digital Display";
	case 0x19:
		return "Int ATAPI";
	case 0x37:
		return "Mobile Lid Inside";
	case 0x38:
		return "Mobile Lid Outside";
	default:
		break;
	}
	switch (loc & 0x30) {
	case AC_JACK_LOC_EXTERNAL:
		switch (loc & 0xf) {
		case 0:
			return "Ext N/A";
		case 1:
			return "Ext Rear";
		case 2:
			return "Ext Front";
		case 3:
			return "Ext Left";
		case 4:
			return "Ext Right";
		case 5:
			return "Ext Top";
		case 6:
			return "Ext Bottom";
		default:
			return "Ext Special";
		}
	case AC_JACK_LOC_INTERNAL:
		return (loc & 0xf) ? "Int Special" : "Int N/A";
	case AC_JACK_LOC_SEPARATE:
		return (loc & 0xf) == 1 ? "Sep Rear" :
		       (loc & 0xf) == 2 ? "Sep Front" :
					  "Sep (dock)";
	default:
		return "Other";
	}
}

const char *hda_pincfg_device_name(uint32_t cfg)
{
	static const char *const n[16] = {
		"Line Out", "Speaker", "HP Out", "CD", "SPDIF Out",
		"Digital Out", "Modem Line", "Modem Hand", "Line In", "Aux",
		"Mic", "Telephony", "SPDIF In", "Digital In", "Reserved",
		"Other"
	};

	return n[hda_cfg_device(cfg) & 0xf];
}

const char *hda_pincfg_conn_name(uint32_t cfg)
{
	static const char *const n[16] = {
		"Unknown", "1/8", "1/4", "ATAPI", "RCA", "Optical",
		"Digital", "Analog", "Multi", "XLR", "RJ11", "Comb",
		"0xc", "0xd", "0xe", "Other"
	};

	return n[hda_cfg_conn_type(cfg) & 0xf];
}

const char *hda_pincfg_color_name(uint32_t cfg)
{
	static const char *const n[16] = {
		"Unknown", "Black", "Grey", "Blue", "Green", "Red",
		"Orange", "Yellow", "Purple", "Pink", "0xa", "0xb",
		"0xc", "0xd", "White", "Other"
	};

	return n[hda_cfg_color(cfg) & 0xf];
}

static const char *out_type_name(uint32_t t)
{
	switch (t) {
	case AUDIO_OUT_SPEAKER:
		return "speaker";
	case AUDIO_OUT_HEADPHONE:
		return "headphone";
	case AUDIO_OUT_LINE:
		return "line";
	case AUDIO_OUT_HDMI:
		return "hdmi";
	case AUDIO_OUT_DP:
		return "dp";
	case AUDIO_OUT_SPDIF:
		return "spdif";
	case AUDIO_OUT_HEADSET:
		return "headset";
	default:
		return "?";
	}
}

/* ======================================================================== */
/* dump                                                                     */
/* ======================================================================== */

struct dbuf {
	char *b;
	size_t len;
	size_t pos;
};

static void dp(struct dbuf *d, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void dp(struct dbuf *d, const char *fmt, ...)
{
	va_list ap;
	size_t room;
	int n;

	if (!d->b || d->pos + 1 >= d->len)
		return;
	room = d->len - d->pos;
	__builtin_va_start(ap, fmt);
	n = kvsnprintf(d->b + d->pos, room, fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return;
	/* kvsnprintf reports the untruncated length */
	if ((size_t)n >= room)
		n = (int)(room - 1);
	d->pos += (size_t)n;
}

static void dump_rates(struct dbuf *d, uint32_t pcm)
{
	static const uint32_t rate[12] = { 8000,  11025, 16000,	 22050,
					   32000, 44100, 48000,	 88200,
					   96000, 176400, 192000, 384000 };

	dp(d, "    rates [0x%x]:", pcm & AC_SUPPCM_RATES);
	for (int i = 0; i < 12; i++)
		if (pcm & (1u << i))
			dp(d, " %u", rate[i]);
	dp(d, "\n    bits [0x%x]:", (pcm >> 16) & 0x1f);
	if (pcm & AC_SUPPCM_BITS_8)
		dp(d, " 8");
	if (pcm & AC_SUPPCM_BITS_16)
		dp(d, " 16");
	if (pcm & AC_SUPPCM_BITS_20)
		dp(d, " 20");
	if (pcm & AC_SUPPCM_BITS_24)
		dp(d, " 24");
	if (pcm & AC_SUPPCM_BITS_32)
		dp(d, " 32");
	dp(d, "\n");
}

static void dump_formats(struct dbuf *d, uint32_t f)
{
	dp(d, "    formats [0x%x]:", f);
	if (f & AC_SUPFMT_PCM)
		dp(d, " PCM");
	if (f & AC_SUPFMT_FLOAT32)
		dp(d, " FLOAT");
	if (f & AC_SUPFMT_AC3)
		dp(d, " AC3");
	dp(d, "\n");
}

static void dump_amp_caps(struct dbuf *d, const char *what, uint32_t caps)
{
	if (!caps) {
		dp(d, "  %s caps: N/A\n", what);
		return;
	}
	dp(d,
	   "  %s caps: ofs=0x%02x, nsteps=0x%02x, stepsize=0x%02x, mute=%u",
	   what, hda_amp_offset(caps), hda_amp_nsteps(caps),
	   hda_amp_stepsize(caps), (caps & AC_AMPCAP_MUTE) ? 1u : 0u);
	if (hda_amp_nsteps(caps)) {
		int32_t lo = hda_amp_step_db(caps, 0);
		int32_t hi = hda_amp_step_db(caps, hda_amp_nsteps(caps));

		dp(d, " (%s%d.%02d .. %s%d.%02d dB)", lo < 0 ? "-" : "",
		   (lo < 0 ? -lo : lo) / 100, (lo < 0 ? -lo : lo) % 100,
		   hi < 0 ? "-" : "", (hi < 0 ? -hi : hi) / 100,
		   (hi < 0 ? -hi : hi) % 100);
	}
	dp(d, "\n");
}

static void dump_amp_vals(struct dbuf *d, struct hda_codec *c,
			  struct hda_widget *w, int dir)
{
	int n = 1;

	if (dir == HDA_INPUT && w->type != AC_WID_PIN && w->nconn > 0)
		n = w->nconn;
	if (n > 16)
		n = 16;
	dp(d, "  Amp-%s vals:", dir == HDA_OUTPUT ? "Out" : "In");
	for (int i = 0; i < n; i++) {
		uint32_t l = hda_amp_get(c, w->nid, dir, i, 1);
		uint32_t r = hda_amp_get(c, w->nid, dir, i, 0);

		if (w->caps & AC_WCAP_STEREO)
			dp(d, " [0x%02x 0x%02x]", l & 0xff, r & 0xff);
		else
			dp(d, " [0x%02x]", l & 0xff);
	}
	dp(d, "\n");
}

static void dump_wcaps(struct dbuf *d, uint32_t caps)
{
	if (caps & AC_WCAP_STEREO)
		dp(d, " Stereo");
	else
		dp(d, " Mono");
	if (caps & AC_WCAP_IN_AMP)
		dp(d, " Amp-In");
	if (caps & AC_WCAP_OUT_AMP)
		dp(d, " Amp-Out");
	if (caps & AC_WCAP_AMP_OVRD)
		dp(d, " AmpOvrd");
	if (caps & AC_WCAP_FORMAT_OVRD)
		dp(d, " FmtOvrd");
	if (caps & AC_WCAP_STRIPE)
		dp(d, " Stripe");
	if (caps & AC_WCAP_PROC_WID)
		dp(d, " Proc");
	if (caps & AC_WCAP_UNSOL_CAP)
		dp(d, " Unsol");
	if (caps & AC_WCAP_DIGITAL)
		dp(d, " Digital");
	if (caps & AC_WCAP_POWER)
		dp(d, " PwrCtl");
	if (caps & AC_WCAP_LR_SWAP)
		dp(d, " R/L");
	if (caps & AC_WCAP_CP_CAPS)
		dp(d, " CP");
	if (caps & AC_WCAP_DELAY)
		dp(d, " Delay=%u", (caps & AC_WCAP_DELAY) >> AC_WCAP_DELAY_SHIFT);
}

static void dump_pincap(struct dbuf *d, uint32_t pc)
{
	uint32_t v = (pc & AC_PINCAP_VREF) >> AC_PINCAP_VREF_SHIFT;

	dp(d, "  Pincap 0x%08x:", pc);
	if (pc & AC_PINCAP_IN)
		dp(d, " IN");
	if (pc & AC_PINCAP_OUT)
		dp(d, " OUT");
	if (pc & AC_PINCAP_HP_DRV)
		dp(d, " HP");
	if (pc & AC_PINCAP_EAPD)
		dp(d, " EAPD");
	if (pc & AC_PINCAP_PRES_DETECT)
		dp(d, " Detect");
	if (pc & AC_PINCAP_TRIG_REQ)
		dp(d, " Trigger");
	if (pc & AC_PINCAP_IMP_SENSE)
		dp(d, " ImpSense");
	if (pc & AC_PINCAP_BALANCE)
		dp(d, " Balanced");
	if (pc & AC_PINCAP_HDMI)
		dp(d, " HDMI");
	if (pc & AC_PINCAP_DP)
		dp(d, " DP");
	if (pc & AC_PINCAP_HBR)
		dp(d, " HBR");
	dp(d, "\n");
	if (v) {
		dp(d, "    Vref caps:");
		if (v & (1u << AC_PINCTL_VREF_HIZ))
			dp(d, " HIZ");
		if (v & (1u << AC_PINCTL_VREF_50))
			dp(d, " 50");
		if (v & (1u << AC_PINCTL_VREF_GRD))
			dp(d, " GRD");
		if (v & (1u << AC_PINCTL_VREF_80))
			dp(d, " 80");
		if (v & (1u << AC_PINCTL_VREF_100))
			dp(d, " 100");
		dp(d, "\n");
	}
}

static void dump_pincfg(struct dbuf *d, const char *label, uint32_t cfg)
{
	uint32_t misc = hda_cfg_misc(cfg);

	dp(d, "  %s 0x%08x: [%s] %s at %s\n", label, cfg,
	   hda_pincfg_port_name(cfg), hda_pincfg_device_name(cfg),
	   hda_pincfg_location_name(cfg));
	dp(d, "    Conn = %s, Color = %s\n", hda_pincfg_conn_name(cfg),
	   hda_pincfg_color_name(cfg));
	dp(d, "    DefAssociation = 0x%x, Sequence = 0x%x\n",
	   hda_cfg_assoc(cfg), hda_cfg_seq(cfg));
	if (misc & AC_DEFCFG_MISC_NO_PRESENCE)
		dp(d, "    Misc = NO_PRESENCE\n");
}

static void dump_pinctl(struct dbuf *d, struct hda_widget *w, uint32_t ctl)
{
	dp(d, "  Pin-ctls: 0x%02x:", ctl & 0xff);
	if (ctl & AC_PINCTL_IN_EN)
		dp(d, " IN");
	if (ctl & AC_PINCTL_OUT_EN)
		dp(d, " OUT");
	if (ctl & AC_PINCTL_HP_EN)
		dp(d, " HP");
	if ((w->caps & AC_WCAP_DIGITAL) ||
	    (w->pincap & (AC_PINCAP_HDMI | AC_PINCAP_DP))) {
		if ((ctl & AC_PINCTL_EPT) == AC_PINCTL_EPT_HBR)
			dp(d, " HBR");
	} else if (w->pincap & AC_PINCAP_VREF) {
		switch (ctl & AC_PINCTL_VREFEN) {
		case AC_PINCTL_VREF_HIZ:
			dp(d, " VREF_HIZ");
			break;
		case AC_PINCTL_VREF_50:
			dp(d, " VREF_50");
			break;
		case AC_PINCTL_VREF_GRD:
			dp(d, " VREF_GRD");
			break;
		case AC_PINCTL_VREF_80:
			dp(d, " VREF_80");
			break;
		case AC_PINCTL_VREF_100:
			dp(d, " VREF_100");
			break;
		default:
			dp(d, " VREF_?");
			break;
		}
	}
	dp(d, "\n");
}

static void dump_power(struct dbuf *d, struct hda_codec *c, uint8_t nid,
		       uint32_t sup)
{
	static const char *const st[4] = { "D0", "D1", "D2", "D3" };
	uint32_t cur = hda_codec_read(c, nid, AC_VERB_GET_POWER_STATE, 0);

	dp(d, "  Power states:");
	if (sup & AC_PWRST_D0SUP)
		dp(d, " D0");
	if (sup & AC_PWRST_D1SUP)
		dp(d, " D1");
	if (sup & AC_PWRST_D2SUP)
		dp(d, " D2");
	if (sup & AC_PWRST_D3SUP)
		dp(d, " D3");
	if (sup & AC_PWRST_D3COLDSUP)
		dp(d, " D3cold");
	if (sup & AC_PWRST_S3D3COLDSUP)
		dp(d, " S3D3cold");
	if (sup & AC_PWRST_CLKSTOP)
		dp(d, " CLKSTOP");
	if (sup & AC_PWRST_EPSS)
		dp(d, " EPSS");
	dp(d, "\n");
	if (cur == 0xffffffffu) {
		dp(d, "  Power: (no response)\n");
		return;
	}
	dp(d, "  Power: setting=%s, actual=%s%s%s\n", st[cur & 3],
	   st[(cur >> AC_PWRST_ACTUAL_SHIFT) & 3],
	   (cur & AC_PWRST_ERROR) ? ", Error" : "",
	   (cur & AC_PWRST_CLK_STOP_OK) ? ", Clock-stop-OK" : "");
}

static void dump_conn(struct dbuf *d, struct hda_codec *c,
		      struct hda_widget *w)
{
	int sel = -1;

	if (!w->nconn)
		return;
	if (w->type != AC_WID_AUD_MIX && w->nconn > 1) {
		uint32_t s = hda_codec_read(c, w->nid, AC_VERB_GET_CONNECT_SEL,
					    0);

		if (s != 0xffffffffu)
			sel = (int)(s & 0xff);
	} else if (w->type != AC_WID_AUD_MIX) {
		sel = 0;
	}
	dp(d, "  Connection: %u\n    ", w->nconn);
	for (int i = 0; i < w->nconn; i++)
		dp(d, " 0x%02x%s", w->conn[i], i == sel ? "*" : "");
	dp(d, "\n");
}

static void dump_node(struct dbuf *d, struct hda_codec *c,
		      struct hda_widget *w)
{
	uint32_t caps = w->caps;

	dp(d, "Node 0x%02x [%s] wcaps 0x%x:", w->nid,
	   hda_widget_type_name(w->type), caps);
	dump_wcaps(d, caps);
	dp(d, "\n");
	if (hda_widget_channels(w) > 2)
		dp(d, "  Channels: %u\n", hda_widget_channels(w));
	if (w->used)
		dp(d, "  (on a parsed path)\n");

	if (caps & AC_WCAP_IN_AMP) {
		dump_amp_caps(d, "Amp-In", hda_amp_caps(c, w->nid, HDA_INPUT));
		dump_amp_vals(d, c, w, HDA_INPUT);
	}
	if (caps & AC_WCAP_OUT_AMP) {
		dump_amp_caps(d, "Amp-Out",
			      hda_amp_caps(c, w->nid, HDA_OUTPUT));
		dump_amp_vals(d, c, w, HDA_OUTPUT);
	}

	switch (w->type) {
	case AC_WID_AUD_OUT:
	case AC_WID_AUD_IN: {
		uint32_t conv = hda_codec_read(c, w->nid, AC_VERB_GET_CONV, 0);
		uint32_t fmt = hda_codec_read(c, w->nid,
					      AC_VERB_GET_STREAM_FORMAT, 0);

		if (conv != 0xffffffffu)
			dp(d, "  Converter: stream=%u, channel=%u\n",
			   (conv & AC_CONV_STREAM) >> AC_CONV_STREAM_SHIFT,
			   conv & AC_CONV_CHANNEL);
		if (fmt != 0xffffffffu)
			dp(d, "  Format: 0x%04x\n", fmt & 0xffff);
		dp(d, "  PCM:%s\n", w->pcm ? "" : " (function group default)");
		dump_rates(d, hda_conv_pcm(c, w->nid));
		dump_formats(d, hda_conv_stream_fmts(c, w->nid));
		if (caps & AC_WCAP_DIGITAL) {
			uint32_t d1 = hda_codec_read(
				c, w->nid, AC_VERB_GET_DIGI_CONVERT_1, 0);

			if (d1 != 0xffffffffu)
				dp(d, "  Digital: 0x%02x%s%s, category 0x%02x\n",
				   d1 & 0xff,
				   (d1 & AC_DIG1_ENABLE) ? " Enabled" : "",
				   (d1 & AC_DIG1_NONAUDIO) ? " Non-Audio" : "",
				   (d1 >> 8) & AC_DIG2_CC);
		}
		break;
	}
	case AC_WID_PIN: {
		uint32_t ctl;

		dump_pincap(d, w->pincap);
		if (w->pincap & AC_PINCAP_EAPD) {
			uint32_t e = hda_codec_read(
				c, w->nid, AC_VERB_GET_EAPD_BTLENABLE, 0);

			if (e != 0xffffffffu)
				dp(d, "  EAPD 0x%x:%s%s%s\n", e & 0xff,
				   (e & AC_EAPDBTL_BALANCED) ? " BALANCED" :
							       "",
				   (e & AC_EAPDBTL_EAPD) ? " EAPD" : "",
				   (e & AC_EAPDBTL_LR_SWAP) ? " R/L" : "");
		}
		dump_pincfg(d, "Pin Default", w->pin_cfg);
		if (w->pin_cfg != w->pin_cfg_orig)
			dp(d, "  (BIOS default 0x%08x, overridden)\n",
			   w->pin_cfg_orig);
		ctl = hda_codec_read(c, w->nid, AC_VERB_GET_PIN_WIDGET_CONTROL,
				     0);
		if (ctl != 0xffffffffu)
			dump_pinctl(d, w, ctl);
		if (w->pincap & AC_PINCAP_PRES_DETECT) {
			uint32_t s = hda_codec_read(c, w->nid,
						    AC_VERB_GET_PIN_SENSE, 0);

			if (s != 0xffffffffu)
				dp(d, "  Pin sense: 0x%08x%s%s\n", s,
				   (s & AC_PINSENSE_PRESENCE) ? " presence" :
								" empty",
				   (s & AC_PINSENSE_ELDV) ? " ELDV" : "");
		}
		break;
	}
	case AC_WID_AUD_SEL:
	case AC_WID_AUD_MIX:
	case AC_WID_BEEP:
	case AC_WID_VOL_KNB:
	case AC_WID_POWER:
	case AC_WID_VENDOR:
	default:
		break;
	}

	if (caps & AC_WCAP_UNSOL_CAP) {
		uint32_t u = hda_codec_read(c, w->nid,
					    AC_VERB_GET_UNSOLICITED_RESPONSE, 0);

		if (u != 0xffffffffu)
			dp(d, "  Unsolicited: tag=0x%02x, enabled=%u\n",
			   u & AC_UNSOL_TAG, (u & AC_UNSOL_ENABLED) ? 1u : 0u);
	}
	if (caps & AC_WCAP_POWER)
		dump_power(d, c, w->nid, w->power_caps);
	if (caps & AC_WCAP_PROC_WID) {
		uint32_t pc = hda_param(c, w->nid, AC_PAR_PROC_CAP);

		if (pc != 0xffffffffu)
			dp(d, "  Processing caps: benign=%u, ncoeff=%u\n",
			   pc & AC_PCAP_BENIGN,
			   (pc & AC_PCAP_NUM_COEF) >> 8);
	}
	dump_conn(d, c, w);
}

static void dump_path(struct dbuf *d, const struct hda_path *p)
{
	dp(d, "    path:");
	for (int j = 0; j < p->depth && j < HDA_MAX_PATH; j++) {
		if (j)
			dp(d, " -> 0x%02x[%u]", p->nid[j], p->idx[j]);
		else
			dp(d, " 0x%02x", p->nid[j]);
	}
	dp(d, "\n");
	if (p->vol_nid)
		dp(d, "    volume: 0x%02x %s[%u]\n", p->vol_nid,
		   p->vol_dir == HDA_OUTPUT ? "out" : "in", p->vol_idx);
	else
		dp(d, "    volume: none (software)\n");
	if (p->mute_nid)
		dp(d, "    mute: 0x%02x %s[%u]\n", p->mute_nid,
		   p->mute_dir == HDA_OUTPUT ? "out" : "in", p->mute_idx);
	else
		dp(d, "    mute: pin control only\n");
}

static void dump_parsed(struct dbuf *d, struct hda_codec *c)
{
	struct hda_codec_priv *p = hda_priv(c);
	struct hda_spec *s = &c->spec;
	static const char *const ctln[HDA_NCTL] = {
		"Master", "PCM", "Speaker", "Headphone", "Line", "HDMI",
		"Capture", "Mic Boost"
	};
	static const char *const modes[3] = { "auto", "id", "all" };

	dp(d, "\nParsed analog function:\n");
	dp(d, "  primary DAC 0x%02x, headphone DAC 0x%02x, ADC 0x%02x, "
	      "S/PDIF 0x%02x, beep 0x%02x\n",
	   s->primary_dac, s->hp_dac, s->adc, s->dig_out_nid, s->beep_nid);
	dp(d, "  output mode %s (id 0x%x), auto-mute %s, jacks %s\n",
	   s->outsel_mode < 3 ? modes[s->outsel_mode] : "?", s->outsel_id,
	   s->auto_mute ? "on" : "off",
	   s->jack_poll ? "polled" : "unsolicited");
	for (int i = 0; i < s->nout; i++) {
		const struct hda_out *o = &s->out[i];

		dp(d,
		   "  Output %d \"%s\" id 0x%x (%s): pin 0x%02x, DAC 0x%02x, "
		   "seq %u%s%s%s%s%s%s\n",
		   i, o->name, o->id, out_type_name(o->type), o->pin, o->dac,
		   o->seq, o->jack_detect ? ", jack" : ", no jack detect",
		   o->jack_detect ? (o->present ? " (plugged)" : " (empty)") :
				    "",
		   o->hp_capable ? ", hp-amp" : "", o->eapd ? ", eapd" : "",
		   o->dock ? ", dock" : "",
		   p->out_active[i] ? ", ACTIVE" : "");
		dump_path(d, &o->path);
	}
	for (int i = 0; i < s->nin; i++) {
		const struct hda_in *in = &s->in[i];

		dp(d,
		   "  Input %d \"%s\" id 0x%x: pin 0x%02x, ADC 0x%02x%s%s%s\n",
		   i, in->name, in->id, in->pin, in->adc,
		   in->jack_detect ? ", jack" : "",
		   in->jack_detect ? (in->present ? " (plugged)" : " (empty)") :
				     "",
		   i == p->in_active ? ", SELECTED" : "");
		dump_path(d, &in->path);
	}
	dp(d, "  Controls:");
	for (int k = 0; k < HDA_NCTL; k++)
		dp(d, " %s=%u/%u%s", ctln[k], p->ctl_l[k], p->ctl_r[k],
		   p->ctl_mute[k] ? "(muted)" : "");
	dp(d, "\n");
	if (p->play_tag)
		dp(d, "  Playback: stream %d, format 0x%04x, %d converter(s)\n",
		   p->play_tag, p->play_fmt, p->nbound_play);
	if (p->cap_tag)
		dp(d, "  Capture: stream %d, format 0x%04x, ADC 0x%02x\n",
		   p->cap_tag, p->cap_fmt, p->bound_cap);
	if (p->fixup)
		dp(d, "  Fixup: %s\n",
		   p->fixup->name ? p->fixup->name : "(unnamed)");
}

long hda_codec_dump(struct hda_codec *c, char *buf, size_t len)
{
	struct dbuf d;
	struct hda_codec_priv *p;

	if (!c || !buf || !len)
		return 0;
	p = hda_priv(c);
	d.b = buf;
	d.len = len;
	d.pos = 0;
	buf[0] = '\0';

	hda_codec_lock(c);
	dp(&d, "Codec: %s %s\n", c->vendor_name, c->chip_name);
	dp(&d, "Address: %d\n", c->addr);
	dp(&d, "AFG Function Id: 0x%x (unsol %u)\n", AC_GRP_AUDIO_FUNCTION,
	   c->unsol_capable ? 1u : 0u);
	if (c->mfg)
		dp(&d, "MFG Function Id: 0x%x at node 0x%02x\n",
		   AC_GRP_MODEM_FUNCTION, c->mfg);
	dp(&d, "Vendor Id: 0x%08x\n", c->vendor_id);
	dp(&d, "Subsystem Id: 0x%08x\n", c->subsystem_id);
	if (p->pci_ssid)
		dp(&d, "PCI Subsystem Id: 0x%08x\n", p->pci_ssid);
	dp(&d, "Revision Id: 0x%x\n", c->revision_id);
	dp(&d, "Driver: %s%s\n",
	   c->vendor && c->vendor->name ? c->vendor->name : "generic",
	   hda_is_display(c) ? " (display codec)" : "");
	if (!c->afg) {
		dp(&d, "No audio function group\n");
		hda_codec_unlock(c);
		return (long)d.pos;
	}
	dp(&d, "Nodes: 0x%02x..0x%02x (%u)\n", c->start_nid,
	   c->start_nid + c->num_nodes - 1, c->num_nodes);
	dp(&d, "AFG caps 0x%08x: output delay %u, input delay %u%s\n",
	   c->afg_caps, c->afg_caps & 0xf, (c->afg_caps >> 8) & 0xf,
	   (c->afg_caps & (1u << 16)) ? ", beep generator" : "");
	dp(&d, "Default PCM:\n");
	dump_rates(&d, c->afg_pcm);
	dump_formats(&d, c->afg_stream_fmts);
	dump_amp_caps(&d, "Default Amp-In", c->afg_amp_in_caps);
	dump_amp_caps(&d, "Default Amp-Out", c->afg_amp_out_caps);
	{
		uint32_t pw = hda_param(c, c->afg, AC_PAR_POWER_STATE);

		if (pw != 0xffffffffu)
			dump_power(&d, c, c->afg, pw);
	}
	dp(&d,
	   "GPIO: io=%u, o=%u, i=%u, unsolicited=%u, wake=%u\n",
	   c->gpio_caps & AC_GPIO_IO_COUNT,
	   (c->gpio_caps & AC_GPIO_O_COUNT) >> 8,
	   (c->gpio_caps & AC_GPIO_I_COUNT) >> 16,
	   (c->gpio_caps & AC_GPIO_UNSOLICITED) ? 1u : 0u,
	   (c->gpio_caps & AC_GPIO_WAKE) ? 1u : 0u);
	if (c->gpio_caps & AC_GPIO_IO_COUNT) {
		uint32_t n = c->gpio_caps & AC_GPIO_IO_COUNT;
		uint32_t data = hda_codec_read(c, c->afg,
					       AC_VERB_GET_GPIO_DATA, 0);
		uint32_t mask = hda_codec_read(c, c->afg,
					       AC_VERB_GET_GPIO_MASK, 0);
		uint32_t dir = hda_codec_read(c, c->afg,
					      AC_VERB_GET_GPIO_DIRECTION, 0);

		if (n > 8)
			n = 8;
		for (uint32_t i = 0; i < n; i++)
			dp(&d,
			   "  IO[%u]: enable=%u, dir=%u, data=%u\n", i,
			   (mask >> i) & 1, (dir >> i) & 1, (data >> i) & 1);
	}

	for (uint32_t i = 0; c->w && i < c->num_nodes; i++) {
		if (d.pos + 1 >= d.len)
			break;
		if (c->w[i].type == 0xe && !c->w[i].caps) {
			dp(&d, "Node 0x%02x: no response\n", c->w[i].nid);
			continue;
		}
		dump_node(&d, c, &c->w[i]);
	}
	if (!hda_is_display(c) && p->generic_parsed)
		dump_parsed(&d, c);
	hda_codec_unlock(c);
	/* the display driver adds its pins, converters and ELDs */
	if (hda_is_display(c) && d.pos + 2 < d.len) {
		long n = hda_hdmi_dump(c, d.b + d.pos, d.len - d.pos);

		if (n > 0) {
			size_t real = kstrlen(d.b + d.pos);

			d.pos += (size_t)n < real ? (size_t)n : real;
			if (d.pos >= d.len)
				d.pos = d.len - 1;
			d.b[d.pos] = '\0';
		}
	}
	return (long)d.pos;
}
