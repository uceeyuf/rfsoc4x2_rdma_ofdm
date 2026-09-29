# Build the MTS design (bitstream + .xsa), resuming an existing project if there is one.
#
#   vivado -mode batch -source hw/scripts/build_mts.tcl [-tclargs <jobs>]
#
# Output: build/mts_wrapper.xsa (with the bitstream), build/timing_summary.rpt, build/utilization.rpt
#
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

set here [file normalize [file dirname [info script]]/../..]
set xpr  $here/build/mts/mts_jtag.xpr
set jobs [expr {$argc > 0 ? [lindex $argv 0] : 8}]
set bf [expr {[info exists ::env(RFSOC4X2_BOARD_FILES)] ? $::env(RFSOC4X2_BOARD_FILES) : "$::env(HOME)/fpga/board_files"}]
set_param board.repoPaths [list $bf]

if {[file exists $xpr]} {
    open_project $xpr
} else {
    source $here/hw/scripts/make_mts.tcl
}
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    # a run left "running" by a Vivado that was killed (the IP runs included) blocks the scheduler:
    # reset every run that is not finished
    foreach r [get_runs] {
        if {[get_property PROGRESS $r] ne "100%"} { reset_run $r }
    }
    launch_runs impl_1 -to_step write_bitstream -jobs $jobs
    wait_on_run impl_1
}
if {[get_property PROGRESS [get_runs impl_1]] ne "100%"} {
    error "implementation failed"
}
open_run impl_1
report_timing_summary -file $here/build/timing_summary.rpt
report_utilization -file $here/build/utilization.rpt
write_hw_platform -fixed -include_bit -force -file $here/build/mts_wrapper.xsa
puts "XSA: $here/build/mts_wrapper.xsa"
