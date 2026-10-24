/* Basic 2.4 GHz transport based on WCH EVT RF_Basic (Apache-2.0). */
#include "CH57x_common.h"
#include "CH572rf.h"
#include "radio.h"
#include "platform.h"

/* Pinned CH57x_RF_BASIC_LIB_V1.1 ABI (CH572rf.h V1.10): objdump shows
 * RFRole_Stop merely sets LLE abort and ALWAYS returns zero; it does not wait.
 * RFIP_StartTx/SetRx write LLE[0] = 2/1; SetPHYIdle tests LLE[0]&3.
 * LLE IRQ synchronously dispatches (LLE[2]&LLE[3]); BB IRQ consumes bit 6
 * of BB[16] and can raise LLE timeout. rfBasicProcess calls our callback
 * synchronously, without a deferred queue; callback id is reserved, not a tag.
 * These exported register pointers are intentionally private-SDK dependencies.
 * Re-audit these offsets/semantics when changing the pinned RF library.
 * A time delay or Stop success alone is NOT a DMA/callback cancellation fence.
 */
extern volatile uint32_t *gptrLLEReg;
extern volatile uint32_t *gptrBBReg;
#define TX_WATCHDOG_MS 100u
#define RECOVERY_RETRY_MS 10u
#define BASIC_LLE_EVENTS ((1u << 0) | (1u << 1) | (1u << 17))
static rf_config_t active_config;
static uint8_t initialized, paused;
static rfipTx_t tx;
static rfipRx_t rx;
static uint8_t tx_dma[RADIO_FRAME_SIZE + 2] __attribute__((aligned(4)));
static uint8_t rx_dma[264] __attribute__((aligned(4)));
static uint8_t mailbox[RADIO_FRAME_SIZE];
static volatile uint8_t ready, sending, isolated, recovering, rx_needed;
static uint8_t recovery_wait;
static uint32_t tx_started, recovery_at;
volatile uint32_t radio_rx_count, radio_crc_errors, radio_tx_count, radio_drops;
static volatile uint32_t tx_failures, tx_timeouts, rx_timeouts;
static volatile uint32_t tx_watchdogs, recovery_attempts, recovery_failures;
static volatile uint32_t recoveries, stale_events;

static bool hardware_idle(void)
{
    return gptrLLEReg && gptrBBReg && !(gptrLLEReg[0] & 3u);
}
static bool pending_sources(void)
{
    return (gptrLLEReg[2] & (gptrLLEReg[3] | BASIC_LLE_EVENTS)) || (gptrBBReg[16] & 0x40u);
}
/* IRQs are globally masked. Keep isolated set throughout Stop/drain, including
 * synchronous SDK callbacks. Never clear ready or touch tx_dma here.
 * Success requires observed hardware idle, not just RFRole_Stop's return code.
 * Failed stop/abort leaves sending owned until a later successful fence.
 */
static bool stop_fence(void)
{
    isolated = 1;
    if (RFRole_Stop() || !hardware_idle()) return false;
    /* Disabled old RX/TX/timeout flags can become live when the next SDK call
     * changes its mask. Drain those too; rfBasicProcess maps bits 0/1/17.
     * Do not clear PFIC alone: that does not acknowledge peripheral sources. */
    uint32_t previous_mask = gptrLLEReg[3];
    gptrLLEReg[3] = previous_mask | BASIC_LLE_EVENTS;
    for (unsigned i = 0; i < 2; ++i) {
        BB_LibIRQHandler();
        LLE_LibIRQHandler();
    }
    gptrLLEReg[3] = previous_mask;
    /* BB dispatch may have programmed the receive-match timeout; reassert
     * Stop to disable its timer before releasing the cancellation fence. */
    if (RFRole_Stop() || !hardware_idle() || pending_sources()) return false;
    PFIC_ClearPendingIRQ(BLEB_IRQn);
    PFIC_ClearPendingIRQ(BLEL_IRQn);
    return hardware_idle() && !pending_sources();
}
static void need_recovery(void)
{
    recovering = isolated = 1;
    recovery_wait = 0;
    rx_needed = 1;
}
static __HIGH_CODE bStatus_t start_rx(void)
{
    if (paused || isolated || sending) return 1;
    /* Set before the call: a synchronous callback may require another rearm. */
    rx_needed = 0;
    bStatus_t result = RFIP_SetRx(&rx);
    if (result) { ++rx_timeouts; rx_needed = 1; }
    return result;
}
static __HIGH_CODE void radio_callback(rfRole_States_t state, uint8_t id)
{
    (void)id; /* SDK sample explicitly calls this reserved; never an epoch. */
    if ((state & RF_STATE_RX) && !(state & RF_STATE_RX_CRCERR)) {
        if (!ready && rx_dma[1] == RADIO_FRAME_SIZE) {
            for (unsigned i = 0; i < RADIO_FRAME_SIZE; ++i) mailbox[i] = rx_dma[i + 2];
            __asm__ volatile("" ::: "memory");
            ready = 1;
            ++radio_rx_count;
        } else ++radio_drops;
    }
    if (state & RF_STATE_RX_CRCERR) ++radio_crc_errors;
    if (isolated) {
        if (state & (RF_STATE_TX_FINISH | RF_STATE_TIMEOUT)) ++stale_events;
        return;
    }
    /* A combined terminal event completes a TX once, success taking precedence.
     * Re-arm only in foreground: no nested SetRx during StartTx/Stop callbacks.
     */
    if (sending && (state & (RF_STATE_TX_FINISH | RF_STATE_TIMEOUT))) {
        sending = 0;
        if (state & RF_STATE_TX_FINISH) ++radio_tx_count;
        else { ++tx_timeouts; ++tx_failures; }
        rx_needed = 1;
    } else {
        if (state & RF_STATE_TX_FINISH) ++stale_events;
        if (state & RF_STATE_TIMEOUT) { ++rx_timeouts; rx_needed = 1; }
    }
    if (state & (RF_STATE_RX | RF_STATE_RX_CRCERR)) rx_needed = 1;
}
__INTERRUPT __HIGH_CODE void BB_IRQHandler(void) { BB_LibIRQHandler(); }
__INTERRUPT __HIGH_CODE void LLE_IRQHandler(void) { LLE_LibIRQHandler(); }

void radio_init(void)
{
    rfRoleConfig_t config = {0};
    config.rfProcessCB = radio_callback;
    config.processMask = RF_STATE_RX | RF_STATE_RX_CRCERR | RF_STATE_TX_FINISH | RF_STATE_TIMEOUT;
    uint32_t irq = __risc_v_disable_irq();
    isolated = 1;
    bStatus_t result = RFRole_BasicInit(&config);
    rf_config_defaults(&active_config);
    rx.accessAddress = tx.accessAddress = active_config.access_address;
    rx.crcInit = tx.crcInit = 0x555555;
    rx.crcPoly = tx.crcPoly = 0x80032d;
    rx.frequency = tx.frequency = active_config.channel;
    rx.properties = tx.properties = PHY_MODE_PHY_2M;
    rx.rxDMA = (uint32_t)(uintptr_t)rx_dma;
    rx.rxMaxLen = RADIO_FRAME_SIZE;
    rx.timeOut = 0;
    tx.txDMA = (uint32_t)(uintptr_t)tx_dma;
    tx.txLen = RADIO_FRAME_SIZE;
    tx.waitTime = 160; /* 80 us, SDK half-microsecond units. */
    tx.txPowerVal = LL_TX_PWR_0_DBM;
    ready = sending = paused = recovering = recovery_wait = 0;
    initialized = result == 0;
    rx_needed = 1;
    if (!initialized) paused = 1;
    else if (!stop_fence()) need_recovery();
    else {
        isolated = 0;
        if (start_rx()) need_recovery();
    }
    PFIC_EnableIRQ(BLEB_IRQn);
    PFIC_EnableIRQ(BLEL_IRQn);
    __risc_v_enable_irq(irq);
}
void radio_poll(uint32_t now)
{
    uint32_t irq = __risc_v_disable_irq();
    if (!initialized || paused) { __risc_v_enable_irq(irq); return; }
    if (sending && !recovering && (uint32_t)(now - tx_started) >= TX_WATCHDOG_MS) {
        ++tx_watchdogs; ++tx_timeouts; ++tx_failures;
        need_recovery();
    }
    if (recovering) {
        if (recovery_wait && (uint32_t)(now - recovery_at) < RECOVERY_RETRY_MS) {
            __risc_v_enable_irq(irq); return;
        }
        recovery_wait = 1; recovery_at = now;
        ++recovery_attempts;
        if (!stop_fence()) ++recovery_failures;
        else {
            sending = 0; /* Hardware idle and old IRQ sources drained. */
            isolated = 0;
            if (start_rx()) { isolated = 1; ++recovery_failures; }
            else { recovering = recovery_wait = 0; ++recoveries; }
        }
    } else if (!sending && rx_needed) {
        /* Fence before SetRx too: a late terminal event must not be attributed
         * to a new operation. SetRx itself can abort a still-active PHY. */
        if (!stop_fence()) need_recovery();
        else {
            isolated = 0;
            if (start_rx()) need_recovery();
        }
    }
    __risc_v_enable_irq(irq);
}
bool radio_send(const uint8_t frame[RADIO_FRAME_SIZE])
{
    if (!frame) return false;
    uint32_t irq = __risc_v_disable_irq();
    if (!initialized || paused) { ++tx_failures; __risc_v_enable_irq(irq); return false; }
    if (sending || recovering) { __risc_v_enable_irq(irq); return false; }
    if (!stop_fence()) {
        ++tx_failures; need_recovery();
        __risc_v_enable_irq(irq); return false;
    }
    tx_dma[0] = 0x55; tx_dma[1] = RADIO_FRAME_SIZE;
    for (unsigned i = 0; i < RADIO_FRAME_SIZE; ++i) tx_dma[i + 2] = frame[i];
    isolated = 0; rx_needed = 0; sending = 1;
    bStatus_t result = RFIP_StartTx(&tx);
    if (!result) tx_started = platform_millis();
    else {
        ++tx_failures;
        /* Do not infer quiescence from an SDK error; fence before DMA reuse. */
        need_recovery();
    }
    __risc_v_enable_irq(irq);
    return result == 0;
}
bool radio_receive(uint8_t frame[RADIO_FRAME_SIZE])
{
    if (!frame) return false;
    uint32_t irq = __risc_v_disable_irq();
    if (!ready) { __risc_v_enable_irq(irq); return false; }
    for (unsigned i = 0; i < RADIO_FRAME_SIZE; ++i) frame[i] = mailbox[i];
    ready = 0;
    __risc_v_enable_irq(irq);
    return true;
}
bool radio_reconfigure(const rf_config_t *config)
{
    uint8_t power;
    if (!rf_config_valid(config) || !rf_config_tx_power_sdk(config->tx_power_dbm, &power)) return false;
    uint32_t irq = __risc_v_disable_irq();
    if (!initialized || sending || ready || recovering) { __risc_v_enable_irq(irq); return false; }
    if (!stop_fence()) { need_recovery(); __risc_v_enable_irq(irq); return false; }
    rf_config_t previous = active_config;
    active_config = *config;
    rx.frequency = tx.frequency = config->channel;
    rx.accessAddress = tx.accessAddress = config->access_address;
    tx.txPowerVal = (int8_t)power;
    isolated = 0;
    if (!paused && start_rx()) {
        /* Restore parameters only after proving failed SetRx no longer runs. */
        if (!stop_fence()) { need_recovery(); __risc_v_enable_irq(irq); return false; }
        active_config = previous;
        rx.frequency = tx.frequency = previous.channel;
        rx.accessAddress = tx.accessAddress = previous.access_address;
        rf_config_tx_power_sdk(previous.tx_power_dbm, &power);
        tx.txPowerVal = (int8_t)power;
        need_recovery();
        __risc_v_enable_irq(irq); return false;
    }
    __risc_v_enable_irq(irq); return true;
}
void radio_get_config(rf_config_t *config)
{
    if (!config) return;
    uint32_t irq = __risc_v_disable_irq(); *config = active_config; __risc_v_enable_irq(irq);
}
void radio_get_stats(radio_stats_t *stats)
{
    if (!stats) return;
    uint32_t irq = __risc_v_disable_irq();
    stats->rx_count = radio_rx_count; stats->crc_errors = radio_crc_errors;
    stats->tx_count = radio_tx_count; stats->drops = radio_drops;
    stats->tx_failures = tx_failures; stats->tx_timeouts = tx_timeouts; stats->rx_timeouts = rx_timeouts;
    stats->tx_watchdogs = tx_watchdogs; stats->recovery_attempts = recovery_attempts;
    stats->recovery_failures = recovery_failures; stats->recoveries = recoveries;
    stats->stale_events = stale_events;
    __risc_v_enable_irq(irq);
}
bool radio_is_idle(void)
{
    uint32_t irq = __risc_v_disable_irq();
    bool idle = initialized && !sending && !ready && !recovering && !isolated;
    __risc_v_enable_irq(irq); return idle;
}
bool radio_pause(void)
{
    uint32_t irq = __risc_v_disable_irq();
    bool ok = initialized && !sending && !ready && !recovering;
    if (ok && !paused) {
        ok = stop_fence();
        if (ok) paused = 1;
        else need_recovery();
    }
    __risc_v_enable_irq(irq); return ok;
}
void radio_resume(void)
{
    uint32_t irq = __risc_v_disable_irq();
    if (initialized && paused) { paused = 0; need_recovery(); }
    __risc_v_enable_irq(irq);
}
bool radio_is_paused(void) { return paused != 0; }
bool radio_get_hardware_id(uint8_t id[RF_CONFIG_ID_SIZE])
{
    uint8_t factory[8] __attribute__((aligned(4))) = {0};
    if (!id) return false;
    uint32_t irq = __risc_v_disable_irq();
    uint32_t result = GetMACAddress(factory);
    __risc_v_enable_irq(irq);
    if (result) return false;
    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < 6; ++i) { any |= factory[i]; all &= factory[i]; }
    if (!any || all == 0xff) return false;
    uint16_t sum = (uint16_t)((factory[0] | (factory[1] << 8)) +
        (factory[2] | (factory[3] << 8)) + (factory[4] | (factory[5] << 8)));
    factory[6] = (uint8_t)sum; factory[7] = (uint8_t)(sum >> 8);
    for (unsigned i = 0; i < RF_CONFIG_ID_SIZE; ++i) id[i] = factory[i];
    return true;
}
