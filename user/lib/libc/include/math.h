/*
 * math.h - minimal C99 math.h subset.
 *
 * Only the entry points used by ported applications (currently tmux) are
 * declared.  Implementations live in src/math/math.c.
 */
#ifndef _MATH_H
#define _MATH_H

#define HUGE_VAL      __builtin_huge_val()
#define HUGE_VALF     __builtin_huge_valf()
#define INFINITY      __builtin_inff()
#define NAN           __builtin_nanf("")

#define M_PI          3.14159265358979323846
#define M_E           2.7182818284590452354

#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)

#ifdef __cplusplus
extern "C" {
#endif

double fabs(double x);
double fmod(double x, double y);
double round(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double x, double y);
double log(double x);
double exp(double x);
double sin(double x);
double cos(double x);

float fabsf(float x);
float fmodf(float x, float y);
float roundf(float x);
float floorf(float x);
float ceilf(float x);

#ifdef __cplusplus
}
#endif

/* Added for the X.Org port; implemented on the x87 unit in
 * src/math/math.c.  See the comment there on why not series expansions. */
double log2(double x);
double log10(double x);
double log1p(double x);
double exp2(double x);
double expm1(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double cbrt(double x);
double trunc(double x);
double copysign(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
double fdim(double a, double b);
double ldexp(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *iptr);
float sqrtf(float x);
float expf(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float a, float b);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atanf(float x);
float atan2f(float a, float b);
float asinf(float x);
float acosf(float x);
float hypotf(float a, float b);
float truncf(float x);
float copysignf(float a, float b);
float fminf(float a, float b);
float fmaxf(float a, float b);

/* Round to nearest, halfway away from zero, converting to an integer type. */
long        lround(double x);
long long   llround(double x);
long        lroundf(float x);
long long   llroundf(float x);
long double roundl(long double x);
long        lroundl(long double x);
long long   llroundl(long double x);
long        lrint(double x);
long long   llrint(double x);
double      rint(double x);
double      nearbyint(double x);

/* long double variants.  sqrtl/fabsl/copysignl keep the full 80-bit format;
 * the rest compute in double, which is the precision their x87 primitives
 * deliver anyway. */
long double sqrtl(long double x);
long double fabsl(long double x);
long double floorl(long double x);
long double ceill(long double x);
long double truncl(long double x);
long double fmodl(long double x, long double y);
long double expl(long double x);
long double logl(long double x);
long double log2l(long double x);
long double log10l(long double x);
long double powl(long double x, long double y);
long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double atanl(long double x);
long double atan2l(long double y, long double x);
long double hypotl(long double x, long double y);
long double copysignl(long double x, long double y);

#endif /* _MATH_H */
