// LikeOS-64 Kernel Initialization
#include <kernel/io/console.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/interrupt.h>
#include <kernel/dev/input/keyboard.h>
#include <kernel/dev/input/mouse.h>
#include <kernel/mm/memory.h>
#include <kernel/mm/slab.h>
#include <kernel/io/scrollbar.h>
#include <kernel/dev/video/fb_optimize.h>
#include <kernel/hal/pci.h>
#include <kernel/dev/hid/ps2.h>
#include <kernel/hal/ioapic.h>
#include <kernel/ke/xhci_boot.h>
#include <kernel/ke/storage.h>
#include <kernel/ke/userinit.h>
#include <kernel/ke/timer.h>
#include <kernel/ke/sched.h>
#include <kernel/io/tty.h>
#include <kernel/fs/devfs.h>
#include <kernel/hal/acpi.h>
#include <kernel/ke/percpu.h>
#include <kernel/ke/smp.h>
#include <kernel/hal/lapic.h>
#include <kernel/dev/hid/usb_hid.h>
#include <kernel/dev/usb/usb_serial.h>
#include <kernel/net/net.h>
#include <kernel/uapi/types.h>
#include <kernel/ke/futex.h>
#include <kernel/uapi/bug.h>

void system_startup(boot_info_t *boot_info);
void kernel_main(boot_info_t *boot_info);

static xhci_boot_state_t g_xhci_boot;
static storage_fs_state_t g_storage_state;
static boot_info_t *g_boot_info;
static uint64_t
	g_rsdp_address; // Saved RSDP address (copied before identity unmap)
static uint64_t
	g_smp_trampoline_address; // Saved SMP trampoline address from bootloader
static uint64_t g_boot_epoch_saved; // Saved boot epoch from UEFI GetTime

__no_stack_protector void kernel_main(boot_info_t *boot_info)
{
	BUG_ON(boot_info == NULL);
	g_boot_info = boot_info;
	console_init((framebuffer_info_t *)&boot_info->fb_info);
	console_init_fb_optimization();
	system_startup(boot_info);
}

__no_stack_protector void system_startup(boot_info_t *boot_info)
{
	BUG_ON(boot_info == NULL);
	WARN_ON_ONCE(boot_info->fb_info.horizontal_resolution == 0 ||
		     boot_info->fb_info.vertical_resolution ==
			     0); /* UEFI framebuffer has zero resolution */
	console_set_color(10, 0);
	kprintf("\nLikeOS-64 Kernel v0.2\n\n");
	console_set_color(15, 0);

	kprintf("64-bit Long Mode Active\n");
	kprintf("Framebuffer: %ux%u (%u bpp, pitch %u)\n",
		boot_info->fb_info.horizontal_resolution,
		boot_info->fb_info.vertical_resolution,
		boot_info->fb_info.bytes_per_pixel * 8,
		boot_info->fb_info.pixels_per_scanline);

	interrupts_init();

	uint64_t memory_size = boot_info->mem_info.total_memory;
	if (memory_size < 256 * 1024 * 1024) {
		memory_size = 256 * 1024 * 1024;
	}

	// Store UEFI memory map before initializing memory manager
	// This allows us to mark UEFI reserved regions as off-limits
	mm_initialize_from_boot_info(boot_info);

	mm_initialize_physical_memory(memory_size);
	mm_initialize_virtual_memory();
	mm_initialize_heap();

	// Initialize page table pool (needs heap ready, before SLAB)
	mm_init_pt_pool();

	// Initialize SLAB allocator (dynamic kernel heap)
	slab_init();

	mm_print_memory_stats();

	mm_enable_nx();
	mm_remap_kernel_with_nx();
	mm_enable_smep_smap();

	// Before removing identity mapping, remap framebuffer pointers to direct map
	console_remap_to_direct_map();
	fb_optimize_remap_to_direct_map();

	// Save RSDP address before identity mapping is removed (boot_info is in low memory)
	g_rsdp_address = boot_info->rsdp_address;

	// Save SMP trampoline address from bootloader
	g_smp_trampoline_address = boot_info->smp_trampoline_addr;

	// Save boot epoch from UEFI GetTime (before identity unmap)
	g_boot_epoch_saved = boot_info->boot_epoch;

	// Switch to a kernel stack in higher-half space before removing identity mapping
	// The current stack is in low memory (set up by bootloader)
	// This function does NOT return - it switches stacks and calls continue_system_startup
	mm_switch_to_kernel_stack();

	// Never reached - execution continues in continue_system_startup()
	for (;;)
		__asm__ volatile("hlt");
}

// This function continues system startup after switching to the kernel stack
// and removing identity mapping. Called from mm_switch_to_kernel_stack()
__no_stack_protector void continue_system_startup(void)
{
	// Establish per-CPU GS base and stack canary FIRST, before any
	// stack-protected callee runs (compiler emits gs:104 canary loads).
	percpu_init();
	mm_initialize_syscall();

	pci_init();
	pci_enumerate();
	pci_assign_unassigned_bars();

	vfs_init();
	devfs_init();
	vfs_register_devfs(devfs_get_ops());
	tty_init();

	static scrollbar_t system_scrollbar;
	if (scrollbar_init_system_default(&system_scrollbar) == 0) {
		scrollbar_render(&system_scrollbar);
		fb_flush_dirty_regions();
	}

	irq_enable(0);
	irq_enable(2);

	// ACPI must be ready before PS/2 PNP detection
	// so targeted firmware discovery can inspect the controller and HID
	// device nodes.
	WARN_ON_ONCE(
		g_rsdp_address ==
		0); /* RSDP not provided by bootloader: ACPI, xHCI, HPET, and timers will all fail */
	acpi_init(g_rsdp_address);
	acpi_pm_init();
	timer_init_hpet(); // Prefer HPET for precise wall-clock timing if available
	timer_init_pmtimer(); // Probe ACPI PM Timer for sub-tick interpolation

	// Detect x2APIC mode early so lapic_eoi() works for all drivers.
	// Without this, MMIO-based EOI is a no-op in x2APIC mode and
	// interrupts get stuck in the LAPIC ISR (blocking same-priority vectors).
	lapic_early_detect();

	xhci_boot_init(&g_xhci_boot);
	usbhid_init();
	usbserial_init();

	ps2_init();
	keyboard_init();
	mouse_init();

	storage_fs_init(&g_storage_state);

	// Initialize networking (E1000 NIC driver, protocol stack, DHCP)
	net_init();

	futex_init();
	sched_init();

	/* The primordial task established by sched_init() is the sole root origin
	 * of the credential system (real/effective/saved uid/gid all 0).  Every
	 * other task inherits its credentials from a spawning task, so none is
	 * silently born privileged.  Assert that initial security context here so a
	 * regression in the "init is root" invariant is caught at boot. */
	BUG_ON(!current_is_root());

	// Mask ACPI SCI permanently — level-triggered EC GPE 0x66 fires
	// continuously on this platform, causing an interrupt storm that
	// blocks init progress.  We don't need EC events (battery/thermal)
	// for PS/2 keyboard/mouse operation.
	uint32_t sci_gsi = acpi_get_sci_gsi();
	if (sci_gsi)
		ioapic_mask_gsi((uint8_t)sci_gsi);

	// Initialize SMP support
	smp_init(g_smp_trampoline_address);

	// Boot Application Processors (APs)
	smp_boot_aps();
	kprintf("SMP: %u CPU(s) online\n", smp_get_cpu_count());

	timer_set_boot_epoch(g_boot_epoch_saved);

	// Spawn one ksoftirqd kernel thread per CPU before enabling interrupts.
	// These threads sleep until softirq_raise() bumps a pending vector for
	// their CPU; the IRQ-tail drain handles the common case but ksoftirqd
	// catches sustained load and any vectors raised from non-IRQ context.
	{
		extern void ksoftirqd_start_all(void);
		ksoftirqd_start_all();
	}

	// Enable interrupts (SCI stays masked — no EC event storm).
	WARN_ON_ONCE(smp_get_cpu_count() <
		     1); /* at least BSP must be online before STI */
	__asm__ volatile("sti");

	// Send DHCP DISCOVER now that interrupts are enabled and the E1000
	// can actually receive the OFFER response.
	net_start_dhcp();

	timer_init(100);

	// Use LAPIC timer when available — it was already calibrated in smp_init()
	// and is 100% reliable on all platforms (QEMU, VMware, bare metal).
	// PIT delivery via virtual wire (ExtINT→LINT0) is unreliable: VMware
	// may deliver it sporadically (just enough to pass a tick-detection test,
	// then nearly stop), causing g_ticks to crawl and all sleeps to hang.
	if (lapic_is_available()) {
		lapic_timer_start(100);
		kprintf("Timer: using LAPIC timer at 100 Hz\n");
	} else {
		// No LAPIC — legacy PIT IRQ0 (old hardware / single-CPU)
		timer_start();
		kprintf("Timer: using PIT at 100 Hz\n");
	}

	// Measure the actual tick rate against the CMOS RTC second boundary.
	// LAPIC timer calibration (via PIT ch2) can be inaccurate in virtual
	// machines, leading to g_frequency diverging from the real tick rate.
	// This takes ~2 seconds but keeps wall-clock time accurate.
	timer_calibrate_frequency();

	// Wait for DHCP to complete before starting the shell so that DNS
	// and routing are ready for the very first user command.
	{
		uint64_t dhcp_deadline =
			timer_ticks() + 500; // 5 seconds at 100 Hz
		while (!dhcp_configured() && timer_ticks() < dhcp_deadline) {
			__asm__ volatile("hlt"); // sleep until next interrupt
		}
	}

	userinit_start();
	storage_fs_set_ready(&g_storage_state);
	keyboard_activate();

	while (1) {
		__asm__ volatile("sti");
		int handled_input = userinit_tick();
		xhci_boot_poll(&g_xhci_boot);
		xhci_hotplug_poll(&g_xhci);
		xhci_hotplug_poll(&g_xhci_hid);
		usbhid_poll();
		storage_fs_poll(&g_storage_state);
		sched_run_ready();

		if (!handled_input) {
			__asm__ volatile("hlt");
		}
	}
}
