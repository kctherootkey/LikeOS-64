/*
 * math.c - tiny IEEE-754 math primitives.
 *
 * Just enough to satisfy ports (tmux uses fabs/fmod/round).  The
 * implementations operate on the bit-pattern of `double` directly so
 * they need no FP register tricks beyond what GCC emits inline.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <math.h>
#include <stdint.h>

/* Bit-twiddling helpers --------------------------------------------------- */
static inline uint64_t d2u(double x)
{
	union {
		double d;
		uint64_t u;
	} v;
	v.d = x;
	return v.u;
}
static inline double u2d(uint64_t u)
{
	union {
		double d;
		uint64_t u;
	} v;
	v.u = u;
	return v.d;
}

/* Truncation toward zero -------------------------------------------------- */
static double trunc_d(double x)
{
	if (x >= 0) {
		long long i = (long long)x;
		return (double)i;
	} else {
		long long i = (long long)x;
		return (double)i;
	}
}

/* Transcendentals.
 *
 * These use the x87 unit rather than series expansions.  The hardware
 * instructions (fyl2x, f2xm1, fpatan, fsin/fcos) are accurate to about one ulp
 * over their whole domain, where the Taylor series they replace were good to
 * ~1e-9 only for small arguments and drifted badly outside that — log() in
 * particular ran 32 Newton steps, each evaluating a 32-term exp().
 *
 * x87 is available unconditionally on x86-64, and using it keeps this file
 * small enough to be obviously correct.
 */

void sincosl(long double x, long double *s, long double *c)
{
	long double sn, cs;

	__asm__("fsincos" : "=t"(cs), "=u"(sn) : "0"(x));
	if (s)
		*s = sn;
	if (c)
		*c = cs;
}

/* The inverse hyperbolics.
 *
 * Each has a closed form -- asinh(x) = log(x + sqrt(x*x + 1)) and its
 * relatives -- and each closed form is unusable over part of its own domain.
 * Two things go wrong.  For a large argument the x*x overflows while the
 * ANSWER is a perfectly ordinary number near log(2x).  For a small one the
 * argument of the logarithm is 1 plus something tiny, and forming that sum
 * throws away every significant digit of the tiny part before log() ever sees
 * it: asinh(1e-10) comes out as exactly 0 rather than 1e-10.
 *
 * So each is written in ranges, with log1p() carrying the small end -- it
 * exists for exactly this -- and the log(2x) asymptote carrying the large one.
 * The algebra in the middle branches is the closed form rearranged to keep the
 * cancelling subtraction out of it; it is the classical fdlibm arrangement.
 *
 * The cutoffs are powers of two so they are exact: 2^-28 is where x*x falls
 * below the last bit of 1.0, and 2^28 is where x*x nears the top of the range
 * in which the middle branch is worth its extra arithmetic.
 */
#define LIKEOS_LN2 0.69314718055994530942

double scalbln(double x, long e)
{
	return (double)scalblnl((long double)x, e);
}

float scalblnf(float x, long e)
{
	return (float)scalblnl((long double)x, e);
}

/* float entry points, for callers that use the f-suffixed names. */
long double expl(long double x) { return (long double)exp((double)x); }
long double logl(long double x) { return (long double)log((double)x); }
long double log2l(long double x) { return (long double)log2((double)x); }
long double log10l(long double x) { return (long double)log10((double)x); }
long double powl(long double x, long double y)
{
	return (long double)pow((double)x, (double)y);
}
long double sinl(long double x) { return (long double)sin((double)x); }
long double cosl(long double x) { return (long double)cos((double)x); }
long double tanl(long double x) { return (long double)tan((double)x); }
long double atanl(long double x) { return (long double)atan((double)x); }
long double atan2l(long double y, long double x)
{
	return (long double)atan2((double)y, (double)x);
}
long double cbrtl(long double x) { return (long double)cbrt((double)x); }
long double sinhl(long double x) { return (long double)sinh((double)x); }
long double coshl(long double x) { return (long double)cosh((double)x); }
long double tanhl(long double x) { return (long double)tanh((double)x); }
long double asinhl(long double x) { return (long double)asinh((double)x); }
long double acoshl(long double x) { return (long double)acosh((double)x); }
long double atanhl(long double x) { return (long double)atanh((double)x); }
long double hypotl(long double x, long double y)
{
	return (long double)hypot((double)x, (double)y);
}
/* frexpl / ldexpl — taking a long double apart and putting it back together.
 *
 * These are how software that formats floating point by hand gets at the
 * exponent: gnulib's printf implementation, which GLib carries, refuses to
 * configure without them ("frexpl() is missing or broken beyond repair").
 *
 * Deliberately NOT written in terms of log2l and powl, the way frexp above is
 * written in terms of log2 and exp2.  That approach goes through a
 * transcendental to recover a value that is sitting in the exponent field, so
 * it is approximate where this operation is exact -- and for a long double the
 * whole point is the extra range and precision.  These read the bits instead.
 *
 * The x87 80-bit format, which is what a long double is here:
 *
 *     bits 0..63    the significand, WITH its leading integer bit stored
 *                   explicitly (unlike float and double, which imply it)
 *     bits 64..78   the exponent, biased by 16383
 *     bit 79        the sign
 *     bits 80..127  padding, so the type is 16 bytes and stays aligned
 */
union ldbits {
	long double f;
	struct {
		uint64_t m;  /* significand */
		uint16_t se; /* sign in bit 15, biased exponent below it */
		uint16_t pad[3];
	} i;
};

long double scalblnl(long double x, long e)
{
	/* Clamped rather than truncated: a count this large has already
	 * overflowed or underflowed the format, and saturating keeps that
	 * answer instead of wrapping it into a small one. */
	if (e > 100000L)
		e = 100000L;
	else if (e < -100000L)
		e = -100000L;
	return ldexpl(x, (int)e);
}

/* ========================================================================
 * The rest of the C99 surface (7.12.6-7.12.14), added for the C++ runtime.
 *
 * libstdc++'s configure names every one of these in a single probe program,
 * and one missing declaration switches every std:: math wrapper off --
 * <cmath> then has no std::round, which is where ICU's build first stopped.
 * The float and long double variants delegate to double, the convention the
 * whole file already follows.
 * ======================================================================== */

/* float counterpart of u2d() at the top of the file. */
static inline float u2f_(uint32_t u)
{
	union {
		float f;
		uint32_t u;
	} v;
	v.u = u;
	return v.f;
}

double nexttoward(double x, long double y)
{
	/* The comparison happens at the wider type so a target between two
	 * doubles still says which way to step. */
	if ((long double)x == y)
		return (double)y;
	return nextafter(x, (y > (long double)x) ? u2d(0x7FF0000000000000ULL)
						 : u2d(0xFFF0000000000000ULL));
}

float nexttowardf(float x, long double y)
{
	if ((long double)x == y)
		return (float)y;
	return nextafterf(x, (y > (long double)x) ? u2f_(0x7F800000u)
						  : u2f_(0xFF800000u));
}

long double nexttowardl(long double x, long double y)
{
	return nextafterl(x, y);
}

long double fmal(long double x, long double y, long double z)
{
	return (long double)fma((double)x, (double)y, (double)z);
}

/* The error function pair (7.12.8.1-2).
 *
 * Two regimes, split where each method is comfortably inside its range:
 *
 *   |x| < 2.5   the Maclaurin series erf(x) = 2/sqrt(pi) * sum
 *               (-1)^n x^(2n+1) / (n! (2n+1)).  Alternating with factorial
 *               decay; at x = 2.5 the largest term is ~2^9 against an answer
 *               near 1, so under seven digits of cancellation -- fine in
 *               double.
 *
 *   |x| >= 2.5  the continued fraction erfc(x) = exp(-x^2)/sqrt(pi) *
 *               1/(x + (1/2)/(x + (2/2)/(x + (3/2)/(x + ...)))), evaluated
 *               with modified Lentz.  Converges in a few dozen terms and has
 *               no cancellation at all.
 *
 * erfc for small x comes from 1 - erf (harmless there: erf < 0.9996), and
 * erf for large x from 1 - erfc, where erfc is already tiny. */
static double erf_series(double x)
{
	double t = x; /* term n = 0 */
	double s = x;
	double x2 = x * x;
	for (int n = 1; n < 80; n++) {
		t *= -x2 / n;
		double add = t / (2 * n + 1);
		s += add;
		if (fabs(add) < 1e-20 * fabs(s))
			break;
	}
	return s * 1.1283791670955125739; /* 2/sqrt(pi) */
}

static double erfc_cf(double x) /* x >= 2.5 */
{
	/* Modified Lentz on the continued fraction above. */
	double tiny = 1e-300;
	double f = x, c = f, d = 0.0;
	for (int n = 1; n < 300; n++) {
		double a = n * 0.5;
		/* b = x for every level; a walks 1/2, 1, 3/2, ... */
		d = x + a * d;
		if (fabs(d) < tiny)
			d = tiny;
		c = x + a / c;
		if (fabs(c) < tiny)
			c = tiny;
		d = 1.0 / d;
		double delta = c * d;
		f *= delta;
		if (fabs(delta - 1.0) < 1e-17)
			break;
	}
	/* f now holds x + CF; erfc = exp(-x^2)/sqrt(pi) / f. */
	return exp(-x * x) * 0.5641895835477562869 / f;
}

double erf(double x)
{
	if (isnan(x))
		return x;
	double ax = fabs(x);
	double r;
	if (ax < 2.5)
		r = erf_series(ax);
	else if (ax < 40.0)
		r = 1.0 - erfc_cf(ax);
	else
		r = 1.0;
	return (x < 0) ? -r : r;
}

double erfc(double x)
{
	if (isnan(x))
		return x;
	if (x < 0)
		return 2.0 - erfc(-x);
	if (x < 2.5)
		return 1.0 - erf_series(x);
	if (x > 27.5)
		return 0.0; /* underflows: exp(-x^2) below the format */
	return erfc_cf(x);
}

float erfcf(float x) { return (float)erfc((double)x); }
long double erfl(long double x) { return (long double)erf((double)x); }
long double erfcl(long double x) { return (long double)erfc((double)x); }

/* The gamma functions (7.12.8.3-4), by Lanczos approximation (g = 7, the
 * standard nine-coefficient set), with the reflection formula carrying
 * arguments below one half.  Good to ~1e-13 relative over the real line,
 * which is the same neighbourhood the x87 transcendentals above live in.
 *
 * lgamma computes through the logarithm of the same approximation rather
 * than log(tgamma(x)), so it keeps working long after tgamma has overflowed
 * (tgamma overflows past x = 171.6; lgamma is finite to the end of the
 * format). */
static const double lanczos_g = 7.0;
static const double lanczos_c[9] = {
	0.99999999999980993,
	676.5203681218851,
	-1259.1392167224028,
	771.32342877765313,
	-176.61502916214059,
	12.507343278686905,
	-0.13857109526572012,
	9.9843695780195716e-6,
	1.5056327351493116e-7,
};

static double lanczos_sum(double x) /* x >= 0.5 */
{
	double a = lanczos_c[0];
	for (int i = 1; i < 9; i++)
		a += lanczos_c[i] / (x - 1.0 + i);
	return a;
}

double tgamma(double x)
{
	if (isnan(x))
		return x;
	if (x == 0.0)
		return copysign(u2d(0x7FF0000000000000ULL), x); /* pole */
	if (x < 0.0 && x == trunc(x))
		return u2d(0x7FF8000000000000ULL); /* negative integer: NaN */
	if (x < 0.5) {
		/* Reflection: gamma(x) = pi / (sin(pi x) gamma(1 - x)). */
		return 3.14159265358979323846 /
		       (sin(3.14159265358979323846 * x) * tgamma(1.0 - x));
	}
	double z = x - 1.0;
	double t = z + lanczos_g + 0.5;
	return 2.5066282746310002 /* sqrt(2 pi) */
	       * pow(t, z + 0.5) * exp(-t) * lanczos_sum(x);
}

double lgamma(double x)
{
	if (isnan(x))
		return x;
	if (x == trunc(x) && x <= 0.0)
		return u2d(0x7FF0000000000000ULL); /* poles: +inf */
	if (x < 0.5) {
		/* log|gamma(x)| = log(pi/|sin(pi x)|) - log|gamma(1 - x)| */
		return log(3.14159265358979323846 /
			   fabs(sin(3.14159265358979323846 * x))) -
		       lgamma(1.0 - x);
	}
	double z = x - 1.0;
	double t = z + lanczos_g + 0.5;
	return 0.91893853320467274178 /* log(sqrt(2 pi)) */
	       + (z + 0.5) * log(t) - t + log(lanczos_sum(x));
}

float tgammaf(float x) { return (float)tgamma((double)x); }
float lgammaf(float x) { return (float)lgamma((double)x); }
long double tgammal(long double x) { return (long double)tgamma((double)x); }
long double lgammal(long double x) { return (long double)lgamma((double)x); }

/* Width variants the probe wants and nothing here had yet: each one computes
 * in double, which represents every float exactly. */
long double exp2l(long double x) { return (long double)exp2((double)x); }
long double expm1l(long double x) { return (long double)expm1((double)x); }
long double log1pl(long double x) { return (long double)log1p((double)x); }

/* fmodl: fprem is exact and this is its purpose.  It reduces by at most 64
 * bits of quotient per step, so it is repeated until the C2 flag reports the
 * reduction complete.  (The double and float forms come from the LLVM libc
 * sources under llvm-libc/; their long double form wants a 128-bit division
 * from libgcc, which this library does not link.) */
long double fmodl(long double x, long double y)
{
	long double r = x;
	unsigned short sw;

	if (isnan(x) || isnan(y) || isinf(x) || y == 0.0L)
		return (x * y) / (x * y); /* NaN */
	if (isinf(y))
		return x;
	do {
		__asm__("fprem\n\tfnstsw %%ax"
			: "=t"(r), "=a"(sw)
			: "0"(r), "u"(y));
	} while (sw & 0x0400); /* C2: partial remainder, go again */
	return r;
}
