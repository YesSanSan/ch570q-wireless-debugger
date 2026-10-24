/* Compile the actual SWD implementation as C++ solely to intercept register
 * accesses. Target SWDIO advances on rising edges; the host must sample it
 * while SWCLK is low before that edge (CMSIS SW_DP.c SW_READ_BIT ordering).
 * These wire fixtures are independent of the implementation's bit helpers. */
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include "CH57x_common.h"

MockRegister R32_PA_SET(MockRegister::Set), R32_PA_CLR(MockRegister::Clear);
MockRegister R32_PA_DIR(MockRegister::Direction), R32_PA_PIN(MockRegister::Pins);
MockRegister R16_PIN_ALTERNATE, R32_PA_PD_DRV, R32_PA_PU;
static uint32_t latch;
uint32_t mock_system_clock = 100000000u, mock_tick_step = 7;
uint64_t mock_core_ticks;
MockSysTick mock_systick = {0, 0, {}};
static std::vector<uint64_t> rising_ticks;
struct Slot { bool output; bool bit; bool sample; };
struct Target {
    std::vector<Slot> slots;
    size_t edge = 0;
    bool sampled = false;
    bool wire = true;
    void require(bool ok, const char *what) const {
        if (!ok) throw std::runtime_error(std::string(what) + " at clock " + std::to_string(edge));
    }
    void rising() {
        require(edge < slots.size(), "extra clock");
        const Slot &s = slots[edge];
        require(bool(R32_PA_DIR.value & 1u) == s.output, "SWDIO direction/turnaround");
        if (s.output) require(bool(latch & 1u) == s.bit, "host request/data/parity bit");
        if (s.sample) require(sampled, "target bit was not sampled before rising edge");
        sampled = false;
        ++edge;
        // At the edge the target advances to the next cycle's SWDIO value.
        wire = edge < slots.size() ? slots[edge].bit : true;
    }
    uint32_t pins() {
        require(!(latch & 2u), "target sampled with SWCLK high");
        require(edge < slots.size() && slots[edge].sample, "unexpected PIN read");
        require(!(R32_PA_DIR.value & 1u), "host driving during target read");
        require(!sampled, "target bit sampled twice");
        sampled = true;
        return (latch & ~1u) | unsigned(wire);
    }
};
static Target *active;
MockRegister::operator uint32_t() const {
    if (kind == Pins) return active ? active->pins() : latch;
    return value;
}
MockRegister &MockRegister::operator=(uint32_t bits) {
    value = bits;
    if (kind == Set || kind == Clear) {
        uint32_t before = latch;
        if (kind == Set) latch |= bits; else latch &= ~bits;
        if (active && active->edge < active->slots.size() &&
            active->slots[active->edge].output && ((before ^ latch) & 1u))
            active->require(!(before & 2u), "host changed SWDIO before lowering SWCLK");
        if (!(before & 2u) && (latch & 2u)) {
            rising_ticks.push_back(mock_core_ticks);
            if (active) active->rising();
        }
    }
    return *this;
}
#ifndef SWD_SOURCE
#define SWD_SOURCE "../src/swd.c"
#endif
#include SWD_SOURCE

static void host_byte(Target &t, uint8_t byte) {
    for (unsigned i=0; i<8; ++i) t.slots.push_back({true, bool((byte >> i) & 1u), false});
}
static void turnaround_slots(Target &t, unsigned count) {
    for (unsigned i=0; i<count; ++i) t.slots.push_back({false, true, false});
}
static void target_bits(Target &t, uint32_t value, unsigned count) {
    for (unsigned i=0; i<count; ++i) t.slots.push_back({false, bool((value >> i) & 1u), true});
}
static unsigned parity(uint32_t value) {
    unsigned result=0;
    while (value) { result ^= value & 1u; value >>= 1; }
    return result;
}
static void setup(unsigned turns, bool data_phase_enabled) {
    active = nullptr; latch = 0;
    R32_PA_DIR.value = 0;
    swd_init();
    swd_configure(uint8_t((turns-1u) | (data_phase_enabled ? 4u : 0u)));
}
static void finish(Target &t) {
    active = nullptr;
    t.require(t.edge == t.slots.size(), "missing clock cycles");
    t.require(bool(latch & 2u), "SWCLK must end high");
    t.require(bool(R32_PA_DIR.value & 1u), "host must reclaim SWDIO");
    t.require(bool(latch & 1u), "SWDIO must park high");
}
static void run_read(unsigned turns, bool bad_parity) {
    setup(turns, false);
    Target t;
    host_byte(t, 0xa5); // DP IDCODE read: start, RnW, header parity, park.
    turnaround_slots(t, turns);
    target_bits(t, 1, 3); // OK is 1,0,0 LSB first; never a synthetic ACK FIFO.
    const uint32_t id = 0x1ba01477u;
    target_bits(t, id, 32);
    target_bits(t, parity(id) ^ unsigned(bad_parity), 1);
    turnaround_slots(t, turns);
    active = &t;
    uint32_t actual = 0xdeadbeefu;
    uint8_t ack = swd_transfer(2, &actual);
    finish(t);
    t.require(ack == (bad_parity ? SWD_ERROR : SWD_ACK_OK), "read ACK/parity result");
    t.require(actual == (bad_parity ? 0xdeadbeefu : id), "IDCODE or parity-failure output");
}
static void run_write(unsigned turns) {
    setup(turns, false);
    Target t;
    host_byte(t, 0x81); // DP write address 0, explicit known wire request.
    turnaround_slots(t, turns);
    target_bits(t, 1, 3);
    turnaround_slots(t, turns);
    uint32_t value = 0xa5963cc3u;
    for (unsigned i=0; i<32; ++i) t.slots.push_back({true, bool((value >> i)&1u), false});
    t.slots.push_back({true, bool(parity(value)), false});
    active = &t;
    uint8_t ack = swd_transfer(0, &value);
    finish(t);
    t.require(ack == SWD_ACK_OK, "write ACK");
}
static void run_ack(unsigned ack, bool read, bool data_phase_enabled, unsigned turns) {
    setup(turns, data_phase_enabled);
    Target t;
    host_byte(t, read ? 0xa5 : 0x81);
    turnaround_slots(t, turns);
    target_bits(t, ack, 3);
    bool valid = ack == SWD_ACK_WAIT || ack == SWD_ACK_FAULT;
    if (valid && data_phase_enabled && read) {
        target_bits(t, 0xa5a55a5au, 32);
        target_bits(t, 0, 1);
    }
    turnaround_slots(t, turns);
    if (!valid) turnaround_slots(t, 33); // CMSIS invalid-ACK backoff, still input.
    if (valid && data_phase_enabled && !read)
        for (unsigned i=0; i<33; ++i) t.slots.push_back({true, false, false});
    active = &t;
    uint32_t value = 0x12345678u;
    uint8_t result = swd_transfer(read ? 2 : 0, &value);
    finish(t);
    t.require(result == (valid ? ack : SWD_ERROR), "WAIT/FAULT/invalid ACK result");
    t.require(value == 0x12345678u, "failed read must preserve caller data");
}
static void require(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what);
}
static uint64_t bit_period() {
    const uint8_t data = 0x55;
    rising_ticks.clear();
    swd_write_sequence(&data, 8);
    require(rising_ticks.size() == 8, "clock test edge count");
    uint64_t period = rising_ticks[1] - rising_ticks[0];
    for (size_t i=2; i<rising_ticks.size(); ++i)
        require(rising_ticks[i] - rising_ticks[i-1] == period, "uneven modeled clock period");
    return period;
}
static void run_clocks() {
    active = nullptr;
    mock_systick.CTLR = SysTick_CTLR_STE | SysTick_CTLR_STCLK | SysTick_CTLR_STRE;
    mock_systick.CMPL = 0xffffffffu;
    const uint32_t control = mock_systick.CTLR;
    swd_init();
    uint64_t previous = UINT64_MAX;
    for (uint32_t hz : {100000u, 333333u, 1000000u}) {
        require(swd_clock(hz) == 1, "valid clock rejected");
        uint64_t period = bit_period();
        uint64_t minimum = 2u * ((uint64_t(mock_system_clock) + 2u*hz - 1u) / (2u*hz));
        require(period >= minimum && period < minimum + 6u*mock_tick_step,
                "elapsed-core-clock delay outside polling tolerance");
        require(period < previous, "higher requested clock did not shorten period");
        previous = period;
    }
    for (uint32_t hz : {0u, 99999u, 1000001u, UINT32_MAX}) {
        require(!swd_clock(hz), "out-of-range clock accepted");
        require(bit_period() == previous, "rejected clock changed timing");
    }
    mock_system_clock = 0;
    require(!swd_clock(100000u), "unknown system clock accepted");
    require(bit_period() == previous, "unknown system clock changed timing");
    mock_system_clock = 100000000u;
    mock_core_ticks = UINT32_MAX - 20u;
    require(bit_period() == previous, "counter wrap changed delay");
    require(mock_systick.CTLR == control && mock_systick.CMPL == 0xffffffffu,
            "SWD reconfigured shared SysTick");
    for (uint32_t disabled : {0u, SysTick_CTLR_STE, SysTick_CTLR_STE | SysTick_CTLR_STRE,
                              SysTick_CTLR_STE | SysTick_CTLR_STCLK}) {
        mock_systick.CTLR = disabled;
        require(swd_clock(1000000u) == 1, "fallback clock rejected");
        uint64_t before = mock_core_ticks;
        require(bit_period() == 0 && mock_core_ticks == before, "invalid timer used for delay");
        require(mock_systick.CTLR == disabled, "fallback reconfigured timer");
    }
    mock_systick.CTLR = control;
    mock_systick.CMPL = 99999;
    require(swd_clock(1000000u) == 1 && bit_period() == 0, "reloading timer used for delay");
    mock_systick.CMPL = 0xffffffffu;
    require(swd_clock(1000000u) == 1, "core clock restore failed");
}
static void run_sequences(unsigned bits) {
    setup(1, false);
    Target t;
    std::vector<uint8_t> data((bits+7)/8, 0xa5), result((bits+7)/8, 0xff);
    for (unsigned i=0; i<bits; ++i)
        t.slots.push_back({true, bool((data[i/8] >> (i%8)) & 1u), false});
    for (unsigned i=0; i<bits; ++i)
        t.slots.push_back({false, bool((data[i/8] >> (i%8)) & 1u), true});
    host_byte(t, 0xff);
    active = &t;
    swd_write_sequence(data.data(), bits);
    swd_read_sequence(result.data(), bits);
    const uint8_t park = 0xff;
    swd_write_sequence(&park, 8);
    finish(t);
    if (bits%8) data.back() &= (1u << (bits%8)) - 1u;
    require(result == data, "sequence bit order or unused high bits");
}
int main() {
    unsigned cases=0;
    try {
        for (unsigned turns=1; turns<=4; ++turns) {
            run_read(turns, false); ++cases;
            run_read(turns, true); ++cases;
            run_write(turns); ++cases;
            for (unsigned ack : {2u,4u})
                for (bool read : {false,true})
                    for (bool data : {false,true}) {
                        run_ack(ack, read, data, turns); ++cases;
                    }
            run_ack(0, true, false, turns); ++cases;
            run_ack(7, false, false, turns); ++cases;
        }
        for (unsigned bits : {1u, 7u, 8u, 9u, 64u, 256u}) {
            run_sequences(bits); ++cases;
        }
        run_clocks(); ++cases;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "SWD edge regression failed after %u cases: %s\n", cases, e.what());
        return 1;
    }
    std::printf("SWD GPIO tests passed (%u cases: transfers, sequences, clock bounds/fallback/wrap).\n", cases);
    return 0;
}
