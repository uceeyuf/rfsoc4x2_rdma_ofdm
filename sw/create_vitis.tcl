# Vitis 2023.2 (classic, xsct) workspace for the RFSoC4x2 MTS application.
#   xsct create_vitis.tcl [path/to/design.xsa [workspace]]
# (build/mts_wrapper.xsa into sw/vitis_ws by default; the RDMA OFDM design:
#  xsct sw/create_vitis.tcl build/rdma_ofdm.xsa sw/vitis_rdma; 4 GSPS:
#  MTS_GSPS=4.0 xsct sw/create_vitis.tcl build/mts4g_wrapper.xsa sw/vitis_mts4g)
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

set here [file normalize [file dirname [info script]]]
set xsa  [expr {[llength $argv] > 0 ? [file normalize [lindex $argv 0]] : "$here/../build/mts_wrapper.xsa"}]
set ws   [expr {[llength $argv] > 1 ? [file normalize [lindex $argv 1]] : "$here/vitis_ws"}]

file delete -force $ws
setws $ws

platform create -name mts_pf -hw $xsa -proc psu_cortexa53_0 -os standalone -out $ws
platform generate

app create -name mts_jtag -platform mts_pf -domain standalone_domain -template "Empty Application"
importsources -name mts_jtag -path $here/src
app config -name mts_jtag -add libraries metal
app config -name mts_jtag -add libraries m
# MTS_GSPS=4.0: the 4 GSPS standalone design (FS_HZ); OFDM_K_LO / OFDM_K_HI: the OFDM band
if {[info exists ::env(OFDM_K_LO)]} {
    app config -name mts_jtag -add define-compiler-symbols "K_LO=$::env(OFDM_K_LO)"
}
if {[info exists ::env(MTS_GSPS)]} {
    app config -name mts_jtag -add define-compiler-symbols "FS_HZ=$::env(MTS_GSPS)e9"
}
if {[info exists ::env(OFDM_K_HI)]} {
    app config -name mts_jtag -add define-compiler-symbols "K_HI=$::env(OFDM_K_HI)"
}
# 1 MB stack and heap (the template's 8 KB each overflow in the float printf of the reports)
app config -name mts_jtag -add linker-misc {-Wl,--defsym=_STACK_SIZE=0x100000 -Wl,--defsym=_HEAP_SIZE=0x100000}

# app build generates Debug/makefile; in batch mode it may drop the xsct channel before
# linking, so finish with make
catch {app build -name mts_jtag}
set elf $ws/mts_jtag/Debug/mts_jtag.elf
if {![file exists $elf]} {
    set vitis $::env(XILINX_VITIS)
    set ::env(PATH) "$vitis/gnu/aarch64/lin/aarch64-none/bin:$::env(PATH)"
    exec make -C $ws/mts_jtag/Debug all >@ stdout 2>@ stderr
}
puts "ELF: $elf"
