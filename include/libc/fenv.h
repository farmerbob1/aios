#ifndef _AIOS_FENV_H
#define _AIOS_FENV_H

/* Minimal fenv.h stub for QuickJS.
 * QuickJS includes fenv.h but doesn't call fesetround/fegetround. */

typedef unsigned int fenv_t;
typedef unsigned int fexcept_t;

#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3

static inline int fegetround(void) { return FE_TONEAREST; }
static inline int fesetround(int round) { (void)round; return 0; }

#endif
