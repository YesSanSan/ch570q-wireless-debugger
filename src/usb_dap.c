/* CH570Q composite CMSIS-DAP v1 HID / v2 Bulk + CDC ACM.
 * Register sequencing follows the pinned WCH SDK. WCH MCU use only.
 * VID 1209 / PID 0001 remain TEST PLACEHOLDERS, not an allocated identity.
 * Owns USB_IRQHandler. Physical enumeration/timing validation is pending.
 */
#include "usb_dap.h"
#include "uart_log.h"
#include "CH57x_common.h"
#include <string.h>

#define REPORT_SIZE 64u
#define EP0_SIZE 64u
#define EP1_OUT 0x01u
#define EP1_IN 0x81u
#define EP2_OUT 0x02u
#define EP2_IN 0x82u
#define EP3_OUT 0x03u
#define EP3_IN 0x83u
#define EP4_IN 0x84u
#define MS_VENDOR_CODE 0x20u

#ifndef WD_USB_POLLING
#define WD_USB_POLLING 0
#endif

static const uint8_t device_descriptor[] = {
    18, 1, 0x00, 0x02, 0xef, 2, 1, EP0_SIZE,
    0x09, 0x12, 0x01, 0x00, 0x00, 0x02, 1, 2, 0, 1
};
static const uint8_t report_descriptor[] = {
    0x06, 0x00, 0xff,       /* vendor usage page */
    0x09, 0x01, 0xa1, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, REPORT_SIZE,
    0x09, 0x01, 0x81, 0x02, /* Input: data, variable, absolute */
    0x95, REPORT_SIZE,
    0x09, 0x02, 0x91, 0x02, /* Output, no Report ID or feature report */
    0xc0
};
static const uint8_t configuration_descriptor[] = {
    9, 2, 130, 0, 4, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 3, 0, 0, 3,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof(report_descriptor), 0,
    7, 5, EP1_IN, 3, REPORT_SIZE, 0, 1,
    7, 5, EP1_OUT, 3, REPORT_SIZE, 0, 1,
    /* CMSIS-DAP v2 interface: OUT must precede IN for host discovery. */
    9, 4, 1, 0, 2, 0xff, 0, 0, 4,
    7, 5, EP2_OUT, 2, REPORT_SIZE, 0, 0,
    7, 5, EP2_IN, 2, REPORT_SIZE, 0, 0,
    /* CDC IAD and ACM control interface 2, data interface 3. */
    8, 11, 2, 2, 2, 2, 1, 5,
    9, 4, 2, 0, 1, 2, 2, 1, 5,
    5, 0x24, 0, 0x10, 0x01,
    5, 0x24, 1, 0, 3,
    4, 0x24, 2, 6, /* line coding/control, break, serial-state notifications */
    5, 0x24, 6, 2, 3,
    7, 5, EP4_IN, 3, 16, 0, 16,
    9, 4, 3, 0, 2, 0x0a, 0, 0, 5,
    7, 5, EP3_OUT, 2, REPORT_SIZE, 0, 0,
    7, 5, EP3_IN, 2, REPORT_SIZE, 0, 0
};
static const uint8_t language_descriptor[] = {4, 3, 0x09, 0x04};
static const char *const strings[] = {
    "", "CH570Q Project", "CH570Q Wireless CMSIS-DAP", "CMSIS-DAP v1 HID",
    "CMSIS-DAP v2", "CH570Q CDC ACM"
};
/* Microsoft OS 1.0 descriptors: usbccgp binds only interface 1 to WinUSB.
 * OS 1.0 works without a BOS descriptor and leaves HID/CDC class drivers intact.
 * CMSIS-DAP's standard interface GUID supports Windows probe discovery. */
static const uint8_t ms_os_string[] = {
    18, 3, 'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0, MS_VENDOR_CODE, 0
};
static const uint8_t ms_compat_id[] = {
    40,0,0,0, 0,1, 4,0, 1,0,0,0,0,0,0,0,
    1,1, 'W','I','N','U','S','B',0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};
static const uint8_t ms_properties[] = {
    146,0,0,0, 0,1, 5,0, 1,0,
    136,0,0,0, 7,0,0,0, 42,0,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,'I',0,'n',0,'t',0,'e',0,'r',0,
    'f',0,'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,'s',0,0,0,
    80,0,0,0,
    '{',0,'C',0,'D',0,'B',0,'3',0,'B',0,'5',0,'A',0,'D',0,'-',0,
    '2',0,'9',0,'3',0,'B',0,'-',0,'4',0,'6',0,'6',0,'3',0,'-',0,
    'A',0,'A',0,'3',0,'6',0,'-',0,'1',0,'A',0,'A',0,'E',0,'4',0,'6',0,
    '4',0,'6',0,'3',0,'7',0,'7',0,'6',0,'}',0,0,0,0,0
};

/* CH572SFR.h: EP1/2/3 RX+TX, BUF_MOD=0 => OUT +0, IN +64.
 * EP4 TX shares UEP0_DMA+64; EP0 allocation MUST therefore be 128 B.
 * DMA: 512 B. DAP request + HID snapshot: 128 B. CDC reuses DMA packets,
 * NAKing OUT until read and IN until write; no heap or unbounded queues. */
static uint8_t ep0_buffer[EP0_SIZE * 2] __attribute__((aligned(4)));
static uint8_t ep1_buffer[REPORT_SIZE * 2] __attribute__((aligned(4)));
static uint8_t ep2_buffer[REPORT_SIZE * 2] __attribute__((aligned(4)));
static uint8_t ep3_buffer[REPORT_SIZE * 2] __attribute__((aligned(4)));
static uint8_t request_buffer[REPORT_SIZE];
static uint8_t last_report[REPORT_SIZE];
enum request_state { REQ_IDLE, REQ_CONTROL_RX, REQ_CONTROL_STATUS,
                     REQ_READY, REQ_PROCESSING, REQ_RESPONSE };
enum control_state { CTRL_IDLE, CTRL_IN, CTRL_OUT, CTRL_STATUS_IN,
                     CTRL_STATUS_OUT, CTRL_STALL };
enum action { ACT_NONE, ACT_ADDRESS, ACT_CONFIGURATION, ACT_INTERFACE,
              ACT_HALT_IN, ACT_HALT_OUT, ACT_CLEAR_IN, ACT_CLEAR_OUT,
              ACT_IDLE, ACT_REPORT, ACT_LINE_CODING, ACT_CONTROL_LINE, ACT_BREAK };

static volatile uint8_t configuration, suspended, request_state;
static volatile uint32_t generation;
static uint8_t request_length, halt_in, halt_out, idle_rate;
static uint8_t control_state, control_action, action_value;
static uint8_t control_out_length, control_zlp;
static uint16_t control_remaining;
static const uint8_t *control_data;
static usb_dap_transport_t transport;
static uint8_t cdc_rx_length, cdc_rx_offset, cdc_tx_busy, cdc_tx_length;
static uint8_t cdc_control, notify_busy;
static uint16_t cdc_break, serial_state, notified_state;
static uint8_t line_coding[7], pending_line_coding[7];
static uint16_t le16(const uint8_t *p);
static bool bus_seen;
static void select_transport(usb_dap_transport_t next);
static void usb_service(void);

/* APIs only exclude this interrupt; do not change unrelated interrupt enables.
 * RB_UC_INT_BUSY holds off the next USB transaction while UIF_TRANSFER is set.
 */
static uint32_t usb_lock(void)
{
    uint32_t enabled = PFIC_GetStatusIRQ(USB_IRQn);
    PFIC_DisableIRQ(USB_IRQn);
    __asm volatile("" ::: "memory");
    return enabled;
}
static void usb_unlock(uint32_t enabled)
{
    __asm volatile("" ::: "memory");
    if (enabled) PFIC_EnableIRQ(USB_IRQn);
}

static volatile uint8_t *ep_ctrl(uint8_t ep)
{
    switch (ep) {
    case 1: return &R8_UEP1_CTRL;
    case 2: return &R8_UEP2_CTRL;
    case 3: return &R8_UEP3_CTRL;
    default: return &R8_UEP4_CTRL;
    }
}
static bool halted(uint8_t ep, bool in)
{
    return ((in ? halt_in : halt_out) & (1u << ep)) != 0;
}
static void notify_update(void)
{
    if (configuration && !notify_busy && serial_state != notified_state) {
        uint8_t *p = ep0_buffer + EP0_SIZE;
        p[0] = 0xa1; p[1] = 0x20; p[2] = p[3] = 0;
        p[4] = 2; p[5] = 0; p[6] = 2; p[7] = 0;
        p[8] = (uint8_t)serial_state; p[9] = (uint8_t)(serial_state >> 8);
        notified_state = serial_state;
        notify_busy = 1;
        R8_UEP4_T_LEN = 10;
    }
    R8_UEP4_CTRL = (R8_UEP4_CTRL & RB_UEP_T_TOG) | UEP_R_RES_NAK |
        (halted(4, true) ? UEP_T_RES_STALL :
         (configuration && notify_busy ? UEP_T_RES_ACK : UEP_T_RES_NAK));
}
/* Manual toggles: change exactly once per successful transaction. */
static void ep1_update(void)
{
    uint8_t ep;
    for (ep = 1; ep <= 2; ++ep) {
        bool selected = (ep == (uint8_t)transport);
        uint8_t ctrl = *ep_ctrl(ep) & (RB_UEP_R_TOG | RB_UEP_T_TOG);
        ctrl |= halted(ep, false) ? UEP_R_RES_STALL :
                (configuration && request_state == REQ_IDLE ? UEP_R_RES_ACK : UEP_R_RES_NAK);
        ctrl |= halted(ep, true) ? UEP_T_RES_STALL :
                (configuration && selected && request_state == REQ_RESPONSE ? UEP_T_RES_ACK : UEP_T_RES_NAK);
        *ep_ctrl(ep) = ctrl;
    }
}
static void cdc_update(void)
{
    R8_UEP3_CTRL = (R8_UEP3_CTRL & (RB_UEP_R_TOG | RB_UEP_T_TOG)) |
        (halted(3, false) ? UEP_R_RES_STALL :
         (configuration && !cdc_rx_length ? UEP_R_RES_ACK : UEP_R_RES_NAK)) |
        (halted(3, true) ? UEP_T_RES_STALL :
         (configuration && cdc_tx_busy ? UEP_T_RES_ACK : UEP_T_RES_NAK));
    notify_update();
}
static void dap_reset(void)
{
    request_state = REQ_IDLE;
    request_length = 0;
    transport = USB_DAP_TRANSPORT_NONE;
    idle_rate = 0;
    memset(last_report, 0, sizeof(last_report));
    R8_UEP1_T_LEN = R8_UEP2_T_LEN = 0;
    ++generation;
    ep1_update();
}
/* SET_INTERFACE resets only the endpoint state of the selected interface. The
 * previous single cdc_reset() let SET_INTERFACE(2) on the ACM control interface
 * discard data interface 3's partially received and queued bytes together with
 * the line coding and DTR, killing an already open CDC session. */
static void cdc_control_reset(void)
{
    notify_busy = 0;
    notified_state = 0xffff; /* send SERIAL_STATE again for the new setting */
    R8_UEP4_T_LEN = 0;
}
static void cdc_data_reset(void)
{
    cdc_rx_length = cdc_rx_offset = cdc_tx_busy = cdc_tx_length = 0;
    R8_UEP3_T_LEN = 0;
}
static void cdc_reset(void)
{
    cdc_data_reset();
    cdc_control_reset();
    cdc_control = 0;
    cdc_break = serial_state = 0;
    memset(line_coding, 0, sizeof(line_coding));
    line_coding[0] = 0x00; line_coding[1] = 0xc2; line_coding[2] = 1; /* 115200 */
    line_coding[6] = 8;
    cdc_update();
}
static void new_session(uint8_t config)
{
    configuration = config;
    halt_in = halt_out = 0;
    R8_UEP1_CTRL = R8_UEP2_CTRL = R8_UEP3_CTRL = R8_UEP4_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK;
    dap_reset();
    cdc_reset();
}
static void ep0_idle(void)
{
    control_state = CTRL_IDLE;
    control_action = ACT_NONE;
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
}
static void ep0_stall(void)
{
    uart_log_event(UART_LOG_USB_STALL, control_state, control_action);
    if (request_state == REQ_CONTROL_RX || request_state == REQ_CONTROL_STATUS) {
        request_state = REQ_IDLE;
        ep1_update();
    }
    control_state = CTRL_STALL;
    control_action = ACT_NONE;
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
}
static void ep0_packet(void)
{
    uint8_t n = control_remaining > EP0_SIZE ? EP0_SIZE : (uint8_t)control_remaining;
    if (n && control_data != ep0_buffer) memcpy(ep0_buffer, control_data, n);
    control_data += n;
    control_remaining -= n;
    if (!n) control_zlp = 0;
    R8_UEP0_T_LEN = n;
    R8_UEP0_CTRL = (R8_UEP0_CTRL & (RB_UEP_R_TOG | RB_UEP_T_TOG)) |
                   UEP_R_RES_ACK | UEP_T_RES_ACK;
}
static void ep0_read(const uint8_t *data, uint16_t size, uint16_t requested)
{
    control_data = data;
    control_remaining = size < requested ? size : requested;
    control_zlp = (size < requested && !(size % EP0_SIZE));
    control_state = requested ? CTRL_IN : CTRL_STATUS_OUT;
    if (requested) ep0_packet();
    else R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
}
static void ep0_status(uint8_t action, uint8_t value)
{
    control_action = action;
    action_value = value;
    control_state = CTRL_STATUS_IN;
    R8_UEP0_T_LEN = 0;
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_NAK | UEP_T_RES_ACK;
}
static void ep0_complete(void)
{
    switch (control_action) {
    case ACT_ADDRESS:
        R8_USB_DEV_AD = (R8_USB_DEV_AD & RB_UDA_GP_BIT) | action_value;
        uart_log_event(UART_LOG_USB_ADDRESS, action_value, 0);
        break;
    case ACT_CONFIGURATION:
        new_session(action_value);
        uart_log_event(UART_LOG_USB_CONFIG, action_value, generation);
        break;
    case ACT_INTERFACE:
        if (action_value < 2) {
            halt_in &= (uint8_t)~(1u << (action_value + 1));
            halt_out &= (uint8_t)~(1u << (action_value + 1));
            *ep_ctrl(action_value + 1) = UEP_R_RES_NAK | UEP_T_RES_NAK;
            dap_reset();
        } else {
            if (action_value == 2) {
                halt_in &= (uint8_t)~(1u << 4);
                R8_UEP4_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK;
                cdc_control_reset();
            } else {
                halt_in &= (uint8_t)~(1u << 3); halt_out &= (uint8_t)~(1u << 3);
                R8_UEP3_CTRL = UEP_R_RES_NAK | UEP_T_RES_NAK;
                cdc_data_reset();
            }
            cdc_update();
        }
        break;
    case ACT_HALT_IN: halt_in |= 1u << action_value; goto update_endpoints;
    case ACT_HALT_OUT: halt_out |= 1u << action_value; goto update_endpoints;
    case ACT_CLEAR_IN:
        halt_in &= (uint8_t)~(1u << action_value);
        *ep_ctrl(action_value) &= (uint8_t)~RB_UEP_T_TOG;
        goto update_endpoints;
    case ACT_CLEAR_OUT:
        halt_out &= (uint8_t)~(1u << action_value);
        *ep_ctrl(action_value) &= (uint8_t)~RB_UEP_R_TOG;
    update_endpoints:
        ep1_update(); cdc_update();
        break;
    case ACT_IDLE: idle_rate = action_value; break;
    case ACT_REPORT:
        if (request_state == REQ_CONTROL_STATUS) {
            select_transport(USB_DAP_TRANSPORT_HID);
            request_state = REQ_READY;
        }
        break;
    case ACT_LINE_CODING: memcpy(line_coding, pending_line_coding, 7); break;
    case ACT_CONTROL_LINE: cdc_control = action_value; break;
    case ACT_BREAK: cdc_break = le16(pending_line_coding); break;
    default: break;
    }
    ep0_idle();
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static bool endpoint_valid(uint16_t index)
{
    return index == 0 || index == 0x80 ||
           (configuration && (index == EP1_OUT || index == EP1_IN ||
            index == EP2_OUT || index == EP2_IN || index == EP3_OUT ||
            index == EP3_IN || index == EP4_IN));
}
static void ep0_setup(void)
{
    /* Copy SETUP fields before any descriptor/response overwrites shared DMA. */
    uint8_t type = ep0_buffer[0], req = ep0_buffer[1];
    uint16_t value = le16(ep0_buffer + 2), index = le16(ep0_buffer + 4);
    uint16_t length = le16(ep0_buffer + 6);
    uart_log_event(UART_LOG_USB_SETUP, (uint32_t)type | ((uint32_t)req << 8) | ((uint32_t)value << 16),
                   (uint32_t)index | ((uint32_t)length << 16));
    const uint8_t *descriptor = 0;
    uint16_t size = 0;

    /* A new SETUP aborts the preceding control transfer, including a partially
     * received SET_REPORT. Never publish that report before its status ACK.
     */
    if (request_state == REQ_CONTROL_RX || request_state == REQ_CONTROL_STATUS) {
        request_state = REQ_IDLE;
        ep1_update();
    }
    control_action = ACT_NONE;
    control_remaining = 0;
    control_zlp = 0;
    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;

    bus_seen = true;
    if (type == 0xc0 && req == MS_VENDOR_CODE && !value && index == 4) {
        ep0_read(ms_compat_id, sizeof(ms_compat_id), length); return;
    } else if (type == 0xc1 && req == MS_VENDOR_CODE && value == 1 && index == 5) {
        ep0_read(ms_properties, sizeof(ms_properties), length); return;
    } else if (configuration && index == 2 && type == 0xa1 && req == 0x21 && !value && length == 7) {
        ep0_read(line_coding, 7, length); return;
    } else if (configuration && index == 2 && type == 0x21 && req == 0x20 && !value && length == 7) {
        control_action = ACT_LINE_CODING;
        control_state = CTRL_OUT;
        control_out_length = 7;
        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        return;
    } else if (configuration && index == 2 && type == 0x21 && req == 0x22 && value <= 3 && !length) {
        ep0_status(ACT_CONTROL_LINE, (uint8_t)value); return;
    } else if (configuration && index == 2 && type == 0x21 && req == 0x23 && !length) {
        pending_line_coding[0] = (uint8_t)value; pending_line_coding[1] = value >> 8;
        ep0_status(ACT_BREAK, 0); return;
    } else if (req == USB_GET_DESCRIPTOR && (type == 0x80 || type == 0x81)) {
        uint8_t kind = value >> 8, number = (uint8_t)value;
        if (kind == 1 && type == 0x80 && !number && !index) {
            descriptor = device_descriptor; size = sizeof(device_descriptor);
        } else if (kind == 2 && type == 0x80 && !number && !index) {
            descriptor = configuration_descriptor; size = sizeof(configuration_descriptor);
        } else if (kind == 3 && type == 0x80) {
            if (!number && !index) {
                descriptor = language_descriptor; size = sizeof(language_descriptor);
            } else if (number == 0xee && !index) {
                descriptor = ms_os_string; size = sizeof(ms_os_string);
            } else if (number > 0 && number < sizeof(strings) / sizeof(strings[0]) &&
                       (index == 0x0409 || !index)) {
                const char *text = strings[number];
                uint8_t n = (uint8_t)strlen(text), i;
                if (n > 31) n = 31; /* one EP0 packet, bounded UTF-16LE */
                ep0_buffer[0] = 2 + 2 * n; ep0_buffer[1] = 3;
                for (i = 0; i < n; ++i) {
                    ep0_buffer[2 + i * 2] = (uint8_t)text[i];
                    ep0_buffer[3 + i * 2] = 0;
                }
                descriptor = ep0_buffer; size = 2 + 2 * n;
            }
        } else if (kind == 0x21 && type == 0x81 && !number && !index) {
            descriptor = configuration_descriptor + 18; size = 9;
        } else if (kind == 0x22 && type == 0x81 && !number && !index) {
            descriptor = report_descriptor; size = sizeof(report_descriptor);
        }
        if (descriptor) { ep0_read(descriptor, size, length); return; }
    } else if (type == 0 && req == USB_SET_ADDRESS && !index && !length &&
               value <= 127 && !configuration) {
        ep0_status(ACT_ADDRESS, (uint8_t)value); return;
    } else if (type == 0 && req == USB_SET_CONFIGURATION && !index && !length && value <= 1 &&
               (R8_USB_DEV_AD & MASK_USB_ADDR)) {
        ep0_status(ACT_CONFIGURATION, (uint8_t)value); return;
    } else if (type == 0x80 && req == USB_GET_CONFIGURATION && !value && !index && length == 1) {
        ep0_buffer[0] = configuration; ep0_read(ep0_buffer, 1, length); return;
    } else if (type == 0x81 && req == USB_GET_INTERFACE && !value && index < 4 && length == 1 && configuration) {
        ep0_buffer[0] = 0; ep0_read(ep0_buffer, 1, length); return;
    } else if (type == 0x01 && req == USB_SET_INTERFACE && !value && index < 4 && !length && configuration) {
        ep0_status(ACT_INTERFACE, (uint8_t)index); return;
    } else if (req == USB_GET_STATUS && !value && length == 2 &&
               ((type == 0x80 && !index) || (type == 0x81 && index < 4 && configuration) ||
                (type == 0x82 && endpoint_valid(index)))) {
        ep0_buffer[0] = type == 0x82 && (index & 0x0f) ? halted(index & 0x0f, (index & 0x80) != 0) : 0;
        ep0_buffer[1] = 0; ep0_read(ep0_buffer, 2, length); return;
    } else if (type == 0x02 && (req == USB_SET_FEATURE || req == USB_CLEAR_FEATURE) &&
               !value && !length && configuration && endpoint_valid(index) && (index & 0x0f)) {
        uint8_t action = req == USB_SET_FEATURE ?
                         (index & 0x80 ? ACT_HALT_IN : ACT_HALT_OUT) :
                         (index & 0x80 ? ACT_CLEAR_IN : ACT_CLEAR_OUT);
        ep0_status(action, index & 0x0f); return;
    } else if (!index && configuration && type == 0xa1 && req == 0x01 && value == 0x0100) {
        /* GET_REPORT(Input, ID 0) is a non-consuming snapshot. Interrupt IN is
         * the response queue; this control read never acknowledges it for RF.
         */
        ep0_read(last_report, sizeof(last_report), length); return;
    } else if (!index && configuration && type == 0x21 && req == DEF_USB_SET_REPORT &&
               value == 0x0200 && length > 0 && length <= REPORT_SIZE) {
        /* Accept Output report over control, as allowed by HID. Never overwrite
         * the interrupt OUT request. Busy SET_REPORT is deliberately STALLed;
         * the normal EP1 OUT path provides retryable NAK backpressure.
         */
        if (request_state == REQ_IDLE && !halted(1, false)) {
            request_state = REQ_CONTROL_RX;
            control_state = CTRL_OUT;
            control_out_length = (uint8_t)length;
            ep1_update();
            R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
            return;
        }
    } else if (!index && configuration && type == 0x21 && req == DEF_USB_SET_IDLE &&
               !(value & 0xff) && !length) {
        /* Store idle rate for HID compatibility. CMSIS-DAP reports are command
         * responses, never unsolicited periodic duplicates of a response.
         */
        ep0_status(ACT_IDLE, value >> 8); return;
    } else if (!index && configuration && type == 0xa1 && req == DEF_USB_GET_IDLE &&
               !value && length == 1) {
        ep0_buffer[0] = idle_rate; ep0_read(ep0_buffer, 1, length); return;
    }
    /* No boot protocol, remote wake, feature reports, nonzero alternate
     * settings, or device qualifier. Unsupported/malformed requests STALL. */
    ep0_stall();
}

static void ep0_in(void)
{
    if (control_state == CTRL_STATUS_IN) {
        ep0_complete();
    } else if (control_state == CTRL_IN) {
        R8_UEP0_CTRL ^= RB_UEP_T_TOG;
        if (control_remaining || control_zlp) ep0_packet();
        else {
            control_state = CTRL_STATUS_OUT;
            R8_UEP0_T_LEN = 0;
            R8_UEP0_CTRL = (R8_UEP0_CTRL & RB_UEP_T_TOG) | RB_UEP_R_TOG |
                           UEP_R_RES_ACK | UEP_T_RES_NAK;
        }
    } else {
        R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
    }
}
static void select_transport(usb_dap_transport_t next)
{
    if (transport != next) {
        transport = next;
        ++generation;
        memset(last_report, 0, sizeof(last_report));
        R8_UEP1_T_LEN = R8_UEP2_T_LEN = 0;
    }
}
static void accept_request(const uint8_t *data, uint8_t n, usb_dap_transport_t next)
{
    select_transport(next);
    memcpy(request_buffer, data, n);
    if (n < REPORT_SIZE) memset(request_buffer + n, 0, REPORT_SIZE - n);
    request_length = n;
    request_state = REQ_READY;
    ep1_update();
}
static void ep0_out(uint8_t length)
{
    if (control_state == CTRL_OUT && control_action == ACT_LINE_CODING && length == 7) {
        /* USB CDC fields, not native struct packing. Commit only after status. */
        if (ep0_buffer[4] > 2 || ep0_buffer[5] > 4 ||
            !(ep0_buffer[6] == 5 || ep0_buffer[6] == 6 || ep0_buffer[6] == 7 || ep0_buffer[6] == 8 || ep0_buffer[6] == 16)) {
            ep0_stall(); return;
        }
        memcpy(pending_line_coding, ep0_buffer, 7);
        ep0_status(ACT_LINE_CODING, 0);
    } else if (control_state == CTRL_OUT && request_state == REQ_CONTROL_RX && length == control_out_length) {
        /* Delay the transport switch/generation until the control status ACK. */
        memcpy(request_buffer, ep0_buffer, length);
        if (length < REPORT_SIZE) memset(request_buffer + length, 0, REPORT_SIZE - length);
        request_length = length;
        request_state = REQ_CONTROL_STATUS;
        ep0_status(ACT_REPORT, 0);
    } else if ((control_state == CTRL_STATUS_OUT || control_state == CTRL_IN) && !length) {
        ep0_idle();
    } else ep0_stall();
}
static void usb_service(void)
{
    uint8_t flags = R8_USB_INT_FG;
    if (flags & RB_UIF_BUS_RST) {
        bus_seen = true;
        uart_log_event(UART_LOG_USB_RESET, flags, R8_USB_INT_ST);
        R8_USB_DEV_AD = 0;
        suspended = 0;
        new_session(0);
        ep0_idle();
        R8_USB_INT_FG = flags & (RB_UIF_BUS_RST | RB_UIF_TRANSFER | RB_UIF_SUSPEND | RB_UIF_FIFO_OV);
        return;
    }
    if (flags & RB_UIF_TRANSFER) {
        uint8_t status = R8_USB_INT_ST;
        uint8_t endpoint = status & MASK_UIS_ENDP;
        uint8_t token = status & MASK_UIS_TOKEN;
        uint8_t have_setup = status & RB_UIS_SETUP_ACT;
        uint8_t setup_copy[8];
        if (have_setup) memcpy(setup_copy, ep0_buffer, sizeof(setup_copy));
        /* Datasheet: SETUP leaves token/endpoint/toggle/RX_LEN unchanged.
         * A non-idle token is a pending former transaction: finish it first,
         * clear TRANSFER, then process SETUP and clear TRANSFER again. */
        if (token != MASK_UIS_TOKEN) {
            if (!endpoint) {
                if (token == UIS_TOKEN_IN) {
                    uart_log_event(UART_LOG_USB_IN0, control_state, R8_UEP0_T_LEN);
                    ep0_in();
                } else if (token == UIS_TOKEN_OUT && (status & RB_UIS_TOG_OK)) {
                    uint8_t n = R8_USB_RX_LEN;
                    uart_log_event(UART_LOG_USB_OUT0, control_state, n);
                    /* New SETUP has overwritten EP0 DMA; an uncommitted OUT
                     * data stage is aborted. A zero-length status is safe. */
                    if (!have_setup || !n) ep0_out(n);
                }
            } else if (endpoint == 1 || endpoint == 2) {
                uint8_t *buffer = endpoint == 1 ? ep1_buffer : ep2_buffer;
                if (token == UIS_TOKEN_OUT && (status & RB_UIS_TOG_OK) &&
                    !halted(endpoint, false) && (*ep_ctrl(endpoint) & MASK_UEP_R_RES) == UEP_R_RES_ACK) {
                    uint8_t n = R8_USB_RX_LEN;
                    *ep_ctrl(endpoint) ^= RB_UEP_R_TOG;
                    if (configuration && request_state == REQ_IDLE && n && n <= REPORT_SIZE)
                        accept_request(buffer, n, (usb_dap_transport_t)endpoint);
                    else ep1_update();
                } else if (token == UIS_TOKEN_IN && (*ep_ctrl(endpoint) & MASK_UEP_T_RES) == UEP_T_RES_ACK) {
                    *ep_ctrl(endpoint) ^= RB_UEP_T_TOG;
                    if (request_state == REQ_RESPONSE && endpoint == (uint8_t)transport) request_state = REQ_IDLE;
                    if (endpoint == 1) R8_UEP1_T_LEN = 0; else R8_UEP2_T_LEN = 0;
                    ep1_update();
                }
            } else if (endpoint == 3) {
                if (token == UIS_TOKEN_OUT && (status & RB_UIS_TOG_OK) &&
                    (R8_UEP3_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK && configuration) {
                    uint8_t n = R8_USB_RX_LEN;
                    R8_UEP3_CTRL ^= RB_UEP_R_TOG;
                    if (n <= REPORT_SIZE && !cdc_rx_length) { cdc_rx_length = n; cdc_rx_offset = 0; }
                } else if (token == UIS_TOKEN_IN && (R8_UEP3_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) {
                    R8_UEP3_CTRL ^= RB_UEP_T_TOG;
                    if (cdc_tx_busy == 1 && cdc_tx_length == REPORT_SIZE) cdc_tx_busy = 2;
                    else cdc_tx_busy = 0;
                    R8_UEP3_T_LEN = 0;
                }
                cdc_update();
            } else if (endpoint == 4 && token == UIS_TOKEN_IN &&
                       (R8_UEP4_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK) {
                R8_UEP4_CTRL ^= RB_UEP_T_TOG;
                notify_busy = 0;
                R8_UEP4_T_LEN = 0;
                notify_update();
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
        if (have_setup) {
            memcpy(ep0_buffer, setup_copy, sizeof(setup_copy));
            /* SETUP_ACT guarantees all eight setup bytes; never test RX_LEN. */
            ep0_setup();
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    }
    if (flags & RB_UIF_SUSPEND) {
        suspended = !!(R8_USB_MIS_ST & RB_UMS_SUSPEND);
        uart_log_event(UART_LOG_USB_SUSPEND, suspended, flags);
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
    if (flags & RB_UIF_FIFO_OV) R8_USB_INT_FG = RB_UIF_FIFO_OV;
}

__INTERRUPT
__HIGH_CODE
void USB_IRQHandler(void)
{
    usb_service();
}

void usb_dap_init(void)
{
    PFIC_DisableIRQ(USB_IRQn);
    R8_USB_INT_EN = 0;
    R16_PIN_ALTERNATE &= ~(RB_UDP_PU_EN | RB_PIN_USB_EN);
    R8_UDEV_CTRL = 0;
    /* The ISP and application expose different USB personalities. Hold an
     * explicit detach interval before enabling this device's pull-up again.
     * This is a reconnect precaution, not proof of the enumeration failure. */
    mDelaymS(20);
    R8_USB_CTRL = RB_UC_RESET_SIE | RB_UC_CLR_ALL;
    R8_USB_CTRL = 0;
    /* Explicit host USB ownership: PA0/1 cannot remain the chip's debug port. */
    R16_PIN_ALTERNATE &= ~RB_PIN_DEBUG_EN;
    R8_UEP4_1_MOD = RB_UEP1_RX_EN | RB_UEP1_TX_EN | RB_UEP4_TX_EN;
    R8_UEP2_3_MOD = RB_UEP2_RX_EN | RB_UEP2_TX_EN | RB_UEP3_RX_EN | RB_UEP3_TX_EN;
    R8_UEP567_MOD = 0;
    R16_UEP0_DMA = (uint16_t)(uint32_t)ep0_buffer;
    R16_UEP1_DMA = (uint16_t)(uint32_t)ep1_buffer;
    R16_UEP2_DMA = (uint16_t)(uint32_t)ep2_buffer;
    R16_UEP3_DMA = (uint16_t)(uint32_t)ep3_buffer;
    suspended = 0;
    bus_seen = false;
    new_session(0);
    ep0_idle();
    R8_USB_DEV_AD = 0;
    R8_USB_INT_FG = 0xff;
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
    R16_PIN_ALTERNATE |= RB_PIN_USB_EN | RB_UDP_PU_EN;
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;
    uart_log_event(UART_LOG_USB_INIT, R16_UEP0_DMA, (uint32_t)R16_PIN_ALTERNATE << 16 | R8_USB_CTRL);
#if !WD_USB_POLLING
    PFIC_EnableIRQ(USB_IRQn);
#endif
}
void usb_dap_poll(void)
{
    uint32_t irq = usb_lock();
    /* Also service pending flags synchronously. This closes the main-loop
     * reset race before RF queue operations and is useful when IRQ was masked.
     */
    usb_service();
    usb_unlock(irq);
}
bool usb_dap_take_request(uint8_t out[64], uint8_t *length)
{
    uint32_t ignored_generation;
    return usb_dap_take_request_tagged(out, length, &ignored_generation);
}
bool usb_dap_take_request_tagged(uint8_t out[64], uint8_t *length, uint32_t *tag)
{
    usb_dap_transport_t ignored_transport;
    return usb_dap_take_request_routed(out, length, tag, &ignored_transport);
}
bool usb_dap_take_request_routed(uint8_t out[64], uint8_t *length, uint32_t *tag, usb_dap_transport_t *route)
{
    uint32_t irq;
    bool ready;
    if (!out || !length || !tag || !route) return false;
    irq = usb_lock(); usb_service();
    ready = configuration && !suspended && request_state == REQ_READY;
    if (ready) {
        memcpy(out, request_buffer, REPORT_SIZE);
        *length = request_length; *tag = generation; *route = transport;
        request_state = REQ_PROCESSING;
    }
    usb_unlock(irq);
    return ready;
}
static bool send_response(const uint8_t *data, uint8_t length, bool tagged, uint32_t tag)
{
    uint32_t irq;
    bool ready;
    if (length > REPORT_SIZE || (!data && length)) return false;
    irq = usb_lock(); usb_service();
    ready = configuration && !suspended && !halted((uint8_t)transport, true) &&
            request_state == REQ_PROCESSING && (!tagged || tag == generation) &&
            (length || transport == USB_DAP_TRANSPORT_HID);
    if (ready) {
        uint8_t *p = (transport == USB_DAP_TRANSPORT_HID ? ep1_buffer : ep2_buffer) + REPORT_SIZE;
        if (length) memcpy(p, data, length);
        if (transport == USB_DAP_TRANSPORT_HID) {
            if (length < REPORT_SIZE) memset(p + length, 0, REPORT_SIZE - length);
            memcpy(last_report, p, REPORT_SIZE);
            R8_UEP1_T_LEN = REPORT_SIZE;
        } else R8_UEP2_T_LEN = length;
        request_state = REQ_RESPONSE;
        ep1_update();
    }
    usb_unlock(irq);
    return ready;
}
bool usb_dap_send_response(const uint8_t *data, uint8_t length)
{
    return send_response(data, length, false, 0);
}
bool usb_dap_send_response_tagged(const uint8_t *data, uint8_t length, uint32_t tag)
{
    return send_response(data, length, true, tag);
}
static bool discard_request(bool tagged, uint32_t tag)
{
    uint32_t irq = usb_lock();
    bool ready;
    usb_service();
    ready = request_state == REQ_PROCESSING && (!tagged || tag == generation);
    if (ready) { request_state = REQ_IDLE; ep1_update(); }
    usb_unlock(irq);
    return ready;
}
void usb_dap_discard_request(void) { (void)discard_request(false, 0); }
bool usb_dap_discard_request_tagged(uint32_t tag) { return discard_request(true, tag); }
bool usb_dap_configured(void)
{
    uint32_t irq = usb_lock(); bool result;
    usb_service(); result = configuration != 0; usb_unlock(irq); return result;
}
uint32_t usb_dap_generation(void)
{
    uint32_t irq = usb_lock(), result;
    usb_service(); result = generation; usb_unlock(irq); return result;
}
usb_dap_transport_t usb_dap_transport(void)
{
    uint32_t irq = usb_lock(); usb_dap_transport_t result;
    usb_service(); result = transport; usb_unlock(irq); return result;
}
uint16_t usb_cdc_read(uint8_t *data, uint16_t capacity)
{
    uint32_t irq; uint16_t n = 0;
    if (!data || !capacity) return 0;
    irq = usb_lock(); usb_service();
    if (configuration && !suspended && !halted(3, false)) {
        n = cdc_rx_length - cdc_rx_offset;
        if (n > capacity) n = capacity;
        if (n) memcpy(data, ep3_buffer + cdc_rx_offset, n);
        cdc_rx_offset += n;
        if (cdc_rx_offset == cdc_rx_length) cdc_rx_offset = cdc_rx_length = 0;
        cdc_update();
    }
    usb_unlock(irq); return n;
}
uint16_t usb_cdc_write(const uint8_t *data, uint16_t length)
{
    uint32_t irq; uint16_t n = 0;
    if (!data || !length) return 0;
    irq = usb_lock(); usb_service();
    if (configuration && !suspended && !halted(3, true) && !cdc_tx_busy) {
        n = length > REPORT_SIZE ? REPORT_SIZE : length;
        memcpy(ep3_buffer + REPORT_SIZE, data, n);
        cdc_tx_busy = 1; cdc_tx_length = (uint8_t)n; R8_UEP3_T_LEN = (uint8_t)n;
        cdc_update();
    }
    usb_unlock(irq); return n;
}
void usb_cdc_get_line_coding(usb_cdc_line_coding_t *coding)
{
    uint32_t irq;
    if (!coding) return;
    irq = usb_lock(); usb_service();
    coding->baud_rate = (uint32_t)line_coding[0] | ((uint32_t)line_coding[1] << 8) |
        ((uint32_t)line_coding[2] << 16) | ((uint32_t)line_coding[3] << 24);
    coding->stop_bits = line_coding[4]; coding->parity = line_coding[5]; coding->data_bits = line_coding[6];
    usb_unlock(irq);
}
uint8_t usb_cdc_control_line_state(void)
{
    uint32_t irq = usb_lock(); uint8_t result;
    usb_service(); result = cdc_control; usb_unlock(irq); return result;
}
uint16_t usb_cdc_break_duration(void)
{
    uint32_t irq = usb_lock(); uint16_t result;
    usb_service(); result = cdc_break; usb_unlock(irq); return result;
}
void usb_cdc_set_serial_state(uint16_t state)
{
    uint32_t irq = usb_lock(); usb_service();
    serial_state = state & 0x7f; notify_update(); usb_unlock(irq);
}
bool usb_dap_bus_seen(void)
{
    uint32_t irq = usb_lock(); bool result;
    usb_service(); result = bus_seen; usb_unlock(irq); return result;
}
void usb_dap_deinit(void)
{
    PFIC_DisableIRQ(USB_IRQn);
    R8_USB_INT_EN = 0;
    R16_PIN_ALTERNATE &= (uint16_t)~(RB_UDP_PU_EN | RB_PIN_USB_EN | RB_PIN_DEBUG_EN);
    R8_UDEV_CTRL = 0;
    R8_USB_CTRL = RB_UC_RESET_SIE | RB_UC_CLR_ALL;
    R8_USB_CTRL = 0;
    R8_UEP4_1_MOD = R8_UEP2_3_MOD = R8_UEP567_MOD = 0;
    R8_USB_INT_FG = 0xff;
    suspended = 0;
    new_session(0); ep0_idle();
    R32_PA_DIR &= ~3u; R32_PA_PU &= ~3u; R32_PA_PD_DRV &= ~3u;
}
