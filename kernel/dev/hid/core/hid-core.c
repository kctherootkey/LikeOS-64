// LikeOS HID core - descriptor parser, report processing, driver binding
//
// The report descriptor is a program for a small stack machine.  Global
// items (Usage Page, Logical/Physical Minimum/Maximum, Unit, Unit Exponent,
// Report Size, Report Count, Report ID, Push, Pop) set state that holds
// until it is changed; local items (Usage, Usage Minimum/Maximum, Delimiter)
// collect the usages of the next main item and are forgotten after it; main
// items (Input, Output, Feature, Collection, End Collection) consume that
// state.  Every Input/Output/Feature item becomes one struct hid_field in
// the report of its type and ID; its values sit report_offset bits into the
// report, report_count of them, report_size bits each, little-endian bit
// order.
//
// The parser trusts nothing: every size, count, ID, nesting level and
// range is bounded before it is used (see the limits in hid.h), a
// truncated item or an unbalanced collection rejects the descriptor, and
// the memory a descriptor can make it allocate is capped.
//
// Incoming reports are dispatched by their ID byte to the parsed report,
// checked against its length (a short report is zero-padded), and every
// field's values are extracted: variable fields report every value, array
// fields report the usages that appeared (value 1) and disappeared (0)
// since the previous report.  The bound driver sees the raw bytes
// (raw_event), every value (event), and the end of the report (report);
// hid-input turns the values into input events.
//
// Copyright (C) 2026 The LikeOS Project

#include <kernel/dev/hid/core/hid.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h> /* errno values */
#include <kernel/ke/timer.h>

// ---------------------------------------------------------------------------
// Bus state
// ---------------------------------------------------------------------------
static DEFINE_MUTEX(hid_bus_lock);
static struct list_head hid_drivers = LIST_HEAD_INIT(hid_drivers);
static struct list_head hid_devices = LIST_HEAD_INIT(hid_devices);
static unsigned int hid_next_id;
/* Bind every device to the generic driver (a debugging aid, off). */
static const bool hid_ignore_special_drivers = false;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
void hid_printk(const struct hid_device *hdev, const char *level,
		const char *fmt, ...)
{
	char msg[256];
	va_list ap;
	const char *drv = "hid";
	const char *who = "?";

	if (hdev) {
		if (hdev->driver && hdev->driver->name)
			drv = hdev->driver->name;
		if (hdev->dev_name[0])
			who = hdev->dev_name;
		else if (hdev->name[0])
			who = hdev->name;
	}
	__builtin_va_start(ap, fmt);
	kvsnprintf(msg, sizeof(msg), fmt, ap);
	__builtin_va_end(ap);
	kprintf("%s %s: %s%s", drv, who, level, msg);
}

/* At most 10 messages in 5 seconds per device (short reports). */
static bool hid_short_report_ratelimit(struct hid_device *hdev)
{
	uint64_t now = timer_get_precise_ns();

	if (!hdev->short_warn_ns ||
	    now - hdev->short_warn_ns > 5000000000ull) {
		hdev->short_warn_ns = now ? now : 1;
		hdev->short_warn_burst = 0;
	}
	return hdev->short_warn_burst++ < 10;
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

/* Decode the item at `start'; the position after it, or NULL when the
 * descriptor ends inside the item. */
static const uint8_t *hid_fetch_item(const uint8_t *start, const uint8_t *end,
				     struct hid_item *item)
{
	uint8_t b;

	if (start >= end)
		return NULL;
	b = *start++;
	item->type = (b >> 2) & 3;
	item->tag = (b >> 4) & 15;

	if (item->tag == HID_ITEM_TAG_LONG) {
		item->format = HID_ITEM_FORMAT_LONG;
		if (end - start < 2)
			return NULL;
		item->size = *start++;
		item->tag = *start++;
		if (end - start < item->size)
			return NULL;
		item->data.longdata = start;
		return start + item->size;
	}

	item->format = HID_ITEM_FORMAT_SHORT;
	item->size = b & 3;
	switch (item->size) {
	case 0:
		return start;
	case 1:
		if (end - start < 1)
			return NULL;
		item->data.u8 = start[0];
		return start + 1;
	case 2:
		if (end - start < 2)
			return NULL;
		item->data.u16 = (uint16_t)(start[0] | (start[1] << 8));
		return start + 2;
	default: /* size code 3 means four bytes */
		item->size = 4;
		if (end - start < 4)
			return NULL;
		item->data.u32 = (uint32_t)start[0] | ((uint32_t)start[1] << 8) |
				 ((uint32_t)start[2] << 16) |
				 ((uint32_t)start[3] << 24);
		return start + 4;
	}
}

static uint32_t item_udata(const struct hid_item *item)
{
	if (item->format != HID_ITEM_FORMAT_SHORT)
		return 0;
	switch (item->size) {
	case 1:
		return item->data.u8;
	case 2:
		return item->data.u16;
	case 4:
		return item->data.u32;
	}
	return 0;
}

static int32_t item_sdata(const struct hid_item *item)
{
	if (item->format != HID_ITEM_FORMAT_SHORT)
		return 0;
	switch (item->size) {
	case 1:
		return (int32_t)item->data.u8 - ((item->data.u8 & 0x80) << 1);
	case 2:
		return (int32_t)item->data.u16 -
		       ((int32_t)(item->data.u16 & 0x8000) << 1);
	case 4:
		return (int32_t)item->data.u32;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------------
struct hid_global {
	unsigned int usage_page;
	int32_t logical_minimum;
	int32_t logical_maximum;
	int32_t physical_minimum;
	int32_t physical_maximum;
	int32_t unit_exponent;
	unsigned int unit;
	unsigned int report_id;
	unsigned int report_size;
	unsigned int report_count;
};

struct hid_local {
	unsigned int usage[HID_MAX_USAGES];
	uint8_t usage_size[HID_MAX_USAGES]; // item size the usage came with
	unsigned int collection_index[HID_MAX_USAGES];
	unsigned int usage_index; // usages collected
	unsigned int usage_minimum;
	unsigned int delimiter_depth;
	unsigned int delimiter_branch;
};

#define HID_SCAN_FLAG_MT_WIN_8 (1u << 0)
#define HID_SCAN_FLAG_VENDOR_SPECIFIC (1u << 1)
#define HID_SCAN_FLAG_GD_POINTER (1u << 2)

struct hid_parser {
	struct hid_global global;
	struct hid_global global_stack[HID_GLOBAL_STACK_SIZE];
	unsigned int global_stack_ptr;
	struct hid_local local;
	unsigned int collection_stack[HID_MAX_COLLECTION_DEPTH];
	unsigned int collection_stack_ptr;
	struct hid_device *device;
	unsigned int scan_flags;
	bool scanning; // the group scan: quiet, nothing allocated
};

#define parser_err(p, fmt, ...)                                        \
	do {                                                           \
		if (!(p)->scanning)                                    \
			hid_err((p)->device, fmt, ##__VA_ARGS__);      \
	} while (0)
#define parser_warn(p, fmt, ...)                                       \
	do {                                                           \
		if (!(p)->scanning)                                    \
			hid_warn((p)->device, fmt, ##__VA_ARGS__);     \
	} while (0)

/* Forget the local items: after every main item.  The arrays need no
 * clearing: nothing reads past usage_index. */
static void hid_reset_local(struct hid_parser *parser)
{
	parser->local.usage_index = 0;
	parser->local.usage_minimum = 0;
	parser->local.delimiter_depth = 0;
	parser->local.delimiter_branch = 0;
}

/* The usage of the innermost enclosing collection of `type', 0 if none. */
static unsigned int hid_lookup_collection(struct hid_parser *parser,
					  unsigned int type)
{
	struct hid_collection *collection = parser->device->collection;
	int n;

	for (n = (int)parser->collection_stack_ptr - 1; n >= 0; n--) {
		unsigned int index = parser->collection_stack[n];

		if (collection[index].type == type)
			return collection[index].usage;
	}
	return 0;
}

/* A usage given with a 1- or 2-byte item is an id within the current usage
 * page; a 4-byte item gives the whole usage. */
static void hid_complete_usage(struct hid_parser *parser, unsigned int index)
{
	parser->local.usage[index] &= 0xffff;
	parser->local.usage[index] |= (parser->global.usage_page & 0xffff)
				      << 16;
}

/* A Usage Page item that comes after the usages of a main item still
 * applies to them: re-complete the short usages that were completed with
 * an older page, back to the first one that already has the current one. */
static void hid_concatenate_last_usage_page(struct hid_parser *parser)
{
	unsigned int page = parser->global.usage_page;
	int i;

	for (i = (int)parser->local.usage_index - 1; i >= 0; i--) {
		if (parser->local.usage_size[i] > 2)
			continue; // extended usage: complete on its own
		if ((parser->local.usage[i] >> 16) == page)
			break;
		hid_complete_usage(parser, (unsigned int)i);
	}
}

static int hid_add_usage(struct hid_parser *parser, unsigned int usage,
			 uint8_t size)
{
	unsigned int idx = parser->local.usage_index;

	if (idx >= HID_MAX_USAGES) {
		parser_err(parser, "usage index exceeded\n");
		return -1;
	}
	parser->local.usage[idx] = usage;
	if (size <= 2)
		hid_complete_usage(parser, idx);
	parser->local.usage_size[idx] = size;
	parser->local.collection_index[idx] =
		parser->collection_stack_ptr ?
			parser->collection_stack[parser->collection_stack_ptr -
						 1] :
			0;
	parser->local.usage_index++;
	return 0;
}

// ---------------------------------------------------------------------------
// Reports and fields
// ---------------------------------------------------------------------------

static struct hid_report *hid_register_report(struct hid_device *device,
					      unsigned int type,
					      unsigned int id,
					      unsigned int application)
{
	struct hid_report_enum *report_enum = device->report_enum + type;
	struct hid_report *report;

	if (id >= HID_MAX_IDS)
		return NULL;
	if (report_enum->report_id_hash[id])
		return report_enum->report_id_hash[id];

	report = kcalloc(1, sizeof(*report));
	if (!report)
		return NULL;
	if (id != 0)
		report_enum->numbered = 1;
	report->id = id;
	report->type = type;
	report->size = 0;
	report->device = device;
	report->application = application;
	INIT_LIST_HEAD(&report->hidinput_list);
	report_enum->report_id_hash[id] = report;
	list_add_tail(&report->list, &report_enum->report_list);
	return report;
}

/* A field with room for `usages' usages and values, in one allocation. */
static struct hid_field *hid_register_field(struct hid_parser *parser,
					    struct hid_report *report,
					    unsigned int usages)
{
	struct hid_device *device = parser->device;
	struct hid_field *field;
	size_t bytes;

	if (report->maxfield == HID_MAX_FIELDS) {
		parser_err(parser, "too many fields in report\n");
		return NULL;
	}
	if (usages > HID_MAX_TOTAL_USAGES - device->total_usages) {
		parser_err(parser, "descriptor declares too many usages\n");
		return NULL;
	}

	bytes = sizeof(*field) + usages * sizeof(struct hid_usage) +
		2 * usages * sizeof(int32_t);
	field = kcalloc(1, bytes);
	if (!field)
		return NULL;
	device->total_usages += usages;
	field->index = report->maxfield++;
	report->field[field->index] = field;
	field->usage = (struct hid_usage *)(field + 1);
	field->value = (int32_t *)(field->usage + usages);
	field->new_value = field->value + usages;
	field->report = report;
	return field;
}

/* An Input, Output or Feature item. */
static int hid_add_field(struct hid_parser *parser, unsigned int report_type,
			 unsigned int flags)
{
	struct hid_device *device = parser->device;
	struct hid_global *g = &parser->global;
	struct hid_report *report;
	struct hid_field *field;
	unsigned int application, usages, offset, i;
	unsigned int max_buffer_size = HID_MAX_BUFFER_SIZE;

	application = hid_lookup_collection(parser, HID_COLLECTION_APPLICATION);

	report = hid_register_report(device, report_type, g->report_id,
				     application);
	if (!report) {
		parser_err(parser, "hid_register_report failed\n");
		return -1;
	}

	/* The logical range must not be inverted (signed comparison when
	 * the minimum is negative, unsigned otherwise). */
	if ((g->logical_minimum < 0 &&
	     g->logical_maximum < g->logical_minimum) ||
	    (g->logical_minimum >= 0 &&
	     (uint32_t)g->logical_maximum < (uint32_t)g->logical_minimum)) {
		parser_err(parser, "logical range invalid 0x%x 0x%x\n",
			   (unsigned int)g->logical_minimum,
			   (unsigned int)g->logical_maximum);
		return -1;
	}

	/* report_size <= 256 and report_count <= 12288 (checked when they
	 * were set), and report->size never passes the buffer limit, so no
	 * sum here can overflow. */
	offset = report->size;
	report->size += g->report_size * g->report_count;

	if (device->ll_driver && device->ll_driver->max_buffer_size)
		max_buffer_size = device->ll_driver->max_buffer_size;
	/* Room for the report ID byte. */
	if (report->size > (max_buffer_size - 1) << 3) {
		parser_err(parser, "report is too long\n");
		return -1;
	}

	/* A main item without usages is padding: it takes up space and has
	 * no field. */
	if (!parser->local.usage_index)
		return 0;

	usages = parser->local.usage_index > g->report_count ?
			 parser->local.usage_index :
			 g->report_count;

	field = hid_register_field(parser, report, usages);
	if (!field)
		return 0; // too many fields: ignored, as padding would be

	field->physical = hid_lookup_collection(parser, HID_COLLECTION_PHYSICAL);
	field->logical = hid_lookup_collection(parser, HID_COLLECTION_LOGICAL);
	field->application = application;

	for (i = 0; i < usages; i++) {
		unsigned int j = i;

		/* More values than usages: the last usage repeats. */
		if (i >= parser->local.usage_index)
			j = parser->local.usage_index - 1;
		field->usage[i].hid = parser->local.usage[j];
		field->usage[i].collection_index =
			parser->local.collection_index[j];
		field->usage[i].usage_index = i;
		field->usage[i].resolution_multiplier = 1;
	}

	field->maxusage = usages;
	field->flags = flags;
	field->report_offset = offset;
	field->report_type = report_type;
	field->report_size = g->report_size;
	field->report_count = g->report_count;
	field->logical_minimum = g->logical_minimum;
	field->logical_maximum = g->logical_maximum;
	field->physical_minimum = g->physical_minimum;
	field->physical_maximum = g->physical_maximum;
	field->unit_exponent = g->unit_exponent;
	field->unit = g->unit;
	return 0;
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

static int hid_open_collection(struct hid_parser *parser, unsigned int type)
{
	struct hid_device *device = parser->device;
	struct hid_collection *collection;
	unsigned int usage, index;

	usage = parser->local.usage_index ? parser->local.usage[0] : 0;

	if (parser->collection_stack_ptr == HID_MAX_COLLECTION_DEPTH) {
		parser_err(parser, "collection stack overflow\n");
		return -EINVAL;
	}

	if (device->maxcollection == device->collection_size) {
		unsigned int n = device->collection_size * 2;
		struct hid_collection *grown;

		if (!n)
			n = HID_DEFAULT_NUM_COLLECTIONS;
		grown = kcalloc(n, sizeof(*grown));
		if (!grown) {
			parser_err(parser,
				   "failed to reallocate collection array\n");
			return -ENOMEM;
		}
		if (device->collection)
			kmemcpy(grown, device->collection,
				device->collection_size * sizeof(*grown));
		for (unsigned int i = device->collection_size; i < n; i++)
			grown[i].parent_idx = -1;
		kfree(device->collection);
		device->collection = grown;
		device->collection_size = n;
	}

	index = device->maxcollection++;
	parser->collection_stack[parser->collection_stack_ptr++] = index;
	collection = device->collection + index;
	collection->type = type;
	collection->usage = usage;
	collection->level = parser->collection_stack_ptr - 1;
	collection->parent_idx =
		collection->level == 0 ?
			-1 :
			(int)parser->collection_stack[collection->level - 1];
	if (type == HID_COLLECTION_APPLICATION)
		device->maxapplication++;
	return 0;
}

static int hid_close_collection(struct hid_parser *parser)
{
	if (!parser->collection_stack_ptr) {
		parser_err(parser, "collection stack underflow\n");
		return -EINVAL;
	}
	parser->collection_stack_ptr--;
	return 0;
}

// ---------------------------------------------------------------------------
// Item handlers
// ---------------------------------------------------------------------------

static int hid_parser_global(struct hid_parser *parser, struct hid_item *item)
{
	struct hid_global *g = &parser->global;
	uint32_t raw;

	switch (item->tag) {
	case HID_GLOBAL_ITEM_TAG_PUSH:
		if (parser->global_stack_ptr == HID_GLOBAL_STACK_SIZE) {
			parser_err(parser, "global environment stack overflow\n");
			return -1;
		}
		parser->global_stack[parser->global_stack_ptr++] = *g;
		return 0;

	case HID_GLOBAL_ITEM_TAG_POP:
		if (!parser->global_stack_ptr) {
			parser_err(parser,
				   "global environment stack underflow\n");
			return -1;
		}
		*g = parser->global_stack[--parser->global_stack_ptr];
		return 0;

	case HID_GLOBAL_ITEM_TAG_USAGE_PAGE:
		g->usage_page = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_LOGICAL_MINIMUM:
		g->logical_minimum = item_sdata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_LOGICAL_MAXIMUM:
		/* Signed only when the minimum is: "0 .. 255" in one byte is
		 * 0x00 .. 0xff, not 0 .. -1. */
		if (g->logical_minimum < 0)
			g->logical_maximum = item_sdata(item);
		else
			g->logical_maximum = (int32_t)item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_PHYSICAL_MINIMUM:
		g->physical_minimum = item_sdata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_PHYSICAL_MAXIMUM:
		if (g->physical_minimum < 0)
			g->physical_maximum = item_sdata(item);
		else
			g->physical_maximum = (int32_t)item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_UNIT_EXPONENT:
		/* The exponent is a signed 4-bit value in the low nibble
		 * (0x0e is -2); a value that does not fit a nibble is taken
		 * as it is, for descriptors that wrote a full integer. */
		raw = item_udata(item);
		if (!(raw & 0xfffffff0u))
			g->unit_exponent = hid_snto32(raw, 4);
		else
			g->unit_exponent = (int32_t)raw;
		return 0;

	case HID_GLOBAL_ITEM_TAG_UNIT:
		g->unit = item_udata(item);
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_SIZE:
		g->report_size = item_udata(item);
		if (g->report_size > HID_MAX_REPORT_SIZE) {
			parser_err(parser, "invalid report_size %u\n",
				   g->report_size);
			return -1;
		}
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_COUNT:
		g->report_count = item_udata(item);
		if (g->report_count > HID_MAX_USAGES) {
			parser_err(parser, "invalid report_count %u\n",
				   g->report_count);
			return -1;
		}
		return 0;

	case HID_GLOBAL_ITEM_TAG_REPORT_ID:
		g->report_id = item_udata(item);
		if (g->report_id == 0 || g->report_id >= HID_MAX_IDS) {
			parser_err(parser, "report_id %u is invalid\n",
				   g->report_id);
			return -1;
		}
		return 0;

	default:
		parser_err(parser, "unknown global tag 0x%x\n", item->tag);
		return -1;
	}
}

static int hid_parser_local(struct hid_parser *parser, struct hid_item *item)
{
	struct hid_local *l = &parser->local;
	uint32_t data, count;
	uint64_t n;

	data = item_udata(item);

	switch (item->tag) {
	case HID_LOCAL_ITEM_TAG_DELIMITER:
		if (data) {
			/* Usages before the first delimiter belong to every
			 * set; of the sets only the first is used. */
			if (l->delimiter_depth != 0) {
				parser_err(parser, "nested delimiters\n");
				return -1;
			}
			l->delimiter_depth++;
			l->delimiter_branch++;
		} else {
			if (l->delimiter_depth < 1) {
				parser_err(parser, "bogus close delimiter\n");
				return -1;
			}
			l->delimiter_depth--;
		}
		return 0;

	case HID_LOCAL_ITEM_TAG_USAGE:
		if (l->delimiter_branch > 1)
			return 0; // an alternative usage
		return hid_add_usage(parser, data, item->size);

	case HID_LOCAL_ITEM_TAG_USAGE_MINIMUM:
		if (l->delimiter_branch > 1)
			return 0;
		l->usage_minimum = data;
		return 0;

	case HID_LOCAL_ITEM_TAG_USAGE_MAXIMUM:
		if (l->delimiter_branch > 1)
			return 0;
		count = data - l->usage_minimum;
		if ((uint64_t)count + l->usage_index >= HID_MAX_USAGES) {
			parser_warn(parser, "ignoring exceeding usage max\n");
			data = HID_MAX_USAGES - l->usage_index +
			       l->usage_minimum - 1;
			if (data == 0) {
				parser_err(parser,
					   "no more usage index available\n");
				return -1;
			}
		}
		for (n = l->usage_minimum; n <= data; n++)
			if (hid_add_usage(parser, (unsigned int)n, item->size))
				return -1;
		return 0;

	default:
		/* Designators and strings: not used. */
		return 0;
	}
}

static int hid_parser_main(struct hid_parser *parser, struct hid_item *item)
{
	uint32_t data;
	int ret;

	hid_concatenate_last_usage_page(parser);
	data = item_udata(item);

	switch (item->tag) {
	case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
		ret = hid_open_collection(parser, data & 0xff);
		break;
	case HID_MAIN_ITEM_TAG_END_COLLECTION:
		ret = hid_close_collection(parser);
		break;
	case HID_MAIN_ITEM_TAG_INPUT:
		ret = hid_add_field(parser, HID_INPUT_REPORT, data);
		break;
	case HID_MAIN_ITEM_TAG_OUTPUT:
		ret = hid_add_field(parser, HID_OUTPUT_REPORT, data);
		break;
	case HID_MAIN_ITEM_TAG_FEATURE:
		ret = hid_add_field(parser, HID_FEATURE_REPORT, data);
		break;
	default:
		if (item->tag >= HID_MAIN_ITEM_TAG_RESERVED_MIN &&
		    item->tag <= HID_MAIN_ITEM_TAG_RESERVED_MAX)
			parser_warn(parser, "reserved main item tag 0x%x\n",
				    item->tag);
		else
			parser_warn(parser, "unknown main item tag 0x%x\n",
				    item->tag);
		ret = 0;
	}

	hid_reset_local(parser);
	return ret;
}

static int hid_parser_reserved(struct hid_parser *parser,
			       struct hid_item *item)
{
	(void)parser;
	(void)item;
	return 0;
}

// ---------------------------------------------------------------------------
// The group scan: before a driver is chosen, a quiet pass over the
// descriptor finds what kind of device it is.
// ---------------------------------------------------------------------------

static void hid_scan_input_usage(struct hid_parser *parser, uint32_t usage)
{
	if (usage == HID_DG_CONTACTID)
		parser->device->group = HID_GROUP_MULTITOUCH;
}

static void hid_scan_feature_usage(struct hid_parser *parser, uint32_t usage)
{
	/* The certification blob of a "Windows 8" touch device. */
	if (usage == HID_VD_WIN8_CERT_BLOB &&
	    parser->global.report_count == 256 &&
	    parser->global.report_size == 8)
		parser->scan_flags |= HID_SCAN_FLAG_MT_WIN_8;
	if (usage == HID_VD_WIN8_CERT_BYTE &&
	    parser->global.report_count == 1 &&
	    parser->global.report_size == 8)
		parser->scan_flags |= HID_SCAN_FLAG_MT_WIN_8;
}

static void hid_scan_collection(struct hid_parser *parser, unsigned int type)
{
	struct hid_device *hid = parser->device;
	unsigned int page = parser->global.usage_page << 16;
	unsigned int i;

	if (page == HID_UP_SENSOR && (type == HID_COLLECTION_PHYSICAL ||
				      type == HID_COLLECTION_APPLICATION))
		hid->group = HID_GROUP_SENSOR_HUB;

	if (hid->vendor == USB_VENDOR_ID_MICROSOFT &&
	    hid->product == USB_DEVICE_ID_MS_POWER_COVER &&
	    hid->group == HID_GROUP_MULTITOUCH)
		hid->group = HID_GROUP_GENERIC;

	if (page == HID_UP_GENDESK)
		for (i = 0; i < parser->local.usage_index; i++)
			if (parser->local.usage[i] == HID_GD_POINTER)
				parser->scan_flags |= HID_SCAN_FLAG_GD_POINTER;

	if (page >= HID_UP_MSVENDOR)
		parser->scan_flags |= HID_SCAN_FLAG_VENDOR_SPECIFIC;

	if (page == HID_UP_GOOGLEVENDOR)
		for (i = 0; i < parser->local.usage_index; i++)
			if (parser->local.usage[i] ==
			    (HID_UP_GOOGLEVENDOR | 0x0001))
				hid->group = HID_GROUP_VIVALDI;
}

static int hid_scan_main(struct hid_parser *parser, struct hid_item *item)
{
	uint32_t data;
	unsigned int i;

	hid_concatenate_last_usage_page(parser);
	data = item_udata(item);

	switch (item->tag) {
	case HID_MAIN_ITEM_TAG_BEGIN_COLLECTION:
		hid_scan_collection(parser, data & 0xff);
		break;
	case HID_MAIN_ITEM_TAG_INPUT:
		/* Constant inputs are padding to hid-input too. */
		if (data & HID_MAIN_ITEM_CONSTANT)
			break;
		for (i = 0; i < parser->local.usage_index; i++)
			hid_scan_input_usage(parser, parser->local.usage[i]);
		break;
	case HID_MAIN_ITEM_TAG_FEATURE:
		for (i = 0; i < parser->local.usage_index; i++)
			hid_scan_feature_usage(parser, parser->local.usage[i]);
		break;
	default:
		break;
	}
	hid_reset_local(parser);
	return 0;
}

static int hid_scan_report(struct hid_device *hid)
{
	struct hid_parser *parser;
	struct hid_item item;
	const uint8_t *start = hid->dev_rdesc;
	const uint8_t *end = start + hid->dev_rsize;

	parser = kcalloc(1, sizeof(*parser));
	if (!parser)
		return -ENOMEM;
	parser->device = hid;
	parser->scanning = true;
	hid->group = HID_GROUP_GENERIC;

	/* Errors do not stop the scan: the real parse reports them. */
	while ((start = hid_fetch_item(start, end, &item)) != NULL) {
		if (item.format != HID_ITEM_FORMAT_SHORT)
			continue;
		switch (item.type) {
		case HID_ITEM_TYPE_MAIN:
			hid_scan_main(parser, &item);
			break;
		case HID_ITEM_TYPE_GLOBAL:
			hid_parser_global(parser, &item);
			break;
		case HID_ITEM_TYPE_LOCAL:
			hid_parser_local(parser, &item);
			break;
		default:
			break;
		}
	}

	if ((parser->scan_flags & HID_SCAN_FLAG_MT_WIN_8) &&
	    hid->group == HID_GROUP_MULTITOUCH)
		hid->group = HID_GROUP_MULTITOUCH_WIN_8;

	switch (hid->vendor) {
	case USB_VENDOR_ID_WACOM:
		hid->group = HID_GROUP_WACOM;
		break;
	case USB_VENDOR_ID_SYNAPTICS:
		/* A vendor-specific pointer: an RMI device, not a HID one. */
		if (hid->group == HID_GROUP_GENERIC &&
		    (parser->scan_flags & HID_SCAN_FLAG_VENDOR_SPECIFIC) &&
		    (parser->scan_flags & HID_SCAN_FLAG_GD_POINTER))
			hid->group = HID_GROUP_RMI;
		break;
	}

	kfree(parser);
	return 0;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

int hid_parse_report(struct hid_device *hid, const uint8_t *start,
		     unsigned int size)
{
	uint8_t *copy;

	if (!hid || !start || !size || size > HID_MAX_RDESC_SIZE)
		return -EINVAL;
	copy = kalloc(size);
	if (!copy)
		return -ENOMEM;
	kmemcpy(copy, start, size);
	kfree(hid->dev_rdesc);
	hid->dev_rdesc = copy;
	hid->dev_rsize = size;
	return 0;
}

static void hid_free_report(struct hid_report *report)
{
	for (unsigned int n = 0; n < report->maxfield; n++)
		kfree(report->field[n]);
	kfree(report);
}

/* Free everything the parse built.  The device's own descriptor copy
 * (dev_rdesc) stays. */
static void hid_close_report(struct hid_device *device)
{
	for (unsigned int i = 0; i < HID_REPORT_TYPES; i++) {
		struct hid_report_enum *report_enum = device->report_enum + i;

		for (unsigned int j = 0; j < HID_MAX_IDS; j++) {
			struct hid_report *report =
				report_enum->report_id_hash[j];

			if (report)
				hid_free_report(report);
		}
		kmemset(report_enum, 0, sizeof(*report_enum));
		INIT_LIST_HEAD(&report_enum->report_list);
	}

	kfree(device->rdesc);
	device->rdesc = NULL;
	device->rsize = 0;

	kfree(device->collection);
	device->collection = NULL;
	device->collection_size = 0;
	device->maxcollection = 0;
	device->maxapplication = 0;
	device->total_usages = 0;

	kfree(device->pad_buf);
	device->pad_buf = NULL;
	device->pad_len = 0;

	device->status &= ~HID_STAT_PARSED;
}

/* The zero-padding buffer: as long as the longest report. */
static int hid_alloc_pad_buffer(struct hid_device *device)
{
	unsigned int longest = 0;

	for (unsigned int t = 0; t < HID_REPORT_TYPES; t++) {
		struct hid_report *report;

		list_for_each_entry(report, &device->report_enum[t].report_list,
				    list) {
			unsigned int len = hid_report_len(report);

			if (len > longest)
				longest = len;
		}
	}
	device->pad_len = longest + 8;
	device->pad_buf = kcalloc(1, device->pad_len);
	return device->pad_buf ? 0 : -ENOMEM;
}

int hid_open_report(struct hid_device *device)
{
	struct hid_parser *parser = NULL;
	struct hid_item item;
	unsigned int size, orig_size;
	const uint8_t *start, *end, *next;
	uint8_t *buf;
	const uint8_t *fixed;
	int ret;

	if (WARN_ON(device->status & HID_STAT_PARSED))
		return -EBUSY;
	if (!device->dev_rdesc || !device->dev_rsize)
		return -ENODEV;

	/* The driver may rewrite a copy; the device's original stays. */
	orig_size = size = device->dev_rsize;
	buf = kalloc(size);
	if (!buf)
		return -ENOMEM;
	kmemcpy(buf, device->dev_rdesc, size);
	fixed = buf;
	if (device->driver && device->driver->report_fixup)
		fixed = device->driver->report_fixup(device, buf, &size);
	if (!fixed || !size || size > HID_MAX_RDESC_SIZE ||
	    (fixed == buf && size > orig_size)) {
		hid_err(device, "report_fixup returned a bad descriptor\n");
		kfree(buf);
		return -EINVAL;
	}
	device->rdesc = kalloc(size);
	if (!device->rdesc) {
		kfree(buf);
		return -ENOMEM;
	}
	kmemcpy(device->rdesc, fixed, size);
	device->rsize = size;
	kfree(buf);

	parser = kcalloc(1, sizeof(*parser));
	if (!parser) {
		ret = -ENOMEM;
		goto err;
	}
	parser->device = device;

	device->collection =
		kcalloc(HID_DEFAULT_NUM_COLLECTIONS, sizeof(struct hid_collection));
	if (!device->collection) {
		ret = -ENOMEM;
		goto err;
	}
	device->collection_size = HID_DEFAULT_NUM_COLLECTIONS;
	for (unsigned int i = 0; i < HID_DEFAULT_NUM_COLLECTIONS; i++)
		device->collection[i].parent_idx = -1;

	ret = -EINVAL;
	start = device->rdesc;
	end = start + size;
	while ((next = hid_fetch_item(start, end, &item)) != NULL) {
		int r;

		start = next;
		if (item.format != HID_ITEM_FORMAT_SHORT) {
			hid_err(device, "unexpected long global item\n");
			goto err;
		}
		switch (item.type) {
		case HID_ITEM_TYPE_MAIN:
			r = hid_parser_main(parser, &item);
			break;
		case HID_ITEM_TYPE_GLOBAL:
			r = hid_parser_global(parser, &item);
			break;
		case HID_ITEM_TYPE_LOCAL:
			r = hid_parser_local(parser, &item);
			break;
		default:
			r = hid_parser_reserved(parser, &item);
			break;
		}
		if (r) {
			hid_err(device, "item %u %u %u %u parsing failed\n",
				item.format, (unsigned int)item.size,
				(unsigned int)item.type, (unsigned int)item.tag);
			if (r == -ENOMEM)
				ret = -ENOMEM;
			goto err;
		}

		if (start == end) {
			if (parser->collection_stack_ptr) {
				hid_err(device, "unbalanced collection at end "
						"of report description\n");
				goto err;
			}
			if (parser->local.delimiter_depth) {
				hid_err(device, "unbalanced delimiter at end "
						"of report description\n");
				goto err;
			}
			ret = hid_alloc_pad_buffer(device);
			if (ret)
				goto err;
			/* Values of the resolution multipliers before the
			 * driver sets them: what the device starts with. */
			hid_setup_resolution_multiplier(device);
			kfree(parser);
			device->status |= HID_STAT_PARSED;
			return 0;
		}
	}

	hid_err(device, "item fetching failed at offset %u/%u\n",
		(unsigned int)(start - device->rdesc), size);
err:
	kfree(parser);
	hid_close_report(device);
	return ret;
}

// ---------------------------------------------------------------------------
// Resolution multipliers (Generic Desktop 0x48): a wheel that reports in
// fractions of a notch says so with a feature whose value selects the
// multiplier.
// ---------------------------------------------------------------------------

static int hid_calculate_multiplier(struct hid_device *hid,
				    struct hid_field *multiplier)
{
	int64_t v = multiplier->value[0];
	int64_t lmin = multiplier->logical_minimum;
	int64_t lmax = multiplier->logical_maximum;
	int64_t pmin = multiplier->physical_minimum;
	int64_t pmax = multiplier->physical_maximum;
	int64_t m;

	/* A multiplier of zero is not allowed; a field without a logical
	 * range can only mean "no multiplier". */
	if (lmax - lmin == 0)
		return 1;
	/* Integer arithmetic on purpose: the multiplier is the physical
	 * minimum until the value reaches the logical maximum. */
	m = (v - lmin) / (lmax - lmin) * (pmax - pmin) + pmin;
	if (multiplier->unit_exponent != 0)
		hid_warn(hid,
			 "unsupported Resolution Multiplier unit exponent %d\n",
			 multiplier->unit_exponent);
	if (m == 0 || m > 255 || m < -255) {
		hid_warn(hid, "unsupported Resolution Multiplier %lld\n",
			 (long long)m);
		m = 1;
	}
	return (int)m;
}

static void hid_apply_multiplier_to_field(struct hid_device *hid,
					  struct hid_field *field,
					  struct hid_collection *mcoll,
					  int effective)
{
	for (unsigned int i = 0; i < field->maxusage; i++) {
		struct hid_usage *usage = &field->usage[i];
		struct hid_collection *c;
		unsigned int hops = 0;

		if (usage->collection_index >= hid->maxcollection)
			continue;
		c = &hid->collection[usage->collection_index];
		/* Walk up to the multiplier's logical collection (the
		 * parent chain is acyclic: parents come first). */
		while (c->parent_idx != -1 && c != mcoll &&
		       hops++ < HID_MAX_COLLECTION_DEPTH)
			c = &hid->collection[c->parent_idx];
		if (c->parent_idx != -1 || mcoll == NULL)
			usage->resolution_multiplier = effective;
	}
}

static void hid_apply_multiplier(struct hid_device *hid,
				 struct hid_field *multiplier)
{
	struct hid_collection *mcoll;
	struct hid_report *rep;
	unsigned int hops = 0;
	int effective;

	/* The multiplier applies to the controls of its logical collection,
	 * or of its whole report when it has none. */
	if (multiplier->usage->collection_index >= hid->maxcollection)
		return;
	mcoll = &hid->collection[multiplier->usage->collection_index];
	while (mcoll->parent_idx != -1 &&
	       mcoll->type != HID_COLLECTION_LOGICAL &&
	       hops++ < HID_MAX_COLLECTION_DEPTH)
		mcoll = &hid->collection[mcoll->parent_idx];
	if (mcoll->type != HID_COLLECTION_LOGICAL)
		mcoll = NULL;

	effective = hid_calculate_multiplier(hid, multiplier);

	list_for_each_entry(rep, &hid->report_enum[HID_INPUT_REPORT].report_list,
			    list)
		for (unsigned int i = 0; i < rep->maxfield; i++)
			hid_apply_multiplier_to_field(hid, rep->field[i], mcoll,
						      effective);
}

void hid_setup_resolution_multiplier(struct hid_device *hid)
{
	struct hid_report *rep;

	list_for_each_entry(rep,
			    &hid->report_enum[HID_FEATURE_REPORT].report_list,
			    list) {
		for (unsigned int i = 0; i < rep->maxfield; i++) {
			struct hid_field *field = rep->field[i];

			if (field->report_count < 1)
				continue;
			for (unsigned int j = 0; j < field->maxusage; j++)
				if (field->usage[j].hid ==
				    HID_GD_RESOLUTION_MULTIPLIER)
					hid_apply_multiplier(hid, field);
		}
	}
}

// ---------------------------------------------------------------------------
// Bits
// ---------------------------------------------------------------------------

/* `n' (<= 32) bits starting `offset' bits into `buf' (little-endian bit
 * order); bits beyond `len' bytes read as zero. */
static uint32_t hid_get_bits(const uint8_t *buf, size_t len,
			     unsigned int offset, unsigned int n)
{
	uint32_t value = 0;
	unsigned int got = 0;

	while (got < n) {
		unsigned int idx = offset >> 3, shift = offset & 7;
		unsigned int chunk = 8 - shift;
		uint32_t byte;

		if (chunk > n - got)
			chunk = n - got;
		byte = idx < len ? buf[idx] : 0;
		value |= ((byte >> shift) & ((1u << chunk) - 1)) << got;
		got += chunk;
		offset += chunk;
	}
	return value;
}

/* Store the low `n' (<= 32) bits of `value' at `offset'. */
static void hid_put_bits(uint8_t *buf, unsigned int offset, unsigned int n,
			 uint32_t value)
{
	while (n) {
		unsigned int idx = offset >> 3, shift = offset & 7;
		unsigned int chunk = 8 - shift;
		uint8_t mask;

		if (chunk > n)
			chunk = n;
		mask = (uint8_t)(((1u << chunk) - 1) << shift);
		buf[idx] = (uint8_t)((buf[idx] & ~mask) |
				     ((value << shift) & mask));
		value >>= chunk;
		offset += chunk;
		n -= chunk;
	}
}

static uint32_t hid_extract(const struct hid_device *hid, const uint8_t *buf,
			    size_t len, unsigned int offset, unsigned int n)
{
	if (n > 32) {
		static bool warned;

		if (!warned) {
			warned = true;
			hid_warn(hid, "field extraction of %u bits (> 32)\n",
				 n);
		}
		n = 32;
	}
	return hid_get_bits(buf, len, offset, n);
}

uint32_t hid_field_extract(const struct hid_device *hid, const uint8_t *report,
			   unsigned int offset, unsigned int n)
{
	return hid_extract(hid, report, (size_t)-1, offset, n);
}

/* Sign-extend an n-bit two's complement value. */
int32_t hid_snto32(uint32_t value, unsigned int n)
{
	if (!value || !n)
		return 0;
	if (n >= 32)
		return (int32_t)value;
	if (value & (1u << (n - 1)))
		return (int32_t)(value | (~0u << n));
	return (int32_t)value;
}

/* The n-bit two's complement form of `value', saturated to the range. */
static uint32_t hid_s32ton(int32_t value, unsigned int n)
{
	int32_t high;

	if (!n)
		return 0;
	if (n >= 32)
		return (uint32_t)value;
	high = value >> (n - 1);
	if (high && high != -1)
		return value < 0 ? 1u << (n - 1) : (1u << (n - 1)) - 1;
	return (uint32_t)value & ((1u << n) - 1);
}

static void hid_implement(const struct hid_device *hid, uint8_t *report,
			  unsigned int offset, unsigned int n, uint32_t value)
{
	if (n > 32) {
		static bool warned;

		if (!warned) {
			warned = true;
			hid_warn(hid, "field packing of %u bits (> 32)\n", n);
		}
		n = 32;
	} else if (n < 32) {
		uint32_t m = (1u << n) - 1;

		if (value > m) {
			hid_warn(hid, "value %u is out of range\n", value);
			value &= m;
		}
	}
	hid_put_bits(report, offset, n, value);
}

// ---------------------------------------------------------------------------
// Incoming reports
// ---------------------------------------------------------------------------

static struct hid_report *hid_get_report(struct hid_report_enum *report_enum,
					 const uint8_t *data)
{
	unsigned int n = 0; // unnumbered reports are report 0

	if (report_enum->numbered)
		n = data[0];
	return report_enum->report_id_hash[n];
}

static unsigned int hid_compute_report_size(const struct hid_report *report)
{
	return report->size ? ((report->size - 1) >> 3) + 1 : 0;
}

/* Is `value' an index into the usages of an array field. */
static bool hid_array_value_is_valid(const struct hid_field *field,
				     int32_t value)
{
	int64_t min = field->logical_minimum;

	return value >= min && value <= field->logical_maximum &&
	       (int64_t)value - min < (int64_t)field->maxusage;
}

static void hid_process_event(struct hid_device *hid, struct hid_field *field,
			      struct hid_usage *usage, int32_t value,
			      int interrupt)
{
	struct hid_driver *hdrv = hid->driver;

	(void)interrupt;
	if (hdrv && hdrv->event) {
		int ret = hdrv->event(hid, field, usage, value);

		if (ret != 0) {
			if (ret < 0)
				hid_err(hid, "%s's event failed with %d\n",
					hdrv->name, ret);
			return;
		}
	}
	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_hid_event(hid, field, usage, value);
}

/* Extract every value of the field into new_value. */
static void hid_input_fetch_field(struct hid_device *hid,
				  struct hid_field *field, const uint8_t *data,
				  size_t len)
{
	unsigned int count = field->report_count;
	unsigned int size = field->report_size;
	int32_t min = field->logical_minimum;
	int32_t *value = field->new_value;

	kmemset(value, 0, count * sizeof(int32_t));
	field->ignored = false;

	for (unsigned int n = 0; n < count; n++) {
		uint32_t raw = hid_extract(hid, data, len,
					   field->report_offset + n * size,
					   size);

		value[n] = min < 0 ? hid_snto32(raw, size) : (int32_t)raw;

		/* An array reporting ErrorRollOver (too many keys down) says
		 * nothing about the keys: keep the previous state. */
		if (!(field->flags & HID_MAIN_ITEM_VARIABLE) &&
		    hid_array_value_is_valid(field, value[n]) &&
		    field->usage[(int64_t)value[n] - min].hid ==
			    HID_UP_KEYBOARD + 1) {
			field->ignored = true;
			return;
		}
	}
}

static void hid_input_var_field(struct hid_device *hid,
				struct hid_field *field, int interrupt)
{
	unsigned int count = field->report_count;
	int32_t *value = field->new_value;

	for (unsigned int n = 0; n < count; n++)
		hid_process_event(hid, field, &field->usage[n], value[n],
				  interrupt);
	kmemcpy(field->value, value, count * sizeof(int32_t));
}

static bool hid_value_absent(const int32_t *array, int32_t value,
			     unsigned int n)
{
	while (n--)
		if (*array++ == value)
			return false;
	return true;
}

/* An array field lists the usages that are active: report the ones that
 * went away (0) and the ones that came (1). */
static void hid_input_array_field(struct hid_device *hid,
				  struct hid_field *field, int interrupt)
{
	unsigned int count = field->report_count;
	int64_t min = field->logical_minimum;
	int32_t *value = field->new_value;

	if (field->ignored)
		return;

	for (unsigned int n = 0; n < count; n++) {
		if (hid_array_value_is_valid(field, field->value[n]) &&
		    hid_value_absent(value, field->value[n], count))
			hid_process_event(hid, field,
					  &field->usage[field->value[n] - min],
					  0, interrupt);
		if (hid_array_value_is_valid(field, value[n]) &&
		    hid_value_absent(field->value, value[n], count))
			hid_process_event(hid, field,
					  &field->usage[value[n] - min], 1,
					  interrupt);
	}
	kmemcpy(field->value, value, count * sizeof(int32_t));
}

static void hid_process_report(struct hid_device *hid,
			       struct hid_report *report, const uint8_t *data,
			       size_t len, int interrupt)
{
	unsigned int a;

	/* Every value first, then the events: a driver looking at another
	 * field's value sees this report's. */
	for (a = 0; a < report->maxfield; a++)
		hid_input_fetch_field(hid, report->field[a], data, len);

	for (a = 0; a < report->maxfield; a++) {
		struct hid_field *field = report->field[a];

		if (field->flags & HID_MAIN_ITEM_VARIABLE)
			hid_input_var_field(hid, field, interrupt);
		else
			hid_input_array_field(hid, field, interrupt);
	}
}

int hid_report_raw_event(struct hid_device *hid, int type, uint8_t *data,
			 uint32_t size, int interrupt)
{
	struct hid_report_enum *report_enum;
	struct hid_report *report;
	struct hid_driver *hdrv;
	unsigned int max_buffer_size = HID_MAX_BUFFER_SIZE;
	uint32_t rsize, csize = size;
	const uint8_t *cdata = data;

	if (type < 0 || type >= HID_REPORT_TYPES || !data || !size)
		return -EINVAL;
	mutex_assert_held(&hid->driver_input_lock);

	report_enum = hid->report_enum + type;
	report = hid_get_report(report_enum, data);
	if (!report)
		return 0;

	if (report_enum->numbered) {
		cdata++;
		csize--;
	}

	rsize = hid_compute_report_size(report);
	if (hid->ll_driver && hid->ll_driver->max_buffer_size)
		max_buffer_size = hid->ll_driver->max_buffer_size;
	if (report_enum->numbered && rsize >= max_buffer_size)
		rsize = max_buffer_size - 1;
	else if (rsize > max_buffer_size)
		rsize = max_buffer_size;

	if (csize < rsize) {
		/* A short report: the missing bytes read as zero.  Padded in
		 * a buffer of our own -- the caller's ends where it ends. */
		if (hid_short_report_ratelimit(hid))
			hid_warn(hid,
				 "event data for report %u was too short "
				 "(%u vs %u)\n",
				 report->id, csize, rsize);
		if (!hid->pad_buf || rsize > hid->pad_len)
			return -EINVAL;
		kmemcpy(hid->pad_buf, cdata, csize);
		kmemset(hid->pad_buf + csize, 0, rsize - csize);
		cdata = hid->pad_buf;
		csize = rsize;
	}

	if (report->maxfield) {
		hid_process_report(hid, report, cdata, csize, interrupt);
		hdrv = hid->driver;
		if (hdrv && hdrv->report)
			hdrv->report(hid, report);
	}

	if (hid->claimed & HID_CLAIMED_INPUT)
		hidinput_report_event(hid, report);
	return 0;
}

int hid_input_report(struct hid_device *hid, int type, uint8_t *data,
		     uint32_t size, int interrupt)
{
	struct hid_report_enum *report_enum;
	struct hid_driver *hdrv;
	struct hid_report *report;
	int ret;

	if (!hid)
		return -ENODEV;
	if (type < 0 || type >= HID_REPORT_TYPES)
		return -EINVAL;
	/* A driver is being bound or unbound: the report is not wanted. */
	if (!mutex_trylock(&hid->driver_input_lock))
		return -EBUSY;

	if (!hid->driver) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!size || !data) {
		ret = -EINVAL;
		goto unlock;
	}
	report_enum = hid->report_enum + type;
	hdrv = hid->driver;

	report = hid_get_report(report_enum, data);
	if (!report) {
		hid_dbg(hid, "undefined report id %u received\n",
			(unsigned int)data[0]);
		ret = -EINVAL;
		goto unlock;
	}

	if (hdrv->raw_event) {
		ret = hdrv->raw_event(hid, report, data, (int)size);
		if (ret < 0)
			goto unlock;
	}

	ret = hid_report_raw_event(hid, type, data, size, interrupt);
unlock:
	mutex_unlock(&hid->driver_input_lock);
	return ret;
}

// ---------------------------------------------------------------------------
// Outgoing reports
// ---------------------------------------------------------------------------

uint8_t *hid_alloc_report_buf(struct hid_report *report)
{
	/* The report, 7 bytes of slack and the ID byte of an unnumbered
	 * report (raw requests always carry one). */
	return kcalloc(1, hid_report_len(report) + 7 + (report->id == 0));
}

static void hid_output_field(const struct hid_device *hid,
			     struct hid_field *field, uint8_t *data)
{
	unsigned int count = field->report_count;
	unsigned int offset = field->report_offset;
	unsigned int size = field->report_size;

	for (unsigned int n = 0; n < count; n++) {
		if (field->logical_minimum < 0)
			hid_implement(hid, data, offset + n * size, size,
				      hid_s32ton(field->value[n], size));
		else
			hid_implement(hid, data, offset + n * size, size,
				      (uint32_t)field->value[n]);
	}
}

void hid_output_report(struct hid_report *report, uint8_t *data)
{
	if (report->id > 0)
		*data++ = (uint8_t)report->id;
	kmemset(data, 0, (report->size + 7) >> 3);
	for (unsigned int n = 0; n < report->maxfield; n++)
		hid_output_field(report->device, report->field[n], data);
}

int hid_set_field(struct hid_field *field, unsigned int offset, int32_t value)
{
	unsigned int size;

	if (!field)
		return -1;
	size = field->report_size;
	if (offset >= field->report_count) {
		hid_err(field->report->device,
			"offset (%u) exceeds report_count (%u)\n", offset,
			field->report_count);
		return -1;
	}
	if (field->logical_minimum < 0 &&
	    value != hid_snto32(hid_s32ton(value, size), size)) {
		hid_err(field->report->device, "value %d is out of range\n",
			value);
		return -1;
	}
	field->value[offset] = value;
	return 0;
}

int __hid_request(struct hid_device *hid, struct hid_report *report,
		  int reqtype)
{
	uint8_t *buf, *data_buf;
	uint32_t len;
	int ret;

	buf = hid_alloc_report_buf(report);
	if (!buf)
		return -ENOMEM;

	/* buf[0] is the report ID, also for an unnumbered report (0). */
	data_buf = buf;
	len = hid_report_len(report);
	if (report->id == 0) {
		data_buf++;
		len++;
	}

	if (reqtype == HID_REQ_SET_REPORT)
		hid_output_report(report, data_buf);

	ret = hid_hw_raw_request(hid, (unsigned char)report->id, buf, len,
				 (unsigned char)report->type, reqtype);
	if (ret < 0) {
		hid_dbg(hid, "unable to complete request: %d\n", ret);
		goto out;
	}
	if ((uint32_t)ret > len)
		ret = (int)len;

	if (reqtype == HID_REQ_GET_REPORT) {
		/* The answer is processed like an incoming report (dropped
		 * while the driver is still probing, as every report is). */
		if (hid->report_enum[report->type].numbered)
			hid_input_report(hid, (int)report->type, buf,
					 (uint32_t)ret, 0);
		else if (ret > 1)
			hid_input_report(hid, (int)report->type, buf + 1,
					 (uint32_t)ret - 1, 0);
	}
	ret = 0;
out:
	kfree(buf);
	return ret;
}

void hid_hw_request(struct hid_device *hdev, struct hid_report *report,
		    int reqtype)
{
	if (hdev->ll_driver->request) {
		hdev->ll_driver->request(hdev, report, reqtype);
		return;
	}
	__hid_request(hdev, report, reqtype);
}

static unsigned int hid_max_buffer(const struct hid_device *hdev)
{
	if (hdev->ll_driver && hdev->ll_driver->max_buffer_size)
		return hdev->ll_driver->max_buffer_size;
	return HID_MAX_BUFFER_SIZE;
}

int hid_hw_raw_request(struct hid_device *hdev, unsigned char reportnum,
		       uint8_t *buf, size_t len, unsigned char rtype,
		       int reqtype)
{
	might_sleep();
	if (len < 1 || len > hid_max_buffer(hdev) || !buf)
		return -EINVAL;
	if (rtype >= HID_REPORT_TYPES)
		return -EINVAL;
	return hdev->ll_driver->raw_request(hdev, reportnum, buf, len, rtype,
					    reqtype);
}

int hid_hw_output_report(struct hid_device *hdev, uint8_t *buf, size_t len)
{
	might_sleep();
	if (len < 1 || len > hid_max_buffer(hdev) || !buf)
		return -EINVAL;
	if (hdev->ll_driver->output_report)
		return hdev->ll_driver->output_report(hdev, buf, len);
	return -ENOSYS;
}

int hid_hw_wait(struct hid_device *hdev)
{
	if (hdev->ll_driver->wait)
		return hdev->ll_driver->wait(hdev);
	return 0;
}

int hid_hw_idle(struct hid_device *hdev, int report, int idle, int reqtype)
{
	if (hdev->ll_driver->idle)
		return hdev->ll_driver->idle(hdev, report, idle, reqtype);
	return 0;
}

bool hid_hw_may_wakeup(struct hid_device *hdev)
{
	if (hdev->ll_driver->may_wakeup)
		return hdev->ll_driver->may_wakeup(hdev);
	return false;
}

int hid_hw_power(struct hid_device *hdev, int level)
{
	if (hdev->ll_driver->power)
		return hdev->ll_driver->power(hdev, level);
	return 0;
}

int hid_hw_open(struct hid_device *hdev)
{
	int ret = 0;

	mutex_lock(&hdev->ll_open_lock);
	if (!hdev->ll_open_count++) {
		if (hdev->ll_driver->open)
			ret = hdev->ll_driver->open(hdev);
		if (ret)
			hdev->ll_open_count--;
		if (hdev->driver && hdev->driver->on_hid_hw_open)
			hdev->driver->on_hid_hw_open(hdev);
	}
	mutex_unlock(&hdev->ll_open_lock);
	return ret;
}

void hid_hw_close(struct hid_device *hdev)
{
	mutex_lock(&hdev->ll_open_lock);
	if (!WARN_ON(hdev->ll_open_count == 0) && !--hdev->ll_open_count) {
		if (hdev->ll_driver->close)
			hdev->ll_driver->close(hdev);
		if (hdev->driver && hdev->driver->on_hid_hw_close)
			hdev->driver->on_hid_hw_close(hdev);
	}
	mutex_unlock(&hdev->ll_open_lock);
}

void hid_device_io_start(struct hid_device *hdev)
{
	if (hdev->io_started) {
		hid_warn(hdev, "io already started\n");
		return;
	}
	hdev->io_started = true;
	mutex_unlock(&hdev->driver_input_lock);
}

void hid_device_io_stop(struct hid_device *hdev)
{
	if (!hdev->io_started) {
		hid_warn(hdev, "io already stopped\n");
		return;
	}
	hdev->io_started = false;
	mutex_lock(&hdev->driver_input_lock);
}

// ---------------------------------------------------------------------------
// Lookups for drivers
// ---------------------------------------------------------------------------

struct hid_report *hid_validate_values(struct hid_device *hid,
				       unsigned int type, unsigned int id,
				       unsigned int field_index,
				       unsigned int report_counts)
{
	struct hid_report *report;

	if (type > HID_FEATURE_REPORT) {
		hid_err(hid, "invalid HID report type %u\n", type);
		return NULL;
	}
	if (id >= HID_MAX_IDS) {
		hid_err(hid, "invalid HID report id %u\n", id);
		return NULL;
	}
	/* By hash, not hid_get_report(): drivers ask before knowing whether
	 * the device numbers its reports.  ID 0 means the first report. */
	if (id == 0)
		report = list_first_entry_or_null(
			&hid->report_enum[type].report_list, struct hid_report,
			list);
	else
		report = hid->report_enum[type].report_id_hash[id];
	if (!report) {
		hid_err(hid, "missing report type %u id %u\n", type, id);
		return NULL;
	}
	if (report->maxfield <= field_index) {
		hid_err(hid, "not enough fields in report %u\n", id);
		return NULL;
	}
	if (report->field[field_index]->report_count < report_counts) {
		hid_err(hid, "not enough values in report %u field %u\n", id,
			field_index);
		return NULL;
	}
	return report;
}

struct hid_field *hid_find_field(struct hid_device *hdev,
				 unsigned int report_type,
				 unsigned int application, unsigned int usage)
{
	struct hid_report *report;

	if (report_type >= HID_REPORT_TYPES)
		return NULL;
	list_for_each_entry(report, &hdev->report_enum[report_type].report_list,
			    list) {
		for (unsigned int i = 0; i < report->maxfield; i++) {
			struct hid_field *field = report->field[i];

			for (unsigned int j = 0; j < field->maxusage; j++)
				if (field->usage[j].hid == usage &&
				    field->application == application)
					return field;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Connecting the device to its consumers
// ---------------------------------------------------------------------------

static const char *const hid_gd_types[] = {
	"Device", "Pointer", "Mouse", "Device", "Joystick",
	"Gamepad", "Keyboard", "Keypad", "Multi-Axis Controller"
};

int hid_connect(struct hid_device *hdev, unsigned int connect_mask)
{
	const char *type, *bus;
	char buf[64];
	unsigned int i;

	if (hdev->quirks & HID_QUIRK_HIDINPUT_FORCE)
		connect_mask |= HID_CONNECT_HIDINPUT_FORCE;

	if ((connect_mask & HID_CONNECT_HIDINPUT) &&
	    !hidinput_connect(hdev, connect_mask & HID_CONNECT_HIDINPUT_FORCE))
		hdev->claimed |= HID_CLAIMED_INPUT;
	/* There is no raw HID node and no hiddev: input is the only
	 * consumer. */
	if (connect_mask & HID_CONNECT_DRIVER)
		hdev->claimed |= HID_CLAIMED_DRIVER;

	/* A driver with a raw_event hook may consume the reports itself. */
	if (!hdev->claimed && !hdev->driver->raw_event) {
		hid_err(hdev, "device has no listeners, quitting\n");
		return -ENODEV;
	}

	buf[0] = 0;
	if (hdev->claimed & HID_CLAIMED_INPUT)
		ksnprintf(buf, sizeof(buf), "input");

	type = "Device";
	for (i = 0; i < hdev->maxcollection; i++) {
		struct hid_collection *col = &hdev->collection[i];

		if (col->type == HID_COLLECTION_APPLICATION &&
		    (col->usage & HID_USAGE_PAGE) == HID_UP_GENDESK &&
		    (col->usage & 0xffff) < HID_ARRAY_SIZE(hid_gd_types)) {
			type = hid_gd_types[col->usage & 0xffff];
			break;
		}
	}

	switch (hdev->bus) {
	case BUS_USB:
		bus = "USB";
		break;
	case BUS_BLUETOOTH:
		bus = "BLUETOOTH";
		break;
	case BUS_I2C:
		bus = "I2C";
		break;
	case BUS_VIRTUAL:
		bus = "VIRTUAL";
		break;
	default:
		bus = "<UNKNOWN>";
	}

	hid_info(hdev, "%s: %s HID v%x.%02x %s [%s] on %s\n", buf, bus,
		 hdev->version >> 8, hdev->version & 0xff, type, hdev->name,
		 hdev->phys);
	return 0;
}

void hid_disconnect(struct hid_device *hdev)
{
	if (hdev->claimed & HID_CLAIMED_INPUT)
		hidinput_disconnect(hdev);
	hdev->claimed = 0;
}

int hid_hw_start(struct hid_device *hdev, unsigned int connect_mask)
{
	int error;

	might_sleep();
	error = hdev->ll_driver->start(hdev);
	if (error)
		return error;
	if (connect_mask) {
		error = hid_connect(hdev, connect_mask);
		if (error) {
			hdev->ll_driver->stop(hdev);
			return error;
		}
	}
	return 0;
}

void hid_hw_stop(struct hid_device *hdev)
{
	might_sleep();
	hid_disconnect(hdev);
	hdev->ll_driver->stop(hdev);
}

// ---------------------------------------------------------------------------
// Driver matching and binding
// ---------------------------------------------------------------------------

bool hid_match_one_id(const struct hid_device *hdev,
		      const struct hid_device_id *id)
{
	return (id->bus == HID_BUS_ANY || id->bus == hdev->bus) &&
	       (id->group == HID_GROUP_ANY || id->group == hdev->group) &&
	       (id->vendor == HID_ANY_ID || id->vendor == hdev->vendor) &&
	       (id->product == HID_ANY_ID || id->product == hdev->product);
}

const struct hid_device_id *hid_match_id(const struct hid_device *hdev,
					 const struct hid_device_id *id)
{
	if (!id)
		return NULL;
	for (; id->bus; id++)
		if (hid_match_one_id(hdev, id))
			return id;
	return NULL;
}

static const struct hid_device_id *hid_match_device(struct hid_device *hdev,
						    struct hid_driver *hdrv)
{
	return hid_match_id(hdev, hdrv->id_table);
}

/* Try one driver on an unbound device.  hid_bus_lock held. */
static int hid_device_probe(struct hid_device *hdev, struct hid_driver *hdrv)
{
	const struct hid_device_id *id;
	int ret;

	id = hid_match_device(hdev, hdrv);
	if (!id)
		return -ENODEV;
	if (hdrv->match) {
		if (!hdrv->match(hdev, hid_ignore_special_drivers))
			return -ENODEV;
	} else if (hid_ignore_special_drivers ||
		   (hdev->quirks & HID_QUIRK_IGNORE_SPECIAL_DRIVER)) {
		return -ENODEV;
	}

	mutex_lock(&hdev->driver_input_lock);
	hdev->io_started = false;
	/* A previous driver's quirk changes do not carry over. */
	hdev->quirks = hid_lookup_quirk(hdev);
	hdev->driver = hdrv;

	if (hdrv->probe) {
		ret = hdrv->probe(hdev, id);
	} else {
		ret = hid_open_report(hdev);
		if (!ret)
			ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	}
	if (ret) {
		hid_close_report(hdev);
		hdev->driver = NULL;
		hdev->drvdata = NULL;
		if (ret != -ENODEV)
			hid_printk(hdev, "", "probe with driver %s failed "
					     "with error %d\n",
				   hdrv->name, ret);
	}
	if (!hdev->io_started)
		mutex_unlock(&hdev->driver_input_lock);
	return ret;
}

/* Unbind.  hid_bus_lock held. */
static void hid_device_remove(struct hid_device *hdev)
{
	struct hid_driver *hdrv;

	mutex_lock(&hdev->driver_input_lock);
	hdev->io_started = false;
	hdrv = hdev->driver;
	if (hdrv) {
		if (hdrv->remove)
			hdrv->remove(hdev);
		else
			hid_hw_stop(hdev);
		hid_close_report(hdev);
		hdev->driver = NULL;
		hdev->drvdata = NULL;
	}
	if (!hdev->io_started)
		mutex_unlock(&hdev->driver_input_lock);
}

/* Offer an unbound device to every driver, in registration order. */
static void hid_bind_device(struct hid_device *hdev)
{
	struct hid_driver *drv;

	list_for_each_entry(drv, &hid_drivers, node)
		if (!hid_device_probe(hdev, drv))
			return;
}

int hid_register_driver(struct hid_driver *drv)
{
	struct hid_device *hdev;

	if (!drv || !drv->name)
		return -EINVAL;
	mutex_lock(&hid_bus_lock);
	if (drv->registered) {
		mutex_unlock(&hid_bus_lock);
		return -EBUSY;
	}
	list_add_tail(&drv->node, &hid_drivers);
	drv->registered = true;

	list_for_each_entry(hdev, &hid_devices, node) {
		if (!hdev->driver) {
			hid_device_probe(hdev, drv);
			continue;
		}
		/* A device the generic driver took because nothing better
		 * was registered yet goes to the new driver. */
		if (hdev->driver == &hid_generic_driver &&
		    drv != &hid_generic_driver && hid_match_device(hdev, drv)) {
			hid_device_remove(hdev);
			if (hid_device_probe(hdev, drv))
				hid_bind_device(hdev);
		}
	}
	mutex_unlock(&hid_bus_lock);
	return 0;
}

void hid_unregister_driver(struct hid_driver *drv)
{
	struct hid_device *hdev;

	mutex_lock(&hid_bus_lock);
	if (!drv->registered) {
		mutex_unlock(&hid_bus_lock);
		return;
	}
	list_for_each_entry(hdev, &hid_devices, node)
		if (hdev->driver == drv)
			hid_device_remove(hdev);
	list_del(&drv->node);
	drv->registered = false;
	/* What it had goes to whoever wants it now. */
	list_for_each_entry(hdev, &hid_devices, node)
		if (!hdev->driver)
			hid_bind_device(hdev);
	mutex_unlock(&hid_bus_lock);
}

// ---------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------

struct hid_device *hid_allocate_device(void)
{
	struct hid_device *hdev;

	might_sleep();
	hdev = kcalloc(1, sizeof(*hdev));
	if (!hdev)
		return NULL;
	for (unsigned int i = 0; i < HID_REPORT_TYPES; i++)
		INIT_LIST_HEAD(&hdev->report_enum[i].report_list);
	INIT_LIST_HEAD(&hdev->inputs);
	INIT_LIST_HEAD(&hdev->node);
	mutex_init(&hdev->driver_input_lock, "hid_driver_input");
	mutex_init(&hdev->ll_open_lock, "hid_ll_open");
	refcount_set(&hdev->ref, 1);
	return hdev;
}

void hid_get_device(struct hid_device *hdev)
{
	refcount_inc(&hdev->ref);
}

void hid_put_device(struct hid_device *hdev)
{
	if (!hdev || !refcount_dec_and_test(&hdev->ref))
		return;
	WARN_ON(hdev->status & HID_STAT_ADDED);
	hid_close_report(hdev);
	kfree(hdev->dev_rdesc);
	kfree(hdev);
}

int hid_add_device(struct hid_device *hdev)
{
	int ret;

	might_sleep();
	if (!hdev || WARN_ON(hdev->status & HID_STAT_ADDED))
		return -EBUSY;
	/* Names from the transport must be terminated strings. */
	hdev->name[sizeof(hdev->name) - 1] = 0;
	hdev->phys[sizeof(hdev->phys) - 1] = 0;
	hdev->uniq[sizeof(hdev->uniq) - 1] = 0;
	hdev->parent_path[sizeof(hdev->parent_path) - 1] = 0;

	hdev->quirks = hid_lookup_quirk(hdev);
	if (hid_ignore(hdev))
		return -ENODEV;

	if (!hdev->ll_driver || !hdev->ll_driver->raw_request ||
	    !hdev->ll_driver->parse || !hdev->ll_driver->start ||
	    !hdev->ll_driver->stop) {
		hid_err(hdev, "transport driver incomplete\n");
		return -EINVAL;
	}

	/* The descriptor, read once; every binding parses a copy. */
	ret = hdev->ll_driver->parse(hdev);
	if (ret)
		return ret;
	if (!hdev->dev_rdesc)
		return -ENODEV;

	if (hid_ignore_special_drivers) {
		hdev->group = HID_GROUP_GENERIC;
	} else if (!hdev->group &&
		   !(hdev->quirks & HID_QUIRK_HAVE_SPECIAL_DRIVER)) {
		ret = hid_scan_report(hdev);
		if (ret)
			hid_warn(hdev, "bad device descriptor (%d)\n", ret);
	}

	mutex_lock(&hid_bus_lock);
	hdev->id = ++hid_next_id;
	ksnprintf(hdev->dev_name, sizeof(hdev->dev_name), "%04X:%04X:%04X.%04X",
		  hdev->bus, hdev->vendor, hdev->product, hdev->id);
	if (hdev->parent_path[0])
		ksnprintf(hdev->devpath, sizeof(hdev->devpath), "%s/%s",
			  hdev->parent_path, hdev->dev_name);
	else
		hdev->devpath[0] = 0;
	list_add_tail(&hdev->node, &hid_devices);
	hdev->status |= HID_STAT_ADDED;
	hid_bind_device(hdev);
	if (!hdev->driver)
		hid_info(hdev, "no driver for the device (group %u)\n",
			 hdev->group);
	mutex_unlock(&hid_bus_lock);
	return 0;
}

void hid_destroy_device(struct hid_device *hdev)
{
	might_sleep();
	if (!hdev)
		return;
	mutex_lock(&hid_bus_lock);
	if (hdev->status & HID_STAT_ADDED) {
		hid_device_remove(hdev);
		list_del_init(&hdev->node);
		hdev->status &= ~HID_STAT_ADDED;
	}
	mutex_unlock(&hid_bus_lock);
	kfree(hdev->dev_rdesc);
	hdev->dev_rdesc = NULL;
	hdev->dev_rsize = 0;
	hid_put_device(hdev);
}

// ---------------------------------------------------------------------------
// Power management
// ---------------------------------------------------------------------------

int hid_driver_suspend(struct hid_device *hdev, int pm_event)
{
	int ret = 0;

	mutex_lock(&hid_bus_lock);
	if (hdev->driver && hdev->driver->suspend)
		ret = hdev->driver->suspend(hdev, pm_event);
	mutex_unlock(&hid_bus_lock);
	return ret;
}

int hid_driver_resume(struct hid_device *hdev)
{
	int ret = 0;

	mutex_lock(&hid_bus_lock);
	if (hdev->driver && hdev->driver->resume)
		ret = hdev->driver->resume(hdev);
	mutex_unlock(&hid_bus_lock);
	return ret;
}

int hid_driver_reset_resume(struct hid_device *hdev)
{
	int ret = 0;

	mutex_lock(&hid_bus_lock);
	if (hdev->driver && hdev->driver->reset_resume)
		ret = hdev->driver->reset_resume(hdev);
	mutex_unlock(&hid_bus_lock);
	return ret;
}

// ---------------------------------------------------------------------------
// The generic driver: every device no specific driver wants.
// ---------------------------------------------------------------------------

static bool hid_generic_match(struct hid_device *hdev,
			      bool ignore_special_driver)
{
	struct hid_driver *drv;

	if (ignore_special_driver)
		return true;
	if (hdev->quirks & HID_QUIRK_HAVE_SPECIAL_DRIVER)
		return false;
	/* hid_bus_lock is held by every caller of a match hook. */
	list_for_each_entry(drv, &hid_drivers, node)
		if (drv != &hid_generic_driver && hid_match_device(hdev, drv))
			return false;
	return true;
}

static int hid_generic_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	int ret;

	(void)id;
	/* One input device per application collection: a keyboard's
	 * consumer keys and a touchpad's mouse collection get their own. */
	hdev->quirks |= HID_QUIRK_INPUT_PER_APP;
	ret = hid_parse(hdev);
	if (ret)
		return ret;
	return hid_hw_start(hdev, HID_CONNECT_DEFAULT);
}

static const struct hid_device_id hid_generic_table[] = {
	{ HID_DEVICE(HID_BUS_ANY, HID_GROUP_ANY, HID_ANY_ID, HID_ANY_ID) },
	{ 0 }
};

struct hid_driver hid_generic_driver = {
	.name = "hid-generic",
	.id_table = hid_generic_table,
	.match = hid_generic_match,
	.probe = hid_generic_probe,
};

// ---------------------------------------------------------------------------

void hid_core_init(void)
{
	static DEFINE_MUTEX(init_lock);
	static bool done;

	mutex_lock(&init_lock);
	if (!done) {
		/* Specific drivers first, the generic one last. */
		if (hid_multitouch_init())
			kprintf("hid: hid-multitouch failed to register\n");
		if (hid_register_driver(&hid_generic_driver))
			kprintf("hid: hid-generic failed to register\n");
		done = true;
	}
	mutex_unlock(&init_lock);
}
