# Configure ERNIC over JTAG for continuous I/Q streaming (rf_stream), then dump its state.
#
#   vivado -mode batch -source tests/stream_config.tcl -tclargs <host params file> [program]
#   vivado -mode batch -source tests/stream_config.tcl -tclargs status [program]
#
# The params file comes from host/rf_stream_host (host QPN, MAC, PSN, RX ring VA and R_Key);
# when ERNIC is configured the script creates "<file>.ready" and the host connects.
# program = 1 loads build/rdma_ofdm.bit (the PS application still has to set up the clocks,
# the RF tiles and MTS: sw/run_jtag.tcl, then key m).
#
# Memory:
#   ERNIC 0x8000_0000  rf_stream window = memory region 0 (VA = PA, R_Key 0x12): TX ring 2 MB,
#                      RX ring 512 KB (8 chunks) at +0x20_0000, control / status at +0x28_0000
#                      (RX_CHUNKS below must match rf_stream's parameter)
#   DDR4  0x0010_0000  error buffer     0x0014_0000 response errors    0x0050_0000 data buffers
#         0x0020_0000  doorbells        0x0030_0000 QP1 RQ / SQ / CQ
#         0x0040_0000  QP2 RQ (unused)  0x0048_0000 QP2 SQ: 1024 WQEs   0x004A_0000 QP2 CQ
#
# QP2 sends: WQE k is an RDMA WRITE WITH IMMEDIATE of RX chunk k mod RX_CHUNKS (64 KB) to host ring slot
# k (host VA + k x 64 KB), immediate data k. rf_stream rings the SQ doorbell over the handshake
# ports (QPCONF[4] = 0) as chunks fill, and frees them on the CQ doorbell (address 0x004).
#
# Copyright (c) 2026, Yijie Yu. BSD-3-Clause.

set repo_dir [file normalize [file join [file dirname [info script]] ..]]
set arg0 [expr {$argc > 0 ? [lindex $argv 0] : "status"}]
set prog [expr {$argc > 1 ? [lindex $argv 1] : 0}]

set FPGA_MAC  0x020000000001
set FPGA_IP   0xC0A86401              ;# 192.168.100.1
set HOST_IP   0xC0A86402              ;# 192.168.100.2
set WIN       0x80000000              ;# rf_stream window
set WIN_LEN   0x290000
set MR_KEY    0x12
set UDP_SPORT 0xC000
set NUM_QP    8
set SQ_DEPTH  1024                  ;# = rf_stream SQ_DEPTH = host RL_SLOTS
set CHUNK     0x10000
set RX_CHUNKS 8
set RX_RING   [expr {$WIN + 0x200000}]
set SQ_BASE   0x00480000

open_hw_manager
connect_hw_server -allow_non_jtag
open_hw_target
set dev [lindex [get_hw_devices xczu48dr*] 0]
current_hw_device $dev
set bitf [expr {[info exists ::env(BIT)] ? $::env(BIT) : [file join $repo_dir build rdma_ofdm.bit]}]
set ltxf [file rootname $bitf].ltx
if {[file exists $ltxf]} {
    set_property PROBES.FILE $ltxf $dev
    set_property FULL_PROBES.FILE $ltxf $dev
}
if {$prog} {
    set_property PROGRAM.FILE $bitf $dev
    program_hw_devices $dev
    after 3000
}
refresh_hw_device $dev

set LITE ""; set MEM ""
foreach a [get_hw_axis] {
    if {[string match *jtag_lite* [get_property CELL_NAME $a]]} { set LITE $a } else { set MEM $a }
}
set vio [lindex [get_hw_vios -of_objects $dev] 0]

set ::txn 0
proc wr {axi addr data} {
    set t [create_hw_axi_txn t[incr ::txn] $axi -type write -address [format %08x $addr] \
        -data [format %08x [expr {$data & 0xFFFFFFFF}]] -len 1]
    run_hw_axi -quiet $t
    delete_hw_axi_txn $t
}
proc rd {axi addr} {
    set t [create_hw_axi_txn t[incr ::txn] $axi -type read -address [format %08x $addr] -len 1]
    run_hw_axi -quiet $t
    set v [get_property DATA $t]
    delete_hw_axi_txn $t
    return [expr {"0x$v"}]
}
# words: 32-bit values from the lowest address up (the data string starts with the highest)
proc wr_words {axi addr words} {
    set d ""
    for {set i [expr {[llength $words] - 1}]} {$i >= 0} {incr i -1} { append d [format %08x [expr {[lindex $words $i] & 0xFFFFFFFF}]] }
    set t [create_hw_axi_txn t[incr ::txn] $axi -type write -address [format %08x $addr] -data $d -len [llength $words]]
    run_hw_axi -quiet $t
    delete_hw_axi_txn $t
}
proc zero {axi addr words} {
    for {set i 0} {$i < $words} {incr i 64} {
        wr_words $axi [expr {$addr + 4 * $i}] [lrepeat [expr {min(64, $words - $i)}] 0]
    }
}
proc reg  {addr val} { wr $::LITE $addr $val }
proc qreg {qp off val} { wr $::LITE [expr {0x180000 + $qp * 0x100 + $off}] $val }
proc qget {qp off} { return [rd $::LITE [expr {0x180000 + $qp * 0x100 + $off}]] }
proc mac_regs {mac} { return [list [expr {$mac & 0xFFFFFFFF}] [expr {($mac >> 32) & 0xFFFF}]] }

proc dump {} {
    foreach {n a} {XRNICCONF 0x100000 LSTINPKT 0x100110 LSTOUTPKT 0x100114 ININVDUP 0x100118
                   INALLDRP 0x100130 OUTACKMAD 0x10010C} {
        puts [format "  %-12s 0x%08x" $n [rd $::LITE $a]]
    }
    puts "  QP2:"
    foreach {n o} {QPCONF 0x00 SQPSN 0x40 LSTRQREQ 0x44 DESTQP 0x48 CQHEAD 0x30 SQPI 0x38
                   STATQP 0x88 STATCURSQPTR 0x8C STATRESPSN 0x90} {
        puts [format "    %-13s 0x%08x" $n [qget 2 $o]]
    }
}

if {$arg0 eq "status"} {
    dump
    close_hw_manager
    return
}

# ---------------------------------------------------------------- host side
set f [open $arg0 r]
while {[gets $f line] >= 0} {
    lassign $line k v
    set host($k) $v
}
close $f
set hmac 0x[string map {: ""} $host(mac)]
puts [format "host: QPN %d  MAC %012x  PSN 0x%x  RX ring VA 0x%x R_Key 0x%x" \
    $host(qpn) $hmac $host(psn) $host(rx_va) $host(rx_rkey)]

# ---------------------------------------------------------------- DDR: doorbells, queues, errors
zero $MEM 0x00200000 64
zero $MEM 0x00202000 64
zero $MEM 0x00100000 128
foreach b {0x00310000 0x00320000} { zero $MEM $b 1024 }
zero $MEM 0x004A0000 [expr {$SQ_DEPTH * 8}]                ;# QP2 CQ

# ---------------------------------------------------------------- global
reg 0x100000 0
reg 0x100004 [expr {10 << 16}]                             ;# 200 MHz
reg 0x100044 [expr {$NUM_QP - 1}]                         ;# XRNIC_CONF_QP_EN
lassign [mac_regs $FPGA_MAC] ml mh
reg 0x100010 $ml
reg 0x100014 $mh
reg 0x100070 $FPGA_IP
reg 0x100060 0x00100000 ; reg 0x100064 0 ; reg 0x100068 [expr {(256 << 16) | 64}]
reg 0x1000B0 0x00140000 ; reg 0x1000B4 0 ; reg 0x1000B8 [expr {(256 << 16) | 64}]
reg 0x1000A0 0x00500000 ; reg 0x1000A4 0 ; reg 0x1000A8 [expr {(4096 << 16) | 64}]
reg 0x100180 0

# ---------------------------------------------------------------- memory region 0: rf_stream
reg 0x00 0
reg 0x04 $WIN ; reg 0x08 0
reg 0x0C $WIN ; reg 0x10 0
reg 0x14 $MR_KEY
reg 0x18 $WIN_LEN
reg 0x1C 2

# ---------------------------------------------------------------- QP1 (management datagrams)
set QPEN [expr {1 | (1 << 4) | (1 << 5)}]
qreg 1 0x08 0x00300000 ; qreg 1 0x10 0x00310000 ; qreg 1 0x18 0x00320000
qreg 1 0x20 [expr {0x00200000 + 0x10 + 8}] ; qreg 1 0x28 [expr {0x00200000 + 0x10}]
qreg 1 0x3C [expr {(16 << 16) | 16}]
qreg 1 0x00 [expr {$QPEN | (1 << 16)}]

# ---------------------------------------------------------------- QP2: streams the RX chunks
# WQE k: WRID k, local address RX chunk k mod RX_CHUNKS, length 64 KB, opcode 0x01 (WRITE WITH
# IMMEDIATE), remote address host ring slot k, R_Key, immediate k
for {set k0 0} {$k0 < $SQ_DEPTH} {incr k0 16} {
    set words {}
    for {set k $k0} {$k < $k0 + 16} {incr k} {
        set ra [expr {$host(rx_va) + $k * $CHUNK}]
        set w [lrepeat 16 0]
        lset w 0 $k
        lset w 1 [expr {$RX_RING + ($k % $RX_CHUNKS) * $CHUNK}]
        lset w 3 $CHUNK
        lset w 4 0x01
        lset w 5 [expr {$ra & 0xFFFFFFFF}]
        lset w 6 [expr {($ra >> 32) & 0xFFFFFFFF}]
        lset w 7 $host(rx_rkey)
        lset w 12 $k
        set words [concat $words $w]
    }
    wr_words $MEM [expr {$SQ_BASE + $k0 * 64}] $words
}
qreg 2 0x04 [expr {(0xFFFF << 16) | (64 << 8)}]            ;# P_Key, TTL
qreg 2 0x08 0x00400000 ; qreg 2 0x10 $SQ_BASE ; qreg 2 0x18 0x004A0000
qreg 2 0x20 0x00200004 ; qreg 2 0x28 0x00202004            ;# doorbell addresses end in 0x004
qreg 2 0x3C [expr {(16 << 16) | $SQ_DEPTH}]               ;# RQ depth 16 (unused), SQ depth
qreg 2 0x40 $host(psn)
qreg 2 0x44 [expr {(($host(psn) - 1) & 0xFFFFFF) | (4 << 24)}]
qreg 2 0x48 $host(qpn)
qreg 2 0x4C [expr {18 | (7 << 8) | (7 << 11) | (1 << 16)}] ;# timeout, retries, RNR retries, RNR timer 0.01 ms
lassign [mac_regs $hmac] ml mh
qreg 2 0x50 $ml ; qreg 2 0x54 $mh
qreg 2 0x60 $HOST_IP
qreg 2 0xB0 0                                              ;# PD 0
qreg 2 0x00 [expr {1 | (1 << 5) | (4 << 8) | (16 << 16)}]  ;# HW handshake doorbells, CQE writes, PMTU 4096

reg 0x100000 [expr {1 | (1 << 5) | ($UDP_SPORT << 8)}]
dump
puts "ERNIC configured"

# WATCH=n: n seconds of VIO counters first (Vivado then competes with the host for the CPU,
# which shows as TX underflows); the host starts on "<params>.ready", written when Vivado is done
set secs [expr {[info exists ::env(WATCH)] ? $::env(WATCH) : 0}]
if {$secs == 0} {
    close_hw_manager
    set rf [open "$arg0.ready" w]; close $rf
    puts "$arg0.ready written"
    return
}
set rf [open "$arg0.ready" w]; close $rf
for {set t 1} {$t <= $secs} {incr t} {
    after 1000
    if {$vio eq ""} continue
    refresh_hw_vio $vio
    set line "  t$t"
    foreach n {c_mac_rx c_roce_rx c_mac_tx c_rxq_drop} {
        set p [get_hw_probes $n -of_objects $vio]
        set_property INPUT_VALUE_RADIX UNSIGNED $p
        append line "  $n [get_property INPUT_VALUE $p]"
    }
    puts $line
}
dump
for {set e 0} {$e < 2} {incr e} {
    set t [create_hw_axi_txn e[incr ::txn] $MEM -type read -address [format %08x [expr {0x00100000 + $e * 256}]] -len 16]
    run_hw_axi -quiet $t
    set d [get_property DATA $t]
    delete_hw_axi_txn $t
    puts "  err\[$e\]: syndrome 0x[string range $d end-7 end]"
}
close_hw_manager
