# Build the combined design: bitstream, probes and the hardware platform for the PS application.
#   vivado -mode batch -source hw/scripts/build.tcl [-tclargs <jobs>]
# Output: build/rdma_ofdm.bit / .ltx, build/rdma_ofdm.xsa (with the bitstream), timing and
# utilization reports. Resumes an existing project; runs a killed Vivado left "running" are reset.
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.
set here [file normalize [file join [file dirname [info script]] .. ..]]
set xpr  [file join $here build rdma_ofdm rdma_ofdm.xpr]
set jobs [expr {$argc > 0 ? [lindex $argv 0] : 8}]
set bf [expr {[info exists ::env(RFSOC4X2_BOARD_FILES)] ? $::env(RFSOC4X2_BOARD_FILES) : "$::env(HOME)/fpga/board_files"}]
set_param board.repoPaths [list $bf]
if {[file exists $xpr]} { open_project $xpr } else { source [file join $here hw scripts create_project.tcl] }

# sources changed since the last build: start that run (and the implementation) again
foreach r [get_runs] { if {[get_property NEEDS_REFRESH $r]} { reset_run $r } }
if {[get_property NEEDS_REFRESH [get_runs synth_1]] || [get_property PROGRESS [get_runs synth_1]] ne "100%"} { reset_run impl_1 }
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    foreach r [get_runs] { if {[get_property PROGRESS $r] ne "100%"} { reset_run $r } }
    launch_runs impl_1 -to_step write_bitstream -jobs $jobs
    wait_on_run impl_1
}
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} { error "impl_1 failed" }

open_run impl_1
report_timing_summary -file [file join $here build timing_summary.rpt]
report_utilization    -file [file join $here build utilization.rpt]
set impl_dir [get_property DIRECTORY [get_runs impl_1]]
file copy -force [file join $impl_dir rdma_ofdm_top.bit] [file join $here build rdma_ofdm.bit]
file copy -force [file join $impl_dir rdma_ofdm_top.ltx] [file join $here build rdma_ofdm.ltx]
write_hw_platform -fixed -include_bit -force -file [file join $here build rdma_ofdm.xsa]
puts "Bitstream: [file join $here build rdma_ofdm.bit]"
