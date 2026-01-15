// LikeOS-64 Simple Shell interface
// Provides initialization and polling for the console shell

#ifndef _KERNEL_SHELL_H_
#define _KERNEL_SHELL_H_

// Initialize shell state and print initial prompt
void shell_init(void);

// Poll shell input once; returns non-zero if a character was handled
int shell_tick(void);

#endif // _KERNEL_SHELL_H_
