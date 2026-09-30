![语言](https://img.shields.io/badge/语言-verilog_(IEEE1364_2001)-9A90FD.svg) ![RDMA](https://img.shields.io/badge/RDMA-RoCE_v2_(ERNIC)-orange.svg) ![RF](https://img.shields.io/badge/RF-2_GSPS_I/Q_(MTS)-purple.svg) ![部署](https://img.shields.io/badge/部署-vivado_2023.2-FF1010.svg) ![板卡](https://img.shields.io/badge/板卡-RFSoC_4x2-blue.svg) ![主机](https://img.shields.io/badge/主机-Linux_rdma--core-green.svg)

[English](#en) | [中文](#cn)

　

<span id="en">RFSoC 4x2: 2 GSPS OFDM with the host CPU as the modem, over 100G RDMA</span>
===========================

Continuous I/Q streaming between a Linux host and the RF data converters of the RFSoC 4x2 (XCZU48DR), carried by RoCE v2 (AMD ERNIC) on the QSFP28 port: **64 Gbit/s to the DACs and 64 Gbit/s from the ADCs at the same time, i.e. 2.0 GSPS × I/Q × 16 bit each way.** The OFDM transmitter and receiver run in C on the host CPU. Through a loopback cable (DAC_A → ADC_B for I, DAC_B → ADC_D for Q, multi-tile synchronised) the link carries raw, uncompressed video:

* **16-QAM**: 5.29 Gb/s of payload, 720p at 478 frames/s. In 60 s, 58 of 59 seconds were free of bit errors (BER < 10⁻⁸), 99.6 % of the video frames were byte-exact, and the overall BER was 3 × 10⁻⁵.
* **64-QAM**: 7.94 Gb/s of payload, 1080p at 319 frames/s. The median BER per second was 2 × 10⁻⁷ (the cable's SNR), and 97.4 % of the video frames were byte-exact.

It combines [rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic) (100G RDMA) with [rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts) (multi-tile sync, I/Q OFDM), both ported to Vivado 2023.2 and the RF side to 2.0 GSPS.

　

| ![video](./docs/img/ofdm_720p.gif) |
| :--------------------------------: |
| **Figure1** : 16-QAM, 720p: the video frame sent (left) and received (right), counters at that moment |

| ![video](./docs/img/ofdm_1080p_64qam.gif) |
| :---------------------------------------: |
| **Figure2** : 64-QAM, 1080p |

　

## How it works

```
host CPU: video -> scrambler -> OFDM modulators (3 threads) -> TX frame ring (16 MB) --RDMA WRITE--> rf_stream TX ring (2 MB) -> DAC_A / DAC_B
host CPU: checker <- OFDM demodulators (12 threads) <- RX ring (64 MB) <--RDMA WRITE WITH IMMEDIATE-- rf_stream RX ring (512 KB) <- ADC_B / ADC_D
```

* **rf_stream** (`hw/rtl/rf_stream.v`) sits in ERNIC's on-chip window at `0x8000_0000` behind an address split.
  * **TX**: the host RDMA WRITEs samples into a 2 MB TX ring (262 µs at 8 GB/s) and posts its write pointer. The player sends 512 bits every two 250 MHz RF cycles to the DACs (8 samples per rail per cycle). The host paces itself on the read pointer, read with an RDMA READ of a 64-byte status word.
  * **RX**: the ADCs fill 64 KB chunks. Every full chunk rings ERNIC's SQ doorbell over the hardware handshake ports. One of 1024 pre-filled RDMA WRITE WITH IMMEDIATE WQEs then sends the chunk to host ring slot *k* (immediate *k*), and its completion frees the chunk. No processor is involved on either path.
  * **Keeping time**: the TX read pointer never waits. A word the host has not written in time plays as zeros, so later samples stay on the time grid, and the host skips ahead. An RX overflow drops whole 512-bit words, which the FPGA counts.
  * **Restarting**: ERNIC keeps its SQ consumer index when a QP is configured again, so the SQ producer index runs on from run to run. The host stops cleanly, waiting until every RX chunk is completed.
* **Clocks**: ERNIC runs at 200 MHz and the RF fabric at 250 MHz (2.0 GSPS, 8 samples per cycle). The MTS block design (LMK04828 / LMX2594 from the A53, tile 2 PLL, SYSREF 5 MHz) comes from rfsoc4x2_mts.
* **Host modem** (`host/ofdm_modem.c`): N = 1024, CP = 128, 890 active sub-carriers (±16 … ±460) with 56 pilots. A frame of 32768 samples holds 2 training symbols, 26 data symbols and 512 zeros. The receiver uses a widely linear equaliser that also removes the I/Q image.
  * FFTW-based.
  * Transmitter: about 1 GS/s per core.
  * Receiver: about 310 MS/s per core on a Core Ultra 7 265K.
* **rf_ofdm** (`host/rf_ofdm.c`):
  * Sends the payload scrambled: flat video would otherwise put the same point on every sub-carrier, and the IFFT turns that into one huge peak.
  * Locks to the frame grid once. The ADCs and DACs share the sample clock, so the grid then holds.
  * Demodulates every frame at its known position, from a copy taken out of the ring.
  * Checks the sequence numbers and bit errors against the source, and reassembles the video.
  * Moves the grid by the exact shift after an RX overflow (16 samples per dropped word), and falls back to a full frame search if headers stay bad.

　

## Results

Host: Core Ultra 7 265K (8 P + 12 E cores), Mellanox ConnectX-4 (PCIe 3.0 x16), Ubuntu 24.04, CPUs 4-7 isolated. Board: RFSoC 4x2, SMA loopback DAC_A → ADC_B, DAC_B → ADC_D.

| Test | Result |
| :--- | :----- |
| Streaming (`rf_stream_host`, tone), 10 s | 64.00 Gbit/s to the DACs and 64.00 Gbit/s from the ADCs, 0 RX gaps, 0 TX underflows, 0 RX overflows |
| OFDM 16-QAM, raw 720p, 60 s | 61035 OFDM frames/s (2.0 GSPS), 5.29 Gb/s, 3.66 M OFDM frames, 58 / 59 s without errors, 28551 of 28660 video frames byte-exact, BER 3.1 × 10⁻⁵ (all from one RX overflow), 0 TX underflows ([log](./docs/results/ofdm_stream_16qam_720p_60s_log.txt)) |
| OFDM 64-QAM, raw 1080p, 60 s | 7.94 Gb/s, per-second BER median 2.3 × 10⁻⁷, 18621 of 19120 video frames byte-exact, BER 2.0 × 10⁻⁴ (mostly one RX stall) ([log](./docs/results/ofdm_stream_64qam_1080p_60s_log.txt)) |
| OFDM from the A53 (buffer mode, MTS) | 16-QAM 5.29 Gb/s 0 errors (EVM −27.4 dB), 64-QAM 7.94 Gb/s BER 4 × 10⁻⁴, 256-QAM 10.59 Gb/s BER 6 × 10⁻³ ([log](./docs/results/ofdm_2gsps_board_log.txt)) |
| MTS | DAC_B / ADC_D vs DAC_A / ADC_B after sync: +0.012 … +0.014 samples (6 … 7 ps) ([log](./docs/results/mts_2gsps_board_log.txt)) |
| Timing | all constraints met, WNS +0.098 ns, 81 % of the BRAM ([report](./docs/results/rdma_ofdm_timing_summary.rpt), [utilisation](./docs/results/rdma_ofdm_utilization.rpt)) |

**What the buffers are sized for.** With the CPU busy, the NIC's DMA pauses for up to ~0.3 ms every few seconds, occasionally for several ms. The status READ and the data WRITEs stall together.
* The 2 MB TX ring bridges these pauses. With 1 MB, every pause was a TX underflow.
* The 64 MB host RX ring (8 ms) covers demodulator threads that get preempted. With 16 MB, a few hundred frames per minute were overwritten before they were read.
* What is left is the rare pause longer than the 512 KB FPGA RX ring (65 µs): an RX overflow, after which the grid shift is exact.

A CPU package that reaches TjMax (105 °C) throttles, and each throttle event is such a pause. Keep the host cool, and keep sysfs reads (MSRs) off the RDMA engine's thread.

　

## Build and run

Requirements:
* Vivado / Vitis 2023.2 with an ERNIC licence.
* RFSoC 4x2 board files ([RealDigitalOrg/RFSoC4x2-BSP](https://github.com/RealDigitalOrg/RFSoC4x2-BSP), in `~/fpga/board_files`).
* rdma-core and libfftw3f.
* The host port at 192.168.100.2/24 with MTU 9000.
* For clean runs:
  * kernel command line `isolcpus=4-7 nohz_full=4-7 irqaffinity=0-3,8-19`, so the RDMA engine (CPU 7) and the TX threads (CPUs 4-6) run undisturbed;
  * `sudo tests/host_tune.sh` after every boot (governor `performance`).

```sh
git submodule update --init
vivado -mode batch -source hw/scripts/build.tcl -tclargs 4          # build/rdma_ofdm.bit, .xsa (4 parallel runs: 30 GB RAM)
xsct sw/create_vitis.tcl build/rdma_ofdm.xsa sw/vitis_rdma          # A53 application: clocks, RF tiles, MTS
host/build.sh                                                        # ofdm_bench, rf_stream_host, rf_ofdm
hw/sim/run.sh                                                        # rf_stream testbench (xsim)
tests/stream_up.sh 1 host/rf_stream_host --seconds 10 --tx tone:100  # program, MTS, configure ERNIC, stream a tone
tests/stream_up.sh 0 host/rf_ofdm --seconds 60 --save build/rx.yuv --save-frames 120 --save-every 225
tests/stream_up.sh 0 host/rf_ofdm --m 6 --video 1920x1080 --seconds 60
python3 host/make_gif.py build/rx.yuv 1280x720 docs/img/ofdm_720p.gif
```

* `tests/stream_up.sh 1` loads the bitstream and the A53 application over JTAG, then sends key `m` (MTS) over the UART.
* The host program writes its QP number, MAC and RX ring address to `build/stream_params.txt`. `tests/stream_config.tcl` programs ERNIC over JTAG-to-AXI (1024 WQEs) and signals the host once Vivado has let go of the CPU.
* Without a board:
  * `host/rf_ofdm --sim 1` loops the TX stream back in memory;
  * `--sim-slip N` shifts it like an RX overflow;
  * `host/ofdm_bench` measures the modem's speed.
* The standalone MTS design (buffer play / capture, A53 OFDM) is still built by `hw/scripts/make_mts.tcl` and `build_mts.tcl`.

　

## Files

| Path | |
| :--- | :-- |
| `hw/rtl/rdma_ofdm_top.v` | top level: CMAC, ERNIC, DDR4, address splits, MTS block design, rf_stream |
| `hw/rtl/rf_stream.v` | TX / RX rings, doorbells, status word |
| `hw/sim/` | rf_stream testbench |
| `hw/scripts/mts_bd.tcl` | MTS block design (RFDC, clocks, players, captures; DAC source mux and ADC taps when integrated) |
| `host/rf_ofdm.c` | continuous OFDM video link |
| `host/rf_stream_host.c` | streaming test (tone), with READ / WRITE latency statistics (`LAT=1`) |
| `host/rdma_link.c` | RC QP to ERNIC, clean stop |
| `host/ofdm_modem.c`, `ofdm_bench.c` | OFDM modem and its benchmark |
| `sw/src` | A53 application (clocks, RF tiles, MTS, buffer-mode OFDM) |
| `tests/stream_config.tcl`, `stream_up.sh`, `host_tune.sh` | ERNIC configuration over JTAG, bring-up, host tuning |

　

<span id="cn">RFSoC 4x2：主机 CPU 做调制解调的 2 GSPS OFDM（100G RDMA）</span>
===========================

Linux 主机与 RFSoC 4x2（XCZU48DR）射频数据转换器之间的连续 I/Q 流，由 QSFP28 口上的 RoCE v2（AMD ERNIC）承载：**同时 64 Gbit/s 送往 DAC、64 Gbit/s 来自 ADC，即双向 2.0 GSPS × I/Q × 16 bit。** OFDM 发射机和接收机都在主机 CPU 上用 C 实现。经过环回线缆（I：DAC_A → ADC_B，Q：DAC_B → ADC_D，多 tile 同步），链路传送未压缩的原始视频：

* **16-QAM**：净荷 5.29 Gb/s，720p 478 帧/s。60 s 中 58/59 秒无误码（BER < 10⁻⁸），99.6 % 的视频帧逐字节正确，总 BER 3 × 10⁻⁵。
* **64-QAM**：净荷 7.94 Gb/s，1080p 319 帧/s。每秒 BER 中位数 2 × 10⁻⁷（线缆 SNR 所限），97.4 % 的视频帧逐字节正确。

本仓库结合了 [rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic)（100G RDMA）与 [rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts)（多 tile 同步、I/Q OFDM），两者移植到 Vivado 2023.2，射频侧提到 2.0 GSPS。

## 原理

* **rf_stream**（`hw/rtl/rf_stream.v`）位于 ERNIC 片上窗口 `0x8000_0000`。
  * **TX**：主机用 RDMA WRITE 写 2 MB TX 环（8 GB/s 下可撑 262 µs）并更新写指针，播放器每两个 250 MHz 周期向 DAC 送 512 bit；主机通过 RDMA READ 64 字节状态字获得读指针来定速。
  * **RX**：ADC 填满 64 KB 块后，经硬件握手口敲 ERNIC 的 SQ 门铃，由 1024 个预置的 RDMA WRITE WITH IMMEDIATE WQE 发到主机环第 *k* 槽，完成后释放该块，全程无处理器参与。
  * **按时间走**：TX 读指针从不等待，主机没来得及写的字播放为零，后续样本保持在时间网格上，主机跳帧追上；RX 溢出按整个 512 bit 字丢弃，FPGA 记录丢弃数。
  * **重跑**：ERNIC 重新配置 QP 时保留 SQ 消费指针，因此 SQ 生产指针跨运行累加；主机退出前等所有 RX 块完成。
* **主机调制解调**（`host/ofdm_modem.c`）：N = 1024、CP = 128，890 个有效子载波，其中 56 个导频；每帧 32768 个样本。
  * 基于 FFTW。
  * 发射每核约 1 GS/s，接收每核约 310 MS/s。
  * 接收端用宽线性均衡器，同时去除 I/Q 镜像。
* **rf_ofdm**（`host/rf_ofdm.c`）：
  * 载荷先加扰码，避免平坦视频在 IFFT 后形成巨大峰值。
  * 只锁定一次帧网格：ADC 与 DAC 共用采样时钟，锁定后网格保持不变。
  * 每帧先从环中拷出再解调。
  * 对照源数据检查序号和误码，并重组视频。
  * RX 溢出后按精确平移量移动网格（每丢一个字 16 个样本），帧头持续错误时再做全搜索。

## 结果

| 测试 | 结果 |
| :--- | :--- |
| 流测试（单音）10 s | 双向 64.00 Gbit/s，0 RX gap，0 TX underflow，0 RX overflow |
| OFDM 16-QAM，原始 720p，60 s | 每秒 61035 个 OFDM 帧（2.0 GSPS），5.29 Gb/s，58/59 秒无误码，28660 帧视频中 28551 帧逐字节正确，BER 3.1 × 10⁻⁵（全部来自一次 RX 溢出），0 TX underflow |
| OFDM 64-QAM，原始 1080p，60 s | 7.94 Gb/s，每秒 BER 中位数 2.3 × 10⁻⁷，19120 帧中 18621 帧逐字节正确 |
| A53 OFDM（缓冲模式，MTS） | 16-QAM 5.29 Gb/s 0 误码（EVM −27.4 dB），64-QAM 7.94 Gb/s BER 4 × 10⁻⁴，256-QAM 10.59 Gb/s BER 6 × 10⁻³ |
| MTS | 同步后 DAC_B / ADC_D 相对 DAC_A / ADC_B：+0.012 … +0.014 样本（6 … 7 ps） |
| 时序 | 全部满足，WNS +0.098 ns，BRAM 81 % |

**缓冲区大小的依据**：CPU 满载时，网卡 DMA 每隔几秒会停顿最多约 0.3 ms，偶尔达到数 ms，状态读和数据写同时停。
* 2 MB 的 TX 环能跨过这些停顿；用 1 MB 时每次停顿都会造成 TX underflow。
* 64 MB 的主机 RX 环（8 ms）能扛住解调线程被抢占；用 16 MB 时每分钟有几百帧在读之前就被覆盖。
* 剩下的是少数长于 FPGA 512 KB RX 环（65 µs）的停顿，表现为 RX overflow，之后网格可以精确平移。

CPU 封装达到 TjMax（105 °C）时会热降频，每次降频就是一次这样的停顿，所以主机要做好散热，RDMA 引擎线程也不要读 sysfs（MSR）。

构建与运行步骤见上文英文部分（建议 `isolcpus=4-7`，每次开机后运行 `sudo tests/host_tune.sh`）。

　

## License

BSD 3-Clause, Copyright (c) 2026 Yijie Yu. `third_party/verilog-ethernet` (MIT) by Alex Forencich.
