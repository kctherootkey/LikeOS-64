// LikeOS-64 user-space init launcher
// Starts /sbin/init (PID 1) once the root filesystem is ready and keeps it
// alive: init spawns getty, which runs login and the user's shell.

#ifndef _KERNEL_USERINIT_H_
#define _KERNEL_USERINIT_H_

// Prepare the console and launch /sbin/init.
void userinit_start(void);

// Poll once; re-launch /sbin/init if it has exited.  Returns 0.
int userinit_tick(void);

// Hook called after the kernel prints asynchronous console messages (e.g.
// mount notifications).  Prompt redisplay is owned by user space, so this is
// currently a no-op kept for call-site compatibility.
void userinit_redisplay_prompt(void);

#endif // _KERNEL_USERINIT_H_
