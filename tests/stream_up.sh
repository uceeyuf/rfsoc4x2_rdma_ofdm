#!/bin/sh
# Bring up the RDMA streaming design and run a host program against it.
#
#   tests/stream_up.sh 1 host/rf_stream_host --seconds 10 --tx tone:100
#
# First argument 1: load the bitstream and the PS application over JTAG (sw/vitis_rdma: clocks and
# RF tiles at boot), then key m (multi-tile sync) over the UART; 0: the board is already up.
# The host program writes build/stream_params.txt, tests/stream_config.tcl programs ERNIC with it
# and creates the .ready file, and the host program runs.
#
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
set -e
cd "$(dirname "$0")/.."
PROG=${1:-0}
shift

IF=enp2s0np0
MTU=$(cat /sys/class/net/$IF/mtu 2>/dev/null || echo 0)
if [ "$MTU" -lt 4200 ]; then
    echo "$IF has MTU $MTU: RDMA needs >= 4200 (4096-byte packets); run the MTU script first" >&2
    exit 1
fi

if [ "$PROG" = 1 ]; then
    xsct sw/run_jtag.tcl sw/vitis_rdma
    sleep 8
    python3 host/uart.py --log build/uart_stream.log --quiet 4 m
fi

P=build/stream_params.txt
rm -f $P $P.ready
"$@" --params $P &
H=$!
while [ ! -f $P ]; do sleep 0.2; done
vivado -mode batch -nojournal -nolog -source tests/stream_config.tcl -tclargs $P > build/stream_config.log 2>&1 &
V=$!
wait $H
wait $V || true
grep -E "^  [A-Z]|err\[" build/stream_config.log | tail -24
