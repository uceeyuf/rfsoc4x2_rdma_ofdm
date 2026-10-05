![语言](https://img.shields.io/badge/语言-verilog_(IEEE1364_2001)-9A90FD.svg) ![RDMA](https://img.shields.io/badge/RDMA-RoCE_v2_(ERNIC)-orange.svg) ![RF](https://img.shields.io/badge/RF-2_GSPS_I/Q_(MTS)-purple.svg) ![部署](https://img.shields.io/badge/部署-vivado_2023.2-FF1010.svg) ![板卡](https://img.shields.io/badge/板卡-RFSoC_4x2-blue.svg) ![主机](https://img.shields.io/badge/主机-Linux_rdma--core-green.svg)

[English](#en) | [中文](#cn)

　

<span id="en">RFSoC 4x2: 2 GSPS I/Q streaming over 100G RDMA, Modem 2 on the board</span>
===========================

Continuous I/Q streaming between a Linux host and the RF data converters of the RFSoC 4x2 (XCZU48DR), carried by RoCE v2 (AMD ERNIC) on the QSFP28 port: **64 Gbit/s to the DACs and 64 Gbit/s from the ADCs at the same time, i.e. 2.0 GSPS × I/Q × 16 bit each way**, multi-tile synchronised. It combines [rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic) (100G RDMA) with [rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts) (multi-tile sync), both ported to Vivado 2023.2 and the RF side to 2.0 GSPS.

On this streaming core runs **Modem 2** ([zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm)), an OFDM modem for two independent ends (independent lasers or oscillators, independent sample clocks): the transmitter and, since October 2026, the receiver in the FPGA: in real time at 2 GSPS (61035 frames/s; 16-QAM, 5.10 Gb/s: 1.40 M frames, BER 5.0 × 10⁻¹⁰), at 4 GSPS in bursts. Through a loopback cable (DAC_A → ADC_B for I, DAC_B → ADC_D for Q) it carries scrambled 720p video at 16-QAM (5.10 Gb/s): one 24.5 ms burst of 1497 OFDM frames holds 11 consecutive video frames; over three bursts, 375 M bits without an error, all 32 video frames byte-exact. The modem's source (RTL, models) is not published.

　

| ![video](./docs/img/m2_720p_burst.gif) |
| :------------------------------------: |
| **Figure1** : Modem 2, 16-QAM, 720p, one 24.5 ms burst: the video frame sent (left) and received (right) |

　

## How it works

```
host: samples or payloads --RDMA WRITE--> rf_stream TX ring (2 MB) -> [Modem 2 transmitter] -> DAC_A / DAC_B
host: samples             <--RDMA WRITE WITH IMMEDIATE-- rf_stream RX ring (512 KB) <- ADC_B / ADC_D (+ ADC_A / ADC_C)
```

* **rf_stream** (`hw/rtl/rf_stream.v`) sits in ERNIC's on-chip window at `0x8000_0000` behind an address split.
  * **TX**: the host RDMA WRITEs into a 2 MB TX ring (262 µs at 8 GB/s) and posts its write pointer. The player sends 512 bits every two 250 MHz RF cycles to the DACs (8 samples per rail per cycle). The host paces itself on the read pointer, read with an RDMA READ of a 64-byte status word.
  * **RX**: the ADCs fill 64 KB chunks. Every full chunk rings ERNIC's SQ doorbell over the hardware handshake ports. One of 1024 pre-filled RDMA WRITE WITH IMMEDIATE WQEs then sends the chunk to host ring slot *k* (immediate *k*), and its completion frees the chunk. No processor is involved on either path.
  * **Keeping time**: the TX read pointer never waits. A word the host has not written in time plays as zeros, so later samples stay on the time grid, and the host skips ahead. An RX overflow drops whole 512-bit words, which the FPGA counts.
  * **Restarting**: ERNIC keeps its SQ consumer index when a QP is configured again, so the SQ producer index runs on from run to run. The host stops cleanly, waiting until every RX chunk is completed.
* **Clocks**: ERNIC runs at 200 MHz and the RF fabric at 250 MHz (2.0 GSPS, 8 samples per cycle). The MTS block design (LMK04828 / LMX2594 from the A53, tile 2 PLL, SYSREF 5 MHz) comes from rfsoc4x2_mts.
* **Modem 2** sits on the same core: its transmitter between the TX ring and the DACs, payloads in 32 KB slots; its receiver between the ADCs and the RX ring, which then carries payload slots instead of samples (or raw ADC captures, two or four ADCs, for the acquisition). The ADCs' background calibration converges on a tone asynchronous to their interleaving (frames muted) and is frozen before the frames start. Design and measurements: [zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm).

　

## Results

Host: Core Ultra 7 265K (8 P + 12 E cores), Mellanox ConnectX-4 (PCIe 3.0 x16), Ubuntu 24.04, CPUs 4-7 isolated. Board: RFSoC 4x2, SMA loopback DAC_A → ADC_B, DAC_B → ADC_D.

| Test | Result |
| :--- | :----- |
| Streaming (`rf_stream_host`, tone), 10 s | 64.00 Gbit/s to the DACs and 64.00 Gbit/s from the ADCs, 0 RX gaps, 0 TX underflows, 0 RX overflows |
| Modem 2 on the board (FPGA transmitter, receiver on raw ADC captures) | SMA loopback, the ADCs' background calibration converged on a tone asynchronous to their interleaving, then frozen. 2 GSPS: EVM −37.2 dB at 16 / 64 / 256-QAM; 16-QAM (5.10 Gb/s) 0 errors in 415 M bits (BER < 7.2 × 10⁻⁹), 64-QAM 0 / 7.7 M, 256-QAM (10.2 Gb/s) 0 / 10.2 M. 4 GSPS: 16-QAM −34.2 dB, 0 errors; 256-QAM (20.4 Gb/s) −34.4 dB, BER 4.6 × 10⁻⁵ |
| Modem 2 on the board, FPGA transmitter → cable → FPGA receiver, real time | 2 GSPS, 16-QAM (5.10 Gb/s), 30 s: 1.40 M frames, 59 bit errors in 117 G bits (BER 5.0 × 10⁻¹⁰); 64-QAM 2.7 × 10⁻⁷, 256-QAM 1.6 × 10⁻⁶. 4 GSPS receiver in bursts (an on-chip buffer of both branches, read at half speed): 16-QAM, 200 bursts, no error in 66.9 M bits |
| Modem 2, two independent ends, worst case (C model) | two 300 kHz lasers, carrier offset 5 MHz, sample clocks 50 ppm apart, IQ imbalance at both ends, SNR 30 dB, 16-QAM, receiver in fixed point, 12 seeds × 30 frames: BER 2.6 × 10⁻⁵ (1 polarization), 3.1 × 10⁻⁵ (2 polarizations, worst seed 5.3 × 10⁻⁵) |
| MTS | DAC_B / ADC_D vs DAC_A / ADC_B after sync: +0.012 … +0.014 samples (6 … 7 ps) ([log](./docs/results/mts_2gsps_board_log.txt)) |
| Timing, resources (streaming core) | all constraints met (WNS +0.141 ns); BRAM 64 %, UltraRAM 75 %, DSP 0.1 %, LUT 28 % ([report](./docs/results/rdma_ofdm_timing_summary.rpt), [utilisation](./docs/results/rdma_ofdm_utilization.rpt), [by instance](./docs/results/rdma_ofdm_utilization_hierarchical.rpt)) |

**What the buffers are sized for.** With the CPU busy, the NIC's DMA pauses for up to ~0.3 ms every few seconds, occasionally for several ms. The status READ and the data WRITEs stall together.
* The 2 MB TX ring bridges these pauses. With 1 MB, every pause was a TX underflow.
* The 64 MB host RX ring (8 ms) covers consumer threads that get preempted. With 16 MB, a few hundred chunks per minute were overwritten before they were read.
* What is left is the rare pause longer than the 512 KB FPGA RX ring (65 µs): an RX overflow, after which the sample grid shift is exact (16 samples per dropped word).

A CPU package that reaches TjMax (105 °C) throttles, and each throttle event is such a pause. Keep the host cool, and keep sysfs reads (MSRs) off the RDMA engine's thread.

　

## Spectrum and constellation

Modem 2 on the board: the FPGA transmitter, the ADCs' background calibration converged on its calibration tone and frozen, 64 frames of raw ADC samples of the running link, the receiver of [zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm) in fixed point (as in the FPGA).
* **Left**: the received PSD (blue) against the transmitter's own output, its bit-exact model at the same digital level (grey).
  * Welch estimate: 2048-point FFT, Hann window, 0.98 MHz bins.
  * Data on 62.5 … 898 MHz on both sides (804 data sub-carriers, 54 pilots); the RF pilot tone at +15.6 MHz.
  * The analogue path costs about 7 dB, and the band stays flat within about ±1 dB; the noise floor near DC and above ±898 MHz is about −78 dBFS.
* **Right**: every data sub-carrier symbol after the widely linear combiner and the per-symbol pilot phase, as a density plot, with the ideal points.
  * EVM is −37.2 dB for 16, 64 and 256-QAM alike: the link is limited by its SNR, not by the modulation. No errors at any of them (5.1 / 7.7 / 10.2 M bits).

| ![16-QAM](./docs/img/m2_16qam.png) |
| :--------------------------------: |
| **Figure2** : Modem 2, 16-QAM, EVM −37.2 dB |

| ![64-QAM](./docs/img/m2_64qam.png) |
| :--------------------------------: |
| **Figure3** : Modem 2, 64-QAM, EVM −37.2 dB |

| ![256-QAM](./docs/img/m2_256qam.png) |
| :----------------------------------: |
| **Figure4** : Modem 2, 256-QAM, EVM −37.2 dB |

　

## Build and run

Requirements:
* Vivado / Vitis 2023.2 with an ERNIC licence.
* RFSoC 4x2 board files ([RealDigitalOrg/RFSoC4x2-BSP](https://github.com/RealDigitalOrg/RFSoC4x2-BSP), in `~/fpga/board_files`).
* rdma-core.
* The host port at 192.168.100.2/24 with MTU 9000.
* For clean runs:
  * kernel command line `isolcpus=4-7 nohz_full=4-7 irqaffinity=0-3,8-19`, so the RDMA engine (CPU 7) runs undisturbed;
  * `sudo tests/host_tune.sh` after every boot (governor `performance`).

```sh
git submodule update --init
vivado -mode batch -source hw/scripts/build.tcl -tclargs 4          # build/rdma_ofdm.bit, .xsa (4 parallel runs: 30 GB RAM)
xsct sw/create_vitis.tcl build/rdma_ofdm.xsa sw/vitis_rdma          # A53 application: clocks, RF tiles, MTS
host/build.sh                                                        # rf_stream_host
hw/sim/run.sh                                                        # rf_stream testbench (xsim)
tests/stream_up.sh 1 host/rf_stream_host --seconds 10 --tx tone:100  # program, MTS, configure ERNIC, stream a tone
```

* `tests/stream_up.sh 1` loads the bitstream and the A53 application over JTAG, then sends key `m` (MTS) over the UART.
* The host program writes its QP number, MAC and RX ring address to `build/stream_params.txt`. `tests/stream_config.tcl` programs ERNIC over JTAG-to-AXI (1024 WQEs) and signals the host once Vivado has let go of the CPU.
* The standalone MTS design (buffer play / capture over JTAG, tone and chirp, alignment) is still built by `hw/scripts/make_mts.tcl` and `build_mts.tcl` (`MTS_GSPS=4.0` builds it at 4 GSPS); `sw/dump_captures.tcl` and `host/plot_captures.py` read and plot its captures.

　

## Files

| Path | |
| :--- | :-- |
| `hw/rtl/rdma_ofdm_top.v` | top level: CMAC, ERNIC, DDR4, address splits, MTS block design, rf_stream |
| `hw/rtl/rf_stream.v` | TX / RX rings (RX in UltraRAM), doorbells, status word |
| `hw/sim/` | rf_stream testbench |
| `hw/scripts/mts_bd.tcl` | MTS block design (RFDC, clocks, players, captures; DAC source mux and ADC taps when integrated) |
| `host/rf_stream_host.c` | streaming test (tone), with READ / WRITE latency statistics (`LAT=1`) |
| `host/rdma_link.c` | RC QP to ERNIC, clean stop |
| `host/plot_captures.py`, `uart.py` | MTS captures, the A53's UART |
| `sw/src` | A53 application (clocks, RF tiles, MTS, buffer play / capture, alignment) |
| `tests/stream_config.tcl`, `stream_up.sh`, `host_tune.sh` | ERNIC configuration over JTAG, bring-up, host tuning |

　

## Citation

If this work helps your research, please cite it:

```bibtex
@misc{yu2026rfsoc4x2_rdma_ofdm,
    author = {Yijie Yu},
    title = {{RFSoC 4x2: 2 GSPS I/Q streaming over 100G RDMA, Modem 2 on the board}},
    year = {2026},
    howpublished = {\url{https://github.com/uceeyuf/rfsoc4x2_rdma_ofdm}},
    note = {GitHub repository},
}
```

GitHub also offers the citation under **Cite this repository** (from [CITATION.cff](CITATION.cff)).

　

## Credits

* RDMA side: [rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic).
* RF side: [rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts).
* URAM player / capture blocks (`third_party/rfsoc_mts`) and the MTS design approach: [Xilinx/RFSoC-MTS](https://github.com/Xilinx/RFSoC-MTS) (MIT, `third_party/rfsoc_mts/LICENSE`).
* Clock register values: PYNQ RFSoC4x2 `LMK04828_500.0` / `LMX2594_500.0`.
* SPI driver: from [RFSoC4x2_clock_LMK_LMX](https://github.com/uceeyuf/RFSoC4x2_clock_LMK_LMX).
* RF data converter driver: Xilinx `rfdc`.
* Ethernet / AXI stream modules: Alex Forencich's [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet) (MIT, submodule pinned at `274831c`).
* PL DDR4 pin constraints (`hw/constraints/4x2_PL_DDR4.xdc`): the RFSoC 4x2 board's DDR4 constraints.

　

## License

* The files of this design are BSD 3-Clause, Copyright (c) 2026, Yijie Yu.
* `third_party/rfsoc_mts` (AMD) and verilog-ethernet are MIT.
* ERNIC, CMAC, MIG, the RF data converter and the other AMD IP are generated from their configuration scripts and are not included; they need their own licenses.
* Modem 2 (transmitter, receiver, models) is not published here; figures and logs: Copyright (c) 2026, Yijie Yu.

　

<span id="cn">RFSoC 4x2：100G RDMA 上的 2 GSPS I/Q 流，Modem 2 上板</span>
===========================

Linux 主机与 RFSoC 4x2（XCZU48DR）射频数据转换器之间的连续 I/Q 流，由 QSFP28 口上的 RoCE v2（AMD ERNIC）承载：**同时 64 Gbit/s 送往 DAC、64 Gbit/s 来自 ADC，即双向 2.0 GSPS × I/Q × 16 bit**，多 tile 同步。本仓库结合了 [rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic)（100G RDMA）与 [rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts)（多 tile 同步），两者移植到 Vivado 2023.2，射频侧提到 2.0 GSPS。

在这个流式核心上运行 **Modem 2**（[zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm)）：面向两端独立（激光器或本振独立、采样时钟独立）的 OFDM 调制解调，发射机与（2026 年 10 月起）接收机都在 FPGA 内：2 GSPS 实时（每秒 61035 帧；16-QAM，5.10 Gb/s：140 万帧，BER 5.0 × 10⁻¹⁰），4 GSPS 为突发接收。经过环回线缆（I：DAC_A → ADC_B，Q：DAC_B → ADC_D），以 16-QAM（5.10 Gb/s）传送加扰后的 720p 视频：一次 24.5 ms、1497 个 OFDM 帧的 burst 含 11 帧连续视频；三次 burst 共 375 M bit 零误码，32 帧视频全部逐字节正确。调制解调的源码（RTL、模型）不公开。

见上文图 1。

## 原理

* **rf_stream**（`hw/rtl/rf_stream.v`）位于 ERNIC 片上窗口 `0x8000_0000`。
  * **TX**：主机用 RDMA WRITE 写 2 MB TX 环（8 GB/s 下可撑 262 µs）并更新写指针，播放器每两个 250 MHz 周期向 DAC 送 512 bit；主机通过 RDMA READ 64 字节状态字获得读指针来定速。
  * **RX**：ADC 填满 64 KB 块后，经硬件握手口敲 ERNIC 的 SQ 门铃，由 1024 个预置的 RDMA WRITE WITH IMMEDIATE WQE 发到主机环第 *k* 槽，完成后释放该块，全程无处理器参与。
  * **按时间走**：TX 读指针从不等待，主机没来得及写的字播放为零，后续样本保持在时间网格上，主机跳帧追上；RX 溢出按整个 512 bit 字丢弃，FPGA 记录丢弃数。
  * **重跑**：ERNIC 重新配置 QP 时保留 SQ 消费指针，因此 SQ 生产指针跨运行累加；主机退出前等所有 RX 块完成。
* **时钟**：ERNIC 200 MHz，射频侧 250 MHz（2.0 GSPS，每周期 8 个样本）。MTS block design（A53 配置 LMK04828 / LMX2594，tile 2 PLL，SYSREF 5 MHz）来自 rfsoc4x2_mts。
* **Modem 2** 接在同一个核心上：发射机位于 TX 环与 DAC 之间，净荷按 32 KB 槽放；接收机位于 ADC 与 RX 环之间，RX 环随后传净荷槽而不是样本（捕获阶段则传两路或四路 ADC 的原始采集）。ADC 后台校准先在与其交织不同步的单音上收敛（帧静音），冻结后再发帧。设计与测量见 [zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm)。

## 结果

| 测试 | 结果 |
| :--- | :--- |
| 流测试（单音）10 s | 双向 64.00 Gbit/s，0 RX gap，0 TX underflow，0 RX overflow |
| Modem 2 上板（FPGA 发射，接收机解原始 ADC 采集） | SMA 环回，ADC 后台校准先在与其交织不同步的单音上收敛，然后冻结。2 GSPS：16 / 64 / 256-QAM 的 EVM 都是 −37.2 dB；16-QAM（5.10 Gb/s）415 M bit 零误码（BER < 7.2 × 10⁻⁹），64-QAM 0 / 7.7 M，256-QAM（10.2 Gb/s）0 / 10.2 M。4 GSPS：16-QAM −34.2 dB，零误码；256-QAM（20.4 Gb/s）−34.4 dB，BER 4.6 × 10⁻⁵ |
| Modem 2 上板，FPGA 发射 → 线缆 → FPGA 接收，实时 | 2 GSPS，16-QAM（5.10 Gb/s），30 s：140 万帧，1170 亿 bit 中 59 个误码（BER 5.0 × 10⁻¹⁰）；64-QAM 2.7 × 10⁻⁷，256-QAM 1.6 × 10⁻⁶。4 GSPS 突发接收（两条支路存进片上缓冲，半速读出）：16-QAM，200 次突发，66.9 M bit 零误码 |
| Modem 2，两端独立，最坏情况（C 模型） | 两个 300 kHz 激光器，载波频偏 5 MHz，采样时钟相差 50 ppm，两端 IQ 失衡，SNR 30 dB，16-QAM，接收机定点，12 个种子 × 30 帧：BER 2.6 × 10⁻⁵（1 个偏振），3.1 × 10⁻⁵（2 个偏振，最差种子 5.3 × 10⁻⁵） |
| MTS | 同步后 DAC_B / ADC_D 相对 DAC_A / ADC_B：+0.012 … +0.014 样本（6 … 7 ps） |
| 时序、资源（流式核心） | 全部满足（WNS +0.141 ns）；BRAM 64 %，UltraRAM 75 %，DSP 0.1 %，LUT 28 % |

**缓冲区大小的依据**：CPU 满载时，网卡 DMA 每隔几秒会停顿最多约 0.3 ms，偶尔达到数 ms，状态读和数据写同时停。
* 2 MB 的 TX 环能跨过这些停顿；用 1 MB 时每次停顿都会造成 TX underflow。
* 64 MB 的主机 RX 环（8 ms）能扛住处理线程被抢占；用 16 MB 时每分钟有几百个块在读之前就被覆盖。
* 剩下的是少数长于 FPGA 512 KB RX 环（65 µs）的停顿，表现为 RX overflow，之后样本网格可以精确平移（每丢一个字 16 个样本）。

CPU 封装达到 TjMax（105 °C）时会热降频，每次降频就是一次这样的停顿，所以主机要做好散热，RDMA 引擎线程也不要读 sysfs（MSR）。

## 频谱与星座图

Modem 2 上板：FPGA 发射机，ADC 后台校准先在其校准单音上收敛并冻结，取运行中链路的 64 帧原始 ADC 样本，由 [zcu208_4gsps_ofdm](https://github.com/uceeyuf/zcu208_4gsps_ofdm) 的接收机按 FPGA 的定点处理（见上文图 2–4）。
* **左图**：接收 PSD（蓝）与发射机自身输出（其位精确模型，相同数字电平，灰）的对比。
  * Welch 估计：2048 点 FFT，Hann 窗，每格 0.98 MHz。
  * 数据在两侧 62.5 … 898 MHz（804 个数据子载波，54 个导频）；射频导频单音在 +15.6 MHz。
  * 模拟链路损耗约 7 dB，带内平坦度约 ±1 dB；DC 附近和 ±898 MHz 以外噪底约 −78 dBFS。
* **右图**：宽线性合并与逐符号导频相位校正之后的全部数据子载波符号密度图，叠加理想星座点。
  * 16 / 64 / 256-QAM 的 EVM 都是 −37.2 dB，说明链路受 SNR 限制，与调制阶数无关。三种调制都没有误码（5.1 / 7.7 / 10.2 M bit）。

构建与运行步骤见上文英文部分（建议 `isolcpus=4-7`，每次开机后运行 `sudo tests/host_tune.sh`）。

　

## 引用

如果这个项目对你的研究有帮助，请引用：

```bibtex
@misc{yu2026rfsoc4x2_rdma_ofdm,
    author = {Yijie Yu},
    title = {{RFSoC 4x2: 2 GSPS I/Q streaming over 100G RDMA, Modem 2 on the board}},
    year = {2026},
    howpublished = {\url{https://github.com/uceeyuf/rfsoc4x2_rdma_ofdm}},
    note = {GitHub repository},
}
```

GitHub 仓库页的 **Cite this repository** 也提供同样的引用（来自 [CITATION.cff](CITATION.cff)）。

　

## 致谢

* RDMA 部分：[rfsoc4x2_ernic](https://github.com/uceeyuf/rfsoc4x2_ernic)。
* 射频部分：[rfsoc4x2_mts](https://github.com/uceeyuf/rfsoc4x2_mts)。
* URAM 播放 / 采集模块（`third_party/rfsoc_mts`）和 MTS 设计思路：[Xilinx/RFSoC-MTS](https://github.com/Xilinx/RFSoC-MTS)（MIT，`third_party/rfsoc_mts/LICENSE`）。
* 时钟寄存器值：PYNQ RFSoC4x2 `LMK04828_500.0` / `LMX2594_500.0`。
* SPI 驱动：来自 [RFSoC4x2_clock_LMK_LMX](https://github.com/uceeyuf/RFSoC4x2_clock_LMK_LMX)。
* RF 数据转换器驱动：Xilinx `rfdc`。
* 以太网 / AXI stream 模块：Alex Forencich 的 [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet)（MIT，子模块 `274831c`）。
* PL DDR4 管脚约束（`hw/constraints/4x2_PL_DDR4.xdc`）：RFSoC 4x2 板卡的 DDR4 约束。

　

## 许可证

* 本设计的文件采用 BSD 3-Clause，版权所有 (c) 2026 Yijie Yu。
* `third_party/rfsoc_mts`（AMD）与 verilog-ethernet 为 MIT。
* ERNIC、CMAC、MIG、RF 数据转换器等 AMD IP 由配置脚本生成，不包含在仓库中，需要各自的许可证。
* Modem 2（发射机、接收机、模型）不在本仓库公开；图和日志：版权所有 (c) 2026 Yijie Yu。
