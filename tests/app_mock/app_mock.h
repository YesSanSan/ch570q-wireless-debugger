#ifndef APP_MOCK_H
#define APP_MOCK_H
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include "platform.h"
#include "usb_dap.h"
#include "radio.h"
#include "rf_config.h"
#include "uart_bridge.h"
#include "uart_log.h"
#include "dap.h"

static uint32_t mock_now, mock_generation=1;
static bool mock_usb_configured=true, mock_dtr;
static usb_cdc_line_coding_t mock_line={115200,0,0,8};
static uint8_t mock_usb_rx[256], mock_usb_tx[256], mock_dap_request[64], mock_dap_response[64];
static unsigned mock_usb_rx_len, mock_usb_rx_pos, mock_usb_tx_len;
static unsigned mock_dap_request_len, mock_dap_response_len, mock_dap_calls;
static uint32_t mock_request_tag=1, mock_response_tag;
static uint16_t mock_serial_state;
static bool mock_usb_tx_busy;
static uint8_t mock_rf_rx[32][78], mock_rf_tx[64][78];
static unsigned mock_rf_rx_head, mock_rf_rx_tail, mock_rf_tx_count;
static bool mock_rf_send_ok=true;
static uart_bridge_snapshot_t mock_uart;
static uint8_t mock_uart_rx[256], mock_uart_tx[512];
static unsigned mock_uart_rx_len, mock_uart_rx_pos, mock_uart_tx_len;
static size_t mock_uart_accept=256;
static unsigned mock_configure_calls, mock_enable_calls, mock_disable_calls;
static bool mock_enable_busy, mock_disable_busy;
static unsigned mock_clock_inits, mock_radio_inits, mock_uart_inits, mock_usb_inits;
static bool mock_main_escape;
static unsigned mock_millis_calls;
static jmp_buf mock_main_jump;

void platform_clock_init(void) { ++mock_clock_inits; }
uint32_t platform_millis(void) {
    if(mock_main_escape && ++mock_millis_calls==5) longjmp(mock_main_jump,1);
    return mock_now;
}
void platform_idle(void) {}
void usb_dap_init(void) { ++mock_usb_inits; }
void usb_dap_deinit(void) {}
void usb_dap_poll(void) {}
bool usb_dap_bus_seen(void) { return true; }
bool usb_dap_configured(void) { return mock_usb_configured; }
uint32_t usb_dap_generation(void) { return mock_generation; }
bool usb_dap_take_request_tagged(uint8_t out[64],uint8_t *length,uint32_t *tag) {
    if(!mock_dap_request_len) return false;
    memcpy(out,mock_dap_request,mock_dap_request_len);
    *length=(uint8_t)mock_dap_request_len;*tag=mock_request_tag;mock_dap_request_len=0;return true;
}
bool usb_dap_send_response_tagged(const uint8_t *data,uint8_t length,uint32_t tag) {
    if(tag!=mock_generation) return false;
    memcpy(mock_dap_response,data,length);mock_dap_response_len=length;mock_response_tag=tag;return true;
}
bool usb_dap_discard_request_tagged(uint32_t tag) { return tag==mock_generation; }
uint16_t usb_cdc_read(uint8_t *data,uint16_t capacity) {
    unsigned n=mock_usb_rx_len-mock_usb_rx_pos;if(n>capacity)n=capacity;
    memcpy(data,mock_usb_rx+mock_usb_rx_pos,n);mock_usb_rx_pos+=n;return (uint16_t)n;
}
uint16_t usb_cdc_write(const uint8_t *data,uint16_t length) {
    if(mock_usb_tx_busy)return 0;
    assert(mock_usb_tx_len+length<=sizeof(mock_usb_tx));
    memcpy(mock_usb_tx+mock_usb_tx_len,data,length);mock_usb_tx_len+=length;return length;
}
void usb_cdc_get_line_coding(usb_cdc_line_coding_t *coding) { *coding=mock_line; }
uint8_t usb_cdc_control_line_state(void) { return mock_dtr?1:0; }
void usb_cdc_set_serial_state(uint16_t state) { mock_serial_state=state; }
void radio_init(void) { ++mock_radio_inits; }
bool radio_reconfigure(const rf_config_t *config) { (void)config;return true; }
bool radio_is_idle(void) { return true; }
bool radio_get_hardware_id(uint8_t id[8]) { memset(id,0x11,8);return true; }
void radio_get_stats(radio_stats_t *stats) { memset(stats,0,sizeof(*stats)); }
bool radio_send(const uint8_t frame[78]) {
    if(!mock_rf_send_ok)return false;
    assert(mock_rf_tx_count<64);memcpy(mock_rf_tx[mock_rf_tx_count++],frame,78);return true;
}
bool radio_receive(uint8_t frame[78]) {
    if(mock_rf_rx_tail==mock_rf_rx_head)return false;
    memcpy(frame,mock_rf_rx[mock_rf_rx_tail++],78);return true;
}
void rf_config_defaults(rf_config_t *config) { memset(config,0,sizeof(*config));config->role=RF_CONFIG_ROLE_SLAVE; }
rf_config_status_t rf_config_load(rf_config_t *config) { (void)config;return RF_CONFIG_NOT_FOUND; }
rf_config_status_t rf_config_save(const rf_config_t *config) { (void)config;return RF_CONFIG_UNSUPPORTED; }
bool rf_config_encode(const rf_config_t *config,uint8_t *record,size_t size) {
    (void)config;memset(record,0,size);return true;
}
bool rf_config_decode(rf_config_t *config,const uint8_t *record,size_t size) {
    (void)config;(void)record;(void)size;return false;
}
void dap_init(void) {}
size_t dap_process(const uint8_t *request,size_t length,uint8_t *response,size_t capacity) {
    (void)capacity;++mock_dap_calls;memcpy(response,request,length);return length;
}
void uart_bridge_init(void) {
    ++mock_uart_inits;memset(&mock_uart,0,sizeof(mock_uart));
    mock_uart.baud=115200;mock_uart.bits=8;
}
bool uart_bridge_validate(uint32_t baud,uint8_t stop,uint8_t parity,uint8_t bits) {
    return baud && (stop==0 || stop==2) && parity<=4 && bits>=5 && bits<=8;
}
bool uart_bridge_configure(uint32_t baud,uint8_t stop,uint8_t parity,uint8_t bits) {
    ++mock_configure_calls;
    if(!baud || (stop!=0 && stop!=2) || parity>4 || bits<5 || bits>8)return false;
    /* Match production: even identical configure is busy while data remains. */
    if(mock_uart.enabled && (mock_uart.rx_count || mock_uart.tx_count))return false;
    mock_uart.baud=baud;mock_uart.stop=stop;mock_uart.parity=parity;mock_uart.bits=bits;return true;
}
bool uart_bridge_enable(void) {
    ++mock_enable_calls;mock_uart.enable_pending=mock_enable_busy;
    if(mock_enable_busy)return false;
    mock_uart.enabled=true;return true;
}
bool uart_bridge_disable(void) {
    ++mock_disable_calls;if(mock_disable_busy || mock_uart.tx_count)return false;
    mock_uart.enabled=mock_uart.enable_pending=false;return true;
}
bool uart_bridge_is_enabled(void) { return mock_uart.enabled; }
void uart_bridge_poll(void) { mock_uart.tx_count=0; }
size_t uart_bridge_write(const uint8_t *data,size_t length) {
    if(!mock_uart.enabled)return 0;
    size_t n=length<mock_uart_accept?length:mock_uart_accept;
    assert(mock_uart_tx_len+n<=sizeof(mock_uart_tx));
    memcpy(mock_uart_tx+mock_uart_tx_len,data,n);mock_uart_tx_len+=(unsigned)n;
    mock_uart.tx_count+=(uint16_t)n;return n;
}
size_t uart_bridge_read(uint8_t *data,size_t capacity) {
    unsigned n=mock_uart_rx_len-mock_uart_rx_pos;if(n>capacity)n=(unsigned)capacity;
    memcpy(data,mock_uart_rx+mock_uart_rx_pos,n);mock_uart_rx_pos+=n;
    mock_uart.rx_count=(uint16_t)(mock_uart_rx_len-mock_uart_rx_pos);return n;
}
void uart_bridge_get_snapshot(uart_bridge_snapshot_t *value) { *value=mock_uart; }
#endif
