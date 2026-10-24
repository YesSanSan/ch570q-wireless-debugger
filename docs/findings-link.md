# 主机调度器 + RF 链路审计

范围：`src/app.c`（host 侧）、`src/link.c`、`src/radio.c`、`src/retry.c`、`include/{link,radio,retry}.h`。
方法：只读静态阅读 + 推演；未修改任何源码，未构建/烧录，未打开 USB/COM，未运行 `tests/run.ps1`。
目的：解释"Bulk + CDC 并发时，单次应用层请求偶发超过 `REQUEST_TIMEOUT_MS=1500`，随后 `fault` 锁存直到 USB generation 变化"。

约定：`PROVEN` = 能在代码里从头跟到尾的缺陷/行为；`HYPOTHESIS` = 需要插桩确认。
所有引用格式为 `文件:函数`，并给出决定性原句。

---

## 0. 先固定结论链（读故障现象的前提）

三个事实决定了"一次超时 = 永久失效"：

1. **只有 phase 2 超时会锁存 fault。**
   `src/app.c:host_poll()`（末尾）：
   ```c
   if(phase==1 || phase==2) {
       if(now-started>=REQUEST_TIMEOUT_MS) {++link_timeouts;fail_request(phase==2);}
   ```
   `fail_request(bool uncertain)`：`fault|=uncertain;phase=3;`。phase 1（HELLO）→ `uncertain=false`，**不锁存**。

2. **锁存后拒绝一切**：DAP 侧 `else if(!request_len || fault || discover_active) fail_request(false);`；CDC 侧 `if(fault || ...) return false;`（`src/app.c:cdc_request()`）。`session` 保持非 0，所以 `begin_request()` 的 `if(session) {queue_request(now);return;}` 永远不会再走 HELLO 分支——**协议层没有"重新握手"的出口**。

3. **唯一解锁是 generation 变化**：`if(generation!=current) {generation=current;session=0;sequence=1;phase=0;fault=false;...}`。`usb_dap_generation()` 只在 USB bus reset 与 `select_transport()`（HID↔Bulk 切换）时自增（`src/usb_dap.c:dap_reset()` `++generation;`、`select_transport()` `++generation;`）。CDC 流量不碰 generation。

⇒ 故障现场只要看到 `fault_locked=true`，那条请求必然是 **phase 2 超时**（或其他见 M10 的唯一例外），且此后"所有请求被拒"是设计行为，不是第二个 bug。**真正要找的是"第一条请求为什么 >1500 ms 没有匹配响应"。**

`WD_CTL_STATS` 字段顺序（`src/app.c:control()`），后文按此名字引用：
`0 rx_count, 1 tx_count, 2 crc_errors, 3 drops, 4 tx_failures, 5 tx_timeouts, 6 link_retries, 7 link_timeouts, 8 link_rejected, 9 completed, 10 srtt, 11 rto`。

---

## 1. 机制清单（每种都能让某次请求 >1500 ms 无匹配响应）

### M1 — 重传预算与 RTO 的自锁死：rto 饱和在 400 ms 后，schedule 只剩 4 次发送、最后一次只有 300 ms 窗口  【PROVEN 算术缺陷】

这是我认为最可能的根因，且它是**自增强**的，能解释"偶发两次"。

**(a) 时间表是硬编码的 1500，而重传间隔由 rto 决定。** `src/retry.c`：
```c
static uint32_t bound(uint32_t x) { return x < 20 ? 20 : (x > 400 ? 400 : x); }
void retry_init(struct retry_state *s) { ...; s->rto = 100; }
uint32_t retry_delay(const struct retry_state *s) {
    uint32_t delay = s->rto;
    unsigned shift = s->attempts > 1 ? s->attempts - 1 : 0;
    if (shift > 4) shift = 4;
    return bound(delay << shift);
}
```
`bound()` 把 **移位后的结果** 夹到 400，所以 rto≥100 时指数退避被彻底抹平，重传变成固定节拍。

派生时间表（`started`=首次发送时刻，`attempts` 在 `retry_sent()` 中自增）：

| rto | 实际发送时刻（相对 started, ms） | TX 次数 | `link_retries` | **最后一次发送的应答窗口** |
|---|---|---|---|---|
| 20 | 0,20,60,140,300,620,940,1260 | 8 | +7 | 240 ms |
| 100 | 0,100,300,700,1100 | 5 | +4 | 400 ms |
| 200 | 0,200,600,1000,1400 | 5 | +4 | **100 ms** |
| 400 | 0,400,800,1200 | 4 | +3 | **300 ms** |

注意 rto=200 比 rto=100 **更差**（末次只剩 100 ms）：schedule 是否可用完全取决于它和 1500 的相位关系，这是硬编码 deadline 的直接后果。

**(b) rto 一旦饱和就长期粘住。** `src/retry.c:retry_complete()`：
```c
if (s->attempts != 1) return;
uint32_t sample = now - s->sent_at;
if (sample > 1500) return;
if (!s->srtt) { s->srtt = sample; s->variance = (sample + 1) / 2; }
else { ... s->srtt = (7 * s->srtt + sample) / 8; }
s->rto = bound(s->srtt + 4 * s->variance);
```
`tests/test_retry.c` 已经把这个行为固化成断言：`retry_begin;retry_sent(0);retry_complete(1500); assert(s->rto==400);`
单次 ~134 ms 的**首次即成功**样本就会让 `rto = bound(134 + 4*67) = 400`；而 7/8 的 EWMA 衰减极慢（srtt 从 1499 降到 <100 需要 ~22 个干净样本）。**在这一长串后续请求里，主机始终只有 4 次发送机会、末次 300 ms 窗口。**

**(c) 为什么最后 300 ms 不够：现场数据。** `docs/bulk-cdc-investigation.md` 实测：同一 DAP 请求的最大时延 Bulk 单跑 **64.83 ms**，Bulk+CDC **307.59 ms**（4.7×）。307 ms 已逼近甚至超过 300 ms 的末次窗口。

**事件轨迹**（示例值；`H4=0x4A3B2C1D=get32(hardware_id)`，`K=0x9e3779b9`）：

| # | 位置 | 事件 | 关键字段 |
|---|---|---|---|
| 1 | 稳态 | 大量 Bulk+CDC 混合请求，其中某次首次即成功但耗时 150 ms | `retry.attempts=1`, `sample=150`, `srtt=150`, `variance=75`, **`rto=400`** |
| 2 | `host_poll` | 取到 DAP 请求 `request[0]=0x05`(DAP_Transfer) | `owner=0`, `phase=0`, `generation=7`, `session=0x4A3AAABD`, `sequence=812` |
| 3 | `queue_request()` | `p.sequence=812`，编码入 `outgoing` | `phase=2`, `started=Now`, `last_tx=Now-400`, `retry.attempts=0` |
| 4 | `host_poll`末 | 立即发出 TX#1 | `last_tx=Now`, `attempts=1`, `link_retries+0` |
| 5 | 空中/从机 | TX#1 与从机的 CDC 响应 TX 撞车（半双工，同频道）→ 无应答 | `crc_errors+?` |
| 6 | `Now+400` | `now-last_tx>=bound(400<<0)=400` → TX#2 | `last_tx=Now+400`, `attempts=2`, **`link_retries+1`** |
| 7 | 从机 | TX#2 到达，`p.sequence==next_sequence(812)` → **`dispatch()` 执行目标写入**（SWD 慢时钟，64 B 传输 ≈ 250–300 ms） | slave `next_sequence→813`, `have_cache=true`，`pending_response=true` |
| 8 | `Now+800` / `Now+1200` | 主机仍在等，继续 TX#3 / TX#4（重复帧，从机此时忙于 dispatch，`++radio_drops`） | `attempts=4`, `link_retries+3` |
| 9 | `Now+1500` | `now-started>=1500` → `++link_timeouts; fail_request(true)` | `link_timeouts+1`, `phase=3`, **`fault=true`** |
| 10 | 稍后 | 从机的应答（`dispatch` 完成后的 cached response）到达，但 `phase==3` | `phase==2` 条件不成立 → `++link_rejected` |
| 11 | 之后 | DAP/管理/CDC 请求全部被合成失败或直接拒绝 | `fault` 保持，直到 generation 变化 |

第 9 步的死因是**纯算术**：从机在 TX#2 上花了 250–300 ms（`dispatch` 期间从机不接收，见 M5），主机在第 4 次发送后只给它 300 ms。**只要"首次可用的发送落在 schedule 后半段" + "dispatch 落在时延尾部"，就一定超时，链路本身完全正常。**

`PROVEN` 的部分：表格全部数字、`bound()` 夹断、`retry_complete` 的粘滞、deadline 与 schedule 的相位无关性。
`HYPOTHESIS` 的部分：事故当时 `rto` 确实已饱和到 400（需要现场 `WD_CTL_STATS[11]` 佐证）。

---

### M2 — 从机链路状态被清空，而主机没有任何"对端重置了我"的信号  【PROVEN 死锁；触发源为 HYPOTHESIS】

`src/link.c:link_slave_handle()`：
```c
if (p.type != LINK_REQUEST || !p.length || !s->connected || p.session != s->session)
    return false;
if (s->have_cache && memcmp(request, s->last_request, RADIO_FRAME_SIZE) == 0) { ...replay... }
if (!s->next_sequence || p.sequence != s->next_sequence || !dispatch) return false;
```
被拒时**一个字节都不回**（`src/app.c:slave_poll()` 只在返回 true 时 `pending_response=true`，否则仅 `++link_rejected`）。主机的 `host_receive()` 因此什么也收不到 → 只能等到 1500 ms。

`slave` 状态被清零的**确定**入口：
- 从机复位/上电：`if(role==RF_CONFIG_ROLE_SLAVE) {usb_dap_deinit();dap_init();link_slave_init(&slave);}`（`main()`）。
- 从机执行 APPLY：`if(radio_reconfigure(&settings)) {apply_pending=false;session=0;sequence=1;link_slave_init(&slave);discover_active=false;discover_reply_pending=false;}`（`main()` 主循环）。**可达路径**：`control()` 对 `internal=false`（`WD_CMD_REMOTE`）时也执行 `case WD_CTL_APPLY: ... else {apply_pending=true;apply_after=platform_millis()+500;}` → 一个管理客户端对**从机**发 APPLY 就能让从机清空链路，而主机毫不知情。
- 同频道另一台主机发出 HELLO（新 nonce）→ `s->session = new_session;` 覆盖。

**事件轨迹**：

| # | 位置 | 事件 | 关键字段（主机 / 从机） |
|---|---|---|---|
| 1 | 握手完成 | 主机发出 HELLO，`nonce=N1=Now0^(7*K)^H4` | host `phase=1, session=0, sequence=1, nonce=N1` |
| 2 | 从机 | `(!s->connected \|\| s->hello_nonce != p.sequence)` 成立 → 新会话 | slave `connected=true, session=S0=Now0^H4, hello_nonce=N1, next_sequence=1, have_cache=false`；`reset()`→`dap_init()` |
| 3 | 主机 | 收到 `LINK_HELLO_ACK{session=S0,sequence=N1}` → `session=p.session;queue_request(now);` | host `session=S0, sequence=1, phase=2` |
| 4 | 运行 500 次请求后 | 双方序号同步推进 | host `sequence=501, completed=500`；slave `next_sequence=501` |
| 5 | **从机 APPLY** | `link_slave_init(&slave)` | slave `connected=false, session=0, next_sequence=0, have_cache=false`；**host 不变** `session=S0, sequence=501` |
| 6 | 主机重传 | `LINK_REQUEST{session=S0,sequence=501}` | `p.session != s->session(0)` → `return false`，**静默** |
| 7 | `Now+400/800/1200` | 重传 3 次 | host `link_retries+3`，`rx_count` **完全不增**，`crc_errors` **不增** |
| 8 | `Now+1500` | 超时 | `link_timeouts+1`, `fail_request(true)` → `fault=true, phase=3` |
| 9 | 之后 | 永久拒绝 | 直到 `generation` 变化 |

与 M1 的**判别证据**：M2 的 `rx_count` 在故障窗口内**绝对不增长**（从机不说话），`crc_errors`/`drops` 也不增；M1 的 `crc_errors`/`drops`/`tx_timeouts` 会增长，且 `rx_count` 通常仍在缓慢增长。
唯一无法只靠主机计数器区分的是 M2 与 M4（主机 RX 死）。

---

### M3 — `radio_send()` 被拒绝而没有任何计数，"发不出去"和"发出去丢了"完全不可区分  【PROVEN 代码路径；触发源 HYPOTHESIS】

`src/radio.c:radio_send()`：
```c
if (!initialized || paused || sending) { __risc_v_enable_irq(irq); return false; }
if (RFRole_Stop()) { ++tx_failures; ... return false; }
...
sending = 1;
bStatus_t result = RFIP_StartTx(&tx);
if (result) { ++tx_failures; sending = 0; start_rx(); }
```
第一个 `return false`（`paused`/`sending`）**不增加任何计数器**。`src/app.c:host_poll()` 里：
```c
else if(now-last_tx>=retry_delay(&retry) && radio_send(outgoing)) {
    last_tx=now;if(retry.attempts) ++link_retries;retry_sent(&retry,now);
}
```
⇒ `link_retries`、`last_tx`、`attempts` 全部不动，重传被无限期推迟，而 `started` 照常走，1500 ms 后照样 `fail_request(true)`。

**事件轨迹**：

| # | 位置 | 事件 | 关键字段 |
|---|---|---|---|
| 1 | 稳态 | `phase=2, session=S0, sequence=812, started=Now, attempts=1` | 正常 |
| 2 | BLE IRQ | 某次 TX 的 `RF_STATE_TX_FINISH` 或 `RF_STATE_TIMEOUT` 丢失/被长时间推迟 → `sending` 卡在 1 | `sending=1` |
| 3 | `host_poll` | `radio_send()` 每次 `return false`（`sending` 分支），无计数 | `attempts=1` 冻结，`link_retries` **冻结**，`tx_count` **冻结** |
| 4 | `Now+1500` | 超时 | `link_timeouts+1`，`fault=true`；`tx_failures`/`tx_timeouts` 也可能冻结 |
| 5 | 现象 | 需要 generation 变化才能恢复（`sending` 只由 IRQ 或 `RFIP_StartTx` 失败清除） | — |

**判别证据**：`link_retries` **冻结而 `link_timeouts` 增 1**，是本机制独有指纹（M1/M2/M6 都会让 `link_retries` 增长）。`tx_count` 也冻结。

次级变体（同为 PROVEN 代码，后果 HYPOTHESIS）：`paused` 被 `radio_pause()` 置 1 后不恢复。`radio_pause` 只有 `src/rf_config.c:rf_config_save()` 调用（远端 `WD_CTL_SAVE_CONFIG`），且 `if (!was_paused) radio_resume();` 会恢复；`radio_resume()` 里 `if (start_rx()) paused = 1;` —— **若 SetRx 失败，`paused` 永久为 1，`radio_send()` 从此永远 false，没有任何计数器，也没有任何恢复路径**。

---

### M4 — 主机 RX 静默死亡：`start_rx()` 的返回值全被丢弃  【PROVEN 缺陷；是否发生 = HYPOTHESIS】

`src/radio.c`：
```c
static __HIGH_CODE bStatus_t start_rx(void) { return paused ? 0 : RFIP_SetRx(&rx); }
```
四个调用点全部忽略返回值：`radio_callback()` 末尾 `if (state & (RF_STATE_RX | RF_STATE_RX_CRCERR | RF_STATE_TX_FINISH | RF_STATE_TIMEOUT)) start_rx();`、`radio_send()` 的两个错误分支、`radio_resume()`（只有这里检查）。
⇒ 若 `RFIP_SetRx()` 返回失败，接收引擎停止，**没有计数、没有重试、没有告警**；主机 `radio_receive()` 永远 false，`host_receive()` 变成空函数，`rx_count` 冻结。

事件轨迹同 M2 的第 6–9 步，只是"从机说话但主机听不见"：
`phase=2 → 重传 3–4 次（`link_retries` 增长、`tx_count` 增长）→ rx_count 冻结 → 1500 ms → fail_request(true) → fault`。

`HYPOTHESIS`：`RFIP_SetRx` 为何会持续失败（射频库二进制，无法静态验证）。**判别证据**：与 M2 完全相同；只能靠"generation 变化后链路立刻恢复"来反证 RX 硬件没坏（若 RX 真死，重新枚举后 HELLO 也永远收不到 ACK，会退化成 M7 的 phase-1 超时而非 fault）。

---

### M5 — 从机 `dispatch()` 期间完全失聪 + 单槽 mailbox，长 SWD/UART 操作把应答推迟到预算之外  【PROVEN 结构；量级 HYPOTHESIS】

`src/app.c:slave_poll()`：
```c
if(pending_response) { if(radio_send(outgoing)) pending_response=false; return; }
...
if(radio_receive(incoming)) {
    ... link_slave_handle(&slave,incoming,outgoing,now^get32(hardware_id),dispatch,reset_dispatch) ...
```
`link_slave_handle()` 内部同步调用 `dispatch()`（即 `src/app.c:dispatch()` → `dap_process()` 位操作 SWD，或 `uart_bridge_*`）。这段时间里从机不读 mailbox；`src/radio.c:radio_callback()` 的 `if (!ready && rx_dma[1] == RADIO_FRAME_SIZE) {...} else { ++radio_drops; }` 只有 **1 帧**缓冲。重传帧全部进 `drops`。

这不是超时的直接原因（从机最终会应答），但它是 M1(c) 的物理放大器：**主机 4 次发送里前 3 次可能全落在从机的 dispatch 窗口内，第 4 次才被执行**，于是"deadline 从第一次发送算起、执行从最后一次发送算起"。
`HYPOTHESIS`：单次 `dap_process()` 在 100 kHz SWD 下可达 250–300 ms（现场最大时延 307 ms 与此一致）。

---

### M6 — 帧被"撕裂"：BLE IRQ 被 UART/USB ISR 推迟，`rx_dma` 已被下一帧覆盖  【HYPOTHESIS】

`src/radio.c:radio_callback()` 在 IRQ 里把 `rx_dma[i+2]` 拷到 `mailbox`。若 BLE IRQ 被更早到达的 UART ISR（`src/uart_bridge.c:UART_IRQHandler()`，每次最多 8 字节，且 `if (R8_UART_RFC) PFIC_SetPendingIRQ(UART_IRQn);` 自我重挂）或 `usb_service()` 推迟到下一帧 DMA 开始写入 `rx_dma`（2M PHY 下一帧约 312 µs），拷贝得到的是两帧混合数据。
后果等价于丢帧，但**表现为 `link_rejected` 增长而 `crc_errors` 不增长**（硬件 CRC 已通过）。
`PROVEN`：这条路径在代码上成立（只有 1 个 `rx_dma`，没有序号/双缓冲保护）；`HYPOTHESIS`：ISR 延迟量级是否达到数百 µs。
**CDC 相关性**：UART ISR 只在 CDC 打开 UART 桥后才使能（`dispatch()` 的 `WD_CMD_UART` 是唯一的 `uart_bridge_enable/configure/disable` 调用者，而 `cdc_request()` 是唯一的 `WD_CMD_UART` 生产者）。所以这是本清单里少数**真正与 CDC 因果相关**的机制——但它只造成丢帧，要变成 1500 ms 超时仍需叠加 M1。

---

### M7 — HELLO 无人应答（`peer_id` 不匹配 / 从机非 SLAVE）→ 长期 1500 ms，但**不锁存 fault**  【PROVEN；可排除本次故障】

`src/link.c:link_slave_handle()` 要求 `p.type == LINK_HELLO && !p.session && (p.length == 0 || p.length == 8)`；`src/app.c:slave_poll()` 在此之前还有一层过滤：
```c
if(p.type==LINK_HELLO && p.length==8 && (!identity_valid || memcmp(p.data,hardware_id,8))) return;
```
主机侧 `src/app.c:begin_request()`：`if(nonzero(settings.peer_id,8)) {p.length=8;memcpy(p.data,settings.peer_id,8);}`。
若 `settings.peer_id` 与从机 8 字节 ID 不一致（例如 `WD_CTL_PAIR` 配错、或对端换了硬件），从机永远静默。

轨迹：`session=0 → phase=1 → 1500 ms → ++link_timeouts → fail_request(false) → phase=3 → 交付 0xFF 响应，fault 不变 → 下一次请求再 HELLO`。
**该路径只累加 `link_timeouts` 而 `fault` 恒为 false**，与本次现象（fault 被锁存）矛盾 ⇒ 可排除。但它污染了 `link_timeouts` 这个计数器的语义（见 §5 的 T4）。

---

### M8 — 非 nonce 重复不可达的序号错位（对 `docs/bulk-cdc-investigation.md` 末条假设的证伪）  【PROVEN 不可达（除 49.7 天 millis 回绕）】

若从机保留 `hello_nonce=N` 且 `next_sequence=K>1`，而主机用**同一 nonce** 重新 HELLO，则从机不重置 `next_sequence`：
```c
if (!s->connected || s->hello_nonce != p.sequence) { ... s->next_sequence = 1; ... }
```
主机随后从 `sequence=1` 发起请求 → `p.sequence != s->next_sequence` → 永久拒绝 → 1500 ms → fault。

**关键证伪**：nonce = `now ^ (generation*0x9e3779b9u) ^ get32(hardware_id)`（`src/app.c:begin_request()`）。
- `generation` 每次变化都改变 nonce：`0x9e3779b9` 是奇数，`g ↦ g*K mod 2^32` 是双射，`g1≠g2 ⇒ 乘积不同`。
- 所有把 `session` 置 0 的入口都不改 `generation` 的只有 `WD_CTL_DISCOVER`(`session=0;sequence=1`)、`WD_CTL_PAIR`、主循环 APPLY，而这些与上一次 HELLO 之间必然隔着 ≥1 个完整请求周期（>1 ms），`now` 不同 ⇒ nonce 不同。
- 同一个 `host_poll()` 调用内 `begin_request()` 只会执行一次；唯一可能出现"同一毫秒两次 HELLO"的是"phase-1 超时 → 下一轮 `phase=3` 交付 → `cdc_request()` 在同一毫秒内 `begin_request()`"，但那两次 HELLO 相距 1500 ms，**且此时从机 `next_sequence` 仍是 1**（第一份 HELLO 的 ACK 都没回来，从未发出过 REQUEST），所以无害。
⇒ 自然重复 nonce 需要 32 位碰撞，或 `platform_millis()` 回绕（`uint32_t` 毫秒 ≈ **49.71 天**）且 `generation` 期间未变。**rank 最低**。

---

### M9 — 相位 3 卡死（响应交付不出去）：DAP 永久静默，但**不锁存 fault**  【PROVEN；可排除本次故障】

`src/app.c:host_poll()`：`if(!owner) {if(usb_dap_send_response_tagged(response,response_len,generation)) phase=0;}`。`src/usb_dap.c:send_response()` 要求 `request_state == REQ_PROCESSING && tag == generation && !halted(transport,true)`。若 `halted(2,true)` 被置位且不清除，`phase` 永远停在 3 → 不再取新请求、也不再生成 CDC 请求（`cdc_request` 在 `if(!phase)` 块内）。现象是"DAP 和 CDC 都停住"，但 `fault=false`、`link_timeouts` 不增长。
⇒ 与"fault 锁存"矛盾，可排除；但它是另一类"看起来像死机"的故障，留给 USB 侧审计。

### M10 — 唯一的第二个 fault 来源：CDC 响应校验失败  【PROVEN 分支；实际几乎不可达】

```c
} else {
    unsigned count=response[3];
    if(count>54 || response_len<4+count || response[2]>cdc_tx_len || count>sizeof(cdc_rx)-cdc_rx_count) {
        fault=true;usb_cdc_set_serial_state(0x40);phase=0;
    } else { ... }
```
这条路径**不经过任何超时**就直接置 `fault`。逐项检查可达性：`count>54`（从机 `uart_bridge_read(out+4,54)` 封顶）、`response_len<4+count`（从机 `return 4+out[3]`）、`response[2]>cdc_tx_len`（`cdc_tx_len` 只在 `stream_reset()` 和本分支被改，而 `stream_reset()` 伴随 `phase=0`）、`count>free`（主机在 `free<54` 时 `request[8]|=2`，从机 `if(!(p[8]&2))` 才读）。
⇒ 实际不可达，但**它提供了一个干净的判别式：若故障后 `link_timeouts==0` 而 `fault==1`，事故就不是超时，而是这条分支**（需要能通过 CRC 的畸形/外来帧）。

---

## 2. 排序（可能性 + 判别证据）

| 排名 | 机制 | 可能性 | `WD_CTL_STATS` 指纹（故障后立刻、仍在原 Bulk 通道读） |
|---|---|---|---|
| **1** | **M1 rto 饱和 + 固定 1500 deadline** | **高**：`PROVEN` 算术；自增强；能解释"偶发两次"和"Bulk 单跑不出" | `rto==400` 且 `srtt` 在 100+ 区间；`link_retries` 每次请求固定 +3（400 ms 节拍）；`link_timeouts+1`；`completed` 在故障前**仍在增长**；`crc_errors`/`drops` 有增长；`rx_count` 缓慢增长 |
| **2** | M2 从机链路状态被清空（APPLY/复位/外来 HELLO） | 中：死锁 `PROVEN`，触发未证 | `rx_count` 在窗口内**完全冻结**；`crc_errors`/`drops` **不增**；`link_retries+3~4`；`tx_count` 正常增长；`srtt/rto` 保持故障前的正常值（小） |
| **3** | M3 `radio_send` 被拒（`sending` 卡死 / `paused` 永久） | 中低 | **`link_retries` 冻结**而 `link_timeouts+1`；`tx_count` **冻结**；`tx_failures`/`tx_timeouts` 可能冻结 |
| **4** | M4 主机 RX 死（`start_rx` 失败无计数） | 低 | 与 M2 一样 `rx_count` 冻结，但**重新枚举后 HELLO 也收不到 ACK**（退化为 `link_timeouts` 单调增长、`fault` 恒 false） |
| **5** | M5 从机长 `dispatch` + 单槽 mailbox | 中（作为放大器） | `drops` 明显增长；`completed` 仍增长；`crc_errors` 不增；单次请求时延出现 250–300 ms 的台阶（见 `docs/bulk-cdc-investigation.md` 的 307.59 ms） |
| **6** | M6 帧撕裂（ISR 延迟） | 低中 | `link_rejected` 增长而 `crc_errors` **不增**；`rx_count` 增长 |
| — | M7 HELLO 无应答 | 可排除（不锁存 fault） | `link_timeouts` 单调增长、`fault==0`、`rx_count` 冻结 |
| — | M8 nonce 重复 | 极低（需 32 位碰撞或 49.7 天回绕） | 同 M2 的指纹；只有从机侧 `link_rejected` 能区分 |
| — | M9 phase 3 卡死 | 可排除（不锁存 fault） | `fault==0`、`link_timeouts` 冻结、DAP+CDC 同时停 |
| — | M10 CDC 响应校验 | 几乎不可达 | `fault==1` 而 `link_timeouts` **不变** |

**一句话判据**：故障后保持 Bulk 路由、先读 `WD_CTL_STATS`：
- `rto==400 && srtt>100` → M1（改重传预算即可解决）；
- `rx_count` 冻结 + `srtt/rto` 正常 → M2/M4（改链路握手恢复）；
- `link_retries` 冻结 → M3（改 `radio_send` 可观测性 + TX 看门狗）；
- `fault==1 && link_timeouts==0` → M10（不是超时）。

---

## 3. CDC 并发：因果还是巧合（逐机制）

| 机制 | 判定 | 理由 |
|---|---|---|
| M1 | **因果（作为负载放大），但不是协议参与方** | CDC 不让 `rto` 增大的机制存在；它通过提高每单位时间的 RF 占用与共享总线的碰撞率，抬高"首次即成功样本"的时延，从而把 `rto` 顶到 400 的沙箱上限。现场 64.83 → 307.59 ms 的 4.7× 时延尾部增长是直接证据。一旦 `rto=400`，即使 CDC 停掉，接下来 ~20 个请求仍然背着 4 次发送/300 ms 末窗的脆弱时间表 ⇒ **"先由 CDC 抬高 rto，再由 Bulk 请求撞上 deadline"的跨界组合**，这正好解释"Bulk 单跑不出、并发才出"。 |
| M2 | **巧合** | `link_slave_handle()` 的会话/序号状态与 `request[0]` 是 DAP 还是 `WD_CMD_UART` 完全无关；CDC 没有任何代码路径能清空 `slave.session` / `next_sequence`（唯一入口是 `link_slave_init()`，只被 `main()` 启动与 APPLY 调用）。 |
| M3 | **巧合** | `sending`/`paused` 与 CDC 无关。CDC 只增加 `radio_send` 的调用频率，从而等概率地更早撞上已死的 TX 状态。 |
| M4 | **巧合** | `start_rx()` 与 CDC 无关。 |
| M5 | **因果/协同** | 从机的 `dispatch()` 串行执行 DAP 与 UART 两类请求，且 CDC 请求永远带着 `uart_bridge_configure/enable/write/read`（唯一调用者），把从机前台占得更满，从而加大"主机第 3/4 次发送才被执行"的概率。注意这是**排队放大**，不是 CDC 直接产生超时。 |
| M6 | **因果** | UART ISR 只在 CDC 打开 UART 桥后使能，它推迟 BLE ISR → 帧撕裂/丢帧率上升。仍只是丢帧，必须叠加 M1 才会变成 1500 ms。 |
| M7/M8/M9/M10 | **巧合**（M10 反而是 CDC 专属，但不可达） | 与 CDC 并发无关（M10 必需 CDC 才可达，但需要 CRC 通过的畸形帧）。 |

**结论**：没有任何代码路径让 CDC 流量直接破坏会话/序号/RF 状态机。CDC 的作用是**把 `rto` 顶到上限并抬高时延尾部**，使一个本来就过紧的固定 1500 ms 预算进入失效区。所以"CDC 并发"是**必要条件式的放大器，不是独立根因**。

---

## 4. 最小修复（针对已确认缺陷，diff 风格伪补丁）

### F1 — 让 deadline 与重传时间表一致，保证最后一次发送有一个完整 RTT  【对应 M1，最高优先】

`src/retry.h` 增加一个只读访问器（避免 app.c 直接摸 `struct retry_state` 内部）：
```c
+ /* 一个完整往返的保守估计：已测样本，未测过时退回 rto。 */
+ static inline uint32_t retry_guard(const struct retry_state *s) {
+     return s->srtt ? s->srtt : s->rto;
+ }
```
`src/app.c:host_poll()`：
```c
-        if(now-started>=REQUEST_TIMEOUT_MS) {++link_timeouts;fail_request(phase==2);}
-        else if(now-last_tx>=retry_delay(&retry) && radio_send(outgoing)) {
+        uint32_t guard=retry_guard(&retry);
+        uint32_t budget=REQUEST_TIMEOUT_MS+guard;   /* 末次发送必须留够一个 RTT */
+        if(now-started>=budget) {++link_timeouts;fail_request(phase==2);}
+        else if(now-last_tx>=retry_delay(&retry) && now+guard<=started+budget &&
+                radio_send(outgoing)) {
             last_tx=now;if(retry.attempts) ++link_retries;retry_sent(&retry,now);
         }
```
效果：`rto=400, srtt=300` 时预算 1800 ms，且 `now+300<=1800` 允许发送到 1500 ms —— 末次发送始终有 300 ms。顺带修掉 rto=200 时末次只剩 100 ms 的非单调性。语义不变（仍然禁止在不确定时重发）。

### F2 — 阻止单次慢样本把 rto 顶到上限并长期粘住  【对应 M1(b)】

`src/retry.c:retry_complete()`：
```c
-    uint32_t sample = now - s->sent_at;
-    if (!sample) sample = 1;
-    if (sample > 1500) return;
+    uint32_t sample = now - s->sent_at;
+    if (!sample) sample = 1;
+    if (sample > 1500) return;
+    /* 单次极端样本不该把 RTO 直接钉在上限；按 RFC6298 的 4*rttvar 同时夹样本与结果。 */
+    if (sample > 400) sample = 400;
```
（保留 `bound()` 的 20..400 沙箱，但样本先夹到 400，使 `rto` 不再由一次 1500 ms 的样本决定；若要更彻底，可把 `bound()` 的上限提到 `4*REQUEST_TIMEOUT_MS` 之外，但这会改变 `tests/test_retry.c` 的既有断言，需要同步更新。）

### F3 — 从机在被会话拒绝时回一个显式 RESET，主机据此"无副作用地"重新握手  【对应 M2，最高价值】

关键前提（已核实）：`link_slave_handle()` 的所有拒绝分支都发生在 `dispatch()` **之前**，因此"被拒绝"是**未在目标上执行**的证明 —— 主机可以安全地不锁存 fault。

`include/link.h`：
```c
+#define LINK_RESET 5u   /* slave proves the request was never dispatched */
```
`src/link.c:packet_type()`：
```c
-    return (type >= LINK_HELLO && type <= LINK_RESPONSE) ||
+    return (type >= LINK_HELLO && type <= LINK_RESET) ||
         type == LINK_DISCOVER || type == LINK_DISCOVER_REPLY;
```
`src/link.c:link_slave_handle()`：
```c
-    if (p.type != LINK_REQUEST || !p.length || !s->connected || p.session != s->session)
-        return false;
+    if (p.type != LINK_REQUEST || !p.length) return false;
+    if (!s->connected || p.session != s->session) {
+        /* 未 dispatch，因此对目标绝无副作用；告诉主机该从零握手，别静默。 */
+        out.type = LINK_RESET; out.session = 0; out.sequence = p.sequence;
+        return link_encode(&out, response);
+    }
```
`src/app.c:host_receive()`（放在 phase-2 匹配分支之前）：
```c
+        } else if((phase==1 || phase==2) && p.type==LINK_RESET &&
+                  p.sequence==(phase==1?nonce:sequence)) {
+            ++link_resets;                       /* 新增计数器，进 WD_CTL_STATS */
+            session=0;sequence=1;retry_begin(&retry);
+            fail_request(false);                 /* phase=3；fault 不变 */
         } else if(phase==2 && p.type==LINK_RESPONSE && ...) {
```
注意：`fail_request(false)` 仍然给 DAP 一个 0xFF 失败响应（OpenOCD 会看到命令失败），但**不再锁存 fault**，下一条请求会重新 HELLO。`src/app.c:slave_poll()` 需要区分"链路层 RESET"与"业务应答"，建议把 `link_slave_handle()` 的返回改为枚举：
```c
+typedef enum { LINK_SLAVE_IGNORED, LINK_SLAVE_ACKED, LINK_SLAVE_DISPATCHED, LINK_SLAVE_RESET } link_slave_result_t;
```
`slave_poll()` 只在 `LINK_SLAVE_DISPATCHED` 时 `++completed`，`LINK_SLAVE_RESET` 时只置 `pending_response=true`。

### F4 — 让 `radio_send()` 的拒绝可观测，并给 `sending` 加看门狗  【对应 M3】

`src/radio.c`：
```c
+static volatile uint32_t tx_blocked;   /* 因 paused/sending 被拒的次数 */
+static uint32_t tx_started_ms;
...
 bool radio_send(const uint8_t frame[RADIO_FRAME_SIZE]) {
     if (!frame) return false;
     uint32_t irq = __risc_v_disable_irq();
-    if (!initialized || paused || sending) { __risc_v_enable_irq(irq); return false; }
+    if (sending && (uint32_t)(platform_millis() - tx_started_ms) > RADIO_TX_STUCK_MS) {
+        /* TX 完成中断丢失：回收状态机并计数，避免永久无法发送。 */
+        ++tx_timeouts; sending = 0; start_rx();
+    }
+    if (!initialized || paused || sending) { ++tx_blocked; __risc_v_enable_irq(irq); return false; }
@@
     sending = 1;
+    tx_started_ms = platform_millis();
     bStatus_t result = RFIP_StartTx(&tx);
```
并把 `tx_blocked` 加进 `radio_stats_t` 与 `WD_CTL_STATS`。`RADIO_TX_STUCK_MS` 取 20（一帧 2M 约 312 µs，留 60× 余量）。

### F5 — `start_rx()` 失败必须计数并可在前台重试  【对应 M4】

```c
-static __HIGH_CODE bStatus_t start_rx(void) { return paused ? 0 : RFIP_SetRx(&rx); }
+static volatile uint32_t rx_arm_failures;
+static __HIGH_CODE bStatus_t start_rx(void) {
+    if (paused) return 0;
+    bStatus_t r = RFIP_SetRx(&rx);
+    if (r) ++rx_arm_failures;      /* 中断里只计数，恢复交给前台 */
+    return r;
+}
+bool radio_rearm_rx(void) {        /* 由 host_poll()/slave_poll() 每轮调用 */
+    uint32_t irq = __risc_v_disable_irq();
+    bool ok = initialized && !paused && !sending;
+    if (ok) ok = (RFIP_SetRx(&rx) == 0);
+    __risc_v_enable_irq(irq);
+    return ok;
+}
```

### F6 — 结束 `last_tx` 的双重身份（潜伏缺陷，当前不可达）

`host_discover()` 里 `if(radio_send(frame)) last_tx=now;` 与请求重传共用 `last_tx`。当前 `discover_active` 只在 `WD_CTL_DISCOVER` 中置位，而该命令要求 `phase==0`，所以暂时无法与飞行中的请求叠加；但任何一次重构都会把它变成"发现帧把请求重传推迟一个 rto"的 bug。
```c
-static uint32_t last_activity, last_uart, discovery_start, discovery_nonce;
+static uint32_t last_activity, last_uart, discovery_start, discovery_nonce, discovery_tx;
...
-    if(now-last_tx>=100) { ... if(radio_send(frame)) last_tx=now; }
+    if(now-discovery_tx>=100) { ... if(radio_send(frame)) discovery_tx=now; }
```

### F7 — 把"首个超时的现场"做成可回溯计数器（压测取证必需）

`docs/bulk-cdc-investigation.md` 已经点明需要 `owner/command/phase/session/sequence/attempts/EP2 request state`。最小实现：在 `fail_request()` 里快照到一个结构体，并追加到 `WD_CTL_STATS`：
```c
+struct link_fault_record { uint32_t owner_cmd; /* owner<<8|request[0] */ uint32_t sequence;
+                           uint32_t attempts, rx_count, crc_errors, srtt, rto; };
+static struct link_fault_record last_timeout;
...
 static void fail_request(bool uncertain) {
+    if (uncertain) { last_timeout.owner_cmd=((uint32_t)owner<<8)|request[0];
+                     last_timeout.sequence=sequence; last_timeout.attempts=retry.attempts;
+                     radio_stats_t st; radio_get_stats(&st);
+                     last_timeout.rx_count=st.rx_count; last_timeout.crc_errors=st.crc_errors;
+                     last_timeout.srtt=retry.srtt; last_timeout.rto=retry.rto; }
```
这是唯一能在**不重新插拔**的前提下区分 M1/M2/M3 的改动。

---

## 5. 可证伪的原生测试（针对 `tests/test_app.c`）

`tests/test_app.c` 用 `#include "../src/app.c"` 编译真实 `app.c` + 真实 `src/link.c` + `src/retry.c`，其余由 `tests/app_mock/app_mock.h` 提供。可用抓手：`mock_now`、`step(now)`（= `mock_now=now;host_poll(now)`）、`queue_packet(&p)`（编码后推入 `mock_rf_rx`）、`last_packet()`（解出最近一次 `mock_rf_tx`）、`answer(accepted,bytes,count)`、`mock_generation`、`mock_rf_send_ok`、`mock_dtr`、`mock_line`、`mock_usb_rx/mock_usb_rx_len`、`mock_usb_tx_busy`、`mock_uart_*`、`dap_request(cmd)`。**从机侧**用独立实例 `struct link_slave peer;` + `link_slave_handle(&peer,host_frame,slave_frame,new_session,dispatch,reset_dispatch)`（`test_cached_uart_reply_and_busy_configuration()` 已是范本），不要用 `slave_poll()`（会与主机的单份 `outgoing`/`incoming` 冲突）。测试可直接读写 `app.c` 的静态变量（`phase/session/sequence/nonce/retry/link_*`），既有测试已如此。

### T1 `test_link_reset_recovers_without_fault`（钉 M2 + F3）
```c
reset_app();                                   /* session=0x1234, sequence=1, generation=1 */
dap_request(0x05); step(10);                   /* -> phase==2, sequence==1 */
struct link_packet f=last_packet(); uint8_t frame[78]; assert(link_encode(&f,frame));
struct link_slave peer; link_slave_init(&peer);            /* 模拟从机刚复位 */
uint8_t reply[78];
assert(link_slave_handle(&peer,frame,reply,0x99,dispatch,reset_dispatch));   /* 现状:false */
struct link_packet r; assert(link_decode(reply,&r));  assert(r.type==LINK_RESET);
queue_packet(&r); step(11);
assert(!fault && session==0 && sequence==1 && phase==0 && link_resets==1);
```
现状**必失败**（`link_slave_handle` 返回 false、`reply` 未定义），修复后必通过。这是把"静默拒绝"变成可观测行为的判据。

### T2 `test_budget_reserves_one_rtt`（钉 M1 + F1，纯算术，最有力）
```c
reset_app(); dap_request(0x05); step(10);
retry.attempts=0; retry.srtt=300; retry.variance=25; retry.rto=400;   /* 现场饱和态 */
unsigned base=mock_rf_tx_count;
step(410);  assert(mock_rf_tx_count==base+1);          /* 0     */
step(810);  assert(mock_rf_tx_count==base+2);          /* +400  */
step(1210); assert(mock_rf_tx_count==base+3);          /* +800  */
step(1510); assert(mock_rf_tx_count==base+3);          /* 现状:1500 超时 */
assert(fault && link_timeouts==1 && link_retries==3);
```
修复后同一序列断言：`link_retries==4`（第 4 次在 1600 ms 发出）、`link_timeouts` 在 `started+1500+guard(300)` 才 +1。**现状的失败点精确证明"末次发送只有 300 ms 窗口"**。再加一个参数化版本扫 `rto ∈ {20,100,200,400}`，断言"任何 rto 下，最后一次成功发送距离 deadline ≥ guard"，把 rto=200 比 rto=100 更差的非单调性也钉住。

### T3 `test_rejected_tx_is_counted_and_recovers`（钉 M3 + F4）
```c
reset_app(); dap_request(0x05); step(10);               /* phase==2 */
unsigned tx=mock_rf_tx_count; mock_rf_send_ok=false;
step(500); step(900); step(1300);
assert(phase==2 && mock_rf_tx_count==tx && link_retries==0);   /* 完全静默 */
step(1600); assert(fault && link_timeouts==1);
```
修复后追加 `assert(tx_blocked>=3)`。现状断言"`link_retries` 冻结"就是 M3 的判别指纹，任何人都能复现。

### T4 `test_hello_timeout_is_not_a_fault`（钉 M7，排除项）
```c
reset_app(); session=0; dap_request(0x02); step(100);
assert(phase==1 && last_packet().type==LINK_HELLO);
step(1600); assert(link_timeouts==1 && phase==3 && !fault);
step(1601); assert(mock_dap_response_len==2 && mock_dap_response[1]==0xff && !fault);
step(1602); assert(phase==1);                    /* 下一条请求重新 HELLO */
```
钉住"`link_timeouts` 同时统计 phase-1 与 phase-2"这一语义陷阱：**故障后若 `link_timeouts>0` 但 `fault==0`，说明只是 HELLO 打不通。**

### T5 `test_cdc_response_validation_latches_without_timeout`（钉 M10，判别式）
```c
reset_app(); mock_dtr=true; step(10);            /* CDC 请求, cdc_tx_len==0 */
struct link_packet p={0}; p.type=LINK_RESPONSE; p.session=session; p.sequence=sequence;
p.length=4; p.data[0]=WD_CMD_UART; p.data[1]=WD_STATUS_OK; p.data[2]=1; p.data[3]=0;
queue_packet(&p); step(11);
assert(fault && link_timeouts==0 && mock_serial_state==0x40);
```
证明"`fault==1 && link_timeouts==0`"只可能来自 M10；并保证在健康往返中该分支永不被触发（在 T6 的 loopback 里断言 `fault==0`）。

### T6 `test_mixed_bulk_cdc_loopback_no_fault`（端到端家族测试，钉 M1/M5/M6）
用真实 `link_slave_handle()` 做一个闭环，不需要新增 mock 能力：
```c
static struct link_slave peer;  static unsigned tx_sent;  static bool have_slave_frame;
static uint8_t slave_frame[78];
/* 每一轮: (1) step(now) 让主机发/重传; (2) 把新出现的 mock_rf_tx 帧喂给 peer,
   (3) 若 peer 返回 true 且本帧属于本次要丢的帧则丢弃，否则 queue_packet 回主机;
   (4) 主机 phase==0 时再 dap_request()/CDC 数据交替注入。 */
for (unsigned i=0;i<2000 && !fault;i++) { now+=17; pump_once(now, /*drop_every=*/29); }
assert(!fault && link_timeouts==0 && completed>=400);
assert(link_retries>0 && link_rejected>=0);           /* 重传必然发生，但不应超时 */
link_slave_init(&peer);                                /* 中途模拟从机复位 */
for (unsigned i=0;i<50 && !fault;i++) { now+=17; pump_once(now,0); }
assert(!fault);                                        /* 现状必失败（M2），修 F3 后通过 */
```
`drop_every=29` 注入周期性丢帧，验证"丢帧可恢复"（`link_retries>0 && link_timeouts==0`），再单独把丢帧率拉到 100% 断言"必然超时且必然锁存"，把两条时间常数（`REQUEST_TIMEOUT_MS`、`bound()`）钉成可回归的数字。

### T7 `test_nonce_never_repeats_across_handshakes`（钉 M8 的证伪）
```c
reset_app(); session=0; dap_request(0x02); step(100);   uint32_t n1=nonce;
/* 让 phase-1 超时，再用 CDC 在同一毫秒内触发下一次 HELLO */
mock_dtr=true; step(1600); assert(phase==3 && link_timeouts==1);
step(1600);                    /* 同一 now: phase3 交付 -> cdc_request -> begin_request */
assert(phase==1 && nonce!=n1);                        /* 相同 now 也不同, 因为 generation 参与 */
/* 反向断言 generation 的注入性: */
reset_app(); generation=1; session=0; dap_request(0x02); step(100);
uint32_t a=nonce; phase=0; session=0; sequence=1; generation=2; step(100); assert(nonce!=a);
```
最后一条断言的就是 §M8 的证明：`g1≠g2 ⇒ g1*K≠g2*K (mod 2^32) ⇒ nonce 必不同`。

> 说明：`tests/test_host.c` 覆盖的是**旧版** `src/main.c` 主机（它 `#include "../src/main.c"`），不是 `app.c` 的调度器。当前 `app.c` 主机路径只有 `tests/test_app.c` 覆盖，这正是这些缺陷没被发现的原因之一。

---

## 6. 我不能确定的（需要插桩）

1. `RFIP_SetRx/RFRole_Stop/RFIP_StartTx` 的真实返回码语义与失败率 —— 射频库是二进制（`vendor/ch570/EVT/EXAM/RF/LIB/CH572rf.h` + lib），只能靠 F4/F5 的计数器观测。
2. 事故当时 `retry.rto` 是否已饱和到 400（M1 的成立条件）—— 需要 `WD_CTL_STATS[11]`。
3. 从机是否在事故前经历过复位/APPLY/外来 HELLO（M2 的触发）—— 需要从机侧 `link_rejected` 与 `completed` 的上报（当前 `WD_CTL_STATS` 只在**本地**读，无法读从机）。
4. `dispatch()` 在真实 SWD 时钟下的耗时分布 —— 需要 F7 的 per-request 快照。
5. `radio_callback()` 里"TX 期间收到的 pending RX 完成事件会在 `RFIP_StartTx` 之后才被服务、从而在 TX 进行中调用 `RFIP_SetRx`"这一时序（`radio_send()` 全程关中断 + 回调末尾无条件 `start_rx()`）是否会中止发送。`PROVEN` 的是这个时序窗口存在；后果未知。若它确实会中止发送，则表现为 `rx_count` 增长的同时 `tx_timeouts` 偶发 +1 —— 与 `docs/bulk-cdc-investigation.md` 观测到的"RF 硬件发送超时 0→8、应用重传可恢复"完全吻合，值得作为 M1 的次要放大器单独插桩。

---

## 7. 与既有文档的关系

`docs/bulk-cdc-investigation.md` 的结论（"已确认 fault 锁存机制，但首个请求为何超时未定"）与本报告一致，并有两点补充/修正：

- 该文档末条怀疑"HELLO nonce 可能重复 ⇒ 从机序号已推进而主机从 1 开始"：**本报告证伪为自然不可达**（§M8），`generation*K` 的注入性 + `now` 单调性共同排除；只剩 49.7 天 `platform_millis()` 回绕这一条路。真正需要修的同类风险是 **M2**（从机状态被清空且静默拒绝），它才是"序号/会话错位"的现实形态。
- 该文档把 `tx_timeouts`（RF 硬件）与 `link_timeouts`（应用 1500 ms）正确区分；本报告进一步给出 `rto/srtt` 判别式与 F1/F2，把"为什么 RF 偶发超时会升级成应用层 1500 ms 锁存"变成可计算、可修复的问题。
