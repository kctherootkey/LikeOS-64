// LikeOS Intel High Definition Audio - codec verbs, parameters and fields
// as defined by the HD Audio specification (revision 1.0a).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_HDA_VERBS_H
#define KERNEL_DEV_SOUND_HDA_VERBS_H

/* ---- command word ------------------------------------------------------- */

/* bits 31:28 codec address, 27:20 node id, 19:0 verb and payload.  The
 * verbs below are written so that (verb << 8) | payload is right for both
 * kinds: a 12-bit verb (0x7xx) takes an 8-bit payload, a 4-bit one (0x2xx,
 * 0x3xx, 0x4xx, 0x5xx, 0xaxx-0xdxx) a 16-bit payload. */
#define HDA_CMD(addr, nid, verb, payload)                                   \
	((((uint32_t)(addr) & 0xf) << 28) | (((uint32_t)(nid) & 0xff) << 20) | \
	 (((uint32_t)(verb) & 0xfff) << 8) | ((uint32_t)(payload) & 0xffff))

/* ---- GET verbs (12 bit) -------------------------------------------------- */
#define AC_VERB_GET_STREAM_FORMAT 0x0a00
#define AC_VERB_GET_AMP_GAIN_MUTE 0x0b00
#define AC_VERB_GET_PROC_COEF 0x0c00
#define AC_VERB_GET_COEF_INDEX 0x0d00
#define AC_VERB_PARAMETERS 0x0f00
#define AC_VERB_GET_CONNECT_SEL 0x0f01
#define AC_VERB_GET_CONNECT_LIST 0x0f02
#define AC_VERB_GET_PROC_STATE 0x0f03
#define AC_VERB_GET_SDI_SELECT 0x0f04
#define AC_VERB_GET_POWER_STATE 0x0f05
#define AC_VERB_GET_CONV 0x0f06
#define AC_VERB_GET_PIN_WIDGET_CONTROL 0x0f07
#define AC_VERB_GET_UNSOLICITED_RESPONSE 0x0f08
#define AC_VERB_GET_PIN_SENSE 0x0f09
#define AC_VERB_GET_BEEP_CONTROL 0x0f0a
#define AC_VERB_GET_EAPD_BTLENABLE 0x0f0c
#define AC_VERB_GET_DIGI_CONVERT_1 0x0f0d
#define AC_VERB_GET_DIGI_CONVERT_2 0x0f0e
#define AC_VERB_GET_VOLUME_KNOB_CONTROL 0x0f0f
#define AC_VERB_GET_GPIO_DATA 0x0f15
#define AC_VERB_GET_GPIO_MASK 0x0f16
#define AC_VERB_GET_GPIO_DIRECTION 0x0f17
#define AC_VERB_GET_GPIO_WAKE_MASK 0x0f18
#define AC_VERB_GET_GPIO_UNSOLICITED_RSP_MASK 0x0f19
#define AC_VERB_GET_GPIO_STICKY_MASK 0x0f1a
#define AC_VERB_GET_CONFIG_DEFAULT 0x0f1c
#define AC_VERB_GET_SUBSYSTEM_ID 0x0f20
#define AC_VERB_GET_STRIPE_CONTROL 0x0f24
#define AC_VERB_GET_CVT_CHAN_COUNT 0x0f2d
#define AC_VERB_GET_HDMI_DIP_SIZE 0x0f2e
#define AC_VERB_GET_HDMI_ELDD 0x0f2f
#define AC_VERB_GET_HDMI_DIP_INDEX 0x0f30
#define AC_VERB_GET_HDMI_DIP_DATA 0x0f31
#define AC_VERB_GET_HDMI_DIP_XMIT 0x0f32
#define AC_VERB_GET_HDMI_CP_CTRL 0x0f33
#define AC_VERB_GET_HDMI_CHAN_SLOT 0x0f34
#define AC_VERB_GET_DEVICE_SEL 0x0f35
#define AC_VERB_GET_DEVICE_LIST 0x0f36

/* ---- SET verbs (12 bit, except stream format/amp/coef: 4 bit) -------------- */
#define AC_VERB_SET_STREAM_FORMAT 0x200
#define AC_VERB_SET_AMP_GAIN_MUTE 0x300
#define AC_VERB_SET_PROC_COEF 0x400
#define AC_VERB_SET_COEF_INDEX 0x500
#define AC_VERB_SET_CONNECT_SEL 0x701
#define AC_VERB_SET_PROC_STATE 0x703
#define AC_VERB_SET_SDI_SELECT 0x704
#define AC_VERB_SET_POWER_STATE 0x705
#define AC_VERB_SET_CHANNEL_STREAMID 0x706
#define AC_VERB_SET_PIN_WIDGET_CONTROL 0x707
#define AC_VERB_SET_UNSOLICITED_ENABLE 0x708
#define AC_VERB_SET_PIN_SENSE 0x709
#define AC_VERB_SET_BEEP_CONTROL 0x70a
#define AC_VERB_SET_EAPD_BTLENABLE 0x70c
#define AC_VERB_SET_DIGI_CONVERT_1 0x70d
#define AC_VERB_SET_DIGI_CONVERT_2 0x70e
#define AC_VERB_SET_DIGI_CONVERT_3 0x73e
#define AC_VERB_SET_VOLUME_KNOB_CONTROL 0x70f
#define AC_VERB_SET_GPIO_DATA 0x715
#define AC_VERB_SET_GPIO_MASK 0x716
#define AC_VERB_SET_GPIO_DIRECTION 0x717
#define AC_VERB_SET_GPIO_WAKE_MASK 0x718
#define AC_VERB_SET_GPIO_UNSOLICITED_RSP_MASK 0x719
#define AC_VERB_SET_GPIO_STICKY_MASK 0x71a
#define AC_VERB_SET_CONFIG_DEFAULT_BYTES_0 0x71c
#define AC_VERB_SET_CONFIG_DEFAULT_BYTES_1 0x71d
#define AC_VERB_SET_CONFIG_DEFAULT_BYTES_2 0x71e
#define AC_VERB_SET_CONFIG_DEFAULT_BYTES_3 0x71f
#define AC_VERB_SET_EAPD 0x788
#define AC_VERB_SET_CODEC_RESET 0x7ff
#define AC_VERB_SET_STRIPE_CONTROL 0x724
#define AC_VERB_SET_CVT_CHAN_COUNT 0x72d
#define AC_VERB_SET_HDMI_DIP_INDEX 0x730
#define AC_VERB_SET_HDMI_DIP_DATA 0x731
#define AC_VERB_SET_HDMI_DIP_XMIT 0x732
#define AC_VERB_SET_HDMI_CP_CTRL 0x733
#define AC_VERB_SET_HDMI_CHAN_SLOT 0x734
#define AC_VERB_SET_DEVICE_SEL 0x735

/* ---- parameters (AC_VERB_PARAMETERS payload) -------------------------- */
#define AC_PAR_VENDOR_ID 0x00
#define AC_PAR_SUBSYSTEM_ID 0x01
#define AC_PAR_REV_ID 0x02
#define AC_PAR_NODE_COUNT 0x04
#define AC_PAR_FUNCTION_TYPE 0x05
#define AC_PAR_AUDIO_FG_CAP 0x08
#define AC_PAR_AUDIO_WIDGET_CAP 0x09
#define AC_PAR_PCM 0x0a
#define AC_PAR_STREAM 0x0b
#define AC_PAR_PIN_CAP 0x0c
#define AC_PAR_AMP_IN_CAP 0x0d
#define AC_PAR_CONNLIST_LEN 0x0e
#define AC_PAR_POWER_STATE 0x0f
#define AC_PAR_PROC_CAP 0x10
#define AC_PAR_GPIO_CAP 0x11
#define AC_PAR_AMP_OUT_CAP 0x12
#define AC_PAR_VOL_KNB_CAP 0x13
#define AC_PAR_DEVLIST_LEN 0x15
#define AC_PAR_HDMI_LPCM_CAP 0x20

/* function group types */
#define AC_GRP_AUDIO_FUNCTION 0x01
#define AC_GRP_MODEM_FUNCTION 0x02
#define AC_FGT_UNSOL_CAP (1u << 8)

/* audio widget capabilities */
#define AC_WCAP_STEREO (1u << 0)
#define AC_WCAP_IN_AMP (1u << 1)
#define AC_WCAP_OUT_AMP (1u << 2)
#define AC_WCAP_AMP_OVRD (1u << 3)
#define AC_WCAP_FORMAT_OVRD (1u << 4)
#define AC_WCAP_STRIPE (1u << 5)
#define AC_WCAP_PROC_WID (1u << 6)
#define AC_WCAP_UNSOL_CAP (1u << 7)
#define AC_WCAP_CONN_LIST (1u << 8)
#define AC_WCAP_DIGITAL (1u << 9)
#define AC_WCAP_POWER (1u << 10)
#define AC_WCAP_LR_SWAP (1u << 11)
#define AC_WCAP_CP_CAPS (1u << 12)
#define AC_WCAP_CHAN_CNT_EXT (7u << 13)
#define AC_WCAP_DELAY (0xfu << 16)
#define AC_WCAP_DELAY_SHIFT 16
#define AC_WCAP_TYPE (0xfu << 20)
#define AC_WCAP_TYPE_SHIFT 20

/* widget types */
#define AC_WID_AUD_OUT 0x0 /* DAC */
#define AC_WID_AUD_IN 0x1 /* ADC */
#define AC_WID_AUD_MIX 0x2
#define AC_WID_AUD_SEL 0x3
#define AC_WID_PIN 0x4
#define AC_WID_POWER 0x5
#define AC_WID_VOL_KNB 0x6
#define AC_WID_BEEP 0x7
#define AC_WID_VENDOR 0xf

/* AC_PAR_PCM: supported rates (bits 0-11) and bit depths (16-20) */
#define AC_SUPPCM_RATES 0xfff
#define AC_SUPPCM_BITS_8 (1u << 16)
#define AC_SUPPCM_BITS_16 (1u << 17)
#define AC_SUPPCM_BITS_20 (1u << 18)
#define AC_SUPPCM_BITS_24 (1u << 19)
#define AC_SUPPCM_BITS_32 (1u << 20)
/* rate bits: 8k 11.025k 16k 22.05k 32k 44.1k 48k 88.2k 96k 176.4k 192k 384k */

/* AC_PAR_STREAM */
#define AC_SUPFMT_PCM (1u << 0)
#define AC_SUPFMT_FLOAT32 (1u << 1)
#define AC_SUPFMT_AC3 (1u << 2)

/* GET_STREAM_FORMAT / stream format word */
#define AC_FMT_CHAN_SHIFT 0
#define AC_FMT_CHAN_MASK (0x0fu << 0)
#define AC_FMT_BITS_SHIFT 4
#define AC_FMT_BITS_MASK (7u << 4)
#define AC_FMT_BITS_8 (0u << 4)
#define AC_FMT_BITS_16 (1u << 4)
#define AC_FMT_BITS_20 (2u << 4)
#define AC_FMT_BITS_24 (3u << 4)
#define AC_FMT_BITS_32 (4u << 4)
#define AC_FMT_DIV_SHIFT 8
#define AC_FMT_DIV_MASK (7u << 8)
#define AC_FMT_MULT_SHIFT 11
#define AC_FMT_MULT_MASK (7u << 11)
#define AC_FMT_BASE_SHIFT 14
#define AC_FMT_BASE_48K (0u << 14)
#define AC_FMT_BASE_44K (1u << 14)
#define AC_FMT_TYPE_SHIFT 15
#define AC_FMT_TYPE_PCM (0u << 15)
#define AC_FMT_TYPE_NON_PCM (1u << 15)

/* pin capabilities */
#define AC_PINCAP_IMP_SENSE (1u << 0)
#define AC_PINCAP_TRIG_REQ (1u << 1)
#define AC_PINCAP_PRES_DETECT (1u << 2)
#define AC_PINCAP_HP_DRV (1u << 3)
#define AC_PINCAP_OUT (1u << 4)
#define AC_PINCAP_IN (1u << 5)
#define AC_PINCAP_BALANCE (1u << 6)
#define AC_PINCAP_HDMI (1u << 7)
#define AC_PINCAP_LR_SWAP (1u << 7)
#define AC_PINCAP_VREF (0x37u << 8)
#define AC_PINCAP_VREF_SHIFT 8
#define AC_PINCAP_EAPD (1u << 16)
#define AC_PINCAP_DP (1u << 24)
#define AC_PINCAP_HBR (1u << 27)

/* amplifier capabilities */
#define AC_AMPCAP_OFFSET (0x7fu << 0)
#define AC_AMPCAP_OFFSET_SHIFT 0
#define AC_AMPCAP_NUM_STEPS (0x7fu << 8)
#define AC_AMPCAP_NUM_STEPS_SHIFT 8
#define AC_AMPCAP_STEP_SIZE (0x7fu << 16)
#define AC_AMPCAP_STEP_SIZE_SHIFT 16
#define AC_AMPCAP_MUTE (1u << 31)
#define AC_AMPCAP_MIN_MUTE (1u << 31)

/* connection list length */
#define AC_CLIST_LENGTH 0x7f
#define AC_CLIST_LONG (1u << 7)

/* supported power states */
#define AC_PWRST_D0SUP (1u << 0)
#define AC_PWRST_D1SUP (1u << 1)
#define AC_PWRST_D2SUP (1u << 2)
#define AC_PWRST_D3SUP (1u << 3)
#define AC_PWRST_D3COLDSUP (1u << 4)
#define AC_PWRST_S3D3COLDSUP (1u << 29)
#define AC_PWRST_CLKSTOP (1u << 30)
#define AC_PWRST_EPSS (1u << 31)

/* power state values */
#define AC_PWRST_SETTING (0xfu << 0)
#define AC_PWRST_ACTUAL (0xfu << 4)
#define AC_PWRST_ACTUAL_SHIFT 4
#define AC_PWRST_D0 0x00
#define AC_PWRST_D1 0x01
#define AC_PWRST_D2 0x02
#define AC_PWRST_D3 0x03
#define AC_PWRST_ERROR (1u << 8)
#define AC_PWRST_CLK_STOP_OK (1u << 9)
#define AC_PWRST_SETTING_RESET (1u << 10)

/* processing capabilities */
#define AC_PCAP_BENIGN (1u << 0)
#define AC_PCAP_NUM_COEF (0xffu << 8)

/* GPIO capabilities */
#define AC_GPIO_IO_COUNT (0xffu << 0)
#define AC_GPIO_O_COUNT (0xffu << 8)
#define AC_GPIO_I_COUNT (0xffu << 16)
#define AC_GPIO_UNSOLICITED (1u << 30)
#define AC_GPIO_WAKE (1u << 31)

/* converter stream / channel */
#define AC_CONV_CHANNEL (0xfu << 0)
#define AC_CONV_STREAM (0xfu << 4)
#define AC_CONV_STREAM_SHIFT 4

/* digital converter control (DIGI_CONVERT_1/2) */
#define AC_DIG1_ENABLE (1u << 0)
#define AC_DIG1_V (1u << 1)
#define AC_DIG1_VCFG (1u << 2)
#define AC_DIG1_EMPHASIS (1u << 3)
#define AC_DIG1_COPYRIGHT (1u << 4)
#define AC_DIG1_NONAUDIO (1u << 5)
#define AC_DIG1_PROFESSIONAL (1u << 6)
#define AC_DIG1_LEVEL (1u << 7)
#define AC_DIG2_CC (0x7fu << 0)

/* pin widget control */
#define AC_PINCTL_EPT (0x3u << 0)
#define AC_PINCTL_EPT_NATIVE 0
#define AC_PINCTL_EPT_HBR 3
#define AC_PINCTL_VREFEN (0x7u << 0)
#define AC_PINCTL_VREF_HIZ 0
#define AC_PINCTL_VREF_50 1
#define AC_PINCTL_VREF_GRD 2
#define AC_PINCTL_VREF_80 4
#define AC_PINCTL_VREF_100 5
#define AC_PINCTL_IN_EN (1u << 5)
#define AC_PINCTL_OUT_EN (1u << 6)
#define AC_PINCTL_HP_EN (1u << 7)

/* pin sense */
#define AC_PINSENSE_IMPEDANCE_MASK 0x7fffffffu
#define AC_PINSENSE_PRESENCE (1u << 31)
#define AC_PINSENSE_ELDV (1u << 30)

/* EAPD/BTL enable */
#define AC_EAPDBTL_BALANCED (1u << 0)
#define AC_EAPDBTL_EAPD (1u << 1)
#define AC_EAPDBTL_LR_SWAP (1u << 2)

/* unsolicited response control (payload of SET_UNSOLICITED_ENABLE) */
#define AC_UNSOL_TAG (0x3fu << 0)
#define AC_UNSOL_ENABLED (1u << 7)
#define AC_USRSP_EN AC_UNSOL_ENABLED

/* unsolicited response word (RIRB) */
#define AC_UNSOL_RES_TAG (0x3fu << 26)
#define AC_UNSOL_RES_TAG_SHIFT 26
#define AC_UNSOL_RES_SUBTAG (0x1fu << 21)
#define AC_UNSOL_RES_SUBTAG_SHIFT 21
#define AC_UNSOL_RES_DE (0x3fu << 15)
#define AC_UNSOL_RES_DE_SHIFT 15
#define AC_UNSOL_RES_IA (1u << 2)
#define AC_UNSOL_RES_ELDV (1u << 1)
#define AC_UNSOL_RES_PD (1u << 0)

/* amp gain/mute SET payload (16 bit) */
#define AC_AMP_SET_LEFT (1u << 13)
#define AC_AMP_SET_RIGHT (1u << 12)
#define AC_AMP_SET_INPUT (1u << 14)
#define AC_AMP_SET_OUTPUT (1u << 15)
#define AC_AMP_SET_INDEX (0xfu << 8)
#define AC_AMP_SET_INDEX_SHIFT 8
#define AC_AMP_MUTE (1u << 7)
#define AC_AMP_GAIN (0x7fu)
/* amp gain/mute GET payload */
#define AC_AMP_GET_LEFT (1u << 13)
#define AC_AMP_GET_RIGHT (0u << 13)
#define AC_AMP_GET_OUTPUT (1u << 15)
#define AC_AMP_GET_INPUT (0u << 15)

/* pin default configuration */
#define AC_DEFCFG_SEQUENCE (0xfu << 0)
#define AC_DEFCFG_DEF_ASSOC (0xfu << 4)
#define AC_DEFCFG_ASSOC_SHIFT 4
#define AC_DEFCFG_MISC (0xfu << 8)
#define AC_DEFCFG_MISC_SHIFT 8
#define AC_DEFCFG_MISC_NO_PRESENCE (1u << 0)
#define AC_DEFCFG_COLOR (0xfu << 12)
#define AC_DEFCFG_COLOR_SHIFT 12
#define AC_DEFCFG_CONN_TYPE (0xfu << 16)
#define AC_DEFCFG_CONN_TYPE_SHIFT 16
#define AC_DEFCFG_DEVICE (0xfu << 20)
#define AC_DEFCFG_DEVICE_SHIFT 20
#define AC_DEFCFG_LOCATION (0x3fu << 24)
#define AC_DEFCFG_LOCATION_SHIFT 24
#define AC_DEFCFG_PORT_CONN (0x3u << 30)
#define AC_DEFCFG_PORT_CONN_SHIFT 30

/* device types (AC_DEFCFG_DEVICE) */
#define AC_JACK_LINE_OUT 0x0
#define AC_JACK_SPEAKER 0x1
#define AC_JACK_HP_OUT 0x2
#define AC_JACK_CD 0x3
#define AC_JACK_SPDIF_OUT 0x4
#define AC_JACK_DIG_OTHER_OUT 0x5
#define AC_JACK_MODEM_LINE_SIDE 0x6
#define AC_JACK_MODEM_HAND_SIDE 0x7
#define AC_JACK_LINE_IN 0x8
#define AC_JACK_AUX 0x9
#define AC_JACK_MIC_IN 0xa
#define AC_JACK_TELEPHONY 0xb
#define AC_JACK_SPDIF_IN 0xc
#define AC_JACK_DIG_OTHER_IN 0xd
#define AC_JACK_OTHER 0xf

/* port connectivity (AC_DEFCFG_PORT_CONN) */
#define AC_JACK_PORT_COMPLEX 0x0 /* a jack */
#define AC_JACK_PORT_NONE 0x1 /* not connected */
#define AC_JACK_PORT_FIXED 0x2 /* built in */
#define AC_JACK_PORT_BOTH 0x3 /* both a jack and built in */

/* location (low 4 bits = geometric, high 2 = chassis) */
#define AC_JACK_LOC_EXTERNAL 0x00
#define AC_JACK_LOC_INTERNAL 0x10
#define AC_JACK_LOC_SEPARATE 0x20
#define AC_JACK_LOC_OTHER 0x30
#define AC_JACK_LOC_REAR 0x01
#define AC_JACK_LOC_FRONT 0x02
#define AC_JACK_LOC_LEFT 0x03
#define AC_JACK_LOC_RIGHT 0x04
#define AC_JACK_LOC_TOP 0x05
#define AC_JACK_LOC_BOTTOM 0x06
#define AC_JACK_LOC_HDMI 0x18 /* internal, special */
#define AC_JACK_LOC_SPDIF 0x18

#endif /* KERNEL_DEV_SOUND_HDA_VERBS_H */
