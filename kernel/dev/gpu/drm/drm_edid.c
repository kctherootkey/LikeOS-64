// LikeOS -- reading a display's EDID over DDC.
//
// The sink answers at I2C address 0x50; block 0 at offset 0, extension
// blocks after it, the E-DDC segment pointer (0x30) selecting each pair
// of blocks beyond the first two -- and written only for those, as some
// monitors that are not E-DDC compliant are upset by it.  A transfer is
// retried: bit-banged and slow DDC lines see spurious NAKs and timeouts
// under load, and a panel that has just been powered answers late.  A
// block that arrives damaged (bad checksum, a header with a byte or two
// wrong) is read again, and a base header that is mostly right is
// repaired.  An HDMI forum EEODB in the first extension can raise the
// block count past the 1 the base block then announces.  Extension blocks
// that stay invalid are dropped, the base block's count and checksum
// adjusted to match what is kept.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from Luc Verhaegen's, Intel's, Red Hat's and Dennis Munsie's code: MIT
// Portions Copyright (C) 2006 Luc Verhaegen (quirks list)
// Portions Copyright (C) 2007-2008 Intel Corporation
// Portions Copyright (C) 2010 Red Hat, Inc.
// Portions Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>

#include <kernel/dev/gpu/drm.h>
#include <kernel/dev/i2c.h>
#include <kernel/hal/lapic.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#include "drm_edid_internal.h"

#define DDC_SEGMENT_ADDR 0x30

/* A NAK (-ENXIO from the adapter) means nobody answers at the address:
 * stop retrying the transfer at once instead of spending the retries on
 * an empty DDC line.  0 retries a NAK like any other failure. */
#define DRM_EDID_DDC_STOP_ON_NAK 1

/* Transfer attempts per block, and the pause between two (ms). */
#define DDC_XFER_TRIES 5
#define DDC_RETRY_DELAY_MS 5

/* Reads of one block that arrives damaged before giving up on it. */
#define EDID_BLOCK_READ_TRIES 4

/*
 * Fetch `len' bytes of EDID from block `block' on: 0, or -1 on failure.
 */
static int drm_do_probe_ddc_edid(struct i2c_adapter *adapter, uint8_t *buf,
				 unsigned int block, size_t len)
{
	uint8_t start = (uint8_t)(block * EDID_LENGTH);
	uint8_t segment = (uint8_t)(block >> 1);
	int xfers = segment ? 3 : 2;
	int ret = -1, retries = DDC_XFER_TRIES;
	struct i2c_msg msgs[3];

	msgs[0].addr = DDC_SEGMENT_ADDR;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &segment;
	msgs[1].addr = DDC_ADDR;
	msgs[1].flags = 0;
	msgs[1].len = 1;
	msgs[1].buf = &start;
	msgs[2].addr = DDC_ADDR;
	msgs[2].flags = I2C_M_RD;
	msgs[2].len = (uint16_t)len;
	msgs[2].buf = buf;

	do {
		/*
		 * Avoid sending the segment addr to not upset non-compliant
		 * DDC monitors.
		 */
		ret = i2c_transfer(adapter, &msgs[3 - xfers], xfers);
		if (ret == xfers)
			break;

		if (DRM_EDID_DDC_STOP_ON_NAK && ret == -ENXIO)
			break;
		if (retries > 1)
			lapic_delay_ms(DDC_RETRY_DELAY_MS);
	} while (--retries);

	return ret == xfers ? 0 : -1;
}

/*
 * Read one block, again while it arrives damaged; a base block header
 * that is mostly right is repaired in place.
 */
static enum edid_block_status edid_block_read(void *block, unsigned int block_num,
					      struct i2c_adapter *adapter)
{
	enum edid_block_status status = EDID_BLOCK_READ_FAIL;
	bool is_base_block = block_num == 0;
	int try;

	for (try = 0; try < EDID_BLOCK_READ_TRIES; try++) {
		if (drm_do_probe_ddc_edid(adapter, block, block_num, EDID_LENGTH))
			return EDID_BLOCK_READ_FAIL;

		status = edid_block_check(block, is_base_block);
		if (status == EDID_BLOCK_HEADER_REPAIR) {
			edid_header_fix(block);

			/* Retry with fixed header, update status if that worked. */
			status = edid_block_check(block, is_base_block);
			if (status == EDID_BLOCK_OK)
				status = EDID_BLOCK_HEADER_FIXED;
		}

		if (edid_block_status_valid(status, edid_block_tag(block)))
			break;

		/* Fail early for unrepairable base block all zeros. */
		if (try == 0 && is_base_block && status == EDID_BLOCK_ZERO)
			break;
	}

	return status;
}

/* What a block that will not be used is reported as; quiet for the
 * expected cases (nothing connected, a CTA block with a bad checksum),
 * and only the first few times: a polled output with a broken EDID would
 * say the same on every probe. */
#define EDID_NOTICES_MAX 8

static void edid_block_status_print(enum edid_block_status status, int block_num,
				    const uint8_t *block)
{
	static int notices;

	if (notices >= EDID_NOTICES_MAX)
		return;
	if (status == EDID_BLOCK_ZERO || status == EDID_BLOCK_HEADER_CORRUPT ||
	    status == EDID_BLOCK_VERSION ||
	    (status == EDID_BLOCK_CHECKSUM &&
	     !edid_block_status_valid(status, edid_block_tag(block))))
		notices++;

	switch (status) {
	case EDID_BLOCK_OK:
	case EDID_BLOCK_READ_FAIL:
	case EDID_BLOCK_NULL:
	case EDID_BLOCK_HEADER_REPAIR:
	case EDID_BLOCK_HEADER_FIXED:
		break;
	case EDID_BLOCK_ZERO:
		kprintf("[drm] EDID block %d is all zeroes\n", block_num);
		break;
	case EDID_BLOCK_HEADER_CORRUPT:
		kprintf("[drm] EDID has corrupt header\n");
		break;
	case EDID_BLOCK_CHECKSUM:
		if (!edid_block_status_valid(status, edid_block_tag(block)))
			kprintf("[drm] EDID block %d (tag 0x%02x) checksum is invalid, remainder is %d\n",
				block_num, edid_block_tag(block),
				edid_block_compute_checksum(block));
		break;
	case EDID_BLOCK_VERSION:
		kprintf("[drm] EDID has major version %d, instead of 1\n", block[18]);
		break;
	default:
		break;
	}
}

/*
 * Read the EDID into `buf' (room for max_blocks blocks): the number of
 * blocks stored, 0 when the base block cannot be read (nothing connected),
 * or -EINVAL when what answers is not a usable EDID.  An EDID longer than
 * max_blocks keeps its first max_blocks blocks (and its own extension
 * count); an extension that cannot be read ends the read with what was
 * read so far.
 */
int drm_edid_read(struct i2c_adapter *adapter, uint8_t *buf, int max_blocks)
{
	enum edid_block_status status;
	struct edid *edid = (struct edid *)buf;
	int i, num_blocks, got, invalid_blocks = 0;

	if (!adapter || !buf || max_blocks < 1)
		return -EINVAL;

	status = edid_block_read(edid, 0, adapter);
	if (status == EDID_BLOCK_READ_FAIL)
		return 0;
	edid_block_status_print(status, 0, buf);
	if (!edid_block_status_valid(status, edid_block_tag(edid)))
		return -EINVAL;

	num_blocks = min(edid_block_count(edid), max_blocks);
	got = 1;
	for (i = 1; i < num_blocks; i++) {
		void *block = buf + i * EDID_LENGTH;

		status = edid_block_read(block, (unsigned int)i, adapter);
		if (status == EDID_BLOCK_READ_FAIL)
			break;
		got++;

		edid_block_status_print(status, i, block);
		if (!edid_block_status_valid(status, edid_block_tag(block))) {
			invalid_blocks++;
		} else if (i == 1) {
			/*
			 * If the first EDID extension is a CTA extension, and
			 * the first Data Block is HF-EEODB, override the
			 * extension block count (up to what fits).
			 */
			int eeodb = edid_hfeeodb_block_count(edid);

			if (eeodb > num_blocks)
				num_blocks = min(eeodb, max_blocks);
		}
	}

	if (invalid_blocks)
		got = edid_filter_invalid_blocks(edid, got);

	return got;
}
