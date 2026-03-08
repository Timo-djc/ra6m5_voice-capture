#include "drv_lcd.h"
#include "drv_disp.h"
#include <stdio.h>

extern void sci_spi0_init_baremetal(void);
extern void sci_spi0_write_byte(uint8_t dat);
extern void sci_spi0_wait_tx_end(void);
extern void sci_spi0_write_buffer(uint8_t* pbuf, uint32_t length);
extern void drv_lcd_pin_init(void);
static void LCDDrvWriteCS(CS eState);
static void LCDDrvWriteDCX(DCX eState);
static void LCDDrvWriteReset(Reset eState);
static void LCDDrvWriteBlack(Black eState);
static void LCDDrvHWReset(void);
static void LCDDrvWriteReg(uint8_t reg);
static void LCDDrvWriteDat(uint8_t dat);

static void LCDDrvInit(struct DisplayDevice* ptDev);
static void LCDDrvSetDisplayOn(struct DisplayDevice* ptDev);
static void LCDDrvSetDisplayOff(struct DisplayDevice* ptDev);
static void LCDDrvSetDisplayWindow(struct DisplayDevice* ptDev, \
                                   unsigned short wXs, unsigned short wYs, \
                                   unsigned short wXe, unsigned short wYe);
static void LCDDrvFlush(struct DisplayDevice *ptDev);
static int  LCDDrvSetPixel(struct DisplayDevice *ptDev, \
                           unsigned short wX, unsigned short wY, \
                           uint32_t color888);

// Framebuffer array completely removed to prevent massive RAM stack overflow.

/* Convert RGB888 to RGB666 bytes for ILI9488 SPI 18-bit mode */
static inline void rgb888_to_rgb666(uint32_t color888, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r8 = (uint8_t)((color888 >> 16) & 0xFF);
    uint8_t g8 = (uint8_t)((color888 >> 8) & 0xFF);
    uint8_t b8 = (uint8_t)(color888 & 0xFF);
    uint8_t r6 = (uint8_t)(r8 >> 2);
    uint8_t g6 = (uint8_t)(g8 >> 2);
    uint8_t b6 = (uint8_t)(b8 >> 2);

    /* ILI9488 18-bit mode uses the upper 6 bits of each transmitted byte. */
    *r = (uint8_t)(r6 << 2);
    *g = (uint8_t)(g6 << 2);
    *b = (uint8_t)(b6 << 2);
}

static DisplayDevice gLcdDevice = {
        .name = "LCD",
        .FBBase = NULL,
        .wXres = 480,
        .wYres = 320,
        .wBpp = 18,
        .dwSize = 0,
        .Init = LCDDrvInit,
        .DisplayON = LCDDrvSetDisplayOn,
        .DisplayOFF = LCDDrvSetDisplayOff,
        .SetDisplayWindow = LCDDrvSetDisplayWindow,
        .Flush = LCDDrvFlush,
        .SetPixel = LCDDrvSetPixel
};

struct DisplayDevice *LCDGetDevice(void)
{
    return &gLcdDevice;
}

static void LCDDrvWriteCS(CS eState)
{
    if (eState == isSelect) R_PORT1->PODR &= ~(1U << 3);
    else                    R_PORT1->PODR |=  (1U << 3);
}
static void LCDDrvWriteDCX(DCX eState)
{
    if (eState == isCommand) R_PORT1->PODR &= ~(1U << 4);
    else                     R_PORT1->PODR |=  (1U << 4);
}
static void LCDDrvWriteReset(Reset eState)
{
    if (eState == isReset) R_PORT1->PODR &= ~(1U << 5);
    else                   R_PORT1->PODR |=  (1U << 5);
}
static void LCDDrvWriteBlack(Black eState)
{
    if (eState == isLight) R_PORT6->PODR |=  (1U << 8); // Backlight on (HIGH)
    else                   R_PORT6->PODR &= ~(1U << 8); // Backlight off (LOW)
}

static void LCDDrvHWReset(void)
{
    LCDDrvWriteReset(isReset);
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MILLISECONDS);
    LCDDrvWriteReset(notReset);
    R_BSP_SoftwareDelay(50, BSP_DELAY_UNITS_MILLISECONDS);
}

static void LCDDrvWriteReg(uint8_t reg)
{
    sci_spi0_wait_tx_end();    // Ensure previous transfer finishes before switching DCX
    LCDDrvWriteDCX(isCommand);
    sci_spi0_write_byte(reg);
    sci_spi0_wait_tx_end();    // Wait again for safety
}

static void LCDDrvWriteDat(uint8_t dat)
{
    sci_spi0_wait_tx_end();
    LCDDrvWriteDCX(isData);
    sci_spi0_write_byte(dat);
    sci_spi0_wait_tx_end();
}

void LCDDrvInit(struct DisplayDevice* ptDev)
{
    if(NULL == ptDev->name)    return;
    
    /* Config MCU Pins manually for LCD */
    drv_lcd_pin_init();

    /* Config MCU Pins manually for Touch I2C (SCI1) */
    /* Handled inside drv_lcd_pin_init now */
    
    /* 打开SPI设备完成初始化 */
    sci_spi0_init_baremetal();

    /* --- SPI Hardware Diagnostic --- */
    {
        extern void uart_write_line_ext(const char* str);
        char dbg[120];
        // Dump SCI0 registers
        sprintf(dbg, "SPI_DIAG: SSR=0x%02X SCR=0x%02X SMR=0x%02X SCMR=0x%02X SPMR=0x%02X BRR=%d\r\n",
                R_SCI0->SSR, R_SCI0->SCR, R_SCI0->SMR, R_SCI0->SCMR, R_SCI0->SPMR, R_SCI0->BRR);
        uart_write_line_ext(dbg);
        // Dump PFS for P100 (MISO), P101 (MOSI), P102 (SCK)
        sprintf(dbg, "SPI_DIAG: PFS_P100=0x%08lX PFS_P101=0x%08lX PFS_P102=0x%08lX\r\n",
                (unsigned long)R_PFS->PORT[1].PIN[0].PmnPFS,
                (unsigned long)R_PFS->PORT[1].PIN[1].PmnPFS,
                (unsigned long)R_PFS->PORT[1].PIN[2].PmnPFS);
        uart_write_line_ext(dbg);
        // Dump PFS for P103 (CS), P104 (DCX), P105 (RESET)
        sprintf(dbg, "SPI_DIAG: PFS_P103=0x%08lX PFS_P104=0x%08lX PFS_P105=0x%08lX\r\n",
                (unsigned long)R_PFS->PORT[1].PIN[3].PmnPFS,
                (unsigned long)R_PFS->PORT[1].PIN[4].PmnPFS,
                (unsigned long)R_PFS->PORT[1].PIN[5].PmnPFS);
        uart_write_line_ext(dbg);
        // Test: write 1 byte dummydump SSR before and after
        uint8_t ssr_before = R_SCI0->SSR;
        R_SCI0->TDR = 0xAA;
        for (volatile int d = 0; d < 100; d++); // brief wait
        uint8_t ssr_after = R_SCI0->SSR;
        sprintf(dbg, "SPI_DIAG: test_write SSR_before=0x%02X SSR_after=0x%02X\r\n",
                ssr_before, ssr_after);
        uart_write_line_ext(dbg);
    }

    /* Clear SPI errors/data left by diagnostic test above */
    {
        volatile uint8_t ssr = R_SCI0->SSR;
        if (ssr & 0x20) { /* ORER */
            R_SCI0->SSR = (uint8_t)((ssr & ~0x38) | 0x84);
        }
        volatile uint8_t d = R_SCI0->RDR; (void)d;
    }

    /* ============================================================
     * ILI9488 Full Initialization Sequence (SPI 4-wire, 18-bit)
     * ============================================================ */
    LCDDrvHWReset();
    LCDDrvWriteCS(isSelect);

    /* Software Reset */
    LCDDrvWriteReg(0x01);
    R_BSP_SoftwareDelay(10, BSP_DELAY_UNITS_MILLISECONDS);

    /* Sleep Out */
    LCDDrvWriteReg(0x11);
    R_BSP_SoftwareDelay(120, BSP_DELAY_UNITS_MILLISECONDS);

    /* Positive Gamma Control */
    LCDDrvWriteReg(0xE0);
    LCDDrvWriteDat(0x00); LCDDrvWriteDat(0x07);
    LCDDrvWriteDat(0x10); LCDDrvWriteDat(0x09);
    LCDDrvWriteDat(0x17); LCDDrvWriteDat(0x0B);
    LCDDrvWriteDat(0x41); LCDDrvWriteDat(0x89);
    LCDDrvWriteDat(0x4B); LCDDrvWriteDat(0x0A);
    LCDDrvWriteDat(0x0C); LCDDrvWriteDat(0x0E);
    LCDDrvWriteDat(0x18); LCDDrvWriteDat(0x1B);
    LCDDrvWriteDat(0x0F);

    /* Negative Gamma Control */
    LCDDrvWriteReg(0xE1);
    LCDDrvWriteDat(0x00); LCDDrvWriteDat(0x17);
    LCDDrvWriteDat(0x1A); LCDDrvWriteDat(0x04);
    LCDDrvWriteDat(0x0E); LCDDrvWriteDat(0x06);
    LCDDrvWriteDat(0x2F); LCDDrvWriteDat(0x45);
    LCDDrvWriteDat(0x43); LCDDrvWriteDat(0x02);
    LCDDrvWriteDat(0x0A); LCDDrvWriteDat(0x09);
    LCDDrvWriteDat(0x32); LCDDrvWriteDat(0x36);
    LCDDrvWriteDat(0x0F);

    /* Power Control 1 */
    LCDDrvWriteReg(0xC0);
    LCDDrvWriteDat(0x17);
    LCDDrvWriteDat(0x15);

    /* Power Control 2 */
    LCDDrvWriteReg(0xC1);
    LCDDrvWriteDat(0x41);

    /* VCOM Control */
    LCDDrvWriteReg(0xC5);
    LCDDrvWriteDat(0x00);
    LCDDrvWriteDat(0x12);
    LCDDrvWriteDat(0x80);

    /* Memory Access Control (MADCTL): Landscape CCW + RGB */
    LCDDrvWriteReg(0x36);
    LCDDrvWriteDat(0xE0); /* MY|MX|MV */

    /* Interface Pixel Format — CRITICAL: ILI9488 SPI only works with 18-bit */
    LCDDrvWriteReg(0x3A);
    LCDDrvWriteDat(0x66); /* 18-bit/pixel (3 bytes per pixel over SPI) */

    /* Interface Mode Control */
    LCDDrvWriteReg(0xB0);
    LCDDrvWriteDat(0x00);

    /* Frame Rate Control (Normal Mode) — 60 Hz */
    LCDDrvWriteReg(0xB1);
    LCDDrvWriteDat(0xA0);

    /* Display Inversion Control — 2-dot */
    LCDDrvWriteReg(0xB4);
    LCDDrvWriteDat(0x02);

    /* Display Function Control */
    LCDDrvWriteReg(0xB6);
    LCDDrvWriteDat(0x02);
    LCDDrvWriteDat(0x02);
    LCDDrvWriteDat(0x3B);

    /* Entry Mode Set */
    LCDDrvWriteReg(0xB7);
    LCDDrvWriteDat(0xC6);

    /* Adjust Control 3 (required for ILI9488 SPI) */
    LCDDrvWriteReg(0xF7);
    LCDDrvWriteDat(0xA9);
    LCDDrvWriteDat(0x51);
    LCDDrvWriteDat(0x2C);
    LCDDrvWriteDat(0x82);

    /* Display Inversion OFF */
    LCDDrvWriteReg(0x20);

    /* Normal Display Mode ON */
    LCDDrvWriteReg(0x13);

    /* Display ON */
    LCDDrvWriteReg(0x29);
    R_BSP_SoftwareDelay(20, BSP_DELAY_UNITS_MILLISECONDS);

    /* Backlight ON */
    LCDDrvWriteBlack(isLight);
}

void LCDDrvSetDisplayOn(struct DisplayDevice* ptDev)
{
    if(NULL == ptDev->name)    return;
    LCDDrvWriteReg(0x29);
}
void LCDDrvSetDisplayOff(struct DisplayDevice* ptDev)
{
    if(NULL == ptDev->name)    return;
    LCDDrvWriteReg(0x28);
}

void LCDDrvSetDisplayWindow(struct DisplayDevice* ptDev, \
                           unsigned short hwXs, unsigned short hwYs, \
                           unsigned short hwXe, unsigned short hwYe)
{
    if(NULL == ptDev->name)    return;
    /* 设置列地址 */
    LCDDrvWriteReg(0x2A);
    LCDDrvWriteDat((uint8_t)(hwXs>>8));       // 起始地址先高后低
    LCDDrvWriteDat((uint8_t)(0x00FF&hwXs));
    LCDDrvWriteDat((uint8_t)(hwXe>>8));        // 结束地址先高后低
    LCDDrvWriteDat((uint8_t)(0x00FF&hwXe));

    /* 设置行地址 */
    LCDDrvWriteReg(0x2B);
    LCDDrvWriteDat((uint8_t)(hwYs>>8));
    LCDDrvWriteDat((uint8_t)(0x00FF&hwYs));
    LCDDrvWriteDat((uint8_t)(hwYe>>8));
    LCDDrvWriteDat((uint8_t)(0x00FF&hwYe));
}

/* FBBase已被移除，因为RAM不足以支持如此巨大的显存。直接通过SPI刷图。 */
void LCDDrvFlush(struct DisplayDevice *ptDev)
{
    if(NULL == ptDev->name)    return;
    // No-op because there is no framebuffer. All draws are immediate.
}

int LCDDrvSetPixel(struct DisplayDevice *ptDev, \
                   unsigned short wX, unsigned short wY, \
                   uint32_t color888)
{
    if(NULL == ptDev->name)    return -1;
    if (wX >= ptDev->wXres || wY >= ptDev->wYres)
        return -1;

    LCDDrvSetDisplayWindow(ptDev, wX, wY, wX, wY);
    LCDDrvWriteReg(0x2C); // Memory Write
    /* ILI9488 SPI: 18-bit mode, 3 bytes per pixel */
    uint8_t r, g, b;
    rgb888_to_rgb666(color888, &r, &g, &b);
    LCDDrvWriteDat(r);
    LCDDrvWriteDat(g);
    LCDDrvWriteDat(b);

    return 0;
}

/* ============================================================
 * Fast batched fill: set window ONCE, then stream pixel data.
 * This is ~10x faster than per-pixel SetPixel for rectangles.
 * ============================================================ */
void LCDDrvFillRect(unsigned short x, unsigned short y,
                    unsigned short w, unsigned short h,
                    uint32_t color888)
{
    DisplayDevice *ptDev = &gLcdDevice;
    if(NULL == ptDev->name)    return;
    if (x >= ptDev->wXres || y >= ptDev->wYres) return;
    if (x + w > ptDev->wXres) w = ptDev->wXres - x;
    if (y + h > ptDev->wYres) h = ptDev->wYres - y;

    LCDDrvSetDisplayWindow(ptDev, x, y, x + w - 1, y + h - 1);
    LCDDrvWriteReg(0x2C); // Memory Write command

    sci_spi0_wait_tx_end();
    LCDDrvWriteDCX(isData);

    /* ILI9488 SPI: 18-bit mode, 3 bytes per pixel */
    uint8_t r, g, b_byte;
    rgb888_to_rgb666(color888, &r, &g, &b_byte);
    uint32_t total = (uint32_t)w * (uint32_t)h;
    for (uint32_t i = 0; i < total; i++) {
        sci_spi0_write_byte(r);
        sci_spi0_write_byte(g);
        sci_spi0_write_byte(b_byte);
    }
    sci_spi0_wait_tx_end();
}
