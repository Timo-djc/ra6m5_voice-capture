#ifndef MVP_CONFIG_H_
#define MVP_CONFIG_H_

#include <stdint.h>
#include <stddef.h>

#define WIFI_SSID              "timo"
#define WIFI_PASSWORD          "dcjdcjdcj"

#define CLOUD_HOST_NAME        ""
#define CLOUD_HOST_IP          "172.20.10.6"
#define CLOUD_PORT             (8000U)
#define CLOUD_PATH             "/asr/recognize"

#define DEVICE_ID              "ra6m5-w800-01"
#define SAMPLE_RATE            (16000U)
#define AUDIO_CHANNELS         (1U)

#define HTTP_TIMEOUT_MS        (10000U)
#define RETRY_COUNT            (2U)

#define W800_UART_BAUD         (115200U)
#define W800_CMD_TIMEOUT_MS    (1500U)
#define W800_JOIN_TIMEOUT_MS   (10000U)
#define W800_BOOT_WAIT_MS      (5000U)

#define W800_UART_RX_BUF_SIZE  (8192U)
#define W800_LINE_BUF_SIZE     (384U)

#define CLOUD_RX_BUFFER_SIZE   (12288U)
#define CLOUD_TEXT_MAX_LEN     (256U)

typedef enum e_mvp_err
{
    MVP_OK = 0,
    MVP_ERR_ARG = -1,
    MVP_ERR_TIMEOUT = -2,
    MVP_ERR_UART = -3,
    MVP_ERR_AT = -4,
    MVP_ERR_NOT_READY = -5,
    MVP_ERR_WIFI_JOIN = -6,
    MVP_ERR_NO_IP = -7,
    MVP_ERR_SOCKET = -8,
    MVP_ERR_HTTP = -9,
    MVP_ERR_JSON = -10,
    MVP_ERR_OVERFLOW = -11,
    MVP_ERR_SIZE_MISMATCH = -12,
    MVP_ERR_NOMEM = -13
} mvp_err_t;

#endif
