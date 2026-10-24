#include "swd.h"
#include "CH57x_common.h"

#define DIO (1u << 0)
#define CLK (1u << 1)
/* Requested clock is an upper bound, not a measured GPIO rate. Use the
 * application's free-running core-clock SysTick without reconfiguring it.
 * GPIO, loop and interrupt overhead can only lengthen these half periods. */
static uint32_t half_cycles = 500;
static uint8_t turnaround = 1, data_phase, core_counter;
static void half_delay(void) {
    if (core_counter) {
        uint32_t start = SysTick->CNTL;
        while ((uint32_t)(SysTick->CNTL - start) < half_cycles) {}
    } else {
        /* Safe before the application enables SysTick: uncalibrated, slower. */
        volatile uint32_t n = half_cycles;
        while (n--) __asm volatile("nop");
    }
}
static void dio_out(void) { R32_PA_DIR |= DIO; }
static void dio_in(void) { R32_PA_DIR &= ~DIO; }
static void dio(uint8_t b) { if (b) R32_PA_SET = DIO; else R32_PA_CLR = DIO; }
/* End each bit high, as in Arm CMSIS-DAP SW_DP.c. Target output changes
 * after the rising edge: sample the stable input BEFORE raising SWCLK.
 * Sampling afterwards skips ACK bit 0 and shifts the following data. */
static void tick(void) { R32_PA_CLR = CLK; half_delay(); R32_PA_SET = CLK; half_delay(); }
static void put(uint8_t b) { R32_PA_CLR = CLK; dio(b); tick(); }
static uint8_t get(void) {
    uint8_t b;
    R32_PA_CLR = CLK; half_delay(); b = (R32_PA_PIN & DIO) != 0;
    R32_PA_SET = CLK; half_delay(); return b;
}
static void turn(void) { uint8_t n; for(n = 0; n < turnaround; ++n) tick(); }
void swd_init(void) {
    /* This is the only place that disables CH570's own debug interface. */
    /* GPIOADigitalCfg(ENABLE, PA0|PA1) semantics from the official driver. */
    R16_PIN_ALTERNATE &= ~(RB_PIN_DEBUG_EN | DIO | CLK);
    R32_PA_CLR = CLK; R32_PA_SET = DIO;
    R32_PA_PD_DRV &= ~(DIO | CLK); R32_PA_PU |= DIO;
    R32_PA_DIR |= DIO | CLK;
    turnaround = 1; data_phase = 0; (void)swd_clock(100000);
}
void swd_off(void) { R32_PA_DIR &= ~(DIO | CLK); }
int swd_clock(uint32_t hz) {
    uint32_t sys;
    if (hz < 100000u || hz > 1000000u) return 0;
    sys = GetSysClock();
    if (!sys) return 0;
    half_cycles = (uint32_t)(((uint64_t)sys + 2u * (uint64_t)hz - 1u) / (2u * (uint64_t)hz));
    core_counter = (SysTick->CTLR & (SysTick_CTLR_STE | SysTick_CTLR_STCLK | SysTick_CTLR_STRE)) ==
                   (SysTick_CTLR_STE | SysTick_CTLR_STCLK | SysTick_CTLR_STRE) &&
                   SysTick->CMPL == 0xffffffffu;
    return 1;
}
void swd_configure(uint8_t cfg) { turnaround = (cfg & 3u) + 1u; data_phase = (cfg >> 2) & 1u; }
void swd_delay_us(uint32_t us) { while(us) { uint16_t n = us > 60000u ? 60000u : (uint16_t)us; mDelayuS(n); us -= n; } }
void swd_idle(uint8_t cycles) { dio_out(); dio(0); while(cycles--) tick(); dio(1); }
void swd_write_sequence(const uint8_t *data, size_t bits) {
    size_t i; R32_PA_CLR = CLK; dio_out(); for(i=0;i<bits;++i) put((data[i/8] >> (i%8)) & 1u);
}
void swd_read_sequence(uint8_t *data, size_t bits) {
    size_t i; R32_PA_CLR = CLK; dio_in();
    for(i=0;i<(bits+7)/8;++i) data[i]=0;
    for(i=0;i<bits;++i) data[i/8] |= get() << (i%8);
}
uint8_t swd_pins(uint8_t value, uint8_t select, uint32_t wait_us) {
    uint8_t state;
    if(select & 1u) { R32_PA_DIR |= CLK; if(value & 1u) R32_PA_SET=CLK; else R32_PA_CLR=CLK; }
    if(select & 2u) { dio(value & 2u); dio_out(); }
    do {
        state = 0x80u | ((R32_PA_PIN & CLK) ? 1u : 0u) | ((R32_PA_PIN & DIO) ? 2u : 0u);
        if ((state & (select & 3u)) == (value & (select & 3u)) || !wait_us) break;
        swd_delay_us(1); --wait_us;
    } while(1);
    return state; /* Absent nRESET reports deasserted; it cannot be driven. */
}
uint8_t swd_transfer(uint8_t request, uint32_t *data) {
    uint8_t i, p=0, ack=0, bit; uint32_t value=0;
    R32_PA_CLR = CLK; dio_out(); put(1);
    for(i=0;i<4;++i) { bit=(request >> i)&1u; p ^= bit; put(bit); }
    put(p); put(0); put(1);
    dio_in(); turn();
    for(i=0;i<3;++i) ack |= get() << i;
    if(ack == SWD_ACK_OK) {
        if(request & 2u) {
            p=0; for(i=0;i<32;++i) { bit=get(); value |= (uint32_t)bit << i; p ^= bit; }
            bit=get(); turn(); dio(1); dio_out();
            if(bit != p) return SWD_ERROR;
            *data=value;
        } else {
            turn(); dio_out(); value=*data; p=0;
            for(i=0;i<32;++i) { bit=(value >> i)&1u; p ^= bit; put(bit); }
            put(p); dio(1);
        }
    } else if(ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT) {
        if(data_phase && (request & 2u)) for(i=0;i<33;++i) (void)get();
        turn(); dio(1); dio_out();
        if(data_phase && !(request & 2u)) { for(i=0;i<33;++i) put(0); dio(1); }
    } else {
        /* Invalid ACK may be a shifted OK: leave SWDIO released while any
         * target data/parity completes before reclaiming the wire. */
        turn(); for(i=0;i<33;++i) tick();
        dio(1); dio_out(); ack=SWD_ERROR;
    }
    return ack;
}
