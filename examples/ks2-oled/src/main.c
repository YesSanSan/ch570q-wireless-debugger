#include "board.h"
#include "ssd1306.h"
/* All diagnostic symbols have external linkage and stable source names.
 * phase: 1 board ready, 2 OLED init/retry, 3 rendering, 4 frame complete.
 */
volatile uint32_t g_phase;
volatile uint32_t g_frame_count;
volatile uint32_t g_frame_attempts;
volatile uint32_t g_oled_ready;
volatile uint32_t g_oled_init_attempts;
volatile uint32_t g_last_frame_ms;
volatile uint32_t g_ball_x = 8;
volatile uint32_t g_ball_y = 24;
volatile uint32_t g_heartbeat;
volatile uint32_t g_animation_pause;
volatile uint32_t g_frame_delay_ms = 50;
static int dx = 2, dy = 1;
static void render(void) {
    oled_clear();
    /* Top: frame number / uptime seconds; middle: bouncing square; bottom: sweep. */
    oled_number(2, 1, g_frame_count, 6);
    oled_number(88, 1, g_uptime_ms / 1000u, 5);
    oled_rect(0, 14, 128, 38, false);
    oled_rect((int)g_ball_x, (int)g_ball_y, 7, 7, true);
    oled_rect(0, 56, 128, 8, false);
    oled_rect(2, 58, (int)(g_frame_count % 124u), 4, true);
}
static void move_ball(void) {
    int x = (int)g_ball_x + dx, y = (int)g_ball_y + dy;
    if (x <= 1 || x >= 120) { dx = -dx; x = x <= 1 ? 1 : 120; }
    if (y <= 15 || y >= 44) { dy = -dy; y = y <= 15 ? 15 : 44; }
    g_ball_x = (uint32_t)x; g_ball_y = (uint32_t)y;
}
static void log_state(void) {
    uart_puts("[oled] ms="); uart_u32(g_uptime_ms);
    uart_puts(" frames="); uart_u32(g_frame_count);
    uart_puts(" ready="); uart_u32(g_oled_ready);
    uart_puts(" i2c_errors="); uart_u32(g_i2c_errors);
    uart_puts(" fault="); uart_u32(g_fault);
    uart_puts("\r\n"); ++g_heartbeat;
}
int main(void) {
    board_init(); g_phase = 1;
    uart_puts("\r\nKS2 OLED demo | STM32F103C8 | UART1 115200 8N1 | Hz=");
    uart_u32(g_clock_hz); uart_puts("\r\n");
    delay_ms(100);
    uint32_t last_log = g_uptime_ms, last_retry = g_uptime_ms - 1000u;
    for (;;) {
        if (!g_oled_ready && (uint32_t)(g_uptime_ms - last_retry) >= 1000u) {
            g_phase = 2; ++g_oled_init_attempts; last_retry = g_uptime_ms;
            g_oled_ready = oled_init();
            uart_puts(g_oled_ready ? "[oled] init OK\r\n" : "[oled] init failed; retry in 1s\r\n");
        }
        if (g_oled_ready && !g_animation_pause) {
            g_phase = 3; ++g_frame_attempts; render();
            if (oled_update()) { ++g_frame_count; g_last_frame_ms = g_uptime_ms; move_ball(); g_phase = 4; }
            else { g_oled_ready = 0; last_retry = g_uptime_ms; }
        }
        if ((uint32_t)(g_uptime_ms - last_log) >= 1000u) { last_log = g_uptime_ms; log_state(); }
        uint32_t wait = g_frame_delay_ms;
        if (wait < 10) wait = 10;
        if (wait > 200) wait = 200;
        delay_ms(wait); /* Busy wait keeps runtime SWD memory observations accessible. */
    }
}
