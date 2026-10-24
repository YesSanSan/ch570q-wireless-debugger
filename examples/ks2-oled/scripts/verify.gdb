set pagination off
set confirm off
set remotetimeout 10
file examples/ks2-oled/build/ks2-oled.elf
target remote localhost:3333
monitor reset halt
hbreak main
continue
printf "BREAK_MAIN pc=0x%x\n", $pc
delete breakpoints
hbreak oled_update
continue
printf "BREAK_OLED pc=0x%x ready=%u frame=%u fault=%u\n", $pc, g_oled_ready, g_frame_count, g_fault
if g_oled_ready != 1 || g_fault != 0
  echo FAIL: OLED initialization or target fault\n
  quit 1
end
set $before_pc = $pc
stepi
printf "SINGLE_STEP before=0x%x after=0x%x\n", $before_pc, $pc
if $before_pc == $pc
  echo FAIL: single step did not move PC\n
  quit 1
end
delete breakpoints
set var g_animation_pause=1
monitor resume
monitor sleep 400
monitor halt
set $paused_frame=g_frame_count
monitor resume
monitor sleep 400
monitor halt
printf "PAUSED settled_before=%u after=%u\n", $paused_frame, g_frame_count
if g_frame_count != $paused_frame
  echo FAIL: paused animation kept advancing\n
  quit 1
end
set var g_frame_delay_ms=10
set var g_animation_pause=0
monitor resume
monitor sleep 1500
monitor halt
printf "RUNNING frame=%u attempts=%u ready=%u i2c_errors=%u uart_errors=%u fault=%u clock=%u uptime=%u\n", g_frame_count, g_frame_attempts, g_oled_ready, g_i2c_errors, g_uart_errors, g_fault, g_clock_hz, g_uptime_ms
if g_frame_count <= $paused_frame || g_oled_ready != 1 || g_i2c_errors != 0 || g_uart_errors != 0 || g_fault != 0
  echo FAIL: resumed animation status invalid\n
  quit 1
end
dump binary memory examples/ks2-oled/build/oled-framebuffer.bin g_oled_buffer g_oled_buffer+1024
set var g_frame_delay_ms=50
monitor resume
echo HARDWARE_VALIDATION_PASS\n
detach
quit
