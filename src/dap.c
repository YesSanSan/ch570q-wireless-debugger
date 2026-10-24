#include "dap.h"
#include "swd.h"
#include <string.h>

#define OK 0u
#define ERROR 0xffu
#define MATCH_ERROR (SWD_ACK_OK | 0x10u)
/* Packet bounds limit initial transfers; only EXTRA WAIT/match attempts share
 * this retry budget. Never truncate a legal packet or invent a target WAIT.
 * A 64-byte Transfer packet needs at most 48 initial physical transactions
 * (including AP completions); at most 32 additional wire operations are allowed. */
#define RETRY_BUDGET 32u
static uint16_t wait_retry=100, match_retry;
static uint8_t idle_cycles, connected, retry_budget;
static uint32_t match_mask=0xffffffffu;
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1]<<8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static void put32(uint8_t *p,uint32_t v) { unsigned i; for(i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint8_t wire(uint8_t r,uint32_t *v) {
    uint32_t retry=0; uint8_t a;
    for(;;) {
        a=swd_transfer(r&15u,v); swd_idle(idle_cycles);
        if(a!=SWD_ACK_WAIT || retry>=wait_retry || !retry_budget) return a;
        --retry_budget; ++retry;
    }
}
/* Complete AP posted reads immediately via DP RDBUFF. This deliberately trades
 * throughput for simple ordering, including AP-read followed by DP operations. */
static uint8_t read_reg(uint8_t r,uint32_t *v) {
    uint8_t a=wire(r,v);
    if(a==SWD_ACK_OK && (r&1u)) a=wire(0x0eu,v);
    return a;
}
void dap_init(void) {
    wait_retry=100; match_retry=0; idle_cycles=0; connected=0;
    match_mask=0xffffffffu; swd_init();
}
static size_t info(uint8_t id,uint8_t *o) {
    const char *s=0; size_t n;
    switch(id) {
    case 1:s="WCH";break; case 2:s="CH570Q Wireless ARM DAP";break;
    case 3:s="CH570Q-0001";break; case 4:s="1.3.1";break;
    case 0xf0:o[1]=1;o[2]=1;return 3; /* SWD only; no SWO/JTAG/timestamp */
    case 0xfe:o[1]=1;o[2]=1;return 3;
    case 0xff:o[1]=2;o[2]=64;o[3]=0;return 4;
    default:o[1]=0;return 2;
    }
    n=strlen(s)+1; o[1]=(uint8_t)n; memcpy(o+2,s,n);return n+2;
}
/* All transfers are validated before touching GPIO; padding in HID/RF packets
 * is allowed. No command can read past len or write beyond the 64-byte packet. */
size_t dap_process(const uint8_t *q,size_t len,uint8_t *resp,size_t cap) {
    uint8_t o[64]={0}, cmd, a, r, i, done, last_write;
    uint32_t v, want, retry, wait;
    uint16_t count, j; size_t p=0, need=2, out=2, bits, bytes;
    if(!q || !resp || !len || len>64 || !cap) return 0;
    cmd=q[0]; o[0]=cmd;
    /* First pass establishes complete input/output bounds, with no side effects. */
    switch(cmd) {
    case 0x00: if(len<2)return 0; need=info(q[1],o); break;
    case 0x01: if(len<3)return 0; break;
    case 0x02: if(len<2)return 0; break;
    case 0x03: break;
    case 0x04: if(len<6)return 0;break;
    case 0x05:
        if(len<3)return 0;
        p=3; need=3;
        for(i=0;i<q[2];++i) {
            if(p>=len)return 0;
            r=q[p++];
            if(r&0xc0u)return 0;
            if((r&0x10u) && !(r&2u))return 0;
            if((r&0x20u) && (r&0x12u))return 0;
            if(!(r&2u) || (r&0x10u)) { if(len-p<4)return 0; p+=4; }
            if((r&2u) && !(r&0x10u)) need+=4;
        }
        if(need>64)return 0;
        break;
    case 0x06:
        if(len<5)return 0;
        count=u16(q+2);r=q[4];
        if(r&0xf0u)return 0;
        if(r&2u) { if(count>15)return 0;need=4+4u*count; }
        else { if(count>(len-5)/4)return 0;need=4; }
        break;
    /* TransferAbort has no response. This synchronous endpoint cannot preempt
     * an active transfer; the physical transaction budget bounds its work. */
    case 0x07: return 0;
    case 0x08: if(len<6)return 0;break;
    case 0x09: if(len<3)return 0;break;
    case 0x0a: need=3;break;
    case 0x10: if(len<7)return 0;break;
    case 0x11: if(len<5)return 0;break;
    case 0x12:
        if(len<2)return 0;
        bits=q[1]?q[1]:256;
        if(len-2<(bits+7)/8)return 0;
        break;
    case 0x13: if(len<2)return 0;break;
    case 0x1d:
        if(len<2)return 0;
        p=2;need=2;
        for(i=0;i<q[1];++i) {
            if(p>=len)return 0;
            r=q[p++];if(r&0x40u)return 0;
            bits=(r&63u)?(r&63u):64;bytes=(bits+7)/8;
            if(r&0x80u)need+=bytes;else {if(len-p<bytes)return 0;p+=bytes;}
        }
        if(need>64)return 0;
        break;
    default: if(cap<1)return 0;resp[0]=0xff;return 1;
    }
    if(cap<need)return 0;
    retry_budget=RETRY_BUDGET;
    switch(cmd) {
    case 0x00: out=need;break;
    case 0x01: o[1]=(q[1]<=1 && q[2]<=1)?OK:ERROR;break;
    case 0x02:
        if(q[1]==0 || q[1]==1) { if(!connected)swd_init();connected=1;o[1]=1; }
        else o[1]=0;
        break;
    case 0x03:swd_off();connected=0;o[1]=OK;break;
    case 0x04:
        if(q[1]>32) {o[1]=ERROR;break;}
        idle_cycles=q[1];wait_retry=u16(q+2);match_retry=u16(q+4);o[1]=OK;break;
    case 0x05:
        p=3;out=3;done=0;a=connected?SWD_ACK_OK:SWD_ERROR;last_write=0;
        for(i=0;connected && i<q[2];++i) {
            r=q[p++];v=0;
            if(!(r&2u) || (r&0x10u)){v=u32(q+p);p+=4;}
            if(r&0x20u) {match_mask=v;++done;continue;}
            if(r&0x10u) {
                want=v;retry=0;
                for(;;) {
                    a=read_reg(r,&v);if(a!=SWD_ACK_OK)break;
                    if((v&match_mask)==(want&match_mask))break;
                    a=MATCH_ERROR;
                    if(retry>=match_retry || retry_budget<((r&1u)?2u:1u))break;
                    /* Charge the whole repeated logical read before issuing
                     * it, so its mandatory AP RDBUFF can never be stranded. */
                    retry_budget-=(r&1u)?2u:1u; ++retry;
                }
            } else if(r&2u) a=read_reg(r,&v);
            else a=wire(r,&v);
            if(a!=SWD_ACK_OK)break;
            if((r&2u) && !(r&0x10u)){put32(o+out,v);out+=4;}
            last_write=!(r&2u);++done;
        }
        if(a==SWD_ACK_OK && last_write) {v=0;a=wire(0x0e,&v);}
        o[1]=done;o[2]=a;break;
    case 0x06:
        count=u16(q+2);r=q[4];out=4;done=0;a=connected?SWD_ACK_OK:SWD_ERROR;
        for(j=0;connected && j<count;++j) {
            v=(r&2u)?0:u32(q+5+4u*j);
            a=(r&2u)?read_reg(r,&v):wire(r,&v);
            if(a!=SWD_ACK_OK)break;
            if(r&2u){put32(o+out,v);out+=4;}++done;
        }
        if(a==SWD_ACK_OK && count && !(r&2u)){v=0;a=wire(0x0e,&v);}
        o[1]=done;o[2]=0;o[3]=a;break;
    case 0x08:
        v=u32(q+2);o[1]=(connected && wire(0,&v)==SWD_ACK_OK)?OK:ERROR;break;
    case 0x09:
        count=u16(q+1);if(count>50000u)o[1]=ERROR;else{swd_delay_us(count);o[1]=OK;}break;
    /* No target-specific reset callback is installed. A debugger can request
     * Cortex-M SYSRESETREQ by writing AIRCR through the ordinary MEM-AP path;
     * that is not a generic ResetTarget implementation or an nRESET pulse. */
    case 0x0a:o[1]=ERROR;o[2]=0;out=3;break;
    case 0x10:
        wait=u32(q+3);
        /* Only bits 0/1 are driven. Absent nRESET reads deasserted/high. */
        if(wait>50000u)wait=50000u;
        o[1]=connected?swd_pins(q[1],q[2]&3u,wait):0x80u;break;
    case 0x11:o[1]=swd_clock(u32(q+1))?OK:ERROR;break;
    case 0x12:
        if(!connected){o[1]=ERROR;break;}bits=q[1]?q[1]:256;
        swd_write_sequence(q+2,bits);o[1]=OK;break;
    case 0x13:
        if(q[1]&0xf8u){o[1]=ERROR;break;}swd_configure(q[1]);o[1]=OK;break;
    case 0x1d:
        if(!connected){o[1]=ERROR;break;}p=2;out=2;
        for(i=0;i<q[1];++i) {
            r=q[p++];bits=(r&63u)?(r&63u):64;bytes=(bits+7)/8;
            if(r&0x80u){swd_read_sequence(o+out,bits);out+=bytes;}
            else{swd_write_sequence(q+p,bits);p+=bytes;}
        }
        o[1]=OK;break;
    default:break;
    }
    memcpy(resp,o,out);return out;
}
