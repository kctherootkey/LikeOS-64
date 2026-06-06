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

#endif /* _MATH_H */
