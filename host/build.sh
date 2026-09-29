#!/bin/sh
# Build the host OFDM tools. -fcx-limited-range lets the compiler inline complex multiply and
# divide (C99 complex otherwise calls __mulsc3 / __divsc3 for the NaN / Inf corner cases).
set -e
cd "$(dirname "$0")"
cc -O3 -march=native -fcx-limited-range -Wall -Wextra -o ofdm_bench ofdm_bench.c ofdm_modem.c -lm -lpthread
echo "built $(pwd)/ofdm_bench"
