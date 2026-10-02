#!/bin/sh
# Build the host streaming test.
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -Wextra -o rf_stream_host rf_stream_host.c rdma_link.c -libverbs -lm
echo "built rf_stream_host"
