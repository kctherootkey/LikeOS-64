// LikeOS user-space init launcher
// Starts /sbin/init (PID 1) once the root filesystem is ready and keeps it
// alive: init spawns getty, which runs login and the user's shell.
//
// Copyright (C) 2026 The LikeOS Project

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

// Late boot work that still prints to the console after init is running:
// the firmware loads and device bring-up that follow the root mount.  Each
// begin is matched by one end, from any context.  The boot has settled once
// that work has started and none of it is left; getty waits for this
// (/proc/boot_settled) before its first login prompt, so the prompt is not
// buried under the messages.
void boot_work_begin(void);
void boot_work_end(void);
int boot_settled(void);

#endif // _KERNEL_USERINIT_H_
