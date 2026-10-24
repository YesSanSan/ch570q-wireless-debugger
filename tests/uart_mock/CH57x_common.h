#ifndef UART_LOG_MOCK_H
#define UART_LOG_MOCK_H
#include <stdint.h>
#include <assert.h>
#define GPIO_Pin_2 4u
#define GPIO_Pin_3 8u
#define GPIO_ModeIN_PU 0
#define GPIO_ModeOut_PP_5mA 1
#define ENABLE 1
#define UART_TX_REMAP_PA3 3
#define UART_RX_REMAP_PA2 2
#define UART_FIFO_SIZE 8u
static uint8_t mock_fifo, mock_irq = 0x88;
static uint32_t mock_pins;
static int mock_uart_init;
static char output[8192];
static unsigned output_len;
#define R8_UART_TFC mock_fifo
static inline uint32_t __risc_v_disable_irq(void) { uint32_t x=mock_irq;mock_irq=0;return x; }
static inline void __risc_v_enable_irq(uint32_t x) { mock_irq=(uint8_t)x; }
static inline void GPIOA_SetBits(uint32_t x) { mock_pins|=x; }
static inline void GPIOA_ModeCfg(uint32_t x,int mode) { (void)mode;mock_pins|=x; }
static inline void UART_Remap(int e,int tx,int rx) { assert(e==1 && tx==3 && rx==2); }
static inline void UART_DefInit(void) { ++mock_uart_init; }
static inline void mock_uart_send(uint8_t b) { assert(mock_fifo<8 && output_len<sizeof(output));++mock_fifo;output[output_len++]=(char)b; }
#define UART_SendByte(b) mock_uart_send(b)
#endif
