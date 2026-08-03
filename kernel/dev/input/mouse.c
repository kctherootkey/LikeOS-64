// LikeOS-64 I/O Subsystem - Mouse Driver
// PS/2 mouse input handling and cursor management

#include <kernel/dev/video/fbdev.h>
#include <kernel/dev/input/mouse.h>
#include <kernel/ke/interrupt.h>
#include <kernel/dev/video/fb.h>
#include <kernel/dev/video/vmsvga2.h>
#include <kernel/dev/input/evdev.h>
#include <kernel/mm/memory.h>
#include <kernel/io/console.h>
#include <kernel/ke/sched.h>
#include <kernel/io/cursor.h>
#include <kernel/io/tty.h>
#include <kernel/hal/ioapic.h>
#include <kernel/dev/input/keyboard.h>
#include <kernel/uapi/bug.h>

// Global mouse state
static mouse_state_t mouse_state = { 0 };

// Flag indicating whether to use loaded cursor or built-in
static int use_loaded_cursor = 0;

// Spinlock for mouse state protection
static spinlock_t mouse_lock = SPINLOCK_INIT("mouse");

// How many top rows of the arrow remain visible at the bottom edge
#define TIP_VISIBLE_ROWS 3
// Sentinel color stored in background buffer for "not saved" entries

// Forward declaration - internal cursor update (caller must hold mouse_lock)
static void mouse_update_cursor_internal(void);


// Mouse cursor bitmap (11x19 Windows-style arrow cursor)
static const uint32_t cursor_bitmap[CURSOR_HEIGHT][CURSOR_WIDTH] = {
	// Classic Windows cursor: white arrow with black outline
	{ 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFF000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	  0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000, 0xFF000000,
	  0xFF000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
	  0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0xFF000000,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0xFF000000, 0x00000000, 0x00000000, 0xFF000000,
	  0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000,
	  0x00000000 },
	{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, 0x00000000,
	  0x00000000 },
	{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000, 0xFF000000, 0xFF000000, 0x00000000, 0x00000000,
	  0x00000000 },
	{ 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	  0x00000000 }
};

/* The pointer image handed to the framebuffer layer, which composites it into
 * the front buffer on every flush.  Flat ARGB so it can be blended without
 * calling back here, and static because the overlay holds the pointer for as
 * long as it is installed. */
static uint32_t g_pointer_image[CURSOR_MAX_WIDTH * CURSOR_MAX_HEIGHT];


// Helper to get cursor pixel at position (from loaded cursor or built-in)
static inline uint32_t get_cursor_pixel(int cx, int cy)
{
	if (use_loaded_cursor && cursor_is_loaded()) {
		return cursor_get_pixel((uint32_t)cx, (uint32_t)cy);
	}
	// Built-in cursor
	if (cx >= 0 && cx < CURSOR_WIDTH && cy >= 0 && cy < CURSOR_HEIGHT) {
		return cursor_bitmap[cy][cx];
	}
	return 0;
}

/* Rebuild g_pointer_image from whichever cursor is in effect and install it. */
static void mouse_publish_pointer_image(void)
{
	int w = mouse_state.cursor_w;
	int h = mouse_state.cursor_h;

	if (w <= 0 || h <= 0 || w > CURSOR_MAX_WIDTH || h > CURSOR_MAX_HEIGHT) {
		fb_pointer_set_image(NULL, 0, 0);
		return;
	}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			g_pointer_image[y * w + x] = get_cursor_pixel(x, y);
	fb_pointer_set_image(g_pointer_image, (uint32_t)w, (uint32_t)h);
}

// ---------------------------------------------------------------------------
// Hardware cursor (VMware SVGA II): when the display driver owns the screen
// and exposes a hardware cursor, position updates go straight to the device
// and the software cursor (back-buffer drawing) is bypassed entirely.
// ---------------------------------------------------------------------------

static int g_hw_cursor_defined = 0;
/* Is the DEVICE currently displaying our hardware cursor?  Tracked so the hide
 * below happens once rather than on every packet -- see mouse_hw_cursor_update(). */
static int g_hw_cursor_shown = 0;

// Build + upload the current effective cursor image as a hardware cursor.
static int mouse_hw_cursor_define(void)
{
	int w = mouse_state.cursor_w;
	int h = mouse_state.cursor_h;

	if (w <= 0 || h <= 0 || w > 64 || h > 64)
		return -1;

	if (vmsvga2_get_caps() & SVGA_CAP_ALPHA_CURSOR) {
		static uint32_t px[64 * 64];
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				px[y * w + x] = get_cursor_pixel(x, y);
		return vmsvga2_cursor_define_alpha((uint32_t)w, (uint32_t)h, 0,
						   0, px);
	}

	// Monochrome AND/XOR cursor: transparent = AND 1 / XOR 0,
	// black = AND 0 / XOR 0, anything else = AND 0 / XOR 1 (white).
	// Wire format is byte-oriented MSB-first scanlines padded to 32 bits
	// (pixel 0 = byte 0 bit 7, matching the VirtualBox/QEMU decoders).
	// The width itself is padded to a dword multiple with transparent
	// columns: QEMU's vmware-vga walks mono rows with byte-aligned
	// stride instead of the spec's dword alignment, and at 32/64-pixel
	// widths the two agree.
	{
		uint32_t and_mask[64 * 2];
		uint32_t xor_mask[64 * 2] = { 0 };
		uint8_t *andb = (uint8_t *)and_mask;
		uint8_t *xorb = (uint8_t *)xor_mask;
		int pw = (w + 31) & ~31;
		int row_bytes = pw / 8;

		for (unsigned i = 0; i < sizeof(and_mask) / sizeof(and_mask[0]);
		     i++)
			and_mask[i] = 0xFFFFFFFFu; // default: transparent
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint32_t p = get_cursor_pixel(x, y);
				int i = y * row_bytes + (x >> 3);
				uint8_t bit = (uint8_t)(0x80u >> (x & 7));

				if ((p >> 24) >= 0x80) {
					andb[i] &= (uint8_t)~bit;
					if (p & 0xFFFFFF)
						xorb[i] |= bit;
				}
			}
		}
		return vmsvga2_cursor_define((uint32_t)pw, (uint32_t)h, 0, 0,
					     and_mask, xor_mask);
	}
}

/* Take the hardware cursor down for as long as another program owns the screen.
 *
 * Clearing the visible flag is what the interface offers, and on its own it is
 * not enough everywhere: VirtualBox kept drawing ours after being told to stop,
 * leaving a second pointer on screen beside the one X was drawing.  So a fully
 * transparent shape is uploaded as well -- a cursor with no opaque pixel cannot
 * be drawn whatever the host decides about the flag.
 *
 * The real shape is dropped with it, so it is uploaded again when the screen
 * comes back.
 */
static void mouse_hw_cursor_hide(void)
{
	/* Never written, so it is a whole cursor's worth of zeroes: alpha 0 in
	 * every pixel.  Sized for the largest cursor the device accepts. */
	static const uint32_t transparent[CURSOR_MAX_WIDTH * CURSOR_MAX_HEIGHT];

	vmsvga2_cursor_show(0);

	if (vmsvga2_get_caps() & SVGA_CAP_ALPHA_CURSOR) {
		int w = mouse_state.cursor_w;
		int h = mouse_state.cursor_h;

		if (w > 0 && h > 0 && w <= CURSOR_MAX_WIDTH &&
		    h <= CURSOR_MAX_HEIGHT)
			vmsvga2_cursor_define_alpha((uint32_t)w, (uint32_t)h, 0,
						    0, transparent);
	}

	g_hw_cursor_defined = 0; /* the real shape is gone; re-upload it later */
	g_hw_cursor_shown = 0;
}

// Returns 1 when the hardware cursor handled this update (no software draw).
static int mouse_hw_cursor_update(void)
{
	if (!vmsvga2_active() || !vmsvga2_has_hw_cursor())
		return 0;
	/* While another program owns the framebuffer it draws its own pointer.
	 * The hardware cursor is composited by the DEVICE, so it would appear
	 * on top of that program's output no matter what the console does --
	 * hide it, and report the update as handled so the software path does
	 * not draw one either.
	 *
	 * Hidden ONCE, and then the device is left alone.  This used to send a
	 * position update with the visible flag clear on every packet, which is
	 * a cursor command however it is labelled: a host that treats any
	 * cursor update as a reason to display one kept drawing ours, tracking
	 * the kernel's pointer a little away from the one X was drawing from
	 * the same movements.  That is the two-pointers-in-lockstep seen under
	 * VirtualBox.  Not writing the position at all leaves nothing to
	 * re-assert. */
	if (fbdev_display_owned()) {
		if (g_hw_cursor_shown)
			mouse_hw_cursor_hide();
		mouse_state.last_x = mouse_state.x;
		mouse_state.last_y = mouse_state.y;
		return 1;
	}
	// Alpha-cursor hosts (VMware/VirtualBox) composite the cursor
	// themselves.  Mono-only hosts (QEMU) set the HOST pointer shape to
	// the guest cursor — but with a relative pointing device the host
	// pointer is grabbed and hidden, so the hardware cursor can never be
	// seen there.  Use the software cursor instead.
	if (!(vmsvga2_get_caps() & SVGA_CAP_ALPHA_CURSOR))
		return 0;
	if (!g_hw_cursor_defined) {
		if (mouse_hw_cursor_define() != 0)
			return 0;
		g_hw_cursor_defined = 1;
	}
	vmsvga2_cursor_move(mouse_state.x, mouse_state.y,
			    mouse_state.cursor_visible);
	g_hw_cursor_shown = mouse_state.cursor_visible ? 1 : 0;
	mouse_state.last_x = mouse_state.x;
	mouse_state.last_y = mouse_state.y;
	return 1;
}

/*
 * Make sure exactly one pointer is drawn.
 *
 * The console can draw the pointer two ways: the display device composites a
 * hardware cursor, or the framebuffer layer composites the software overlay.
 * Enabling both paints two, and they do not move together -- the device's
 * follows the mouse while the overlay stays wherever it was last positioned,
 * so the second one appears frozen somewhere on screen.  That is what turning
 * the overlay on unconditionally at init produced on a VMware guest, where the
 * hardware cursor takes over later in boot.
 *
 * `hw_handled` is the answer mouse_hw_cursor_update() gave, not a predicate
 * re-derived from vmsvga2_active(): whether the device really draws the
 * pointer also depends on the host's cursor capabilities and on whether the
 * shape upload succeeded, and only that function knows how it came out.
 */
static void mouse_sync_overlay(int hw_handled)
{
	fb_pointer_set_visible(!hw_handled && mouse_state.cursor_visible);
}

/*
 * Another program has taken the display.
 *
 * Called on the FIRST open of /dev/fb0 rather than waiting for the next mouse
 * packet to notice: the hardware cursor is composited by the device and would
 * otherwise sit on top of that program's output until the pointer next moved.
 */
void mouse_console_display_taken(void)
{
	if (!vmsvga2_active() || !vmsvga2_has_hw_cursor())
		return;
	if (g_hw_cursor_shown)
		mouse_hw_cursor_hide();
}

/*
 * The display has been handed back to the console: make the pointer work again.
 *
 * On a host with an alpha hardware cursor the console pointer is NOT drawn into
 * the framebuffer at all -- the device composites it, from a shape uploaded
 * once and then only repositioned.  That shape lives in device memory which a
 * mode set clears, so after another program has driven the display it is gone,
 * while g_hw_cursor_defined still claims otherwise.  cursor_move() then
 * addresses a shape that no longer exists: the pointer stops moving, and
 * nothing about that suggests the cursor image is the problem.  Clearing the
 * flag makes the next update upload it again.
 *
 * The software path needs only to say that the rectangle the pointer occupies
 * is suspect, which it is -- the other program drew over it.  There is no saved
 * background to discard, because the overlay keeps none.
 */
void mouse_console_display_released(void)
{
	g_hw_cursor_defined = 0;
	g_hw_cursor_shown = 0;
	fb_pointer_damage();
	mouse_state.last_x = mouse_state.x;
	mouse_state.last_y = mouse_state.y;
	/* Decides which pointer draws and repaints it. */
	mouse_update_cursor_internal();
}

// Wait for PS/2 controller input buffer to be ready
static void mouse_wait_input(void)
{
	int timeout = 100000;
	while ((inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) &&
	       timeout-- > 0) {
		// Wait for input buffer to be empty
	}
}

// Wait for PS/2 controller output buffer to have data
static void mouse_wait_output(void)
{
	int timeout = 100000;
	while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) &&
	       timeout-- > 0) {
		// Wait for output buffer to have data
	}
}

// Read data from PS/2 controller
static uint8_t mouse_read_data(void)
{
	mouse_wait_output();
	return inb(PS2_DATA_PORT);
}

// Flush any pending bytes from controller output (bounded)
static void mouse_flush_output(void)
{
	for (int i = 0; i < 32; i++) {
		if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
			(void)inb(PS2_DATA_PORT);
		} else {
			break;
		}
	}
}

/* Read one byte, or -1 if none arrives before the spin budget runs out.
 *
 * mouse_read_data() reads the port whether or not a byte ever showed up, so a
 * timeout there returns whatever the empty port floats to and the caller
 * cannot tell that apart from a real response. */
static int mouse_try_read(void)
{
	for (int spin = 0; spin < 200000; spin++) {
		if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
			return (int)inb(PS2_DATA_PORT);
	}
	return -1;
}

/* Read until `want` appears, discarding whatever comes first.  Returns 1 if it
 * was found.
 *
 * The answer to a PS/2 command has to be FOUND, not counted.  A device left
 * streaming by the firmware keeps sending movement packets while the driver is
 * talking to it, so the next byte in the buffer is not necessarily the reply to
 * the last command -- with a hand on the TrackPoint it is just as likely to be
 * the middle of a movement packet.  Reading positionally then shifts every
 * later read by one, and the detection sequence below comes out with the wrong
 * answer: a wheel mouse framed as a 3-byte device (or the reverse) never
 * resynchronises, because the framing check keys on a bit that is only in the
 * first byte of a correctly-aligned packet.  The pointer then stops responding
 * for the rest of the session -- which is exactly what moving the pointer
 * during boot used to produce. */
static int mouse_expect(uint8_t want, int budget)
{
	for (int i = 0; i < budget; i++) {
		int b = mouse_try_read();
		if (b < 0)
			return 0;
		if ((uint8_t)b == want)
			return 1;
	}
	return 0;
}


static uint8_t mouse_read_controller_config(void)
{
	mouse_wait_input();
	outb(PS2_COMMAND_PORT, PS2_CMD_READ_CONFIG);
	return mouse_read_data();
}

static void mouse_write_controller_config(uint8_t config)
{
	mouse_wait_input();
	outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_CONFIG);
	mouse_wait_input();
	outb(PS2_DATA_PORT, config);
}

// Write command to mouse via PS/2 controller
static void mouse_write_command(uint8_t cmd, uint8_t data)
{
	mouse_wait_input();
	outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);

	mouse_wait_input();
	outb(PS2_DATA_PORT, cmd);

	if (data != 0xFF) { // 0xFF means no data byte
		// Consume ACK for the command byte before sending data.
		// Without this, the ACK stays in the output buffer and
		// desynchronises subsequent reads (especially with IRQ12
		// masked during init, where nothing else drains it).
		mouse_wait_output();
		(void)inb(PS2_DATA_PORT);

		mouse_wait_input();
		outb(PS2_COMMAND_PORT, PS2_CMD_WRITE_PORT2);

		mouse_wait_input();
		outb(PS2_DATA_PORT, data);
	}
}

/* Stop the device sending, so the command sequence that follows is talking to
 * something quiet.  Everything else here depends on this having worked. */
static void mouse_quiesce(void)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		mouse_flush_output();
		mouse_write_command(MOUSE_CMD_DISABLE_REPORTING, 0xFF);
		if (!mouse_expect(MOUSE_ACK, 8))
			continue;
		/* Drain what was already in flight when it stopped, and
		 * confirm it stays quiet -- a device that is still streaming
		 * refills the buffer between these two checks. */
		mouse_flush_output();
		if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL))
			return;
	}
	mouse_flush_output();
}

// Detect mouse type and capabilities
static uint8_t mouse_detect_type(void)
{
	int response;

	kprintf("  Attempting mouse reset...\n");
	mouse_write_command(MOUSE_CMD_RESET, 0xFF);

	/* The reset reply is ACK, then 0xAA (self-test passed), then the device
	 * ID.  Find the 0xAA rather than counting bytes to it: anything the
	 * device sent before the reset took effect is still ahead of it in the
	 * buffer.  The budget is generous because a reset takes a while and the
	 * pre-reset stream can be several packets long. */
	if (!mouse_expect(0xAA, 32)) {
		kprintf("  Mouse self-test byte not seen; assuming standard mouse\n");
		mouse_state.has_scroll_wheel = 0;
		mouse_state.packet_size = 3;
		mouse_flush_output();
		return MOUSE_TYPE_STANDARD;
	}

	response = mouse_try_read(); // Device ID
	kprintf("  Device ID: 0x%02X\n", response);
	if (response != 0x00) {
		kprintf("  Mouse reset failed or not standard mouse\n");
		mouse_state.has_scroll_wheel = 0;
		mouse_state.packet_size = 3;
		mouse_flush_output();
		return MOUSE_TYPE_STANDARD;
	}

	/* A reset leaves reporting disabled, so from here the device only speaks
	 * when spoken to -- provided every ACK below is consumed. */
	kprintf("  Attempting IntelliMouse detection sequence...\n");
	static const uint8_t knock[3] = { 200, 100, 80 };
	for (int i = 0; i < 3; i++) {
		mouse_write_command(MOUSE_CMD_SET_SAMPLE_RATE, knock[i]);
		if (!mouse_expect(MOUSE_ACK, 8)) {
			kprintf("  Sample rate %u not acknowledged; standard mouse\n",
				knock[i]);
			mouse_state.has_scroll_wheel = 0;
			mouse_state.packet_size = 3;
			mouse_flush_output();
			return MOUSE_TYPE_STANDARD;
		}
	}

	kprintf("  Getting device ID after sequence...\n");
	mouse_write_command(MOUSE_CMD_GET_DEVICE_ID, 0xFF);
	if (!mouse_expect(MOUSE_ACK, 8)) {
		kprintf("  Get ID not acknowledged; standard mouse\n");
		mouse_state.has_scroll_wheel = 0;
		mouse_state.packet_size = 3;
		mouse_flush_output();
		return MOUSE_TYPE_STANDARD;
	}
	response = mouse_try_read();
	kprintf("  New Device ID: 0x%02X\n", response);

	if (response == MOUSE_TYPE_INTELLIMOUSE) {
		kprintf("  IntelliMouse detected (scroll wheel supported)\n");
		mouse_state.has_scroll_wheel = 1;
		mouse_state.packet_size = 4;
		return MOUSE_TYPE_INTELLIMOUSE;
	}

	// Standard 3-byte mouse - don't force IntelliMouse mode
	// VirtualBox and some other emulators don't support 4-byte packets
	kprintf("  Standard mouse detected (3-byte mode)\n");
	mouse_state.has_scroll_wheel = 0;
	mouse_state.packet_size = 3;
	return MOUSE_TYPE_STANDARD;
}

// Draw cursor at specified position
static void mouse_process_packet(void)
{
	uint8_t flags = mouse_state.packet_buffer[0];
	int8_t raw_x =
		(int8_t)mouse_state.packet_buffer[1]; // Cast to signed 8-bit
	int8_t raw_y =
		(int8_t)mouse_state.packet_buffer[2]; // Cast to signed 8-bit
	int8_t raw_z = 0;

	// Check if this is a valid packet
	if (!(flags & 0x08)) {
		// Bit 3 should always be set in the first byte
		// Try to resynchronize by looking for a valid first byte in the buffer
		int sync_found = 0;
		for (int i = 1; i < mouse_state.packet_size; i++) {
			if (mouse_state.packet_buffer[i] & 0x08) {
				// Shift buffer left to align with the valid flags byte
				for (int j = 0; j < mouse_state.packet_size - i;
				     j++) {
					mouse_state.packet_buffer[j] =
						mouse_state
							.packet_buffer[j + i];
				}
				mouse_state.packet_index =
					mouse_state.packet_size - i;
				sync_found = 1;
				break;
			}
		}
		if (!sync_found) {
			mouse_state.packet_index = 0;
		}
		return;
	}

	if (mouse_state.has_scroll_wheel && mouse_state.packet_size == 4)
		raw_z = (int8_t)mouse_state.packet_buffer[3];

	// evdev tap: raw pre-sensitivity deltas in event-device orientation
	// (Y+ = down, wheel + = away from user).  While a client holds a
	// grab, the console cursor and tty mouse reporting are suppressed.
	if (evdev_feed_mouse((int)raw_x, -(int)raw_y, flags & 0x07,
			     -(int)raw_z, 0)) {
		mouse_state.packet_index = 0;
		return;
	}

	// Handle scroll wheel for IntelliMouse
	if (mouse_state.has_scroll_wheel && mouse_state.packet_size == 4) {
		// For IntelliMouse, use the full Z byte without masking
		mouse_state.scroll_delta = raw_z;
		if (raw_z != 0 && !fbdev_display_owned()) {
			// Forward wheel to console immediately
			console_handle_mouse_wheel((int)raw_z);
			// Also report scroll to TTY as button 64|0 (up) or 64|1 (down)
			// SGR scroll: Cb = 64 for scroll-up, 65 for scroll-down
			tty_mouse_report_scroll(mouse_state.x, mouse_state.y,
						raw_z);
		}
	}

	// Process button states
	mouse_state.last_buttons =
		(mouse_state.left_button ? MOUSE_LEFT_BUTTON : 0) |
		(mouse_state.right_button ? MOUSE_RIGHT_BUTTON : 0) |
		(mouse_state.middle_button ? MOUSE_MIDDLE_BUTTON : 0);

	mouse_state.left_button = (flags & MOUSE_LEFT_BUTTON) ? 1 : 0;
	mouse_state.right_button = (flags & MOUSE_RIGHT_BUTTON) ? 1 : 0;
	mouse_state.middle_button = (flags & MOUSE_MIDDLE_BUTTON) ? 1 : 0;

	// Skip if overflow occurred (check flags, not the data values)
	if (flags & (MOUSE_X_OVERFLOW | MOUSE_Y_OVERFLOW)) {
		mouse_state.packet_index = 0;
		return;
	}

	// Apply sensitivity and update position
	mouse_state.delta_x = (raw_x * mouse_state.sensitivity) /
			      2; // Less division for IntelliMouse
	mouse_state.delta_y = -(raw_y * mouse_state.sensitivity) /
			      2; // Less division for IntelliMouse

	// Store last position for cursor clearing
	mouse_state.last_x = mouse_state.x;
	mouse_state.last_y = mouse_state.y;

	// Update mouse position with bounds checking
	mouse_state.x += mouse_state.delta_x;
	mouse_state.y += mouse_state.delta_y;

	// Clamp origin to screen boundaries with partial visibility policy
	if (mouse_state.x < 0) {
		mouse_state.x = 0;
	}
	if (mouse_state.y < 0) {
		mouse_state.y = 0;
	}
	int max_x_for_partial_visibility = mouse_state.screen_width - 2;
	int max_y_for_tip_visibility =
		mouse_state.screen_height - TIP_VISIBLE_ROWS;
	if (max_x_for_partial_visibility < 0) {
		max_x_for_partial_visibility = 0;
	}
	if (max_y_for_tip_visibility < 0) {
		max_y_for_tip_visibility = 0;
	}
	// Keep at least 2px (right) and TIP_VISIBLE_ROWS (bottom) visible
	if (mouse_state.x > max_x_for_partial_visibility) {
		mouse_state.x = max_x_for_partial_visibility;
	}
	if (mouse_state.y > max_y_for_tip_visibility) {
		mouse_state.y = max_y_for_tip_visibility;
	}

	// Update cursor if position changed
	WARN_ON(mouse_state.x < 0 ||
		mouse_state.y < 0); /* clamp failed: negative mouse position */
	WARN_ON(mouse_state.screen_width > 0 &&
		mouse_state.x >=
			mouse_state
				.screen_width); /* mouse X past screen width after clamp */
	WARN_ON(mouse_state.screen_height > 0 &&
		mouse_state.y >=
			mouse_state
				.screen_height); /* mouse Y past screen height after clamp */
	if (mouse_state.x != mouse_state.last_x ||
	    mouse_state.y != mouse_state.last_y) {
		mouse_update_cursor_internal();
	}

	/* Forward to the console (scrollbar, scrollback) and to the tty
	 * (terminal mouse tracking) -- but only while the console owns the
	 * display.
	 *
	 * While a display server has /dev/fb0 open the pointer is its input,
	 * and it reads it from the event device.  Acting on it here as well
	 * means two programs respond to one movement: scrolling inside a
	 * terminal window also scrolled the console's scrollback underneath,
	 * which nothing showed until X exited and the console repainted
	 * somewhere else entirely.
	 *
	 * Checked here rather than relying on evdev's grab suppression above,
	 * because the X.org evdev driver does not grab by default -- its
	 * "Grab Device" option is off -- so the grab test is false for the one
	 * case that matters most. */
	if (!fbdev_display_owned()) {
		console_handle_mouse_event(mouse_state.x, mouse_state.y,
					   mouse_state.left_button ? 1 : 0);

		// Build current button bitmask: bit0=left, bit1=right, bit2=middle
		uint8_t cur_btns = (mouse_state.left_button ? 0x01 : 0) |
				   (mouse_state.right_button ? 0x02 : 0) |
				   (mouse_state.middle_button ? 0x04 : 0);
		tty_mouse_report(mouse_state.x, mouse_state.y, cur_btns,
				 mouse_state.last_buttons);
	}

	// Reset packet index for next packet
	mouse_state.packet_index = 0;
}

// Initialize mouse system
// Update cursor clamping bounds after a runtime resolution change and pull
// the cursor back inside the new screen if necessary.
void mouse_set_bounds(int width, int height)
{
	if (WARN_ON_ONCE(width <= 0 || height <= 0))
		return;
	mouse_state.screen_width = width;
	mouse_state.screen_height = height;
	if (mouse_state.x >= width)
		mouse_state.x = width - 1;
	if (mouse_state.y >= height)
		mouse_state.y = height - 1;

	/* A mode set is when a display driver takes over -- which is how the
	 * hardware cursor arrives, well after mouse_init() ran -- so this is
	 * the moment to work out again which pointer should be drawn.  It also
	 * clears the uploaded shape, so make the next update re-upload it. */
	g_hw_cursor_defined = 0;
	/* Assume the device may still be displaying it: a mode set clears the
	 * uploaded shape, but whether it also takes the cursor down is the
	 * host's business.  Claiming it is already hidden would skip the one
	 * hide that matters when a display server is mid-startup. */
	g_hw_cursor_shown = 1;
	fb_pointer_move(mouse_state.x, mouse_state.y);
	mouse_sync_overlay(mouse_hw_cursor_update());
	/* Switching the overlay off marks the rectangle it occupied dirty;
	 * flush now so it is repainted from the back buffer immediately rather
	 * than lingering on screen until something else happens to draw. */
	fb_flush_dirty_regions();
}

void mouse_init(void)
{
	kprintf("Initializing PS/2 mouse...\n");
	uint8_t controller_config = 0;

	// Initialize mouse state
	mouse_state.x = 400; // Start in center of screen (assuming 800x600)
	mouse_state.y = 300;
	mouse_state.last_x = mouse_state.x;
	mouse_state.last_y = mouse_state.y;
	mouse_state.left_button = 0;
	mouse_state.right_button = 0;
	mouse_state.middle_button = 0;
	mouse_state.scroll_delta = 0;
	mouse_state.packet_index = 0;
	mouse_state.expecting_ack = 0;
	mouse_state.enabled = 0;
	mouse_state.cursor_visible = 1;
	mouse_state.sensitivity = 4; // Default sensitivity

	// Get screen dimensions from framebuffer optimization system
	fb_double_buffer_t *fb_buffer = get_fb_double_buffer();
	BUG_ON(fb_buffer ==
	       NULL); /* mouse_init before framebuffer optimization is set up */
	mouse_state.screen_width = fb_buffer->width;
	mouse_state.screen_height = fb_buffer->height;

	// Set runtime cursor dimensions
	mouse_state.cursor_w = CURSOR_WIDTH;
	mouse_state.cursor_h = CURSOR_HEIGHT;

	// Hand the built-in pointer image to the framebuffer layer, which
	// composites it into the front buffer on every flush.  Whether the
	// overlay is the pointer that gets drawn depends on the display device,
	// which on a VMware guest has not been probed yet at this point -- so
	// this is only the first answer, and mouse_set_bounds() revisits it
	// after the mode set that brings the hardware cursor up.
	mouse_publish_pointer_image();
	fb_pointer_move(mouse_state.x, mouse_state.y);
	mouse_sync_overlay(mouse_hw_cursor_update());

	// Quiesce both PS/2 IRQ lines for the duration of polled mouse init.
	// VMware can inject keyboard bytes while the mouse setup code is
	// polling the shared data port; if those bytes get consumed as mouse
	// responses, the controller can come out of init in a broken state.
	// Early boot keypresses do not need to be preserved, so keep the
	// keyboard side disabled until the mouse path is fully configured.
	ioapic_mask_gsi(1);
	ioapic_mask_gsi(12);

	mouse_flush_output();
	controller_config = mouse_read_controller_config();

	uint8_t ctrl = controller_config;
	ctrl |= PS2_CTR_KBDDIS;
	ctrl &= ~PS2_CTR_KBDINT;
	ctrl |= PS2_CTR_AUXDIS;
	ctrl &= ~PS2_CTR_AUXINT;
	mouse_write_controller_config(ctrl);

	// Drain any stale bytes from the output buffer before issuing mouse commands.
	mouse_flush_output();

	// Enable PS/2 mouse port
	mouse_wait_input();
	outb(PS2_COMMAND_PORT, PS2_CMD_ENABLE_PORT2);
	mouse_flush_output();

	// Test mouse port — non-fatal on failure (stale bytes from eSPI can
	// cause spurious results; the mouse reset below is the real gate).
	mouse_wait_input();
	outb(PS2_COMMAND_PORT, PS2_CMD_TEST_PORT2);
	uint8_t test_result = mouse_read_data();
	if (test_result != 0x00) {
		kprintf("PS2 mouse: port test returned 0x%02x, continuing\n",
			test_result);
		mouse_flush_output();
	}

	/* Silence the device before asking it anything.
	 *
	 * Firmware commonly hands the mouse over still in streaming mode, and a
	 * hand resting on a TrackPoint keeps it sending.  Every response read
	 * below would then race movement bytes, and the detection sequence
	 * would settle on the wrong packet size -- which mis-frames the stream
	 * permanently, because the framing check keys on a bit that only means
	 * anything in a correctly-aligned first byte.  That is what moving the
	 * pointer during boot used to cost: a mouse that never moved again. */
	mouse_quiesce();

	// Detect mouse type and capabilities (may leave extra bytes in buffer depending on emulation)
	mouse_state.mouse_type = mouse_detect_type();

	// Re-apply the IntelliMouse detection sequence after SET_DEFAULTS
	// would reset the mouse to 3-byte mode.  Instead, just set stream
	// mode (0xEA) which preserves the IntelliMouse 4-byte mode.
	mouse_flush_output();
	mouse_write_command(MOUSE_CMD_SET_STREAM_MODE, 0xFF); // expect ACK
	if (!mouse_expect(MOUSE_ACK, 8))
		kprintf("Mouse: SET_STREAM_MODE not acknowledged (continuing)\n");
	mouse_flush_output();

	// Drain any leftover bytes before making IRQ12 live.
	mouse_flush_output();

	ctrl = controller_config;
	ctrl |= PS2_CTR_KBDDIS;
	ctrl &= ~PS2_CTR_KBDINT;
	ctrl &= ~PS2_CTR_AUXDIS;
	ctrl |= PS2_CTR_AUXINT;
	mouse_write_controller_config(ctrl);

	ioapic_unmask_gsi(12);

	// Only allow streaming packets after the controller-side AUX IRQ path is
	// live; otherwise an early movement byte can sit in OBF with no IRQ edge
	// and block both PS/2 devices behind it.
	mouse_state.enabled = 1;
	mouse_state.packet_index = 0;
	mouse_state.expecting_ack = 1;
	mouse_write_command(MOUSE_CMD_ENABLE_REPORTING, 0xFF);

	// If interrupts are not yet flowing, poll a short time for the ACK.
	int acked = 0;
	for (int i = 0; i < 100000 && !acked; ++i) {
		uint8_t status = inb(PS2_STATUS_PORT);
		if (status & PS2_STATUS_OUTPUT_FULL) {
			if (!(status & PS2_STATUS_AUXDATA)) {
				(void)inb(PS2_DATA_PORT);
				continue;
			}

			uint8_t data = inb(PS2_DATA_PORT);
			if (data == MOUSE_ACK)
				acked = 1;
		}
	}

	if (!acked)
		kprintf("Mouse: enable-reporting ACK not seen, continuing\n");

	/* Cleared unconditionally, ACK or no ACK.
	 *
	 * The interrupt handler discards EVERY byte it receives while this flag
	 * is set, waiting for a 0xFA.  Leaving it set after a missed ACK means
	 * waiting for one that already went past: the handler then swallows the
	 * packet stream until a movement byte happens to equal 0xFA, which
	 * lands mid-packet and mis-frames the stream for good.  Losing one byte
	 * and letting the framing check in mouse_process_packet() resynchronise
	 * is strictly better than swallowing the stream. */
	mouse_state.expecting_ack = 0;
	mouse_state.packet_index = 0;

	mouse_flush_output();
	keyboard_reset_state();

	ctrl = controller_config;
	ctrl &= ~PS2_CTR_AUXDIS;
	ctrl |= PS2_CTR_AUXINT;
	mouse_write_controller_config(ctrl);
	ioapic_unmask_gsi(1);

	kprintf("Mouse initialized successfully\n");
	kprintf("  Position: (%d, %d)\n", mouse_state.x, mouse_state.y);
	kprintf("  Screen size: %dx%d\n", mouse_state.screen_width,
		mouse_state.screen_height);
	kprintf("  Mouse type: %s\n",
		mouse_state.has_scroll_wheel ? "IntelliMouse" : "Standard");
	kprintf("  Cursor size: %dx%d\n", mouse_state.cursor_w,
		mouse_state.cursor_h);
}

// Mouse IRQ handler (called from interrupt.c)
void mouse_irq_handler(void)
{
	uint64_t flags;
	spin_lock_irqsave(&mouse_lock, &flags);

	if (!mouse_state.enabled) {
		// Clear the data port to prevent buffer overflow
		inb(PS2_DATA_PORT);
		spin_unlock_irqrestore(&mouse_lock, flags);
		return;
	}

	uint8_t data = inb(PS2_DATA_PORT);

	// Handle ACK responses
	if (mouse_state.expecting_ack) {
		if (data == MOUSE_ACK) {
			mouse_state.expecting_ack = 0;
		} else {
			// Stay silent
			// kprintf("Expected ACK, got 0x%02X\n", data);
		}
		spin_unlock_irqrestore(&mouse_lock, flags);
		return;
	}

	// Store packet data
	mouse_state.packet_buffer[mouse_state.packet_index] = data;
	mouse_state.packet_index++;

	// Process packet when complete
	if (mouse_state.packet_index >= mouse_state.packet_size) {
		mouse_process_packet();
	}

	spin_unlock_irqrestore(&mouse_lock, flags);
}

// Internal cursor update - caller must already hold mouse_lock
static void mouse_update_cursor_internal(void)
{
	if (!mouse_state.enabled) {
		return;
	}
	// Hardware cursor path: the device composites the cursor itself,
	// nothing is drawn into the framebuffer -- and the overlay must be off,
	// or the console draws a second pointer that never moves.
	if (mouse_hw_cursor_update()) {
		mouse_sync_overlay(1);
		return;
	}
	mouse_sync_overlay(0);
	if (!mouse_state.cursor_visible) {
		return;
	}

	/* Do not touch the screen while another program owns the framebuffer:
	 * it draws its own pointer, and two would be visible.
	 *
	 * The position still tracks (above), so the pointer is where the user
	 * left it when the display comes back -- only the drawing stops.  This
	 * does not depend on the display server grabbing the device: it is true
	 * whether it grabs or not, which is why it is checked here rather than
	 * relying on evdev's grab suppression. */
	if (fbdev_display_owned())
		return;

	/* Software path: the pointer is not drawn into the back buffer at all.
	 * Telling the framebuffer layer where it is marks the rectangle it left
	 * and the one it now covers dirty; the flush then repaints the first
	 * from the back buffer and composites the pointer into the second.
	 *
	 * Nothing is saved and nothing is restored, which is what makes this
	 * free of the ghosts the save-and-restore version left behind whenever
	 * the console drew or scrolled underneath the pointer.  Clipping at the
	 * screen edges falls out of the compositing, so the partial-visibility
	 * cases need no code of their own either. */
	fb_pointer_move(mouse_state.x, mouse_state.y);
	/* Also forwards the flushed rect to the display driver (the SVGA
	 * update commands), after releasing the framebuffer lock. */
	fb_flush_dirty_regions();
}

// Update cursor position on screen (public wrapper, takes lock)
void mouse_update_cursor(void)
{
	uint64_t flags;
	spin_lock_irqsave(&mouse_lock, &flags);
	mouse_update_cursor_internal();
	spin_unlock_irqrestore(&mouse_lock, flags);
}

// Get current mouse X position
int mouse_get_x(void)
{
	return mouse_state.x;
}

// Get current mouse Y position
int mouse_get_y(void)
{
	return mouse_state.y;
}

// Get left button state
int mouse_button_left(void)
{
	return mouse_state.left_button;
}

// Get right button state
int mouse_button_right(void)
{
	return mouse_state.right_button;
}

// Get middle button state
int mouse_button_middle(void)
{
	return mouse_state.middle_button;
}

// Get scroll wheel delta
int mouse_scroll_delta(void)
{
	int delta = mouse_state.scroll_delta;
	mouse_state.scroll_delta = 0; // Reset after reading
	return delta;
}

// Set mouse sensitivity
void mouse_set_sensitivity(int sensitivity)
{
	if (sensitivity >= 1 && sensitivity <= 10) {
		mouse_state.sensitivity = sensitivity;
	}
}

// Show or hide cursor
void mouse_show_cursor(int show)
{
	int changed = (show ? 1 : 0) != mouse_state.cursor_visible;
	mouse_state.cursor_visible = show ? 1 : 0;
	if (mouse_hw_cursor_update()) {
		mouse_sync_overlay(1);
		return;
	}
	/* Synced even when the flag did not change: the overlay may still be
	 * showing because the hardware cursor was in charge a moment ago. */
	mouse_sync_overlay(0);
	if (changed)
		fb_flush_dirty_regions();
}

// No-flush variant: record the change, leave the repaint to the caller's flush
void mouse_show_cursor_noflush(int show)
{
	mouse_state.cursor_visible = show ? 1 : 0;
	if (mouse_hw_cursor_update()) {
		mouse_sync_overlay(1);
		return;
	}
	mouse_sync_overlay(0);
}

// Apply a loaded cursor (must be called after cursor_load succeeded)
void mouse_apply_cursor(void)
{
	uint64_t flags;
	spin_lock_irqsave(&mouse_lock, &flags);

	if (!cursor_is_loaded()) {
		spin_unlock_irqrestore(&mouse_lock, flags);
		return;
	}

	uint32_t new_w = cursor_get_width();
	uint32_t new_h = cursor_get_height();
	if (new_w == 0 || new_h == 0 || new_w > CURSOR_MAX_WIDTH ||
	    new_h > CURSOR_MAX_HEIGHT) {
		kprintf("mouse: loaded cursor %ux%u out of range\n", new_w,
			new_h);
		spin_unlock_irqrestore(&mouse_lock, flags);
		return;
	}

	mouse_state.cursor_w = (int)new_w;
	mouse_state.cursor_h = (int)new_h;

	// Enable the loaded cursor
	use_loaded_cursor = 1;
	// Force re-upload of the hardware cursor image on the next update.
	g_hw_cursor_defined = 0;

	/* Republish the image.  The overlay marks the old and the new
	 * rectangle dirty, so a change of size repaints both -- nothing of the
	 * previous pointer is left at the edges. */
	mouse_publish_pointer_image();

	int hw = mouse_hw_cursor_update();
	mouse_sync_overlay(hw);
	if (mouse_state.cursor_visible && !hw)
		fb_flush_dirty_regions();

	spin_unlock_irqrestore(&mouse_lock, flags);

	kprintf("mouse: using loaded cursor (%ux%u)\n", new_w, new_h);
}

// Inject a USB HID mouse movement/button event into the mouse subsystem.
// This replicates the same position update, clamping, cursor redraw, and
// event forwarding that mouse_process_packet() does for PS/2 mice.
// Called from the USB HID driver (usbhid.c) in IRQ or process context.
// Parameters:
//   dx, dy       - relative displacement (signed, raw from HID report)
//   buttons      - HID button bits (bit0=left, bit1=right, bit2=middle)
//   wheel        - scroll wheel delta (signed, 0 = none)
void mouse_inject_usb_movement(int dx, int dy, uint8_t buttons, int8_t wheel)
{
	uint64_t flags;
	spin_lock_irqsave(&mouse_lock, &flags);

	// Auto-enable the mouse subsystem on first USB/I2C event.
	// PS/2 init sets enabled=1, but when there's no PS/2 mouse
	// (e.g. Dell laptops with only an I2C touchpad) enabled stays 0
	// and mouse_update_cursor_internal() silently returns without
	// drawing anything.
	if (!mouse_state.enabled) {
		mouse_state.enabled = 1;
	}

	// evdev tap (USB/I2C injected events use event-device orientation
	// already: Y+ = down, wheel + = away from user).
	if (evdev_feed_mouse(dx, dy, buttons & 0x07, (int)wheel, 0)) {
		spin_unlock_irqrestore(&mouse_lock, flags);
		return; // grabbed: suppress console cursor/tty reporting
	}

	// --- Button state ---
	mouse_state.last_buttons =
		(mouse_state.left_button ? MOUSE_LEFT_BUTTON : 0) |
		(mouse_state.right_button ? MOUSE_RIGHT_BUTTON : 0) |
		(mouse_state.middle_button ? MOUSE_MIDDLE_BUTTON : 0);

	mouse_state.left_button = (buttons & 0x01) ? 1 : 0;
	mouse_state.right_button = (buttons & 0x02) ? 1 : 0;
	mouse_state.middle_button = (buttons & 0x04) ? 1 : 0;

	// --- Scroll wheel ---
	if (wheel != 0) {
		mouse_state.scroll_delta = wheel;
		if (!fbdev_display_owned()) {
			console_handle_mouse_wheel((int)wheel);
			tty_mouse_report_scroll(mouse_state.x, mouse_state.y,
						wheel);
		}
	}

	// --- Movement ---
	// Apply sensitivity (same formula as PS/2 path)
	mouse_state.delta_x = (dx * mouse_state.sensitivity) / 2;
	mouse_state.delta_y =
		(dy * mouse_state.sensitivity) /
		2; // USB HID Y+ = down, screen Y+ = down (no inversion needed, unlike PS/2)

	mouse_state.last_x = mouse_state.x;
	mouse_state.last_y = mouse_state.y;

	mouse_state.x += mouse_state.delta_x;
	mouse_state.y += mouse_state.delta_y;

	// Clamp to screen boundaries (same as PS/2 path)
	if (mouse_state.x < 0)
		mouse_state.x = 0;
	if (mouse_state.y < 0)
		mouse_state.y = 0;

	int max_x = mouse_state.screen_width - 2;
	int max_y = mouse_state.screen_height - TIP_VISIBLE_ROWS;
	if (max_x < 0)
		max_x = 0;
	if (max_y < 0)
		max_y = 0;
	if (mouse_state.x > max_x)
		mouse_state.x = max_x;
	if (mouse_state.y > max_y)
		mouse_state.y = max_y;
	WARN_ON(mouse_state.x < 0 ||
		mouse_state.y <
			0); /* clamp failed: negative mouse position (USB path) */

	// --- Cursor redraw ---
	if (mouse_state.x != mouse_state.last_x ||
	    mouse_state.y != mouse_state.last_y) {
		mouse_update_cursor_internal();
	}

	// --- Forward events (same as PS/2 path, same ownership rule) ---
	if (!fbdev_display_owned()) {
		console_handle_mouse_event(mouse_state.x, mouse_state.y,
					   mouse_state.left_button ? 1 : 0);

		uint8_t cur_btns = (mouse_state.left_button ? 0x01 : 0) |
				   (mouse_state.right_button ? 0x02 : 0) |
				   (mouse_state.middle_button ? 0x04 : 0);
		tty_mouse_report(mouse_state.x, mouse_state.y, cur_btns,
				 mouse_state.last_buttons);
	}

	spin_unlock_irqrestore(&mouse_lock, flags);
}
