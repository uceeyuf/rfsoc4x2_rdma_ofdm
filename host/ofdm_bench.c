/*
 * How fast can the host CPU run the OFDM modem? One frame is looped (as the DAC buffer plays it)
 * with noise and a time offset, demodulated once for EVM / bit errors, then timed:
 * modulation, acquisition (frame search) and tracking (known frame position, both equalizers),
 * on one core and on 1..T threads pinned to P-cores first (CPUs 0-7), then E-cores.
 * The sustained demodulation rate gives the sample rate a CPU receiver can follow, and with it
 * the signal bandwidth (the active sub-carriers cover 890 / 1024 of the sample rate).
 *
 *   host/ofdm_bench [--m 4] [--snr 35] [--rms 0.18] [--seconds 2] [--threads 1,2,4,8,12,16,20]
 *
 * --rms: RMS per rail as a fraction of full scale; at 0.25 the peaks of this frame clip (a floor
 * near -22 dB EVM from the clipping alone), 0.18 keeps them inside.
 *
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#define _GNU_SOURCE
#include "ofdm_modem.h"

#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int M = 4, NTH[32] = {1, 2, 4, 8, 12, 16, 20}, nth = 7;
static double SNR = 35, SECS = 2, RMS = 0.18;
static int16_t rx_i[2 * OFDM_FRAME], rx_q[2 * OFDM_FRAME];
static ofdm_result locked;
static atomic_int go;

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

struct targ { int cpu; long frames; };

static void *worker(void *arg)
{
    struct targ *t = arg;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(t->cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    ofdm_ctx *c = ofdm_new();
    static __thread uint8_t bits[OFDM_DATA_SYM * 1024];
    while (!atomic_load(&go)) {}
    double t0 = now();
    long n = 0;
    while (now() - t0 < SECS) {
        ofdm_demod_stream(c, M, rx_i, rx_q, locked.frame_pos, locked.q_sign, bits);
        n++;
    }
    t->frames = n;
    ofdm_free(c);
    return NULL;
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--m")) M = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--snr")) SNR = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) SECS = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--rms")) RMS = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) {
            nth = 0;
            for (char *t = strtok(argv[i + 1], ","); t && nth < 32; t = strtok(NULL, ",")) NTH[nth++] = atoi(t);
        }
    }
    const double fs = 2.0e9, frame_s = OFDM_FRAME / fs;
    const double occ = 2.0 * (OFDM_K_HI - OFDM_K_LO + 1) / OFDM_N;
    ofdm_ctx *c = ofdm_new();
    static int16_t ti[OFDM_FRAME], tq[OFDM_FRAME];

    /* transmit frame, looped, time offset and noise */
    int clip = ofdm_mod(c, M, RMS, ti, tq);
    double p = 0;
    for (int n = 0; n < OFDM_FRAME; n++) p += (double)ti[n] * ti[n];
    double sig = sqrt(p / OFDM_FRAME), sd = sig / pow(10, SNR / 20);
    const int shift = 12345;
    for (int n = 0; n < 2 * OFDM_FRAME; n++) {
        int k = (n + shift) % OFDM_FRAME;
        rx_i[n] = (int16_t)lround(ti[k] + sd * gauss());
        rx_q[n] = (int16_t)lround(tq[k] + sd * gauss());
    }
    ofdm_result r;
    int ok = ofdm_demod(c, M, rx_i, rx_q, 2 * OFDM_FRAME, 1, &r);
    printf("%d-QAM, %d data sub-carriers, frame %d samples (%.2f us at 2 GSPS), RMS %.2f FS, %d clipped\n",
           1 << M, ofdm_data_carriers(), OFDM_FRAME, frame_s * 1e6, RMS, clip);
    printf("check (SNR %.0f dB): frame %s at %d, Q sign %+d lag %+d, EVM lin %.1f / WL %.1f dB, "
           "bit errors %d / %d (lin), %d (WL)\n",
           SNR, ok ? "NOT found" : "found", r.frame_pos, r.q_sign, r.q_lag, r.evm_lin_db, r.evm_wl_db,
           r.err_lin, r.bits, r.err_wl);
    locked = r;

    /* one core */
    int reps = 20;
    double t0 = now();
    for (int i = 0; i < reps; i++) ofdm_mod(c, M, RMS, ti, tq);
    double t_mod = (now() - t0) / reps;
    t0 = now();
    for (int i = 0; i < 3; i++) ofdm_demod(c, M, rx_i, rx_q, 2 * OFDM_FRAME, 1, &r);
    double t_acq = (now() - t0) / 3;
    r = locked;
    t0 = now();
    for (int i = 0; i < reps; i++) ofdm_demod(c, M, rx_i, rx_q, 2 * OFDM_FRAME, 0, &r);
    double t_trk = (now() - t0) / reps;
    /* streaming receiver: bits against the transmitted payload, then its time */
    static uint8_t got[OFDM_DATA_SYM * 1024], ref[OFDM_DATA_SYM * 1024];
    int nb = ofdm_demod_stream(c, M, rx_i, rx_q, locked.frame_pos, locked.q_sign, got);
    ofdm_ref_bits(M, ref);
    long be = 0;
    for (int b = 0; b < nb; b++) be += ((got[b >> 3] ^ ref[b >> 3]) >> (b & 7)) & 1;
    t0 = now();
    for (int i = 0; i < reps * 5; i++) ofdm_demod_stream(c, M, rx_i, rx_q, locked.frame_pos, locked.q_sign, got);
    double t_str = (now() - t0) / (reps * 5);
    printf("\none core (CPU %d), per frame:\n"
           "  modulation                       %8.1f us  = %6.1f MS/s\n"
           "  acquisition (frame search, once) %8.1f us\n"
           "  diagnostic receiver (A53 code)   %8.1f us  = %6.1f MS/s  (both equalizers, IRR, BER)\n"
           "  streaming receiver               %8.1f us  = %6.1f MS/s  (bits: %ld errors / %d)\n",
           sched_getcpu(), t_mod * 1e6, OFDM_FRAME / t_mod / 1e6, t_acq * 1e6, t_trk * 1e6,
           OFDM_FRAME / t_trk / 1e6, t_str * 1e6, OFDM_FRAME / t_str / 1e6, be, nb);
    printf("  real time at 2 GSPS: %.0f cores for the streaming receiver, %.0f for the modulator\n",
           t_str / frame_s, t_mod / frame_s);

    /* threads: P-cores 0..7 first, then E-cores 8..19 */
    printf("\nstreaming receiver, threads on CPUs 0..T-1 (P-cores 0-7, then E-cores)\n"
           "threads  frames/s   demod MS/s   real-time share of 2 GSPS   bandwidth   %d-QAM PHY rate\n", 1 << M);
    for (int j = 0; j < nth; j++) {
        int T = NTH[j];
        pthread_t th[32];
        struct targ a[32];
        atomic_store(&go, 0);
        for (int t = 0; t < T; t++) {
            a[t].cpu = t;
            pthread_create(&th[t], NULL, worker, &a[t]);
        }
        atomic_store(&go, 1);
        long tot = 0;
        for (int t = 0; t < T; t++) {
            pthread_join(th[t], NULL);
            tot += a[t].frames;
        }
        double fps = tot / SECS, sps = fps * OFDM_FRAME;
        printf("%7d  %9.0f   %10.1f   %24.2f %%   %6.1f MHz   %8.2f Gb/s\n", T, fps, sps / 1e6,
               100 * sps / fs, sps * occ / 1e6, ofdm_rate_bps(M, sps) / 1e9);
    }
    ofdm_free(c);
    return 0;
}
