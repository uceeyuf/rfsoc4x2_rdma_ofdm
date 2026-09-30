// Pass-through for one RF data converter ADC stream that also brings the samples out of the
// block design (tap_data / tap_valid, RF fabric clock) for rf_stream.
//
// Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

`timescale 1ns / 1ps

module adc_tap #(
    parameter DWIDTH = 128
) (
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF S_AXIS:M_AXIS" *)
    input  wire              aclk,

    input  wire [DWIDTH-1:0] S_AXIS_tdata,
    input  wire              S_AXIS_tvalid,
    output wire              S_AXIS_tready,

    output wire [DWIDTH-1:0] M_AXIS_tdata,
    output wire              M_AXIS_tvalid,
    input  wire              M_AXIS_tready,

    output wire [DWIDTH-1:0] tap_data,
    output wire              tap_valid
);

assign M_AXIS_tdata  = S_AXIS_tdata;
assign M_AXIS_tvalid = S_AXIS_tvalid;
assign S_AXIS_tready = M_AXIS_tready;
assign tap_data     = S_AXIS_tdata;
assign tap_valid    = S_AXIS_tvalid;

endmodule
