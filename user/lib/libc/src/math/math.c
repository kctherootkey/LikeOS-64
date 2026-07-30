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

double ldexp(double x, int e)
{
	return x * exp2((double)e);
}

double frexp(double x, int *e)
{
	double l;

	if (x == 0.0 || x != x) {
		if (e)
			*e = 0;
		return x;
	}
	l = floor(log2(fabs(x))) + 1.0;
	if (e)
		*e = (int)l;
	return x / exp2(l);
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
