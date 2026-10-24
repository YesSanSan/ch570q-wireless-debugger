#ifndef WD_LINK_H
#define WD_LINK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio.h"
#define DAP_PACKET_SIZE 64u
#define LINK_HELLO 1u
#define LINK_HELLO_ACK 2u
#define LINK_REQUEST 3u
#define LINK_RESPONSE 4u
#define LINK_DISCOVER 16u
#define LINK_DISCOVER_REPLY 17u
struct link_packet {
    uint32_t session, sequence;
    uint8_t type, length, data[DAP_PACKET_SIZE];
};
bool link_encode(const struct link_packet *packet, uint8_t frame[RADIO_FRAME_SIZE]);
bool link_decode(const uint8_t frame[RADIO_FRAME_SIZE], struct link_packet *packet);
typedef size_t (*link_dispatch_fn)(const uint8_t *, size_t, uint8_t *, size_t);
typedef void (*link_reset_fn)(void);
struct link_slave {
    uint32_t session, hello_nonce, next_sequence;
    bool connected, have_cache;
    uint8_t last_request[RADIO_FRAME_SIZE], last_response[RADIO_FRAME_SIZE];
};
void link_slave_init(struct link_slave *state);
bool link_slave_handle(struct link_slave *state, const uint8_t request[RADIO_FRAME_SIZE],
    uint8_t response[RADIO_FRAME_SIZE], uint32_t new_session,
    link_dispatch_fn dispatch, link_reset_fn reset);
#endif
