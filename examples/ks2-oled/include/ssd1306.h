#ifndef SSD1306_H
#define SSD1306_H
#include <stdint.h>
#include <stdbool.h>
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
extern uint8_t g_oled_buffer[1024];
bool oled_init(void);
bool oled_update(void);
void oled_clear(void);
void oled_pixel(int x, int y);
void oled_rect(int x, int y, int w, int h, bool filled);
void oled_number(int x, int y, uint32_t value, unsigned digits);
#endif
