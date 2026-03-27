/* AIOS QuickJS Math Shim — SSE2 enabled
 * Additional math functions needed by QuickJS beyond what Lua's math shim provides.
 * Compiled with RENDERER_CFLAGS (SSE2) — only called from task context
 * where fxsave/fxrstor protects XMM registers. */

#include <math.h>

/* ── lrint / llrint — round to nearest integer ──────── */

long lrint(double x) {
    return (long)round(x);
}

long long llrint(double x) {
    return (long long)round(x);
}

double rint(double x) {
    return round(x);
}

double nearbyint(double x) {
    return round(x);
}

/* ── cbrt — cube root ───────────────────────────────── */

double cbrt(double x) {
    if (x == 0.0 || isnan(x) || isinf(x)) return x;
    int neg = (x < 0.0);
    if (neg) x = -x;
    double r = pow(x, 1.0 / 3.0);
    if (neg) r = -r;
    return r;
}

/* ── log1p — log(1 + x), accurate for small x ──────── */

double log1p(double x) {
    if (fabs(x) < 1e-8) {
        /* Taylor: x - x^2/2 + x^3/3 - ... */
        return x - x * x * 0.5;
    }
    return log(1.0 + x);
}

/* ── expm1 — exp(x) - 1, accurate for small x ──────── */

double expm1(double x) {
    if (fabs(x) < 1e-8) {
        /* Taylor: x + x^2/2 + x^3/6 + ... */
        return x + x * x * 0.5;
    }
    return exp(x) - 1.0;
}

/* ── hypot — sqrt(x*x + y*y) with overflow protection ─ */

double hypot(double x, double y) {
    x = fabs(x);
    y = fabs(y);
    if (x == 0.0) return y;
    if (y == 0.0) return x;
    if (isinf(x) || isinf(y)) return INFINITY;
    /* Scale to avoid overflow */
    double max = (x > y) ? x : y;
    double min = (x > y) ? y : x;
    double r = min / max;
    return max * sqrt(1.0 + r * r);
}

/* ── acosh / asinh / atanh — inverse hyperbolics ────── */

double acosh(double x) {
    if (x < 1.0) return NAN;
    return log(x + sqrt(x * x - 1.0));
}

double asinh(double x) {
    if (fabs(x) < 1e-8) return x;
    int neg = (x < 0.0);
    if (neg) x = -x;
    double r = log(x + sqrt(x * x + 1.0));
    return neg ? -r : r;
}

double atanh(double x) {
    if (fabs(x) >= 1.0) {
        if (x == 1.0) return INFINITY;
        if (x == -1.0) return -INFINITY;
        return NAN;
    }
    return 0.5 * log((1.0 + x) / (1.0 - x));
}

/* ── fma — fused multiply-add (non-fused fallback) ──── */

double fma(double x, double y, double z) {
    return x * y + z;
}
