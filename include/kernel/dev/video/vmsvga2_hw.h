// LikeOS-64 -- VMware SVGA II hardware access exported to the
// display-manager driver (kernel/dev/gpu/vmwgfx).
//
// The boot console driver (vmsvga2.c) keeps the register file, the FIFO
// engine and the fence interrupt; the display-manager driver builds its
// command streams, KMS and buffer objects on top of these.
#ifndef KERNEL_DEV_VIDEO_VMSVGA2_HW_H
#define KERNEL_DEV_VIDEO_VMSVGA2_HW_H

#include <kernel/uapi/types.h>
#include <kernel/hal/pci.h>

struct vmsvga2_hw_geometry {
	uint64_t fb_phys;
	uint8_t *fb_virt;
	uint32_t vram_size;
	uint32_t max_width, max_height;
	uint32_t width, height, bpp, pitch, fb_offset; /* current scanout */
	uint32_t caps, fifo_caps, fifo_size;
	int irq_enabled;
};

typedef void (*vmsvga2_hw_irq_cb_t)(uint32_t irq_status);

int vmsvga2_hw_present(void);
const pci_device_t *vmsvga2_hw_pci(void);
uint32_t vmsvga2_hw_read_reg(uint32_t index);
void vmsvga2_hw_write_reg(uint32_t index, uint32_t value);
int vmsvga2_hw_has_fifo_cap(uint32_t cap);
int vmsvga2_hw_has_fifo_reg(uint32_t reg);
uint32_t vmsvga2_hw_fifo_reg(uint32_t reg);
void vmsvga2_hw_geometry(struct vmsvga2_hw_geometry *g);

/* FIFO: one atomic command from a buffer, or reserve/fill/commit. */
int vmsvga2_hw_fifo_submit(const void *data, uint32_t bytes);
/* ...queued without ringing; finish the run with vmsvga2_hw_doorbell(). */
int vmsvga2_hw_fifo_submit_batch(const void *data, uint32_t bytes);
void *vmsvga2_hw_fifo_reserve(uint32_t bytes);
void vmsvga2_hw_fifo_commit(uint32_t bytes);
void vmsvga2_hw_fifo_abort(void);
void vmsvga2_hw_doorbell(void);

/* Fences: the device's last-passed fence, the goal IRQ, the IRQ hook. */
int vmsvga2_hw_has_fence(void);
uint32_t vmsvga2_hw_fence_current(void);
void vmsvga2_hw_set_fence_goal(uint32_t goal);
void vmsvga2_hw_set_irq_callback(vmsvga2_hw_irq_cb_t cb);

/* Display hand-over between the console and a display-manager master. */
void vmsvga2_hw_display_take(void);
void vmsvga2_hw_display_release(void);
int vmsvga2_hw_display_taken(void);
int vmsvga2_hw_set_mode(uint32_t width, uint32_t height);
/* ...and the same without the console following it, for a console that
 * scans out of a buffer object rather than out of this device's memory. */
int vmsvga2_hw_set_mode_device(uint32_t width, uint32_t height);
/* The resolution the host recommends, sampled once at probe; <0 when the
 * hypervisor gave no answer.  Declared in vmsvga2.h too. */
int vmsvga2_get_host_preferred(uint32_t *width, uint32_t *height);

/* Console-driver services the display-manager backend reuses (also
 * declared in vmsvga2.h, which carries its own register definitions and
 * so cannot be included beside the full device headers). */
int vmsvga2_gmr_alloc(uint32_t num_pages);
int vmsvga2_gmr_bind(int gmr_id, const uint64_t *page_phys, uint32_t num_pages);
int vmsvga2_gmr_free(int gmr_id);
void vmsvga2_update_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void vmsvga2_set_traces(int enable);
int vmsvga2_display_enable(int enable);
void vmsvga2_fifo_flush(void);
uint32_t vmsvga2_fence_insert(void);
uint32_t vmsvga2_fence_alloc(void);
/* Tell this layer that something above it owns a command-buffer channel on
 * the same device: it then emits no FIFO fences of its own, so the device's
 * one fence register has a single writer.  See vmsvga2.c. */
void vmsvga2_set_cmdbuf_owner(int on);
int vmsvga2_cursor_define_alpha(uint32_t width, uint32_t height, uint32_t hot_x,
				uint32_t hot_y, const uint32_t *argb_pixels);
int vmsvga2_cursor_move(int32_t x, int32_t y, int visible);
int vmsvga2_cursor_show(int visible);

#endif
