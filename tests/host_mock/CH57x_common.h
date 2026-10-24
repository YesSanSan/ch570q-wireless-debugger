#ifndef HOST_TEST_PLATFORM_H
#define HOST_TEST_PLATFORM_H
#include <stdint.h>
#include <string.h>
struct fake_systick { uint32_t CTLR, CNTL, CMPL; };
extern struct fake_systick fake_systick;
#define SysTick (&fake_systick)
#define SysTick_CTLR_STE 1u
#define SysTick_CTLR_STCLK 4u
#define SysTick_CTLR_STRE 8u
#define HSECap_12p 3
#define CLK_SOURCE_HSE_PLL_100MHz 0
extern int host_test_hse_cap;
static inline void HSECFG_Capacitance(int x) { host_test_hse_cap = x; }
static inline void SetSysClock(int x) { (void)x; }
#endif
