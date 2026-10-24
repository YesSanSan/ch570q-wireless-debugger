/* Native integration test, actual app.c with renamed main and production link/retry:
 * gcc -std=c11 -Wall -Wextra -Werror -DWD_UART_LOG=0 -Itests/app_mock -Iinclude
 *   tests/test_app.c src/link.c src/retry.c -o build/native/test_app.exe
 */
#include <stdio.h>
#include "app_mock/app_mock.h"
#define main app_firmware_main
#include "../src/app.c"
#undef main

static void reset_app(void)
{
    mock_now=100;mock_generation=mock_request_tag=1;mock_response_tag=0;
    mock_usb_configured=true;mock_dtr=false;mock_usb_tx_busy=false;
    mock_line=(usb_cdc_line_coding_t){115200,0,0,8};
    mock_usb_rx_len=mock_usb_rx_pos=mock_usb_tx_len=0;
    mock_dap_request_len=mock_dap_response_len=mock_dap_calls=0;
    mock_rf_rx_head=mock_rf_rx_tail=mock_rf_tx_count=0;mock_rf_send_ok=true;
    mock_uart_rx_len=mock_uart_rx_pos=mock_uart_tx_len=0;mock_uart_accept=256;
    mock_configure_calls=mock_enable_calls=mock_disable_calls=0;
    mock_enable_busy=mock_disable_busy=false;mock_serial_state=0;
    uart_bridge_init();rf_config_defaults(&settings);retry_init(&retry);link_slave_init(&slave);
    memset(hardware_id,0x11,sizeof(hardware_id));identity_valid=true;
    role=RF_CONFIG_ROLE_HOST;phase=owner=0;generation=1;session=0x1234;sequence=1;
    nonce=started=last_tx=last_activity=last_uart=0;
    discovery_start=discovery_nonce=discovery_due=discovery_reply_nonce=discovery_round=0;
    link_timeouts=link_retries=link_rejected=completed=0;
    fault=pending_response=discover_active=discover_reply_pending=apply_pending=false;
    cdc_quarantined=reconnecting=false;response_started=reconnects=response_expirations=0;
    request_len=response_len=0;peer_count=0;stream_reset();last_stream=0;
}
static void step(uint32_t now) { mock_now=now;host_poll(now); }
static void queue_packet(const struct link_packet *packet)
{
    assert(mock_rf_rx_head<32);
    assert(link_encode(packet,mock_rf_rx[mock_rf_rx_head++]));
}
static struct link_packet last_packet(void)
{
    struct link_packet packet;assert(mock_rf_tx_count);
    assert(link_decode(mock_rf_tx[mock_rf_tx_count-1],&packet));return packet;
}
static void answer(uint8_t accepted,const uint8_t *bytes,uint8_t count)
{
    struct link_packet packet={0};packet.type=LINK_RESPONSE;
    packet.session=session;packet.sequence=sequence;packet.data[0]=request[0];
    if(request[0]==WD_CMD_UART) {
        packet.length=4+count;packet.data[1]=WD_STATUS_OK;
        packet.data[2]=accepted;packet.data[3]=count;
        if(count)memcpy(packet.data+4,bytes,count);
    } else {packet.length=2;packet.data[1]=(request[0]==0x02 && request[1]<=1)?1:0;}
    queue_packet(&packet);
}
static void dap_request(uint8_t command)
{
    mock_dap_request[0]=command;mock_dap_request[1]=0x42;mock_dap_request_len=2;
    mock_request_tag=mock_generation;
}
static void uart_payload(uint8_t *data,uint8_t enabled,const char *text)
{
    size_t count=text?strlen(text):0;
    memset(data,0,64);data[0]=WD_CMD_UART;put32(data+1,115200);
    data[7]=8;data[8]=enabled;data[9]=(uint8_t)count;
    if(count)memcpy(data+10,text,count);
}
static void test_real_main_startup(void)
{
    reset_app();mock_clock_inits=mock_radio_inits=mock_uart_inits=mock_usb_inits=0;
    mock_millis_calls=0;mock_main_escape=true;
    if(!setjmp(mock_main_jump)) app_firmware_main();
    mock_main_escape=false;
    assert(mock_clock_inits==1 && mock_radio_inits==1 && mock_uart_inits==1);
    assert(role==RF_CONFIG_ROLE_SLAVE && !uart_bridge_is_enabled());
}
static void test_shared_dap_cdc_sequence(void)
{
    reset_app();mock_dtr=true;dap_request(0x02);
    step(10);
    struct link_packet packet=last_packet();
    assert(packet.type==LINK_REQUEST && packet.sequence==1 && packet.data[0]==0x02);
    answer(0,NULL,0);step(11);
    assert(mock_dap_response_len==2 && mock_response_tag==1);
    packet=last_packet();
    assert(packet.type==LINK_REQUEST && packet.sequence==2 && packet.data[0]==WD_CMD_UART);
    answer(0,NULL,0);step(12);
    dap_request(0x05);step(13);packet=last_packet();
    assert(packet.sequence==3 && packet.data[0]==0x05);
    assert(session==0x1234 && !fault);
}
static void test_cached_uart_reply_and_busy_configuration(void)
{
    reset_app();
    struct link_slave peer;link_slave_init(&peer);
    uint8_t frame[78], first[78], replay[78];
    struct link_packet packet={0};packet.type=LINK_HELLO;packet.sequence=17;
    assert(link_encode(&packet,frame));
    assert(link_slave_handle(&peer,frame,first,0x99,dispatch,reset_dispatch));
    packet.type=LINK_REQUEST;packet.session=peer.session;packet.sequence=1;
    uart_payload(packet.data,1,"abcd");packet.length=14;
    assert(uart_bridge_enable()); /* RX below belongs to an already open session. */
    mock_uart_accept=2;mock_uart_rx[0]='R';mock_uart_rx[1]='X';mock_uart_rx_len=2;mock_uart.rx_count=2;
    assert(link_encode(&packet,frame));
    assert(link_slave_handle(&peer,frame,first,0,dispatch,reset_dispatch));
    assert(mock_uart_tx_len==2 && !memcmp(mock_uart_tx,"ab",2));
    struct link_packet reply;assert(link_decode(first,&reply));
    assert(reply.data[2]==2 && reply.data[3]==2 && !memcmp(reply.data+4,"RX",2));
    unsigned enables=mock_enable_calls, configurations=mock_configure_calls;
    assert(link_slave_handle(&peer,frame,replay,0,dispatch,reset_dispatch)); /* Lost first RF reply. */
    assert(!memcmp(first,replay,78));
    assert(mock_uart_tx_len==2 && mock_enable_calls==enables && mock_configure_calls==configurations);
    packet.data[10]='!';assert(link_encode(&packet,frame));
    assert(!link_slave_handle(&peer,frame,replay,0,dispatch,reset_dispatch)); /* Changed duplicate. */
    packet.sequence=2;packet.length=1;packet.data[0]=0x02;
    assert(link_encode(&packet,frame));
    assert(link_slave_handle(&peer,frame,replay,0,dispatch,reset_dispatch));
    assert(mock_dap_calls==1);
    /* Identical settings must not call busy configure while previous TX remains. */
    packet.sequence=3;uart_payload(packet.data,1,"cd");packet.length=12;
    assert(link_encode(&packet,frame));
    assert(link_slave_handle(&peer,frame,replay,0,dispatch,reset_dispatch));
    assert(mock_uart_tx_len==4 && !memcmp(mock_uart_tx,"abcd",4));
    assert(mock_configure_calls==configurations);
}
static void test_partial_tx_and_dtr_drain(void)
{
    reset_app();mock_dtr=true;memcpy(mock_usb_rx,"abcdef",6);mock_usb_rx_len=6;
    step(10);struct link_packet packet=last_packet();
    assert(packet.data[8]==1 && packet.data[9]==6 && !memcmp(packet.data+10,"abcdef",6));
    answer(2,NULL,0);step(11);
    assert(cdc_tx_len==4 && !memcmp(cdc_tx,"cdef",4));
    mock_dtr=false;step(20);packet=last_packet();
    assert(packet.data[8]==1 && packet.data[9]==4 && !memcmp(packet.data+10,"cdef",4));
    answer(4,NULL,0);step(21);assert(!cdc_tx_len);
    step(30);packet=last_packet();
    assert(packet.data[8]==0 && packet.data[9]==0);
    answer(0,NULL,0);step(31);
    assert(!previous_uart_enabled && !cdc_open && !cdc_tx_len);
    unsigned sent=mock_rf_tx_count;step(40);assert(mock_rf_tx_count==sent);
    mock_dtr=true;step(50);packet=last_packet();
    assert(packet.data[8]==1 && packet.data[9]==0); /* No stale suffix on reopen. */
}
static void test_cdc_rx_backpressure(void)
{
    reset_app();mock_dtr=true;step(10);
    static const uint8_t received[]={0x00,0xff,0x5a};
    mock_usb_tx_busy=true;answer(0,received,sizeof(received));step(11);
    assert(phase==0 && !mock_usb_tx_len && cdc_rx_count==sizeof(received));
    dap_request(0x02);step(15);
    assert(phase==2 && owner==0 && cdc_rx_count==sizeof(received)); /* DAP proceeds while CDC IN busy. */
    mock_usb_tx_busy=false;step(16);
    assert(mock_usb_tx_len==sizeof(received) && !memcmp(mock_usb_tx,received,sizeof(received)));
    assert(!cdc_rx_count && phase==2);
    step(17);assert(mock_usb_tx_len==sizeof(received)); /* Delivered exactly once. */
}
static void test_invalid_settings_and_pending_close(void)
{
    reset_app();uint8_t data[64], out[64];uart_payload(data,1,"bad");
    data[5]=1;memset(out,0,sizeof(out));
    assert(dispatch(data,13,out,sizeof(out))==4);
    assert(out[1]==WD_STATUS_INVALID && !mock_uart_tx_len && !mock_uart.enabled);
    uart_payload(data,1,NULL);mock_enable_busy=true;
    assert(dispatch(data,10,out,sizeof(out))==4);
    assert(mock_uart.enable_pending && !mock_uart.enabled);
    role=RF_CONFIG_ROLE_SLAVE;mock_now=2201;slave_poll(mock_now);
    assert(!mock_uart.enable_pending); /* Peer vanished during ownership handoff. */
}
static void test_timeout_fault_lock_and_generation(void)
{
    reset_app();dap_request(0x05);step(10);assert(phase==2);
    uint8_t old_frame[78];memcpy(old_frame,mock_rf_tx[0],78);
    step(111);assert(link_retries==1 && mock_rf_tx_count==2);
    assert(!memcmp(old_frame,mock_rf_tx[1],78)); /* Exact request, same sequence. */
    step(1510);assert(fault && link_timeouts==1);
    step(1511);assert(mock_dap_response_len && phase==0);
    unsigned sent=mock_rf_tx_count;dap_request(0x02);step(1512);step(1513);
    assert(fault && mock_rf_tx_count==sent); /* Uncertain execution locks target operations. */
    mock_dtr=true;step(1520);assert(mock_rf_tx_count==sent);
    mock_generation=2;mock_dtr=false;mock_usb_configured=false;step(1521);
    assert(!fault && !phase && !session && sequence==1 && !cdc_tx_len);
    /* Old radio completion is rejected after generation reset. */
    struct link_packet stale={0};stale.type=LINK_RESPONSE;stale.session=0x1234;
    stale.sequence=1;stale.length=2;stale.data[0]=0x05;queue_packet(&stale);
    mock_usb_configured=true;dap_request(0x02);step(1522);
    struct link_packet hello=last_packet();assert(hello.type==LINK_HELLO && phase==1);
    struct link_packet ack={0};ack.type=LINK_HELLO_ACK;ack.sequence=hello.sequence;ack.session=0x7777;
    queue_packet(&ack);step(1523);
    assert(session==0x7777 && phase==2 && !fault && link_rejected);
    answer(0,NULL,0);step(1524);
    assert(mock_response_tag==2 && !fault);
}
static void test_generation_discards_inflight_cdc(void)
{
    reset_app();mock_dtr=true;memcpy(mock_usb_rx,"old",3);mock_usb_rx_len=3;
    step(10);assert(cdc_tx_len==3 && phase==2);
    struct link_packet old={0};old.type=LINK_RESPONSE;old.session=session;
    old.sequence=sequence;old.length=5;old.data[0]=WD_CMD_UART;
    old.data[1]=WD_STATUS_OK;old.data[2]=3;old.data[3]=1;old.data[4]='X';
    queue_packet(&old);
    mock_generation=2;mock_dtr=false;mock_usb_configured=false;
    step(11);
    assert(!cdc_tx_len && !phase && !session && !mock_usb_tx_len && link_rejected==1);
    mock_usb_configured=true;mock_dtr=false;step(12);
    mock_dtr=true;step(20);
    struct link_packet hello=last_packet();assert(hello.type==LINK_HELLO);
    struct link_packet ack={0};ack.type=LINK_HELLO_ACK;ack.sequence=hello.sequence;ack.session=0x8888;
    queue_packet(&ack);step(21);
    struct link_packet fresh=last_packet();
    assert(fresh.type==LINK_REQUEST && fresh.session==0x8888 && fresh.data[9]==0);
    assert(!mock_usb_tx_len); /* Neither old target RX nor stale TX suffix crosses reset. */
}
static void test_cdc_credit_and_closing_rx_drain(void)
{
    reset_app();mock_dtr=true;mock_usb_tx_busy=true;
    uint8_t bytes[54];for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)i;
    for(unsigned batch=1;batch<=4;++batch) {
        step(batch*10);assert(!(request[8]&2));
        answer(0,bytes,sizeof(bytes));step(batch*10+1);
    }
    assert(cdc_rx_count==216 && !fault);
    step(50);assert(request[8]==3); /* Host ring has <54 free bytes: advertise zero RX credit. */
    answer(0,NULL,0);step(51);assert(cdc_rx_count==216 && !phase);
    dap_request(0x02);step(52);assert(phase==2 && owner==0); /* Full CDC queue does not consume DAP credit. */
    reset_app();assert(uart_bridge_enable());
    for(unsigned i=0;i<70;++i)mock_uart_rx[i]=(uint8_t)i;
    mock_uart_rx_len=mock_uart.rx_count=70;
    uint8_t data[64],out[64];uart_payload(data,3,NULL);
    assert(dispatch(data,10,out,sizeof(out))==4 && out[3]==0 && mock_uart.rx_count==70);
    uart_payload(data,2,NULL); /* Disabled with RX paused still reports data waiting. */
    assert(dispatch(data,10,out,sizeof(out))==4 && out[1]==WD_STATUS_BUSY && mock_uart.rx_count==70);
    uart_payload(data,0,NULL);
    assert(dispatch(data,10,out,sizeof(out))==58 && out[1]==WD_STATUS_BUSY && out[3]==54);
    for(unsigned i=0;i<54;++i)assert(out[4+i]==i);
    assert(dispatch(data,10,out,sizeof(out))==20 && out[1]==WD_STATUS_OK && out[3]==16);
    for(unsigned i=0;i<16;++i)assert(out[4+i]==54+i);
    assert(!mock_uart.rx_count && !mock_uart.enabled);
}
static void test_short_bulk_control_response(void)
{
    reset_app();mock_dap_request[0]=WD_CMD_LOCAL;mock_dap_request_len=1;
    step(10);assert(phase==3 && response_len==3 && response[2]==WD_STATUS_INVALID);
    step(11);assert(mock_dap_response_len==3 && mock_dap_response[0]==WD_CMD_LOCAL);
    uint8_t command=WD_CMD_REMOTE,out[64];
    assert(dispatch(&command,1,out,sizeof(out))==3 && out[2]==WD_STATUS_INVALID);
}
/* Synthesized errors follow command-specific CMSIS-DAP layouts, including
 * the 16-bit TransferBlock count and the SWD_Sequence status byte. */
static void test_timeout_reply_shapes(void)
{
    static const uint8_t command[]={0x06,0x1d,0x05,0x0a,0x10,0x02};
    static const uint8_t expected[][4]={ {0x06,0x00,0x00,0x08}, {0x1d,0xff}, {0x05,0x00,0x08},
                                        {0x0a,0xff,0x00}, {0x10,0x80}, {0x02,0x00} };
    static const uint8_t length[]={4,2,3,3,2,2};
    for(unsigned i=0;i<sizeof(command);++i) {
        reset_app();dap_request(command[i]);step(10);assert(phase==2);
        step(1510);assert(fault && phase==3 && link_timeouts==1);
        step(1511);
        assert(mock_dap_response_len==length[i]);
        assert(!memcmp(mock_dap_response,expected[i],length[i]));
        assert(mock_response_tag==mock_generation);
    }
}
/* A saturated RTO makes retry_delay() a flat 400 ms, which fits only four
 * transmissions inside REQUEST_TIMEOUT_MS; four consecutive lost frames then
 * latched the global fault in the field. host_poll() now caps each interval so
 * RETRY_ATTEMPTS_IN_BUDGET more transmissions still fit the same deadline. */
static void test_retry_budget_survives_saturated_rto(void)
{
    reset_app();
    retry.rto=400;retry.srtt=300;retry.variance=25; /* measured field saturation */
    dap_request(0x05);step(10);assert(phase==2);
    unsigned sends=0;
    for(uint32_t now=11;now<1500;now+=10) {
        unsigned before=mock_rf_tx_count;
        step(now);
        if(mock_rf_tx_count!=before) ++sends;
    }
    assert(!fault);
    assert(sends>=12); /* 3 retransmissions before the clamp, ~15 after */
}
static void test_explicit_reconnect_after_unknown_write(void)
{
    reset_app();dap_request(0x05);step(10);
    struct link_packet old=last_packet();step(1510);step(1511);
    assert(fault && !phase);
    unsigned sends=mock_rf_tx_count;
    dap_request(0x05);step(1512);step(1513);
    assert(fault && sends==mock_rf_tx_count); /* No implicit replay. */
    dap_request(0x02);mock_dap_request[1]=1;step(1514);
    struct link_packet hello=last_packet();
    assert(fault && reconnecting && phase==1 && hello.type==LINK_HELLO && !session);
    struct link_packet stale={0};stale.type=LINK_RESPONSE;stale.session=old.session;
    stale.sequence=old.sequence;stale.length=2;stale.data[0]=0x05;queue_packet(&stale);
    struct link_packet ack={0};ack.type=LINK_HELLO_ACK;ack.sequence=hello.sequence;ack.session=0x8888;
    queue_packet(&ack);step(1515);
    struct link_packet fresh=last_packet();
    assert(fresh.type==LINK_REQUEST && fresh.data[0]==0x02 && fresh.data[1]==1);
    assert(fresh.session==0x8888 && fresh.sequence==1 && link_rejected);
    answer(0,NULL,0);step(1516);assert(!fault && phase==0 && reconnects==1);
    /* Same-millisecond reconnects cannot obtain the old HELLO's cache. */
    uint32_t first_nonce=nonce;uint8_t cmd[]={WD_CMD_LOCAL,WD_CTL_RECONNECT},out[64];
    assert(control(cmd,sizeof(cmd),out,sizeof(out),true)==3 && out[2]==WD_STATUS_OK);
    begin_request(1516);assert(nonce!=first_nonce);
    struct link_packet late_ack=ack;queue_packet(&late_ack);step(1517);
    assert(phase==1 && !session);
}
static void test_reconnect_control_and_cdc_quarantine(void)
{
    reset_app();mock_dtr=true;memcpy(mock_usb_rx,"old",3);mock_usb_rx_len=3;
    step(10);assert(cdc_tx_len==3 && owner==1);step(1510);step(1511);
    assert(fault && cdc_quarantined && !cdc_tx_len);
    uint8_t cmd[]={WD_CMD_LOCAL,WD_CTL_RECONNECT},out[64];
    assert(control(cmd,2,out,64,true)==3 && out[2]==WD_STATUS_BUSY && fault);
    assert(control(cmd,2,out,64,false)==3 && out[2]==WD_STATUS_DENIED && fault);
    dap_request(0x02);mock_dap_request[1]=1;step(1512);
    struct link_packet hello=last_packet(),ack={0};
    ack.type=LINK_HELLO_ACK;ack.sequence=hello.sequence;ack.session=0x9999;queue_packet(&ack);
    step(1513);answer(0,NULL,0);step(1514);
    unsigned sent=mock_rf_tx_count;step(1520);
    assert(cdc_quarantined && mock_rf_tx_count==sent); /* High DTR cannot auto-resume old stream. */
    memcpy(mock_usb_rx,"tail",4);mock_usb_rx_len=4;mock_usb_rx_pos=0;mock_dtr=false;
    step(1521);assert(!cdc_quarantined && mock_usb_rx_pos==4);
    mock_dtr=true;step(1530);assert(last_packet().data[9]==0);
    reset_app();fault=true;session=0x1234;cdc_tx_len=3;
    assert(control(cmd,2,out,64,true)==3 && out[2]==WD_STATUS_OK);
    assert(!fault && !session && sequence==1 && !cdc_tx_len && reconnects==1);
}
static void test_slave_response_expiry_keeps_cached_write(void)
{
    reset_app();role=RF_CONFIG_ROLE_SLAVE;
    struct link_packet hello={0};hello.type=LINK_HELLO;hello.sequence=77;queue_packet(&hello);
    mock_now=100;slave_poll(100);slave_poll(101);assert(!pending_response);
    struct link_packet write={0};write.type=LINK_REQUEST;write.session=slave.session;write.sequence=1;
    uart_payload(write.data,1,"once");write.length=14;queue_packet(&write);
    mock_rf_send_ok=false;mock_now=110;slave_poll(110);
    assert(pending_response && mock_uart_tx_len==4 && slave.have_cache);
    uint8_t cached[78];memcpy(cached,slave.last_response,78);
    slave_poll(209);assert(pending_response && !response_expirations);
    queue_packet(&write);mock_now=210;slave_poll(210);
    assert(response_expirations==1 && pending_response && mock_uart_tx_len==4);
    assert(!memcmp(cached,slave.last_response,78) && slave.next_sequence==2);
    mock_rf_send_ok=true;slave_poll(211);assert(!pending_response);
    assert(!memcmp(cached,mock_rf_tx[mock_rf_tx_count-1],78));
    /* Expiry also releases HELLO reception, without resetting a cached write early. */
    pending_response=true;response_started=UINT32_MAX-49;mock_rf_send_ok=false;
    hello.sequence=78;queue_packet(&hello);mock_now=50;slave_poll(50);
    assert(response_expirations==2 && slave.hello_nonce==78 && pending_response);
}
static void test_host_restart_nonce_collision(void)
{
    reset_app();struct link_slave peer;link_slave_init(&peer);
    uint8_t frame[78],reply[78];struct link_packet p={0};
    p.type=LINK_HELLO;p.sequence=1;assert(link_encode(&p,frame));
    assert(link_slave_handle(&peer,frame,reply,0x1111,dispatch,reset_dispatch));
    p.type=LINK_REQUEST;p.session=peer.session;p.sequence=1;p.length=2;p.data[0]=0x02;p.data[1]=1;
    assert(link_encode(&p,frame));assert(link_slave_handle(&peer,frame,reply,0,dispatch,reset_dispatch));
    unsigned executions=mock_dap_calls;
    session=0;nonce=0;dap_request(0x02);mock_dap_request[1]=1;step(10);
    assert(last_packet().sequence==1); /* Repeated boot seed. */
    assert(link_slave_handle(&peer,mock_rf_tx[mock_rf_tx_count-1],reply,0x2222,dispatch,reset_dispatch));
    assert(mock_rf_rx_head<32);memcpy(mock_rf_rx[mock_rf_rx_head++],reply,78);step(11);
    assert(phase==1 && nonce==2 && !session && started==10 && mock_dap_calls==executions);
    assert(link_slave_handle(&peer,mock_rf_tx[mock_rf_tx_count-1],reply,0x2222,dispatch,reset_dispatch));
    memcpy(mock_rf_rx[mock_rf_rx_head++],reply,78);step(12);
    assert(phase==2 && session==0x2222 && last_packet().sequence==1 && last_packet().data[0]==0x02);
    assert(mock_dap_calls==executions); /* No request dispatched until fresh handshake. */
}
static void test_generation_discards_unread_usb_suffix(void)
{
    reset_app();mock_dtr=true;memset(mock_usb_rx,'S',64);mock_usb_rx_len=64;
    step(10);assert(cdc_tx_len==54 && mock_usb_rx_pos==54);
    mock_generation=2;step(11);
    assert(cdc_quarantined && mock_usb_rx_pos==64 && !cdc_tx_len);
    unsigned sent=mock_rf_tx_count;step(20);assert(mock_rf_tx_count==sent);
    mock_dtr=false;step(21);assert(!cdc_quarantined);
    mock_dtr=true;step(30);assert(last_packet().type==LINK_HELLO);
    struct link_packet ack={0};ack.type=LINK_HELLO_ACK;ack.sequence=nonce;ack.session=0x5555;
    queue_packet(&ack);step(31);assert(last_packet().data[9]==0);
    uint8_t cmd[]={WD_CMD_LOCAL,WD_CTL_RECOVERY_STATS},out[64];
    assert(control(cmd,2,out,64,true)==38 && out[3]==1);
}
static void test_failed_reconnect_keeps_fault(void)
{
    for(unsigned fail_phase=1;fail_phase<=3;++fail_phase) {
        reset_app();fault=true;dap_request(0x02);mock_dap_request[1]=1;step(10);
        assert(fault && reconnecting && phase==1);
        if(fail_phase>=2) {
            struct link_packet ack={0};ack.type=LINK_HELLO_ACK;ack.sequence=nonce;ack.session=0x7777;
            queue_packet(&ack);step(11);assert(phase==2 && fault);
        }
        if(fail_phase==3) {
            struct link_packet bad={0};bad.type=LINK_RESPONSE;bad.session=session;bad.sequence=sequence;
            bad.length=2;bad.data[0]=0x02;bad.data[1]=0;queue_packet(&bad);step(12);
        } else {step(fail_phase==1?1510:1511);step(1512);}
        assert(fault && !reconnecting && !phase);
        unsigned sent=mock_rf_tx_count;dap_request(0x05);step(1520);
        assert(fault && sent==mock_rf_tx_count);
    }
    reset_app();mock_dtr=false;memset(mock_usb_rx,'Q',10);mock_usb_rx_len=10;
    mock_generation=2;step(10);assert(mock_usb_rx_pos==10); /* No app stream state, but EP3 had data. */
}
int main(void)
{
    test_real_main_startup();test_shared_dap_cdc_sequence();
    test_cached_uart_reply_and_busy_configuration();test_partial_tx_and_dtr_drain();
    test_cdc_rx_backpressure();test_cdc_credit_and_closing_rx_drain();
    test_short_bulk_control_response();test_invalid_settings_and_pending_close();
    test_timeout_reply_shapes();test_retry_budget_survives_saturated_rto();
    test_timeout_fault_lock_and_generation();test_generation_discards_inflight_cdc();
    test_explicit_reconnect_after_unknown_write();test_reconnect_control_and_cdc_quarantine();
    test_slave_response_expiry_keeps_cached_write();
    test_host_restart_nonce_collision();test_generation_discards_unread_usb_suffix();
    test_failed_reconnect_keeps_fault();
    puts("App tests passed: real startup, shared DAP/CDC sequence, cached UART replies, partial TX, DTR drain, RX backpressure, invalid coding, timeout lock, timeout reply shapes, retry budget, generation isolation");
    return 0;
}
