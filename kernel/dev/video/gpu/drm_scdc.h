// LikeOS -- the register map of the HDMI 2.0 Status and Control Data
// Channel (SCDC).
//
// An HDMI 2.0 sink answers at I2C address 0x54 on the same DDC bus that
// carries its EDID.  Through these registers the source tells the sink
// that it scrambles the TMDS stream and which TMDS bit clock ratio it uses
// (1/10 below 340 MHz, 1/40 above), and reads back whether the sink's
// descrambler locked, the per-channel lock and error counters, and who
// made the sink.  The accessors are in drm_scdc_helper.h.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from NVIDIA's code: MIT
// Portions Copyright (c) 2015 NVIDIA Corporation. All rights reserved.

#ifndef KERNEL_DEV_GPU_DRM_SCDC_H
#define KERNEL_DEV_GPU_DRM_SCDC_H

#define SCDC_SINK_VERSION 0x01

#define SCDC_SOURCE_VERSION 0x02

#define SCDC_UPDATE_0 0x10
#define  SCDC_READ_REQUEST_TEST (1 << 2)
#define  SCDC_CED_UPDATE (1 << 1)
#define  SCDC_STATUS_UPDATE (1 << 0)

#define SCDC_UPDATE_1 0x11

#define SCDC_TMDS_CONFIG 0x20
#define  SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 (1 << 1)
#define  SCDC_TMDS_BIT_CLOCK_RATIO_BY_10 (0 << 1)
#define  SCDC_SCRAMBLING_ENABLE (1 << 0)

#define SCDC_SCRAMBLER_STATUS 0x21
#define  SCDC_SCRAMBLING_STATUS (1 << 0)

#define SCDC_CONFIG_0 0x30
#define  SCDC_READ_REQUEST_ENABLE (1 << 0)

#define SCDC_STATUS_FLAGS_0 0x40
#define  SCDC_CH2_LOCK (1 << 3)
#define  SCDC_CH1_LOCK (1 << 2)
#define  SCDC_CH0_LOCK (1 << 1)
#define  SCDC_CH_LOCK_MASK (SCDC_CH2_LOCK | SCDC_CH1_LOCK | SCDC_CH0_LOCK)
#define  SCDC_CLOCK_DETECT (1 << 0)

#define SCDC_STATUS_FLAGS_1 0x41

#define SCDC_ERR_DET_0_L 0x50
#define SCDC_ERR_DET_0_H 0x51
#define SCDC_ERR_DET_1_L 0x52
#define SCDC_ERR_DET_1_H 0x53
#define SCDC_ERR_DET_2_L 0x54
#define SCDC_ERR_DET_2_H 0x55
#define  SCDC_CHANNEL_VALID (1 << 7)

#define SCDC_ERR_DET_CHECKSUM 0x56

#define SCDC_TEST_CONFIG_0 0xc0
#define  SCDC_TEST_READ_REQUEST (1 << 7)
#define  SCDC_TEST_READ_REQUEST_DELAY(x) ((x) & 0x7f)

#define SCDC_MANUFACTURER_IEEE_OUI 0xd0
#define SCDC_MANUFACTURER_IEEE_OUI_SIZE 3

#define SCDC_DEVICE_ID 0xd3
#define SCDC_DEVICE_ID_SIZE 8

#define SCDC_DEVICE_HARDWARE_REVISION 0xdb
#define  SCDC_GET_DEVICE_HARDWARE_REVISION_MAJOR(x) (((x) >> 4) & 0xf)
#define  SCDC_GET_DEVICE_HARDWARE_REVISION_MINOR(x) (((x) >> 0) & 0xf)

#define SCDC_DEVICE_SOFTWARE_MAJOR_REVISION 0xdc
#define SCDC_DEVICE_SOFTWARE_MINOR_REVISION 0xdd

#define SCDC_MANUFACTURER_SPECIFIC 0xde
#define SCDC_MANUFACTURER_SPECIFIC_SIZE 34

#endif
