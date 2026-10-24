# 1.3.3 上板验收：SWD 负载、GDB 与恢复（2026-10-25）

本页记录 2026-10-25 对已烧录 **1.3.3** 的主从机做的上板复测，逐项对照 [1.3.2 的实测记录](bulk-cdc-investigation.md#12-真实-gdb-场景复测2026-10-24)与[结论页](swd-conclusions.md)。固件行为说明见[重连实现](reconnection.md)。

## 结论

**1.3.2 记录中的两个硬故障在这一轮都没有复现，并且是同一口径下反复通过：**

| 1.3.2 的故障 | 1.3.2 观测 | 1.3.3 本轮 |
| --- | --- | --- |
| 连续 fb 块读触发永久静默 | 第 4 个 cycle（约 4 s）后不再应答，合成超时 1500.1 ms | **134 cycle / 2576 次交换，0 超时、0 数据校验失败** |
| GDB 连续 8×1 KiB `dump binary memory` | 首轮约 24 s 失败，`Cannot access memory at 0x20000008` | **100 kHz 20/20 轮、1 MHz 20/20 轮，共 320 次 1 KiB dump 全部成功** |
| GDB `load` | 两次运行一次失败，失败时 Flash 被擦空、CPU lockup、`fault` 锁存 | **5/5 轮 `HARDWARE_VALIDATION_PASS`**（含 `load`、`compare-sections -r` 回读、启动检查、双硬件断点、单步、RAM 参数、暂停恢复、8×1 KiB 读） |
| OpenOCD `program`+`verify` | 仅 2728 B 小镜像有记录（10.9 s） | **10/10 成功**，ELF 文件 116180 B（非有效烧录字节数），中位 43.85 s（100 kHz） |

同时，三组 120 s 混合负载全部零请求超时、零数据校验失败，吞吐与 p99 均优于 1.3.2；但 1 MHz+CDC 的 max 上升，CDC 组也未固定相同的目标日志流量。**本地统计未观察到射频看护或显式重连触发；从机门控情况需另读远端统计。除 §H 的 HELLO 超时外，“故障后恢复”仍缺实板证据**，见[未验证](#未验证与待办)。

**新增的一条失败：强杀主机客户端后主机固件仍会卡死。** 本轮两次复现（详见[§F](#f-强杀客户端仍然打死主机)），设备保持枚举但 HID 写入返回 -1、Bulk 读超时、管理命令读不到，只能拔插。这是 1.3.2 已知问题的同族，1.3.3 未覆盖。

## 环境与口径

| 项目 | 值 |
| --- | --- |
| 从机版本 | `DAP_Info(4)` = `1.3.3`（HID 与 Bulk 均确认） |
| 主机版本 | `WD_CTL_RECOVERY_STATS` 返回 version 1（1.3.3 新增），`WD_CTL_STATS` 12 字段兼容 |
| 工具链 | Python 3.14 + `build/host-venv`（hidapi 0.15 / pyusb 1.3.1 / pyserial 3.5） |
| 主机接口 | Bulk（interface 1，EP 0x02/0x82）与 HID；CDC = COM13 |
| 目标 | STM32F103C8T6，SWD DPIDR `0x1BA01477` |

**目标镜像在这轮中途变过，读数的可比性要分开看：**

1. 本轮开始时目标里是 **ks2 电机镜像（ELF 文件 116180 B，不等于有效烧录字节数）**，向量表指向 `0x0800Bxxx`，UART 日志在 **~921600 baud**（`USART1_BRR=0x4E`，PCLK2 72 MHz）。这与 1.3.2 那轮的 2728 B ks2-oled 镜像（`0x080000ED` 起的小向量表）不同，所以 `swd_workload.py` 的 `flash_block_status` 内容与 1.3.2 记录不可逐字对照；本轮该工具的 Flash 校验是**同一次运行内**的自比对，仍成立。
2. 该电机镜像的 921600 波特率**从机配不出来**：`uart_bridge.c` 按 `div = round(clock/(baud*8))` 计算，100 MHz 下 921600 得到 13.56 → 取 14 → 实际 892857，误差 3.1 % 超过 2 % 门限，`uart_bridge_configure()` 直接拒绝并保留旧值。于是 CDC 只能收到误采样的乱码（约 37 B/s、可打印率 21 %、0 个换行、`framing_errors` 持续增长）。**这解释了本轮早段 CDC 的“乱码”，不是 1.3.3 的回归。**
3. GDB 脚本把目标刷成 ks2-oled（`clock=64000000`），其 UART 是 115200；此后 CDC 立即恢复为 **100 % 可打印、11 行 `[oled] ms=... frames=... ready=1 i2c_errors=0 fault=0`**。
4. 因此 **1.3.2 的“5795 B / 95 行干净日志”不能与 921600 目标下的字节数直接比**。三组 cycle 负载的**延迟与吞吐**仍可对照（负载形状一致），CDC 字节数不作为结论。

## A. 连续 fb 块读：1.3.2 的致命用例

命令与 1.3.2 复现时相同：

```powershell
build/host-venv/Scripts/python tools/swd_workload.py --mode fb --seconds 120 --stop-on-error
```

| 指标 | 1.3.2 | 1.3.3 |
| --- | ---: | ---: |
| 结果 | 失败（第 4 个 cycle 后静默） | **通过** |
| cycle 数 | 4 | 134 |
| DAP 交换数 | 76 | 2576 |
| p50 / p99 / p999 / max (ms) | 37.94 / 1500.1 / 1500.1 / 1500.1 | 41.39 / 223.79 / 662.87 / 800.64 |
| `fb_blocks` | 失败 @ index 3（good=6） | 134/134 |
| 请求超时 / `fault` | 1 / **锁存**（`fault_locked=true`） | 0 / 未锁存 |

100 kHz 下 p50 由 37.9 升到 41.4 ms（略慢），但**尾延迟从“锁死”变成最多 800 ms**，且 120 s 内没出现 1500 ms。注意 1.3.2 的另一份记录（`build/fb-repro.json`）在**第 1 个 cycle** 就失败，说明该故障在 1.3.2 上很快触发；本轮每个 cycle 连续读 17×15 字，即 1020 B，接近 1 KiB。`fb_blocks` 检查响应长度、完成数和 ACK，未逐字节校验帧缓冲内容；混合负载的 Flash 自比对与 RAM 写后回读另有校验证据。

## B. 混合 cycle 负载三组 120 s

条件与 1.3.2 相同（同工具、同目标、同为 `--mode cycle`）。CDC 组用 COM13。

| 组 / 指标 | 1.3.2 cycle | 1.3.3 cycle | 1.3.2 p50 | 1.3.3 p50 | 1.3.2 p99 | 1.3.3 p99 | 1.3.2 p999 | 1.3.3 p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Bulk，100 kHz | 992 | **1083** | 7.85 | **6.17** | 67.23 | **63.57** | 223.63 | **144.05** |
| Bulk+CDC，100 kHz | 671 | **826** | 12.87 | **11.33** | 146.49 | **73.13** | 300.80 | **226.32** |
| Bulk+CDC，1 MHz | 733 | **882** | 10.84 | **10.11** | 145.84 | **85.44** | 300.56 | 299.70 |

| 组 | 交换/s 1.3.2 → 1.3.3 | max 1.3.2 → 1.3.3 (ms) | 超时/损坏 1.3.3 |
| --- | --- | --- | --- |
| Bulk 100 kHz | 51.6 → **56.4** | 298.41 → **225.02** | 0 / 0 |
| Bulk+CDC 100 kHz | 34.9 → **43.0** | 388.73 → **337.74** | 0 / 0 |
| Bulk+CDC 1 MHz | 38.1 → **45.9** | 373.15 → **523.57** | 0 / 0 |

- 按 cycle 数计算，吞吐 +9.2 % / +23.1 % / +20.3 %；p99 在三组都下降，其中 CDC 100 kHz 减半（146.49 → 73.13 ms）。CDC 组的目标日志流量与波特率不同，提升不能全部归因于固件调度。
- **例外：1 MHz + CDC 的 max 由 373 升到 523.57 ms**（单次离群，p999 仍 299.7 ms，与 1.3.2 持平）。样本只有一组，不能证明回归，但也不该宣称“尾部全部改善”。
- 三组的 Flash 块读逐次逐位、RAM 写入-回读-恢复、暂停/恢复、双硬件断点前置检查全部通过（如 Bulk 组 `flash_immutable` 1082/1082、`ram_write_roundtrip` 54/54）。

## C. GDB 连续 8×1 KiB 帧缓冲读

`tools/gdb_dump_stress.py --probe ch570q`，每轮 = OpenOCD+GDB 启动 + 8 次 1 KiB `dump binary memory`。

| 条件 | 1.3.2 | 1.3.3 |
| --- | --- | --- |
| 1 MHz，20 轮 | 首轮约 24 s 失败（同一序列） | **20/20 通过**，单轮 6.99–10.32 s，共 160 次 dump |
| 100 kHz，20 轮 | 未记录 | **20/20 通过**，单轮 8.89–15.60 s，共 160 次 dump |

证据：`build/v133/gdb-dump-1m/ch570q-20261025-004947/summary.json`、`build/v133/gdb-dump/ch570q-20261025-004548/summary.json`（构建产物，可能被清理）。

## D. 烧录路径

| 路径 | 1.3.2 | 1.3.3 |
| --- | --- | --- |
| `openocd ... program+verify`（`flash_compare.py --probe wireless`） | 只有 2728 B 镜像的 10.9 s 记录 | **10/10 成功**，ELF 文件 116180 B，100 kHz，中位 43.85 s（41.38–45.96 s），无一例需要 `reset failed, trying force halt` 回退 |
| GDB `load`（`verify_oled_gdb.py --script verify-flash.gdb --bulk`） | 1/2，失败时 Flash 擦空 + lockup | **5/5 `HARDWARE_VALIDATION_PASS`** |

`load` 每轮都跑完整链路：`monitor reset halt` → `load`（擦除+编程）→ `compare-sections -r` 回读 → 复位启动并检查 `g_oled_ready=1 / g_fault=0` → `hbreak main` / `hbreak oled_update` → 单步 → 暂停中改 RAM 参数并验证冻结 → 8×1 KiB 帧缓冲读 → 恢复运行。五轮都通过，没有再出现擦空或 lockup。

对照 WCH-LinkW（1.3.2 记录，同一 ELF、1 MHz）：10/10，中位 14.45 s。**本轮 CH570Q 只跑了 100 kHz**，两者速率不同，不能据此比较烧录速度；CH570Q 的 43.85 s 里大部分是擦除+编程时间。

证据：`build/v133/flash-compare/wireless-20261025-023652/summary.json`（min 41.38 s / median 43.85 s / max 45.96 s）与每轮 OpenOCD 日志；GDB 五轮日志在 `build/v133/gdb-flash-round2..5.log`（首轮见 `verify_oled_gdb.py` 直接输出）。

## E. 恢复计数与 CDC 会话语义

`tools/debugger_ctl.py recovery-stats` 在整轮测试中：

| 计数 | 值 | 说明 |
| --- | --- | --- |
| `tx_watchdogs` | 0 | 没有发送完成事件缺失，看护未被触发 |
| `recovery_attempts` / `recovery_failures` / `recoveries` | 0 / 0 / 0 | 没有进入恢复流程 |
| `reconnects` / `response_expirations` | 0 / 0 | 本地主机未显式重连；从机门控过期情况需另读远端统计 |
| `rx_timeouts` | 0 → 29 → 7（中途拔插复位后归零） | `RFIP_SetRx()` 返回失败与非发送状态 TIMEOUT 回调的合计，不等于请求超时；30 min 浸泡内累计 7 次 / 42616 次交换 |
| `fault_locked` | 全程 false | 没有任何一次请求超时 |

本节默认是本地主机统计；不能据此推断从机的 `response_expirations` 或恢复计数也为零，需另读远端统计。`rx_timeouts` 在约 22 分钟真实负载（§H 诱导之前）里累积 29 次而不产生任何请求超时或数据校验失败，说明这些 RX 事件在本轮没有导致会话失败；但计数混合了两类来源，应后续拆分量化。§H 诱导三次超时后 `request_timeouts` = 3、`fault_locked` 仍为 false。

CDC 会话隔离按文档工作：每次 DAP 会话换代（工具 claim interface / 新会话）后 `cdc_quarantined=true`，把 DTR 拉低一次即清除（`!opened && usb_cdc_discard_pending()`），随后重新拉高 DTR 就能收到数据。

**工具改动：** `tools/swd_workload.py` 的 CDC 打开序列改为“先 DTR 低打开 → 250 ms → 拉高 DTR”。否则 1.3.3 的隔离会让 CDC 组静默测到 0 字节（测得“流畅”但根本没有 CDC 流量）。这是 1.3.3 语义要求的握手，不是放宽判据。

## F. 强杀客户端仍然打死主机

本轮两次触发，都不是固件故障注入，而是测试进程被外部杀掉：

1. 后台压测任务在 30 min 浸泡中途被宿主环境 kill；
2. 前台浸泡在命令 10 min 上限处被 kill。

两次结果相同：**CH570Q 主机保持 USB 枚举**（`VID_1209&PID_0001` 四接口与 COM13 都在），但

- HID 写入 `short write -1/65`；
- Bulk IN `USBTimeoutError`；
- `debugger_ctl.py` 的 `info` / `stats` / `recovery-stats` 全部 `Short HID write`；
- `pnputil /restart-device` 需要管理员权限，被拒；
- **只能物理拔插**。

这证明 1.3.3 **没有覆盖客户端异常退出后的可用性恢复**。源码中，共用 DAP 请求槽在 `REQ_RESPONSE` 等待对应 IN 完成，期间 HID/Bulk OUT 均 NAK；无人读取的响应是与现场吻合的候选原因。但当前日志不足以证明 MCU 主循环停机，或排除 Windows 驱动、EP0 与端点状态异常。应记录请求状态、generation、端点状态及主循环心跳进一步定位。`docs/reconnection.md` 的“强杀客户端后的恢复”本轮判为**未通过**。

复现步骤与现场：`build/v133/fb-soak-1800s.err`（第二次）以及后台任务日志（第一次，已被宿主回收）。

## G. 30 min 连续 fb 浸泡

`--mode fb --seconds 1800 --stop-on-error`，100 kHz，单会话连续 30 分钟（`elapsed_s = 1800.68`）。

| 指标 | 1.3.2 | 1.3.3 |
| --- | ---: | ---: |
| 结果 | 从未跑满（4–24 s 内静默） | **通过** |
| cycle / 近 1 KiB 连续读次数 | — | **2214** |
| DAP 交换数 | — | **42616** |
| 15 字块读次数 | — | 37638（2214 × 17） |
| p50 / p90 / p99 / p999 / max (ms) | — | 41.44 / 60.62 / 64.06 / 223.61 / **298.95** |
| 请求超时 / `fault` / 重同步 | — | **0 / 未锁存 / 0** |
| 协议校验失败 | — | **0**（响应长度、完成数与 ACK；未逐字节校验帧缓冲内容） |
| 计数器增量 | — | rx 56233、tx 85969、`crc_errors` 462、`drops` 0、`tx_failures` 0、`tx_timeouts` 0、重传 41592 |
| 侵入式检查 | — | 110 轮 halt / DCRSR / SP·PC / RAM 写入-回读-恢复 / resume 全通过 |

**关键点：30 分钟里最大单次事务 298.95 ms，没有一次接近 1500 ms 截止线。** 1.3.2 的同形负载在 4 s 内就永久静默。重传率约 48 %（41592/85969）与 1.3.2 记录的同量级（35–38 %），说明链路仍在靠重传维持，只是不再卡死。

证据：`build/v133/fb-soak-1800s.json`（30 KB，含逐周期明细）。

## H. 诱导 1500 ms 超时的响应与恢复

`tools/induce_timeout.py`：把主机射频切到别的信道后发 DAP 请求，制造真实 1500 ms 超时，再切回原信道验证链路恢复。

| 请求 | 实测响应 | 依据 | 耗时 |
| --- | --- | --- | ---: |
| `DAP_TransferBlock 06` | `06 00 00 08` | 16 位完成数=0 + 协议错误 | 1500.9 ms |
| `DAP_SWD_Sequence 1d` | `1d ff` | 单个 `DAP_ERROR` 状态字节 | 1501.1 ms |
| `DAP_Transfer 05` | `05 00 08` | 8 位完成数=0 + 协议错误 | 1500.5 ms |

- `all_replies_correct_shape = true`：与[超时响应格式勘误](reconnection.md#超时响应格式勘误)表逐字节一致，硬件上确认了 1.3.3 的修正；旧记录里的“`06 00 00 08` 是假成功”“`1d ff` 长度不够”在这个版本不成立。
- `fault_not_latched_on_phase1 = true`：phase 1（HELLO）超时不锁 `fault`，未触发重新枚举。
- `link_recovered = true`：切回原信道后 `DAP_Info` 立即返回 `1.3.3`、RF 配置可读；随后 10 s cycle 负载 90 cycle / 560 交换全绿。
- 计数器：`request_timeouts` 3（正好三次诱导），`fault_locked` false。

**未覆盖：** phase 2（会话已建立后对端不再应答）会锁存 `fault`，这条路径没有被驱动，因此“锁存后靠新 `DAP_Connect` 或本地 `reconnect` 解除”仍只有原生测试证据。

## 未验证与待办

1. **射频看护与锁存后的恢复路径未被验证。** 除 §H 的 phase 1 超时外，本轮读取的本地统计为 `fault_locked=false`、`tx_watchdogs=0`、`recoveries=0`。因此“发送看护 → 隔离 → 排空旧事件 → 重挂 RX”以及“`DAP_Connect` 解除 fault / 本地 `reconnect`”仍缺上板证据。应通过受控漏完成事件等方法验证；目标 halt 本身不会可靠地阻止 RF 发送完成，不能用它代替射频故障注入。
2. **phase 2 超时与 `fault` 锁存后的重连流程未被验证。** `induce_timeout.py` 只覆盖 phase 1（HELLO 超时，不锁 `fault`）；要驱动 phase 2 必须让对端在会话建立后停应答（从机复位、从机 APPLY 或外来 HELLO），本轮没有做。
3. 从机 UART 无法配置 921600（2 % 分频门限）；如果目标固件用高波特率打日志，CDC 侧只会得到乱码。这与 1.3.3 无关但会误判为回归，使用时先核对 `uart-stats` 的 `actual_baud` 与 `framing_errors`。
4. 本轮没有做 1.3.2 ↔ 1.3.3 的**同一目标镜像** A/B（目标中途被刷成 116180 B 的电机镜像，再被 GDB 脚本刷成 ks2-oled），所以 CDC 字节量、烧录耗时都不构成跨版本对照。
5. 工具侧：`swd_workload.py` 的 DTR 握手改动需要与 1.3.2 的记录分开看；它只影响 CDC 组的字节数，不影响延迟统计。
