#ifndef CH570_SWD_H
#define CH570_SWD_H
#include <stdint.h>
#include <stddef.h>
#define SWD_ACK_OK 1u
#define SWD_ACK_WAIT 2u
#define SWD_ACK_FAULT 4u
#define SWD_ERROR 8u
/* Slave only: releases the chip's own debug pins. Never call with USB active. */
void swd_init(void);
void swd_off(void);
/* Accept 100 kHz..1 MHz as an upper bound; failure preserves the prior clock.
 * Uses elapsed core ticks when SysTick is already free-running at core rate
 * with a full 32-bit period. Does not configure/own SysTick. Otherwise uses
 * conservative loop delays. Actual pin frequency requires board measurement. */
int swd_clock(uint32_t hz);
void swd_configure(uint8_t cfg);
uint8_t swd_transfer(uint8_t request, uint32_t *data);
void swd_write_sequence(const uint8_t *data, size_t bits);
void swd_read_sequence(uint8_t *data, size_t bits);
uint8_t swd_pins(uint8_t value, uint8_t select, uint32_t wait_us);
void swd_delay_us(uint32_t us);
void swd_idle(uint8_t cycles);
#endif
