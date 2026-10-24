#ifndef WD_RETRY_H
#define WD_RETRY_H
#include <stdint.h>
#include <stdbool.h>
/* Units are milliseconds. Karn's rule excludes retransmitted RTT samples. */
struct retry_state { uint32_t srtt, variance, rto, sent_at; uint8_t attempts; };
void retry_init(struct retry_state *s);
void retry_begin(struct retry_state *s);
void retry_sent(struct retry_state *s, uint32_t now);
void retry_complete(struct retry_state *s, uint32_t now);
uint32_t retry_delay(const struct retry_state *s);
#endif
