// LikeOS Intel audio DSP - topology files and IPC4 pipeline management.
//
// A topology file (".tplg", the ALSA SoC binary topology format) describes
// the processing graph the DSP firmware runs for one machine: the PCM
// devices offered to the host, the DAPM widgets (host copiers, gains,
// mixers, sample rate converters, processing modules, DAI copiers and the
// pipeline "scheduler" widgets that group them), the routes between the
// widgets, and the DAI links to the codecs.  The DSP-specific properties of
// every element ride along as vendor tuple arrays in the elements' private
// data.
//
// This file does two things:
//
//   1. Parses topology files into an in-memory model.  The files come from
//      disk and are treated as hostile: every length, count, offset and
//      string is bounds checked before it is used, sizes are validated
//      against the fixed layouts, arithmetic is overflow safe, and names
//      that are not NUL terminated in the file are truncated.
//
//   2. Drives the firmware over IPC4 when a PCM is used: finds the path of
//      widgets from the host copier to the DAI copier(s), chooses the
//      audio formats of every module for the requested rate, channel count
//      and sample size, creates the pipelines, instantiates the modules with
//      their configuration (base config, copier gateway, gain, mixer, SRC
//      and process module payloads), binds them, configures the DAI side
//      (SoundWire streams, SSP/DMIC NHLT blobs, HDA link DMA) and moves the
//      pipelines through RESET -> PAUSED -> RUNNING and back.
//
// Locking: one operation runs at a time (`busy', guarded by the state's
// spinlock).  IPC messages are never sent with the spinlock held.  The
// trigger path can be called with interrupts disabled; it only busy-waits.
//
// Copyright (C) 2026 The LikeOS Project

#include "sof_regs.h"
#include "sof_tplg_internal.h"

#include <kernel/mm/memory.h>
#include <kernel/ke/firmware.h>
#include <kernel/ke/syscall.h>
#ifndef SOF_TPLG_HOST_TEST
#include <kernel/hal/acpi.h>
#endif

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

#define TPLG_TAG "sof-tplg"
#define tplg_err(fmt, ...) kprintf("[snd] " TPLG_TAG ": " fmt, ##__VA_ARGS__)
#define tplg_info(fmt, ...) kprintf("[snd] " TPLG_TAG ": " fmt, ##__VA_ARGS__)
#define tplg_dbg(fmt, ...)                                             \
	do {                                                           \
		if (tplg_debug)                                        \
			kprintf("[snd] " TPLG_TAG ": " fmt,            \
				##__VA_ARGS__);                        \
	} while (0)

/* Verbose tracing of parsing and IPC traffic (0 = quiet). */
static int tplg_debug;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline uint32_t umin32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static inline uint32_t umax32(uint32_t a, uint32_t b)
{
	return a > b ? a : b;
}

/* Copy a fixed-size, possibly unterminated name field from the file into a
 * SOF_TPLG_NAME_MAX buffer, always terminating it.  Control characters are
 * replaced so that dumps of hostile files stay printable. */
static void name_copy(char *dst, const char *src, size_t srclen)
{
	size_t i;

	for (i = 0; i + 1 < SOF_TPLG_NAME_MAX && i < srclen; i++) {
		char c = src[i];

		if (c == '\0')
			break;
		if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7f)
			c = '?';
		dst[i] = c;
	}
	dst[i] = '\0';
}

static int name_eq(const char *a, const char *b)
{
	return a[0] != '\0' && kstrcmp(a, b) == 0;
}

/* Case-insensitive substring search for the name heuristics. */
static int name_has(const char *hay, const char *needle)
{
	size_t n = kstrlen(needle), i, j;

	if (n == 0)
		return 1;
	for (i = 0; hay[i]; i++) {
		for (j = 0; j < n; j++) {
			char a = hay[i + j], b = needle[j];

			if (a == '\0')
				return 0;
			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == n)
			return 1;
	}
	return 0;
}

static void *zalloc(size_t n)
{
	void *p = kalloc(n);

	if (p)
		kmemset(p, 0, n);
	return p;
}

/* Overflow-checked "does [off, off+len) fit in size" */
static inline int range_ok(size_t size, size_t off, size_t len)
{
	return off <= size && len <= size - off;
}

/* Bounded append into a text buffer. */
struct tbuf {
	char *buf;
	size_t len, pos;
};

static void tb_printf(struct tbuf *tb, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!tb->buf || tb->pos + 1 >= tb->len)
		return;
	__builtin_va_start(ap, fmt);
	n = kvsnprintf(tb->buf + tb->pos, tb->len - tb->pos, fmt, ap);
	__builtin_va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= tb->len - tb->pos)
		tb->pos = tb->len - 1;
	else
		tb->pos += (size_t)n;
}

static const char *dapm_type_name(uint32_t id)
{
	static const char *const names[] = {
		"input",  "output",  "mux",	  "mixer",    "pga",
		"out_drv", "adc",    "dac",	  "switch",   "pre",
		"post",	  "aif_in",  "aif_out",  "dai_in",   "dai_out",
		"dai_link", "buffer", "scheduler", "effect",  "siggen",
		"src",	  "asrc",    "encoder",  "decoder",
	};

	return id < ARRAY_LEN(names) ? names[id] : "?";
}

static const char *dai_type_name(int t)
{
	switch (t) {
	case SOF_DAI_HDA:
		return "HDA";
	case SOF_DAI_ALH:
		return "ALH";
	case SOF_DAI_SSP:
		return "SSP";
	case SOF_DAI_DMIC:
		return "DMIC";
	default:
		return "none";
	}
}

static const char *pipe_state_name(int s)
{
	switch (s) {
	case IPC4_PIPE_UNINITIALIZED:
		return "uninit";
	case IPC4_PIPE_RESET:
		return "reset";
	case IPC4_PIPE_PAUSED:
		return "paused";
	case IPC4_PIPE_RUNNING:
		return "running";
	case IPC4_PIPE_EOS:
		return "eos";
	default:
		return "invalid";
	}
}

static const char *node_type_name(uint32_t t)
{
	switch (t) {
	case IPC4_NODE_HDA_HOST_OUT:
		return "hda-host-out";
	case IPC4_NODE_HDA_HOST_IN:
		return "hda-host-in";
	case IPC4_NODE_HDA_HOST_INOUT:
		return "hda-host-inout";
	case IPC4_NODE_HDA_LINK_OUT:
		return "hda-link-out";
	case IPC4_NODE_HDA_LINK_IN:
		return "hda-link-in";
	case IPC4_NODE_HDA_LINK_INOUT:
		return "hda-link-inout";
	case IPC4_NODE_DMIC_LINK_IN:
		return "dmic-in";
	case IPC4_NODE_I2S_LINK_OUT:
		return "i2s-out";
	case IPC4_NODE_I2S_LINK_IN:
		return "i2s-in";
	case IPC4_NODE_ALH_LINK_OUT:
		return "alh-out";
	case IPC4_NODE_ALH_LINK_IN:
		return "alh-in";
	default:
		return "node";
	}
}

/* "SSP", "DMIC", "HDA", "ALH" -> SOF_DAI_*; other DSP DAI kinds (i.MX SAI,
 * AMD ACP, MediaTek AFE) cannot occur on these Intel DSPs and map to NONE. */
static int dai_type_from_string(const char *s)
{
	if (!kstrcmp(s, "SSP"))
		return SOF_DAI_SSP;
	if (!kstrcmp(s, "DMIC"))
		return SOF_DAI_DMIC;
	if (!kstrcmp(s, "HDA"))
		return SOF_DAI_HDA;
	if (!kstrcmp(s, "ALH"))
		return SOF_DAI_ALH;
	return SOF_DAI_NONE;
}

static struct tplg_state *tplg_of(struct sof_dev *sdev)
{
	return sdev ? (struct tplg_state *)sdev->tplg : NULL;
}

/* ==========================================================================
 * One operation at a time
 *
 * Firmware IPC can take milliseconds, so the state is not protected by
 * holding a spinlock across it.  Instead an operation marks the topology
 * busy; anyone else waits (busy-waiting, which also works with interrupts
 * disabled in the trigger path) with a bound.
 * ========================================================================== */

#define TPLG_BUSY_WAIT_US 2000000u

static int tplg_op_begin(struct tplg_state *t)
{
	uint32_t waited = 0;

	for (;;) {
		uint64_t fl;
		int got = 0;

		tplg_lock_irqsave(&t->lock, &fl);
		if (!t->busy) {
			t->busy = 1;
			got = 1;
		}
		tplg_unlock_irqrestore(&t->lock, fl);
		if (got)
			return 0;
		if (waited >= TPLG_BUSY_WAIT_US)
			return -EBUSY;
		snd_udelay(20);
		waited += 20;
	}
}

static void tplg_op_end(struct tplg_state *t)
{
	uint64_t fl;

	tplg_lock_irqsave(&t->lock, &fl);
	t->busy = 0;
	tplg_unlock_irqrestore(&t->lock, fl);
}

/* ==========================================================================
 * Model storage
 * ========================================================================== */

static struct tplg_widget *widget_new(struct tplg_state *t)
{
	struct tplg_widget *w;

	if (t->nw >= TPLG_MAX_WIDGETS)
		return NULL;
	if (t->nw >= t->wcap) {
		int ncap = t->wcap ? t->wcap * 2 : 64;
		struct tplg_widget *nw;

		if (ncap > TPLG_MAX_WIDGETS)
			ncap = TPLG_MAX_WIDGETS;
		nw = krealloc(t->w, (size_t)ncap * sizeof(*nw));
		if (!nw)
			return NULL;
		t->w = nw;
		t->wcap = ncap;
	}
	w = &t->w[t->nw];
	kmemset(w, 0, sizeof(*w));
	w->pipe = -1;
	w->instance_id = -1;
	w->alh_group = -1;
	w->sdw_link = -1;
	w->in_fmt_idx = -1;
	w->out_fmt_idx = -1;
	return w;
}

static struct tplg_route *route_new(struct tplg_state *t)
{
	struct tplg_route *r;

	if (t->nr >= TPLG_MAX_ROUTES)
		return NULL;
	if (t->nr >= t->rcap) {
		int ncap = t->rcap ? t->rcap * 2 : 128;
		struct tplg_route *nr;

		if (ncap > TPLG_MAX_ROUTES)
			ncap = TPLG_MAX_ROUTES;
		nr = krealloc(t->r, (size_t)ncap * sizeof(*nr));
		if (!nr)
			return NULL;
		t->r = nr;
		t->rcap = ncap;
	}
	r = &t->r[t->nr];
	kmemset(r, 0, sizeof(*r));
	r->src = r->sink = -1;
	r->src_q = r->dst_q = -1;
	return r;
}

static void widget_release(struct tplg_widget *w)
{
	int i;

	if (w->in_fmts)
		kfree(w->in_fmts);
	if (w->out_fmts)
		kfree(w->out_fmts);
	w->in_fmts = w->out_fmts = NULL;
	for (i = 0; i < w->nkctl; i++) {
		if (w->kctl[i].bytes)
			kfree(w->kctl[i].bytes);
		w->kctl[i].bytes = NULL;
	}
	w->nkctl = 0;
}

static int widget_find(struct tplg_state *t, int file, const char *name)
{
	int i;

	if (!name[0])
		return -1;
	/* the same file first: names are only unique within one topology */
	for (i = 0; i < t->nw; i++)
		if (t->w[i].file == file && !kstrcmp(t->w[i].name, name))
			return i;
	for (i = 0; i < t->nw; i++)
		if (!kstrcmp(t->w[i].name, name))
			return i;
	return -1;
}

/* ==========================================================================
 * Vendor tuple arrays
 *
 * Private data is a sequence of arrays; each array carries elements of one
 * type (UUID, string or 32-bit value) tagged with a token number.
 * ========================================================================== */

struct tuple {
	uint32_t type; /* TUPLE_* */
	uint32_t token;
	uint32_t value; /* word/short/byte/bool */
	const char *str; /* terminated copy for strings */
	const uint8_t *uuid; /* 16 bytes for UUIDs */
};

typedef int (*tuple_fn)(void *ctx, const struct tuple *tp);

static int tuples_walk(const uint8_t *data, uint32_t size, tuple_fn fn,
		       void *ctx)
{
	uint32_t pos = 0;

	while (pos < size) {
		const struct tplg_vendor_array *va;
		uint32_t asz, type, n, esz, i;
		const uint8_t *e;

		if (size - pos < sizeof(*va))
			return -EINVAL;
		va = (const struct tplg_vendor_array *)(data + pos);
		asz = va->size;
		type = va->type;
		n = va->num_elems;
		if (asz < sizeof(*va) || asz > size - pos)
			return -EINVAL;
		switch (type) {
		case TUPLE_UUID:
			esz = sizeof(struct tplg_vendor_uuid_elem);
			break;
		case TUPLE_STRING:
			esz = sizeof(struct tplg_vendor_string_elem);
			break;
		case TUPLE_BOOL:
		case TUPLE_BYTE:
		case TUPLE_WORD:
		case TUPLE_SHORT:
			esz = sizeof(struct tplg_vendor_value_elem);
			break;
		default:
			return -EINVAL;
		}
		if (n > (asz - sizeof(*va)) / esz)
			return -EINVAL;
		e = data + pos + sizeof(*va);
		for (i = 0; i < n; i++, e += esz) {
			struct tuple tp;
			char sbuf[SOF_TPLG_NAME_MAX];
			int ret;

			kmemset(&tp, 0, sizeof(tp));
			tp.type = type;
			if (type == TUPLE_UUID) {
				const struct tplg_vendor_uuid_elem *u =
					(const void *)e;

				tp.token = u->token;
				tp.uuid = u->uuid;
			} else if (type == TUPLE_STRING) {
				const struct tplg_vendor_string_elem *s =
					(const void *)e;

				tp.token = s->token;
				name_copy(sbuf, s->string, TPLG_NAME_LEN);
				tp.str = sbuf;
			} else {
				const struct tplg_vendor_value_elem *v =
					(const void *)e;

				tp.token = v->token;
				tp.value = v->value;
				if (type == TUPLE_SHORT)
					tp.value &= 0xffffu;
				else if (type == TUPLE_BYTE)
					tp.value &= 0xffu;
				else if (type == TUPLE_BOOL)
					tp.value = tp.value ? 1 : 0;
			}
			ret = fn(ctx, &tp);
			if (ret < 0)
				return ret;
		}
		pos += asz;
	}
	return 0;
}

/* ---- widget tokens ------------------------------------------------------ */

/* Every audio format entry is described by a set of tokens; the N-th
 * occurrence of a token belongs to format N.  Pass 1 counts, pass 2 fills. */
#define FMT_TOKENS 10 /* 1900..1909 and 1930..1939 */

struct wparse {
	struct tplg_widget *w;
	int pass;
	uint32_t in_occ[FMT_TOKENS + 1]; /* + ibs */
	uint32_t out_occ[FMT_TOKENS + 1]; /* + obs */
	uint32_t num_in_tkn, num_out_tkn;
	int has_num_in, has_num_out;
};

static void fmt_set(struct tplg_fmt *f, uint32_t idx, uint32_t v)
{
	switch (idx) {
	case 0:
		f->rate = v;
		break;
	case 1:
		f->bit_depth = v;
		break;
	case 2:
		f->valid_bits = v;
		break;
	case 3:
		f->channels = v;
		break;
	case 4:
		f->ch_map = v;
		break;
	case 5:
		f->ch_cfg = v;
		break;
	case 6:
		f->interleaving = v;
		break;
	case 7:
		f->fmt_cfg = v;
		break;
	case 8:
		f->sample_type = v;
		break;
	case 9:
		f->pin_index = v;
		break;
	case 10:
		f->buffer_size = v;
		break;
	}
}

static int widget_tuple(void *ctx, const struct tuple *tp)
{
	struct wparse *wp = ctx;
	struct tplg_widget *w = wp->w;
	uint32_t v = tp->value;
	uint32_t tok = tp->token;

	/* audio format tokens: counted in pass 1, stored in pass 2 */
	if ((tok >= TKN_FMT_IN_RATE && tok <= TKN_FMT_IN_PIN_INDEX) ||
	    tok == TKN_FMT_IBS) {
		uint32_t k = tok == TKN_FMT_IBS ? FMT_TOKENS :
						  tok - TKN_FMT_IN_RATE;

		if (tp->type == TUPLE_STRING || tp->type == TUPLE_UUID)
			return 0;
		if (wp->pass == 2 && wp->in_occ[k] < w->num_in_fmts)
			fmt_set(&w->in_fmts[wp->in_occ[k]], k, v);
		if (wp->in_occ[k] < 0xffffu)
			wp->in_occ[k]++;
		return 0;
	}
	if ((tok >= TKN_FMT_OUT_RATE && tok <= TKN_FMT_OUT_PIN_INDEX) ||
	    tok == TKN_FMT_OBS) {
		uint32_t k = tok == TKN_FMT_OBS ? FMT_TOKENS :
						  tok - TKN_FMT_OUT_RATE;

		if (tp->type == TUPLE_STRING || tp->type == TUPLE_UUID)
			return 0;
		if (wp->pass == 2 && wp->out_occ[k] < w->num_out_fmts)
			fmt_set(&w->out_fmts[wp->out_occ[k]], k, v);
		if (wp->out_occ[k] < 0xffffu)
			wp->out_occ[k]++;
		return 0;
	}
	if (wp->pass != 1)
		return 0;

	if (tp->type == TUPLE_UUID) {
		if (tok == TKN_COMP_UUID) {
			kmemcpy(w->uuid, tp->uuid, 16);
			w->has_uuid = 1;
		}
		return 0;
	}
	if (tp->type == TUPLE_STRING) {
		switch (tok) {
		case TKN_DAI_TYPE:
			name_copy(w->dai_type_str, tp->str,
				  sizeof(w->dai_type_str));
			w->dai_type = dai_type_from_string(w->dai_type_str);
			break;
		case TKN_COMP_SCHED_DOMAIN:
			if (!kstrcmp(tp->str, "LL"))
				w->domain = TPLG_DOMAIN_LL;
			else if (!kstrcmp(tp->str, "DP"))
				w->domain = TPLG_DOMAIN_DP;
			break;
		case TKN_COMP_INPUT_PIN_BINDING_WNAME:
			if (w->n_in_bind < TPLG_MAX_PINS)
				name_copy(w->in_bind[w->n_in_bind++], tp->str,
					  SOF_TPLG_NAME_MAX);
			break;
		case TKN_COMP_OUTPUT_PIN_BINDING_WNAME:
			if (w->n_out_bind < TPLG_MAX_PINS)
				name_copy(w->out_bind[w->n_out_bind++],
					  tp->str, SOF_TPLG_NAME_MAX);
			break;
		case TKN_PROCESS_TYPE:
			name_copy(w->process_type, tp->str, SOF_TPLG_NAME_MAX);
			break;
		default:
			break;
		}
		return 0;
	}

	switch (tok) {
	case TKN_BUF_SIZE:
		w->buf_size = v;
		break;
	case TKN_BUF_CAPS:
		w->buf_caps = v;
		break;
	case TKN_DAI_INDEX:
		w->dai_index = v;
		w->has_dai_index = 1;
		break;
	case TKN_DAI_DIRECTION:
		w->dai_direction = v;
		break;
	case TKN_SCHED_PERIOD:
		w->period = v;
		break;
	case TKN_SCHED_PRIORITY:
		w->priority = v;
		break;
	case TKN_SCHED_CORE:
		w->sched_core = v;
		break;
	case TKN_SCHED_FRAMES:
		w->frames = v;
		break;
	case TKN_SCHED_TIME_DOMAIN:
		w->time_domain = v;
		break;
	case TKN_SCHED_DYNAMIC_PIPELINE:
		w->dynamic = v;
		break;
	case TKN_SCHED_LP_MODE:
		w->lp_mode = v;
		break;
	case TKN_SCHED_MEM_USAGE:
		w->mem_usage_tkn = v;
		break;
	case TKN_SCHED_USE_CHAIN_DMA:
		w->use_chain_dma = v;
		break;
	case TKN_SCHED_KCPS:
		w->kcps = v;
		break;
	case TKN_SCHED_DIRECTION:
		w->direction = v;
		w->direction_valid = 1;
		break;
	case TKN_SCHED_DIRECTION_VALID:
		w->direction_valid = v ? 1 : 0;
		break;
	case TKN_GAIN_RAMP_TYPE:
	case TKN_VOLUME_RAMP_STEP_TYPE:
		w->gain_ramp_type = v;
		break;
	case TKN_GAIN_RAMP_DURATION:
	case TKN_VOLUME_RAMP_STEP_MS:
		w->gain_ramp_duration = v;
		break;
	case TKN_GAIN_VAL:
		w->gain_init = v;
		w->has_gain_init = 1;
		break;
	case TKN_SRC_RATE_IN:
		w->src_rate_in = v;
		break;
	case TKN_SRC_RATE_OUT:
		w->src_rate_out = v;
		break;
	case TKN_ASRC_RATE_IN:
		w->src_rate_in = v;
		break;
	case TKN_ASRC_RATE_OUT:
		w->src_rate_out = v;
		break;
	case TKN_ASRC_ASYNCHRONOUS_MODE:
		w->asrc_async = v;
		break;
	case TKN_ASRC_OPERATION_MODE:
		w->asrc_mode = v;
		break;
	case TKN_COMP_CORE_ID:
		w->core = v;
		break;
	case TKN_COMP_CPC:
		w->cpc = v;
		break;
	case TKN_COMP_IS_PAGES:
		w->is_pages = v;
		break;
	case TKN_COMP_NUM_INPUT_PINS:
		w->num_input_pins = v;
		break;
	case TKN_COMP_NUM_OUTPUT_PINS:
		w->num_output_pins = v;
		break;
	case TKN_COMP_NUM_INPUT_AUDIO_FORMATS:
		wp->num_in_tkn = v;
		wp->has_num_in = 1;
		break;
	case TKN_COMP_NUM_OUTPUT_AUDIO_FORMATS:
		wp->num_out_tkn = v;
		wp->has_num_out = 1;
		break;
	case TKN_COMP_DOMAIN_ID:
		w->domain_id = v;
		break;
	case TKN_COMP_STACK_BYTES:
		w->stack_bytes = v;
		break;
	case TKN_COMP_HEAP_BYTES:
		w->heap_bytes = v;
		break;
	case TKN_MIXER_TYPE:
		w->mixer_type = v;
		break;
	case TKN_COPIER_NODE_TYPE:
		w->node_type = v;
		w->has_node_type = 1;
		break;
	case TKN_COPIER_DEEP_BUFFER_DMA_MS:
		w->deep_buffer_ms = v;
		break;
	default:
		break;
	}
	return 0;
}

/* Compose fmt_cfg from the separate tokens when the topology did not
 * provide it, and fill in the separate fields from fmt_cfg otherwise. */
static void fmt_normalize(struct tplg_fmt *f)
{
	uint32_t ch = IPC4_FMT_CFG_CHANNELS(f->fmt_cfg);
	uint32_t vb = IPC4_FMT_CFG_VALID_BITS(f->fmt_cfg);
	uint32_t st = IPC4_FMT_CFG_SAMPLE_TYPE(f->fmt_cfg);

	if (!ch && f->channels)
		ch = f->channels & 0xff;
	if (!vb)
		vb = f->valid_bits ? f->valid_bits & 0xff :
				     f->bit_depth & 0xff;
	if (f->sample_type != 0xffffffffu && !st)
		st = f->sample_type & 0xff;
	f->fmt_cfg = ch | (vb << 8) | (st << 16);
	if (!f->channels)
		f->channels = ch;
	if (!f->valid_bits)
		f->valid_bits = vb;
	if (!f->bit_depth)
		f->bit_depth = vb > 16 ? 32 : vb;
	if (f->sample_type == 0xffffffffu)
		f->sample_type = st;
}

static int widget_parse_tokens(struct tplg_widget *w, const uint8_t *priv,
			       uint32_t psize)
{
	struct wparse wp;
	uint32_t nin = 0, nout = 0, k, i;
	int ret;

	kmemset(&wp, 0, sizeof(wp));
	wp.w = w;
	wp.pass = 1;
	ret = tuples_walk(priv, psize, widget_tuple, &wp);
	if (ret < 0)
		return ret;

	for (k = 0; k <= FMT_TOKENS; k++) {
		nin = umax32(nin, wp.in_occ[k]);
		nout = umax32(nout, wp.out_occ[k]);
	}
	if (wp.has_num_in)
		nin = wp.num_in_tkn;
	if (wp.has_num_out)
		nout = wp.num_out_tkn;
	if (nin > TPLG_MAX_FMTS || nout > TPLG_MAX_FMTS) {
		tplg_err("widget %s: %u/%u audio formats is too many\n",
			 w->name, nin, nout);
		return -E2BIG;
	}
	if (nin) {
		w->in_fmts = zalloc(nin * sizeof(struct tplg_fmt));
		if (!w->in_fmts)
			return -ENOMEM;
	}
	if (nout) {
		w->out_fmts = zalloc(nout * sizeof(struct tplg_fmt));
		if (!w->out_fmts)
			return -ENOMEM;
	}
	w->num_in_fmts = nin;
	w->num_out_fmts = nout;
	for (i = 0; i < nin; i++)
		w->in_fmts[i].sample_type = 0xffffffffu;
	for (i = 0; i < nout; i++)
		w->out_fmts[i].sample_type = 0xffffffffu;

	if (nin || nout) {
		kmemset(wp.in_occ, 0, sizeof(wp.in_occ));
		kmemset(wp.out_occ, 0, sizeof(wp.out_occ));
		wp.pass = 2;
		ret = tuples_walk(priv, psize, widget_tuple, &wp);
		if (ret < 0)
			return ret;
	}
	for (i = 0; i < nin; i++)
		fmt_normalize(&w->in_fmts[i]);
	for (i = 0; i < nout; i++)
		fmt_normalize(&w->out_fmts[i]);

	/* every DSP module has at least one pin each way */
	if (!w->num_input_pins)
		w->num_input_pins = 1;
	if (!w->num_output_pins)
		w->num_output_pins = 1;
	if (w->num_input_pins > TPLG_MAX_PINS ||
	    w->num_output_pins > TPLG_MAX_PINS) {
		tplg_err("widget %s: %u/%u pins is too many\n", w->name,
			 w->num_input_pins, w->num_output_pins);
		return -EINVAL;
	}
	if (w->core > 15)
		w->core = 0;
	if (w->sched_core > 15)
		w->sched_core = 0;
	if (w->priority > 31)
		w->priority = 31;
	return 0;
}

/* ==========================================================================
 * Controls
 * ========================================================================== */

static int ctl_kind_of(uint32_t info)
{
	switch (info) {
	case TPLG_CTL_VOLSW:
	case TPLG_CTL_STROBE:
	case TPLG_CTL_VOLSW_SX:
	case TPLG_CTL_VOLSW_XR_SX:
	case TPLG_CTL_RANGE:
	case TPLG_DAPM_CTL_VOLSW:
	case TPLG_DAPM_CTL_PIN:
		return TPLG_TYPE_MIXER;
	case TPLG_CTL_ENUM:
	case TPLG_CTL_ENUM_VALUE:
	case TPLG_DAPM_CTL_ENUM_DOUBLE:
	case TPLG_DAPM_CTL_ENUM_VIRT:
	case TPLG_DAPM_CTL_ENUM_VALUE:
		return TPLG_TYPE_ENUM;
	case TPLG_CTL_BYTES:
		return TPLG_TYPE_BYTES;
	default:
		return 0;
	}
}

/* Parse one control element at buf[pos].  Stores it into `out' when not
 * NULL.  Returns the element's total size (fixed part + private data) or a
 * negative errno. */
static long ctl_parse(const uint8_t *buf, size_t size, size_t pos,
		      struct tplg_kctl *out)
{
	const struct tplg_ctl_hdr *h;
	uint32_t psize;
	size_t fixed;
	int kind;

	if (!range_ok(size, pos, sizeof(*h)))
		return -EINVAL;
	h = (const struct tplg_ctl_hdr *)(buf + pos);
	if (h->size != sizeof(*h))
		return -EINVAL;
	kind = ctl_kind_of(h->ops.info);
	switch (kind) {
	case TPLG_TYPE_MIXER: {
		const struct tplg_mixer_control *mc;

		fixed = sizeof(*mc);
		if (!range_ok(size, pos, fixed))
			return -EINVAL;
		mc = (const void *)(buf + pos);
		if (mc->size != fixed)
			return -EINVAL;
		psize = mc->priv.size;
		if (!range_ok(size, pos + fixed, psize))
			return -EINVAL;
		if (out) {
			out->min = mc->min;
			out->max = mc->max;
			out->invert = mc->invert;
			out->num_channels = umin32(mc->num_channels,
						   TPLG_MAX_CHAN);
			if (mc->max > 0x10000u)
				out->max = 0x10000u;
		}
		break;
	}
	case TPLG_TYPE_ENUM: {
		const struct tplg_enum_control *ec;

		fixed = sizeof(*ec);
		if (!range_ok(size, pos, fixed))
			return -EINVAL;
		ec = (const void *)(buf + pos);
		if (ec->size != fixed)
			return -EINVAL;
		psize = ec->priv.size;
		if (!range_ok(size, pos + fixed, psize))
			return -EINVAL;
		if (out) {
			out->items = umin32(ec->items, TPLG_NUM_TEXTS);
			out->num_channels = umin32(ec->num_channels,
						   TPLG_MAX_CHAN);
		}
		break;
	}
	case TPLG_TYPE_BYTES: {
		const struct tplg_bytes_control *bc;

		fixed = sizeof(*bc);
		if (!range_ok(size, pos, fixed))
			return -EINVAL;
		bc = (const void *)(buf + pos);
		if (bc->size != fixed)
			return -EINVAL;
		psize = bc->priv.size;
		if (!range_ok(size, pos + fixed, psize))
			return -EINVAL;
		if (out) {
			const uint8_t *pd = buf + pos + fixed;

			out->bytes_max = bc->max;
			/* keep the default blob: sof_abi_hdr + data */
			if (psize >= SOF_ABI_HDR_SIZE &&
			    psize <= TPLG_MAX_BYTES_DATA) {
				uint32_t magic, dsize;

				kmemcpy(&magic, pd, 4);
				kmemcpy(&dsize, pd + 8, 4);
				if (magic == SOF_ABI_MAGIC &&
				    dsize <= psize - SOF_ABI_HDR_SIZE) {
					out->bytes = kalloc(psize);
					if (out->bytes) {
						kmemcpy(out->bytes, pd, psize);
						out->bytes_len = psize;
					}
				}
			}
		}
		break;
	}
	default:
		return -EINVAL;
	}

	if (out) {
		out->kind = (uint32_t)kind;
		name_copy(out->name, h->name, TPLG_NAME_LEN);
		out->info = h->ops.info;
		out->get = h->ops.get;
		out->put = h->ops.put;
		out->access = h->access;
		if (h->tlv.type == TPLG_TLVT_DB_SCALE && h->tlv.size > 0) {
			out->has_db = 1;
			out->db_min = (int32_t)h->tlv.scale.min;
			out->db_step = h->tlv.scale.step & 0xffffu;
			out->db_mute = h->tlv.scale.mute ? 1 : 0;
		}
		out->value[0] = out->value[1] = out->max;
	}
	if (fixed + (size_t)psize > 0x7fffffffu)
		return -EINVAL;
	return (long)(fixed + psize);
}

/* ==========================================================================
 * Blocks
 * ========================================================================== */

/* graph routes are collected by name and resolved after the whole file is
 * parsed (a graph block may come before the widgets it names) */
struct pending_route {
	char sink[SOF_TPLG_NAME_MAX];
	char source[SOF_TPLG_NAME_MAX];
	char control[SOF_TPLG_NAME_MAX];
};

struct load_ctx {
	struct tplg_state *t;
	int file;
	const uint8_t *buf;
	size_t size;
	struct pending_route *pr;
	int npr, prcap;
	int widgets, routes, pcms, links, dais, controls;
	int first_widget; /* index of this file's first widget */
	int first_pcm, first_link;
};

static int parse_manifest(struct load_ctx *lc, const uint8_t *p, size_t len,
			  uint32_t count)
{
	struct tplg_state *t = lc->t;
	const struct tplg_manifest *m;
	const uint8_t *pd;
	uint32_t psize, items, i, off;
	uint16_t v16[4];

	if (count != 1 && count != 0)
		tplg_dbg("manifest block with %u elements\n", count);
	if (len < sizeof(*m))
		return -EINVAL;
	m = (const struct tplg_manifest *)p;
	if (m->size != sizeof(*m))
		return -EINVAL;
	psize = m->priv.size;
	if (!range_ok(len, sizeof(*m), psize))
		return -EINVAL;
	tplg_dbg("manifest: %u controls %u widgets %u routes %u pcms %u links\n",
		 m->control_elems, m->widget_elems, m->graph_elems,
		 m->pcm_elems, m->dai_link_elems);
	if (psize < SOF_MANIFEST_HDR_SIZE)
		return 0;

	/* the DSP manifest: ABI version and optional data items (an NHLT
	 * table for machines whose BIOS has none) */
	pd = p + sizeof(*m);
	kmemcpy(v16, pd, sizeof(v16));
	t->abi_major = v16[0];
	t->abi_minor = v16[1];
	t->abi_patch = v16[2];
	items = v16[3];
	off = SOF_MANIFEST_HDR_SIZE;
	for (i = 0; i < items && i < 64; i++) {
		uint32_t type, sz;

		if (!range_ok(psize, off, SOF_MANIFEST_TLV_HDR))
			break;
		kmemcpy(&type, pd + off, 4);
		kmemcpy(&sz, pd + off + 4, 4);
		off += SOF_MANIFEST_TLV_HDR;
		if (!range_ok(psize, off, sz))
			break;
		if (type == SOF_MANIFEST_DATA_TYPE_NHLT && sz >= 37 &&
		    sz <= (1u << 20) && t->nnhlt < TPLG_MAX_NHLT) {
			uint8_t *copy = kalloc(sz);

			if (copy) {
				kmemcpy(copy, pd + off, sz);
				t->nhlt[t->nnhlt] = copy;
				t->nhlt_len[t->nnhlt] = sz;
				t->nnhlt++;
				tplg_dbg("manifest carries an NHLT (%u bytes)\n",
					 sz);
			}
		}
		off += sz;
	}
	return 0;
}

static int parse_widgets(struct load_ctx *lc, const uint8_t *p, size_t len,
			 uint32_t count, uint32_t index)
{
	struct tplg_state *t = lc->t;
	size_t pos = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		const struct tplg_dapm_widget *tw;
		struct tplg_widget *w;
		uint32_t psize, nk, k;
		int ret;

		if (!range_ok(len, pos, sizeof(*tw)))
			return -EINVAL;
		tw = (const struct tplg_dapm_widget *)(p + pos);
		if (tw->size != sizeof(*tw))
			return -EINVAL;
		psize = tw->priv.size;
		if (!range_ok(len, pos + sizeof(*tw), psize))
			return -EINVAL;
		if (tw->id > DAPM_TYPE_LAST)
			return -EINVAL;

		w = widget_new(t);
		if (!w)
			return -ENOSPC;
		w->file = lc->file;
		w->pipe_index = index;
		w->id = tw->id;
		name_copy(w->name, tw->name, TPLG_NAME_LEN);
		name_copy(w->sname, tw->sname, TPLG_NAME_LEN);
		t->nw++; /* owned from here on: freed with the state */

		ret = widget_parse_tokens(w, p + pos + sizeof(*tw), psize);
		if (ret < 0) {
			tplg_err("widget %s: bad private data (%d)\n", w->name,
				 ret);
			return ret;
		}
		pos += sizeof(*tw) + psize;

		/* the widget's controls follow its private data */
		nk = tw->num_kcontrols;
		if (nk > 64)
			return -EINVAL;
		for (k = 0; k < nk; k++) {
			struct tplg_kctl *kc = NULL;
			long sz;

			if (w->nkctl < TPLG_MAX_KCTLS)
				kc = &w->kctl[w->nkctl];
			sz = ctl_parse(p, len, pos, kc);
			if (sz < 0) {
				tplg_err("widget %s: bad control %u\n",
					 w->name, k);
				return (int)sz;
			}
			if (kc)
				w->nkctl++;
			pos += (size_t)sz;
			lc->controls++;
		}
		lc->widgets++;
	}
	if (pos != len)
		tplg_dbg("widget block: %zu trailing bytes\n",
			 (size_t)(len - pos));
	return 0;
}

static int parse_graph(struct load_ctx *lc, const uint8_t *p, size_t len,
		       uint32_t count)
{
	uint32_t i;

	if (count > TPLG_MAX_ROUTES ||
	    (size_t)count * sizeof(struct tplg_graph_elem) > len)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct tplg_graph_elem *g =
			(const void *)(p + (size_t)i * sizeof(*g));
		struct pending_route *pr;

		if (lc->npr >= lc->prcap) {
			int ncap = lc->prcap ? lc->prcap * 2 : 64;
			struct pending_route *np;

			if (ncap > TPLG_MAX_ROUTES)
				return -ENOSPC;
			np = krealloc(lc->pr, (size_t)ncap * sizeof(*np));
			if (!np)
				return -ENOMEM;
			lc->pr = np;
			lc->prcap = ncap;
		}
		pr = &lc->pr[lc->npr++];
		name_copy(pr->sink, g->sink, TPLG_NAME_LEN);
		name_copy(pr->source, g->source, TPLG_NAME_LEN);
		name_copy(pr->control, g->control, TPLG_NAME_LEN);
	}
	return 0;
}

/* SNDRV-style format bits -> AUDIO_FMT_BIT mask */
static uint32_t pcm_formats_to_audio(uint64_t f)
{
	uint32_t m = 0;

	if (f & (1ull << PCMF_U8))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_U8);
	if (f & (1ull << PCMF_S16_LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
	if (f & (1ull << PCMF_S24_LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_S24_LE);
	if (f & (1ull << PCMF_S32_LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_S32_LE);
	if (f & (1ull << PCMF_FLOAT_LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_FLOAT_LE);
	if (f & (1ull << PCMF_S20_LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_S20_LE);
	if (f & (1ull << PCMF_S24_3LE))
		m |= AUDIO_FMT_BIT(AUDIO_FMT_S24_3LE);
	return m;
}

static const struct {
	uint32_t hz;
	uint32_t pcm_bit; /* bit in the file's rate mask */
	uint32_t audio_bit; /* AUDIO_RATE_* */
} rate_map[] = {
	{ 8000, 1u << 1, AUDIO_RATE_8000 },
	{ 11025, 1u << 2, AUDIO_RATE_11025 },
	{ 16000, 1u << 3, AUDIO_RATE_16000 },
	{ 22050, 1u << 4, AUDIO_RATE_22050 },
	{ 32000, 1u << 5, AUDIO_RATE_32000 },
	{ 44100, 1u << 6, AUDIO_RATE_44100 },
	{ 48000, 1u << 7, AUDIO_RATE_48000 },
	{ 88200, 1u << 9, AUDIO_RATE_88200 },
	{ 96000, 1u << 10, AUDIO_RATE_96000 },
	{ 176400, 1u << 11, AUDIO_RATE_176400 },
	{ 192000, 1u << 12, AUDIO_RATE_192000 },
	{ 384000, 1u << 14, AUDIO_RATE_384000 },
};

static uint32_t rate_hz_to_audio(uint32_t hz)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(rate_map); i++)
		if (rate_map[i].hz == hz)
			return rate_map[i].audio_bit;
	return 0;
}

static uint32_t pcm_rates_to_audio(uint32_t rates, uint32_t rmin,
				   uint32_t rmax)
{
	uint32_t m = 0;
	size_t i;

	for (i = 0; i < ARRAY_LEN(rate_map); i++)
		if (rates & rate_map[i].pcm_bit)
			m |= rate_map[i].audio_bit;
	/* continuous ranges, or no explicit list: take the min..max span */
	if ((rates & (PCMR_CONTINUOUS | PCMR_KNOT)) || !m) {
		if (rmin && rmax >= rmin)
			for (i = 0; i < ARRAY_LEN(rate_map); i++)
				if (rate_map[i].hz >= rmin &&
				    rate_map[i].hz <= rmax)
					m |= rate_map[i].audio_bit;
	}
	return m;
}

struct pcm_tok_ctx {
	struct tplg_pcm_int *pc;
};

static int pcm_tuple(void *ctx, const struct tuple *tp)
{
	struct pcm_tok_ctx *c = ctx;

	if (tp->type == TUPLE_STRING || tp->type == TUPLE_UUID)
		return 0;
	switch (tp->token) {
	case TKN_STREAM_PLAYBACK_D0I3:
		c->pc->d0i3_compatible[0] = tp->value ? 1 : 0;
		break;
	case TKN_STREAM_CAPTURE_D0I3:
		c->pc->d0i3_compatible[1] = tp->value ? 1 : 0;
		break;
	case TKN_STREAM_PLAYBACK_PAUSE:
		c->pc->pause_supported[0] = tp->value ? 1 : 0;
		break;
	case TKN_STREAM_CAPTURE_PAUSE:
		c->pc->pause_supported[1] = tp->value ? 1 : 0;
		break;
	default:
		break;
	}
	return 0;
}

static int parse_pcms(struct load_ctx *lc, const uint8_t *p, size_t len,
		      uint32_t count)
{
	struct tplg_state *t = lc->t;
	size_t pos = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		const struct tplg_pcm *tp;
		struct tplg_pcm_int *pc;
		struct pcm_tok_ctx c;
		uint32_t psize;
		int d, ret;

		if (!range_ok(len, pos, sizeof(*tp)))
			return -EINVAL;
		tp = (const struct tplg_pcm *)(p + pos);
		if (tp->size != sizeof(*tp))
			return -EINVAL;
		psize = tp->priv.size;
		if (!range_ok(len, pos + sizeof(*tp), psize))
			return -EINVAL;
		/* an absent direction may leave its caps all zero */
		for (d = 0; d < 2; d++)
			if (tp->caps[d].size != sizeof(struct tplg_stream_caps) &&
			    tp->caps[d].size != 0)
				return -EINVAL;

		if (t->npcm >= TPLG_MAX_PCMS) {
			tplg_err("too many PCMs, ignoring the rest\n");
			pos += sizeof(*tp) + psize;
			continue;
		}
		if (tp->compress) {
			/* compressed (offload) streams are not offered */
			pos += sizeof(*tp) + psize;
			continue;
		}
		pc = &t->pcm[t->npcm];
		kmemset(pc, 0, sizeof(*pc));
		pc->file = lc->file;
		pc->pub.id = (int)(tp->pcm_id & 0x7fffffff);
		name_copy(pc->pub.name, tp->pcm_name, TPLG_NAME_LEN);
		name_copy(pc->dai_name, tp->dai_name, TPLG_NAME_LEN);
		pc->pub.has_playback = tp->playback ? 1 : 0;
		pc->pub.has_capture = tp->capture ? 1 : 0;
		for (d = 0; d < 2; d++) {
			const struct tplg_stream_caps *cp = &tp->caps[d];

			pc->host[d] = -1;
			pc->rt[d].gain = -1;
			pc->pause_supported[d] = 1;
			if (!cp->size)
				continue;
			name_copy(pc->cap_name[d], cp->name, TPLG_NAME_LEN);
			pc->pcm_formats[d] = cp->formats;
			pc->pcm_rates[d] = cp->rates;
			pc->rate_min[d] = cp->rate_min;
			pc->rate_max[d] = cp->rate_max;
			pc->ch_min[d] = cp->channels_min;
			pc->ch_max[d] = cp->channels_max;
		}
		pc->vol_l = pc->vol_r = 100;
		c.pc = pc;
		ret = tuples_walk(p + pos + sizeof(*tp), psize, pcm_tuple, &c);
		if (ret < 0) {
			tplg_err("PCM %s: bad private data\n", pc->pub.name);
			return ret;
		}
		t->npcm++;
		lc->pcms++;
		pos += sizeof(*tp) + psize;
	}
	return 0;
}

struct link_tok_ctx {
	struct tplg_link *l;
};

static int link_tuple(void *ctx, const struct tuple *tp)
{
	struct tplg_link *l = ((struct link_tok_ctx *)ctx)->l;
	uint32_t v = tp->value;

	if (tp->type == TUPLE_UUID)
		return 0;
	if (tp->type == TUPLE_STRING) {
		if (tp->token == TKN_DAI_TYPE)
			l->dai_type = dai_type_from_string(tp->str);
		return 0;
	}
	switch (tp->token) {
	case TKN_INTEL_ALH_RATE:
		l->alh_rate = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_ALH;
		break;
	case TKN_INTEL_ALH_CH:
		l->alh_ch = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_ALH;
		break;
	case TKN_INTEL_HDA_RATE:
		l->hda_rate = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_HDA;
		break;
	case TKN_INTEL_HDA_CH:
		l->hda_ch = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_HDA;
		break;
	case TKN_INTEL_SSP_SAMPLE_BITS:
		l->ssp_sample_bits = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_SSP;
		break;
	case TKN_INTEL_SSP_MCLK_ID:
		l->ssp_mclk_id = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_SSP;
		break;
	case TKN_INTEL_DMIC_SAMPLE_RATE:
		l->dmic_rate = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_DMIC;
		break;
	case TKN_INTEL_DMIC_NUM_PDM_ACTIVE:
		l->dmic_pdm_active = v;
		if (!l->dai_type)
			l->dai_type = SOF_DAI_DMIC;
		break;
	default:
		break;
	}
	return 0;
}

static int parse_links(struct load_ctx *lc, const uint8_t *p, size_t len,
		       uint32_t count)
{
	struct tplg_state *t = lc->t;
	size_t pos = 0;
	uint32_t i;

	for (i = 0; i < count; i++) {
		const struct tplg_link_config *lk;
		struct tplg_link *l;
		struct link_tok_ctx c;
		uint32_t psize, nhw, h;
		int ret;

		if (!range_ok(len, pos, sizeof(*lk)))
			return -EINVAL;
		lk = (const struct tplg_link_config *)(p + pos);
		if (lk->size != sizeof(*lk))
			return -EINVAL;
		psize = lk->priv.size;
		if (!range_ok(len, pos + sizeof(*lk), psize))
			return -EINVAL;
		nhw = lk->num_hw_configs;
		if (nhw > TPLG_HW_CONFIG_MAX || lk->num_streams >
							TPLG_STREAM_CONFIG_MAX)
			return -EINVAL;

		if (t->nlink >= TPLG_MAX_LINKS) {
			pos += sizeof(*lk) + psize;
			continue;
		}
		l = &t->link[t->nlink];
		kmemset(l, 0, sizeof(*l));
		l->file = lc->file;
		l->id = lk->id;
		name_copy(l->name, lk->name, TPLG_NAME_LEN);
		name_copy(l->stream_name, lk->stream_name, TPLG_NAME_LEN);
		l->num_hw_configs = (int)nhw;
		/* the default (or first) hardware configuration: what an SSP
		 * port runs at, used to pick its NHLT blob */
		for (h = 0; h < nhw; h++) {
			const struct tplg_hw_config *hc = &lk->hw_config[h];

			if (hc->size != sizeof(*hc))
				continue;
			if (h == 0 || hc->id == lk->default_hw_config_id) {
				l->hw_rate = hc->fsync_rate;
				l->hw_channels = hc->tdm_slots;
				l->hw_slot_width = hc->tdm_slot_width;
			}
		}
		c.l = l;
		ret = tuples_walk(p + pos + sizeof(*lk), psize, link_tuple, &c);
		if (ret < 0) {
			tplg_err("DAI link %s: bad private data\n", l->name);
			return ret;
		}
		t->nlink++;
		lc->links++;
		pos += sizeof(*lk) + psize;
	}
	return 0;
}

static int parse_dais(struct load_ctx *lc, const uint8_t *p, size_t len,
		      uint32_t count)
{
	size_t pos = 0;
	uint32_t i;

	/* physical DAI capabilities: validated, nothing kept (the DSP side
	 * takes its formats from the copier widgets) */
	for (i = 0; i < count; i++) {
		const struct tplg_dai *d;
		uint32_t psize;

		if (!range_ok(len, pos, sizeof(*d)))
			return -EINVAL;
		d = (const struct tplg_dai *)(p + pos);
		if (d->size != sizeof(*d))
			return -EINVAL;
		psize = d->priv.size;
		if (!range_ok(len, pos + sizeof(*d), psize))
			return -EINVAL;
		pos += sizeof(*d) + psize;
		lc->dais++;
	}
	return 0;
}

static int parse_controls(struct load_ctx *lc, const uint8_t *p, size_t len,
			  uint32_t count)
{
	size_t pos = 0;
	uint32_t i;

	/* stand-alone controls (not attached to a widget) */
	for (i = 0; i < count; i++) {
		long sz = ctl_parse(p, len, pos, NULL);

		if (sz < 0)
			return (int)sz;
		pos += (size_t)sz;
		lc->controls++;
	}
	return 0;
}

static int parse_block(struct load_ctx *lc, const struct tplg_hdr *h,
		       const uint8_t *p, size_t len)
{
	switch (h->type) {
	case TPLG_TYPE_MANIFEST:
		return parse_manifest(lc, p, len, h->count);
	case TPLG_TYPE_DAPM_WIDGET:
		return parse_widgets(lc, p, len, h->count, h->index);
	case TPLG_TYPE_DAPM_GRAPH:
		return parse_graph(lc, p, len, h->count);
	case TPLG_TYPE_PCM:
		return parse_pcms(lc, p, len, h->count);
	case TPLG_TYPE_DAI_LINK:
	case TPLG_TYPE_BACKEND_LINK:
		return parse_links(lc, p, len, h->count);
	case TPLG_TYPE_DAI:
		return parse_dais(lc, p, len, h->count);
	case TPLG_TYPE_MIXER:
	case TPLG_TYPE_BYTES:
	case TPLG_TYPE_ENUM:
		return parse_controls(lc, p, len, h->count);
	case TPLG_TYPE_CODEC_LINK:
	case TPLG_TYPE_PDATA:
	case TPLG_TYPE_VENDOR_FW:
	case TPLG_TYPE_VENDOR_CONFIG:
	case TPLG_TYPE_VENDOR_COEFF:
	case TPLG_TYPE_VENDOR_CODEC:
	default:
		tplg_dbg("skipping block type %u (%zu bytes)\n", h->type,
			 (size_t)len);
		return 0;
	}
}

/* ==========================================================================
 * Resolution after a file is parsed
 * ========================================================================== */

static int pipe_lookup(struct tplg_state *t, int file, uint32_t index,
		       int create)
{
	int i;

	for (i = 0; i < t->np; i++)
		if (t->pipes[i].file == file && t->pipes[i].index == index)
			return i;
	if (!create || t->np >= TPLG_MAX_PIPES)
		return -1;
	kmemset(&t->pipes[t->np], 0, sizeof(t->pipes[0]));
	t->pipes[t->np].file = file;
	t->pipes[t->np].index = index;
	t->pipes[t->np].sched = -1;
	t->pipes[t->np].instance_id = -1;
	t->pipes[t->np].state = IPC4_PIPE_UNINITIALIZED;
	return t->np++;
}

/* Is this a widget the firmware instantiates as a module? */
static int widget_is_module(const struct tplg_widget *w)
{
	switch (w->id) {
	case DAPM_AIF_IN:
	case DAPM_AIF_OUT:
	case DAPM_DAI_IN:
	case DAPM_DAI_OUT:
	case DAPM_BUFFER:
	case DAPM_PGA:
	case DAPM_MIXER:
	case DAPM_MUX:
	case DAPM_EFFECT:
	case DAPM_SRC:
	case DAPM_ASRC:
	case DAPM_SIGGEN:
	case DAPM_ENCODER:
	case DAPM_DECODER:
		return w->has_uuid;
	default:
		return 0;
	}
}

static int widget_is_dai(const struct tplg_widget *w)
{
	return w->id == DAPM_DAI_IN || w->id == DAPM_DAI_OUT;
}

static int widget_is_host(const struct tplg_widget *w)
{
	return w->id == DAPM_AIF_IN || w->id == DAPM_AIF_OUT;
}

static int widget_is_copier(const struct tplg_widget *w)
{
	return widget_is_host(w) || widget_is_dai(w) || w->id == DAPM_BUFFER;
}

/* ---- which SoundWire link a DAI copier drives ------------------------------
 *
 * Older topologies name their SoundWire DAI links after the link
 * ("SDW2-Playback").  Newer, machine independent ones name them after the
 * codec function instead ("Playback-SimpleJack", "Playback-SmartAmp",
 * "Capture-SmartMic"); aggregated amplifiers get one DAI copier per
 * amplifier link, numbered by their DAI index.  The link then comes from the
 * machine description: which peripherals implement that function, on which
 * links.
 */
#define FN_JACK SOF_SDW_FN_JACK
#define FN_AMP SOF_SDW_FN_AMP
#define FN_MIC SOF_SDW_FN_MIC

/* SDCA function types */
#define SDCA_FN_SMART_AMP 0x01
#define SDCA_FN_SIMPLE_AMP 0x02
#define SDCA_FN_SMART_MIC 0x03
#define SDCA_FN_SIMPLE_MIC 0x04
#define SDCA_FN_SPEAKER_MIC 0x05
#define SDCA_FN_UAJ 0x06
#define SDCA_FN_RJ 0x07
#define SDCA_FN_SIMPLE_JACK 0x08

static int dai_function(const struct tplg_widget *w)
{
	const char *s = w->sname[0] ? w->sname : w->name;

	if (name_has(s, "Jack") || name_has(s, "Headset") ||
	    name_has(s, "Headphone"))
		return FN_JACK;
	if (name_has(s, "Amp") || name_has(s, "Speaker") ||
	    name_has(s, "Spk"))
		return FN_AMP;
	if (name_has(s, "Mic") || name_has(s, "DMIC"))
		return FN_MIC;
	return 0;
}

/* function classes of a peripheral from its SDCA function list */
static int periph_sdca_functions(const struct sof_sdw_peripheral *p)
{
	int f = 0;

	if (p->functions & ((1u << SDCA_FN_SMART_AMP) |
			    (1u << SDCA_FN_SIMPLE_AMP)))
		f |= FN_AMP;
	if (p->functions & ((1u << SDCA_FN_SMART_MIC) |
			    (1u << SDCA_FN_SIMPLE_MIC) |
			    (1u << SDCA_FN_SPEAKER_MIC)))
		f |= FN_MIC;
	if (p->functions & ((1u << SDCA_FN_UAJ) | (1u << SDCA_FN_RJ) |
			    (1u << SDCA_FN_SIMPLE_JACK)))
		f |= FN_JACK;
	return f;
}

/* What well-known SoundWire parts do: `main' is what they are built for,
 * `extra' what they can also do when nothing dedicated is present (the
 * speaker driver inside a headset codec, for instance). */
static const struct {
	uint16_t mfg, part;
	uint8_t main, extra;
} sdw_parts[] = {
	{ 0x01fa, 0x4243, FN_JACK | FN_MIC, FN_AMP }, /* CS42L43 */
	{ 0x01fa, 0x4245, FN_JACK | FN_MIC, FN_AMP }, /* CS42L45 */
	{ 0x01fa, 0x4242, FN_JACK, 0 }, /* CS42L42 */
	{ 0x01fa, 0x3556, FN_AMP, 0 }, /* CS35L56 */
	{ 0x01fa, 0x3557, FN_AMP, 0 }, /* CS35L57 */
	{ 0x01fa, 0x3563, FN_AMP, 0 }, /* CS35L63 */
	{ 0x025d, 0x0700, FN_JACK, FN_AMP | FN_MIC }, /* RT700 */
	{ 0x025d, 0x0711, FN_JACK, 0 }, /* RT711 */
	{ 0x025d, 0x0712, FN_JACK, FN_AMP | FN_MIC }, /* RT712 */
	{ 0x025d, 0x0713, FN_JACK, FN_MIC }, /* RT713 */
	{ 0x025d, 0x0721, FN_JACK, FN_AMP | FN_MIC }, /* RT721 */
	{ 0x025d, 0x0722, FN_JACK, FN_AMP | FN_MIC }, /* RT722 */
	{ 0x025d, 0x0714, FN_MIC, 0 }, /* RT714 */
	{ 0x025d, 0x0715, FN_MIC, 0 }, /* RT715 */
	{ 0x025d, 0x1712, FN_MIC, 0 }, /* RT1712 */
	{ 0x025d, 0x1713, FN_MIC, 0 }, /* RT1713 */
	{ 0x025d, 0x1308, FN_AMP, 0 }, /* RT1308 */
	{ 0x025d, 0x1316, FN_AMP, 0 }, /* RT1316 */
	{ 0x025d, 0x1318, FN_AMP, 0 }, /* RT1318 */
	{ 0x025d, 0x1320, FN_AMP, 0 }, /* RT1320 */
	{ 0x025d, 0x1321, FN_AMP, 0 }, /* RT1321 */
	{ 0x019f, 0x8373, FN_AMP, 0 }, /* MAX98373 */
	{ 0x019f, 0x8363, FN_AMP, 0 }, /* MAX98363 */
};

static int periph_functions(const struct sof_sdw_peripheral *p, int extra)
{
	size_t i;
	int f = periph_sdca_functions(p);

	if (f)
		return f;
	for (i = 0; i < ARRAY_LEN(sdw_parts); i++)
		if (sdw_parts[i].mfg == p->mfg_id &&
		    sdw_parts[i].part == p->part_id)
			return sdw_parts[i].main |
			       (extra ? sdw_parts[i].extra : 0);
	return 0;
}

/* Links carrying function `fn', ascending; returns the count. */
static int sdw_links_for(struct tplg_state *t, int fn, int *links, int max)
{
	const struct sof_machine *m = &t->sdev->machine;
	int pass, n = 0, i, j, np = m->nperiph;

	if (np > SOF_MAX_SDW_PERIPHERALS)
		np = SOF_MAX_SDW_PERIPHERALS;
	/* dedicated parts first; parts that can also do it second */
	for (pass = 0; pass < 2 && n == 0 && fn; pass++)
		for (i = 0; i < np; i++) {
			int l = m->periph[i].link, dup = 0;

			if (l < 0 || l >= SOF_MAX_SDW_LINKS ||
			    !(periph_functions(&m->periph[i], pass) & fn))
				continue;
			for (j = 0; j < n; j++)
				if (links[j] == l)
					dup = 1;
			if (!dup && n < max)
				links[n++] = l;
		}
	/* nothing known: the links the machine uses, lowest first */
	if (!n)
		for (i = 0; i < SOF_MAX_SDW_LINKS && n < max; i++)
			if (m->sdw_link_mask & (1u << i))
				links[n++] = i;
	/* sort */
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && links[j - 1] > links[j]; j--) {
			int tmp = links[j];

			links[j] = links[j - 1];
			links[j - 1] = tmp;
		}
	return n;
}

/* The peripherals on `link' taking part in function `fn' (bit mask of
 * indices into the machine's peripheral table). */
static uint32_t sdw_periph_mask(struct tplg_state *t, int link, int fn)
{
	const struct sof_machine *m = &t->sdev->machine;
	uint32_t all = 0, match = 0;
	int i, np = m->nperiph;

	if (np > SOF_MAX_SDW_PERIPHERALS)
		np = SOF_MAX_SDW_PERIPHERALS;
	for (i = 0; i < np; i++) {
		if (m->periph[i].link != link)
			continue;
		all |= 1u << i;
		if (fn && (periph_functions(&m->periph[i], 1) & fn))
			match |= 1u << i;
	}
	return match ? match : all;
}

static int dai_sdw_link(struct tplg_state *t, const struct tplg_widget *w)
{
	const char *s = w->sname[0] ? w->sname : w->name;
	int links[SOF_MAX_SDW_LINKS], n, i;

	/* "SDWn" in the DAI link name */
	for (i = 0; s[i]; i++) {
		if ((s[i] == 'S' || s[i] == 's') &&
		    (s[i + 1] == 'D' || s[i + 1] == 'd') &&
		    (s[i + 2] == 'W' || s[i + 2] == 'w') && s[i + 3] >= '0' &&
		    s[i + 3] <= '9') {
			int l = s[i + 3] - '0';

			return l < SOF_MAX_SDW_LINKS ? l : -1;
		}
	}
	n = sdw_links_for(t, dai_function(w), links, SOF_MAX_SDW_LINKS);
	if (!n)
		return -1;
	i = w->has_dai_index ? (int)(w->dai_index & 0xff) : 0;
	return links[i < n ? i : n - 1];
}

/* Walk the graph from a host copier in its data direction and report the
 * first DAI copier reached (breadth first), for the PCM's description. */
static int path_first_dai(struct tplg_state *t, int start, int dir)
{
	int queue[TPLG_MAX_PATH], seen[TPLG_MAX_PATH];
	int qh = 0, qt = 0, ns = 0, i, j;

	queue[qt++] = start;
	seen[ns++] = start;
	while (qh < qt) {
		int cur = queue[qh++];

		if (cur != start && widget_is_dai(&t->w[cur]))
			return cur;
		for (i = 0; i < t->nr; i++) {
			const struct tplg_route *r = &t->r[i];
			int next, dup = 0;

			if (dir == AUDIO_DIR_PLAYBACK && r->src == cur)
				next = r->sink;
			else if (dir == AUDIO_DIR_CAPTURE && r->sink == cur)
				next = r->src;
			else
				continue;
			if (next < 0)
				continue;
			if (widget_is_host(&t->w[next]) ||
			    !widget_is_module(&t->w[next]))
				continue;
			for (j = 0; j < ns; j++)
				if (seen[j] == next)
					dup = 1;
			if (dup || qt >= TPLG_MAX_PATH || ns >= TPLG_MAX_PATH)
				continue;
			seen[ns++] = next;
			queue[qt++] = next;
		}
	}
	return -1;
}

static uint32_t output_type_guess(const struct tplg_pcm_int *pc,
				  const struct tplg_widget *dai)
{
	const char *n = pc->pub.name;

	if (name_has(n, "HDMI") || name_has(n, "iDisp"))
		return AUDIO_OUT_HDMI;
	if (name_has(n, "DisplayPort") || name_has(n, "DP"))
		return AUDIO_OUT_DP;
	if (name_has(n, "Speaker") || name_has(n, "Amp") ||
	    name_has(n, "Spk"))
		return AUDIO_OUT_SPEAKER;
	if (name_has(n, "Headset"))
		return AUDIO_OUT_HEADSET;
	if (name_has(n, "Jack") || name_has(n, "Headphone"))
		return AUDIO_OUT_HEADPHONE;
	if (name_has(n, "SPDIF") || name_has(n, "S/PDIF"))
		return AUDIO_OUT_SPDIF;
	if (dai) {
		if (name_has(dai->sname, "iDisp") ||
		    name_has(dai->name, "iDisp"))
			return AUDIO_OUT_HDMI;
		if (name_has(dai->sname, "SmartAmp") ||
		    name_has(dai->sname, "Speaker"))
			return AUDIO_OUT_SPEAKER;
	}
	if (name_has(n, "Analog") || name_has(n, "Line"))
		return AUDIO_OUT_LINE;
	return 0;
}

/* Fill in what a PCM offers from its capabilities and its host copier's
 * format list (the host DMA can only deliver what the copier accepts). */
static void pcm_describe(struct tplg_state *t, struct tplg_pcm_int *pc)
{
	int d, dai = -1;

	pc->pub.formats = 0;
	pc->pub.rates = 0;
	pc->pub.channels_min = 0;
	pc->pub.channels_max = 0;
	for (d = 0; d < 2; d++) {
		uint32_t fm = 0, rm = 0, cmin = 0xffffffffu, cmax = 0;
		int present = d == 0 ? pc->pub.has_playback :
				       pc->pub.has_capture;
		uint32_t caps_f, caps_r;
		const struct tplg_widget *hw;
		uint32_t i, nf;
		const struct tplg_fmt *fl;

		if (!present)
			continue;
		caps_f = pcm_formats_to_audio(pc->pcm_formats[d]);
		caps_r = pcm_rates_to_audio(pc->pcm_rates[d], pc->rate_min[d],
					    pc->rate_max[d]);
		if (pc->host[d] >= 0) {
			hw = &t->w[pc->host[d]];
			/* playback: the host copier's input faces the host;
			 * capture: its output does */
			fl = d == 0 ? hw->in_fmts : hw->out_fmts;
			nf = d == 0 ? hw->num_in_fmts : hw->num_out_fmts;
			for (i = 0; i < nf; i++) {
				uint32_t vb = fl[i].valid_bits;
				uint32_t ch = fl[i].channels;

				if (fl[i].pin_index)
					continue;
				if (vb == 16)
					fm |= AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
				else if (vb == 24)
					fm |= AUDIO_FMT_BIT(AUDIO_FMT_S24_LE);
				else if (vb == 32 &&
					 fl[i].sample_type == IPC4_TYPE_FLOAT)
					fm |= AUDIO_FMT_BIT(AUDIO_FMT_FLOAT_LE);
				else if (vb == 32)
					fm |= AUDIO_FMT_BIT(AUDIO_FMT_S32_LE);
				else if (vb == 8)
					fm |= AUDIO_FMT_BIT(AUDIO_FMT_U8);
				rm |= rate_hz_to_audio(fl[i].rate);
				if (ch) {
					cmin = umin32(cmin, ch);
					cmax = umax32(cmax, ch);
				}
			}
		}
		/* intersect with the capabilities when both say something */
		if (fm && caps_f && (fm & caps_f))
			fm &= caps_f;
		else if (!fm)
			fm = caps_f;
		if (rm && caps_r && (rm & caps_r))
			rm &= caps_r;
		else if (!rm)
			rm = caps_r;
		if (!cmax) {
			cmin = pc->ch_min[d];
			cmax = pc->ch_max[d];
		} else if (pc->ch_max[d] && pc->ch_min[d] <= pc->ch_max[d]) {
			uint32_t a = umax32(cmin, pc->ch_min[d]);
			uint32_t b = umin32(cmax, pc->ch_max[d]);

			if (a <= b) {
				cmin = a;
				cmax = b;
			}
		}
		if (!fm)
			fm = AUDIO_FMT_BIT(AUDIO_FMT_S16_LE);
		if (!rm)
			rm = AUDIO_RATE_48000;
		if (!cmax || cmin == 0xffffffffu) {
			cmin = 2;
			cmax = 2;
		}
		if (cmax > 8)
			cmax = 8;
		if (cmin > cmax)
			cmin = cmax;
		/* the published fields describe the union; playback first */
		pc->pub.formats |= fm;
		pc->pub.rates |= rm;
		if (!pc->pub.channels_max) {
			pc->pub.channels_min = cmin;
			pc->pub.channels_max = cmax;
		} else {
			pc->pub.channels_min = umin32(pc->pub.channels_min,
						      cmin);
			pc->pub.channels_max = umax32(pc->pub.channels_max,
						      cmax);
		}
	}

	pc->pub.dai_type = SOF_DAI_NONE;
	pc->pub.dai_index = -1;
	for (d = 0; d < 2 && dai < 0; d++)
		if (pc->host[d] >= 0)
			dai = path_first_dai(t, pc->host[d], d);
	if (dai >= 0) {
		const struct tplg_widget *dw = &t->w[dai];

		pc->pub.dai_type = dw->dai_type;
		if (dw->dai_type == SOF_DAI_ALH)
			pc->pub.dai_index = dai_sdw_link(t, dw);
		else
			pc->pub.dai_index = dw->has_dai_index ?
						    (int)dw->dai_index :
						    -1;
	}
	pc->pub.output_type =
		pc->pub.has_playback ?
			output_type_guess(pc, dai >= 0 ? &t->w[dai] : NULL) :
			0;
}

static int resolve_file(struct load_ctx *lc)
{
	struct tplg_state *t = lc->t;
	int i, d, unresolved = 0;

	/* routes */
	for (i = 0; i < lc->npr; i++) {
		struct pending_route *pr = &lc->pr[i];
		int src = widget_find(t, lc->file, pr->source);
		int sink = widget_find(t, lc->file, pr->sink);
		struct tplg_route *r;

		if (src < 0 || sink < 0) {
			unresolved++;
			tplg_dbg("route %s -> %s: unknown widget\n",
				 pr->source, pr->sink);
			continue;
		}
		/* the same connection listed twice is one connection */
		{
			int k, dup = 0;

			for (k = 0; k < t->nr && !dup; k++)
				if (t->r[k].src == src && t->r[k].sink == sink)
					dup = 1;
			if (dup)
				continue;
		}
		r = route_new(t);
		if (!r)
			return -ENOSPC;
		r->file = lc->file;
		r->src = src;
		r->sink = sink;
		kmemcpy(r->control, pr->control, SOF_TPLG_NAME_MAX);
		t->nr++;
		lc->routes++;
	}
	if (unresolved)
		tplg_info("%d routes name unknown widgets, ignored\n",
			  unresolved);

	/* pipelines: widgets with the same block index form one pipeline,
	 * owned by its scheduler widget */
	for (i = lc->first_widget; i < t->nw; i++) {
		struct tplg_widget *w = &t->w[i];
		int p = pipe_lookup(t, w->file, w->pipe_index, 1);

		if (p < 0)
			return -ENOSPC;
		w->pipe = p;
		if (w->id == DAPM_SCHEDULER) {
			if (t->pipes[p].sched >= 0)
				tplg_dbg("pipeline %u has two schedulers\n",
					 w->pipe_index);
			else
				t->pipes[p].sched = i;
		}
	}

	/* DAI copiers without a type token: take it from the DAI link */
	for (i = lc->first_widget; i < t->nw; i++) {
		struct tplg_widget *w = &t->w[i];
		int l;

		if (!widget_is_dai(w) || w->dai_type)
			continue;
		for (l = lc->first_link; l < t->nlink; l++)
			if (name_eq(t->link[l].name, w->sname) &&
			    t->link[l].dai_type) {
				w->dai_type = t->link[l].dai_type;
				break;
			}
	}

	/* PCMs <-> host copiers: the copier's stream name is the PCM's
	 * capability (stream) name, or the PCM's DAI name */
	for (i = lc->first_pcm; i < t->npcm; i++) {
		struct tplg_pcm_int *pc = &t->pcm[i];

		for (d = 0; d < 2; d++) {
			int want = d == 0 ? DAPM_AIF_IN : DAPM_AIF_OUT;
			int k;

			for (k = lc->first_widget; k < t->nw; k++) {
				const struct tplg_widget *w = &t->w[k];

				if ((int)w->id != want || !w->sname[0])
					continue;
				if (name_eq(pc->cap_name[d], w->sname) ||
				    name_eq(pc->dai_name, w->sname)) {
					pc->host[d] = k;
					break;
				}
			}
		}
		if (pc->pub.has_playback && pc->host[0] < 0)
			tplg_dbg("PCM %s: no playback host copier\n",
				 pc->pub.name);
		if (pc->pub.has_capture && pc->host[1] < 0)
			tplg_dbg("PCM %s: no capture host copier\n",
				 pc->pub.name);
		pcm_describe(t, pc);
	}
	return 0;
}

/* ==========================================================================
 * Loading
 * ========================================================================== */

static struct tplg_state *tplg_state_get(struct sof_dev *sdev)
{
	struct tplg_state *t = tplg_of(sdev);
	int l, d;

	if (t)
		return t;
	t = zalloc(sizeof(*t));
	if (!t)
		return NULL;
	t->sdev = sdev;
	tplg_lock_init(&t->lock, "sof_tplg");
	for (l = 0; l < SOF_MAX_SDW_LINKS; l++)
		for (d = 0; d < 2; d++)
			t->alh_stream[l][d] = -1;
	sdev->tplg = t;
	return t;
}

/* Undo a failed load: drop everything appended since `lc' started. */
static void load_rollback(struct load_ctx *lc, int first_route, int first_pipe)
{
	struct tplg_state *t = lc->t;
	int i;

	for (i = lc->first_widget; i < t->nw; i++)
		widget_release(&t->w[i]);
	t->nw = lc->first_widget;
	t->nr = first_route;
	t->np = first_pipe;
	t->npcm = lc->first_pcm;
	t->nlink = lc->first_link;
}

static int tplg_parse_buffer(struct tplg_state *t, int file,
			     const uint8_t *buf, size_t size)
{
	struct load_ctx lc;
	size_t pos = 0;
	int first_route = t->nr, first_pipe = t->np, ret = 0, blocks = 0;

	kmemset(&lc, 0, sizeof(lc));
	lc.t = t;
	lc.file = file;
	lc.buf = buf;
	lc.size = size;
	lc.first_widget = t->nw;
	lc.first_pcm = t->npcm;
	lc.first_link = t->nlink;

	if (size < sizeof(struct tplg_hdr)) {
		ret = -EINVAL;
		goto out;
	}
	while (pos < size) {
		const struct tplg_hdr *h;

		if (!range_ok(size, pos, sizeof(*h))) {
			tplg_err("truncated block header at %zu\n", pos);
			ret = -EINVAL;
			break;
		}
		h = (const struct tplg_hdr *)(buf + pos);
		if (h->magic != TPLG_MAGIC) {
			tplg_err("bad magic %08x at %zu\n", h->magic, pos);
			ret = -EINVAL;
			break;
		}
		if (h->abi != TPLG_ABI_V5) {
			tplg_err("unsupported ABI version %u\n", h->abi);
			ret = -EPROTO;
			break;
		}
		if (h->size != sizeof(*h)) {
			tplg_err("bad block header size %u\n", h->size);
			ret = -EINVAL;
			break;
		}
		if (!range_ok(size, pos + sizeof(*h), h->payload_size)) {
			tplg_err("block at %zu overruns the file\n", pos);
			ret = -EINVAL;
			break;
		}
		if (h->count > 4096) {
			ret = -EINVAL;
			break;
		}
		ret = parse_block(&lc, h, buf + pos + sizeof(*h),
				  h->payload_size);
		if (ret < 0) {
			tplg_err("block type %u at %zu: error %d\n", h->type,
				 pos, ret);
			break;
		}
		pos += sizeof(*h) + h->payload_size;
		if (++blocks > 65536) {
			ret = -EINVAL;
			break;
		}
	}
	if (!ret)
		ret = resolve_file(&lc);
out:
	if (ret < 0)
		load_rollback(&lc, first_route, first_pipe);
	else
		tplg_info("%s: %d widgets, %d routes, %d PCMs, %d DAI links, "
			  "%d controls\n",
			  t->file_name[file], lc.widgets, lc.routes, lc.pcms,
			  lc.links, lc.controls);
	if (lc.pr)
		kfree(lc.pr);
	return ret;
}

int sof_tplg_load(struct sof_dev *sdev, const char *path)
{
	struct tplg_state *t;
	void *data = NULL;
	size_t len = 0;
	const char *name = path;
	int ret, file;
	size_t plen;

	if (!sdev || !path)
		return -EINVAL;
	plen = kstrlen(path);
	if (plen == 0 || plen >= 256)
		return -EINVAL;
	/* accept both "intel/sof-..." and "/lib/firmware/intel/sof-..." */
	if (!kstrncmp(name, "/lib/firmware/", 14))
		name += 14;
	if (name[0] == '/' || name[0] == '\0')
		return -EINVAL;

	t = tplg_state_get(sdev);
	if (!t)
		return -ENOMEM;
	ret = tplg_op_begin(t);
	if (ret)
		return ret;
	if (t->nfiles >= TPLG_MAX_FILES) {
		ret = -ENOSPC;
		goto out;
	}

	ret = firmware_request(name, &data, &len);
	if (ret < 0) {
		tplg_err("cannot load %s: %d\n", name, ret);
		goto out;
	}
	file = t->nfiles;
	{
		/* keep the tail of long names, which is the useful part */
		size_t n = kstrlen(name);
		size_t cap = sizeof(t->file_name[0]) - 1;

		if (n > cap) {
			kmemcpy(t->file_name[file], name + (n - cap), cap);
			t->file_name[file][cap] = '\0';
		} else {
			kmemcpy(t->file_name[file], name, n + 1);
		}
	}
	ret = tplg_parse_buffer(t, file, data, len);
	firmware_release(data);
	if (ret == 0)
		t->nfiles++;
out:
	tplg_op_end(t);
	return ret;
}

int sof_tplg_pcm_count(struct sof_dev *sdev)
{
	struct tplg_state *t = tplg_of(sdev);

	return t ? t->npcm : 0;
}

const struct sof_tplg_pcm *sof_tplg_pcm_get(struct sof_dev *sdev, int idx)
{
	struct tplg_state *t = tplg_of(sdev);

	if (!t || idx < 0 || idx >= t->npcm)
		return NULL;
	return &t->pcm[idx].pub;
}

/* ==========================================================================
 * Firmware modules: UUID -> module id, instance ids
 * ========================================================================== */

static struct tplg_fw_module *mod_lookup(struct tplg_state *t,
					 const uint8_t *uuid)
{
	int i;

	for (i = 0; i < t->nmods; i++)
		if (!kmemcmp(t->mods[i].uuid, uuid, 16))
			return &t->mods[i];
	return NULL;
}

static struct tplg_fw_module *mod_add(struct tplg_state *t,
				      const uint8_t *uuid,
				      const struct sof_fw_module_info *info)
{
	struct tplg_fw_module *m = mod_lookup(t, uuid);

	if (m)
		return m;
	if (t->nmods >= TPLG_MAX_MODULES)
		return NULL;
	m = &t->mods[t->nmods++];
	kmemset(m, 0, sizeof(*m));
	kmemcpy(m->uuid, uuid, 16);
	m->info = *info;
	return m;
}

/* Ask the base firmware for its module list (and those of loaded
 * libraries).  Every entry is a manifest module record whose first word
 * the firmware replaced with the runtime module id. */
static int mods_query_fw(struct tplg_state *t)
{
	uint8_t *reply;
	uint32_t primary, ext, count, i;
	int ret;

	if (t->mods_queried)
		return 0;
	t->mods_queried = 1;
	reply = zalloc(IPC4_MSG_MAX_SIZE);
	if (!reply)
		return -ENOMEM;
	primary = IPC4_MOD_ID(IPC4_BASEFW_MODULE_ID) | IPC4_MOD_INSTANCE(0) |
		  IPC4_MSG_TYPE(IPC4_MOD_LARGE_CONFIG_GET) | IPC4_MSG_REQUEST |
		  IPC4_MSG_TARGET_MODULE;
	ext = IPC4_MOD_EXT_MSG_SIZE(IPC4_MSG_MAX_SIZE) |
	      IPC4_MOD_EXT_MSG_PARAM_ID(IPC4_BASEFW_PARAM_MODULES_INFO) |
	      IPC4_MOD_EXT_MSG_FIRST_BLOCK | IPC4_MOD_EXT_MSG_LAST_BLOCK;
	ret = sof_ipc4_send(t->sdev, primary, ext, NULL, 0, reply,
			    IPC4_MSG_MAX_SIZE);
	if (ret < 0) {
		tplg_err("firmware module list query failed: %d\n", ret);
		kfree(reply);
		return ret;
	}
	kmemcpy(&count, reply, 4);
	if (count > (IPC4_MSG_MAX_SIZE - 4) / FW_MAN_MODULE_SIZE)
		count = (IPC4_MSG_MAX_SIZE - 4) / FW_MAN_MODULE_SIZE;
	for (i = 0; i < count; i++) {
		const uint8_t *e = reply + 4 + i * FW_MAN_MODULE_SIZE;
		struct sof_fw_module_info info;
		uint16_t id, imax, bss;
		uint32_t type;

		kmemcpy(&id, e + FW_MAN_MODULE_OFF_ID, 2);
		kmemcpy(&type, e + FW_MAN_MODULE_OFF_TYPE, 4);
		kmemcpy(&imax, e + FW_MAN_MODULE_OFF_INSTANCE_MAX, 2);
		kmemcpy(&bss, e + FW_MAN_MODULE_OFF_BSS, 2);
		kmemset(&info, 0, sizeof(info));
		info.module_id = id;
		info.type = type;
		info.instance_max = imax;
		info.is_pages = bss;
		mod_add(t, e + FW_MAN_MODULE_OFF_UUID, &info);
	}
	tplg_dbg("firmware reports %u modules\n", count);
	kfree(reply);
	return 0;
}

static int widget_resolve_module(struct tplg_state *t, struct tplg_widget *w)
{
	struct tplg_fw_module *m;

	if (w->module_resolved)
		return 0;
	if (!w->has_uuid)
		return -ENOENT;
	m = mod_lookup(t, w->uuid);
	if (!m) {
		struct sof_fw_module_info info;

		kmemset(&info, 0, sizeof(info));
		if (sof_fw_module_find(t->sdev, w->uuid, &info) == 0)
			m = mod_add(t, w->uuid, &info);
	}
	if (!m) {
		mods_query_fw(t);
		m = mod_lookup(t, w->uuid);
	}
	if (!m) {
		const uint8_t *u = w->uuid;

		tplg_err("%s: no firmware module for UUID "
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
			 "%02x%02x%02x%02x%02x%02x\n",
			 w->name, u[3], u[2], u[1], u[0], u[5], u[4], u[7],
			 u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14],
			 u[15]);
		return -ENODEV;
	}
	w->mod = m->info;
	w->module_resolved = 1;
	return 0;
}

static int mod_instance_alloc(struct tplg_state *t, struct tplg_widget *w)
{
	struct tplg_fw_module *m = mod_lookup(t, w->uuid);
	uint32_t max, i;

	if (!m)
		return -ENODEV;
	max = m->info.instance_max ? m->info.instance_max :
				     TPLG_MAX_MOD_INSTANCES;
	if (max > TPLG_MAX_MOD_INSTANCES)
		max = TPLG_MAX_MOD_INSTANCES;
	for (i = 0; i < max; i++) {
		if (!(m->inst_used[i / 64] & (1ull << (i % 64)))) {
			m->inst_used[i / 64] |= 1ull << (i % 64);
			return (int)i;
		}
	}
	return -ENOSPC;
}

static void mod_instance_free(struct tplg_state *t, struct tplg_widget *w)
{
	struct tplg_fw_module *m = mod_lookup(t, w->uuid);
	int i = w->instance_id;

	if (m && i >= 0 && i < TPLG_MAX_MOD_INSTANCES)
		m->inst_used[i / 64] &= ~(1ull << (i % 64));
	w->instance_id = -1;
}

static int pipe_id_alloc(struct tplg_state *t)
{
	int i;

	for (i = 0; i < TPLG_MAX_PIPE_INSTANCES; i++)
		if (!(t->pipe_ids_used & (1ull << i))) {
			t->pipe_ids_used |= 1ull << i;
			return i;
		}
	return -ENOSPC;
}

static void pipe_id_free(struct tplg_state *t, int id)
{
	if (id >= 0 && id < TPLG_MAX_PIPE_INSTANCES)
		t->pipe_ids_used &= ~(1ull << id);
}

/* ==========================================================================
 * IPC4 primitives
 * ========================================================================== */

static int ipc_tx(struct tplg_state *t, uint32_t primary, uint32_t ext,
		  const void *data, size_t len)
{
	int ret;

	if (len > IPC4_MSG_MAX_SIZE)
		return -EMSGSIZE;
	tplg_dbg("ipc %08x %08x (%zu bytes)\n", primary, ext, len);
	ret = sof_ipc4_send(t->sdev, primary, ext, data, len, NULL, 0);
	if (ret < 0)
		t->ipc_errors++;
	return ret;
}

static uint32_t mod_primary(const struct tplg_widget *w, uint32_t type)
{
	return IPC4_MOD_ID(w->mod.module_id) |
	       IPC4_MOD_INSTANCE((uint32_t)w->instance_id) |
	       IPC4_MSG_TYPE(type) | IPC4_MSG_REQUEST | IPC4_MSG_TARGET_MODULE;
}

/* LARGE_CONFIG_SET, split into mailbox-sized blocks when needed: the first
 * block carries the total size, later ones their offset. */
static int ipc_large_config_set(struct tplg_state *t,
				const struct tplg_widget *w, uint32_t param,
				const void *data, uint32_t size)
{
	const uint8_t *p = data;
	uint32_t off = 0;

	if (size > 0xfffffu)
		return -EMSGSIZE;
	if (size == 0)
		return ipc_tx(t, mod_primary(w, IPC4_MOD_LARGE_CONFIG_SET),
			      IPC4_MOD_EXT_MSG_PARAM_ID(param) |
				      IPC4_MOD_EXT_MSG_FIRST_BLOCK |
				      IPC4_MOD_EXT_MSG_LAST_BLOCK,
			      NULL, 0);
	while (off < size) {
		uint32_t chunk = umin32(IPC4_MSG_MAX_SIZE, size - off);
		uint32_t ext = IPC4_MOD_EXT_MSG_PARAM_ID(param);
		int ret;

		if (off == 0)
			ext |= IPC4_MOD_EXT_MSG_SIZE(size) |
			       IPC4_MOD_EXT_MSG_FIRST_BLOCK;
		else
			ext |= IPC4_MOD_EXT_MSG_SIZE(off);
		if (off + chunk == size)
			ext |= IPC4_MOD_EXT_MSG_LAST_BLOCK;
		ret = ipc_tx(t, mod_primary(w, IPC4_MOD_LARGE_CONFIG_SET), ext,
			     p + off, chunk);
		if (ret < 0)
			return ret;
		off += chunk;
	}
	return 0;
}

static int ipc_pipe_create(struct tplg_state *t, struct tplg_pipe *p)
{
	const struct tplg_widget *s = &t->w[p->sched];
	uint32_t pages = p->mem_pages;
	uint32_t primary, ext;

	if (s->mem_usage_tkn > pages)
		pages = s->mem_usage_tkn;
	if (pages > IPC4_PIPE_MEM_SIZE_MAX)
		pages = IPC4_PIPE_MEM_SIZE_MAX;
	if (!pages)
		pages = 1;
	primary = IPC4_PIPE_MEM_SIZE(pages) | IPC4_PIPE_PRIORITY(s->priority) |
		  IPC4_PIPE_INSTANCE((uint32_t)p->instance_id) |
		  IPC4_MSG_TYPE(IPC4_GLB_CREATE_PIPELINE) | IPC4_MSG_REQUEST |
		  IPC4_MSG_TARGET_FW;
	ext = IPC4_PIPE_EXT_LP(s->lp_mode) | IPC4_PIPE_EXT_CORE(s->sched_core);
	tplg_dbg("create pipeline %s: instance %d, priority %u, %u pages, "
		 "core %u, lp %u\n",
		 s->name, p->instance_id, s->priority, pages, s->sched_core,
		 s->lp_mode);
	return ipc_tx(t, primary, ext, NULL, 0);
}

static int ipc_pipe_delete(struct tplg_state *t, struct tplg_pipe *p)
{
	uint32_t primary = IPC4_PIPE_INSTANCE((uint32_t)p->instance_id) |
			   IPC4_MSG_TYPE(IPC4_GLB_DELETE_PIPELINE) |
			   IPC4_MSG_REQUEST | IPC4_MSG_TARGET_FW;

	return ipc_tx(t, primary, 0, NULL, 0);
}

/* SET_PIPELINE_STATE for one pipeline, or for several with one message
 * (payload: count followed by the instance ids). */
static int ipc_pipes_set_state(struct tplg_state *t, const int *ids, int n,
			       int state)
{
	uint32_t primary = IPC4_PIPE_STATE((uint32_t)state) |
			   IPC4_MSG_TYPE(IPC4_GLB_SET_PIPELINE_STATE) |
			   IPC4_MSG_REQUEST | IPC4_MSG_TARGET_FW;
	uint32_t payload[1 + TPLG_MAX_PATH_PIPES];
	int i;

	if (n <= 0)
		return 0;
	if (n == 1)
		return ipc_tx(t, primary | IPC4_PIPE_STATE_ID((uint32_t)ids[0]),
			      0, NULL, 0);
	if (n > TPLG_MAX_PATH_PIPES)
		return -E2BIG;
	payload[0] = (uint32_t)n;
	for (i = 0; i < n; i++)
		payload[1 + i] = (uint32_t)ids[i];
	return ipc_tx(t, primary, IPC4_PIPE_STATE_EXT_MULTI, payload,
		      (size_t)(n + 1) * sizeof(uint32_t));
}

/* ==========================================================================
 * Volume: 0..100 -> dB -> Q1.31 linear gain
 * ========================================================================== */

/* 2^(-k/32) in Q1.31, k = 0..32 */
static const uint32_t exp2_neg_tab[33] = {
	0x7fffffffu, 0x7d41d96eu, 0x7a92be8bu, 0x77f25cceu, 0x75606374u,
	0x72dc8374u, 0x70666f76u, 0x6dfddbccu, 0x6ba27e65u, 0x69540ec9u,
	0x6712460bu, 0x64dcdec3u, 0x62b39509u, 0x60962665u, 0x5e8451d0u,
	0x5c7dd7a4u, 0x5a82799au, 0x5891fac1u, 0x56ac1f75u, 0x54d0ad5au,
	0x52ff6b55u, 0x51382182u, 0x4f7a9930u, 0x4dc69cddu, 0x4c1bf829u,
	0x4a7a77d4u, 0x48e1e9bau, 0x47521cc6u, 0x45cae0f2u, 0x444c0740u,
	0x42d561b4u, 0x4166c34cu, 0x40000000u,
};

/* 10^(dB/20) for dB <= 0 given in 1/100 dB, as Q1.31.  Integer only:
 * 10^(x/20) = 2^(x * log2(10) / 20), log2(10)/2000 = 0.00166096 per
 * centi-dB, carried in Q16 and finished with a linearly interpolated
 * table of 2^(-k/32). */
static uint32_t db_to_q31(int32_t centi_db)
{
	uint64_t x, e;
	uint32_t n, frac, k, rem, a, b, v;

	if (centi_db >= 0)
		return IPC4_VOL_ZERO_DB;
	x = (uint64_t)(-(int64_t)centi_db);
	e = x * 108853u / 1000u; /* octaves, Q16 */
	n = (uint32_t)(e >> 16);
	if (n >= 31)
		return 0;
	frac = (uint32_t)(e & 0xffffu);
	k = frac >> 11;
	rem = frac & 0x7ffu;
	a = exp2_neg_tab[k];
	b = exp2_neg_tab[k + 1];
	v = a - (uint32_t)(((uint64_t)(a - b) * rem) >> 11);
	return v >> n;
}

/* The gain for a volume 0..100 on a gain widget: through the volume
 * control's dB scale when the topology has one, else -64 dB .. 0 dB. */
static uint32_t volume_to_gain(const struct tplg_widget *w, uint32_t vol)
{
	const struct tplg_kctl *kc = NULL;
	int32_t db;
	int i;

	if (vol == 0)
		return 0;
	if (vol >= 100)
		vol = 100;
	for (i = 0; i < w->nkctl; i++)
		if (w->kctl[i].kind == TPLG_TYPE_MIXER && w->kctl[i].has_db &&
		    w->kctl[i].max > 0) {
			kc = &w->kctl[i];
			break;
		}
	if (kc) {
		uint32_t step = (vol * kc->max + 50) / 100;

		int64_t d64;

		if (step == 0 && kc->db_mute)
			return 0;
		d64 = (int64_t)kc->db_min + (int64_t)step * kc->db_step;
		if (d64 < -14400)
			d64 = -14400;
		if (d64 > 0)
			d64 = 0;
		db = (int32_t)d64;
	} else {
		db = -6400 + (int32_t)(vol * 64);
	}
	if (db > 0)
		db = 0;
	return db_to_q31(db);
}

/* ==========================================================================
 * Audio format selection
 *
 * Every module lists the formats it accepts on its input pins and produces
 * on its output pins.  Walking the path in data-flow order, each module
 * takes the format its predecessor produces (the host's format at the
 * start), picks the matching entry of its input list, and picks the output
 * entry that keeps rate/channels (or converts, for modules that do).
 * ========================================================================== */

struct pparams {
	uint32_t rate, ch, valid, tclass;
};

/* sample type classes compared between formats */
#define TCLASS_INT 0
#define TCLASS_FLOAT 1
#define TCLASS_UNSIGNED 2

static uint32_t fmt_tclass(const struct tplg_fmt *f)
{
	/* 16/20/24-bit containers are always little endian integers; the
	 * type matters only for 8 bit (unsigned) and 32 bit (float) */
	if (f->valid_bits == 32 && f->sample_type == IPC4_TYPE_FLOAT)
		return TCLASS_FLOAT;
	if (f->valid_bits == 8 && f->sample_type == IPC4_TYPE_UNSIGNED_INTEGER)
		return TCLASS_UNSIGNED;
	return TCLASS_INT;
}

static void fmt_to_params(const struct tplg_fmt *f, struct pparams *p)
{
	p->rate = f->rate;
	p->ch = f->channels;
	p->valid = f->valid_bits;
	p->tclass = fmt_tclass(f);
}

static void fmt_to_ipc4(const struct tplg_fmt *f, struct ipc4_audio_format *a)
{
	a->sampling_frequency = f->rate;
	a->bit_depth = f->bit_depth;
	a->ch_map = f->ch_map;
	a->ch_cfg = f->ch_cfg;
	a->interleaving_style = f->interleaving;
	a->fmt_cfg = f->fmt_cfg;
}

static void ipc4_to_params(const struct ipc4_audio_format *a,
			   struct pparams *p)
{
	uint32_t st = IPC4_FMT_CFG_SAMPLE_TYPE(a->fmt_cfg);

	p->rate = a->sampling_frequency;
	p->ch = IPC4_FMT_CFG_CHANNELS(a->fmt_cfg);
	p->valid = IPC4_FMT_CFG_VALID_BITS(a->fmt_cfg);
	if (!p->valid)
		p->valid = a->bit_depth;
	p->tclass = TCLASS_INT;
	if (p->valid == 32 && st == IPC4_TYPE_FLOAT)
		p->tclass = TCLASS_FLOAT;
	else if (p->valid == 8 && st == IPC4_TYPE_UNSIGNED_INTEGER)
		p->tclass = TCLASS_UNSIGNED;
}

static int fmts_all_same(const struct tplg_fmt *l, uint32_t n)
{
	uint32_t i;

	for (i = 1; i < n; i++)
		if (l[i].rate != l[0].rate || l[i].channels != l[0].channels ||
		    l[i].valid_bits != l[0].valid_bits)
			return 0;
	return 1;
}

static int fmts_single_bits(const struct tplg_fmt *l, uint32_t n)
{
	uint32_t i;

	for (i = 1; i < n; i++)
		if (l[i].valid_bits != l[0].valid_bits)
			return 0;
	return 1;
}

/* Pick the entry matching `ref' (exactly, or the closest when `exact' is
 * 0: rate weighs most, then channels, then sample size).  `pin0' limits
 * the search to pin 0 entries.  -1 if nothing fits. */
static int fmt_choose(const struct tplg_fmt *l, uint32_t n,
		      const struct pparams *ref, int pin0, int exact)
{
	int best = -1, best_score = -1;
	uint32_t i;

	if (!n)
		return -1;
	if (fmts_all_same(l, n))
		return 0;
	for (i = 0; i < n; i++) {
		int score = 0;

		if (pin0 && l[i].pin_index)
			continue;
		if (l[i].rate == ref->rate)
			score += 8;
		if (l[i].channels == ref->ch)
			score += 4;
		if (l[i].valid_bits == ref->valid)
			score += 2;
		if (fmt_tclass(&l[i]) == ref->tclass)
			score += 1;
		if (score == 15)
			return (int)i;
		if (score > best_score) {
			best_score = score;
			best = (int)i;
		}
	}
	return exact ? -1 : best;
}

/* The parameters a DAI imposes: any of rate/channels/sample size that is
 * the same in all of the DAI side formats replaces the requested one. */
static void dai_params_adjust(const struct tplg_fmt *l, uint32_t n,
			      struct pparams *p)
{
	uint32_t i;
	int same_rate = 1, same_ch = 1, same_bits = 1;

	if (!n)
		return;
	for (i = 1; i < n; i++) {
		if (l[i].rate != l[0].rate)
			same_rate = 0;
		if (l[i].channels != l[0].channels)
			same_ch = 0;
		if (l[i].valid_bits != l[0].valid_bits)
			same_bits = 0;
	}
	if (same_rate)
		p->rate = l[0].rate;
	if (same_ch)
		p->ch = l[0].channels;
	if (same_bits) {
		p->valid = l[0].valid_bits;
		p->tclass = fmt_tclass(&l[0]);
	}
}

/* Do any input/output format pairs differ in rate / channels / bits? */
static void fmts_changed(const struct tplg_widget *w, int *rate, int *ch,
			 int *bits)
{
	uint32_t i, j;

	*rate = *ch = *bits = 0;
	for (i = 0; i < w->num_in_fmts; i++)
		for (j = 0; j < w->num_out_fmts; j++) {
			if (w->in_fmts[i].rate != w->out_fmts[j].rate)
				*rate = 1;
			if (w->in_fmts[i].channels != w->out_fmts[j].channels)
				*ch = 1;
			if (w->in_fmts[i].valid_bits !=
			    w->out_fmts[j].valid_bits)
				*bits = 1;
		}
}

/* ==========================================================================
 * NHLT: the gateway configuration blobs for SSP and DMIC ports
 *
 * The table (from ACPI, or embedded in a topology manifest) lists
 * endpoints; each has a link type, an instance (the SSP port), a
 * direction and a list of formats, every format with its own blob.
 * ========================================================================== */

#define NHLT_EP_FIXED 19 /* length .. virtual_bus_id */
#define NHLT_WAVEFMT_SIZE 40

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static int nhlt_find(const uint8_t *tab, uint32_t len, int link_type,
		     int instance, int dir, uint32_t rate, uint32_t ch,
		     uint32_t bits, const uint8_t **blob, uint32_t *blen)
{
	uint32_t pos = 36, neps, e;

	/* ACPI header (36 bytes) + endpoint count */
	if (len < 37 || kmemcmp(tab, "NHLT", 4) != 0)
		return -ENOENT;
	neps = tab[pos++];
	for (e = 0; e < neps; e++) {
		uint32_t elen, cfgsz, off, nf, f;
		uint8_t ltype, edir, vbus;

		if (!range_ok(len, pos, NHLT_EP_FIXED + 4))
			return -ENOENT;
		elen = rd32(tab + pos);
		if (elen < NHLT_EP_FIXED + 4 || !range_ok(len, pos, elen))
			return -ENOENT;
		ltype = tab[pos + 4];
		edir = tab[pos + 17];
		vbus = tab[pos + 18];
		off = pos + NHLT_EP_FIXED;
		cfgsz = rd32(tab + off);
		if (!range_ok(elen, NHLT_EP_FIXED + 4, cfgsz)) {
			pos += elen;
			continue;
		}
		off += 4 + cfgsz;
		if (ltype != link_type ||
		    (link_type == NHLT_LINK_SSP && vbus != instance) ||
		    (dir == AUDIO_DIR_PLAYBACK && edir != 0 && edir != 2) ||
		    (dir == AUDIO_DIR_CAPTURE && edir != 1)) {
			pos += elen;
			continue;
		}
		if (off >= pos + elen) {
			pos += elen;
			continue;
		}
		nf = tab[off++];
		for (f = 0; f < nf; f++) {
			const uint8_t *wf;
			uint32_t bsz;

			if (off + NHLT_WAVEFMT_SIZE + 4 > pos + elen)
				break;
			wf = tab + off;
			off += NHLT_WAVEFMT_SIZE;
			bsz = rd32(tab + off);
			off += 4;
			if (bsz > pos + elen - off)
				break;
			if (rd16(wf + 2) == ch && rd32(wf + 4) == rate &&
			    rd16(wf + 14) == bits && (bsz & 3) == 0 && bsz) {
				*blob = tab + off;
				*blen = bsz;
				return 0;
			}
			off += bsz;
		}
		pos += elen;
	}
	return -ENOENT;
}

#ifndef SOF_TPLG_HOST_TEST
static const uint8_t *acpi_nhlt(uint32_t *len)
{
	acpi_sdt_header_t *h = acpi_find_table("NHLT");

	if (!h || h->length < 37 || h->length > (4u << 20))
		return NULL;
	*len = h->length;
	return (const uint8_t *)h;
}
#else
static const uint8_t *acpi_nhlt(uint32_t *len)
{
	*len = 0;
	return NULL;
}
#endif

static int dai_find_blob(struct tplg_state *t, int link_type, int instance,
			 int dir, uint32_t rate, uint32_t ch, uint32_t bits,
			 const uint8_t **blob, uint32_t *blen)
{
	const uint8_t *tab;
	uint32_t len = 0;
	int i;

	if (sof_nhlt_get_blob) {
		const void *b = NULL;
		uint32_t l = 0;

		if (sof_nhlt_get_blob(t->sdev, link_type, instance, dir, rate,
				      ch, bits, &b, &l) == 0 &&
		    b && l && !(l & 3)) {
			*blob = b;
			*blen = l;
			return 0;
		}
	}
	tab = acpi_nhlt(&len);
	if (tab && nhlt_find(tab, len, link_type, instance, dir, rate, ch,
			     bits, blob, blen) == 0)
		return 0;
	for (i = 0; i < t->nnhlt; i++)
		if (nhlt_find(t->nhlt[i], t->nhlt_len[i], link_type, instance,
			      dir, rate, ch, bits, blob, blen) == 0)
			return 0;
	return -ENOENT;
}

/* ==========================================================================
 * Per-module preparation: formats, base config, gateway
 * ========================================================================== */

static void base_from(struct tplg_widget *w, int in_idx, int out_idx)
{
	struct ipc4_base_module_cfg *b = &w->base;

	kmemset(b, 0, sizeof(*b));
	b->cpc = w->cpc ? w->cpc : w->mod.cpc;
	b->is_pages = w->is_pages ? w->is_pages : w->mod.is_pages;
	if (in_idx >= 0) {
		fmt_to_ipc4(&w->in_fmts[in_idx], &b->audio_fmt);
		b->ibs = w->in_fmts[in_idx].buffer_size;
	}
	if (out_idx >= 0) {
		b->obs = w->out_fmts[out_idx].buffer_size;
		fmt_to_ipc4(&w->out_fmts[out_idx], &w->out_format);
	} else {
		w->out_format = b->audio_fmt;
	}
	if (in_idx < 0 && out_idx >= 0) {
		/* modules without inputs (generators) describe their output */
		b->audio_fmt = w->out_format;
	}
	w->in_fmt_idx = in_idx;
	w->out_fmt_idx = out_idx;
}

/* Memory pages a module needs inside its pipeline, as the firmware
 * accounts them (task and queue objects plus the input buffers). */
static uint32_t module_pages(const struct tplg_widget *w)
{
	uint64_t task, queue, ibs;

	task = IPC4_PIPELINE_OBJECT_SIZE + IPC4_MODULE_INSTANCE_LIST_ITEM_SIZE +
	       w->base.is_pages;
	if (!(w->mod.type & IPC4_MODULE_DP)) {
		task += (IPC4_LL_TASK_OBJECT_SIZE + 63u) & ~63u;
		task += IPC4_FW_MAX_QUEUE_COUNT *
			IPC4_MODULE_INSTANCE_LIST_ITEM_SIZE;
		task += IPC4_LL_TASK_LIST_ITEM_SIZE;
	} else {
		task += (IPC4_DP_TASK_OBJECT_SIZE + 63u) & ~63u;
		task += IPC4_DP_TASK_LIST_SIZE;
	}
	ibs = ((uint64_t)w->base.ibs + 63u) & ~63ull;
	queue = IPC4_FW_MAX_QUEUE_COUNT * (IPC4_DATA_QUEUE_OBJECT_SIZE + ibs);
	return (uint32_t)((task + queue + IPC4_FW_PAGE_BYTES - 1) /
			  IPC4_FW_PAGE_BYTES);
}

/* channel mask from a 4-bit-per-slot channel map (0xf = unused slot) */
static uint32_t chmap_mask(uint32_t ch_map, uint32_t *count)
{
	uint32_t mask = 0, n = 0, i;

	for (i = 0; i < 8 && ch_map; i++, ch_map >>= 4) {
		if ((ch_map & 0xf) != 0xf) {
			mask |= 1u << i;
			n++;
		}
	}
	*count = n;
	return mask;
}

struct prep_ctx {
	struct tplg_state *t;
	struct tplg_pcm_int *pc;
	struct tplg_stream_rt *rt;
	int dir;
	struct pparams fe;
	int host_tag;
};

/* Choose the formats of one module.  `in' is what arrives at its input.
 * On success `out' is what it produces. */
static int prepare_module(struct prep_ctx *pcx, struct tplg_widget *w,
			  const struct pparams *in, struct pparams *out)
{
	struct pparams ref;
	int ii = -1, oi = -1;
	int pin0 = 1;

	switch (w->id) {
	case DAPM_AIF_IN:
		/* host -> DSP: the input must be exactly the host format */
		ii = fmt_choose(w->in_fmts, w->num_in_fmts, &pcx->fe, pin0, 1);
		if (ii < 0) {
			tplg_err("%s: no input format for %u Hz %u ch %u bit\n",
				 w->name, pcx->fe.rate, pcx->fe.ch,
				 pcx->fe.valid);
			return -EINVAL;
		}
		fmt_to_params(&w->in_fmts[ii], &ref);
		if (w->num_out_fmts &&
		    fmts_single_bits(w->out_fmts, w->num_out_fmts)) {
			ref.valid = w->out_fmts[0].valid_bits;
			ref.tclass = fmt_tclass(&w->out_fmts[0]);
		}
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 0);
		break;

	case DAPM_AIF_OUT:
		/* DSP -> host: the output must be exactly the host format */
		ii = fmt_choose(w->in_fmts, w->num_in_fmts, in, pin0, 0);
		ref = pcx->fe;
		if (w->num_out_fmts &&
		    fmts_single_bits(w->out_fmts, w->num_out_fmts) &&
		    w->out_fmts[0].valid_bits != ref.valid) {
			tplg_err("%s: capture is %u bit only\n", w->name,
				 w->out_fmts[0].valid_bits);
			return -EINVAL;
		}
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 1);
		if (oi < 0 && w->num_out_fmts) {
			tplg_err("%s: no output format for %u Hz %u ch %u bit\n",
				 w->name, ref.rate, ref.ch, ref.valid);
			return -EINVAL;
		}
		break;

	case DAPM_DAI_IN: {
		/* DSP -> DAI: whatever arrives in, what the link runs out */
		struct pparams dai = pcx->fe;

		dai_params_adjust(w->out_fmts, w->num_out_fmts, &dai);
		ii = fmt_choose(w->in_fmts, w->num_in_fmts, in, pin0, 0);
		ref = dai;
		if (w->num_out_fmts &&
		    fmts_single_bits(w->out_fmts, w->num_out_fmts)) {
			ref.valid = w->out_fmts[0].valid_bits;
			ref.tclass = fmt_tclass(&w->out_fmts[0]);
		}
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 0);
		break;
	}

	case DAPM_DAI_OUT: {
		/* DAI -> DSP: the link's format in, same rate/channels out */
		struct pparams dai = pcx->fe;

		dai_params_adjust(w->in_fmts, w->num_in_fmts, &dai);
		ii = fmt_choose(w->in_fmts, w->num_in_fmts, &dai, pin0, 0);
		if (ii >= 0)
			fmt_to_params(&w->in_fmts[ii], &ref);
		else
			ref = dai;
		if (w->num_out_fmts &&
		    fmts_single_bits(w->out_fmts, w->num_out_fmts)) {
			ref.valid = w->out_fmts[0].valid_bits;
			ref.tclass = fmt_tclass(&w->out_fmts[0]);
		}
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 0);
		break;
	}

	case DAPM_SRC:
	case DAPM_ASRC:
		ii = fmt_choose(w->in_fmts, w->num_in_fmts, in, pin0, 0);
		if (ii >= 0)
			fmt_to_params(&w->in_fmts[ii], &ref);
		else
			ref = *in;
		/* the converter's output rate: its token, the host rate for
		 * capture, otherwise whatever its output list offers */
		if (w->src_rate_out)
			ref.rate = w->src_rate_out;
		else if (pcx->dir == AUDIO_DIR_CAPTURE)
			ref.rate = pcx->fe.rate;
		else if (w->num_out_fmts)
			ref.rate = w->out_fmts[0].rate;
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 0);
		break;

	default: {
		/* gain, mixer, module copier, processing modules */
		int crate, cch, cbits;

		if (w->num_in_fmts) {
			ii = fmt_choose(w->in_fmts, w->num_in_fmts, in, pin0,
					0);
			if (ii < 0) {
				tplg_err("%s: no usable input format\n",
					 w->name);
				return -EINVAL;
			}
			fmt_to_params(&w->in_fmts[ii], &ref);
		} else {
			ref = pcx->fe;
		}
		/* capture paths convert towards the host format */
		fmts_changed(w, &crate, &cch, &cbits);
		if (pcx->dir == AUDIO_DIR_CAPTURE) {
			if (crate)
				ref.rate = pcx->fe.rate;
			if (cch)
				ref.ch = pcx->fe.ch;
			if (cbits) {
				ref.valid = pcx->fe.valid;
				ref.tclass = pcx->fe.tclass;
			}
		}
		if (w->num_out_fmts &&
		    fmts_single_bits(w->out_fmts, w->num_out_fmts) &&
		    w->id != DAPM_EFFECT) {
			ref.valid = w->out_fmts[0].valid_bits;
			ref.tclass = fmt_tclass(&w->out_fmts[0]);
		}
		oi = fmt_choose(w->out_fmts, w->num_out_fmts, &ref, 0, 0);
		break;
	}
	}

	if (ii < 0 && w->num_in_fmts) {
		tplg_err("%s: no usable input format\n", w->name);
		return -EINVAL;
	}
	if (!w->num_in_fmts && !w->num_out_fmts) {
		tplg_err("%s: the topology gives no audio formats\n", w->name);
		return -EINVAL;
	}
	base_from(w, ii, oi);
	ipc4_to_params(&w->out_format, out);
	w->fmt_valid = 1;
	return 0;
}

/* Gateway setup of a copier, after its formats are known. */
static int prepare_gateway(struct prep_ctx *pcx, struct tplg_widget *w)
{
	struct tplg_state *t = pcx->t;
	uint32_t ntype = w->node_type;
	uint32_t ms;

	w->dai_blob = NULL;
	w->dai_blob_len = 0;
	w->link_tag = 0;
	switch (w->id) {
	case DAPM_AIF_IN:
	case DAPM_AIF_OUT:
		if (!w->has_node_type) {
			tplg_err("%s: host copier without a node type\n",
				 w->name);
			return -EINVAL;
		}
		w->node_id = IPC4_NODE_TYPE(ntype) |
			     IPC4_NODE_INDEX((uint32_t)pcx->host_tag - 1);
		ms = umax32(IPC4_MIN_DMA_BUFFER_MS, w->deep_buffer_ms);
		if (ms > 1000)
			ms = 1000;
		w->dma_buffer_size = ms * (w->id == DAPM_AIF_IN ? w->base.ibs :
								   w->base.obs);
		return 0;

	case DAPM_BUFFER:
		w->node_id = IPC4_INVALID_NODE_ID;
		w->dma_buffer_size = 0;
		return 0;

	case DAPM_DAI_IN:
	case DAPM_DAI_OUT:
		break;
	default:
		return 0;
	}

	if (!w->has_node_type) {
		tplg_err("%s: DAI copier without a node type\n", w->name);
		return -EINVAL;
	}
	w->dma_buffer_size = IPC4_MIN_DMA_BUFFER_MS *
			     (w->id == DAPM_DAI_IN ? w->base.ibs : w->base.obs);

	switch (w->dai_type) {
	case SOF_DAI_HDA:
		/* HDA link DMA: the link stream uses the host stream's tag */
		w->link_tag = (uint32_t)pcx->host_tag;
		w->node_id = IPC4_NODE_TYPE(ntype) |
			     IPC4_NODE_INDEX((uint32_t)pcx->host_tag - 1);
		return 0;

	case SOF_DAI_SSP:
	case SOF_DAI_DMIC: {
		struct pparams p;
		uint32_t bits, rate, ch;
		int ltype = w->dai_type == SOF_DAI_SSP ? NHLT_LINK_SSP :
							 NHLT_LINK_DMIC;
		int ret, l;

		/* the format on the wire: the DAI side of the copier */
		if (w->id == DAPM_DAI_IN)
			ipc4_to_params(&w->out_format, &p);
		else
			ipc4_to_params(&w->base.audio_fmt, &p);
		rate = p.rate;
		ch = p.ch;
		bits = p.valid > 16 ? 32 : 16;
		if (w->dai_type == SOF_DAI_SSP) {
			/* an SSP port runs at its link's hardware config */
			for (l = 0; l < t->nlink; l++)
				if (name_eq(t->link[l].name, w->sname)) {
					if (t->link[l].hw_rate)
						rate = t->link[l].hw_rate;
					if (t->link[l].hw_channels)
						ch = t->link[l].hw_channels;
					if (t->link[l].hw_slot_width)
						bits = t->link[l].hw_slot_width;
					break;
				}
			w->node_id = IPC4_NODE_TYPE(ntype) |
				     IPC4_NODE_INDEX_SSP(w->dai_index);
		} else {
			w->node_id = IPC4_NODE_TYPE(ntype) |
				     IPC4_NODE_INDEX_DMIC(w->dai_index);
		}
		ret = dai_find_blob(t, ltype, (int)w->dai_index, pcx->dir, rate,
				    ch, bits, &w->dai_blob, &w->dai_blob_len);
		if (ret < 0 && w->dai_type == SOF_DAI_DMIC)
			ret = dai_find_blob(t, ltype, (int)w->dai_index,
					    pcx->dir, rate, ch,
					    bits == 32 ? 16 : 32, &w->dai_blob,
					    &w->dai_blob_len);
		if (ret < 0) {
			tplg_err("%s: no NHLT configuration for %u Hz %u ch "
				 "%u bit\n",
				 w->name, rate, ch, bits);
			return -ENOENT;
		}
		if (w->dai_blob_len > IPC4_MSG_MAX_SIZE - 256) {
			tplg_err("%s: NHLT blob too large\n", w->name);
			return -E2BIG;
		}
		return 0;
	}

	case SOF_DAI_ALH:
		/* node ids are filled in once the SoundWire side assigned
		 * the ALH streams (prepare_alh) */
		return 0;

	default:
		tplg_err("%s: unsupported DAI type '%s'\n", w->name,
			 w->dai_type_str[0] ? w->dai_type_str : "?");
		return -EOPNOTSUPP;
	}
}

static int alh_group_alloc(struct tplg_state *t)
{
	int i;

	for (i = 0; i < IPC4_ALH_MULTI_GTW_COUNT; i++)
		if (!(t->alh_groups_used & (1u << i))) {
			t->alh_groups_used |= 1u << i;
			return i;
		}
	return -ENOSPC;
}

static void alh_group_free(struct tplg_state *t, struct tplg_widget *w)
{
	if (w->alh_group >= 0 && w->alh_group < IPC4_ALH_MULTI_GTW_COUNT)
		t->alh_groups_used &= ~(1u << w->alh_group);
	w->alh_group = -1;
}

/* The SoundWire DAI configurations the DSP host recorded for this PCM
 * direction on `link': a bit per table slot. */
static uint32_t sdwcfg_snapshot(struct tplg_state *t, int link, int dir)
{
	struct sof_host *h = sof_host_of(t->sdev);
	uint32_t m = 0;
	int i;

	for (i = 0; i < SOF_MAX_SDW_CFG && i < 32; i++) {
		const struct sof_sdw_cfg *c = &h->sdwcfg[i];

		if (c->used && c->link == link && c->pcm_dir == dir &&
		    c->pcm_idx == h->prep_pcm)
			m |= 1u << i;
	}
	return m;
}

/* The ALH stream the SoundWire side assigned on `link' during the last
 * sdw_stream_prepare(): a configuration that appeared then, else any for
 * this PCM direction.  -1 if none.  `dma_tag' gets the HDA link DMA tag
 * the host attached to it (ACE 2+), 0 if none. */
static int sdwcfg_lookup(struct tplg_state *t, int link, int dir,
			 uint32_t before, int *dma_tag)
{
	struct sof_host *h = sof_host_of(t->sdev);
	uint32_t now = sdwcfg_snapshot(t, link, dir);
	uint32_t pick = (now & ~before) ? (now & ~before) : now;
	int i;

	*dma_tag = 0;
	for (i = 0; i < SOF_MAX_SDW_CFG && i < 32; i++) {
		const struct sof_sdw_cfg *c = &h->sdwcfg[i];

		if (!(pick & (1u << i)))
			continue;
		if (c->alh_stream < 0 || c->alh_stream > 0xff)
			continue;
		if (c->link_dma && c->link_dma->tag > 0)
			*dma_tag = c->link_dma->tag;
		return c->alh_stream;
	}
	return -1;
}

/* A SoundWire DAI: prepare the stream on its link (and on the links of
 * the DAIs aggregated with it: same stream name, one per link), learn the
 * ALH stream ids and build the gateway's device map. */
static int prepare_alh(struct prep_ctx *pcx, struct tplg_widget *w)
{
	struct tplg_state *t = pcx->t;
	struct tplg_stream_rt *rt = pcx->rt;
	int members[IPC4_ALH_MAX_DEVICES];
	uint32_t nm = 0, i, chmask, chcount, step, mask;
	struct pparams wire;
	int k, ret;

	for (k = 0; k < t->nw && nm < IPC4_ALH_MAX_DEVICES; k++) {
		const struct tplg_widget *o = &t->w[k];

		if (o->file != w->file || o->id != w->id ||
		    o->dai_type != SOF_DAI_ALH || !name_eq(o->sname, w->sname))
			continue;
		members[nm++] = k;
	}
	if (!nm)
		members[nm++] = (int)(w - t->w);

	if (w->id == DAPM_DAI_IN)
		ipc4_to_params(&w->out_format, &wire);
	else
		ipc4_to_params(&w->base.audio_fmt, &wire);
	chmask = chmap_mask(w->base.audio_fmt.ch_map, &chcount);
	if (!chcount) {
		chcount = wire.ch;
		chmask = chcount >= 32 ? 0xffffffffu : (1u << chcount) - 1;
	}
	/* playback with as many channels as the stream: every amplifier
	 * gets all channels; otherwise the channels are split evenly */
	if (w->id == DAPM_DAI_IN && chcount == pcx->fe.ch) {
		step = 0;
		mask = chmask;
	} else {
		step = chcount / nm;
		if (!step)
			step = 1;
		mask = step >= 32 ? 0xffffffffu : (1u << step) - 1;
	}

	w->alh_count = nm;
	for (i = 0; i < nm; i++) {
		struct tplg_widget *mw = &t->w[members[i]];
		int link = dai_sdw_link(t, mw);
		uint32_t mch = step ? step : chcount;
		uint32_t pmask = 0;
		int stream, j, dup = -1, dtag = 0;

		if (link < 0) {
			tplg_err("%s: cannot tell its SoundWire link\n",
				 mw->name);
			return -EINVAL;
		}
		mw->sdw_link = link;
		pmask = sdw_periph_mask(t, link, dai_function(mw));

		/* two devices of the gateway on one link: one stream */
		for (j = 0; j < rt->nsdw; j++)
			if (rt->sdw_link[j] == link)
				dup = j;
		if (dup >= 0) {
			stream = t->alh_stream[link][pcx->dir];
			dtag = t->alh_dma_tag[link][pcx->dir];
		} else {
			uint32_t before;

			if (rt->nsdw >= TPLG_MAX_PATH_DAIS)
				return -E2BIG;
			/* the SoundWire side reports the ALH stream it picks
			 * through sof_dai_sdw_config() into the DSP host's
			 * table, tagged with the PCM being prepared */
			before = sdwcfg_snapshot(t, link, pcx->dir);
			t->sdev->sdw_prep_function = dai_function(mw);
			ret = sdw_stream_prepare(t->sdev, link, pcx->dir,
						 wire.rate, mch,
						 wire.valid > 16 ? 32 :
								   wire.valid,
						 pmask);
			t->sdev->sdw_prep_function = 0;
			if (ret < 0) {
				tplg_err("SoundWire link %d: stream prepare "
					 "failed (%d)\n",
					 link, ret);
				return ret;
			}
			stream = sdwcfg_lookup(t, link, pcx->dir, before,
					       &dtag);
			t->alh_stream[link][pcx->dir] = stream;
			t->alh_dma_tag[link][pcx->dir] = dtag;
			rt->sdw_link[rt->nsdw] = link;
			rt->sdw_fn[rt->nsdw] = dai_function(mw);
			rt->sdw_enabled[rt->nsdw] = 0;
			rt->nsdw++;
		}

		if (stream < 0) {
			/* nobody reported the stream: the conventional
			 * numbering (16 per link, PDIs from 2, offset 3) */
			stream = link * 16 + 3 +
				 (pcx->dir == AUDIO_DIR_PLAYBACK ? 2 : 3);
			tplg_info("SoundWire link %d: no ALH stream reported, "
				  "assuming %d\n",
				  link, stream);
		}
		w->alh_link[i] = (uint8_t)link;
		w->alh_stream_id[i] = (uint8_t)stream;
		w->alh_dma_tag[i] = (uint8_t)dtag;
		w->alh_map[i].device = IPC4_NODE_TYPE(w->node_type) |
				       IPC4_NODE_INDEX((uint32_t)stream);
		w->alh_map[i].channel_mask = step ? mask << (step * i) : mask;
		if (i == 0)
			w->node_id = IPC4_NODE_TYPE(w->node_type) |
				     IPC4_NODE_INDEX((uint32_t)stream);
	}

	if (nm > 1) {
		int g = alh_group_alloc(t);

		if (g < 0)
			return g;
		w->alh_group = g;
		w->node_id = IPC4_NODE_TYPE(w->node_type) |
			     IPC4_NODE_INDEX(IPC4_ALH_MULTI_GTW_BASE + g);
	}
	return 0;
}

/* ==========================================================================
 * Module instantiation payloads
 * ========================================================================== */

/* The gateway DMA description newer DSPs (ACE 2 and later) take for link
 * DMA they do not own: one record per device of the gateway. */
struct ipc4_dma_config_tlv {
	uint32_t type; /* 0x1000 */
	uint32_t length; /* of the config that follows */
	uint8_t dma_method; /* 1 = HDA DMA */
	uint8_t pre_allocated_by_host;
	uint16_t rsvd;
	uint32_t dma_channel_id;
	uint32_t stream_id;
	uint32_t device_count;
	struct ipc4_alh_mapping mapping[16];
	uint32_t dma_priv_config_size;
} __attribute__((packed));

_Static_assert(sizeof(struct ipc4_dma_config_tlv) == 156, "dma tlv");

#define IPC4_GTW_DMA_CONFIG_ID 0x1000u
#define IPC4_DMA_METHOD_HDA 1

static int platform_has_dma_tlv(const struct tplg_state *t)
{
	return t->sdev->platform >= SOF_PLAT_LNL;
}

static uint32_t pipe_lp_mode(const struct tplg_state *t,
			     const struct tplg_widget *w)
{
	if (w->pipe < 0 || t->pipes[w->pipe].sched < 0)
		return 0;
	return t->w[t->pipes[w->pipe].sched].lp_mode & 1u;
}

/* The DMA record device of ALH gateway device i: (link << 8) | PDI, the
 * PDI being the ALH stream's offset within its link. */
static uint32_t alh_tlv_device(const struct tplg_widget *w, uint32_t i)
{
	uint32_t link = w->alh_link[i];
	uint32_t s = w->alh_stream_id[i];
	uint32_t pdi = s >= link * 16 + 3 ? s - link * 16 - 3 : 0;

	return (link << 8) | (pdi & 0xff);
}

/* Build the INIT_INSTANCE payload of a module into buf (cap bytes). */
static int build_init_payload(struct tplg_state *t, struct prep_ctx *pcx,
			      struct tplg_widget *w, uint8_t *buf,
			      uint32_t cap, uint32_t *len)
{
	uint32_t n = 0;

	switch (w->id) {
	case DAPM_AIF_IN:
	case DAPM_AIF_OUT:
	case DAPM_DAI_IN:
	case DAPM_DAI_OUT:
	case DAPM_BUFFER: {
		struct ipc4_copier_data cd;
		uint32_t words[2 + 2 * IPC4_ALH_MAX_DEVICES];
		uint32_t nwords = 0, blob_bytes = 0, tlv_count = 0, i, off, tag;
		uint32_t pad;
		const uint8_t *blob = NULL;
		struct ipc4_dma_config_tlv d;

		kmemset(&cd, 0, sizeof(cd));
		cd.base = w->base;
		cd.out_format = w->out_format;
		cd.gtw_cfg.node_id = w->node_id;
		cd.gtw_cfg.dma_buffer_size = w->dma_buffer_size;

		if (w->id == DAPM_BUFFER) {
			nwords = 0;
		} else if (widget_is_host(w) || w->dai_type == SOF_DAI_HDA) {
			words[nwords++] = pipe_lp_mode(t, w); /* gtw attrs */
		} else if (w->dai_type == SOF_DAI_ALH) {
			words[nwords++] = 0; /* gtw attrs: no lp buffer */
			words[nwords++] = w->alh_count;
			for (i = 0; i < w->alh_count; i++) {
				words[nwords++] = w->alh_map[i].device;
				words[nwords++] = w->alh_map[i].channel_mask;
			}
		} else if (w->dai_type == SOF_DAI_SSP ||
			   w->dai_type == SOF_DAI_DMIC) {
			blob = w->dai_blob;
			blob_bytes = w->dai_blob_len & ~3u;
		}

		/* ACE 2+: link DMA records for SoundWire/SSP/DMIC, one per
		 * device of the gateway */
		if (widget_is_dai(w) && w->dai_type != SOF_DAI_HDA &&
		    platform_has_dma_tlv(t) && pcx)
			tlv_count = w->dai_type == SOF_DAI_ALH ?
					    umin32(w->alh_count,
						   IPC4_ALH_MAX_DEVICES) :
					    1;
		/* the gateway map names each device by its DMA record */
		for (i = 0; i < tlv_count && w->dai_type == SOF_DAI_ALH; i++)
			words[2 + 2 * i] = alh_tlv_device(w, i);

		cd.gtw_cfg.config_length =
			nwords + blob_bytes / 4 +
			tlv_count * (uint32_t)(sizeof(d) / 4);
		/* The firmware's copier configuration ends in a one-word
		 * gateway config array, and a payload shorter than that
		 * structure is refused ("cfg size exceeds init payload"):
		 * a copier without gateway config -- the module copier --
		 * still sends the word, zero, with config_length 0. */
		pad = cd.gtw_cfg.config_length ? 0 : 4;
		n = sizeof(cd) + nwords * 4 + blob_bytes +
		    tlv_count * (uint32_t)sizeof(d) + pad;
		if (n > cap)
			return -E2BIG;
		kmemset(buf, 0, n);
		kmemcpy(buf, &cd, sizeof(cd));
		off = sizeof(cd);
		kmemcpy(buf + off, words, nwords * 4);
		off += nwords * 4;
		if (blob_bytes)
			kmemcpy(buf + off, blob, blob_bytes);
		off += blob_bytes;
		for (i = 0; i < tlv_count; i++) {
			kmemset(&d, 0, sizeof(d));
			d.type = IPC4_GTW_DMA_CONFIG_ID;
			d.length = sizeof(d) - 8;
			d.dma_method = IPC4_DMA_METHOD_HDA;
			d.pre_allocated_by_host = 1;
			/* the HDA link DMA the host attached to the device
			 * (reported with its SoundWire stream), else the
			 * host stream's own tag */
			tag = (uint32_t)pcx->host_tag;
			if (w->dai_type == SOF_DAI_ALH && w->alh_dma_tag[i])
				tag = w->alh_dma_tag[i];
			d.dma_channel_id = tag - 1;
			d.stream_id = tag;
			d.device_count = 1;
			if (w->dai_type == SOF_DAI_ALH) {
				d.mapping[0].device = alh_tlv_device(w, i);
				d.mapping[0].channel_mask =
					w->alh_map[i].channel_mask;
			}
			kmemcpy(buf + off, &d, sizeof(d));
			off += sizeof(d);
		}
		break;
	}

	case DAPM_PGA: {
		struct ipc4_gain_data g;

		kmemset(&g, 0, sizeof(g));
		g.base = w->base;
		g.params.channels = IPC4_GAIN_ALL_CHANNELS;
		g.params.init_val = w->has_gain_init ? w->gain_init :
						       IPC4_VOL_ZERO_DB;
		if (pcx && pcx->rt->gain == (int)(w - t->w) &&
		    pcx->pc->vol_set)
			g.params.init_val =
				volume_to_gain(w, umax32(pcx->pc->vol_l,
							 pcx->pc->vol_r));
		g.params.curve_type = w->gain_ramp_type;
		g.params.curve_duration_l = w->gain_ramp_duration;
		n = sizeof(g);
		if (n > cap)
			return -E2BIG;
		kmemcpy(buf, &g, n);
		break;
	}

	case DAPM_SRC: {
		struct ipc4_src_data s;

		kmemset(&s, 0, sizeof(s));
		s.base = w->base;
		s.sink_rate = w->out_format.sampling_frequency;
		n = sizeof(s);
		if (n > cap)
			return -E2BIG;
		kmemcpy(buf, &s, n);
		break;
	}

	case DAPM_ASRC: {
		struct ipc4_asrc_data a;

		kmemset(&a, 0, sizeof(a));
		a.base = w->base;
		a.out_freq = w->out_format.sampling_frequency;
		a.asrc_mode = w->asrc_mode;
		n = sizeof(a);
		if (n > cap)
			return -E2BIG;
		kmemcpy(buf, &a, n);
		break;
	}

	case DAPM_MIXER:
		n = sizeof(w->base);
		if (n > cap)
			return -E2BIG;
		kmemcpy(buf, &w->base, n);
		break;

	default: {
		/* processing modules: the base config, plus the format of
		 * every pin when the module asks for the extended form */
		uint32_t npins, i;

		n = sizeof(w->base);
		if (n > cap)
			return -E2BIG;
		kmemcpy(buf, &w->base, n);
		if (IPC4_MODULE_INIT_CONFIG(w->mod.type) !=
		    IPC4_INIT_CONFIG_BASE_CFG_WITH_EXT)
			break;
		npins = w->num_input_pins + w->num_output_pins;
		if (n + sizeof(struct ipc4_base_module_cfg_ext) +
			    npins * sizeof(struct ipc4_pin_format) >
		    cap)
			return -E2BIG;
		{
			struct ipc4_base_module_cfg_ext ext;
			struct ipc4_pin_format pf;
			uint32_t pin;

			kmemset(&ext, 0, sizeof(ext));
			ext.num_input_pin_fmts = (uint16_t)w->num_input_pins;
			ext.num_output_pin_fmts = (uint16_t)w->num_output_pins;
			kmemcpy(buf + n, &ext, sizeof(ext));
			n += sizeof(ext);
			for (i = 0; i < npins; i++) {
				int input = i < w->num_input_pins;
				const struct tplg_fmt *l = input ? w->in_fmts :
								   w->out_fmts;
				uint32_t nl = input ? w->num_in_fmts :
						      w->num_out_fmts;
				uint32_t j;

				pin = input ? i : i - w->num_input_pins;
				kmemset(&pf, 0, sizeof(pf));
				pf.pin_index = pin;
				if (pin == 0) {
					pf.buffer_size = input ? w->base.ibs :
								 w->base.obs;
					pf.audio_fmt = input ?
							       w->base.audio_fmt :
							       w->out_format;
				} else {
					for (j = 0; j < nl; j++)
						if (l[j].pin_index == pin)
							break;
					if (j == nl) {
						tplg_err("%s: no format for "
							 "%s pin %u\n",
							 w->name,
							 input ? "input" :
								 "output",
							 pin);
						return -EINVAL;
					}
					pf.buffer_size = l[j].buffer_size;
					fmt_to_ipc4(&l[j], &pf.audio_fmt);
				}
				kmemcpy(buf + n, &pf, sizeof(pf));
				n += sizeof(pf);
			}
		}
		break;
	}
	}
	*len = n;
	return 0;
}

static int volume_send(struct tplg_state *t, struct tplg_widget *w,
		       uint32_t l, uint32_t r)
{
	struct ipc4_gain_params gp;
	uint32_t gl = volume_to_gain(w, l), gr = volume_to_gain(w, r);
	int ret;

	kmemset(&gp, 0, sizeof(gp));
	gp.curve_type = w->gain_ramp_type;
	gp.curve_duration_l = w->gain_ramp_duration;
	if (gl == gr) {
		gp.channels = IPC4_GAIN_ALL_CHANNELS;
		gp.init_val = gl;
		return ipc_large_config_set(t, w, IPC4_GAIN_PARAM_ID, &gp,
					    sizeof(gp));
	}
	gp.channels = 0;
	gp.init_val = gl;
	ret = ipc_large_config_set(t, w, IPC4_GAIN_PARAM_ID, &gp, sizeof(gp));
	if (ret < 0)
		return ret;
	gp.channels = 1;
	gp.init_val = gr;
	return ipc_large_config_set(t, w, IPC4_GAIN_PARAM_ID, &gp, sizeof(gp));
}

/* Send the default data of the module's bytes controls (filter
 * coefficients, processing configuration) right after it exists. */
static int widget_send_bytes(struct tplg_state *t, struct tplg_widget *w)
{
	int i, ret;

	for (i = 0; i < w->nkctl; i++) {
		const struct tplg_kctl *kc = &w->kctl[i];
		uint32_t type, size;

		if (kc->kind != TPLG_TYPE_BYTES || !kc->bytes ||
		    kc->bytes_len < SOF_ABI_HDR_SIZE)
			continue;
		kmemcpy(&type, kc->bytes + 4, 4);
		kmemcpy(&size, kc->bytes + 8, 4);
		if (!size || size > kc->bytes_len - SOF_ABI_HDR_SIZE)
			continue;
		ret = ipc_large_config_set(t, w, type & 0xffu,
					   kc->bytes + SOF_ABI_HDR_SIZE, size);
		if (ret < 0) {
			tplg_err("%s: control '%s' data rejected (%d)\n",
				 w->name, kc->name, ret);
			return ret;
		}
	}
	return 0;
}

static int widget_init(struct tplg_state *t, struct prep_ctx *pcx,
		       struct tplg_widget *w)
{
	uint8_t *buf;
	uint32_t len = 0, pre = 0, primary, ext, ppl;
	int ret, inst;

	if (w->pipe < 0 || t->pipes[w->pipe].instance_id < 0)
		return -EINVAL;
	ppl = (uint32_t)t->pipes[w->pipe].instance_id;
	buf = zalloc(IPC4_MSG_MAX_SIZE);
	if (!buf)
		return -ENOMEM;

	/* modules in the data-processing domain carry their memory needs
	 * in an extended-init object array in front of the payload */
	if (w->domain == TPLG_DOMAIN_DP) {
		struct ipc4_module_init_ext_init ei;
		struct ipc4_dp_memory_data dm;
		uint32_t hdr;

		kmemset(&ei, 0, sizeof(ei));
		ei.word0 = IPC4_MOD_INIT_EXT_OBJ_ARRAY;
		hdr = IPC4_MOD_INIT_EXT_OBJ_ID(IPC4_MOD_INIT_DATA_ID_DP_DATA) |
		      IPC4_MOD_INIT_EXT_OBJ_WORDS(sizeof(dm) / 4) |
		      IPC4_MOD_INIT_EXT_OBJ_LAST;
		dm.domain_id = w->domain_id;
		dm.stack_bytes = w->stack_bytes;
		dm.heap_bytes = w->heap_bytes;
		kmemcpy(buf, &ei, sizeof(ei));
		kmemcpy(buf + sizeof(ei), &hdr, 4);
		kmemcpy(buf + sizeof(ei) + 4, &dm, sizeof(dm));
		pre = sizeof(ei) + 4 + sizeof(dm);
	}
	ret = build_init_payload(t, pcx, w, buf + pre, IPC4_MSG_MAX_SIZE - pre,
				 &len);
	if (ret < 0) {
		tplg_err("%s: cannot build its configuration (%d)\n", w->name,
			 ret);
		kfree(buf);
		return ret;
	}
	len += pre;

	inst = mod_instance_alloc(t, w);
	if (inst < 0) {
		tplg_err("%s: no free module instance\n", w->name);
		kfree(buf);
		return inst;
	}
	w->instance_id = inst;
	primary = mod_primary(w, IPC4_MOD_INIT_INSTANCE);
	ext = IPC4_MOD_EXT_PARAM_SIZE(len / 4) | IPC4_MOD_EXT_PPL_ID(ppl) |
	      IPC4_MOD_EXT_CORE_ID(w->core) |
	      IPC4_MOD_EXT_DOMAIN(w->domain == TPLG_DOMAIN_DP ? 1 :
				  (w->domain == TPLG_DOMAIN_UNSET &&
				   (w->mod.type & IPC4_MODULE_DP)) ?
								  1 :
								  0);
	if (pre)
		ext |= IPC4_MOD_EXT_EXTENDED_INIT;
	tplg_dbg("init %s: module %u instance %d pipe %u, %u bytes\n",
		 w->name, w->mod.module_id, inst, ppl, len);
	ret = ipc_tx(t, primary, ext, buf, len);
	kfree(buf);
	if (ret < 0) {
		tplg_err("%s: firmware refused the module (%d)\n", w->name,
			 ret);
		mod_instance_free(t, w);
		return ret;
	}
	ret = widget_send_bytes(t, w);
	if (ret < 0)
		return ret;
	if (pcx && w->id == DAPM_PGA && pcx->rt->gain == (int)(w - t->w) &&
	    pcx->pc->vol_set && pcx->pc->vol_l != pcx->pc->vol_r)
		volume_send(t, w, pcx->pc->vol_l, pcx->pc->vol_r);
	return 0;
}

static void widget_delete(struct tplg_state *t, struct tplg_widget *w,
			  int send)
{
	if (w->instance_id < 0)
		return;
	if (send)
		ipc_tx(t, mod_primary(w, IPC4_MOD_DELETE_INSTANCE), 0, NULL, 0);
	mod_instance_free(t, w);
	alh_group_free(t, w);
	w->fmt_valid = 0;
	w->in_q_used = w->out_q_used = 0;
}

/* ==========================================================================
 * Binding modules
 * ========================================================================== */

static int queue_id(struct tplg_widget *self, const struct tplg_widget *peer,
		    int output)
{
	uint32_t npins = output ? self->num_output_pins : self->num_input_pins;
	int nbind = output ? self->n_out_bind : self->n_in_bind;
	char(*bind)[SOF_TPLG_NAME_MAX] = output ? self->out_bind :
						   self->in_bind;
	uint32_t *used = output ? &self->out_q_used : &self->in_q_used;
	uint32_t i;

	if (npins <= 1)
		return 0;
	if (nbind) {
		for (i = 0; i < (uint32_t)nbind && i < npins; i++)
			if (!kstrcmp(bind[i], peer->name))
				return (int)i;
		tplg_err("%s: no %s pin bound to %s\n", self->name,
			 output ? "output" : "input", peer->name);
		return -EINVAL;
	}
	for (i = 0; i < npins && i < 32; i++)
		if (!(*used & (1u << i))) {
			*used |= 1u << i;
			return (int)i;
		}
	return -ENOSPC;
}

static void queue_id_put(struct tplg_widget *self, int q, int output)
{
	uint32_t npins = output ? self->num_output_pins : self->num_input_pins;
	int nbind = output ? self->n_out_bind : self->n_in_bind;

	if (npins <= 1 || nbind || q < 0 || q >= 32)
		return;
	if (output)
		self->out_q_used &= ~(1u << q);
	else
		self->in_q_used &= ~(1u << q);
}

static int route_bind(struct tplg_state *t, struct tplg_route *r)
{
	struct tplg_widget *src = &t->w[r->src], *sink = &t->w[r->sink];
	uint32_t primary, ext;
	int ret;

	r->src_q = queue_id(src, sink, 1);
	if (r->src_q < 0)
		return r->src_q;
	r->dst_q = queue_id(sink, src, 0);
	if (r->dst_q < 0) {
		queue_id_put(src, r->src_q, 1);
		return r->dst_q;
	}
	/* a copier's extra outputs need their format before binding */
	if (r->src_q > 0 && widget_is_copier(src)) {
		struct ipc4_copier_sink_format sf;

		kmemset(&sf, 0, sizeof(sf));
		sf.sink_id = (uint32_t)r->src_q;
		sf.source_fmt = src->base.audio_fmt;
		sf.sink_fmt = sink->base.audio_fmt;
		ret = ipc_large_config_set(t, src,
					   IPC4_COPIER_PARAM_SET_SINK_FORMAT,
					   &sf, sizeof(sf));
		if (ret < 0)
			goto fail;
	}
	primary = mod_primary(src, IPC4_MOD_BIND);
	ext = IPC4_MOD_EXT_DST_ID(sink->mod.module_id) |
	      IPC4_MOD_EXT_DST_INSTANCE((uint32_t)sink->instance_id) |
	      IPC4_MOD_EXT_DST_QUEUE((uint32_t)r->dst_q) |
	      IPC4_MOD_EXT_SRC_QUEUE((uint32_t)r->src_q);
	tplg_dbg("bind %s:%d -> %s:%d\n", src->name, r->src_q, sink->name,
		 r->dst_q);
	ret = ipc_tx(t, primary, ext, NULL, 0);
	if (ret < 0)
		goto fail;
	return 0;
fail:
	tplg_err("cannot bind %s -> %s (%d)\n", src->name, sink->name, ret);
	queue_id_put(src, r->src_q, 1);
	queue_id_put(sink, r->dst_q, 0);
	r->src_q = r->dst_q = -1;
	return ret;
}

static void route_unbind(struct tplg_state *t, struct tplg_route *r)
{
	struct tplg_widget *src = &t->w[r->src], *sink = &t->w[r->sink];

	/* bindings inside one pipeline go away with the pipeline */
	if (src->pipe != sink->pipe && src->instance_id >= 0 &&
	    sink->instance_id >= 0) {
		uint32_t primary = mod_primary(src, IPC4_MOD_UNBIND);
		uint32_t ext =
			IPC4_MOD_EXT_DST_ID(sink->mod.module_id) |
			IPC4_MOD_EXT_DST_INSTANCE((uint32_t)sink->instance_id) |
			IPC4_MOD_EXT_DST_QUEUE((uint32_t)r->dst_q) |
			IPC4_MOD_EXT_SRC_QUEUE((uint32_t)r->src_q);

		ipc_tx(t, primary, ext, NULL, 0);
	}
	queue_id_put(src, r->src_q, 1);
	queue_id_put(sink, r->dst_q, 0);
	r->src_q = r->dst_q = -1;
}

/* ==========================================================================
 * The path of a PCM direction
 *
 * From the host copier, follow the routes in the data direction (towards
 * the sinks for playback, towards the sources for capture) and collect the
 * firmware modules up to and including the DAI copiers.  Other host
 * copiers (e.g. an echo-reference capture tapping a playback path) end the
 * walk.  The set is then ordered by data flow, which is the order modules
 * are configured in.
 * ========================================================================== */

static int in_list(const int *l, int n, int v)
{
	int i;

	for (i = 0; i < n; i++)
		if (l[i] == v)
			return i;
	return -1;
}

static int path_build(struct tplg_state *t, struct tplg_pcm_int *pc, int dir,
		      struct tplg_stream_rt *rt)
{
	int visit[TPLG_MAX_PATH * 2];
	int set[TPLG_MAX_PATH], indeg[TPLG_MAX_PATH], done[TPLG_MAX_PATH];
	int nv = 0, vh = 0, ns = 0, i, k, host = pc->host[dir];
	int ordered = 0;

	rt->nw = rt->np = rt->nr = rt->nd = 0;
	rt->gain = -1;
	if (host < 0)
		return -ENODEV;
	visit[nv++] = host;
	while (vh < nv) {
		int cur = visit[vh++];
		const struct tplg_widget *cw = &t->w[cur];

		/* virtual widgets (codec-side helpers without a firmware
		 * module) end the path */
		if (!widget_is_module(cw))
			continue;
		if (ns >= TPLG_MAX_PATH) {
			tplg_err("PCM %s: path longer than %d modules\n",
				 pc->pub.name, TPLG_MAX_PATH);
			return -E2BIG;
		}
		set[ns++] = cur;
		/* a DAI copier ends the path; so does another host copier */
		if (cur != host && (widget_is_dai(cw) || widget_is_host(cw)))
			continue;
		for (i = 0; i < t->nr; i++) {
			const struct tplg_route *r = &t->r[i];
			int next;

			if (dir == AUDIO_DIR_PLAYBACK && r->src == cur)
				next = r->sink;
			else if (dir == AUDIO_DIR_CAPTURE && r->sink == cur)
				next = r->src;
			else
				continue;
			if (widget_is_host(&t->w[next]) && next != host)
				continue;
			if (in_list(visit, nv, next) >= 0)
				continue;
			if (nv >= (int)ARRAY_LEN(visit))
				return -E2BIG;
			visit[nv++] = next;
		}
	}

	/* order by data flow (Kahn), stable with respect to the walk */
	for (i = 0; i < ns; i++) {
		indeg[i] = 0;
		done[i] = 0;
	}
	for (i = 0; i < t->nr; i++) {
		int s = in_list(set, ns, t->r[i].src);
		int d = in_list(set, ns, t->r[i].sink);

		if (s >= 0 && d >= 0 && s != d)
			indeg[d]++;
	}
	while (ordered < ns) {
		int pick = -1;

		for (i = 0; i < ns; i++)
			if (!done[i] && indeg[i] == 0) {
				pick = i;
				break;
			}
		if (pick < 0) /* a cycle: take the walk order */
			for (i = 0; i < ns; i++)
				if (!done[i]) {
					pick = i;
					break;
				}
		done[pick] = 1;
		rt->w[rt->nw] = set[pick];
		rt->w_acq[rt->nw] = 0;
		rt->nw++;
		ordered++;
		for (i = 0; i < t->nr; i++)
			if (t->r[i].src == set[pick]) {
				int d = in_list(set, ns, t->r[i].sink);

				if (d >= 0 && indeg[d] > 0)
					indeg[d]--;
			}
	}

	/* routes inside the path, pipelines, DAIs, the volume gain */
	for (i = 0; i < t->nr; i++) {
		if (in_list(rt->w, rt->nw, t->r[i].src) < 0 ||
		    in_list(rt->w, rt->nw, t->r[i].sink) < 0 ||
		    t->r[i].src == t->r[i].sink)
			continue;
		if (rt->nr >= TPLG_MAX_PATH_ROUTES)
			return -E2BIG;
		rt->r[rt->nr] = i;
		rt->r_acq[rt->nr] = 0;
		rt->nr++;
	}
	for (k = 0; k < rt->nw; k++) {
		const struct tplg_widget *w = &t->w[rt->w[k]];

		if (w->pipe >= 0 && in_list(rt->p, rt->np, w->pipe) < 0) {
			if (rt->np >= TPLG_MAX_PATH_PIPES)
				return -E2BIG;
			rt->p[rt->np] = w->pipe;
			rt->p_acq[rt->np] = 0;
			rt->np++;
		}
		if (widget_is_dai(w) && rt->nd < TPLG_MAX_PATH_DAIS)
			rt->dai[rt->nd++] = rt->w[k];
		if (w->id == DAPM_PGA && rt->gain < 0)
			rt->gain = rt->w[k];
	}
	if (!rt->nd)
		tplg_info("PCM %s: the %s path reaches no DAI\n", pc->pub.name,
			  dir == AUDIO_DIR_PLAYBACK ? "playback" : "capture");
	return 0;
}

/* ==========================================================================
 * Prepare / trigger / free
 * ========================================================================== */

static void stream_teardown(struct tplg_state *t, struct tplg_pcm_int *pc,
			    int dir);

/* ---- chained DMA ----------------------------------------------------------
 *
 * Pipelines marked for chained DMA (typically HDMI/DP) carry no
 * processing: the firmware connects the host DMA to the HDA link DMA and
 * moves the samples itself.  Nothing is created at prepare time; the trigger
 * allocates/enables the chain, a stop disables it, the free releases it.
 */
static int chain_prepare(struct tplg_state *t, struct tplg_pcm_int *pc,
			 int dir, uint32_t rate, uint32_t channels,
			 uint32_t bits, int host_tag)
{
	struct tplg_stream_rt *rt = &pc->rt[dir];
	uint32_t width = bits <= 8 ? 8 : bits <= 16 ? 16 : 32;
	uint64_t fifo;

	(void)t;
	if (dir != AUDIO_DIR_PLAYBACK) {
		/* capture DMA ids are offset by the playback stream count,
		 * which the topology code does not know */
		tplg_err("PCM %s: chained DMA capture is not supported\n",
			 pc->pub.name);
		return -EOPNOTSUPP;
	}
	if (host_tag < 1 || host_tag > 32)
		return -EINVAL;
	fifo = ((uint64_t)IPC4_CHAIN_DMA_BUF_MS * rate * channels * width +
		7999) /
	       8000;
	if (fifo > IPC4_CHAIN_DMA_FIFO_SIZE(0xffffffffu))
		return -EINVAL;
	rt->chain = 1;
	rt->chain_allocated = 0;
	rt->chain_fifo = (uint32_t)fifo;
	rt->chain_primary =
		IPC4_CHAIN_DMA_HOST_ID((uint32_t)host_tag - 1) |
		IPC4_CHAIN_DMA_LINK_ID((uint32_t)host_tag - 1) |
		(width <= 16 ? IPC4_CHAIN_DMA_SCS : 0) |
		IPC4_MSG_TYPE(IPC4_GLB_CHAIN_DMA) | IPC4_MSG_REQUEST |
		IPC4_MSG_TARGET_FW;
	rt->prepared = 1;
	rt->state = IPC4_PIPE_RESET;
	return 0;
}

/* allocate/enable as asked; `fifo' only when a new stream starts */
static int chain_msg(struct tplg_state *t, struct tplg_stream_rt *rt,
		     int allocate, int enable, int fifo)
{
	uint32_t primary = rt->chain_primary;
	int ret;

	if (allocate)
		primary |= IPC4_CHAIN_DMA_ALLOCATE;
	if (enable)
		primary |= IPC4_CHAIN_DMA_ENABLE;
	ret = ipc_tx(t, primary,
		     fifo ? IPC4_CHAIN_DMA_FIFO_SIZE(rt->chain_fifo) : 0, NULL,
		     0);
	if (ret == 0)
		rt->chain_allocated = allocate;
	return ret;
}

static int chain_trigger(struct tplg_state *t, struct tplg_stream_rt *rt,
			 int cmd)
{
	int ret;

	switch (cmd) {
	case SND_TRIGGER_START:
	case SND_TRIGGER_RESUME:
		if (rt->state == IPC4_PIPE_RUNNING)
			return 0;
		ret = chain_msg(t, rt, 1, 1, !rt->pushed);
		if (ret == 0) {
			rt->state = IPC4_PIPE_RUNNING;
			rt->pushed = 0;
		}
		return ret;
	case SND_TRIGGER_PAUSE:
	case SND_TRIGGER_STOP:
		if (rt->state != IPC4_PIPE_RUNNING) {
			if (cmd == SND_TRIGGER_STOP)
				rt->pushed = 0;
			return 0;
		}
		ret = chain_msg(t, rt, 1, 0, 0);
		if (ret == 0) {
			rt->state = IPC4_PIPE_PAUSED;
			rt->pushed = cmd == SND_TRIGGER_PAUSE;
		}
		return ret;
	default:
		return -EINVAL;
	}
}

static void chain_teardown(struct tplg_state *t, struct tplg_stream_rt *rt)
{
	if (rt->state == IPC4_PIPE_RUNNING)
		chain_trigger(t, rt, SND_TRIGGER_STOP);
	if (rt->chain_allocated)
		chain_msg(t, rt, 0, 0, 0);
	rt->chain = 0;
	rt->chain_allocated = 0;
	rt->pushed = 0;
	rt->prepared = 0;
	rt->state = IPC4_PIPE_UNINITIALIZED;
}

static int stream_prepare(struct tplg_state *t, struct tplg_pcm_int *pc,
			  int dir, uint32_t rate, uint32_t channels,
			  uint32_t bits, int host_tag)
{
	struct tplg_stream_rt *rt = &pc->rt[dir];
	struct pparams outp[TPLG_MAX_PATH];
	struct prep_ctx pcx;
	int i, k, ret;

	kmemset(&pcx, 0, sizeof(pcx));
	pcx.t = t;
	pcx.pc = pc;
	pcx.rt = rt;
	pcx.dir = dir;
	pcx.host_tag = host_tag;
	pcx.fe.rate = rate;
	pcx.fe.ch = channels;
	pcx.fe.valid = bits;
	pcx.fe.tclass = TCLASS_INT;

	ret = path_build(t, pc, dir, rt);
	if (ret < 0)
		return ret;
	rt->nsdw = 0;
	rt->rate = rate;
	rt->channels = channels;
	rt->bits = bits;
	rt->host_tag = host_tag;

	/* chained DMA: no modules, the firmware just links the DMAs */
	rt->chain = 0;
	rt->chain_allocated = 0;
	for (i = 0, k = 0; i < rt->np; i++)
		if (t->pipes[rt->p[i]].sched >= 0 &&
		    t->w[t->pipes[rt->p[i]].sched].use_chain_dma)
			k++;
	if (k && k != rt->np) {
		tplg_err("PCM %s: chained DMA mixed with DSP pipelines\n",
			 pc->pub.name);
		return -EINVAL;
	}
	if (k)
		return chain_prepare(t, pc, dir, rate, channels, bits,
				     host_tag);

	/* everything must exist in the firmware and be well formed */
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];

		if (w->pipe < 0 || t->pipes[w->pipe].sched < 0) {
			tplg_err("%s: not in a pipeline\n", w->name);
			return -EINVAL;
		}
		ret = widget_resolve_module(t, w);
		if (ret < 0)
			return ret;
	}

	/* formats, in data-flow order */
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];
		struct pparams in = pcx.fe;
		int pred = -1;

		if (w->use_count > 0 && w->fmt_valid) {
			/* shared with a running stream: keep its formats */
			ipc4_to_params(&w->out_format, &outp[k]);
			continue;
		}
		for (i = 0; i < rt->nr && pred < 0; i++) {
			const struct tplg_route *r = &t->r[rt->r[i]];
			int s;

			if (r->sink != rt->w[k])
				continue;
			s = in_list(rt->w, k, r->src);
			if (s >= 0)
				pred = s;
		}
		if (pred < 0) {
			/* a shared upstream module outside this path? */
			for (i = 0; i < t->nr; i++)
				if (t->r[i].sink == rt->w[k] &&
				    t->w[t->r[i].src].fmt_valid &&
				    t->w[t->r[i].src].use_count > 0) {
					ipc4_to_params(
						&t->w[t->r[i].src].out_format,
						&in);
					break;
				}
		} else {
			in = outp[pred];
		}
		ret = prepare_module(&pcx, w, &in, &outp[k]);
		if (ret < 0)
			return ret;
		tplg_dbg("%s: in %u Hz %u ch %u bit -> out %u Hz %u ch %u bit\n",
			 w->name, in.rate, in.ch, in.valid, outp[k].rate,
			 outp[k].ch, outp[k].valid);
	}

	/* gateways (host DMA, link DMA, SSP/DMIC blobs, SoundWire) */
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];

		if (w->use_count > 0 || !widget_is_copier(w))
			continue;
		ret = prepare_gateway(&pcx, w);
		if (ret < 0)
			return ret;
		if (widget_is_dai(w) && w->dai_type == SOF_DAI_ALH) {
			ret = prepare_alh(&pcx, w);
			if (ret < 0)
				return ret;
		}
	}

	/* pipelines first, holding their memory budget */
	for (i = 0; i < rt->np; i++)
		if (t->pipes[rt->p[i]].instance_id < 0)
			t->pipes[rt->p[i]].mem_pages = 0;
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];
		struct tplg_pipe *p = &t->pipes[w->pipe];

		if (w->use_count == 0 && p->instance_id < 0)
			p->mem_pages += module_pages(w);
	}
	for (i = 0; i < rt->np; i++) {
		struct tplg_pipe *p = &t->pipes[rt->p[i]];

		if (p->instance_id < 0) {
			int id = pipe_id_alloc(t);

			if (id < 0) {
				tplg_err("no free pipeline instance\n");
				return id;
			}
			p->instance_id = id;
			ret = ipc_pipe_create(t, p);
			if (ret < 0) {
				tplg_err("pipeline %u: create failed (%d)\n",
					 p->index, ret);
				pipe_id_free(t, id);
				p->instance_id = -1;
				p->mem_pages = 0;
				return ret;
			}
			p->state = IPC4_PIPE_RESET;
			p->started = p->paused = 0;
		}
		p->use_count++;
		rt->p_acq[i] = 1;
	}

	/* modules */
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];

		if (w->instance_id < 0) {
			ret = widget_init(t, &pcx, w);
			if (ret < 0) {
				/* instantiated but not configured: hold it
				 * so the teardown deletes it */
				if (w->instance_id >= 0) {
					w->use_count++;
					rt->w_acq[k] = 1;
				}
				return ret;
			}
		}
		w->use_count++;
		rt->w_acq[k] = 1;
	}

	/* bindings */
	for (i = 0; i < rt->nr; i++) {
		struct tplg_route *r = &t->r[rt->r[i]];

		if (r->refs == 0) {
			ret = route_bind(t, r);
			if (ret < 0)
				return ret;
		}
		r->refs++;
		rt->r_acq[i] = 1;
	}

	rt->prepared = 1;
	rt->state = IPC4_PIPE_RESET;
	return 0;
}

/* Collect the pipelines of a stream whose state has to change to reach
 * `state', following the shared-pipeline bookkeeping, ordered by
 * priority.  Returns the count. */
static int collect_pipes(struct tplg_state *t, struct tplg_stream_rt *rt,
			 int state, int *ids, int *idx)
{
	int n = 0, i, j;
	int prio[TPLG_MAX_PATH_PIPES];

	for (j = 0; j < rt->np; j++) {
		/* running and reset go sink -> source, pause source -> sink */
		int pi = (state == IPC4_PIPE_PAUSED) ? j : rt->np - 1 - j;
		struct tplg_pipe *p = &t->pipes[rt->p[pi]];
		int pr, include = 0, pos;

		if (!rt->p_acq[pi] || p->instance_id < 0)
			continue;
		switch (state) {
		case IPC4_PIPE_RUNNING:
			include = p->started == p->paused;
			break;
		case IPC4_PIPE_PAUSED:
			include = p->paused == p->started - 1;
			break;
		case IPC4_PIPE_RESET:
			include = !p->started && !p->paused &&
				  p->state != IPC4_PIPE_RESET;
			break;
		}
		if (!include)
			continue;
		pr = p->sched >= 0 ? (int)t->w[p->sched].priority : 0;
		/* running: high priority first; others: low first */
		for (pos = 0; pos < n; pos++)
			if (state == IPC4_PIPE_RUNNING ? pr > prio[pos] :
							 pr < prio[pos])
				break;
		for (i = n; i > pos; i--) {
			ids[i] = ids[i - 1];
			idx[i] = idx[i - 1];
			prio[i] = prio[i - 1];
		}
		ids[pos] = p->instance_id;
		idx[pos] = rt->p[pi];
		prio[pos] = pr;
		n++;
	}
	return n;
}

static void sdw_enable_all(struct tplg_state *t, struct tplg_stream_rt *rt,
			   int dir, int on)
{
	int i, ret;

	for (i = 0; i < rt->nsdw; i++) {
		if (!!rt->sdw_enabled[i] == !!on)
			continue;
		t->sdev->sdw_prep_function = rt->sdw_fn[i];
		ret = sdw_stream_enable(t->sdev, rt->sdw_link[i], dir, on);
		t->sdev->sdw_prep_function = 0;
		if (ret < 0)
			tplg_err("SoundWire link %d: stream %s failed (%d)\n",
				 rt->sdw_link[i], on ? "enable" : "disable",
				 ret);
		else
			rt->sdw_enabled[i] = on;
	}
}

static int stream_trigger(struct tplg_state *t, struct tplg_pcm_int *pc,
			  int dir, int cmd)
{
	struct tplg_stream_rt *rt = &pc->rt[dir];
	int ids[TPLG_MAX_PATH_PIPES], idx[TPLG_MAX_PATH_PIPES];
	int state, n, i, ret = 0;

	if (!rt->prepared)
		return -EINVAL;
	if (rt->chain)
		return chain_trigger(t, rt, cmd);
	switch (cmd) {
	case SND_TRIGGER_START:
	case SND_TRIGGER_RESUME:
		state = IPC4_PIPE_RUNNING;
		if (rt->state == IPC4_PIPE_RUNNING)
			return 0;
		/* leaving a pause is a release, anything else a start */
		cmd = rt->pushed ? SND_TRIGGER_RESUME : SND_TRIGGER_START;
		break;
	case SND_TRIGGER_PAUSE:
		state = IPC4_PIPE_PAUSED;
		if (rt->state != IPC4_PIPE_RUNNING)
			return 0;
		break;
	case SND_TRIGGER_STOP:
		state = IPC4_PIPE_PAUSED;
		if (rt->pushed) {
			/* stop while paused: just forget the pause */
			for (i = 0; i < rt->np; i++) {
				struct tplg_pipe *p = &t->pipes[rt->p[i]];

				if (!rt->p_acq[i])
					continue;
				if (p->paused > 0)
					p->paused--;
				if (p->started > 0)
					p->started--;
			}
			rt->pushed = 0;
			sdw_enable_all(t, rt, dir, 0);
			return 0;
		}
		if (rt->state != IPC4_PIPE_RUNNING)
			return 0;
		break;
	default:
		return -EINVAL;
	}

	n = collect_pipes(t, rt, state, ids, idx);

	if (state == IPC4_PIPE_RUNNING && cmd == SND_TRIGGER_START) {
		/* RESET -> PAUSED first, then the links, then RUNNING */
		if (n) {
			ret = ipc_pipes_set_state(t, ids, n, IPC4_PIPE_PAUSED);
			if (ret < 0) {
				tplg_err("PCM %s: pause before start failed "
					 "(%d)\n",
					 pc->pub.name, ret);
				return ret;
			}
			for (i = 0; i < n; i++)
				t->pipes[idx[i]].state = IPC4_PIPE_PAUSED;
		}
		sdw_enable_all(t, rt, dir, 1);
	}
	if (n) {
		ret = ipc_pipes_set_state(t, ids, n, state);
		if (ret < 0) {
			tplg_err("PCM %s: pipelines to %s failed (%d)\n",
				 pc->pub.name, pipe_state_name(state), ret);
			return ret;
		}
		for (i = 0; i < n; i++)
			t->pipes[idx[i]].state = state;
	}

	/* shared-pipeline counters of every pipeline on the path */
	for (i = 0; i < rt->np; i++) {
		struct tplg_pipe *p = &t->pipes[rt->p[i]];

		if (!rt->p_acq[i])
			continue;
		switch (cmd) {
		case SND_TRIGGER_START:
			p->started++;
			break;
		case SND_TRIGGER_RESUME:
			if (p->paused > 0)
				p->paused--;
			break;
		case SND_TRIGGER_PAUSE:
			p->paused++;
			break;
		case SND_TRIGGER_STOP:
			if (p->started > 0)
				p->started--;
			break;
		}
	}
	if (cmd == SND_TRIGGER_STOP)
		sdw_enable_all(t, rt, dir, 0);
	rt->pushed = cmd == SND_TRIGGER_PAUSE;
	rt->state = state;
	return 0;
}

static void stream_teardown(struct tplg_state *t, struct tplg_pcm_int *pc,
			    int dir)
{
	struct tplg_stream_rt *rt = &pc->rt[dir];
	int ids[TPLG_MAX_PATH_PIPES], idx[TPLG_MAX_PATH_PIPES];
	int i, k, n;

	if (rt->chain) {
		chain_teardown(t, rt);
		return;
	}
	/* a running or pause-pushed stream is stopped first */
	if (rt->prepared &&
	    (rt->state == IPC4_PIPE_RUNNING || rt->pushed))
		stream_trigger(t, pc, dir, SND_TRIGGER_STOP);
	rt->pushed = 0;
	sdw_enable_all(t, rt, dir, 0);

	/* pipelines nobody else runs go to RESET */
	n = collect_pipes(t, rt, IPC4_PIPE_RESET, ids, idx);
	if (n) {
		int m = 0;

		/* only those this stream is the last user of */
		for (i = 0; i < n; i++)
			if (t->pipes[idx[i]].use_count <= 1) {
				ids[m] = ids[i];
				idx[m] = idx[i];
				m++;
			}
		if (m && ipc_pipes_set_state(t, ids, m, IPC4_PIPE_RESET) == 0)
			for (i = 0; i < m; i++)
				t->pipes[idx[i]].state = IPC4_PIPE_RESET;
	}

	/* bindings this stream was the last to need */
	for (i = rt->nr - 1; i >= 0; i--) {
		struct tplg_route *r = &t->r[rt->r[i]];

		if (!rt->r_acq[i])
			continue;
		rt->r_acq[i] = 0;
		if (r->refs > 0 && --r->refs == 0)
			route_unbind(t, r);
	}

	/* modules: drop the use count; delete those whose pipeline stays */
	for (k = rt->nw - 1; k >= 0; k--) {
		struct tplg_widget *w = &t->w[rt->w[k]];
		const struct tplg_pipe *p = &t->pipes[w->pipe];

		if (!rt->w_acq[k])
			continue;
		rt->w_acq[k] = 0;
		if (w->use_count > 0 && --w->use_count == 0)
			widget_delete(t, w, p->use_count > 1);
	}

	/* pipelines, sink side first */
	for (i = rt->np - 1; i >= 0; i--) {
		struct tplg_pipe *p = &t->pipes[rt->p[i]];

		if (!rt->p_acq[i])
			continue;
		rt->p_acq[i] = 0;
		if (p->use_count > 0 && --p->use_count == 0 &&
		    p->instance_id >= 0) {
			ipc_pipe_delete(t, p);
			pipe_id_free(t, p->instance_id);
			p->instance_id = -1;
			p->state = IPC4_PIPE_UNINITIALIZED;
			p->mem_pages = 0;
			p->started = p->paused = 0;
		}
	}
	/* modules of deleted pipelines that were never instantiated, and
	 * any whose pipeline just went away, are gone in the firmware */
	for (k = 0; k < rt->nw; k++) {
		struct tplg_widget *w = &t->w[rt->w[k]];

		if (w->use_count == 0 && w->instance_id >= 0 &&
		    t->pipes[w->pipe].instance_id < 0)
			widget_delete(t, w, 0);
		if (w->use_count == 0) {
			alh_group_free(t, w);
			w->fmt_valid = 0;
		}
	}

	for (i = 0; i < rt->nsdw; i++) {
		int ret;

		t->sdev->sdw_prep_function = rt->sdw_fn[i];
		ret = sdw_stream_release(t->sdev, rt->sdw_link[i], dir);
		t->sdev->sdw_prep_function = 0;

		if (ret < 0)
			tplg_err("SoundWire link %d: release failed (%d)\n",
				 rt->sdw_link[i], ret);
	}
	rt->nsdw = 0;
	rt->prepared = 0;
	rt->state = IPC4_PIPE_UNINITIALIZED;
}

static struct tplg_pcm_int *pcm_checked(struct tplg_state *t, int idx,
					int dir)
{
	struct tplg_pcm_int *pc;

	if (!t || idx < 0 || idx >= t->npcm)
		return NULL;
	if (dir != AUDIO_DIR_PLAYBACK && dir != AUDIO_DIR_CAPTURE)
		return NULL;
	pc = &t->pcm[idx];
	if (dir == AUDIO_DIR_PLAYBACK ? !pc->pub.has_playback :
					!pc->pub.has_capture)
		return NULL;
	return pc;
}

int sof_tplg_pcm_prepare(struct sof_dev *sdev, int idx, int direction,
			 uint32_t rate, uint32_t channels, uint32_t bits,
			 int host_stream_tag)
{
	struct tplg_state *t = tplg_of(sdev);
	struct tplg_pcm_int *pc = pcm_checked(t, idx, direction);
	int ret;

	if (!pc)
		return -EINVAL;
	if (rate < 8000 || rate > 384000 || channels < 1 || channels > 8 ||
	    (bits != 8 && bits != 16 && bits != 20 && bits != 24 &&
	     bits != 32) ||
	    host_stream_tag < 1 || host_stream_tag > 64)
		return -EINVAL;
	if (!sdev->fw_booted)
		return -ENODEV;
	ret = tplg_op_begin(t);
	if (ret)
		return ret;
	if (pc->rt[direction].prepared)
		stream_teardown(t, pc, direction);
	ret = stream_prepare(t, pc, direction, rate, channels, bits,
			     host_stream_tag);
	if (ret < 0) {
		tplg_err("PCM %s %s: prepare failed (%d)\n", pc->pub.name,
			 direction == AUDIO_DIR_PLAYBACK ? "playback" :
							   "capture",
			 ret);
		/* undo whatever got set up */
		pc->rt[direction].prepared = 1;
		pc->rt[direction].state = IPC4_PIPE_RESET;
		stream_teardown(t, pc, direction);
	} else {
		tplg_dbg("PCM %s %s: prepared %u Hz %u ch %u bit, tag %d\n",
			 pc->pub.name,
			 direction == AUDIO_DIR_PLAYBACK ? "playback" :
							   "capture",
			 rate, channels, bits, host_stream_tag);
	}
	tplg_op_end(t);
	return ret;
}

int sof_tplg_pcm_trigger(struct sof_dev *sdev, int idx, int direction,
			 int cmd)
{
	struct tplg_state *t = tplg_of(sdev);
	struct tplg_pcm_int *pc = pcm_checked(t, idx, direction);
	int ret;

	if (!pc)
		return -EINVAL;
	ret = tplg_op_begin(t);
	if (ret)
		return ret;
	ret = stream_trigger(t, pc, direction, cmd);
	tplg_op_end(t);
	return ret;
}

int sof_tplg_pcm_free(struct sof_dev *sdev, int idx, int direction)
{
	struct tplg_state *t = tplg_of(sdev);
	struct tplg_pcm_int *pc = pcm_checked(t, idx, direction);
	int ret;

	if (!pc)
		return -EINVAL;
	ret = tplg_op_begin(t);
	if (ret)
		return ret;
	if (pc->rt[direction].prepared)
		stream_teardown(t, pc, direction);
	tplg_op_end(t);
	return 0;
}

int sof_tplg_set_volume(struct sof_dev *sdev, int idx, uint32_t l,
			uint32_t r)
{
	struct tplg_state *t = tplg_of(sdev);
	struct tplg_pcm_int *pc = pcm_checked(t, idx, AUDIO_DIR_PLAYBACK);
	struct tplg_stream_rt *rt;
	int ret = 0, g;

	if (!pc)
		return -EINVAL;
	if (l > 100)
		l = 100;
	if (r > 100)
		r = 100;
	ret = tplg_op_begin(t);
	if (ret)
		return ret;
	rt = &pc->rt[AUDIO_DIR_PLAYBACK];
	pc->vol_l = l;
	pc->vol_r = r;
	pc->vol_set = 1;
	if (rt->prepared) {
		g = rt->gain;
	} else {
		/* not running: is there a gain at all? (applied at init) */
		struct tplg_stream_rt *tmp = zalloc(sizeof(*tmp));

		g = -1;
		if (tmp) {
			if (path_build(t, pc, AUDIO_DIR_PLAYBACK, tmp) == 0)
				g = tmp->gain;
			kfree(tmp);
		}
	}
	if (g < 0)
		ret = -ENOENT;
	else if (rt->prepared && t->w[g].instance_id >= 0)
		ret = volume_send(t, &t->w[g], l, r);
	tplg_op_end(t);
	return ret;
}

void sof_tplg_free(struct sof_dev *sdev)
{
	struct tplg_state *t = tplg_of(sdev);
	int i, d;

	if (!t)
		return;
	if (tplg_op_begin(t) == 0) {
		for (i = 0; i < t->npcm; i++)
			for (d = 0; d < 2; d++)
				if (t->pcm[i].rt[d].prepared)
					stream_teardown(t, &t->pcm[i], d);
		tplg_op_end(t);
	}
	for (i = 0; i < t->nw; i++)
		widget_release(&t->w[i]);
	if (t->w)
		kfree(t->w);
	if (t->r)
		kfree(t->r);
	for (i = 0; i < t->nnhlt; i++)
		kfree(t->nhlt[i]);
	sdev->tplg = NULL;
	kfree(t);
}

/* ==========================================================================
 * Text dump (AUDIO_DIAG)
 * ========================================================================== */

static void dump_uuid(struct tbuf *tb, const uint8_t *u)
{
	tb_printf(tb,
		  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		  "%02x%02x%02x%02x%02x%02x",
		  u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9],
		  u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void dump_fmts(struct tbuf *tb, const char *what,
		      const struct tplg_fmt *l, uint32_t n)
{
	uint32_t i;

	if (!n)
		return;
	tb_printf(tb, "      %s:", what);
	for (i = 0; i < n && i < 12; i++)
		tb_printf(tb, " %s%u/%u/%u%s", l[i].pin_index ? "p" : "",
			  l[i].rate, l[i].channels, l[i].valid_bits,
			  l[i].sample_type == IPC4_TYPE_FLOAT ? "f" : "");
	if (n > 12)
		tb_printf(tb, " (+%u)", n - 12);
	tb_printf(tb, "\n");
}

static void dump_formats_mask(struct tbuf *tb, uint32_t f)
{
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_U8))
		tb_printf(tb, " U8");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_S16_LE))
		tb_printf(tb, " S16");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_S20_LE))
		tb_printf(tb, " S20");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_S24_LE))
		tb_printf(tb, " S24");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_S24_3LE))
		tb_printf(tb, " S24_3");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_S32_LE))
		tb_printf(tb, " S32");
	if (f & AUDIO_FMT_BIT(AUDIO_FMT_FLOAT_LE))
		tb_printf(tb, " FLOAT");
}

static void dump_rates_mask(struct tbuf *tb, uint32_t r)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(rate_map); i++)
		if (r & rate_map[i].audio_bit)
			tb_printf(tb, " %u", rate_map[i].hz);
}

long sof_tplg_dump(struct sof_dev *sdev, char *buf, size_t len)
{
	struct tplg_state *t = tplg_of(sdev);
	struct tbuf tb;
	int i, d;

	if (!buf || !len)
		return 0;
	tb.buf = buf;
	tb.len = len;
	tb.pos = 0;
	buf[0] = '\0';
	if (!t) {
		tb_printf(&tb, "topology: none loaded\n");
		return (long)tb.pos;
	}
	tb_printf(&tb, "topology: %d file(s), ABI %u.%u.%u, %d widgets, "
		       "%d routes, %d pipelines, %d PCMs, %d DAI links, "
		       "%u IPC errors\n",
		  t->nfiles, t->abi_major, t->abi_minor, t->abi_patch, t->nw,
		  t->nr, t->np, t->npcm, t->nlink, t->ipc_errors);
	for (i = 0; i < t->nfiles; i++)
		tb_printf(&tb, "  file %d: %s\n", i, t->file_name[i]);

	tb_printf(&tb, "PCMs:\n");
	for (i = 0; i < t->npcm; i++) {
		const struct tplg_pcm_int *pc = &t->pcm[i];

		tb_printf(&tb, "  [%d] id %d '%s'%s%s dai %s.%d out %u ch %u-%u",
			  i, pc->pub.id, pc->pub.name,
			  pc->pub.has_playback ? " play" : "",
			  pc->pub.has_capture ? " cap" : "",
			  dai_type_name(pc->pub.dai_type), pc->pub.dai_index,
			  pc->pub.output_type, pc->pub.channels_min,
			  pc->pub.channels_max);
		tb_printf(&tb, " fmt");
		dump_formats_mask(&tb, pc->pub.formats);
		tb_printf(&tb, " rates");
		dump_rates_mask(&tb, pc->pub.rates);
		tb_printf(&tb, "\n");
		for (d = 0; d < 2; d++) {
			const struct tplg_stream_rt *rt = &pc->rt[d];

			if (pc->host[d] >= 0)
				tb_printf(&tb, "      %s host %s",
					  d ? "capture" : "playback",
					  t->w[pc->host[d]].name);
			else
				continue;
			if (rt->prepared)
				tb_printf(&tb,
					  " [%s %u Hz %u ch %u bit tag %d, "
					  "%d modules, %d pipelines]",
					  pipe_state_name(rt->state), rt->rate,
					  rt->channels, rt->bits, rt->host_tag,
					  rt->nw, rt->np);
			tb_printf(&tb, "\n");
		}
	}

	tb_printf(&tb, "pipelines:\n");
	for (i = 0; i < t->np; i++) {
		const struct tplg_pipe *p = &t->pipes[i];
		const struct tplg_widget *s = p->sched >= 0 ? &t->w[p->sched] :
							      NULL;

		tb_printf(&tb, "  pipe %u (file %d) %s", p->index, p->file,
			  s ? s->name : "(no scheduler)");
		if (s)
			tb_printf(&tb, " prio %u core %u lp %u period %u%s%s",
				  s->priority, s->sched_core, s->lp_mode,
				  s->period, s->use_chain_dma ? " chain-dma" : "",
				  s->dynamic ? " dynamic" : "");
		if (p->instance_id >= 0)
			tb_printf(&tb, " -> instance %d %s, %u pages, users %d",
				  p->instance_id, pipe_state_name(p->state),
				  p->mem_pages, p->use_count);
		tb_printf(&tb, "\n");
	}

	tb_printf(&tb, "widgets:\n");
	for (i = 0; i < t->nw; i++) {
		const struct tplg_widget *w = &t->w[i];

		if (w->id == DAPM_SCHEDULER)
			continue;
		tb_printf(&tb, "  %-30s %-9s pipe %-3u", w->name,
			  dapm_type_name(w->id), w->pipe_index);
		if (w->sname[0])
			tb_printf(&tb, " stream '%s'", w->sname);
		if (w->has_uuid) {
			tb_printf(&tb, " uuid ");
			dump_uuid(&tb, w->uuid);
		}
		tb_printf(&tb, " core %u", w->core);
		if (w->domain == TPLG_DOMAIN_DP)
			tb_printf(&tb, " DP");
		if (w->has_node_type)
			tb_printf(&tb, " node %s(%u)", node_type_name(w->node_type),
				  w->node_type);
		if (widget_is_dai(w))
			tb_printf(&tb, " dai %s.%u",
				  w->dai_type_str[0] ? w->dai_type_str : "?",
				  w->dai_index);
		if (w->num_input_pins > 1 || w->num_output_pins > 1)
			tb_printf(&tb, " pins %u/%u", w->num_input_pins,
				  w->num_output_pins);
		if (w->deep_buffer_ms)
			tb_printf(&tb, " deep %u ms", w->deep_buffer_ms);
		if (w->id == DAPM_PGA)
			tb_printf(&tb, " ramp %u/%u ms", w->gain_ramp_type,
				  w->gain_ramp_duration);
		if (w->src_rate_out)
			tb_printf(&tb, " rate_out %u", w->src_rate_out);
		if (w->process_type[0])
			tb_printf(&tb, " process '%s'", w->process_type);
		if (w->instance_id >= 0)
			tb_printf(&tb, " => module %u inst %d node %08x users %d",
				  w->mod.module_id, w->instance_id, w->node_id,
				  w->use_count);
		tb_printf(&tb, "\n");
		dump_fmts(&tb, "in ", w->in_fmts, w->num_in_fmts);
		dump_fmts(&tb, "out", w->out_fmts, w->num_out_fmts);
		for (d = 0; d < w->nkctl; d++) {
			const struct tplg_kctl *kc = &w->kctl[d];

			tb_printf(&tb, "      control '%s' %s", kc->name,
				  kc->kind == TPLG_TYPE_MIXER ? "mixer" :
				  kc->kind == TPLG_TYPE_ENUM  ? "enum" :
								"bytes");
			if (kc->kind == TPLG_TYPE_MIXER)
				tb_printf(&tb, " 0..%u", kc->max);
			if (kc->has_db)
				tb_printf(&tb, " %d dB/100 step %u%s", kc->db_min,
					  kc->db_step, kc->db_mute ? " mute" : "");
			if (kc->bytes)
				tb_printf(&tb, " %u bytes", kc->bytes_len);
			tb_printf(&tb, "\n");
		}
	}

	tb_printf(&tb, "routes:\n");
	for (i = 0; i < t->nr; i++) {
		const struct tplg_route *r = &t->r[i];

		tb_printf(&tb, "  %s -> %s", t->w[r->src].name,
			  t->w[r->sink].name);
		if (r->control[0])
			tb_printf(&tb, " (%s)", r->control);
		if (r->refs)
			tb_printf(&tb, " bound %d:%d", r->src_q, r->dst_q);
		tb_printf(&tb, "\n");
	}

	tb_printf(&tb, "DAI links:\n");
	for (i = 0; i < t->nlink; i++) {
		const struct tplg_link *l = &t->link[i];

		tb_printf(&tb, "  id %u '%s' %s", l->id, l->name,
			  dai_type_name(l->dai_type));
		if (l->alh_rate || l->alh_ch)
			tb_printf(&tb, " alh %u Hz %u ch", l->alh_rate,
				  l->alh_ch);
		if (l->hda_rate || l->hda_ch)
			tb_printf(&tb, " hda %u Hz %u ch", l->hda_rate, l->hda_ch);
		if (l->num_hw_configs)
			tb_printf(&tb, " hw %u Hz %u slots x %u", l->hw_rate,
				  l->hw_channels, l->hw_slot_width);
		tb_printf(&tb, "\n");
	}
	return (long)tb.pos;
}
