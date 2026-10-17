// LikeOS AC'97 codec layer.
//
// Everything about an AC'97 codec that does not depend on the controller
// carrying the link: waiting for it to answer, telling an audio codec from
// a modem, identification (vendor and chip names), the register reset and
// the powerdown-register handshake, per-vendor and per-machine fixups,
// variable rate audio, and the analog mixer behind the sound core's
// controls (master, speaker/line out, headphone, PCM, capture gain, mic
// boost, output routing, record source).
//
// Register access goes through the controller's bus ops.  The codec keeps a
// shadow of every register it wrote, so read-modify-write sequences do not
// depend on registers that read back differently from what was written,
// and so the whole state can be replayed after a link reset.
//
// Locking: c->lock (interrupts off) is held around every register access
// made after init; codec register cycles are a few AC-link frames (tens of
// microseconds) each.  Init runs single-threaded from the probe path and
// may busy-wait for hundreds of milliseconds, so it does not take the lock.
//
// Copyright (C) 2026 The LikeOS Project

#include "ac97_codec.h"
#include <kernel/ke/syscall.h>
#include <kernel/io/console.h>

#define DRV "ac97"

/* ---- identification tables ------------------------------------------------ */

struct ac97_name {
	uint32_t id;
	uint32_t mask;
	const char *name;
};

/* Vendor ids are three ASCII letters in the top 24 bits (or, for a few
 * early parts, a PCI-style number). */
static const struct ac97_name ac97_vendors[] = {
	{ 0x41445300, 0xffffff00, "Analog Devices" },
	{ 0x414b4d00, 0xffffff00, "Asahi Kasei" },
	{ 0x414c4300, 0xffffff00, "Realtek" },
	{ 0x414c4700, 0xffffff00, "Realtek" },
	{ 0x415a5400, 0xffffff00, "Aztech" },
	{ 0x434d4900, 0xffffff00, "C-Media" },
	{ 0x43525900, 0xffffff00, "Cirrus Logic" },
	{ 0x43585400, 0xffffff00, "Conexant" },
	{ 0x44543000, 0xffffff00, "Diamond Technology" },
	{ 0x454d4300, 0xffffff00, "eMicro" },
	{ 0x45838300, 0xffffff00, "ESS Technology" },
	{ 0x48525300, 0xffffff00, "Intersil" },
	{ 0x49434500, 0xffffff00, "ICEnsemble" },
	{ 0x49544500, 0xffffff00, "ITE" },
	{ 0x4e534300, 0xffffff00, "National Semiconductor" },
	{ 0x50534300, 0xffffff00, "Philips" },
	{ 0x53494c00, 0xffffff00, "Silicon Laboratories" },
	{ 0x53544d00, 0xffffff00, "STMicroelectronics" },
	{ 0x54524100, 0xffffff00, "TriTech" },
	{ 0x54584e00, 0xffffff00, "Texas Instruments" },
	{ 0x56494100, 0xffffff00, "VIA" },
	{ 0x57454300, 0xffffff00, "Winbond" },
	{ 0x574d4c00, 0xffffff00, "Wolfson" },
	{ 0x594d4800, 0xffffff00, "Yamaha" },
	{ 0x83847600, 0xffffff00, "SigmaTel" },
	{ 0, 0, 0 }
};

/* Chips, most specific mask first within a vendor. */
static const struct ac97_name ac97_chips[] = {
	/* Analog Devices SoundMAX */
	{ 0x41445303, 0xffffffff, "AD1819" },
	{ 0x41445340, 0xffffffff, "AD1881" },
	{ 0x41445348, 0xffffffff, "AD1881A" },
	{ 0x41445360, 0xffffffff, "AD1885" },
	{ 0x41445361, 0xffffffff, "AD1886" },
	{ 0x41445362, 0xffffffff, "AD1887" },
	{ 0x41445363, 0xffffffff, "AD1886A" },
	{ 0x41445368, 0xffffffff, "AD1888" },
	{ 0x41445370, 0xffffffff, "AD1980" },
	{ 0x41445372, 0xffffffff, "AD1981A" },
	{ 0x41445374, 0xffffffff, "AD1981B" },
	{ 0x41445375, 0xffffffff, "AD1985" },
	{ 0x41445378, 0xffffffff, "AD1986" },
	/* Asahi Kasei */
	{ 0x414b4d00, 0xffffffff, "AK4540" },
	{ 0x414b4d01, 0xffffffff, "AK4542" },
	{ 0x414b4d02, 0xffffffff, "AK4543" },
	{ 0x414b4d06, 0xffffffff, "AK4544A" },
	{ 0x414b4d07, 0xffffffff, "AK4545" },
	/* Realtek / Avance Logic */
	{ 0x414c4300, 0xffffff00, "ALC100" },
	{ 0x414c4710, 0xfffffff0, "ALC200" },
	{ 0x414c4721, 0xffffffff, "ALC650D" },
	{ 0x414c4722, 0xffffffff, "ALC650E" },
	{ 0x414c4723, 0xffffffff, "ALC650F" },
	{ 0x414c4720, 0xfffffff0, "ALC650" },
	{ 0x414c4730, 0xffffffff, "ALC101" },
	{ 0x414c4740, 0xfffffff0, "ALC202" },
	{ 0x414c4750, 0xfffffff0, "ALC250" },
	{ 0x414c4760, 0xfffffff0, "ALC655" },
	{ 0x414c4770, 0xfffffff0, "ALC203" },
	{ 0x414c4781, 0xffffffff, "ALC658D" },
	{ 0x414c4780, 0xfffffff0, "ALC658" },
	{ 0x414c4790, 0xfffffff0, "ALC850" },
	/* Aztech */
	{ 0x415a5401, 0xffffffff, "AZF3328" },
	/* C-Media */
	{ 0x434d4941, 0xffffffff, "CMI9738" },
	{ 0x434d4961, 0xffffffff, "CMI9739" },
	{ 0x434d4969, 0xffffffff, "CMI9780" },
	{ 0x434d4978, 0xffffffff, "CMI9761A" },
	{ 0x434d4982, 0xffffffff, "CMI9761B" },
	{ 0x434d4983, 0xffffffff, "CMI9761A+" },
	/* Cirrus Logic / Crystal */
	{ 0x43525900, 0xfffffff8, "CS4297" },
	{ 0x43525910, 0xfffffff8, "CS4297A" },
	{ 0x43525920, 0xfffffff8, "CS4298" },
	{ 0x43525928, 0xfffffff8, "CS4294" },
	{ 0x43525930, 0xfffffff8, "CS4299" },
	{ 0x43525948, 0xfffffff8, "CS4201" },
	{ 0x43525958, 0xfffffff8, "CS4205" },
	{ 0x43525960, 0xfffffff8, "CS4291" },
	{ 0x43525970, 0xfffffff8, "CS4202" },
	/* Conexant */
	{ 0x43585421, 0xffffffff, "HSD11246" },
	{ 0x43585428, 0xfffffff8, "CX20468" },
	{ 0x43585430, 0xffffffff, "CX20468-31" },
	{ 0x43585431, 0xffffffff, "CX20551" },
	/* misc */
	{ 0x44543031, 0xfffffff0, "DT0398" },
	{ 0x454d4328, 0xffffffff, "EM28028" },
	{ 0x45838308, 0xffffffff, "ES1988" },
	{ 0x48525300, 0xffffff00, "HMP9701" },
	/* ICEnsemble (and VIA parts with an ICE id) */
	{ 0x49434501, 0xffffffff, "ICE1230" },
	{ 0x49434511, 0xffffffff, "ICE1232" },
	{ 0x49434514, 0xffffffff, "ICE1232A" },
	{ 0x49434551, 0xffffffff, "VT1616" },
	{ 0x49434552, 0xffffffff, "VT1616i" },
	/* ITE */
	{ 0x49544520, 0xffffffff, "IT2226E" },
	{ 0x49544561, 0xffffffff, "IT2646E" },
	/* National Semiconductor */
	{ 0x4e534331, 0xffffffff, "LM4549" },
	{ 0x4e534350, 0xffffffff, "LM4550" },
	{ 0x4e534300, 0xffffffff, "LM4540/43/45/46/48" },
	/* Philips */
	{ 0x50534304, 0xffffffff, "UCB1400" },
	/* Silicon Laboratories (modem) */
	{ 0x53494c20, 0xffffffe0, "Si3036/8" },
	/* ST */
	{ 0x53544d02, 0xffffffff, "ST7597" },
	/* TriTech */
	{ 0x54524102, 0xffffffff, "TR28022" },
	{ 0x54524103, 0xffffffff, "TR28023" },
	{ 0x54524106, 0xffffffff, "TR28026" },
	{ 0x54524108, 0xffffffff, "TR28028" },
	{ 0x54524123, 0xffffffff, "TR28602" },
	/* Texas Instruments */
	{ 0x54584e03, 0xffffffff, "TLV320AIC27" },
	{ 0x54584e20, 0xffffffff, "TLC320AD9xC" },
	/* VIA */
	{ 0x56494120, 0xfffffff0, "VT1613" },
	{ 0x56494161, 0xffffffff, "VT1612A" },
	{ 0x56494170, 0xffffffff, "VT1617A" },
	{ 0x56494182, 0xffffffff, "VT1618" },
	/* Winbond */
	{ 0x57454301, 0xffffffff, "W83971D" },
	/* Wolfson */
	{ 0x574d4c00, 0xffffffff, "WM9701A" },
	{ 0x574d4c03, 0xffffffff, "WM9703/07/08/17" },
	{ 0x574d4c04, 0xffffffff, "WM9704M/Q" },
	{ 0x574d4c05, 0xffffffff, "WM9705/10" },
	{ 0x574d4c09, 0xffffffff, "WM9709" },
	{ 0x574d4c12, 0xffffffff, "WM9711/12/15" },
	{ 0x574d4c13, 0xffffffff, "WM9713/14" },
	/* Yamaha */
	{ 0x594d4800, 0xffffffff, "YMF743" },
	{ 0x594d4802, 0xffffffff, "YMF752" },
	{ 0x594d4803, 0xffffffff, "YMF753" },
	/* SigmaTel */
	{ 0x83847600, 0xffffffff, "STAC9700/83/84" },
	{ 0x83847604, 0xffffffff, "STAC9701/03/04/05" },
	{ 0x83847605, 0xffffffff, "STAC9704" },
	{ 0x83847608, 0xffffffff, "STAC9708/11" },
	{ 0x83847609, 0xffffffff, "STAC9721/23" },
	{ 0x83847644, 0xffffffff, "STAC9744" },
	{ 0x83847650, 0xffffffff, "STAC9750/51" },
	{ 0x83847652, 0xffffffff, "STAC9752/53" },
	{ 0x83847656, 0xffffffff, "STAC9756/57" },
	{ 0x83847658, 0xffffffff, "STAC9758/59" },
	{ 0x83847666, 0xffffffff, "STAC9766/67" },
	{ 0, 0, 0 }
};

/* Machines (by the controller's PCI subsystem id) whose board wiring
 * differs from the codec defaults. */
struct ac97_machine_quirk {
	uint16_t vendor, device, mask;
	uint32_t quirks;
	const char *name;
};

static const struct ac97_machine_quirk ac97_machine_quirks[] = {
	{ 0x0e11, 0x000e, 0xffff, AC97_Q_HP_ONLY, "Compaq Deskpro EN" },
	{ 0x0e11, 0x008a, 0xffff, AC97_Q_HP_ONLY, "Compaq Evo W4000" },
	{ 0x0e11, 0x00b8, 0xffff, AC97_Q_HP_ONLY, "Compaq Evo D510C" },
	{ 0x1014, 0x0534, 0xffff, AC97_Q_INV_EAPD, "ThinkPad X31" },
	{ 0x1014, 0x0267, 0xffff, AC97_Q_HP_ONLY, "IBM NetVista A30p" },
	{ 0x1025, 0x0082, 0xffff, AC97_Q_HP_ONLY, "Acer Travelmate 2310" },
	{ 0x1025, 0x0083, 0xffff, AC97_Q_HP_ONLY, "Acer Aspire 3003LCi" },
	{ 0x1028, 0x00d8, 0xffff, AC97_Q_HP_ONLY, "Dell Precision 530" },
	{ 0x1028, 0x010d, 0xffff, AC97_Q_HP_ONLY, "Dell" },
	{ 0x1028, 0x0126, 0xffff, AC97_Q_HP_ONLY, "Dell Optiplex GX260" },
	{ 0x1028, 0x012c, 0xffff, AC97_Q_HP_ONLY, "Dell Precision 650" },
	{ 0x1028, 0x012d, 0xffff, AC97_Q_HP_ONLY, "Dell Precision 450" },
	{ 0x1028, 0x0147, 0xffff, AC97_Q_HP_ONLY, "Dell" },
	{ 0x1028, 0x0151, 0xffff, AC97_Q_HP_ONLY, "Dell Optiplex GX270" },
	{ 0x1028, 0x014e, 0xffff, AC97_Q_HP_ONLY, "Dell D800" },
	{ 0x1028, 0x0163, 0xffff, AC97_Q_HP_ONLY, "Dell" },
	{ 0x1028, 0x016a, 0xffff, AC97_Q_HP_ONLY, "Dell Inspiron 8600" },
	{ 0x1028, 0x0182, 0xffff, AC97_Q_HP_ONLY, "Dell Latitude D610" },
	{ 0x1028, 0x0191, 0xffff, AC97_Q_HP_ONLY, "Dell Inspiron 8600" },
	{ 0x103c, 0x00c3, 0xffff, AC97_Q_HP_ONLY, "HP xw6000" },
	{ 0x103c, 0x129d, 0xffff, AC97_Q_HP_ONLY, "HP xw8000" },
	{ 0x103c, 0x12f1, 0xffff, AC97_Q_HP_ONLY, "HP xw8200" },
	{ 0x103c, 0x12f2, 0xffff, AC97_Q_HP_ONLY, "HP xw6200" },
	{ 0x103c, 0x3008, 0xffff, AC97_Q_HP_ONLY, "HP xw4200" },
	{ 0x104d, 0x8144, 0xffff, AC97_Q_INV_EAPD, "Sony" },
	{ 0x104d, 0x8197, 0xffff, AC97_Q_INV_EAPD, "Sony S1XP" },
	{ 0x104d, 0x81c0, 0xffff, AC97_Q_INV_EAPD, "Sony VAIO VGN-T350P" },
	{ 0x104d, 0x81c5, 0xffff, AC97_Q_INV_EAPD, "Sony VAIO VGN-B1VP" },
	{ 0x1043, 0x80f3, 0xffff, AC97_Q_AD_SHARING, "ASUS ICH5/AD1985" },
	{ 0x10cf, 0x11c3, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens E4010" },
	{ 0x10cf, 0x1225, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens T3010" },
	{ 0x10cf, 0x1253, 0xffff, AC97_Q_HP_ONLY, "Fujitsu S6210" },
	{ 0x10cf, 0x127d, 0xffff, AC97_Q_HP_ONLY, "Fujitsu Lifebook P7010" },
	{ 0x10cf, 0x127e, 0xffff, AC97_Q_HP_ONLY, "Fujitsu Lifebook C1211D" },
	{ 0x10cf, 0x12ec, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens 4010" },
	{ 0x10cf, 0x12f2, 0xffff, AC97_Q_SWAP_HP, "Fujitsu-Siemens Celsius H320" },
	{ 0x10f1, 0x2665, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens Celsius" },
	{ 0x10f1, 0x2885, 0xffff, AC97_Q_HP_ONLY, "AMD64 board" },
	{ 0x10f1, 0x2895, 0xffff, AC97_Q_HP_ONLY, "Tyan Thunder K8WE" },
	{ 0x10f7, 0x834c, 0xffff, AC97_Q_HP_ONLY, "Panasonic CF-R4" },
	{ 0x110a, 0x0056, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens Scenic" },
	{ 0x11d4, 0x5375, 0xffff, AC97_Q_HP_ONLY, "AD1985 (discrete)" },
	{ 0x1462, 0x5470, 0xffff, AC97_Q_HP_ONLY, "MSI P4 ATX 645 Ultra" },
	{ 0x161f, 0x202f, 0xffff, AC97_Q_INV_EAPD, "Gateway M520" },
	{ 0x161f, 0x203a, 0xffff, AC97_Q_INV_EAPD, "Gateway 4525GZ" },
	{ 0x1734, 0x0088, 0xffff, AC97_Q_HP_ONLY, "Fujitsu-Siemens D1522" },
	{ 0x8086, 0x2000, 0xfff0, AC97_Q_AD_SHARING, "Intel ICH5/AD1985" },
	{ 0x8086, 0x4000, 0xfff0, AC97_Q_AD_SHARING, "Intel ICH5/AD1985" },
	{ 0x8086, 0x4856, 0xffff, AC97_Q_SWAP_HP, "Intel D845WN" },
	{ 0x8086, 0x4d44, 0xffff, AC97_Q_HP_ONLY, "Intel D850EMV2" },
	{ 0x8086, 0x4d56, 0xffff, AC97_Q_HP_ONLY, "Intel ICH/AD1885" },
	{ 0x8086, 0x6000, 0xfff0, AC97_Q_AD_SHARING, "Intel ICH5/AD1985" },
	{ 0x8086, 0xe000, 0xfff0, AC97_Q_AD_SHARING, "Intel ICH5/AD1985" },
	{ 0, 0, 0, 0, 0 }
};

static const char *const ac97_rec_names[AC97_REC_NSOURCES] = {
	"Mic", "CD", "Video", "Aux", "Line In", "Stereo Mix", "Mono Mix", "Phone"
};

/* Standard rates tried for variable rate audio, lowest first. */
static const uint32_t ac97_std_rates[] = { 8000,  11025, 16000, 22050,
					   32000, 44100, 48000 };
#define AC97_NUM_STD_RATES (sizeof(ac97_std_rates) / sizeof(ac97_std_rates[0]))

/* Default levels */
#define AC97_DEFAULT_MASTER 85
#define AC97_DEFAULT_OUT 100
#define AC97_DEFAULT_PCM 100
#define AC97_DEFAULT_CAPTURE 0 /* 0 dB of record gain */

/* Most attenuation steps (1.5 dB each) the 1..100 % range is spread over;
 * 0 % always selects the field's maximum. */
#define AC97_OUT_USE_STEPS 31

/* ---- small helpers -------------------------------------------------------- */

static const char *name_lookup(const struct ac97_name *t, uint32_t id)
{
	for (; t->name; t++)
		if ((id & t->mask) == t->id)
			return t->name;
	return 0;
}

const char *ac97_vendor_name(uint32_t id)
{
	const char *n = name_lookup(ac97_vendors, id);
	return n ? n : "Unknown";
}

const char *ac97_chip_name(uint32_t id)
{
	const char *n = name_lookup(ac97_chips, id);
	return n ? n : "AC'97 codec";
}

static int is_vendor(const struct ac97_codec *c, uint32_t prefix)
{
	return (c->id & 0xffffff00u) == prefix;
}

static int id_is(const struct ac97_codec *c, uint32_t id, uint32_t mask)
{
	return (c->id & mask) == id;
}

#define VENDOR_AD 0x41445300u
#define VENDOR_ALC 0x414c4700u
#define VENDOR_WOLFSON 0x574d4c00u
#define VENDOR_SIGMATEL 0x83847600u

/* Formatted append into a bounded buffer. */
struct ac97_dbuf {
	char *buf;
	size_t len;
	size_t pos;
};

static void dbuf_printf(struct ac97_dbuf *d, const char *fmt, ...)
{
	__builtin_va_list ap;
	if (!d->buf || d->len == 0 || d->pos + 1 >= d->len)
		return;
	__builtin_va_start(ap, fmt);
	kvsnprintf(d->buf + d->pos, d->len - d->pos, fmt, ap);
	__builtin_va_end(ap);
	d->pos += kstrlen(d->buf + d->pos);
	if (d->pos >= d->len)
		d->pos = d->len - 1;
}

/* ---- register access ------------------------------------------------------ */

static int reg_ok(unsigned int reg)
{
	return reg < 0x80 && (reg & 1) == 0;
}

/* Unlocked: init path, or callers holding c->lock. */
static int rd(struct ac97_codec *c, unsigned int reg, uint16_t *val)
{
	uint16_t v = 0xffff;
	int rc;

	if (!reg_ok(reg)) {
		*val = 0xffff;
		return -EINVAL;
	}
	rc = c->ops->read(c->bus, c->num, reg, &v);
	*val = rc ? 0xffff : v;
	return rc;
}

static uint16_t rdv(struct ac97_codec *c, unsigned int reg)
{
	uint16_t v;
	rd(c, reg, &v);
	return v;
}

static int wr(struct ac97_codec *c, unsigned int reg, uint16_t val)
{
	int rc;

	if (!reg_ok(reg))
		return -EINVAL;
	rc = c->ops->write(c->bus, c->num, reg, val);
	/* The shadow holds what we asked for, even when the link timed
	 * out: a later restore replays it. */
	c->regs[reg >> 1] = val;
	c->reg_valid |= 1ull << (reg >> 1);
	return rc;
}

/* Value of a register as last written, or read now if never written. */
static uint16_t cached(struct ac97_codec *c, unsigned int reg)
{
	if (!reg_ok(reg))
		return 0xffff;
	if (c->reg_valid & (1ull << (reg >> 1)))
		return c->regs[reg >> 1];
	return rdv(c, reg);
}

/* Read-modify-write against the live register. */
static int update_bits(struct ac97_codec *c, unsigned int reg, uint16_t mask,
		       uint16_t val)
{
	uint16_t old = rdv(c, reg);
	uint16_t nv = (uint16_t)((old & ~mask) | (val & mask));
	if (old == nv && (c->reg_valid & (1ull << (reg >> 1))))
		return 0;
	return wr(c, reg, nv);
}

int ac97_codec_read(struct ac97_codec *c, unsigned int reg, uint16_t *val)
{
	uint64_t fl;
	int rc;

	if (!c || !val)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, &fl);
	rc = rd(c, reg, val);
	spin_unlock_irqrestore(&c->lock, fl);
	return rc;
}

int ac97_codec_write(struct ac97_codec *c, unsigned int reg, uint16_t val)
{
	uint64_t fl;
	int rc;

	if (!c)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, &fl);
	rc = wr(c, reg, val);
	spin_unlock_irqrestore(&c->lock, fl);
	return rc;
}

/* ---- presence and readiness ----------------------------------------------- */

/* The codec answers and holds a written value.  The record gain register
 * is used because every audio codec has it and nothing else depends on its
 * contents during init (the master/PCM registers may already be set up
 * by firmware and the reset register has side effects). */
static int codec_answers(struct ac97_codec *c)
{
	uint16_t v;

	/* preliminary reads settle the link on some parts */
	rdv(c, AC97_RESET);
	rdv(c, AC97_VENDOR_ID1);
	rdv(c, AC97_VENDOR_ID2);
	wr(c, AC97_REC_GAIN, 0x8a05);
	if (rd(c, AC97_REC_GAIN, &v) == 0 && (v & 0x7fff) == 0x0a05)
		return 1;
	return 0;
}

static int wait_codec_answers(struct ac97_codec *c, uint32_t timeout_ms)
{
	uint32_t waited = 0;

	for (;;) {
		if (codec_answers(c))
			return 0;
		if (waited >= timeout_ms)
			return -ETIMEDOUT;
		snd_mdelay(5);
		waited += 5;
	}
}

static int is_modem_only(struct ac97_codec *c)
{
	uint16_t mid = rdv(c, AC97_EXTENDED_MID);
	return mid != 0xffff && (mid & 0x0001);
}

/* ---- naming and quirks ---------------------------------------------------- */

static void set_names(struct ac97_codec *c)
{
	const char *v = ac97_vendor_name(c->id);
	const char *ch = name_lookup(ac97_chips, c->id);

	kstrncpy(c->vendor, v, sizeof(c->vendor) - 1);
	c->vendor[sizeof(c->vendor) - 1] = 0;
	if (ch) {
		kstrncpy(c->chip, ch, sizeof(c->chip) - 1);
		c->chip[sizeof(c->chip) - 1] = 0;
	} else {
		/* Unknown part: show the raw id so it can be added. */
		ksnprintf(c->chip, sizeof(c->chip), "id %08x", c->id);
	}
	ksnprintf(c->name, sizeof(c->name), "%s %s", c->vendor, c->chip);
}

static uint32_t machine_quirks(uint16_t sv, uint16_t sd, const char **name)
{
	const struct ac97_machine_quirk *q;

	for (q = ac97_machine_quirks; q->name; q++) {
		if (q->vendor == sv && (sd & q->mask) == q->device) {
			*name = q->name;
			return q->quirks;
		}
	}
	return 0;
}

/* ---- probe ---------------------------------------------------------------- */

int ac97_codec_probe(struct ac97_codec *c, const struct ac97_bus_ops *ops,
		     void *bus, unsigned int num, uint16_t subsys_vendor,
		     uint16_t subsys_device, uint32_t ctrl_flags)
{
	uint16_t id1, id2;
	const char *qname = 0;
	int rc;

	if (!c || !ops || !ops->read || !ops->write ||
	    num >= AC97_MAX_CODECS)
		return -EINVAL;

	kmemset(c, 0, sizeof(*c));
	spinlock_init(&c->lock, "ac97-codec");
	c->ops = ops;
	c->bus = bus;
	c->num = num;
	c->subsys_vendor = subsys_vendor;
	c->subsys_device = subsys_device;
	c->ctrl_flags = ctrl_flags;
	c->clock = 48000;

	/* A codec that has just left cold reset can take a long while
	 * (the spec allows the analog section hundreds of ms) before its
	 * registers hold values. */
	rc = wait_codec_answers(c, (ctrl_flags & AC97_CTRL_IN_VM) ? 100 : 750);
	id1 = rdv(c, AC97_VENDOR_ID1);
	id2 = rdv(c, AC97_VENDOR_ID2);
	if (rc) {
		if (is_modem_only(c))
			return -ENODEV;
		if (id1 == 0xffff && id2 == 0xffff)
			return -EIO;
		return -ENODEV;
	}

	if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0))
		c->id = 0;
	else
		c->id = ((uint32_t)id1 << 16) | id2;

	c->caps = rdv(c, AC97_RESET);
	c->ext_id = rdv(c, AC97_EXTENDED_ID);
	if (c->ext_id == 0xffff)
		c->ext_id = 0;
	c->revision = (c->ext_id & AC97_EI_REV_MASK) >> AC97_EI_REV_SHIFT;

	/* A Si3036-style modem answers some audio registers too. */
	if (id_is(c, 0x53494c20, 0xffffffe0))
		return -ENODEV;

	set_names(c);

	c->quirks = machine_quirks(subsys_vendor, subsys_device, &qname);
	if (c->quirks)
		snd_info(DRV, "machine quirk for %04x:%04x (%s): 0x%x\n",
			 subsys_vendor, subsys_device, qname, c->quirks);
	if ((c->quirks & AC97_Q_AD_SHARING) && !is_vendor(c, VENDOR_AD))
		c->quirks &= ~AC97_Q_AD_SHARING;
	if (c->quirks & AC97_Q_AD_SHARING)
		c->quirks |= AC97_Q_HP_ONLY;
	return 0;
}

/* ---- vendor fixups -------------------------------------------------------- */

/* Analog Devices register extensions */
#define AD_CODEC_CFG 0x70
#define AD_JACK_SPDIF 0x72
#define AD_SERIAL_CFG 0x74
#define AD_MISC 0x76
#define AD_MISC_HPSEL 0x0400
#define AD_MISC_LOSEL 0x0020
#define AD_MISC_MSPLT 0x2000
#define AD_MISC_AC97NC 0x4000

/* Realtek ALC65x/85x */
#define ALC_SURR_DAC_VOL 0x64
#define ALC_LFE_DAC_VOL 0x66
#define ALC_MULTICH 0x6a
#define ALC_REVISION 0x6e
#define ALC_MISC 0x74
#define ALC_GPIO_SETUP 0x76
#define ALC_GPIO_STATUS 0x78
#define ALC_CLOCK 0x7a

/* SigmaTel */
#define STAC_OUTSEL 0x64
#define STAC_INSEL 0x66
#define STAC_IOMISC 0x68
#define STAC_ANALOG 0x6c
#define STAC_DAC2INVERT 0x6e
#define STAC_BIAS1 0x70
#define STAC_BIAS2 0x72
#define STAC_VARIOUS 0x72
#define STAC_MULTICHN 0x74
#define STAC_CIC1 0x76
#define STAC_CIC2 0x78

/* Wolfson */
#define WM_FMIXER_VOL 0x72

/* Register-file quirks that make the generic mixer setup wrong */
#define AC97_F_NONSTD_MIXER (1u << 16) /* do not touch the input mixers */
#define AC97_F_NONSTD_RECORD (1u << 17) /* record path registers differ */

static void select_vendor_page(struct ac97_codec *c)
{
	/* AC'97 2.3 codecs page the vendor registers 0x60-0x6e; page 0 is
	 * the vendor page.  Older codecs ignore the write. */
	if (c->revision >= 2)
		update_bits(c, AC97_INT_PAGING, 0x000f, 0x0000);
}

static void fixup_analog_devices(struct ac97_codec *c)
{
	uint16_t misc;

	if (id_is(c, 0x41445303, 0xffffffff)) {
		/* AD1819: broadcast to every chained codec, no ID bits */
		update_bits(c, AD_SERIAL_CFG, 0x7000, 0x7000);
		wr(c, AD_CODEC_CFG, 0x0000);
		return;
	}

	/* AD1881 and later: a single codec answers writes addressed to
	 * any of the chained slots; select all of them so that writes are
	 * not silently dropped when the board straps a secondary id. */
	update_bits(c, AD_SERIAL_CFG, 0x7000, 0x7000);
	if (c->quirks & AC97_Q_AD_SHARING)
		update_bits(c, AD_SERIAL_CFG, 0x0200, 0x0200);

	switch (c->id) {
	case 0x41445360: /* AD1885: line out is the headphone driver */
		wr(c, AD_MISC, 0x0404);
		break;
	case 0x41445361: /* AD1886: jack sense / SPDIF register default */
		wr(c, AD_JACK_SPDIF, 0x0010);
		break;
	case 0x41445368: /* AD1888 */
	case 0x41445370: /* AD1980 */
		misc = rdv(c, AD_MISC);
		misc |= AD_MISC_MSPLT | AD_MISC_AC97NC;
		/* Boards wire the line-out jack to the HP driver, except
		 * the ASUS A9T, which uses LINE_OUT as a real line out. */
		if (!(c->subsys_vendor == 0x1043 &&
		      c->subsys_device == 0x1193))
			misc |= AD_MISC_LOSEL | AD_MISC_HPSEL;
		wr(c, AD_MISC, misc);
		c->quirks |= AC97_Q_STEREO_MUTES;
		break;
	case 0x41445372: /* AD1981A */
	case 0x41445374: /* AD1981B */
		update_bits(c, AD_MISC, AD_MISC_MSPLT, AD_MISC_MSPLT);
		c->quirks |= AC97_Q_STEREO_MUTES;
		break;
	case 0x41445375: /* AD1985 */
		misc = rdv(c, AD_MISC);
		misc |= AD_MISC_LOSEL | AD_MISC_HPSEL | AD_MISC_MSPLT |
			AD_MISC_AC97NC;
		wr(c, AD_MISC, misc);
		c->quirks |= AC97_Q_STEREO_MUTES;
		/* rev 3 parts report AC'97 revision 0 */
		c->revision = 2;
		break;
	case 0x41445378: /* AD1986 */
		c->quirks |= AC97_Q_STEREO_MUTES;
		break;
	default:
		break;
	}
}

static void fixup_alc650(struct ac97_codec *c)
{
	uint16_t rev = rdv(c, ALC_REVISION) & 0x3f;
	uint16_t v;

	/* refine the name by silicon revision */
	if (rev < 3)
		kstrncpy(c->chip, "ALC650", sizeof(c->chip) - 1);
	else if (rev < 0x10)
		kstrncpy(c->chip, "ALC650D", sizeof(c->chip) - 1);
	else if (rev < 0x20)
		kstrncpy(c->chip, "ALC650E", sizeof(c->chip) - 1);
	else if (rev < 0x30)
		kstrncpy(c->chip, "ALC650F", sizeof(c->chip) - 1);
	ksnprintf(c->name, sizeof(c->name), "%s %s", c->vendor, c->chip);

	/* unlock GPIO setup and the clock register */
	wr(c, ALC_GPIO_STATUS, rdv(c, ALC_GPIO_STATUS) | 0x8000);
	/* S/PDIF input off: the pin may be wired as EAPD */
	v = rdv(c, ALC_CLOCK);
	wr(c, ALC_CLOCK, v & ~0x0003);
	/* slots 3/4 front, no duplication, line-in and mic stay inputs */
	wr(c, ALC_MULTICH, 0x0000);
	/* GPIO0 output, high: microphone bias */
	wr(c, ALC_GPIO_SETUP, rdv(c, ALC_GPIO_SETUP) | 0x0001);
	wr(c, ALC_GPIO_STATUS,
	   (uint16_t)((rdv(c, ALC_GPIO_STATUS) | 0x0100) & ~0x0010));
	/* surround and LFE DACs at 0 dB */
	wr(c, ALC_SURR_DAC_VOL, 0x0808);
	wr(c, ALC_LFE_DAC_VOL, 0x0808);
}

static void fixup_alc655(struct ac97_codec *c)
{
	int is658 = id_is(c, 0x414c4780, 0xfffffff0);
	int is658d = 0;
	uint16_t v;

	if (is658 && (rdv(c, ALC_REVISION) & 0x3f) == 2) {
		is658d = 1;
		kstrncpy(c->chip, "ALC658D", sizeof(c->chip) - 1);
		ksnprintf(c->name, sizeof(c->name), "%s %s", c->vendor,
			  c->chip);
	}
	select_vendor_page(c);

	v = rdv(c, ALC_CLOCK); /* misc control */
	if (is658) {
		v &= ~(1u << 1); /* pin 47 = S/PDIF in */
	} else {
		/* Some MSI-built laptops use pin 47 as EAPD for the
		 * internal speaker amplifier. */
		int pin47_eapd =
			c->subsys_vendor == 0x1462 &&
			(c->subsys_device == 0x0131 ||
			 c->subsys_device == 0x0161 ||
			 c->subsys_device == 0x0351 ||
			 c->subsys_device == 0x0471 ||
			 c->subsys_device == 0x0061);
		if (pin47_eapd)
			v &= ~(1u << 1);
		else
			v |= (1u << 1);
	}
	v &= ~(1u << 12); /* Vref enable */
	wr(c, ALC_CLOCK, v);
	wr(c, ALC_MULTICH, 1u << 15);
	wr(c, ALC_SURR_DAC_VOL, 0x0808);
	wr(c, ALC_LFE_DAC_VOL, 0x0808);
	if (is658d)
		update_bits(c, ALC_MISC, 0x0800, 0x0800);
}

static void fixup_alc850(struct ac97_codec *c)
{
	select_vendor_page(c);
	wr(c, ALC_MULTICH, 1u << 15);
	/* front amp on, Vref enabled, surround out on, mixer on */
	wr(c, ALC_CLOCK, (uint16_t)((1u << 1) | (1u << 4) | (1u << 6) |
				    (1u << 7) | (1u << 13)));
	/* jack routing: UIO1 front, UIO3 line, mic on its own pin */
	wr(c, ALC_GPIO_SETUP, (uint16_t)((1u << 4) | (1u << 7) | (2u << 8) |
					 (1u << 11) | (1u << 15)));
	wr(c, ALC_SURR_DAC_VOL, 0x0808);
	wr(c, ALC_LFE_DAC_VOL, 0x0808);
}

static void fixup_sigmatel(struct ac97_codec *c)
{
	uint16_t b2, an;

	switch (c->id) {
	case 0x83847600: /* STAC9700 */
		break;
	case 0x83847608: /* STAC9708/11 */
		b2 = rdv(c, STAC_BIAS2) & 0x8000;
		an = rdv(c, STAC_ANALOG);
		if (b2 == 0 && an == 0) {
			wr(c, STAC_CIC1, 0xabba);
			wr(c, STAC_CIC2, 0x1000);
			wr(c, STAC_BIAS1, 0xabba);
			wr(c, STAC_BIAS2, 0x0007);
		} else if (b2 == 0x8000 && an == 0) {
			wr(c, STAC_CIC1, 0xabba);
			wr(c, STAC_CIC2, 0x1001);
			wr(c, STAC_DAC2INVERT, 0x0008);
		}
		wr(c, STAC_MULTICHN, 0x0000);
		/* the headphone register drives the surround pair */
		c->caps |= AC97_BC_HEADPHONE;
		break;
	case 0x83847609: /* STAC9721/23 */
		if (rdv(c, STAC_ANALOG) == 0) {
			wr(c, STAC_CIC1, 0xabba);
			wr(c, STAC_CIC2, 0x4000);
			wr(c, STAC_BIAS1, 0xabba);
			wr(c, STAC_BIAS2, 0x0002);
		}
		wr(c, STAC_MULTICHN, 0x0000);
		break;
	case 0x83847644: /* STAC9744 */
	case 0x83847656: /* STAC9756/57 */
		wr(c, STAC_CIC1, 0xabba);
		wr(c, STAC_CIC2, 0x0000);
		wr(c, STAC_BIAS1, 0xabba);
		wr(c, STAC_BIAS2, 0x0002);
		wr(c, STAC_MULTICHN, 0x0000);
		break;
	case 0x83847658: /* STAC9758/59: jack routing */
		select_vendor_page(c);
		if (c->subsys_vendor == 0x107b && c->subsys_device == 0x0601) {
			/* Gateway M675 */
			wr(c, STAC_OUTSEL, 0xfc70);
			wr(c, STAC_IOMISC, 0x2102);
			wr(c, STAC_INSEL, 0x0203);
			wr(c, STAC_VARIOUS, 0x0041);
		} else {
			wr(c, STAC_OUTSEL, 0xd794);
			wr(c, STAC_IOMISC, 0x2001);
			wr(c, STAC_INSEL, 0x0201);
			wr(c, STAC_VARIOUS, 0x0040);
		}
		c->quirks |= AC97_Q_STEREO_MUTES;
		return;
	default:
		return;
	}
	/* no DAC/ADC 6 dB attenuation on the parts that have it */
	if (c->id == 0x83847600 || c->id == 0x83847608 ||
	    c->id == 0x83847609 || c->id == 0x83847644 ||
	    c->id == 0x83847656)
		wr(c, STAC_ANALOG, rdv(c, STAC_ANALOG) & ~0x0003);
}

static void fixup_wolfson(struct ac97_codec *c)
{
	switch (c->id) {
	case 0x574d4c03: /* WM9703/07/08: DAC to the front mixer at 0 dB */
		wr(c, WM_FMIXER_VOL, 0x0808);
		break;
	case 0x574d4c12: /* WM9711/12/15: sidetone/out3 mixers */
		wr(c, 0x60, 0x0808);
		wr(c, 0x62, 0x0808);
		wr(c, AC97_VIDEO, 0x0808);
		wr(c, AC97_AUX, 0x0808);
		wr(c, AC97_PC_BEEP, 0x0808);
		wr(c, AC97_CD, 0x0000);
		c->ctrl_flags |= AC97_F_NONSTD_MIXER;
		break;
	case 0x574d4c13: /* WM9713/14: its own mixer layout */
		wr(c, AC97_PC_BEEP, 0x0808);
		wr(c, AC97_PHONE, 0x0808);
		wr(c, AC97_MIC, 0x0808);
		wr(c, AC97_LINE, 0x00da);
		wr(c, AC97_CD, 0x0808);
		wr(c, AC97_VIDEO, 0xd612);
		wr(c, AC97_REC_GAIN, 0x1ba0);
		wr(c, AC97_EXTENDED_MID, 0xda00);
		wr(c, AC97_EXTENDED_MSTATUS, 0x3810);
		c->ctrl_flags |= AC97_F_NONSTD_MIXER | AC97_F_NONSTD_RECORD;
		break;
	default:
		break;
	}
}

/* Fixups that must run before the power-up handshake. */
static void vendor_fixups_early(struct ac97_codec *c)
{
	if (is_vendor(c, VENDOR_AD))
		fixup_analog_devices(c);
	if (id_is(c, 0x54524108, 0xffffffff)) {
		/* TR28028: cycle the analog section once */
		wr(c, AC97_POWERDOWN, 0x0300);
		snd_udelay(100);
		wr(c, AC97_POWERDOWN, 0x0000);
		wr(c, AC97_SURROUND_MASTER, 0x0000);
		wr(c, AC97_SPDIF, 0x0000);
	}
}

/* Fixups after power-up, before the mixer is set. */
static void vendor_fixups_late(struct ac97_codec *c)
{
	if (id_is(c, 0x414c4720, 0xfffffff0))
		fixup_alc650(c);
	else if (id_is(c, 0x414c4760, 0xfffffff0) ||
		 id_is(c, 0x414c4780, 0xfffffff0))
		fixup_alc655(c);
	else if (id_is(c, 0x414c4790, 0xfffffff0))
		fixup_alc850(c);
	else if (id_is(c, 0x414c4770, 0xfffffff0))
		update_bits(c, ALC_CLOCK, 0x0400, 0x0400); /* ALC203 */
	else if (id_is(c, 0x43585431, 0xffffffff))
		update_bits(c, 0x5c, 0x0001, 0x0001); /* CX20551 */
	else if (is_vendor(c, VENDOR_SIGMATEL))
		fixup_sigmatel(c);
	else if (is_vendor(c, VENDOR_WOLFSON))
		fixup_wolfson(c);
}

/* ---- power ---------------------------------------------------------------- */

/* The EAPD bit for "external amplifier on/off", honouring inversion. */
static uint16_t eapd_bits(const struct ac97_codec *c, int amp_on)
{
	int set;

	if (c->quirks & AC97_Q_INV_EAPD)
		set = amp_on; /* bit set = amplifier powered */
	else
		set = !amp_on; /* bit set = amplifier powered down */
	return set ? AC97_PD_EAPD : 0;
}

/* Clear the powerdown requests and wait for the ADC, DAC, analog mixer
 * and Vref ready bits.  A codec without a capture path never reports the
 * ADC ready; one that never reports anything still gets used after the
 * timeout (some emulations leave the status at zero). */
static int power_up(struct ac97_codec *c)
{
	uint16_t pd, want = AC97_PD_READY_MASK;
	uint32_t waited = 0, limit;

	pd = cached(c, AC97_POWERDOWN);
	pd &= ~(AC97_PD_PR0 | AC97_PD_PR1 | AC97_PD_PR2 | AC97_PD_PR3 |
		AC97_PD_PR4 | AC97_PD_PR5 | AC97_PD_PR6);
	if (!(c->quirks & AC97_Q_NO_EAPD)) {
		pd &= ~AC97_PD_EAPD;
		pd |= eapd_bits(c, 1);
	}
	wr(c, AC97_POWERDOWN, pd);

	if (c->quirks & AC97_Q_NO_POWER_WAIT)
		return 0;
	limit = (c->ctrl_flags & AC97_CTRL_IN_VM) ? 50 : 1000;
	for (;;) {
		uint16_t st = rdv(c, AC97_POWERDOWN);
		if (st != 0xffff && (st & want) == want)
			return 0;
		if (waited >= limit) {
			if (st != 0xffff &&
			    (st & (AC97_PD_DAC_READY | AC97_PD_REF_READY |
				   AC97_PD_ANL_READY)) ==
				    (AC97_PD_DAC_READY | AC97_PD_REF_READY |
				     AC97_PD_ANL_READY))
				return 0; /* playback side is up */
			snd_info(DRV,
				 "codec %u: power-up not confirmed "
				 "(status 0x%04x), continuing\n",
				 c->num, st);
			return -ETIMEDOUT;
		}
		snd_mdelay(2);
		waited += 2;
	}
}

void ac97_codec_capture_power(struct ac97_codec *c, int on)
{
	uint64_t fl;
	uint16_t pd;

	if (!c || !c->ready)
		return;
	spin_lock_irqsave(&c->lock, &fl);
	pd = cached(c, AC97_POWERDOWN);
	if (on)
		pd &= ~AC97_PD_PR0;
	else
		pd |= AC97_PD_PR0;
	if (pd != cached(c, AC97_POWERDOWN))
		wr(c, AC97_POWERDOWN, pd);
	spin_unlock_irqrestore(&c->lock, fl);
}

/* ---- sample rates --------------------------------------------------------- */

static unsigned int rate_reg(int dir)
{
	return dir == AUDIO_DIR_CAPTURE ? AC97_PCM_LR_ADC_RATE
					: AC97_PCM_FRONT_DAC_RATE;
}

/* The register value for `rate' given the link clock: a codec whose bit
 * clock runs off-nominal is compensated by scaling the request. */
static uint32_t rate_to_reg(const struct ac97_codec *c, uint32_t rate)
{
	uint64_t v = (uint64_t)rate * c->clock;
	return (uint32_t)((v + 24000) / 48000);
}

static int rate_try(struct ac97_codec *c, unsigned int reg, uint32_t rate)
{
	uint32_t v = rate_to_reg(c, rate);
	uint16_t back;

	if (v == 0 || v > 0xffff)
		return 0;
	wr(c, reg, (uint16_t)v);
	back = rdv(c, reg);
	return back == (uint16_t)v;
}

static int enable_vra(struct ac97_codec *c)
{
	uint16_t st;

	c->vra = 0;
	if (!(c->ext_id & AC97_EI_VRA) || (c->quirks & AC97_Q_NO_VRA) ||
	    (c->ctrl_flags & AC97_CTRL_NO_VRA))
		return 0;
	st = rdv(c, AC97_EXTENDED_STATUS);
	if (st == 0xffff)
		return 0;
	st &= ~AC97_EA_DRA; /* single rate only: the link runs 2 slots */
	st |= AC97_EA_VRA;
	wr(c, AC97_EXTENDED_STATUS, st);
	st = rdv(c, AC97_EXTENDED_STATUS);
	if (st & AC97_EA_VRA)
		c->vra = 1;
	return c->vra;
}

static void detect_rates(struct ac97_codec *c)
{
	int dir;
	unsigned int i;

	for (dir = 0; dir < 2; dir++) {
		uint32_t mask = 0;
		unsigned int reg = rate_reg(dir);

		if (c->vra) {
			for (i = 0; i < AC97_NUM_STD_RATES; i++)
				if (rate_try(c, reg, ac97_std_rates[i]))
					mask |= audio_rate_bit(ac97_std_rates[i]);
		}
		/* Without VRA (or when the register would not take a
		 * value) the converter runs at the link rate. */
		if (!(mask & AUDIO_RATE_48000))
			mask = AUDIO_RATE_48000;
		c->rates[dir] = mask;
		c->cur_rate[dir] = 48000;
		if (c->vra)
			wr(c, reg, (uint16_t)rate_to_reg(c, 48000));
	}
}

void ac97_codec_set_clock(struct ac97_codec *c, uint32_t clock)
{
	if (!c || clock == 0)
		return;
	if (clock < 8000 || clock > 96000)
		return;
	c->clock = clock;
}

uint32_t ac97_codec_rates(const struct ac97_codec *c, int dir)
{
	if (!c || (dir != AUDIO_DIR_PLAYBACK && dir != AUDIO_DIR_CAPTURE))
		return 0;
	return c->rates[dir];
}

int ac97_codec_set_rate(struct ac97_codec *c, int dir, uint32_t rate)
{
	uint64_t fl;
	uint32_t bit = audio_rate_bit(rate);
	int rc = 0;

	if (!c || (dir != AUDIO_DIR_PLAYBACK && dir != AUDIO_DIR_CAPTURE))
		return -EINVAL;
	if (!bit || !(c->rates[dir] & bit))
		return -EINVAL;
	if (!c->vra) {
		/* fixed rate codec: only the link rate */
		if (rate != 48000)
			return -EINVAL;
		c->cur_rate[dir] = 48000;
		return 0;
	}
	spin_lock_irqsave(&c->lock, &fl);
	if (!rate_try(c, rate_reg(dir), rate)) {
		/* the register would not take it: retry once after
		 * re-asserting VRA (a link glitch may have reset it) */
		update_bits(c, AC97_EXTENDED_STATUS, AC97_EA_VRA, AC97_EA_VRA);
		if (!rate_try(c, rate_reg(dir), rate))
			rc = -EIO;
	}
	if (rc == 0)
		c->cur_rate[dir] = rate;
	spin_unlock_irqrestore(&c->lock, fl);
	return rc;
}

/* ---- mixer layout detection ----------------------------------------------- */

/* A volume register exists when it holds a mute bit (set at reset) or
 * takes one when written. */
static int vol_exists(struct ac97_codec *c, unsigned int reg)
{
	uint16_t v = rdv(c, reg);

	if (v == 0xffff)
		return 0;
	if (v & AC97_MUTE)
		return 1;
	wr(c, reg, v | AC97_MUTE);
	rdv(c, reg); /* some parts echo the write on the first read */
	v = rdv(c, reg);
	return (v != 0xffff) && (v & AC97_MUTE);
}

/* Width of the level fields: write a single high bit and see whether it
 * sticks.  A 5-bit field turns 0x20 into 0x1f, a 4-bit one 0x10 into
 * 0x0f.  Returns the field maximum (63, 31 or 15). */
static uint8_t vol_width(struct ac97_codec *c, unsigned int reg, int stereo)
{
	static const uint16_t probe[3] = { 0x20, 0x10, 0x01 };
	static const uint8_t maxv[3] = { 63, 31, 15 };
	unsigned int i;

	for (i = 0; i < 3; i++) {
		uint16_t w = AC97_MUTE | probe[i];
		uint16_t v;
		if (stereo)
			w |= (uint16_t)(probe[i] << 8);
		wr(c, reg, w);
		rdv(c, reg);
		v = rdv(c, reg);
		if ((v & 0x3f) == probe[i])
			return maxv[i];
	}
	return 31; /* the AC'97 minimum for master/PCM */
}

static void vol_setup(struct ac97_codec *c, struct ac97_vol *v,
		      unsigned int reg, int stereo, int gain, uint8_t zero,
		      uint8_t fixed_max)
{
	kmemset(v, 0, sizeof(*v));
	v->reg = (uint8_t)reg;
	if (!vol_exists(c, reg))
		return;
	v->present = 1;
	v->stereo = (uint8_t)stereo;
	v->gain = (uint8_t)gain;
	v->max = fixed_max ? fixed_max : vol_width(c, reg, stereo);
	/* Some emulated codecs store any value written but only decode
	 * five bits of it, so the six-bit probe cannot be trusted there;
	 * field values up to 31 mean the same on either width. */
	if (v->max > 31 && (c->ctrl_flags & AC97_CTRL_IN_VM))
		v->max = 31;
	v->zero = zero;
	if (v->zero > v->max)
		v->zero = 0;
	if (gain)
		v->use_steps = v->max;
	else if (zero)
		v->use_steps = (uint8_t)(v->max - zero);
	else
		v->use_steps = v->max < AC97_OUT_USE_STEPS
				       ? v->max
				       : AC97_OUT_USE_STEPS;
	if (v->use_steps == 0)
		v->use_steps = 1;
}

static struct ac97_vol *vol_for_reg(struct ac97_codec *c, unsigned int reg)
{
	if (reg == AC97_MASTER)
		return &c->hw_master;
	if (reg == AC97_HEADPHONE)
		return &c->hw_hp;
	if (reg == AC97_MASTER_MONO)
		return &c->hw_mono;
	if (reg == AC97_PCM)
		return &c->hw_pcm;
	if (reg == AC97_REC_GAIN)
		return &c->hw_recgain;
	return 0;
}

static void detect_mixer(struct ac97_codec *c)
{
	vol_setup(c, &c->hw_master, AC97_MASTER, 1, 0, 0, 0);
	if (!c->hw_master.present) {
		/* Every audio codec has a master volume; a part that
		 * hides it (emulations) still takes the writes. */
		c->hw_master.present = 1;
		c->hw_master.stereo = 1;
		c->hw_master.max = 31;
		c->hw_master.use_steps = 31;
	}
	if (c->caps & AC97_BC_HEADPHONE)
		vol_setup(c, &c->hw_hp, AC97_HEADPHONE, 1, 0, 0, 0);
	vol_setup(c, &c->hw_mono, AC97_MASTER_MONO, 0, 0, 0, 0);
	vol_setup(c, &c->hw_pcm, AC97_PCM, 1, 0, 0x08, 0);
	if (!c->hw_pcm.present) {
		c->hw_pcm.present = 1;
		c->hw_pcm.stereo = 1;
		c->hw_pcm.max = 31;
		c->hw_pcm.zero = 8;
		c->hw_pcm.use_steps = 23;
	}
	vol_setup(c, &c->hw_recgain, AC97_REC_GAIN, 1, 1, 0, 15);
	vol_setup(c, &c->hw_mic, AC97_MIC, 0, 0, 0x08, 31);

	/* Map the physical outputs onto registers. */
	kmemset(c->out_reg, 0, sizeof(c->out_reg));
	kmemset(c->out_reg2, 0, sizeof(c->out_reg2));
	if ((c->quirks & AC97_Q_HP_ONLY) && c->hw_hp.present) {
		/* one jack, fed by the headphone amplifier; master kept in
		 * step for codecs that chain the two */
		c->out_reg[AC97_OUT_MAIN] = AC97_HEADPHONE;
		c->out_reg2[AC97_OUT_MAIN] = AC97_MASTER;
		c->num_outputs = 1;
	} else if ((c->quirks & AC97_Q_SWAP_HP) && c->hw_hp.present) {
		c->out_reg[AC97_OUT_MAIN] = AC97_HEADPHONE;
		c->out_reg[AC97_OUT_HP] = AC97_MASTER;
		c->num_outputs = 2;
	} else {
		c->out_reg[AC97_OUT_MAIN] = AC97_MASTER;
		if (c->hw_hp.present) {
			c->out_reg[AC97_OUT_HP] = AC97_HEADPHONE;
			c->num_outputs = 2;
		} else {
			c->num_outputs = 1;
		}
	}

	/* record sources: all eight selector positions exist on every
	 * codec; which are wired depends on the board */
	c->rec_sources = (1u << AC97_REC_NSOURCES) - 1;
}

/* ---- level arithmetic ----------------------------------------------------- */

/* 0..100 % to a register field value. */
static uint32_t pct_to_field(const struct ac97_vol *v, uint32_t pct)
{
	uint32_t range;

	if (pct > 100)
		pct = 100;
	if (v->gain)
		return (pct * v->max + 50) / 100;
	if (pct == 0)
		return v->max;
	range = v->use_steps;
	return v->zero + ((100 - pct) * range + 50) / 100;
}

/* Field value to hundredths of a dB. */
static int32_t field_to_db(const struct ac97_vol *v, uint32_t field)
{
	if (v->gain)
		return (int32_t)field * 150;
	return ((int32_t)v->zero - (int32_t)field) * 150;
}

static int32_t vol_db_min(const struct ac97_vol *v)
{
	return v->gain ? 0 : field_to_db(v, v->max);
}

static int32_t vol_db_max(const struct ac97_vol *v)
{
	return v->gain ? field_to_db(v, v->max) : 0;
}

static uint32_t vol_steps(const struct ac97_vol *v)
{
	return v->gain ? v->max : (uint32_t)(v->max - v->zero);
}

static uint16_t vol_value(const struct ac97_codec *c, const struct ac97_vol *v,
			  uint32_t lf, uint32_t rf, int mute)
{
	uint16_t val = (uint16_t)(rf & 0x3f);

	if (v->stereo)
		val |= (uint16_t)((lf & 0x3f) << 8);
	else
		val = (uint16_t)(((lf + rf) / 2) & 0x3f);
	if (mute) {
		val |= AC97_MUTE;
		if (v->stereo && (c->quirks & AC97_Q_STEREO_MUTES))
			val |= AC97_MUTE_RIGHT;
	}
	return val;
}

static int output_active(const struct ac97_codec *c, int out)
{
	if (out < 0 || out >= c->num_outputs)
		return 0;
	if (c->out_mode == AUDIO_OUTSEL_ID)
		return c->out_selected == (uint32_t)(out + 1);
	/* AUTO and ALL: no jack sensing on these codecs, every output
	 * plays */
	return 1;
}

static uint32_t scale_pct(uint32_t a, uint32_t b)
{
	return (a * b + 50) / 100;
}

/* Effective output level: the master control scales each output. */
static void out_level(const struct ac97_codec *c, int out, uint32_t *l,
		      uint32_t *r, int *mute)
{
	*l = scale_pct(c->master.left, c->out[out].left);
	*r = scale_pct(c->master.right, c->out[out].right);
	*mute = c->master.mute || c->out[out].mute || !output_active(c, out);
}

static void write_vol(struct ac97_codec *c, unsigned int reg, uint32_t l,
		      uint32_t r, int mute)
{
	struct ac97_vol *v = vol_for_reg(c, reg);

	if (!v || !v->present)
		return;
	/* an attenuation control at zero on both channels is silence */
	if (!v->gain && l == 0 && r == 0)
		mute = 1;
	wr(c, reg,
	   vol_value(c, v, pct_to_field(v, l), pct_to_field(v, r), mute));
}

static void apply_eapd(struct ac97_codec *c)
{
	uint16_t pd;
	int amp_on;

	if (c->quirks & AC97_Q_NO_EAPD)
		return;
	/* The external amplifier feeds the speakers off the main
	 * output: power it while that output is selected. */
	amp_on = output_active(c, AC97_OUT_MAIN);
	pd = cached(c, AC97_POWERDOWN);
	pd = (uint16_t)((pd & ~AC97_PD_EAPD) | eapd_bits(c, amp_on));
	if (pd != cached(c, AC97_POWERDOWN) ||
	    !(c->reg_valid & (1ull << (AC97_POWERDOWN >> 1))))
		wr(c, AC97_POWERDOWN, pd);
	c->eapd_on = amp_on;
}

static void apply_outputs(struct ac97_codec *c)
{
	int i;

	for (i = 0; i < c->num_outputs; i++) {
		uint32_t l, r;
		int mute;
		out_level(c, i, &l, &r, &mute);
		write_vol(c, c->out_reg[i], l, r, mute);
		if (c->out_reg2[i])
			write_vol(c, c->out_reg2[i], l, r, mute);
	}
	/* The mono output follows the main output (laptops use it for a
	 * single internal speaker). */
	if (c->hw_mono.present) {
		uint32_t l, r;
		int mute;
		out_level(c, AC97_OUT_MAIN, &l, &r, &mute);
		write_vol(c, AC97_MASTER_MONO, l, r, mute);
	}
	apply_eapd(c);
}

static void apply_pcm(struct ac97_codec *c)
{
	write_vol(c, AC97_PCM, c->pcm.left, c->pcm.right, (int)c->pcm.mute);
}

static void apply_capture(struct ac97_codec *c)
{
	uint16_t sel = (uint16_t)(c->rec_source & 7);
	uint16_t mic;

	if (c->ctrl_flags & AC97_F_NONSTD_RECORD)
		return; /* left as the vendor fixup set it */
	wr(c, AC97_REC_SEL, (uint16_t)((sel << 8) | sel));
	write_vol(c, AC97_REC_GAIN, c->capture.left, c->capture.right,
		  (int)c->capture.mute);
	/* The microphone register's loopback into the output mixer stays
	 * muted (no feedback); its preamp boost applies to recording. */
	if (c->hw_mic.present && !(c->ctrl_flags & AC97_F_NONSTD_MIXER)) {
		mic = AC97_MUTE | 0x0008;
		if (c->mic_boost)
			mic |= AC97_MIC_BOOST;
		wr(c, AC97_MIC, mic);
	}
	if ((c->caps & AC97_BC_DEDICATED_MIC) &&
	    !(c->ctrl_flags & AC97_F_NONSTD_MIXER))
		wr(c, AC97_REC_GAIN_MIC, AC97_MUTE);
}

/* Analog inputs are summed into the output; keep them silent so the
 * outputs carry only the PCM stream. */
static void mute_loopbacks(struct ac97_codec *c)
{
	static const uint8_t stereo_in[] = { AC97_LINE, AC97_CD, AC97_VIDEO,
					     AC97_AUX };
	unsigned int i;

	if (c->ctrl_flags & AC97_F_NONSTD_MIXER)
		return;
	for (i = 0; i < sizeof(stereo_in); i++)
		wr(c, stereo_in[i], AC97_MUTE | 0x0808);
	wr(c, AC97_PHONE, AC97_MUTE | 0x0008);
	wr(c, AC97_PC_BEEP, AC97_MUTE);
	if (c->caps & AC97_BC_BASS_TREBLE)
		wr(c, AC97_MASTER_TONE, 0x0f0f); /* tone bypass */
	if (c->caps & AC97_BC_3D_MASK) {
		/* 3D enhancement off when the register is writable */
		if (rdv(c, AC97_3D_CONTROL) != 0)
			wr(c, AC97_3D_CONTROL, 0x0000);
	}
	/* surround and center/LFE outputs are not driven: keep muted */
	if (c->ext_id & AC97_EI_SDAC)
		wr(c, AC97_SURROUND_MASTER, AC97_MUTE | AC97_MUTE_RIGHT);
	if (c->ext_id & (AC97_EI_CDAC | AC97_EI_LDAC))
		wr(c, AC97_CENTER_LFE_MASTER, AC97_MUTE | AC97_MUTE_RIGHT);
}

static void apply_all(struct ac97_codec *c)
{
	mute_loopbacks(c);
	apply_pcm(c);
	apply_capture(c);
	apply_outputs(c);
}

static void set_defaults(struct ac97_codec *c)
{
	int i;

	c->master.left = c->master.right = AC97_DEFAULT_MASTER;
	c->master.mute = 0;
	for (i = 0; i < AC97_NUM_OUTS; i++) {
		c->out[i].left = c->out[i].right = AC97_DEFAULT_OUT;
		c->out[i].mute = 0;
	}
	c->pcm.left = c->pcm.right = AC97_DEFAULT_PCM;
	c->pcm.mute = 0;
	c->capture.left = c->capture.right = AC97_DEFAULT_CAPTURE;
	c->capture.mute = 0;
	c->mic_boost = 0;
	c->out_mode = AUDIO_OUTSEL_AUTO;
	c->out_selected = AC97_OUT_ID_MAIN;
	c->rec_source = AC97_REC_MIC;
}

/* ---- init / restore / shutdown -------------------------------------------- */

int ac97_codec_init(struct ac97_codec *c)
{
	int rc;

	if (!c || !c->ops)
		return -EINVAL;

	/* Register reset: every mixer register back to its default, so
	 * nothing a previous owner (firmware, an earlier boot stage) left
	 * behind survives. */
	wr(c, AC97_RESET, 0x0000);
	snd_mdelay(1);
	rc = wait_codec_answers(c,
				(c->ctrl_flags & AC97_CTRL_IN_VM) ? 100 : 500);
	if (rc) {
		snd_info(DRV, "codec %u: no answer after register reset\n",
			 c->num);
		return rc;
	}
	c->reg_valid = 0;

	/* re-read what the reset may have changed */
	c->caps = rdv(c, AC97_RESET);
	c->ext_id = rdv(c, AC97_EXTENDED_ID);
	if (c->ext_id == 0xffff)
		c->ext_id = 0;
	if (c->revision < 2)
		c->revision =
			(c->ext_id & AC97_EI_REV_MASK) >> AC97_EI_REV_SHIFT;

	vendor_fixups_early(c);
	power_up(c);
	vendor_fixups_late(c);

	enable_vra(c);
	detect_rates(c);
	detect_mixer(c);

	set_defaults(c);
	apply_all(c);
	c->ready = 1;
	ac97_codec_capture_power(c, 1);

	snd_info(DRV,
		 "codec %u: %s (id %08x, caps 0x%04x, ext 0x%04x, AC'97 2.%d)"
		 "%s%s%s\n",
		 c->num, c->name, c->id, c->caps, c->ext_id,
		 c->revision == 0 ? 1 : (c->revision == 1 ? 2 : 3),
		 c->vra ? ", VRA" : ", fixed 48 kHz",
		 c->hw_hp.present ? ", headphone" : "",
		 (c->quirks & AC97_Q_INV_EAPD) ? ", inverted EAPD" : "");
	snd_info(DRV,
		 "codec %u: master %u-bit, pcm %u-bit, playback rates 0x%x, "
		 "capture rates 0x%x\n",
		 c->num, c->hw_master.max == 63 ? 6u : 5u,
		 c->hw_pcm.max == 63 ? 6u : 5u, c->rates[0], c->rates[1]);
	return 0;
}

int ac97_codec_restore(struct ac97_codec *c)
{
	uint64_t fl;
	int dir;

	if (!c || !c->ready)
		return -EINVAL;
	/* The power-up wait can take a while; do it without the lock
	 * (callers serialise restore against the control paths). */
	vendor_fixups_early(c);
	power_up(c);
	vendor_fixups_late(c);
	enable_vra(c);

	spin_lock_irqsave(&c->lock, &fl);
	if (c->vra)
		for (dir = 0; dir < 2; dir++)
			wr(c, rate_reg(dir),
			   (uint16_t)rate_to_reg(c, c->cur_rate[dir]));
	apply_all(c);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

void ac97_codec_shutdown(struct ac97_codec *c)
{
	uint64_t fl;
	uint16_t pd;

	if (!c || !c->ready)
		return;
	spin_lock_irqsave(&c->lock, &fl);
	/* mute first, then power down: avoids a thump on the outputs */
	wr(c, AC97_MASTER, AC97_MUTE | 0x1f1f);
	if (c->hw_hp.present)
		wr(c, AC97_HEADPHONE, AC97_MUTE | 0x1f1f);
	if (c->hw_mono.present)
		wr(c, AC97_MASTER_MONO, AC97_MUTE | 0x001f);
	pd = cached(c, AC97_POWERDOWN);
	pd &= ~AC97_PD_EAPD;
	if (!(c->quirks & AC97_Q_NO_EAPD))
		pd |= eapd_bits(c, 0);
	pd |= AC97_PD_PR0 | AC97_PD_PR1 | AC97_PD_PR6;
	wr(c, AC97_POWERDOWN, pd);
	snd_udelay(100);
	pd |= AC97_PD_PR2;
	wr(c, AC97_POWERDOWN, pd);
	c->ready = 0;
	spin_unlock_irqrestore(&c->lock, fl);
}

/* ---- controls ------------------------------------------------------------- */

static int ctl_valid_level(uint32_t l, uint32_t r)
{
	return l <= 100 && r <= 100;
}

/* Which state a control id refers to.  Returns 0 or -ENOENT. */
static int ctl_target(struct ac97_codec *c, uint32_t ctl,
		      struct ac97_level **lvl, const struct ac97_vol **hw)
{
	switch (ctl) {
	case AUDIO_CTL_MASTER:
		*lvl = &c->master;
		*hw = vol_for_reg(c, c->out_reg[AC97_OUT_MAIN]);
		return 0;
	case AUDIO_CTL_SPEAKER:
	case AUDIO_CTL_LINE:
		/* the main output is both the speaker feed and line out */
		*lvl = &c->out[AC97_OUT_MAIN];
		*hw = vol_for_reg(c, c->out_reg[AC97_OUT_MAIN]);
		return 0;
	case AUDIO_CTL_HEADPHONE:
		if (c->num_outputs < 2)
			return -ENOENT;
		*lvl = &c->out[AC97_OUT_HP];
		*hw = vol_for_reg(c, c->out_reg[AC97_OUT_HP]);
		return 0;
	case AUDIO_CTL_PCM:
		*lvl = &c->pcm;
		*hw = &c->hw_pcm;
		return 0;
	case AUDIO_CTL_CAPTURE:
		if (!c->hw_recgain.present)
			return -ENOENT;
		*lvl = &c->capture;
		*hw = &c->hw_recgain;
		return 0;
	default:
		return -ENOENT;
	}
}

int ac97_codec_get_volume(struct ac97_codec *c, uint32_t ctl,
			  struct audio_volume *v)
{
	struct ac97_level *lvl;
	const struct ac97_vol *hw;
	uint64_t fl;

	if (!c || !v || !c->ready)
		return -ENODEV;
	kmemset(v, 0, sizeof(*v));
	v->control = ctl;

	spin_lock_irqsave(&c->lock, &fl);
	if (ctl == AUDIO_CTL_MIC_BOOST) {
		if (!c->hw_mic.present) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -ENOENT;
		}
		v->left = v->right = c->mic_boost ? 100 : 0;
		v->db_left = v->db_right = c->mic_boost ? 2000 : 0;
		v->db_min = 0;
		v->db_max = 2000;
		v->steps = 1;
		spin_unlock_irqrestore(&c->lock, fl);
		return 0;
	}
	if (ctl_target(c, ctl, &lvl, &hw) || !hw || !hw->present) {
		spin_unlock_irqrestore(&c->lock, fl);
		return -ENOENT;
	}
	v->left = lvl->left;
	v->right = lvl->right;
	v->db_left = field_to_db(hw, pct_to_field(hw, lvl->left));
	v->db_right = field_to_db(hw, pct_to_field(hw, lvl->right));
	v->db_min = vol_db_min(hw);
	v->db_max = vol_db_max(hw);
	v->steps = vol_steps(hw);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_set_volume(struct ac97_codec *c, uint32_t ctl, uint32_t left,
			  uint32_t right)
{
	struct ac97_level *lvl;
	const struct ac97_vol *hw;
	uint64_t fl;

	if (!c || !c->ready)
		return -ENODEV;
	if (!ctl_valid_level(left, right))
		return -EINVAL;

	spin_lock_irqsave(&c->lock, &fl);
	if (ctl == AUDIO_CTL_MIC_BOOST) {
		if (!c->hw_mic.present) {
			spin_unlock_irqrestore(&c->lock, fl);
			return -ENOENT;
		}
		c->mic_boost = (left + right) / 2 >= 50 ? 100 : 0;
		apply_capture(c);
		spin_unlock_irqrestore(&c->lock, fl);
		return 0;
	}
	if (ctl_target(c, ctl, &lvl, &hw) || !hw || !hw->present) {
		spin_unlock_irqrestore(&c->lock, fl);
		return -ENOENT;
	}
	lvl->left = left;
	lvl->right = right;
	switch (ctl) {
	case AUDIO_CTL_PCM:
		apply_pcm(c);
		break;
	case AUDIO_CTL_CAPTURE:
		apply_capture(c);
		break;
	default:
		apply_outputs(c);
		break;
	}
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_get_mute(struct ac97_codec *c, uint32_t ctl, uint32_t *mute)
{
	struct ac97_level *lvl;
	const struct ac97_vol *hw;
	uint64_t fl;

	if (!c || !mute || !c->ready)
		return -ENODEV;
	spin_lock_irqsave(&c->lock, &fl);
	if (ctl_target(c, ctl, &lvl, &hw) || !hw || !hw->present) {
		spin_unlock_irqrestore(&c->lock, fl);
		return -ENOENT;
	}
	*mute = lvl->mute ? 1 : 0;
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_set_mute(struct ac97_codec *c, uint32_t ctl, uint32_t mute)
{
	struct ac97_level *lvl;
	const struct ac97_vol *hw;
	uint64_t fl;

	if (!c || !c->ready)
		return -ENODEV;
	if (mute > 1)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, &fl);
	if (ctl_target(c, ctl, &lvl, &hw) || !hw || !hw->present) {
		spin_unlock_irqrestore(&c->lock, fl);
		return -ENOENT;
	}
	lvl->mute = mute;
	switch (ctl) {
	case AUDIO_CTL_PCM:
		apply_pcm(c);
		break;
	case AUDIO_CTL_CAPTURE:
		apply_capture(c);
		break;
	default:
		apply_outputs(c);
		break;
	}
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

static void fill_out_desc(struct audio_output_desc *d, uint32_t id,
			  uint32_t type, const char *name, uint32_t active)
{
	kmemset(d, 0, sizeof(*d));
	d->id = id;
	d->type = type;
	d->flags = 0; /* no jack sensing */
	d->present = 1;
	d->active = active;
	kstrncpy(d->name, name, sizeof(d->name) - 1);
	d->name[sizeof(d->name) - 1] = 0;
}

int ac97_codec_get_outputs(struct ac97_codec *c, struct audio_outputs *o)
{
	uint64_t fl;

	if (!c || !o || !c->ready)
		return -ENODEV;
	kmemset(o, 0, sizeof(*o));
	spin_lock_irqsave(&c->lock, &fl);
	o->mode = c->out_mode;
	o->selected = c->out_selected;
	fill_out_desc(&o->out[0], AC97_OUT_ID_MAIN, AUDIO_OUT_SPEAKER,
		      c->num_outputs == 1 && (c->quirks & AC97_Q_HP_ONLY)
			      ? "Line Out"
			      : "Speaker/Line Out",
		      (uint32_t)output_active(c, AC97_OUT_MAIN));
	o->count = 1;
	if (c->num_outputs > 1) {
		fill_out_desc(&o->out[1], AC97_OUT_ID_HP, AUDIO_OUT_HEADPHONE,
			      "Headphone",
			      (uint32_t)output_active(c, AC97_OUT_HP));
		o->count = 2;
	}
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_set_output(struct ac97_codec *c,
			  const struct audio_setoutput *s)
{
	uint64_t fl;

	if (!c || !s || !c->ready)
		return -ENODEV;
	if (s->mode != AUDIO_OUTSEL_AUTO && s->mode != AUDIO_OUTSEL_ID &&
	    s->mode != AUDIO_OUTSEL_ALL)
		return -EINVAL;
	if (s->mode == AUDIO_OUTSEL_ID &&
	    (s->id < 1 || s->id > (uint32_t)c->num_outputs))
		return -EINVAL;

	spin_lock_irqsave(&c->lock, &fl);
	c->out_mode = s->mode;
	if (s->mode == AUDIO_OUTSEL_ID)
		c->out_selected = s->id;
	apply_outputs(c);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_get_inputs(struct ac97_codec *c, struct audio_outputs *o)
{
	uint64_t fl;
	uint32_t i, n = 0;

	if (!c || !o || !c->ready)
		return -ENODEV;
	kmemset(o, 0, sizeof(*o));
	spin_lock_irqsave(&c->lock, &fl);
	for (i = 0; i < AC97_REC_NSOURCES && n < AUDIO_MAX_OUTPUTS; i++) {
		struct audio_output_desc *d;
		if (!(c->rec_sources & (1u << i)))
			continue;
		d = &o->out[n++];
		/* there is no input type set in the interface: the type
		 * stays 0 and the name says what the source is */
		fill_out_desc(d, i + 1, 0, ac97_rec_names[i],
			      c->rec_source == i);
	}
	o->count = n;
	o->mode = c->rec_source == AC97_REC_MIC ? AUDIO_OUTSEL_AUTO
						: AUDIO_OUTSEL_ID;
	o->selected = c->rec_source + 1;
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

int ac97_codec_set_input(struct ac97_codec *c, const struct audio_setoutput *s)
{
	uint64_t fl;
	uint32_t src;

	if (!c || !s || !c->ready)
		return -ENODEV;
	if (s->mode == AUDIO_OUTSEL_AUTO) {
		src = AC97_REC_MIC;
	} else if (s->mode == AUDIO_OUTSEL_ID) {
		if (s->id < 1 || s->id > AC97_REC_NSOURCES)
			return -EINVAL;
		src = s->id - 1;
		if (!(c->rec_sources & (1u << src)))
			return -EINVAL;
	} else {
		/* one ADC: a single source at a time */
		return -EINVAL;
	}
	spin_lock_irqsave(&c->lock, &fl);
	c->rec_source = src;
	apply_capture(c);
	spin_unlock_irqrestore(&c->lock, fl);
	return 0;
}

/* ---- diagnostics ---------------------------------------------------------- */

static void diag_rates(struct ac97_dbuf *d, uint32_t mask)
{
	unsigned int i;
	int first = 1;

	for (i = 0; i < AC97_NUM_STD_RATES; i++) {
		if (!(mask & audio_rate_bit(ac97_std_rates[i])))
			continue;
		dbuf_printf(d, "%s%u", first ? "" : " ", ac97_std_rates[i]);
		first = 0;
	}
	if (first)
		dbuf_printf(d, "none");
}

long ac97_codec_diag(struct ac97_codec *c, int full, char *buf, size_t len)
{
	struct ac97_dbuf d = { buf, len, 0 };
	uint64_t fl;
	int i;

	if (!c || !buf || len == 0)
		return 0;
	buf[0] = 0;
	dbuf_printf(&d, "Codec %u: %s\n", c->num, c->name);
	dbuf_printf(&d, "  id %08x  caps 0x%04x  ext id 0x%04x  AC'97 2.%d\n",
		    c->id, c->caps, c->ext_id,
		    c->revision == 0 ? 1 : (c->revision == 1 ? 2 : 3));
	dbuf_printf(&d, "  quirks 0x%x  clock %u Hz  %s\n", c->quirks,
		    c->clock, c->vra ? "variable rate" : "fixed 48 kHz");
	dbuf_printf(&d, "  playback rates: ");
	diag_rates(&d, c->rates[0]);
	dbuf_printf(&d, " (now %u)\n  capture rates: ", c->cur_rate[0]);
	diag_rates(&d, c->rates[1]);
	dbuf_printf(&d, " (now %u)\n", c->cur_rate[1]);
	dbuf_printf(&d,
		    "  master %u/%u%s  pcm %u/%u%s  capture %u/%u%s  "
		    "mic boost %s\n",
		    c->master.left, c->master.right,
		    c->master.mute ? " muted" : "", c->pcm.left, c->pcm.right,
		    c->pcm.mute ? " muted" : "", c->capture.left,
		    c->capture.right, c->capture.mute ? " muted" : "",
		    c->mic_boost ? "on" : "off");
	for (i = 0; i < c->num_outputs; i++)
		dbuf_printf(&d,
			    "  output %d (%s): reg 0x%02x%s, level %u/%u%s, "
			    "%s\n",
			    i + 1, i == AC97_OUT_MAIN ? "main" : "headphone",
			    c->out_reg[i],
			    c->out_reg2[i] ? " +master" : "", c->out[i].left,
			    c->out[i].right, c->out[i].mute ? " muted" : "",
			    output_active(c, i) ? "active" : "off");
	dbuf_printf(&d, "  output mode %u, EAPD %s, record source %s\n",
		    c->out_mode, c->eapd_on ? "on" : "off",
		    c->rec_source < AC97_REC_NSOURCES
			    ? ac97_rec_names[c->rec_source]
			    : "?");
	if (full) {
		dbuf_printf(&d, "  registers:\n");
		for (i = 0; i < AC97_NUM_REGS; i++) {
			uint16_t v = 0xffff;
			if (c->ready) {
				spin_lock_irqsave(&c->lock, &fl);
				rd(c, (unsigned int)(i * 2), &v);
				spin_unlock_irqrestore(&c->lock, fl);
			}
			dbuf_printf(&d, "%s%02x:%04x", (i % 8) ? " " : "    ",
				    i * 2, v);
			if ((i % 8) == 7)
				dbuf_printf(&d, "\n");
		}
	}
	return (long)d.pos;
}
