/* AIOS v2 — Lua Math Shim (double-precision)
 * Compiled with RENDERER_CFLAGS (SSE2 enabled).
 * Only called from Lua task context where fxsave/fxrstor protects XMM state. */

#include <stddef.h>

/* Use GCC builtins for simple operations */
double sqrt(double x)  { return __builtin_sqrt(x); }
double fabs(double x)  { return __builtin_fabs(x); }
float  sqrtf(float x)  { return __builtin_sqrtf(x); }
float  fabsf(float x)  { return __builtin_fabsf(x); }

/* ── IEEE 754 bit manipulation helpers ────────────────── */

typedef union { double d; unsigned long long u; } double_bits;
typedef union { float f; unsigned int u; } float_bits;

static inline double make_double(unsigned long long bits) {
    double_bits db; db.u = bits; return db.d;
}

static inline unsigned long long get_bits(double d) {
    double_bits db; db.d = d; return db.u;
}

/* ── floor / ceil / trunc / round ─────────────────────── */

double floor(double x) {
    if (__builtin_isnan(x) || __builtin_isinf(x)) return x;
    long long i = (long long)x;
    double d = (double)i;
    if (d > x) d -= 1.0;
    return d;
}

double ceil(double x) {
    if (__builtin_isnan(x) || __builtin_isinf(x)) return x;
    long long i = (long long)x;
    double d = (double)i;
    if (d < x) d += 1.0;
    return d;
}

double trunc(double x) {
    if (__builtin_isnan(x) || __builtin_isinf(x)) return x;
    return (double)(long long)x;
}

double round(double x) {
    if (x >= 0.0) return floor(x + 0.5);
    else return ceil(x - 0.5);
}

float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x)  { return (float)ceil((double)x); }

/* ── fmod ─────────────────────────────────────────────── */

double fmod(double x, double y) {
    if (y == 0.0 || __builtin_isnan(x) || __builtin_isinf(x)) return __builtin_nan("");
    if (__builtin_isinf(y)) return x;
    double q = trunc(x / y);
    return x - q * y;
}

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

/* ── frexp / ldexp / scalbn / modf / ilogb ────────────── */

double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    if (__builtin_isnan(x) || __builtin_isinf(x)) { *exp = 0; return x; }
    unsigned long long bits = get_bits(x);
    int e = (int)((bits >> 52) & 0x7FF);
    if (e == 0) {
        /* Denormalized: multiply by 2^53 to normalize */
        x *= 9007199254740992.0; /* 2^53 */
        bits = get_bits(x);
        e = (int)((bits >> 52) & 0x7FF);
        *exp = e - 1023 - 52;
    } else {
        *exp = e - 1022;
    }
    /* Set exponent to -1 (biased: 1022) → result in [0.5, 1.0) */
    bits = (bits & 0x800FFFFFFFFFFFFFULL) | 0x3FE0000000000000ULL;
    return make_double(bits);
}

double ldexp(double x, int exp) {
    if (x == 0.0 || __builtin_isnan(x) || __builtin_isinf(x)) return x;
    /* Multiply by 2^exp using repeated doubling/halving */
    while (exp > 0) {
        if (exp >= 30) { x *= (double)(1 << 30); exp -= 30; }
        else { x *= (double)(1 << exp); break; }
    }
    while (exp < 0) {
        if (exp <= -30) { x /= (double)(1 << 30); exp += 30; }
        else { x /= (double)(1 << (-exp)); break; }
    }
    return x;
}

double scalbn(double x, int n) { return ldexp(x, n); }

double modf(double x, double *iptr) {
    double t = trunc(x);
    *iptr = t;
    return x - t;
}

int ilogb(double x) {
    if (x == 0.0) return -2147483647;  /* FP_ILOGB0 */
    if (__builtin_isnan(x)) return -2147483647;
    if (__builtin_isinf(x)) return 2147483647;
    int exp;
    frexp(x, &exp);
    return exp - 1;
}

/* ── fmax / fmin / copysign / remainder ───────────────── */

double fmax(double x, double y) {
    if (__builtin_isnan(x)) return y;
    if (__builtin_isnan(y)) return x;
    return x > y ? x : y;
}

double fmin(double x, double y) {
    if (__builtin_isnan(x)) return y;
    if (__builtin_isnan(y)) return x;
    return x < y ? x : y;
}

double copysign(double x, double y) {
    unsigned long long xb = get_bits(x);
    unsigned long long yb = get_bits(y);
    xb = (xb & 0x7FFFFFFFFFFFFFFFULL) | (yb & 0x8000000000000000ULL);
    return make_double(xb);
}

double remainder(double x, double y) {
    if (y == 0.0) return __builtin_nan("");
    double q = round(x / y);
    return x - q * y;
}

/* ── sin / cos (Chebyshev on [-pi, pi]) ───────────────── */

static const double PI       = 3.14159265358979323846;
static const double TWO_PI   = 6.28318530717958647692;
static const double HALF_PI  = 1.57079632679489661923;

/* Range reduce x to [-pi, pi] */
static double reduce_angle(double x) {
    if (x >= -PI && x <= PI) return x;
    x = fmod(x, TWO_PI);
    if (x > PI) x -= TWO_PI;
    else if (x < -PI) x += TWO_PI;
    return x;
}

/* Minimax polynomial for sin(x) on [-pi/2, pi/2], odd terms only
 * sin(x) ≈ x - x^3/6 + x^5/120 - x^7/5040 + x^9/362880 - x^11/39916800 */
static double sin_core(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0 +
           x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0 + x2 * 1.0/6227020800.0))))));
}

/* Minimax polynomial for cos(x) on [-pi/2, pi/2], even terms only */
static double cos_core(double x) {
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24.0 + x2 * (-1.0/720.0 +
           x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0 + x2 * 1.0/479001600.0)))));
}

double sin(double x) {
    if (__builtin_isnan(x) || __builtin_isinf(x)) return __builtin_nan("");
    x = reduce_angle(x);
    /* Reduce to [-pi/2, pi/2] */
    if (x > HALF_PI) return cos_core(x - HALF_PI);
    if (x < -HALF_PI) return -cos_core(x + HALF_PI);
    return sin_core(x);
}

double cos(double x) {
    if (__builtin_isnan(x) || __builtin_isinf(x)) return __builtin_nan("");
    x = reduce_angle(x);
    if (x > HALF_PI) return -sin_core(x - HALF_PI);
    if (x < -HALF_PI) return -sin_core(x + HALF_PI);
    return cos_core(x);
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return copysign(__builtin_huge_val(), sin(x));
    return sin(x) / c;
}

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }

/* ── Hyperbolic functions ─────────────────────────────── */

double exp(double x); /* forward declaration */

double sinh(double x) {
    if (fabs(x) < 1e-9) return x;
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x) {
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

/* ── log (natural logarithm) ──────────────────────────── */

double log(double x) {
    if (x < 0.0) return __builtin_nan("");
    if (x == 0.0) return -__builtin_huge_val();
    if (__builtin_isinf(x)) return x;
    if (__builtin_isnan(x)) return x;

    /* Decompose: x = m * 2^e where m in [0.5, 1.0) */
    int e;
    double m = frexp(x, &e);

    /* Shift to [sqrt(2)/2, sqrt(2)] for better convergence */
    if (m < 0.70710678118654752) {
        m *= 2.0;
        e--;
    }

    /* log(m) where m is near 1: use log(1+f) = f - f^2/2 + f^3/3 - ...
     * but with a minimax rational approximation for better convergence */
    double f = (m - 1.0) / (m + 1.0);
    double f2 = f * f;

    /* log(m) = 2*f*(1 + f^2/3 + f^4/5 + f^6/7 + f^8/9 + f^10/11) */
    double result = 2.0 * f * (1.0 + f2 * (1.0/3.0 + f2 * (1.0/5.0 +
                    f2 * (1.0/7.0 + f2 * (1.0/9.0 + f2 * (1.0/11.0 +
                    f2 * 1.0/13.0))))));

    /* log(x) = log(m * 2^e) = log(m) + e * log(2) */
    return result + (double)e * 0.69314718055994530942;
}

double log2(double x)  { return log(x) * 1.44269504088896340736; }  /* 1/ln(2) */
double log10(double x) { return log(x) * 0.43429448190325182765; }  /* 1/ln(10) */
float  logf(float x)   { return (float)log((double)x); }

/* ── exp (e^x) ────────────────────────────────────────── */

double exp(double x) {
    if (__builtin_isnan(x)) return x;
    if (x > 709.0) return __builtin_huge_val();
    if (x < -745.0) return 0.0;

    /* Reduce: x = k * ln(2) + r where |r| <= ln(2)/2
     * Then e^x = 2^k * e^r */
    double k_real = x * 1.44269504088896340736; /* x / ln(2) */
    int k = (int)(k_real >= 0 ? k_real + 0.5 : k_real - 0.5);
    double r = x - (double)k * 0.69314718055994530942;

    /* e^r using Taylor series (r is small, converges fast) */
    double r2 = r * r;
    double result = 1.0 + r * (1.0 + r * (0.5 + r * (1.0/6.0 + r * (1.0/24.0 +
                    r * (1.0/120.0 + r * (1.0/720.0 + r * (1.0/5040.0 +
                    r * (1.0/40320.0 + r * (1.0/362880.0 + r * 1.0/3628800.0)))))))));
    (void)r2;

    /* Multiply by 2^k */
    return ldexp(result, k);
}

double exp2(double x) { return exp(x * 0.69314718055994530942); }
float  expf(float x)  { return (float)exp((double)x); }

/* ── pow ──────────────────────────────────────────────── */

double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (base == 1.0) return 1.0;
    if (__builtin_isnan(base) || __builtin_isnan(exponent)) return __builtin_nan("");
    if (base == 0.0) {
        if (exponent > 0.0) return 0.0;
        return __builtin_huge_val();
    }

    /* Check for integer exponent (fast path) */
    if (exponent == (double)(int)exponent && fabs(exponent) < 1000.0) {
        int n = (int)exponent;
        if (n < 0) { base = 1.0 / base; n = -n; }
        double result = 1.0;
        double b = base;
        while (n > 0) {
            if (n & 1) result *= b;
            b *= b;
            n >>= 1;
        }
        return result;
    }

    /* General case: base^exp = exp(exp * log(base)) */
    if (base < 0.0) {
        /* Negative base with non-integer exponent → NaN */
        return __builtin_nan("");
    }
    return exp(exponent * log(base));
}

float powf(float b, float e) { return (float)pow((double)b, (double)e); }

/* ── Inverse trigonometric functions ──────────────────── */

/* atan(x) for |x| <= 1 using minimax polynomial */
static double atan_core(double x) {
    double x2 = x * x;
    /* Minimax: atan(x) ≈ x*(1 - x^2/3 + x^4/5 - x^6/7 + ...) */
    return x * (1.0 + x2 * (-1.0/3.0 + x2 * (1.0/5.0 + x2 * (-1.0/7.0 +
           x2 * (1.0/9.0 + x2 * (-1.0/11.0 + x2 * (1.0/13.0 +
           x2 * (-1.0/15.0 + x2 * (1.0/17.0 + x2 * (-1.0/19.0))))))))));
}

double atan(double x) {
    if (__builtin_isnan(x)) return x;
    if (__builtin_isinf(x)) return copysign(HALF_PI, x);
    if (fabs(x) <= 1.0) return atan_core(x);
    /* |x| > 1: use identity atan(x) = pi/2 - atan(1/x) */
    if (x > 0) return HALF_PI - atan_core(1.0 / x);
    return -HALF_PI - atan_core(1.0 / x);
}

double atan2(double y, double x) {
    if (__builtin_isnan(x) || __builtin_isnan(y)) return __builtin_nan("");
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + PI;
    if (x < 0.0 && y < 0.0) return atan(y / x) - PI;
    if (x == 0.0 && y > 0.0) return HALF_PI;
    if (x == 0.0 && y < 0.0) return -HALF_PI;
    return 0.0; /* x == 0 && y == 0 */
}

double asin(double x) {
    if (x < -1.0 || x > 1.0 || __builtin_isnan(x)) return __builtin_nan("");
    if (x == 1.0) return HALF_PI;
    if (x == -1.0) return -HALF_PI;
    /* asin(x) = atan(x / sqrt(1 - x*x)) */
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0 || __builtin_isnan(x)) return __builtin_nan("");
    return HALF_PI - asin(x);
}
