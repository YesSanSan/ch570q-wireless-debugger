# Full GDB pass: flash program + readback compare, hardware breakpoints,
# single step, RAM parameter writes, pause/resume, and eight 1 KiB framebuffer
# reads. Designed to run while the CDC port streams the target's own log.
set pagination off
set confirm off
set remotetimeout 30
file examples/ks2-oled/build/ks2-oled.elf
target remote localhost:3333

echo \n===== FLASH: erase + program over SWD =====\n
monitor reset halt
load
echo FLASH_LOAD_OK\n

echo \n===== FLASH: read back and compare with the ELF =====\n
compare-sections -r
echo FLASH_COMPARE_OK\n

echo \n===== BOOT: the freshly written image must start =====\n
monitor reset halt
monitor resume
monitor sleep 800
monitor halt
printf "BOOT pc=0x%x ready=%u frames=%u fault=%u\n", $pc, g_oled_ready, g_frame_count, g_fault
if g_oled_ready != 1 || g_fault != 0
  echo FAIL: flashed image did not start\n
  quit 1
end

echo \n===== BREAKPOINTS: two hardware breakpoints =====\n
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

echo \n===== SINGLE STEP =====\n
set $before_pc = $pc
stepi
printf "SINGLE_STEP before=0x%x after=0x%x\n", $before_pc, $pc
if $before_pc == $pc
  echo FAIL: single step did not move PC\n
  quit 1
end

echo \n===== RAM PARAMETERS: write while halted, verify pause =====\n
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

echo \n===== FRAMEBUFFER: eight 1 KiB reads of a frozen buffer =====\n
set $i = 0
while $i < 8
  dump binary memory examples/ks2-oled/build/oled-fb-$i.bin g_oled_buffer g_oled_buffer+1024
  set $i = $i + 1
end
printf "FRAMEBUFFER dumps=%d bytes=%d\n", $i, $i * 1024

echo \n===== RESUME: hand the target back running =====\n
set var g_frame_delay_ms=50
set var g_animation_pause=0
monitor resume
monitor sleep 1500
monitor halt
printf "RUNNING frame=%u attempts=%u ready=%u i2c_errors=%u uart_errors=%u fault=%u clock=%u uptime=%u\n", g_frame_count, g_frame_attempts, g_oled_ready, g_i2c_errors, g_uart_errors, g_fault, g_clock_hz, g_uptime_ms
if g_frame_count <= $paused_frame || g_oled_ready != 1 || g_i2c_errors != 0 || g_uart_errors != 0 || g_fault != 0
  echo FAIL: resumed animation status invalid\n
  quit 1
end
monitor resume
echo HARDWARE_VALIDATION_PASS\n
detach
quit
