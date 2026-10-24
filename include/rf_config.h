#ifndef WD_RF_CONFIG_H
#define WD_RF_CONFIG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef WD_RF_CHANNEL
#define WD_RF_CHANNEL 16
#endif
#ifndef WD_RF_ADDRESS
#define WD_RF_ADDRESS 0x8E89BED6u
#endif
#define RF_CONFIG_ID_SIZE 8u
#define RF_CONFIG_RECORD_SIZE 32u
#define RF_CONFIG_MAX_IDLE_TIMEOUT_MS 60000u
/* Reserved PROGRAM flash, not a separately addressed dataflash region.
 * The build must reduce application FLASH to 0x3a000 before enabling writes. */
#ifndef WD_RF_CONFIG_FLASH_STORAGE
#define WD_RF_CONFIG_FLASH_STORAGE 0
#endif
#define RF_CONFIG_PERSISTENCE_SUPPORTED WD_RF_CONFIG_FLASH_STORAGE
#define RF_CONFIG_FLASH_BASE 0x0003a000u
#define RF_CONFIG_FLASH_BLOCK_SIZE 4096u

typedef enum {
    RF_CONFIG_ROLE_AUTO = 0,
    RF_CONFIG_ROLE_HOST = 1,
    RF_CONFIG_ROLE_SLAVE = 2
} rf_config_role_t;

typedef struct {
    uint32_t access_address;
    uint32_t idle_timeout_ms; /* 0 disables scheduler idle policy; max 60 seconds. */
    uint8_t peer_id[RF_CONFIG_ID_SIZE]; /* All zero means no selected peer. */
    uint8_t channel; /* SDK BLE PHY channel index 0..39. */
    int8_t tx_power_dbm; /* Exact SDK levels only; see rf_config_tx_power_sdk. */
    uint8_t role; /* rf_config_role_t, independent of compiled firmware role. */
} rf_config_t;

void rf_config_defaults(rf_config_t *config);
bool rf_config_valid(const rf_config_t *config);
/* Allowed dBm: -25,-20,-15,-10,-8,-5,-3,-1,0,1,2,3,4,5,6.
 * sdk_value may be NULL when only checking support. */
bool rf_config_tx_power_sdk(int8_t dbm, uint8_t *sdk_value);
/* Fixed little-endian v1 record with CRC32; exact buffer size required.
 * Decode rejects corrupt/partial/unknown records, leaving output unchanged.
 * Serialization alone is not a power-fail-safe flash storage implementation. */
bool rf_config_encode(const rf_config_t *config, uint8_t *record, size_t size);
bool rf_config_decode(rf_config_t *config, const uint8_t *record, size_t size);

typedef enum {
    RF_CONFIG_OK = 0,
    RF_CONFIG_NOT_FOUND,
    RF_CONFIG_INVALID,
    RF_CONFIG_IO_ERROR,
    RF_CONFIG_BUSY,
    RF_CONFIG_UNSUPPORTED
} rf_config_status_t;

/* Portable storage engine. Backends address two blocks by slot 0/1; all
 * accesses are bounded to 48 bytes except erase, which clears one full block.
 * Each operation returns true only on success. Backends must serialize callers. */
typedef struct {
    void *context;
    bool (*read)(void *context, unsigned slot, unsigned offset, void *data, unsigned size);
    bool (*erase)(void *context, unsigned slot);
    bool (*write)(void *context, unsigned slot, unsigned offset, const void *data, unsigned size);
} rf_config_storage_t;
rf_config_status_t rf_config_storage_load(const rf_config_storage_t *storage, rf_config_t *config);
rf_config_status_t rf_config_storage_save(const rf_config_storage_t *storage, const rf_config_t *config);
/* Target wrappers. Explicit save only; init does not load or write flash.
 * Save pauses an idle radio, disables IRQs for the SDK erase/write commands,
 * preserves prior pause state, and verifies before committing the new slot.
 * Last valid slot survives a torn erase/write in the other slot. An actual
 * flash command failure returns IO_ERROR; an already committed update may be
 * recovered by load if the final verification read itself failed. */
rf_config_status_t rf_config_load(rf_config_t *config);
rf_config_status_t rf_config_save(const rf_config_t *config);
#endif
