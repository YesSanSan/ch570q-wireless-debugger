#include "rf_config.h"
#include <string.h>

bool rf_config_tx_power_sdk(int8_t dbm, uint8_t *sdk_value)
{
    /* Exact LL_TX_PWR_* constants from pinned official CH572rf.h v1.10. */
    static const int8_t levels[] = {-25,-20,-15,-10,-8,-5,-3,-1,0,1,2,3,4,5,6};
    static const uint8_t values[] = {0x01,0x02,0x03,0x05,0x07,0x0a,0x0c,0x10,
                                   0x12,0x15,0x18,0x1b,0x1f,0x25,0x2d};
    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        if (levels[i] == dbm) {
            if (sdk_value) *sdk_value = values[i];
            return true;
        }
    }
    return false;
}

void rf_config_defaults(rf_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->channel = WD_RF_CHANNEL;
    config->access_address = WD_RF_ADDRESS;
}

bool rf_config_valid(const rf_config_t *config)
{
    /* SDK accepts a full 32-bit proprietary access address; do not impose
     * BLE connection-address restrictions on this non-BLE link. */
    return config && config->channel <= 39 &&
        config->role <= RF_CONFIG_ROLE_SLAVE &&
        config->idle_timeout_ms <= RF_CONFIG_MAX_IDLE_TIMEOUT_MS &&
        rf_config_tx_power_sdk(config->tx_power_dbm, NULL);
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc32(const uint8_t *p, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

bool rf_config_encode(const rf_config_t *config, uint8_t *record, size_t size)
{
    if (!record || size != RF_CONFIG_RECORD_SIZE || !rf_config_valid(config))
        return false;
    memset(record, 0, size);
    record[0] = 'W'; record[1] = 'R'; record[2] = 'F'; record[3] = 'C';
    record[4] = 1;
    record[5] = RF_CONFIG_RECORD_SIZE;
    record[6] = config->channel;
    record[7] = (uint8_t)config->tx_power_dbm;
    put32(record + 8, config->access_address);
    memcpy(record + 12, config->peer_id, RF_CONFIG_ID_SIZE);
    record[20] = config->role;
    put32(record + 24, config->idle_timeout_ms);
    put32(record + 28, crc32(record, 28));
    return true;
}

bool rf_config_decode(rf_config_t *config, const uint8_t *record, size_t size)
{
    rf_config_t candidate;
    if (!config || !record || size != RF_CONFIG_RECORD_SIZE ||
        memcmp(record, "WRFC", 4) || record[4] != 1 || record[5] != size ||
        record[21] || record[22] || record[23] ||
        get32(record + 28) != crc32(record, 28)) return false;
    memset(&candidate, 0, sizeof(candidate));
    candidate.channel = record[6];
    candidate.tx_power_dbm = record[7] < 128 ? (int8_t)record[7] :
        (int8_t)((int)record[7] - 256);
    candidate.access_address = get32(record + 8);
    memcpy(candidate.peer_id, record + 12, RF_CONFIG_ID_SIZE);
    candidate.role = record[20];
    candidate.idle_timeout_ms = get32(record + 24);
    if (!rf_config_valid(&candidate)) return false;
    *config = candidate;
    return true;
}

/* Each slot lives in a separate erase block. Commit word is written last. */
#define SLOT_SIZE 48u
#define SLOT_COMMIT 0x434f4d54u

static bool slot_decode(const uint8_t *slot, rf_config_t *config)
{
    return !memcmp(slot, "WRS1", 4) && get32(slot + 44) == SLOT_COMMIT &&
        get32(slot + 40) == crc32(slot, 40) &&
        rf_config_decode(config, slot + 8, RF_CONFIG_RECORD_SIZE);
}

static bool storage_valid(const rf_config_storage_t *s)
{
    return s && s->read;
}

/* A modular sequence comparison also handles 0xffffffff -> 0 rollover. */
static unsigned newest_slot(const uint8_t slots[2][SLOT_SIZE], const bool valid[2])
{
    uint32_t delta = get32(slots[1] + 4) - get32(slots[0] + 4);
    return valid[1] && (!valid[0] || (delta != 0 && delta < 0x80000000u));
}

rf_config_status_t rf_config_storage_load(const rf_config_storage_t *s, rf_config_t *config)
{
    uint8_t slots[2][SLOT_SIZE];
    rf_config_t candidates[2];
    bool valid[2];
    if (!storage_valid(s) || !config) return RF_CONFIG_INVALID;
    for (unsigned i = 0; i < 2; ++i) {
        if (!s->read(s->context, i, 0, slots[i], SLOT_SIZE)) return RF_CONFIG_IO_ERROR;
        valid[i] = slot_decode(slots[i], &candidates[i]);
    }
    if (!valid[0] && !valid[1]) return RF_CONFIG_NOT_FOUND;
    *config = candidates[newest_slot(slots, valid)];
    return RF_CONFIG_OK;
}

rf_config_status_t rf_config_storage_save(const rf_config_storage_t *s, const rf_config_t *config)
{
    uint8_t slots[2][SLOT_SIZE], next[SLOT_SIZE], check[SLOT_SIZE];
    rf_config_t candidate;
    bool valid[2];
    if (!storage_valid(s) || !s->erase || !s->write || !rf_config_valid(config))
        return RF_CONFIG_INVALID;
    for (unsigned i = 0; i < 2; ++i) {
        if (!s->read(s->context, i, 0, slots[i], SLOT_SIZE)) return RF_CONFIG_IO_ERROR;
        valid[i] = slot_decode(slots[i], &candidate);
    }
    unsigned current = newest_slot(slots, valid);
    bool have_current = valid[0] || valid[1];
    /* Identical explicit saves consume no erase cycles. */
    rf_config_encode(config, next + 8, RF_CONFIG_RECORD_SIZE);
    if (have_current && !memcmp(next + 8, slots[current] + 8, RF_CONFIG_RECORD_SIZE))
        return RF_CONFIG_OK;
    memcpy(next, "WRS1", 4);
    put32(next + 4, have_current ? get32(slots[current] + 4) + 1u : 0u);
    put32(next + 40, crc32(next, 40));
    put32(next + 44, SLOT_COMMIT);
    unsigned target = have_current ? current ^ 1u : 0u;
    if (!s->erase(s->context, target) ||
        !s->write(s->context, target, 0, next, 44) ||
        !s->read(s->context, target, 0, check, SLOT_SIZE) ||
        memcmp(next, check, 44) || get32(check + 44) != 0xffffffffu ||
        !s->write(s->context, target, 44, next + 44, 4) ||
        !s->read(s->context, target, 0, check, SLOT_SIZE) ||
        memcmp(next, check, SLOT_SIZE) || !slot_decode(check, &candidate))
        return RF_CONFIG_IO_ERROR;
    return RF_CONFIG_OK;
}

#if WD_RF_CONFIG_FLASH_STORAGE
#include "CH57x_common.h"
#include "radio.h"
/* These are program-flash addresses, bounded below the SDK's 0x3c000
 * application limit. 0x3c000..0x3ffff boot/vendor/config space is untouched.
 * Application linker FLASH must end at RF_CONFIG_FLASH_BASE. */
#if FLASH_ROM_MAX_SIZE != 0x3c000 || FLASH_BLOCK_SIZE != RF_CONFIG_FLASH_BLOCK_SIZE
#error Review reserved RF config layout against the selected official ISP SDK
#endif

/* An enabled build must supply this absolute symbol from its reserved linker
 * layout. A stale/unreserved linker script therefore fails to link. */
extern const uint8_t __rf_config_reserved_start[];

static bool target_read(void *context, unsigned slot, unsigned offset, void *data, unsigned size)
{
    (void)context;
    if (slot > 1 || offset > SLOT_SIZE || size > SLOT_SIZE - offset) return false;
    memcpy(data, (const void *)(RF_CONFIG_FLASH_BASE + slot * RF_CONFIG_FLASH_BLOCK_SIZE + offset), size);
    return true;
}

static bool target_erase(void *context, unsigned slot)
{
    (void)context;
    if (slot > 1) return false;
    uint32_t irq = __risc_v_disable_irq();
    uint32_t result = FLASH_ROM_ERASE(RF_CONFIG_FLASH_BASE + slot * RF_CONFIG_FLASH_BLOCK_SIZE,
                                     RF_CONFIG_FLASH_BLOCK_SIZE);
    __risc_v_enable_irq(irq);
    return result == 0;
}

static bool target_write(void *context, unsigned slot, unsigned offset, const void *data, unsigned size)
{
    uint32_t aligned[SLOT_SIZE / 4];
    (void)context;
    if (slot > 1 || offset > SLOT_SIZE || size > SLOT_SIZE - offset ||
        (offset & 3) || (size & 3) || !size) return false;
    memcpy(aligned, data, size);
    uint32_t irq = __risc_v_disable_irq();
    uint32_t result = FLASH_ROM_WRITE(RF_CONFIG_FLASH_BASE + slot * RF_CONFIG_FLASH_BLOCK_SIZE + offset,
                                     aligned, size);
    __risc_v_enable_irq(irq);
    return result == 0;
}

static const rf_config_storage_t target_storage = {NULL, target_read, target_erase, target_write};

rf_config_status_t rf_config_load(rf_config_t *config)
{
    if ((uintptr_t)__rf_config_reserved_start != RF_CONFIG_FLASH_BASE)
        return RF_CONFIG_UNSUPPORTED;
    return rf_config_storage_load(&target_storage, config);
}

rf_config_status_t rf_config_save(const rf_config_t *config)
{
    if (!rf_config_valid(config)) return RF_CONFIG_INVALID;
    if ((uintptr_t)__rf_config_reserved_start != RF_CONFIG_FLASH_BASE)
        return RF_CONFIG_UNSUPPORTED;
    bool was_paused = radio_is_paused();
    if (!radio_pause()) return RF_CONFIG_BUSY;
    rf_config_status_t status = rf_config_storage_save(&target_storage, config);
    if (!was_paused) {
        radio_resume();
        if (radio_is_paused() && status == RF_CONFIG_OK) status = RF_CONFIG_IO_ERROR;
    }
    return status;
}
#else
rf_config_status_t rf_config_load(rf_config_t *config)
{
    return config ? RF_CONFIG_UNSUPPORTED : RF_CONFIG_INVALID;
}
rf_config_status_t rf_config_save(const rf_config_t *config)
{
    return rf_config_valid(config) ? RF_CONFIG_UNSUPPORTED : RF_CONFIG_INVALID;
}
#endif
