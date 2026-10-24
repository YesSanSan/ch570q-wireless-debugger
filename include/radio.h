#ifndef WD_RADIO_H
#define WD_RADIO_H
#include <stdbool.h>
#include <stdint.h>
#include "rf_config.h"
#define RADIO_FRAME_SIZE 78u

typedef struct {
    uint32_t rx_count, crc_errors, tx_count, drops;
    uint32_t tx_failures, tx_timeouts, rx_timeouts;
} radio_stats_t;

void radio_init(void);
bool radio_send(const uint8_t frame[RADIO_FRAME_SIZE]);
bool radio_receive(uint8_t frame[RADIO_FRAME_SIZE]);
/* Foreground only. Reconfiguration refuses pending TX or an unread mailbox.
 * The RX engine restarts immediately unless explicitly paused. */
bool radio_reconfigure(const rf_config_t *config);
void radio_get_config(rf_config_t *config);
void radio_get_stats(radio_stats_t *stats); /* Atomic, monotonically wrapping counters. */
/* Idle = initialized, no active TX and no unread mailbox; RX may be listening. */
bool radio_is_idle(void);
/* Pause stops RX for scheduler sleep / reserved-flash writes. */
bool radio_pause(void);
void radio_resume(void);
bool radio_is_paused(void);
/* Official 8-byte ID: six factory MAC bytes + little-endian word checksum.
 * Output is unchanged if the ROM command fails or factory MAC is blank. */
bool radio_get_hardware_id(uint8_t id[RF_CONFIG_ID_SIZE]);
#endif
