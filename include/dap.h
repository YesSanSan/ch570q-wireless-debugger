#ifndef CH570_DAP_H
#define CH570_DAP_H
#include <stdint.h>
#include <stddef.h>
#define DAP_PACKET_SIZE 64u
/* Serialized slave-side API. req and resp must not overlap. Returns zero for
 * invalid pointers/length/capacity or malformed requests; never partial output.
 * Commands with semantic failures return their standard DAP error response. */
size_t dap_process(const uint8_t *req, size_t len, uint8_t *resp, size_t cap);
/* Slave only: initializes GPIO SWD and disables this chip's own debug port. */
void dap_init(void);
#endif
