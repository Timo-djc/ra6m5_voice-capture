#include "lcd_draw.h"
#include "../drivers/drv_disp.h"
#include <stdlib.h>
#include <stdint.h>

// Assuming font data exists in font.h
#include "font.h"

/* Fast batched fill from drv_lcd.c */
extern void LCDDrvFillRect(unsigned short x, unsigned short y,
                            unsigned short w, unsigned short h,
                            uint32_t color);

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    LCDDrvFillRect(x, y, w, h, color);
}

void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint32_t color) {
    DisplayDevice *ptDispDev = LCDGetDevice();
    if (!ptDispDev) return;

    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        ptDispDev->SetPixel(ptDispDev, x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    LCD_DrawLine(x, y, x + w - 1, y, color);
    LCD_DrawLine(x, y, x, y + h - 1, color);
    LCD_DrawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
    LCD_DrawLine(x, y + h - 1, x + w - 1, y + h - 1, color);
}

void LCD_DrawChar(uint16_t x, uint16_t y, char c, uint32_t color, uint32_t bg_color, bool transparent) {
    DisplayDevice *ptDispDev = LCDGetDevice();
    if (!ptDispDev || c < ' ' || c > '~') return;
    
    uint8_t index = c - ' ';
    for (uint8_t i = 0; i < FONT_HEIGHT; i++) {
        uint8_t row = font_data[index][i];
        for (uint8_t j = 0; j < FONT_WIDTH; j++) {
            if (row & (0x01 << j)) {
                ptDispDev->SetPixel(ptDispDev, x + j, y + i, color);
            } else if (!transparent) {
                ptDispDev->SetPixel(ptDispDev, x + j, y + i, bg_color);
            }
        }
    }
}

void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint32_t color, uint32_t bg_color, bool transparent) {
    uint16_t cx = x;
    while (*str) {
        if (*str == '\n') {
            y += FONT_HEIGHT;
            cx = x;
        } else {
            LCD_DrawChar(cx, y, *str, color, bg_color, transparent);
            cx += FONT_WIDTH;
        }
        str++;
    }
}

void LCD_DrawCharScaled(uint16_t x, uint16_t y, char c, uint32_t color, uint32_t bg_color, bool transparent, uint8_t scale) {
    DisplayDevice *ptDispDev = LCDGetDevice();
    if (!ptDispDev || c < ' ' || c > '~' || scale == 0U) return;

    uint8_t index = c - ' ';
    for (uint8_t i = 0; i < FONT_HEIGHT; i++) {
        uint8_t row = font_data[index][i];
        for (uint8_t j = 0; j < FONT_WIDTH; j++) {
            uint16_t px = (uint16_t)(x + (uint16_t)j * scale);
            uint16_t py = (uint16_t)(y + (uint16_t)i * scale);
            if (row & (0x01 << j)) {
                LCDDrvFillRect(px, py, scale, scale, color);
            } else if (!transparent) {
                LCDDrvFillRect(px, py, scale, scale, bg_color);
            }
        }
    }
}

void LCD_DrawStringScaled(uint16_t x, uint16_t y, const char *str, uint32_t color, uint32_t bg_color, bool transparent, uint8_t scale) {
    uint16_t cx = x;
    if (scale == 0U) return;
    while (*str) {
        if (*str == '\n') {
            y = (uint16_t)(y + (uint16_t)FONT_HEIGHT * scale);
            cx = x;
        } else {
            LCD_DrawCharScaled(cx, y, *str, color, bg_color, transparent, scale);
            cx = (uint16_t)(cx + (uint16_t)FONT_WIDTH * scale);
        }
        str++;
    }
}

// ------ 圆角矩形相关原生绘图函数 ------
static bool lcd_coord_fits_u16(int32_t v) {
    return (v >= 0) && (v <= (int32_t) UINT16_MAX);
}

static void lcd_set_pixel_safe(DisplayDevice *ptDispDev, int32_t x, int32_t y, uint32_t color) {
    if (!ptDispDev) return;
    if (!lcd_coord_fits_u16(x) || !lcd_coord_fits_u16(y)) return;
    ptDispDev->SetPixel(ptDispDev, (uint16_t) x, (uint16_t) y, color);
}

static void lcd_draw_line_safe(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    if (!lcd_coord_fits_u16(x1) || !lcd_coord_fits_u16(y1) ||
        !lcd_coord_fits_u16(x2) || !lcd_coord_fits_u16(y2)) {
        return;
    }
    LCD_DrawLine((uint16_t) x1, (uint16_t) y1, (uint16_t) x2, (uint16_t) y2, color);
}

static void LCD_DrawCircleHelper(uint16_t x0, uint16_t y0, uint16_t r, uint8_t cornername, uint32_t color) {
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;
    DisplayDevice *ptDispDev = LCDGetDevice();
    if (!ptDispDev) return;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        if (cornername & 0x4) {
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 + x, (int32_t) y0 + y, color);
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 + y, (int32_t) y0 + x, color);
        }
        if (cornername & 0x2) {
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 + x, (int32_t) y0 - y, color);
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 + y, (int32_t) y0 - x, color);
        }
        if (cornername & 0x8) {
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 - y, (int32_t) y0 + x, color);
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 - x, (int32_t) y0 + y, color);
        }
        if (cornername & 0x1) {
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 - y, (int32_t) y0 - x, color);
            lcd_set_pixel_safe(ptDispDev, (int32_t) x0 - x, (int32_t) y0 - y, color);
        }
    }
}

static void LCD_FillCircleHelper(uint16_t x0, uint16_t y0, uint16_t r, uint8_t cornername, int16_t delta, uint32_t color) {
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;

    while (x < y) {
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;

        if (cornername & 0x1) {
            lcd_draw_line_safe((int32_t) x0 + x,
                               (int32_t) y0 - y,
                               (int32_t) x0 + x,
                               (int32_t) y0 - y + (2 * y) + 1 + delta,
                               color);
            lcd_draw_line_safe((int32_t) x0 + y,
                               (int32_t) y0 - x,
                               (int32_t) x0 + y,
                               (int32_t) y0 - x + (2 * x) + 1 + delta,
                               color);
        }
        if (cornername & 0x2) {
            lcd_draw_line_safe((int32_t) x0 - x,
                               (int32_t) y0 - y,
                               (int32_t) x0 - x,
                               (int32_t) y0 - y + (2 * y) + 1 + delta,
                               color);
            lcd_draw_line_safe((int32_t) x0 - y,
                               (int32_t) y0 - x,
                               (int32_t) x0 - y,
                               (int32_t) y0 - x + (2 * x) + 1 + delta,
                               color);
        }
    }
}

void LCD_DrawRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color) {
    // top edge
    LCD_DrawLine(x + r, y, x + w - r - 1, y, color);
    // bottom edge
    LCD_DrawLine(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
    // left edge
    LCD_DrawLine(x, y + r, x, y + h - r - 1, color);
    // right edge
    LCD_DrawLine(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

    // draw four corners
    LCD_DrawCircleHelper(x + r, y + r, r, 1, color);
    LCD_DrawCircleHelper(x + w - r - 1, y + r, r, 2, color);
    LCD_DrawCircleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
    LCD_DrawCircleHelper(x + r, y + h - r - 1, r, 8, color);
}

void LCD_FillRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color) {
    LCD_FillRect(x + r, y, w - 2 * r, h, color);
    // draw corners
    LCD_FillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
    LCD_FillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
}

void LCD_DrawThickRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color, uint8_t thickness) {
    for (uint8_t i = 0; i < thickness; i++) {
        LCD_DrawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, (r > i) ? (r - i) : 1, color);
    }
}
