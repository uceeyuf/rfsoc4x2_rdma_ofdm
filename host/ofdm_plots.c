/*
 * ofdm_plots - spectrum and constellation from a raw sample dump of rf_ofdm (--dump):
 *   <out>_spectrum.txt   frequency (MHz), received PSD (dB), transmitted PSD (dB): Welch, 2048-point
 *                        FFT, Hann window, over every sample of the dump; the transmitted reference
 *                        is the streaming modulator's output for random payloads at the same level
 *   <out>_const.bin      data sub-carrier symbols after equalisation (float re, im; QAM grid at the
 *                        odd integers), from every frame of the dump
 *   stdout               EVM (RMS error vector / RMS grid point) per frame and overall
 * Then: python3 host/plot_ofdm.py <out>
 *
 *   host/ofdm_plots build/rx.dump build/plot_16qam [--rms 0.18]
 *
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#include "ofdm_modem.h"

#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NF 2048                                 /* spectrum FFT size (0.98 MHz bins at 2 GSPS) */
#define FS 2.0e9

typedef float complex cf;

static void fft(cf *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) { cf t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        cf wl = cexpf(-2.0f * (float)M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            cf w = 1;
            for (int k = 0; k < len / 2; k++, w *= wl) {
                cf u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
            }
        }
    }
}

/* Welch PSD of nsamp samples in the stream memory format (Q times q_sign), accumulated in psd */
static void welch(const int16_t *mem, long nsamp, int q_sign, double *psd, long *nseg)
{
    static float win[NF];
    if (!win[1])
        for (int i = 0; i < NF; i++) win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / NF);
    cf x[NF];
    for (long s = 0; s + NF <= nsamp; s += NF / 2) {
        for (int i = 0; i < NF; i++) {
            long t = s + i;
            const int16_t *b = mem + (t >> 3) * 16 + (t & 7);
            x[i] = (b[0] + I * (float)q_sign * b[8]) * win[i];
        }
        fft(x, NF);
        for (int i = 0; i < NF; i++) psd[i] += crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
        (*nseg)++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: ofdm_plots DUMP OUT_PREFIX [--rms 0.18]\n"); return 1; }
    double rms = 0.18;
    for (int i = 3; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--rms")) rms = atof(argv[i + 1]);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    char magic[8];
    int32_t h[6];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "OFDMDUMP", 8) || fread(h, sizeof(h), 1, f) != 1) {
        fprintf(stderr, "%s: not an rf_ofdm dump\n", argv[1]);
        return 1;
    }
    int m = h[0], p = h[1], q_sign = h[2], nfr = h[3];
    if (h[4] != OFDM_FRAME) { fprintf(stderr, "frame length %d, expected %d\n", h[4], OFDM_FRAME); return 1; }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f) - 32;
    fseek(f, 32, SEEK_SET);
    int16_t *mem = aligned_alloc(64, ((size_t)bytes + 63) & ~(size_t)63);
    if (fread(mem, 1, (size_t)bytes, f) != (size_t)bytes) { perror("read"); return 1; }
    fclose(f);

    /* constellation and EVM */
    ofdm_ctx *c = ofdm_new();
    int nd = ofdm_data_carriers(), per = OFDM_DATA_SYM * nd, L = 1 << (m / 2);
    float *sym = malloc(sizeof(float) * 2 * per);
    uint8_t *bits = malloc((size_t)ofdm_frame_bits(m) / 8 + 64);
    char path[512];
    snprintf(path, sizeof(path), "%s_const.bin", argv[2]);
    FILE *fc = fopen(path, "wb");
    double e_tot = 0, r_tot = 0;
    printf("%d-QAM, %d frames, Q sign %+d\nframe  EVM (dB)\n", 1 << m, nfr, q_sign);
    for (int fr = 0; fr < nfr; fr++) {
        ofdm_symbols_mem(c, m, mem, p + fr * OFDM_FRAME, q_sign, bits, sym);
        double e = 0, r = 0;
        for (int i = 0; i < per; i++)
            for (int a = 0; a < 2; a++) {
                float v = sym[2 * i + a];
                float g = 2 * floorf(v / 2) + 1;            /* nearest odd integer */
                g = g > L - 1 ? L - 1 : g < -(L - 1) ? -(L - 1) : g;
                e += (v - g) * (v - g);
                r += g * g;
            }
        e_tot += e;
        r_tot += r;
        if (fr < 8 || fr == nfr - 1) printf("%5d  %7.2f\n", fr, 10 * log10(e / r));
        fwrite(sym, sizeof(float), 2 * (size_t)per, fc);
    }
    fclose(fc);
    printf("overall EVM %.2f dB (%d symbols)\n", 10 * log10(e_tot / r_tot), per * nfr);

    /* spectra: received, and the modulator's own output at the same level */
    static double prx[NF], ptx[NF];
    long nrx = 0, ntx = 0;
    welch(mem, (long)nfr * OFDM_FRAME, q_sign, prx, &nrx);
    int16_t *tx = aligned_alloc(64, OFDM_FRAME * 4);
    uint8_t *pay = malloc((size_t)ofdm_frame_bits(m) / 8 + 64);
    uint32_t x = 12345;
    for (int fr = 0; fr < 32; fr++) {
        for (int b = 0; b < ofdm_frame_bits(m) / 8 + 64; b++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; pay[b] = (uint8_t)x; }
        ofdm_mod_stream(c, m, ofdm_stream_gain(rms), pay, tx);
        welch(tx, OFDM_FRAME, 1, ptx, &ntx);
    }
    snprintf(path, sizeof(path), "%s_spectrum.txt", argv[2]);
    FILE *fs = fopen(path, "w");
    fprintf(fs, "# %d-QAM, EVM %.2f dB; frequency_MHz rx_dBFS tx_dBFS (per %.3f MHz bin)\n", 1 << m,
            10 * log10(e_tot / r_tot), FS / NF / 1e6);
    /* dB full scale: a full-scale complex tone in one bin = 0 dB */
    double wsum = 0;
    for (int i = 0; i < NF; i++) { double w = 0.5 - 0.5 * cos(2 * M_PI * i / NF); wsum += w; }
    double ref = (32767.0 * wsum) * (32767.0 * wsum);
    for (int k = -NF / 2; k < NF / 2; k++) {
        int i = (k + NF) % NF;
        fprintf(fs, "%.3f %.2f %.2f\n", k * FS / NF / 1e6, 10 * log10(prx[i] / nrx / ref + 1e-30),
                10 * log10(ptx[i] / ntx / ref + 1e-30));
    }
    fclose(fs);
    printf("wrote %s_spectrum.txt, %s_const.bin\n", argv[2], argv[2]);
    return 0;
}
