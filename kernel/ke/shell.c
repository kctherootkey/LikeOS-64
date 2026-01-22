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

// Simple argument handling
#define SHELL_MAX_ARGS 16

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

    // If user is scrolled up, snap back to bottom on any keypress
    console_scroll_to_bottom();

    if (c == '\n') {
        cmd_buf[cmd_len] = '\0';
        kprintf("\n");
        if (cmd_len > 0) {
            // Tokenize command line (simple whitespace split, no quotes)
            char* argv[SHELL_MAX_ARGS + 1];
            int argc = 0;
            cmd_buf[cmd_len] = '\0';
            char* p = cmd_buf;
            while (*p && argc < SHELL_MAX_ARGS) {
                while (*p == ' ') p++;
                if (!*p) break;
                argv[argc++] = p;
                while (*p && *p != ' ') p++;
                if (*p) {
                    *p = '\0';
                    p++;
                }
            }
            argv[argc] = 0;
            if (argc == 0) {
                goto after_cmd;
            }

            const char* cmd = argv[0];
            int is_ls = (kstrcmp(cmd, "ls") == 0);
            int is_cat = (kstrcmp(cmd, "cat") == 0);
            int is_cd = (kstrcmp(cmd, "cd") == 0);
            int is_pwd = (kstrcmp(cmd, "pwd") == 0);
            int is_stat = (kstrcmp(cmd, "stat") == 0);
            int is_help = (kstrcmp(cmd, "help") == 0);
            if (is_help) {
                kprintf("LikeOS-64 Shell - Available Commands:\n");
                kprintf("  ls [path]      - List directory contents\n");
                kprintf("  cd <dir>       - Change directory\n");
                kprintf("  pwd            - Print working directory\n");
                kprintf("  cat <file>     - Display file contents\n");
                kprintf("  stat <path>    - Show file/directory information\n");
                kprintf("  help           - Display this help message\n");
                kprintf("  <cmd> [args]   - Execute program via PATH (/), ./, or absolute path\n");
            } else if (is_ls) {
                if (block_count() > 0) {
                    unsigned long list_cluster;
                    if (argc < 2) {
                        list_cluster = fat32_get_cwd();
                    } else {
                        const char* path = argv[1];
                        unsigned a;
                        unsigned long fc;
                        unsigned long sz;
                        if (fat32_resolve_path(fat32_get_cwd(), path, &a, &fc, &sz) == ST_OK) {
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
                if (argc < 2) {
                    kprintf("Usage: cd <dir>\n");
                } else {
                    const char* path = argv[1];
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
                if (argc < 2) {
                    kprintf("Usage: stat <path>\n");
                } else {
                    unsigned a;
                    unsigned long fc;
                    unsigned long sz;
                    if (fat32_stat(fat32_get_cwd(), argv[1], &a, &fc, &sz) == ST_OK) {
                        kprintf("attr=%c size=%lu cluster=%lu\n", (a & 0x10) ? 'd' : 'f', sz, fc);
                    } else {
                        kprintf("stat: not found\n");
                    }
                }
            } else if (is_cat) {
                if (argc < 2) {
                    kprintf("Usage: cat <file>\n");
                } else {
                    vfs_file_t* vf = 0;
                    const char* name = argv[1];
                    char pathbuf[128];
                    (void)pathbuf;
                    if (name[0] != '/') {
                        unsigned long cc = fat32_get_cwd();
                        if (cc != fat32_root_cluster()) {
                            name = argv[1];
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
                // Execute program (absolute path, relative path with '/', or PATH lookup)
                const char* exec_path = argv[0];
                char exec_buf[128];
                int has_slash = 0;
                for (const char* s = argv[0]; *s; ++s) {
                    if (*s == '/') { has_slash = 1; break; }
                }
                if (!has_slash) {
                    size_t cmdlen = kstrlen(argv[0]);
                    if (cmdlen + 1 >= sizeof(exec_buf)) {
                        kprintf("exec: path too long\n");
                        goto after_cmd;
                    }
                    exec_buf[0] = '/';
                    kstrcpy(&exec_buf[1], argv[0]);
                    exec_path = exec_buf;
                }

                struct kstat st;
                if (vfs_stat(exec_path, &st) != ST_OK) {
                    kprintf("exec: not found: %s\n", exec_path);
                    goto after_cmd;
                }

                // Build argv for exec (argv[0] = exec_path)
                char* exec_argv[SHELL_MAX_ARGS + 1];
                exec_argv[0] = (char*)exec_path;
                for (int i = 1; i < argc && i < SHELL_MAX_ARGS; ++i) {
                    exec_argv[i] = argv[i];
                }
                exec_argv[(argc < SHELL_MAX_ARGS) ? argc : SHELL_MAX_ARGS] = NULL;

                // Build envp with PATH=/
                char* envp[2];
                static char path_env[] = "PATH=/";
                envp[0] = path_env;
                envp[1] = NULL;

                int ret = elf_exec(exec_path, exec_argv, envp, NULL);
                if (ret != 0) {
                    kprintf("exec: failed (error %d)\n", ret);
                } else {
                    // Program started successfully - wait for it to complete
                    shell_waiting_for_program = 1;
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
