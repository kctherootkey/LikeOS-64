/*
 * fenv.h - the floating-point environment.
 *
 * C99 7.6.  Three things live in here: the exception flags a computation has
 * raised (invalid, overflow, inexact and friends), the rounding direction, and
 * the whole environment as one object that can be saved and put back.
 *
 * On x86-64 there are TWO floating-point units and every one of these
 * functions has to touch both.  The x87 unit has its own control and status
 * words, reached with fnstenv/fldenv and fnstsw; SSE has MXCSR, reached with
 * stmxcsr/ldmxcsr.  Ordinary double arithmetic compiles to SSE, and long
 * double arithmetic to x87, so a program that ignored one of them would see
 * exception flags appear and disappear depending on which type the expression
 * happened to use.  The implementation therefore reads the union of the two
 * and writes both.
 *
 * The exception macros are the x87 status-word bit positions, which MXCSR
 * deliberately reuses for its own low six bits -- that is why one mask can be
 * applied to both.
 */
#ifndef _FENV_H
#define _FENV_H

#ifdef __cplusplus
extern "C" {
#endif

/* The exceptions.  FE_DENORMAL is an x86 extension, present on this hardware
 * and absent from the standard's list; it is included because the hardware
 * raises it and code that clears FE_ALL_EXCEPT should be clearing it too. */
#define FE_INVALID 0x01
#define FE_DENORMAL 0x02
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20

#define FE_ALL_EXCEPT 0x3F

/* The rounding directions, as the x87 control word encodes them (bits 10-11).
 * MXCSR uses the same two-bit code three bits further up, which fesetround
 * shifts for. */
#define FE_TONEAREST 0x000
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xC00

/* A set of exception flags, on its own.  Wide enough for FE_ALL_EXCEPT. */
typedef unsigned short fexcept_t;

/* The whole environment.
 *
 * The first seven fields are the 28-byte block fnstenv writes in 64-bit mode,
 * laid out exactly as the hardware defines it -- so a fenv_t can be handed
 * straight to fldenv.  __mxcsr is appended because the SSE unit has no part in
 * that block and its state has to travel with the rest.
 */
typedef struct {
	unsigned short __control_word;
	unsigned short __unused1;
	unsigned short __status_word;
	unsigned short __unused2;
	unsigned short __tags;
	unsigned short __unused3;
	unsigned int __eip;
	unsigned short __cs_selector;
	unsigned short __opcode;
	unsigned int __data_offset;
	unsigned short __data_selector;
	unsigned short __unused5;
	unsigned int __mxcsr;
} fenv_t;

/* The environment in force at program start: all exceptions masked, flags
 * clear, round to nearest.  A pointer value rather than an object, as the
 * standard requires, and one that cannot be a real address. */
#define FE_DFL_ENV ((const fenv_t *)-1)

/* Clear the given exception flags.  Returns 0 on success. */
int feclearexcept(int __excepts);

/* Raise the given exception flags, as though a computation had. */
int feraiseexcept(int __excepts);

/* Which of the given flags are currently raised. */
int fetestexcept(int __excepts);

/* Save and restore a subset of the flags. */
int fegetexceptflag(fexcept_t *__flagp, int __excepts);
int fesetexceptflag(const fexcept_t *__flagp, int __excepts);

/* The rounding direction: one of the four FE_ macros above, or a negative
 * value from fegetround if it cannot be determined. */
int fegetround(void);
int fesetround(int __round);

/* The environment as a whole. */
int fegetenv(fenv_t *__envp);
int fesetenv(const fenv_t *__envp);

/* Save the environment, then clear the flags and mask every exception, so a
 * block of computation cannot trap and its flags can be inspected on their
 * own.  feupdateenv puts the saved environment back and re-raises whatever was
 * raised in the meantime. */
int feholdexcept(fenv_t *__envp);
int feupdateenv(const fenv_t *__envp);

#ifdef __cplusplus
}
#endif

#endif /* _FENV_H */
