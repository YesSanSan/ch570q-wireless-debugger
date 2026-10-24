/* gcc -std=c99 -Wall -Wextra -Werror -I tests/platform_mock -I include
 *     tests/test_platform.c src/platform.c -o build/native/test_platform.exe */
#include "CH57x_common.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
struct mock_systick mock_systick;
struct mock_pfic mock_pfic;
uint8_t R8_TMR_CTRL_MOD, R8_TMR_CTRL_DMA, R8_TMR_INTER_EN, R8_TMR_INT_FLAG;
uint32_t R32_TMR_CNT_END;
static uint32_t enabled, pending;
static unsigned sleeps, clock_changes, irq_disabled, irq_enable_calls;
static uint32_t core_irq_bits;
static void check_clock(void);
uint32_t __risc_v_disable_irq(void) {
    uint32_t before=core_irq_bits & 0x88u;
    ++irq_disabled;core_irq_bits &= ~0x88u;return before;
}
uint32_t __risc_v_enable_irq(uint32_t bits) {
    uint32_t before=core_irq_bits;
    assert(bits==0x88u);
    /* Enabling at the beginning would expose stale timer/PFIC state to IRQs. */
    check_clock();
    core_irq_bits |= bits;++irq_enable_calls;return before;
}
static int capacitor;
void HSECFG_Capacitance(int cap) { capacitor=cap; }
void SetSysClock(int source) { assert(source==CLK_SOURCE_HSE_PLL_100MHz);++clock_changes; }
void PFIC_EnableIRQ(int irq) { enabled|=1u<<irq; }
void PFIC_DisableIRQ(int irq) { enabled&=~(1u<<irq); }
void PFIC_ClearPendingIRQ(int irq) { pending&=~(1u<<irq); }
void TMR_IRQHandler(void);
void __WFI(void) {
    assert(!(PFIC->SCTLR & (1u<<2)));
    assert((core_irq_bits & 0x88u)==0x88u);
    assert((enabled & (1u<<TMR_IRQn)) && R8_TMR_CTRL_MOD==RB_TMR_COUNT_EN);
    assert(R8_TMR_INTER_EN==RB_TMR_IE_CYC_END);
    /* The SDK WFI intrinsic clears event mode. Model the next periodic wake,
     * even if an earlier wake IRQ ran between the main-loop check and WFI. */
    PFIC->SCTLR &= ~(1u<<3);
    SysTick->CNTL += R32_TMR_CNT_END;
    TMR_IRQHandler();
    ++sleeps;
}
static void check_clock(void) {
    assert(capacitor==HSECap_12p);
    assert(SysTick->CTLR==(SysTick_CTLR_STE|SysTick_CTLR_STCLK|SysTick_CTLR_STRE));
    assert(SysTick->CMPL==UINT32_MAX && SysTick->SR==0);
    assert(!(enabled & (1u<<SysTick_IRQn)) && !(pending & (1u<<SysTick_IRQn)));
    assert((enabled & (1u<<TMR_IRQn)) && !(pending & (1u<<TMR_IRQn)));
    assert(R32_TMR_CNT_END==100000u && R8_TMR_CTRL_MOD==RB_TMR_COUNT_EN);
    assert(!R8_TMR_CTRL_DMA && R8_TMR_INTER_EN==RB_TMR_IE_CYC_END);
}
int main(void) {
    enabled=pending=UINT32_MAX;
    R8_TMR_CTRL_DMA=0xff;R8_TMR_CTRL_MOD=0xff;
    /* Reset starts with CSR 0x800 interrupt bits disabled. */
    core_irq_bits=0;
    platform_clock_init();check_clock();
    assert(core_irq_bits==0x88u && irq_enable_calls==1);
    assert(clock_changes==1 && platform_millis()==0);
    /* Fractional core ticks are retained, never rounded up per poll. */
    SysTick->CNTL=50000;assert(platform_millis()==0);
    SysTick->CNTL=99999;assert(platform_millis()==0);
    SysTick->CNTL=100000;assert(platform_millis()==1);
    SysTick->CNTL=299999;assert(platform_millis()==2);
    SysTick->CNTL=300000;assert(platform_millis()==3);
    /* Timer IRQ only acknowledges its source; it does not touch SysTick or
     * invent elapsed milliseconds when foreground service was delayed. */
    R8_TMR_INT_FLAG=0xff;TMR_IRQHandler();
    assert(R8_TMR_INT_FLAG==RB_TMR_IF_CYC_END);
    assert(SysTick->CNTL==300000 && platform_millis()==3);check_clock();
    TMR_IRQHandler();
    PFIC->SCTLR=0xff;
    platform_idle();
    assert(sleeps==1 && platform_millis()==4 && PFIC->SCTLR==(0xffu & ~12u));
    check_clock();
    core_irq_bits=0;
    platform_clock_init();check_clock();assert(platform_millis()==0);
    assert(core_irq_bits==0x88u && irq_enable_calls==2);
    SysTick->CNTL=UINT32_MAX-50000u;assert(platform_millis()==42949u);
    SysTick->CNTL=49999;assert(platform_millis()==42950u);
    assert(clock_changes==2);
    puts("Platform tests passed: core tick configuration, fractions/wrap, timer IRQ, disabled-IRQ handoff and core idle.");
    return 0;
}