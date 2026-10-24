#include "CH57x_common.h"
#include "board.h"
#include "platform.h"
static uint32_t previous_ticks, milliseconds, remainder;
#define TICKS_PER_MS (BOARD_SYS_CLOCK_HZ / 1000u)
/* The general TMR is application-owned in WCH RF_Basic as well; the RF
 * library uses BLEB/BLEL, not TMR. Its only job here is to wake core idle. */
__INTERRUPT __HIGH_CODE void TMR_IRQHandler(void) {
    R8_TMR_INT_FLAG = RB_TMR_IF_CYC_END;
}
void platform_clock_init(void) {
    HSECFG_Capacitance(BOARD_HSE_CAPACITANCE);
    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
    SysTick->CTLR = 0;
    SysTick->CNTL = 0;
    SysTick->CMPL = 0xffffffffu;
    SysTick->SR = 0;
    PFIC_DisableIRQ(SysTick_IRQn);
    PFIC_ClearPendingIRQ(SysTick_IRQn);
    previous_ticks = milliseconds = remainder = 0;
    /* Preserve the full-period core counter also consumed by SWD delays. */
    SysTick->CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK | SysTick_CTLR_STRE;

    PFIC_DisableIRQ(TMR_IRQn);
    R8_TMR_INTER_EN = 0;
    R8_TMR_CTRL_DMA = 0;
    R8_TMR_CTRL_MOD = RB_TMR_ALL_CLEAR;
    R32_TMR_CNT_END = TICKS_PER_MS;
    R8_TMR_INT_FLAG = RB_TMR_IF_CYC_END | RB_TMR_IF_DATA_ACT |
                      RB_TMR_IF_FIFO_HF | RB_TMR_IF_DMA_END | RB_TMR_IF_FIFO_OV;
    PFIC_ClearPendingIRQ(TMR_IRQn);
    R8_TMR_INTER_EN = RB_TMR_IE_CYC_END;
    PFIC_EnableIRQ(TMR_IRQn);
    /* Same timer-mode setup as SDK TMR_TimerInit; no output or DMA enabled. */
    R8_TMR_CTRL_MOD = RB_TMR_COUNT_EN;
    /* Startup has installed vectors before main reaches this point; enable
     * only after clock/timer setup and pending-IRQ cleanup complete. */
    (void)__risc_v_enable_irq(0x88u);
}
uint32_t platform_millis(void) {
    uint32_t now = SysTick->CNTL, delta = now - previous_ticks;
    previous_ticks = now;
    milliseconds += delta / TICKS_PER_MS;
    remainder += delta % TICKS_PER_MS;
    if (remainder >= TICKS_PER_MS) { remainder -= TICKS_PER_MS; ++milliseconds; }
    return milliseconds;
}
void platform_idle(void) {
    /* Core idle only. Periodic TMR bounds a check/sleep race to a nominal
     * 1 ms plus interrupt service latency; RF/USB clocks and RAM stay live. */
    PFIC->SCTLR &= ~(1u << 2);
    __WFI();
}