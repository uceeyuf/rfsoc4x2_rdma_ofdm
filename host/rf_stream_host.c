// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Yijie Yu
//
// rf_stream_host - continuous I/Q streaming with the RFSoC 4x2 over 100G RDMA (ERNIC).
//
// TX: a waveform (int16 I / Q, looped) is written into rf_stream's 2 MB TX ring by RDMA WRITE,
//     paced by the ring's read pointer (RDMA READ of the status word); each write is followed by
//     an 8-byte write of the new write pointer, which RC ordering places after the data.
// RX: the FPGA sends every 64 KB chunk of ADC samples as an RDMA WRITE WITH IMMEDIATE into a host
//     ring of 256 slots (immediate = slot); the program checks that the slots arrive in order.
//
// Memory format of the samples (TX and RX): per 64 bytes 8 I, 8 Q, 8 I, 8 Q (int16).
//
//   rf_stream_host [--dev NAME] [--seconds S] [--tx tone:MHz | off] [--amp 0.25] [--rx on|off]
//                  [--save FILE --save-chunks N] [--params FILE]
//
// Connects like rdma_loopback: writes its QP number, MAC, PSN and the RX ring VA / R_Key to the
// params file and waits for "<params>.ready" from tests/stream_config.tcl.

#include "rdma_link.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN          RL_WIN
#define WIN_RKEY     RL_WIN_RKEY
static uint32_t TX_RING = RL_TX_RING;            // from the FPGA status word
#define CTRL_ADDR    RL_CTRL_ADDR
#define STAT_ADDR    RL_STAT_ADDR
#define CHUNK        RL_CHUNK
#define SLOTS        RL_SLOTS
#define TX_PIECE     (64u << 10)
#define FS           2.0e9
#define now          rl_now
#define die          rl_die

static struct {
    const char *dev, *params, *save;
    double seconds, tone_mhz, amp;
    int tx, rx, save_chunks, fpga_qpn, src_mb;
    uint32_t psn;
} O = {.params = "stream_params.txt", .seconds = 10, .tone_mhz = 100, .amp = 0.25, .tx = 1, .rx = 1,
       .fpga_qpn = 2, .psn = 0x100};

// one waveform period that fits the ring evenly: a tone of whole cycles over TX_RING bytes
static void make_tone(int16_t *buf, size_t bytes, double mhz, double amp)
{
    size_t n = bytes / 4;                                   // complex samples
    double cycles = round(mhz * 1e6 / FS * n);
    for (size_t s = 0; s < n; s++) {
        double ph = 2 * M_PI * cycles * s / n;
        size_t blk = s / 8, j = s % 8;                      // 8 I then 8 Q per 32 bytes
        buf[blk * 16 + j]     = (int16_t)lround(amp * 32767 * cos(ph));
        buf[blk * 16 + 8 + j] = (int16_t)lround(amp * 32767 * sin(ph));
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--dev")) O.dev = v;
        else if (!strcmp(k, "--seconds")) O.seconds = atof(v);
        else if (!strcmp(k, "--tx")) { if (!strcmp(v, "off")) O.tx = 0; else if (!strncmp(v, "tone:", 5)) O.tone_mhz = atof(v + 5); }
        else if (!strcmp(k, "--amp")) O.amp = atof(v);
        else if (!strcmp(k, "--rx")) O.rx = strcmp(v, "off") != 0;
        else if (!strcmp(k, "--save")) O.save = v;
        else if (!strcmp(k, "--save-chunks")) O.save_chunks = atoi(v);
        else if (!strcmp(k, "--params")) O.params = v;
        else if (!strcmp(k, "--src-mb")) O.src_mb = atoi(v);    // TX source: N MB (tone repeated), sent in turn
        else { fprintf(stderr, "unknown option %s\n", k); return 1; }
    }

    rlink L;
    rl_open(&L, O.dev, 1000);
    struct ibv_qp *qp = L.qp;
    struct ibv_cq *scq = L.scq, *rcq = L.rcq;

    // buffers: RX ring (FPGA writes), TX waveform, control / status words
    uint8_t *rx = rl_huge_alloc((size_t)SLOTS * CHUNK);
    size_t src_b = O.src_mb > 0 ? (size_t)O.src_mb << 20 : TX_RING;
    int16_t *wave = rl_huge_alloc(src_b);
    uint64_t *ctl = rl_huge_alloc(4096), *sts = ctl + 64;
    struct ibv_mr *rx_mr = ibv_reg_mr(L.pd, rx, (size_t)SLOTS * CHUNK, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    struct ibv_mr *tx_mr = ibv_reg_mr(L.pd, wave, src_b, IBV_ACCESS_LOCAL_WRITE);
    struct ibv_mr *c_mr = ibv_reg_mr(L.pd, ctl, 4096, IBV_ACCESS_LOCAL_WRITE);
    if (!rx_mr || !tx_mr || !c_mr) die("ibv_reg_mr");
    rl_connect(&L, O.params, (uint64_t)(uintptr_t)rx, rx_mr->rkey, O.fpga_qpn, O.psn);
    TX_RING = rl_status(&L, sts, c_mr->lkey);
    make_tone(wave, TX_RING, O.tone_mhz, O.amp);
    for (size_t o = TX_RING; o + TX_RING <= src_b; o += TX_RING) memcpy((uint8_t *)wave + o, wave, TX_RING);
    if (src_b % TX_RING) { fprintf(stderr, "--src-mb must be a multiple of the FPGA TX ring\n"); return 1; }
    printf("FPGA TX ring %u KB\n", TX_RING >> 10);

    // ---- helpers on the one QP: all sends signaled, completions only counted
    #define POST(wr) do { struct ibv_send_wr *bad; if (ibv_post_send(qp, &(wr), &bad)) die("ibv_post_send"); } while (0)
    struct ibv_sge sg_ctl = {.addr = (uintptr_t)ctl, .length = 16, .lkey = c_mr->lkey};
    struct ibv_sge sg_st = {.addr = (uintptr_t)sts, .length = 64, .lkey = c_mr->lkey};
    struct ibv_send_wr w_ctl = {.sg_list = &sg_ctl, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                                .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE};
    w_ctl.wr.rdma.remote_addr = CTRL_ADDR; w_ctl.wr.rdma.rkey = WIN_RKEY;
    struct ibv_send_wr w_st = {.wr_id = 1, .sg_list = &sg_st, .num_sge = 1, .opcode = IBV_WR_RDMA_READ,
                               .send_flags = IBV_SEND_SIGNALED};
    w_st.wr.rdma.remote_addr = STAT_ADDR; w_st.wr.rdma.rkey = WIN_RKEY;

    // reset, then fill the TX ring and switch on
    ctl[0] = 4; ctl[1] = 0; POST(w_ctl);
    uint64_t wptr = 0;
    if (O.tx) {
        for (uint32_t off = 0; off < TX_RING; off += TX_PIECE) {
            struct ibv_sge sg = {.addr = (uintptr_t)wave + off, .length = TX_PIECE, .lkey = tx_mr->lkey};
            struct ibv_send_wr w = {.sg_list = &sg, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED};
            w.wr.rdma.remote_addr = WIN + off; w.wr.rdma.rkey = WIN_RKEY;
            POST(w);
        }
        wptr = TX_RING;
    }
    ctl[0] = (O.tx ? 1 : 0) | (O.rx ? 2 : 0); ctl[1] = wptr; POST(w_ctl);

    // ---- main loop
    FILE *sv = O.save ? fopen(O.save, "wb") : NULL;
    int saved = 0;
    double t_st = 0, st_max = 0, wr_max = 0, wq_t[64];
    uint64_t wq_h = 0, wq_c = 0, fill_min = ~0ull;
    uint64_t rptr = 0, stat_pending = 0, tx_bytes = 0, rx_chunks = 0, gaps = 0, expect = 0;
    double t0 = now(), tlast = t0;
    uint64_t tx_last = 0, rx_last = 0;
    printf("  t     TX Gbit/s  RX Gbit/s   RX chunks  gaps   TX underflow  RX overflow  (FPGA counters)\n");
    while (now() - t0 < O.seconds) {
        // status READ: one at a time
        if (!stat_pending) { w_st.wr_id = 1; POST(w_st); stat_pending = 1; t_st = now(); }
        // TX: room in the ring (by the last read pointer) -> next piece + write pointer
        if (O.tx && (int64_t)(wptr - rptr) < 0)            // the FPGA read pointer keeps time: skip ahead
            wptr = (rptr + (256u << 10) + TX_PIECE - 1) & ~(uint64_t)(TX_PIECE - 1);
        if (O.tx && (int64_t)(wptr + TX_PIECE - rptr) <= (int64_t)TX_RING) {
            uint32_t off = (uint32_t)(wptr % TX_RING);
            struct ibv_sge sg = {.addr = (uintptr_t)wave + wptr % src_b, .length = TX_PIECE, .lkey = tx_mr->lkey};
            struct ibv_send_wr w = {.wr_id = 2, .sg_list = &sg, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE};
            wq_t[wq_h++ % 64] = now();          // data writes complete in order
            w.wr.rdma.remote_addr = WIN + off; w.wr.rdma.rkey = WIN_RKEY;
            w.send_flags = IBV_SEND_SIGNALED;
            POST(w);
            wptr += TX_PIECE; tx_bytes += TX_PIECE;
            ctl[1] = wptr; w_ctl.wr_id = 3; POST(w_ctl);   // inline: the value is copied now
        }
        struct ibv_wc wc[64];
        int n = ibv_poll_cq(scq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "send wr %" PRIu64 ": %s (0x%x)\n", wc[i].wr_id, ibv_wc_status_str(wc[i].status), wc[i].vendor_err);
                exit(1);
            }
            if (wc[i].wr_id == 1) {
                rptr = rl_rptr64(sts[0], wptr); stat_pending = 0;
                double d = now() - t_st; if (d > st_max) st_max = d;
                if (wptr - rptr < fill_min) fill_min = wptr - rptr;
            } else if (wc[i].wr_id == 2) {
                double d = now() - wq_t[wq_c++ % 64]; if (d > wr_max) wr_max = d;
            }
        }
        // RX: chunk k arrived in slot k
        n = ibv_poll_cq(rcq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "recv: %s (0x%x)\n", ibv_wc_status_str(wc[i].status), wc[i].vendor_err);
                exit(1);
            }
            uint32_t k = ntohl(wc[i].imm_data);
            if (!rx_chunks) expect = k;                     // runs start at ERNIC's SQ index
            if (k != expect % SLOTS) gaps++;
            expect = k + 1;
            rx_chunks++;
            if (sv && saved < O.save_chunks) { fwrite(rx + (size_t)k * CHUNK, 1, CHUNK, sv); saved++; }
            struct ibv_recv_wr r = {.wr_id = k}, *bad;
            if (ibv_post_recv(qp, &r, &bad)) die("ibv_post_recv");
        }
        double t = now();
        if (t - tlast >= 1.0) {
            printf("%4.0f   %9.2f  %9.2f  %10" PRIu64 "  %5" PRIu64 "   %12" PRIu64 "  %11" PRIu64 "   rptr %" PRIu64 " wptr %" PRIu64 "\n", t - t0,
                   (tx_bytes - tx_last) * 8 / (t - tlast) / 1e9, (rx_chunks - rx_last) * (double)CHUNK * 8 / (t - tlast) / 1e9,
                   rx_chunks, gaps, sts[1], sts[4], sts[0], wptr);
            fflush(stdout);
            if (getenv("LAT")) printf("      max status READ %.0f us, max data WRITE %.0f us, min fill %lu KB\n",
                                      st_max * 1e6, wr_max * 1e6, (unsigned long)(fill_min >> 10));
            st_max = wr_max = 0; fill_min = ~0ull;
            tx_last = tx_bytes; rx_last = rx_chunks; tlast = t;
        }
    }
    // drain the send queue, then stop cleanly
    for (double td = now(); now() - td < 0.5;) {
        struct ibv_wc wc[64];
        int n = ibv_poll_cq(scq, 64, wc);
        for (int i = 0; i < n; i++) if (wc[i].wr_id == 1) { stat_pending = 0; }
        if (!stat_pending) break;
    }
    rl_stop(&L, ctl, c_mr->lkey, sts, c_mr->lkey, wptr);
    if (sv) fclose(sv);
    printf("total: TX %.2f GB, RX %" PRIu64 " chunks (%.2f GB), %" PRIu64 " gaps; FPGA: TX underflow %" PRIu64
           ", RX overflow %" PRIu64 ", chunks produced %" PRIu64 ", sent %" PRIu64 "\n",
           tx_bytes / 1e9, rx_chunks, rx_chunks * (double)CHUNK / 1e9, gaps, sts[1], sts[4], sts[2], sts[3]);
    return 0;
}
