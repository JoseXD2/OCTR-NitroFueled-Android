/* SacredFire Android (AArch64): the few x86 intrinsics the native code uses, mapped to ARM
 * equivalents. Force-included into the port's own C/C++ sources (not into the guest's code,
 * which stays x86 and runs on Box64). */
#ifndef BB_ARM_COMPAT_H
#define BB_ARM_COMPAT_H
#if defined(__aarch64__)
#include <stdint.h>
/* rdtsc: the generic timer (cntvct_el0). Its frequency (cntfrq_el0, often 19.2-25 MHz) is
 * lower than a TSC's: the code only uses differences and calibrates against the clock. */
static inline __attribute__((always_inline)) uint64_t bb_cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
static inline __attribute__((always_inline)) uint64_t bb_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}
#ifndef __rdtsc
#define __rdtsc() bb_cntvct()
#endif
#define __builtin_ia32_pause() __asm__ volatile("yield" ::: "memory")
#define _mm_pause() __asm__ volatile("yield" ::: "memory")
#define _mm_lfence() __asm__ volatile("dmb ishld" ::: "memory")
#define _mm_mfence() __asm__ volatile("dmb ish" ::: "memory")
#define _mm_sfence() __asm__ volatile("dmb ishst" ::: "memory")
#endif
#endif
