#include <kernel/ke/userinit.h>
#include <kernel/io/console.h>
#include <kernel/ke/elf.h>
#include <kernel/ke/sched.h>
#include <kernel/io/tty.h>
#include <kernel/fs/vfs.h>
#include <kernel/ke/syscall.h>
#include <kernel/uapi/bug.h>

static int g_init_started = 0;

static int init_spawn(void)
{
	if (!vfs_root_ready()) {
		return -EAGAIN;
	}
	/* PID 1 is /sbin/init; it spawns getty, which authenticates via login
	 * and starts the user's shell.  init runs in its own session but has no
	 * controlling terminal: getty calls setsid()+TIOCSCTTY to claim the
	 * console for each login session. */
	char *argv[] = { "/sbin/init", NULL };
	char *envp[] = { "PATH=/bin:/usr/local/bin", NULL };
	task_t *task = NULL;
	int ret = elf_exec("/sbin/init", argv, envp, &task);
	if (ret == 0 && task) {
		/* Reserve process id 1 for init and make it a session leader.
		 * tgid must track id so getpid() (which returns tgid) reports 1.
		 * The kernel then protects it from stray signals and panics if
		 * it ever exits (see sched_mark_task_exited). */
		task->id = 1;
		task->tgid = 1;
		task->pgid = task->id;
		task->sid = task->id;
		sched_set_init_task(task);
		g_init_started = 1;
	} else if (ret != -EAGAIN) {
		kprintf("init: failed to start /sbin/init (error %d)\n", ret);
	}
	return ret;
}

void userinit_start(void)
{
	BUG_ON(tty_get_console() ==
	       NULL); /* userinit: console TTY not ready */
	tty_reset_termios(tty_get_console());
	init_spawn();
	console_cursor_enable();
}

void userinit_redisplay_prompt(void)
{
	// User space (getty/login/sh) owns its own prompt.
}

int userinit_tick(void)
{
	/* Launch init once the root filesystem is ready.  init is not
	 * respawned if it exits: the kernel panics instead (init must never
	 * die), so here we only need to keep retrying the initial start until
	 * the filesystem becomes available. */
	if (!g_init_started) {
		init_spawn();
	}
	return 0;
}
