#ifndef RADIO_MOCK_COMMON_H
#define RADIO_MOCK_COMMON_H
#include <stdint.h>
#define __HIGH_CODE
#define __INTERRUPT
#define BLEB_IRQn 20
#define BLEL_IRQn 21
uint32_t __risc_v_disable_irq(void);
void __risc_v_enable_irq(uint32_t saved);
void PFIC_ClearPendingIRQ(int irq);
void PFIC_EnableIRQ(int irq);
uint32_t GetMACAddress(uint8_t *id);
#endif
