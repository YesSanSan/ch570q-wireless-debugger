/* Native register/state-machine tests, not hardware enumeration/timing validation.
 * Events are serialized; the mock does not implement SIE PID checking, DMA
 * arbitration or W1C. Tests supply TOG_OK and clear flags after each entry.
 * gcc tests/test_usb.c -Itests/usb_mock -Iinclude -std=gnu99 -Wall -Wextra \
 *     -Wno-pointer-to-int-cast -o build/test_usb.exe
 */
#include <assert.h>
#include <stdio.h>
#include "../src/usb_dap.c"

static void event(uint8_t token, uint8_t ep, uint8_t n, uint8_t toggle)
{
    R8_USB_INT_FG = RB_UIF_TRANSFER;
    R8_USB_INT_ST = token | ep | toggle;
    R8_USB_RX_LEN = n;
#if WD_USB_POLLING
    usb_dap_poll();
#else
    USB_IRQHandler();
#endif
    R8_USB_INT_FG = 0; /* emulate W1C */
}
static uint8_t setup_rx_len = 8;
static void setup_status(uint8_t type, uint8_t request, uint16_t value,
                         uint16_t index, uint16_t length, uint8_t status)
{
    ep0_buffer[0] = type; ep0_buffer[1] = request;
    ep0_buffer[2] = value; ep0_buffer[3] = value >> 8;
    ep0_buffer[4] = index; ep0_buffer[5] = index >> 8;
    ep0_buffer[6] = length; ep0_buffer[7] = length >> 8;
    event(status, 0, setup_rx_len, 0);
}
static void setup(uint8_t type, uint8_t request, uint16_t value,
                  uint16_t index, uint16_t length)
{
    /* At startup token bits can be 0x30 (free), not a SETUP PID code. */
    setup_status(type, request, value, index, length,
                 RB_UIS_SETUP_ACT | UIS_TOKEN_SETUP);
}
static void in0(void) { event(UIS_TOKEN_IN, 0, 0, 0); }
static void out0(uint8_t n) { event(UIS_TOKEN_OUT, 0, n, RB_UIS_TOG_OK); }
static void in1(void) { event(UIS_TOKEN_IN, 1, 0, 0); }
static void out1(uint8_t n) { event(UIS_TOKEN_OUT, 1, n, RB_UIS_TOG_OK); }
static void configure(void)
{
    setup(0, USB_SET_ADDRESS, 7, 0, 0);
    in0();
    setup(0, USB_SET_CONFIGURATION, 1, 0, 0);
    in0();
    assert(usb_dap_configured());
}
/* SET_INTERFACE resets only the selected interface's endpoint state. It used to
 * run the full cdc_reset(), so selecting the ACM control interface (2) threw
 * away data interface 3's partially received and queued bytes together with the
 * line coding and DTR, silently killing an open CDC session. */
static void test_set_interface_isolation(void)
{
    uint8_t rx[64];
    usb_cdc_line_coding_t coding;
    usb_dap_init();
    configure();
    setup(0x21, 0x22, 3, 2, 0); in0(); /* DTR + RTS */
    assert(usb_cdc_control_line_state() == 3);
    setup(0x21, 0x20, 0, 2, 7); /* SET_LINE_CODING: data stage follows in DMA */
    ep0_buffer[0] = 0x80; ep0_buffer[1] = 0x25; /* 9600 baud */
    ep0_buffer[2] = 0; ep0_buffer[3] = 0;
    ep0_buffer[4] = 0; ep0_buffer[5] = 0; ep0_buffer[6] = 7; /* 1 stop, none, 7 bits */
    out0(7); in0();
    usb_cdc_get_line_coding(&coding);
    assert(coding.baud_rate == 9600 && coding.data_bits == 7);
    memset(ep3_buffer, 0x5a, 16);
    event(UIS_TOKEN_OUT, 3, 16, RB_UIS_TOG_OK);
    assert(cdc_rx_length == 16 && usb_cdc_write(rx, 4) == 4 && cdc_tx_busy);
    setup(0x01, USB_SET_INTERFACE, 0, 2, 0); in0(); /* control interface only */
    assert(usb_cdc_control_line_state() == 3);
    usb_cdc_get_line_coding(&coding);
    assert(coding.baud_rate == 9600 && coding.data_bits == 7);
    assert(cdc_rx_length == 16 && cdc_tx_busy); /* data interface untouched */
    assert(usb_cdc_read(rx, sizeof(rx)) == 16 && rx[0] == 0x5a);
    event(UIS_TOKEN_IN, 3, 0, 0);
    assert(!cdc_tx_busy && usb_cdc_write(rx, 4) == 4);
    /* Selecting the data interface does reset that interface, as the spec says. */
    setup(0x01, USB_SET_INTERFACE, 0, 3, 0); in0();
    assert(!cdc_tx_busy && !cdc_rx_length && !usb_cdc_read(rx, 1));
    assert(!R8_UEP3_T_LEN && usb_cdc_control_line_state() == 3);
}
static void test_cdc_recovery_discard(void)
{
    uint8_t data[64]={0},packet[64],length;uint32_t tag;
    for(unsigned blocked=0;blocked<3;++blocked) {
        usb_dap_init();R8_USB_INT_FG=0;configure();
        setup(0x21,0x22,3,2,0);in0();
        memset(ep3_buffer,0x5a,64);event(UIS_TOKEN_OUT,3,64,RB_UIS_TOG_OK);
        assert(usb_cdc_write(data,64)==64);
        ep2_buffer[0]=0x02;event(UIS_TOKEN_OUT,2,2,RB_UIS_TOG_OK);
        assert(usb_dap_take_request_tagged(packet,&length,&tag));
        uint32_t old_gen=generation;uint8_t toggles=R8_UEP3_CTRL&(RB_UEP_R_TOG|RB_UEP_T_TOG);
        R8_USB_MIS_ST=RB_UMS_SIE_FREE;
        if(blocked==1) suspended=1;
        if(blocked==2) {halt_in|=1u<<3;halt_out|=1u<<3;cdc_update();}
        assert(usb_cdc_discard_pending());
        assert(!cdc_rx_length && !cdc_rx_offset && !cdc_tx_busy && !cdc_tx_length && !R8_UEP3_T_LEN);
        assert((R8_UEP3_CTRL&(RB_UEP_R_TOG|RB_UEP_T_TOG))==toggles);
        assert(generation==old_gen && request_state==REQ_PROCESSING && usb_cdc_control_line_state()==3);
        suspended=0;halt_in=halt_out=0;cdc_update();
        assert(!usb_cdc_read(data,64));
        assert(usb_cdc_write(data,1)==1);event(UIS_TOKEN_IN,3,0,0);
        assert(!cdc_tx_busy); /* Old full-packet ZLP was cancelled as well. */
        assert(usb_dap_send_response_tagged(packet,length,tag));
    }
}
static void test_cdc_discard_inflight_completion(void)
{
    for(unsigned direction=0;direction<2;++direction) {
        uint8_t data[64]={0};usb_dap_init();R8_USB_INT_FG=0;configure();
        R8_USB_MIS_ST=0; /* Token has started; CPU IRQ masking cannot stop SIE. */
        if(direction) assert(usb_cdc_write(data,64)==64);
        uint8_t toggle=R8_UEP3_CTRL&(direction?RB_UEP_T_TOG:RB_UEP_R_TOG);
        assert(!usb_cdc_discard_pending() && cdc_discarding);
        assert((R8_UEP3_CTRL&MASK_UEP_T_RES)==UEP_T_RES_NAK);
        assert((R8_UEP3_CTRL&MASK_UEP_R_RES)==UEP_R_RES_NAK);
        assert(!usb_cdc_read(data,64) && !usb_cdc_write(data,1));
        if(direction) {
            assert(cdc_tx_busy==1 && R8_UEP3_T_LEN==64);
            event(UIS_TOKEN_IN,3,0,0); /* ACK already accepted before NAK. */
        } else {
            memset(ep3_buffer,0x5a,64);event(UIS_TOKEN_OUT,3,64,RB_UIS_TOG_OK);
        }
        assert((R8_UEP3_CTRL&(direction?RB_UEP_T_TOG:RB_UEP_R_TOG))!=toggle);
        assert(!usb_cdc_discard_pending()); /* Still not hardware free. */
        R8_USB_MIS_ST=RB_UMS_SIE_FREE;assert(usb_cdc_discard_pending());
        assert(!cdc_discarding && !cdc_tx_busy && !cdc_rx_length && !R8_UEP3_T_LEN);
        assert((R8_UEP3_CTRL&(direction?RB_UEP_T_TOG:RB_UEP_R_TOG))!=toggle);
        assert(usb_cdc_write(data,1)==1);event(UIS_TOKEN_IN,3,0,0);assert(!cdc_tx_busy);
    }
}
#include "usb_recovery_cases.h"
static void test_composite(void);
int main(void)
{
    uint8_t out[64], length;
    uint32_t tag, gen;
    /* Start as an attached ISP personality to verify detach-before-reattach. */
    R16_PIN_ALTERNATE = RB_PIN_DEBUG_EN | RB_PIN_USB_EN | RB_UDP_PU_EN;
    R8_UDEV_CTRL = RB_UD_PORT_EN;
    usb_dap_init();
    assert(mock_detach_delay_ms == 20 && !mock_delay_while_attached);
    assert(mock_irq_enabled == !WD_USB_POLLING);
    R8_USB_INT_FG = 0;
    assert((R16_PIN_ALTERNATE & (RB_PIN_USB_EN | RB_UDP_PU_EN)) ==
           (RB_PIN_USB_EN | RB_UDP_PU_EN));
    assert(!(R16_PIN_ALTERNATE & RB_PIN_DEBUG_EN));
    assert(R8_USB_CTRL == (RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN));
    assert(R8_UDEV_CTRL == (RB_UD_PD_DIS | RB_UD_PORT_EN));
    assert(R8_UEP4_1_MOD == (RB_UEP1_RX_EN | RB_UEP1_TX_EN | RB_UEP4_TX_EN));
    assert(R8_UEP2_3_MOD == (RB_UEP2_RX_EN | RB_UEP2_TX_EN | RB_UEP3_RX_EN | RB_UEP3_TX_EN));
    assert(!R8_UEP567_MOD);
    assert(sizeof(configuration_descriptor) == 130);
    assert(sizeof(report_descriptor) == 27);
    assert(configuration_descriptor[25] == sizeof(report_descriptor));
    assert(!usb_dap_configured());
    assert(!usb_dap_take_request(out, &length));
    assert(!usb_dap_take_request_tagged(out, &length, 0));
    assert(((uintptr_t)ep0_buffer & 3) == 0 && ((uintptr_t)ep1_buffer & 3) == 0);

    /* CH570 SETUP_ACT is independent of the retained token and endpoint bits.
     * Exercise all token bit patterns after both EP0 and EP1 traffic. A handler
     * comparing token == UIS_TOKEN_SETUP instead of SETUP_ACT fails this test. */
    for (uint8_t token = 0; token <= MASK_UIS_TOKEN; token += 0x10) {
        for (uint8_t ep = 0; ep <= 1; ++ep) {
            setup_status(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18,
                         RB_UIS_SETUP_ACT | token | ep);
            assert(control_state == CTRL_IN && R8_UEP0_T_LEN == 18);
            assert(!memcmp(ep0_buffer, device_descriptor, 18));
            in0(); out0(0);
        }
    }
    /* SETUP_ACT validates EP0 setup data independently of the normal OUT
     * byte-count register. Do not reject a valid SETUP using stale RX_LEN. */
    const uint8_t prior_lengths[] = {0, 1, 18, 64};
    for (unsigned i = 0; i < sizeof(prior_lengths); ++i) {
        setup_rx_len = prior_lengths[i];
        setup_status(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18,
                     RB_UIS_SETUP_ACT | UIS_TOKEN_IN | 1);
        assert(control_state == CTRL_IN && R8_UEP0_T_LEN == 18);
        assert(!memcmp(ep0_buffer, device_descriptor, 18));
        in0(); out0(0);
    }
    setup_rx_len = 8;
    /* Conversely token bits 0x30 alone mean free, not a new SETUP event. */
    setup_status(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18, UIS_TOKEN_SETUP);
    assert(control_state == CTRL_IDLE && R8_UEP0_T_LEN == 0);
    setup(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 8);
    assert(R8_UEP0_T_LEN == 8 && ep0_buffer[0] == 18);
    in0(); assert(control_state == CTRL_STATUS_OUT);
    out0(0); assert(control_state == CTRL_IDLE);
    setup(0, USB_SET_ADDRESS, 7, 0, 0);
    assert(R8_USB_DEV_AD == 0); /* address must wait for status IN ACK */
    in0(); assert(R8_USB_DEV_AD == 7);
    setup(0, USB_SET_CONFIGURATION, 1, 0, 0);
    assert(!configuration);
    in0(); assert(configuration == 1);
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
    setup(0x81, USB_GET_DESCRIPTOR, 0x2200, 0, 255);
    assert(R8_UEP0_T_LEN == sizeof(report_descriptor));
    in0(); assert(control_state == CTRL_STATUS_OUT);
    setup(0x80, USB_GET_DESCRIPTOR, 0x0302, 0x409, 255);
    assert(R8_UEP0_T_LEN == 2 + 2 * strlen(strings[2]) && ep0_buffer[1] == 3);
    setup(0x80, USB_GET_DESCRIPTOR, 0x03ff, 0x409, 255);
    assert(control_state == CTRL_STALL);
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 65);
    assert(control_state == CTRL_STALL && request_state == REQ_IDLE);

    memset(ep1_buffer, 0x5a, 64);
    out1(64);
    assert(request_state == REQ_READY && (R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert(usb_dap_take_request_tagged(out, &length, &tag));
    assert(length == 64 && out[0] == 0x5a && tag == generation);
    assert(!usb_dap_take_request(out, &length));
    assert(!usb_dap_send_response(out, 65));
    assert(usb_dap_send_response(out, 3));
    assert(R8_UEP1_T_LEN == 64 && ep1_buffer[67] == 0);
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert(!usb_dap_send_response(out, 1));
    setup(0xa1, 1, 0x0100, 0, 255); /* Input GET_REPORT, needs terminating ZLP */
    assert(R8_UEP0_T_LEN == 64 && control_zlp);
    in0(); assert(R8_UEP0_T_LEN == 0 && control_state == CTRL_IN);
    in0(); assert(control_state == CTRL_STATUS_OUT);
    assert(request_state == REQ_RESPONSE); /* GET_REPORT doesn't consume IN */
    in1();
    assert(request_state == REQ_IDLE && (R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);

    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    assert(request_state == REQ_CONTROL_RX && (R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    memset(ep0_buffer, 0xa3, 4);
    out0(4);
    assert(!usb_dap_take_request(out, &length) && request_state == REQ_CONTROL_STATUS);
    in0();
    assert(usb_dap_take_request(out, &length));
    assert(length == 4 && out[3] == 0xa3 && out[4] == 0);
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    assert(control_state == CTRL_STALL && request_state == REQ_PROCESSING);
    gen = usb_dap_generation();
    R8_USB_INT_FG = RB_UIF_BUS_RST | RB_UIF_TRANSFER;
    usb_dap_poll();
    R8_USB_INT_FG = 0;
    assert(!configuration && R8_USB_DEV_AD == 0 && generation == gen + 1);
    assert(request_state == REQ_IDLE && !usb_dap_send_response(out, 4));
    assert(generation != tag);

    configure();
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 64);
    setup(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18); /* abort control OUT */
    assert(request_state == REQ_IDLE && (R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    memset(ep0_buffer, 0x27, 4); out0(4);
    setup(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18); /* abort before status ACK */
    assert(request_state == REQ_IDLE && !usb_dap_take_request(out, &length));
    setup(0x02, USB_SET_FEATURE, 0, 0x81, 0); in0();
    assert(halt_in && (R8_UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL);
    setup(0x02, USB_CLEAR_FEATURE, 0, 0x81, 0); in0();
    assert(!halt_in && !(R8_UEP1_CTRL & RB_UEP_T_TOG));
    event(UIS_TOKEN_OUT, 1, 4, 0); /* wrong toggle must not become request */
    assert(request_state == REQ_IDLE);
    memset(ep1_buffer, 0xa4, 4); out1(4);
    assert(usb_dap_take_request(out, &length));
    R8_USB_MIS_ST = RB_UMS_SUSPEND; R8_USB_INT_FG = RB_UIF_SUSPEND;
    usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(configuration && !usb_dap_send_response(out, 4));
    R8_USB_MIS_ST = 0; R8_USB_INT_FG = RB_UIF_SUSPEND;
    usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(usb_dap_send_response(out, 4));
    gen = generation;
    setup(0x01, USB_SET_INTERFACE, 0, 0, 0); in0();
    assert(generation == gen + 1 && request_state == REQ_IDLE);
    memset(ep1_buffer, 0x07, 64); out1(64);
    assert(usb_dap_take_request_tagged(out, &length, &tag));
    usb_dap_discard_request();
    assert(request_state == REQ_IDLE && R8_UEP1_T_LEN == 0);
    assert((R8_UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_NAK);
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
    assert(!usb_dap_send_response(out, 1));
    mock_irq_enabled = 0;
    usb_dap_poll();
    assert(!mock_irq_enabled); /* APIs preserve prior USB IRQ enable state */
    /* Datasheet ordering: finish the previous status IN before new SETUP. */
    usb_dap_init(); R8_USB_INT_FG = 0;
    setup(0, USB_SET_ADDRESS, 7, 0, 0);
    assert(R8_USB_DEV_AD == 0);
    setup_rx_len = 0;
    setup_status(0x80, USB_GET_DESCRIPTOR, 0x0100, 0, 18,
                 RB_UIS_SETUP_ACT | UIS_TOKEN_IN);
    assert(R8_USB_DEV_AD == 7 && control_state == CTRL_IN);
    assert(R8_UEP0_T_LEN == 18 && !memcmp(ep0_buffer, device_descriptor, 18));
    in0(); out0(0); setup_rx_len = 8;
    setup(0, USB_SET_CONFIGURATION, 1, 0, 0); in0();
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    memset(ep0_buffer, 0x5a, 4); out0(4);
    assert(request_state == REQ_CONTROL_STATUS);
    setup_status(0x80, USB_GET_CONFIGURATION, 0, 0, 1,
                 RB_UIS_SETUP_ACT | UIS_TOKEN_IN);
    assert(request_state == REQ_READY && ep0_buffer[0] == 1);
    assert(usb_dap_take_request(out, &length) && length == 4 && out[0] == 0x5a);
    usb_dap_discard_request();
    test_composite();
    test_set_interface_isolation();test_cdc_recovery_discard();test_cdc_discard_inflight_completion();
    test_ep0_recovery();
#if WD_USB_POLLING
    puts("USB polling tests passed: HID/Bulk routing, Bulk+CDC interleaving/ZLP/toggles/backpressure, descriptors and lifecycle");
#else
    puts("USB IRQ tests passed: HID/Bulk routing, Bulk+CDC interleaving/ZLP/toggles/backpressure, descriptors and lifecycle");
#endif
    return 0;
}

/* Read descriptor bytes as a host would, including all continuation packets. */
static unsigned control_read(uint8_t *out, unsigned capacity)
{
    unsigned total = 0;
    while (control_state == CTRL_IN) {
        unsigned n = R8_UEP0_T_LEN;
        assert(total + n <= capacity);
        memcpy(out + total, ep0_buffer, n); total += n;
        in0();
    }
    assert(control_state == CTRL_STATUS_OUT);
    out0(0); return total;
}
/* Both DAP OUT endpoints must stay closed while CDC progresses independently. */
static void assert_bulk_processing(uint32_t tag, const uint8_t request[64],
                                   uint8_t ep1_ctrl, uint8_t ep2_ctrl)
{
    assert(request_state == REQ_PROCESSING);
    assert(generation == tag && transport == USB_DAP_TRANSPORT_BULK);
    assert(request_length == 17 && !memcmp(request_buffer, request, 64));
    assert(R8_UEP1_CTRL == ep1_ctrl && R8_UEP2_CTRL == ep2_ctrl);
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert((R8_UEP2_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert((R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_NAK);
}
static void test_bulk_cdc_interleaved(void)
{
    uint8_t request[64] = {0}, taken[64], rx[64], tx[64], desc[256], length;
    uint8_t ep1_ctrl, ep2_ctrl, ep3_ctrl, ep4_ctrl;
    uint32_t before = usb_dap_generation(), tag;
    usb_dap_transport_t route;
    const uint8_t notification[] = {0xa1, 0x20, 0, 0, 2, 0, 2, 0, 3, 0};
    assert(request_state == REQ_IDLE && transport == USB_DAP_TRANSPORT_HID);
    for (unsigned i = 0; i < 64; ++i) {
        if (i < 17) request[i] = (uint8_t)(0xa0 + i);
        tx[i] = (uint8_t)(0x80 ^ i);
        rx[i] = (uint8_t)(0x3f - i);
    }
    ep2_ctrl = R8_UEP2_CTRL;
    memcpy(ep2_buffer, request, 17);
    event(UIS_TOKEN_OUT, 2, 17, RB_UIS_TOG_OK);
    assert((R8_UEP2_CTRL & RB_UEP_R_TOG) == ((ep2_ctrl ^ RB_UEP_R_TOG) & RB_UEP_R_TOG));
    assert(usb_dap_take_request_routed(taken, &length, &tag, &route));
    assert(route == USB_DAP_TRANSPORT_BULK && tag == before + 1 && length == 17);
    assert(!memcmp(taken, request, 64));
    ep1_ctrl = R8_UEP1_CTRL; ep2_ctrl = R8_UEP2_CTRL;
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);
    /* Synthetic completions under NAK must not advance DAP or its toggles.
     * Do not overwrite DMA on these events: a real NAK accepts no payload. */
    out1(64); event(UIS_TOKEN_OUT, 2, 17, RB_UIS_TOG_OK);
    in1(); event(UIS_TOKEN_IN, 2, 0, 0);
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);

    event(UIS_TOKEN_IN, 4, 0, 0); /* retire initial SERIAL_STATE */
    assert(!notify_busy);
    ep4_ctrl = R8_UEP4_CTRL;
    usb_cdc_set_serial_state(3);
    assert(R8_UEP4_CTRL == ((ep4_ctrl & RB_UEP_T_TOG) | UEP_R_RES_NAK | UEP_T_RES_ACK));
    assert(R8_UEP4_T_LEN == sizeof(notification));
    assert(!memcmp(ep0_buffer + 64, notification, sizeof(notification)));
    usb_cdc_set_serial_state(1); /* queue a changed state behind this IN */
    assert(!memcmp(ep0_buffer + 64, notification, sizeof(notification)));

    ep3_ctrl = R8_UEP3_CTRL;
    memcpy(ep3_buffer, rx, 64);
    event(UIS_TOKEN_OUT, 3, 64, 0); /* bad PID: no receive or toggle */
    assert(!cdc_rx_length && R8_UEP3_CTRL == ep3_ctrl);
    event(UIS_TOKEN_OUT, 3, 64, RB_UIS_TOG_OK);
    assert(cdc_rx_length == 64 && !cdc_rx_offset);
    assert((R8_UEP3_CTRL & RB_UEP_R_TOG) == ((ep3_ctrl ^ RB_UEP_R_TOG) & RB_UEP_R_TOG));
    assert((R8_UEP3_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert(usb_cdc_write(tx, 64) == 64 && R8_UEP3_T_LEN == 64);
    assert(!memcmp(ep3_buffer + 64, tx, 64));
    ep3_ctrl = R8_UEP3_CTRL;
    assert(!usb_cdc_write(request, 1));
    event(UIS_TOKEN_OUT, 3, 64, RB_UIS_TOG_OK); /* RX NAK */
    assert(R8_UEP3_CTRL == ep3_ctrl && cdc_rx_length == 64);
    assert(usb_cdc_read(desc, 13) == 13 && !memcmp(desc, rx, 13));
    assert(cdc_rx_offset == 13 && (R8_UEP3_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);

    /* EP0 and EP4 share an allocation but must not alias notification bytes. */
    setup(0x80, USB_GET_DESCRIPTOR, 0x0200, 0, 255);
    assert(control_read(desc, sizeof(desc)) == 130);
    assert(!memcmp(ep0_buffer + 64, notification, sizeof(notification)));
    event(UIS_TOKEN_IN, 4, 0, 0);
    assert((R8_UEP4_CTRL & RB_UEP_T_TOG) == ((ep4_ctrl ^ RB_UEP_T_TOG) & RB_UEP_T_TOG));
    assert(notify_busy && R8_UEP4_T_LEN == 10 && ep0_buffer[72] == 1 && !ep0_buffer[73]);
    assert(R8_UEP3_CTRL == ep3_ctrl && !memcmp(ep3_buffer + 64, tx, 64));
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);

    event(UIS_TOKEN_IN, 3, 0, 0); /* 64B ACK queues the terminating ZLP */
    assert(cdc_tx_busy == 2 && R8_UEP3_T_LEN == 0);
    assert(R8_UEP3_CTRL == (uint8_t)(ep3_ctrl ^ RB_UEP_T_TOG));
    assert(!usb_cdc_write(request, 1));
    assert(usb_cdc_read(desc, sizeof(desc)) == 51 && !memcmp(desc, rx + 13, 51));
    assert(!cdc_rx_length && !cdc_rx_offset && !usb_cdc_read(desc, 1));
    assert((R8_UEP3_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
    /* Receive another packet while the previous transmit still owes its ZLP. */
    memcpy(ep3_buffer, rx + 7, 9);
    ep3_ctrl = R8_UEP3_CTRL;
    event(UIS_TOKEN_OUT, 3, 9, RB_UIS_TOG_OK);
    assert((R8_UEP3_CTRL & RB_UEP_R_TOG) == ((ep3_ctrl ^ RB_UEP_R_TOG) & RB_UEP_R_TOG));
    assert(cdc_tx_busy == 2 && !R8_UEP3_T_LEN && !usb_cdc_write(tx, 1));
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);
    ep3_ctrl = R8_UEP3_CTRL;
    event(UIS_TOKEN_IN, 3, 0, 0); /* only this ACK releases TX backpressure */
    assert(!cdc_tx_busy && !R8_UEP3_T_LEN);
    assert(R8_UEP3_CTRL == (uint8_t)((ep3_ctrl ^ RB_UEP_T_TOG) | UEP_T_RES_NAK));
    assert(usb_cdc_read(desc, sizeof(desc)) == 9 && !memcmp(desc, rx + 7, 9));
    assert(usb_cdc_write(tx + 4, 3) == 3 && R8_UEP3_T_LEN == 3);
    assert(!memcmp(ep3_buffer + 64, tx + 4, 3));
    assert_bulk_processing(tag, request, ep1_ctrl, ep2_ctrl);

    assert(!usb_dap_send_response_tagged(request, 17, before));
    assert(usb_dap_send_response_tagged(request, 17, tag));
    assert(R8_UEP2_T_LEN == 17 && !memcmp(ep2_buffer + 64, request, 17));
    assert((R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_ACK);
    ep2_ctrl = R8_UEP2_CTRL; ep3_ctrl = R8_UEP3_CTRL; ep4_ctrl = R8_UEP4_CTRL;
    event(UIS_TOKEN_IN, 4, 0, 0);
    assert(!notify_busy && !R8_UEP4_T_LEN);
    assert((R8_UEP4_CTRL & RB_UEP_T_TOG) == ((ep4_ctrl ^ RB_UEP_T_TOG) & RB_UEP_T_TOG));
    event(UIS_TOKEN_IN, 3, 0, 0); /* short packet needs no ZLP */
    assert(!cdc_tx_busy && !R8_UEP3_T_LEN);
    assert(R8_UEP3_CTRL == (uint8_t)((ep3_ctrl ^ RB_UEP_T_TOG) | UEP_T_RES_NAK));
    in1(); out1(64); event(UIS_TOKEN_OUT, 2, 17, RB_UIS_TOG_OK);
    assert(request_state == REQ_RESPONSE && generation == tag && transport == route);
    assert(R8_UEP1_CTRL == ep1_ctrl && R8_UEP2_CTRL == ep2_ctrl);
    assert(!memcmp(request_buffer, request, 64) && !memcmp(ep2_buffer + 64, request, 17));
    assert(!memcmp(taken, request, 64));
    event(UIS_TOKEN_IN, 2, 0, 0);
    assert(request_state == REQ_IDLE && generation == tag && transport == route);
    assert(!R8_UEP2_T_LEN && (R8_UEP2_CTRL & MASK_UEP_T_RES) == UEP_T_RES_NAK);
    assert((R8_UEP2_CTRL & RB_UEP_T_TOG) == ((ep2_ctrl ^ RB_UEP_T_TOG) & RB_UEP_T_TOG));
    assert((R8_UEP2_CTRL & RB_UEP_R_TOG) == (ep2_ctrl & RB_UEP_R_TOG));
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
    assert((R8_UEP2_CTRL & MASK_UEP_R_RES) == UEP_R_RES_ACK);
}
static void test_composite(void)
{
    uint8_t desc[256], packet[64], length;
    uint32_t tag, old_tag, before;
    usb_dap_transport_t route;
    usb_cdc_line_coding_t coding;
    unsigned pos, iface = 255, endpoints = 0, interfaces = 0;
    uint8_t ep_seen[256] = {0};
    assert(sizeof(ep0_buffer) + sizeof(ep1_buffer) + sizeof(ep2_buffer) + sizeof(ep3_buffer) == 512);
    assert(((uintptr_t)ep2_buffer & 3) == 0 && ((uintptr_t)ep3_buffer & 3) == 0);
    assert(R16_UEP2_DMA == (uint16_t)(uintptr_t)ep2_buffer);
    assert(R16_UEP3_DMA == (uint16_t)(uintptr_t)ep3_buffer);
    setup(0x80, USB_GET_DESCRIPTOR, 0x0200, 0, 255);
    assert(control_read(desc, sizeof(desc)) == 130);
    assert(le16(desc + 2) == 130 && desc[4] == 4);
    for (pos = 9; pos < 130; pos += desc[pos]) {
        assert(desc[pos] >= 2 && pos + desc[pos] <= 130);
        if (desc[pos + 1] == 4) { iface = desc[pos + 2]; ++interfaces; }
        if (desc[pos + 1] == 5) {
            uint8_t ep = desc[pos + 2];
            assert(!ep_seen[ep]); ep_seen[ep] = 1; ++endpoints;
            assert(le16(desc + pos + 4) <= 64);
            if (iface == 1) assert((ep == 2 || ep == 0x82) && desc[pos + 3] == 2);
            if (iface == 2) assert(ep == 0x84 && desc[pos + 3] == 3);
            if (iface == 3) assert((ep == 3 || ep == 0x83) && desc[pos + 3] == 2);
        }
    }
    assert(pos == 130 && interfaces == 4 && endpoints == 7);
    setup(0x80, USB_GET_DESCRIPTOR, 0x03ee, 0, 255);
    assert(control_read(desc, sizeof(desc)) == 18 && desc[16] == MS_VENDOR_CODE);
    setup(0xc0, MS_VENDOR_CODE, 0, 4, 255);
    assert(control_read(desc, sizeof(desc)) == 40 && desc[16] == 1);
    assert(!memcmp(desc + 18, "WINUSB", 6));
    setup(0xc1, MS_VENDOR_CODE, 1, 5, 255);
    assert(control_read(desc, sizeof(desc)) == 146);
    assert(le16(desc) == 146 && le16(desc + 10) == 136 && desc[14] == 7);
    assert(le16(desc + 18) == 42 && le16(desc + 62) == 80);
    assert(desc[142] == 0 && desc[144] == 0);
    setup(0xc1, MS_VENDOR_CODE, 0, 5, 255); assert(control_state == CTRL_STALL);
    setup(0xc0, MS_VENDOR_CODE, 1, 4, 255); assert(control_state == CTRL_STALL);
    setup(0x81, USB_GET_INTERFACE, 0, 3, 1);
    assert(control_read(desc, sizeof(desc)) == 1 && !desc[0]);
    setup(0x81, USB_GET_INTERFACE, 0, 4, 1); assert(control_state == CTRL_STALL);

    /* Route a Bulk packet, block both OUT paths until its own IN ACK. */
    before = usb_dap_generation();
    memset(ep2_buffer, 0x24, 5); event(UIS_TOKEN_OUT, 2, 5, RB_UIS_TOG_OK);
    assert(usb_dap_take_request_routed(packet, &length, &tag, &route));
    assert(route == USB_DAP_TRANSPORT_BULK && tag == before + 1 && length == 5);
    assert(packet[0] == 0x24 && packet[5] == 0);
    assert((R8_UEP1_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    assert((R8_UEP2_CTRL & MASK_UEP_R_RES) == UEP_R_RES_NAK);
    old_tag = tag;
    memset(ep1_buffer, 0xf1, 64); out1(64); /* synthetic NAK ignored */
    assert(generation == tag && transport == USB_DAP_TRANSPORT_BULK);
    assert(usb_dap_send_response_tagged(packet, 3, tag));
    assert(R8_UEP2_T_LEN == 3 && !memcmp(ep2_buffer + 64, packet, 3));
    assert((R8_UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_NAK);
    in1(); assert(request_state == REQ_RESPONSE); /* cannot consume Bulk */
    event(UIS_TOKEN_IN, 2, 0, 0); assert(request_state == REQ_IDLE);
    memset(ep1_buffer, 0x42, 64); out1(64);
    assert(usb_dap_take_request_routed(packet, &length, &tag, &route));
    assert(route == USB_DAP_TRANSPORT_HID && tag == old_tag + 1);
    assert(!usb_dap_send_response_tagged(packet, 4, old_tag));
    assert(!usb_dap_discard_request_tagged(old_tag));
    assert(request_state == REQ_PROCESSING);
    assert(usb_dap_send_response_tagged(packet, 2, tag));
    assert(R8_UEP1_T_LEN == 64 && ep1_buffer[66] == 0);
    event(UIS_TOKEN_IN, 2, 0, 0); assert(request_state == REQ_RESPONSE);
    in1();
    /* Same transport does not reset session. */
    out1(64); assert(usb_dap_take_request_tagged(packet, &length, &old_tag));
    assert(old_tag == tag); usb_dap_discard_request();
    /* Control report commits transport switch only on status ACK. */
    memset(ep2_buffer, 7, 1); event(UIS_TOKEN_OUT, 2, 1, RB_UIS_TOG_OK);
    assert(usb_dap_take_request_tagged(packet, &length, &old_tag)); usb_dap_discard_request();
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    memset(ep0_buffer, 3, 4); out0(4);
    setup(0x80, USB_GET_CONFIGURATION, 0, 0, 1);
    assert(generation == old_tag && transport == USB_DAP_TRANSPORT_BULK);
    setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4);
    memset(ep0_buffer, 3, 4); out0(4); in0();
    assert(generation == old_tag + 1 && transport == USB_DAP_TRANSPORT_HID);
    assert(usb_dap_take_request_tagged(packet, &length, &tag)); usb_dap_discard_request();

    usb_cdc_get_line_coding(&coding);
    assert(coding.baud_rate == 115200 && coding.data_bits == 8 && !coding.parity);
    setup(0xa1, 0x21, 0, 2, 7);
    assert(control_read(desc, sizeof(desc)) == 7 && desc[6] == 8);
    setup(0x21, 0x20, 0, 2, 7);
    { const uint8_t line[] = {0x80,0x25,0,0,2,2,7}; memcpy(ep0_buffer, line, 7); }
    out0(7); usb_cdc_get_line_coding(&coding); assert(coding.baud_rate == 115200);
    in0(); usb_cdc_get_line_coding(&coding);
    assert(coding.baud_rate == 9600 && coding.stop_bits == 2 && coding.parity == 2 && coding.data_bits == 7);
    setup(0x21, 0x20, 0, 2, 7); memset(ep0_buffer, 0, 7); out0(7);
    assert(control_state == CTRL_STALL); usb_cdc_get_line_coding(&coding); assert(coding.baud_rate == 9600);
    setup(0x21, 0x20, 0, 2, 7); memset(ep0_buffer, 0, 7); ep0_buffer[6] = 8; out0(7);
    setup(0x80, USB_GET_CONFIGURATION, 0, 0, 1); /* abort before ACK */
    usb_cdc_get_line_coding(&coding); assert(coding.baud_rate == 9600);
    setup(0x21, 0x22, 3, 2, 0); assert(!usb_cdc_control_line_state());
    in0(); assert(usb_cdc_control_line_state() == 3);
    setup(0x21, 0x23, 0xffff, 2, 0); in0(); assert(usb_cdc_break_duration() == 0xffff);
    setup(0xa1, 0x21, 0, 3, 7); assert(control_state == CTRL_STALL);

    test_bulk_cdc_interleaved();
    for (uint8_t ep = 1; ep <= 4; ++ep) {
        setup(0x02, USB_SET_FEATURE, 0, 0x80 | ep, 0); in0();
        setup(0x82, USB_GET_STATUS, 0, 0x80 | ep, 2);
        assert(control_read(desc, sizeof(desc)) == 2 && desc[0] == 1);
        setup(0x02, USB_CLEAR_FEATURE, 0, 0x80 | ep, 0); in0();
        assert(!(*ep_ctrl(ep) & RB_UEP_T_TOG));
    }
    setup(0x02, USB_SET_FEATURE, 0, 4, 0); assert(control_state == CTRL_STALL);
    assert(usb_dap_bus_seen());
    R8_USB_MIS_ST = RB_UMS_SUSPEND; R8_USB_INT_FG = RB_UIF_SUSPEND;
    usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(!usb_cdc_write(packet, 1) && !usb_cdc_read(desc, 1));
    R8_USB_MIS_ST = 0; R8_USB_INT_FG = RB_UIF_SUSPEND;
    usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(usb_cdc_write(packet, 1));
    R8_USB_INT_FG = RB_UIF_BUS_RST; usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(!usb_dap_configured() && !cdc_tx_busy && !cdc_rx_length && !usb_cdc_control_line_state());
    assert(!usb_cdc_write(packet, 1));
    R32_PA_DIR = R32_PA_PU = R32_PA_PD_DRV = 0xffffffff;
    usb_dap_deinit(); R8_USB_INT_FG = 0;
    assert(!mock_irq_enabled && !R8_USB_INT_EN && !R8_USB_CTRL && !R8_UDEV_CTRL);
    assert(!(R16_PIN_ALTERNATE & (RB_UDP_PU_EN | RB_PIN_USB_EN | RB_PIN_DEBUG_EN)));
    assert(!(R32_PA_DIR & 3) && !(R32_PA_PU & 3) && !(R32_PA_PD_DRV & 3));
    usb_dap_init(); R8_USB_INT_FG = 0; assert(!usb_dap_bus_seen());
    R8_USB_INT_FG = RB_UIF_BUS_RST; usb_dap_poll(); R8_USB_INT_FG = 0;
    assert(usb_dap_bus_seen());
}
