#include "app_main.h"

#include "config.h"
#include "cloud_speaker_demo_client.h"
#include "log.h"
#include "test_audio_data.h"
#include "w800_at.h"
#include "w800_wifi.h"
#include "../audio_capture.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define W800_BOOT_PROBE_MAX_RETRY    (3U)
#define WIFI_IP_WAIT_TIMEOUT_MS       (20000U)
#define WIFI_IP_REJOIN_MAX_RETRY      (1U)

typedef enum e_app_state
{
    WIFI_INIT = 0,
    WIFI_CONFIG,
    WIFI_REBOOT,
    WIFI_WAIT_READY,
    WIFI_JOIN,
    WIFI_CHECK_IP,
    WIFI_READY,
    UPLOAD_START,
    UPLOAD_SEND,
    UPLOAD_WAIT_RESP,
    RESULT_PARSE,
    DONE,
    ERROR_STATE
} app_state_t;

static app_state_t s_state = WIFI_INIT;
static int s_last_err = MVP_OK;
static const test_audio_clip_t * s_clip = NULL;
static size_t s_clip_index = 0U;
static size_t s_clip_count = 0U;
static char s_last_reply[CLOUD_TEXT_MAX_LEN];
static const char * s_last_step = "init";
static uint8_t s_boot_probe_retry = 0U;
static bool s_boot_wait_done = false;

static int contains_ipv4(const char * text)
{
    size_t i;

    if (NULL == text)
    {
        return 0;
    }

    for (i = 0U; text[i] != '\0'; i++)
    {
        if (isdigit((unsigned char) text[i]))
        {
            if (strchr(text + i, '.') != NULL)
            {
                return 1;
            }
        }
    }

    return 0;
}

static int parse_lkstt_status(const char * text, int * out_status)
{
    const char * p;
    int found = 0;
    int last_status = -1;

    if ((NULL == text) || (NULL == out_status))
    {
        return 0;
    }

    p = text;
    while (NULL != (p = strstr(p, "+OK=")))
    {
        p += 4;
        while ((' ' == *p) || ('\t' == *p))
        {
            p++;
        }
        if ((*p >= '0') && (*p <= '9'))
        {
            last_status = (*p - '0');
            found = 1;
            if (1 == last_status)
            {
                *out_status = last_status;
                return 1;
            }
        }
        p++;
    }

    if (found)
    {
        *out_status = last_status;
    }

    return found;
}

static int config_is_placeholder(void)
{
    if ((0 == strcmp(WIFI_SSID, "YOUR_SSID")) || (0 == strcmp(WIFI_PASSWORD, "YOUR_PASSWORD")))
    {
        return 1;
    }

    return 0;
}

static void enter_error(int err)
{
    s_last_step = "unknown";
    s_last_err = err;
    s_state = ERROR_STATE;
}

static void enter_error_step(const char * step, int err)
{
    s_last_step = (NULL != step) ? step : "unknown";
    s_last_err = err;
    s_state = ERROR_STATE;
}

void app_main_init(void)
{
    audio_debug_only_init();

    s_state = WIFI_INIT;
    s_last_err = MVP_OK;
    s_clip = NULL;
    s_clip_index = 0U;
    s_clip_count = 0U;
    s_last_reply[0] = '\0';
    s_last_step = "WIFI_INIT";
    s_boot_probe_retry = 0U;
    s_boot_wait_done = false;

    LOGI("MVP start: RA6M5 + W800 speaker replay demo");
}

void app_main_poll(void)
{
    int rc;

    switch (s_state)
    {
        case WIFI_INIT:
            rc = w800_init();
            if (MVP_OK != rc)
            {
                LOGE("w800_init failed rc=%d", rc);
                enter_error_step("WIFI_INIT:w800_init", rc);
                break;
            }
            s_state = WIFI_CONFIG;
            break;

        case WIFI_CONFIG:
            if (config_is_placeholder())
            {
                LOGE("Please set WIFI_SSID and WIFI_PASSWORD in src/mvp/config.h");
                enter_error(MVP_ERR_ARG);
                break;
            }

            if (!s_boot_wait_done)
            {
                int ready_rc = w800_wait_for_user_task(W800_BOOT_WAIT_MS);
                if (MVP_OK == ready_rc)
                {
                    LOGI("W800 boot banner detected.");
                }
                else
                {
                    LOGW("W800 boot banner not seen within %lu ms, continue AT probe.",
                         (unsigned long) W800_BOOT_WAIT_MS);
                }
                s_boot_wait_done = true;
            }

            rc = w800_basic_check();
            if (MVP_OK != rc)
            {
                if ((MVP_ERR_TIMEOUT == rc) && (s_boot_probe_retry < W800_BOOT_PROBE_MAX_RETRY))
                {
                    s_boot_probe_retry++;
                    LOGW("W800 AT probe timeout, retry %u/%u - HW reset and re-wait",
                         (unsigned int) s_boot_probe_retry,
                         (unsigned int) W800_BOOT_PROBE_MAX_RETRY);
                    w800_hw_reset();
                    s_boot_wait_done = false;
                    break;
                }
                LOGE("w800_basic_check failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_basic_check", rc);
                break;
            }

            if (s_boot_probe_retry > 0U)
            {
                LOGI("W800 AT probe recovered after retry=%u", (unsigned int) s_boot_probe_retry);
            }

            rc = w800_set_echo(false);
            if (MVP_OK != rc)
            {
                LOGW("set echo off failed rc=%d, continue", rc);
            }

            rc = w800_set_mode_sta();
            if (MVP_OK != rc)
            {
                LOGE("w800_set_mode_sta failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_set_mode_sta", rc);
                break;
            }
            rc = w800_set_ssid(WIFI_SSID);
            if (MVP_OK != rc)
            {
                LOGE("w800_set_ssid failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_set_ssid", rc);
                break;
            }
            rc = w800_set_key_ascii(WIFI_PASSWORD);
            if (MVP_OK != rc)
            {
                LOGE("w800_set_key_ascii failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_set_key_ascii", rc);
                break;
            }
            rc = w800_set_dhcp();
            if (MVP_OK != rc)
            {
                LOGE("w800_set_dhcp failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_set_dhcp", rc);
                break;
            }
            rc = w800_save_config();
            if (MVP_OK != rc)
            {
                LOGE("w800_save_config failed rc=%d", rc);
                enter_error_step("WIFI_CONFIG:w800_save_config", rc);
                break;
            }
            s_state = WIFI_REBOOT;
            break;

        case WIFI_REBOOT:
            rc = w800_reset();
            if (MVP_OK != rc)
            {
                LOGE("w800_reset failed rc=%d", rc);
                enter_error_step("WIFI_REBOOT:w800_reset", rc);
                break;
            }
            s_state = WIFI_WAIT_READY;
            break;

        case WIFI_WAIT_READY:
            (void) w800_wait_for_user_task(W800_BOOT_WAIT_MS);
            rc = w800_basic_check();
            if (MVP_OK != rc)
            {
                LOGE("W800 not ready after reboot rc=%d", rc);
                enter_error_step("WIFI_WAIT_READY:w800_basic_check", MVP_ERR_NOT_READY);
                break;
            }
            s_state = WIFI_JOIN;
            break;

        case WIFI_JOIN:
            rc = w800_join_ap();
            if (MVP_OK != rc)
            {
                char st[512];
                int st_rc = w800_get_link_status(st, sizeof(st));
                LOGE("AT+WJOIN failed rc=%d", rc);
                if (MVP_OK == st_rc)
                {
                    LOGW("LKSTT after WJOIN fail: %s", st);
                    if (contains_ipv4(st))
                    {
                        LOGW("Already got IP, continue workflow.");
                        s_state = WIFI_CHECK_IP;
                        break;
                    }
                }

                enter_error(MVP_ERR_WIFI_JOIN);
                break;
            }
            s_state = WIFI_CHECK_IP;
            break;

        case WIFI_CHECK_IP:
        {
            char st[512];
            char last_st[512];
            uint32_t wait_deadline;
            int lk_status;
            int ip_wait_round;
            last_st[0] = '\0';

            for (ip_wait_round = 0; ip_wait_round <= (int) WIFI_IP_REJOIN_MAX_RETRY; ip_wait_round++)
            {
                wait_deadline = w800_now_ms() + WIFI_IP_WAIT_TIMEOUT_MS;
                lk_status = -1;

                while ((int32_t) (w800_now_ms() - wait_deadline) < 0)
                {
                    rc = w800_get_link_status(st, sizeof(st));
                    if (MVP_OK != rc)
                    {
                        R_BSP_SoftwareDelay(200U, BSP_DELAY_UNITS_MILLISECONDS);
                        continue;
                    }

                    if (0 != strcmp(last_st, st))
                    {
                        LOGI("LKSTT: %s", st);
                        strncpy(last_st, st, sizeof(last_st) - 1U);
                        last_st[sizeof(last_st) - 1U] = '\0';
                    }

                    if (parse_lkstt_status(st, &lk_status) && (lk_status == 1))
                    {
                        if (!contains_ipv4(st))
                        {
                            LOGW("LKSTT status=1 (linked), IP text not fully present yet.");
                        }

                        s_state = WIFI_READY;
                        break;
                    }

                    R_BSP_SoftwareDelay(200U, BSP_DELAY_UNITS_MILLISECONDS);
                }

                if (WIFI_READY == s_state)
                {
                    break;
                }

                if (ip_wait_round < (int) WIFI_IP_REJOIN_MAX_RETRY)
                {
                    LOGW("No IP yet after %lu ms, retry WJOIN (%d/%u). Last LKSTT: %s",
                         (unsigned long) WIFI_IP_WAIT_TIMEOUT_MS,
                         ip_wait_round + 1,
                         (unsigned int) WIFI_IP_REJOIN_MAX_RETRY,
                         ('\0' != last_st[0]) ? last_st : "<none>");
                    (void) w800_join_ap();
                }
            }

            if (WIFI_READY != s_state)
            {
                enter_error_step("WIFI_CHECK_IP", MVP_ERR_NO_IP);
            }

            break;
        }

        case WIFI_READY:
            rc = speaker_demo_client_init();
            if (MVP_OK != rc)
            {
                enter_error(rc);
                break;
            }

            s_clip_count = test_audio_clip_count();
            s_clip_index = 0U;
            if (0U == s_clip_count)
            {
                LOGE("No embedded voice clips found. Generate src/mvp/test_audio_data.c with tools/record_speaker_dataset.py");
                enter_error(MVP_ERR_ARG);
                break;
            }

            LOGI("WIFI ready, embedded clip count=%lu", (unsigned long) s_clip_count);
            s_state = UPLOAD_START;
            break;

        case UPLOAD_START:
            s_clip = test_audio_get_clip(s_clip_index);
            if (NULL == s_clip)
            {
                enter_error(MVP_ERR_ARG);
                break;
            }
            LOGI("Prepare clip[%lu/%lu]: %s role=%s speaker=%s",
                 (unsigned long) (s_clip_index + 1U),
                 (unsigned long) s_clip_count,
                 s_clip->name,
                 (TEST_AUDIO_ROLE_ENROLL == s_clip->role) ? "enroll" : "identify",
                 s_clip->speaker_id);
            s_state = UPLOAD_SEND;
            break;

        case UPLOAD_SEND:
        {
            int upload_try;
            int upload_ok = 0;
            for (upload_try = 0; upload_try < 3; upload_try++)
            {
                if (upload_try > 0)
                {
                    LOGW("UPLOAD retry %d/2", upload_try);
                    R_BSP_SoftwareDelay(500U, BSP_DELAY_UNITS_MILLISECONDS);
                }
                rc = speaker_demo_client_send_clip(s_clip, s_last_reply, sizeof(s_last_reply));
                if (MVP_OK == rc)
                {
                    upload_ok = 1;
                    break;
                }
                LOGW("upload attempt %d failed rc=%d", upload_try + 1, rc);
            }
            if (!upload_ok)
            {
                enter_error_step("UPLOAD_SEND", rc);
                break;
            }
            s_state = UPLOAD_WAIT_RESP;
            break;
        }

        case UPLOAD_WAIT_RESP:
            s_state = RESULT_PARSE;
            break;

        case RESULT_PARSE:
        {
            LOGI("Speaker server reply: %s", s_last_reply);
            s_clip_index++;
            if (s_clip_index < s_clip_count)
            {
                s_state = UPLOAD_START;
            }
            else
            {
                s_state = DONE;
            }
            break;
        }

        case DONE:
            break;

        case ERROR_STATE:
            LOGE("MVP state error=%d step=%s", s_last_err, s_last_step);
            s_state = DONE;
            break;

        default:
            enter_error(MVP_ERR_ARG);
            break;
    }
}
