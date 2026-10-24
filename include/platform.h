#ifndef WD_PLATFORM_H
#define WD_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
/* Only positive USB reset/SETUP activity selects host. No VBUS detector exists. */
#define PLATFORM_USB_PROBE_MS 3000u
/* Starts a full-period, free-running core-clock SysTick (shared with SWD)
 * and owns the general TMR peripheral/IRQ for nominal 1 ms idle wakeups. */
void platform_clock_init(void);
/* Foreground polling clock; call at least once per 32-bit core-tick wrap. */
uint32_t platform_millis(void);
/* Core WFI only; interrupts must remain enabled. No deep sleep/clock gating. */
void platform_idle(void);
#endif