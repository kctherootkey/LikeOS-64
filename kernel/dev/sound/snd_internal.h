// LikeOS sound core - private interfaces shared by snd_core.c, snd_pcm.c,
// snd_ctl.c and snd_oss.c, plus the few extra helpers a driver may use
// beyond <kernel/dev/sound/snd.h>.
//
// The public structures (struct snd_card, struct snd_pcm) are allocated by
// the core only (snd_card_new, snd_pcm_new), each embedded at the start of
// a larger core-private wrapper, so the core reaches its own bookkeeping
// from any card or substream pointer a driver hands it.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_DEV_SOUND_SND_INTERNAL_H
#define KERNEL_DEV_SOUND_SND_INTERNAL_H

#include <kernel/dev/sound/snd.h>
#include <kernel/uapi/soundcard.h>
#include <kernel/ke/hrtimer.h>

/* ---- driver extras -------------------------------------------------------- */


/* mmap() layout of a PCM node: offset 0 maps the ring of the handle's
 * primary direction (playback when the handle was opened for writing,
 * capture otherwise).  A handle opened O_RDWR on a full-duplex device maps
 * its capture ring at this offset. */
#define SND_MMAP_CAPTURE_OFFSET 0x10000000ULL

/* ---- limits ----------------------------------------------------------------- */

#define SND_RING_MAX_BYTES (16u * 1024u * 1024u) /* absolute cap per ring */
#define SND_DIAG_MAX_BYTES (64u * 1024u) /* AUDIO_DIAG bounce buffer */
#define SND_MAX_CHANNELS 32u
#define SND_WORK_SLOTS 128
#define SND_OSS_PARTIAL_MAX 128 /* > the largest frame (32 ch x 4 bytes) */

/* ---- a sleeping lock -------------------------------------------------------- */

/* Process-context mutual exclusion that may be held across sleeps (ring
 * allocation, driver hw_params, waiting for a drain).  Before the scheduler
 * runs there is only one thread, so it is never contended there. */
struct snd_mutex {
	spinlock_t lock;
	int locked;
	task_t *owner;
	struct wait_queue_head wq;
};

void snd_mutex_init(struct snd_mutex *m, const char *name);
/* 0, or -ERESTARTSYS when `interruptible' and a signal arrived first. */
int snd_mutex_lock(struct snd_mutex *m, int interruptible);
void snd_mutex_unlock(struct snd_mutex *m);

/* ---- waiting ---------------------------------------------------------------- */

/* Block the calling task on `wq' until cond(arg) is true.  The condition
 * is evaluated with the task already queued and marked blocked, so a wake
 * between the test and the sleep is never lost (the drm_read pattern).
 * timeout_ns 0 = no timeout.  Returns 0 when the condition held,
 * -ERESTARTSYS when `interruptible' and a signal is pending, -ETIMEDOUT. */
typedef int (*snd_cond_fn)(void *arg);
int snd_wait_event(struct wait_queue_head *wq, snd_cond_fn cond, void *arg,
		   uint64_t timeout_ns, int interruptible);

/* ---- the DMA ring --------------------------------------------------------- */

/* One PCM ring.  Reference counted: the substream holds one while it is
 * attached, and every user mapping holds one (taken in ->mmap and by the
 * address space for every split/forked copy of the mapping), so the pages
 * outlive a process that keeps them mapped after the stream changed. */
struct snd_ring {
	int refs;
	int maps; /* user mappings alive */
	uint64_t phys;
	uint8_t *virt; /* kernel mapping (UC unless coherent) */
	uint32_t pages;
	uint32_t bytes; /* usable bytes (the buffer) */
	int dma32;
	int coherent;
	int uc_mapped; /* virt came from mm_map_mmio_flags */
	void *heap; /* kalloc_dma fallback block, else NULL */
};

struct snd_ring *snd_ring_alloc(uint32_t bytes, uint32_t align, int dma32,
				int coherent);
void snd_ring_get(struct snd_ring *r);
/* Drop a reference; frees the memory at zero.  Process context. */
void snd_ring_put(struct snd_ring *r);
/* The same from a path that must not do the unmapping work itself (an
 * address space dropping a mapping): the final free runs on ksnd. */
void snd_ring_put_async(struct snd_ring *r);

/* ---- per-substream core state -------------------------------------------- */

#define SND_TIMER_OFF 0
#define SND_TIMER_WATCHDOG 1 /* keeps the position current between interrupts */
#define SND_TIMER_PERIODIC 2 /* drives the position itself */

struct snd_file;

struct snd_ss_rt {
	struct snd_pcm_substream *ss;
	struct snd_ring *ring;
	struct snd_mutex mtx; /* control path: params, start/stop, drain */
	struct snd_mutex io_mtx; /* one reader/writer at a time */
	struct snd_file *owner; /* the open handle that claimed it */

	/* geometry (frames) */
	uint32_t buffer_frames;
	uint32_t period_frames;
	uint64_t period_ns;
	uint32_t start_threshold; /* frames queued before auto-start */

	/* position bookkeeping (under ss->lock) */
	uint64_t last_update_ns;
	uint64_t last_irq_ns;
	uint64_t irq_hw_frames; /* hw_frames at the last period interrupt,
				 * rounded down to a period boundary */
	uint32_t irq_count;
	uint64_t last_wake_frames; /* hw_frames at the last wake-up */
	int need_prepare; /* stopped after drain/xrun: prepare first */
	int io_active; /* a read/write is in progress */
	int bogus_ptr_reported;
	/* OSS-style mmap: the application works in the mapped ring without
	 * telling the kernel how far it got, so the ring is free-running --
	 * playback never underruns and capture never overruns. */
	int free_run;

	/* timer */
	hrtimer_t timer;
	int timer_driven; /* the driver asked for it */
	int timer_mode; /* SND_TIMER_* in force */
	volatile int timer_cb_active;
	uint64_t drain_deadline_ns;

	/* OSS GETOPTR/GETIPTR bookkeeping */
	uint64_t oss_blocks_base;
};

struct snd_pcm_priv {
	struct snd_pcm pcm;
	struct snd_ss_rt rt[2];
};

/* ---- per-card core state ---------------------------------------------------- */

#define SND_NODE_MIXER 0
#define SND_NODE_DSP 1
#define SND_NODE_AUDIO 2

struct snd_card_priv {
	struct snd_card card;
	int refs; /* the driver's + one per open handle */
	int slot; /* index in the global card table, -1 = none */
	struct snd_mutex ctl_mtx; /* serialises snd_ctl_ops calls */
	char sysname[3][16]; /* "audio0", "dsp", "mixer" */
	int node_ok[3];
	int open_files;
	uint32_t magic;
};

#define SND_CARD_MAGIC 0x53434152u /* "SCAR" */
#define SND_FILE_MAGIC 0x53464c45u /* "SFLE" */

static inline struct snd_card_priv *snd_card_priv(struct snd_card *c)
{
	return (struct snd_card_priv *)c; /* first member */
}

static inline struct snd_pcm_priv *snd_pcm_priv(struct snd_pcm *p)
{
	return (struct snd_pcm_priv *)p; /* first member */
}

static inline struct snd_ss_rt *snd_rt(struct snd_pcm_substream *ss)
{
	return &snd_pcm_priv(ss->pcm)->rt[ss->direction ? 1 : 0];
}

/* ---- an open handle ------------------------------------------------------------ */

struct snd_file {
	uint32_t magic;
	struct snd_card *card;
	struct snd_pcm *pcm; /* NULL for the mixer node */
	int kind; /* SND_NODE_* */
	uint32_t dirs; /* (1 << AUDIO_DIR_*) claimed */
	int primary; /* AUDIO_DIR_* the direction-less requests use */
	int can_write; /* opened with write access */
	uint32_t uid;
	uint32_t ev_seq; /* next card event this handle reads */
	vfs_file_t *vfs;
	/* AUDIO_SELECTPCM swaps `pcm' only while no other call is inside
	 * the handle: every entry point counts itself in `active' and backs
	 * out with -EBUSY while `switching' is set. */
	int active;
	int switching;

	/* OSS state (the /dev/dsp dialect) */
	int oss_used; /* OSS requests shaped the stream: use the OSS data path */
	int oss_dirty; /* the wanted parameters differ from the ring's */
	uint32_t oss_format; /* AUDIO_FMT_* */
	uint32_t oss_rate;
	uint32_t oss_channels;
	uint32_t oss_frag_bytes; /* 0 = default */
	uint32_t oss_frag_count; /* 0 = default */
	uint32_t oss_subdivide;
	uint32_t oss_low_water; /* bytes, 0 = one fragment */
	uint32_t oss_trigger; /* PCM_ENABLE_* */
	uint8_t oss_partial[SND_OSS_PARTIAL_MAX];
	uint32_t oss_partial_len;
};

static inline struct snd_pcm_substream *snd_file_ss(struct snd_file *f, int dir)
{
	if (!f->pcm || dir < 0 || dir > 1 || !(f->dirs & (1u << dir)))
		return NULL;
	return &f->pcm->sub[dir];
}

/* ---- small helpers (snd_core.c) -------------------------------------------- */

void snd_strlcpy(char *dst, const char *src, size_t cap);
int snd_user_in(void *dst, const void *user, size_t n);
int snd_user_out(void *user, const void *src, size_t n);

/* Text building for diagnostics. */
struct snd_text {
	char *buf;
	size_t cap;
	size_t len;
};
void snd_text_printf(struct snd_text *t, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* The card table. */
struct snd_card *snd_card_get(int index);
int snd_card_count(void);
void snd_card_ref(struct snd_card *card);
void snd_card_unref(struct snd_card *card);

/* ---- PCM (snd_pcm.c) ------------------------------------------------------- */

/* Name of an AUDIO_STATE_* value. */
const char *snd_pcm_state_name(uint32_t state);

/* Refinement of single parameters against a direction's capabilities. */
uint32_t snd_pcm_pick_format(const struct snd_pcm_hw *hw, uint32_t want);
uint32_t snd_pcm_pick_rate(const struct snd_pcm_hw *hw, uint32_t want);
uint32_t snd_pcm_pick_channels(const struct snd_pcm_hw *hw, uint32_t want);
/* Full negotiation: `p' in, granted values out.  0 or -EINVAL. */
int snd_pcm_refine(const struct snd_pcm_hw *hw, struct audio_params *p,
		   uint32_t *period_bytes, uint32_t *periods,
		   uint32_t *buffer_bytes);

/* Claim / release a substream for an open handle. */
int snd_pcm_claim(struct snd_file *f, struct snd_pcm_substream *ss,
		  uint32_t uid);
void snd_pcm_release_ss(struct snd_file *f, struct snd_pcm_substream *ss);

/* Configure (allocate the ring, call hw_params and prepare).  Takes rt->mtx. */
int snd_pcm_set_params(struct snd_pcm_substream *ss, struct audio_params *p);
/* State changes, from process context.  Each takes rt->mtx. */
int snd_pcm_start(struct snd_pcm_substream *ss);
int snd_pcm_pause(struct snd_pcm_substream *ss, int pause);
int snd_pcm_drop(struct snd_pcm_substream *ss);
int snd_pcm_prepare(struct snd_pcm_substream *ss);
int snd_pcm_drain(struct snd_pcm_substream *ss, int nonblock);
/* Advance the application pointer by `frames' (mmap use).  Returns the
 * frames committed. */
long snd_pcm_commit(struct snd_pcm_substream *ss, uint32_t frames);

/* Position, refreshed from the hardware first. */
void snd_pcm_get_pos(struct snd_pcm_substream *ss, struct audio_pos *pos);
/* Frames writable (playback) / readable (capture) now, refreshed. */
uint32_t snd_pcm_avail(struct snd_pcm_substream *ss);

/* Data transfer.  `user' says whether `buf' is a user pointer.
 * Returns bytes moved or a negative errno; never moves a partial frame. */
long snd_pcm_transfer(struct snd_pcm_substream *ss, void *buf, long bytes,
		      int nonblock, int user, int autostart);

/* poll bits for one substream (no poll_wait: the caller registers). */
short snd_pcm_poll_ss(struct snd_pcm_substream *ss, short events, int oss);

/* mmap of a handle's ring. */
int snd_pcm_mmap(struct snd_file *f, struct device_mmap *m);

/* The native PCM requests (AUDIO_SETPARAMS ... AUDIO_COMMIT). */
long snd_pcm_ioctl(struct snd_file *f, unsigned long req, void *argp);

/* Stop everything on a card that is going away (no driver calls). */
void snd_pcm_disconnect_all(struct snd_card *card);

/* Core-side part of the stream dump. */
void snd_pcm_diag(struct snd_card *card, struct snd_text *t);

/* ---- controls (snd_ctl.c) ---------------------------------------------------- */

long snd_ctl_ioctl(struct snd_file *f, unsigned long req, void *argp);
/* Wrappers serialised by the card's control mutex; -ENOENT without ops. */
int snd_ctl_get_volume(struct snd_card *card, uint32_t ctl,
		       struct audio_volume *v);
int snd_ctl_set_volume(struct snd_card *card, uint32_t ctl, uint32_t left,
		       uint32_t right);
int snd_ctl_get_mute(struct snd_card *card, uint32_t ctl, uint32_t *mute);
int snd_ctl_get_outputs(struct snd_card *card, struct audio_outputs *o,
			int inputs);
int snd_ctl_set_output(struct snd_card *card, const struct audio_setoutput *s,
		       int inputs);
void snd_ctl_fill_info(struct snd_card *card, struct snd_pcm *pcm,
		       struct audio_info *info);
/* Pull the next card event for a handle: 0, or -EAGAIN when none. */
int snd_ctl_next_event(struct snd_file *f, struct audio_event *ev);
int snd_ctl_event_pending(struct snd_file *f);

/* ---- OSS (snd_oss.c) ----------------------------------------------------------- */

void snd_oss_file_init(struct snd_file *f);
long snd_oss_dsp_ioctl(struct snd_file *f, unsigned long req, void *argp);
long snd_oss_mixer_ioctl(struct snd_file *f, unsigned long req, void *argp);
long snd_oss_sysinfo_ioctl(struct snd_file *f, unsigned long req, void *argp);
long snd_oss_write(struct snd_file *f, const void *buf, long bytes,
		   int nonblock);
long snd_oss_read(struct snd_file *f, void *buf, long bytes, int nonblock);
/* Apply pending OSS parameter changes (configure the ring lazily). */
int snd_oss_configure(struct snd_file *f);

#endif /* KERNEL_DEV_SOUND_SND_INTERNAL_H */
