#ifndef PLATFORM_TEST_CH57X_COMMON_H
#define PLATFORM_TEST_CH57X_COMMON_H
#include <stdint.h>
#define __INTERRUPT
#define __HIGH_CODE
struct mock_systick { uint32_t CTLR, CNTL, CMPL, SR; };
struct mock_pfic { uint32_t SCTLR; };
extern struct mock_systick mock_systick;
extern struct mock_pfic mock_pfic;
#define SysTick (&mock_systick)
#define PFIC (&mock_pfic)
#define SysTick_CTLR_STE 1u
#define SysTick_CTLR_STIE 2u
#define SysTick_CTLR_STCLK 4u
#define SysTick_CTLR_STRE 8u
#define SysTick_IRQn 12
#define TMR_IRQn 24
#define HSECap_12p 3
#define CLK_SOURCE_HSE_PLL_100MHz 0
extern uint8_t R8_TMR_CTRL_MOD, R8_TMR_CTRL_DMA, R8_TMR_INTER_EN, R8_TMR_INT_FLAG;
extern uint32_t R32_TMR_CNT_END;
#define RB_TMR_COUNT_EN 0x04u
#define RB_TMR_ALL_CLEAR 0x02u
#define RB_TMR_IE_CYC_END 0x01u
#define RB_TMR_IF_CYC_END 0x01u
#define RB_TMR_IF_DATA_ACT 0x02u
#define RB_TMR_IF_FIFO_HF 0x04u
#define RB_TMR_IF_DMA_END 0x08u
#define RB_TMR_IF_FIFO_OV 0x10u
void HSECFG_Capacitance(int cap);
void SetSysClock(int source);
void PFIC_EnableIRQ(int irq);
void PFIC_DisableIRQ(int irq);
void PFIC_ClearPendingIRQ(int irq);
void __WFI(void);
uint32_t __risc_v_disable_irq(void);
uint32_t __risc_v_enable_irq(uint32_t bits);
#endif
