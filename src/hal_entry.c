#include "hal_data.h"
#include "audio_capture.h"
#include "net/cloud_asr_cfg.h"
#include "net/cloud_asr_client.h"
#include "net/w800_at.h"
#include "mvp/app_main.h"
#include "ui/ui_state_machine.h"
#include "drivers/drv_disp.h"
#include "drivers/drv_touch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);
FSP_CPP_FOOTER

volatile bool g_ui_switch_requested = false;

#define UI_RECORD_MS         (1500U)
#define UI_TOTAL_RECORD_MS   (UI_RECORD_MS * 4U)
#define UI_PROGRESS_BUCKET_STEP_PCT (5U)
#define UI_BYPASS_HOLD_MS    (3000U)

typedef enum
{
    UI_CTRL_NONE = 0,
    UI_CTRL_STANDBY_START,
    UI_CTRL_FACE_ABORT,
    UI_CTRL_FACE_PROCEED,
    UI_CTRL_VOICE_REFRESH,
    UI_CTRL_VOICE_HOLD,
    UI_CTRL_RESULT_ACTION,
    UI_CTRL_RESULT_BYPASS
} UIControl;

static int s_challenge_digits[4] = {0, 0, 0, 0};
static int s_recognized_digits[4] = {0, 0, 0, 0};
static uint8_t s_auth_progress = 0U;
static bool s_result_passed = false;
static bool s_infer_busy = false;

static bool s_standby_pressed = false;
static bool s_face_abort_pressed = false;
static bool s_face_proceed_pressed = false;
static bool s_voice_refresh_pressed = false;
static bool s_voice_hold_pressed = false;
static bool s_result_action_pressed = false;

static bool s_bypass_holding = false;
static uint32_t s_bypass_press_ms = 0U;
static bool s_voice_session_active = false;
static bool s_voice_recording_active = false;
static uint32_t s_voice_record_start_ms = 0U;
static uint8_t s_voice_record_progress_pct = 0U;
static uint8_t s_voice_progress_bucket = 0U;
static uint8_t s_voice_record_slot_index = 0U;

static UIControl s_active_control = UI_CTRL_NONE;

static uint32_t s_core_hz = 0U;
/* Temporary trim: keep only W800 flow (MCU UART6/7 + W800 UART0/1). */
#define W800_ONLY_DEBUG_MODE    (1U)

#if defined(__clang__) || defined(__GNUC__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

static bool MAYBE_UNUSED s_w800_bridge_prev_active = false;

static void send_control_line(const char * msg)
{
    extern void uart_write_line_ext(const char * str);
    uart_write_line_ext(msg);
}

static uint32_t ui_now_ms(void)
{
    if (0U == s_core_hz)
    {
        s_core_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);
        if (0U == s_core_hz)
        {
            s_core_hz = 1U;
        }
    }

    return (uint32_t)(((uint64_t) DWT->CYCCNT * 1000ULL) / s_core_hz);
}

static void reset_voice_record_session_state(void)
{
    s_voice_session_active = false;
    s_voice_recording_active = false;
    s_voice_record_start_ms = 0U;
    s_voice_record_progress_pct = 0U;
    s_voice_progress_bucket = 0U;
    s_voice_record_slot_index = 0U;
}

static void start_voice_record_slot(uint8_t slot_index)
{
    uint32_t base_ms;

    if (slot_index >= 4U)
    {
        slot_index = 3U;
    }

    s_voice_session_active = true;
    s_voice_recording_active = true;
    s_voice_record_slot_index = slot_index;
    s_voice_record_start_ms = ui_now_ms();

    base_ms = (uint32_t) slot_index * UI_RECORD_MS;
    s_voice_record_progress_pct = (uint8_t) ((base_ms * 100U) / UI_TOTAL_RECORD_MS);
    s_voice_progress_bucket = (uint8_t) (s_voice_record_progress_pct / UI_PROGRESS_BUCKET_STEP_PCT);

    audio_capture_ptt_press();
}

static void reset_recognized_digits(void)
{
    uint8_t i;
    for (i = 0U; i < 4U; i++)
    {
        s_recognized_digits[i] = 0;
    }
}

static void generate_challenge_digits(void)
{
    uint8_t i;
    for (i = 0U; i < 4U; i++)
    {
        s_challenge_digits[i] = rand() % 10;
    }
}

static void set_control_pressed(UIControl control, bool pressed)
{
    switch (control)
    {
        case UI_CTRL_STANDBY_START:
            s_standby_pressed = pressed;
            break;
        case UI_CTRL_FACE_ABORT:
            s_face_abort_pressed = pressed;
            break;
        case UI_CTRL_FACE_PROCEED:
            s_face_proceed_pressed = pressed;
            break;
        case UI_CTRL_VOICE_REFRESH:
            s_voice_refresh_pressed = pressed;
            break;
        case UI_CTRL_VOICE_HOLD:
            s_voice_hold_pressed = pressed;
            break;
        case UI_CTRL_RESULT_ACTION:
            s_result_action_pressed = pressed;
            break;
        default:
            break;
    }
}

static void clear_all_pressed(void)
{
    s_standby_pressed = false;
    s_face_abort_pressed = false;
    s_face_proceed_pressed = false;
    s_voice_refresh_pressed = false;
    s_voice_hold_pressed = false;
    s_result_action_pressed = false;
}

static UIControl hit_test_control(uint16_t x, uint16_t y)
{
    switch (current_state)
    {
        case UI_STATE_STANDBY:
            return UI_PointInRect(x, y, UI_GetStandbyStartRect()) ? UI_CTRL_STANDBY_START : UI_CTRL_NONE;

        case UI_STATE_FACE_LOCKED:
            if (UI_PointInRect(x, y, UI_GetFaceAbortRect()))
            {
                return UI_CTRL_FACE_ABORT;
            }
            if (UI_PointInRect(x, y, UI_GetFaceProceedRect()))
            {
                return UI_CTRL_FACE_PROCEED;
            }
            return UI_CTRL_NONE;

        case UI_STATE_VOICE_AUTH:
            if (UI_PointInRect(x, y, UI_GetVoiceRefreshRect()))
            {
                if (s_voice_recording_active || s_voice_session_active || s_infer_busy)
                {
                    return UI_CTRL_NONE;
                }
                return UI_CTRL_VOICE_REFRESH;
            }
            if (UI_PointInRect(x, y, UI_GetVoiceHoldRect()))
            {
                return UI_CTRL_VOICE_HOLD;
            }
            return UI_CTRL_NONE;

        case UI_STATE_RESULT:
            if (!s_result_passed && UI_PointInRect(x, y, UI_GetResultBypassRect()))
            {
                return UI_CTRL_RESULT_BYPASS;
            }
            if (UI_PointInRect(x, y, UI_GetResultActionRect()))
            {
                return UI_CTRL_RESULT_ACTION;
            }
            return UI_CTRL_NONE;

        default:
            return UI_CTRL_NONE;
    }
}

static void render_ui(DisplayDevice * disp)
{
    switch (current_state)
    {
        case UI_STATE_STANDBY:
            UI_DrawStandby(s_standby_pressed);
            break;

        case UI_STATE_FACE_LOCKED:
            UI_DrawFaceLocked(s_face_abort_pressed, s_face_proceed_pressed);
            break;

        case UI_STATE_VOICE_AUTH:
            UI_DrawVoiceAuth(s_challenge_digits,
                             s_auth_progress,
                             s_voice_refresh_pressed,
                             s_voice_hold_pressed,
                             s_infer_busy,
                             s_voice_recording_active,
                             s_voice_record_progress_pct);
            break;

        case UI_STATE_RESULT:
            UI_DrawResult(s_result_passed, s_recognized_digits, s_result_action_pressed);
            break;

        default:
            break;
    }

    if (disp != NULL)
    {
        disp->Flush(disp);
    }
}

static void render_control_feedback(DisplayDevice * disp, UIControl control)
{
    switch (current_state)
    {
        case UI_STATE_STANDBY:
            if (UI_CTRL_STANDBY_START == control)
            {
                UI_RedrawStandbyStartButton(s_standby_pressed);
            }
            break;

        case UI_STATE_FACE_LOCKED:
            if (UI_CTRL_FACE_ABORT == control)
            {
                UI_RedrawFaceAbortButton(s_face_abort_pressed);
            }
            else if (UI_CTRL_FACE_PROCEED == control)
            {
                UI_RedrawFaceProceedButton(s_face_proceed_pressed);
            }
            break;

        case UI_STATE_VOICE_AUTH:
            if (UI_CTRL_VOICE_REFRESH == control)
            {
                UI_RedrawVoiceRefreshButton(s_voice_refresh_pressed);
            }
            else if (UI_CTRL_VOICE_HOLD == control)
            {
                UI_RedrawVoiceHoldButton(s_voice_hold_pressed, s_infer_busy, s_voice_recording_active);
            }
            break;

        case UI_STATE_RESULT:
            if (UI_CTRL_RESULT_ACTION == control)
            {
                UI_RedrawResultActionButton(s_result_passed, s_result_action_pressed);
            }
            break;

        default:
            break;
    }

    if (disp != NULL)
    {
        disp->Flush(disp);
    }
}

static void enter_voice_auth(DisplayDevice * disp)
{
    current_state = UI_STATE_VOICE_AUTH;
    s_auth_progress = 0U;
    s_infer_busy = false;
    s_bypass_holding = false;
    reset_voice_record_session_state();
    reset_recognized_digits();
    generate_challenge_digits();
    clear_all_pressed();
    render_ui(disp);
}

static void enter_result(DisplayDevice * disp, bool is_passed)
{
    current_state = UI_STATE_RESULT;
    s_result_passed = is_passed;
    s_infer_busy = false;
    s_bypass_holding = false;
    reset_voice_record_session_state();
    clear_all_pressed();
    render_ui(disp);
}

static void reset_to_standby(DisplayDevice * disp)
{
    current_state = UI_STATE_STANDBY;
    s_auth_progress = 0U;
    s_result_passed = false;
    s_infer_busy = false;
    s_bypass_holding = false;
    reset_voice_record_session_state();
    s_active_control = UI_CTRL_NONE;
    clear_all_pressed();
    render_ui(disp);
}

static void on_control_release(UIControl control, DisplayDevice * disp)
{
    switch (control)
    {
        case UI_CTRL_STANDBY_START:
            send_control_line("K210_CMD:START_TRACK\r\n");
            current_state = UI_STATE_FACE_LOCKED;
            clear_all_pressed();
            render_ui(disp);
            break;

        case UI_CTRL_FACE_ABORT:
            send_control_line("K210_CMD:ABORT_RELOCK\r\n");
            clear_all_pressed();
            render_control_feedback(disp, UI_CTRL_FACE_ABORT);
            break;

        case UI_CTRL_FACE_PROCEED:
            enter_voice_auth(disp);
            break;

        case UI_CTRL_VOICE_REFRESH:
            s_auth_progress = 0U;
            s_infer_busy = false;
            reset_voice_record_session_state();
            reset_recognized_digits();
            generate_challenge_digits();
            clear_all_pressed();
            render_ui(disp);
            break;

        case UI_CTRL_RESULT_ACTION:
            if (s_result_passed)
            {
                reset_to_standby(disp);
            }
            else
            {
                current_state = UI_STATE_VOICE_AUTH;
                s_auth_progress = 0U;
                s_infer_busy = false;
                reset_voice_record_session_state();
                reset_recognized_digits();
                clear_all_pressed();
                render_ui(disp);
            }
            break;

        default:
            break;
    }
}

static void MAYBE_UNUSED process_voice_digit_result(DisplayDevice * disp)
{
    int digit = -1;
    float conf = 0.0f;

    if (!audio_capture_consume_digit_result(&digit, &conf))
    {
        return;
    }

    (void) conf;
    s_infer_busy = false;
    s_voice_recording_active = false;

    if (current_state != UI_STATE_VOICE_AUTH)
    {
        return;
    }

    if (s_auth_progress >= 4U)
    {
        return;
    }

    s_recognized_digits[s_auth_progress] = digit;

    if ((digit >= 0) && (digit == s_challenge_digits[s_auth_progress]))
    {
        s_auth_progress++;
        if (s_auth_progress >= 4U)
        {
            s_voice_record_progress_pct = 100U;
            s_voice_progress_bucket = (uint8_t) (100U / UI_PROGRESS_BUCKET_STEP_PCT);
            s_voice_session_active = false;
            send_control_line("GIMBAL_CMD:UNLOCK\r\n");
            enter_result(disp, true);
            return;
        }

        if (s_voice_session_active)
        {
            start_voice_record_slot(s_auth_progress);
            render_ui(disp);
            return;
        }

        render_ui(disp);
        return;
    }

    s_voice_session_active = false;
    enter_result(disp, false);
}

static void MAYBE_UNUSED handle_touch_event(const TouchEvent * event, DisplayDevice * disp)
{
    UIControl hit;

    if (NULL == event)
    {
        return;
    }

    hit = hit_test_control(event->point.x, event->point.y);

    if (TOUCH_EVENT_DOWN == event->type)
    {
        if (UI_CTRL_NONE == hit)
        {
            s_active_control = UI_CTRL_NONE;
            return;
        }

        if ((UI_CTRL_VOICE_HOLD == hit) &&
            (s_voice_recording_active || s_voice_session_active || s_infer_busy))
        {
            s_active_control = UI_CTRL_NONE;
            return;
        }

        s_active_control = hit;
        set_control_pressed(hit, true);

        if (UI_CTRL_RESULT_BYPASS == hit)
        {
            s_bypass_holding = true;
            s_bypass_press_ms = ui_now_ms();
        }

        if (UI_CTRL_RESULT_BYPASS != hit)
        {
            render_control_feedback(disp, hit);
        }
        return;
    }

    if (TOUCH_EVENT_MOVE == event->type)
    {
        if ((s_active_control != UI_CTRL_NONE) && (s_active_control != UI_CTRL_VOICE_HOLD) && (s_active_control != UI_CTRL_RESULT_BYPASS))
        {
            bool inside = (hit == s_active_control);
            bool was_pressed = false;

            switch (s_active_control)
            {
                case UI_CTRL_STANDBY_START: was_pressed = s_standby_pressed; break;
                case UI_CTRL_FACE_ABORT: was_pressed = s_face_abort_pressed; break;
                case UI_CTRL_FACE_PROCEED: was_pressed = s_face_proceed_pressed; break;
                case UI_CTRL_VOICE_REFRESH: was_pressed = s_voice_refresh_pressed; break;
                case UI_CTRL_RESULT_ACTION: was_pressed = s_result_action_pressed; break;
                default: break;
            }

            if (inside != was_pressed)
            {
                set_control_pressed(s_active_control, inside);
                render_control_feedback(disp, s_active_control);
            }
        }

        if (s_bypass_holding && !UI_PointInRect(event->point.x, event->point.y, UI_GetResultBypassRect()))
        {
            s_bypass_holding = false;
        }

        return;
    }

    if (TOUCH_EVENT_UP == event->type)
    {
        if (UI_CTRL_NONE == s_active_control)
        {
            return;
        }

        if (UI_CTRL_VOICE_HOLD == s_active_control)
        {
            s_voice_hold_pressed = false;

            if ((!s_voice_recording_active) && (!s_voice_session_active) && (!s_infer_busy) && (s_auth_progress < 4U))
            {
                start_voice_record_slot(s_auth_progress);
            }

            render_ui(disp);
            s_active_control = UI_CTRL_NONE;
            return;
        }

        /* For tap controls, fire the action whenever DOWN was registered on
         * the control (s_active_control != NONE), regardless of where the
         * finger finally lifted.  A tiny drift during the tap must NOT swallow
         * the action (standard mobile "tap" behaviour).
         * HOLD and BYPASS are position-sensitive and handled separately.     */
        bool trigger_action = (s_active_control != UI_CTRL_NONE) &&
                              (s_active_control != UI_CTRL_VOICE_HOLD) &&
                              (s_active_control != UI_CTRL_RESULT_BYPASS);

        if (UI_CTRL_RESULT_BYPASS == s_active_control)
        {
            s_bypass_holding = false;
        }

        if (!trigger_action &&
            (s_active_control != UI_CTRL_VOICE_HOLD) &&
            (s_active_control != UI_CTRL_RESULT_BYPASS))
        {
            /* No page-switch will follow: clear pressed state and refresh
             * the current page to show the button in its normal colour.  */
            set_control_pressed(s_active_control, false);
            render_control_feedback(disp, s_active_control);
        }

        if (trigger_action)
        {
            on_control_release(s_active_control, disp);
        }

        s_active_control = UI_CTRL_NONE;
    }
}

void hal_entry(void)
{
#if W800_ONLY_DEBUG_MODE
    app_main_init();

    while (1)
    {
        app_main_poll();
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }
#else
    DisplayDevice * ptDispDev;
    TouchEvent touch_event;

    audio_capture_init();
    audio_capture_start();
    cloud_asr_client_init();

    send_control_line("BOOT: audio ok, init LCD...\r\n");

    ptDispDev = LCDGetDevice();
    if (ptDispDev)
    {
        ptDispDev->Init(ptDispDev);
        send_control_line("BOOT: LCD init done\r\n");
        ptDispDev->SetDisplayWindow(ptDispDev, 0, 0, UI_SCREEN_W - 1, UI_SCREEN_H - 1);
    }

    send_control_line("BOOT: UI init\r\n");
    UI_Init();
    if (0U == s_core_hz)
    {
        s_core_hz = R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_ICLK);
    }
    srand((unsigned int) DWT->CYCCNT);

    FT6336_Init();

    reset_to_standby(ptDispDev);

    send_control_line("BOOT: touch interrupt + UI state machine online\r\n");

    while (1)
    {
        TouchPoint live_point;
        uint32_t now_ms;
        bool bridge_active = w800_bridge_is_active();

        if (!bridge_active)
        {
            audio_capture_process();
        }

        w800_bridge_poll();

        bridge_active = w800_bridge_is_active();
        if (bridge_active != s_w800_bridge_prev_active)
        {
            if (bridge_active)
            {
                cloud_asr_client_suspend();
            }
            else
            {
                cloud_asr_client_restart();
            }

            s_w800_bridge_prev_active = bridge_active;
        }

        if (!bridge_active)
        {
            cloud_asr_client_poll();
        }
        else
        {
            continue;
        }

        while (FT6336_GetEvent(&touch_event))
        {
            handle_touch_event(&touch_event, ptDispDev);
        }

        now_ms = ui_now_ms();

        if ((current_state == UI_STATE_VOICE_AUTH) && s_voice_recording_active)
        {
            uint32_t elapsed_ms = now_ms - s_voice_record_start_ms;
            uint8_t progress_pct;
            uint8_t progress_bucket;
            uint32_t total_ms;

            if (elapsed_ms >= UI_RECORD_MS)
            {
                audio_capture_ptt_release();
                s_voice_recording_active = false;
                s_infer_busy = true;

                total_ms = (uint32_t) (s_voice_record_slot_index + 1U) * UI_RECORD_MS;
            }
            else
            {
                total_ms = ((uint32_t) s_voice_record_slot_index * UI_RECORD_MS) + elapsed_ms;
            }

            progress_pct = (uint8_t) ((total_ms * 100U) / UI_TOTAL_RECORD_MS);
            if (progress_pct > 100U)
            {
                progress_pct = 100U;
            }
            progress_bucket = (uint8_t) (progress_pct / UI_PROGRESS_BUCKET_STEP_PCT);

            if ((progress_bucket != s_voice_progress_bucket) || (elapsed_ms >= UI_RECORD_MS))
            {
                s_voice_record_progress_pct = progress_pct;
                s_voice_progress_bucket = progress_bucket;
                render_ui(ptDispDev);
            }
            else
            {
                s_voice_record_progress_pct = progress_pct;
            }
        }

        if ((current_state == UI_STATE_RESULT) && (!s_result_passed) && s_bypass_holding)
        {
            bool still_pressed = FT6336_IsPressed(&live_point);
            bool still_inside = still_pressed && UI_PointInRect(live_point.x, live_point.y, UI_GetResultBypassRect());

            if (!still_inside)
            {
                s_bypass_holding = false;
            }
            else if ((now_ms - s_bypass_press_ms) >= UI_BYPASS_HOLD_MS)
            {
                s_bypass_holding = false;
                send_control_line("GIMBAL_CMD:FORCE_UNLOCK\r\n");
                enter_result(ptDispDev, true);
            }
        }



        audio_capture_run_inference_if_ready();
        process_voice_digit_result(ptDispDev);

#if ASR_MODE_CLOUD
        R_BSP_SoftwareDelay(2U, BSP_DELAY_UNITS_MILLISECONDS);
#else
        R_BSP_SoftwareDelay(10U, BSP_DELAY_UNITS_MILLISECONDS);
#endif
    }
#endif

#if BSP_TZ_SECURE_BUILD
    R_BSP_NonSecureEnter();
#endif
}

void R_BSP_WarmStart (bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0

        R_FACI_LP->DFLCTL = 1U;

#endif
    }

    if (BSP_WARM_START_POST_C == event)
    {
        R_IOPORT_Open(&g_ioport_ctrl, g_ioport.p_cfg);
    }
}

#if BSP_TZ_SECURE_BUILD

BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ();

BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ()
{

}
#endif
