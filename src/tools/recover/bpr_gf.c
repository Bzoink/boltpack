/* GF(2^8) arithmetic for the sidecar (polynomial 0x11D, generator 2).
 *
 * bpr_gf_muladd(dst, src, n, c) is dst ^= c * src over n bytes.  Three kernels
 * give the same bytes: a 256x256 table (portable, about 1.9 GB/s), and on x86
 * split-nibble versions with pshufb (SSSE3, AVX2), chosen at run time.  The
 * environment variable BPR_GF_KERNEL=scalar|ssse3|avx2 forces one, so the
 * portable path can be tested on a machine that has the fast ones. */
#include "bpr.h"
#include "bpr_sc.h"

#include <stdlib.h>
#include <string.h>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#define BPR_X86 1
#include <immintrin.h>
#endif

static uint8_t exp_t[512], log_t[256];
static uint8_t mul_t[256][256];
static int ready;
typedef void (*muladd_fn)(uint8_t *, const uint8_t *, size_t, uint8_t);
static muladd_fn kernel;
static const char *kernel_name = "scalar";

uint8_t bpr_gf_mul(uint8_t a, uint8_t b) { return mul_t[a][b]; }
uint8_t bpr_gf_inv(uint8_t a) { return exp_t[255 - log_t[a]]; }

static void k_scalar(uint8_t *d, const uint8_t *s, size_t n, uint8_t c)
{
    const uint8_t *t = mul_t[c];
    size_t i;
    for (i = 0; i < n; i++) d[i] ^= t[s[i]];
}

#ifdef BPR_X86
static void nibbles(uint8_t c, uint8_t lo[16], uint8_t hi[16])
{
    int x;
    for (x = 0; x < 16; x++) {
        lo[x] = mul_t[c][x];
        hi[x] = mul_t[c][x << 4];
    }
}

__attribute__((target("ssse3")))
static void k_ssse3(uint8_t *d, const uint8_t *s, size_t n, uint8_t c)
{
    uint8_t lo[16], hi[16];
    __m128i l, h, m = _mm_set1_epi8(0x0f);
    size_t i = 0;
    nibbles(c, lo, hi);
    l = _mm_loadu_si128((const __m128i *)lo);
    h = _mm_loadu_si128((const __m128i *)hi);
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i a = _mm_shuffle_epi8(l, _mm_and_si128(v, m));
        __m128i b = _mm_shuffle_epi8(h, _mm_and_si128(_mm_srli_epi16(v, 4), m));
        __m128i o = _mm_loadu_si128((const __m128i *)(d + i));
        _mm_storeu_si128((__m128i *)(d + i), _mm_xor_si128(o, _mm_xor_si128(a, b)));
    }
    for (; i < n; i++) d[i] ^= (uint8_t)(lo[s[i] & 15] ^ hi[s[i] >> 4]);
}

__attribute__((target("avx2")))
static void k_avx2(uint8_t *d, const uint8_t *s, size_t n, uint8_t c)
{
    uint8_t lo[16], hi[16];
    __m256i l, h, m = _mm256_set1_epi8(0x0f);
    size_t i = 0;
    nibbles(c, lo, hi);
    l = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)lo));
    h = _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)hi));
    for (; i + 32 <= n; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i a = _mm256_shuffle_epi8(l, _mm256_and_si256(v, m));
        __m256i b = _mm256_shuffle_epi8(h, _mm256_and_si256(_mm256_srli_epi16(v, 4), m));
        __m256i o = _mm256_loadu_si256((const __m256i *)(d + i));
        _mm256_storeu_si256((__m256i *)(d + i), _mm256_xor_si256(o, _mm256_xor_si256(a, b)));
    }
    for (; i < n; i++) d[i] ^= (uint8_t)(lo[s[i] & 15] ^ hi[s[i] >> 4]);
}
#endif

void bpr_gf_init(void)
{
    unsigned x = 1;
    int i, a, b;
    const char *want;
    if (ready) return;
    for (i = 0; i < 255; i++) {
        exp_t[i] = (uint8_t)x;
        log_t[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (i = 255; i < 512; i++) exp_t[i] = exp_t[i - 255];
    for (a = 0; a < 256; a++)
        for (b = 0; b < 256; b++)
            mul_t[a][b] = (a && b) ? exp_t[log_t[a] + log_t[b]] : 0;
    kernel = k_scalar;
    kernel_name = "scalar";
    want = getenv("BPR_GF_KERNEL");
#ifdef BPR_X86
    __builtin_cpu_init();
    if (want == NULL || strcmp(want, "avx2") == 0) {
        if (__builtin_cpu_supports("avx2")) { kernel = k_avx2; kernel_name = "avx2"; }
    }
    if (kernel == k_scalar && (want == NULL || strcmp(want, "ssse3") == 0)) {
        if (__builtin_cpu_supports("ssse3")) { kernel = k_ssse3; kernel_name = "ssse3"; }
    }
#endif
    (void)want;
    ready = 1;
}

const char *bpr_gf_kernel_name(void) { return kernel_name; }

void bpr_gf_muladd(uint8_t *dst, const uint8_t *src, size_t n, uint8_t c)
{
    if (c == 0) return;
    kernel(dst, src, n, c);
}

/* Cauchy coefficient of parity row j and group member k: 1 / (j xor (r + k)).
 * The caller guarantees r + k <= 255 and j < r, so the argument is never 0. */
uint8_t bpr_gf_cauchy(unsigned r, unsigned j, unsigned k)
{
    return bpr_gf_inv((uint8_t)(j ^ (r + k)));
}

/* Gauss-Jordan inverse of an n x n matrix (row-major).  m is destroyed.
 * Returns 0, or -1 if singular (cannot happen for a Cauchy submatrix). */
int bpr_gf_invert(uint8_t *m, uint8_t *inv, unsigned n)
{
    unsigned i, j, k;
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) inv[i * n + j] = (uint8_t)(i == j);
    for (i = 0; i < n; i++) {
        uint8_t ip;
        if (m[i * n + i] == 0) {
            for (j = i + 1; j < n && m[j * n + i] == 0; j++) ;
            if (j == n) return -1;
            for (k = 0; k < n; k++) {
                uint8_t t = m[i * n + k]; m[i * n + k] = m[j * n + k]; m[j * n + k] = t;
                t = inv[i * n + k]; inv[i * n + k] = inv[j * n + k]; inv[j * n + k] = t;
            }
        }
        ip = bpr_gf_inv(m[i * n + i]);
        for (k = 0; k < n; k++) {
            m[i * n + k] = bpr_gf_mul(m[i * n + k], ip);
            inv[i * n + k] = bpr_gf_mul(inv[i * n + k], ip);
        }
        for (j = 0; j < n; j++) {
            uint8_t f = m[j * n + i];
            if (j == i || f == 0) continue;
            for (k = 0; k < n; k++) {
                m[j * n + k] ^= bpr_gf_mul(f, m[i * n + k]);
                inv[j * n + k] ^= bpr_gf_mul(f, inv[i * n + k]);
            }
        }
    }
    return 0;
}
