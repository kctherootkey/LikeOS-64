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
// Only a display unit that scans out of guest memory (screen objects or
// screen targets) can name a GMR in the stream registers; the legacy
// unit would need the frame in the framebuffer BAR, which this driver's
// buffers never are, so on such a host no stream is offered.
//
// Copyright (C) 2026 The LikeOS Project

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

struct vmw_overlay {
	mm_rwsem_t lock;
	struct vmw_stream stream[VMW_MAX_NUM_STREAMS];
};

/* the FIFO escape wrapper: the command, the namespace, the payload size */
struct vmw_escape_header {
	uint32_t cmd;
	uint32_t nsid;
	uint32_t size;
};

static void fill_escape(struct vmw_escape_header *h, uint32_t size)
{
	h->cmd = SVGA_CMD_ESCAPE;
	h->nsid = SVGA_ESCAPE_NSID_VMWARE;
	h->size = size;
}

static int vmw_overlay_available(struct vmw_device *v)
{
	return v->overlay_priv && v->has_gmr && v->scanout_in_guest_memory &&
	       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_VIDEO) &&
	       vmsvga2_hw_has_fifo_cap(SVGA_FIFO_CAP_ESCAPE);
}

/* The frame must be reachable by the host as {gmr, offset}: bind a GMR to
 * the buffer if it has none (a guest-backed host gives ordinary buffers
 * only a MOB). */
static int stream_bind_buffer(struct vmw_stream *s, struct drm_gem_object *o)
{
	struct vmw_bo *b = o->priv;

	if (!b || !o->pages)
		return -EINVAL;
	if (b->gmr_id >= 0)
		return 0;
	int id = vmsvga2_gmr_alloc((uint32_t)o->npages);
	if (id < 0)
		return -ENOSPC;
	if (vmsvga2_gmr_bind(id, o->pages, (uint32_t)o->npages) != 0) {
		vmsvga2_gmr_free(id);
		return -EIO;
	}
	b->gmr_id = id;
	s->gmr_bound_here = 1;
	return 0;
}

static void stream_release_buffer(struct vmw_stream *s)
{
	struct drm_gem_object *o = s->buf;
	if (!o)
		return;
	if (s->gmr_bound_here) {
		struct vmw_bo *b = o->priv;
		if (b && b->gmr_id >= 0) {
			vmsvga2_gmr_free(b->gmr_id);
			b->gmr_id = -1;
		}
		s->gmr_bound_here = 0;
	}
	s->buf = NULL;
	drm_gem_put(o);
}

/* The whole register set of a stream, then a flush. */
static int vmw_overlay_send_put(struct vmw_device *v, struct drm_gem_object *o,
				const struct drm_vmw_control_stream_arg *arg)
{
	struct vmw_bo *b = o->priv;
	/* screen objects/targets take the GMR id and screen id registers as
	 * well; the register ids are consecutive from 0 */
	const uint32_t num_items = SVGA_VIDEO_DST_SCREEN_ID + 1;
	uint32_t buf[3 + 2 + 2 * SVGA_VIDEO_NUM_REGS + 3 + 2];
	uint32_t *p = buf;
	uint32_t values[SVGA_VIDEO_NUM_REGS];

	if (!b || b->gmr_id < 0)
		return -EINVAL;
	mm_memset(values, 0, sizeof(values));
	values[SVGA_VIDEO_ENABLED] = 1;
	values[SVGA_VIDEO_FLAGS] = arg->flags;
	values[SVGA_VIDEO_DATA_OFFSET] = arg->offset;
	values[SVGA_VIDEO_FORMAT] = (uint32_t)arg->format;
	values[SVGA_VIDEO_COLORKEY] = arg->color_key;
	values[SVGA_VIDEO_SIZE] = arg->size;
	values[SVGA_VIDEO_WIDTH] = arg->width;
	values[SVGA_VIDEO_HEIGHT] = arg->height;
	values[SVGA_VIDEO_SRC_X] = (uint32_t)arg->src.x;
	values[SVGA_VIDEO_SRC_Y] = (uint32_t)arg->src.y;
	values[SVGA_VIDEO_SRC_WIDTH] = arg->src.w;
	values[SVGA_VIDEO_SRC_HEIGHT] = arg->src.h;
	values[SVGA_VIDEO_DST_X] = (uint32_t)arg->dst.x;
	values[SVGA_VIDEO_DST_Y] = (uint32_t)arg->dst.y;
	values[SVGA_VIDEO_DST_WIDTH] = arg->dst.w;
	values[SVGA_VIDEO_DST_HEIGHT] = arg->dst.h;
	values[SVGA_VIDEO_PITCH_1] = arg->pitch[0];
	values[SVGA_VIDEO_PITCH_2] = arg->pitch[1];
	values[SVGA_VIDEO_PITCH_3] = arg->pitch[2];
	values[SVGA_VIDEO_DATA_GMRID] = (uint32_t)b->gmr_id;
	values[SVGA_VIDEO_DST_SCREEN_ID] = SVGA_ID_INVALID;

	/* SET_REGS: escape header, {cmdType, streamId}, {registerId, value}... */
	fill_escape((struct vmw_escape_header *)p, 8 * (num_items + 1));
	p += 3;
	*p++ = SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS;
	*p++ = arg->stream_id;
	for (uint32_t i = 0; i < num_items; i++) {
		*p++ = i;
		*p++ = values[i];
	}
	/* FLUSH: escape header, {cmdType, streamId} */
	fill_escape((struct vmw_escape_header *)p, 8);
	p += 3;
	*p++ = SVGA_ESCAPE_VMWARE_VIDEO_FLUSH;
	*p++ = arg->stream_id;
	return vmw_cmd_raw(v, buf, (uint32_t)((p - buf) * 4), 1);
}

static int vmw_overlay_send_stop(struct vmw_device *v, uint32_t stream_id)
{
	uint32_t buf[3 + 2 + 2 + 3 + 2];
	uint32_t *p = buf;

	fill_escape((struct vmw_escape_header *)p, 8 * 2);
	p += 3;
	*p++ = SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS;
	*p++ = stream_id;
	*p++ = SVGA_VIDEO_ENABLED;
	*p++ = 0;
	fill_escape((struct vmw_escape_header *)p, 8);
	p += 3;
	*p++ = SVGA_ESCAPE_VMWARE_VIDEO_FLUSH;
	*p++ = stream_id;
	return vmw_cmd_raw(v, buf, (uint32_t)((p - buf) * 4), 1);
}

/* Stop (or pause) a stream: the device is told, and unless pausing the
 * buffer is let go of.  With the overlay lock held. */
static int vmw_overlay_stop(struct vmw_device *v, uint32_t stream_id, int pause)
{
	struct vmw_overlay *ov = v->overlay_priv;
	struct vmw_stream *s = &ov->stream[stream_id];
	int rc = 0;

	if (!s->buf)
		return 0;
	if (!s->paused)
		rc = vmw_overlay_send_stop(v, stream_id);
	if (pause) {
		s->paused = 1;
	} else {
		stream_release_buffer(s);
		s->paused = 0;
	}
	return rc;
}

/* A new register set for a stream, and the buffer it now shows.  With the
 * overlay lock held. */
static int vmw_overlay_update_stream(struct vmw_device *v, struct drm_gem_object *o,
				     const struct drm_vmw_control_stream_arg *arg)
{
	struct vmw_overlay *ov = v->overlay_priv;
	struct vmw_stream *s = &ov->stream[arg->stream_id];
	int rc;

	if (s->buf == o && !s->paused) {
		rc = vmw_overlay_send_put(v, o, arg);
		if (rc == 0)
			s->saved = *arg;
		return rc;
	}
	if (s->buf != o) {
		rc = vmw_overlay_stop(v, arg->stream_id, 0);
		if (rc)
			return rc;
		s->gmr_bound_here = 0;
		rc = stream_bind_buffer(s, o);
		if (rc)
			return rc;
		drm_gem_get(o);
		s->buf = o;
	}
	rc = vmw_overlay_send_put(v, o, arg);
	if (rc) {
		stream_release_buffer(s);
		return rc;
	}
	s->saved = *arg;
	s->paused = 0;
	return 0;
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
	struct vmw_overlay *ov = v->overlay_priv;
	long rc = -ESRCH;

	if (!vmw_overlay_available(v))
		return -ENOSYS;
	if (!fp->is_master)
		return -EACCES;
	mm_write_lock(&ov->lock);
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++) {
		if (ov->stream[i].claimed)
			continue;
		ov->stream[i].claimed = 1;
		ov->stream[i].owner = fp;
		arg->stream_id = i;
		rc = 0;
		break;
	}
	mm_write_unlock(&ov->lock);
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
	if (!s->claimed || s->owner != fp) {
		rc = -EINVAL;
	} else {
		vmw_overlay_stop(v, arg->stream_id, 0);
		s->claimed = 0;
		s->owner = NULL;
	}
	mm_write_unlock(&ov->lock);
	return rc;
}

int vmw_overlay_num_streams(struct vmw_device *v)
{
	return vmw_overlay_available(v) ? VMW_MAX_NUM_STREAMS : 0;
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
		vmw_overlay_stop(v, i, 0);
		s->claimed = 0;
		s->owner = NULL;
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
	v->overlay_priv = ov;
	return 0;
}

void vmw_overlay_close(struct vmw_device *v)
{
	struct vmw_overlay *ov = v->overlay_priv;

	if (!ov)
		return;
	for (uint32_t i = 0; i < VMW_MAX_NUM_STREAMS; i++)
		if (ov->stream[i].buf)
			vmw_overlay_stop(v, i, 0);
	v->overlay_priv = NULL;
	kfree(ov);
}
