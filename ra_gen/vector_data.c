/* generated vector source file - do not edit */
        #include "bsp_api.h"
        /* Do not build these data structures if no interrupts are currently allocated because IAR will have build errors. */
        #if VECTOR_DATA_IRQ_COUNT > 0
        BSP_DONT_REMOVE const fsp_vector_t g_vector_table[BSP_ICU_VECTOR_NUM_ENTRIES] BSP_PLACE_IN_SECTION(BSP_SECTION_APPLICATION_VECTORS) =
        {
                        [0] = ssi_txi_isr, /* SSI0 TXI (Transmit data empty) */
            [1] = ssi_rxi_isr, /* SSI0 RXI (Receive data full) */
            [2] = ssi_int_isr, /* SSI0 INT (Error interrupt) */
            [3] = sci_uart_rxi_isr, /* SCI7 RXI (Receive data full) */
            [4] = sci_uart_txi_isr, /* SCI7 TXI (Transmit data empty) */
            [5] = sci_uart_tei_isr, /* SCI7 TEI (Transmit end) */
            [6] = sci_uart_eri_isr, /* SCI7 ERI (Receive error) */
            [7] = sci_uart_rxi_isr, /* SCI6 RXI (Receive data full) */
            [8] = sci_uart_txi_isr, /* SCI6 TXI (Transmit data empty) */
            [9] = sci_uart_tei_isr, /* SCI6 TEI (Transmit end) */
            [10] = sci_uart_eri_isr, /* SCI6 ERI (Receive error) */
            [11] = sci_uart_rxi_isr, /* SCI5 RXI (Receive data full) */
            [12] = sci_uart_txi_isr, /* SCI5 TXI (Transmit data empty) */
            [13] = sci_uart_tei_isr, /* SCI5 TEI (Transmit end) */
            [14] = sci_uart_eri_isr, /* SCI5 ERI (Receive error) */
            [15] = key_irq6_isr, /* ICU IRQ6 (External pin interrupt 6) */
            [16] = sci_i2c_txi_isr, /* SCI3 TXI (Transmit data empty) */
            [17] = sci_i2c_tei_isr, /* SCI3 TEI (Transmit end) */
        };
        #if BSP_FEATURE_ICU_HAS_IELSR
        const bsp_interrupt_event_t g_interrupt_event_link_select[BSP_ICU_VECTOR_NUM_ENTRIES] =
        {
            [0] = BSP_PRV_VECT_ENUM(EVENT_SSI0_TXI,GROUP0), /* SSI0 TXI (Transmit data empty) */
            [1] = BSP_PRV_VECT_ENUM(EVENT_SSI0_RXI,GROUP1), /* SSI0 RXI (Receive data full) */
            [2] = BSP_PRV_VECT_ENUM(EVENT_SSI0_INT,GROUP2), /* SSI0 INT (Error interrupt) */
            [3] = BSP_PRV_VECT_ENUM(EVENT_SCI7_RXI,GROUP3), /* SCI7 RXI (Receive data full) */
            [4] = BSP_PRV_VECT_ENUM(EVENT_SCI7_TXI,GROUP4), /* SCI7 TXI (Transmit data empty) */
            [5] = BSP_PRV_VECT_ENUM(EVENT_SCI7_TEI,GROUP5), /* SCI7 TEI (Transmit end) */
            [6] = BSP_PRV_VECT_ENUM(EVENT_SCI7_ERI,GROUP6), /* SCI7 ERI (Receive error) */
            [7] = BSP_PRV_VECT_ENUM(EVENT_SCI6_RXI,GROUP7), /* SCI6 RXI (Receive data full) */
            [8] = BSP_PRV_VECT_ENUM(EVENT_SCI6_TXI,GROUP0), /* SCI6 TXI (Transmit data empty) */
            [9] = BSP_PRV_VECT_ENUM(EVENT_SCI6_TEI,GROUP1), /* SCI6 TEI (Transmit end) */
            [10] = BSP_PRV_VECT_ENUM(EVENT_SCI6_ERI,GROUP2), /* SCI6 ERI (Receive error) */
            [11] = BSP_PRV_VECT_ENUM(EVENT_SCI5_RXI,GROUP3), /* SCI5 RXI (Receive data full) */
            [12] = BSP_PRV_VECT_ENUM(EVENT_SCI5_TXI,GROUP4), /* SCI5 TXI (Transmit data empty) */
            [13] = BSP_PRV_VECT_ENUM(EVENT_SCI5_TEI,GROUP5), /* SCI5 TEI (Transmit end) */
            [14] = BSP_PRV_VECT_ENUM(EVENT_SCI5_ERI,GROUP6), /* SCI5 ERI (Receive error) */
            [15] = BSP_PRV_VECT_ENUM(EVENT_ICU_IRQ6,GROUP7), /* ICU IRQ6 (External pin interrupt 6) */
            [16] = BSP_PRV_VECT_ENUM(EVENT_SCI3_TXI,GROUP8), /* SCI3 TXI (Transmit data empty) */
            [17] = BSP_PRV_VECT_ENUM(EVENT_SCI3_TEI,GROUP9), /* SCI3 TEI (Transmit end) */
        };
        #endif
        #endif
