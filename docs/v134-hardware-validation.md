# 1.3.4 上板验收：EP0 独立 USB 会话恢复（2026-10-25）

本页按 [1.3.4 独立 EP0 USB 会话恢复](usb-recovery.md)的上板验收步骤逐条执行，记录实测结果。1.3.3 的长期读与烧录结果保留为历史基线，见 [1.3.3 上板验收](v133-hardware-validation.md)。

## 结论

**EP0 恢复在实板上成立：被强杀的客户端留下的阻塞槽位可以在不拔插的情况下清掉，计数与代次都对得上。**

| 项目 | 结果 |
| --- | --- |
| 强杀客户端后的可恢复性 | **真实故障复现并按预期恢复**：`dap_request_state=5`、普通 HID 写入失败时，EP0 `usb-status` 仍可读，`usb-recover` 一次即恢复 |
| 计数与代次 | 每次 RESET 专用计数**恰好 +1**，`cleanup_generation == generation`（前台确认），`generation` +1 |
| 恢复耗时 | 自 RESET 起 0.6–52 ms（两个台阶：约 1 ms 与约 52 ms），远低于 2 s 预算 |
| 规模 | **Bulk 20/20、HID 20/20** 轮“强杀→重置→HID Info 校验”全过，无需拔插；目标 RAM 写读另有独立测试 |
| 旧响应归属 | 3 次 STATUS 查询不消费已发布响应；恢复后首次请求拿到自己的应答，没有把旧响应当新结果 |
| 旧写重放 | 未重放：被杀前已执行的写保持生效，恢复后写入的新值不会被旧请求覆盖 |
| 回归 | GDB 8×1 KiB 1 MHz **20/20**；30 min 连续 fb **1800.84 s / 2204 cycle / 42426 交换**，零超时（首次尝试在 19.4 min 出现一次主机侧 `EPIPE`，见 §G） |

**三条边界必须一起看：**

1. **本机 Windows HID 驱动与本轮退出方式下，没有形成未读响应阻塞。** HID 20/20 轮杀后均为 idle；说明这些轮次的重置与后续通信正常，不等于 20 次从阻塞恢复。不能据此断言所有主机环境下 HID 都不会阻塞，也不能将 1.3.3 所有失联唯一归为 Bulk。
2. **`usb-recover` 只清主机侧 USB/DAP 会话，不动目标。** 本轮观测到一次杀进程后目标侧日志中断，恢复目标运行后 CDC 才继续；调试软件必须重新完成 DAP Connect 与目标初始化，这与文档声明一致。
3. **`wIndex` 扫描不能据此判定固件漏检。** WinUSB 对 interface recipient 控制请求会将 `wIndex` 低字节设为目标接口号，因此应用传入 0x00–0xFF 时，设备可能全部收到 1。需查看设备 SETUP 日志或总线抓包，见 §H。
另外记录一次未复现的 USB 事件：首次 30 min 浸泡在 19.4 min 出现 `USBError: [Errno 32] Pipe error`，客户端退出后设备仍可继续通信（无需恢复/拔插，计数未动）。该轮测试失败，错误来源未定位，见 §G。

## 环境

| 项目 | 值 |
| --- | --- |
| 主机 | 1.3.4（EP0 `WUSB` v1 应答，`usb-status` 可用） |
| 从机 | `DAP_Info(4)` = `1.3.4` |
| 构建 | `build/ch570q_host_full.hex` 等，2026-10-25 04:43 |
| 目标 | STM32F103C8T6，ks2-oled 镜像（`clock=64000000`，UART 115200） |
| 工具 | `build/host-venv`（hidapi 0.15 / pyusb 1.3.1 / pyserial 3.5） |

## A. 正常路径不产生多余 RESET

| 步骤 | 观测 |
| --- | --- |
| 初始 | `usb_reset_counter=0`、`dap_request_state=0` |
| 8 s Bulk 混合负载 + 一次 HID Info 全量读取 | `usb_reset_counter` 仍为 **0**；`generation` 5→7（传输切换），`cleanup_generation == generation` |
| 全程 | `halt_in=0`、`halt_out=0`、`suspended=false` |

正常结束（工具主动 `DAP_Disconnect` 并释放接口）不会触发 RESET，计数保持 0。

## B. 复现 1.3.3 的失联形状

用 `build/v134/wedge.py bulk-response` 确定性复现：发 `DAP_Info(4)` 后**不读** IN 就退出（与客户端被杀等价）。

| 观测 | 值 |
| --- | --- |
| 设备状态 | `dap_request_state=5`（response）、`transport=2`（Bulk） |
| 普通 HID 管理 | 失败：`short write -1/65`（与 [1.3.3 §F](v133-hardware-validation.md#f-强杀客户端仍然打死主机) 现场一致） |
| EP0 `usb-status` | **正常返回** 24 B 状态 |
| EP0 是否消费响应 | 连续 3 次 STATUS 后，客户端仍能读到完整应答 `1.3.4` |

在一次真实请求期间用同进程轮询 EP0，状态机按文档列出 READY→PROCESSING→RESPONSE：

```text
before: state=0 transport=0 gen=19 counter=4 cleanup=19
  0.00s state=4 transport=2 gen=20      # PROCESSING，EP0 可读
  0.15s state=5 transport=2 gen=20      # RESPONSE，EP0 可读
```

（READY=3、PROCESSING=4、RESPONSE=5 都在实测中出现并被 EP0 正确报告；两个窗口在 100 kHz SWD 下都短于 150 ms，所以 RESET 是在 RESPONSE 与 idle 两种状态下执行的。）

## C. `usb-recover` 的恢复效果

从 `state=5` 执行一次：

| 观测 | 前 | 后 |
| --- | --- | --- |
| `status` | — | `local_session_reset` |
| `dap_request_state` | 5（response） | **0（idle）** |
| `transport` | 2（Bulk） | 0（none） |
| `generation` | 8 | **9** |
| `usb_reset_counter` | 0 | **1**（恰好 +1） |
| `cleanup_generation` | 8 | **9**（等于 generation） |
| `reset_transfer_timed_out` | — | false |

恢复后立即：

- `hid_smoke` 全量 Info 通过（vendor `WCH`、product、serial、firmware `1.3.4`）——**旧响应没有被当成新结果**（被杀的请求是 `DAP_Info(4)`，若误投递会在 vendor 查询里表现为字符串错位）。
- `swd_identify.py`：DP IDCODE `0x1BA01477`，SWD 原始序列正常。
- 10 s Bulk 混合负载：96 cycle、块读逐次一致、RAM 写入-回读-恢复、暂停/恢复全部通过。
- `recovery-stats`：`reconnects=0`、`response_expirations=0`、`fault_locked=false`——EP0 重置确实只动本地 USB 会话，没有被记成 RF 重连。

## D. 20 轮强杀 → 恢复 → 校验

驱动器 `build/v134/recover_loop.py`：起一个真实客户端 → 硬杀（`Process.kill()`，无清理）→ 读 EP0 状态 → RESET → 校验计数/代次/状态 → 用 HID 读 `DAP_Info(1)` 必须等于 `WCH`。每轮未做目标 RAM 写读，目标正确性由 §C、§E 及后续负载另行验证。

| 传输 | 轮数 | 通过 | 杀前状态分布 | 计数 +1 | 恢复耗时 | 每轮 DAP 校验 |
| --- | ---: | ---: | --- | ---: | --- | ---: |
| Bulk（`swd_workload.py`） | 20 | **20** | response 17、idle 3 | 20/20 | 0.9–52.4 ms | 20/20 |
| HID（`wedge.py hid-loop`） | 20 | **20** | **idle 20** | 20/20 | 0.6–52.0 ms | 20/20 |

- Bulk 20 轮里 17 轮留下了 RESPONSE 阻塞，正是 1.3.3 需要拔插的那种状态；1.3.4 全部就地恢复。
- HID 20 轮全部停在 idle；本轮 Windows HID 驱动持续提交 IN，未读报告在设备端已被取走。另用 `wedge.py hid-response`（写报告后不读、退出）做了 5 轮，设备同样为 idle。该结论限定在本机驱动、队列和退出窗口。
- 两个恢复耗时台阶（≈1 ms 与 ≈52 ms）对应“RESET 事务结束后第一次状态轮询即看到 idle”与“需要多等一个 50 ms 轮询周期”，不是两种失败模式。

## E. 旧写是否被重放

`build/v134/write_replay.py`：在目标 RAM `0x20003000` 上先确认正常写读，再写出标记 `0x22222222` 且**不读应答**退出（被杀形状），执行 `usb-recover`，然后写入 `0x33333333` 并在 2 s 后复查。

```text
after reset: 0x22222222                    # 被杀前已经执行，无法撤回
wrote 0x33333333, read back 0x33333333
2 s later: 0x33333333 (marker replay would read 0x22222222)
uncertain write executed before reset: True
uncertain write replayed after reset:  False
original restored: True
```

即：本次未观察到旧写在恢复后覆盖新值；被杀的写已经执行且不会被恢复操作撤销。原始值 `0x00000000` 已还原。相同 RAM 值写入是幂等操作，不能检测所有重复执行，尤其不能排除新值写入前发生的重复写；本测试不构成跨恢复 exactly-once 证明。

## F. CDC 与目标侧

- 会话重置后 CDC 重新打开即可拿到目标日志（`[oled] ms=... frames=... ready=1 i2c_errors=0 fault=0`，100 % 可打印）。
- 本轮同一 reset 之后，“DTR 直接拉高打开”和“先 DTR 低再拉高”均能收到数据。这不表示固件放宽了条件：关闭串口期间设备可能已经观察到 DTR 低并解除隔离。源码仍要求隔离时先观察到低电平，操作上继续保留 DTR 低→高。
- 有两次 `usb-recover` 之后立即打开 CDC 收到 0 字节，随后（经过一次新的 DAP 会话）恢复正常，之后未再复现。保留为未定位现象；后续记录 `cdc_quarantined`、DTR、UART 启用状态和目标运行状态，不能以未复现为由排除缺陷。
- 杀进程后目标可能停在 halted：一次杀进程后 CDC 无日志，恢复目标运行后即恢复；`usb-recover` 本身不动目标（文档已声明）。DHCSR 在另一次杀进程+恢复后读得 `0x01010001`（S_HALT=0，目标在跑），说明不是每次都会留下 halted。
- **suspend/HALT 未验证**：全程 `suspended=false`、`halt_in=0`、`halt_out=0`；没有制造真实 USB suspend 或端点 HALT。这部分仍只有原生测试覆盖。

## G. 连续 fb 浸泡

`--mode fb --seconds 1800 --stop-on-error`，100 kHz。

| 尝试 | 结果 | 时长 | cycle | 交换 | 延迟 p50 / p99 / p999 / max (ms) | 超时/故障 |
| --- | --- | ---: | ---: | ---: | --- | --- |
| 1（后台独立进程） | **失败**，`USBError: [Errno 32] Pipe error` @ `fb_read` | 1162.03 s | 1418 | 27302 | 41.47 / 64.22 / 223.75 / 434.45 | 0 / 未锁存 |
| 2（后台独立进程） | **通过** | **1800.84 s** | **2204** | **42426** | 41.43 / 64.24 / 223.84 / 496.92 | **0 / 未锁存** |
| 附加（前台 570 s） | 通过 | 570.67 s | 717 | 13798 | 41.33 / 63.93 / 144.19 / 298.73 | 0 / 未锁存 |

- 通过的那一轮：`usb_reset_counter` 全程未动（62），`fault_locked=false`，`reconnects=0`；块读与 RAM 写入-回读-恢复 70/70、110/110 全部一致。
- **第一次尝试的 `EPIPE` 值得记录**：故障点是 Bulk OUT 已成功（0.3 ms）后的 IN 读，主机侧报 pipe error，客户端进程退出；**设备本身没有受影响**——无需 `usb-recover`、无需拔插，紧接着的 `usb-status`、HID Info 与 Bulk 负载全部正常，专用计数仍为 62。这与 1.3.3 的“卡死后必须拔插”不同，但不能归因为 1.3.4 的修复（当时调用方已死，EP0 并未参与）。已跑的三段连续 fb 合计约 59 min，只有这一次事件，且未复现。

## H. EP0 请求校验与 WinUSB 接口号映射

按文档“其他方向、长度或 magic 返回 STALL”逐项验证：

| 请求 | 实测 |
| --- | --- |
| 错误 `bRequest`（0x53） | STALL ✓ |
| `STATUS` 的 `wValue≠0` | STALL ✓ |
| `STATUS` 长度 23（非 24） | STALL ✓ |
| `RESET` 的 `wValue≠0x5253` | STALL ✓ |
| `RESET` 的方向错（0xC1） | STALL ✓ |
| **`wIndex` 取 0/2/5（甚至 0xFF）** | **不 STALL，照常返回 24 B STATUS；`wIndex=5` 的 RESET 也被接受并执行（计数 62→63）** |
| `wIndex ≥ 0x0100` | STALL ✓ |

上述是应用传入参数与 API 返回结果，不能证明总线上的 `wIndex` 原样到达设备。源码对 STATUS/RESET 均检查完整的 `index == 1`。Microsoft [WinUsb_ControlTransfer 文档](https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_controltransfer)说明：interface recipient 的标准、类和 vendor 请求，Winusb.sys 将 Index 低字节设置为目标接口号。本轮已打开 interface 1，低字节扫描成功、高字节非零失败与此机制吻合。因此撤回“固件未限定 wIndex”的判断；应先用 `UART_LOG_USB_SETUP` 或总线抓包确认实际收到的值，再用可原样发送接口号的后端验证非法值，无需据此放宽协议。

## 未验证与待办

1. **suspend / 端点 HALT**：需要真实挂起总线或让客户端把端点 halt，本轮未做。RESET 保留 HALT 与 toggle 的说法仍只有原生测试证据。
2. **PROCESSING / READY 状态下执行 RESET**：两个窗口在实测里都短于 150 ms（把 SWD 时钟降到 10 Hz 也一样），只能用同进程轮询观察到状态，没能让 RESET 恰好落在其中。原生测试覆盖，实板未覆盖。
3. **30 min 浸泡中的那一次 `EPIPE`**：未复现，也没能定位到主机控制器、驱动还是设备侧；建议后续用保留 URB 日志的抓包器重试。
4. **`wIndex` 线上值验证**：WinUSB 会重写低字节，当前扫描不构成固件漏检证据；用设备 SETUP 日志或总线抓包验证，再决定是否存在代码问题。
5. **`cleanup_generation` 与 CDC 取消完成的时序**：文档已说明前者不代表后者；本轮两次 0 字节 CDC 未复现，需要更多样本。
6. **被杀时目标处于 halt 的恢复流程**：`usb-recover` 不恢复目标运行，调试软件必须重新初始化；本轮只观察到一次，未做系统性统计。
7. 本轮未复测烧录路径（1.3.3 已通过 `program+verify` 10/10、GDB `load` 5/5）；1.3.4 的改动只涉及 USB 会话层，但没有在 1.3.4 上重跑。
