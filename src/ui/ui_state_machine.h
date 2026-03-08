#ifndef UI_STATE_MACHINE_H_
#define UI_STATE_MACHINE_H_

#include "ui_config.h"

typedef enum {
    UI_STATE_STANDBY = 0,
    UI_STATE_FACE_LOCKED,
    UI_STATE_VOICE_AUTH,
    UI_STATE_RESULT
} SystemUIState;

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} UIRect;

extern SystemUIState current_state;

void UI_Init(void);
bool UI_PointInRect(uint16_t x, uint16_t y, const UIRect * rect);

const UIRect * UI_GetStandbyStartRect(void);
const UIRect * UI_GetFaceAbortRect(void);
const UIRect * UI_GetFaceProceedRect(void);
const UIRect * UI_GetVoiceRefreshRect(void);
const UIRect * UI_GetVoiceHoldRect(void);
const UIRect * UI_GetResultActionRect(void);
const UIRect * UI_GetResultBypassRect(void);

void UI_DrawStandby(bool pressed);
void UI_DrawFaceLocked(bool abort_pressed, bool proceed_pressed);
void UI_DrawVoiceAuth(const int * random_digits,
                      uint8_t progress,
                      bool refresh_pressed,
                      bool tap_pressed,
                      bool infer_busy,
                      bool recording_active,
                      uint8_t record_progress_pct);
void UI_DrawResult(bool is_passed, const int * recognized_digits, bool action_pressed);

/* Local redraw helpers for click-color feedback (partial refresh). */
void UI_RedrawStandbyStartButton(bool pressed);
void UI_RedrawFaceAbortButton(bool pressed);
void UI_RedrawFaceProceedButton(bool pressed);
void UI_RedrawVoiceRefreshButton(bool pressed);
void UI_RedrawVoiceHoldButton(bool tap_pressed, bool infer_busy, bool recording_active);
void UI_RedrawResultActionButton(bool is_passed, bool action_pressed);

#endif /* UI_STATE_MACHINE_H_ */
