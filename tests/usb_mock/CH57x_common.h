#ifndef USB_MOCK_CH57X_COMMON_H
#define USB_MOCK_CH57X_COMMON_H
/* Register model only: events emulate W1C after servicing. */
#include <stdint.h>
#define __INTERRUPT
#define __HIGH_CODE
static uint8_t mock_usb_ctrl, mock_intfg, mock_intst, mock_rxlen;
static uint8_t mock_ep0ctrl, mock_ep1ctrl, mock_ep0len, mock_ep1len;
static uint8_t mock_intenable, mock_devad, mock_ep2ctrl, mock_ep3ctrl, mock_ep4ctrl;
static uint8_t mock_ep2len, mock_ep3len, mock_ep4len;
static uint8_t mock_modes1, mock_modes2, mock_modes3, mock_devctrl, mock_mis;
static uint16_t mock_alternate, mock_dma0, mock_dma1, mock_dma2, mock_dma3;
static uint32_t mock_pa_dir, mock_pa_pu, mock_pa_pd;
static uint32_t mock_irq_enabled = 1;
#define R8_USB_CTRL mock_usb_ctrl
#define R8_USB_INT_FG mock_intfg
#define R8_USB_INT_ST mock_intst
#define R8_USB_RX_LEN mock_rxlen
#define R8_UEP0_CTRL mock_ep0ctrl
#define R8_UEP1_CTRL mock_ep1ctrl
#define R8_UEP2_CTRL mock_ep2ctrl
#define R8_UEP3_CTRL mock_ep3ctrl
#define R8_UEP4_CTRL mock_ep4ctrl
#define R8_UEP0_T_LEN mock_ep0len
#define R8_UEP1_T_LEN mock_ep1len
#define R8_UEP2_T_LEN mock_ep2len
#define R8_UEP3_T_LEN mock_ep3len
#define R8_UEP4_T_LEN mock_ep4len
#define R8_USB_DEV_AD mock_devad
#define R8_USB_INT_EN mock_intenable
#define R16_PIN_ALTERNATE mock_alternate
#define R8_UDEV_CTRL mock_devctrl
#define R8_USB_MIS_ST mock_mis
#define R8_UEP4_1_MOD mock_modes1
#define R8_UEP2_3_MOD mock_modes2
#define R8_UEP567_MOD mock_modes3
#define R16_UEP0_DMA mock_dma0
#define R16_UEP1_DMA mock_dma1
#define R16_UEP2_DMA mock_dma2
#define R16_UEP3_DMA mock_dma3
#define R32_PA_DIR mock_pa_dir
#define R32_PA_PU mock_pa_pu
#define R32_PA_PD_DRV mock_pa_pd
#define RB_UEP_R_TOG 0x80
#define RB_UEP_T_TOG 0x40
#define MASK_UEP_R_RES 0x0c
#define MASK_UEP_T_RES 3
#define UEP_R_RES_ACK 0
#define UEP_R_RES_NAK 8
#define UEP_R_RES_STALL 12
#define UEP_T_RES_ACK 0
#define UEP_T_RES_NAK 2
#define UEP_T_RES_STALL 3
#define RB_UDA_GP_BIT 0x80
#define MASK_USB_ADDR 0x7f
#define RB_UIF_BUS_RST 1
#define RB_UIF_TRANSFER 2
#define RB_UIF_SUSPEND 4
#define RB_UIF_FIFO_OV 16
#define RB_UIS_SETUP_ACT 0x80
#define RB_UIS_TOG_OK 0x40
#define MASK_UIS_ENDP 15
#define MASK_UIS_TOKEN 0x30
#define UIS_TOKEN_IN 0x20
#define UIS_TOKEN_OUT 0x00
#define UIS_TOKEN_SOF 0x10
/* 0x30 = idle, not SETUP. SETUP_ACT is independent of retained token/EP. */
#define UIS_TOKEN_SETUP 0x30
#define RB_UMS_SUSPEND 4
#define RB_UMS_SIE_FREE 0x20
#define RB_UDP_PU_EN 0x1000
#define RB_PIN_USB_EN 0x2000
#define RB_PIN_DEBUG_EN 0x4000
#define RB_UC_RESET_SIE 4
#define RB_UC_CLR_ALL 2
#define RB_UEP1_RX_EN 0x80
#define RB_UEP1_TX_EN 0x40
#define RB_UEP4_TX_EN 0x04
#define RB_UEP2_RX_EN 0x08
#define RB_UEP2_TX_EN 0x04
#define RB_UEP3_RX_EN 0x80
#define RB_UEP3_TX_EN 0x40
#define RB_UC_DEV_PU_EN 0x20
#define RB_UC_INT_BUSY 8
#define RB_UC_DMA_EN 1
#define RB_UD_PD_DIS 0x80
#define RB_UD_PORT_EN 1
#define RB_UIE_SUSPEND 4
#define RB_UIE_BUS_RST 1
#define RB_UIE_TRANSFER 2
#define USB_GET_DESCRIPTOR 6
#define USB_SET_ADDRESS 5
#define USB_SET_CONFIGURATION 9
#define USB_GET_CONFIGURATION 8
#define USB_GET_INTERFACE 10
#define USB_SET_INTERFACE 11
#define USB_GET_STATUS 0
#define USB_SET_FEATURE 3
#define USB_CLEAR_FEATURE 1
#define DEF_USB_SET_REPORT 9
#define DEF_USB_SET_IDLE 10
#define DEF_USB_GET_IDLE 2
#define USB_IRQn 0
static uint32_t mock_detach_delay_ms;
static uint8_t mock_delay_while_attached;
static void mDelaymS(uint16_t ms)
{
    mock_detach_delay_ms += ms;
    if ((R16_PIN_ALTERNATE & (RB_PIN_USB_EN | RB_UDP_PU_EN)) ||
        (R8_UDEV_CTRL & RB_UD_PORT_EN)) mock_delay_while_attached = 1;
}
static uint32_t PFIC_GetStatusIRQ(int x) { (void)x; return mock_irq_enabled; }
static void PFIC_DisableIRQ(int x) { (void)x; mock_irq_enabled = 0; }
static void PFIC_EnableIRQ(int x) { (void)x; mock_irq_enabled = 1; }
#endif
