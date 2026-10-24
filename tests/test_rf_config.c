#include "rf_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SLOT_BYTES 48u
static uint8_t flash[2][RF_CONFIG_FLASH_BLOCK_SIZE];
static unsigned erases, writes;
static int erase_budget = -1, write_budget = -1, read_budget = -1;

static bool mock_read(void *ctx, unsigned slot, unsigned offset, void *data, unsigned size)
{
    (void)ctx;
    assert(slot < 2 && offset + size <= SLOT_BYTES);
    if (read_budget == 0) return false;
    if (read_budget > 0) --read_budget;
    memcpy(data, flash[slot] + offset, size);
    return true;
}
static bool mock_erase(void *ctx, unsigned slot)
{
    (void)ctx;
    assert(slot < 2);
    ++erases;
    unsigned count = erase_budget < 0 ? RF_CONFIG_FLASH_BLOCK_SIZE : (unsigned)erase_budget;
    if (count > RF_CONFIG_FLASH_BLOCK_SIZE) count = RF_CONFIG_FLASH_BLOCK_SIZE;
    memset(flash[slot], 0xff, count);
    return count == RF_CONFIG_FLASH_BLOCK_SIZE;
}
static bool mock_write(void *ctx, unsigned slot, unsigned offset, const void *data, unsigned size)
{
    const uint8_t *bytes = data;
    (void)ctx;
    assert(slot < 2 && offset + size <= SLOT_BYTES && !(offset & 3) && !(size & 3));
    ++writes;
    unsigned count = write_budget < 0 ? size : (unsigned)write_budget;
    if (count > size) count = size;
    for (unsigned i = 0; i < count; ++i) flash[slot][offset + i] &= bytes[i];
    if (write_budget >= 0) write_budget -= (int)count;
    return count == size;
}
static const rf_config_storage_t storage = {NULL, mock_read, mock_erase, mock_write};
static void reset_flash(void)
{
    memset(flash, 0xff, sizeof(flash));
    erases = writes = 0;
    erase_budget = write_budget = read_budget = -1;
}
static void same_config(const rf_config_t *a, const rf_config_t *b)
{
    uint8_t ea[RF_CONFIG_RECORD_SIZE], eb[RF_CONFIG_RECORD_SIZE];
    assert(rf_config_encode(a, ea, sizeof(ea)));
    assert(rf_config_encode(b, eb, sizeof(eb)));
    assert(!memcmp(ea, eb, sizeof(ea)));
}
static uint32_t test_crc(const uint8_t *p, unsigned size)
{
    uint32_t c = 0xffffffffu;
    for (unsigned i = 0; i < size; ++i) {
        c ^= p[i];
        for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
    }
    return ~c;
}
static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
int main(void)
{
    rf_config_t a, b, out, unchanged;
    uint8_t record[RF_CONFIG_RECORD_SIZE], damaged[RF_CONFIG_RECORD_SIZE];
    rf_config_defaults(&a);
    assert(rf_config_valid(&a) && a.channel == WD_RF_CHANNEL && a.access_address == WD_RF_ADDRESS);
    assert(a.tx_power_dbm == 0 && a.role == RF_CONFIG_ROLE_AUTO && a.idle_timeout_ms == 0);
    const int8_t powers[] = {-25,-20,-15,-10,-8,-5,-3,-1,0,1,2,3,4,5,6};
    const uint8_t sdk[] = {1,2,3,5,7,10,12,16,18,21,24,27,31,37,45};
    for (int p = -128; p <= 127; ++p) {
        bool expected = false;
        uint8_t value = 0xff;
        for (unsigned i = 0; i < sizeof(powers); ++i) if (p == powers[i]) {
            expected = true;
            assert(rf_config_tx_power_sdk((int8_t)p, &value) && value == sdk[i]);
        }
        assert(rf_config_tx_power_sdk((int8_t)p, NULL) == expected);
        a.tx_power_dbm = (int8_t)p;
        assert(rf_config_valid(&a) == expected);
    }
    rf_config_defaults(&a);
    for (unsigned ch = 0; ch < 256; ++ch) {
        a.channel = (uint8_t)ch;
        assert(rf_config_valid(&a) == (ch < 40));
    }
    rf_config_defaults(&a);
    a.access_address = 0; assert(rf_config_valid(&a));
    a.access_address = 0xffffffffu; assert(rf_config_valid(&a));
    a.role = 3; assert(!rf_config_valid(&a)); a.role = RF_CONFIG_ROLE_SLAVE;
    a.idle_timeout_ms = RF_CONFIG_MAX_IDLE_TIMEOUT_MS + 1; assert(!rf_config_valid(&a));
    a.idle_timeout_ms = RF_CONFIG_MAX_IDLE_TIMEOUT_MS;
    for (unsigned i = 0; i < RF_CONFIG_ID_SIZE; ++i) a.peer_id[i] = (uint8_t)(i + 1);
    a.tx_power_dbm = -25; a.channel = 39;
    assert(rf_config_encode(&a, record, sizeof(record)));
    assert(record[6] == 39 && record[7] == 231 && record[8] == 255 && record[20] == 2);
    assert(rf_config_decode(&out, record, sizeof(record))); same_config(&a, &out);
    unchanged = out;
    for (unsigned bit = 0; bit < sizeof(record) * 8; ++bit) {
        memcpy(damaged, record, sizeof(record)); damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        assert(!rf_config_decode(&out, damaged, sizeof(damaged))); same_config(&out, &unchanged);
    }
    for (unsigned n = 0; n < sizeof(record); ++n) {
        assert(!rf_config_decode(&out, record, n)); same_config(&out, &unchanged);
    }
    assert(!rf_config_decode(&out, record, sizeof(record) + 1));
    assert(!rf_config_decode(NULL, record, sizeof(record)) && !rf_config_decode(&out, NULL, sizeof(record)));
    memcpy(damaged, record, sizeof(record)); damaged[6] = 40;
    put32(damaged + 28, test_crc(damaged, 28));
    assert(!rf_config_decode(&out, damaged, sizeof(damaged)));
    assert(!rf_config_valid(NULL)); rf_config_defaults(NULL);
    assert(!rf_config_encode(NULL, record, sizeof(record)));
    assert(!rf_config_encode(&a, NULL, sizeof(record)));

    reset_flash();
    assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_NOT_FOUND);
    assert(rf_config_storage_save(&storage, &a) == RF_CONFIG_OK);
    assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK); same_config(&a, &out);
    unsigned previous_erases = erases;
    assert(rf_config_storage_save(&storage, &a) == RF_CONFIG_OK && erases == previous_erases);
    b = a; b.channel = 0; b.role = RF_CONFIG_ROLE_HOST;
    uint8_t baseline[sizeof(flash)]; memcpy(baseline, flash, sizeof(flash));
    /* Cut power at every byte in payload or final commit. Old slot survives. */
    for (unsigned cut = 0; cut < SLOT_BYTES; ++cut) {
        memcpy(flash, baseline, sizeof(flash)); write_budget = (int)cut;
        assert(rf_config_storage_save(&storage, &b) == RF_CONFIG_IO_ERROR);
        write_budget = -1;
        assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK); same_config(&a, &out);
    }
    /* Read failures before/after commit report IO_ERROR. A completed commit
     * remains recoverable even if the verification read failed. */
    for (int cut = 0; cut < 4; ++cut) {
        memcpy(flash, baseline, sizeof(flash)); read_budget = cut;
        assert(rf_config_storage_save(&storage, &b) == RF_CONFIG_IO_ERROR);
        read_budget = -1;
        assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK);
        same_config(cut == 3 ? &b : &a, &out);
    }
    /* Cycle once so erasing the old inactive slot can also tear. */
    memcpy(flash, baseline, sizeof(flash));
    assert(rf_config_storage_save(&storage, &b) == RF_CONFIG_OK);
    memcpy(baseline, flash, sizeof(flash));
    for (unsigned cut = 0; cut < RF_CONFIG_FLASH_BLOCK_SIZE; ++cut) {
        memcpy(flash, baseline, sizeof(flash)); erase_budget = (int)cut;
        assert(rf_config_storage_save(&storage, &a) == RF_CONFIG_IO_ERROR);
        erase_budget = -1;
        assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK); same_config(&b, &out);
    }
    memcpy(flash, baseline, sizeof(flash));
    /* Corruption of newest slot recovers previous slot. */
    flash[1][10] ^= 1;
    assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK); same_config(&a, &out);
    memcpy(flash, baseline, sizeof(flash));
    put32(flash[0] + 4, 0xffffffffu); put32(flash[0] + 40, test_crc(flash[0], 40));
    put32(flash[1] + 4, 0); put32(flash[1] + 40, test_crc(flash[1], 40));
    assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_OK); same_config(&b, &out);
    read_budget = 0;
    assert(rf_config_storage_save(&storage, &a) == RF_CONFIG_IO_ERROR);
    assert(rf_config_storage_load(&storage, &out) == RF_CONFIG_IO_ERROR);
    read_budget = -1;
    assert(rf_config_storage_load(NULL, &out) == RF_CONFIG_INVALID);
    assert(rf_config_storage_save(&storage, NULL) == RF_CONFIG_INVALID);
#if !WD_RF_CONFIG_FLASH_STORAGE
    assert(rf_config_load(&out) == RF_CONFIG_UNSUPPORTED);
    assert(rf_config_save(&a) == RF_CONFIG_UNSUPPORTED);
#endif
    puts("RF config: exact SDK validation, serialization, corruption, dual-slot torn writes/erases and rollover passed");
    return 0;
}
