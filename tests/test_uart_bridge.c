/* Native build (also build with -DWD_UART_LOG=0):
 * cc -std=c11 -Wall -Wextra -Werror -Itests/bridge_mock -Iinclude
 *    tests/test_uart_bridge.c -o build/native/test_uart_bridge
 * Production C is deliberately included so tests exercise actual queue logic.
 */
#include <stdio.h>
#include <string.h>
#ifndef WD_UART_LOG
#define WD_UART_LOG 1
#endif
#include "../src/uart_log.c"
#include "../src/uart_bridge.c"

static uart_bridge_snapshot_t snapshot(void)
{
    uart_bridge_snapshot_t result;
    uart_bridge_get_snapshot(&result);
    return result;
}
static void reset(void)
{
    /* Tests reset the model between independent startup scenarios. */
    mock_on_irq_restore=NULL;mock_irq=0x88;
    mock_tfc=mock_rfc=0; mock_shifting=mock_refill_rx=false;
    mock_empty_errors=0; mock_clock=100000000u;
    mock_output_len=0;
    (void)uart_bridge_disable();
    mock_drain_tx();
    uart_log_init();
    uart_bridge_init();
}
static void test_default_and_configuration(void)
{
    reset();
    assert(!uart_bridge_is_enabled());
    assert(snapshot().baud==115200 && snapshot().rx_room==256 && snapshot().tx_room==0);
    unsigned enables=mock_irq_enables, disables=mock_irq_disables;
    uart_bridge_init();
    assert(mock_irq_enables==enables && mock_irq_disables==disables && !mock_uart_irq_enabled);
    unsigned remaps=mock_remaps, reads=mock_lsr_reads;
    UART_IRQHandler(); /* Stray vector cannot consume legacy log RX/status. */
    assert(mock_lsr_reads==reads);
    uint16_t dl=mock_dl;
    uint8_t data=1;
    assert(uart_bridge_write(&data,1)==0);
    uart_bridge_poll();
    assert(mock_lsr_reads==reads);
    assert(uart_bridge_validate(9600,2,4,7));
    assert(!uart_bridge_validate(0,0,0,8));
    assert(!uart_bridge_validate(1,0,0,8));
    assert(!uart_bridge_validate(UINT32_MAX,0,0,8));
    assert(!uart_bridge_validate(9000000,0,0,8));
    assert(!uart_bridge_validate(115200,1,0,8));
    assert(!uart_bridge_validate(115200,0,5,8));
    assert(!uart_bridge_validate(115200,0,0,9));
    assert(mock_remaps==remaps && mock_dl==dl && snapshot().baud==115200);
    assert(uart_bridge_configure(9600,2,4,7));
    assert(mock_remaps==remaps && mock_dl==dl); /* Stage only, log UART untouched. */
    assert(!uart_bridge_configure(0,0,0,8));
    assert(!uart_bridge_configure(1,0,0,8)); /* Divisor overflow. */
    assert(!uart_bridge_configure(UINT32_MAX,0,0,8));
    assert(!uart_bridge_configure(9000000,0,0,8)); /* Cannot meet 2% baud error. */
    assert(!uart_bridge_configure(115200,1,0,8));
    assert(!uart_bridge_configure(115200,3,0,8));
    assert(!uart_bridge_configure(115200,0,5,8));
    assert(!uart_bridge_configure(115200,0,0,4));
    assert(!uart_bridge_configure(115200,0,0,9));
    assert(snapshot().baud==9600 && snapshot().parity==4);
    mock_clock=0;
    assert(!uart_bridge_enable());
    mock_clock=100000000u;
    mock_receive(0xee,0);
    assert(uart_bridge_enable());
    assert(snapshot().rx_discarded==1 && !snapshot().rx_received);
    assert(mock_lcr==(2u|4u|8u|0x30u) && mock_dl==1302);
    assert(mock_fcr==RB_FCR_FIFO_EN && mock_div==1);
    assert(mock_ier==(RB_IER_TXD_EN|RB_IER_RECV_RDY|RB_IER_LINE_STAT));
    assert(mock_uart_irq_enabled && (mock_mcr & RB_MCR_INT_OE));
    assert(mock_pins==12);
    for (unsigned bits=5;bits<=8;++bits)
        for (unsigned parity=0;parity<=4;++parity)
            for (unsigned stop=0;stop<=2;stop+=2) {
                assert(uart_bridge_configure(115200,(uint8_t)stop,(uint8_t)parity,(uint8_t)bits));
                unsigned expected=(bits-5u)|(stop?4u:0u)|(parity?8u|((parity-1u)<<4):0u);
                assert(mock_lcr==expected);
            }
    uart_bridge_snapshot_t before=snapshot();
    mock_receive(2,RB_LSR_PAR_ERR);
    assert(uart_bridge_validate(9600,0,0,8)); /* Support independent of busy state. */
    assert(!uart_bridge_configure(9600,0,0,8));
    assert(snapshot().baud==before.baud && snapshot().parity_errors==1);
    uart_bridge_poll();
    assert(!uart_bridge_configure(9600,0,0,8)); /* Software RX must also drain. */
    assert(uart_bridge_read(&data,1)==1 && data==2);
    mock_shifting=true;
    assert(!uart_bridge_configure(9600,0,0,8));
    mock_drain_tx();
    assert(uart_bridge_configure(9600,0,0,8));
}
static void test_logging_ownership(void)
{
    reset();
#if WD_UART_LOG
    uart_log_event(UART_LOG_BOOT,0,0);
    uart_log_poll();
    assert(mock_tfc==8);
    assert(!uart_bridge_enable());
    assert(snapshot().enable_pending && !snapshot().enabled);
    unsigned pending_output=mock_output_len, pending_inits=mock_uart_init;
    uart_log_poll();
    uart_log_init(); /* Even init cannot reclaim UART while suspended. */
    assert(mock_output_len==pending_output && mock_uart_init==pending_inits);
    mock_drain_tx();
    assert(uart_bridge_disable()); /* Cancels pending open. */
    assert(!snapshot().enable_pending && mock_uart_init==pending_inits+1);
#endif
    mock_drain_tx();
    assert(uart_bridge_enable());
    unsigned output=mock_output_len, inits=mock_uart_init;
    uart_log_event(UART_LOG_BOOT,0,0);
    uart_log_poll();
    uart_log_init();
    assert(mock_output_len==output && mock_uart_init==inits);
    mock_receive(0x91,0);
    assert(uart_bridge_disable());
    assert(!uart_bridge_is_enabled());
    assert(!mock_uart_irq_enabled && !mock_uart_irq_pending);
    assert(!(mock_ier & (RB_IER_RECV_RDY|RB_IER_LINE_STAT)) && !(mock_mcr & RB_MCR_INT_OE));
    uint8_t byte=0;
    assert(uart_bridge_read(&byte,1)==1 && byte==0x91); /* Preserve closing RX. */
#if WD_UART_LOG
    assert(mock_lcr==3 && mock_dl==109);
    uart_log_event(UART_LOG_BOOT,0,0);
    uart_log_poll();
    assert(mock_output_len>output);
#endif
}
static void test_full_duplex_and_backpressure(void)
{
    reset();
    assert(uart_bridge_enable());
    uint8_t input[300], output[300];
    for (unsigned i=0;i<sizeof(input);++i) input[i]=(uint8_t)i;
    assert(uart_bridge_write(input,sizeof(input))==256);
    assert(snapshot().tx_room==0 && snapshot().tx_accepted==256);
    assert(!uart_bridge_disable());
    mock_tfc=8; mock_shifting=true;
    for (unsigned i=0;i<8;++i) mock_receive((uint8_t)(0xa0+i),0);
    unsigned reads=mock_rbr_reads;
    uart_bridge_poll();
    assert(snapshot().tx_count==256 && mock_output_len==0);
    assert(mock_rbr_reads-reads==8 && snapshot().rx_count==8); /* RX still runs with TX full. */
    assert(uart_bridge_read(output,3)==3 && !memcmp(output,"\xa0\xa1\xa2",3));
    assert(uart_bridge_read(output+3,sizeof(output)-3)==5);
    mock_tfc=6;
    uart_bridge_poll();
    assert(mock_output_len==2 && snapshot().tx_count==254);
    assert(uart_bridge_write(input+256,44)==2); /* Ring wrap and partial acceptance. */
    for (unsigned i=0;i<32;++i) { mock_drain_tx(); uart_bridge_poll(); }
    assert(snapshot().tx_count==0 && snapshot().tx_sent==258);
    assert(mock_output_len==258 && !memcmp(mock_output,input,258));
    assert(!uart_bridge_disable()); /* FIFO and shift register still busy. */
    mock_tfc=0; mock_shifting=true;
    assert(!uart_bridge_disable()); /* Empty FIFO alone is insufficient. */
    mock_drain_tx();
    assert(uart_bridge_disable());
    assert(snapshot().tx_accepted==258 && snapshot().tx_sent==258);
    assert(uart_bridge_read(NULL,8)==0 && uart_bridge_write(NULL,8)==0);
    uart_bridge_get_snapshot(NULL);
}
static void test_rx_overflow_errors_and_bounded_work(void)
{
    reset(); assert(uart_bridge_enable());
    uint8_t packet[64];
    for (unsigned batch=0;batch<33;++batch) {
        for (unsigned i=0;i<8;++i) mock_receive((uint8_t)(batch*8+i),0);
        uart_bridge_poll();
    }
    assert(snapshot().rx_count==256 && snapshot().rx_overflow==8);
    assert(snapshot().rx_received==264 && snapshot().rx_room==0 && !mock_rfc);
    for (unsigned batch=0;batch<4;++batch) {
        assert(uart_bridge_read(packet,sizeof(packet))==64);
        for (unsigned i=0;i<64;++i) assert(packet[i]==(uint8_t)(batch*64+i));
    }
    assert(!uart_bridge_read(packet,64) && snapshot().rx_read==256);
    mock_receive(0x12,RB_LSR_PAR_ERR|RB_LSR_OVER_ERR);
    mock_receive(0x34,RB_LSR_FRAME_ERR|RB_LSR_BREAK_ERR);
    uart_bridge_poll();
    assert(snapshot().overrun_errors==1 && snapshot().parity_errors==1);
    assert(snapshot().framing_errors==1 && snapshot().break_errors==1);
    assert(uart_bridge_read(packet,64)==2 && packet[0]==0x12 && packet[1]==0x34);
    mock_empty_errors=RB_LSR_OVER_ERR;
    uart_bridge_poll(); uart_bridge_poll();
    assert(snapshot().overrun_errors==2); /* Clear-on-read, no double counting. */
    mock_receive(0x55,0); mock_refill_rx=true;
    unsigned reads=mock_rbr_reads, statuses=mock_lsr_reads;
    uart_bridge_poll();
    assert(mock_rbr_reads-reads==8 && mock_lsr_reads-statuses==8);
    mock_refill_rx=false;
    uart_bridge_poll();
    assert(snapshot().rx_count==9); /* Continuous input cannot trap poll. */
}
static unsigned interleaved_bytes;
static void interrupt_during_read(void)
{
    ++interleaved_bytes;
    mock_receive(0xc3,0);
    mock_fire_uart_irq();
}
static void test_interrupt_rx_and_concurrency(void)
{
    reset();assert(uart_bridge_enable());
    /* Foreground does no poll/read for a burst: ISR alone preserves 256 bytes. */
    for(unsigned batch=0;batch<33;++batch) {
        for(unsigned i=0;i<8;++i)mock_receive((uint8_t)(batch*8+i),0);
        mock_fire_uart_irq();
    }
    uart_bridge_snapshot_t before=snapshot();
    assert(before.rx_count==256 && before.rx_received==264 && before.rx_overflow==8);
    assert(!mock_output_len); /* IRQ never sends TX. */
    uint8_t packet[64];interleaved_bytes=0;mock_on_irq_restore=interrupt_during_read;
    assert(uart_bridge_read(packet,sizeof(packet))==64);
    mock_on_irq_restore=NULL;
    for(unsigned i=0;i<64;++i)assert(packet[i]==i);
    assert(interleaved_bytes==65); /* IRQ after initial snapshot and each consumed byte. */
    uart_bridge_snapshot_t after=snapshot();
    assert(after.rx_count==256 && after.rx_received==329 && after.rx_overflow==9);
    assert(after.rx_read==64 && after.rx_room+after.rx_count==256);
    for(unsigned batch=1;batch<4;++batch) {
        assert(uart_bridge_read(packet,64)==64);
        for(unsigned i=0;i<64;++i)assert(packet[i]==(uint8_t)(batch*64+i));
    }
    assert(uart_bridge_read(packet,64)==64);
    for(unsigned i=0;i<64;++i)assert(packet[i]==0xc3);
    /* Line errors clear once even if IRQ and foreground fallback alternate. */
    mock_receive(0xa5,RB_LSR_PAR_ERR|RB_LSR_FRAME_ERR|RB_LSR_BREAK_ERR);
    mock_empty_errors=RB_LSR_OVER_ERR;
    mock_fire_uart_irq();uart_bridge_poll();
    after=snapshot();
    assert(after.parity_errors==1 && after.framing_errors==1 && after.break_errors==1 && after.overrun_errors==1);
    assert(uart_bridge_read(packet,1)==1 && packet[0]==0xa5);
    /* Exhausting one ISR budget re-pends its source rather than spinning. */
    unsigned pends=mock_irq_pends, reads=mock_rbr_reads;
    mock_receive(0x55,0);mock_refill_rx=true;
    mock_fire_uart_irq();
    assert(mock_rbr_reads-reads==8 && mock_irq_pends==pends+1 && mock_uart_irq_pending);
    mock_refill_rx=false;mock_fire_uart_irq();
    assert(!mock_uart_irq_pending && !mock_rfc);
    /* API critical sections restore an already-masked caller exactly. */
    mock_irq=0;after=snapshot();assert(mock_irq==0);
    assert(uart_bridge_read(packet,1)==1 && mock_irq==0);
    mock_irq=0x88;assert(mock_max_masked_rx<=8);
    assert(uart_bridge_disable());
    reads=mock_lsr_reads;UART_IRQHandler();assert(mock_lsr_reads==reads);
}
int main(void)
{
    test_default_and_configuration();
    test_logging_ownership();
    test_full_duplex_and_backpressure();
    test_rx_overflow_errors_and_bounded_work();
    test_interrupt_rx_and_concurrency();
    puts("UART bridge tests passed: config, exclusive ownership, duplex, bounded work, FIFO errors, backpressure, overflow, wrap, drain");
    return 0;
}
