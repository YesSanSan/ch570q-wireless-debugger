/* Extended unified firmware. Legacy minimal images remain in main.c. */
#include "CH57x_common.h"
#include "board.h"
#include "platform.h"
#include "usb_dap.h"
#include "dap.h"
#include "swd.h"
#include "link.h"
#include "radio.h"
#include "rf_config.h"
#include "uart_bridge.h"
#include "uart_log.h"
#include "retry.h"
#include "control.h"
#include <string.h>

#ifndef WD_ROLE_POLICY
#define WD_ROLE_POLICY RF_CONFIG_ROLE_AUTO
#endif
#define REQUEST_TIMEOUT_MS 1500u
/* Transmissions that must still fit inside the remaining budget when the retry
 * interval is capped. See the retry clamp in host_poll(). */
#define RETRY_ATTEMPTS_IN_BUDGET 16u
#define DISCOVERY_MS 1000u
#define UART_INTERVAL_MS 10u
static rf_config_t settings;
static uint8_t hardware_id[8], peers[4][8], peer_count;
static bool identity_valid;
static uint8_t role, phase, owner; /* owner: 0 DAP, 1 CDC stream */
static uint8_t request[64], response[64], request_len, response_len;
static uint8_t incoming[78], outgoing[78];
static uint32_t generation, session, sequence = 1, nonce, started, last_tx;
static uint32_t last_activity, last_uart, discovery_start, discovery_nonce;
static uint32_t discovery_due, discovery_reply_nonce, discovery_round;
static uint32_t link_timeouts, link_retries, link_rejected, completed;
static bool fault, pending_response, discover_active, discover_reply_pending;
static bool apply_pending;
static uint32_t apply_after;
static struct link_slave slave;
static struct retry_state retry;
static uint8_t cdc_tx[54], cdc_tx_len;
static uint8_t cdc_rx[256];
static uint16_t cdc_rx_head, cdc_rx_tail, cdc_rx_count;
static usb_cdc_line_coding_t cdc_line;
static bool cdc_open;
static uint8_t previous_uart_enabled;
static uint32_t last_stream;

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void put32(uint8_t *p, uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i)); }
static bool nonzero(const uint8_t *p, unsigned n) {
    uint8_t x=0; while(n--) x|=*p++; return x!=0;
}
static uint8_t config_status(rf_config_status_t s) {
    if(s==RF_CONFIG_OK) return WD_STATUS_OK;
    if(s==RF_CONFIG_BUSY) return WD_STATUS_BUSY;
    if(s==RF_CONFIG_UNSUPPORTED) return WD_STATUS_UNSUPPORTED;
    if(s==RF_CONFIG_INVALID) return WD_STATUS_INVALID;
    return WD_STATUS_IO;
}
static void stream_reset(void) {
    cdc_tx_len=0; cdc_open=false; previous_uart_enabled=0;
    cdc_rx_head=cdc_rx_tail=cdc_rx_count=0;
}
/* RF settings are staged until APPLY (500 ms grace); saved role changes
 * take effect at reboot. ACKs always leave on the previous RF parameters. */
static size_t control(const uint8_t *p, size_t n, uint8_t *out, size_t cap, bool local) {
    if(!p || !n || !out || cap<64) return 0;
    memset(out,0,cap);out[0]=p[0];
    if(n<2) {out[1]=0xff;out[2]=WD_STATUS_INVALID;return 3;}
    out[1]=p[1];out[2]=WD_STATUS_OK;
    uart_bridge_snapshot_t bridge;uart_bridge_get_snapshot(&bridge);
    switch(p[1]) {
    case WD_CTL_INFO:
        out[3]=2;out[4]=role;out[5]=settings.role;
        out[6]=identity_valid;out[7]=0; /* reserved: resident loader removed 2026-10-24 */
        memcpy(out+8,hardware_id,8);out[16]=RF_CONFIG_PERSISTENCE_SUPPORTED;
        out[17]=fault;out[18]=peer_count;return 19;
    case WD_CTL_STATS: {
        radio_stats_t stats; radio_get_stats(&stats);
        const uint32_t values[]={stats.rx_count,stats.tx_count,stats.crc_errors,stats.drops,
            stats.tx_failures,stats.tx_timeouts,link_retries,link_timeouts,link_rejected,
            completed,retry.srtt,retry.rto};
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) put32(out+3+4*i,values[i]);
        return 3+sizeof(values);
    }
    case WD_CTL_DISCOVER:
        if(!local || fault || cdc_open) out[2]=WD_STATUS_BUSY;
        else {peer_count=0;discover_active=true;discovery_start=platform_millis();
            discovery_nonce=discovery_start ^ get32(hardware_id) ^ 0x44534356u;
            if(!discovery_nonce) discovery_nonce=1;
            last_tx=discovery_start-100;session=0;sequence=1;}
        break;
    case WD_CTL_DEVICES:
        out[3]=discover_active;out[4]=peer_count;memcpy(out+5,peers,8*peer_count);return 5+8*peer_count;
    case WD_CTL_GET_CONFIG:
        rf_config_encode(&settings,out+3,RF_CONFIG_RECORD_SIZE);return 3+RF_CONFIG_RECORD_SIZE;
    case WD_CTL_SET_CONFIG: {
        rf_config_t proposed;
        if(n<2+RF_CONFIG_RECORD_SIZE || !rf_config_decode(&proposed,p+2,RF_CONFIG_RECORD_SIZE)) out[2]=WD_STATUS_INVALID;
        else settings=proposed;
        break;
    }
    case WD_CTL_SAVE_CONFIG:
        if(cdc_open || bridge.enabled || bridge.enable_pending) out[2]=WD_STATUS_BUSY;
        else out[2]=config_status(rf_config_save(&settings));
        break;
    case WD_CTL_PAIR:
        if(!local || n<10 || !nonzero(p+2,8) || cdc_open || fault) out[2]=WD_STATUS_INVALID;
        else {memcpy(settings.peer_id,p+2,8);session=0;sequence=1;}
        break;
    case WD_CTL_ROLE:
        if(n<3 || p[2]>RF_CONFIG_ROLE_SLAVE) out[2]=WD_STATUS_INVALID;
        else settings.role=p[2];
        break;
    case WD_CTL_IDLE:
        if(n<6 || get32(p+2)>RF_CONFIG_MAX_IDLE_TIMEOUT_MS) out[2]=WD_STATUS_INVALID;
        else settings.idle_timeout_ms=get32(p+2);
        break;
    case WD_CTL_APPLY:
        if(cdc_open || bridge.enabled || bridge.enable_pending || fault) out[2]=WD_STATUS_BUSY;
        else {apply_pending=true;apply_after=platform_millis()+500;}
        break;
    case WD_CTL_UART_STATS: {
        uart_bridge_snapshot_t state;uart_bridge_get_snapshot(&state);
        const uint32_t values[]={state.rx_received,state.rx_read,state.rx_overflow,state.rx_discarded,
            state.tx_accepted,state.tx_sent,state.overrun_errors,state.parity_errors,state.framing_errors,
            state.break_errors,state.actual_baud};
        out[3]=state.enabled;out[4]=state.enable_pending;
        for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) put32(out+5+4*i,values[i]);
        return 5+sizeof(values);
    }
    default:out[2]=WD_STATUS_UNSUPPORTED;break;
    }
    return 3;
}
static size_t dispatch(const uint8_t *p,size_t n,uint8_t *out,size_t cap) {
    if(!n) return 0;
    if(p[0]==WD_CMD_REMOTE) return control(p,n,out,cap,false);
    if(p[0]==WD_CMD_LOCAL) {out[0]=p[0];out[1]=0xff;return 2;}
    if(p[0]==WD_CMD_UART) {
        memset(out,0,cap);
        if(n<10 || p[9]>54 || n<10u+p[9]) {out[0]=WD_CMD_UART;out[1]=WD_STATUS_INVALID;return 4;}
        out[0]=WD_CMD_UART;out[1]=WD_STATUS_OK;
        last_stream=platform_millis();
        if(p[8]>3 || ((p[8]&1) && !uart_bridge_validate(get32(p+1),p[5],p[6],p[7]))) {
            out[1]=WD_STATUS_INVALID;return 4;
        }
        if(!(p[8]&1)) {
            if(!uart_bridge_disable()) out[1]=WD_STATUS_BUSY;
            else previous_uart_enabled=0;
        } else {
            uart_bridge_snapshot_t state;uart_bridge_get_snapshot(&state);
            bool same=state.baud==get32(p+1) && state.stop==p[5] && state.parity==p[6] && state.bits==p[7];
            if(!same && !uart_bridge_configure(get32(p+1),p[5],p[6],p[7])) out[1]=WD_STATUS_BUSY;
            else if(!uart_bridge_enable()) out[1]=WD_STATUS_BUSY;
            else {
                previous_uart_enabled=1;
                out[2]=(uint8_t)uart_bridge_write(p+10,p[9]);
            }
        }
        /* Retained by link cache even if this response is lost. */
        if(!(p[8]&2)) out[3]=(uint8_t)uart_bridge_read(out+4,54);
        uart_bridge_snapshot_t remaining;uart_bridge_get_snapshot(&remaining);
        if(!(p[8]&1) && remaining.rx_count) out[1]=WD_STATUS_BUSY;
        return 4+out[3];
    }
    return dap_process(p,n,out,cap);
}
static void reset_dispatch(void) {
    dap_init();
    /* UART closure is cooperative; no FIFO flush or duplicated target bytes. */
}
/* A timed-out request may already have executed, so the reply must not invite a
 * blind replay. It must still have the exact response shape of its command: the
 * host parses the short packet by command, and a malformed one desynchronises
 * it instead of failing it. DAP_TransferBlock used to answer
 * {0x06, count=0, response=0x00} which reads as "OK, zero words transferred",
 * so a host streaming a framebuffer with 0x06 could spin on a false success
 * rather than fail. 0x08 is DAP_TRANSFER_PROTOCOL_ERROR. */
static void fail_request(bool uncertain) {
    memset(response,0,sizeof(response));response_len=2;response[0]=request[0];response[1]=0xff;
    switch(request[0]) {
    case 0x00: /* DAP_Info: acknowledge with a zero-length info field. */
    case 0x02: /* DAP_Connect: report no port connected. */
        response[1]=0;break;
    case 0x05: /* DAP_Transfer: TransferCount 0, TransferResponse error. */
    case 0x06: /* DAP_TransferBlock: identical shape, never a false OK. */
    case 0x1d: /* DAP_SWD_Sequence: SequenceCount 0, SequenceResponse error. */
        response[1]=0;response[2]=8;response_len=3;break;
    case 0x0a: /* DAP_ResetTarget: failed, Execute 0. */
        response[2]=0;response_len=3;break;
    case 0x10: /* DAP_SWJ_Pins: report the pins as unavailable. */
        response[1]=0x80;break;
    default: break;
    }
    fault|=uncertain;phase=3;
}
static void queue_request(uint32_t now) {
    struct link_packet p={0};p.type=LINK_REQUEST;p.session=session;p.sequence=sequence;
    p.length=request_len;memcpy(p.data,request,request_len);link_encode(&p,outgoing);
    phase=2;started=now;last_tx=now-retry.rto;retry_begin(&retry);
}
static void begin_request(uint32_t now) {
    if(session) {queue_request(now);return;}
    struct link_packet p={0};
    nonce=now ^ (generation*0x9e3779b9u) ^ get32(hardware_id);
    if(!nonce) nonce=1;
    p.type=LINK_HELLO;p.sequence=nonce;
    if(nonzero(settings.peer_id,8)) {p.length=8;memcpy(p.data,settings.peer_id,8);}
    link_encode(&p,outgoing);phase=1;started=now;last_tx=now-retry.rto;retry_begin(&retry);
}
static void host_receive(uint32_t now) {
    while(radio_receive(incoming)) {
        struct link_packet p;
        if(!link_decode(incoming,&p)) {++link_rejected;continue;}
        if(discover_active && p.type==LINK_DISCOVER_REPLY && p.sequence==discovery_nonce &&
            p.length==8 && !p.session && nonzero(p.data,8)) {
            unsigned i;for(i=0;i<peer_count;i++) if(!memcmp(peers[i],p.data,8)) break;
            if(i==peer_count && peer_count<4) memcpy(peers[peer_count++],p.data,8);
        } else if(phase==1 && p.type==LINK_HELLO_ACK && p.sequence==nonce && p.session && !p.length) {
            retry_complete(&retry,now);session=p.session;queue_request(now);
        } else if(phase==2 && p.type==LINK_RESPONSE && p.session==session && p.sequence==sequence &&
            p.length && (p.data[0]==request[0] || p.data[0]==0xff)) {
            retry_complete(&retry,now);response_len=p.length;memcpy(response,p.data,p.length);
            ++completed;++sequence;if(!sequence) {session=0;sequence=1;} phase=3;last_activity=now;
        } else ++link_rejected;
    }
}
static void host_discover(uint32_t now) {
    if(!discover_active) return;
    if(now-discovery_start>=DISCOVERY_MS) {discover_active=false;return;}
    if(now-last_tx>=100) {
        struct link_packet p={0};p.type=LINK_DISCOVER;p.sequence=discovery_nonce;
        uint8_t frame[78];link_encode(&p,frame);
        if(radio_send(frame)) last_tx=now;
    }
}
static bool cdc_request(uint32_t now) {
    usb_cdc_get_line_coding(&cdc_line);
    bool opened=(usb_cdc_control_line_state()&1)!=0;
    if(fault || (apply_pending && !cdc_open && !previous_uart_enabled) ||
        (!opened && !cdc_open && !previous_uart_enabled)) return false;
    if(now-last_uart<UART_INTERVAL_MS) return false;
    cdc_open=opened;
    if(!cdc_tx_len && opened) cdc_tx_len=(uint8_t)usb_cdc_read(cdc_tx,sizeof(cdc_tx));
    memset(request,0,sizeof(request));request[0]=WD_CMD_UART;put32(request+1,cdc_line.baud_rate);
    request[5]=cdc_line.stop_bits;request[6]=cdc_line.parity;request[7]=cdc_line.data_bits;
    request[8]=(opened || cdc_tx_len)?1:0;
    if(sizeof(cdc_rx)-cdc_rx_count<54) request[8]|=2; /* Receiver credit zero. */
    request[9]=cdc_tx_len;
    memcpy(request+10,cdc_tx,cdc_tx_len);
    request_len=10+request[9];owner=1;last_uart=now;begin_request(now);return true;
}
static void host_poll(uint32_t now) {
    usb_dap_poll();
    uint32_t current=usb_dap_generation();
    if(generation!=current) {
        generation=current;session=0;sequence=1;phase=0;fault=false;
        discover_active=false;stream_reset();
    }
    host_receive(now);
    if(!usb_dap_configured()) return;
    if(cdc_rx_count) {
        unsigned n=sizeof(cdc_rx)-cdc_rx_tail;
        if(n>cdc_rx_count) n=cdc_rx_count;
        if(n>64) n=64;
        unsigned sent=usb_cdc_write(cdc_rx+cdc_rx_tail,n);
        cdc_rx_tail=(cdc_rx_tail+sent)%sizeof(cdc_rx);cdc_rx_count-=sent;
    }
    if(phase==3) {
        if(!owner) {if(usb_dap_send_response_tagged(response,response_len,generation)) phase=0;}
        else if(response[0]!=WD_CMD_UART || response_len<4 || fault) {
            usb_cdc_set_serial_state(0x40);cdc_tx_len=0;phase=0;
        } else {
            unsigned count=response[3];
            if(count>54 || response_len<4+count || response[2]>cdc_tx_len || count>sizeof(cdc_rx)-cdc_rx_count) {
                fault=true;usb_cdc_set_serial_state(0x40);phase=0;
            } else {
                for(unsigned i=0;i<count;i++) {cdc_rx[cdc_rx_head]=response[4+i];cdc_rx_head=(cdc_rx_head+1)%sizeof(cdc_rx);}
                cdc_rx_count+=count;
                unsigned used=response[2];
                memmove(cdc_tx,cdc_tx+used,cdc_tx_len-used);cdc_tx_len-=used;
                if(response[1]==WD_STATUS_OK) previous_uart_enabled=request[8]&1;
                usb_cdc_set_serial_state(response[1]==WD_STATUS_INVALID?0x40:(cdc_open?3:0));phase=0;
            }
        }
    }
    if(!phase) {
        uint32_t tag;
        if(usb_dap_take_request_tagged(request,&request_len,&tag)) {
            if(tag!=generation) {generation=tag;session=0;sequence=1;fault=false;stream_reset();}
            owner=0;last_activity=now;
            if(request_len && request[0]==7) usb_dap_discard_request_tagged(tag);
            else if(request_len && request[0]==WD_CMD_LOCAL) {
                response_len=(uint8_t)control(request,request_len,response,64,true);phase=3;
            } else if(!request_len || fault || discover_active) fail_request(false);
            else begin_request(now);
        } else if(!discover_active) cdc_request(now);
    }
    host_discover(now);
    if(phase==1 || phase==2) {
        uint32_t elapsed=now-started;
        if(elapsed>=REQUEST_TIMEOUT_MS) {++link_timeouts;fail_request(phase==2);}
        else {
            /* retry_delay() backs off to 16x the RTO and then clamps at 400 ms,
             * so a saturated RTO makes the interval flat 400 ms: only four
             * transmissions (0/400/800/1200) fit inside REQUEST_TIMEOUT_MS and
             * the last one gets a 300 ms window. Measured per-attempt frame loss
             * on this link is ~14%, and that is what latched the global fault in
             * the field. Cap each interval so RETRY_ATTEMPTS_IN_BUDGET more
             * transmissions still fit the remaining budget: the backoff is a
             * network congestion control, while this is a single-outstanding
             * point-to-point ARQ with a hard deadline and a 2-4 ms round trip. */
            uint32_t remaining=REQUEST_TIMEOUT_MS-elapsed;
            uint32_t delay=retry_delay(&retry);
            uint32_t cap=remaining/RETRY_ATTEMPTS_IN_BUDGET;
            if(!cap) cap=1;
            if(delay>cap) delay=cap;
            if(now-last_tx>=delay && radio_send(outgoing)) {
                last_tx=now;if(retry.attempts) ++link_retries;retry_sent(&retry,now);
            }
        }
    }
}
static void slave_poll(uint32_t now) {
    uart_bridge_poll();
    uart_bridge_snapshot_t uart_state;uart_bridge_get_snapshot(&uart_state);
    if((uart_state.enabled || uart_state.enable_pending) && now-last_stream>2000 && uart_bridge_disable()) previous_uart_enabled=0;
    if(pending_response) {
        if(radio_send(outgoing)) pending_response=false;
        return;
    }
    if(discover_reply_pending && (int32_t)(now-discovery_due)>=0) {
        struct link_packet p={0};p.type=LINK_DISCOVER_REPLY;p.sequence=discovery_reply_nonce;
        p.length=8;memcpy(p.data,hardware_id,8);link_encode(&p,outgoing);
        pending_response=true;discover_reply_pending=false;return;
    }
    if(radio_receive(incoming)) {
        struct link_packet p;
        if(!link_decode(incoming,&p)) {++link_rejected;return;}
        if(p.type==LINK_DISCOVER && !p.session && !p.length && identity_valid) {
            /* Stable per-device slots reduce same-channel discovery collisions. */
            discovery_reply_nonce=p.sequence;
            uint32_t jitter=get32(hardware_id)^get32(hardware_id+4)^p.sequence^(++discovery_round*0x9e3779b9u);
            jitter^=jitter>>16;jitter*=0x7feb352du;jitter^=jitter>>15;
            discovery_due=now+1+(jitter%32);
            discover_reply_pending=true;return;
        }
        if(p.type==LINK_HELLO && p.length==8 && (!identity_valid || memcmp(p.data,hardware_id,8))) return;
        if(link_slave_handle(&slave,incoming,outgoing,now^get32(hardware_id),dispatch,reset_dispatch)) {
            pending_response=true;last_activity=now;++completed;
        } else ++link_rejected;
    }
}
#if WD_UART_LOG
/* Debug build only: drain queued records so an early hang still leaves evidence
 * on PA3. One millisecond per call; a no-op in production builds. */
static void log_flush(unsigned ms) { while(ms--) { uart_log_poll(); mDelaymS(1); } }
#else
static void log_flush(unsigned ms) { (void)ms; }
#endif
int main(void) {
    bool usb_seen=false;
    /* Early boot checkpoint, emitted before platform_clock_init() re-arms
     * SysTick: it proves main() was reached and does not depend on a timer. */
    uart_log_init();
    uart_log_event(UART_LOG_BOOT,0xDFu,0);
#if WD_UART_LOG
    for(unsigned i=0;i<40;i++) { uart_log_poll(); for(volatile uint32_t d=0;d<20000u;++d) { } }
#endif
    platform_clock_init();
    uart_log_init();
    uart_log_event(UART_LOG_BOOT,0xE0u|(uint32_t)WD_ROLE_POLICY,BOARD_SYS_CLOCK_HZ);log_flush(16);
    uart_bridge_init();retry_init(&retry);
    uart_log_event(UART_LOG_BOOT,0xE2u,0);log_flush(16);
    rf_config_defaults(&settings);
    rf_config_t saved;
    if(rf_config_load(&saved)==RF_CONFIG_OK) settings=saved;
    identity_valid=radio_get_hardware_id(hardware_id);
    uart_log_event(UART_LOG_BOOT,0xE1u,(uint32_t)settings.role|((uint32_t)settings.channel<<8)|
        ((uint32_t)identity_valid<<16));log_flush(24);
    role=WD_ROLE_POLICY==RF_CONFIG_ROLE_AUTO?settings.role:WD_ROLE_POLICY;
    if(role!=RF_CONFIG_ROLE_SLAVE) {
        usb_dap_init();
        if(role==RF_CONFIG_ROLE_AUTO) {
            uint32_t start=platform_millis();
            while(platform_millis()-start<PLATFORM_USB_PROBE_MS && !usb_dap_bus_seen()) {
                usb_dap_poll();uart_log_poll();
            }
            usb_seen=usb_dap_bus_seen();
            role=usb_seen?RF_CONFIG_ROLE_HOST:RF_CONFIG_ROLE_SLAVE;
        }
    }
    if(role==RF_CONFIG_ROLE_SLAVE) {usb_dap_deinit();dap_init();link_slave_init(&slave);}
    uart_log_event(UART_LOG_BOOT,role==RF_CONFIG_ROLE_HOST?1u:0u,usb_seen?1u:0u);log_flush(24);
    uart_log_event(UART_LOG_RF_INIT,0,0);
    radio_init();radio_reconfigure(&settings);
    uart_log_event(UART_LOG_RF_INIT,1,0);log_flush(24);
    last_activity=platform_millis();
    for(;;) {
        uint32_t now=platform_millis();
        uart_log_poll();
#if WD_UART_LOG
        { static uint32_t beat; if(now-beat>=1000u) { beat=now; uart_log_event(UART_LOG_HEARTBEAT,role,phase); } }
#endif
        if(role==RF_CONFIG_ROLE_HOST) host_poll(now);else slave_poll(now);
        uart_bridge_snapshot_t bridge_state;uart_bridge_get_snapshot(&bridge_state);
        if(apply_pending && (int32_t)(now-apply_after)>=0 && !phase && !pending_response &&
            !cdc_open && !bridge_state.enabled && !bridge_state.enable_pending && radio_is_idle()) {
            if(radio_reconfigure(&settings)) {
                apply_pending=false;session=0;sequence=1;link_slave_init(&slave);
                discover_active=false;discover_reply_pending=false;
            }
        }
        /* UART is polled: never sleep while it owns the peripheral. */
        if(role==RF_CONFIG_ROLE_SLAVE && settings.idle_timeout_ms &&
            now-last_activity>=settings.idle_timeout_ms && !pending_response &&
            !discover_reply_pending && !uart_bridge_is_enabled() && radio_is_idle()) platform_idle();
    }
}
