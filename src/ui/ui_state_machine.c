#include "ui_state_machine.h"
#include "lcd_draw.h"

#include <stdio.h>
#include <string.h>

SystemUIState current_state = UI_STATE_STANDBY;

enum
{
    UI_DIGIT_COUNT = 4
};

static const UIRect s_rect_standby_start = {34U, 148U, 196U, 132U};
static const UIRect s_rect_standby_speaker = {250U, 148U, 196U, 132U};
static const UIRect s_rect_face_abort = {324U, 50U, 142U, 50U};
static const UIRect s_rect_face_proceed = {118U, 252U, 244U, 46U};
static const UIRect s_rect_voice_refresh = {398U, 82U, 68U, 36U};
static const UIRect s_rect_voice_hold = {56U, 248U, 368U, 58U};
static const UIRect s_rect_speaker_identify = {44U, 210U, 132U, 72U};
static const UIRect s_rect_speaker_enroll = {188U, 210U, 132U, 72U};
static const UIRect s_rect_speaker_back = {332U, 210U, 104U, 72U};
static const UIRect s_rect_result_action = {120U, 240U, 240U, 56U};
static const UIRect s_rect_result_bypass = {4U, 6U, 34U, 22U};

static void Draw_Grid_Region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    const uint16_t step = 24U;
    uint16_t x_end = (uint16_t) (x + w);
    uint16_t y_end = (uint16_t) (y + h);
    uint16_t gx = (uint16_t) ((x / step) * step);
    uint16_t gy = (uint16_t) ((y / step) * step);

    for (; gx <= x_end; gx = (uint16_t) (gx + step))
    {
        LCD_DrawLine(gx, y, gx, y_end, COLOR_GRID);
    }

    for (; gy <= y_end; gy = (uint16_t) (gy + step))
    {
        LCD_DrawLine(x, gy, x_end, gy, COLOR_GRID);
    }
}

static void Draw_BackgroundHUD(void)
{
    LCD_FillRect(0, 0, UI_SCREEN_W, UI_SCREEN_H, COLOR_BG);
    Draw_Grid_Region(0, 0, (uint16_t) (UI_SCREEN_W - 1U), (uint16_t) (UI_SCREEN_H - 1U));
    LCD_FillRect(0, 0, UI_SCREEN_W, UI_TOP_BAR_H, COLOR_PANEL);
    LCD_DrawLine(0, UI_TOP_BAR_H, (uint16_t) (UI_SCREEN_W - 1U), UI_TOP_BAR_H, COLOR_PRIMARY);
    LCD_DrawLine(0, (uint16_t) (UI_SCREEN_H - UI_BOT_BAR_H), (uint16_t) (UI_SCREEN_W - 1U), (uint16_t) (UI_SCREEN_H - UI_BOT_BAR_H), COLOR_GRID);
}

static void Draw_Header(const char * title, const char * status, uint32_t status_color)
{
    uint16_t status_w;
    uint16_t status_x;

    LCD_DrawStringScaled(10, 8, title, COLOR_PRIMARY, COLOR_PANEL, true, 2U);
    if (NULL == status)
    {
        return;
    }

    status_w = (uint16_t) (strlen(status) * 16U);
    status_x = (status_w + 12U < UI_SCREEN_W) ? (uint16_t) (UI_SCREEN_W - status_w - 12U) : 10U;
    LCD_DrawStringScaled(status_x, 8, status, status_color, COLOR_PANEL, true, 2U);
}

static void Draw_CenteredTextScaled(uint16_t y, const char * text, uint32_t color, uint32_t bg, uint8_t scale)
{
    uint16_t width = (uint16_t) (strlen(text) * 8U * scale);
    uint16_t x = (width < UI_SCREEN_W) ? (uint16_t) ((UI_SCREEN_W - width) / 2U) : 0U;
    LCD_DrawStringScaled(x, y, text, color, bg, true, scale);
}

static void Draw_Button(const UIRect * rect,
                        uint32_t fill_color,
                        uint32_t edge_color,
                        uint32_t text_color,
                        bool pressed,
                        const char * line1,
                        const char * line2)
{
    uint16_t radius = (rect->h >= 16U) ? 14U : (uint16_t) (rect->h / 2U);

    LCD_FillRoundRect(rect->x, rect->y, rect->w, rect->h, radius, fill_color);
    LCD_DrawThickRoundRect(rect->x, rect->y, rect->w, rect->h, radius, edge_color, pressed ? 3U : 2U);

    if (NULL != line1)
    {
        uint16_t line1_w = (uint16_t) (strlen(line1) * 8U * 2U);
        uint16_t line1_x = (line1_w < rect->w) ? (uint16_t) (rect->x + ((rect->w - line1_w) / 2U)) : rect->x;
        uint16_t line1_y = (uint16_t) (rect->y + (pressed ? 16U : 14U));
        LCD_DrawStringScaled(line1_x, line1_y, line1, text_color, fill_color, true, 2U);
    }

    if (NULL != line2)
    {
        uint16_t line2_w = (uint16_t) (strlen(line2) * 8U);
        uint16_t line2_x = (line2_w < rect->w) ? (uint16_t) (rect->x + ((rect->w - line2_w) / 2U)) : rect->x;
        uint16_t line2_y = (uint16_t) (rect->y + (pressed ? 48U : 46U));
        LCD_DrawString(line2_x, line2_y, line2, text_color, fill_color, true);
    }
}

static void Draw_RecordProgressBar(uint8_t pct, bool active)
{
    const uint16_t bar_x = 56U;
    const uint16_t bar_y = 310U;
    const uint16_t bar_w = 368U;
    const uint16_t bar_h = 6U;
    uint16_t fill_w;
    uint32_t fill_color;

    if (pct > 100U)
    {
        pct = 100U;
    }

    fill_w = (uint16_t) (((uint32_t) pct * bar_w) / 100U);
    fill_color = active ? COLOR_PRIMARY : COLOR_TEXT_DIM;

    LCD_DrawRect((uint16_t) (bar_x - 1U), (uint16_t) (bar_y - 1U), (uint16_t) (bar_w + 2U), (uint16_t) (bar_h + 2U), COLOR_GRID);
    LCD_FillRect(bar_x, bar_y, bar_w, bar_h, COLOR_PANEL);
    if (fill_w > 0U)
    {
        LCD_FillRect(bar_x, bar_y, fill_w, bar_h, fill_color);
    }
}

static void VoiceAuth_NormalizeDigits(const int * in_digits, int out_digits[UI_DIGIT_COUNT])
{
    int i;

    if (NULL == in_digits)
    {
        for (i = 0; i < UI_DIGIT_COUNT; i++)
        {
            out_digits[i] = 0;
        }
        return;
    }

    for (i = 0; i < UI_DIGIT_COUNT; i++)
    {
        int d = in_digits[i] % 10;
        if (d < 0)
        {
            d += 10;
        }
        out_digits[i] = d;
    }
}

static void Draw_VoiceDigits(const int digits[UI_DIGIT_COUNT], uint8_t progress)
{
    uint16_t box_w = 88U;
    uint16_t box_h = 102U;
    uint16_t spacing = 18U;
    uint16_t top_y = (uint16_t) (UI_TOP_BAR_H + 70U);
    uint16_t start_x = (uint16_t) ((UI_SCREEN_W - (uint16_t) (UI_DIGIT_COUNT * box_w + (UI_DIGIT_COUNT - 1) * spacing)) / 2U);
    uint8_t i;

    for (i = 0U; i < UI_DIGIT_COUNT; i++)
    {
        uint16_t bx = (uint16_t) (start_x + (uint16_t) i * (box_w + spacing));
        uint32_t fill = COLOR_BG;
        uint32_t edge = COLOR_INACTIVE;
        uint32_t text = COLOR_INACTIVE;
        char d[2];

        if (i < progress)
        {
            edge = COLOR_SUCCESS;
            text = COLOR_SUCCESS;
        }
        else if (i == progress)
        {
            fill = COLOR_ACTIVE_BG;
            edge = COLOR_PRIMARY;
            text = COLOR_WHITE;
        }

        LCD_FillRoundRect(bx, top_y, box_w, box_h, 10U, fill);
        LCD_DrawThickRoundRect(bx, top_y, box_w, box_h, 10U, edge, (i == progress) ? 2U : 1U);
        d[0] = (char) ('0' + digits[i]);
        d[1] = '\0';
        LCD_DrawStringScaled((uint16_t) (bx + 24U), (uint16_t) (top_y + 32U), d, text, fill, true, 5U);
    }
}

static void Draw_StandbyStartButton(bool pressed)
{
    uint32_t fill = pressed ? COLOR_WHITE : COLOR_ACTIVE_BG;
    uint32_t edge = pressed ? COLOR_SUCCESS : COLOR_PRIMARY;
    uint32_t text = pressed ? COLOR_BG : COLOR_WHITE;
    Draw_Button(&s_rect_standby_start, fill, edge, text, pressed, "DEFENSE", "START");
}

static void Draw_StandbySpeakerButton(bool pressed)
{
    uint32_t fill = pressed ? COLOR_WHITE : COLOR_PANEL;
    uint32_t edge = COLOR_PRIMARY;
    uint32_t text = pressed ? COLOR_BG : COLOR_PRIMARY;
    Draw_Button(&s_rect_standby_speaker, fill, edge, text, pressed, "VOICEPRINT", "MODE");
}

static void Draw_FaceAbortButton(bool abort_pressed)
{
    Draw_Button(&s_rect_face_abort,
                abort_pressed ? COLOR_WHITE : COLOR_ALERT,
                COLOR_ALERT,
                abort_pressed ? COLOR_ALERT : COLOR_WHITE,
                abort_pressed,
                "ABORT",
                "RELOCK");
}

static void Draw_FaceProceedButton(bool proceed_pressed)
{
    Draw_Button(&s_rect_face_proceed,
                proceed_pressed ? COLOR_WHITE : COLOR_ACTIVE_BG,
                COLOR_PRIMARY,
                proceed_pressed ? COLOR_BG : COLOR_PRIMARY,
                proceed_pressed,
                "VOICE AUTH",
                "PROCEED");
}

static void Draw_VoiceRefreshButton(bool refresh_pressed)
{
    Draw_Button(&s_rect_voice_refresh,
                refresh_pressed ? COLOR_WHITE : COLOR_PANEL,
                COLOR_PRIMARY,
                refresh_pressed ? COLOR_BG : COLOR_PRIMARY,
                refresh_pressed,
                "REFRESH",
                "REROLL");
}

static void Draw_VoiceHoldButton(bool tap_pressed, bool infer_busy, bool recording_active)
{
    uint32_t hold_fill;
    uint32_t hold_edge;
    uint32_t hold_text;
    bool hold_btn_pressed;
    const char * hold_line1;
    const char * hold_line2;

    hold_btn_pressed = tap_pressed && !recording_active && !infer_busy;
    if (infer_busy)
    {
        hold_fill = COLOR_PANEL;
        hold_edge = COLOR_WAIT;
        hold_text = COLOR_WAIT;
        hold_line1 = "PROCESSING";
        hold_line2 = "MODEL RUNNING";
    }
    else if (recording_active)
    {
        hold_fill = COLOR_ACTIVE_BG;
        hold_edge = COLOR_PRIMARY;
        hold_text = COLOR_PRIMARY;
        hold_line1 = "RECORDING";
        hold_line2 = "READ 4 DIGITS";
    }
    else
    {
        hold_fill = hold_btn_pressed ? COLOR_WHITE : COLOR_PRIMARY;
        hold_edge = COLOR_PRIMARY;
        hold_text = COLOR_BG;
        hold_line1 = "TAP TO RECORD";
        hold_line2 = "AUTO 6S";
    }

    Draw_Button(&s_rect_voice_hold, hold_fill, hold_edge, hold_text, hold_btn_pressed, hold_line1, hold_line2);
}

static void Draw_SpeakerIdentifyButton(bool pressed, bool busy, bool recording_active)
{
    uint32_t fill = pressed ? COLOR_WHITE : COLOR_ACTIVE_BG;
    uint32_t edge = busy ? COLOR_WAIT : COLOR_PRIMARY;
    uint32_t text = pressed ? COLOR_BG : (busy ? COLOR_WAIT : COLOR_PRIMARY);
    const char * line1 = recording_active ? "REC..." : (busy ? "WAIT" : "IDENTIFY");
    const char * line2 = recording_active ? "CAPTURE" : "CHECK NOW";
    Draw_Button(&s_rect_speaker_identify, fill, edge, text, pressed, line1, line2);
}

static void Draw_SpeakerEnrollButton(bool pressed, bool busy, bool recording_active)
{
    uint32_t fill = pressed ? COLOR_WHITE : COLOR_PANEL;
    uint32_t edge = busy ? COLOR_WAIT : COLOR_SUCCESS;
    uint32_t text = pressed ? COLOR_BG : edge;
    const char * line1 = recording_active ? "REC..." : "ENROLL";
    const char * line2 = recording_active ? "CAPTURE" : "3X SAMPLE";
    Draw_Button(&s_rect_speaker_enroll, fill, edge, text, pressed, line1, line2);
}

static void Draw_SpeakerBackButton(bool pressed)
{
    Draw_Button(&s_rect_speaker_back,
                pressed ? COLOR_WHITE : COLOR_PANEL,
                COLOR_TEXT_DIM,
                pressed ? COLOR_BG : COLOR_TEXT_DIM,
                pressed,
                "BACK",
                "STANDBY");
}

static void Draw_ResultActionButton(bool is_passed, bool action_pressed)
{
    const char * btn1 = is_passed ? "ACKNOWLEDGE" : "RETRY";
    const char * btn2 = is_passed ? "BACK TO IDLE" : "TRY AGAIN";

    Draw_Button(&s_rect_result_action,
                action_pressed ? COLOR_WHITE : (is_passed ? COLOR_ACTIVE_BG : COLOR_ALERT),
                is_passed ? COLOR_SUCCESS : COLOR_ALERT,
                action_pressed ? COLOR_BG : COLOR_WHITE,
                action_pressed,
                btn1,
                btn2);
}

void UI_Init(void)
{
    current_state = UI_STATE_STANDBY;
    Draw_BackgroundHUD();
}

bool UI_PointInRect(uint16_t x, uint16_t y, const UIRect * rect)
{
    if (NULL == rect)
    {
        return false;
    }

    return (x >= rect->x) &&
           (x < (uint16_t) (rect->x + rect->w)) &&
           (y >= rect->y) &&
           (y < (uint16_t) (rect->y + rect->h));
}

const UIRect * UI_GetStandbyStartRect(void) { return &s_rect_standby_start; }
const UIRect * UI_GetStandbySpeakerRect(void) { return &s_rect_standby_speaker; }
const UIRect * UI_GetFaceAbortRect(void) { return &s_rect_face_abort; }
const UIRect * UI_GetFaceProceedRect(void) { return &s_rect_face_proceed; }
const UIRect * UI_GetVoiceRefreshRect(void) { return &s_rect_voice_refresh; }
const UIRect * UI_GetVoiceHoldRect(void) { return &s_rect_voice_hold; }
const UIRect * UI_GetSpeakerIdentifyRect(void) { return &s_rect_speaker_identify; }
const UIRect * UI_GetSpeakerEnrollRect(void) { return &s_rect_speaker_enroll; }
const UIRect * UI_GetSpeakerBackRect(void) { return &s_rect_speaker_back; }
const UIRect * UI_GetResultActionRect(void) { return &s_rect_result_action; }
const UIRect * UI_GetResultBypassRect(void) { return &s_rect_result_bypass; }

void UI_DrawStandby(bool start_pressed, bool speaker_pressed)
{
    Draw_BackgroundHUD();
    Draw_Header("DEFENSE STANDBY", "IDLE", COLOR_SUCCESS);
    Draw_StandbyStartButton(start_pressed);
    Draw_StandbySpeakerButton(speaker_pressed);
    Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 34U), "SELECT FLOW", COLOR_TEXT_DIM, COLOR_BG, 2U);
}

void UI_DrawFaceLocked(bool abort_pressed, bool proceed_pressed)
{
    Draw_BackgroundHUD();
    Draw_Header("FACE LOCKED", "TRACK", COLOR_SUCCESS);
    LCD_DrawRoundRect(82U, 72U, 220U, 150U, 12U, COLOR_SUCCESS);
    LCD_DrawLine(192U, 72U, 192U, 222U, COLOR_SUCCESS);
    LCD_DrawLine(82U, 147U, 302U, 147U, COLOR_SUCCESS);
    Draw_CenteredTextScaled(104U, "TARGET LOCKED", COLOR_SUCCESS, COLOR_BG, 2U);
    Draw_CenteredTextScaled(132U, "CONFIDENCE 100%", COLOR_TEXT_DIM, COLOR_BG, 2U);
    Draw_FaceAbortButton(abort_pressed);
    Draw_FaceProceedButton(proceed_pressed);
}

void UI_DrawVoiceAuth(const int * random_digits,
                      uint8_t progress,
                      bool refresh_pressed,
                      bool tap_pressed,
                      bool infer_busy,
                      bool recording_active,
                      uint8_t record_progress_pct)
{
    int digits[UI_DIGIT_COUNT];
    char status[16];

    if (progress > UI_DIGIT_COUNT)
    {
        progress = UI_DIGIT_COUNT;
    }

    VoiceAuth_NormalizeDigits(random_digits, digits);

    if (infer_busy)
    {
        strcpy(status, "INFER");
    }
    else if (recording_active)
    {
        strcpy(status, "REC");
    }
    else
    {
        strcpy(status, "READY");
    }

    Draw_BackgroundHUD();
    Draw_Header("VOICE AUTH", status, infer_busy ? COLOR_WAIT : (recording_active ? COLOR_PRIMARY : COLOR_SUCCESS));
    Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 24U), "READ DIGITS IN ORDER", COLOR_TEXT_DIM, COLOR_BG, 2U);
    Draw_VoiceDigits(digits, progress);
    Draw_VoiceRefreshButton(refresh_pressed);
    Draw_VoiceHoldButton(tap_pressed, infer_busy, recording_active);
    Draw_RecordProgressBar(record_progress_pct, recording_active);
}

void UI_DrawSpeakerMode(const char * speaker_id,
                        const char * status_text,
                        bool identify_pressed,
                        bool enroll_pressed,
                        bool back_pressed,
                        bool busy,
                        bool recording_active,
                        uint8_t record_progress_pct)
{
    char id_line[64];
    const char * show_id = ((NULL != speaker_id) && ('\0' != speaker_id[0])) ? speaker_id : "speaker1";
    const char * show_status = ((NULL != status_text) && ('\0' != status_text[0])) ? status_text : "READY";

    Draw_BackgroundHUD();
    Draw_Header("VOICEPRINT", busy ? "BUSY" : "READY", busy ? COLOR_WAIT : COLOR_SUCCESS);
    snprintf(id_line, sizeof(id_line), "SPEAKER ID: %s", show_id);
    Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 32U), id_line, COLOR_PRIMARY, COLOR_BG, 2U);
    Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 68U), show_status, COLOR_TEXT_DIM, COLOR_BG, 2U);
    Draw_SpeakerIdentifyButton(identify_pressed, busy, recording_active);
    Draw_SpeakerEnrollButton(enroll_pressed, busy, recording_active);
    Draw_SpeakerBackButton(back_pressed);
    Draw_RecordProgressBar(record_progress_pct, recording_active);
}

void UI_DrawResult(bool is_passed, const int * recognized_digits, bool action_pressed)
{
    char buf[72];
    uint32_t main_color = is_passed ? COLOR_SUCCESS : COLOR_ALERT;
    const char * title = is_passed ? "ACCESS GRANTED" : "ACCESS DENIED";

    Draw_BackgroundHUD();
    Draw_Header("AUTH RESULT", is_passed ? "PASS" : "FAIL", main_color);
    Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 86U), title, main_color, COLOR_BG, 3U);

    if (NULL != recognized_digits)
    {
        snprintf(buf, sizeof(buf), "INPUT %d %d %d %d",
                 recognized_digits[0], recognized_digits[1], recognized_digits[2], recognized_digits[3]);
        Draw_CenteredTextScaled((uint16_t) (UI_TOP_BAR_H + 138U), buf, COLOR_TEXT_DIM, COLOR_BG, 2U);
    }

    Draw_ResultActionButton(is_passed, action_pressed);

    if (!is_passed)
    {
        uint16_t bx = s_rect_result_bypass.x;
        uint16_t by = s_rect_result_bypass.y;
        LCD_DrawRect(bx, by, 24U, 12U, COLOR_TEXT_DIM);
        LCD_FillRect((uint16_t) (bx + 24U), (uint16_t) (by + 3U), 3U, 6U, COLOR_TEXT_DIM);
        LCD_DrawString((uint16_t) (bx + 2U), (uint16_t) (by + 14U), "PWR", COLOR_TEXT_DIM, COLOR_BG, true);
    }
}

void UI_RedrawStandbyStartButton(bool pressed) { Draw_StandbyStartButton(pressed); }
void UI_RedrawStandbySpeakerButton(bool pressed) { Draw_StandbySpeakerButton(pressed); }
void UI_RedrawFaceAbortButton(bool pressed) { Draw_FaceAbortButton(pressed); }
void UI_RedrawFaceProceedButton(bool pressed) { Draw_FaceProceedButton(pressed); }
void UI_RedrawVoiceRefreshButton(bool pressed) { Draw_VoiceRefreshButton(pressed); }
void UI_RedrawVoiceHoldButton(bool tap_pressed, bool infer_busy, bool recording_active) { Draw_VoiceHoldButton(tap_pressed, infer_busy, recording_active); }
void UI_RedrawSpeakerIdentifyButton(bool pressed, bool busy, bool recording_active) { Draw_SpeakerIdentifyButton(pressed, busy, recording_active); }
void UI_RedrawSpeakerEnrollButton(bool pressed, bool busy, bool recording_active) { Draw_SpeakerEnrollButton(pressed, busy, recording_active); }
void UI_RedrawSpeakerBackButton(bool pressed) { Draw_SpeakerBackButton(pressed); }
void UI_RedrawResultActionButton(bool is_passed, bool action_pressed) { Draw_ResultActionButton(is_passed, action_pressed); }
