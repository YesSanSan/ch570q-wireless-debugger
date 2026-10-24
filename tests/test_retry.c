#include "retry.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct retry_state s; retry_init(&s); assert(retry_delay(&s)==100);
    retry_begin(&s); retry_sent(&s, 0xfffffff8u); retry_complete(&s, 2);
    assert(s.srtt==10 && s.rto==30);
    retry_begin(&s); retry_sent(&s, 10); retry_sent(&s, 40);
    retry_complete(&s, 41); assert(s.srtt==10 && s.rto==30);
    assert(retry_delay(&s)==60);
    for (unsigned i=0;i<1000;i++) retry_sent(&s,i);
    assert(s.attempts==255 && retry_delay(&s)==400);
    for (unsigned i=0;i<100;i++) {retry_begin(&s);retry_sent(&s,0);retry_complete(&s,1);}
    assert(s.rto==20);
    retry_begin(&s);retry_sent(&s,0);retry_complete(&s,1500);
    assert(s.rto==400);
    puts("retry tests passed: wraparound, Karn ambiguity, smoothing, bounded backoff");
    return 0;
}
