#include "../../include/kernel/shell.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/keyboard.h"
#include "../../include/kernel/fat32.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/block.h"
#include "../../include/kernel/memory.h"
#include "../../include/kernel/elf.h"
#include "../../include/kernel/sched.h"

// Simple path stack for prompt (stores directory names after root)
#define SHELL_MAX_DEPTH 16
#define SHELL_NAME_MAX  64
static unsigned long shell_path_clusters[SHELL_MAX_DEPTH];
static char          shell_path_names[SHELL_MAX_DEPTH][SHELL_NAME_MAX];
static int           shell_path_depth = 0; // 0 means at root

// Track if we're waiting for a user program to complete
static int shell_waiting_for_program = 0;

// Shell helpers
static int shell_ls_count = 0;
static void shell_ls_cb(const char* name, unsigned attr, unsigned long size) {
    if (!name || !name[0]) {
        return;
    }
    shell_ls_count++;
    kprintf("%s %c %lu\n", name, (attr & 0x10) ? 'd' : '-', size);
}

static void shell_path_reset(void) {
    shell_path_depth = 0;
}

static void shell_path_push(unsigned long cluster, const char* name) {
    if (shell_path_depth >= SHELL_MAX_DEPTH) {
        return;
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
        kprintf("/ # ");
        return;
    }
    kprintf("/");
    for (int i = 0; i < shell_path_depth; i++) {
        kprintf("%s/", shell_path_names[i]);
    }
    kprintf(" # ");
}

void shell_redisplay_prompt(void) {
    shell_prompt();
    console_cursor_enable();
}

void shell_init(void) {
    console_set_color(11, 0);
    kprintf("\nSystem ready! Type to test keyboard input:\n");
    shell_path_reset();
    shell_prompt();
    console_set_color(15, 0);
    console_cursor_enable();  // Enable blinking cursor
}

int shell_tick(void) {
    static char cmd_buf[128];
    static int cmd_len = 0;
    
    // Check if we're waiting for a program to finish
    if (shell_waiting_for_program) {
        if (!sched_has_user_tasks()) {
            // Program finished - reap zombie children and show prompt
            task_t* cur = sched_current();
            if (cur) {
                sched_reap_zombies(cur);
            }
            shell_waiting_for_program = 0;
            kprintf("\n");
            shell_prompt();
            console_cursor_enable();
        }
        // Don't process keyboard input while program is running, but keep checking
        return 1;  // Return 1 to prevent HLT so we keep checking task completion
    }
    
    char c = keyboard_get_char();

    if (c == 0) {
        return 0;
    }

    if (c == '\n') {
        cmd_buf[cmd_len] = '\0';
        kprintf("\n");
        if (cmd_len > 0) {
            int start = 0;
            while (cmd_buf[start] == ' ' && start < cmd_len) {
                start++;
            }
            int sp = start;
            while (sp < cmd_len && cmd_buf[sp] != ' ') {
                sp++;
            }
            int cmdlen = sp - start;
            int is_ls = (cmdlen == 2 && cmd_buf[start] == 'l' && cmd_buf[start + 1] == 's');
            int is_cat = (cmdlen == 3 && cmd_buf[start] == 'c' && cmd_buf[start + 1] == 'a' && cmd_buf[start + 2] == 't');
            int is_cd = (cmdlen == 2 && cmd_buf[start] == 'c' && cmd_buf[start + 1] == 'd');
            int is_pwd = (cmdlen == 3 && cmd_buf[start] == 'p' && cmd_buf[start + 1] == 'w' && cmd_buf[start + 2] == 'd');
            int is_stat = (cmdlen == 4 && cmd_buf[start] == 's' && cmd_buf[start + 1] == 't' && cmd_buf[start + 2] == 'a' && cmd_buf[start + 3] == 't');
            int is_help = (cmdlen == 4 && cmd_buf[start] == 'h' && cmd_buf[start + 1] == 'e' && cmd_buf[start + 2] == 'l' && cmd_buf[start + 3] == 'p');
            if (is_help) {
                kprintf("LikeOS-64 Shell - Available Commands:\n");
                kprintf("  ls [path]      - List directory contents\n");
                kprintf("  cd <dir>       - Change directory\n");
                kprintf("  pwd            - Print working directory\n");
                kprintf("  cat <file>     - Display file contents\n");
                kprintf("  stat <path>    - Show file/directory information\n");
                kprintf("  help           - Display this help message\n");
                kprintf("  ./<file>       - Execute a program (e.g., ./tests)\n");
            } else if (is_ls) {
                if (block_count() > 0) {
                    int fn = sp;
                    while (fn < cmd_len && cmd_buf[fn] == ' ') {
                        fn++;
                    }
                    unsigned long list_cluster;
                    if (fn >= cmd_len) {
                        list_cluster = fat32_get_cwd();
                    } else {
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
                    if (path[0] == '/') {
                        fat32_set_cwd(0);
                        shell_path_reset();
                    }
                    char segment[64];
                    int idx = 0;
                    int i = 0;
                    while (1) {
                        char ch = path[i];
                        if (ch == '/' || ch == '\0') {
                            segment[idx] = '\0';
                            if (idx > 0) {
                                if (segment[0] == '.' && segment[1] == '\0') {
                                } else if (segment[0] == '.' && segment[1] == '.' && segment[2] == '\0') {
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
                int fn = sp;
                while (fn < cmd_len && cmd_buf[fn] == ' ') {
                    fn++;
                }
                if (fn >= cmd_len) {
                    kprintf("Usage: cat <file>\n");
                } else {
                    vfs_file_t* vf = 0;
                    const char* name = &cmd_buf[fn];
                    char pathbuf[128];
                    (void)pathbuf;
                    if (name[0] != '/') {
                        unsigned long cc = fat32_get_cwd();
                        if (cc != fat32_root_cluster()) {
                            name = &cmd_buf[fn];
                        }
                    }
                    if (vfs_open(name, 0, &vf) == ST_OK) {
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
                // Check if command starts with ./ (execute ELF)
                int is_exec = (cmdlen >= 2 && cmd_buf[start] == '.' && cmd_buf[start + 1] == '/');
                if (is_exec) {
                    // Extract path (skip the ./)
                    const char* exec_path = &cmd_buf[start + 1];  // Keep the leading /
                    
                    // Build argv: program name and any arguments
                    // For now, just use the path as argv[0]
                    char* argv[2];
                    argv[0] = (char*)exec_path;
                    argv[1] = NULL;
                    
                    // Build envp with PATH=/
                    char* envp[2];
                    char path_env[] = "PATH=/";
                    envp[0] = path_env;
                    envp[1] = NULL;
                    
                    int ret = elf_exec(exec_path, argv, envp, NULL);
                    if (ret != 0) {
                        kprintf("exec: failed (error %d)\n", ret);
                    } else {
                        // Program started successfully - wait for it to complete
                        shell_waiting_for_program = 1;
                    }
                } else {
                    kprintf("Unknown command\n");
                }
            }
after_cmd:
            ;
        }
        cmd_len = 0;
        // Only show prompt if we're not waiting for a program
        if (!shell_waiting_for_program) {
            shell_prompt();
            console_cursor_enable();  // Re-enable cursor after command
        }
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
    return 1;
}
