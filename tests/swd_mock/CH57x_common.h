#ifndef SWD_TEST_CH57X_COMMON_H
#define SWD_TEST_CH57X_COMMON_H
#include <stdint.h>

/* C++ proxies observe every write made by the real C source. No SDK code is
 * linked, and PIN reads return the target's current wire level, not a FIFO. */
struct MockRegister {
    enum Kind { Plain, Set, Clear, Direction, Pins } kind;
    uint32_t value;
    explicit MockRegister(Kind k = Plain) : kind(k), value(0) {}
    operator uint32_t() const;
    MockRegister &operator=(uint32_t bits);
    MockRegister &operator|=(uint32_t bits) { return *this = uint32_t(*this) | bits; }
    MockRegister &operator&=(uint32_t bits) { return *this = uint32_t(*this) & bits; }
};
extern MockRegister R32_PA_SET, R32_PA_CLR, R32_PA_DIR, R32_PA_PIN;
extern MockRegister R16_PIN_ALTERNATE, R32_PA_PD_DRV, R32_PA_PU;
#define RB_PIN_DEBUG_EN (1u << 7)
extern uint32_t mock_system_clock, mock_tick_step;
extern uint64_t mock_core_ticks;
struct MockCounter {
    operator uint32_t() const {
        uint32_t result = uint32_t(mock_core_ticks);
        mock_core_ticks += mock_tick_step;
        return result;
    }
};
struct MockSysTick { uint32_t CTLR, CMPL; MockCounter CNTL; };
extern MockSysTick mock_systick;
#define SysTick (&mock_systick)
#define SysTick_CTLR_STE (1u << 0)
#define SysTick_CTLR_STCLK (1u << 2)
#define SysTick_CTLR_STRE (1u << 3)
static inline uint32_t GetSysClock(void) { return mock_system_clock; }
static inline void mDelayuS(uint16_t) {}
#endif
