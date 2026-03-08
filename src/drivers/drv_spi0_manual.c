#include "hal_data.h"
#include "bsp_api.h"

// ==========================================
// Port Configuration (Manual)
// ==========================================
void drv_lcd_pin_init(void) {
    // SPI Pins for SCI0
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_00, ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SCI0_2_4_6_8)); // MISO/RXD
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_01, ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SCI0_2_4_6_8)); // MOSI/TXD
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_02, ((uint32_t) IOPORT_CFG_PERIPHERAL_PIN | (uint32_t) IOPORT_PERIPHERAL_SCI0_2_4_6_8)); // SCK
    
    // LCD Control Pins
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_03, ((uint32_t) IOPORT_CFG_DRIVE_HS_HIGH | (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)); // CS
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_04, ((uint32_t) IOPORT_CFG_DRIVE_HS_HIGH | (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)); // DCX
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_01_PIN_05, ((uint32_t) IOPORT_CFG_DRIVE_HS_HIGH | (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)); // RESET
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_06_PIN_08, ((uint32_t) IOPORT_CFG_DRIVE_HIGH | (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH)); // BLACKLIGHT

    // Touch pins are configured in FT6336_Init() to avoid coupling display bring-up with touch wiring.
}

// ==========================================
// SCI0 Simple SPI - Non-FIFO mode
// ==========================================
void sci_spi0_init_baremetal(void) {
    // 1. Ungate clock to SCI0 module
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_MSTP->MSTPCRB &= ~(1U << 31);
    volatile uint32_t flush = R_MSTP->MSTPCRB;
    (void) flush;
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MICROSECONDS);

    // 2. Disable TE/RE
    R_SCI0->SCR = 0x00;

    // 3. Clear error flags (read-then-write sequence per HW manual)
    volatile uint8_t ssr_val = R_SCI0->SSR;
    (void) ssr_val;
    R_SCI0->SSR = 0x84; // Write 1 to TDRE+TEND, 0 to error bits

    // 4. DO NOT touch FCR - keep non-FIFO mode so SSR polling works correctly

    // 5. Configure mode registers
    R_SCI0->SMR  = 0x80;  // CM=1 (clock synchronous), CKS=00
    R_SCI0->SCR  = 0x00;  // CKE=00 (internal clock, SCK output)
    R_SCI0->SCMR = 0xFA;  // CHR1+BCP2+SDIR(MSB first)+reserved
    R_SCI0->BRR  = 3;     // PCLKA/32
    R_SCI0->MDDR = 0xFF;
    R_SCI0->SEMR = 0x00;
    R_SCI0->SPMR = 0x00;  // SSE=0 (no slave-select/mode-fault), CKPOL=0, CKPH=0 → SPI Mode 0

    // 6. Reset auxiliary registers
    R_SCI0->SNFR  = 0x00;
    R_SCI0->SIMR1 = 0x00;
    R_SCI0->SIMR2 = 0x00;
    R_SCI0->SIMR3 = 0x00;
    R_SCI0->CDR   = 0x0000;
    R_SCI0->DCCR  = 0x40;
    R_SCI0->SPTR  = 0x03;

    // 7. Stabilize
    R_BSP_SoftwareDelay(100, BSP_DELAY_UNITS_MICROSECONDS);

    // 8. Enable TX+RX (RE=1 needed for clock generation in SCI sync mode)
    R_SCI0->SCR = 0x30;

    // 9. Drain any stale RX data and clear error flags
    {
        volatile uint8_t ssr = R_SCI0->SSR;
        if (ssr & 0x20) { // ORER
            R_SCI0->SSR = (uint8_t)((ssr & ~0x38) | 0x84);
        }
        volatile uint8_t d = R_SCI0->RDR; (void)d;
    }
}

// ==========================================
// SPI Transfer Functions
// At BRR=3, bit rate ~3MHz, one byte = ~2.7us.
// CRITICAL: In SCI clock-sync mode, every TX also produces an RX.
//           We MUST drain RDR to prevent ORER (overrun error).
// ==========================================

/* Drain SCI0 receive data register to prevent overrun error (ORER) */
static inline void spi_drain_rx(void) {
    volatile uint8_t ssr = R_SCI0->SSR;
    /* Clear overrun error if set */
    if (ssr & 0x20) { /* ORER */
        R_SCI0->SSR = (uint8_t)((ssr & ~0x38) | 0x84); /* clear ORER/FER/PER, keep TDRE/TEND */
    }
    /* Read RDR to clear RDRF (harmless if RDRF=0) */
    volatile uint8_t d = R_SCI0->RDR;
    (void)d;
}

void sci_spi0_write_byte(uint8_t dat) {
    /* Wait for TDRE (TX Data Register Empty) - generous timeout */
    for (volatile uint32_t t = 0; t < 100000; t++) {
        if (R_SCI0->SSR & 0x80) break;
    }

    /* Drain any pending RX data to prevent ORER on next transfer */
    spi_drain_rx();

    R_SCI0->TDR = dat;
}

void sci_spi0_wait_tx_end(void) {
    /* Wait for TEND (Transmit End) - generous timeout */
    for (volatile uint32_t t = 0; t < 100000; t++) {
        if (R_SCI0->SSR & 0x04) break;
    }
    /* Drain RX to keep RDRF clear */
    spi_drain_rx();
}

void sci_spi0_write_buffer(uint8_t* pbuf, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) {
        sci_spi0_write_byte(pbuf[i]);
    }
    sci_spi0_wait_tx_end();
}
