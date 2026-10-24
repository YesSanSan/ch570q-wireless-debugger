/* Native fault injection of the REAL radio.c, not an app-level boolean mock.
 * gcc -std=c11 -Wall -Wextra -Werror -Itests/radio_mock -Iinclude \
 *     tests/test_radio.c -o build/test_radio.exe && build/test_radio.exe
 * Register layout/IRQ model follows the pinned RF binary documented in radio.c.
 * This proves firmware ownership/fencing logic under that model, not RF silicon.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "CH57x_common.h"
#include "CH572rf.h"
#include "radio.h"
static volatile uint32_t lle[32], bb[32];
volatile uint32_t *gptrLLEReg = lle;
volatile uint32_t *gptrBBReg = bb;
static unsigned irq_mask, pending_b, pending_l, stop_calls, rx_calls, tx_calls;
static unsigned drain_calls, in_sdk, fail_stop_on_call;
static uint32_t fake_now, sync_stop, sync_tx, sync_rx, queued_terminal;
static int fail_stop, abort_pending, fail_rx, fail_tx, fail_init, stick_source;
static pfnRfRoleProcess callback;
static uint8_t tx_snapshot[RADIO_FRAME_SIZE + 2];
static int dma_owned;
#include "../src/radio.c"

uint32_t __risc_v_disable_irq(void) { uint32_t old = irq_mask; irq_mask = 1; return old; }
void __risc_v_enable_irq(uint32_t saved) { irq_mask = saved; }
void PFIC_ClearPendingIRQ(int irq) {
    assert(irq_mask);
    if (irq == BLEB_IRQn) pending_b = 0; else { assert(irq == BLEL_IRQn); pending_l = 0; }
}
void PFIC_EnableIRQ(int irq) { assert(irq == BLEB_IRQn || irq == BLEL_IRQn); }
uint32_t platform_millis(void) { return fake_now; }
uint32_t GetMACAddress(uint8_t *id) { memset(id, 1, 6); return 0; }
void rf_config_defaults(rf_config_t *c) { memset(c, 0, sizeof(*c)); c->channel = 16; c->access_address = WD_RF_ADDRESS; }
bool rf_config_valid(const rf_config_t *c) { return c && c->channel < 40; }
bool rf_config_tx_power_sdk(int8_t p, uint8_t *v) { if (p) return false; if (v) *v = 0x12; return true; }
static void assert_dma(void) { if (dma_owned) assert(!memcmp(tx_snapshot, tx_dma, sizeof(tx_snapshot))); }
static void deliver(uint32_t state) {
    if (state & (RF_STATE_TX_FINISH | RF_STATE_TIMEOUT)) { assert_dma(); lle[0] = 0; dma_owned = 0; }
    callback(state, 0);
}
bStatus_t RFRole_BasicInit(rfRoleConfig_t *c) { assert(irq_mask); callback = c->rfProcessCB; return fail_init; }
bStatus_t RFRole_Stop(void) {
    assert(irq_mask); assert_dma(); ++stop_calls;
    if (sync_stop) { uint32_t s = sync_stop; sync_stop = 0; callback(s, 0); }
    if (fail_stop || (fail_stop_on_call && stop_calls == fail_stop_on_call)) return 1;
    if (!abort_pending) { lle[0] = 0; dma_owned = 0; }
    return 0;
}
bStatus_t RFIP_StartTx(rfipTx_t *c) {
    assert(irq_mask); assert(!in_sdk); assert(!dma_owned); assert(lle[0] == 0);
    assert(!pending_b && !pending_l && !queued_terminal); assert(!pending_sources());
    assert(c->txLen == RADIO_FRAME_SIZE); ++tx_calls;
    memcpy(tx_snapshot, tx_dma, sizeof(tx_snapshot)); dma_owned = 1; lle[0] = 2;
    in_sdk = 1;
    if (sync_tx) { uint32_t s = sync_tx; sync_tx = 0; deliver(s); }
    in_sdk = 0;
    return fail_tx;
}
bStatus_t RFIP_SetRx(rfipRx_t *c) {
    assert(irq_mask); assert(!in_sdk); assert(!dma_owned); assert(lle[0] == 0);
    assert(c->rxMaxLen == RADIO_FRAME_SIZE); ++rx_calls;
    in_sdk = 1;
    if (sync_rx) { uint32_t s = sync_rx; sync_rx = 0; callback(s, 0); }
    in_sdk = 0;
    if (fail_rx) return 1;
    lle[0] = 1; return 0;
}
void BB_LibIRQHandler(void) {
    assert(irq_mask); ++drain_calls;
    if (bb[16] & 0x40u) {
        bb[16] = 0;
        lle[2] |= 1u << 17; queued_terminal |= RF_STATE_TIMEOUT;
        pending_l = 1;
    }
}
void LLE_LibIRQHandler(void) {
    assert(irq_mask); ++drain_calls;
    if (lle[2] & lle[3]) {
        if (!stick_source) lle[2] = 0;
        uint32_t s = queued_terminal; queued_terminal = 0;
        if (s) callback(s, 0);
    }
}
static void terminal(uint32_t s) { uint32_t old = irq_mask; irq_mask = 1; deliver(s); irq_mask = old; }
static void queue_old(uint32_t state) {
    uint32_t hw = ((state & RF_STATE_TX_FINISH) ? 2u : 0u) |
                  ((state & RF_STATE_TIMEOUT) ? (1u << 17) : 0u) |
                  ((state & RF_STATE_RX) ? 1u : 0u);
    lle[2] |= hw; lle[3] |= hw; queued_terminal |= state; pending_l = 1;
}
static void packet(uint8_t v) {
    rx_dma[1] = RADIO_FRAME_SIZE; memset(rx_dma + 2, v, RADIO_FRAME_SIZE);
    terminal(RF_STATE_RX);
}
static void poll_at(uint32_t now) { fake_now = now; radio_poll(now); assert(!irq_mask); assert_dma(); }
static void reset(void) {
    memset((void *)lle, 0, sizeof(lle)); memset((void *)bb, 0, sizeof(bb));
    irq_mask = pending_b = pending_l = stop_calls = rx_calls = tx_calls = drain_calls = 0;
    in_sdk = fail_stop_on_call = 0; fake_now = sync_stop = sync_tx = sync_rx = queued_terminal = 0;
    fail_stop = abort_pending = fail_rx = fail_tx = fail_init = stick_source = dma_owned = 0;
    radio_rx_count = radio_crc_errors = radio_tx_count = radio_drops = 0;
    tx_failures = tx_timeouts = rx_timeouts = tx_watchdogs = 0;
    recovery_attempts = recovery_failures = recoveries = stale_events = 0;
    radio_init(); assert(!irq_mask); assert(lle[0] == 1); assert(radio_is_idle());
}
static uint8_t a[RADIO_FRAME_SIZE], b[RADIO_FRAME_SIZE];
static void test_normal_sync_and_combined(void) {
    reset(); memset(a, 0xa1, sizeof(a)); memset(b, 0xb2, sizeof(b));
    fake_now = 7; assert(radio_send(a)); assert(sending && tx_started == 7);
    assert(!radio_send(b)); assert_dma(); assert(!radio_is_idle());
    terminal(RF_STATE_TX_FINISH | RF_STATE_TIMEOUT);
    assert(!sending && radio_tx_count == 1 && !tx_timeouts);
    poll_at(8); assert(lle[0] == 1);
    sync_tx = RF_STATE_TX_FINISH; assert(radio_send(b));
    assert(!sending && radio_tx_count == 2); /* No StartTx->SetRx recursion. */
    poll_at(9); assert(lle[0] == 1);
    sync_tx = RF_STATE_TIMEOUT; assert(radio_send(a));
    assert(!sending && tx_timeouts == 1); poll_at(10);
}
static void test_watchdog_independent_and_mailbox(void) {
    reset(); fake_now = 200; assert(radio_send(a)); packet(0x6d);
    poll_at(299); assert(sending && !tx_watchdogs);
    /* No radio_send call is needed: application may already be faulted. */
    poll_at(300); assert(!sending && !recovering && recoveries == 1 && tx_watchdogs == 1);
    assert(ready && !radio_is_idle());
    uint8_t out[RADIO_FRAME_SIZE]; assert(radio_receive(out));
    for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0x6d);
    assert(!radio_receive(out)); assert(radio_send(b));
}
static void test_failed_stop_and_async_abort(void) {
    reset(); fake_now = 5; assert(radio_send(a)); fail_stop = 1;
    poll_at(105); assert(sending && recovering && !radio_is_idle());
    assert(recovery_attempts == 1 && recovery_failures == 1);
    assert(!radio_send(b)); assert_dma(); unsigned n = stop_calls;
    poll_at(114); assert(stop_calls == n); /* Retry budget bounded. */
    fail_stop = 0; abort_pending = 1;
    poll_at(115); assert(sending && recovering); assert_dma();
    /* Stop returned success, but mode still TX: no DMA reuse or RX start. */
    assert(tx_calls == 1 && rx_calls == 1);
    terminal(RF_STATE_TIMEOUT); assert(!radio_tx_count && sending); /* isolated */
    abort_pending = 0; poll_at(125); assert(!sending && !recovering);
    assert(stale_events == 1); assert(radio_send(b));
}
static void test_late_sources_and_synchronous_stop(void) {
    reset(); assert(radio_send(a)); fail_stop = 1;
    poll_at(100); assert(recovering);
    /* A completion arriving after watchdog cannot turn recovery into idle. */
    terminal(RF_STATE_TX_FINISH); assert(sending && recovering && !radio_tx_count);
    queue_old(RF_STATE_TX_FINISH); bb[16] = 0x40; pending_b = 1;
    lle[3] = 0; /* Old flags disabled now, enabled by a future SDK TX. */
    sync_stop = RF_STATE_TIMEOUT; fail_stop = 0;
    poll_at(110); assert(!recovering && !sending && !queued_terminal);
    assert(stale_events >= 3 && !radio_tx_count);
    unsigned before = rx_calls;
    assert(radio_send(b)); assert(sending && dma_owned);
    /* Empty old PFIC delivery does not finish the new TX or rearm RX. */
    irq_mask = 1; LLE_IRQHandler(); BB_IRQHandler(); irq_mask = 0;
    assert(sending && rx_calls == before); assert_dma();
    terminal(RF_STATE_TX_FINISH); assert(radio_tx_count == 1);
}
static void test_rearm_failures_and_ready(void) {
    reset(); packet(0x73); fail_rx = 1; poll_at(1);
    assert(recovering && rx_timeouts == 1 && ready);
    poll_at(2); assert(recovering && recovery_failures == 1 && ready);
    unsigned n = rx_calls; poll_at(11); assert(rx_calls == n);
    fail_rx = 0; poll_at(12); assert(!recovering && recoveries == 1 && ready);
    uint8_t out[RADIO_FRAME_SIZE]; assert(radio_receive(out)); assert(out[0] == 0x73);
    /* RX callbacks can occur synchronously inside SetRx, without recursion. */
    sync_rx = RF_STATE_RX_CRCERR; packet(3); poll_at(13);
    assert(rx_needed && radio_crc_errors == 1); poll_at(14); assert(!rx_needed);
}
static void test_start_failure_dma_and_stuck_source(void) {
    reset(); fail_tx = 1; assert(!radio_send(a)); assert(recovering && dma_owned);
    assert(!radio_send(b)); assert_dma(); fail_stop = 1;
    poll_at(0); assert(dma_owned && sending); assert_dma();
    fail_stop = 0; fail_tx = 0; queue_old(RF_STATE_TIMEOUT); stick_source = 1;
    poll_at(10); assert(recovering && !radio_is_idle()); assert(tx_calls == 1);
    assert(!radio_send(b)); stick_source = 0; poll_at(20);
    assert(!recovering); assert(radio_send(b));
}
static void test_wrap_pause_and_stats(void) {
    reset(); fake_now = UINT32_MAX - 49; assert(radio_send(a));
    poll_at(49); assert(!tx_watchdogs); /* 99 ms */
    fail_stop = 1; poll_at(50); assert(tx_watchdogs == 1);
    poll_at(59); assert(recovery_attempts == 1);
    fail_stop = 0; poll_at(60); assert(!recovering);
    assert(radio_pause()); assert(radio_is_paused());
    unsigned n = rx_calls; poll_at(200); assert(rx_calls == n);
    assert(!radio_send(b)); radio_resume(); assert(recovering); poll_at(201);
    assert(!radio_is_paused() && !recovering);
    radio_stats_t s; memset(&s, 0xa5, sizeof(s)); radio_get_stats(&s);
    assert(s.tx_watchdogs == 1 && s.recoveries == 2 && s.recovery_failures == 1);
    assert(s.recovery_attempts == 3 && s.tx_timeouts == 1);
    /* Recovery retry interval also wraps safely. */
    reset(); fake_now = UINT32_MAX - 109; assert(radio_send(a)); fail_stop = 1;
    poll_at(UINT32_MAX - 9); unsigned attempts = recovery_attempts;
    poll_at(UINT32_MAX); assert(recovery_attempts == attempts);
    fail_stop = 0; poll_at(0); assert(!recovering);
}
static void test_second_stop_and_rx_drain(void) {
    reset(); assert(radio_send(a));
    /* A successful first abort does not excuse failure of the final fence. */
    fail_stop_on_call = stop_calls + 2;
    rx_dma[1] = RADIO_FRAME_SIZE; memset(rx_dma + 2, 0x9e, RADIO_FRAME_SIZE);
    queue_old(RF_STATE_RX | RF_STATE_TX_FINISH); lle[3] = 0;
    poll_at(100);
    assert(recovering && sending && ready && recovery_failures == 1);
    assert(!radio_is_idle() && !radio_send(b));
    assert(tx_calls == 1 && rx_calls == 1 && radio_tx_count == 0);
    fail_stop_on_call = 0; poll_at(110);
    assert(!recovering && !sending && ready);
    /* A second RX arriving while mailbox is full is counted, never replaces it. */
    packet(0x22); assert(radio_drops == 1);
    uint8_t out[RADIO_FRAME_SIZE]; assert(radio_receive(out));
    for (unsigned i = 0; i < sizeof(out); ++i) assert(out[i] == 0x9e);
}
int main(void) {
    test_normal_sync_and_combined();
    test_watchdog_independent_and_mailbox();
    test_failed_stop_and_async_abort();
    test_late_sources_and_synchronous_stop();
    test_rearm_failures_and_ready();
    test_start_failure_dma_and_stuck_source();
    test_wrap_pause_and_stats();
    test_second_stop_and_rx_drain();
    puts("radio fault injection: PASS (8 scenarios)"); return 0;
}
