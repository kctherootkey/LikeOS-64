/* makecontext(3): rewrite a context so that resuming it calls `func' on
 * ucp->uc_stack, then continues with ucp->uc_link (or exits). */
#include <ucontext.h>
#include <stdarg.h>
#include <stdint.h>

extern void __start_context(void);

void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...)
{
	uintptr_t top = (uintptr_t)ucp->uc_stack.ss_sp + ucp->uc_stack.ss_size;
	int nstack = argc > 6 ? argc - 6 : 0;
	uintptr_t *sp;
	va_list ap;

	/* Room for the stack-passed arguments and the return slot, then
	 * align so that `func' is entered with RSP % 16 == 8, as after a
	 * call.  The link register value (uc_link) rides in RBX, which the
	 * callee preserves, so __start_context can find it after return. */
	sp = (uintptr_t *)((top - (nstack + 1) * sizeof(uintptr_t)) & ~15UL);
	sp -= 1; /* now (sp) is the return address slot, sp % 16 == 8 */
	sp[0] = (uintptr_t)__start_context;

	greg_t *g = ucp->uc_mcontext.gregs;
	g[REG_RIP] = (greg_t)(uintptr_t)func;
	g[REG_RSP] = (greg_t)(uintptr_t)sp;
	g[REG_RBX] = (greg_t)(uintptr_t)ucp->uc_link;
	g[REG_RBP] = 0;

	va_start(ap, argc);
	for (int i = 0; i < argc; i++) {
		greg_t a = va_arg(ap, greg_t);

		switch (i) {
		case 0: g[REG_RDI] = a; break;
		case 1: g[REG_RSI] = a; break;
		case 2: g[REG_RDX] = a; break;
		case 3: g[REG_RCX] = a; break;
		case 4: g[REG_R8] = a; break;
		case 5: g[REG_R9] = a; break;
		default: sp[1 + (i - 6)] = (uintptr_t)a; break;
		}
	}
	va_end(ap);

	/* A sane FPU environment for the new function: whatever the caller
	 * has right now. */
	ucp->uc_mcontext.fpregs = &ucp->__fpregs_mem;
	__asm__ volatile("fxsave (%0)" : : "r"(&ucp->__fpregs_mem) : "memory");
}
