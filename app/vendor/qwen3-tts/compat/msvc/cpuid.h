/* cpuid.h - GCC's leaf queries over the MSVC intrinsic. */
#ifndef QWEN_MSVC_CPUID_H
#define QWEN_MSVC_CPUID_H

#include <intrin.h>

static __inline int __get_cpuid_count(unsigned leaf, unsigned subleaf, unsigned *a, unsigned *b,
                                      unsigned *c, unsigned *d) {
    int regs[4];
    __cpuidex(regs, (int)leaf, (int)subleaf);
    *a = (unsigned)regs[0];
    *b = (unsigned)regs[1];
    *c = (unsigned)regs[2];
    *d = (unsigned)regs[3];
    return 1;
}

static __inline int __get_cpuid(unsigned leaf, unsigned *a, unsigned *b, unsigned *c, unsigned *d) {
    return __get_cpuid_count(leaf, 0, a, b, c, d);
}

#endif
