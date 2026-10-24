#ifndef BOARD_H
#define BOARD_H
#include <stdint.h>
#include <stdbool.h>
#define REG32(a) (*(volatile uint32_t *)(a))
extern volatile uint32_t g_uptime_ms;
extern volatile uint32_t g_clock_hz;
extern volatile uint32_t g_i2c_errors;
extern volatile uint32_t g_uart_errors;
extern volatile uint32_t g_fault;
void board_init(void);
void delay_ms(uint32_t ms);
void uart_puts(const char *s);
void uart_u32(uint32_t value);
bool i2c_write(uint8_t address, uint8_t control, const uint8_t *data, uint32_t length);
void i2c_recover(void);
#endif
