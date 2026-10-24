# USB device layer findings — Bulk + CDC concurrent request timeout

Audit scope (read-only): `src/usb_dap.c`, `include/usb_dap.h`, `tests/test_usb.c`,
`tests/usb_mock/CH57x_common.h`, plus the register authority
`vendor/ch570/EVT/EXAM/SRC/StdPeriphDriver/inc/CH572SFR.h`. No build, no flash, no
device access. `src/app.c` / `src/link.c` are read only for the consequences of
USB-layer state.

> **Revision note.** Line numbers below are the working tree at 2026-10-24 14:14-14:30
> (`src/usb_dap.c` grew 33975 B → 34557 B *during* this audit). While I was reading,
> a concurrent edit split `cdc_reset()` into `cdc_control_reset()` /
> `cdc_data_reset()` and changed `src/app.c` retry scheduling; both are reflected
> below (C2 is now fixed, see §2). `src/app.c` was rewritten twice while I read, so it
> is cited by function (`src/app.c:host_poll()`, `:fail_request()`, `:cdc_request()`),
> never by line; `src/usb_dap.c` numbers are from the 14:24:40 tree. If either file
> moves, match on the quoted text, not the number.

## 0. Bottom line

* The USB device layer **cannot corrupt or starve the DAP request payload**. Every
  shared buffer has disjoint writers with state-gated admission (Q1 refuted), and
  EP3/EP4 (CDC) have a handshake budget independent of the single EP1/EP2 DAP credit.
* The 1500 ms timeout that latches `fault` happens **only** in `src/app.c:host_poll()`
  — `if(elapsed>=REQUEST_TIMEOUT_MS) {++link_timeouts;fail_request(phase==2);}`
  (`src/app.c:host_poll`) with `fail_request()` (`src/app.c:fail_request`)
  `fault|=uncertain;phase=3;`. Nothing in
  `usb_dap.c` can advance that elapsed time. The USB layer is therefore not the direct
  cause; it can only (a) delete an outstanding credit so the host waits on NAK
  forever, or (b) starve the main loop until an already-armed RF deadline expires.
  (a) is confirmed, (b) is unproven.
* Defects confirmed here: **`SET_INTERFACE(0|1, alt 0)` resets the whole DAP session**
  (C1, open), **no `request_state` has a bounded escape** (C3, open), **a no-op
  `SET_INTERFACE(2)` used to wipe an open CDC session** (C2, fixed in the tree during
  this audit), plus two latent and one diagnostic defect (C4-C6).
* The concurrent `src/app.c` change (`RETRY_ATTEMPTS_IN_BUDGET`, "Measured per-attempt
  frame loss on this link is ~14%") supplies the number my H1 hypothesis needed: the
  fault is a retry-budget failure on the RF link, not a USB delivery failure.

## 1. Per-question verdict

### Q1 — can EP2/EP3/EP4 traffic corrupt or starve the DAP request path? **REFUTED, for memory and for the credit.**

**`ep0_buffer` vs the EP4 notify region: disjoint by construction.**
`src/usb_dap.c:notify_update()` (L153-167) is the only writer above offset 63:

```
156:        uint8_t *p = ep0_buffer + EP0_SIZE;
157:        p[0] = 0xa1; p[1] = 0x20; p[2] = p[3] = 0;      ...  159: p[9] = (uint8_t)(serial_state >> 8);
162:        R8_UEP4_T_LEN = 10;
```

and `src/usb_dap.c:ep0_packet()` (L255-265) can never reach offset 64:

```
257:    uint8_t n = control_remaining > EP0_SIZE ? EP0_SIZE : (uint8_t)control_remaining;
258:    if (n && control_data != ep0_buffer) memcpy(ep0_buffer, control_data, n);
```

Every other `ep0_buffer` write is bounded: string descriptors clamp `if (n > 31) n = 31;`
(L407, max index `3 + 2*30 = 63`); `GET_CONFIGURATION` / `GET_INTERFACE` / `GET_STATUS`
/ `GET_IDLE` write 1-2 bytes (L428, L430, L436-437, L471); the 130-byte configuration
descriptor is streamed 64 B at a time through `ep0_packet()`, i.e. through the same
offset-0 window.

The `+64` hardware mapping is not guesswork — `CH572SFR.h` (L1174-1179):

```
// bUEP4_RX_EN & bUEP4_TX_EN: USB endpoint 4 buffer mode, buffer start address is UEP0_DMA
//   0 0:  single 64 bytes buffer for endpoint 0 receiving & transmittal (OUT & IN endpoint)
//   0 1:  single 64 bytes buffer for endpoint 0 receiving & transmittal (OUT & IN endpoint) + 64 bytes buffer for endpoint 4 transmittal (IN endpoint), total=128bytes
```

With `RB_UEP4_TX_EN` set and `RB_UEP4_RX_EN` clear (`src/usb_dap.c:usb_dap_init()`
L642: `R8_UEP4_1_MOD = RB_UEP1_RX_EN | RB_UEP1_TX_EN | RB_UEP4_TX_EN;`) the EP4 IN
window is exactly `UEP0_DMA+64`, and `ep0_buffer` is exactly 128 B (L93
`static uint8_t ep0_buffer[EP0_SIZE * 2]`). `tests/test_usb.c:374` pins the allocation
(`assert(sizeof(ep0_buffer) + ... + sizeof(ep3_buffer) == 512);`) and
`tests/test_usb.c:312` pins non-aliasing after a full 130-byte EP0 read
(`assert(!memcmp(ep0_buffer + 64, notification, sizeof(notification)));`).

**`ep1_buffer` / `ep2_buffer` / `ep3_buffer`: no cross-endpoint aliasing.** Each is
128 B (`REPORT_SIZE * 2`, L94-96) and the SFR layout for `BUF_MOD=0` is OUT at `+0`,
IN at `+64` (`CH572SFR.h` L1168: `1 1 0: 64 bytes buffer for receiving (OUT endpoint)
+ 64 bytes buffer for transmittal (IN endpoint), total=128bytes`). Bases are distinct
(`R16_UEP1_DMA`/`R16_UEP2_DMA`/`R16_UEP3_DMA`, L646-648). Writers:
`accept_request()` reads `+0` (L507, buffer chosen at L570), `send_response()` writes
`+REPORT_SIZE` (L708 `uint8_t *p = (transport == USB_DAP_TRANSPORT_HID ? ep1_buffer :
ep2_buffer) + REPORT_SIZE;`), `usb_cdc_write()` writes `ep3_buffer + REPORT_SIZE`
(L778), `usb_cdc_read()` reads `ep3_buffer + cdc_rx_offset` with
`offset < length <= 64` (L762-766).

**DMA/T_LEN registers are distinct bytes.** `CH572SFR.h`: `R8_UEP0_T_LEN` 0x40008020,
`R8_UEP1_T_LEN` 0x40008024, `R8_UEP2_T_LEN` 0x40008028, `R8_UEP3_T_LEN` 0x4000802c,
`R8_UEP4_T_LEN` 0x40008030, `*_CTRL` at 0x…22/26/2A/2E/32. The only alias in the block
is `R8_UEP4_1_MOD` = `R32_USB_BUF_MODE` at 0x4000800C, written once in `usb_dap_init()`
(L642) and cleared in `usb_dap_deinit()` (L822). `ep1_update()`'s read-modify-write
(L174-179) preserves only `RB_UEP_R_TOG|RB_UEP_T_TOG`, which is correct because
`RB_UEP_AUTO_TOG` (0x10, `CH572SFR.h` L1233) is left 0 — manual toggling, as the
comment at L168 states.

**`request_buffer` cannot be overwritten while the app owns it.** Two writers only:
`src/usb_dap.c:accept_request()` L507, admitted only by `request_state == REQ_IDLE` at
its single call site L575, and `src/usb_dap.c:ep0_out()` L525, admitted only by
`request_state == REQ_CONTROL_RX` (L523) with `length == control_out_length <= 64`
(from L450/L458). `REQ_PROCESSING`/`REQ_RESPONSE`/`REQ_READY` admit neither, so the copy
at L691 (`memcpy(out, request_buffer, REPORT_SIZE);`) always sees the packet it was
handed. `last_report` is written only by `send_response()` (L712) and read only by
`GET_REPORT` (L448), so the HID snapshot cannot alias an armed reply.

**Starvation: none, and the DAP credit is deliberately exclusive.** One slot exists and
CDC does not consume it:

```
src/usb_dap.c:ep1_update()  (L172-179)  loops ep 1..2, gates OUT on request_state == REQ_IDLE
src/usb_dap.c:cdc_update()  (L182-190)  touches only R8_UEP3_CTRL, then notify_update()
src/usb_dap.c:usb_service() (L584-596)  EP3 has its own branch and its own NAK/ACK budget
```

Bulk therefore blocks HID (and vice versa) by design, while CDC progresses
independently — exercised by `tests/test_usb.c:test_bulk_cdc_interleaved()`, which
drives EP2/EP3/EP4/EP0 concurrently and re-asserts `assert_bulk_processing(...)` after
every CDC event.

### Q2 — can `request_state` stick outside `{IDLE, PROCESSING, RESPONSE}` or a reply be lost? **CONFIRMED: the enum is closed, four states have no bounded escape, and one legal control request destroys an armed reply.**

Closed transition set (every writer; there are no others):

| L | target | trigger |
|---|---|---|
| 193 | `REQ_IDLE` | `dap_reset()` ← bus reset / SET_CONFIGURATION / SET_INTERFACE(0,1) / init / deinit |
| 247 | `REQ_IDLE` | `ep0_stall()`, only from `REQ_CONTROL_RX|STATUS` |
| 366 | `REQ_IDLE` | `ep0_setup()` abort guard, only from `REQ_CONTROL_RX|STATUS` |
| 329 | `REQ_READY` | `ACT_REPORT` from `REQ_CONTROL_STATUS` |
| 456 | `REQ_CONTROL_RX` | SET_REPORT SETUP while `REQ_IDLE` and EP1 OUT not halted |
| 510 | `REQ_READY` | `accept_request()` from `REQ_IDLE` |
| 528 | `REQ_CONTROL_STATUS` | SET_REPORT data stage |
| 580 | `REQ_IDLE` | EP1/EP2 IN ACK of the armed reply |
| 693 | `REQ_PROCESSING` | `usb_dap_take_request_routed()` from `REQ_READY` |
| 715 | `REQ_RESPONSE` | `send_response()` from `REQ_PROCESSING` (+tag) |
| 735 | `REQ_IDLE` | `discard_request()` from `REQ_PROCESSING` (+tag) |

No writer can produce a value outside the enum, so nothing can leave the set. What is
missing is **liveness**: `src/usb_dap.c` has no time source at all (the only delay is
`mDelaymS(20)` in `usb_dap_init()`, L637), therefore

* `REQ_RESPONSE` escapes only via the host's IN ACK (L580), `discard_request()` (L735)
  or `dap_reset()` (L193). **If the host cancels the bulk IN URB after the reply was
  armed** (`R8_UEP2_T_LEN` set, T_RES ACK), every later OUT is NAK'd by L176 forever
  and nothing recovers it. `src/app.c` never discards a phase-3 reply —
  `if(!owner) {if(usb_dap_send_response_tagged(response,response_len,generation)) phase=0;}`
  (`src/app.c:host_poll`, phase-3 branch) has no deadline, unlike the 1500 ms bound on
  phase 1/2.
* `REQ_CONTROL_RX` / `REQ_CONTROL_STATUS` (HID control SET_REPORT) escape only via the
  data/status stage, a **new SETUP** (L366), a stall (L247) or a session reset. A
  control pipe torn down without a following SETUP (driver unload, host controller
  error, port error) parks the state there, and L176 then NAKs *both* DAP OUT
  endpoints — the whole DAP path is dead until SET_CONFIGURATION / SET_INTERFACE /
  bus reset.
* `REQ_READY` survives a rejected SET_REPORT (`ep0_stall()` L532 does not touch it —
  `tests/test_usb.c:156` pins `assert(control_state == CTRL_STALL && request_state ==
  REQ_PROCESSING);`), so a stuck app phase-3 wedges the USB credit too (cross-layer
  deadlock; root cause in app.c).

**A reply is provably lost in one interleaving:** `SET_INTERFACE(0|1)` while
`REQ_RESPONSE` or `REQ_PROCESSING` (see Q5) — `dap_reset()` zeroes
`R8_UEP1_T_LEN = R8_UEP2_T_LEN = 0` (L198), sets `transport = NONE` (L195) and
`request_state = REQ_IDLE` (L193), deleting the armed response; the app's later tagged
send can never match (`tag == generation`, L705).

Latent spec deviation in the same area: `src/usb_dap.c:ep0_in()` L491-493

```
491:    } else {
492:        R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
```

turns a STALLed EP0 back into NAK, so an IN token aimed at a stalled control endpoint
clears the stall without a SETUP. Not reachable from a conforming host (a STALL aborts
the control transfer), so it is latent rather than live.

### Q3 — does `SET_INTERFACE(2)` → `cdc_reset()` explain the stall or the fault? **The defect was real; causation is REFUTED (and the defect has just been fixed).**

Current `src/usb_dap.c:ep0_complete()` L294-311 (post-fix) and the pre-fix behaviour it
replaced (the comment at L202-205 is the tree's own account):

```
202: /* SET_INTERFACE resets only the endpoint state of the selected interface. The
203:  * previous single cdc_reset() let SET_INTERFACE(2) on the ACM control interface
204:  * discard data interface 3's partially received and queued bytes together with
205:  * the line coding and DTR, killing an already open CDC session. */
...
294:    case ACT_INTERFACE:
295:        if (action_value < 2) {
...
299:            dap_reset();
300:        } else {
301:            if (action_value == 2) { ...; cdc_control_reset(); }
305:            else { ...; cdc_data_reset(); }
310:            cdc_update();
```

Before that split, `SET_INTERFACE(2|3)` called the full `cdc_reset()`, whose damage to
an open CDC session was, in order of severity:

1. `cdc_rx_length = cdc_rx_offset = 0` — a received-but-unread EP3 packet discarded
   *after* it was ACKed: 1-64 B lost from the byte stream with no NAK.
2. `cdc_tx_busy = cdc_tx_length = 0; R8_UEP3_T_LEN = 0;` — an armed IN withdrawn; the
   host's pending read just NAKs (silent, no error).
3. `cdc_control = 0` — DTR/RTS drop to 0, read by `src/app.c:cdc_request()`
   `bool opened=(usb_cdc_control_line_state()&1)!=0;`.
4. `line_coding` reset to 115200/8N1 — the host's SET_LINE_CODING forgotten; app.c
   forwards `cdc_line.baud_rate` to the peer as `WD_CMD_UART`, so the peer reconfigures
   its UART behind the user's back.
5. `cdc_break = serial_state = 0; notified_state = 0xffff; notify_busy = 0;` — the
   in-flight notification dropped and an unsolicited all-zero SERIAL_STATE emitted
   (DCD/DSR appear lost).
6. Interface 3 also cleared the EP3 halts; interface 2 only the EP4 halt.

**Why it cannot be the fault cause, before or after the fix:** `cdc_reset()` touches no
DAP state — no `request_state`, no `transport`, no `generation`, no `request_buffer`, no
`ep1_buffer`/`ep2_buffer` — and never calls `dap_reset()`. `fault` is set only by
`fail_request(uncertain)` from the 1500 ms phase-1/2 deadline (`host_poll()` →
`fail_request()`) or by
the CDC count checks in the phase-3 owner==1 branch. `cdc_reset()` can only change the
*inputs* of the next `src/app.c:cdc_request()`, and `cdc_request()` is reachable
only from `host_poll()`'s `if(!phase)`, i.e. never while a request is outstanding. Its real
damage was silent CDC data loss plus desynchronizing app.c's own
`cdc_open`/`previous_uart_enabled` from USB state (app.c is not told, because
`generation` is unchanged). Note `SET_INTERFACE(2|3)` is also a spec no-op here: the
guard accepts alt 0 only (L431 `req == USB_SET_INTERFACE && !value && index < 4`), and
the descriptors declare one alternate setting per interface
(`configuration_descriptor`: `9, 4, 2, 0, 1, ...` and `9, 4, 3, 0, 2, ...`).

### Q4 — can any host control request reach `ACT_REPORT` → `select_transport(HID)` and silently abort an in-flight app request? **REFUTED for in-flight requests; CONFIRMED as evidence destruction between requests.**

`src/usb_dap.c:ep0_complete()` L326-331:

```
326:    case ACT_REPORT:
327:        if (request_state == REQ_CONTROL_STATUS) {
328:            select_transport(USB_DAP_TRANSPORT_HID);
329:            request_state = REQ_READY;
```

Entry is gated at L455: `if (request_state == REQ_IDLE && !halted(1, false)) {`, otherwise
the request falls through to `ep0_stall()` (L532). `REQ_CONTROL_STATUS` is only
reachable from `REQ_CONTROL_RX` (L528), which is only reachable from `REQ_IDLE`. So
while the app holds `REQ_PROCESSING`/`REQ_RESPONSE` (or a `REQ_READY` packet is pending)
the HID control path stalls and `select_transport()` (L495-502) cannot run:

```
497:    if (transport != next) {
498:        transport = next;
499:        ++generation;
```

`tests/test_usb.c:156` is the regression pin:
`setup(0x21, DEF_USB_SET_REPORT, 0x0200, 0, 4); assert(control_state == CTRL_STALL &&
request_state == REQ_PROCESSING);`.

What it *can* do — and what matters for a capture procedure — is bump
`generation` **between** requests. `src/app.c:host_poll()` then runs
`generation=current;session=0;sequence=1;phase=0;fault=false;discover_active=false;stream_reset();`
which discards the CDC stream buffers and **clears a latched `fault`**. Any HID admin
client that pokes interface 0 therefore destroys the evidence of a preceding timeout
(already flagged in `docs/bulk-cdc-investigation.md`). The only control request that can
abort an *in-flight* request is `SET_INTERFACE(0|1)` (Q5), not `ACT_REPORT`.

### Q5 — does anything bump `generation` mid-request, dropping a reply or answering stale? **CONFIRMED (drop); REFUTED for stale replies through the tagged API.**

Two bump sites only: L199 (`dap_reset()`) and L499 (`select_transport()`).
`select_transport()` is reachable only from `REQ_IDLE` (via `accept_request()` L506) or
from `REQ_CONTROL_STATUS` (L328), so it can never fire during
`PROCESSING`/`RESPONSE`. `dap_reset()` **is** reachable at any moment, unguarded:

* `usb_service()` L537-542 `if (flags & RB_UIF_BUS_RST) { … new_session(0); … }`
* `ep0_complete()` L290-291 `case ACT_CONFIGURATION: new_session(action_value);`
* `ep0_complete()` L299 `dap_reset();` for `SET_INTERFACE(0|1, alt 0)` — the only one a
  host can issue on a live, configured device with no error condition.
* `usb_dap_init()` L651 / `usb_dap_deinit()` L825 (`new_session(0)`).

Effect of any of them mid-credit: `request_state = REQ_IDLE` (L193), `transport = NONE`
(L195), `R8_UEP1_T_LEN = R8_UEP2_T_LEN = 0` (L198), `++generation` (L199). Consequences:

* **Reply armed (`REQ_RESPONSE`) → deleted**; the host NAKs forever because no endpoint
  is selected any more (L178 `selected && request_state == REQ_RESPONSE`).
* **App mid-processing (`REQ_PROCESSING`) → tag stale**, so `send_response()` (L704-706
  `request_state == REQ_PROCESSING && (!tagged || tag == generation)`) can never
  succeed; app.c abandons the RF request on the next poll.
* **app.c hides it**: no error reply is produced and `fault` is *cleared*
  (`fault=false`, the `host_poll()` generation-change branch). A mid-request
  from a healthy reset except that `link_timeouts` does not increment while the host
  hangs. Pinned as intended behaviour by `tests/test_usb.c:157-163`
  (`R8_USB_INT_FG = RB_UIF_BUS_RST | RB_UIF_TRANSFER; … assert(request_state ==
  REQ_IDLE && !usb_dap_send_response(out, 4)); assert(generation != tag);`) and
  `tests/test_usb.c:188-189` (`setup(0x01, USB_SET_INTERFACE, 0, 0, 0); in0();
  assert(generation == gen + 1 && request_state == REQ_IDLE);`) — the test asserts the
  defect as intended behaviour.
* **Stale reply: not through the tagged API.** `send_response()`'s `tag == generation`
  filter is sound. The untagged entry `usb_dap_send_response()` (L721-724) has no
  filter, and `include/usb_dap.h` forbids it for asynchronous callers ("Async callers
  MUST use tagged send/discard: a stale reply must never consume another session's
  request"), yet `src/main.c:host_poll()` L125
  (`if (phase == 3 && usb_dap_send_response(reply, reply_length)) phase = 0;`) and
  `src/usb_diag_main.c` L61 (after a separate `usb_dap_generation() != tag` check — a
  TOCTOU window) use it. Latent in the shipped app (`src/app.c` is correctly tagged);
  a contract violation in the legacy images.

### Q6 — what the register model deliberately does not simulate

`tests/usb_mock/CH57x_common.h` maps every SFR to a plain `static uint8_t`
(`#define R8_UEP4_T_LEN mock_ep4len`, `#define R16_UEP0_DMA mock_dma0`, …), and
`tests/test_usb.c` states its own limits (L2-3): "Events are serialized; the mock does
not implement SIE PID checking, DMA arbitration or W1C. Tests supply TOG_OK and clear
flags after each entry." Concretely absent:

1. **W1C semantics.** `R8_USB_INT_FG = 0; /* emulate W1C */` (test L21) after each
   entry — a flag that arrives *during* `usb_service()` and is masked out of the
   `flags & (...)` write at L544/L610, or a missed clear, is invisible.
2. **SIE PID / toggle checking.** `event(token, ep, n, toggle)` (test L10-21) lets the
   test *choose* `RB_UIS_TOG_OK`; the firmware's manual toggle bookkeeping is checked
   only against hard-coded expectations.
3. **DMA arbitration and single-buffer ownership.** Nothing ever writes `ep*_buffer` on
   a transaction; the test memcpys payloads in by hand and explicitly refuses to model
   a rejected packet ("Do not overwrite DMA on these events: a real NAK accepts no
   payload"). So a firmware error that consumes a buffer after re-arming ACK, or that
   relies on `RB_UC_INT_BUSY` (L649) to hold off the next transaction, cannot fail here.
4. **Transaction arrival order / concurrency.** Events are serialized from the main
   thread; `PFIC_DisableIRQ` is a flag (`mock_irq_enabled`) and there is no
   preemption. Re-entrancy, a SETUP landing mid-`ep0_setup()` (L604/L610 clears
   `UIF_TRANSFER` *before* `ep0_setup()` runs, and only the 8 SETUP bytes are protected
   by `setup_copy`, L553/L607), and lost updates are unexercised.
5. **Time.** There is no millisecond clock in the mock at all, and none in
   `usb_dap.c`; the whole stuck-credit class (Q2) is untestable by construction.
6. **The EP4 DMA offset.** `ep0_buffer + 64` content is asserted (test L287/312) but
   nothing verifies the SIE transmits from `UEP0_DMA+64`; that rests on `CH572SFR.h`.
7. **`RB_UEP_AUTO_TOG`** (0x10) is not modelled, so the firmware's implicit clearing of
   it (masks at L174/L184) is unchecked.
8. **BUS_RST / suspend timing, SOF, FIFO_OV.** `R8_USB_MIS_ST` carries only the suspend
   bit (L614); `if (flags & RB_UIF_FIFO_OV) R8_USB_INT_FG = RB_UIF_FIFO_OV;` (L618) has
   no behavioural counterpart.
9. **The host.** Nothing models usbccgp/hidclass/usbser — the actual source of
   SET_INTERFACE / SET_REPORT / CDC class requests.

## 2. Confirmed defects vs unproven hypotheses

**Confirmed (code + test evidence):**

* **C1 (OPEN) — `SET_INTERFACE(i, alt 0)`, i∈{0,1}, tears down the DAP session**
  (`src/usb_dap.c:ep0_complete()` L294-299 → `dap_reset()`) and bumps `generation`,
  deleting an armed or in-progress reply (L193-199) and clearing app.c's latched
  `fault` (`src/app.c:host_poll`, generation-change branch). Spec no-op; the only host
  request that can destroy an
  outstanding credit at any moment. *Trigger unproven*: no control-request evidence in
  the field logs (`docs/bulk-cdc-investigation.md`).
* **C2 (FIXED in the tree during this audit) — `SET_INTERFACE(2|3, alt 0)` ran the full
  `cdc_reset()`**, discarding RX/TX data, DTR/RTS, line coding and the pending
  notification of an open CDC session (Q3 list). The landed fix splits
  `cdc_control_reset()` (L206-211: `notify_busy`, `notified_state`, `R8_UEP4_T_LEN`)
  from `cdc_data_reset()` (L212-216: `cdc_rx_*`/`cdc_tx_*`, `R8_UEP3_T_LEN`) and calls
  only the matching one per interface (L301-310), keeping `cdc_reset()` for
  `new_session()` (L234). Residual note: `SET_INTERFACE(3)` still drops partially
  received/queued bytes on the *data* interface, which is spec-correct for an endpoint
  reset, and the forced SERIAL_STATE re-send (L209) is now benign because
  `serial_state`/`cdc_control` are no longer zeroed on that path.
* **C3 (OPEN) — no bounded recovery for `REQ_RESPONSE`, `REQ_CONTROL_RX`,
  `REQ_CONTROL_STATUS` or `REQ_READY`.** `usb_dap.c` has no clock and app.c's phase-3
  has no deadline (`src/app.c:host_poll`, phase-3 branch). A cancelled host URB NAKs the
  DAP endpoints forever and
  also stops CDC streaming (`cdc_request()` runs only in `host_poll()`'s `if(!phase)`).
* **C4 (latent, OPEN) — `ep0_in()` rewrites a STALLed EP0 to NAK** (L491-493).
* **C5 (latent, OPEN) — untagged `usb_dap_send_response()` in `src/main.c:125` and
  `src/usb_diag_main.c:61`**, against the documented `include/usb_dap.h` contract.
* **C6 (diagnostics, OPEN) — `RB_UIF_FIFO_OV` is cleared without an event log** (L618).
  `UART_LOG_USB_STALL` (L245) is the only negative-path log, and full images force
  `WD_UART_LOG=0` (`CMakeLists.txt:49-52`).

**Unproven hypotheses (need hardware):**

* **H1 — the historical fault is an RF retry-budget failure, not a USB fault.** CDC
  streams `WD_CMD_UART` requests through the same `session`/`sequence`/`phase` machine
  (app.c:cdc_request()), so CDC presence raises channel occupancy and competes with the
  DAP request's retransmissions. `src/retry.c` backs off to `bound(rto << shift)`,
  clamped to 20..400 ms, i.e. only ~8 transmissions fit in 1500 ms; the concurrent
  app.c change states "Measured per-attempt frame loss on this link is ~14%", which
  makes a run of 8 lost attempts reachable. The USB layer neither corrupts nor blocks
  the credit (Q1/Q4), so the timeout is produced above it.
* **H2 — peer-side sequence gate wedges the session.** `src/link.c:link_slave_handle()`:
  `if (!s->next_sequence || p.sequence != s->next_sequence || !dispatch) return false;`
  drops mismatched requests silently. A host `sequence` reset not paired with a new
  HELLO (`session=0`) would produce *permanent* 1500 ms timeouts. In app.c every
  `sequence=1` reset is paired with `session=0`, so this needs either a peer-side wedge
  or the HELLO-nonce repeat already suspected in `docs/bulk-cdc-investigation.md`.
* **H3 — IN completions are trusted without `RB_UIS_TOG_OK`** (L578:
  `token == UIS_TOKEN_IN && (*ep_ctrl(endpoint) & MASK_UEP_T_RES) == UEP_T_RES_ACK`).
  This matches the WCH SDK convention
  (`vendor/ch570/EVT/EXAM/USB/Device/CompoundDev/src/Main.c:144-146` does the same), so
  it is an assumption about the SIE: if a TRANSFER interrupt can fire for an IN the
  host did not ACK, the reply is dropped while `request_state` returns to `REQ_IDLE`.
* **H4 — SIE/ISR concurrency outside the `UIF_TRANSFER` window.** `usb_service()` is
  also entered from the main loop (`usb_dap_poll()` L664-672, and from every API
  wrapper) with IRQ masked, and it read-modify-writes live CTRL registers (L179, L184,
  L164) even when no flag is pending. `RB_UC_INT_BUSY` (L649) covers only the
  pending-flag window.
* **H5 — `ep0_setup()` runs after `UIF_TRANSFER` is cleared** (L604 → L610), so a
  following SETUP can overwrite the descriptor/string bytes `ep0_setup()` is still
  formatting in `ep0_buffer`; only the 8 SETUP bytes are cookie-copied (L553/L607).
* **H6 — USB ISR duty cycle starves the main loop** (RF RX FIFO overflow / missed
  `radio_receive`) when Bulk and CDC are both saturated. `uart_log_event()` is
  documented as "bounded enqueue only, never UART or printf"
  (`include/uart_log.h`), so logging inside the ISR is not a candidate source.

## 3. Minimal fixes

C1 — keep the spec effect of SET_INTERFACE (reset that interface's endpoint state),
drop the session teardown:

```diff
--- a/src/usb_dap.c  (ep0_complete, ACT_INTERFACE)
         if (action_value < 2) {
             halt_in &= (uint8_t)~(1u << (action_value + 1));
             halt_out &= (uint8_t)~(1u << (action_value + 1));
+            /* Only alt 0 exists: the spec effect is "reset this interface's
+             * endpoints", so clear toggles/halts and re-arm. Never drop a credit
+             * the host still expects a reply for, nor bump generation. */
             *ep_ctrl(action_value + 1) = UEP_R_RES_NAK | UEP_T_RES_NAK;
-            dap_reset();
+            ep1_update();
         } else {
```
If a session reset is genuinely wanted on an alt-setting *change*, track
`alt_setting[interface]` and call `dap_reset()` only when it changes — with one
alternate setting in `configuration_descriptor`, that branch is dead.

C3 — bound the credit. app.c already has a clock, so the smallest correct fix lives
there (`usb_dap_discard_request_tagged()` already exists, L740):

```diff
--- a/src/app.c
+++ b/src/app.c
     if(phase==3) {
-        if(!owner) {if(usb_dap_send_response_tagged(response,response_len,generation)) phase=0;}
+        if(!owner) {
+            if(usb_dap_send_response_tagged(response,response_len,generation)) phase=0;
+            else if(now-reply_started>=REQUEST_TIMEOUT_MS) {
+                usb_dap_discard_request_tagged(generation);  /* release the credit */
+                fault=true; phase=0;                         /* reply is unverifiable */
+            }
+        }
```
(`reply_started=now` must be set wherever `phase=3` is entered.) If the fix must live
in the USB layer, add an age counter advanced by `usb_dap_poll()` that calls
`discard_request(false, 0)` after a caller-supplied bound — the layer has no clock
today, so that adds a `platform_millis()` dependency.

C4 — keep the stall:

```diff
     } else {
-        R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
+        if (control_state != CTRL_STALL)
+            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
     }
```

C5 — use the tagged API in the legacy images:

```diff
--- a/src/main.c
+++ b/src/main.c
-    if (phase == 3 && usb_dap_send_response(reply, reply_length)) phase = 0;
+    if (phase == 3 && usb_dap_send_response_tagged(reply, reply_length,
+                                                  request_generation)) phase = 0;
```
and likewise `src/usb_diag_main.c:61`, dropping its separate `usb_dap_generation()`
pre-check (a TOCTOU window).

C6 — log the overflow:

```diff
-    if (flags & RB_UIF_FIFO_OV) R8_USB_INT_FG = RB_UIF_FIFO_OV;
+    if (flags & RB_UIF_FIFO_OV) { uart_log_event(UART_LOG_USB_FIFO_OV, R8_USB_INT_ST, 0); R8_USB_INT_FG = RB_UIF_FIFO_OV; }
```
(new `UART_LOG_USB_FIFO_OV` id in `include/uart_log.h`.)

Hardening (cheap, not a defect): pin the EP4 window at compile time so nobody shrinks
`ep0_buffer` again —
`_Static_assert(sizeof(ep0_buffer) >= EP0_SIZE * 2 && EP0_SIZE == 64, "EP4 TX lives at UEP0_DMA+64");`
Today only `tests/test_usb.c:374` enforces it, and tests are not part of the image.

## 4. Facts that must be measured on hardware

To settle the unproven items, in priority order (all need `WD_UART_LOG=1`; full images
force it off at `CMakeLists.txt:49-52`):

1. **Which control requests the host actually sends** (settles C1's trigger and draws
   the USB-vs-RF line). The firmware already logs the full 8-byte SETUP at
   `src/usb_dap.c:ep0_setup()` L357-358
   (`uart_log_event(UART_LOG_USB_SETUP, type|req<<8|value<<16, index|length<<16)`).
   Grep the PA3 capture for `req=0x0b` (SET_INTERFACE, record `index`),
   `req=0x09 type=0x21 value=0x0200` (HID SET_REPORT), `req=0x09 type=0x00`
   (SET_CONFIGURATION) and `req=0x0b type=0x02` (endpoint-halt SET_INTERFACE).
   Without this line C1 and Q4 cannot be closed.
2. **Whether a reset ever fires with a credit outstanding** (settles C1's severity and
   Q5). One new counter, e.g. `usb_reset_busy` incremented in `dap_reset()` when
   `request_state != REQ_IDLE`, plus `usb_set_interface_count[4]`; expose through
   `WD_CTL_STATS` (`src/app.c:control()`, `case WD_CTL_STATS`).
3. **`request_state` at the instant of the timeout** — the decisive field for Q2/H1.
   A read-only accessor (`uint8_t usb_dap_request_state(void)`) sampled inside
   `fail_request()`, recorded with `owner`, `request[0]`, `phase`, `session`,
   `sequence`, `retry.attempts`, `retry.rto`.
4. **Whether the host ever cancels a bulk IN URB** (settles C3's live reachability).
   Host side: a `USBD_STATUS_CANCELED` / WPP-usb trace. Device side: `request_state`
   stuck in `REQ_RESPONSE` while `R8_UEP2_CTRL` reads `UEP_R_RES_NAK`, sampled with
   counter 3.
5. **SIE-level IN/OUT handshakes on EP2/EP3 during a failure** (settles H3/H4/H5). A
   bus analyzer (or WCH SIE trace) counting EP2 IN tokens answered with DATA vs NAK,
   whether any DATA was ACKed twice or not at all, and whether a SETUP arrives while
   `ep0_buffer` still holds a partially streamed descriptor (H5 — correlate the SETUP
   timestamp with the 130-byte descriptor reads).
6. **USB ISR duty cycle and RF RX continuity** (settles H6). Measure the fraction of CPU
   in `USB_IRQHandler` (GPIO toggle or cycle counter around `usb_service()`) while
   Bulk+CDC saturate, together with `radio_get_stats()` `rx_count`, `drops`,
   `crc_errors`, `tx_failures`, `tx_timeouts` and `link_rejected` at the failure. If
   `drops` stays 0 while `link_timeouts` increments, H6 is dead and the loss is on the
   peer/medium.
7. **Peer-side sequence state at the failure** (settles H2). Not visible today: add
   `s->next_sequence`, `s->connected`, `s->hello_nonce` to the slave's
   `UART_LOG_HEARTBEAT` or a slave stats command, then compare against the host's
   `session`/`sequence` at the timeout.

## 5. Evidence index

* `src/usb_dap.c` — L93-98 buffers; L153-167 `notify_update()`; L169-181 `ep1_update()`;
  L182-190 `cdc_update()`; L191-201 `dap_reset()`; L202-227 the `cdc_*_reset()` split;
  L228-235 `new_session()`; L243-252 `ep0_stall()`; L255-265 `ep0_packet()`; L290-312
  `ACT_CONFIGURATION`/`ACT_INTERFACE`; L326-331 `ACT_REPORT`; L365-368 SETUP abort
  guard; L431 SET_INTERFACE guard; L449-462 SET_REPORT accept; L478-494 `ep0_in()`;
  L495-502 `select_transport()`; L504-511 `accept_request()`; L513-533 `ep0_out()`;
  L534-625 `usb_service()`; L683-735 take/send/discard; L740 tagged discard.
* `include/usb_dap.h` — "single DAP credit shared by HID and Bulk"; "Generation changes
  on init, reset, SET_CONFIGURATION, DAP SET_INTERFACE, and accepted HID/Bulk transport
  switch"; "Async callers MUST use tagged send/discard".
* `tests/test_usb.c` — L2-3 model limits; L21 W1C emulation; L156 SET_REPORT stall;
  L157-163 bus-reset credit loss; L188-189 SET_INTERFACE generation bump; L287/312
  EP0/EP4 non-aliasing; L374 allocation size.
* `vendor/ch570/EVT/EXAM/SRC/StdPeriphDriver/inc/CH572SFR.h` — L1174-1179 EP4 buffer
  mode; L1216-1221 DMA bases; L1226-1288 T_LEN/CTRL addresses; L1233 `RB_UEP_AUTO_TOG`.
* Registers referenced: `R16_UEP0_DMA`, `R8_UEP0..4_T_LEN`, `R8_UEP0..4_CTRL`,
  `R8_UEP4_1_MOD`, `R8_USB_INT_FG`, `R8_USB_INT_ST`, `R8_USB_RX_LEN`, `R8_USB_CTRL`.
