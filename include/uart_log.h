#ifndef WD_UART_LOG_H
#define WD_UART_LOG_H

#include <stdint.h>
#include <stdbool.h>

#ifndef WD_UART_LOG
#define WD_UART_LOG 0
#endif

enum uart_log_event_id {
    UART_LOG_BOOT = 1,
    UART_LOG_USB_INIT,
    UART_LOG_USB_RESET,
    UART_LOG_USB_SETUP,
    UART_LOG_USB_IN0,
    UART_LOG_USB_OUT0,
    UART_LOG_USB_STALL,
    UART_LOG_USB_ADDRESS,
    UART_LOG_USB_CONFIG,
    UART_LOG_USB_SUSPEND,
    UART_LOG_HEARTBEAT,
    UART_LOG_RF_INIT
};

#if WD_UART_LOG
/* Call init in the foreground after configuring the system clock.
 * Claims PA3 TX / PA2 RX at 115200 8N1; leaves PA0/PA1 untouched.
 * event() may run in an ISR: bounded enqueue only, never UART or printf.
 * poll() is foreground-only and writes at most the initial FIFO free space.
 * Lines: EVENT_NAME AAAAAAAA BBBBBBBB\r\n (hexadecimal arguments).
 * DROPPED lines report newly lost records and the cumulative lost total.
 */
void uart_log_init(void);
void uart_log_event(uint8_t event, uint32_t a, uint32_t b);
void uart_log_poll(void);
/* Foreground UART ownership handoff. Suspended poll/init never access UART;
 * event() continues bounded enqueue. Resume restores PA3/PA2 115200 8N1.
 * Owner must drain hardware TX before suspending/resuming configuration. */
void uart_log_suspend(bool suspended);
#else
static inline void uart_log_init(void) {}
static inline void uart_log_event(uint8_t event, uint32_t a, uint32_t b)
{
    (void)event; (void)a; (void)b;
}
static inline void uart_log_poll(void) {}
static inline void uart_log_suspend(bool suspended) { (void)suspended; }
#endif

#endif
