// Testbench for rf_stream (small rings): TX words through port a -> DAC beats in order,
// RX ADC counter -> chunks -> SQ doorbells, RX read-back through port b with random rready,
// completions, the status word, and a second run after a reset (SQ index carried on).
//   hw/sim/run.sh
// Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
`timescale 1ns/1ps
module tb;
reg clk = 0, clk_rf = 0, rst = 1, rst_rf = 1;
always #2.5 clk = ~clk;         // 200 MHz
always #2.0 clk_rf = ~clk_rf;   // 250 MHz

localparam TXW = 64, CHW = 16, RXC = 4, SQD = 16;
localparam [31:0] BASE = 32'h8000_0000, RX_OFF = 32'h20_0000, CTL = 32'h28_0000, STAT = 32'h28_0040;

// port a
reg [31:0] awaddr = 0; reg [7:0] awlen = 0; reg awvalid = 0; wire awready;
reg [511:0] wdata = 0; reg [63:0] wstrb = 0; reg wlast = 0, wvalid = 0; wire wready, bvalid;
// port b
reg [31:0] araddr = 0; reg [7:0] arlen = 0; reg arvalid = 0; wire arready;
wire [511:0] rdata; wire rvalid, rlast; reg rready = 1;
// doorbells
reg [31:0] cq_cnt = 0; reg cq_valid = 0;
wire [15:0] sq_pidb; wire sq_valid; reg sq_rdy = 1;
// RF
wire [255:0] dac; wire dac_v, dac_act;
reg [127:0] adc_i = 0, adc_q = 0;

rf_stream #(.TX_WORDS(TXW), .CHUNK_WORDS(CHW), .RX_CHUNKS(RXC), .SQ_DEPTH(SQD)) dut (
 .clk(clk), .rst(rst), .clk_rf(clk_rf), .rst_rf(rst_rf),
 .a_awid(1'b0), .a_awaddr(awaddr), .a_awlen(awlen), .a_awsize(3'd6), .a_awvalid(awvalid), .a_awready(awready),
 .a_wdata(wdata), .a_wstrb(wstrb), .a_wlast(wlast), .a_wvalid(wvalid), .a_wready(wready),
 .a_bid(), .a_bresp(), .a_bvalid(bvalid), .a_bready(1'b1),
 .a_arid(1'b0), .a_arlen(8'd0), .a_arvalid(1'b0), .a_arready(), .a_rid(), .a_rdata(), .a_rresp(), .a_rlast(), .a_rvalid(), .a_rready(1'b1),
 .b_arid(1'b0), .b_araddr(araddr), .b_arlen(arlen), .b_arsize(3'd6), .b_arvalid(arvalid), .b_arready(arready),
 .b_rid(), .b_rdata(rdata), .b_rresp(), .b_rlast(rlast), .b_rvalid(rvalid), .b_rready(rready),
 .b_awid(1'b0), .b_awvalid(1'b0), .b_awready(), .b_wlast(1'b0), .b_wvalid(1'b0), .b_wready(), .b_bid(), .b_bresp(), .b_bvalid(), .b_bready(1'b1),
 .cq_db_cnt(cq_cnt), .cq_db_addr(13'h004), .cq_db_valid(cq_valid), .cq_db_rdy(),
 .sq_pidb(sq_pidb), .sq_pidb_addr(), .sq_pidb_valid(sq_valid), .sq_pidb_rdy(sq_rdy),
 .dac_tdata(dac), .dac_tvalid(dac_v), .dac_active(dac_act), .adc_i(adc_i), .adc_q(adc_q), .adc_valid(1'b1));

function [511:0] txword(input integer n); txword = {16{n[31:0] ^ 32'hA5A5_0000}} + n; endfunction

task wr_burst(input [31:0] a, input integer n0, input integer beats, input integer ctl, input [63:0] wp);
 integer j; begin
  @(posedge clk); awaddr <= a; awlen <= beats - 1; awvalid <= 1;
  @(posedge clk); while (!awready) @(posedge clk); awvalid <= 0;
  for (j = 0; j < beats; j = j + 1) begin
    if (ctl >= 0) begin wdata <= {384'd0, wp, 64'd0 + ctl}; wstrb <= 64'hFFFF; end
    else begin wdata <= txword(n0 + j); wstrb <= {64{1'b1}}; end
    wlast <= (j == beats - 1); wvalid <= 1;
    @(posedge clk); while (!wready) @(posedge clk);
  end
  wvalid <= 0; wlast <= 0;
 end endtask

// ---- DAC check: every non-zero DAC beat must be the next half of the TX words, in order
integer tx_seen = 0, tx_bad = 0, half = 0;
reg [511:0] tw;
always @(posedge clk_rf) if (dac_act && dac != 0) begin
  tw = txword(tx_seen);
  if (dac !== (half ? tw[511:256] : tw[255:0])) tx_bad = tx_bad + 1;
  if (half) tx_seen = tx_seen + 1;
  half = !half;
end

// ---- ADC pattern: counter per RF cycle
integer rfc = 0;
always @(posedge clk_rf) begin rfc <= rfc + 1; adc_i <= {4{rfc[31:0]}}; adc_q <= ~{4{rfc[31:0]}}; end

// ---- SQ doorbells
integer n_db = 0; reg [15:0] last_db = 0;
always @(posedge clk) if (sq_valid && sq_rdy) begin n_db = n_db + 1; last_db = sq_pidb; end

integer rbad = 0, rbeats = 0, fails = 0; reg [511:0] first_w;
initial begin
  repeat (10) @(posedge clk); rst <= 0; rst_rf <= 0;
  repeat (20) @(posedge clk);
  // TX: 64 words, then TX on + write pointer
  wr_burst(BASE, 0, 64, -1, 0);
  wr_burst(BASE + CTL, 0, 1, 1, 64 * 64);
  repeat (400) @(posedge clk);
  $display("TX: %0d words played, %0d bad beats, underflows %0d", tx_seen, tx_bad, dut.tx_under_rf);
  if (tx_seen != 64 || tx_bad) fails = fails + 1;
  // RX: on
  wr_burst(BASE + CTL, 0, 1, 3, 64 * 64);
  repeat (300) @(posedge clk);
  $display("RX: words written %0d, chunks ready %0d, SQ doorbells %0d (last %0d), overflow %0d",
           dut.rx_wr, dut.chunks_ready, n_db, last_db, dut.rx_over_rf);
  if (dut.chunks_ready != RXC || n_db != RXC) fails = fails + 1;
  // read chunk 0 through port b: a run of consecutive ADC cycles
  @(posedge clk); araddr <= BASE + RX_OFF; arlen <= CHW - 1; arvalid <= 1;
  @(posedge clk); while (!arready) @(posedge clk); arvalid <= 0;
  fork begin repeat (40) begin @(posedge clk) rready <= $random; end rready <= 1; end join_none
  while (rbeats < CHW) begin
    @(posedge clk);
    if (rvalid && rready) begin
      if (rbeats == 0) first_w = rdata;
      else if (rdata[31:0] !== first_w[31:0] + 2 * rbeats || rdata[159:128] !== ~(first_w[31:0] + 2 * rbeats)
               || rdata[287:256] !== first_w[31:0] + 2 * rbeats + 1) rbad = rbad + 1;
      rbeats = rbeats + 1;
    end
  end
  $display("RX read: %0d beats, %0d bad, last had rlast %0d", rbeats, rbad, rlast);
  if (rbad) fails = fails + 1;
  // complete 1 chunk, read the status word
  @(posedge clk); cq_cnt <= 1; cq_valid <= 1; @(posedge clk); @(posedge clk); cq_valid <= 0;
  repeat (100) @(posedge clk);
  @(posedge clk); araddr <= BASE + STAT; arlen <= 0; arvalid <= 1;
  @(posedge clk); while (!arready) @(posedge clk); arvalid <= 0;
  while (!(rvalid && rready)) @(posedge clk);
  $display("status: tx_rptr %0d B, under %0d, chunks ready %0d, done %0d, over %0d, sq %0d, magic %h",
           rdata[63:0], rdata[127:64], rdata[191:128], rdata[255:192], rdata[319:256], rdata[351:320], rdata[511:448]);
  if (rdata[511:448] !== 64'h5246_5354_524D_3031) fails = fails + 1;
  // second run: RX off, complete the rest, reset, RX on again
  wr_burst(BASE + CTL, 0, 1, 0, 64 * 64);
  repeat (50) @(posedge clk);
  @(posedge clk); cq_cnt <= dut.chunks_ready - dut.chunks_done; cq_valid <= 1; @(posedge clk); cq_valid <= 0;
  repeat (20) @(posedge clk);
  $display("run 1 end: chunks ready %0d done %0d, pi_abs %0d", dut.chunks_ready, dut.chunks_done, dut.pi_abs);
  wr_burst(BASE + CTL, 0, 1, 4, 0);
  repeat (50) @(posedge clk);
  n_db = 0;
  wr_burst(BASE + CTL, 0, 1, 2, 0);
  repeat (200) @(posedge clk);
  $display("run 2: sq_base %0d, rx_off %0d, chunks ready %0d, doorbells %0d, last PI %0d (expect sq_base + ready mod %0d)",
           dut.sq_base, dut.rx_off_rf, dut.chunks_ready, n_db, last_db, SQD);
  if (last_db != (dut.sq_base + dut.chunks_ready) % SQD) fails = fails + 1;
  // run 2's first chunk is ring chunk sq_base mod RXC, the next ones follow in time
  $display("ring chunk %0d word 0: %h, chunk %0d word 0: %h", dut.sq_base % RXC, dut.rx_mem[(dut.sq_base % RXC) * CHW][31:0],
           (dut.sq_base + 2) % RXC, dut.rx_mem[((dut.sq_base + 2) % RXC) * CHW][31:0]);
  if (dut.rx_mem[((dut.sq_base + 2) % RXC) * CHW][31:0] !== dut.rx_mem[(dut.sq_base % RXC) * CHW][31:0] + 4 * CHW) fails = fails + 1;
  if (fails) $display("FAIL (%0d checks)", fails); else $display("PASS");
  $finish;
end
endmodule
