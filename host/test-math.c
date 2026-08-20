/*
 * test-math.c - check libc's hyperbolic functions against the host's glibc.
 *
 * The inverse hyperbolics are the reason this exists.  Each one has a closed
 * form that is easy to write and wrong at both ends of its domain -- asinh of
 * a tiny argument cancels to exactly zero, asinh of a large one overflows on
 * the x*x while the answer is an ordinary number -- so src/math/math.c writes
 * them in ranges instead.  Range-split code has seams, and a seam is exactly
 * the sort of thing that looks right, passes a spot check at 1.0, and is off
 * by a factor of two at 2.0000001.  So the whole domain is swept and every
 * value is compared against a reference implementation.
 *
 * glibc is that reference.  Its libm is correctly rounded to well under an ulp
 * over these domains, which is far tighter than anything here needs to be; the
 * tolerance below is what this libc promises, not what glibc delivers.
 *
 * How the two implementations coexist in one program:  math.c defines sqrt,
 * log, fabs and the rest under their standard names, which are glibc's names
 * too.  Rather than rename them one at a time with -D (there are sixty), the
 * shell script compiles math.c on its own and runs objcopy --prefix-symbols
 * over the result, so every symbol it defines AND every symbol it calls gains
 * an lk_ prefix in one step.  math.c calls nothing outside itself, so the
 * renaming is self-consistent and the object still links.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The renamed copies of the functions under test. */
double lk_sinh(double);
double lk_cosh(double);
double lk_tanh(double);
double lk_asinh(double);
double lk_acosh(double);
double lk_atanh(double);
float lk_asinhf(float);
double lk_log1p(double);
double lk_expm1(double);
long double lk_asinhl(long double);

/* <fenv.h>, from the same libc.  Every one of these executes a real
 * LDMXCSR/FLDENV on this machine's own FPU, so a wrong register-layout
 * constant faults HERE rather than on the target. */
#include <fenv.h>
int lk_feclearexcept(int);
int lk_feraiseexcept(int);
int lk_fetestexcept(int);
int lk_fegetround(void);
int lk_fesetround(int);
int lk_fegetenv(void *);
int lk_fesetenv(const void *);
int lk_feholdexcept(void *);
int lk_feupdateenv(const void *);

static int failures;
static long checks;

/* Per-function tallies.  A sweep makes thousands of comparisons, and the
 * printed failures are capped, so without this a single broken function looks
 * identical to every function being broken. */
#define MAX_FUNCS 32
static struct {
	const char *name;
	long checks;
	long fails;
	double worst;
	double worst_at;
} tally[MAX_FUNCS];
static int ntally;

static int tally_slot(const char *name)
{
	for (int i = 0; i < ntally; i++)
		if (strcmp(tally[i].name, name) == 0)
			return i;
	tally[ntally].name = name;
	return ntally++;
}

/* Relative error in units in the last place.
 *
 * Absolute error is the wrong measure for a function whose output spans
 * hundreds of orders of magnitude: 1e-15 is a catastrophe near 1e-300 and
 * beneath notice near 1e300.  Ulps are the same size everywhere.
 */
static double ulp_error(double got, double want)
{
	if (got == want)
		return 0.0;
	/* Both NaN counts as agreement; one NaN does not. */
	if (isnan(got) || isnan(want))
		return (isnan(got) && isnan(want)) ? 0.0 : INFINITY;
	if (isinf(got) || isinf(want))
		return INFINITY;
	if (want == 0.0)
		return fabs(got) / 0x1p-1074; /* every subnormal step away */

	{
		double ulp = nextafter(fabs(want), INFINITY) - fabs(want);

		return fabs(got - want) / ulp;
	}
}

/* MXCSR as the CPU actually holds it.
 *
 * The libc's fenv functions write this very register, so reading it back
 * directly is how the LAYOUT gets tested -- which is the thing that can be
 * wrong here.  Asserting on a fault instead would prove nothing: mxcsr_set()
 * masks reserved bits precisely so a bad value cannot kill the process, and
 * that mask would swallow the evidence. */
static unsigned int host_mxcsr(void)
{
	unsigned int v;

	__asm__ __volatile__("stmxcsr %0" : "=m"(v));
	return v;
}

/* A plain pass/fail, for the checks that are not a numeric comparison. */
static void fail(const char *what, const char *detail)
{
	int t = tally_slot(what);

	checks++;
	tally[t].checks++;
	tally[t].fails++;
	if (++failures <= 12)
		printf("  FAIL %-12s %s\n", what, detail);
}

static void check(const char *name, double x, double got, double want,
		  double max_ulp)
{
	double e = ulp_error(got, want);
	int t = tally_slot(name);

	checks++;
	tally[t].checks++;
	if (e > tally[t].worst) {
		tally[t].worst = e;
		tally[t].worst_at = x;
	}
	if (e <= max_ulp)
		return;
	tally[t].fails++;
	if (++failures <= 12)
		printf("  FAIL %-12s(%- .17g)\n"
		       "        got  %- .17g\n"
		       "        want %- .17g   (%.1f ulp)\n",
		       name, x, got, want, e);
}

/* One function swept over one range.
 *
 * The sweep is geometric rather than linear: these functions change character
 * by orders of magnitude, not by fixed steps, and the interesting points --
 * the branch cutoffs at 2^-28, 0.5, 2 and 2^28 -- are decades apart.  A linear
 * sweep over [0, 1e30] would put every one of its samples in the last branch.
 */
static void sweep(const char *name, double (*mine)(double),
		  double (*ref)(double), double lo, double hi, double max_ulp)
{
	/* Multiplicative step chosen for about 4000 samples per sweep.
	 *
	 * The ratio is taken in log space rather than as hi/lo.  A sweep from
	 * 1e-320 to 1e300 spans 1e620, which is not a double: hi/lo came out
	 * as infinity, the step with it, and the very first multiply sent x
	 * past hi.  Every wide sweep then ran exactly one iteration and
	 * reported a clean pass over a domain it had never touched. */
	double step = exp((log(hi) - log(lo)) / 4000.0);

	for (double x = lo; x < hi; x *= step) {
		check(name, x, mine(x), ref(x), max_ulp);
		/* The odd functions again on the negative side: the sign
		 * handling is a separate line of code from the value. */
		if (mine != lk_acosh && mine != lk_cosh)
			check(name, -x, mine(-x), ref(-x), max_ulp);
	}
}

/* The exact branch boundaries, and the values either side of each.
 *
 * A range-split implementation is at its most fragile one ulp either side of
 * a cutoff, where two entirely different expressions have to agree.  The
 * sweep above steps past those points; this lands on them.
 */
static void boundaries(const char *name, double (*mine)(double),
		       double (*ref)(double), double max_ulp)
{
	static const double cuts[] = { 0x1p-28, 0.5,	1.0,   2.0,
				       0x1p28,	0x1p-30, 1e-300 };

	for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
		double c = cuts[i];
		double at[] = { nextafter(c, -INFINITY), c,
				nextafter(c, INFINITY) };

		for (unsigned j = 0; j < 3; j++) {
			check(name, at[j], mine(at[j]), ref(at[j]), max_ulp);
			check(name, -at[j], mine(-at[j]), ref(-at[j]),
			      max_ulp);
		}
	}
}

/* Values the standard names explicitly, which a sweep never lands on. */
static void specials(void)
{
	double inf = INFINITY;
	double nan = NAN;

	printf("special values\n");

	/* asinh: odd, defined everywhere, signed zero preserved. */
	check("asinh", 0.0, lk_asinh(0.0), 0.0, 0);
	if (signbit(lk_asinh(-0.0)) != 1) {
		printf("  FAIL asinh(-0) lost the sign of zero\n");
		failures++;
	}
	checks++;
	check("asinh", inf, lk_asinh(inf), inf, 0);
	check("asinh", -inf, lk_asinh(-inf), -inf, 0);
	check("asinh", nan, lk_asinh(nan), nan, 0);

	/* acosh: undefined below 1, zero at 1, +inf at +inf. */
	check("acosh", 1.0, lk_acosh(1.0), 0.0, 0);
	check("acosh", 0.5, lk_acosh(0.5), nan, 0);
	check("acosh", -1.0, lk_acosh(-1.0), nan, 0);
	check("acosh", inf, lk_acosh(inf), inf, 0);
	check("acosh", nan, lk_acosh(nan), nan, 0);

	/* atanh: poles at +-1, undefined outside, signed zero preserved. */
	check("atanh", 0.0, lk_atanh(0.0), 0.0, 0);
	if (signbit(lk_atanh(-0.0)) != 1) {
		printf("  FAIL atanh(-0) lost the sign of zero\n");
		failures++;
	}
	checks++;
	check("atanh", 1.0, lk_atanh(1.0), inf, 0);
	check("atanh", -1.0, lk_atanh(-1.0), -inf, 0);
	check("atanh", 2.0, lk_atanh(2.0), nan, 0);
	check("atanh", nan, lk_atanh(nan), nan, 0);

	/* The identities, which catch a wrong branch that is nevertheless a
	 * smooth function.  Checked at a value in each branch. */
	{
		static const double v[] = { 1e-10, 0.25, 1.5, 3.0, 1e10 };

		for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
			check("sinh(asinh)", v[i], lk_sinh(lk_asinh(v[i])),
			      v[i], 64);
			check("cosh(acosh)", 1.0 + v[i],
			      lk_cosh(lk_acosh(1.0 + v[i])), 1.0 + v[i], 64);
		}
		/* Only the values inside atanh's domain: v[2] is 1.5, and
		 * atanh(1.5) is correctly a NaN. */
		for (unsigned i = 0; i < 2; i++)
			check("tanh(atanh)", v[i], lk_tanh(lk_atanh(v[i])),
			      v[i], 64);
	}

	/* The float and long double variants exist and agree with the double
	 * one to their own precision.  They are one-line casts, so this is
	 * checking that they were declared and linked, not the arithmetic. */
	check("asinhf", 0.75, lk_asinhf(0.75f), (float)asinh(0.75), 1);
	check("asinhl", 0.75, (double)lk_asinhl(0.75L), asinh(0.75), 1);
}

int main(void)
{
	/* The tolerances.
	 *
	 * These are built on log(), log1p() and sqrt(), each of which costs a
	 * fraction of an ulp, and the range-reduction arithmetic costs a few
	 * more.  A handful of ulps is what a libm assembled this way delivers
	 * and is far below anything a calculator, a plotting library or a
	 * spreadsheet can tell apart.  The point of the number is that it is a
	 * CEILING that holds across the whole domain -- a wrong branch misses
	 * it by millions, not by threes.
	 */
	const double tol = 8.0;

	/* The looser ceiling, for the three functions that are bounded by
	 * exp() rather than by their own arithmetic.
	 *
	 * exp(x) here is exp2(x * 1/ln2), and that single multiplication
	 * rounds.  At x = 700 the discarded part is about 1e-13 OF AN
	 * EXPONENT, and an error in the exponent comes back as a relative
	 * error of the same size in the result -- a few hundred ulp.  A libm
	 * that cared would split ln2 into a head and a tail and reduce the
	 * argument against both, which is a rewrite of exp() and of nothing
	 * else; it is a known limit of this exp(), not of the functions
	 * measured here, and it is why sinh, cosh and expm1 all peak at the
	 * SAME error at the SAME argument.  Recorded rather than hidden: if
	 * exp() is ever reduced properly, these three drop to `tol' with no
	 * other change.
	 */
	const double exp_tol = 512.0;

	printf("asinh\n");
	sweep("asinh", lk_asinh, asinh, 1e-320, 1e300, tol);
	boundaries("asinh", lk_asinh, asinh, tol);

	printf("acosh\n");
	/* Domain is [1, inf).  Swept from just above 1 -- the near-1 branch
	 * is where the cancellation lives -- and separately over the bulk. */
	for (double d = 1e-15; d < 1.0; d *= 1.05)
		check("acosh", 1.0 + d, lk_acosh(1.0 + d), acosh(1.0 + d), tol);
	sweep("acosh", lk_acosh, acosh, 1.0000001, 1e300, tol);
	boundaries("acosh", lk_acosh, acosh, tol);

	printf("atanh\n");
	sweep("atanh", lk_atanh, atanh, 1e-320, 0.9999999, tol);
	/* Approaching the pole from below, where 1 - |x| cancels. */
	for (double d = 1e-15; d < 0.5; d *= 1.05)
		check("atanh", 1.0 - d, lk_atanh(1.0 - d), atanh(1.0 - d), tol);
	boundaries("atanh", lk_atanh, atanh, tol);

	/* The two primitives the inverse hyperbolics are built on.  If either
	 * is wrong the functions above inherit it, and a sweep of asinh would
	 * blame asinh.  log1p in particular carries the entire small-argument
	 * end of both asinh and atanh. */
	printf("log1p / expm1\n");
	sweep("log1p", lk_log1p, log1p, 1e-320, 1e300, tol);
	for (double d = 1e-15; d < 0.9; d *= 1.05)
		check("log1p", -d, lk_log1p(-d), log1p(-d), tol);
	sweep("expm1", lk_expm1, expm1, 1e-320, 700.0, exp_tol);

	printf("sinh / cosh / tanh\n");
	/* The forward three, swept over their whole domains.  They matter here
	 * for two reasons: the inverse ones are checked against them by
	 * identity above, so a wrong reference would prove nothing, and this
	 * sweep is what found that sinh and tanh were written as
	 * (e^x -+ e^-x)/2 and lost every significant digit near zero to the
	 * subtraction.  Both are through expm1 now; tanh meets `tol', and
	 * sinh is left at the exp() ceiling above for large arguments only. */
	sweep("sinh", lk_sinh, sinh, 1e-320, 700.0, exp_tol);
	sweep("cosh", lk_cosh, cosh, 1e-320, 700.0, exp_tol);
	sweep("tanh", lk_tanh, tanh, 1e-320, 19.0, tol);

	specials();

	/* ---- <fenv.h> ----
	 *
	 * A shallow test on purpose: what these can get wrong is not arithmetic
	 * but the MXCSR and x87 bit layouts, and a wrong layout does not return
	 * a wrong value -- LDMXCSR raises #GP and the process dies.  So simply
	 * CALLING each one on real hardware is most of the test, and the
	 * assertions below check the parts that can fail quietly.
	 *
	 * A big enough buffer for the libc's fenv_t without depending on the
	 * host's definition of it, which is a different struct. */
	printf("fenv\n");
	{
		unsigned char envbuf[512], envbuf2[512];

		lk_feclearexcept(FE_ALL_EXCEPT);
		if (lk_fetestexcept(FE_ALL_EXCEPT) != 0)
			fail("feclearexcept", "flags still set");

		lk_feraiseexcept(FE_DIVBYZERO);
		if (lk_fetestexcept(FE_DIVBYZERO) != FE_DIVBYZERO)
			fail("feraiseexcept", "FE_DIVBYZERO did not stick");
		if (lk_fetestexcept(FE_OVERFLOW) != 0)
			fail("feraiseexcept", "raised more than it was asked");

		/* feholdexcept is the one that faulted: it writes the mask
		 * field, so a wrong shift lands on MXCSR's rounding-control
		 * and reserved bits instead.  Checked against the register
		 * itself, since that is where the mistake shows. */
		if (lk_fegetenv(envbuf) != 0)
			fail("fegetenv", "returned non-zero");
		{
			unsigned int rc_before = host_mxcsr() & 0x6000u;

			if (lk_feholdexcept(envbuf2) != 0)
				fail("feholdexcept", "returned non-zero");
			if (lk_fetestexcept(FE_ALL_EXCEPT) != 0)
				fail("feholdexcept", "did not clear the flags");

			/* All six masks are MXCSR bits 7-12, i.e. 0x1F80. */
			if ((host_mxcsr() & 0x1F80u) != 0x1F80u)
				fail("feholdexcept",
				     "did not mask all six exceptions in MXCSR");
			/* ...and it must not have touched anything else. */
			if ((host_mxcsr() & 0x6000u) != rc_before)
				fail("feholdexcept",
				     "changed the SSE rounding mode");
			if ((host_mxcsr() & 0xFFFF0000u) != 0u)
				fail("feholdexcept",
				     "set reserved MXCSR bits");
		}

		/* ...and the environment it saved must come back. */
		lk_feraiseexcept(FE_INEXACT);
		if (lk_feupdateenv(envbuf2) != 0)
			fail("feupdateenv", "returned non-zero");
		if (lk_fetestexcept(FE_DIVBYZERO) != FE_DIVBYZERO)
			fail("feupdateenv", "lost the held flags");
		if (lk_fetestexcept(FE_INEXACT) != FE_INEXACT)
			fail("feupdateenv", "lost what was raised meanwhile");

		/* Rounding reaches both units; the SSE half is a second shift
		 * that can be wrong in the same way. */
		lk_feclearexcept(FE_ALL_EXCEPT);
		if (lk_fegetround() != FE_TONEAREST)
			fail("fegetround", "did not start at FE_TONEAREST");
		if (lk_fesetround(FE_UPWARD) != 0 ||
		    lk_fegetround() != FE_UPWARD)
			fail("fesetround", "FE_UPWARD did not read back");
		/* fegetround reads the x87 word, so the SSE half needs its own
		 * check: MXCSR rounding is bits 13-14, the same two-bit code
		 * three places up from the x87 one. */
		if ((host_mxcsr() & 0x6000u) !=
		    ((unsigned int)FE_UPWARD << 3))
			fail("fesetround", "did not set MXCSR rounding");
		{
			volatile double x = 1.5, y;

			y = x + 1e-30;
			if (!(y > 1.5))
				fail("fesetround", "FE_UPWARD did not round up (SSE)");
		}
		if (lk_fesetround(FE_TOWARDZERO) != 0 ||
		    lk_fegetround() != FE_TOWARDZERO)
			fail("fesetround", "FE_TOWARDZERO did not read back");
		if (lk_fesetround(999999) == 0)
			fail("fesetround", "accepted a value that is not a direction");

		lk_fesetround(FE_TONEAREST);
		if (lk_fesetenv(envbuf) != 0)
			fail("fesetenv", "returned non-zero");

		/* A fenv_t that was never filled in: must not fault. */
		memset(envbuf2, 0xFF, sizeof envbuf2);
		lk_fesetenv(envbuf2);
		lk_fesetround(FE_TONEAREST);
		lk_feclearexcept(FE_ALL_EXCEPT);
		checks++; /* reaching here at all is the assertion */
	}

	printf("\n%-14s %10s %10s %14s %s\n",
	       "function", "checks", "failures", "worst (ulp)", "at");
	for (int i = 0; i < ntally; i++)
		printf("%-14s %10ld %10ld %14.1f %- .6g\n",
		       tally[i].name, tally[i].checks, tally[i].fails,
		       tally[i].worst, tally[i].worst_at);

	printf("\n%ld comparisons, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
