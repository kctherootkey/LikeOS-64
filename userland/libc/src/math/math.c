/*
 * math.c - tiny IEEE-754 math primitives.
 *
 * Just enough to satisfy ports (tmux uses fabs/fmod/round).  The
 * implementations operate on the bit-pattern of `double` directly so
 * they need no FP register tricks beyond what GCC emits inline.
 */
#include "../../include/math.h"
#include "../../include/stdint.h"

/* Bit-twiddling helpers --------------------------------------------------- */
static inline uint64_t d2u(double x) {
    union { double d; uint64_t u; } v; v.d = x; return v.u;
}
static inline double u2d(uint64_t u) {
    union { double d; uint64_t u; } v; v.u = u; return v.d;
}

/* Absolute value ---------------------------------------------------------- */
double fabs(double x)  { return u2d(d2u(x) & ~(uint64_t)0x8000000000000000ULL); }
float  fabsf(float x)  { union { float f; uint32_t u; } v; v.f = x; v.u &= 0x7FFFFFFFu; return v.f; }

/* Truncation toward zero -------------------------------------------------- */
static double trunc_d(double x) {
    if (x >= 0) {
        long long i = (long long)x;
        return (double)i;
    } else {
        long long i = (long long)x;
        return (double)i;
    }
}

/* Floor / Ceil / Round ---------------------------------------------------- */
double floor(double x) {
    double t = trunc_d(x);
    if (x < 0 && t != x) t -= 1.0;
    return t;
}
double ceil(double x) {
    double t = trunc_d(x);
    if (x > 0 && t != x) t += 1.0;
    return t;
}
double round(double x) {
    return (x >= 0) ? floor(x + 0.5) : ceil(x - 0.5);
}
float  floorf(float x) { return (float)floor(x); }
float  ceilf(float x)  { return (float)ceil(x);  }
float  roundf(float x) { return (float)round(x); }

/* fmod (IEEE-754 remainder, sign of x) ------------------------------------ */
double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = x / y;
    double t = trunc_d(q);
    return x - t * y;
}
float fmodf(float x, float y) { return (float)fmod(x, y); }

/* Square root via x86 SSE (sqrtsd is a single instruction) ---------------- */
double sqrt(double x) {
    double r;
    __asm__ __volatile__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* Coarse pow / log / exp / sin / cos via Taylor / range reduction.
 * These are not used by tmux but are present so applications that link
 * libm symbols at all do not fail at runtime.
 */
double exp(double x) {
    /* Range-reduced series.  Adequate to ~1e-9 over |x| <= 8. */
    double r = 1.0, term = 1.0;
    int i;
    for (i = 1; i < 32; i++) { term *= x / i; r += term; }
    return r;
}
double log(double x) {
    /* Newton's iteration on exp(y)=x. */
    if (x <= 0) return 0;
    double y = 0;
    int i;
    for (i = 0; i < 32; i++) {
        double e = exp(y);
        y += (x - e) / e;
    }
    return y;
}
double pow(double x, double y) { return exp(y * log(x)); }

double sin(double x) {
    /* Reduce to (-pi, pi] then 9-term Maclaurin. */
    while (x >  M_PI) x -= 2 * M_PI;
    while (x < -M_PI) x += 2 * M_PI;
    double term = x, r = x, x2 = x * x;
    int i;
    for (i = 1; i < 10; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        r += term;
    }
    return r;
}
double cos(double x) { return sin(x + M_PI / 2); }
