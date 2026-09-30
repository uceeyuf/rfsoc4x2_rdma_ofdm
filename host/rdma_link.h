/*
 * RDMA link to ERNIC on the RFSoC 4x2: one RC QP on the host, connected by hand to QP 2 of the
 * FPGA (no connection manager). The host's QP number, MAC, PSN and the RX ring VA / R_Key go to
 * a params file for tests/stream_config.tcl, which programs ERNIC and creates "<params>.ready".
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#ifndef RDMA_LINK_H_
#define RDMA_LINK_H_

#include <infiniband/verbs.h>
#include <stdint.h>

#define RL_WIN        0x80000000ull     /* rf_stream window (FPGA memory region 0) */
#define RL_WIN_RKEY   0x12
#define RL_CTRL_ADDR  (RL_WIN + 0x280000)
#define RL_STAT_ADDR  (RL_WIN + 0x280040)
#define RL_TX_RING    (2u << 20)        /* FPGA TX ring, at most (the status word has the size) */
#define RL_CHUNK      (64u << 10)       /* RX chunk = one RDMA WRITE WITH IMMEDIATE */
#define RL_SLOTS      1024              /* host RX ring slots (= FPGA SQ depth): 64 MB, 8 ms */

typedef struct {
    const char         *dev;
    struct ibv_context *ctx;
    struct ibv_pd      *pd;
    struct ibv_cq      *scq, *rcq;
    struct ibv_qp      *qp;
    int                 gid;
} rlink;

double rl_now(void);
void   rl_die(const char *what);
void  *rl_huge_alloc(size_t n);         /* 2 MB aligned, transparent huge pages, zeroed */

/* the RX ring mapped twice back to back, so that a frame running past the end reads on from the
   start; returns the first mapping (bytes long) */
void  *rl_mirror_alloc(size_t bytes);

/* device (NULL: the first), PD, CQs, QP in INIT, n_recv receives posted */
void   rl_open(rlink *l, const char *dev, int n_recv);
/* params file, wait for "<params>.ready", then RTR / RTS */
void   rl_connect(rlink *l, const char *params, uint64_t rx_va, uint32_t rx_rkey, int fpga_qpn, uint32_t psn);

/* one status READ (64 bytes at sts); returns the FPGA TX ring size in bytes */
uint32_t rl_status(rlink *l, uint64_t *sts, uint32_t sts_lkey);

/* the FPGA's TX read pointer (status +0x00) is a 32-bit word count in bytes, 38 bits: extend
   it to 64 bits next to the host's write pointer (within 2^37 bytes of it) */
static inline uint64_t rl_rptr64(uint64_t sts0, uint64_t wptr)
{
    const uint64_t m = (1ull << 38) - 1;
    int64_t d = (int64_t)(((wptr - sts0) & m) << 26) >> 26;   /* sign-extended wptr - rptr */
    return wptr - (uint64_t)d;
}

/* TX / RX off, then wait until the FPGA's RX chunks are all sent and completed (a clean QP for
   the next run) */
void   rl_stop(rlink *l, uint64_t *ctl, uint32_t ctl_lkey, uint64_t *sts, uint32_t sts_lkey, uint64_t wptr);

#endif
