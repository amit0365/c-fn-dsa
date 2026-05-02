/*
 * Standalone microbench for the phase-1 "recompute step" cost.
 *
 * Hypothetical phase-1 reduction: the lattice basis B = [[g, -f], [G, -F]]
 * is precomputed in FFT format (b00, b01, b10, b11) once at key load and
 * stored in flash, together with d00 (= g00, self-adjoint). What is NOT
 * stored is g01 and l10 — those would still be recomputed per signature:
 *
 *     g01 = b00 * adj(b10) + b01 * adj(b11)        (n FLR)
 *     l10 = g01 / d00                              (in place over g01)
 *
 * This bench measures the cost of that recompute in isolation, against
 * the cost of a full current fndsa_sign call, for logn=9 and logn=10.
 *
 * The recompute kernels mirror the SIMD branch of fpoly_gram_fft (the
 * same primitive the library would call) so the host ratio is fair —
 * it isolates the b00*adj(b10) + b01*adj(b11) term out of the 3-output
 * gram_fft, plus the elementwise divide by d00. NEON path on aarch64,
 * SSE2 on x86_64, plain doubles otherwise.
 *
 * The reported ratio = recompute_cost / sign_cost is a host-CPU filter
 * for the M35P question:
 *     ratio < 2%  → recompute is cheap; phase 1 viable without storing l10.
 *     ratio > 5%  → recompute is non-trivial; consider storing l10 too.
 *
 * Build (matches Makefile's library object flags — must be NEON-compatible):
 *     clang -W -Wextra -O2 -c -o bench_recompute.o bench_recompute.c
 *     clang -O2 -o bench_recompute bench_recompute.o \
 *         codec.o mq.o sha3.o sysrng.o util.o \
 *         kgen.o kgen_fxp.o kgen_gauss.o kgen_mp31.o kgen_ntru.o \
 *         kgen_poly.o kgen_zint31.o \
 *         sign.o sign_core.o sign_fpoly.o sign_fpr.o sign_sampler.o \
 *         vrfy.o
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "fndsa.h"
#include "sign_inner.h"
#include "inner.h"

#if FNDSA_NEON
#include <arm_neon.h>
#elif FNDSA_SSE2
#include <emmintrin.h>
#endif

/* ---------- timing primitive ---------- */

#if defined __aarch64__ && (defined __GNUC__ || defined __clang__) && !defined(__APPLE__)
static inline uint64_t cycles_now(void) {
    uint64_t x;
    __asm__ __volatile__ ("dsb sy\n\tmrs %0, pmccntr_el0" : "=r" (x) : : );
    return x;
}
#define TIME_UNIT "cycles"
#elif (defined __x86_64__ || defined __i386__) && !defined(__APPLE__)
#include <x86intrin.h>
__attribute__((target("sse2")))
static inline uint64_t cycles_now(void) {
    _mm_lfence();
    return __rdpmc(0x40000001);
}
#define TIME_UNIT "cycles"
#else
/* Fallback: nanoseconds (macOS host etc.). The ratio (recompute/sign) is
 * unitless and unaffected by the choice of clock. */
static inline uint64_t cycles_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#define TIME_UNIT "ns"
#endif

/* ---------- key-decode helpers (mirroring sign_core.c) ---------- */

static unsigned
nbits_for_logn(unsigned logn)
{
    switch (logn) {
    case 2: case 3: case 4: case 5: return 8;
    case 6: case 7:                 return 7;
    case 8: case 9:                 return 6;
    default:                        return 5;
    }
}

/* Same as sign_core.c's static basis_to_FFT helper. */
static void
build_basis_fft(unsigned logn,
    const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
    fpr *dst)
{
    size_t n = (size_t)1 << logn;
    fpr *b00 = dst;
    fpr *b01 = b00 + n;
    fpr *b10 = b01 + n;
    fpr *b11 = b10 + n;
    fpoly_set_small(logn, b01, f);
    fpoly_set_small(logn, b00, g);
    fpoly_set_small(logn, b11, F);
    fpoly_set_small(logn, b10, G);
    fpoly_FFT(logn, b01);
    fpoly_FFT(logn, b00);
    fpoly_FFT(logn, b11);
    fpoly_FFT(logn, b10);
    fpoly_neg(logn, b01);
    fpoly_neg(logn, b11);
}

/* ---------- recompute-step kernels ---------- */

/* g01 = b00*adj(b10) + b01*adj(b11)
 * FFT layout: real coeffs in [0..n/2-1], imag coeffs in [n/2..n-1].
 * Output buffer must be disjoint from inputs. Mirrors the g01-only
 * arithmetic from fpoly_gram_fft (sign_fpoly.c). */
#if FNDSA_NEON
static void
recompute_g01(unsigned logn, fpr *g01,
    const fpr *b00, const fpr *b01,
    const fpr *b10, const fpr *b11)
{
    size_t hn = (size_t)1 << (logn - 1);
    const float64_t *p00 = (const float64_t *)b00;
    const float64_t *p01 = (const float64_t *)b01;
    const float64_t *p10 = (const float64_t *)b10;
    const float64_t *p11 = (const float64_t *)b11;
    float64_t *p_out = (float64_t *)g01;
    for (size_t i = 0; i < hn; i += 2) {
        float64x2_t b00_re = vld1q_f64(p00 + i);
        float64x2_t b00_im = vld1q_f64(p00 + i + hn);
        float64x2_t b01_re = vld1q_f64(p01 + i);
        float64x2_t b01_im = vld1q_f64(p01 + i + hn);
        float64x2_t b10_re = vld1q_f64(p10 + i);
        float64x2_t b10_im = vld1q_f64(p10 + i + hn);
        float64x2_t b11_re = vld1q_f64(p11 + i);
        float64x2_t b11_im = vld1q_f64(p11 + i + hn);

        float64x2_t u_re = vaddq_f64(vmulq_f64(b00_re, b10_re),
                                     vmulq_f64(b00_im, b10_im));
        float64x2_t u_im = vsubq_f64(vmulq_f64(b00_im, b10_re),
                                     vmulq_f64(b00_re, b10_im));
        float64x2_t v_re = vaddq_f64(vmulq_f64(b01_re, b11_re),
                                     vmulq_f64(b01_im, b11_im));
        float64x2_t v_im = vsubq_f64(vmulq_f64(b01_im, b11_re),
                                     vmulq_f64(b01_re, b11_im));
        vst1q_f64(p_out + i,      vaddq_f64(u_re, v_re));
        vst1q_f64(p_out + i + hn, vaddq_f64(u_im, v_im));
    }
}

/* l10 = g01 / d00 in place. d00 is self-adjoint (n/2 reals).
 * NEON divides via vdivq_f64 (matches the library's f64_div on aarch64). */
static void
recompute_l10(unsigned logn, fpr *g01, const fpr *d00)
{
    size_t hn = (size_t)1 << (logn - 1);
    float64_t *p = (float64_t *)g01;
    const float64_t *pd = (const float64_t *)d00;
    for (size_t i = 0; i < hn; i += 2) {
        float64x2_t d_v = vld1q_f64(pd + i);
        float64x2_t re  = vld1q_f64(p + i);
        float64x2_t im  = vld1q_f64(p + i + hn);
        vst1q_f64(p + i,      vdivq_f64(re, d_v));
        vst1q_f64(p + i + hn, vdivq_f64(im, d_v));
    }
}

#elif FNDSA_SSE2
static void
recompute_g01(unsigned logn, fpr *g01,
    const fpr *b00, const fpr *b01,
    const fpr *b10, const fpr *b11)
{
    size_t hn = (size_t)1 << (logn - 1);
    const double *p00 = (const double *)b00;
    const double *p01 = (const double *)b01;
    const double *p10 = (const double *)b10;
    const double *p11 = (const double *)b11;
    double *p_out = (double *)g01;
    for (size_t i = 0; i < hn; i += 2) {
        __m128d b00_re = _mm_loadu_pd(p00 + i);
        __m128d b00_im = _mm_loadu_pd(p00 + i + hn);
        __m128d b01_re = _mm_loadu_pd(p01 + i);
        __m128d b01_im = _mm_loadu_pd(p01 + i + hn);
        __m128d b10_re = _mm_loadu_pd(p10 + i);
        __m128d b10_im = _mm_loadu_pd(p10 + i + hn);
        __m128d b11_re = _mm_loadu_pd(p11 + i);
        __m128d b11_im = _mm_loadu_pd(p11 + i + hn);

        __m128d u_re = _mm_add_pd(_mm_mul_pd(b00_re, b10_re),
                                  _mm_mul_pd(b00_im, b10_im));
        __m128d u_im = _mm_sub_pd(_mm_mul_pd(b00_im, b10_re),
                                  _mm_mul_pd(b00_re, b10_im));
        __m128d v_re = _mm_add_pd(_mm_mul_pd(b01_re, b11_re),
                                  _mm_mul_pd(b01_im, b11_im));
        __m128d v_im = _mm_sub_pd(_mm_mul_pd(b01_im, b11_re),
                                  _mm_mul_pd(b01_re, b11_im));
        _mm_storeu_pd(p_out + i,      _mm_add_pd(u_re, v_re));
        _mm_storeu_pd(p_out + i + hn, _mm_add_pd(u_im, v_im));
    }
}

static void
recompute_l10(unsigned logn, fpr *g01, const fpr *d00)
{
    size_t hn = (size_t)1 << (logn - 1);
    double *p = (double *)g01;
    const double *pd = (const double *)d00;
    for (size_t i = 0; i < hn; i += 2) {
        __m128d d_v = _mm_loadu_pd(pd + i);
        __m128d re  = _mm_loadu_pd(p + i);
        __m128d im  = _mm_loadu_pd(p + i + hn);
        _mm_storeu_pd(p + i,      _mm_div_pd(re, d_v));
        _mm_storeu_pd(p + i + hn, _mm_div_pd(im, d_v));
    }
}

#else
/* Scalar fallback using native doubles (works as long as fpr representation
 * is bit-equivalent to IEEE-754 binary64, which it is). */
static void
recompute_g01(unsigned logn, fpr *g01,
    const fpr *b00, const fpr *b01,
    const fpr *b10, const fpr *b11)
{
    size_t hn = (size_t)1 << (logn - 1);
    const double *p00 = (const double *)b00;
    const double *p01 = (const double *)b01;
    const double *p10 = (const double *)b10;
    const double *p11 = (const double *)b11;
    double *po = (double *)g01;
    for (size_t i = 0; i < hn; i ++) {
        double a_re = p00[i], a_im = p00[i + hn];
        double c_re = p01[i], c_im = p01[i + hn];
        double d_re = p10[i], d_im = p10[i + hn];
        double e_re = p11[i], e_im = p11[i + hn];
        double u_re = a_re * d_re + a_im * d_im;
        double u_im = a_im * d_re - a_re * d_im;
        double v_re = c_re * e_re + c_im * e_im;
        double v_im = c_im * e_re - c_re * e_im;
        po[i]      = u_re + v_re;
        po[i + hn] = u_im + v_im;
    }
}

static void
recompute_l10(unsigned logn, fpr *g01, const fpr *d00)
{
    size_t hn = (size_t)1 << (logn - 1);
    double *p = (double *)g01;
    const double *pd = (const double *)d00;
    for (size_t i = 0; i < hn; i ++) {
        double inv = 1.0 / pd[i];
        p[i]      *= inv;
        p[i + hn] *= inv;
    }
}
#endif

/* ---------- comparison helpers ---------- */

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static double
median_per_iter(uint64_t *tt, int samples, int inner_iters, int warmup)
{
    qsort(tt + warmup, samples - warmup, sizeof *tt, cmp_u64);
    return (double)tt[warmup + (samples - warmup) / 2] / (double)inner_iters;
}

/* ---------- per-logn driver ---------- */

static void
bench_logn(unsigned logn)
{
    size_t n = (size_t)1 << logn;
    size_t hn = n >> 1;
    unsigned nbits = nbits_for_logn(logn);
    size_t flen = (nbits << logn) >> 3;

    /* 1. Generate a key. */
    uint8_t *sk = malloc(FNDSA_SIGN_KEY_SIZE(10));
    uint8_t *vk = malloc(FNDSA_VRFY_KEY_SIZE(10));
    if (!sk || !vk) { fprintf(stderr, "alloc fail\n"); exit(1); }
    uint8_t kseed[32];
    for (size_t i = 0; i < sizeof kseed; i ++) kseed[i] = (uint8_t)(i * 17u + logn);
    fndsa_keygen_seeded(logn, kseed, sizeof kseed, sk, vk);

    /* 2. Decode (f, g) and read F, G; build FFT-form basis once. */
    int8_t *f = calloc(n, 1);
    int8_t *g = calloc(n, 1);
    if (!f || !g) { fprintf(stderr, "alloc fail\n"); exit(1); }
    (void)trim_i8_decode(logn, sk + 1,        f, nbits);
    (void)trim_i8_decode(logn, sk + 1 + flen, g, nbits);
    int8_t *F = (int8_t *)(sk + 1 + (flen << 1));
    int8_t *G = (int8_t *)(sk + 1 + (flen << 1) + flen);

    fpr *basis  = aligned_alloc(64, 4 * n * sizeof(fpr));
    fpr *d00    = aligned_alloc(64,    hn * sizeof(fpr));
    fpr *g01    = aligned_alloc(64,     n * sizeof(fpr));
    fpr *scratch = aligned_alloc(64, 4 * n * sizeof(fpr));
    if (!basis || !d00 || !g01 || !scratch) {
        fprintf(stderr, "alloc fail\n"); exit(1);
    }

    build_basis_fft(logn, f, g, F, G, basis);

    /* 3. Compute d00 once via gram_fft on a basis copy (g00 written into b00). */
    memcpy(scratch, basis, 4 * n * sizeof(fpr));
    fpoly_gram_fft(logn,
        scratch + 0 * n, scratch + 1 * n, scratch + 2 * n, scratch + 3 * n);
    /* g00 lives in scratch[0..n-1]; self-adjoint, so first n/2 reals are d00. */
    memcpy(d00, scratch, hn * sizeof(fpr));

    /* 4. Time recompute_g01 + recompute_l10. */
    enum { RECOMPUTE_INNER = 1000, RECOMPUTE_SAMPLES = 30, RECOMPUTE_WARMUP = 5 };
    uint64_t tt_rec[RECOMPUTE_SAMPLES];
    volatile fpr sink = 0;
    for (int s = 0; s < RECOMPUTE_SAMPLES; s ++) {
        uint64_t t0 = cycles_now();
        for (int k = 0; k < RECOMPUTE_INNER; k ++) {
            recompute_g01(logn, g01,
                basis + 0 * n, basis + 1 * n,
                basis + 2 * n, basis + 3 * n);
            recompute_l10(logn, g01, d00);
            sink ^= g01[0] ^ g01[hn];
        }
        uint64_t t1 = cycles_now();
        tt_rec[s] = t1 - t0;
    }
    double recompute_per_iter = median_per_iter(
        tt_rec, RECOMPUTE_SAMPLES, RECOMPUTE_INNER, RECOMPUTE_WARMUP);

    /* 5. Time current fndsa_sign_seeded for the same key. */
    enum { SIGN_SAMPLES = 60, SIGN_WARMUP = 10 };
    uint64_t tt_sgn[SIGN_SAMPLES];
    uint8_t *sig = malloc(FNDSA_SIGNATURE_SIZE(10));
    if (!sig) { fprintf(stderr, "alloc fail\n"); exit(1); }
    uint8_t sgnseed[32] = { 0x42, 0x42, 0x42, 0x42 };
    for (int s = 0; s < SIGN_SAMPLES; s ++) {
        sgnseed[0] = (uint8_t)s;
        sgnseed[1] = (uint8_t)logn;
        uint64_t t0 = cycles_now();
        size_t r = fndsa_sign_seeded(sk, FNDSA_SIGN_KEY_SIZE(logn),
            NULL, 0, FNDSA_HASH_ID_RAW, "test", 4,
            sgnseed, sizeof sgnseed,
            sig, FNDSA_SIGNATURE_SIZE(logn));
        uint64_t t1 = cycles_now();
        if (r == 0) { fprintf(stderr, "sign fail logn=%u\n", logn); exit(1); }
        tt_sgn[s] = t1 - t0;
        sink ^= sig[1];
    }
    double sign_per_iter = median_per_iter(
        tt_sgn, SIGN_SAMPLES, 1, SIGN_WARMUP);

    /* 6. Report. */
    double ratio_pct = 100.0 * recompute_per_iter / sign_per_iter;
    printf("  logn=%-2u  recompute = %12.1f " TIME_UNIT
           "  sign = %14.1f " TIME_UNIT
           "  ratio = %6.3f %%\n",
           logn, recompute_per_iter, sign_per_iter, ratio_pct);
    fflush(stdout);

    if (sink == (fpr)0xFFFFFFFFFFFFFFFFULL) {
        fprintf(stderr, "(sink trap, ignore)\n");
    }

    free(sig);
    free(scratch); free(g01); free(d00); free(basis);
    free(g); free(f);
    free(vk); free(sk);
}

int
main(void)
{
    printf("=== Phase-1 recompute-step microbench ===\n");
    printf("Recompute step per call: g01 = b00*adj(b10) + b01*adj(b11);\n");
    printf("                         l10 = g01 / d00 (in place over g01)\n");
    printf("Sign baseline: fndsa_sign_seeded (full signing path; FNDSA_PATH_B if libs built so)\n");
    printf("Time unit: %s. Ratio = recompute / sign (smaller is better for phase 1).\n\n",
           TIME_UNIT);
    bench_logn(9);
    bench_logn(10);
    printf("\nInterpretation:\n");
    printf("  Ratio < 2%%  → recompute is cheap; phase 1 deployable without storing l10.\n");
    printf("  Ratio 2-5%% → marginal; consider storing l10 alongside basis (extra n FLR).\n");
    printf("  Ratio > 5%%  → storing only the basis loses too much; revisit phase-1 layout.\n");
    printf("Note: M35P is scalar-only; its ratio will likely shift up vs this SIMD host\n");
    printf("      number, since recompute_g01/l10 SIMD-vectorize cleanly while sign_core\n");
    printf("      already SIMD-vectorizes too. Treat host ratio as a coarse filter.\n");
    return 0;
}
