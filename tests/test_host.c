#include <assert.h>
#include <stdio.h>
#define WD_ROLE_HOST 1
#define main firmware_main
#include "../src/main.c"
#undef main
struct fake_systick fake_systick;
int host_test_hse_cap = -1;
static uint32_t usb_gen=1, tag_gen=1;
static bool configured=true, pending_usb, pending_rf, allow_response=true;
static uint8_t usb_req[64], rf_rx[78], rf_tx[78], usb_reply[64];
static unsigned sends, replies, executions;
void radio_init(void) {}
void radio_poll(uint32_t now) { (void)now; }
uint32_t platform_millis(void) { return 0; }
bool radio_send(const uint8_t p[RADIO_FRAME_SIZE]) { memcpy(rf_tx,p,78);++sends;return true; }
bool radio_receive(uint8_t p[RADIO_FRAME_SIZE]) { if(!pending_rf)return false;memcpy(p,rf_rx,78);pending_rf=false;return true; }
void usb_dap_init(void) {}
void usb_dap_poll(void) {}
bool usb_dap_configured(void) { return configured; }
uint32_t usb_dap_generation(void) { return usb_gen; }
static unsigned discards;
void usb_dap_discard_request(void) { ++discards; }
bool usb_dap_take_request_tagged(uint8_t p[64],uint8_t *n,uint32_t *g) {
    if(!pending_usb)return false;
    memcpy(p,usb_req,64);*n=64;*g=tag_gen;pending_usb=false;return true;
}
bool usb_dap_send_response(const uint8_t *p,uint8_t n) {
    if(!allow_response)return false;
    memset(usb_reply,0,64);memcpy(usb_reply,p,n);++replies;return true;
}
static size_t exec(const uint8_t *p,size_t n,uint8_t *r,size_t cap) {
    (void)n;(void)cap;++executions;r[0]=p[0];r[1]=1;r[2]=1;return 3;
}
static void submit(uint8_t cmd) {memset(usb_req,0,64);usb_req[0]=cmd;pending_usb=true;tag_gen=usb_gen;}
int main(void)
{
    clock_init();
    assert(host_test_hse_cap == HSECap_12p);
    struct link_slave s;link_slave_init(&s);
    submit(5);host_poll(1);assert(phase==1 && sends==1);
    assert(link_slave_handle(&s,rf_tx,rf_rx,77,exec,0));pending_rf=true;
    host_poll(100);assert(phase==2 && sends==2);
    assert(link_slave_handle(&s,rf_tx,rf_rx,77,exec,0));
    /* First SWD reply lost. Retry must return cache, one execution only. */
    host_poll(100+RETRY_CYCLES);assert(sends==3);
    assert(link_slave_handle(&s,rf_tx,rf_rx,77,exec,0));pending_rf=true;
    host_poll(101+RETRY_CYCLES);
    assert(executions==1 && replies==1 && usb_reply[0]==5 && sequence==2 && phase==0);
    submit(5);host_poll(2*RETRY_CYCLES);assert(phase==2);
    unsigned before=sends;
    host_poll(2*RETRY_CYCLES+TIMEOUT_CYCLES);
    assert(fault && replies==2 && usb_reply[1]==0 && usb_reply[2]==8);
    submit(5);host_poll(3*RETRY_CYCLES+TIMEOUT_CYCLES);
    assert(sends==before && replies==3 && fault); /* No unsafe retry after timeout. */
    ++usb_gen;submit(0);host_poll(4*RETRY_CYCLES+TIMEOUT_CYCLES);
    assert(!fault && phase==1 && !session); /* USB reset starts fresh handshake. */
    /* In-flight response from old session cannot complete the new request. */
    struct link_packet old={.type=LINK_RESPONSE,.session=77,.sequence=2,.length=1,.data={5}};
    assert(link_encode(&old,rf_rx));pending_rf=true;
    host_poll(4*RETRY_CYCLES+TIMEOUT_CYCLES+1);assert(phase==1 && replies==3);
    /* A reset BETWEEN reading generation and take is tagged atomically. */
    phase=0;session=99;sequence=12;fault=true;
    submit(0);tag_gen=usb_gen+1;
    host_poll(5*RETRY_CYCLES+TIMEOUT_CYCLES);
    assert(generation==tag_gen && phase==1 && !fault && !session && sequence==1);
    phase=0;session=0;generation=usb_gen;submit(7);
    host_poll(6*RETRY_CYCLES+TIMEOUT_CYCLES);
    assert(discards==1 && replies==3 && phase==0);
    puts("host tests passed: retry/cache integration, timeout write lockout, late response rejection, USB generation race, abort without response");
    return 0;
}
