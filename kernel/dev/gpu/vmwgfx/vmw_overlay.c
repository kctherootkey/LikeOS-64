// LikeOS -- vmwgfx: the SVGA video overlay streams.
//
// The virtual display adapter can composite one video stream over the
// screen itself: the guest hands it a YUV frame in guest memory (a
// buffer bound as a GMR), the pixel format, the source rectangle within
// the frame and the destination rectangle on the screen, and the host
// converts, scales and shows the frame wherever the colour key is set in
// the framebuffer.  The registers of a stream are programmed through the
// FIFO's escape command (SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS) and take
// effect at the flush that follows (SVGA_ESCAPE_VMWARE_VIDEO_FLUSH).
//
// A client -- the X server's overlay video adaptor -- claims a stream
// (there is one), controls it with whole register sets per frame, and
// gives it up; the stream stops with the client's file if it forgets.
// The buffer of a running stream is referenced and its GMR binding kept
// until the stream stops or moves to another buffer.
//
// A stream can also be paused: the device is told to stop showing it, but
// the stream keeps its buffer, its GMR and the last register set it was
// given, and resuming sends that register set again.  The display code
// pauses every stream around a change of what the screen scans out
// (vmw_overlay_pause_all / vmw_overlay_resume_all), so the host is never
// left compositing a stream onto a screen that is being redefined under
// it, and the client does not have to notice the mode set to get its video
// back.
//
// Only a display unit that scans out of guest memory (screen objects or
// screen targets) can name a GMR in the stream registers; the legacy
// unit would need the frame in the framebuffer BAR, which this driver's
// buffers never are, so on such a host no stream is offered.
//
// Copyright (C) 2026 The LikeOS Project
// SPDX-License-Identifier for the portions derived from VMware's code: GPL-2.0 OR MIT
// Portions Copyright 2009-2023 VMware, Inc., Palo Alto, CA., USA

#include <kernel/dev/gpu/vmwgfx/vmw_gb.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga_escape.h>
#include <kernel/dev/gpu/vmwgfx/svga/svga_overlay.h>
#include <kernel/io/console.h>
#include <kernel/ke/syscall.h>
#include <kernel/mm/memory.h>

#define VMW_MAX_NUM_STREAMS 1

struct vmw_stream {
	struct drm_gem_object *buf; /* referenced while the stream shows it */
	int gmr_bound_here; /* the GMR was bound for the stream, not by the buffer */
	int claimed;
	int paused;
	struct drm_file *owner;
	struct drm_vmw_control_stream_arg saved;
};

/* Each stream is a single overlay; a video client calls them ports. */
struct vmw_overlay {
	mm_rwsem_t lock;
	struct vmw_stream stream[VMW_MAX_NUM_STREAMS];
};

/* the FIFO escape wrapper: the command, then the namespace and payload size */
struct vmw_escape_header {
	uint32_t cmd;
	SVGAFifoCmdEscape body;
};

struct vmw_escape_video_flush {
	struct vmw_escape_header escape;
	SVGAEscapeVideoFlush flush;
};

static inline void fill_escape(struct vmw_escape_header *header, uint32_t size)
{
	header->cmd = SVGA_CMD_ESCAPE;
	header->body.nsid = SVGA_ESCAPE_NSID_VMWARE;
	header->body.size = size;
}

static inline void fill_flush(struct vmw_escape_video_flush *cmd,
			      uint32_t stream_id)
{
	fill_escape(&cmd->escape, sizeof(cmd->flush));
	cmd->flush.cmdType = SVGA_ESCAPE_VMWARE_VIDEO_FLUSH;
	cmd->flush.streamId = stream_id;
}

static int vmw_overlay_available(struct vmw_device *v)
{
	return v->overlay_priv && v->has_gmr && v->scanout_in_guest_memory &&
	       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_VIDEO) &&
	       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_ESCAPE);
}

/* The whole register set of a stream, then a flush.
 *
 * Screen objects and screen targets take the GMR id and screen id registers
 * as well (the legacy unit, which stops at PITCH_3, never gets here -- see
 * the top of the file); the register ids are consecutive from 0. */
static int vmw_overlay_send_put(struct vmw_device *v, struct drm_gem_object *o,
				const struct drm_vmw_control_stream_arg *arg)
{
	struct vmw_bo *b = o->priv;
	struct vmw_escape_video_flush *flush;
	uint32_t fifo_size;
	const uint32_t num_items = SVGA_VIDEO_DST_SCREEN_ID + 1;
	uint32_t i;

	struct {
		struct vmw_escape_header escape;
		struct {
			uint32_t cmdType;
			uint32_t streamId;
		} header;
	} *cmds;
	struct {
		uint32_t registerId;
		uint32_t value;
	} *items;

	if (!b || b->gmr_id < 0)
		return -EINVAL;

	fifo_size = sizeof(*cmds) + sizeof(*flush) + sizeof(*items) * num_items;

	cmds = VMW_CMD_RESERVE(v, fifo_size);
	/* the device is wedged; nothing to be done here */
	if (!cmds)
		return -ENOMEM;

	items = (void *)&cmds[1];
	flush = (struct vmw_escape_video_flush *)&items[num_items];

	/* the size is the header plus the register items */
	fill_escape(&cmds->escape, sizeof(*items) * (num_items + 1));

	cmds->header.cmdType = SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS;
	cmds->header.streamId = arg->stream_id;

	/* the ids are numbered consecutively */
	for (i = 0; i < num_items; i++)
		items[i].registerId = i;

	items[SVGA_VIDEO_ENABLED].value = 1;
	items[SVGA_VIDEO_FLAGS].value = arg->flags;
	items[SVGA_VIDEO_DATA_OFFSET].value = arg->offset;
	items[SVGA_VIDEO_FORMAT].value = (uint32_t)arg->format;
	items[SVGA_VIDEO_COLORKEY].value = arg->color_key;
	items[SVGA_VIDEO_SIZE].value = arg->size;
	items[SVGA_VIDEO_WIDTH].value = arg->width;
	items[SVGA_VIDEO_HEIGHT].value = arg->height;
	items[SVGA_VIDEO_SRC_X].value = (uint32_t)arg->src.x;
	items[SVGA_VIDEO_SRC_Y].value = (uint32_t)arg->src.y;
	items[SVGA_VIDEO_SRC_WIDTH].value = arg->src.w;
	items[SVGA_VIDEO_SRC_HEIGHT].value = arg->src.h;
	items[SVGA_VIDEO_DST_X].value = (uint32_t)arg->dst.x;
	items[SVGA_VIDEO_DST_Y].value = (uint32_t)arg->dst.y;
	items[SVGA_VIDEO_DST_WIDTH].value = arg->dst.w;
	items[SVGA_VIDEO_DST_HEIGHT].value = arg->dst.h;
	items[SVGA_VIDEO_PITCH_1].value = arg->pitch[0];
	items[SVGA_VIDEO_PITCH_2].value = arg->pitch[1];
	items[SVGA_VIDEO_PITCH_3].value = arg->pitch[2];
	items[SVGA_VIDEO_DATA_GMRID].value = (uint32_t)b->gmr_id;
	items[SVGA_VIDEO_DST_SCREEN_ID].value = SVGA_ID_INVALID;

	fill_flush(flush, arg->stream_id);

	/* Handed over at once: a frame is shown when its flush arrives, not
	 * whenever the next batch happens to go. */
	vmw_cmd_commit_flush(v, fifo_size);
	return 0;
}

/* SET_REGS of ENABLED = 0, then a flush. */
static int vmw_overlay_send_stop(struct vmw_device *v, uint32_t stream_id)
{
	struct {
		struct vmw_escape_header escape;
		SVGAEscapeVideoSetRegs body;
		struct vmw_escape_video_flush flush;
	} *cmds;

	cmds = VMW_CMD_RESERVE(v, sizeof(*cmds));
	if (!cmds)
		return -ENOMEM;

	fill_escape(&cmds->escape, sizeof(cmds->body));
	cmds->body.header.cmdType = SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS;
	cmds->body.header.streamId = stream_id;
	cmds->body.items[0].registerId = SVGA_VIDEO_ENABLED;
	cmds->body.items[0].value = 0;
	fill_flush(&cmds->flush, stream_id);

	vmw_cmd_commit_flush(v, sizeof(*cmds));
	return 0;
}

/* Make a buffer reachable by the host as {gmr, offset} (pin), or undo what
 * pinning did (unpin).
 *
 * Pinning binds a GMR to the buffer if it has none -- a guest-backed host
 * gives ordinary buffers only a MOB -- and says so in *gmr_bound_here, so
 * that unpinning frees only a GMR the stream bound itself, never one the
 * buffer had for its own reasons (a scan-out buffer, say).
 *
 * The region is taken from the GMR manager, so its pages count against the
 * device's region budget like every other region, and given back to it --
 * which also unbinds and frees the id -- with the same page count.  Without
 * a manager the id comes straight from the hardware layer's table, as it
 * always did. */
static int vmw_overlay_move_buffer(struct vmw_device *v, struct drm_gem_object *o,
				   int pin, int *gmr_bound_here)
{
	struct vmw_bo *b = o->priv;
	uint32_t id;
	int rc;

	if (!pin) {
		if (*gmr_bound_here && b && b->gmr_id >= 0) {
			vmw_gmrid_man_put_node(v, VMW_PL_GMR, (uint32_t)b->gmr_id,
					       (uint32_t)o->npages);
			b->gmr_id = -1;
		}
		*gmr_bound_here = 0;
		return 0;
	}

	*gmr_bound_here = 0;
	if (!b || !o->pages)
		return -EINVAL;
	if (b->gmr_id >= 0)
		return 0;
	rc = vmw_gmrid_man_get_node(v, VMW_PL_GMR, (uint32_t)o->npages, &id);
	if (rc)
		return rc;
	if (vmsvga2_gmr_bind((int)id, o->pages, (uint32_t)o->npages) != 0) {
		vmw_gmrid_man_put_node(v, VMW_PL_GMR, id, (uint32_t)o->npages);
		return -EIO;
	}
	b->gmr_id = (int)id;
	*gmr_bound_here = 1;
	return 0;
}

/* Stop or pause a stream.  The device is told either way; stopping also
 * lets go of the buffer, pausing keeps it -- and its GMR and the saved
 * register set -- for vmw_overlay_resume_all().  With the overlay lock
 * held. */
static int vmw_overlay_stop(struct vmw_device *v, uint32_t stream_id, int pause)
{
	struct vmw_overlay *ov = v->overlay_priv;
	struct vmw_stream *s = &ov->stream[stream_id];
	int rc = 0;

	/* no buffer attached: the stream is completely stopped */
	if (!s->buf)
		return 0;

	/* a paused stream has been told already */
	if (!s->paused) {
		rc = vmw_overlay_send_stop(v, stream_id);
		/* A pause that did not reach the device leaves the stream
		 * running, and not paused, so resume leaves it alone.  A stop
		 * lets go of the buffer regardless: the file that owned it
		 * may be on its way out, and nobody would ever drop the
		 * reference otherwise. */
		if (rc && pause)
			return rc;
	}

	if (!pause) {
		struct drm_gem_object *o = s->buf;

		vmw_overlay_move_buffer(v, o, 0, &s->gmr_bound_here);
		s->buf = NULL;
		s->paused = 0;
		drm_gem_put(o);
	} else {
		s->paused = 1;
	}
	return rc;
}

/* A new register set for a stream, and the buffer it now shows: a put, after
 * a stop of whatever the stream showed before if that was another buffer.
 * With the overlay lock held. */
static int vmw_overlay_update_stream(struct vmw_device *v, struct drm_gem_object *o,
				     const struct drm_vmw_control_stream_arg *arg)
{
	struct vmw_overlay *ov = v->overlay_priv;
	struct vmw_stream *s = &ov->stream[arg->stream_id];
	int bound_here = 0;
	int rc;

	if (!o)
		return -EINVAL;

	if (s->buf != o) {
		rc = vmw_overlay_stop(v, arg->stream_id, 0);
		if (rc)
			return rc;
	} else if (!s->paused) {
		/* Same buffer and running: just the register set. */
		rc = vmw_overlay_send_put(v, o, arg);
		if (rc == 0)
			s->saved = *arg;
		return rc;
	}

	/* A new buffer is made reachable first.  (The same buffer, paused,
	 * still has the GMR it had while running: pausing keeps it.) */
	if (s->buf != o) {
		rc = vmw_overlay_move_buffer(v, o, 1, &bound_here);
		if (rc)
			return rc;
	}

	rc = vmw_overlay_send_put(v, o, arg);
	if (rc) {
		if (s->buf != o)
			vmw_overlay_move_buffer(v, o, 0, &bound_here);
		return rc;
	}

	if (s->buf != o) {
		drm_gem_get(o);
		s->buf = o;
		s->gmr_bound_here = bound_here;
	}
	s->saved = *arg;
	/* the stream is no longer stopped or paused */
	s->paused = 0;
	return 0;
}

/* Start every paused stream again with its last register set.  Called by
 * the display code once the scan-out change it paused them for is done.
 * Takes the overlay lock. */
int vmw_overlay_resume_all(struct vmw_device *v)
{
	struct vmw_overlay *ov = v->overlay_priv;

	if (!ov)
		return 0;

	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		struct vmw_stream *s = &ov->stream[i];

		if (!s->paused)
			continue;
		if (vmw_overlay_update_stream(v, s->buf, &s->saved) != 0)
			kprintf("[drm] vmwgfx: failed to resume overlay stream %u\n", i);
	}
	mm_write_unlock(&ov->lock);
	return 0;
}

/* Pause every running stream.  Called by the display code before it
 * changes what the screen scans out.  Takes the overlay lock. */
int vmw_overlay_pause_all(struct vmw_device *v)
{
	struct vmw_overlay *ov = v->overlay_priv;

	if (!ov)
		return 0;

	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		if (ov->stream[i].paused)
			kprintf("[drm] vmwgfx: overlay stream %u already paused\n", i);
		WARN_ON_ONCE(vmw_overlay_stop(v, i, 1) != 0);
	}
	mm_write_unlock(&ov->lock);
	return 0;
}

/* ---- claiming and releasing streams --------------------------------- */

/* A free stream for `fp'; -ESRCH when all are taken. */
static int vmw_overlay_claim(struct vmw_device *v, struct drm_file *fp,
			     uint32_t *out)
{
	struct vmw_overlay *ov = v->overlay_priv;

	if (!ov)
		return -ENOSYS;

	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		if (ov->stream[i].claimed)
			continue;
		ov->stream[i].claimed = 1;
		ov->stream[i].owner = fp;
		*out = i;
		mm_write_unlock(&ov->lock);
		return 0;
	}
	mm_write_unlock(&ov->lock);
	return -ESRCH;
}

/* Stop a stream and hand it back.  With the overlay lock held. */
static void vmw_overlay_unref_locked(struct vmw_device *v, uint32_t stream_id)
{
	struct vmw_overlay *ov = v->overlay_priv;

	WARN_ON(!ov->stream[stream_id].claimed);
	vmw_overlay_stop(v, stream_id, 0);
	ov->stream[stream_id].claimed = 0;
	ov->stream[stream_id].owner = NULL;
}

/* ---- the interface ---------------------------------------------------- */

long vmw_ioctl_control_stream(struct vmw_device *v, struct drm_file *fp,
			      struct drm_vmw_control_stream_arg *arg)
{
	struct vmw_overlay *ov = v->overlay_priv;
	struct drm_gem_object *o = NULL;
	long rc;

	if (!vmw_overlay_available(v))
		return -ENOSYS;
	if (!fp->is_master)
		return -EACCES;
	if (arg->stream_id >= VMW_MAX_NUM_STREAMS)
		return -EINVAL;
	mm_write_lock(&ov->lock);
	struct vmw_stream *s = &ov->stream[arg->stream_id];
	if (!s->claimed || s->owner != fp) {
		rc = -EINVAL;
		goto out;
	}
	if (!arg->enabled) {
		rc = vmw_overlay_stop(v, arg->stream_id, 0);
		goto out;
	}
	/* The host would take any of these and show garbage, or nothing; a
	 * format it cannot convert or an empty rectangle is refused here. */
	switch (arg->format) {
	case SVGA_OVERLAY_FORMAT_YV12:
	case SVGA_OVERLAY_FORMAT_YUY2:
	case SVGA_OVERLAY_FORMAT_UYVY:
		break;
	default:
		rc = -EINVAL;
		goto out;
	}
	if (arg->width == 0 || arg->height == 0 || arg->size == 0 ||
	    arg->src.w == 0 || arg->src.h == 0 || arg->dst.w == 0 || arg->dst.h == 0) {
		rc = -EINVAL;
		goto out;
	}
	o = drm_gem_lookup(fp, arg->handle);
	if (!o || o->kind != DRM_GEM_BO) {
		rc = -ENOENT;
		goto out;
	}
	/* The frame has to lie inside the buffer the host reads it from. */
	if ((uint64_t)arg->offset + arg->size > o->size) {
		rc = -EINVAL;
		goto out;
	}
	rc = vmw_overlay_update_stream(v, o, arg);
out:
	mm_write_unlock(&ov->lock);
	if (o)
		drm_gem_put(o);
	return rc;
}

long vmw_ioctl_claim_stream(struct vmw_device *v, struct drm_file *fp,
			    struct drm_vmw_stream_arg *arg)
{
	uint32_t id;
	int rc;

	if (!vmw_overlay_available(v))
		return -ENOSYS;
	if (!fp->is_master)
		return -EACCES;
	rc = vmw_overlay_claim(v, fp, &id);
	if (rc == 0)
		arg->stream_id = id;
	return rc;
}

long vmw_ioctl_unref_stream(struct vmw_device *v, struct drm_file *fp,
			    struct drm_vmw_stream_arg *arg)
{
	struct vmw_overlay *ov = v->overlay_priv;
	long rc = 0;

	if (!vmw_overlay_available(v))
		return -ENOSYS;
	if (!fp->is_master)
		return -EACCES;
	if (arg->stream_id >= VMW_MAX_NUM_STREAMS)
		return -EINVAL;
	mm_write_lock(&ov->lock);
	struct vmw_stream *s = &ov->stream[arg->stream_id];
	if (!s->claimed || s->owner != fp)
		rc = -EINVAL;
	else
		vmw_overlay_unref_locked(v, arg->stream_id);
	mm_write_unlock(&ov->lock);
	return rc;
}

int vmw_overlay_num_streams(struct vmw_device *v)
{
	if (!vmw_overlay_available(v))
		return 0;
	return VMW_MAX_NUM_STREAMS;
}

int vmw_overlay_num_free_streams(struct vmw_device *v)
{
	struct vmw_overlay *ov = v->overlay_priv;
	int n = 0;

	if (!vmw_overlay_available(v))
		return 0;
	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++)
		if (!ov->stream[i].claimed)
			n++;
	mm_write_unlock(&ov->lock);
	return n;
}

/* The file is going: whatever it claimed is stopped and freed. */
void vmw_overlay_file_release(struct vmw_device *v, struct drm_file *fp)
{
	struct vmw_overlay *ov = v->overlay_priv;

	if (!ov)
		return;
	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		struct vmw_stream *s = &ov->stream[i];
		if (!s->claimed || s->owner != fp)
			continue;
		vmw_overlay_unref_locked(v, i);
	}
	mm_write_unlock(&ov->lock);
}

int vmw_overlay_init(struct vmw_device *v)
{
	struct vmw_overlay *ov;

	if (v->overlay_priv)
		return -EINVAL;
	ov = kalloc(sizeof(*ov));
	if (!ov)
		return -ENOMEM;
	mm_memset(ov, 0, sizeof(*ov));
	mm_rwsem_init(&ov->lock, "vmw_overlay");
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		ov->stream[i].buf = NULL;
		ov->stream[i].paused = 0;
		ov->stream[i].claimed = 0;
	}
	v->overlay_priv = ov;
	return 0;
}

void vmw_overlay_close(struct vmw_device *v)
{
	struct vmw_overlay *ov = v->overlay_priv;
	int forgotten_buffer = 0;

	if (!ov)
		return;
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		if (ov->stream[i].buf) {
			forgotten_buffer = 1;
			vmw_overlay_stop(v, i, 0);
		}
	}
	/* every stream's file should have stopped it on its way out */
	WARN_ON(forgotten_buffer);
	v->overlay_priv = NULL;
	kfree(ov);
}
