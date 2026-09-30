/*
 * RDMA link to ERNIC on the RFSoC 4x2 (see rdma_link.h).
 * Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
 */

#define _GNU_SOURCE
#include "rdma_link.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double rl_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void rl_die(const char *m)
{
    fprintf(stderr, "%s: %s\n", m, strerror(errno));
    exit(1);
}

void *rl_huge_alloc(size_t n)
{
    size_t a = (n + (2u << 20) - 1) & ~((size_t)(2u << 20) - 1);
    void *p = aligned_alloc(2u << 20, a);
    if (!p) rl_die("alloc");
    madvise(p, a, MADV_HUGEPAGE);
    memset(p, 0, a);
    return p;
}

void *rl_mirror_alloc(size_t bytes)
{
    int fd = memfd_create("rx_ring", 0);
    if (fd < 0 || ftruncate(fd, (off_t)bytes)) rl_die("memfd");
    uint8_t *base = mmap(NULL, 2 * bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) rl_die("mmap");
    for (int i = 0; i < 2; i++)
        if (mmap(base + i * bytes, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
            rl_die("mmap mirror");
    close(fd);
    memset(base, 0, bytes);
    return base;
}

static int find_gid(struct ibv_context *ctx, const char *dev)
{
    for (int i = 0; i < 256; i++) {
        union ibv_gid g;
        if (ibv_query_gid(ctx, 1, i, &g)) break;
        char path[256], type[64] = "";
        snprintf(path, sizeof(path), "/sys/class/infiniband/%s/ports/1/gid_attrs/types/%d", dev, i);
        FILE *f = fopen(path, "r");
        if (f) { if (!fgets(type, sizeof(type), f)) type[0] = 0; fclose(f); }
        static const uint8_t v4[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (strstr(type, "RoCE v2") && !memcmp(g.raw, v4, 12)) return i;
    }
    return -1;
}

void rl_open(rlink *l, const char *dev, int n_recv)
{
    memset(l, 0, sizeof(*l));
    struct ibv_device **list = ibv_get_device_list(NULL);
    struct ibv_device *d = NULL;
    for (int i = 0; list && list[i]; i++)
        if (!dev ? i == 0 : !strcmp(ibv_get_device_name(list[i]), dev)) d = list[i];
    if (!d) { fprintf(stderr, "no RDMA device\n"); exit(1); }
    l->dev = ibv_get_device_name(d);
    l->ctx = ibv_open_device(d);
    if (!l->ctx) rl_die("ibv_open_device");
    l->gid = find_gid(l->ctx, l->dev);
    if (l->gid < 0) { fprintf(stderr, "no RoCE v2 IPv4 GID (is 192.168.100.2 on the interface?)\n"); exit(1); }
    l->pd = ibv_alloc_pd(l->ctx);
    l->scq = ibv_create_cq(l->ctx, 4096, NULL, NULL, 0);
    l->rcq = ibv_create_cq(l->ctx, 4096, NULL, NULL, 0);
    struct ibv_qp_init_attr qia = {.send_cq = l->scq, .recv_cq = l->rcq, .qp_type = IBV_QPT_RC,
                                   .cap = {.max_send_wr = 1024, .max_recv_wr = 1024, .max_send_sge = 1,
                                           .max_recv_sge = 1, .max_inline_data = 64}};
    l->qp = ibv_create_qp(l->pd, &qia);
    if (!l->pd || !l->scq || !l->rcq || !l->qp) rl_die("create pd/cq/qp");
    struct ibv_qp_attr a = {.qp_state = IBV_QPS_INIT, .port_num = 1,
                            .qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ};
    if (ibv_modify_qp(l->qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) rl_die("INIT");
    /* receives for the RDMA WRITE WITH IMMEDIATE of every RX chunk (no buffer needed) */
    for (int i = 0; i < n_recv; i++) {
        struct ibv_recv_wr r = {.wr_id = (uint64_t)i}, *bad;
        if (ibv_post_recv(l->qp, &r, &bad)) rl_die("ibv_post_recv");
    }
}

void rl_connect(rlink *l, const char *params, uint64_t rx_va, uint32_t rx_rkey, int fpga_qpn, uint32_t psn)
{
    char cmd[256], mac[32] = "";
    snprintf(cmd, sizeof(cmd), "ls /sys/class/infiniband/%s/device/net | head -1 | xargs -I{} cat /sys/class/net/{}/address", l->dev);
    FILE *f = popen(cmd, "r");
    if (f) { if (!fgets(mac, sizeof(mac), f)) mac[0] = 0; pclose(f); }
    mac[strcspn(mac, "\n")] = 0;
    char ready[600];
    snprintf(ready, sizeof(ready), "%s.ready", params);
    unlink(ready);
    f = fopen(params, "w");
    if (!f) rl_die(params);
    fprintf(f, "qpn %u\nmac %s\npsn %u\nrx_va %" PRIu64 "\nrx_rkey %u\n", l->qp->qp_num, mac, psn, rx_va, rx_rkey);
    fclose(f);
    printf("host: %s QPN %u, RX ring %d x %u KB at 0x%" PRIx64 " (R_Key 0x%x); waiting for %s\n", l->dev,
           l->qp->qp_num, RL_SLOTS, RL_CHUNK >> 10, rx_va, rx_rkey, ready);
    fflush(stdout);
    struct stat st;
    while (stat(ready, &st)) usleep(100000);
    usleep(1500000);                    /* let Vivado (tests/stream_config.tcl) exit first */

    struct ibv_qp_attr a;
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = IBV_MTU_4096;
    a.dest_qp_num = (uint32_t)fpga_qpn;
    a.rq_psn = psn;
    a.max_dest_rd_atomic = 16;
    a.min_rnr_timer = 1;
    a.ah_attr.is_global = 1;
    a.ah_attr.port_num = 1;
    a.ah_attr.grh.sgid_index = (uint8_t)l->gid;
    a.ah_attr.grh.hop_limit = 64;
    uint8_t *dg = a.ah_attr.grh.dgid.raw;
    memset(dg, 0, 16); dg[10] = dg[11] = 0xff; dg[12] = 192; dg[13] = 168; dg[14] = 100; dg[15] = 1;
    if (ibv_modify_qp(l->qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                     IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) rl_die("RTR");
    memset(&a, 0, sizeof(a));
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = psn;
    a.timeout = 14;
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.max_rd_atomic = 16;
    if (ibv_modify_qp(l->qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                     IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) rl_die("RTS");
}

int rl_modulator, rl_demodulator;

uint32_t rl_status(rlink *l, uint64_t *sts, uint32_t sts_lkey)
{
    struct ibv_sge ss = {.addr = (uintptr_t)sts, .length = 64, .lkey = sts_lkey};
    struct ibv_send_wr ws = {.wr_id = 0xC2, .sg_list = &ss, .num_sge = 1, .opcode = IBV_WR_RDMA_READ,
                             .send_flags = IBV_SEND_SIGNALED}, *bad;
    ws.wr.rdma.remote_addr = RL_STAT_ADDR; ws.wr.rdma.rkey = RL_WIN_RKEY;
    if (ibv_post_send(l->qp, &ws, &bad)) rl_die("ibv_post_send");
    struct ibv_wc wc;
    int n;
    while ((n = ibv_poll_cq(l->scq, 1, &wc)) == 0) {}
    if (n < 0 || wc.status != IBV_WC_SUCCESS) { fprintf(stderr, "status read: %s\n", ibv_wc_status_str(wc.status)); exit(1); }
    if (sts[7] < 0x52465354524D3031ull || sts[7] > 0x52465354524D3033ull) {
        fprintf(stderr, "status word: no rf_stream magic (0x%016lx)\n", (unsigned long)sts[7]);
        exit(1);
    }
    rl_modulator = sts[7] >= 0x52465354524D3032ull;
    rl_demodulator = sts[7] >= 0x52465354524D3033ull;
    uint32_t tx_ring = (uint32_t)(sts[6] >> 32) * 64;
    if (!tx_ring || tx_ring > RL_TX_RING) { fprintf(stderr, "status word: TX ring %u bytes?\n", tx_ring); exit(1); }
    return tx_ring;
}

/* switch TX and RX off and wait until every RX chunk the FPGA produced has been sent and completed:
   a WRITE WITH IMMEDIATE still in flight when the host QP goes away leaves ERNIC's QP stuck
   (reconfiguring it does not clear that, only a new bitstream does) */
void rl_stop(rlink *l, uint64_t *ctl, uint32_t ctl_lkey, uint64_t *sts, uint32_t sts_lkey, uint64_t wptr)
{
    struct ibv_sge sc = {.addr = (uintptr_t)ctl, .length = 16, .lkey = ctl_lkey};
    struct ibv_sge ss = {.addr = (uintptr_t)sts, .length = 64, .lkey = sts_lkey};
    struct ibv_send_wr wc_ = {.wr_id = 0xC0, .sg_list = &sc, .num_sge = 1, .opcode = IBV_WR_RDMA_WRITE,
                              .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE}, *bad;
    wc_.wr.rdma.remote_addr = RL_CTRL_ADDR; wc_.wr.rdma.rkey = RL_WIN_RKEY;
    struct ibv_send_wr ws = {.wr_id = 0xC1, .sg_list = &ss, .num_sge = 1, .opcode = IBV_WR_RDMA_READ,
                             .send_flags = IBV_SEND_SIGNALED};
    ws.wr.rdma.remote_addr = RL_STAT_ADDR; ws.wr.rdma.rkey = RL_WIN_RKEY;
    ctl[0] = 0; ctl[1] = wptr;
    if (ibv_post_send(l->qp, &wc_, &bad)) rl_die("ibv_post_send");
    double t0 = rl_now();
    int pending = 0, reads = 0;
    while (rl_now() - t0 < 2.0) {
        if (!pending) {
            if (ibv_post_send(l->qp, &ws, &bad)) rl_die("ibv_post_send");
            pending = 1;
        }
        struct ibv_wc wc[64];
        int n = ibv_poll_cq(l->scq, 64, wc);
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) { fprintf(stderr, "stop: %s\n", ibv_wc_status_str(wc[i].status)); return; }
            if (wc[i].wr_id == 0xC1) { pending = 0; reads++; }
        }
        n = ibv_poll_cq(l->rcq, 64, wc);
        for (int i = 0; i < n; i++) {
            struct ibv_recv_wr r = {.wr_id = 0}, *rb;
            ibv_post_recv(l->qp, &r, &rb);
        }
        /* after the off write (the first read is ordered behind it): produced == completed */
        /* chunk counts: produced wraps with the 32-bit word counter (2^22 chunks) */
        if (!pending && reads >= 2 && ((sts[2] ^ sts[3]) & 0x3FFFFF) == 0) {
            usleep(1000);
            while (ibv_poll_cq(l->rcq, 64, wc) > 0) {}
            return;
        }
    }
    fprintf(stderr, "stop: RX chunks produced %lu, completed %lu after 2 s\n", (unsigned long)sts[2], (unsigned long)sts[3]);
}
