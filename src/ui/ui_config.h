#ifndef UI_CONFIG_H_
#define UI_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

/* RGB888 helper: 0x00RRGGBB */
#define RGB888(r, g, b)   ((((uint32_t) (r)) << 16) | (((uint32_t) (g)) << 8) | ((uint32_t) (b)))

/* Theme: cool cyber blue */
#define COLOR_BG          RGB888(0x05, 0x0B, 0x14)
#define COLOR_GRID        RGB888(0x12, 0x1D, 0x2C)
#define COLOR_PRIMARY     RGB888(0x37, 0xE5, 0xFF)
#define COLOR_SUCCESS     RGB888(0x2A, 0xF5, 0x8A)
#define COLOR_ALERT       RGB888(0xFF, 0x4D, 0x5A)
#define COLOR_WAIT        RGB888(0xFF, 0xC8, 0x3A)
#define COLOR_INACTIVE    RGB888(0x5E, 0x71, 0x8A)
#define COLOR_ACTIVE_BG   RGB888(0x0D, 0x2B, 0x3E)
#define COLOR_PANEL       RGB888(0x0A, 0x14, 0x24)
#define COLOR_TEXT_DIM    RGB888(0x8E, 0xA2, 0xBF)
#define COLOR_WHITE       RGB888(0xFF, 0xFF, 0xFF)

/* Layout constants */
#define UI_SCREEN_W       480
#define UI_SCREEN_H       320
#define UI_ORIENTATION_LANDSCAPE_CCW 1
#define UI_TOP_BAR_H      40
#define UI_BOT_BAR_H      60

#endif /* UI_CONFIG_H_ */
