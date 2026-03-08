#include "drv_touch.h"
#include "hal_data.h"
#include "drv_disp.h"
#include "vector_data.h"
#include <stdio.h>

/* SCI3 I2C pins */
#define SCL_PIN BSP_IO_PORT_07_PIN_06
#define SDA_PIN BSP_IO_PORT_07_PIN_07
#define TP_RST_PIN BSP_IO_PORT_04_PIN_03
#define TP_INT_PIN BSP_IO_PORT_01_PIN_11
#define TP_INT_IRQ_CHANNEL   (6U)
#define TP_INT_IRQ_PRIORITY  (12U)
#define TOUCH_IDLE_POLL_CYCLES (5U)

#define I2C_WAIT_TIMEOUT  200000U
#define I2C_WAIT_STEP_US  5U
#define I2C_RETRY_COUNT   2U

typedef struct
{
    uint16_t width;
    uint16_t height;
    tp_rotation_t rotation;
    uint8_t i2c_addr;
    uint8_t touch_count;
    TouchPoint points[FT6336_MAX_POINTS];
    bool comm_ok;
    bool i2c_opened;
} ft6336_driver_t;

static ft6336_driver_t s_ft6336 =
{
    .width = 480U,
    .height = 320U,
    .rotation = TP_ROT_90,
    .i2c_addr = FT6336_ADDR,
    .touch_count = 0U,
    .points = {{0U, 0U, false}, {0U, 0U, false}},
    .comm_ok = false,
    .i2c_opened = false
};

static volatile bool s_i2c_tx_done = false;
static volatile bool s_i2c_rx_done = false;
static volatile bool s_i2c_aborted = false;
static uint32_t s_i2c_dbg_fail_logs = 0U;
static volatile bool s_touch_irq_pending = false;
static bool s_touch_tracking = false;
static TouchPoint s_touch_last_point = {0U, 0U, false};
static uint8_t s_idle_poll_div = 0U;

extern void uart_write_line_ext(const char * str);

void sci_i2c_master_callback(i2c_master_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    switch (p_args->event)
    {
        case I2C_MASTER_EVENT_TX_COMPLETE:
            s_i2c_tx_done = true;
            break;
        case I2C_MASTER_EVENT_RX_COMPLETE:
            s_i2c_rx_done = true;
            break;
        case I2C_MASTER_EVENT_ABORTED:
        default:
            s_i2c_aborted = true;
            break;
    }
}

static bool i2c_wait_flag(volatile bool * flag)
{
    for (uint32_t t = 0; t < I2C_WAIT_TIMEOUT; t++)
    {
        if (*flag)
        {
            *flag = false;
            return true;
        }
        if (s_i2c_aborted)
        {
            return false;
        }
        R_BSP_SoftwareDelay(I2C_WAIT_STEP_US, BSP_DELAY_UNITS_MICROSECONDS);
    }

    return false;
}

static void FT6336_UpdateDisplaySize(void)
{
    DisplayDevice * disp = LCDGetDevice();
    if (disp != NULL)
    {
        s_ft6336.width = disp->wXres;
        s_ft6336.height = disp->wYres;
    }
    else
    {
        s_ft6336.width = 480U;
        s_ft6336.height = 320U;
    }
}

static void FT6336_MapPoint(uint16_t raw_x, uint16_t raw_y, uint16_t * x, uint16_t * y)
{
    uint16_t w = s_ft6336.width;
    uint16_t h = s_ft6336.height;
    int32_t mx = (int32_t) raw_x;
    int32_t my = (int32_t) raw_y;
    int32_t t;

    if ((w == 0U) || (h == 0U))
    {
        *x = 0U;
        *y = 0U;
        return;
    }

    switch (s_ft6336.rotation)
    {
        case TP_ROT_NONE:
            break;
        case TP_ROT_90:
            t = mx;
            mx = my;
            my = (int32_t) h - 1 - t;
            break;
        case TP_ROT_180:
            mx = (int32_t) w - 1 - mx;
            my = (int32_t) h - 1 - my;
            break;
        case TP_ROT_270:
            t = mx;
            mx = (int32_t) w - 1 - my;
            my = t;
            break;
        default:
            break;
    }

    if (mx < 0)
    {
        mx = 0;
    }
    if (my < 0)
    {
        my = 0;
    }
    if (mx >= (int32_t) w)
    {
        mx = (int32_t) w - 1;
    }
    if (my >= (int32_t) h)
    {
        my = (int32_t) h - 1;
    }

    *x = (uint16_t) mx;
    *y = (uint16_t) my;
}

static bool FT6336_ReadRegs(uint8_t reg, uint8_t * buf, uint8_t len)
{
    fsp_err_t err;
    char logbuf[96];
    bool logged = false;

    if ((NULL == buf) || (0U == len))
    {
        return false;
    }

    for (uint32_t try_idx = 0U; try_idx < I2C_RETRY_COUNT; try_idx++)
    {
        s_i2c_tx_done = false;
        s_i2c_rx_done = false;
        s_i2c_aborted = false;

        err = g_i2c0.p_api->write(g_i2c0.p_ctrl, &reg, 1U, true);
        if (FSP_SUCCESS != err)
        {
            if ((!logged) && (s_i2c_dbg_fail_logs < 10U))
            {
                snprintf(logbuf, sizeof(logbuf), "TP_I2C: write reg=0x%02X err=%d\r\n", reg, (int) err);
                uart_write_line_ext(logbuf);
                s_i2c_dbg_fail_logs++;
                logged = true;
            }
            (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
            R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }
        if (!i2c_wait_flag(&s_i2c_tx_done))
        {
            if ((!logged) && (s_i2c_dbg_fail_logs < 10U))
            {
                snprintf(logbuf, sizeof(logbuf), "TP_I2C: tx wait fail reg=0x%02X abort=%u\r\n", reg, s_i2c_aborted ? 1U : 0U);
                uart_write_line_ext(logbuf);
                s_i2c_dbg_fail_logs++;
                logged = true;
            }
            s_i2c_aborted = false;
            (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
            R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        err = g_i2c0.p_api->read(g_i2c0.p_ctrl, buf, (uint32_t) len, false);
        if (FSP_SUCCESS != err)
        {
            if ((!logged) && (s_i2c_dbg_fail_logs < 10U))
            {
                snprintf(logbuf, sizeof(logbuf), "TP_I2C: read reg=0x%02X err=%d\r\n", reg, (int) err);
                uart_write_line_ext(logbuf);
                s_i2c_dbg_fail_logs++;
                logged = true;
            }
            (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
            R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }
        if (!i2c_wait_flag(&s_i2c_rx_done))
        {
            if ((!logged) && (s_i2c_dbg_fail_logs < 10U))
            {
                snprintf(logbuf, sizeof(logbuf), "TP_I2C: rx wait fail reg=0x%02X abort=%u\r\n", reg, s_i2c_aborted ? 1U : 0U);
                uart_write_line_ext(logbuf);
                s_i2c_dbg_fail_logs++;
                logged = true;
            }
            s_i2c_aborted = false;
            (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
            R_BSP_SoftwareDelay(1, BSP_DELAY_UNITS_MILLISECONDS);
            continue;
        }

        return true;
    }

    return false;
}

static bool FT6336_Probe(void)
{
    uint8_t chip_id = 0U;
    uint8_t vendor_id = 0U;
    bool chip_ok = FT6336_ReadRegs(0xA3U, &chip_id, 1U);
    bool vendor_ok = FT6336_ReadRegs(0xA8U, &vendor_id, 1U);
    char logbuf[96];

    if (chip_ok && vendor_ok)
    {
        snprintf(logbuf, sizeof(logbuf), "FT6336 probe ok: addr=0x%02X CHIP=0x%02X VENDOR=0x%02X (SCI3-I2C)\r\n",
                 s_ft6336.i2c_addr, chip_id, vendor_id);
        uart_write_line_ext(logbuf);
        return true;
    }

    snprintf(logbuf, sizeof(logbuf), "FT6336 probe fail on addr=0x%02X (SCI3-I2C P706/P707)\r\n", s_ft6336.i2c_addr);
    uart_write_line_ext(logbuf);
    return false;
}

static bool FT6336_SelectAddressAndProbe(void)
{
    static const uint8_t addr_candidates[] = {FT6336_ADDR, 0x70U};
    char logbuf[96];

    for (uint32_t i = 0U; i < (sizeof(addr_candidates) / sizeof(addr_candidates[0])); i++)
    {
        s_ft6336.i2c_addr = addr_candidates[i];
        (void) g_i2c0.p_api->slaveAddressSet(g_i2c0.p_ctrl, s_ft6336.i2c_addr, I2C_MASTER_ADDR_MODE_7BIT);

        snprintf(logbuf, sizeof(logbuf), "FT6336 probe try addr=0x%02X\r\n", s_ft6336.i2c_addr);
        uart_write_line_ext(logbuf);

        if (FT6336_Probe())
        {
            return true;
        }
    }

    return false;
}

static bool FT6336_PingAddress(uint8_t addr)
{
    uint8_t dummy = 0x00U;
    fsp_err_t err;

    s_i2c_tx_done = false;
    s_i2c_rx_done = false;
    s_i2c_aborted = false;

    (void) g_i2c0.p_api->slaveAddressSet(g_i2c0.p_ctrl, addr, I2C_MASTER_ADDR_MODE_7BIT);

    err = g_i2c0.p_api->write(g_i2c0.p_ctrl, &dummy, 1U, false);
    if (FSP_SUCCESS != err)
    {
        (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
        return false;
    }

    if (!i2c_wait_flag(&s_i2c_tx_done))
    {
        s_i2c_aborted = false;
        (void) g_i2c0.p_api->abort(g_i2c0.p_ctrl);
        return false;
    }

    return true;
}

static void FT6336_ScanBusOnce(void)
{
    char logbuf[96];
    uint8_t found = 0U;

    uart_write_line_ext("TP_I2C: bus scan 0x08..0x77 start\r\n");
    for (uint8_t addr = 0x08U; addr <= 0x77U; addr++)
    {
        if (FT6336_PingAddress(addr))
        {
            snprintf(logbuf, sizeof(logbuf), "TP_I2C: device ack at 0x%02X\r\n", addr);
            uart_write_line_ext(logbuf);
            found++;
        }
    }

    if (0U == found)
    {
        uart_write_line_ext("TP_I2C: no ack device found\r\n");
    }
}

static void FT6336_ClearPoints(void)
{
    s_ft6336.touch_count = 0U;
    for (uint8_t i = 0; i < FT6336_MAX_POINTS; i++)
    {
        s_ft6336.points[i].x = 0U;
        s_ft6336.points[i].y = 0U;
        s_ft6336.points[i].is_pressed = false;
    }
}

void FT6336_Init(void)
{
    fsp_err_t err;
    char logbuf[96];

    uart_write_line_ext("FT6336_Init enter\r\n");

    /* Configure SCI3 I2C pins to peripheral mode (open-drain). */
    R_IOPORT_PinCfg(&g_ioport_ctrl, SCL_PIN,
                    ((uint32_t) IOPORT_CFG_NMOS_ENABLE |
                     (uint32_t) IOPORT_CFG_PULLUP_ENABLE |
                     (uint32_t) IOPORT_CFG_PERIPHERAL_PIN |
                     (uint32_t) IOPORT_PERIPHERAL_SCI1_3_5_7_9));
    R_IOPORT_PinCfg(&g_ioport_ctrl, SDA_PIN,
                    ((uint32_t) IOPORT_CFG_NMOS_ENABLE |
                     (uint32_t) IOPORT_CFG_PULLUP_ENABLE |
                     (uint32_t) IOPORT_CFG_PERIPHERAL_PIN |
                     (uint32_t) IOPORT_PERIPHERAL_SCI1_3_5_7_9));

    R_IOPORT_PinCfg(&g_ioport_ctrl, TP_RST_PIN,
                    ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                     (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH));
    R_IOPORT_PinCfg(&g_ioport_ctrl, TP_INT_PIN,
                    ((uint32_t) IOPORT_CFG_PULLUP_ENABLE |
                     (uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT |
                     (uint32_t) IOPORT_CFG_IRQ_ENABLE |
                     (uint32_t) IOPORT_CFG_EVENT_FALLING_EDGE));

    /* Optional touch reset pulse (if TP_RST is connected). */
    R_IOPORT_PinWrite(&g_ioport_ctrl, TP_RST_PIN, BSP_IO_LEVEL_LOW);
    R_BSP_SoftwareDelay(5, BSP_DELAY_UNITS_MILLISECONDS);
    R_IOPORT_PinWrite(&g_ioport_ctrl, TP_RST_PIN, BSP_IO_LEVEL_HIGH);
    R_BSP_SoftwareDelay(50, BSP_DELAY_UNITS_MILLISECONDS);

    if (!s_ft6336.i2c_opened)
    {
        err = g_i2c0.p_api->open(g_i2c0.p_ctrl, g_i2c0.p_cfg);
        if (FSP_SUCCESS != err)
        {
            snprintf(logbuf, sizeof(logbuf), "FT6336 I2C open failed: %d\r\n", (int) err);
            uart_write_line_ext(logbuf);
            s_ft6336.comm_ok = false;
            return;
        }
        s_ft6336.i2c_opened = true;
    }

    FT6336_UpdateDisplaySize();
    /* Default to raw mapping; panel + LCD orientation is already aligned. */
    s_ft6336.rotation = TP_ROT_NONE;
    s_ft6336.comm_ok = FT6336_SelectAddressAndProbe();
    if (!s_ft6336.comm_ok)
    {
        FT6336_ScanBusOnce();
    }
    FT6336_ClearPoints();
    s_touch_irq_pending = true;
    s_touch_tracking = false;
    s_touch_last_point.x = 0U;
    s_touch_last_point.y = 0U;
    s_touch_last_point.is_pressed = false;
    s_idle_poll_div = 0U;

    R_ICU->IRQCR[TP_INT_IRQ_CHANNEL] = (uint8_t) (R_ICU_IRQCR_FLTEN_Msk | 0x01U);
    R_BSP_IrqCfgEnable(VECTOR_NUMBER_ICU_IRQ6, TP_INT_IRQ_PRIORITY, NULL);

    uart_write_line_ext("FT6336 SCI3-I2C driver init done.\r\n");
}

void FT6336_SetRotation(tp_rotation_t rotation)
{
    s_ft6336.rotation = rotation;
}

bool FT6336_Scan(void)
{
    uint8_t buf[11];
    uint8_t touch_count;
    static uint32_t comm_fail_count = 0U;
    char logbuf[96];

    FT6336_UpdateDisplaySize();
    FT6336_ClearPoints();

    if (!FT6336_ReadRegs(FT_REG_NUM_TOUCH, buf, sizeof(buf)))
    {
        s_ft6336.comm_ok = false;
        comm_fail_count++;
        if ((comm_fail_count % 20U) == 1U)
        {
            snprintf(logbuf, sizeof(logbuf), "FT6336 SCI3-I2C comm fail cnt=%lu\r\n", (unsigned long) comm_fail_count);
            uart_write_line_ext(logbuf);
        }
        return false;
    }

    comm_fail_count = 0U;
    s_ft6336.comm_ok = true;

    touch_count = (uint8_t) (buf[0] & 0x0FU);
    if (touch_count > FT6336_MAX_POINTS)
    {
        touch_count = FT6336_MAX_POINTS;
    }
    s_ft6336.touch_count = touch_count;

    if (touch_count >= 1U)
    {
        uint16_t raw_x = (uint16_t) (((uint16_t) (buf[1] & 0x0FU) << 8) | (uint16_t) buf[2]);
        uint16_t raw_y = (uint16_t) (((uint16_t) (buf[3] & 0x0FU) << 8) | (uint16_t) buf[4]);
        FT6336_MapPoint(raw_x, raw_y, &s_ft6336.points[0].x, &s_ft6336.points[0].y);
        s_ft6336.points[0].is_pressed = true;
    }

    if (touch_count >= 2U)
    {
        uint16_t raw_x = (uint16_t) (((uint16_t) (buf[7] & 0x0FU) << 8) | (uint16_t) buf[8]);
        uint16_t raw_y = (uint16_t) (((uint16_t) (buf[9] & 0x0FU) << 8) | (uint16_t) buf[10]);
        FT6336_MapPoint(raw_x, raw_y, &s_ft6336.points[1].x, &s_ft6336.points[1].y);
        s_ft6336.points[1].is_pressed = true;
    }

    return true;
}

uint8_t FT6336_GetTouchCount(void)
{
    return s_ft6336.touch_count;
}

bool FT6336_GetPoint(uint8_t index, TouchPoint * point)
{
    if (NULL == point)
    {
        return false;
    }

    point->x = 0U;
    point->y = 0U;
    point->is_pressed = false;

    if (index >= s_ft6336.touch_count)
    {
        return false;
    }

    *point = s_ft6336.points[index];
    return true;
}

bool FT6336_Read_Touch(TouchPoint * point)
{
    if (NULL == point)
    {
        return false;
    }

    point->x = 0U;
    point->y = 0U;
    point->is_pressed = false;

    if (!FT6336_Scan())
    {
        return false;
    }

    if (0U == s_ft6336.touch_count)
    {
        return true;
    }

    *point = s_ft6336.points[0];
    return true;
}

void FT6336_OnInterrupt(void)
{
    s_touch_irq_pending = true;
    s_idle_poll_div = 0U;
}

bool FT6336_IsPressed(TouchPoint * point)
{
    if (point != NULL)
    {
        *point = s_touch_last_point;
    }

    return s_touch_last_point.is_pressed;
}

bool FT6336_GetEvent(TouchEvent * event)
{
    TouchPoint now_point = {0U, 0U, false};
    bool scan_ok;
    bool should_scan;

    if (NULL == event)
    {
        return false;
    }

    event->type = TOUCH_EVENT_NONE;
    event->point.x = 0U;
    event->point.y = 0U;
    event->point.is_pressed = false;

    should_scan = s_touch_irq_pending || s_touch_tracking;
    if (!should_scan)
    {
        /* Fallback: keep low-rate polling so touch still works if INT wiring/IRQ mapping is wrong. */
        s_idle_poll_div++;
        if (s_idle_poll_div < TOUCH_IDLE_POLL_CYCLES)
        {
            return false;
        }
        s_idle_poll_div = 0U;
        should_scan = true;
    }

    s_touch_irq_pending = false;
    scan_ok = FT6336_Read_Touch(&now_point);
    if (!scan_ok)
    {
        return false;
    }

    if (now_point.is_pressed && !s_touch_tracking)
    {
        s_touch_tracking = true;
        s_touch_last_point = now_point;
        event->type = TOUCH_EVENT_DOWN;
        event->point = now_point;
        return true;
    }

    if (now_point.is_pressed && s_touch_tracking)
    {
        if ((now_point.x != s_touch_last_point.x) || (now_point.y != s_touch_last_point.y))
        {
            s_touch_last_point = now_point;
            event->type = TOUCH_EVENT_MOVE;
            event->point = now_point;
            return true;
        }

        s_touch_last_point = now_point;
        return false;
    }

    if ((!now_point.is_pressed) && s_touch_tracking)
    {
        event->type = TOUCH_EVENT_UP;
        event->point = s_touch_last_point;
        event->point.is_pressed = false;
        s_touch_tracking = false;
        s_touch_last_point.is_pressed = false;
        return true;
    }

    return false;
}
