/* Native test: gcc -std=c99 -Iinclude src/dap.c tests/test_dap.c -o test_dap */
#include "dap.h"
#include "swd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls, inits, off_calls, seq_bits;
static uint8_t ack=1, requests[128], scripted_ack[128];
static uint32_t read_value=0x12345678, values[128], scripted_value[128];
static unsigned script_count;
void swd_init(void){++inits;}
void swd_off(void){++off_calls;}
int swd_clock(uint32_t hz){return hz>=100000 && hz<=1000000;}
void swd_configure(uint8_t c){(void)c;}
uint8_t swd_transfer(uint8_t r,uint32_t *v){
    unsigned index=calls++;uint8_t result=index<script_count?scripted_ack[index]:ack;
    if(index<128){requests[index]=r;values[index]=*v;}
    if(r&2)*v=index<script_count?scripted_value[index]:read_value;
    return result;
}
void swd_write_sequence(const uint8_t *p,size_t b){(void)p;seq_bits+=(unsigned)b;}
void swd_read_sequence(uint8_t *p,size_t b){memset(p,0x5a,(b+7)/8);seq_bits+=(unsigned)b;}
uint8_t swd_pins(uint8_t v,uint8_t s,uint32_t w){(void)s;(void)w;return v&3;}
void swd_delay_us(uint32_t us){assert(us<=50000);}
void swd_idle(uint8_t n){assert(n<=32);}
static uint8_t out[66];
static size_t run(const uint8_t *q,size_t n){memset(out,0xa5,sizeof(out));return dap_process(q,n,out,64);}
static void software_paths(void){
    /* Tool-driven Cortex-M SYSRESETREQ: DP SELECT, MEM-AP CSW/TAR/DRW.
     * AIRCR's key and address belong to the tool, not a generic ResetTarget. */
    const uint8_t reset[]={5,0,4,8,0,0,0,0,1,0x52,0,0,0x23,
                           5,0x0c,0xed,0,0xe0,13,4,0,0xfa,5};
    const uint8_t ap_read[]={5,0,1,3};
    const uint8_t ap_block[]={6,0,2,0,3};
    const uint8_t write_block[]={6,0,2,0,13,1,0,0,0,2,0,0,0};
    const uint8_t abort_command[]={7};
    const uint8_t reset_target[]={0x0a};
    unsigned i;
    calls=0;
    assert(run(reset,sizeof(reset))==3 && out[1]==4 && out[2]==SWD_ACK_OK);
    assert(calls==5 && requests[0]==8 && requests[1]==1 && requests[2]==5 && requests[3]==13 && requests[4]==14);
    assert(values[0]==0 && values[1]==0x23000052u && values[2]==0xe000ed0cu && values[3]==0x05fa0004u);
    /* Posted write completion must report a delayed AP fault, not success. */
    script_count=5;for(i=0;i<script_count;++i)scripted_ack[i]=SWD_ACK_OK;
    scripted_ack[4]=SWD_ACK_FAULT;calls=0;
    assert(run(reset,sizeof(reset))==3 && out[1]==4 && out[2]==SWD_ACK_FAULT && calls==5);
    /* Distinct stale AP data and RDBUFF data detect a missing completion read. */
    script_count=2;scripted_ack[0]=scripted_ack[1]=SWD_ACK_OK;
    scripted_value[0]=0xdeadbeefu;scripted_value[1]=0xcafebabeu;calls=0;
    assert(run(ap_read,sizeof(ap_read))==7 && out[1]==1 && out[2]==SWD_ACK_OK);
    assert(out[3]==0xbe && out[4]==0xba && out[5]==0xfe && out[6]==0xca && calls==2);
    scripted_ack[1]=SWD_ACK_FAULT;calls=0;
    assert(run(ap_read,sizeof(ap_read))==3 && !out[1] && out[2]==SWD_ACK_FAULT && calls==2);
    script_count=4;for(i=0;i<script_count;++i)scripted_ack[i]=SWD_ACK_OK;
    scripted_value[1]=0x11223344u;scripted_value[3]=0xaabbccddu;calls=0;
    assert(run(ap_block,sizeof(ap_block))==12 && out[1]==2 && out[2]==0 && out[3]==SWD_ACK_OK);
    assert(calls==4 && requests[0]==3 && requests[1]==14 && requests[2]==3 && requests[3]==14);
    assert(out[4]==0x44 && out[7]==0x11 && out[8]==0xdd && out[11]==0xaa);
    script_count=3;scripted_ack[2]=SWD_ACK_FAULT;calls=0;
    assert(run(write_block,sizeof(write_block))==4 && out[1]==2 && out[3]==SWD_ACK_FAULT);
    assert(calls==3 && requests[0]==13 && requests[1]==13 && requests[2]==14 && values[0]==1 && values[1]==2);
    script_count=0;calls=0;
    assert(run(reset_target,sizeof(reset_target))==3 && out[1]==255 && out[2]==0 && !calls);
    assert(run(abort_command,sizeof(abort_command))==0 && out[0]==0xa5 && !calls);
}
static void full_packets(void){
    uint8_t q[64]={5,0,24};unsigned p=3,i;
    /* Maximum mixed reply: 15 AP reads + 9 writes, 40 physical operations
     * including the final write completion. Legal request/reply are 63 bytes. */
    for(i=0;i<15;++i)q[p++]=3;
    for(i=0;i<9;++i){q[p++]=8;memset(q+p,0,4);p+=4;}
    assert(p==63);calls=0;
    assert(run(q,p)==63 && out[1]==24 && out[2]==SWD_ACK_OK && calls==40);
    for(i=0;i<15;++i){assert(requests[2*i]==3 && requests[2*i+1]==14);assert(out[3+4*i]==0x78);}
    assert(requests[39]==14);
    /* Exhaust every WAIT retry in the first operation, then complete every
     * later AP read and the final write flush with no synthetic WAIT. */
    script_count=32;for(i=0;i<script_count;++i)scripted_ack[i]=SWD_ACK_WAIT;
    calls=0;assert(run(q,p)==63 && out[1]==24 && out[2]==SWD_ACK_OK && calls==72);
    script_count=0;
    /* First successful match reads are normal packet work too: worst case
     * 15 ordinary AP reads + 9 AP comparisons = 48 operations, not 40. */
    p=3;for(i=0;i<15;++i)q[p++]=3;
    for(i=0;i<9;++i){q[p++]=0x13;q[p++]=0x78;q[p++]=0x56;q[p++]=0x34;q[p++]=0x12;}
    calls=0;assert(p==63 && run(q,p)==63 && out[1]==24 && out[2]==SWD_ACK_OK && calls==48);
    script_count=32;for(i=0;i<script_count;++i)scripted_ack[i]=SWD_ACK_WAIT;
    calls=0;assert(run(q,p)==63 && out[1]==24 && out[2]==SWD_ACK_OK && calls==80);
    script_count=0;
    /* Last logical AP read must still get its RDBUFF after 32 wire ops. */
    p=3;for(i=0;i<9;++i){q[p++]=8;memset(q+p,0,4);p+=4;}
    for(i=0;i<15;++i)q[p++]=3;
    calls=0;assert(run(q,p)==63 && out[1]==24 && out[2]==SWD_ACK_OK && calls==39);
    assert(requests[37]==3 && requests[38]==14);
    /* OpenOCD banked core-register reads: WAITs in an earlier transfer must
     * not steal the first attempts/completions of the remaining transfers. */
    p=3;q[2]=21;
    for(i=0;i<5;++i){q[p++]=5;memset(q+p,0,4);p+=4;q[p++]=3;q[p++]=11;q[p++]=14;}
    q[p++]=5;memset(q+p,0,4);p+=4;
    script_count=4;for(i=0;i<4;++i)scripted_ack[i]=SWD_ACK_WAIT;
    calls=0;assert(run(q,p)==63 && out[1]==21 && out[2]==SWD_ACK_OK && calls==36);
    /* A real WAIT at AP completion remains bounded and is reported honestly. */
    q[2]=1;q[3]=3;script_count=34;
    scripted_ack[0]=SWD_ACK_OK;
    for(i=1;i<script_count;++i)scripted_ack[i]=SWD_ACK_WAIT;
    calls=0;assert(run(q,4)==3 && out[1]==0 && out[2]==SWD_ACK_WAIT && calls==34);
    /* With zero extra budget, AP still gets its first completion attempt;
     * if that actual completion says WAIT, report it without retrying. */
    q[2]=2;q[3]=2;q[4]=3;script_count=35;
    for(i=0;i<script_count;++i)scripted_ack[i]=SWD_ACK_WAIT;
    scripted_ack[32]=scripted_ack[33]=SWD_ACK_OK;scripted_value[32]=read_value;
    calls=0;assert(run(q,5)==7 && out[1]==1 && out[2]==SWD_ACK_WAIT && calls==35);
    script_count=0;
    /* Full standard block capacities remain usable. */
    q[0]=6;q[2]=15;q[3]=0;q[4]=3;calls=0;
    assert(run(q,5)==64 && out[1]==15 && out[3]==SWD_ACK_OK && calls==30);
    q[2]=14;q[4]=13;memset(q+5,0,56);calls=0;
    assert(run(q,61)==4 && out[1]==14 && out[3]==SWD_ACK_OK && calls==15);
}
static void retry_edges(void){
    uint8_t cfg[]={4,0,0,0,0,0},q[]={5,0,1,2,0,0,0,0};unsigned i;
    assert(run(cfg,sizeof(cfg))==2);ack=SWD_ACK_WAIT;calls=0;
    assert(run(q,4)==3 && out[2]==SWD_ACK_WAIT && calls==1);
    cfg[2]=1;assert(run(cfg,sizeof(cfg))==2);calls=0;
    assert(run(q,4)==3 && out[2]==SWD_ACK_WAIT && calls==2);
    memset(cfg+2,255,4);assert(run(cfg,sizeof(cfg))==2);ack=SWD_ACK_OK;
    q[3]=0x13;calls=0;
    assert(run(q,8)==3 && out[1]==0 && out[2]==0x11 && calls==34);
    /* One WAIT leaves an odd retry budget: never issue a half-budget AP
     * comparison (its request and completion must both be reserved). */
    script_count=1;scripted_ack[0]=SWD_ACK_WAIT;calls=0;
    assert(run(q,8)==3 && out[1]==0 && out[2]==0x11 && calls==33);
    /* A match on the final allowed AP RDBUFF must count as success. */
    script_count=34;for(i=0;i<script_count;++i){scripted_ack[i]=SWD_ACK_OK;scripted_value[i]=read_value;}
    scripted_value[33]=0;calls=0;
    assert(run(q,8)==3 && out[1]==1 && out[2]==SWD_ACK_OK && calls==34);
    script_count=0;
}
int main(void){
    uint8_t q[64]={0};size_t n,cap,len;unsigned seed=12345,t,i,before;
    dap_init();assert(inits==1);
    q[0]=0;q[1]=0xf0;assert(run(q,2)==3 && out[1]==1 && out[2]==1);
    q[1]=0xfe;assert(run(q,2)==3 && out[2]==1);
    q[1]=0xff;assert(run(q,2)==4 && out[2]==64 && out[3]==0);
    q[1]=4;assert(run(q,2)==8 && out[1]==6 && memcmp(out+2,"1.3.3",6)==0);
    q[0]=0x0a;assert(run(q,1)==3 && out[1]==255 && out[2]==0);
    q[0]=2;q[1]=2;assert(run(q,2)==2 && out[1]==0);
    q[1]=1;assert(run(q,2)==2 && out[1]==1);
    software_paths();
    full_packets();
    retry_edges();
    /* AP read consumes posted AP data then fetches DP RDBUFF. */
    q[0]=5;q[1]=0;q[2]=1;q[3]=3;calls=0;
    assert(run(q,4)==7 && out[1]==1 && out[2]==1 && out[3]==0x78);
    assert(calls==2 && requests[0]==3 && requests[1]==14);
    /* A truncated later write must cause NO earlier transfer. */
    q[2]=2;q[3]=2;q[4]=0;q[5]=0;calls=0;
    assert(run(q,6)==0 && calls==0 && out[0]==0xa5);
    q[2]=1;q[3]=2;calls=0;
    assert(dap_process(q,4,out,6)==0 && calls==0);
    /* WAIT and matching are bounded even with retry=65535. */
    q[0]=4;q[1]=0;memset(q+2,255,4);assert(run(q,6)==2);
    q[0]=5;q[1]=0;q[2]=1;q[3]=2;ack=2;calls=0;
    assert(run(q,4)==3 && out[1]==0 && out[2]==2 && calls==33);ack=1;
    q[3]=0x12;memset(q+4,0,4);calls=0;
    assert(run(q,8)==3 && out[1]==0 && out[2]==0x11 && calls==33);
    /* Block sizes are bounded by both input and output, including uint16 counts. */
    q[0]=6;q[1]=0;q[2]=255;q[3]=255;q[4]=2;calls=0;
    assert(run(q,5)==0 && calls==0);
    q[2]=2;q[3]=0;assert(run(q,5)==12 && out[1]==2 && out[3]==1);
    q[4]=0;assert(run(q,8)==0);
    q[0]=0x1d;q[1]=2;q[2]=0x88;q[3]=8;q[4]=0xab;seq_bits=0;
    assert(run(q,5)==3 && out[2]==0x5a && seq_bits==16);
    q[1]=3;seq_bits=0;assert(run(q,5)==0 && !seq_bits);
    q[0]=0x12;q[1]=0;seq_bits=0;assert(run(q,33)==0 && !seq_bits);
    assert(run(q,34)==2 && seq_bits==256);
    /* Deterministic malformed packet fuzz + output canaries for all capacities. */
    for(t=0;t<20000;++t){
        for(i=0;i<64;++i){seed=seed*1664525u+1013904223u;q[i]=(uint8_t)(seed>>24);}
        len=(seed>>8)%65;cap=seed%65;memset(out,0xa5,sizeof(out));before=calls;
        n=dap_process(q,len,out,cap);assert(n<=cap && n<=64 && calls-before<=80);
        for(i=(unsigned)cap;i<66;++i)assert(out[i]==0xa5);
        if(!n)assert(calls==before);
    }
    puts("DAP parser tests passed");return 0;
}
