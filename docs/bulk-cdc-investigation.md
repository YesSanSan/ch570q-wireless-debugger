# Bulk + CDC 并发排查

> **1.3.3 勘误与状态：**本文保留 1.3.2 当时的观测与解释。其中“`06 00 00 08` 是假成功”“`1d ff` 少字节”的协议判断错误：前者是正确的 16 位计数加错误状态，后者是正确的单状态字节。1.3.2 改成的 `06 00 08` / `1d 00 08` 才不符合命令布局。1.3.3 已修正固件和测试，并实现发送看护、从机有界门控与显式重连；原生及构建通过，2026-10-25 已完成[上板复测](v133-hardware-validation.md)，连续块读与 GDB 下载通过，客户端异常退出恢复仍未通过。旧文中的相反协议判断和“建议未实施”状态以 [重连实现说明](reconnection.md)为准。

本文记录三轮排查。第一轮（2026-10-24 上午）确认了失效机制并完成现场对照；第二轮（同日）用新写的交错压测拿到延迟分布，把“首个请求为什么超时”从猜测变成可计算的根因，并修复了四个可证缺陷；第三轮（同日下午至傍晚）换成真 OpenOCD+GDB 跑 flash 擦写、硬件断点与 1 KiB 帧缓冲读取，**复现了现场故障并推翻了“已彻底修好”的结论**。

> **先读这一段：**§1–§5 的根因分析本身成立（重传预算确实把 1500 ms 窗口压到只剩 4 次发送，响应形状确实错），修复也有“改前失败/改后通过”的原生测试钉住，§11 的 120 s 负载三组全绿。但 §12 的真实 GDB 复测表明：**1.3.2 并没有消除现场故障**。持续连续的帧缓冲读取会在 4–24 s 内把主机或从机卡成“一帧不发”的静默状态（`radio_send()` 卡在 `sending`），随后一次 1500 ms 超时就把 `fault` 锁存、会话关闭，只能复位。结论与建议修复见 §12。**§13 用一台 WCH-LinkW 跑了同一套负载做对照：同一个把 CH570Q 打死的帧缓冲读序列，WCH-LinkW 5/5 通过、单轮 3.2 s，而且卡住后能自愈——说明这是 CH570Q 的实现缺陷，不是无线调试器的宿命。**

## 结论摘要

1. **根因是重传预算在硬编码 1500 ms 截止时间前就被退避吃光，不是 USB 层故障。** `retry_delay()` 的退避被 `bound()` 夹在 400 ms，`rto` 一旦饱和到 400，`REQUEST_TIMEOUT_MS=1500` 内只剩 **4 次**发送（0/400/800/1200 ms），末次只有 300 ms 应答窗口。而 `retry_complete()` 允许**一次** ≥134 ms 的"首发即成功"样本就把 `rto` 钉到 400，并按 7/8 的 EWMA 保持约 20 个请求。实测该链路每次发送丢帧率约 **13.9%**，连续 4 次全丢的概率约 3.8e-4 —— 在数万次事务里出现两次完全符合观测。
2. **CDC 不是独立根因，是放大器。** 没有任何代码路径让 CDC 触碰 `session`/`sequence`/RF 状态机；CDC 抬高的是每次事务的时延尾部（p999 64 ms → 146 ms，120 s 长跑最大 305.76 ms），从而更容易产生 ≥134 ms 的样本、把 `rto` 顶到上限。这正是"Bulk 单跑不出、并发才出"的形状：先由 CDC 顶高 `rto`，再由其后的 Bulk 请求撞上只剩 4 次机会、末次仅 300 ms 窗口的时间表。
3. **超时之后的合成失败响应格式错误**，`DAP_TransferBlock (0x06)` 会返回 `{0x06, count=0, response=0x00}`，即"成功、传输 0 字"，而 `DAP_SWD_Sequence (0x1d)` 少一个字节。GDB 读帧缓冲走的正是 0x06，这会把它带进假成功/解析错位，比超时本身更容易表现为"主机会话锁死"。
4. **USB 复合层不背这个锅。** 独立审计核对到 SFR 地址：EP0/EP4 的 128 B 分配、EP1/2/3 各自的 OUT+0/IN+64、`T_LEN`/`CTRL` 字节互不重叠；`request_buffer` 的两个写者分别受 `REQ_IDLE` / `REQ_CONTROL_RX` 门控，CDC 也不占用 DAP 的那一份信用。CDC 的 EP3/EP4 有独立握手机制。
5. 已修 4 项，其中 3 项有"改前失败、改后通过"的原生测试钉住（见 §5）。剩余缺陷与未证明部分见 §6、§7。
5. 已修 4 项，其中 3 项有“改前失败、改后通过”的原生测试钉住（见 §5）。剩余缺陷与未证明部分见 §6、§7。
6. **上述 1–5 是 1.3.2 修复的直接依据，但不足以关闭这个问题。** §12 在真实 GDB 下复现了同一现场故障，并把触发点定位到 `radio_send()` 的静默失败（而不是重传预算）：故障窗口内 `tx` 增量为 0、清 `fault` 后仍死、只能复位；主机与从机两侧都能卡。因此把 §12 当作当前结论，§1–§11 当作已经取得的修复与量化基础。

## 1. 失效机制（第一轮已确认，仍然成立）

- `src/app.c:host_poll()`：phase 1/2 超过 `REQUEST_TIMEOUT_MS` 调 `fail_request(phase==2)`。
- `fail_request()` 用 `fault|=uncertain` 锁存；只有 phase 2 超时会锁存，phase 1（HELLO）超时不锁存。
- 锁存后 DAP 请求被合成失败响应，`cdc_request()` 直接 `return false`；**CDC 与 DAP 一起死**，这正是"Bulk 的故障打进串口"的表现。
- 唯一解锁途径是 generation 变化：USB 重新枚举、`SET_CONFIGURATION`，或 HID↔Bulk transport 切换。因此**故障后切到 HID 查状态会先清掉证据**，必须在原 Bulk 通道读本地 info/stats。
- 超时的请求可能已经执行过，所以不支持自动重新握手重放；这条安全约束保留，未修改。

## 2. 本轮实测

### 2.1 方法

新增 `tools/bulk_cdc_stress.py`：只 claim Bulk 接口 1，逐事务记录并把时间拆成两段——`write_ms`（Bulk OUT 被接受的时间，等价于设备 NAK 反压）与 `read_ms`（应答延迟）；任一事务超过阈值就立刻在**同一** Bulk 通道回读固件计数器，把该次事件的 `retransmissions`/`rejected`/`crc_errors` 增量留在证据里。CDC 由独立线程读取并记录间隔。

设备：USB 1209:0001，CDC COM14，固件 1.3.1，STM32F103C8T6 / OLED 示例，115200 8N1，无并行 OpenOCD。

### 2.2 延迟分布

| 指标 (ms) | Bulk 单独 20 s | Bulk + CDC 60 s |
| --- | --- | --- |
| 事务数 | 2838 | 5641 |
| p50 | 4.04 | 6.95 |
| p90 | 23.06 | 25.47 |
| p99 | 45.96 | 65.71 |
| p999 | 63.84 | 145.74 |
| max | 143.66 | 164.54 |
| mean | 7.01 | 10.40 |

计数器增量：

| 计数器 | Bulk 单独 | Bulk + CDC |
| --- | --- | --- |
| `completed` | +2838 | +8936 |
| `retransmissions` / `completed` | 13.9 % | 13.9 % |
| `crc_errors` / `rx` | 0.92 % | 1.33 % |
| `rejected` | +0 | +0 |
| `drops` | +0 | +0 |
| `request_timeouts` | 0 | 0 |

### 2.3 归因

- 最差的几次事件 `write_ms` 均 < 1 ms、`read_ms` 145–164 ms，`retransmissions` 增量 5–18。**延迟全部来自空中重传，USB 反压为零。**
- `rejected` 两轮都为 +0，`drops` 为 +0：**没有会话/序号失步，也没有单槽 mailbox 溢出**，即本次不是"链路错位型"故障。

### 2.4 长跑复现出 300 ms 量级的尾部

再跑 120 s（12483 次 DAP 事务 + CDC 6944 B）：p50 6.90 / p90 25.10 / p99 65.13 / p999 145.35 / **max 305.76 ms** / mean 9.44；`retransmissions` 2167、占 `completed` 的 11.1 %，`crc_errors` +410，`rejected` 仍为 +0。最差事件的 `write_ms` 是 0.43 ms——依然是纯空中延迟。

这个 305.76 ms 与第一轮现场 DP IDCODE 并发测得的 307.59 ms 几乎重合，把"并发尾部在 300 ms 量级"从一次性观测变成了可重复现象。而 `rto=400` 时最后一次发送只剩 300 ms 窗口，正好被这个尾部穿透。

### 2.5 为什么我这几轮还是没触发 fault

三轮跑下来 `request_timeouts` 始终为 0，`srtt` 始终 2 ms、`rto` 始终 20。原因在 `retry_complete()` 的 Karn 式过滤：`if (s->attempts != 1) return;`——凡是发生过重传的慢事务都不会进入估计器。我的探测命令（DAP Info / DP IDCODE）在目标侧几乎是瞬时的，所以"首发即成功但耗时 130 ms 以上"的样本根本不会出现，`rto` 自然不会饱和。

需要这种样本的是**从机侧本身耗时的请求**：真实 GDB 的 flash 操作、硬件断点、1 KiB 帧缓冲读取，`dap_process()` 在 100 kHz SWCLK 下可达数百毫秒。这也解释了为什么前三轮原 GDB 回放都通过：回放总量只够覆盖几万个事务里的一小部分，而 `fault` 需要的正是"`rto` 已饱和"与"随后连续丢 4 次"同时成立。

## 3. 根因：RTO 饱和把重传预算压到 4 次

### 3.1 退避时间表

`src/retry.c:retry_delay()` 把 `rto` 左移 `attempts-1`（上限 4）后交给 `bound()` 夹到 20..400 ms。因此 `rto ≥ 100` 时退避被完全抹平，重传变成固定节拍：

| `rto` | 发送时刻（相对 `started`, ms） | TX 次数 | 末次应答窗口 |
| --- | --- | --- | --- |
| 20 | 0, 20, 60, 140, 300, 620, 940, 1260 | 8 | 240 ms |
| 100 | 0, 100, 300, 700, 1100 | 5 | 400 ms |
| 200 | 0, 200, 600, 1000, 1400 | 5 | **100 ms** |
| 400 | 0, 400, 800, 1200 | **4** | 300 ms |

注意 `rto=200` 比 `rto=100` 更差：可用性取决于节拍与 1500 的相位，这是硬编码截止时间的直接后果。

### 3.2 `rto` 如何被一次慢样本钉到上限

`src/retry.c:retry_complete()` 只对"首发即成功"的样本更新估计（`attempts != 1` 直接返回），首个样本走 `srtt=sample, variance=(sample+1)/2`，随后 `rto = bound(srtt + 4*variance)`。取 `sample = 134 ms` 即得 `134 + 4*67 = 402 → 400`。之后 7/8 的 EWMA 与 3/4 的方差衰减极慢，要把 `rto` 拉回 100 以下需要连续二十多个干净样本。**在这串请求期间，主机始终只有 4 次发送机会。**

第一轮现场最大时延 Bulk 单跑 64.83 ms、并发 307.59 ms；本轮 p999 已达 145.74 ms。也就是说"≥134 ms 的首发样本"在这个系统里是常态量级，不是极端离群值。

### 3.3 事件链

| # | 位置 | 事件 | 关键字段 |
| --- | --- | --- | --- |
| 1 | 稳态 | 某次首发即成功但耗时 150 ms | `attempts=1`, `srtt=150`, `variance=75`, **`rto=400`** |
| 2 | `host_poll` | 取到 DAP 请求 | `owner=0`, `phase=2`, `session=S`, `sequence=n` |
| 3 | 空中 | 前 1–3 次发送落在从机的 `dispatch()` 窗口内被丢弃 | `drops` 或从机 `link_rejected` 增长 |
| 4 | `started+400/800/1200` | 重传 3 次，`link_retries` 固定 +1/次 | `attempts=4` |
| 5 | `started+1500` | 截止时间到 | `link_timeouts+1`, `fail_request(true)` |
| 6 | 之后 | `fault` 锁存，CDC 与 DAP 全部拒绝，需重新枚举 | `fault_locked=true` |

死因是纯算术：第 4 次发送发生在 `started+1200`，只给对端 300 ms，而并发场景的尾部已经达到 146–307 ms 量级。

### 3.4 CDC 的因果角色

| 机制 | 与 CDC 的关系 |
| --- | --- |
| RTO 饱和 + 固定截止 | **放大器**：CDC 抬高时延尾部 → 更多 ≥134 ms 样本 → `rto` 顶到 400 |
| 从机 `dispatch()` 串行执行 DAP 与 UART 两类请求 | **协同**：CDC 是 `WD_CMD_UART` 的唯一生产者，把从机前台占得更满 |
| UART ISR（仅 CDC 打开后使能）推迟 BLE ISR | **协同**：可能增加丢帧率 |
| 会话/序号/RF 状态机 | **无关**：代码上 CDC 没有任何入口能改写它们 |

## 4. 放大症状：超时后的合成响应格式错误

`src/app.c:fail_request()` 原来的形状表把两个命令写反了：

| 命令 | 原返回 | 正确形状 |
| --- | --- | --- |
| `0x06` DAP_TransferBlock | `{06, 00, 00, 08}`（4 B，`TransferResponse=0x00` 即 OK） | `{06, 00, 08}`（3 B） |
| `0x1d` DAP_SWD_Sequence | `{1d, ff}`（2 B，默认分支） | `{1d, 00, 08}`（3 B） |

0x06 被 GDB 用来读取目标内存（例如 1 KiB 帧缓冲）。收到"成功、0 字"后主机会继续按 0 字推进而不是报错，表现为卡住/重试风暴——这与现场描述的"请求超时并锁定主机会话"比超时本身更吻合。0x1d 少一个字节会让主机把下一字段读成序列数据。

## 5. 已修复（4 项）

三项带"改前失败、改后通过"的原生测试：

| 修复 | 位置 | 测试 | 改前 |
| --- | --- | --- | --- |
| 重传间隔按剩余预算收缩，保证 16 次发送仍能落进截止窗口 | `src/app.c:host_poll()` + `RETRY_ATTEMPTS_IN_BUDGET` | `test_retry_budget_survives_saturated_rto` | `sends>=12` 断言失败（实际 3 次重传） |
| 合成响应按命令给出正确形状，0x06 不再假成功 | `src/app.c:fail_request()` | `test_timeout_reply_shapes` | `mock_dap_response_len==3` 断言失败（实际 4 字节） |
| `SET_INTERFACE` 只复位所选接口的端点状态，不再跨接口清掉数据接口 3 的待收/待发数据和线路编码、DTR | `src/usb_dap.c`：拆出 `cdc_control_reset()` / `cdc_data_reset()` | `test_set_interface_isolation` | `usb_cdc_control_line_state()==3` 断言失败（DTR 被清） |
| `radio_send()` 因 `paused`/未初始化被拒时计入 `tx_failures`；`start_rx()` 重挂失败计入 `rx_timeouts` | `src/radio.c` | 无（radio 无原生测试骨架） | —— |

第 4 项的意义在于可观测性：原来"发不出去"和"发出去丢了"都返回 `false` 且不计数，现场无法区分（判别式见 §6）。

重传收缩的效果：`delay = min(retry_delay(), remaining/16)`。`rto=400` 时有效间隔从 400 ms 降到 ≤93 ms，且随剩余预算几何收缩，整个 1500 ms 窗口内可发 40 次以上；时间常数（`REQUEST_TIMEOUT_MS`）与安全语义（不自动重放）都没有改动。代价是最后十几毫秒会连续发帧（间隔缩到 1–2 ms，短于一个往返），但这段只发生在一个已经在失败的请求尾部，且每帧只占约 312 µs 空口时间；如果后续想更保守，可以给间隔加一个"不小于实测 `srtt`"的地板。

## 6. 已确认但本轮未修

1. **`SET_INTERFACE(0|1, alt 0)` 会调用 `dap_reset()`**：清掉已装载应答、`generation++`，并让 `app.c` 顺手 `fault=false`。描述符里每个接口只有一个备用设置，所以它对 DAP 会话是本该无语义的。它是唯一能在任意时刻摧毁 DAP 信用的主机请求，也是唯一能"意外清掉故障证据"的合法请求。**未修原因**：`include/usb_dap.h` 把"generation 在 DAP SET_INTERFACE 上变化"写成了契约，`tests/test_usb.c` 也把它当预期行为钉住；改动需要同步更新契约与测试，且无现场请求记录佐证其触发。
2. **REQ_RESPONSE / REQ_CONTROL_RX / REQ_CONTROL_STATUS / REQ_READY 都没有超时出口。** `usb_dap.c` 里没有时钟，`app.c` 的 phase 3 也没有截止时间。若主机在应答装载后取消 Bulk IN URB，EP2 OUT 会被永久 NAK，CDC 也会停（`cdc_request()` 只在 `!phase` 时运行）。**未修原因**：直接在 `app.c` 加超时会误伤 USB 挂起——`send_response()` 在 `suspended` 时同样返回 `false`，一次长挂起就会被误判为故障并锁存。正确做法是先给 `usb_dap.h` 加一个挂起状态访问器，再让超时只在总线活动时计时。
3. **`ep0_in()` 会把已 STALL 的 EP0 改回 NAK**（潜在的规范偏离，符合规范的主机不可达）。
4. **`src/main.c` / `src/usb_diag_main.c` 使用未加标签的 `usb_dap_send_response()`**，与 `include/usb_dap.h` 的"异步调用必须用 tagged 接口"契约冲突。交付镜像走 `src/app.c`，不受影响。
5. **`RB_UIF_FIFO_OV` 被清掉但没有任何事件记录**；完整镜像又强制 `WD_UART_LOG=0`，等于诊断信息被丢两次。
6. **`host_discover()` 与请求重传共用 `last_tx`**（当前不可达，但任何重构都会把它变成"发现帧推迟重传一个 rto"）。

## 7. 仍未证明的部分

- **事故当时 `rto` 是否已饱和到 400。** §3 证明的是"一旦饱和必然超时"且"饱和很容易发生"，但没有当时 `WD_CTL_STATS[11]`（`rto`）的记录。本轮三轮压测的 `rto` 全部停在 20（原因见 §2.5），**因此本轮并未在硬件上重现 fault**；重现必须使用从机侧本身耗时的请求（真实 GDB 的 flash / 断点 / 1 KiB 读取），而这类操作有写入目标的风险，不能靠盲测刷多。判别指纹：故障瞬间 `rto==400` 且 `srtt` 在 100+ 区间、`link_retries` 按固定节拍 +3。
- **从机是否曾静默丢弃整个会话**（`link_slave_handle()` 在 `p.session != s->session` 时一个字节都不回）。死锁路径已证，触发源未证。判别指纹：故障窗口内 `rx_count` 完全冻结、`crc_errors`/`drops` 不增。
- **`radio_send()` 被拒 / RX 重挂失败是否真的发生过。** 本轮加了计数，下一次故障可读 `tx_failures`/`rx_timeouts` 增量；指纹是 `link_retries` 冻结而 `link_timeouts+1`。
- **首个异常事务的完整现场**（owner、命令、phase、session、sequence、重试次数、`request_state`）仍无法回溯。需要一个故障记录结构并通过 `WD_CTL_STATS` 追加读出；注意现有工具只解前 12 个 32 位值，追加字段对它们向后兼容。

## 8. 复现与取证命令

单一故障判别先看两个数：`fault_locked` 与 `link_timeouts`。**`fault==1 && link_timeouts==0` 说明不是超时**，而是 CDC 响应校验分支（`count>54 || response_len<4+count || response[2]>cdc_tx_len || 信用溢出`）直接置位。

```powershell
# 交错压测（Bulk + CDC），逐事务计时并在离群时抓计数器
build/host-venv/Scripts/python tools/bulk_cdc_stress.py --port COM14 --seconds 120 --outlier-ms 60 --output build/bulk-cdc-stress.json
# Bulk 单独对照
build/host-venv/Scripts/python tools/bulk_cdc_stress.py --no-cdc --seconds 60 --output build/bulk-cdc-stress-control.json
# 只读 DP，验证 SWD 路径同样受压
build/host-venv/Scripts/python tools/bulk_cdc_stress.py --port COM14 --seconds 120 --dp-idcode --output build/bulk-cdc-stress-idcode.json
```

原有的 `tools/bulk_cdc_diagnostic.py` 仍负责"阶段式对照 + 故障快照"：

```powershell
build/host-venv/Scripts/python tools/bulk_cdc_diagnostic.py --port COM14 --seconds 30
# 失败后保持 Bulk 路由、关闭 OpenOCD 与串口，先取证再考虑重新插拔
build/host-venv/Scripts/python tools/bulk_cdc_diagnostic.py --snapshot-only --output build/bulk-cdc-failure-state.json
```

不要在取证前运行 HID 管理工具或重新插拔：generation 变化会先清掉锁存。

## 9. 第一轮现场证据（保留）

设备 USB 1209:0001，CDC COM14，固件 1.3.1，STM32F103C8T6 / OLED 示例，115200 8N1。

| 测试 | 单独 Bulk | Bulk + CDC |
| --- | --- | --- |
| DAP Info 固件查询，各 8 s | 927 次，最大 143.45 ms | 730 次，最大 144.06 ms，CDC 413 B |
| DP IDCODE 只读，各 15 s，100 kHz | 1615 次，最大 64.83 ms | 1069 次，最大 **307.59 ms**，CDC 767 B |

两组均未新增 `request_timeouts`，`fault_locked=false`，IDCODE 均为 `0x1BA01477`。原 `tools/verify_oled_gdb.py --bulk` 与 CDC 并发的三轮均输出 `HARDWARE_VALIDATION_PASS`（软件复位、main/oled_update 硬件断点、单步、RAM 参数修改、暂停恢复、1 KiB 帧缓冲读取）。逐轮产物见 Git 忽略的 `build/bulk-cdc-*`。

**验证边界**：短时通过不代表历史偶发故障已修复；以下修复改变了超时概率与失败路径的形状。烧录后复测见 §10。

## 10. 烧录后复测（2026-10-24 14:44）

新固件已写入现场主机，同一台设备、同一端口 COM14、同一目标板。

### 10.1 上电状态

计数器全零、`fault_locked=false`、`role=1`、`rto=100`（`retry_init` 初值，尚未适配）——确认是一次干净启动。

### 10.2 交错压测（120 s，参数与烧录前完全一致）

| 指标 (ms) | 烧录前 | **烧录后** |
| --- | --- | --- |
| 事务数 | 12483 | **13868**（+11 %） |
| p50 | 6.90 | 6.64 |
| p90 | 25.10 | 23.61 |
| p99 | 65.13 | 62.92 |
| p999 | 145.35 | **83.93** |
| max | 305.76 | **225.38** |
| mean | 9.44 | 8.55 |
| `retransmissions`/`completed` | 11.1 % | 9.6 % |
| `request_timeouts` | 0 | 0 |
| `fault_locked` | false | false |

**这个 225.38 ms 是修复生效的定量证据，不是巧合。** 按 §3.1，`rto=20` 时未加限制的第 5 次发送落在 `started+300`，只有一个 4 连丢事件才会在那里完成——烧录前的 305.76 ms 正是它。加限制后：`e=140` 时 `backoff=160`、`cap=(1500-140)/16=85` → 第 5 次发送移到 225 ms。两次长跑的最大值分别钉在 300 与 225 两个理论台阶上，说明设备跑的确实是带重传收缩的固件。更深丢包序列的改善更大：第 6 次 620 → 304 ms，第 7 次 940 → 378 ms。

### 10.3 原失效场景：GDB/OpenOCD 与 CDC 并发

一个终端 `tools/capture_uart.py --port COM14 --dtr --seconds 100`，另一终端 `tools/verify_oled_gdb.py --bulk`：

- GDB 端输出 `HARDWARE_VALIDATION_PASS`（软件复位、main/oled_update 硬件断点、单步、RAM 参数修改、暂停恢复、1 KiB 帧缓冲读取）；
- CDC 端同时收到 5021 B，目标 `frames` 从 418 推进到 436，`ready=1`、`i2c_errors=0`、`fault=0`；
- 全程累计 30002 次完成、`request_timeouts=0`、`fault_locked=false`。

### 10.4 全量计数器（约 3 万次事务后）

`rx=30003`、`tx=33201`、`completed=30002`、`retransmissions=3198`（10.7 %）、`crc_errors=638`（2.1 %）、`rejected=0`、`drops=0`、`tx_failures=0`、`tx_timeouts=0`、`srtt=2`、`rto=20`。

`tx_failures` 与 `tx_timeouts` 全程为 0 是**有信息量的阴性结果**：3 万次事务里射频从未出现"发不出去"或被拒发送，所以 §7 中的 M3（`sending`/`paused` 卡死）与 M6 类机制至少在本次负载下未发生；唯一的丢包渠道就是普通空口丢帧。

### 10.5 人为超时：`fail_request()` 响应形状上板验证

`tools/induce_timeout.py` 把主机信道切到 21、`APPLY`，对端仍在 16，于是每个请求都各等满 1500 ms。`APPLY` 同时把 `session` 清 0，所以超时发生在 **phase 1（HELLO）**，按设计**不锁存 `fault`**——这一点让测试变成可逆的：切回原信道后链路自己恢复，*不需要重新插拔*。

实测（`build/induced-timeout.json`）：

| 命令 | 实测耗时 | 实测响应 | 旧固件会返回 |
| --- | --- | --- | --- |
| `0x06` DAP_TransferBlock | 1499.9 ms | `06 00 08` | `06 00 00 08`（假成功） |
| `0x1d` DAP_SWD_Sequence | 1500.0 ms | `1d 00 08` | `1d ff` |
| `0x05` DAP_Transfer | 1500.8 ms | `05 00 08` | ——（同时也验证） |

同时确认：`request_timeouts` +3，`fault_locked=false`；切回信道后第一条 `DAP_Info` 在 **7.0 ms** 内返回（当时镜像还是 1.3.1），链路完好，信道 16 → 21 → 16 已恢复。
这一步同时是**主机镜像的身份判据**：旧形状完全没出现，证明设备跑的确实是带修复的镜像。也正因为如此，后续把版本号升到 **1.3.2**。注意 `DAP_Info` 的版本字段由**从机**镜像提供，所以要把两块板都刷新后才能在线上读到 1.3.2；工具已改为按 `1.3.` 前缀兼容，不会因版本号变化而误报。

### 10.6 仍只有原生测试覆盖的一项

- **`SET_INTERFACE(2)` 隔离**：需要接口收件人的控制传输，而 Windows 上 libusb 只能对已 claim 的接口做控制传输；接口 2 被 usbser 占用，因此 `libusb_control_transfer` 返回 `LIBUSB_ERROR_NOT_SUPPORTED`（已实测：设备收件人可以，接口收件人不行）。该项仍由 `tests/test_usb.c` 钉住；如需上板验证，可用 USB 分析仪或对接口 2 临时绑定 WinUSB（会失去 COM 口）。工具已备在 `tools/verify_bulk_cdc_fixes.py`。

> **未测到的分支**：`fault` 锁存发生在 **phase 2** 超时。要在保持会话的前提下让对端不答，需要从机重启/从机 APPLY/外来 HELLO，主机侧无此开关（`APPLY` 本身会清 `session`）。该分支由 `tests/test_app.c:test_timeout_fault_lock_and_generation` 覆盖；2026-10-24 的强杀客户端意外在板子上触发了同一条路径，见 §11.5 末尾的方框。

## 11. 真实 SWD 负载复测（2026-10-24 15:35–15:45）

### 11.1 为什么要换负载

§2 和 §10 的交错压测只用两种探针：`DAP_Info`（从机自己回答，5 B 请求 / 4 B 应答）和一次 DP IDCODE 读（一条物理 SWD 事务）。两者都是最便宜的交换，64 字节的 Bulk 包只用了几字节。真实调试器不是这么说话：OpenOCD 会发多事务 `DAP_Transfer`（选 bank + 写 TAR + 读 DRW 打在一个请求里）、15 字的 `DAP_TransferBlock`（4 + 4×15 = 64 B，正好占满应答包）、块写，以及 `DAP_SWD_Sequence` 原始捕获。这些请求让射频帧变长、让从机忙上几十毫秒，正是历史超时发生的那类负载。

新工具 `tools/swd_workload.py` 直接对真实目标（STM32F103C8T6，IDCODE `0x1BA01477`）跑这套负载，并把每个应答当断言而不是流量：

- 每个周期 4 个 DAP 请求：DP IDCODE → `DAP_Transfer`（SELECT + CSW + TAR + AP 读，共 4 个事务）→ 15 字 flash 块读（满包）→ `DAP_SWD_Sequence` 原始 IDCODE 捕获，再补线复位与空闲。
- 每 20 个周期做一次真调试器会做的事：halt → 读 DHCSR → 经 DCSR/DCRDR 读 SP 与 PC → 在 SP 以下的死栈区做 14 字块读 / 块写 / 回读 / 恢复 → resume → 确认 `S_HALT` 清零。
- 校验：IDCODE 恒为 `0x1BA01477`；flash 块读必须逐次逐位相同（不可变内存 = 载荷损坏判据）；SP 落在 SRAM、PC 落在 Flash；RAM 写入-回读-恢复逐字节一致。

```powershell
build/host-venv/Scripts/python tools/swd_workload.py --seconds 120
build/host-venv/Scripts/python tools/swd_workload.py --port COM13 --seconds 120
```

### 11.2 结果（各 120 s，同一目标、同一信道）

| 指标 | A：Bulk 单独 | B：Bulk + CDC（100 kHz） | C：Bulk + CDC（1 MHz） |
| --- | --- | --- | --- |
| 周期 / DAP 事务 | 992 / 6197 | 671 / 4191 | 733 / 4578 |
| 延迟 p50 / p90 | 7.85 / 38.13 ms | 12.87 / 59.44 ms | 10.84 / 47.54 ms |
| 延迟 p99 / p999 | 67.23 / 223.63 ms | 146.49 / 300.80 ms | 145.84 / 300.56 ms |
| 延迟 max | 298.41 ms | 388.73 ms | 373.15 ms |
| `crc_errors` | 76 | 110 | 133 |
| `retransmissions` / `completed` | 38 % | 35 % | 36 % |
| `request_timeouts` | 0 | 0 | 0 |
| `fault_locked` | false | false | false |
| `tx_failures` / `tx_timeouts` | 0 / 0 | 2 / 2 | 3 / 3 |
| 功能校验 | 全部 100 % | 全部 100 % | 全部 100 % |
| CDC 收到 | — | 5795 B / 95 行 | 5795 B / 95 行 |

三组都没有一次请求超时、没有锁存、没有一次数据损坏，`drops` 全为 0。CDC 并发把尾部抬高约 30–40 %（p99 67 → 146 ms、p999 224 → 301 ms），但离 1500 ms 截止线仍有 5 倍以上余量。

端到端再跑一次原失效场景（`tools/verify_oled_gdb.py --bulk` 并发 `tools/capture_uart.py`）：GDB 输出 `HARDWARE_VALIDATION_PASS`（软件复位、main/oled_update 硬件断点、单步、RAM 参数修改、暂停恢复、**单次** 1 KiB 帧缓冲读取），同时 CDC 收到 2826 B，目标 `frames` 0 → 232、`ready=1`、`i2c_errors=0`、`fault=0`。

> 注意：这一轮只读了 **1 个 KiB** 帧缓冲。当天稍后把同样的读连续做 8 次就复现了现场故障，见 §12——本节“全绿”不代表故障已消除。

累计（本次上电复位后）：**28886 次完成、0 次请求超时**、`fault` 从未锁存、`tx_failures` 5 次且全部被重传吸收。设备同时报告 `DAP_Info(4) = "1.3.2"`，确认两块板都是新镜像。

### 11.3 真实负载下重传率不再等于空口丢帧率

15 字块读的服务时间 p50 = 40.9 ms（100 kHz）/ 28.9 ms（1 MHz），**都高于 RTO 下限 20 ms**，所以主机在读块期间必然重传一次：这就是 35–38 % 的重传率配 1.4 % CRC 错误的来源。§10.2 用 1 字探针测到的 9.6 % 才接近纯空口丢帧。

同一机制解释 `rejected`：RTO 到期重传会让从机重发缓存应答，而主机此时已在 phase 3，于是计一次 `link_rejected`（A 组 813、B 组 1、C 组 0——取决于应答到达时刻，与链路质量无关）。读 `link_rejected` 时要知道这一点。

顺带一个结论：把 SWD 时钟从 100 kHz 提到 1 MHz 只把块读 p50 从 40.9 ms 压到 28.9 ms（−29 %）。慢的不是 SWD 位时间，而是每个 AP 事务的固定开销（读 + 强制 RDBUFF + GPIO/延时），靠提时钟改善大批量传输效果有限。

### 11.4 新缺陷：`DAP_SWD_Sequence` 读序列后必须补线复位

A/B 实测（各 3 轮，脚本见 Git 忽略的 `build/swd-raw-abi.py`）：

| 做法 | 捕获本身 | 之后 5 次 DP IDCODE |
| --- | --- | --- |
| 捕获后发线复位 + 空闲 | 5/5 正确 | 5/5 正确 |
| 捕获后不发线复位 | 第 1 次正确，之后全 1 | 0/5，全部 `SWD_ERROR` |

现象：`0x1d` 请求本身成功（ACK=1、数据 `0x1BA01477`），但紧随其后的任何 `DAP_Transfer` 都返回 `05 00 08`（`SWD_ERROR = 8`，读不到有效 ACK），再往后连 `0x1d` 自己也只能读到全 1（目标已不再驱动 SWDIO）。发一条线复位（`0x12` 64 位 0xFF）即恢复：3/3 复现、3/3 恢复。

源码定位：`src/swd.c` 里 `swd_read_sequence()` 是唯一以 `dio_in()` 结束、不恢复 `dio_out()` 的路径；`swd_transfer()` 的三条分支（正常读、正常写、WAIT/FAULT/无效 ACK）都以 `dio(1); dio_out()` 收尾。读序列固定读 40 位而实际应答 37 位，多出的 3 个周期里 SWDIO 处于释放态，之后也没有人把线拉回输出高。`tools/swd_identify.py` 一直靠"每次捕获后补线复位"规避，所以这个坑之前没被踩到。

建议修复（未实施，需烧录验证）：`swd_read_sequence()` 结束处补 `dio(1); dio_out();`，并考虑在尾部补 2–3 个驱动空闲周期。`swd_workload.py` 目前按"捕获后线复位"写，并对每一步校验状态字节、必要时自动重同步（本次三组跑共 0 次重同步）。

### 11.5 新缺陷：主机可进入"静默静音"状态

强杀压测客户端（留下一个未取走的 Bulk 应答）后，主机进入如下状态：

- DAP 请求照样在 **1.500 s** 后超时并返回合成形状（`00 00`、`02 00`、`12 ff`、`05 00 08`），`request_timeouts` 逐次 +1；
- 但整个 1.5 s 窗口内 **`tx` 增量为 0**，`tx_failures`、`tx_timeouts` 也都是 0——`radio_send()` 一帧都没发，而且走的是唯一不计数的那条路径 `if (sending) return false;`（`src/radio.c`）；
- `WD_CTL_APPLY` 返回 OK 但只是暂存，真正的 `radio_reconfigure()` 在 500 ms 后要等 `radio_is_idle()`（= `!sending && !ready`），永远等不到；`WD_CTL_IDLE` 返回 INVALID；
- 换代也救不了：`SET_INTERFACE(1)` 因传输未变而不 `++generation`；切到 HID 能 `++generation` 并清掉 `fault`，但射频静音照旧；
- 唯一恢复手段是复位：拔插主机 USB 后计数器全零。这也解释了现场历史上"只拔插主机 USB 即恢复"为什么有效。

建议修复（未实施）：给 `sending` 加超时看护——超过 N ms（例如 100 ms）仍未收到 TX 完成中断，就按发送失败处理：清 `sending`、`start_rx()`、`++tx_timeouts`，让下一次重传有机会发出。

> 一次副作用收获：强杀时那个未完成的请求正好按 **phase 2** 超时，`fault` 被锁存（`request_timeouts` +1、`fault=1`，之后所有命令都返回合成形状）。这补上了 §10.6 遗留的"phase-2 分支只有原生测试覆盖"。`fault` 随后被 HID 换代的 `generation` 变化清零，与设计一致。

### 11.6 仍不可观测的一项

`radio_stats_t` 里有 `rx_timeouts`（接收机重挂失败计数），但 `WD_CTL_STATS` 的 12 个字段里没有它，所以"接收机失聪"仍然无法远程确认，只能靠"主机 `tx` 正常增长但 `rx` 不动"间接判断。建议把它加进应答——会改变 `WD_CTL_STATS` 的长度，需同步改工具与文档。

## 12. 真实 GDB 场景复测（2026-10-24 16:00–19:50）

§11 的负载是本工具直接发的 DAP 命令。这一轮换回**真 OpenOCD + GDB**，按现场用法跑三件事：目标 Flash 擦写、硬件断点、1 KiB 帧缓冲读取。新增脚本 `examples/ks2-oled/scripts/verify-flash.gdb`，用 `tools/verify_oled_gdb.py --script verify-flash.gdb --timeout 600` 驱动。

### 12.1 Flash 擦写

目标镜像 2728 B（`.isr_vector` 0xec + `.text` 0x9a4 + `.data` 0x18）。

| 路径 | 结果 | 耗时 |
| --- | --- | --- |
| `ocd verify_image`（只回读比对） | 通过，2728 B 逐字节一致 | 1.0 s |
| `ocd flash write_image erase` + `verify_image` | 通过，写入 4.58 s、校验 1.36 s | 10.9 s |
| GDB `load`（擦除+编程+写后 `reset halt`） | **两次运行一次成功、一次失败** | 27.1 s / 17.2 s |

失败那次的后果是坏的：**Flash 被擦空**（回读全 `0xFF`）、CPU 进 lockup、主机 `fault` 锁存。OpenOCD 侧依次报 `Error connecting DP: cannot read IDR` → `AP write error, reset will not halt` → `DP initialisation failed`，然后 `gdb-flash-write-end` 的 `reset halt` 报错。用 `flash write_image erase` 恢复了现场。

单独隔离测试“编程 + 紧接 `reset halt`”（不经 GDB）**通过**，11.7 s；所以失败不在“写后复位”本身，而在 GDB `load` 这条路径上——它把擦除与编程拆成 `vFlashErase`/`vFlashWrite` 两段并额外触发 `gdb-flash-write-end`。在查清之前，**不要用 GDB `load` 刷这个目标**，用 `openocd ... -c "flash write_image erase <elf>" -c "verify_image <elf>"`。

### 12.2 硬件断点、单步、RAM 参数

两组硬件断点（`hbreak main`、`hbreak oled_update`）均命中，单步 PC 前进，`set var` 改写 RAM 参数即时生效、暂停后 `g_frame_count` 不再推进。这部分两轮都通过。

### 12.3 1 KiB 帧缓冲读取：连读 8 次复现现场故障

单次 1 KiB（`dump binary memory ... g_oled_buffer+1024`）多次通过；**连续 8 次（8 KiB）直接复现了现场记录里的那个失败**：

```
FRAMEBUFFER dumps=8 bytes=8192
Cannot access memory at address 0x20000008        # g_frame_delay_ms
```

OpenOCD 日志在这个位置开始连续刷 `CMSIS-DAP Protocol Error @ 0 (wrong parity)`。**这些不是奇偶错**：主机一旦锁存 `fault`，`fail_request()` 就把每个请求合成成 `06 00 08` / `05 00 08`，OpenOCD 把状态字节 8 读作协议错误，于是一条真错被放大成一片假奇偶错。排查时不要被这个信号带偏。

### 12.4 定位：不是重传预算，是射频静默卡死

三条互相独立的判据，实测四项全部指向同一个结论：

| 观测 | 空口丢包（重传预算） | 静默卡死 |
| --- | --- | --- |
| 失败窗口内 `tx` 增量 | 上百次重传 | **0** |
| 清掉 `fault`（HID 换代）后重试 | 立即恢复 | **仍 1.500 s 超时，`tx` 增量仍为 0** |
| 恢复手段 | 无需干预 | **只能复位** |

`radio_send()` 里只有一条不计数就返回 `false` 的路径：`if (sending) { return false; }`（`src/radio.c`）——`sending` 置位后若 TX 完成中断没来，就永远不会清。主机侧卡住表现为一帧不发；从机侧卡住更严重：`slave_poll()` 开头是 `if(pending_response) { if(radio_send(outgoing)) pending_response=false; return; }`，一次发送失败就把整个从机主循环锁死，连 `HELLO` 都不再应答（实测：主机 `tx` 正常增长、`rx` 恒为 0、`crc_errors` 不动）。

作为对照，今天一个 30 s 的正常 cycle 跑里出现过 555 ms / 662 ms(p999) / 880 ms(max) 的慢请求，其窗口 `tx` 速率是 **32–72 次/秒**——健康但慢的请求与静默卡死在 `tx` 速率上差两个数量级，这也是新加的 `tx_per_second` 判据的依据。

### 12.5 请求密度与故障时间

| 负载 | 请求密度 | 首次超时 |
| --- | --- | --- |
| cycle 模式，100 kHz / 1 MHz，各 120 s | 约 30 次/秒，混有多种命令 | 未发生（3 组全绿） |
| OpenOCD + GDB 连续帧缓冲读（1 MHz） | 约 27 次/秒 | 24 s |
| `--mode fb` 纯背靠背块读（100 kHz） | 约 19–21 次/秒 | **4 s（第 4 个 KiB）** |

不是单调的“越快越容易”：`fb` 模式密度更低却更快翻车，差别在于它**没有任何间隙、也不夹杂其他命令**。竞争条件看起来与“连续同类型请求”相关，而不是纯吞吐。同一轮里 30 s cycle 跑虽然全绿，但 max 已到 880 ms，离 1500 ms 截止线只剩 1.7 倍。

### 12.6 新增工具能力

- `--mode fb`：每周期 17 次 60 B 块读，正好一个 1 KiB 帧缓冲，对应 GDB 的 `dump binary memory`。
- `--stop-on-error`：遇到第一个非 OK 应答立即停下并快照计数器，不再靠重同步把现场掩盖掉（之前的版本会把失败重试掉，只留结果）。
- 离群记录新增 `window_ms` / `window_exchanges` / `tx_per_second`，用来区分“慢但健康”与“静默”。
- `fb` 模式第一笔必须先发一次 DP IDCODE：实测这块目标在换线复位后的第一条 AP 访问不被应答（`05 00 08`），先做一次 DP 读就正常。

复现命令：

```powershell
build/host-venv/Scripts/python tools/swd_workload.py --mode fb --seconds 120 --stop-on-error
```

### 12.7 建议修复（均未实施）

1. **`radio_send()` 的发送看护**：`sending` 置位后超过 N ms（建议 100 ms）仍未收到 TX 完成或超时中断，就按发送失败处理：清 `sending`、`start_rx()`、`++tx_timeouts`。这一条同时治主机与从机两侧的静默。
2. **从机 `pending_response` 门控加超时**：超时后清 `pending_response` 并把失败当作未知应答，继续服务链路。否则一次发送失败就是永久静音。
3. **`Fault` 与超时的耦合需要重新审视**：任何一次 1500 ms 超时都会锁存 `fault` 并关闭会话，而上面两个缺陷都能造出这种超时。在 1、2 修好之前，`fault` 锁存实际上是“缺陷放大器”。
4. **`rx_timeouts` 仍不可读**：接收机重挂失败计数在 `radio_stats_t` 里，但没进 `WD_CTL_STATS`，从机失聪只能靠“主机 `tx` 涨、`rx` 不动”间接判断。

### 12.8 验证边界

- Flash 擦写走 `flash write_image erase` 可用；GDB `load` 未通过，不要用。
- 1 KiB 帧缓冲单次读可用；连续多 KiB 会在 4–24 s 内卡死，卡死后整机静默，只能复位。
- 本轮所有结论都基于固件 **1.3.2**（`DAP_Info(4)` 实测 `31 2e 33 2e 32`），即已包含重传预算与响应形状修复；也就是说**这两项修复不足以消除现场故障**。

## 13. 与 WCH-LinkW 的对照复测（2026-10-24）

手边有一台第三方 mini 版 WCH-LinkW（原理图与固件为 WCH 官方；USB 标识 `1a86:8012`、序列号 `453A9BB8F6B1`、`DAP_Info(4)` = `2.0.0`）。它和 CH570Q 是同一类东西：2.4 GHz 无线 CMSIS-DAP。用它把 §11–§12 的负载原地重跑一遍，才能把「CH570Q 的缺陷」和「无线调试器都有的毛病」分开。

固定不变的条件：同一块 STM32F103C8T6、同一份 `build/ks2.elf`（116180 B）、同一台主机、同一套工具；变的只有探头和接口配置。两者的 CMSIS-DAP v2 布局不同，工具已改成自动识别端点——CH570Q 是 interface 1 + EP `0x02`/`0x82`，WCH-LinkW 是 interface 0 + EP `0x02`/`0x83`。

WCH-LinkW 没有 `0x80` 本地计数命令（回单个 `ff`），所以本轮所有计数器判据（`tx_failures`/`request_timeouts`/`fault`）对它不适用，**只有延迟、速率与功能校验可比**。下文 CH570Q 一侧的数字取自 §11/§12 已存档的同名工具结果文件。

### 13.1 烧录：`program` + `verify`，与 `scripts/upload.ps1` 接线完全相同

| 探头 | 次数 | 成功 | 单次中位数 | 区间 |
| --- | --- | --- | --- | --- |
| WCH-LinkW @1000 kHz | 10 | **10** | 14.45 s | 13.78–15.16 s |
| WCH-LinkW @100 kHz | 5 | **5** | 15.28 s | 15.03–15.70 s |
| CH570Q 1.3.2（§12.1） | 3 | 2 | 10.9 s（`flash write_image erase`+`verify_image`） | GDB `load` 那次把 Flash 擦空并锁死 CPU |

`upload.ps1` 的「失败自动重试 5 次」在这 15 次里一次都没用到。把速度从 1000 kHz 降到 100 kHz 只多花 0.8 s，说明这块板的烧录时间几乎全在 Flash 擦写上，SWD 位时间不是瓶颈。

### 13.2 SWD 负载：`swd_workload.py --mode cycle`（100 kHz，Bulk 单跑）

| 指标 | CH570Q 120 s | CH570Q + CDC 120 s | WCH-LinkW 30 s（6 轮） |
| --- | --- | --- | --- |
| cycle 数 | 992 | 671 | 708–862 |
| cycle/s | 8.3 | 5.6 | **23.4–28.7** |
| 事务/s | 51.6 | 34.9 | **148–180** |
| p50 | 7.85 ms | 12.87 ms | **4.4–5.6 ms** |
| p99 | 67.23 ms | 146.49 ms | **10.2–20.2 ms** |
| p999 | 223.63 ms | 300.80 ms | **14.2–20.2 ms** |
| max | 298.41 ms | 388.73 ms | 21–45 ms |
| 功能校验 | 全过 | 全过 | 全过 |

WCH-LinkW 的事务吞吐是 CH570Q 的 **3–5 倍**，尾部延迟低一个数量级。

但它并非无瑕：16 轮里 5 轮出现过一次**请求完全不返回**（Bulk OUT 被反压，或 OUT 在 0.24–0.5 ms 内被收下后 IN 永不到来），主机等满 5 s 超时；**探针随后自己恢复，不需要拔插**。按事务数归一化，约每 1–2 千次交换一次。

### 13.3 连续帧缓冲读：CH570Q 的专属缺口

同一 17 × 60 B 块读序列（一个 1 KiB 帧缓冲）：

| 指标 | CH570Q 1.3.2 | WCH-LinkW |
| --- | --- | --- |
| 单次 15 字块读 p50 | 38–41 ms | **7.4–8.1 ms** |
| 读满 1 KiB 耗时 | ≈0.65 s | ≈0.13 s |
| 20 s 内完成 cycle | 24–25 | 121–128 |
| 存活 | 第 4 个 KiB（约 76 次事务）永久静默 | 3/5 跑满 20 s，2/5 卡 5 s 后自愈 |

GDB 侧同形测试（OpenOCD + GDB，8 次连续 `dump binary memory`，各 1 KiB，1 MHz）：

| 探头 | 结果 | 单轮耗时 |
| --- | --- | --- |
| CH570Q（§12.3） | 第 24 秒失败，`Cannot access memory at address 0x20000008` | 24 s 未跑完 |
| WCH-LinkW | **5/5 通过**（共 40 次 1 KiB dump） | 3.13–3.37 s（首轮 6.03 s） |

这是本轮最关键的差别：**§12 里那个把 CH570Q 打死的负载，WCH-LinkW 连续跑 5 轮毫无问题**。

### 13.4 故障形状的差别

| | CH570Q 1.3.2 | WCH-LinkW 2.0.0 |
| --- | --- | --- |
| 卡住的样子 | 请求不返回（`radio_send()` 卡在 `sending`） | 请求不返回（OUT 反压或 IN 丢失，见 13.2） |
| 之后 | 1500 ms 超时 → `fault` 锁存 → 会话关闭 | 主机侧 5 s 超时，探针内部状态自行复原 |
| 能否自愈 | **不能**，只能主机拔插或从机断电 | **能**，下一轮直接正常 |
| 本轮触发率 | cycle 6197 次事务无故障；fb 76 次事务后永久卡死 | 16 轮中 5 轮偶发一次 5 s 无应答，全部自愈 |

也就是说，两者都不是完美的无线链路，但**失败代价差一个数量级**：一边是几秒停顿后继续，一边是整台调试器失联到复位。

### 13.5 结论与边界

- **「CH570Q 不如 WCH-LinkW」不是准确说法**：在 `cycle` 这类混合负载下 CH570Q 慢 3–5 倍但 120 s 零故障；只在**连续同类块读**（帧缓冲刷新）这个形状上，它 4–24 s 就永久卡死，而 WCH-LinkW 不会。
- **WCH-LinkW 的弱点是极高事务率下的偶发 5 s 停顿**，在真实烧录速率（OpenOCD `program`+`verify`，15/15）下从未出现。
- 因此 §12.7 的四条建议依然成立且更有针对性：CH570Q 要补的不是「更快的链路」，而是**发送看护**与**从机 `pending_response` 超时**——让一次失败变成可重试的失败，而不是永久静默。
- 边界：本章测的是 CH570Q **1.3.2** 与 WCH-LinkW **2.0.0**；目标板在这一轮被反复烧录，测的是 `build/ks2.elf` 运行时（§11–§12 的帧缓冲地址语义来自 `examples/ks2-oled`，本轮 `--address` 取 `0x20000000`，只影响内容不影响流量形状）。

## 14. 相关文件

- `docs/findings-link.md`：主机调度器 + RF 链路审计（RTO 饱和的算术推导、各机制事件轨迹与 `WD_CTL_STATS` 指纹表、F1–F7 建议、T1–T7 测试设计）。
- `docs/findings-usb.md`：USB 复合层审计（SFR 级缓冲核对、`request_state` 闭包与活性、C1–C6 缺陷、mock 能力边界）。
- `tools/bulk_cdc_stress.py`：交错压测工具（逐事务计时 + 离群时抓计数器）。
- `tools/induce_timeout.py`：可逆地制造一次真实 1500 ms 超时，抓取合成响应形状。
- `tools/verify_bulk_cdc_fixes.py`：`SET_INTERFACE` 隔离与 CDC 存活的线上验证（需能对接口 2 发控制传输的具体环境）。
- `tools/swd_workload.py`：真实 SWD 负载压测（多事务 `DAP_Transfer`、15 字块读写、`DAP_SWD_Sequence`、halt/寄存器/死栈往返），逐事务计时、逐步校验 ACK、离群时回读计数器，可选并发 CDC；`--mode fb` 复现连续帧缓冲读故障，`--stop-on-error` 保留首次失败现场。`--vid/--pid/--serial/--interface/--no-local-stats` 让它能在任意 CMSIS-DAP v2 探头上跑（端点自动识别），换探头就能做同口径对照；没有 `0x80` 计数命令的探头只失去计数器，不失去测量。
- `tools/flash_compare.py`：两探头、同一条 OpenOCD 烧录命令（与 `scripts/upload.ps1` 完全一致）的逐次基准，不内部重试，只统计首烧失败率与耗时分布。
- `tools/gdb_dump_stress.py`：OpenOCD + GDB 连续 1 KiB `dump binary memory` 压力（`--probe linkw|ch570q`），即 §12.3 那条打死 CH570Q 的命令行，可直接对两个探头跑同一个脚本。
- `examples/ks2-oled/scripts/verify-flash.gdb`：GDB 全流程脚本（flash 擦写 + 回读比对 + 硬件断点 + 单步 + 8x1 KiB 帧缓冲读），用 `tools/verify_oled_gdb.py --script verify-flash.gdb` 驱动。
