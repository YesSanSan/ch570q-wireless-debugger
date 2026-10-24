/* SDK-based toolchain smoke program using the actual board clock load. */
#include "CH57x_common.h"
#include "board.h"
volatile uint32_t heartbeat;
int main(void)
{
    HSECFG_Capacitance(BOARD_HSE_CAPACITANCE);
    SetSysClock(CLK_SOURCE_HSE_PLL_100MHz);
    for (;;) { mDelaymS(1000); ++heartbeat; }
}
