// LikeOS SoundWire - private interfaces between the machine discovery
// (sof_acpi.c), the Intel SoundWire master (sdw_intel.c), the bus core
// (sdw_bus.c) and the codec drivers (sdw_cs42l43.c, sdca.c).
//
// Layering:
//
//   sof_acpi.c   reads the firmware tables: NHLT, the DSDT SoundWire
//                controller ("SNDW", _ADR 0x40000000) below the HD Audio
//                device, its link and peripheral descriptions (MIPI DisCo
//                _DSD properties) and chooses firmware/topology names.
//   sdw_intel.c  the Intel glue (SHIM, ALH, link power through the SHIM or
//                the HDA multi-link capability) and the Cadence master IP
//                of every link: command FIFO, interrupts, data ports.
//   sdw_bus.c    protocol logic: register access with paging, enumeration,
//                device number assignment, peripheral interrupts, frame
//                shape and bandwidth, stream prepare/enable/release with
//                bank switching, and the card-level codec controls.
//   sdw_cs42l43.c, sdca.c
//                codec drivers behind struct sdw_codec_ops.
//
// Context rules: register access (sdw_read & co.) never sleeps and may be
// used from any context except hard interrupt handlers; it busy-polls the
// command FIFO for at most a few milliseconds.  Enumeration, codec
// initialisation and everything that waits for hardware longer than that
// runs in process context (the sound work thread or an ioctl).
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SDW_H
#define KERNEL_DEV_SOUND_SDW_H

#include "sof.h"
#include "snd_internal.h"

/* ==========================================================================
 * Limits
 * ========================================================================== */

#define SDW_MAX_LINKS SOF_MAX_SDW_LINKS
#define SDW_MAX_DEVNUM 11 /* device numbers 1..11 */
#define SDW_ENUM_DEVNUM 0 /* unenumerated devices answer here */
#define SDW_BCAST_DEVNUM 15
#define SDW_MAX_PORTS 15 /* data ports 0..14 */
#define SDW_MAX_PDI 16
#define SDW_MAX_CLK_FREQS 8
#define SDW_STREAM_MAX_SLAVES 4 /* peripherals in one stream on a link */
#define SDW_LINK_STREAMS 4 /* concurrent streams per link */
#define SDW_MAX_FUNCTIONS 8 /* SDCA functions per peripheral */

/* ==========================================================================
 * MIPI SoundWire register map (peripheral side)
 * ========================================================================== */

#define SDW_R_NO_PAGE_LIMIT 0x00008000u /* below: never paged */
#define SDW_R_OPTIONAL_PAGE 0x00010000u /* below: paged only if supported */
#define SDW_R_MAX 0x48000000u
#define SDW_R_PAGE1(a) (((a) >> 23) & 0xffu)
#define SDW_R_PAGE2(a) (((a) >> 15) & 0xffu)

#define SDW_DPN_SIZE 0x100u
#define SDW_BANK1_OFFSET 0x10u

/* DP0 */
#define SDW_DP0_INT 0x00
#define SDW_DP0_INTMASK 0x01
#define SDW_DP0_PORTCTRL 0x02
#define SDW_DP0_INT_TEST_FAIL 0x01
#define SDW_DP0_INT_PORT_READY 0x02
#define SDW_DP0_INT_BRA_FAILURE 0x04
#define SDW_DP0_SDCA_CASCADE 0x08
#define SDW_DP0_INT_IMPDEF 0xe0

/* SCP */
#define SDW_SCP_INT1 0x40
#define SDW_SCP_INTMASK1 0x41
#define SDW_SCP_INT1_PARITY 0x01
#define SDW_SCP_INT1_BUS_CLASH 0x02
#define SDW_SCP_INT1_IMPL_DEF 0x04
#define SDW_SCP_INT1_PORT0_3 0x78
#define SDW_SCP_INT1_SCP2_CASCADE 0x80
#define SDW_SCP_INTSTAT2 0x42
#define SDW_SCP_INTSTAT2_PORT4_10 0x7f
#define SDW_SCP_INTSTAT2_SCP3_CASCADE 0x80
#define SDW_SCP_INTSTAT3 0x43
#define SDW_SCP_INTSTAT3_PORT11_14 0x0f
#define SDW_SCP_CTRL 0x44
#define SDW_SCP_STAT 0x44
#define SDW_SCP_CTRL_CLK_STP_NOW 0x02
#define SDW_SCP_CTRL_FORCE_RESET 0x80
#define SDW_SCP_STAT_CURR_BANK 0x40
#define SDW_SCP_SYSTEMCTRL 0x45
#define SDW_SCP_DEVNUMBER 0x46
#define SDW_SCP_ADDRPAGE1 0x48
#define SDW_SCP_ADDRPAGE2 0x49
#define SDW_SCP_KEEPEREN 0x4a
#define SDW_SCP_BANKDELAY 0x4b
#define SDW_SCP_COMMIT 0x4c
#define SDW_SCP_BUS_CLOCK_BASE 0x4d
#define SDW_SCP_DEVID_0 0x50
#define SDW_SCP_SDCA_INT1 0x58
#define SDW_SCP_SDCA_INTMASK1 0x5c
#define SDW_SCP_FRAMECTRL_B0 0x60
#define SDW_SCP_FRAMECTRL_B1 0x70
#define SDW_SCP_NEXTFRAME_B0 0x61
#define SDW_SCP_BUSCLOCK_SCALE_B0 0x62
#define SDW_SCP_BUSCLOCK_SCALE_B1 0x72

/* SCP_BUS_CLOCK_BASE encodings */
#define SDW_CLK_BASE_19200000 1
#define SDW_CLK_BASE_24000000 2
#define SDW_CLK_BASE_24576000 3
#define SDW_CLK_BASE_22579200 4
#define SDW_CLK_BASE_32000000 5

/* DPn (n = 1..14) */
#define SDW_DPN_INT(n) (0x00u + SDW_DPN_SIZE * (n))
#define SDW_DPN_INTMASK(n) (0x01u + SDW_DPN_SIZE * (n))
#define SDW_DPN_PORTCTRL(n) (0x02u + SDW_DPN_SIZE * (n))
#define SDW_DPN_BLOCKCTRL1(n) (0x03u + SDW_DPN_SIZE * (n))
#define SDW_DPN_PREPARESTATUS(n) (0x04u + SDW_DPN_SIZE * (n))
#define SDW_DPN_PREPARECTRL(n) (0x05u + SDW_DPN_SIZE * (n))
/* banked: add SDW_BANK1_OFFSET for bank 1 */
#define SDW_DPN_CHANNELEN(n) (0x20u + SDW_DPN_SIZE * (n))
#define SDW_DPN_BLOCKCTRL2(n) (0x21u + SDW_DPN_SIZE * (n))
#define SDW_DPN_SAMPLECTRL1(n) (0x22u + SDW_DPN_SIZE * (n))
#define SDW_DPN_SAMPLECTRL2(n) (0x23u + SDW_DPN_SIZE * (n))
#define SDW_DPN_OFFSETCTRL1(n) (0x24u + SDW_DPN_SIZE * (n))
#define SDW_DPN_OFFSETCTRL2(n) (0x25u + SDW_DPN_SIZE * (n))
#define SDW_DPN_HCTRL(n) (0x26u + SDW_DPN_SIZE * (n))
#define SDW_DPN_BLOCKCTRL3(n) (0x27u + SDW_DPN_SIZE * (n))
#define SDW_DPN_LANECTRL(n) (0x28u + SDW_DPN_SIZE * (n))
#define SDW_DPN_INT_TEST_FAIL 0x01
#define SDW_DPN_INT_PORT_READY 0x02
#define SDW_DPN_INT_IMPDEF 0xe0

/* SDCA control address: function, entity, control selector, channel */
#define SDW_SDCA_CTL(fun, ent, ctl, ch)                                 \
	((1u << 30) | (((uint32_t)(fun) & 0x7u) << 22) |                \
	 (((uint32_t)(ent) & 0x40u) << 15) |                            \
	 (((uint32_t)(ent) & 0x3fu) << 7) |                             \
	 (((uint32_t)(ctl) & 0x30u) << 15) |                            \
	 (((uint32_t)(ctl) & 0x0fu) << 3) |                             \
	 (((uint32_t)(ch) & 0x38u) << 12) | ((uint32_t)(ch) & 0x07u))
#define SDW_SDCA_MBQ(reg) ((reg) | (1u << 13))
#define SDW_SDCA_NEXT(reg) ((reg) | (1u << 14))

/* ==========================================================================
 * Firmware (MIPI DisCo) descriptions, filled by sof_acpi.c
 * ========================================================================== */

/* One link (master) as the controller's _DSD describes it. */
struct sdw_master_props {
	int valid;
	uint32_t ip_clock; /* "intel-sdw-ip-clock" as found (2x) */
	uint32_t mclk_freq; /* the master clock the divider uses */
	uint32_t max_clk_freq; /* bus clock, Hz */
	uint32_t clk_freq[SDW_MAX_CLK_FREQS];
	int nclk_freq;
	uint32_t clk_gears[SDW_MAX_CLK_FREQS];
	int nclk_gears;
	uint32_t default_frame_rate;
	uint32_t default_row, default_col;
	uint32_t quirk_mask; /* "intel-quirk-mask" */
	int clk_stop_mode0, clk_stop_mode1;
	int dynamic_frame;
	uint32_t err_threshold;
	/* Intel timing knobs (ACE2+) */
	uint16_t doaise, doais, dodse, dods;
	uint16_t clde, doaise2, dodse2, clds, clss;
};

#define SDW_INTEL_QUIRK_BUS_DISABLE (1u << 1)

/* Data port types */
#define SDW_DPN_FULL 0
#define SDW_DPN_SIMPLE 1
#define SDW_DPN_REDUCED 2

struct sdw_dpn_props {
	uint8_t valid;
	uint8_t source; /* 1: peripheral -> master (capture) */
	uint8_t type; /* SDW_DPN_* */
	uint8_t simple_ch_prep_sm;
	uint32_t ch_prep_timeout_ms;
	uint32_t max_ch;
	uint32_t min_word, max_word;
	uint32_t imp_def_interrupts;
	uint8_t read_only_wordlength;
	uint8_t lane_control;
};

struct sdw_slave_props {
	int valid; /* from firmware (else defaults) */
	int paging;
	int clock_reg; /* bus clock base/scale registers */
	int lane_control;
	int wake_capable;
	int simple_clk_stop;
	int high_phy;
	uint32_t sdw_rev, sdca_rev;
	uint32_t source_ports, sink_ports; /* bit n = port n */
	uint32_t scp_int1_mask;
	int invalid_initial_parity; /* ignore the first parity report */
	struct sdw_dpn_props dp[SDW_MAX_PORTS];
};

/* ---- _DSD property access (sof_acpi.c) -------------------------------- */

/* An open property set: a device's _DSD, or a data-only child node that a
 * hierarchical _DSD link points to.  Opaque outside sof_acpi.c. */
struct sdw_dsd;

/* `handle' is an ACPI_HANDLE.  NULL when the object has no _DSD. */
struct sdw_dsd *sdw_dsd_open(void *handle);
/* The child node `name' links to ("mipi-sdw-link-0-subproperties"). */
struct sdw_dsd *sdw_dsd_child(struct sdw_dsd *d, const char *name);
void sdw_dsd_close(struct sdw_dsd *d);
/* 0 or -ENOENT/-EINVAL.  Integers accept Integer objects; arrays accept
 * packages of integers and (for u8) buffers. */
int sdw_dsd_u32(struct sdw_dsd *d, const char *name, uint32_t *out);
int sdw_dsd_u64(struct sdw_dsd *d, const char *name, uint64_t *out);
int sdw_dsd_bool(struct sdw_dsd *d, const char *name);
/* Element count copied (<= max), or -errno; *total gets the real count. */
int sdw_dsd_u32_array(struct sdw_dsd *d, const char *name, uint32_t *out,
		      int max, int *total);
int sdw_dsd_u8_array(struct sdw_dsd *d, const char *name, uint8_t *out,
		     int max, int *total);
int sdw_dsd_string(struct sdw_dsd *d, const char *name, char *out,
		   size_t len);
/* The name of the node a hierarchical link `name' points to (for
 * identifying the same node reached through different links). */
int sdw_dsd_link_target(struct sdw_dsd *d, const char *name, char *out,
			size_t len);

/* SDCA function type (1 SmartAmp .. 8 SimpleJack, 0x0a HID, ...) of a
 * function node, or -errno; `if_rev' the peripheral's
 * mipi-sdw-sdca-interface-revision (older numbering translated). */
int sdw_sdca_function_type(void *fn_handle, uint32_t if_rev);

/* Device children of an ACPI object with their _ADR (depth 1). */
int sdw_acpi_children(void *handle, void **child, uint64_t *adr, int max);

/* ---- what sof_acpi.c learned, beyond struct sof_machine --------------- */

void *sof_acpi_controller_handle(struct sof_dev *sdev); /* "SNDW" */
void *sof_acpi_periph_handle(struct sof_dev *sdev, int periph_idx);
int sof_acpi_master_props(struct sof_dev *sdev, int link,
			  struct sdw_master_props *out);
int sof_acpi_slave_props(struct sof_dev *sdev, int periph_idx,
			 struct sdw_slave_props *out);
/* Further topology files to try, in order, when tplg_name is missing
 * (idx 0..n-1; NULL past the end).  Includes the per-function topologies
 * and the generic HDA-only fallback. */
const char *sof_acpi_tplg_candidate(struct sof_dev *sdev, int idx);
/* The topology directory chosen for this platform ("intel/sof-ipc4-tplg"). */
const char *sof_acpi_tplg_dir(struct sof_dev *sdev);
/* Nonzero when the machine topology is loaded before the per-function
 * topologies (boards whose machine description says so). */
int sof_acpi_machine_tplg_first(struct sof_dev *sdev);

/* ==========================================================================
 * Runtime model
 * ========================================================================== */

/* peripheral (slave) status as the master reports it */
#define SDW_SLAVE_UNATTACHED 0
#define SDW_SLAVE_ATTACHED 1
#define SDW_SLAVE_ALERT 2
#define SDW_SLAVE_RESERVED 3

/* command results */
#define SDW_CMD_OK 0
#define SDW_CMD_IGNORED 1 /* nobody acknowledged */
#define SDW_CMD_FAIL 2 /* NAK */
#define SDW_CMD_TIMEOUT 3 /* the master never answered */

/* stream states */
#define SDW_STREAM_IDLE 0
#define SDW_STREAM_CONFIGURED 1
#define SDW_STREAM_PREPARED 2
#define SDW_STREAM_ENABLED 3
#define SDW_STREAM_DISABLED 4

/* codec capabilities (struct sdw_slave.caps) */
#define SDW_CAP_SPEAKER (1u << 0) /* drives the built-in speakers */
#define SDW_CAP_HEADPHONE (1u << 1) /* headphone/line output on a jack */
#define SDW_CAP_JACK_DETECT (1u << 2) /* reports jack insertion */
#define SDW_CAP_HEADSET_MIC (1u << 3)
#define SDW_CAP_DMIC (1u << 4)
#define SDW_CAP_PLAYBACK (1u << 5) /* takes a playback stream */
#define SDW_CAP_CAPTURE (1u << 6) /* gives a capture stream */

/* jack bits for sdw_report_jack() */
#define SDW_JACK_HEADPHONE (1u << 0)
#define SDW_JACK_MIC (1u << 1)
#define SDW_JACK_LINEOUT (1u << 2)
#define SDW_JACK_MECHANICAL (1u << 3)

/* the output ids the SoundWire side publishes (audio_output_desc.id) */
#define SDW_OUT_ID_SPEAKER SOF_OUT_ID_SPEAKER
#define SDW_OUT_ID_HEADPHONE SOF_OUT_ID_HEADPHONE

struct sdw_link;
struct sdw_slave;
struct sdw_ctx;

struct sdw_codec_ops {
	const char *name;
	/* 1 if this driver handles the peripheral */
	int (*match)(const struct sof_sdw_peripheral *p);
	/* Allocate state, set s->caps and adjust s->props.  No bus I/O. */
	int (*probe)(struct sdw_slave *s);
	/* The device is attached and has its device number: bring it up.
	 * Process context, may sleep (bounded). */
	int (*init)(struct sdw_slave *s);
	/* The device fell off the bus (no I/O possible). */
	void (*detached)(struct sdw_slave *s);
	void (*remove)(struct sdw_slave *s);
	/* The data port a stream of `dir' (AUDIO_DIR_*) for audio function
	 * `fn' (SOF_SDW_FN_*, 0 = unknown) uses, or -ENOENT. */
	int (*port_for)(struct sdw_slave *s, int dir, int fn);
	/* Bus clock changed (Hz, the clock, not the double rate). */
	int (*bus_config)(struct sdw_slave *s, uint32_t bus_freq);
	/* Stream life cycle; prepare/release in process context. */
	int (*prepare)(struct sdw_slave *s, int dir, int port, uint32_t rate,
		       uint32_t ch, uint32_t bits);
	/* May be called with interrupts off: short register I/O only. */
	int (*trigger)(struct sdw_slave *s, int dir, int port, int on);
	int (*release)(struct sdw_slave *s, int dir, int port);
	/* Peripheral alert: implementation-defined (SCP_INT1 bit 2) and SDCA
	 * interrupt status bytes.  Called from the bus service with the link
	 * serviced; must not sleep long and must not take the codec lock if
	 * init may be waiting for the bus (schedule work instead). */
	void (*alert)(struct sdw_slave *s, uint8_t scp_int1,
		      const uint8_t sdca_int[4]);
	/* Periodic (about once a second): jack polling and health checks. */
	void (*poll)(struct sdw_slave *s);
	/* Which outputs should sound now. */
	int (*set_routing)(struct sdw_slave *s, int speaker_on, int hp_on);
	/* AUDIO_CTL_* volume 0..100 */
	int (*set_volume)(struct sdw_slave *s, uint32_t ctl, uint32_t l,
			  uint32_t r);
	int (*get_volume)(struct sdw_slave *s, uint32_t ctl,
			  struct audio_volume *v);
	int (*set_mute)(struct sdw_slave *s, uint32_t ctl, uint32_t mute);
	long (*dump)(struct sdw_slave *s, char *buf, size_t len);
};

struct sdw_port_rt {
	int num; /* data port number */
	uint32_t ch_mask;
	uint32_t bps; /* bits per sample on the bus */
	uint32_t sample_interval;
	uint32_t offset1, offset2;
	uint32_t hstart, hstop;
	uint32_t lane;
	uint32_t blk_pkg_mode, blk_grp_ctrl;
};

struct sdw_stream_rt {
	int state; /* SDW_STREAM_* */
	int dir; /* AUDIO_DIR_* */
	int fn; /* SOF_SDW_FN_* it was prepared for, 0 = unknown */
	uint32_t rate, ch, bits, bps;
	int pdi; /* the master PDI (== master data port) */
	int alh_id; /* the DSP's gateway id for this PDI */
	int stream_tag; /* ACE2+: HDA link DMA stream tag, 0 = unknown */
	struct sdw_port_rt mport;
	int nslave;
	struct {
		struct sdw_slave *s;
		struct sdw_port_rt port;
	} sl[SDW_STREAM_MAX_SLAVES];
	/* trigger() requested while the bus was busy: done from work */
	volatile int deferred_on; /* -1 none, 0 off, 1 on */
	uint32_t generation;
};

struct sdw_bus_params {
	uint32_t max_dr_freq; /* double rate: 2x bus clock */
	uint32_t curr_dr_freq;
	uint32_t row, col;
	uint32_t bandwidth; /* bits per second the streams use */
	int curr_bank, next_bank;
};

struct sdw_slave {
	struct sdw_link *link;
	struct sof_sdw_peripheral *p; /* the machine description entry */
	int periph_idx; /* index in sdev->machine.periph */
	int dev_num; /* sticky device number, 0 = none yet */
	int status; /* SDW_SLAVE_* */
	int present; /* ever seen on the bus */
	volatile int in_init; /* codec init running */
	volatile int needs_init;
	int initialized;
	int init_errors;
	uint64_t id48; /* the DEVID registers as read */
	uint32_t caps; /* SDW_CAP_* */
	struct sdw_slave_props props;
	const struct sdw_codec_ops *ops;
	void *codec; /* codec private */
	uint32_t attach_count, alert_count, io_errors;
	uint8_t parity_reported;
	char name[24]; /* "cs42l43-l0-u0" */
};

struct sdw_link {
	struct sof_dev *sdev;
	struct sdw_ctx *ctx;
	int id;
	int present; /* in the firmware's mask */
	int enabled; /* hardware exists and we use it */
	int powered;
	int up; /* the Cadence IP runs, commands possible */
	int dead; /* stopped answering: no more I/O */
	int ace2; /* ACE2+: per-link SHIM2 through the HDA BAR */

	volatile uint8_t *regs; /* Cadence register block */
	uint32_t ip_offset; /* IP-specific registers inside it */
	volatile uint8_t *shim; /* cAVS/ACE1: shared SHIM; ACE2: SHIM2 */
	volatile uint8_t *shim_vs; /* ACE2 vendor-specific SHIM */
	volatile uint8_t *alh; /* cAVS/ACE1: shared ALH */

	struct sdw_master_props mprops;
	struct sdw_bus_params params;
	int multi_link;

	/* PDIs (master-side data ports) */
	int pcm_bd, pcm_in, pcm_out;
	uint32_t pdi_ch[SDW_MAX_PDI];
	uint32_t pdi_busy;

	struct sdw_slave slaves[SOF_MAX_SDW_PERIPHERALS];
	int nslaves;
	uint32_t devnum_used; /* bit n: device number n handed out */

	struct sdw_stream_rt streams[SDW_LINK_STREAMS];

	/* locking: see sdw_bus.c */
	spinlock_t bus_lock; /* one command FIFO transaction */
	spinlock_t stream_lock; /* short stream field updates */
	volatile int stream_busy; /* a stream sequence runs (see sdw_bus.c) */
	volatile int stream_work_queued;
	spinlock_t irq_lock; /* INTMASK read-modify-write */
	struct snd_mutex service_lock; /* enumeration / alert handling */
	volatile uint32_t irq_pending; /* slave status changed */
	volatile int work_queued;

	uint32_t msg_count; /* FIFOLEVEL as programmed */
	uint32_t last_slave_stat;

	/* statistics for sdw_dump */
	uint32_t st_cmds, st_ignored, st_nak, st_timeout;
	uint32_t st_irqs, st_parity, st_ctrl_clash, st_data_clash;
	uint32_t st_bank_switch, st_enum, st_unknown_dev, st_cmd_err;
	uint32_t consecutive_timeouts;
};

struct sdw_ctx {
	struct sof_dev *sdev;
	int platform; /* SOF_PLAT_* */
	int ace1, ace2, ace3; /* ACE generations (ace2 also set on ace3) */
	volatile uint8_t *dsp; /* BAR4 */
	volatile uint8_t *hda; /* BAR0 */
	uint32_t shim_off, alh_off; /* cAVS/ACE1 offsets in BAR4 */
	uint32_t hw_lcount;
	uint32_t link_mask; /* links we drive */
	int shim_up; /* shared SHIM link power done */
	struct sdw_link link[SDW_MAX_LINKS];

	/* HDA multi-link capability (ACE2+) */
	uint32_t ml_cap; /* offset of the ML capability in BAR0 */
	uint32_t ml_sdw; /* offset of the SoundWire extended link */
	uint32_t ml_lcap;
	uint32_t ml_base; /* LEPTR pointer: SoundWire block in BAR0 */
	int ml_slcount, ml_intc, ml_lss;

	/* card-level control state (sdw_bus.c) */
	struct snd_mutex ctl_lock;
	uint32_t out_mode; /* AUDIO_OUTSEL_* */
	uint32_t out_selected;
	uint32_t jack; /* SDW_JACK_* now */
	uint32_t vol[AUDIO_CTL_MAX + 1][2];
	uint32_t mute[AUDIO_CTL_MAX + 1];
	int spk_active, hp_active;

	volatile int routing_work_queued;
	volatile int stopping;
	volatile int poll_armed;
	uint32_t poll_count;
	int irq_enabled;
};

static inline struct sdw_ctx *sdw_ctx_of(struct sof_dev *sdev)
{
	return sdev ? (struct sdw_ctx *)sdev->sdw : NULL;
}

/* ==========================================================================
 * Master (sdw_intel.c) services used by the bus core
 * ========================================================================== */

/* One batch of up to 8 commands.  `read': fill buf, else send buf.  addr
 * is the 16-bit bus address (paging already applied).  When `page' is
 * set, ADDRPAGE1/2 are programmed first in the same transaction.
 * Returns SDW_CMD_*.  Caller holds link->bus_lock. */
int sdw_cdns_xfer(struct sdw_link *l, int dev_num, uint16_t addr, int read,
		  uint8_t *buf, int count, int ssp_sync, int page,
		  uint8_t page1, uint8_t page2);
uint32_t sdw_cdns_slave_stat(struct sdw_link *l);
void sdw_cdns_ack_slave_intstat(struct sdw_link *l, uint32_t *s0,
				uint32_t *s1);
void sdw_cdns_slave_irq_enable(struct sdw_link *l, int on);
/* master port programming, into register bank `bank' */
void sdw_cdns_dpn_params(struct sdw_link *l, int bank,
			 const struct sdw_port_rt *p);
void sdw_cdns_dpn_enable(struct sdw_link *l, int bank, int port,
			 uint32_t ch_mask);
void sdw_cdns_bus_conf(struct sdw_link *l, int bank, uint32_t curr_dr_freq);
int sdw_cdns_active_bank(struct sdw_link *l);
/* PDI <-> DSP gateway */
int sdw_intel_pdi_alloc(struct sdw_link *l, int dir, uint32_t ch, int *pdi,
			int *alh_id);
void sdw_intel_pdi_config(struct sdw_link *l, int pdi, int dir, uint32_t ch,
			  int alh_id, int stream_tag);
void sdw_intel_pdi_free(struct sdw_link *l, int pdi);
/* Multi-link synchronisation for a bank switch (no-op on one link). */
void sdw_intel_sync_arm(struct sdw_link *l);
int sdw_intel_sync_go(struct sdw_link *l);
const char *sdw_intel_gen_name(const struct sdw_ctx *c);
long sdw_intel_dump(struct sdw_ctx *c, char *buf, size_t len);
/* ACE2+: the DSP side allocated HDA link DMA `stream_tag' for the stream
 * of `dir' on `link'; maps the PDI to it (PCMSyCM). */
int sdw_intel_map_pdi_stream(struct sof_dev *sdev, int link, int dir,
			     int stream_tag);
/* PDI number from an ALH gateway id. */
int sdw_intel_alh_to_pdi(int link, int alh_id);

/* ==========================================================================
 * Bus core (sdw_bus.c)
 * ========================================================================== */

/* Register access; return 0 / value >= 0 or -errno (-ENODATA: nobody
 * answered, -EIO: NAK, -ETIMEDOUT, -ENODEV: link down). */
int sdw_read(struct sdw_slave *s, uint32_t addr);
int sdw_write(struct sdw_slave *s, uint32_t addr, uint8_t val);
int sdw_update(struct sdw_slave *s, uint32_t addr, uint8_t mask,
	       uint8_t val);
int sdw_nread(struct sdw_slave *s, uint32_t addr, size_t n, uint8_t *buf);
int sdw_nwrite(struct sdw_slave *s, uint32_t addr, size_t n,
	       const uint8_t *buf);
/* 32-bit little endian registers spread over 4 bus addresses */
int sdw_read32(struct sdw_slave *s, uint32_t addr, uint32_t *val);
int sdw_write32(struct sdw_slave *s, uint32_t addr, uint32_t val);
int sdw_update32(struct sdw_slave *s, uint32_t addr, uint32_t mask,
		 uint32_t val);
/* SDCA multi-byte-quantity controls (1..4 bytes) */
int sdw_mbq_read(struct sdw_slave *s, uint32_t addr, int bytes,
		 uint32_t *val);
int sdw_mbq_write(struct sdw_slave *s, uint32_t addr, int bytes,
		  uint32_t val);

/* Sleep (process context) or busy-wait (otherwise). */
void sdw_msleep(uint32_t ms);
int sdw_can_sleep(void);

/* Enumeration and status: handle what changed on a link.  Process
 * context. */
void sdw_bus_service(struct sdw_link *l);
/* Wait until the peripheral is attached (servicing the bus meanwhile). */
int sdw_wait_attached(struct sdw_slave *s, uint32_t timeout_ms);
/* Wait until it detached (after a soft reset). */
int sdw_wait_detached(struct sdw_slave *s, uint32_t timeout_ms);
/* Called by sdw_intel.c once a link runs. */
void sdw_bus_link_started(struct sdw_link *l);
/* Prepare the per-link slave table from sdev->machine. */
int sdw_bus_link_setup(struct sdw_link *l);
void sdw_bus_link_teardown(struct sdw_link *l);
/* Interrupt-side: slave status changed on `l' (schedules work). */
void sdw_bus_irq_notify(struct sdw_link *l);
/* Start the periodic service. */
void sdw_bus_start_polling(struct sdw_ctx *c);
/* A jack changed (from a codec). */
void sdw_report_jack(struct sdw_slave *s, uint32_t jack, uint32_t mask);
/* Initialise codecs that are attached but not yet set up. */
void sdw_bus_init_pending(struct sdw_ctx *c);

/* Frame shapes: the row and column counts the FRAMECTRL register can
 * encode, and the index lookups (-1 when not encodable). */
extern const uint16_t sdw_rows[24];
extern const uint8_t sdw_cols[8];
int sdw_find_row_index(uint32_t row);
int sdw_find_col_index(uint32_t col);

/* Codec drivers */
extern const struct sdw_codec_ops sdw_cs42l43_ops;
extern const struct sdw_codec_ops sdw_sdca_ops;
extern const struct sdw_codec_ops sdw_max98373_ops;
extern const struct sdw_codec_ops sdw_null_ops;

/* small helpers */
static inline uint32_t sdw_hweight32(uint32_t v)
{
	/* no popcount builtin: it would need a runtime library call */
	v = v - ((v >> 1) & 0x55555555u);
	v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
	v = (v + (v >> 4)) & 0x0f0f0f0fu;
	return (v * 0x01010101u) >> 24;
}

static inline uint32_t sdw_get_bits(uint32_t v, uint32_t mask)
{
	return mask ? (v & mask) >> __builtin_ctz(mask) : 0;
}

static inline uint32_t sdw_set_bits(uint32_t v, uint32_t mask, uint32_t f)
{
	if (!mask)
		return v;
	return (v & ~mask) | ((f << __builtin_ctz(mask)) & mask);
}

/* bounded text append for the dumps: returns the new position */
size_t sdw_dump_printf(char *buf, size_t len, size_t pos, const char *fmt,
		       ...) __attribute__((format(printf, 4, 5)));

#define sdw_info(fmt, ...) kprintf("[snd] sdw: " fmt, ##__VA_ARGS__)

#endif /* KERNEL_DEV_SOUND_SDW_H */
