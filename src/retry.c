#include "retry.h"
#include <string.h>
static uint32_t bound(uint32_t x) { return x < 20 ? 20 : (x > 400 ? 400 : x); }
void retry_init(struct retry_state *s) { memset(s, 0, sizeof(*s)); s->rto = 100; }
void retry_begin(struct retry_state *s) { s->attempts = 0; }
void retry_sent(struct retry_state *s, uint32_t now) {
    if (!s->attempts) s->sent_at = now;
    if (s->attempts < 255) ++s->attempts;
}
uint32_t retry_delay(const struct retry_state *s) {
    uint32_t delay = s->rto;
    unsigned shift = s->attempts > 1 ? s->attempts - 1 : 0;
    if (shift > 4) shift = 4;
    return bound(delay << shift);
}
void retry_complete(struct retry_state *s, uint32_t now) {
    if (s->attempts != 1) return;
    uint32_t sample = now - s->sent_at;
    if (!sample) sample = 1;
    if (sample > 1500) return;
    if (!s->srtt) { s->srtt = sample; s->variance = (sample + 1) / 2; }
    else {
        uint32_t error = s->srtt > sample ? s->srtt - sample : sample - s->srtt;
        s->variance = (3 * s->variance + error) / 4;
        s->srtt = (7 * s->srtt + sample) / 8;
    }
    s->rto = bound(s->srtt + 4 * s->variance);
}
