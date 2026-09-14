// LikeOS-64 -- the GT's firmware images: GuC and HuC.
//
// The GuC is a controller in the GT that can own command submission and
// power management; the HuC authenticates media workloads.  On the
// platforms whose policy leaves both unloaded (i915_guc.c: Gen9 keeps
// the execution lists alone) the images are only looked for and
// described: the package that ships them is staged on the image, the
// loader below reads their headers, and the log says what would be
// used.
#include <kernel/dev/gpu/i915/i915_drv.h>
#include <kernel/io/console.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>

/* The controllers' firmware wrapper, by dword: module type, the header's
 * own size in dwords, header version, reserved, vendor, date, the whole
 * image in dwords, then the sizes of the signature key material, and --
 * past a build string -- the firmware version.  The header is much
 * larger than the display controller's: it carries the signature. */
#define UC_CSS_MODULE_TYPE_DW 0
#define UC_CSS_HEADER_SIZE_DW 1
#define UC_CSS_SIZE_DW 6
#define UC_CSS_KEY_SIZE_DW 7
#define UC_CSS_MODULUS_SIZE_DW 8
#define UC_CSS_EXPONENT_SIZE_DW 9
#define UC_CSS_SW_VERSION_DW 16
#define UC_CSS_MODULE_TYPE 6
#define UC_CSS_MIN_HEADER_DW 32
#define UC_CSS_MAX_HEADER_DW 256

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void describe(const char *what, const char *name)
{
	void *data = NULL;
	size_t len = 0;
	int rc = firmware_request(name, &data, &len);
	if (rc) {
		kprintf("[drm] i915: %s firmware %s not available (%d)\n", what, name, rc);
		return;
	}
	const uint8_t *css = data;
	uint32_t header_dw = len >= 8 ? rd32(css + UC_CSS_HEADER_SIZE_DW * 4) : 0;
	if (len < UC_CSS_MAX_HEADER_DW * 4 ||
	    rd32(css + UC_CSS_MODULE_TYPE_DW * 4) != UC_CSS_MODULE_TYPE ||
	    header_dw < UC_CSS_MIN_HEADER_DW || header_dw > UC_CSS_MAX_HEADER_DW) {
		kprintf("[drm] i915: %s firmware %s: not a firmware image (type %u, header %u dwords)\n",
			what, name, len >= 4 ? rd32(css) : 0, header_dw);
		firmware_release(data);
		return;
	}
	uint32_t size_dw = rd32(css + UC_CSS_SIZE_DW * 4);
	uint32_t key_dw = rd32(css + UC_CSS_KEY_SIZE_DW * 4);
	uint32_t modulus_dw = rd32(css + UC_CSS_MODULUS_SIZE_DW * 4);
	uint32_t exponent_dw = rd32(css + UC_CSS_EXPONENT_SIZE_DW * 4);
	uint32_t ver = rd32(css + UC_CSS_SW_VERSION_DW * 4);
	/* the image: header, then the code, then the signature key material */
	uint32_t overhead = (header_dw + key_dw + modulus_dw + exponent_dw) * 4;
	uint32_t ucode = size_dw * 4 > overhead ? size_dw * 4 - overhead : 0;
	i915_dbg("[drm] i915: %s firmware %s: version %u.%u.%u, %u bytes of code in %u bytes; not loaded (submission by execution lists)\n",
		what, name, (ver >> 16) & 0xff, (ver >> 8) & 0xff, ver & 0xff, ucode,
		(unsigned)len);
	firmware_release(data);
}

void i915_uc_firmware_describe(struct i915_device *i915)
{
	if ((i915->info->flags & I915_INFO_HAS_GUC) && i915->info->guc_fw)
		describe("GuC", i915->info->guc_fw);
	if ((i915->info->flags & I915_INFO_HAS_GUC) && i915->info->huc_fw)
		describe("HuC", i915->info->huc_fw);
}
