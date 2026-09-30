#!/bin/sh
# Simulate rf_stream with Vivado's xsim (XPM CDC macros from the Vivado install).
#   hw/sim/run.sh
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=$here/../../build/sim
mkdir -p "$out" && cd "$out"
xvlog -sv "$here/tb_rf_stream.v" "$here/../rtl/rf_stream.v" \
    "$XILINX_VIVADO/data/ip/xpm/xpm_cdc/hdl/xpm_cdc.sv" "$XILINX_VIVADO/data/verilog/src/glbl.v" > xvlog.log
xelab -debug off tb glbl -s tb > xelab.log
xsim tb -R | grep -E "^(TX:|RX|status|run [12]|ring|PASS|FAIL)"
