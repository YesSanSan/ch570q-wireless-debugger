#include "uart_log.h"
#if WD_UART_LOG
#include "CH57x_common.h"
#define LOG_CAPACITY 32u
struct log_record { uint32_t a, b; uint8_t event; };
static struct log_record records[LOG_CAPACITY];
static volatile uint8_t head, tail;
static volatile uint32_t lost;
static uint32_t reported_lost;
static char line[64];
static uint8_t line_len, line_pos;
static bool suspended;
static const char *const names[] = {"UNKNOWN", "BOOT", "USB_INIT", "USB_RESET",
    "USB_SETUP", "USB_IN0", "USB_OUT0", "USB_STALL", "USB_ADDRESS", "USB_CONFIG",
    "USB_SUSPEND", "HEARTBEAT", "RF_INIT"};
static void configure_uart(void)
{
    GPIOA_SetBits(GPIO_Pin_3);
    GPIOA_ModeCfg(GPIO_Pin_2, GPIO_ModeIN_PU);
    GPIOA_ModeCfg(GPIO_Pin_3, GPIO_ModeOut_PP_5mA);
    UART_Remap(ENABLE, UART_TX_REMAP_PA3, UART_RX_REMAP_PA2);
    UART_DefInit(); /* SDK: 115200 baud, 8 data bits, no parity, 1 stop bit. */
}
void uart_log_init(void)
{
    head = tail = 0; lost = reported_lost = 0; line_len = line_pos = 0;
    if (!suspended) configure_uart();
}
void uart_log_suspend(bool suspend)
{
    bool was_suspended = suspended;
    suspended = suspend;
    if (was_suspended && !suspended) configure_uart();
}
void uart_log_event(uint8_t event, uint32_t a, uint32_t b)
{
    uint32_t irq = __risc_v_disable_irq();
    uint8_t next = (head + 1u) % LOG_CAPACITY;
    if (next == tail) ++lost;
    else {
        records[head].event = event; records[head].a = a; records[head].b = b;
        __asm__ volatile("" ::: "memory");
        head = next;
    }
    __risc_v_enable_irq(irq);
}
static void text(const char *s) { while (*s && line_len < sizeof(line)) line[line_len++] = *s++; }
static void hex(uint32_t x)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; --i) line[line_len++] = digits[(x >> (i * 4)) & 15u];
}
void uart_log_poll(void)
{
    if (suspended) return;
    if (line_pos == line_len) {
        struct log_record r;
        uint8_t have = 0, dropped = 0;
        uint32_t irq = __risc_v_disable_irq();
        if (tail != head) {
            r = records[tail]; tail = (tail + 1u) % LOG_CAPACITY; have = 1;
        } else if (reported_lost != lost) {
            r.a = lost - reported_lost; r.b = lost; reported_lost = lost;
            have = dropped = 1; r.event = 0;
        }
        __risc_v_enable_irq(irq);
        if (!have) return;
        line_len = line_pos = 0;
        text(dropped ? "DROPPED" : (r.event < sizeof(names)/sizeof(names[0]) ? names[r.event] : names[0]));
        text(" "); hex(r.a); text(" "); hex(r.b); text("\r\n");
    }
    uint8_t count = R8_UART_TFC;
    uint8_t free_slots = count < UART_FIFO_SIZE ? UART_FIFO_SIZE - count : 0;
    while (free_slots-- && line_pos < line_len) UART_SendByte(line[line_pos++]);
}
#endif
