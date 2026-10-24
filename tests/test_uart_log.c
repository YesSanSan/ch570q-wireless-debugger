#include <stdio.h>
#include <string.h>
#define WD_UART_LOG 1
#include "../src/uart_log.c"
static void drain(void) { for(unsigned i=0;i<1000;++i) { mock_fifo=0;uart_log_poll(); } output[output_len]=0; }
int main(void)
{
    uart_log_init();
    assert(mock_uart_init==1 && mock_pins==12);
    uart_log_event(UART_LOG_BOOT,0xd0,100000000);
    assert(!output_len && mock_irq==0x88); /* Enqueue has no UART side effects. */
    mock_fifo=8;uart_log_poll();assert(!output_len);
    mock_fifo=0;uart_log_poll();assert(output_len==8);
    drain();
    assert(strcmp(output,"BOOT 000000D0 05F5E100\r\n")==0);
    output_len=0;uart_log_init();
    for(unsigned i=0;i<40;++i) uart_log_event(UART_LOG_USB_SETUP,i,0);
    assert(!output_len && lost==9);
    mock_irq=0;uart_log_event(UART_LOG_USB_RESET,0,0);assert(mock_irq==0 && lost==10);
    drain();
    assert(strstr(output,"USB_SETUP 00000000 00000000\r\n"));
    assert(strstr(output,"USB_SETUP 0000001E 00000000\r\n"));
    assert(strstr(output,"DROPPED 0000000A 0000000A\r\n"));
    puts("UART log tests passed: PA2/PA3 init, enqueue-only ISR, FIFO backpressure, order, overflow reporting, IRQ state preservation");
    return 0;
}
