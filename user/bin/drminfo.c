/* drminfo -- report what the display-manager device offers.
 *
 * The first question on a machine that is supposed to have a GPU is whether
 * the kernel found one, and the second is what it can do.  This answers both
 * from the interface itself rather than from the log: it opens every
 * /dev/dri node, asks each one its driver version and capabilities, walks the
 * mode-setting objects (connectors, encoders, CRTCs, planes) and, when the
 * driver is vmwgfx, asks for the device parameters that decide which of
 * Mesa's drivers can run on it.
 *
 * Everything here goes through the kernel's own headers (<drm/drm.h>,
 * <drm/drm_mode.h>, <drm/vmwgfx_drm.h>) and plain ioctl(2) -- no libdrm.  The
 * point is to be able to tell whether the KERNEL side works when the
 * userspace stack above it does not, which is exactly when a tool that
 * depends on that stack is no use.
 *
 * Usage: drminfo [-n node] [-v] [--driver [node]]
 *   -n   examine one node (a path, or a number meaning /dev/dri/card<N>)
 *   -v   also list every mode of every connector and every property value
 *   --driver   print only the driver's name for the node (default
 *              /dev/dri/card0) and exit 1 when there is none -- what a
 *              startup script asks to decide which X configuration and
 *              which rendering policy this machine gets
 *   --has-display   exit 0 when the node reports a connector and a CRTC,
 *              1 otherwise: a display server should not be pointed at a
 *              device that has no screen to offer
 *   --has-3d   exit 0 when the node's driver renders 3D on the device: i915,
 *              or vmwgfx with the VM's 3D acceleration switched on.  Decides
 *              glamor in X and GPU vs llvmpipe for GL clients (xserverrc,
 *              xinitrc)
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdint.h>
#include <sys/sysmacros.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <drm/i915_drm.h>

/* vmwgfx_drm.h numbers the driver's commands but does not spell the ioctls
 * out: every consumer builds them from DRM_COMMAND_BASE, and the one this
 * needs is the parameter query. */
#define DRMINFO_IOCTL_VMW_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM, struct drm_vmw_getparam_arg)

static int verbose;

struct name_map {
	unsigned long value;
	const char *name;
};

static const char *lookup(const struct name_map *m, unsigned long v)
{
	for (; m->name; m++)
		if (m->value == v)
			return m->name;
	return NULL;
}

static const struct name_map connector_types[] = {
	{ DRM_MODE_CONNECTOR_Unknown, "Unknown" },
	{ DRM_MODE_CONNECTOR_VGA, "VGA" },
	{ DRM_MODE_CONNECTOR_DVII, "DVI-I" },
	{ DRM_MODE_CONNECTOR_DVID, "DVI-D" },
	{ DRM_MODE_CONNECTOR_DVIA, "DVI-A" },
	{ DRM_MODE_CONNECTOR_Composite, "Composite" },
	{ DRM_MODE_CONNECTOR_SVIDEO, "S-Video" },
	{ DRM_MODE_CONNECTOR_LVDS, "LVDS" },
	{ DRM_MODE_CONNECTOR_Component, "Component" },
	{ DRM_MODE_CONNECTOR_9PinDIN, "DIN" },
	{ DRM_MODE_CONNECTOR_DisplayPort, "DisplayPort" },
	{ DRM_MODE_CONNECTOR_HDMIA, "HDMI-A" },
	{ DRM_MODE_CONNECTOR_HDMIB, "HDMI-B" },
	{ DRM_MODE_CONNECTOR_TV, "TV" },
	{ DRM_MODE_CONNECTOR_eDP, "eDP" },
	{ DRM_MODE_CONNECTOR_VIRTUAL, "Virtual" },
	{ DRM_MODE_CONNECTOR_DSI, "DSI" },
	{ 0, NULL }
};

static const struct name_map encoder_types[] = {
	{ DRM_MODE_ENCODER_NONE, "None" },
	{ DRM_MODE_ENCODER_DAC, "DAC" },
	{ DRM_MODE_ENCODER_TMDS, "TMDS" },
	{ DRM_MODE_ENCODER_LVDS, "LVDS" },
	{ DRM_MODE_ENCODER_TVDAC, "TV DAC" },
	{ DRM_MODE_ENCODER_VIRTUAL, "Virtual" },
	{ DRM_MODE_ENCODER_DSI, "DSI" },
	{ 0, NULL }
};

static const struct name_map caps[] = {
	{ DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER" },
	{ DRM_CAP_VBLANK_HIGH_CRTC, "VBLANK_HIGH_CRTC" },
	{ DRM_CAP_DUMB_PREFERRED_DEPTH, "DUMB_PREFERRED_DEPTH" },
	{ DRM_CAP_DUMB_PREFER_SHADOW, "DUMB_PREFER_SHADOW" },
	{ DRM_CAP_PRIME, "PRIME" },
	{ DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
	{ DRM_CAP_ASYNC_PAGE_FLIP, "ASYNC_PAGE_FLIP" },
	{ DRM_CAP_CURSOR_WIDTH, "CURSOR_WIDTH" },
	{ DRM_CAP_CURSOR_HEIGHT, "CURSOR_HEIGHT" },
	{ DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS" },
	{ DRM_CAP_PAGE_FLIP_TARGET, "PAGE_FLIP_TARGET" },
	{ DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT" },
	{ DRM_CAP_SYNCOBJ, "SYNCOBJ" },
	{ DRM_CAP_SYNCOBJ_TIMELINE, "SYNCOBJ_TIMELINE" },
	{ 0, NULL }
};

static const struct name_map vmw_params[] = {
	{ DRM_VMW_PARAM_NUM_STREAMS, "NUM_STREAMS" },
	{ DRM_VMW_PARAM_NUM_FREE_STREAMS, "NUM_FREE_STREAMS" },
	{ DRM_VMW_PARAM_NUM_FREE_STREAMS, "NUM_FREE_STREAMS" },
	{ DRM_VMW_PARAM_3D, "3D" },
	{ DRM_VMW_PARAM_HW_CAPS, "HW_CAPS" },
	{ DRM_VMW_PARAM_FIFO_CAPS, "FIFO_CAPS" },
	{ DRM_VMW_PARAM_MAX_FB_SIZE, "MAX_FB_SIZE" },
	{ DRM_VMW_PARAM_FIFO_HW_VERSION, "FIFO_HW_VERSION" },
	{ DRM_VMW_PARAM_MAX_SURF_MEMORY, "MAX_SURF_MEMORY" },
	{ DRM_VMW_PARAM_3D_CAPS_SIZE, "3D_CAPS_SIZE" },
	{ DRM_VMW_PARAM_MAX_MOB_MEMORY, "MAX_MOB_MEMORY" },
	{ DRM_VMW_PARAM_MAX_MOB_SIZE, "MAX_MOB_SIZE" },
	{ DRM_VMW_PARAM_SCREEN_TARGET, "SCREEN_TARGET" },
	{ DRM_VMW_PARAM_DX, "DX" },
	{ DRM_VMW_PARAM_HW_CAPS2, "HW_CAPS2" },
	{ DRM_VMW_PARAM_SM4_1, "SM4_1" },
	{ DRM_VMW_PARAM_SM5, "SM5" },
	{ DRM_VMW_PARAM_GL43, "GL43" },
	{ DRM_VMW_PARAM_DEVICE_ID, "DEVICE_ID" },
	{ DRM_VMW_PARAM_USER_SRF, "USER_SRF" },
	{ 0, NULL }
};

/* ioctl(2) with the errno left in place for the caller to report. */
static int drm_ioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static void print_version(int fd)
{
	struct drm_version v;
	char name[64], date[64], desc[128];

	memset(&v, 0, sizeof(v));
	if (drm_ioctl(fd, DRM_IOCTL_VERSION, &v) == -1) {
		printf("  version:            unavailable (%s)\n", strerror(errno));
		return;
	}

	/* The lengths came back in the first call; ask again with buffers.
	 * The kernel writes at most what it is given, so the sizes are
	 * clamped rather than trusted. */
	if (v.name_len >= sizeof(name))
		v.name_len = sizeof(name) - 1;
	if (v.date_len >= sizeof(date))
		v.date_len = sizeof(date) - 1;
	if (v.desc_len >= sizeof(desc))
		v.desc_len = sizeof(desc) - 1;
	memset(name, 0, sizeof(name));
	memset(date, 0, sizeof(date));
	memset(desc, 0, sizeof(desc));
	v.name = name;
	v.date = date;
	v.desc = desc;
	if (drm_ioctl(fd, DRM_IOCTL_VERSION, &v) == -1) {
		printf("  version:            unavailable (%s)\n", strerror(errno));
		return;
	}

	printf("  driver:             %s %d.%d.%d\n", name,
	       v.version_major, v.version_minor, v.version_patchlevel);
	printf("  date:               %s\n", date);
	printf("  description:        %s\n", desc);
}

static int get_cap(int fd, unsigned long cap, unsigned long long *out)
{
	struct drm_get_cap c;

	memset(&c, 0, sizeof(c));
	c.capability = cap;
	if (drm_ioctl(fd, DRM_IOCTL_GET_CAP, &c) == -1)
		return -1;
	*out = c.value;
	return 0;
}

static void print_caps(int fd)
{
	const struct name_map *m;

	printf("  capabilities:\n");
	for (m = caps; m->name; m++) {
		unsigned long long v;

		if (get_cap(fd, m->value, &v) == -1)
			continue;
		printf("    %-22s %llu\n", m->name, v);
	}
}

static void print_mode(const struct drm_mode_modeinfo *mode, const char *lead)
{
	printf("%s%ux%u@%u%s (%u kHz)\n", lead, mode->hdisplay, mode->vdisplay,
	       mode->vrefresh,
	       (mode->type & DRM_MODE_TYPE_PREFERRED) ? " preferred" : "",
	       mode->clock);
}

static void print_connector(int fd, unsigned int id)
{
	struct drm_mode_get_connector c;
	struct drm_mode_modeinfo *modes = NULL;
	unsigned int *encoders = NULL;
	const char *type;
	unsigned int i;

	memset(&c, 0, sizeof(c));
	c.connector_id = id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == -1) {
		printf("    connector %u:      unavailable (%s)\n", id,
		       strerror(errno));
		return;
	}

	if (c.count_modes) {
		modes = calloc(c.count_modes, sizeof(*modes));
		if (modes) {
			c.modes_ptr = (unsigned long long)(unsigned long)modes;
			/* Props and encoders are not wanted here; asking for
			 * none of them keeps the second call from needing
			 * buffers for them (the kernel fills only the arrays
			 * whose count it was given). */
			c.count_props = 0;
			c.count_encoders = 0;
			if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) == -1) {
				free(modes);
				modes = NULL;
			}
		}
	}

	type = lookup(connector_types, c.connector_type);
	printf("    connector %u:      %s-%u, %s, %ux%u mm, %u mode%s, %u encoder%s\n",
	       id, type ? type : "?", c.connector_type_id,
	       c.connection == 1 ? "connected" :
	       c.connection == 2 ? "disconnected" : "unknown",
	       c.mm_width, c.mm_height,
	       c.count_modes, c.count_modes == 1 ? "" : "s",
	       c.count_encoders, c.count_encoders == 1 ? "" : "s");

	if (modes) {
		for (i = 0; i < c.count_modes; i++) {
			if (!verbose && i >= 4) {
				printf("      ... %u more (-v for all)\n",
				       c.count_modes - i);
				break;
			}
			print_mode(&modes[i], "      mode ");
		}
		free(modes);
	}
	free(encoders);
}

static void print_encoder(int fd, unsigned int id)
{
	struct drm_mode_get_encoder e;
	const char *type;

	memset(&e, 0, sizeof(e));
	e.encoder_id = id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) == -1) {
		printf("    encoder %u:        unavailable (%s)\n", id,
		       strerror(errno));
		return;
	}
	type = lookup(encoder_types, e.encoder_type);
	printf("    encoder %u:        %s, crtc %u, possible crtcs 0x%x\n",
	       id, type ? type : "?", e.crtc_id, e.possible_crtcs);
}

static void print_crtc(int fd, unsigned int id)
{
	struct drm_mode_crtc c;

	memset(&c, 0, sizeof(c));
	c.crtc_id = id;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) == -1) {
		printf("    crtc %u:           unavailable (%s)\n", id,
		       strerror(errno));
		return;
	}
	if (c.mode_valid) {
		printf("    crtc %u:           fb %u at +%u+%u, mode ", id,
		       c.fb_id, c.x, c.y);
		print_mode(&c.mode, "");
	} else {
		printf("    crtc %u:           no mode set\n", id);
	}
}

static void print_planes(int fd)
{
	struct drm_mode_get_plane_res r;
	unsigned int *ids;
	unsigned int i;

	memset(&r, 0, sizeof(r));
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &r) == -1)
		return;
	if (!r.count_planes) {
		printf("    planes:           none\n");
		return;
	}
	ids = calloc(r.count_planes, sizeof(*ids));
	if (!ids)
		return;
	r.plane_id_ptr = (unsigned long long)(unsigned long)ids;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &r) == -1) {
		free(ids);
		return;
	}

	for (i = 0; i < r.count_planes; i++) {
		struct drm_mode_get_plane p;
		unsigned int *formats;
		unsigned int j;

		memset(&p, 0, sizeof(p));
		p.plane_id = ids[i];
		if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) == -1)
			continue;

		printf("    plane %u:          crtc %u, fb %u, possible crtcs 0x%x, %u format%s",
		       ids[i], p.crtc_id, p.fb_id, p.possible_crtcs,
		       p.count_format_types, p.count_format_types == 1 ? "" : "s");

		formats = NULL;
		if (p.count_format_types) {
			formats = calloc(p.count_format_types, sizeof(*formats));
			if (formats) {
				p.format_type_ptr =
					(unsigned long long)(unsigned long)formats;
				if (drm_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) == -1) {
					free(formats);
					formats = NULL;
				}
			}
		}
		if (formats) {
			printf(" (");
			for (j = 0; j < p.count_format_types; j++) {
				unsigned int f = formats[j];

				printf("%s%c%c%c%c", j ? " " : "",
				       (char)(f & 0xff), (char)((f >> 8) & 0xff),
				       (char)((f >> 16) & 0xff),
				       (char)((f >> 24) & 0xff));
			}
			printf(")");
			free(formats);
		}
		printf("\n");
	}
	free(ids);
}

static void print_kms(int fd)
{
	struct drm_mode_card_res r;
	unsigned int *fbs = NULL, *crtcs = NULL, *conns = NULL, *encs = NULL;
	unsigned int i;

	memset(&r, 0, sizeof(r));
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == -1) {
		printf("  mode setting:       unavailable (%s)\n", strerror(errno));
		return;
	}

	if (r.count_crtcs)
		crtcs = calloc(r.count_crtcs, sizeof(*crtcs));
	if (r.count_connectors)
		conns = calloc(r.count_connectors, sizeof(*conns));
	if (r.count_encoders)
		encs = calloc(r.count_encoders, sizeof(*encs));
	if (r.count_fbs)
		fbs = calloc(r.count_fbs, sizeof(*fbs));
	r.crtc_id_ptr = (unsigned long long)(unsigned long)crtcs;
	r.connector_id_ptr = (unsigned long long)(unsigned long)conns;
	r.encoder_id_ptr = (unsigned long long)(unsigned long)encs;
	r.fb_id_ptr = (unsigned long long)(unsigned long)fbs;
	if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == -1) {
		printf("  mode setting:       unavailable (%s)\n", strerror(errno));
		goto out;
	}

	printf("  mode setting:       %u crtc, %u connector, %u encoder, %u fb\n",
	       r.count_crtcs, r.count_connectors, r.count_encoders, r.count_fbs);
	printf("  framebuffer size:   %ux%u minimum, %ux%u maximum\n",
	       r.min_width, r.min_height, r.max_width, r.max_height);

	for (i = 0; conns && i < r.count_connectors; i++)
		print_connector(fd, conns[i]);
	for (i = 0; encs && i < r.count_encoders; i++)
		print_encoder(fd, encs[i]);
	for (i = 0; crtcs && i < r.count_crtcs; i++)
		print_crtc(fd, crtcs[i]);
	print_planes(fd);

out:
	free(crtcs);
	free(conns);
	free(encs);
	free(fbs);
}

static void print_vmw(int fd)
{
	const struct name_map *m;

	printf("  vmwgfx parameters:\n");
	for (m = vmw_params; m->name; m++) {
		struct drm_vmw_getparam_arg a;

		memset(&a, 0, sizeof(a));
		a.param = m->value;
		if (drm_ioctl(fd, DRMINFO_IOCTL_VMW_GET_PARAM, &a) == -1)
			continue;
		if (m->value == DRM_VMW_PARAM_HW_CAPS ||
		    m->value == DRM_VMW_PARAM_HW_CAPS2 ||
		    m->value == DRM_VMW_PARAM_FIFO_CAPS ||
		    m->value == DRM_VMW_PARAM_DEVICE_ID)
			printf("    %-22s 0x%llx\n", m->name,
			       (unsigned long long)a.value);
		else
			printf("    %-22s %llu\n", m->name,
			       (unsigned long long)a.value);
	}
}

/* Which driver this node belongs to, so the driver-specific section is only
 * attempted where it means something. */
static void driver_name(int fd, char *out, size_t cap)
{
	struct drm_version v;

	out[0] = '\0';
	memset(&v, 0, sizeof(v));
	if (drm_ioctl(fd, DRM_IOCTL_VERSION, &v) == -1)
		return;
	if (v.name_len >= cap)
		v.name_len = cap - 1;
	v.name = out;
	if (drm_ioctl(fd, DRM_IOCTL_VERSION, &v) == -1) {
		out[0] = '\0';
		return;
	}
	out[v.name_len] = '\0';
}

/* The Intel driver's parameters: what the device is and how it is built,
 * as GETPARAM answers them.  A parameter the driver does not answer yet is
 * shown as such rather than skipped, so the list doubles as a record of
 * what is implemented. */
static void print_i915(int fd)
{
	static const struct {
		int param;
		const char *name;
		int hex;
	} params[] = {
		{ I915_PARAM_CHIPSET_ID, "chipset id", 1 },
		{ I915_PARAM_REVISION, "revision", 1 },
		{ I915_PARAM_HAS_LLC, "has LLC", 0 },
		{ I915_PARAM_SLICE_MASK, "slice mask", 1 },
		{ I915_PARAM_SUBSLICE_MASK, "subslice mask", 1 },
		{ I915_PARAM_SUBSLICE_TOTAL, "subslices", 0 },
		{ I915_PARAM_EU_TOTAL, "execution units", 0 },
		{ I915_PARAM_CS_TIMESTAMP_FREQUENCY, "timestamp Hz", 0 },
		{ I915_PARAM_MMAP_GTT_VERSION, "mmap version", 0 },
		{ I915_PARAM_HAS_EXEC_SOFTPIN, "softpin", 0 },
		{ I915_PARAM_HAS_EXEC_NO_RELOC, "no-reloc", 0 },
		{ I915_PARAM_HAS_EXEC_HANDLE_LUT, "handle LUT", 0 },
		{ I915_PARAM_HAS_EXEC_BATCH_FIRST, "batch first", 0 },
		{ I915_PARAM_HAS_EXEC_FENCE_ARRAY, "fence arrays", 0 },
		{ I915_PARAM_HAS_CONTEXT_ISOLATION, "context isolation", 0 },
		{ I915_PARAM_HAS_GPU_RESET, "gpu reset", 0 },
		{ I915_PARAM_HAS_SCHEDULER, "scheduler caps", 1 },
		{ I915_PARAM_HAS_BSD, "video engine", 0 },
		{ I915_PARAM_HAS_BSD2, "second video engine", 0 },
		{ I915_PARAM_HAS_VEBOX, "video enhance engine", 0 },
		{ I915_PARAM_HUC_STATUS, "media firmware (HuC)", 0 },
		{ I915_PARAM_NUM_FENCES_AVAIL, "fence registers", 0 },
		{ I915_PARAM_MMAP_VERSION, "legacy mmap version", 0 },
	};
	printf("  i915 parameters:\n");
	for (unsigned i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct drm_i915_getparam gp;
		int value = 0;
		gp.param = params[i].param;
		gp.value = &value;
		if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == -1) {
			printf("    %-20s (%s)\n", params[i].name,
			       errno == EINVAL ? "not implemented" :
						 strerror(errno));
			continue;
		}
		if (params[i].hex)
			printf("    %-20s 0x%x\n", params[i].name, value);
		else
			printf("    %-20s %d\n", params[i].name, value);
	}
	/* A round trip through the object and context interface: what the
	 * Mesa driver does first when it opens the device. */
	{
		struct drm_i915_gem_create c;
		struct drm_i915_gem_mmap_offset mo;
		struct drm_i915_gem_context_create cc;
		struct drm_i915_gem_context_destroy cd;
		struct drm_i915_gem_vm_control vm;
		struct drm_gem_close gc;
		memset(&c, 0, sizeof(c));
		c.size = 65536;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &c) == 0) {
			memset(&mo, 0, sizeof(mo));
			mo.handle = c.handle;
			mo.flags = I915_MMAP_OFFSET_WB;
			int mrc = ioctl(fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &mo);
			printf("    object create        handle %u, %llu bytes, mmap offset %s\n",
			       c.handle, (unsigned long long)c.size,
			       mrc == 0 ? "ok" : strerror(errno));
			if (mrc == 0) {
				void *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
					       (off_t)mo.offset);
				if (p != MAP_FAILED) {
					((volatile uint32_t *)p)[0] = 0x12345678;
					printf("    object mmap          ok (%s)\n",
					       ((volatile uint32_t *)p)[0] == 0x12345678 ? "readback ok" : "readback FAILED");
					munmap(p, 65536);
				} else {
					printf("    object mmap          %s\n", strerror(errno));
				}
			}
			memset(&gc, 0, sizeof(gc));
			gc.handle = c.handle;
			ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		} else {
			printf("    object create        %s\n", strerror(errno));
		}
		memset(&cc, 0, sizeof(cc));
		if (ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &cc) == 0) {
			printf("    context create       id %u\n", cc.ctx_id);
			memset(&cd, 0, sizeof(cd));
			cd.ctx_id = cc.ctx_id;
			ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &cd);
		} else {
			printf("    context create       %s\n", strerror(errno));
		}
		memset(&vm, 0, sizeof(vm));
		if (ioctl(fd, DRM_IOCTL_I915_GEM_VM_CREATE, &vm) == 0) {
			printf("    vm create            id %u\n", vm.vm_id);
			ioctl(fd, DRM_IOCTL_I915_GEM_VM_DESTROY, &vm);
		} else {
			printf("    vm create            %s\n", strerror(errno));
		}
		/* engines and topology through QUERY */
		struct drm_i915_query_item it[2];
		struct drm_i915_query q;
		memset(it, 0, sizeof(it));
		it[0].query_id = DRM_I915_QUERY_ENGINE_INFO;
		it[1].query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
		memset(&q, 0, sizeof(q));
		q.num_items = 2;
		q.items_ptr = (uint64_t)(uintptr_t)it;
		if (ioctl(fd, DRM_IOCTL_I915_QUERY, &q) == 0) {
			printf("    query engine info    %d bytes\n", it[0].length);
			printf("    query topology       %d bytes\n", it[1].length);
			if (it[0].length > 0) {
				char *buf = calloc(1, (size_t)it[0].length);
				it[0].data_ptr = (uint64_t)(uintptr_t)buf;
				q.num_items = 1;
				if (buf && ioctl(fd, DRM_IOCTL_I915_QUERY, &q) == 0) {
					struct drm_i915_query_engine_info *ei = (void *)buf;
					static const char *cls[] = { "render", "copy", "video", "video-enhance", "compute" };
					printf("    engines             ");
					for (unsigned k = 0; k < ei->num_engines; k++) {
						uint64_t caps = ei->engines[k].capabilities;
						printf(" %s%u%s%s",
						       ei->engines[k].engine.engine_class < 5 ?
							       cls[ei->engines[k].engine.engine_class] : "?",
						       ei->engines[k].engine.engine_instance,
						       (caps & I915_VIDEO_CLASS_CAPABILITY_HEVC) ? "(hevc" : "",
						       (caps & I915_VIDEO_AND_ENHANCE_CLASS_CAPABILITY_SFC) ?
							       ((caps & I915_VIDEO_CLASS_CAPABILITY_HEVC) ? ",sfc)" : "(sfc)") :
							       ((caps & I915_VIDEO_CLASS_CAPABILITY_HEVC) ? ")" : ""));
					}
					printf("\n");
				}
				free(buf);
			}
		} else {
			printf("    query                %s\n", strerror(errno));
		}
	}
}

/* Does the node offer a display: at least one connector and one CRTC?
 * A driver that has found its device but not yet lit a screen (or a
 * render-only device) answers no, and the X server's start-up script then
 * keeps the framebuffer server. */
static int has_display(const char *node)
{
	struct drm_mode_card_res res;
	int fd = open(node, O_RDWR);

	if (fd < 0)
		return 1;
	memset(&res, 0, sizeof(res));
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == -1) {
		close(fd);
		return 1;
	}
	close(fd);
	return (res.count_connectors > 0 && res.count_crtcs > 0) ? 0 : 1;
}

/* Does the node's driver render 3D on the device itself?  i915 always does;
 * vmwgfx only when the virtual device offers 3D -- the VM's "Accelerate 3D
 * graphics" setting, which the kernel reads from the device and answers as
 * DRM_VMW_PARAM_3D (the same question Mesa's svga driver asks before it
 * binds).  Anything else answers no.  The session start-up scripts decide
 * from this whether X runs glamor and whether GL clients are pinned to the
 * software rasteriser, so the two always agree. */
static int has_3d(const char *node)
{
	char name[32];
	int fd = open(node, O_RDWR);
	int ret = 1;

	if (fd < 0)
		return 1;
	driver_name(fd, name, sizeof(name));
	if (!strcmp(name, "i915")) {
		ret = 0;
	} else if (!strcmp(name, "vmwgfx")) {
		struct drm_vmw_getparam_arg a;

		memset(&a, 0, sizeof(a));
		a.param = DRM_VMW_PARAM_3D;
		if (drm_ioctl(fd, DRMINFO_IOCTL_VMW_GET_PARAM, &a) == 0 && a.value)
			ret = 0;
	}
	close(fd);
	return ret;
}

static int examine(const char *path)
{
	char name[64];
	struct stat st;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd == -1) {
		printf("%s: %s\n", path, strerror(errno));
		return -1;
	}

	printf("%s\n", path);
	if (fstat(fd, &st) == 0)
		printf("  device number:      %u:%u\n", major(st.st_rdev),
		       minor(st.st_rdev));

	print_version(fd);
	print_caps(fd);

	driver_name(fd, name, sizeof(name));
	if (!strcmp(name, "vmwgfx"))
		print_vmw(fd);
	else if (!strcmp(name, "i915"))
		print_i915(fd);

	print_kms(fd);
	printf("\n");
	close(fd);
	return 0;
}

static int examine_all(void)
{
	DIR *d;
	struct dirent *e;
	char paths[16][64];
	int n = 0, i;

	d = opendir("/dev/dri");
	if (!d) {
		fprintf(stderr, "drminfo: /dev/dri: %s\n", strerror(errno));
		fprintf(stderr,
			"drminfo: no display-manager node -- the kernel bound no GPU driver,\n"
			"         so the system is running on the framebuffer console and X\n"
			"         falls back to /etc/X11/xorg.conf (fbdev, software rendering).\n");
		return 1;
	}

	while ((e = readdir(d)) && n < (int)(sizeof(paths) / sizeof(paths[0]))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(paths[n], sizeof(paths[n]), "/dev/dri/%s", e->d_name);
		n++;
	}
	closedir(d);

	if (!n) {
		fprintf(stderr, "drminfo: /dev/dri is empty\n");
		return 1;
	}

	/* Sorted, so card0 comes before renderD128 whatever order the
	 * directory happens to hand back. */
	for (i = 1; i < n; i++) {
		char tmp[64];
		int j = i;

		while (j > 0 && strcmp(paths[j - 1], paths[j]) > 0) {
			memcpy(tmp, paths[j - 1], sizeof(tmp));
			memcpy(paths[j - 1], paths[j], sizeof(tmp));
			memcpy(paths[j], tmp, sizeof(tmp));
			j--;
		}
	}

	for (i = 0; i < n; i++)
		examine(paths[i]);
	return 0;
}

/* The driver's name alone, for scripts.  Nothing else is printed, so the
 * output can be compared with a string. */
static int print_driver(const char *node)
{
	struct drm_version v;
	char name[32];
	int fd = open(node, O_RDWR);

	if (fd < 0)
		return 1;
	memset(&v, 0, sizeof(v));
	memset(name, 0, sizeof(name));
	v.name = name;
	v.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &v) == -1) {
		close(fd);
		return 1;
	}
	close(fd);
	if (!name[0])
		return 1;
	printf("%s\n", name);
	return 0;
}

int main(int argc, char **argv)
{
	const char *node = NULL;
	char path[64];
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			node = argv[++i];
		} else if (!strcmp(argv[i], "--has-display")) {
			const char *n = "/dev/dri/card0";
			if (i + 1 < argc)
				n = argv[++i];
			if (n[0] >= '0' && n[0] <= '9') {
				snprintf(path, sizeof(path), "/dev/dri/card%s", n);
				n = path;
			}
			return has_display(n);
		} else if (!strcmp(argv[i], "--has-3d")) {
			const char *n = "/dev/dri/card0";
			if (i + 1 < argc)
				n = argv[++i];
			if (n[0] >= '0' && n[0] <= '9') {
				snprintf(path, sizeof(path), "/dev/dri/card%s", n);
				n = path;
			}
			return has_3d(n);
		} else if (!strcmp(argv[i], "--driver")) {
			const char *n = "/dev/dri/card0";
			if (i + 1 < argc)
				n = argv[++i];
			if (n[0] >= '0' && n[0] <= '9') {
				snprintf(path, sizeof(path), "/dev/dri/card%s", n);
				n = path;
			}
			return print_driver(n);
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("usage: drminfo [-n node] [-v] [--driver [node]]\n"
			       "  -n   one node: a path, or a number N for /dev/dri/card<N>\n"
			       "  -v   list every mode rather than the first few\n"
			       "  --driver   print the driver name of the node (card0) only\n"
			       "  --has-display   exit 0 when the node has a connector and a CRTC\n"
			       "  --has-3d   exit 0 when the node's driver renders 3D on the device\n"
			       "             (i915; vmwgfx with the VM's 3D acceleration on)\n");
			return 0;
		} else {
			fprintf(stderr, "drminfo: unknown argument '%s'\n", argv[i]);
			return 2;
		}
	}

	if (!node)
		return examine_all();

	if (node[0] >= '0' && node[0] <= '9') {
		snprintf(path, sizeof(path), "/dev/dri/card%s", node);
		node = path;
	}
	return examine(node) == 0 ? 0 : 1;
}
