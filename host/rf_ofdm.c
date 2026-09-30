/*
 * rf_ofdm - continuous I/Q OFDM through the RFSoC 4x2 at 2 GSPS, modulated and demodulated on the
 * host CPU, samples carried by 100G RDMA (ERNIC + rf_stream).
 *
 * TX: raw video (a synthetic test pattern, or a .yuv file) is cut into OFDM frame payloads with a
 *     sequence number; TX threads modulate frames into a host frame ring; the RDMA engine writes
 *     them into the FPGA TX ring as its read pointer advances, and the DACs play them.
 * RX: the FPGA streams the ADC samples into a host ring (mapped twice, so frames can run past its
 *     end); after one acquisition (frame search) the frame positions are fixed, since the ADCs and
 *     DACs share the sample clock, and RX threads demodulate every frame at its known position.
 *     A checker thread takes the payloads in order, checks sequence numbers and bit errors against
 *     the source, reassembles video frames and can save some of them (raw YUV 4:2:0).
 *
 *   rf_ofdm [--m 4] [--rms 0.18] [--seconds 10] [--tx-threads 3] [--rx-threads 12]
 *           [--video WxH | --yuv FILE --size WxH] [--vframes 30]
 *           [--save FILE --save-frames N --save-every K (every K-th received video frame)]
 *           [--dump FILE --dump-frames K (raw ADC samples of K frames after the first lock, for
 *            host/ofdm_plots: spectrum and constellation)]
 *           [--dev NAME] [--params FILE] [--sim 1]
 *
 * --sim 1: no board; the TX stream comes back delayed by 12344 samples with Q inverted (as ADC_D),
 *          at whatever rate the threads manage (tests everything but the RDMA and the RF);
 *          --sim-slip N delays it by 8 N samples more every second (as a TX underflow does).
 *
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#define _GNU_SOURCE
#include "ofdm_modem.h"
#include "rdma_link.h"

#include <arpa/inet.h>
#include <immintrin.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAME_B     (OFDM_FRAME * 4)                /* one OFDM frame in the stream format */
#define NTX         128                             /* host TX frame ring (16 MB) */
#define RING_B      ((size_t)RL_SLOTS * RL_CHUNK)   /* host RX ring (64 MB = 512 frames) */
#define CH_SAMP     (RL_CHUNK / 4)                  /* samples per RX chunk */
#define NOUT        256                             /* demodulated payloads waiting for the checker */
#define PIECE       (64u << 10)
#define MAGIC       0x4D44464Fu                     /* "OFDM" */
#define HDR         8                               /* magic, sequence number */

static struct {
    const char *dev, *params, *yuv, *save, *dump;
    int m, tx_threads, rx_threads, w, h, vframes, save_frames, save_every, dump_frames, sim, sim_free, sim_slip;
    double rms, seconds;
} O = {.params = "stream_params.txt", .m = 4, .tx_threads = 3, .rx_threads = 12, .w = 1280, .h = 720,
       .vframes = 30, .save_every = 1, .dump_frames = 64, .rms = 0.18, .seconds = 10};

static int FB, PL;                          /* payload bytes per OFDM frame (with header), video bytes */
static size_t VF, VB;                       /* bytes per video frame, per video loop */
static uint8_t *video;                      /* VB bytes, followed by one more OFDM payload (wrap) */
static uint8_t *scr;                        /* scrambler: every payload is XORed with this sequence,
                                               so that constant video (flat areas) does not put the
                                               same point on every sub-carrier (a huge peak) */
static double gain;

static uint8_t *txr;                        /* NTX frames */
static _Atomic uint64_t tx_ready[NTX];      /* frame f in slot f % NTX when tx_ready = f + 1 */
static _Atomic uint64_t tx_done;            /* frames whose RDMA writes completed (or skipped) */
static _Atomic uint64_t tx_need;            /* the next frame the RDMA engine sends */
static _Atomic uint32_t rx_over_seen;       /* FPGA RX overflow count (512-bit words = 16 samples
                                               each dropped), from the engine's status reads */
static uint8_t *rxr;                        /* mirrored RX ring */
static _Atomic uint64_t rx_chunks;          /* chunks arrived, in order */
static uint64_t rx_slot0;                   /* ring slot of the first chunk (ERNIC's SQ index) */
static _Atomic int stop;
static double t_start;                      /* stream start (TX / RX on) */

/* frame lock: in lock generation g, frame j (j >= jstart) has its first training body at stream
   sample S0 + j * FRAME. A TX underflow shifts the frames; the checker then locks again (g + 1). */
struct lock { int64_t S0, jstart; int q_sign; };
static struct lock locks[4];                /* generation g in locks[g % 4] */
static _Atomic uint64_t lock_gen;           /* 0: not locked yet */
static uint8_t *outb;                       /* NOUT payloads of FB + 8 bytes */
static _Atomic uint64_t out_tag[NOUT];      /* payload j of generation g: out_tag[j % NOUT] = TAG(g, j) */
static _Atomic uint8_t out_late[NOUT];
static _Atomic int64_t next_j;              /* the frame the checker waits for */
#define TAG(g, j) (((uint64_t)(g) << 40) | (uint64_t)((j) + 1))

/* checker statistics */
static _Atomic uint64_t st_frames, st_bad_hdr, st_late, st_bit_err, st_bits, st_vframes, st_seq_err, st_missing, st_relock, st_shift, st_vintact;

/* waiting threads spin briefly, then sleep: CPUs left free for the rest of the system keep it off
   the RDMA engine's CPU (a preempted engine starves the FPGA TX ring) */
static inline void backoff(int *n)
{
    if (++*n < 200) _mm_pause();
    else usleep(20);
}

static void pin(int cpu)
{
    prctl(PR_SET_TIMERSLACK, 1000UL);       /* 1 us: short sleeps stay short */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu % CPU_SETSIZE, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

/* ------------------------------------------------------------------ video source */
static void make_video(void)
{
    VF = (size_t)O.w * O.h * 3 / 2;
    if (O.yuv) {
        FILE *f = fopen(O.yuv, "rb");
        if (!f) rl_die(O.yuv);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        O.vframes = (int)(n / (long)VF);
        if (O.vframes < 1) { fprintf(stderr, "%s: shorter than one %dx%d frame\n", O.yuv, O.w, O.h); exit(1); }
        VB = VF * O.vframes;
        video = rl_huge_alloc(VB + FB);
        if (fread(video, 1, VB, f) != VB) rl_die("read video");
        fclose(f);
    } else {
        VB = VF * O.vframes;
        video = rl_huge_alloc(VB + FB);
        for (int v = 0; v < O.vframes; v++) {
            uint8_t *Y = video + v * VF, *U = Y + O.w * O.h, *V = U + O.w * O.h / 4;
            int bx = (int)((O.w - O.h / 4) * (0.5 + 0.5 * sin(2 * M_PI * v / O.vframes)));
            int by = (int)((O.h - O.h / 4) * (0.5 + 0.5 * cos(2 * M_PI * v / O.vframes)));
            for (int y = 0; y < O.h; y++)
                for (int x = 0; x < O.w; x++) {
                    int in = x >= bx && x < bx + O.h / 4 && y >= by && y < by + O.h / 4;
                    Y[y * O.w + x] = in ? 235 : (uint8_t)(16 + ((x * 8 / O.w) * 26 + (y + 4 * v) % 64 / 8));
                }
            for (int y = 0; y < O.h / 2; y++)
                for (int x = 0; x < O.w / 2; x++) {
                    U[y * O.w / 2 + x] = (uint8_t)(128 + 100 * sin(2 * M_PI * (x * 2.0 / O.w + (double)v / O.vframes)));
                    V[y * O.w / 2 + x] = (uint8_t)(128 + 100 * cos(2 * M_PI * (y * 2.0 / O.h)));
                }
        }
    }
    memcpy(video + VB, video, FB);          /* a payload that runs past the end wraps */
}

static inline const uint8_t *payload_src(uint64_t seq) { return video + (seq * (uint64_t)PL) % VB; }

static void make_scrambler(void)
{
    scr = rl_huge_alloc(FB + 64);
    uint32_t x = 0x9E3779B9u;
    for (int i = 0; i < FB; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        scr[i] = (uint8_t)x;
    }
}

static inline void xor_bytes(uint8_t *d, const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) d[i] = a[i] ^ b[i];
}

/* ------------------------------------------------------------------ TX threads */
struct targ { int id, cpu; double t_work, t_copy, t_wait_data, t_wait_out; long n; };

static void *tx_thread(void *arg)
{
    struct targ *t = arg;
    pin(t->cpu);
    ofdm_ctx *c = ofdm_new();
    uint8_t *bits = aligned_alloc(64, (FB + 64 + 63) & ~63);
    memset(bits, 0, FB + 64);
    for (uint64_t f = t->id; !atomic_load_explicit(&stop, memory_order_relaxed); f += O.tx_threads) {
        /* the engine skipped ahead (the FPGA played on without data): so do we */
        uint64_t need = atomic_load_explicit(&tx_need, memory_order_relaxed);
        if (f < need) f += (need - f + O.tx_threads - 1) / O.tx_threads * O.tx_threads;
        /* the producers spin (they are few): on the board, sleeping producers (even with the
           "performance" governor) gave steady TX underflows */
        while (f >= atomic_load_explicit(&tx_done, memory_order_acquire) + NTX)
            if (atomic_load_explicit(&stop, memory_order_relaxed)) goto out; else _mm_pause();
        uint32_t h[2] = {MAGIC, (uint32_t)f};
        xor_bytes(bits, (const uint8_t *)h, scr, HDR);
        xor_bytes(bits + HDR, payload_src(f), scr + HDR, PL);
        ofdm_mod_stream(c, O.m, gain, bits, (int16_t *)(txr + (f % NTX) * FRAME_B));
        atomic_store_explicit(&tx_ready[f % NTX], f + 1, memory_order_release);
    }
out:
    ofdm_free(c);
    free(bits);
    return NULL;
}

/* ------------------------------------------------------------------ RX threads */
static void *rx_thread(void *arg)
{
    struct targ *t = arg;
    pin(t->cpu);
    _mm_setcsr(_mm_getcsr() | 0x8040);     /* flush denormals to zero: garbage frames stay fast */
    ofdm_ctx *c = ofdm_new();
    int16_t *fbuf = rl_huge_alloc(FRAME_B + 2048);
    uint64_t g = 0;
    struct lock lk = {0, 0, 1};
    int64_t j = 0;
    #define STOP_OR_RELOCK() if (atomic_load_explicit(&stop, memory_order_relaxed)) goto out; \
        if (atomic_load_explicit(&lock_gen, memory_order_acquire) != g) continue
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        uint64_t cg = atomic_load_explicit(&lock_gen, memory_order_acquire);
        if (!cg) { usleep(100); continue; }
        if (cg != g) { g = cg; lk = locks[g % 4]; j = lk.jstart + t->id; }
        int64_t s = lk.S0 + j * OFDM_FRAME;                 /* first training body */
        int64_t b = (s - 256) & ~(int64_t)7;                /* start of the samples handed over */
        int64_t end = b + OFDM_FRAME + 512;                 /* past the last data symbol */
        double ta = rl_now();
        int ready = 0, bo = 0;
        while (!ready) {
            ready = (int64_t)atomic_load_explicit(&rx_chunks, memory_order_acquire) * CH_SAMP >= end;
            if (!ready) { if (atomic_load_explicit(&stop, memory_order_relaxed)) goto out; backoff(&bo); }
            if (atomic_load_explicit(&lock_gen, memory_order_relaxed) != g) break;
        }
        STOP_OR_RELOCK();
        double tb = rl_now();
        bo = 0;
        while (j >= atomic_load_explicit(&next_j, memory_order_acquire) + NOUT) {
            if (atomic_load_explicit(&stop, memory_order_relaxed)) goto out;
            if (atomic_load_explicit(&lock_gen, memory_order_relaxed) != g) break;
            backoff(&bo);
        }
        STOP_OR_RELOCK();
        double tc = rl_now();
        t->t_wait_data += tb - ta; t->t_wait_out += tc - tb;
        uint8_t *o = outb + (size_t)(j % NOUT) * (FB + 8);
        /* one copy out of the ring first (the ring may be overwritten while a frame is worked on) */
        memcpy(fbuf, rxr + (rx_slot0 * RL_CHUNK + (size_t)b * 4) % RING_B, FRAME_B + 2048);
        uint64_t now_ch = atomic_load_explicit(&rx_chunks, memory_order_acquire);
        t->t_copy += rl_now() - tc;
        ofdm_demod_stream_mem(c, O.m, fbuf, (int)(s - b), lk.q_sign, o);
        STOP_OR_RELOCK();
        atomic_store_explicit(&out_late[j % NOUT], now_ch - (uint64_t)(b / CH_SAMP) >= RL_SLOTS - 1, memory_order_relaxed);
        atomic_store_explicit(&out_tag[j % NOUT], TAG(g, j), memory_order_release);
        t->t_work += rl_now() - tc; t->n++;
        j += O.rx_threads;
    }
out:
    ofdm_free(c);
    return NULL;
}

/* ------------------------------------------------------------------ acquisition and checker */
/* raw samples of O.dump_frames frames from lk->jstart on, in the stream memory format, after a
   header: "OFDMDUMP", m, p (first training body, in samples from the start of the data), Q sign,
   frames, samples per frame */
static void dump_frames(const struct lock *lk)
{
    int64_t s0 = lk->S0 + lk->jstart * OFDM_FRAME, b = (s0 - 256) & ~(int64_t)7;
    size_t bytes = (size_t)O.dump_frames * FRAME_B + 4096;
    if (bytes > RING_B / 2) { fprintf(stderr, "dump: at most %zu frames\n", RING_B / 2 / FRAME_B); return; }
    while ((int64_t)atomic_load(&rx_chunks) * CH_SAMP < b + (int64_t)(bytes / 4))
        if (atomic_load(&stop)) return; else usleep(100);
    uint8_t *buf = malloc(bytes);
    memcpy(buf, rxr + (rx_slot0 * RL_CHUNK + (size_t)b * 4) % RING_B, bytes);
    if (atomic_load(&rx_chunks) - (uint64_t)(b / CH_SAMP) >= RL_SLOTS - 1) {
        fprintf(stderr, "dump: overwritten while copied\n");
        free(buf);
        return;
    }
    FILE *f = fopen(O.dump, "wb");
    if (f) {
        int32_t h[6] = {O.m, (int32_t)(s0 - b), lk->q_sign, O.dump_frames, OFDM_FRAME, 0};
        fwrite("OFDMDUMP", 1, 8, f);
        fwrite(h, sizeof(h), 1, f);
        fwrite(buf, 1, bytes, f);
        fclose(f);
        printf("dump: %d frames of raw samples to %s\n", O.dump_frames, O.dump);
    }
    free(buf);
}

/* frame search on the 4 latest complete chunks (2 frames); fills *lk, returns 0 when found */
static int acquire(ofdm_ctx *c, int16_t *ci, int16_t *cq, struct lock *lk)
{
    uint64_t c0 = atomic_load(&rx_chunks) - 4;
    const int16_t *m = (const int16_t *)(rxr + ((rx_slot0 + c0) * RL_CHUNK) % RING_B);
    for (int n = 0; n < 2 * OFDM_FRAME; n++) {
        ci[n] = m[(n >> 3) * 16 + (n & 7)];
        cq[n] = m[(n >> 3) * 16 + 8 + (n & 7)];
    }
    if (atomic_load(&rx_chunks) >= c0 + RL_SLOTS - 1)
        return -1;                                  /* overwritten while copied */
    ofdm_result r;
    int bad = ofdm_demod(c, O.m, ci, cq, 2 * OFDM_FRAME, 1, &r);
    printf("acquisition: frame %s at sample %d of chunk %" PRIu64 ", Q sign %+d lag %+d\n",
           bad ? "NOT found" : "found", r.frame_pos, c0, r.q_sign, r.q_lag);
    if (r.q_lag)
        printf("acquisition: warning, Q lags I by %d samples (the streaming receiver assumes 0)\n", r.q_lag);
    fflush(stdout);
    lk->q_sign = r.q_sign;
    lk->S0 = (int64_t)c0 * CH_SAMP + r.frame_pos;
    lk->jstart = ((int64_t)atomic_load(&rx_chunks) * CH_SAMP - lk->S0) / OFDM_FRAME + 2;
    return bad;
}

static void *checker(void *arg)
{
    pin(*(int *)arg);
    ofdm_ctx *c = ofdm_new();
    int16_t *ci = malloc(2 * OFDM_FRAME * 2), *cq = malloc(2 * OFDM_FRAME * 2);
    while (atomic_load(&rx_chunks) < 64)
        if (atomic_load(&stop)) return NULL; else usleep(100);
    FILE *sv = O.save ? fopen(O.save, "wb") : NULL, *sm = NULL;
    int saved = 0;
    if (sv) {
        /* next to the received frames: the source loop (<save>.src) and per frame "index intact" */
        char pth[600];
        snprintf(pth, sizeof(pth), "%s.src", O.save);
        FILE *f = fopen(pth, "wb");
        if (f) { fwrite(video, 1, VB, f); fclose(f); }
        snprintf(pth, sizeof(pth), "%s.txt", O.save);
        sm = fopen(pth, "w");
    }
    uint8_t *rv = rl_huge_alloc(VB + FB);                   /* received video, one loop */
    uint32_t *got = calloc(O.vframes, sizeof(uint32_t));    /* bytes of each video frame this pass */
    uint64_t *got_pass = calloc(O.vframes, sizeof(uint64_t));
    int64_t seq0 = -1, j = 0;
    uint64_t g = 0;
    uint32_t over_lock = 0;
    int bad_run = 1 << 20;                                  /* start by locking */
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        uint32_t over = atomic_load_explicit(&rx_over_seen, memory_order_relaxed);
        if (g && bad_run >= 4 && over != over_lock) {
            /* the FPGA dropped RX words: every later frame is 16 samples per word earlier */
            struct lock lk = locks[g % 4];
            lk.S0 -= 16 * (int64_t)(uint32_t)(over - over_lock);
            lk.jstart = ((int64_t)atomic_load(&rx_chunks) * CH_SAMP - lk.S0) / OFDM_FRAME + 2;
            over_lock = over;
            atomic_fetch_add(&st_shift, 1);
            g++;
            locks[g % 4] = lk;
            j = lk.jstart;
            atomic_store_explicit(&next_j, j, memory_order_release);
            atomic_store_explicit(&lock_gen, g, memory_order_release);
            seq0 = -1;
            bad_run = 0;
            continue;
        }
        if (bad_run >= 32) {
            /* (re)lock: frame search on the newest samples */
            struct lock lk;
            over_lock = atomic_load_explicit(&rx_over_seen, memory_order_relaxed);
            if (acquire(c, ci, cq, &lk)) { usleep(1000); continue; }
            if (g) atomic_fetch_add(&st_relock, 1);
            g++;
            locks[g % 4] = lk;
            j = lk.jstart;
            atomic_store_explicit(&next_j, j, memory_order_release);
            atomic_store_explicit(&lock_gen, g, memory_order_release);
            seq0 = -1;
            bad_run = 0;
            if (g == 1 && O.dump) dump_frames(&lk);
        }
        /* wait for frame j; give up on it when the stream is well past it (a worker dropped it) */
        struct lock *lk = &locks[g % 4];
        double tw = 0;
        int missing = 0, bo = 0;
        while (atomic_load_explicit(&out_tag[j % NOUT], memory_order_acquire) != TAG(g, j)) {
            if (atomic_load_explicit(&stop, memory_order_relaxed)) goto out;
            if ((int64_t)atomic_load(&rx_chunks) * CH_SAMP > lk->S0 + (j + 120) * OFDM_FRAME) {
                if (tw == 0) tw = rl_now();
                else if (rl_now() - tw > 0.02) { missing = 1; break; }
            }
            backoff(&bo);
        }
        if (missing) {
            atomic_fetch_add(&st_missing, 1);
            bad_run++;
            atomic_store_explicit(&next_j, ++j, memory_order_release);
            continue;
        }
        uint8_t *o = outb + (size_t)(j % NOUT) * (FB + 8);
        xor_bytes(o, o, scr, FB);
        uint32_t h[2];
        memcpy(h, o, HDR);
        if (atomic_load_explicit(&out_late[j % NOUT], memory_order_relaxed)) atomic_fetch_add(&st_late, 1);
        uint64_t seq;
        if (h[0] == MAGIC) {
            if (seq0 < 0) seq0 = (int64_t)h[1] - j;
            seq = (uint64_t)(seq0 + j);
            if ((uint32_t)seq != h[1]) atomic_fetch_add(&st_seq_err, 1);
            bad_run = 0;
        } else {
            atomic_fetch_add(&st_bad_hdr, 1);
            seq = seq0 >= 0 ? (uint64_t)(seq0 + j) : 0;
            bad_run++;
        }
        /* bit errors against the source */
        const uint8_t *ref = payload_src(seq), *pay = o + HDR;
        uint64_t e = 0;
        int i = 0;
        for (; i + 8 <= PL; i += 8) {
            uint64_t x, y;
            memcpy(&x, pay + i, 8);
            memcpy(&y, ref + i, 8);
            e += (uint64_t)__builtin_popcountll(x ^ y);
        }
        for (; i < PL; i++) e += (uint64_t)__builtin_popcount(pay[i] ^ ref[i]);
        atomic_fetch_add(&st_bit_err, e);
        atomic_fetch_add(&st_bits, (uint64_t)PL * 8);
        atomic_fetch_add(&st_frames, 1);
        /* reassemble video: the payload covers bytes [off, off + PL) of loop pass seq * PL / VB */
        if (h[0] == MAGIC) {
            uint64_t pos = seq * (uint64_t)PL, pass = pos / VB;
            size_t off = pos % VB;
            memcpy(rv + off, pay, PL);
            if (off + PL > VB) memcpy(rv, pay + (VB - off), off + PL - VB);
            for (size_t a = off; a < off + PL;) {
                size_t v = (a / VF) % O.vframes, lim = (a / VF + 1) * VF;
                size_t n = (off + PL < lim ? off + PL : lim) - a;
                uint64_t p = pass + (a >= VB);
                if (got_pass[v] != p) { got_pass[v] = p; got[v] = 0; }
                got[v] += (uint32_t)n;
                if (got[v] == VF) {
                    atomic_fetch_add(&st_vframes, 1);
                    int intact = !memcmp(rv + v * VF, video + v * VF, VF);
                    if (intact) atomic_fetch_add(&st_vintact, 1);
                    if (sv && saved < O.save_frames && st_vframes % O.save_every == 0) {
                        fwrite(rv + v * VF, 1, VF, sv);
                        /* index, intact, time, video frames (intact), OFDM frames, bit errors / bits */
                        if (sm) fprintf(sm, "%zu %d %.3f %lu %lu %lu %lu %lu\n", v, intact, rl_now() - t_start,
                                        (unsigned long)st_vframes, (unsigned long)st_vintact, (unsigned long)st_frames,
                                        (unsigned long)st_bit_err, (unsigned long)st_bits);
                        saved++;
                    }
                }
                a += n;
            }
        }
        atomic_store_explicit(&next_j, ++j, memory_order_release);
    }
out:
    if (sm) fclose(sm);
    if (sv) { fclose(sv); printf("saved %d video frames (%dx%d yuv420p) to %s\n", saved, O.w, O.h, O.save); }
    ofdm_free(c);
    return NULL;
}

/* ------------------------------------------------------------------ threads */
static pthread_t th[64], chk;
static struct targ ta[64];
static int nth, chk_cpu;

/* CPUs in order of use: engine, checker, then TX and RX threads (Core Ultra 7 265K: P-cores 0-7,
   E-cores 8-19; the engine on a P-core away from CPU 0, where most interrupts go); --cpus overrides */
static int cpus[64] = {7, 19, 6, 5, 4, 3, 2, 1, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 0}, ncpus = 20;

static void start_threads(void)
{
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN), k = 0;
    #define NEXT_CPU() (cpus[k++ % ncpus] % ncpu)
    pin(NEXT_CPU());
    struct sched_param sp = {.sched_priority = 10};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0)
        printf("RDMA engine: SCHED_FIFO\n");
    chk_cpu = NEXT_CPU();
    for (int i = 0; i < O.tx_threads; i++, nth++) {
        ta[nth] = (struct targ){.id = i, .cpu = NEXT_CPU()};
        pthread_create(&th[nth], NULL, tx_thread, &ta[nth]);
    }
    for (int i = 0; i < O.rx_threads; i++, nth++) {
        ta[nth] = (struct targ){.id = i, .cpu = NEXT_CPU()};
        pthread_create(&th[nth], NULL, rx_thread, &ta[nth]);
    }
}

static void stop_threads(void)
{
    atomic_store(&stop, 1);
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    pthread_join(chk, NULL);
    if (getenv("THREAD_STATS"))
        for (int i = O.tx_threads; i < nth; i++)
            printf("RX thread %d (CPU %d): %ld frames, per frame %.1f us copy + %.1f us demodulation, waiting %.2f s for data, %.2f s for the checker\n",
                   ta[i].id, ta[i].cpu, ta[i].n, ta[i].n ? ta[i].t_copy / ta[i].n * 1e6 : 0.0,
                   ta[i].n ? (ta[i].t_work - ta[i].t_copy) / ta[i].n * 1e6 : 0.0, ta[i].t_wait_data, ta[i].t_wait_out);
}

/* CPU package temperature (C) and thermal throttle events, -1 when unavailable */
static long sysfs_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}
static const char *pkg_temp_path(void)
{
    static char p[128];
    for (int i = 0; i < 32 && !p[0]; i++) {
        char t[128], type[32] = "";
        snprintf(t, sizeof(t), "/sys/class/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(t, "r");
        if (!f) continue;
        if (fscanf(f, "%31s", type) == 1 && !strcmp(type, "x86_pkg_temp"))
            snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
        fclose(f);
    }
    return p;
}

static void print_stats(double t, double dt, double tx_b, double rx_b, uint64_t under, uint64_t over)
{
    static uint64_t l_fr, l_be, l_bits, l_vf;
    uint64_t fr = st_frames, be = st_bit_err, bits = st_bits, vf = st_vframes, db = bits - l_bits;
    /* only with CPU_TEMP set: on the RDMA engine's thread these sysfs reads (MSRs) cost TX
       underflows */
    static long thr0 = -2;
    int want = getenv("CPU_TEMP") != NULL;
    long temp = want ? sysfs_long(pkg_temp_path()) : -1000;
    long thr = want ? sysfs_long("/sys/devices/system/cpu/cpu0/thermal_throttle/package_throttle_count") : -1;
    if (thr0 == -2) thr0 = thr;
    printf("%3.0f   %9.2f  %9.2f  %8.0f   %-9.2e  %7" PRIu64 "  %4" PRIu64 "  %6" PRIu64 "  %9.1f  %12" PRIu64 "  %11" PRIu64 "  %3ld C %5ld\n",
           t, tx_b * 8 / dt / 1e9, rx_b * 8 / dt / 1e9, (fr - l_fr) / dt, db ? (double)(be - l_be) / db : 0.0,
           (uint64_t)st_bad_hdr, (uint64_t)st_late, (uint64_t)st_relock, (vf - l_vf) / dt, under, over,
           temp / 1000, thr >= 0 ? thr - thr0 : -1);
    fflush(stdout);
    l_fr = fr; l_be = be; l_bits = bits; l_vf = vf;
}

/* ------------------------------------------------------------------ simulation (no board) */
static int simulate(void)
{
    int64_t D = 12344;                          /* delay, samples (a multiple of 8) */
    start_threads();
    pthread_create(&chk, NULL, checker, &chk_cpu);
    uint64_t c = 0, l_c = 0;
    double t0 = rl_now(), tlast = t0;
    printf("  t   TX Gbit/s  RX Gbit/s  frames/s   BER        bad hdr  late  relock  video fps  (simulation)\n");
    while (rl_now() - t0 < O.seconds) {
        /* RX chunk c = TX samples [c * CH_SAMP - D, ...) */
        int64_t t = (int64_t)c * CH_SAMP - D;
        int16_t *dst = (int16_t *)(rxr + (c * RL_CHUNK) % RING_B);
        int ok = 1;
        uint64_t done = 0;                      /* frames finished in this chunk (published with it) */
        for (int64_t n = 0; n < CH_SAMP; n += 8) {
            int64_t ts = t + n;
            int16_t *d = dst + n * 2;
            if (ts < 0) { memset(d, 0, 32); continue; }
            uint64_t f = (uint64_t)ts / OFDM_FRAME;
            if (atomic_load_explicit(&tx_ready[f % NTX], memory_order_acquire) != f + 1) { ok = 0; break; }
            const int16_t *s = (const int16_t *)(txr + (f % NTX) * FRAME_B) + (ts % OFDM_FRAME) * 2;
            for (int j = 0; j < 8; j++) { d[j] = s[j]; d[8 + j] = (int16_t)-s[8 + j]; }
            if ((ts + 8) % OFDM_FRAME == 0) done = f + 1;
        }
        if (ok) {
            c++;
            atomic_store_explicit(&rx_chunks, c, memory_order_release);
            if (done) atomic_store_explicit(&tx_done, done, memory_order_release);
        }
        static double t_ok;
        if (ok) t_ok = rl_now();
        else if (getenv("WATCHDOG") && t_ok && rl_now() - t_ok > 0.5) {
            uint64_t f = (uint64_t)(t < 0 ? 0 : t) / OFDM_FRAME;
            printf("stalled: chunk %lu, needs TX frame %lu (slot has %lu), tx_done %lu, lock %lu, next_j %ld\n",
                   (unsigned long)c, (unsigned long)f, (unsigned long)tx_ready[f % NTX], (unsigned long)tx_done,
                   (unsigned long)lock_gen, (long)next_j);
            t_ok = rl_now();
        }
        /* slow receivers: at most 3/4 of the ring ahead of the checker (the board does not wait) */
        uint64_t g;
        while (!O.sim_free && (g = atomic_load(&lock_gen)) && !atomic_load(&stop) &&
               (int64_t)c * CH_SAMP > locks[g % 4].S0 + atomic_load(&next_j) * OFDM_FRAME + (int64_t)(RING_B / 4) * 3 / 4 &&
               rl_now() - t0 < O.seconds)
            _mm_pause();
        double tn = rl_now();
        if (tn - tlast >= 1.0) {
            if (O.sim_slip) D += 8 * O.sim_slip;     /* as a TX underflow: later frames arrive later */
            print_stats(tn - t0, tn - tlast, (double)(c - l_c) * RL_CHUNK, (double)(c - l_c) * RL_CHUNK, 0, 0);
            l_c = c; tlast = tn;
        }
    }
    stop_threads();
    printf("total: %" PRIu64 " frames checked, %" PRIu64 " bit errors / %" PRIu64 ", %" PRIu64 " bad headers, %" PRIu64
           " sequence errors, %" PRIu64 " late, %" PRIu64 " missing, %" PRIu64 " relocks, %" PRIu64 " video frames\n",
           (uint64_t)st_frames, (uint64_t)st_bit_err, (uint64_t)st_bits, (uint64_t)st_bad_hdr, (uint64_t)st_seq_err,
           (uint64_t)st_late, (uint64_t)st_missing, (uint64_t)st_relock, (uint64_t)st_vframes);
    return 0;
}

/* ------------------------------------------------------------------ main: RDMA engine */
int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--m")) O.m = atoi(v);
        else if (!strcmp(k, "--rms")) O.rms = atof(v);
        else if (!strcmp(k, "--seconds")) O.seconds = atof(v);
        else if (!strcmp(k, "--tx-threads")) O.tx_threads = atoi(v);
        else if (!strcmp(k, "--rx-threads")) O.rx_threads = atoi(v);
        else if (!strcmp(k, "--video") || !strcmp(k, "--size")) sscanf(v, "%dx%d", &O.w, &O.h);
        else if (!strcmp(k, "--yuv")) O.yuv = v;
        else if (!strcmp(k, "--vframes")) O.vframes = atoi(v);
        else if (!strcmp(k, "--save")) O.save = v;
        else if (!strcmp(k, "--save-frames")) O.save_frames = atoi(v);
        else if (!strcmp(k, "--save-every")) O.save_every = atoi(v) > 0 ? atoi(v) : 1;
        else if (!strcmp(k, "--dump")) O.dump = v;
        else if (!strcmp(k, "--dump-frames")) O.dump_frames = atoi(v);
        else if (!strcmp(k, "--dev")) O.dev = v;
        else if (!strcmp(k, "--params")) O.params = v;
        else if (!strcmp(k, "--sim")) O.sim = atoi(v);
        else if (!strcmp(k, "--sim-free")) O.sim_free = atoi(v);
        else if (!strcmp(k, "--sim-slip")) O.sim_slip = atoi(v);
        else if (!strcmp(k, "--cpus")) {
            ncpus = 0;
            for (char *q = strtok((char *)v, ","); q && ncpus < 64; q = strtok(NULL, ",")) cpus[ncpus++] = atoi(q);
        }
        else { fprintf(stderr, "unknown option %s\n", k); return 1; }
    }
    if (O.m < 2 || O.m > 8 || O.m & 1) { fprintf(stderr, "--m must be 2, 4, 6 or 8\n"); return 1; }
    FB = ofdm_frame_bits(O.m) / 8;
    PL = FB - HDR;
    gain = ofdm_stream_gain(O.rms);
    make_scrambler();
    make_video();
    const double fs = 2.0e9, fps = fs / OFDM_FRAME;
    printf("%d-QAM, %d payload bytes per OFDM frame, %.0f frames/s = %.2f Gbit/s of video;\n"
           "video %dx%d yuv420p (%zu bytes, %d frames in the loop) = up to %.0f video frames/s\n",
           1 << O.m, PL, fps, PL * 8 * fps / 1e9, O.w, O.h, VF, O.vframes, PL * fps / VF);

    txr = rl_huge_alloc((size_t)NTX * FRAME_B);
    rxr = rl_mirror_alloc(RING_B);
    outb = rl_huge_alloc((size_t)NOUT * (FB + 8));
    if (O.sim) return simulate();
    rlink L;
    rl_open(&L, O.dev, 1000);
    uint64_t *ctl = rl_huge_alloc(4096), *sts = ctl + 64;
    struct ibv_mr *rx_mr = ibv_reg_mr(L.pd, rxr, RING_B, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct ibv_mr *tx_mr = ibv_reg_mr(L.pd, txr, (size_t)NTX * FRAME_B, IBV_ACCESS_LOCAL_WRITE);
    struct ibv_mr *c_mr = ibv_reg_mr(L.pd, ctl, 4096, IBV_ACCESS_LOCAL_WRITE);
    if (!rx_mr || !tx_mr || !c_mr) rl_die("ibv_reg_mr");

    start_threads();

    rl_connect(&L, O.params, (uint64_t)(uintptr_t)rxr, rx_mr->rkey, 2, 0x100);
    struct ibv_qp *qp = L.qp;
    #define POST(wr) do { struct ibv_send_wr *bad; if (ibv_post_send(qp, &(wr), &bad)) rl_die("ibv_post_send"); } while (0)
    struct ibv_sge sg_ctl = {.addr = (uintptr_t)ctl, .length = 16, .lkey = c_mr->lkey};
    struct ibv_sge sg_st = {.addr = (uintptr_t)sts, .length = 64, .lkey = c_mr->lkey};
    struct ibv_send_wr w_ctl = {.wr_id = 3, .sg_list = &sg_ctl, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                                .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE};
    w_ctl.wr.rdma.remote_addr = RL_CTRL_ADDR; w_ctl.wr.rdma.rkey = RL_WIN_RKEY;
    struct ibv_send_wr w_st = {.wr_id = 1, .sg_list = &sg_st, .num_sge = 1, .opcode = IBV_WR_RDMA_READ,
                               .send_flags = IBV_SEND_SIGNALED};
    w_st.wr.rdma.remote_addr = RL_STAT_ADDR; w_st.wr.rdma.rkey = RL_WIN_RKEY;

    /* reset, prefill the FPGA ring with the first frames, switch on */
    const uint64_t tx_ring = rl_status(&L, sts, c_mr->lkey);
    printf("FPGA TX ring %lu KB\n", (unsigned long)(tx_ring >> 10));
    ctl[0] = 4; ctl[1] = 0; POST(w_ctl);
    uint64_t wptr = 0;
    for (; wptr < tx_ring; wptr += PIECE) {
        uint64_t f = wptr / FRAME_B;
        while (atomic_load_explicit(&tx_ready[f % NTX], memory_order_acquire) != f + 1) _mm_pause();
        struct ibv_sge sg = {.addr = (uintptr_t)txr + wptr % ((size_t)NTX * FRAME_B), .length = PIECE, .lkey = tx_mr->lkey};
        struct ibv_send_wr w = {.wr_id = 2 | ((wptr + PIECE) << 8), .sg_list = &sg, .num_sge = 1,
                                .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED};
        w.wr.rdma.remote_addr = RL_WIN + wptr % tx_ring; w.wr.rdma.rkey = RL_WIN_RKEY;
        POST(w);
    }
    ctl[0] = 3; ctl[1] = wptr; POST(w_ctl);
    t_start = rl_now();
    pthread_create(&chk, NULL, checker, &chk_cpu);

    uint64_t rptr = 0, expect = 0, gaps = 0, tx_stall = 0, rx_n = 0, tx_skip = 0;
    int stat_pending = 0;
    double t0 = rl_now(), tlast = t0;
    uint64_t l_tx = 0, l_rx = 0;
    printf("  t   TX Gbit/s  RX Gbit/s  frames/s   BER        bad hdr  late  relock  video fps  TX underflow  RX overflow  CPU  throttle\n");
    double gap_max = 0, t_loop = rl_now();
    while (rl_now() - t0 < O.seconds) {
        double tl = rl_now();
        if (tl - t_loop > gap_max) gap_max = tl - t_loop;
        t_loop = tl;
        if (!stat_pending) { POST(w_st); stat_pending = 1; }
        /* the FPGA read pointer keeps time (it plays zeros for words not written): when it has
           passed the write pointer, skip ahead of it; the frames in between are lost, the frame
           grid stays */
        if ((int64_t)(wptr - rptr) < 0) {
            wptr = (rptr + (256u << 10) + PIECE - 1) & ~(uint64_t)(PIECE - 1);
            tx_skip++;
            /* frames before the new one are not needed; the slots of the frames still in flight
               (at most the FPGA ring's, 16 of 2 MB) stay untouched */
            uint64_t fn = wptr / FRAME_B, keep = tx_ring / FRAME_B + 8;
            atomic_store_explicit(&tx_need, fn, memory_order_relaxed);
            if (fn + keep > NTX && fn + keep - NTX > atomic_load(&tx_done))
                atomic_store_explicit(&tx_done, fn + keep - NTX, memory_order_release);
        }
        if ((int64_t)(wptr + PIECE - rptr) <= (int64_t)tx_ring) {
            uint64_t f = wptr / FRAME_B;
            if (atomic_load_explicit(&tx_ready[f % NTX], memory_order_acquire) == f + 1) {
                struct ibv_sge sg = {.addr = (uintptr_t)txr + wptr % ((size_t)NTX * FRAME_B), .length = PIECE, .lkey = tx_mr->lkey};
                struct ibv_send_wr w = {.wr_id = 2 | ((wptr + PIECE) << 8), .sg_list = &sg, .num_sge = 1,
                                        .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED};
                w.wr.rdma.remote_addr = RL_WIN + wptr % tx_ring; w.wr.rdma.rkey = RL_WIN_RKEY;
                POST(w);
                wptr += PIECE;
                ctl[1] = wptr; POST(w_ctl);     /* inline: the value is copied now */
            } else
                tx_stall++;
        }
        struct ibv_wc wc[64];
        int n = ibv_poll_cq(L.scq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "send wr 0x%" PRIx64 ": %s (0x%x)\n", wc[i].wr_id, ibv_wc_status_str(wc[i].status), wc[i].vendor_err);
                exit(1);
            }
            if (wc[i].wr_id == 1) {
                rptr = rl_rptr64(sts[0], wptr);
                stat_pending = 0;
                atomic_store_explicit(&rx_over_seen, (uint32_t)sts[4], memory_order_relaxed);
            }
            else if ((wc[i].wr_id & 0xFF) == 2) {
                uint64_t d = (wc[i].wr_id >> 8) / FRAME_B;
                if (d > atomic_load_explicit(&tx_done, memory_order_relaxed))
                    atomic_store_explicit(&tx_done, d, memory_order_release);
            }
        }
        n = ibv_poll_cq(L.rcq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "recv: %s (0x%x)\n", ibv_wc_status_str(wc[i].status), wc[i].vendor_err);
                exit(1);
            }
            uint32_t k = ntohl(wc[i].imm_data);
            if (!rx_n) expect = rx_slot0 = k;       /* runs start at ERNIC's SQ index */
            if (k != expect % RL_SLOTS) gaps++;
            expect = k + 1;
            rx_n++;
            struct ibv_recv_wr r = {.wr_id = k}, *bad;
            if (ibv_post_recv(qp, &r, &bad)) rl_die("ibv_post_recv");
        }
        if (n) atomic_store_explicit(&rx_chunks, rx_n, memory_order_release);
        double t = rl_now();
        if (t - tlast >= 1.0) {
            print_stats(t - t0, t - tlast, (double)(wptr - l_tx), (double)(rx_n - l_rx) * RL_CHUNK, sts[1], sts[4]);
            if (getenv("ENGINE_STATS")) printf("      engine: longest loop gap %.0f us, TX skips %lu, stalls %lu\n", gap_max * 1e6, (unsigned long)tx_skip, (unsigned long)tx_stall);
            gap_max = 0;
            if (gaps) printf("      RX gaps %" PRIu64 "\n", gaps);
            l_tx = wptr; l_rx = rx_n; tlast = t;
        }
    }
    for (double td = rl_now(); stat_pending && rl_now() - td < 0.5;) {
        struct ibv_wc wc[64];
        int n = ibv_poll_cq(L.scq, 64, wc);
        for (int i = 0; i < n; i++) if (wc[i].wr_id == 1) stat_pending = 0;
    }
    rl_stop(&L, ctl, c_mr->lkey, sts, c_mr->lkey, wptr);
    stop_threads();
    usleep(100000);
    printf("total: TX %.2f GB, RX %.2f GB, %" PRIu64 " RX gaps, %" PRIu64 " TX stalls (frame not ready), %" PRIu64 " TX skips; "
           "%" PRIu64 " frames checked, %" PRIu64 " bit errors / %" PRIu64 " (BER %.2e), %" PRIu64 " bad headers, "
           "%" PRIu64 " sequence errors, %" PRIu64 " late, %" PRIu64 " missing, %" PRIu64 " relocks, %" PRIu64 " shifts, %" PRIu64 " video frames (%" PRIu64 " byte-exact); FPGA: TX underflow %" PRIu64
           ", RX overflow %" PRIu64 "\n",
           wptr / 1e9, rx_n * (double)RL_CHUNK / 1e9, gaps, tx_stall, tx_skip, (uint64_t)st_frames, (uint64_t)st_bit_err,
           (uint64_t)st_bits, st_bits ? (double)st_bit_err / st_bits : 0.0, (uint64_t)st_bad_hdr,
           (uint64_t)st_seq_err, (uint64_t)st_late, (uint64_t)st_missing, (uint64_t)st_relock, (uint64_t)st_shift, (uint64_t)st_vframes, (uint64_t)st_vintact, sts[1], sts[4]);
    return 0;
}
