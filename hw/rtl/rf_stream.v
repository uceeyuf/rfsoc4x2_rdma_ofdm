// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Yijie Yu
//
// Continuous I/Q streaming between the host (over ERNIC RDMA) and the RF data converters.
// Sits in ERNIC's on-chip window (behind axi_split2, ERNIC address BASE = 0x8000_0000):
//
//   BASE + 0x00_0000  TX ring  (TX_WORDS x 64 B, 2 MB = 262 us): the host RDMA WRITEs samples,
//                     the player sends them to the DACs, 2 memory words per 2 RF cycles
//   BASE + 0x20_0000  RX ring  (RX_CHUNKS x CHUNK_WORDS x 64 B, 512 KB): the ADCs fill it; every full
//                     chunk rings ERNIC's SQ doorbell, and a pre-filled RDMA WRITE WITH
//                     IMMEDIATE WQE sends it to the host; its completion frees the chunk
//   BASE + 0x28_0000  control (host RDMA WRITE): +0x00 [0] TX on, [1] RX on, [2] reset
//                     (pointers, counters); +0x08 TX write pointer (bytes, cumulative, 64 bit)
//   BASE + 0x28_0040  status (host RDMA READ, one 64-byte word): +0x00 TX read pointer (bytes),
//                     +0x08 TX underflows (words played as zeros: the read pointer never
//                     waits, see the TX player), +0x10 RX chunks
//                     produced, +0x18 RX chunks completed, +0x20 RX overflows (words dropped),
//                     +0x28 {SQ base, SQ doorbells}, +0x30 {TX_WORDS, CHUNK_WORDS},
//                     +0x38 magic 'RFSTRM01'
//
// (The OFDM modem in the FPGA, on this streaming core: https://github.com/uceeyuf/zcu208_4gsps_ofdm)
//
// ERNIC keeps its SQ consumer index when the QP is configured again, so the SQ producer index
// is never reset: a run starts at WQE sq_base (all WQEs of the last run were sent, the host
// stops cleanly), and RX chunk c of the run goes to ring chunk (sq_base + c) mod RX_CHUNKS,
// which is the chunk that WQE sq_base + c sends.
//
// Sample format (TX and RX memory words): each 512-bit word is two RF cycles, each 256 bits
// {Q 8 samples, I 8 samples} as the DAC stream ([127:0] -> DAC_A = I, [255:128] -> DAC_B = Q),
// i.e. in memory: 8 I samples, 8 Q samples, 8 I samples, 8 Q samples (int16, little endian).
// RX: I from ADC_B, Q from ADC_D (ADC_D is inverted on the RFSoC 4x2; the host corrects it).
//
// Clocks: clk (ERNIC AXI, 200 MHz) and clk_rf (RF fabric, 250 MHz = 8 samples / cycle at 2 GSPS).

`resetall
`timescale 1ns / 1ps
`default_nettype none

module rf_stream #(
    parameter [31:0] BASE        = 32'h8000_0000,
    parameter        TX_WORDS    = 32768,           // 2 MB
    parameter        CHUNK_WORDS = 1024,            // 64 KB
    parameter        RX_CHUNKS   = 8,               // 512 KB
    parameter        SQ_DEPTH    = 256,             // WQEs pre-filled, power of two
    parameter        QP          = 2,
    parameter [31:0] SQPI_ADDR   = 32'h5018_0038 + QP * 32'h100,
    parameter [12:0] CQ_DB_ADDR  = 13'h004
)(
    input  wire         clk,
    input  wire         rst,
    input  wire         clk_rf,
    input  wire         rst_rf,

    // ERNIC writes (rx_pkt_hndler_ddr through the split): TX ring and control
    input  wire         a_awid,
    input  wire [31:0]  a_awaddr,
    input  wire [7:0]   a_awlen,
    input  wire [2:0]   a_awsize,
    input  wire         a_awvalid,
    output wire         a_awready,
    input  wire [511:0] a_wdata,
    input  wire [63:0]  a_wstrb,
    input  wire         a_wlast,
    input  wire         a_wvalid,
    output wire         a_wready,
    output wire         a_bid,
    output wire [1:0]   a_bresp,
    output wire         a_bvalid,
    input  wire         a_bready,
    input  wire         a_arid,
    input  wire [7:0]   a_arlen,
    input  wire         a_arvalid,
    output wire         a_arready,
    output reg          a_rid = 1'b0,
    output wire [511:0] a_rdata,
    output wire [1:0]   a_rresp,
    output wire         a_rlast,
    output wire         a_rvalid,
    input  wire         a_rready,

    // ERNIC reads (wqe_proc_top through the split): RX ring and status
    input  wire         b_arid,
    input  wire [31:0]  b_araddr,
    input  wire [7:0]   b_arlen,
    input  wire [2:0]   b_arsize,
    input  wire         b_arvalid,
    output wire         b_arready,
    output reg          b_rid = 1'b0,
    output wire [511:0] b_rdata,
    output wire [1:0]   b_rresp,
    output wire         b_rlast,
    output wire         b_rvalid,
    input  wire         b_rready,
    input  wire         b_awid,
    input  wire         b_awvalid,
    output wire         b_awready,
    input  wire         b_wlast,
    input  wire         b_wvalid,
    output wire         b_wready,
    output reg          b_bid = 1'b0,
    output wire [1:0]   b_bresp,
    output reg          b_bvalid = 1'b0,
    input  wire         b_bready,

    // ERNIC doorbell handshake ports (QPCONF[4] = 0)
    input  wire [31:0]  cq_db_cnt,
    input  wire [12:0]  cq_db_addr,
    input  wire         cq_db_valid,
    output wire         cq_db_rdy,
    output reg  [15:0]  sq_pidb = 16'd0,
    output wire [31:0]  sq_pidb_addr,
    output reg          sq_pidb_valid = 1'b0,
    input  wire         sq_pidb_rdy,

    // RF side (clk_rf)
    output reg  [255:0] dac_tdata = 256'd0,        // {Q 8 samples, I 8 samples}
    output reg          dac_tvalid = 1'b0,
    output wire         dac_active,                // player on (select this source for the DACs)
    input  wire [127:0] adc_i,                     // ADC_B, 8 samples
    input  wire [127:0] adc_q,                     // ADC_D, 8 samples
    input  wire         adc_valid
);

localparam TXA   = $clog2(TX_WORDS);
localparam RXW   = CHUNK_WORDS * RX_CHUNKS;
localparam RXA   = $clog2(RXW);
localparam CHB   = $clog2(CHUNK_WORDS);
localparam SQB   = $clog2(SQ_DEPTH);
localparam RCB   = $clog2(RX_CHUNKS);
localparam [31:0] CTL_OFF = 32'h0028_0000;

// ======================================================================== memories
(* ram_style = "block" *) reg [511:0] tx_mem [0:TX_WORDS-1];
// RX ring: UltraRAM in the clk domain (below), written through an asynchronous FIFO

// ======================================================================== control (clk)
reg         ctl_tx = 1'b0, ctl_rx = 1'b0, ctl_clr = 1'b0;
reg  [63:0] tx_wptr_b = 64'd0;                   // host write pointer, bytes

// ---------------------------------------------------------------- port a: write bursts
reg        w_act = 1'b0;
reg [31:0] w_addr = 32'd0;
reg [2:0]  w_size = 3'd0;
reg        w_id = 1'b0;
reg [7:0]  bq = 8'd0;
reg [3:0]  bcnt = 4'd0;

wire w_beat = a_wvalid && a_wready;
wire w_done = w_beat && a_wlast;
wire b_pop  = a_bvalid && a_bready;
wire [31:0] w_off = w_addr - BASE;

assign a_awready = !w_act;
assign a_wready  = w_act && bcnt < 4'd7;
assign a_bid     = bq[0];
assign a_bresp   = 2'b00;
assign a_bvalid  = bcnt != 4'd0;

// one register stage in front of the 2 MB of BRAM
integer i;
(* shreg_extract = "no" *) reg         tw_e = 1'b0;
(* shreg_extract = "no" *) reg [TXA-1:0] tw_a = 0;
(* shreg_extract = "no" *) reg [511:0] tw_d = 512'd0;
(* shreg_extract = "no" *) reg [63:0]  tw_s = 64'd0;
always @(posedge clk) begin
    tw_e <= w_beat && w_off < TX_WORDS * 64;
    tw_a <= w_off[TXA+5:6];
    tw_d <= a_wdata;
    tw_s <= a_wstrb;
    if (tw_e)
        for (i = 0; i < 64; i = i + 1)
            if (tw_s[i]) tx_mem[tw_a][i*8 +: 8] <= tw_d[i*8 +: 8];
end

always @(posedge clk) begin
    ctl_clr <= 1'b0;
    if (a_awvalid && a_awready) begin
        w_act <= 1'b1; w_addr <= a_awaddr; w_size <= a_awsize; w_id <= a_awid;
    end
    if (w_beat) begin
        w_addr <= w_addr + (32'd1 << w_size);
        // control word: bytes 0..7 control, 8..15 TX write pointer
        if (w_off[31:6] == (CTL_OFF >> 6)) begin
            if (a_wstrb[0]) begin
                ctl_tx  <= a_wdata[0];
                ctl_rx  <= a_wdata[1];
                ctl_clr <= a_wdata[2];
            end
            for (i = 0; i < 8; i = i + 1)
                if (a_wstrb[8 + i]) tx_wptr_b[i*8 +: 8] <= a_wdata[64 + i*8 +: 8];
        end
    end
    if (w_done) w_act <= 1'b0;
    if (w_done && !b_pop) begin
        bq[bcnt] <= w_id; bcnt <= bcnt + 1'b1;
    end else if (!w_done && b_pop) begin
        bq <= bq >> 1; bcnt <= bcnt - 1'b1;
    end else if (w_done && b_pop) begin
        bq <= bq >> 1; bq[bcnt - 1'b1] <= w_id;
    end
    if (rst) begin
        w_act <= 1'b0; bcnt <= 4'd0; ctl_tx <= 1'b0; ctl_rx <= 1'b0; tx_wptr_b <= 64'd0;
    end
    if (ctl_clr) tx_wptr_b <= 64'd0;
end

// port a reads (unused): zeros
reg       ad_act = 1'b0;
reg [7:0] ad_left = 8'd0;
assign a_arready = !ad_act;
assign a_rvalid  = ad_act;
assign a_rdata   = 512'd0;
assign a_rresp   = 2'b00;
assign a_rlast   = ad_left == 8'd0;
always @(posedge clk) begin
    if (a_arvalid && a_arready) begin
        ad_act <= 1'b1; ad_left <= a_arlen; a_rid <= a_arid;
    end
    if (a_rvalid && a_rready) begin
        ad_left <= ad_left - 1'b1;
        if (a_rlast) ad_act <= 1'b0;
    end
    if (rst) ad_act <= 1'b0;
end

// ======================================================================== clock crossings
// RF -> clk: word counters that step by one (Gray)
wire [31:0] tx_rd_rf, rx_wr_rf;                  // RF domain counters (words)
wire [31:0] tx_rd_s;                             // synchronized to clk
xpm_cdc_gray #(.WIDTH(32), .DEST_SYNC_FF(3)) cdc_txrd (
    .src_clk(clk_rf), .src_in_bin(tx_rd_rf), .dest_clk(clk), .dest_out_bin(tx_rd_s));


// clk -> RF: values that jump (handshake, re-sent whenever the previous one is through)
reg  [31:0] tx_wr_w = 32'd0, rx_free_w = 32'd0;  // TX words written by the host, RX words freed
wire [65:0] hs_src = {ctl_tx, ctl_rx, tx_wr_w, rx_free_w};
wire [65:0] hs_dst;
wire        hs_rcv, hs_ack;
reg         hs_send = 1'b0;
always @(posedge clk) begin
    tx_wr_w <= tx_wptr_b[37:6];
    if (!hs_send && !hs_ack) hs_send <= 1'b1;
    else if (hs_ack) hs_send <= 1'b0;
end
xpm_cdc_handshake #(.WIDTH(66), .DEST_EXT_HSK(0), .DEST_SYNC_FF(3), .SRC_SYNC_FF(3)) cdc_hs (
    .src_clk(clk), .src_in(hs_src), .src_send(hs_send), .src_rcv(hs_ack),
    .dest_clk(clk_rf), .dest_req(hs_rcv), .dest_ack(1'b0), .dest_out(hs_dst));
wire clr_rf;
xpm_cdc_pulse #(.DEST_SYNC_FF(3), .RST_USED(0)) cdc_clr (
    .src_clk(clk), .src_pulse(ctl_clr), .src_rst(1'b0), .dest_clk(clk_rf), .dest_rst(1'b0), .dest_pulse(clr_rf));

reg        tx_on_rf = 1'b0, rx_on_rf = 1'b0;
reg [31:0] tx_wr_rf = 32'd0, rx_free_rf = 32'd0;
always @(posedge clk_rf) begin
    if (hs_rcv) begin
        tx_on_rf <= hs_dst[65]; rx_on_rf <= hs_dst[64];
        tx_wr_rf <= hs_dst[63:32]; rx_free_rf <= hs_dst[31:0];
    end
    if (rst_rf || clr_rf) begin
        tx_on_rf <= 1'b0; rx_on_rf <= 1'b0; tx_wr_rf <= 32'd0; rx_free_rf <= 32'd0;
    end
end
assign dac_active = tx_on_rf;

// ======================================================================== TX player (clk_rf)
// one 512-bit word per two RF cycles; read latency 4 (address register, BRAM, two output
// registers) for the 2 MB of BRAM
reg [31:0] tx_rd = 32'd0;
reg        ph = 1'b0;                            // 0: low half this cycle, fetch next word
reg [3:0]  rv = 4'b0000;                         // read valid pipeline
(* shreg_extract = "no" *) reg [TXA-1:0] tx_ra = 0;
reg [511:0] tx_q1 = 512'd0, tx_q2 = 512'd0, tx_word = 512'd0, tx_hold = 512'd0;
reg [31:0] tx_under_rf = 32'd0;
assign tx_rd_rf = tx_rd;
wire tx_avail = (tx_wr_rf - tx_rd) != 32'd0 && (tx_wr_rf - tx_rd) <= TX_WORDS;

always @(posedge clk_rf) begin
    ph <= ~ph;
    // fetch on ph == 0, the word is there four cycles later (the ph == 0 after next)
    rv <= {rv[2:0], 1'b0};
    // the read pointer keeps time: a word the host has not written yet plays as zeros and is
    // skipped, so later samples stay on their time grid (the host sees rptr > wptr and moves on)
    if (!ph && tx_on_rf) begin
        tx_rd <= tx_rd + 1'b1;
        if (tx_avail)
            rv[0] <= 1'b1;
        else
            tx_under_rf <= tx_under_rf + 1'b1;
    end
    tx_ra <= tx_rd[TXA-1:0];
    tx_q1 <= tx_mem[tx_ra];
    tx_q2 <= tx_q1;
    tx_word <= tx_q2;
    if (!ph) begin
        tx_hold <= rv[3] ? tx_word : 512'd0;
        dac_tdata <= rv[3] ? tx_word[255:0] : 256'd0;
    end else begin
        dac_tdata <= tx_hold[511:256];
    end
    dac_tvalid <= 1'b1;
    if (rst_rf || clr_rf) begin
        tx_rd <= 32'd0; tx_under_rf <= 32'd0; rv <= 4'b0000;
    end
end

// ======================================================================== RX ring (UltraRAM, clk)
// the packer's words (address, data) cross in a small FIFO (clk drains 200 M/s, the packer fills
// 125 M/s); rx_wr_c counts the words in the ring, so the chunk doorbells follow the ring itself
wire [RXA+511:0] rxf_dout;
wire             rxf_empty;
reg  [31:0]      rx_wr_c = 32'd0;
wire [511:0]     rxr_dout;
wire [RXA-1:0]   rxr_raddr;
xpm_fifo_async #(
    .FIFO_MEMORY_TYPE("distributed"), .FIFO_WRITE_DEPTH(16), .WRITE_DATA_WIDTH(RXA + 512),
    .READ_DATA_WIDTH(RXA + 512), .READ_MODE("fwft"), .FIFO_READ_LATENCY(0), .CDC_SYNC_STAGES(3),
    .RELATED_CLOCKS(0), .USE_ADV_FEATURES("0000"), .ECC_MODE("no_ecc"), .FULL_RESET_VALUE(0),
    .DOUT_RESET_VALUE("0"), .WAKEUP_TIME(0), .SIM_ASSERT_CHK(0)
) rx_cdc_fifo (
    .rst(rst_rf), .wr_clk(clk_rf), .wr_en(w2_e), .din({w2_a, w2_d}), .full(), .overflow(),
    .rd_clk(clk), .rd_en(!rxf_empty), .dout(rxf_dout), .empty(rxf_empty), .underflow(),
    .prog_full(), .wr_data_count(), .prog_empty(), .rd_data_count(), .almost_full(), .almost_empty(),
    .wr_ack(), .data_valid(), .rd_rst_busy(), .wr_rst_busy(), .sleep(1'b0),
    .injectsbiterr(1'b0), .injectdbiterr(1'b0), .sbiterr(), .dbiterr());
xpm_memory_sdpram #(
    .MEMORY_SIZE(RXW * 512), .MEMORY_PRIMITIVE("ultra"), .CLOCKING_MODE("common_clock"),
    .MEMORY_INIT_FILE("none"), .USE_MEM_INIT(0), .ECC_MODE("no_ecc"), .AUTO_SLEEP_TIME(0),
    .WRITE_DATA_WIDTH_A(512), .BYTE_WRITE_WIDTH_A(512), .ADDR_WIDTH_A(RXA),
    .READ_DATA_WIDTH_B(512), .ADDR_WIDTH_B(RXA), .READ_LATENCY_B(2), .WRITE_MODE_B("read_first"),
    .READ_RESET_VALUE_B("0")
) rx_ring (
    .clka(clk), .ena(1'b1), .wea(!rxf_empty), .addra(rxf_dout[RXA+511:512]), .dina(rxf_dout[511:0]),
    .clkb(clk), .rstb(1'b0), .enb(1'b1), .regceb(1'b1), .addrb(rxr_raddr), .doutb(rxr_dout),
    .sleep(1'b0), .injectsbiterra(1'b0), .injectdbiterra(1'b0), .sbiterrb(), .dbiterrb());

// ======================================================================== RX packer (clk_rf)
// ring chunk of the run's first chunk: quasi static (changes on reset, long before RX is on)
wire [RCB-1:0] rx_off;
(* ASYNC_REG = "TRUE" *) reg [RCB-1:0] rx_off_m = 0, rx_off_rf = 0;
always @(posedge clk_rf) begin rx_off_m <= rx_off; rx_off_rf <= rx_off_m; end

reg [31:0] rx_wr = 32'd0, rx_over_rf = 32'd0;
reg        rph = 1'b0;
reg [255:0] rx_lo = 256'd0;
assign rx_wr_rf = rx_wr;
wire rx_space = (rx_wr - rx_free_rf) < RXW;

// two register stages between the ADC and the 1 MB of BRAM (spread over many columns): the ADC
// samples (a0), then data, address and write enable (w2); rx_wa is the ring address, preset to
// the run's first chunk while RX is off
(* shreg_extract = "no" *) reg [255:0] a0_d = 256'd0;
(* shreg_extract = "no" *) reg         a0_v = 1'b0;
(* shreg_extract = "no" *) reg [511:0] w2_d = 512'd0;
(* shreg_extract = "no" *) reg [RXA-1:0] w2_a = 0;
(* shreg_extract = "no" *) reg         w2_e = 1'b0;
reg [RXA-1:0] rx_wa = 0;

always @(posedge clk_rf) begin
    a0_d <= {adc_q, adc_i};
    a0_v <= adc_valid;
    w2_e <= 1'b0;
    if (a0_v) begin
        rph <= ~rph;
        if (!rph)
            rx_lo <= a0_d;
        else if (rx_on_rf) begin
            if (rx_space) begin
                w2_d <= {a0_d, rx_lo};
                w2_a <= rx_wa;
                w2_e <= 1'b1;
                rx_wa <= rx_wa + 1'b1;
                rx_wr <= rx_wr + 1'b1;
            end else begin
                rx_over_rf <= rx_over_rf + 1'b1;
            end
        end
    end

    if (!rx_on_rf)
        rx_wa <= {rx_off_rf, {CHB{1'b0}}};         // settled long before RX goes on
    if (rst_rf || clr_rf) begin
        rx_wr <= 32'd0; rx_over_rf <= 32'd0; rph <= 1'b0;
    end
end

// RF counters for the status word (quasi static reads, Gray through the same crossing style)
wire [31:0] tx_under_s, rx_over_s;
xpm_cdc_gray #(.WIDTH(32), .DEST_SYNC_FF(3)) cdc_under (
    .src_clk(clk_rf), .src_in_bin(tx_under_rf), .dest_clk(clk), .dest_out_bin(tx_under_s));
xpm_cdc_gray #(.WIDTH(32), .DEST_SYNC_FF(3)) cdc_over (
    .src_clk(clk_rf), .src_in_bin(rx_over_rf), .dest_clk(clk), .dest_out_bin(rx_over_s));

// ======================================================================== doorbells (clk)
// chunks produced -> SQ PI (WQE k sends RX chunk k mod RX_CHUNKS); completions -> chunks freed
reg [31:0] chunks_done = 32'd0, n_sq = 32'd0;
reg [31:0] chunks_ready = 32'd0, chunks_rung = 32'd0;
reg [31:0] sq_base = 32'd0, pi_abs = 32'd0, pi_pend = 32'd0;   // WQEs: run start, told to ERNIC
assign cq_db_rdy    = 1'b1;
assign sq_pidb_addr = SQPI_ADDR;
assign rx_off       = sq_base[RCB-1:0];

function [15:0] db_value(input [31:0] v);
    db_value = (v[SQB-1:0] == 0) ? SQ_DEPTH : v[SQB-1:0];
endfunction

always @(posedge clk) begin
    if (!rxf_empty) rx_wr_c <= rx_wr_c + 1'b1;
    chunks_ready <= rx_wr_c >> CHB;
    if (cq_db_valid && cq_db_addr == CQ_DB_ADDR)
        chunks_done <= chunks_done + cq_db_cnt;
    rx_free_w <= chunks_done << CHB;
    if (sq_pidb_valid && sq_pidb_rdy) begin
        sq_pidb_valid <= 1'b0;
        n_sq <= n_sq + 1'b1;
        pi_abs <= pi_pend;
    end else if (!sq_pidb_valid && chunks_ready != chunks_rung) begin
        sq_pidb <= db_value(sq_base + chunks_ready);
        pi_pend <= sq_base + chunks_ready;
        sq_pidb_valid <= 1'b1;
        chunks_rung <= chunks_ready;
    end
    if (rst || ctl_clr) begin
        rx_wr_c <= 32'd0;
        chunks_done <= 32'd0; chunks_rung <= 32'd0; n_sq <= 32'd0; sq_pidb_valid <= 1'b0;
        rx_free_w <= 32'd0;
        // the next run starts where ERNIC is: after the last producer index it accepted
        sq_base <= (sq_pidb_valid && sq_pidb_rdy) ? pi_pend : pi_abs;
    end
    if (rst) begin
        sq_base <= 32'd0; pi_abs <= 32'd0;
    end
end

// ======================================================================== port b: read bursts
// RX ring (BRAM, latency 2) and the status word; one burst at a time, R one beat per cycle
reg [511:0] status = 512'd0;
always @(posedge clk)
    status <= {64'h5246_5354_524D_3031,                       // 'RFSTRM01'
               32'd0 + TX_WORDS, 32'd0 + CHUNK_WORDS,
               sq_base, n_sq,
               32'd0, rx_over_s,
               32'd0, chunks_done,
               32'd0, chunks_ready,
               32'd0, tx_under_s,
               {26'd0, tx_rd_s, 6'd0}};

reg        r_act = 1'b0, r_stat = 1'b0;
reg [31:0] r_addr = 32'd0;
reg [2:0]  r_size = 3'd0;
reg [7:0]  r_left = 8'd0;
reg [1:0]  p_v = 2'b00, p_last = 2'b00, p_stat = 2'b00, p_id = 2'b00;

wire [31:0] r_off = r_addr - BASE;
assign rxr_raddr = r_off[RXA+5:6];                  // UltraRAM read, latency 2 (as the pipeline)

// a 4-deep output queue absorbs the 2-cycle read pipeline when R stalls
reg [512+2-1:0] oq [0:3];
reg [2:0]  oq_n = 3'd0;
reg [1:0]  oq_rd = 2'd0, oq_wr = 2'd0;
wire oq_pop  = b_rvalid && b_rready;
wire can_issue = r_act && (oq_n + p_v[0] + p_v[1]) < 3'd4;     // at most 4 beats in flight

assign b_arready = !r_act;
assign b_rresp   = 2'b00;
assign b_rdata   = oq[oq_rd][511:0];

always @(posedge clk) begin
    if (b_arvalid && b_arready) begin
        r_act <= 1'b1; r_addr <= b_araddr; r_size <= b_arsize; r_left <= b_arlen; b_rid <= b_arid;
    end
    p_v <= {p_v[0], can_issue};
    p_last <= {p_last[0], r_left == 8'd0};
    p_stat <= {p_stat[0], r_off >= CTL_OFF};

    if (can_issue) begin
        r_addr <= r_addr + (32'd1 << r_size);
        r_left <= r_left - 1'b1;
        if (r_left == 8'd0) r_act <= 1'b0;
    end
    // output queue
    if (p_v[1]) begin
        oq[oq_wr] <= {p_last[1], 1'b0, p_stat[1] ? status : rxr_dout};
        oq_wr <= oq_wr + 1'b1;
    end
    if (oq_pop) oq_rd <= oq_rd + 1'b1;
    oq_n <= oq_n + p_v[1] - oq_pop;
    if (rst) begin
        r_act <= 1'b0; p_v <= 2'b00; oq_n <= 3'd0; oq_rd <= 2'd0; oq_wr <= 2'd0;
    end
end
assign b_rvalid = oq_n != 3'd0;
assign b_rlast  = oq[oq_rd][513];

// port b writes (unused): discarded
reg bd_act = 1'b0;
assign b_awready = !bd_act && !b_bvalid;
assign b_wready  = bd_act;
assign b_bresp   = 2'b00;
always @(posedge clk) begin
    if (b_awvalid && b_awready) begin
        bd_act <= 1'b1; b_bid <= b_awid;
    end
    if (b_wvalid && b_wready && b_wlast) begin
        bd_act <= 1'b0; b_bvalid <= 1'b1;
    end
    if (b_bvalid && b_bready) b_bvalid <= 1'b0;
    if (rst) begin
        bd_act <= 1'b0; b_bvalid <= 1'b0;
    end
end

endmodule

`resetall
