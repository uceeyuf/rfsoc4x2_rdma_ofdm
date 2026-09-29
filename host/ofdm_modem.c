/*
 * I/Q OFDM modem on the host: the transmitter and receiver of sw/src/ofdm.c (A53) and
 * host/ofdm.py without the board I/O, with the state in a context so that several threads can
 * each work on their own frames.
 *
 * 2.0 GSPS, N = 1024, CP = 128, active k = +/-16 .. +/-460, pilots on k = +/-16, 32, ...
 * Frame: two training symbols, 26 data symbols, 512 zeros = 32768 samples.
 *
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#include "ofdm_modem.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define N           OFDM_N
#define CP          OFDM_CP
#define SYM         OFDM_SYM
#define K_LO        OFDM_K_LO
#define K_HI        OFDM_K_HI
#define N_DATA_SYM  OFDM_DATA_SYM
#define FRAME       OFDM_FRAME
#define PILOT_STEP  16
#define BACKOFF     8
#define SMOOTH      3
#define MAX_DATA    (2 * (K_HI - K_LO + 1))

typedef float complex cf;

/* tables shared by every context, filled once */
static int n_data, data_k[MAX_DATA];
static int n_pilot, pilot_k[MAX_DATA];
static cf  t1[N], t2[N], tw[N / 2];
static float rr[N], ri[N];              /* Re / Im of the time-domain training symbol */
static int tables_ready;

struct ofdm_ctx {
    uint32_t xs_state, bit_word;
    int      bit_pos;
    cf       frame[FRAME];
    cf       Y[2 + N_DATA_SYM][N];
    cf       H[N], A[N], B[N], X[N], buf[N];
    float    ai[2 * FRAME], aq[2 * FRAME];
    float    irr[MAX_DATA + 64];
};

static uint32_t xs_next(ofdm_ctx *c)
{
    uint32_t s = c->xs_state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return c->xs_state = s;
}

static int kidx(int k) { return k < 0 ? k + N : k; }
static float pilot_value(int k) { return (((k < 0 ? k - PILOT_STEP + 1 : k) / PILOT_STEP) & 1) ? -1.0f : 1.0f; }

/* radix-2 FFT in place, sign -1 forward, +1 inverse (unscaled) */
static void fft(cf *x, int sign)
{
    for (int i = 1, j = 0; i < N; i++) {
        int b = N >> 1;
        for (; j & b; b >>= 1)
            j ^= b;
        j |= b;
        if (i < j) {
            cf t = x[i]; x[i] = x[j]; x[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int step = N / len;
        for (int i = 0; i < N; i += len)
            for (int k = 0; k < len / 2; k++) {
                cf w = sign < 0 ? tw[k * step] : conjf(tw[k * step]);
                cf u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }
    }
}

static void tables(void)
{
    if (tables_ready)
        return;
    for (int k = 0; k < N / 2; k++)
        tw[k] = cexpf(-2.0f * (float)M_PI * I * k / N);
    n_data = n_pilot = 0;
    for (int k = -K_HI; k <= K_HI; k++) {
        if (k > -K_LO && k < K_LO)
            continue;
        if (k % PILOT_STEP == 0)
            pilot_k[n_pilot++] = k;
        else
            data_k[n_data++] = k;
    }
    ofdm_ctx tmp = {.xs_state = 0x1234ABCD};
    memset(t1, 0, sizeof(t1));
    for (int k = -K_HI; k <= K_HI; k++) {
        if (k > -K_LO && k < K_LO)
            continue;
        uint32_t r = xs_next(&tmp);
        t1[kidx(k)] = ((r & 1 ? 1.0f : -1.0f) + I * (r & 2 ? 1.0f : -1.0f)) / sqrtf(2.0f);
    }
    for (int k = 0; k < N; k++)
        t2[k] = (k > N / 2) ? -t1[k] : t1[k];
    cf b[N];
    memcpy(b, t1, sizeof(b));
    fft(b, +1);
    for (int n = 0; n < N; n++) {
        rr[n] = crealf(b[n]);
        ri[n] = cimagf(b[n]);
    }
    tables_ready = 1;
}

ofdm_ctx *ofdm_new(void)
{
    tables();
    return calloc(1, sizeof(ofdm_ctx));
}

void ofdm_free(ofdm_ctx *c) { free(c); }
int ofdm_data_carriers(void) { tables(); return n_data; }
double ofdm_rate_bps(int m, double fs) { tables(); return (double)N_DATA_SYM * n_data * m / (FRAME / fs); }

/* payload: xorshift32 words, LSB first */
static void bits_reset(ofdm_ctx *c, int m) { c->xs_state = 0xC0FFEE00u + m; c->bit_pos = 32; }
static int bit_next(ofdm_ctx *c)
{
    if (c->bit_pos == 32) {
        c->bit_word = xs_next(c);
        c->bit_pos = 0;
    }
    return (c->bit_word >> c->bit_pos++) & 1;
}

static float qam_scale(int m) { int L = 1 << (m / 2); return sqrtf(2.0f * (L * L - 1) / 3.0f); }

static float axis_map(ofdm_ctx *c, int h)
{
    int v = 0;
    for (int j = 0; j < h; j++)
        v = (v << 1) | (bit_next(c) ^ (v & 1));   /* Gray -> binary */
    return (float)(2 * v - ((1 << h) - 1));
}

static cf qam_next(ofdm_ctx *c, int m)
{
    float s = qam_scale(m);
    float re = axis_map(c, m / 2);
    float im = axis_map(c, m / 2);
    return (re + I * im) / s;
}

static int axis_errors(ofdm_ctx *c, float v, int h, float s)
{
    int L = 1 << h;
    int idx = (int)lroundf((v * s + (L - 1)) / 2);
    idx = idx < 0 ? 0 : idx > L - 1 ? L - 1 : idx;
    int g = idx ^ (idx >> 1), e = 0;
    for (int j = h - 1; j >= 0; j--)
        e += ((g >> j) & 1) != bit_next(c);
    return e;
}

int ofdm_mod(ofdm_ctx *c, int m, double rms_fs, int16_t *out_i, int16_t *out_q)
{
    cf *f = c->buf;
    memset(c->frame, 0, sizeof(c->frame));
    bits_reset(c, m);
    for (int s = 0; s < 2 + N_DATA_SYM; s++) {
        if (s == 0)
            memcpy(f, t1, sizeof(c->buf));
        else if (s == 1)
            memcpy(f, t2, sizeof(c->buf));
        else {
            memset(f, 0, sizeof(c->buf));
            for (int i = 0; i < n_data; i++)
                f[kidx(data_k[i])] = qam_next(c, m);
            for (int i = 0; i < n_pilot; i++)
                f[kidx(pilot_k[i])] = pilot_value(pilot_k[i]);
        }
        fft(f, +1);
        cf *o = &c->frame[s * SYM];
        memcpy(o, &f[N - CP], CP * sizeof(cf));
        memcpy(o + CP, f, N * sizeof(cf));
    }
    double p = 0;
    for (int n = 0; n < (2 + N_DATA_SYM) * SYM; n++)
        p += crealf(c->frame[n]) * crealf(c->frame[n]);
    float g = (float)(rms_fs * 32767 / sqrt(p / ((2 + N_DATA_SYM) * SYM)));
    int clip = 0;
    for (int n = 0; n < FRAME; n++) {
        cf v = c->frame[n] * g;
        float re = crealf(v), im = cimagf(v);
        if (fabsf(re) > 32767 || fabsf(im) > 32767)
            clip++;
        out_i[n] = (int16_t)fmaxf(-32767, fminf(32767, lroundf(re)));
        out_q[n] = (int16_t)fmaxf(-32767, fminf(32767, lroundf(im)));
    }
    return clip;
}

static float dotf(const float *a, const float *b, int n)
{
    float s = 0;
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void smooth(cf *arr)
{
    enum { NK = K_HI - K_LO + 1 };
    cf v[NK], ph[NK];
    for (int sgn = 1; sgn >= -1; sgn -= 2) {
        for (int i = 0; i < NK; i++) {
            int k = sgn * (K_LO + i);
            ph[i] = cexpf(-2.0f * (float)M_PI * I * k * BACKOFF / N);
            v[i] = arr[kidx(k)] / ph[i];
        }
        for (int i = 0; i < NK; i++) {
            int lo = i - SMOOTH < 0 ? 0 : i - SMOOTH, hi = i + SMOOTH > NK - 1 ? NK - 1 : i + SMOOTH;
            cf s = 0;
            for (int j = lo; j <= hi; j++)
                s += v[j];
            arr[kidx(sgn * (K_LO + i))] = s / (float)(hi - lo + 1) * ph[i];
        }
    }
}

int ofdm_demod(ofdm_ctx *c, int m, const int16_t *ci, const int16_t *cq, int nsamp, int acquire,
               ofdm_result *r)
{
    if (nsamp > 2 * FRAME)
        nsamp = 2 * FRAME;
    for (int n = 0; n < nsamp; n++) {
        c->ai[n] = ci[n];
        c->aq[n] = cq[n];
    }
    float best = 1;
    if (acquire) {
        memset(r, 0, sizeof(*r));
        /* timing on the I rail against Re{training} */
        best = 0;
        int p = 0;
        for (int l = 0; l < FRAME && l + N <= nsamp; l++) {
            float v = fabsf(dotf(&c->ai[l], rr, N));
            if (v > best) {
                best = v;
                p = l;
            }
        }
        /* Q rail: polarity and offset against Im{training} */
        float bq = 0, cqv = 0;
        for (int l = -CP; l <= CP; l++) {
            if (p + l < 0 || p + l + N > nsamp)
                continue;
            float v = dotf(&c->aq[p + l], ri, N);
            if (fabsf(v) > bq) {
                bq = fabsf(v);
                cqv = v;
                r->q_lag = l;
            }
        }
        r->frame_pos = p;
        r->q_sign = cqv < 0 ? -1 : 1;
    }
    int p = r->frame_pos;

    int start = p - CP - BACKOFF;
    if (start < 0)
        start += FRAME;
    if (start + (2 + N_DATA_SYM) * SYM > nsamp)
        return -1;
    for (int s = 0; s < 2 + N_DATA_SYM; s++) {
        int o = start + s * SYM + CP;
        for (int n = 0; n < N; n++)
            c->Y[s][n] = (c->ai[o + n] + I * r->q_sign * c->aq[o + n]) / N;
        fft(c->Y[s], -1);
    }

    cf *H = c->H, *A = c->A, *B = c->B, *X = c->X;
    cf (*Y)[N] = c->Y;
    int ni = 0;
    for (int k = K_LO; k <= K_HI; k++) {
        int kp = k, kn = N - k;
        H[kp] = 0.5f * (Y[0][kp] / t1[kp] + Y[1][kp] / t2[kp]);
        H[kn] = 0.5f * (Y[0][kn] / t1[kn] + Y[1][kn] / t2[kn]);
        A[kp] = (Y[0][kp] + Y[1][kp]) / (2 * t1[kp]);
        B[kp] = (Y[0][kp] - Y[1][kp]) / (2 * conjf(t1[kn]));
        A[kn] = (Y[0][kn] - Y[1][kn]) / (2 * t1[kn]);
        B[kn] = (Y[0][kn] + Y[1][kn]) / (2 * conjf(t1[kp]));
    }
    if (abs(r->q_lag) <= 2) {
        smooth(H);
        smooth(A);
        smooth(B);
    }
    for (int k = K_LO; k <= K_HI; k++) {
        int kp = k, kn = N - k;
        c->irr[ni++] = 10 * log10f(cabsf(A[kp]) * cabsf(A[kp]) / fmaxf(cabsf(B[kp]) * cabsf(B[kp]), 1e-30f));
        c->irr[ni++] = 10 * log10f(cabsf(A[kn]) * cabsf(A[kn]) / fmaxf(cabsf(B[kn]) * cabsf(B[kn]), 1e-30f));
    }
    qsort(c->irr, ni, sizeof(float), cmpf);
    r->irr_db = 0.5 * (c->irr[ni / 2 - 1] + c->irr[ni / 2]);

    float sc = qam_scale(m);
    for (int wl = 0; wl < 2; wl++) {
        double e2 = 0, s2 = 0;
        int errs = 0;
        bits_reset(c, m);
        for (int s = 0; s < N_DATA_SYM; s++) {
            memset(X, 0, sizeof(c->X));
            for (int k = K_LO; k <= K_HI; k++) {
                int kp = k, kn = N - k;
                if (!wl) {
                    X[kp] = Y[2 + s][kp] / H[kp];
                    X[kn] = Y[2 + s][kn] / H[kn];
                } else {
                    cf a = A[kp], b = B[kp], cc = conjf(B[kn]), d = conjf(A[kn]);
                    cf y1 = Y[2 + s][kp], y2 = conjf(Y[2 + s][kn]);
                    cf det = a * d - b * cc;
                    X[kp] = (d * y1 - b * y2) / det;
                    X[kn] = conjf((-cc * y1 + a * y2) / det);
                }
            }
            cf acc = 0;
            for (int i = 0; i < n_pilot; i++)
                acc += X[kidx(pilot_k[i])] * pilot_value(pilot_k[i]);
            cf rot = cexpf(-I * cargf(acc));
            for (int i = 0; i < n_data; i++) {
                cf x = X[kidx(data_k[i])] * rot;
                uint32_t st = c->xs_state, w = c->bit_word;
                int bp = c->bit_pos;
                cf ref = qam_next(c, m);
                c->xs_state = st; c->bit_word = w; c->bit_pos = bp;   /* replay the same bits */
                errs += axis_errors(c, crealf(x), m / 2, sc);
                errs += axis_errors(c, cimagf(x), m / 2, sc);
                cf e = x - ref;
                e2 += crealf(e) * crealf(e) + cimagf(e) * cimagf(e);
                s2 += crealf(ref) * crealf(ref) + cimagf(ref) * cimagf(ref);
            }
        }
        double evm = 10 * log10(e2 / s2);
        if (wl) { r->evm_wl_db = evm; r->err_wl = errs; }
        else    { r->evm_lin_db = evm; r->err_lin = errs; }
    }
    r->bits = N_DATA_SYM * n_data * m;
    return best > 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- streaming receiver */
static cf ph_tab[K_HI - K_LO + 1][2];          /* back-off phase slope, k > 0 / k < 0 */
static int ph_ready;

static void smooth_fast(cf *arr)
{
    enum { NK = K_HI - K_LO + 1 };
    cf v[NK];
    if (!ph_ready) {
        for (int i = 0; i < NK; i++)
            for (int s = 0; s < 2; s++)
                ph_tab[i][s] = cexpf(-2.0f * (float)M_PI * I * (s ? -1 : 1) * (K_LO + i) * BACKOFF / N);
        ph_ready = 1;
    }
    for (int s = 0; s < 2; s++) {
        int sgn = s ? -1 : 1;
        for (int i = 0; i < NK; i++)
            v[i] = arr[kidx(sgn * (K_LO + i))] * conjf(ph_tab[i][s]);     /* |ph| = 1 */
        cf run = 0;
        for (int j = 0; j <= SMOOTH && j < NK; j++)
            run += v[j];
        for (int i = 0; i < NK; i++) {
            int lo = i - SMOOTH < 0 ? 0 : i - SMOOTH, hi = i + SMOOTH > NK - 1 ? NK - 1 : i + SMOOTH;
            arr[kidx(sgn * (K_LO + i))] = run / (float)(hi - lo + 1) * ph_tab[i][s];
            if (i + SMOOTH + 1 < NK) run += v[i + SMOOTH + 1];
            if (i - SMOOTH >= 0) run -= v[i - SMOOTH];
        }
    }
}

/* bits are appended LSB first into a 64-bit accumulator, 32 at a time to memory; a Gray code
   goes out MSB first, so it is bit-reversed (h <= 4) through a table */
static uint8_t rev_h[5][16];

static inline int slice(float v, int h, float s)
{
    int L = 1 << h;
    int idx = (int)lrintf((v * s + (L - 1)) * 0.5f);
    idx = idx < 0 ? 0 : idx > L - 1 ? L - 1 : idx;
    return idx ^ (idx >> 1);                    /* binary -> Gray */
}

int ofdm_demod_stream(ofdm_ctx *c, int m, const int16_t *ci, const int16_t *cq, int p, int q_sign,
                      uint8_t *out)
{
    int start = p - CP - BACKOFF;
    if (start < 0)
        start += FRAME;
    const float qs = (float)q_sign, inv_n = 1.0f / N;
    for (int s = 0; s < 2 + N_DATA_SYM; s++) {
        int o = start + s * SYM + CP;
        cf *y = c->Y[s];
        for (int n = 0; n < N; n++)
            y[n] = (ci[o + n] + I * qs * cq[o + n]) * inv_n;
        fft(y, -1);
    }
    cf (*Y)[N] = c->Y;
    cf *A = c->A, *B = c->B;
    for (int k = K_LO; k <= K_HI; k++) {
        int kp = k, kn = N - k;
        A[kp] = (Y[0][kp] + Y[1][kp]) / (2 * t1[kp]);
        B[kp] = (Y[0][kp] - Y[1][kp]) / (2 * conjf(t1[kn]));
        A[kn] = (Y[0][kn] - Y[1][kn]) / (2 * t1[kn]);
        B[kn] = (Y[0][kn] + Y[1][kn]) / (2 * conjf(t1[kp]));
    }
    smooth_fast(A);
    smooth_fast(B);
    /* per pair: X[kp] = m11 y[kp] + m12 conj(y[kn]),  X[kn] = m21 conj(y[kp]) + m22 y[kn] */
    cf *m11 = c->H, *m12 = c->X, *m21 = c->buf, m22[N];
    for (int k = K_LO; k <= K_HI; k++) {
        int kp = k, kn = N - k;
        cf a = A[kp], b = B[kp], cc = conjf(B[kn]), d = conjf(A[kn]);
        cf idet = 1.0f / (a * d - b * cc);
        m11[kp] = d * idet;
        m12[kp] = -b * idet;
        m21[kp] = conjf(-cc * idet);
        m22[kp] = conjf(a * idet);
    }
    float sc = qam_scale(m);
    int h = m / 2;
    if (!rev_h[4][1]) {
        for (int hh = 1; hh <= 4; hh++)
            for (int g = 0; g < (1 << hh); g++) {
                int r = 0;
                for (int j = 0; j < hh; j++) r |= ((g >> j) & 1) << (hh - 1 - j);
                rev_h[hh][g] = (uint8_t)r;
            }
    }
    const uint8_t *rv = rev_h[h];
    uint64_t acc64 = 0;
    int nacc = 0;
    uint8_t *wp = out;
    cf x[N];
    for (int s = 0; s < N_DATA_SYM; s++) {
        const cf *y = Y[2 + s];
        for (int k = K_LO; k <= K_HI; k++) {
            int kp = k, kn = N - k;
            cf y1 = y[kp], y2 = y[kn];
            x[kp] = m11[kp] * y1 + m12[kp] * conjf(y2);
            x[kn] = m21[kp] * conjf(y1) + m22[kp] * y2;
        }
        cf acc = 0;
        for (int i = 0; i < n_pilot; i++)
            acc += x[kidx(pilot_k[i])] * pilot_value(pilot_k[i]);
        float mag = cabsf(acc);
        cf rot = mag > 0 ? conjf(acc) / mag : 1.0f;
        for (int i = 0; i < n_data; i++) {
            cf v = x[kidx(data_k[i])] * rot;
            acc64 |= (uint64_t)(rv[slice(crealf(v), h, sc)] | (rv[slice(cimagf(v), h, sc)] << h)) << nacc;
            nacc += 2 * h;
            if (nacc >= 32) {
                memcpy(wp, &acc64, 4);
                wp += 4;
                acc64 >>= 32;
                nacc -= 32;
            }
        }
    }
    int total = N_DATA_SYM * n_data * m;
    memcpy(wp, &acc64, (size_t)(nacc + 7) / 8);
    return total;
}

int ofdm_ref_bits(int m, uint8_t *out)
{
    tables();
    ofdm_ctx c;
    bits_reset(&c, m);
    int n = N_DATA_SYM * n_data * m;
    memset(out, 0, (size_t)(n + 7) / 8);
    for (int b = 0; b < n; b++)
        if (bit_next(&c))
            out[b >> 3] |= (uint8_t)(1u << (b & 7));
    return n;
}
