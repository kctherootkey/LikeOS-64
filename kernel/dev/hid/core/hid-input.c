// LikeOS HID core - hid-input: HID usages to input events
//
// hidinput_connect() turns a bound HID device into input devices.  Which
// reports go into which input device depends on the device's quirks:
//
//   default                 every report into one input device
//   HID_QUIRK_INPUT_PER_APP one input device per application collection,
//                           named "<device> <suffix>" (Mouse, Keyboard,
//                           Touchpad, ...) when there is more than one
//   HID_QUIRK_MULTI_INPUT   one input device per report ID
//
// Every usage of every field of those reports is then mapped: the driver's
// input_mapping hook may claim it first, otherwise the tables below decide
// -- Generic Desktop axes become REL_* (relative fields) or ABS_* axes, the
// Button page BTN_* codes, the Keyboard page KEY_* codes through
// hid_keyboard[], the Consumer page multimedia keys and the horizontal
// wheel, the LED page LEDs, the Digitizer page touch and pen state.  An
// absolute axis gets its range from the logical range and its resolution
// from the physical range, the unit and the unit exponent.  Input devices
// that got nothing mapped are dropped; the rest are registered.
//
// At run time hidinput_hid_event() turns each value into an event (clamped
// to the logical range, out-of-range values of fields with a null state
// ignored, unchanged absolute values skipped, hi-resolution wheels split
// into notches and fractions), and hidinput_report_event() ends the packet.
// LED changes written to an input node go back to the device as an output
// report, from a work item.
//
// Copyright (C) 2026 The LikeOS Project
// Portions Copyright (C) 2000-2001 Vojtech Pavlik, (C) 2006-2010 Jiri
// Kosina (usage-to-event tables and mapping rules)

#include <kernel/dev/hid/core/hid.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */

#define INT32_MAX_VALUE 2147483647LL

// ---------------------------------------------------------------------------
// Keyboard page usages 0x00..0xff to key codes (0: not a key).
// ---------------------------------------------------------------------------
static const uint16_t hid_keyboard[256] = {
	[0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
	[0x08] = KEY_E, [0x09] = KEY_F, [0x0a] = KEY_G, [0x0b] = KEY_H,
	[0x0c] = KEY_I, [0x0d] = KEY_J, [0x0e] = KEY_K, [0x0f] = KEY_L,
	[0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
	[0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
	[0x18] = KEY_U, [0x19] = KEY_V, [0x1a] = KEY_W, [0x1b] = KEY_X,
	[0x1c] = KEY_Y, [0x1d] = KEY_Z, [0x1e] = KEY_1, [0x1f] = KEY_2,
	[0x20] = KEY_3, [0x21] = KEY_4, [0x22] = KEY_5, [0x23] = KEY_6,
	[0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9, [0x27] = KEY_0,
	[0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2a] = KEY_BACKSPACE,
	[0x2b] = KEY_TAB, [0x2c] = KEY_SPACE, [0x2d] = KEY_MINUS,
	[0x2e] = KEY_EQUAL, [0x2f] = KEY_LEFTBRACE, [0x30] = KEY_RIGHTBRACE,
	[0x31] = KEY_BACKSLASH, [0x32] = KEY_BACKSLASH, // non-US # and ~
	[0x33] = KEY_SEMICOLON, [0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE,
	[0x36] = KEY_COMMA, [0x37] = KEY_DOT, [0x38] = KEY_SLASH,
	[0x39] = KEY_CAPSLOCK, [0x3a] = KEY_F1, [0x3b] = KEY_F2,
	[0x3c] = KEY_F3, [0x3d] = KEY_F4, [0x3e] = KEY_F5, [0x3f] = KEY_F6,
	[0x40] = KEY_F7, [0x41] = KEY_F8, [0x42] = KEY_F9, [0x43] = KEY_F10,
	[0x44] = KEY_F11, [0x45] = KEY_F12, [0x46] = KEY_SYSRQ,
	[0x47] = KEY_SCROLLLOCK, [0x48] = KEY_PAUSE, [0x49] = KEY_INSERT,
	[0x4a] = KEY_HOME, [0x4b] = KEY_PAGEUP, [0x4c] = KEY_DELETE,
	[0x4d] = KEY_END, [0x4e] = KEY_PAGEDOWN, [0x4f] = KEY_RIGHT,
	[0x50] = KEY_LEFT, [0x51] = KEY_DOWN, [0x52] = KEY_UP,
	[0x53] = KEY_NUMLOCK, [0x54] = KEY_KPSLASH, [0x55] = KEY_KPASTERISK,
	[0x56] = KEY_KPMINUS, [0x57] = KEY_KPPLUS, [0x58] = KEY_KPENTER,
	[0x59] = KEY_KP1, [0x5a] = KEY_KP2, [0x5b] = KEY_KP3, [0x5c] = KEY_KP4,
	[0x5d] = KEY_KP5, [0x5e] = KEY_KP6, [0x5f] = KEY_KP7, [0x60] = KEY_KP8,
	[0x61] = KEY_KP9, [0x62] = KEY_KP0, [0x63] = KEY_KPDOT,
	[0x64] = KEY_102ND, [0x65] = KEY_COMPOSE, [0x66] = KEY_POWER,
	[0x67] = KEY_KPEQUAL, [0x68] = KEY_F13, [0x69] = KEY_F14,
	[0x6a] = KEY_F15, [0x6b] = KEY_F16, [0x6c] = KEY_F17, [0x6d] = KEY_F18,
	[0x6e] = KEY_F19, [0x6f] = KEY_F20, [0x70] = KEY_F21, [0x71] = KEY_F22,
	[0x72] = KEY_F23, [0x73] = KEY_F24, [0x74] = KEY_OPEN,
	[0x75] = KEY_HELP, [0x76] = KEY_PROPS, [0x77] = KEY_FRONT,
	[0x78] = KEY_STOP, [0x79] = KEY_AGAIN, [0x7a] = KEY_UNDO,
	[0x7b] = KEY_CUT, [0x7c] = KEY_COPY, [0x7d] = KEY_PASTE,
	[0x7e] = KEY_FIND, [0x7f] = KEY_MUTE, [0x80] = KEY_VOLUMEUP,
	[0x81] = KEY_VOLUMEDOWN, [0x82] = KEY_UNKNOWN, [0x83] = KEY_UNKNOWN,
	[0x84] = KEY_UNKNOWN, [0x85] = KEY_KPCOMMA, [0x86] = KEY_UNKNOWN,
	[0x87] = KEY_RO, [0x88] = KEY_KATAKANAHIRAGANA, [0x89] = KEY_YEN,
	[0x8a] = KEY_HENKAN, [0x8b] = KEY_MUHENKAN, [0x8c] = KEY_KPJPCOMMA,
	[0x8d] = KEY_UNKNOWN, [0x8e] = KEY_UNKNOWN, [0x8f] = KEY_UNKNOWN,
	[0x90] = KEY_HANGEUL, [0x91] = KEY_HANJA, [0x92] = KEY_KATAKANA,
	[0x93] = KEY_HIRAGANA, [0x94] = KEY_ZENKAKUHANKAKU,
	[0x95 ... 0x9b] = KEY_UNKNOWN, [0x9c] = KEY_DELETE,
	[0x9d ... 0xb5] = KEY_UNKNOWN, [0xb6] = KEY_KPLEFTPAREN,
	[0xb7] = KEY_KPRIGHTPAREN, [0xb8 ... 0xd7] = KEY_UNKNOWN,
	[0xd8] = KEY_DELETE, [0xd9 ... 0xdf] = KEY_UNKNOWN,
	[0xe0] = KEY_LEFTCTRL, [0xe1] = KEY_LEFTSHIFT, [0xe2] = KEY_LEFTALT,
	[0xe3] = KEY_LEFTMETA, [0xe4] = KEY_RIGHTCTRL, [0xe5] = KEY_RIGHTSHIFT,
	[0xe6] = KEY_RIGHTALT, [0xe7] = KEY_RIGHTMETA, [0xe8] = KEY_PLAYPAUSE,
	[0xe9] = KEY_STOPCD, [0xea] = KEY_PREVIOUSSONG, [0xeb] = KEY_NEXTSONG,
	[0xec] = KEY_EJECTCD, [0xed] = KEY_VOLUMEUP, [0xee] = KEY_VOLUMEDOWN,
	[0xef] = KEY_MUTE, [0xf0] = KEY_WWW, [0xf1] = KEY_BACK,
	[0xf2] = KEY_FORWARD, [0xf3] = KEY_STOP, [0xf4] = KEY_FIND,
	[0xf5] = KEY_SCROLLUP, [0xf6] = KEY_SCROLLDOWN, [0xf7] = KEY_EDIT,
	[0xf8] = KEY_SLEEP, [0xf9] = KEY_COFFEE, [0xfa] = KEY_REFRESH,
	[0xfb] = KEY_CALC, [0xfc ... 0xff] = KEY_UNKNOWN,
};

// ---------------------------------------------------------------------------
// Consumer page usages that are keys.  Not listed: 0x000 (ignored),
// 0x0e0 (volume axis), 0x238 (horizontal wheel), see below; everything
// else maps to KEY_UNKNOWN.
// ---------------------------------------------------------------------------
struct hid_usage_key {
	uint16_t usage;
	uint16_t code;
};

static const struct hid_usage_key hid_consumer_keys[] = {
	{ 0x030, KEY_POWER },
	{ 0x031, KEY_RESTART },
	{ 0x032, KEY_SLEEP },
	{ 0x034, KEY_SLEEP },
	{ 0x035, KEY_KBDILLUMTOGGLE },
	{ 0x036, BTN_MISC },
	{ 0x040, KEY_MENU }, // Menu
	{ 0x041, KEY_SELECT }, // Menu Pick
	{ 0x042, KEY_UP }, // Menu Up
	{ 0x043, KEY_DOWN },
	{ 0x044, KEY_LEFT },
	{ 0x045, KEY_RIGHT },
	{ 0x046, KEY_ESC }, // Menu Escape
	{ 0x047, KEY_KPPLUS }, // Menu Value Increase
	{ 0x048, KEY_KPMINUS },
	{ 0x060, KEY_INFO }, // Data On Screen
	{ 0x061, KEY_SUBTITLE }, // Closed Caption
	{ 0x063, KEY_VCR },
	{ 0x065, KEY_CAMERA }, // Snapshot
	{ 0x069, KEY_RED },
	{ 0x06a, KEY_GREEN },
	{ 0x06b, KEY_BLUE },
	{ 0x06c, KEY_YELLOW },
	{ 0x06d, KEY_ASPECT_RATIO },
	{ 0x06f, KEY_BRIGHTNESSUP },
	{ 0x070, KEY_BRIGHTNESSDOWN },
	{ 0x072, KEY_BRIGHTNESS_TOGGLE },
	{ 0x073, KEY_BRIGHTNESS_MIN },
	{ 0x074, KEY_BRIGHTNESS_MAX },
	{ 0x075, KEY_BRIGHTNESS_AUTO },
	{ 0x076, KEY_CAMERA_ACCESS_ENABLE },
	{ 0x077, KEY_CAMERA_ACCESS_DISABLE },
	{ 0x078, KEY_CAMERA_ACCESS_TOGGLE },
	{ 0x079, KEY_KBDILLUMUP },
	{ 0x07a, KEY_KBDILLUMDOWN },
	{ 0x07c, KEY_KBDILLUMTOGGLE },
	{ 0x082, KEY_VIDEO_NEXT },
	{ 0x083, KEY_LAST },
	{ 0x084, KEY_ENTER },
	{ 0x088, KEY_PC },
	{ 0x089, KEY_TV },
	{ 0x08a, KEY_WWW },
	{ 0x08b, KEY_DVD },
	{ 0x08c, KEY_PHONE },
	{ 0x08d, KEY_PROGRAM },
	{ 0x08e, KEY_VIDEOPHONE },
	{ 0x08f, KEY_GAMES },
	{ 0x090, KEY_MEMO },
	{ 0x091, KEY_CD },
	{ 0x092, KEY_VCR },
	{ 0x093, KEY_TUNER },
	{ 0x094, KEY_EXIT },
	{ 0x095, KEY_HELP },
	{ 0x096, KEY_TAPE },
	{ 0x097, KEY_TV2 },
	{ 0x098, KEY_SAT },
	{ 0x09a, KEY_PVR },
	{ 0x09c, KEY_CHANNELUP },
	{ 0x09d, KEY_CHANNELDOWN },
	{ 0x0a0, KEY_VCR2 },
	{ 0x0b0, KEY_PLAY },
	{ 0x0b1, KEY_PAUSE },
	{ 0x0b2, KEY_RECORD },
	{ 0x0b3, KEY_FASTFORWARD },
	{ 0x0b4, KEY_REWIND },
	{ 0x0b5, KEY_NEXTSONG },
	{ 0x0b6, KEY_PREVIOUSSONG },
	{ 0x0b7, KEY_STOPCD },
	{ 0x0b8, KEY_EJECTCD },
	{ 0x0b9, KEY_SHUFFLE },
	{ 0x0bc, KEY_MEDIA_REPEAT },
	{ 0x0bf, KEY_SLOW },
	{ 0x0cd, KEY_PLAYPAUSE },
	{ 0x0cf, KEY_VOICECOMMAND },
	{ 0x0d8, KEY_DICTATE },
	{ 0x0d9, KEY_EMOJI_PICKER },
	{ 0x0e2, KEY_MUTE },
	{ 0x0e5, KEY_BASSBOOST },
	{ 0x0e9, KEY_VOLUMEUP },
	{ 0x0ea, KEY_VOLUMEDOWN },
	{ 0x0f5, KEY_SLOW },
	{ 0x181, KEY_BUTTONCONFIG },
	{ 0x182, KEY_BOOKMARKS },
	{ 0x183, KEY_CONFIG },
	{ 0x184, KEY_WORDPROCESSOR },
	{ 0x185, KEY_EDITOR },
	{ 0x186, KEY_SPREADSHEET },
	{ 0x187, KEY_GRAPHICSEDITOR },
	{ 0x188, KEY_PRESENTATION },
	{ 0x189, KEY_DATABASE },
	{ 0x18a, KEY_MAIL },
	{ 0x18b, KEY_NEWS },
	{ 0x18c, KEY_VOICEMAIL },
	{ 0x18d, KEY_ADDRESSBOOK },
	{ 0x18e, KEY_CALENDAR },
	{ 0x18f, KEY_TASKMANAGER },
	{ 0x190, KEY_JOURNAL },
	{ 0x191, KEY_FINANCE },
	{ 0x192, KEY_CALC },
	{ 0x193, KEY_PLAYER },
	{ 0x194, KEY_FILE },
	{ 0x196, KEY_WWW },
	{ 0x199, KEY_CHAT },
	{ 0x19c, KEY_LOGOFF },
	{ 0x19e, KEY_COFFEE },
	{ 0x19f, KEY_CONTROLPANEL },
	{ 0x1a2, KEY_APPSELECT },
	{ 0x1a3, KEY_NEXT },
	{ 0x1a4, KEY_PREVIOUS },
	{ 0x1a6, KEY_HELP },
	{ 0x1a7, KEY_DOCUMENTS },
	{ 0x1ab, KEY_SPELLCHECK },
	{ 0x1ae, KEY_KEYBOARD },
	{ 0x1b1, KEY_SCREENSAVER },
	{ 0x1b4, KEY_FILE },
	{ 0x1b6, KEY_IMAGES },
	{ 0x1b7, KEY_AUDIO },
	{ 0x1b8, KEY_VIDEO },
	{ 0x1bc, KEY_MESSENGER },
	{ 0x1bd, KEY_INFO },
	{ 0x1cb, KEY_ASSISTANT },
	{ 0x201, KEY_NEW },
	{ 0x202, KEY_OPEN },
	{ 0x203, KEY_CLOSE },
	{ 0x204, KEY_EXIT },
	{ 0x207, KEY_SAVE },
	{ 0x208, KEY_PRINT },
	{ 0x209, KEY_PROPS },
	{ 0x21a, KEY_UNDO },
	{ 0x21b, KEY_COPY },
	{ 0x21c, KEY_CUT },
	{ 0x21d, KEY_PASTE },
	{ 0x21f, KEY_FIND },
	{ 0x221, KEY_SEARCH },
	{ 0x222, KEY_GOTO },
	{ 0x223, KEY_HOMEPAGE },
	{ 0x224, KEY_BACK },
	{ 0x225, KEY_FORWARD },
	{ 0x226, KEY_STOP },
	{ 0x227, KEY_REFRESH },
	{ 0x22a, KEY_BOOKMARKS },
	{ 0x22d, KEY_ZOOMIN },
	{ 0x22e, KEY_ZOOMOUT },
	{ 0x22f, KEY_ZOOMRESET },
	{ 0x232, KEY_FULL_SCREEN },
	{ 0x233, KEY_SCROLLUP },
	{ 0x234, KEY_SCROLLDOWN },
	{ 0x23d, KEY_EDIT },
	{ 0x25f, KEY_CANCEL },
	{ 0x269, KEY_INSERT },
	{ 0x26a, KEY_DELETE },
	{ 0x279, KEY_REDO },
	{ 0x289, KEY_REPLY },
	{ 0x28b, KEY_FORWARDMAIL },
	{ 0x28c, KEY_SEND },
	{ 0x29d, KEY_KBD_LAYOUT_NEXT },
	{ 0x29f, KEY_SCALE },
	{ 0x2a2, KEY_ALL_APPLICATIONS },
	{ 0x2c7, KEY_KBDINPUTASSIST_PREV },
	{ 0x2c8, KEY_KBDINPUTASSIST_NEXT },
	{ 0x2c9, KEY_KBDINPUTASSIST_PREVGROUP },
	{ 0x2ca, KEY_KBDINPUTASSIST_NEXTGROUP },
	{ 0x2cb, KEY_KBDINPUTASSIST_ACCEPT },
	{ 0x2cc, KEY_KBDINPUTASSIST_CANCEL },
};

/* Telephony page keys. */
static const struct hid_usage_key hid_telephony_keys[] = {
	{ 0x02f, KEY_MICMUTE },	   { 0x0b0, KEY_NUMERIC_0 },
	{ 0x0b1, KEY_NUMERIC_1 },  { 0x0b2, KEY_NUMERIC_2 },
	{ 0x0b3, KEY_NUMERIC_3 },  { 0x0b4, KEY_NUMERIC_4 },
	{ 0x0b5, KEY_NUMERIC_5 },  { 0x0b6, KEY_NUMERIC_6 },
	{ 0x0b7, KEY_NUMERIC_7 },  { 0x0b8, KEY_NUMERIC_8 },
	{ 0x0b9, KEY_NUMERIC_9 },  { 0x0ba, KEY_NUMERIC_STAR },
	{ 0x0bb, KEY_NUMERIC_POUND }, { 0x0bc, KEY_NUMERIC_A },
	{ 0x0bd, KEY_NUMERIC_B },  { 0x0be, KEY_NUMERIC_C },
	{ 0x0bf, KEY_NUMERIC_D },
};

/* HP vendor page keys (keyboards of HP desktops and notebooks). */
static const struct hid_usage_key hid_hpvendor_keys[] = {
	{ 0x021, KEY_PRINT },  { 0x070, KEY_HP },	{ 0x071, KEY_CAMERA },
	{ 0x072, KEY_SOUND },  { 0x073, KEY_QUESTION }, { 0x080, KEY_EMAIL },
	{ 0x081, KEY_CHAT },   { 0x082, KEY_SEARCH },	{ 0x083, KEY_CONNECT },
	{ 0x084, KEY_FINANCE }, { 0x085, KEY_SPORT },	{ 0x086, KEY_SHOP },
};

static int hid_lookup_key(const struct hid_usage_key *table, unsigned int n,
			  unsigned int usage)
{
	for (unsigned int i = 0; i < n; i++)
		if (table[i].usage == usage)
			return table[i].code;
	return -1;
}

/* Hat switch direction 0 (centred) .. 8 (north-west) to axis values. */
static const struct {
	int8_t x, y;
} hid_hat_to_axis[] = { { 0, 0 },  { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
			{ 0, 1 },  { -1, 1 }, { -1, 0 }, { -1, -1 } };

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------

void hid_map_usage(struct hid_input *hidinput, struct hid_usage *usage,
		   unsigned long **bit, int *max, uint8_t type, unsigned int c)
{
	struct input_dev *input = hidinput->input;
	unsigned long *bmap = NULL;
	int limit = 0;

	switch (type) {
	case EV_ABS:
		bmap = input->absbit;
		limit = ABS_MAX;
		break;
	case EV_REL:
		bmap = input->relbit;
		limit = REL_MAX;
		break;
	case EV_KEY:
		bmap = input->keybit;
		limit = KEY_MAX;
		break;
	case EV_LED:
		bmap = input->ledbit;
		limit = LED_MAX;
		break;
	case EV_MSC:
		bmap = input->mscbit;
		limit = MSC_MAX;
		break;
	}

	if (!bmap || c > (unsigned int)limit) {
		kprintf("hid: %s: invalid code %u type %u\n",
			input->name ? input->name : "?", c, type);
		*bit = NULL;
		return;
	}
	usage->type = type;
	usage->code = (uint16_t)c;
	*max = limit;
	*bit = bmap;
}

void hid_map_usage_clear(struct hid_input *hidinput, struct hid_usage *usage,
			 unsigned long **bit, int *max, uint8_t type,
			 unsigned int c)
{
	hid_map_usage(hidinput, usage, bit, max, type, c);
	if (*bit)
		input_clear_bit(usage->code, *bit);
}

int hidinput_set_name(struct hid_input *hi, const char *base,
		      const char *suffix)
{
	size_t len = kstrlen(base) + (suffix ? kstrlen(suffix) + 1 : 0) + 1;
	char *name = kalloc(len);

	if (!name)
		return -ENOMEM;
	if (suffix)
		ksnprintf(name, len, "%s %s", base, suffix);
	else
		ksnprintf(name, len, "%s", base);
	kfree(hi->name);
	hi->name = name;
	hi->input->name = name;
	return 0;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

int32_t hidinput_calc_abs_res(const struct hid_field *field, uint16_t code)
{
	int64_t unit_exponent = field->unit_exponent;
	int64_t logical = (int64_t)field->logical_maximum -
			  field->logical_minimum;
	int64_t physical = (int64_t)field->physical_maximum -
			   field->physical_minimum;

	/* Extents beyond 32 bits are as meaningless as empty ones. */
	if (logical <= 0 || physical <= 0 || logical > INT32_MAX_VALUE ||
	    physical > INT32_MAX_VALUE)
		return 0;

	switch (code) {
	case ABS_X:
	case ABS_Y:
	case ABS_Z:
	case ABS_MT_POSITION_X:
	case ABS_MT_POSITION_Y:
	case ABS_MT_TOOL_X:
	case ABS_MT_TOOL_Y:
	case ABS_MT_TOUCH_MAJOR:
	case ABS_MT_TOUCH_MINOR:
		if (field->unit == 0x11) { // centimetres -> millimetres
			unit_exponent += 1;
		} else if (field->unit == 0x13) { // inches -> millimetres
			physical *= 254;
			if (physical > INT32_MAX_VALUE)
				return 0;
			unit_exponent -= 1;
		} else {
			return 0;
		}
		break;

	case ABS_RX:
	case ABS_RY:
	case ABS_RZ:
	case ABS_WHEEL:
	case ABS_TILT_X:
	case ABS_TILT_Y:
		if (field->unit == 0x14) { // degrees -> radians
			logical *= 573;
			if (logical > INT32_MAX_VALUE)
				return 0;
			unit_exponent += 1;
		} else if (field->unit != 0x12) { // not radians
			return 0;
		}
		break;

	default:
		return 0;
	}

	/* A negative exponent scales the logical extent up, a positive one
	 * the physical extent; past 32 bits there is no resolution. */
	for (; unit_exponent < 0; unit_exponent++) {
		logical *= 10;
		if (logical > INT32_MAX_VALUE)
			return 0;
	}
	for (; unit_exponent > 0; unit_exponent--) {
		physical *= 10;
		if (physical > INT32_MAX_VALUE)
			return 0;
	}
	return (int32_t)((logical + physical / 2) / physical);
}

// ---------------------------------------------------------------------------
// Mapping one usage
// ---------------------------------------------------------------------------

static bool hidinput_field_in_collection(struct hid_device *device,
					 struct hid_field *field,
					 unsigned int type, unsigned int usage)
{
	struct hid_collection *collection;

	if (field->usage->collection_index >= device->maxcollection)
		return false;
	collection = &device->collection[field->usage->collection_index];
	return collection->type == type && collection->usage == usage;
}

#define map_abs(c) hid_map_usage(hidinput, usage, &bit, &max, EV_ABS, (c))
#define map_rel(c) hid_map_usage(hidinput, usage, &bit, &max, EV_REL, (c))
#define map_key(c) hid_map_usage(hidinput, usage, &bit, &max, EV_KEY, (c))
#define map_led(c) hid_map_usage(hidinput, usage, &bit, &max, EV_LED, (c))
#define map_msc(c) hid_map_usage(hidinput, usage, &bit, &max, EV_MSC, (c))
#define map_abs_clear(c) \
	hid_map_usage_clear(hidinput, usage, &bit, &max, EV_ABS, (c))
#define map_key_clear(c) \
	hid_map_usage_clear(hidinput, usage, &bit, &max, EV_KEY, (c))

static void hidinput_configure_usage(struct hid_input *hidinput,
				     struct hid_field *field,
				     struct hid_usage *usage)
{
	struct input_dev *input = hidinput->input;
	struct hid_device *device = input_get_drvdata(input);
	unsigned long *bit = NULL;
	int max = 0, code;
	unsigned int page = usage->hid & HID_USAGE_PAGE;
	unsigned int id = usage->hid & HID_USAGE;

	field->hidinput = hidinput;

	if (field->flags & HID_MAIN_ITEM_CONSTANT)
		goto ignore;
	if (field->report_count < 1)
		goto ignore;
	/* Of output fields only LEDs (and haptics) mean anything here. */
	if (field->report_type == HID_OUTPUT_REPORT && page != HID_UP_LED &&
	    page != HID_UP_HAPTIC)
		goto ignore;

	if (device->driver->input_mapping) {
		int ret = device->driver->input_mapping(device, hidinput, field,
							usage, &bit, &max);

		if (ret > 0)
			goto mapped;
		if (ret < 0)
			goto ignore;
	}

	switch (page) {
	case HID_UP_UNDEFINED:
		goto ignore;

	case HID_UP_KEYBOARD:
		input_set_bit(EV_REP, input->evbit);
		if (id < 256) {
			if (!hid_keyboard[id])
				goto ignore;
			map_key_clear(hid_keyboard[id]);
		} else {
			map_key(KEY_UNKNOWN);
		}
		break;

	case HID_UP_BUTTON:
		code = (int)((usage->hid - 1) & HID_USAGE);
		switch (field->application) {
		case HID_GD_MOUSE:
		case HID_GD_POINTER:
			code += BTN_MOUSE;
			break;
		case HID_GD_JOYSTICK:
			if (code <= 0xf)
				code += BTN_JOYSTICK;
			else
				code += BTN_TRIGGER_HAPPY - 0x10;
			break;
		case HID_GD_GAMEPAD:
			if (code <= 0xf)
				code += BTN_GAMEPAD;
			else
				code += BTN_TRIGGER_HAPPY - 0x10;
			break;
		case HID_CP_CONSUMER_CONTROL:
			if (hidinput_field_in_collection(
				    device, field, HID_COLLECTION_NAMED_ARRAY,
				    HID_CP_PROGRAMMABLEBUTTONS)) {
				if (code <= 0x1d)
					code += KEY_MACRO1;
				else
					code += BTN_TRIGGER_HAPPY - 0x1e;
				break;
			}
			__attribute__((fallthrough));
		default:
			switch (field->physical) {
			case HID_GD_MOUSE:
			case HID_GD_POINTER:
				code += BTN_MOUSE;
				break;
			case HID_GD_JOYSTICK:
				code += BTN_JOYSTICK;
				break;
			case HID_GD_GAMEPAD:
				code += BTN_GAMEPAD;
				break;
			default:
				code += BTN_MISC;
			}
		}
		map_key(code);
		break;

	case HID_UP_SIMULATION:
		switch (id) {
		case 0xba:
			map_abs(ABS_RUDDER);
			break;
		case 0xbb:
			map_abs(ABS_THROTTLE);
			break;
		case 0xc4:
			map_abs(ABS_GAS);
			break;
		case 0xc5:
			map_abs(ABS_BRAKE);
			break;
		case 0xc8:
			map_abs(ABS_WHEEL);
			break;
		default:
			goto ignore;
		}
		break;

	case HID_UP_GENDESK:
		if ((usage->hid & 0xf0) == 0x80) { // System Control
			static const uint16_t sysctl[16] = {
				[0x1] = KEY_POWER,	  [0x2] = KEY_SLEEP,
				[0x3] = KEY_WAKEUP,	  [0x4] = KEY_CONTEXT_MENU,
				[0x5] = KEY_MENU,	  [0x6] = KEY_PROG1,
				[0x7] = KEY_HELP,	  [0x8] = KEY_EXIT,
				[0x9] = KEY_SELECT,	  [0xa] = KEY_RIGHT,
				[0xb] = KEY_LEFT,	  [0xc] = KEY_UP,
				[0xd] = KEY_DOWN,	  [0xe] = KEY_POWER2,
				[0xf] = KEY_RESTART,
			};

			if (!sysctl[usage->hid & 0xf])
				goto unknown;
			map_key_clear(sysctl[usage->hid & 0xf]);
			break;
		}
		if ((usage->hid & 0xf0) == 0x90) { // D-pad, Do Not Disturb
			switch (usage->hid) {
			case HID_GD_UP:
				usage->hat_dir = 1;
				break;
			case HID_GD_DOWN:
				usage->hat_dir = 5;
				break;
			case HID_GD_RIGHT:
				usage->hat_dir = 3;
				break;
			case HID_GD_LEFT:
				usage->hat_dir = 7;
				break;
			case HID_GD_DO_NOT_DISTURB:
				map_key_clear(KEY_DO_NOT_DISTURB);
				break;
			default:
				goto unknown;
			}
			/* The four directions of a D-pad share one hat: the
			 * first one maps it, the others report into it. */
			if (usage->hid <= HID_GD_LEFT) {
				if (field->dpad) {
					map_abs(field->dpad);
					goto ignore;
				}
				map_abs(ABS_HAT0X);
			}
			break;
		}
		if ((usage->hid & 0xf0) == 0xa0) { // System Control, more
			switch (usage->hid & 0xf) {
			case 0x9:
				map_key_clear(KEY_MICMUTE);
				break;
			case 0xa:
				map_key_clear(KEY_ACCESSIBILITY);
				break;
			default:
				goto ignore;
			}
			break;
		}
		if ((usage->hid & 0xf0) == 0xb0) { // System Display
			if ((usage->hid & 0xf) == 0x05) {
				map_key_clear(KEY_SWITCHVIDEOMODE);
				break;
			}
			goto ignore;
		}
		/* A System Control application declaring hundreds of
		 * usages would otherwise sprout axes. */
		if (field->application == HID_GD_SYSTEM_CONTROL)
			goto ignore;

		switch (usage->hid) {
		case HID_GD_Z:
			/* A pen's Z is its distance from the surface. */
			if (!(field->flags & HID_MAIN_ITEM_RELATIVE) &&
			    (field->application == HID_DG_PEN ||
			     field->physical == HID_DG_PEN ||
			     field->logical == HID_DG_STYLUS ||
			     field->physical == HID_DG_STYLUS ||
			     field->application == HID_DG_DIGITIZER)) {
				map_abs_clear(ABS_DISTANCE);
				break;
			}
			__attribute__((fallthrough));
		case HID_GD_X:
		case HID_GD_Y:
		case HID_GD_RX:
		case HID_GD_RY:
		case HID_GD_RZ:
			if (field->flags & HID_MAIN_ITEM_RELATIVE)
				map_rel(usage->hid & 0xf);
			else
				map_abs_clear(usage->hid & 0xf);
			break;
		case HID_GD_WHEEL:
			if (field->flags & HID_MAIN_ITEM_RELATIVE) {
				/* Notches on REL_WHEEL, 1/120 notches on the
				 * hi-res axis. */
				input_set_bit(REL_WHEEL, input->relbit);
				map_rel(REL_WHEEL_HI_RES);
			} else {
				map_abs(usage->hid & 0xf);
			}
			break;
		case HID_GD_SLIDER:
		case HID_GD_DIAL:
			if (field->flags & HID_MAIN_ITEM_RELATIVE)
				map_rel(usage->hid & 0xf);
			else
				map_abs(usage->hid & 0xf);
			break;
		case HID_GD_HATSWITCH:
			usage->hat_min = (int16_t)field->logical_minimum;
			usage->hat_max = (int16_t)field->logical_maximum;
			map_abs(ABS_HAT0X);
			break;
		case HID_GD_START:
			map_key_clear(BTN_START);
			break;
		case HID_GD_SELECT:
			map_key_clear(BTN_SELECT);
			break;
		case HID_GD_RFKILL_BTN:
			if (field->application == HID_GD_WIRELESS_RADIO_CTLS) {
				map_key_clear(KEY_RFKILL);
				/* A press without a release: send both. */
				field->flags |= HID_MAIN_ITEM_RELATIVE;
				break;
			}
			goto unknown;
		default:
			goto unknown;
		}
		break;

	case HID_UP_LED:
		switch (id) {
		case 0x01:
			map_led(LED_NUML);
			break;
		case 0x02:
			map_led(LED_CAPSL);
			break;
		case 0x03:
			map_led(LED_SCROLLL);
			break;
		case 0x04:
			map_led(LED_COMPOSE);
			break;
		case 0x05:
			map_led(LED_KANA);
			break;
		case 0x27:
			map_led(LED_SLEEP);
			break;
		case 0x4c:
			map_led(LED_SUSPEND);
			break;
		case 0x09:
			map_led(LED_MUTE);
			break;
		case 0x4b:
			map_led(LED_MISC);
			break;
		case 0x19:
			map_led(LED_MAIL);
			break;
		case 0x4d:
			map_led(LED_CHARGING);
			break;
		default:
			goto ignore;
		}
		break;

	case HID_UP_DIGITIZER:
		if ((field->application & 0xff) == 0x01) // Digitizer
			input_set_bit(INPUT_PROP_POINTER, input->propbit);
		else if ((field->application & 0xff) == 0x02) // Pen
			input_set_bit(INPUT_PROP_DIRECT, input->propbit);

		switch (usage->hid & 0xff) {
		case 0x00: // undefined
			goto ignore;
		case 0x30: // Tip Pressure
			if (!input_test_bit(BTN_TOUCH, input->keybit)) {
				device->quirks |= HID_QUIRK_NOTOUCH;
				input_set_bit(EV_KEY, input->evbit);
				input_set_bit(BTN_TOUCH, input->keybit);
			}
			map_abs_clear(ABS_PRESSURE);
			break;
		case 0x32: // In Range
			switch (field->physical) {
			case HID_DG_PUCK:
				map_key(BTN_TOOL_MOUSE);
				break;
			case HID_DG_FINGER:
				map_key(BTN_TOOL_FINGER);
				break;
			default:
				if (!field->physical) {
					switch (field->application) {
					case HID_DG_TOUCHSCREEN:
					case HID_DG_TOUCHPAD:
						map_key_clear(BTN_TOOL_FINGER);
						break;
					default:
						map_key_clear(BTN_TOOL_PEN);
					}
				} else {
					map_key(BTN_TOOL_PEN);
				}
				break;
			}
			break;
		case 0x3b: // Battery Strength: no power-supply class here
			goto ignore;
		case 0x3c: // Invert
			device->quirks &= ~HID_QUIRK_NOINVERT;
			map_key_clear(BTN_TOOL_RUBBER);
			break;
		case 0x3d: // X Tilt
			map_abs_clear(ABS_TILT_X);
			break;
		case 0x3e: // Y Tilt
			map_abs_clear(ABS_TILT_Y);
			break;
		case 0x33: // Touch
		case 0x42: // Tip Switch
		case 0x43: // Secondary Tip Switch
			device->quirks &= ~HID_QUIRK_NOTOUCH;
			map_key_clear(BTN_TOUCH);
			break;
		case 0x44: // Barrel Switch
			map_key_clear(BTN_STYLUS);
			break;
		case 0x45: // Eraser
			/* The eraser touching; the tool itself comes from
			 * Invert, or from Eraser when there is no Invert. */
			if (!input_test_bit(BTN_TOOL_RUBBER, input->keybit)) {
				device->quirks |= HID_QUIRK_NOINVERT;
				input_set_bit(BTN_TOOL_RUBBER, input->keybit);
			}
			map_key_clear(BTN_TOUCH);
			break;
		case 0x46: // Tablet Pick
		case 0x5a: // Secondary Barrel Switch
			map_key_clear(BTN_STYLUS2);
			break;
		case 0x5b: // Transducer Serial Number
		case 0x6e: // Transducer Serial Number Part 2
			map_msc(MSC_SERIAL);
			break;
		default:
			goto unknown;
		}
		break;

	case HID_UP_TELEPHONY:
		code = hid_lookup_key(hid_telephony_keys,
				      HID_ARRAY_SIZE(hid_telephony_keys), id);
		if (code < 0)
			goto ignore;
		map_key_clear(code);
		break;

	case HID_UP_CONSUMER:
		if (id == 0x000)
			goto ignore;
		if (id == 0x0e0) { // Volume, as an axis
			map_abs_clear(ABS_VOLUME);
			break;
		}
		if (id == 0x238) { // AC Pan: the horizontal wheel
			input_set_bit(REL_HWHEEL, input->relbit);
			map_rel(REL_HWHEEL_HI_RES);
			break;
		}
		code = hid_lookup_key(hid_consumer_keys,
				      HID_ARRAY_SIZE(hid_consumer_keys), id);
		map_key_clear(code < 0 ? KEY_UNKNOWN : code);
		break;

	case HID_UP_GENDEVCTRLS:
		if (usage->hid == HID_DC_BATTERYSTRENGTH)
			goto ignore; // battery: no power-supply class
		goto unknown;

	case HID_UP_BATTERY:
		if (usage->hid == HID_BAT_ABSOLUTESTATEOFCHARGE ||
		    usage->hid == HID_BAT_CHARGING)
			goto ignore;
		goto unknown;

	case HID_UP_CAMERA:
		switch (id) {
		case 0x020:
			map_key_clear(KEY_CAMERA_FOCUS);
			break;
		case 0x021:
			map_key_clear(KEY_CAMERA);
			break;
		default:
			goto ignore;
		}
		break;

	case HID_UP_HPVENDOR: // seen on HP keyboards
		input_set_bit(EV_REP, input->evbit);
		code = hid_lookup_key(hid_hpvendor_keys,
				      HID_ARRAY_SIZE(hid_hpvendor_keys), id);
		if (code < 0)
			goto ignore;
		map_key_clear(code);
		break;

	case HID_UP_HPVENDOR2:
		input_set_bit(EV_REP, input->evbit);
		switch (id) {
		case 0x001:
			map_key_clear(KEY_MICMUTE);
			break;
		case 0x003:
			map_key_clear(KEY_BRIGHTNESSDOWN);
			break;
		case 0x004:
			map_key_clear(KEY_BRIGHTNESSUP);
			break;
		default:
			goto ignore;
		}
		break;

	case HID_UP_MSVENDOR:
		goto ignore;

	case HID_UP_CUSTOM: // Apple and Logitech keyboards
		input_set_bit(EV_REP, input->evbit);
		goto ignore;

	case HID_UP_LOGIVENDOR:
	case HID_UP_LOGIVENDOR2:
	case HID_UP_LOGIVENDOR3:
		goto ignore;

	case HID_UP_PID:
		if (id == 0xa4) {
			map_key_clear(BTN_DEAD);
			break;
		}
		goto ignore;

	default:
	unknown:
		/* Something we have no name for: a button, an LED, or a
		 * miscellaneous axis, by its shape. */
		if (field->report_size == 1) {
			if (field->report->type == HID_OUTPUT_REPORT) {
				map_led(LED_MISC);
				break;
			}
			map_key(BTN_MISC);
			break;
		}
		if (field->flags & HID_MAIN_ITEM_RELATIVE) {
			map_rel(REL_MISC);
			break;
		}
		map_abs(ABS_MISC);
		break;
	}

mapped:
	if (!bit)
		return; // nothing mapped (or the driver did it all)

	if (device->driver->input_mapped &&
	    device->driver->input_mapped(device, hidinput, field, usage, &bit,
					 &max) < 0)
		return; // the driver takes it from here

	input_set_bit(usage->type, input->evbit);

	/* Two usages mapped to the same code would mix their events: the
	 * second one is dropped, unless the device asks for it to move to
	 * the next free code. */
	while ((int)usage->code <= max && input_test_bit(usage->code, bit)) {
		if (device->quirks & HID_QUIRK_INCREMENT_USAGE_ON_DUPLICATE) {
			unsigned int c = usage->code;

			while ((int)c <= max && input_test_bit(c, bit))
				c++;
			usage->code = (uint16_t)c;
		} else {
			device->status |= HID_STAT_DUP_DETECTED;
			goto ignore;
		}
	}
	if ((int)usage->code > max)
		goto ignore;
	input_set_bit(usage->code, bit);

	if (usage->type == EV_ABS) {
		int a = field->logical_minimum;
		int b = field->logical_maximum;

		if ((device->quirks & HID_QUIRK_BADPAD) &&
		    (usage->code == ABS_X || usage->code == ABS_Y)) {
			a = field->logical_minimum = 0;
			b = field->logical_maximum = 255;
		}
		if (field->application == HID_GD_GAMEPAD ||
		    field->application == HID_GD_JOYSTICK)
			input_set_abs_params(input, usage->code, a, b,
					     (int)(((int64_t)b - a) >> 8),
					     (int)(((int64_t)b - a) >> 4));
		else
			input_set_abs_params(input, usage->code, a, b, 0, 0);
		input_abs_set_res(input, usage->code,
				  hidinput_calc_abs_res(field, usage->code));

		/* Multitouch frames are long. */
		if (usage->code == ABS_MT_POSITION_X &&
		    input->hint_events_per_packet == 0)
			input->hint_events_per_packet = 60;
	}

	if (usage->type == EV_ABS &&
	    (usage->hat_min < usage->hat_max || usage->hat_dir)) {
		for (int i = usage->code; i < usage->code + 2 && i <= max; i++) {
			input_set_abs_params(input, (unsigned int)i, -1, 1, 0,
					     0);
			input_set_bit((unsigned int)i, input->absbit);
		}
		if (usage->hat_dir && !field->dpad)
			field->dpad = usage->code;
	}

	/* A relative volume control is turned into key presses. */
	if (usage->type == EV_ABS && (field->flags & HID_MAIN_ITEM_RELATIVE) &&
	    usage->code == ABS_VOLUME) {
		input_set_bit(KEY_VOLUMEUP, input->keybit);
		input_set_bit(KEY_VOLUMEDOWN, input->keybit);
	}

	/* Keys come with their usage as scan code. */
	if (usage->type == EV_KEY) {
		input_set_bit(EV_MSC, input->evbit);
		input_set_bit(MSC_SCAN, input->mscbit);
	}
	return;

ignore:
	usage->type = 0;
	usage->code = 0;
}

#undef map_abs
#undef map_rel
#undef map_key
#undef map_led
#undef map_msc
#undef map_abs_clear
#undef map_key_clear

static void hidinput_configure_usages(struct hid_input *hidinput,
				      struct hid_report *report)
{
	for (unsigned int i = 0; i < report->maxfield; i++)
		for (unsigned int j = 0; j < report->field[i]->maxusage; j++)
			hidinput_configure_usage(hidinput, report->field[i],
						 report->field[i]->usage + j);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void hid_report_release_tool(struct hid_report *report,
				    struct input_dev *input, unsigned int tool)
{
	/* Only a tool that is down is released, with its touch. */
	if (!tool || tool > KEY_MAX || !input_test_bit(tool, input->key))
		return;
	input_event(input, EV_KEY, BTN_TOUCH, 0);
	input_event(input, EV_KEY, tool, 0);
	input_event(input, EV_SYN, SYN_REPORT, 0);
	report->tool = 0;
}

static void hid_report_set_tool(struct hid_report *report,
				struct input_dev *input, unsigned int new_tool)
{
	if (report->tool != new_tool)
		hid_report_release_tool(report, input, report->tool);
	input_event(input, EV_KEY, new_tool, 1);
	report->tool = new_tool;
}

/* A hi-resolution wheel: the value is in 1/multiplier notches.  REL_*_HI_RES
 * carries 1/120 notches, REL_WHEEL/HWHEEL whole ones (the remainder is
 * kept for the next movement). */
static void hidinput_handle_scroll(struct hid_usage *usage,
				   struct input_dev *input, int32_t value)
{
	int multiplier = usage->resolution_multiplier;
	int code, hi_res, lo_res;

	if (value == 0)
		return;
	if (multiplier == 0)
		multiplier = 1;
	code = usage->code == REL_WHEEL_HI_RES ? REL_WHEEL : REL_HWHEEL;

	hi_res = (int)((int64_t)value * 120 / multiplier);
	usage->wheel_accumulated += hi_res;
	lo_res = usage->wheel_accumulated / 120;
	if (lo_res)
		usage->wheel_accumulated -= lo_res * 120;

	input_event(input, EV_REL, (unsigned int)code, lo_res);
	input_event(input, EV_REL, usage->code, hi_res);
}

void hidinput_hid_event(struct hid_device *hid, struct hid_field *field,
			struct hid_usage *usage, int32_t value)
{
	struct input_dev *input;
	struct hid_report *report = field->report;
	unsigned long *quirks = &hid->quirks;

	if (!usage->type)
		return;
	if (!field->hidinput)
		return;
	input = field->hidinput->input;

	if (usage->hat_min < usage->hat_max || usage->hat_dir) {
		int64_t hat_dir = usage->hat_dir;

		if (!hat_dir)
			hat_dir = ((int64_t)value - usage->hat_min) * 8 /
					  ((int64_t)usage->hat_max -
					   usage->hat_min + 1) +
				  1;
		if (hat_dir < 0 || hat_dir > 8)
			hat_dir = 0;
		input_event(input, usage->type, usage->code,
			    hid_hat_to_axis[hat_dir].x);
		input_event(input, usage->type, usage->code + 1u,
			    hid_hat_to_axis[hat_dir].y);
		return;
	}

	/* Out-of-range values: ignored when the field has a null state
	 * (that is what they mean), clamped otherwise.  Only for fields that
	 * state a range at all. */
	if ((field->flags & HID_MAIN_ITEM_VARIABLE) &&
	    field->logical_minimum < field->logical_maximum) {
		if ((field->flags & HID_MAIN_ITEM_NULL_STATE) &&
		    (value < field->logical_minimum ||
		     value > field->logical_maximum))
			return;
		if (value < field->logical_minimum)
			value = field->logical_minimum;
		if (value > field->logical_maximum)
			value = field->logical_maximum;
	}

	switch (usage->hid) {
	case HID_DG_ERASER:
		report->tool_active |= !!value;
		/* A pen whose eraser touches is the rubber tool, whatever
		 * Invert said. */
		if (value)
			hid_report_set_tool(report, input, BTN_TOOL_RUBBER);
		else if (report->tool != BTN_TOOL_RUBBER)
			return;
		else if ((*quirks & HID_QUIRK_NOINVERT) &&
			 !input_test_bit(BTN_TOUCH, input->key)) {
			/* No Invert to take the tool away: release it here
			 * after hid-input sent BTN_TOUCH. */
			hid_report_release_tool(report, input, BTN_TOOL_RUBBER);
			return;
		}
		break;

	case HID_DG_INVERT:
		report->tool_active |= !!value;
		if (value)
			hid_report_set_tool(report, input, BTN_TOOL_RUBBER);
		else if (!report->tool_active)
			hid_report_release_tool(report, input, BTN_TOOL_RUBBER);
		return;

	case HID_DG_INRANGE:
		report->tool_active |= !!value;
		if (report->tool_active) {
			if (!report->tool)
				report->tool = usage->code;
			/* Send it again: a driver may have changed it. */
			hid_report_set_tool(report, input, report->tool);
		} else {
			hid_report_release_tool(report, input, usage->code);
		}
		report->tool_active = false;
		return;

	case HID_DG_TIPSWITCH:
		report->tool_active |= !!value;
		if (report->tool == BTN_TOOL_RUBBER)
			return;
		break;

	case HID_DG_TIPPRESSURE:
		if (*quirks & HID_QUIRK_NOTOUCH) {
			int64_t a = field->logical_minimum;
			int64_t b = field->logical_maximum;

			if (value > a + ((b - a) >> 3)) {
				input_event(input, EV_KEY, BTN_TOUCH, 1);
				report->tool_active = true;
			}
		}
		break;

	case HID_UP_PID | 0x83u: // Simultaneous Effects Max
	case HID_UP_PID | 0x7fu: // PID Pool Report
		return;
	}

	switch (usage->type) {
	case EV_KEY:
		if (usage->code == 0) // key 0 is "unassigned"
			return;
		break;
	case EV_REL:
		if (usage->code == REL_WHEEL_HI_RES ||
		    usage->code == REL_HWHEEL_HI_RES) {
			hidinput_handle_scroll(usage, input, value);
			return;
		}
		break;
	case EV_ABS:
		if ((field->flags & HID_MAIN_ITEM_RELATIVE) &&
		    usage->code == ABS_VOLUME) {
			int64_t count = value < 0 ? -(int64_t)value : value;
			unsigned int key = value > 0 ? KEY_VOLUMEUP :
						       KEY_VOLUMEDOWN;

			/* Bounded: a device cannot make us loop for ever. */
			if (count > 64)
				count = 64;
			for (int64_t i = 0; i < count; i++) {
				input_event(input, EV_KEY, key, 1);
				input_sync(input);
				input_event(input, EV_KEY, key, 0);
				input_sync(input);
			}
			return;
		}
		if (((*quirks & HID_QUIRK_X_INVERT) && usage->code == ABS_X) ||
		    ((*quirks & HID_QUIRK_Y_INVERT) && usage->code == ABS_Y))
			value = (int32_t)((int64_t)field->logical_maximum -
					  value);
		break;
	}

	/* An absolute value that did not change is not reported again --
	 * this also hides the constant zeros of the "dead" keys of national
	 * layouts that share a key code with a real one. */
	if (!(field->flags &
	      (HID_MAIN_ITEM_RELATIVE | HID_MAIN_ITEM_BUFFERED_BYTE)) &&
	    (field->flags & HID_MAIN_ITEM_VARIABLE) &&
	    usage->usage_index < field->maxusage &&
	    value == field->value[usage->usage_index])
		return;

	/* The usage as scan code, when the key changes state. */
	if (usage->type == EV_KEY &&
	    (!input_test_bit(usage->code, input->key)) == value)
		input_event(input, EV_MSC, MSC_SCAN, (int)usage->hid);

	input_event(input, usage->type, usage->code, value);

	/* A relative key has no release of its own. */
	if ((field->flags & HID_MAIN_ITEM_RELATIVE) && usage->type == EV_KEY &&
	    value) {
		input_sync(input);
		input_event(input, usage->type, usage->code, 0);
	}
}

void hidinput_report_event(struct hid_device *hid, struct hid_report *report)
{
	struct hid_input *hidinput;

	(void)report;
	if (hid->quirks & HID_QUIRK_NO_INPUT_SYNC)
		return; // the driver ends its packets itself
	list_for_each_entry(hidinput, &hid->inputs, list)
		input_sync(hidinput->input);
}

// ---------------------------------------------------------------------------
// LEDs: a client writes EV_LED to the node, the device gets an output
// report.  The input core calls us under its event spinlock, so the report
// goes out from a work item.
// ---------------------------------------------------------------------------

static int hidinput_find_field(struct hid_device *hid, unsigned int type,
			       unsigned int code, struct hid_field **field)
{
	struct hid_report *report;

	list_for_each_entry(report,
			    &hid->report_enum[HID_OUTPUT_REPORT].report_list,
			    list) {
		for (unsigned int i = 0; i < report->maxfield; i++) {
			struct hid_field *f = report->field[i];

			for (unsigned int j = 0; j < f->maxusage; j++)
				if (f->usage[j].type == type &&
				    f->usage[j].code == code) {
					*field = f;
					return (int)j;
				}
		}
	}
	return -1;
}

static struct hid_field *hidinput_get_led_field(struct hid_device *hid)
{
	struct hid_report *report;

	list_for_each_entry(report,
			    &hid->report_enum[HID_OUTPUT_REPORT].report_list,
			    list) {
		for (unsigned int i = 0; i < report->maxfield; i++) {
			struct hid_field *field = report->field[i];

			for (unsigned int j = 0; j < field->maxusage; j++)
				if (field->usage[j].type == EV_LED)
					return field;
		}
	}
	return NULL;
}

static void hidinput_led_worker(struct work_struct *work)
{
	struct hid_device *hid = container_of(work, struct hid_device, led_work);
	struct hid_field *field;
	struct hid_report *report;
	uint8_t *buf;
	uint32_t len;
	int ret;

	field = hidinput_get_led_field(hid);
	if (!field)
		return;
	report = field->report;

	/* The values may change while we pack them: a later LED event
	 * queues the worker again, which then sends the final state. */
	if (hid->ll_driver->request) {
		hid->ll_driver->request(hid, report, HID_REQ_SET_REPORT);
		return;
	}

	buf = hid_alloc_report_buf(report);
	if (!buf)
		return;
	/* Buffers to the transport start with the report ID byte. */
	len = hid_report_len(report);
	if (report->id == 0) {
		hid_output_report(report, buf + 1);
		len++;
	} else {
		hid_output_report(report, buf);
	}
	ret = hid_hw_output_report(hid, buf, len);
	if (ret == -ENOSYS)
		hid_hw_raw_request(hid, (unsigned char)report->id, buf, len,
				   HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	kfree(buf);
}

static int hidinput_input_event(struct input_dev *dev, unsigned int type,
				unsigned int code, int value)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct hid_field *field = NULL;
	int offset;

	if (type != EV_LED)
		return -1;
	offset = hidinput_find_field(hid, type, code, &field);
	if (offset == -1) {
		hid_warn(hid, "event field not found\n");
		return -1;
	}
	hid_set_field(field, (unsigned int)offset, value);
	schedule_work(&hid->led_work);
	return 0;
}

static int hidinput_open(struct input_dev *dev)
{
	return hid_hw_open(input_get_drvdata(dev));
}

static void hidinput_close(struct input_dev *dev)
{
	hid_hw_close(input_get_drvdata(dev));
}

// ---------------------------------------------------------------------------
// Resolution multipliers: ask the device for its finest wheel resolution.
// ---------------------------------------------------------------------------

static bool __hidinput_change_resolution_multipliers(struct hid_device *hid,
						     struct hid_report *report,
						     bool use_logical_max)
{
	bool update_needed = false;
	bool get_report_completed = false;

	if (report->maxfield == 0)
		return false;

	for (unsigned int i = 0; i < report->maxfield; i++) {
		struct hid_field *field = report->field[i];
		int32_t value = use_logical_max ? field->logical_maximum :
						  field->logical_minimum;

		/* A multiplier with more than one value makes no sense. */
		if (field->report_count != 1)
			continue;

		for (unsigned int j = 0; j < field->maxusage; j++) {
			if (field->usage[j].hid != HID_GD_RESOLUTION_MULTIPLIER)
				continue;
			/* Other features in the same report must keep their
			 * values: read them first (unless the device must not
			 * be asked). */
			if (!get_report_completed && report->maxfield > 1) {
				if (hid->quirks & HID_QUIRK_NO_INIT_REPORTS)
					return update_needed;
				hid_hw_request(hid, report, HID_REQ_GET_REPORT);
				hid_hw_wait(hid);
				get_report_completed = true;
			}
			field->value[j] = value;
			update_needed = true;
		}
	}
	return update_needed;
}

static void hidinput_change_resolution_multipliers(struct hid_device *hid)
{
	struct hid_report *rep;

	list_for_each_entry(rep,
			    &hid->report_enum[HID_FEATURE_REPORT].report_list,
			    list) {
		if (__hidinput_change_resolution_multipliers(hid, rep, true) &&
		    __hid_request(hid, rep, HID_REQ_SET_REPORT)) {
			/* The device refused: our values go back to what it
			 * has, and the multipliers follow them. */
			__hidinput_change_resolution_multipliers(hid, rep,
								 false);
			return;
		}
	}
	hid_setup_resolution_multiplier(hid);
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------

/* Every usage of every feature report goes to the driver's
 * feature_mapping hook (contact count maximum, button type, ...). */
static void report_features(struct hid_device *hid)
{
	struct hid_driver *drv = hid->driver;
	struct hid_report *rep;

	if (!drv->feature_mapping)
		return;
	list_for_each_entry(rep,
			    &hid->report_enum[HID_FEATURE_REPORT].report_list,
			    list)
		for (unsigned int i = 0; i < rep->maxfield; i++) {
			struct hid_field *field = rep->field[i];

			if (field->report_count < 1)
				continue;
			for (unsigned int j = 0; j < field->maxusage; j++)
				drv->feature_mapping(hid, field,
						     &field->usage[j]);
		}
}

static struct hid_input *hidinput_allocate(struct hid_device *hid,
					   unsigned int application)
{
	struct hid_input *hidinput = kcalloc(1, sizeof(*hidinput));
	struct input_dev *input_dev = input_allocate_device();
	const char *suffix = NULL;

	if (!hidinput || !input_dev)
		goto fail;
	hidinput->input = input_dev;

	if ((hid->quirks & HID_QUIRK_INPUT_PER_APP) &&
	    hid->maxapplication > 1) {
		switch (application) {
		case HID_GD_KEYBOARD:
			suffix = "Keyboard";
			break;
		case HID_GD_KEYPAD:
			suffix = "Keypad";
			break;
		case HID_GD_MOUSE:
			suffix = "Mouse";
			break;
		case HID_DG_PEN:
			/* Pen collections are "Stylus" and stylus ones
			 * "Pen": names users already match on. */
			suffix = "Stylus";
			break;
		case HID_DG_STYLUS:
			suffix = "Pen";
			break;
		case HID_DG_TOUCHSCREEN:
			suffix = "Touchscreen";
			break;
		case HID_DG_TOUCHPAD:
			suffix = "Touchpad";
			break;
		case HID_GD_SYSTEM_CONTROL:
			suffix = "System Control";
			break;
		case HID_CP_CONSUMER_CONTROL:
			suffix = "Consumer Control";
			break;
		case HID_GD_WIRELESS_RADIO_CTLS:
			suffix = "Wireless Radio Control";
			break;
		case HID_GD_SYSTEM_MULTIAXIS:
			suffix = "System Multi Axis";
			break;
		default:
			break;
		}
	}

	input_dev->name = hid->name;
	if (suffix) {
		size_t name_len = kstrlen(hid->name);
		size_t suffix_len = kstrlen(suffix);

		/* No suffix when the name already ends with it. */
		if (name_len < suffix_len ||
		    kstrcmp(hid->name + name_len - suffix_len, suffix))
			if (hidinput_set_name(hidinput, hid->name, suffix))
				goto fail;
	}

	input_set_drvdata(input_dev, hid);
	input_dev->event = hidinput_input_event;
	input_dev->open = hidinput_open;
	input_dev->close = hidinput_close;
	input_dev->phys = hid->phys;
	input_dev->uniq = hid->uniq;
	input_dev->devpath = hid->devpath[0] ? hid->devpath : NULL;
	input_dev->id.bustype = hid->bus;
	input_dev->id.vendor = (uint16_t)hid->vendor;
	input_dev->id.product = (uint16_t)hid->product;
	input_dev->id.version = (uint16_t)hid->version;

	hidinput->application = application;
	INIT_LIST_HEAD(&hidinput->reports);
	list_add_tail(&hidinput->list, &hid->inputs);
	return hidinput;

fail:
	if (input_dev)
		input_free_device(input_dev);
	if (hidinput)
		kfree(hidinput->name);
	kfree(hidinput);
	hid_err(hid, "Out of memory during hid input probe\n");
	return NULL;
}

static bool hidinput_has_been_populated(struct hid_input *hidinput)
{
	struct input_dev *in = hidinput->input;
	unsigned long r = 0;
	unsigned int i;

#define OR_MAP(map)                                                     \
	for (i = 0; i < HID_ARRAY_SIZE(in->map); i++)                   \
		r |= in->map[i]
	OR_MAP(evbit);
	OR_MAP(keybit);
	OR_MAP(relbit);
	OR_MAP(absbit);
	OR_MAP(mscbit);
	OR_MAP(ledbit);
	OR_MAP(sndbit);
	OR_MAP(ffbit);
	OR_MAP(swbit);
	OR_MAP(propbit);
#undef OR_MAP
	return r != 0;
}

static void hidinput_cleanup_hidinput(struct hid_device *hid,
				      struct hid_input *hidinput)
{
	struct hid_report *report;

	struct hid_report *tmp;

	list_del(&hidinput->list);
	input_free_device(hidinput->input);
	kfree(hidinput->name);
	list_for_each_entry_safe(report, tmp, &hidinput->reports, hidinput_list)
		list_del_init(&report->hidinput_list);

	/* Its reports no longer feed an input device. */
	for (unsigned int k = HID_INPUT_REPORT; k <= HID_OUTPUT_REPORT; k++)
		list_for_each_entry(report, &hid->report_enum[k].report_list,
				    list)
			for (unsigned int i = 0; i < report->maxfield; i++)
				if (report->field[i]->hidinput == hidinput)
					report->field[i]->hidinput = NULL;
	kfree(hidinput);
}

/* The input device a report goes into under HID_QUIRK_MULTI_INPUT. */
static struct hid_input *hidinput_match(struct hid_report *report)
{
	struct hid_device *hid = report->device;
	struct hid_input *hidinput;

	list_for_each_entry(hidinput, &hid->inputs, list)
		if (hidinput->report && hidinput->report->id == report->id)
			return hidinput;
	return NULL;
}

/* ... and under HID_QUIRK_INPUT_PER_APP. */
static struct hid_input *hidinput_match_application(struct hid_report *report)
{
	struct hid_device *hid = report->device;
	struct hid_input *hidinput;

	list_for_each_entry(hidinput, &hid->inputs, list) {
		if (hidinput->application == report->application)
			return hidinput;
		/* A keyboard's system and consumer keys stay with it. */
		if ((report->application == HID_GD_SYSTEM_CONTROL ||
		     report->application == HID_CP_CONSUMER_CONTROL) &&
		    hidinput->application == HID_GD_KEYBOARD)
			return hidinput;
	}
	return NULL;
}

int hidinput_connect(struct hid_device *hid, unsigned int force)
{
	struct hid_driver *drv = hid->driver;
	struct hid_report *report;
	struct hid_input *hidinput = NULL, *next;
	unsigned int i, k;

	INIT_LIST_HEAD(&hid->inputs);
	INIT_WORK(&hid->led_work, hidinput_led_worker);
	hid->status &= ~HID_STAT_DUP_DETECTED;

	/* Without an input application (and without being forced) the
	 * device is none of ours. */
	if (!force) {
		for (i = 0; i < hid->maxcollection; i++) {
			struct hid_collection *col = &hid->collection[i];

			if ((col->type == HID_COLLECTION_APPLICATION ||
			     col->type == HID_COLLECTION_PHYSICAL) &&
			    HID_IS_INPUT_APPLICATION(col->usage))
				break;
		}
		if (i == hid->maxcollection)
			return -1;
	}

	report_features(hid);

	for (k = HID_INPUT_REPORT; k <= HID_OUTPUT_REPORT; k++) {
		if (k == HID_OUTPUT_REPORT &&
		    (hid->quirks & HID_QUIRK_SKIP_OUTPUT_REPORTS))
			continue;

		list_for_each_entry(report, &hid->report_enum[k].report_list,
				    list) {
			if (!report->maxfield)
				continue;

			if (hid->quirks & HID_QUIRK_MULTI_INPUT)
				hidinput = hidinput_match(report);
			else if (hid->maxapplication > 1 &&
				 (hid->quirks & HID_QUIRK_INPUT_PER_APP))
				hidinput = hidinput_match_application(report);

			if (!hidinput) {
				hidinput = hidinput_allocate(hid,
							     report->application);
				if (!hidinput)
					goto out_unwind;
			}

			hidinput_configure_usages(hidinput, report);

			if (hid->quirks & HID_QUIRK_MULTI_INPUT)
				hidinput->report = report;
			list_add_tail(&report->hidinput_list,
				      &hidinput->reports);
		}
	}

	hidinput_change_resolution_multipliers(hid);

	list_for_each_entry_safe(hidinput, next, &hid->inputs, list) {
		if (drv->input_configured &&
		    drv->input_configured(hid, hidinput))
			goto out_unwind;

		if (!hidinput_has_been_populated(hidinput)) {
			/* Nothing mapped: no device. */
			hidinput_cleanup_hidinput(hid, hidinput);
			continue;
		}

		if (input_register_device(hidinput->input))
			goto out_unwind;
		hidinput->registered = true;
	}

	if (list_empty(&hid->inputs)) {
		hid_err(hid, "No inputs registered, leaving\n");
		goto out_unwind;
	}

	if (hid->status & HID_STAT_DUP_DETECTED)
		hid_dbg(hid, "some usages could not be mapped (duplicates)\n");
	return 0;

out_unwind:
	hidinput_disconnect(hid);
	return -1;
}

void hidinput_disconnect(struct hid_device *hid)
{
	struct hid_input *hidinput, *next;
	struct hid_report *report;

	list_for_each_entry_safe(hidinput, next, &hid->inputs, list) {
		list_del(&hidinput->list);
		if (hidinput->registered)
			input_unregister_device(hidinput->input);
		else
			input_free_device(hidinput->input);
		kfree(hidinput->name);
		kfree(hidinput);
	}

	/* The reports' links into the input devices are gone with them. */
	for (unsigned int k = 0; k < HID_REPORT_TYPES; k++)
		list_for_each_entry(report, &hid->report_enum[k].report_list,
				    list) {
			INIT_LIST_HEAD(&report->hidinput_list);
			for (unsigned int i = 0; i < report->maxfield; i++)
				report->field[i]->hidinput = NULL;
		}

	/* The LED worker is only queued from input-device callbacks, and
	 * those are all gone: wait for one that may still run. */
	cancel_work_sync(&hid->led_work);
}
