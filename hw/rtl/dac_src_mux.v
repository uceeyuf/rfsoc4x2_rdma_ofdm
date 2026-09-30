// DAC source switch in the RF fabric clock domain: the block design's URAM player (S_AXIS) or
// the streaming player of rf_stream (ext_tdata, ext_sel = 1). One register stage; the RF data
// converter takes a beat every cycle, so there is no back-pressure to pass on.
//
// Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

`timescale 1ns / 1ps

module dac_src_mux #(
    parameter DWIDTH = 256
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXIS:M_AXIS" *)
    input  wire              aclk,

    input  wire [DWIDTH-1:0] S_AXIS_tdata,
    input  wire              S_AXIS_tvalid,
    output wire              S_AXIS_tready,

    input  wire [DWIDTH-1:0] ext_tdata,
    input  wire              ext_sel,

    output reg  [DWIDTH-1:0] M_AXIS_tdata,
    output reg               M_AXIS_tvalid,
    input  wire              M_AXIS_tready
);

assign S_AXIS_tready = 1'b1;

always @(posedge aclk) begin
    M_AXIS_tdata  <= ext_sel ? ext_tdata : S_AXIS_tdata;
    M_AXIS_tvalid <= ext_sel ? 1'b1 : S_AXIS_tvalid;
end

endmodule
