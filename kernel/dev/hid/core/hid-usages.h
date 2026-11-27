// LikeOS HID core - usage pages and usages
//
// A HID usage is a 32-bit number: the usage page in the upper 16 bits, the
// usage id within the page in the lower 16.  The report descriptor names
// every control by its usage ("Generic Desktop / X", "Digitizer / Contact
// Identifier"), and the drivers decide what a control means by comparing
// full 32-bit usages against the constants below.
//
// The numbers are those of the USB-IF "HID Usage Tables" document, plus the
// vendor pages that keyboards and touchpads found in notebooks use.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_DEV_HID_CORE_HID_USAGES_H_
#define _KERNEL_DEV_HID_CORE_HID_USAGES_H_

#define HID_USAGE_PAGE 0xffff0000u
#define HID_USAGE 0x0000ffffu

// ---------------------------------------------------------------------------
// Usage pages (already shifted into the upper half)
// ---------------------------------------------------------------------------
#define HID_UP_UNDEFINED 0x00000000u
#define HID_UP_GENDESK 0x00010000u
#define HID_UP_SIMULATION 0x00020000u
#define HID_UP_GENDEVCTRLS 0x00060000u
#define HID_UP_KEYBOARD 0x00070000u
#define HID_UP_LED 0x00080000u
#define HID_UP_BUTTON 0x00090000u
#define HID_UP_ORDINAL 0x000a0000u
#define HID_UP_TELEPHONY 0x000b0000u
#define HID_UP_CONSUMER 0x000c0000u
#define HID_UP_DIGITIZER 0x000d0000u
#define HID_UP_HAPTIC 0x000e0000u
#define HID_UP_PID 0x000f0000u
#define HID_UP_SENSOR 0x00200000u
#define HID_UP_BATTERY 0x00850000u
#define HID_UP_CAMERA 0x00900000u
#define HID_UP_CUSTOM 0x00ff0000u
#define HID_UP_HPVENDOR 0xff7f0000u
#define HID_UP_HPVENDOR2 0xff010000u
#define HID_UP_MSVENDOR 0xff000000u
#define HID_UP_LOGIVENDOR 0xffbc0000u
#define HID_UP_LOGIVENDOR2 0xff090000u
#define HID_UP_LOGIVENDOR3 0xff430000u
#define HID_UP_LNVENDOR 0xffa00000u
#define HID_UP_GOOGLEVENDOR 0xffd10000u

// ---------------------------------------------------------------------------
// Generic Desktop page (0x01)
// ---------------------------------------------------------------------------
#define HID_GD_POINTER 0x00010001u
#define HID_GD_MOUSE 0x00010002u
#define HID_GD_JOYSTICK 0x00010004u
#define HID_GD_GAMEPAD 0x00010005u
#define HID_GD_KEYBOARD 0x00010006u
#define HID_GD_KEYPAD 0x00010007u
#define HID_GD_MULTIAXIS 0x00010008u
#define HID_GD_WIRELESS_RADIO_CTLS 0x0001000cu
#define HID_GD_SYSTEM_MULTIAXIS 0x0001000eu
#define HID_GD_X 0x00010030u
#define HID_GD_Y 0x00010031u
#define HID_GD_Z 0x00010032u
#define HID_GD_RX 0x00010033u
#define HID_GD_RY 0x00010034u
#define HID_GD_RZ 0x00010035u
#define HID_GD_SLIDER 0x00010036u
#define HID_GD_DIAL 0x00010037u
#define HID_GD_WHEEL 0x00010038u
#define HID_GD_HATSWITCH 0x00010039u
#define HID_GD_BUFFER 0x0001003au
#define HID_GD_BYTECOUNT 0x0001003bu
#define HID_GD_MOTION 0x0001003cu
#define HID_GD_START 0x0001003du
#define HID_GD_SELECT 0x0001003eu
#define HID_GD_VX 0x00010040u
#define HID_GD_VY 0x00010041u
#define HID_GD_VZ 0x00010042u
#define HID_GD_VBRX 0x00010043u
#define HID_GD_VBRY 0x00010044u
#define HID_GD_VBRZ 0x00010045u
#define HID_GD_VNO 0x00010046u
#define HID_GD_FEATURE 0x00010047u
#define HID_GD_RESOLUTION_MULTIPLIER 0x00010048u
#define HID_GD_SYSTEM_CONTROL 0x00010080u
#define HID_GD_UP 0x00010090u
#define HID_GD_DOWN 0x00010091u
#define HID_GD_RIGHT 0x00010092u
#define HID_GD_LEFT 0x00010093u
#define HID_GD_DO_NOT_DISTURB 0x0001009bu
#define HID_GD_RFKILL_BTN 0x000100c6u

// ---------------------------------------------------------------------------
// Generic Device Controls page (0x06) and Battery System page (0x85)
// ---------------------------------------------------------------------------
#define HID_DC_BATTERYSTRENGTH 0x00060020u
#define HID_BAT_ABSOLUTESTATEOFCHARGE 0x00850065u
#define HID_BAT_CHARGING 0x00850044u

// ---------------------------------------------------------------------------
// Consumer page (0x0c)
// ---------------------------------------------------------------------------
#define HID_CP_CONSUMER_CONTROL 0x000c0001u
#define HID_CP_NUMERICKEYPAD 0x000c0002u
#define HID_CP_PROGRAMMABLEBUTTONS 0x000c0003u
#define HID_CP_MICROPHONE 0x000c0004u
#define HID_CP_HEADPHONE 0x000c0005u
#define HID_CP_GRAPHICEQUALIZER 0x000c0006u
#define HID_CP_FUNCTIONBUTTONS 0x000c0036u
#define HID_CP_SELECTION 0x000c0080u
#define HID_CP_MEDIASELECTION 0x000c0087u
#define HID_CP_SELECTDISC 0x000c00bau
#define HID_CP_VOLUMEUP 0x000c00e9u
#define HID_CP_VOLUMEDOWN 0x000c00eau
#define HID_CP_PLAYBACKSPEED 0x000c00f1u
#define HID_CP_PROXIMITY 0x000c0109u
#define HID_CP_SPEAKERSYSTEM 0x000c0160u
#define HID_CP_CHANNELLEFT 0x000c0161u
#define HID_CP_CHANNELRIGHT 0x000c0162u
#define HID_CP_CHANNELCENTER 0x000c0163u
#define HID_CP_CHANNELFRONT 0x000c0164u
#define HID_CP_CHANNELCENTERFRONT 0x000c0165u
#define HID_CP_CHANNELSIDE 0x000c0166u
#define HID_CP_CHANNELSURROUND 0x000c0167u
#define HID_CP_CHANNELLOWFREQUENCYENHANCEMENT 0x000c0168u
#define HID_CP_CHANNELTOP 0x000c0169u
#define HID_CP_CHANNELUNKNOWN 0x000c016au
#define HID_CP_APPLICATIONLAUNCHBUTTONS 0x000c0180u
#define HID_CP_GENERICGUIAPPLICATIONCONTROLS 0x000c0200u
#define HID_CP_AC_PAN 0x000c0238u

// ---------------------------------------------------------------------------
// Digitizer page (0x0d)
// ---------------------------------------------------------------------------
#define HID_DG_DIGITIZER 0x000d0001u
#define HID_DG_PEN 0x000d0002u
#define HID_DG_LIGHTPEN 0x000d0003u
#define HID_DG_TOUCHSCREEN 0x000d0004u
#define HID_DG_TOUCHPAD 0x000d0005u
#define HID_DG_WHITEBOARD 0x000d0006u
#define HID_DG_DEVICECONFIG 0x000d000eu
#define HID_DG_STYLUS 0x000d0020u
#define HID_DG_PUCK 0x000d0021u
#define HID_DG_FINGER 0x000d0022u
#define HID_DG_DEVICESETTINGS 0x000d0023u
#define HID_DG_CHARACTERGESTURE 0x000d0024u
#define HID_DG_TIPPRESSURE 0x000d0030u
#define HID_DG_BARRELPRESSURE 0x000d0031u
#define HID_DG_INRANGE 0x000d0032u
#define HID_DG_TOUCH 0x000d0033u
#define HID_DG_UNTOUCH 0x000d0034u
#define HID_DG_TAP 0x000d0035u
#define HID_DG_TRANSDUCER_INDEX 0x000d0038u
#define HID_DG_TABLETFUNCTIONKEY 0x000d0039u
#define HID_DG_PROGRAMCHANGEKEY 0x000d003au
#define HID_DG_BATTERYSTRENGTH 0x000d003bu
#define HID_DG_INVERT 0x000d003cu
#define HID_DG_TILT_X 0x000d003du
#define HID_DG_TILT_Y 0x000d003eu
#define HID_DG_AZIMUTH 0x000d003fu
#define HID_DG_TWIST 0x000d0041u
#define HID_DG_TIPSWITCH 0x000d0042u
#define HID_DG_TIPSWITCH2 0x000d0043u
#define HID_DG_BARRELSWITCH 0x000d0044u
#define HID_DG_ERASER 0x000d0045u
#define HID_DG_TABLETPICK 0x000d0046u
#define HID_DG_CONFIDENCE 0x000d0047u
#define HID_DG_WIDTH 0x000d0048u
#define HID_DG_HEIGHT 0x000d0049u
#define HID_DG_CONTACTID 0x000d0051u
#define HID_DG_INPUTMODE 0x000d0052u
#define HID_DG_DEVICEINDEX 0x000d0053u
#define HID_DG_CONTACTCOUNT 0x000d0054u
#define HID_DG_CONTACTMAX 0x000d0055u
#define HID_DG_SCANTIME 0x000d0056u
#define HID_DG_SURFACESWITCH 0x000d0057u
#define HID_DG_BUTTONSWITCH 0x000d0058u
#define HID_DG_BUTTONTYPE 0x000d0059u
#define HID_DG_BARRELSWITCH2 0x000d005au
#define HID_DG_TOOLSERIALNUMBER 0x000d005bu
#define HID_DG_LATENCYMODE 0x000d0060u

// ---------------------------------------------------------------------------
// Vendor usages that matter to the drivers
// ---------------------------------------------------------------------------
/* The 256-byte certification blob of a "Windows 8 compatible" touch
 * device, and its single-byte variant. */
#define HID_VD_WIN8_CERT_BLOB 0xff0000c5u
#define HID_VD_WIN8_CERT_BYTE 0xff0000c6u
/* Hot keys some ASUS keyboards declare inside the touchpad descriptor. */
#define HID_VD_ASUS_CUSTOM_MEDIA_KEYS 0xff310076u

// ---------------------------------------------------------------------------
// Collection types
// ---------------------------------------------------------------------------
#define HID_COLLECTION_PHYSICAL 0
#define HID_COLLECTION_APPLICATION 1
#define HID_COLLECTION_LOGICAL 2
#define HID_COLLECTION_NAMED_ARRAY 4

/* Applications hid-input turns into input devices. */
#define HID_IS_INPUT_APPLICATION(a)                                         \
	((((a) >= HID_UP_GENDESK) && ((a) <= HID_GD_MULTIAXIS)) ||          \
	 (((a) >= HID_DG_DIGITIZER) && ((a) <= HID_DG_WHITEBOARD)) ||       \
	 ((a) == HID_GD_SYSTEM_CONTROL) || ((a) == HID_CP_CONSUMER_CONTROL) || \
	 ((a) == HID_GD_WIRELESS_RADIO_CTLS))

#endif // _KERNEL_DEV_HID_CORE_HID_USAGES_H_
