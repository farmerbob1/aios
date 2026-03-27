#ifndef _AIOS_MATH_H
#define _AIOS_MATH_H

/* IEEE 754 special values */
#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)

#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.43429448190325182765
#define M_SQRT2     1.41421356237309504880

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double base, double exponent);
double log(double x);
double log2(double x);
double log10(double x);
double exp(double x);
double exp2(double x);

double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double scalbn(double x, int n);
int    ilogb(double x);

double fmax(double x, double y);
double fmin(double x, double y);
double copysign(double x, double y);
double trunc(double x);
double round(double x);
double remainder(double x, double y);

/* QuickJS needs these */
long   lrint(double x);
long long llrint(double x);
double rint(double x);
double cbrt(double x);
double log1p(double x);
double expm1(double x);
double hypot(double x, double y);
double acosh(double x);
double asinh(double x);
double atanh(double x);
double fma(double x, double y, double z);
double nearbyint(double x);

float sinf(float x);
float cosf(float x);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float fmodf(float x, float y);
float powf(float base, float exponent);
float logf(float x);
float expf(float x);
float strtof(const char *nptr, char **endptr);
float atan2f(float y, float x);
float acosf(float x);
float tanf(float x);
float roundf(float x);
float truncf(float x);

#endif
