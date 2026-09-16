/*
 * hyperbolic.c - sinh, cosh, tanh, asinh, acosh and atanh.
 *
 * The LLVM libc sources under llvm-libc/ provide every other double and float
 * transcendental with correctly rounded results, the float forms of these six
 * included.  The double forms are not among them, so they are written here
 * the way fdlibm -- and hence every libm descended from it, glibc's included
 * -- writes them: as short identities over exp, expm1, log, log1p and sqrt.
 * With those primitives correctly rounded the identities reproduce what a
 * glibc-based browser computes for the same inputs, which is what web content
 * compares against.
 *
 * Each identity is chosen for the range where it does not cancel: expm1 near
 * zero rather than exp(x) - 1, log1p for arguments near one, and the reduced
 * forms for large |x| where the naive formula overflows before the result.
 *
 * Copyright (C) 2026 The LikeOS Project
 */
#include <math.h>
#include <stdint.h>

static const double ln2 = 0.69314718055994530942;

static inline double abs_d(double x)
{
	return x < 0 ? -x : x;
}

/* sinh(x) = sign(x) * (expm1(|x|) + expm1(|x|) / (expm1(|x|) + 1)) / 2 */
double sinh(double x)
{
	double h = x < 0 ? -0.5 : 0.5;
	double ax = abs_d(x);

	if (isnan(x) || isinf(x))
		return x + x;
	if (ax < 22.0) {
		if (ax < 0x1p-28)
			return x; /* below the rounding threshold: sinh(x) = x */
		double t = expm1(ax);
		if (ax < 1.0)
			return h * (2.0 * t - t * t / (t + 1.0));
		return h * (t + t / (t + 1.0));
	}
	if (ax < 709.7822265625) /* log(DBL_MAX) */
		return h * exp(ax);
	/* |x| in [log(DBL_MAX), overflow threshold]: exp(|x|/2)^2 / 2, so the
	 * intermediate does not overflow before the result would. */
	double w = exp(0.5 * ax);
	return (h * w) * w;
}

/* cosh(x) = (exp(|x|) + 1/exp(|x|)) / 2 */
double cosh(double x)
{
	double ax = abs_d(x);

	if (isnan(x))
		return x + x;
	if (isinf(x))
		return INFINITY;
	if (ax < 0.5 * ln2) {
		double t = expm1(ax);
		double w = 1.0 + t;
		if (ax < 0x1p-55)
			return w; /* cosh(tiny) = 1 */
		return 1.0 + (t * t) / (w + w);
	}
	if (ax < 22.0) {
		double t = exp(ax);
		return 0.5 * t + 0.5 / t;
	}
	if (ax < 709.7822265625)
		return 0.5 * exp(ax);
	double w = exp(0.5 * ax);
	return (0.5 * w) * w;
}

/* tanh(x) = sign(x) * (1 - 2/(expm1(2|x|) + 2)) */
double tanh(double x)
{
	double ax = abs_d(x);
	double z;

	if (isnan(x))
		return x + x;
	if (isinf(x))
		return x > 0 ? 1.0 : -1.0;
	if (ax < 22.0) {
		if (ax < 0x1p-55)
			return x; /* tanh(tiny) = tiny */
		if (ax >= 1.0) {
			double t = expm1(2.0 * ax);
			z = 1.0 - 2.0 / (t + 2.0);
		} else {
			double t = expm1(-2.0 * ax);
			z = -t / (t + 2.0);
		}
	} else {
		z = 1.0; /* |x| >= 22: 1 to within rounding */
	}
	return x < 0 ? -z : z;
}

/* asinh(x) = sign(x) * log(|x| + sqrt(x^2 + 1)) */
double asinh(double x)
{
	double ax = abs_d(x);
	double w;

	if (isnan(x) || isinf(x))
		return x + x;
	if (ax < 0x1p-28)
		return x; /* asinh(tiny) = tiny */
	if (ax > 0x1p28) {
		w = log(ax) + ln2; /* sqrt(x^2 + 1) = |x| here */
	} else if (ax > 2.0) {
		w = log(2.0 * ax + 1.0 / (sqrt(x * x + 1.0) + ax));
	} else {
		double t = x * x;
		w = log1p(ax + t / (1.0 + sqrt(1.0 + t)));
	}
	return x < 0 ? -w : w;
}

/* acosh(x) = log(x + sqrt(x^2 - 1)), x >= 1 */
double acosh(double x)
{
	if (isnan(x))
		return x + x;
	if (x < 1.0)
		return (x - x) / (x - x); /* NaN, domain */
	if (x == 1.0)
		return 0.0;
	if (x > 0x1p28) {
		if (isinf(x))
			return x;
		return log(x) + ln2;
	}
	if (x > 2.0)
		return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
	double t = x - 1.0;
	return log1p(t + sqrt(2.0 * t + t * t));
}

/* atanh(x) = sign(x) * log1p(2|x| / (1 - |x|)) / 2, |x| < 1 */
double atanh(double x)
{
	double ax = abs_d(x);
	double t;

	if (isnan(x))
		return x + x;
	if (ax > 1.0)
		return (x - x) / (x - x); /* NaN, domain */
	if (ax == 1.0)
		return x / 0.0; /* +-inf */
	if (ax < 0x1p-28)
		return x; /* atanh(tiny) = tiny */
	if (ax < 0.5) {
		t = ax + ax;
		t = 0.5 * log1p(t + t * ax / (1.0 - ax));
	} else {
		t = 0.5 * log1p((ax + ax) / (1.0 - ax));
	}
	return x < 0 ? -t : t;
}
