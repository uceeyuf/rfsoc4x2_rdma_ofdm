# RFSoC4x2 multi-tile synchronization over JTAG at 2.0 GSPS, Vivado 2023.2 (no PYNQ).
# Ported from rfsoc4x2_mts (4.0 GSPS, Vivado 2020.2).
#
#   vivado -mode batch -source hw/scripts/make_mts.tcl                 (project + block design)
#   vivado -mode batch -source hw/scripts/make_mts.tcl -tclargs build  (+ bitstream + .xsa)
#
# 2.0 GSPS. DAC tiles 230 (DAC_A = I) and 228 (DAC_B = Q) play from one URAM; ADC tiles
# 224 (ADC_D, ADC_C) and 226 (ADC_B, ADC_A) are captured into URAM, 64 k samples each,
# on the same SYSREF-aligned fabric cycle. Tile 2 PLLs run from the 500 MHz LMX2594
# reference and distribute the sample clock to tiles 1 and 0. PL_CLK / PL_SYSREF come
# from the LMK04828 (500 MHz). RF fabric 250 MHz (8 samples / cycle), player and captures
# 125 MHz. Clock chips are programmed by the bare-metal app (PS SPI0).
#
# The RFSoC 4x2 board files (realdigital.org:rfsoc4x2:part0:1.0) are looked up in
# $RFSOC4X2_BOARD_FILES, default ~/fpga/board_files
# (https://github.com/RealDigitalOrg/RFSoC4x2-BSP, board_files/).
#
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

set build [expr {[llength $argv] > 0 && [lindex $argv 0] eq "build"}]

set here     [file normalize [file dirname [info script]]/../..]
set proj_dir $here/build/mts
set bd_name  mts
set bf [expr {[info exists ::env(RFSOC4X2_BOARD_FILES)] ? $::env(RFSOC4X2_BOARD_FILES) : "$::env(HOME)/fpga/board_files"}]
set_param board.repoPaths [list $bf]

create_project mts_jtag $proj_dir -part xczu48dr-ffvg1517-2-e -force
set_property board_part realdigital.org:rfsoc4x2:part0:1.0 [current_project]
set_property target_language Verilog [current_project]

add_files -norecurse [list \
    $here/third_party/rfsoc_mts/DACRAMstreamer.v \
    $here/third_party/rfsoc_mts/ADCRAMcapture.v \
    $here/hw/rtl/mts_sync.v \
    $here/hw/rtl/cap_gate.v \
    $here/hw/rtl/clk_meter.v]
add_files -fileset constrs_1 -norecurse $here/hw/constraints/mts.xdc
update_compile_order -fileset sources_1

set integrate 0
source [file join $here hw scripts mts_bd.tcl]
validate_bd_design
save_bd_design
make_wrapper -files [get_files $bd_name.bd] -top -import
set_property top ${bd_name}_wrapper [current_fileset]
update_compile_order -fileset sources_1

if {$build} {
    launch_runs impl_1 -to_step write_bitstream -jobs 8
    wait_on_run impl_1
    if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
        error "implementation failed"
    }
    open_run impl_1
    report_timing_summary -file $here/build/timing_summary.rpt
    report_utilization -file $here/build/utilization.rpt
    write_hw_platform -fixed -include_bit -force -file $here/build/mts_wrapper.xsa
}
