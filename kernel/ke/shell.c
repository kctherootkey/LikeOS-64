#include "../../include/kernel/shell.h"
#include "../../include/kernel/console.h"
#include "../../include/kernel/elf.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/tty.h"
#include "../../include/kernel/vfs.h"
#include "../../include/kernel/syscall.h"

static task_t* g_shell_task = NULL;

static int shell_spawn(void) {
    if (!vfs_root_ready()) {
        return -EAGAIN;
    }
    char* argv[] = { "/bin/sh", NULL };
    char* envp[] = { "PATH=/bin:/", NULL };
    task_t* task = NULL;
    int ret = elf_exec("/bin/sh", argv, envp, &task);
    if (ret == 0 && task) {
        tty_t* tty = tty_get_console();
        if (tty) {
            tty->fg_pgid = task->pgid;
        }
        g_shell_task = task;
    } else if (ret != -EAGAIN) {
        g_shell_task = NULL;
        kprintf("shell: failed to start /bin/sh (error %d)\n", ret);
    }
    return ret;
}

void shell_init(void) {
    tty_reset_termios(tty_get_console());
    shell_spawn();
    console_cursor_enable();
}

void shell_redisplay_prompt(void) {
    // Userland /bin/sh handles its own prompt.
}

int shell_tick(void) {
    task_t* cur = sched_current();
    if (g_shell_task && (g_shell_task->has_exited || g_shell_task->state == TASK_ZOMBIE)) {
        if (cur) {
            sched_reap_zombies(cur);
        }
        g_shell_task = NULL;
    }
    if (!g_shell_task) {
        shell_spawn();
    }
    return 0;
}
