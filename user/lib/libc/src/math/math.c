/*
 * math.c - tiny IEEE-754 math primitives.
 *
 * Just enough to satisfy ports (tmux uses fabs/fmod/round).  The
 * implementations operate on the bit-pattern of `double` directly so
 * they need no FP register tricks beyond what GCC emits inline.
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

/* Absolute value ---------------------------------------------------------- */
double fabs(double x)
{
	return u2d(d2u(x) & ~(uint64_t)0x8000000000000000ULL);
}
float fabsf(float x)
{
	union {
		float f;
		uint32_t u;
	} v;
	v.f = x;
	v.u &= 0x7FFFFFFFu;
	return v.f;
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

/* Floor / Ceil / Round ---------------------------------------------------- */
double floor(double x)
{
	double t = trunc_d(x);
	if (x < 0 && t != x)
		t -= 1.0;
	return t;
}
double ceil(double x)
{
	double t = trunc_d(x);
	if (x > 0 && t != x)
		t += 1.0;
	return t;
}
double round(double x)
{
	return (x >= 0) ? floor(x + 0.5) : ceil(x - 0.5);
}
float floorf(float x)
{
	return (float)floor(x);
}
float ceilf(float x)
{
	return (float)ceil(x);
}
float roundf(float x)
{
	return (float)round(x);
}

/* fmod (IEEE-754 remainder, sign of x) ------------------------------------ */
double fmod(double x, double y)
{
	if (y == 0.0)
		return 0.0;
	double q = x / y;
	double t = trunc_d(q);
	return x - t * y;
}
float fmodf(float x, float y)
{
	return (float)fmod(x, y);
}

/* Square root via x86 SSE (sqrtsd is a single instruction) ---------------- */
double sqrt(double x)
{
	double r;
	__asm__ __volatile__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
	return r;
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

/* log2(x), the primitive the other logarithms are built from. */
double log2(double x)
{
	double r;
	__asm__("fld1\n\tfxch %%st(1)\n\tfyl2x" : "=t"(r) : "0"(x) : "st(1)");
	return r;
}

double log(double x)
{
	return log2(x) * 0.69314718055994530942; /* ln(2) */
}

double log10(double x)
{
	return log2(x) * 0.30102999566398119521; /* log10(2) */
}

/* log(1+x), accurate for tiny x where log(1+x) would lose everything to
 * cancellation.  fyl2xp1 exists precisely for this and is valid for
 * |x| < 1 - sqrt(2)/2; fall back outside that. */
double log1p(double x)
{
	double r;

	if (x < -0.29 || x > 0.29)
		return log(1.0 + x);
	__asm__("fld1\n\tfxch %%st(1)\n\tfyl2xp1" : "=t"(r) : "0"(x) : "st(1)");
	return r * 0.69314718055994530942;
}

/* 2^x, the primitive the exponentials are built from.  f2xm1 computes
 * 2^f - 1 for |f| <= 1, so the argument is split into integer and fractional
 * parts and recombined with fscale. */
double exp2(double x)
{
	double r;

	__asm__("fld %%st(0)\n\t"     /* x x            */
		"frndint\n\t"         /* i x            */
		"fsubr %%st,%%st(1)\n\t" /* i f  (f=x-i)  */
		"fxch %%st(1)\n\t"    /* f i            */
		"f2xm1\n\t"           /* 2^f-1 i        */
		"fld1\n\t"            /* 1 2^f-1 i      */
		"faddp\n\t"           /* 2^f i          */
		"fscale\n\t"          /* 2^f*2^i i      */
		"fstp %%st(1)"        /* result         */
		: "=t"(r)
		: "0"(x));
	return r;
}

double exp(double x)
{
	return exp2(x * 1.4426950408889634074); /* 1/ln(2) */
}

/* e^x - 1, for tiny x where exp(x)-1 would cancel to nothing. */
double expm1(double x)
{
	if (x > -0.5 && x < 0.5) {
		/* 2^y - 1 directly, which is what f2xm1 computes. */
		double r;
		double y = x * 1.4426950408889634074;
		if (y > -1.0 && y < 1.0) {
			__asm__("f2xm1" : "=t"(r) : "0"(y));
			return r;
		}
	}
	return exp(x) - 1.0;
}

double pow(double x, double y)
{
	/* The special cases are not decoration: pow(x,0) is 1 for every x
	 * including NaN, and log2 of a non-positive number is undefined. */
	if (y == 0.0)
		return 1.0;
	if (x == 0.0)
		return (y > 0.0) ? 0.0 : 1.0 / 0.0;
	if (x < 0.0) {
		/* Defined only for integral exponents; the sign follows the
		 * parity. */
		double iy = (y < 0) ? -y : y;
		if (iy != (double)(long long)iy)
			return 0.0 / 0.0; /* NaN */
		return (((long long)iy & 1) ? -1.0 : 1.0) * exp2(y * log2(-x));
	}
	return exp2(y * log2(x));
}

double sin(double x)
{
	double r;
	__asm__("fsin" : "=t"(r) : "0"(x));
	return r;
}

double cos(double x)
{
	double r;
	__asm__("fcos" : "=t"(r) : "0"(x));
	return r;
}

/* Both at once.
 *
 * A GNU extension rather than a standard function, and worth having as more
 * than a convenience wrapper: fsincos computes the pair in ONE instruction,
 * where calling sin() and cos() separately does the argument reduction and the
 * polynomial evaluation twice.  Software that rotates or draws circles asks
 * for it by name -- GTK's GL demo does, which is where its absence showed.
 *
 * fsincos leaves cos in ST(0) and sin in ST(1), which is what "=t" and "=u"
 * name.  Its argument domain is |x| < 2^63, the same as the fsin and fcos
 * above; outside that all three leave the operand untouched, which is a
 * property of this file's trigonometry generally rather than of this function.
 */
void sincos(double x, double *s, double *c)
{
	double sn, cs;

	__asm__("fsincos" : "=t"(cs), "=u"(sn) : "0"(x));
	if (s)
		*s = sn;
	if (c)
		*c = cs;
}

void sincosf(float x, float *s, float *c)
{
	double sn, cs;

	sincos((double)x, &sn, &cs);
	if (s)
		*s = (float)sn;
	if (c)
		*c = (float)cs;
}

void sincosl(long double x, long double *s, long double *c)
{
	long double sn, cs;

	__asm__("fsincos" : "=t"(cs), "=u"(sn) : "0"(x));
	if (s)
		*s = sn;
	if (c)
		*c = cs;
}

double tan(double x)
{
	double r, discard;
	/* fptan pushes 1.0 after the result, so the stack has to be unwound. */
	__asm__("fptan" : "=t"(discard), "=u"(r) : "0"(x));
	(void)discard;
	return r;
}

/* atan2 is the primitive: fpatan takes both arguments and gets the quadrant
 * right, which is the whole reason atan2 exists. */
double atan2(double y, double x)
{
	double r;
	__asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
	return r;
}

double atan(double x)
{
	return atan2(x, 1.0);
}

double asin(double x)
{
	/* asin(x) = atan2(x, sqrt(1-x^2)) — correct at |x| = 1, where the
	 * atan(x/sqrt(1-x^2)) form divides by zero. */
	return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x)
{
	return atan2(sqrt(1.0 - x * x), x);
}

double sinh(double x)
{
	double e = exp(x);
	return (e - 1.0 / e) * 0.5;
}

double cosh(double x)
{
	double e = exp(x);
	return (e + 1.0 / e) * 0.5;
}

double tanh(double x)
{
	double e;
	/* Saturate early: exp(2x) overflows long before tanh stops being +-1. */
	if (x > 20.0)
		return 1.0;
	if (x < -20.0)
		return -1.0;
	e = exp(2.0 * x);
	return (e - 1.0) / (e + 1.0);
}

/* hypot: sqrt(x^2 + y^2) WITHOUT overflowing on the intermediate square, which
 * is the only reason to call it rather than writing the formula out. */
double hypot(double x, double y)
{
	double t;

	x = fabs(x);
	y = fabs(y);
	if (x < y) {
		t = x;
		x = y;
		y = t;
	}
	if (x == 0.0)
		return 0.0;
	t = y / x;
	return x * sqrt(1.0 + t * t);
}

double cbrt(double x)
{
	if (x == 0.0)
		return 0.0;
	if (x < 0.0)
		return -exp2(log2(-x) / 3.0);
	return exp2(log2(x) / 3.0);
}

double trunc(double x)
{
	return (x < 0.0) ? ceil(x) : floor(x);
}

double copysign(double x, double y)
{
	return __builtin_copysign(x, y);
}

double fmin(double a, double b)
{
	if (a != a)
		return b; /* NaN loses, per the standard */
	if (b != b)
		return a;
	return a < b ? a : b;
}

double fmax(double a, double b)
{
	if (a != a)
		return b;
	if (b != b)
		return a;
	return a > b ? a : b;
}

double fdim(double a, double b)
{
	return (a > b) ? a - b : 0.0;
}

/* Quiet NaN.
 *
 * The argument is a payload: an implementation may encode it in the NaN's
 * significand so that a program can tell one NaN from another.  This one
 * ignores it and returns the default quiet NaN, which the standard permits
 * ("if the argument is not a valid n-char sequence ... the result is a quiet
 * NaN") and which is what every caller here actually wants -- the payload is
 * used by numerical debuggers, and nothing on this system reads it back. */
double nan(const char *tag)
{
	(void)tag;
	return __builtin_nan("");
}

float nanf(const char *tag)
{
	(void)tag;
	return __builtin_nanf("");
}

long double nanl(const char *tag)
{
	(void)tag;
	return __builtin_nanl("");
}

/* The double and float halves of the scaling family.
 *
 * All of them go through the long double primitives at the bottom of this
 * file, which read and write the exponent field directly.  Scaling by a power
 * of two is EXACT in the 80-bit format -- its exponent range is far wider than
 * either of these types -- so converting the result back rounds exactly once
 * and lands on the correctly rounded answer, subnormal results included.
 *
 * ldexp used to be `x * exp2((double) e)' and frexp used to recover the
 * exponent with floor(log2(fabs(x))).  Both were wrong at the ends of the
 * range, not merely slow: the intermediate 2^e overflows to infinity for e
 * above 1023 even when x * 2^e is perfectly representable, so
 * ldexp(5e-324, 1074) -- the smallest subnormal scaled up to exactly 1.0 --
 * returned infinity.  Reading the exponent field cannot overflow, because it
 * never forms 2^e as a value at all. */
double ldexp(double x, int e)
{
	return (double)ldexpl((long double)x, e);
}

float ldexpf(float x, int e)
{
	return (float)ldexpl((long double)x, e);
}

double frexp(double x, int *e)
{
	return (double)frexpl((long double)x, e);
}

float frexpf(float x, int *e)
{
	return (float)frexpl((long double)x, e);
}

/* scalbn is the same operation under the name C99 gives it for radix-2
 * systems, which this is; scalbln takes a long count.  Software asks for one
 * spelling or the other depending on its age -- HarfBuzz uses scalbnf. */
double scalbn(double x, int e)
{
	return ldexp(x, e);
}

float scalbnf(float x, int e)
{
	return ldexpf(x, e);
}

double scalbln(double x, long e)
{
	return (double)scalblnl((long double)x, e);
}

float scalblnf(float x, long e)
{
	return (float)scalblnl((long double)x, e);
}

double modf(double x, double *iptr)
{
	double i = trunc(x);

	if (iptr)
		*iptr = i;
	return x - i;
}

/* float entry points, for callers that use the f-suffixed names. */
float sqrtf(float x) { return (float)sqrt(x); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float asinf(float x) { return (float)asin(x); }
float acosf(float x) { return (float)acos(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float truncf(float x) { return (float)trunc(x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }

/* Round-to-nearest with the result converted to an integer type.
 *
 * These are not lround(round(x)): the standard says they round halfway cases
 * AWAY from zero, whereas the current rounding mode (which a cast follows)
 * rounds to even.  Building them on round(), which already rounds away from
 * zero, keeps that right. */
long lround(double x)
{
	return (long)round(x);
}

long long llround(double x)
{
	return (long long)round(x);
}

long lroundf(float x)
{
	return (long)round((double)x);
}

long long llroundf(float x)
{
	return (long long)round((double)x);
}

/* long double is x87 80-bit here; the value is rounded in that precision
 * before conversion so the extra range is not lost first. */
long double roundl(long double x)
{
	/* Away from zero on halfway cases, matching round(). */
	return (x < 0.0L) ? -(long double)floor((double)(-x) + 0.5)
			  : (long double)floor((double)x + 0.5);
}

long lroundl(long double x)
{
	return (long)roundl(x);
}

long long llroundl(long double x)
{
	return (long long)roundl(x);
}

long lrint(double x)
{
	return (long)x;
}

long long llrint(double x)
{
	return (long long)x;
}

double rint(double x)
{
	double r;
	__asm__("frndint" : "=t"(r) : "0"(x));
	return r;
}

double nearbyint(double x)
{
	return rint(x);
}

/* long double entry points.
 *
 * long double is the x87 80-bit format here, so sqrtl and fabsl use the x87
 * instructions directly rather than narrowing to double first — narrowing
 * would throw away the extra range and precision that is the only reason to
 * ask for a long double. The transcendentals do narrow: the x87 ones already
 * compute in 80-bit internally, so the accuracy lost is in the final rounding
 * only. */
long double sqrtl(long double x)
{
	long double r;
	__asm__("fsqrt" : "=t"(r) : "0"(x));
	return r;
}

long double fabsl(long double x)
{
	long double r;
	__asm__("fabs" : "=t"(r) : "0"(x));
	return r;
}

long double floorl(long double x) { return (long double)floor((double)x); }
long double ceill(long double x) { return (long double)ceil((double)x); }
long double truncl(long double x) { return (long double)trunc((double)x); }
long double fmodl(long double x, long double y)
{
	return (long double)fmod((double)x, (double)y);
}
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
long double hypotl(long double x, long double y)
{
	return (long double)hypot((double)x, (double)y);
}
long double copysignl(long double x, long double y)
{
	return __builtin_copysignl(x, y);
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

long double frexpl(long double x, int *e)
{
	union ldbits u = { .f = x };
	int ee = u.i.se & 0x7fff;
	int dummy;

	if (!e)
		e = &dummy; /* every path below writes it */

	if (ee == 0) {
		/* Zero, or subnormal.  A subnormal has no leading one to
		 * report an exponent against, so scale it into the normal
		 * range first and take the scaling back off the answer.  2^64
		 * is more than the width of the significand, so one step is
		 * always enough. */
		if (x != 0.0L) {
			x = frexpl(x * 0x1p64L, e);
			*e -= 64;
		} else {
			*e = 0;
		}
		return x;
	}
	if (ee == 0x7fff) {
		/* Infinity or NaN.  The standard leaves *e unspecified; zero
		 * is the least surprising thing to leave behind. */
		*e = 0;
		return x;
	}

	/* A normal value is 1.significand x 2^(ee - 16383), and the result
	 * wants 0.5 <= |m| < 1 -- so the exponent is one larger and the
	 * significand is that of a value in [0.5, 1), which is the biased
	 * exponent 16382. */
	*e = ee - 0x3ffe;
	u.i.se &= 0x8000;
	u.i.se |= 0x3ffe;
	return u.f;
}

long double ldexpl(long double x, int e)
{
	long double r;

	/* fscale multiplies by 2 raised to the truncated integer in ST(1), and
	 * is exact.  It needs no special cases: zero and infinity scale to
	 * themselves, a NaN propagates, and a result too large or too small for
	 * the format saturates to infinity or to zero the same way an ordinary
	 * multiplication would. */
	__asm__("fscale" : "=t"(r) : "0"(x), "u"((long double)e));
	return r;
}

/* Same operation under the name C99 gives it for radix-2 systems, which this
 * is.  Software asks for one or the other depending on its age. */
long double scalbnl(long double x, int e)
{
	return ldexpl(x, e);
}

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
