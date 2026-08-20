/*
 * fenv.c - the floating-point environment, on both of this machine's FPUs.
 *
 * x86-64 has two, and every function here touches both.  The x87 unit keeps a
 * control word and a status word inside a 28-byte block that fnstenv writes
 * and fldenv reads; SSE keeps the same information in one 32-bit register,
 * MXCSR, reached with stmxcsr and ldmxcsr.  Which unit runs a given expression
 * is the compiler's choice -- double and float go to SSE, long double to x87 --
 * so reading only one of them would report exception flags that come and go
 * with the type an expression happened to use.
 *
 * MXCSR reuses the x87 status-word bit positions for its own exception flags
 * (bits 0-5, in the same order), which is why a single FE_* mask can be applied
 * to both registers.  Everything ELSE in MXCSR sits somewhere of its own: the
 * exception masks are bits 7-12, the rounding control bits 13-14, and bits 16
 * and up are reserved.  The two shift constants below are those offsets, and
 * getting one wrong is not a wrong answer but a #GP -- see MXCSR_MASK_SHIFT.
 *
 * One instruction needs care: FNSTENV does not only store the environment, it
 * also MASKS every exception as a side effect.  Every use of it here is
 * therefore paired with an FLDENV that puts a control word back, and the
 * pairing is not optional -- dropping it leaves the process unable to trap on
 * anything for the rest of its life.
 */
#include <fenv.h>

/* Where MXCSR keeps the exception masks: the same six bits as the flags,
 * shifted up by SEVEN.
 *
 * Flags are bits 0-5, DAZ is bit 6, and the masks are bits 7-12 -- so the
 * six-bit mask field starts one bit above the flags, not twelve.  FE_ALL_EXCEPT
 * shifted by 7 is 0x1F80, which is also the value the hardware resets MXCSR to
 * (every exception masked, round to nearest); load_default_env() below spells
 * that same number out, which is the cross-check that this shift is right.
 *
 * It was 12 here, and 0x3F << 12 is 0x3F000 -- bits 16 and 17, which are
 * RESERVED.  LDMXCSR faults with #GP on any reserved bit set, so feholdexcept()
 * killed the process outright rather than masking anything. */
#define MXCSR_MASK_SHIFT 7

/* Where MXCSR keeps the rounding control: the same two bits as the x87 control
 * word's, shifted up by three (x87 bits 10-11, MXCSR bits 13-14). */
#define MXCSR_ROUND_SHIFT 3

static inline unsigned int mxcsr_get(void)
{
	unsigned int v;

	__asm__ __volatile__("stmxcsr %0" : "=m"(v));
	return v;
}

static inline void mxcsr_set(unsigned int v)
{
	/* Bits 16-31 are reserved and LDMXCSR raises #GP if any of them is
	 * set -- an instruction fault, not an error return, so a bad value
	 * here kills the process.
	 *
	 * fesetenv() and feupdateenv() take a fenv_t from the caller, and it
	 * is only ever as trustworthy as whatever produced it: a structure
	 * that was never filled in by fegetenv(), or one saved by a different
	 * build.  Masking to the architectural sixteen bits means such a value
	 * gives a wrong rounding mode at worst, instead of a crash. */
	v &= 0xFFFFu;
	__asm__ __volatile__("ldmxcsr %0" : : "m"(v));
}

static inline unsigned short x87_status(void)
{
	unsigned short sw;

	__asm__ __volatile__("fnstsw %0" : "=am"(sw));
	return sw;
}

int feclearexcept(int excepts)
{
	fenv_t env;
	unsigned int mxcsr;

	excepts &= FE_ALL_EXCEPT;
	if (!excepts)
		return 0;

	/* The x87 side.  The status word cannot be written directly, so the
	 * whole environment goes out, the bits are cleared in the copy, and it
	 * comes back -- which also undoes the masking FNSTENV just did, since
	 * the control word in the copy is the original one. */
	__asm__ __volatile__("fnstenv %0" : "=m"(env));
	env.__status_word &= (unsigned short)~excepts;
	__asm__ __volatile__("fldenv %0" : : "m"(env));

	mxcsr = mxcsr_get();
	mxcsr &= ~(unsigned int)excepts;
	mxcsr_set(mxcsr);
	return 0;
}

int feraiseexcept(int excepts)
{
	fenv_t env;
	unsigned int mxcsr;

	excepts &= FE_ALL_EXCEPT;
	if (!excepts)
		return 0;

	/* Setting the status bits, rather than performing operations chosen to
	 * provoke each exception.
	 *
	 * The two differ only when an exception is UNMASKED, where a real
	 * operation would trap at the point it happened and this does not.
	 * Nothing on this system unmasks any of them -- there is no SIGFPE
	 * delivery for masked-off arithmetic to escape from -- and the flags,
	 * which are what every caller of this reads, come out identical. */
	__asm__ __volatile__("fnstenv %0" : "=m"(env));
	env.__status_word |= (unsigned short)excepts;
	__asm__ __volatile__("fldenv %0" : : "m"(env));

	mxcsr = mxcsr_get();
	mxcsr |= (unsigned int)excepts;
	mxcsr_set(mxcsr);
	return 0;
}

int fetestexcept(int excepts)
{
	/* The union of the two units: an exception raised on either one has
	 * been raised as far as the program is concerned. */
	return (int)((x87_status() | mxcsr_get()) & (unsigned)excepts &
		     FE_ALL_EXCEPT);
}

int fegetexceptflag(fexcept_t *flagp, int excepts)
{
	if (!flagp)
		return -1;
	*flagp = (fexcept_t)fetestexcept(excepts);
	return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts)
{
	fenv_t env;
	unsigned int mxcsr;
	unsigned int set;

	if (!flagp)
		return -1;
	excepts &= FE_ALL_EXCEPT;
	set = (unsigned int)*flagp & (unsigned int)excepts;

	/* Set the flags that are in the saved set and clear the ones that are
	 * not -- within `excepts' only.  This is a restore, not a raise: the
	 * standard is explicit that it must not trap even where feraiseexcept
	 * would. */
	__asm__ __volatile__("fnstenv %0" : "=m"(env));
	env.__status_word &= (unsigned short)~excepts;
	env.__status_word |= (unsigned short)set;
	__asm__ __volatile__("fldenv %0" : : "m"(env));

	mxcsr = mxcsr_get();
	mxcsr &= ~(unsigned int)excepts;
	mxcsr |= set;
	mxcsr_set(mxcsr);
	return 0;
}

int fegetround(void)
{
	unsigned short cw;

	/* From the x87 control word, which is where the FE_* rounding values
	 * are defined to live.  MXCSR is kept in step by fesetround, so either
	 * would answer; this one needs no shift. */
	__asm__ __volatile__("fnstcw %0" : "=m"(cw));
	return cw & 0xC00;
}

int fesetround(int round)
{
	unsigned short cw;
	unsigned int mxcsr;

	switch (round) {
	case FE_TONEAREST:
	case FE_DOWNWARD:
	case FE_UPWARD:
	case FE_TOWARDZERO:
		break;
	default:
		return -1; /* not a rounding direction: change nothing */
	}

	__asm__ __volatile__("fnstcw %0" : "=m"(cw));
	cw = (unsigned short)((cw & ~0xC00) | round);
	__asm__ __volatile__("fldcw %0" : : "m"(cw));

	/* And the same direction on the SSE side, three bits further up. */
	mxcsr = mxcsr_get();
	mxcsr = (mxcsr & ~0x6000u) |
		((unsigned int)round << MXCSR_ROUND_SHIFT);
	mxcsr_set(mxcsr);
	return 0;
}

int fegetenv(fenv_t *envp)
{
	if (!envp)
		return -1;

	__asm__ __volatile__("fnstenv %0" : "=m"(*envp));
	/* FNSTENV has just masked every x87 exception.  Put the control word
	 * it saved straight back, so reading the environment does not change
	 * it -- which is the one thing fegetenv must not do. */
	__asm__ __volatile__("fldenv %0" : : "m"(*envp));
	envp->__mxcsr = mxcsr_get();
	return 0;
}

/* The environment as it is at program start: every exception masked and clear,
 * round to nearest, extended precision.  0x037F is the x87 control word the
 * hardware itself comes up with after FINIT; 0x1F80 is the MXCSR reset value.
 */
static void load_default_env(void)
{
	static const fenv_t dfl = {
		.__control_word = 0x037F,
		.__status_word = 0,
		.__tags = 0xFFFF, /* every register empty */
		.__mxcsr = 0x1F80,
	};

	__asm__ __volatile__("fldenv %0" : : "m"(dfl));
	mxcsr_set(dfl.__mxcsr);
}

int fesetenv(const fenv_t *envp)
{
	if (!envp)
		return -1;
	if (envp == FE_DFL_ENV) {
		load_default_env();
		return 0;
	}
	__asm__ __volatile__("fldenv %0" : : "m"(*envp));
	mxcsr_set(envp->__mxcsr);
	return 0;
}

int feholdexcept(fenv_t *envp)
{
	fenv_t env;
	unsigned int mxcsr;

	if (!envp)
		return -1;
	if (fegetenv(envp) != 0)
		return -1;

	/* Clear every flag and mask every exception, so the block that follows
	 * runs without trapping and its flags can be read on their own.
	 *
	 * On the x87 side FNSTENV has already done the masking for us; all
	 * that is left is to clear the status word before loading it back. */
	env = *envp;
	env.__control_word |= FE_ALL_EXCEPT;
	env.__status_word = 0;
	__asm__ __volatile__("fldenv %0" : : "m"(env));

	mxcsr = envp->__mxcsr;
	mxcsr &= ~(unsigned int)FE_ALL_EXCEPT;              /* flags clear */
	mxcsr |= (unsigned int)FE_ALL_EXCEPT << MXCSR_MASK_SHIFT; /* masked */
	mxcsr_set(mxcsr);
	return 0;
}

int feupdateenv(const fenv_t *envp)
{
	int raised;

	if (!envp)
		return -1;

	/* What has been raised since the environment was saved has to survive
	 * being put back, so it is read first and re-raised after. */
	raised = fetestexcept(FE_ALL_EXCEPT);
	if (fesetenv(envp) != 0)
		return -1;
	return feraiseexcept(raised);
}
