#ifndef UART_BRIDGE_MOCK_H
#define UART_BRIDGE_MOCK_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#define GPIO_Pin_2 4u
#define GPIO_Pin_3 8u
#define GPIO_ModeIN_PU 0
#define GPIO_ModeOut_PP_5mA 1
#define ENABLE 1
#define UART_TX_REMAP_PA3 3
#define UART_RX_REMAP_PA2 2
#define UART_FIFO_SIZE 8u
#define RB_LCR_STOP_BIT 0x04u
#define RB_LCR_PAR_EN 0x08u
#define RB_LCR_WORD_SZ 0x03u
#define RB_FCR_FIFO_EN 0x01u
#define RB_IER_TXD_EN 0x40u
#define RB_IER_LINE_STAT 0x04u
#define RB_IER_RECV_RDY 0x01u
#define RB_MCR_INT_OE 0x08u
#define UART_IRQn 27
#define __INTERRUPT
#define __HIGH_CODE
#define RB_LSR_DATA_RDY 0x01u
#define RB_LSR_OVER_ERR 0x02u
#define RB_LSR_PAR_ERR 0x04u
#define RB_LSR_FRAME_ERR 0x08u
#define RB_LSR_BREAK_ERR 0x10u
#define RB_LSR_TX_ALL_EMP 0x40u

static uint32_t mock_clock = 100000000u, mock_pins;
static unsigned mock_uart_init, mock_remaps, mock_lsr_reads, mock_rbr_reads;
static uint8_t mock_irq = 0x88, mock_tfc, mock_rfc;
static uint8_t mock_lcr, mock_fcr, mock_ier, mock_div, mock_mcr;
static bool mock_uart_irq_enabled, mock_uart_irq_pending;
static unsigned mock_irq_enables, mock_irq_disables, mock_irq_clears, mock_irq_pends;
static unsigned mock_masked_rx, mock_max_masked_rx;
static void (*mock_on_irq_restore)(void);
void UART_IRQHandler(void);
static uint16_t mock_dl;
static bool mock_shifting, mock_refill_rx;
static uint8_t mock_rx[8], mock_errors[8], mock_empty_errors;
static uint8_t mock_output[16384];
static unsigned mock_output_len;
#define R8_UART_TFC mock_tfc
#define R8_UART_RFC mock_rfc
#define R8_UART_LCR mock_lcr
#define R8_UART_FCR mock_fcr
#define R8_UART_IER mock_ier
#define R8_UART_MCR mock_mcr
#define R8_UART_DIV mock_div
#define R16_UART_DL mock_dl
static inline uint32_t GetSysClock(void) { return mock_clock; }
static inline uint32_t __risc_v_disable_irq(void) {
    uint32_t x=mock_irq;mock_irq=0;mock_masked_rx=0;return x;
}
static inline void __risc_v_enable_irq(uint32_t x) {
    if(mock_masked_rx>mock_max_masked_rx)mock_max_masked_rx=mock_masked_rx;
    mock_irq=(uint8_t)x;
    if(mock_irq && mock_on_irq_restore)mock_on_irq_restore();
}
static inline void PFIC_EnableIRQ(int irq) { assert(irq==UART_IRQn);mock_uart_irq_enabled=true;++mock_irq_enables; }
static inline void PFIC_DisableIRQ(int irq) { assert(irq==UART_IRQn);mock_uart_irq_enabled=false;++mock_irq_disables; }
static inline void PFIC_ClearPendingIRQ(int irq) { assert(irq==UART_IRQn);mock_uart_irq_pending=false;++mock_irq_clears; }
static inline void PFIC_SetPendingIRQ(int irq) { assert(irq==UART_IRQn);mock_uart_irq_pending=true;++mock_irq_pends; }
static inline void mock_fire_uart_irq(void) {
    assert(mock_uart_irq_enabled && mock_irq);
    uint8_t prior=mock_irq;mock_irq=0;UART_IRQHandler();mock_irq=prior;
}
static inline void GPIOA_SetBits(uint32_t x) { mock_pins |= x; }
static inline void GPIOA_ModeCfg(uint32_t x, int mode) { (void)mode; mock_pins |= x; }
static inline void UART_Remap(int e, int tx, int rx)
{ assert(e==1 && tx==3 && rx==2); ++mock_remaps; }
static inline void UART_DefInit(void)
{
    ++mock_uart_init;
    mock_dl=109; mock_lcr=3; mock_fcr=0x81;
    mock_ier=RB_IER_TXD_EN; mock_div=1;
}
static inline void UART_SendByte(uint8_t byte)
{
    assert(mock_tfc < UART_FIFO_SIZE);
    assert(mock_output_len < sizeof(mock_output));
    mock_output[mock_output_len++] = byte;
    ++mock_tfc; mock_shifting=true;
}
static inline uint8_t UART_GetLinSTA(void)
{
    ++mock_lsr_reads;
    uint8_t status = mock_shifting || mock_tfc ? 0 : RB_LSR_TX_ALL_EMP;
    if (mock_rfc) {
        status |= RB_LSR_DATA_RDY | mock_errors[0];
        mock_errors[0]=0; /* Hardware errors are clear-on-read. */
    } else { status |= mock_empty_errors; mock_empty_errors=0; }
    return status;
}
static inline uint8_t UART_RecvByte(void)
{
    assert(mock_rfc);
    ++mock_rbr_reads;
    if(!mock_irq)++mock_masked_rx;
    uint8_t byte=mock_rx[0];
    for (unsigned i=1; i<mock_rfc; ++i) {
        mock_rx[i-1]=mock_rx[i]; mock_errors[i-1]=mock_errors[i];
    }
    --mock_rfc;
    if (mock_refill_rx) { mock_rx[mock_rfc]=0x55; mock_errors[mock_rfc++]=0; }
    return byte;
}
static inline void mock_receive(uint8_t byte, uint8_t errors)
{
    assert(mock_rfc < UART_FIFO_SIZE);
    mock_rx[mock_rfc]=byte; mock_errors[mock_rfc++]=errors;
}
static inline void mock_drain_tx(void) { mock_tfc=0; mock_shifting=false; }
#endif
