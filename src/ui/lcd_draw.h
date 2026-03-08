#ifndef LCD_DRAW_H_
#define LCD_DRAW_H_

#include <stdint.h>
#include <stdbool.h>

void LCD_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint32_t color);
void LCD_DrawChar(uint16_t x, uint16_t y, char c, uint32_t color, uint32_t bg_color, bool transparent);
void LCD_DrawString(uint16_t x, uint16_t y, const char *str, uint32_t color, uint32_t bg_color, bool transparent);
void LCD_DrawCharScaled(uint16_t x, uint16_t y, char c, uint32_t color, uint32_t bg_color, bool transparent, uint8_t scale);
void LCD_DrawStringScaled(uint16_t x, uint16_t y, const char *str, uint32_t color, uint32_t bg_color, bool transparent, uint8_t scale);
void LCD_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color);

void LCD_DrawRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color);
void LCD_FillRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color);
void LCD_DrawThickRoundRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t r, uint32_t color, uint8_t thickness);

#endif /* LCD_DRAW_H_ */
