/* USB-only board bring-up image. No RF, SysTick or SWD. USB handled by polling.
 * This is an isolation diagnostic, not the wireless debugger application.
 */
#include "CH57x_common.h"
#include "usb_dap.h"
#include "uart_log.h"
#include "board.h"
#include <string.h>
static uint8_t request[64], response[64];
volatile uint32_t usb_diag_requests;
static uint8_t process(uint8_t n)
{
    const char *text = 0;
    memset(response, 0, sizeof(response));
    response[0] = request[0];
    if (n >= 2 && request[0] == 0) {
        switch (request[1]) {
        case 1: text = "CH570Q"; break;
        case 2: text = "USB polling diagnostic"; break;
        case 3: text = "USB-DIAG-0001"; break;
        case 4: text = "0.1.0"; break;
        case 0xf0: response[1]=1; response[2]=0; return 3; /* No debug port. */
        case 0xfe: response[1]=1; response[2]=1; return 3;
        case 0xff: response[1]=2; response[2]=64; return 4;
        default: return 2;
        }
        size_t length = strlen(text) + 1;
        response[1] = (uint8_t)length;
        memcpy(response + 2, text, length);
        return (uint8_t)(length + 2);
    }
    if (request[0] == 2) return 2; /* Connect: no SWD available. */
    if (request[0] == 3) return 2; /* Disconnect. */
    response[0] = 0xff;
    return 1;
}
int main(void)
{
    HSECFG_Capacitance(BOARD_HSE_CAPACITANCE);
    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
    uart_log_init();
    uart_log_event(UART_LOG_BOOT, WD_USB_POLLING ? 0xd0 : 0xd1, BOARD_SYS_CLOCK_HZ);
    uart_log_poll();
    usb_dap_init();
    uint8_t pending = 0, length = 0;
    uint32_t tag = 0, loops = 0;
    for (;;) {
        uart_log_poll();
        if (++loops % 100000u == 0) uart_log_event(UART_LOG_HEARTBEAT, loops, usb_diag_requests);
        usb_dap_poll();
        if (pending && usb_dap_generation() != tag) pending = 0;
        if (!pending && usb_dap_take_request_tagged(request, &length, &tag)) {
            ++usb_diag_requests;
            if (length && request[0] == 7) {
                usb_dap_discard_request();
            } else {
                length = process(length);
                pending = 1;
            }
        }
        if (pending && usb_dap_send_response(response, length)) pending = 0;
    }
}
