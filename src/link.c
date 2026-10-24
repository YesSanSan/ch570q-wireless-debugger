#include "link.h"
#include <string.h>
static void put32(uint8_t *p, uint32_t x)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(x >> (i * 8));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool packet_type(uint8_t type) {
    return (type >= LINK_HELLO && type <= LINK_RESPONSE) ||
        type == LINK_DISCOVER || type == LINK_DISCOVER_REPLY;
}
bool link_encode(const struct link_packet *p, uint8_t frame[RADIO_FRAME_SIZE])
{
    if (!p || !frame || p->length > DAP_PACKET_SIZE || !packet_type(p->type))
        return false;
    memset(frame, 0, RADIO_FRAME_SIZE);
    frame[0] = 'W'; frame[1] = 'D'; frame[2] = 1; frame[3] = p->type;
    put32(frame + 4, p->session); put32(frame + 8, p->sequence);
    frame[12] = p->length;
    memcpy(frame + 14, p->data, p->length);
    return true;
}
bool link_decode(const uint8_t frame[RADIO_FRAME_SIZE], struct link_packet *p)
{
    if (!frame || !p || frame[0] != 'W' || frame[1] != 'D' || frame[2] != 1 ||
        !packet_type(frame[3]) || frame[12] > DAP_PACKET_SIZE || frame[13])
        return false;
    p->type = frame[3]; p->session = get32(frame + 4); p->sequence = get32(frame + 8);
    p->length = frame[12];
    memset(p->data, 0, sizeof(p->data));
    memcpy(p->data, frame + 14, p->length);
    /* Canonical zero padding makes duplicate comparisons unambiguous. */
    for (unsigned i = 14u + p->length; i < RADIO_FRAME_SIZE; ++i)
        if (frame[i]) return false;
    return true;
}
void link_slave_init(struct link_slave *s) { memset(s, 0, sizeof(*s)); }
bool link_slave_handle(struct link_slave *s, const uint8_t request[RADIO_FRAME_SIZE],
    uint8_t response[RADIO_FRAME_SIZE], uint32_t new_session,
    link_dispatch_fn dispatch, link_reset_fn reset)
{
    struct link_packet p, out = {0};
    if (!s || !response || !link_decode(request, &p) || !p.sequence) return false;
    if (p.type == LINK_HELLO && !p.session && (p.length == 0 || p.length == 8)) {
        if (!s->connected || s->hello_nonce != p.sequence) {
            uint32_t old_session = s->session;
            s->session = new_session ? new_session : 1;
            if (s->session == old_session) {
                ++s->session;
                if (!s->session) s->session = 1;
            }
            s->hello_nonce = p.sequence;
            s->next_sequence = 1;
            s->have_cache = false;
            s->connected = true;
            if (reset) reset();
        }
        out.type = LINK_HELLO_ACK; out.session = s->session; out.sequence = p.sequence;
        return link_encode(&out, response);
    }
    if (p.type != LINK_REQUEST || !p.length || !s->connected || p.session != s->session)
        return false;
    if (s->have_cache && memcmp(request, s->last_request, RADIO_FRAME_SIZE) == 0) {
        memcpy(response, s->last_response, RADIO_FRAME_SIZE);
        return true;
    }
    /* No gaps, changed duplicates, or sequence wrap: never repeat target writes. */
    if (!s->next_sequence || p.sequence != s->next_sequence || !dispatch) return false;
    out.type = LINK_RESPONSE; out.session = s->session; out.sequence = p.sequence;
    size_t n = dispatch(p.data, p.length, out.data, sizeof(out.data));
    if (!n || n > sizeof(out.data)) { out.data[0] = 0xff; n = 1; }
    out.length = (uint8_t)n;
    link_encode(&out, response);
    memcpy(s->last_request, request, RADIO_FRAME_SIZE);
    memcpy(s->last_response, response, RADIO_FRAME_SIZE);
    s->have_cache = true;
    ++s->next_sequence;
    return true;
}
