#include "../../include/kernel/console.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/vfs.h"

// Shell helpers
static int shell_ls_count = 0;
static void shell_ls_cb(const char* name, unsigned attr, unsigned long size) {
    if (!name || !name[0]) {
        return; // skip unexpected blank names
    }
    shell_ls_count++;
    kprintf("%s %c %lu\n", name, (attr & 0x10) ? 'd' : '-', size);
}
// LikeOS-64 Kernel Executive - Initialization
// Main kernel initialization and executive services
#include "../../include/kernel/interrupt.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/mouse.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/scrollbar.h"
#include "../../include/kernel/fb_optimize.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/xhci.h"
#include "../../include/kernel/usb.h"
#include "../../include/kernel/usb_msd.h"
#include "../../include/kernel/block.h"
// (fat32.h and vfs.h already included above)

// Function prototypes
void KiSystemStartup(void);
void kernel_main(framebuffer_info_t* fb_info);

// Simple path stack for prompt (stores directory names after root)
#define SHELL_MAX_DEPTH 16
#define SHELL_NAME_MAX  64
static unsigned long shell_path_clusters[SHELL_MAX_DEPTH];
static char          shell_path_names[SHELL_MAX_DEPTH][SHELL_NAME_MAX];
static int           shell_path_depth = 0; // 0 means at root

static void shell_path_reset(void) {
    shell_path_depth = 0;
}
static void shell_path_push(unsigned long cluster, const char* name) {
    if (shell_path_depth >= SHELL_MAX_DEPTH) {
        return; // ignore overflow
    }
    shell_path_clusters[shell_path_depth] = cluster;
    int i = 0;
    for (; name && name[i] && i < (SHELL_NAME_MAX - 1); ++i) {
        shell_path_names[shell_path_depth][i] = name[i];
    }
    shell_path_names[shell_path_depth][i] = '\0';
    shell_path_depth++;
}
static void shell_path_pop(void) {
    if (shell_path_depth > 0) {
        shell_path_depth--;
    }
}
static void shell_prompt(void) {
    if (fat32_get_cwd() == fat32_root_cluster()) {
        kprintf("/ > ");
        return;
    }
    kprintf("/");
    for (int i = 0; i < shell_path_depth; i++) {
        kprintf("%s/", shell_path_names[i]);
    }
    kprintf(" > ");
}
// UEFI kernel entry point - called by bootloader
void kernel_main(framebuffer_info_t* fb_info) {
    // Initialize console system with framebuffer info
    console_init(fb_info);
    // Initialize framebuffer optimization after console is ready
    console_init_fb_optimization();
    
    // Call the main system initialization
    KiSystemStartup();
}

// Kernel Executive entry point
void KiSystemStartup(void) {
    // Console is already initialized by kernel_main()
    
    // Print our boot message using kprintf
    kprintf("\nLikeOS-64 Kernel v1.0\n");
    kprintf("64-bit Long Mode Active\n");
    kprintf("Higher Half Kernel loaded at virtual address %p\n", (void*)KiSystemStartup);

    // Set colored output
    console_set_color(10, 0); // Light Green on Black
    kprintf("\nKernel initialization complete!\n");
        
    // Reset to default colors
    console_set_color(15, 0); // White on Black
    
    // Initialize interrupt system
    kprintf("\nInitializing interrupt system...\n");
    interrupts_init();

    kprintf("Interrupt system initialized successfully\n");
    
    // Initialize memory management subsystem
    kprintf("\nInitializing Memory Management Subsystem...\n");
    MmDetectMemory();

    MmInitializePhysicalMemory(32 * 1024 * 1024); // 32MB minimum requirement

    MmInitializeVirtualMemory();
    MmInitializeHeap();

    // Print initial memory statistics
    MmPrintMemoryStats();

    // Initialize PCI and enumerate devices
    kprintf("\nInitializing PCI subsystem...\n");
    pci_init();
    int pci_count = pci_enumerate();
    kprintf("PCI enumeration complete (%d devices)\n", pci_count);
    // Assign MMIO BARs for devices lacking firmware-assigned addresses (very simple allocator)
    pci_assign_unassigned_bars();

    // Initialize VFS
    vfs_init();

    // Locate first XHCI controller (USB 3) and initialize skeleton
    const pci_device_t* xhci_dev = pci_get_first_xhci();
    extern xhci_controller_t g_xhci; // global defined in xhci.c
    int xhci_available = 0;
    if (xhci_dev) {
        kprintf("Found XHCI: bus=%u dev=%u func=%u vendor=%04x device=%04x\n",
                xhci_dev->bus, xhci_dev->device, xhci_dev->function,
                xhci_dev->vendor_id, xhci_dev->device_id);
        int xr = xhci_init(&g_xhci, xhci_dev);
        if (xr == ST_OK) {
            kprintf("XHCI controller running (mmio=%p)\n", (void*)g_xhci.mmio_base);
            xhci_available = 1;
            /* Enable legacy INTx line for xHCI if valid */
            if (xhci_dev->interrupt_line != 0xFF) {
                irq_enable(xhci_dev->interrupt_line);
                kprintf("XHCI: enabled IRQ line %u\n", xhci_dev->interrupt_line);
            } else {
                kprintf("XHCI: no valid interrupt line, remaining in polling mode\n");
            }
        } else {
            kprintf("XHCI initialization failed (status=%d)\n", xr);
        }
    } else {
        kprintf("No XHCI controller found (USB mass storage unavailable yet)\n");
    }

    // Initialize USB core scaffolding
    usb_core_init();

    // Test memory allocation
    kprintf("Testing memory allocation...\n");
    void* test_ptr1 = kalloc(1024);
    void* test_ptr2 = kalloc(2048);
    kprintf("  Allocated test blocks: %p (1KB), %p (2KB)\n", test_ptr1, test_ptr2);
    kfree(test_ptr1);
    kfree(test_ptr2);
    kprintf("  Test blocks freed successfully\n");

    // Display framebuffer optimization status
    kprintf("\nFramebuffer Optimization Status:\n");
    console_show_fb_status();

    // Initialize and render system scrollbar (single source of truth)
    kprintf("\nInitializing visual scrollbar system...\n");
    static scrollbar_t system_scrollbar;
    if (scrollbar_init_system_default(&system_scrollbar) == 0) {
        scrollbar_render(&system_scrollbar);
        fb_flush_dirty_regions();
        kprintf("Visual scrollbar initialized and rendered successfully\n");
    } else {
        kprintf("Warning: Failed to initialize scrollbar\n");
    }

    // Initialize keyboard
    keyboard_init();
    
    // Enable keyboard IRQ (IRQ 1)
    irq_enable(1);

    // Initialize mouse
    mouse_init();
    
    // Enable cascade IRQ (IRQ 2) - required for slave PIC interrupts
    irq_enable(2);
    
    // Enable mouse IRQ (IRQ 12)
    irq_enable(12);
        
    // Enable interrupts after everything is initialized
    __asm__ volatile ("sti");
    kprintf("Interrupts enabled!\n");
    
    // Show ready prompt
    console_set_color(11, 0); // Light Cyan on Black
    kprintf("\nSystem ready! Type to test keyboard input:\n");
    shell_path_reset();
    shell_prompt();
    console_set_color(15, 0); // White on Black

    // Main input loop
    char c;
    int poll_counter = 0;
    // Simple command shell state
    static char cmd_buf[128];
    static int cmd_len = 0;
    static unsigned long cwd_cluster = 0; // shadow copy for prompt (synced with fat32 layer)

    while (1) {
        c = keyboard_get_char();

        if (c != 0) {
            if (c == '\n') {
                // Finish command
                cmd_buf[cmd_len] = '\0';
                kprintf("\n");
                // Process command
                if (cmd_len > 0) {
                    // Trim leading spaces
                    int start = 0;
                    while (cmd_buf[start] == ' ' && start < cmd_len) {
                        start++;
                    }
                    // Find first space
                    int sp = start;
                    while (sp < cmd_len && cmd_buf[sp] != ' ') {
                        sp++;
                    }
                    int cmdlen = sp - start;
                    // Compare commands manually
                    int is_ls = (cmdlen == 2 && cmd_buf[start] == 'l' && cmd_buf[start + 1] == 's');
                    int is_cat = (cmdlen == 3 && cmd_buf[start] == 'c' && cmd_buf[start + 1] == 'a' && cmd_buf[start + 2] == 't');
                    int is_cd = (cmdlen == 2 && cmd_buf[start] == 'c' && cmd_buf[start + 1] == 'd');
                    int is_pwd = (cmdlen == 3 && cmd_buf[start] == 'p' && cmd_buf[start + 1] == 'w' && cmd_buf[start + 2] == 'd');
                    int is_stat = (cmdlen == 4 && cmd_buf[start] == 's' && cmd_buf[start + 1] == 't' && cmd_buf[start + 2] == 'a' && cmd_buf[start + 3] == 't');
                    if (is_ls) {
                        if (block_count() > 0) {
                            int fn = sp;
                            while (fn < cmd_len && cmd_buf[fn] == ' ') {
                                fn++;
                            }
                            unsigned long list_cluster;
                            if (fn >= cmd_len) {
                                list_cluster = fat32_get_cwd();
                            } else {
                                // Resolve provided path
                                unsigned a;
                                unsigned long fc;
                                unsigned long sz;
                                if (fat32_resolve_path(fat32_get_cwd(), &cmd_buf[fn], &a, &fc, &sz) == ST_OK) {
                                    if (a & 0x10) {
                                        list_cluster = fc;
                                    } else {
                                        kprintf("ls: not a directory\n");
                                        goto after_cmd;
                                    }
                                } else {
                                    kprintf("ls: path not found\n");
                                    goto after_cmd;
                                }
                            }
                            shell_ls_count = 0;
                            if (list_cluster == fat32_root_cluster()) {
                                fat32_list_root(shell_ls_cb);
                            } else {
                                fat32_dir_list(list_cluster, shell_ls_cb);
                            }
                            if (shell_ls_count == 0 && list_cluster == fat32_root_cluster()) {
                                // Extra debug: dump first 4 raw 8.3 names from cached root to see what's there
                                extern void fat32_debug_dump_root(void);
                                fat32_debug_dump_root();
                            }
                            if (shell_ls_count == 0) {
                                kprintf("(empty)\n");
                            }
                        } else {
                            kprintf("No block device yet\n");
                        }
                    } else if (is_cd) {
                        int fn = sp;
                        while (fn < cmd_len && cmd_buf[fn] == ' ') {
                            fn++;
                        }
                        if (fn >= cmd_len) {
                            kprintf("Usage: cd <dir>\n");
                        } else {
                            const char* path = &cmd_buf[fn];
                            // Handle absolute path by resetting stack
                            if (path[0] == '/') {
                                fat32_set_cwd(0);
                                shell_path_reset();
                            }
                            // Tokenize path by '/'
                            char segment[64];
                            int idx = 0;
                            int i = 0;
                            while (1) {
                                char ch = path[i];
                                if (ch == '/' || ch == '\0') {
                                    segment[idx] = '\0';
                                    if (idx > 0) {
                                        if (segment[0] == '.' && segment[1] == '\0') {
                                            // stay
                                        } else if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
                                            // up
                                            unsigned long cur = fat32_get_cwd();
                                            unsigned long parent = fat32_parent_cluster(cur);
                                            fat32_set_cwd(parent == fat32_root_cluster() ? 0 : parent);
                                            shell_path_pop();
                                        } else {
                                            unsigned a;
                                            unsigned long fc;
                                            unsigned long sz;
                                            if (fat32_resolve_path(fat32_get_cwd(), segment, &a, &fc, &sz) == ST_OK && (a & 0x10)) {
                                                fat32_set_cwd(fc == fat32_root_cluster() ? 0 : fc);
                                                shell_path_push(fc, segment);
                                            } else {
                                                kprintf("cd: component '%s' not dir\n", segment);
                                                break;
                                            }
                                        }
                                    }
                                    idx = 0;
                                    if (ch == '\0') {
                                        break;
                                    }
                                    i++;
                                    continue;
                                } else if (idx < 63) {
                                    segment[idx++] = ch;
                                    i++;
                                }
                            }
                            kprintf("cd ok\n");
                        }
                    } else if (is_pwd) {
                        if (fat32_get_cwd() == fat32_root_cluster()) {
                            kprintf("/\n");
                        } else {
                            kprintf("/");
                            for (int i = 0; i < shell_path_depth; i++) {
                                kprintf("%s/", shell_path_names[i]);
                            }
                            kprintf("\n");
                        }
                    } else if (is_stat) {
                        int fn = sp;
                        while (fn < cmd_len && cmd_buf[fn] == ' ') {
                            fn++;
                        }
                        if (fn >= cmd_len) {
                            kprintf("Usage: stat <path>\n");
                        } else {
                            unsigned a;
                            unsigned long fc;
                            unsigned long sz;
                            if (fat32_stat(fat32_get_cwd(), &cmd_buf[fn], &a, &fc, &sz) == ST_OK) {
                                kprintf("attr=%c size=%lu cluster=%lu\n", (a & 0x10) ? 'd' : 'f', sz, fc);
                            } else {
                                kprintf("stat: not found\n");
                            }
                        }
                    } else if (is_cat) {
                        // Skip spaces to filename
                        int fn = sp;
                        while (fn < cmd_len && cmd_buf[fn] == ' ') {
                            fn++;
                        }
                        if (fn >= cmd_len) {
                            kprintf("Usage: cat <file>\n");
                        } else {
                            // Open via VFS
                            vfs_file_t* vf = 0;
                            const char* name = &cmd_buf[fn];
                            char pathbuf[128];
                            (void)pathbuf; // unused in current logic
                            if (name[0] != '/') {
                                unsigned long cc = fat32_get_cwd();
                                if (cc != fat32_root_cluster()) {
                                    // Not yet supporting subdirs, so fallback to root
                                    name = &cmd_buf[fn];
                                }
                            }
                            if (vfs_open(name, &vf) == ST_OK) {
                                // Read up to 4096 bytes
                                char* rbuf = (char*)kalloc(4096);
                                if (rbuf) {
                                    long r = vfs_read(vf, rbuf, 4096);
                                    if (r > 0) {
                                        for (long i = 0; i < r; i++) {
                                            char ch = rbuf[i];
                                            if (ch == '\r') {
                                                ch = '\n';
                                            }
                                            kprintf("%c", ch);
                                        }
                                    }
                                    kfree(rbuf);
                                }
                                vfs_close(vf);
                                kprintf("\n");
                            } else {
                                kprintf("File not found or open error\n");
                            }
                        }
                    } else {
                        kprintf("Unknown command\n");
                    }
after_cmd:				;
                }
                cmd_len = 0;
                shell_prompt();
            } else if (c == '\b') {
                if (cmd_len > 0) {
                    console_backspace();
                    cmd_len--;
                }
            } else if (c >= ' ' && c <= '~') {
                if (cmd_len < (int)(sizeof(cmd_buf) - 1)) {
                    cmd_buf[cmd_len++] = c;
                    kprintf("%c", c);
                }
            }
        }

        // Periodic XHCI polling (ports + events) - skip heavy polling when interrupts active
        if (xhci_available) {
#ifdef XHCI_USE_INTERRUPTS
            static unsigned poll_div = 0;
            // Still poll ports occasionally (connect status changes may not always interrupt early proto)
            if ((poll_div++ & 0xF) == 0) {
                xhci_poll_ports(&g_xhci);
            }
            // Event ring normally advanced by IRQ handler; optional safety poll if no events for a while
            if ((poll_div & 0x1F) == 0) {
                xhci_process_events(&g_xhci);
            }
#else
            xhci_poll_ports(&g_xhci);
            xhci_process_events(&g_xhci);
#endif
            usb_msd_poll(&g_xhci); // attempt MSD actions
            static int dbg_counter = 0;
            if (((dbg_counter++) & 0x3F) == 0) {
                extern void xhci_debug_state(xhci_controller_t*);
                xhci_debug_state(&g_xhci);
            }
            // Command ring stall poke: if a command pending too long, re-doorbell
            if (g_xhci.pending_cmd_type) {
                g_xhci.cmd_ring_stall_ticks++;
                if ((g_xhci.cmd_ring_stall_ticks & 0xFF) == 0) {
                    volatile uint32_t* db0 = (volatile uint32_t*)(g_xhci.doorbell_array + 0x00);
                    *db0 = 0; // poke doorbell
                }
                if ((g_xhci.cmd_ring_stall_ticks & 0x3FF) == 0) {
                    // Rewrite CRCR (some controllers may need reprogram after stall) and doorbell again
                    volatile unsigned long* crcr = (volatile unsigned long*)(g_xhci.op_base + XHCI_OP_CRCR);
                    unsigned long val = (g_xhci.cmd_ring_phys & ~0x3FUL) | (g_xhci.cmd_cycle_state & 1);
                    *crcr = val;
                    volatile uint32_t* db0 = (volatile uint32_t*)(g_xhci.doorbell_array + 0x00);
                    *db0 = 0;
                }
            } else {
                g_xhci.cmd_ring_stall_ticks = 0;
            }
            // Attempt FAT32 mounts across all block devices until signature file /LIKEOS.SIG is found
            // We keep testing new devices; once signature is found we stop further attempts
            static int signature_found = 0;
            static unsigned int tested_mask = 0; // bit i set => device i fully tested (mounted + signature check)
            static fat32_fs_t fs_instances[BLOCK_MAX_DEVICES];
            if (!signature_found) {
                int nblk = block_count();
                for (int bi = 0; bi < nblk && !signature_found; ++bi) {
                    if (tested_mask & (1u << bi))
                        continue; // already tested this device
                    const block_device_t* bdev = block_get(bi);
                    if (!bdev || !bdev->driver_data) {
                        tested_mask |= (1u << bi);
                        continue;
                    }
                    xhci_controller_t* xh = (xhci_controller_t*)bdev->driver_data;
                    if (!xh || !xh->msd_ready) {
                        // Not yet ready (capacity unknown) - skip this round, will retry later
                        continue;
                    }
                    fat32_fs_t* fs = &fs_instances[bi];
                    if (fat32_mount(bdev, fs) == ST_OK) {
                        fat32_vfs_register_root(fs); // temporarily becomes active root
                        kprintf("FAT32: mount succeeded on %s (checking signature)\n", bdev->name);
                        vfs_file_t* sf = 0;
                        if (vfs_open("/LIKEOS.SIG", &sf) == ST_OK) {
                            // Signature present - select this device permanently
                            vfs_close(sf);
                            signature_found = 1;
                            kprintf("FAT32: signature /LIKEOS.SIG found on %s (root storage selected)\n", bdev->name);
                        } else {
                            kprintf("FAT32: signature not found on %s\n", bdev->name);
                            tested_mask |= (1u << bi); // do not retry unless rebooted
                        }
                    } else {
                        kprintf("FAT32: mount failed on %s\n", bdev->name ? bdev->name : "(unnamed)");
                        tested_mask |= (1u << bi);
                    }
                }
            }
        }

        // Halt CPU until next interrupt
        //__asm__ volatile ("hlt");
    }
}


