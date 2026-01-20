#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

// Options for waitpid
#define WNOHANG     1   // Don't block
#define WUNTRACED   2   // Also wait for stopped children

// Macros to interpret status
#define WIFEXITED(status)    (((status) & 0x7f) == 0)
#define WEXITSTATUS(status)  (((status) >> 8) & 0xff)
#define WIFSIGNALED(status)  (((status) & 0x7f) != 0)
#define WTERMSIG(status)     ((status) & 0x7f)
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)     (((status) >> 8) & 0xff)

pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);

#endif
