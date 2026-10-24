#ifndef WD_UART_BRIDGE_H
#define WD_UART_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UART_BRIDGE_RX_CAPACITY 256u
#define UART_BRIDGE_TX_CAPACITY 256u

/* Foreground-only API: serialize all calls in the cooperative main loop.
 * PA3 TX / PA2 RX, same UART as uart_log. Owns UART_IRQHandler while enabled.
 * RX IRQ remains active during foreground SWD work; TX remains foreground.
 * RX queue accesses use short critical sections (one byte per read lock).
 * init() initializes software only, once at startup; bridge starts disabled.
 * configure() accepts CDC stop=0/2 (1/2 stops), parity=0..4
 * (none/odd/even/mark/space), bits=5..8. CDC stop=1 is unsupported.
 * Baud must fit the 16-bit divisor with <=2% error at GetSysClock().
 * Disabled configure only stages settings; logging retains the UART.
 * Active configure returns false if either software queue, RX FIFO, or TX
 * shift register/FIFO is nonempty. No queue/FIFO is reset or silently drained.
 * Invalid/busy requests leave configuration and queues unchanged.
 * Do not change the system clock while enabled.
 */
void uart_bridge_init(void);
/* Pure support check against current system clock; no UART/state changes. */
bool uart_bridge_validate(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits);
bool uart_bridge_configure(uint32_t baud, uint8_t stop, uint8_t parity, uint8_t bits);
/* enable() suspends logs immediately, returns false until prior log TX drains.
 * Retry it on subsequent loop turns. During this pending state log output stays
 * suspended. disable() also cancels a pending enable and resumes logging.
 * Successful enable discards pre-session hardware RX, counting rx_discarded.
 * disable() returns false while queued/hardware TX remains: continue poll()
 * and retry. On success it captures at most one RX FIFO, resumes 115200 8N1
 * logging, and preserves software RX for read(). No TX is silently discarded.
 */
bool uart_bridge_enable(void);
bool uart_bridge_disable(void);
bool uart_bridge_is_enabled(void);
/* Each UART IRQ handles <=8 RX bytes and rearms if FIFO remains nonempty.
 * poll() handles <=8 RX fallback and <=8 TX bytes without spinning/waiting.
 * Call poll regularly for TX. RX continues through foreground delays only
 * while interrupts remain enabled. There is no hardware flow control:
 * 256 queued bytes fill in ~22ms at 115200 8N1 without foreground reads.
 * Beyond capacity, drop-newest is counted; line-error bytes are retained.
 * Interrupt blackout beyond 8 hardware bytes can still cause overrun; its
 * event counter cannot report the exact number of bytes silicon discarded.
 */
void uart_bridge_poll(void);
/* read() copies <=capacity bytes; use the transport packet size as capacity.
 * write() accepts <=available TX room, returns accepted count. Caller retains
 * and retries the unaccepted suffix. Disabled writes accept zero bytes.
 * NULL buffers accept/copy zero bytes. Zero-length calls are harmless.
 */
size_t uart_bridge_read(uint8_t *data, size_t capacity);
size_t uart_bridge_write(const uint8_t *data, size_t length);

typedef struct {
    bool enabled;
    bool enable_pending;
    uint16_t rx_count, tx_count, rx_room, tx_room;
    uint32_t baud, actual_baud;
    uint8_t stop, parity, bits;
    /* Cumulative modulo-2^32 since init; snapshots do not clear counters.
     * rx_received counts all RBR reads while enabled, including rx_overflow.
     * tx_sent counts FIFO submissions, not completed bytes on the wire.
     * overrun_errors counts hardware status events; bytes lost by the silicon
     * cannot be measured. Other errors are events, not exact bad-byte counts.
     */
    uint32_t rx_received, rx_read, rx_overflow, rx_discarded;
    uint32_t tx_accepted, tx_sent;
    uint32_t overrun_errors, parity_errors, framing_errors, break_errors;
} uart_bridge_snapshot_t;
void uart_bridge_get_snapshot(uart_bridge_snapshot_t *snapshot);

#endif
