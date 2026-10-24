/* SPDX-License-Identifier: GPL-3.0-or-later
 * SSD1306 init/page addressing adapted from KS2 lib/oled_stm32_lib/ssd1306.c.
 * Copyright (C) Guilherme Amorim and Renan Guedes, 2020.
 * Simplified transport and pixel/number renderer for the independent demo.
 */
#include "ssd1306.h"
#include "board.h"
uint8_t g_oled_buffer[1024];
static bool commands(const uint8_t *data, uint32_t length) { return i2c_write(0x3c, 0, data, length); }
bool oled_init(void) {
    /* Same 128x64 panel wiring/orientation/charge pump as KS2.
       Use the documented page-mode value 0x02 instead of legacy 0x10. */
    static const uint8_t init[] = {
        0xae, 0x20, 0x02, 0xb0, 0xc8, 0x00, 0x10, 0x40,
        0x81, 0x7f, 0xa1, 0xa6, 0xa8, 0x3f, 0xa4, 0xd3,
        0x00, 0xd5, 0x80, 0xd9, 0x22, 0xda, 0x12, 0xdb,
        0x20, 0x8d, 0x14, 0x2e, 0xaf
    };
    i2c_recover();
    return commands(init, sizeof init);
}
bool oled_update(void) {
    for (uint8_t page = 0; page < 8; ++page) {
        uint8_t cmd[] = {(uint8_t)(0xb0 + page), 0, 0x10};
        if (!commands(cmd, sizeof cmd) || !i2c_write(0x3c, 0x40, &g_oled_buffer[128u * page], 128)) return false;
    }
    return true;
}
void oled_clear(void) { for (unsigned n = 0; n < sizeof g_oled_buffer; ++n) g_oled_buffer[n] = 0; }
void oled_pixel(int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 64) g_oled_buffer[x + (y / 8) * 128] |= (uint8_t)(1u << (y & 7));
}
void oled_rect(int x, int y, int w, int h, bool filled) {
    for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i)
        if (filled || i == 0 || j == 0 || i == w - 1 || j == h - 1) oled_pixel(x + i, y + j);
}
void oled_number(int x, int y, uint32_t value, unsigned digits) {
    static const uint8_t font[10][3] = {
        {31,17,31}, {18,31,16}, {29,21,23}, {21,21,31}, {7,4,31},
        {23,21,29}, {31,21,29}, {1,1,31}, {31,21,31}, {23,21,31}
    };
    while (digits) {
        --digits; unsigned digit = value % 10u; value /= 10u;
        for (int c = 0; c < 3; ++c) for (int r = 0; r < 5; ++r)
            if (font[digit][c] & (1u << r)) oled_rect(x + (int)digits * 8 + c * 2, y + r * 2, 2, 2, true);
    }
}
