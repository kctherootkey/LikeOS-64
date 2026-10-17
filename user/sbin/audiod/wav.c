// audiod WAV reader for event sounds.
//
// RIFF/WAVE in memory: the chunk walk never trusts a size field further
// than the bytes actually present, odd-sized chunks are padded as the
// format requires, and the format chunk is checked for consistency
// (block alignment, bytes per second) before any sample is interpreted.
// Files come from a root-owned directory, but the parser is written as if
// they did not.
//
// Copyright (C) 2026 The LikeOS Project

#include <errno.h>
#include <string.h>

#include "dsp.h"
#include "parse.h"

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The tail shared by the KSDATAFORMAT_SUBTYPE_* GUIDs; the first two
 * bytes carry the plain format tag. */
static const uint8_t guid_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
				       0x80, 0x00, 0x00, 0xaa, 0x00, 0x38,
				       0x9b, 0x71 };

static int pick_format(uint16_t tag, uint32_t bits, uint32_t container)
{
	if (tag == WAVE_FORMAT_PCM) {
		switch (container) {
		case 8:
			return bits == 8 ? SF_U8 : SF_NONE;
		case 16:
			return bits <= 16 && bits > 8 ? SF_S16LE : SF_NONE;
		case 24:
			return bits <= 24 && bits > 16 ? SF_S24_3LE : SF_NONE;
		case 32:
			/* 24 valid bits in a 32-bit container are stored
			 * left-justified in WAV, i.e. exactly S32 */
			return bits <= 32 && bits > 16 ? SF_S32LE : SF_NONE;
		default:
			return SF_NONE;
		}
	}
	if (tag == WAVE_FORMAT_IEEE_FLOAT) {
		if (container == 32 && bits == 32)
			return SF_F32LE;
		if (container == 64 && bits == 64)
			return SF_F64LE;
	}
	return SF_NONE;
}

int wav_parse(const uint8_t *buf, size_t len, struct wav_info *out)
{
	memset(out, 0, sizeof(*out));
	if (!buf || len < 12 || len > WAV_MAX_BYTES)
		return -EINVAL;
	if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
		return -EINVAL;

	/* the RIFF size may claim more than the file has (truncated
	 * download) or less (junk appended); trust the smaller */
	size_t riff_end = (size_t)rd32(buf + 4) + 8;
	if (riff_end > len || riff_end < 12)
		riff_end = len;

	int have_fmt = 0;
	uint16_t tag = 0, channels = 0, block_align = 0, bits = 0;
	uint32_t rate = 0, byte_rate = 0, valid_bits = 0;
	const uint8_t *data = NULL;
	size_t data_len = 0;

	size_t off = 12;
	while (off + 8 <= riff_end) {
		const uint8_t *ck = buf + off;
		uint32_t ck_size = rd32(ck + 4);
		size_t body = off + 8;
		size_t avail = riff_end - body;
		size_t size = ck_size;

		if (memcmp(ck, "fmt ", 4) == 0) {
			if (have_fmt)
				return -EINVAL; /* two format chunks */
			if (size < 16 || size > avail)
				return -EINVAL;
			const uint8_t *f = buf + body;
			tag = rd16(f);
			channels = rd16(f + 2);
			rate = rd32(f + 4);
			byte_rate = rd32(f + 8);
			block_align = rd16(f + 12);
			bits = rd16(f + 14);
			valid_bits = bits;
			if (tag == WAVE_FORMAT_EXTENSIBLE) {
				if (size < 40)
					return -EINVAL;
				uint16_t cb = rd16(f + 16);
				if (cb < 22)
					return -EINVAL;
				valid_bits = rd16(f + 18);
				/* f + 20: channel mask, ignored: the
				 * standard order is assumed */
				const uint8_t *g = f + 24;
				if (memcmp(g + 2, guid_tail, sizeof(guid_tail)) != 0)
					return -ENOTSUP;
				tag = rd16(g);
				if (valid_bits == 0 || valid_bits > bits)
					valid_bits = bits;
			}
			have_fmt = 1;
		} else if (memcmp(ck, "data", 4) == 0) {
			if (!have_fmt)
				return -EINVAL; /* data before fmt */
			if (data)
				return -EINVAL; /* two data chunks */
			data = buf + body;
			/* a streamed file may carry 0 or 0xffffffff here;
			 * clip to what is present */
			data_len = size > avail ? avail : size;
		}

		/* next chunk: bodies are padded to an even length; stop
		 * when the claimed size runs off the end */
		if (size > avail)
			break;
		size_t next = body + size + (size & 1);
		if (next <= off)
			break; /* cannot happen; guards against wrap */
		off = next;
	}

	if (!have_fmt || !data)
		return -EINVAL;
	if (channels < 1 || channels > DSP_MAX_CH)
		return -ENOTSUP;
	if (rate < PROTO_MIN_RATE || rate > PROTO_MAX_RATE)
		return -ENOTSUP;
	if (bits == 0 || bits % 8 != 0 || bits > 64)
		return -ENOTSUP;

	int fmt = pick_format(tag, valid_bits, bits);
	if (fmt == SF_NONE)
		return -ENOTSUP;

	uint32_t frame = (uint32_t)(bits / 8) * channels;
	if (block_align != frame)
		return -EINVAL;
	/* byte_rate is advisory, but a wildly wrong one marks a broken
	 * writer; accept it only when consistent */
	if (byte_rate != 0 && byte_rate != frame * rate)
		return -EINVAL;

	out->fmt = fmt;
	out->rate = rate;
	out->channels = channels;
	out->block_align = block_align;
	out->bits = valid_bits;
	out->frames = data_len / frame;
	out->data_len = out->frames * frame;
	out->data = data;
	if (out->frames == 0)
		return -ENODATA;
	return 0;
}
