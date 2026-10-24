#include "uart_bridge.h"
#include "uart_log.h"
#include "CH57x_common.h"
#include <string.h>

static uint8_t rx[UART_BRIDGE_RX_CAPACITY], tx[UART_BRIDGE_TX_CAPACITY];
static volatile uint16_t rx_head, rx_tail;
static uint16_t tx_head, tx_tail;
static uint16_t divisor;
static volatile uart_bridge_snapshot_t state;

/* Only the RX producer runs in IRQ context. Foreground consumers and hardware
 * transitions use short critical sections; never mask IRQs over packet copies. */
static uint32_t lock(void) { return __risc_v_disable_irq(); }
static void unlock(uint32_t irq) { __risc_v_enable_irq(irq); }

static bool baud_divisor(uint32_t baud, uint16_t *result, uint32_t *actual)
{
    uint32_t clock = GetSysClock();
    if (!baud || !clock) return false;
    uint64_t denominator = (uint64_t)baud * 8u;
    uint64_t div = ((uint64_t)clock + denominator / 2u) / denominator;
    if (!div || div > UINT16_MAX) return false;
    uint64_t target = denominator * div;
    uint64_t error = target > clock ? target - clock : clock - target;
    if (error * 100u > target * 2u) return false;
    *result = (uint16_t)div;
    *actual = clock / (8u * (uint32_t)div);
    return true;
}

bool uart_bridge_validate(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits)
{
    uint16_t checked_divisor;
    uint32_t actual;
    return (stop == 0 || stop == 2) && parity <= 4 && bits >= 5 && bits <= 8 &&
        baud_divisor(baud, &checked_divisor, &actual);
}

static void apply_configuration(void)
{
    uint8_t lcr = state.bits - 5u;
    if (state.stop == 2u) lcr |= RB_LCR_STOP_BIT;
    if (state.parity) lcr |= RB_LCR_PAR_EN | ((state.parity - 1u) << 4);
    GPIOA_SetBits(GPIO_Pin_3);
    GPIOA_ModeCfg(GPIO_Pin_2, GPIO_ModeIN_PU);
    GPIOA_ModeCfg(GPIO_Pin_3, GPIO_ModeOut_PP_5mA);
    UART_Remap(ENABLE, UART_TX_REMAP_PA3, UART_RX_REMAP_PA2);
    /* Same divisor/oversampling as SDK UART_BaudRateCfg, using 64-bit
     * validation above to avoid divide-by-zero/truncation/overflow. */
    R16_UART_DL = divisor;
    R8_UART_DIV = 1;
    R8_UART_LCR = lcr;
    R8_UART_FCR = RB_FCR_FIFO_EN; /* Never assert FIFO clear bits. */
    R8_UART_MCR |= RB_MCR_INT_OE;
    R8_UART_IER = RB_IER_TXD_EN | RB_IER_RECV_RDY | RB_IER_LINE_STAT;
}

static uint8_t line_status(void)
{
    /* LSR errors clear on read. Read before RBR, including when RFC is zero,
     * so overrun is recorded even if no byte is currently available. */
    uint8_t status = UART_GetLinSTA();
    if (status & RB_LSR_OVER_ERR) ++state.overrun_errors;
    if (status & RB_LSR_PAR_ERR) ++state.parity_errors;
    if (status & RB_LSR_FRAME_ERR) ++state.framing_errors;
    if (status & RB_LSR_BREAK_ERR) ++state.break_errors;
    return status;
}

static void receive(bool discard)
{
    for (unsigned i = 0; i < UART_FIFO_SIZE; ++i) {
        uint8_t status = line_status();
        if (!(status & RB_LSR_DATA_RDY)) break;
        uint8_t byte = UART_RecvByte();
        if (discard) { ++state.rx_discarded; continue; }
        ++state.rx_received;
        if (state.rx_count == UART_BRIDGE_RX_CAPACITY) ++state.rx_overflow;
        else {
            rx[rx_head] = byte;
            rx_head = (rx_head + 1u) % UART_BRIDGE_RX_CAPACITY;
            ++state.rx_count;
        }
    }
}

__INTERRUPT __HIGH_CODE
void UART_IRQHandler(void)
{
    if (!state.enabled) return; /* Legacy logging never enables this source. */
    PFIC_ClearPendingIRQ(UART_IRQn);
    receive(false); /* <=8 bytes, including errors; never spin on live input. */
    /* A full budget can leave bytes arriving during this ISR. Explicitly pend
     * another bounded pass; one-byte trigger also covers arrivals after here. */
    if (R8_UART_RFC) PFIC_SetPendingIRQ(UART_IRQn);
}

void uart_bridge_init(void)
{
    /* Startup-only: no peripheral/PFIC access while logging owns the UART. */
    memset((void *)&state, 0, sizeof(state));
    rx_head = rx_tail = tx_head = tx_tail = 0;
    divisor = 0;
    (void)uart_bridge_configure(115200, 0, 0, 8);
}

bool uart_bridge_configure(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits)
{
    uint16_t next_divisor;
    uint32_t actual;
    if ((stop != 0 && stop != 2) || parity > 4 || bits < 5 || bits > 8 ||
        !baud_divisor(baud, &next_divisor, &actual)) return false;
    uint32_t irq = lock();
    if (state.enabled) {
        uint8_t status = line_status();
        if (state.rx_count || state.tx_count || R8_UART_RFC ||
            !(status & RB_LSR_TX_ALL_EMP)) { unlock(irq); return false; }
    }
    state.baud = baud; state.actual_baud = actual;
    state.stop = stop; state.parity = parity; state.bits = bits;
    divisor = next_divisor;
    if (state.enabled) apply_configuration();
    unlock(irq);
    return true;
}

bool uart_bridge_enable(void)
{
    if (state.enabled) return true;
    /* Revalidate staged settings against the current system clock. */
    uint16_t next_divisor;
    uint32_t actual;
    if (!baud_divisor(state.baud, &next_divisor, &actual)) return false;
    uart_log_suspend(true);
    uint32_t irq = lock();
    state.enable_pending = true;
    if (!(line_status() & RB_LSR_TX_ALL_EMP)) { unlock(irq); return false; }
    receive(true); /* Bytes received while logger owned UART are not session RX. */
    divisor = next_divisor;
    state.actual_baud = actual;
    PFIC_DisableIRQ(UART_IRQn);
    PFIC_ClearPendingIRQ(UART_IRQn);
    apply_configuration();
    state.enable_pending = false;
    state.enabled = true;
    PFIC_EnableIRQ(UART_IRQn);
    unlock(irq);
    return true;
}

bool uart_bridge_disable(void)
{
    uint32_t irq = lock();
    if (state.enabled) {
        if (state.tx_count || !(line_status() & RB_LSR_TX_ALL_EMP)) { unlock(irq); return false; }
        receive(false);
    }
    bool owned = state.enabled || state.enable_pending;
    if (owned) {
        PFIC_DisableIRQ(UART_IRQn);
        R8_UART_IER &= ~(RB_IER_RECV_RDY | RB_IER_LINE_STAT);
        R8_UART_MCR &= ~RB_MCR_INT_OE;
        PFIC_ClearPendingIRQ(UART_IRQn);
    }
    state.enabled = state.enable_pending = false;
    unlock(irq);
    if (owned) uart_log_suspend(false);
    return true;
}

bool uart_bridge_is_enabled(void) { return state.enabled; }

void uart_bridge_poll(void)
{
    if (!state.enabled) return;
    uint32_t irq = lock();
    receive(false); /* Foreground fallback serialized with IRQ RX. */
    unlock(irq);
    uint8_t used = R8_UART_TFC;
    unsigned room = used < UART_FIFO_SIZE ? UART_FIFO_SIZE - used : 0;
    while (room-- && state.tx_count) {
        UART_SendByte(tx[tx_tail]);
        tx_tail = (tx_tail + 1u) % UART_BRIDGE_TX_CAPACITY;
        --state.tx_count;
        ++state.tx_sent;
    }
}

size_t uart_bridge_read(uint8_t *data, size_t capacity)
{
    if (!data) return 0;
    uint32_t irq = lock();
    size_t count = capacity < state.rx_count ? capacity : state.rx_count;
    unlock(irq);
    for (size_t i = 0; i < count; ++i) {
        irq = lock();
        uint8_t byte = rx[rx_tail];
        rx_tail = (rx_tail + 1u) % UART_BRIDGE_RX_CAPACITY;
        --state.rx_count;
        ++state.rx_read;
        unlock(irq);
        data[i] = byte;
    }
    return count;
}

size_t uart_bridge_write(const uint8_t *data, size_t length)
{
    if (!state.enabled || !data) return 0;
    size_t room = UART_BRIDGE_TX_CAPACITY - state.tx_count;
    size_t count = length < room ? length : room;
    for (size_t i = 0; i < count; ++i) {
        tx[tx_head] = data[i];
        tx_head = (tx_head + 1u) % UART_BRIDGE_TX_CAPACITY;
    }
    state.tx_count += (uint16_t)count;
    state.tx_accepted += (uint32_t)count;
    return count;
}

void uart_bridge_get_snapshot(uart_bridge_snapshot_t *snapshot)
{
    if (!snapshot) return;
    uint32_t irq = lock();
    *snapshot = state;
    snapshot->rx_room = UART_BRIDGE_RX_CAPACITY - state.rx_count;
    snapshot->tx_room = state.enabled ? UART_BRIDGE_TX_CAPACITY - state.tx_count : 0;
    unlock(irq);
}
