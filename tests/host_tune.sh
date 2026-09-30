#!/bin/sh
# Host tuning for continuous 64 + 64 Gbit/s RDMA streaming (runtime only, undone by a reboot):
#   - CPU governor "performance" (TX producers that sleep come back on a slow clock otherwise)
#   - uncore clock scaling left on: fixing it at its maximum costs core clock (the RX
#     demodulators fall behind) and did not remove the DMA stalls
#   - package power limit PL1 / PL2 in watts (default 200 / 250 as set by the board firmware;
#     125 W is too little for the modem threads)
# CPUs 4-7 should also be isolated (kernel command line: isolcpus=4-7 nohz_full=4-7
# irqaffinity=0-3,8-19) for the RDMA engine and the TX threads.
#   sudo tests/host_tune.sh [PL1 [PL2]]
#   sudo tests/host_tune.sh off     (governor powersave, uncore scaling, 200 / 250 W)
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
set -e
U=/sys/devices/system/cpu/intel_uncore_frequency
R=/sys/class/powercap/intel-rapl:0
if [ "$1" = off ]; then
    for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo powersave > $c; done
    for d in $U/*/; do cat $d/initial_min_freq_khz > $d/min_freq_khz; done
    echo 200000000 > $R/constraint_0_power_limit_uw
    echo 250000000 > $R/constraint_1_power_limit_uw
else
    P1=${1:-200}; P2=${2:-250}
    for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $c; done
    for d in $U/*/; do cat $d/initial_min_freq_khz > $d/min_freq_khz; done
    echo $((P2 * 1000000)) > $R/constraint_1_power_limit_uw
    echo $((P1 * 1000000)) > $R/constraint_0_power_limit_uw
fi
echo "governor: $(sort -u /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | tr '\n' ' ')"
for d in $U/*/; do echo "uncore $(basename $d): $(cat $d/min_freq_khz) .. $(cat $d/max_freq_khz) kHz"; done
echo "power limit: PL1 $(($(cat $R/constraint_0_power_limit_uw) / 1000000)) W, PL2 $(($(cat $R/constraint_1_power_limit_uw) / 1000000)) W"
