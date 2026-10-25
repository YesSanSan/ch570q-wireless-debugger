#include "CH57x_common.h"
#include "link.h"
#include "uart_log.h"
#include "board.h"
#include "platform.h"
#if WD_ROLE_HOST
#include "usb_dap.h"
#else
#include "dap.h"
#endif

/* Poll a free-running core-clock counter. Unsigned differences tolerate wrap. */
#define CYCLES_PER_MS (BOARD_SYS_CLOCK_HZ / 1000u)
#define RETRY_CYCLES (100u * CYCLES_PER_MS)
#define TIMEOUT_CYCLES (1500u * CYCLES_PER_MS)
volatile uint32_t heartbeat, link_timeouts, link_retries, link_rejected;
static uint8_t incoming[RADIO_FRAME_SIZE], outgoing[RADIO_FRAME_SIZE];

static void clock_init(void)
{
    HSECFG_Capacitance(BOARD_HSE_CAPACITANCE);
    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
    SysTick->CTLR = 0;
    SysTick->CNTL = 0;
    SysTick->CMPL = 0xffffffffu;
    SysTick->CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK | SysTick_CTLR_STRE;
}

#if WD_ROLE_HOST
static uint8_t request[64], reply[64], request_length, reply_length;
static struct link_packet packet;
static uint32_t session, sequence = 1, nonce, generation, started, last_tx;
static uint8_t phase; /* 0 idle, 1 hello, 2 request, 3 USB reply */
static bool fault;

static void fail_request(bool uncertain)
{
    memset(reply, 0, sizeof(reply));
    reply_length = 2;
    reply[0] = request[0];
    reply[1] = 0xff;
    if (request[0] == 0x00 || request[0] == 0x02) reply[1] = 0;
    if (request[0] == 0x05) { /* DAP_Transfer: zero completed, protocol error. */
        reply[1] = 0; reply[2] = 8; reply_length = 3;
    }
    if (request[0] == 0x06) {
        reply[1] = reply[2] = 0; reply[3] = 8; reply_length = 4;
    }
    if (request[0] == 0x0a) { reply[2] = 0; reply_length = 3; }
    if (request[0] == 0x10) reply[1] = 0x80; /* Absent reset remains deasserted. */
    fault |= uncertain;
    phase = 3;
}
static void queue_request(uint32_t now)
{
    memset(&packet, 0, sizeof(packet));
    packet.type = LINK_REQUEST; packet.session = session; packet.sequence = sequence;
    packet.length = request_length;
    memcpy(packet.data, request, request_length);
    link_encode(&packet, outgoing);
    phase = 2; started = now; last_tx = now - RETRY_CYCLES;
}
static void host_poll(uint32_t now)
{
    usb_dap_poll();
    uint32_t current_generation = usb_dap_generation();
    if (generation != current_generation) {
        generation = current_generation;
        phase = 0; session = 0; sequence = 1; fault = false;
    }
    usb_dap_ack_generation(generation);
    while (radio_receive(incoming)) {
        struct link_packet received;
        if (!link_decode(incoming, &received)) { ++link_rejected; continue; }
        if (phase == 1 && received.type == LINK_HELLO_ACK && received.sequence == nonce &&
            received.session && !received.length) {
            session = received.session;
            queue_request(now);
        } else if (phase == 2 && received.type == LINK_RESPONSE &&
            received.session == session && received.sequence == sequence && received.length &&
            (received.data[0] == request[0] || received.data[0] == 0xff)) {
            reply_length = received.length;
            memcpy(reply, received.data, reply_length);
            ++sequence;
            if (!sequence) { session = 0; sequence = 1; }
            phase = 3;
        } else ++link_rejected;
    }
    if (!usb_dap_configured()) return;
    uint32_t request_generation;
    if (!phase && usb_dap_take_request_tagged(request, &request_length, &request_generation)) {
        if (generation != request_generation) {
            generation = request_generation;
            session = 0; sequence = 1; fault = false;
        }
        usb_dap_ack_generation(generation);
        if (request_length && request[0] == 0x07) {
            /* CMSIS-DAP TransferAbort has no response. The one-credit transport
             * cannot preempt an active command; physical retries are bounded. */
            usb_dap_discard_request_tagged(generation);
            return;
        }
        if (!request_length || fault) {
            fail_request(false);
        } else if (session) {
            queue_request(now);
        } else {
            memset(&packet, 0, sizeof(packet));
            /* Fresh hello nonce for this USB request; not a security nonce. */
            nonce = now ^ (generation * 0x9e3779b9u);
            if (!nonce) nonce = 1;
            packet.type = LINK_HELLO; packet.sequence = nonce;
            link_encode(&packet, outgoing);
            phase = 1; started = now; last_tx = now - RETRY_CYCLES;
        }
    }
    if (phase == 1 || phase == 2) {
        if ((uint32_t)(now - started) >= TIMEOUT_CYCLES) {
            ++link_timeouts;
            /* A lost write response is ambiguous. Require USB re-enumeration;
             * never silently start a new RF session and replay target writes. */
            fail_request(phase == 2);
        } else if ((uint32_t)(now - last_tx) >= RETRY_CYCLES && radio_send(outgoing)) {
            last_tx = now;
            ++link_retries;
        }
    }
    if (phase == 3 && usb_dap_send_response_tagged(reply, reply_length, generation)) phase = 0;
}
#else
static struct link_slave slave;
static bool response_pending;
static void slave_poll(uint32_t now)
{
    if (response_pending) {
        if (radio_send(outgoing)) response_pending = false;
        return;
    }
    if (radio_receive(incoming)) {
        if (link_slave_handle(&slave, incoming, outgoing, now, dap_process, dap_init))
            response_pending = true;
        else ++link_rejected;
    }
}
#endif

int main(void)
{
    clock_init();
    uart_log_init();
    uart_log_event(UART_LOG_BOOT, WD_ROLE_HOST, BOARD_SYS_CLOCK_HZ);
    uart_log_poll();
#if WD_ROLE_HOST
    usb_dap_init();
#else
    dap_init();
    link_slave_init(&slave);
#endif
    uart_log_event(UART_LOG_RF_INIT, 0, 0);
    radio_init();
    uart_log_event(UART_LOG_RF_INIT, 1, 0);
    uint32_t last_heartbeat = SysTick->CNTL;
    for (;;) {
        radio_poll(platform_millis());
        uart_log_poll();
        uint32_t now = SysTick->CNTL;
#if WD_ROLE_HOST
        host_poll(now);
#else
        slave_poll(now);
#endif
        if ((uint32_t)(now - last_heartbeat) >= 1000u * CYCLES_PER_MS) {
            ++heartbeat;
            last_heartbeat = now;
            uart_log_event(UART_LOG_HEARTBEAT, heartbeat, link_timeouts);
        }
    }
}
