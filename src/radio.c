/* Basic 2.4 GHz transport based on WCH EVT RF_Basic (Apache-2.0).
 * One fixed-length DMA frame, hardware CRC, foreground-owned TX.
 */
#include "CH57x_common.h"
#include "CH572rf.h"
#include "radio.h"
static rf_config_t active_config;
static uint8_t initialized, paused;
static rfipTx_t tx;
static rfipRx_t rx;
static uint8_t tx_dma[RADIO_FRAME_SIZE + 2] __attribute__((aligned(4)));
static uint8_t rx_dma[264] __attribute__((aligned(4)));
static uint8_t mailbox[RADIO_FRAME_SIZE];
static volatile uint8_t ready, sending;
volatile uint32_t radio_rx_count, radio_crc_errors, radio_tx_count, radio_drops;
static volatile uint32_t tx_failures, tx_timeouts, rx_timeouts;

static __HIGH_CODE bStatus_t start_rx(void)
{
    return paused ? 0 : RFIP_SetRx(&rx);
}

static __HIGH_CODE void radio_callback(rfRole_States_t state, uint8_t id)
{
    (void)id;
    if ((state & RF_STATE_RX) && !(state & RF_STATE_RX_CRCERR)) {
        if (!ready && rx_dma[1] == RADIO_FRAME_SIZE) {
            for (unsigned i = 0; i < RADIO_FRAME_SIZE; ++i)
                mailbox[i] = rx_dma[i + 2];
            __asm__ volatile("" ::: "memory");
            ready = 1;
            ++radio_rx_count;
        } else {
            ++radio_drops;
        }
    }
    if (state & RF_STATE_RX_CRCERR) ++radio_crc_errors;
    if (state & RF_STATE_TX_FINISH) {
        sending = 0;
        ++radio_tx_count;
    }
    if (state & RF_STATE_TIMEOUT) {
        if (sending) { ++tx_timeouts; ++tx_failures; }
        else ++rx_timeouts;
        sending = 0;
    }
    if (state & (RF_STATE_RX | RF_STATE_RX_CRCERR | RF_STATE_TX_FINISH | RF_STATE_TIMEOUT))
        start_rx();
}

__INTERRUPT __HIGH_CODE void BB_IRQHandler(void) { BB_LibIRQHandler(); }
__INTERRUPT __HIGH_CODE void LLE_IRQHandler(void) { LLE_LibIRQHandler(); }

void radio_init(void)
{
    rfRoleConfig_t config = {0};
    /* Preserve SetSysClock's PLL settings for USB+RF operation.
     * R32_MISC_CTRL[29:24] aliases R8_PLL_CONFIG; the official combined
     * RF_UartDongle example does not apply RF_Basic's extra PLL retune. */
    config.rfProcessCB = radio_callback;
    config.processMask = RF_STATE_RX | RF_STATE_RX_CRCERR | RF_STATE_TX_FINISH | RF_STATE_TIMEOUT;
    bStatus_t init_result = RFRole_BasicInit(&config);
    /* WCH's PHY_2M packet format, proprietary link (no BLE stack). */
    rf_config_defaults(&active_config);
    rx.accessAddress = tx.accessAddress = active_config.access_address;
    rx.crcInit = tx.crcInit = 0x555555;
    rx.crcPoly = tx.crcPoly = 0x80032d; /* SDK RF_Basic polynomial representation. */
    rx.frequency = tx.frequency = active_config.channel;
    rx.properties = tx.properties = PHY_MODE_PHY_2M;
    rx.rxDMA = (uint32_t)rx_dma;
    rx.rxMaxLen = RADIO_FRAME_SIZE;
    rx.timeOut = 0;
    tx.txDMA = (uint32_t)tx_dma;
    tx.txLen = RADIO_FRAME_SIZE;
    tx.waitTime = 160; /* 80us, SDK uses half-microsecond units. */
    tx.txPowerVal = LL_TX_PWR_0_DBM;
    ready = sending = paused = 0;
    initialized = init_result == 0;
    if (!initialized) paused = 1;
    PFIC_EnableIRQ(BLEB_IRQn);
    PFIC_EnableIRQ(BLEL_IRQn);
    start_rx();
}

bool radio_send(const uint8_t frame[RADIO_FRAME_SIZE])
{
    if (!frame) return false;
    uint32_t irq = __risc_v_disable_irq();
    if (!initialized || paused || sending) { __risc_v_enable_irq(irq); return false; }
    if (RFRole_Stop()) {
        ++tx_failures;
        __risc_v_enable_irq(irq);
        return false;
    }
    tx_dma[0] = 0x55;
    tx_dma[1] = RADIO_FRAME_SIZE;
    for (unsigned i = 0; i < RADIO_FRAME_SIZE; ++i) tx_dma[i + 2] = frame[i];
    sending = 1;
    bStatus_t result = RFIP_StartTx(&tx);
    if (result) { ++tx_failures; sending = 0; start_rx(); }
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
    if (!rf_config_valid(config) || !rf_config_tx_power_sdk(config->tx_power_dbm, &power))
        return false;
    uint32_t irq = __risc_v_disable_irq();
    if (!initialized || sending || ready || (!paused && RFRole_Stop())) {
        __risc_v_enable_irq(irq);
        return false;
    }
    rf_config_t previous = active_config;
    active_config = *config;
    rx.frequency = tx.frequency = config->channel;
    rx.accessAddress = tx.accessAddress = config->access_address;
    tx.txPowerVal = (int8_t)power;
    if (start_rx()) {
        active_config = previous;
        rx.frequency = tx.frequency = previous.channel;
        rx.accessAddress = tx.accessAddress = previous.access_address;
        rf_config_tx_power_sdk(previous.tx_power_dbm, &power);
        tx.txPowerVal = (int8_t)power;
        if (start_rx()) paused = 1;
        __risc_v_enable_irq(irq);
        return false;
    }
    __risc_v_enable_irq(irq);
    return true;
}

void radio_get_config(rf_config_t *config)
{
    if (!config) return;
    uint32_t irq = __risc_v_disable_irq();
    *config = active_config;
    __risc_v_enable_irq(irq);
}

void radio_get_stats(radio_stats_t *stats)
{
    if (!stats) return;
    uint32_t irq = __risc_v_disable_irq();
    stats->rx_count = radio_rx_count;
    stats->crc_errors = radio_crc_errors;
    stats->tx_count = radio_tx_count;
    stats->drops = radio_drops;
    stats->tx_failures = tx_failures;
    stats->tx_timeouts = tx_timeouts;
    stats->rx_timeouts = rx_timeouts;
    __risc_v_enable_irq(irq);
}

bool radio_is_idle(void)
{
    uint32_t irq = __risc_v_disable_irq();
    bool idle = initialized && !sending && !ready;
    __risc_v_enable_irq(irq);
    return idle;
}

bool radio_pause(void)
{
    uint32_t irq = __risc_v_disable_irq();
    bool ok = initialized && !sending && !ready;
    if (ok && !paused) {
        ok = RFRole_Stop() == 0;
        if (ok) paused = 1;
    }
    __risc_v_enable_irq(irq);
    return ok;
}

void radio_resume(void)
{
    uint32_t irq = __risc_v_disable_irq();
    if (initialized && paused) {
        paused = 0;
        if (start_rx()) paused = 1;
    }
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
    /* Same algorithm as official CH57x_flash.c GET_UNIQUE_ID, with the
     * ROM command result checked instead of the SDK wrapper's void return. */
    uint16_t sum = (uint16_t)((factory[0] | (factory[1] << 8)) +
        (factory[2] | (factory[3] << 8)) + (factory[4] | (factory[5] << 8)));
    factory[6] = (uint8_t)sum;
    factory[7] = (uint8_t)(sum >> 8);
    for (unsigned i = 0; i < RF_CONFIG_ID_SIZE; ++i) id[i] = factory[i];
    return true;
}
