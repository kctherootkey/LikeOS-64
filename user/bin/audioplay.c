/* audioplay -- play sound files through the system audio service.
 *
 * Decodes the uncompressed and simply-coded formats itself and streams the
 * samples to audiod with libaudio (la_open / la_write / la_drain):
 *
 *   WAV      RIFF, RIFX (big-endian) and RF64; integer PCM 8/16/24/32 bit,
 *            IEEE float 32/64 bit, G.711 mu-law and A-law, IMA and
 *            Microsoft ADPCM, WAVE_FORMAT_EXTENSIBLE with any of those
 *   AU/SND   Sun/NeXT: linear 8/16/24/32 bit, float, double, mu-law, A-law
 *   AIFF     and AIFF-C (NONE, twos, sowt, raw, fl32, fl64, in24, in32,
 *            ulaw, alaw)
 *   CAF      Apple Core Audio, linear PCM, float, mu-law, A-law
 *   raw      headerless PCM described with -r/-c/-f
 *
 * from files or from standard input.  Compressed formats (MP3, Ogg Vorbis,
 * Opus, FLAC, AAC/M4A, WMA, ...) are handed to gst-play-1.0 when GStreamer
 * is installed; a message says what to do when it is not.
 *
 * Only byte-order and representation are changed here (swaps, sign flips,
 * G.711 and ADPCM expansion).  audiod does the real format conversion,
 * resampling and channel mapping, so a 24-bit 96 kHz file reaches the
 * mixer with its full resolution.
 *
 * Ctrl-C stops cleanly: what is buffered is dropped, the stream is closed
 * and the program exits with status 130.  A second Ctrl-C exits at once.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <audio/audio.h>

#define READ_CHUNK (32 * 1024) /* input bytes per la_write round */
#define RDBUF_SIZE (64 * 1024)
#define MAX_CHANNELS 8
#define MIN_RATE 4000
#define MAX_RATE 384000
#define META_LEN 160
#define UNKNOWN_SIZE UINT64_MAX

static const char *prog = "audioplay";

/* ---- options --------------------------------------------------------------- */

static struct {
	int quiet;
	int progress;
	int list_only;
	int force_raw;
	int no_gst;
	int have_volume;
	uint32_t volume;
	uint32_t role;
	uint32_t buffer_ms;
	uint32_t loops; /* times to play, 0 = forever */
	uint64_t start_ms;
	uint64_t duration_ms; /* 0 = to the end */
	const char *stream_name;
	/* raw */
	int raw_given;
	uint32_t raw_rate, raw_channels;
	int raw_enc; /* enum enc, -1 = default */
} opt = {
	.loops = 1,
	.raw_enc = -1,
};

static volatile sig_atomic_t interrupted;

/* ---- sample encodings in files ----------------------------------------------- */

enum enc {
	E_NONE = 0,
	E_U8, E_S8,
	E_S16LE, E_S16BE, E_U16LE, E_U16BE,
	E_S24LE, E_S24BE, /* packed 3 bytes */
	E_S24LE32, /* 24 bits in the low bits of a 32-bit LE word */
	E_S32LE, E_S32BE,
	E_F32LE, E_F32BE,
	E_F64LE, E_F64BE,
	E_ULAW, E_ALAW,
	E_IMA_ADPCM, E_MS_ADPCM,
	E_COUNT
};

static const struct enc_desc {
	const char *name; /* raw -f name */
	const char *text; /* description */
	uint32_t bytes; /* bytes per sample in the file (0: block codec) */
	uint32_t out_fmt; /* AUD_FMT_* sent to audiod */
	uint32_t out_bytes;
} encs[E_COUNT] = {
	[E_NONE] = { "none", "none", 0, 0, 0 },
	[E_U8] = { "u8", "unsigned 8-bit PCM", 1, AUD_FMT_U8, 1 },
	[E_S8] = { "s8", "signed 8-bit PCM", 1, AUD_FMT_U8, 1 },
	[E_S16LE] = { "s16le", "signed 16-bit little-endian PCM", 2, AUD_FMT_S16_LE, 2 },
	[E_S16BE] = { "s16be", "signed 16-bit big-endian PCM", 2, AUD_FMT_S16_BE, 2 },
	[E_U16LE] = { "u16le", "unsigned 16-bit little-endian PCM", 2, AUD_FMT_S16_LE, 2 },
	[E_U16BE] = { "u16be", "unsigned 16-bit big-endian PCM", 2, AUD_FMT_S16_BE, 2 },
	[E_S24LE] = { "s24le", "signed 24-bit little-endian PCM", 3, AUD_FMT_S24_3LE, 3 },
	[E_S24BE] = { "s24be", "signed 24-bit big-endian PCM", 3, AUD_FMT_S24_3LE, 3 },
	[E_S24LE32] = { "s24_32le", "signed 24-bit PCM in 32-bit little-endian words", 4, AUD_FMT_S24_LE, 4 },
	[E_S32LE] = { "s32le", "signed 32-bit little-endian PCM", 4, AUD_FMT_S32_LE, 4 },
	[E_S32BE] = { "s32be", "signed 32-bit big-endian PCM", 4, AUD_FMT_S32_LE, 4 },
	[E_F32LE] = { "f32le", "32-bit little-endian float", 4, AUD_FMT_F32_LE, 4 },
	[E_F32BE] = { "f32be", "32-bit big-endian float", 4, AUD_FMT_F32_LE, 4 },
	[E_F64LE] = { "f64le", "64-bit little-endian float", 8, AUD_FMT_F64_LE, 8 },
	[E_F64BE] = { "f64be", "64-bit big-endian float", 8, AUD_FMT_F64_LE, 8 },
	[E_ULAW] = { "ulaw", "G.711 mu-law", 1, AUD_FMT_S16_LE, 2 },
	[E_ALAW] = { "alaw", "G.711 A-law", 1, AUD_FMT_S16_LE, 2 },
	[E_IMA_ADPCM] = { "ima-adpcm", "IMA ADPCM (4 bit)", 0, AUD_FMT_S16_LE, 2 },
	[E_MS_ADPCM] = { "ms-adpcm", "Microsoft ADPCM (4 bit)", 0, AUD_FMT_S16_LE, 2 },
};

static const struct {
	const char *alias;
	enum enc e;
} enc_aliases[] = {
	{ "u8", E_U8 }, { "s8", E_S8 },
	{ "s16", E_S16LE }, { "s16le", E_S16LE }, { "s16_le", E_S16LE },
	{ "s16be", E_S16BE }, { "s16_be", E_S16BE },
	{ "u16", E_U16LE }, { "u16le", E_U16LE }, { "u16_le", E_U16LE },
	{ "u16be", E_U16BE }, { "u16_be", E_U16BE },
	{ "s24", E_S24LE }, { "s24le", E_S24LE }, { "s24_3le", E_S24LE },
	{ "s24be", E_S24BE }, { "s24_3be", E_S24BE },
	{ "s24_32", E_S24LE32 }, { "s24_32le", E_S24LE32 }, { "s24_le", E_S24LE32 },
	{ "s32", E_S32LE }, { "s32le", E_S32LE }, { "s32_le", E_S32LE },
	{ "s32be", E_S32BE }, { "s32_be", E_S32BE },
	{ "f32", E_F32LE }, { "f32le", E_F32LE }, { "float", E_F32LE },
	{ "float_le", E_F32LE }, { "f32be", E_F32BE }, { "float_be", E_F32BE },
	{ "f64", E_F64LE }, { "f64le", E_F64LE }, { "double", E_F64LE },
	{ "float64_le", E_F64LE }, { "f64be", E_F64BE }, { "double_be", E_F64BE },
	{ "ulaw", E_ULAW }, { "mulaw", E_ULAW }, { "mu-law", E_ULAW }, { "u-law", E_ULAW },
	{ "alaw", E_ALAW }, { "a-law", E_ALAW },
};

static int parse_enc(const char *s)
{
	size_t i;
	uint32_t f;

	for (i = 0; i < sizeof(enc_aliases) / sizeof(enc_aliases[0]); i++)
		if (!strcasecmp(s, enc_aliases[i].alias))
			return enc_aliases[i].e;
	/* The service's own names ("S16_LE", "F32_LE", ...) as a fallback. */
	if (la_parse_format(s, &f) >= 0) {
		switch (f) {
		case AUD_FMT_U8: return E_U8;
		case AUD_FMT_S16_LE: return E_S16LE;
		case AUD_FMT_S16_BE: return E_S16BE;
		case AUD_FMT_S24_LE: return E_S24LE32;
		case AUD_FMT_S24_3LE: return E_S24LE;
		case AUD_FMT_S32_LE: return E_S32LE;
		case AUD_FMT_F32_LE: return E_F32LE;
		case AUD_FMT_F64_LE: return E_F64LE;
		}
	}
	return -1;
}

/* ---- the description of one input --------------------------------------------- */

struct src {
	const char *path; /* as given ("-" = stdin) */
	char display[96]; /* printable name */
	const char *container; /* "WAV", "Sun AU", ... */
	char container_detail[64];
	enum enc enc;
	uint32_t rate, channels;
	uint32_t valid_bits; /* meaningful bits per sample, 0 = all */
	uint32_t channel_mask; /* WAV speaker mask, 0 = default order */
	/* block codecs (ADPCM): bytes per block and frames it decodes to */
	uint32_t block_align;
	uint32_t frames_per_block;
	/* MS ADPCM coefficient table */
	int ms_ncoef;
	int16_t ms_coef[32][2];
	/* where the samples are */
	uint64_t data_offset;
	uint64_t data_bytes; /* UNKNOWN_SIZE when streaming */
	uint64_t total_frames; /* UNKNOWN_SIZE when not known */
	uint64_t fact_frames; /* WAV 'fact' sample length, 0 = none */
	/* metadata, already made printable */
	char title[META_LEN], artist[META_LEN], album[META_LEN];
	char date[META_LEN], comment[META_LEN], genre[META_LEN];
	char software[META_LEN], copyright[META_LEN];
};

/* ---- buffered reader ------------------------------------------------------------ */

struct reader {
	int fd;
	int seekable;
	uint64_t file_size; /* 0 when unknown */
	uint64_t pos; /* file offset of buf[rpos] */
	uint8_t *buf;
	size_t rpos, rlen;
	int eof, error;
};

static int rd_init(struct reader *r, int fd)
{
	struct stat st;

	memset(r, 0, sizeof(*r));
	r->fd = fd;
	r->buf = malloc(RDBUF_SIZE);
	if (!r->buf)
		return -ENOMEM;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
		r->file_size = (uint64_t)st.st_size;
		if (lseek(fd, 0, SEEK_CUR) >= 0)
			r->seekable = 1;
	}
	return 0;
}

static void rd_free(struct reader *r)
{
	free(r->buf);
	r->buf = NULL;
}

/* Make at least `want' bytes available in the buffer (fewer at EOF). */
static size_t rd_fill(struct reader *r, size_t want)
{
	if (want > RDBUF_SIZE)
		want = RDBUF_SIZE;
	if (r->rlen - r->rpos >= want)
		return r->rlen - r->rpos;
	if (r->rpos) {
		memmove(r->buf, r->buf + r->rpos, r->rlen - r->rpos);
		r->rlen -= r->rpos;
		r->rpos = 0;
	}
	while (r->rlen < want && !r->eof && !r->error) {
		ssize_t n = read(r->fd, r->buf + r->rlen, RDBUF_SIZE - r->rlen);
		if (n < 0) {
			if (errno == EINTR) {
				if (interrupted)
					break;
				continue;
			}
			r->error = errno;
			break;
		}
		if (n == 0) {
			r->eof = 1;
			break;
		}
		r->rlen += (size_t)n;
	}
	return r->rlen - r->rpos;
}

static const uint8_t *rd_peek(struct reader *r, size_t n, size_t *got)
{
	*got = rd_fill(r, n);
	return r->buf + r->rpos;
}

static size_t rd_read(struct reader *r, void *dst, size_t n)
{
	size_t done = 0;

	while (done < n) {
		size_t avail = rd_fill(r, n - done < RDBUF_SIZE ? n - done : RDBUF_SIZE);
		size_t take;
		if (avail == 0)
			break;
		take = avail < n - done ? avail : n - done;
		memcpy((uint8_t *)dst + done, r->buf + r->rpos, take);
		r->rpos += take;
		r->pos += take;
		done += take;
	}
	return done;
}

/* Skip forward `n' bytes: a seek where possible, a read otherwise. */
static int rd_skip(struct reader *r, uint64_t n)
{
	size_t inbuf = r->rlen - r->rpos;

	if (n <= inbuf) {
		r->rpos += (size_t)n;
		r->pos += n;
		return 0;
	}
	if (r->seekable) {
		uint64_t target = r->pos + n;
		if (target > (uint64_t)INT64_MAX)
			return -EINVAL;
		if (lseek(r->fd, (off_t)target, SEEK_SET) < 0)
			return -errno;
		r->rpos = r->rlen = 0;
		r->pos = target;
		r->eof = 0;
		return 0;
	}
	while (n) {
		size_t avail = rd_fill(r, RDBUF_SIZE);
		size_t take;
		if (avail == 0)
			return r->error ? -r->error : -EIO;
		take = avail < n ? avail : (size_t)n;
		r->rpos += take;
		r->pos += take;
		n -= take;
		if (interrupted)
			return -EINTR;
	}
	return 0;
}

static int rd_seek(struct reader *r, uint64_t off)
{
	/* Inside what is buffered, or forward on a pipe: skip. */
	if (off >= r->pos && (off - r->pos <= r->rlen - r->rpos || !r->seekable))
		return rd_skip(r, off - r->pos);
	if (!r->seekable)
		return -ESPIPE;
	if (off > (uint64_t)INT64_MAX)
		return -EINVAL;
	if (lseek(r->fd, (off_t)off, SEEK_SET) < 0)
		return -errno;
	r->rpos = r->rlen = 0;
	r->pos = off;
	r->eof = 0;
	return 0;
}

/* ---- byte helpers ------------------------------------------------------------- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static uint64_t be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

/* Copy metadata text out of a file: at most dstsz-1 bytes, stopped at NUL,
 * control characters and invalid bytes replaced, trailing blanks cut.  The
 * text comes from an untrusted file and ends up on a terminal. */
static void meta_copy(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
	size_t i, o = 0;

	for (i = 0; i < n && o + 1 < dstsz; i++) {
		uint8_t c = src[i];
		if (c == 0)
			break;
		if (c < 0x20 || c == 0x7f)
			c = ' ';
		dst[o++] = (char)c;
	}
	while (o > 0 && dst[o - 1] == ' ')
		o--;
	dst[o] = 0;
}

static void sanitize_name(char *dst, size_t dstsz, const char *src)
{
	meta_copy(dst, dstsz, (const uint8_t *)src, strlen(src));
}

/* An 80-bit IEEE extended (AIFF sample rate) to an integer, rounded.
 * Exponent bias 16383, explicit integer bit at the top of the mantissa. */
static uint32_t ext80_to_u32(const uint8_t *p)
{
	int sign = p[0] & 0x80;
	int exp = ((p[0] & 0x7f) << 8) | p[1];
	uint64_t mant = be64(p + 2);
	int shift;

	if (sign || mant == 0 || exp == 0x7fff)
		return 0;
	shift = exp - 16383 - 63; /* value = mant * 2^shift */
	if (shift >= 0)
		return 0; /* far beyond any sample rate */
	if (shift <= -64)
		return 0;
	/* round half up */
	mant = (mant >> (-shift - 1));
	mant = (mant + 1) >> 1;
	return mant > UINT32_MAX ? 0 : (uint32_t)mant;
}

/* IEEE double, big-endian bytes, to an integer (CAF sample rate) without
 * floating-point parsing tricks: exponent bias 1023, 52-bit fraction. */
static uint32_t f64be_to_u32(const uint8_t *p)
{
	uint64_t bits = be64(p);
	int exp = (int)((bits >> 52) & 0x7ff);
	uint64_t mant = bits & ((1ull << 52) - 1);
	int shift;

	if ((bits >> 63) || exp == 0 || exp == 0x7ff)
		return 0;
	mant |= 1ull << 52;
	shift = exp - 1023 - 52;
	if (shift >= 0)
		return 0; /* 2^52 Hz and up: nonsense */
	if (shift <= -63)
		return 0;
	mant >>= (-shift - 1);
	mant = (mant + 1) >> 1;
	return mant > UINT32_MAX ? 0 : (uint32_t)mant;
}

/* ---- G.711 tables ----------------------------------------------------------- */

static int16_t ulaw_tab[256], alaw_tab[256];

static int16_t ulaw_decode(uint8_t u)
{
	int t;

	u = (uint8_t)~u;
	t = ((u & 0x0f) << 3) + 0x84; /* mantissa plus bias */
	t <<= (u & 0x70) >> 4; /* segment */
	return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

static int16_t alaw_decode(uint8_t a)
{
	int t, seg;

	a ^= 0x55; /* even bits are inverted on the wire */
	t = (a & 0x0f) << 4;
	seg = (a & 0x70) >> 4;
	if (seg == 0)
		t += 8;
	else if (seg == 1)
		t += 0x108;
	else
		t = (t + 0x108) << (seg - 1);
	return (int16_t)((a & 0x80) ? t : -t);
}

static void g711_init(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		ulaw_tab[i] = ulaw_decode((uint8_t)i);
		alaw_tab[i] = alaw_decode((uint8_t)i);
	}
}

/* ---- ADPCM ------------------------------------------------------------------ */

static const int16_t ima_steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
	41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
	190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
	724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
	2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
	6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
	16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t ima_index_adj[16] = {
	-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static int clamp16(int v)
{
	return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static int ima_nibble(int *pred, int *index, int nib)
{
	int step = ima_steps[*index];
	int diff = step >> 3;

	if (nib & 1)
		diff += step >> 2;
	if (nib & 2)
		diff += step >> 1;
	if (nib & 4)
		diff += step;
	if (nib & 8)
		*pred = clamp16(*pred - diff);
	else
		*pred = clamp16(*pred + diff);
	*index += ima_index_adj[nib & 15];
	if (*index < 0)
		*index = 0;
	if (*index > 88)
		*index = 88;
	return *pred;
}

/* One IMA ADPCM block (WAV layout) of `len' bytes -> interleaved S16.
 * Header per channel: 16-bit predictor, 8-bit step index, 1 reserved byte.
 * Then for each channel in turn 4 bytes = 8 samples, low nibble first.
 * Returns the frames produced (a short last block decodes partially). */
static uint32_t ima_block(const struct src *s, const uint8_t *in, size_t len,
			  int16_t *out)
{
	uint32_t ch = s->channels, c, frames = 0, maxf = s->frames_per_block;
	int pred[MAX_CHANNELS], idx[MAX_CHANNELS];
	size_t p;

	if (len < 4u * ch)
		return 0;
	for (c = 0; c < ch; c++) {
		pred[c] = (int16_t)le16(in + 4 * c);
		idx[c] = in[4 * c + 2];
		if (idx[c] > 88)
			idx[c] = 88;
		out[c] = (int16_t)pred[c];
	}
	frames = 1;
	p = 4u * ch;
	/* Each round: 4 bytes per channel -> 8 frames. */
	while (p + 4u * ch <= len && frames + 8 <= maxf + 0u) {
		for (c = 0; c < ch; c++) {
			const uint8_t *b = in + p + 4 * c;
			uint32_t k;
			for (k = 0; k < 8; k++) {
				int nib = (b[k >> 1] >> ((k & 1) * 4)) & 15;
				out[(frames + k) * ch + c] =
					(int16_t)ima_nibble(&pred[c], &idx[c], nib);
			}
		}
		frames += 8;
		p += 4u * ch;
	}
	return frames > maxf ? maxf : frames;
}

static const int ms_adapt[16] = {
	230, 230, 230, 230, 307, 409, 512, 614,
	768, 614, 512, 409, 307, 230, 230, 230
};

static const int16_t ms_default_coef[7][2] = {
	{ 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 },
	{ 240, 0 }, { 460, -208 }, { 392, -232 }
};

struct ms_chan {
	int c1, c2, delta, s1, s2;
};

static int16_t ms_nibble(struct ms_chan *m, int nib)
{
	int sn = nib >= 8 ? nib - 16 : nib; /* signed 4 bit */
	int pred = (m->s1 * m->c1 + m->s2 * m->c2) / 256;

	pred = clamp16(pred + sn * m->delta);
	m->s2 = m->s1;
	m->s1 = pred;
	m->delta = (ms_adapt[nib] * m->delta) / 256;
	if (m->delta < 16)
		m->delta = 16;
	return (int16_t)pred;
}

/* One Microsoft ADPCM block.  Header (each field for all channels in
 * turn): predictor index (1 byte), delta (16 bit), sample1, sample2; the
 * two header samples come out oldest (sample2) first.  The rest are
 * nibbles, high one first; in stereo the high nibble is the left channel
 * and the low one the right. */
static uint32_t msadpcm_block(const struct src *s, const uint8_t *in, size_t len,
			      int16_t *out)
{
	uint32_t ch = s->channels, c, frames, maxf = s->frames_per_block;
	struct ms_chan m[2];
	size_t p, hdr = 7u * ch;

	if (ch < 1 || ch > 2 || len < hdr || maxf < 2)
		return 0;
	for (c = 0; c < ch; c++) {
		int pi = in[c];
		if (pi >= s->ms_ncoef)
			pi = 0;
		m[c].c1 = s->ms_coef[pi][0];
		m[c].c2 = s->ms_coef[pi][1];
		m[c].delta = (int16_t)le16(in + ch + 2 * c);
		m[c].s1 = (int16_t)le16(in + 3 * ch + 2 * c);
		m[c].s2 = (int16_t)le16(in + 5 * ch + 2 * c);
		out[c] = (int16_t)m[c].s2;
		out[ch + c] = (int16_t)m[c].s1;
	}
	frames = 2;
	for (p = hdr; p < len && frames < maxf; p++) {
		int hi = in[p] >> 4, lo = in[p] & 15;
		if (ch == 2) {
			out[frames * 2] = ms_nibble(&m[0], hi);
			out[frames * 2 + 1] = ms_nibble(&m[1], lo);
			frames++;
		} else {
			out[frames++] = ms_nibble(&m[0], hi);
			if (frames < maxf)
				out[frames++] = ms_nibble(&m[0], lo);
		}
	}
	return frames;
}

/* ---- format conversion (to something audiod accepts) ------------------------- */

static uint32_t in_frame_bytes(const struct src *s)
{
	return encs[s->enc].bytes * s->channels;
}

static uint32_t out_frame_bytes(const struct src *s)
{
	return encs[s->enc].out_bytes * s->channels;
}

static int is_block_codec(const struct src *s)
{
	return s->enc == E_IMA_ADPCM || s->enc == E_MS_ADPCM;
}

/* In-unit: a frame for PCM, a block for ADPCM. */
static uint32_t unit_in_bytes(const struct src *s)
{
	return is_block_codec(s) ? s->block_align : in_frame_bytes(s);
}

static uint32_t unit_frames(const struct src *s)
{
	return is_block_codec(s) ? s->frames_per_block : 1;
}

static void swap_bytes(uint8_t *p, size_t n, uint32_t width)
{
	size_t i;

	for (i = 0; i + width <= n; i += width) {
		uint32_t a, b;
		for (a = 0, b = width - 1; a < b; a++, b--) {
			uint8_t t = p[i + a];
			p[i + a] = p[i + b];
			p[i + b] = t;
		}
	}
}

/* Convert `n' input bytes (whole units) into `out'; returns output bytes. */
static size_t convert(const struct src *s, uint8_t *in, size_t n, uint8_t *out)
{
	size_t i;

	switch (s->enc) {
	case E_U8: case E_S16LE: case E_S16BE: case E_S24LE: case E_S24LE32:
	case E_S32LE: case E_F32LE: case E_F64LE:
		memcpy(out, in, n);
		return n;
	case E_S8:
		for (i = 0; i < n; i++)
			out[i] = in[i] ^ 0x80;
		return n;
	case E_U16LE:
		for (i = 0; i + 1 < n; i += 2) {
			out[i] = in[i];
			out[i + 1] = in[i + 1] ^ 0x80;
		}
		return n;
	case E_U16BE:
		for (i = 0; i + 1 < n; i += 2) {
			out[i] = in[i] ^ 0x80;
			out[i + 1] = in[i + 1];
		}
		return n;
	case E_S24BE:
		memcpy(out, in, n);
		swap_bytes(out, n, 3);
		return n;
	case E_S32BE: case E_F32BE:
		memcpy(out, in, n);
		swap_bytes(out, n, 4);
		return n;
	case E_F64BE:
		memcpy(out, in, n);
		swap_bytes(out, n, 8);
		return n;
	case E_ULAW: case E_ALAW: {
		const int16_t *tab = s->enc == E_ULAW ? ulaw_tab : alaw_tab;
		for (i = 0; i < n; i++) {
			uint16_t v = (uint16_t)tab[in[i]];
			out[2 * i] = (uint8_t)v;
			out[2 * i + 1] = (uint8_t)(v >> 8);
		}
		return 2 * n;
	}
	case E_IMA_ADPCM: case E_MS_ADPCM: {
		size_t o = 0, p = 0, ob = out_frame_bytes(s);
		int16_t *tmp = malloc((size_t)s->frames_per_block * s->channels * 2);
		if (!tmp)
			return 0;
		while (p < n) {
			size_t len = n - p < s->block_align ? n - p : s->block_align;
			uint32_t f = s->enc == E_IMA_ADPCM ?
					     ima_block(s, in + p, len, tmp) :
					     msadpcm_block(s, in + p, len, tmp);
			memcpy(out + o, tmp, f * ob);
			o += f * ob;
			p += len;
		}
		free(tmp);
		return o;
	}
	default:
		return 0;
	}
}

/* ---- format detection --------------------------------------------------------- */

enum kind {
	K_UNKNOWN, K_WAV, K_RIFX, K_RF64, K_W64, K_AU, K_AIFF, K_CAF,
	K_MP3, K_OGG, K_FLAC, K_MP4, K_MKV, K_ASF, K_AAC, K_MIDI, K_AC3,
	K_AMR, K_APE, K_WV, K_RIFF_OTHER, K_VIDEO, K_WRAPPED
};

static const char *kind_text(enum kind k)
{
	switch (k) {
	case K_MP3: return "MPEG audio (MP3)";
	case K_OGG: return "Ogg (Vorbis, Opus or FLAC)";
	case K_FLAC: return "FLAC";
	case K_MP4: return "MPEG-4 (AAC/ALAC, M4A)";
	case K_MKV: return "Matroska/WebM";
	case K_ASF: return "ASF (WMA)";
	case K_AAC: return "AAC (ADTS)";
	case K_MIDI: return "MIDI";
	case K_AC3: return "AC-3";
	case K_AMR: return "AMR";
	case K_APE: return "Monkey's Audio";
	case K_WV: return "WavPack";
	case K_W64: return "Wave64";
	case K_RIFF_OTHER: return "RIFF (not WAVE)";
	case K_VIDEO: return "video container";
	case K_WRAPPED: return "compressed audio in a WAV/CAF container";
	default: return "unknown";
	}
}

static enum kind sniff(const uint8_t *p, size_t n)
{
	if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4))
		return K_WAV;
	if (n >= 12 && !memcmp(p, "RIFX", 4) && !memcmp(p + 8, "WAVE", 4))
		return K_RIFX;
	if (n >= 12 && !memcmp(p, "RF64", 4) && !memcmp(p + 8, "WAVE", 4))
		return K_RF64;
	if (n >= 12 && !memcmp(p, "BW64", 4) && !memcmp(p + 8, "WAVE", 4))
		return K_RF64;
	if (n >= 16 && !memcmp(p, "riff\x2e\x91\xcf\x11", 8))
		return K_W64;
	if (n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "AVI ", 4))
		return K_VIDEO;
	if (n >= 12 && !memcmp(p, "RIFF", 4))
		return K_RIFF_OTHER;
	if (n >= 4 && !memcmp(p, ".snd", 4))
		return K_AU;
	if (n >= 12 && !memcmp(p, "FORM", 4) &&
	    (!memcmp(p + 8, "AIFF", 4) || !memcmp(p + 8, "AIFC", 4)))
		return K_AIFF;
	if (n >= 8 && !memcmp(p, "caff", 4))
		return K_CAF;
	if (n >= 4 && !memcmp(p, "OggS", 4))
		return K_OGG;
	if (n >= 4 && !memcmp(p, "fLaC", 4))
		return K_FLAC;
	if (n >= 3 && !memcmp(p, "ID3", 3))
		return K_MP3;
	if (n >= 8 && !memcmp(p + 4, "ftyp", 4))
		return K_MP4;
	if (n >= 4 && p[0] == 0x1a && p[1] == 0x45 && p[2] == 0xdf && p[3] == 0xa3)
		return K_MKV;
	if (n >= 4 && p[0] == 0x30 && p[1] == 0x26 && p[2] == 0xb2 && p[3] == 0x75)
		return K_ASF;
	if (n >= 4 && !memcmp(p, "MThd", 4))
		return K_MIDI;
	if (n >= 5 && !memcmp(p, "#!AMR", 5))
		return K_AMR;
	if (n >= 4 && !memcmp(p, "MAC ", 4))
		return K_APE;
	if (n >= 4 && !memcmp(p, "wvpk", 4))
		return K_WV;
	if (n >= 2 && p[0] == 0x0b && p[1] == 0x77)
		return K_AC3;
	if (n >= 2 && p[0] == 0xff && (p[1] & 0xf6) == 0xf0)
		return K_AAC; /* ADTS: sync 0xFFF, layer 00 */
	if (n >= 2 && p[0] == 0xff && (p[1] & 0xe0) == 0xe0)
		return K_MP3; /* MPEG audio frame sync */
	if (n >= 4 && be32(p) == 0x000001ba)
		return K_VIDEO; /* MPEG program stream */
	if (n >= 1 && p[0] == 0x47 && n >= 189 && p[188] == 0x47)
		return K_VIDEO; /* MPEG transport stream */
	return K_UNKNOWN;
}

static enum kind kind_from_extension(const char *path)
{
	static const struct {
		const char *ext;
		enum kind k;
	} map[] = {
		{ "mp3", K_MP3 }, { "mp2", K_MP3 }, { "ogg", K_OGG }, { "oga", K_OGG },
		{ "opus", K_OGG }, { "flac", K_FLAC }, { "m4a", K_MP4 },
		{ "m4b", K_MP4 }, { "mp4", K_MP4 }, { "aac", K_AAC },
		{ "mka", K_MKV }, { "mkv", K_MKV }, { "webm", K_MKV },
		{ "wma", K_ASF }, { "asf", K_ASF }, { "mid", K_MIDI },
		{ "midi", K_MIDI }, { "ac3", K_AC3 }, { "amr", K_AMR },
		{ "ape", K_APE }, { "wv", K_WV }, { "avi", K_VIDEO },
		{ "mov", K_VIDEO }, { "ts", K_VIDEO }, { "mpg", K_VIDEO },
		{ "mpeg", K_VIDEO },
	};
	const char *dot = strrchr(path, '.');
	size_t i;

	if (!dot || strchr(dot, '/'))
		return K_UNKNOWN;
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcasecmp(dot + 1, map[i].ext))
			return map[i].k;
	return K_UNKNOWN;
}

/* ---- WAV / RIFX / RF64 ---------------------------------------------------------- */

static const char *wav_tag_name(uint32_t tag)
{
	switch (tag) {
	case 0x0001: return "PCM";
	case 0x0002: return "Microsoft ADPCM";
	case 0x0003: return "IEEE float";
	case 0x0006: return "A-law";
	case 0x0007: return "mu-law";
	case 0x0011: return "IMA ADPCM";
	case 0x0031: return "GSM 6.10";
	case 0x0050: return "MPEG";
	case 0x0055: return "MPEG layer 3";
	case 0x00ff: return "AAC";
	case 0x0161: case 0x0162: case 0x0163: return "WMA";
	case 0x2000: return "AC-3";
	case 0xf1ac: return "FLAC";
	case 0xfffe: return "extensible";
	default: return "unknown";
	}
}

/* Pick the encoding for a WAV format tag and container size. */
static int wav_choose_enc(struct src *s, uint32_t tag, uint32_t bits, int big)
{
	switch (tag) {
	case 0x0001: /* integer PCM; 8-bit is unsigned in WAV */
		switch (bits) {
		case 8: s->enc = E_U8; return 0;
		case 16: s->enc = big ? E_S16BE : E_S16LE; return 0;
		case 24: s->enc = big ? E_S24BE : E_S24LE; return 0;
		case 32: s->enc = big ? E_S32BE : E_S32LE; return 0;
		}
		return -1;
	case 0x0003:
		if (bits == 32) {
			s->enc = big ? E_F32BE : E_F32LE;
			return 0;
		}
		if (bits == 64) {
			s->enc = big ? E_F64BE : E_F64LE;
			return 0;
		}
		return -1;
	case 0x0006:
		if (bits != 8)
			return -1;
		s->enc = E_ALAW;
		return 0;
	case 0x0007:
		if (bits != 8)
			return -1;
		s->enc = E_ULAW;
		return 0;
	case 0x0011:
		if (bits != 4 || big)
			return -1;
		s->enc = E_IMA_ADPCM;
		return 0;
	case 0x0002:
		if (bits != 4 || big)
			return -1;
		s->enc = E_MS_ADPCM;
		return 0;
	}
	return -1;
}

static void wav_info_list(struct src *s, const uint8_t *p, size_t n, int big)
{
	size_t off = 4; /* after "INFO" */

	if (n < 4 || memcmp(p, "INFO", 4))
		return;
	while (off + 8 <= n) {
		const uint8_t *id = p + off;
		uint32_t len = big ? be32(p + off + 4) : le32(p + off + 4);
		const uint8_t *v = p + off + 8;
		char *dst = NULL;

		if (len > n - off - 8)
			break;
		if (!memcmp(id, "INAM", 4)) dst = s->title;
		else if (!memcmp(id, "IART", 4)) dst = s->artist;
		else if (!memcmp(id, "IPRD", 4)) dst = s->album;
		else if (!memcmp(id, "ICRD", 4)) dst = s->date;
		else if (!memcmp(id, "ICMT", 4)) dst = s->comment;
		else if (!memcmp(id, "IGNR", 4)) dst = s->genre;
		else if (!memcmp(id, "ISFT", 4)) dst = s->software;
		else if (!memcmp(id, "ICOP", 4)) dst = s->copyright;
		if (dst)
			meta_copy(dst, META_LEN, v, len);
		off += 8 + len + (len & 1);
	}
}

static int parse_wav(struct reader *r, struct src *s, enum kind k, char *err,
		     size_t errsz)
{
	uint8_t h[12], ch[8], fmt[64];
	int big = k == K_RIFX;
	int have_fmt = 0, have_data = 0;
	uint64_t ds64_data = 0, riff_end = UNKNOWN_SIZE;
	uint32_t tag = 0, bits = 0, block_align = 0, riff_size;
	uint64_t data_pos = 0, data_len = 0;

	if (rd_read(r, h, 12) != 12) {
		snprintf(err, errsz, "truncated header");
		return -1;
	}
	riff_size = big ? be32(h + 4) : le32(h + 4);
	if (k == K_WAV || k == K_RIFX) {
		if (riff_size >= 4 && riff_size != 0xffffffffu)
			riff_end = 8 + (uint64_t)riff_size;
	}
	s->container = k == K_RF64 ? "RF64" : k == K_RIFX ? "RIFX (big-endian WAV)" : "WAV";

	for (;;) {
		uint64_t len;
		if (interrupted)
			return -1;
		if (riff_end != UNKNOWN_SIZE && r->pos + 8 > riff_end && have_data)
			break;
		if (rd_read(r, ch, 8) != 8)
			break;
		len = big ? be32(ch + 4) : le32(ch + 4);

		if (!memcmp(ch, "ds64", 4)) {
			uint8_t d[28];
			size_t want = len < sizeof(d) ? (size_t)len : sizeof(d);
			if (len < 24 || rd_read(r, d, want) != want) {
				snprintf(err, errsz, "bad ds64 chunk");
				return -1;
			}
			riff_end = 8 + le64(d);
			ds64_data = le64(d + 8);
			if (want >= 24 && le64(d + 16))
				s->fact_frames = le64(d + 16);
			if (rd_skip(r, len - want + (len & 1)))
				return -1;
		} else if (!memcmp(ch, "fmt ", 4)) {
			size_t want = len < sizeof(fmt) ? (size_t)len : sizeof(fmt);
			uint16_t (*rd16)(const uint8_t *) = big ? be16 : le16;
			uint32_t (*rd32)(const uint8_t *) = big ? be32 : le32;

			if (len < 16) {
				snprintf(err, errsz, "fmt chunk too short");
				return -1;
			}
			memset(fmt, 0, sizeof(fmt));
			if (rd_read(r, fmt, want) != want) {
				snprintf(err, errsz, "truncated fmt chunk");
				return -1;
			}
			tag = rd16(fmt);
			s->channels = rd16(fmt + 2);
			s->rate = rd32(fmt + 4);
			block_align = rd16(fmt + 12);
			bits = rd16(fmt + 14);
			snprintf(s->container_detail, sizeof(s->container_detail),
				 "format 0x%04x (%s)", tag, wav_tag_name(tag));
			if (tag == 0xfffe) {
				/* WAVE_FORMAT_EXTENSIBLE: cbSize, valid bits (or
				 * samples per block), channel mask, then a GUID
				 * whose first two bytes are the real format tag. */
				if (want < 40) {
					snprintf(err, errsz, "extensible fmt chunk too short");
					return -1;
				}
				s->valid_bits = rd16(fmt + 18);
				s->channel_mask = rd32(fmt + 20);
				tag = rd16(fmt + 24);
				snprintf(s->container_detail, sizeof(s->container_detail),
					 "WAVE_FORMAT_EXTENSIBLE, %s", wav_tag_name(tag));
				if (s->valid_bits > bits)
					s->valid_bits = 0;
			} else if (bits % 8 == 0 && bits) {
				s->valid_bits = 0;
			}
			/* ADPCM extras follow cbSize (offset 16):
			 * samples per block at 18. */
			if (tag == 0x0011 && want >= 20)
				s->frames_per_block = rd16(fmt + 18);
			if (tag == 0x0002) {
				/* cbSize, samples per block, coefficient count,
				 * pairs.  Only the table's first seven are
				 * standard; read what fits. */
				uint32_t i, nc;
				if (want < 20) {
					snprintf(err, errsz, "MS ADPCM fmt chunk too short");
					return -1;
				}
				s->frames_per_block = rd16(fmt + 18);
				nc = want >= 22 ? rd16(fmt + 20) : 7;
				if (nc == 0 || nc > 32)
					nc = 7;
				for (i = 0; i < nc; i++) {
					if (22 + 4 * i + 4 <= want) {
						s->ms_coef[i][0] = (int16_t)rd16(fmt + 22 + 4 * i);
						s->ms_coef[i][1] = (int16_t)rd16(fmt + 24 + 4 * i);
					} else if (i < 7) {
						s->ms_coef[i][0] = ms_default_coef[i][0];
						s->ms_coef[i][1] = ms_default_coef[i][1];
					} else {
						break;
					}
				}
				s->ms_ncoef = (int)i;
			}
			/* 12- or 20-bit samples sit left-justified in
			 * whole bytes; the container width decides. */
			if ((tag == 0x0001 || tag == 0x0003) && bits % 8 &&
			    s->channels && block_align % s->channels == 0) {
				s->valid_bits = bits;
				bits = block_align / s->channels * 8;
			}
			if (wav_choose_enc(s, tag, bits, big)) {
				snprintf(err, errsz, "%s with %u-bit samples is not supported",
					 wav_tag_name(tag), bits);
				return tag == 0x0055 || tag == 0x0050 || tag == 0x00ff ||
						       tag == 0x2000 || tag == 0xf1ac ||
						       (tag >= 0x0160 && tag <= 0x0163) ?
					       -2 :
					       -1;
			}
			s->block_align = block_align;
			have_fmt = 1;
			if (rd_skip(r, len - want + (len & 1)))
				return -1;
		} else if (!memcmp(ch, "fact", 4)) {
			uint8_t f[4];
			if (len >= 4 && rd_read(r, f, 4) == 4) {
				if (!s->fact_frames)
					s->fact_frames = big ? be32(f) : le32(f);
				if (rd_skip(r, len - 4 + (len & 1)))
					return -1;
			} else if (rd_skip(r, len + (len & 1))) {
				return -1;
			}
		} else if (!memcmp(ch, "LIST", 4) && len >= 4 && len <= 65536) {
			uint8_t *buf = malloc((size_t)len);
			if (!buf)
				return -1;
			if (rd_read(r, buf, (size_t)len) != len) {
				free(buf);
				break;
			}
			wav_info_list(s, buf, (size_t)len, big);
			free(buf);
			if (len & 1)
				rd_skip(r, 1);
		} else if (!memcmp(ch, "data", 4)) {
			data_pos = r->pos;
			data_len = len;
			if (k == K_RF64 && len == 0xffffffffu)
				data_len = ds64_data;
			/* A writer that streams sets the size to 0 or
			 * all-ones and never comes back to fix it. */
			if (data_len == 0 || data_len == 0xffffffffu ||
			    data_len == 0x7ffff000u)
				data_len = UNKNOWN_SIZE;
			have_data = 1;
			/* On a pipe the samples start here.  On a file keep
			 * walking for metadata chunks after the data, then
			 * come back. */
			if (!r->seekable || data_len == UNKNOWN_SIZE || !have_fmt)
				break;
			if (rd_skip(r, data_len + (data_len & 1)))
				break;
		} else {
			if (len > (uint64_t)INT64_MAX ||
			    rd_skip(r, len + (len & 1)))
				break;
		}
	}
	if (!have_fmt) {
		snprintf(err, errsz, "no fmt chunk before the data");
		return -1;
	}
	if (!have_data) {
		snprintf(err, errsz, "no data chunk");
		return -1;
	}
	s->data_offset = data_pos;
	s->data_bytes = data_len;
	if (rd_seek(r, data_pos)) {
		snprintf(err, errsz, "cannot seek back to the samples");
		return -1;
	}

	if (is_block_codec(s)) {
		uint32_t ch2 = s->channels ? s->channels : 1;
		if (block_align == 0 || block_align > 65536) {
			snprintf(err, errsz, "bad ADPCM block size %u", block_align);
			return -1;
		}
		if (s->enc == E_IMA_ADPCM) {
			uint32_t calc;
			if (block_align < 4 * ch2 || ((block_align - 4 * ch2) % (4 * ch2))) {
				snprintf(err, errsz, "bad IMA ADPCM block size %u", block_align);
				return -1;
			}
			calc = (block_align - 4 * ch2) * 2 / ch2 + 1;
			if (s->frames_per_block == 0 || s->frames_per_block > calc)
				s->frames_per_block = calc;
		} else {
			uint32_t calc;
			if (ch2 > 2 || block_align < 7 * ch2) {
				snprintf(err, errsz, "bad MS ADPCM layout");
				return -1;
			}
			calc = (block_align - 7 * ch2) * 2 / ch2 + 2;
			if (s->frames_per_block < 2 || s->frames_per_block > calc)
				s->frames_per_block = calc;
			if (s->ms_ncoef == 0) {
				memcpy(s->ms_coef, ms_default_coef, sizeof(ms_default_coef));
				s->ms_ncoef = 7;
			}
		}
	} else if (block_align && s->channels &&
		   block_align != in_frame_bytes(s)) {
		/* Trust the sample size over a wrong block align. */
		s->block_align = in_frame_bytes(s);
	}
	return 0;
}

/* ---- Sun / NeXT AU -------------------------------------------------------------- */

static int parse_au(struct reader *r, struct src *s, char *err, size_t errsz)
{
	uint8_t h[24];
	uint32_t hdr, size, encoding;

	if (rd_read(r, h, 24) != 24) {
		snprintf(err, errsz, "truncated header");
		return -1;
	}
	hdr = be32(h + 4);
	size = be32(h + 8);
	encoding = be32(h + 12);
	s->rate = be32(h + 16);
	s->channels = be32(h + 20);
	s->container = "Sun/NeXT AU";
	snprintf(s->container_detail, sizeof(s->container_detail),
		 "encoding %u", encoding);
	if (hdr < 24 || hdr > (1u << 20)) {
		snprintf(err, errsz, "bad header size %u", hdr);
		return -1;
	}
	if (hdr > 24) {
		uint32_t ann = hdr - 24;
		uint8_t buf[META_LEN];
		uint32_t take = ann < sizeof(buf) ? ann : (uint32_t)sizeof(buf);
		if (rd_read(r, buf, take) != take) {
			snprintf(err, errsz, "truncated annotation");
			return -1;
		}
		meta_copy(s->comment, META_LEN, buf, take);
		if (rd_skip(r, ann - take))
			return -1;
	}
	switch (encoding) {
	case 1: s->enc = E_ULAW; break;
	case 2: s->enc = E_S8; break;
	case 3: s->enc = E_S16BE; break;
	case 4: s->enc = E_S24BE; break;
	case 5: s->enc = E_S32BE; break;
	case 6: s->enc = E_F32BE; break;
	case 7: s->enc = E_F64BE; break;
	case 27: s->enc = E_ALAW; break;
	default:
		snprintf(err, errsz, "AU encoding %u (compressed) is not supported",
			 encoding);
		return -1;
	}
	s->data_offset = r->pos;
	s->data_bytes = size == 0xffffffffu ? UNKNOWN_SIZE : size;
	return 0;
}

/* ---- AIFF / AIFF-C ---------------------------------------------------------- */

static int parse_aiff(struct reader *r, struct src *s, char *err, size_t errsz)
{
	uint8_t h[12], ch[8], comm[64];
	int aifc, have_comm = 0, have_ssnd = 0;
	uint32_t bits = 0, nframes = 0;
	char comp[5] = "NONE";
	uint64_t data_pos = 0, data_len = 0;

	if (rd_read(r, h, 12) != 12) {
		snprintf(err, errsz, "truncated header");
		return -1;
	}
	aifc = !memcmp(h + 8, "AIFC", 4);
	s->container = aifc ? "AIFF-C" : "AIFF";

	for (;;) {
		uint64_t len;
		if (interrupted)
			return -1;
		if (rd_read(r, ch, 8) != 8)
			break;
		len = be32(ch + 4);
		if (!memcmp(ch, "COMM", 4)) {
			size_t want = len < sizeof(comm) ? (size_t)len : sizeof(comm);
			if (len < 18 || rd_read(r, comm, want) != want) {
				snprintf(err, errsz, "bad COMM chunk");
				return -1;
			}
			s->channels = be16(comm);
			nframes = be32(comm + 2);
			bits = be16(comm + 6);
			s->rate = ext80_to_u32(comm + 8);
			if (aifc && want >= 22) {
				memcpy(comp, comm + 18, 4);
				comp[4] = 0;
			}
			have_comm = 1;
			if (rd_skip(r, len - want + (len & 1)))
				return -1;
		} else if (!memcmp(ch, "SSND", 4)) {
			uint8_t o[8];
			uint32_t offset;
			if (len < 8 || rd_read(r, o, 8) != 8) {
				snprintf(err, errsz, "bad SSND chunk");
				return -1;
			}
			offset = be32(o);
			if (offset > len - 8) {
				snprintf(err, errsz, "bad SSND offset");
				return -1;
			}
			if (rd_skip(r, offset))
				return -1;
			data_pos = r->pos;
			data_len = len - 8 - offset;
			have_ssnd = 1;
			if (!r->seekable || !have_comm)
				break;
			if (rd_skip(r, data_len + (len & 1)))
				break;
		} else if ((!memcmp(ch, "NAME", 4) || !memcmp(ch, "AUTH", 4) ||
			    !memcmp(ch, "ANNO", 4) || !memcmp(ch, "(c) ", 4)) &&
			   len <= 4096) {
			uint8_t buf[4096];
			char *dst = !memcmp(ch, "NAME", 4) ? s->title :
				    !memcmp(ch, "AUTH", 4) ? s->artist :
				    !memcmp(ch, "ANNO", 4) ? s->comment : s->copyright;
			if (rd_read(r, buf, (size_t)len) != len)
				break;
			if (!dst[0])
				meta_copy(dst, META_LEN, buf, (size_t)len);
			if (len & 1)
				rd_skip(r, 1);
		} else {
			if (rd_skip(r, len + (len & 1)))
				break;
		}
	}
	if (!have_comm || !have_ssnd) {
		snprintf(err, errsz, "missing %s chunk", have_comm ? "SSND" : "COMM");
		return -1;
	}
	snprintf(s->container_detail, sizeof(s->container_detail),
		 "compression '%s', %u-bit", comp, bits);
	/* Samples narrower than their byte width are left-justified, so the
	 * byte width alone decides the layout. */
	{
		uint32_t bytes = (bits + 7) / 8;
		if (bits && bits % 8)
			s->valid_bits = bits;
		if (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4)) {
			s->enc = bytes == 1 ? E_S8 : bytes == 2 ? E_S16BE :
				 bytes == 3 ? E_S24BE : bytes == 4 ? E_S32BE : E_NONE;
		} else if (!memcmp(comp, "sowt", 4)) {
			s->enc = bytes == 1 ? E_S8 : bytes == 2 ? E_S16LE :
				 bytes == 3 ? E_S24LE : bytes == 4 ? E_S32LE : E_NONE;
		} else if (!memcmp(comp, "raw ", 4)) {
			s->enc = bytes == 1 ? E_U8 : bytes == 2 ? E_U16BE : E_NONE;
		} else if (!memcmp(comp, "in24", 4)) {
			s->enc = E_S24BE;
		} else if (!memcmp(comp, "in32", 4)) {
			s->enc = E_S32BE;
		} else if (!memcmp(comp, "42ni", 4)) {
			s->enc = E_S24LE;
		} else if (!memcmp(comp, "23ni", 4)) {
			s->enc = E_S32LE;
		} else if (!memcmp(comp, "fl32", 4) || !memcmp(comp, "FL32", 4)) {
			s->enc = E_F32BE;
		} else if (!memcmp(comp, "fl64", 4) || !memcmp(comp, "FL64", 4)) {
			s->enc = E_F64BE;
		} else if (!memcmp(comp, "ulaw", 4) || !memcmp(comp, "ULAW", 4)) {
			s->enc = E_ULAW;
		} else if (!memcmp(comp, "alaw", 4) || !memcmp(comp, "ALAW", 4)) {
			s->enc = E_ALAW;
		} else {
			s->enc = E_NONE;
		}
		if (s->enc == E_NONE) {
			snprintf(err, errsz, "AIFF-C compression '%s' (%u-bit) is not supported",
				 comp, bits);
			return -1;
		}
	}
	s->data_offset = data_pos;
	s->data_bytes = data_len;
	if (nframes && s->channels)
		s->fact_frames = nframes;
	if (rd_seek(r, data_pos)) {
		snprintf(err, errsz, "cannot seek back to the samples");
		return -1;
	}
	return 0;
}

/* ---- CAF ---------------------------------------------------------------------- */

static int parse_caf(struct reader *r, struct src *s, char *err, size_t errsz)
{
	uint8_t h[8], ch[12], d[32];
	int have_desc = 0, have_data = 0;
	uint64_t data_pos = 0, data_len = 0;
	uint32_t fmtid = 0, flags = 0, bpp = 0, fpp = 0, bits = 0;

	if (rd_read(r, h, 8) != 8) {
		snprintf(err, errsz, "truncated header");
		return -1;
	}
	s->container = "CAF";
	for (;;) {
		uint64_t len;
		int64_t slen;
		if (interrupted)
			return -1;
		if (rd_read(r, ch, 12) != 12)
			break;
		len = be64(ch + 4);
		slen = (int64_t)len;
		if (!memcmp(ch, "desc", 4)) {
			if (len < 32 || rd_read(r, d, 32) != 32) {
				snprintf(err, errsz, "bad desc chunk");
				return -1;
			}
			s->rate = f64be_to_u32(d);
			fmtid = be32(d + 8);
			flags = be32(d + 12);
			bpp = be32(d + 16);
			fpp = be32(d + 20);
			s->channels = be32(d + 24);
			bits = be32(d + 28);
			have_desc = 1;
			if (rd_skip(r, len - 32))
				return -1;
		} else if (!memcmp(ch, "info", 4) && len >= 4 && len <= 65536) {
			uint8_t *buf = malloc((size_t)len);
			uint32_t n, i;
			size_t off = 4;
			if (!buf)
				return -1;
			if (rd_read(r, buf, (size_t)len) != len) {
				free(buf);
				break;
			}
			n = be32(buf);
			/* key\0value\0 pairs */
			for (i = 0; i < n && off < len; i++) {
				const char *key = (const char *)buf + off;
				size_t kl = strnlen(key, (size_t)len - off);
				const uint8_t *val;
				size_t vl;
				char *dst = NULL;
				if (off + kl + 1 >= len)
					break;
				val = buf + off + kl + 1;
				vl = strnlen((const char *)val, (size_t)len - off - kl - 1);
				if (!strncmp(key, "title", kl) && kl == 5) dst = s->title;
				else if (!strncmp(key, "artist", kl) && kl == 6) dst = s->artist;
				else if (!strncmp(key, "album", kl) && kl == 5) dst = s->album;
				else if (!strncmp(key, "year", kl) && kl == 4) dst = s->date;
				else if (!strncmp(key, "comments", kl) && kl == 8) dst = s->comment;
				else if (!strncmp(key, "genre", kl) && kl == 5) dst = s->genre;
				else if (!strncmp(key, "copyright", kl) && kl == 9) dst = s->copyright;
				if (dst)
					meta_copy(dst, META_LEN, val, vl);
				off += kl + 1 + vl + 1;
			}
			free(buf);
		} else if (!memcmp(ch, "data", 4)) {
			uint8_t ed[4];
			if (rd_read(r, ed, 4) != 4) {
				snprintf(err, errsz, "truncated data chunk");
				return -1;
			}
			data_pos = r->pos;
			/* -1: the chunk runs to the end of the file. */
			data_len = slen == -1 ? UNKNOWN_SIZE : (len >= 4 ? len - 4 : 0);
			have_data = 1;
			if (!r->seekable || data_len == UNKNOWN_SIZE || !have_desc)
				break;
			if (rd_skip(r, data_len))
				break;
		} else {
			if (slen < 0 || rd_skip(r, len))
				break;
		}
	}
	if (!have_desc || !have_data) {
		snprintf(err, errsz, "missing %s chunk", have_desc ? "data" : "desc");
		return -1;
	}
	snprintf(s->container_detail, sizeof(s->container_detail),
		 "'%c%c%c%c', %u-bit, flags 0x%x",
		 (fmtid >> 24) & 0xff, (fmtid >> 16) & 0xff, (fmtid >> 8) & 0xff,
		 fmtid & 0xff, bits, flags);
	(void)fpp;
	if (fmtid == 0x6c70636d) { /* 'lpcm' */
		int isfloat = flags & 1, little = flags & 2;
		uint32_t bytes = s->channels ? bpp / s->channels : 0;
		if (isfloat)
			s->enc = bytes == 4 ? (little ? E_F32LE : E_F32BE) :
				 bytes == 8 ? (little ? E_F64LE : E_F64BE) : E_NONE;
		else
			s->enc = bytes == 1 ? E_S8 :
				 bytes == 2 ? (little ? E_S16LE : E_S16BE) :
				 bytes == 3 ? (little ? E_S24LE : E_S24BE) :
				 bytes == 4 ? (little ? E_S32LE : E_S32BE) : E_NONE;
		if (bits && bits % 8)
			s->valid_bits = bits;
	} else if (fmtid == 0x756c6177) { /* 'ulaw' */
		s->enc = E_ULAW;
	} else if (fmtid == 0x616c6177) { /* 'alaw' */
		s->enc = E_ALAW;
	} else {
		s->enc = E_NONE;
	}
	if (s->enc == E_NONE) {
		snprintf(err, errsz, "CAF data format %s is not supported",
			 s->container_detail);
		return -2;
	}
	s->data_offset = data_pos;
	s->data_bytes = data_len;
	if (rd_seek(r, data_pos)) {
		snprintf(err, errsz, "cannot seek back to the samples");
		return -1;
	}
	return 0;
}

/* ---- validation and derived values ---------------------------------------- */

static int finish_src(struct reader *r, struct src *s, char *err, size_t errsz)
{
	uint64_t ub, units;

	if (s->channels < 1 || s->channels > MAX_CHANNELS) {
		snprintf(err, errsz, "%u channels (1..%d can be played)", s->channels,
			 MAX_CHANNELS);
		return -1;
	}
	if (s->rate < MIN_RATE || s->rate > MAX_RATE) {
		snprintf(err, errsz, "sample rate %u Hz (%d..%d can be played)",
			 s->rate, MIN_RATE, MAX_RATE);
		return -1;
	}
	if (s->enc <= E_NONE || s->enc >= E_COUNT) {
		snprintf(err, errsz, "no usable sample encoding");
		return -1;
	}
	if (is_block_codec(s) &&
	    (s->frames_per_block == 0 || s->frames_per_block > 65536)) {
		snprintf(err, errsz, "bad ADPCM block layout");
		return -1;
	}
	/* Clip a size that claims more than the file has (truncated
	 * downloads, writers that crashed). */
	if (s->data_bytes != UNKNOWN_SIZE && r->file_size &&
	    s->data_offset + s->data_bytes > r->file_size)
		s->data_bytes = r->file_size > s->data_offset ?
					r->file_size - s->data_offset : 0;
	if (s->data_bytes == UNKNOWN_SIZE && r->file_size &&
	    r->file_size > s->data_offset)
		s->data_bytes = r->file_size - s->data_offset;

	ub = unit_in_bytes(s);
	if (s->data_bytes == UNKNOWN_SIZE || ub == 0) {
		s->total_frames = UNKNOWN_SIZE;
	} else {
		units = s->data_bytes / ub;
		s->total_frames = units * unit_frames(s);
		if (is_block_codec(s) && s->data_bytes % ub) {
			/* a short final block still decodes */
			s->total_frames += s->enc == E_IMA_ADPCM ?
				(s->data_bytes % ub >= 4u * s->channels ?
				 1 + ((s->data_bytes % ub - 4u * s->channels) / (4u * s->channels)) * 8 : 0) :
				(s->data_bytes % ub >= 7u * s->channels ?
				 2 + (s->data_bytes % ub - 7u * s->channels) * 2 / s->channels : 0);
		}
		if (is_block_codec(s) && s->fact_frames &&
		    s->fact_frames < s->total_frames)
			s->total_frames = s->fact_frames;
	}
	return 0;
}

/* ---- opening and identifying an input --------------------------------------- */

struct input {
	struct reader rd;
	struct src src;
	enum kind kind;
	int fd;
	int is_stdin;
};

/* Returns 0 ready to play, 1 = a compressed format for GStreamer (kind
 * set), -1 error (message printed). */
static int open_input(const char *path, struct input *in)
{
	const uint8_t *p;
	size_t got;
	char err[160] = "";
	int rc = -1;

	memset(in, 0, sizeof(*in));
	in->src.path = path;
	in->is_stdin = !strcmp(path, "-");
	if (in->is_stdin) {
		in->fd = 0;
		snprintf(in->src.display, sizeof(in->src.display), "(standard input)");
	} else {
		const char *base = strrchr(path, '/');
		in->fd = open(path, O_RDONLY | O_CLOEXEC);
		if (in->fd < 0) {
			fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
			return -1;
		}
		sanitize_name(in->src.display, sizeof(in->src.display),
			      base && base[1] ? base + 1 : path);
	}
	if (rd_init(&in->rd, in->fd) < 0) {
		fprintf(stderr, "%s: out of memory\n", prog);
		goto fail;
	}
	if (!in->is_stdin && !in->rd.seekable) {
		struct stat st;
		if (fstat(in->fd, &st) == 0 && S_ISDIR(st.st_mode)) {
			fprintf(stderr, "%s: %s: is a directory\n", prog, path);
			goto fail;
		}
	}

	p = rd_peek(&in->rd, 256, &got);
	if (in->rd.error) {
		fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(in->rd.error));
		goto fail;
	}
	in->kind = opt.force_raw ? K_UNKNOWN : sniff(p, got);

	switch (in->kind) {
	case K_WAV: case K_RIFX: case K_RF64:
		rc = parse_wav(&in->rd, &in->src, in->kind, err, sizeof(err));
		break;
	case K_AU:
		rc = parse_au(&in->rd, &in->src, err, sizeof(err));
		break;
	case K_AIFF:
		rc = parse_aiff(&in->rd, &in->src, err, sizeof(err));
		break;
	case K_CAF:
		rc = parse_caf(&in->rd, &in->src, err, sizeof(err));
		break;
	case K_UNKNOWN: {
		enum kind ek = in->is_stdin ? K_UNKNOWN : kind_from_extension(path);
		if (!opt.force_raw && !opt.raw_given && ek != K_UNKNOWN) {
			in->kind = ek;
			return 1;
		}
		if (!opt.force_raw && !opt.raw_given && got == 0) {
			fprintf(stderr, "%s: %s: empty input\n", prog, in->src.display);
			goto fail;
		}
		if (!opt.force_raw && !opt.raw_given) {
			fprintf(stderr,
				"%s: %s: not a format I recognise; for headerless PCM give "
				"--raw with -r RATE -c CHANNELS -f FORMAT\n",
				prog, in->src.display);
			goto fail;
		}
		in->src.container = "raw";
		in->src.enc = opt.raw_enc >= 0 ? (enum enc)opt.raw_enc : E_S16LE;
		in->src.rate = opt.raw_rate ? opt.raw_rate : 48000;
		in->src.channels = opt.raw_channels ? opt.raw_channels : 2;
		in->src.data_offset = 0;
		in->src.data_bytes = UNKNOWN_SIZE;
		snprintf(in->src.container_detail, sizeof(in->src.container_detail),
			 "headerless");
		rc = 0;
		break;
	}
	default:
		return 1; /* compressed: GStreamer's business */
	}
	if (rc == -2) {
		/* A known container around a compressed codec. */
		in->kind = K_WRAPPED;
		if (!opt.quiet && !opt.list_only)
			fprintf(stderr, "%s: %s: %s; trying GStreamer\n", prog,
				in->src.display, err);
		return 1;
	}
	if (rc < 0) {
		if (interrupted)
			goto fail;
		fprintf(stderr, "%s: %s: %s file: %s\n", prog, in->src.display,
			in->src.container ? in->src.container : "bad",
			err[0] ? err : "cannot parse");
		goto fail;
	}
	if (finish_src(&in->rd, &in->src, err, sizeof(err))) {
		fprintf(stderr, "%s: %s: %s\n", prog, in->src.display, err);
		goto fail;
	}
	return 0;
fail:
	rd_free(&in->rd);
	if (!in->is_stdin && in->fd >= 0)
		close(in->fd);
	in->fd = -1;
	return -1;
}

static void close_input(struct input *in)
{
	rd_free(&in->rd);
	if (!in->is_stdin && in->fd >= 0)
		close(in->fd);
	in->fd = -1;
}

/* ---- printing ------------------------------------------------------------------- */

static void fmt_time(uint64_t frames, uint32_t rate, char *buf, size_t sz, int cs)
{
	uint64_t total_cs, h, m, s, c;

	if (!rate || frames == UNKNOWN_SIZE) {
		snprintf(buf, sz, "--:--");
		return;
	}
	total_cs = frames * 100 / rate;
	c = total_cs % 100;
	s = (total_cs / 100) % 60;
	m = (total_cs / 6000) % 60;
	h = total_cs / 360000;
	if (h && cs)
		snprintf(buf, sz, "%llu:%02llu:%02llu.%02llu", (unsigned long long)h,
			 (unsigned long long)m, (unsigned long long)s,
			 (unsigned long long)c);
	else if (h)
		snprintf(buf, sz, "%llu:%02llu:%02llu", (unsigned long long)h,
			 (unsigned long long)m, (unsigned long long)s);
	else if (cs)
		snprintf(buf, sz, "%llu:%02llu.%02llu", (unsigned long long)m,
			 (unsigned long long)s, (unsigned long long)c);
	else
		snprintf(buf, sz, "%llu:%02llu", (unsigned long long)m,
			 (unsigned long long)s);
}

static void fmt_bytes(uint64_t b, char *buf, size_t sz)
{
	if (b == UNKNOWN_SIZE)
		snprintf(buf, sz, "unknown");
	else if (b >= 10ull * 1024 * 1024)
		snprintf(buf, sz, "%llu.%01llu MB", (unsigned long long)(b >> 20),
			 (unsigned long long)(((b & 0xfffff) * 10) >> 20));
	else if (b >= 10 * 1024)
		snprintf(buf, sz, "%llu KB", (unsigned long long)(b >> 10));
	else
		snprintf(buf, sz, "%llu bytes", (unsigned long long)b);
}

static const char *const wav_speakers[18] = {
	"FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR",
	"TC", "TFL", "TFC", "TFR", "TBL", "TBC", "TBR"
};

static void fmt_layout(const struct src *s, char *buf, size_t sz)
{
	static const char *const def[MAX_CHANNELS + 1] = {
		"", "mono", "stereo", "FL FR FC", "FL FR BL BR",
		"FL FR FC BL BR", "5.1: FL FR FC LFE BL BR",
		"6.1: FL FR FC LFE BC SL SR", "7.1: FL FR FC LFE BL BR SL SR"
	};
	size_t o = 0;
	int bit, n = 0;

	buf[0] = 0;
	if (!s->channel_mask) {
		snprintf(buf, sz, "%s", s->channels <= MAX_CHANNELS ? def[s->channels] : "");
		return;
	}
	for (bit = 0; bit < 18 && o + 6 < sz; bit++) {
		if (!(s->channel_mask & (1u << bit)))
			continue;
		o += (size_t)snprintf(buf + o, sz - o, "%s%s", n ? " " : "",
				      wav_speakers[bit]);
		n++;
	}
	if ((uint32_t)n != s->channels && o + 16 < sz)
		snprintf(buf + o, sz - o, " (mask 0x%x)", s->channel_mask);
}

static void print_info(const struct input *in)
{
	const struct src *s = &in->src;
	char t[48], b[32], l[96];

	printf("%s\n", s->display);
	printf("  container   %s", s->container);
	if (s->container_detail[0])
		printf(" (%s)", s->container_detail);
	putchar('\n');
	printf("  encoding    %s", encs[s->enc].text);
	if (s->valid_bits)
		printf(", %u valid bits", s->valid_bits);
	if (is_block_codec(s))
		printf(", %u-byte blocks of %u frames", s->block_align,
		       s->frames_per_block);
	putchar('\n');
	printf("  rate        %u Hz\n", s->rate);
	fmt_layout(s, l, sizeof(l));
	printf("  channels    %u%s%s%s\n", s->channels, l[0] ? " (" : "", l,
	       l[0] ? ")" : "");
	fmt_time(s->total_frames, s->rate, t, sizeof(t), 1);
	if (s->total_frames != UNKNOWN_SIZE)
		printf("  duration    %s (%llu frames)\n", t,
		       (unsigned long long)s->total_frames);
	else
		printf("  duration    unknown (streamed)\n");
	fmt_bytes(s->data_bytes, b, sizeof(b));
	printf("  data        %s at offset %llu\n", b,
	       (unsigned long long)s->data_offset);
	if (s->total_frames != UNKNOWN_SIZE && s->total_frames && s->rate)
		printf("  bit rate    %llu kbit/s\n",
		       (unsigned long long)(s->data_bytes * 8ull * s->rate /
					    s->total_frames / 1000));
	printf("  sent as     %s%s\n", la_format_name(encs[s->enc].out_fmt) ?
	       la_format_name(encs[s->enc].out_fmt) : "?",
	       (s->enc == E_U8 || s->enc == E_S16LE || s->enc == E_S16BE ||
		s->enc == E_S24LE || s->enc == E_S24LE32 || s->enc == E_S32LE ||
		s->enc == E_F32LE || s->enc == E_F64LE) ? " (unchanged)" : "");
	if (s->title[0]) printf("  title       %s\n", s->title);
	if (s->artist[0]) printf("  artist      %s\n", s->artist);
	if (s->album[0]) printf("  album       %s\n", s->album);
	if (s->date[0]) printf("  date        %s\n", s->date);
	if (s->genre[0]) printf("  genre       %s\n", s->genre);
	if (s->comment[0]) printf("  comment     %s\n", s->comment);
	if (s->copyright[0]) printf("  copyright   %s\n", s->copyright);
	if (s->software[0]) printf("  software    %s\n", s->software);
}

/* ---- GStreamer hand-off ------------------------------------------------------ */

static int find_in_path(const char *name, char *out, size_t outsz)
{
	const char *path = getenv("PATH");
	const char *fallback = "/usr/bin:/bin:/usr/local/bin";
	const char *p;

	if (!path || !*path)
		path = fallback;
	for (p = path; *p;) {
		const char *e = strchr(p, ':');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len && len + 1 + strlen(name) + 1 <= outsz) {
			memcpy(out, p, len);
			out[len] = '/';
			strcpy(out + len + 1, name);
			if (access(out, X_OK) == 0)
				return 0;
		}
		if (!e)
			break;
		p = e + 1;
	}
	if (path != fallback) {
		/* PATH may be minimal under init or su. */
		const char *const dirs[] = { "/usr/bin/", "/usr/local/bin/" };
		size_t i;
		for (i = 0; i < 2; i++) {
			snprintf(out, outsz, "%s%s", dirs[i], name);
			if (access(out, X_OK) == 0)
				return 0;
		}
	}
	return -1;
}

/* Play a compressed file with gst-play-1.0.  `last' means nothing else
 * follows, so the process may simply be replaced. */
static int play_with_gst(const char *path, enum kind k, int last)
{
	char exe[512], vol[32];
	char *argv[8];
	int argc = 0, status;
	pid_t pid;

	if (opt.no_gst || find_in_path("gst-play-1.0", exe, sizeof(exe))) {
		fprintf(stderr,
			"%s: %s: %s is not decoded by %s itself.\n"
			"%s: install GStreamer (gst-play-1.0 with the matching plugins) "
			"to play it,\n"
			"%s: or convert it first, e.g. ffmpeg -i FILE -c:a pcm_s16le out.wav\n",
			prog, path, kind_text(k), prog, prog, prog);
		return 1;
	}
	if (!strcmp(path, "-")) {
		fprintf(stderr, "%s: %s on standard input: run gst-play-1.0 on the file "
			"itself\n", prog, kind_text(k));
		return 1;
	}
	argv[argc++] = "gst-play-1.0";
	if (opt.have_volume) {
		snprintf(vol, sizeof(vol), "--volume=%u.%02u", opt.volume / 100,
			 opt.volume % 100);
		argv[argc++] = vol;
	}
	if (opt.quiet)
		argv[argc++] = "--quiet";
	if (!isatty(0))
		argv[argc++] = "--no-interactive";
	argv[argc++] = (char *)path;
	argv[argc] = NULL;
	if (!opt.quiet)
		fprintf(stderr, "%s: %s: %s, playing with gst-play-1.0\n", prog, path,
			kind_text(k));
	fflush(NULL);
	signal(SIGPIPE, SIG_DFL); /* an ignored signal survives exec */
	if (last && opt.loops == 1) {
		execv(exe, argv);
		fprintf(stderr, "%s: cannot run %s: %s\n", prog, exe, strerror(errno));
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: fork: %s\n", prog, strerror(errno));
		return 1;
	}
	if (pid == 0) {
		signal(SIGINT, SIG_DFL);
		execv(exe, argv);
		_exit(127);
	}
	signal(SIGPIPE, SIG_IGN);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			status = 1 << 8;
			break;
		}
		if (interrupted)
			kill(pid, SIGINT);
	}
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return WEXITSTATUS(status);
}

/* ---- playback ------------------------------------------------------------------ */

struct player {
	la_stream *st;
	la_format fmt;
	uint64_t frames_written; /* into the current stream */
	struct timespec last_progress;
};

static void on_signal(int sig)
{
	if (interrupted) {
		/* Second Ctrl-C: the user wants out now. */
		static const char msg[] = "\n";
		(void)!write(2, msg, 1);
		_exit(128 + sig);
	}
	interrupted = sig;
}

static void close_stream(struct player *pl, int drain)
{
	if (!pl->st)
		return;
	if (drain && !interrupted)
		(void)la_drain(pl->st);
	else
		(void)la_flush(pl->st);
	la_close(pl->st);
	pl->st = NULL;
	pl->frames_written = 0;
}

static int open_stream(struct player *pl, const struct src *s)
{
	la_stream_options o;
	char name[AUDIOD_NAME_MAX];
	int err = 0;

	memset(&o, 0, sizeof(o));
	o.role = opt.role ? opt.role : AUD_ROLE_MUSIC;
	o.buffer_ms = opt.buffer_ms;
	if (opt.have_volume) {
		o.volume = opt.volume;
		if (opt.volume == 0)
			o.flags |= LA_OPT_VOLUME_ZERO;
	}
	o.app_name = prog;
	if (opt.stream_name)
		sanitize_name(name, sizeof(name), opt.stream_name);
	else if (s->title[0] && s->artist[0])
		snprintf(name, sizeof(name), "%.*s - %.*s", 30, s->artist,
			 (int)sizeof(name) - 34, s->title);
	else if (s->title[0])
		snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, s->title);
	else
		snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, s->display);
	o.stream_name = name;

	pl->fmt.format = encs[s->enc].out_fmt;
	pl->fmt.rate = s->rate;
	pl->fmt.channels = s->channels;
	pl->st = la_open(&pl->fmt, &o, &err);
	if (!pl->st) {
		fprintf(stderr, "%s: cannot open an audio stream: %s\n", prog,
			la_strerror(err));
		if (err == -ENOENT || err == -ECONNREFUSED)
			fprintf(stderr, "%s: the audio service (audiod) is not running\n",
				prog);
		return -1;
	}
	pl->frames_written = 0;
	return 0;
}

static void show_progress(struct player *pl, const struct src *s,
			  uint64_t file_base, uint64_t base_frame,
			  uint64_t region_frames, int force)
{
	struct timespec now;
	uint64_t played = 0, lat = 0, pos;
	char cur[32], tot[32], bar[32];
	long ms;
	int i, cells = 20, fill;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (now.tv_sec - pl->last_progress.tv_sec) * 1000 +
	     (now.tv_nsec - pl->last_progress.tv_nsec) / 1000000;
	if (!force && ms < 200)
		return;
	pl->last_progress = now;

	/* The stream may carry earlier files (gapless); count from where
	 * this one began. */
	if (la_latency(pl->st, &lat, &played) < 0 || played > pl->frames_written) {
		uint64_t lf = lat * s->rate / 1000000;
		played = pl->frames_written > lf ? pl->frames_written - lf : 0;
	}
	played = played > file_base ? played - file_base : 0;
	if (region_frames != UNKNOWN_SIZE && region_frames &&
	    played > region_frames)
		played %= region_frames; /* looping */
	pos = base_frame + played;
	fmt_time(pos, s->rate, cur, sizeof(cur), 0);
	if (s->total_frames != UNKNOWN_SIZE) {
		fmt_time(s->total_frames, s->rate, tot, sizeof(tot), 0);
		fill = s->total_frames ? (int)(pos * (uint64_t)cells / s->total_frames) : 0;
		if (fill > cells)
			fill = cells;
		for (i = 0; i < cells; i++)
			bar[i] = i < fill ? '#' : '-';
		bar[cells] = 0;
		fprintf(stderr, "\r  %s / %s [%s] %3d%%  ", cur, tot, bar,
			s->total_frames ? (int)(pos * 100 / s->total_frames) : 0);
	} else {
		fprintf(stderr, "\r  %s  ", cur);
	}
}

/* Play one prepared input.  `next' tells whether another input follows
 * (and whether the stream may be kept for gapless continuation). */
static int play_input(struct player *pl, struct input *in)
{
	const struct src *s = &in->src;
	uint32_t ub = unit_in_bytes(s), uf = unit_frames(s);
	uint32_t ofb = out_frame_bytes(s);
	size_t units_per_chunk, in_cap, out_cap;
	uint8_t *ibuf = NULL, *obuf = NULL;
	uint64_t start_frame = 0, limit_frames = UNKNOWN_SIZE;
	uint64_t skip_units, drop_frames, region_frames;
	uint64_t file_base;
	uint32_t pass = 0;
	int rc = 0;

	if (ub == 0 || uf == 0 || ofb == 0)
		return 1;

	/* Reuse the stream when the format did not change: no gap between
	 * files of an album. */
	if (pl->st && (pl->fmt.format != encs[s->enc].out_fmt ||
		       pl->fmt.rate != s->rate || pl->fmt.channels != s->channels))
		close_stream(pl, 1);
	if (!pl->st && open_stream(pl, s))
		return 1;

	file_base = pl->frames_written;
	units_per_chunk = READ_CHUNK / ub;
	if (units_per_chunk == 0)
		units_per_chunk = 1;
	in_cap = units_per_chunk * ub;
	out_cap = units_per_chunk * (size_t)uf * ofb;
	ibuf = malloc(in_cap);
	obuf = malloc(out_cap);
	if (!ibuf || !obuf) {
		fprintf(stderr, "%s: out of memory\n", prog);
		rc = 1;
		goto out;
	}

	if (opt.start_ms)
		start_frame = opt.start_ms * s->rate / 1000;
	if (s->total_frames != UNKNOWN_SIZE && start_frame >= s->total_frames) {
		fprintf(stderr, "%s: %s: start is past the end\n", prog, s->display);
		rc = 1;
		goto out;
	}
	if (opt.duration_ms)
		limit_frames = opt.duration_ms * s->rate / 1000;
	region_frames = s->total_frames == UNKNOWN_SIZE ? UNKNOWN_SIZE :
			s->total_frames - start_frame;
	if (limit_frames != UNKNOWN_SIZE &&
	    (region_frames == UNKNOWN_SIZE || limit_frames < region_frames))
		region_frames = limit_frames;
	skip_units = start_frame / uf;
	drop_frames = start_frame % uf;

	if (!opt.quiet && !opt.list_only) {
		char t[32];
		fmt_time(s->total_frames, s->rate, t, sizeof(t), 0);
		fprintf(stderr, "Playing %s: %s, %u Hz, %u ch%s%s\n", s->display,
			encs[s->enc].text, s->rate, s->channels,
			s->total_frames != UNKNOWN_SIZE ? ", " : "",
			s->total_frames != UNKNOWN_SIZE ? t : "");
	}

	for (;;) {
		uint64_t frames_left = region_frames;
		uint64_t bytes_left;
		int err;

		/* Position at the start of this pass. */
		err = rd_seek(&in->rd, s->data_offset + skip_units * ub);
		if (err) {
			if (pass > 0 && err == -ESPIPE) {
				if (!opt.quiet)
					fprintf(stderr, "%s: cannot loop standard input\n",
						prog);
				break;
			}
			fprintf(stderr, "%s: %s: seek: %s\n", prog, s->display,
				strerror(-err));
			rc = 1;
			break;
		}
		bytes_left = s->data_bytes == UNKNOWN_SIZE ? UNKNOWN_SIZE :
			     (s->data_bytes > skip_units * ub ?
			      s->data_bytes - skip_units * ub : 0);

		while (!interrupted && frames_left) {
			size_t want = in_cap, got, ob;
			uint64_t frames;
			const uint8_t *src;

			if (bytes_left != UNKNOWN_SIZE && bytes_left < want)
				want = (size_t)bytes_left;
			if (want == 0)
				break;
			got = rd_read(&in->rd, ibuf, want);
			if (got == 0) {
				if (in->rd.error && !interrupted) {
					fprintf(stderr, "%s: %s: read: %s\n", prog,
						s->display, strerror(in->rd.error));
					rc = 1;
				}
				break;
			}
			if (bytes_left != UNKNOWN_SIZE)
				bytes_left -= got;
			/* Whole units only; a torn final frame is dropped,
			 * a torn final ADPCM block still decodes. */
			if (!is_block_codec(s))
				got -= got % ub;
			if (got == 0)
				break;
			ob = convert(s, ibuf, got, obuf);
			if (ob == 0 && got) {
				fprintf(stderr, "%s: out of memory\n", prog);
				rc = 1;
				break;
			}
			frames = ob / ofb;
			src = obuf;
			if (drop_frames) {
				uint64_t d = drop_frames < frames ? drop_frames : frames;
				src += d * ofb;
				frames -= d;
				drop_frames -= d;
			}
			if (frames_left != UNKNOWN_SIZE && frames > frames_left)
				frames = frames_left;
			/* Hand everything over; la_write blocks until the
			 * service has room. */
			{
				size_t todo = (size_t)(frames * ofb), off = 0;
				while (todo > off && !interrupted) {
					ssize_t w = la_write(pl->st, src + off, todo - off);
					if (w == -EINTR || w == -EAGAIN)
						continue;
					if (w < 0) {
						fprintf(stderr, "%s%s: playback failed: %s\n",
							opt.progress ? "\n" : "", prog,
							la_strerror((int)w));
						rc = 1;
						goto out;
					}
					off += (size_t)w;
				}
			}
			pl->frames_written += frames;
			if (frames_left != UNKNOWN_SIZE)
				frames_left -= frames;
			if (opt.progress && !opt.quiet)
				show_progress(pl, s, file_base, start_frame,
					      region_frames, 0);
		}
		pass++;
		drop_frames = start_frame % uf;
		if (interrupted || rc)
			break;
		if (opt.loops && pass >= opt.loops)
			break;
		if (in->is_stdin || !in->rd.seekable) {
			if (!opt.quiet)
				fprintf(stderr, "%s: cannot loop a pipe\n", prog);
			break;
		}
	}
	if (opt.progress && !opt.quiet && !interrupted) {
		show_progress(pl, s, file_base, start_frame, region_frames, 1);
	}
out:
	if (opt.progress && !opt.quiet)
		fputc('\n', stderr);
	free(ibuf);
	free(obuf);
	return rc;
}

/* ---- argument parsing ------------------------------------------------------------- */

/* Time: "90", "1.5", "1:30", "1:30.25", "1:02:03".  Result in ms. */
static int parse_time(const char *s, uint64_t *ms)
{
	uint64_t parts[3] = { 0, 0, 0 }, frac = 0;
	int np = 0, fd = 0;
	const char *p = s;

	if (!*p)
		return -1;
	for (;;) {
		uint64_t v = 0;
		int digits = 0;
		while (isdigit((unsigned char)*p)) {
			v = v * 10 + (uint64_t)(*p - '0');
			if (v > 100000000ull)
				return -1;
			p++;
			digits++;
		}
		if (!digits || np >= 3)
			return -1;
		parts[np++] = v;
		if (*p == ':') {
			p++;
			continue;
		}
		break;
	}
	if (*p == '.') {
		p++;
		while (isdigit((unsigned char)*p)) {
			if (fd < 3) {
				frac = frac * 10 + (uint64_t)(*p - '0');
				fd++;
			}
			p++;
		}
		while (fd < 3) {
			frac *= 10;
			fd++;
		}
	}
	if (*p == 's' && p[1] == 0)
		p++;
	if (*p)
		return -1;
	if (np == 1)
		*ms = parts[0] * 1000 + frac;
	else if (np == 2)
		*ms = (parts[0] * 60 + parts[1]) * 1000 + frac;
	else
		*ms = (parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000 + frac;
	return 0;
}

static int parse_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (!s || !isdigit((unsigned char)*s))
		return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || (*end && strcmp(end, "%")) || v < lo || v > hi)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

/* Rates may be written 44100, 44.1k or 48k. */
static int parse_rate(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v, frac = 0, scale = 1;

	if (!isdigit((unsigned char)*s))
		return -1;
	v = strtoul(s, &end, 10);
	if (*end == '.') {
		end++;
		while (isdigit((unsigned char)*end) && scale < 1000) {
			frac = frac * 10 + (unsigned long)(*end - '0');
			scale *= 10;
			end++;
		}
	}
	if (*end == 'k' || *end == 'K') {
		v = v * 1000 + frac * 1000 / scale;
		end++;
	} else if (scale != 1) {
		return -1;
	}
	if (*end || v < MIN_RATE || v > MAX_RATE)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: %s [options] [FILE...]\n"
		"Play WAV, AU, AIFF, CAF or raw PCM through the audio service.\n"
		"With no FILE, or FILE '-', standard input is played.\n"
		"\n"
		"  -v, --volume N        stream volume 0..100 (default 100)\n"
		"  -R, --role ROLE       music, video, game, event, phone, web, other\n"
		"  -L, --loop[=N]        play N times (0 or no N: forever)\n"
		"  -s, --start TIME      start at TIME (90, 1:30, 1:02:03, 12.5)\n"
		"  -t, --duration TIME   play at most TIME\n"
		"  -p, --progress        show position while playing\n"
		"  -l, --list            print what the files are; play nothing\n"
		"  -n, --name NAME       stream name shown by audioctl\n"
		"  -b, --buffer MS       buffering wanted from the service\n"
		"  -q, --quiet           no messages\n"
		"      --no-gst          never hand files to gst-play-1.0\n"
		"\n"
		"Headerless PCM:\n"
		"      --raw             treat input as raw even if it has a header\n"
		"  -r, --rate HZ         sample rate (default 48000; 44.1k allowed)\n"
		"  -c, --channels N      channels 1..8 (default 2)\n"
		"  -f, --format FMT      u8 s8 s16le s16be u16le u16be s24le s24be\n"
		"                        s24_32le s32le s32be f32le f32be f64le f64be\n"
		"                        ulaw alaw (default s16le)\n"
		"\n"
		"  -h, --help            this text\n"
		"  -V, --version         version\n",
		prog);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "volume", required_argument, NULL, 'v' },
		{ "role", required_argument, NULL, 'R' },
		{ "loop", optional_argument, NULL, 'L' },
		{ "start", required_argument, NULL, 's' },
		{ "duration", required_argument, NULL, 't' },
		{ "progress", no_argument, NULL, 'p' },
		{ "list", no_argument, NULL, 'l' },
		{ "name", required_argument, NULL, 'n' },
		{ "buffer", required_argument, NULL, 'b' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "raw", no_argument, NULL, 1 },
		{ "no-gst", no_argument, NULL, 2 },
		{ "rate", required_argument, NULL, 'r' },
		{ "channels", required_argument, NULL, 'c' },
		{ "format", required_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, 0, NULL, 0 }
	};
	const char **files;
	int nfiles = 0, c, i, rc = 0;
	struct sigaction sa;
	struct player pl;

	if (argv[0] && argv[0][0]) {
		const char *s = strrchr(argv[0], '/');
		prog = s ? s + 1 : argv[0];
	}
	files = calloc((size_t)argc + 1, sizeof(*files));
	if (!files)
		return 1;

	/* This getopt stops at the first operand; collect operands and go on
	 * so options may follow file names. */
	opterr = 1;
	while (optind < argc) {
		if (!strcmp(argv[optind], "--")) {
			for (optind++; optind < argc; optind++)
				files[nfiles++] = argv[optind];
			break;
		}
		if (argv[optind][0] != '-' || argv[optind][1] == 0) {
			files[nfiles++] = argv[optind++];
			continue;
		}
		c = getopt_long(argc, argv, "v:R:L:s:t:pln:b:qr:c:f:hV", longopts, NULL);
		if (c == -1) {
			files[nfiles++] = argv[optind++];
			continue;
		}
		switch (c) {
		case 'v':
			if (parse_u32(optarg, 0, 100, &opt.volume)) {
				fprintf(stderr, "%s: bad volume '%s' (0..100)\n", prog, optarg);
				return 2;
			}
			opt.have_volume = 1;
			break;
		case 'R':
			if (la_parse_role(optarg, &opt.role) < 0) {
				fprintf(stderr, "%s: unknown role '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'L':
			if (!optarg) {
				opt.loops = 0;
			} else if (parse_u32(optarg, 0, 1000000, &opt.loops)) {
				fprintf(stderr, "%s: bad loop count '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 's':
			if (parse_time(optarg, &opt.start_ms)) {
				fprintf(stderr, "%s: bad start time '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 't':
			if (parse_time(optarg, &opt.duration_ms) || !opt.duration_ms) {
				fprintf(stderr, "%s: bad duration '%s'\n", prog, optarg);
				return 2;
			}
			break;
		case 'p': opt.progress = 1; break;
		case 'l': opt.list_only = 1; break;
		case 'n': opt.stream_name = optarg; break;
		case 'b':
			if (parse_u32(optarg, 1, 10000, &opt.buffer_ms)) {
				fprintf(stderr, "%s: bad buffer '%s' (1..10000 ms)\n", prog,
					optarg);
				return 2;
			}
			break;
		case 'q': opt.quiet = 1; break;
		case 1: opt.force_raw = 1; opt.raw_given = 1; break;
		case 2: opt.no_gst = 1; break;
		case 'r':
			if (parse_rate(optarg, &opt.raw_rate)) {
				fprintf(stderr, "%s: bad rate '%s' (%d..%d)\n", prog, optarg,
					MIN_RATE, MAX_RATE);
				return 2;
			}
			opt.raw_given = 1;
			break;
		case 'c':
			if (parse_u32(optarg, 1, MAX_CHANNELS, &opt.raw_channels)) {
				fprintf(stderr, "%s: bad channel count '%s' (1..%d)\n", prog,
					optarg, MAX_CHANNELS);
				return 2;
			}
			opt.raw_given = 1;
			break;
		case 'f':
			opt.raw_enc = parse_enc(optarg);
			if (opt.raw_enc < 0) {
				fprintf(stderr, "%s: unknown sample format '%s'\n", prog,
					optarg);
				return 2;
			}
			opt.raw_given = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		case 'V':
			printf("%s (LikeOS) libaudio %d.%d\n", prog, LA_VERSION_MAJOR,
			       LA_VERSION_MINOR);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (nfiles == 0) {
		if (isatty(0)) {
			usage(stderr);
			return 2;
		}
		files[nfiles++] = "-";
	}
	if (opt.list_only)
		opt.progress = 0;
	if (opt.progress && !isatty(2))
		opt.progress = 0;

	g711_init();
	signal(SIGPIPE, SIG_IGN);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	memset(&pl, 0, sizeof(pl));
	for (i = 0; i < nfiles && !interrupted; i++) {
		struct input in;
		int r = open_input(files[i], &in);

		if (r < 0) {
			rc = 1;
			continue;
		}
		if (r == 1) {
			if (opt.list_only) {
				printf("%s\n  container   %s (played through gst-play-1.0)\n",
				       files[i], kind_text(in.kind));
				close_input(&in);
				continue;
			}
			close_input(&in);
			/* Finish what is queued before another player
			 * starts on the device. */
			close_stream(&pl, 1);
			r = play_with_gst(files[i], in.kind, i == nfiles - 1);
			if (r)
				rc = r == 130 ? 130 : 1;
			continue;
		}
		if (opt.list_only) {
			print_info(&in);
		} else if (play_input(&pl, &in)) {
			rc = 1;
			close_stream(&pl, 0);
		}
		close_input(&in);
	}
	if (interrupted) {
		close_stream(&pl, 0);
		if (!opt.quiet)
			fprintf(stderr, "%s: interrupted\n", prog);
		free(files);
		return 128 + (int)interrupted;
	}
	close_stream(&pl, 1);
	free(files);
	return rc;
}
