#ifndef WD_RADIO_H
#define WD_RADIO_H
#include <stdbool.h>
#include <stdint.h>
#include "rf_config.h"
#define RADIO_FRAME_SIZE 78u

typedef struct {
    uint32_t rx_count, crc_errors, tx_count, drops;
    uint32_t tx_failures, tx_timeouts, rx_timeouts;
    /* Appended diagnostics; original seven fields retain order/meaning.
     * Watchdogs count missing TX terminals, attempts/failures count poll's
     * recovery fences, recoveries count successful RX restart, stale_events
     * count terminal callbacks suppressed during isolation or stray FINISH. */
    uint32_t tx_watchdogs, recovery_attempts, recovery_failures, recoveries, stale_events;
} radio_stats_t;

void radio_init(void);
/* Foreground, every main-loop iteration, including a faulted application.
 * now must be from platform_millis(); 100 ms TX watchdog, 10 ms recovery retry.
 * Bounded: no busy wait; preserves an unread mailbox. Uses pinned SDK's LLE/BB
 * register ABI to observe hardware idle and drain old IRQs before DMA reuse.
 * SDK upgrades require re-auditing radio.c's documented cancellation fence. */
void radio_poll(uint32_t now);
bool radio_send(const uint8_t frame[RADIO_FRAME_SIZE]);
bool radio_receive(uint8_t frame[RADIO_FRAME_SIZE]);
/* Foreground only. Reconfiguration refuses pending TX or an unread mailbox.
 * The RX engine restarts immediately unless explicitly paused. */
bool radio_reconfigure(const rf_config_t *config);
void radio_get_config(rf_config_t *config);
void radio_get_stats(radio_stats_t *stats); /* Atomic, monotonically wrapping counters. */
/* Idle excludes recovery/isolation and unread mailbox; RX may be listening. */
bool radio_is_idle(void);
/* Pause stops RX for scheduler sleep / reserved-flash writes. */
bool radio_pause(void);
void radio_resume(void);
bool radio_is_paused(void);
/* Official 8-byte ID: six factory MAC bytes + little-endian word checksum.
 * Output is unchanged if the ROM command fails or factory MAC is blank. */
bool radio_get_hardware_id(uint8_t id[RF_CONFIG_ID_SIZE]);
#endif
