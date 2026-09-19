/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Runtime ISA selection, done once at context creation.
 *
 * The AVX-512 policy is deliberate. On a bandwidth-bound kernel the wider
 * vectors buy very little, and on Intel client parts the frequency licence
 * transition can leave AVX-512 slower than AVX2 in mixed workloads. On
 * Zen 4 and later there is no such penalty and 512-bit is a genuine win.
 * So AUTO picks AVX-512 only on AMD; anyone who wants it elsewhere can ask
 * for MBLUR_BACKEND_CPU_AVX512 explicitly and measure it with the bench.
 */
#include "../core/internal.h"

#if defined(MBLUR_X86)
#if defined(_MSC_VER)
#include <intrin.h>
static void mblur_cpuid(int out[4], int leaf, int sub)
{
    __cpuidex(out, leaf, sub);
}
static uint64_t mblur_xgetbv0(void)
{
    return _xgetbv(0);
}
#else
#include <cpuid.h>
static void mblur_cpuid(int out[4], int leaf, int sub)
{
    __cpuid_count(leaf, sub, out[0], out[1], out[2], out[3]);
}
static uint64_t mblur_xgetbv0(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}
#endif

typedef struct {
    int have_avx2;
    int have_avx512bw;
    int is_amd;
} mblur_cpu_info;

static mblur_cpu_info detect(void)
{
    mblur_cpu_info info = {0, 0, 0};
    int r[4] = {0, 0, 0, 0};

    mblur_cpuid(r, 0, 0);
    const int max_leaf = r[0];
    /* "AuthenticAMD" spelled across ebx, edx, ecx. */
    info.is_amd = (r[1] == 0x68747541 && r[3] == 0x69746E65 && r[2] == 0x444D4163);
    if (max_leaf < 7)
        return info;

    mblur_cpuid(r, 1, 0);
    const int osxsave = (r[2] >> 27) & 1;
    const int cpu_avx = (r[2] >> 28) & 1;
    if (!osxsave || !cpu_avx)
        return info;

    /* The OS must have enabled XMM+YMM state, and ZMM+opmask on top of that
     * for AVX-512, or the instructions fault however capable the silicon. */
    const uint64_t xcr0 = mblur_xgetbv0();
    const int ymm_ok = (xcr0 & 0x6) == 0x6;
    const int zmm_ok = (xcr0 & 0xE6) == 0xE6;
    if (!ymm_ok)
        return info;

    mblur_cpuid(r, 7, 0);
    info.have_avx2 = (r[1] >> 5) & 1;
    if (zmm_ok) {
        const int f = (r[1] >> 16) & 1;
        const int bw = (r[1] >> 30) & 1;
        info.have_avx512bw = f && bw;
    }
    return info;
}
#endif /* MBLUR_X86 */

mblur_backend mblur_cpu_best_backend(void)
{
#if defined(MBLUR_X86)
    const mblur_cpu_info info = detect();
    if (info.have_avx512bw && info.is_amd)
        return MBLUR_BACKEND_CPU_AVX512;
    if (info.have_avx2)
        return MBLUR_BACKEND_CPU_AVX2;
    /* Every x86-64 has SSE2 by definition of the ABI. */
    return MBLUR_BACKEND_CPU_SSE2;
#else
    return MBLUR_BACKEND_CPU_SCALAR;
#endif
}

int mblur_backend_available(mblur_backend b)
{
#if defined(MBLUR_X86)
    const mblur_cpu_info info = detect();
    switch (b) {
    case MBLUR_BACKEND_CPU_SCALAR:
        return 1;
    case MBLUR_BACKEND_CPU_SSE2:
        return 1;
    case MBLUR_BACKEND_CPU_AVX2:
        return info.have_avx2;
    case MBLUR_BACKEND_CPU_AVX512:
        return info.have_avx512bw;
    default:
        return 0;
    }
#else
    return b == MBLUR_BACKEND_CPU_SCALAR;
#endif
}

mblur_backend mblur_resolve_backend(mblur_backend requested)
{
    if (requested == MBLUR_BACKEND_AUTO)
        return mblur_cpu_best_backend();
    if (!mblur_backend_available(requested))
        return MBLUR_BACKEND_COUNT; /* caller reports the failure */
    return requested;
}

const mblur_kernels *mblur_kernels_for(mblur_backend b)
{
    switch (b) {
#if defined(MBLUR_X86)
    case MBLUR_BACKEND_CPU_SSE2:
        return &mblur_kernels_sse2;
    case MBLUR_BACKEND_CPU_AVX2:
        return &mblur_kernels_avx2;
    case MBLUR_BACKEND_CPU_AVX512:
        return &mblur_kernels_avx512;
#endif
    case MBLUR_BACKEND_CPU_SCALAR:
        return &mblur_kernels_scalar;
    default:
        return NULL;
    }
}
