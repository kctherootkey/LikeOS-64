// LikeOS-64 -- reading a display's EDID over DDC.
//
// The sink answers at I2C address 0x50; block 0 at offset 0, extension
// blocks after it, the segment register (0x30) selecting each pair of
// blocks beyond the first two.  Retried: a panel that has just been
// powered answers late, and a monitor's DDC is slow.
#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/gpu/drm_edid.h>
#include <kernel/dev/i2c.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>

#define DDC_ADDR 0x50
#define DDC_SEGMENT_ADDR 0x30

static int edid_read_block(struct i2c_adapter *a, int block, uint8_t *out)
{
	uint8_t segment = (uint8_t)(block >> 1);
	uint8_t offset = (uint8_t)((block & 1) * DRM_EDID_BLOCK);
	struct i2c_msg msgs[3];
	int n = 0;

	if (segment) {
		msgs[n].addr = DDC_SEGMENT_ADDR;
		msgs[n].flags = 0;
		msgs[n].len = 1;
		msgs[n].buf = &segment;
		n++;
	}
	msgs[n].addr = DDC_ADDR;
	msgs[n].flags = 0;
	msgs[n].len = 1;
	msgs[n].buf = &offset;
	n++;
	msgs[n].addr = DDC_ADDR;
	msgs[n].flags = I2C_M_RD;
	msgs[n].len = DRM_EDID_BLOCK;
	msgs[n].buf = out;
	n++;
	for (int attempt = 0; attempt < 4; attempt++) {
		int rc = i2c_transfer(a, msgs, n);
		if (rc == n)
			return 0;
		lapic_delay_ms(5);
	}
	return -EIO;
}

int drm_edid_read(struct i2c_adapter *adapter, uint8_t *buf, int max_blocks)
{
	if (!adapter || !buf || max_blocks < 1)
		return -EINVAL;
	if (edid_read_block(adapter, 0, buf) != 0)
		return 0;
	if (!drm_edid_block_valid(buf) || buf[0] != 0x00 || buf[1] != 0xff)
		return -EINVAL;
	int ext = drm_edid_extensions(buf);
	int got = 1;
	for (int b = 1; b <= ext && b < max_blocks; b++) {
		if (edid_read_block(adapter, b, buf + b * DRM_EDID_BLOCK) != 0)
			break;
		got++;
	}
	return got;
}
