/* <ucontext.h> -- user-level context switching.
 *
 * getcontext() records the calling thread's registers, signal mask and
 * stack; setcontext() resumes such a record; makecontext() rewrites one so
 * that resuming it calls a function on a stack of the caller's choosing;
 * swapcontext() saves the current context and resumes another in one step.
 * Coroutine libraries and green-thread schedulers are built on these. */
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

int getcontext(ucontext_t *ucp);
int setcontext(const ucontext_t *ucp);
int swapcontext(ucontext_t *oucp, const ucontext_t *ucp);
/* argc integer arguments follow; up to six are passed in registers and any
 * beyond that on the new stack.  When func returns, the context in
 * ucp->uc_link is resumed, or the thread exits if that is NULL. */
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);

#ifdef __cplusplus
}
#endif

#endif /* _UCONTEXT_H */
