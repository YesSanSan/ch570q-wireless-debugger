/* STM32F103C8 register-level board support; register layout: ST RM0008. */
#include "board.h"
#define RCC_CR REG32(0x40021000u)
#define RCC_CFGR REG32(0x40021004u)
#define RCC_APB2ENR REG32(0x40021018u)
#define GPIOA_CRH REG32(0x40010804u)
#define GPIOB_CRH REG32(0x40010c04u)
#define GPIOB_IDR REG32(0x40010c08u)
#define GPIOB_BSRR REG32(0x40010c10u)
#define UART_SR REG32(0x40013800u)
#define UART_DR REG32(0x40013804u)
#define SCL (1u << 10)
#define SDA (1u << 11)
volatile uint32_t g_uptime_ms;
volatile uint32_t g_clock_hz = 8000000;
volatile uint32_t g_i2c_errors;
volatile uint32_t g_uart_errors;
volatile uint32_t g_fault;
void SysTick_Handler(void) { ++g_uptime_ms; }
void HardFault_Handler(void) { g_fault = 0xdead0001; for (;;) { __asm volatile("nop"); } }
static bool wait_mask(volatile uint32_t *reg, uint32_t mask, uint32_t expected) {
    for (uint32_t n = 0; n < 1000000u; ++n) if ((*reg & mask) == expected) return true;
    return false;
}
void board_init(void) {
    /* HSI / 2 * 16 = 64 MHz; no dependency on the populated HSE crystal. */
    RCC_CR |= 1u;
    (void)wait_mask(&RCC_CR, 2u, 2u);
    RCC_CFGR &= ~3u;
    (void)wait_mask(&RCC_CFGR, 12u, 0u);
    RCC_CR &= ~(1u << 24);
    (void)wait_mask(&RCC_CR, 1u << 25, 0u);
    REG32(0x40022000u) = 0x12; /* FLASH prefetch, two wait states */
    RCC_CFGR = (14u << 18) | (4u << 8); /* PLL x16; APB1 / 2 */
    RCC_CR |= 1u << 24;
    if (wait_mask(&RCC_CR, 1u << 25, 1u << 25)) {
        RCC_CFGR |= 2u;
        if (wait_mask(&RCC_CFGR, 12u, 8u)) g_clock_hz = 64000000u;
        else { RCC_CFGR &= ~3u; g_fault = 2; }
    } else g_fault = 1;
    REG32(0xe000ed08u) = 0x08000000; /* VTOR */
    REG32(0xe000edfcu) |= 1u << 24; /* DEMCR.TRCENA */
    REG32(0xe0001004u) = 0;
    REG32(0xe0001000u) |= 1u; /* DWT.CYCCNTENA */
    REG32(0xe000e014u) = g_clock_hz / 1000u - 1u;
    REG32(0xe000e018u) = 0;
    REG32(0xe000e010u) = 7; /* SysTick 1 ms, core clock */
    RCC_APB2ENR |= (1u << 2) | (1u << 3) | (1u << 14);
    /* PA9 AF push-pull 2 MHz; PA10 floating input. PA13/14 untouched. */
    GPIOA_CRH = (GPIOA_CRH & ~0xff0u) | 0x4a0u;
    REG32(0x40013808u) = (g_clock_hz + 57600u) / 115200u;
    REG32(0x4001380cu) = (1u << 13) | (1u << 3) | (1u << 2);
    GPIOB_BSRR = SCL | SDA; /* release before enabling open-drain outputs */
    GPIOB_CRH = (GPIOB_CRH & ~0xff00u) | 0x6600u;
    i2c_recover();
}
void delay_ms(uint32_t ms) {
    uint32_t start = g_uptime_ms;
    while ((uint32_t)(g_uptime_ms - start) < ms) __asm volatile("nop");
}
static void uart_char(char c) {
    uint32_t start = g_uptime_ms;
    while (!(UART_SR & (1u << 7))) {
        if ((uint32_t)(g_uptime_ms - start) >= 20) { ++g_uart_errors; return; }
    }
    UART_DR = (uint8_t)c;
}
void uart_puts(const char *s) { while (*s) uart_char(*s++); }
void uart_u32(uint32_t value) {
    char buf[10]; unsigned n = 0;
    do { buf[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) uart_char(buf[--n]);
}
/* Conservative software I2C timing: >= 5 us half-period plus GPIO overhead.
   PB10/11 always open drain: writing 1 releases the line to module pull-ups. */
static void half_period(void) {
    uint32_t start = REG32(0xe0001004u);
    uint32_t cycles = g_clock_hz / 200000u;
    while ((uint32_t)(REG32(0xe0001004u) - start) < cycles) __asm volatile("nop");
}
static void low(uint32_t pin) { GPIOB_BSRR = pin << 16; }
static void release(uint32_t pin) { GPIOB_BSRR = pin; }
static bool clock_high(void) {
    release(SCL);
    uint32_t start = g_uptime_ms;
    while (!(GPIOB_IDR & SCL)) if ((uint32_t)(g_uptime_ms - start) >= 2) return false;
    half_period(); return true;
}
static bool stop(void) {
    low(SCL); low(SDA); half_period();
    bool ok = clock_high(); release(SDA); half_period(); return ok;
}
void i2c_recover(void) {
    release(SDA);
    for (unsigned n = 0; n < 9; ++n) { low(SCL); half_period(); if (!clock_high()) break; }
    (void)stop();
}
static bool byte_out(uint8_t value) {
    for (unsigned n = 0; n < 8; ++n) {
        low(SCL);
        if (value & 0x80u) release(SDA); else low(SDA);
        half_period(); if (!clock_high()) return false;
        value <<= 1;
    }
    low(SCL); release(SDA); half_period();
    if (!clock_high()) return false;
    bool ack = !(GPIOB_IDR & SDA); low(SCL); return ack;
}
bool i2c_write(uint8_t address, uint8_t control, const uint8_t *data, uint32_t length) {
    release(SDA);
    bool ok = clock_high() && ((GPIOB_IDR & SDA) != 0);
    if (ok) { low(SDA); half_period(); low(SCL); ok = byte_out((uint8_t)(address << 1)) && byte_out(control); }
    for (uint32_t n = 0; ok && n < length; ++n) ok = byte_out(data[n]);
    bool stopped = stop();
    if (!ok || !stopped) { ++g_i2c_errors; return false; }
    return true;
}
