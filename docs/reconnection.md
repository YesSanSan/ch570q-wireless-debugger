# 1.3.3 重连与射频恢复实现

**后续 1.3.4：**已实现独立 EP0 USB 状态与恢复入口，处理 DAP 请求槽阻塞时普通管理命令也送不进去的问题。软件回归及构建通过，[1.3.4 实板复测](v134-hardware-validation.md)已验证 Bulk 未读响应阻塞后的恢复；处理中 RESET、HALT/suspend 等仍待验证。详见 [USB 会话恢复](usb-recovery.md)；本页保留 1.3.3 的实现与实板基线。

本页描述 2026-10-24 开始实现的 1.3.3。原生回归和完整固件构建通过；**2026-10-25 已完成上板复测**，结果见[1.3.3 上板验收](v133-hardware-validation.md)。简短结论：连续 fb 块读（134 cycle / 30 min 2214 cycle）、20+20 轮 GDB 8×1 KiB、10/10 `program+verify`、5/5 GDB `load` 全部通过，1.3.2 的连续块读静默与 GDB `load` 失败没有复现；但**真实故障后的恢复路径（`tx_watchdogs`/`recoveries`/`reconnect`）仍未在硬件上触发过**，且强杀主机客户端后主机仍会卡死、只能拔插。

## 使用行为

一次 REQUEST 超过 1500 ms 仍没有匹配响应时，主机返回明确的命令失败，保留 `fault` 门控。目标可能已经执行过该请求；本实现不会把旧 payload 自动放进新会话。

恢复分为两层：

1. 射频状态看护由每轮主循环独立调用，不依赖新请求。发送超过 100 ms 未收到终结事件时进入恢复；失败时每 10 ms 再尝试。Stop 尚未生效或恢复未成功时，继续隔离，不覆盖发送 DMA、不报告 idle。
2. 软件会话需要显式重新连接。停止当前 OpenOCD/GDB，重新启动调试连接时，合法的 `DAP_Connect`（端口 0/1）允许在 fault 下发起新 HELLO。只有新会话的 Connect 成功才解除该路径的 fault；HELLO 超时、Connect 超时或返回失败仍保持门控。旧命令的失败已经上报，重连不代表旧命令成功。

也可以关闭 CDC 串口、停止 OpenOCD 后，使用本地管理命令：

```powershell
python tools/debugger_ctl.py recovery-stats
python tools/debugger_ctl.py reconnect
```

`reconnect` 只清理本地主机会话并允许后续新连接，返回成功并不表示从机已连通；下一次调试请求才实际 HELLO。它只允许在固定/实际 HOST 角色本地执行，远端调用拒绝，DTR 仍为高时返回 BUSY。若物理 RF 恢复失败，后续连接仍可能超时。

CDC 在故障/会话换代后进入隔离：重建/换代时清掉应用层旧后缀，取消设备端 EP3 未交付收发数据，等待观察到 DTR 为低，之后才接受重新打开的流量。正常 DTR 关闭仍保留原先的有序排空语义。已经被 PC ACK 或保存在操作系统/串口应用中的旧数据无法由设备撤回；重连前应关闭旧串口会话，不将旧发送缓冲重新提交。

取消 EP3 数据不会只依靠 CPU 中断锁：先将端点 NAK，保留此前允许完成的 IN/OUT 事务状态，使在途完成仍正确翻转 toggle；观察公开 `RB_UMS_SIE_FREE` 且处理完完成事件后才释放队列。接口返回 BUSY 时保持端点隔离，由后续轮询重试，无忙等。USB IRQ/polling 模型均注入 NAK 之后旧 IN/OUT 才完成的交错。

两块 CH570Q 都需要更新。`DAP_Info(4)` 的版本来自从机，单刷主机无法证明从机已更新；旧从机不支持下述 HELLO 已使用标记，不具备本轮完整恢复保证。

## 射频停止与事件隔离

实现位于 [radio.c](../src/radio.c)，接口位于 [radio.h](../include/radio.h)。100 ms 是当前初始 TX 看护阈值，不是 SWD 请求的总超时，也不是已测量的最佳值。

对固定 SDK 的反汇编确认：

- `RFRole_Stop` 调用 `RFIP_SetPHYIdle` 后返回 0。`SetPHYIdle` 对活动硬件置 abort 位，没有等待硬件停止。
- `RFIP_StartTx` / `RFIP_SetRx` 分别写 LLE 模式 2/1；低两位为零才是当前实现检查的 idle。
- `LLE_LibIRQHandler` 读取事件和 mask，清事件后同步调用处理函数；`rfBasicProcess` 同步调用应用回调，没有另一个延后回调队列。回调 `id` 不能作为事务代次。
- BB bit6 的 IRQ 处理会推动 LLE timeout 路径，因此需先处理 BB，再处理 LLE。

恢复在全局 IRQ 屏蔽的有界临界区中执行：进入隔离、请求停止、确认硬件 idle，排空旧 BB/LLE 事件及 PFIC pending，复核源状态和硬件 idle，之后才允许 RX 重挂或新 TX。基础 RF 旧事件即使原 mask 未启用也要处理；采用固定轮数处理而非循环忙等。失败时保持隔离交给后续 poll 重试。

隔离回调不重挂 RX、不将旧终结算到新发送，但仍保存可用 RX mailbox；已有未读邮箱不会被清空。正常回调也只设置前台重挂标志，避免 RF SDK 同步回调过程中递归 SetRx。固件主循环（包括历史最小诊断入口）必须及时调用 `radio_poll(platform_millis())`。

这依赖 SDK 导出的 `gptrLLEReg` / `gptrBBReg` 内部寄存器 ABI。构建固定 RF 库 SHA256：

```text
a20477a01e6349d03b08efe75e357ce89ca385d5bad292872695f73797d381b8
```

[CMakeLists.txt](../CMakeLists.txt) 在库变化时拒绝继续构建；更换 SDK 时必须重新审查取消、事件、mask 与回调语义。原生模型不证明硅片实际 abort 时序、空口恢复效果或临界区墙钟时长。

## 从机与握手

[app.c](../src/app.c) 的待发送响应门控设为 100 ms；期限从生成响应之后开始。发送持续无法启动时，仅释放待发门控以便继续取邮箱和处理 HELLO，保留 [link.c](../src/link.c) 中最近请求、响应和 next_sequence。相同请求仍重发缓存，SWD/UART 副作用不会因该门控退出而再次执行。

HELLO nonce 在主机一次启动内单调推进。为处理主机重启但从机仍保留旧会话、nonce 恰好重复的情况，从机对已经执行过数据的同 nonce HELLO 返回 `HELLO_ACK length=1, data=01`，同时保留旧缓存。新主机收到标记后换 nonce，仍受原 HELLO 的 1500 ms 预算约束；取得新空会话前不发新 payload。普通新 HELLO ACK 仍为 length=0。

跨复位不会透明续传写操作，不承诺跨断电 exactly-once。重连之后仍须由调试软件完成目标初始化。

## 管理接口

旧 `WD_CTL_STATS` 的 12 字段保持不变。新增：

| 子命令 | 范围 | 返回 |
| --- | --- | --- |
| 13 `WD_CTL_RECONNECT` | 仅 HOST 本地 | `cmd sub status`，清本地会话，旧请求不重放 |
| 14 `WD_CTL_RECOVERY_STATS` | 本地或已连接远端 | 有效长度 38 B，HID 补齐 64 B |

统计布局：`cmd sub status version fault cdc_quarantined`，version=1；偏移 6 起依次为 8 个 LE32：`rx_timeouts`、`tx_watchdogs`、`recovery_attempts`、`recovery_failures`、`recoveries`、`stale_events`、`reconnects`、`response_expirations`。远端的 host 专用字段可能为零；远端失联时应先读取本地统计。恢复计数是本机 RF 恢复事件，并不表示目标操作执行成功。

## 超时响应格式勘误

1.3.2 的调查文档和测试把两个正确格式误判成错误，导致该版反而生成了不符合命令布局的响应。1.3.3 修正如下：

| 命令 | 正确失败响应 | 解释 |
| --- | --- | --- |
| `DAP_Transfer 05` | `05 00 08` | 8 位完成数=0，协议错误 |
| `DAP_TransferBlock 06` | `06 00 00 08` | 16 位完成数=0，协议错误 |
| `DAP_SWD_Sequence 1d` | `1d ff` | 单个 DAP_ERROR 状态字节 |

历史“06 00 00 08 是假成功”“1d ff 长度不够”的说法错误；历史上板记录保留作为当时的观测，但其协议解释由本节替代。对应 [超时注入工具](../tools/induce_timeout.py) 和原生断言同步修正。

## 已验证与待验证

软件验证：

- `pwsh -NoProfile -File tests/run.ps1`：14 个原生执行套件全部通过，使用 `-Wall -Wextra -Werror`；原有 59 个 SWD GPIO 用例仍通过。
- 新增真实 `radio.c` 寄存器/SDK mock 的 8 组故障注入：漏/迟到/同步/合并终结事件，Stop/StartTx/SetRx 失败，硬件停止未完成，disabled 旧事件，邮箱保留、回绕与恢复节流。
- 应用层覆盖重连成功及 HELLO/Connect 失败、旧响应拒绝、主机 nonce 重启碰撞、重复 UART 写仅执行一次、响应门控超时、64 B USB 输入中的未读后缀、应用状态为空但端点有旧数据。
- USB IRQ 与 polling 两种模型覆盖清理待收/待发/ZLP、suspend/halt 下清理、保持端点 toggle、线路设置和 DAP 请求归属。
- `cmake --build build`：HOST_FULL、SLAVE_FULL、AUTO 三个完整固件构建通过，版本 1.3.3；另启用 `WD_BUILD_DIAGNOSTICS=ON` 构建全部历史最小/USB 诊断镜像通过。
- Python 语法与管理 CLI 假设备包解码通过，包含 `reconnect`、版本化 `recovery-stats`；未打开真实 HID。

上板进展（2026-10-25，详见[1.3.3 上板验收](v133-hardware-validation.md)）：已完成连续 fb 30 min、20 轮 GDB 8×1 KiB（100 kHz 与 1 MHz 各 20 轮）、`program+verify` 10/10、独立 GDB `load` 5/5、真实 1500 ms 超时响应形状与链路恢复、CDC 隔离与 DTR 重开。

仍需上板：**主从分别漏完成事件 / phase 2 超时后的 `fault` 锁存与显式重连**（本轮 `tx_watchdogs`、`recoveries`、`reconnects` 全为 0，恢复路径没有被真实故障驱动过）；**强杀客户端后的恢复**（本轮实测两次都卡死，需拔插）；USB HALT/suspend 与 CDC 隔离在真实客户端下的表现。记录要求不变：首次错误、恢复时间、是否需断电、写后数据校验，不能仅靠自动重试后的最终成功验收。
