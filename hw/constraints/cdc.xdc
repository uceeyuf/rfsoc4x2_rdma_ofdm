## The ERNIC clocks (from the 100 MHz board clock) and the RF fabric clocks (from the LMK04828
## PL_CLK) are unrelated. rf_stream crosses between clk_200 and clk_rf only through XPM CDC
## macros (which carry their own constraints) and dual-clock block RAMs.
set_clock_groups -asynchronous \
    -group [get_clocks -include_generated_clocks -of_objects [get_nets clk_200]] \
    -group [get_clocks -include_generated_clocks -of_objects [get_nets clk_rf]]
