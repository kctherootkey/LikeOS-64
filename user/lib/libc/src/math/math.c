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

/* sinh and tanh, through expm1 rather than exp.
 *
 * Both are differences of two exponentials that approach each other as the
 * argument approaches zero, and writing them that way destroys the answer:
 * sinh(x) as (e^x - e^-x)/2 subtracts 1.0000000001 from 0.9999999999 and keeps
 * whatever survives, which for x = 1e-10 is eight correct digits out of
 * sixteen.  The error is in the SUBTRACTION, so no amount of accuracy in exp()
 * helps -- the digits are gone before it returns.
 *
 * expm1(x) computes e^x - 1 without ever forming e^x, so the small quantity
 * stays small and keeps its precision.  Rewriting each function in terms of
 * t = e^|x| - 1 is then exact algebra:
 *
 *   sinh = (t*t + 2t) / (2(t+1))     tanh = (e^2a - 1) / (e^2a + 1)
 *
 * and each is given in the two forms below because they round differently --
 * which of the two is used is the only thing the |x| < 1 test decides.
 *
 * Both take |x| and put the sign back at the end: they are odd functions, and
 * a branch on the sign is a second thing that can be wrong.
 */
double sinh(double x)
{
	double a = fabs(x);
	double t;

	/* Above this, e^-a has underflowed relative to e^a and sinh IS
	 * e^a / 2.  exp() overflows a little beyond it, which is correct:
	 * so does sinh. */
	if (a > 22.0)
		return copysign(0.5 * exp(a), x);

	t = expm1(a);
	if (a < 1.0)
		return copysign(0.5 * (2.0 * t - t * t / (t + 1.0)), x);
	return copysign(0.5 * (t + t / (t + 1.0)), x);
}

double cosh(double x)
{
	/* No cancellation here -- cosh is a SUM of the same two exponentials,
	 * and near zero both are near 1 and add to 2 without losing anything.
	 * So the direct form is the right one. */
	double e = exp(fabs(x));

	return (e + 1.0 / e) * 0.5;
}

double tanh(double x)
{
	double a = fabs(x);
	double t;

	/* Saturate early: e^2a overflows long before tanh stops being 1 to
	 * every bit of a double. */
	if (a > 22.0)
		return copysign(1.0, x);
	if (a < 0x1p-28)
		return x; /* tanh(x) == x down here, sign and all */

	if (a >= 1.0) {
		t = expm1(2.0 * a);
		return copysign(1.0 - 2.0 / (t + 2.0), x);
	}
	t = expm1(-2.0 * a);
	return copysign(-t / (t + 2.0), x);
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

double asinh(double x)
{
	double a = fabs(x);
	double r;

	if (a < 0x1p-28)
		return x; /* asinh(x) == x to the last bit down here */
	if (a > 0x1p28)
		r = log(a) + LIKEOS_LN2; /* a*a would overflow */
	else if (a > 2.0)
		r = log(2.0 * a + 1.0 / (sqrt(a * a + 1.0) + a));
	else
		r = log1p(a + a * a / (1.0 + sqrt(a * a + 1.0)));
	/* Odd function, and every branch above took |x|. */
	return copysign(r, x);
}

double acosh(double x)
{
	if (x < 1.0)
		return 0.0 / 0.0; /* NaN: acosh is undefined below 1 */
	if (x == 1.0)
		return 0.0;
	if (x > 0x1p28)
		return log(x) + LIKEOS_LN2;
	if (x > 2.0)
		return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
	/* Near 1, where x*x - 1 cancels: work in t = x - 1, which is exact. */
	{
		double t = x - 1.0;

		return log1p(t + sqrt(2.0 * t + t * t));
	}
}

double atanh(double x)
{
	double a = fabs(x);
	double r;

	if (a > 1.0)
		return 0.0 / 0.0; /* NaN: outside the domain */
	if (a == 1.0)
		return x / 0.0; /* the poles, with the sign of x */
	if (a < 0x1p-28)
		return x;
	if (a < 0.5)
		r = 0.5 * log1p(2.0 * a + 2.0 * a * a / (1.0 - a));
	else
		r = 0.5 * log1p(2.0 * a / (1.0 - a));
	return copysign(r, x);
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

float modff(float x, float *iptr)
{
	/* Not (float)modf(): the integral part must be computed and stored in
	 * FLOAT precision, or a float just below a large power of two could
	 * round the wrong way through the double round-trip. */
	float i = truncf(x);

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
float cbrtf(float x) { return (float)cbrt(x); }
float sinhf(float x) { return (float)sinh(x); }
float coshf(float x) { return (float)cosh(x); }
float tanhf(float x) { return (float)tanh(x); }
float asinhf(float x) { return (float)asinh(x); }
float acoshf(float x) { return (float)acosh(x); }
float atanhf(float x) { return (float)atanh(x); }
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

/* Exponent extraction (7.12.6.11, 7.12.6.5).
 *
 * Read from the bit pattern rather than computed with a logarithm: logb must
 * be exact, and a subnormal's exponent is below what the biased field can
 * say, so the mantissa is renormalised first with an exact power-of-two
 * multiply. */
double logb(double x)
{
	uint64_t u = d2u(x);
	int e = (int)((u >> 52) & 0x7FF);

	if (e == 0x7FF) {
		if (u << 12)
			return x; /* NaN propagates */
		return u2d(0x7FF0000000000000ULL); /* logb(+-inf) = +inf */
	}
	if (e == 0) {
		if ((u << 1) == 0) /* logb(+-0) = -inf, and it is exact */
			return u2d(0xFFF0000000000000ULL);
		/* Subnormal: renormalise, then correct for the scaling. */
		return logb(x * 0x1p64) - 64.0;
	}
	return (double)(e - 1023);
}

int ilogb(double x)
{
	uint64_t u = d2u(x);
	int e = (int)((u >> 52) & 0x7FF);

	if (e == 0x7FF)
		return (u << 12) ? FP_ILOGBNAN : 2147483647;
	if (e == 0) {
		if ((u << 1) == 0)
			return FP_ILOGB0;
		return ilogb(x * 0x1p64) - 64;
	}
	return e - 1023;
}

float logbf(float x) { return (float)logb((double)x); }
int ilogbf(float x)
{
	/* Through double, which represents every float exactly, so the
	 * special-value answers carry over unchanged. */
	return ilogb((double)x);
}
long double logbl(long double x) { return (long double)logb((double)x); }
int ilogbl(long double x) { return ilogb((double)x); }

/* Next representable value (7.12.11.3-4).
 *
 * Stepping the bit pattern by one IS the operation: IEEE-754 doubles of one
 * sign compare like their bit patterns, so +-1 in the integer view is the
 * adjacent value, subnormals and exponent boundaries included. */
double nextafter(double x, double y)
{
	if (isnan(x) || isnan(y))
		return x + y;
	if (x == y)
		return y; /* including the +0/-0 pair, per the standard */

	uint64_t u = d2u(x);
	if ((u << 1) == 0) {
		/* From zero, the first step is the smallest subnormal with
		 * the direction's sign. */
		return u2d((y > 0.0) ? 1ULL : 0x8000000000000001ULL);
	}
	/* Toward the target means away from zero when the signs of x and the
	 * direction agree, toward zero when they do not. */
	if ((x < y) == (x > 0.0))
		u++;
	else
		u--;
	return u2d(u);
}

float nextafterf(float x, float y)
{
	if (isnan(x) || isnan(y))
		return x + y;
	if (x == y)
		return y;

	union { float f; uint32_t u; } v;
	v.f = x;
	if ((v.u << 1) == 0)
		return (y > 0.0f) ? u2f_(1u) : u2f_(0x80000001u);
	if ((x < y) == (x > 0.0f))
		v.u++;
	else
		v.u--;
	return v.f;
}

long double nextafterl(long double x, long double y)
{
	/* long double computes in double here, like the rest of the file. */
	return (long double)nextafter((double)x, (double)y);
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

/* IEEE remainder (7.12.10.2-3), on the x87 unit.
 *
 * fprem1 is the instruction FOR this operation: it reduces by the quotient
 * rounded to nearest-even -- exactly the definition -- and it is exact, where
 * the naive x - rint(x/y)*y loses the answer entirely once x/y overflows the
 * mantissa.  The instruction reduces the exponent difference by at most 63
 * per issue and says "not done yet" in C2, hence the loop.
 *
 * remquo additionally reports the low three bits of that quotient, which is
 * precisely what condition bits C0, C3, C1 hold after the final reduction
 * (Q2, Q1, Q0 in the manual's naming). */
double remainder(double x, double y)
{
	if (isnan(x) || isnan(y))
		return x + y;
	if (isinf(x) || y == 0.0)
		return u2d(0x7FF8000000000000ULL); /* domain error: NaN */

	double r = x;
	unsigned short sw;
	do {
		__asm__ volatile("fprem1\n\tfnstsw %%ax"
				 : "=t"(r), "=a"(sw)
				 : "0"(r), "u"(y)
				 : "cc");
	} while (sw & 0x0400); /* C2: reduction incomplete */
	return r;
}

double remquo(double x, double y, int *quo)
{
	if (quo)
		*quo = 0;
	if (isnan(x) || isnan(y))
		return x + y;
	if (isinf(x) || y == 0.0)
		return u2d(0x7FF8000000000000ULL);

	double r = x;
	unsigned short sw;
	do {
		__asm__ volatile("fprem1\n\tfnstsw %%ax"
				 : "=t"(r), "=a"(sw)
				 : "0"(r), "u"(y)
				 : "cc");
	} while (sw & 0x0400);

	if (quo) {
		/* C0 (0x0100) = Q2, C3 (0x4000) = Q1, C1 (0x0200) = Q0. */
		int q = ((sw & 0x0100) ? 4 : 0) | ((sw & 0x4000) ? 2 : 0) |
			((sw & 0x0200) ? 1 : 0);
		/* The quotient's sign is the XOR of the operands' signs. */
		if ((d2u(x) ^ d2u(y)) & 0x8000000000000000ULL)
			q = -q;
		*quo = q;
	}
	return r;
}

float remainderf(float x, float y) { return (float)remainder(x, y); }
float remquof(float x, float y, int *quo)
{
	return (float)remquo((double)x, (double)y, quo);
}
long double remainderl(long double x, long double y)
{
	return (long double)remainder((double)x, (double)y);
}
long double remquol(long double x, long double y, int *quo)
{
	return (long double)remquo((double)x, (double)y, quo);
}

/* Fused multiply-add (7.12.13.1), in plain double arithmetic.
 *
 * NOT __float128: the soft-float routines that would drag in live in libgcc
 * with hidden visibility, so a libc.so referencing them cannot be linked
 * against at all -- every executable link fails with "hidden symbol
 * __addtf3 referenced by DSO".
 *
 * Instead the classical error-free transformations: Dekker's splitting makes
 * the product exact as a hi+lo pair (x*y == p + e, exactly), Knuth's two-sum
 * makes the first addition exact the same way, and only the final collapse
 * rounds.  That last step can double-round in the same sub-ulp sliver the
 * quad approach could; it is the file's usual trade -- an ulp-accurate
 * fma whose one job here (letting the C++ runtime and WebKit's std:: calls
 * resolve) it does exactly.
 *
 * The splitter overflows for |x| >= 2^970, so arguments that large (or
 * non-finite) take the naive path: at those magnitudes the product has
 * overflowed or the error term is beyond the format anyway. */
double fma(double x, double y, double z)
{
	if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
	    fabs(x) >= 0x1p970 || fabs(y) >= 0x1p970 || fabs(z) >= 0x1p970 ||
	    x == 0.0 || y == 0.0)
		return x * y + z;

	const double C = 0x1p27 + 1.0; /* Dekker's splitter for 53 bits */
	double t, xh, xl, yh, yl;

	t = C * x;
	xh = t - (t - x);
	xl = x - xh;
	t = C * y;
	yh = t - (t - y);
	yl = y - yh;

	double p = x * y; /* rounded product */
	double e = ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;
	/* x*y == p + e, exactly */

	double s = p + z; /* rounded first sum */
	double v = s - p;
	double err = (p - (s - v)) + (z - v);
	/* p + z == s + err, exactly */

	return s + (err + e);
}

float fmaf(float x, float y, float z)
{
	/* Exact: the float product fits a double's mantissa whole, and the
	 * one rounding to float happens at the end. */
	return (float)((double)x * (double)y + (double)z);
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

float erff(float x) { return (float)erf((double)x); }
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
float exp2f(float x) { return (float)exp2((double)x); }
float expm1f(float x) { return (float)expm1((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }
float fdimf(float a, float b) { return (float)fdim((double)a, (double)b); }
float rintf(float x) { return (float)rint((double)x); }
float nearbyintf(float x) { return (float)rint((double)x); }
long lrintf(float x) { return lrint((double)x); }
long long llrintf(float x) { return llrint((double)x); }

long double exp2l(long double x) { return (long double)exp2((double)x); }
long double expm1l(long double x) { return (long double)expm1((double)x); }
long double log1pl(long double x) { return (long double)log1p((double)x); }
long double fdiml(long double a, long double b)
{
	return (long double)fdim((double)a, (double)b);
}
long double rintl(long double x) { return (long double)rint((double)x); }
long double nearbyintl(long double x) { return (long double)rint((double)x); }
long lrintl(long double x) { return lrint((double)x); }
long long llrintl(long double x) { return llrint((double)x); }
long double fminl(long double a, long double b)
{
	return (long double)fmin((double)a, (double)b);
}
long double fmaxl(long double a, long double b)
{
	return (long double)fmax((double)a, (double)b);
}
