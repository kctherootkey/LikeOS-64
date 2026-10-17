// LikeOS SoundWire Device Class for Audio (SDCA) - generic codec support,
// plus the Maxim MAX98373 amplifier.
//
// SDCA codecs describe themselves: every audio function (jack codec,
// smart amplifier, microphone array, ...) is a child of the peripheral's
// ACPI node with a MIPI DisCo _DSD listing its entities - input and output
// terminals, feature units (mute, volume), clock sources (sample rate),
// power domains, group entities (jack detection) - their controls and how
// the entities connect.  A control is a register at an address built from
// function, entity, control selector and channel; values wider than a byte
// go through the "multi-byte quantity" shadow registers.
//
// This driver reads those descriptions and drives the functions in the
// generic way the class defines: it runs the function initialisation
// tables from the firmware, powers the power domains of a function while
// it streams, programs the clock sources' sample rate index, opens and
// sets the feature units on the paths to the speaker and headphone
// terminals, and follows the group entity's detected mode for the jack.
// For the common Realtek parts (RT711-SDCA, RT712, RT713, RT721, RT722,
// RT1316, RT1318, RT1320) a profile names the entities and data ports
// directly, so they work even when the firmware's description is thin,
// and the RT72x vendor presets run before the generic set-up.
//
// Copyright (C) 2026 The LikeOS Project

#include "sdw.h"
#include <kernel/mm/memory.h>
#include <kernel/ke/syscall.h>

/* ==========================================================================
 * SDCA constants
 * ========================================================================== */

#define FT_SMART_AMP 0x01
#define FT_SIMPLE_AMP 0x02
#define FT_SMART_MIC 0x03
#define FT_SIMPLE_MIC 0x04
#define FT_SPEAKER_MIC 0x05
#define FT_UAJ 0x06
#define FT_RJ 0x07
#define FT_SIMPLE_JACK 0x08
#define FT_HID 0x0a
#define FT_COMPANION_AMP 0x0b

#define ET_ENTITY0 0x00
#define ET_IT 0x02
#define ET_OT 0x03
#define ET_MU 0x05
#define ET_SU 0x06
#define ET_FU 0x07
#define ET_XU 0x0a
#define ET_CS 0x0b
#define ET_CX 0x0c
#define ET_PDE 0x11
#define ET_GE 0x12
#define ET_SPE 0x13

#define CTL_FU_MUTE 0x01
#define CTL_FU_VOLUME 0x02
#define CTL_CS_SAMPLERATEINDEX 0x10
#define CTL_PDE_REQUESTED_PS 0x01
#define CTL_PDE_ACTUAL_PS 0x10
#define CTL_GE_SELECTED_MODE 0x01
#define CTL_GE_DETECTED_MODE 0x02
#define CTL_E0_FUNCTION_STATUS 0x10
#define CTL_E0_FUNCTION_ACTION 0x11
#define E0_NEEDS_INIT 0x20
#define E0_STATUS_W1C 0xff

#define PS0 0
#define PS3 3

#define DET_UNPLUGGED 0
#define DET_UNKNOWN 1
#define DET_IN_PROGRESS 2

#define TT_GENERIC 0x101
#define TT_SPEAKER_FIRST 0x380
#define TT_SPEAKER_LAST 0x3a3
#define TT_LINEOUT_FIRST 0x690
#define TT_LINEOUT_LAST 0x694
#define TT_STEREO_JACK_FIRST 0x6b0
#define TT_STEREO_JACK_LAST 0x6b4
#define TT_HEADPHONE_JACK 0x6c0
#define TT_HEADSET_JACK 0x6d0
#define TT_MIC_TRANSDUCER 0x201
#define TT_MIC_ARRAY 0x205
#define TT_MIC_JACK 0x6a0

#define SDCA_MAX_ENTITIES 64
#define SDCA_MAX_INPUTS 8
#define SDCA_MAX_INIT 1024 /* writes in a function initialisation table */
#define SDCA_MAX_RANGE 16

/* ==========================================================================
 * Model
 * ========================================================================== */

struct sdca_entity {
	uint8_t id;
	uint8_t type;
	uint16_t term_type;
	uint64_t controls; /* bit n: control selector n */
	char label[16];
	char path[40]; /* the node it was read from: links name it */
	int ninputs;
	uint8_t inputs[SDCA_MAX_INPUTS];
	char in_path[SDCA_MAX_INPUTS][40];
	/* FU volume range, Q7.8 dB */
	int16_t vol_min, vol_max, vol_step;
	int has_range;
	uint64_t cn_list; /* channels of the volume/mute controls */
	/* CS: (index, rate) pairs */
	uint32_t sr[SDCA_MAX_RANGE][2];
	int nsr;
	/* GE: (mode, terminal type) pairs */
	uint32_t modes[SDCA_MAX_RANGE][2];
	int nmodes;
};

struct sdca_function {
	uint8_t addr; /* function number 1..7 */
	uint8_t type; /* FT_* */
	int nent;
	struct sdca_entity ent[SDCA_MAX_ENTITIES];
	uint8_t *init; /* 5-byte entries: le32 address, value */
	int ninit;
	/* derived */
	uint8_t play_fu[2], cap_fu; /* [0] speaker, [1] headphone path */
	uint8_t ge;
	int powered;
};

/* A known part: entity numbers and data ports. */
struct sdca_profile {
	uint16_t mfg, part;
	const char *name;
	uint8_t fn_jack, fn_amp, fn_mic;
	uint8_t hp_fu, spk_fu, mic_fu, dmic_fu;
	uint8_t hp_pde, spk_pde, mic_pde, dmic_pde;
	uint8_t cs_play, cs_cap, cs_amp, cs_dmic;
	uint8_t ge;
	uint8_t port_hp, port_mic, port_spk, port_dmic;
	int16_t vol_min, vol_max; /* Q7.8 dB of the playback FUs */
	int vendor; /* SDCA_VENDOR_* preset */
};

#define SDCA_VENDOR_NONE 0
#define SDCA_VENDOR_RT722 1
#define SDCA_VENDOR_RT712 2

#define Q78(db100) ((int16_t)(((db100) * 256) / 100))

static const struct sdca_profile g_profiles[] = {
	{ 0x025d, 0x0722, "rt722", 1, 4, 2, 0x05, 0x06, 0x0f, 0x1e, 0x40, 0x23,
	  0x12, 0x2a, 0x01, 0x11, 0x31, 0x1f, 0x49, 1, 2, 3, 6, Q78(-6525), 0,
	  SDCA_VENDOR_RT722 },
	{ 0x025d, 0x0721, "rt721", 1, 4, 2, 0x05, 0x06, 0x0f, 0x1e, 0x40, 0x23,
	  0x12, 0x2a, 0x01, 0x11, 0x31, 0x1f, 0x49, 1, 2, 3, 6, Q78(-6525), 0,
	  SDCA_VENDOR_NONE },
	{ 0x025d, 0x0712, "rt712", 1, 4, 2, 0x05, 0x06, 0x0f, 0x1e, 0x40, 0x23,
	  0x12, 0x11, 0x01, 0x11, 0x31, 0x1f, 0x49, 1, 4, 3, 8, Q78(-6525), 0,
	  SDCA_VENDOR_RT712 },
	{ 0x025d, 0x0713, "rt713", 1, 0, 2, 0x05, 0, 0x0f, 0x1e, 0x40, 0, 0x12,
	  0x11, 0x01, 0x11, 0, 0x1f, 0x49, 1, 4, 0, 8, Q78(-6525), 0,
	  SDCA_VENDOR_RT712 },
	{ 0x025d, 0x0711, "rt711-sdca", 1, 0, 2, 0x05, 0, 0x0f, 0x1e, 0x28, 0,
	  0x29, 0x2a, 0x01, 0x11, 0, 0x1f, 0x49, 3, 2, 0, 4, Q78(-6525), 0,
	  SDCA_VENDOR_NONE },
	{ 0x025d, 0x1316, "rt1316", 0, 4, 0, 0, 0x03, 0, 0, 0, 0x31, 0, 0, 0, 0,
	  0x21, 0, 0, 0, 0, 1, 0, Q78(-6525), 0, SDCA_VENDOR_NONE },
	{ 0x025d, 0x1318, "rt1318", 0, 4, 0, 0, 0x03, 0, 0, 0, 0x31, 0, 0, 0, 0,
	  0x21, 0, 0, 0, 0, 1, 0, Q78(-6525), 0, SDCA_VENDOR_NONE },
	{ 0x025d, 0x1320, "rt1320", 0, 4, 2, 0, 0x03, 0, 0x30, 0, 0x33, 0, 0x2a,
	  0, 0, 0x21, 0x12, 0, 0, 0, 1, 8, Q78(-6525), 0, SDCA_VENDOR_NONE },
	{ 0x025d, 0x1321, "rt1320", 0, 4, 2, 0, 0x03, 0, 0x30, 0, 0x33, 0, 0x2a,
	  0, 0, 0x21, 0x12, 0, 0, 0, 1, 8, Q78(-6525), 0, SDCA_VENDOR_NONE },
};

struct sdca_dev {
	struct sdw_slave *s;
	struct snd_mutex lock;
	const struct sdca_profile *prof;
	uint32_t if_rev;
	int nfn;
	struct sdca_function *fn[SDW_MAX_FUNCTIONS];
	int booted;
	int first_init_done;

	/* which functions play the roles (function numbers, 0 = none) */
	uint8_t f_jack, f_amp, f_mic;

	uint32_t play_ports, cap_ports;
	uint32_t rate;

	int spk_on, hp_on;
	uint32_t spk_vol[2], hp_vol[2], cap_vol, master_vol;
	int spk_mute, hp_mute, cap_mute, master_mute;

	/* jack */
	int jack_present;
	uint32_t jack_report;
	uint32_t det_mode;
	volatile int jack_work_queued;
};

static inline struct sdca_dev *sd_of(struct sdw_slave *s)
{
	return (struct sdca_dev *)s->codec;
}

/* ==========================================================================
 * Control access
 * ========================================================================== */

static int ctl_wr(struct sdca_dev *d, int fn, int ent, int ctl, int ch,
		  uint8_t v)
{
	return sdw_write(d->s, SDW_SDCA_CTL(fn, ent, ctl, ch), v);
}

static int ctl_rd(struct sdca_dev *d, int fn, int ent, int ctl, int ch)
{
	return sdw_read(d->s, SDW_SDCA_CTL(fn, ent, ctl, ch));
}

static int ctl_wr16(struct sdca_dev *d, int fn, int ent, int ctl, int ch,
		    uint16_t v)
{
	return sdw_mbq_write(d->s, SDW_SDCA_CTL(fn, ent, ctl, ch), 2, v);
}

/* Realtek private registers: 16-bit values at (nid << 20 | reg). */
static int rt_index_wr(struct sdca_dev *d, uint32_t nid, uint32_t reg,
		       uint16_t v)
{
	return sdw_mbq_write(d->s, (nid << 20) | reg, 2, v);
}

static int rt_index_rd(struct sdca_dev *d, uint32_t nid, uint32_t reg,
		       uint32_t *v)
{
	return sdw_mbq_read(d->s, (nid << 20) | reg, 2, v);
}

static int rt_index_upd(struct sdca_dev *d, uint32_t nid, uint32_t reg,
			uint16_t mask, uint16_t val)
{
	uint32_t v = 0;
	int rc = rt_index_rd(d, nid, reg, &v);

	if (rc)
		return rc;
	return rt_index_wr(d, nid, reg, (uint16_t)((v & ~mask) | (val & mask)));
}

/* ==========================================================================
 * DisCo parsing
 * ========================================================================== */

static struct sdca_function *fn_by_addr(struct sdca_dev *d, int addr)
{
	for (int i = 0; i < d->nfn; i++)
		if (d->fn[i]->addr == addr)
			return d->fn[i];
	return NULL;
}

static struct sdca_entity *ent_by_id(struct sdca_function *f, int id)
{
	if (!f)
		return NULL;
	for (int i = 0; i < f->nent; i++)
		if (f->ent[i].id == id)
			return &f->ent[i];
	return NULL;
}

/* "mipi-sdca-control-range": u16 cols, u16 rows, then u32 cells */
static int read_range(struct sdw_dsd *c, uint32_t cells[][2], int max,
		      int want_cols, uint32_t *flat, int flat_max)
{
	uint8_t raw[4 + 4 * 2 * SDCA_MAX_RANGE + 4 * 3];
	int n, total, cols, rows, k = 0;

	n = sdw_dsd_u8_array(c, "mipi-sdca-control-range", raw, sizeof(raw),
			     &total);
	if (n < 4)
		return -ENOENT;
	cols = raw[0] | (raw[1] << 8);
	rows = raw[2] | (raw[3] << 8);
	if (cols <= 0 || rows <= 0 || cols > 8 || rows > 64)
		return -EINVAL;
	if (total != 4 + 4 * cols * rows)
		return -EINVAL;
	if (flat) {
		for (int i = 0; i < cols * rows && i < flat_max &&
				4 + 4 * (i + 1) <= n;
		     i++) {
			const uint8_t *p = raw + 4 + 4 * i;
			flat[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
				  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		}
		return cols * rows;
	}
	if (cols != want_cols)
		return -EINVAL;
	for (int r = 0; r < rows && k < max; r++) {
		const uint8_t *p = raw + 4 + 4 * cols * r;
		if (4 + 4 * cols * (r + 1) > n)
			break;
		cells[k][0] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
			      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		cells[k][1] = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
			      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
		k++;
	}
	return k;
}

static void parse_entity(struct sdw_dsd *fd, struct sdw_dsd *e,
			 struct sdca_entity *en)
{
	uint32_t v;
	uint64_t list;
	char name[48];

	if (!sdw_dsd_u32(e, "mipi-sdca-entity-type", &v))
		en->type = (uint8_t)v;
	sdw_dsd_string(e, "mipi-sdca-entity-label", en->label,
		       sizeof(en->label));
	if (!sdw_dsd_u32(e, "mipi-sdca-terminal-type", &v))
		en->term_type = (uint16_t)v;
	if (!sdw_dsd_u64(e, "mipi-sdca-control-list", &list))
		en->controls = list;
	(void)fd;

	/* inputs: names of the connected entities' nodes */
	if (!sdw_dsd_u64(e, "mipi-sdca-input-pin-list", &list)) {
		for (int pin = 1; pin < 64 && en->ninputs < SDCA_MAX_INPUTS;
		     pin++) {
			if (!(list & (1ull << pin)))
				continue;
			ksnprintf(name, sizeof(name), "mipi-sdca-input-pin-%d",
				  pin);
			if (!sdw_dsd_link_target(e, name,
						 en->in_path[en->ninputs],
						 sizeof(en->in_path[0])))
				en->ninputs++;
		}
	}

	if (en->type == ET_FU && (en->controls & (1ull << CTL_FU_VOLUME))) {
		struct sdw_dsd *c = sdw_dsd_child(
			e, "mipi-sdca-control-0x2-subproperties");
		if (c) {
			uint32_t flat[3];
			if (read_range(c, NULL, 0, 0, flat, 3) >= 3) {
				en->vol_min = (int16_t)flat[0];
				en->vol_max = (int16_t)flat[1];
				en->vol_step = (int16_t)flat[2];
				if (en->vol_max > en->vol_min)
					en->has_range = 1;
			}
			sdw_dsd_u64(c, "mipi-sdca-control-cn-list",
				    &en->cn_list);
			sdw_dsd_close(c);
		}
	}
	if (en->type == ET_CS) {
		struct sdw_dsd *c = sdw_dsd_child(
			e, "mipi-sdca-control-0x10-subproperties");
		if (c) {
			int k = read_range(c, en->sr, SDCA_MAX_RANGE, 2, NULL, 0);
			en->nsr = k > 0 ? k : 0;
			sdw_dsd_close(c);
		}
	}
	if (en->type == ET_GE) {
		struct sdw_dsd *c = sdw_dsd_child(
			e, "mipi-sdca-control-0x1-subproperties");
		if (c) {
			int k = read_range(c, en->modes, SDCA_MAX_RANGE, 2, NULL,
					   0);
			en->nmodes = k > 0 ? k : 0;
			sdw_dsd_close(c);
		}
	}
}

static int is_play_terminal(uint16_t tt)
{
	return (tt >= TT_SPEAKER_FIRST && tt <= TT_SPEAKER_LAST) ||
	       (tt >= TT_LINEOUT_FIRST && tt <= TT_LINEOUT_LAST) ||
	       (tt >= TT_STEREO_JACK_FIRST && tt <= TT_STEREO_JACK_LAST) ||
	       tt == TT_HEADPHONE_JACK || tt == TT_HEADSET_JACK;
}

static int is_speaker_terminal(uint16_t tt)
{
	return tt >= TT_SPEAKER_FIRST && tt <= TT_SPEAKER_LAST;
}

/* Walk upstream from `start' to the first feature unit with a volume. */
static int first_fu_upstream(struct sdca_function *f, struct sdca_entity *start)
{
	struct sdca_entity *cur = start;

	for (int depth = 0; cur && depth < 16; depth++) {
		if (cur != start && cur->type == ET_FU &&
		    (cur->controls & ((1ull << CTL_FU_VOLUME) |
				      (1ull << CTL_FU_MUTE))))
			return cur->id;
		if (!cur->ninputs)
			break;
		cur = ent_by_id(f, cur->inputs[0]);
	}
	return 0;
}

static void derive_paths(struct sdca_function *f)
{
	/* resolve input names to entity ids */
	for (int i = 0; i < f->nent; i++) {
		struct sdca_entity *e = &f->ent[i];
		int n = 0;
		for (int k = 0; k < e->ninputs; k++) {
			for (int j = 0; j < f->nent; j++) {
				if (f->ent[j].path[0] &&
				    !kstrcmp(f->ent[j].path, e->in_path[k])) {
					e->inputs[n++] = f->ent[j].id;
					break;
				}
			}
		}
		e->ninputs = n;
	}
	for (int i = 0; i < f->nent; i++) {
		struct sdca_entity *e = &f->ent[i];
		int fu;

		if (e->type == ET_GE && !f->ge)
			f->ge = e->id;
		if (e->type != ET_OT)
			continue;
		fu = first_fu_upstream(f, e);
		if (!fu)
			continue;
		if (is_play_terminal(e->term_type)) {
			int idx = is_speaker_terminal(e->term_type) ? 0 : 1;
			if (!f->play_fu[idx])
				f->play_fu[idx] = (uint8_t)fu;
		} else if (!f->cap_fu) {
			f->cap_fu = (uint8_t)fu;
		}
	}
}

static struct sdca_function *parse_function(void *h, int addr,
					    uint32_t if_rev)
{
	struct sdw_dsd *fd = sdw_dsd_open(h);
	struct sdca_function *f;
	uint32_t ids[SDCA_MAX_ENTITIES];
	int t, n, total;

	t = sdw_sdca_function_type(h, if_rev);
	if (t <= 0 || !fd) {
		sdw_dsd_close(fd);
		return NULL;
	}
	f = kcalloc(1, sizeof(*f));
	if (!f) {
		sdw_dsd_close(fd);
		return NULL;
	}
	f->addr = (uint8_t)addr;
	f->type = (uint8_t)t;

	n = sdw_dsd_u32_array(fd, "mipi-sdca-entity-id-list", ids,
			      SDCA_MAX_ENTITIES, &total);
	for (int i = 0; i < n && f->nent < SDCA_MAX_ENTITIES; i++) {
		char name[48];
		struct sdw_dsd *e;
		struct sdca_entity *en;

		if (ids[i] > 0x7f)
			continue;
		ksnprintf(name, sizeof(name),
			  "mipi-sdca-entity-id-0x%X-subproperties", ids[i]);
		e = sdw_dsd_child(fd, name);
		if (!e)
			continue;
		en = &f->ent[f->nent++];
		en->id = (uint8_t)ids[i];
		sdw_dsd_link_target(fd, name, en->path, sizeof(en->path));
		parse_entity(fd, e, en);
		sdw_dsd_close(e);
	}

	/* the initialisation table: 5-byte entries */
	{
		uint8_t probe;
		int tot = 0;
		sdw_dsd_u8_array(fd, "mipi-sdca-function-initialization-table",
				 &probe, 1, &tot);
		if (tot >= 5 && tot % 5 == 0 && tot / 5 <= SDCA_MAX_INIT) {
			f->init = kalloc((size_t)tot);
			if (f->init &&
			    sdw_dsd_u8_array(fd,
					     "mipi-sdca-function-initialization-table",
					     f->init, tot, NULL) == tot)
				f->ninit = tot / 5;
			else if (f->init) {
				kfree(f->init);
				f->init = NULL;
			}
		}
	}
	sdw_dsd_close(fd);
	derive_paths(f);
	return f;
}

static void parse_functions(struct sdca_dev *d)
{
	void *h = sof_acpi_periph_handle(d->s->link->sdev, d->s->periph_idx);
	void *kids[SDW_MAX_FUNCTIONS];
	uint64_t adr[SDW_MAX_FUNCTIONS];
	struct sdw_dsd *pd;
	int n;

	if (!h)
		return;
	pd = sdw_dsd_open(h);
	if (pd) {
		sdw_dsd_u32(pd, "mipi-sdw-sdca-interface-revision", &d->if_rev);
		sdw_dsd_close(pd);
	}
	n = sdw_acpi_children(h, kids, adr, SDW_MAX_FUNCTIONS);
	for (int i = 0; i < n && d->nfn < SDW_MAX_FUNCTIONS; i++) {
		struct sdca_function *f;
		if (!adr[i] || adr[i] > 7)
			continue;
		f = parse_function(kids[i], (int)adr[i], d->if_rev);
		if (f)
			d->fn[d->nfn++] = f;
	}
}

static void free_functions(struct sdca_dev *d)
{
	for (int i = 0; i < d->nfn; i++) {
		if (d->fn[i]) {
			kfree(d->fn[i]->init);
			kfree(d->fn[i]);
		}
		d->fn[i] = NULL;
	}
	d->nfn = 0;
}

static void assign_roles(struct sdca_dev *d)
{
	const struct sdca_profile *p = d->prof;

	if (p) {
		d->f_jack = p->fn_jack;
		d->f_amp = p->fn_amp;
		d->f_mic = p->fn_mic;
	}
	for (int i = 0; i < d->nfn; i++) {
		struct sdca_function *f = d->fn[i];
		switch (f->type) {
		case FT_UAJ:
		case FT_RJ:
		case FT_SIMPLE_JACK:
			if (!d->f_jack)
				d->f_jack = f->addr;
			break;
		case FT_SMART_AMP:
		case FT_SIMPLE_AMP:
		case FT_COMPANION_AMP:
			if (!d->f_amp)
				d->f_amp = f->addr;
			break;
		case FT_SMART_MIC:
		case FT_SIMPLE_MIC:
		case FT_SPEAKER_MIC:
			if (!d->f_mic)
				d->f_mic = f->addr;
			break;
		default:
			break;
		}
	}
}

/* entity numbers, from the profile or the parsed graph */
static int hp_fu(struct sdca_dev *d)
{
	struct sdca_function *f = fn_by_addr(d, d->f_jack);

	if (d->prof && d->prof->hp_fu)
		return d->prof->hp_fu;
	if (f)
		return f->play_fu[1] ? f->play_fu[1] : f->play_fu[0];
	return 0;
}

static int spk_fu(struct sdca_dev *d)
{
	struct sdca_function *f = fn_by_addr(d, d->f_amp);

	if (d->prof && d->prof->spk_fu)
		return d->prof->spk_fu;
	if (f)
		return f->play_fu[0] ? f->play_fu[0] : f->play_fu[1];
	return 0;
}

static int cap_fu(struct sdca_dev *d, int dmic)
{
	struct sdca_function *f = fn_by_addr(d, dmic ? d->f_mic : d->f_jack);

	if (d->prof) {
		int e = dmic ? d->prof->dmic_fu : d->prof->mic_fu;
		if (e)
			return e;
	}
	return f ? f->cap_fu : 0;
}

/* ==========================================================================
 * Realtek presets
 * ========================================================================== */

#define RT_VENDOR_REG 0x20
#define RT_VENDOR_CALI 0x58
#define RT_VENDOR_HDA_CTL 0x61
#define RT_JD_PRODUCT_NUM 0x00
#define RT_ANALOG_BIAS_CTL3 0x04
#define RT_JD_CTRL1 0x09
#define RT_LDO1_CTL 0x1a
#define RT_CLSD_CTRL6 0x3c
#define RT_COMBO_JACK_AUTO_CTL1 0x45
#define RT_DIGITAL_MISC_CTRL4 0x4a
#define RT_VREFO_GAT 0x63
#define RT_FSM_CTL 0x67
#define RT_DAC_DC_CALI_CTL3 0x03
#define RT_HDA_LEGACY_CONFIG_CTL0 0x06
#define RT_HDA_LEGACY_RESET_CTL 0x08
#define RT_DMIC_ENT_FLOAT_CTL 0x10
#define RT_DMIC_GAIN_ENT_FLOAT_CTL0 0x11
#define RT_DMIC_GAIN_ENT_FLOAT_CTL2 0x13
#define RT_ADC_ENT_FLOAT_CTL 0x15
#define RT_ADC_VOL_CH_FLOAT_CTL 0x17
#define RT_DAC03_HP_PDE_FLOAT_CTL 0x22
#define RT_MIC2_LINE2_PDE_FLOAT_CTL 0x23
#define RT_ET41_LINE2_PDE_FLOAT_CTL 0x24
#define RT_ADC0A_08_PDE_FLOAT_CTL 0x25
#define RT_ADC10_PDE_FLOAT_CTL 0x26
#define RT_DMIC1_2_PDE_FLOAT_CTL 0x28
#define RT_AMP_PDE_FLOAT_CTL 0x29
#define RT_GE_RELATED_CTL2 0x46
#define RT_EAPD_CTL 0x55
#define RT_UMP_HID_CTL0 0x60
#define RT_UMP_HID_CTL4 0x64
#define RT_UMP_HID_CTL5 0x65
#define RT_UMP_HID_CTL7 0x67
#define RT_FLOAT_CTRL_1 0x70
#define RT_ENT_FLOAT_CTRL_1 0x76
#define RT_HIDDEN_REG_SW_RESET 0x4000
#define RT_DC_CALIB_CTRL 0x10000
#define RT_CTL_VENDOR_DEF 0x30

static int fn_needs_init(struct sdca_dev *d, int fn)
{
	int v = ctl_rd(d, fn, 0, CTL_E0_FUNCTION_STATUS, 0);

	return v < 0 || (v & E0_NEEDS_INIT) || !d->first_init_done;
}

static void fn_clear_needs_init(struct sdca_dev *d, int fn)
{
	ctl_wr(d, fn, 0, CTL_E0_FUNCTION_STATUS, 0, E0_NEEDS_INIT);
}

static void rt72x_presets(struct sdca_dev *d)
{
	uint32_t vid = 0;
	int vb;

	rt_index_rd(d, RT_VENDOR_REG, RT_JD_PRODUCT_NUM, &vid);
	vb = ((vid & 0x0f00) >> 8) == 1;
	if (!d->first_init_done) {
		rt_index_upd(d, RT_VENDOR_REG, RT_LDO1_CTL,
			     RT_HIDDEN_REG_SW_RESET, RT_HIDDEN_REG_SW_RESET);
		rt_index_upd(d, RT_VENDOR_HDA_CTL, RT_HDA_LEGACY_RESET_CTL, 1, 1);
		if (!vb)
			rt_index_wr(d, RT_VENDOR_REG, RT_LDO1_CTL, 0xb091);
	}

	/* microphone array function */
	if (fn_needs_init(d, 2)) {
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ADC0A_08_PDE_FLOAT_CTL,
			    0x2a29);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ADC10_PDE_FLOAT_CTL, 0x2a00);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_DMIC1_2_PDE_FLOAT_CTL,
			    0x2a2a);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_DMIC_ENT_FLOAT_CTL, 0x2626);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ADC_ENT_FLOAT_CTL, 0x1e00);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_DMIC_GAIN_ENT_FLOAT_CTL0,
			    0x1515);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ADC_VOL_CH_FLOAT_CTL, 0x0304);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_DMIC_GAIN_ENT_FLOAT_CTL2,
			    0x0304);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_HDA_LEGACY_CONFIG_CTL0,
			    0x0000);
		ctl_wr(d, 2, 0x26, RT_CTL_VENDOR_DEF, 0, 0x01);
		sdw_write(d->s, 0x2f5c, 0x25);
		sdw_write(d->s, 0x2f03, 0x06);
		if (vb)
			sdw_write(d->s, 0x2f52, 0x00);
		fn_clear_needs_init(d, 2);
	}

	/* amplifier function */
	if (d->prof && d->prof->fn_amp && fn_needs_init(d, 4)) {
		rt_index_wr(d, RT_VENDOR_REG, RT_CLSD_CTRL6, 0xc215);
		rt_index_wr(d, RT_VENDOR_CALI, RT_DC_CALIB_CTRL, 0x702c);
		rt_index_wr(d, RT_VENDOR_CALI, RT_DC_CALIB_CTRL, 0xf02d);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_AMP_PDE_FLOAT_CTL, 0x2323);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_EAPD_CTL, 0x0002);
		ctl_wr(d, 4, 0x42, RT_CTL_VENDOR_DEF, 0x08, 0x04);
		if (vb)
			sdw_write(d->s, 0x2f54, 0x00);
		fn_clear_needs_init(d, 4);
	}

	/* jack function: bias, detection, DC calibration */
	if (fn_needs_init(d, 1)) {
		uint32_t cal = 0;
		rt_index_wr(d, RT_VENDOR_REG, RT_ANALOG_BIAS_CTL3, 0xa081);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_GE_RELATED_CTL2, 0xa009);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_UMP_HID_CTL4, 0xcf00);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_UMP_HID_CTL5, 0x000f);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_UMP_HID_CTL0, 0x1100);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_UMP_HID_CTL7, 0x0c12);
		rt_index_wr(d, RT_VENDOR_REG, RT_JD_CTRL1, 0x7002);
		rt_index_wr(d, RT_VENDOR_REG, RT_CLSD_CTRL6, 0xc215);
		rt_index_wr(d, RT_VENDOR_REG, RT_FSM_CTL, 0x4100);
		rt_index_wr(d, RT_VENDOR_CALI, RT_DAC_DC_CALI_CTL3, 0x008d);
		for (int i = 0; i < 100; i++) {
			sdw_msleep(10);
			if (!rt_index_rd(d, RT_VENDOR_CALI, RT_DAC_DC_CALI_CTL3,
					 &cal) &&
			    !(cal & 0x0040))
				break;
		}
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ADC0A_08_PDE_FLOAT_CTL,
			    0x2a12);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_MIC2_LINE2_PDE_FLOAT_CTL,
			    0x3429);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ET41_LINE2_PDE_FLOAT_CTL,
			    0x4112);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_DAC03_HP_PDE_FLOAT_CTL,
			    0x4040);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_ENT_FLOAT_CTRL_1, 0x4141);
		rt_index_wr(d, RT_VENDOR_HDA_CTL, RT_FLOAT_CTRL_1, 0x0101);
		sdw_write(d->s, 0x2f58, 0x07);
		sdw_write(d->s, 0x2f03, 0x06);
		rt_index_upd(d, RT_VENDOR_REG, RT_COMBO_JACK_AUTO_CTL1, 0x0200,
			     0x0200);
		rt_index_upd(d, RT_VENDOR_REG, RT_VREFO_GAT, 0x4000, 0x4000);
		rt_index_wr(d, RT_VENDOR_REG, RT_DIGITAL_MISC_CTRL4, 0x0010);
		if (vb)
			sdw_write(d->s, 0x2f51, 0x00);
		fn_clear_needs_init(d, 1);
	}
}

/* ==========================================================================
 * Generic function control
 * ========================================================================== */

static void run_init_tables(struct sdca_dev *d)
{
	for (int i = 0; i < d->nfn; i++) {
		struct sdca_function *f = d->fn[i];
		if (!f->ninit || !fn_needs_init(d, f->addr))
			continue;
		for (int k = 0; k < f->ninit; k++) {
			const uint8_t *e = f->init + 5 * k;
			uint32_t a = (uint32_t)e[0] | ((uint32_t)e[1] << 8) |
				     ((uint32_t)e[2] << 16) |
				     ((uint32_t)e[3] << 24);
			if (a >= SDW_R_MAX)
				continue;
			sdw_write(d->s, a, e[4]);
		}
		fn_clear_needs_init(d, f->addr);
	}
}

static int pde_list(struct sdca_dev *d, int fn, int dmic, uint8_t *out,
		    int max)
{
	struct sdca_function *f = fn_by_addr(d, fn);
	int n = 0;

	if (d->prof) {
		int e = 0;
		if (fn == d->f_amp && fn)
			e = d->prof->spk_pde;
		else if (fn == d->f_mic && fn && dmic)
			e = d->prof->dmic_pde;
		else if (fn == d->f_jack)
			e = 0; /* jack: hp and mic PDEs below */
		if (e && n < max)
			out[n++] = (uint8_t)e;
		if (fn == d->f_jack && fn) {
			if (d->prof->hp_pde && n < max)
				out[n++] = d->prof->hp_pde;
			if (d->prof->mic_pde && n < max)
				out[n++] = d->prof->mic_pde;
		}
		if (n)
			return n;
	}
	if (!f)
		return 0;
	for (int i = 0; i < f->nent && n < max; i++)
		if (f->ent[i].type == ET_PDE)
			out[n++] = f->ent[i].id;
	return n;
}

static void fn_power(struct sdca_dev *d, int fn, int dmic, int on)
{
	uint8_t pdes[8];
	int n = pde_list(d, fn, dmic, pdes, 8);
	uint8_t ps = on ? PS0 : PS3;

	for (int i = 0; i < n; i++)
		ctl_wr(d, fn, pdes[i], CTL_PDE_REQUESTED_PS, 0, ps);
	/* wait for the actual state (bounded) */
	for (int i = 0; i < n; i++) {
		for (int t = 0; t < 300; t++) {
			int v = ctl_rd(d, fn, pdes[i], CTL_PDE_ACTUAL_PS, 0);
			if (v < 0 || v == ps)
				break;
			sdw_msleep(1);
		}
	}
}

static int rate_index(struct sdca_entity *cs, uint32_t rate)
{
	if (cs) {
		for (int i = 0; i < cs->nsr; i++)
			if (cs->sr[i][1] == rate)
				return (int)cs->sr[i][0];
	}
	/* the index table the class codecs share */
	switch (rate) {
	case 16000:
		return 0x04;
	case 32000:
		return 0x07;
	case 44100:
		return 0x08;
	case 48000:
		return 0x09;
	case 96000:
		return 0x0b;
	case 192000:
		return 0x0d;
	default:
		return -EINVAL;
	}
}

static void fn_set_rate(struct sdca_dev *d, int fn, uint32_t rate,
			const uint8_t *cs_ids, int ncs)
{
	struct sdca_function *f = fn_by_addr(d, fn);

	if (ncs) {
		for (int i = 0; i < ncs; i++) {
			int idx;
			if (!cs_ids[i])
				continue;
			idx = rate_index(ent_by_id(f, cs_ids[i]), rate);
			if (idx >= 0)
				ctl_wr(d, fn, cs_ids[i], CTL_CS_SAMPLERATEINDEX, 0,
				       (uint8_t)idx);
		}
		return;
	}
	if (!f)
		return;
	for (int i = 0; i < f->nent; i++) {
		struct sdca_entity *e = &f->ent[i];
		int idx;
		if (e->type != ET_CS)
			continue;
		idx = rate_index(e, rate);
		if (idx >= 0)
			ctl_wr(d, fn, e->id, CTL_CS_SAMPLERATEINDEX, 0,
			       (uint8_t)idx);
	}
}

/* FU volume: 0..100 onto the unit's range, Q7.8 dB */
static void fu_set(struct sdca_dev *d, int fn, int fu, uint32_t vl,
		   uint32_t vr, int mute)
{
	struct sdca_entity *e = ent_by_id(fn_by_addr(d, fn), fu);
	int min = d->prof ? d->prof->vol_min : Q78(-6525);
	int max = d->prof ? d->prof->vol_max : 0;
	int step = 0;
	uint32_t vols[2] = { vl, vr };

	if (!fn || !fu)
		return;
	if (e && e->has_range) {
		min = e->vol_min;
		max = e->vol_max;
		step = e->vol_step;
		if (max > 0)
			max = 0; /* no digital gain above 0 dB */
	}
	for (int ch = 1; ch <= 2; ch++) {
		uint32_t v = vols[ch - 1] > 100 ? 100 : vols[ch - 1];
		int q = min + (int)((max - min) * (int)v / 100);
		if (step > 0)
			q = min + ((q - min) / step) * step;
		ctl_wr16(d, fn, fu, CTL_FU_VOLUME, ch, (uint16_t)(int16_t)q);
		ctl_wr(d, fn, fu, CTL_FU_MUTE, ch, (mute || !v) ? 1 : 0);
	}
}

static int port_kind(struct sdca_dev *d, int dir, int port)
{
	const struct sdca_profile *p = d->prof;

	if (dir == AUDIO_DIR_PLAYBACK) {
		if (p && p->port_spk && port == p->port_spk)
			return 1; /* speaker */
		return 0; /* headphone/jack */
	}
	if (p && p->port_dmic && port == p->port_dmic)
		return 3; /* dmic */
	return 2; /* jack microphone */
}

static void apply_outputs(struct sdca_dev *d)
{
	int playing_hp = 0, playing_spk = 0;
	uint32_t mv = d->master_mute ? 0 : d->master_vol;

	for (int p = 1; p < SDW_MAX_PORTS; p++) {
		if (!(d->play_ports & (1u << p)))
			continue;
		if (port_kind(d, AUDIO_DIR_PLAYBACK, p) == 1)
			playing_spk = 1;
		else
			playing_hp = 1;
	}
	/* a codec with one playback path drives both outputs from it */
	if (d->f_amp && spk_fu(d) && !d->prof)
		playing_spk |= playing_hp;
	if (d->f_jack && hp_fu(d))
		fu_set(d, d->f_jack, hp_fu(d), d->hp_vol[0] * mv / 100,
		       d->hp_vol[1] * mv / 100,
		       !playing_hp || !d->hp_on || d->hp_mute);
	if (d->f_amp && spk_fu(d))
		fu_set(d, d->f_amp, spk_fu(d), d->spk_vol[0] * mv / 100,
		       d->spk_vol[1] * mv / 100,
		       !(playing_spk || playing_hp) || !d->spk_on ||
			       d->spk_mute);
}

static void apply_capture(struct sdca_dev *d)
{
	int jack_cap = 0;

	for (int p = 1; p < SDW_MAX_PORTS; p++)
		if ((d->cap_ports & (1u << p)) &&
		    port_kind(d, AUDIO_DIR_CAPTURE, p) == 2)
			jack_cap = 1;
	if (d->f_jack && cap_fu(d, 0))
		fu_set(d, d->f_jack, cap_fu(d, 0), d->cap_vol, d->cap_vol,
		       d->cap_mute || !jack_cap);
	if (d->f_mic && cap_fu(d, 1))
		fu_set(d, d->f_mic, cap_fu(d, 1), d->cap_vol, d->cap_vol,
		       d->cap_mute || !(d->cap_ports));
}

/* ==========================================================================
 * Jack (group entity)
 * ========================================================================== */

static uint32_t mode_to_jack(struct sdca_dev *d, uint32_t mode)
{
	struct sdca_entity *ge =
		ent_by_id(fn_by_addr(d, d->f_jack),
			  d->prof && d->prof->ge ? d->prof->ge :
						   (fn_by_addr(d, d->f_jack) ?
							    fn_by_addr(d, d->f_jack)->ge :
							    0));

	if (mode <= DET_IN_PROGRESS)
		return 0;
	if (ge) {
		for (int i = 0; i < ge->nmodes; i++) {
			uint32_t tt;
			if (ge->modes[i][0] != mode)
				continue;
			tt = ge->modes[i][1];
			if (tt == TT_HEADSET_JACK)
				return SDW_JACK_HEADPHONE | SDW_JACK_MIC;
			if (tt >= TT_LINEOUT_FIRST && tt <= TT_LINEOUT_LAST)
				return SDW_JACK_LINEOUT;
			if (tt == TT_MIC_JACK)
				return SDW_JACK_MIC;
			return SDW_JACK_HEADPHONE;
		}
	}
	/* conventional numbering: 3 headphone, 4 line, 5 headset */
	switch (mode) {
	case 4:
		return SDW_JACK_LINEOUT;
	case 5:
		return SDW_JACK_HEADPHONE | SDW_JACK_MIC;
	default:
		return SDW_JACK_HEADPHONE;
	}
}

static int jack_ge(struct sdca_dev *d)
{
	struct sdca_function *f = fn_by_addr(d, d->f_jack);

	if (d->prof && d->prof->ge)
		return d->prof->ge;
	return f ? f->ge : 0;
}

static void sd_jack_update(struct sdca_dev *d)
{
	int ge = jack_ge(d);
	int det;
	uint32_t report;

	if (!d->f_jack || !ge)
		return;
	if (snd_mutex_lock(&d->lock, 0))
		return;
	det = ctl_rd(d, d->f_jack, ge, CTL_GE_DETECTED_MODE, 0);
	if (det < 0 || det == DET_IN_PROGRESS || det == DET_UNKNOWN ||
	    (uint32_t)det == d->det_mode) {
		snd_mutex_unlock(&d->lock);
		return;
	}
	d->det_mode = (uint32_t)det;
	/* acknowledge the mode: the function configures itself for it */
	ctl_wr(d, d->f_jack, ge, CTL_GE_SELECTED_MODE, 0, (uint8_t)det);
	report = mode_to_jack(d, (uint32_t)det);
	d->jack_present = report != 0;
	d->jack_report = report;
	apply_outputs(d);
	snd_mutex_unlock(&d->lock);
	sdw_report_jack(d->s, report | (report ? SDW_JACK_MECHANICAL : 0),
			SDW_JACK_HEADPHONE | SDW_JACK_MIC | SDW_JACK_LINEOUT |
				SDW_JACK_MECHANICAL);
}

static void sd_jack_work(void *arg)
{
	struct sdca_dev *d = arg;

	__atomic_store_n(&d->jack_work_queued, 0, __ATOMIC_RELEASE);
	if (d->s->link->ctx->stopping || !d->s->codec || !d->booted)
		return;
	sd_jack_update(d);
}

static void sd_queue_jack(struct sdca_dev *d)
{
	if (__atomic_exchange_n(&d->jack_work_queued, 1, __ATOMIC_ACQ_REL))
		return;
	if (snd_schedule_work(sd_jack_work, d, 0))
		__atomic_store_n(&d->jack_work_queued, 0, __ATOMIC_RELEASE);
}

/* ==========================================================================
 * Codec operations
 * ========================================================================== */

static const struct sdca_profile *find_profile(const struct sof_sdw_peripheral *p)
{
	for (unsigned i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]);
	     i++)
		if (g_profiles[i].mfg == p->mfg_id &&
		    g_profiles[i].part == p->part_id)
			return &g_profiles[i];
	return NULL;
}

static int sd_match(const struct sof_sdw_peripheral *p)
{
	/* the rt711 without SDCA (class 0, version < 3) is not ours */
	if (p->mfg_id == 0x025d && p->part_id == 0x0711 && !p->class_id)
		return 0;
	return find_profile(p) || p->class_id || p->functions;
}

static int sd_probe(struct sdw_slave *s)
{
	struct sdca_dev *d = kcalloc(1, sizeof(*d));

	if (!d)
		return -ENOMEM;
	d->s = s;
	snd_mutex_init(&d->lock, "sdca");
	d->prof = find_profile(s->p);
	d->spk_vol[0] = d->spk_vol[1] = 100;
	d->hp_vol[0] = d->hp_vol[1] = 80;
	d->cap_vol = 80;
	d->master_vol = 100;
	d->spk_on = 1;
	s->codec = d;
	parse_functions(d);
	assign_roles(d);

	s->props.paging = 1;
	s->props.scp_int1_mask |= SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY;
	s->caps = 0;
	if (d->f_jack) {
		s->caps |= SDW_CAP_HEADPHONE | SDW_CAP_PLAYBACK | SDW_CAP_CAPTURE |
			   SDW_CAP_HEADSET_MIC;
		if (jack_ge(d))
			s->caps |= SDW_CAP_JACK_DETECT;
	}
	if (d->f_amp)
		s->caps |= SDW_CAP_SPEAKER | SDW_CAP_PLAYBACK;
	if (d->f_mic)
		s->caps |= SDW_CAP_DMIC | SDW_CAP_CAPTURE;
	/* a profile's ports always exist, whatever the firmware lists */
	if (d->prof) {
		uint8_t pl[2] = { d->prof->port_hp, d->prof->port_spk };
		uint8_t cp[2] = { d->prof->port_mic, d->prof->port_dmic };
		for (int i = 0; i < 2; i++) {
			if (pl[i] && pl[i] < SDW_MAX_PORTS) {
				s->props.sink_ports |= 1u << pl[i];
				s->props.dp[pl[i]].valid = 1;
				s->props.dp[pl[i]].source = 0;
			}
			if (cp[i] && cp[i] < SDW_MAX_PORTS) {
				s->props.source_ports |= 1u << cp[i];
				s->props.dp[cp[i]].valid = 1;
				s->props.dp[cp[i]].source = 1;
			}
		}
	}
	return 0;
}

static int sd_init(struct sdw_slave *s)
{
	struct sdca_dev *d = sd_of(s);

	if (!d)
		return -ENODEV;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	d->booted = 0;
	if (d->prof && (d->prof->vendor == SDCA_VENDOR_RT722 ||
			d->prof->vendor == SDCA_VENDOR_RT712))
		rt72x_presets(d);
	run_init_tables(d);
	/* everything off until a stream needs it */
	if (d->f_amp)
		fn_power(d, d->f_amp, 0, 0);
	if (d->f_mic)
		fn_power(d, d->f_mic, 1, 0);
	d->det_mode = 0xff;
	d->first_init_done = 1;
	d->booted = 1;
	apply_outputs(d);
	snd_mutex_unlock(&d->lock);
	sdw_info("%s: SDCA %s, functions jack %u amp %u mic %u (%d described)\n",
		 s->name, d->prof ? d->prof->name : "generic", d->f_jack,
		 d->f_amp, d->f_mic, d->nfn);
	if (jack_ge(d))
		sd_queue_jack(d);
	return 0;
}

static void sd_detached(struct sdw_slave *s)
{
	struct sdca_dev *d = sd_of(s);

	if (d) {
		for (int i = 0; i < d->nfn; i++)
			d->fn[i]->powered = 0;
	}
}

static void sd_remove(struct sdw_slave *s)
{
	struct sdca_dev *d = sd_of(s);

	if (!d)
		return;
	if (d->jack_work_queued)
		return; /* the work still holds it: keep the memory */
	free_functions(d);
	s->codec = NULL;
	kfree(d);
}

static int sd_port_for(struct sdw_slave *s, int dir, int fn)
{
	struct sdca_dev *d = sd_of(s);
	const struct sdca_profile *p = d ? d->prof : NULL;

	if (p) {
		if (dir == AUDIO_DIR_PLAYBACK) {
			if (fn == SOF_SDW_FN_AMP && p->port_spk)
				return p->port_spk;
			if (fn == SOF_SDW_FN_JACK && p->port_hp)
				return p->port_hp;
			return p->port_hp ? p->port_hp : p->port_spk;
		}
		if (fn == SOF_SDW_FN_MIC && p->port_dmic)
			return p->port_dmic;
		if (fn == SOF_SDW_FN_JACK && p->port_mic)
			return p->port_mic;
		if (fn == SOF_SDW_FN_AMP)
			return -ENOENT; /* feedback is not a capture stream */
		return p->port_dmic ? p->port_dmic : p->port_mic;
	}
	/* generic: the lowest port of the right direction */
	{
		uint32_t ports = dir == AUDIO_DIR_PLAYBACK ?
					 s->props.sink_ports :
					 s->props.source_ports;
		for (int i = 1; i < SDW_MAX_PORTS; i++)
			if (ports & (1u << i))
				return i;
	}
	return -ENOENT;
}

static int sd_prepare(struct sdw_slave *s, int dir, int port, uint32_t rate,
		      uint32_t ch, uint32_t bits)
{
	struct sdca_dev *d = sd_of(s);
	const struct sdca_profile *p;
	int kind;

	(void)ch;
	(void)bits;
	if (!d || !d->booted)
		return -ENODEV;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	p = d->prof;
	kind = port_kind(d, dir, port);
	d->rate = rate;
	switch (kind) {
	case 1: /* speaker */
		if (d->f_amp) {
			uint8_t cs = p ? p->cs_amp : 0;
			fn_set_rate(d, d->f_amp, rate, &cs, p ? 1 : 0);
			fn_power(d, d->f_amp, 0, 1);
		}
		d->play_ports |= 1u << port;
		break;
	case 0: /* jack playback (or the only playback) */
		if (d->f_jack) {
			uint8_t cs = p ? p->cs_play : 0;
			fn_set_rate(d, d->f_jack, rate, &cs, p ? 1 : 0);
			fn_power(d, d->f_jack, 0, 1);
		} else if (d->f_amp) {
			uint8_t cs = p ? p->cs_amp : 0;
			fn_set_rate(d, d->f_amp, rate, &cs, p ? 1 : 0);
			fn_power(d, d->f_amp, 0, 1);
		}
		d->play_ports |= 1u << port;
		break;
	case 3: /* digital microphones */
		if (d->f_mic) {
			uint8_t cs = p ? p->cs_dmic : 0;
			fn_set_rate(d, d->f_mic, rate, &cs, p ? 1 : 0);
			fn_power(d, d->f_mic, 1, 1);
		}
		d->cap_ports |= 1u << port;
		break;
	default: /* jack microphone */
		if (d->f_jack) {
			uint8_t cs = p ? p->cs_cap : 0;
			fn_set_rate(d, d->f_jack, rate, &cs, p ? 1 : 0);
			fn_power(d, d->f_jack, 0, 1);
		}
		d->cap_ports |= 1u << port;
		break;
	}
	apply_outputs(d);
	apply_capture(d);
	snd_mutex_unlock(&d->lock);
	return 0;
}

static int sd_release(struct sdw_slave *s, int dir, int port)
{
	struct sdca_dev *d = sd_of(s);
	int kind;

	if (!d)
		return 0;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	kind = port_kind(d, dir, port);
	if (dir == AUDIO_DIR_PLAYBACK)
		d->play_ports &= ~(1u << port);
	else
		d->cap_ports &= ~(1u << port);
	if (d->booted && s->status != SDW_SLAVE_UNATTACHED) {
		apply_outputs(d);
		apply_capture(d);
		if (kind == 1 && d->f_amp)
			fn_power(d, d->f_amp, 0, 0);
		if (kind == 3 && d->f_mic)
			fn_power(d, d->f_mic, 1, 0);
		/* the jack function keeps power for detection */
	}
	snd_mutex_unlock(&d->lock);
	return 0;
}

static void sd_alert(struct sdw_slave *s, uint8_t int1, const uint8_t *sdca)
{
	struct sdca_dev *d = sd_of(s);

	(void)int1;
	if (!d || !d->booted)
		return;
	if (sdca[0] | sdca[1] | sdca[2] | sdca[3])
		sd_queue_jack(d);
}

static void sd_poll(struct sdw_slave *s)
{
	struct sdca_dev *d = sd_of(s);

	if (d && d->booted && jack_ge(d))
		sd_queue_jack(d);
}

static int sd_set_routing(struct sdw_slave *s, int spk, int hp)
{
	struct sdca_dev *d = sd_of(s);

	if (!d || !d->booted)
		return -ENODEV;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	d->spk_on = spk;
	d->hp_on = hp;
	apply_outputs(d);
	snd_mutex_unlock(&d->lock);
	return 0;
}

static int sd_set_volume(struct sdw_slave *s, uint32_t ctl, uint32_t l,
			 uint32_t r)
{
	struct sdca_dev *d = sd_of(s);
	int rc = 0;

	if (!d || !d->booted)
		return -ENODEV;
	if (l > 100)
		l = 100;
	if (r > 100)
		r = 100;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		d->master_vol = l > r ? l : r;
		break;
	case AUDIO_CTL_SPEAKER:
		if (!d->f_amp)
			rc = -ENOENT;
		d->spk_vol[0] = l;
		d->spk_vol[1] = r;
		break;
	case AUDIO_CTL_HEADPHONE:
		if (!d->f_jack)
			rc = -ENOENT;
		d->hp_vol[0] = l;
		d->hp_vol[1] = r;
		break;
	case AUDIO_CTL_CAPTURE:
		d->cap_vol = (l + r) / 2;
		apply_capture(d);
		snd_mutex_unlock(&d->lock);
		return 0;
	default:
		rc = -ENOENT;
		break;
	}
	if (!rc)
		apply_outputs(d);
	snd_mutex_unlock(&d->lock);
	return rc;
}

static int sd_get_volume(struct sdw_slave *s, uint32_t ctl,
			 struct audio_volume *v)
{
	struct sdca_dev *d = sd_of(s);
	int min = Q78(-6525), max = 0;

	if (!d || !v)
		return -ENODEV;
	if (d->prof) {
		min = d->prof->vol_min;
		max = d->prof->vol_max;
	}
	switch (ctl) {
	case AUDIO_CTL_SPEAKER:
		if (!d->f_amp)
			return -ENOENT;
		v->left = d->spk_vol[0];
		v->right = d->spk_vol[1];
		break;
	case AUDIO_CTL_HEADPHONE:
		if (!d->f_jack)
			return -ENOENT;
		v->left = d->hp_vol[0];
		v->right = d->hp_vol[1];
		break;
	case AUDIO_CTL_CAPTURE:
		v->left = v->right = d->cap_vol;
		break;
	default:
		return -ENOENT;
	}
	/* hundredths of a dB from Q7.8 */
	v->db_min = min * 100 / 256;
	v->db_max = max * 100 / 256;
	v->db_left = v->db_min + (v->db_max - v->db_min) * (int)v->left / 100;
	v->db_right = v->db_min + (v->db_max - v->db_min) * (int)v->right / 100;
	v->steps = 101;
	return 0;
}

static int sd_set_mute(struct sdw_slave *s, uint32_t ctl, uint32_t mute)
{
	struct sdca_dev *d = sd_of(s);

	if (!d || !d->booted)
		return -ENODEV;
	if (snd_mutex_lock(&d->lock, 0))
		return -EINTR;
	switch (ctl) {
	case AUDIO_CTL_MASTER:
	case AUDIO_CTL_PCM:
		d->master_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_SPEAKER:
		d->spk_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_HEADPHONE:
		d->hp_mute = mute ? 1 : 0;
		break;
	case AUDIO_CTL_CAPTURE:
		d->cap_mute = mute ? 1 : 0;
		apply_capture(d);
		snd_mutex_unlock(&d->lock);
		return 0;
	default:
		snd_mutex_unlock(&d->lock);
		return -ENOENT;
	}
	apply_outputs(d);
	snd_mutex_unlock(&d->lock);
	return 0;
}

static const char *ft_name(int t)
{
	switch (t) {
	case FT_SMART_AMP:
		return "SmartAmp";
	case FT_SIMPLE_AMP:
		return "SimpleAmp";
	case FT_SMART_MIC:
		return "SmartMic";
	case FT_SIMPLE_MIC:
		return "SimpleMic";
	case FT_SPEAKER_MIC:
		return "SpeakerMic";
	case FT_UAJ:
		return "UAJ";
	case FT_RJ:
		return "RJ";
	case FT_SIMPLE_JACK:
		return "SimpleJack";
	case FT_HID:
		return "HID";
	case FT_COMPANION_AMP:
		return "CompanionAmp";
	default:
		return "other";
	}
}

static long sd_dump(struct sdw_slave *s, char *buf, size_t len)
{
	struct sdca_dev *d = sd_of(s);
	size_t pos = 0;

	if (!d)
		return 0;
	pos = sdw_dump_printf(buf, len, pos,
			      "    sdca %s rev 0x%x: jack fn %u (FU 0x%x GE 0x%x) amp fn %u (FU 0x%x) mic fn %u, ports play 0x%x cap 0x%x, jack 0x%x mode %u\n",
			      d->prof ? d->prof->name : "generic", d->if_rev,
			      d->f_jack, hp_fu(d), jack_ge(d), d->f_amp,
			      spk_fu(d), d->f_mic, d->play_ports, d->cap_ports,
			      d->jack_report, d->det_mode);
	for (int i = 0; i < d->nfn; i++) {
		struct sdca_function *f = d->fn[i];
		pos = sdw_dump_printf(buf, len, pos,
				      "      function %u %s: %d entities, %d init writes, play FU 0x%x/0x%x capture FU 0x%x\n",
				      f->addr, ft_name(f->type), f->nent,
				      f->ninit, f->play_fu[0], f->play_fu[1],
				      f->cap_fu);
	}
	return (long)pos;
}

const struct sdw_codec_ops sdw_sdca_ops = {
	.name = "sdca",
	.match = sd_match,
	.probe = sd_probe,
	.init = sd_init,
	.detached = sd_detached,
	.remove = sd_remove,
	.port_for = sd_port_for,
	.prepare = sd_prepare,
	.release = sd_release,
	.alert = sd_alert,
	.poll = sd_poll,
	.set_routing = sd_set_routing,
	.set_volume = sd_set_volume,
	.get_volume = sd_get_volume,
	.set_mute = sd_set_mute,
	.dump = sd_dump,
};

/* ==========================================================================
 * Maxim MAX98373 smart amplifier (not SDCA: its own register map)
 * ========================================================================== */

#define MX_SW_RESET 0x2000
#define MX_PCM_TX_HIZ_EN_1 0x2020
#define MX_PCM_TX_HIZ_EN_2 0x2021
#define MX_PCM_TX_SRC_1 0x2022
#define MX_PCM_TX_SRC_2 0x2023
#define MX_PCM_DATA_FMT_CFG 0x2024
#define MX_AUDIO_IF_MODE 0x2025
#define MX_PCM_SR_SETUP_2 0x2028
#define MX_PCM_TO_SPK_MONO_MIX_1 0x2029
#define MX_PCM_TO_SPK_MONO_MIX_2 0x202A
#define MX_SOUNDWIRE_CTRL 0x2036
#define MX_AMP_DIG_VOL_CTRL 0x203D
#define MX_AMP_DSP_CFG 0x203F
#define MX_AMP_EN 0x2043
#define MX_IV_SENSE_ADC_DSP_CFG 0x2046
#define MX_IV_SENSE_ADC_EN 0x2047
#define MX_BDE_EN 0x20B5
#define MX_LIMITER_EN 0x20E2
#define MX_GLOBAL_SHDN 0x20FF
#define MX_CHANSZ_MASK 0xC0
#define MX_SR_MASK 0xF0
#define MX_IVADC_SR_MASK 0x0F

struct max98373 {
	struct sdw_slave *s;
	struct snd_mutex lock;
	int booted;
	uint32_t vol, master_vol;
	int mute, master_mute, spk_on, playing;
};

static int mx_match(const struct sof_sdw_peripheral *p)
{
	return p->mfg_id == 0x019f && p->part_id == 0x8373;
}

static int mx_probe(struct sdw_slave *s)
{
	struct max98373 *m = kcalloc(1, sizeof(*m));

	if (!m)
		return -ENOMEM;
	m->s = s;
	snd_mutex_init(&m->lock, "max98373");
	m->vol = 100;
	m->master_vol = 100;
	m->spk_on = 1;
	s->codec = m;
	s->caps = SDW_CAP_SPEAKER | SDW_CAP_PLAYBACK;
	s->props.paging = 1;
	s->props.sink_ports |= 1u << 1;
	s->props.source_ports |= 1u << 3;
	s->props.dp[1].valid = 1;
	s->props.dp[1].max_ch = 2;
	s->props.dp[3].valid = 1;
	s->props.dp[3].source = 1;
	return 0;
}

static void mx_apply(struct max98373 *m)
{
	uint32_t v = m->vol * m->master_vol / 100;
	int mute = m->mute || m->master_mute || !m->spk_on || !m->playing || !v;
	/* attenuation register: 0 = 0 dB, 0x7f = -63.5 dB */
	uint8_t att = (uint8_t)((100 - (v > 100 ? 100 : v)) * 0x7f / 100);

	sdw_write(m->s, MX_AMP_DIG_VOL_CTRL, att);
	sdw_write(m->s, MX_GLOBAL_SHDN, mute ? 0 : 1);
}

static int mx_init(struct sdw_slave *s)
{
	struct max98373 *m = s->codec;

	if (!m)
		return -ENODEV;
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	sdw_write(s, MX_SW_RESET, 1);
	sdw_msleep(10);
	sdw_write(s, MX_AUDIO_IF_MODE, 3); /* SoundWire */
	sdw_write(s, MX_IV_SENSE_ADC_EN, 3);
	sdw_write(s, MX_SOUNDWIRE_CTRL, 5);
	sdw_write(s, MX_PCM_SR_SETUP_2, 0x88); /* 48 kHz speaker and IV */
	sdw_write(s, MX_PCM_TX_HIZ_EN_1, 0xff);
	sdw_write(s, MX_PCM_TX_HIZ_EN_2, 0xff);
	sdw_write(s, MX_PCM_TO_SPK_MONO_MIX_1, 0x80);
	sdw_write(s, MX_PCM_TO_SPK_MONO_MIX_2, 0x1);
	sdw_write(s, MX_AMP_DSP_CFG, 0x3);
	sdw_write(s, MX_IV_SENSE_ADC_DSP_CFG, 0x7);
	/* current on slot 1, voltage on slot 0, feedback slots driven */
	sdw_write(s, MX_PCM_TX_SRC_1, (1 << 4) | 0);
	sdw_update(s, MX_PCM_TX_HIZ_EN_1, 0x3, 0);
	sdw_write(s, MX_PCM_TX_SRC_2, 2);
	sdw_update(s, MX_AMP_EN, 0x1, 0x1);
	sdw_write(s, MX_BDE_EN, 1);
	sdw_write(s, MX_LIMITER_EN, 1);
	m->booted = 1;
	mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static void mx_remove(struct sdw_slave *s)
{
	if (s->codec) {
		kfree(s->codec);
		s->codec = NULL;
	}
}

static int mx_port_for(struct sdw_slave *s, int dir, int fn)
{
	(void)s;
	(void)fn;
	return dir == AUDIO_DIR_PLAYBACK ? 1 : -ENOENT;
}

static int mx_prepare(struct sdw_slave *s, int dir, int port, uint32_t rate,
		      uint32_t ch, uint32_t bits)
{
	struct max98373 *m = s->codec;
	int sr;
	uint8_t chsz;

	(void)port;
	(void)ch;
	if (!m || !m->booted || dir != AUDIO_DIR_PLAYBACK)
		return -ENODEV;
	switch (rate) {
	case 8000:
		sr = 0x0;
		break;
	case 11025:
		sr = 0x1;
		break;
	case 12000:
		sr = 0x2;
		break;
	case 16000:
		sr = 0x3;
		break;
	case 22050:
		sr = 0x4;
		break;
	case 24000:
		sr = 0x5;
		break;
	case 32000:
		sr = 0x6;
		break;
	case 44100:
		sr = 0x7;
		break;
	case 48000:
		sr = 0x8;
		break;
	case 88200:
		sr = 0x9;
		break;
	case 96000:
		sr = 0xa;
		break;
	default:
		return -EINVAL;
	}
	chsz = bits <= 16 ? 0x40 : (bits <= 24 ? 0x80 : 0xC0);
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	sdw_update(s, MX_PCM_DATA_FMT_CFG, MX_CHANSZ_MASK, chsz);
	sdw_update(s, MX_PCM_SR_SETUP_2, MX_SR_MASK, (uint8_t)(sr << 4));
	sdw_update(s, MX_PCM_SR_SETUP_2, MX_IVADC_SR_MASK, (uint8_t)sr);
	m->playing = 1;
	mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static int mx_release(struct sdw_slave *s, int dir, int port)
{
	struct max98373 *m = s->codec;

	(void)dir;
	(void)port;
	if (!m)
		return 0;
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	m->playing = 0;
	if (m->booted && s->status != SDW_SLAVE_UNATTACHED)
		mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static int mx_set_routing(struct sdw_slave *s, int spk, int hp)
{
	struct max98373 *m = s->codec;

	(void)hp;
	if (!m || !m->booted)
		return -ENODEV;
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	m->spk_on = spk;
	mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static int mx_set_volume(struct sdw_slave *s, uint32_t ctl, uint32_t l,
			 uint32_t r)
{
	struct max98373 *m = s->codec;
	uint32_t v = l > r ? l : r;

	if (!m || !m->booted)
		return -ENODEV;
	if (v > 100)
		v = 100;
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	if (ctl == AUDIO_CTL_MASTER || ctl == AUDIO_CTL_PCM)
		m->master_vol = v;
	else if (ctl == AUDIO_CTL_SPEAKER)
		m->vol = v;
	else {
		snd_mutex_unlock(&m->lock);
		return -ENOENT;
	}
	mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static int mx_get_volume(struct sdw_slave *s, uint32_t ctl,
			 struct audio_volume *v)
{
	struct max98373 *m = s->codec;

	if (!m || ctl != AUDIO_CTL_SPEAKER)
		return -ENOENT;
	v->left = v->right = m->vol;
	v->db_left = v->db_right = -(int32_t)((100 - m->vol) * 0x7f / 100) * 50;
	v->db_min = -6350;
	v->db_max = 0;
	v->steps = 0x80;
	return 0;
}

static int mx_set_mute(struct sdw_slave *s, uint32_t ctl, uint32_t mute)
{
	struct max98373 *m = s->codec;

	if (!m || !m->booted)
		return -ENODEV;
	if (snd_mutex_lock(&m->lock, 0))
		return -EINTR;
	if (ctl == AUDIO_CTL_MASTER || ctl == AUDIO_CTL_PCM)
		m->master_mute = mute ? 1 : 0;
	else if (ctl == AUDIO_CTL_SPEAKER)
		m->mute = mute ? 1 : 0;
	else {
		snd_mutex_unlock(&m->lock);
		return -ENOENT;
	}
	mx_apply(m);
	snd_mutex_unlock(&m->lock);
	return 0;
}

static long mx_dump(struct sdw_slave *s, char *buf, size_t len)
{
	struct max98373 *m = s->codec;

	if (!m)
		return 0;
	return (long)sdw_dump_printf(buf, len, 0,
				     "    max98373: %s, volume %u, %s%s\n",
				     m->booted ? "ready" : "not ready", m->vol,
				     m->playing ? "playing" : "idle",
				     m->mute || m->master_mute ? ", muted" : "");
}

const struct sdw_codec_ops sdw_max98373_ops = {
	.name = "max98373",
	.match = mx_match,
	.probe = mx_probe,
	.init = mx_init,
	.remove = mx_remove,
	.port_for = mx_port_for,
	.prepare = mx_prepare,
	.release = mx_release,
	.set_routing = mx_set_routing,
	.set_volume = mx_set_volume,
	.get_volume = mx_get_volume,
	.set_mute = mx_set_mute,
	.dump = mx_dump,
};
