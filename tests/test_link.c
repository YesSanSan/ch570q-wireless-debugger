#include "link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls, resets;
static size_t dispatch(const uint8_t *p, size_t n, uint8_t *out, size_t cap)
{
    assert(n && cap == 64); ++calls; out[0] = p[0]; out[1] = 0; return 2;
}
static void reset(void) { ++resets; }
int main(void)
{
    struct link_slave s;
    struct link_packet p = {0}, decoded;
    uint8_t frame[78], response[78], cached[78], mutated[78];
    link_slave_init(&s);
    p.type = LINK_REQUEST; p.session = 10; p.sequence = 1; p.length = 64;
    for (unsigned i = 0; i < 64; ++i) p.data[i] = (uint8_t)i;
    assert(link_encode(&p, frame) && link_decode(frame, &decoded));
    assert(decoded.length == 64 && !memcmp(p.data, decoded.data, 64));
    assert(!link_slave_handle(&s, frame, response, 10, dispatch, reset));
    p.type = LINK_HELLO; p.session = 0; p.sequence = 123; p.length = 0;
    assert(link_encode(&p, frame));
    assert(link_slave_handle(&s, frame, response, 10, dispatch, reset));
    assert(link_decode(response, &decoded));
    assert(decoded.type == LINK_HELLO_ACK && decoded.session == 10 && resets == 1);
    assert(link_slave_handle(&s, frame, response, 20, dispatch, reset));
    assert(s.session == 10 && resets == 1); /* Lost hello reply retry. */
    p.type = LINK_REQUEST; p.session = 10; p.sequence = 1; p.length = 2;
    p.data[0] = 5; p.data[1] = 0;
    assert(link_encode(&p, frame));
    assert(link_slave_handle(&s, frame, response, 20, dispatch, reset));
    assert(calls == 1);
    memcpy(cached, response, 78);
    for (unsigned i = 0; i < 20; ++i) {
        assert(link_slave_handle(&s, frame, response, 20, dispatch, reset));
        assert(!memcmp(cached, response, 78) && calls == 1);
    }
    memcpy(mutated, frame, 78); mutated[14] ^= 1;
    assert(!link_slave_handle(&s, mutated, response, 20, dispatch, reset));
    p.sequence = 3; assert(link_encode(&p, mutated));
    assert(!link_slave_handle(&s, mutated, response, 20, dispatch, reset));
    p.sequence = 2; assert(link_encode(&p, mutated));
    assert(link_slave_handle(&s, mutated, response, 20, dispatch, reset));
    assert(calls == 2);
    assert(!link_slave_handle(&s, frame, response, 20, dispatch, reset)); /* Too old. */
    p.type = LINK_HELLO; p.session = 0; p.sequence = 124; p.length = 0;
    assert(link_encode(&p, mutated));
    assert(link_slave_handle(&s, mutated, response, 20, dispatch, reset));
    assert(s.session == 20 && resets == 2);
    assert(!link_slave_handle(&s, frame, response, 20, dispatch, reset)); /* Old session. */
    p.type = LINK_REQUEST; p.session = 20; p.sequence = 1; p.length = 1;
    assert(link_encode(&p, frame));
    /* Truncated/oversized/corrupt/padded frames must fail before dispatch. */
    const unsigned offsets[] = {0, 1, 2, 3, 12, 13, 77};
    const uint8_t bad[] = {0, 0, 2, 5, 65, 1, 1};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        memcpy(mutated, frame, 78); mutated[offsets[i]] = bad[i];
        assert(!link_slave_handle(&s, mutated, response, 20, dispatch, reset));
    }
    assert(calls == 2);
    p.length = 65; assert(!link_encode(&p, frame));
    /* Discovery is app-owned and must never disturb an established link. */
    struct link_slave before_discovery = s;
    const uint8_t discovery_types[] = {LINK_DISCOVER, LINK_DISCOVER_REPLY};
    for (unsigned i = 0; i < sizeof(discovery_types); ++i) {
        memset(&p, 0, sizeof(p));
        p.type = discovery_types[i]; p.sequence = 0x12345678;
        p.length = i ? 8 : 0;
        for (unsigned j = 0; j < p.length; ++j) p.data[j] = (uint8_t)(j + 1);
        assert(link_encode(&p, frame) && link_decode(frame, &decoded));
        assert(decoded.type == p.type && decoded.sequence == p.sequence && decoded.length == p.length);
        assert(!link_slave_handle(&s, frame, response, 30, dispatch, reset));
        assert(!memcmp(&s, &before_discovery, sizeof(s)) && calls == 2 && resets == 2);
    }
    /* App filters targeted HELLO IDs. Link preserves target payload codec and
     * treats an identical HELLO retry as the same handshake even after data. */
    memset(&p, 0, sizeof(p));
    p.type = LINK_HELLO; p.sequence = 200; p.length = 8;
    for (unsigned i = 0; i < 8; ++i) p.data[i] = (uint8_t)(i + 1);
    assert(link_encode(&p, cached) && link_decode(cached, &decoded));
    assert(!memcmp(decoded.data, p.data, 8));
    assert(link_slave_handle(&s, cached, response, 30, dispatch, reset));
    assert(s.session == 30 && s.next_sequence == 1 && resets == 3);
    p.type = LINK_REQUEST; p.session = 30; p.sequence = 1; p.length = 1; p.data[0] = 5;
    assert(link_encode(&p, frame));
    assert(link_slave_handle(&s, frame, response, 99, dispatch, reset));
    assert(calls == 3 && s.next_sequence == 2 && s.have_cache);
    memcpy(mutated, response, sizeof(mutated));
    assert(link_slave_handle(&s, cached, response, 99, dispatch, reset));
    assert(s.session == 30 && s.next_sequence == 2 && s.have_cache && resets == 3);
    assert(link_slave_handle(&s, frame, response, 99, dispatch, reset));
    assert(calls == 3 && !memcmp(response, mutated, sizeof(mutated)));
    puts("link tests passed: codec, handshake, lost reply retries, duplicate writes, order, sessions, malformed frames, discovery isolation and targeted HELLO cache retention");
    return 0;
}
