// LikeOS pinctrl - Intel Gemini Lake SoC pinctrl/GPIO tables
//
// Pads, pad groups, communities and ACPI ids of this SoC for the
// Intel pin controller core (pinctrl-intel.c, pinctrl-intel.h).  The
// data is taken over unchanged from the published Intel tables;
// host/pinctrl/gen-intel-tables.py regenerates this file.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2017 Intel Corporation

#include <kernel/dev/gpio/pinctrl/intel/pinctrl-intel.h>

#define GLK_PAD_OWN	0x020
#define GLK_PADCFGLOCK	0x080
#define GLK_HOSTSW_OWN	0x0b0
#define GLK_GPI_IS	0x100
#define GLK_GPI_IE	0x110

#define GLK_COMMUNITY(b, s, e)				\
	INTEL_COMMUNITY_SIZE(b, s, e, 32, 4, GLK)

static const struct pinctrl_pin_desc glk_northwest_pins[] = {
	PINCTRL_PIN(0, "TCK"),
	PINCTRL_PIN(1, "TRST_B"),
	PINCTRL_PIN(2, "TMS"),
	PINCTRL_PIN(3, "TDI"),
	PINCTRL_PIN(4, "TDO"),
	PINCTRL_PIN(5, "JTAGX"),
	PINCTRL_PIN(6, "CX_PREQ_B"),
	PINCTRL_PIN(7, "CX_PRDY_B"),
	PINCTRL_PIN(8, "GPIO_8"),
	PINCTRL_PIN(9, "GPIO_9"),
	PINCTRL_PIN(10, "GPIO_10"),
	PINCTRL_PIN(11, "GPIO_11"),
	PINCTRL_PIN(12, "GPIO_12"),
	PINCTRL_PIN(13, "GPIO_13"),
	PINCTRL_PIN(14, "GPIO_14"),
	PINCTRL_PIN(15, "GPIO_15"),
	PINCTRL_PIN(16, "GPIO_16"),
	PINCTRL_PIN(17, "GPIO_17"),
	PINCTRL_PIN(18, "GPIO_18"),
	PINCTRL_PIN(19, "GPIO_19"),
	PINCTRL_PIN(20, "GPIO_20"),
	PINCTRL_PIN(21, "GPIO_21"),
	PINCTRL_PIN(22, "GPIO_22"),
	PINCTRL_PIN(23, "GPIO_23"),
	PINCTRL_PIN(24, "GPIO_24"),
	PINCTRL_PIN(25, "GPIO_25"),
	PINCTRL_PIN(26, "ISH_GPIO_0"),
	PINCTRL_PIN(27, "ISH_GPIO_1"),
	PINCTRL_PIN(28, "ISH_GPIO_2"),
	PINCTRL_PIN(29, "ISH_GPIO_3"),
	PINCTRL_PIN(30, "ISH_GPIO_4"),
	PINCTRL_PIN(31, "ISH_GPIO_5"),
	PINCTRL_PIN(32, "ISH_GPIO_6"),
	PINCTRL_PIN(33, "ISH_GPIO_7"),
	PINCTRL_PIN(34, "ISH_GPIO_8"),
	PINCTRL_PIN(35, "ISH_GPIO_9"),
	PINCTRL_PIN(36, "GPIO_36"),
	PINCTRL_PIN(37, "GPIO_37"),
	PINCTRL_PIN(38, "GPIO_38"),
	PINCTRL_PIN(39, "GPIO_39"),
	PINCTRL_PIN(40, "GPIO_40"),
	PINCTRL_PIN(41, "GPIO_41"),
	PINCTRL_PIN(42, "GP_INTD_DSI_TE1"),
	PINCTRL_PIN(43, "GP_INTD_DSI_TE2"),
	PINCTRL_PIN(44, "USB_OC0_B"),
	PINCTRL_PIN(45, "USB_OC1_B"),
	PINCTRL_PIN(46, "DSI_I2C_SDA"),
	PINCTRL_PIN(47, "DSI_I2C_SCL"),
	PINCTRL_PIN(48, "PMC_I2C_SDA"),
	PINCTRL_PIN(49, "PMC_I2C_SCL"),
	PINCTRL_PIN(50, "LPSS_I2C0_SDA"),
	PINCTRL_PIN(51, "LPSS_I2C0_SCL"),
	PINCTRL_PIN(52, "LPSS_I2C1_SDA"),
	PINCTRL_PIN(53, "LPSS_I2C1_SCL"),
	PINCTRL_PIN(54, "LPSS_I2C2_SDA"),
	PINCTRL_PIN(55, "LPSS_I2C2_SCL"),
	PINCTRL_PIN(56, "LPSS_I2C3_SDA"),
	PINCTRL_PIN(57, "LPSS_I2C3_SCL"),
	PINCTRL_PIN(58, "LPSS_I2C4_SDA"),
	PINCTRL_PIN(59, "LPSS_I2C4_SCL"),
	PINCTRL_PIN(60, "LPSS_UART0_RXD"),
	PINCTRL_PIN(61, "LPSS_UART0_TXD"),
	PINCTRL_PIN(62, "LPSS_UART0_RTS_B"),
	PINCTRL_PIN(63, "LPSS_UART0_CTS_B"),
	PINCTRL_PIN(64, "LPSS_UART2_RXD"),
	PINCTRL_PIN(65, "LPSS_UART2_TXD"),
	PINCTRL_PIN(66, "LPSS_UART2_RTS_B"),
	PINCTRL_PIN(67, "LPSS_UART2_CTS_B"),
	PINCTRL_PIN(68, "PMC_SPI_FS0"),
	PINCTRL_PIN(69, "PMC_SPI_FS1"),
	PINCTRL_PIN(70, "PMC_SPI_FS2"),
	PINCTRL_PIN(71, "PMC_SPI_RXD"),
	PINCTRL_PIN(72, "PMC_SPI_TXD"),
	PINCTRL_PIN(73, "PMC_SPI_CLK"),
	PINCTRL_PIN(74, "THERMTRIP_B"),
	PINCTRL_PIN(75, "PROCHOT_B"),
	PINCTRL_PIN(76, "EMMC_RST_B"),
	PINCTRL_PIN(77, "GPIO_212"),
	PINCTRL_PIN(78, "GPIO_213"),
	PINCTRL_PIN(79, "GPIO_214"),
};

static const struct intel_community glk_northwest_communities[] = {
	GLK_COMMUNITY(0, 0, 79),
};

static const struct intel_pinctrl_soc_data glk_northwest_soc_data = {
	.uid = "1",
	.pins = glk_northwest_pins,
	.npins = ARRAY_SIZE(glk_northwest_pins),
	.communities = glk_northwest_communities,
	.ncommunities = ARRAY_SIZE(glk_northwest_communities),
};

static const struct pinctrl_pin_desc glk_north_pins[] = {
	PINCTRL_PIN(0, "SVID0_ALERT_B"),
	PINCTRL_PIN(1, "SVID0_DATA"),
	PINCTRL_PIN(2, "SVID0_CLK"),
	PINCTRL_PIN(3, "LPSS_SPI_0_CLK"),
	PINCTRL_PIN(4, "LPSS_SPI_0_FS0"),
	PINCTRL_PIN(5, "LPSS_SPI_0_FS1"),
	PINCTRL_PIN(6, "LPSS_SPI_0_RXD"),
	PINCTRL_PIN(7, "LPSS_SPI_0_TXD"),
	PINCTRL_PIN(8, "LPSS_SPI_2_CLK"),
	PINCTRL_PIN(9, "LPSS_SPI_2_FS0"),
	PINCTRL_PIN(10, "LPSS_SPI_2_FS1"),
	PINCTRL_PIN(11, "LPSS_SPI_2_FS2"),
	PINCTRL_PIN(12, "LPSS_SPI_2_RXD"),
	PINCTRL_PIN(13, "LPSS_SPI_2_TXD"),
	PINCTRL_PIN(14, "FST_SPI_CS0_B"),
	PINCTRL_PIN(15, "FST_SPI_CS1_B"),
	PINCTRL_PIN(16, "FST_SPI_MOSI_IO0"),
	PINCTRL_PIN(17, "FST_SPI_MISO_IO1"),
	PINCTRL_PIN(18, "FST_SPI_IO2"),
	PINCTRL_PIN(19, "FST_SPI_IO3"),
	PINCTRL_PIN(20, "FST_SPI_CLK"),
	PINCTRL_PIN(21, "FST_SPI_CLK_FB"),
	PINCTRL_PIN(22, "PMU_PLTRST_B"),
	PINCTRL_PIN(23, "PMU_PWRBTN_B"),
	PINCTRL_PIN(24, "PMU_SLP_S0_B"),
	PINCTRL_PIN(25, "PMU_SLP_S3_B"),
	PINCTRL_PIN(26, "PMU_SLP_S4_B"),
	PINCTRL_PIN(27, "SUSPWRDNACK"),
	PINCTRL_PIN(28, "EMMC_DNX_PWR_EN_B"),
	PINCTRL_PIN(29, "GPIO_105"),
	PINCTRL_PIN(30, "PMU_BATLOW_B"),
	PINCTRL_PIN(31, "PMU_RESETBUTTON_B"),
	PINCTRL_PIN(32, "PMU_SUSCLK"),
	PINCTRL_PIN(33, "SUS_STAT_B"),
	PINCTRL_PIN(34, "LPSS_I2C5_SDA"),
	PINCTRL_PIN(35, "LPSS_I2C5_SCL"),
	PINCTRL_PIN(36, "LPSS_I2C6_SDA"),
	PINCTRL_PIN(37, "LPSS_I2C6_SCL"),
	PINCTRL_PIN(38, "LPSS_I2C7_SDA"),
	PINCTRL_PIN(39, "LPSS_I2C7_SCL"),
	PINCTRL_PIN(40, "PCIE_WAKE0_B"),
	PINCTRL_PIN(41, "PCIE_WAKE1_B"),
	PINCTRL_PIN(42, "PCIE_WAKE2_B"),
	PINCTRL_PIN(43, "PCIE_WAKE3_B"),
	PINCTRL_PIN(44, "PCIE_CLKREQ0_B"),
	PINCTRL_PIN(45, "PCIE_CLKREQ1_B"),
	PINCTRL_PIN(46, "PCIE_CLKREQ2_B"),
	PINCTRL_PIN(47, "PCIE_CLKREQ3_B"),
	PINCTRL_PIN(48, "HV_DDI0_DDC_SDA"),
	PINCTRL_PIN(49, "HV_DDI0_DDC_SCL"),
	PINCTRL_PIN(50, "HV_DDI1_DDC_SDA"),
	PINCTRL_PIN(51, "HV_DDI1_DDC_SCL"),
	PINCTRL_PIN(52, "PANEL0_VDDEN"),
	PINCTRL_PIN(53, "PANEL0_BKLTEN"),
	PINCTRL_PIN(54, "PANEL0_BKLTCTL"),
	PINCTRL_PIN(55, "HV_DDI0_HPD"),
	PINCTRL_PIN(56, "HV_DDI1_HPD"),
	PINCTRL_PIN(57, "HV_EDP_HPD"),
	PINCTRL_PIN(58, "GPIO_134"),
	PINCTRL_PIN(59, "GPIO_135"),
	PINCTRL_PIN(60, "GPIO_136"),
	PINCTRL_PIN(61, "GPIO_137"),
	PINCTRL_PIN(62, "GPIO_138"),
	PINCTRL_PIN(63, "GPIO_139"),
	PINCTRL_PIN(64, "GPIO_140"),
	PINCTRL_PIN(65, "GPIO_141"),
	PINCTRL_PIN(66, "GPIO_142"),
	PINCTRL_PIN(67, "GPIO_143"),
	PINCTRL_PIN(68, "GPIO_144"),
	PINCTRL_PIN(69, "GPIO_145"),
	PINCTRL_PIN(70, "GPIO_146"),
	PINCTRL_PIN(71, "LPC_ILB_SERIRQ"),
	PINCTRL_PIN(72, "LPC_CLKOUT0"),
	PINCTRL_PIN(73, "LPC_CLKOUT1"),
	PINCTRL_PIN(74, "LPC_AD0"),
	PINCTRL_PIN(75, "LPC_AD1"),
	PINCTRL_PIN(76, "LPC_AD2"),
	PINCTRL_PIN(77, "LPC_AD3"),
	PINCTRL_PIN(78, "LPC_CLKRUNB"),
	PINCTRL_PIN(79, "LPC_FRAMEB"),
};

static const struct intel_community glk_north_communities[] = {
	GLK_COMMUNITY(0, 0, 79),
};

static const struct intel_pinctrl_soc_data glk_north_soc_data = {
	.uid = "2",
	.pins = glk_north_pins,
	.npins = ARRAY_SIZE(glk_north_pins),
	.communities = glk_north_communities,
	.ncommunities = ARRAY_SIZE(glk_north_communities),
};

static const struct pinctrl_pin_desc glk_audio_pins[] = {
	PINCTRL_PIN(0, "AVS_I2S0_MCLK"),
	PINCTRL_PIN(1, "AVS_I2S0_BCLK"),
	PINCTRL_PIN(2, "AVS_I2S0_WS_SYNC"),
	PINCTRL_PIN(3, "AVS_I2S0_SDI"),
	PINCTRL_PIN(4, "AVS_I2S0_SDO"),
	PINCTRL_PIN(5, "AVS_I2S1_MCLK"),
	PINCTRL_PIN(6, "AVS_I2S1_BCLK"),
	PINCTRL_PIN(7, "AVS_I2S1_WS_SYNC"),
	PINCTRL_PIN(8, "AVS_I2S1_SDI"),
	PINCTRL_PIN(9, "AVS_I2S1_SDO"),
	PINCTRL_PIN(10, "AVS_HDA_BCLK"),
	PINCTRL_PIN(11, "AVS_HDA_WS_SYNC"),
	PINCTRL_PIN(12, "AVS_HDA_SDI"),
	PINCTRL_PIN(13, "AVS_HDA_SDO"),
	PINCTRL_PIN(14, "AVS_HDA_RSTB"),
	PINCTRL_PIN(15, "AVS_M_CLK_A1"),
	PINCTRL_PIN(16, "AVS_M_CLK_B1"),
	PINCTRL_PIN(17, "AVS_M_DATA_1"),
	PINCTRL_PIN(18, "AVS_M_CLK_AB2"),
	PINCTRL_PIN(19, "AVS_M_DATA_2"),
};

static const struct intel_community glk_audio_communities[] = {
	GLK_COMMUNITY(0, 0, 19),
};

static const struct intel_pinctrl_soc_data glk_audio_soc_data = {
	.uid = "3",
	.pins = glk_audio_pins,
	.npins = ARRAY_SIZE(glk_audio_pins),
	.communities = glk_audio_communities,
	.ncommunities = ARRAY_SIZE(glk_audio_communities),
};

static const struct pinctrl_pin_desc glk_scc_pins[] = {
	PINCTRL_PIN(0, "SMB_ALERTB"),
	PINCTRL_PIN(1, "SMB_CLK"),
	PINCTRL_PIN(2, "SMB_DATA"),
	PINCTRL_PIN(3, "SDCARD_LVL_WP"),
	PINCTRL_PIN(4, "SDCARD_CLK"),
	PINCTRL_PIN(5, "SDCARD_CLK_FB"),
	PINCTRL_PIN(6, "SDCARD_D0"),
	PINCTRL_PIN(7, "SDCARD_D1"),
	PINCTRL_PIN(8, "SDCARD_D2"),
	PINCTRL_PIN(9, "SDCARD_D3"),
	PINCTRL_PIN(10, "SDCARD_CMD"),
	PINCTRL_PIN(11, "SDCARD_CD_B"),
	PINCTRL_PIN(12, "SDCARD_PWR_DOWN_B"),
	PINCTRL_PIN(13, "GPIO_210"),
	PINCTRL_PIN(14, "OSC_CLK_OUT_0"),
	PINCTRL_PIN(15, "OSC_CLK_OUT_1"),
	PINCTRL_PIN(16, "CNV_BRI_DT"),
	PINCTRL_PIN(17, "CNV_BRI_RSP"),
	PINCTRL_PIN(18, "CNV_RGI_DT"),
	PINCTRL_PIN(19, "CNV_RGI_RSP"),
	PINCTRL_PIN(20, "CNV_RF_RESET_B"),
	PINCTRL_PIN(21, "XTAL_CLKREQ"),
	PINCTRL_PIN(22, "SDIO_CLK_FB"),
	PINCTRL_PIN(23, "EMMC0_CLK"),
	PINCTRL_PIN(24, "EMMC0_CLK_FB"),
	PINCTRL_PIN(25, "EMMC0_D0"),
	PINCTRL_PIN(26, "EMMC0_D1"),
	PINCTRL_PIN(27, "EMMC0_D2"),
	PINCTRL_PIN(28, "EMMC0_D3"),
	PINCTRL_PIN(29, "EMMC0_D4"),
	PINCTRL_PIN(30, "EMMC0_D5"),
	PINCTRL_PIN(31, "EMMC0_D6"),
	PINCTRL_PIN(32, "EMMC0_D7"),
	PINCTRL_PIN(33, "EMMC0_CMD"),
	PINCTRL_PIN(34, "EMMC0_STROBE"),
};

static const struct intel_community glk_scc_communities[] = {
	GLK_COMMUNITY(0, 0, 34),
};

static const struct intel_pinctrl_soc_data glk_scc_soc_data = {
	.uid = "4",
	.pins = glk_scc_pins,
	.npins = ARRAY_SIZE(glk_scc_pins),
	.communities = glk_scc_communities,
	.ncommunities = ARRAY_SIZE(glk_scc_communities),
};

static const struct intel_pinctrl_soc_data *const glk_pinctrl_soc_data[] = {
	&glk_northwest_soc_data,
	&glk_north_soc_data,
	&glk_audio_soc_data,
	&glk_scc_soc_data,
	NULL
};

const struct intel_pinctrl_match intel_geminilake_match[] = {
	{ .hid = "INT3453", .uid_table = glk_pinctrl_soc_data },
	{ }
};
