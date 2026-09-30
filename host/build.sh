#!/bin/sh
# Build the host OFDM tools. -fcx-limited-range lets the compiler inline complex multiply and
# divide (C99 complex otherwise calls __mulsc3 / __divsc3 for the NaN / Inf corner cases).
# FFTW (libfftw3f) is used when it is installed (the runtime library is enough).
set -e
cd "$(dirname "$0")"
FFT=""
if [ -e /usr/lib/x86_64-linux-gnu/libfftw3f.so.3 ] || ldconfig -p 2>/dev/null | grep -q 'libfftw3f\.so\.3 '; then
    FFT="-DOFDM_FFTW -l:libfftw3f.so.3"
fi
cc -O3 -march=native -fcx-limited-range -Wall -Wextra -o ofdm_bench ofdm_bench.c ofdm_modem.c $FFT -lm -lpthread
echo "built $(pwd)/ofdm_bench${FFT:+ (FFTW)}"
cc -O3 -g -march=native -fcx-limited-range -Wall -Wextra -o rf_ofdm rf_ofdm.c ofdm_modem.c rdma_link.c $FFT -libverbs -lm -lpthread
cc -O2 -Wall -Wextra -o rf_stream_host rf_stream_host.c rdma_link.c -libverbs -lm
echo "built rf_ofdm, rf_stream_host"
cc -O2 -march=native -fcx-limited-range -Wall -Wextra -o ofdm_plots ofdm_plots.c ofdm_modem.c $FFT -lm
echo "built ofdm_plots"
