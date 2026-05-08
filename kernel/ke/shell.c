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
    char* envp[] = { "PATH=/bin:/usr/local/bin", NULL };
    task_t* task = NULL;
    int ret = elf_exec("/bin/sh", argv, envp, &task);
    if (ret == 0 && task) {
        // Make the shell a session leader with its own process group
        task->pgid = task->id;
        task->sid  = task->id;
        tty_t* tty = tty_get_console();
        if (tty) {
            task->ctty = tty;
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
    if (g_shell_task && (g_shell_task->has_exited || g_shell_task->state == TASK_ZOMBIE)) {
        /* The shell task is done.  Do NOT call sched_reap_zombies here.
         * When the shell's parent is the bootstrap task, sched_mark_task_exited
         * clears exit_signal so that the context-switch path enqueues the task
         * in dead_thread_queue, which calls sched_remove_task() after the CPU
         * has safely switched away.  Calling sched_reap_zombies() here causes a
         * second sched_remove_task() on the same task_t → double-free → page fault
         * at task->parent (offset 0x60).  Nulling the pointer is sufficient;
         * the kernel owns the task lifecycle from this point. */
        g_shell_task = NULL;
    }
    if (!g_shell_task) {
        shell_spawn();
    }
    return 0;
}
