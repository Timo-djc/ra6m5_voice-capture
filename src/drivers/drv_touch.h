#ifndef _DRV_TOUCH_H_
#define _DRV_TOUCH_H_

#include <stdint.h>
#include <stdbool.h>

#define FT6336_ADDR         0x38
#define FT6336_MAX_POINTS   2U

// Standard FT6x36 Registers
#define FT_REG_NUM_TOUCH    0x02
#define FT_REG_P1_XH        0x03
#define FT_REG_P1_XL        0x04
#define FT_REG_P1_YH        0x05
#define FT_REG_P1_YL        0x06
#define FT_REG_P2_XH        0x09
#define FT_REG_P2_XL        0x0A
#define FT_REG_P2_YH        0x0B
#define FT_REG_P2_YL        0x0C

typedef enum {
    TP_ROT_NONE = 0,
    TP_ROT_90,
    TP_ROT_180,
    TP_ROT_270
} tp_rotation_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    bool is_pressed;
} TouchPoint;

typedef enum
{
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_DOWN,
    TOUCH_EVENT_UP,
    TOUCH_EVENT_MOVE
} TouchEventType;

typedef struct
{
    TouchEventType type;
    TouchPoint point;
} TouchEvent;

void FT6336_Init(void);
void FT6336_SetRotation(tp_rotation_t rotation);
bool FT6336_Scan(void);
uint8_t FT6336_GetTouchCount(void);
bool FT6336_GetPoint(uint8_t index, TouchPoint *point);
bool FT6336_Read_Touch(TouchPoint* point);
bool FT6336_GetEvent(TouchEvent * event);
bool FT6336_IsPressed(TouchPoint * point);
void FT6336_OnInterrupt(void);

#endif /* _DRV_TOUCH_H_ */
