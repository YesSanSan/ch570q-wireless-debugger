#ifndef USB_DAP_H
#define USB_DAP_H

#include <stdbool.h>
#include <stdint.h>

/* Full-speed composite: interface 0 HID EP1, interface 1 WinUSB DAP EP2,
 * interfaces 2/3 CDC ACM EP4 notification / EP3 data. 64-byte DAP packets.
 * Init after SDK clock setup; claims PA0/UDM PA1/UDP (disables their debug
 * function). Main-loop APIs only. No heap; single DAP credit shared by HID
 * and Bulk. CDC has one 64-byte RX and TX packet with nonblocking backpressure.
 */
typedef enum {
    USB_DAP_TRANSPORT_NONE = 0,
    USB_DAP_TRANSPORT_HID = 1,
    USB_DAP_TRANSPORT_BULK = 2
} usb_dap_transport_t;

void usb_dap_init(void);
void usb_dap_poll(void);
/* Positive host evidence since init: BUS_RESET or SETUP, not VBUS sensing. */
bool usb_dap_bus_seen(void);
/* Disable IRQ/controller/pull-up and leave PA0/1 floating inputs, debug off.
 * Caller must cease USB use before assigning the pins to SWD. */
void usb_dap_deinit(void);
bool usb_dap_take_request(uint8_t out[64], uint8_t *length);
bool usb_dap_take_request_tagged(uint8_t out[64], uint8_t *length,
                                 uint32_t *request_generation);
/* Atomic packet + session + route capture, preferred for asynchronous work. */
bool usb_dap_take_request_routed(uint8_t out[64], uint8_t *length,
                                uint32_t *request_generation,
                                usb_dap_transport_t *request_transport);
/* Existing APIs route to the taken request. HID pads to 64; Bulk sends the
 * actual length (1..64). Retry on suspend/halt. Async callers MUST use tagged
 * send/discard: a stale reply must never consume another session's request. */
bool usb_dap_send_response(const uint8_t *data, uint8_t length);
bool usb_dap_send_response_tagged(const uint8_t *data, uint8_t length,
                                  uint32_t request_generation);
void usb_dap_discard_request(void);
bool usb_dap_discard_request_tagged(uint32_t request_generation);
bool usb_dap_configured(void);
uint32_t usb_dap_generation(void);
usb_dap_transport_t usb_dap_transport(void);
/* Generation changes on init, reset, SET_CONFIGURATION, DAP SET_INTERFACE,
 * and accepted HID/Bulk transport switch. Compare the atomically captured tag
 * BEFORE executing DAP, reset the DAP engine when it changes, and tag replies.
 * Both DAP OUT endpoints NAK until reply IN ACK/discard: no transport takeover
 * during an outstanding request. Suspend retains the configuration/session.
 */

typedef struct {
    uint32_t baud_rate;
    uint8_t stop_bits; /* USB CDC encoding: 0=1, 1=1.5, 2=2 */
    uint8_t parity;    /* 0=none, 1=odd, 2=even, 3=mark, 4=space */
    uint8_t data_bits;
} usb_cdc_line_coding_t;
/* Return copied/queued bytes, 0 for busy/unconfigured/suspended/invalid args.
 * Read may consume part of the pending packet. Write queues at most 64 bytes;
 * callers retain unsent bytes and retry. Full packets get a terminating ZLP.
 * DTR is exposed, not required for transfer. Reset discards pending CDC data. */
uint16_t usb_cdc_read(uint8_t *data, uint16_t capacity);
uint16_t usb_cdc_write(const uint8_t *data, uint16_t length);
/* Begin/retry cancellation of pending CDC IN/OUT. Returns true only after
 * SIE is free and accepted old tokens were serviced. No busy wait; false keeps
 * EP3 NAKed. Preserve coding, DTR, endpoint toggles and DAP ownership. */
bool usb_cdc_discard_pending(void);
void usb_cdc_get_line_coding(usb_cdc_line_coding_t *coding);
uint8_t usb_cdc_control_line_state(void); /* bit 0 DTR, bit 1 RTS */
uint16_t usb_cdc_break_duration(void); /* 0 stop, 0xffff indefinite, else ms */
/* CDC SERIAL_STATE bits 0..6. Changes coalesce while notification is pending. */
void usb_cdc_set_serial_state(uint16_t state);

#endif
