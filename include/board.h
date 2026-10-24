#ifndef WD_BOARD_H
#define WD_BOARD_H
/* Board confirmed by user: 32 MHz external crystal, CL=12 pF,
 * no external load capacitors. Use the SDK's internal 12 pF setting.
 * Final oscillator frequency/stability still needs physical measurement.
 */
#define BOARD_HSE_CAPACITANCE HSECap_12p
#define BOARD_SYS_CLOCK_HZ 100000000u
#endif
